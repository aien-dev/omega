/* ESTIMATION v6 pre-freeze development analysis (docs/estimation/protocols/est-v6.md section 9
 * items 1 and 4; appendix est-v6-appendix-prefreeze.md). NOT the v6 tool, not a result about any
 * held-out data, and not a protocol tool: it changes no v5 threshold, parameter or receipt.
 *
 *   est6dev g1pit --raw R --marks M --dyn D --q Q --lam L --nu N --c C [--boot B] [--seed S] [--blocks]
 *       One-step G1 (est_v4.c, the v5 code, unchanged) with the GIVEN parameters on one run folder.
 *       Prints the v5 statistics it must reproduce (n, PIT bin shares, coverage 50/80/95, mean z,
 *       lag-1 z), the lower and upper 2.5 % miss rates separately, and a block bootstrap over load
 *       segments (a block is one marked load segment, or one idle stretch between marked segments)
 *       giving standard errors, percentile intervals and the design effect against independent
 *       steps for every bin share and miss rate.
 *   est6dev sched <seed> <seconds>
 *       The est_load schedule for a seed (same splitmix64 draw order as est_load.c) with per-level
 *       totals and the section 3 balance check (each of idle, L6, L12, L18 within 400..900 s).
 *
 * Refuses, before opening anything, any path that names a held-out run (v3, v4, v5 or v6 held-out
 * tag) or the v5 D2 folder; after loading, refuses an input whose SHA-256 is the v5 D2 raw or marks SHA (committed d2.sha256). */
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "est3c_common.h"
#include "est_v4.h"

#define D2_DATE_TAG "20261005T112838Z"
#define D2_RAW_SHA "7dda21f664587c4401326c3082f28039e133ba5627c8564678f259e2d7036ff0"
#define D2_MARKS_SHA "95879ed909b8fec1207695dc1093b24be56f00563cd364c62bd30007b0dc26bb"

