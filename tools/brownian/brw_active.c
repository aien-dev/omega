/* BRW-ACT-DEV0 candidate library. See brw_active.h. */
#include "brownian/brw_active.h"

#include <math.h>
#include <stdlib.h>

const int brw_tau[BRW_NT] = {1, 2, 4, 8, 16, 32};

#define TWO_PI 6.283185307179586476925286766559
#define SQRT1_2 0.70710678118654752440084436210485
#define INF_ (1.0 / 0.0)

const char *brw_verdict_name(int v)
{
    switch (v) {
    case BRW_V_INADEQUATE: return "INADEQUATE";
    case BRW_V_M0: return "M0";
    case BRW_V_M1: return "M1";
    case BRW_V_M2: return "M2";
    case BRW_V_UNDETERMINED: return "UNDETERMINED";
    default: return "?";
    }
}

void brw_grid_init(brw_grid *g)
{
    double D[16], th[16], vm[8];
    for (int i = 0; i < 16; i++) {
        D[i] = 0.001 * pow(10000.0, i / 15.0);     /* 16 log-spaced on [0.001, 10] */
        th[i] = 0.01 * pow(100.0, i / 15.0);       /* 16 log-spaced on [0.01, 1] */
    }
    for (int i = 0; i < 8; i++)
        vm[i] = 0.02 * pow(50.0, i / 7.0);         /* 8 log-spaced on [0.02, 1], both signs below */
    int n = 0;
    for (int i = 0; i < 16; i++) g->h[n++] = (brw_hyp){0, D[i], 0.0, 0.0};
    for (int i = 0; i < 16; i++)
        for (int k = 0; k < 16; k++)
            g->h[n++] = (brw_hyp){1, D[i], (k < 8 ? -vm[7 - k] : vm[k - 8]), 0.0};
    for (int i = 0; i < 16; i++)
        for (int k = 0; k < 16; k++) g->h[n++] = (brw_hyp){2, D[i], 0.0, th[k]};
    int cnt[BRW_NM] = {0, 0, 0};
    for (int i = 0; i < BRW_NH; i++) cnt[g->h[i].model]++;
    for (int m = 0; m < BRW_NM; m++) g->lprior_m[m] = log(1.0 / 3.0) - log((double)cnt[m]);
    for (int t = 0; t < BRW_NT; t++) {
        double tau = brw_tau[t];
        for (int i = 0; i < BRW_NH; i++) {
            const brw_hyp *h = &g->h[i];
            double mu = 0.0, var;
            if (h->model == 2)
                var = (h->D / h->theta) * (1.0 - exp(-2.0 * h->theta * tau)) + 1.0;
            else
                var = 2.0 * h->D * tau + 1.0;
            if (h->model == 1) mu = h->v * tau;
            g->mu[t][i] = mu;
            g->var[t][i] = var;
            g->lnorm[t][i] = -0.5 * log(TWO_PI * var);
        }
    }
}

double brw_lse(const double *a, size_t n)
{
    double m = -INF_;
    for (size_t i = 0; i < n; i++)
        if (a[i] > m) m = a[i];
    if (!(m > -INF_)) return -INF_;
    if (m == INF_) return INF_;
    double s = 0.0;
    for (size_t i = 0; i < n; i++) s += exp(a[i] - m);
    return m + log(s);
}

void brw_state_init(brw_state *s, const brw_grid *g)
{
    s->g = g;
    for (int i = 0; i < BRW_NH; i++) s->logw[i] = g->lprior_m[g->h[i].model];
    s->n_obs = s->n_pred = s->n_out99 = 0;
    s->lik_evals = s->quad_evals = s->cdf_evals = 0;
}

void brw_model_post(const brw_state *s, double p[BRW_NM])
{
    double a[BRW_NM][BRW_NH];
    size_t c[BRW_NM] = {0, 0, 0};
    for (int i = 0; i < BRW_NH; i++) {
        int m = s->g->h[i].model;
        a[m][c[m]++] = s->logw[i];
    }
    for (int m = 0; m < BRW_NM; m++) p[m] = exp(brw_lse(a[m], c[m]));
}

double brw_cdf(brw_state *s, int ti, double y)
{
    double f = 0.0;
    for (int i = 0; i < BRW_NH; i++) {
        double w = exp(s->logw[i]);
        if (w <= 0.0) continue;
        double z = (y - s->g->mu[ti][i]) / sqrt(s->g->var[ti][i]);
        f += w * 0.5 * erfc(-z * SQRT1_2);
    }
    s->cdf_evals += BRW_NH;
    return f;
}

int brw_observe(brw_state *s, int ti, double y)
{
    if (!isfinite(y) || ti < 0 || ti >= BRW_NT) return -1;
    double u = brw_cdf(s, ti, y);
    int out = (u < 0.005 || u > 0.995) ? 1 : 0;
    s->n_pred++;
    s->n_out99 += (unsigned)out;
    for (int i = 0; i < BRW_NH; i++) {
        double d = y - s->g->mu[ti][i];
        s->logw[i] += s->g->lnorm[ti][i] - 0.5 * d * d / s->g->var[ti][i];
    }
    s->lik_evals += BRW_NH;
    double z = brw_lse(s->logw, BRW_NH);
    for (int i = 0; i < BRW_NH; i++) s->logw[i] -= z;
    s->n_obs++;
    return out;
}

