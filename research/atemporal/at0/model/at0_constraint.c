/* Hamiltonian constraint: exact kernel of H_total, nullspace projection of the
 * reference product state, and the residual || H_total Psi_hat ||. */
#include "at0_model.h"
#include <math.h>
#include <string.h>

at0_status at0_constraint_kernel(const at0_case *c, const at0_system_hamiltonian *hs, at0_kernel *out)
{
    if (!c || !hs || !out) return AT0_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);
    /* E_j + h0 -/+ |h| == 0  <=>  (E_j + h0) == +/-|h|. E_j + h0 has a denominator <= 2^40 and
     * |h| is representable, so at0_rat_cmp decides exactly; the eigenvalues themselves
     * (reduced denominator up to 2^80) are never materialized as rationals. */
    at0_rat neg_norm = c->h_norm;
    neg_norm.n = -neg_norm.n;
    for (int j = 0; j < c->clock_dim; j++) {
        at0_rat t;
        at0_status st = at0_rat_add(c->clock_energies[j], c->h0, &t);
        if (st != AT0_OK) return st;
        for (int s = 0; s < AT0_SYSTEM_DIM; s++) {
            if (at0_rat_cmp(t, s == 0 ? c->h_norm : neg_norm) == 0) {
                if (out->kernel_dim >= AT0_TOTAL_DIM_MAX) return AT0_ERR_INTERNAL;
                out->match_j[out->kernel_dim] = j;
                out->match_s[out->kernel_dim] = s;
                out->kernel_dim++;
            }
        }
    }
    return AT0_OK;
}

/* exact test: is <e_s|psi_0> zero? uses the unnormalized rational eigenvector
 * (-(hx - i hy), |h| + hz) for s = 0 and (|h| + hz, hx + i hy) for s = 1;
 * in the degenerate case the eigenvectors are |0>, |1>. */
static at0_status overlap_is_zero(const at0_case *c, const at0_system_hamiltonian *hs, int s, int *zero)
{
    at0_crat p0 = c->psi0[0], p1 = c->psi0[1];
    if (hs->degenerate) {
        at0_crat p = s == 0 ? p0 : p1;
        *zero = at0_rat_is_zero(p.re) && at0_rat_is_zero(p.im);
        return AT0_OK;
    }
    /* a = |h| + hz = A / D with D = dx dy dz (unreduced, |A| < 2^62, D < 2^60) */
    at0_i128 A, D;
    at0_status st = at0_exact_norm_plus_hz(c->hx, c->hy, c->hz, c->h_norm, &A, &D);
    if (st != AT0_OK) return st;
    if (A == 0) {
        /* h = (0, 0, -|h|): the eigenvectors are |0> (s = 0) and |1> (s = 1), as in at0_hamiltonian_system */
        at0_crat p = s == 0 ? p0 : p1;
        *zero = at0_rat_is_zero(p.re) && at0_rat_is_zero(p.im);
        return AT0_OK;
    }
    /* conj(v_s) . psi0 with v_0 = (-(hx - i hy), a), v_1 = (a, hx + i hy):
     *   s=0: conj(v_0) = (-(hx + i hy), a)  -> X p0 + Y p1 with X = -(hx + i hy), Y = a
     *   s=1: conj(v_1) = (a, hx - i hy)     -> X p0 + Y p1 with X = a, Y = hx - i hy
     * Every coefficient is written over the common denominator D: hx = nx (dy dz) / D,
     * hy = ny (dx dz) / D, a = A / D, so each coefficient numerator is < 2^62.
     * real = Xr p0r - Xi p0i + Yr p1r - Yi p1i,  imag = Xr p0i + Xi p0r + Yr p1i + Yi p1r.
     * Each part is a sum of four terms (coef / D) * (m / d) with m <= 2^20, d <= 2^20, of which
     * at most three have a nonzero coefficient (X or Y is real). at0_exact_sum_is_zero drops the
     * zero terms and cross-multiplies: |coef * m * d' * d''| < 2^62 * 2^20 * 2^40 = 2^122 per
     * term, three terms < 2^124, within 128-bit signed range; the common factor D cancels. */
    at0_i128 hxn = (at0_i128)c->hx.n * ((at0_i128)c->hy.d * c->hz.d);
    at0_i128 hyn = (at0_i128)c->hy.n * ((at0_i128)c->hx.d * c->hz.d);
    at0_i128 Xr, Xi, Yr, Yi;
    if (s == 0) { Xr = -hxn; Xi = -hyn; Yr = A; Yi = 0; }
    else        { Xr = A;    Xi = 0;    Yr = hxn; Yi = -hyn; }
    at0_i128 num[4], den[4];
    int zr, zi;
    /* real part */
    num[0] =  Xr * p0.re.n; den[0] = p0.re.d;
    num[1] = -Xi * p0.im.n; den[1] = p0.im.d;
    num[2] =  Yr * p1.re.n; den[2] = p1.re.d;
    num[3] = -Yi * p1.im.n; den[3] = p1.im.d;
    if ((st = at0_exact_sum_is_zero(4, num, den, &zr)) != AT0_OK) return st;
    /* imaginary part */
    num[0] = Xr * p0.im.n; den[0] = p0.im.d;
    num[1] = Xi * p0.re.n; den[1] = p0.re.d;
    num[2] = Yr * p1.im.n; den[2] = p1.im.d;
    num[3] = Yi * p1.re.n; den[3] = p1.re.d;
    if ((st = at0_exact_sum_is_zero(4, num, den, &zi)) != AT0_OK) return st;
    *zero = zr && zi;
    return AT0_OK;
}

