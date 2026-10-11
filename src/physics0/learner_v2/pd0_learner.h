/* PD-0 learner v2 (adaptive-intelligence milestone 3, gap G3). A NEW version beside the
 * seal-1 learner (src/physics0/learner is frozen and untouched). Differences from v1:
 * (1) candidates carry a 20-step rollout error on the SELECT episodes and the ranking
 * score adds its code length; (2) the hidden-state flag also fires on rollout/one-step
 * inconsistency; (3) an optional prior relation can be injected as a ranked candidate.
 * Everything else is the v1 text.
 *
 * PD-0 learner (Physics-0 Discovery Engine, Direction 4): the first genuine
 * learner. It sees only the describe record and PD0REC1 observation records
 * the harness forwards (FIT and SELECT episodes), fits compact one-step
 * relations over a bounded monomial vocabulary, keeps several candidates,
 * and emits evidence for the independent ladder checker. It never declares
 * a ladder state, never reads HOLDOUT/TRIAL/REP raw records, holds no file,
 * no generator symbol, no level identity. Formats come from pd0_fmt.h
 * (byte layouts only). Floating point is used only inside fitting; every
 * serialised number is a micro-unit integer. */
#ifndef PD0_LEARNER_H
#define PD0_LEARNER_H
#include "pd0_fmt.h"

#define PD0L_MAX_TRANS 65536u
#define PD0L_MAX_LIB 128u
#define PD0L_MAX_CAND 8
#define PD0L_MAX_SEL 10u          /* terms per equation the search may select */
#define PD0L_MAX_FORBID 4096u
#define PD0L_ROLL_H 20u                 /* rollout horizon on SELECT episodes (the ladder uses 20-step rollouts too) */
#define PD0L_ROLL_CLAMP 100000000       /* 100 units: a diverging rollout is clamped here so integer arithmetic cannot overflow */
#ifndef PD0L_DET_BASE_EP
#define PD0L_DET_BASE_EP 32u            /* episodes in the detector's reference block */
#endif
#ifndef PD0L_DET_SLACK
#define PD0L_DET_SLACK 0.3              /* CUSUM allowance (nats): an episode must be about 1.35x worse than the reference error to push the sum up; tuned on dev seeds 101-150 and 201-250, never the evaluation seeds 1-25 */
#endif
#ifndef PD0L_DET_CLIP
#define PD0L_DET_CLIP 5.0               /* per-episode log-ratio cap: one episode alone can never raise an alarm */
#endif
#ifndef PD0L_DET_H
#define PD0L_DET_H 8.0                  /* CUSUM alarm threshold */
#endif
#ifndef PD0L_DET_FLOOR
#define PD0L_DET_FLOOR 0.0001           /* units: floor of the reference residual scale, so a noise-free exact model has a finite scale */
#endif

typedef struct {
    uint8_t n_obs, n_channels;
    int64_t dt_micro;
    int64_t chan_min[PD0_MAX_CHAN], chan_max[PD0_MAX_CHAN];
    int64_t reset_min[PD0_MAX_OBS], reset_max[PD0_MAX_OBS];   /* per variable (PD0DESC2); a one-pair describe is broadcast by the harness */
    uint32_t episode_max_steps, budget_steps, budget_episodes;
} pd0l_desc;

typedef struct {
    pd0_rel rel;                 /* serialisable relation (coefficients in micro) */
    uint32_t bits;               /* description_bits (spec 6.2 rule) */
    uint32_t size;               /* |R| */
    int64_t fit_nrmse_micro;     /* one-step NRMSE on FIT (integer evaluator) */
    int64_t select_nrmse_micro;  /* one-step NRMSE on SELECT (integer evaluator) */
    double mdl_bits;             /* description bits + residual code length on SELECT */
    double lag1_autocorr[PD0_MAX_OBS]; /* residual lag-1 autocorrelation on FIT per variable */
    int64_t select_roll_nrmse_micro; /* v2: 20-step rollout NRMSE over the SELECT episodes (latent starts at 0) */
    double roll_bits;            /* v2: residual code length of those rollouts (added to mdl_bits to give rank_bits) */
    double rank_bits;            /* v2: mdl_bits + roll_bits, the ranking score */
    int hidden_by_rollout;       /* v2: rollout error is large and out of proportion to the one-step error */
    int hidden_state_suspected;  /* residuals carry memory the vocabulary cannot express (v1 rule OR v2 rollout rule) */
    int is_prior;                /* v2: the injected prior relation */
    int is_old;                  /* v2.1: the model held before a detected change, kept as a rival candidate */
    int is_null;
} pd0l_candidate;

