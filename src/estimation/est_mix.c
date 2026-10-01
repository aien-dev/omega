#include "est_mix.h"

#include <math.h>

#define LOG_2PI 1.8378770664093453

est_status est_mix_check(const est_mix *mx)
{
    if (!mx) return EST_ERR_NULL;
    if (mx->k < 1 || mx->k > EST_MIX_MAX) return EST_ERR_DIM;
    double s = 0;
    for (uint32_t j = 0; j < mx->k; j++) {
        if (!isfinite(mx->w[j]) || !isfinite(mx->v[j])) return EST_ERR_NONFINITE;
        if (mx->w[j] < 0.0) return EST_ERR_NOT_PSD;
        if (!(mx->v[j] > 0.0)) return EST_ERR_NOT_PD;
        s += mx->w[j];
    }
    return fabs(s - 1.0) <= 1e-12 ? EST_OK : EST_ERR_ENCODING;
}

double est_mix_total_var(const est_mix *mx)
{
    double s = 0;
    for (uint32_t j = 0; j < mx->k; j++) s += mx->w[j] * mx->v[j];
    return s;
}

/* Visit every count vector a[0..k-1] with sum h and positive probability:
 * log weight = log multinomial + sum a_j log w_j, variance = sum a_j v_j. */
typedef struct { double lw, var; } term;
typedef void (*term_fn)(const term *t, void *ctx);

static void each_term(const est_mix *mx, uint32_t h, term_fn fn, void *ctx)
{
    uint32_t a[EST_MIX_MAX] = { 0 };
    uint32_t k = mx->k;
    double lh = lgamma((double)h + 1.0);
    /* a[0..k-2] free, a[k-1] = remainder */
    for (;;) {
        uint32_t used = 0;
        for (uint32_t j = 0; j + 1 < k; j++) used += a[j];
        if (used <= h) {
            a[k - 1] = h - used;
            term t = { lh, 0.0 };
            int ok = 1;
            for (uint32_t j = 0; j < k; j++) {
                if (a[j] == 0) continue;
                if (mx->w[j] <= 0.0) { ok = 0; break; }
                t.lw += -lgamma((double)a[j] + 1.0) + (double)a[j] * log(mx->w[j]);
                t.var += (double)a[j] * mx->v[j];
            }
            if (ok) fn(&t, ctx);
        }
        /* next count vector over the k-1 free entries */
        if (k == 1) return;
        uint32_t j = 0;
        while (j + 1 < k) {
            a[j]++;
            uint32_t u = 0;
            for (uint32_t i = 0; i + 1 < k; i++) u += a[i];
            if (u <= h) break;
            a[j] = 0; j++;
        }
        if (j + 1 >= k) return;
    }
}

typedef struct { double t, acc; } cdf_ctx;
static void cdf_term(const term *t, void *c)
{
    cdf_ctx *x = c;
    x->acc += exp(t->lw) * erf(x->t / sqrt(2.0 * t->var));
}

est_status est_mix_cdf_abs(const est_mix *mx, uint32_t h, double t, double *out)
{
    est_status st = est_mix_check(mx);
    if (st != EST_OK) return st;
    if (!out) return EST_ERR_NULL;
    if (h < 1 || h > EST_MIX_MAX_H) return EST_ERR_TIME;
    if (!isfinite(t)) return EST_ERR_NONFINITE;
    if (t <= 0.0) { *out = 0.0; return EST_OK; }
    cdf_ctx c = { t, 0.0 };
    each_term(mx, h, cdf_term, &c);
    *out = c.acc > 1.0 ? 1.0 : c.acc;
    return EST_OK;
}

est_status est_mix_quantile_abs(const est_mix *mx, uint32_t h, double p, double *out)
{
    est_status st = est_mix_check(mx);
    if (st != EST_OK) return st;
    if (!out) return EST_ERR_NULL;
    if (h < 1 || h > EST_MIX_MAX_H) return EST_ERR_TIME;
    if (!(p > 0.0 && p < 1.0)) return EST_ERR_DIM;
    double vmax = 0;
    for (uint32_t j = 0; j < mx->k; j++) if (mx->v[j] > vmax) vmax = mx->v[j];
    double lo = 0.0, hi = 64.0 * sqrt((double)h * vmax);
    for (int i = 0; i < 200; i++) {
        double mid = 0.5 * (lo + hi), c = 0.0;
        est_mix_cdf_abs(mx, h, mid, &c);
        if (c < p) lo = mid; else hi = mid;
    }
    *out = hi;
    return EST_OK;
}

