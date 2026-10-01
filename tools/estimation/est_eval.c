/* EST-3 evaluation (protocol v1 sections 4 and 5; --protocol-v2 for EST23_PROTOCOL_V2).
 *   est_eval --params P --raw machine-state.ndjson --marks machine-state-marks.txt
 *            --out receipt.json [--recorded] [--synthetic-test]
 * Takes the parameter file and the held-out file only; never fits. Refuses when
 * the parameter file was not fit on run A (unless --synthetic-test), and
 * refuses run B unless --recorded. Writes a JSON receipt. */
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "est_replay.h"

#define TWO_PI 6.283185307179586
#define ZC50 0.674490
#define ZC80 1.281552
#define ZC95 1.959964
#define TEN_STEP 10

typedef struct { double lo, hi; } band;
static const band B50 = { 0.46, 0.54 }, B80 = { 0.76, 0.84 }, B95 = { 0.93, 0.97 };
static const band BNIS = { 0.80, 1.25 }, BQ = { 0.90, 0.99 };

typedef struct {
    size_t n;
    double cov[3], wlo[3], whi[3];
    double mean_nis, bias, lag1, ljung_box10;
    size_t qn[4]; double qcov[4];
    size_t regime_n[2]; double regime_cov[2];     /* 0 idle, 1 in-trial */
    size_t ten_n; double ten_cov, ten_lo, ten_hi;
    int64_t tmin_ns, tmax_ns;                      /* included-sample logical time span */
    double logscore, rmse, persist_rmse, zero_innov_frac;
    int ok_cov[3], ok_nis, ok_bias, ok_lag1, ok_q[4], ok_regime[2], ok_ten, calibrated;
    int rmse_le_persist;
} model_stats;

#define MAX_PAIRS 512
typedef struct {
    int64_t b[MAX_PAIRS], e[MAX_PAIRS];
    size_t n, begins, ends, unclosed, nested_begins, stray_ends;
    char sha[65];
} marks;

static void wilson(size_t k, size_t n, double *lo, double *hi)
{
    double z = ZC95, p = (double)k / (double)n, z2 = z * z, dn = (double)n;
    double den = 1.0 + z2 / dn, c = (p + z2 / (2.0 * dn)) / den;
    double h = z * sqrt(p * (1.0 - p) / dn + z2 / (4.0 * dn * dn)) / den;
    *lo = c - h; *hi = c + h;
}

static int in_band(double v, band b) { return v >= b.lo && v <= b.hi; }

static int load_marks(const char *path, marks *mk, char *err, size_t cap)
{
    memset(mk, 0, sizeof *mk);
    est_file f;
    if (est_file_load(path, &f, err, cap)) return 1;
    memcpy(mk->sha, f.sha_hex, 65);
    int64_t open_t = 0; int is_open = 0;
    for (size_t i = 0; i < f.nlines; i++) {
        const char *ln = (const char *)f.data + f.off[i]; size_t ll = f.llen[i];
        int64_t t; size_t used;
        if (est_parse_time_prefix(ln, ll, &t, &used)) continue;
        const char *rest = ln + used; size_t rl = ll - used;
        if (rl >= 7 && memcmp(rest, " begin ", 7) == 0) {
            mk->begins++;
            if (is_open) {            /* nested begin: the earlier one ends where the new one starts */
                mk->nested_begins++;
                if (mk->n >= MAX_PAIRS) { snprintf(err, cap, "more than %d marks pairs", MAX_PAIRS); est_file_free(&f); return 1; }
                mk->b[mk->n] = open_t; mk->e[mk->n] = t; mk->n++;
            }
            open_t = t; is_open = 1;
        } else if (rl >= 5 && memcmp(rest, " end ", 5) == 0) {
            mk->ends++;
            if (!is_open) { mk->stray_ends++; continue; }
            if (mk->n >= MAX_PAIRS) { snprintf(err, cap, "more than %d marks pairs", MAX_PAIRS); est_file_free(&f); return 1; }
            mk->b[mk->n] = open_t; mk->e[mk->n] = t; mk->n++;
            is_open = 0;
        }
    }
    if (is_open) {                    /* unclosed begin runs to the end of the file */
        if (mk->n >= MAX_PAIRS) { snprintf(err, cap, "more than %d marks pairs", MAX_PAIRS); est_file_free(&f); return 1; }
        mk->b[mk->n] = open_t; mk->e[mk->n] = INT64_MAX; mk->n++;
        mk->unclosed = 1;
    }
    est_file_free(&f);
    return 0;
}


static int step_equal(const est_step *a, const est_step *b)
{
    return a->line == b->line && a->L == b->L && a->prior == b->prior && a->coast == b->coast
        && a->t_ok == b->t_ok && a->chain_ok == b->chain_ok && a->wall_ns == b->wall_ns
        && a->t_ns == b->t_ns && a->horizon == b->horizon
        && memcmp(&a->z, &b->z, sizeof(double)) == 0 && memcmp(&a->y_mean, &b->y_mean, sizeof(double)) == 0
        && memcmp(&a->S, &b->S, sizeof(double)) == 0 && memcmp(&a->nu, &b->nu, sizeof(double)) == 0
        && memcmp(&a->nis, &b->nis, sizeof(double)) == 0
        && memcmp(&a->evidence, &b->evidence, sizeof a->evidence) == 0 && memcmp(&a->obs_d, &b->obs_d, sizeof a->obs_d) == 0
        && memcmp(&a->pred_d, &b->pred_d, sizeof a->pred_d) == 0 && memcmp(&a->innov_d, &b->innov_d, sizeof a->innov_d) == 0
        && memcmp(&a->belief_d, &b->belief_d, sizeof a->belief_d) == 0
        && memcmp(a->post.x, b->post.x, sizeof a->post.x) == 0 && memcmp(a->post.P, b->post.P, sizeof a->post.P) == 0
        && a->post.generation == b->post.generation && a->post.t_ns == b->post.t_ns;
}

static int in_trial(const marks *mk, int64_t t)
{
    for (size_t i = 0; i < mk->n; i++) if (t >= mk->b[i] && t <= mk->e[i]) return 1;
    return 0;
}

/* mx == NULL: Gaussian intervals (v1). mx != NULL (protocol v2, M2): the
 * h-step change has the mixture shape; intervals are its central quantiles and
 * the log score is its log density. Thresholds are cached per horizon. */
typedef struct { const est_mix *mx; double t[EST_MIX_MAX_H + 1][3]; int have[EST_MIX_MAX_H + 1]; size_t beyond; } shape_q;
static const double PCOV[3] = { 0.50, 0.80, 0.95 };
static int shape_thr(shape_q *sq, uint32_t h, double out[3])
{
    if (h < 1 || h > EST_MIX_MAX_H) { sq->beyond++; return 1; }
    if (!sq->have[h]) {
        for (int k = 0; k < 3; k++) if (est_mix_quantile_abs(sq->mx, h, PCOV[k], &sq->t[h][k]) != EST_OK) return 1;
        sq->have[h] = 1;
    }
    for (int k = 0; k < 3; k++) out[k] = sq->t[h][k];
    return 0;
}

