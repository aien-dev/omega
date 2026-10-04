/* ANS state machine, control law and promotion gate (see ans.h). The filter is
 * EST-1 (est_kf.c) and is not reimplemented here. */
#include "ans.h"
#include "est_kf.h"

#include <math.h>
#include <string.h>

ans_atom ans_component_atom(uint32_t component)
{
    static const ans_atom map[ANS_DIM] = {
        ANS_ATOM_GOAL_IDENTITY, ANS_ATOM_CONSENT, ANS_ATOM_PROVENANCE, ANS_ATOM_SCOPE
    };
    return map[component % ANS_DIM];
}

/* ---- reference ---- */
static int pos_fin(double d) { return isfinite(d) && d > 0.0; }

ans_status ans_reference_validate(const ans_reference *ref)
{
    if (!ref) return ANS_ERR_NULL;
    if (ref->version == 0u) return ANS_ERR_REFERENCE;
    for (uint32_t i = 0; i < ANS_ATOMS; i++) if (!pos_fin(ref->limit[i])) return ANS_ERR_REFERENCE;
    for (uint32_t i = 0; i < ANS_ACT_KINDS; i++) if (!pos_fin(ref->q[i])) return ANS_ERR_REFERENCE;
    for (uint32_t i = 0; i < ANS_SENSOR_KINDS; i++) if (!pos_fin(ref->r[i])) return ANS_ERR_REFERENCE;
    if (!(ref->r[ANS_SENSOR_FIX] < ref->r[ANS_SENSOR_REFERENCE] &&
          ref->r[ANS_SENSOR_REFERENCE] < ref->r[ANS_SENSOR_IMU])) return ANS_ERR_REFERENCE;
    if (!pos_fin(ref->tier_sigma[0]) || !(ref->tier_sigma[0] < ref->tier_sigma[1]) ||
        !(ref->tier_sigma[1] < ref->tier_sigma[2]) || !isfinite(ref->tier_sigma[2])) return ANS_ERR_REFERENCE;
    if (!pos_fin(ref->nis_limit) || !pos_fin(ref->promote_limit)) return ANS_ERR_REFERENCE;
    return ANS_OK;
}

ans_status ans_reference_freeze(ans_reference *ref, ans_digest *out_digest)
{
    if (!ref || !out_digest) return ANS_ERR_NULL;
    ans_status st = ans_reference_validate(ref);
    if (st != ANS_OK) return st;
    ans_digest d;
    st = ans_digest_reference(ref, &d);
    if (st != ANS_OK) return st;
    ref->frozen = 1;
    *out_digest = d;
    return ANS_OK;
}

ans_status ans_reference_check(const ans_reference *ref, const ans_digest *expected)
{
    if (!ref || !expected) return ANS_ERR_NULL;
    if (ref->frozen != 1u) return ANS_ERR_REFERENCE;
    if (ans_reference_validate(ref) != ANS_OK) return ANS_ERR_REFERENCE;
    ans_digest d;
    if (ans_digest_reference(ref, &d) != ANS_OK) return ANS_ERR_REFERENCE;
    if (memcmp(d.b, expected->b, EST_DIGEST_SIZE) != 0) return ANS_ERR_REFERENCE;
    return ANS_OK;
}

/* ---- filter plumbing ---- */
static void build_model(const ans_reference *ref, double q, est_model *m)
{
    memset(m, 0, sizeof *m);
    m->n = m->m = ANS_DIM;
    m->estimator = EST_ESTIMATOR_LINEAR_KALMAN;
    m->meaning = EST_UNCERTAINTY_GAUSSIAN_COVARIANCE;
    m->step_ns = 1;
    for (uint32_t i = 0; i < ANS_DIM; i++) {
        m->state_unit[i] = m->obs_unit[i] = EST_UNIT_DIMENSIONLESS;
        m->F[i * ANS_DIM + i] = 1.0;
        m->H[i * ANS_DIM + i] = 1.0;
        m->Q[i * ANS_DIM + i] = q;
        m->R[i * ANS_DIM + i] = ref->r[ANS_SENSOR_FIX];
    }
}

