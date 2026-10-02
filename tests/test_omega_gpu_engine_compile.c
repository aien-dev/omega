/* Omega GPU Engine A2: the stub returns its defined results and every enum name is distinct. */
#include "omega_gpu_engine.h"
#include <stdio.h>
#include <string.h>

static int bad;
#define CHECK(c) do { if (!(c)) { printf("[FAIL] line %d: %s\n", __LINE__, #c); bad++; } } while (0)

static int f_ctx(void *c) { (void)c; return 0; }
static int f_alloc(void *c, int r, unsigned i, size_t n) { (void)c; (void)r; (void)i; (void)n; return 0; }
static int f_in(void *c, int r, unsigned i, const void *s, size_t n) { (void)c; (void)r; (void)i; (void)s; (void)n; return 0; }
static int f_poison(void *c, unsigned i, uint32_t w, size_t n) { (void)c; (void)i; (void)w; (void)n; return 0; }
static int f_submit(void *c, const OmegaGpuEngineRequest *rq) { (void)c; (void)rq; return 0; }
static int f_wait(void *c, uint64_t ms) { (void)c; (void)ms; return 0; }
static int f_out(void *c, unsigned i, void *d, size_t n) { (void)c; (void)i; (void)d; (void)n; return 0; }
static int f_free(void *c, int r, unsigned i) { (void)c; (void)r; (void)i; return 0; }

static int all_distinct(const char *(*name)(int), int count)
{
    for (int i = 0; i < count; i++) {
        CHECK(name(i) != NULL && strcmp(name(i), "UNKNOWN") != 0);
        for (int j = i + 1; j < count; j++) CHECK(strcmp(name(i), name(j)) != 0);
    }
    return strcmp(name(-1), "UNKNOWN") == 0 && strcmp(name(count), "UNKNOWN") == 0;
}

int main(void)
{
    CHECK(all_distinct(omega_gpu_engine_failure_name, OMEGA_GPU_ENGINE_FAILURE_COUNT));
    CHECK(all_distinct(omega_gpu_engine_state_name, OMEGA_GPU_ENGINE_STATE_COUNT));
    CHECK(all_distinct(omega_gpu_engine_wait_name, OMEGA_GPU_ENGINE_WAIT_COUNT));
    CHECK(OMEGA_GPU_ENGINE_OK == 0 && strcmp(omega_gpu_engine_failure_name(0), "OK") == 0);
    CHECK(strcmp(omega_gpu_engine_failure_name(OMEGA_GPU_ENGINE_GPU_COMPLETION_TIMEOUT), "GPU_COMPLETION_TIMEOUT") == 0);

    OmegaGpuEngineBackend be = { f_ctx, f_alloc, f_in, f_poison, f_submit, f_wait, f_wait, f_wait, f_ctx, f_out, f_free, f_ctx };
    unsigned char code[4] = {0}; uint32_t outw[4] = {0};
    OmegaGpuEngineOutBuf ob = { outw, sizeof outw, 0 };
    OmegaGpuEngineRequest rq; memset(&rq, 0, sizeof rq);
    rq.kernel_image = code; rq.kernel_len = sizeof code; rq.outputs = &ob; rq.n_outputs = 1;

    OmegaGpuEngineResult r; memset(&r, 0x7f, sizeof r);
    CHECK(omega_gpu_engine_run(&be, NULL, &rq, &r) == OMEGA_GPU_ENGINE_DEVICE_ERROR);
    CHECK(r.failure == OMEGA_GPU_ENGINE_DEVICE_ERROR && r.state == OMEGA_GPU_ENGINE_STATE_NONE);
    CHECK(r.step != NULL && strcmp(r.step, "not_implemented") == 0);
    CHECK(r.wait == OMEGA_GPU_ENGINE_WAIT_NONE && r.waited_ms == 0 && r.drv_rc == 0 && r.n_outputs == 0);

    memset(&r, 0x7f, sizeof r);
    rq.kernel_image = NULL; /* counterexample: a bad request must not look like not_implemented */
    CHECK(omega_gpu_engine_run(&be, NULL, &rq, &r) == OMEGA_GPU_ENGINE_DEVICE_ERROR);
    CHECK(r.step != NULL && strcmp(r.step, "bad_request") == 0);
    rq.kernel_image = code;
    memset(&r, 0x7f, sizeof r);
    be.close = NULL; /* an incomplete backend table is refused too */
    CHECK(omega_gpu_engine_run(&be, NULL, &rq, &r) == OMEGA_GPU_ENGINE_DEVICE_ERROR);
    CHECK(r.step != NULL && strcmp(r.step, "bad_request") == 0);
    CHECK(omega_gpu_engine_run(&be, NULL, &rq, NULL) == OMEGA_GPU_ENGINE_DEVICE_ERROR);

    printf("SUMMARY failed=%d\n", bad);
    return bad ? 1 : 0;
}
