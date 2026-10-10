/* binary64 evaluation of the AT-1 quantities (AT1_RESULT_V1 section 3) from the exact kernel.
 *
 *   |t_k>  = N^(-1/2) sum_j exp(-2 pi i E_j k tau) |E_j>      turn count E_j k tau reduced
 *                                                             exactly modulo 1 (int64) first
 *   Psi    = P_0 (|t_r> (x) psi_0) = sum_j <E_j|t_r> y_j       y_j = P_0 (|E_j> (x) psi_0) exact
 *                                                             (at1_exact.c), rounded once
 *   constraint_residual = || H_total Psi_hat ||_2             literal D x D product, V included
 *   povm_residual = || sum_k w |t_k><t_k| - I ||_F            pairwise sum over k
 *   phi_k  = (<t_k| (x) I) Psi                                bra conjugates the coefficients
 *   p(k)   = w ||phi_k||^2 / ||Psi||^2
 *   P(A, s | k) = Tr(rho_k (I + s A)/2), rho_k = phi_k phi_k^dagger / ||phi_k||^2
 * Each Pauli outcome is a separate trace against its own projector, never 1 - other.
 *
 * Bounds (bound_kind ESTIMATED, first-order operation counts with a safety factor 2; not a proof).
 * eps = 2^-53, L = number of clock levels carrying Psi (exact, at1_exact.c), c = L + 12,
 * delta = 16 eps (relative error of Psi), eta = sqrt(L / N) (delta + c eps) (error of phi_k / ||Psi||):
 *   constraint_residual  2 [Hmax delta + 8 eps (Hmax + 1)] + 4 eps value
 *   povm_residual        2 (ceil(log2 M) + 8) eps (M w + 1) + 4 eps value
 *   clock_probability    2 [2 sqrt(w p) eta + (2 delta + (2L + 12) eps) p] + 4 w eta^2
 *   pauli                2 [2 eta sqrt(w / p) + 16 eps]
 * Derivation in README.md (section "Numerics"). They assume libm cos, sin and sqrt within a few
 * ulp, which holds for glibc and Apple libm on the targets named there. */
#include "at1_model.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AT1_EPS 1.1102230246251565404e-16      /* 2^-53 */
static const double TWO_PI = 6.283185307179586476925286766559005768;

typedef struct { double re, im; } cplx;
static cplx c_mul(cplx a, cplx b) { cplx r = { a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re }; return r; }
static cplx c_add(cplx a, cplx b) { cplx r = { a.re + b.re, a.im + b.im }; return r; }
static cplx c_conj(cplx a) { cplx r = { a.re, -a.im }; return r; }
static double c_abs2(cplx a) { return a.re * a.re + a.im * a.im; }

void at1_f64_format(double x, char *buf, size_t cap)
{
    if (!isfinite(x)) { snprintf(buf, cap, "nonfinite"); return; }
    if (x == 0.0) x = 0.0;                     /* -0.0 is written as +0.0 */
    uint64_t bits; memcpy(&bits, &x, sizeof bits);
    snprintf(buf, cap, "f64:%016llx", (unsigned long long)bits);
}

/* smallest N@k >= x with at most 7 significant digits, verified exactly */
static void bound_scaled(double x, uint64_t *N, unsigned *k)
{
    if (!(x > 0.0)) { *N = 0; *k = 0; return; }
    int kk = (int)ceil(6.0 - log10(x));
    if (kk < 0) kk = 0;
    if (kk > AT1_SCALED_K_MAX) kk = AT1_SCALED_K_MAX;
    double y = ceil(x * pow(10.0, kk)) + 1.0;
    uint64_t n = (uint64_t)y;
    bq ex, sc; bn nb; bq_init(&ex); bq_init(&sc); bn_init(&nb);
    bq_from_double(&ex, x);
    for (;;) {                                 /* exact check N / 10^k >= x */
        bn_set_u64(&nb, n); bq_from_scaled(&sc, &nb, (unsigned)kk);
        if (bq_cmp(&sc, &ex) >= 0) break;
        n++;
    }
    bq_free(&ex); bq_free(&sc); bn_free(&nb);
    while (kk > 0 && n % 10 == 0) { n /= 10; kk--; }
    *N = n; *k = (unsigned)kk;
}

void at1_bound_format(double x, char *buf, size_t cap)
{
    uint64_t n; unsigned k;
    bound_scaled(x, &n, &k);
    snprintf(buf, cap, "%llu@%u", (unsigned long long)n, k);
}

