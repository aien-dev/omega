/* Observable probabilities: P(sigma = +/-1) = Tr(rho (I +/- sigma)/2), each
 * sign computed directly from rho (never as one minus the other). */
#include "at0_model.h"
#include <math.h>

at0_status at0_pauli_matrix(at0_axis axis, at0_qubit_op *out)
{
    if (!out) return AT0_ERR_ARGUMENT;
    switch (axis) {
    case AT0_AXIS_X: out->m[0][0] = 0; out->m[0][1] = 1;    out->m[1][0] = 1;   out->m[1][1] = 0;  return AT0_OK;
    case AT0_AXIS_Y: out->m[0][0] = 0; out->m[0][1] = -I;   out->m[1][0] = I;   out->m[1][1] = 0;  return AT0_OK;
    case AT0_AXIS_Z: out->m[0][0] = 1; out->m[0][1] = 0;    out->m[1][0] = 0;   out->m[1][1] = -1; return AT0_OK;
    default: return AT0_ERR_ARGUMENT;
    }
}

at0_status at0_observable_probability(const at0_qubit_op *rho, at0_axis axis, at0_sign sign, double *out)
{
    if (!rho || !out) return AT0_ERR_ARGUMENT;
    if (sign != AT0_SIGN_PLUS && sign != AT0_SIGN_MINUS) return AT0_ERR_ARGUMENT;
    at0_qubit_op sigma;
    at0_status st = at0_pauli_matrix(axis, &sigma);
    if (st != AT0_OK) return st;
    double sgn = sign == AT0_SIGN_PLUS ? 1.0 : -1.0;
    /* projector Pi = (I + sgn * sigma) / 2 ; probability = Tr(rho Pi) = sum_ab rho[a][b] Pi[b][a] */
    double complex tr = 0.0;
    for (int a = 0; a < AT0_SYSTEM_DIM; a++) {
        for (int b = 0; b < AT0_SYSTEM_DIM; b++) {
            double complex pi_ba = 0.5 * ((a == b ? 1.0 : 0.0) + sgn * sigma.m[b][a]);
            tr += rho->m[a][b] * pi_ba;
        }
    }
    double p = creal(tr);
    if (!isfinite(p)) return AT0_ERR_NONFINITE;
    *out = p;
    return AT0_OK;
}
