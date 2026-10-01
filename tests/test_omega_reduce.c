/*
 * E1 WP-D reduction tests (docs/numeric/E1_REDUCTION_CONTRACT.md).
 *
 *   -DOMEGA_NUMERIC_CPU_ONLY   host tiers only; RED_GB10_PARITY prints SKIP
 *                              (the one declared chip-only ID).
 *   chip build                 also runs the GB10 realization (SUM) and
 *                              compares it bit for bit with the reference.
 *
 * Prints "<ID>: PASS|FAIL|SKIP ..." per test and a final verdict line.
 * Exit 0 only when no test failed (and, on the chip build, nothing skipped).
 */
#include "omega_numeric.h"
#include "omega_numeric_reduce.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_skip;
static uint64_t g_rng = 0x9e3779b97f4a7c15ULL;

static uint64_t rng(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return g_rng;
}

static void verdict(const char *id, int ok, const char *detail) {
    printf("%s: %s%s%s\n", id, ok ? "PASS" : "FAIL", detail && *detail ? " " : "", detail ? detail : "");
    if (!ok) g_fail++;
}

static float fb(uint32_t u) { return omega_bits_to_float(u); }
static uint32_t bf(float f) { return omega_float_to_bits(f); }

/* ---- input generators ---------------------------------------------------- */

enum { DIST_BITS, DIST_MIXED, DIST_SUBNORMAL, DIST_SPECIAL, DIST_CANCEL, DIST_COUNT };
static const char *DIST_NAMES[DIST_COUNT] = { "bits", "mixed", "subnormal", "special", "cancel" };

static float gen(int dist) {
    uint64_t r = rng();
    switch (dist) {
    case DIST_BITS: return fb((uint32_t)r);
    case DIST_MIXED: { /* finite, exponent within +-20 of 1, random sign */
        uint32_t e = 127u - 20u + (uint32_t)((r >> 32) % 41u);
        return fb(((uint32_t)(r >> 63) << 31) | (e << 23) | ((uint32_t)r & 0x7fffffu));
    }
    case DIST_SUBNORMAL: { /* subnormals and tiny normals, both signs */
        uint32_t m = (uint32_t)r & ((r >> 40) & 1 ? 0x7fffffu : 0xffffffu);
        return fb(((uint32_t)(r >> 63) << 31) | m);
    }
    case DIST_SPECIAL: {
        static const uint32_t sp[] = { 0x00000000u, 0x80000000u, 0x7f800000u, 0xff800000u, 0x7fc00000u,
                                       0x7fa00001u, 0xffc00123u, 0x00000001u, 0x80000001u, 0x007fffffu,
                                       0x00800000u, 0x7f7fffffu, 0xff7fffffu, 0x3f800000u, 0xbf800000u };
        if ((r >> 32) % 4u == 0) return fb(sp[(r >> 8) % (sizeof(sp) / sizeof(sp[0]))]);
        uint32_t e = 127u - 30u + (uint32_t)((r >> 33) % 61u);
        return fb(((uint32_t)(r >> 63) << 31) | (e << 23) | ((uint32_t)r & 0x7fffffu));
    }
    default: { /* large values that cancel, plus small ones: order-sensitive */
        uint32_t e = (r >> 40) % 3u == 0 ? 127u + 24u + (uint32_t)((r >> 20) % 4u) : 127u + (uint32_t)((r >> 24) % 3u);
        return fb(((uint32_t)(r >> 63) << 31) | (e << 23) | ((uint32_t)r & 0x7fffffu));
    }
    }
}

/* ---- non-contract orders for the negative test --------------------------- */

/* Same recursion but lane deltas 1, 2, 4, 8, 16 (adjacent pairs first). */
static float mutant_reverse_deltas(const float *x, size_t n) {
    size_t len = n;
    float *cur = malloc(((n + 31) & ~(size_t)31) * sizeof(float));
    memcpy(cur, x, n * sizeof(float));
    do {
        size_t padded = (len + 31) & ~(size_t)31;
        for (size_t i = len; i < padded; i++) cur[i] = fb(0x80000000u);
        for (size_t j = 0; j < padded / 32; j++) {
            float v[32];
            memcpy(v, cur + 32 * j, sizeof(v));
            for (int d = 1; d <= 16; d <<= 1)
                for (int i = 0; i + d < 32; i++) v[i] = v[i] + v[i + d];
            cur[j] = v[0];
        }
        len = padded / 32;
    } while (len > 1);
    float r = cur[0];
    free(cur);
    return r;
}

