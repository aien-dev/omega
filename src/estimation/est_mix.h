/* EST-3b noise shape: zero-mean Gaussian scale mixture of a one-step change
 * (protocol v2, docs/estimation/EST23_PROTOCOL_V2.md).
 *
 * Why it exists: under protocol v1 the one-step innovations of the CPU thermal
 * signal were heavy-tailed (recorded failure class: model misspecification,
 * docs/estimation/receipts/est23-v1/RESULT.md). One Gaussian width cannot be
 * right both in the middle and in the tails. This module gives the predicted
 * change a declared shape: k components with weights w[j] and variances v[j]
 * per logical step. An h-step change is the sum of h independent draws, which
 * is again a finite Gaussian mixture (enumerated over the multinomial counts),
 * so coverage, quantiles and log density are exact for h <= EST_MIX_MAX_H.
 *
 * The shape is data about uncertainty, like a covariance: it grants nothing
 * and reads nothing. Pure (no I/O, no allocation, no globals); libm only. */
#ifndef OMEGA_EST_MIX_H
#define OMEGA_EST_MIX_H

#include <stddef.h>
#include <stdint.h>

#include "est_types.h"

#define EST_MIX_MAX 3u
#define EST_MIX_MAX_H 64u        /* longest horizon with an exact h-fold sum */

typedef struct {
    uint32_t k;                  /* components, 1..EST_MIX_MAX */
    double w[EST_MIX_MAX];       /* weights, each >= 0, sum 1 within 1e-12 */
    double v[EST_MIX_MAX];       /* variance per step, each finite and > 0 */
} est_mix;

est_status est_mix_check(const est_mix *mx);
/* sum_j w[j] v[j]: the variance of a one-step change */
double est_mix_total_var(const est_mix *mx);
/* P(|e| <= t) for e the sum of h independent draws (1 <= h <= EST_MIX_MAX_H) */
est_status est_mix_cdf_abs(const est_mix *mx, uint32_t h, double t, double *out);
/* smallest t with P(|e| <= t) >= p, p in (0,1): bisection, 200 halvings */
est_status est_mix_quantile_abs(const est_mix *mx, uint32_t h, double p, double *out);
/* natural log of the density of the h-step sum at e */
est_status est_mix_logpdf(const est_mix *mx, uint32_t h, double e, double *out);
/* Deterministic EM fit of a k-component shape to changes e[0..n-1] (one step
 * each). Start: weights 1/k, variances V0 * {0.01, 0.3, 3} (first k), V0 the
 * mean square of e. `iters` full EM rounds; every variance is held at or above
 * floor_var after each M step. loglik = sum of log densities at the end. */
est_status est_mix_fit_em(const double *e, size_t n, uint32_t k, double floor_var,
                          uint32_t iters, est_mix *out, double *loglik);

#endif /* OMEGA_EST_MIX_H */
