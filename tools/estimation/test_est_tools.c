/* Tests for est_replay / est_fit / est_eval (protocol v1 and v2). Run A and synthetic
 * data only; this program never names run B's data file.
 *   test_est_tools <est_fit> <est_eval> <workdir> */
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "est_replay.h"
#include "test_est_ka.inc"

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define RUN_A_DIR "evidence/R15/raw/" EST_RUN_A_ID
static char *g_fit, *g_eval, *g_evalt, *g_work;

static uint64_t g_rng;
static uint64_t rnd(void) { g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27; return g_rng * 0x2545F4914F6CDD1Dull; }
static double unif(void) { return ((double)(rnd() >> 11) + 0.5) / 9007199254740992.0; }
static double gauss(void) { return sqrt(-2.0 * log(unif())) * cos(6.283185307179586 * unif()); }

static void sh(const char *fmt, const char *a, const char *b, const char *c, const char *d)
{
    char cmd[16384]; snprintf(cmd, sizeof cmd, fmt, a, b, c, d);
    int rc = system(cmd); (void)rc;
}
static int sh_rc(const char *fmt, const char *a, const char *b, const char *c, const char *d)
{
    char cmd[16384]; snprintf(cmd, sizeof cmd, fmt, a, b, c, d);
    return system(cmd);
}

static char *slurp(const char *path)
{
    FILE *fp = fopen(path, "rb"); if (!fp) return NULL;
    char *buf = malloc(1 << 20); size_t n = fread(buf, 1, (1 << 20) - 1, fp); buf[n] = 0; fclose(fp); return buf;
}

/* Random-walk + noise series with known q, r; optional damage. Writes the
 * dir with machine-state.ndjson, machine-state-marks.txt and SHA256SUMS. */
static void make_series(const char *dir, size_t n, double q, double r, uint64_t seed, int damage)
{
    char p[1024];
    sh("rm -rf %s && mkdir -p %s", dir, dir, "", "");
    snprintf(p, sizeof p, "%s/machine-state.ndjson", dir);
    FILE *fp = fopen(p, "wb");
    g_rng = seed; double x = 45000.0; long long wall = 1790000000ll; int extra = 0;
    for (size_t i = 0; i < n; i++) {
        x += sqrt(q) * gauss();
        double z = floor(x + sqrt(r) * gauss() + 0.5);
        if (damage && i == 100) extra = 2;            /* a 3 s gap */
        long long t = wall + (long long)i + extra;
        int jit = (int)(rnd() % 3000000);             /* 0..3 ms jitter */
        if (damage && i == 0) fprintf(fp, "garbage line\n");
        else if (damage && i == 50) fprintf(fp, "{\"t\":%lld.%09d,\"thermal_mc\":\"nan 1 2 \"}\n", t, jit);
        else if (damage && i == 51) fprintf(fp, "{\"t\":%lld.%09d,\"spbm_uj\":1}\n", t, jit);
        else if (damage && i == 52) fprintf(fp, "not json at all\n");
        else fprintf(fp, "{\"t\":%lld.%09d,\"spbm_uj\":%zu,\"thermal_mc\":\"%.0f 30000 \",\"gpu\":\"x\"}\n", t, jit, i, z);
    }
    fclose(fp);
    snprintf(p, sizeof p, "%s/machine-state-marks.txt", dir);
    fp = fopen(p, "wb");
    fprintf(fp, "%lld.000000001 begin trial-1.jsonl\n%lld.000000001 end trial-1.jsonl exit 0\n",
            wall + (long long)(n / 5), wall + (long long)(n * 3 / 5));
    fclose(fp);
    char h1[65], h2[65], a[1024], b[1024];
    snprintf(a, sizeof a, "%s/machine-state.ndjson", dir); snprintf(b, sizeof b, "%s/machine-state-marks.txt", dir);
    est_sha_file_hex(a, h1); est_sha_file_hex(b, h2);
    snprintf(p, sizeof p, "%s/SHA256SUMS", dir);
    fp = fopen(p, "wb");
    fprintf(fp, "%s  machine-state.ndjson\n%s  machine-state-marks.txt\n", h1, h2);
    fclose(fp);
}

static int step_bits_equal(const est_step *a, const est_step *b)
{
    return a->line == b->line && a->coast == b->coast && a->t_ns == b->t_ns && a->horizon == b->horizon
        && memcmp(&a->z, &b->z, 8) == 0 && memcmp(&a->y_mean, &b->y_mean, 8) == 0
        && memcmp(&a->S, &b->S, 8) == 0 && memcmp(&a->nu, &b->nu, 8) == 0 && memcmp(&a->nis, &b->nis, 8) == 0
        && memcmp(&a->obs_d, &b->obs_d, 32) == 0 && memcmp(&a->pred_d, &b->pred_d, 32) == 0
        && memcmp(&a->innov_d, &b->innov_d, 32) == 0 && memcmp(&a->belief_d, &b->belief_d, 32) == 0
        && memcmp(a->post.x, b->post.x, sizeof a->post.x) == 0 && memcmp(a->post.P, b->post.P, sizeof a->post.P) == 0;
}

static void test_prefix_run_a(void)
{
    est_file f; char err[300];
    CHECK(est_file_load(RUN_A_DIR "/machine-state.ndjson", &f, err, sizeof err) == 0);
    CHECK(strcmp(f.sha_hex, EST_RUN_A_SHA) == 0);
    static est_replay full, pre;
    size_t ks[3] = { 100, 500, 1000 };
    int all = 1;
    for (int m = 0; m < 2; m++) {
        CHECK(est_replay_run(&f, (size_t)-1, m, 100.0, 10000.0, &full) == 0);
        CHECK(full.nsteps + full.leading_missing == f.nlines);
        for (int ki = 0; ki < 3; ki++) {
            CHECK(est_replay_run(&f, ks[ki], m, 100.0, 10000.0, &pre) == 0);
            CHECK(pre.nsteps == ks[ki] - pre.leading_missing);
            for (size_t i = 0; i < pre.nsteps; i++) if (!step_bits_equal(&pre.steps[i], &full.steps[i])) all = 0;
        }
        for (size_t i = 0; i < full.nsteps; i++) CHECK(full.steps[i].chain_ok);
    }
    CHECK(all);
    printf("test_est_tools: prefix invariance on run A, k=100,500,1000, M0 and M1: %s\n", all ? "PASS" : "FAIL");
    est_replay_free(&full); est_replay_free(&pre); est_file_free(&f);
}

