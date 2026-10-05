/* ESTIMATION v4 tool (protocol docs/estimation/protocols/est-v4.md).
 *
 *   est4 fit --raw <D1>/machine-state.ndjson --marks <D1>/machine-state-marks.txt
 *            --out <params.txt> [--synthetic-test [--grid-stride N]]
 *       Protocol section 5: two-stage grid fit of G1, G2, G3 on D1, the v3 F1
 *       grid and the E0 histogram, in-sample screen (v3 section 6 rules,
 *       c3_judge), selection, PHASE_A verdict. Binding mode refuses: a dirty
 *       tree (build time and run time), a compiled protocol SHA that differs
 *       from the doc on disk, no compiled D1 identity, inputs whose SHA-256
 *       differs from it or whose path lacks the D1 tag, an existing output.
 *       Every mode refuses a held-out path (v4 or v3 tag) before opening
 *       anything.
 *   est4 precheck --dir <D>
 *       v3 section 7 pre-check (c3_precheck; never reads thermal values).
 *   est4 recorded --dir <D2> --params <params.txt> --outdir <receipts/est-v4>
 *            [--synthetic-test]
 *       The single scored run: S, F1 and E0 on D2 with D1-frozen parameters.
 *   est4 make-synthetic <dir> [--seed N] [--lines N]
 *       Synthetic run folder (tests only), c3_synth_write.
 */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "est3c_common.h"
#include "est_v4.h"
#include "sha256.h"

#ifndef TOOL_COMMIT
#define TOOL_COMMIT "unknown"
#endif
#ifndef TOOL_DIRTY
#define TOOL_DIRTY 1
#endif
#ifndef E4_PROTOCOL_SHA
#define E4_PROTOCOL_SHA "unknown"
#endif
#ifndef E4_D1_RAW_SHA
#define E4_D1_RAW_SHA "absent"
#endif
#ifndef E4_D1_MARKS_SHA
#define E4_D1_MARKS_SHA "absent"
#endif
#ifndef E4_D2_RAW_SHA
#define E4_D2_RAW_SHA "absent"
#endif
#ifndef E4_D2_MARKS_SHA
#define E4_D2_MARKS_SHA "absent"
#endif
#ifndef E4_D2_SCHED_SHA
#define E4_D2_SCHED_SHA "absent"
#endif
#ifndef E4_PARAMS_SHA
#define E4_PARAMS_SHA "absent"
#endif
#ifndef E4_REPO_ROOT
#define E4_REPO_ROOT "unknown"
#endif

#define E4_PROTOCOL_DOC "docs/estimation/protocols/est-v4.md"
#define E4_RECEIPT_SUBDIR "docs/estimation/receipts/est-v4"
#define E4_FIT_TAG "-est4-fit-"
#define E4_HELDOUT_TAG "-est4-heldout-"
#define E4_AGREE 1e-6   /* fast vs pmf mean log score, nats/step */

static const char *envor(const char *name, const char *dflt)
{
#ifdef E4_TEST_BUILD
    const char *e = getenv(name); if (e) return e;
#else
    (void)name;
#endif
    return dflt;
}

static int tool_dirty(void)
{
    static int cached = -1;
#ifdef E4_TEST_BUILD
    const char *e = getenv("E4_TEST_DIRTY"); if (e && *e) return atoi(e);
#endif
    if (cached < 0) cached = TOOL_DIRTY ? TOOL_DIRTY : c3_tree_dirty_now();
    return cached;
}

/* held-out: the v4 tag or the v3 tag, on the path as given or resolved */
static int e4_heldout(const char *path)
{
    char rp[PATH_MAX];
    if (strstr(path, E4_HELDOUT_TAG) || strstr(path, C3_HELDOUT_TAG)) return 1;
    if (realpath(path, rp) && (strstr(rp, E4_HELDOUT_TAG) || strstr(rp, C3_HELDOUT_TAG))) return 1;
    return 0;
}

/* ------------------------------------------------------------ grids (s5) */
static const double G1_TAU[] = { 0.5, 1, 2, 4, 10, 30 };
static const double G2_RHO[] = { 0, 0.2, 0.4, 0.6, 0.8, 0.9 };
static const double G3_TAU[] = { 0.5, 1, 2, 4, 10 };
#define NQ4 9      /* q = 10^(2 + 0.5 i) */
static const double LAM4[] = { 0.1, 0.2, 0.3, 0.5, 0.7, 0.9 };
static const double NU4[] = { 0.8, 1, 1.25, 1.5, 2, 3, 5 };
#define NC4 21     /* c = 0.20 + 0.05 i */
static const double PHI4[] = { 1, 0.95, 0.9, 0.8, 0.6 };
static const double NUH4[] = { 0.5, 0.6, 0.7, 0.8, 1, 1.25, 1.5, 2, 3, 5 };
#define NCH4 31    /* c_h = 0.10 + 0.05 i */
#define N_(a) ((int)(sizeof(a) / sizeof((a)[0])))
static double gq4(int i) { return pow(10.0, 2.0 + 0.5 * (double)i); }
static double gc4(int i) { return (double)(20 + 5 * i) / 100.0; }
static double gch4(int i) { return (double)(10 + 5 * i) / 100.0; }
#define NQ1 25
#define NR1 17
static double gq1(int i) { return pow(10.0, 1.0 + 0.25 * (double)i); }
static double gr1(int j) { return pow(10.0, 2.0 + 0.25 * (double)j); }

static const char *gname(int id) { return id == 1 ? "G1" : id == 2 ? "G2" : id == 3 ? "G3" : "none"; }

/* ------------------------------------------------- tick walk (both paths) */
static est_obs_class e4_classify(const c3_tick *t, est_pobs *ob)
{
    est_assumption a; c3_assumption_base(&a, EST_FAM_STUDENT_T);
    a.nparam = 2; a.param[0] = 5; a.param[1] = 100;
    if (est_pobs_classify(&a, t->value, t->present, &t->ev, ob) != EST_OK) return EST_OBS_CLASSES_;
    return ob->cls;
}

/* Fast-path trace for one dynamic setting (dyn, q, lam): the scale-free parts
 * of every scored one-step and ten-step predictive. */
