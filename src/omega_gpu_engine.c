/*
 * Omega GPU Engine, A2 compile stub. See src/omega_gpu_engine.h and
 * docs/numeric/OMEGA_GPU_ENGINE.md. No launch logic yet: omega_gpu_engine_run
 * validates its arguments, then reports DEVICE_ERROR at step "not_implemented".
 * The real engine core (A3) replaces omega_gpu_engine_run; the name functions
 * below are final.
 */
#include "omega_gpu_engine.h"

#include <string.h>

static const char *const FAILURE_NAMES[OMEGA_GPU_ENGINE_FAILURE_COUNT] = {
    "OK",
    "DEVICE_OPEN",
    "DEVICE_ALLOC",
    "DEVICE_SUBMIT",
    "GPU_COMPLETION_TIMEOUT",
    "OUTPUT_NOT_VISIBLE",
    "OUTPUT_NOT_WRITTEN",
    "DEVICE_ERROR",
};

static const char *const STATE_NAMES[OMEGA_GPU_ENGINE_STATE_COUNT] = {
    "NONE",
    "PREPARED",
    "SUBMITTED",
    "GPU_COMPLETE",
    "OUTPUT_VISIBLE",
    "OUTPUT_PRODUCED",
    "COMMITTED",
};

static const char *const WAIT_NAMES[OMEGA_GPU_ENGINE_WAIT_COUNT] = {
    "NONE",
    "MARKER",
    "SEMAPHORE",
    "MARKER2",
};

const char *omega_gpu_engine_failure_name(int failure)
{
    if (failure < 0 || failure >= (int)OMEGA_GPU_ENGINE_FAILURE_COUNT) return "UNKNOWN";
    return FAILURE_NAMES[failure];
}

const char *omega_gpu_engine_state_name(int state)
{
    if (state < 0 || state >= (int)OMEGA_GPU_ENGINE_STATE_COUNT) return "UNKNOWN";
    return STATE_NAMES[state];
}

const char *omega_gpu_engine_wait_name(int wait)
{
    if (wait < 0 || wait >= (int)OMEGA_GPU_ENGINE_WAIT_COUNT) return "UNKNOWN";
    return WAIT_NAMES[wait];
}

static int backend_complete(const OmegaGpuEngineBackend *b)
{
    return b->open && b->alloc && b->copy_in && b->poison_output && b->build_and_submit &&
           b->wait_marker && b->wait_marker2 && b->wait_semaphore && b->flush_barrier &&
           b->copy_out && b->free_buf && b->close;
}

static int request_valid(const OmegaGpuEngineRequest *rq)
{
    if (!rq->kernel_image || rq->kernel_len == 0) return 0;
    if (rq->n_inputs > OMEGA_GPU_ENGINE_MAX_BUFFERS) return 0;
    if (rq->n_outputs == 0 || rq->n_outputs > OMEGA_GPU_ENGINE_MAX_BUFFERS) return 0;
    if (rq->n_inputs > 0 && !rq->inputs) return 0;
    if (!rq->outputs) return 0;
    return 1;
}

int omega_gpu_engine_run(const OmegaGpuEngineBackend *b, void *backend_ctx,
                         const OmegaGpuEngineRequest *rq, OmegaGpuEngineResult *out)
{
    (void)backend_ctx;
    if (!out) return OMEGA_GPU_ENGINE_DEVICE_ERROR;
    memset(out, 0, sizeof *out);
    out->state = OMEGA_GPU_ENGINE_STATE_NONE;
    out->failure = OMEGA_GPU_ENGINE_DEVICE_ERROR;
    out->wait = OMEGA_GPU_ENGINE_WAIT_NONE;
    if (!b || !rq || !backend_complete(b) || !request_valid(rq)) {
        out->step = "bad_request";
        return OMEGA_GPU_ENGINE_DEVICE_ERROR;
    }
    out->step = "not_implemented";
    return OMEGA_GPU_ENGINE_DEVICE_ERROR;
}
