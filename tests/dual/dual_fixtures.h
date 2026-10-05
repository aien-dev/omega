/* Shared fixtures for DUAL-0b / DUAL-1a tests: minimal valid ESTIMATION
 * records (n = m = 1) and a controller. */
#ifndef DUAL_FIXTURES_H
#define DUAL_FIXTURES_H
#include "rx_dual_update.h"
#include <string.h>

static inline RxDualDigest fx_dig(uint8_t v) { RxDualDigest d; memset(d.b, v, 32); return d; }

static inline est_model fx_model(est_unit unit)
{
    est_model m; memset(&m, 0, sizeof m);
    m.n = 1; m.m = 1; m.estimator = EST_ESTIMATOR_LINEAR_KALMAN; m.meaning = EST_UNCERTAINTY_GAUSSIAN_COVARIANCE;
    m.state_unit[0] = unit; m.obs_unit[0] = unit;
    m.F[0] = 1.0; m.Q[0] = 0.01; m.H[0] = 1.0; m.R[0] = 0.04; m.step_ns = 1000;
    return m;
}
static inline est_observation fx_obs(est_unit unit, double z, double var, uint64_t seq)
{
    est_observation o; memset(&o, 0, sizeof o);
    o.m = 1; o.unit[0] = unit; o.z[0] = z; o.R[0] = var; o.t_ns = (int64_t)seq * 1000; o.seq = seq;
    memset(o.source.b, 0x5A, 32); memset(o.evidence.b, 0xE1 + (uint8_t)seq, 32);
    return o;
}
/* Belief at `generation` with x, variance; evidence_root = extend(parent_root, digest(obs)). */
static inline est_belief fx_belief(const est_model *m, const est_observation *obs, const est_digest *parent_root,
                            est_unit unit, double x, double var, uint64_t generation)
{
    est_belief b; est_digest od; memset(&b, 0, sizeof b);
    b.n = 1; b.unit[0] = unit; b.x[0] = x; b.P[0] = var; b.generation = generation; b.t_ns = (int64_t)generation * 1000;
    est_digest_model(m, &b.model);
    memset(b.parent.b, 0x77, 32);
    est_digest_observation(obs, &od);
    est_evidence_root_extend(parent_root, &od, &b.evidence_root);
    return b;
}
static inline RxDualController fx_ctl(uint32_t id, double eta, double rho, double k_sigma, double lmax, uint64_t max_age)
{
    RxDualController c; memset(&c, 0, sizeof c);
    c.eta = eta; c.rho = rho; c.k_sigma = k_sigma; c.max_age = max_age; c.cadence = 1;
    c.n = 1; c.resource_id[0] = id; c.lambda_max[0] = lmax;
    return c;
}
static inline RxDualResource fx_res(uint32_t id, RxDualUnit unit, double scale)
{
    RxDualResource r; memset(&r, 0, sizeof r);
    r.resource_id = id; r.unit = unit; r.scale = scale; r.contract = fx_dig(0x11);
    return r;
}
/* A verified, calibrated input: estimate x, sd, at generation g, unit bytes. */
static inline RxDualInput fx_input(double x, double sd, uint64_t g, int calibrated)
{
    RxDualInput in; memset(&in, 0, sizeof in);
    in.estimate_ref = fx_dig(0x30 + (uint8_t)(g & 0x3F)); in.estimate_kind = RX_DUAL_EST_ESTIMATED;
    in.unit = RX_DUAL_UNIT_BYTES; in.estimate = x; in.uncertainty = sd; in.generation = g;
    in.evidence_root = fx_dig(0x66); in.evidence_verified = 1;
    if (calibrated) in.calibration_ref = fx_dig(0x44);
    return in;
}
#endif
