/* Tests for the EST-3c v3 tools (est3c_fit, est3c_eval, est3c_common) on
 * synthetic data only. Built with -DC3_AS_LIB -DC3_TEST_BUILD: both tools run
 * in-process, and the dirty flag, the D1 folder and the D2 identity come from
 * the environment (C3_TEST_DIRTY, C3_TEST_D1_DIR, C3_TEST_D2_{RAW,MARKS,SCHED}_SHA),
 * as do the compiled params SHA (C3_TEST_PARAMS_SHA), the receipt folder
 * (C3_TEST_RECEIPT_DIR) and, for est3c_fit, the protocol SHA (C3_TEST_PROTOCOL_SHA).
 * Usage: test_est3c_tools <workdir>   (workdir must not exist) */
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "est3c_common.h"
#include "sha256.h"

int c3_fit_main(int argc, char **argv);
int c3_eval_main(int argc, char **argv);

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static char W[1024];

static int run(int (*fn)(int, char **), ...)
{
    char *av[32]; int n = 0; va_list ap;
    av[n++] = (char *)"tool";
    va_start(ap, fn);
    for (char *s; n < 31 && (s = va_arg(ap, char *)); ) av[n++] = s;
    va_end(ap);
    av[n] = NULL;
    fflush(stdout);
    return fn(n, av);
}

static void synth(const char *dir, uint64_t seed, size_t lines, double foreign, size_t gap_every, int drop, size_t spike)
{
    c3_synth s; memset(&s, 0, sizeof s);
    s.seed = seed; s.lines = lines; s.foreign = foreign; s.gap_every = gap_every; s.drop_end_mark = drop; s.spike_every = spike;
    if (c3_synth_write(dir, &s)) { fprintf(stderr, "synthetic write failed: %s\n", dir); exit(1); }
}

static void resums(const char *dir)
{
    static const char *const names[3] = { "machine-state.ndjson", "machine-state-marks.txt", "schedule.txt" };
    if (c3_write_sums(dir, names, 3)) { fprintf(stderr, "sums failed\n"); exit(1); }
}

static char *slurp(const char *p, size_t *len)
{
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f); b[n] = 0; if (len) *len = (size_t)n; return b;
}

/* copy src to dst replacing every line that starts with `key ` by `repl` */
static void copy_edit(const char *src, const char *dst, const char *key, const char *repl, const char *key2, const char *repl2)
{
    FILE *in = fopen(src, "r"), *out = fopen(dst, "w");
    if (!in || !out) { fprintf(stderr, "copy_edit open failed\n"); exit(1); }
    char line[8192]; size_t kl = strlen(key), k2 = key2 ? strlen(key2) : 0;
    while (fgets(line, sizeof line, in)) {
        if (!strncmp(line, key, kl) && line[kl] == ' ') fprintf(out, "%s\n", repl);
        else if (key2 && !strncmp(line, key2, k2) && line[k2] == ' ') fprintf(out, "%s\n", repl2);
        else fputs(line, out);
    }
    fclose(in); fclose(out);
}

static int count_receipts(const char *dir, char *first, size_t cap)
{
    DIR *d = opendir(dir); if (!d) return -1;
    int n = 0; struct dirent *e;
    while ((e = readdir(d))) if (!strncmp(e->d_name, "receipt-", 8)) { if (!n && first) snprintf(first, cap, "%s/%s", dir, e->d_name); n++; }
    closedir(d); return n;
}

static void path(char *out, size_t cap, const char *a) { snprintf(out, cap, "%s/%s", W, a); }

