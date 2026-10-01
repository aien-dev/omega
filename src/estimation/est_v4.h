/* ESTIMATION v4 (protocol docs/estimation/protocols/est-v4.md): persistence-
 * aware predictive families for a quantized, 1 Hz temperature series.
 *
 * v3 finding (receipts/est3c-v3/RESULT.md): F5 passed every one-step rule and
 * failed only the ten-step rule (0.809), because it treated successive changes
 * as independent and held its volatility constant over the horizon. v4 keeps
 * the v3 discrete predictive (est_dpred, scored by est_dpred_score) and replaces
 * the change model with a small linear state model whose multi-step forecast is
 * propagated through the model, plus a Student-t scale with mean reversion.
 *
 * Families (state dimension d, observation y = H x + quantization noise):
 *   G1 thermal lag      x = [T, U]       T' = a T + (1-a) U,  U' = U + w,
 *                                        a = exp(-1/tau), w ~ (0, q).
 *   G2 AR(1) change     x = [Y, D]       D' = rho D + w,  Y' = Y + D' ,
 *                                        so changes are AR(1) correlated.
 *   G3 two time consts  x = [Tf, Ts, U]  Tf' = af Tf + (1-af) w0 U,
 *                                        Ts' = as Ts + (1-as)(1-w0) U, U' = U + w,
 *                                        tau_s = 10 tau_f, w0 = 1/2, y = Tf + Ts.
 *   r = quantum^2 / 12 is the observation variance in every family.
 *
 * Predictive at horizon h (ticks after the current state):
 *   location m_h = H A^h x,  V_h = H (A^h P A^h' + sum_{j<h} A^j Q A^j') H' + r.
 *   One-step law (h = 1):   Y_1 ~ m_1 + c sqrt(g V_1) t_nu.
 *   Horizon law (h >= 2):   Y_h ~ m_h + c_h sqrt(gbar_h V_h) t_nu_h,
 *     gbar_h = (1/h) sum_{j=1..h} (gl + (g - gl) phi^(j-1)),
 *   so location and the shape of the spread come from propagating the model,
 *   the scale forecast reverts toward the slow level gl at rate phi, and the
 *   horizon tail (nu_h) and multiplier (c_h) are separate parameters fitted on
 *   the ten-step score (protocol section 5, stage 2). Both laws are discretized
 *   onto the quantum grid around the anchor exactly as v3 (edge bins take the
 *   tails, EST_PRED_FLOOR mixed in).
 * Update on a valid observation y (e = y - m_1 before the update):
 *   Kalman time + measurement update of (x, P) with the unscaled Q and r;
 *   g' = lam g + (1-lam) max(e^2, r) / V_1;
 *   gl' = EST4_GL_LAMBDA gl + (1 - EST4_GL_LAMBDA) g'.
 * On a missing tick: time update of (x, P) only; g unchanged; gl as above.
 * phi, nu_h and c_h never change the filter or the one-step law.
 *
 * Standalone, bounded, no allocation, no globals; like est_pred.c it is not
 * linked into the runtime and references no authority entry point. */
#ifndef OMEGA_EST_V4_H
#define OMEGA_EST_V4_H

#include <stdint.h>

#include "est_pred.h"

#define EST4_MAX_D 3u
#define EST4_GL_LAMBDA 0.995
#define EST4_TAU_RATIO 10.0      /* G3: tau_slow / tau_fast */
#define EST4_G3_W 0.5            /* G3: share of U carried by the fast part */
#define EST_DOMAIN_V4 "omega.est.v4.params.v1"

typedef enum { EST4_G1_LAG = 1, EST4_G2_AR = 2, EST4_G3_TWO = 3 } est4_family;

typedef struct {
    est4_family family;
    double dyn;      /* G1, G3: tau (fast) in ticks, > 0; G2: rho in [0, 1) */
    double q;        /* process variance per tick, mC^2, > 0 */
    double lam;      /* fast scale memory, [0, 1) */
    double nu;       /* one-step Student-t degrees of freedom, > 0 */
    double c;        /* one-step scale multiplier, > 0 */
    double phi;      /* horizon scale persistence, (0, 1] */
    double nu_h;     /* horizon Student-t degrees of freedom, > 0 */
    double c_h;      /* horizon scale multiplier, > 0 */
    double quantum;  /* grid quantum, mC, > 0 */
} est4_params;

typedef struct {
    est4_params p;
    uint32_t d;
    double A[EST4_MAX_D * EST4_MAX_D], Q[EST4_MAX_D * EST4_MAX_D], H[EST4_MAX_D];
    double r;
    uint32_t has_anchor;
    double anchor;          /* last valid observation */
    uint32_t gap;           /* ticks since the last valid observation */
    double x[EST4_MAX_D], P[EST4_MAX_D * EST4_MAX_D];
    double g, gl;           /* scale state (dimensionless, relative to S_1) */
    uint64_t generation;    /* +1 per update or missing tick */
} est4_state;

/* Continuous predictive: Y_h ~ loc + scale * t_nu. */
typedef struct { uint32_t h; double loc, scale, nu; } est4_pred;

est_status est4_params_check(const est4_params *p);
est_status est4_params_digest(const est4_params *p, est_digest *out);
est_status est4_init(const est4_params *p, est4_state *s);
/* Predict h ticks ahead of the current state (1 <= h <= EST_PRED_MAX_H).
 * Refuses (EST_ERR_STALE) before the first valid observation. */
est_status est4_predict(const est4_state *s, uint32_t h, est4_pred *out);
/* Discrete predictive pmf around s->anchor, floored as in v3. dp->horizon is
 * gap + h (ticks after the anchor); dp->family is 0 (not an est_pred family). */
est_status est4_dpred(const est4_state *s, const est4_pred *pr, est_dpred *dp);
/* Closed-form log score of an on-grid y under the est4_dpred pmf for anchor:
 * outcome bin k = round((y - anchor)/q) clamped to +-EST_PRED_K, edge bins
 * take the tails, EST_PRED_FLOOR mix applied. Equals the pmf score up to
 * rounding (the pmf path renormalises sums that are 1 to rounding).
 * F_lo/F_hi optional (floored CDF below / through the bin). */
double est4_fast_logp(const est4_pred *pr, double quantum, double anchor, double y, double *F_lo, double *F_hi);
/* Model moments at horizon h: m_h and V_h (est_v4.h), no scale law. */
est_status est4_moments(const est4_state *s, uint32_t h, double *loc, double *V);
/* gbar_h = (1/h) sum_{j=1..h} (gl + (g - gl) phi^(j-1)). */
double est4_gbar(double g, double gl, double phi, uint32_t h);
/* Advance one tick. present = 0 (or a non-finite value) is a missing tick. */
est_status est4_update(est4_state *s, int present, double y);

#endif
