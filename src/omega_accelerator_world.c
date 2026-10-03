#include "omega_accelerator_world.h"
#include "omega_blackwell_qmd.h"
#include "omega_blackwell_encoder.h"
#include "omega_blackwell_codegen.h"
#include "omega_blackwell_submit.h"
#include "omega_gpu_wait.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdatomic.h>

/* Capability epochs are process unique; the local epoch remains available for
 * deterministic rolling provenance across independent world instances. */
static _Atomic uint32_t next_handle_epoch = 1;

static uint32_t allocate_handle_epoch(void) {
    uint32_t old = atomic_load_explicit(&next_handle_epoch, memory_order_relaxed);
    for (;;) {
        if (old == UINT32_MAX) return 0;
        if (atomic_compare_exchange_weak_explicit(&next_handle_epoch, &old, old + 1,
                                                  memory_order_relaxed, memory_order_relaxed))
            return old;
    }
}


static const uint32_t WORLD_SETUP_WORDS[18] = {
    0x20012061, 0x0000cec0, 0x20012092, 0x00000001, 0x200120a8, 0x0000000f, 0x2001255d, 0x00000003,
    0x2001255e, 0x20000000, 0x2001255f, 0x000fffff, 0x20012557, 0x00000003, 0x20012558, 0x22000000,
    0x20012559, 0x00000000,
};

/* A submission counts as complete once the hardware marker has reached or
 * passed its payload (serial-number arithmetic, correct across 2^32 wrap). */
static bool payload_reached(uint32_t payload, uint32_t upto) {
    return (int32_t)(payload - upto) <= 0;
}

/* CHIPWAIT-2: completion memory and waits for the world's completion page.
 * The page is GPU-uncached. Each synchronous dispatch (execute and the matmul
 * variant) releases its payload at +0 and, after an L2_FLUSH_DIRTY mem-op
 * (NVC96F_MEM_OP_A..D = 0x28..0x34, D bits 31:27 OPERATION = 0x10, as in
 * omega_numeric_gb10.c), the same payload at +0x10; the host waits for both with
 * the wrap-safe sequence wait. The same payload sequence is used so the check is
 * "reached or passed", never exact equality. */
#define WORLD_MARKER2_OFFSET 0x10

static volatile uint32_t *world_marker2(const OmegaAcceleratorWorld *world) {
    return (volatile uint32_t *)((uint8_t *)world->completion.cpu_marker + WORLD_MARKER2_OFFSET);
}


/* Wait until word has reached `payload` (sequence wait). Returns 0 on PASS;
 * otherwise prints one tagged line with the report and returns -1. The caller
 * keeps its own fault handling. */
static int world_wait_seq(const char *site, const char *what, volatile uint32_t *word,
                          uint32_t payload, uint32_t progress_ms, uint32_t total_ms) {
    omega_gpu_wait_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.progress_timeout_ms = progress_ms;
    cfg.total_timeout_ms = total_ms;
    omega_gpu_wait_report_t r;
    if (omega_gpu_wait_sequence(word, payload, &r, &cfg)) return 0;
    fprintf(stderr,
            "WORLD_WAIT_FAIL site=%s wait=%s kind=%s result=%s expected=%u observed=%u "
            "elapsed_ns=%llu progress_count=%llu polls=%llu\n",
            site, what, omega_gpu_wait_kind_name(r.wait_kind), omega_gpu_wait_result_name(r.result),
            r.expected, r.last_observed, (unsigned long long)r.elapsed_ns,
            (unsigned long long)r.progress_count, (unsigned long long)r.polls);
    return -1;
}

/* Synchronous dispatch: marker, then marker2 (progress stall 5000 ms, hard total 600000 ms). */
static int world_wait_dispatch(const OmegaAcceleratorWorld *world, const char *site, uint32_t payload) {
    if (world_wait_seq(site, "marker", world->completion.cpu_marker, payload, 5000, 600000) != 0) return -1;
    return world_wait_seq(site, "marker2", world_marker2(world), payload, 5000, 600000);
}

static bool buffer_referenced_in_flight(const OmegaAcceleratorWorld *world, uint32_t id) {
    for (uint32_t i = 0; i < world->in_flight_count; i++) {
        const OmegaWorldSubmission *s = &world->in_flight[i];
        if (s->a_object_id == id || s->b_object_id == id || s->c_object_id == id) return true;
    }
    return false;
}

static bool code_referenced_in_flight(const OmegaAcceleratorWorld *world, uint32_t id) {
    for (uint32_t i = 0; i < world->in_flight_count; i++) {
        if (world->in_flight[i].code_object_id == id) return true;
    }
    return false;
}

/* Return a revoked slot's memory to PHYSICS. The slot becomes reusable only
 * after this succeeds; a PHYSICS refusal is an accounting failure. */
static int release_buffer_memory(OmegaAcceleratorWorld *world, OmegaBufferEntry *entry) {
    if (nvrm_free(&world->m16.rm, &entry->mem) != 0) {
        world->faulted = true;
        return OMEGA_WORLD_ERR_FAULT;
    }
    entry->retiring = false;
    entry->cpu_addr = NULL;
    entry->gpu_va = 0;
    entry->size_bytes = 0;
    return OMEGA_WORLD_OK;
}

static int release_code_memory(OmegaAcceleratorWorld *world, OmegaCodeEntry *entry) {
    if (nvrm_free(&world->m16.rm, &entry->mem) != 0) {
        world->faulted = true;
        return OMEGA_WORLD_ERR_FAULT;
    }
    entry->retiring = false;
    entry->cpu_addr = NULL;
    entry->gpu_va = 0;
    entry->size_bytes = 0;
    return OMEGA_WORLD_OK;
}

/* Release every retiring slot that no remaining in-flight submission binds. */
static void release_unreferenced_retiring(OmegaAcceleratorWorld *world) {
    for (uint32_t i = 0; i < OMEGA_WORLD_MAX_BUFFERS; i++) {
        OmegaBufferEntry *e = &world->buffers[i];
        if (e->retiring && !buffer_referenced_in_flight(world, i)) (void)release_buffer_memory(world, e);
    }
    for (uint32_t i = 0; i < OMEGA_WORLD_MAX_CODE_ENTRIES; i++) {
        OmegaCodeEntry *e = &world->code_entries[i];
        if (e->retiring && !code_referenced_in_flight(world, i)) (void)release_code_memory(world, e);
    }
}

int omega_world_init(OmegaAcceleratorWorld *world) {
    if (!world) return OMEGA_WORLD_ERR_INVALID_ARG;
    memset(world, 0, sizeof(*world));

    world->current_epoch = 1;
    world->handle_epoch = allocate_handle_epoch();
    if (!world->handle_epoch) return OMEGA_WORLD_ERR_FAULT;
    world->channel_generation = 1;

    if (m16_native_open(&world->m16) != 0) {
        return OMEGA_WORLD_ERR_HARDWARE;
    }
    if (m16_native_create_channel(&world->m16) != 0) {
        m16_native_close(&world->m16);
        return OMEGA_WORLD_ERR_HARDWARE;
    }

    world->ring_capacity = world->m16.rm.entries;
    world->channel_active = true;

    /* The CPU rewrites retired command slots. GPU-cached slots can replay
     * earlier commands despite CPU store barriers, so bypass that cache. */
    if (nvrm_alloc_gpu_uncached(&world->m16.rm, 0x10000, &world->pb_mem) != 0) {
        m16_native_close(&world->m16);
        return OMEGA_WORLD_ERR_NO_MEM;
    }
    world->m16.pb_mem = world->pb_mem;

    /* Allocate persistent scratch arena */
    /* GB10: GPU-cacheable system memory is not coherent with CPU mappings (nvos.h:1115-1118, hive-phases I37). */
    if (nvrm_alloc_gpu_uncached(&world->m16.rm, OMEGA_WORLD_SCRATCH_SIZE, &world->scratch.mem) != 0) {
        m16_native_close(&world->m16);
        return OMEGA_WORLD_ERR_NO_MEM;
    }
    world->scratch.capacity_bytes = OMEGA_WORLD_SCRATCH_SIZE;
    world->scratch.allocated_bytes = 0;

    /* Allocate persistent completion marker */
    /* GB10: GPU-cacheable system memory is not coherent with CPU mappings (nvos.h:1115-1118, hive-phases I37). */
    if (nvrm_alloc_gpu_uncached(&world->m16.rm, 0x1000, &world->completion.mem) != 0) {
        m16_native_close(&world->m16);
        return OMEGA_WORLD_ERR_NO_MEM;
    }
    world->completion.cpu_marker = (volatile uint32_t *)world->completion.mem.cpu;
    world->completion.gpu_va = world->completion.mem.va;
    *world->completion.cpu_marker = 0;
    world->completion.cpu_marker2 = (volatile uint32_t *)((uint8_t *)world->completion.mem.cpu + 0x10);
    world->completion.gpu_va2 = world->completion.mem.va + 0x10;
    *world->completion.cpu_marker2 = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    world->completion.last_payload = 0;

    /* Initialize rolling state digest D_0 */
    memset(world->rolling_state_digest, 0, sizeof(world->rolling_state_digest));
    world->sequence_number = 0;

    world->initialized = true;
    return OMEGA_WORLD_OK;
}

