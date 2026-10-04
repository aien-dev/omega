/* PD-0 experiment planner (spec section 8, revision 5). Direction 5.
 *
 * The planner answers one question: given two or more plausible relations,
 * which bounded experiment makes them disagree most? It PROPOSES only. It has
 * no execution path: the proposal is a PD0EXP1 preregistration record (the
 * verifier-owned layout in pd0_fmt.h) that the recorder must chain ahead of
 * any TRIAL record before the schedule may run (spec behaviour 6).
 *
 * Inputs are learner-visible only: the hypothesis set as relationship blocks,
 * the visited domain, the remaining budget, the describe bounds and a seed.
 * No generator, oracle or authority symbol is referenced (purity check in
 * mk/physics0-planner.mk). Floating point is used nowhere in this file: the
 * divergence and cost are integer micro-unit arithmetic (128-bit intermediate). */
#ifndef PD0_PLANNER_H
#define PD0_PLANNER_H

#include <stddef.h>
#include <stdint.h>
#include "pd0_fmt.h"

#define PD0P_CANDIDATES 256u        /* spec 8: at least 256 seeded random schedules */
#define PD0P_REFINE_TOP 16u         /* spec 8: local refinement of the top 16 */
#define PD0P_REFINE_ROUNDS 8u       /* implementation choice, recorded in the plan record */
#define PD0P_MAX_PRIOR 4096u
#define PD0P_MIN_D_MICRO 3000000LL  /* spec 8 behaviour 4: best D < 3 means no discriminating experiment */

/* status of a proposal / result of validation */
enum {
    PD0P_OK = 0,
    PD0P_NO_DISCRIMINATING_EXPERIMENT = 1,
    PD0P_ERR_BOUNDS = -1,     /* schedule outside the describe bounds: a planner defect (behaviour 2) */
    PD0P_ERR_DUPLICATE = -2,  /* schedule hash already used (behaviour 3) */
    PD0P_ERR_INPUT = -3,      /* malformed input or hash that does not recompute */
    PD0P_ERR_BUDGET = -4      /* fewer than 2 steps of budget left */
};

typedef struct {
    uint8_t n_obs, n_channels;
    int64_t chan_min[PD0_MAX_CHAN], chan_max[PD0_MAX_CHAN];
    int64_t reset_min[PD0_MAX_OBS], reset_max[PD0_MAX_OBS];   /* PD0DESC2, per variable */
    uint32_t episode_max_steps;
} pd0p_bounds;

/* visited range per observed variable (from the observed_domain block) */
typedef struct { int64_t var_min[PD0_MAX_OBS], var_max[PD0_MAX_OBS]; } pd0p_domain;

typedef struct {
    uint8_t n_hyp;                      /* 2..PD0_MAX_HYP; hyp[0] is the best candidate */
    const pd0_rel *hyp[PD0_MAX_HYP];
    int64_t residual_sd_micro;          /* pooled residual sd of the hypotheses on SELECT, micro, > 0 */
} pd0p_hyps;

typedef struct { uint32_t n; uint8_t hash[PD0P_MAX_PRIOR][PD0_HASH]; } pd0p_prior;

typedef struct {
    int status;                 /* PD0P_OK or PD0P_NO_DISCRIMINATING_EXPERIMENT (negative: error) */
    pd0_exp exp;                /* the PD0EXP1 preregistration (schedule, expected trajectories, D, hash) */
    int64_t divergence_micro;   /* D in micro-units (1_000_000 = one residual sd) */
    int64_t info_proxy_micro;   /* sum over steps of (max pair difference / r)^2, micro */
    int64_t cost_micro;         /* total |intervention| over the schedule, micro-units */
    uint32_t n_candidates, n_refined, n_skipped_duplicate;
    uint8_t outside_middle;     /* 1 if the schedule starts or reaches outside the middle 50% of the visited range of some variable */
    uint64_t seed;
} pd0p_proposal;

/* Behaviours 1 to 6 of spec section 8. Returns the status (also stored in out->status). */
int pd0p_propose(const pd0p_bounds *b, const pd0p_domain *dom, const pd0p_hyps *h,
                 uint32_t budget_steps, const pd0p_prior *prior, uint64_t seed, pd0p_proposal *out);

/* Independent check of any PD0EXP1 against the bounds and the prior hashes:
 * 0 ok, PD0P_ERR_BOUNDS, PD0P_ERR_DUPLICATE, PD0P_ERR_INPUT (hash does not recompute). */
int pd0p_validate(const pd0p_bounds *b, const pd0p_prior *prior, const pd0_exp *e);

/* PD0EXP1 bytes for the recorder to chain before any TRIAL record. 0 on error. */
size_t pd0p_prereg_bytes(const pd0p_proposal *p, uint8_t *out, size_t cap);

/* PD0PLN1: planner side record for receipts (fixed 84 bytes, little-endian):
 * magic "PD0PLN1\0", version u16 = 1, status u8, n_candidates u32, n_refined u32,
 * seed u64, divergence i64, info_proxy i64, cost i64, outside_middle u8, schedule_hash 32. */
#define PD0P_PLAN_SIZE 84u
size_t pd0p_plan_write(const pd0p_proposal *p, uint8_t out[PD0P_PLAN_SIZE]);
int pd0p_plan_parse(const uint8_t *in, size_t len, pd0p_proposal *p);   /* fills status, counters, hash only */

/* Preregistered passive baseline: the index-th uniform random schedule of a
 * fixed stream. Deterministic, within bounds, no hypothesis is consulted. */
int pd0p_passive(const pd0p_bounds *b, uint32_t budget_steps, uint64_t stream_seed, uint32_t index, pd0_exp *out);

void pd0p_prior_add(pd0p_prior *p, const uint8_t hash[PD0_HASH]);
int  pd0p_prior_has(const pd0p_prior *p, const uint8_t hash[PD0_HASH]);

/* Run-level check of behaviour 5 (provisional 20% rule): fraction of proposals
 * flagged outside_middle, in ppm. The caller decides what to do with it. */
uint32_t pd0p_outside_fraction_ppm(const uint8_t *flags, uint32_t n);

#endif
