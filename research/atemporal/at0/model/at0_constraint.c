/* Hamiltonian constraint: exact kernel of H_total, nullspace projection of the
 * reference product state, and the residual || H_total Psi_hat ||. */
#include "at0_model.h"
#include <math.h>
#include <string.h>

at0_status at0_constraint_kernel(const at0_case *c, const at0_system_hamiltonian *hs, at0_kernel *out)
{
    if (!c || !hs || !out) return AT0_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);
    for (int j = 0; j < c->clock_dim; j++) {
        for (int s = 0; s < AT0_SYSTEM_DIM; s++) {
            at0_rat sum;
            at0_status st = at0_rat_add(c->clock_energies[j], hs->eig[s], &sum);
            if (st != AT0_OK) return st;
            if (at0_rat_is_zero(sum)) {
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
    at0_rat a, rpz, hx = c->hx, hy = c->hy;
    at0_status st = at0_rat_add(c->h_norm, c->hz, &rpz);
    if (st != AT0_OK) return st;
    if (at0_rat_is_zero(rpz)) {
        /* h = (0, 0, -|h|): the eigenvectors are |0> (s = 0) and |1> (s = 1), as in at0_hamiltonian_system */
        at0_crat p = s == 0 ? p0 : p1;
        *zero = at0_rat_is_zero(p.re) && at0_rat_is_zero(p.im);
        return AT0_OK;
    }
    /* conj(v_s) . psi0 with v_0 = (-(hx - i hy), a), v_1 = (a, hx + i hy), a = |h| + hz:
     *   s=0: conj(v_0) = (-(hx + i hy), a)  -> -(hx + i hy) p0 + a p1
     *   s=1: conj(v_1) = (a, hx - i hy)     ->  a p0 + (hx - i hy) p1
     * Each is u*p + v*q with u, v complex rationals; test both parts for zero
     * with cross-multiplied 128-bit integers (no reduction needed). */
    a = rpz;
    at0_crat u, v, p, q;
    if (s == 0) {
        u.re.n = -hx.n; u.re.d = hx.d; u.im.n = -hy.n; u.im.d = hy.d;   /* -(hx + i hy) */
        v.re = a; v.im.n = 0; v.im.d = 1;
        p = p0; q = p1;
    } else {
        u.re = a; u.im.n = 0; u.im.d = 1;
        v.re = hx; v.im.n = -hy.n; v.im.d = hy.d;                          /* hx - i hy */
        p = p0; q = p1;
    }
    /* (u.re + i u.im)(p.re + i p.im) + (v.re + i v.im)(q.re + i q.im) */
    /* real: u.re p.re - u.im p.im + v.re q.re - v.im q.im ; imag: u.re p.im + u.im p.re + v.re q.im + v.im q.re */
    /* The product test above can overflow for extreme inputs, so decide the zero test with
     * the limit-checked rational API instead: each operand is at most 2^20, products are
     * at most 2^40 and are reduced after every step; AT0_ERR_OVERFLOW is reported, never wrapped. */
    {
        at0_rat r1, r2, r3, r4, re, im, t;
        /* real part */
        if ((st = at0_rat_mul(u.re, p.re, &r1)) != AT0_OK) return st;
        if ((st = at0_rat_mul(u.im, p.im, &r2)) != AT0_OK) return st;
        if ((st = at0_rat_mul(v.re, q.re, &r3)) != AT0_OK) return st;
        if ((st = at0_rat_mul(v.im, q.im, &r4)) != AT0_OK) return st;
        if ((st = at0_rat_sub(r1, r2, &t)) != AT0_OK) return st;
        if ((st = at0_rat_add(t, r3, &t)) != AT0_OK) return st;
        if ((st = at0_rat_sub(t, r4, &re)) != AT0_OK) return st;
        /* imaginary part */
        if ((st = at0_rat_mul(u.re, p.im, &r1)) != AT0_OK) return st;
        if ((st = at0_rat_mul(u.im, p.re, &r2)) != AT0_OK) return st;
        if ((st = at0_rat_mul(v.re, q.im, &r3)) != AT0_OK) return st;
        if ((st = at0_rat_mul(v.im, q.re, &r4)) != AT0_OK) return st;
        if ((st = at0_rat_add(r1, r2, &t)) != AT0_OK) return st;
        if ((st = at0_rat_add(t, r3, &t)) != AT0_OK) return st;
        if ((st = at0_rat_add(t, r4, &im)) != AT0_OK) return st;
        *zero = at0_rat_is_zero(re) && at0_rat_is_zero(im);
    }
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
