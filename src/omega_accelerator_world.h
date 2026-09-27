#ifndef OMEGA_ACCELERATOR_WORLD_H
#define OMEGA_ACCELERATOR_WORLD_H

#include "omega_types.h"
#include "omega_vector.h"
#include "omega_blackwell_realize.h"
#include "omega_blackwell_matmul.h"
#include "m16_native.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OMEGA_WORLD_MAX_BUFFERS        128
#define OMEGA_WORLD_MAX_CODE_ENTRIES    64
#define OMEGA_WORLD_SCRATCH_SIZE       0x200000 /* 2 MiB persistent scratch */
#define OMEGA_WORLD_MAX_PB_WORDS       4096
#define OMEGA_WORLD_MAX_IN_FLIGHT      256

/* Object Types */
#define OMEGA_OBJ_NONE                 0x00000000
#define OMEGA_OBJ_BUFFER               0x00000001
#define OMEGA_OBJ_CODE                 0x00000002

/* Permission Flags */
#define OMEGA_PERM_READ                0x00000001
#define OMEGA_PERM_WRITE               0x00000002
#define OMEGA_PERM_EXECUTE             0x00000004

/* Error Codes */
#define OMEGA_WORLD_OK                  0
#define OMEGA_WORLD_ERR_INVALID_ARG    -1
#define OMEGA_WORLD_ERR_NO_MEM         -2
#define OMEGA_WORLD_ERR_STALE_EPOCH    -3
#define OMEGA_WORLD_ERR_STALE_GEN      -4
#define OMEGA_WORLD_ERR_PERM_DENIED    -5
#define OMEGA_WORLD_ERR_BOUNDS         -6
#define OMEGA_WORLD_ERR_NOT_FOUND      -7
#define OMEGA_WORLD_ERR_HARDWARE       -8
#define OMEGA_WORLD_ERR_FAULT          -9
/* The world observed a hardware or accounting failure it could not resolve.
 * Every submission path refuses until recover_channel_fault or rebuild. */
#define OMEGA_WORLD_ERR_FAULTED       -10

/* External Capability Handle */
typedef struct {
    uint32_t world_epoch;
    uint32_t object_id;
    uint32_t object_generation;
    uint32_t object_type;
    uint32_t permissions;
} OmegaHandle;

/* Internal Buffer Registry Entry */
typedef struct {
    uint32_t object_id;
    uint32_t generation;
    uint32_t permissions;
    bool active;
    /* Revoked while an in-flight submission still referenced it: the handle
     * is already refused, but the memory is released only after drain. */
    bool retiring;
    size_t size_bytes;
    void *cpu_addr;
    uint64_t gpu_va;
    NvrmMem mem;
} OmegaBufferEntry;

/* Internal Code Registry Entry */
typedef struct {
    uint32_t object_id;
    uint32_t generation;
    bool active;
    bool retiring;
    size_t size_bytes;
    void *cpu_addr;
    uint64_t gpu_va;
    uint8_t code_digest[32];
    uint8_t realization_id[32];
    NvrmMem mem;
} OmegaCodeEntry;

/* Scratch Arena Allocation */
typedef struct {
    NvrmMem mem;
    size_t capacity_bytes;
    size_t allocated_bytes;
    uint32_t active_slices;
} OmegaScratchArena;

/* Persistent Completion Marker */
typedef struct {
    NvrmMem mem;
    volatile uint32_t *cpu_marker;
    uint64_t gpu_va;
    uint32_t last_payload;
} OmegaCompletionTracker;

/* Semantic descriptor for one queued dispatch. Captured at submission time and
 * folded into the rolling digest only after completion is observed, so digest
 * provenance is bound to real code/realization/input/result identities. */
typedef struct {
    uint32_t semantic_words[5];
    uint32_t code_object_id;
    uint32_t code_generation;
    uint32_t code_permissions;
    uint32_t a_object_id;
    uint32_t a_generation;
    uint32_t a_permissions;
    uint32_t b_object_id;
    uint32_t b_generation;
    uint32_t b_permissions;
    uint32_t c_object_id;
    uint32_t c_generation;
    uint32_t c_permissions;
    uint32_t a_bytes;
    uint32_t b_bytes;
    uint32_t c_bytes;
    uint32_t completion_val;
    /* GPFIFO put count immediately after this submission was enqueued.
     * This is queue accounting used for retirement and is deliberately
     * distinct from completion_val (hardware-completion identity). The
     * world fills it in omega_world_submit; callers must leave it zero. */
    uint32_t gp_seq;
    uint32_t channel_generation;
    uint8_t semantic_id[32];
    uint8_t input_bindings_digest[32];
    bool output_cpu_protected;
    bool committed;
} OmegaWorldSubmission;

