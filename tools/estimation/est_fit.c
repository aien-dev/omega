/* EST-2 fit (protocol v1 section 3): grid search on run A only.
 *   est_fit <machine-state.ndjson of run A> <out-params-file>
 * Refuses any path containing the run B directory id. Prints the parameter
 * file's SHA-256. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "est_replay.h"

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: est_fit <run-A machine-state.ndjson> <out-params>\n"); return 2; }
    if (strstr(argv[1], EST_RUN_B_ID)) {
        fprintf(stderr, "refuse: est_fit opens run A only (path names run B)\n");
        return 2;
    }
    est_file f; char err[300];
    if (est_file_load(argv[1], &f, err, sizeof err)) { fprintf(stderr, "refuse: %s\n", err); return 1; }
    est_params p; memset(&p, 0, sizeof p);
    snprintf(p.fit_path, sizeof p.fit_path, "%s", argv[1]);
    memcpy(p.fit_sha, f.sha_hex, 65);
    p.fit_lines = f.nlines;
    static est_replay rp;
    for (int m = 0; m < 2; m++) {
        double best = -INFINITY; int bi = -1, bj = -1; size_t used = 0;
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
                if (!isfinite(ll)) continue;
                if (ll > best) { best = ll; bi = i; bj = j; used = n; }
            }
        }
        if (bi < 0) { fprintf(stderr, "no finite likelihood for M%d\n", m); return 1; }
        p.q[m] = est_grid_q(bi); p.r[m] = est_grid_r(bj); p.ll[m] = best;
        fprintf(stderr, "M%d: q=%.17g (10^%.1f) r=%.17g (10^%.2f) loglik=%.6f over %zu steps\n",
                m, p.q[m], -3.0 + 0.5 * bi, p.r[m], 0.25 * bj, best, used);
    }
    est_replay_free(&rp);
    if (est_params_write(argv[2], &p)) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    char hex[65];
    if (est_sha_file_hex(argv[2], hex)) return 1;
    printf("%s  %s\n", hex, argv[2]);
    est_file_free(&f);
    return 0;
}
