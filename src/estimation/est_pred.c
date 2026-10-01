/* ESTIMATION-2 (protocol v3) discrete predictive layer. Contract: est_pred.h.
 * No allocation, no globals, no I/O; libm only (plus snprintf for the verdict
 * reason text). */
#include "est_pred.h"
#include "sha256.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define SQRT1_2 0.70710678118654752440
#define BETACF_MAXIT 20000
#define BETACF_EPS 1e-15
#define BETACF_FPMIN 1e-300

/* ------------------------------------------------------------- numerics */

/* Continued fraction for I_x(a,b) (modified Lentz, Numerical-Recipes form).
 * Returns NaN if it does not converge within BETACF_MAXIT. */
static double betacf(double a, double b, double x)
{
    double qab = a + b, qap = a + 1.0, qam = a - 1.0;
    double c = 1.0, d = 1.0 - qab * x / qap;
    if (fabs(d) < BETACF_FPMIN) d = BETACF_FPMIN;
    d = 1.0 / d;
    double h = d;
    for (int m = 1; m <= BETACF_MAXIT; m++) {
        double m2 = 2.0 * m;
        double aa = m * (b - m) * x / ((qam + m2) * (a + m2));
        d = 1.0 + aa * d;
        if (fabs(d) < BETACF_FPMIN) d = BETACF_FPMIN;
        c = 1.0 + aa / c;
        if (fabs(c) < BETACF_FPMIN) c = BETACF_FPMIN;
        d = 1.0 / d;
        h *= d * c;
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
        d = 1.0 + aa * d;
        if (fabs(d) < BETACF_FPMIN) d = BETACF_FPMIN;
        c = 1.0 + aa / c;
        if (fabs(c) < BETACF_FPMIN) c = BETACF_FPMIN;
        d = 1.0 / d;
        double del = d * c;
        h *= del;
        if (fabs(del - 1.0) <= BETACF_EPS) return h;
    }
    return NAN;
}

/* I_x(a,b) with y = 1 - x supplied exactly by the caller (avoids the
 * cancellation in 1 - x when x is close to 1). */
static double betai_xy(double a, double b, double x, double y)
{
    if (!(a > 0.0) || !(b > 0.0) || !isfinite(a) || !isfinite(b)) return NAN;
    if (!(x >= 0.0 && x <= 1.0) || !(y >= 0.0 && y <= 1.0)) return NAN;
    if (x == 0.0) return 0.0;
    if (y == 0.0) return 1.0;
    double lbt = lgamma(a + b) - lgamma(a) - lgamma(b) + a * log(x) + b * log(y);
    double bt = exp(lbt);
    if (x < (a + 1.0) / (a + b + 2.0)) {
        double cf = betacf(a, b, x);
        return bt * cf / a;
    }
    double cf = betacf(b, a, y);
    return 1.0 - bt * cf / b;
}

double est_betai(double a, double b, double x)
{
    if (!(x >= 0.0 && x <= 1.0)) return NAN;
    return betai_xy(a, b, x, 1.0 - x);
}

/* P(T > t) for t >= 0: 0.5 * I_{nu/(nu+t^2)}(nu/2, 1/2). */
static double t_sf_pos(double t, double nu)
{
    if (isinf(t)) return 0.0;
    double t2 = t * t, den = nu + t2;
    if (!isfinite(den)) return 0.0;
    return 0.5 * betai_xy(0.5 * nu, 0.5, nu / den, t2 / den);
}

double est_t_sf(double t, double nu)
{
    if (isnan(t) || isnan(nu) || !(nu > 0.0)) return NAN;
    if (t >= 0.0) return t_sf_pos(t, nu);
    return 1.0 - t_sf_pos(-t, nu);
}

double est_t_cdf(double t, double nu)
{
    if (isnan(t) || isnan(nu) || !(nu > 0.0)) return NAN;
    return est_t_sf(-t, nu);
}

double est_norm_cdf(double x) { return 0.5 * erfc(-x * SQRT1_2); }

double est_norm_ppf(double p)
{
    if (isnan(p)) return NAN;
    if (p <= 0.0) return -8.0;
    if (p >= 1.0) return 8.0;
    double lo = -8.0, hi = 8.0;
    if (est_norm_cdf(lo) >= p) return -8.0;
    if (est_norm_cdf(hi) <= p) return 8.0;
    for (int i = 0; i < 200; i++) {
        double mid = 0.5 * (lo + hi);
        if (mid <= lo || mid >= hi) break;
        if (est_norm_cdf(mid) < p) lo = mid; else hi = mid;
    }
    return 0.5 * (lo + hi);
}