static void compute(const est_replay *rp, const marks *mk, model_stats *st, const est_mix *mx, size_t *beyond)
{
    memset(st, 0, sizeof *st);
    static shape_q sq;
    memset(&sq, 0, sizeof sq); sq.mx = mx;
    size_t cap = rp->nsteps;
    double *w = malloc(cap * sizeof *w), *nu = malloc(cap * sizeof *nu);
    size_t n = 0, hit[3] = { 0, 0, 0 }, zero = 0;
    double snis = 0, sls = 0, sse = 0, ssp = 0, prev_z = 0; int have_prev = 0;
    int64_t tmin = 0, tmax = 0; int first = 1;
    for (size_t i = 0; i < rp->nsteps; i++) {           /* time range of included samples */
        const est_step *s = &rp->steps[i];
        if (s->prior || s->coast || s->L < EST_BURN_IN) continue;
        if (first) { tmin = tmax = s->t_ns; first = 0; }
        if (s->t_ns < tmin) tmin = s->t_ns;
        if (s->t_ns > tmax) tmax = s->t_ns;
    }
    size_t qh[4] = { 0 };
    size_t rh[2] = { 0 };
    for (size_t i = 0; i < rp->nsteps; i++) {
        const est_step *s = &rp->steps[i];
        if (s->prior) { prev_z = s->z; have_prev = 1; continue; }
        if (s->coast) continue;
        if (s->L >= EST_BURN_IN) {
            double sd = sqrt(s->S), a = fabs(s->nu);
            double thr[3] = { ZC50 * sd, ZC80 * sd, ZC95 * sd };
            if (mx && shape_thr(&sq, s->horizon, thr)) thr[0] = thr[1] = thr[2] = -1.0;   /* counted as misses, reported */
            nu[n] = s->nu; w[n] = s->nu / sd; n++;
            if (a <= thr[0]) hit[0]++;
            if (a <= thr[1]) hit[1]++;
            int h95 = a <= thr[2];
            if (h95) hit[2]++;
            snis += s->nis;
            if (mx) { double lp = -INFINITY; if (s->horizon >= 1 && s->horizon <= EST_MIX_MAX_H) est_mix_logpdf(mx, s->horizon, s->nu, &lp); sls += lp; }
            else sls += -0.5 * (log(TWO_PI * s->S) + s->nis);
            sse += s->nu * s->nu;
            if (s->nu == 0.0) zero++;
            if (have_prev) { double e = s->z - prev_z; ssp += e * e; }
            int q = (int)((4 * (s->t_ns - tmin)) / (tmax - tmin + 1));
            if (q < 0) q = 0;
            if (q > 3) q = 3;
            st->qn[q]++; if (h95) qh[q]++;
            int rg = s->t_ok && in_trial(mk, s->wall_ns);
            st->regime_n[rg]++; if (h95) rh[rg]++;
        }
        prev_z = s->z; have_prev = 1;
    }
    st->n = n; st->tmin_ns = tmin; st->tmax_ns = tmax;
    if (n >= 3) {
        double dn = (double)n;
        for (int k = 0; k < 3; k++) { st->cov[k] = hit[k] / dn; wilson(hit[k], n, &st->wlo[k], &st->whi[k]); }
        st->mean_nis = snis / dn;
        double mn = 0; for (size_t i = 0; i < n; i++) mn += nu[i]; mn /= dn;
        double v = 0; for (size_t i = 0; i < n; i++) v += (nu[i] - mn) * (nu[i] - mn);
        double sdv = sqrt(v / (dn - 1.0));
        st->bias = sdv > 0 ? mn / sdv : INFINITY;
        double wm = 0; for (size_t i = 0; i < n; i++) wm += w[i]; wm /= dn;
        double den = 0; for (size_t i = 0; i < n; i++) den += (w[i] - wm) * (w[i] - wm);
        double lb = 0;
        for (int k = 1; k <= 10 && (size_t)k < n; k++) {
            double num = 0; for (size_t i = 0; i + k < n; i++) num += (w[i] - wm) * (w[i + k] - wm);
            double rk = den > 0 ? num / den : 0;
            if (k == 1) st->lag1 = rk;
            lb += rk * rk / (dn - k);
        }
        st->ljung_box10 = dn * (dn + 2.0) * lb;
        st->logscore = sls / dn; st->rmse = sqrt(sse / dn); st->persist_rmse = sqrt(ssp / dn);
        st->zero_innov_frac = (double)zero / dn;
    }
    for (int q = 0; q < 4; q++) st->qcov[q] = st->qn[q] ? (double)qh[q] / (double)st->qn[q] : 0;
    for (int g = 0; g < 2; g++) st->regime_cov[g] = st->regime_n[g] ? (double)rh[g] / (double)st->regime_n[g] : 0;
    /* ten-step coverage from each belief at L >= burn-in */
    size_t tn = 0, th = 0;
    for (size_t i = 0; i < rp->nsteps; i++) {
        const est_step *s = &rp->steps[i];
        if (s->prior || s->coast || s->L < EST_BURN_IN) continue;   /* coasted origins are skipped */
        int64_t target = s->t_ns + (int64_t)TEN_STEP * 1000000000ll;
        const est_step *tg = NULL;
        for (size_t j = i + 1; j < rp->nsteps && rp->steps[j].t_ns <= target; j++)
            if (rp->steps[j].t_ns == target && !rp->steps[j].coast) { tg = &rp->steps[j]; break; }
        if (!tg) continue;
        est_prediction p;
        if (est_kf_predict(&rp->model, &s->post, NULL, TEN_STEP, &p) != EST_OK) continue;
        tn++;
        double t10 = ZC95 * sqrt(p.S[0]);
        if (mx) { double tt[3]; if (shape_thr(&sq, TEN_STEP, tt)) t10 = -1.0; else t10 = tt[2]; }
        if (fabs(tg->z - p.y_mean[0]) <= t10) th++;
    }
    st->ten_n = tn;
    if (tn) { st->ten_cov = (double)th / (double)tn; wilson(th, tn, &st->ten_lo, &st->ten_hi); }
    /* pass rules */
    st->ok_cov[0] = in_band(st->cov[0], B50); st->ok_cov[1] = in_band(st->cov[1], B80);
    st->ok_cov[2] = in_band(st->cov[2], B95);
    st->ok_nis = in_band(st->mean_nis, BNIS);
    st->ok_bias = fabs(st->bias) <= 0.10;
    st->ok_lag1 = fabs(st->lag1) <= 0.20;
    st->calibrated = n >= 3 && st->ok_cov[0] && st->ok_cov[1] && st->ok_cov[2] && st->ok_nis && st->ok_bias && st->ok_lag1;
    for (int q = 0; q < 4; q++) { st->ok_q[q] = st->qn[q] > 0 && in_band(st->qcov[q], BQ); if (!st->ok_q[q]) st->calibrated = 0; }
    for (int g = 0; g < 2; g++) {
        st->ok_regime[g] = st->regime_n[g] < 100 ? 1 : in_band(st->regime_cov[g], BQ);
        if (!st->ok_regime[g]) st->calibrated = 0;
    }
    st->ok_ten = tn > 0 && in_band(st->ten_cov, BQ);
    if (!st->ok_ten) st->calibrated = 0;
    st->rmse_le_persist = n >= 3 && st->rmse <= st->persist_rmse;
    if (beyond) *beyond = sq.beyond;
    free(w); free(nu);
}

