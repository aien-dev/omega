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
#include "omega_blackwell_qmd.h"

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
    /* overflow: (rows-1)*row_stride + n must not wrap size_t (Codex P2) */
    if (omega_reduce_rows_reference(OMEGA_RED_SUM, x, 3, 10, SIZE_MAX / 2, tmp) != OMEGA_NUMERIC_ERR_OPERANDS) mism++;
    if (omega_reduce_rows_cpu(OMEGA_RED_SUM, x, 3, 10, SIZE_MAX / 2, tmp) != OMEGA_NUMERIC_ERR_OPERANDS) mism++;
    if (omega_reduce_rows_cpu(OMEGA_RED_SUM, x, 2, 10, SIZE_MAX / sizeof(float), tmp) != OMEGA_NUMERIC_ERR_OPERANDS) mism++;
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
    if (omega_reduce_gb10_check((OmegaReduceOp)7, v, 64, &o, err, sizeof(err)) != OMEGA_NUMERIC_ERR_BAD_ARGS) ok = 0;
    for (int op = 0; op < OMEGA_RED_COUNT; op++) {
        if (omega_reduce_gb10_check((OmegaReduceOp)op, NULL, 64, &o, err, sizeof(err)) != OMEGA_NUMERIC_ERR_BAD_ARGS) ok = 0;
        if (omega_reduce_gb10_check((OmegaReduceOp)op, v, 64, NULL, err, sizeof(err)) != OMEGA_NUMERIC_ERR_BAD_ARGS) ok = 0;
        if (omega_reduce_gb10_check((OmegaReduceOp)op, v, OMEGA_REDUCE_GB10_MAX_N + 1, &o, err, sizeof(err)) != OMEGA_NUMERIC_ERR_OPERANDS) ok = 0;
    }
    if (omega_reduce_gb10_check(OMEGA_RED_MEAN, v, (size_t)OMEGA_REDUCE_MEAN_MAX_N + 1, &o, err, sizeof(err)) != OMEGA_NUMERIC_ERR_OPERANDS) ok = 0;
    if (omega_reduce_gb10_check(OMEGA_RED_MAX, v, (size_t)OMEGA_REDUCE_MEAN_MAX_N + 1, &o, err, sizeof(err)) != OMEGA_NUMERIC_OK) ok = 0;
    if (!ok) snprintf(d, sizeof(d), "a refusal or acceptance is wrong");
    for (int op = 0; op < OMEGA_RED_COUNT; op++)
        for (size_t k = 0; k < NLIST_N; k++)
            if (omega_reduce_gb10_check((OmegaReduceOp)op, v, NLIST[k], &o, err, sizeof(err)) != OMEGA_NUMERIC_OK) {
                ok = 0; snprintf(d, sizeof(d), "%s n=%zu refused: %s", omega_reduce_op_name((OmegaReduceOp)op), NLIST[k], err);
            }
    verdict("RED_GB10_PRESUBMIT_CHECKS", ok, d);
}

/* Crafted special-value vectors (fill, x[0] = s0, x[i1] = s1) with hand-written
 * expected bits per op; checked on both CPU tiers (RED_CRAFTED_TABLE) and on chip. */