int omega_world_destroy(OmegaAcceleratorWorld *world) {
    if (!world || !world->initialized) return OMEGA_WORLD_ERR_INVALID_ARG;

    for (uint32_t i = 0; i < OMEGA_WORLD_MAX_BUFFERS; i++) {
        world->buffers[i].active = false;
    }
    for (uint32_t i = 0; i < OMEGA_WORLD_MAX_CODE_ENTRIES; i++) {
        world->code_entries[i].active = false;
    }

    int close_rc = m16_native_close(&world->m16);
    if (world->m16.rm.faulted) world->faulted = true;
    world->buffer_count = 0;
    world->code_count = 0;
    world->in_flight_count = 0;
    world->scratch.allocated_bytes = 0;
    world->completion.cpu_marker = NULL;
    world->completion.gpu_va = 0;
    world->completion.cpu_marker2 = NULL;
    world->completion.gpu_va2 = 0;
    memset(&world->pb_mem, 0, sizeof world->pb_mem);
    world->channel_active = false;
    world->initialized = false;
    return close_rc == 0 ? OMEGA_WORLD_OK : OMEGA_WORLD_ERR_HARDWARE;
}

int omega_world_rebuild(OmegaAcceleratorWorld *world) {
    if (!world || !world->initialized) return OMEGA_WORLD_ERR_INVALID_ARG;
    if (world->current_epoch == UINT32_MAX) return OMEGA_WORLD_ERR_FAULT;
    uint32_t next_epoch = world->current_epoch + 1;
    if (world->initialized && omega_world_destroy(world) != OMEGA_WORLD_OK)
        return OMEGA_WORLD_ERR_HARDWARE;

    int rc = omega_world_init(world);
    if (rc == OMEGA_WORLD_OK) {
        world->current_epoch = next_epoch;
    }
    return rc;
}

int omega_world_register_buffer(OmegaAcceleratorWorld *world,
                                 size_t size_bytes,
                                 uint32_t permissions,
                                 OmegaHandle *out_handle) {
    if (!world || !world->initialized || size_bytes == 0 || !out_handle) {
        return OMEGA_WORLD_ERR_INVALID_ARG;
    }
    if (world->faulted) return OMEGA_WORLD_ERR_FAULTED;

    int free_slot = -1;
    for (uint32_t i = 0; i < OMEGA_WORLD_MAX_BUFFERS; i++) {
        if (!world->buffers[i].active && !world->buffers[i].retiring &&
            world->buffers[i].generation != UINT32_MAX) {
            free_slot = (int)i;
            break;
        }
    }
    if (free_slot < 0) return OMEGA_WORLD_ERR_NO_MEM;

    OmegaBufferEntry *entry = &world->buffers[free_slot];
    if (size_bytes > SIZE_MAX - 0xfffULL) return OMEGA_WORLD_ERR_BOUNDS;
    size_t alloc_bytes = (size_bytes + 0xfffULL) & ~0xfffULL;
    if (alloc_bytes < 0x1000) alloc_bytes = 0x1000;

    /* GB10: GPU-cacheable system memory is not coherent with CPU mappings (nvos.h:1115-1118, hive-phases I37). */
    if (nvrm_alloc_gpu_uncached(&world->m16.rm, alloc_bytes, &entry->mem) != 0) {
        return OMEGA_WORLD_ERR_NO_MEM;
    }

    entry->object_id = (uint32_t)free_slot;
    if (entry->generation == 0) {
        entry->generation = 1;
    }
    entry->permissions = permissions;
    entry->size_bytes = size_bytes;
    entry->cpu_addr = entry->mem.cpu;
    entry->gpu_va = entry->mem.va;
    entry->active = true;

    out_handle->world_epoch = world->handle_epoch;
    out_handle->object_id = (uint32_t)free_slot;
    out_handle->object_generation = entry->generation;
    out_handle->object_type = OMEGA_OBJ_BUFFER;
    out_handle->permissions = permissions;

    world->buffer_count++;
    return OMEGA_WORLD_OK;
}

int omega_world_revoke_buffer(OmegaAcceleratorWorld *world,
                               const OmegaHandle *handle) {
    if (!world || !world->initialized || !handle) return OMEGA_WORLD_ERR_INVALID_ARG;
    if (handle->world_epoch != world->handle_epoch) return OMEGA_WORLD_ERR_STALE_EPOCH;
    if (handle->object_id >= OMEGA_WORLD_MAX_BUFFERS) return OMEGA_WORLD_ERR_INVALID_ARG;

    OmegaBufferEntry *entry = &world->buffers[handle->object_id];
    if (!entry->active) return OMEGA_WORLD_ERR_NOT_FOUND;
    if (handle->object_generation != entry->generation) return OMEGA_WORLD_ERR_STALE_GEN;
    if (handle->object_type != OMEGA_OBJ_BUFFER) return OMEGA_WORLD_ERR_INVALID_ARG;

    /* The handle is refused from this instant; the memory is returned now,
     * or after drain if an in-flight submission still binds it. */
    entry->active = false;
    if (entry->generation != UINT32_MAX) entry->generation++;
    if (world->buffer_count > 0) world->buffer_count--;
    if (buffer_referenced_in_flight(world, handle->object_id)) {
        entry->retiring = true;
        return OMEGA_WORLD_OK;
    }
    return release_buffer_memory(world, entry);
}

int omega_world_resolve_buffer(OmegaAcceleratorWorld *world,
                                const OmegaHandle *handle,
                                uint32_t required_perms,
                                size_t req_offset,
                                size_t req_size,
                                void **out_cpu,
                                uint64_t *out_gpu_va) {
    if (!world || !world->initialized || !handle) return OMEGA_WORLD_ERR_INVALID_ARG;
    if (handle->world_epoch != world->handle_epoch) return OMEGA_WORLD_ERR_STALE_EPOCH;
    if (handle->object_id >= OMEGA_WORLD_MAX_BUFFERS) return OMEGA_WORLD_ERR_INVALID_ARG;

    OmegaBufferEntry *entry = &world->buffers[handle->object_id];
    if (!entry->active) return OMEGA_WORLD_ERR_NOT_FOUND;
    if (handle->object_generation != entry->generation) return OMEGA_WORLD_ERR_STALE_GEN;
    if (handle->object_type != OMEGA_OBJ_BUFFER) return OMEGA_WORLD_ERR_INVALID_ARG;

    if ((entry->permissions & required_perms) != required_perms) return OMEGA_WORLD_ERR_PERM_DENIED;
    if ((handle->permissions & required_perms) != required_perms) return OMEGA_WORLD_ERR_PERM_DENIED;
    if ((required_perms & OMEGA_PERM_WRITE) &&
        buffer_referenced_in_flight(world, handle->object_id))
        return OMEGA_WORLD_ERR_FAULT;

    if (req_offset > entry->size_bytes || req_size > entry->size_bytes - req_offset ||
        entry->gpu_va > UINT64_MAX - req_offset) return OMEGA_WORLD_ERR_BOUNDS;

    if (out_cpu) {
        *out_cpu = (uint8_t *)entry->cpu_addr + req_offset;
    }
    if (out_gpu_va) {
        *out_gpu_va = entry->gpu_va + req_offset;
    }
    return OMEGA_WORLD_OK;
}