static void test_ticks(void)
{
    char d[1200], f[1300]; path(d, sizeof d, "ticks");
    synth(d, 5, 100, 0.0, 7, 0, 0);
    snprintf(f, sizeof f, "%s/machine-state.ndjson", d);
    est_file ef; char err[256];
    CHECK(est_file_load(f, &ef, err, sizeof err) == 0, "load: %s", err);
    c3_ticks tk;
    CHECK(c3_ticks_build(&ef, &tk, err, sizeof err) == 0, "ticks: %s", err);
    /* lines 7, 14, ..., 98 come after a 3 s gap: 14 gaps, 2 inserted ticks each */
    CHECK(tk.inserted == 28, "inserted %zu (want 28)", tk.inserted);
    CHECK(tk.n == 128, "ticks %zu (want 128)", tk.n);
    size_t ins_ok = 0;
    for (size_t i = 0; i + 2 < tk.n; i++)
        if (tk.t[i].inserted && tk.t[i + 1].inserted && !tk.t[i + 2].inserted && !tk.t[i].present && tk.t[i + 2].line % 7 == 0) ins_ok++;
    CHECK(ins_ok == 14, "inserted pairs before gap lines %zu (want 14)", ins_ok);
    CHECK(memcmp(tk.t[0].ev.b, tk.t[1].ev.b, sizeof tk.t[0].ev.b) != 0, "distinct evidence digests");
    c3_ticks_free(&tk); est_file_free(&ef);
}

static void pre(const char *name, uint64_t seed, size_t lines, double foreign, size_t gap_every, int drop, size_t spike, c3_pre *p)
{
    char d[1200], err[256]; path(d, sizeof d, name);
    synth(d, seed, lines, foreign, gap_every, drop, spike);
    if (c3_precheck(d, p, err, sizeof err)) { fprintf(stderr, "precheck %s: %s\n", name, err); exit(1); }
}

static void test_precheck(void)
{
    c3_pre p;
    pre("pre-ok", 3, 2100, 0.0, 0, 0, 0, &p);
    CHECK(p.valid && p.ok_marks && p.sched_trials > 0, "clean synthetic run VALID (%s)", p.why);
    CHECK(fabs(p.foreign_mean) < 0.2, "foreign mean %.3f near 0", p.foreign_mean);
    pre("pre-short", 3, 1500, 0.0, 0, 0, 0, &p);
    CHECK(!p.valid && !p.ok_lines && p.ok_gaps, "short file INCONCLUSIVE on lines only (%s)", p.why);
    pre("pre-gaps", 3, 2100, 0.0, 20, 0, 0, &p);
    CHECK(!p.valid && !p.ok_gaps && p.ok_lines && p.ok_foreign_mean, "5 %% gaps INCONCLUSIVE on gaps (%s; %.4f)", p.why, p.gap_big_frac);
    pre("pre-gaps-ok", 3, 2100, 0.0, 100, 0, 0, &p);
    CHECK(p.valid && p.gaps_big > 0, "1 %% gaps stays VALID (%s)", p.why);
    pre("pre-dropend", 3, 2100, 0.0, 0, 1, 0, &p);
    CHECK(!p.valid && !p.ok_marks, "dropped end mark INCONCLUSIVE (%s)", p.why);
    /* an extra begin/end pair */
    char d[1200], m[1300]; path(d, sizeof d, "pre-extra");
    synth(d, 3, 2100, 0.0, 0, 0, 0);
    snprintf(m, sizeof m, "%s/machine-state-marks.txt", d);
    FILE *f = fopen(m, "a"); fprintf(f, "1790009999.000000000 begin load-L6-999\n1790010000.000000000 end load-L6-999 exit 0\n"); fclose(f);
    resums(d);
    char err[256];
    CHECK(c3_precheck(d, &p, err, sizeof err) == 0 && !p.valid && !p.ok_marks, "extra marks pair INCONCLUSIVE (%s)", p.why);
    /* a missing pair: drop the first begin and end lines */
    path(d, sizeof d, "pre-missing");
    synth(d, 3, 2100, 0.0, 0, 0, 0);
    snprintf(m, sizeof m, "%s/machine-state-marks.txt", d);
    char *b = slurp(m, NULL); char *nl = b ? strchr(b, '\n') : NULL; nl = nl ? strchr(nl + 1, '\n') : NULL;
    if (nl) { f = fopen(m, "w"); fputs(nl + 1, f); fclose(f); }
    free(b); resums(d);
    CHECK(c3_precheck(d, &p, err, sizeof err) == 0 && !p.valid && !p.ok_marks, "missing marks pair INCONCLUSIVE (%s)", p.why);
    pre("pre-foreign", 3, 2100, 2.0, 0, 0, 0, &p);
    CHECK(!p.valid && !p.ok_foreign_mean, "foreign 2.0 INCONCLUSIVE on mean (%s; %.3f)", p.why, p.foreign_mean);
    pre("pre-foreign-ok", 3, 2100, 1.2, 0, 0, 0, &p);
    CHECK(p.valid, "foreign 1.2 stays VALID (%s; %.3f)", p.why, p.foreign_mean);
    pre("pre-tail", 3, 2100, 0.0, 0, 0, 5, &p);
    CHECK(!p.valid && p.ok_foreign_mean && !p.ok_foreign_tail, "spikes INCONCLUSIVE on tail only (%s; mean %.3f tail %.3f)", p.why, p.foreign_mean, p.foreign_over3_frac);
}