struct crafted { size_t n; uint32_t fill, s0, s1; size_t i1; uint32_t want[4]; };
static const struct crafted CRAFTED[] = {
            /*                                                     SUM          MAX          MIN          MEAN */
            { 33, 0x80000000u, 0x80000000u, 0x80000000u, 1,      { 0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u } }, /* all -0          */
            { 65537, 0x80000000u, 0x80000000u, 0x80000000u, 1,   { 0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u } }, /* all -0, 4 levels */
            { 1025, 0x80000000u, 0x80000000u, 0x00000000u, 1024, { 0x00000000u, 0x00000000u, 0x80000000u, 0x00000000u } }, /* one +0 in -0    */
            { 1025, 0x00000000u, 0x00000000u, 0x80000000u, 1024, { 0x00000000u, 0x00000000u, 0x80000000u, 0x00000000u } }, /* one -0 in +0    */
            { 40, 0x7f7fffffu, 0x7f7fffffu, 0x7f7fffffu, 1,      { 0x7f800000u, 0x7f7fffffu, 0x7f7fffffu, 0x7f800000u } }, /* overflow        */
            { 40, 0x3f800000u, 0x7f800000u, 0xff800000u, 38,     { 0x7fc00000u, 0x7f800000u, 0xff800000u, 0x7fc00000u } }, /* inf and -inf    */
            { 40, 0x3f800000u, 0xff800000u, 0x3f800000u, 38,     { 0xff800000u, 0x3f800000u, 0xff800000u, 0xff800000u } }, /* -inf            */
            { 100, 0x00000001u, 0x00000001u, 0x00000001u, 1,     { 0x00000064u, 0x00000001u, 0x00000001u, 0x00000001u } }, /* subnormal sum   */
            { 100, 0x00000001u, 0x00000001u, 0x80000002u, 50,    { 0x00000061u, 0x00000001u, 0x80000002u, 0x00000001u } }, /* subnormal signs */
            { 33, 0x7fc00000u, 0x7fc00000u, 0x7fc00000u, 1,      { 0x7fc00000u, 0x7fc00000u, 0x7fc00000u, 0x7fc00000u } }, /* all NaN         */
            { 1025, 0x7fc00000u, 0x7fc00000u, 0x40a00000u, 1000, { 0x7fc00000u, 0x40a00000u, 0x40a00000u, 0x7fc00000u } }, /* one 5.0 in NaN  */
            { 33, 0x7fa00001u, 0x7fa00001u, 0xc0400000u, 17,     { 0x7fc00000u, 0xc0400000u, 0xc0400000u, 0x7fc00000u } }, /* one -3 in sNaN  */
            { 65, 0xffc00123u, 0xffc00123u, 0xff800000u, 64,     { 0x7fc00000u, 0xff800000u, 0xff800000u, 0x7fc00000u } }, /* -inf in -NaN    */
            { 1024, 0x3f800000u, 0x7f7fffffu, 0xff7fffffu, 1023, { 0x00000000u, 0x7f7fffffu, 0xff7fffffu, 0x00000000u } }, /* +-max cancel    */
};
#define CRAFTED_N (sizeof(CRAFTED) / sizeof(CRAFTED[0]))

static void test_crafted_table(void) {
    static float v[65537];
    char d[512] = "";
    size_t bad = 0;
    for (int op = 0; op < OMEGA_RED_COUNT; op++)
        for (size_t k = 0; k < CRAFTED_N; k++) {
            for (size_t i = 0; i < CRAFTED[k].n; i++) v[i] = fb(CRAFTED[k].fill);
            v[0] = fb(CRAFTED[k].s0);
            v[CRAFTED[k].i1] = fb(CRAFTED[k].s1);
            if (!expect_bits((OmegaReduceOp)op, v, CRAFTED[k].n, CRAFTED[k].want[op], d, sizeof(d))) bad++;
        }
    char det[700];
    snprintf(det, sizeof(det), "%zu vectors x 4 ops, hand-written bits on reference and CPU tiers, bad=%zu %s", CRAFTED_N, bad, d);
    verdict("RED_CRAFTED_TABLE", bad == 0, det);
}

/* ---- MAX/MIN warp patch: host model of the decoded words and mutations ---- */

/* Runs a patch on 32 lanes from its decoded words: SHFL.DOWN by the delta in
 * the word (clamp: a lane past 31 reads its own value), FMNMX by the
 * predicate bit of the word (PT = min, !PT = max), register numbers from the
 * words. Returns lane 0 of the register the STG stores. */
