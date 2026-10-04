/* DUAL-0b: binding to ESTIMATION records and freshness classification. */
#include "rx_dual_bind.h"

#include <math.h>
#include <string.h>

static void cp(RxDualDigest *d, const est_digest *e) { memcpy(d->b, e->b, RX_DUAL_DIGEST_SIZE); }

RxDualStatus rx_dual_unit_from_est(est_unit u, RxDualUnit *out)
{
    if (!out) return RX_DUAL_ERR_NULL;
    switch (u) {
    case EST_UNIT_MILLI_CELSIUS:       *out = RX_DUAL_UNIT_MILLI_CELSIUS; return RX_DUAL_OK;
    case EST_UNIT_MILLI_CELSIUS_PER_S: *out = RX_DUAL_UNIT_MILLI_CELSIUS_PER_S; return RX_DUAL_OK;
    case EST_UNIT_WATT:                *out = RX_DUAL_UNIT_WATT; return RX_DUAL_OK;
    case EST_UNIT_WATT_PER_S:          *out = RX_DUAL_UNIT_WATT_PER_S; return RX_DUAL_OK;
    case EST_UNIT_NANOSECOND:          *out = RX_DUAL_UNIT_NS; return RX_DUAL_OK;
    case EST_UNIT_BYTE:                *out = RX_DUAL_UNIT_BYTES; return RX_DUAL_OK;
    case EST_UNIT_DIMENSIONLESS:       *out = RX_DUAL_UNIT_DIMENSIONLESS; return RX_DUAL_OK;
    case EST_UNIT_LOG2_PICOSECOND:     *out = RX_DUAL_UNIT_LOG2_PS; return RX_DUAL_OK;
    case EST_UNIT_NONE:
    case EST_UNIT_MAX_:
    default:
        *out = RX_DUAL_UNIT_NONE;
        return RX_DUAL_ERR_UNIT;
    }
}

static RxDualStatus sd_from_var(double var, double *sd)
{
    if (!isfinite(var)) return RX_DUAL_ERR_NONFINITE;
    if (var < 0.0) return RX_DUAL_ERR_RANGE;
    *sd = sqrt(var);
    return RX_DUAL_OK;
}

RxDualStatus rx_dual_bind_belief(const est_belief *b, uint32_t k, const RxDualDigest *calibration_ref,
                                 RxDualInput *out)
{
    est_digest d;
    RxDualStatus st;
    if (!b || !out) return RX_DUAL_ERR_NULL;
    memset(out, 0, sizeof *out);
    if (est_check_belief(b) != EST_OK) return RX_DUAL_ERR_KIND;
    if (k >= b->n) return RX_DUAL_ERR_RANGE;
    if (est_digest_is_zero(&b->evidence_root)) return RX_DUAL_ERR_EVIDENCE; /* a prior is not evidence */
    if ((st = rx_dual_unit_from_est(b->unit[k], &out->unit)) != RX_DUAL_OK) return st;
    if (est_digest_belief(b, &d) != EST_OK) return RX_DUAL_ERR_KIND;
    cp(&out->estimate_ref, &d);
    out->estimate_kind = RX_DUAL_EST_ESTIMATED;
    out->estimate = b->x[k];
    if ((st = sd_from_var(b->P[k * b->n + k], &out->uncertainty)) != RX_DUAL_OK) return st;
    out->generation = b->generation;
    cp(&out->evidence_root, &b->evidence_root);
    if (calibration_ref) out->calibration_ref = *calibration_ref;
    return rx_dual_check_input(out);
}