/* Flat fold tree over the next power of two (lane i with i + N/2). */
static float mutant_flat_fold(const float *x, size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    float *v = malloc(p * sizeof(float));
    for (size_t i = 0; i < p; i++) v[i] = i < n ? x[i] : fb(0x80000000u);
    for (size_t d = p / 2; d >= 1; d /= 2)
        for (size_t i = 0; i < d; i++) v[i] = v[i] + v[i + d];
    float r = v[0];
    free(v);
    return r;
}

/* ---- tests ---------------------------------------------------------------- */

static void test_order_declared(void) {
    int ok = 1;
    char d[512] = "";
    if (strcmp(OMEGA_REDUCE_DECLARED_ORDER,
               "RECURSIVE_TILE32_PAIRWISE_TREE_LANE_DELTA_16_8_4_2_1_PAD_IDENTITY_MIN_ONE_LEVEL") != 0) ok = 0;
    if (!strstr(OMEGA_REDUCE_DECLARED_ORDER, OMEGA_WARP_REDUCTION_DECLARED_ORDER)) ok = 0;
    static const struct { size_t n; unsigned l; } lv[] = {
        { 0, 0 }, { 1, 1 }, { 32, 1 }, { 33, 2 }, { 1024, 2 }, { 1025, 3 }, { 32768, 3 }, { 32769, 4 },
        { 1000000, 4 }, { 1048576, 4 }, { 1048577, 5 } };
    for (size_t i = 0; i < sizeof(lv) / sizeof(lv[0]); i++)
        if (omega_reduce_levels(lv[i].n) != lv[i].l) { ok = 0; snprintf(d, sizeof(d), "levels(%zu)", lv[i].n); }
    /* n = 32 per tile equals the chip-proven REDUCE_SUM op (lane 0 of each warp) */
    {
        float w[256], wr[256];
        for (int i = 0; i < 256; i++) w[i] = gen(DIST_CANCEL);
        omega_numeric_reference(OMEGA_NOP_REDUCE_SUM, w, NULL, NULL, wr, 256);
        for (int t = 0; t < 8; t++) {
            float r = 0;
            omega_reduce_reference(OMEGA_RED_SUM, w + 32 * t, 32, &r);
            if (bf(r) != bf(wr[32 * t])) { ok = 0; snprintf(d, sizeof(d), "tile %d differs from REDUCE_SUM", t); }
        }
    }
    /* Hand-derived expected bits (contract doc, worked examples). */
    float a33[33], a32[32], a64[64];
    a33[0] = 16777216.0f; for (int i = 1; i < 33; i++) a33[i] = 1.0f;
    for (int i = 0; i < 32; i++) a32[i] = fb(0x80000000u);
    a32[0] = 16777216.0f; a32[1] = 1.0f; a32[17] = 1.0f;
    for (int i = 0; i < 64; i++) a64[i] = fb(0x80000000u);
    a64[0] = 16777216.0f; a64[1] = 1.0f; a64[33] = 1.0f;
    static const uint32_t expect[3] = { 0x4B800010u, 0x4B800001u, 0x4B800000u };
    const float *in[3] = { a33, a32, a64 };
    size_t ns[3] = { 33, 32, 64 };
    for (int k = 0; k < 3; k++) {
        float r = 0, c = 0;
        omega_reduce_reference(OMEGA_RED_SUM, in[k], ns[k], &r);
        omega_reduce_cpu(OMEGA_RED_SUM, in[k], ns[k], &c);
        if (bf(r) != expect[k] || bf(c) != expect[k]) {
            ok = 0;
            snprintf(d, sizeof(d), "worked example n=%zu ref=0x%08x cpu=0x%08x want 0x%08x", ns[k], bf(r), bf(c), expect[k]);
        }
    }
    char det[1024];
    snprintf(det, sizeof(det), "order=%s %s", OMEGA_REDUCE_DECLARED_ORDER, d);
    verdict("RED_ORDER_DECLARED", ok, det);
}

static int expect_bits(OmegaReduceOp op, const float *x, size_t n, uint32_t want, char *d, size_t dl) {
    float r = 0, c = 0;
    int rr = omega_reduce_reference(op, x, n, &r), rc = omega_reduce_cpu(op, x, n, &c);
    int okr = rr == OMEGA_NUMERIC_OK && omega_numeric_bits_equal(r, fb(want));
    int okc = rc == OMEGA_NUMERIC_OK && omega_numeric_bits_equal(c, fb(want));
    /* expect NaN class exactly when want is NaN */
    if (!okr || !okc) {
        snprintf(d, dl, "%s n=%zu ref=0x%08x cpu=0x%08x want 0x%08x", omega_reduce_op_name(op), n, bf(r), bf(c), want);
        return 0;
    }
    return 1;
}

