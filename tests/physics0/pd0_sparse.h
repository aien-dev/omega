/* Reference sparse solver (spec section 11 step 3: "one plain reference
 * solver (a sparse least-squares fit over the monomial library)"). Test-only
 * calibration code, not a learner: it is given the transitions and the
 * variable counts, nothing else. Library: every monomial of total degree <= 3
 * in the observed variables, plus each channel linearly. Per target: greedy
 * forward selection (orthogonal matching pursuit) on FIT episodes; the term
 * count is chosen on SELECT episodes by minimum total bits =
 * description_bits (spec 6.2 rule) + n_select * log2(RSS / n_select + eps). */
#ifndef PD0_SPARSE_H
#define PD0_SPARSE_H

#include "pd0_calib.h"

#define PD0_SPARSE_MAX_TERMS 6

/* FIT = episodes with episode % 5 in {0,1,2}, SELECT = episode % 5 == 3,
 * HOLDOUT (episode % 5 == 4) is never touched. Returns 0 ok. */
int pd0_sparse_fit(const pd0_gather *g, int n_obs, int n_ch, pd0_relation *out);

#endif
