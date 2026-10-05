/* PD-0 scorer (spec section 6). Holds generator-side truth; never linked into a learner. */
#ifndef PD0_SCORE_H
#define PD0_SCORE_H
#include "pd0_fmt.h"
#include "pd0_codes.h"
#define PD0_MAX_TRUE_TERMS 32
#define PD0_MAX_FIT 65536u
typedef struct { uint8_t target; uint8_t expo[PD0_MAX_VARS + PD0_MAX_CHAN]; int64_t coef; } pd0_true_term;
typedef struct {
    uint8_t n_obs, n_channels;
    uint32_t size_bound;                       /* 6.2 S*+2 */
    int64_t inbox_bound, extrap_bound, onestep_bound; /* 6.1, micro; onestep_bound 0 = not scored */
    int score_constants; int64_t const_tol_ppm;  /* 6.3 */
    int require_latent; int64_t latent_ref_factor; /* L6: reference must fail by this factor */
    uint32_t n_true_terms; pd0_true_term true_terms[PD0_MAX_TRUE_TERMS];
} pd0_score_params;
typedef struct { int64_t init[PD0_MAX_OBS]; pd0_step steps[PD0_MAX_STEPS]; int64_t truth[PD0_MAX_STEPS][PD0_MAX_OBS]; uint8_t in_box; } pd0_score_episode;
typedef struct { int64_t before[PD0_MAX_OBS]; uint8_t chan; int64_t value; int64_t after[PD0_MAX_OBS]; } pd0_transition;
/* learner-side prediction interfaces (observed variables only) */
typedef int (*pd0_rollout_fn)(void *ctx, const int64_t *init, const pd0_step *steps, uint32_t n, int64_t *out);
typedef int (*pd0_predict_fn)(void *ctx, const int64_t *state, uint8_t chan, int64_t value, int64_t *next);
typedef struct { int code; uint32_t fail_mask; int64_t inbox_nrmse, extrap_nrmse, onestep_nrmse, ref_nrmse; uint32_t size, bits; } pd0_score_result;
#define PD0_FAIL_BIT(code) (1u << ((code) - 300))
/* declared_stored_numbers: count of stored numbers in any non-parametric component (spec 6.2: one term each) */
int pd0_score(const pd0_score_params *P, const pd0_rel *rel, uint32_t declared_stored_numbers,
              pd0_rollout_fn roll, pd0_predict_fn pred, void *ctx,
              const pd0_score_episode *eps, uint32_t n_eps,
              const pd0_transition *fit, uint32_t n_fit, pd0_score_result *out);
/* relation-backed predictors (ctx = const pd0_rel *) */
int pd0_rel_rollout_fn(void *ctx, const int64_t *init, const pd0_step *steps, uint32_t n, int64_t *out);
int pd0_rel_predict_fn(void *ctx, const int64_t *state, uint8_t chan, int64_t value, int64_t *next);
/* harness reference: greedy sparse least squares over the degree<=3 monomial library, latent-free */
int pd0_reference_fit(uint8_t n_obs, uint8_t n_channels, uint32_t max_terms_per_eq, const pd0_transition *fit, uint32_t n_fit, pd0_rel *out);
#endif
