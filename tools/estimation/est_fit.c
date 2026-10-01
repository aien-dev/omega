/* EST-2 fit (protocol v1 section 3): grid search on run A only; --m2 fits the protocol v2 mixture on run C1.
 *   est_fit <machine-state.ndjson of run A> <out-params-file>
 * Refuses run B (resolved path or SHA256SUMS digest) and, without
 * --synthetic-test, any file that is not run A. Prints the parameter
 * file's SHA-256. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "est_replay.h"

/* Protocol v2 section 3: fit M2's noise shape on fit run C1 only.
 *   est_fit --m2 [--synthetic-test] <C1 machine-state.ndjson> <v1 params> <out-params2>
 * One-step changes (horizon 1, observed, L >= burn-in) of a model 2 replay;
 * deterministic EM, k = EST_M2_K, floor EST_M2_FLOOR, EST_M2_ITERS rounds.
 * The held-out run is refused by est_file_load before it is opened. */
static int fit_m2(int synth, const char *data, const char *v1, const char *outp)
{
    est_file f; char err[300];
    if (est_file_load(data, &f, err, sizeof err)) { fprintf(stderr, "refuse: %s\n", err); return 1; }
    if (!synth && (strstr(data, EST_V2_FIT_TAG) == NULL || strcmp(f.sha_hex, EST_V2_FIT_SHA) != 0)) {
        fprintf(stderr, "refuse: est_fit --m2 fits run C1 only (path tag %s and SHA-256 %s)\n", EST_V2_FIT_TAG, EST_V2_FIT_SHA); return 2;
    }
    est_params p1;
    if (est_params_read(v1, &p1) || est_params_validate(&p1, err, sizeof err)) { fprintf(stderr, "refuse: v1 parameter file %s is not valid\n", v1); return 1; }
    est_params2 p; memset(&p, 0, sizeof p);
    snprintf(p.v1_params_path, sizeof p.v1_params_path, "%s", v1);
    if (est_sha_file_hex(v1, p.v1_params_sha)) return 1;
    snprintf(p.fit_path, sizeof p.fit_path, "%s", data);
    memcpy(p.fit_sha, f.sha_hex, 65);
    p.fit_lines = f.nlines;
    static est_replay rp;
    if (est_replay_run(&f, (size_t)-1, 2, 1e6, EST_M2_R, &rp)) { fprintf(stderr, "replay failed: %s\n", rp.errmsg); return 1; }
    double *e = malloc((rp.nsteps + 1) * sizeof *e);
    if (!e) return 1;
    size_t n = est_m2_changes(&rp, e, rp.nsteps);
    if (n == (size_t)-1 || n < 2) { fprintf(stderr, "refuse: replay mean is not persistence, or too few changes\n"); return 1; }
    p.fit_n = n;
    if (est_mix_fit_em(e, n, EST_M2_K, EST_M2_FLOOR, EST_M2_ITERS, &p.mix, &p.ll) != EST_OK) { fprintf(stderr, "EM fit refused\n"); return 1; }
    p.q = est_mix_total_var(&p.mix); p.r = EST_M2_R;
    size_t zero = 0; for (size_t i = 0; i < n; i++) if (e[i] == 0.0) zero++;
    fprintf(stderr, "M2: changes=%zu zero_fraction=%.6f q=%.17g loglik=%.6f\n", n, (double)zero / (double)n, p.q, p.ll);
    for (uint32_t j = 0; j < p.mix.k; j++) fprintf(stderr, "  c%u w=%.6f sd=%.3f\n", j, p.mix.w[j], sqrt(p.mix.v[j]));
    free(e); est_replay_free(&rp); est_file_free(&f);
    if (est_params2_write(outp, &p)) { fprintf(stderr, "cannot write %s\n", outp); return 1; }
    char hex[65];
    if (est_sha_file_hex(outp, hex)) return 1;
    printf("%s  %s\n", hex, outp);
    return 0;
}

int main(int argc, char **argv)
{
    int synth = 0;
    if (argc >= 2 && strcmp(argv[1], "--m2") == 0) {
        argv++; argc--;
        if (argc == 5 && strcmp(argv[1], "--synthetic-test") == 0) { synth = 1; argv++; argc--; }
        if (argc != 4) { fprintf(stderr, "usage: est_fit --m2 [--synthetic-test] <C1 machine-state.ndjson> <v1 params> <out-params2>\n"); return 2; }
        return fit_m2(synth, argv[1], argv[2], argv[3]);
    }
    if (argc == 4 && strcmp(argv[1], "--synthetic-test") == 0) { synth = 1; argv++; argc--; }
    if (argc != 3) { fprintf(stderr, "usage: est_fit [--synthetic-test] <run-A machine-state.ndjson> <out-params>\n"); return 2; }
    est_file f; char err[300];
    /* est_file_load refuses run B by resolved path and by SHA256SUMS digest,
     * before the data file is opened. */
    if (est_file_load(argv[1], &f, err, sizeof err)) { fprintf(stderr, "refuse: %s\n", err); return 1; }
    if (!synth && strcmp(f.sha_hex, EST_RUN_A_SHA) != 0) { fprintf(stderr, "refuse: est_fit fits run A only (SHA-256 is not run A's)\n"); return 2; }
    est_params p; memset(&p, 0, sizeof p);
    snprintf(p.fit_path, sizeof p.fit_path, "%s", argv[1]);
    memcpy(p.fit_sha, f.sha_hex, 65);
    p.fit_lines = f.nlines;
    static est_replay rp;
    for (int m = 0; m < 2; m++) {
        static double grid[EST_NQ][EST_NR]; size_t cnt[EST_NQ][EST_NR]; int bi = -1, bj = -1;
        for (int i = 0; i < EST_NQ; i++) {
            for (int j = 0; j < EST_NR; j++) {
                double q = est_grid_q(i), r = est_grid_r(j);
                if (est_replay_run(&f, (size_t)-1, m, q, r, &rp)) {
                    fprintf(stderr, "replay failed M%d q=%g r=%g: %s\n", m, q, r, rp.errmsg);
                    return 1;
                }
                double ll = 0; size_t n = 0;
                for (size_t k = 0; k < rp.nsteps; k++) {
                    const est_step *s = &rp.steps[k];
                    if (s->prior || s->coast || s->L < EST_BURN_IN) continue;
                    ll += -0.5 * (log(6.283185307179586 * s->S) + s->nu * s->nu / s->S);
                    n++;
                }
                /* strict > : ties keep the earlier (smaller q, then smaller r) */
                grid[i][j] = ll; cnt[i][j] = n;
            }
        }
        if (est_fit_pick((const double (*)[EST_NR])grid, &bi, &bj)) { fprintf(stderr, "no finite likelihood for M%d\n", m); return 1; }
        p.q[m] = est_grid_q(bi); p.r[m] = est_grid_r(bj); p.ll[m] = grid[bi][bj];
        fprintf(stderr, "M%d: q=%.17g (10^%.1f) r=%.17g (10^%.2f) loglik=%.6f over %zu steps\n",
                m, p.q[m], -3.0 + 0.5 * bi, p.r[m], 0.25 * bj, grid[bi][bj], cnt[bi][bj]);
    }
    est_replay_free(&rp);
    if (est_params_write(argv[2], &p)) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    char hex[65];
    if (est_sha_file_hex(argv[2], hex)) return 1;
    printf("%s  %s\n", hex, argv[2]);
    est_file_free(&f);
    return 0;
}