RxDualStatus rx_dual_bind_prediction(const est_prediction *p, const est_model *model, const est_belief *prior,
                                     uint32_t k, const RxDualDigest *calibration_ref, RxDualInput *out)
{
    est_digest d;
    RxDualStatus st;
    if (!p || !model || !prior || !out) return RX_DUAL_ERR_NULL;
    memset(out, 0, sizeof *out);
    if (est_check_prediction(p) != EST_OK || est_check_model(model) != EST_OK || est_check_belief(prior) != EST_OK)
        return RX_DUAL_ERR_KIND;
    if (k >= p->n || k >= model->n) return RX_DUAL_ERR_RANGE;
    if (est_digest_model(model, &d) != EST_OK || memcmp(d.b, p->model.b, EST_DIGEST_SIZE) != 0) return RX_DUAL_ERR_DIGEST;
    if (est_digest_belief(prior, &d) != EST_OK || memcmp(d.b, p->prior.b, EST_DIGEST_SIZE) != 0) return RX_DUAL_ERR_DIGEST;
    if (est_digest_is_zero(&prior->evidence_root)) return RX_DUAL_ERR_EVIDENCE;
    if ((st = rx_dual_unit_from_est(model->state_unit[k], &out->unit)) != RX_DUAL_OK) return st;
    if (est_digest_prediction(p, &d) != EST_OK) return RX_DUAL_ERR_KIND;
    cp(&out->estimate_ref, &d);
    out->estimate_kind = RX_DUAL_EST_PREDICTED;
    out->estimate = p->x[k];
    if ((st = sd_from_var(p->P[k * p->n + k], &out->uncertainty)) != RX_DUAL_OK) return st;
    out->generation = p->generation;
    cp(&out->evidence_root, &prior->evidence_root);
    if (calibration_ref) out->calibration_ref = *calibration_ref;
    return rx_dual_check_input(out);
}

RxDualStatus rx_dual_bind_observation(const est_observation *o, uint32_t k, uint64_t generation,
                                      const RxDualDigest *calibration_ref, RxDualInput *out)
{
    est_digest d;
    RxDualStatus st;
    if (!o || !out) return RX_DUAL_ERR_NULL;
    memset(out, 0, sizeof *out);
    if (est_check_observation(o) != EST_OK) return RX_DUAL_ERR_KIND;
    if (k >= o->m) return RX_DUAL_ERR_RANGE;
    if ((st = rx_dual_unit_from_est(o->unit[k], &out->unit)) != RX_DUAL_OK) return st;
    if (est_digest_observation(o, &d) != EST_OK) return RX_DUAL_ERR_KIND;
    cp(&out->estimate_ref, &d);
    cp(&out->observation_ref, &d);
    out->estimate_kind = RX_DUAL_EST_MEASURED;
    out->estimate = o->z[k];
    if ((st = sd_from_var(o->R[k * o->m + k], &out->uncertainty)) != RX_DUAL_OK) return st;
    out->generation = generation;
    cp(&out->evidence_root, &o->evidence);   /* the raw-bytes digest the sensor path declared */
    out->evidence_verified = 1;              /* an observation is its own evidence (ARCH-0020 caller rule) */
    if (calibration_ref) out->calibration_ref = *calibration_ref;
    return rx_dual_check_input(out);
}

RxDualStatus rx_dual_verify_belief_evidence(const est_belief *b, const est_digest *parent_root,
                                            const est_observation *obs, RxDualInput *in)
{
    est_digest obs_d, want, have;
    if (!b || !parent_root || !obs || !in) return RX_DUAL_ERR_NULL;
    if (est_check_belief(b) != EST_OK || est_check_observation(obs) != EST_OK) return RX_DUAL_ERR_KIND;
    if (est_digest_belief(b, &have) != EST_OK) return RX_DUAL_ERR_KIND;
    if (memcmp(have.b, in->estimate_ref.b, EST_DIGEST_SIZE) != 0) return RX_DUAL_ERR_DIGEST; /* input not bound from b */
    if (est_digest_observation(obs, &obs_d) != EST_OK) return RX_DUAL_ERR_KIND;
    if (est_evidence_root_extend(parent_root, &obs_d, &want) != EST_OK) return RX_DUAL_ERR_EVIDENCE;
    if (memcmp(want.b, b->evidence_root.b, EST_DIGEST_SIZE) != 0) { in->evidence_verified = 0; return RX_DUAL_ERR_EVIDENCE; }
    in->evidence_verified = 1;
    return RX_DUAL_OK;
}

