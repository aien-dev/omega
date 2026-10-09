/* Conditional subsystem state: phi_k = (<t_k| (x) I) Psi, the clock
 * probability w ||phi_k||^2 / ||Psi||^2 and rho_k = phi phi^dagger / ||phi||^2.
 * Each label is computed from Psi alone; no state is carried between labels. */
#include "at0_model.h"
#include <math.h>
#include <string.h>

at0_status at0_conditional_compute(const at0_case *c, const at0_state *psi, double psi_norm2,
                                   int k, at0_conditional *out)
{
    if (!c || !psi || !out || psi->clock_dim != c->clock_dim) return AT0_ERR_ARGUMENT;
    if (!(psi_norm2 > 0.0) || !isfinite(psi_norm2)) return AT0_ERR_ARGUMENT;
    if (k < 0 || k >= c->label_count) return AT0_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);
    at0_clock_vec t;
    at0_status st = at0_povm_clock_state(c, k, &t);
    if (st != AT0_OK) return st;
    for (int s = 0; s < AT0_SYSTEM_DIM; s++) {
        double complex acc = 0.0;
        for (int j = 0; j < c->clock_dim; j++) {
            double complex a;
            if ((st = at0_state_get(psi, j, s, &a)) != AT0_OK) return st;
            acc += conj(t.v[j]) * a;
        }
        out->phi.v[s] = acc;
    }
    double n2 = 0.0;
    for (int s = 0; s < AT0_SYSTEM_DIM; s++) {
        double re = creal(out->phi.v[s]), im = cimag(out->phi.v[s]);
        n2 += re * re + im * im;
    }
    if (!isfinite(n2)) return AT0_ERR_NONFINITE;
    out->phi_norm2 = n2;
    out->clock_probability = at0_rat_to_double(c->povm_weight) * n2 / psi_norm2;
    if (!isfinite(out->clock_probability)) return AT0_ERR_NONFINITE;
    out->defined_numeric = n2 > 0.0;
    if (out->defined_numeric) {
        for (int a = 0; a < AT0_SYSTEM_DIM; a++)
            for (int b = 0; b < AT0_SYSTEM_DIM; b++)
                out->rho.m[a][b] = out->phi.v[a] * conj(out->phi.v[b]) / n2;
    }
    return AT0_OK;
}