static float model_patch_tile(const OmegaNumericPatchInsn *p, int n, const float tile[32]) {
    float r[32][16];
    for (int i = 0; i < 32; i++) for (int k = 0; k < 16; k++) r[i][k] = 0;
    for (int i = 0; i < 32; i++) r[i][2] = tile[i];
    for (int t = 0; t < n; t++) {
        uint32_t o = p[t].w[0] & 0xffffu, dst = (p[t].w[0] >> 16) & 0xfu, sa = (p[t].w[0] >> 24) & 0xfu, sb = p[t].w[1] & 0xfu;
        if (o == 0x7f89u) {
            uint32_t dl = (p[t].w[1] >> 21) & 0x1fu;
            float nv[32];
            for (uint32_t i = 0; i < 32; i++) nv[i] = r[(i + dl) < 32 ? i + dl : i][sa];
            for (int i = 0; i < 32; i++) r[i][dst] = nv[i];
        } else if (o == 0x7209u) {
            int mx = ((p[t].w[2] >> 26) & 1u) != 0; /* !PT */
            for (int i = 0; i < 32; i++) r[i][dst] = mx ? omega_ref_fmax(r[i][sa], r[i][sb]) : omega_ref_fmin(r[i][sa], r[i][sb]);
        } else if (o == 0x7986u) {
            return r[0][p[t].w[1] & 0xfu];
        }
    }
    return fb(0xdeadbeefu);
}

/* The host level loop with a given patch model and pad, as the chip runs it. */
static float model_levels(const OmegaNumericPatchInsn *p, int np, uint32_t pad, const float *x, size_t n, float *buf) {
    memcpy(buf, x, n * sizeof(float));
    size_t len = n;
    do {
        size_t padded = (len + 31) & ~(size_t)31;
        for (size_t i = len; i < padded; i++) buf[i] = fb(pad);
        for (size_t j = 0; j < padded / 32; j++) buf[j] = model_patch_tile(p, np, buf + 32 * j);
        len = padded / 32;
    } while (len > 1);
    return buf[0];
}

