/* CPU realization of M20 OMEGA_TENSOR: the E1 CPU tier (explicit AArch64
 * instructions in omega_numeric_cpu_realize) for elementwise ops, the frozen
 * E1 transcendental sequences (omega_numeric_transc.c, bounded contract) for
 * EXP2 / LOG2 / SIGMOID / TANH, the E1 polynomials omega_math_exp / omega_math_log
 * (omega_numeric.c) for EXP / LOG, and the reduction seam for reductions. No
 * arithmetic outside those. RELU is a bit select in omega_tensor.c. */
#include "omega_tensor.h"
#include "omega_tensor_reduce_seam.h"
#include "omega_numeric_transc.h"

static int cpu_elementwise(OmegaNumericOp op, const float *a, const float *b, const float *c,
                           float *out, size_t n) {
    return omega_numeric_cpu_realize(op, a, b, c, out, n);
}

/* One E1 scalar call per element; nothing else touches the value. */
static float (*const TRANSC_FN[OMEGA_TU_COUNT])(float) = {
    [OMEGA_TU_EXP2] = omega_math_exp2, [OMEGA_TU_LOG2] = omega_math_log2, /* MUT:TRANSC_EXP2_LOG2_SWAP */
    [OMEGA_TU_SIGMOID] = omega_math_sigmoid,
    [OMEGA_TU_TANH] = omega_math_tanh,
    /* E1 Omega-defined polynomials, src/omega_numeric.c:458 and :501 (cut ops) */
    [OMEGA_TU_EXP] = omega_math_exp, [OMEGA_TU_LOG] = omega_math_log, /* MUT:TRANSC_EXP_LOG_SWAP */
};

static int cpu_transc(OmegaTensorUnaryOp op, const float *a, float *out, size_t n) {
    if ((unsigned)op >= OMEGA_TU_COUNT || !TRANSC_FN[op] || !a || !out) return OMEGA_NUMERIC_ERR_BAD_ARGS;
    if (!omega_numeric_fpenv_ok()) return OMEGA_NUMERIC_ERR_FPENV;
    float (*f)(float) = TRANSC_FN[op];
    for (size_t i = 0; i < n; i++) out[i] = f(a[i]);
    return OMEGA_NUMERIC_OK;
}

static const OmegaTensorRealization CPU_REALIZATION = {
    .name = "CPU_E1_TIER",
    .reduce_order = OMEGA_TENSOR_REDUCE_DECLARED_ORDER,
    .elementwise = cpu_elementwise,
    .reduce = omega_tensor_seam_reduce_cpu,
    .transc = cpu_transc,
};

const OmegaTensorRealization *omega_tensor_cpu_realization(void) { return &CPU_REALIZATION; }
