/* Heterogeneity measurements, campaign driver, canonical row text, digests
 * and Pearson correlation for the DUAL applicability experiment. */
#include "applic.h"
#include "sha256.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* load of one item in slot-steps */
static double item_load(const ap_item *it) { return (double)it->demand0 * (double)it->slots; }

/* Deterministic in-place heapsort of item indexes by load, descending; ties
 * broken by index so the order never depends on the sort algorithm. */
static int load_before(const ap_sim *s, uint32_t a, uint32_t b)
{
    double la = item_load(&s->items[a]), lb = item_load(&s->items[b]);
    if (la != lb) return la > lb;
    return a < b;
}
static void sift(const ap_sim *s, uint32_t *idx, uint32_t root, uint32_t n)
{
    /* max-heap on "smaller load first" so that popping yields descending order */
    for (;;) {
        uint32_t child = 2u * root + 1u, pick = root, tmp;
        if (child >= n) return;
        if (!load_before(s, idx[child], idx[pick])) pick = child;
        if (child + 1u < n && !load_before(s, idx[child + 1u], idx[pick])) pick = child + 1u;
        if (pick == root) return;
        tmp = idx[root]; idx[root] = idx[pick]; idx[pick] = tmp; root = pick;
    }
}
static void sort_desc(const ap_sim *s, uint32_t *idx, uint32_t n)
{
    uint32_t i;
    if (n < 2) return;
    for (i = n / 2u; i-- > 0;) sift(s, idx, i, n);
    for (i = n - 1u; i > 0; i--) { uint32_t t = idx[0]; idx[0] = idx[i]; idx[i] = t; sift(s, idx, 0, i); }
}

ap_hetero ap_measure(const ap_sim *s, uint32_t *idx, ap_rng *noise)
{
    ap_hetero h;
    uint32_t n = s->n_items, i, k;
    double sum = 0.0, sum2 = 0.0, total_load = 0.0, top = 0.0;
    uint32_t cls_count[AP_CLASSES];
    memset(&h, 0, sizeof h);
    memset(cls_count, 0, sizeof cls_count);
    h.broken_const = AP_BROKEN_CONST;
    h.broken_noise = ap_rng_unit(noise);
    if (n == 0) return h;
    for (i = 0; i < n; i++) {
        double d = (double)s->items[i].demand0;
        sum += d; sum2 += d * d; total_load += item_load(&s->items[i]);
        cls_count[s->items[i].cls]++; idx[i] = i;
    }
    {
        double mean = sum / (double)n, var = sum2 / (double)n - mean * mean;
        if (var < 0.0) var = 0.0;
        h.cv_demand = mean > 0.0 ? sqrt(var) / mean : 0.0;
    }
    sort_desc(s, idx, n);
    k = (uint32_t)ceil(AP_TOP_SHARE_FRACTION * (double)n);
    if (k < 1u) k = 1u;
    if (k > n) k = n;
    for (i = 0; i < k; i++) top += item_load(&s->items[idx[i]]);
    h.top_share = total_load > 0.0 ? top / total_load : 0.0;
    h.max_item_share = total_load > 0.0 ? item_load(&s->items[idx[0]]) / total_load : 0.0;
    {
        double H = 0.0;
        for (i = 0; i < AP_CLASSES; i++) {
            double p = (double)cls_count[i] / (double)n;
            if (p > 0.0) H -= p * log2(p);
        }
        h.class_entropy = H / log2((double)AP_CLASSES);
    }
    {
        double a = 0.0, a2 = 0.0, m, v;
        for (i = 0; i < s->n_windows; i++) { double x = (double)s->windows[i].arrivals; a += x; a2 += x * x; }
        if (s->n_windows) {
            m = a / (double)s->n_windows; v = a2 / (double)s->n_windows - m * m;
            if (v < 0.0) v = 0.0;
            h.arrival_cv = m > 0.0 ? sqrt(v) / m : 0.0;
        }
    }
    return h;
}