/* ----------------------------------------------------------- assumption */

static uint32_t fam_nparam(est_family f)
{
    switch (f) {
    case EST_FAM_GAUSS_KF: return 3u;
    case EST_FAM_HUBER_KF: return 4u;
    case EST_FAM_STUDENT_T: return 2u;
    case EST_FAM_SCALE_MIX: return 7u;
    case EST_FAM_ADAPTIVE_T: return 5u;
    }
    return 0u;
}

static int pos_fin(double v) { return isfinite(v) && v > 0.0; }
static int nonneg_fin(double v) { return isfinite(v) && v >= 0.0; }

/* Builds the F4 shape from the parameter vector (k, w0, w1, w2, v0, v1, v2). */
static est_status mix_from(const est_assumption *a, est_mix *mx)
{
    double kd = a->param[0];
    if (!(kd == 1.0 || kd == 2.0 || kd == 3.0)) return EST_ERR_DIM;
    memset(mx, 0, sizeof *mx);
    mx->k = (uint32_t)kd;
    for (uint32_t j = 0; j < EST_MIX_MAX; j++) {
        double w = a->param[1 + j], v = a->param[4 + j];
        if (j >= mx->k) {
            if (w != 0.0 || v != 0.0) return EST_ERR_DIM;
            continue;
        }
        mx->w[j] = w;
        mx->v[j] = v;
    }
    return est_mix_check(mx);
}

/* Status codes: NULL; KIND (unknown family); UNIT (unit outside 1..MAX_-1);
 * NONFINITE (NaN/Inf in quantum, lo, hi or any param); NOT_PD (quantum <= 0,
 * or a variance/scale/nu/c/floor that must be > 0 is not); DIM (nparam is not
 * the family's count, a param beyond nparam is nonzero, F4 k not in {1,2,3}
 * or an unused F4 weight/variance nonzero); ENCODING (lo >= hi, F1/F2
 * q_proc + r == 0, F5 lambda outside [0,1], F4 weights not summing to 1). */
est_status est_assumption_check(const est_assumption *a)
{
    if (!a) return EST_ERR_NULL;
    uint32_t np = fam_nparam(a->family);
    if (np == 0u) return EST_ERR_KIND;
    if ((uint32_t)a->unit < 1u || (uint32_t)a->unit >= (uint32_t)EST_UNIT_MAX_) return EST_ERR_UNIT;
    if (!isfinite(a->quantum) || !isfinite(a->lo) || !isfinite(a->hi)) return EST_ERR_NONFINITE;
    for (uint32_t i = 0; i < EST_PRED_NPARAM; i++)
        if (!isfinite(a->param[i])) return EST_ERR_NONFINITE;
    if (!(a->quantum > 0.0)) return EST_ERR_NOT_PD;
    if (!(a->lo < a->hi)) return EST_ERR_ENCODING;
    if (a->nparam != np) return EST_ERR_DIM;
    for (uint32_t i = np; i < EST_PRED_NPARAM; i++)
        if (a->param[i] != 0.0) return EST_ERR_DIM;
    const double *p = a->param;
    switch (a->family) {
    case EST_FAM_GAUSS_KF:
    case EST_FAM_HUBER_KF:
        if (!nonneg_fin(p[0]) || !nonneg_fin(p[1]) || !nonneg_fin(p[2])) return EST_ERR_NOT_PD;
        if (!(p[0] + p[1] > 0.0)) return EST_ERR_ENCODING;
        if (a->family == EST_FAM_HUBER_KF && !pos_fin(p[3])) return EST_ERR_NOT_PD;
        return EST_OK;
    case EST_FAM_STUDENT_T:
        if (!pos_fin(p[0]) || !pos_fin(p[1])) return EST_ERR_NOT_PD;
        return EST_OK;
    case EST_FAM_SCALE_MIX: {
        est_mix mx;
        return mix_from(a, &mx);
    }
    case EST_FAM_ADAPTIVE_T:
        if (!(p[0] >= 0.0 && p[0] <= 1.0)) return EST_ERR_ENCODING;
        if (!pos_fin(p[1]) || !pos_fin(p[2]) || !pos_fin(p[3]) || !pos_fin(p[4])) return EST_ERR_NOT_PD;
        return EST_OK;
    }
    return EST_ERR_KIND;
}

