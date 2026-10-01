/* ESTIMATION-2, protocol v3: quantization-aware discrete predictive layer.
 *
 * Why it exists. Protocol v1 failed on heavy tails and volatility clustering;
 * protocol v2 failed because about 80 % of 1 Hz thermal one-step changes are
 * exactly zero on the 100 mC sensor grid, so the central 50 % interval of any
 * continuous predictive covers every zero change and 50 % coverage cannot pass
 * (docs/estimation/EST23_PROTOCOL_V2.md). Here every candidate estimator
 * predicts the NEXT OBSERVATION as a probability mass function over the grid,
 * and calibration is scored with the fractional (randomized-PIT) rule, which is
 * exactly calibrated for a correct discrete predictive.
 *
 * Typed concepts (distinct C types; none collapses into another):
 *   est_assumption  ModelAssumption: family, grid quantum (mC), valid range,
 *                   parameter vector, unit. Identity = SHA-256(
 *                   "omega.est.assumption.v1" || 0x00 || encoding), encoding
 *                   in est_pred.c above est_assumption_digest.
 *   est_pobs        Observation: raw value + validity class + evidence digest.
 *                   Bad values are classified and treated as missing, counted,
 *                   kept raw (never repaired, never silently dropped).
 *   est_bstate      BeliefState: candidate internal state. Never an output.
 *   est_dpred       Estimate/Uncertainty: pmf of the next observation over
 *                   offsets k*quantum from the last valid observation (the
 *                   anchor), k in [-EST_PRED_K, +EST_PRED_K]. An estimate is
 *                   never an observation: update accepts only est_pobs.
 *   est_pinnov      Innovation: where the observation fell in the pmf.
 *   est_calib       CalibrationEvidence: accumulator; est_calib_verdict reads
 *                   it against est_bands.
 * Confidence in any of these is data. Nothing here is an authority input.
 *
 * Step semantics. One logical step = one sampling tick. est_pred_update
 * consumes exactly one observation record per tick, whatever its class.
 * gap = ticks elapsed since the last valid observation (0 right after one).
 * est_pred_predict(h) forecasts the observation h ticks ahead of NOW, so its
 * effective horizon (ticks after the anchor) is gap + h; that is what
 * est_dpred.horizon holds. Effective horizon 1..EST_PRED_MAX_H, else
 * EST_ERR_TIME. predict changes only the generation (+1). update: generation
 * +1, evidence_root extended with the observation's evidence digest (every
 * class, so a bad reading is on the record), class counter +1; a valid value
 * becomes the new anchor (also after a gap beyond EST_PRED_MAX_H, so the state
 * never dead-ends), anything else just grows the gap.
 * Before the first valid observation there is no anchor: predict returns
 * EST_ERR_STALE.
 *
 * Families. A continuous one-step change distribution G is discretized:
 *   p(k) = G((k+0.5)q) - G((k-0.5)q), edge bins take the whole outer tails.
 *   F1 GAUSS_KF   random-walk Kalman on the level (q_proc, r, p0);
 *                 predictive change ~ N(xhat - anchor, P + H*q_proc + r),
 *                 H the effective horizon (closed form, no convolution).
 *   F2 HUBER_KF   predictive as F1; the update clips the innovation at
 *                 c*sqrt(S) (Huber); params (q_proc, r, p0, c).
 *   F3 STUDENT_T  persistence centre, change ~ t_nu(0, s); params (nu, s).
 *   F4 SCALE_MIX  persistence centre, change ~ est_mix (k <= 3); params
 *                 (k, w0, w1, w2, v0, v1, v2), unused w = v = 0.
 *   F5 ADAPTIVE_T persistence centre, change ~ t_nu(0, c*s_t),
 *                 s_t^2 = lambda*s_{t-1}^2 + (1-lambda)*max(d^2, floor), d the
 *                 last one-step grid change, updated only on two consecutive
 *                 valid observations; params (lambda, nu, c, floor, s0).
 * F3/F4/F5 H-step pmf = H-fold convolution of the DISCRETE one-step pmf (the
 * sum of H iid grid changes is exactly the H-step grid change), by repeated
 * squaring; mass beyond +-EST_PRED_K after each product is folded into the
 * edge bins (edge bin = "at or beyond the edge"). F4 uses the convolution too,
 * not the est_mix exact h-sum, so all persistence families share one rule.
 * Documented approximation: mass in an edge bin ("at or beyond +-EST_PRED_K")
 * is convolved as if it sat exactly at +-EST_PRED_K, so some tail mass moves
 * inward on each squaring. Measured edge mass for F3 nu=1.5 s=10 mC: 1.5e-6
 * at h=1, <= 4.4e-5 at h=64 (no effect on coverage; not exact).
 * Finally every returned pmf is mixed with EST_PRED_FLOOR of uniform mass and
 * renormalized: no outcome on the grid has zero probability, and the pmf sums
 * to 1 within 1e-12.
 *
 * Observation classes (checked in this order): MISSING (caller says absent),
 * NONFINITE, OUT_OF_RANGE (value < lo or > hi), OFF_GRID (|value/q - round|
 * > EST_PRED_GRID_TOL), else OK. An OK value whose offset from the anchor is
 * beyond +-EST_PRED_K is scored in the edge bin.
 *
 * Calibration (est_calib_verdict). Statistics are checked in this fixed order
 * and the first failure is named: n, cov50, cov80, cov95, pit[0..9], bias
 * (mean mid-PIT z), lag1 (lag-1 autocorrelation of z, from streaming sums:
 * (S_xy/(n-1) - m^2) / (S_xx/n - m^2)).
 * This library order (n first) is NOT the EST-3c v3 protocol's first-failing-
 * statistic order (section 6 lists n last). The v3 tools apply the protocol
 * order themselves (c3_judge in tools/estimation/est3c_common.c) and do not
 * call est_calib_verdict.
 *
 * Bounded (pmf arrays of fixed size), no allocation, no globals; libm only.
 * Not linked into the runtime; built by mk/estimation_v3.mk only. */
