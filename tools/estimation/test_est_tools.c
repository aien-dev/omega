/* Tests for est_replay / est_fit / est_eval (protocol v1). Run A and synthetic
 * data only; this program never names run B's data file.
 *   test_est_tools <est_fit> <est_eval> <workdir> */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "est_replay.h"

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define RUN_A_DIR "evidence/R15/raw/" EST_RUN_A_ID
static char *g_fit, *g_eval, *g_work;

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
    CHECK(sh_rc("%s %s %s >/dev/null 2>&1", g_fit, path, out, "") != 0);
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
    CHECK(sh_rc("%s %s %s >/dev/null 2>&1", g_fit, f1, par, "") == 0);
    est_params p; CHECK(est_params_read(par, &p) == 0);
    double dq = fabs(log10(p.q[0]) - 1.5), dr = fabs(log10(p.r[0]) - 3.5);
    CHECK(dq <= 0.5 + 1e-9);
    CHECK(dr <= 0.25 + 1e-9);
    printf("test_est_tools: synthetic fit M0 true q=%.4g r=%.4g, fitted q=%.4g r=%.4g (grid steps %.1f, %.1f) %s\n",
           q, r, p.q[0], p.r[0], dq / 0.5, dr / 0.25, (dq <= 0.5 + 1e-9 && dr <= 0.25 + 1e-9) ? "PASS" : "FAIL");
    /* eval on an independent series with the same true q, r */
    snprintf(cmd, sizeof cmd, "%s --params %s --raw %s --marks %s --tool-commit test --out %s --synthetic-test >%s/eval.out 2>&1",
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
    snprintf(cmd, sizeof cmd, "%s --params %s --raw %s --marks %s --tool-commit test --out %s --synthetic-test >%s/eval-bad.out 2>&1",
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
    snprintf(cmd, sizeof cmd, "%s --params %s --raw %s --marks %s --tool-commit test --out %s/x.json >/dev/null 2>&1", g_eval, par, f2, m2, d2);
    CHECK(system(cmd) != 0);
}

static void test_run_a_and_flags(void)
{
    char par[1100], rec[1100], cmd[16384], fake[1100];
    snprintf(par, sizeof par, "%s/paramsA.txt", g_work);
    snprintf(cmd, sizeof cmd, "%s %s %s > %s/fitA.out 2> %s/fitA.err", g_fit, RUN_A_DIR "/machine-state.ndjson", par, g_work, g_work);
    CHECK(system(cmd) == 0);
    est_params p; CHECK(est_params_read(par, &p) == 0);
    CHECK(strcmp(p.fit_sha, EST_RUN_A_SHA) == 0);
    printf("test_est_tools: run A fit: M0 q=%.17g r=%.17g | M1 q=%.17g r=%.17g\n", p.q[0], p.r[0], p.q[1], p.r[1]);
    /* eval on run A (test use), without and with --recorded */
    for (int rc = 0; rc < 2; rc++) {
        snprintf(rec, sizeof rec, "%s/receiptA%d.json", g_work, rc);
        snprintf(cmd, sizeof cmd, "%s --params %s --raw %s --marks %s --tool-commit test%s --out %s > %s/evalA%d.out 2>&1",
                 g_eval, par, RUN_A_DIR "/machine-state.ndjson", RUN_A_DIR "/machine-state-marks.txt",
                 rc ? " --recorded" : "", rec, g_work, rc);
        CHECK(system(cmd) == 0);
        char *js = slurp(rec); CHECK(js != NULL);
        if (js) {
            CHECK(strstr(js, rc ? "\"recorded\": true" : "\"recorded\": false") != NULL);
            CHECK(strstr(js, "\"fit_file_is_run_A\": true") != NULL);
            CHECK(strstr(js, "\"ESTIMATION_REAL_SIGNAL\": \"PASS\"") != NULL);
            free(js);
        }
    }
    printf("test_est_tools: receipt records --recorded flag (false without, true with) PASS\n");
    /* run-B safeguards: only strings are checked, nothing is opened */
    snprintf(fake, sizeof fake, "%s/raw/x-%s-silicon/machine-state.ndjson", g_work, EST_RUN_B_ID);
    CHECK(sh_rc("%s %s %s/none >/dev/null 2>&1", g_fit, fake, g_work, "") != 0);
    snprintf(cmd, sizeof cmd, "%s --params %s --raw %s --marks %s --tool-commit test --out %s/nb.json >/dev/null 2>&1", g_eval, par, fake, fake, g_work);
    CHECK(system(cmd) != 0);
    snprintf(cmd, sizeof cmd, "test ! -e %s/nb.json", g_work);
    CHECK(system(cmd) == 0);
    printf("test_est_tools: fit refuses run B path; eval refuses run B without --recorded PASS\n");
}

int main(int argc, char **argv)
{
    if (argc != 4) { fprintf(stderr, "usage: test_est_tools est_fit est_eval workdir\n"); return 2; }
    g_fit = argv[1]; g_eval = argv[2]; g_work = argv[3];
    sh("mkdir -p %s", g_work, "", "", "");
    test_prefix_run_a();
    test_coasting();
    test_sha_refusal();
    test_synthetic();
    test_run_a_and_flags();
    printf("test_est_tools: %d checks, %d failed\n", g_checks, g_fail);
    if (g_fail) { printf("test_est_tools: FAIL\n"); return 1; }
    printf("test_est_tools: PASS\n");
    return 0;
}
