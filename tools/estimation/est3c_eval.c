/* EST-3c protocol v3, sections 6 to 8: the sealed evaluator.
 *
 *   est3c_eval --precheck --dir D
 *       Section 7 pre-check. Reads only t, loadavg, cpu, marks and schedule.txt
 *       (never thermal_mc). Prints VALID or INCONCLUSIVE with the numbers.
 *   est3c_eval --recorded --dir D2 --params P --outdir O [--synthetic-test]
 *       The one scored run (step 4). Refuses: dirty tree, absent compiled D2
 *       identity (ndjson, marks and schedule.txt from d2.sha256), protocol doc
 *       changed, absent compiled params SHA or a params file whose SHA-256
 *       differs from it (receipts/est3c-v3/params.txt at build time), O other
 *       than <repo>/docs/estimation/receipts/est3c-v3 (by realpath), params not
 *       a binding D1 fit, params PHASE_A_FAIL, a `selected` that the section 5
 *       rule (c3_select on the params' screen results and log scores) does not
 *       reproduce, an existing receipt in O, a pre-check that is not VALID, D2
 *       data whose SHA-256 differs from the compiled one. Scores S, F1 and E0
 *       on the same steps (section 6), adds the informational block (S on A, B,
 *       C1), writes O/receipt-<sha256 of the receipt>.json, prints the verdict.
 *       --synthetic-test lifts only the dirty-tree, D1, compiled-D2,
 *       compiled-params and receipt-folder guards (never the selection check),
 *       refuses a held-out (D2) or D1 path, skips the informational block and
 *       marks the receipt synthetic_test true.
 *   est3c_eval --make-synthetic DIR [--seed N] [--lines N] [--foreign X]
 *       Writes a synthetic run folder (tests and smoke runs only).
 */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
/* From docs/estimation/receipts/est3c-v3/d2.sha256 at build time ("absent" when
 * that file does not exist yet: --recorded then refuses). */
#ifndef C3_D2_RAW_SHA
#define C3_D2_RAW_SHA "absent"
#endif
#ifndef C3_D2_MARKS_SHA
#define C3_D2_MARKS_SHA "absent"
#endif
#ifndef C3_D2_SCHED_SHA
#define C3_D2_SCHED_SHA "absent"
#endif
/* SHA-256 of docs/estimation/receipts/est3c-v3/params.txt at build time
 * ("absent" before it is committed: --recorded then refuses). */
#ifndef C3_PARAMS_SHA
#define C3_PARAMS_SHA "absent"
#endif
/* repository root at build time; the one receipt folder lives under it */
#ifndef C3_REPO_ROOT
#define C3_REPO_ROOT "unknown"
#endif
#define C3_RECEIPT_SUBDIR "docs/estimation/receipts/est3c-v3"

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
static const char *d2_raw_sha(void)
{
#ifdef C3_TEST_BUILD
    const char *e = getenv("C3_TEST_D2_RAW_SHA"); if (e) return e;
#endif
    return C3_D2_RAW_SHA;
}
static const char *d2_marks_sha(void)
{
#ifdef C3_TEST_BUILD
    const char *e = getenv("C3_TEST_D2_MARKS_SHA"); if (e) return e;
#endif
    return C3_D2_MARKS_SHA;
}
static const char *d2_sched_sha(void)
{
#ifdef C3_TEST_BUILD
    const char *e = getenv("C3_TEST_D2_SCHED_SHA"); if (e) return e;
#endif
    return C3_D2_SCHED_SHA;
}
static const char *params_sha(void)
{
#ifdef C3_TEST_BUILD
    const char *e = getenv("C3_TEST_PARAMS_SHA"); if (e) return e;
#endif
    return C3_PARAMS_SHA;
}
/* the only folder a recorded (non-synthetic) receipt may be written to */
static int receipt_dir(char *out, size_t cap)
{
#ifdef C3_TEST_BUILD
    const char *e = getenv("C3_TEST_RECEIPT_DIR");
    if (e) { snprintf(out, cap, "%s", e); return 0; }
#endif
    if (!strcmp(C3_REPO_ROOT, "unknown")) return 1;
    snprintf(out, cap, "%s/%s", C3_REPO_ROOT, C3_RECEIPT_SUBDIR);
    return 0;
}
/* D1 folder whose SHA256SUMS (text only) proves the params' fit identity. */
static const char *d1_dir(void)
{
#ifdef C3_TEST_BUILD
    return getenv("C3_TEST_D1_DIR"); /* the test build never falls back to real D1 */
#else
    return C3_D1_DIR_DEFAULT;
#endif
}

