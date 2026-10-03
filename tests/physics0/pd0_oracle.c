/* pd0-oracle: test-only positive control and threshold calibration
 * (spec 4.2 V1-V3, section 11 steps 2 and 3). For each level and each
 * development seed it reports, against the frozen section 6 bounds:
 *   oracle_exact  the true map's coefficients written as a PDLAW1 relation
 *                 (what a perfect learner would emit);
 *   oracle_fit    the generator FORM with constants re-fitted by least squares
 *                 on gathered FIT data (the spec's oracle solver);
 *   sparse_ref    the reference sparse solver (pd0_sparse.c): greedy forward
 *                 selection over the degree <= 3 monomial library, size chosen
 *                 by description_bits + residual bits on SELECT.
 * Also V3: the breach rate under a uniform random schedule, seeds 1..20.
 * Output: one JSON document per level (argv[2] directory) and PASS/FAIL lines.
 * Build with PD0_MUTANT_SIGN in pd0_gen.c to check that a wrong generator sign
 * makes the L1 oracle check FAIL (the reference law is the spec's). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pd0_calib.h"
#include "pd0_sparse.h"


#define SEEDS 5

static const double BOUND_IN[PD0_LEVELS] = {0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.03, 0};
static const double BOUND_EX[PD0_LEVELS] = {0.06, 0.06, 0.06, 0.06, 0.06, 0.06, 0.08, 0};
static const double BOUND_ONE[PD0_LEVELS] = {0.01, 0.01, 0.01, 0.01, 0.01, 0.01, -1, 0};
static const int64_t COEF_PPM[PD0_LEVELS] = {50000, 50000, 50000, 50000, 50000, 100000, -1, 0};

/* L6 latent estimator. The oracle knows the generator FORM (spec 4.1):
 *   ds1 = (-k s0 + m h + u) dt,   dh = (-w h + q s0) dt,   h = 0 at every reset.
 * The scale of h is not identifiable, so h is fixed to the q = 1 convention
 * (h_hat = h / q) and the product m*q is what is fitted. Given a = 1 - w dt the
 * latent trajectory is a linear recursion of observed s0 alone,
 *   g' = a g + dt s0, g = 0 at the first step of every episode,
 * and (ds1 - u dt) = (-k dt) s0 + (m q dt) g is linear in (k, m q). So: for each
 * candidate w solve a 2-parameter least squares, keep the w with least squared
 * residual (coarse grid over a wide w range, then golden-section refinement).
 * Exact and deterministic; no alternation needed. FIT/SELECT data only. */
typedef struct { double k, mq, w, sse; } l6_est;

static double l6_eval(const pd0_gather *g, double w, double dt, double *k_out, double *mq_out) {
    int n = g->n;
    double a = 1.0 - w * dt, hh = 0, sxx = 0, sxg = 0, sgg = 0, sxy = 0, sgy = 0, syy = 0;
    uint32_t ep = 0;
    for (int i = 0; i < n; i++) {
        const pd0_transition *t = &g->t[i];
        if (i == 0 || t->episode != ep) { hh = 0; ep = t->episode; }
        double s0 = (double)t->before[0] / 1e6;
        double y = (double)(t->after[1] - t->before[1]) / 1e6 - dt * ((double)t->u[0] / 1e6);
        sxx += s0 * s0; sxg += s0 * hh; sgg += hh * hh; sxy += s0 * y; sgy += hh * y; syy += y * y;
        hh = a * hh + dt * s0;
    }
    double det = sxx * sgg - sxg * sxg;
    if (fabs(det) < 1e-18) return -1;
    double bx = (sxy * sgg - sgy * sxg) / det, bg = (sgy * sxx - sxy * sxg) / det;
    if (k_out) *k_out = -bx / dt;
    if (mq_out) *mq_out = bg / dt;
    return syy - 2 * (bx * sxy + bg * sgy) + bx * bx * sxx + 2 * bx * bg * sxg + bg * bg * sgg;
}

