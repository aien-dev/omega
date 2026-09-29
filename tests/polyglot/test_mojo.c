/* POLYGLOT-0 lane B3: correctness gate for the Mojo Omega-X candidates.
 * spec/polyglot-0.md. Correctness only; timed benchmarks belong to lane F.
 *
 * For each candidate in omx_lane_mojo[] (called through its oma_rz_impl):
 *   1. shape grid n in {1..5,15,16,17,63,64,65,127,128,129,255,256,257,1000,1025}
 *      x m in {1..5,7,16,17,33}, sparsity {0, 0.33, 0.66, 1}, x patterns
 *      random / all -128 / all +127 / alternating -128,127; bit-exact vs
 *      oma_rz_oracle;
 *   2. >= 5,000 random cases (random m, n, sparsity; x with forced -128/127;
 *      unaligned x, y offset by one int32);
 *   3. error contract: bad trit (several values and positions, incl. tails),
 *      n > max_n, m = 0, n = 0, NULL p / w, run on empty plan, NULL x / y;
 *   (every packed plan is byte-identical to the MA-3 twin R1_sdot / R2c_crumb);
 *   4. null kernel: the C -> Mojo boundary returns 0 and leaves y untouched.
 */
#include "polyglot/omx_lang.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int omx_mojo_null_run(const oma_rz_plan *p, const int8_t *x, int32_t *y);

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static unsigned long checks, failures;

static void fail(const char *id, const char *what, size_t m, size_t n, long a, long b) {
    failures++;
    if (failures <= 20)
        fprintf(stderr, "FAIL %s %s m=%zu n=%zu got=%ld want=%ld\n", id, what, m, n, a, b);
}

static void fill_w(int8_t *w, size_t cnt, unsigned zero_pct) {
    for (size_t i = 0; i < cnt; i++) {
        uint64_t r = rnd();
        if (r % 100u < zero_pct) w[i] = 0;
        else w[i] = (r >> 20) & 1 ? 1 : -1;
    }
}

static void fill_x(int8_t *x, size_t n, int pattern) {
    for (size_t j = 0; j < n; j++) {
        switch (pattern) {
        case 1: x[j] = -128; break;
        case 2: x[j] = 127; break;
        case 3: x[j] = (j & 1) ? 127 : -128; break;
        default: {
            uint64_t r = rnd();
            unsigned s = (unsigned)(r % 16u);
            x[j] = s == 0 ? -128 : s == 1 ? 127 : (int8_t)(r >> 24);
        }
        }
    }
}