int omega_world_register_code(OmegaAcceleratorWorld *world,
                               const void *code_bytes,
                               size_t code_size,
                               const uint8_t realization_id[32],
                               OmegaHandle *out_handle) {
    if (!world || !world->initialized || !code_bytes || code_size == 0 || !out_handle) {
        return OMEGA_WORLD_ERR_INVALID_ARG;
    }
    if (world->faulted) return OMEGA_WORLD_ERR_FAULTED;

    int free_slot = -1;
    for (uint32_t i = 0; i < OMEGA_WORLD_MAX_CODE_ENTRIES; i++) {
        if (!world->code_entries[i].active && !world->code_entries[i].retiring &&
            world->code_entries[i].generation != UINT32_MAX) {
            free_slot = (int)i;
            break;
        }
    }
    if (free_slot < 0) return OMEGA_WORLD_ERR_NO_MEM;

    OmegaCodeEntry *entry = &world->code_entries[free_slot];
    if (code_size > SIZE_MAX - 0xfffULL) return OMEGA_WORLD_ERR_BOUNDS;
    size_t alloc_bytes = (code_size + 0xfffULL) & ~0xfffULL;
    if (alloc_bytes < 0x1000) alloc_bytes = 0x1000;

    /* GB10: GPU-cacheable system memory is not coherent with CPU mappings (nvos.h:1115-1118, hive-phases I37). */
    if (nvrm_alloc_gpu_uncached(&world->m16.rm, alloc_bytes, &entry->mem) != 0) {
        return OMEGA_WORLD_ERR_NO_MEM;
    }

    memcpy(entry->mem.cpu, code_bytes, code_size);

    entry->object_id = (uint32_t)free_slot;
    if (entry->generation == 0) {
        entry->generation = 1;
    }
    entry->size_bytes = code_size;
    entry->cpu_addr = entry->mem.cpu;
    entry->gpu_va = entry->mem.va;

    sha256_hash(code_bytes, code_size, entry->code_digest);
    if (realization_id) {
        memcpy(entry->realization_id, realization_id, 32);
    } else {
        /* Derive a real realization identity from the realized machine code when
         * the registrant does not supply one, so digest provenance never falls
         * back to a placeholder. */
        static const uint8_t REALIZATION_DOMAIN[32] = "OMEGA_CODE_REALIZATION_V1";
        uint8_t rbuf[64];
        memcpy(rbuf, REALIZATION_DOMAIN, 32);
        memcpy(rbuf + 32, entry->code_digest, 32);
        sha256_hash(rbuf, sizeof(rbuf), entry->realization_id);
    }
    entry->active = true;

    out_handle->world_epoch = world->handle_epoch;
    out_handle->object_id = (uint32_t)free_slot;
    out_handle->object_generation = entry->generation;
    out_handle->object_type = OMEGA_OBJ_CODE;
    out_handle->permissions = OMEGA_PERM_EXECUTE;

    world->code_count++;
    return OMEGA_WORLD_OK;
}

int omega_world_revoke_code(OmegaAcceleratorWorld *world,
                             const OmegaHandle *handle) {
    if (!world || !world->initialized || !handle) return OMEGA_WORLD_ERR_INVALID_ARG;
    if (handle->world_epoch != world->handle_epoch) return OMEGA_WORLD_ERR_STALE_EPOCH;
    if (handle->object_id >= OMEGA_WORLD_MAX_CODE_ENTRIES) return OMEGA_WORLD_ERR_INVALID_ARG;

    OmegaCodeEntry *entry = &world->code_entries[handle->object_id];
    if (!entry->active) return OMEGA_WORLD_ERR_NOT_FOUND;
    if (handle->object_generation != entry->generation) return OMEGA_WORLD_ERR_STALE_GEN;
    if (handle->object_type != OMEGA_OBJ_CODE) return OMEGA_WORLD_ERR_INVALID_ARG;

    entry->active = false;
    if (entry->generation != UINT32_MAX) entry->generation++;
    if (world->code_count > 0) world->code_count--;
    if (code_referenced_in_flight(world, handle->object_id)) {
        entry->retiring = true;
        return OMEGA_WORLD_OK;
    }
    return release_code_memory(world, entry);
}

int omega_world_resolve_code(OmegaAcceleratorWorld *world,
                              const OmegaHandle *handle,
                              const void **out_cpu,
                              uint64_t *out_gpu_va,
                              size_t *out_size) {
    if (!world || !world->initialized || !handle) return OMEGA_WORLD_ERR_INVALID_ARG;
    if (handle->world_epoch != world->handle_epoch) return OMEGA_WORLD_ERR_STALE_EPOCH;
    if (handle->object_id >= OMEGA_WORLD_MAX_CODE_ENTRIES) return OMEGA_WORLD_ERR_INVALID_ARG;

    OmegaCodeEntry *entry = &world->code_entries[handle->object_id];
    if (!entry->active) return OMEGA_WORLD_ERR_NOT_FOUND;
    if (handle->object_generation != entry->generation) return OMEGA_WORLD_ERR_STALE_GEN;
    if (handle->object_type != OMEGA_OBJ_CODE) return OMEGA_WORLD_ERR_INVALID_ARG;
    if (!(handle->permissions & OMEGA_PERM_EXECUTE)) return OMEGA_WORLD_ERR_PERM_DENIED;

    if (out_cpu) *out_cpu = entry->cpu_addr;
    if (out_gpu_va) *out_gpu_va = entry->gpu_va;
    if (out_size) *out_size = entry->size_bytes;
    return OMEGA_WORLD_OK;
}

int omega_world_validate_handle(const OmegaAcceleratorWorld *world,
                                 const OmegaHandle *handle,
                                 uint32_t expected_type,
                                 uint32_t required_perms) {
    if (!world || !world->initialized || !handle) return OMEGA_WORLD_ERR_INVALID_ARG;
    if (handle->world_epoch != world->handle_epoch) return OMEGA_WORLD_ERR_STALE_EPOCH;
    if (handle->object_type != expected_type) return OMEGA_WORLD_ERR_INVALID_ARG;

    if (expected_type == OMEGA_OBJ_BUFFER) {
        if (handle->object_id >= OMEGA_WORLD_MAX_BUFFERS) return OMEGA_WORLD_ERR_INVALID_ARG;
        const OmegaBufferEntry *entry = &world->buffers[handle->object_id];
        if (!entry->active) return OMEGA_WORLD_ERR_NOT_FOUND;
        if (handle->object_generation != entry->generation) return OMEGA_WORLD_ERR_STALE_GEN;
        if ((entry->permissions & required_perms) != required_perms) return OMEGA_WORLD_ERR_PERM_DENIED;
    } else if (expected_type == OMEGA_OBJ_CODE) {
        if (handle->object_id >= OMEGA_WORLD_MAX_CODE_ENTRIES) return OMEGA_WORLD_ERR_INVALID_ARG;
        const OmegaCodeEntry *entry = &world->code_entries[handle->object_id];
        if (!entry->active) return OMEGA_WORLD_ERR_NOT_FOUND;
        if (handle->object_generation != entry->generation) return OMEGA_WORLD_ERR_STALE_GEN;
    } else {
        return OMEGA_WORLD_ERR_INVALID_ARG;
    }

    if ((handle->permissions & required_perms) != required_perms) return OMEGA_WORLD_ERR_PERM_DENIED;
    return OMEGA_WORLD_OK;
}

int omega_world_scratch_acquire(OmegaAcceleratorWorld *world,
                                 size_t size_bytes,
                                 void **out_cpu,
                                 uint64_t *out_gpu_va,
                                 size_t *out_offset) {
    if (!world || !world->initialized || size_bytes == 0 || !out_cpu || !out_gpu_va) {
        return OMEGA_WORLD_ERR_INVALID_ARG;
    }
    if (size_bytes > SIZE_MAX - 255ULL) return OMEGA_WORLD_ERR_BOUNDS;
    size_t aligned = (size_bytes + 255ULL) & ~255ULL;
    if (world->scratch.allocated_bytes > world->scratch.capacity_bytes ||
        aligned > world->scratch.capacity_bytes - world->scratch.allocated_bytes) {
        return OMEGA_WORLD_ERR_NO_MEM;
    }

    size_t off = world->scratch.allocated_bytes;
    *out_cpu = (uint8_t *)world->scratch.mem.cpu + off;
    *out_gpu_va = world->scratch.mem.va + off;
    if (out_offset) *out_offset = off;

    world->scratch.allocated_bytes += aligned;
    return OMEGA_WORLD_OK;
}

void omega_world_scratch_reset(OmegaAcceleratorWorld *world) {
    if (world) {
        world->scratch.allocated_bytes = 0;
    }
}