static void test_empty_identity_special(void) {
    int ok = 1;
    char d[512] = "";
    float dummy = 0;
    ok &= expect_bits(OMEGA_RED_SUM, NULL, 0, 0x00000000u, d, sizeof(d));
    ok &= expect_bits(OMEGA_RED_MAX, NULL, 0, 0x7fc00000u, d, sizeof(d));
    ok &= expect_bits(OMEGA_RED_MIN, NULL, 0, 0x7fc00000u, d, sizeof(d));
    ok &= expect_bits(OMEGA_RED_MEAN, &dummy, 0, 0x7fc00000u, d, sizeof(d));
    float out;
    if (omega_reduce_reference(OMEGA_RED_SUM, NULL, 0, &out) || bf(out) != 0) ok = 0;   /* +0 exactly */
    /* identities are exact */
    static const uint32_t probe[] = { 0, 0x80000000u, 1, 0x80000001u, 0x7f7fffffu, 0x7f800000u, 0xff800000u, 0x3f800000u };
    for (size_t i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
        float x = fb(probe[i]);
        if (bf(x + omega_reduce_identity(OMEGA_RED_SUM)) != probe[i]) ok = 0;
        if (bf(omega_ref_fmax(x, omega_reduce_identity(OMEGA_RED_MAX))) != probe[i]) ok = 0;
        if (bf(omega_ref_fmin(omega_reduce_identity(OMEGA_RED_MIN), x)) != probe[i]) ok = 0;
    }
    static float v[2000];
    size_t lens[] = { 1, 33, 1025 };
    for (int k = 0; k < 3; k++) {          /* all -0 -> -0 ; one +0 among -0 -> +0 */
        for (size_t i = 0; i < lens[k]; i++) v[i] = fb(0x80000000u);
        ok &= expect_bits(OMEGA_RED_SUM, v, lens[k], 0x80000000u, d, sizeof(d));
        ok &= expect_bits(OMEGA_RED_MAX, v, lens[k], 0x80000000u, d, sizeof(d));
        v[lens[k] - 1] = 0.0f;
        ok &= expect_bits(OMEGA_RED_SUM, v, lens[k], 0x00000000u, d, sizeof(d));
        ok &= expect_bits(OMEGA_RED_MAX, v, lens[k], 0x00000000u, d, sizeof(d));
        ok &= expect_bits(OMEGA_RED_MIN, v, lens[k], lens[k] == 1 ? 0x00000000u : 0x80000000u, d, sizeof(d));
    }
    for (size_t i = 0; i < 100; i++) v[i] = fb(1);             /* 100 * 2^-149, exact */
    ok &= expect_bits(OMEGA_RED_SUM, v, 100, 100u, d, sizeof(d));
    ok &= expect_bits(OMEGA_RED_MEAN, v, 100, 1u, d, sizeof(d));
    for (size_t i = 0; i < 40; i++) v[i] = fb(0x7f7fffffu);    /* overflow -> +inf */
    ok &= expect_bits(OMEGA_RED_SUM, v, 40, 0x7f800000u, d, sizeof(d));
    ok &= expect_bits(OMEGA_RED_MAX, v, 40, 0x7f7fffffu, d, sizeof(d));
    for (size_t i = 0; i < 40; i++) v[i] = 1.0f;
    v[3] = fb(0x7f800000u); v[38] = fb(0xff800000u);            /* inf + -inf -> NaN */
    ok &= expect_bits(OMEGA_RED_SUM, v, 40, 0x7fc00000u, d, sizeof(d));
    ok &= expect_bits(OMEGA_RED_MAX, v, 40, 0x7f800000u, d, sizeof(d));
    ok &= expect_bits(OMEGA_RED_MIN, v, 40, 0xff800000u, d, sizeof(d));
    v[38] = 1.0f;
    ok &= expect_bits(OMEGA_RED_SUM, v, 40, 0x7f800000u, d, sizeof(d));
    for (size_t i = 0; i < 70; i++) v[i] = fb(i % 2 ? 0x7fa00001u : 0xffc00002u); /* all NaN (quiet + signaling) */
    ok &= expect_bits(OMEGA_RED_MAX, v, 70, 0x7fc00000u, d, sizeof(d));
    ok &= expect_bits(OMEGA_RED_MIN, v, 70, 0x7fc00000u, d, sizeof(d));
    ok &= expect_bits(OMEGA_RED_SUM, v, 70, 0x7fc00000u, d, sizeof(d));
    v[45] = -3.0f; v[2] = 7.5f;                                  /* NaN is missing data for MAX/MIN */
    ok &= expect_bits(OMEGA_RED_MAX, v, 70, bf(7.5f), d, sizeof(d));
    ok &= expect_bits(OMEGA_RED_MIN, v, 70, bf(-3.0f), d, sizeof(d));
    ok &= expect_bits(OMEGA_RED_SUM, v, 70, 0x7fc00000u, d, sizeof(d));
    v[0] = 3.0f;                                                 /* n=1: the element itself (NaN class kept) */
    ok &= expect_bits(OMEGA_RED_SUM, v, 1, bf(3.0f), d, sizeof(d));
    ok &= expect_bits(OMEGA_RED_MEAN, v, 1, bf(3.0f), d, sizeof(d));
    float t3[3] = { 1.0f, 1.0f, 1.0f };                          /* MEAN = SUM / n, correctly rounded */
    ok &= expect_bits(OMEGA_RED_MEAN, t3, 3, 0x3f800000u, d, sizeof(d));
    float t2[3] = { 1.0f, 0.0f, 0.0f };
    ok &= expect_bits(OMEGA_RED_MEAN, t2, 3, 0x3eaaaaabu, d, sizeof(d)); /* RNE(1/3) */
    if (omega_reduce_reference(OMEGA_RED_MEAN, v, (size_t)OMEGA_REDUCE_MEAN_MAX_N + 1, &out) != OMEGA_NUMERIC_ERR_OPERANDS ||
        omega_reduce_cpu(OMEGA_RED_MEAN, v, (size_t)OMEGA_REDUCE_MEAN_MAX_N + 1, &out) != OMEGA_NUMERIC_ERR_OPERANDS) {
        ok = 0; snprintf(d, sizeof(d), "MEAN n > 2^24 not refused");
    }
    if (omega_reduce_reference(OMEGA_RED_SUM, NULL, 5, &out) != OMEGA_NUMERIC_ERR_BAD_ARGS ||
        omega_reduce_cpu((OmegaReduceOp)9, v, 5, &out) != OMEGA_NUMERIC_ERR_BAD_ARGS) {
        ok = 0; snprintf(d, sizeof(d), "bad args not refused");
    }
    verdict("RED_EMPTY_IDENTITY_SPECIAL_VALUES", ok, d);
}