/* The process noise differs per action kind, so the model digest differs per
 * step. The belief is re-labelled with the step's model digest (same x, P,
 * generation, evidence chain) before est_kf_predict, which binds by digest. */
static est_status rebind(const est_model *m, const est_belief *b, est_belief *out)
{
    *out = *b;
    return est_digest_model(m, &out->model);
}

static ans_status wrap(est_status st, est_status *est_st)
{
    if (est_st) *est_st = st;
    return st == EST_OK ? ANS_OK : ANS_ERR_EST;
}

static ans_status guard(const ans_reference *ref, const ans_digest *digest, const ans_state *state)
{
    if (!ref || !digest || !state) return ANS_ERR_NULL;
    ans_status st = ans_reference_check(ref, digest);
    if (st != ANS_OK) return st;
    if (est_check_belief(&state->drift) != EST_OK || state->drift.n != ANS_DIM) return ANS_ERR_EST;
    if (!isfinite(state->risk) || !isfinite(state->last_nis)) return ANS_ERR_NONFINITE;
    if (memcmp(state->constraints.b, digest->b, EST_DIGEST_SIZE) != 0) return ANS_ERR_REFERENCE;
    return ANS_OK;
}

ans_status ans_state_prior(const ans_reference *ref, const ans_digest *digest,
                           const ans_digest *goal, const ans_digest *intent,
                           ans_state *out, est_status *est_st)
{
    if (!ref || !digest || !goal || !intent || !out) return ANS_ERR_NULL;
    ans_status st = ans_reference_check(ref, digest);
    if (st != ANS_OK) return st;
    if (est_digest_is_zero(goal) || est_digest_is_zero(intent)) return ANS_ERR_RANGE;
    est_model m;
    build_model(ref, ref->q[ANS_ACT_OBSERVE], &m);
    double x0[ANS_DIM] = {0}, P0[ANS_DIM * ANS_DIM] = {0};
    for (uint32_t i = 0; i < ANS_DIM; i++) P0[i * ANS_DIM + i] = ref->r[ANS_SENSOR_FIX];
    ans_state s;
    memset(&s, 0, sizeof s);
    est_status es = est_kf_prior(&m, x0, P0, 0, &s.drift);
    if (es != EST_OK) return wrap(es, est_st);
    s.goal = *goal;
    s.intent = *intent;
    s.constraints = *digest;
    s.evidence_root = s.drift.evidence_root;
    s.last_reference_digest_seen = *digest;
    ans_digest root;
    memset(&root, 0, sizeof root);
    ans_provenance_extend(&root, "prior", digest, &s.provenance);
    *out = s;
    return wrap(EST_OK, est_st);
}

ans_status ans_step(const ans_reference *ref, const ans_digest *digest, const ans_state *state,
                    const ans_action *action, ans_state *out, est_status *est_st)
{
    if (!action || !out) return ANS_ERR_NULL;
    ans_status st = guard(ref, digest, state);
    if (st != ANS_OK) return st;
    ans_digest adig;
    st = ans_digest_action(action, &adig); /* validates kind, risk, touches, id */
    if (st != ANS_OK) return st;
    ans_state s = *state;
    if (s.generation == UINT64_MAX || s.since_fix == UINT64_MAX) return ANS_ERR_RANGE;
    if (action->touches[ANS_ATOM_SELF_MODIFICATION_LIMITS] &&
        action->reversibility_risk > ref->limit[ANS_ATOM_SELF_MODIFICATION_LIMITS]) {
        /* refusal is recorded, never repaired: drift unchanged */
        ans_provenance_extend(&s.provenance, "refused", &adig, &s.provenance);
        s.generation++;
        *out = s;
        return ANS_ERR_ATOM;
    }
    est_model m;
    build_model(ref, ref->q[action->kind], &m);
    est_belief rb;
    est_prediction pred;
    est_status es = rebind(&m, &state->drift, &rb);
    if (es == EST_OK) es = est_kf_predict(&m, &rb, NULL, 1, &pred);
    if (es == EST_OK) es = est_kf_coast(&m, &pred, &rb, &s.drift);
    if (es != EST_OK) return wrap(es, est_st);
    s.evidence_root = s.drift.evidence_root;
    s.risk = action->reversibility_risk;
    s.since_fix++;
    s.generation++;
    if (action->kind == ANS_ACT_SELF_MODIFY || action->touches[ANS_ATOM_SELF_MODIFICATION_LIMITS]) {
        s.self_mod_steps++;
        s.self_mod_unfixed++;
    }
    ans_provenance_extend(&s.provenance, "action", &adig, &s.provenance);
    *out = s;
    return wrap(EST_OK, est_st);
}