static void test_select(void)
{
    int av[6] = { 0, 1, 1, 1, 1, 1 }, ps[6] = { 0, 1, 1, 1, 1, 1 };
    double ls[6] = { 0, -1.000, -1.000, -0.995, -1.200, -0.990 };
    CHECK(c3_select(av, ps, ls) == 2 - 1, "F1 within 0.01 of best F5 is preferred (got %d)", c3_select(av, ps, ls));
    ps[1] = 0;
    CHECK(c3_select(av, ps, ls) == 2, "F2 next earliest within 0.01 (got %d)", c3_select(av, ps, ls));
    ls[2] = -1.0105; ls[3] = -1.02;
    CHECK(c3_select(av, ps, ls) == 5, "best F5 wins when no earlier family is within 0.01 (got %d)", c3_select(av, ps, ls));
    ps[5] = 0;
    CHECK(c3_select(av, ps, ls) == 2, "non-passers are ignored (got %d)", c3_select(av, ps, ls));
    av[2] = 0; av[3] = 0; av[4] = 0;
    CHECK(c3_select(av, ps, ls) == 0, "no passer: PHASE_A_FAIL (0)");
    CHECK(c3_grid_take(0, -2.0, 0.0) && !c3_grid_take(1, -1.0, -1.0) && c3_grid_take(1, -0.9, -1.0) && !c3_grid_take(0, NAN, 0.0),
          "grid tie keeps the first point; non-finite points are skipped");
}

static void test_e0(void)
{
    uint32_t c[EST_PRED_N]; memset(c, 0, sizeof c);
    double e[10] = { 0, 0, 0, 0, 0, 0, 100, 100, -100, 90000 };
    c3_e0_counts(e, 10, c);
    CHECK(c[EST_PRED_K] == 6 && c[EST_PRED_K + 1] == 2 && c[EST_PRED_K - 1] == 1 && c[EST_PRED_N - 1] == 1, "E0 counts, outside value clipped to the edge bin");
    static c3_model m;
    c3_make_e0(&m, c, 10);
    double s = 0; for (int i = 0; i < EST_PRED_N; i++) s += m.e0_one[i];
    CHECK(fabs(s - 1.0) < 1e-12, "E0 pmf sums to 1 (%.17g)", s);
    CHECK(fabs(m.e0_one[EST_PRED_K] - (6.0 + 1.0 / 801.0) / 11.0) < 1e-15 && fabs(m.e0_one[EST_PRED_K + 5] - (1.0 / 801.0) / 11.0) < 1e-15, "E0 one-pseudo-observation smoothing (c+1/801)/(N+1)");
    CHECK(m.id == 0, "E0 id");
}

static void hexof(const char *dir, const char *name, char out[65])
{
    char p[1300]; snprintf(p, sizeof p, "%s/%s", dir, name);
    if (est_sha_file_hex(p, out)) { fprintf(stderr, "sha %s\n", p); exit(1); }
}

/* the compiled params SHA stand-in: the SHA-256 of p */
static void use_params(const char *p)
{
    char h[65];
    if (est_sha_file_hex(p, h)) { fprintf(stderr, "sha %s\n", p); exit(1); }
    setenv("C3_TEST_PARAMS_SHA", h, 1);
}