typedef struct { double e, mx, acc; int pass; } lp_ctx;
static void lp_term(const term *t, void *c)
{
    lp_ctx *x = c;
    double l = t->lw - 0.5 * (LOG_2PI + log(t->var) + x->e * x->e / t->var);
    if (x->pass == 0) { if (l > x->mx) x->mx = l; }
    else x->acc += exp(l - x->mx);
}

est_status est_mix_logpdf(const est_mix *mx, uint32_t h, double e, double *out)
{
    est_status st = est_mix_check(mx);
    if (st != EST_OK) return st;
    if (!out) return EST_ERR_NULL;
    if (h < 1 || h > EST_MIX_MAX_H) return EST_ERR_TIME;
    if (!isfinite(e)) return EST_ERR_NONFINITE;
    lp_ctx c = { e, -INFINITY, 0.0, 0 };
    each_term(mx, h, lp_term, &c);
    c.pass = 1;
    each_term(mx, h, lp_term, &c);
    *out = c.mx + log(c.acc);
    return EST_OK;
}

est_status est_mix_fit_em(const double *e, size_t n, uint32_t k, double floor_var,
                          uint32_t iters, est_mix *out, double *loglik)
{
    static const double start[EST_MIX_MAX] = { 0.01, 0.3, 3.0 };
    if (!e || !out) return EST_ERR_NULL;
    if (k < 1 || k > EST_MIX_MAX || n < 2) return EST_ERR_DIM;
    if (!isfinite(floor_var) || !(floor_var > 0.0)) return EST_ERR_NOT_PD;
    double v0 = 0;
    for (size_t i = 0; i < n; i++) {
        if (!isfinite(e[i])) return EST_ERR_NONFINITE;
        v0 += e[i] * e[i];
    }
    v0 /= (double)n;
    est_mix m = { k, { 0 }, { 0 } };
    for (uint32_t j = 0; j < k; j++) {
        m.w[j] = 1.0 / (double)k;
        m.v[j] = v0 * start[j];
        if (m.v[j] < floor_var) m.v[j] = floor_var;
    }
    for (uint32_t it = 0; it < iters; it++) {
        double sw[EST_MIX_MAX] = { 0 }, sv[EST_MIX_MAX] = { 0 };
        for (size_t i = 0; i < n; i++) {
            double l[EST_MIX_MAX], mxl = -INFINITY, tot = 0;
            for (uint32_t j = 0; j < k; j++) {
                l[j] = m.w[j] > 0 ? log(m.w[j]) - 0.5 * (log(m.v[j]) + e[i] * e[i] / m.v[j]) : -INFINITY;
                if (l[j] > mxl) mxl = l[j];
            }
            for (uint32_t j = 0; j < k; j++) { l[j] = exp(l[j] - mxl); tot += l[j]; }
            for (uint32_t j = 0; j < k; j++) { double g = l[j] / tot; sw[j] += g; sv[j] += g * e[i] * e[i]; }
        }
        double ws = 0;
        for (uint32_t j = 0; j < k; j++) {
            m.w[j] = sw[j] / (double)n;
            if (sw[j] > 1e-300) m.v[j] = sv[j] / sw[j];
            if (m.v[j] < floor_var) m.v[j] = floor_var;
            ws += m.w[j];
        }
        for (uint32_t j = 0; j < k; j++) m.w[j] /= ws;
    }
    est_status st = est_mix_check(&m);
    if (st != EST_OK) return st;
    if (loglik) {
        double ll = 0;
        for (size_t i = 0; i < n; i++) { double l; est_mix_logpdf(&m, 1, e[i], &l); ll += l; }
        *loglik = ll;
    }
    *out = m;
    return EST_OK;
}
