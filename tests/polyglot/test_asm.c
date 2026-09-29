/* POLYGLOT-0 lane B1 correctness gate for the hand-assembly candidates
 * (asm_sdot, asm_crumb). spec/polyglot-0.md sections 1 and 10.3.
 *   1. MA-3 shape grid (copied from tests/algebra/test_realize.c), every
 *      weight/x kind, bit-exact vs oma_rz_oracle, run twice per plan.
 *   2. >= 20,000 random cases: m 1..70, n 1..2100, random sparsity, x kinds
 *      including -128/127, x and y unaligned by offsets 1..15, canary bytes
 *      around y (no write outside y[0..m)).
 *   3. Large magnitudes up to n = max_n (= OMA_RZ_MAX_N).
 *   4. Guard pages: W (packed plan memory and pack input), x and y each
 *      placed so their last byte touches a PROT_NONE page, then so their
 *      first byte follows one.
 *   5. Error contract: bad trits, NULL, m/n = 0, n > max_n, empty plan,
 *      no partial plan kept on error.
 * Fixed seed; reproducible. */
#include "polyglot/omx_lang.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned long long g_checks, g_fail, g_cases;
static uint64_t g_rng = 0x504f4c5941534d31ULL; /* "POLYASM1" */

static uint64_t rnd(void) {
    uint64_t z = (g_rng += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
static double rnd01(void) { return (double)(rnd() >> 11) * (1.0 / 9007199254740992.0); }

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            g_fail++;                                                         \
            if (g_fail <= 20) {                                               \
                fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);          \
                fprintf(stderr, __VA_ARGS__);                                 \
                fputc('\n', stderr);                                          \
            }                                                                 \
        }                                                                     \
    } while (0)

enum { WK_RANDOM, WK_ZERO, WK_POS, WK_NEG };
enum { XK_RANDOM, XK_MIN, XK_MAX, XK_ALT, XK_ZERO };

static void fill_w(int8_t *w, size_t cnt, int kind, double sparsity) {
    for (size_t i = 0; i < cnt; i++) {
        switch (kind) {
        case WK_ZERO: w[i] = 0; break;
        case WK_POS: w[i] = 1; break;
        case WK_NEG: w[i] = -1; break;
        default: w[i] = rnd01() < sparsity ? 0 : ((rnd() & 1) ? 1 : -1); break;
        }
    }
}

static void fill_x(int8_t *x, size_t n, int kind) {
    for (size_t j = 0; j < n; j++) {
        switch (kind) {
        case XK_MIN: x[j] = -128; break;
        case XK_MAX: x[j] = 127; break;
        case XK_ALT: x[j] = (j & 1) ? 127 : -128; break;
        case XK_ZERO: x[j] = 0; break;
        default: x[j] = (int8_t)(uint8_t)rnd(); break;
        }
    }
}

/* y may be unaligned: compare through memcpy, never an int32 dereference. */
static size_t count_bad(const void *y, const int32_t *yref, size_t m, size_t *first, int32_t *got) {
    size_t bad = 0;
    for (size_t i = 0; i < m; i++) {
        int32_t v;
        memcpy(&v, (const char *)y + 4 * i, 4);
        if (v != yref[i] && bad++ == 0) { *first = i; *got = v; }
    }
    return bad;
}

/* pack w, run twice into y (any alignment), compare with yref. */
static void check_impl(const oma_rz_impl *im, const int8_t *w, size_t m, size_t n, const int8_t *x,
                       int32_t *y, const int32_t *yref, const char *tag) {
    oma_rz_plan p;
    int rc = im->pack(&p, w, m, n);
    CHECK(rc == OMA_RZ_OK, "%s %s pack m=%zu n=%zu rc %d", im->id, tag, m, n, rc);
    if (rc) { oma_rz_free(&p); return; }
    CHECK(p.weight_bytes > 0 && p.footprint_bytes >= p.weight_bytes, "%s byte accounting", im->id);
    for (int rep = 0; rep < 2; rep++) {
        memset(y, 0x5a, m * 4);
        rc = im->run(&p, x, y);
        CHECK(rc == OMA_RZ_OK, "%s %s run rc %d", im->id, tag, rc);
        size_t first = 0;
        int32_t got = 0;
        size_t bad = count_bad(y, yref, m, &first, &got);
        CHECK(bad == 0, "%s %s m=%zu n=%zu: %zu rows differ, first %zu got %d want %d", im->id, tag, m, n,
              bad, first, got, yref[first]);
    }
    oma_rz_free(&p);
    g_cases++;
}