static void test_tools(void)
{
    char d1[1200], d2[1200], dh[1200], d2s[1200], raw1[1300], mk1[1300], raw2[1300], mk2[1300], rawh[1300], mkh[1300];
    char psyn[1200], pbind[1200], pfail[1200], pbad[1200], psel[1200], pscr[1200], pdup[1200], o1[1200], o2[1200], o3[1200];
    path(d1, sizeof d1, C3_D1_ID); path(d2, sizeof d2, "run-d2"); path(dh, sizeof dh, "x" C3_HELDOUT_TAG "1"); path(d2s, sizeof d2s, "run-d2-short");
    synth(d1, 11, 2300, 0.0, 0, 0, 0); synth(d2, 12, 2300, 0.0, 0, 0, 0); synth(dh, 13, 2100, 0.0, 0, 0, 0); synth(d2s, 14, 1200, 0.0, 0, 0, 0);
    snprintf(raw1, sizeof raw1, "%s/machine-state.ndjson", d1); snprintf(mk1, sizeof mk1, "%s/machine-state-marks.txt", d1);
    snprintf(raw2, sizeof raw2, "%s/machine-state.ndjson", d2); snprintf(mk2, sizeof mk2, "%s/machine-state-marks.txt", d2);
    snprintf(rawh, sizeof rawh, "%s/machine-state.ndjson", dh); snprintf(mkh, sizeof mkh, "%s/machine-state-marks.txt", dh);
    path(psyn, sizeof psyn, "params-syn.txt"); path(pbind, sizeof pbind, "params-bind.txt");
    path(pfail, sizeof pfail, "params-fail.txt"); path(pbad, sizeof pbad, "params-bad.txt");
    path(psel, sizeof psel, "params-sel.txt"); path(pscr, sizeof pscr, "params-scr.txt"); path(pdup, sizeof pdup, "params-dup.txt");
    path(o1, sizeof o1, "out1"); path(o2, sizeof o2, "out2"); path(o3, sizeof o3, "out3");
    mkdir(o1, 0755); mkdir(o2, 0755); mkdir(o3, 0755);

    /* fit guards */
    setenv("C3_TEST_DIRTY", "1", 1);
    CHECK(run(c3_fit_main, "--raw", raw1, "--marks", mk1, "--out", pbind, (char *)NULL) == 2, "fit refuses a dirty tree");
    CHECK(run(c3_fit_main, "--raw", rawh, "--marks", mkh, "--out", psyn, "--synthetic-test", (char *)NULL) == 2, "fit refuses a held-out path even in synthetic mode");
    setenv("C3_TEST_DIRTY", "0", 1);
    CHECK(run(c3_fit_main, "--raw", raw2, "--marks", mk2, "--out", pbind, (char *)NULL) == 2, "binding fit refuses data outside D1");
    setenv("C3_TEST_PROTOCOL_SHA", "unknown", 1);
    CHECK(run(c3_fit_main, "--raw", raw1, "--marks", mk1, "--out", pbind, "--grid-stride", "4", (char *)NULL) == 2, "binding fit refuses protocol SHA unknown");
    setenv("C3_TEST_PROTOCOL_SHA", "1111111111111111111111111111111111111111111111111111111111111111", 1);
    CHECK(run(c3_fit_main, "--raw", raw1, "--marks", mk1, "--out", pbind, "--grid-stride", "4", (char *)NULL) == 2, "binding fit refuses a compiled protocol SHA that differs from the doc on disk");
    unsetenv("C3_TEST_PROTOCOL_SHA");
    { struct stat sb; CHECK(stat(pbind, &sb) != 0, "refused binding fits write no params file"); }
    CHECK(run(c3_fit_main, "--raw", raw1, "--marks", mk1, "--out", psyn, "--synthetic-test", "--grid-stride", "4", (char *)NULL) == 0, "synthetic fit runs");
    CHECK(run(c3_fit_main, "--raw", raw1, "--marks", mk1, "--out", psyn, "--synthetic-test", "--grid-stride", "4", (char *)NULL) == 1, "fit never overwrites params");
    CHECK(run(c3_fit_main, "--raw", raw1, "--marks", mk1, "--out", pbind, "--grid-stride", "4", (char *)NULL) == 0, "binding-mode fit on the fake D1 runs");
    c3_params P; char err[512];
    CHECK(c3_params_read(pbind, &P, err, sizeof err) == 0, "params read: %s", err);
    CHECK(P.synthetic == 0 && P.tool_dirty == 0 && P.e0_n > 2000, "params identity (synthetic %d dirty %d e0_n %llu)", P.synthetic, P.tool_dirty, (unsigned long long)P.e0_n);
    CHECK(P.phase_a_pass && P.selected >= 1, "synthetic iid data passes the in-sample screen (selected %d)", P.selected);
    char h[65]; hexof(d1, "machine-state.ndjson", h);
    CHECK(!strcmp(h, P.fit_raw_sha), "params record the D1 raw SHA");
    copy_edit(pbind, pfail, "phase_a", "phase_a PHASE_A_FAIL", "selected", "selected none");

    /* eval refusals; C3_TEST_PARAMS_SHA / C3_TEST_RECEIPT_DIR stand in for the compiled params SHA and receipt folder */
    char r2[65], m2[65], s2[65]; hexof(d2, "machine-state.ndjson", r2); hexof(d2, "machine-state-marks.txt", m2); hexof(d2, "schedule.txt", s2);
    setenv("C3_TEST_D1_DIR", d1, 1); setenv("C3_TEST_D2_RAW_SHA", r2, 1); setenv("C3_TEST_D2_MARKS_SHA", m2, 1); setenv("C3_TEST_D2_SCHED_SHA", s2, 1);
    setenv("C3_TEST_RECEIPT_DIR", o1, 1); use_params(pbind);
    setenv("C3_TEST_DIRTY", "1", 1);
    CHECK(run(c3_eval_main, "--recorded", "--dir", d2, "--params", pbind, "--outdir", o1, (char *)NULL) == 2, "eval refuses a dirty tree");
    setenv("C3_TEST_DIRTY", "0", 1);
    setenv("C3_TEST_D2_RAW_SHA", "absent", 1);
    CHECK(run(c3_eval_main, "--recorded", "--dir", d2, "--params", pbind, "--outdir", o1, (char *)NULL) == 2, "eval refuses without a compiled D2 identity");
    setenv("C3_TEST_D2_RAW_SHA", r2, 1);
    setenv("C3_TEST_D2_SCHED_SHA", "absent", 1);
    CHECK(run(c3_eval_main, "--recorded", "--dir", d2, "--params", pbind, "--outdir", o1, (char *)NULL) == 2, "eval refuses without a compiled D2 schedule SHA");
    setenv("C3_TEST_D2_SCHED_SHA", "2222222222222222222222222222222222222222222222222222222222222222", 1);
    CHECK(run(c3_eval_main, "--recorded", "--dir", d2, "--params", pbind, "--outdir", o1, (char *)NULL) == 2, "eval refuses a D2 schedule.txt with another SHA");
    setenv("C3_TEST_D2_SCHED_SHA", s2, 1);
    setenv("C3_TEST_PARAMS_SHA", "absent", 1);
    CHECK(run(c3_eval_main, "--recorded", "--dir", d2, "--params", pbind, "--outdir", o1, (char *)NULL) == 2, "eval refuses without a compiled params SHA");
    use_params(psyn);
    CHECK(run(c3_eval_main, "--recorded", "--dir", d2, "--params", pbind, "--outdir", o1, (char *)NULL) == 2, "eval refuses a params file whose SHA differs from the compiled one");
    use_params(pbind);
    CHECK(run(c3_eval_main, "--recorded", "--dir", d2, "--params", pbind, "--outdir", o2, (char *)NULL) == 2, "eval refuses an outdir other than the receipt folder");
    setenv("C3_TEST_RECEIPT_DIR", "/nonexistent-est3c-receipts", 1);
    CHECK(run(c3_eval_main, "--recorded", "--dir", d2, "--params", pbind, "--outdir", o1, (char *)NULL) == 2, "eval refuses when the receipt folder does not resolve");
    setenv("C3_TEST_RECEIPT_DIR", o1, 1);
    /* selection must be reproduced from the params' own screen results */
    char sel_other[32], scr_line[64], scr_key[32];
    snprintf(sel_other, sizeof sel_other, "selected F%d", P.selected % 5 + 1);
    copy_edit(pbind, psel, "selected", sel_other, NULL, NULL);
    use_params(psel);
    CHECK(run(c3_eval_main, "--recorded", "--dir", d2, "--params", psel, "--outdir", o1, (char *)NULL) == 2, "eval refuses an edited `selected` (%s)", sel_other);
    snprintf(scr_key, sizeof scr_key, "screen_result F%d", P.selected);
    snprintf(scr_line, sizeof scr_line, "screen_result F%d pass 0 first_fail coverage50 n 0 logscore 0", P.selected);
    copy_edit(pbind, pscr, scr_key, scr_line, NULL, NULL);
    use_params(pscr);
    CHECK(run(c3_eval_main, "--recorded", "--dir", d2, "--params", pscr, "--outdir", o1, (char *)NULL) == 2, "eval refuses a selected family that did not pass the screen");
    use_params(psyn);
    CHECK(run(c3_eval_main, "--recorded", "--dir", d2, "--params", psyn, "--outdir", o1, (char *)NULL) == 2, "eval refuses synthetic params");
    use_params(pfail);
    CHECK(run(c3_eval_main, "--recorded", "--dir", d2, "--params", pfail, "--outdir", o1, (char *)NULL) == 2, "eval refuses PHASE_A_FAIL params");
    use_params(pbind);
    setenv("C3_TEST_D1_DIR", d2, 1);
    CHECK(run(c3_eval_main, "--recorded", "--dir", d2, "--params", pbind, "--outdir", o1, (char *)NULL) == 2, "eval refuses params whose D1 SHA differs");
    setenv("C3_TEST_D1_DIR", d1, 1);
    setenv("C3_TEST_D2_RAW_SHA", "0000000000000000000000000000000000000000000000000000000000000000", 1);
    CHECK(run(c3_eval_main, "--recorded", "--dir", d2, "--params", pbind, "--outdir", o1, (char *)NULL) == 2, "eval refuses D2 data with another SHA");
    setenv("C3_TEST_D2_RAW_SHA", r2, 1);
    CHECK(run(c3_eval_main, "--recorded", "--dir", d2s, "--params", pbind, "--outdir", o1, (char *)NULL) == 3, "eval refuses an INCONCLUSIVE D2");
    CHECK(count_receipts(o1, NULL, 0) == 0, "no receipt after refusals");

    /* the one scored run, then the second-run refusals (same folder, another folder) */
    CHECK(run(c3_eval_main, "--recorded", "--dir", d2, "--params", pbind, "--outdir", o1, (char *)NULL) == 0, "recorded run on synthetic D2");
    char rp[1400] = "";
    CHECK(count_receipts(o1, rp, sizeof rp) == 1, "exactly one receipt");
    size_t rl = 0; char *rc = slurp(rp, &rl);
    CHECK(rc && strstr(rc, "\"verdict\": \"") && strstr(rc, "\"protocol_sha256\": \"" C3_PROTOCOL_SHA "\""), "receipt has verdict and protocol SHA");
    CHECK(rc && strstr(rc, "\"E0\"") && strstr(rc, "\"F1\"") && strstr(rc, "\"informational\""), "receipt scores S, F1, E0 and the informational block");
    if (rc) {
        uint8_t dg[32]; char hx[65]; sha256_hash((const uint8_t *)rc, rl, dg); est_hex(dg, 32, hx);
        CHECK(strstr(rp, hx) != NULL, "receipt is named by its SHA-256");
    }
    free(rc);
    CHECK(run(c3_eval_main, "--recorded", "--dir", d2, "--params", pbind, "--outdir", o1, (char *)NULL) == 2, "a second scored run is refused");
    CHECK(run(c3_eval_main, "--recorded", "--dir", d2, "--params", pbind, "--outdir", o3, (char *)NULL) == 2, "a second scored run into another folder is refused");
    CHECK(count_receipts(o3, NULL, 0) == 0 && count_receipts(o1, NULL, 0) == 1, "still exactly one recorded receipt");
    /* synthetic-test path */
    CHECK(run(c3_eval_main, "--recorded", "--synthetic-test", "--dir", d2, "--params", psyn, "--outdir", o2, (char *)NULL) == 0, "synthetic recorded run");
    CHECK(run(c3_eval_main, "--recorded", "--synthetic-test", "--dir", d1, "--params", psyn, "--outdir", o3, (char *)NULL) == 2, "synthetic mode refuses D1");
    CHECK(run(c3_eval_main, "--recorded", "--synthetic-test", "--dir", d2, "--params", psel, "--outdir", o3, (char *)NULL) == 2, "synthetic mode still refuses an edited `selected`");
    CHECK(run(c3_eval_main, "--precheck", "--dir", d2s, (char *)NULL) == 3, "precheck exit 3 on INCONCLUSIVE");
    copy_edit(pbind, pbad, "protocol_doc_sha256", "protocol_doc_sha256 1111111111111111111111111111111111111111111111111111111111111111", NULL, NULL);
    use_params(pbad); setenv("C3_TEST_RECEIPT_DIR", o3, 1);
    CHECK(run(c3_eval_main, "--recorded", "--dir", d2, "--params", pbad, "--outdir", o3, (char *)NULL) == 2, "eval refuses params from another protocol");

    /* parameter file: duplicate keys refused, distinct e0_count k accepted */
    FILE *pf;
    pf = fopen(pdup, "w"); { size_t bl; char *b = slurp(pbind, &bl); if (b) { fputs(b, pf); free(b); } } fprintf(pf, "%s\n", sel_other); fclose(pf);
    CHECK(c3_params_read(pdup, &P, err, sizeof err) != 0 && strstr(err, "duplicate"), "params with two selected lines refused (%s)", err);
    pf = fopen(pdup, "w"); { size_t bl; char *b = slurp(pbind, &bl); if (b) { fputs(b, pf); free(b); } } fprintf(pf, "e0_count 0 0\n"); err[0] = 0; fclose(pf);
    CHECK(c3_params_read(pbind, &P, err, sizeof err) == 0, "params with many distinct e0_count lines read: %s", err);
    CHECK(c3_params_read(pdup, &P, err, sizeof err) != 0 && strstr(err, "duplicate"), "params with a repeated e0_count k refused (%s)", err);
    pf = fopen(pdup, "w"); { size_t bl; char *b = slurp(pbind, &bl); if (b) { fputs(b, pf); free(b); } } fprintf(pf, "screen_result F1 pass 1\n"); fclose(pf);
    CHECK(c3_params_read(pdup, &P, err, sizeof err) != 0 && strstr(err, "duplicate"), "params with a repeated screen_result refused (%s)", err);
}

