/* EST-3c protocol v3, section 5: the D1 fit (docs/estimation/EST3C_PROTOCOL_V3.md).
 *
 *   est3c_fit --raw <dir>/machine-state.ndjson --marks <dir>/machine-state-marks.txt
 *             --out <params.txt> [--synthetic-test] [--grid-stride N]
 *
 * Grids exactly as section 5 (ties: first grid point in listed order, i.e. the
 * first-listed parameter is the outer loop; strict > keeps the earlier point).
 * Objective: mean discrete one-step log score after burn-in (est3c_common.h).
 * F4 is the EM fit (k = 3, floor 100^2/12, 1000 iterations) on the horizon-1
 * changes. Every family is then screened in sample with every section 6 rule,
 * and the selection rule picks the family (or PHASE_A_FAIL).
 *
 * Binding mode (no --synthetic-test) refuses: a dirty tree at build time, a
 * compiled protocol SHA that is "unknown" or differs from the doc on disk, an
 * input outside the D1 run folder, any held-out (D2) path, an existing output,
 * --grid-stride. --synthetic-test lifts the dirty-tree and D1 guards (never the
 * D2 guard) and marks the parameter file synthetic_test 1; --grid-stride N
 * (synthetic only) keeps every Nth grid point per axis, for tests. */
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "est3c_common.h"
#include "sha256.h"

#ifndef TOOL_COMMIT
#define TOOL_COMMIT "unknown"
#endif
#ifndef TOOL_DIRTY
#define TOOL_DIRTY 1
#endif
#ifndef C3_PROTOCOL_SHA
#define C3_PROTOCOL_SHA "unknown"
#endif

static int tool_dirty(void)
{
#ifdef C3_TEST_BUILD
    const char *e = getenv("C3_TEST_DIRTY"); if (e && *e) return atoi(e);
    return TOOL_DIRTY;
#else
    /* measured once, at the first call (before any output file exists) */
    static int cached = -1;
    if (cached < 0) cached = TOOL_DIRTY ? TOOL_DIRTY : c3_tree_dirty_now();
    return cached;
#endif
}

/* compiled protocol SHA (test build: C3_TEST_PROTOCOL_SHA overrides it) */
static const char *protocol_sha(void)
{
#ifdef C3_TEST_BUILD
    const char *e = getenv("C3_TEST_PROTOCOL_SHA"); if (e) return e;
#endif
    return C3_PROTOCOL_SHA;
}

static int path_in_d1(const char *path)
{
    char rp[PATH_MAX];
    return strstr(path, C3_D1_ID) != NULL || (realpath(path, rp) && strstr(rp, C3_D1_ID) != NULL);
}

/* ---- grids (protocol section 5) ---- */
#define NQ 25   /* q_proc = 10^(1.0 + 0.25 i) */
#define NR 17   /* r = 10^(2.0 + 0.25 j) */
#define NC2 4
#define NNU3 10
#define NS3 51  /* s = 10^(1.0 + 0.05 j) */
#define NL5 7
#define NNU5 7
#define NC5 21  /* c = 0.50 + 0.05 j */
#define NF5 3
static const double C2[NC2] = { 1.0, 1.345, 2.0, 3.0 };
static const double NU3[NNU3] = { 1.5, 2, 2.5, 3, 4, 5, 7, 10, 15, 30 };
static const double L5[NL5] = { 0.5, 0.7, 0.8, 0.9, 0.95, 0.97, 0.99 };
static const double NU5[NNU5] = { 2, 3, 4, 5, 7, 10, 30 };
static const double F5FL[NF5] = { 100.0 * 100.0 / 12.0, 50.0 * 50.0, 100.0 * 100.0 };
static double gq(int i) { return pow(10.0, 1.0 + 0.25 * (double)i); }
static double gr(int j) { return pow(10.0, 2.0 + 0.25 * (double)j); }
static double gs(int j) { return pow(10.0, 1.0 + 0.05 * (double)j); }
static double gc5(int j) { return (double)(50 + 5 * j) / 100.0; }

typedef struct {
    int have; c3_model m; double ls;
    size_t points, finite, errors;
} fam_fit;