static uint64_t splitmix64(uint64_t *s)
{
    uint64_t z = (*s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static int forbidden_path(const char *path)
{
    static const char *tags[] = { "-heldout-", D2_DATE_TAG, "-est5-heldout", "-est4-heldout", "-est3c-heldout", "-est6-heldout", NULL };
    char rp[PATH_MAX];
    for (int i = 0; tags[i]; i++) {
        if (strstr(path, tags[i])) return 1;
        if (realpath(path, rp) && strstr(rp, tags[i])) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ schedule (item 4) */
static int cmd_sched(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: est6dev sched <seed> <seconds>\n"); return 2; }
    uint64_t seed = strtoull(argv[0], NULL, 0);
    long total = strtol(argv[1], NULL, 10);
    if (total <= 0) { fprintf(stderr, "seconds must be > 0\n"); return 2; }
    static const int levels[4] = { 0, 6, 12, 18 };
    long secs_by_level[4] = { 0, 0, 0, 0 };
    long acc = 0; int n = 0;
    uint64_t s = seed;
    printf("seed %llu seconds %ld\n", (unsigned long long)seed, total);
    while (acc < total && n < 4096) {
        int li = (int)(splitmix64(&s) % 4u);
        long d = 20 + (long)(splitmix64(&s) % 101u);
        if (acc + d > total) d = total - acc;
        printf("schedule %d %d %ld\n", n, levels[li], d);
        secs_by_level[li] += d; acc += d; n++;
    }
    int ok = 1;
    for (int i = 0; i < 4; i++) if (secs_by_level[i] < 400 || secs_by_level[i] > 900) ok = 0;
    printf("segments %d idle %ld L6 %ld L12 %ld L18 %ld\n", n, secs_by_level[0], secs_by_level[1], secs_by_level[2], secs_by_level[3]);
    printf("balance %s\n", ok ? "PASS" : "FAIL");
    return 0;
}

/* first seed at or after <seed> (step +1) whose schedule passes the section 3 balance rule */
static int balance_ok(uint64_t seed, long total, long lv[4], int *nseg)
{
    static const int levels[4] = { 0, 6, 12, 18 };
    uint64_t s = seed; long acc = 0; int n = 0, ok = 1;
    for (int i = 0; i < 4; i++) lv[i] = 0;
    while (acc < total && n < 4096) {
        int li = (int)(splitmix64(&s) % 4u);
        long d = 20 + (long)(splitmix64(&s) % 101u);
        if (acc + d > total) d = total - acc;
        lv[li] += d; acc += d; n++;
    }
    (void)levels;
    for (int i = 0; i < 4; i++) if (lv[i] < 400 || lv[i] > 900) ok = 0;
    *nseg = n;
    return ok;
}

static int cmd_pick(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: est6dev pick <seed> <seconds>\n"); return 2; }
    uint64_t seed = strtoull(argv[0], NULL, 0);
    long total = strtol(argv[1], NULL, 10), lv[4]; int ns;
    for (int k = 0; k < 100000; k++) {
        int ok = balance_ok(seed + (uint64_t)k, total, lv, &ns);
        printf("seed 0x%llx segments %d idle %ld L6 %ld L12 %ld L18 %ld balance %s\n", (unsigned long long)(seed + (uint64_t)k), ns, lv[0], lv[1], lv[2], lv[3], ok ? "PASS" : "FAIL");
        if (ok) { printf("picked 0x%llx after %d failures\n", (unsigned long long)(seed + (uint64_t)k), k); return 0; }
    }
    printf("none found\n");
    return 1;
}

/* ------------------------------------------------------------ one-step G1 PIT trace */
typedef struct { int64_t t; double flo, fhi, z; } step_t;

static est_obs_class classify(const c3_tick *t, est_pobs *ob)
{
    est_assumption a; c3_assumption_base(&a, EST_FAM_STUDENT_T);
    a.nparam = 2; a.param[0] = 5; a.param[1] = 100;
    if (est_pobs_classify(&a, t->value, t->present, &t->ev, ob) != EST_OK) return EST_OBS_CLASSES_;
    return ob->cls;
}

/* mirrors est5.c e4_score, one-step part only (same classification, burn-in, unscorable rule) */
static int run_g1(const c3_ticks *tk, const est4_params *p, step_t **out, size_t *nout, size_t *unscorable)
{
    est4_state s;
    est_dpred *dp = malloc(sizeof *dp);
    step_t *st = malloc((tk->n ? tk->n : 1) * sizeof *st);
    size_t n = 0; *unscorable = 0;
    if (!dp || !st) return 1;
    if (est4_init(p, &s) != EST_OK) return 1;
    long first = -1;
    for (size_t t = 0; t < tk->n; t++) {
        const c3_tick *tc = &tk->t[t];
        est_pobs ob; est_pinnov iv;
        est_obs_class cls = classify(tc, &ob);
        if (cls == EST_OBS_CLASSES_) return 1;
        int ok = cls == EST_OBS_OK;
        if (ok && first < 0) first = (long)t;
        int scored = first >= 0 && (long)t - first >= (long)C3_BURN_IN;
        if (ok && scored) {
            if ((uint64_t)s.gap + 1u > EST_PRED_MAX_H) (*unscorable)++;
            else {
                est4_pred pr;
                if (est4_predict(&s, 1, &pr) != EST_OK || est4_dpred(&s, &pr, dp) != EST_OK) return 1;
                if (est_dpred_score(dp, &ob, &iv) != EST_OK) return 1;
                if (iv.valid) { st[n].t = tc->wall_ns; st[n].flo = iv.F_lo; st[n].fhi = iv.F_hi; st[n].z = iv.z; n++; }
            }
        }
        if (est4_update(&s, ok, ok ? ob.value : NAN) != EST_OK) return 1;
    }
    free(dp);
    *out = st; *nout = n;
    return 0;
}

#define NSTAT 13   /* bins 0..9, lower miss, upper miss, cov95 */
static const char *STAT_NAME[NSTAT] = { "pit_bin0", "pit_bin1", "pit_bin2", "pit_bin3", "pit_bin4", "pit_bin5", "pit_bin6",
                                        "pit_bin7", "pit_bin8", "pit_bin9", "lower_miss_2.5", "upper_miss_2.5", "coverage95" };

static double overlap(double a, double b, double c, double d)
{
    double lo = a > c ? a : c, hi = b < d ? b : d;
    return hi > lo ? hi - lo : 0.0;
}

/* per-step contribution to each statistic: expected value under u ~ U(F_lo, F_hi) */
static void contrib(const step_t *s, double v[NSTAT])
{
    double span = s->fhi - s->flo;
    for (int j = 0; j < 10; j++) {
        if (span > 0.0) v[j] = overlap(s->flo, s->fhi, j / 10.0, (j + 1) / 10.0) / span;
        else { int k = (int)floor(s->flo * 10.0); if (k < 0) k = 0; if (k > 9) k = 9; v[j] = (k == j) ? 1.0 : 0.0; }
    }
    if (span > 0.0) {
        double l = (0.025 - s->flo) / span, u = (s->fhi - 0.975) / span;
        v[10] = l < 0 ? 0 : l > 1 ? 1 : l;
        v[11] = u < 0 ? 0 : u > 1 ? 1 : u;
    } else { v[10] = s->flo < 0.025; v[11] = s->flo > 0.975; }
    v[12] = 1.0 - v[10] - v[11];
}

static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

static double quant(const double *sorted, int n, double q)
{
    double pos = q * (double)(n - 1);
    int i = (int)floor(pos);
    if (i >= n - 1) return sorted[n - 1];
    return sorted[i] + (pos - i) * (sorted[i + 1] - sorted[i]);
}

/* block id of a time: odd 2i+1 inside marked pair i, even 2m between (m = pairs ended before t) */
static size_t block_of(const c3_marks *mk, int64_t t)
{
    size_t ended = 0;
    for (size_t i = 0; i < mk->n; i++) {
        if (t >= mk->b[i] && t <= mk->e[i]) return 2 * i + 1;
        if (mk->e[i] < t) ended++;
    }
    return 2 * ended;
}

static int cmd_g1pit(int argc, char **argv)
{
    const char *raw = NULL, *mkp = NULL;
    double dyn = NAN, q = NAN, lam = NAN, nu = NAN, c = NAN;
    int boot = 2000, show_blocks = 0;
    uint64_t seed = 0xE6B007ull;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--raw") && i + 1 < argc) raw = argv[++i];
        else if (!strcmp(argv[i], "--marks") && i + 1 < argc) mkp = argv[++i];
        else if (!strcmp(argv[i], "--dyn") && i + 1 < argc) dyn = atof(argv[++i]);
        else if (!strcmp(argv[i], "--q") && i + 1 < argc) q = atof(argv[++i]);
        else if (!strcmp(argv[i], "--lam") && i + 1 < argc) lam = atof(argv[++i]);
        else if (!strcmp(argv[i], "--nu") && i + 1 < argc) nu = atof(argv[++i]);
        else if (!strcmp(argv[i], "--c") && i + 1 < argc) c = atof(argv[++i]);
        else if (!strcmp(argv[i], "--boot") && i + 1 < argc) boot = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--blocks")) show_blocks = 1;
        else { fprintf(stderr, "usage: est6dev g1pit --raw R --marks M --dyn D --q Q --lam L --nu N --c C [--boot B] [--seed S] [--blocks]\n"); return 2; }
    }
    if (!raw || !mkp || isnan(dyn) || isnan(q) || isnan(lam) || isnan(nu) || isnan(c) || boot < 10) { fprintf(stderr, "est6dev g1pit: missing argument\n"); return 2; }
    if (forbidden_path(raw) || forbidden_path(mkp)) { fprintf(stderr, "refuse: a held-out path (v5 D2 and every held-out tag are forbidden in v6 development)\n"); return 2; }
    char err[512];
    est_file f;
    if (est_file_load(raw, &f, err, sizeof err)) { fprintf(stderr, "refuse: %s\n", err); return 1; }
    if (!strcmp(f.sha_hex, D2_RAW_SHA)) { fprintf(stderr, "refuse: input is the v5 D2 raw data (SHA-256)\n"); est_file_free(&f); return 2; }
    static c3_marks mk;
    if (c3_marks_load(mkp, 1, &mk, err, sizeof err)) { fprintf(stderr, "refuse: marks: %s\n", err); est_file_free(&f); return 1; }
    if (!strcmp(mk.sha, D2_MARKS_SHA)) { fprintf(stderr, "refuse: marks are the v5 D2 marks (SHA-256)\n"); est_file_free(&f); return 2; }
    c3_ticks tk;
    if (c3_ticks_build(&f, &tk, err, sizeof err)) { fprintf(stderr, "ticks: %s\n", err); est_file_free(&f); return 1; }
    printf("input raw_sha256 %s marks_sha256 %s lines %zu marks_pairs %zu\n", f.sha_hex, mk.sha, tk.lines, mk.n);

    est4_params p = { EST4_G1_LAG, dyn, q, lam, nu, c, 0.95, 0.8, 0.2, C3_QUANTUM };
    step_t *st = NULL; size_t n = 0, unsc = 0;
    if (run_g1(&tk, &p, &st, &n, &unsc) || n == 0) { fprintf(stderr, "g1 run failed\n"); return 1; }

    /* per-step contributions, per-block sums */
    size_t nb_max = 2 * mk.n + 2;
    double (*bs)[NSTAT] = calloc(nb_max, sizeof *bs);
    double *bn = calloc(nb_max, sizeof *bn);
    double tot[NSTAT] = { 0 };
    double zsum = 0;
    if (!bs || !bn) return 1;
    for (size_t i = 0; i < n; i++) {
        double v[NSTAT];
        contrib(&st[i], v);
        size_t b = block_of(&mk, st[i].t);
        for (int k = 0; k < NSTAT; k++) { bs[b][k] += v[k]; tot[k] += v[k]; }
        bn[b] += 1.0;
        zsum += st[i].z;
    }
    double zm = zsum / (double)n;
    printf("params dyn %.17g q %.17g lam %.17g nu %.17g c %.17g\n", dyn, q, lam, nu, c);
    printf("n_scored %zu unscorable %zu\n", n, unsc);
    printf("mean_z_midpit %.6f\n", zm);

    /* blocks that have steps */
    size_t nb = 0, nload = 0, *idx = malloc(nb_max * sizeof *idx);
    for (size_t b = 0; b < nb_max; b++) if (bn[b] > 0) { idx[nb++] = b; nload += (b & 1u); }
    printf("blocks %zu (loaded %zu, idle %zu) mean_steps_per_block %.1f\n", nb, nload, nb - nload, (double)n / (double)nb);
    if (show_blocks)
        for (size_t i = 0; i < nb; i++) {
            size_t b = idx[i];
            printf("block %zu %s level %d steps %.0f bin0_mass %.3f lower_miss_mass %.3f upper_miss_mass %.3f\n", b, (b & 1u) ? "load" : "idle",
                   (b & 1u) ? mk.level[b / 2] : 0, bn[b], bs[b][0], bs[b][10], bs[b][11]);
        }

    {   /* descriptive: shares by declared load level (idle = 0) */
        double ls[4][NSTAT] = { { 0 } }, ln[4] = { 0 };
        for (size_t i = 0; i < nb; i++) {
            size_t b = idx[i];
            int lv = (b & 1u) ? mk.level[b / 2] : 0, li = lv == 6 ? 1 : lv == 12 ? 2 : lv == 18 ? 3 : 0;
            ln[li] += bn[b];
            for (int k = 0; k < NSTAT; k++) ls[li][k] += bs[b][k];
        }
        static const int lvn[4] = { 0, 6, 12, 18 };
        for (int li = 0; li < 4; li++)
            if (ln[li] > 0) printf("by_level L%d steps %.0f pit_bin0 %.4f pit_bin9 %.4f lower_miss %.4f upper_miss %.4f\n", lvn[li], ln[li], ls[li][0] / ln[li], ls[li][9] / ln[li], ls[li][10] / ln[li], ls[li][11] / ln[li]);
    }
    /* bootstrap: resample whole blocks with replacement; ratio estimator */
    double *draw = malloc((size_t)boot * NSTAT * sizeof(double));
    if (!draw) return 1;
    uint64_t rng = seed;
    for (int r = 0; r < boot; r++) {
        double sum[NSTAT] = { 0 }, cnt = 0;
        for (size_t i = 0; i < nb; i++) {
            size_t b = idx[splitmix64(&rng) % nb];
            for (int k = 0; k < NSTAT; k++) sum[k] += bs[b][k];
            cnt += bn[b];
        }
        for (int k = 0; k < NSTAT; k++) draw[(size_t)r * NSTAT + k] = sum[k] / cnt;
    }
    printf("bootstrap draws %d seed 0x%llx unit whole_blocks\n", boot, (unsigned long long)seed);
    printf("# stat value iid_se boot_se design_effect ci95_lo ci95_hi ci995_lo ci995_hi\n");
    double *col = malloc((size_t)boot * sizeof *col);
    for (int k = 0; k < NSTAT; k++) {
        double val = tot[k] / (double)n, mean = 0, var = 0;
        for (int r = 0; r < boot; r++) { col[r] = draw[(size_t)r * NSTAT + k]; mean += col[r]; }
        mean /= boot;
        for (int r = 0; r < boot; r++) var += (col[r] - mean) * (col[r] - mean);
        var /= (boot - 1);
        double iid = sqrt(val * (1.0 - val) / (double)n);
        qsort(col, (size_t)boot, sizeof *col, cmpd);
        double bse = sqrt(var);
        printf("stat %s %.6f %.6f %.6f %.3f %.6f %.6f %.6f %.6f\n", STAT_NAME[k], val, iid, bse, iid > 0 ? (bse * bse) / (iid * iid) : NAN,
               quant(col, boot, 0.025), quant(col, boot, 0.975), quant(col, boot, 0.0025), quant(col, boot, 0.9975));
    }
    free(st); free(bs); free(bn); free(idx); free(draw); free(col);
    c3_ticks_free(&tk); est_file_free(&f);
    return 0;
}

/* ------------------------------------------------------------ G1S exploratory (item 2, reduced) */
/* Reduced item 2: the filter parameters (dyn q lam nu) stay at the v5 D1 values; only the skew kappa
 * and the scale c are searched (kappa x c grid), by 5-fold block cross-validation over load segments.
 * The two-piece t is built from the symmetric est4_fast_logp kernel (est_v4.c unchanged): scale c b on
 * the upper side and c kappa b on the lower side, side masses 1/(1+kappa) and kappa/(1+kappa). */
typedef struct { int64_t t; double y, m, b, anchor; } trace_t;

static int run_g1_trace(const c3_ticks *tk, const est4_params *p, trace_t **out, size_t *nout)
{
    est4_state s;
    trace_t *tr = malloc((tk->n ? tk->n : 1) * sizeof *tr);
    size_t n = 0;
    if (!tr || est4_init(p, &s) != EST_OK) return 1;
    long first = -1;
    for (size_t t = 0; t < tk->n; t++) {
        est_pobs ob;
        est_obs_class cls = classify(&tk->t[t], &ob);
        if (cls == EST_OBS_CLASSES_) return 1;
        int ok = cls == EST_OBS_OK;
        if (ok && first < 0) first = (long)t;
        int scored = first >= 0 && (long)t - first >= (long)C3_BURN_IN;
        if (ok && scored && (uint64_t)s.gap + 1u <= EST_PRED_MAX_H) {
            double m, v;
            if (est4_moments(&s, 1, &m, &v) != EST_OK) return 1;
            tr[n].t = tk->t[t].wall_ns; tr[n].y = ob.value; tr[n].m = m; tr[n].b = sqrt(s.g * v); tr[n].anchor = s.anchor; n++;
        }
        if (est4_update(&s, ok, ok ? ob.value : NAN) != EST_OK) return 1;
    }
    *out = tr; *nout = n;
    return 0;
}

/* un-floored cdf at the bin edges from the symmetric kernel */
static void sym_edges(const trace_t *r, double scale, double nu, double *lo, double *hi)
{
    est4_pred pr = { 1u, r->m, scale, nu };
    double Fl, Fh;
    est4_fast_logp(&pr, C3_QUANTUM, r->anchor, r->y, &Fl, &Fh);
    double kd = nearbyint((r->y - r->anchor) / C3_QUANTUM);
    if (kd < -(double)EST_PRED_K) kd = -(double)EST_PRED_K;
    if (kd > (double)EST_PRED_K) kd = (double)EST_PRED_K;
    double f = EST_PRED_FLOOR, u = EST_PRED_FLOOR / (double)EST_PRED_N, idx = kd + (double)EST_PRED_K;
    *lo = (Fl - u * idx) / (1.0 - f);
    *hi = (Fh - u * (idx + 1.0)) / (1.0 - f);
}

static double two_piece(const trace_t *r, double c, double kappa, double nu, double *Flo, double *Fhi)
{
    double a = 1.0 / (1.0 + kappa), b = kappa / (1.0 + kappa);
    double lo_u, hi_u, lo_l, hi_l;
    sym_edges(r, c * r->b, nu, &lo_u, &hi_u);
    sym_edges(r, c * kappa * r->b, nu, &lo_l, &hi_l);
    double kd = nearbyint((r->y - r->anchor) / C3_QUANTUM);
    if (kd < -(double)EST_PRED_K) kd = -(double)EST_PRED_K;
    if (kd > (double)EST_PRED_K) kd = (double)EST_PRED_K;
    double mu = r->m - r->anchor, xa = (kd - 0.5) * C3_QUANTUM, xb = (kd + 0.5) * C3_QUANTUM;
    double Fa = xa < mu ? 2.0 * b * lo_l : b + 2.0 * a * (lo_u - 0.5);
    double Fb = xb < mu ? 2.0 * b * hi_l : b + 2.0 * a * (hi_u - 0.5);
    if (kd == -(double)EST_PRED_K) Fa = 0.0;
    if (kd == (double)EST_PRED_K) Fb = 1.0;
    double p = Fb - Fa; if (p < 0) p = 0;
    double f = EST_PRED_FLOOR, u = EST_PRED_FLOOR / (double)EST_PRED_N, idx = kd + (double)EST_PRED_K;
    *Flo = (1.0 - f) * Fa + u * idx; *Fhi = (1.0 - f) * Fb + u * (idx + 1.0);
    p = (1.0 - f) * p + u;
    return log(p > 1e-300 ? p : 1e-300);
}

#define NK 5
#define NCG 21
static const double KAPPA[NK] = { 0.6, 0.7, 0.8, 0.9, 1.0 };

static int cmd_g1s(int argc, char **argv)
{
    const char *raw = NULL, *mkp = NULL;
    double dyn = NAN, q = NAN, lam = NAN, nu = NAN;
    int nfold = 5;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--raw") && i + 1 < argc) raw = argv[++i];
        else if (!strcmp(argv[i], "--marks") && i + 1 < argc) mkp = argv[++i];
        else if (!strcmp(argv[i], "--dyn") && i + 1 < argc) dyn = atof(argv[++i]);
        else if (!strcmp(argv[i], "--q") && i + 1 < argc) q = atof(argv[++i]);
        else if (!strcmp(argv[i], "--lam") && i + 1 < argc) lam = atof(argv[++i]);
        else if (!strcmp(argv[i], "--nu") && i + 1 < argc) nu = atof(argv[++i]);
        else if (!strcmp(argv[i], "--folds") && i + 1 < argc) nfold = atoi(argv[++i]);
        else { fprintf(stderr, "usage: est6dev g1s --raw R --marks M --dyn D --q Q --lam L --nu N [--folds K]\n"); return 2; }
    }
    if (!raw || !mkp || isnan(dyn) || isnan(q) || isnan(lam) || isnan(nu) || nfold < 2 || nfold > 20) { fprintf(stderr, "est6dev g1s: missing argument\n"); return 2; }
    if (forbidden_path(raw) || forbidden_path(mkp)) { fprintf(stderr, "refuse: a held-out path (v5 D2 and every held-out tag are forbidden in v6 development)\n"); return 2; }
    char err[512];
    est_file f;
    if (est_file_load(raw, &f, err, sizeof err)) { fprintf(stderr, "refuse: %s\n", err); return 1; }
    if (!strcmp(f.sha_hex, D2_RAW_SHA)) { fprintf(stderr, "refuse: input is the v5 D2 raw data (SHA-256)\n"); est_file_free(&f); return 2; }
    static c3_marks mk;
    if (c3_marks_load(mkp, 1, &mk, err, sizeof err)) { fprintf(stderr, "refuse: marks: %s\n", err); est_file_free(&f); return 1; }
    if (!strcmp(mk.sha, D2_MARKS_SHA)) { fprintf(stderr, "refuse: marks are the v5 D2 marks (SHA-256)\n"); est_file_free(&f); return 2; }
    c3_ticks tk;
    if (c3_ticks_build(&f, &tk, err, sizeof err)) { fprintf(stderr, "ticks: %s\n", err); est_file_free(&f); return 1; }
    printf("input raw_sha256 %s marks_sha256 %s\n", f.sha_hex, mk.sha);
    est4_params p = { EST4_G1_LAG, dyn, q, lam, nu, 1.0, 0.95, 0.8, 0.2, C3_QUANTUM };
    trace_t *tr = NULL; size_t n = 0;
    if (run_g1_trace(&tk, &p, &tr, &n) || n == 0) { fprintf(stderr, "g1 trace failed\n"); return 1; }
    /* fold of a step = rank of its block among blocks that have steps, modulo nfold */
    size_t nb_max = 2 * mk.n + 2;
    int *rank = malloc(nb_max * sizeof *rank), *seen = calloc(nb_max, sizeof *seen);
    size_t *blk = malloc(n * sizeof *blk);
    for (size_t i = 0; i < n; i++) { blk[i] = block_of(&mk, tr[i].t); seen[blk[i]] = 1; }
    int nr = 0;
    for (size_t b = 0; b < nb_max; b++) rank[b] = seen[b] ? nr++ : -1;
    int *fold = malloc(n * sizeof *fold);
    for (size_t i = 0; i < n; i++) fold[i] = rank[blk[i]] % nfold;
    /* per grid point, per step: logp, F_lo, F_hi */
    size_t G = (size_t)NK * NCG;
    double *lp = malloc(G * n * sizeof *lp), *Flo = malloc(G * n * sizeof *Flo), *Fhi = malloc(G * n * sizeof *Fhi);
    if (!lp || !Flo || !Fhi) return 1;
    for (int ki = 0; ki < NK; ki++)
        for (int ci = 0; ci < NCG; ci++) {
            size_t g = (size_t)ki * NCG + (size_t)ci;
            double c = (double)(20 + 5 * ci) / 100.0;
            for (size_t i = 0; i < n; i++) lp[g * n + i] = two_piece(&tr[i], c, KAPPA[ki], nu, &Flo[g * n + i], &Fhi[g * n + i]);
        }
    /* bit-for-bit style check: kappa = 1.0 against est4_fast_logp (maximum absolute difference) */
    double maxd = 0;
    for (int ci = 0; ci < NCG; ci++)
        for (size_t i = 0; i < n; i++) {
            est4_pred pr = { 1u, tr[i].m, ((double)(20 + 5 * ci) / 100.0) * tr[i].b, nu };
            double d = fabs(est4_fast_logp(&pr, C3_QUANTUM, tr[i].anchor, tr[i].y, NULL, NULL) - lp[(size_t)(NK - 1) * NCG * n + (size_t)ci * n + i]);
            if (d > maxd) maxd = d;
        }
    printf("check kappa1_vs_est4_fast_logp max_abs_diff %.3g\n", maxd);
    /* in-sample best over the whole run, per kappa and overall */
    for (int ki = 0; ki < NK; ki++) {
        double best = -INFINITY; int bc = 0;
        for (int ci = 0; ci < NCG; ci++) {
            double s = 0; size_t g = (size_t)ki * NCG + (size_t)ci;
            for (size_t i = 0; i < n; i++) s += lp[g * n + i];
            if (s / (double)n > best) { best = s / (double)n; bc = ci; }
        }
        {
            size_t g = (size_t)ki * NCG + (size_t)bc; double b0 = 0, b9 = 0, l = 0, u = 0;
            for (size_t i = 0; i < n; i++) { step_t st = { 0, Flo[g * n + i], Fhi[g * n + i], 0 }; double v[NSTAT]; contrib(&st, v); b0 += v[0]; b9 += v[9]; l += v[10]; u += v[11]; }
            printf("insample kappa %.1f best_c %.2f mean_logscore %.6f pit_bin0 %.4f pit_bin9 %.4f lower_miss %.4f upper_miss %.4f\n", KAPPA[ki], (double)(20 + 5 * bc) / 100.0, best, b0 / (double)n, b9 / (double)n, l / (double)n, u / (double)n);
        }
    }
    /* out-of-fold: for each fold choose (kappa, c) on the other folds, score the held-out fold. restrict = 0: kappa free; 1: kappa fixed at 1.0 */
    for (int restrict_k = 0; restrict_k < 2; restrict_k++) {
        double bins[10] = { 0 }, lm = 0, um = 0, ls = 0;
        printf("oof %s\n", restrict_k ? "kappa=1.0 only (c refit by fold, control)" : "kappa free in {0.6..1.0}, c refit by fold");
        for (int fo = 0; fo < nfold; fo++) {
            double best = -INFINITY; size_t bg = 0;
            for (int ki = restrict_k ? NK - 1 : 0; ki < NK; ki++)
                for (int ci = 0; ci < NCG; ci++) {
                    size_t g = (size_t)ki * NCG + (size_t)ci; double s = 0;
                    for (size_t i = 0; i < n; i++) if (fold[i] != fo) s += lp[g * n + i];
                    if (s > best) { best = s; bg = g; }
                }
            size_t nf = 0; double fl = 0;
            for (size_t i = 0; i < n; i++) if (fold[i] == fo) {
                step_t st = { 0, Flo[bg * n + i], Fhi[bg * n + i], 0 };
                double v[NSTAT]; contrib(&st, v);
                for (int j = 0; j < 10; j++) bins[j] += v[j];
                lm += v[10]; um += v[11]; ls += lp[bg * n + i]; fl += lp[bg * n + i]; nf++;
            }
            printf("fold %d steps %zu chosen_kappa %.1f chosen_c %.2f oof_mean_logscore %.6f\n", fo, nf, KAPPA[bg / NCG], (double)(20 + 5 * (int)(bg % NCG)) / 100.0, nf ? fl / (double)nf : 0.0);
        }
        printf("oof_pooled n %zu mean_logscore %.6f pit_bin0 %.4f pit_bin9 %.4f lower_miss %.4f upper_miss %.4f bins", n, ls / (double)n, bins[0] / (double)n, bins[9] / (double)n, lm / (double)n, um / (double)n);
        for (int j = 0; j < 10; j++) printf(" %.4f", bins[j] / (double)n);
        printf("\n");
    }
    /* in-sample overall best (kappa, c) PIT shares, for reference */
    {
        double best = -INFINITY; size_t bg = 0;
        for (size_t g = 0; g < G; g++) { double s = 0; for (size_t i = 0; i < n; i++) s += lp[g * n + i]; if (s > best) { best = s; bg = g; } }
        double bins[10] = { 0 }, lm = 0, um = 0;
        for (size_t i = 0; i < n; i++) { step_t st = { 0, Flo[bg * n + i], Fhi[bg * n + i], 0 }; double v[NSTAT]; contrib(&st, v); for (int j = 0; j < 10; j++) bins[j] += v[j]; lm += v[10]; um += v[11]; }
        printf("insample_best kappa %.1f c %.2f mean_logscore %.6f pit_bin0 %.4f pit_bin9 %.4f lower_miss %.4f upper_miss %.4f\n", KAPPA[bg / NCG], (double)(20 + 5 * (int)(bg % NCG)) / 100.0, best / (double)n, bins[0] / (double)n, bins[9] / (double)n, lm / (double)n, um / (double)n);
    }
    free(tr); free(rank); free(seen); free(blk); free(fold); free(lp); free(Flo); free(Fhi);
    c3_ticks_free(&tk); est_file_free(&f);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: est6dev g1pit|g1s|sched|pick ...\n"); return 2; }
    if (!strcmp(argv[1], "g1pit")) return cmd_g1pit(argc - 2, argv + 2);
    if (!strcmp(argv[1], "sched")) return cmd_sched(argc - 2, argv + 2);
    if (!strcmp(argv[1], "pick")) return cmd_pick(argc - 2, argv + 2);
    if (!strcmp(argv[1], "g1s")) return cmd_g1s(argc - 2, argv + 2);
    fprintf(stderr, "usage: est6dev g1pit|g1s|sched|pick ...\n");
    return 2;
}
