/* Turing Yield TY-1 math. See ty_math.h. Plain C11, integer scoring path. */
#include "turing/ty_math.h"

#include <math.h>
#include <string.h>

const char *ty_err_name(int e) {
    switch (e) {
    case TY_OK: return "OK";
    case TY_E_NAN: return "NAN";
    case TY_E_INF: return "INF";
    case TY_E_NEG: return "NEGATIVE";
    case TY_E_NORM: return "NOT_NORMALIZED";
    case TY_E_ZERO_REALIZED: return "ZERO_PROB_ON_REALIZED";
    case TY_E_UNDERFLOW: return "UNDERFLOW";
    case TY_E_RANGE: return "RANGE";
    case TY_E_OVERFLOW: return "OVERFLOW";
    case TY_E_ARG: return "ARG";
    case TY_E_FORMAT: return "FORMAT";
    case TY_E_PROFILE: return "PROFILE_MISMATCH";
    case TY_E_BASELINE: return "BASELINE_MISMATCH";
    case TY_E_LEAK: return "LEAK_HELDOUT_IN_FIT";
    case TY_E_PROVENANCE: return "PROVENANCE_MISMATCH";
    case TY_E_DIGEST: return "DIGEST_MISMATCH";
    case TY_E_IO: return "IO";
    default: return "?";
    }
}

/* Bit-by-bit binary logarithm. x = 2^e * m with m in [1, 2) held as Q62.
 * Each step squares m (exact 124-bit product, truncated back to Q62); when
 * m^2 >= 2 the next fractional bit is 1 and m is halved. Truncating the product
 * loses < 2^-62 relative per step; over 32 steps that perturbs m by < 2^-29
 * relative at the last step, which can move the result by < 2^-31. */
uint64_t ty_log2_q32(uint64_t x) {
    if (x == 0) return 0;
    unsigned e = 63u - (unsigned)__builtin_clzll(x);
    ty_u128 m;
    if (e <= 62)
        m = (ty_u128)x << (62 - e);
    else
        m = (ty_u128)(x >> 1); /* e == 63: drops one low bit, < 2^-62 relative */
    uint64_t frac = 0;
    const ty_u128 two = (ty_u128)1 << 63;
    for (int i = 0; i < 32; ++i) {
        m = (m * m) >> 62;
        frac <<= 1;
        if (m >= two) {
            m >>= 1;
            frac |= 1;
        }
    }
    return ((uint64_t)e << 32) | frac;
}

/* Round a nonnegative Q32 bit count to micro-bits. */
static int64_t q32_to_ub(uint64_t v) {
    ty_u128 t = (ty_u128)v * (uint64_t)TY_UB_PER_BIT + ((uint64_t)1 << 31);
    return (int64_t)(t >> 32);
}

static int64_t ub_table[TY_QONE + 1];
static int ub_table_ready;

static void ub_table_init(void) {
    ub_table[0] = TY_E_RANGE;
    for (uint32_t q = 1; q <= TY_QONE; ++q) {
        uint64_t v = ((uint64_t)TY_QBITS << 32) - ty_log2_q32(q); /* q <= 2^16, so v >= 0 */
        ub_table[q] = q32_to_ub(v);
    }
    ub_table_ready = 1;
}

int64_t ty_ubits_q16(uint32_t q) {
    if (q == 0 || q > TY_QONE) return TY_E_RANGE;
    if (!ub_table_ready) ub_table_init();
    return ub_table[q];
}

int ty_ubits_ratio(uint64_t num, uint64_t den, int64_t *out) {
    if (!out) return TY_E_ARG;
    if (num == 0) return TY_E_ZERO_REALIZED;
    if (num > den) return TY_E_RANGE;
    uint64_t v = ty_log2_q32(den) - ty_log2_q32(num);
    /* Both logs truncate by < 2^-31; the difference is off by < 2^-31. Guard the
     * num == den case against a spurious negative. */
    if (num == den) v = 0;
    *out = q32_to_ub(v);
    return TY_OK;
}