/* Assumption encoding (input to est_assumption_digest), little-endian:
 *   u32 family | u32 unit | f64 quantum | f64 lo | f64 hi | u32 nparam |
 *   f64 param[0..nparam-1]
 * f64 = IEEE-754 binary64 bit pattern, -0.0 canonicalized to +0.0 (NaN/Inf
 * are refused by est_assumption_check first). Entries param[nparam..] are
 * zero by contract and not encoded. Length = 36 + 8 * nparam bytes.
 * Digest = SHA-256("omega.est.assumption.v1" || 0x00 || encoding). */
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

est_status est_assumption_digest(const est_assumption *a, est_digest *out)
{
    if (!a || !out) return EST_ERR_NULL;
    est_status st = est_assumption_check(a);
    if (st != EST_OK) return st;
    uint8_t enc[36u + 8u * EST_PRED_NPARAM];
    size_t o = 0;
    o = put_u32(enc, o, (uint32_t)a->family);
    o = put_u32(enc, o, (uint32_t)a->unit);
    o = put_f64(enc, o, a->quantum);
    o = put_f64(enc, o, a->lo);
    o = put_f64(enc, o, a->hi);
    o = put_u32(enc, o, a->nparam);
    for (uint32_t i = 0; i < a->nparam; i++) o = put_f64(enc, o, a->param[i]);
    const uint8_t zero = 0;
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)EST_DOMAIN_ASSUMPTION, strlen(EST_DOMAIN_ASSUMPTION));
    sha256_update(&c, &zero, 1);
    sha256_update(&c, enc, o);
    sha256_final(&c, out->b);
    return EST_OK;
}

/* ---------------------------------------------------------- observation */

static est_obs_class classify(const est_assumption *a, double v, int present)
{
    if (!present) return EST_OBS_MISSING;
    if (!isfinite(v)) return EST_OBS_NONFINITE;
    if (v < a->lo || v > a->hi) return EST_OBS_OUT_OF_RANGE;
    double u = v / a->quantum;
    if (fabs(u - nearbyint(u)) > EST_PRED_GRID_TOL) return EST_OBS_OFF_GRID;
    return EST_OBS_OK;
}

est_status est_pobs_classify(const est_assumption *a, double value, int present,
                             const est_digest *evidence, est_pobs *out)
{
    if (!a || !evidence || !out) return EST_ERR_NULL;
    est_status st = est_assumption_check(a);
    if (st != EST_OK) return st;
    if (est_digest_is_zero(evidence)) return EST_ERR_KIND;
    out->value = value;
    out->cls = classify(a, value, present);
    out->evidence = *evidence;
    return EST_OK;
}

/* ------------------------------------------------- continuous change law */

typedef enum { D_GAUSS, D_T, D_MIX } dist_kind;
typedef struct {
    dist_kind kind;
    double scale;      /* sd (gauss) or t scale */
    double nu;
    est_mix mx;
} dist;

/* upper tail P(X > x) for x >= 0 of the zero-centred law */
static double dist_sf_pos(const dist *d, double x)
{
    switch (d->kind) {
    case D_GAUSS: return 0.5 * erfc(x / d->scale * SQRT1_2);
    case D_T: return t_sf_pos(x / d->scale, d->nu);
    case D_MIX: {
        double s = 0.0;
        for (uint32_t j = 0; j < d->mx.k; j++)
            if (d->mx.w[j] > 0.0) s += d->mx.w[j] * 0.5 * erfc(x / sqrt(d->mx.v[j]) * SQRT1_2);
        return s;
    }
    }
    return NAN;
}

/* p(k) = G((k+.5)q - mu) - G((k-.5)q - mu), edge bins take the outer tails.
 * S[e] = P(X > |x_e|) at every bin edge x_e; each difference is formed on the
 * side of zero where both tails are small (no cancellation). Normalized. */
