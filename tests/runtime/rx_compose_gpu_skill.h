/*
 * rx_compose_gpu_skill.h -- the COMPOSITION-2 gate's two Skills executed on
 * the GB10 (GPU tier). Same contract and same faults as the host Skills in
 * rx_compose_fixture.h, but the arithmetic runs on the chip.
 *
 * A Skill is the affine map y = w0 * x + w1 with its own weights: Skill A
 * (3, 1), or (3, 2) when made to break the contract; Skill B (3, 1), or
 * (3, 6). Each call builds one INT32 matrix product C = A * B with the
 * synthesized sm_121 kernel and submits it through the sovereign M16 native
 * path (omega_blackwell_execute_matmul: nvrm channel, no CUDA): A is 16x2
 * with row 0 = [x, 1], B is 2x16 with column 0 = [w0, w1], every other entry
 * zero, so every chip thread stays inside the arrays. The Skill result is
 * C[0][0] read back from chip memory. The host reference product is used
 * only to check the chip (a mismatch fails the call); it is never returned.
 *
 * A call that cannot reach the chip, or whose input or result does not fit
 * the exact 32-bit product, reports failed and returns 0: the gate then
 * fails, it never falls back to host arithmetic.
 */
#ifndef RX_COMPOSE_GPU_SKILL_H
#define RX_COMPOSE_GPU_SKILL_H

#include "rx_compose_fixture.h"
#include "omega_blackwell_matmul.h"
#include "omega_blackwell_submit.h"

#define GPU_SK_DIM 16u
#define GPU_SK_K 2u

typedef struct {
    uint32_t runs[2];             /* chip executions per Skill (A, B) */
    uint32_t failures;            /* chip calls that did not return a checked result */
    uint32_t out_of_range;        /* inputs refused before the chip */
    uint32_t last_marker, last_semaphore;
    uint64_t elapsed_ns;          /* sum over chip executions */
    int zero_libcuda_linkage, zero_cuda_symbols, zero_libcuda_runtime;
    int have_kernel;
    uint8_t kernel_digest[32];
    size_t kernel_insns;
    OmegaBlackwellKernel kernel;
    OmegaMatMulSpec spec;
} GpuSkillLedger;

static GpuSkillLedger g_gpu;

static __attribute__((unused)) int gpu_sk_init(void) {
    memset(&g_gpu, 0, sizeof g_gpu);
    g_gpu.zero_libcuda_linkage = g_gpu.zero_cuda_symbols = g_gpu.zero_libcuda_runtime = 1;
    if (omega_matmul_spec_init(&g_gpu.spec, GPU_SK_DIM, GPU_SK_K, GPU_SK_DIM,
                               OMEGA_MATMUL_PRECISION_INT32) != 0)
        return -1;
    if (omega_blackwell_codegen_matmul(&g_gpu.spec, &g_gpu.kernel) != 0) return -2;
    memcpy(g_gpu.kernel_digest, g_gpu.kernel.code_digest, 32);
    g_gpu.kernel_insns = g_gpu.kernel.insn_count;
    g_gpu.have_kernel = 1;
    return 0;
}

static __attribute__((unused)) void gpu_sk_free(void) {
    if (g_gpu.have_kernel) omega_blackwell_kernel_free(&g_gpu.kernel);
    g_gpu.have_kernel = 0;
}

static uint64_t gpu_sk_affine(int which, uint64_t x, uint32_t w0, uint32_t w1, int *failed) {
    *failed = 1;
    if (!g_gpu.have_kernel) { g_gpu.failures++; return 0; }
    if (x > (UINT32_MAX - w1) / w0) { g_gpu.out_of_range++; return 0; }
    uint32_t a[GPU_SK_DIM * GPU_SK_K], b[GPU_SK_K * GPU_SK_DIM], c[GPU_SK_DIM * GPU_SK_DIM];
    memset(a, 0, sizeof a);
    memset(b, 0, sizeof b);
    a[0] = (uint32_t)x;
    a[1] = 1;
    b[0] = w0;                    /* B[0][0] */
    b[GPU_SK_DIM] = w1;           /* B[1][0] */
    OmegaBlackwellMatMulExecution ex;
    memset(&ex, 0, sizeof ex);
    int rc = omega_blackwell_execute_matmul(&g_gpu.spec, &g_gpu.kernel, a, b, c, &ex);
    g_gpu.last_marker = ex.completion_marker;
    g_gpu.last_semaphore = ex.intermediate_semaphore;
    if (!ex.zero_libcuda_linkage) g_gpu.zero_libcuda_linkage = 0;
    if (!ex.zero_cuda_symbols) g_gpu.zero_cuda_symbols = 0;
    if (!ex.zero_libcuda_runtime) g_gpu.zero_libcuda_runtime = 0;
    if (rc != 0 || !ex.parity_verified || ex.completion_marker != OMEGA_BW_MARKER_COMPLETION_PAYLOAD ||
        ex.intermediate_semaphore != OMEGA_BW_SEMAPHORE_INTERMEDIATE_DONE) {
        g_gpu.failures++;
        return 0;
    }
    g_gpu.runs[which]++;
    g_gpu.elapsed_ns += ex.elapsed_ns;
    *failed = 0;
    return c[0];
}

static __attribute__((unused)) uint64_t gpu_sk_a(const uint64_t *in, uint32_t n, uint32_t attempt, int *failed) {
    (void)attempt;
    return gpu_sk_affine(0, n ? in[0] : 0, 3, fx_a_bad ? 2 : 1, failed);
}
static __attribute__((unused)) uint64_t gpu_sk_b(const uint64_t *in, uint32_t n, uint32_t attempt, int *failed) {
    (void)attempt;
    return gpu_sk_affine(1, n ? in[0] : 0, 3, fx_b_bad ? 6 : 1, failed);
}

#endif
