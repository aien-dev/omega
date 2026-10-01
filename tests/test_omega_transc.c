/*
 * test_omega_transc.c -- E1 WP-B: bounded-contract checks for the frozen FP32
 * transcendental sequences in src/omega_numeric_transc.c.
 *
 * Oracle: an independent IEEE binary128 evaluation (long double on AArch64
 * Linux: 113-bit significand, software arithmetic from the compiler runtime,
 * no libm). It shares no code with the sequences: different algorithms
 * (series run to 2^-118 relative, squaring of expm1, a 2^23-entry log2
 * table, Newton for 1/sqrt, the Kummer series and a continued fraction for
 * erf/erfc, 80+113+113-bit pi/2 reduction), then one rounding to FP32 by the
 * compiler runtime. Where an input range is skipped by a short closed form
 * (tiny |x|, saturation) the bound that justifies it is written next to it.
 *
 * Error metric: d(s, o) = | ord(s) - ord(o) |, where o is the oracle value
 * rounded once to FP32 and ord() maps FP32 bits to consecutive integers
 * (+0 and -0 both map to 0, the smallest subnormal to +-1). So d counts the
 * representable FP32 values between the sequence result and the correctly
 * rounded result: d = 0 means correctly rounded, d <= n means within n ulp
 * of the CR result (ulp of the result's binade, 2^-149 in the subnormal
 * range). Special results must match exactly: NaN (canonical 0x7fc00000),
 * +-inf, and the sign of a zero result.
 *
 * Modes:
 *   (none) | fast   special values, oracle self-checks, about 1.1M inputs per
 *                   op (2^20 multiplicative-hash samples of all 2^32 bit
 *                   patterns plus +-64 ulp around every threshold),
 *                   determinism (two evaluations, different thread counts).
 *   full            all 2^32 inputs of every op against the oracle, then a
 *                   second sequence-only pass with another thread count and
 *                   reversed chunk order; prints the full-domain digests.
 *   digest          sequence-only full-domain digests, compared with the
 *                   frozen digests below (cross-run, cross-build parity).
 * Optional second argument: an op name to restrict the run.
 * Env OMEGA_TRANSC_THREADS (default 8).
 * Exit 0 only if every check passes.
 */
#include "omega_numeric_transc.h"
#include "omega_numeric.h"
#include "sha256.h"

#include <float.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if LDBL_MANT_DIG != 113
#error "oracle needs IEEE binary128 long double (AArch64 Linux)"
#endif

typedef long double Q;

static int g_fail = 0;
static int g_verbose = 0;            /* env OMEGA_TRANSC_VERBOSE: print up to 40 over-bound inputs */
static atomic_int g_printed;
#define FAIL(...) do { printf("FAIL: " __VA_ARGS__); printf("\n"); g_fail = 1; } while (0)

