/* Hamiltonian construction: H_S = h0 I + hx X + hy Y + hz Z with its exact
 * rational spectrum h0 +/- |h| and binary64 eigenvectors; H_total applied as
 * H_C (x) I + I (x) H_S on the combined state. */
#include "at0_model.h"
#include <math.h>
#include <string.h>

at0_status at0_hamiltonian_system(const at0_case *c, at0_system_hamiltonian *out)
{
    if (!c || !out) return AT0_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);
    at0_status st;
    if ((st = at0_rat_sub(c->h0, c->h_norm, &out->eig[0])) != AT0_OK) return st;
    if ((st = at0_rat_add(c->h0, c->h_norm, &out->eig[1])) != AT0_OK) return st;
    double h0 = at0_rat_to_double(c->h0), hx = at0_rat_to_double(c->hx);
    double hy = at0_rat_to_double(c->hy), hz = at0_rat_to_double(c->hz);
    double r = at0_rat_to_double(c->h_norm);
    out->hs.m[0][0] = h0 + hz;      out->hs.m[0][1] = hx - I * hy;
    out->hs.m[1][0] = hx + I * hy;  out->hs.m[1][1] = h0 - hz;
    if (at0_rat_is_zero(c->h_norm)) {
        /* H_S = h0 I: any basis is an eigenbasis; use the computational one */
        out->degenerate = 1;
        out->vec[0].v[0] = 1.0; out->vec[0].v[1] = 0.0;
        out->vec[1].v[0] = 0.0; out->vec[1].v[1] = 1.0;
        return AT0_OK;
    }
    at0_rat rpz;
    if ((st = at0_rat_add(c->h_norm, c->hz, &rpz)) != AT0_OK) return st;
    if (at0_rat_is_zero(rpz)) {
        /* h = (0, 0, -|h|): eigenvalue -|h| on |0>, +|h| on |1> */
        out->vec[0].v[0] = 1.0; out->vec[0].v[1] = 0.0;
        out->vec[1].v[0] = 0.0; out->vec[1].v[1] = 1.0;
        return AT0_OK;
    }
    double a = r + hz;
    double norm = sqrt(2.0 * r * a);          /* |(a, hx + i hy)| = sqrt(a^2 + hx^2 + hy^2) = sqrt(2 r a) */
    if (!(norm > 0.0) || !isfinite(norm)) return AT0_ERR_NONFINITE;
    /* eigenvalue h0 - |h| */
    out->vec[0].v[0] = -(hx - I * hy) / norm;
    out->vec[0].v[1] = a / norm;
    /* eigenvalue h0 + |h| */
    out->vec[1].v[0] = a / norm;
    out->vec[1].v[1] = (hx + I * hy) / norm;
    return AT0_OK;
}

at0_status at0_hamiltonian_apply_total(const at0_case *c, const at0_system_hamiltonian *hs,
                                       const at0_state *in, at0_state *out)
{
    if (!c || !hs || !in || !out || in->clock_dim != c->clock_dim) return AT0_ERR_ARGUMENT;
    at0_status st = at0_state_init(out, c->clock_dim);
    if (st != AT0_OK) return st;
    for (int j = 0; j < c->clock_dim; j++) {
        double ej = at0_rat_to_double(c->clock_energies[j]);
        double complex a0, a1;
        if ((st = at0_state_get(in, j, 0, &a0)) != AT0_OK) return st;
        if ((st = at0_state_get(in, j, 1, &a1)) != AT0_OK) return st;
        double complex b0 = ej * a0 + hs->hs.m[0][0] * a0 + hs->hs.m[0][1] * a1;
        double complex b1 = ej * a1 + hs->hs.m[1][0] * a0 + hs->hs.m[1][1] * a1;
        if ((st = at0_state_set(out, j, 0, b0)) != AT0_OK) return st;
        if ((st = at0_state_set(out, j, 1, b1)) != AT0_OK) return st;
    }
    return at0_state_check_finite(out);
}