ans_status ans_measure(const ans_reference *ref, const ans_digest *digest, const ans_state *state,
                       const ans_measurement *meas, ans_state *out, est_innovation *innovation_out,
                       est_status *est_st)
{
    if (!meas || !out) return ANS_ERR_NULL;
    ans_status st = guard(ref, digest, state);
    if (st != ANS_OK) return st;
    for (uint32_t i = 0; i < ANS_DIM; i++) if (!isfinite(meas->z[i])) return ANS_ERR_NONFINITE;
    if ((unsigned)meas->cls >= ANS_SENSOR_KINDS) return ANS_ERR_SENSOR;
    if (est_digest_is_zero(&meas->source) || est_digest_is_zero(&meas->evidence)) return ANS_ERR_EVIDENCE;
    if (meas->cls == ANS_SENSOR_IMU) {
        if (!est_digest_is_zero(&meas->reference)) return ANS_ERR_SENSOR;
    } else if (memcmp(meas->reference.b, digest->b, EST_DIGEST_SIZE) != 0) {
        return ANS_ERR_SENSOR; /* zero, or any digest other than the frozen reference */
    }
    ans_digest mdig;
    st = ans_digest_measurement(meas, &mdig);
    if (st != ANS_OK) return st;
    ans_state s = *state;
    if (s.generation == UINT64_MAX || s.since_fix == UINT64_MAX) return ANS_ERR_RANGE;

    est_model m;
    build_model(ref, ANS_Q_HOLD, &m);
    est_belief rb;
    est_prediction pred;
    est_observation obs;
    est_innovation iv;
    est_status es = rebind(&m, &state->drift, &rb);
    if (es == EST_OK) es = est_kf_predict(&m, &rb, NULL, 1, &pred);
    if (es != EST_OK) return wrap(es, est_st);
    memset(&obs, 0, sizeof obs);
    obs.m = ANS_DIM;
    for (uint32_t i = 0; i < ANS_DIM; i++) {
        obs.unit[i] = EST_UNIT_DIMENSIONLESS;
        obs.z[i] = meas->z[i];
        obs.R[i * ANS_DIM + i] = ref->r[meas->cls];
    }
    obs.t_ns = pred.t_ns;
    obs.seq = meas->seq;
    obs.source = meas->source;
    obs.evidence = meas->evidence;
    es = est_kf_update(&m, &rb, &pred, &obs, &s.drift, &iv);
    if (es != EST_OK) return wrap(es, est_st);
    s.evidence_root = s.drift.evidence_root;
    s.generation++;
    s.since_fix++;
    if (meas->cls != ANS_SENSOR_IMU) {
        s.last_nis = iv.nis;
        s.last_reference_digest_seen = meas->reference;
        if (iv.nis > ref->nis_limit) s.disagreement = 1; /* fused anyway: the disagreement is the signal */
    }
    if (meas->cls == ANS_SENSOR_FIX) {
        s.since_fix = 0;
        s.self_mod_unfixed = 0;
        if (iv.nis <= ref->nis_limit) s.disagreement = 0;
    }
    ans_provenance_extend(&s.provenance, "measurement", &mdig, &s.provenance);
    if (innovation_out) *innovation_out = iv;
    *out = s;
    return wrap(EST_OK, est_st);
}

/* ---- drift and control law ---- */
ans_status ans_drift(const ans_state *state, double *mean_norm, double *sigma)
{
    if (!state || !mean_norm || !sigma) return ANS_ERR_NULL;
    if (state->drift.n != ANS_DIM) return ANS_ERR_RANGE;
    double ss = 0.0, tr = 0.0;
    for (uint32_t i = 0; i < ANS_DIM; i++) {
        ss += state->drift.x[i] * state->drift.x[i];
        tr += state->drift.P[i * ANS_DIM + i];
    }
    if (!isfinite(ss) || !isfinite(tr) || tr < 0.0) return ANS_ERR_NONFINITE;
    *mean_norm = sqrt(ss);
    *sigma = sqrt(tr);
    return ANS_OK;
}