typedef struct {
    size_t n1, n10, unscorable, cap;
    double *y1, *m1, *b1, *a1;          /* outcome, location, sqrt(g V_1), anchor */
    double *y10, *m10, *v10, *g10, *gl10, *a10;
} e4_trace;

static int trace_alloc(e4_trace *tr, size_t n)
{
    memset(tr, 0, sizeof *tr);
    tr->cap = n ? n : 1;
    double **f[10] = { &tr->y1, &tr->m1, &tr->b1, &tr->a1, &tr->y10, &tr->m10, &tr->v10, &tr->g10, &tr->gl10, &tr->a10 };
    for (int i = 0; i < 10; i++) if (!(*f[i] = malloc(tr->cap * sizeof(double)))) return 1;
    return 0;
}
static void trace_free(e4_trace *tr)
{
    free(tr->y1); free(tr->m1); free(tr->b1); free(tr->a1); free(tr->a10); free(tr->y10); free(tr->m10); free(tr->v10); free(tr->g10); free(tr->gl10);
    memset(tr, 0, sizeof *tr);
}

typedef struct { int ok; double m, v, g, gl, a; } e4_origin;

static int trace_run(const c3_ticks *tk, const est4_params *p, e4_trace *tr)
{
    est4_state s;
    if (est4_init(p, &s) != EST_OK) return 1;
    e4_origin ring[C3_TEN];
    memset(ring, 0, sizeof ring);
    tr->n1 = tr->n10 = tr->unscorable = 0;
    long first = -1;
    for (size_t t = 0; t < tk->n; t++) {
        est_pobs ob;
        est_obs_class cls = e4_classify(&tk->t[t], &ob);
        if (cls == EST_OBS_CLASSES_) return 1;
        int ok = cls == EST_OBS_OK;
        if (ok && first < 0) first = (long)t;
        int scored = first >= 0 && (long)t - first >= (long)C3_BURN_IN;
        e4_origin *o = &ring[t % C3_TEN];
        if (o->ok) {
            o->ok = 0;
            if (ok && tr->n10 < tr->cap) {
                size_t k = tr->n10++;
                tr->y10[k] = ob.value; tr->m10[k] = o->m; tr->v10[k] = o->v; tr->g10[k] = o->g; tr->gl10[k] = o->gl; tr->a10[k] = o->a;
            }
        }
        if (ok && scored) {
            if ((uint64_t)s.gap + 1u > EST_PRED_MAX_H) tr->unscorable++;
            else {
                double m, v;
                if (est4_moments(&s, 1, &m, &v) != EST_OK) return 1;
                if (tr->n1 < tr->cap) { size_t k = tr->n1++; tr->y1[k] = ob.value; tr->m1[k] = m; tr->b1[k] = sqrt(s.g * v); tr->a1[k] = s.anchor; }
            }
        }
        if (est4_update(&s, ok, ok ? ob.value : NAN) != EST_OK) return 1;
        if (ok && scored) {
            double m, v;
            if (est4_moments(&s, C3_TEN, &m, &v) != EST_OK) return 1;
            o->ok = 1; o->m = m; o->v = v; o->g = s.g; o->gl = s.gl; o->a = s.anchor;
        }
    }
    return 0;
}

static double fast_ls1(const e4_trace *tr, double nu, double c)
{
    double sum = 0.0;
    for (size_t i = 0; i < tr->n1; i++) {
        est4_pred pr = { 1u, tr->m1[i], c * tr->b1[i], nu };
        sum += est4_fast_logp(&pr, C3_QUANTUM, tr->a1[i], tr->y1[i], NULL, NULL);
    }
    return tr->n1 ? sum / (double)tr->n1 : -INFINITY;
}

static double fast_ls10(const e4_trace *tr, double phi, double nuh, double ch)
{
    double sum = 0.0;
    for (size_t i = 0; i < tr->n10; i++) {
        double gb = est4_gbar(tr->g10[i], tr->gl10[i], phi, C3_TEN);
        est4_pred pr = { C3_TEN, tr->m10[i], ch * sqrt(gb * tr->v10[i]), nuh };
        sum += est4_fast_logp(&pr, C3_QUANTUM, tr->a10[i], tr->y10[i], NULL, NULL);
    }
    return tr->n10 ? sum / (double)tr->n10 : -INFINITY;
}

/* Full path: discrete pmf predictives scored by est_dpred_score, statistics
 * by c3_judge (v3 section 6), plus the ten-step mean log score. Mirrors
 * c3_score tick for tick. */
