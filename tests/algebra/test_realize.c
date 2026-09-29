/* MA-3 correctness gate: every realization of Omega-X (y = W.x, ternary W,
 * int8 x, int32 y) must be bit-identical to the naive oracle.
 * The oracle itself is cross-checked row by row against the reference
 * library (oma_pack_bitplane + oma_dot_tw_i8). Fixed seed; reproducible. */
#include "algebra/oma_pack.h"
#include "algebra/oma_trit.h"
#include "algebra/realize_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long g_checks, g_fail;
static uint64_t g_rng = 0x4d41332d7265616cULL; /* "MA3-real" */

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

/* Oracle cross-check against the reference library for every row. */
static void check_oracle_vs_library(const int8_t *w, size_t m, size_t n, const int8_t *x,
                                    const int32_t *yref) {
    size_t nb = oma_bitplane_blocks(n);
    oma_block *blk = malloc((nb ? nb : 1) * sizeof *blk);
    if (!blk) { CHECK(0, "oom"); return; }
    for (size_t i = 0; i < m; i++) {
        int rc = oma_pack_bitplane(w + i * n, n, blk, nb);
        CHECK(rc == OMA_OK, "oma_pack_bitplane rc %d", rc);
        int32_t v = 0;
        rc = oma_dot_tw_i8(blk, x, n, &v);
        CHECK(rc == OMA_OK && v == yref[i], "library dot row %zu: rc %d %d vs oracle %d", i, rc, v, yref[i]);
    }
    free(blk);
}

static void run_case(size_t m, size_t n, int wk, double sp, int xk, int lib_check) {
    int8_t *w = malloc(m * n), *x = malloc(n);
    int32_t *yref = malloc(m * sizeof *yref), *y = malloc(m * sizeof *y);
    if (!w || !x || !yref || !y) { CHECK(0, "oom"); goto out; }
    fill_w(w, m * n, wk, sp);
    fill_x(x, n, xk);
    CHECK(oma_rz_oracle(w, m, n, x, yref) == OMA_RZ_OK, "oracle rc");
    if (lib_check) check_oracle_vs_library(w, m, n, x, yref);
    for (size_t r = 0; r < oma_rz_count(); r++) {
        const oma_rz_impl *im = oma_rz_get(r);
        oma_rz_plan p;
        memset(&p, 0, sizeof p);
        int rc = im->pack(&p, w, m, n);
        if (n > im->max_n) {
            CHECK(rc == OMA_RZ_E_ARG, "%s must refuse n=%zu > max_n, rc %d", im->id, n, rc);
            continue;
        }
        CHECK(rc == OMA_RZ_OK, "%s pack m=%zu n=%zu rc %d (%s)", im->id, m, n, rc, oma_rz_strerror(rc));
        if (rc) continue;
        for (size_t i = 0; i < m; i++) y[i] = (int32_t)0x5a5a5a5a;
        /* run twice: per-call scratch must not leak state between calls */
        for (int rep = 0; rep < 2; rep++) {
            rc = im->run(&p, x, y);
            CHECK(rc == OMA_RZ_OK, "%s run rc %d", im->id, rc);
            size_t bad = 0, first = 0;
            for (size_t i = 0; i < m; i++)
                if (y[i] != yref[i] && bad++ == 0) first = i;
            CHECK(bad == 0, "%s m=%zu n=%zu wk=%d sp=%.2f xk=%d: %zu rows differ, first row %zu got %d want %d",
                  im->id, m, n, wk, sp, xk, bad, first, y[first], yref[first]);
        }
        CHECK(p.weight_bytes > 0 && p.footprint_bytes >= p.weight_bytes, "%s byte accounting", im->id);
        /* dense5: every byte is a valid reference dense byte (< 243) and
         * decodes with the library to the right trits (strided layout). */
        if (strcmp(im->id, "R5_dense5") == 0) {
            size_t chunks = (n + 79) / 80, rb = chunks * 16;
            const uint8_t *b = p.mem;
            size_t bad = 0;
            for (size_t i = 0; i < m && bad == 0; i++)
                for (size_t c = 0; c < chunks; c++)
                    for (size_t j = 0; j < 16; j++) {
                        int8_t t[5];
                        if (oma_dense_byte_decode(b[i * rb + c * 16 + j], t) != OMA_OK) { bad++; continue; }
                        for (size_t k = 0; k < 5; k++) {
                            size_t col = c * 80 + 16 * k + j;
                            int8_t want = col < n ? w[i * n + col] : 0;
                            if (t[k] != want) bad++;
                        }
                    }
            CHECK(bad == 0, "R5_dense5 bytes vs oma_dense_byte_decode: %zu bad", bad);
        }
        oma_rz_free(&p);
    }
out:
    free(w); free(x); free(yref); free(y);
}