int ty_validate_f64(const double *p, size_t n, double tol) {
    if (!p || n == 0 || !(tol >= 0.0) || isinf(tol)) return TY_E_ARG;
    double s = 0.0;
    for (size_t i = 0; i < n; ++i) {
        if (isnan(p[i])) return TY_E_NAN;
        if (isinf(p[i])) return TY_E_INF;
        if (p[i] < 0.0 || signbit(p[i])) {
            if (p[i] == 0.0) continue; /* -0.0 counts as zero */
            return TY_E_NEG;
        }
        s += p[i];
    }
    if (isinf(s) || fabs(s - 1.0) > tol) return TY_E_NORM;
    return TY_OK;
}

int ty_ubits_f64(const double *p, size_t n, size_t realized, double tol, int64_t *out) {
    if (!out || realized >= n) return TY_E_ARG;
    int rc = ty_validate_f64(p, n, tol);
    if (rc != TY_OK) return rc;
    double v = p[realized];
    if (v == 0.0) return TY_E_ZERO_REALIZED;
    if (fpclassify(v) == FP_SUBNORMAL) return TY_E_UNDERFLOW;
    if (v > 1.0) return TY_E_RANGE; /* only possible inside tol; refuse, never clamp */
    double scaled = ldexp(v, 62);    /* exact: power-of-two scaling */
    if (scaled < 1.0) return TY_E_UNDERFLOW;
    uint64_t num = (uint64_t)scaled; /* truncation, exact integer part */
    return ty_ubits_ratio(num, (uint64_t)1 << 62, out);
}

int ty_add(int64_t a, int64_t b, int64_t *out) {
    if (!out) return TY_E_ARG;
    if (__builtin_add_overflow(a, b, out)) return TY_E_OVERFLOW;
    return TY_OK;
}

int ty_sub(int64_t a, int64_t b, int64_t *out) {
    if (!out) return TY_E_ARG;
    if (__builtin_sub_overflow(a, b, out)) return TY_E_OVERFLOW;
    return TY_OK;
}

int ty_bits_to_ub(uint64_t bits, int64_t *out) {
    if (!out) return TY_E_ARG;
    if (bits > (uint64_t)(INT64_MAX / TY_UB_PER_BIT)) return TY_E_OVERFLOW;
    *out = (int64_t)bits * TY_UB_PER_BIT;
    return TY_OK;
}

int ty_dl(uint64_t model_bits, int64_t data_ub, int64_t *out) {
    int64_t m;
    if (data_ub < 0) return TY_E_RANGE;
    int rc = ty_bits_to_ub(model_bits, &m);
    if (rc != TY_OK) return rc;
    return ty_add(m, data_ub, out);
}

int ty_gain(int64_t dl_baseline_ub, int64_t dl_candidate_ub, int64_t *out) {
    if (dl_baseline_ub < 0 || dl_candidate_ub < 0) return TY_E_RANGE;
    return ty_sub(dl_baseline_ub, dl_candidate_ub, out);
}

int ty_quantize_kt(const uint64_t *counts, unsigned K, uint32_t *q) {
    if (!counts || !q || K < 2 || K > TY_KMAX) return TY_E_ARG;
    ty_u128 w[TY_KMAX], W = 0;
    for (unsigned s = 0; s < K; ++s) {
        if (counts[s] > ((uint64_t)1 << 62)) return TY_E_RANGE;
        w[s] = (ty_u128)counts[s] * 2 + 1; /* KT: n + 1/2, doubled */
        W += w[s];
    }
    const uint64_t R = TY_QONE - K; /* one unit per symbol is the floor */
    uint64_t base_sum = 0;
    ty_u128 rem[TY_KMAX];
    for (unsigned s = 0; s < K; ++s) {
        ty_u128 t = (ty_u128)R * w[s];
        uint64_t b = (uint64_t)(t / W);
        rem[s] = t % W;
        q[s] = 1u + (uint32_t)b;
        base_sum += b;
    }
    uint64_t left = R - base_sum; /* < K */
    unsigned char given[TY_KMAX];
    memset(given, 0, sizeof given);
    while (left > 0) {
        int best = -1;
        for (unsigned s = 0; s < K; ++s)
            if (!given[s] && (best < 0 || rem[s] > rem[best])) best = (int)s;
        given[best] = 1;
        q[best] += 1;
        --left;
    }
    return TY_OK;
}