#ifndef OMEGA_EST_PRED_H
#define OMEGA_EST_PRED_H

#include <stddef.h>
#include <stdint.h>

#include "est_types.h"
#include "est_mix.h"

#define EST_PRED_K 400
#define EST_PRED_N (2 * EST_PRED_K + 1)
#define EST_PRED_MAX_H 64u
#define EST_PRED_NPARAM 8u
#define EST_PRED_FLOOR 1e-12      /* uniform mass mixed into every pmf */
#define EST_PRED_GRID_TOL 1e-6    /* in quanta */
#define EST_DOMAIN_ASSUMPTION "omega.est.assumption.v1"

typedef enum {
    EST_FAM_GAUSS_KF = 1,
    EST_FAM_HUBER_KF = 2,
    EST_FAM_STUDENT_T = 3,
    EST_FAM_SCALE_MIX = 4,
    EST_FAM_ADAPTIVE_T = 5
} est_family;

/* ModelAssumption. nparam must equal the family's count (3, 4, 2, 7, 5);
 * param[nparam..] must be 0. Variances in mC^2, scales in mC. */
typedef struct {
    est_family family;
    est_unit unit;                 /* unit of the observed value */
    double quantum;                /* grid quantum, > 0, finite */
    double lo, hi;                 /* valid observation range, lo < hi */
    uint32_t nparam;
    double param[EST_PRED_NPARAM];
} est_assumption;

typedef enum {
    EST_OBS_OK = 0,
    EST_OBS_MISSING = 1,
    EST_OBS_NONFINITE = 2,
    EST_OBS_OFF_GRID = 3,
    EST_OBS_OUT_OF_RANGE = 4,
    EST_OBS_CLASSES_ = 5
} est_obs_class;

/* Observation. value is kept exactly as received (NaN stays NaN). */
typedef struct {
    double value;
    est_obs_class cls;
    est_digest evidence;           /* binds the raw bytes; must be non-zero */
} est_pobs;

/* BeliefState. Fields are the candidate's internal state; callers read but do
 * not write them. cache holds a one-step pmf (no floor) reused while the
 * scale that produced it (cache_key) is unchanged. */
typedef struct {
    est_digest assumption;
    uint32_t has_anchor;
    double anchor;                 /* last valid observation */
    double xhat, P;                /* F1/F2 level mean and variance */
    double s2;                     /* F5 adaptive scale squared */
    uint32_t gap;                  /* ticks since the last valid observation */
    uint64_t generation;           /* +1 per predict and per update */
    est_digest evidence_root;      /* chained over every observation's evidence */
    uint64_t count[EST_OBS_CLASSES_];
    uint32_t cache_valid;
    double cache_key;
    int32_t cache_lo, cache_hi;    /* nonzero support of cache, as offsets */
    double cache[EST_PRED_N];
} est_bstate;

/* Estimate: pmf[i] = P(next obs = anchor + (i - EST_PRED_K) * quantum). */
typedef struct {
    est_digest assumption;
    est_family family;
    uint32_t horizon;              /* effective: ticks after the anchor */
    uint64_t generation;           /* state generation after this predict */
    double anchor;
    double quantum;
    double pmf[EST_PRED_N];
} est_dpred;