static void test_coasting(void)
{
    char dir[1024]; snprintf(dir, sizeof dir, "%s/coast", g_work);
    make_series(dir, 300, 30.0, 3000.0, 7, 1);
    char path[1100]; snprintf(path, sizeof path, "%s/machine-state.ndjson", dir);
    est_file f; char err[300];
    CHECK(est_file_load(path, &f, err, sizeof err) == 0);
    static est_replay rp;
    CHECK(est_replay_run(&f, (size_t)-1, 0, 30.0, 3000.0, &rp) == 0);
    CHECK(rp.leading_missing == 1);
    CHECK(rp.coasts == 3);
    CHECK(rp.bad_value == 4);
    CHECK(rp.nsteps == 299);
    /* steps: index = line - 1; lines 50,51,52 coast */
    CHECK(rp.steps[49].coast && rp.steps[50].coast && rp.steps[51].coast);
    CHECK(!rp.steps[48].coast && !rp.steps[52].coast);
    CHECK(rp.steps[52].horizon == 1);
    CHECK(rp.steps[98].horizon == 1 && rp.steps[99].horizon == 3);
    CHECK(rp.steps[99].t_ns == rp.steps[98].t_ns + 3000000000ll);
    /* coasting inflates uncertainty: variance after 3 coasts exceeds before */
    CHECK(rp.steps[51].post.P[0] > rp.steps[48].post.P[0]);
    CHECK(est_digest_is_zero(&rp.steps[50].obs_d) && !est_digest_is_zero(&rp.steps[50].pred_d));
    CHECK(rp.steps[50].chain_ok);
    printf("test_est_tools: missing-value coasting: leading=%zu coasted=%zu steps=%zu PASS\n", rp.leading_missing, rp.coasts, rp.nsteps);
    est_replay_free(&rp); est_file_free(&f);
}

static void test_sha_refusal(void)
{
    char dir[1024]; snprintf(dir, sizeof dir, "%s/sha", g_work);
    make_series(dir, 200, 30.0, 3000.0, 11, 0);
    char path[1100]; snprintf(path, sizeof path, "%s/machine-state.ndjson", dir);
    est_file f; char err[300] = "";
    CHECK(est_file_load(path, &f, err, sizeof err) == 0);
    est_file_free(&f);
    FILE *fp = fopen(path, "r+b"); fseek(fp, 20, SEEK_SET); int c = fgetc(fp); fseek(fp, 20, SEEK_SET);
    fputc(c == '5' ? '6' : '5', fp); fclose(fp);
    CHECK(est_file_load(path, &f, err, sizeof err) != 0);
    CHECK(strstr(err, "mismatch") != NULL);
    char out[1100], par[1100];
    snprintf(out, sizeof out, "%s/sha/params", g_work);
    CHECK(sh_rc("%s --synthetic-test %s %s >/dev/null 2>&1", g_fit, path, out, "") != 0);
    /* a file with no SHA256SUMS line is refused too */
    snprintf(par, sizeof par, "%s/sha/other.ndjson", g_work);
    sh("cp %s %s", path, par, "", "");
    CHECK(est_file_load(par, &f, err, sizeof err) != 0);
    printf("test_est_tools: SHA-256 mismatch and missing-SUMS refusal (%s) PASS\n", err);
}

static double param_val(const char *txt, const char *tag, const char *key)
{
    const char *p = strstr(txt, tag); if (!p) return NAN;
    p = strstr(p, key); if (!p) return NAN;
    return atof(p + strlen(key));
}

static void test_synthetic(void)
{
    char d1[1024], d2[1024], par[1100], rec[1100], f1[1100], f2[1100], m2[1100], cmd[16384];
    snprintf(d1, sizeof d1, "%s/synth-fit", g_work); snprintf(d2, sizeof d2, "%s/synth-eval", g_work);
    double q = pow(10.0, 1.5), r = pow(10.0, 3.5);
    make_series(d1, 5000, q, r, 101, 0);
    make_series(d2, 4000, q, r, 202, 0);
    snprintf(f1, sizeof f1, "%s/machine-state.ndjson", d1); snprintf(f2, sizeof f2, "%s/machine-state.ndjson", d2);
    snprintf(m2, sizeof m2, "%s/machine-state-marks.txt", d2);
    snprintf(par, sizeof par, "%s/params.txt", d1); snprintf(rec, sizeof rec, "%s/receipt.json", d2);
    CHECK(sh_rc("%s --synthetic-test %s %s >/dev/null 2>&1", g_fit, f1, par, "") == 0);
    est_params p; CHECK(est_params_read(par, &p) == 0);
    double dq = fabs(log10(p.q[0]) - 1.5), dr = fabs(log10(p.r[0]) - 3.5);
    CHECK(dq <= 0.5 + 1e-9);
    CHECK(dr <= 0.25 + 1e-9);
    printf("test_est_tools: synthetic fit M0 true q=%.4g r=%.4g, fitted q=%.4g r=%.4g (grid steps %.1f, %.1f) %s\n",
           q, r, p.q[0], p.r[0], dq / 0.5, dr / 0.25, (dq <= 0.5 + 1e-9 && dr <= 0.25 + 1e-9) ? "PASS" : "FAIL");
    /* eval on an independent series with the same true q, r */
    snprintf(cmd, sizeof cmd, "%s --params %s --raw %s --marks %s --out %s --synthetic-test >%s/eval.out 2>&1",
             g_eval, par, f2, m2, rec, d2);
    CHECK(system(cmd) == 0);
    char *js = slurp(rec); CHECK(js != NULL);
    if (js) {
        const char *m0 = strstr(js, "\"M0\": {");
        CHECK(m0 != NULL);
        const char *cal = m0 ? strstr(m0, "\"calibrated\":") : NULL;
        CHECK(cal && strncmp(cal + 14, "true", 4) == 0);
        CHECK(strstr(js, "\"ESTIMATION_REAL_SIGNAL\": \"PASS\"") != NULL);
        CHECK(strstr(js, "\"ESTIMATION_CALIBRATION\": \"PASS\"") != NULL);
        CHECK(strstr(js, "\"selected_model\": \"M0\"") != NULL);
        CHECK(strstr(js, "\"recorded\": false") != NULL);
        CHECK(strstr(js, "\"protocol_commit\": \"" EST_PROTOCOL_COMMIT "\"") != NULL);
        double c95 = m0 ? param_val(m0, "coverage_95", "\"value\": ") : NAN;
        printf("test_est_tools: synthetic eval, right model: 95%% coverage %.4f, M0 calibrated PASS\n", c95);
        free(js);
    }
    /* (c) deliberately wrong observation noise: r far too small */
    char bad[1100]; snprintf(bad, sizeof bad, "%s/params-bad.txt", d1);
    p.r[0] = 1.0; p.r[1] = 1.0;
    CHECK(est_params_write(bad, &p) == 0);
    snprintf(rec, sizeof rec, "%s/receipt-bad.json", d2);
    snprintf(cmd, sizeof cmd, "%s --params %s --raw %s --marks %s --out %s --synthetic-test >%s/eval-bad.out 2>&1",
             g_eval, bad, f2, m2, rec, d2);
    CHECK(system(cmd) == 0);
    js = slurp(rec); CHECK(js != NULL);
    if (js) {
        const char *m0 = strstr(js, "\"M0\": {");
        const char *cal = m0 ? strstr(m0, "\"calibrated\":") : NULL;
        CHECK(cal && strncmp(cal + 14, "false", 5) == 0);
        CHECK(strstr(js, "\"ESTIMATION_CALIBRATION\": \"FAIL\"") != NULL);
        CHECK(strstr(js, "\"failed_criteria\": [\"coverage_50\"") != NULL);
        printf("test_est_tools: synthetic eval, wrong r: ESTIMATION_CALIBRATION FAIL as expected, M0 95%% coverage %.4f\n",
               m0 ? param_val(m0, "coverage_95", "\"value\": ") : NAN);
        free(js);
    }
    /* eval refuses non-run-A parameters without --synthetic-test */
    snprintf(cmd, sizeof cmd, "%s --params %s --raw %s --marks %s --out %s/x.json >/dev/null 2>&1", g_eval, par, f2, m2, d2);
    CHECK(system(cmd) != 0);
}


