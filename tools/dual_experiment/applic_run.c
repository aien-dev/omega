/* DUAL applicability experiment runner. Runs the pre-registered campaign
 * (AP_WL_COUNT workloads x AP_SEEDS seeds), prints the summary to stdout and
 * writes results.csv, params.txt and results.md under the given directory.
 * EXPERIMENTAL output only: nothing here is read by any production path.
 * Usage: applic_run <out_dir> */
#include "applic.h"
#include "sha256.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NROWS (AP_WL_COUNT * AP_SEEDS)

static ap_sim g_sim;
static uint32_t g_idx[AP_MAX_ITEMS];
static ap_row g_rows[NROWS];
static char g_params[2048];

static void hex(const uint8_t *d, char *out)
{
    static const char *h = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) { out[2 * i] = h[d[i] >> 4]; out[2 * i + 1] = h[d[i] & 15]; }
    out[64] = 0;
}
static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}
static double quantile(double *v, size_t n, double q)
{
    size_t k;
    if (n == 0) return 0.0;
    qsort(v, n, sizeof v[0], cmp_double);
    k = (size_t)floor(q * (double)(n - 1) + 0.5);
    return v[k];
}
typedef struct { double sum, sum2; size_t n; } acc;
static void add(acc *a, double x) { a->sum += x; a->sum2 += x * x; a->n++; }
static double mean(const acc *a) { return a->n ? a->sum / (double)a->n : 0.0; }
static double sd(const acc *a)
{
    double m = mean(a), v = a->n ? a->sum2 / (double)a->n - m * m : 0.0;
    return v > 0.0 ? sqrt(v) : 0.0;
}
static void emit(FILE *md, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    if (md) { va_start(ap, fmt); vfprintf(md, fmt, ap); va_end(ap); }
}
static double metric(const ap_hetero *h, int m)
{
    switch (m) {
    case 0: return h->cv_demand;
    case 1: return h->top_share;
    case 2: return h->max_item_share;
    case 3: return h->class_entropy;
    case 4: return h->arrival_cv;
    case 5: return h->broken_const;
    default: return h->broken_noise;
    }
}

static void section_per_workload(FILE *md, size_t n)
{
    size_t i, j;
    emit(md, "## 1. Aggregate predictor error per workload (mean over seeds, sd in parentheses)\n\n");
    emit(md, "| workload | completions NRMSE (primary) | known-arrivals completions NRMSE | queue RMSE | blocked RMSE | composite | known-arrivals composite | shuffled composite | cv_demand | top5 share | max item share | class entropy | arrival cv | rejected/run |\n");
    emit(md, "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n");
    for (i = 0; i < (size_t)AP_WL_COUNT; i++) {
        acc cn = {0,0,0}, kn = {0,0,0}, qn = {0,0,0}, bn = {0,0,0}, comp = {0,0,0}, kc = {0,0,0}, sh = {0,0,0};
        acc cv = {0,0,0}, top = {0,0,0}, mx = {0,0,0}, ent = {0,0,0}, acv = {0,0,0}, rej = {0,0,0};
        for (j = 0; j < n; j++) {
            const ap_row *r = &g_rows[j];
            if (r->wl != (uint8_t)i) continue;
            add(&cn, r->real.completions_nrmse); add(&kn, r->known_arrivals.completions_nrmse);
            add(&qn, r->real.queue_rmse); add(&bn, r->real.blocked_rmse);
            add(&comp, r->real.composite); add(&kc, r->known_arrivals.composite); add(&sh, r->shuffled.composite);
            add(&cv, r->h.cv_demand); add(&top, r->h.top_share); add(&mx, r->h.max_item_share);
            add(&ent, r->h.class_entropy); add(&acv, r->h.arrival_cv); add(&rej, (double)r->rejected);
        }
        emit(md, "| %s | %.3f (%.3f) | %.3f (%.3f) | %.3f (%.3f) | %.3f (%.3f) | %.3f (%.3f) | %.3f (%.3f) | %.3f (%.3f) | %.3f | %.3f | %.4f | %.3f | %.3f | %.1f |\n",
             ap_workload_name((ap_workload)i), mean(&cn), sd(&cn), mean(&kn), sd(&kn), mean(&qn), sd(&qn), mean(&bn), sd(&bn),
             mean(&comp), sd(&comp), mean(&kc), sd(&kc), mean(&sh), sd(&sh),
             mean(&cv), mean(&top), mean(&mx), mean(&ent), mean(&acv), mean(&rej));
    }
}