int omega_world_discover_ring_capacity(OmegaAcceleratorWorld *world,
                                        uint32_t *out_capacity) {
    if (!world || !world->initialized || !out_capacity) return OMEGA_WORLD_ERR_INVALID_ARG;
    *out_capacity = world->ring_capacity;
    return OMEGA_WORLD_OK;
}

int omega_world_recover_channel_fault(OmegaAcceleratorWorld *world) {
    if (!world || !world->initialized) return OMEGA_WORLD_ERR_INVALID_ARG;

    Nvrm *rm = &world->m16.rm;
    /* Reconstruct channel within existing VAS and device root */
    if (nvrm_channel_destroy(rm) != 0) {
        world->faulted = true;
        world->channel_active = false;
        return OMEGA_WORLD_ERR_HARDWARE;
    }

    if (nvrm_channel(rm) != 0) {
        world->faulted = true;
        world->channel_active = false;
        return OMEGA_WORLD_ERR_HARDWARE;
    }

    if (world->channel_generation == UINT32_MAX || world->channel_reconstructions == UINT64_MAX) {
        world->faulted = true;
        world->channel_active = false;
        return OMEGA_WORLD_ERR_FAULT;
    }
    world->channel_generation++;
    world->channel_reconstructions++;
    world->channel_active = true;

    /* Submissions queued on the torn-down channel will never complete and
     * are never committed. */
    for (uint32_t i = 0; i < world->in_flight_count; i++) {
        OmegaWorldSubmission *s = &world->in_flight[i];
        if (!s->committed) {
            if (world->abandoned_dispatches == UINT64_MAX) {
                world->faulted = true;
                return OMEGA_WORLD_ERR_FAULT;
            }
            world->abandoned_dispatches++;
        }
        if (s->output_cpu_protected && s->c_object_id < OMEGA_WORLD_MAX_BUFFERS) {
            OmegaBufferEntry *c = &world->buffers[s->c_object_id];
            if (c->mem.handle && mprotect(c->cpu_addr, c->mem.size, PROT_READ | PROT_WRITE) != 0) {
                world->faulted = true;
                return OMEGA_WORLD_ERR_FAULT;
            }
        }
    }
    world->in_flight_count = 0;
    release_unreferenced_retiring(world);
    if (world->faulted && rm->faulted) return OMEGA_WORLD_ERR_FAULT;
    world->faulted = false;
    return OMEGA_WORLD_OK;
}

int omega_world_update_digest(OmegaAcceleratorWorld *world,
                               const uint8_t *semantic_id,
                               const uint8_t *realization_id,
                               const uint8_t *code_digest,
                               const uint8_t *input_bindings_digest,
                               const uint32_t *obj_id_gen_pairs,
                               uint32_t obj_count,
                               uint32_t capability_scope,
                               uint32_t completion_val,
                               const uint8_t *result_digest,
                               uint32_t outcome_code) {
    if (!world || !world->initialized) return OMEGA_WORLD_ERR_INVALID_ARG;

    /* Canonical binary serialization per spec section 6 */
    uint8_t buf[512];
    size_t off = 0;

    static const uint8_t DOMAIN_TAG[32] = "OMEGA_WORLD_ROLLING_STATE_V1";
    memcpy(&buf[off], DOMAIN_TAG, 32); off += 32;
    memcpy(&buf[off], world->rolling_state_digest, 32); off += 32;

    uint32_t epoch = world->current_epoch;
    memcpy(&buf[off], &epoch, 4); off += 4;

    if (world->sequence_number == UINT64_MAX) return OMEGA_WORLD_ERR_FAULT;
    uint64_t seq = world->sequence_number + 1;
    memcpy(&buf[off], &seq, 8); off += 8;

    uint8_t zero32[32] = {0};
    memcpy(&buf[off], semantic_id ? semantic_id : zero32, 32); off += 32;
    memcpy(&buf[off], realization_id ? realization_id : zero32, 32); off += 32;
    memcpy(&buf[off], code_digest ? code_digest : zero32, 32); off += 32;
    memcpy(&buf[off], input_bindings_digest ? input_bindings_digest : zero32, 32); off += 32;

    uint32_t cnt = obj_count;
    memcpy(&buf[off], &cnt, 4); off += 4;

    size_t pairs_bytes = obj_count * 8;
    if (pairs_bytes > 64) pairs_bytes = 64;
    if (obj_id_gen_pairs && pairs_bytes > 0) {
        memcpy(&buf[off], obj_id_gen_pairs, pairs_bytes);
    } else {
        memset(&buf[off], 0, pairs_bytes);
    }
    off += pairs_bytes;

    memcpy(&buf[off], &capability_scope, 4); off += 4;
    memcpy(&buf[off], &completion_val, 4); off += 4;
    memcpy(&buf[off], result_digest ? result_digest : zero32, 32); off += 32;
    memcpy(&buf[off], &outcome_code, 4); off += 4;

    sha256_hash(buf, off, world->rolling_state_digest);
    world->sequence_number = seq;
    return OMEGA_WORLD_OK;
}

/* Commit a successful physical dispatch to the world trace. The semantic
 * words and handle generations are fixed-width; host addresses and time are
 * deliberately absent from the serialized state. */
static int record_completed_dispatch(OmegaAcceleratorWorld *world,
                                     const uint32_t semantic_words[5],
                                     const OmegaHandle *code,
                                     const OmegaHandle *a,
                                     const OmegaHandle *b,
                                     const OmegaHandle *c,
                                     const void *a_cpu, size_t a_bytes,
                                     const void *b_cpu, size_t b_bytes,
                                     const void *c_cpu, size_t c_bytes,
                                     uint32_t completion,
                                     const uint8_t *submitted_semantic_id,
                                     const uint8_t *submitted_input_digest) {
    if (world->total_dispatches == UINT64_MAX) return OMEGA_WORLD_ERR_FAULT;
    uint8_t semantic_id[32], a_digest[32], b_digest[32];
    uint8_t bindings[64], bindings_digest[32], result_digest[32];
    uint32_t pairs[8] = {
        code->object_id, code->object_generation,
        a->object_id, a->object_generation,
        b->object_id, b->object_generation,
        c->object_id, c->object_generation
    };
    const OmegaCodeEntry *entry = &world->code_entries[code->object_id];
    if (submitted_semantic_id) memcpy(semantic_id, submitted_semantic_id, 32);
    else sha256_hash((const uint8_t *)semantic_words, 5 * sizeof(uint32_t), semantic_id);
    if (submitted_input_digest) memcpy(bindings_digest, submitted_input_digest, 32);
    else {
        sha256_hash((const uint8_t *)a_cpu, a_bytes, a_digest);
        sha256_hash((const uint8_t *)b_cpu, b_bytes, b_digest);
        memcpy(bindings, a_digest, 32);
        memcpy(bindings + 32, b_digest, 32);
        sha256_hash(bindings, sizeof(bindings), bindings_digest);
    }
    sha256_hash((const uint8_t *)c_cpu, c_bytes, result_digest);
    return omega_world_update_digest(world, semantic_id, entry->realization_id,
                                     entry->code_digest, bindings_digest,
                                     pairs, 4, code->permissions | a->permissions |
                                     b->permissions | c->permissions,
                                     completion, result_digest, 0);
}