/* value of the first number after `key` following `scope` (or from the start) */
static double jv(const char *js, const char *scope, const char *key)
{
    const char *p = scope ? strstr(js, scope) : js; if (!p) return NAN;
    p = strstr(p, key); if (!p) return NAN;
    return atof(p + strlen(key));
}
static int near(double a, double b) { return fabs(a - b) <= 1e-9 * (1.0 + fabs(b)); }

static void test_run_a_and_flags(void)
{
    char par[4096], rec[4096], cmd[16384], fake[4096];
    snprintf(par, sizeof par, "%s/paramsA.txt", g_work);
    snprintf(cmd, sizeof cmd, "%s %s %s > %s/fitA.out 2> %s/fitA.err", g_fit, RUN_A_DIR "/machine-state.ndjson", par, g_work, g_work);
    CHECK(system(cmd) == 0);
    est_params p; CHECK(est_params_read(par, &p) == 0);
    CHECK(strcmp(p.fit_sha, EST_RUN_A_SHA) == 0);
    printf("test_est_tools: run A fit: M0 q=%.17g r=%.17g | M1 q=%.17g r=%.17g\n", p.q[0], p.r[0], p.q[1], p.r[1]);
    /* eval on run A (test use): receipt, both streams, and the stream digests in the receipt */
    snprintf(rec, sizeof rec, "%s/receiptA.json", g_work);
    snprintf(cmd, sizeof cmd, "%s --params %s --raw %s --marks %s --out %s > %s/evalA.out 2>&1",
             g_eval, par, RUN_A_DIR "/machine-state.ndjson", RUN_A_DIR "/machine-state-marks.txt", rec, g_work);
    CHECK(system(cmd) == 0);
    char *js = slurp(rec); CHECK(js != NULL);
    if (js) {
        CHECK(strstr(js, "\"recorded\": false") != NULL);
        CHECK(strstr(js, "\"fit_file_is_run_A\": true") != NULL);
        CHECK(strstr(js, "\"fit_sha_matches_run_A_SHA256SUMS_line\": true") != NULL);
        CHECK(strstr(js, "\"ESTIMATION_REAL_SIGNAL\": \"PASS\"") != NULL);
        CHECK(strstr(js, "\"protocol_doc_matches_frozen_constant\": true") != NULL);
        CHECK(strstr(js, "\"prefix_invariance\": {\"method\": \"first k lines physically written") != NULL);
        CHECK(strstr(js, "\"runs\": 3, \"pass\": true") != NULL);
        for (int m = 0; m < 2; m++) {
            char sp[4096], hx[65], want[200];
            snprintf(sp, sizeof sp, "%s.M%d.stream", rec, m);
            CHECK(est_sha_file_hex(sp, hx) == 0);
            snprintf(want, sizeof want, "\"sha256\": \"%s\"", hx);
            CHECK(strstr(js, want) != NULL);
            char *st = slurp(sp);
            CHECK(st && strstr(st, " obs=") && strstr(st, "innov=") && strstr(st, "pred="));
            free(st);
        }
        free(js);
    }
    printf("test_est_tools: run A eval writes receipt + M0/M1 streams with digests in one run PASS\n");
    /* existing outputs are never overwritten */
    CHECK(system(cmd) != 0);
    /* --recorded is refused for anything that is not run B (run A here) */
    snprintf(cmd, sizeof cmd, "%s --params %s --raw %s --marks %s --recorded --out %s/rec-on-A.json >/dev/null 2>&1",
             g_eval, par, RUN_A_DIR "/machine-state.ndjson", RUN_A_DIR "/machine-state-marks.txt", g_work);
    CHECK(system(cmd) != 0);
    snprintf(cmd, sizeof cmd, "test ! -e %s/rec-on-A.json", g_work);
    CHECK(system(cmd) == 0);
    printf("test_est_tools: receipt/stream files are exclusive-create; --recorded refused on run A PASS\n");

    /* run-B guards, without any run B data. (1) path with the B id */
    snprintf(fake, sizeof fake, "%s/raw/x-%s-silicon/machine-state.ndjson", g_work, EST_RUN_B_ID);
    CHECK(sh_rc("%s %s %s/none >/dev/null 2>&1", g_fit, fake, g_work, "") != 0);
    snprintf(cmd, sizeof cmd, "%s --params %s --raw %s --marks %s --out %s/nb.json >/dev/null 2>&1", g_eval, par, fake, fake, g_work);
    CHECK(system(cmd) != 0);
    snprintf(cmd, sizeof cmd, "test ! -e %s/nb.json", g_work);
    CHECK(system(cmd) == 0);
    /* (2) relative path and symlink into a directory named for B: resolved components decide */
    char d[4096]; snprintf(d, sizeof d, "%s/bdir/20260929T025735Z-%s-silicon", g_work, EST_RUN_B_ID);
    sh("mkdir -p %s && cp %s/machine-state.ndjson %s/", d, RUN_A_DIR, d, "");
    sh("cp %s/SHA256SUMS %s/SHA256SUMS", RUN_A_DIR, d, "", "");
    snprintf(cmd, sizeof cmd, "cd %s && %s --synthetic-test machine-state.ndjson %s/relB.params >/dev/null 2>&1", d, g_fit, g_work);
    CHECK(system(cmd) != 0);
    snprintf(cmd, sizeof cmd, "cd %s && %s --params %s --raw machine-state.ndjson --marks machine-state.ndjson --out %s/relB.json >/dev/null 2>&1", d, g_eval, par, g_work);
    CHECK(system(cmd) != 0);
    sh("ln -sfn %s %s/linkB", d, g_work, "", "");
    snprintf(cmd, sizeof cmd, "%s --synthetic-test %s/linkB/machine-state.ndjson %s/linkB.params >/dev/null 2>&1", g_fit, g_work, g_work);
    CHECK(system(cmd) != 0);
    /* (3) a copy in an innocent directory whose SHA256SUMS line carries B's digest */
    snprintf(d, sizeof d, "%s/copyB", g_work);
    sh("rm -rf %s && mkdir -p %s", d, d, "", "");
    snprintf(cmd, sizeof cmd, "cp %s/machine-state.ndjson %s/machine-state.ndjson && echo '%s  machine-state.ndjson' > %s/SHA256SUMS",
             RUN_A_DIR, d, EST_RUN_B_RAW_SHA_DEFAULT, d);
    CHECK(system(cmd) == 0);
    snprintf(cmd, sizeof cmd, "%s --synthetic-test %s/machine-state.ndjson %s/copyB.params >/dev/null 2>&1", g_fit, d, g_work);
    CHECK(system(cmd) != 0);
    snprintf(cmd, sizeof cmd, "%s/est_replay 0 100 1000 %s/machine-state.ndjson >/dev/null 2>&1", g_work, d);
    est_file lf; char err[300];
    CHECK(est_file_load(RUN_A_DIR "/machine-state.ndjson", &lf, err, sizeof err) == 0); est_file_free(&lf);
    char cpath[4096]; snprintf(cpath, sizeof cpath, "%s/machine-state.ndjson", d);
    CHECK(est_file_load(cpath, &lf, err, sizeof err) != 0);
    CHECK(strstr(err, "run B") != NULL);
    snprintf(cmd, sizeof cmd, "%s --params %s --raw %s --marks %s --out %s/copyB.json >/dev/null 2>&1", g_eval, par, cpath, cpath, g_work);
    CHECK(system(cmd) != 0);
    /* est_fit refuses a non-run-A file unless --synthetic-test */
    snprintf(cmd, sizeof cmd, "%s %s/synth-eval/machine-state.ndjson %s/notA.params >/dev/null 2>&1", g_fit, g_work, g_work);
    CHECK(system(cmd) != 0);
    printf("test_est_tools: run B refused by resolved path, relative path, symlink and SUMS digest (fit, eval, replay lib) PASS\n");
}