static void section_correlations(FILE *md, size_t n)
{
    static double xs[NROWS], yc[NROWS], yn[NROWS], yq[NROWS], yb[NROWS], yk[NROWS];
    static const char *const names[7] = { "cv_demand", "top5_share", "max_item_share", "class_entropy", "arrival_cv",
                                          "broken_const (control)", "broken_noise (control)" };
    size_t i;
    int m;
    emit(md, "\n## 2. Correlation of aggregate prediction error with heterogeneity measurements (Pearson r over all %zu runs)\n\n", n);
    emit(md, "| measurement | r vs completions NRMSE (primary) | r vs known-arrivals completions NRMSE | r vs queue RMSE | r vs blocked RMSE | r vs composite | covariance vs completions NRMSE | note |\n|---|---|---|---|---|---|---|---|\n");
    for (i = 0; i < n; i++) {
        yc[i] = g_rows[i].real.composite; yn[i] = g_rows[i].real.completions_nrmse;
        yq[i] = g_rows[i].real.queue_rmse; yb[i] = g_rows[i].real.blocked_rmse;
        yk[i] = g_rows[i].known_arrivals.completions_nrmse;
    }
    for (m = 0; m < 7; m++) {
        double cov;
        for (i = 0; i < n; i++) xs[i] = metric(&g_rows[i].h, m);
        emit(md, "| %s | %+.3f | %+.3f | %+.3f | %+.3f | %+.3f | %+.6f | %s |\n", names[m],
             ap_pearson(xs, yn, n, &cov), ap_pearson(xs, yk, n, NULL), ap_pearson(xs, yq, n, NULL),
             ap_pearson(xs, yb, n, NULL), ap_pearson(xs, yc, n, NULL), cov,
             m == 5 ? "zero variance: covariance exactly 0, r reported as 0" : m == 6 ? "independent of the run: expected near 0" : "");
    }
}

static void section_sweep(FILE *md, size_t n)
{
    static double sx[AP_SEEDS], st[AP_SEEDS], sy[AP_SEEDS], sk[AP_SEEDS];
    size_t i, k = 0;
    emit(md, "\n## 2b. Heterogeneity sweep (workload h only, %u runs, offered load held at %.2f, spread rises with seed index)\n\n", AP_SEEDS, AP_SWEEP_LOAD);
    for (i = 0; i < n; i++) if (g_rows[i].wl == (uint8_t)AP_WL_HETERO_SWEEP && k < AP_SEEDS) {
        sx[k] = g_rows[i].h.cv_demand; st[k] = g_rows[i].h.top_share;
        sy[k] = g_rows[i].real.completions_nrmse; sk[k] = g_rows[i].known_arrivals.completions_nrmse; k++;
    }
    emit(md, "- r(cv_demand, completions NRMSE) = %+.3f; r(cv_demand, known-arrivals completions NRMSE) = %+.3f\n",
         ap_pearson(sx, sy, k, NULL), ap_pearson(sx, sk, k, NULL));
    emit(md, "- r(top5_share, completions NRMSE) = %+.3f; r(top5_share, known-arrivals completions NRMSE) = %+.3f\n\n",
         ap_pearson(st, sy, k, NULL), ap_pearson(st, sk, k, NULL));
    emit(md, "| seed_index | sigma | cv_demand | top5 share | max item share | completions NRMSE | known-arrivals completions NRMSE | blocked RMSE |\n|---|---|---|---|---|---|---|---|\n");
    for (i = 0; i < n; i++) if (g_rows[i].wl == (uint8_t)AP_WL_HETERO_SWEEP) {
        const ap_row *r = &g_rows[i];
        emit(md, "| %u | %.3f | %.3f | %.3f | %.4f | %.3f | %.3f | %.3f |\n", r->seed_index, ap_sweep_sigma(r->seed_index),
             r->h.cv_demand, r->h.top_share, r->h.max_item_share, r->real.completions_nrmse,
             r->known_arrivals.completions_nrmse, r->real.blocked_rmse);
    }
}