static const size_t NLIST[] = { 0, 1, 2, 3, 5, 7, 13, 31, 32, 33, 37, 63, 64, 65, 97, 127, 128, 255, 256, 257,
                                1000, 1021, 1023, 1024, 1025, 4093, 4096, 4097, 32767, 32768, 32769,
                                65535, 65536, 65537, 131071, 1000003, 1000000 };
#define NLIST_N (sizeof(NLIST) / sizeof(NLIST[0]))

static void test_cpu_equals_reference(void) {
    size_t cases = 0, mism = 0, elems = 0;
    char d[512] = "";
    float *x = malloc(1048576 * sizeof(float));
    size_t ns[NLIST_N + 24];
    size_t nn = 0;
    for (size_t i = 0; i < NLIST_N; i++) ns[nn++] = NLIST[i];
    for (int i = 0; i < 24; i++) ns[nn++] = 1 + (size_t)(rng() % 200000u);
    for (size_t k = 0; k < nn; k++) {
        size_t n = ns[k];
        int trials = n > 100000 ? 1 : 3;
        for (int dist = 0; dist < DIST_COUNT; dist++) {
            for (int t = 0; t < trials; t++) {
                for (size_t i = 0; i < n; i++) x[i] = gen(dist);
                for (int op = 0; op < OMEGA_RED_COUNT; op++) {
                    float r = 0, c = 0;
                    int rr = omega_reduce_reference((OmegaReduceOp)op, x, n, &r);
                    int rc = omega_reduce_cpu((OmegaReduceOp)op, x, n, &c);
                    cases++;
                    if (rr != OMEGA_NUMERIC_OK || rc != OMEGA_NUMERIC_OK || !omega_numeric_bits_equal(r, c)) {
                        if (!mism)
                            snprintf(d, sizeof(d), "first: %s n=%zu dist=%s ref=0x%08x cpu=0x%08x rc=%d/%d",
                                     omega_reduce_op_name((OmegaReduceOp)op), n, DIST_NAMES[dist], bf(r), bf(c), rr, rc);
                        mism++;
                    }
                }
                elems += n;
            }
        }
    }
    /* batched: [rows, n] with padding columns, each row equals the single reduce */
    size_t rows = 37, rn = 1000, stride = 1003;
    for (size_t i = 0; i < rows * stride; i++) x[i] = gen(DIST_CANCEL);
    for (int op = 0; op < OMEGA_RED_COUNT; op++) {
        float ro[37], co[37];
        if (omega_reduce_rows_reference((OmegaReduceOp)op, x, rows, rn, stride, ro) ||
            omega_reduce_rows_cpu((OmegaReduceOp)op, x, rows, rn, stride, co)) { mism++; continue; }
        for (size_t r = 0; r < rows; r++) {
            float s = 0;
            omega_reduce_reference((OmegaReduceOp)op, x + r * stride, rn, &s);
            cases++;
            if (!omega_numeric_bits_equal(ro[r], co[r]) || !omega_numeric_bits_equal(ro[r], s)) mism++;
        }
    }
    float tmp[2];
    if (omega_reduce_rows_cpu(OMEGA_RED_SUM, x, 2, 10, 9, tmp) != OMEGA_NUMERIC_ERR_OPERANDS) mism++;
    free(x);
    char det[1024];
    snprintf(det, sizeof(det), "cases=%zu elements=%zu mismatches=%zu %s", cases, elems, mism, d);
    verdict("RED_CPU_EQUALS_REFERENCE", mism == 0 && cases > 0, det);
}