static inline uint32_t bits_of(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float float_of(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline int nan_bits(uint32_t u) { return (u & 0x7fffffffu) > 0x7f800000u; }

/* ---- binary128 oracle ------------------------------------------------------ */

static const Q QLN2 = 0.6931471805599453094172321214581765680755001343L;
static const Q QINVLN2 = 1.4426950408889634073599246810018921374266459541L;
static const Q QTWO_OVER_SQRTPI = 1.1283791670955125738961589031215451716881012586L;
static const Q QINV_SQRTPI = 0.5641895835477562869480794515607725858440506293L;
static const Q QINV_SQRT2 = 0.7071067811865475244008443621048490392848359376L;
static const Q QINV_SQRT2PI = 0.3989422804014326779399460599343818684758586311L;
static const Q QTWO_OVER_PI = 0.6366197723675813430755350534900574481378385829L;
static const Q QPIO4 = 0.78539816339744830961566084581987572104929234985L;
/* pi/2 = PA + PB + PC: PA has 80 bits (k PA exact for k < 2^33). */
static const Q QPA = 1.570796326794896619231321054808035091003570737910877141985110938549041748046875L;
static const Q QPB = 6.36831716351095013961776675768502358811299070339e-25L;
static const Q QPC = 2.54630579611570017288406302157224126338250084121e-61L;

static Q qabs(Q a) { return a < 0 ? -a : a; }

/* 2^k exactly, |k| <= 16000. */
static Q q_pow2(int k) {
    union { Q q; uint64_t w[2]; } u;
    u.w[0] = 0;
    u.w[1] = (uint64_t)(k + 16383) << 48;
    return u.q;
}

static uint32_t q_round(Q v) { return bits_of((float)v); }

/* e^u - 1 for |u| <= 0.75: Taylor on u/256 to 2^-120, then 8 squarings in
 * the form (1+e)^2 - 1 = e (2 + e), which keeps relative accuracy. */
static Q q_expm1_small(Q u) {
    Q v = u * q_pow2(-8);
    Q s = v, t = v;
    for (int n = 2; n < 40; n++) {
        t = t * v / (Q)n;
        s += t;
        if (qabs(t) < qabs(s) * q_pow2(-120)) break;
    }
    for (int i = 0; i < 8; i++) s = s * (2.0L + s);
    return s;
}

static Q q_exp(Q a) {
    Q kq = a * QINVLN2;
    long long k = (long long)(kq >= 0 ? kq + 0.5L : kq - 0.5L);
    Q r = a - (Q)k * QLN2;
    return (1.0L + q_expm1_small(r)) * q_pow2((int)k);
}

static Q q_expm1(Q u) {
    if (qabs(u) <= 0.75L) return q_expm1_small(u);
    return q_exp(u) - 1.0L;
}

/* ln(m), m in [1, 2): 2 atanh(z), z = (m-1)/(m+1) <= 1/3, run to 2^-120. */
static Q q_ln_mant(Q m) {
    Q z = (m - 1.0L) / (m + 1.0L), z2 = z * z;
    Q s = z, t = z;
    for (int n = 3; n < 400; n += 2) {
        t *= z2;
        Q term = t / (Q)n;
        s += term;
        if (term < s * q_pow2(-120)) break;
    }
    return 2.0L * s;
}

/* erf(y), y >= 0, y < 4.5. */
static Q q_erf(Q y) {
    if (y < 0.5L) {                         /* alternating Maclaurin series */
        Q y2 = y * y, t = y, s = y;
        for (int n = 1; n < 200; n++) {
            t = -t * y2 / (Q)n;
            Q term = t / (Q)(2 * n + 1);
            s += term;
            if (qabs(term) < qabs(s) * q_pow2(-120)) break;
        }
        return QTWO_OVER_SQRTPI * s;
    }
    /* Kummer form, all terms positive: e^-y^2 sum y (2y^2)^n / (2n+1)!! */
    Q y2 = y * y, t = y, s = y;
    for (int n = 1; n < 2000; n++) {
        t = t * 2.0L * y2 / (Q)(2 * n + 1);
        s += t;
        if (t < s * q_pow2(-120)) break;
    }
    return QTWO_OVER_SQRTPI * q_exp(-y2) * s;
}

/* erfc(y), y >= 4.5: continued fraction
 * erfc(y) = e^-y^2 / sqrt(pi) / (y + (1/2)/(y + 1/(y + (3/2)/(y + ...)))),
 * evaluated backwards from depth n. Self-checked against depth 2n and
 * against 1 - erf at y = 4.5 (oracle_selfcheck). */
static Q q_erfc_cf(Q y, int depth) {
    Q f = y;
    for (int n = depth; n >= 1; n--) f = y + ((Q)n * 0.5L) / f;
    return q_exp(-y * y) * QINV_SQRTPI / f;
}
#define ERFC_CF_DEPTH 240

static Q q_erfc(Q y) {                       /* any sign */
    if (y < 0) return -y < 4.5L ? 1.0L + q_erf(-y) : 2.0L - q_erfc_cf(-y, ERFC_CF_DEPTH);
    if (y < 4.5L) return 1.0L - q_erf(y);
    return q_erfc_cf(y, ERFC_CF_DEPTH);
}

/* sin / cos of a reduced |r| <= ~pi/4 + tiny, series to 2^-120. */
static Q q_sin_r(Q r) {
    Q r2 = r * r, t = r, s = r;
    for (int n = 1; n < 60; n++) {
        t = -t * r2 / (Q)((2 * n) * (2 * n + 1));
        s += t;
        if (qabs(t) < qabs(s) * q_pow2(-120)) break;
    }
    return s;
}
static Q q_cos_r(Q r) {
    Q r2 = r * r, t = 1.0L, s = 1.0L;
    for (int n = 1; n < 60; n++) {
        t = -t * r2 / (Q)((2 * n - 1) * (2 * n));
        s += t;
        if (qabs(t) < qabs(s) * q_pow2(-120)) break;
    }
    return s;
}
/* x = k pi/2 + r, |x| <= 2^22; x - k PA exact (see QPA). */
static Q q_sincos(Q x, int want_cos) {
    long long k = 0;
    Q r = x;
    if (qabs(x) > QPIO4) {
        Q kq = x * QTWO_OVER_PI;
        k = (long long)(kq >= 0 ? kq + 0.5L : kq - 0.5L);
        r = ((x - (Q)k * QPA) - (Q)k * QPB) - (Q)k * QPC;
    }
    unsigned q = (unsigned)((k + want_cos) & 3);
    Q v = (q & 1) ? q_cos_r(r) : q_sin_r(r);
    return (q & 2) ? -v : v;
}

/* 1/sqrt(v), v in [1, 4): double seed (FSQRT/FDIV instructions), two Newton
 * steps in binary128 (53 -> 106 -> >113 bits). */
static Q q_rsqrt(Q v) {
    double d = (double)v, s;
    __asm__("fsqrt %d0, %d1" : "=w"(s) : "w"(d));
    Q y = (Q)(1.0 / s);
    for (int i = 0; i < 3; i++) y = y * (3.0L - v * y * y) * 0.5L;
    return y;
}

/* Tables (built once, in parallel): log2 of every mantissa 1 + j 2^-23 and
 * the FP32-rounded 1/sqrt of every (mantissa, exponent parity) pair. */
static Q *g_log2_tab;        /* 2^23 entries */
static uint32_t *g_rsqrt_tab; /* 2^24 entries */

/* positive finite nonzero x = X 2^E with X in [2^23, 2^24) */
static void norm_pos(uint32_t u, uint32_t *X, int *E) {
    uint32_t e = (u >> 23) & 0xffu, m = u & 0x7fffffu;
    if (e == 0) {
        int sh = 0;
        while (!(m & 0x800000u)) { m <<= 1; sh++; }
        *X = m;
        *E = 1 - 150 - sh;
    } else {
        *X = m | 0x800000u;
        *E = (int)e - 150;
    }
}

/* ---- per-op oracles: FP32 bits of the correctly rounded result -------------- */

#define QNANB 0x7fc00000u
#define PINFB 0x7f800000u
#define NINFB 0xff800000u

static Q xq(uint32_t u) { return (Q)float_of(u); }

static uint32_t or_sigmoid(uint32_t u) {
    if (nan_bits(u)) return QNANB;
    Q x = xq(u);
    if (x >= 40.0L) return 0x3f800000u;       /* 1 - s < e^-40 << 2^-25 */
    if (x <= -110.0L) return 0;               /* s < e^-110 < 2^-158 */
    if (qabs(x) < q_pow2(-40)) return q_round(0.5L + x * 0.25L - x * x * x / 48.0L); /* next term x^5/480 */
    return q_round(1.0L / (1.0L + q_exp(-x)));
}

static uint32_t or_tanh(uint32_t u) {
    if (nan_bits(u)) return QNANB;
    Q x = xq(u), ax = qabs(x), r;
    if (ax >= 12.0L) r = 1.0L;                /* 1 - tanh < 2e^-24 << 2^-25 */
    else if (ax < q_pow2(-40)) r = ax - ax * ax * ax / 3.0L + 2.0L * ax * ax * ax * ax * ax / 15.0L;
    else if (ax < 0.5L) { Q em = q_expm1(2.0L * ax); r = em / (em + 2.0L); }
    else r = 1.0L - 2.0L / (q_exp(2.0L * ax) + 1.0L);
    uint32_t b = q_round(r);
    return b | (u & 0x80000000u);
}

static uint32_t or_rsqrt(uint32_t u) {
    if (nan_bits(u)) return QNANB;
    if (u == 0) return PINFB;
    if (u == 0x80000000u) return NINFB;
    if (u & 0x80000000u) return QNANB;
    if (u == PINFB) return 0;
    uint32_t X; int E;
    norm_pos(u, &X, &E);
    /* x = (X/2^23) 2^(E+23); p = parity of that exponent */
    int ee = E + 23, p = ee & 1;
    uint32_t r = g_rsqrt_tab[(X - 0x800000u) | ((uint32_t)p << 23)];
    int adj = -(ee - p) / 2;                  /* result normal: exact scaling */
    return r + (uint32_t)(adj * (1 << 23));
}

static uint32_t or_exp2(uint32_t u) {
    if (nan_bits(u)) return QNANB;
    Q x = xq(u);
    if (x >= 128.0L) return PINFB;
    if (x < -152.0L) return 0;
    if (x == (Q)(long long)x) return q_round(q_pow2((int)(long long)x));   /* exact (2^-150 ties to +0) */
    if (qabs(x) < q_pow2(-40)) { Q t = x * QLN2; return q_round(1.0L + t + t * t / 2.0L + t * t * t / 6.0L); }
    return q_round(q_exp(x * QLN2));
}

static uint32_t or_log2(uint32_t u) {
    if (nan_bits(u)) return QNANB;
    if ((u & 0x7fffffffu) == 0) return NINFB;
    if (u & 0x80000000u) return QNANB;
    if (u == PINFB) return PINFB;
    uint32_t X; int E;
    norm_pos(u, &X, &E);
    return q_round((Q)(E + 23) + g_log2_tab[X - 0x800000u]);
}

static uint32_t or_erf(uint32_t u) {
    if (nan_bits(u)) return QNANB;
    Q ax = qabs(xq(u));
    uint32_t b = ax >= 4.5L ? 0x3f800000u : q_round(q_erf(ax));   /* erfc(4.5) < 2^-32 */
    return b | (u & 0x80000000u);
}

static uint32_t or_trig(uint32_t u, int want_cos) {
    if (nan_bits(u)) return QNANB;
    Q x = xq(u);
    if (!(qabs(x) <= 4194304.0L)) return QNANB;   /* contract domain |x| <= 2^22 */
    if (qabs(x) < q_pow2(-40)) {                  /* next terms < 2^-160 relative */
        if (want_cos) return q_round(1.0L - x * x / 2.0L);
        if (x == 0) return u;
        return q_round(x - x * x * x / 6.0L);
    }
    return q_round(q_sincos(x, want_cos));
}
static uint32_t or_sin(uint32_t u) { return or_trig(u, 0); }
static uint32_t or_cos(uint32_t u) { return or_trig(u, 1); }

static uint32_t or_gelu(uint32_t u) {
    if (nan_bits(u)) return QNANB;
    Q x = xq(u);
    if (x >= 16.0L) return u;                 /* x erfc(11.3)/2 < 2^-180 x; +inf */
    if (x <= -16.0L) return 0x80000000u;      /* |gelu| < 8 erfc(11.3) < 2^-180 */
    if (x == 0) return u;
    if (qabs(x) < q_pow2(-100)) {
        /* x^2/sqrt(2 pi) is below 2^-100 of x/2 and would vanish from the
         * binary128 sum; any value in (x/2, x/2 + |x/2| 2^-100] rounds to float
         * like the true one (only x/2 itself can be a float boundary). */
        Q t = 0.5L * x;
        return q_round(t + qabs(t) * q_pow2(-100));
    }
    if (qabs(x) < q_pow2(-40)) return q_round(0.5L * x + x * x * QINV_SQRT2PI);  /* next x^4/(6 sqrt(2pi)) */
    return q_round(x * 0.5L * q_erfc(-x * QINV_SQRT2));
}

/* ---- op table ---------------------------------------------------------------- */

typedef struct {
    const char *name;
    float (*seq)(float);
    uint32_t (*oracle)(uint32_t);
    uint32_t bound;
    const char *frozen;         /* full-domain digest hex, "" until recorded */
} op_t;

/* Frozen full-domain digests (hex): D = sha256(C_0 .. C_255), C_c = sha256(B_c,0 .. B_c,1023),
 * B_c,b = sha256 of the outputs (FP32 bits, little-endian) for the 2^14 inputs
 * whose bits are (c 2^24 + b 2^14) .. +2^14-1, in increasing order.
 * Recorded from the first exhaustive run (make test-numeric-transc-full);
 * An empty string would mean "not recorded" and makes digest mode FAIL. */
#define DIGEST_SIGMOID "b4e43cb75fb625314aa464a4b4cffd7bc398d75cb7a50ff23af786025af1886d"
#define DIGEST_TANH    "932c09ffe7f02125dc302bfe9c3666a4866b109627fd89cfca0827cf3b1e5daa"
#define DIGEST_RSQRT   "454fc9d5fb2e640acc8ba48422c3ae5e61d17fcee8cec20091cfb4a044ec51e4"
#define DIGEST_EXP2    "227cb65782077d8224121eca527f8beba5d9fc80648ee57c38895466a8345bd8"
#define DIGEST_LOG2    "fdf21ee2f13fa4ffec0c5c5ea1af4e1650ff9dbd91b138a608a68b43670a07f3"
#define DIGEST_ERF     "aef63f62db823726c45f88b6605263d614854595679edcb17c3a4492abc74aba"
#define DIGEST_SIN     "9e4debc75f832121cea47e9fad2e008329ccc62b291c3b68e6f60bb3f908cddc"
#define DIGEST_COS     "28d580ee2efe1256397cc8a3b2dcd4bfe397100a0f9663e1c010baa136dcceb3"
#define DIGEST_GELU    "bda4da73a44f4b06cd1ebebe13dec56b91c7e87b826253380e922b3ba38689db"

static op_t g_ops[] = {
    { "SIGMOID", omega_math_sigmoid, or_sigmoid, OMEGA_TRANSC_MAX_ULP_SIGMOID, DIGEST_SIGMOID },
    { "TANH",    omega_math_tanh,    or_tanh,    OMEGA_TRANSC_MAX_ULP_TANH,    DIGEST_TANH },
    { "RSQRT",   omega_math_rsqrt,   or_rsqrt,   OMEGA_TRANSC_MAX_ULP_RSQRT,   DIGEST_RSQRT },
    { "EXP2",    omega_math_exp2,    or_exp2,    OMEGA_TRANSC_MAX_ULP_EXP2,    DIGEST_EXP2 },
    { "LOG2",    omega_math_log2,    or_log2,    OMEGA_TRANSC_MAX_ULP_LOG2,    DIGEST_LOG2 },
    { "ERF",     omega_math_erf,     or_erf,     OMEGA_TRANSC_MAX_ULP_ERF,     DIGEST_ERF },
    { "SIN",     omega_math_sin,     or_sin,     OMEGA_TRANSC_MAX_ULP_SIN,     DIGEST_SIN },
    { "COS",     omega_math_cos,     or_cos,     OMEGA_TRANSC_MAX_ULP_COS,     DIGEST_COS },
    { "GELU",    omega_math_gelu,    or_gelu,    OMEGA_TRANSC_MAX_ULP_GELU,    DIGEST_GELU },
};
#define NOPS ((int)(sizeof(g_ops) / sizeof(g_ops[0])))

/* ---- comparison ---------------------------------------------------------------- */

static int64_t ord(uint32_t u) {
    return (u & 0x80000000u) ? -(int64_t)(u & 0x7fffffffu) : (int64_t)u;
}

/* Returns the distance, or UINT32_MAX for a special-value mismatch. */
static uint32_t dist(uint32_t s, uint32_t o) {
    if (nan_bits(o) || nan_bits(s)) return (s == QNANB && nan_bits(o)) ? 0 : UINT32_MAX;
    if ((o & 0x7fffffffu) == PINFB || (s & 0x7fffffffu) == PINFB) return s == o ? 0 : UINT32_MAX;
    if ((o & 0x7fffffffu) == 0 || (s & 0x7fffffffu) == 0) return s == o ? 0 : UINT32_MAX;  /* zero is exact: either side zero demands identical bits */
    int64_t d = ord(s) - ord(o);
    if (d < 0) d = -d;
    return d >= UINT32_MAX ? UINT32_MAX - 1 : (uint32_t)d;
}

typedef struct {
    uint64_t n, over, hist[4];   /* hist: d = 0, 1, 2, >= 3 */
    uint32_t maxd, worst_in, worst_seq, worst_or;
} stats_t;

static void stats_add(stats_t *st, uint32_t in, uint32_t s, uint32_t o, uint32_t bound) {
    uint32_t d = dist(s, o);
    if (st->n == 0 || d > st->maxd) { st->maxd = d; st->worst_in = in; st->worst_seq = s; st->worst_or = o; }
    st->n++;
    st->hist[d < 3 ? d : 3]++;
    if (d > bound) {
        st->over++;
        if (g_verbose && atomic_fetch_add(&g_printed, 1) < 40)
            printf("  over: x=0x%08x seq=0x%08x cr=0x%08x d=%u\n", in, s, o, d);
    }
}

static void stats_merge(stats_t *a, const stats_t *b) {
    if (b->n == 0) return;
    if (a->n == 0 || b->maxd > a->maxd) {
        a->maxd = b->maxd; a->worst_in = b->worst_in; a->worst_seq = b->worst_seq; a->worst_or = b->worst_or;
    }
    a->n += b->n;
    a->over += b->over;
    for (int i = 0; i < 4; i++) a->hist[i] += b->hist[i];
}

static void stats_print(const op_t *op, const stats_t *st, const char *what) {
    printf("%-8s %-10s n=%llu max_ulp=%s%u (bound %u) at x=0x%08x seq=0x%08x cr=0x%08x  d0=%llu d1=%llu d2=%llu d3+=%llu over=%llu  %s\n",
           op->name, what, (unsigned long long)st->n,
           st->maxd == UINT32_MAX ? "SPECIAL_MISMATCH:" : "", st->maxd == UINT32_MAX ? 0 : st->maxd,
           op->bound, st->worst_in, st->worst_seq, st->worst_or,
           (unsigned long long)st->hist[0], (unsigned long long)st->hist[1],
           (unsigned long long)st->hist[2], (unsigned long long)st->hist[3],
           (unsigned long long)st->over, st->over ? "FAIL" : "PASS");
    if (st->over) FAIL("%s %s: %llu inputs exceed the declared bound", op->name, what, (unsigned long long)st->over);
}

/* ---- threads -------------------------------------------------------------- */

static int g_threads = 8;

typedef struct {
    void (*fn)(void *ctx, uint32_t item, stats_t *st);
    void *ctx;
    uint32_t nitems;
    int reverse;
    atomic_uint next;
    pthread_mutex_t mu;
    stats_t total;
} pool_t;

static void *pool_worker(void *arg) {
    pool_t *p = arg;
    stats_t local;
    memset(&local, 0, sizeof(local));
    for (;;) {
        uint32_t i = atomic_fetch_add(&p->next, 1u);
        if (i >= p->nitems) break;
        uint32_t item = p->reverse ? p->nitems - 1u - i : i;
        p->fn(p->ctx, item, &local);
    }
    pthread_mutex_lock(&p->mu);
    stats_merge(&p->total, &local);
    pthread_mutex_unlock(&p->mu);
    return NULL;
}

static stats_t pool_run(void (*fn)(void *, uint32_t, stats_t *), void *ctx, uint32_t nitems, int threads, int reverse) {
    pool_t p;
    memset(&p, 0, sizeof(p));
    p.fn = fn; p.ctx = ctx; p.nitems = nitems; p.reverse = reverse;
    atomic_init(&p.next, 0u);
    pthread_mutex_init(&p.mu, NULL);
    pthread_t th[64];
    if (threads > 64) threads = 64;
    for (int t = 0; t < threads; t++) pthread_create(&th[t], NULL, pool_worker, &p);
    for (int t = 0; t < threads; t++) pthread_join(th[t], NULL);
    pthread_mutex_destroy(&p.mu);
    return p.total;
}

/* ---- tables ----------------------------------------------------------------- */

static void build_log2_item(void *ctx, uint32_t item, stats_t *st) {
    (void)ctx; (void)st;
    for (uint32_t j = item << 15; j < ((item + 1u) << 15); j++) {
        Q m = 1.0L + (Q)j * q_pow2(-23);
        g_log2_tab[j] = q_ln_mant(m) * QINVLN2;
    }
}
static void build_rsqrt_item(void *ctx, uint32_t item, stats_t *st) {
    (void)ctx; (void)st;
    for (uint32_t j = item << 16; j < ((item + 1u) << 16); j++) {
        Q v = (1.0L + (Q)(j & 0x7fffffu) * q_pow2(-23)) * ((j >> 23) ? 2.0L : 1.0L);
        g_rsqrt_tab[j] = q_round(q_rsqrt(v));
    }
}
static void build_tables(void) {
    if (g_log2_tab) return;
    g_log2_tab = malloc(sizeof(Q) << 23);
    g_rsqrt_tab = malloc(sizeof(uint32_t) << 24);
    if (!g_log2_tab || !g_rsqrt_tab) { printf("FAIL: oracle table allocation\n"); exit(2); }
    pool_run(build_log2_item, NULL, 1u << 8, g_threads, 0);
    pool_run(build_rsqrt_item, NULL, 1u << 8, g_threads, 0);
}

/* ---- oracle self-checks ------------------------------------------------------ */

static void check_rel(const char *what, Q got, Q want, int bits) {
    Q e = qabs(got - want);
    if (!(e <= qabs(want) * q_pow2(-bits))) FAIL("oracle self-check %s", what);
}

static void oracle_selfcheck(void) {
    check_rel("exp(ln2) = 2", q_exp(QLN2), 2.0L, 100);
    check_rel("exp(1) exp(-1) = 1", q_exp(1.0L) * q_exp(-1.0L), 1.0L, 100);
    check_rel("exp(-100) e^100", q_exp(-100.0L) * q_exp(100.0L), 1.0L, 98);
    check_rel("ln(1.5) inverts exp", q_exp(q_ln_mant(1.5L)), 1.5L, 100);
    check_rel("ln(1.9999) inverts exp", q_exp(q_ln_mant(1.9999L)), 1.9999L, 100);
    check_rel("log2 table entry 2^22 is log2(1.5)", g_log2_tab[1u << 22] * QLN2, q_ln_mant(1.5L), 100);
    check_rel("expm1 small", q_expm1(1e-20L), 1e-20L + 0.5e-40L, 100);
    check_rel("erfc CF depth", q_erfc_cf(4.5L, ERFC_CF_DEPTH), q_erfc_cf(4.5L, 2 * ERFC_CF_DEPTH), 105);
    check_rel("erfc CF vs 1-erf at 4.5", q_erfc_cf(4.5L, ERFC_CF_DEPTH), 1.0L - q_erf(4.5L), 70);
    check_rel("erf small/Kummer seam at 0.5", q_erf(0.4999999999999999999999L), q_erf(0.5L), 60);
    check_rel("sin^2 + cos^2", q_sin_r(0.7L) * q_sin_r(0.7L) + q_cos_r(0.7L) * q_cos_r(0.7L), 1.0L, 108);
    check_rel("sin(pi/6) = 1/2", q_sincos(QPA / 3.0L + QPB / 3.0L, 0), 0.5L, 100);
    check_rel("cos(2^20 pi/2 + 1) reduction", q_sincos(1048576.0L * QPA + 1048576.0L * QPB + 1.0L, 1), q_cos_r(1.0L), 70);
    check_rel("rsqrt(2)^2 * 2", q_rsqrt(2.0L) * q_rsqrt(2.0L) * 2.0L, 1.0L, 110);
    check_rel("erf(1) known value", q_erf(1.0L), 0.84270079294971486934122063508260925929606699796630L, 108);
    check_rel("erfc(5) known value", q_erfc(5.0L), 1.5374597944280348501883434e-12L, 80);
    check_rel("gelu constant 1/sqrt(2pi)", QINV_SQRT2PI * QINV_SQRT2PI * 2.0L / QINV_SQRTPI / QINV_SQRTPI, 1.0L, 108);
    printf("oracle self-checks: %s\n", g_fail ? "FAIL" : "PASS");
}

/* ---- special values (the contract table) ------------------------------------ */

static void sv(const char *op, float (*fn)(float), uint32_t in, uint32_t want) {
    uint32_t got = bits_of(fn(float_of(in)));
    if (got != want) FAIL("special %s(0x%08x) = 0x%08x, contract says 0x%08x", op, in, got, want);
}

static void special_values(void) {
    const uint32_t PZ = 0, NZ = 0x80000000u, ONE = 0x3f800000u, MONE = 0xbf800000u, SNAN = 0x7f800001u, NNAN = 0xffc00001u;
    sv("sigmoid", omega_math_sigmoid, SNAN, QNANB);  sv("sigmoid", omega_math_sigmoid, NNAN, QNANB);
    sv("sigmoid", omega_math_sigmoid, PINFB, ONE);   sv("sigmoid", omega_math_sigmoid, NINFB, PZ);
    sv("sigmoid", omega_math_sigmoid, PZ, 0x3f000000u); sv("sigmoid", omega_math_sigmoid, NZ, 0x3f000000u);
    sv("tanh", omega_math_tanh, SNAN, QNANB); sv("tanh", omega_math_tanh, PINFB, ONE); sv("tanh", omega_math_tanh, NINFB, MONE);
    sv("tanh", omega_math_tanh, PZ, PZ); sv("tanh", omega_math_tanh, NZ, NZ); sv("tanh", omega_math_tanh, 1u, 1u);
    sv("rsqrt", omega_math_rsqrt, PZ, PINFB); sv("rsqrt", omega_math_rsqrt, NZ, NINFB); sv("rsqrt", omega_math_rsqrt, PINFB, PZ);
    sv("rsqrt", omega_math_rsqrt, NINFB, QNANB); sv("rsqrt", omega_math_rsqrt, MONE, QNANB); sv("rsqrt", omega_math_rsqrt, SNAN, QNANB);
    sv("rsqrt", omega_math_rsqrt, 0x40800000u, 0x3f000000u); sv("rsqrt", omega_math_rsqrt, ONE, ONE);
    sv("rsqrt", omega_math_rsqrt, 0x00000001u, 0x64b504f3u);    /* 2^74.5, correctly rounded */
    sv("exp2", omega_math_exp2, SNAN, QNANB); sv("exp2", omega_math_exp2, PINFB, PINFB); sv("exp2", omega_math_exp2, NINFB, PZ);
    sv("exp2", omega_math_exp2, PZ, ONE); sv("exp2", omega_math_exp2, NZ, ONE); sv("exp2", omega_math_exp2, 0x43000000u, PINFB);
    sv("exp2", omega_math_exp2, 0xc3160000u, PZ);       /* 2^-150: tie, rounds to +0 */
    sv("exp2", omega_math_exp2, 0xc3150000u, 1u);       /* 2^-149 */
    sv("exp2", omega_math_exp2, 0x42fe0000u, 0x7f000000u); /* 2^127 */
    sv("exp2", omega_math_exp2, 0x41200000u, 0x44800000u); /* 2^10 */
    sv("log2", omega_math_log2, PZ, NINFB); sv("log2", omega_math_log2, NZ, NINFB); sv("log2", omega_math_log2, MONE, QNANB);
    sv("log2", omega_math_log2, NINFB, QNANB); sv("log2", omega_math_log2, PINFB, PINFB); sv("log2", omega_math_log2, ONE, PZ);
    sv("log2", omega_math_log2, SNAN, QNANB);
    sv("log2", omega_math_log2, 1u, 0xc3150000u);      /* -149 */
    sv("log2", omega_math_log2, 0x7f000000u, 0x42fe0000u); /* 127 */
    sv("log2", omega_math_log2, 0x3f000000u, MONE);
    sv("erf", omega_math_erf, SNAN, QNANB); sv("erf", omega_math_erf, PINFB, ONE); sv("erf", omega_math_erf, NINFB, MONE);
    sv("erf", omega_math_erf, PZ, PZ); sv("erf", omega_math_erf, NZ, NZ);
    sv("sin", omega_math_sin, PZ, PZ); sv("sin", omega_math_sin, NZ, NZ); sv("sin", omega_math_sin, PINFB, QNANB);
    sv("sin", omega_math_sin, NINFB, QNANB); sv("sin", omega_math_sin, SNAN, QNANB);
    sv("sin", omega_math_sin, 0x4a800001u, QNANB);     /* 2^22 + 0.5: outside the domain */
    sv("cos", omega_math_cos, PZ, ONE); sv("cos", omega_math_cos, NZ, ONE); sv("cos", omega_math_cos, PINFB, QNANB);
    sv("cos", omega_math_cos, NINFB, QNANB); sv("cos", omega_math_cos, 0xca800001u, QNANB);
    sv("gelu", omega_math_gelu, SNAN, QNANB); sv("gelu", omega_math_gelu, PINFB, PINFB); sv("gelu", omega_math_gelu, NINFB, NZ);
    sv("gelu", omega_math_gelu, PZ, PZ); sv("gelu", omega_math_gelu, NZ, NZ);
    /* tiny subnormal ties (Codex): x/2 + x^2/sqrt(2pi) breaks toward +inf */
    sv("gelu", omega_math_gelu, 0x00000001u, 0x00000001u); sv("gelu", omega_math_gelu, 0x80000001u, NZ);
    sv("gelu", omega_math_gelu, 0x00000003u, 0x00000002u); sv("gelu", omega_math_gelu, 0x80000003u, 0x80000001u);
    sv("gelu", omega_math_gelu, 0x00800001u, 0x00400001u); sv("gelu", omega_math_gelu, 0x80800001u, 0x80400000u);
    sv("gelu", omega_math_gelu, 0x00000002u, 0x00000001u); sv("gelu", omega_math_gelu, 0x80000002u, 0x80000001u);
    printf("special values: %s\n", g_fail ? "FAIL" : "PASS");
}

/* ---- fast mode: sampled inputs ------------------------------------------------- */

static uint32_t *g_sample;
static uint32_t g_nsample;

static void build_sample(void) {
    static const float th[] = {
        0.0f, 1.0f, 0.5f, 0.5625f, 0.75f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f, 3.5f, 4.0f, 5.0f, 6.0f, 7.0f,
        8.0f, 9.0f, 9.5f, 10.0f, 11.0f, 12.0f, 15.5f, 16.0f, 17.5f, 18.0f, 20.0f, 40.0f, 87.0f, 88.7f, 100.0f,
        103.9f, 104.0f, 110.0f, 126.0f, 127.0f, 128.0f, 149.0f, 150.0f, 151.0f, 152.0f,
        0.78539816f, 1.5707963f, 3.1415927f, 4.712389f, 6.2831855f, 1.41421356f, 0.70710678f,
        4194304.0f, 1048576.0f, 0x1p-12f, 0x1p-24f, 0x1p-40f, 0x1p-126f, 0x1p-149f, 0x1.fffffep127f,
    };
    uint32_t nth = sizeof(th) / sizeof(th[0]);
    uint32_t nhash = 1u << 20;
    g_nsample = nhash + nth * 2u * 129u + 8u;
    g_sample = malloc(sizeof(uint32_t) * g_nsample);
    uint32_t n = 0;
    for (uint32_t i = 0; i < nhash; i++) g_sample[n++] = i * 0x9E3779B1u + 0x7f4a7c15u;
    for (uint32_t i = 0; i < nth; i++) {
        for (uint32_t s = 0; s < 2; s++) {
            uint32_t b = bits_of(th[i]) | (s << 31);
            for (int d = -64; d <= 64; d++) g_sample[n++] = (uint32_t)((int64_t)b + d);
        }
    }
    static const uint32_t extra[] = { 0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u, 0x7f800001u, 0x00000000u, 0x80000000u, 0x00000001u };
    for (uint32_t i = 0; i < 8; i++) g_sample[n++] = extra[i];
    g_nsample = n;
}

typedef struct { const op_t *op; int use_oracle; uint8_t (*dig)[32]; } job_t;

#define SAMPLE_BLOCK 4096u
static void sample_item(void *ctx, uint32_t item, stats_t *st) {
    job_t *j = ctx;
    uint32_t lo = item * SAMPLE_BLOCK, hi = lo + SAMPLE_BLOCK;
    if (hi > g_nsample) hi = g_nsample;
    uint8_t buf[SAMPLE_BLOCK * 4];
    uint32_t nb = 0;
    for (uint32_t i = lo; i < hi; i++) {
        uint32_t in = g_sample[i];
        uint32_t s = bits_of(j->op->seq(float_of(in)));
        memcpy(buf + 4 * nb++, &s, 4);
        if (j->use_oracle) stats_add(st, in, s, j->op->oracle(in), j->op->bound);
    }
    sha256_hash(buf, nb * 4u, j->dig[item]);
}

static void digest_of(uint8_t (*dig)[32], uint32_t n, uint8_t out[32]) {
    sha256_ctx c;
    sha256_init(&c);
    for (uint32_t i = 0; i < n; i++) sha256_update(&c, dig[i], 32);
    sha256_final(&c, out);
}

static void hex32(const uint8_t d[32], char out[65]) {
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

static void run_fast(const op_t *op) {
    uint32_t nitems = (g_nsample + SAMPLE_BLOCK - 1u) / SAMPLE_BLOCK;
    uint8_t (*d1)[32] = calloc(nitems, 32), (*d2)[32] = calloc(nitems, 32);
    job_t j1 = { op, 1, d1 }, j2 = { op, 0, d2 };
    stats_t st = pool_run(sample_item, &j1, nitems, g_threads, 0);
    stats_print(op, &st, "sampled");
    pool_run(sample_item, &j2, nitems, g_threads > 3 ? 3 : 1, 1);
    uint8_t a[32], b[32];
    digest_of(d1, nitems, a);
    digest_of(d2, nitems, b);
    char h[65];
    hex32(a, h);
    if (memcmp(a, b, 32)) FAIL("%s: sampled outputs differ between two runs", op->name);
    else printf("%-8s determinism: two runs bit-identical, sample digest %.16s\n", op->name, h);
    free(d1); free(d2);
}

/* ---- full mode: all 2^32 inputs ------------------------------------------------ */

#define CHUNK_BITS 24u
#define NCHUNK (1u << (32u - CHUNK_BITS))
#define BLOCK_BITS 14u   /* work item = 2^14 inputs; 2^10 items per chunk */

typedef struct { const op_t *op; int use_oracle; uint8_t (*blk)[32]; } fjob_t;

static void full_item(void *ctx, uint32_t item, stats_t *st) {
    fjob_t *j = ctx;
    uint32_t base = item << BLOCK_BITS;
    static __thread uint8_t buf[4u << BLOCK_BITS];
    for (uint32_t i = 0; i < (1u << BLOCK_BITS); i++) {
        uint32_t in = base + i;
        uint32_t s = bits_of(j->op->seq(float_of(in)));
        memcpy(buf + 4 * i, &s, 4);
        if (j->use_oracle) stats_add(st, in, s, j->op->oracle(in), j->op->bound);
    }
    sha256_hash(buf, sizeof(buf), j->blk[item]);
}

/* chunk digest = sha256 of its 2^10 block digests; final = sha256 of 256 chunk digests */
static void full_digest(uint8_t (*blk)[32], uint8_t out[32]) {
    uint32_t per = 1u << (CHUNK_BITS - BLOCK_BITS);
    uint8_t (*ch)[32] = calloc(NCHUNK, 32);
    for (uint32_t c = 0; c < NCHUNK; c++) digest_of(blk + c * per, per, ch[c]);
    digest_of(ch, NCHUNK, out);
    free(ch);
}

static void run_full(const op_t *op, int use_oracle) {
    uint32_t nitems = 1u << (32u - BLOCK_BITS);
    uint8_t (*b1)[32] = calloc(nitems, 32);
    fjob_t j1 = { op, use_oracle, b1 };
    stats_t st = pool_run(full_item, &j1, nitems, g_threads, 0);
    uint8_t a[32];
    full_digest(b1, a);
    char h[65];
    hex32(a, h);
    if (use_oracle) {
        stats_print(op, &st, "exhaustive");
        /* determinism: second pass, other thread count, reversed order */
        uint8_t (*b2)[32] = calloc(nitems, 32);
        fjob_t j2 = { op, 0, b2 };
        pool_run(full_item, &j2, nitems, g_threads > 3 ? g_threads - 3 : 1, 1);
        uint8_t b[32];
        full_digest(b2, b);
        if (memcmp(a, b, 32)) FAIL("%s: full-domain outputs differ between two runs", op->name);
        else printf("%-8s determinism: two full passes bit-identical\n", op->name);
        free(b2);
    }
    printf("%-8s full-domain digest %s\n", op->name, h);
    if (op->frozen[0]) {
        if (strcmp(h, op->frozen)) FAIL("%s: full-domain digest differs from the frozen digest", op->name);
        else printf("%-8s matches frozen digest\n", op->name);
    } else if (!use_oracle) {
        FAIL("%s: no frozen digest recorded", op->name);
    }
    free(b1);
}

/* ---- main ------------------------------------------------------------------------ */

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *mode = argc > 1 ? argv[1] : "fast";
    const char *only = argc > 2 ? argv[2] : NULL;
    const char *t = getenv("OMEGA_TRANSC_THREADS");
    if (t) g_threads = atoi(t);
    if (g_threads < 1) g_threads = 1;
    g_verbose = getenv("OMEGA_TRANSC_VERBOSE") != NULL;

    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    if (fpcr & OMEGA_NUMERIC_FPCR_REQUIRED_CLEAR) {
        printf("FAIL: FPCR 0x%llx has required-clear bits set (need RNE, no FZ/DN/AH)\n", (unsigned long long)fpcr);
        return 1;
    }
    printf("E1 WP-B transcendental CPU tier: mode %s, %d threads, FPCR 0x%llx\n", mode, g_threads, (unsigned long long)fpcr);

    int is_full = !strcmp(mode, "full"), is_digest = !strcmp(mode, "digest");
    if (!is_full && !is_digest && strcmp(mode, "fast")) { printf("usage: %s [fast|full|digest] [OP]\n", argv[0]); return 2; }

    if (!is_digest) {
        build_tables();
        oracle_selfcheck();
        special_values();
    }
    if (!is_full && !is_digest) build_sample();
    int ran = 0;
    for (int i = 0; i < NOPS; i++) {
        if (only && strcmp(only, g_ops[i].name)) continue;
        ran++;
        if (is_full) run_full(&g_ops[i], 1);
        else if (is_digest) run_full(&g_ops[i], 0);
        else run_fast(&g_ops[i]);
    }
    if (!ran) { printf("FAIL: unknown op %s\n", only); return 2; }
    printf("E1 WP-B verdict: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
