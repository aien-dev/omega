/* POLYGLOT-0 shared verifier (lane F). spec/polyglot-0.md sections 1 and 10.3.
 *
 * Every registered candidate (omx_candidate_get) is checked against
 * oma_rz_oracle on:
 *   - MA-3's full shape grid (tests/algebra/test_realize.c: every n x m tail
 *     combination, every weight/x kind, the 400 random shapes and the large-
 *     magnitude cases);
 *   - 20,000 random cases (random m, n, sparsity; x including -128/127 edges;
 *     m and n not multiples of any vector width; unaligned W, x and y);
 *   - guard-page placement for every case: W, x and y each live in their own
 *     mapping with PROT_NONE pages on both sides, placed either flush against
 *     the trailing guard page or at a small offset after the leading one; W
 *     and x are made read-only before any candidate runs;
 *   - the error contract (E_TRIT, E_ARG, E_OVERFLOW, no partial plan kept,
 *     run on an empty plan).
 * y is written with a sentinel before every run, and run three times per
 * plan (x1, x2, x1) so per-call scratch cannot leak state.
 * Output: one PASS/FAIL line per candidate with counts, then a summary line.
 * Environment: OMX_VERIFY_RANDOM overrides the random case count (the gate
 * uses the default 20000). Fixed seed; reproducible.
 */
#define _GNU_SOURCE
#include "polyglot/omx_bench.h"
#include "polyglot/omx_lang.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef OMX_VERIFY_BUILD
#define OMX_VERIFY_BUILD "plain"
#endif

#define MAXC 64
static size_t g_nc;
static unsigned long long g_checks[MAXC], g_fail[MAXC], g_cases[MAXC];
static unsigned long long g_hchecks, g_hfail; /* harness self-checks */
static omx_rng g_rng = {0x504f4c59474c4f54ULL}; /* "POLYGLOT" */
static size_t g_pg;
static const char *volatile g_cur_id = "(harness)";

/* A guard-page fault names the candidate that caused it, then fails the run. */
static void on_fault(int sig) {
    const char *a = "POLYGLOT verify FAIL: guard-page fault (signal) in candidate ";
    ssize_t r = write(2, a, strlen(a));
    r = write(2, g_cur_id, strlen(g_cur_id));
    r = write(2, "\n", 1);
    (void)r;
    _exit(128 + sig);
}

#define CHECK(ci, cond, ...)                                                   \
    do {                                                                       \
        g_checks[ci]++;                                                        \
        if (!(cond)) {                                                         \
            g_fail[ci]++;                                                      \
            if (g_fail[ci] <= 10) {                                            \
                fprintf(stderr, "FAIL [%s] ", omx_candidate_get(ci)->impl->id); \
                fprintf(stderr, __VA_ARGS__);                                  \
                fputc('\n', stderr);                                           \
            }                                                                  \
        }                                                                      \
    } while (0)

#define HCHECK(cond, ...)                                                      \
    do {                                                                       \
        g_hchecks++;                                                           \
        if (!(cond)) {                                                         \
            g_hfail++;                                                         \
            fprintf(stderr, "HARNESS FAIL: ");                                 \
            fprintf(stderr, __VA_ARGS__);                                      \
            fputc('\n', stderr);                                               \
        }                                                                      \
    } while (0)

static uint64_t rnd(void) { return omx_rng_next(&g_rng); }
static double rnd01(void) { return omx_rng_01(&g_rng); }

/* ---- guard-page buffers ---- */
typedef struct {
    unsigned char *map;
    size_t maplen;
    unsigned char *p;
    size_t size;
} gbuf;

/* flush_end: buffer ends at the trailing PROT_NONE page. Otherwise it starts
 * `off` bytes after the leading PROT_NONE page. */