static void test_gb10_minmax_patch(void) {
    int ok = 1;
    char d[640] = "", err[320];
    uint32_t qmd1[OMEGA_BW_QMD_WORDS];
    OmegaBlackwellQmdConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.num_elements = 64;
    omega_numeric_launch_shape(64, &cfg.threads_per_block, &cfg.grid_width);
    if (omega_blackwell_build_qmd1(qmd1, &cfg) != 0) { verdict("RED_GB10_MINMAX_PATCH", 0, "qmd"); return; }
    size_t mutants = 0, caught = 0, model_cases = 0, model_bad = 0;
    float *x = malloc(40000 * sizeof(float)), *buf = malloc(40000 * sizeof(float));
    for (int op = OMEGA_RED_MAX; op <= OMEGA_RED_MIN; op++) {
        OmegaReduceOp rop = (OmegaReduceOp)op;
        OmegaNumericPatchInsn p[OMEGA_NUMERIC_PATCH_MAX], m[OMEGA_NUMERIC_PATCH_MAX];
        int np = omega_reduce_gb10_minmax_patch(rop, p);
        if (np != 12 || omega_reduce_gb10_check_minmax_patch(rop, p, np, qmd1, err, sizeof(err)) != OMEGA_NUMERIC_OK) {
            ok = 0; snprintf(d, sizeof(d), "%s patch refused: %s", omega_reduce_op_name(rop), err);
        }
        /* each mutation of the order, the combine or the schedule is refused */
        for (int k = 0; k < 11; k++) {
            memcpy(m, p, sizeof(p));
            switch (k) {
            case 0: for (int s = 0; s < 5; s++) m[2 * s].w[1] = p[2 * (4 - s)].w[1]; break; /* deltas 1,2,4,8,16 */
            case 1: m[0].w[1] = p[2].w[1]; m[2].w[1] = p[0].w[1]; break;                   /* 8,16,4,2,1        */
            case 2: m[4].w[1] = (p[4].w[1] & ~(0x1fu << 21)) | (3u << 21); break;          /* delta 3           */
            case 3: m[3].w[2] ^= 0x04000000u; break;                                       /* min <-> max       */
            case 4: m[1].w[0] = (p[1].w[0] & 0x00ffffffu) | (9u << 24); m[1].w[1] = 2u; break; /* operands swapped */
            case 5: m[9].w[0] = (p[9].w[0] & 0xff00ffffu) | (2u << 16); break;            /* last dst R2       */
            case 6: m[5].w[3] &= ~(0x3fu << 20); break;                                    /* no wait on SHFL   */
            case 7: m[6].w[1] = (p[6].w[1] & ~(0x1fu << 8)) | (0x0fu << 8); break;         /* clamp 0x0f        */
            case 8: m[7].w[3] = (p[7].w[3] & ~(0xfu << 9)) | (2u << 9); break;             /* stall 2           */
            case 9: m[1].w[1] |= 0x100u; break;                                            /* modifier bit      */
            default: memcpy(&m[2], &p[4], 2 * sizeof(p[0])); memcpy(&m[4], &p[2], 2 * sizeof(p[0])); break; /* pairs swapped */
            }
            mutants++;
            if (omega_reduce_gb10_check_minmax_patch(rop, m, np, qmd1, err, sizeof(err)) != OMEGA_NUMERIC_OK) caught++;
            else if (strlen(d) < 400) snprintf(d + strlen(d), sizeof(d) - strlen(d), " %s mutant %d not caught;", omega_reduce_op_name(rop), k);
        }
        mutants++; /* the other op's patch under this op */
        omega_reduce_gb10_minmax_patch(rop == OMEGA_RED_MAX ? OMEGA_RED_MIN : OMEGA_RED_MAX, m);
        if (omega_reduce_gb10_check_minmax_patch(rop, m, np, qmd1, err, sizeof(err)) != OMEGA_NUMERIC_OK) caught++;
        /* the decoded patch, run by the host model with the declared pad, equals the reference */
        static const size_t mn[] = { 1, 2, 31, 32, 33, 64, 97, 1023, 1024, 1025, 33000 };
        for (size_t a = 0; a < sizeof(mn) / sizeof(mn[0]); a++)
            for (int dist = 0; dist < DIST_COUNT; dist++) {
                for (size_t i = 0; i < mn[a]; i++) x[i] = gen(dist);
                float r, g = model_levels(p, np, OMEGA_REDUCE_MINMAX_PAD_BITS, x, mn[a], buf);
                omega_reduce_reference(rop, x, mn[a], &r);
                model_cases++;
                if (!omega_numeric_bits_equal(r, g)) model_bad++;
            }
    }
    free(x); free(buf);
    if (model_bad) ok = 0;
    if (caught != mutants) ok = 0;
    char det[1024];
    snprintf(det, sizeof(det), "patch accepted; mutants caught %zu/%zu; host model of decoded words = reference %zu/%zu%s",
             caught, mutants, model_cases - model_bad, model_cases, d);
    verdict("RED_GB10_MINMAX_PATCH", ok, det);
}

/* Wrong padding must be caught: the same level loop with -inf, +inf or +0 as
 * pad gives different bits from the reference on crafted inputs. */