static est_status pmf_from_dist(const dist *d, double mu, double q, double *out)
{
    double S[EST_PRED_N - 1];
    double x[EST_PRED_N - 1];
    for (int e = 0; e < EST_PRED_N - 1; e++) x[e] = ((double)(e - EST_PRED_K) + 0.5) * q - mu;
    if (mu == 0.0) {
        for (int e = EST_PRED_K; e < EST_PRED_N - 1; e++) {
            S[e] = dist_sf_pos(d, x[e]);
            S[EST_PRED_N - 2 - e] = S[e];
        }
    } else {
        for (int e = 0; e < EST_PRED_N - 1; e++) S[e] = dist_sf_pos(d, fabs(x[e]));
    }
    for (int e = 0; e < EST_PRED_N - 1; e++)
        if (!(S[e] >= 0.0 && S[e] <= 1.0)) return EST_ERR_NONFINITE;
    /* lower edge: P(X <= x_0); upper edge: P(X > x_{N-2}) */
    out[0] = x[0] <= 0.0 ? S[0] : 1.0 - S[0];
    out[EST_PRED_N - 1] = x[EST_PRED_N - 2] >= 0.0 ? S[EST_PRED_N - 2] : 1.0 - S[EST_PRED_N - 2];
    for (int i = 1; i < EST_PRED_N - 1; i++) {
        double a = x[i - 1], b = x[i], m;
        if (a >= 0.0) m = S[i - 1] - S[i];
        else if (b <= 0.0) m = S[i] - S[i - 1];
        else m = 1.0 - S[i - 1] - S[i];
        out[i] = m > 0.0 ? m : 0.0;
    }
    double s = 0.0;
    for (int i = 0; i < EST_PRED_N; i++) s += out[i];
    if (!(s > 0.0) || !isfinite(s)) return EST_ERR_NONFINITE;
    for (int i = 0; i < EST_PRED_N; i++) out[i] /= s;
    return EST_OK;
}

/* ------------------------------------------------------- pmf utilities */

static void support(const double *p, int32_t *lo, int32_t *hi)
{
    int l = 0, h = EST_PRED_N - 1;
    while (l < EST_PRED_N - 1 && p[l] == 0.0) l++;
    while (h > l && p[h] == 0.0) h--;
    *lo = l;
    *hi = h;
}

/* out = a * b with offsets beyond +-K folded into the edge bins. out may not
 * alias a or b. The folded mass of row i is a[i] times a cumulative tail of b
 * (C summed up from the low end, D down from the high end, so small tails are
 * not differences of large numbers); the in-range part is a plain loop. */
static void conv_fold(const double *a, const double *b, double *out)
{
    double C[EST_PRED_N], D[EST_PRED_N];
    int32_t al, ah, bl, bh;
    support(a, &al, &ah);
    support(b, &bl, &bh);
    double s = 0.0;
    for (int j = 0; j < EST_PRED_N; j++) { s += b[j]; C[j] = s; }
    s = 0.0;
    for (int j = EST_PRED_N - 1; j >= 0; j--) { s += b[j]; D[j] = s; }
    for (int i = 0; i < EST_PRED_N; i++) out[i] = 0.0;
    for (int32_t i = al; i <= ah; i++) {
        double ai = a[i];
        if (ai == 0.0) continue;
        int32_t m = EST_PRED_K - i - 1;            /* j <= m lands below -K */
        if (m >= bl) out[0] += ai * C[m < bh ? m : bh];
        m = EST_PRED_N + EST_PRED_K - i;           /* j >= m lands above +K */
        if (m <= bh) out[EST_PRED_N - 1] += ai * D[m > bl ? m : bl];
        int32_t j0 = EST_PRED_K - i, j1 = EST_PRED_N - 1 + EST_PRED_K - i;
        if (j0 < bl) j0 = bl;
        if (j1 > bh) j1 = bh;
        int32_t off = i - EST_PRED_K;
        for (int32_t j = j0; j <= j1; j++) out[j + off] += ai * b[j];
    }
}