unsigned brw_binom_crit(unsigned n)
{
    double *pm = malloc(((size_t)n + 1) * sizeof *pm);
    if (!pm) return n + 1u;
    double l1 = log(0.01), l2 = log(0.99), lg = lgamma((double)n + 1.0);
    for (unsigned k = 0; k <= n; k++)
        pm[k] = exp(lg - lgamma((double)k + 1.0) - lgamma((double)(n - k) + 1.0) + k * l1 + (double)(n - k) * l2);
    /* tail[c] = P(X >= c), summed from the small end; smallest c with tail <= 0.001 */
    double *tail = pm;                       /* in place: tail[c] = pm[c] + tail[c+1] */
    double acc = 0.0;
    unsigned best = n + 1u;
    for (unsigned c = n + 1u; c-- > 0;) {
        acc += pm[c];
        tail[c] = acc;
    }
    for (unsigned c = 0; c <= n; c++)
        if (tail[c] <= 0.001) { best = c; break; }
    free(pm);
    return best;
}

int brw_verdict(const brw_state *s)
{
    if (s->n_out99 > brw_binom_crit(s->n_pred)) return BRW_V_INADEQUATE;
    double p[BRW_NM];
    brw_model_post(s, p);
    for (int m = 0; m < BRW_NM; m++)
        if (p[m] >= BRW_P_SURE) return BRW_V_M0 + m;
    return BRW_V_UNDETERMINED;
}

/* ---- quadrature ---- */

static void range8(size_t n, const double *mu, const double *var, double *lo, double *hi)
{
    double a = INF_, b = -INF_;
    for (size_t i = 0; i < n; i++) {
        double s8 = 8.0 * sqrt(var[i]);
        if (mu[i] - s8 < a) a = mu[i] - s8;
        if (mu[i] + s8 > b) b = mu[i] + s8;
    }
    *lo = a;
    *hi = b;
}

static double plogp(double p) { return p > 0.0 ? -p * log2(p) : 0.0; }

double brw_mix_entropy_bits(const double *w, const double *mu, const double *var, size_t n, uint64_t *quad)
{
    if (n == 0) return 0.0;
    double lo, hi;
    range8(n, mu, var, &lo, &hi);
    double *a = malloc(2 * n * sizeof *a);
    if (!a) return -1.0;
    double *c = a + n;
    for (size_t i = 0; i < n; i++) {
        a[i] = 0.5 / var[i];
        c[i] = w[i] / sqrt(TWO_PI * var[i]);
    }
    double h = (hi - lo) / (BRW_NQ - 1), tot = 0.0;
    for (int q = 0; q < BRW_NQ; q++) {
        double x = lo + h * q, p = 0.0;
        for (size_t i = 0; i < n; i++) {
            double d = x - mu[i], e = a[i] * d * d;
            if (e < 745.0) p += c[i] * exp(-e);
        }
        tot += ((q == 0 || q == BRW_NQ - 1) ? 0.5 : 1.0) * plogp(p);
    }
    free(a);
    if (quad) *quad += (uint64_t)n * BRW_NQ;
    return tot * h;
}

void brw_info_arrays(size_t n, const double *w, const int *grp, const double *mu, const double *var,
                     int ngrp, double *i_group, double *i_hyp, uint64_t *quad)
{
    double pg[8] = {0}, hg[8] = {0}, hh = 0.0, hy = 0.0;
    *i_group = *i_hyp = 0.0;
    if (n == 0 || ngrp < 1 || ngrp > 8) return;
    double *a = malloc(2 * n * sizeof *a);
    if (!a) return;
    double *c = a + n;
    double lo, hi;
    range8(n, mu, var, &lo, &hi);
    double h = (hi - lo) / (BRW_NQ - 1);
    for (size_t i = 0; i < n; i++) {
        pg[grp[i]] += w[i];
        hh += w[i] * 0.5 * log2(TWO_PI * 2.718281828459045235360287471353 * var[i]);   /* H(Y | hyp), exact */
        a[i] = 0.5 / var[i];
        c[i] = w[i] / sqrt(TWO_PI * var[i]);
    }
    for (int q = 0; q < BRW_NQ; q++) {
        double x = lo + h * q, sg[8] = {0}, p = 0.0;
        double tw = (q == 0 || q == BRW_NQ - 1) ? 0.5 : 1.0;
        for (size_t i = 0; i < n; i++) {
            double d = x - mu[i], e = a[i] * d * d;
            if (e < 745.0) sg[grp[i]] += c[i] * exp(-e);
        }
        for (int g = 0; g < ngrp; g++) {
            p += sg[g];
            if (pg[g] > 0.0) hg[g] += tw * plogp(sg[g] / pg[g]);
        }
        hy += tw * plogp(p);
    }
    free(a);
    if (quad) *quad += (uint64_t)n * BRW_NQ;
    hy *= h;
    double cond = 0.0;
    for (int g = 0; g < ngrp; g++) cond += pg[g] * hg[g] * h;
    *i_group = hy - cond;
    *i_hyp = hy - hh;
}

