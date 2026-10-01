/* ESTIMATION v4 predictive families. Model and formulas: est_v4.h. */
#include "est_v4.h"
#include "sha256.h"

#include <math.h>
#include <string.h>

#define D EST4_MAX_D

static void mat_mul(uint32_t d, const double *a, const double *b, double *o)
{
    double t[D * D];
    for (uint32_t i = 0; i < d; i++)
        for (uint32_t j = 0; j < d; j++) {
            double s = 0.0;
            for (uint32_t k = 0; k < d; k++) s += a[i * d + k] * b[k * d + j];
            t[i * d + j] = s;
        }
    memcpy(o, t, sizeof(double) * d * d);
}

/* o = a b a' */
static void sandwich(uint32_t d, const double *a, const double *b, double *o)
{
    double t[D * D], at[D * D];
    for (uint32_t i = 0; i < d; i++)
        for (uint32_t j = 0; j < d; j++) at[j * d + i] = a[i * d + j];
    mat_mul(d, a, b, t);
    mat_mul(d, t, at, o);
}

static int fin(double v) { return isfinite(v); }

est_status est4_params_check(const est4_params *p)
{
    if (!p) return EST_ERR_NULL;
    if (p->family != EST4_G1_LAG && p->family != EST4_G2_AR && p->family != EST4_G3_TWO) return EST_ERR_KIND;
    if (!fin(p->dyn) || !fin(p->q) || !fin(p->lam) || !fin(p->phi) || !fin(p->nu) || !fin(p->c) ||
        !fin(p->nu_h) || !fin(p->c_h) || !fin(p->quantum))
        return EST_ERR_NONFINITE;
    if (p->family == EST4_G2_AR) { if (!(p->dyn >= 0.0 && p->dyn < 1.0)) return EST_ERR_DIM; }
    else if (!(p->dyn > 0.0)) return EST_ERR_DIM;
    if (!(p->q > 0.0) || !(p->lam >= 0.0 && p->lam < 1.0) || !(p->phi > 0.0 && p->phi <= 1.0) ||
        !(p->nu > 0.0) || !(p->c > 0.0) || !(p->nu_h > 0.0) || !(p->c_h > 0.0) ||
        !(p->quantum > 0.0))
        return EST_ERR_DIM;
    return EST_OK;
}

static size_t put_u32(uint8_t *b, size_t o, uint32_t v)
{
    for (int i = 0; i < 4; i++) b[o + (size_t)i] = (uint8_t)(v >> (8 * i));
    return o + 4u;
}
static size_t put_f64(uint8_t *b, size_t o, double d)
{
    if (d == 0.0) d = 0.0;
    uint64_t v;
    memcpy(&v, &d, sizeof v);
    for (int i = 0; i < 8; i++) b[o + (size_t)i] = (uint8_t)(v >> (8 * i));
    return o + 8u;
}

/* Encoding: u32 family, then f64 dyn q lam nu c phi nu_h c_h quantum, then f64
 * EST4_GL_LAMBDA, EST4_TAU_RATIO, EST4_G3_W (the fixed constants are bound). */
est_status est4_params_digest(const est4_params *p, est_digest *out)
{
    if (!p || !out) return EST_ERR_NULL;
    est_status st = est4_params_check(p);
    if (st != EST_OK) return st;
    uint8_t enc[4 + 12 * 8];
    size_t o = put_u32(enc, 0, (uint32_t)p->family);
    const double v[12] = { p->dyn, p->q, p->lam, p->nu, p->c, p->phi, p->nu_h, p->c_h, p->quantum,
                           EST4_GL_LAMBDA, EST4_TAU_RATIO, EST4_G3_W };
    for (int i = 0; i < 12; i++) o = put_f64(enc, o, v[i]);
    const uint8_t zero = 0;
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)EST_DOMAIN_V4, strlen(EST_DOMAIN_V4));
    sha256_update(&c, &zero, 1);
    sha256_update(&c, enc, o);
    sha256_final(&c, out->b);
    return EST_OK;
}