/* --recorded end to end with the test build, on a synthetic directory standing in for run B */
static void test_recorded_path(void)
{
    char d[4096], par[4096], cmd[16384], rec[4096], marks[4096], raw[4096], hx1[65], hx2[65];
    snprintf(d, sizeof d, "%s/rb/synthB-silicon", g_work);
    make_series(d, 3000, 100.0, 3000.0, 909, 0);
    snprintf(raw, sizeof raw, "%s/machine-state.ndjson", d); snprintf(marks, sizeof marks, "%s/machine-state-marks.txt", d);
    est_sha_file_hex(raw, hx1); est_sha_file_hex(marks, hx2);
    snprintf(par, sizeof par, "%s/paramsA.txt", g_work);
    snprintf(rec, sizeof rec, "%s/rb/receiptB.json", g_work);
    char envs[512];
    snprintf(envs, sizeof envs, "EST_TEST_B_ID=synthB EST_TEST_B_RAW_SHA=%s EST_TEST_B_MARKS_SHA=%s", hx1, hx2);
    /* not recorded: refused */
    snprintf(cmd, sizeof cmd, "%s %s --params %s --raw %s --marks %s --out %s/rb/x0.json >/dev/null 2>&1", envs, g_evalt, par, raw, marks, g_work);
    CHECK(system(cmd) != 0);
    /* recorded, dirty tree: refused */
    snprintf(cmd, sizeof cmd, "%s EST_TEST_DIRTY=1 %s --params %s --raw %s --marks %s --recorded --out %s/rb/x1.json >/dev/null 2>&1", envs, g_evalt, par, raw, marks, g_work);
    CHECK(system(cmd) != 0);
    /* recorded, synthetic fit params (not run A): refused */
    snprintf(cmd, sizeof cmd, "%s EST_TEST_DIRTY=0 %s --params %s/synth-fit/params.txt --raw %s --marks %s --recorded --out %s/rb/x2.json >/dev/null 2>&1", envs, g_evalt, g_work, raw, marks, g_work);
    CHECK(system(cmd) != 0);
    /* recorded, clean, run A fit: one execution gives receipt + streams */
    snprintf(cmd, sizeof cmd, "%s EST_TEST_DIRTY=0 %s --params %s --raw %s --marks %s --recorded --out %s > %s/rb/out.txt 2>&1", envs, g_evalt, par, raw, marks, rec, g_work);
    CHECK(system(cmd) == 0);
    char *js = slurp(rec); CHECK(js != NULL);
    if (js) {
        CHECK(strstr(js, "\"recorded\": true") != NULL);
        CHECK(strstr(js, "\"tool_commit\": \"test-build\"") != NULL);
        CHECK(strstr(js, "\"tool_dirty\": 0") != NULL);
        CHECK(strstr(js, "\"fit_file_is_run_A\": true") != NULL);
        CHECK(strstr(js, "\"ESTIMATION_REAL_SIGNAL\": \"PASS\"") != NULL);
        char want[200];
        snprintf(want, sizeof want, "\"marks_file_sha256\": \"%s\"", hx2); CHECK(strstr(js, want) != NULL);
        snprintf(want, sizeof want, "\"input_file_sha256\": \"%s\"", hx1); CHECK(strstr(js, want) != NULL);
        CHECK(strstr(js, "\"eval_binary_sha256\": \"") != NULL && !strstr(js, "\"eval_binary_sha256\": \"unavailable\""));
        CHECK(strstr(js, "\"interpretations_sha256\": \"") != NULL && !strstr(js, "\"interpretations_sha256\": \"unavailable\""));
        for (int m = 0; m < 2; m++) {
            char sp[4096], hx[65];
            snprintf(sp, sizeof sp, "%s.M%d.stream", rec, m);
            CHECK(est_sha_file_hex(sp, hx) == 0);
            snprintf(want, sizeof want, "\"sha256\": \"%s\"", hx); CHECK(strstr(js, want) != NULL);
        }
        free(js);
    }
    /* second recorded execution: refused, nothing overwritten */
    char before[65], after[65];
    est_sha_file_hex(rec, before);
    snprintf(cmd, sizeof cmd, "%s EST_TEST_DIRTY=0 %s --params %s --raw %s --marks %s --recorded --out %s >/dev/null 2>&1", envs, g_evalt, par, raw, marks, rec);
    CHECK(system(cmd) != 0);
    est_sha_file_hex(rec, after);
    CHECK(strcmp(before, after) == 0);
    /* a recorded run whose marks file is not B's marks: refused */
    snprintf(cmd, sizeof cmd, "%s EST_TEST_DIRTY=0 %s --params %s --raw %s --marks %s/synth-eval/machine-state-marks.txt --recorded --out %s/rb/x3.json >/dev/null 2>&1", envs, g_evalt, par, raw, g_work, g_work);
    CHECK(system(cmd) != 0);
    printf("test_est_tools: --recorded end to end (test build): streams+digests in one run, dirty/non-A/second-run/wrong-marks refused PASS\n");
}