/* Innovation. valid = 0 for a non-OK observation (nothing else is filled). */
typedef struct {
    uint32_t valid;
    est_obs_class cls;
    int32_t k;                     /* outcome offset, clipped to +-EST_PRED_K */
    double F_lo, F_hi;             /* P(Y < y), P(Y <= y) */
    double logp;                   /* log p(y), p floored at 1e-300 */
    double z;                      /* Phi^-1((F_lo+F_hi)/2), clipped to [-8,8] */
} est_pinnov;

typedef struct {
    uint64_t n;
    double cov[3];                 /* fractional coverage sums: 0.50 0.80 0.95 */
    double pit[10];
    double logp_sum;
    double width_sum;              /* central 80 % width, mC */
    double z_sum, z_sq, z_lag;     /* sum z, sum z^2, sum z_t*z_{t-1} */
    double z_prev;
} est_calib;

typedef struct {
    double cov50[2], cov80[2], cov95[2];
    double pit[2];                 /* per-bin fraction band */
    double bias_max;               /* |mean z| */
    double lag1_max;               /* |lag-1 autocorrelation of z| */
    uint64_t min_n;
} est_bands;

typedef enum { EST_NOT_CALIBRATED = 0, EST_CALIBRATED = 1 } est_verdict;

/* ---- numerics (exposed for tests) ---- */
/* regularized incomplete beta I_x(a,b); NaN on bad input or non-convergence */
double est_betai(double a, double b, double x);
/* Student-t (location 0, scale 1) CDF and upper tail P(T > t) */
double est_t_cdf(double t, double nu);
double est_t_sf(double t, double nu);
double est_norm_cdf(double x);
/* Phi^-1(p) by bisection on erfc, clipped to [-8, 8] (p <= 0 -> -8, >= 1 -> 8) */
double est_norm_ppf(double p);

/* ---- assumption ---- */
est_status est_assumption_check(const est_assumption *a);
est_status est_assumption_digest(const est_assumption *a, est_digest *out);

/* ---- observation ---- */
/* present = 0 means the sample is absent (MISSING); value is stored raw. */
est_status est_pobs_classify(const est_assumption *a, double value, int present,
                             const est_digest *evidence, est_pobs *out);

/* ---- candidate ---- */
est_status est_pred_init(const est_assumption *a, est_bstate *b);
est_status est_pred_predict(const est_assumption *a, est_bstate *b, uint32_t h,
                            est_dpred *out);
/* Re-classifies obs->value; a record whose class disagrees is EST_ERR_KIND. */
est_status est_pred_update(const est_assumption *a, est_bstate *b, const est_pobs *obs);
/* Scores obs against p. p must be the one-tick forecast made from b's current
 * state: same assumption, p->generation == b->generation and
 * p->horizon == b->gap + 1, else EST_ERR_STALE. Call before est_pred_update. */
est_status est_pred_innov(const est_bstate *b, const est_dpred *p, const est_pobs *obs,
                          est_pinnov *out);
/* Scores obs against ANY estimate p (any horizon), no staleness check: fills
 * out as est_pred_innov does (valid = 0 for a non-OK observation). Used for
 * multi-step scoring and for tool-built baseline pmfs. */
est_status est_dpred_score(const est_dpred *p, const est_pobs *obs, est_pinnov *out);
/* H-fold convolution of a one-step grid pmf (EST_PRED_N bins, offset index
 * EST_PRED_K = 0) by repeated squaring with edge folding as for F3/F4/F5; no
 * uniform floor is applied. h in 1..EST_PRED_MAX_H, else EST_ERR_TIME. */
est_status est_pmf_conv_pow(const double *one, uint32_t h, double *out);
/* Mixes EST_PRED_FLOOR uniform mass into pmf and renormalizes (as predict does). */
void est_pmf_floor(double *pmf);
/* width in mC of the central 80 % grid interval (k at CDF >= 0.1 .. >= 0.9) */
est_status est_dpred_width80(const est_dpred *p, double *mc);

/* ---- calibration evidence ---- */
void est_calib_init(est_calib *c);
/* Fractional coverage of central alpha interval for one innovation. */
double est_frac_cover(double F_lo, double F_hi, double alpha);
/* adds one valid innovation (iv->valid must be 1) and the width of p */
est_status est_calib_add(est_calib *c, const est_dpred *p, const est_pinnov *iv);
void est_bands_default(est_bands *b);
double est_calib_lag1(const est_calib *c);
est_verdict est_calib_verdict(const est_calib *c, const est_bands *b, char *reason,
                              size_t cap);

#endif /* OMEGA_EST_PRED_H */