static void test_gb10_wrong_pad_caught(void) {
    int ok = 1;
    char d[512] = "";
    OmegaNumericPatchInsn pmax[OMEGA_NUMERIC_PATCH_MAX], pmin[OMEGA_NUMERIC_PATCH_MAX];
    int nmax = omega_reduce_gb10_minmax_patch(OMEGA_RED_MAX, pmax), nmin = omega_reduce_gb10_minmax_patch(OMEGA_RED_MIN, pmin);
    float x[33], buf[64], r;
    /* all NaN: declared pad keeps NaN; -inf pad (MAX) or +inf pad (MIN) does not */
    for (int i = 0; i < 33; i++) x[i] = fb(0x7fc00000u);
    omega_reduce_reference(OMEGA_RED_MAX, x, 33, &r);
    if (!omega_numeric_bits_equal(r, model_levels(pmax, nmax, OMEGA_REDUCE_MINMAX_PAD_BITS, x, 33, buf))) { ok = 0; snprintf(d, sizeof(d), "declared pad differs"); }
    if (omega_numeric_bits_equal(r, model_levels(pmax, nmax, 0xff800000u, x, 33, buf))) { ok = 0; snprintf(d, sizeof(d), "-inf pad (MAX) not caught"); }
    if (omega_numeric_bits_equal(r, model_levels(pmin, nmin, 0x7f800000u, x, 33, buf))) { ok = 0; snprintf(d, sizeof(d), "+inf pad (MIN) not caught"); }
    /* all negative: +0 pad wins MAX; all positive: -0 pad wins MIN */
    for (int i = 0; i < 33; i++) x[i] = -2.0f;
    omega_reduce_reference(OMEGA_RED_MAX, x, 33, &r);
    if (bf(r) != 0xc0000000u || omega_numeric_bits_equal(r, model_levels(pmax, nmax, 0x00000000u, x, 33, buf))) { ok = 0; snprintf(d, sizeof(d), "+0 pad (MAX) not caught"); }
    for (int i = 0; i < 33; i++) x[i] = 2.0f;
    omega_reduce_reference(OMEGA_RED_MIN, x, 33, &r);
    if (bf(r) != 0x40000000u || omega_numeric_bits_equal(r, model_levels(pmin, nmin, 0x80000000u, x, 33, buf))) { ok = 0; snprintf(d, sizeof(d), "-0 pad (MIN) not caught"); }
    /* the GB10 path refuses to run with a pad that is not the declared identity:
     * it pads with omega_reduce_identity and checks those bits (CHECK:red_mm_pad_identity) */
    if (bf(omega_reduce_identity(OMEGA_RED_MAX)) != OMEGA_REDUCE_MINMAX_PAD_BITS || bf(omega_reduce_identity(OMEGA_RED_MIN)) != OMEGA_REDUCE_MINMAX_PAD_BITS ||
        bf(omega_reduce_identity(OMEGA_RED_MEAN)) != OMEGA_REDUCE_SUM_PAD_BITS) { ok = 0; snprintf(d, sizeof(d), "identity bits changed"); }
    /* MEAN order: the n = 33 worked example divided by 33, hand-derived bits.
     * (2^24 + 32) / 33 = 508401.4545..., ulp 2^-5 -> 508401.46875 = 0x48F83E2F;
     * left-to-right gives 2^24 / 33 -> 0x48F83E1F. */
    float a33[33];
    a33[0] = 16777216.0f; for (int i = 1; i < 33; i++) a33[i] = 1.0f;
    float mr, mc;
    omega_reduce_reference(OMEGA_RED_MEAN, a33, 33, &mr);
    omega_reduce_cpu(OMEGA_RED_MEAN, a33, 33, &mc);
    float seq = omega_math_div(omega_reduce_sequential_sum_not_contract(a33, 33), 33.0f);
    if (bf(mr) != 0x48F83E2Fu || bf(mc) != 0x48F83E2Fu || bf(seq) == bf(mr)) { ok = 0; snprintf(d, sizeof(d), "MEAN worked example ref=0x%08x cpu=0x%08x seq=0x%08x", bf(mr), bf(mc), bf(seq)); }
    verdict("RED_WRONG_PAD_CAUGHT", ok, d);
}