static void test_params_validation(void)
{
    char pth[4096]; est_params p; char err[200];
    snprintf(pth, sizeof pth, "%s/pv.txt", g_work);
    memset(&p, 0, sizeof p);
    snprintf(p.fit_path, sizeof p.fit_path, "x/%s/machine-state.ndjson", EST_RUN_A_ID);
    memset(p.fit_sha, 'a', 64);
    for (int m = 0; m < 2; m++) { p.q[m] = est_grid_q(8); p.r[m] = est_grid_r(10); p.ll[m] = -1.0; }
    CHECK(est_params_write(pth, &p) == 0);
    est_params q; CHECK(est_params_read(pth, &q) == 0);
    CHECK(est_params_validate(&q, err, sizeof err) == 0);
    p.q[0] = 12.0;                       /* off grid */
    CHECK(est_params_write(pth, &p) == 0 && est_params_read(pth, &q) == 0 && est_params_validate(&q, err, sizeof err) != 0);
    p.q[0] = est_grid_q(8); p.r[1] = NAN;
    CHECK(est_params_write(pth, &p) == 0 && est_params_read(pth, &q) == 0 && est_params_validate(&q, err, sizeof err) != 0);
    p.r[1] = est_grid_r(10); CHECK(est_params_write(pth, &p) == 0);
    sh("echo 'extra_key 1' >> %s", pth, "", "", "");
    CHECK(est_params_read(pth, &q) != 0);            /* unexpected extra key */
    CHECK(est_params_write(pth, &p) == 0);
    sh("sed -i '/^fit_lines/d' %s", pth, "", "", "");
    CHECK(est_params_read(pth, &q) != 0);            /* missing key */
    printf("test_est_tools: parameter file: exact keys, finite, on the protocol grid PASS\n");
}

static void test_fit_tie_break(void)
{
    static double ll[EST_NQ][EST_NR];
    for (int i = 0; i < EST_NQ; i++) for (int j = 0; j < EST_NR; j++) ll[i][j] = -100.0 - i - j;
    ll[5][7] = -3.0; ll[5][9] = -3.0; ll[7][2] = -3.0; ll[7][7] = -3.0;   /* four-way tie */
    int bi = -1, bj = -1;
    CHECK(est_fit_pick((const double (*)[EST_NR])ll, &bi, &bj) == 0);
    CHECK(bi == 5 && bj == 7);                                         /* smaller q, then smaller r */
    ll[3][3] = NAN; ll[3][4] = INFINITY;                                /* non-finite never wins */
    CHECK(est_fit_pick((const double (*)[EST_NR])ll, &bi, &bj) == 0 && bi == 5 && bj == 7);
    printf("test_est_tools: fit tie-break keeps smaller q then smaller r PASS\n");
}

static void test_parse_and_gaps(void)
{
    int64_t t;
    static const char ok[] = "{\"t\":1790000002.500000000,\"x\":1}";
    CHECK(est_parse_t(ok, sizeof ok - 1, &t) == 0 && t == 1790000002500000000ll);
    static const char big[] = "{\"t\":12345678901.000000000,\"x\":1}";
    CHECK(est_parse_t(big, sizeof big - 1, &t) == 2);
    static const char ov[] = "{\"t\":9999999999.000000000,\"x\":1}";
    CHECK(est_parse_t(ov, sizeof ov - 1, &t) == 2);
    static const char shortf[] = "{\"t\":1790000002.5,\"x\":1}";
    CHECK(est_parse_t(shortf, sizeof shortf - 1, &t) != 0);
    /* duplicate, backward and overflow t, and a parsable value with an unparsable t */
    char dir[4096], path[4096];
    snprintf(dir, sizeof dir, "%s/gaps", g_work);
    sh("rm -rf %s && mkdir -p %s", dir, dir, "", "");
    snprintf(path, sizeof path, "%s/g.ndjson", dir);
    FILE *fp = fopen(path, "wb");
    const char *ts[] = { "1790000000.000000000", "1790000001.000000000", "1790000001.000000000", "1790000000.000000000",
                         "1790000001.000000000", "99999999999.000000000", "1790000003.000000000" };
    for (int i = 0; i < 7; i++) fprintf(fp, "{\"t\":%s,\"thermal_mc\":\"%d 1 \"}\n", ts[i], 45000 + i);
    fclose(fp);
    est_file f; char err[300];
    CHECK(est_file_load_raw(path, &f, err, sizeof err) == 0);
    static est_replay rp;
    CHECK(est_replay_run(&f, (size_t)-1, 0, 10.0, 100.0, &rp) == 0);
    CHECK(rp.gap_zero == 1 && rp.gap_backward == 1);
    CHECK(rp.bad_t == 1 && rp.bad_t_overflow == 1);
    CHECK(rp.coasts == 1 && rp.steps[5].coast);        /* t unparsable, value fine: still missing */
    CHECK(rp.steps[2].horizon == 1 && rp.steps[3].horizon == 1);
    CHECK(rp.steps[1].horizon == 1 && rp.multi_horizon == 0);
    est_replay_free(&rp); est_file_free(&f);
    printf("test_est_tools: t parsing (overflow, short fraction), duplicate/backward gaps, bad-t = missing PASS\n");
}

static void run_eval_ka(const char *tag, const char *marks_path, char *rec_out, size_t cap)
{
    char dir[4096], par[4096], raw[4096], cmd[8192];
    snprintf(dir, sizeof dir, "%s/ka-%s", g_work, tag);
    sh("rm -rf %s && mkdir -p %s", dir, dir, "", "");
    snprintf(raw, sizeof raw, "%s/machine-state.ndjson", dir);
    FILE *fp = fopen(raw, "wb");
    for (int i = 0; i < 60; i++) fprintf(fp, "%s\n", KA_LINES[i]);
    fclose(fp);
    char h[65]; est_sha_file_hex(raw, h);
    snprintf(cmd, sizeof cmd, "%s/SHA256SUMS", dir);
    fp = fopen(cmd, "wb"); fprintf(fp, "%s  machine-state.ndjson\n", h); fclose(fp);
    est_params p; memset(&p, 0, sizeof p);
    snprintf(p.fit_path, sizeof p.fit_path, "synthetic/ka");
    memset(p.fit_sha, 'b', 64);
    for (int m = 0; m < 2; m++) { p.q[m] = est_grid_q(8); p.r[m] = est_grid_r(8); p.ll[m] = -1.0; }
    snprintf(par, sizeof par, "%s/params.txt", dir);
    est_params_write(par, &p);
    snprintf(rec_out, cap, "%s/receipt.json", dir);
    snprintf(cmd, sizeof cmd, "%s --params %s --raw %s --marks %s --out %s --synthetic-test >%s/out.txt 2>&1", g_eval, par, raw, marks_path, rec_out, dir);
    CHECK(system(cmd) == 0);
}

#define NEAR(a, b) CHECK(near((a), (b)))
static void check_ka_model(const char *js0, int m, const double *ex)
{
    /* ex: nis bias lag1 rmse persist logscore cov50 cov80 cov95 w50lo w50hi w80lo w80hi w95lo w95hi tcov tlo thi */
    const char *js = strstr(js0, "\"models\":"); char sc[8]; snprintf(sc, sizeof sc, "\"M%d\": {", m);
    const char *keys[] = { "\"mean_nis\": {\"value\": ", "\"standardized_bias\": {\"value\": ", "\"lag1_autocorr\": {\"value\": ",
                           "\"rmse_one_step\": ", "\"persistence_rmse\": ", "\"log_score_per_step\": " };
    for (int i = 0; i < 6; i++) NEAR(jv(js, sc, keys[i]), ex[i]);
    const char *cn[3] = { "coverage_50", "coverage_80", "coverage_95" };
    for (int k = 0; k < 3; k++) {
        char key[64]; snprintf(key, sizeof key, "\"%s\": {\"value\": ", cn[k]);
        NEAR(jv(js, sc, key), ex[6 + k]);
        const char *p = strstr(strstr(js, sc), key); p = strstr(p, "wilson95\": [");
        NEAR(atof(p + 12), ex[9 + 2 * k]);
        NEAR(atof(strchr(p + 12, ',') + 1), ex[10 + 2 * k]);
    }
    NEAR(jv(js, sc, "\"ten_step_coverage95\": {\"n\": "), 16.0);
    const char *tp = strstr(strstr(js, sc), "\"ten_step_coverage95\"");
    NEAR(jv(tp, NULL, "\"value\": "), ex[15]);
    const char *wp = strstr(tp, "wilson95\": [");
    NEAR(atof(wp + 12), ex[16]);
    NEAR(atof(strchr(wp + 12, ',') + 1), ex[17]);
}