est_status est_pmf_conv_pow(const double *one, uint32_t h, double *out)
{
    if (!one || !out) return EST_ERR_NULL;
    if (h < 1u || h > EST_PRED_MAX_H) return EST_ERR_TIME;
    for (int i = 0; i < EST_PRED_N; i++)
        if (!isfinite(one[i]) || one[i] < 0.0) return EST_ERR_NONFINITE;
    double base[EST_PRED_N], acc[EST_PRED_N], tmp[EST_PRED_N];
    memcpy(base, one, sizeof base);
    int have = 0;
    for (uint32_t r = h;;) {
        if (r & 1u) {
            if (!have) { memcpy(acc, base, sizeof acc); have = 1; }
            else { conv_fold(acc, base, tmp); memcpy(acc, tmp, sizeof acc); }
        }
        r >>= 1;
        if (!r) break;
        conv_fold(base, base, tmp);
        memcpy(base, tmp, sizeof base);
    }
    memcpy(out, acc, sizeof acc);
    return EST_OK;
}

void est_pmf_floor(double *pmf)
{
    if (!pmf) return;
    double s = 0.0;
    for (int i = 0; i < EST_PRED_N; i++) s += pmf[i];
    double u = EST_PRED_FLOOR / (double)EST_PRED_N;
    double t = 0.0;
    for (int i = 0; i < EST_PRED_N; i++) {
        pmf[i] = (1.0 - EST_PRED_FLOOR) * (s > 0.0 ? pmf[i] / s : 0.0) + u;
        t += pmf[i];
    }
    for (int i = 0; i < EST_PRED_N; i++) pmf[i] /= t;
}

/* ------------------------------------------------------------ candidate */

static est_status bind(const est_assumption *a, const est_bstate *b)
{
    est_digest d;
    est_status st = est_assumption_digest(a, &d);
    if (st != EST_OK) return st;
    if (memcmp(d.b, b->assumption.b, EST_DIGEST_SIZE) != 0) return EST_ERR_MODEL;
    return EST_OK;
}

est_status est_pred_init(const est_assumption *a, est_bstate *b)
{
    if (!a || !b) return EST_ERR_NULL;
    est_digest d;
    est_status st = est_assumption_digest(a, &d);
    if (st != EST_OK) return st;
    memset(b, 0, sizeof *b);
    b->assumption = d;
    if (a->family == EST_FAM_GAUSS_KF || a->family == EST_FAM_HUBER_KF) b->P = a->param[2];
    if (a->family == EST_FAM_ADAPTIVE_T) b->s2 = a->param[4] * a->param[4];
    return EST_OK;
}

/* The persistence families' continuous one-step change law and its cache key. */
static est_status persist_dist(const est_assumption *a, const est_bstate *b, dist *d, double *key)
{
    memset(d, 0, sizeof *d);
    switch (a->family) {
    case EST_FAM_STUDENT_T:
        d->kind = D_T; d->nu = a->param[0]; d->scale = a->param[1];
        *key = d->scale;
        return EST_OK;
    case EST_FAM_ADAPTIVE_T:
        d->kind = D_T; d->nu = a->param[1]; d->scale = a->param[2] * sqrt(b->s2);
        if (!pos_fin(d->scale)) return EST_ERR_NONFINITE;
        *key = d->scale;
        return EST_OK;
    case EST_FAM_SCALE_MIX: {
        d->kind = D_MIX;
        est_status st = mix_from(a, &d->mx);
        if (st != EST_OK) return st;
        *key = est_mix_total_var(&d->mx);
        return EST_OK;
    }
    default:
        return EST_ERR_KIND;
    }
}

est_status est_pred_predict(const est_assumption *a, est_bstate *b, uint32_t h, est_dpred *out)
{
    if (!a || !b || !out) return EST_ERR_NULL;
    est_status st = bind(a, b);
    if (st != EST_OK) return st;
    if (!b->has_anchor) return EST_ERR_STALE;
    uint64_t H = (uint64_t)b->gap + (uint64_t)h;
    if (h < 1u || H < 1u || H > EST_PRED_MAX_H) return EST_ERR_TIME;
    double q = a->quantum;

    if (a->family == EST_FAM_GAUSS_KF || a->family == EST_FAM_HUBER_KF) {
        double var = b->P + (double)H * a->param[0] + a->param[1];
        dist d;
        memset(&d, 0, sizeof d);
        d.kind = D_GAUSS;
        d.scale = sqrt(var);
        if (!pos_fin(d.scale)) return EST_ERR_NONFINITE;
        st = pmf_from_dist(&d, b->xhat - b->anchor, q, out->pmf);
        if (st != EST_OK) return st;
    } else {
        dist d;
        double key = 0.0;
        st = persist_dist(a, b, &d, &key);
        if (st != EST_OK) return st;
        if (!b->cache_valid || b->cache_key != key) {
            st = pmf_from_dist(&d, 0.0, q, b->cache);
            if (st != EST_OK) { b->cache_valid = 0; return st; }
            support(b->cache, &b->cache_lo, &b->cache_hi);
            b->cache_lo -= EST_PRED_K;
            b->cache_hi -= EST_PRED_K;
            b->cache_key = key;
            b->cache_valid = 1;
        }
        st = est_pmf_conv_pow(b->cache, (uint32_t)H, out->pmf);
        if (st != EST_OK) return st;
    }
    est_pmf_floor(out->pmf);
    b->generation++;
    out->assumption = b->assumption;
    out->family = a->family;
    out->horizon = (uint32_t)H;
    out->generation = b->generation;
    out->anchor = b->anchor;
    out->quantum = q;
    return EST_OK;
}

