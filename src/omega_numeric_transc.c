/*
 * E1 WP-B: frozen FP32 transcendental sequences, CPU tier (bounded contract).
 *
 * Every arithmetic step below is one explicit AArch64 binary32 instruction
 * (FADD, FSUB, FMUL, FMADD, FDIV, FSQRT) issued through inline asm, so the
 * compiler cannot fuse, reassociate, constant-fold or widen it. Sign flips,
 * classification, exponent extraction and powers of two are integer bit
 * operations. Comparisons are exact. Nothing here uses libm, long double or
 * double. The written order of operations IS the specification: a GB10 (or
 * any other) realization must issue the same operations in the same order
 * (FMADD = single-rounding fused multiply-add, FDIV/FSQRT = correctly rounded)
 * to be bit-identical.
 *
 * Coefficients are frozen FP32 values written as exact hex literals
 * (mantissa integer times a power of two). Derivation (50+ digit arithmetic,
 * each rounded once to FP32): Taylor coefficients of e^r, 2^f (ln2^n/n!),
 * tanh (Bernoulli form), erf, sin, cos, the atanh series of log2, and
 * Taylor coefficients of erfcx(y) = e^(y^2) erfc(y) about 17 centres via the
 * exact recurrence (n+1) a_(n+1) = 2c a_n + 2 a_(n-1) - (2/sqrt(pi)) [n=0].
 * See docs/numeric/E1_TRANSCENDENTAL_CONTRACT.md.
 *
 * OMEGA_TRANSC_MUTATE=<id> (test builds only) perturbs one coefficient or
 * step so the test can prove its bound check catches a wrong sequence.
 */
#include "omega_numeric_transc.h"

#include <stdint.h>
#include <string.h>

#if !defined(__aarch64__)
#error "omega_numeric_transc.c issues AArch64 binary32 instructions (fadd, fmul, fmadd, fdiv, fsqrt)"
#endif

#ifndef OMEGA_TRANSC_MUTATE
#define OMEGA_TRANSC_MUTATE 0
#endif

/* ---- Instruction primitives ------------------------------------------------ */