/* Independent oracles: exact integer sums; MAX/MIN against a sequential scan
 * (both rules are order-independent); MEAN of exact sums. */
static void test_independent_oracle(void) {
    size_t cases = 0, bad = 0;
    char d[512] = "";
    float *x = malloc(1000003 * sizeof(float));
    for (size_t k = 0; k < NLIST_N; k++) {
        size_t n = NLIST[k];
        if (n == 0) continue;
        int64_t exact = 0;
        for (size_t i = 0; i < n; i++) { int v = (int)(rng() % 31u) - 15; x[i] = (float)v; exact += v; }
        /* |partial| <= 15 n <= 1.6e7 < 2^24: every partial sum is exact */
        float r = 0, c = 0;
        omega_reduce_reference(OMEGA_RED_SUM, x, n, &r);
        omega_reduce_cpu(OMEGA_RED_SUM, x, n, &c);
        float want = exact == 0 ? 0.0f : (float)exact;
        cases++;
        if (bf(r) != bf(want) || bf(c) != bf(want)) {
            bad++; snprintf(d, sizeof(d), "SUM n=%zu got 0x%08x want 0x%08x", n, bf(r), bf(want));
        }
        float m = 0;
        omega_reduce_reference(OMEGA_RED_MEAN, x, n, &m);
        float mc = 0;
        omega_reduce_cpu(OMEGA_RED_MEAN, x, n, &mc);
        cases++;
        if (!omega_numeric_bits_equal(m, omega_ieee_div(want, (float)n)) || !omega_numeric_bits_equal(mc, omega_ieee_div(want, (float)n))) { bad++; snprintf(d, sizeof(d), "MEAN n=%zu", n); }
        for (int dist = 0; dist < DIST_COUNT; dist++) {
            for (size_t i = 0; i < n; i++) x[i] = gen(dist);
            float smax = fb(0x7fc00000u), smin = fb(0x7fc00000u);
            for (size_t i = 0; i < n; i++) { smax = omega_ref_fmax(smax, x[i]); smin = omega_ref_fmin(smin, x[i]); }
            float a = 0, b = 0;
            omega_reduce_cpu(OMEGA_RED_MAX, x, n, &a);
            omega_reduce_cpu(OMEGA_RED_MIN, x, n, &b);
            float ra = 0, rb = 0;
            omega_reduce_reference(OMEGA_RED_MAX, x, n, &ra);
            omega_reduce_reference(OMEGA_RED_MIN, x, n, &rb);
            if (!omega_numeric_bits_equal(ra, smax) || !omega_numeric_bits_equal(rb, smin)) { bad++; snprintf(d, sizeof(d), "ref MAX/MIN n=%zu", n); }
            cases += 2;
            if (!omega_numeric_bits_equal(a, smax) || !omega_numeric_bits_equal(b, smin)) {
                bad++; snprintf(d, sizeof(d), "MAX/MIN n=%zu dist=%s", n, DIST_NAMES[dist]);
            }
        }
    }
    free(x);
    char det[1024];
    snprintf(det, sizeof(det), "cases=%zu bad=%zu %s", cases, bad, d);
    verdict("RED_INDEPENDENT_ORACLE", bad == 0, det);
}