static void consider(fam_fit *ff, const c3_ticks *tk, const c3_model *m)
{
    c3_stats st;
    ff->points++;
    if (c3_score(tk, NULL, m, 0, &st)) { ff->errors++; return; }
    if (!isfinite(st.logscore)) return;
    ff->finite++;
    if (c3_grid_take(ff->have, st.logscore, ff->ls)) { ff->have = 1; ff->m = *m; ff->ls = st.logscore; }
}

static void put_family(FILE *fp, int id, const fam_fit *ff)
{
    fprintf(fp, "family %s available %d nparam %u param", c3_name(id), ff->have, ff->have ? ff->m.a.nparam : 0u);
    for (int j = 0; j < 8; j++) fprintf(fp, " %.17g", ff->have ? ff->m.a.param[j] : 0.0);
    fprintf(fp, " logscore %.17g grid_points %zu grid_finite %zu grid_errors %zu\n",
            ff->have ? ff->ls : -INFINITY, ff->points, ff->finite, ff->errors);
}

static void put_screen(FILE *fp, int id, const c3_stats *st, int have)
{
    if (!have) { fprintf(fp, "screen_result %s pass 0 first_fail unavailable n 0\n", c3_name(id)); return; }
    for (int i = 0; i < C3_ST_COUNT; i++)
        fprintf(fp, "screen %s %s value %.17g band %.17g %.17g n %zu gated %d pass %d\n", c3_name(id), c3_stat_name(i),
                st->value[i], st->band_lo[i], st->band_hi[i],
                (i >= C3_ST_Q0 && i <= C3_ST_TEN) ? st->sub_n[i] : st->n, st->gated[i], st->pass[i]);
    fprintf(fp, "screen_sharpness %s width80_mean_mc %.17g width80_median_mc %.17g unscorable %zu\n",
            c3_name(id), st->width_mean, st->width_median, st->unscorable);
    fprintf(fp, "screen_result %s pass %d first_fail %s n %zu logscore %.17g\n", c3_name(id), st->calibrated,
            st->calibrated ? "none" : c3_stat_name(st->first_fail), st->n, st->logscore);
}

#ifdef C3_AS_LIB
#define C3_MAIN c3_fit_main
int c3_fit_main(int argc, char **argv);
#else
#define C3_MAIN main
#endif