at0_status at0_constraint_physical_state(const at0_case *c, const at0_system_hamiltonian *hs,
                                         const at0_kernel *k, at0_state *psi)
{
    if (!c || !hs || !k || !psi) return AT0_ERR_ARGUMENT;
    at0_status st = at0_state_init(psi, c->clock_dim);
    if (st != AT0_OK) return st;
    at0_clock_vec tr;
    if ((st = at0_povm_clock_state(c, c->reference_index, &tr)) != AT0_OK) return st;
    double complex psi0[AT0_SYSTEM_DIM];
    for (int s = 0; s < AT0_SYSTEM_DIM; s++)
        psi0[s] = at0_rat_to_double(c->psi0[s].re) + I * at0_rat_to_double(c->psi0[s].im);
    /* exact decision whether the projected state is the zero vector */
    int all_zero = 1;
    for (int m = 0; m < k->kernel_dim; m++) {
        int z;
        if ((st = overlap_is_zero(c, hs, k->match_s[m], &z)) != AT0_OK) return st;
        if (!z) { all_zero = 0; break; }
    }
    if (all_zero) return AT0_OK;      /* Psi = 0 exactly: leave the zero state */
    for (int m = 0; m < k->kernel_dim; m++) {
        int j = k->match_j[m], s = k->match_s[m];
        if (j < 0 || j >= c->clock_dim || s < 0 || s >= AT0_SYSTEM_DIM) return AT0_ERR_ARGUMENT;
        const at0_qubit *e = &hs->vec[s];
        double complex overlap = conj(e->v[0]) * psi0[0] + conj(e->v[1]) * psi0[1];   /* <e_s|psi_0> */
        double complex coef = tr.v[j] * overlap;                                        /* <E_j|t_r><e_s|psi_0> */
        for (int sp = 0; sp < AT0_SYSTEM_DIM; sp++) {
            double complex cur;
            if ((st = at0_state_get(psi, j, sp, &cur)) != AT0_OK) return st;
            if ((st = at0_state_set(psi, j, sp, cur + coef * e->v[sp])) != AT0_OK) return st;
        }
    }
    return at0_state_check_finite(psi);
}

at0_status at0_constraint_residual(const at0_case *c, const at0_system_hamiltonian *hs,
                                   const at0_state *psi, double *out)
{
    if (!c || !hs || !psi || !out) return AT0_ERR_ARGUMENT;
    double n2;
    at0_status st = at0_state_norm2(psi, &n2);
    if (st != AT0_OK) return st;
    if (!(n2 > 0.0)) return AT0_ERR_ARGUMENT;        /* residual of the zero state is undefined */
    at0_state h;
    if ((st = at0_hamiltonian_apply_total(c, hs, psi, &h)) != AT0_OK) return st;
    double hn2;
    if ((st = at0_state_norm2(&h, &hn2)) != AT0_OK) return st;
    double r = sqrt(hn2 / n2);
    if (!isfinite(r)) return AT0_ERR_NONFINITE;
    *out = r;
    return AT0_OK;
}