void brw_info(brw_state *s, int ti, double *i_model, double *i_hyp)
{
    double w[BRW_NH], mu[BRW_NH], var[BRW_NH], tot = 0.0;
    int grp[BRW_NH];
    size_t n = 0;
    for (int i = 0; i < BRW_NH; i++) {
        double p = exp(s->logw[i]);
        if (p > BRW_W_ACTIVE) {
            w[n] = p;
            mu[n] = s->g->mu[ti][i];
            var[n] = s->g->var[ti][i];
            grp[n] = s->g->h[i].model;
            tot += p;
            n++;
        }
    }
    for (size_t i = 0; i < n; i++) w[i] /= tot;
    brw_info_arrays(n, w, grp, mu, var, BRW_NM, i_model, i_hyp, &s->quad_evals);
}

int brw_pick(const double *score, const int *ok, int n)
{
    int best = -1;
    for (int i = 0; i < n; i++)
        if (ok[i] && (best < 0 || score[i] > score[best])) best = i;
    return best;
}

int brw_choose(brw_state *s, int remaining, int *phase)
{
    int ok[BRW_NT], any = 0;
    double sc[BRW_NT] = {0};
    for (int t = 0; t < BRW_NT; t++) {
        ok[t] = BRW_COST(t) <= remaining;
        any |= ok[t];
    }
    if (!any) return -1;
    double p[BRW_NM], pmax = 0.0;
    brw_model_post(s, p);
    for (int m = 0; m < BRW_NM; m++)
        if (p[m] > pmax) pmax = p[m];
    int ph = pmax < BRW_P_SURE ? 1 : 2;
    if (phase) *phase = ph;
    for (int t = 0; t < BRW_NT; t++) {
        if (!ok[t]) continue;
        double im, ih;
        brw_info(s, t, &im, &ih);
        sc[t] = (ph == 1 ? im : ih) / (double)BRW_COST(t);
    }
    return brw_pick(sc, ok, BRW_NT);
}

/* ---- PRD2 reduction ---- */

int brw_reduce(const double *w, const double *mu, const double *sd, size_t n, uint32_t index, tyq_pred *p)
{
    size_t *ix = malloc((n ? n : 1) * sizeof *ix);
    unsigned char *used = calloc(n ? n : 1, 1);
    if (!ix || !used) {
        free(ix);
        free(used);
        return TYQ_E_IO;
    }
    size_t m = 0;
    double tot = 0.0;
    for (size_t i = 0; i < n; i++)
        if (w[i] > 0.0) { ix[m++] = i; tot += w[i]; }
    tyq_pred q = {0};
    q.index = index;
    q.family = TYQ_FAM_MIX;
    size_t keep = m <= TYQ_KMAX ? m : TYQ_KMAX - 1;
    for (size_t k = 0; k < keep; k++) {                 /* heaviest first, ties to the lower index */
        size_t bi = (size_t)-1;
        for (size_t j = 0; j < m; j++)
            if (!used[ix[j]] && (bi == (size_t)-1 || w[ix[j]] > w[bi])) bi = ix[j];
        used[bi] = 1;
        q.comp[k].pi = w[bi] / tot;
        q.comp[k].loc = mu[bi];
        q.comp[k].scale = sd[bi];
    }
    q.K = (uint32_t)keep;
    if (m > TYQ_KMAX) {
        double wr = 0.0, mean = 0.0, v = 0.0;
        for (size_t j = 0; j < m; j++)
            if (!used[ix[j]]) { wr += w[ix[j]]; mean += w[ix[j]] * mu[ix[j]]; }
        mean /= wr;
        for (size_t j = 0; j < m; j++) {
            size_t i = ix[j];
            if (!used[i]) v += w[i] * (sd[i] * sd[i] + (mu[i] - mean) * (mu[i] - mean));
        }
        v /= wr;
        q.comp[keep].pi = wr / tot;
        q.comp[keep].loc = mean;
        q.comp[keep].scale = sqrt(v);
        q.K++;
    }
    double sum = 0.0;
    for (uint32_t k = 0; k < q.K; k++) sum += q.comp[k].pi;
    for (uint32_t k = 0; k < q.K; k++) {
        q.comp[k].pi /= sum;
        if (q.comp[k].pi > 1.0) q.comp[k].pi = 1.0;
    }
    free(ix);
    free(used);
    *p = q;
    return ty_prd2_validate(p, NULL);
}

int brw_predict_prd2(const brw_state *s, int ti, uint32_t index, tyq_pred *p)
{
    double w[BRW_NH], sd[BRW_NH];
    for (int i = 0; i < BRW_NH; i++) {
        w[i] = exp(s->logw[i]);
        sd[i] = sqrt(s->g->var[ti][i]);
    }
    return brw_reduce(w, s->g->mu[ti], sd, BRW_NH, index, p);
}