static int e4_score(const c3_ticks *tk, const c3_marks *mk, const est4_params *p, c3_stats *st, double *ls10)
{
    memset(st, 0, sizeof *st);
    est_calib_init(&st->c);
    st->first_fail = -1;
    st->cap = tk->n ? tk->n : 1;
    st->cov95_step = malloc(st->cap * sizeof(double));
    st->width_step = malloc(st->cap * sizeof(double));
    st->regime_step = malloc(st->cap);
    est_dpred *ring = malloc(C3_TEN * sizeof *ring), *dp = malloc(sizeof *dp);
    int ring_ok[C3_TEN];
    memset(ring_ok, 0, sizeof ring_ok);
    double l10 = 0.0;
    int rc = 0;
    est4_state s;
    if (!st->cov95_step || !st->width_step || !st->regime_step || !ring || !dp) { rc = 1; snprintf(st->errmsg, sizeof st->errmsg, "out of memory"); goto done; }
    if (est4_init(p, &s) != EST_OK) { rc = 1; snprintf(st->errmsg, sizeof st->errmsg, "params"); goto done; }
    long first = -1;
    for (size_t t = 0; t < tk->n && !rc; t++) {
        const c3_tick *tc = &tk->t[t];
        est_pobs ob; est_pinnov iv; est_status r;
        est_obs_class cls = e4_classify(tc, &ob);
        if (cls == EST_OBS_CLASSES_) { rc = 1; break; }
        int ok = cls == EST_OBS_OK;
        if (ok && first < 0) first = (long)t;
        int scored = first >= 0 && (long)t - first >= (long)C3_BURN_IN;
        if (ring_ok[t % C3_TEN]) {
            ring_ok[t % C3_TEN] = 0;
            if (ok) {
                if ((r = est_dpred_score(&ring[t % C3_TEN], &ob, &iv)) != EST_OK) { rc = 1; break; }
                if (iv.valid) { st->ten_cov += est_frac_cover(iv.F_lo, iv.F_hi, 0.95); st->ten_n++; l10 += iv.logp; }
            }
        }
        if (ok && scored) {
            if ((uint64_t)s.gap + 1u > EST_PRED_MAX_H) st->unscorable++;
            else {
                est4_pred pr;
                if (est4_predict(&s, 1, &pr) != EST_OK || est4_dpred(&s, &pr, dp) != EST_OK) { rc = 1; break; }
                if ((r = est_dpred_score(dp, &ob, &iv)) != EST_OK) { rc = 1; break; }
                if (iv.valid) {
                    st->logsum += iv.logp; st->n++;
                    if (est_calib_add(&st->c, dp, &iv) != EST_OK) { rc = 1; break; }
                    size_t k = st->n - 1;
                    st->cov95_step[k] = est_frac_cover(iv.F_lo, iv.F_hi, 0.95);
                    double w = NAN;
                    if (est_dpred_width80(dp, &w) != EST_OK) { rc = 1; break; }
                    st->width_step[k] = w;
                    st->regime_step[k] = (unsigned char)c3_in_trial(mk, tc->wall_ns);
                }
            }
        }
        if (est4_update(&s, ok, ok ? ob.value : NAN) != EST_OK) { rc = 1; break; }
        if (ok && scored) {
            est4_pred pr;
            if (est4_predict(&s, C3_TEN, &pr) != EST_OK || est4_dpred(&s, &pr, &ring[t % C3_TEN]) != EST_OK) { rc = 1; break; }
            ring_ok[t % C3_TEN] = 1;
        }
    }
    if (rc && !st->errmsg[0]) snprintf(st->errmsg, sizeof st->errmsg, "v4 scoring error");
    st->error = rc;
    st->logscore = st->n ? st->logsum / (double)st->n : -INFINITY;
    *ls10 = st->ten_n ? l10 / (double)st->ten_n : -INFINITY;
    if (!rc) c3_judge(st);
done:
    free(ring); free(dp);
    return rc;
}

/* ------------------------------------------------------------- the fit */
typedef struct {
    int have;
    est4_params p;
    double fast1, fast10, full1, full10;
    size_t points1, points2, n1, n10, skipped;   /* skipped: (dyn, q, lam) points whose filter failed */
    int edge[8];     /* chosen value on a grid edge: dyn q lam nu c phi nu_h c_h */
} g_fit;

static int edge_of(double v, const double *g, int n) { return v == g[0] || v == g[n - 1]; }

static int fit_family(const c3_ticks *tk, int id, int stride, g_fit *gf)
{
    const double *dyn = id == 1 ? G1_TAU : id == 2 ? G2_RHO : G3_TAU;
    int nd = id == 1 ? N_(G1_TAU) : id == 2 ? N_(G2_RHO) : N_(G3_TAU);
    e4_trace tr;
    if (trace_alloc(&tr, tk->n)) { trace_free(&tr); return 1; }
    memset(gf, 0, sizeof *gf);
    double best = -INFINITY;
    est4_params p;
    memset(&p, 0, sizeof p);
    p.family = (est4_family)id; p.quantum = C3_QUANTUM;
    p.phi = 1.0; p.nu_h = 1.0; p.c_h = 1.0; p.nu = 1.0; p.c = 1.0;
    /* stage 1: one-step score over dyn, q, lam, nu, c */
    for (int a = 0; a < nd; a += stride)
        for (int b = 0; b < NQ4; b += stride)
            for (int l = 0; l < N_(LAM4); l += stride) {
                p.dyn = dyn[a]; p.q = gq4(b); p.lam = LAM4[l];
                if (trace_run(tk, &p, &tr)) { gf->skipped++; continue; }
                for (int u = 0; u < N_(NU4); u += stride)
                    for (int c = 0; c < NC4; c += stride) {
                        double ls = fast_ls1(&tr, NU4[u], gc4(c));
                        gf->points1++;
                        if (isfinite(ls) && c3_grid_take(gf->have, ls, best)) {
                            gf->have = 1; best = ls;
                            gf->p = p; gf->p.nu = NU4[u]; gf->p.c = gc4(c);
                        }
                    }
            }
    if (!gf->have) { trace_free(&tr); return 0; }
    gf->fast1 = best;
    /* stage 2: ten-step score over phi, nu_h, c_h with stage 1 frozen */
    if (trace_run(tk, &gf->p, &tr)) { trace_free(&tr); gf->have = 0; return 0; }
    int have2 = 0; double best2 = -INFINITY;
    for (int a = 0; a < N_(PHI4); a += stride)
        for (int u = 0; u < N_(NUH4); u += stride)
            for (int c = 0; c < NCH4; c += stride) {
                double ls = fast_ls10(&tr, PHI4[a], NUH4[u], gch4(c));
                gf->points2++;
                if (isfinite(ls) && c3_grid_take(have2, ls, best2)) {
                    have2 = 1; best2 = ls;
                    gf->p.phi = PHI4[a]; gf->p.nu_h = NUH4[u]; gf->p.c_h = gch4(c);
                }
            }
    if (!have2) { trace_free(&tr); gf->have = 0; return 0; }
    gf->fast10 = best2;
    gf->n1 = tr.n1; gf->n10 = tr.n10;
    double cg[NC4], chg[NCH4], qg[NQ4];
    for (int i = 0; i < NC4; i++) cg[i] = gc4(i);
    for (int i = 0; i < NCH4; i++) chg[i] = gch4(i);
    for (int i = 0; i < NQ4; i++) qg[i] = gq4(i);
    gf->edge[0] = edge_of(gf->p.dyn, dyn, nd); gf->edge[1] = edge_of(gf->p.q, qg, NQ4);
    gf->edge[2] = edge_of(gf->p.lam, LAM4, N_(LAM4)); gf->edge[3] = edge_of(gf->p.nu, NU4, N_(NU4));
    gf->edge[4] = edge_of(gf->p.c, cg, NC4); gf->edge[5] = edge_of(gf->p.phi, PHI4, N_(PHI4));
    gf->edge[6] = edge_of(gf->p.nu_h, NUH4, N_(NUH4)); gf->edge[7] = edge_of(gf->p.c_h, chg, NCH4);
    trace_free(&tr);
    return 0;
}