int ap_campaign(ap_sim *scratch, uint32_t *scratch_idx, ap_row *rows, size_t max_rows, size_t *n_out)
{
    size_t n = 0;
    uint32_t wl, si;
    *n_out = 0;
    for (wl = 0; wl < (uint32_t)AP_WL_COUNT; wl++) {
        for (si = 0; si < AP_SEEDS; si++) {
            ap_row *r;
            ap_rng noise;
            if (n >= max_rows) return -1;
            r = &rows[n];
            memset(r, 0, sizeof *r);
            r->wl = (uint8_t)wl; r->seed_index = si; r->seed = ap_seed((ap_workload)wl, si);
            ap_sim_init_indexed(scratch, (ap_workload)wl, r->seed, si);
            if (ap_sim_run(scratch) != 0) return -2;
            r->arrived = scratch->arrived; r->completed = scratch->completed; r->rejected = scratch->rejected;
            r->real = ap_error_real(scratch);
            r->known_arrivals = ap_error_known_arrivals(scratch);
            r->shuffled = ap_error_shuffled(scratch, (uint32_t)(r->seed >> 7));
            noise.s = r->seed ^ 0xA5A5A5A5A5A5A5A5ull;
            if (!noise.s) noise.s = 1u;
            r->h = ap_measure(scratch, scratch_idx, &noise);
            n++;
        }
    }
    *n_out = n;
    return 0;
}

const char *ap_row_header(void)
{
    return "workload,seed_index,seed,arrived,completed,rejected,windows,"
           "err_completions_nrmse,err_queue_rmse,err_blocked_rmse,err_composite,"
           "known_completions_nrmse,known_queue_rmse,known_blocked_rmse,known_composite,"
           "shuf_completions_nrmse,shuf_queue_rmse,shuf_blocked_rmse,shuf_composite,"
           "cv_demand,top5_share,max_item_share,class_entropy,arrival_cv,broken_const,broken_noise";
}

int ap_row_format(const ap_row *r, char *buf, size_t n)
{
    int w = snprintf(buf, n, "%s,%u,%016llx,%llu,%llu,%llu,%u,"
                     "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
                     "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f",
                     ap_workload_name((ap_workload)r->wl), r->seed_index, (unsigned long long)r->seed,
                     (unsigned long long)r->arrived, (unsigned long long)r->completed, (unsigned long long)r->rejected,
                     r->real.windows,
                     r->real.completions_nrmse, r->real.queue_rmse, r->real.blocked_rmse, r->real.composite,
                     r->known_arrivals.completions_nrmse, r->known_arrivals.queue_rmse, r->known_arrivals.blocked_rmse, r->known_arrivals.composite,
                     r->shuffled.completions_nrmse, r->shuffled.queue_rmse, r->shuffled.blocked_rmse, r->shuffled.composite,
                     r->h.cv_demand, r->h.top_share, r->h.max_item_share, r->h.class_entropy, r->h.arrival_cv,
                     r->h.broken_const, r->h.broken_noise);
    return (w < 0 || (size_t)w >= n) ? -1 : w;
}

void ap_rows_digest(const ap_row *rows, size_t n, uint8_t out[32])
{
    sha256_ctx c;
    char line[512];
    size_t i;
    const char *hdr = ap_row_header();
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)hdr, strlen(hdr));
    sha256_update(&c, (const uint8_t *)"\n", 1);
    for (i = 0; i < n; i++) {
        int w = ap_row_format(&rows[i], line, sizeof line);
        if (w < 0) continue;
        sha256_update(&c, (const uint8_t *)line, (size_t)w);
        sha256_update(&c, (const uint8_t *)"\n", 1);
    }
    sha256_final(&c, out);
}

