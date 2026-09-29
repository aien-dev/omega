/* MA-2 correctness gate: every realization of Omega-X (y = W.x, ternary W,
 * int8 x, int32 y) must be bit-identical to the naive oracle.
 * The oracle itself is cross-checked row by row against the reference
 * library (oma_pack_bitplane + oma_dot_tw_i8). Fixed seed; reproducible. */
#include "algebra/oma_pack.h"
#include "algebra/oma_select.h"
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


/* ---- selector tie rule (ADR 0019 section 9.1), in-memory cost tables ---- */
static oma_sel_table g_st;

static void sel_row(int run, const char *rz, double median_ns, double noise_rel) {
    oma_sel_row *r = &g_st.rows[g_st.nrows++];
    memset(r, 0, sizeof *r);
    r->run = run;
    r->n = 1024;
    r->m = 64;
    r->sparsity = 0.3;
    snprintf(r->rz, sizeof r->rz, "%s", rz);
    r->eligible = r->verified = 1;
    r->median_ns = r->min_ns = median_ns;
    r->noise_rel = noise_rel;
    r->pack_ns = 10;
}

static void sel_decide(const char *incumbent, oma_sel_decision *d) {
    oma_sel_query q = {1024, 64, 0.3, 0, -1, incumbent};
    CHECK(oma_sel_decide(&g_st, &q, d) == OMA_SEL_OK, "selector rc");
}

/* chosen must always be a member of the tied set (ADR 0019 9.1) */
static int chosen_in_tied_set(const oma_sel_decision *d) {
    for (size_t i = 0; i < d->ntie; i++)
        if (strcmp(d->tie_set[i], d->chosen) == 0) return 1;
    return 0;
}

static void expect(const oma_sel_decision *d, int tie, const char *chosen, const char *res, const char *what) {
    CHECK(d->tie == tie && strcmp(d->chosen, chosen) == 0 && strcmp(d->tie_resolution, res) == 0,
          "%s: want %s/%s tie=%d, got %s/%s tie=%d", what, chosen, res, tie, d->chosen, d->tie_resolution, d->tie);
    CHECK(chosen_in_tied_set(d), "%s: chosen %s outside the tied set", what, d->chosen);
    CHECK(d->tie == (d->ntie > 1), "%s: tie iff tied set has >1 member", what);
}