static void put_screen(FILE *fp, const char *name, const c3_stats *st, double ls10)
{
    for (int i = 0; i < C3_ST_COUNT; i++)
        fprintf(fp, "screen %s %s value %.17g band %.17g %.17g n %zu gated %d pass %d\n", name, c3_stat_name(i),
                st->value[i], st->band_lo[i], st->band_hi[i],
                (i >= C3_ST_Q0 && i <= C3_ST_TEN) ? st->sub_n[i] : st->n, st->gated[i], st->pass[i]);
    fprintf(fp, "screen_sharpness %s width80_mean_mc %.17g width80_median_mc %.17g unscorable %zu ten_logscore %.17g\n",
            name, st->width_mean, st->width_median, st->unscorable, ls10);
    fprintf(fp, "screen_result %s pass %d first_fail %s n %zu logscore %.17g\n", name, st->calibrated,
            st->calibrated ? "none" : c3_stat_name(st->first_fail), st->n, st->logscore);
}

static int cmd_fit(int argc, char **argv)
{
    const char *raw = NULL, *mkp = NULL, *out = NULL;
    int synth = 0, stride = 1;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--raw") && i + 1 < argc) raw = argv[++i];
        else if (!strcmp(argv[i], "--marks") && i + 1 < argc) mkp = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--synthetic-test")) synth = 1;
        else if (!strcmp(argv[i], "--grid-stride") && i + 1 < argc) stride = atoi(argv[++i]);
        else { fprintf(stderr, "usage: est4 fit --raw R --marks M --out P [--synthetic-test [--grid-stride N]]\n"); return 2; }
    }
    if (!raw || !mkp || !out) { fprintf(stderr, "est4 fit: --raw, --marks and --out are required\n"); return 2; }
    /* held-out guard first, before any file is opened, in every mode */
    if (e4_heldout(raw) || e4_heldout(mkp) || e4_heldout(out)) { fprintf(stderr, "refuse: a held-out path; fitting on it is not allowed\n"); return 2; }
    if (stride < 1 || (stride != 1 && !synth)) { fprintf(stderr, "refuse: --grid-stride only with --synthetic-test\n"); return 2; }
    const char *proto = envor("E4_TEST_PROTOCOL_SHA", E4_PROTOCOL_SHA);
    const char *d1raw = envor("E4_TEST_D1_RAW_SHA", E4_D1_RAW_SHA), *d1mk = envor("E4_TEST_D1_MARKS_SHA", E4_D1_MARKS_SHA);
    if (!synth) {
        if (tool_dirty()) { fprintf(stderr, "refuse: the binding fit needs a clean tree (dirty=%d)\n", tool_dirty()); return 2; }
        char doc[65] = "";
        if (!strcmp(proto, "unknown") || est_sha_file_hex(E4_PROTOCOL_DOC, doc) || strcmp(doc, proto)) {
            fprintf(stderr, "refuse: compiled protocol SHA %s differs from %s on disk (%s)\n", proto, E4_PROTOCOL_DOC, doc[0] ? doc : "unreadable"); return 2; }
        if (!strcmp(d1raw, "absent") || !strcmp(d1mk, "absent")) { fprintf(stderr, "refuse: no D1 identity compiled in (receipts/est-v4/d1.sha256)\n"); return 2; }
        if (!strstr(raw, E4_FIT_TAG) || !strstr(mkp, E4_FIT_TAG)) { fprintf(stderr, "refuse: the binding fit opens only a %s run\n", E4_FIT_TAG); return 2; }
    }
    char err[512];
    est_file f;
    if (est_file_load(raw, &f, err, sizeof err)) { fprintf(stderr, "refuse: %s\n", err); return 1; }
    static c3_marks mk;
    if (c3_marks_load(mkp, 1, &mk, err, sizeof err)) { fprintf(stderr, "refuse: marks: %s\n", err); est_file_free(&f); return 1; }
    if (!synth && (strcmp(f.sha_hex, d1raw) || strcmp(mk.sha, d1mk))) {
        fprintf(stderr, "refuse: input SHA-256 differs from the compiled D1 identity\n"); est_file_free(&f); return 2; }
    FILE *fp = fopen(out, "wbx");
    if (!fp) { fprintf(stderr, "refuse: cannot create %s (%s); outputs are never overwritten\n", out, strerror(errno)); est_file_free(&f); return 1; }
    c3_ticks tk;
    if (c3_ticks_build(&f, &tk, err, sizeof err)) { fprintf(stderr, "ticks: %s\n", err); fclose(fp); est_file_free(&f); return 1; }

    static g_fit gf[4];
    static c3_stats st[4];
    double full10[4] = { 0 };
    int agree[4] = { 0 };
    for (int id = 1; id <= 3; id++) {
        if (fit_family(&tk, id, stride, &gf[id])) { fprintf(stderr, "out of memory\n"); fclose(fp); return 1; }
        fprintf(stderr, "est4 fit: %s done (%zu + %zu points)\n", gname(id), gf[id].points1, gf[id].points2);
        if (!gf[id].have) continue;
        if (e4_score(&tk, &mk, &gf[id].p, &st[id], &full10[id])) { fprintf(stderr, "screen %s: %s\n", gname(id), st[id].errmsg); gf[id].have = 0; continue; }
        gf[id].full1 = st[id].logscore; gf[id].full10 = full10[id];
        agree[id] = fabs(gf[id].full1 - gf[id].fast1) <= E4_AGREE && fabs(gf[id].full10 - gf[id].fast10) <= E4_AGREE &&
                    st[id].n == gf[id].n1 && st[id].ten_n == gf[id].n10;
        if (!agree[id]) { fprintf(stderr, "est4 fit: %s fast and pmf scores disagree; family unavailable\n", gname(id)); gf[id].have = 0; }
    }
    /* baselines (v3 definitions): F1 grid and E0 histogram on D1 */
    int f1have = 0; double f1ls = -INFINITY; c3_model m, f1m;
    memset(&f1m, 0, sizeof f1m);
    for (int i = 0; i < NQ1; i += stride)
        for (int j = 0; j < NR1; j += stride) {
            c3_stats s1;
            c3_make_f1(&m, gq1(i), gr1(j));
            if (c3_score(&tk, NULL, &m, 0, &s1)) continue;
            if (isfinite(s1.logscore) && c3_grid_take(f1have, s1.logscore, f1ls)) { f1have = 1; f1ls = s1.logscore; f1m = m; }
        }
    double *e = malloc((tk.n + 1) * sizeof *e);
    uint32_t *e0c = calloc(EST_PRED_N, sizeof *e0c);
    if (!e || !e0c) { fprintf(stderr, "out of memory\n"); fclose(fp); return 1; }
    size_t ne = c3_h1_changes(&tk, e, tk.n + 1);
    c3_e0_counts(e, ne, e0c);

    int av[6] = { 0 }, ps[6] = { 0 }; double lsv[6] = { 0 };
    for (int id = 1; id <= 3; id++) { av[id] = gf[id].have; ps[id] = gf[id].have && st[id].calibrated; lsv[id] = gf[id].have ? gf[id].full1 : -INFINITY; }
    int sel = c3_select(av, ps, lsv);

    char doc_hex[65] = "unavailable";
    est_sha_file_hex(E4_PROTOCOL_DOC, doc_hex);
    fprintf(fp, "est4_params v4\n");
    fprintf(fp, "protocol %s\nprotocol_doc_sha256 %s\nprotocol_doc_sha256_on_disk %s\n", E4_PROTOCOL_DOC, proto, doc_hex);
    fprintf(fp, "tool_commit %s\ntool_dirty %d\nsynthetic_test %d\ngrid_stride %d\n", TOOL_COMMIT, tool_dirty(), synth, stride);
    fprintf(fp, "fit_raw_path %s\nfit_raw_sha256 %s\nfit_marks_path %s\nfit_marks_sha256 %s\nfit_lines %zu\n", raw, f.sha_hex, mkp, mk.sha, f.nlines);
    fprintf(fp, "d1_identity_compiled raw %s marks %s\n", d1raw, d1mk);
    fprintf(fp, "ticks total %zu inserted %zu gaps_capped %zu bad_t %zu absent_value %zu unparsable_value %zu\n",
            tk.n, tk.inserted, tk.gaps_capped, tk.bad_t, tk.absent_value, tk.unparsable_value);
    fprintf(fp, "marks pairs %zu begins %zu ends %zu unclosed %zu\n", mk.n, mk.begins, mk.ends, mk.unclosed);
    fprintf(fp, "rule stage1 maximise fast one-step mean log score over dyn q lam nu c (phi nu_h c_h unused)\n");
    fprintf(fp, "rule stage2 maximise fast ten-step mean log score over phi nu_h c_h with stage 1 frozen\n");
    fprintf(fp, "rule agreement fast vs pmf mean log score within %g nats, same step counts\n", E4_AGREE);
    for (int id = 1; id <= 3; id++) {
        const g_fit *g = &gf[id];
        fprintf(fp, "family %s available %d dyn %.17g q %.17g lam %.17g nu %.17g c %.17g phi %.17g nu_h %.17g c_h %.17g"
                    " logscore %.17g ten_logscore %.17g fast1 %.17g fast10 %.17g points1 %zu points2 %zu skipped %zu agree %d\n",
                gname(id), g->have, g->p.dyn, g->p.q, g->p.lam, g->p.nu, g->p.c, g->p.phi, g->p.nu_h, g->p.c_h,
                g->have ? g->full1 : -INFINITY, g->have ? g->full10 : -INFINITY, g->fast1, g->fast10, g->points1, g->points2, g->skipped, agree[id]);
        fprintf(fp, "grid_edge %s dyn %d q %d lam %d nu %d c %d phi %d nu_h %d c_h %d\n", gname(id),
                g->edge[0], g->edge[1], g->edge[2], g->edge[3], g->edge[4], g->edge[5], g->edge[6], g->edge[7]);
    }
    fprintf(fp, "baseline F1 available %d q_proc %.17g r %.17g logscore %.17g\n", f1have, f1m.a.param[0], f1m.a.param[1], f1ls);
    for (int id = 1; id <= 3; id++) {
        if (!gf[id].have) { fprintf(fp, "screen_result %s pass 0 first_fail unavailable n 0\n", gname(id)); continue; }
        put_screen(fp, gname(id), &st[id], full10[id]);
    }
    fprintf(fp, "e0_changes %zu\n", ne);
    for (int i = 0; i < EST_PRED_N; i++) if (e0c[i]) fprintf(fp, "e0_count %d %u\n", i - EST_PRED_K, e0c[i]);
    fprintf(fp, "selection_rule best D1 one-step log score among screen-passers; earlier of G1 < G2 < G3 within %.2f nats preferred\n", C3_TIE_NATS);
    if (sel) fprintf(fp, "selected %s\nphase_a PASS\n", gname(sel));
    else {
        fprintf(fp, "selected none\nphase_a PHASE_A_FAIL\n");
        for (int id = 1; id <= 3; id++) {
            fprintf(fp, "phase_a_fail %s", gname(id));
            if (!gf[id].have) fprintf(fp, " unavailable");
            else for (int i = 0; i < C3_ST_COUNT; i++)
                if (st[id].gated[i] && !st[id].pass[i]) fprintf(fp, " %s=%.6g", c3_stat_name(i), st[id].value[i]);
            fprintf(fp, "\n");
        }
    }
    int bad = ferror(fp) != 0;
    if (fclose(fp) != 0) bad = 1;
    if (bad) {
        /* never leave a partial params file behind: it would block a rerun (wbx) */
        remove(out);
        fprintf(stderr, "est4 fit: writing %s failed; partial file removed\n", out);
        for (int id = 1; id <= 3; id++) if (gf[id].have) c3_stats_free(&st[id]);
        free(e); free(e0c); c3_ticks_free(&tk); est_file_free(&f);
        return 1;
    }
    char ph[65] = "unavailable";
    est_sha_file_hex(out, ph);
    printf("EST4_FIT selected=%s phase_a=%s params_sha256=%s synthetic_test=%d\n", gname(sel), sel ? "PASS" : "PHASE_A_FAIL", ph, synth);
    for (int id = 1; id <= 3; id++) if (gf[id].have) c3_stats_free(&st[id]);
    free(e); free(e0c); c3_ticks_free(&tk); est_file_free(&f);
    return bad ? 1 : 0;
}