static void run_case(size_t m, size_t n, int wk, double sp, int xk) {
    int8_t *w = malloc(m * n), *x = malloc(n);
    int32_t *yref = malloc(m * 4), *y = malloc(m * 4);
    if (!w || !x || !yref || !y) { CHECK(0, "oom"); goto out; }
    fill_w(w, m * n, wk, sp);
    fill_x(x, n, xk);
    CHECK(oma_rz_oracle(w, m, n, x, yref) == OMA_RZ_OK, "oracle rc");
    for (size_t c = 0; c < omx_lane_asm_count; c++) check_impl(omx_lane_asm[c].impl, w, m, n, x, y, yref, "grid");
out:
    free(w); free(x); free(yref); free(y);
}

/* random case with unaligned x/y and canaries around y */
static void run_random(size_t m, size_t n, int wk, double sp, int xk, size_t xo, size_t yo) {
    int8_t *w = malloc(m * n), *xb = malloc(n + 32);
    int32_t *yref = malloc(m * 4);
    unsigned char *yb = malloc(m * 4 + 64);
    if (!w || !xb || !yref || !yb) { CHECK(0, "oom"); goto out; }
    int8_t *x = xb + xo;
    fill_w(w, m * n, wk, sp);
    fill_x(x, n, xk);
    CHECK(oma_rz_oracle(w, m, n, x, yref) == OMA_RZ_OK, "oracle rc");
    for (size_t c = 0; c < omx_lane_asm_count; c++) {
        const oma_rz_impl *im = omx_lane_asm[c].impl;
        memset(yb, 0xa5, m * 4 + 64);
        unsigned char *y = yb + 16 + yo;
        check_impl(im, w, m, n, x, (int32_t *)(void *)y, yref, "random");
        size_t spill = 0;
        for (size_t i = 0; i < 16 + yo; i++) spill += yb[i] != 0xa5;
        for (size_t i = 16 + yo + m * 4; i < m * 4 + 64; i++) spill += yb[i] != 0xa5;
        CHECK(spill == 0, "%s wrote %zu bytes outside y (m=%zu n=%zu yo=%zu)", im->id, spill, m, n, yo);
    }
out:
    free(w); free(xb); free(yref); free(yb);
}

/* ---- guard pages ---- */
typedef struct { unsigned char *base; size_t len; } gmap;