/* section 6 rule 5: when both regimes fail, in-trial is named first */
static void test_rule_order(void)
{
    static c3_stats st; memset(&st, 0, sizeof st);
    size_t n = 400;
    st.n = n; st.cov95_step = calloc(n, sizeof(double)); st.width_step = calloc(n, sizeof(double)); st.regime_step = calloc(n, 1);
    if (!st.cov95_step || !st.width_step || !st.regime_step) { fprintf(stderr, "oom\n"); exit(1); }
    st.c.n = n; st.c.cov[0] = 0.50 * n; st.c.cov[1] = 0.80 * n; st.c.cov[2] = 0.95 * n;
    for (int j = 0; j < 10; j++) st.c.pit[j] = 0.1 * n;
    st.c.z_sum = 0; st.c.z_sq = (double)n; st.c.z_lag = 0;
    for (size_t k = 0; k < n; k++) { st.regime_step[k] = (unsigned char)(k & 1); st.cov95_step[k] = (k & 1) ? 1.0 : 0.82; }
    st.ten_n = 100; st.ten_cov = 95;
    c3_judge(&st);
    CHECK(!st.pass[C3_ST_REG_TRIAL] && !st.pass[C3_ST_REG_IDLE] && st.pass[C3_ST_Q0] && st.pass[C3_ST_LAG1], "both regimes fail, quarters pass");
    CHECK(st.first_fail == C3_ST_REG_TRIAL && !strcmp(c3_stat_name(st.first_fail), "regime_trial_cov95"), "first failure is in-trial (got %s)", st.first_fail >= 0 ? c3_stat_name(st.first_fail) : "none");
    c3_stats_free(&st);
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: test_est3c_tools <workdir>\n"); return 2; }
    snprintf(W, sizeof W, "%s", argv[1]);
    if (mkdir(W, 0755)) { fprintf(stderr, "workdir %s: %s\n", W, strerror(errno)); return 2; }
    test_ticks(); printf("ticks: done\n");
    test_precheck(); printf("precheck: done\n");
    test_select(); printf("selection: done\n");
    test_e0(); printf("E0: done\n");
    test_rule_order(); printf("rule order: done\n");
    test_tools(); printf("tools: done\n");
    printf("test_est3c_tools: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