static void print_pre(FILE *o, const c3_pre *p)
{
    fprintf(o, "precheck lines %zu (min %u) %s\n", p->lines, C3_MIN_LINES, p->ok_lines ? "ok" : "FAIL");
    fprintf(o, "precheck gaps_above_1.5s %zu of %zu = %.4f (max 0.02) %s\n", p->gaps_big, p->gaps, p->gap_big_frac, p->ok_gaps ? "ok" : "FAIL");
    fprintf(o, "precheck schedule %s trials %zu segments %zu; marks %s (%s)\n", p->have_schedule ? "present" : "ABSENT",
            p->sched_trials, p->sched_segments, p->ok_marks ? "ok" : "FAIL", p->marks_why);
    fprintf(o, "precheck foreign_busy_cores mean %.4f (max 1.5) %s; over_3.0 %zu of %zu = %.4f (max 0.10) %s; unmeasured %zu %s\n",
            p->foreign_mean, p->ok_foreign_mean ? "ok" : "FAIL", p->foreign_over3, p->foreign_n, p->foreign_over3_frac,
            p->ok_foreign_tail ? "ok" : "FAIL", p->foreign_unmeasured, p->ok_foreign_measured ? "ok" : "FAIL");
    fprintf(o, "precheck loadavg1_mean %.3f bad_t %zu\n", p->loadavg1_mean, p->bad_t);
    fprintf(o, "precheck raw_sha256 %s marks_sha256 %s schedule_sha256 %s\n", p->raw_sha, p->marks_sha, p->sched_sha);
    fprintf(o, "%s: %s\n", p->valid ? "VALID" : "INCONCLUSIVE", p->why);
}

static int has_receipt(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e; int found = 0;
    while ((e = readdir(d))) if (!strncmp(e->d_name, "receipt-", 8)) found = 1;
    closedir(d);
    return found;
}

static const char *class_hint(int s)
{
    if (s < 0) return "none";
    if (s <= C3_ST_COV95 || (s >= C3_ST_PIT0 && s < C3_ST_BIAS)) return "wrong noise shape";
    if (s == C3_ST_BIAS) return "misspecification";
    if (s == C3_ST_LAG1 || s == C3_ST_TEN) return "dependence";
    if (s >= C3_ST_Q0 && s < C3_ST_REG_TRIAL) return "nonstationarity";
    if (s == C3_ST_REG_IDLE || s == C3_ST_REG_TRIAL) return "regime switching";
    return "sensor fault";
}

static int load_run(const char *dir, c3_ticks *tk, c3_marks *mk, char *raw_sha, char *err, size_t cap)
{
    char path[1200];
    est_file f;
    snprintf(path, sizeof path, "%s/machine-state.ndjson", dir);
    if (est_file_load(path, &f, err, cap)) return 1;
    memcpy(raw_sha, f.sha_hex, 65);
    int r = c3_ticks_build(&f, tk, err, cap);
    est_file_free(&f);
    if (r) return 1;
    snprintf(path, sizeof path, "%s/machine-state-marks.txt", dir);
    if (c3_marks_load(path, 1, mk, err, cap)) { c3_ticks_free(tk); return 1; }
    return 0;
}

