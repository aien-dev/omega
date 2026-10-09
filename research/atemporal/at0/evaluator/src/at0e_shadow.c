/* Shadow oracle: an independent derivation of every AT-0 quantity from the case
 * alone (AT0_CASE_V1 section 3), used to falsify engine and reference values.
 *
 * Exact part (int128 rationals): eigenvalues e_s = h0 +/- |h|, eigenvectors with
 * complex-rational components, kernel pairs, Psi != 0, exact POVM identity test.
 * Numeric part (long double, 113-bit mantissa on this host): the conditional
 * state phi_k, clock probabilities, Pauli probabilities, Schrodinger reference,
 * POVM residual. Angles are reduced modulo one turn exactly in rationals before
 * any transcendental call. The stated bound is ESTIMATED, not proven.
 *
 * Derivation used (hand, from the contract):
 *   |t_k> = N^-1/2 sum_j exp(-2 pi i E_j k tau) |E_j>
 *   Psi   = P0 (|t_r> (x) |psi0>) = sum_{(j,s): E_j + e_s = 0} <E_j|t_r> <e_s|psi0> |E_j>|e_s>
 *   phi_k = (<t_k| (x) I) Psi = (1/N) sum_{s matched} exp(-2 pi i e_s (k - r) tau) <e_s|psi0> |e_s>
 * which equals Schrodinger evolution of psi0 over t_k - t_r restricted to the
 * matched eigencomponents. The reference uses all eigencomponents.
 */
#include "at0e.h"
#include <float.h>
#include <math.h>
#include <string.h>

typedef struct { long double re, im; } cld;
static cld cmk(long double re, long double im) { cld z = { re, im }; return z; }
static cld cadd(cld a, cld b) { return cmk(a.re + b.re, a.im + b.im); }
static cld cmul(cld a, cld b) { return cmk(a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re); }
static cld cconj(cld a) { return cmk(a.re, -a.im); }
static long double cabs2(cld a) { return a.re * a.re + a.im * a.im; }
static cld cscale(cld a, long double s) { return cmk(a.re * s, a.im * s); }
static long double i128_ld(i128 v) { int neg = v < 0; u128 u = neg ? (u128)0 - (u128)v : (u128)v; long double r = (long double)(unsigned long long)(u >> 64) * 18446744073709551616.0L + (long double)(unsigned long long)u; return neg ? -r : r; }
static long double rat_ld(rat a) { return i128_ld(a.n) / i128_ld(a.d); }
static cld crat_ld(crat z) { return cmk(rat_ld(z.re), rat_ld(z.im)); }
/* exp(-2 pi i theta) with theta a rational, reduced mod 1 exactly first */
static cld expm2pi(rat theta) {
    rat t = rat_mod1(theta);
    if (rat_is_zero(t)) return cmk(1.0L, 0.0L);
    /* exact quarter turns avoid rounding on the common ideal-clock grid */
    if (t.d == 4) { if (t.n == 1) return cmk(0.0L, -1.0L); if (t.n == 3) return cmk(0.0L, 1.0L); }
    if (t.d == 2) return cmk(-1.0L, 0.0L);
    long double a = 2.0L * 3.14159265358979323846264338327950288L * rat_ld(t);
    return cmk(cosl(a), -sinl(a));
}

/* eigenvectors (unnormalized complex rational); index 0 = e_+ = h0 + r, 1 = e_- = h0 - r */
static void eigvecs(const at0_case *c, crat v[2][2], rat e[2]) {
    rat r = c->radius;
    e[0] = rat_add(c->h0, r); e[1] = rat_sub(c->h0, r);
    if (rat_is_zero(r)) {
        v[0][0].re = rat_make(1, 1); v[0][0].im = rat_make(0, 1); v[0][1].re = rat_make(0, 1); v[0][1].im = rat_make(0, 1);
        v[1][0].re = rat_make(0, 1); v[1][0].im = rat_make(0, 1); v[1][1].re = rat_make(1, 1); v[1][1].im = rat_make(0, 1);
        return;
    }
    /* v_+ = (hz + r, hx + i hy); if zero, (hx - i hy, r - hz) */
    rat a = rat_add(c->hz, r);
    if (rat_is_zero(a) && rat_is_zero(c->hx) && rat_is_zero(c->hy)) {
        v[0][0].re = c->hx; v[0][0].im = rat_neg(c->hy); v[0][1].re = rat_sub(r, c->hz); v[0][1].im = rat_make(0, 1);
    } else {
        v[0][0].re = a; v[0][0].im = rat_make(0, 1); v[0][1].re = c->hx; v[0][1].im = c->hy;
    }
    /* v_- = (hz - r, hx + i hy); if zero, (hx - i hy, -r - hz) */
    rat b = rat_sub(c->hz, r);
    if (rat_is_zero(b) && rat_is_zero(c->hx) && rat_is_zero(c->hy)) {
        v[1][0].re = c->hx; v[1][0].im = rat_neg(c->hy); v[1][1].re = rat_sub(rat_neg(r), c->hz); v[1][1].im = rat_make(0, 1);
    } else {
        v[1][0].re = b; v[1][0].im = rat_make(0, 1); v[1][1].re = c->hx; v[1][1].im = c->hy;
    }
}
/* <v|psi0> exactly: conj(v0) psi0_0 + conj(v1) psi0_1 */
static crat inner_exact(const crat v[2], const crat psi[2]) {
    crat acc = { rat_make(0, 1), rat_make(0, 1) };
    for (int i = 0; i < 2; i++) {
        rat re = rat_add(rat_mul(v[i].re, psi[i].re), rat_mul(v[i].im, psi[i].im));
        rat im = rat_sub(rat_mul(v[i].re, psi[i].im), rat_mul(v[i].im, psi[i].re));
        acc.re = rat_add(acc.re, re); acc.im = rat_add(acc.im, im);
    }
    return acc;
}
static void pauli_probs(cld a, cld b, long double out[3][2]) {
    long double n2 = cabs2(a) + cabs2(b);
    cld ab = cmul(cconj(a), b);
    long double x = 2.0L * ab.re, y = 2.0L * ab.im, z = cabs2(a) - cabs2(b);
    out[0][0] = (n2 + x) / (2.0L * n2); out[0][1] = (n2 - x) / (2.0L * n2);
    out[1][0] = (n2 + y) / (2.0L * n2); out[1][1] = (n2 - y) / (2.0L * n2);
    out[2][0] = (n2 + z) / (2.0L * n2); out[2][1] = (n2 - z) / (2.0L * n2);
}