/* Omega Accelerator World Context */
typedef struct {
    uint32_t current_epoch;
    uint32_t handle_epoch; /* unique across destroy/init, never reused */
    uint32_t channel_generation;
    bool initialized;
    /* Latched on an unresolved hardware/accounting failure; cleared only by
     * omega_world_recover_channel_fault or omega_world_rebuild. */
    bool faulted;

    /* Persistent hardware context */
    M16NativeContext m16;
    bool channel_active;

    /* Live queue ring topology */
    uint32_t ring_capacity;
    uint32_t put_count;
    uint32_t retired_count;

    /* Registries */
    OmegaBufferEntry buffers[OMEGA_WORLD_MAX_BUFFERS];
    uint32_t buffer_count;

    OmegaCodeEntry code_entries[OMEGA_WORLD_MAX_CODE_ENTRIES];
    uint32_t code_count;

    /* Persistent scratch arena & completion */
    OmegaScratchArena scratch;
    OmegaCompletionTracker completion;

    /* Batched submissions awaiting observed completion */
    OmegaWorldSubmission in_flight[OMEGA_WORLD_MAX_IN_FLIGHT];
    uint32_t in_flight_count;

    /* Pushbuffer allocation */
    NvrmMem pb_mem;

    /* Rolling state digest */
    uint64_t sequence_number;
    uint8_t rolling_state_digest[32];

    /* Statistics */
    uint64_t total_dispatches;
    uint64_t channel_reconstructions;
    /* Completed submissions that were not committed because an object they
     * bound was revoked while they were in flight, or lost to a channel
     * recovery. Never folded into total_dispatches or the digest. */
    uint64_t abandoned_dispatches;
} OmegaAcceleratorWorld;

/* World Lifecycle */
int  omega_world_init(OmegaAcceleratorWorld *world);
int  omega_world_destroy(OmegaAcceleratorWorld *world);
int  omega_world_rebuild(OmegaAcceleratorWorld *world);

/* Buffer Registry Operations */
int  omega_world_register_buffer(OmegaAcceleratorWorld *world,
                                 size_t size_bytes,
                                 uint32_t permissions,
                                 OmegaHandle *out_handle);
int  omega_world_revoke_buffer(OmegaAcceleratorWorld *world,
                               const OmegaHandle *handle);
int  omega_world_resolve_buffer(OmegaAcceleratorWorld *world,
                                const OmegaHandle *handle,
                                uint32_t required_perms,
                                size_t req_offset,
                                size_t req_size,
                                void **out_cpu,
                                uint64_t *out_gpu_va);

/* Code Registry Operations */
int  omega_world_register_code(OmegaAcceleratorWorld *world,
                               const void *code_bytes,
                               size_t code_size,
                               const uint8_t realization_id[32],
                               OmegaHandle *out_handle);
int  omega_world_revoke_code(OmegaAcceleratorWorld *world,
                             const OmegaHandle *handle);
int  omega_world_resolve_code(OmegaAcceleratorWorld *world,
                              const OmegaHandle *handle,
                              const void **out_cpu,
                              uint64_t *out_gpu_va,
                              size_t *out_size);

/* Handle Validation */
int  omega_world_validate_handle(const OmegaAcceleratorWorld *world,
                                 const OmegaHandle *handle,
                                 uint32_t expected_type,
                                 uint32_t required_perms);

/* Scratch Arena */
int  omega_world_scratch_acquire(OmegaAcceleratorWorld *world,
                                 size_t size_bytes,
                                 void **out_cpu,
                                 uint64_t *out_gpu_va,
                                 size_t *out_offset);
void omega_world_scratch_reset(OmegaAcceleratorWorld *world);

/* Hardware Ring Topology */
int  omega_world_discover_ring_capacity(OmegaAcceleratorWorld *world,
                                        uint32_t *out_capacity);

/* Persistent Execution Operations */
int  omega_world_dispatch_vector(OmegaAcceleratorWorld *world,
                                 const OmegaHandle *code_handle,
                                 const OmegaHandle *h_a,
                                 const OmegaHandle *h_b,
                                 const OmegaHandle *h_c,
                                 uint32_t n,
                                 uint32_t *out_completion_code);

int  omega_world_dispatch_matmul(OmegaAcceleratorWorld *world,
                                 const OmegaMatMulSpec *spec,
                                 const OmegaHandle *code_handle,
                                 const OmegaHandle *h_a,
                                 const OmegaHandle *h_b,
                                 const OmegaHandle *h_c,
                                 uint32_t *out_completion_code);

/* Batched persistent submission. Enqueues a prebuilt pushbuffer whose terminal
 * semaphore release carries submission->completion_val. The world owns the
 * authoritative sequence/dispatch accounting: counters and the rolling digest
 * advance only when completion is observed, never at enqueue time. */
int  omega_world_submit(OmegaAcceleratorWorld *world,
                        const NvrmMem *pb_mem,
                        uint32_t off_bytes,
                        uint32_t nwords,
                        const OmegaWorldSubmission *submission);

/* Ring the doorbell after a run of submissions. */
int  omega_world_ring(OmegaAcceleratorWorld *world);

/* Block until the completion marker reaches upto_payload, retire the GPFIFO
 * entries, and commit every in-flight dispatch whose completion_val <=
 * upto_payload by advancing total_dispatches and folding the dispatch into the
 * rolling digest. Returns the number committed, or a negative error code. */
int  omega_world_drain(OmegaAcceleratorWorld *world,
                       uint32_t upto_payload,
                       int timeout_ms);

/* Channel Fault Injection and Recovery */
int  omega_world_recover_channel_fault(OmegaAcceleratorWorld *world);

/* Rolling State Digest */
int  omega_world_update_digest(OmegaAcceleratorWorld *world,
                               const uint8_t *semantic_id,
                               const uint8_t *realization_id,
                               const uint8_t *code_digest,
                               const uint8_t *input_bindings_digest,
                               const uint32_t *obj_id_gen_pairs,
                               uint32_t obj_count,
                               uint32_t capability_scope,
                               uint32_t completion_val,
                               const uint8_t *result_digest,
                               uint32_t outcome_code);

#endif /* OMEGA_ACCELERATOR_WORLD_H */