static int l6_estimate(const pd0_gather *g, double dt, l6_est *e) {
    double best = -1, bw = 0;
    for (double w = 0.2; w <= 4.0001; w += 0.05) {
        double s = l6_eval(g, w, dt, NULL, NULL);
        if (s >= 0 && (best < 0 || s < best)) { best = s; bw = w; }
    }
    if (best < 0) return -1;
    double lo = bw - 0.05, hi = bw + 0.05, gr = 0.6180339887498949;
    for (int it = 0; it < 60; it++) {
        double c = hi - gr * (hi - lo), d = lo + gr * (hi - lo);
        if (l6_eval(g, c, dt, NULL, NULL) < l6_eval(g, d, dt, NULL, NULL)) hi = d; else lo = c;
    }
    e->w = 0.5 * (lo + hi);
    e->sse = l6_eval(g, e->w, dt, &e->k, &e->mq);
    return e->sse < 0 ? -1 : 0;
}

/* per-coefficient errors in ppm vs truth (k, w directly; m*q as the identifiable product) */
static l6_est g_l6_est;
static int64_t g_l6_err[3];
static int g_l6_in_range;
static int64_t l6_ppm(double est, double tru) { return (int64_t)llround(fabs(est - tru) / fabs(tru) * 1e6); }

static int oracle_fit_l6(const pd0_relation *truth, const pd0_gather *g, pd0_relation *out) {
    *out = *truth;
    double dt = (double)truth->eq[0].t[0].coef / 1e6, ktr = -(double)truth->eq[1].t[0].coef / 1e6 / dt;
    double m = (double)truth->eq[1].t[1].coef / 1e6 / dt, w_tr = -(double)truth->eq[2].t[0].coef / 1e6 / dt;
    double q = (double)truth->eq[2].t[1].coef / 1e6 / dt;
    l6_est e;
    if (l6_estimate(g, dt, &e)) return -1;
    g_l6_est = e;
    g_l6_err[0] = l6_ppm(e.k, ktr); g_l6_err[1] = l6_ppm(e.w, w_tr); g_l6_err[2] = l6_ppm(e.mq, m * q);
    /* the form's declared constant ranges (spec 4.1): k [2,5], w [1,2], m*q [0.25,1] */
    g_l6_in_range = e.k >= 2.0 && e.k <= 5.0 && e.w >= 1.0 && e.w <= 2.0 && e.mq >= 0.25 && e.mq <= 1.0;
    out->eq[1].t[0].coef = -(int64_t)llround(e.k * dt * 1e6);
    out->eq[1].t[1].coef = (int64_t)llround(e.mq * dt * 1e6);   /* h scale: q = 1 */
    out->eq[2].t[0].coef = -(int64_t)llround(e.w * dt * 1e6);
    out->eq[2].t[1].coef = (int64_t)llround(dt * 1e6);
    return 0;
}

/* fit only the constants of the true form: per equation, least squares of
 * D target on the true monomials over FIT transitions (observed vars only) */
static int oracle_fit(const pd0_relation *truth, const pd0_gather *g, pd0_relation *out) {
    *out = *truth;
    if (truth->n_latent) return oracle_fit_l6(truth, g, out);
    for (int e = 0; e < truth->n_eq; e++) {
        const pd0_eq *te = &truth->eq[e];
        int p = te->n_terms, n = g->n;
        double *X = malloc(sizeof(double) * (size_t)n * (size_t)p), *y = malloc(sizeof(double) * (size_t)n), b[PD0_MAX_TERMS];
        for (int k = 0; k < n; k++) {
            int64_t vars[PD0_MAX_VARS] = {0};
            for (int v = 0; v < truth->n_vars; v++) vars[v] = g->t[k].before[v];
            for (int t = 0; t < p; t++)
                X[k * p + t] = (double)pd0_monomial(&te->t[t], vars, g->t[k].u, truth->n_vars, truth->n_channels) / 1e6;
            y[k] = (double)(g->t[k].after[te->target] - g->t[k].before[te->target]) / 1e6;
        }
        int rc = pd0_lstsq(X, y, n, p, b);
        free(X); free(y);
        if (rc) return -1;
        for (int t = 0; t < p; t++) out->eq[e].t[t].coef = (int64_t)llround(b[t] * 1e6);
    }
    return 0;
}