at1_label_status at1_status_rule(double p, double bound, const at1_scaled *tol_zero)
{
    uint64_t n; unsigned k;
    bound_scaled(bound, &n, &k);
    bq P, B, T, s; bn nb; bq_init(&P); bq_init(&B); bq_init(&T); bq_init(&s); bn_init(&nb);
    bq_from_double(&P, p);
    bn_set_u64(&nb, n); bq_from_scaled(&B, &nb, k);
    bq_from_scaled(&T, &tol_zero->n, tol_zero->k);
    at1_label_status st;
    bq_add(&s, &P, &B);
    if (bq_cmp(&s, &T) <= 0) st = AT1_LABEL_UNDEFINED;
    else { bq_sub(&s, &P, &B); st = bq_cmp(&s, &T) > 0 ? AT1_LABEL_DEFINED : AT1_LABEL_INDETERMINATE; }
    bq_free(&P); bq_free(&B); bq_free(&T); bq_free(&s); bn_free(&nb);
    return st;
}

static int rat_i64(const bq *q, int64_t *n, int64_t *d)
{
    return bn_fits_i64(&q->num, n) && bn_fits_i64(&q->den, d);
}

/* exp(-2 pi i E k tau) with the turn count reduced exactly to [-1/2, 1/2) */
static int clock_phase(const bq *E, int k, const bq *tau, cplx *out)
{
    int64_t en, ed, tn, td;
    if (!rat_i64(E, &en, &ed) || !rat_i64(tau, &tn, &td)) return 0;
    /* in-limit tokens: |en|, ed, tn, td <= 2^20 and k <= 255, so |num| < 2^48, den <= 2^40 */
    int64_t num = en * (int64_t)k * tn, den = ed * td;
    int64_t m = num % den;
    if (m < 0) m += den;
    if (2 * m >= den) m -= den;
    double ang = TWO_PI * ((double)m / (double)den);
    out->re = cos(ang); out->im = -sin(ang);
    return 1;
}

/* pairwise sum of a[lo..hi) (complex), for the POVM sum over labels */
static cplx pair_sum(const cplx *a, int lo, int hi)
{
    if (hi - lo == 1) return a[lo];
    if (hi - lo == 0) { cplx z = { 0, 0 }; return z; }
    int mid = lo + (hi - lo) / 2;
    return c_add(pair_sum(a, lo, mid), pair_sum(a, mid, hi));
}