static int gb_alloc(gbuf *b, size_t size, int flush_end, size_t off) {
    size_t need = size + off;
    size_t pages = (need + g_pg - 1) / g_pg;
    if (pages == 0) pages = 1;
    b->maplen = (pages + 2) * g_pg;
    b->map = mmap(NULL, b->maplen, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (b->map == MAP_FAILED) { b->map = NULL; return -1; }
    if (mprotect(b->map, g_pg, PROT_NONE) || mprotect(b->map + (pages + 1) * g_pg, g_pg, PROT_NONE)) return -1;
    b->p = flush_end ? b->map + (pages + 1) * g_pg - size : b->map + g_pg + off;
    b->size = size;
    return 0;
}

static void gb_readonly(gbuf *b) {
    size_t pages = b->maplen / g_pg - 2;
    mprotect(b->map + g_pg, pages * g_pg, PROT_READ);
}

static void gb_free(gbuf *b) {
    if (b->map) munmap(b->map, b->maplen);
    memset(b, 0, sizeof *b);
}

enum { WK_RANDOM, WK_ZERO, WK_POS, WK_NEG };
enum { XK_RANDOM, XK_MIN, XK_MAX, XK_ALT, XK_ZERO, XK_EDGEMIX, XK_COUNT };

static void fill_w(int8_t *w, size_t cnt, int kind, double sp) {
    for (size_t i = 0; i < cnt; i++) {
        switch (kind) {
        case WK_ZERO: w[i] = 0; break;
        case WK_POS: w[i] = 1; break;
        case WK_NEG: w[i] = -1; break;
        default: w[i] = rnd01() < sp ? 0 : ((rnd() & 1) ? 1 : -1); break;
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
        case XK_EDGEMIX: {
            uint64_t r = rnd() % 4;
            x[j] = r == 0 ? -128 : r == 1 ? 127 : (int8_t)(uint8_t)rnd();
            break;
        }
        default: x[j] = (int8_t)(uint8_t)rnd(); break;
        }
    }
}

typedef struct {
    int flush_w, flush_x, flush_y;
    size_t off_w, off_x, off_y; /* off_y is a multiple of 4 */
} placement;

static placement random_placement(void) {
    placement p;
    p.flush_w = (int)(rnd() & 1);
    p.flush_x = (int)(rnd() & 1);
    p.flush_y = (int)(rnd() & 1);
    p.off_w = rnd() % 64;
    p.off_x = rnd() % 64;
    p.off_y = 4 * (rnd() % 16);
    return p;
}

static void run_case(size_t m, size_t n, int wk, double sp, int xk, placement pl) {
    gbuf W = {0}, X1 = {0}, X2 = {0}, Y = {0};
    int32_t *yref1 = malloc(m * sizeof *yref1), *yref2 = malloc(m * sizeof *yref2);
    int ok = yref1 && yref2 && gb_alloc(&W, m * n, pl.flush_w, pl.off_w) == 0 &&
             gb_alloc(&X1, n, pl.flush_x, pl.off_x) == 0 && gb_alloc(&X2, n, !pl.flush_x, pl.off_x ^ 17) == 0 &&
             gb_alloc(&Y, m * sizeof(int32_t), pl.flush_y, pl.off_y) == 0;
    HCHECK(ok, "allocation m=%zu n=%zu", m, n);
    if (!ok) goto out;
    int8_t *w = (int8_t *)W.p, *x1 = (int8_t *)X1.p, *x2 = (int8_t *)X2.p;
    int32_t *y = (int32_t *)(void *)Y.p;
    fill_w(w, m * n, wk, sp);
    fill_x(x1, n, xk);
    fill_x(x2, n, (xk + 1) % XK_COUNT);
    gb_readonly(&W);
    gb_readonly(&X1);
    gb_readonly(&X2);
    HCHECK(oma_rz_oracle(w, m, n, x1, yref1) == OMA_RZ_OK, "oracle rc");
    HCHECK(oma_rz_oracle(w, m, n, x2, yref2) == OMA_RZ_OK, "oracle rc");
    for (size_t ci = 0; ci < g_nc; ci++) {
        const oma_rz_impl *im = omx_candidate_get(ci)->impl;
        g_cases[ci]++;
        g_cur_id = im->id;
        oma_rz_plan p;
        memset(&p, 0, sizeof p);
        int rc = im->pack(&p, w, m, n);
        if (n > im->max_n) {
            CHECK(ci, rc == OMA_RZ_E_ARG, "must refuse n=%zu > max_n, rc %d", n, rc);
            oma_rz_free(&p);
            continue;
        }
        CHECK(ci, rc == OMA_RZ_OK, "pack m=%zu n=%zu rc %d (%s)", m, n, rc, oma_rz_strerror(rc));
        if (rc) { oma_rz_free(&p); continue; }
        CHECK(ci, p.weight_bytes > 0 && p.footprint_bytes >= p.weight_bytes, "byte accounting");
        CHECK(ci, p.m == m && p.n == n, "plan shape %zux%zu, want %zux%zu", p.m, p.n, m, n);
        const int8_t *xs[3] = {x1, x2, x1};
        const int32_t *refs[3] = {yref1, yref2, yref1};
        for (int rep = 0; rep < 3; rep++) {
            for (size_t i = 0; i < m; i++) y[i] = (int32_t)0x5a5a5a5a;
            rc = im->run(&p, xs[rep], y);
            CHECK(ci, rc == OMA_RZ_OK, "run rc %d", rc);
            size_t bad = 0, first = 0;
            for (size_t i = 0; i < m; i++)
                if (y[i] != refs[rep][i] && bad++ == 0) first = i;
            CHECK(ci, bad == 0,
                  "m=%zu n=%zu wk=%d sp=%.2f xk=%d rep=%d place=%d%d%d/%zu,%zu,%zu: %zu rows differ, row %zu got %d "
                  "want %d",
                  m, n, wk, sp, xk, rep, pl.flush_w, pl.flush_x, pl.flush_y, pl.off_w, pl.off_x, pl.off_y, bad,
                  first, y[first], refs[rep][first]);
        }
        oma_rz_free(&p);
    }
out:
    gb_free(&W); gb_free(&X1); gb_free(&X2); gb_free(&Y);
    free(yref1); free(yref2);
}

static placement fixed_placement(size_t k) {
    placement p;
    p.flush_w = (int)(k & 1);
    p.flush_x = (int)((k >> 1) & 1);
    p.flush_y = (int)((k >> 2) & 1);
    p.off_w = k % 7;
    p.off_x = (k * 3) % 13;
    p.off_y = 4 * (k % 5);
    return p;
}

static int plan_empty(const oma_rz_plan *p) { return p->mem == NULL && p->aux == NULL && p->scratch == NULL; }

static void test_error_contract(void) {
    /* A 2x3 matrix whose bad weight sits at the very end of a guarded buffer. */
    gbuf W = {0}, T = {0};
    HCHECK(gb_alloc(&W, 6, 1, 0) == 0 && gb_alloc(&T, 16, 1, 0) == 0, "alloc");
    int8_t *w = (int8_t *)W.p;
    int8_t x[3] = {1, 2, 3};
    static const int8_t bad[] = {2, -2, -128, 127, 3};
    for (size_t ci = 0; ci < g_nc; ci++) {
        const omx_candidate *c = omx_candidate_get(ci);
        const oma_rz_impl *im = c->impl;
        g_cur_id = im->id;
        CHECK(ci, im->id && im->label && im->family && im->pack && im->run, "incomplete impl record");
        CHECK(ci, c->language && c->toolchain && c->source, "incomplete language metadata");
        CHECK(ci, im->exact == 1, "not exact");
        CHECK(ci, im->max_n >= 1 && im->max_n <= OMA_RZ_MAX_N, "max_n %zu outside [1, OMA_RZ_MAX_N]", im->max_n);
        for (size_t cj = 0; cj < ci; cj++)
            CHECK(ci, strcmp(omx_candidate_get(cj)->impl->id, im->id) != 0, "duplicate id");
        for (size_t b = 0; b < sizeof bad; b++)
            for (size_t pos = 0; pos < 6; pos += 5) { /* first and last element */
                const int8_t good[6] = {1, 0, -1, 1, 0, -1};
                memcpy(w, good, 6);
                w[pos] = bad[b];
                oma_rz_plan p;
                memset(&p, 0, sizeof p);
                int rc = im->pack(&p, w, 2, 3);
                CHECK(ci, rc == OMA_RZ_E_TRIT, "weight %d at %zu: rc %d, want E_TRIT", bad[b], pos, rc);
                CHECK(ci, plan_empty(&p), "partial plan kept after E_TRIT");
                oma_rz_free(&p);
            }
        const int8_t good[6] = {1, 0, -1, 1, 0, -1};
        memcpy(w, good, 6);
        oma_rz_plan p;
        memset(&p, 0, sizeof p);
        int rc = im->pack(&p, w, 0, 3);
        CHECK(ci, rc == OMA_RZ_E_ARG && plan_empty(&p), "m=0: rc %d", rc);
        rc = im->pack(&p, w, 2, 0);
        CHECK(ci, rc == OMA_RZ_E_ARG && plan_empty(&p), "n=0: rc %d", rc);
        rc = im->pack(&p, NULL, 2, 3);
        CHECK(ci, rc == OMA_RZ_E_ARG && plan_empty(&p), "W=NULL: rc %d", rc);
        /* shape errors are reported before any weight is read: T is 16 bytes */
        rc = im->pack(&p, (const int8_t *)T.p, 1, OMA_RZ_MAX_N + 1);
        CHECK(ci, rc == OMA_RZ_E_ARG && plan_empty(&p), "n above int32 bound: rc %d", rc);
        if (im->max_n < OMA_RZ_MAX_N) {
            rc = im->pack(&p, (const int8_t *)T.p, 1, im->max_n + 1);
            CHECK(ci, rc == OMA_RZ_E_ARG && plan_empty(&p), "n above max_n: rc %d", rc);
        }
        rc = im->pack(&p, (const int8_t *)T.p, SIZE_MAX / 2, 4);
        CHECK(ci, (rc == OMA_RZ_E_OVERFLOW || rc == OMA_RZ_E_NOMEM) && plan_empty(&p),
              "m*n size overflow: rc %d, want E_OVERFLOW or E_NOMEM", rc);
        oma_rz_free(&p); /* safe on an empty plan */
        CHECK(ci, plan_empty(&p), "free of empty plan");
        oma_rz_plan empty;
        memset(&empty, 0, sizeof empty);
        int32_t y[2];
        CHECK(ci, im->run(&empty, x, y) == OMA_RZ_E_ARG, "runs an empty plan");
        /* plan reuse: pack, free, pack again into the same struct */
        rc = im->pack(&p, w, 2, 3);
        CHECK(ci, rc == OMA_RZ_OK, "pack good 2x3: rc %d", rc);
        oma_rz_free(&p);
        CHECK(ci, plan_empty(&p), "free leaves plan non-empty");
        rc = im->pack(&p, w, 2, 3);
        int32_t yr[2], yo[2];
        oma_rz_oracle(w, 2, 3, x, yo);
        CHECK(ci, rc == OMA_RZ_OK && im->run(&p, x, yr) == OMA_RZ_OK && yr[0] == yo[0] && yr[1] == yo[1],
              "repack into reused plan");
        oma_rz_free(&p);
    }
    gb_free(&W);
    gb_free(&T);
}

int main(void) {
    g_pg = (size_t)sysconf(_SC_PAGESIZE);
    signal(SIGSEGV, on_fault);
    signal(SIGBUS, on_fault);
    g_nc = omx_candidate_count();
    if (g_nc == 0 || g_nc > MAXC) {
        fprintf(stderr, "verify_polyglot: %zu candidates (need 1..%d)\n", g_nc, MAXC);
        return 2;
    }
    long nrand = 20000;
    const char *env = getenv("OMX_VERIFY_RANDOM");
    if (env && *env) nrand = atol(env);

    test_error_contract();

    /* MA-3 structured grid (tests/algebra/test_realize.c), each case placed
     * with guard pages in one of 8 flush patterns and small offsets. */
    static const size_t ns[] = {1, 2, 3, 4, 5, 7, 15, 16, 17, 31, 32, 33, 63, 64, 65, 79, 80, 81,
                                127, 128, 129, 159, 160, 161, 255, 256, 257, 1000, 1023, 1024, 1025};
    static const size_t ms[] = {1, 2, 3, 4, 5, 7, 15, 16, 17, 33};
    static const double sps[] = {0.0, 0.3, 0.6, 0.9, 1.0};
    size_t k = 0;
    for (size_t a = 0; a < sizeof ns / sizeof ns[0]; a++)
        for (size_t b = 0; b < sizeof ms / sizeof ms[0]; b++) {
            run_case(ms[b], ns[a], WK_ZERO, 0, XK_RANDOM, fixed_placement(k++));
            run_case(ms[b], ns[a], WK_POS, 0, XK_MIN, fixed_placement(k++));
            run_case(ms[b], ns[a], WK_POS, 0, XK_MAX, fixed_placement(k++));
            run_case(ms[b], ns[a], WK_NEG, 0, XK_MIN, fixed_placement(k++));
            run_case(ms[b], ns[a], WK_NEG, 0, XK_MAX, fixed_placement(k++));
            run_case(ms[b], ns[a], WK_RANDOM, sps[(a + b) % 5], XK_ALT, fixed_placement(k++));
            run_case(ms[b], ns[a], WK_RANDOM, sps[(a * 3 + b) % 5], XK_RANDOM, fixed_placement(k++));
        }
    size_t grid_cases = k;
    for (int it = 0; it < 400; it++) {
        size_t n = 1 + (size_t)(rnd() % 2100), m = 1 + (size_t)(rnd() % 70);
        run_case(m, n, WK_RANDOM, rnd01(), (int)(rnd() % 5), random_placement());
    }
    /* large magnitudes (MA-3 edges) */
    run_case(5, 16384, WK_NEG, 0, XK_MIN, fixed_placement(1));
    run_case(5, 16384, WK_POS, 0, XK_MIN, fixed_placement(2));
    run_case(3, 64512, WK_NEG, 0, XK_MIN, fixed_placement(3));
    run_case(3, 64512, WK_POS, 0, XK_MIN, fixed_placement(4));
    run_case(2, 64513, WK_NEG, 0, XK_MIN, fixed_placement(5));
    run_case(2, 65536, WK_RANDOM, 0.5, XK_RANDOM, fixed_placement(6));
    run_case(2, 65537, WK_NEG, 0, XK_MIN, fixed_placement(7));
    run_case(1, 100003, WK_NEG, 0, XK_MIN, fixed_placement(0));
    run_case(17, 4099, WK_RANDOM, 0.97, XK_RANDOM, fixed_placement(5));
    size_t ma3_cases = grid_cases + 400 + 9;

    /* 20,000 random cases: tails, unaligned pointers, x edges */
    static const double spc[] = {0.0, 0.33, 0.66, 1.0};
    for (long it = 0; it < nrand; it++) {
        uint64_t r = rnd() % 10;
        size_t n = r < 6 ? 1 + (size_t)(rnd() % 300) : r < 9 ? 1 + (size_t)(rnd() % 2100) : 1 + (size_t)(rnd() % 8200);
        size_t m = (rnd() % 8) ? 1 + (size_t)(rnd() % 70) : 1 + (size_t)(rnd() % 300);
        double sp = (rnd() & 1) ? spc[rnd() % 4] : rnd01();
        int wk = (rnd() % 20) == 0 ? (int)(1 + rnd() % 3) : WK_RANDOM;
        run_case(m, n, wk, sp, (int)(rnd() % XK_COUNT), random_placement());
    }

    unsigned long long tc = 0, tf = 0;
    size_t bad_cands = 0;
    for (size_t ci = 0; ci < g_nc; ci++) {
        const omx_candidate *c = omx_candidate_get(ci);
        printf("POLYGLOT verify %-16s %-14s %s: %llu checks, %llu failures, %llu cases (%s build)\n", c->impl->id,
               c->language, g_fail[ci] || g_checks[ci] == 0 ? "FAIL" : "PASS", g_checks[ci], g_fail[ci],
               g_cases[ci], OMX_VERIFY_BUILD);
        tc += g_checks[ci];
        tf += g_fail[ci];
        if (g_fail[ci] || g_checks[ci] == 0) bad_cands++;
    }
    int fail = bad_cands || g_hfail;
    printf("POLYGLOT verify-polyglot %s: %zu candidates (%zu failing), %llu checks, %llu failures, "
           "%zu MA-3 grid cases + %ld random cases, guard pages on W/x/y, harness %llu/%llu (%s build)\n",
           fail ? "FAIL" : "PASS", g_nc, bad_cands, tc, tf, ma3_cases, nrand, g_hchecks - g_hfail, g_hchecks,
           OMX_VERIFY_BUILD);
    return fail ? 1 : 0;
}