est_status est_pred_update(const est_assumption *a, est_bstate *b, const est_pobs *obs)
{
    if (!a || !b || !obs) return EST_ERR_NULL;
    est_status st = bind(a, b);
    if (st != EST_OK) return st;
    if (est_digest_is_zero(&obs->evidence)) return EST_ERR_KIND;
    if ((uint32_t)obs->cls >= (uint32_t)EST_OBS_CLASSES_) return EST_ERR_KIND;
    est_obs_class cls = classify(a, obs->value, obs->cls != EST_OBS_MISSING);
    if (cls != obs->cls) return EST_ERR_KIND;

    est_digest root;
    st = est_evidence_root_extend(&b->evidence_root, &obs->evidence, &root);
    if (st != EST_OK) return st;

    if (cls == EST_OBS_OK) {
        double y = obs->value;
        if (!b->has_anchor) {
            /* First anchor: F1/F2 start at xhat = y with level variance p0
             * (p0 is the variance of the level given that one reading). */
            if (a->family == EST_FAM_GAUSS_KF || a->family == EST_FAM_HUBER_KF) {
                b->xhat = y;
                b->P = a->param[2];
            }
        } else {
            if (a->family == EST_FAM_GAUSS_KF || a->family == EST_FAM_HUBER_KF) {
                double Pp = b->P + ((double)b->gap + 1.0) * a->param[0];
                double S = Pp + a->param[1];
                double K = Pp / S;
                double e = y - b->xhat;
                if (a->family == EST_FAM_HUBER_KF) {
                    double c = a->param[3] * sqrt(S);
                    if (e > c) e = c;
                    else if (e < -c) e = -c;
                }
                b->xhat += K * e;
                b->P = (1.0 - K) * Pp;
                if (b->P < 0.0) b->P = 0.0;
            } else if (a->family == EST_FAM_ADAPTIVE_T && b->gap == 0u) {
                double dd = y - b->anchor, lam = a->param[0], fl = a->param[3];
                double d2 = dd * dd;
                b->s2 = lam * b->s2 + (1.0 - lam) * (d2 > fl ? d2 : fl);
            }
        }
        b->anchor = y;
        b->has_anchor = 1u;
        b->gap = 0u;
    } else if (b->gap < UINT32_MAX) {
        b->gap++;
    }
    b->generation++;
    b->evidence_root = root;
    b->count[cls]++;
    return EST_OK;
}

/* -------------------------------------------------------------- scoring */

static est_status check_dpred(const est_dpred *p)
{
    if (!isfinite(p->anchor) || !pos_fin(p->quantum)) return EST_ERR_NONFINITE;
    if (p->horizon < 1u || p->horizon > EST_PRED_MAX_H) return EST_ERR_TIME;
    double s = 0.0;
    for (int i = 0; i < EST_PRED_N; i++) {
        if (!isfinite(p->pmf[i]) || p->pmf[i] < 0.0) return EST_ERR_NONFINITE;
        s += p->pmf[i];
    }
    if (!(s > 0.0)) return EST_ERR_NONFINITE;
    return EST_OK;
}