static void test_rejections(void) {
    int8_t w[6] = {1, 0, -1, 2, 0, 1}, x[3] = {1, 2, 3};
    for (size_t r = 0; r < oma_rz_count(); r++) {
        const oma_rz_impl *im = oma_rz_get(r);
        oma_rz_plan p;
        memset(&p, 0, sizeof p);
        CHECK(im->pack(&p, w, 2, 3) == OMA_RZ_E_TRIT, "%s accepts weight 2", im->id);
        w[3] = -2;
        CHECK(im->pack(&p, w, 2, 3) == OMA_RZ_E_TRIT, "%s accepts weight -2", im->id);
        w[3] = (int8_t)-128;
        CHECK(im->pack(&p, w, 2, 3) == OMA_RZ_E_TRIT, "%s accepts weight -128", im->id);
        w[3] = 2;
        CHECK(im->pack(&p, w, 0, 3) == OMA_RZ_E_ARG, "%s accepts m=0", im->id);
        CHECK(im->pack(&p, w, 2, 0) == OMA_RZ_E_ARG, "%s accepts n=0", im->id);
        CHECK(im->pack(&p, NULL, 2, 3) == OMA_RZ_E_ARG, "%s accepts NULL", im->id);
        /* shape is checked before any weight is read */
        CHECK(im->pack(&p, w, 1, OMA_RZ_MAX_N + 1) == OMA_RZ_E_ARG, "%s accepts n above int32 bound", im->id);
        CHECK(im->max_n <= OMA_RZ_MAX_N, "%s max_n above int32 bound", im->id);
        CHECK(im->exact == 1, "%s not exact", im->id);
        oma_rz_plan empty;
        memset(&empty, 0, sizeof empty);
        int32_t y[2];
        CHECK(im->run(&empty, x, y) == OMA_RZ_E_ARG, "%s runs an empty plan", im->id);
    }
    /* overflow contract: 128 * OMA_RZ_MAX_N fits int32, one more does not */
    CHECK((int64_t)128 * (int64_t)OMA_RZ_MAX_N <= INT32_MAX, "bound");
    CHECK((int64_t)128 * (int64_t)(OMA_RZ_MAX_N + 1) > INT32_MAX, "bound tight");
}

int main(void) {
    test_rejections();

    static const size_t ns[] = {1, 2, 3, 4, 5, 7, 15, 16, 17, 31, 32, 33, 63, 64, 65, 79, 80, 81,
                                127, 128, 129, 159, 160, 161, 255, 256, 257, 1000, 1023, 1024, 1025};
    static const size_t ms[] = {1, 2, 3, 4, 5, 7, 15, 16, 17, 33};
    static const double sps[] = {0.0, 0.3, 0.6, 0.9, 1.0};
    /* structured: every n x m tail combination, every weight/x kind */
    for (size_t a = 0; a < sizeof ns / sizeof ns[0]; a++)
        for (size_t b = 0; b < sizeof ms / sizeof ms[0]; b++) {
            run_case(ms[b], ns[a], WK_ZERO, 0, XK_RANDOM, 0);
            run_case(ms[b], ns[a], WK_POS, 0, XK_MIN, 0);
            run_case(ms[b], ns[a], WK_POS, 0, XK_MAX, 0);
            run_case(ms[b], ns[a], WK_NEG, 0, XK_MIN, 0);
            run_case(ms[b], ns[a], WK_NEG, 0, XK_MAX, 0);
            run_case(ms[b], ns[a], WK_RANDOM, sps[(a + b) % 5], XK_ALT, 1);
            run_case(ms[b], ns[a], WK_RANDOM, sps[(a * 3 + b) % 5], XK_RANDOM, 1);
        }
    /* random shapes, random sparsity in [0,1] */
    for (int it = 0; it < 400; it++) {
        size_t n = 1 + (size_t)(rnd() % 2100), m = 1 + (size_t)(rnd() % 70);
        int xk = (int)(rnd() % 5);
        run_case(m, n, WK_RANDOM, rnd01(), xk, it % 4 == 0);
    }
    /* large magnitudes: bench-size n, and the RNS / sparse edges */
    run_case(5, 16384, WK_NEG, 0, XK_MIN, 1);   /* y = +2,097,152 */
    run_case(5, 16384, WK_POS, 0, XK_MIN, 0);   /* y = -2,097,152 */
    run_case(3, 64512, WK_NEG, 0, XK_MIN, 0);   /* RNS limit: y = 8,257,536 */
    run_case(3, 64512, WK_POS, 0, XK_MIN, 0);
    run_case(2, 64513, WK_NEG, 0, XK_MIN, 0);   /* RNS must refuse */
    run_case(2, 65536, WK_RANDOM, 0.5, XK_RANDOM, 0); /* sparse uint16 limit */
    run_case(2, 65537, WK_NEG, 0, XK_MIN, 0);   /* sparse must refuse */
    run_case(1, 100003, WK_NEG, 0, XK_MIN, 1);  /* y = 12,800,384 */
    run_case(17, 4099, WK_RANDOM, 0.97, XK_RANDOM, 1);

    printf("MA3 test-realize %s: %llu checks, %llu failures, %zu realizations\n",
           g_fail ? "FAIL" : "PASS", g_checks, g_fail, oma_rz_count());
    return g_fail ? 1 : 0;
}
