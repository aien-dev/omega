/*
 * Omega GPU Engine: launch one already-built GPU kernel and return SUCCESS or
 * exactly one named failure.
 *
 * Purpose: today five launcher files repeat the same open / alloc / copy-in /
 * pushbuffer / submit / completion-wait / readback / close sequence with three
 * different error styles. This module owns that sequence once. Design:
 * docs/numeric/OMEGA_GPU_ENGINE.md.
 *
 * Contract: the caller gives a kernel image, an opaque launch descriptor,
 * input buffers, output buffers and per-step timeouts. The engine walks the
 * states NONE -> PREPARED -> SUBMITTED -> GPU_COMPLETE -> OUTPUT_VISIBLE ->
 * OUTPUT_PRODUCED -> COMMITTED. Success is returned only at COMMITTED. Every
 * output is poison-filled before submit and must hold non-poison words before
 * OUTPUT_PRODUCED. The engine never judges whether the math is right: it
 * promises only "faithfully launched, GPU completed, these bytes are
 * genuinely its output".
 *
 * Return value of omega_gpu_engine_run: 0 (OMEGA_GPU_ENGINE_OK) on success,
 * otherwise the failure code, which is also stored in the result.
 *
 * STATUS: A2 stub. omega_gpu_engine_run does not launch anything yet; it
 * validates its arguments and reports DEVICE_ERROR at step "not_implemented".
 */
#ifndef OMEGA_GPU_ENGINE_H
#define OMEGA_GPU_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#define OMEGA_GPU_ENGINE_MAX_BUFFERS 8u
#define OMEGA_GPU_ENGINE_POISON_DEFAULT 0x55555555u

/* Request flags. A zeroed request means: poison ON, C3 protection ON. */
#define OMEGA_GPU_ENGINE_FLAG_NO_POISON 0x1u /* skip poison fill and unwritten scan */
#define OMEGA_GPU_ENGINE_FLAG_NO_C3 0x2u     /* A/B control arm only: skip L2 flush and marker2 */

/* One failure name per run. OK is 0, so the return value reads as a boolean. */
typedef enum {
    OMEGA_GPU_ENGINE_OK = 0,
    OMEGA_GPU_ENGINE_DEVICE_OPEN,
    OMEGA_GPU_ENGINE_DEVICE_ALLOC,
    OMEGA_GPU_ENGINE_DEVICE_SUBMIT,
    OMEGA_GPU_ENGINE_GPU_COMPLETION_TIMEOUT,
    OMEGA_GPU_ENGINE_OUTPUT_NOT_VISIBLE,
    OMEGA_GPU_ENGINE_OUTPUT_NOT_WRITTEN,
    OMEGA_GPU_ENGINE_DEVICE_ERROR,
    OMEGA_GPU_ENGINE_FAILURE_COUNT /* not a failure: number of names above */
} OmegaGpuEngineFailure;

/* How far a run got. Success is only COMMITTED. */
typedef enum {
    OMEGA_GPU_ENGINE_STATE_NONE = 0, /* nothing done yet (device may be unopened) */
    OMEGA_GPU_ENGINE_STATE_PREPARED,
    OMEGA_GPU_ENGINE_STATE_SUBMITTED,
    OMEGA_GPU_ENGINE_STATE_GPU_COMPLETE,
    OMEGA_GPU_ENGINE_STATE_OUTPUT_VISIBLE,
    OMEGA_GPU_ENGINE_STATE_OUTPUT_PRODUCED,
    OMEGA_GPU_ENGINE_STATE_COMMITTED,
    OMEGA_GPU_ENGINE_STATE_COUNT /* not a state: number of names above */
} OmegaGpuEngineState;

/* Which completion wait timed out (only meaningful for GPU_COMPLETION_TIMEOUT). */
typedef enum {
    OMEGA_GPU_ENGINE_WAIT_NONE = 0,
    OMEGA_GPU_ENGINE_WAIT_MARKER,
    OMEGA_GPU_ENGINE_WAIT_SEMAPHORE,
    OMEGA_GPU_ENGINE_WAIT_MARKER2,
    OMEGA_GPU_ENGINE_WAIT_COUNT /* not a wait: number of names above */
} OmegaGpuEngineWait;

/* What a device buffer is for. SCRATCH is backend-internal (pushbuffer, completion
 * marker, QMD); the engine asks for it once so its allocation failure is named. */
typedef enum {
    OMEGA_GPU_ENGINE_BUF_SCRATCH = 0,
    OMEGA_GPU_ENGINE_BUF_KERNEL,
    OMEGA_GPU_ENGINE_BUF_LAUNCH,
    OMEGA_GPU_ENGINE_BUF_INPUT,
    OMEGA_GPU_ENGINE_BUF_OUTPUT
} OmegaGpuEngineBufRole;