est_status est_dpred_score(const est_dpred *p, const est_pobs *obs, est_pinnov *out)
{
    if (!p || !obs || !out) return EST_ERR_NULL;
    est_status st = check_dpred(p);
    if (st != EST_OK) return st;
    if ((uint32_t)obs->cls >= (uint32_t)EST_OBS_CLASSES_) return EST_ERR_KIND;
    memset(out, 0, sizeof *out);
    out->cls = obs->cls;
    if (obs->cls != EST_OBS_OK) return EST_OK;
    /* an OK record must carry a finite on-grid value */
    if (!isfinite(obs->value)) return EST_ERR_KIND;
    double u = obs->value / p->quantum;
    if (fabs(u - nearbyint(u)) > EST_PRED_GRID_TOL) return EST_ERR_KIND;
    double kd = nearbyint((obs->value - p->anchor) / p->quantum);
    if (kd < -(double)EST_PRED_K) kd = -(double)EST_PRED_K;
    if (kd > (double)EST_PRED_K) kd = (double)EST_PRED_K;
    int32_t k = (int32_t)kd;
    int idx = k + EST_PRED_K;
    double below = 0.0, above = 0.0;
    for (int i = 0; i < idx; i++) below += p->pmf[i];
    for (int i = EST_PRED_N - 1; i > idx; i--) above += p->pmf[i];
    double pk = p->pmf[idx], tot = below + pk + above;
    out->valid = 1u;
    out->k = k;
    out->F_lo = below / tot;
    out->F_hi = (below + pk) / tot;
    if (out->F_hi > 1.0) out->F_hi = 1.0;
    double pr = pk / tot;
    out->logp = log(pr > 1e-300 ? pr : 1e-300);
    out->z = est_norm_ppf(0.5 * (out->F_lo + out->F_hi));
    return EST_OK;
}

est_status est_pred_innov(const est_bstate *b, const est_dpred *p, const est_pobs *obs,
                          est_pinnov *out)
{
    if (!b || !p || !obs || !out) return EST_ERR_NULL;
    if (memcmp(p->assumption.b, b->assumption.b, EST_DIGEST_SIZE) != 0) return EST_ERR_STALE;
    if (!b->has_anchor || p->generation != b->generation || (uint64_t)p->horizon != (uint64_t)b->gap + 1u)
        return EST_ERR_STALE;
    return est_dpred_score(p, obs, out);
}

est_status est_dpred_width80(const est_dpred *p, double *mc)
{
    if (!p || !mc) return EST_ERR_NULL;
    est_status st = check_dpred(p);
    if (st != EST_OK) return st;
    double tot = 0.0;
    for (int i = 0; i < EST_PRED_N; i++) tot += p->pmf[i];
    int lo = -1, hi = -1;
    double c = 0.0;
    for (int i = 0; i < EST_PRED_N; i++) {
        c += p->pmf[i] / tot;
        if (lo < 0 && c >= 0.1) lo = i;
        if (hi < 0 && c >= 0.9) { hi = i; break; }
    }
    if (lo < 0) lo = EST_PRED_N - 1;
    if (hi < 0) hi = EST_PRED_N - 1;
    *mc = (double)(hi - lo) * p->quantum;
    return EST_OK;
}

/* ---------------------------------------------------------- calibration */

void est_calib_init(est_calib *c)
{
    if (c) memset(c, 0, sizeof *c);
}

static double overlap(double a0, double a1, double b0, double b1)
{
    double lo = a0 > b0 ? a0 : b0, hi = a1 < b1 ? a1 : b1;
    return hi > lo ? hi - lo : 0.0;
}

double est_frac_cover(double F_lo, double F_hi, double alpha)
{
    double lo = 0.5 * (1.0 - alpha), hi = 0.5 * (1.0 + alpha);
    if (!(F_hi > F_lo)) return (F_lo >= lo && F_lo <= hi) ? 1.0 : 0.0;
    return overlap(F_lo, F_hi, lo, hi) / (F_hi - F_lo);
}