static void test_known_answers(void)
{
    char rec[4096], mk[4096];
    snprintf(mk, sizeof mk, "%s/ka-marks", g_work);
    sh("rm -rf %s && mkdir -p %s", mk, mk, "", "");
    char mp[4096]; snprintf(mp, sizeof mp, "%s/machine-state-marks.txt", mk);
    FILE *fp = fopen(mp, "wb");
    fprintf(fp, "1790000032.000000000 begin trial-ka.jsonl\n1790000047.500000000 end trial-ka.jsonl exit 0\n");
    fclose(fp);
    char h[65]; est_sha_file_hex(mp, h);
    char sp[4096]; snprintf(sp, sizeof sp, "%s/SHA256SUMS", mk);
    fp = fopen(sp, "wb"); fprintf(fp, "%s  machine-state-marks.txt\n", h); fclose(fp);
    run_eval_ka("a", mp, rec, sizeof rec);
    char *js = slurp(rec); CHECK(js != NULL);
    if (!js) return;
    /* values from an independent scalar / 2x2 Kalman oracle written outside the repo */
    static const double m0[18] = { 2.086259463846454, 0.06467032815179131, -0.02771647957840814, 17.05425813763435,
        20.979581910582898, -4.4320995834844865, 0.2857142857142857, 0.6071428571428571, 0.8928571428571429,
        0.15253995979312637, 0.47059288582378966, 0.42409044159980325, 0.7643431355917387, 0.7280414829086315,
        0.962881633460356, 0.6875, 0.4440435565274821, 0.858353562348491 };
    static const double m1[18] = { 1.8281515606971528, 0.016086778302452025, -0.179859160525074, 20.539325725342962,
        20.979581910582898, -4.573313892158871, 0.32142857142857145, 0.5714285714285714, 0.8214285714285714,
        0.179332468369093, 0.5066115696450038, 0.39070785495839777, 0.7349145298359636, 0.6440857505322257,
        0.9212149810424002, 1.0, 0.8063923170025951, 1.0 };
    check_ka_model(js, 0, m0);
    check_ka_model(js, 1, m1);
    /* counts: samples, quarters, regimes, coasts, gaps */
    const char *s0 = strstr(strstr(js, "\"models\":"), "\"M0\": {");
    NEAR(jv(s0, NULL, "\"samples\": "), 28.0);
    const char *q0 = strstr(s0, "\"quarters_coverage95\"");
    static const int qn[4] = { 6, 7, 7, 8 }, qh0[4] = { 5, 7, 6, 7 };
    const char *p = q0;
    for (int q = 0; q < 4; q++) {
        p = strstr(p, "{\"n\": ");
        NEAR(atof(p + 6), (double)qn[q]);
        p = strstr(p, "\"value\": ");
        NEAR(atof(p + 9), (double)qh0[q] / (double)qn[q]);
        p += 9;
    }
    const char *r0 = strstr(s0, "\"idle\": {\"n\": ");
    NEAR(atof(r0 + 14), 15.0); NEAR(jv(r0, NULL, "\"value\": "), 13.0 / 15.0);
    const char *r1 = strstr(s0, "\"in_trial\": {\"n\": ");
    NEAR(atof(r1 + 18), 13.0); NEAR(jv(r1, NULL, "\"value\": "), 12.0 / 13.0);
    /* M1 regime and quarter hits */
    const char *s1 = strstr(strstr(js, "\"models\":"), "\"M1\": {");
    const char *r1i = strstr(s1, "\"idle\": {\"n\": "); NEAR(jv(r1i, NULL, "\"value\": "), 11.0 / 15.0);
    const char *r1t = strstr(s1, "\"in_trial\": {\"n\": "); NEAR(jv(r1t, NULL, "\"value\": "), 12.0 / 13.0);
    CHECK(strstr(js, "\"coasted\": 2,") != NULL);
    CHECK(strstr(js, "\"bad_value_lines\": 1,") != NULL);
    CHECK(strstr(js, "\"bad_t_lines\": 1,") != NULL);
    CHECK(strstr(js, "\"steps_with_horizon_gt_1\": 1}") != NULL);
    CHECK(strstr(js, "\"unclosed_begin_ran_to_eof\": false") != NULL);
    NEAR(jv(js, "\"zero_innovation_fraction_extra\": ", "\"zero_innovation_fraction_extra\": "), 0.0);
    /* included logical span 30..61 (2.5 s gap made line 35 a horizon-3 step) */
    CHECK(strstr(s0, "\"included_logical_span_ns\": [30000000000, 61000000000]") != NULL);
    free(js);
    printf("test_est_tools: known-answer statistics (coverage, Wilson, NIS, bias, lag-1, RMSE, persistence, quarters, regimes, ten-step) match the independent oracle PASS\n");

    /* unclosed begin runs to end of file and is reported */
    snprintf(mk, sizeof mk, "%s/ka-marks2", g_work);
    sh("rm -rf %s && mkdir -p %s", mk, mk, "", "");
    snprintf(mp, sizeof mp, "%s/machine-state-marks.txt", mk);
    fp = fopen(mp, "wb"); fprintf(fp, "1790000032.000000000 begin trial-ka.jsonl\n"); fclose(fp);
    est_sha_file_hex(mp, h);
    snprintf(sp, sizeof sp, "%s/SHA256SUMS", mk);
    fp = fopen(sp, "wb"); fprintf(fp, "%s  machine-state-marks.txt\n", h); fclose(fp);
    run_eval_ka("b", mp, rec, sizeof rec);
    js = slurp(rec); CHECK(js != NULL);
    if (js) {
        CHECK(strstr(js, "\"unclosed_begin_ran_to_eof\": true") != NULL);
        CHECK(strstr(js, "\"begins\": 1, \"ends\": 0") != NULL);
        const char *m0s = strstr(strstr(js, "\"models\":"), "\"M0\": {");
        const char *ri = strstr(m0s, "\"idle\": {\"n\": "), *rt = strstr(m0s, "\"in_trial\": {\"n\": ");
        NEAR(atof(ri + 14), 2.0); NEAR(atof(rt + 18), 26.0);
        free(js);
    }
    printf("test_est_tools: unclosed begin runs to end of file and is reported PASS\n");
}