/* ------------------------------------------------------ params reader */
typedef struct {
    int phase_a_pass, selected, synthetic, tool_dirty;
    int have[4], pass[4];
    est4_params g[4];
    double ls[4];
    double f1q, f1r; int f1have;
    uint64_t e0_n; uint32_t e0[EST_PRED_N];
    char protocol_sha[65], fit_raw_sha[65], fit_marks_sha[65], fit_raw_path[1024];
} e4_params_file;

/* the whole key including its trailing space: no prefix of a longer key matches */
static int key(const char *line, const char *k) { return !strncmp(line, k, strlen(k)); }

static int gid(const char *s) { return !strcmp(s, "G1") ? 1 : !strcmp(s, "G2") ? 2 : !strcmp(s, "G3") ? 3 : 0; }

static int params_read(const char *path, e4_params_file *P, char *err, size_t cap)
{
    memset(P, 0, sizeof *P);
    FILE *fp = fopen(path, "rb");
    if (!fp) { snprintf(err, cap, "cannot open %s", path); return 1; }
    char line[4096], a[64], b[1100];
    int magic = 0, have_phase = 0, have_sel = 0;
    P->selected = -1;
    while (fgets(line, sizeof line, fp)) {
        if (!strncmp(line, "est4_params v4", 14)) magic = 1;
        else if (key(line, "protocol_doc_sha256 ") && sscanf(line + 20, "%64s", P->protocol_sha) == 1) {}
        else if (key(line, "fit_raw_sha256 ") && sscanf(line + 15, "%64s", P->fit_raw_sha) == 1) {}
        else if (key(line, "fit_marks_sha256 ") && sscanf(line + 17, "%64s", P->fit_marks_sha) == 1) {}
        else if (key(line, "fit_raw_path ")) { size_t L = strcspn(line + 13, "\n"); if (L >= sizeof P->fit_raw_path) L = sizeof P->fit_raw_path - 1; memcpy(P->fit_raw_path, line + 13, L); P->fit_raw_path[L] = 0; }
        else if (key(line, "synthetic_test ") && sscanf(line + 15, "%d", &P->synthetic) == 1) {}
        else if (key(line, "tool_dirty ") && sscanf(line + 11, "%d", &P->tool_dirty) == 1) {}
        else if (!strncmp(line, "family ", 7)) {
            est4_params g; int av; double ls;
            memset(&g, 0, sizeof g);
            if (sscanf(line, "family %63s available %d dyn %lf q %lf lam %lf nu %lf c %lf phi %lf nu_h %lf c_h %lf logscore %lf",
                       a, &av, &g.dyn, &g.q, &g.lam, &g.nu, &g.c, &g.phi, &g.nu_h, &g.c_h, &ls) != 11 || !gid(a)) {
                snprintf(err, cap, "bad family line"); fclose(fp); return 1; }
            int id = gid(a);
            g.family = (est4_family)id; g.quantum = C3_QUANTUM;
            P->have[id] = av; P->g[id] = g; P->ls[id] = av ? ls : -INFINITY;
        } else if (!strncmp(line, "baseline F1 ", 12)) {
            if (sscanf(line, "baseline F1 available %d q_proc %lf r %lf", &P->f1have, &P->f1q, &P->f1r) != 3) { snprintf(err, cap, "bad F1 line"); fclose(fp); return 1; }
        } else if (!strncmp(line, "screen_result ", 14)) {
            int ps;
            if (sscanf(line, "screen_result %63s pass %d", a, &ps) != 2 || !gid(a)) { snprintf(err, cap, "bad screen_result"); fclose(fp); return 1; }
            P->pass[gid(a)] = ps;
        } else if (!strncmp(line, "e0_count ", 9)) {
            int k; unsigned c;
            if (sscanf(line, "e0_count %d %u", &k, &c) != 2 || k < -EST_PRED_K || k > EST_PRED_K) { snprintf(err, cap, "bad e0_count"); fclose(fp); return 1; }
            P->e0[k + EST_PRED_K] = c; P->e0_n += c;
        } else if (!strncmp(line, "selected ", 9) && sscanf(line + 9, "%63s", a) == 1) { have_sel = 1; P->selected = gid(a); }
        else if (!strncmp(line, "phase_a ", 8) && sscanf(line + 8, "%1099s", b) == 1) { have_phase = 1; P->phase_a_pass = !strcmp(b, "PASS"); }
    }
    fclose(fp);
    if (!magic || !have_phase || !have_sel) { snprintf(err, cap, "not a v4 params file"); return 1; }
    for (int id = 1; id <= 3; id++) if (P->have[id] && est4_params_check(&P->g[id]) != EST_OK) { snprintf(err, cap, "%s params invalid", gname(id)); return 1; }
    return 0;
}