typedef struct pd0_learner pd0_learner;

pd0_learner *pd0_learner_new(const pd0l_desc *d, uint64_t seed);
void pd0_learner_free(pd0_learner *L);
/* a FIT or SELECT record (tag TAG_FIT / TAG_SELECT); others are refused (-1) */
int pd0_learner_observe(pd0_learner *L, const pd0_rec *r, uint8_t tag);
/* the records of an episode that was TRIAL/REP and is now FIT after a refutation */
uint32_t pd0_learner_n_transitions(const pd0_learner *L, uint8_t tag);
/* exploration: seeded random reset + schedule inside the describe bounds */
void pd0_learner_explore(pd0_learner *L, uint32_t episode_index, int64_t *reset, pd0_step *steps, uint32_t n_steps);
/* fit: rebuilds the candidate set from FIT, ranked by MDL on SELECT. returns count (>= 1: the null is always present) */
/* v2: inject (rel != NULL) or clear (NULL) a prior relation; fit() ranks it with the fitted candidates on the same SELECT data */
void pd0_learner_set_prior(pd0_learner *L, const pd0_rel *rel);
/* v2.1: change detection and bounded forgetting from admissible observations only. When on, fit() first runs a residual
 * detector (CUSUM on the per-episode one-step prediction error of a reference model fitted on the first PD0L_DET_BASE_EP
 * episodes, scaled by that model's error on those episodes' SELECT data). On an alarm the fit window starts at the estimated change
 * episode (older episodes are forgotten) and the model held before the change stays in the ranking as a rival candidate. */
typedef struct {
    int enabled, alarm;
    uint32_t n_ep, base_ep;          /* episodes seen; episodes in the reference block (0 if the detector had too little data) */
    uint32_t alarm_ep, change_ep;    /* 0-based episode index of the alarm and of the estimated first changed episode (valid if alarm) */
    double cusum_max;                /* largest CUSUM value reached over the monitored episodes */
    int old_kept;                    /* the pre-change model was non-null and entered the ranking */
} pd0l_detect;
void pd0_learner_set_forgetting(pd0_learner *L, int on);
const pd0l_detect *pd0_learner_detection(const pd0_learner *L);
int pd0_learner_fit(pd0_learner *L);
int pd0_learner_n_candidates(const pd0_learner *L);
const pd0l_candidate *pd0_learner_candidate(const pd0_learner *L, int i);   /* 0 = best */
/* the strongest (variable or intervention, one-step change) correlation over all visible OK step
 * records, with a permutation test (recorded seed). returns 0 or -1 (too few records). */
int pd0_learner_correlation(const pd0_learner *L, uint64_t shuffle_seed, uint32_t n_shuffles, pd0_corr *out);
/* planner stand-in (Direction 5 owns the real one): among n_random seeded schedules inside the
 * bounds, not in the forbidden hash list, pick the one where the hypotheses disagree most.
 * hyps[0] is the candidate. returns divergence in micro (D * 1e6), or -1 if none (NO_DISCRIMINATING_EXPERIMENT). */
int64_t pd0_learner_propose(const pd0_learner *L, const pd0_rel *hyps, int n_hyp, uint64_t planner_seed, uint32_t n_random,
                            const uint8_t (*forbidden)[PD0_HASH], uint32_t n_forbidden, pd0_exp *out);
/* a random schedule from a named stream (used for the random half of replication) */
void pd0_learner_random_schedule(const pd0l_desc *d, uint64_t seed, const char *tag, uint32_t idx, int64_t *reset, pd0_step *steps, uint32_t n);
/* prediction interface over a relation (observed variables in and out; latent starts at 0) */
void pd0_learner_rollout(const pd0_rel *rel, const int64_t *init, const pd0_step *steps, uint32_t n, int64_t *out);
/* one-step NRMSE of rel over the learner's records of a tag (integer evaluator, latent carried per episode) */
int64_t pd0_learner_onestep_nrmse(const pd0_learner *L, const pd0_rel *rel, uint8_t tag);
/* human report (JSON-ish, non-canonical) */
size_t pd0_learner_report(const pd0_learner *L, char *buf, size_t cap);
#endif