static void test_different_order_caught(void) {
    int ok = 1;
    char d[512] = "";
    float a33[33], a32[32], a64[64];
    a33[0] = 16777216.0f; for (int i = 1; i < 33; i++) a33[i] = 1.0f;
    for (int i = 0; i < 32; i++) a32[i] = fb(0x80000000u);
    a32[0] = 16777216.0f; a32[1] = 1.0f; a32[17] = 1.0f;
    for (int i = 0; i < 64; i++) a64[i] = fb(0x80000000u);
    a64[0] = 16777216.0f; a64[1] = 1.0f; a64[33] = 1.0f;
    float r33, r32, r64;
    omega_reduce_reference(OMEGA_RED_SUM, a33, 33, &r33);
    omega_reduce_reference(OMEGA_RED_SUM, a32, 32, &r32);
    omega_reduce_reference(OMEGA_RED_SUM, a64, 64, &r64);
    float c33, c32, c64; /* the CPU tier must match the declared bits, not the mutants */
    omega_reduce_cpu(OMEGA_RED_SUM, a33, 33, &c33);
    omega_reduce_cpu(OMEGA_RED_SUM, a32, 32, &c32);
    omega_reduce_cpu(OMEGA_RED_SUM, a64, 64, &c64);
    if (bf(c33) != bf(r33) || bf(c32) != bf(r32) || bf(c64) != bf(r64)) { ok = 0; snprintf(d, sizeof(d), "CPU tier differs on crafted inputs"); }
    float s33 = omega_reduce_sequential_sum_not_contract(a33, 33);   /* 0x4B800000 */
    float m32 = mutant_reverse_deltas(a32, 32);                       /* 0x4B800000 */
    float f64 = mutant_flat_fold(a64, 64);                            /* 0x4B800001 */
    if (omega_numeric_bits_equal(r33, s33) || bf(s33) != 0x4B800000u) { ok = 0; snprintf(d, sizeof(d), "sequential not caught"); }
    if (omega_numeric_bits_equal(r32, m32) || bf(m32) != 0x4B800000u) { ok = 0; snprintf(d, sizeof(d), "reverse deltas not caught"); }
    if (omega_numeric_bits_equal(r64, f64) || bf(f64) != 0x4B800001u) { ok = 0; snprintf(d, sizeof(d), "flat fold not caught"); }
    /* on random order-sensitive data the sequential order disagrees often */
    float *x = malloc(4097 * sizeof(float));
    int caught = 0, trials = 50;
    for (int t = 0; t < trials; t++) {
        for (int i = 0; i < 4097; i++) x[i] = gen(DIST_CANCEL);
        float r;
        omega_reduce_reference(OMEGA_RED_SUM, x, 4097, &r);
        float c;
        omega_reduce_cpu(OMEGA_RED_SUM, x, 4097, &c);
        if (bf(c) != bf(r)) { ok = 0; snprintf(d, sizeof(d), "CPU tier differs on random input"); }
        if (!omega_numeric_bits_equal(r, omega_reduce_sequential_sum_not_contract(x, 4097))) caught++;
    }
    free(x);
    if (caught < trials / 2) { ok = 0; snprintf(d, sizeof(d), "sequential differs in only %d/%d random cases", caught, trials); }
    char det[1024];
    snprintf(det, sizeof(det), "sequential/reverse/flat mutants all differ on crafted inputs; random n=4097 sequential differs %d/%d %s",
             caught, trials, d);
    verdict("RED_DIFFERENT_ORDER_CAUGHT", ok, det);
}

static void test_determinism(void) {
    int ok = 1;
    size_t n = 1000003;
    float *x = malloc(n * sizeof(float)), *y = malloc((n + 1) * sizeof(float));
    for (size_t i = 0; i < n; i++) x[i] = gen(DIST_CANCEL);
    memcpy(y + 1, x, n * sizeof(float)); /* different alignment, same values */
    for (int op = 0; op < OMEGA_RED_COUNT; op++) {
        float r[4];
        omega_reduce_reference((OmegaReduceOp)op, x, n, &r[0]);
        omega_reduce_reference((OmegaReduceOp)op, y + 1, n, &r[1]);
        omega_reduce_cpu((OmegaReduceOp)op, x, n, &r[2]);
        omega_reduce_cpu((OmegaReduceOp)op, y + 1, n, &r[3]);
        for (int k = 1; k < 4; k++) if (bf(r[k]) != bf(r[0])) ok = 0;
    }
    free(x); free(y);
    verdict("RED_DETERMINISM", ok, "n=1000003, 4 ops, two runs, two alignments, identical bits");
}

