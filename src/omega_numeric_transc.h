/*
 * omega_numeric_transc.h -- E1 WP-B: frozen FP32 transcendental sequences,
 * CPU tier, under the bounded contract.
 *
 * Contract (full text: docs/numeric/E1_TRANSCENDENTAL_CONTRACT.md):
 *   - FP32 in, FP32 out. Every op is a fixed sequence of IEEE binary32
 *     instructions (FADD, FSUB, FMUL, FMADD, FDIV, FSQRT), integer operations
 *     and comparisons, written as explicit AArch64 instructions so the
 *     compiler cannot contract, reassociate or re-round anything. No libm,
 *     no long double, no double inside a sequence.
 *   - Required FP environment: FPCR round to nearest even, no flush to zero,
 *     no default NaN, no FEAT_AFP controls (OMEGA_NUMERIC_FPCR_REQUIRED_CLEAR).
 *   - Determinism: bit-identical output for every input on every run.
 *   - Cross-substrate parity: a future realization (GB10 or other) must
 *     reproduce these sequences bit for bit; the full-domain output digests
 *     in the contract are the parity target.
 *   - Error: integer ulp distance from the correctly rounded result (see the
 *     contract), bounded by OMEGA_TRANSC_MAX_ULP_* below and checked over all
 *     2^32 inputs by tests/test_omega_transc.c (full mode).
 *   - NaN results are the canonical quiet NaN 0x7fc00000.
 *
 * These ops are not registered in the OMEGA_NOP table yet.
 */
#ifndef OMEGA_NUMERIC_TRANSC_H
#define OMEGA_NUMERIC_TRANSC_H

#include <stdint.h>

/* Declared maximum ulp distance from the correctly rounded result:
 * observed maximum over all 2^32 inputs + 1 ulp margin (RSQRT: correctly
 * rounded by construction, observed 0, no margin). */
#define OMEGA_TRANSC_MAX_ULP_SIGMOID 3u   /* observed 2 */
#define OMEGA_TRANSC_MAX_ULP_TANH    3u   /* observed 2 */
#define OMEGA_TRANSC_MAX_ULP_RSQRT   0u   /* correctly rounded, observed 0 */
#define OMEGA_TRANSC_MAX_ULP_EXP2    2u   /* observed 1 */
#define OMEGA_TRANSC_MAX_ULP_LOG2    2u   /* observed 1 */
#define OMEGA_TRANSC_MAX_ULP_ERF     3u   /* observed 2 */
#define OMEGA_TRANSC_MAX_ULP_SIN     2u   /* observed 1, |x| <= 2^22 */
#define OMEGA_TRANSC_MAX_ULP_COS     2u   /* observed 1, |x| <= 2^22 */
#define OMEGA_TRANSC_MAX_ULP_GELU    3u   /* observed 2 */

/* SIN/COS admitted domain: |x| <= 2^22. Outside it the result is NaN. */
#define OMEGA_TRANSC_TRIG_MAX_ABS 4194304.0f

float omega_math_sigmoid(float x); /* 1 / (1 + e^-x)                          */
float omega_math_tanh(float x);
float omega_math_rsqrt(float x);   /* 1 / sqrt(x), correctly rounded            */
float omega_math_exp2(float x);
float omega_math_log2(float x);
float omega_math_erf(float x);
float omega_math_sin(float x);     /* |x| <= 2^22, else NaN                      */
float omega_math_cos(float x);     /* |x| <= 2^22, else NaN                      */
float omega_math_gelu(float x);    /* x * Phi(x) = x/2 * (1 + erf(x / sqrt 2))   */

#endif