/* ------------------------------------------------------ eval */
static int has_receipt(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e; int found = 0;
    while ((e = readdir(d))) if (!strncmp(e->d_name, "receipt-", 8)) found = 1;
    closedir(d);
    return found;
}

static int cmd_recorded(int argc, char **argv)
{
    const char *dir = NULL, *ppath = NULL, *outdir = NULL;
    int synth = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--params") && i + 1 < argc) ppath = argv[++i];
        else if (!strcmp(argv[i], "--outdir") && i + 1 < argc) outdir = argv[++i];
        else if (!strcmp(argv[i], "--synthetic-test")) synth = 1;
        else { fprintf(stderr, "usage: est4 recorded --dir D2 --params P --outdir O [--synthetic-test]\n"); return 2; }
    }
    if (!dir || !ppath || !outdir) { fprintf(stderr, "est4 recorded: --dir, --params and --outdir are required\n"); return 2; }
    char err[512];
    const char *proto = envor("E4_TEST_PROTOCOL_SHA", E4_PROTOCOL_SHA);
    const char *d2r = envor("E4_TEST_D2_RAW_SHA", E4_D2_RAW_SHA), *d2m = envor("E4_TEST_D2_MARKS_SHA", E4_D2_MARKS_SHA),
               *d2s = envor("E4_TEST_D2_SCHED_SHA", E4_D2_SCHED_SHA), *psha = envor("E4_TEST_PARAMS_SHA", E4_PARAMS_SHA);
    if (!synth) {
        if (tool_dirty()) { fprintf(stderr, "refuse: recorded needs a clean tree (dirty=%d)\n", tool_dirty()); return 2; }
        if (!strcmp(d2r, "absent") || !strcmp(d2m, "absent") || !strcmp(d2s, "absent")) { fprintf(stderr, "refuse: no D2 identity compiled in (receipts/est-v4/d2.sha256)\n"); return 2; }
        char doc[65] = "";
        if (est_sha_file_hex(E4_PROTOCOL_DOC, doc) || strcmp(doc, proto)) { fprintf(stderr, "refuse: protocol doc on disk differs from the compiled SHA\n"); return 2; }
        char ph0[65] = "";
        if (!strcmp(psha, "absent") || est_sha_file_hex(ppath, ph0) || strcmp(ph0, psha)) { fprintf(stderr, "refuse: params SHA-256 differs from the compiled params SHA\n"); return 2; }
        char want[PATH_MAX + 64], rw[PATH_MAX], ro[PATH_MAX];
        const char *root = envor("E4_TEST_REPO_ROOT", E4_REPO_ROOT);
        if (!strcmp(root, "unknown")) { fprintf(stderr, "refuse: no repo root compiled in\n"); return 2; }
        snprintf(want, sizeof want, "%s/%s", root, E4_RECEIPT_SUBDIR);
        if (!realpath(want, rw) || !realpath(outdir, ro) || strcmp(rw, ro)) { fprintf(stderr, "refuse: a recorded receipt goes only to <repo>/%s\n", E4_RECEIPT_SUBDIR); return 2; }
        if (!strstr(dir, E4_HELDOUT_TAG)) { fprintf(stderr, "refuse: the recorded run scores only a %s run\n", E4_HELDOUT_TAG); return 2; }
    } else if (e4_heldout(dir) || strstr(dir, E4_FIT_TAG) || strstr(dir, C3_D1_ID)) {
        fprintf(stderr, "refuse: --synthetic-test never runs on D1 or a held-out run\n"); return 2;
    }
    static e4_params_file P;
    if (params_read(ppath, &P, err, sizeof err)) { fprintf(stderr, "refuse: %s\n", err); return 2; }
    if (!P.phase_a_pass) { fprintf(stderr, "refuse: params say PHASE_A_FAIL; D2 is NOT_RUN\n"); return 2; }
    int av[6] = { 0 }, ps[6] = { 0 }; double lsv[6] = { 0 };
    for (int id = 1; id <= 3; id++) { av[id] = P.have[id]; ps[id] = P.pass[id]; lsv[id] = P.ls[id]; }
    int resel = c3_select(av, ps, lsv);
    if (resel != P.selected || P.selected < 1) { fprintf(stderr, "refuse: params selected %s but the rule gives %s\n", gname(P.selected), gname(resel)); return 2; }
    if (!P.f1have) { fprintf(stderr, "refuse: params carry no F1 baseline\n"); return 2; }
    if (!synth) {
        if (P.synthetic || P.tool_dirty) { fprintf(stderr, "refuse: params are not a binding fit\n"); return 2; }
        if (strcmp(P.protocol_sha, proto)) { fprintf(stderr, "refuse: params fitted under another protocol SHA\n"); return 2; }
        if (strcmp(P.fit_raw_sha, envor("E4_TEST_D1_RAW_SHA", E4_D1_RAW_SHA)) || strcmp(P.fit_marks_sha, envor("E4_TEST_D1_MARKS_SHA", E4_D1_MARKS_SHA)) || !strstr(P.fit_raw_path, E4_FIT_TAG)) {
            fprintf(stderr, "refuse: params are not a fit on the compiled D1\n"); return 2; }
    }
    int hr = has_receipt(outdir);
    if (hr < 0) { fprintf(stderr, "refuse: outdir %s is not a readable directory\n", outdir); return 2; }
    if (hr > 0) { fprintf(stderr, "refuse: %s already holds a receipt; a second scored run is a new protocol version\n", outdir); return 2; }
    c3_pre pre;
    if (c3_precheck(dir, &pre, err, sizeof err)) { fprintf(stderr, "refuse: precheck: %s\n", err); return 2; }
    printf("precheck %s: %s\n", pre.valid ? "VALID" : "INCONCLUSIVE", pre.why);
    if (!pre.valid) { fprintf(stderr, "refuse: the pre-check is INCONCLUSIVE; D2 is not scored\n"); return 3; }
    if (!synth && (strcmp(pre.raw_sha, d2r) || strcmp(pre.marks_sha, d2m) || strcmp(pre.sched_sha, d2s))) {
        fprintf(stderr, "refuse: D2 data SHA-256 differs from the compiled D2 identity\n"); return 2; }
    char path[1200];
    est_file f;
    snprintf(path, sizeof path, "%s/machine-state.ndjson", dir);
    if (est_file_load(path, &f, err, sizeof err)) { fprintf(stderr, "refuse: %s\n", err); return 2; }
    /* the scored bytes are the identified bytes (the pre-check hashed a separate read) */
    if (!synth && strcmp(f.sha_hex, d2r)) { fprintf(stderr, "refuse: scored D2 bytes differ from the compiled D2 identity\n"); est_file_free(&f); return 2; }
    if (synth && strcmp(d2r, "absent") && !strcmp(f.sha_hex, d2r)) { fprintf(stderr, "refuse: --synthetic-test never runs on D1 or a held-out run (D2 bytes)\n"); est_file_free(&f); return 2; }
    c3_ticks tk;
    if (c3_ticks_build(&f, &tk, err, sizeof err)) { fprintf(stderr, "refuse: %s\n", err); est_file_free(&f); return 2; }
    static c3_marks mk;
    snprintf(path, sizeof path, "%s/machine-state-marks.txt", dir);
    if (c3_marks_load(path, 1, &mk, err, sizeof err)) { fprintf(stderr, "refuse: %s\n", err); return 2; }
    static c3_stats sS, s1, s0;
    static c3_model m1, m0;
    double l10S = -INFINITY;
    c3_make_f1(&m1, P.f1q, P.f1r);
    c3_make_e0(&m0, P.e0, P.e0_n);
    if (e4_score(&tk, &mk, &P.g[P.selected], &sS, &l10S) || c3_score(&tk, &mk, &m1, 1, &s1) || c3_score(&tk, &mk, &m0, 1, &s0)) {
        fprintf(stderr, "scoring error: %s %s %s\n", sS.errmsg, s1.errmsg, s0.errmsg); return 1; }
    int same = sS.n == s1.n && sS.n == s0.n && sS.unscorable == s1.unscorable && sS.unscorable == s0.unscorable;
    int beat1 = sS.logscore >= s1.logscore - C3_TIE_NATS, beat0 = sS.logscore >= s0.logscore - C3_TIE_NATS;
    /* est-json-1: a non-finite measured value is recorded as invalid and can never pass */
    size_t inv = c3_stats_invalid(&sS, C3_TEN_MEASURED, l10S) + c3_stats_invalid(&s1, C3_TEN_UNAVAILABLE, NAN) + c3_stats_invalid(&s0, C3_TEN_UNAVAILABLE, NAN);
    int pass = inv == 0 && same && sS.calibrated && beat1 && beat0;
    const char *verdict = pass ? "PASS" : "HELD_OUT_FAIL";
    const char *reason = inv ? "non-finite measured value (marked invalid in the receipt)" : !same ? "S, F1 and E0 were not scored on the same steps" : !sS.calibrated ? "S not calibrated"
                       : (!beat1 || !beat0) ? "calibrated but not better than a baseline" : "calibrated and no worse than F1 and E0";
    char *buf = NULL; size_t blen = 0;
    FILE *o = open_memstream(&buf, &blen);
    if (!o) { fprintf(stderr, "out of memory\n"); return 1; }
    char pdisk[65] = "unavailable";
    est_sha_file_hex(ppath, pdisk);
    fprintf(o, "{\n  \"receipt\": \"est4 v4 sealed held-out run\",\n  \"verdict\": \"%s\",\n  \"reason\": \"%s\",\n", verdict, reason);
    fprintf(o, "  \"synthetic_test\": %s,\n  \"tool_commit\": \"%s\",\n  \"tool_dirty\": %d,\n  \"protocol_sha256\": \"%s\",\n  \"params_sha256\": \"%s\",\n",
            synth ? "true" : "false", TOOL_COMMIT, tool_dirty(), proto, pdisk);
    fprintf(o, "  \"d2\": {\"dir\": "); c3_json_str(o, dir);
    fprintf(o, ", \"raw_sha256\": \"%s\", \"marks_sha256\": \"%s\", \"schedule_sha256\": \"%s\", \"lines\": %zu},\n", pre.raw_sha, pre.marks_sha, pre.sched_sha, pre.lines);
    fprintf(o, "  \"selected\": \"%s\",\n  \"comparisons\": {\"same_steps\": %s, \"no_worse_than_F1\": %s, \"no_worse_than_E0\": %s, \"tie_nats\": %.2f},\n",
            gname(P.selected), same ? "true" : "false", beat1 ? "true" : "false", beat0 ? "true" : "false", C3_TIE_NATS);
    fprintf(o, "  \"json_rules\": \"est-json-1\",\n  \"invalid_measurements\": %zu,\n", inv);
    fprintf(o, "  \"scores\": {\n");
    c3_json_stats(o, gname(P.selected), &sS, C3_TEN_MEASURED, l10S); fprintf(o, ",\n");
    c3_json_stats(o, "F1", &s1, C3_TEN_UNAVAILABLE, NAN); fprintf(o, ",\n");
    c3_json_stats(o, "E0", &s0, C3_TEN_UNAVAILABLE, NAN); fprintf(o, "\n  }\n}\n");
    fclose(o);
    uint8_t dg[32]; char hex[65];
    sha256_ctx c; sha256_init(&c); sha256_update(&c, (const uint8_t *)buf, blen); sha256_final(&c, dg);
    est_hex(dg, 32, hex);
    snprintf(path, sizeof path, "%s/receipt-%s.json", outdir, hex);
    FILE *w = fopen(path, "wbx");
    if (!w || fwrite(buf, 1, blen, w) != blen || fclose(w)) { fprintf(stderr, "cannot write %s\n", path); free(buf); return 1; }
    free(buf);
    printf("EST4_RECORDED verdict=%s selected=%s receipt=%s reason=%s\n", verdict, gname(P.selected), path, reason);
    c3_stats_free(&sS); c3_stats_free(&s1); c3_stats_free(&s0); c3_ticks_free(&tk); est_file_free(&f);
    return 0;
}

