/* EST-1 independent reference: linear-Gaussian filter in INFORMATION FORM.
 *
 * Plain double arrays at the interface, long double inside. Depends on
 * est_types.h only for EST_MAX_DIM. Matrices are row-major.
 *
 * State is kept as Y = P^-1 and y = Y x.
 *   update:  Y+ = Y- + H^T R^-1 H,   y+ = y- + H^T R^-1 z
 *   predict: P = Y^-1, x = P y, x' = F x, P' = F P F^T + Q, Y' = P'^-1, y' = Y' x'
 * Needs a finite, invertible P0. Test-only code, not part of the runtime. */
#ifndef OMEGA_EST_REF_INFO_H
#define OMEGA_EST_REF_INFO_H

#include "est_types.h"

typedef struct {
    unsigned n;
    long double Y[EST_MAX_DIM * EST_MAX_DIM];
    long double y[EST_MAX_DIM];
} est_ref_info;

/* All functions return 0 on success, nonzero on bad dimension or a singular
 * matrix (never repaired). */
int est_ref_init(est_ref_info *r, unsigned n, const double *x0, const double *P0);
int est_ref_predict(est_ref_info *r, const double *F, const double *Q);
int est_ref_update(est_ref_info *r, unsigned m, const double *H, const double *R,
                   const double *z);
/* Current mean and covariance (P = Y^-1, x = P y). Either output may be NULL. */
int est_ref_get(const est_ref_info *r, double *x, double *P);
/* Against the CURRENT state (call after predict, before update):
 * ymean = H x, S = H P H^T + R, nu = z - H x, nis = nu^T S^-1 nu (own solver). */
int est_ref_innovation(const est_ref_info *r, unsigned m, const double *H,
                       const double *R, const double *z, double *ymean, double *S,
                       double *nu, double *nis);

#endif
