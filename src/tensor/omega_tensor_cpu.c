/* CPU realization of M20 OMEGA_TENSOR: the E1 CPU tier (explicit AArch64
 * instructions in omega_numeric_cpu_realize) for elementwise ops, the frozen
 * E1 transcendental sequences (omega_numeric_transc.c, bounded contract) for
 * EXP2 / LOG2 / SIGMOID / TANH / SIN / COS, the E1 polynomials omega_math_exp /
 * omega_math_log (omega_numeric.c) for EXP / LOG, the E1 FCMP conditions for the
 * CR-3 mask compare, and the reduction seam for reductions. No arithmetic outside
 * those. RELU is a bit select in omega_tensor.c. */
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
    [OMEGA_TU_RSQRT] = omega_math_rsqrt, [OMEGA_TU_ERF] = omega_math_erf, /* MUT:TRANSC_RSQRT_ERF_SWAP */
    [OMEGA_TU_GELU] = omega_math_gelu,
    [OMEGA_TU_SIN] = omega_math_sin, [OMEGA_TU_COS] = omega_math_cos, /* MUT:TRIG_SIN_COS_SWAP */
};

static int cpu_transc(OmegaTensorUnaryOp op, const float *a, float *out, size_t n) {
    if ((unsigned)op >= OMEGA_TU_COUNT || !TRANSC_FN[op] || !a || !out) return OMEGA_NUMERIC_ERR_BAD_ARGS;
    if (!omega_numeric_fpenv_ok()) return OMEGA_NUMERIC_ERR_FPENV;
    float (*f)(float) = TRANSC_FN[op];
    for (size_t i = 0; i < n; i++) out[i] = f(a[i]); /* MUT:TRIG_DOMAIN_NUMBER */
    return OMEGA_NUMERIC_OK;
}


/*
 * CR-3 mask compare. Same FCMP + condition per predicate as the E1 CPU
 * compare-select ops (src/omega_numeric.c, CPU_SEL table after the
 * "E1 compare-and-select" comment), but FCSEL picks between the constants
 * +1.0 and +0.0 instead of between a and b. FCMP flags: less 1000, equal
 * 0110, greater 0010, unordered 0011 (NZCV), so every ordered predicate is
 * false on NaN and -0 == +0. NE and EQU chain two FCSELs, as in E1.
 * FCSEL moves bits only: no rounding, no arithmetic.
 */
static int cpu_compare(OmegaNumericOp op, const float *a, const float *b, float *out, size_t n) {
    if (!a || !b || !out) return OMEGA_NUMERIC_ERR_BAD_ARGS;
    if (!omega_numeric_fpenv_ok()) return OMEGA_NUMERIC_ERR_FPENV;
    const float one = 1.0f;
    const float zero = 0.0f; /* MUT:CMP_FALSE_NEG_ZERO */
    for (size_t i = 0; i < n; i++) {
        float x = a[i], y = b[i], r, t;
#define CPU_CMP(cond_) __asm__ volatile("fcmp %s1, %s2\n\tfcsel %s0, %s3, %s4, " cond_ \
                                        : "=&w"(r) : "w"(x), "w"(y), "w"(one), "w"(zero) : "cc")
        switch (op) {
        case OMEGA_NOP_FSETP_SEL:     CPU_CMP("ge"); break;   /* GE: N==V    */
        case OMEGA_NOP_FSETP_LT_SEL:  CPU_CMP("mi"); break;   /* N           */
        case OMEGA_NOP_FSETP_LE_SEL:  CPU_CMP("ls"); break;   /* C==0 or Z   */
        case OMEGA_NOP_FSETP_GT_SEL:  CPU_CMP("gt"); break;   /* !Z, N==V    */
        case OMEGA_NOP_FSETP_EQ_SEL:  CPU_CMP("eq"); break;
        case OMEGA_NOP_FSETP_NUM_SEL: CPU_CMP("vc"); break;   /* ordered     */
        case OMEGA_NOP_FSETP_NAN_SEL: CPU_CMP("vs"); break;   /* unordered   */
        case OMEGA_NOP_FSETP_LTU_SEL: CPU_CMP("lt"); break;   /* N!=V        */
        case OMEGA_NOP_FSETP_LEU_SEL: CPU_CMP("le"); break;   /* Z or N!=V   */
        case OMEGA_NOP_FSETP_GTU_SEL: CPU_CMP("hi"); break;   /* C and !Z    */
        case OMEGA_NOP_FSETP_GEU_SEL: CPU_CMP("pl"); break;   /* N==0        */
        case OMEGA_NOP_FSETP_NEU_SEL: CPU_CMP("ne"); break;   /* !Z          */
        case OMEGA_NOP_FSETP_NE_SEL:   /* less or greater */
            __asm__ volatile("fcmp %s2, %s3\n\tfcsel %s1, %s4, %s5, gt\n\tfcsel %s0, %s4, %s1, mi"
                             : "=&w"(r), "=&w"(t) : "w"(x), "w"(y), "w"(one), "w"(zero) : "cc");
            break;
        case OMEGA_NOP_FSETP_EQU_SEL:  /* equal or unordered */
            __asm__ volatile("fcmp %s2, %s3\n\tfcsel %s1, %s4, %s5, vs\n\tfcsel %s0, %s4, %s1, eq"
                             : "=&w"(r), "=&w"(t) : "w"(x), "w"(y), "w"(one), "w"(zero) : "cc");
            break;
        default:
            return OMEGA_NUMERIC_ERR_BAD_ARGS;
        }
#undef CPU_CMP
        out[i] = r;
    }
    return OMEGA_NUMERIC_OK;
}

static const OmegaTensorRealization CPU_REALIZATION = {
    .name = "CPU_E1_TIER",
    .reduce_order = OMEGA_TENSOR_REDUCE_DECLARED_ORDER,
    .elementwise = cpu_elementwise,
    .reduce = omega_tensor_seam_reduce_cpu,
    .transc = cpu_transc,
    .compare = cpu_compare,
};

const OmegaTensorRealization *omega_tensor_cpu_realization(void) { return &CPU_REALIZATION; }