double ans_drift_total(double mean_norm, double sigma)
{
    return sqrt(mean_norm * mean_norm + sigma * sigma);
}

ans_status ans_autonomy(const ans_reference *ref, const ans_digest *digest, const ans_state *state,
                        ans_verdict *out)
{
    if (!out) return ANS_ERR_NULL;
    ans_status st = guard(ref, digest, state);
    if (st != ANS_OK) return st;
    ans_verdict v;
    memset(&v, 0, sizeof v);
    st = ans_drift(state, &v.mean_norm, &v.sigma);
    if (st != ANS_OK) return st;
    v.D = ans_drift_total(v.mean_norm, v.sigma);
    ans_tier t = ANS_TIER_FULL;
    if (v.sigma >= ref->tier_sigma[2]) t = ANS_TIER_HALT_REQUEST_FIX;
    else if (v.sigma >= ref->tier_sigma[1]) t = ANS_TIER_SIMULATE_ONLY;
    else if (v.sigma >= ref->tier_sigma[0]) t = ANS_TIER_REVERSIBLE_ONLY;
    for (uint32_t i = 0; i < ANS_DIM; i++)
        if (fabs(state->drift.x[i]) > ref->limit[ans_component_atom(i)] && t < ANS_TIER_REVERSIBLE_ONLY)
            t = ANS_TIER_REVERSIBLE_ONLY;
    if (state->disagreement) t = ANS_TIER_HALT_REQUEST_FIX;
    v.tier = t;
    v.since_fix = state->since_fix;
    v.disagreement = state->disagreement;
    st = ans_digest_state(state, &v.state);
    if (st != ANS_OK) return st;
    v.reference = *digest;
    st = ans_digest_verdict(&v, &v.digest);
    if (st != ANS_OK) return st;
    *out = v;
    return ANS_OK;
}

/* ---- promotion gate ---- */
ans_status ans_promotion_check(const ans_reference *ref, const ans_digest *digest,
                               const ans_promotion_request *req, ans_promotion_record *out)
{
    if (!ref || !digest || !req || !out) return ANS_ERR_NULL;
    ans_status st = ans_reference_check(ref, digest);
    if (st != ANS_OK) return st;
    const ans_state *cs = &req->candidate_state;
    if (est_check_belief(&cs->drift) != EST_OK || cs->drift.n != ANS_DIM) return ANS_ERR_EST;
    if (!isfinite(cs->risk) || !isfinite(cs->last_nis)) return ANS_ERR_NONFINITE;
    double mean, sigma;
    st = ans_drift(cs, &mean, &sigma);
    if (st != ANS_OK) return st;
    ans_promotion_record r;
    memset(&r, 0, sizeof r);
    r.D = ans_drift_total(mean, sigma);
    r.candidate = req->candidate;
    r.reference = *digest;
    r.tests = req->tests_now;
    st = ans_digest_state(cs, &r.state);
    if (st != ANS_OK) return st;
    if (memcmp(req->reference_at_birth.b, digest->b, EST_DIGEST_SIZE) != 0)
        r.result = ANS_PROMOTE_REFUSED_REFERENCE;
    else if (memcmp(req->tests_now.b, req->tests_at_birth.b, EST_DIGEST_SIZE) != 0)
        r.result = ANS_PROMOTE_REFUSED_TESTS;
    else if (cs->disagreement)
        r.result = ANS_PROMOTE_REFUSED_DISAGREEMENT;
    else if (r.D > ref->promote_limit)
        r.result = ANS_PROMOTE_REFUSED_DRIFT;
    else if (cs->self_mod_unfixed > 0)
        r.result = ANS_PROMOTE_REFUSED_SELF_MOD;
    else
        r.result = ANS_PROMOTE_OK;
    st = ans_digest_promotion_record(&r, &r.digest);
    if (st != ANS_OK) return st;
    *out = r;
    return ANS_OK;
}