/* ---- protocol v2: noise shape, M2 fit/eval, held-out guard ---- */
static void test_mix_known_answers(void)
{
    est_mix g = { 1, { 1.0 }, { 1.0 } };
    double c, t, l;
    CHECK(est_mix_cdf_abs(&g, 1, 1.959963984540054, &c) == EST_OK && fabs(c - 0.95) < 1e-12);
    CHECK(est_mix_quantile_abs(&g, 1, 0.95, &t) == EST_OK && fabs(t - 1.959963984540054) < 1e-9);
    CHECK(est_mix_quantile_abs(&g, 4, 0.95, &t) == EST_OK && fabs(t - 2.0 * 1.959963984540054) < 1e-9);
    CHECK(est_mix_logpdf(&g, 1, 0.0, &l) == EST_OK && fabs(l + 0.5 * log(6.283185307179586)) < 1e-14);
    est_mix m = { 2, { 0.5, 0.5 }, { 1.0, 4.0 } };
    double want1 = 0.5 * erf(1.5 / sqrt(2.0)) + 0.5 * erf(1.5 / sqrt(8.0));
    CHECK(est_mix_cdf_abs(&m, 1, 1.5, &c) == EST_OK && fabs(c - want1) < 1e-14);
    /* two draws: variance 2 w.p. 1/4, 5 w.p. 1/2, 8 w.p. 1/4 */
    double want2 = 0.25 * erf(3.0 / sqrt(4.0)) + 0.5 * erf(3.0 / sqrt(10.0)) + 0.25 * erf(3.0 / sqrt(16.0));
    CHECK(est_mix_cdf_abs(&m, 2, 3.0, &c) == EST_OK && fabs(c - want2) < 1e-14);
    double lw = log(0.25 * exp(-0.5 * (log(6.283185307179586 * 2.0) + 1.0 / 2.0)) + 0.5 * exp(-0.5 * (log(6.283185307179586 * 5.0) + 1.0 / 5.0))
                    + 0.25 * exp(-0.5 * (log(6.283185307179586 * 8.0) + 1.0 / 8.0)));
    CHECK(est_mix_logpdf(&m, 2, 1.0, &l) == EST_OK && fabs(l - lw) < 1e-13);
    CHECK(fabs(est_mix_total_var(&m) - 2.5) < 1e-15);
    est_mix bad = { 2, { 0.5, 0.6 }, { 1.0, 4.0 } };
    CHECK(est_mix_check(&bad) != EST_OK);
    CHECK(est_mix_cdf_abs(&m, 0, 1.0, &c) != EST_OK && est_mix_cdf_abs(&m, EST_MIX_MAX_H + 1, 1.0, &c) != EST_OK);
    /* EM recovers a known two-scale mixture; same input gives the same bits */
    size_t n = 20000; double *e = malloc(n * sizeof *e);
    g_rng = 777;
    for (size_t i = 0; i < n; i++) e[i] = (unif() < 0.3 ? 10.0 : 1000.0) * gauss();
    est_mix f1, f2; double l1, l2;
    CHECK(est_mix_fit_em(e, n, 2, 1.0, 500, &f1, &l1) == EST_OK);
    CHECK(est_mix_fit_em(e, n, 2, 1.0, 500, &f2, &l2) == EST_OK);
    CHECK(memcmp(&f1, &f2, sizeof f1) == 0 && memcmp(&l1, &l2, sizeof l1) == 0);
    CHECK(fabs(f1.w[0] - 0.3) < 0.02 && fabs(sqrt(f1.v[0]) - 10.0) < 1.0 && fabs(sqrt(f1.v[1]) - 1000.0) < 30.0);
    free(e);
    printf("test_est_tools: noise shape known answers and EM recovery (w0=%.3f sd=%.2f,%.1f)\n", f1.w[0], sqrt(f1.v[0]), sqrt(f1.v[1]));
}

/* Random walk whose steps have a three-scale shape, observed exactly to 1 mC. */
static void make_mix_series(const char *dir, size_t n, uint64_t seed)
{
    char p[1024], a[1024], b[1024], h1[65], h2[65];
    sh("rm -rf %s && mkdir -p %s", dir, dir, "", "");
    snprintf(a, sizeof a, "%s/machine-state.ndjson", dir); snprintf(b, sizeof b, "%s/machine-state-marks.txt", dir);
    FILE *fp = fopen(a, "wb");
    g_rng = seed; double x = 45000.0; long long wall = 1790000000ll;
    for (size_t i = 0; i < n; i++) {
        double u = unif();
        x = floor(x + (u < 0.4 ? 20.0 : (u < 0.8 ? 300.0 : 2000.0)) * gauss() + 0.5);
        fprintf(fp, "{\"t\":%lld.%09d,\"thermal_mc\":\"%.0f 30000 \"}\n", wall + (long long)i, (int)(rnd() % 3000000), x);
    }
    fclose(fp);
    fp = fopen(b, "wb"); fclose(fp);          /* no trials, like the EST-3b collections */
    est_sha_file_hex(a, h1); est_sha_file_hex(b, h2);
    snprintf(p, sizeof p, "%s/SHA256SUMS", dir);
    fp = fopen(p, "wb");
    fprintf(fp, "%s  machine-state.ndjson\n%s  machine-state-marks.txt\n", h1, h2);
    fclose(fp);
}