static const char *b(int v) { return v ? "true" : "false"; }
static void js(FILE *fp, const char *s)
{
    fputc('"', fp);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', fp); fputc(c, fp); }
        else if (c < 0x20) fprintf(fp, "\\u%04x", c);
        else fputc(c, fp);
    }
    fputc('"', fp);
}
static void jd(FILE *fp, const char *k, double v)
{
    if (isfinite(v)) fprintf(fp, "\"%s\": %.17g", k, v); else fprintf(fp, "\"%s\": null", k);
}

static void emit_model(FILE *fp, int m, double q, double r, double ll, const est_mix *mx, const model_stats *s, int last)
{
    fprintf(fp, "    \"M%d\": {\n      \"q\": %.17g, \"r\": %.17g, \"fit_loglik\": %.17g,\n", m, q, r, ll);
    if (mx) {
        fprintf(fp, "      \"predictive_shape\": {\"kind\": \"gaussian_scale_mixture\", \"k\": %u, \"w\": [", mx->k);
        for (uint32_t j = 0; j < mx->k; j++) fprintf(fp, "%s%.17g", j ? ", " : "", mx->w[j]);
        fprintf(fp, "], \"v\": [");
        for (uint32_t j = 0; j < mx->k; j++) fprintf(fp, "%s%.17g", j ? ", " : "", mx->v[j]);
        fprintf(fp, "]},\n");
    }
    fprintf(fp, "      \"samples\": %zu, \"included_logical_span_ns\": [%lld, %lld],\n", s->n, (long long)s->tmin_ns, (long long)s->tmax_ns);
    const char *nm[3] = { "50", "80", "95" };
    for (int k = 0; k < 3; k++) {
        fprintf(fp, "      \"coverage_%s\": {\"value\": %.17g, \"wilson95\": [%.17g, %.17g], \"pass\": %s},\n",
                nm[k], s->cov[k], s->wlo[k], s->whi[k], b(s->ok_cov[k]));
    }
    fprintf(fp, "      \"mean_nis\": {"); jd(fp, "value", s->mean_nis); fprintf(fp, ", \"pass\": %s},\n", b(s->ok_nis));
    fprintf(fp, "      \"standardized_bias\": {"); jd(fp, "value", s->bias); fprintf(fp, ", \"pass\": %s},\n", b(s->ok_bias));
    fprintf(fp, "      \"lag1_autocorr\": {"); jd(fp, "value", s->lag1); fprintf(fp, ", \"pass\": %s},\n", b(s->ok_lag1));
    fprintf(fp, "      \"ljung_box_q10_reported_only\": "); if (isfinite(s->ljung_box10)) fprintf(fp, "%.17g", s->ljung_box10); else fprintf(fp, "null");
    fprintf(fp, ",\n      \"quarters_coverage95\": [\n");
    for (int q = 0; q < 4; q++)
        fprintf(fp, "        {\"n\": %zu, \"value\": %.17g, \"pass\": %s}%s\n", s->qn[q], s->qcov[q], b(s->ok_q[q]), q < 3 ? "," : "");
    fprintf(fp, "      ],\n      \"regime_coverage95\": {\n");
    fprintf(fp, "        \"idle\": {\"n\": %zu, \"value\": %.17g, \"gated\": %s, \"pass\": %s},\n", s->regime_n[0], s->regime_cov[0], b(s->regime_n[0] >= 100), b(s->ok_regime[0]));
    fprintf(fp, "        \"in_trial\": {\"n\": %zu, \"value\": %.17g, \"gated\": %s, \"pass\": %s}\n      },\n", s->regime_n[1], s->regime_cov[1], b(s->regime_n[1] >= 100), b(s->ok_regime[1]));
    fprintf(fp, "      \"ten_step_coverage95\": {\"n\": %zu, \"value\": %.17g, \"wilson95\": [%.17g, %.17g], \"pass\": %s},\n", s->ten_n, s->ten_cov, s->ten_lo, s->ten_hi, b(s->ok_ten));
    fprintf(fp, "      \"log_score_per_step\": "); if (isfinite(s->logscore)) fprintf(fp, "%.17g", s->logscore); else fprintf(fp, "null");
    fprintf(fp, ",\n      \"rmse_one_step\": %.17g, \"persistence_rmse\": %.17g, \"rmse_no_worse_than_persistence\": %s,\n",
            s->rmse, s->persist_rmse, b(s->rmse_le_persist));
    fprintf(fp, "      \"zero_innovation_fraction_extra\": %.17g,\n", s->zero_innov_frac);
    fprintf(fp, "      \"calibrated\": %s,\n      \"failed_criteria\": [", b(s->calibrated));
    int first = 1;
#define FC(cond, name) do { if (!(cond)) { fprintf(fp, "%s\"%s\"", first ? "" : ", ", name); first = 0; } } while (0)
    FC(s->ok_cov[0], "coverage_50"); FC(s->ok_cov[1], "coverage_80"); FC(s->ok_cov[2], "coverage_95");
    FC(s->ok_nis, "mean_nis"); FC(s->ok_bias, "standardized_bias"); FC(s->ok_lag1, "lag1_autocorr");
    FC(s->ok_q[0], "quarter_1"); FC(s->ok_q[1], "quarter_2"); FC(s->ok_q[2], "quarter_3"); FC(s->ok_q[3], "quarter_4");
    FC(s->ok_regime[0], "regime_idle"); FC(s->ok_regime[1], "regime_in_trial"); FC(s->ok_ten, "ten_step_coverage_95");
    fprintf(fp, "], \"failed_verdict_only\": [");
    first = 1;
    FC(s->n >= 3, "samples_lt_3"); FC(s->rmse_le_persist, "rmse_vs_persistence");
#undef FC
    fprintf(fp, "]\n    }%s\n", last ? "" : ",");
}

#ifndef TOOL_COMMIT
#define TOOL_COMMIT "unknown"
#endif
#ifndef TOOL_DIRTY
#define TOOL_DIRTY 1
#endif

static int tool_dirty(void)
{
#ifdef EST_TEST_BUILD
    const char *e = getenv("EST_TEST_DIRTY"); if (e && *e) return atoi(e);
#endif
    return TOOL_DIRTY;
}