est_status est4_init(const est4_params *p, est4_state *s)
{
    if (!p || !s) return EST_ERR_NULL;
    est_status st = est4_params_check(p);
    if (st != EST_OK) return st;
    memset(s, 0, sizeof *s);
    s->p = *p;
    s->r = p->quantum * p->quantum / 12.0;
    s->g = 1.0;
    s->gl = 1.0;
    if (p->family == EST4_G1_LAG) {
        double a = exp(-1.0 / p->dyn);
        s->d = 2;
        s->A[0] = a; s->A[1] = 1.0 - a; s->A[3] = 1.0;
        s->Q[3] = p->q;
        s->H[0] = 1.0;
    } else if (p->family == EST4_G2_AR) {
        double r = p->dyn;
        s->d = 2;
        s->A[0] = 1.0; s->A[1] = r; s->A[3] = r;
        s->Q[0] = s->Q[1] = s->Q[2] = s->Q[3] = p->q;
        s->H[0] = 1.0;
    } else {
        double af = exp(-1.0 / p->dyn), as = exp(-1.0 / (p->dyn * EST4_TAU_RATIO)), w = EST4_G3_W;
        s->d = 3;
        s->A[0] = af; s->A[2] = (1.0 - af) * w;
        s->A[4] = as; s->A[5] = (1.0 - as) * (1.0 - w);
        s->A[8] = 1.0;
        s->Q[8] = p->q;
        s->H[0] = 1.0; s->H[1] = 1.0;
    }
    return EST_OK;
}

/* First valid observation: equilibrium state at y0 with r on every observed
 * part and r + q on the driving level. */
static void first_obs(est4_state *s, double y0)
{
    memset(s->x, 0, sizeof s->x);
    memset(s->P, 0, sizeof s->P);
    double q = s->p.q, r = s->r;
    if (s->p.family == EST4_G1_LAG) {
        s->x[0] = y0; s->x[1] = y0; s->P[0] = r; s->P[3] = r + q;
    } else if (s->p.family == EST4_G2_AR) {
        s->x[0] = y0; s->x[1] = 0.0; s->P[0] = r; s->P[3] = q;
    } else {
        s->x[0] = EST4_G3_W * y0; s->x[1] = (1.0 - EST4_G3_W) * y0; s->x[2] = y0;
        s->P[0] = r; s->P[4] = r; s->P[8] = r + q;
    }
}

/* mean and variance of H x after h steps from (x, P) */
static void propagate(const est4_state *s, uint32_t h, double *m, double *v)
{
    uint32_t d = s->d;
    double x[D], P[D * D];
    memcpy(x, s->x, sizeof x);
    memcpy(P, s->P, sizeof P);
    for (uint32_t k = 0; k < h; k++) {
        double xn[D] = { 0 };
        for (uint32_t i = 0; i < d; i++) {
            xn[i] = 0.0;
            for (uint32_t j = 0; j < d; j++) xn[i] += s->A[i * d + j] * x[j];
        }
        memcpy(x, xn, sizeof x);
        sandwich(d, s->A, P, P);
        for (uint32_t i = 0; i < d * d; i++) P[i] += s->Q[i];
    }
    double mm = 0.0, vv = 0.0;
    for (uint32_t i = 0; i < d; i++) {
        mm += s->H[i] * x[i];
        for (uint32_t j = 0; j < d; j++) vv += s->H[i] * P[i * d + j] * s->H[j];
    }
    *m = mm;
    *v = vv + s->r;
}

double est4_gbar(double g, double gl, double phi, uint32_t h)
{
    double f = 1.0, gb = 0.0;
    for (uint32_t j = 0; j < h; j++) { gb += gl + (g - gl) * f; f *= phi; }
    return h ? gb / (double)h : NAN;
}

est_status est4_moments(const est4_state *s, uint32_t h, double *loc, double *V)
{
    if (!s || !loc || !V) return EST_ERR_NULL;
    if (h < 1u || h > EST_PRED_MAX_H) return EST_ERR_TIME;
    if (!s->has_anchor) return EST_ERR_STALE;
    propagate(s, h, loc, V);
    return isfinite(*loc) && *V > 0.0 && isfinite(*V) ? EST_OK : EST_ERR_NONFINITE;
}

