/* Test-only calibration helpers for PD-0 G1 (spec 4.2 V1-V3 and section 11
 * step 3). These compute the section 6.1 numbers for a GIVEN relation against
 * the generator truth so the frozen thresholds can be calibrated. This is not
 * the scorer component (another lane owns src/physics0/score); it links the
 * generator on purpose (harness side) and is never linked into learner code. */
#ifndef PD0_CALIB_H
#define PD0_CALIB_H

#include <stdint.h>

#include "physics0/pd0_gen.h"
#include "physics0/pd0_relation.h"

#define PD0_CALIB_EPISODES 20
#define PD0_CALIB_STEPS 20
#define PD0_GATHER_MAX 8192

typedef struct {
    double nrmse_inbox;       /* 20-step rollout, 10 episodes reset in the reset box (spec 6.1 rev 3) */
    double nrmse_extrap;      /* 20-step rollout, 10 episodes reset in 1.5 x the reset box */
    double nrmse_onestep;     /* one-step from the true state, all 20 episodes */
    int truth_oob_episodes;   /* truth left +-10 inside a scoring episode (steps after that dropped) */
} pd0_calib_result;

/* Section 6.1 held-out scoring episodes from the "score" stream of seed:
 * initial state noise-free, schedule uniform in the channel bounds. */
void pd0_calib_heldout(const pd0_gen *g, const pd0_desc *d, uint64_t seed, const pd0_relation *rel,
                       pd0_calib_result *out);

/* one OK transition as the learner would see it */
typedef struct { int64_t before[PD0_MAX_OBS], after[PD0_MAX_OBS], u[PD0_MAX_CH]; uint32_t episode; } pd0_transition;
typedef struct { pd0_transition *t; int n; int episodes; int oob_episodes; int refused; } pd0_gather;

/* Drive a fresh world (level, seed) with a uniform random schedule drawn from
 * the "gather" stream of seed: random reset in the reset box, one random
 * channel value per step, episodes of episode_max_steps, until a budget is
 * spent. Records every status = OK transition. oob_episodes / episodes is the
 * V3 breach rate. Caller frees out->t. Returns 0 ok. */
int pd0_calib_gather(int level, uint64_t seed, pd0_gather *out);

/* least squares: fit y ~ X b (n rows, p cols, row-major X), returns 0 ok */
int pd0_lstsq(const double *X, const double *y, int n, int p, double *b);

#endif