int omega_world_submit(OmegaAcceleratorWorld *world,
                       const NvrmMem *pb_mem,
                       uint32_t off_bytes,
                       uint32_t nwords,
                       const OmegaWorldSubmission *submission) {
    if (!world || !world->initialized || !pb_mem || !submission) {
        return OMEGA_WORLD_ERR_INVALID_ARG;
    }
    if (world->faulted) return OMEGA_WORLD_ERR_FAULTED;
    if (!world->channel_active) return OMEGA_WORLD_ERR_HARDWARE;
    if (world->in_flight_count >= OMEGA_WORLD_MAX_IN_FLIGHT) {
        return OMEGA_WORLD_ERR_NO_MEM;
    }
    if (submission->c_object_id >= OMEGA_WORLD_MAX_BUFFERS ||
        submission->a_object_id >= OMEGA_WORLD_MAX_BUFFERS ||
        submission->b_object_id >= OMEGA_WORLD_MAX_BUFFERS ||
        submission->code_object_id >= OMEGA_WORLD_MAX_CODE_ENTRIES)
        return OMEGA_WORLD_ERR_INVALID_ARG;
    const OmegaBufferEntry *a = &world->buffers[submission->a_object_id];
    const OmegaBufferEntry *b = &world->buffers[submission->b_object_id];
    const OmegaBufferEntry *c = &world->buffers[submission->c_object_id];
    const OmegaCodeEntry *code = &world->code_entries[submission->code_object_id];
    if (!a->active || !b->active || !c->active || !code->active ||
        a->generation != submission->a_generation ||
        b->generation != submission->b_generation ||
        c->generation != submission->c_generation ||
        code->generation != submission->code_generation ||
        submission->a_bytes > a->size_bytes || submission->b_bytes > b->size_bytes ||
        submission->c_bytes > c->size_bytes || !(c->permissions & OMEGA_PERM_WRITE))
        return OMEGA_WORLD_ERR_INVALID_ARG;
    /* A later submission cannot claim an input digest for an earlier output
     * that the GPU may not have written yet, nor reuse that output as a writer. */
    for (uint32_t i = 0; i < world->in_flight_count; i++) {
        const OmegaWorldSubmission *prior = &world->in_flight[i];
        if (prior->c_bytes &&
            ((submission->c_bytes && prior->c_object_id == submission->c_object_id) ||
             (submission->a_bytes && prior->c_object_id == submission->a_object_id) ||
             (submission->b_bytes && prior->c_object_id == submission->b_object_id)))
            return OMEGA_WORLD_ERR_FAULT;
    }
    /* One payload sequence for the whole world: the pushbuffer's terminal
     * release (built by the caller) must carry a payload strictly after every
     * payload already issued, synchronous or batched. */
    if (!(submission->completion_val != world->completion.last_payload &&
          !payload_reached(submission->completion_val, world->completion.last_payload))) {
        return OMEGA_WORLD_ERR_INVALID_ARG;
    }
    if (submission->c_bytes && mprotect(c->cpu_addr, c->mem.size, PROT_READ) != 0) {
        world->faulted = true;
        return OMEGA_WORLD_ERR_FAULT;
    }
    if (nvrm_enqueue(&world->m16.rm, pb_mem, off_bytes, nwords) != 0) {
        if (submission->c_bytes && mprotect(c->cpu_addr, c->mem.size, PROT_READ | PROT_WRITE) != 0)
            world->faulted = true;
        return OMEGA_WORLD_ERR_HARDWARE;
    }
    world->completion.last_payload = submission->completion_val;
    world->in_flight[world->in_flight_count] = *submission;
    world->in_flight[world->in_flight_count].gp_seq = world->m16.rm.put;
    world->in_flight[world->in_flight_count].channel_generation = world->channel_generation;
    world->in_flight[world->in_flight_count].output_cpu_protected = submission->c_bytes != 0;
    world->in_flight[world->in_flight_count].committed = false;
    sha256_hash((const uint8_t *)submission->semantic_words,
                sizeof submission->semantic_words,
                world->in_flight[world->in_flight_count].semantic_id);
    uint8_t a_digest[32], b_digest[32], bindings[64];
    sha256_hash(a->cpu_addr, submission->a_bytes, a_digest);
    sha256_hash(b->cpu_addr, submission->b_bytes, b_digest);
    memcpy(bindings, a_digest, 32);
    memcpy(bindings + 32, b_digest, 32);
    sha256_hash(bindings, sizeof bindings,
                world->in_flight[world->in_flight_count].input_bindings_digest);
    world->in_flight_count++;
    return OMEGA_WORLD_OK;
}

int omega_world_ring(OmegaAcceleratorWorld *world) {
    if (!world || !world->initialized) return OMEGA_WORLD_ERR_INVALID_ARG;
    if (world->faulted) return OMEGA_WORLD_ERR_FAULTED;
    if (!world->channel_active) return OMEGA_WORLD_ERR_HARDWARE;
    nvrm_ring(&world->m16.rm);
    return OMEGA_WORLD_OK;
}

int omega_world_drain(OmegaAcceleratorWorld *world,
                      uint32_t upto_payload,
                      int timeout_ms) {
    if (!world || !world->initialized) return OMEGA_WORLD_ERR_INVALID_ARG;
    if (world->faulted) return OMEGA_WORLD_ERR_FAULTED;
    if (!world->channel_active) return OMEGA_WORLD_ERR_HARDWARE;
    if (timeout_ms < 0) return OMEGA_WORLD_ERR_INVALID_ARG;

    /* Caller-supplied budget (API contract, gates pass 0..10000 ms): hard total = timeout_ms
     * (0 becomes 1 ms because the primitive maps 0 to its 600 s default), stall = min(5000, total).
     * Waits on the marker only: pushbuffers built by callers (omega_world_gates.c) release
     * the marker without a flush or marker2. */
    uint32_t drain_total = timeout_ms ? (uint32_t)timeout_ms : 1u;
    if (world_wait_seq(__func__, "marker", world->completion.cpu_marker, upto_payload,
                       drain_total < 5000 ? drain_total : 5000, drain_total) != 0) {
        world->faulted = true;
        return OMEGA_WORLD_ERR_HARDWARE;
    }

    /* Pass 1: validate every completed submission before any side effect, so
     * a failure never leaves a partially committed queue behind. */
    for (uint32_t i = 0; i < world->in_flight_count; i++) {
        const OmegaWorldSubmission *s = &world->in_flight[i];
        if (!payload_reached(s->completion_val, upto_payload)) continue;
        if (s->channel_generation != world->channel_generation) {
            world->faulted = true;
            return OMEGA_WORLD_ERR_FAULT;
        }
        if (s->code_object_id >= OMEGA_WORLD_MAX_CODE_ENTRIES ||
            s->a_object_id >= OMEGA_WORLD_MAX_BUFFERS ||
            s->b_object_id >= OMEGA_WORLD_MAX_BUFFERS ||
            s->c_object_id >= OMEGA_WORLD_MAX_BUFFERS) {
            world->faulted = true;
            return OMEGA_WORLD_ERR_INVALID_ARG;
        }
    }

    /* Pass 2: each completed submission is committed exactly once, or
     * abandoned if an object it bound was revoked while it was in flight. */
    int committed = 0;
    uint32_t keep = 0;
    uint32_t retire_to = world->m16.rm.retired;
    for (uint32_t i = 0; i < world->in_flight_count; i++) {
        const OmegaWorldSubmission s = world->in_flight[i];
        if (!payload_reached(s.completion_val, upto_payload)) {
            world->in_flight[keep++] = s;
            continue;
        }
        /* Retire by GPFIFO queue sequence, never by completion payload. */
        if ((int32_t)(s.gp_seq - retire_to) > 0) retire_to = s.gp_seq;

        const OmegaBufferEntry *ab = &world->buffers[s.a_object_id];
        const OmegaBufferEntry *bb = &world->buffers[s.b_object_id];
        const OmegaBufferEntry *cb = &world->buffers[s.c_object_id];
        const OmegaCodeEntry *ce = &world->code_entries[s.code_object_id];
        if (!ab->active || ab->generation != s.a_generation ||
            !bb->active || bb->generation != s.b_generation ||
            !cb->active || cb->generation != s.c_generation ||
            !ce->active || ce->generation != s.code_generation) {
            if (world->abandoned_dispatches == UINT64_MAX) {
                world->faulted = true;
                return OMEGA_WORLD_ERR_FAULT;
            }
            world->abandoned_dispatches++;
            if (s.output_cpu_protected && cb->active &&
                mprotect(cb->cpu_addr, cb->mem.size, PROT_READ | PROT_WRITE) != 0) {
                world->faulted = true;
                return OMEGA_WORLD_ERR_FAULT;
            }
            continue;
        }

        OmegaHandle ch = {
            .world_epoch = world->handle_epoch,
            .object_id = s.code_object_id,
            .object_generation = s.code_generation,
            .object_type = OMEGA_OBJ_CODE,
            .permissions = s.code_permissions & OMEGA_PERM_EXECUTE
        };
        OmegaHandle ah = {
            .world_epoch = world->handle_epoch,
            .object_id = s.a_object_id,
            .object_generation = s.a_generation,
            .object_type = OMEGA_OBJ_BUFFER,
            .permissions = s.a_permissions
        };
        OmegaHandle bh = {
            .world_epoch = world->handle_epoch,
            .object_id = s.b_object_id,
            .object_generation = s.b_generation,
            .object_type = OMEGA_OBJ_BUFFER,
            .permissions = s.b_permissions
        };
        OmegaHandle cwh = {
            .world_epoch = world->handle_epoch,
            .object_id = s.c_object_id,
            .object_generation = s.c_generation,
            .object_type = OMEGA_OBJ_BUFFER,
            .permissions = s.c_permissions
        };

        if (record_completed_dispatch(world, s.semantic_words, &ch, &ah, &bh, &cwh,
                                      ab->cpu_addr, s.a_bytes,
                                      bb->cpu_addr, s.b_bytes,
                                      cb->cpu_addr, s.c_bytes,
                                      s.completion_val, s.semantic_id,
                                      s.input_bindings_digest) != OMEGA_WORLD_OK) {
            /* Keep this and every later submission uncommitted; drop only
             * those already committed above. */
            for (uint32_t j = i; j < world->in_flight_count; j++) {
                world->in_flight[keep++] = world->in_flight[j];
            }
            world->in_flight_count = keep;
            world->faulted = true;
            return OMEGA_WORLD_ERR_FAULT;
        }
        world->in_flight[i].committed = true;
        world->total_dispatches++;
        if (s.output_cpu_protected &&
            mprotect(cb->cpu_addr, cb->mem.size, PROT_READ | PROT_WRITE) != 0) {
            world->faulted = true;
            return OMEGA_WORLD_ERR_FAULT;
        }
        committed++;
    }
    nvrm_retire(&world->m16.rm, retire_to);
    world->in_flight_count = keep;
    release_unreferenced_retiring(world);
    return committed;
}