static void test_selector_tie_rule(void) {
    oma_sel_decision d;
    /* TIE: R1_sdot 100 vs R1_smmla 101, band 5%; R1_plain (reference) at 300
     * is outside the tied set and must never be chosen by tie resolution */
    oma_sel_init(&g_st);
    g_st.nruns = 1;
    sel_row(0, "R1_sdot", 100, 0.05);
    sel_row(0, "R1_smmla", 101, 0.05);
    sel_row(0, "R1_plain", 300, 0.05);
    sel_decide(NULL, &d);
    CHECK(strcmp(d.cheapest, "R1_sdot") == 0 && strcmp(d.runner_up, "R1_smmla") == 0, "cheapest/runner-up");
    expect(&d, 1, "R1_sdot", "cheapest", "reference outside tied set, no incumbent");
    CHECK(d.ntie == 2 && oma_sel_in_tie_set(&d, "R1_sdot") && oma_sel_in_tie_set(&d, "R1_smmla") &&
              !oma_sel_in_tie_set(&d, OMA_SEL_REFERENCE),
          "tied set = {R1_sdot, R1_smmla}");
    CHECK(d.chosen_cost_ns == 100, "chosen cost is the cheapest's");
    sel_decide("", &d);
    expect(&d, 1, "R1_sdot", "cheapest", "empty incumbent");
    /* incumbent inside the tied set stands */
    sel_decide("R1_smmla", &d);
    expect(&d, 1, "R1_smmla", "incumbent", "incumbent (runner-up) stands");
    CHECK(d.chosen_cost_ns == 101, "chosen cost is the incumbent's");
    sel_decide("R1_sdot", &d);
    expect(&d, 1, "R1_sdot", "incumbent", "incumbent = cheapest");
    /* incumbent outside the tied set (the reference here) is not chosen */
    sel_decide("R1_plain", &d);
    expect(&d, 1, "R1_sdot", "cheapest", "incumbent outside tied set");
    /* ineligible or unknown incumbent, reference outside: cheapest */
    sel_decide("R4_rns", &d);
    expect(&d, 1, "R1_sdot", "cheapest", "unmeasured incumbent");
    sel_decide("no_such_rz", &d);
    expect(&d, 1, "R1_sdot", "cheapest", "unknown incumbent");
    /* reference inside the tied set: selected when there is no incumbent in it */
    oma_sel_init(&g_st);
    g_st.nruns = 1;
    sel_row(0, "R1_sdot", 100, 0.05);
    sel_row(0, "R1_smmla", 101, 0.05);
    sel_row(0, "R1_plain", 103, 0.05);
    sel_row(0, "R2c_crumb", 200, 0.05);
    sel_decide(NULL, &d);
    expect(&d, 1, OMA_SEL_REFERENCE, "reference", "reference inside tied set");
    CHECK(d.ntie == 3 && !oma_sel_in_tie_set(&d, "R2c_crumb"), "tied set of 3, R2c_crumb outside");
    CHECK(d.chosen_cost_ns == 103, "chosen cost is the reference's");
    sel_decide("R1_smmla", &d);
    expect(&d, 1, "R1_smmla", "incumbent", "incumbent beats reference inside tied set");
    sel_decide("R2c_crumb", &d);
    expect(&d, 1, OMA_SEL_REFERENCE, "reference", "incumbent outside, reference inside");
    /* no TIE: cheapest chosen, incumbent ignored */
    oma_sel_init(&g_st);
    g_st.nruns = 1;
    sel_row(0, "R1_sdot", 100, 0.01);
    sel_row(0, "R1_smmla", 150, 0.01);
    sel_row(0, "R1_plain", 300, 0.01);
    sel_decide("R1_plain", &d);
    expect(&d, 0, "R1_sdot", "none", "no tie");
    CHECK(d.ntie == 1 && oma_sel_in_tie_set(&d, "R1_sdot") && !oma_sel_in_tie_set(&d, "R1_smmla"), "no-tie set");
    /* TIE with the reference not measured and no incumbent: cheapest stands */
    oma_sel_init(&g_st);
    g_st.nruns = 1;
    sel_row(0, "R1_sdot", 100, 0.05);
    sel_row(0, "R1_smmla", 101, 0.05);
    sel_decide(NULL, &d);
    expect(&d, 1, "R1_sdot", "cheapest", "reference unmeasured");
    /* reference verified in one run only: ineligible over all runs, even though
     * its cost would put it inside the tied set */
    oma_sel_init(&g_st);
    g_st.nruns = 2;
    sel_row(0, "R1_sdot", 100, 0.05);
    sel_row(0, "R1_smmla", 101, 0.05);
    sel_row(0, "R1_plain", 102, 0.05);
    sel_row(1, "R1_sdot", 100, 0.05);
    sel_row(1, "R1_smmla", 101, 0.05);
    sel_row(1, "R1_plain", 102, 0.05);
    g_st.rows[g_st.nrows - 1].verified = 0;
    sel_decide(NULL, &d);
    expect(&d, 1, "R1_sdot", "cheapest", "unverified reference");
    CHECK(!oma_sel_in_tie_set(&d, OMA_SEL_REFERENCE), "ineligible reference not in tied set");
    /* cross-run spread alone makes a TIE: 100/104 in run 1, 104/100 in run 2;
     * the reference at 300 stays outside */
    oma_sel_init(&g_st);
    g_st.nruns = 2;
    sel_row(0, "R1_sdot", 100, 0.001);
    sel_row(0, "R1_smmla", 104, 0.001);
    sel_row(0, "R1_plain", 300, 0.001);
    sel_row(1, "R1_sdot", 104, 0.001);
    sel_row(1, "R1_smmla", 100, 0.001);
    sel_row(1, "R1_plain", 300, 0.001);
    sel_decide(NULL, &d);
    CHECK(d.tie == 1 && d.ntie == 2 && strcmp(d.chosen, OMA_SEL_REFERENCE) != 0 &&
              strcmp(d.tie_resolution, "cheapest") == 0 && chosen_in_tied_set(&d),
          "cross-run tie, got %s %s", d.chosen, d.tie_resolution);
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
    test_selector_tie_rule();

    printf("MA2 test-realize %s: %llu checks, %llu failures, %zu realizations\n",
           g_fail ? "FAIL" : "PASS", g_checks, g_fail, oma_rz_count());
    return g_fail ? 1 : 0;
}