static int open_excl(const char *path, FILE **out)
{
    *out = fopen(path, "wbx");
    if (!*out) { fprintf(stderr, "refuse: cannot create %s (%s); existing outputs are never overwritten\n", path, strerror(errno)); return 1; }
    return 0;
}

static void dir_and_name(const char *path, char *dir, size_t cap, const char **name)
{
    const char *slash = strrchr(path, '/');
    *name = slash ? slash + 1 : path;
    if (slash) { size_t n = (size_t)(slash - path); if (n >= cap) n = cap - 1; memcpy(dir, path, n); dir[n] = 0; }
    else snprintf(dir, cap, ".");
}

/* Both the resolved path (component with run B's id) and the digest in the
 * SHA256SUMS text beside the file identify the input as run B. */
static int is_b_input(const char *path, const char *b_sha, int *by_path, int *by_sums)
{
    char dir[1024], hex[65]; const char *name;
    dir_and_name(path, dir, sizeof dir, &name);
    *by_path = est_path_is_run_b(path);
    *by_sums = (est_sums_lookup(dir, name, hex) == 0 && strcmp(hex, b_sha) == 0);
    return *by_path || *by_sums;
}

/* Physically truncate: write the first k lines to a temp file, replay that
 * file from scratch, compare bit for bit with the first records of the full run. */
static int prefix_check(const est_file *f, const est_replay *full, int nm, const int *ids, const double *q, const double *r,
                        const size_t *ks, int nk, size_t *ran)
{
    const char *tb = getenv("TMPDIR"); if (!tb || !*tb) tb = "/tmp";
    char dir[600]; snprintf(dir, sizeof dir, "%s/est_eval_prefix_XXXXXX", tb);
    if (!mkdtemp(dir)) return 0;
    char path[700]; snprintf(path, sizeof path, "%s/prefix.ndjson", dir);
    static est_replay pre;
    int ok = 1;
    for (int ki = 0; ki < nk; ki++) {
        size_t k = ks[ki];
        if (k > f->nlines) { ok = 0; continue; }
        FILE *fp = fopen(path, "wb");
        if (!fp) { ok = 0; break; }
        for (size_t i = 0; i < k; i++) {
            fwrite(f->data + f->off[i], 1, f->llen[i], fp); fputc('\n', fp);
        }
        if (fclose(fp)) { ok = 0; break; }
        est_file pf; char err[300];
        if (est_file_load_raw(path, &pf, err, sizeof err)) { ok = 0; break; }
        for (int m = 0; m < nm; m++) {
            if (est_replay_run(&pf, (size_t)-1, ids[m], q[m], r[m], &pre)) { ok = 0; continue; }
            size_t nn = pre.nsteps;
            if (nn == 0 || nn > full[m].nsteps) { ok = 0; continue; }
            for (size_t i = 0; i < nn; i++) if (!step_equal(&pre.steps[i], &full[m].steps[i])) { ok = 0; break; }
        }
        est_file_free(&pf);
        (*ran)++;
    }
    est_replay_free(&pre);
    unlink(path); rmdir(dir);
    return ok;
}

/* ---- Protocol v2 (docs/estimation/EST23_PROTOCOL_V2.md) ----
 *   est_eval --protocol-v2 --params2 P2 --raw F --marks M --out R
 *            [--protocol-doc D] [--recorded] [--synthetic-test]
 * Evaluates M0 and M1 with the frozen v1 parameters (named inside P2) and M2
 * with its fitted shape, on one file. --recorded only for held-out run C2. */
static const char *v2_heldout_raw_sha(void)
{
#ifdef EST_TEST_BUILD
    const char *e = getenv("EST_TEST_C2_RAW_SHA"); if (e && *e) return e;
#endif
    return EST_V2_HELDOUT_RAW_SHA;
}
static const char *v2_heldout_marks_sha(void)
{
#ifdef EST_TEST_BUILD
    const char *e = getenv("EST_TEST_C2_MARKS_SHA"); if (e && *e) return e;
#endif
    return EST_V2_HELDOUT_MARKS_SHA;
}
static const char *v2_fit_sha(void)
{
#ifdef EST_TEST_BUILD
    const char *e = getenv("EST_TEST_C1_SHA"); if (e && *e) return e;
#endif
    return EST_V2_FIT_SHA;
}
static const char *v2_doc_sha(void)
{
#ifdef EST_TEST_BUILD
    const char *e = getenv("EST_TEST_V2_DOC_SHA"); if (e && *e) return e;
#endif
    return EST_V2_PROTOCOL_DOC_SHA;
}
static int is_c2_input(const char *path, const char *want, int *by_path, int *by_sums)
{
    char dir[1024], hex[65]; const char *name;
    dir_and_name(path, dir, sizeof dir, &name);
    *by_path = est_path_is_heldout(path);
    *by_sums = (est_sums_lookup(dir, name, hex) == 0 && strcmp(hex, want) == 0);
    return *by_path || *by_sums;
}