static void section_shuffled(FILE *md, size_t n)
{
    size_t i, j, worse_total = 0;
    emit(md, "\n## 3. Negative control: shuffled-window predictor must be worse than the real predictor\n\n");
    emit(md, "| workload | runs where shuffled composite > real composite | mean real composite | mean shuffled composite |\n|---|---|---|---|\n");
    for (i = 0; i < (size_t)AP_WL_COUNT; i++) {
        size_t worse = 0, cnt = 0; acc re = {0,0,0}, sh = {0,0,0};
        for (j = 0; j < n; j++) {
            if (g_rows[j].wl != (uint8_t)i) continue;
            cnt++; add(&re, g_rows[j].real.composite); add(&sh, g_rows[j].shuffled.composite);
            if (g_rows[j].shuffled.composite > g_rows[j].real.composite) worse++;
        }
        worse_total += worse;
        emit(md, "| %s | %zu / %zu | %.3f | %.3f |\n", ap_workload_name((ap_workload)i), worse, cnt, mean(&re), mean(&sh));
    }
    emit(md, "\nshuffled worse in %zu / %zu runs: %s\n", worse_total, n, worse_total * 2 > n ? "CONTROL PASS (majority)" : "CONTROL FAIL");
}

static void section_grid(FILE *md, size_t n)
{
    static const double grid_cv[AP_GRID_CV_COUNT] = AP_GRID_CV;
    static const double grid_top[AP_GRID_TOP_COUNT] = AP_GRID_TOP;
    static double v[NROWS], v2[NROWS], vc[NROWS];
    size_t a, b, j, k;
    acc m1, mc;
    emit(md, "\n## 4. Regime grid (pre-registered thresholds): PRIMARY measure = completions NRMSE of runs with cv_demand < Y and top5 share < Z (composite beside it)\n\n");
    emit(md, "| Y (cv) | Z (top5) | runs | mean completions NRMSE | p90 | max | mean composite | p90 composite | runs also arrival_cv < %.2f | p90 completions NRMSE (that subset) |\n|---|---|---|---|---|---|---|---|---|---|\n", AP_GRID_ARRIVAL_CV);
    for (a = 0; a < AP_GRID_CV_COUNT; a++) for (b = 0; b < AP_GRID_TOP_COUNT; b++) {
        size_t k2 = 0; double mx = 0.0;
        k = 0; memset(&m1, 0, sizeof m1); memset(&mc, 0, sizeof mc);
        for (j = 0; j < n; j++) {
            const ap_row *r = &g_rows[j];
            if (r->h.cv_demand < grid_cv[a] && r->h.top_share < grid_top[b]) {
                v[k] = r->real.completions_nrmse; vc[k] = r->real.composite; k++;
                add(&m1, r->real.completions_nrmse); add(&mc, r->real.composite);
                if (r->real.completions_nrmse > mx) mx = r->real.completions_nrmse;
                if (r->h.arrival_cv < AP_GRID_ARRIVAL_CV) v2[k2++] = r->real.completions_nrmse;
            }
        }
        emit(md, "| %.1f | %.2f | %zu | %.3f | %.3f | %.3f | %.3f | %.3f | %zu | %.3f |\n", grid_cv[a], grid_top[b], k, mean(&m1),
             quantile(v, k, 0.9), mx, mean(&mc), quantile(vc, k, 0.9), k2, quantile(v2, k2, 0.9));
    }
    k = 0; memset(&m1, 0, sizeof m1);
    for (j = 0; j < n; j++) if (g_rows[j].h.cv_demand >= grid_cv[AP_GRID_CV_COUNT - 1] || g_rows[j].h.top_share >= grid_top[AP_GRID_TOP_COUNT - 1]) {
        v[k++] = g_rows[j].real.completions_nrmse; add(&m1, g_rows[j].real.completions_nrmse);
    }
    emit(md, "\ncomplement (cv >= %.1f or top5 >= %.2f): %zu runs, mean completions NRMSE %.3f, p10 %.3f, p90 %.3f\n",
         grid_cv[AP_GRID_CV_COUNT - 1], grid_top[AP_GRID_TOP_COUNT - 1], k, mean(&m1), quantile(v, k, 0.1), quantile(v, k, 0.9));
    k = 0; memset(&m1, 0, sizeof m1);
    for (j = 0; j < n; j++) if (g_rows[j].h.arrival_cv >= AP_GRID_ARRIVAL_CV) {
        v[k++] = g_rows[j].real.completions_nrmse; add(&m1, g_rows[j].real.completions_nrmse);
    }
    emit(md, "temporal complement (arrival_cv >= %.2f): %zu runs, mean completions NRMSE %.3f, p10 %.3f, p90 %.3f\n",
         AP_GRID_ARRIVAL_CV, k, mean(&m1), quantile(v, k, 0.1), quantile(v, k, 0.9));
}