/* one case: pack, run into y (offset buffers), compare with the oracle */
static void one_case(const oma_rz_impl *im, const int8_t *w, size_t m, size_t n, const int8_t *x) {
    oma_rz_plan p;
    memset(&p, 0, sizeof p);
    int rc = im->pack(&p, w, m, n);
    checks++;
    if (rc != OMA_RZ_OK) {
        fail(im->id, "pack", m, n, rc, 0);
        return;
    }
    /* same packed layout as the MA-3 twin (R1_sdot / R2c_crumb) */
    const oma_rz_impl *twin = oma_rz_find(strcmp(im->family, "crumb2") ? "R1_sdot" : "R2c_crumb");
    oma_rz_plan q;
    memset(&q, 0, sizeof q);
    checks++;
    if (!twin || twin->pack(&q, w, m, n) != OMA_RZ_OK || q.weight_bytes != p.weight_bytes ||
        q.nnz != p.nnz || memcmp(q.mem, p.mem, p.weight_bytes) != 0)
        fail(im->id, "layout vs MA-3 twin", m, n, 0, 0);
    oma_rz_free(&q);
    int32_t *ybuf = malloc((m + 2) * sizeof(int32_t));
    int32_t *ref = malloc(m * sizeof(int32_t));
    if (!ybuf || !ref) {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    int32_t *y = ybuf + 1; /* 4-byte aligned, not 16-byte aligned */
    ybuf[0] = 0x5A5A5A5A;
    ybuf[m + 1] = 0x5A5A5A5A;
    for (size_t i = 0; i < m; i++) y[i] = 0x7EADBEEF;
    oma_rz_oracle(w, m, n, x, ref);
    rc = im->run(&p, x, y);
    checks++;
    if (rc != OMA_RZ_OK) fail(im->id, "run rc", m, n, rc, 0);
    for (size_t i = 0; i < m; i++) {
        checks++;
        if (y[i] != ref[i]) {
            fail(im->id, "value", m, n, y[i], ref[i]);
            break;
        }
    }
    checks++;
    if (ybuf[0] != 0x5A5A5A5A || ybuf[m + 1] != 0x5A5A5A5A) fail(im->id, "y guard", m, n, 0, 0);
    free(ybuf);
    free(ref);
    oma_rz_free(&p);
}

static void grid(const oma_rz_impl *im) {
    static const size_t ns[] = {1, 2, 3, 4, 5, 15, 16, 17, 63, 64, 65, 127, 128, 129, 255, 256, 257, 1000, 1025};
    static const size_t ms[] = {1, 2, 3, 4, 5, 7, 16, 17, 33};
    static const unsigned sp[] = {0, 33, 66, 100};
    for (size_t a = 0; a < sizeof ns / sizeof ns[0]; a++)
        for (size_t b = 0; b < sizeof ms / sizeof ms[0]; b++)
            for (size_t s = 0; s < 4; s++)
                for (int pat = 0; pat < 4; pat++) {
                    size_t n = ns[a], m = ms[b];
                    int8_t *w = malloc(m * n), *x = malloc(n);
                    fill_w(w, m * n, sp[s]);
                    fill_x(x, n, pat);
                    one_case(im, w, m, n, x);
                    free(w);
                    free(x);
                }
}

static void random_cases(const oma_rz_impl *im, int count) {
    for (int t = 0; t < count; t++) {
        size_t m = 1 + rnd() % 40u;
        size_t n = 1 + rnd() % (t % 10 == 0 ? 4100u : 700u);
        unsigned zp = (unsigned)(rnd() % 101u);
        size_t off = rnd() % 16u; /* unaligned x */
        int8_t *w = malloc(m * n), *xb = malloc(n + 16);
        fill_w(w, m * n, zp);
        fill_x(xb + off, n, t % 7 == 0 ? (int)(1 + rnd() % 3u) : 0);
        one_case(im, w, m, n, xb + off);
        free(w);
        free(xb);
    }
}

static void expect_rc(const char *id, const char *what, int got, int want) {
    checks++;
    if (got != want) fail(id, what, 0, 0, got, want);
}

static void errors(const oma_rz_impl *im) {
    static const int8_t bad[] = {2, -2, 3, 127, -128, 64};
    static const size_t shapes[][2] = {{1, 1}, {3, 17}, {5, 64}, {4, 65}, {7, 129}};
    for (size_t s = 0; s < sizeof shapes / sizeof shapes[0]; s++)
        for (size_t b = 0; b < sizeof bad / sizeof bad[0]; b++)
            for (int pos = 0; pos < 3; pos++) {
                size_t m = shapes[s][0], n = shapes[s][1], cnt = m * n;
                int8_t *w = malloc(cnt);
                fill_w(w, cnt, 33);
                size_t at = pos == 0 ? 0 : pos == 1 ? cnt - 1 : rnd() % cnt;
                w[at] = bad[b];
                oma_rz_plan p;
                memset(&p, 0, sizeof p);
                expect_rc(im->id, "bad trit", im->pack(&p, w, m, n), OMA_RZ_E_TRIT);
                checks++;
                if (p.mem || p.aux || p.scratch) fail(im->id, "partial plan kept", m, n, 1, 0);
                oma_rz_free(&p);
                free(w);
            }
    int8_t w1[4] = {1, 0, -1, 1}, x1[4] = {1, 2, 3, 4};
    int32_t y1[4];
    oma_rz_plan p;
    memset(&p, 0, sizeof p);
    expect_rc(im->id, "n > max_n", im->pack(&p, w1, 1, im->max_n + 1), OMA_RZ_E_ARG);
    expect_rc(im->id, "n > OMA_RZ_MAX_N", im->pack(&p, w1, 1, OMA_RZ_MAX_N + 1), OMA_RZ_E_ARG);
    expect_rc(im->id, "m = 0", im->pack(&p, w1, 0, 4), OMA_RZ_E_ARG);
    expect_rc(im->id, "n = 0", im->pack(&p, w1, 4, 0), OMA_RZ_E_ARG);
    expect_rc(im->id, "NULL w", im->pack(&p, NULL, 1, 4), OMA_RZ_E_ARG);
    expect_rc(im->id, "NULL p", im->pack(NULL, w1, 1, 4), OMA_RZ_E_ARG);
    memset(&p, 0, sizeof p);
    expect_rc(im->id, "run empty plan", im->run(&p, x1, y1), OMA_RZ_E_ARG);
    expect_rc(im->id, "pack ok", im->pack(&p, w1, 1, 4), OMA_RZ_OK);
    expect_rc(im->id, "run NULL x", im->run(&p, NULL, y1), OMA_RZ_E_ARG);
    expect_rc(im->id, "run NULL y", im->run(&p, x1, NULL), OMA_RZ_E_ARG);
    expect_rc(im->id, "run NULL p", im->run(NULL, x1, y1), OMA_RZ_E_ARG);
    expect_rc(im->id, "run ok", im->run(&p, x1, y1), OMA_RZ_OK);
    checks++;
    if (y1[0] != 1 * 1 + 0 * 2 - 1 * 3 + 1 * 4) fail(im->id, "tiny value", 1, 4, y1[0], 2);
    checks++;
    if (p.nnz != 3) fail(im->id, "nnz", 1, 4, (long)p.nnz, 3);
    oma_rz_free(&p);
    oma_rz_free(&p); /* safe twice */
}

static void null_kernel(void) {
    int8_t w[16] = {0}, x[16] = {0};
    int32_t y[4] = {11, 22, 33, 44};
    oma_rz_plan p;
    memset(&p, 0, sizeof p);
    p.m = 4;
    p.n = 16;
    p.mem = w;
    for (int i = 0; i < 1000; i++) expect_rc("null", "null kernel rc", omx_mojo_null_run(&p, x, y), 0);
    checks++;
    if (y[0] != 11 || y[1] != 22 || y[2] != 33 || y[3] != 44) fail("null", "y touched", 4, 16, 0, 0);
}

int main(void) {
    unsigned long cases_before;
    if (omx_lane_mojo_count != 2) {
        fprintf(stderr, "FAIL expected 2 mojo candidates, got %zu\n", omx_lane_mojo_count);
        return 1;
    }
    for (size_t c = 0; c < omx_lane_mojo_count; c++) {
        const omx_candidate *cd = &omx_lane_mojo[c];
        const oma_rz_impl *im = cd->impl;
        if (strcmp(cd->language, "mojo") || !im->exact || cd->compiler_derived || cd->toolchain_only) {
            fprintf(stderr, "FAIL metadata %s\n", im->id);
            return 1;
        }
        cases_before = failures;
        grid(im);
        random_cases(im, 6000);
        errors(im);
        printf("  %-11s %-7s grid 19x9x4x4 + 6000 random + errors: %s\n", im->id, im->family,
               failures == cases_before ? "ok" : "FAIL");
    }
    null_kernel();
    if (failures) {
        printf("POLYGLOT_MOJO_FAIL checks=%lu failures=%lu\n", checks, failures);
        return 1;
    }
    printf("POLYGLOT_MOJO_PASS candidates=%zu checks=%lu failures=0\n", omx_lane_mojo_count, checks);
    return 0;
}
