/* pd0-learner: deterministic offline fit over a raw PD0REC1 stream on stdin
 * (records are treated as FIT unless an episode index appears in the optional
 * SELECT list). Prints the learner report and the PDLAW1 bytes of the best
 * candidate as hex. Used by the determinism test; holds no world knowledge.
 * Usage: pd0-learner <n_obs> <n_channels> [select-every-k] < records */
#include "pd0_learner.h"
#include "pd0_codes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: pd0-learner <n_obs> <n_channels> [select-every-k]\n"); return 2; }
    pd0l_desc d; memset(&d, 0, sizeof d); d.n_obs = (uint8_t)atoi(argv[1]); d.n_channels = (uint8_t)atoi(argv[2]); int k = argc > 3 ? atoi(argv[3]) : 4;
    for (int c = 0; c < d.n_channels; c++) { d.chan_min[c] = -2000000; d.chan_max[c] = 2000000; } for (int j = 0; j < d.n_obs; j++) { d.reset_min[j] = -2000000; d.reset_max[j] = 2000000; }
    d.dt_micro = 50000; d.episode_max_steps = 100; d.budget_steps = 3000; d.budget_episodes = 300;
    pd0_learner *L = pd0_learner_new(&d, 1); if (!L) return 2;
    static uint8_t buf[1 << 22]; size_t n = fread(buf, 1, sizeof buf, stdin), off = 0; pd0_rec r; size_t used;
    while (off < n && pd0_rec_parse(buf + off, n - off, &r, &used) == PD0V_OK) { pd0_learner_observe(L, &r, (k > 0 && (int)(r.episode % (uint32_t)k) == k - 1) ? TAG_SELECT : TAG_FIT); off += used; }
    pd0_learner_fit(L);
    static char jb[1 << 20]; pd0_learner_report(L, jb, sizeof jb); puts(jb);
    const pd0l_candidate *c = pd0_learner_candidate(L, 0); if (c) { uint8_t law[8192]; pd0_rel rel = c->rel; size_t ln = pd0_rel_write(&rel, law, sizeof law); printf("PDLAW1 "); for (size_t i = 0; i < ln; i++) printf("%02x", law[i]); printf("\n"); }
    pd0_learner_free(L); return 0;
}