static void test_m2(void)
{
    char fd[1024], hd[1024], fr[1100], hr[1100], hm[1100], p2[1100], p2b[1100], rec[1100], v1[1100], cmd[16384], envs[1024];
    char h1[65], h2[65], hf[65], hdoc[65];
    snprintf(fd, sizeof fd, "%s/v2/s-est3b-fit-silicon", g_work);
    snprintf(hd, sizeof hd, "%s/v2/s-est3b-heldout-silicon", g_work);
    make_mix_series(fd, 4000, 4242);
    make_mix_series(hd, 4000, 5353);
    snprintf(fr, sizeof fr, "%s/machine-state.ndjson", fd);
    snprintf(hr, sizeof hr, "%s/machine-state.ndjson", hd); snprintf(hm, sizeof hm, "%s/machine-state-marks.txt", hd);
    snprintf(v1, sizeof v1, "%s/paramsA.txt", g_work);                      /* fit on run A by test_run_a_and_flags */
    snprintf(p2, sizeof p2, "%s/v2/params2.txt", g_work); snprintf(p2b, sizeof p2b, "%s/v2/params2b.txt", g_work);
    /* fit refuses the held-out run, by path, before opening it */
    CHECK(sh_rc("%s --m2 --synthetic-test %s %s %s/v2/no.txt >/dev/null 2>&1", g_fit, hr, v1, g_work) != 0);
    /* without --synthetic-test only run C1 (fixed SHA) is accepted */
    CHECK(sh_rc("%s --m2 %s %s %s/v2/no.txt >/dev/null 2>&1", g_fit, fr, v1, g_work) != 0);
    CHECK(sh_rc("%s --m2 --synthetic-test %s %s %s >/dev/null 2>&1", g_fit, fr, v1, p2) == 0);
    CHECK(sh_rc("%s --m2 --synthetic-test %s %s %s >/dev/null 2>&1", g_fit, fr, v1, p2b) == 0);
    sh("cmp -s %s %s || echo nondeterministic-fit > %s/v2/NONDET", p2, p2b, g_work, "");
    snprintf(cmd, sizeof cmd, "%s/v2/NONDET", g_work);
    FILE *nd = fopen(cmd, "r"); CHECK(nd == NULL); if (nd) fclose(nd);
    /* persistence round trip: read then write gives the same bytes */
    est_params2 pp; CHECK(est_params2_read(p2, &pp) == 0);
    CHECK(pp.mix.k == EST_M2_K && pp.r == EST_M2_R && fabs(pp.q - est_mix_total_var(&pp.mix)) <= 1e-12 * pp.q);
    snprintf(cmd, sizeof cmd, "%s/v2/params2c.txt", g_work);
    CHECK(est_params2_write(cmd, &pp) == 0);
    CHECK(sh_rc("cmp -s %s %s", p2, cmd, "", "") == 0);
    sh("sed 's/^c2 w/c9 w/' %s > %s/v2/params2bad.txt", p2, g_work, "", "");
    snprintf(cmd, sizeof cmd, "%s/v2/params2bad.txt", g_work);
    CHECK(est_params2_read(cmd, &pp) != 0);
    /* M2 replay mean is exactly the previous observation */
    est_file f; char err[300]; static est_replay rp;
    CHECK(est_file_load(fr, &f, err, sizeof err) == 0);
    CHECK(est_params2_read(p2, &pp) == 0);
    CHECK(est_replay_run(&f, (size_t)-1, 2, pp.q, pp.r, &rp) == 0);
    double *e = malloc(rp.nsteps * sizeof *e);
    CHECK(est_m2_changes(&rp, e, rp.nsteps) == pp.fit_n);
    free(e); est_replay_free(&rp); est_file_free(&f);
    /* held-out: v1 and v2 evaluation refuse it unless recorded */
    snprintf(cmd, sizeof cmd, "%s --protocol-v2 --synthetic-test --params2 %s --raw %s --marks %s --out %s/v2/x.json 2>%s/v2/x.err", g_eval, p2, hr, hm, g_work, g_work);
    CHECK(system(cmd) != 0);
    snprintf(cmd, sizeof cmd, "%s --params %s --raw %s --marks %s --out %s/v2/y.json 2>%s/v2/y.err", g_eval, v1, hr, hm, g_work, g_work);
    CHECK(system(cmd) != 0);
    snprintf(cmd, sizeof cmd, "%s/v2/x.err", g_work); { char *t = slurp(cmd); CHECK(t && strstr(t, "held-out")); free(t); }
    snprintf(cmd, sizeof cmd, "%s/v2/y.err", g_work); { char *t = slurp(cmd); CHECK(t && strstr(t, "held-out")); free(t); }
    CHECK(est_file_load(hr, &f, err, sizeof err) != 0 && strstr(err, "held-out") != NULL);
    /* recorded, test build: C2, C1, doc identities from the environment */
    est_sha_file_hex(hr, h1); est_sha_file_hex(hm, h2); est_sha_file_hex(fr, hf);
    snprintf(cmd, sizeof cmd, "%s/v2/doc.md", g_work);
    sh("echo frozen-v2 > %s", cmd, "", "", ""); est_sha_file_hex(cmd, hdoc);
    snprintf(envs, sizeof envs, "EST_TEST_C2_RAW_SHA=%s EST_TEST_C2_MARKS_SHA=%s EST_TEST_C1_SHA=%s EST_TEST_V2_DOC_SHA=%s", h1, h2, hf, hdoc);
    snprintf(rec, sizeof rec, "%s/v2/receipt.json", g_work);
    snprintf(cmd, sizeof cmd, "%s EST_TEST_DIRTY=1 %s --protocol-v2 --params2 %s --raw %s --marks %s --protocol-doc %s/v2/doc.md --recorded --out %s/v2/z.json >/dev/null 2>&1", envs, g_evalt, p2, hr, hm, g_work, g_work);
    CHECK(system(cmd) != 0);
    snprintf(cmd, sizeof cmd, "%s EST_TEST_DIRTY=0 %s --protocol-v2 --params2 %s --raw %s --marks %s --protocol-doc %s/v2/doc.md --recorded --out %s > %s/v2/eval.out 2>&1", envs, g_evalt, p2, hr, hm, g_work, rec, g_work);
    CHECK(system(cmd) == 0);
    char *js = slurp(rec); CHECK(js != NULL);
    if (js) {
        CHECK(strstr(js, "\"receipt\": \"est_eval_v2\"") != NULL);
        CHECK(strstr(js, "\"recorded\": true") != NULL && strstr(js, "\"input_is_heldout_C2\": true") != NULL);
        CHECK(strstr(js, "\"m2_mean_is_exact_persistence\": true") != NULL);
        const char *mods = strstr(js, "\"models\": {");
        const char *m2 = mods ? strstr(mods, "\"M2\": {") : NULL;
        CHECK(m2 != NULL);
        const char *cal = m2 ? strstr(m2, "\"calibrated\": ") : NULL;
        CHECK(cal && strncmp(cal + 14, "true", 4) == 0);
        CHECK(m2 && strstr(m2, "\"rmse_no_worse_than_persistence\": true") != NULL);
        CHECK(strstr(js, "\"selected_model\": \"M2\"") != NULL);
        CHECK(strstr(js, "\"ESTIMATION_REAL_SIGNAL\": \"PASS\"") != NULL);
        CHECK(strstr(js, "\"ESTIMATION_CALIBRATION\": \"PASS\"") != NULL);
        free(js);
    }
    /* a second recorded execution would overwrite: refused */
    snprintf(cmd, sizeof cmd, "%s EST_TEST_DIRTY=0 %s --protocol-v2 --params2 %s --raw %s --marks %s --protocol-doc %s/v2/doc.md --recorded --out %s >/dev/null 2>&1", envs, g_evalt, p2, hr, hm, g_work, rec);
    CHECK(system(cmd) != 0);
    printf("test_est_tools: M2 fit deterministic, params v2 round trip, exact persistence, held-out guard, recorded v2 receipt\n");
}

int main(int argc, char **argv)
{
    if (argc != 5) { fprintf(stderr, "usage: test_est_tools est_fit est_eval est_eval_test_build workdir\n"); return 2; }
    g_fit = argv[1]; g_eval = argv[2]; g_evalt = argv[3]; g_work = argv[4];
    sh("mkdir -p %s", g_work, "", "", "");
    test_prefix_run_a();
    test_coasting();
    test_sha_refusal();
    test_synthetic();
    test_run_a_and_flags();
    test_recorded_path();
    test_params_validation();
    test_fit_tie_break();
    test_parse_and_gaps();
    test_known_answers();
    test_mix_known_answers();
    test_m2();
    printf("test_est_tools: %d checks, %d failed\n", g_checks, g_fail);
    if (g_fail) { printf("test_est_tools: FAIL\n"); return 1; }
    printf("test_est_tools: PASS\n");
    return 0;
}
