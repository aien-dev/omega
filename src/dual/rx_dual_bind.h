/* DUAL-0b: binding DUAL inputs to ESTIMATION records (ARCH-0020) and
 * classifying their freshness (ADR 0031 sections 4.2, 5.2, 8.1).
 *
 * DUAL never reads a raw counter and never computes an estimate. Every
 * estimate and uncertainty in a DUAL record is COPIED from an est_* record
 * whose digest is cited (estimate_ref), together with that record's World
 * generation and evidence root. The dependency direction is fixed:
 *
 *   exact counters -> est_observation -> ESTIMATION -> est_belief / est_prediction
 *                  -> RxDualInput (this file) -> controller (rx_dual_update.h)
 *
 * Units are converted from est_unit to RxDualUnit by one explicit table
 * (rx_dual_unit_from_est); an unknown or NONE unit is refused.
 *
 * Evidence verification reuses est_evidence_root_extend: a belief's root must
 * equal extend(parent_root, digest(observation)); only then is the input
 * marked evidence_verified. An unverified input can only yield STALE. */
#ifndef OMEGA_RX_DUAL_BIND_H
#define OMEGA_RX_DUAL_BIND_H

#include "rx_dual.h"
#include "est_types.h"

/* What the controller may read from ESTIMATION for one resource at one tick. */
typedef struct {
    RxDualDigest estimate_ref;        /* digest of the est record the values come from */
    RxDualDigest observation_ref;     /* digest of the observation (MEASURED), else zero */
    RxDualEstimateKind estimate_kind;
    RxDualUnit unit;                  /* converted from the record's est_unit */
    double estimate;                  /* copied component value */
    double uncertainty;               /* sqrt of the component variance, copied */
    uint64_t generation;              /* World generation of the record */
    RxDualDigest evidence_root;       /* the record's evidence root (non-zero) */
    uint32_t evidence_verified;       /* 1 only after rx_dual_verify_belief_evidence passed */
    RxDualDigest calibration_ref;     /* EST-3 receipt digest for this signal; zero = UNCALIBRATED */
    uint32_t regime_change;           /* 1 = out-of-calibration (EST-9 style) signal: freeze */
} RxDualInput;

RxDualStatus rx_dual_unit_from_est(est_unit u, RxDualUnit *out);

/* Bind component `k` of a belief. calibration_ref may be NULL (UNCALIBRATED).
 * Refuses an invalid belief, k >= n, a zero evidence root (a declared prior
 * is not evidence), or a negative variance. evidence_verified starts at 0. */
RxDualStatus rx_dual_bind_belief(const est_belief *b, uint32_t k, const RxDualDigest *calibration_ref,
                                 RxDualInput *out);
/* Bind component `k` of a prediction. The model (for the unit) and the prior
 * belief (for the evidence root) must be the records the prediction cites:
 * their digests must equal p->model and p->prior, else RX_DUAL_ERR_DIGEST. */
RxDualStatus rx_dual_bind_prediction(const est_prediction *p, const est_model *model, const est_belief *prior,
                                     uint32_t k, const RxDualDigest *calibration_ref, RxDualInput *out);
/* Bind component `k` of an observation as a MEASURED input. An observation
 * carries no World generation, so the caller states it. */
RxDualStatus rx_dual_bind_observation(const est_observation *o, uint32_t k, uint64_t generation,
                                      const RxDualDigest *calibration_ref, RxDualInput *out);

/* Verify a belief's evidence root against the parent root and the observation
 * that produced it (root' = extend(parent_root, digest(obs))). On success sets
 * in->evidence_verified = 1; on mismatch returns RX_DUAL_ERR_EVIDENCE and
 * leaves it 0. `in` must have been bound from `b` (estimate_ref must match). */
RxDualStatus rx_dual_verify_belief_evidence(const est_belief *b, const est_digest *parent_root,
                                            const est_observation *obs, RxDualInput *in);

/* Structural validity of an input (finite, unit known, kind known, digests present). */
RxDualStatus rx_dual_check_input(const RxDualInput *in);

/* Classify an input for the next tick (ADR 0031 section 5.2):
 *   REFUSED       nonfinite or negative values, missing digests, generation in the future
 *   STALE         now - generation > max_age, generation older than prev, or evidence not verified
 *   FROZEN        regime_change set
 *   UNCALIBRATED  no calibration_ref
 *   FRESH         otherwise
 * Unit mismatch against prev is a structural refusal (RX_DUAL_ERR_UNIT), not a state.
 * prev may be NULL for an initial state. */
RxDualStatus rx_dual_classify_input(const RxDualInput *in, const RxDualConstraintState *prev,
                                    const RxDualController *ctl, uint64_t now_generation,
                                    RxDualLambdaState *out);

/* DUAL state evidence chain: root' = est_evidence_root_extend(root, estimate_ref). */
RxDualStatus rx_dual_evidence_extend(const RxDualDigest *root, const RxDualDigest *estimate_ref, RxDualDigest *out);

#endif /* OMEGA_RX_DUAL_BIND_H */