int ap_params_text(char *buf, size_t n)
{
    static const double grid_cv[AP_GRID_CV_COUNT] = AP_GRID_CV;
    static const double grid_top[AP_GRID_TOP_COUNT] = AP_GRID_TOP;
    int w = snprintf(buf, n,
        "experiment=%s\nwindow=%u\nwindows=%u\nwarmup_windows=%u\nslots=%u\nmemory=%u\nmax_queue=%u\nclasses=%u\nmax_items=%u\n"
        "hom_demand=%u..%u\nhom_mem=%u..%u\nhom_slots=%u\n"
        "het_ln_median=%.16g\nhet_sigma=%.16g\nhet_demand_cap=%u\nhet_slots_p=%.6f,%.6f\nhet_mem_base=%u\nhet_mem_span=%.6f\nhet_class_p=%.6f,%.6f,%.6f\n"
        "rate_hom=%.6f\nrate_het=%.6f\nstep_rate=%.6f,%.6f\nsquare_rate=%.6f,%.6f\nsquare_half_period=%u\n"
        "burst_p=%.6f\nburst_size=%u..%u\nburst_background=%.6f\ncap_drop_slots=%u\nswitch_step=%u\n"
        "sweep_sigma=%.6f..%.6f\nsweep_load=%.6f\nhet_mean_slots=%.6f\n"
        "prior_demand=%.6f\nprior_slots=%.6f\nprior_mem=%.6f\n"
        "seeds=%u\nseed_base=%016llx\ntop_share_fraction=%.6f\n"
        "grid_cv=%.6f,%.6f,%.6f\ngrid_top=%.6f,%.6f,%.6f\ngrid_arrival_cv=%.6f\nbroken_const=%.6f\n"
        "blocked_ramp_start=%.6f\npredictor=flow_balance_v2\nerror=completions_nrmse,queue_rmse,blocked_rmse,composite_mean\nprimary_measure=completions_nrmse\n",
        AP_EXPERIMENT_NAME, AP_WINDOW, AP_WINDOWS, AP_WARMUP_WINDOWS, AP_SLOTS, AP_MEMORY, AP_MAX_QUEUE, AP_CLASSES, AP_MAX_ITEMS,
        AP_HOM_DEMAND_MIN, AP_HOM_DEMAND_MAX, AP_HOM_MEM_MIN, AP_HOM_MEM_MAX, AP_HOM_SLOTS,
        AP_HET_LN_MEDIAN, AP_HET_SIGMA, AP_HET_DEMAND_CAP, AP_HET_SLOTS_P1, AP_HET_SLOTS_P2, AP_HET_MEM_BASE, AP_HET_MEM_SPAN,
        AP_HET_CLASS_P0, AP_HET_CLASS_P1, AP_HET_CLASS_P2,
        AP_RATE_HOM, AP_RATE_HET, AP_STEP_RATE_LOW, AP_STEP_RATE_HIGH, AP_SQUARE_RATE_LOW, AP_SQUARE_RATE_HIGH, AP_SQUARE_HALF_PERIOD,
        AP_BURST_P, AP_BURST_MIN, AP_BURST_MAX, AP_BURST_BACKGROUND, AP_CAP_DROP_SLOTS, AP_SWITCH_STEP,
        AP_SWEEP_SIGMA_MIN, AP_SWEEP_SIGMA_MAX, AP_SWEEP_LOAD, AP_HET_MEAN_SLOTS,
        AP_PRIOR_DEMAND, AP_PRIOR_SLOTS, AP_PRIOR_MEM,
        AP_SEEDS, (unsigned long long)AP_SEED_BASE, AP_TOP_SHARE_FRACTION,
        grid_cv[0], grid_cv[1], grid_cv[2], grid_top[0], grid_top[1], grid_top[2], AP_GRID_ARRIVAL_CV, AP_BROKEN_CONST, AP_BLOCKED_RAMP_START);
    return (w < 0 || (size_t)w >= n) ? -1 : w;
}

void ap_params_digest(uint8_t out[32])
{
    char buf[2048];
    int w = ap_params_text(buf, sizeof buf);
    sha256_hash((const uint8_t *)buf, w < 0 ? 0u : (size_t)w, out);
}

double ap_pearson(const double *x, const double *y, size_t n, double *cov_out)
{
    double mx = 0.0, my = 0.0, sxx = 0.0, syy = 0.0, sxy = 0.0;
    size_t i;
    if (cov_out) *cov_out = 0.0;
    if (n < 2) return 0.0;
    for (i = 0; i < n; i++) { mx += x[i]; my += y[i]; }
    mx /= (double)n; my /= (double)n;
    for (i = 0; i < n; i++) {
        double dx = x[i] - mx, dy = y[i] - my;
        sxx += dx * dx; syy += dy * dy; sxy += dx * dy;
    }
    if (cov_out) *cov_out = sxy / (double)(n - 1);
    if (sxx <= 0.0 || syy <= 0.0) return 0.0;
    return sxy / sqrt(sxx * syy);
}