static inline uint32_t tb(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float tf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static inline float fadd(float a, float b) { float r; __asm__("fadd %s0, %s1, %s2" : "=w"(r) : "w"(a), "w"(b)); return r; }
static inline float fsub(float a, float b) { float r; __asm__("fsub %s0, %s1, %s2" : "=w"(r) : "w"(a), "w"(b)); return r; }
static inline float fmul(float a, float b) { float r; __asm__("fmul %s0, %s1, %s2" : "=w"(r) : "w"(a), "w"(b)); return r; }
static inline float fdiv(float a, float b) { float r; __asm__("fdiv %s0, %s1, %s2" : "=w"(r) : "w"(a), "w"(b)); return r; }
static inline float fsqrt(float a) { float r; __asm__("fsqrt %s0, %s1" : "=w"(r) : "w"(a)); return r; }
/* a*b + c, one rounding (FMADD). */
static inline float ffma(float a, float b, float c) {
    float r;
    __asm__("fmadd %s0, %s1, %s2, %s3" : "=w"(r) : "w"(a), "w"(b), "w"(c));
    return r;
}
static inline float fneg(float a) { return tf(tb(a) ^ 0x80000000u); }
static inline float fabs_(float a) { return tf(tb(a) & 0x7fffffffu); }
static inline int is_nan(float a) { return (tb(a) & 0x7fffffffu) > 0x7f800000u; }
/* 2^n as an FP32 normal, n in [-126, 127]. */
static inline float pow2i(int32_t n) { return tf((uint32_t)(n + 127) << 23); }

#define QNAN tf(0x7fc00000u)
#define PINF tf(0x7f800000u)
#define NINF tf(0xff800000u)

/* Mutation hook: flips mantissa bit 12 of one coefficient (relative 2^-11). */
static inline float mut(float v) { return tf(tb(v) ^ (1u << 12)); }
#define K(id, v) ((OMEGA_TRANSC_MUTATE == (id)) ? mut(v) : (v))
enum {
    MUT_EXP2 = 1, MUT_LOG2 = 2, MUT_EXPCORE = 3, MUT_TANH = 4, MUT_RSQRT = 5,
    MUT_ERF = 6, MUT_ERFCX = 7, MUT_SIN = 8, MUT_COS = 9, MUT_GELU = 10,
};

/* 1.5 * 2^23: x + MAGIC rounds x to an integer (ties to even) for |x| < 2^22,
 * and the low mantissa bits of the sum hold that integer. */
#define MAGIC 0xC00000p0f

/* m * 2^k with k in [-252, 254] and m normal (every caller passes a normal m);
 * one rounding (the last multiply), exact when the result is normal. */
static float scale2(float m, int32_t k) {
    if (k > 127) return fmul(fmul(m, pow2i(127)), pow2i(k - 127));
    if (k < -126) return fmul(fmul(m, pow2i(k + 126)), pow2i(-126));
    return fmul(m, pow2i(k));
}

/* 2Sum: s + e == a + b exactly. */
static inline void two_sum(float a, float b, float *s, float *e) {
    float x = fadd(a, b);
    float bv = fsub(x, a);
    float av = fsub(x, bv);
    *s = x;
    *e = fadd(fsub(a, av), fsub(b, bv));
}

/* ---- Shared exp core ------------------------------------------------------- */

/*
 * e^(ah + al) = m * 2^k, |al| tiny next to |ah|, |ah| <= 190.
 * k = nearest(ah / ln2); r = ah - k ln2 (Cody-Waite: the first FMADD is exact:
 * when k != 0, |ah| > 0.34, so ah and k LN2_HI (21 significant bits) are both
 * multiples of g = min(ulp(ah), 2^-24) >= 2^-25, and |r| < 0.35 < 2^24 g),
 * then r += al and
 * m = 1 + r + r^2/2! + ... + r^8/8! (Horner, FMADD), |r| <= 0.35.
 */
static float exp_core2(float ah, float al, int32_t *k_out, float *ml) {
    const float INVLN2 = 0xB8AA3Bp-23f;
    const float LN2_HI = 0xB17218p-24f;
    const float LN2_LO = -0x82E308p-52f;
    float t = fadd(fmul(ah, INVLN2), MAGIC);
    float kf = fsub(t, MAGIC);
    int32_t k = (int32_t)(tb(t) - tb(MAGIC));
    float r = ffma(fneg(kf), LN2_HI, ah);
    r = ffma(fneg(kf), LN2_LO, r);
    r = fadd(r, al);
    float p = 0xD00D01p-39f;                 /* 1/8! */
    p = ffma(p, r, 0xD00D01p-36f);           /* 1/7! */
    p = ffma(p, r, 0xB60B61p-33f);           /* 1/6! */
    p = ffma(p, r, 0x888889p-30f);           /* 1/5! */
    p = ffma(p, r, 0xAAAAABp-28f);           /* 1/4! */
    p = ffma(p, r, K(MUT_EXPCORE, 0xAAAAABp-26f)); /* 1/3! */
    p = ffma(p, r, 0.5f);
    p = ffma(p, r, 1.0f);
    *k_out = k;
    float m = ffma(p, r, 1.0f);
    *ml = ffma(p, r, fsub(1.0f, m));        /* rounding error of m: 1 - m exact (Sterbenz) */
    return m;
}

static float exp_core(float ah, float al, int32_t *k_out) {
    float ml;
    return exp_core2(ah, al, k_out, &ml);
}

/* ---- EXP2 ------------------------------------------------------------------ */

float omega_math_exp2(float x) {
    if (is_nan(x)) return QNAN;
    if (x >= 128.0f) return PINF;            /* includes +inf */
    if (x < -151.0f) return 0.0f;            /* includes -inf; 2^x < 2^-151 rounds to +0 */
    float t = fadd(x, MAGIC);
    float kf = fsub(t, MAGIC);
    int32_t k = (int32_t)(tb(t) - tb(MAGIC));
    float f = fsub(x, kf);                   /* exact, |f| <= 1/2 */
    float p = 0xB16011p-43f;                 /* ln2^8/8! */
    p = ffma(p, f, 0xFFE5FEp-40f);
    p = ffma(p, f, 0xA18489p-36f);
    p = ffma(p, f, 0xAEC3FFp-33f);
    p = ffma(p, f, 0x9D955Bp-30f);
    p = ffma(p, f, 0xE35847p-28f);
    p = ffma(p, f, K(MUT_EXP2, 0xF5FDF0p-26f));
    p = ffma(p, f, 0xB17218p-24f);           /* ln2 */
    float m = ffma(p, f, 1.0f);
    return scale2(m, k);
}

/* ---- LOG2 ------------------------------------------------------------------ */

float omega_math_log2(float x) {
    uint32_t u = tb(x);
    if (is_nan(x)) return QNAN;
    if ((u & 0x7fffffffu) == 0) return NINF;  /* +-0 */
    if (u & 0x80000000u) return QNAN;         /* x < 0, incl. -inf */
    if (u == 0x7f800000u) return PINF;
    int32_t eadj = 0;
    if ((u & 0x7f800000u) == 0) {             /* subnormal: exact scale by 2^23 */
        x = fmul(x, 0x800000p0f);
        u = tb(x);
        eadj = -23;
    }
    int32_t e = (int32_t)((u >> 23) & 0xffu) - 127 + eadj;
    float m = tf((u & 0x007fffffu) | 0x3f800000u);   /* [1, 2) */
    if (m > 0xB504F3p-23f) {                   /* > sqrt(2) rounded: m in (0.707, 1.414] */
        m = fmul(m, 0.5f);
        e += 1;
    }
    /* z = (m - 1)/(m + 1) with its rounding residue: num exact (Sterbenz),
     * den = m + 1 rounded with exact error dl (2Sum), zl ~ (num/(den+dl)) - z. */
    float num = fsub(m, 1.0f);
    float den, dl;
    two_sum(m, 1.0f, &den, &dl);
    float z = fdiv(num, den);
    float rz = ffma(fneg(z), den, num);       /* exact remainder */
    rz = ffma(fneg(z), dl, rz);
    float zl = fdiv(rz, den);
    /* log2(m) = (2/ln2) (z + z^3/3 + z^5/5 + ... + z^11/11) */
    const float C_HI = 0xB8AA3Bp-22f;         /* 2/ln2 */
    const float C_LO = 0xA57060p-48f;
    float z2 = fmul(z, z);
    float p = 0x864D42p-25f;                  /* 2/(11 ln2) */
    p = ffma(p, z2, 0xA4258Ap-25f);           /* 2/(9 ln2) */
    p = ffma(p, z2, 0xD30BB1p-25f);           /* 2/(7 ln2) */
    p = ffma(p, z2, 0x93BB63p-24f);           /* 2/(5 ln2) */
    p = ffma(p, z2, K(MUT_LOG2, 0xF6384Fp-24f)); /* 2/(3 ln2) */
    float z3 = fmul(z, z2);
    float ph = fmul(z, C_HI);
    float pl = ffma(z, C_HI, fneg(ph));       /* exact */
    float lo = ffma(z3, p, ffma(z, C_LO, fmul(zl, C_HI)));
    float s, se;
    two_sum((float)e, ph, &s, &se);           /* e is an exact small integer */
    return fadd(s, fadd(se, fadd(pl, lo)));
}

/* ---- SIGMOID --------------------------------------------------------------- */

float omega_math_sigmoid(float x) {
    if (is_nan(x)) return QNAN;
    if (x >= 18.0f) return 1.0f;              /* 1 - s(x) < 2^-25: rounds to 1 */
    if (x <= -104.0f) return 0.0f;            /* s(x) < e^-104 < 2^-150: rounds to +0 */
    int32_t k;
    float m = exp_core(fneg(fabs_(x)), 0.0f, &k);   /* e^-|x| = m 2^k */
    float t = scale2(m, k);
    float d = fadd(1.0f, t);
    if (!(tb(x) & 0x80000000u)) return fdiv(1.0f, d);   /* x >= +0 */
    return scale2(fdiv(m, d), k);             /* x < 0: e^x/(1+e^x), one rounding into subnormal */
}

/* ---- TANH ------------------------------------------------------------------ */

float omega_math_tanh(float x) {
    if (is_nan(x)) return QNAN;
    uint32_t sign = tb(x) & 0x80000000u;
    float ax = fabs_(x);
    float r;
    if (ax >= 9.5f) {
        r = 1.0f;                              /* 1 - tanh < 2e^-19 < 2^-25 */
    } else if (ax < 0.5625f) {
        /* odd Taylor series to x^21: x + x^3 (c2 + x^2 (c3 + ...)) */
        float x2 = fmul(ax, ax);
        float p = 0xCB3F0Cp-37f;               /* coefficient of x^21 */
        p = ffma(p, x2, -0xFABEBCp-36f);
        p = ffma(p, x2, 0x9AAC12p-34f);
        p = ffma(p, x2, -0xBED1B2p-33f);
        p = ffma(p, x2, 0xEB69E8p-32f);
        p = ffma(p, x2, -0x91371Bp-30f);
        p = ffma(p, x2, 0xB327A4p-29f);
        p = ffma(p, x2, -0xDD0DD1p-28f);
        p = ffma(p, x2, K(MUT_TANH, 0x888889p-26f));
        p = ffma(p, x2, -0xAAAAABp-25f);       /* -1/3 */
        r = ffma(fmul(ax, x2), p, ax);
    } else {
        /* 1 - 2/(e^(2|x|) + 1) */
        int32_t k;
        float m = exp_core(fadd(ax, ax), 0.0f, &k);
        float e2 = scale2(m, k);
        r = fsub(1.0f, fdiv(2.0f, fadd(e2, 1.0f)));
    }
    return tf(tb(r) | sign);
}

/* ---- RSQRT (correctly rounded) ----------------------------------------------- */

typedef unsigned __int128 u128;

/* Positive normal FP32 bits w -> integer significand M (24 bits) and E with
 * value = M * 2^E. */
static inline void unpack_pos(uint32_t w, uint64_t *M, int32_t *E) {
    *M = (uint64_t)((w & 0x7fffffu) | 0x800000u);
    *E = (int32_t)(w >> 23) - 150;
}

/* Is the midpoint between adjacent positive normal floats w1 < w2 below
 * 1/sqrt(X 2^ex)?  Midpoint = N 2^(Elow - 1) with N odd (no ties exist).
 * mid < 1/sqrt(x)  <=>  N^2 X 2^(2 Elow - 2 + ex) < 1. */
static int mid_below(uint32_t w1, uint32_t w2, uint64_t X, int32_t ex) {
    uint64_t M1, M2;
    int32_t E1, E2;
    unpack_pos(w1, &M1, &E1);
    unpack_pos(w2, &M2, &E2);
    int32_t el = E1 < E2 ? E1 : E2;
    uint64_t N = (M1 << (E1 - el)) + (M2 << (E2 - el));
    u128 L = (u128)N * N * X;                  /* < 2^78 */
    int32_t s = 2 * el - 2 + ex;
    if (s >= 0) return 0;                      /* L 2^s >= 1 */
    if (-s >= 127) return 1;
    return L < ((u128)1 << (-s));
}

float omega_math_rsqrt(float x) {
    uint32_t u = tb(x);
    if (is_nan(x)) return QNAN;
    if (u == 0x00000000u) return PINF;
    if (u == 0x80000000u) return NINF;
    if (u & 0x80000000u) return QNAN;          /* x < 0, incl. -inf */
    if (u == 0x7f800000u) return 0.0f;
    int32_t post = 0;
    if ((u & 0x7f800000u) == 0) {              /* subnormal: x 2^24 exact */
        x = fmul(x, 0x1000000p0f);
        u = tb(x);
        post = 12;
    }
    float y = fdiv(1.0f, fsqrt(x));            /* within one ulp of 1/sqrt(x) */
    uint64_t X;
    int32_t ex;
    unpack_pos(u, &X, &ex);
    uint32_t w = tb(y);
    if (OMEGA_TRANSC_MUTATE != MUT_RSQRT) {
        /* Exact rounding step: move to the neighbour while 1/sqrt(x) lies past
         * the midpoint. Results are normal in [2^-64, 2^63], so w +- 1 stays
         * a positive normal. */
        while (mid_below(w, w + 1u, X, ex)) w += 1u;
        while (!mid_below(w - 1u, w, X, ex)) w -= 1u;
    }
    return fmul(tf(w), pow2i(post));
}

/* ---- ERF / ERFC core ------------------------------------------------------- */

/* erf(y) for 0 <= y < 0.5 (also tiny/subnormal y):
 * (2/sqrt(pi)) y (1 + y^2 q(y^2)),  q = sum_(n=1..7) (-1)^n y^(2n-2)/(n!(2n+1)). */
static float erf_small(float y) {
    const float TSP_HI = 0x906EBBp-23f;        /* 2/sqrt(pi) */
    const float TSP_LO = -0xFBD649p-48f;
    float y2 = fmul(y, y);
    float q = -0xDDEBBDp-40f;                  /* n=7 */
    q = ffma(q, y2, 0xE00E01p-37f);
    q = ffma(q, y2, -0xC6980Cp-34f);
    q = ffma(q, y2, 0x97B426p-31f);
    q = ffma(q, y2, -0xC30C31p-29f);
    q = ffma(q, y2, K(MUT_ERF, 0xCCCCCDp-27f));
    q = ffma(q, y2, -0xAAAAABp-25f);           /* n=1: -1/3 */
    float w = fmul(y2, q);
    float A = fmul(y, TSP_HI);
    float Al = ffma(y, TSP_HI, fneg(A));
    float B = ffma(y, TSP_LO, Al);
    return fadd(A, ffma(A, w, B));
}

#include "omega_numeric_transc_tables.h"

/* erfcx(y) = c + cl: c is the plain Horner result, cl collects the FP32
 * rounding of the last step (exact: a0 - c is exact by Sterbenz, then one
 * FMADD) and the low part of a0. */
static float erfcx_eval2(float y, float *cl) {
    int i = 0;
    while (i + 1 < ERFCX_N && y >= ERFCX_LO[i + 1]) i++;
    float t = fsub(y, ERFCX_C[i]);             /* exact (Sterbenz) */
    const float *a = ERFCX_A[i];
    float p = a[10];
    for (int n = 9; n >= 1; n--) p = ffma(p, t, a[n]);
    float a0 = (OMEGA_TRANSC_MUTATE == MUT_ERFCX && i == 2) ? mut(a[0]) : a[0];
    float c = ffma(p, t, a0);
    *cl = fadd(ffma(p, t, fsub(a0, c)), ERFCX_A0LO[i]);
    return c;
}

static float erfcx_eval(float y) {
    float cl;
    return erfcx_eval2(y, &cl);
}

/*
 * erfc(y) = e^-(yy) * erfcx(y) for 0.5 <= y < 11, returned unscaled as
 * m * 2^k (m normal) so a caller can fold further factors in before the
 * single rounding into the subnormal range. yy = hh + hl exactly.
 */
static float erfc_core(float y, float hh, float hl, int32_t *k) {
    float m = exp_core(fneg(hh), fneg(hl), k);
    return fmul(m, erfcx_eval(y));
}

/* ---- ERF ------------------------------------------------------------------- */

float omega_math_erf(float x) {
    if (is_nan(x)) return QNAN;
    uint32_t sign = tb(x) & 0x80000000u;
    float ax = fabs_(x);
    float r;
    if (ax >= 4.0f) {
        r = 1.0f;                              /* erfc(4) < 2^-25: rounds to 1 */
    } else if (ax < 0.5f) {
        r = erf_small(ax);                     /* +0 -> +0 */
    } else {
        float h = fmul(ax, ax);
        float hl = ffma(ax, ax, fneg(h));      /* exact */
        int32_t k;
        float m = erfc_core(ax, h, hl, &k);
        r = fsub(1.0f, scale2(m, k));          /* erfc >= 1.5e-8: normal */
    }
    return tf(tb(r) | sign);
}

/* ---- GELU (erf form) ------------------------------------------------------- */

float omega_math_gelu(float x) {
    if (is_nan(x)) return QNAN;
    if (x >= 8.0f) return x;                   /* x erfc(x/sqrt2)/2 < 2^-25 x: rounds to x; +inf -> +inf */
    if (x < -15.5f) return tf(0x80000000u);    /* |gelu| < 2^-150: rounds to -0; -inf -> -0 */
    if ((tb(x) & 0x7fffffffu) == 0) return x;  /* gelu(+-0) = +-0 */
    if ((tb(x) & 0x7fffffffu) < 0x01000000u) {
        /* |x| < 2^-125: x/2 is exact or an exact midpoint between subnormals,
         * and the correction x^2/sqrt(2 pi) > 0 is below 2^-124 of it, so it
         * only breaks the tie, always toward +inf (Codex finding: rounding x/2
         * first lost it, e.g. gelu(2^-149) = 2^-149, not 0). Integer only. */
        uint32_t m = tb(x) & 0x7fffffffu;
        uint32_t s = tb(x) & 0x80000000u;
        return tf(s | (s ? (m >> 1) : ((m + 1u) >> 1)));
    }
    const float INV_SQRT2 = K(MUT_GELU, 0xB504F3p-24f);
    float y = fmul(x, INV_SQRT2);
    float ay = fabs_(y);
    float hx = fmul(x, 0.5f);
    if (ay < 0.5f) {
        /* x/2 + x/2 * erf(y) */
        float e = erf_small(ay);
        e = tf(tb(e) | (tb(y) & 0x80000000u));
        float r = ffma(hx, e, hx);
        return tf(tb(r) | (tb(x) & 0x80000000u));  /* gelu has the sign of x: -0 when x < 0 underflows */
    }
    /* y^2 = x^2/2 exactly: x^2 = P + Pl (FMADD residue), halving is exact */
    float P = fmul(x, x);
    float Pl = ffma(x, x, fneg(P));
    float h = fmul(P, 0.5f);
    float hl = fmul(Pl, 0.5f);
    /* the rounding of y is put back to first order: yl = x/sqrt2 - y */
    const float INV_SQRT2_LO = 0xCFE77Ap-50f;  /* 1/sqrt2 - INV_SQRT2 (unmutated) */
    float yl = ffma(x, INV_SQRT2, fneg(y));    /* exact */
    yl = ffma(x, INV_SQRT2_LO, yl);
    float ayl = (tb(x) & 0x80000000u) ? fneg(yl) : yl;
    int32_t k;
    float mel, cl;
    float me = exp_core2(fneg(h), fneg(hl), &k, &mel); /* e^(-y^2) ~ (me + mel) 2^k */
    float c = erfcx_eval2(ay, &cl);            /* erfcx(|y|) ~ c + cl */
    /* erfcx'(y) = 2y erfcx(y) - 2/sqrt(pi) */
    float dc = ffma(fmul(ay, 2.0f), c, -0x906EBBp-23f);
    cl = ffma(ayl, dc, cl);
    /* erfc(|y|) ~ (p + pl) 2^k: p + (exact 2Prod residue) = me c, plus the cross terms */
    float p = fmul(me, c);
    float pl = ffma(me, c, fneg(p));
    pl = ffma(me, cl, pl);
    pl = ffma(mel, c, pl);
    if (!(tb(x) & 0x80000000u)) {
        /* x (1 - erfc(y)/2) = x - (x/2) erfc(y); y < 5.66 keeps erfc normal */
        return ffma(fneg(hx), scale2(p, k), x);
    }
    /* x < 0: (x/2) erfc(|y|), folded before the one rounding into subnormals */
    return scale2(ffma(hx, p, fmul(hx, pl)), k);
}

/* ---- SIN / COS ------------------------------------------------------------- */

/* sin(rh + rl), |rh| <= ~pi/4, |rl| <= ulp(rh) */
static float sin_poly(float rh, float rl) {
    float r2 = fmul(rh, rh);
    float S = 0xB09231p-56f;                   /* 1/13! */
    S = ffma(S, r2, -0xD7322Bp-49f);           /* -1/11! */
    S = ffma(S, r2, 0xB8EF1Dp-42f);            /* 1/9! */
    S = ffma(S, r2, -0xD00D01p-36f);           /* -1/7! */
    S = ffma(S, r2, K(MUT_SIN, 0x888889p-30f)); /* 1/5! */
    S = ffma(S, r2, -0xAAAAABp-26f);           /* -1/3! */
    float t = fmul(rh, r2);
    float rlc = ffma(fmul(r2, -0.5f), rl, rl); /* rl cos(rh) to second order */
    return fadd(rh, ffma(t, S, rlc));
}

/* cos(rh + rl) */
static float cos_poly(float rh, float rl) {
    float r2 = fmul(rh, rh);
    float r2l = ffma(rh, rh, fneg(r2));        /* exact */
    float C = -0xC9CBA5p-60f;                  /* -1/14! */
    C = ffma(C, r2, 0x8F76C7p-52f);            /* 1/12! */
    C = ffma(C, r2, -0x93F27Ep-45f);           /* -1/10! */
    C = ffma(C, r2, 0xD00D01p-39f);            /* 1/8! */
    C = ffma(C, r2, -0xB60B61p-33f);           /* -1/6! */
    C = ffma(C, r2, K(MUT_COS, 0xAAAAABp-28f)); /* 1/4! */
    float hr = fmul(r2, 0.5f);
    float h = fsub(1.0f, hr);
    float eh = fsub(fsub(1.0f, h), hr);        /* exact: 1 - hr = h + eh */
    float c1 = ffma(r2l, -0.5f, eh);
    float c2 = ffma(fneg(rl), rh, c1);
    float r4 = fmul(r2, r2);
    return fadd(h, ffma(r4, C, c2));
}

/*
 * x = k pi/2 + (rh + rl). pi/2 = P1 + P2 + P3 (24 bits each). For
 * |x| <= 2^22: r1 = x - k P1 is exact (one FMADD, |r1| < 1); k P2 is split
 * exactly (FMUL + FMADD residue) and subtracted with two 2Sums; k P3 and
 * the 2Sum errors form the low word.
 */
static float sincos_core(float x, int32_t qoff) {
    if (is_nan(x)) return QNAN;
    float ax = fabs_(x);
    if (!(ax <= OMEGA_TRANSC_TRIG_MAX_ABS)) return QNAN;   /* +-inf and out of domain */
    float rh, rl;
    int32_t k;
    if (ax <= 0xC90FDBp-24f) {                 /* |x| <= pi/4 (rounded) */
        k = 0;
        rh = x;
        rl = 0.0f;
    } else {
        const float P1 = 0xC90FDBp-23f;
        const float P2 = -0xBBBD2Ep-48f;
        const float P3 = -0xF72CEDp-73f;
        float t = fadd(fmul(x, 0xA2F983p-24f), MAGIC);   /* 2/pi */
        float kf = fsub(t, MAGIC);
        k = (int32_t)(tb(t) - tb(MAGIC));
        float r1 = ffma(fneg(kf), P1, x);
        float th = fmul(kf, P2);
        float tl = ffma(kf, P2, fneg(th));
        float s1, e1, s2, e2;
        two_sum(r1, fneg(th), &s1, &e1);
        two_sum(s1, fneg(tl), &s2, &e2);
        float lo = fsub(fadd(e1, e2), fmul(kf, P3));
        rh = fadd(s2, lo);
        rl = fsub(lo, fsub(rh, s2));
    }
    uint32_t q = (uint32_t)(k + qoff) & 3u;
    float r = (q & 1u) ? cos_poly(rh, rl) : sin_poly(rh, rl);
    return (q & 2u) ? fneg(r) : r;
}

float omega_math_sin(float x) {
    if ((tb(x) & 0x7fffffffu) == 0) return x;  /* +-0 */
    return sincos_core(x, 0);
}

float omega_math_cos(float x) {
    return sincos_core(x, 1);
}