est_status est4_predict(const est4_state *s, uint32_t h, est4_pred *out)
{
    if (!s || !out) return EST_ERR_NULL;
    if (h < 1u || h > EST_PRED_MAX_H) return EST_ERR_TIME;
    if (!s->has_anchor) return EST_ERR_STALE;
    double mh, Vh;
    propagate(s, h, &mh, &Vh);
    if (!(Vh > 0.0) || !isfinite(Vh)) return EST_ERR_NONFINITE;
    out->h = h;
    out->loc = mh;
    if (h == 1u) {
        out->nu = s->p.nu;
        out->scale = s->p.c * sqrt(s->g * Vh);
    } else {
        double gb = est4_gbar(s->g, s->gl, s->p.phi, h);
        out->nu = s->p.nu_h;
        out->scale = s->p.c_h * sqrt(gb * Vh);
    }
    if (!isfinite(out->loc) || !(out->scale > 0.0) || !isfinite(out->scale)) return EST_ERR_NONFINITE;
    return EST_OK;
}

/* P(X > x) for the centred law, x >= 0 */
static double sf_pos(double x, double scale, double nu) { return est_t_sf(x / scale, nu); }

est_status est4_dpred(const est4_state *s, const est4_pred *pr, est_dpred *dp)
{
    if (!s || !pr || !dp) return EST_ERR_NULL;
    if (!s->has_anchor) return EST_ERR_STALE;
    uint64_t hz = (uint64_t)s->gap + pr->h;
    if (pr->h < 1u || hz > EST_PRED_MAX_H) return EST_ERR_TIME;
    if (!isfinite(pr->loc) || !(pr->scale > 0.0) || !isfinite(pr->scale) || !(pr->nu > 0.0))
        return EST_ERR_NONFINITE;
    memset(dp, 0, sizeof *dp);
    est_status st = est4_params_digest(&s->p, &dp->assumption);
    if (st != EST_OK) return st;
    dp->family = (est_family)0;
    dp->horizon = (uint32_t)hz;
    dp->generation = s->generation;
    dp->anchor = s->anchor;
    dp->quantum = s->p.quantum;
    const double q = s->p.quantum, mu = pr->loc - s->anchor;
    enum { NE = EST_PRED_N - 1 };
    double S[NE], x[NE];
    for (int e = 0; e < NE; e++) {
        x[e] = ((double)(e - EST_PRED_K) + 0.5) * q - mu;
        S[e] = sf_pos(fabs(x[e]), pr->scale, pr->nu);
        if (!(S[e] >= 0.0 && S[e] <= 1.0)) return EST_ERR_NONFINITE;
    }
    double *out = dp->pmf;
    out[0] = x[0] <= 0.0 ? S[0] : 1.0 - S[0];
    out[EST_PRED_N - 1] = x[NE - 1] >= 0.0 ? S[NE - 1] : 1.0 - S[NE - 1];
    for (int i = 1; i < EST_PRED_N - 1; i++) {
        double lo = x[i - 1], hi = x[i], m;
        if (lo >= 0.0) m = S[i - 1] - S[i];
        else if (hi <= 0.0) m = S[i] - S[i - 1];
        else m = 1.0 - S[i - 1] - S[i];
        out[i] = m > 0.0 ? m : 0.0;
    }
    double sum = 0.0;
    for (int i = 0; i < EST_PRED_N; i++) sum += out[i];
    if (!(sum > 0.0) || !isfinite(sum)) return EST_ERR_NONFINITE;
    for (int i = 0; i < EST_PRED_N; i++) out[i] /= sum;
    est_pmf_floor(out);
    return EST_OK;
}

/* survival of the centred standard law at x, x may be +-inf */
static double sf_any(double x, double nu)
{
    if (x == INFINITY) return 0.0;
    if (x == -INFINITY) return 1.0;
    return x >= 0.0 ? est_t_sf(x, nu) : 1.0 - est_t_sf(-x, nu);
}

