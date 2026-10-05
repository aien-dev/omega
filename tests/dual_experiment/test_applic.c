/* DUAL applicability experiment tests: determinism (same seed -> byte-identical
 * results digest), exact ledger conservation at every step, predictor finite
 * and bounded outputs, negative controls (shuffled predictor worse, constant
 * metric has zero covariance), parameter digest pinned. Plain C, assert-style:
 * every CHECK counts as one test case; any failure exits nonzero. */
#include "applic.h"
#include "sha256.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* Pre-registered: SHA-256 of ap_params_text(). Any parameter change breaks
 * this pin on purpose; a new value means a new experiment version. */
#define AP_PARAMS_DIGEST_PIN "e489d25f5f0d23cfe8624a5cc82a5f770f04cb03fd3b441aac627e1a0087ecea"

static ap_sim g_a, g_b;
static uint32_t g_idx[AP_MAX_ITEMS];
static ap_row g_rows_a[AP_WL_COUNT * AP_SEEDS], g_rows_b[AP_WL_COUNT * AP_SEEDS];

static void hex(const uint8_t *d, char *out)
{
    static const char *h = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) { out[2 * i] = h[d[i] >> 4]; out[2 * i + 1] = h[d[i] & 15]; }
    out[64] = 0;
}

static void test_ledger_every_step(void)
{
    uint32_t wl, si;
    for (wl = 0; wl < (uint32_t)AP_WL_COUNT; wl++) for (si = 0; si < 3; si++) {
        uint32_t t, w;
        ap_sim_init_indexed(&g_a, (ap_workload)wl, ap_seed((ap_workload)wl, si), si);
        for (t = 0; t < AP_STEPS; t++) {
            int r = ap_sim_step(&g_a);
            if (r != 0 || !ap_sim_ledger_ok(&g_a)) { CHECK(0 && "ledger violated or pool exhausted"); break; }
        }
        CHECK(g_a.step == AP_STEPS);
        CHECK(g_a.n_windows == AP_WINDOWS);
        CHECK(g_a.pool_exhausted == 0);
        CHECK(g_a.arrived > 0 && g_a.completed > 0);
        /* window ledger: window counts sum to totals */
        {
            uint64_t a = 0, c = 0, rj = 0;
            for (w = 0; w < g_a.n_windows; w++) { a += g_a.windows[w].arrivals; c += g_a.windows[w].completions; rj += g_a.windows[w].rejected; }
            CHECK(a == g_a.arrived); CHECK(c == g_a.completed); CHECK(rj == g_a.rejected);
            /* capacity respected in every window */
            for (w = 0; w < g_a.n_windows; w++) {
                CHECK(g_a.windows[w].used_slots_frac <= 1.0 + 1e-12);
                CHECK(g_a.windows[w].used_mem_frac <= 1.0 + 1e-12);
                CHECK(g_a.windows[w].queue_density <= 1.0 + 1e-12);
                CHECK(g_a.windows[w].blocked_steps <= AP_WINDOW);
            }
        }
    }
    /* workload (f) actually drops capacity; (c) actually rejects under overload */
    ap_sim_init(&g_a, AP_WL_CAPACITY_DROP, ap_seed(AP_WL_CAPACITY_DROP, 0)); ap_sim_run(&g_a);
    CHECK(g_a.windows[0].slots_capacity == AP_SLOTS);
    CHECK(g_a.windows[AP_WINDOWS - 1].slots_capacity == AP_CAP_DROP_SLOTS);
    ap_sim_init(&g_a, AP_WL_STEP, ap_seed(AP_WL_STEP, 0)); ap_sim_run(&g_a);
    CHECK(g_a.rejected > 0);
    /* (h): the sweep covers every seed index; spread and rate are set and the ledger holds */
    for (si = 0; si < AP_SEEDS; si++) {
        ap_sim_init_indexed(&g_a, AP_WL_HETERO_SWEEP, ap_seed(AP_WL_HETERO_SWEEP, si), si);
        CHECK(g_a.sweep_rate > 0.0 && g_a.het_sigma == ap_sweep_sigma(si));
        CHECK(ap_sim_run(&g_a) == 0 && g_a.pool_exhausted == 0);
    }
    CHECK(ap_sweep_sigma(0) == AP_SWEEP_SIGMA_MIN && ap_sweep_sigma(AP_SEEDS - 1u) == AP_SWEEP_SIGMA_MAX);
    /* hostile: a corrupted ledger is detected */
    g_a.completed++;
    CHECK(!ap_sim_ledger_ok(&g_a));
    g_a.completed--;
    CHECK(ap_sim_ledger_ok(&g_a));
}