int C3_MAIN(int argc, char **argv)
{
    const char *raw = NULL, *mkp = NULL, *out = NULL;
    int synth = 0, stride = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--raw") && i + 1 < argc) raw = argv[++i];
        else if (!strcmp(argv[i], "--marks") && i + 1 < argc) mkp = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--synthetic-test")) synth = 1;
        else if (!strcmp(argv[i], "--grid-stride") && i + 1 < argc) stride = atoi(argv[++i]);
        else { fprintf(stderr, "usage: est3c_fit --raw <ndjson> --marks <marks> --out <params.txt> [--synthetic-test] [--grid-stride N]\n"); return 2; }
    }
    if (!raw || !mkp || !out) { fprintf(stderr, "est3c_fit: --raw, --marks and --out are required\n"); return 2; }
    if (stride < 1) { fprintf(stderr, "refuse: --grid-stride must be >= 1\n"); return 2; }
    if (c3_path_is_heldout(raw) || c3_path_is_heldout(mkp)) { fprintf(stderr, "refuse: input is a held-out (D2) run; fitting on it is not allowed\n"); return 2; }
    if (!synth) {
        if (tool_dirty()) { fprintf(stderr, "refuse: the binding fit needs a clean working tree at build time (TOOL_DIRTY=%d); use --synthetic-test for tests\n", tool_dirty()); return 2; }
        if (!path_in_d1(raw) || !path_in_d1(mkp)) { fprintf(stderr, "refuse: the binding fit opens only D1 (%s)\n", C3_D1_ID); return 2; }
        char doc[65] = "";
        if (!strcmp(protocol_sha(), "unknown") || est_sha_file_hex(C3_PROTOCOL_DOC, doc) || strcmp(doc, protocol_sha())) {
            fprintf(stderr, "refuse: the binding fit needs the compiled protocol SHA (%s) to equal the SHA-256 of %s on disk (%s)\n", protocol_sha(), C3_PROTOCOL_DOC, doc[0] ? doc : "unreadable"); return 2; }
#ifndef C3_TEST_BUILD
        if (stride != 1) { fprintf(stderr, "refuse: --grid-stride only with --synthetic-test\n"); return 2; }
#endif
    }
    if (strpbrk(raw, " \t\n") || strpbrk(mkp, " \t\n")) { fprintf(stderr, "refuse: paths with whitespace are not supported\n"); return 2; }

    char err[512];
    est_file f;
    if (est_file_load(raw, &f, err, sizeof err)) { fprintf(stderr, "refuse: %s\n", err); return 1; }
    static c3_marks mk;
    if (c3_marks_load(mkp, 1, &mk, err, sizeof err)) { fprintf(stderr, "refuse: marks: %s\n", err); est_file_free(&f); return 1; }
    FILE *fp = fopen(out, "wbx");
    if (!fp) { fprintf(stderr, "refuse: cannot create %s (%s); outputs are never overwritten\n", out, strerror(errno)); est_file_free(&f); return 1; }
    c3_ticks tk;
    if (c3_ticks_build(&f, &tk, err, sizeof err)) { fprintf(stderr, "ticks: %s\n", err); fclose(fp); est_file_free(&f); return 1; }

    fam_fit ff[6];
    memset(ff, 0, sizeof ff);
    c3_model m;
    for (int i = 0; i < NQ; i += stride)
        for (int j = 0; j < NR; j += stride) { c3_make_f1(&m, gq(i), gr(j)); consider(&ff[1], &tk, &m); }
    fprintf(stderr, "est3c_fit: F1 done (%zu points)\n", ff[1].points);
    for (int i = 0; i < NQ; i += stride)
        for (int j = 0; j < NR; j += stride)
            for (int k = 0; k < NC2; k += (stride > 1 ? 2 : 1)) { c3_make_f2(&m, gq(i), gr(j), C2[k]); consider(&ff[2], &tk, &m); }
    fprintf(stderr, "est3c_fit: F2 done (%zu points)\n", ff[2].points);
    for (int i = 0; i < NNU3; i += (stride > 1 ? 2 : 1))
        for (int j = 0; j < NS3; j += stride) { c3_make_f3(&m, NU3[i], gs(j)); consider(&ff[3], &tk, &m); }
    fprintf(stderr, "est3c_fit: F3 done (%zu points)\n", ff[3].points);

    /* F4: EM on the horizon-1 changes (also the E0 histogram) */
    double *e = malloc((tk.n + 1) * sizeof *e);
    if (!e) { fprintf(stderr, "out of memory\n"); fclose(fp); return 1; }
    size_t ne = c3_h1_changes(&tk, e, tk.n + 1);
    est_mix mx; double em_ll = NAN;
    memset(&mx, 0, sizeof mx);
    est_status ems = est_mix_fit_em(e, ne, 3u, 100.0 * 100.0 / 12.0, 1000u, &mx, &em_ll);
    if (ems == EST_OK && c3_make_f4(&m, &mx) == 0 && est_assumption_check(&m.a) == EST_OK) consider(&ff[4], &tk, &m);
    else ff[4].points = 1;
    uint32_t *e0c = calloc(EST_PRED_N, sizeof *e0c);
    if (!e0c) { fprintf(stderr, "out of memory\n"); fclose(fp); return 1; }
    c3_e0_counts(e, ne, e0c);
    fprintf(stderr, "est3c_fit: F4 EM status %d on %zu changes\n", (int)ems, ne);

    for (int a = 0; a < NL5; a += (stride > 1 ? 3 : 1))
        for (int b = 0; b < NNU5; b += (stride > 1 ? 3 : 1))
            for (int c = 0; c < NC5; c += stride)
                for (int d = 0; d < NF5; d += (stride > 1 ? 2 : 1)) { c3_make_f5(&m, L5[a], NU5[b], gc5(c), F5FL[d]); consider(&ff[5], &tk, &m); }
    fprintf(stderr, "est3c_fit: F5 done (%zu points)\n", ff[5].points);

    /* in-sample screen and selection */
    static c3_stats st[6];
    for (int id = 1; id <= 5; id++) {
        memset(&st[id], 0, sizeof st[id]);
        if (!ff[id].have) continue;
        if (c3_score(&tk, &mk, &ff[id].m, 1, &st[id])) { fprintf(stderr, "screen %s: %s\n", c3_name(id), st[id].errmsg); ff[id].have = 0; }
    }
    int av[6], ps[6]; double lsv[6];
    for (int id = 0; id <= 5; id++) { av[id] = ff[id].have; ps[id] = id ? st[id].calibrated : 0; lsv[id] = ff[id].ls; }
    int sel = c3_select(av, ps, lsv);

    char marks_sha[65]; memcpy(marks_sha, mk.sha, 65);
    char doc_hex[65] = "unavailable";
    est_sha_file_hex(C3_PROTOCOL_DOC, doc_hex);
    fprintf(fp, "est3c_params v3\n");
    fprintf(fp, "protocol %s\nprotocol_doc_sha256 %s\nprotocol_doc_sha256_on_disk %s\n", C3_PROTOCOL_DOC, protocol_sha(), doc_hex);
    fprintf(fp, "tool_commit %s\ntool_dirty %d\nsynthetic_test %d\n", TOOL_COMMIT, tool_dirty(), synth);
    fprintf(fp, "fit_raw_path %s\nfit_raw_sha256 %s\nfit_marks_path %s\nfit_marks_sha256 %s\nfit_lines %zu\n", raw, f.sha_hex, mkp, marks_sha, f.nlines);
    fprintf(fp, "assumption quantum %.17g lo %.17g hi %.17g unit milli_celsius\n", C3_QUANTUM, C3_LO, C3_HI);
    fprintf(fp, "rule burn_in %u ticks from the first valid observation\n", C3_BURN_IN);
    fprintf(fp, "rule f1_f2_p0 p0 = r\nrule f5_s0 s0 = sqrt(floor)\n");
    fprintf(fp, "rule tick_mapping one tick per line; round(gap)-1 inserted MISSING ticks before a line whose gap rounds above 1 s\n");
    fprintf(fp, "rule objective mean discrete one-step log score (nats) over scored ticks\n");
    fprintf(fp, "rule grid_tie first grid point in listed order (strict >)\n");
    fprintf(fp, "grid_stride %d\n", stride);
    fprintf(fp, "ticks total %zu inserted %zu gaps_capped %zu lines_multi %zu bad_t %zu absent_value %zu unparsable_value %zu gap_zero %zu gap_backward %zu\n",
            tk.n, tk.inserted, tk.gaps_capped, tk.lines_multi, tk.bad_t, tk.absent_value, tk.unparsable_value, tk.gap_zero, tk.gap_backward);
    fprintf(fp, "marks pairs %zu begins %zu ends %zu unclosed %zu nested %zu stray_ends %zu bad_lines %zu\n",
            mk.n, mk.begins, mk.ends, mk.unclosed, mk.nested_begins, mk.stray_ends, mk.bad_lines);
    for (int id = 1; id <= 5; id++) put_family(fp, id, &ff[id]);
    fprintf(fp, "f4_em status %d changes %zu loglik %.17g\n", (int)ems, ne, em_ll);
    for (int id = 1; id <= 5; id++) put_screen(fp, id, &st[id], ff[id].have);
    fprintf(fp, "e0_changes %zu\n", ne);
    for (int i = 0; i < EST_PRED_N; i++) if (e0c[i]) fprintf(fp, "e0_count %d %u\n", i - EST_PRED_K, e0c[i]);
    fprintf(fp, "selection_rule best D1 log score among screen-passers; earliest family within %.2f nats preferred\n", C3_TIE_NATS);
    if (sel) fprintf(fp, "selected %s\nphase_a PASS\n", c3_name(sel));
    else {
        fprintf(fp, "selected none\nphase_a PHASE_A_FAIL\n");
        for (int id = 1; id <= 5; id++) {
            fprintf(fp, "phase_a_fail %s", c3_name(id));
            if (!ff[id].have) fprintf(fp, " unavailable");
            else for (int i = 0; i < C3_ST_COUNT; i++)
                if (st[id].gated[i] && !st[id].pass[i]) fprintf(fp, " %s=%.6g", c3_stat_name(i), st[id].value[i]);
            fprintf(fp, "\n");
        }
    }
    int bad = fclose(fp);
    char ph[65] = "unavailable";
    est_sha_file_hex(out, ph);
    printf("EST3C_FIT selected=%s phase_a=%s params_sha256=%s synthetic_test=%d\n", sel ? c3_name(sel) : "none",
           sel ? "PASS" : "PHASE_A_FAIL", ph, synth);
    for (int id = 1; id <= 5; id++) c3_stats_free(&st[id]);
    free(e); free(e0c); c3_ticks_free(&tk); est_file_free(&f);
    return bad ? 1 : 0;
}
