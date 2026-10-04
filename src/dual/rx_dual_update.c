/* DUAL-1a: reference price update. */
#include "rx_dual_update.h"

#include <math.h>
#include <string.h>

RxDualStatus rx_dual_step_lambda(double lambda, double estimate, double uncertainty, double budget,
                                 double scale, const RxDualController *ctl, double lambda_max, double *out)
{
    double pressure, deadband, g, next;
    RxDualStatus st;
    if (!ctl || !out) return RX_DUAL_ERR_NULL;
    if ((st = rx_dual_check_controller(ctl)) != RX_DUAL_OK) return st;
    if (!isfinite(lambda) || !isfinite(estimate) || !isfinite(uncertainty) || !isfinite(budget) ||
        !isfinite(scale) || !isfinite(lambda_max))
        return RX_DUAL_ERR_NONFINITE;
    if (!(scale > 0.0)) return RX_DUAL_ERR_SCALE;
    if (lambda < 0.0 || uncertainty < 0.0 || !(lambda_max > 0.0) || lambda > lambda_max) return RX_DUAL_ERR_RANGE;
    pressure = (estimate - budget) / scale;
    deadband = ctl->k_sigma * uncertainty / scale;
    if (fabs(pressure) <= deadband) g = 0.0;
    else g = pressure - (pressure > 0.0 ? deadband : -deadband);
    next = (1.0 - ctl->rho) * lambda + ctl->eta * g;
    if (!isfinite(next)) return RX_DUAL_ERR_NONFINITE;
    if (next < 0.0) next = 0.0;
    if (next > lambda_max) next = lambda_max;
    *out = next;
    return RX_DUAL_OK;
}

/* Shared structural checks for init and update. */
static RxDualStatus common(const RxDualResource *res, const RxDualInput *in, const RxDualController *ctl,
                           double *lambda_max, RxDualDigest *ctl_id)
{
    RxDualStatus st;
    if (!res || !in || !ctl) return RX_DUAL_ERR_NULL;
    if ((st = rx_dual_check_resource(res)) != RX_DUAL_OK) return st;
    if ((st = rx_dual_check_controller(ctl)) != RX_DUAL_OK) return st;
    if ((st = rx_dual_controller_lambda_max(ctl, res->resource_id, lambda_max)) != RX_DUAL_OK) return st;
    if ((st = rx_dual_digest_controller(ctl, ctl_id)) != RX_DUAL_OK) return st;
    return RX_DUAL_OK;
}

static void take_input(RxDualConstraintState *s, const RxDualInput *in)
{
    s->observation_ref = in->observation_ref;
    s->estimate_ref = in->estimate_ref;
    s->estimate_kind = in->estimate_kind;
    s->estimate = in->estimate;
    s->uncertainty = in->uncertainty;
    s->calibration_ref = in->calibration_ref;
    s->generation = in->generation;
}

RxDualStatus rx_dual_init_state(const RxDualResource *res, RxDualClass cls, double budget,
                                const RxDualDigest *budget_contract, const RxDualInput *in,
                                const RxDualController *ctl, uint64_t now_generation,
                                RxDualConstraintState *out)
{
    double lmax = 0.0, next = 0.0;
    RxDualDigest ctl_id, zero;
    RxDualLambdaState cls_state;
    RxDualStatus st;
    if (!budget_contract || !out) return RX_DUAL_ERR_NULL;
    if ((st = common(res, in, ctl, &lmax, &ctl_id)) != RX_DUAL_OK) return st;
    if (cls != RX_DUAL_CLASS_CAPACITY && cls != RX_DUAL_CLASS_SOFT) return RX_DUAL_ERR_CLASS;
    if (!isfinite(budget)) return RX_DUAL_ERR_NONFINITE;
    if (rx_dual_digest_is_zero(budget_contract)) return RX_DUAL_ERR_DIGEST;
    /* The very first input must be structurally valid and in the resource's unit. */
    if ((st = rx_dual_check_input(in)) != RX_DUAL_OK) return st;
    if (in->unit != res->unit) return RX_DUAL_ERR_UNIT;
    if ((st = rx_dual_classify_input(in, NULL, ctl, now_generation, &cls_state)) != RX_DUAL_OK) return st;
    memset(out, 0, sizeof *out);
    out->resource_id = res->resource_id;
    out->unit = res->unit;
    out->cls = cls;
    out->budget = budget;
    out->budget_contract = *budget_contract;
    take_input(out, in);
    out->controller_id = ctl_id;
    out->tick = 0;
    memset(&zero, 0, sizeof zero);
    if ((st = rx_dual_evidence_extend(&zero, &in->estimate_ref, &out->evidence_root)) != RX_DUAL_OK) return st;
    out->lambda = 0.0;
    out->lambda_state = cls_state;
    if (cls_state == RX_DUAL_LAMBDA_FRESH || cls_state == RX_DUAL_LAMBDA_UNCALIBRATED) {
        if ((st = rx_dual_step_lambda(0.0, in->estimate, in->uncertainty, budget, res->scale, ctl, lmax, &next)) != RX_DUAL_OK)
            return st;
        out->lambda = next;
    }
    return rx_dual_check_constraint(out);
}

RxDualStatus rx_dual_update(const RxDualConstraintState *prev, const RxDualResource *res,
                            const RxDualInput *in, const RxDualController *ctl,
                            uint64_t now_generation, RxDualConstraintState *out)
{
    double lmax = 0.0, next = 0.0;
    RxDualDigest ctl_id, parent;
    RxDualLambdaState state;
    RxDualStatus st;
    if (!prev || !out) return RX_DUAL_ERR_NULL;
    if ((st = common(res, in, ctl, &lmax, &ctl_id)) != RX_DUAL_OK) return st;
    if ((st = rx_dual_check_constraint_against(prev, res)) != RX_DUAL_OK) return st;   /* malformed parent, id, unit */
    if (!rx_dual_digest_eq(&prev->controller_id, &ctl_id)) return RX_DUAL_ERR_CONTROLLER; /* parameter mutation */
    if (prev->lambda > lmax) return RX_DUAL_ERR_RANGE;
    if ((st = rx_dual_digest_constraint(prev, &parent)) != RX_DUAL_OK) return st;
    if ((st = rx_dual_classify_input(in, prev, ctl, now_generation, &state)) != RX_DUAL_OK) return st;
    *out = *prev;
    out->parent = parent;
    out->tick = prev->tick + 1;
    out->lambda_state = state;
    if (state == RX_DUAL_LAMBDA_FRESH || state == RX_DUAL_LAMBDA_UNCALIBRATED) {
        if (in->unit != res->unit) return RX_DUAL_ERR_UNIT;
        if ((st = rx_dual_step_lambda(prev->lambda, in->estimate, in->uncertainty, prev->budget, res->scale, ctl, lmax, &next)) != RX_DUAL_OK)
            return st;
        take_input(out, in);
        out->lambda = next;
        if ((st = rx_dual_evidence_extend(&prev->evidence_root, &in->estimate_ref, &out->evidence_root)) != RX_DUAL_OK) return st;
    }
    /* STALE / FROZEN / REFUSED: lambda, estimate fields, generation and evidence root are held. */
    return rx_dual_check_constraint(out);
}
