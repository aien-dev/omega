/* TEST-ONLY truth process for the learner harness (substrate side: links the
 * generators). Runs as a separate process so no learner or harness object ever
 * links pd0_gen.c. Text protocol on stdin/stdout:
 *   rel                                  -> the true relation (PDLAW1 delta form) and S*
 *   ep <n_obs> <reset..> <n> <ch val>*   -> noise-free trajectory of n steps (or "oob")
 * Usage: pd0-truth <level|null> <seed> */
#include "physics0/pd0_gen.h"
#include "physics0/pd0_relation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    int level = strcmp(argv[1], "null") == 0 ? PD0_LEVEL_NULL : atoi(argv[1]);
    pd0_gen g; pd0_desc d; if (pd0_gen_init(&g, level, strtoull(argv[2], NULL, 0)) != 0 || pd0_gen_desc(level, &d) != 0) return 2;
    pd0_rng dummy_noise, dummy_null; pd0_stream(&dummy_noise, 1, "x"); pd0_stream(&dummy_null, 1, "y");
    char line[4096];
    while (fgets(line, sizeof line, stdin)) {
        if (!strncmp(line, "rel", 3)) {
            pd0_relation r; memset(&r, 0, sizeof r); pd0_gen_true_relation(&g, d.dt_micro, &r);
            printf("rel %u %u %u %u %d %d\n", r.n_vars, r.n_latent, r.n_channels, r.n_eq, pd0_relation_size(&r), pd0_gen_true_size(level));
            for (int e = 0; e < r.n_eq; e++) for (int t = 0; t < r.eq[e].n_terms; t++) { printf("term %u %lld", r.eq[e].target, (long long)r.eq[e].t[t].coef); for (int i = 0; i < r.n_vars; i++) printf(" %u", r.eq[e].t[t].ex[i]); for (int c = 0; c < r.n_channels; c++) printf(" %u", r.eq[e].t[t].ex[PD0_MAX_VARS + c]); printf("\n"); }
            printf("end\n"); fflush(stdout);
        } else if (!strncmp(line, "ep ", 3)) {
            char *p = line + 3; int no = (int)strtol(p, &p, 10); int64_t s[PD0_MAX_OBS] = { 0 }, h = 0, ns[PD0_MAX_OBS], nh, obs[PD0_MAX_OBS];
            for (int j = 0; j < no; j++) s[j] = strtoll(p, &p, 10);
            int n = (int)strtol(p, &p, 10); printf("traj %d\n", n);
            for (int st = 0; st < n; st++) { int ch = (int)strtol(p, &p, 10); int64_t v = strtoll(p, &p, 10); int64_t u[PD0_MAX_CH] = { 0 }; if (ch >= 0 && ch < PD0_MAX_CH) u[ch] = v;
                if (pd0_gen_step(&g, s, h, u, d.dt_micro, ns, &nh, obs, &dummy_noise, &dummy_null)) { printf("oob\n"); break; }
                memcpy(s, ns, sizeof s); h = nh; for (int j = 0; j < no; j++) printf("%s%lld", j ? " " : "", (long long)s[j]); printf("\n"); }
            printf("end\n"); fflush(stdout);
        } else if (!strncmp(line, "quit", 4)) break;
    }
    return 0;
}