static int recorded(const char *dir, const char *ppath, const char *outdir, int synth)
{
    char err[512];
    if (!synth) {
        if (tool_dirty()) { fprintf(stderr, "refuse: --recorded needs a clean working tree at build time (TOOL_DIRTY=%d)\n", tool_dirty()); return 2; }
        if (!strcmp(d2_raw_sha(), "absent") || !strcmp(d2_marks_sha(), "absent") || !strcmp(d2_sched_sha(), "absent")) {
            fprintf(stderr, "refuse: no D2 identity compiled in (docs/estimation/receipts/est3c-v3/d2.sha256 absent or incomplete at build time)\n"); return 2; }
        char doc[65] = "";
        if (est_sha_file_hex(C3_PROTOCOL_DOC, doc) || strcmp(doc, C3_PROTOCOL_SHA)) {
            fprintf(stderr, "refuse: %s SHA-256 on disk differs from the compiled protocol SHA %s\n", C3_PROTOCOL_DOC, C3_PROTOCOL_SHA); return 2; }
        if (!strcmp(params_sha(), "absent")) {
            fprintf(stderr, "refuse: no params SHA-256 compiled in (%s/params.txt absent at build time)\n", C3_RECEIPT_SUBDIR); return 2; }
        char ph0[65] = "";
        if (est_sha_file_hex(ppath, ph0) || strcmp(ph0, params_sha())) {
            fprintf(stderr, "refuse: params file SHA-256 %s differs from the compiled params SHA %s\n", ph0[0] ? ph0 : "unreadable", params_sha()); return 2; }
        char want[PATH_MAX + 64], rw[PATH_MAX], ro[PATH_MAX];
        if (receipt_dir(want, sizeof want) || !realpath(want, rw) || !realpath(outdir, ro) || strcmp(rw, ro)) {
            fprintf(stderr, "refuse: a recorded receipt goes only to <repo>/%s (outdir %s)\n", C3_RECEIPT_SUBDIR, outdir); return 2; }
    } else if (c3_path_is_heldout(dir) || strstr(dir, C3_D1_ID)) {
        fprintf(stderr, "refuse: --synthetic-test never runs on D1 or a held-out run\n"); return 2;
    }
    static c3_params P;
    if (c3_params_read(ppath, &P, err, sizeof err)) { fprintf(stderr, "refuse: %s\n", err); return 2; }
    if (!P.phase_a_pass) { fprintf(stderr, "refuse: params say PHASE_A_FAIL; D2 is not scored\n"); return 2; }
    /* section 5 selection recomputed from the params' own screen results and log scores */
    int resel = c3_select(P.have, P.screen_pass, P.logscore);
    if (resel != P.selected || P.selected < 1 || P.screen_pass[P.selected] != 1) {
        fprintf(stderr, "refuse: params say selected %s but the selection rule on their screen results gives %s\n",
                c3_name(P.selected), resel ? c3_name(resel) : "none"); return 2; }
    if (!synth) {
        const char *d1 = d1_dir();
        char h1[65], h2[65];
        if (P.synthetic != 0 || P.tool_dirty != 0) { fprintf(stderr, "refuse: params are not a binding fit (synthetic_test %d tool_dirty %d)\n", P.synthetic, P.tool_dirty); return 2; }
        if (strcmp(P.protocol_sha, C3_PROTOCOL_SHA)) { fprintf(stderr, "refuse: params were fitted under another protocol SHA\n"); return 2; }
        if (!strstr(P.fit_raw_path, C3_D1_ID) || !strstr(P.fit_marks_path, C3_D1_ID)) { fprintf(stderr, "refuse: params are not a D1 fit (path)\n"); return 2; }
        if (!d1 || est_sums_lookup(d1, "machine-state.ndjson", h1) || est_sums_lookup(d1, "machine-state-marks.txt", h2)
            || strcmp(h1, P.fit_raw_sha) || strcmp(h2, P.fit_marks_sha)) {
            fprintf(stderr, "refuse: params are not a D1 fit (SHA-256 differs from D1 SHA256SUMS)\n"); return 2; }
    }
    int hr = has_receipt(outdir);
    if (hr < 0) { fprintf(stderr, "refuse: outdir %s is not a readable directory\n", outdir); return 2; }
    if (hr > 0) { fprintf(stderr, "refuse: %s already holds a receipt; a second scored run is a new protocol version\n", outdir); return 2; }
    c3_pre pre;
    if (c3_precheck(dir, &pre, err, sizeof err)) { fprintf(stderr, "refuse: precheck: %s\n", err); return 2; }
    print_pre(stdout, &pre);
    if (!pre.valid) { fprintf(stderr, "refuse: the pre-check is INCONCLUSIVE; D2 is not scored\n"); return 3; }
    if (!synth && (strcmp(pre.raw_sha, d2_raw_sha()) || strcmp(pre.marks_sha, d2_marks_sha()) || strcmp(pre.sched_sha, d2_sched_sha()))) {
        fprintf(stderr, "refuse: D2 data SHA-256 differs from the compiled D2 identity\n"); return 2; }

    /* scored run */
    c3_ticks tk; static c3_marks mk; char raw_sha[65];
    if (load_run(dir, &tk, &mk, raw_sha, err, sizeof err)) { fprintf(stderr, "refuse: %s\n", err); return 2; }
    static c3_model mS, m1, m0;
    if (c3_model_from_params(&P, P.selected, &mS) || c3_model_from_params(&P, 1, &m1) || c3_model_from_params(&P, 0, &m0)) {
        fprintf(stderr, "refuse: parameter file models do not validate\n"); c3_ticks_free(&tk); return 2; }
    static c3_stats sS, s1, s0;
    if (c3_score(&tk, &mk, &mS, 1, &sS) || c3_score(&tk, &mk, &m1, 1, &s1) || c3_score(&tk, &mk, &m0, 1, &s0)) {
        fprintf(stderr, "scoring error: %s%s%s\n", sS.errmsg, s1.errmsg, s0.errmsg); c3_ticks_free(&tk); return 1; }
    int same = sS.n == s1.n && sS.n == s0.n && sS.unscorable == s1.unscorable && sS.unscorable == s0.unscorable;
    int beat1 = sS.logscore >= s1.logscore - C3_TIE_NATS, beat0 = sS.logscore >= s0.logscore - C3_TIE_NATS;
    /* est-json-1: a non-finite measured value is recorded as invalid and can never pass */
    size_t inv = c3_stats_invalid(&sS, C3_TEN_ABSENT, 0) + c3_stats_invalid(&s1, C3_TEN_ABSENT, 0) + c3_stats_invalid(&s0, C3_TEN_ABSENT, 0);
    int pass = inv == 0 && same && sS.calibrated && beat1 && beat0;
    const char *verdict = pass ? "PASS" : "FAIL";
    const char *reason = inv ? "non-finite measured value (marked invalid in the receipt)" : !same ? "S, F1 and E0 were not scored on the same steps"
                       : !sS.calibrated ? "S not calibrated"
                       : (!beat1 || !beat0) ? "calibrated but not better than a baseline" : "calibrated and no worse than F1 and E0";

    /* informational block (not gating) */
    static c3_stats si[3]; int si_ok[3] = { 0, 0, 0 }; char si_err[3][256]; char si_sha[3][65];
    const char *dev[3] = { C3_DEV_A, C3_DEV_B, C3_DEV_C1 };
    const char *devn[3] = { "A", "B", "C1" };
    if (!synth) {
        for (int i = 0; i < 3; i++) {
            c3_ticks t2; static c3_marks m2;
            si_err[i][0] = 0; snprintf(si_sha[i], 65, "unavailable");
            est_allow_run_b = (i == 1); /* B is spent (v1 held-out); read here only as information */
            if (load_run(dev[i], &t2, &m2, si_sha[i], si_err[i], sizeof si_err[i])) { est_allow_run_b = 0; continue; }
            est_allow_run_b = 0;
            if (c3_score(&t2, &m2, &mS, 1, &si[i]) == 0) si_ok[i] = 1; else snprintf(si_err[i], sizeof si_err[i], "%s", si[i].errmsg);
            c3_ticks_free(&t2);
        }
    }

    char ph[65] = "unavailable";
    est_sha_file_hex(ppath, ph);
    char *buf = NULL; size_t blen = 0;
    FILE *o = open_memstream(&buf, &blen);
    if (!o) { fprintf(stderr, "out of memory\n"); return 1; }
    fprintf(o, "{\n  \"receipt\": \"est3c-v3\",\n  \"protocol\": \"%s\",\n  \"protocol_sha256\": \"%s\",\n", C3_PROTOCOL_DOC, C3_PROTOCOL_SHA);
    fprintf(o, "  \"tool_commit\": \"%s\",\n  \"tool_dirty\": %d,\n  \"synthetic_test\": %s,\n", TOOL_COMMIT, tool_dirty(), synth ? "true" : "false");
    fprintf(o, "  \"params_path\": "); c3_json_str(o, ppath);
    fprintf(o, ",\n  \"params_sha256\": \"%s\",\n  \"fit_raw_sha256\": \"%s\",\n  \"fit_marks_sha256\": \"%s\",\n", ph, P.fit_raw_sha, P.fit_marks_sha);
    fprintf(o, "  \"d2_dir\": "); c3_json_str(o, dir);
    fprintf(o, ",\n  \"d2_raw_sha256\": \"%s\",\n  \"d2_marks_sha256\": \"%s\",\n  \"d2_schedule_sha256\": \"%s\",\n", pre.raw_sha, pre.marks_sha, pre.sched_sha);
    fprintf(o, "  \"d2_compiled_raw_sha256\": \"%s\",\n  \"d2_compiled_marks_sha256\": \"%s\",\n  \"d2_compiled_schedule_sha256\": \"%s\",\n  \"params_compiled_sha256\": \"%s\",\n", d2_raw_sha(), d2_marks_sha(), d2_sched_sha(), params_sha());
    fprintf(o, "  \"precheck\": {\"verdict\": \"VALID\", \"lines\": %zu, \"gap_big_frac\": ", pre.lines); c3_json_num(o, pre.gap_big_frac);
    fprintf(o, ", \"foreign_mean\": "); c3_json_num(o, pre.foreign_mean);
    fprintf(o, ", \"foreign_over3_frac\": "); c3_json_num(o, pre.foreign_over3_frac);
    fprintf(o, ", \"foreign_unmeasured\": %zu, \"sched_trials\": %zu, \"loadavg1_mean\": ", pre.foreign_unmeasured, pre.sched_trials); c3_json_num(o, pre.loadavg1_mean);
    fprintf(o, "},\n");
    fprintf(o, "  \"ticks\": {\"total\": %zu, \"inserted\": %zu, \"bad_t\": %zu, \"absent_value\": %zu, \"unparsable_value\": %zu},\n",
            tk.n, tk.inserted, tk.bad_t, tk.absent_value, tk.unparsable_value);
    fprintf(o, "  \"selected\": \"%s\",\n  \"same_steps\": %s,\n  \"scores\": {\n", c3_name(P.selected), same ? "true" : "false");
    c3_json_stats(o, "S", &sS, C3_TEN_ABSENT, 0); fprintf(o, ",\n"); c3_json_stats(o, "F1", &s1, C3_TEN_ABSENT, 0); fprintf(o, ",\n"); c3_json_stats(o, "E0", &s0, C3_TEN_ABSENT, 0);
    fprintf(o, "\n  },\n  \"json_rules\": \"est-json-1\",\n  \"invalid_measurements\": %zu,\n  \"comparison\": {\"S_minus_F1\": ", inv);
    c3_json_num(o, sS.logscore - s1.logscore); fprintf(o, ", \"S_minus_E0\": "); c3_json_num(o, sS.logscore - s0.logscore);
    fprintf(o, ", \"tolerance_nats\": %.2f, \"no_worse_than_F1\": %s, \"no_worse_than_E0\": %s},\n", C3_TIE_NATS, beat1 ? "true" : "false", beat0 ? "true" : "false");
    fprintf(o, "  \"informational\": {");
    if (synth) fprintf(o, "\"skipped\": \"synthetic test\"");
    else for (int i = 0; i < 3; i++) {
        fprintf(o, "%s\n    \"%s\": {\"dir\": \"%s\", \"raw_sha256\": \"%s\", ", i ? "," : "", devn[i], dev[i], si_sha[i]);
        if (si_ok[i]) {
            fprintf(o, "\"n\": %zu, \"mean_log_score\": ", si[i].n); c3_json_num(o, si[i].logscore);
            fprintf(o, ", \"calibrated\": %s, \"first_fail\": \"%s\", \"cov95\": ", si[i].calibrated ? "true" : "false",
                    si[i].calibrated ? "none" : c3_stat_name(si[i].first_fail));
            c3_json_num(o, si[i].value[C3_ST_COV95]); fprintf(o, "}");
        }
        else { fprintf(o, "\"error\": "); c3_json_str(o, si_err[i]); fprintf(o, "}"); }
    }
    fprintf(o, "},\n  \"verdict\": \"%s\",\n  \"verdict_reason\": \"%s\",\n", verdict, reason);
    fprintf(o, "  \"first_failing_statistic\": ");
    if (sS.calibrated) fprintf(o, "null"); else c3_json_str(o, c3_stat_name(sS.first_fail));
    fprintf(o, ",\n  \"failure_class_hint\": \"%s\",\n  \"failure_class\": null\n}\n", sS.calibrated ? "none" : class_hint(sS.first_fail));
    if (fclose(o)) { free(buf); return 1; }
    uint8_t dg[32]; char hx[65];
    sha256_hash((const uint8_t *)buf, blen, dg);
    est_hex(dg, 32, hx);
    char rp[1400];
    snprintf(rp, sizeof rp, "%s/receipt-%s.json", outdir, hx);
    FILE *w = fopen(rp, "wbx");
    if (!w || fwrite(buf, 1, blen, w) != blen || fclose(w)) { fprintf(stderr, "cannot write %s: %s\n", rp, strerror(errno)); free(buf); return 1; }
    free(buf);
    printf("S=%s n=%zu logscore S %.6f F1 %.6f E0 %.6f; S calibrated %d first_fail %s; width80 mean/median S %.1f/%.1f F1 %.1f/%.1f E0 %.1f/%.1f mC\n",
           c3_name(P.selected), sS.n, sS.logscore, s1.logscore, s0.logscore, sS.calibrated,
           sS.calibrated ? "none" : c3_stat_name(sS.first_fail), sS.width_mean, sS.width_median, s1.width_mean, s1.width_median, s0.width_mean, s0.width_median);
    printf("receipt %s\n", rp);
    printf("ESTIMATION_CALIBRATION (v3) = %s%s\n", verdict, synth ? " (synthetic test, not a result)" : "");
    c3_stats_free(&sS); c3_stats_free(&s1); c3_stats_free(&s0);
    for (int i = 0; i < 3; i++) if (si_ok[i]) c3_stats_free(&si[i]);
    c3_ticks_free(&tk);
    return 0;
}

