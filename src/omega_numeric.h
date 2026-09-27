#ifndef OMEGA_NUMERIC_H
#define OMEGA_NUMERIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Frozen numerical reduction order specification:
 * Pairwise binary tree across 32 lanes (delta = 16, 8, 4, 2, 1).
 */
#define OMEGA_WARP_REDUCTION_DECLARED_ORDER "PAIRWISE_TREE_LANE_DELTA_16_8_4_2_1"

/*
 * Bit-identical floating-point classification
 */
typedef enum {
    OMEGA_FP_ZERO_POS = 0,
    OMEGA_FP_ZERO_NEG,
    OMEGA_FP_INF_POS,
    OMEGA_FP_INF_NEG,
    OMEGA_FP_QNAN,
    OMEGA_FP_SNAN,
    OMEGA_FP_SUBNORMAL,
    OMEGA_FP_NORMAL_SMALLEST,
    OMEGA_FP_NORMAL_LARGEST,
    OMEGA_FP_NORMAL_RANDOM
} OmegaFpClass;

/*
 * Bitcast utility functions
 */
static inline uint32_t omega_float_to_bits(float f) {
    union { float f; uint32_t u; } u;
    u.f = f;
    return u.u;
}

static inline float omega_bits_to_float(uint32_t u) {
    union { float f; uint32_t u; } un;
    un.u = u;
    return un.f;
}

/*
 * Bit-level IEEE-754 classification and manipulation (Zero Libm)
 */
#define OMEGA_INF_POS   0x7f800000U
#define OMEGA_INF_NEG   0xff800000U
#define OMEGA_QNAN_BITS 0x7fc00000U

static inline float omega_fabs(float x) {
    return omega_bits_to_float(omega_float_to_bits(x) & 0x7fffffffU);
}

static inline bool omega_isnan(float x) {
    uint32_t u = omega_float_to_bits(x);
    return ((u & 0x7f800000U) == 0x7f800000U) && ((u & 0x007fffffU) != 0);
}

static inline bool omega_isinf(float x) {
    uint32_t u = omega_float_to_bits(x);
    return (u & 0x7fffffffU) == 0x7f800000U;
}

static inline bool omega_signbit(float x) {
    return (omega_float_to_bits(x) & 0x80000000U) != 0;
}

static inline bool omega_issubnormal(float x) {
    uint32_t u = omega_float_to_bits(x);
    return ((u & 0x7f800000U) == 0) && ((u & 0x007fffffU) != 0);
}

static inline bool omega_iszero(float x) {
    return (omega_float_to_bits(x) & 0x7fffffffU) == 0;
}

/*
 * Semantic Reference Tier:
 * Exact IEEE 754-2008 single-precision specification.
 * Subnormals are preserved bit-exactly (Flush-To-Zero is strictly rejected).
 */
float omega_ref_fadd(float a, float b);
float omega_ref_fsub(float a, float b);
float omega_ref_fmul(float a, float b);
float omega_ref_ffma(float a, float b, float c);
float omega_ref_fmin(float a, float b);
float omega_ref_fmax(float a, float b);
bool  omega_ref_fsetp_ge(float a, float b);
float omega_ref_i2f(int32_t a);
int32_t omega_ref_f2i(float a);

/*
 * Omega-defined refinement sequences for Division & Square Root:
 * Raw MUFU is allowed ONLY as an initial seed. Final result is computed
 * by an Omega-defined Newton-Raphson refinement sequence with subnormals preserved.
 */
float omega_math_div(float x, float y);
float omega_math_sqrt(float x);

/*
 * Omega-defined polynomial approximations for Transcendentals:
 * Zero dependency on libm or CUDA math libraries.
 * Range reduction, coefficients, and evaluation order are frozen.
 */
float omega_math_exp(float x);
float omega_math_log(float x);

/*
 * Declared-order warp reduction:
 * Reduces 32 float inputs across a warp using the frozen pairwise-tree order.
 */
float omega_warp_reduce_sum(const float warp_inputs[32]);

/*
 * Execution on physical GB10 silicon:
 * Assembles and dispatches SIMT kernels to the GB10 GPFIFO channel
 * and reads back the computed results.
 */
int omega_gb10_execute_simt_op(const char *op_name,
                              const float *in_a,
                              const float *in_b,
                              const float *in_c,
                              float *out_res,
                              size_t count);

/*
 * Equivalence comparator:
 * Verifies that two floats match bit-for-bit, except that all quiet/signaling
 * NaNs are considered equivalent in the NaN class.
 */
bool omega_numeric_bits_equal(float a, float b);

#endif /* OMEGA_NUMERIC_H */
