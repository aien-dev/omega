/* PD-0 independent ladder checker (spec section 5, 7). The learner proposes;
 * this replays the evidence ledger and derives the state itself. */
#ifndef PD0_LADDER_H
#define PD0_LADDER_H
#include "pd0_fmt.h"
#include "pd0_codes.h"

enum { LS_START = 0, LS_OBSERVATION, LS_CORRELATION, LS_CANDIDATE, LS_HYPOTHESIS, LS_PREDICTED, LS_INTERVENED, LS_REPLICATED };
const char *pd0_ladder_state_name(int s);

typedef struct {
    uint8_t n_obs, n_channels, noisy;
    int64_t chan_min[PD0_MAX_CHAN], chan_max[PD0_MAX_CHAN];
    int64_t reset_min, reset_max; uint32_t episode_len; int64_t dt_micro;
    uint32_t size_bound;          /* spec 6.2 S*+2 for the level */
    int64_t eps_bound_micro;      /* spec 6.1 in-box bound (noisy levels: 0.05 cap) */
    uint32_t min_t1_records, min_t1_episodes, min_prereg, min_batches, min_batch_episodes, rollout_steps;
} pd0_ladder_params;
void pd0_ladder_params_default(pd0_ladder_params *P, uint8_t n_obs, uint8_t n_channels);

typedef struct {
    int state, code, transition, demoted;
    int stall_code;               /* first non-OK reason since the last successful transition */
    uint32_t n_refutations, p, f, confidence_ppm;
    int have_candidate; pd0_rel candidate; uint8_t candidate_hash[PD0_HASH];
    int64_t eps_micro;
    pd0_domain dom;
    uint32_t n_exceptions; pd0_exception exc[PD0_MAX_EXC];
    uint32_t n_experiments; pd0_experiment exp[PD0_MAX_EXP];
    uint8_t chain_root[PD0_HASH];
    uint64_t n_entries, n_records;
    int64_t last_nrmse_micro;     /* diagnostics */
    uint32_t n_void_episodes, n_void_batches;   /* spec rev 7: void TRIAL/REP episodes and batches (never in p or f) */
    double last_r, last_p;
} pd0_ladder_report;

/* Replays the ledger. Returns the first fatal code (format/chain/dishonesty) or
 * PD0V_OK; non-fatal "stay" conditions leave the state where it is and the
 * reason in R->code. */
int pd0_ladder_check(const uint8_t *ledger, size_t len, const pd0_ladder_params *P, pd0_ladder_report *R);
/* T8: verify a submitted PDLAW1 against the replayed state. */
int pd0_ladder_verify_law(const pd0_ladder_report *R, const pd0_ladder_params *P, const uint8_t *law, size_t law_len);
/* Build the law record the evidence supports (used by tests and the harness,
 * never by the learner). state is PROVISIONAL_LAW only when R->state == REPLICATED. */
size_t pd0_ladder_emit_law(const pd0_ladder_report *R, const pd0_ladder_params *P, uint8_t *out, size_t cap);
/* generated claim text; returns length */
uint32_t pd0_claim_text(const pd0_ladder_report *R, const pd0_ladder_params *P, char *out, size_t cap);
uint32_t pd0_confidence_ppm(uint32_t p, uint32_t f);
/* statistics exported for tests (the same code the checker uses) */
double pd0_pearson(const double *x, const double *y, uint32_t n);
double pd0_perm_p(const double *x, const double *y, uint32_t n, uint32_t n_shuffles, uint64_t seed);
/* NRMSE over pooled steps: pred/obs are [n][n_obs] in micro; returns micro (1e6 = 1.0) */
int64_t pd0_nrmse_micro(const int64_t *pred, const int64_t *obs, uint32_t n, uint8_t n_obs);
/* spec rev 6 (a): per-variable RMSE over the pooled FIT sd (micro); sd 0 falls back to the RMSE */
int64_t pd0_nrmse_pooled_micro(const int64_t *pred, const int64_t *obs, uint32_t n, uint8_t n_obs, const int64_t *sd_micro);
#endif