at1_status at1_compute(const at1_case *c, const at1_kernel *K, int reversed, at1_values *out)
{
    memset(out, 0, sizeof *out);
    const int N = c->clock_dim, D = 2 * N, M = c->label_count, r = c->ref_index;
    const double w = bq_to_double(&c->weight);
    const double inv_sqrt_n = 1.0 / sqrt((double)N);
    out->kernel_dim = K->kernel_dim;
    out->M = M;
    out->trivial = K->kernel_dim == 0 || K->psi_zero;

    /* clock states */
    cplx *tk = at1_xmalloc((size_t)M * (size_t)N * sizeof(cplx));
    for (int k = 0; k < M; k++) for (int j = 0; j < N; j++) {
        cplx ph;
        if (!clock_phase(&c->energies[j], k, &c->tau, &ph)) { free(tk); return AT1_ERR_INTERNAL; }
        ph.re *= inv_sqrt_n; ph.im *= inv_sqrt_n;
        tk[k * N + j] = ph;
    }

    /* povm_residual (independent of Psi) */
    {
        cplx *terms = at1_xmalloc((size_t)M * sizeof(cplx));
        double fro2 = 0.0;
        for (int j = 0; j < N; j++) for (int l = 0; l < N; l++) {
            for (int k = 0; k < M; k++) {
                cplx t = c_mul(tk[k * N + j], c_conj(tk[k * N + l]));
                terms[k].re = w * t.re; terms[k].im = w * t.im;
            }
            cplx s = pair_sum(terms, 0, M);
            if (j == l) s.re -= 1.0;
            fro2 += c_abs2(s);
        }
        free(terms);
        out->povm.value = sqrt(fro2);
        int lg = 0; while ((1 << lg) < M) lg++;
        out->povm.bound = 2.0 * (lg + 8) * AT1_EPS * (M * w + 1.0) + 4.0 * AT1_EPS * out->povm.value;
    }

    if (out->trivial) {
        out->constraint.undefined = 1;
        for (int k = 0; k < M; k++) {
            out->status[k] = AT1_LABEL_UNDEFINED;
            out->clock[k].undefined = 1;
            for (int s = 0; s < 6; s++) out->pauli[k][s].undefined = 1;
        }
        free(tk);
        return AT1_OK;
    }

    /* Psi = P_0 (|t_r> (x) psi_0) */
    cplx *Psi = at1_xmalloc((size_t)D * sizeof(cplx));
    /* Psi = P_0 (|t_r> (x) psi_0) = sum_j <E_j|t_r> P_0 (|E_j> (x) psi_0) (linearity): entry i is the
     * clock amplitude of its level times the exact Y_i rounded once. No sum, so no cancellation:
     * every entry has relative error at most about 8 eps (Y_i, cos and sin, 1/sqrt(N), one complex
     * product); delta = 16 eps bounds ||Psi_c - Psi|| / ||Psi|| with a safety factor 2. */
    double norm2 = 0.0;
    for (int i = 0; i < D; i++) {
        if (K->owner[i] < 0) { Psi[i].re = 0.0; Psi[i].im = 0.0; continue; }
        cplx yi = { K->y_re[i], K->y_im[i] };
        Psi[i] = c_mul(tk[r * N + K->owner[i]], yi);
        norm2 += c_abs2(Psi[i]);
    }
    const double delta = 16.0 * AT1_EPS;

    /* constraint_residual = || H_total Psi_hat || */
    {
        double inv = 1.0 / sqrt(norm2), res2 = 0.0;
        for (int i = 0; i < D; i++) {
            cplx acc = { 0, 0 };
            for (int l = 0; l < D; l++) {
                cplx h = { K->h_re[i * D + l], K->h_im[i * D + l] };
                if (h.re == 0.0 && h.im == 0.0) continue;
                cplx ph = { Psi[l].re * inv, Psi[l].im * inv };
                acc = c_add(acc, c_mul(h, ph));
            }
            res2 += c_abs2(acc);
        }
        out->constraint.value = sqrt(res2);
        out->constraint.bound = 2.0 * (K->hmax * delta + 8.0 * AT1_EPS * (K->hmax + 1.0)) + 4.0 * AT1_EPS * out->constraint.value;
    }

    /* per label: phi_k, p(k), status, Pauli outcomes */
    static const cplx proj[6][2][2] = {
        { { { 0.5, 0 }, { 0.5, 0 } }, { { 0.5, 0 }, { 0.5, 0 } } },      /* X+ */
        { { { 0.5, 0 }, { -0.5, 0 } }, { { -0.5, 0 }, { 0.5, 0 } } },    /* X- */
        { { { 0.5, 0 }, { 0, -0.5 } }, { { 0, 0.5 }, { 0.5, 0 } } },     /* Y+ */
        { { { 0.5, 0 }, { 0, 0.5 } }, { { 0, -0.5 }, { 0.5, 0 } } },     /* Y- */
        { { { 1, 0 }, { 0, 0 } }, { { 0, 0 }, { 0, 0 } } },              /* Z+ */
        { { { 0, 0 }, { 0, 0 } }, { { 0, 0 }, { 1, 0 } } }               /* Z- */
    };
    const double L = K->levels_in_psi > 0 ? K->levels_in_psi : 1;
    const double cc = L + 12.0;
    const double eta = sqrt(L / N) * (delta + cc * AT1_EPS);     /* |Delta phi_k| / ||Psi|| */
    for (int step = 0; step < M; step++) {
        int k = reversed ? M - 1 - step : step;
        cplx phi[2];
        for (int s = 0; s < 2; s++) {
            cplx acc = { 0, 0 };
            for (int j = 0; j < N; j++) {
                cplx v = Psi[2 * j + s];
                if (v.re == 0.0 && v.im == 0.0) continue;
                acc = c_add(acc, c_mul(c_conj(tk[k * N + j]), v));
            }
            phi[s] = acc;
        }
        double n2 = c_abs2(phi[0]) + c_abs2(phi[1]);
        double p = w * n2 / norm2;
        double pp = p > 0.0 ? p : 0.0;
        out->clock[k].value = p;
        out->clock[k].bound = 2.0 * (2.0 * sqrt(w * pp) * eta + (2.0 * delta + (2.0 * L + 12.0) * AT1_EPS) * pp) + 4.0 * w * eta * eta;
        out->status[k] = at1_status_rule(p, out->clock[k].bound, &c->tol_zero);
        if (out->status[k] != AT1_LABEL_DEFINED) {
            for (int s = 0; s < 6; s++) out->pauli[k][s].undefined = 1;
            continue;
        }
        cplx rho[2][2];
        for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++) {
            cplx t = c_mul(phi[a], c_conj(phi[b]));
            rho[a][b].re = t.re / n2; rho[a][b].im = t.im / n2;
        }
        double bP = 2.0 * (2.0 * eta * sqrt(w / pp) + 16.0 * AT1_EPS);
        for (int s = 0; s < 6; s++) {
            cplx tr = { 0, 0 };
            for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++) tr = c_add(tr, c_mul(rho[a][b], proj[s][b][a]));
            out->pauli[k][s].value = tr.re;
            out->pauli[k][s].bound = bP;
        }
    }
    free(tk); free(Psi);
    return AT1_OK;
}