static const char *pf(int ok) { return ok ? "PASS" : "FAIL"; }

static void emit_result(FILE *f, const char *name, const pd0_calib_result *r, const pd0_relation *rel,
                        const pd0_relation *truth, int level, int *all_ok) {
    int64_t ppm = pd0_relation_coef_err_ppm(rel, truth);
    int ok_in = r->nrmse_inbox <= BOUND_IN[level], ok_ex = r->nrmse_extrap <= BOUND_EX[level];
    int ok_one = BOUND_ONE[level] < 0 || r->nrmse_onestep <= BOUND_ONE[level];
    int ok_coef = COEF_PPM[level] < 0 || (ppm >= 0 && ppm <= COEF_PPM[level]);
    int ok_size = pd0_relation_size(rel) <= pd0_gen_true_size(level) + 2;
    int ok_support = pd0_relation_supports(rel, truth);
    int is_l6fit = level == PD0_L6 && strcmp(name, "oracle_fit") == 0;
    int ok_range = !is_l6fit || g_l6_in_range;
    int ok = ok_in && ok_ex && ok_one && ok_coef && ok_size && ok_support && ok_range;
    if (all_ok) *all_ok &= ok;
    fprintf(f, "      \"%s\": {\"nrmse_inbox\": %.6f, \"nrmse_extrap\": %.6f, \"nrmse_onestep\": %.6f, "
               "\"truth_oob_episodes\": %d, \"coef_err_ppm\": %lld, \"size\": %d, \"description_bits\": %u, "
               "\"support\": %s, \"checks\": {\"inbox\": \"%s\", \"extrap\": \"%s\", \"onestep\": \"%s\", "
               "\"coef\": \"%s\", \"size\": \"%s\", \"support\": \"%s\"}, \"verdict\": \"%s\"}",
            name, r->nrmse_inbox, r->nrmse_extrap, r->nrmse_onestep, r->truth_oob_episodes, (long long)ppm,
            pd0_relation_size(rel), pd0_relation_description_bits(rel), ok_support ? "true" : "false", pf(ok_in),
            pf(ok_ex), BOUND_ONE[level] < 0 ? "NOT_SCORED" : pf(ok_one), COEF_PPM[level] < 0 ? "NOT_SCORED" : pf(ok_coef),
            pf(ok_size), pf(ok_support), pf(ok));
    if (is_l6fit)
        fprintf(f, ", \n        \"oracle_fit_l6_estimate\": {\"k\": %.6f, \"w\": %.6f, \"m_times_q\": %.6f, \"h_scale\": \"q=1 convention\", \"fit_sse\": %.3e, "
                   "\"err_ppm\": {\"k\": %lld, \"w\": %lld, \"m_times_q\": %lld}, \"in_declared_ranges\": \"%s\"}",
                g_l6_est.k, g_l6_est.w, g_l6_est.mq, g_l6_est.sse, (long long)g_l6_err[0], (long long)g_l6_err[1], (long long)g_l6_err[2], pf(ok_range));
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: pd0-oracle <level 0..6|all> <outdir> [mutant-tag]\n"); return 2; }
    int lo = 0, hi = PD0_L6;
    if (strcmp(argv[1], "all") != 0) lo = hi = atoi(argv[1]);
    const char *tag = argc > 3 ? argv[3] : "";
    int overall = 1;
    for (int level = lo; level <= hi; level++) {
        char path[512];
        snprintf(path, sizeof path, "%s/pd0-oracle-%s%s.json", argv[2], pd0_gen_level_name(level), tag);
        FILE *f = fopen(path, "w");
        if (!f) { perror(path); return 2; }
        int v1_exact = 1, v1_fit = 1, v3 = 1;
        double worst_breach = 0;
        fprintf(f, "{\n  \"receipt\": \"PD0_G1\",\n  \"level\": \"%s\",\n  \"spec\": \"aien-dev/physics docs/PD0_HIDDEN_EQUATION_BENCHMARK.md @ 2f881b1 (rev 2)\",\n"
                   "  \"recorder\": \"%s\",\n  \"range_guard\": \"%s\",\n  \"bounds\": {\"inbox\": %.3f, \"extrap\": %.3f, \"onestep\": %.3f, \"coef_ppm\": %lld, \"size\": %d},\n"
                   "  \"instances\": [\n",
                pd0_gen_level_name(level), "STAND_IN", "STAND_IN", BOUND_IN[level], BOUND_EX[level], BOUND_ONE[level],
                (long long)COEF_PPM[level], pd0_gen_true_size(level) + 2);
        for (int si = 0; si < SEEDS; si++) {
            uint64_t seed = (uint64_t)(si + 1);
            pd0_gen g; pd0_desc d;
            pd0_gen_init(&g, level, seed);
            pd0_gen_desc(level, &d);
            pd0_relation truth, fit, sparse;
            pd0_gen_true_relation(&g, d.dt_micro, &truth);
            pd0_gather gather;
            pd0_calib_gather(level, seed, &gather);
            pd0_calib_result r_exact, r_fit, r_sparse;
            pd0_calib_heldout(&g, &d, seed, &truth, &r_exact);
            int have_fit = oracle_fit(&truth, &gather, &fit) == 0;
            if (have_fit) pd0_calib_heldout(&g, &d, seed, &fit, &r_fit);
            int have_sparse = pd0_sparse_fit(&gather, d.n_obs, d.n_channels, &sparse) == 0;
            if (have_sparse) pd0_calib_heldout(&g, &d, seed, &sparse, &r_sparse);
            fprintf(f, "    {\"seed\": %llu, \"constants_micro\": [%lld, %lld, %lld, %lld], \"fit_transitions\": %d, \"fit_episodes\": %d,\n",
                    (unsigned long long)seed, (long long)g.k[0], (long long)g.k[1], (long long)g.k[2], (long long)g.k[3], gather.n, gather.episodes);
            emit_result(f, "oracle_exact", &r_exact, &truth, &truth, level, &v1_exact);
            fprintf(f, ",\n");
            if (have_fit) emit_result(f, "oracle_fit", &r_fit, &fit, &truth, level, &v1_fit);
            else fprintf(f, "      \"oracle_fit\": \"NOT_RUN\"");
            fprintf(f, ",\n");
            if (have_sparse) emit_result(f, "sparse_ref", &r_sparse, &sparse, &truth, level, NULL);
            else fprintf(f, "      \"sparse_ref\": \"NOT_RUN\"");
            fprintf(f, "\n    }%s\n", si + 1 < SEEDS ? "," : "");
            free(gather.t);
        }
        fprintf(f, "  ],\n  \"v3_breach\": [");
        for (uint64_t seed = 1; seed <= 20; seed++) {
            pd0_gather gg;
            pd0_calib_gather(level, seed, &gg);
            double rate = gg.episodes ? (double)gg.oob_episodes / gg.episodes : 0;
            if (rate > worst_breach) worst_breach = rate;
            if (rate > 0.05) v3 = 0;
            fprintf(f, "%s{\"seed\": %llu, \"episodes\": %d, \"oob\": %d, \"rate\": %.4f}", seed > 1 ? ", " : "",
                    (unsigned long long)seed, gg.episodes, gg.oob_episodes, rate);
            free(gg.t);
        }
        fprintf(f, "],\n  \"v3_worst_rate\": %.4f,\n  \"verdicts\": {\"V1_oracle_exact\": \"%s\", \"V1_oracle_fit\": \"%s\", \"V3\": \"%s\"}\n}\n",
                worst_breach, pf(v1_exact), pf(v1_fit), pf(v3));
        fclose(f);
        printf("PHYSICS0_G1_V1_%s%s: %s (oracle_exact %s, oracle_fit %s)\n", pd0_gen_level_name(level), tag,
               pf(v1_exact && v1_fit), pf(v1_exact), pf(v1_fit));
        printf("PHYSICS0_G1_V3_%s%s: %s (worst breach rate %.4f over seeds 1..20)\n", pd0_gen_level_name(level), tag, pf(v3), worst_breach);
        overall &= v1_exact;
    }
    printf("PHYSICS0_ORACLE%s: %s\n", tag, pf(overall));
    return overall ? 0 : 1;
}
