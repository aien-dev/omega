/* EST-1 independent reference, information form. See est_ref_info.h. */
#include "est_ref_info.h"

#include <math.h>
#include <string.h>

typedef long double ld;
#define D EST_MAX_DIM

/* Gauss-Jordan inverse with partial pivoting. A is n*n row-major (stride n),
 * result in Ai. Returns nonzero if a pivot is (relatively) zero. */
static int inv(const ld *A, ld *Ai, unsigned n)
{
    ld a[D * D], b[D * D];
    unsigned i, j, k;
    ld scale = 0;
    for (i = 0; i < n * n; i++) {
        a[i] = A[i];
        if (fabsl(A[i]) > scale) scale = fabsl(A[i]);
        b[i] = 0;
    }
    if (!(scale > 0) || !isfinite((double)scale)) return 1;
    for (i = 0; i < n; i++) b[i * n + i] = 1;
    for (k = 0; k < n; k++) {
        unsigned p = k;
        ld piv, t;
        for (i = k + 1; i < n; i++)
            if (fabsl(a[i * n + k]) > fabsl(a[p * n + k])) p = i;
        if (fabsl(a[p * n + k]) <= scale * 1e-18L) return 1;
        if (p != k)
            for (j = 0; j < n; j++) {
                t = a[k * n + j]; a[k * n + j] = a[p * n + j]; a[p * n + j] = t;
                t = b[k * n + j]; b[k * n + j] = b[p * n + j]; b[p * n + j] = t;
            }
        piv = a[k * n + k];
        for (j = 0; j < n; j++) { a[k * n + j] /= piv; b[k * n + j] /= piv; }
        for (i = 0; i < n; i++) {
            ld f;
            if (i == k) continue;
            f = a[i * n + k];
            if (f == 0) continue;
            for (j = 0; j < n; j++) {
                a[i * n + j] -= f * a[k * n + j];
                b[i * n + j] -= f * b[k * n + j];
            }
        }
    }
    memcpy(Ai, b, n * n * sizeof(ld));
    return 0;
}

/* Solve A x = v via the explicit Gauss-Jordan inverse. */
static int solve(const ld *A, const ld *v, ld *x, unsigned n)
{
    ld Ai[D * D];
    unsigned i, j;
    if (inv(A, Ai, n)) return 1;
    for (i = 0; i < n; i++) {
        ld s = 0;
        for (j = 0; j < n; j++) s += Ai[i * n + j] * v[j];
        x[i] = s;
    }
    return 0;
}

static void sym(ld *A, unsigned n)
{
    unsigned i, j;
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++) {
            ld s = (A[i * n + j] + A[j * n + i]) / 2;
            A[i * n + j] = A[j * n + i] = s;
        }
}

static int okdim(unsigned n) { return n >= 1 && n <= D; }

static void load(ld *dst, const double *src, unsigned count)
{
    unsigned i;
    for (i = 0; i < count; i++) dst[i] = (ld)src[i];
}

int est_ref_init(est_ref_info *r, unsigned n, const double *x0, const double *P0)
{
    ld P[D * D], x[D];
    unsigned i, j;
    if (!r || !x0 || !P0 || !okdim(n)) return 1;
    load(P, P0, n * n);
    load(x, x0, n);
    sym(P, n);
    r->n = n;
    if (inv(P, r->Y, n)) return 1;
    sym(r->Y, n);
    for (i = 0; i < n; i++) {
        ld s = 0;
        for (j = 0; j < n; j++) s += r->Y[i * n + j] * x[j];
        r->y[i] = s;
    }
    return 0;
}

static int moments(const est_ref_info *r, ld *x, ld *P)
{
    ld yv[D];
    unsigned i, n = r->n;
    if (inv(r->Y, P, n)) return 1;
    sym(P, n);
    for (i = 0; i < n; i++) yv[i] = r->y[i];
    return solve(r->Y, yv, x, n);
}