static int main_v2(int argc, char **argv)
{
    const char *params = NULL, *raw = NULL, *mkp = NULL, *out = NULL, *pdoc = EST_V2_PROTOCOL_DOC;
    int recorded = 0, synth = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--params2") && i + 1 < argc) params = argv[++i];
        else if (!strcmp(argv[i], "--raw") && i + 1 < argc) raw = argv[++i];
        else if (!strcmp(argv[i], "--marks") && i + 1 < argc) mkp = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--protocol-doc") && i + 1 < argc) pdoc = argv[++i];
        else if (!strcmp(argv[i], "--recorded")) recorded = 1;
        else if (!strcmp(argv[i], "--synthetic-test")) synth = 1;
        else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }
    if (!params || !raw || !mkp || !out) {
        fprintf(stderr, "usage: est_eval --protocol-v2 --params2 P2 --raw F --marks M --out R [--protocol-doc D] [--recorded] [--synthetic-test]\n");
        return 2;
    }
    if (recorded && synth) { fprintf(stderr, "refuse: --recorded and --synthetic-test are exclusive\n"); return 2; }
    int bp, bs;
    if (is_b_input(raw, est_run_b_raw_sha(), &bp, &bs) || is_b_input(mkp, est_run_b_marks_sha(), &bp, &bs)) {
        fprintf(stderr, "refuse: run B is closed evidence under protocol v1 and is never read by protocol v2\n"); return 2;
    }
    int rp_path, rp_sums, mp_path, mp_sums;
    int raw_c2 = is_c2_input(raw, v2_heldout_raw_sha(), &rp_path, &rp_sums);
    int mk_c2 = is_c2_input(mkp, v2_heldout_marks_sha(), &mp_path, &mp_sums);
    if (recorded) {
        if (!(rp_path && rp_sums)) { fprintf(stderr, "refuse: --recorded is accepted only for held-out run C2's raw file (path and SHA256SUMS digest must both match)\n"); return 2; }
        if (!(mp_path && mp_sums)) { fprintf(stderr, "refuse: --recorded needs C2's own marks file (path and SHA256SUMS digest must both match)\n"); return 2; }
        if (tool_dirty()) { fprintf(stderr, "refuse: --recorded needs a clean working tree at build time (TOOL_DIRTY=%d)\n", tool_dirty()); return 2; }
    } else if (raw_c2 || mk_c2) {
        fprintf(stderr, "refuse: the held-out run may only be evaluated with --recorded (protocol v2)\n"); return 2;
    }
    char err[300], p2hex[65], p1hex[65], pdoc_hex[65], bin_hex[65];
    est_params2 p2; est_params p1;
    if (est_params2_read(params, &p2)) { fprintf(stderr, "refuse: v2 parameter file is not exactly the expected format\n"); return 1; }
    if (est_mix_check(&p2.mix) != EST_OK) { fprintf(stderr, "refuse: M2 shape is not a valid mixture\n"); return 1; }
    double tv = est_mix_total_var(&p2.mix);
    if (!(p2.r == EST_M2_R) || !(fabs(p2.q - tv) <= 1e-12 * tv)) { fprintf(stderr, "refuse: M2 q must be the shape's total variance and r must be %g\n", EST_M2_R); return 1; }
    if (est_params_read(p2.v1_params_path, &p1) || est_params_validate(&p1, err, sizeof err)) { fprintf(stderr, "refuse: v1 parameter file %s is not valid\n", p2.v1_params_path); return 1; }
    if (est_sha_file_hex(p2.v1_params_path, p1hex) || strcmp(p1hex, p2.v1_params_sha) != 0) { fprintf(stderr, "refuse: v1 parameter file digest differs from the one recorded in the v2 parameter file\n"); return 1; }
    int v1_fit_is_a = strstr(p1.fit_path, EST_RUN_A_ID) != NULL && strcmp(p1.fit_sha, EST_RUN_A_SHA) == 0;
    int fit_is_c1 = strstr(p2.fit_path, EST_V2_FIT_TAG) != NULL && strcmp(p2.fit_sha, v2_fit_sha()) == 0;
    int fit_sums_ok = 0;
    {
        char dir[1024], hex[65]; const char *name;
        dir_and_name(p2.fit_path, dir, sizeof dir, &name);
        fit_sums_ok = (strcmp(name, "machine-state.ndjson") == 0 && est_sums_lookup(dir, name, hex) == 0 && strcmp(hex, p2.fit_sha) == 0);
    }
    if (!synth && !(v1_fit_is_a && fit_is_c1)) { fprintf(stderr, "refuse: M0/M1 must be fit on run A and M2 on run C1\n"); return 1; }
    if (recorded && !fit_sums_ok) { fprintf(stderr, "refuse: --recorded needs the M2 fit file to match its SHA256SUMS line\n"); return 1; }
    int pdoc_ok = !est_sha_file_hex(pdoc, pdoc_hex);
    int pdoc_matches = pdoc_ok && strcmp(pdoc_hex, v2_doc_sha()) == 0;
    if (recorded && !pdoc_matches) { fprintf(stderr, "refuse: --recorded needs the frozen v2 protocol document (SHA-256 %s)\n", v2_doc_sha()); return 1; }
    if (!pdoc_ok) snprintf(pdoc_hex, sizeof pdoc_hex, "unavailable");
    if (est_sha_file_hex("/proc/self/exe", bin_hex)) snprintf(bin_hex, sizeof bin_hex, "unavailable");
    if (est_sha_file_hex(params, p2hex)) return 1;

    est_allow_heldout = recorded;     /* only after every identity check above */
    est_file f;
    if (est_file_load(raw, &f, err, sizeof err)) { fprintf(stderr, "refuse: %s\n", err); return 1; }
    if (recorded && strcmp(f.sha_hex, v2_heldout_raw_sha()) != 0) { fprintf(stderr, "refuse: raw digest is not C2's\n"); return 1; }
    marks mk;
    if (load_marks(mkp, &mk, err, sizeof err)) { fprintf(stderr, "refuse: %s\n", err); return 1; }
    if (recorded && strcmp(mk.sha, v2_heldout_marks_sha()) != 0) { fprintf(stderr, "refuse: marks digest is not C2's\n"); return 1; }

    enum { NM = 3 };
    const int ids[NM] = { 0, 1, 2 };
    const double q[NM] = { p1.q[0], p1.q[1], p2.q }, r[NM] = { p1.r[0], p1.r[1], p2.r }, ll[NM] = { p1.ll[0], p1.ll[1], p2.ll };
    FILE *fp = NULL, *sfp[NM] = { NULL, NULL, NULL };
    char spath[NM][1100];
    if (open_excl(out, &fp)) return 1;
    for (int m = 0; m < NM; m++) { snprintf(spath[m], sizeof spath[m], "%s.M%d.stream", out, m); if (open_excl(spath[m], &sfp[m])) return 1; }

    static est_replay rp[NM];
    model_stats st[NM];
    memset(st, 0, sizeof st);
    int replay_ok = 1, chain_ok = 1, m2_persist = 0;
    size_t ks[3] = { 100, 500, 1000 }, prefix_ran = 0, expect_steps = 0, beyond = 0;
    for (int m = 0; m < NM; m++) {
        if (est_replay_run(&f, (size_t)-1, ids[m], q[m], r[m], &rp[m])) { replay_ok = 0; fprintf(stderr, "replay M%d: %s\n", m, rp[m].errmsg); continue; }
        expect_steps = f.nlines - rp[m].leading_missing;
        if (rp[m].nsteps != expect_steps) replay_ok = 0;
        for (size_t i = 0; i < rp[m].nsteps; i++) if (!rp[m].steps[i].chain_ok) chain_ok = 0;
        compute(&rp[m], &mk, &st[m], m == 2 ? &p2.mix : NULL, m == 2 ? &beyond : NULL);
    }
    if (replay_ok) {
        double *e = malloc((rp[2].nsteps + 1) * sizeof *e);
        m2_persist = e && est_m2_changes(&rp[2], e, rp[2].nsteps) != (size_t)-1;
        free(e);
    }
    int prefix_ok = replay_ok && prefix_check(&f, rp, NM, ids, q, r, ks, 3, &prefix_ran);
    char shash[NM][65] = { "unavailable", "unavailable", "unavailable" };
    int streams_ok = replay_ok;
    for (int m = 0; m < NM; m++) {
        if (replay_ok) est_replay_write_stream(sfp[m], &rp[m], m);
        int bad = fclose(sfp[m]);
        if (bad || (replay_ok && est_sha_file_hex(spath[m], shash[m]))) { fprintf(stderr, "cannot write stream %s\n", spath[m]); return 1; }
        if (!strcmp(shash[m], "unavailable")) streams_ok = 0;
    }
    char after[65]; int raw_preserved = !est_sha_file_hex(raw, after) && strcmp(after, f.sha_hex) == 0;
    int sel = -1; double best = -INFINITY;
    if (replay_ok) {
        for (int m = 0; m < NM; m++) if (st[m].calibrated && st[m].logscore > best) best = st[m].logscore;
        for (int m = 0; m < NM && sel < 0; m++) if (st[m].calibrated && st[m].logscore >= best - 0.01) sel = m;
    }
    int real_signal = replay_ok && prefix_ok && raw_preserved && chain_ok && streams_ok && m2_persist;
    int calib = 0;
    if (replay_ok) for (int m = 0; m < NM; m++) if (st[m].calibrated && st[m].rmse_le_persist) calib = 1;

    fprintf(fp, "{\n  \"receipt\": \"est_eval_v2\",\n  \"protocol\": \"EST23_PROTOCOL_V2\",\n");
    fprintf(fp, "  \"protocol_doc_sha256\": \"%s\",\n  \"protocol_doc_matches_frozen_constant\": %s,\n", pdoc_hex, b(pdoc_matches));
    fprintf(fp, "  \"tool_commit\": "); js(fp, TOOL_COMMIT);
    fprintf(fp, ",\n  \"tool_dirty\": %d,\n  \"eval_binary_sha256\": \"%s\",\n", tool_dirty(), bin_hex);
    fprintf(fp, "  \"recorded\": %s,\n  \"synthetic_test\": %s,\n", b(recorded), b(synth));
    fprintf(fp, "  \"parameter_file_v2\": "); js(fp, params);
    fprintf(fp, ",\n  \"parameter_file_v2_sha256\": \"%s\",\n  \"v1_parameter_file\": ", p2hex); js(fp, p2.v1_params_path);
    fprintf(fp, ",\n  \"v1_parameter_file_sha256\": \"%s\",\n  \"v1_models_fit_on_run_A\": %s,\n", p1hex, b(v1_fit_is_a));
    fprintf(fp, "  \"m2_fit_run_path\": "); js(fp, p2.fit_path);
    fprintf(fp, ",\n  \"m2_fit_file_sha256\": \"%s\",\n  \"m2_fit_is_run_C1\": %s,\n  \"m2_fit_sha_matches_SHA256SUMS_line\": %s,\n  \"m2_fit_changes\": %zu,\n",
            p2.fit_sha, b(fit_is_c1), b(fit_sums_ok), p2.fit_n);
    fprintf(fp, "  \"input_file\": "); js(fp, raw);
    fprintf(fp, ",\n  \"input_file_sha256\": \"%s\",\n  \"input_sha_matches_SHA256SUMS\": true,\n  \"input_is_heldout_C2\": %s,\n", f.sha_hex, b(raw_c2 && rp_path && rp_sums));
    fprintf(fp, "  \"marks_file_sha256\": \"%s\",\n", mk.sha);
    fprintf(fp, "  \"lines\": %zu,\n  \"marks\": {\"pairs\": %zu, \"begins\": %zu, \"ends\": %zu, \"unclosed_begin_ran_to_eof\": %s, \"nested_begins\": %zu, \"stray_ends\": %zu},\n",
            f.nlines, mk.n, mk.begins, mk.ends, b(mk.unclosed), mk.nested_begins, mk.stray_ends);
    fprintf(fp, "  \"missing_observations\": {\"leading_before_first_valid\": %zu, \"coasted\": %zu, \"bad_value_lines\": %zu, \"bad_t_lines\": %zu, \"bad_t_overflow_lines\": %zu},\n",
            rp[0].leading_missing, rp[0].coasts, rp[0].bad_value, rp[0].bad_t, rp[0].bad_t_overflow);
    fprintf(fp, "  \"time_gaps\": {\"duplicate_t\": %zu, \"backward_t\": %zu, \"steps_with_horizon_gt_1\": %zu, \"m2_steps_beyond_exact_horizon\": %zu},\n",
            rp[0].gap_zero, rp[0].gap_backward, rp[0].multi_horizon, beyond);
    fprintf(fp, "  \"readings\": {\"burn_in\": \"steps with L<%u counted from the prior line (L=0) are excluded\", \"quarters\": \"four equal spans of included samples by logical time\", \"ten_step_origins\": \"coasted origins skipped\", \"m2_intervals\": \"central quantiles of the h-step mixture sum\"},\n", EST_BURN_IN);
    fprintf(fp, "  \"m2_mean_is_exact_persistence\": %s,\n", b(m2_persist));
    fprintf(fp, "  \"replay\": {\"complete\": %s, \"steps\": %zu, \"expected_steps\": %zu, \"chain_recorded_every_step\": %s, \"raw_file_unchanged\": %s,\n",
            b(replay_ok), rp[0].nsteps, expect_steps, b(chain_ok), b(raw_preserved));
    fprintf(fp, "             \"prefix_invariance\": {\"method\": \"first k lines physically written to a temp file and replayed from scratch\", \"ks\": \"k=100,500,1000\", \"runs\": %zu, \"pass\": %s}},\n", prefix_ran, b(prefix_ok));
    fprintf(fp, "  \"streams\": {");
    for (int m = 0; m < NM; m++) { fprintf(fp, "%s\"M%d\": {\"file\": ", m ? ", " : "", m); js(fp, spath[m]); fprintf(fp, ", \"sha256\": \"%s\"}", shash[m]); }
    fprintf(fp, "},\n  \"models\": {\n");
    if (replay_ok) for (int m = 0; m < NM; m++) emit_model(fp, m, q[m], r[m], ll[m], m == 2 ? &p2.mix : NULL, &st[m], m == NM - 1);
    if (sel >= 0) fprintf(fp, "  },\n  \"selected_model\": \"M%d\",\n", sel); else fprintf(fp, "  },\n  \"selected_model\": null,\n");
    fprintf(fp, "  \"failure_class_to_be_recorded_by_a_human_on_FAIL\": null,\n");
    fprintf(fp, "  \"ESTIMATION_REAL_SIGNAL\": \"%s\",\n  \"ESTIMATION_CALIBRATION\": \"%s\"\n}\n", real_signal ? "PASS" : "FAIL", calib ? "PASS" : "FAIL");
    if (fclose(fp)) return 1;
    char rh[65]; est_sha_file_hex(out, rh);
    printf("ESTIMATION_REAL_SIGNAL=%s ESTIMATION_CALIBRATION=%s selected=%s%d receipt_sha256=%s\n", real_signal ? "PASS" : "FAIL", calib ? "PASS" : "FAIL", sel >= 0 ? "M" : "none", sel, rh);
    for (int m = 0; m < NM; m++) est_replay_free(&rp[m]);
    est_file_free(&f);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "--protocol-v2")) return main_v2(argc - 1, argv + 1);
    const char *params = NULL, *raw = NULL, *mkp = NULL, *out = NULL;
    const char *pdoc = "docs/estimation/EST23_PROTOCOL_V1.md";
    const char *interp = "docs/estimation/EST23_PROTOCOL_V1_INTERPRETATIONS.md";
    int recorded = 0, synth = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--params") && i + 1 < argc) params = argv[++i];
        else if (!strcmp(argv[i], "--raw") && i + 1 < argc) raw = argv[++i];
        else if (!strcmp(argv[i], "--marks") && i + 1 < argc) mkp = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--protocol-doc") && i + 1 < argc) pdoc = argv[++i];
        else if (!strcmp(argv[i], "--interpretations") && i + 1 < argc) interp = argv[++i];
        else if (!strcmp(argv[i], "--recorded")) recorded = 1;
        else if (!strcmp(argv[i], "--synthetic-test")) synth = 1;
        else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }
    if (!params || !raw || !mkp || !out) {
        fprintf(stderr, "usage: est_eval --params P --raw F --marks M --out R [--protocol-doc D] [--interpretations I] [--recorded] [--synthetic-test]\n");
        return 2;
    }
    if (recorded && synth) { fprintf(stderr, "refuse: --recorded and --synthetic-test are exclusive\n"); return 2; }

    /* run B identity: by resolved path AND by the digest in the SHA256SUMS text */
    int rp_path, rp_sums, mp_path, mp_sums;
    int raw_b = is_b_input(raw, est_run_b_raw_sha(), &rp_path, &rp_sums);
    int mk_b = is_b_input(mkp, est_run_b_marks_sha(), &mp_path, &mp_sums);
    if (recorded) {
        if (!(rp_path && rp_sums)) { fprintf(stderr, "refuse: --recorded is accepted only for run B's raw file (path and SHA256SUMS digest must both match)\n"); return 2; }
        if (!(mp_path && mp_sums)) { fprintf(stderr, "refuse: --recorded needs run B's own marks file (path and SHA256SUMS digest must both match)\n"); return 2; }
        if (tool_dirty()) { fprintf(stderr, "refuse: --recorded needs a clean working tree at build time (TOOL_DIRTY=%d)\n", tool_dirty()); return 2; }
    } else if (raw_b || mk_b) {
        fprintf(stderr, "refuse: run B may only be evaluated with --recorded (protocol section 6)\n"); return 2;
    }
    est_params p;
    char err[300], phex[65], pdoc_hex[65], interp_hex[65], bin_hex[65];
    if (est_params_read(params, &p)) { fprintf(stderr, "refuse: parameter file is not exactly the expected format\n"); return 1; }
    if (est_params_validate(&p, err, sizeof err)) { fprintf(stderr, "refuse: parameter file: %s\n", err); return 1; }
    int fit_is_a = strstr(p.fit_path, EST_RUN_A_ID) != NULL && strcmp(p.fit_sha, EST_RUN_A_SHA) == 0;
    int fit_sums_ok = 0;
    {
        char dir[1024], hex[65]; const char *name;
        dir_and_name(p.fit_path, dir, sizeof dir, &name);
        fit_sums_ok = (strcmp(name, "machine-state.ndjson") == 0 && est_sums_lookup(dir, name, hex) == 0 && strcmp(hex, p.fit_sha) == 0);
    }
    if (!fit_is_a && !synth) {
        fprintf(stderr, "refuse: parameter file was not fit on run A (%s)\n", p.fit_path); return 1;
    }
    if (recorded && !(fit_is_a && fit_sums_ok)) {
        fprintf(stderr, "refuse: --recorded needs the fit file to be run A with a matching SHA256SUMS line\n"); return 1;
    }
    int pdoc_ok = !est_sha_file_hex(pdoc, pdoc_hex);
    int interp_ok = !est_sha_file_hex(interp, interp_hex);
    int pdoc_matches = pdoc_ok && strcmp(pdoc_hex, EST_PROTOCOL_DOC_SHA) == 0;
    if (recorded && !(pdoc_matches && interp_ok)) {
        fprintf(stderr, "refuse: --recorded needs the frozen protocol document (SHA-256 %s) and the interpretations file\n", EST_PROTOCOL_DOC_SHA); return 1;
    }
    if (!pdoc_ok) snprintf(pdoc_hex, sizeof pdoc_hex, "unavailable");
    if (!interp_ok) snprintf(interp_hex, sizeof interp_hex, "unavailable");
    if (est_sha_file_hex("/proc/self/exe", bin_hex)) snprintf(bin_hex, sizeof bin_hex, "unavailable");
    if (est_sha_file_hex(params, phex)) return 1;

    est_allow_run_b = recorded;       /* only after every identity check above */
    est_file f;
    if (est_file_load(raw, &f, err, sizeof err)) { fprintf(stderr, "refuse: %s\n", err); return 1; }
    if (recorded && strcmp(f.sha_hex, est_run_b_raw_sha()) != 0) { fprintf(stderr, "refuse: raw digest is not run B's\n"); return 1; }
    marks mk;
    if (load_marks(mkp, &mk, err, sizeof err)) { fprintf(stderr, "refuse: %s\n", err); return 1; }
    if (recorded && strcmp(mk.sha, est_run_b_marks_sha()) != 0) { fprintf(stderr, "refuse: marks digest is not run B's\n"); return 1; }

    /* outputs are created exclusively, before any evaluation work */
    FILE *fp = NULL, *sfp[2] = { NULL, NULL };
    char spath[2][1100];
    snprintf(spath[0], sizeof spath[0], "%s.M0.stream", out);
    snprintf(spath[1], sizeof spath[1], "%s.M1.stream", out);
    if (open_excl(out, &fp)) return 1;
    for (int m = 0; m < 2; m++) if (open_excl(spath[m], &sfp[m])) return 1;

    static est_replay rp[2];
    model_stats st[2];
    memset(st, 0, sizeof st);
    int replay_ok = 1, chain_ok = 1;
    size_t ks[3] = { 100, 500, 1000 }, prefix_ran = 0;
    size_t expect_steps = 0;
    for (int m = 0; m < 2; m++) {
        if (est_replay_run(&f, (size_t)-1, m, p.q[m], p.r[m], &rp[m])) { replay_ok = 0; fprintf(stderr, "replay M%d: %s\n", m, rp[m].errmsg); continue; }
        expect_steps = f.nlines - rp[m].leading_missing;
        if (rp[m].nsteps != expect_steps) replay_ok = 0;
        for (size_t i = 0; i < rp[m].nsteps; i++) if (!rp[m].steps[i].chain_ok) chain_ok = 0;
        compute(&rp[m], &mk, &st[m], NULL, NULL);
    }
    const int ids[2] = { 0, 1 };
    int prefix_ok = replay_ok && prefix_check(&f, rp, 2, ids, p.q, p.r, ks, 3, &prefix_ran);
    /* per-step observation / prediction / innovation stream, every evaluated model */
    char shash[2][65] = { "unavailable", "unavailable" };
    for (int m = 0; m < 2; m++) {
        if (replay_ok) est_replay_write_stream(sfp[m], &rp[m], m);
        int bad = fclose(sfp[m]);
        if (bad || (replay_ok && est_sha_file_hex(spath[m], shash[m]))) { fprintf(stderr, "cannot write stream %s\n", spath[m]); return 1; }
    }
    /* raw preserved: the file on disk still hashes to the verified digest */
    char after[65]; int raw_preserved = !est_sha_file_hex(raw, after) && strcmp(after, f.sha_hex) == 0;
    if (!replay_ok) { fprintf(stderr, "replay did not complete\n"); }

    /* selection: simplest calibrated model within 0.01 nats of the best calibrated */
    int sel = -1; double best = -INFINITY;
    if (replay_ok) {
        for (int m = 0; m < 2; m++) if (st[m].calibrated && st[m].logscore > best) best = st[m].logscore;
        for (int m = 0; m < 2 && sel < 0; m++) if (st[m].calibrated && st[m].logscore >= best - 0.01) sel = m;
    }
    int streams_ok = replay_ok && strcmp(shash[0], "unavailable") && strcmp(shash[1], "unavailable");
    int real_signal = replay_ok && prefix_ok && raw_preserved && chain_ok && streams_ok;
    int calib = 0;
    if (replay_ok) for (int m = 0; m < 2; m++) if (st[m].calibrated && st[m].rmse_le_persist) calib = 1;

    fprintf(fp, "{\n  \"receipt\": \"est_eval_v1\",\n  \"protocol\": \"EST23_PROTOCOL_V1\",\n  \"protocol_commit\": ");
    js(fp, EST_PROTOCOL_COMMIT);
    fprintf(fp, ",\n  \"protocol_doc_sha256\": \"%s\",\n  \"protocol_doc_matches_frozen_constant\": %s,\n  \"interpretations_sha256\": \"%s\",\n",
            pdoc_hex, b(pdoc_matches), interp_hex);
    fprintf(fp, "  \"tool_commit\": "); js(fp, TOOL_COMMIT);
    fprintf(fp, ",\n  \"tool_dirty\": %d,\n  \"eval_binary_sha256\": \"%s\",\n", tool_dirty(), bin_hex);
    fprintf(fp, "  \"recorded\": %s,\n  \"synthetic_test\": %s,\n", b(recorded), b(synth));
    fprintf(fp, "  \"parameter_file\": "); js(fp, params);
    fprintf(fp, ",\n  \"parameter_file_sha256\": \"%s\",\n  \"parameter_file_validated_on_grid\": true,\n", phex);
    fprintf(fp, "  \"fit_run_path\": "); js(fp, p.fit_path);
    fprintf(fp, ",\n  \"fit_file_sha256\": \"%s\",\n  \"fit_file_is_run_A\": %s,\n  \"fit_sha_matches_run_A_SHA256SUMS_line\": %s,\n", p.fit_sha, b(fit_is_a), b(fit_sums_ok));
    fprintf(fp, "  \"input_file\": "); js(fp, raw);
    fprintf(fp, ",\n  \"input_file_sha256\": \"%s\",\n  \"input_sha_matches_SHA256SUMS\": true,\n", f.sha_hex);
    fprintf(fp, "  \"input_sha_note\": \"only the evaluated input is verified by this tool against its SHA256SUMS line (loading refuses otherwise); the fit file digest is as recorded in the parameter file\",\n");
    fprintf(fp, "  \"marks_file_sha256\": \"%s\",\n", mk.sha);
    fprintf(fp, "  \"lines\": %zu,\n  \"marks\": {\"pairs\": %zu, \"begins\": %zu, \"ends\": %zu, \"unclosed_begin_ran_to_eof\": %s, \"nested_begins\": %zu, \"stray_ends\": %zu},\n",
            f.nlines, mk.n, mk.begins, mk.ends, b(mk.unclosed), mk.nested_begins, mk.stray_ends);
    fprintf(fp, "  \"missing_observations\": {\"leading_before_first_valid\": %zu, \"coasted\": %zu, \"bad_value_lines\": %zu, \"bad_t_lines\": %zu, \"bad_t_overflow_lines\": %zu},\n",
            rp[0].leading_missing, rp[0].coasts, rp[0].bad_value, rp[0].bad_t, rp[0].bad_t_overflow);
    fprintf(fp, "  \"time_gaps\": {\"duplicate_t\": %zu, \"backward_t\": %zu, \"steps_with_horizon_gt_1\": %zu},\n",
            rp[0].gap_zero, rp[0].gap_backward, rp[0].multi_horizon);
    fprintf(fp, "  \"readings\": {\"burn_in\": \"steps with L<%u counted from the prior line (L=0) are excluded\", \"quarters\": \"four equal spans of included samples by logical time\", \"ten_step_origins\": \"coasted origins skipped\"},\n", EST_BURN_IN);
    fprintf(fp, "  \"replay\": {\"complete\": %s, \"steps\": %zu, \"expected_steps\": %zu, \"chain_recorded_every_step\": %s, \"raw_file_unchanged\": %s,\n",
            b(replay_ok), rp[0].nsteps, expect_steps, b(chain_ok), b(raw_preserved));
    fprintf(fp, "             \"prefix_invariance\": {\"method\": \"first k lines physically written to a temp file and replayed from scratch\", \"ks\": \"k=100,500,1000\", \"runs\": %zu, \"pass\": %s}},\n", prefix_ran, b(prefix_ok));
    fprintf(fp, "  \"streams\": {\"M0\": {\"file\": "); js(fp, spath[0]);
    fprintf(fp, ", \"sha256\": \"%s\"}, \"M1\": {\"file\": ", shash[0]); js(fp, spath[1]);
    fprintf(fp, ", \"sha256\": \"%s\"}},\n", shash[1]);
    fprintf(fp, "  \"models\": {\n");
    if (replay_ok) { emit_model(fp, 0, p.q[0], p.r[0], p.ll[0], NULL, &st[0], 0); emit_model(fp, 1, p.q[1], p.r[1], p.ll[1], NULL, &st[1], 1); }
    fprintf(fp, "  },\n  \"selected_model\": %s%s%s,\n", sel >= 0 ? "\"M" : "null", sel >= 0 ? (sel ? "1" : "0") : "", sel >= 0 ? "\"" : "");
    fprintf(fp, "  \"failure_class_to_be_recorded_by_a_human_on_FAIL\": null,\n");
    fprintf(fp, "  \"ESTIMATION_REAL_SIGNAL\": \"%s\",\n  \"ESTIMATION_CALIBRATION\": \"%s\"\n}\n", real_signal ? "PASS" : "FAIL", calib ? "PASS" : "FAIL");
    if (fclose(fp)) return 1;
    char rh[65]; est_sha_file_hex(out, rh);
    printf("ESTIMATION_REAL_SIGNAL=%s ESTIMATION_CALIBRATION=%s receipt_sha256=%s\n", real_signal ? "PASS" : "FAIL", calib ? "PASS" : "FAIL", rh);
    est_replay_free(&rp[0]); est_replay_free(&rp[1]); est_file_free(&f);
    return 0;
}