RxDualStatus rx_dual_check_input(const RxDualInput *in)
{
    if (!in) return RX_DUAL_ERR_NULL;
    if (!rx_dual_unit_valid(in->unit)) return RX_DUAL_ERR_UNIT;
    if ((uint32_t)in->estimate_kind < 1u || (uint32_t)in->estimate_kind >= (uint32_t)RX_DUAL_EST_MAX_) return RX_DUAL_ERR_KIND;
    if (!isfinite(in->estimate) || !isfinite(in->uncertainty)) return RX_DUAL_ERR_NONFINITE;
    if (in->uncertainty < 0.0) return RX_DUAL_ERR_RANGE;
    if (rx_dual_digest_is_zero(&in->estimate_ref) || rx_dual_digest_is_zero(&in->evidence_root)) return RX_DUAL_ERR_DIGEST;
    if (in->estimate_kind == RX_DUAL_EST_MEASURED && rx_dual_digest_is_zero(&in->observation_ref)) return RX_DUAL_ERR_DIGEST;
    if (in->evidence_verified > 1u || in->regime_change > 1u) return RX_DUAL_ERR_RANGE;
    return RX_DUAL_OK;
}

RxDualStatus rx_dual_classify_input(const RxDualInput *in, const RxDualConstraintState *prev,
                                    const RxDualController *ctl, uint64_t now_generation,
                                    RxDualLambdaState *out)
{
    RxDualStatus st;
    if (!in || !ctl || !out) return RX_DUAL_ERR_NULL;
    if ((st = rx_dual_check_controller(ctl)) != RX_DUAL_OK) return st;
    if (prev) {
        if ((st = rx_dual_check_constraint(prev)) != RX_DUAL_OK) return st;
        if (in->unit != prev->unit) return RX_DUAL_ERR_UNIT; /* structural: never converted */
    }
    /* value problems: the input is refused for this tick */
    if (rx_dual_check_input(in) != RX_DUAL_OK) { *out = RX_DUAL_LAMBDA_REFUSED; return RX_DUAL_OK; }
    if (in->generation > now_generation) { *out = RX_DUAL_LAMBDA_REFUSED; return RX_DUAL_OK; } /* future generation */
    /* staleness */
    if (now_generation - in->generation > ctl->max_age) { *out = RX_DUAL_LAMBDA_STALE; return RX_DUAL_OK; }
    if (prev && in->generation < prev->generation) { *out = RX_DUAL_LAMBDA_STALE; return RX_DUAL_OK; }
    if (!in->evidence_verified) { *out = RX_DUAL_LAMBDA_STALE; return RX_DUAL_OK; }
    if (in->regime_change) { *out = RX_DUAL_LAMBDA_FROZEN; return RX_DUAL_OK; }
    if (rx_dual_digest_is_zero(&in->calibration_ref)) { *out = RX_DUAL_LAMBDA_UNCALIBRATED; return RX_DUAL_OK; }
    *out = RX_DUAL_LAMBDA_FRESH;
    return RX_DUAL_OK;
}

RxDualStatus rx_dual_evidence_extend(const RxDualDigest *root, const RxDualDigest *estimate_ref, RxDualDigest *out)
{
    est_digest r, e, o;
    if (!root || !estimate_ref || !out) return RX_DUAL_ERR_NULL;
    if (rx_dual_digest_is_zero(estimate_ref)) return RX_DUAL_ERR_DIGEST;
    memcpy(r.b, root->b, 32); memcpy(e.b, estimate_ref->b, 32);
    if (est_evidence_root_extend(&r, &e, &o) != EST_OK) return RX_DUAL_ERR_EVIDENCE;
    memcpy(out->b, o.b, 32);
    return RX_DUAL_OK;
}