int omega_world_dispatch_vector(OmegaAcceleratorWorld *world,
                                 const OmegaHandle *code_handle,
                                 const OmegaHandle *h_a,
                                 const OmegaHandle *h_b,
                                 const OmegaHandle *h_c,
                                 uint32_t n,
                                 uint32_t *out_completion_code) {
    if (!world || !world->initialized || !code_handle || !h_a || !h_b || !h_c || n == 0) {
        return OMEGA_WORLD_ERR_INVALID_ARG;
    }
    if (world->faulted) return OMEGA_WORLD_ERR_FAULTED;
    if (!world->channel_active) return OMEGA_WORLD_ERR_HARDWARE;

    /* Resolve code and buffers with internal bounds and permission authority */
    uint64_t code_va = 0;
    size_t code_size = 0;
    if (omega_world_resolve_code(world, code_handle, NULL, &code_va, &code_size) != 0) {
        return OMEGA_WORLD_ERR_FAULT;
    }

    size_t req_bytes;
    if (n > UINT32_MAX - 63 ||
        __builtin_mul_overflow((size_t)n, sizeof(uint32_t), &req_bytes))
        return OMEGA_WORLD_ERR_BOUNDS;
    void *a_cpu = NULL, *b_cpu = NULL, *c_cpu = NULL;
    uint64_t a_va = 0, b_va = 0, c_va = 0;

    if (omega_world_resolve_buffer(world, h_a, OMEGA_PERM_READ, 0, req_bytes, &a_cpu, &a_va) != 0) {
        return OMEGA_WORLD_ERR_PERM_DENIED;
    }
    if (omega_world_resolve_buffer(world, h_b, OMEGA_PERM_READ, 0, req_bytes, &b_cpu, &b_va) != 0) {
        return OMEGA_WORLD_ERR_PERM_DENIED;
    }
    if (omega_world_resolve_buffer(world, h_c, OMEGA_PERM_WRITE, 0, req_bytes, &c_cpu, &c_va) != 0) {
        return OMEGA_WORLD_ERR_PERM_DENIED;
    }

    /* Acquire scratch space */
    void *cbank_cpu = NULL, *qmd0_cpu = NULL, *qmd1_cpu = NULL, *sem_cpu = NULL;
    uint64_t cbank_va = 0, qmd0_va = 0, qmd1_va = 0, sem_va = 0;
    size_t dummy_off = 0;

    if (omega_world_scratch_acquire(world, 0x1000, &cbank_cpu, &cbank_va, &dummy_off) != 0) return OMEGA_WORLD_ERR_NO_MEM;
    if (omega_world_scratch_acquire(world, 0x1000, &qmd0_cpu, &qmd0_va, &dummy_off) != 0) return OMEGA_WORLD_ERR_NO_MEM;
    if (omega_world_scratch_acquire(world, 0x1000, &qmd1_cpu, &qmd1_va, &dummy_off) != 0) return OMEGA_WORLD_ERR_NO_MEM;
    if (omega_world_scratch_acquire(world, 0x1000, &sem_cpu, &sem_va, &dummy_off) != 0) return OMEGA_WORLD_ERR_NO_MEM;

    uint32_t cbank_data[OMEGA_BW_CBANK_DRIVER_WORDS];
    omega_blackwell_build_cbank_driver(cbank_data, cbank_va);

    uint32_t cbank_args[OMEGA_BW_CBANK_ARGS_WORDS];
    omega_blackwell_build_cbank_args(cbank_args, a_va, b_va, c_va, n);

    memcpy(cbank_cpu, cbank_data, sizeof(cbank_data));
    memcpy((uint8_t *)cbank_cpu + 0x380, cbank_args, sizeof(cbank_args));

    OmegaBlackwellQmdConfig qmd_cfg = {
        .code_va = code_va,
        .cbank_va = cbank_va,
        .scratch_va = cbank_va + 0x2000,
        .sem_va = sem_va,
        .qmd0_va = qmd0_va,
        .qmd1_va = qmd1_va,
        .num_elements = n,
        .threads_per_block = 64,
        .grid_width = (n + 63) / 64
    };
    if (qmd_cfg.grid_width == 0) qmd_cfg.grid_width = 1;

    uint32_t qmd0_words[OMEGA_BW_QMD_WORDS];
    uint32_t qmd1_words[OMEGA_BW_QMD_WORDS];
    omega_blackwell_build_qmd0(qmd0_words, qmd0_va, qmd1_va);
    omega_blackwell_build_qmd1(qmd1_words, &qmd_cfg);

    memcpy(qmd0_cpu, qmd0_words, sizeof(qmd0_words));
    memcpy(qmd1_cpu, qmd1_words, sizeof(qmd1_words));

    volatile uint32_t *hsem = (volatile uint32_t *)sem_cpu;
    *hsem = 0;
    /* The completion marker is monotonic and never written by the CPU after
     * init; this dispatch takes the next payload in the world's sequence. */
    uint32_t payload = ++world->completion.last_payload;

    /* Assemble pushbuffer */
    uint32_t pb[1024];
    size_t pb_len = 0;

    memcpy(&pb[pb_len], WORLD_SETUP_WORDS, sizeof(WORLD_SETUP_WORDS));
    pb_len += sizeof(WORLD_SETUP_WORDS) / 4;

    /* Cbank driver upload */
    pb[pb_len++] = nvrm_mthd(1, 0x0188, 2);
    pb[pb_len++] = (uint32_t)(cbank_va >> 32);
    pb[pb_len++] = (uint32_t)cbank_va;
    pb[pb_len++] = nvrm_mthd(1, 0x0180, 2);
    pb[pb_len++] = 0x00000380;
    pb[pb_len++] = 0x00000001;
    pb[pb_len++] = nvrm_mthd(1, 0x01b0, 1);
    pb[pb_len++] = 0x00000041;
    pb[pb_len++] = (224 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    memcpy(&pb[pb_len], cbank_data, 224 * 4);
    pb_len += 224;

    /* Kernel args upload */
    pb[pb_len++] = nvrm_mthd(1, 0x0188, 2);
    pb[pb_len++] = (uint32_t)((cbank_va + 0x380) >> 32);
    pb[pb_len++] = (uint32_t)(cbank_va + 0x380);
    pb[pb_len++] = nvrm_mthd(1, 0x0180, 2);
    pb[pb_len++] = 0x0000001c;
    pb[pb_len++] = 0x00000001;
    pb[pb_len++] = nvrm_mthd(1, 0x01b0, 1);
    pb[pb_len++] = 0x00000041;
    pb[pb_len++] = (7 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    memcpy(&pb[pb_len], cbank_args, 7 * 4);
    pb_len += 7;

    /* Inline QMD0 */
    pb[pb_len++] = (98 << 16) | (1 << 13) | (0x0318 >> 2) | (2u << 28);
    pb[pb_len++] = (1u << 30) | (uint32_t)((qmd0_va >> 40) & 0x1ff);
    pb[pb_len++] = (uint32_t)(qmd0_va >> 8);
    memcpy(&pb[pb_len], qmd0_words, 96 * 4);
    pb_len += 96;

    /* Intermediate semaphore upload */
    pb[pb_len++] = nvrm_mthd(1, 0x0188, 2);
    pb[pb_len++] = (uint32_t)(sem_va >> 32);
    pb[pb_len++] = (uint32_t)sem_va;
    pb[pb_len++] = nvrm_mthd(1, 0x0180, 2);
    pb[pb_len++] = 0x00000004;
    pb[pb_len++] = 0x00000001;
    pb[pb_len++] = nvrm_mthd(1, 0x01b0, 1);
    pb[pb_len++] = 0x00000041;
    pb[pb_len++] = (1 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    pb[pb_len++] = OMEGA_BW_SEMAPHORE_INTERMEDIATE_INIT;

    /* Inline QMD1 */
    pb[pb_len++] = (98 << 16) | (1 << 13) | (0x0318 >> 2) | (2u << 28);
    pb[pb_len++] = (1u << 30) | (uint32_t)((qmd1_va >> 40) & 0x1ff);
    pb[pb_len++] = (uint32_t)(qmd1_va >> 8);
    memcpy(&pb[pb_len], qmd1_words, 96 * 4);
    pb_len += 96;

    /* Subchannel 0 completion release (WFI) */
    pb[pb_len++] = nvrm_mthd(0, 0x005c, 5);
    pb[pb_len++] = (uint32_t)world->completion.gpu_va;
    pb[pb_len++] = (uint32_t)(world->completion.gpu_va >> 32);
    pb[pb_len++] = payload;
    pb[pb_len++] = 0;
    pb[pb_len++] = 0x1 | (1u << 20); // RELEASE | WFI

    /* C3 tail, copied from omega_blackwell_engine.c:461-472 (MEM_OP_A..D =
     * 0x28..0x34, L2_FLUSH_DIRTY 0x10 in bits 31:27, clc96f.h:36-73): flush the
     * L2, then a second WFI release of the same payload at marker page + 0x10.
     * Dispatch waits for this one before it reads any output. */
    pb[pb_len++] = nvrm_mthd(0, 0x0028, 4);
    pb[pb_len++] = 0;
    pb[pb_len++] = 0;
    pb[pb_len++] = 0;
    pb[pb_len++] = 0x10u << 27;
    pb[pb_len++] = nvrm_mthd(0, 0x005c, 5);
    pb[pb_len++] = (uint32_t)world->completion.gpu_va2;
    pb[pb_len++] = (uint32_t)(world->completion.gpu_va2 >> 32);
    pb[pb_len++] = payload;
    pb[pb_len++] = 0;
    pb[pb_len++] = 0x1 | (1u << 20); // RELEASE | WFI

    /* Submit to persistent GPFIFO ring */
    if (m16_native_submit_methods(&world->m16, pb, pb_len) != 0) {
        omega_world_scratch_reset(world);
        return OMEGA_WORLD_ERR_HARDWARE;
    }

    if (world_wait_dispatch(world, __func__, payload) != 0) {
        world->faulted = true;
        omega_world_scratch_reset(world);
        return OMEGA_WORLD_ERR_HARDWARE;
    }

    nvrm_retire(&world->m16.rm, world->m16.rm.put);

    const uint32_t semantic_words[5] = {1, n, 0, 0, 0};
    if (record_completed_dispatch(world, semantic_words, code_handle,
                                  h_a, h_b, h_c, a_cpu, req_bytes,
                                  b_cpu, req_bytes, c_cpu, req_bytes,
                                  payload, NULL, NULL) != OMEGA_WORLD_OK) {
        omega_world_scratch_reset(world);
        return OMEGA_WORLD_ERR_FAULT;
    }

    if (out_completion_code) {
        *out_completion_code = payload;
    }
    world->total_dispatches++;

    /* Reset scratch arena for next dispatch */
    omega_world_scratch_reset(world);
    return OMEGA_WORLD_OK;
}

int omega_world_dispatch_matmul(OmegaAcceleratorWorld *world,
                                 const OmegaMatMulSpec *spec,
                                 const OmegaHandle *code_handle,
                                 const OmegaHandle *h_a,
                                 const OmegaHandle *h_b,
                                 const OmegaHandle *h_c,
                                 uint32_t *out_completion_code) {
    if (!world || !world->initialized || !spec || !code_handle || !h_a || !h_b || !h_c) {
        return OMEGA_WORLD_ERR_INVALID_ARG;
    }
    if (world->faulted) return OMEGA_WORLD_ERR_FAULTED;
    if (!world->channel_active) return OMEGA_WORLD_ERR_HARDWARE;

    uint64_t code_va = 0;
    size_t code_size = 0;
    if (omega_world_resolve_code(world, code_handle, NULL, &code_va, &code_size) != 0) {
        return OMEGA_WORLD_ERR_FAULT;
    }

    size_t elem_size = (spec->precision == OMEGA_MATMUL_PRECISION_INT32) ? 4 : 2;
    size_t out_elem_size = (spec->precision == OMEGA_MATMUL_PRECISION_INT32) ? 4 : 4; /* FP32 accum */

    size_t req_a, req_b, req_c;
    if (__builtin_mul_overflow((size_t)spec->m, (size_t)spec->k, &req_a) ||
        __builtin_mul_overflow(req_a, elem_size, &req_a) ||
        __builtin_mul_overflow((size_t)spec->k, (size_t)spec->n, &req_b) ||
        __builtin_mul_overflow(req_b, elem_size, &req_b) ||
        __builtin_mul_overflow((size_t)spec->m, (size_t)spec->n, &req_c) ||
        __builtin_mul_overflow(req_c, out_elem_size, &req_c))
        return OMEGA_WORLD_ERR_BOUNDS;
    if (spec->precision == OMEGA_MATMUL_PRECISION_INT32 &&
        (spec->n > UINT32_MAX - 15 || spec->m > UINT32_MAX - 15))
        return OMEGA_WORLD_ERR_BOUNDS;

    void *a_cpu = NULL, *b_cpu = NULL, *c_cpu = NULL;
    uint64_t a_va = 0, b_va = 0, c_va = 0;

    if (omega_world_resolve_buffer(world, h_a, OMEGA_PERM_READ, 0, req_a, &a_cpu, &a_va) != 0) {
        return OMEGA_WORLD_ERR_PERM_DENIED;
    }
    if (omega_world_resolve_buffer(world, h_b, OMEGA_PERM_READ, 0, req_b, &b_cpu, &b_va) != 0) {
        return OMEGA_WORLD_ERR_PERM_DENIED;
    }
    if (omega_world_resolve_buffer(world, h_c, OMEGA_PERM_WRITE, 0, req_c, &c_cpu, &c_va) != 0) {
        return OMEGA_WORLD_ERR_PERM_DENIED;
    }

    void *cbank_cpu = NULL, *qmd0_cpu = NULL, *qmd1_cpu = NULL, *sem_cpu = NULL, *kernel_scratch_cpu = NULL;
    uint64_t cbank_va = 0, qmd0_va = 0, qmd1_va = 0, sem_va = 0, kernel_scratch_va = 0;
    size_t dummy_off = 0;

    if (omega_world_scratch_acquire(world, 0x1000, &cbank_cpu, &cbank_va, &dummy_off) != 0) return OMEGA_WORLD_ERR_NO_MEM;
    if (omega_world_scratch_acquire(world, 0x1000, &qmd0_cpu, &qmd0_va, &dummy_off) != 0) return OMEGA_WORLD_ERR_NO_MEM;
    if (omega_world_scratch_acquire(world, 0x1000, &qmd1_cpu, &qmd1_va, &dummy_off) != 0) return OMEGA_WORLD_ERR_NO_MEM;
    if (omega_world_scratch_acquire(world, 0x1000, &sem_cpu, &sem_va, &dummy_off) != 0) return OMEGA_WORLD_ERR_NO_MEM;
    if (omega_world_scratch_acquire(world, 0x4000, &kernel_scratch_cpu, &kernel_scratch_va, &dummy_off) != 0) return OMEGA_WORLD_ERR_NO_MEM;

    uint32_t cbank_data[OMEGA_BW_CBANK_DRIVER_WORDS];
    uint32_t threads_x, threads_y, grid_x, grid_y, threads_per_block, gpr_count;
    if (spec->precision == OMEGA_MATMUL_PRECISION_INT32) {
        threads_x = 16;
        threads_y = 16;
        grid_x = (spec->n + threads_x - 1) / threads_x;
        grid_y = (spec->m + threads_y - 1) / threads_y;
        threads_per_block = threads_x * threads_y;
        gpr_count = 32;
    } else {
        threads_x = 32;
        threads_y = 1;
        grid_x = spec->n / 8;
        grid_y = spec->m / 16;
        threads_per_block = 32;
        gpr_count = 64;
    }
    if (grid_x == 0) grid_x = 1;
    if (grid_y == 0) grid_y = 1;
    if (grid_y > UINT32_MAX / grid_x) {
        omega_world_scratch_reset(world);
        return OMEGA_WORLD_ERR_BOUNDS;
    }

    omega_blackwell_build_cbank_driver_2d(cbank_data, cbank_va, threads_x, threads_y, grid_x, grid_y);

    uint32_t cbank_args[OMEGA_BW_CBANK_MATMUL_ARGS_WORDS];
    omega_blackwell_build_cbank_args_matmul(cbank_args, a_va, b_va, c_va, spec->m, spec->k, spec->n);

    memcpy(cbank_cpu, cbank_data, sizeof(cbank_data));
    memcpy((uint8_t *)cbank_cpu + 0x380, cbank_args, sizeof(cbank_args));

    OmegaBlackwellQmdConfig qmd_cfg = {
        .code_va = code_va,
        .cbank_va = cbank_va,
        .scratch_va = kernel_scratch_va,
        .sem_va = sem_va,
        .qmd0_va = qmd0_va,
        .qmd1_va = qmd1_va,
        .threads_x = threads_x,
        .threads_y = threads_y,
        .grid_x = grid_x,
        .grid_y = grid_y,
        .threads_per_block = threads_per_block,
        .grid_width = grid_x * grid_y,
        .num_elements = spec->m * spec->n,
        .gpr_count = gpr_count
    };

    uint32_t qmd0_words[OMEGA_BW_QMD_WORDS];
    uint32_t qmd1_words[OMEGA_BW_QMD_WORDS];
    omega_blackwell_build_qmd0(qmd0_words, qmd0_va, qmd1_va);
    omega_blackwell_build_qmd1(qmd1_words, &qmd_cfg);

    memcpy(qmd0_cpu, qmd0_words, sizeof(qmd0_words));
    memcpy(qmd1_cpu, qmd1_words, sizeof(qmd1_words));

    volatile uint32_t *hsem = (volatile uint32_t *)sem_cpu;
    *hsem = 0;
    /* The completion marker is monotonic and never written by the CPU after
     * init; this dispatch takes the next payload in the world's sequence. */
    uint32_t payload = ++world->completion.last_payload;

    /* Assemble pushbuffer */
    uint32_t pb[1024];
    size_t pb_len = 0;

    memcpy(&pb[pb_len], WORLD_SETUP_WORDS, sizeof(WORLD_SETUP_WORDS));
    pb_len += sizeof(WORLD_SETUP_WORDS) / 4;

    /* Cbank driver upload */
    pb[pb_len++] = nvrm_mthd(1, 0x0188, 2);
    pb[pb_len++] = (uint32_t)(cbank_va >> 32);
    pb[pb_len++] = (uint32_t)cbank_va;
    pb[pb_len++] = nvrm_mthd(1, 0x0180, 2);
    pb[pb_len++] = 0x00000380;
    pb[pb_len++] = 0x00000001;
    pb[pb_len++] = nvrm_mthd(1, 0x01b0, 1);
    pb[pb_len++] = 0x00000041;
    pb[pb_len++] = (224 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    memcpy(&pb[pb_len], cbank_data, 224 * 4);
    pb_len += 224;

    /* Kernel args upload (10 words) */
    pb[pb_len++] = nvrm_mthd(1, 0x0188, 2);
    pb[pb_len++] = (uint32_t)((cbank_va + 0x380) >> 32);
    pb[pb_len++] = (uint32_t)(cbank_va + 0x380);
    pb[pb_len++] = nvrm_mthd(1, 0x0180, 2);
    pb[pb_len++] = 0x00000028;
    pb[pb_len++] = 0x00000001;
    pb[pb_len++] = nvrm_mthd(1, 0x01b0, 1);
    pb[pb_len++] = 0x00000041;
    pb[pb_len++] = (10 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    memcpy(&pb[pb_len], cbank_args, 10 * 4);
    pb_len += 10;

    /* Inline QMD0 */
    pb[pb_len++] = (98 << 16) | (1 << 13) | (0x0318 >> 2) | (2u << 28);
    pb[pb_len++] = (1u << 30) | (uint32_t)((qmd0_va >> 40) & 0x1ff);
    pb[pb_len++] = (uint32_t)(qmd0_va >> 8);
    memcpy(&pb[pb_len], qmd0_words, 96 * 4);
    pb_len += 96;

    /* Intermediate semaphore upload */
    pb[pb_len++] = nvrm_mthd(1, 0x0188, 2);
    pb[pb_len++] = (uint32_t)(sem_va >> 32);
    pb[pb_len++] = (uint32_t)sem_va;
    pb[pb_len++] = nvrm_mthd(1, 0x0180, 2);
    pb[pb_len++] = 0x00000004;
    pb[pb_len++] = 0x00000001;
    pb[pb_len++] = nvrm_mthd(1, 0x01b0, 1);
    pb[pb_len++] = 0x00000041;
    pb[pb_len++] = (1 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    pb[pb_len++] = OMEGA_BW_SEMAPHORE_INTERMEDIATE_INIT;

    /* Inline QMD1 */
    pb[pb_len++] = (98 << 16) | (1 << 13) | (0x0318 >> 2) | (2u << 28);
    pb[pb_len++] = (1u << 30) | (uint32_t)((qmd1_va >> 40) & 0x1ff);
    pb[pb_len++] = (uint32_t)(qmd1_va >> 8);
    memcpy(&pb[pb_len], qmd1_words, 96 * 4);
    pb_len += 96;

    /* Subchannel 0 completion release (WFI) */
    pb[pb_len++] = nvrm_mthd(0, 0x005c, 5);
    pb[pb_len++] = (uint32_t)world->completion.gpu_va;
    pb[pb_len++] = (uint32_t)(world->completion.gpu_va >> 32);
    pb[pb_len++] = payload;
    pb[pb_len++] = 0;
    pb[pb_len++] = 0x1 | (1u << 20); // RELEASE | WFI

    /* C3 tail, copied from omega_blackwell_engine.c:461-472 (MEM_OP_A..D =
     * 0x28..0x34, L2_FLUSH_DIRTY 0x10 in bits 31:27, clc96f.h:36-73): flush the
     * L2, then a second WFI release of the same payload at marker page + 0x10.
     * Dispatch waits for this one before it reads any output. */
    pb[pb_len++] = nvrm_mthd(0, 0x0028, 4);
    pb[pb_len++] = 0;
    pb[pb_len++] = 0;
    pb[pb_len++] = 0;
    pb[pb_len++] = 0x10u << 27;
    pb[pb_len++] = nvrm_mthd(0, 0x005c, 5);
    pb[pb_len++] = (uint32_t)world->completion.gpu_va2;
    pb[pb_len++] = (uint32_t)(world->completion.gpu_va2 >> 32);
    pb[pb_len++] = payload;
    pb[pb_len++] = 0;
    pb[pb_len++] = 0x1 | (1u << 20); // RELEASE | WFI

    /* Submit to persistent GPFIFO ring */
    if (m16_native_submit_methods(&world->m16, pb, pb_len) != 0) {
        omega_world_scratch_reset(world);
        return OMEGA_WORLD_ERR_HARDWARE;
    }

    if (world_wait_dispatch(world, __func__, payload) != 0) {
        world->faulted = true;
        omega_world_scratch_reset(world);
        return OMEGA_WORLD_ERR_HARDWARE;
    }

    nvrm_retire(&world->m16.rm, world->m16.rm.put);

    const uint32_t semantic_words[5] = {2, spec->m, spec->k, spec->n,
                                        (uint32_t)spec->precision};
    if (record_completed_dispatch(world, semantic_words, code_handle,
                                  h_a, h_b, h_c, a_cpu, req_a,
                                  b_cpu, req_b, c_cpu, req_c,
                                  payload, NULL, NULL) != OMEGA_WORLD_OK) {
        omega_world_scratch_reset(world);
        return OMEGA_WORLD_ERR_FAULT;
    }

    if (out_completion_code) {
        *out_completion_code = payload;
    }
    world->total_dispatches++;

    omega_world_scratch_reset(world);
    return OMEGA_WORLD_OK;
}