static void test_gb10_presubmit(void) {
    int ok = 1;
    char err[320], d[512] = "";
    float v[64] = { 0 }, o = 0;
    if (omega_reduce_gb10_check(OMEGA_RED_MAX, v, 64, &o, err, sizeof(err)) != OMEGA_NUMERIC_ERR_NOT_ENCODED) ok = 0;
    if (omega_reduce_gb10_check(OMEGA_RED_MIN, v, 64, &o, err, sizeof(err)) != OMEGA_NUMERIC_ERR_NOT_ENCODED) ok = 0;
    if (omega_reduce_gb10_check(OMEGA_RED_MEAN, v, 64, &o, err, sizeof(err)) != OMEGA_NUMERIC_ERR_NOT_ENCODED) ok = 0;
    if (omega_reduce_gb10_check((OmegaReduceOp)7, v, 64, &o, err, sizeof(err)) != OMEGA_NUMERIC_ERR_BAD_ARGS) ok = 0;
    if (omega_reduce_gb10_check(OMEGA_RED_SUM, NULL, 64, &o, err, sizeof(err)) != OMEGA_NUMERIC_ERR_BAD_ARGS) ok = 0;
    if (omega_reduce_gb10_check(OMEGA_RED_SUM, v, 64, NULL, err, sizeof(err)) != OMEGA_NUMERIC_ERR_BAD_ARGS) ok = 0;
    if (omega_reduce_gb10_check(OMEGA_RED_SUM, v, OMEGA_REDUCE_GB10_MAX_N + 1, &o, err, sizeof(err)) != OMEGA_NUMERIC_ERR_OPERANDS) ok = 0;
    if (!ok) snprintf(d, sizeof(d), "a refusal is missing");
    for (size_t k = 0; k < NLIST_N; k++)
        if (omega_reduce_gb10_check(OMEGA_RED_SUM, v, NLIST[k], &o, err, sizeof(err)) != OMEGA_NUMERIC_OK) {
            ok = 0; snprintf(d, sizeof(d), "n=%zu refused: %s", NLIST[k], err);
        }
    verdict("RED_GB10_PRESUBMIT_CHECKS", ok, d);
}