/* --dump DIR: MAX/MIN patch words and their expected text for nvdisasm. */
static int dump_patches(const char *dir) {
    for (int op = OMEGA_RED_MAX; op <= OMEGA_RED_MIN; op++) {
        OmegaNumericPatchInsn p[OMEGA_NUMERIC_PATCH_MAX];
        int np = omega_reduce_gb10_minmax_patch((OmegaReduceOp)op, p);
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.bin", dir, op == OMEGA_RED_MAX ? "max" : "min");
        FILE *fb_ = fopen(path, "wb");
        snprintf(path, sizeof(path), "%s/%s.lst", dir, op == OMEGA_RED_MAX ? "max" : "min");
        FILE *fl = fopen(path, "w");
        if (!fb_ || !fl || np <= 0) { if (fb_) fclose(fb_); if (fl) fclose(fl); return 1; }
        for (int i = 0; i < np; i++) {
            fwrite(p[i].w, 4, 4, fb_);
            fprintf(fl, "%04x %s ;\n", i * 16, p[i].text);
        }
        fclose(fb_); fclose(fl);
    }
    return 0;
}

#ifndef OMEGA_NUMERIC_CPU_ONLY
static int g_nan_bits_seen[4];
static uint32_t g_nan_bits[4];

/* One chip case: compare with the reference (bit for bit; NaN results compare
 * as the NaN class, the contract's parity rule) and with want when given. */
static void chip_case(OmegaReduceOp op, const float *x, size_t n, const char *tag, int has_want, uint32_t want,
                      size_t *cases, size_t *mism, size_t *launches, char *d, size_t dl) {
    float r = 0, g = 0;
    omega_reduce_reference(op, x, n, &r);
    int rc = omega_reduce_gb10(op, x, n, &g);
    *launches += omega_reduce_gb10_last_launches();
    (*cases)++;
    if (rc == OMEGA_NUMERIC_OK && omega_isnan(g) && g_nan_bits_seen[op] < 1) { g_nan_bits_seen[op] = 1; g_nan_bits[op] = bf(g); }
    char ws[24] = "";
    if (has_want) snprintf(ws, sizeof(ws), " want=0x%08x", want);
    printf("RED_GB10_CASE op=%s n=%zu dist=%s ref=0x%08x gb10=0x%08x%s launches=%u rc=%d\n", omega_reduce_op_name(op), n, tag,
           bf(r), bf(g), ws, omega_reduce_gb10_last_launches(), rc);
    int bad = rc != OMEGA_NUMERIC_OK || !omega_numeric_bits_equal(r, g) || (has_want && !omega_numeric_bits_equal(g, fb(want)));
    if (bad) {
        if (!*mism) snprintf(d, dl, "first: op=%s n=%zu dist=%s ref=0x%08x gb10=0x%08x rc=%d", omega_reduce_op_name(op), n, tag, bf(r), bf(g), rc);
        (*mism)++;
    }
}
#endif