int main(int argc, char **argv)
{
    size_t n = 0, i;
    uint8_t dig[32];
    char hx[65], path[1024];
    FILE *f, *md;
    const char *out_dir = argc > 1 ? argv[1] : "evidence/DUAL/applicability";

    if (ap_params_text(g_params, sizeof g_params) < 0) { fprintf(stderr, "params text overflow\n"); return 2; }
    if (ap_campaign(&g_sim, g_idx, g_rows, NROWS, &n) != 0) { fprintf(stderr, "campaign failed (ledger or pool)\n"); return 2; }

    snprintf(path, sizeof path, "%s/results.csv", out_dir);
    f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return 2; }
    fprintf(f, "%s\n", ap_row_header());
    for (i = 0; i < n; i++) { char line[512]; if (ap_row_format(&g_rows[i], line, sizeof line) >= 0) fprintf(f, "%s\n", line); }
    fclose(f);
    snprintf(path, sizeof path, "%s/params.txt", out_dir);
    f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return 2; }
    fputs(g_params, f); fclose(f);
    snprintf(path, sizeof path, "%s/results.md", out_dir);
    md = fopen(path, "w");
    if (!md) { fprintf(stderr, "cannot write %s\n", path); return 2; }

    emit(md, "# DUAL applicability experiment: results (%s)\n\n", AP_EXPERIMENT_NAME);
    emit(md, "EXPERIMENTAL. Synthetic workloads. Not a production gate, not a production classifier. Nothing inserted into production.\n\n");
    ap_params_digest(dig); hex(dig, hx);
    emit(md, "- parameter digest (SHA-256 of params.txt): `%s`\n", hx);
    ap_rows_digest(g_rows, n, dig); hex(dig, hx);
    emit(md, "- results digest (SHA-256 of results.csv text): `%s`\n", hx);
    emit(md, "- runs: %zu (%u workloads x %u seeds), %u windows of %u steps each, %u warm-up windows unscored\n\n",
         n, (unsigned)AP_WL_COUNT, AP_SEEDS, AP_WINDOWS, AP_WINDOW, AP_WARMUP_WINDOWS);
    section_per_workload(md, n);
    section_correlations(md, n);
    section_sweep(md, n);
    section_shuffled(md, n);
    section_grid(md, n);
    emit(md, "\nVerdict line: EXPERIMENTAL; recommends a future regime classifier; nothing inserted into production.\n");
    fclose(md);
    printf("wrote %s/results.csv, params.txt, results.md\n", out_dir);
    return 0;
}