double est4_fast_logp(const est4_pred *pr, double quantum, double anchor, double y, double *F_lo, double *F_hi)
{
    double kd = nearbyint((y - anchor) / quantum);
    if (kd < -(double)EST_PRED_K) kd = -(double)EST_PRED_K;
    if (kd > (double)EST_PRED_K) kd = (double)EST_PRED_K;
    int k = (int)kd;
    const double mu = pr->loc - anchor;   /* bin edges exactly as est4_dpred */
    double a = k == -EST_PRED_K ? -INFINITY : ((kd - 0.5) * quantum - mu) / pr->scale;
    double b = k == EST_PRED_K ? INFINITY : ((kd + 0.5) * quantum - mu) / pr->scale;
    double lo, hi, p;
    /* difference formed on the small-tail side, as est4_dpred */
    if (a >= 0.0) { double sa = sf_any(a, pr->nu), sb = sf_any(b, pr->nu); p = sa - sb; lo = 1.0 - sa; hi = 1.0 - sb; }
    else if (b <= 0.0) { double ca = sf_any(-a, pr->nu), cb = sf_any(-b, pr->nu); p = cb - ca; lo = ca; hi = cb; }
    else { lo = sf_any(-a, pr->nu); double sb = sf_any(b, pr->nu); p = 1.0 - lo - sb; hi = 1.0 - sb; }
    if (p < 0.0) p = 0.0;
    /* the est_pmf_floor mix */
    const double f = EST_PRED_FLOOR, u = EST_PRED_FLOOR / (double)EST_PRED_N;
    double idx = (double)(k + EST_PRED_K);
    p = (1.0 - f) * p + u;
    if (F_lo) *F_lo = (1.0 - f) * lo + u * idx;
    if (F_hi) *F_hi = (1.0 - f) * hi + u * (idx + 1.0);
    return log(p > 1e-300 ? p : 1e-300);
}

static void time_update(est4_state *s)
{
    uint32_t d = s->d;
    double xn[D] = { 0 };
    for (uint32_t i = 0; i < d; i++) {
        xn[i] = 0.0;
        for (uint32_t j = 0; j < d; j++) xn[i] += s->A[i * d + j] * s->x[j];
    }
    memcpy(s->x, xn, sizeof xn);
    sandwich(d, s->A, s->P, s->P);
    for (uint32_t i = 0; i < d * d; i++) s->P[i] += s->Q[i];
}

est_status est4_update(est4_state *s, int present, double y)
{
    if (!s) return EST_ERR_NULL;
    int ok = present && isfinite(y);
    s->generation++;
    if (!s->has_anchor) {
        if (!ok) return EST_OK;
        first_obs(s, y);
        s->has_anchor = 1;
        s->anchor = y;
        s->gap = 0;
        return EST_OK;
    }
    const double lam = s->p.lam;
    if (!ok) {
        time_update(s);
        s->gl = EST4_GL_LAMBDA * s->gl + (1.0 - EST4_GL_LAMBDA) * s->g;
        s->gap++;
        return EST_OK;
    }
    uint32_t d = s->d;
    time_update(s);
    double m = 0.0, PH[D], S1 = s->r;
    for (uint32_t i = 0; i < d; i++) {
        m += s->H[i] * s->x[i];
        PH[i] = 0.0;
        for (uint32_t j = 0; j < d; j++) PH[i] += s->P[i * d + j] * s->H[j];
    }
    for (uint32_t i = 0; i < d; i++) S1 += s->H[i] * PH[i];
    if (!(S1 > 0.0) || !isfinite(S1)) return EST_ERR_NONFINITE;
    double e = y - m;
    for (uint32_t i = 0; i < d; i++) s->x[i] += PH[i] / S1 * e;
    for (uint32_t i = 0; i < d; i++)
        for (uint32_t j = 0; j < d; j++) s->P[i * d + j] -= PH[i] * PH[j] / S1;
    /* keep P exactly symmetric */
    for (uint32_t i = 0; i < d; i++)
        for (uint32_t j = i + 1; j < d; j++) {
            double v = 0.5 * (s->P[i * d + j] + s->P[j * d + i]);
            s->P[i * d + j] = v; s->P[j * d + i] = v;
        }
    double e2 = e * e > s->r ? e * e : s->r;
    s->g = lam * s->g + (1.0 - lam) * e2 / S1;
    s->gl = EST4_GL_LAMBDA * s->gl + (1.0 - EST4_GL_LAMBDA) * s->g;
    s->anchor = y;
    s->gap = 0;
    for (uint32_t i = 0; i < d; i++) if (!isfinite(s->x[i])) return EST_ERR_NONFINITE;
    return EST_OK;
}
