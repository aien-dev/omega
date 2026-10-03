/* PD-0 negative-control toolkit (spec 9.1). Test infrastructure; never linked into a learner. */
#ifndef PD0_CONTROLS_H
#define PD0_CONTROLS_H
#include "pd0_fmt.h"
#include "pd0_score.h"
/* NC-1: null-world records: every post-step observation is a fresh Irwin-Hall draw, sd 0.5, "null" stream */
uint32_t pd0_null_world(uint64_t seed, uint8_t n_obs, uint32_t n_episodes, uint32_t steps, int64_t reset_min, int64_t reset_max, int64_t chan_min, int64_t chan_max, pd0_rec *out, uint32_t cap);
/* NC-2: permute vars_after across OK step records (recorded seed), then rebuild the chain */
void pd0_shuffle_outcomes(pd0_rec *recs, uint32_t n, uint64_t seed);
void pd0_rechain(pd0_rec *recs, uint32_t n);
/* NC-3 mutants */
void pd0_mut_scale_coef(pd0_rel *r, uint8_t target, const uint8_t *expo, int64_t num, int64_t den);   /* M1 */
void pd0_mut_pad(pd0_rel *r, uint32_t n_extra, int64_t tiny_coef);                                    /* M2 */
/* M3: nearest-neighbour memoriser over stored transitions */
typedef struct { const pd0_transition *t; uint32_t n; uint8_t n_obs; } pd0_memoriser;
int pd0_memoriser_rollout(void *ctx, const int64_t *init, const pd0_step *steps, uint32_t n, int64_t *out);
int pd0_memoriser_predict(void *ctx, const int64_t *state, uint8_t chan, int64_t value, int64_t *next);
uint32_t pd0_memoriser_stored_numbers(const pd0_memoriser *m);
#endif