static void test_gb10_parity(void) {
#ifdef OMEGA_NUMERIC_CPU_ONLY
    printf("RED_GB10_PARITY: SKIP chip-only (CPU build)\n");
    g_skip++;
#else
    static const size_t gn[] = { 0, 1, 2, 31, 32, 33, 63, 64, 65, 97, 1000, 1024, 1025, 4097,
                                 65536, 65537, 1000003, 1000000 };
    size_t cases = 0, mism = 0, launches = 0;
    char d[512] = "";
    float *x = malloc(1000003 * sizeof(float));
    for (size_t k = 0; k < sizeof(gn) / sizeof(gn[0]); k++) {
        size_t n = gn[k];
        int dists[3] = { DIST_CANCEL, DIST_SPECIAL, DIST_SUBNORMAL };
        for (int q = 0; q < (n > 70000 ? 1 : 3); q++) {
            for (size_t i = 0; i < n; i++) x[i] = gen(dists[q]);
            float r = 0, g = 0;
            omega_reduce_reference(OMEGA_RED_SUM, x, n, &r);
            int rc = omega_reduce_gb10(OMEGA_RED_SUM, x, n, &g);
            launches += omega_reduce_gb10_last_launches();
            cases++;
            printf("RED_GB10_CASE n=%zu dist=%s ref=0x%08x gb10=0x%08x launches=%u rc=%d\n", n, DIST_NAMES[dists[q]],
                   bf(r), bf(g), omega_reduce_gb10_last_launches(), rc);
            if (rc != OMEGA_NUMERIC_OK || !omega_numeric_bits_equal(r, g)) {
                if (!mism) snprintf(d, sizeof(d), "first: n=%zu dist=%s ref=0x%08x gb10=0x%08x rc=%d", n,
                                    DIST_NAMES[dists[q]], bf(r), bf(g), rc);
                mism++;
            }
        }
    }
    /* crafted special-value vectors on chip, expected bits written out */
    {
        static float v[65537];
        struct { size_t n; uint32_t fill, s0, s1; size_t i1; uint32_t want; } sv[] = {
            { 33, 0x80000000u, 0x80000000u, 0x80000000u, 1, 0x80000000u },     /* all -0 -> -0        */
            { 65537, 0x80000000u, 0x80000000u, 0x80000000u, 1, 0x80000000u },  /* all -0, 4 levels    */
            { 1025, 0x80000000u, 0x80000000u, 0x00000000u, 1024, 0x00000000u },/* one +0 -> +0        */
            { 40, 0x7f7fffffu, 0x7f7fffffu, 0x7f7fffffu, 1, 0x7f800000u },     /* overflow -> +inf    */
            { 40, 0x3f800000u, 0x7f800000u, 0xff800000u, 38, 0x7fc00000u },    /* inf + -inf -> NaN   */
            { 40, 0x3f800000u, 0xff800000u, 0x3f800000u, 38, 0xff800000u },    /* -inf                */
            { 100, 0x00000001u, 0x00000001u, 0x00000001u, 1, 0x00000064u },    /* 100 * 2^-149 exact  */
        };
        for (size_t k = 0; k < sizeof(sv) / sizeof(sv[0]); k++) {
            for (size_t i = 0; i < sv[k].n; i++) v[i] = fb(sv[k].fill);
            v[0] = fb(sv[k].s0);
            v[sv[k].i1] = fb(sv[k].s1);
            float r = 0, g = 0;
            omega_reduce_reference(OMEGA_RED_SUM, v, sv[k].n, &r);
            int rc2 = omega_reduce_gb10(OMEGA_RED_SUM, v, sv[k].n, &g);
            cases++;
            launches += omega_reduce_gb10_last_launches();
            printf("RED_GB10_CASE n=%zu dist=crafted%zu ref=0x%08x gb10=0x%08x want=0x%08x launches=%u rc=%d\n", sv[k].n, k,
                   bf(r), bf(g), sv[k].want, omega_reduce_gb10_last_launches(), rc2);
            if (rc2 != OMEGA_NUMERIC_OK || !omega_numeric_bits_equal(g, fb(sv[k].want)) || !omega_numeric_bits_equal(r, g)) {
                if (!mism) snprintf(d, sizeof(d), "crafted %zu n=%zu gb10=0x%08x want 0x%08x", k, sv[k].n, bf(g), sv[k].want);
                mism++;
            }
        }
    }
    /* the three hand-derived worked examples on chip */
    {
        float a33[33], a32[32], a64[64];
        a33[0] = 16777216.0f; for (int i = 1; i < 33; i++) a33[i] = 1.0f;
        for (int i = 0; i < 32; i++) a32[i] = fb(0x80000000u);
        a32[0] = 16777216.0f; a32[1] = 1.0f; a32[17] = 1.0f;
        for (int i = 0; i < 64; i++) a64[i] = fb(0x80000000u);
        a64[0] = 16777216.0f; a64[1] = 1.0f; a64[33] = 1.0f;
        const float *in[3] = { a33, a32, a64 };
        static const size_t wn[3] = { 33, 32, 64 };
        static const uint32_t want[3] = { 0x4B800010u, 0x4B800001u, 0x4B800000u };
        for (int k = 0; k < 3; k++) {
            float g = 0;
            int rc = omega_reduce_gb10(OMEGA_RED_SUM, in[k], wn[k], &g);
            cases++;
            launches += omega_reduce_gb10_last_launches();
            printf("RED_GB10_CASE n=%zu dist=worked%d gb10=0x%08x want=0x%08x rc=%d\n", wn[k], k, bf(g), want[k], rc);
            if (rc != OMEGA_NUMERIC_OK || bf(g) != want[k]) {
                mism++; snprintf(d, sizeof(d), "worked example n=%zu gb10=0x%08x want 0x%08x", wn[k], bf(g), want[k]);
            }
        }
    }
    free(x);
    char det[1024];
    snprintf(det, sizeof(det), "op=SUM order=%s cases=%zu launches=%zu mismatches=%zu %s",
             OMEGA_REDUCE_DECLARED_ORDER, cases, launches, mism, d);
    verdict("RED_GB10_PARITY", mism == 0 && cases > 0, det);
#endif
}

int main(void) {
    if (!omega_numeric_fpenv_ok()) {
        printf("FPENV: FAIL host FPCR is not RNE / no FTZ\n");
        return 1;
    }
    printf("RED_RUN seed=0x%016" PRIx64 " order=%s\n", g_rng, OMEGA_REDUCE_DECLARED_ORDER);
    test_order_declared();
    test_empty_identity_special();
    test_cpu_equals_reference();
    test_independent_oracle();
    test_different_order_caught();
    test_determinism();
    test_gb10_presubmit();
    test_gb10_parity();
#ifdef OMEGA_NUMERIC_CPU_ONLY
    int ok = g_fail == 0 && g_skip == 1;
    printf("E1 Reduce Verdict: %s\n", ok ? "PASS_EXCEPT_DECLARED_CHIP_ONLY" : "FAIL");
#else
    int ok = g_fail == 0 && g_skip == 0;
    printf("E1 Reduce Verdict: %s\n", ok ? "PASS" : "FAIL");
#endif
    return ok ? 0 : 1;
}