static int cmd_precheck(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[0], "--dir")) { fprintf(stderr, "usage: est4 precheck --dir D\n"); return 2; }
    char err[512]; c3_pre pre;
    if (c3_precheck(argv[1], &pre, err, sizeof err)) { fprintf(stderr, "precheck: %s\n", err); return 2; }
    printf("precheck lines %zu gaps_big %zu/%zu marks %s foreign_mean %.4f over3 %.4f unmeasured %zu\n",
           pre.lines, pre.gaps_big, pre.gaps, pre.ok_marks ? "ok" : pre.marks_why, pre.foreign_mean, pre.foreign_over3_frac, pre.foreign_unmeasured);
    printf("%s: %s\n", pre.valid ? "VALID" : "INCONCLUSIVE", pre.why);
    return pre.valid ? 0 : 3;
}

static int cmd_synth(int argc, char **argv)
{
    if (argc < 1) { fprintf(stderr, "usage: est4 make-synthetic DIR [--seed N] [--lines N]\n"); return 2; }
    c3_synth sy; memset(&sy, 0, sizeof sy);
    sy.seed = 1; sy.lines = 2400;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc) sy.seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--lines") && i + 1 < argc) sy.lines = (size_t)strtoull(argv[++i], NULL, 0);
        else return 2;
    }
    return c3_synth_write(argv[0], &sy) ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: est4 fit|precheck|recorded|make-synthetic ...\n"); return 2; }
    if (!strcmp(argv[1], "fit")) return cmd_fit(argc - 2, argv + 2);
    if (!strcmp(argv[1], "recorded")) return cmd_recorded(argc - 2, argv + 2);
    if (!strcmp(argv[1], "precheck")) return cmd_precheck(argc - 2, argv + 2);
    if (!strcmp(argv[1], "make-synthetic")) return cmd_synth(argc - 2, argv + 2);
    fprintf(stderr, "est4: unknown command %s\n", argv[1]);
    return 2;
}
