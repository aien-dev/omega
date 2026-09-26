#include "omega_accelerator_world.h"
#include "omega_blackwell_qmd.h"
#include "omega_blackwell_encoder.h"
#include "omega_blackwell_codegen.h"
#include "omega_blackwell_submit.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "nvos.h"
#include "nv-ioctl.h"
#include "nv-ioctl-numbers.h"

#define NV_ESC_RM_FREE 0x29
#define NV_IOWR(nr, sz) _IOC(_IOC_READ | _IOC_WRITE, NV_IOCTL_MAGIC, (nr), (sz))


static const uint32_t WORLD_SETUP_WORDS[18] = {
    0x20012061, 0x0000cec0, 0x20012092, 0x00000001, 0x200120a8, 0x0000000f, 0x2001255d, 0x00000003,
    0x2001255e, 0x20000000, 0x2001255f, 0x000fffff, 0x20012557, 0x00000003, 0x20012558, 0x22000000,
    0x20012559, 0x00000000,
};

static void rm_free_obj(Nvrm *rm, uint32_t parent, uint32_t obj) {
    if (!rm->root || !parent || !obj) return;
    NVOS00_PARAMETERS p;
    memset(&p, 0, sizeof(p));
    p.hRoot = rm->root;
    p.hObjectParent = parent;
    p.hObjectOld = obj;
    ioctl(rm->fd_ctl, NV_IOWR(NV_ESC_RM_FREE, sizeof(p)), &p);
}

int omega_world_init(OmegaAcceleratorWorld *world) {
    if (!world) return OMEGA_WORLD_ERR_INVALID_ARG;
    memset(world, 0, sizeof(*world));

    world->current_epoch = 1;
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

    /* Allocate persistent pushbuffer ring */
    if (nvrm_alloc(&world->m16.rm, 0x10000, &world->pb_mem) != 0) {
        m16_native_close(&world->m16);
        return OMEGA_WORLD_ERR_NO_MEM;
    }
    world->m16.pb_mem = world->pb_mem;

    /* Allocate persistent scratch arena */
    if (nvrm_alloc(&world->m16.rm, OMEGA_WORLD_SCRATCH_SIZE, &world->scratch.mem) != 0) {
        m16_native_close(&world->m16);
        return OMEGA_WORLD_ERR_NO_MEM;
    }
    world->scratch.capacity_bytes = OMEGA_WORLD_SCRATCH_SIZE;
    world->scratch.allocated_bytes = 0;

    /* Allocate persistent completion marker */
    if (nvrm_alloc(&world->m16.rm, 0x1000, &world->completion.mem) != 0) {
        m16_native_close(&world->m16);
        return OMEGA_WORLD_ERR_NO_MEM;
    }
    world->completion.cpu_marker = (volatile uint32_t *)world->completion.mem.cpu;
    world->completion.gpu_va = world->completion.mem.va;
    *world->completion.cpu_marker = 0;
    world->completion.last_payload = 0;

    /* Initialize rolling state digest D_0 */
    memset(world->rolling_state_digest, 0, sizeof(world->rolling_state_digest));
    world->sequence_number = 0;

    world->initialized = true;
    return OMEGA_WORLD_OK;
}

void omega_world_destroy(OmegaAcceleratorWorld *world) {
    if (!world || !world->initialized) return;

    for (uint32_t i = 0; i < OMEGA_WORLD_MAX_BUFFERS; i++) {
        world->buffers[i].active = false;
        world->buffers[i].generation++;
    }
    for (uint32_t i = 0; i < OMEGA_WORLD_MAX_CODE_ENTRIES; i++) {
        world->code_entries[i].active = false;
        world->code_entries[i].generation++;
    }

    m16_native_close(&world->m16);
    world->channel_active = false;
    world->initialized = false;
}

