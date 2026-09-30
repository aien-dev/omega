/* EST-3 evaluation (protocol v1 sections 4 and 5).
 *   est_eval --params P --raw machine-state.ndjson --marks machine-state-marks.txt
 *            --tool-commit SHA --out receipt.json [--recorded] [--synthetic-test]
 * Takes the parameter file and the held-out file only; never fits. Refuses when
 * the parameter file was not fit on run A (unless --synthetic-test), and
 * refuses run B unless --recorded. Writes a JSON receipt. */
#include <math.h>
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
    double logscore, rmse, persist_rmse, zero_innov_frac;
    int ok_cov[3], ok_nis, ok_bias, ok_lag1, ok_q[4], ok_regime[2], ok_ten, calibrated;
    int rmse_le_persist;
} model_stats;

typedef struct { int64_t b[512], e[512]; size_t n; } marks;

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
    int64_t open_t = 0; int is_open = 0;
    for (size_t i = 0; i < f.nlines; i++) {
        const char *ln = (const char *)f.data + f.off[i]; size_t ll = f.llen[i];
        int64_t t; size_t used;
        if (est_parse_time_prefix(ln, ll, &t, &used)) continue;
        const char *rest = ln + used; size_t rl = ll - used;
        if (rl >= 7 && memcmp(rest, " begin ", 7) == 0) { open_t = t; is_open = 1; }
        else if (rl >= 5 && memcmp(rest, " end ", 5) == 0 && is_open) {
            if (mk->n < 512) { mk->b[mk->n] = open_t; mk->e[mk->n] = t; mk->n++; }
            is_open = 0;
        }
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

static void compute(const est_replay *rp, const marks *mk, model_stats *st)
{
    memset(st, 0, sizeof *st);
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
            nu[n] = s->nu; w[n] = s->nu / sd; n++;
            if (a <= ZC50 * sd) hit[0]++;
            if (a <= ZC80 * sd) hit[1]++;
            int h95 = a <= ZC95 * sd;
            if (h95) hit[2]++;
            snis += s->nis;
            sls += -0.5 * (log(TWO_PI * s->S) + s->nis);
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
    st->n = n;
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
        if (s->L < EST_BURN_IN) continue;
        int64_t target = s->t_ns + (int64_t)TEN_STEP * 1000000000ll;
        const est_step *tg = NULL;
        for (size_t j = i + 1; j < rp->nsteps && rp->steps[j].t_ns <= target; j++)
            if (rp->steps[j].t_ns == target && !rp->steps[j].coast) { tg = &rp->steps[j]; break; }
        if (!tg) continue;
        est_prediction p;
        if (est_kf_predict(&rp->model, &s->post, NULL, TEN_STEP, &p) != EST_OK) continue;
        tn++;
        if (fabs(tg->z - p.y_mean[0]) <= ZC95 * sqrt(p.S[0])) th++;
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
    free(w); free(nu);
}

static const char *b(int v) { return v ? "true" : "false"; }
static void jd(FILE *fp, const char *k, double v)
{
    if (isfinite(v)) fprintf(fp, "\"%s\": %.17g", k, v); else fprintf(fp, "\"%s\": null", k);
}

static void emit_model(FILE *fp, int m, const est_params *p, const model_stats *s, int failed_prefix)
{
    fprintf(fp, "    \"M%d\": {\n      \"q\": %.17g, \"r\": %.17g, \"fit_loglik\": %.17g,\n", m, p->q[m], p->r[m], p->ll[m]);
    fprintf(fp, "      \"samples\": %zu,\n", s->n);
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
#undef FC
    fprintf(fp, "]\n    }%s\n", m == 0 ? "," : "");
    (void)failed_prefix;
}

int main(int argc, char **argv)
{
    const char *params = NULL, *raw = NULL, *mkp = NULL, *commit = NULL, *out = NULL;
    int recorded = 0, synth = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--params") && i + 1 < argc) params = argv[++i];
        else if (!strcmp(argv[i], "--raw") && i + 1 < argc) raw = argv[++i];
        else if (!strcmp(argv[i], "--marks") && i + 1 < argc) mkp = argv[++i];
        else if (!strcmp(argv[i], "--tool-commit") && i + 1 < argc) commit = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--recorded")) recorded = 1;
        else if (!strcmp(argv[i], "--synthetic-test")) synth = 1;
        else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }
    if (!params || !raw || !mkp || !commit || !out) {
        fprintf(stderr, "usage: est_eval --params P --raw F --marks M --tool-commit SHA --out R [--recorded] [--synthetic-test]\n");
        return 2;
    }
    if (recorded && synth) { fprintf(stderr, "refuse: --recorded and --synthetic-test are exclusive\n"); return 2; }
    if (strstr(raw, EST_RUN_B_ID) && !recorded) {
        fprintf(stderr, "refuse: run B may only be evaluated with --recorded (protocol section 6)\n"); return 2;
    }
    est_params p;
    if (est_params_read(params, &p)) { fprintf(stderr, "refuse: unreadable parameter file\n"); return 1; }
    int fit_is_a = strstr(p.fit_path, EST_RUN_A_ID) != NULL && strcmp(p.fit_sha, EST_RUN_A_SHA) == 0;
    if (!fit_is_a && !synth) {
        fprintf(stderr, "refuse: parameter file was not fit on run A (%s)\n", p.fit_path); return 1;
    }
    if (fit_is_a && synth && strstr(raw, EST_RUN_B_ID)) return 2;
    char err[300], phex[65];
    est_file f;
    if (est_file_load(raw, &f, err, sizeof err)) { fprintf(stderr, "refuse: %s\n", err); return 1; }
    marks mk;
    if (load_marks(mkp, &mk, err, sizeof err)) { fprintf(stderr, "refuse: %s\n", err); return 1; }
    if (est_sha_file_hex(params, phex)) return 1;

    static est_replay rp[2], pre;
    model_stats st[2];
    int replay_ok = 1, chain_ok = 1, prefix_ok = 1;
    size_t ks[3] = { 100, 500, 1000 };
    char prefix_report[256] = "";
    size_t expect_steps = 0;
    for (int m = 0; m < 2; m++) {
        if (est_replay_run(&f, (size_t)-1, m, p.q[m], p.r[m], &rp[m])) { replay_ok = 0; fprintf(stderr, "replay M%d: %s\n", m, rp[m].errmsg); continue; }
        expect_steps = f.nlines - rp[m].leading_missing;
        if (rp[m].nsteps != expect_steps) replay_ok = 0;
        for (size_t i = 0; i < rp[m].nsteps; i++) if (!rp[m].steps[i].chain_ok) chain_ok = 0;
        compute(&rp[m], &mk, &st[m]);
        for (int ki = 0; ki < 3; ki++) {
            size_t k = ks[ki];
            if (k > f.nlines) { prefix_ok = 0; continue; }
            if (est_replay_run(&f, k, m, p.q[m], p.r[m], &pre)) { prefix_ok = 0; continue; }
            size_t nn = pre.nsteps;
            if (nn == 0 || nn > rp[m].nsteps) prefix_ok = 0;
            else for (size_t i = 0; i < nn; i++) if (!step_equal(&pre.steps[i], &rp[m].steps[i])) { prefix_ok = 0; break; }
        }
    }
    /* raw preserved: the file on disk still hashes to the verified digest */
    char after[65]; int raw_preserved = !est_sha_file_hex(raw, after) && strcmp(after, f.sha_hex) == 0;
    if (!replay_ok) { fprintf(stderr, "replay did not complete\n"); }
    snprintf(prefix_report, sizeof prefix_report, "k=100,500,1000");

    /* selection: simplest calibrated model within 0.01 nats of the best calibrated */
    int sel = -1; double best = -INFINITY;
    if (replay_ok) {
        for (int m = 0; m < 2; m++) if (st[m].calibrated && st[m].logscore > best) best = st[m].logscore;
        for (int m = 0; m < 2 && sel < 0; m++) if (st[m].calibrated && st[m].logscore >= best - 0.01) sel = m;
    }
    int real_signal = replay_ok && prefix_ok && raw_preserved && chain_ok;
    int calib = 0;
    if (replay_ok) for (int m = 0; m < 2; m++) if (st[m].calibrated && st[m].rmse_le_persist) calib = 1;

    FILE *fp = fopen(out, "wb");
    if (!fp) { fprintf(stderr, "cannot write %s\n", out); return 1; }
    fprintf(fp, "{\n  \"receipt\": \"est_eval_v1\",\n  \"protocol\": \"EST23_PROTOCOL_V1\",\n  \"protocol_commit\": \"%s\",\n", EST_PROTOCOL_COMMIT);
    fprintf(fp, "  \"tool_commit\": \"%s\",\n  \"recorded\": %s,\n  \"synthetic_test\": %s,\n", commit, b(recorded), b(synth));
    fprintf(fp, "  \"parameter_file\": \"%s\",\n  \"parameter_file_sha256\": \"%s\",\n", params, phex);
    fprintf(fp, "  \"fit_run_path\": \"%s\",\n  \"fit_file_sha256\": \"%s\",\n  \"fit_file_is_run_A\": %s,\n", p.fit_path, p.fit_sha, b(fit_is_a));
    fprintf(fp, "  \"input_file\": \"%s\",\n  \"input_file_sha256\": \"%s\",\n  \"input_sha_matches_SHA256SUMS\": true,\n", raw, f.sha_hex);
    fprintf(fp, "  \"lines\": %zu,\n  \"marks_pairs\": %zu,\n", f.nlines, mk.n);
    fprintf(fp, "  \"missing_observations\": {\"leading_before_first_valid\": %zu, \"coasted\": %zu, \"bad_value_lines\": %zu, \"bad_t_lines\": %zu},\n",
            rp[0].leading_missing, rp[0].coasts, rp[0].bad_value, rp[0].bad_t);
    fprintf(fp, "  \"replay\": {\"complete\": %s, \"steps\": %zu, \"expected_steps\": %zu, \"chain_recorded_every_step\": %s, \"raw_file_unchanged\": %s,\n",
            b(replay_ok), rp[0].nsteps, expect_steps, b(chain_ok), b(raw_preserved));
    fprintf(fp, "             \"prefix_invariance\": {\"ks\": \"%s\", \"pass\": %s}},\n", prefix_report, b(prefix_ok));
    fprintf(fp, "  \"models\": {\n");
    if (replay_ok) { emit_model(fp, 0, &p, &st[0], 0); emit_model(fp, 1, &p, &st[1], 0); }
    fprintf(fp, "  },\n  \"selected_model\": %s%s%s,\n", sel >= 0 ? "\"M" : "null", sel >= 0 ? (sel ? "1" : "0") : "", sel >= 0 ? "\"" : "");
    fprintf(fp, "  \"ESTIMATION_REAL_SIGNAL\": \"%s\",\n  \"ESTIMATION_CALIBRATION\": \"%s\"\n}\n", real_signal ? "PASS" : "FAIL", calib ? "PASS" : "FAIL");
    if (fclose(fp)) return 1;
    char rh[65]; est_sha_file_hex(out, rh);
    printf("ESTIMATION_REAL_SIGNAL=%s ESTIMATION_CALIBRATION=%s receipt_sha256=%s\n", real_signal ? "PASS" : "FAIL", calib ? "PASS" : "FAIL", rh);
    est_replay_free(&rp[0]); est_replay_free(&rp[1]); est_replay_free(&pre); est_file_free(&f);
    return 0;
}