static void test_gb10_parity(void) {
#ifdef OMEGA_NUMERIC_CPU_ONLY
    printf("RED_GB10_PARITY: SKIP chip-only (CPU build)\n");
    g_skip++;
#else
    static const size_t gn[] = { 0, 1, 2, 31, 32, 33, 63, 64, 65, 97, 1000, 1023, 1024, 1025, 4097,
                                 32768, 32769, 65536, 65537, 1000003, 1000000 };
    size_t cases[4] = { 0 }, mism[4] = { 0 }, launches[4] = { 0 };
    char d[4][512] = { "", "", "", "" };
    float *x = malloc(1000003 * sizeof(float));
    for (int op = 0; op < OMEGA_RED_COUNT; op++) {
        OmegaReduceOp rop = (OmegaReduceOp)op;
        for (size_t k = 0; k < sizeof(gn) / sizeof(gn[0]); k++) {
            size_t n = gn[k];
            int dists[4] = { DIST_CANCEL, DIST_SPECIAL, DIST_SUBNORMAL, DIST_BITS };
            for (int q = 0; q < (n > 70000 ? 2 : 4); q++) {
                int dist = n > 70000 ? (q == 0 ? DIST_CANCEL : DIST_SPECIAL) : dists[q];
                for (size_t i = 0; i < n; i++) x[i] = gen(dist);
                chip_case(rop, x, n, DIST_NAMES[dist], 0, 0, &cases[op], &mism[op], &launches[op], d[op], sizeof(d[op]));
            }
        }
    }
    /* SUM crafted special-value vectors, expected bits written out (as PR #134) */
    {
        static float v[65537];
        const struct crafted *sv = CRAFTED;
        for (int op = 0; op < OMEGA_RED_COUNT; op++)
            for (size_t k = 0; k < CRAFTED_N; k++) {
                for (size_t i = 0; i < sv[k].n; i++) v[i] = fb(sv[k].fill);
                v[0] = fb(sv[k].s0);
                v[sv[k].i1] = fb(sv[k].s1);
                char tag[32];
                snprintf(tag, sizeof(tag), "crafted%zu", k);
                chip_case((OmegaReduceOp)op, v, sv[k].n, tag, 1, sv[k].want[op], &cases[op], &mism[op], &launches[op], d[op], sizeof(d[op]));
            }
    }
    /* the three hand-derived SUM worked examples and the MEAN one on chip */
    {
        float a33[33], a32[32], a64[64];
        a33[0] = 16777216.0f; for (int i = 1; i < 33; i++) a33[i] = 1.0f;
        for (int i = 0; i < 32; i++) a32[i] = fb(0x80000000u);
        a32[0] = 16777216.0f; a32[1] = 1.0f; a32[17] = 1.0f;
        for (int i = 0; i < 64; i++) a64[i] = fb(0x80000000u);
        a64[0] = 16777216.0f; a64[1] = 1.0f; a64[33] = 1.0f;
        chip_case(OMEGA_RED_SUM, a33, 33, "worked0", 1, 0x4B800010u, &cases[0], &mism[0], &launches[0], d[0], sizeof(d[0]));
        chip_case(OMEGA_RED_SUM, a32, 32, "worked1", 1, 0x4B800001u, &cases[0], &mism[0], &launches[0], d[0], sizeof(d[0]));
        chip_case(OMEGA_RED_SUM, a64, 64, "worked2", 1, 0x4B800000u, &cases[0], &mism[0], &launches[0], d[0], sizeof(d[0]));
        chip_case(OMEGA_RED_MEAN, a33, 33, "worked_mean", 1, 0x48F83E2Fu, &cases[3], &mism[3], &launches[3], d[3], sizeof(d[3]));
    }
    free(x);
    size_t total_mism = 0, total_cases = 0;
    for (int op = 0; op < OMEGA_RED_COUNT; op++) {
        char det[1200];
        snprintf(det, sizeof(det), "op=%s order=%s cases=%zu launches=%zu mismatches=%zu chip_nan_bits=%s0x%08x%s %.500s",
                 omega_reduce_op_name((OmegaReduceOp)op), OMEGA_REDUCE_DECLARED_ORDER, cases[op], launches[op], mism[op],
                 g_nan_bits_seen[op] ? "" : "(none) ", g_nan_bits[op],
                 op == OMEGA_RED_MEAN ? " final_division=HOST_DECLARED_STEP" : "", d[op]);
        char id[64];
        snprintf(id, sizeof(id), "RED_GB10_PARITY_%s", omega_reduce_op_name((OmegaReduceOp)op));
        verdict(id, mism[op] == 0 && cases[op] > 0, det);
        total_mism += mism[op]; total_cases += cases[op];
    }
    char det[256];
    snprintf(det, sizeof(det), "ops=SUM,MAX,MIN,MEAN cases=%zu mismatches=%zu", total_cases, total_mism);
    printf("RED_GB10_PARITY: %s %s\n", total_mism == 0 && total_cases > 0 ? "PASS" : "FAIL", det);
#endif
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--dump") == 0) return dump_patches(argv[2]);
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
    test_crafted_table();
    test_gb10_presubmit();
    test_gb10_minmax_patch();
    test_gb10_wrong_pad_caught();
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
