/* Linear readout for the Omega reservoir (omega#440, Gate 1): ridge
 * regression with an unpenalized bias, and the NRMSE of Sci. Adv.
 * eaed0971 Text S6 (eqs. 24-25).
 *
 * Precision: binary64. Operations: + - * / and the correctly rounded
 * square root (AArch64 FSQRT through __builtin_sqrt, built with
 * -fno-math-errno so no libm call exists; checked by nm in
 * mk/reservoir.mk). Build with -ffp-contract=off -fno-fast-math.
 *
 * Fit: design Phi = [1, x] (n_samples x (1 + n_features)); normal matrix
 * G = Phi^T Phi + lambda * diag(0, 1, ..., 1), right side R = Phi^T Y. Every
 * entry is a sum over samples in index order 0..n_samples-1. G = L D L^T
 * (no pivoting, row by row); a pivot d_j <= 1e-13 * G_jj refuses with
 * OMEGA_RES_ERR_SINGULAR. Then L z = R, z = z / D, L^T W = z. This is the
 * closed form of the reference's ridge_multioutput (Lorenz). With
 * lambda = 0 it is ordinary least squares with intercept, the model the
 * reference fits with MATLAB fitlm (NARMA); fitlm uses a QR solve, so on
 * badly conditioned features the two can differ (Gate 2 records this).
 *
 * Feature construction (which states, squared states, range scaling) is
 * the caller's; the readout only sees the matrix it is given.
 *
 * Bounded: n_features <= OMEGA_RES_READOUT_MAX_FEATURES, n_outputs <=
 * OMEGA_RES_READOUT_MAX_OUTPUTS; the normal matrix lives in a caller-owned
 * workspace (~0.5 MiB). No allocation, no globals. */
#ifndef OMEGA_RESERVOIR_READOUT_H
#define OMEGA_RESERVOIR_READOUT_H

#include "omega_reservoir.h"

#define OMEGA_RES_READOUT_MAX_FEATURES 256u
#define OMEGA_RES_READOUT_MAX_OUTPUTS 4u
#define OMEGA_RES_READOUT_DIM (OMEGA_RES_READOUT_MAX_FEATURES + 1u)

typedef struct {
    uint32_t n_features, n_outputs;
    double lambda;
    /* (1 + n_features) x n_outputs, row-major; row 0 is the bias. */
    double w[OMEGA_RES_READOUT_DIM * OMEGA_RES_READOUT_MAX_OUTPUTS];
} OmegaResReadout;

typedef struct {
    double g[OMEGA_RES_READOUT_DIM * OMEGA_RES_READOUT_DIM]; /* G, then L below the diagonal */
    double d[OMEGA_RES_READOUT_DIM];
    double rhs[OMEGA_RES_READOUT_DIM * OMEGA_RES_READOUT_MAX_OUTPUTS];
} OmegaResReadoutWorkspace;

/* x: n_samples x n_features row-major; y: n_samples x n_outputs row-major.
 * lambda >= 0. Refuses non-finite data (OMEGA_RES_ERR_NONFINITE), bad
 * sizes or lambda (OMEGA_RES_ERR_PARAM) and a singular system. */
OmegaResStatus omega_res_readout_fit(const double *x, const double *y, uint32_t n_samples, uint32_t n_features,
                                     uint32_t n_outputs, double lambda, OmegaResReadoutWorkspace *ws,
                                     OmegaResReadout *out);

/* y_out[o] = w[0][o] + sum_f x[f] * w[1 + f][o], f in order. */
OmegaResStatus omega_res_readout_apply(const OmegaResReadout *r, const double *x, double *y_out);

/* NRMSE(ref, pred) = sqrt( sum (ref - pred)^2 / sum (ref - mean(ref))^2 ),
 * sums in index order, mean = (sum ref) / n. Refuses n < 2, non-finite
 * data and a constant reference (zero denominator). */
OmegaResStatus omega_res_nrmse(const double *ref, const double *pred, uint32_t n, double *out);

#endif /* OMEGA_RESERVOIR_READOUT_H */