static void test_determinism(void)
{
    size_t na = 0, nb = 0;
    uint8_t da[32], db[32];
    char ha[65], hb[65];
    ap_sim_init(&g_a, AP_WL_HETEROGENEOUS, 12345u); ap_sim_run(&g_a);
    ap_sim_init(&g_b, AP_WL_HETEROGENEOUS, 12345u); ap_sim_run(&g_b);
    CHECK(memcmp(g_a.windows, g_b.windows, sizeof g_a.windows) == 0);
    CHECK(g_a.n_items == g_b.n_items && memcmp(g_a.items, g_b.items, sizeof(ap_item) * g_a.n_items) == 0);
    ap_sim_init(&g_b, AP_WL_HETEROGENEOUS, 12346u); ap_sim_run(&g_b);
    CHECK(memcmp(g_a.windows, g_b.windows, sizeof g_a.windows) != 0);   /* the seed matters */

    CHECK(ap_campaign(&g_a, g_idx, g_rows_a, AP_WL_COUNT * AP_SEEDS, &na) == 0);
    CHECK(ap_campaign(&g_b, g_idx, g_rows_b, AP_WL_COUNT * AP_SEEDS, &nb) == 0);
    CHECK(na == nb && na == (size_t)AP_WL_COUNT * AP_SEEDS);
    ap_rows_digest(g_rows_a, na, da); ap_rows_digest(g_rows_b, nb, db);
    hex(da, ha); hex(db, hb);
    CHECK(strcmp(ha, hb) == 0);
    printf("results digest: %s\n", ha);
    {
        uint8_t pd[32]; char ph[65];
        ap_params_digest(pd); hex(pd, ph);
        printf("params digest:  %s\n", ph);
        CHECK(strcmp(ph, AP_PARAMS_DIGEST_PIN) == 0);
    }
}

static void test_predictor_finite(void)
{
    size_t i;
    uint32_t w;
    for (i = 0; i < (size_t)AP_WL_COUNT; i++) {
        ap_sim_init(&g_a, (ap_workload)i, ap_seed((ap_workload)i, 7)); ap_sim_run(&g_a);
        for (w = 0; w < g_a.n_windows; w++) {
            ap_prediction p = ap_predict(&g_a.windows[w]);
            CHECK(isfinite(p.completions) && p.completions >= 0.0);
            CHECK(isfinite(p.queue_density) && p.queue_density >= 0.0 && p.queue_density <= 1.0);
            CHECK(isfinite(p.blocked) && p.blocked >= 0.0 && p.blocked <= 1.0);
        }
        {
            ap_error e = ap_error_real(&g_a), s = ap_error_shuffled(&g_a, 3), k = ap_error_known_arrivals(&g_a);
            CHECK(isfinite(e.composite) && e.composite >= 0.0 && isfinite(s.composite) && isfinite(k.composite) && k.composite >= 0.0);
            CHECK(e.windows == AP_WINDOWS - 1 - AP_WARMUP_WINDOWS);
        }
    }
    /* a zero window (nothing in system, no arrivals) predicts zero everywhere */
    {
        ap_window z; ap_prediction p;
        memset(&z, 0, sizeof z); z.slots_capacity = AP_SLOTS;
        p = ap_predict(&z);
        CHECK(p.completions == 0.0 && p.queue_density == 0.0 && p.blocked == 0.0);
    }
}

static void test_negative_controls(void)
{
    size_t i, worse = 0;
    static double x[AP_WL_COUNT * AP_SEEDS], y[AP_WL_COUNT * AP_SEEDS];
    double cov = 1.0, r;
    for (i = 0; i < (size_t)AP_WL_COUNT * AP_SEEDS; i++) {
        if (g_rows_a[i].shuffled.composite > g_rows_a[i].real.composite) worse++;
        x[i] = g_rows_a[i].h.broken_const; y[i] = g_rows_a[i].real.composite;
        CHECK(isfinite(g_rows_a[i].h.cv_demand) && g_rows_a[i].h.cv_demand >= 0.0);
        CHECK(g_rows_a[i].h.top_share >= 0.0 && g_rows_a[i].h.top_share <= 1.0);
        CHECK(g_rows_a[i].h.max_item_share >= 0.0 && g_rows_a[i].h.max_item_share <= g_rows_a[i].h.top_share + 1e-12);
        CHECK(g_rows_a[i].h.class_entropy >= 0.0 && g_rows_a[i].h.class_entropy <= 1.0 + 1e-12);
    }
    printf("shuffled worse than real in %zu / %u runs\n", worse, (unsigned)(AP_WL_COUNT * AP_SEEDS));
    CHECK(worse * 2 > (size_t)AP_WL_COUNT * AP_SEEDS);
    r = ap_pearson(x, y, AP_WL_COUNT * AP_SEEDS, &cov);
    CHECK(cov == 0.0 && r == 0.0);
    /* heterogeneous workload is measurably more heterogeneous than homogeneous */
    {
        double cv_a = 0.0, cv_b = 0.0, top_a = 0.0, top_b = 0.0;
        for (i = 0; i < (size_t)AP_WL_COUNT * AP_SEEDS; i++) {
            if (g_rows_a[i].wl == AP_WL_HOMOGENEOUS) { cv_a += g_rows_a[i].h.cv_demand; top_a += g_rows_a[i].h.top_share; }
            if (g_rows_a[i].wl == AP_WL_HETEROGENEOUS) { cv_b += g_rows_a[i].h.cv_demand; top_b += g_rows_a[i].h.top_share; }
        }
        CHECK(cv_b > cv_a && top_b > top_a);
    }
    /* pearson sanity */
    {
        double a[4] = {1, 2, 3, 4}, b[4] = {2, 4, 6, 8}, c[4] = {4, 3, 2, 1};
        CHECK(fabs(ap_pearson(a, b, 4, NULL) - 1.0) < 1e-12);
        CHECK(fabs(ap_pearson(a, c, 4, NULL) + 1.0) < 1e-12);
    }
}

int main(void)
{
    test_ledger_every_step();
    test_determinism();
    test_predictor_finite();
    test_negative_controls();
    printf("test_applic: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