int shadow_compute(const at0_case *c, shadow *s, const char **why) {
    memset(s, 0, sizeof *s);
    *why = NULL;
    if (LDBL_MANT_DIG < 64) { *why = "long double narrower than 64 bits; shadow refuses"; return 0; }
    /* stated ESTIMATED bound: 1e-20 needs the 113-bit binary128 long double (aarch64); a 64-bit mantissa (x86-64) gets 1e-14 */
    s->bound = LDBL_MANT_DIG >= 113 ? 1e-20L : 1e-14L;

    crat v[2][2]; rat e[2];
    eigvecs(c, v, e);
    for (int si = 0; si < 2; si++) {
        rat neg = rat_neg(e[si]);
        s->matched[si] = 0;
        for (int j = 0; j < c->N; j++) if (rat_cmp(c->E[j], neg) == 0) { s->matched[si] = 1; break; }
    }
    s->kernel_dim = s->matched[0] + s->matched[1];
    crat cs[2]; int cs_zero[2];
    for (int si = 0; si < 2; si++) { cs[si] = inner_exact(v[si], c->psi0); cs_zero[si] = crat_is_zero(cs[si]); }
    s->psi_nonzero = (s->matched[0] && !cs_zero[0]) || (s->matched[1] && !cs_zero[1]);

    /* exact POVM identity test */
    s->povm_exact_identity = rat_cmp(c->w, rat_make(c->N, c->M)) == 0;
    for (int j = 0; j < c->N && s->povm_exact_identity; j++)
        for (int jj = j + 1; jj < c->N; jj++) {
            rat D = rat_sub(c->E[jj], c->E[j]);
            rat dt = rat_mul(D, c->tau);
            rat dtm = rat_mul(dt, rat_make(c->M, 1));
            if (rat_is_int(dt) || !rat_is_int(dtm)) { s->povm_exact_identity = 0; break; }
        }
    /* numeric POVM residual: Frobenius norm of w sum_k |t_k><t_k| - I */
    {
        long double fro = 0.0L, wn = rat_ld(c->w) / (long double)c->N;
        for (int j = 0; j < c->N; j++)
            for (int jj = 0; jj < c->N; jj++) {
                rat D = rat_sub(c->E[j], c->E[jj]);
                cld sum = cmk(0.0L, 0.0L);
                for (int k = 0; k < c->M; k++) sum = cadd(sum, expm2pi(rat_mul(rat_mul(D, rat_make(k, 1)), c->tau)));
                cld m = cscale(sum, wn);
                if (j == jj) m.re -= 1.0L;
                fro += cabs2(m);
            }
        s->povm_residual = sqrtl(fro);
    }
    /* norms and states */
    long double vn2[2]; cld vl[2][2], csl[2];
    for (int si = 0; si < 2; si++) {
        vl[si][0] = crat_ld(v[si][0]); vl[si][1] = crat_ld(v[si][1]);
        vn2[si] = cabs2(vl[si][0]) + cabs2(vl[si][1]);
        csl[si] = crat_ld(cs[si]);
    }
    long double psi_norm2 = 0.0L;
    for (int si = 0; si < 2; si++) if (s->matched[si]) psi_norm2 += cabs2(csl[si]) / vn2[si];
    psi_norm2 /= (long double)c->N;
    long double psi0_n2 = cabs2(crat_ld(c->psi0[0])) + cabs2(crat_ld(c->psi0[1]));
    (void)psi0_n2;

    for (int k = 0; k < c->M; k++) {
        cld phi[2] = { cmk(0, 0), cmk(0, 0) }, ref[2] = { cmk(0, 0), cmk(0, 0) };
        rat dk = rat_make(k - c->ref_index, 1);
        for (int si = 0; si < 2; si++) {
            cld ph = expm2pi(rat_mul(rat_mul(e[si], dk), c->tau));
            cld coef = cscale(cmul(ph, csl[si]), 1.0L / vn2[si]);
            for (int i = 0; i < 2; i++) {
                cld term = cmul(coef, vl[si][i]);
                ref[i] = cadd(ref[i], term);
                if (s->matched[si]) phi[i] = cadd(phi[i], cscale(term, 1.0L / (long double)c->N));
            }
        }
        s->clock_p_exact_zero[k] = !s->psi_nonzero;
        if (s->psi_nonzero) {
            s->clock_p[k] = rat_ld(c->w) * (cabs2(phi[0]) + cabs2(phi[1])) / psi_norm2;
            pauli_probs(phi[0], phi[1], s->pauli[k]);
        }
        pauli_probs(ref[0], ref[1], s->reference[k]);
    }
    return 1;
}
