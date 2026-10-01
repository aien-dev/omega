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

/* m * 2^k with k in [-252, 254]; one rounding (the last multiply), exact
 * when the result is normal. */
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
 * k = nearest(ah / ln2); r = ah - k ln2 (Cody-Waite: the first FMADD is exact
 * because LN2_HI has 24 bits and |r| < 1), then r += al and
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

/* erfcx(y) = e^(y^2) erfc(y), 0.5 <= y < 11: degree-10 Taylor polynomial
 * about the centre of one of 17 intervals. */
#define ERFCX_N 17
static const float ERFCX_LO[ERFCX_N] = {
    0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f, 3.5f,
    4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f,
};
static const float ERFCX_C[ERFCX_N] = {
    0.625f, 0.875f, 1.125f, 1.375f, 1.625f, 1.875f, 2.25f, 2.75f, 3.25f, 3.75f,
    4.5f, 5.5f, 6.5f, 7.5f, 8.5f, 9.5f, 10.5f,
};
static const float ERFCX_A[ERFCX_N][11] = {
    { 0x8E8B5Bp-24f, -0xDD5E87p-25f, 0x92BBA1p-25f, -0xACE1AEp-26f, 0xB96A35p-27f, -0xB7E72Fp-28f, 0xAA97F4p-29f, -0x953FA8p-30f, 0xF7E81Ep-32f, -0xC477CDp-33f, 0x953558p-34f },  /* c = 0.625 */
    { 0xEDBA3Fp-25f, -0xA1B4FBp-25f, 0xC077C8p-26f, -0xCEAC34p-27f, 0xCC18E1p-28f, -0xBBCEE9p-29f, 0xA29324p-30f, -0x8559C2p-31f, 0xD077BFp-33f, -0x9BFF38p-34f, 0xE059BEp-36f },  /* c = 0.875 */
    { 0xCA98F0p-25f, -0xF3C59Bp-26f, 0x82F393p-26f, -0x809A1Dp-27f, 0xEA7389p-29f, -0xC8852Fp-30f, 0xA23629p-31f, -0xF9C637p-33f, 0xB7ECD3p-34f, -0x820F1Cp-35f, 0xB13A1Ep-37f },  /* c = 1.125 */
    { 0xAFC47Bp-25f, -0xBCBD32p-26f, 0xB809A1p-27f, -0xA5E6DDp-28f, 0x8BF5D3p-29f, -0xDEF926p-31f, 0xA8D5D0p-32f, -0xF45734p-34f, 0xA9AFACp-35f, -0xE2FD36p-37f, 0x92A769p-38f },  /* c = 1.375 */
    { 0x9AC1FBp-25f, -0x9588F3p-26f, 0x850AD6p-27f, -0xDD00C7p-29f, 0xAD0A15p-30f, -0x80A757p-31f, 0xB6B050p-33f, -0xF8DA1Fp-35f, 0xA32F67p-36f, -0xCEB114p-38f, 0xFD7E48p-40f },  /* c = 1.625 */
    { 0x89F2BAp-25f, -0xF1B2C0p-27f, 0xC5377Fp-28f, -0x977C98p-29f, 0xDCCAC5p-31f, -0x9991BEp-32f, 0xCCDB07p-34f, -0x838706p-35f, 0xA318E2p-37f, -0xC3D30Fp-39f, 0xE42CA4p-41f },  /* c = 1.875 */
    { 0xECA223p-26f, -0xB53869p-27f, 0x830AB4p-28f, -0xB441FDp-30f, 0xED2CABp-32f, -0x95E929p-33f, 0xB6BBA2p-35f, -0xD76BF6p-37f, 0xF63B9Ep-39f, -0x88BD33p-40f, 0x93D7A1p-42f },  /* c = 2.25 */
    { 0xC64F5Bp-26f, -0x8182BCp-27f, 0xA1DCC6p-29f, -0xC274D6p-31f, 0xE16398p-33f, -0xFCCF21p-35f, 0x898DD9p-36f, -0x918959p-38f, 0x95FDAEp-40f, -0x96D195p-42f, 0x942B7Dp-44f },  /* c = 2.75 */
    { 0xAA53D0p-26f, -0xC15429p-28f, 0xD3F6EEp-30f, -0xE125A5p-32f, 0xE842C3p-34f, -0xE92F5Ep-36f, 0xE44178p-38f, -0xDA2E81p-40f, 0xCBEEBCp-42f, -0xBA9E56p-44f, 0xA76049p-46f },  /* c = 3.25 */
    { 0x951579p-26f, -0x955328p-28f, 0x917833p-30f, -0x8A1A57p-32f, 0xFFF819p-35f, -0xE7E6F5p-37f, 0xCDA868p-39f, -0xB2BBC5p-41f, 0x98617Ep-43f, -0xFF1F19p-46f, 0xD1DF72p-48f },  /* c = 3.75 */
    { 0xFAD950p-27f, -0xD51F5Bp-29f, 0xB16096p-31f, -0x90C76Dp-33f, 0xE803C3p-36f, -0xB6AA59p-38f, 0x8D6B52p-40f, -0xD77C75p-43f, 0xA1AA7Ep-45f, -0xEF04AAp-48f, 0xAE325Ap-50f },  /* c = 4.5 */
    { 0xCEC548p-27f, -0x91C654p-29f, 0xCA9296p-32f, -0x8ACBB7p-34f, 0xBBA7A4p-37f, -0xFA7237p-40f, 0xA50BF1p-42f, -0xD6EBD6p-45f, 0x8A4E6Cp-47f, -0xB0000Fp-50f, 0xDD84DDp-53f },  /* c = 5.5 */
    { 0xAFBAE2p-27f, -0xD37168p-30f, 0xFBAF7Cp-33f, -0x943C40p-35f, 0xACD949p-38f, -0xC791FFp-41f, 0xE438D1p-44f, -0x8147A3p-46f, 0x91233Dp-49f, -0xA1801Ap-52f, 0xB2285Ep-55f },  /* c = 6.5 */
    { 0x98BA18p-27f, -0xA0222Bp-30f, 0xA683DEp-33f, -0xABC1D1p-36f, 0xAFC535p-39f, -0xB27E5Ep-42f, 0xB3E516p-45f, -0xB3F8A7p-48f, 0xB2BF9Dp-51f, -0xB04751p-54f, 0xACA33Bp-57f },  /* c = 7.5 */
    { 0x8703C0p-27f, -0xFABE57p-31f, 0xE75118p-34f, -0xD40795p-37f, 0xC12136p-40f, -0xAED44Cp-43f, 0x9D4DC1p-46f, -0x8CB120p-49f, 0xFA31DDp-53f, -0xDD2E77p-56f, 0xC26CBBp-59f },  /* c = 8.5 */
    { 0xF1EDCEp-28f, -0xC98846p-31f, 0xA6FE9Ap-34f, -0x89A7FAp-37f, 0xE1C826p-41f, -0xB83A53p-44f, 0x9592F6p-47f, -0xF1AFD8p-51f, 0xC251B4p-54f, -0x9B7D7Ap-57f, 0xF7AB79p-61f },  /* c = 9.5 */
    { 0xDB1A53p-28f, -0xA5745Ep-31f, 0xF8CB98p-35f, -0xBA40ACp-38f, 0x8AD6E6p-41f, -0xCE1F7Ep-45f, 0x985F41p-48f, -0xE05A4Cp-52f, 0xA47FF1p-55f, -0xF04377p-59f, 0xAEC450p-62f },  /* c = 10.5 */
};

/* a0 = erfcx(c) - (FP32 a0), from the same 120-digit computation */
static const float ERFCX_A0LO[ERFCX_N] = {
    -0xFEC2A1p-50f, 0xE409ACp-50f, 0x9B149Bp-50f, -0xD0C389p-50f, 0xC8864Fp-51f, 0xED8120p-52f,
    0xD62793p-51f, 0x8FF27Dp-51f, 0x8D4BB4p-51f, -0xC7D6DAp-52f, 0x939E99p-53f, -0xDD7B0Dp-52f,
    0xBA5C5Cp-54f, -0xB272B3p-52f, 0xA3E924p-53f, -0x88206Cp-54f, -0xFF9A64p-53f,
};

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