int est_ref_predict(est_ref_info *r, const double *F, const double *Q)
{
    ld P[D * D], x[D], Fm[D * D], Qm[D * D], FP[D * D], Pn[D * D], xn[D];
    unsigned i, j, k, n;
    if (!r || !F || !Q || !okdim(r->n)) return 1;
    n = r->n;
    if (moments(r, x, P)) return 1;
    load(Fm, F, n * n);
    load(Qm, Q, n * n);
    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++) {
            ld s = 0;
            for (k = 0; k < n; k++) s += Fm[i * n + k] * P[k * n + j];
            FP[i * n + j] = s;
        }
    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++) {
            ld s = 0;
            for (k = 0; k < n; k++) s += FP[i * n + k] * Fm[j * n + k];
            Pn[i * n + j] = s + Qm[i * n + j];
        }
    sym(Pn, n);
    for (i = 0; i < n; i++) {
        ld s = 0;
        for (j = 0; j < n; j++) s += Fm[i * n + j] * x[j];
        xn[i] = s;
    }
    if (inv(Pn, r->Y, n)) return 1;
    sym(r->Y, n);
    for (i = 0; i < n; i++) {
        ld s = 0;
        for (j = 0; j < n; j++) s += r->Y[i * n + j] * xn[j];
        r->y[i] = s;
    }
    return 0;
}

int est_ref_update(est_ref_info *r, unsigned m, const double *H, const double *R,
                   const double *z)
{
    ld Hm[D * D], Rm[D * D], Ri[D * D], zv[D], RiH[D * D], Riz[D];
    unsigned i, j, k, n;
    if (!r || !H || !R || !z || !okdim(r->n) || !okdim(m)) return 1;
    n = r->n;
    load(Hm, H, m * n);
    load(Rm, R, m * m);
    load(zv, z, m);
    sym(Rm, m);
    if (inv(Rm, Ri, m)) return 1;
    for (i = 0; i < m; i++) {
        ld s = 0;
        for (j = 0; j < n; j++) {
            ld t = 0;
            for (k = 0; k < m; k++) t += Ri[i * m + k] * Hm[k * n + j];
            RiH[i * n + j] = t;
        }
        for (k = 0; k < m; k++) s += Ri[i * m + k] * zv[k];
        Riz[i] = s;
    }
    for (i = 0; i < n; i++) {
        ld s = 0;
        for (j = 0; j < n; j++) {
            ld t = 0;
            for (k = 0; k < m; k++) t += Hm[k * n + i] * RiH[k * n + j];
            r->Y[i * n + j] += t;
        }
        for (k = 0; k < m; k++) s += Hm[k * n + i] * Riz[k];
        r->y[i] += s;
    }
    sym(r->Y, n);
    return 0;
}

int est_ref_get(const est_ref_info *r, double *x, double *P)
{
    ld xm[D], Pm[D * D];
    unsigned i;
    if (!r || !okdim(r->n)) return 1;
    if (moments(r, xm, Pm)) return 1;
    if (x) for (i = 0; i < r->n; i++) x[i] = (double)xm[i];
    if (P) for (i = 0; i < r->n * r->n; i++) P[i] = (double)Pm[i];
    return 0;
}

int est_ref_innovation(const est_ref_info *r, unsigned m, const double *H,
                       const double *R, const double *z, double *ymean, double *S,
                       double *nu, double *nis)
{
    ld x[D], P[D * D], Hm[D * D], Rm[D * D], zv[D], HP[D * D], Sm[D * D];
    ld ym[D], nv[D], w[D];
    unsigned i, j, k, n;
    if (!r || !H || !R || !z || !okdim(r->n) || !okdim(m)) return 1;
    n = r->n;
    if (moments(r, x, P)) return 1;
    load(Hm, H, m * n);
    load(Rm, R, m * m);
    load(zv, z, m);
    for (i = 0; i < m; i++)
        for (j = 0; j < n; j++) {
            ld s = 0;
            for (k = 0; k < n; k++) s += Hm[i * n + k] * P[k * n + j];
            HP[i * n + j] = s;
        }
    for (i = 0; i < m; i++)
        for (j = 0; j < m; j++) {
            ld s = 0;
            for (k = 0; k < n; k++) s += HP[i * n + k] * Hm[j * n + k];
            Sm[i * m + j] = s + Rm[i * m + j];
        }
    sym(Sm, m);
    for (i = 0; i < m; i++) {
        ld s = 0;
        for (j = 0; j < n; j++) s += Hm[i * n + j] * x[j];
        ym[i] = s;
        nv[i] = zv[i] - s;
    }
    if (solve(Sm, nv, w, m)) return 1;
    {
        ld q = 0;
        for (i = 0; i < m; i++) q += nv[i] * w[i];
        if (nis) *nis = (double)q;
    }
    for (i = 0; i < m; i++) {
        if (ymean) ymean[i] = (double)ym[i];
        if (nu) nu[i] = (double)nv[i];
    }
    if (S) for (i = 0; i < m * m; i++) S[i] = (double)Sm[i];
    return 0;
}