int omega_world_rebuild(OmegaAcceleratorWorld *world) {
    if (!world) return OMEGA_WORLD_ERR_INVALID_ARG;
    uint32_t next_epoch = world->current_epoch + 1;
    omega_world_destroy(world);

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

    int free_slot = -1;
    for (uint32_t i = 0; i < OMEGA_WORLD_MAX_BUFFERS; i++) {
        if (!world->buffers[i].active) {
            free_slot = (int)i;
            break;
        }
    }
    if (free_slot < 0) return OMEGA_WORLD_ERR_NO_MEM;

    OmegaBufferEntry *entry = &world->buffers[free_slot];
    size_t alloc_bytes = (size_bytes + 0xfffULL) & ~0xfffULL;
    if (alloc_bytes < 0x1000) alloc_bytes = 0x1000;

    if (nvrm_alloc(&world->m16.rm, alloc_bytes, &entry->mem) != 0) {
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

    out_handle->world_epoch = world->current_epoch;
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
    if (handle->world_epoch != world->current_epoch) return OMEGA_WORLD_ERR_STALE_EPOCH;
    if (handle->object_id >= OMEGA_WORLD_MAX_BUFFERS) return OMEGA_WORLD_ERR_INVALID_ARG;

    OmegaBufferEntry *entry = &world->buffers[handle->object_id];
    if (!entry->active) return OMEGA_WORLD_ERR_NOT_FOUND;
    if (handle->object_generation != entry->generation) return OMEGA_WORLD_ERR_STALE_GEN;
    if (handle->object_type != OMEGA_OBJ_BUFFER) return OMEGA_WORLD_ERR_INVALID_ARG;

    entry->active = false;
    entry->generation++;
    if (world->buffer_count > 0) world->buffer_count--;
    return OMEGA_WORLD_OK;
}

int omega_world_resolve_buffer(OmegaAcceleratorWorld *world,
                                const OmegaHandle *handle,
                                uint32_t required_perms,
                                size_t req_offset,
                                size_t req_size,
                                void **out_cpu,
                                uint64_t *out_gpu_va) {
    if (!world || !world->initialized || !handle) return OMEGA_WORLD_ERR_INVALID_ARG;
    if (handle->world_epoch != world->current_epoch) return OMEGA_WORLD_ERR_STALE_EPOCH;
    if (handle->object_id >= OMEGA_WORLD_MAX_BUFFERS) return OMEGA_WORLD_ERR_INVALID_ARG;

    OmegaBufferEntry *entry = &world->buffers[handle->object_id];
    if (!entry->active) return OMEGA_WORLD_ERR_NOT_FOUND;
    if (handle->object_generation != entry->generation) return OMEGA_WORLD_ERR_STALE_GEN;
    if (handle->object_type != OMEGA_OBJ_BUFFER) return OMEGA_WORLD_ERR_INVALID_ARG;

    if ((entry->permissions & required_perms) != required_perms) return OMEGA_WORLD_ERR_PERM_DENIED;
    if ((handle->permissions & required_perms) != required_perms) return OMEGA_WORLD_ERR_PERM_DENIED;

    if (req_offset + req_size > entry->size_bytes) return OMEGA_WORLD_ERR_BOUNDS;

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

    int free_slot = -1;
    for (uint32_t i = 0; i < OMEGA_WORLD_MAX_CODE_ENTRIES; i++) {
        if (!world->code_entries[i].active) {
            free_slot = (int)i;
            break;
        }
    }
    if (free_slot < 0) return OMEGA_WORLD_ERR_NO_MEM;

    OmegaCodeEntry *entry = &world->code_entries[free_slot];
    size_t alloc_bytes = (code_size + 0xfffULL) & ~0xfffULL;
    if (alloc_bytes < 0x1000) alloc_bytes = 0x1000;

    if (nvrm_alloc(&world->m16.rm, alloc_bytes, &entry->mem) != 0) {
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
        memset(entry->realization_id, 0, 32);
    }
    entry->active = true;

    out_handle->world_epoch = world->current_epoch;
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
    if (handle->world_epoch != world->current_epoch) return OMEGA_WORLD_ERR_STALE_EPOCH;
    if (handle->object_id >= OMEGA_WORLD_MAX_CODE_ENTRIES) return OMEGA_WORLD_ERR_INVALID_ARG;

    OmegaCodeEntry *entry = &world->code_entries[handle->object_id];
    if (!entry->active) return OMEGA_WORLD_ERR_NOT_FOUND;
    if (handle->object_generation != entry->generation) return OMEGA_WORLD_ERR_STALE_GEN;
    if (handle->object_type != OMEGA_OBJ_CODE) return OMEGA_WORLD_ERR_INVALID_ARG;

    entry->active = false;
    entry->generation++;
    if (world->code_count > 0) world->code_count--;
    return OMEGA_WORLD_OK;
}

int omega_world_resolve_code(OmegaAcceleratorWorld *world,
                              const OmegaHandle *handle,
                              const void **out_cpu,
                              uint64_t *out_gpu_va,
                              size_t *out_size) {
    if (!world || !world->initialized || !handle) return OMEGA_WORLD_ERR_INVALID_ARG;
    if (handle->world_epoch != world->current_epoch) return OMEGA_WORLD_ERR_STALE_EPOCH;
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
    if (handle->world_epoch != world->current_epoch) return OMEGA_WORLD_ERR_STALE_EPOCH;
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
    size_t aligned = (size_bytes + 255ULL) & ~255ULL;
    if (world->scratch.allocated_bytes + aligned > world->scratch.capacity_bytes) {
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
    rm_free_obj(rm, rm->gpfifo, rm->compute_obj);
    rm_free_obj(rm, rm->chgroup, rm->gpfifo);
    rm_free_obj(rm, rm->chgroup, rm->ctxshare);
    rm_free_obj(rm, rm->device, rm->chgroup);

    if (nvrm_channel(rm) != 0) {
        world->channel_active = false;
        return OMEGA_WORLD_ERR_HARDWARE;
    }

    world->channel_generation++;
    world->channel_reconstructions++;
    world->channel_active = true;
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

    /* Resolve code and buffers with internal bounds and permission authority */
    uint64_t code_va = 0;
    size_t code_size = 0;
    if (omega_world_resolve_code(world, code_handle, NULL, &code_va, &code_size) != 0) {
        return OMEGA_WORLD_ERR_FAULT;
    }

    size_t req_bytes = (size_t)n * sizeof(uint32_t);
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
    *world->completion.cpu_marker = 0;
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

    /* Submit to persistent GPFIFO ring */
    if (m16_native_submit_methods(&world->m16, pb, pb_len) != 0) {
        omega_world_scratch_reset(world);
        return OMEGA_WORLD_ERR_HARDWARE;
    }

    if (m16_native_wait_marker(world->completion.cpu_marker, payload, 5000) != 0) {
        omega_world_scratch_reset(world);
        return OMEGA_WORLD_ERR_HARDWARE;
    }

    nvrm_retire(&world->m16.rm, world->m16.rm.put);

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

    uint64_t code_va = 0;
    size_t code_size = 0;
    if (omega_world_resolve_code(world, code_handle, NULL, &code_va, &code_size) != 0) {
        return OMEGA_WORLD_ERR_FAULT;
    }

    size_t elem_size = (spec->precision == OMEGA_MATMUL_PRECISION_INT32) ? 4 : 2;
    size_t out_elem_size = (spec->precision == OMEGA_MATMUL_PRECISION_INT32) ? 4 : 4; /* FP32 accum */

    size_t req_a = (size_t)spec->m * spec->k * elem_size;
    size_t req_b = (size_t)spec->k * spec->n * elem_size;
    size_t req_c = (size_t)spec->m * spec->n * out_elem_size;

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

    void *cbank_cpu = NULL, *qmd0_cpu = NULL, *qmd1_cpu = NULL, *sem_cpu = NULL;
    uint64_t cbank_va = 0, qmd0_va = 0, qmd1_va = 0, sem_va = 0;
    size_t dummy_off = 0;

    if (omega_world_scratch_acquire(world, 0x1000, &cbank_cpu, &cbank_va, &dummy_off) != 0) return OMEGA_WORLD_ERR_NO_MEM;
    if (omega_world_scratch_acquire(world, 0x1000, &qmd0_cpu, &qmd0_va, &dummy_off) != 0) return OMEGA_WORLD_ERR_NO_MEM;
    if (omega_world_scratch_acquire(world, 0x1000, &qmd1_cpu, &qmd1_va, &dummy_off) != 0) return OMEGA_WORLD_ERR_NO_MEM;
    if (omega_world_scratch_acquire(world, 0x1000, &sem_cpu, &sem_va, &dummy_off) != 0) return OMEGA_WORLD_ERR_NO_MEM;

    uint32_t cbank_data[OMEGA_BW_CBANK_DRIVER_WORDS];
    uint32_t threads_x = (spec->precision == OMEGA_MATMUL_PRECISION_INT32) ? 16 : 32;
    uint32_t threads_y = (spec->precision == OMEGA_MATMUL_PRECISION_INT32) ? 16 : 1;
    uint32_t grid_x = (spec->n + threads_x - 1) / threads_x;
    uint32_t grid_y = (spec->m + threads_y - 1) / threads_y;
    if (grid_x == 0) grid_x = 1;
    if (grid_y == 0) grid_y = 1;

    omega_blackwell_build_cbank_driver_2d(cbank_data, cbank_va, threads_x, threads_y, grid_x, grid_y);

    uint32_t cbank_args[OMEGA_BW_CBANK_MATMUL_ARGS_WORDS];
    omega_blackwell_build_cbank_args_matmul(cbank_args, a_va, b_va, c_va, spec->m, spec->k, spec->n);

    memcpy(cbank_cpu, cbank_data, sizeof(cbank_data));
    memcpy((uint8_t *)cbank_cpu + 0x380, cbank_args, sizeof(cbank_args));

    OmegaBlackwellQmdConfig qmd_cfg = {
        .code_va = code_va,
        .cbank_va = cbank_va,
        .scratch_va = cbank_va + 0x2000,
        .sem_va = sem_va,
        .qmd0_va = qmd0_va,
        .qmd1_va = qmd1_va,
        .threads_x = threads_x,
        .threads_y = threads_y,
        .grid_x = grid_x,
        .grid_y = grid_y,
        .gpr_count = (spec->precision == OMEGA_MATMUL_PRECISION_INT32) ? 32 : 64
    };

    uint32_t qmd0_words[OMEGA_BW_QMD_WORDS];
    uint32_t qmd1_words[OMEGA_BW_QMD_WORDS];
    omega_blackwell_build_qmd0(qmd0_words, qmd0_va, qmd1_va);
    omega_blackwell_build_qmd1(qmd1_words, &qmd_cfg);

    memcpy(qmd0_cpu, qmd0_words, sizeof(qmd0_words));
    memcpy(qmd1_cpu, qmd1_words, sizeof(qmd1_words));

    volatile uint32_t *hsem = (volatile uint32_t *)sem_cpu;
    *hsem = 0;
    *world->completion.cpu_marker = 0;
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

    /* Submit to persistent GPFIFO ring */
    if (m16_native_submit_methods(&world->m16, pb, pb_len) != 0) {
        omega_world_scratch_reset(world);
        return OMEGA_WORLD_ERR_HARDWARE;
    }

    if (m16_native_wait_marker(world->completion.cpu_marker, payload, 5000) != 0) {
        omega_world_scratch_reset(world);
        return OMEGA_WORLD_ERR_HARDWARE;
    }

    nvrm_retire(&world->m16.rm, world->m16.rm.put);

    if (out_completion_code) {
        *out_completion_code = payload;
    }
    world->total_dispatches++;

    omega_world_scratch_reset(world);
    return OMEGA_WORLD_OK;
}