static void *gplace(gmap *g, size_t bytes, int at_end) {
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    size_t dp = (bytes + pg - 1) / pg;
    if (dp == 0) dp = 1;
    g->len = (dp + 2) * pg;
    void *b = mmap(NULL, g->len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (b == MAP_FAILED) { g->base = NULL; return NULL; }
    g->base = b;
    if (mprotect(g->base, pg, PROT_NONE) || mprotect(g->base + (dp + 1) * pg, pg, PROT_NONE)) return NULL;
    unsigned char *data = g->base + pg;
    return at_end ? data + dp * pg - bytes : data;
}
static void gfree(gmap *g) { if (g->base) munmap(g->base, g->len); g->base = NULL; }

static void run_guard(size_t m, size_t n, int at_end) {
    gmap gw = {0}, gx = {0}, gy = {0}, gp = {0};
    int8_t *w = gplace(&gw, m * n, at_end), *x = gplace(&gx, n, at_end);
    unsigned char *y = gplace(&gy, m * 4, at_end);
    int32_t *yref = malloc(m * 4);
    if (!w || !x || !y || !yref) { CHECK(0, "guard mmap"); goto out; }
    fill_w(w, m * n, WK_RANDOM, rnd01());
    fill_x(x, n, XK_RANDOM);
    x[0] = -128;
    x[n - 1] = 127;
    CHECK(oma_rz_oracle(w, m, n, x, yref) == OMA_RZ_OK, "oracle rc");
    for (size_t c = 0; c < omx_lane_asm_count; c++) {
        const oma_rz_impl *im = omx_lane_asm[c].impl;
        oma_rz_plan p;
        int rc = im->pack(&p, w, m, n);
        CHECK(rc == OMA_RZ_OK, "%s guard pack rc %d", im->id, rc);
        if (rc) { oma_rz_free(&p); continue; }
        /* move the packed weights against the guard page */
        void *pw = gplace(&gp, p.weight_bytes, at_end);
        if (!pw) { CHECK(0, "guard mmap"); oma_rz_free(&p); continue; }
        memcpy(pw, p.mem, p.weight_bytes);
        void *own = p.mem;
        p.mem = pw;
        memset(y, 0x5a, m * 4);
        rc = im->run(&p, x, (int32_t *)(void *)y);
        p.mem = own;
        size_t first = 0;
        int32_t got = 0;
        size_t bad = count_bad(y, yref, m, &first, &got);
        CHECK(rc == OMA_RZ_OK && bad == 0, "%s guard(%s) m=%zu n=%zu rc %d: %zu rows differ, first %zu got %d want %d",
              im->id, at_end ? "end" : "start", m, n, rc, bad, first, got, yref[first]);
        gfree(&gp);
        oma_rz_free(&p);
        g_cases++;
    }
out:
    gfree(&gw); gfree(&gx); gfree(&gy);
    free(yref);
}

/* ---- error contract ---- */
static int plan_is_zero(const oma_rz_plan *p) {
    static const oma_rz_plan z;
    return memcmp(p, &z, sizeof z) == 0;
}

static void test_errors(void) {
    int8_t w[6] = {1, 0, -1, 2, 0, 1}, x[3] = {1, 2, 3};
    int32_t y[2];
    for (size_t c = 0; c < omx_lane_asm_count; c++) {
        const omx_candidate *cd = &omx_lane_asm[c];
        const oma_rz_impl *im = cd->impl;
        oma_rz_plan p;
        CHECK(strcmp(cd->language, "asm-aarch64") == 0 && strcmp(cd->toolchain, "gnu-as 2.42") == 0,
              "%s metadata", im->id);
        CHECK(cd->compiler_derived == 0 && cd->toolchain_only == 0, "%s derivation flags", im->id);
        CHECK(im->exact == 1 && im->max_n == OMA_RZ_MAX_N, "%s exact/max_n", im->id);
        static const int8_t bad[] = {2, -2, (int8_t)-128, 127, 3};
        for (size_t b = 0; b < sizeof bad; b++) {
            w[3] = bad[b];
            memset(&p, 0x77, sizeof p);
            CHECK(im->pack(&p, w, 2, 3) == OMA_RZ_E_TRIT, "%s accepts weight %d", im->id, bad[b]);
            CHECK(plan_is_zero(&p), "%s keeps a partial plan after E_TRIT", im->id);
            oma_rz_free(&p);
        }
        w[3] = 1;
        memset(&p, 0x77, sizeof p);
        CHECK(im->pack(&p, w, 0, 3) == OMA_RZ_E_ARG, "%s accepts m=0", im->id);
        CHECK(plan_is_zero(&p), "%s partial plan after m=0", im->id);
        CHECK(im->pack(&p, w, 2, 0) == OMA_RZ_E_ARG, "%s accepts n=0", im->id);
        CHECK(im->pack(&p, NULL, 2, 3) == OMA_RZ_E_ARG, "%s accepts NULL w", im->id);
        CHECK(plan_is_zero(&p), "%s partial plan after NULL w", im->id);
        CHECK(im->pack(NULL, w, 2, 3) == OMA_RZ_E_ARG, "%s accepts NULL plan", im->id);
        /* shape is checked before any weight is read (w is only 6 bytes) */
        CHECK(im->pack(&p, w, 1, OMA_RZ_MAX_N + 1) == OMA_RZ_E_ARG, "%s accepts n = max_n + 1", im->id);
        CHECK(im->pack(&p, w, 1, SIZE_MAX) == OMA_RZ_E_ARG, "%s accepts n = SIZE_MAX", im->id);
        CHECK(im->pack(&p, w, SIZE_MAX / 2, 1000) == OMA_RZ_E_OVERFLOW, "%s size overflow", im->id);
        CHECK(plan_is_zero(&p), "%s partial plan after overflow", im->id);
        oma_rz_free(&p);
        /* empty plan and NULL run arguments */
        oma_rz_plan empty;
        memset(&empty, 0, sizeof empty);
        CHECK(im->run(&empty, x, y) == OMA_RZ_E_ARG, "%s runs an empty plan", im->id);
        CHECK(im->run(NULL, x, y) == OMA_RZ_E_ARG, "%s runs NULL plan", im->id);
        CHECK(im->pack(&p, w, 2, 3) == OMA_RZ_OK, "%s pack 2x3", im->id);
        CHECK(im->run(&p, NULL, y) == OMA_RZ_E_ARG, "%s runs NULL x", im->id);
        CHECK(im->run(&p, x, NULL) == OMA_RZ_E_ARG, "%s runs NULL y", im->id);
        oma_rz_plan zm = p;
        zm.m = 0;
        CHECK(im->run(&zm, x, y) == OMA_RZ_E_ARG, "%s runs m=0 plan", im->id);
        zm = p;
        zm.n = 0;
        CHECK(im->run(&zm, x, y) == OMA_RZ_E_ARG, "%s runs n=0 plan", im->id);
        CHECK(im->run(&p, x, y) == OMA_RZ_OK && y[0] == 1 * 1 + 0 * 2 + -1 * 3 && y[1] == 1 * 1 + 0 * 2 + 1 * 3,
              "%s 2x3 value", im->id);
        oma_rz_free(&p);
    }
}

int main(void) {
    test_errors();
    unsigned long long c0 = g_checks;

    /* 1. MA-3 grid (copied from tests/algebra/test_realize.c) */
    static const size_t ns[] = {1, 2, 3, 4, 5, 7, 15, 16, 17, 31, 32, 33, 63, 64, 65, 79, 80, 81,
                                127, 128, 129, 159, 160, 161, 255, 256, 257, 1000, 1023, 1024, 1025};
    static const size_t ms[] = {1, 2, 3, 4, 5, 7, 15, 16, 17, 33};
    static const double sps[] = {0.0, 0.3, 0.6, 0.9, 1.0};
    for (size_t a = 0; a < sizeof ns / sizeof ns[0]; a++)
        for (size_t b = 0; b < sizeof ms / sizeof ms[0]; b++) {
            run_case(ms[b], ns[a], WK_ZERO, 0, XK_RANDOM);
            run_case(ms[b], ns[a], WK_POS, 0, XK_MIN);
            run_case(ms[b], ns[a], WK_POS, 0, XK_MAX);
            run_case(ms[b], ns[a], WK_NEG, 0, XK_MIN);
            run_case(ms[b], ns[a], WK_NEG, 0, XK_MAX);
            run_case(ms[b], ns[a], WK_RANDOM, sps[(a + b) % 5], XK_ALT);
            run_case(ms[b], ns[a], WK_RANDOM, sps[(a * 3 + b) % 5], XK_RANDOM);
        }
    unsigned long long grid_cases = g_cases;

    /* 2. random: 20,000 shapes, unaligned x/y (offsets 1..15) */
    const int n_random = 20000;
    for (int it = 0; it < n_random; it++) {
        size_t n = 1 + (size_t)(rnd() % 2100), m = 1 + (size_t)(rnd() % 70);
        int xk = (rnd() % 4) ? XK_RANDOM : (int)(rnd() % 5);
        size_t xo = 1 + (size_t)(rnd() % 15), yo = 1 + (size_t)(rnd() % 15);
        run_random(m, n, WK_RANDOM, rnd01(), xk, xo, yo);
    }
    unsigned long long random_cases = g_cases - grid_cases;

    /* 3. large magnitudes up to max_n */
    run_case(5, 16384, WK_NEG, 0, XK_MIN);                /* y = +2,097,152 */
    run_case(5, 16384, WK_POS, 0, XK_MIN);
    run_case(1, 100003, WK_NEG, 0, XK_MIN);               /* y = 12,800,384 */
    run_case(6, (1u << 20) + 37, WK_RANDOM, 0.3, XK_RANDOM);
    run_case(17, 4099, WK_RANDOM, 0.97, XK_RANDOM);
    run_case(1, OMA_RZ_MAX_N, WK_NEG, 0, XK_MIN);         /* y = +2,147,483,520 */
    run_case(1, OMA_RZ_MAX_N, WK_POS, 0, XK_MIN);         /* y = -2,147,483,520 */
    run_case(1, OMA_RZ_MAX_N, WK_RANDOM, 0.5, XK_ALT);
    unsigned long long large_cases = g_cases - grid_cases - random_cases;

    /* 4. guard pages on both sides of W (input and packed), x and y */
    static const size_t gms[] = {1, 2, 3, 4, 5, 7, 8, 17};
    for (int side = 0; side < 2; side++)
        for (size_t a = 0; a < sizeof ns / sizeof ns[0]; a++)
            for (size_t b = 0; b < sizeof gms / sizeof gms[0]; b++) run_guard(gms[b], ns[a], side);
    for (int it = 0; it < 200; it++)
        run_guard(1 + (size_t)(rnd() % 70), 1 + (size_t)(rnd() % 2100), it & 1);
    unsigned long long guard_cases = g_cases - grid_cases - random_cases - large_cases;

    printf("POLYGLOT test-asm %s: %llu checks, %llu failures; %zu candidates; cases: grid %llu, random %llu, "
           "large %llu, guard %llu; error-contract checks %llu\n",
           g_fail ? "FAIL" : "PASS", g_checks, g_fail, omx_lane_asm_count, grid_cases, random_cases, large_cases,
           guard_cases, c0);
    return g_fail ? 1 : 0;
}