/* A wait callback returns 0 when satisfied, this value on timeout, any other
 * nonzero value on a driver fault (reported as DEVICE_ERROR, value kept as drv_rc). */
#define OMEGA_GPU_ENGINE_BACKEND_TIMEOUT 1

typedef struct {
    const void *ptr;
    size_t len; /* bytes */
} OmegaGpuEngineInBuf;

typedef struct {
    void *ptr;      /* host destination, must be 4-byte aligned when poison is on */
    size_t len;     /* bytes, multiple of 4 when poison is on */
    uint32_t poison; /* 0 means OMEGA_GPU_ENGINE_POISON_DEFAULT */
} OmegaGpuEngineOutBuf;

/* Per-step timeouts in milliseconds. Never hard-coded by the engine. Each wait
 * that is enabled needs a value greater than 0 (marker2_ms is ignored with NO_C3). */
typedef struct {
    uint64_t marker_ms;
    uint64_t semaphore_ms;
    uint64_t marker2_ms;
} OmegaGpuEngineTimeouts;

typedef struct {
    const void *kernel_image; /* already-built kernel code */
    size_t kernel_len;
    const void *launch;       /* opaque launch descriptor, passed through untouched */
    size_t launch_len;
    const OmegaGpuEngineInBuf *inputs;
    size_t n_inputs;          /* at most OMEGA_GPU_ENGINE_MAX_BUFFERS */
    const OmegaGpuEngineOutBuf *outputs;
    size_t n_outputs;         /* 1 .. OMEGA_GPU_ENGINE_MAX_BUFFERS */
    OmegaGpuEngineTimeouts timeouts;
    uint32_t flags;           /* OMEGA_GPU_ENGINE_FLAG_* */
} OmegaGpuEngineRequest;

typedef struct {
    int state;                /* last OmegaGpuEngineState reached */
    int failure;              /* OmegaGpuEngineFailure, OK on success */
    const char *step;         /* static string naming the failing step, "ok" on success */
    int wait;                 /* OmegaGpuEngineWait, NONE unless a completion wait failed */
    uint64_t wait_timeout_ms; /* the configured limit of that wait */
    uint64_t waited_ms;       /* how long the engine actually waited before giving up */
    int drv_rc;               /* driver return code of the failing call, 0 if none */
    int err_no;               /* errno captured right after the failing call, 0 if none */
    size_t n_outputs;         /* number of valid entries in unwritten_bytes */
    uint64_t unwritten_bytes[OMEGA_GPU_ENGINE_MAX_BUFFERS]; /* poison bytes left, per output */
} OmegaGpuEngineResult;

/*
 * Backend seam. Every callback returns 0 on success and nonzero on failure,
 * except where noted. A fake backend implements these to drive every
 * transition in host tests; the Blackwell GB10 backend wraps the m16_native_*
 * calls the launchers use today (physics/m16/m16_native.h). All callbacks must
 * be non-NULL.
 */
typedef struct {
    int (*open)(void *ctx);                                  /* device + channel */
    int (*alloc)(void *ctx, int role, unsigned index, size_t bytes);
    int (*copy_in)(void *ctx, int role, unsigned index, const void *src, size_t bytes);
    int (*poison_output)(void *ctx, unsigned index, uint32_t word, size_t bytes); /* device side */
    int (*build_and_submit)(void *ctx, const OmegaGpuEngineRequest *rq);
    int (*wait_marker)(void *ctx, uint64_t timeout_ms);      /* may return BACKEND_TIMEOUT */
    int (*wait_marker2)(void *ctx, uint64_t timeout_ms);     /* may return BACKEND_TIMEOUT */
    int (*wait_semaphore)(void *ctx, uint64_t timeout_ms);   /* may return BACKEND_TIMEOUT */
    int (*flush_barrier)(void *ctx);                         /* dsb sy before readback */
    int (*copy_out)(void *ctx, unsigned index, void *dst, size_t bytes);
    int (*free_buf)(void *ctx, int role, unsigned index);
    int (*close)(void *ctx);
} OmegaGpuEngineBackend;

/* Runs one kernel. Returns OMEGA_GPU_ENGINE_OK (0) or the failure code; fills *out. */
int omega_gpu_engine_run(const OmegaGpuEngineBackend *b, void *backend_ctx,
                         const OmegaGpuEngineRequest *rq, OmegaGpuEngineResult *out);

/* Static name strings, never NULL. Out-of-range values give "UNKNOWN". */
const char *omega_gpu_engine_failure_name(int failure);
const char *omega_gpu_engine_state_name(int state);
const char *omega_gpu_engine_wait_name(int wait);

#endif
