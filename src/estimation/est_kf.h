/* EST-1 linear Kalman filter: the first estimator behind the EST-0 contract.
 *
 * Pure functions over est_types.h records. Deterministic for identical inputs
 * (build with -ffp-contract=off -fno-fast-math), bounded (EST_MAX_DIM), no
 * allocation, no globals. Inputs are const; every output is a new record.
 *
 * predict: x' = F x (+ B u), P' = F P F^T + Q, repeated `horizon` times.
 *          Also fills y_mean = H x', S = H P' H^T + R.
 * update:  nu = z - H x', S = H P' H^T + R_obs, K = P' H^T S^-1,
 *          x = x' + K nu, P = (I - K H) P' (I - K H)^T + K R_obs K^T (Joseph),
 *          then P is symmetrized. R_obs is the observation's declared noise.
 * A missing observation is simply no update: the prediction stands and its
 * covariance keeps growing through repeated predict.
 *
 * Refusals (never silently repaired): stale generation or parent digest,
 * model digest mismatch, unit mismatch, non-finite input or output,
 * asymmetric / non-PSD covariance, singular S, time that does not advance. */
#ifndef OMEGA_EST_KF_H
#define OMEGA_EST_KF_H

#include "est_types.h"

/* Declared prior: evidence_root zero, parent zero, generation 0. */
est_status est_kf_prior(const est_model *mdl, const double *x0, const double *P0,
                        int64_t t_ns, est_belief *out);

/* Optional known input u (length n, already multiplied by B); may be NULL. */
est_status est_kf_predict(const est_model *mdl, const est_belief *b,
                          const double *Bu, uint32_t horizon, est_prediction *out);

/* The prediction is recomputed from prior_belief, mdl and the control input it
 * records, and must equal the supplied one bit for bit (else EST_ERR_STALE;
 * EST_ERR_MODEL for a different model). Observation refusals: EST_ERR_KIND if
 * source is zero or evidence is zero or equals a digest of the prediction, the
 * prior belief, its parent, its evidence root or its model. The estimator cannot
 * check that evidence digests match raw bytes: that binding is the caller's. */
/* Needs the belief the prediction came from (checked by digest) so the
 * posterior can extend its evidence root. The observation is only read. */
est_status est_kf_update(const est_model *mdl, const est_belief *prior_belief,
                         const est_prediction *pred, const est_observation *obs,
                         est_belief *posterior, est_innovation *innovation);

/* Turn a prediction into a belief without an observation (for a missing
 * measurement); generation and parent follow the prediction. */
est_status est_kf_coast(const est_model *mdl, const est_prediction *pred,
                        const est_belief *prior_belief, est_belief *out);

#endif /* OMEGA_EST_KF_H */