#ifdef C3_AS_LIB
#define C3_MAIN c3_eval_main
int c3_eval_main(int argc, char **argv);
#else
#define C3_MAIN main
#endif

int C3_MAIN(int argc, char **argv)
{
    const char *dir = NULL, *params = NULL, *outdir = NULL, *mkdir_s = NULL;
    int pre = 0, rec = 0, synth = 0;
    c3_synth sy = { 1, 2400, 0.0, 0, 0, 0, 0 };
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--precheck")) pre = 1;
        else if (!strcmp(argv[i], "--recorded")) rec = 1;
        else if (!strcmp(argv[i], "--synthetic-test")) synth = 1;
        else if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--params") && i + 1 < argc) params = argv[++i];
        else if (!strcmp(argv[i], "--outdir") && i + 1 < argc) outdir = argv[++i];
        else if (!strcmp(argv[i], "--make-synthetic") && i + 1 < argc) mkdir_s = argv[++i];
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) sy.seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--lines") && i + 1 < argc) sy.lines = (size_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--foreign") && i + 1 < argc) sy.foreign = atof(argv[++i]);
        else { fprintf(stderr, "usage: est3c_eval --precheck --dir D | --recorded --dir D2 --params P --outdir O [--synthetic-test] | --make-synthetic DIR [--seed N] [--lines N] [--foreign X]\n"); return 2; }
    }
    if (mkdir_s) {
        if (c3_path_is_heldout(mkdir_s) || strstr(mkdir_s, "evidence/")) { fprintf(stderr, "refuse: synthetic data never goes under evidence/\n"); return 2; }
        if (mkdir(mkdir_s, 0755) && errno != EEXIST) { fprintf(stderr, "cannot create %s\n", mkdir_s); return 1; }
        if (c3_synth_write(mkdir_s, &sy)) { fprintf(stderr, "synthetic write failed\n"); return 1; }
        printf("synthetic run written to %s (%zu lines, seed %llu)\n", mkdir_s, sy.lines, (unsigned long long)sy.seed);
        return 0;
    }
    if (pre + rec != 1 || !dir) { fprintf(stderr, "est3c_eval: exactly one of --precheck / --recorded, and --dir, are required\n"); return 2; }
    if (pre) {
        char err[512]; c3_pre p;
        if (c3_precheck(dir, &p, err, sizeof err)) { fprintf(stderr, "refuse: %s\n", err); return 2; }
        print_pre(stdout, &p);
        return p.valid ? 0 : 3;
    }
    if (!params || !outdir) { fprintf(stderr, "est3c_eval --recorded needs --params and --outdir\n"); return 2; }
    return recorded(dir, params, outdir, synth);
}
