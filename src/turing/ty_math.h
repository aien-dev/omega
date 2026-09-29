/* Turing Yield TY-1: description-length arithmetic (docs/turing/TURING_YIELD_PROFILE_V0.md).
 *
 * Unit: 1 T = 1 bit of net held-out description-length gain versus a declared
 * baseline under a versioned measurement profile. Everything that lands in a
 * digested record is an integer: code lengths are int64 micro-bits (1 bit =
 * 1,000,000 ub), model lengths are exact bit counts. Bits, never nats.
 *
 * The scoring path is integer only (no libm): a probability is a 16-bit
 * quantized table entry q in 1..65536 (P = q / 65536), and -log2 P comes from
 * an integer log2 (ty_log2_q32). Per-symbol error of ty_ubits_q16 against the
 * exact real value is at most 0.5 ub (rounding) + 0.001 ub (log2 truncation),
 * so a sum over N symbols is within 0.501 * N ub of exact, in any build.
 *
 * The double-precision entry points (ty_validate_f64, ty_ubits_f64) exist so
 * that an outside predictor's distribution is checked before it is scored.
 */
#ifndef TURING_TY_MATH_H
#define TURING_TY_MATH_H

#include <stddef.h>
#include <stdint.h>

#define TY_QBITS 16
#define TY_QONE 65536u           /* quantized probability of 1 */
#define TY_UB_PER_BIT 1000000LL  /* micro-bits per bit */
#define TY_KMAX 16               /* largest alphabet a model may declare */

__extension__ typedef unsigned __int128 ty_u128;
/* Worst-case |computed - exact| per scored symbol, in units of 1/1000 ub. */
#define TY_QERR_MILLI_UB_PER_SYMBOL 501

enum {
    TY_OK = 0,
    TY_E_NAN = -1,            /* a probability is NaN */
    TY_E_INF = -2,            /* a probability is infinite */
    TY_E_NEG = -3,            /* a probability is negative */
    TY_E_NORM = -4,           /* the vector does not sum to 1 within tolerance */
    TY_E_ZERO_REALIZED = -5,  /* the outcome that happened was given probability 0 */
    TY_E_UNDERFLOW = -6,      /* realized probability below 2^-62 (or subnormal) */
    TY_E_RANGE = -7,          /* argument outside its declared range */
    TY_E_OVERFLOW = -8,       /* int64 micro-bit arithmetic would overflow */
    TY_E_ARG = -9,            /* NULL or malformed argument */
    TY_E_FORMAT = -10,        /* malformed input bytes (CTR1, model code) */
    TY_E_PROFILE = -11,       /* a model or record disagrees with the measurement profile */
    TY_E_BASELINE = -12,      /* baseline is not the one the profile declares */
    TY_E_LEAK = -13,          /* a model was fit on held-out (split-reserved) data */
    TY_E_PROVENANCE = -14,    /* fit provenance does not match the split manifest */
    TY_E_DIGEST = -15,        /* a recomputed digest differs from the recorded one */
    TY_E_IO = -16
};
const char *ty_err_name(int e);

/* log2(x) for x >= 1 as unsigned Q32.32 (integer part in the high 32 bits),
 * truncated toward zero; error < 2^-31. Returns 0 for x == 0 (callers refuse
 * zero before calling). Integer only. */
uint64_t ty_log2_q32(uint64_t x);

/* Micro-bits of -log2(q / 65536), q in 1..65536 (q = 65536 -> 0). Rounded to
 * nearest. Negative return = TY_E_RANGE. Table-backed after first use. */
int64_t ty_ubits_q16(uint32_t q);

/* Micro-bits of -log2(num / den), 1 <= num <= den. */
int ty_ubits_ratio(uint64_t num, uint64_t den, int64_t *out);

/* Validate a probability vector: no NaN/Inf/negative, |sum - 1| <= tol
 * (summed in index order), n >= 1. */
int ty_validate_f64(const double *p, size_t n, double tol);

/* Validate p, then micro-bits of -log2 p[realized]. The realized value is
 * converted exactly to a Q62 integer (ldexp + truncation), so the result is
 * build-independent: ZERO_REALIZED if p[realized] == 0, UNDERFLOW if it is
 * subnormal or below 2^-62. Normalization is not re-imposed; a vector off by
 * less than tol is scored as given. */
int ty_ubits_f64(const double *p, size_t n, size_t realized, double tol, int64_t *out);

/* Checked int64 arithmetic for description lengths. */
int ty_add(int64_t a, int64_t b, int64_t *out);
int ty_sub(int64_t a, int64_t b, int64_t *out);
int ty_bits_to_ub(uint64_t bits, int64_t *out);
/* DL(M, D) = L(M) + L(D|M): model length in whole bits, data length in ub. */
int ty_dl(uint64_t model_bits, int64_t data_ub, int64_t *out);
/* T(M; B, D) = DL(B, D) - DL(M, D), in ub. */
int ty_gain(int64_t dl_baseline_ub, int64_t dl_candidate_ub, int64_t *out);

/* Profile estimator: KT (add-1/2) smoothing of counts, then 16-bit
 * quantization by largest remainder with every entry >= 1 (the floor) and the
 * K entries summing to exactly 65536. Ties in remainder go to the lower
 * symbol index. 2 <= K <= TY_KMAX. */
int ty_quantize_kt(const uint64_t *counts, unsigned K, uint32_t *q);

#endif /* TURING_TY_MATH_H */