est_status est_calib_add(est_calib *c, const est_dpred *p, const est_pinnov *iv)
{
    static const double alpha[3] = { 0.50, 0.80, 0.95 };
    if (!c || !p || !iv) return EST_ERR_NULL;
    if (iv->valid != 1u || iv->cls != EST_OBS_OK) return EST_ERR_KIND;
    if (!isfinite(iv->F_lo) || !isfinite(iv->F_hi) || !isfinite(iv->z) || !isfinite(iv->logp))
        return EST_ERR_NONFINITE;
    if (iv->F_lo < 0.0 || iv->F_hi > 1.0 || iv->F_hi < iv->F_lo) return EST_ERR_ENCODING;
    double w;
    est_status st = est_dpred_width80(p, &w);
    if (st != EST_OK) return st;
    for (int i = 0; i < 3; i++) c->cov[i] += est_frac_cover(iv->F_lo, iv->F_hi, alpha[i]);
    double span = iv->F_hi - iv->F_lo;
    if (span > 0.0) {
        for (int j = 0; j < 10; j++)
            c->pit[j] += overlap(iv->F_lo, iv->F_hi, j / 10.0, (j + 1) / 10.0) / span;
    } else {
        int j = (int)floor(iv->F_lo * 10.0);
        if (j < 0) j = 0;
        if (j > 9) j = 9;
        c->pit[j] += 1.0;
    }
    c->logp_sum += iv->logp;
    c->width_sum += w;
    c->z_sum += iv->z;
    c->z_sq += iv->z * iv->z;
    if (c->n > 0) c->z_lag += iv->z * c->z_prev;
    c->z_prev = iv->z;
    c->n++;
    return EST_OK;
}

void est_bands_default(est_bands *b)
{
    if (!b) return;
    b->cov50[0] = 0.46; b->cov50[1] = 0.54;
    b->cov80[0] = 0.76; b->cov80[1] = 0.84;
    b->cov95[0] = 0.93; b->cov95[1] = 0.97;
    b->pit[0] = 0.07; b->pit[1] = 0.13;
    b->bias_max = 0.10;
    b->lag1_max = 0.20;
    b->min_n = 2000;
}

/* (S_xy/(n-1) - m^2) / (S_xx/n - m^2). NaN for n < 2. If z never varied
 * (S_xx/n - m^2 <= 0) there is no serial structure to measure: 0. */
double est_calib_lag1(const est_calib *c)
{
    if (!c || c->n < 2) return NAN;
    double n = (double)c->n, m = c->z_sum / n;
    double den = c->z_sq / n - m * m;
    if (!(den > 0.0)) return 0.0;
    return (c->z_lag / (n - 1.0) - m * m) / den;
}

static int in_band(double v, const double *band) { return v >= band[0] && v <= band[1]; }

/* reason: "calibrated", or "<stat> <value> outside <band>" naming the first
 * failing statistic in the order n, cov50, cov80, cov95, pit0..pit9, bias,
 * lag1. A NaN statistic fails. */
est_verdict est_calib_verdict(const est_calib *c, const est_bands *b, char *reason, size_t cap)
{
    char dummy[1];
    if (!reason || cap == 0) { reason = dummy; cap = sizeof dummy; }
    if (!c || !b) { snprintf(reason, cap, "null"); return EST_NOT_CALIBRATED; }
    if (c->n < b->min_n || c->n == 0) {
        snprintf(reason, cap, "n %llu below %llu", (unsigned long long)c->n,
                 (unsigned long long)b->min_n);
        return EST_NOT_CALIBRATED;
    }
    double n = (double)c->n;
    static const char *const cn[3] = { "cov50", "cov80", "cov95" };
    const double *cb[3] = { b->cov50, b->cov80, b->cov95 };
    for (int i = 0; i < 3; i++) {
        double v = c->cov[i] / n;
        if (!in_band(v, cb[i])) {
            snprintf(reason, cap, "%s %.4f outside [%.2f,%.2f]", cn[i], v, cb[i][0], cb[i][1]);
            return EST_NOT_CALIBRATED;
        }
    }
    for (int j = 0; j < 10; j++) {
        double v = c->pit[j] / n;
        if (!in_band(v, b->pit)) {
            snprintf(reason, cap, "pit%d %.4f outside [%.2f,%.2f]", j, v, b->pit[0], b->pit[1]);
            return EST_NOT_CALIBRATED;
        }
    }
    double bias = c->z_sum / n;
    if (!(fabs(bias) <= b->bias_max)) {
        snprintf(reason, cap, "bias %.4f beyond %.2f", bias, b->bias_max);
        return EST_NOT_CALIBRATED;
    }
    double l1 = est_calib_lag1(c);
    if (!(fabs(l1) <= b->lag1_max)) {
        snprintf(reason, cap, "lag1 %.4f beyond %.2f", l1, b->lag1_max);
        return EST_NOT_CALIBRATED;
    }
    snprintf(reason, cap, "calibrated");
    return EST_CALIBRATED;
}
