/* EST-0 implementation: validation, digests, fixed little-endian encoding.
 * See est_types.h for the contract. No allocation, no globals, no I/O.
 *
 * Canonical encoding rules (all encoders):
 *   - Header, 8 bytes: magic "OEST", u8 kind (est_kind), u8 EST_FORMAT_VERSION,
 *     u16 reserved = 0.
 *   - Then the fields in struct order. u32, u64, i64 are little-endian.
 *     Enums are u32. A double is the little-endian IEEE-754 binary64 bit
 *     pattern. A digest is its 32 raw bytes.
 *   - Only the meaningful n / m entries of arrays are encoded (unit[n],
 *     x[n], P as n*n row-major, ...). Dimension fields precede the arrays so
 *     the total length is determined by the fixed part. Entries beyond n / m
 *     are not part of a record's identity.
 *   - NaN and Inf are refused (validation runs before encoding). -0.0 is
 *     canonicalized to +0.0 before encoding so equal values digest equally;
 *     therefore a decoded -0.0 comes back as +0.0. There are no other
 *     equal-but-different double bit patterns once NaN is excluded.
 *   - Digest = SHA-256(domain string bytes || 0x00 || encoding). */
#include "est_types.h"
#include "sha256.h"

#include <math.h>
#include <string.h>

#define EST_TMP_MAX 2304u /* largest encoding: model with n = m = 8 is 2160 bytes */
#define EST_HDR 8u

/* ------------------------------------------------------------ validation */

static int in_dim(uint32_t d) { return d >= 1u && d <= EST_MAX_DIM; }

static est_status check_finite(const double *a, size_t cnt)
{
    for (size_t i = 0; i < cnt; i++)
        if (!isfinite(a[i])) return EST_ERR_NONFINITE;
    return EST_OK;
}

static est_status check_units(const est_unit *u, uint32_t cnt)
{
    for (uint32_t i = 0; i < cnt; i++)
        if ((uint32_t)u[i] < 1u || (uint32_t)u[i] >= (uint32_t)EST_UNIT_MAX_) return EST_ERR_UNIT;
    return EST_OK;
}

est_status est_check_covariance(const double *A, uint32_t n)
{
    if (!A) return EST_ERR_NULL;
    if (!in_dim(n)) return EST_ERR_DIM;
    est_status st = check_finite(A, (size_t)n * n);
    if (st != EST_OK) return st;

    double maxabs = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        if (A[i * n + i] < 0.0) return EST_ERR_NOT_PSD; /* any negative variance */
        for (uint32_t j = 0; j < n; j++) {
            double a = fabs(A[i * n + j]);
            if (a > maxabs) maxabs = a;
        }
    }
    double stol = 1e-9 * maxabs;
    if (stol < 1e-300) stol = 1e-300;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = 0; j < i; j++)
            if (fabs(A[i * n + j] - A[j * n + i]) > stol) return EST_ERR_ASYMMETRIC;

    /* Scale to unit diagonal so each variance is judged against itself, then
     * LDL^T on the lower triangle. A zero variance needs a null row/column.
     * Off-diagonals above the geometric mean of the two sds are refused.
     * A pivot below -1e-12 is a materially negative direction; a pivot within
     * +-1e-12 is a null direction whose column must be null too. */
    double s[EST_MAX_DIM], w[EST_MAX_DIM * EST_MAX_DIM];
    for (uint32_t i = 0; i < n; i++) s[i] = sqrt(A[i * n + i]);
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = 0; j <= i; j++) {
            double c = A[i * n + j];
            if (i == j) { w[i * n + j] = s[i] > 0.0 ? 1.0 : 0.0; continue; }
            if (s[i] == 0.0 || s[j] == 0.0) {
                if (c != 0.0) return EST_ERR_NOT_PSD;
                w[i * n + j] = 0.0;
                continue;
            }
            double r = (c / s[i]) / s[j];
            if (fabs(r) > 1.0 + 1e-9) return EST_ERR_NOT_PSD;
            w[i * n + j] = r;
        }
    const double tol = 1e-12;
    for (uint32_t k = 0; k < n; k++) {
        double d = w[k * n + k];
        if (d < -tol) return EST_ERR_NOT_PSD;
        if (d <= tol) {
            for (uint32_t i = k + 1; i < n; i++) {
                double c = w[i * n + k];
                if (c * c > tol) return EST_ERR_NOT_PSD;
            }
            continue;
        }
        for (uint32_t i = k + 1; i < n; i++)
            for (uint32_t j = k + 1; j <= i; j++)
                w[i * n + j] -= w[i * n + k] * w[j * n + k] / d;
    }
    return EST_OK;
}

int est_digest_is_zero(const est_digest *d)
{
    if (!d) return 1;
    uint8_t acc = 0;
    for (size_t i = 0; i < EST_DIGEST_SIZE; i++) acc |= d->b[i];
    return acc == 0;
}

/* Strict Cholesky: true if the symmetric n*n matrix is positive definite. */
static int is_pd(const double *A, uint32_t n)
{
    double L[EST_MAX_DIM * EST_MAX_DIM];
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = 0; j <= i; j++) {
            double s = A[i * n + j];
            for (uint32_t k = 0; k < j; k++) s -= L[i * n + k] * L[j * n + k];
            if (i == j) {
                if (!(s > 0.0) || !isfinite(s)) return 0;
                L[i * n + i] = sqrt(s);
            } else {
                L[i * n + j] = s / L[j * n + j];
            }
        }
    return 1;
}

est_status est_check_model(const est_model *mdl)
{
    if (!mdl) return EST_ERR_NULL;
    if (!in_dim(mdl->n) || !in_dim(mdl->m)) return EST_ERR_DIM;
    if (mdl->estimator != EST_ESTIMATOR_LINEAR_KALMAN) return EST_ERR_UNIT;
    if (mdl->meaning != EST_UNCERTAINTY_GAUSSIAN_COVARIANCE) return EST_ERR_UNIT;
    est_status st = check_units(mdl->state_unit, mdl->n);
    if (st != EST_OK) return st;
    st = check_units(mdl->obs_unit, mdl->m);
    if (st != EST_OK) return st;
    if (mdl->step_ns <= 0) return EST_ERR_TIME;
    st = check_finite(mdl->F, (size_t)mdl->n * mdl->n);
    if (st != EST_OK) return st;
    st = check_finite(mdl->H, (size_t)mdl->m * mdl->n);
    if (st != EST_OK) return st;
    st = est_check_covariance(mdl->Q, mdl->n);
    if (st != EST_OK) return st;
    st = est_check_covariance(mdl->R, mdl->m);
    if (st != EST_OK) return st;
    /* Observation noise must be positive definite: R = 0 wedges every update. */
    return is_pd(mdl->R, mdl->m) ? EST_OK : EST_ERR_NOT_PD;
}

est_status est_check_observation(const est_observation *o)
{
    if (!o) return EST_ERR_NULL;
    if (!in_dim(o->m)) return EST_ERR_DIM;
    est_status st = check_units(o->unit, o->m);
    if (st != EST_OK) return st;
    st = check_finite(o->z, o->m);
    if (st != EST_OK) return st;
    st = est_check_covariance(o->R, o->m);
    if (st != EST_OK) return st;
    /* Evidence with nothing bound to it is not evidence, and an observation
     * that names no producer is not attributable. */
    if (est_digest_is_zero(&o->evidence)) return EST_ERR_KIND;
    if (est_digest_is_zero(&o->source)) return EST_ERR_KIND;
    return EST_OK;
}

est_status est_check_belief(const est_belief *b)
{
    if (!b) return EST_ERR_NULL;
    if (!in_dim(b->n)) return EST_ERR_DIM;
    est_status st = check_units(b->unit, b->n);
    if (st != EST_OK) return st;
    st = check_finite(b->x, b->n);
    if (st != EST_OK) return st;
    /* Structure: a belief always names its model; generation 0 is a declared
     * prior (no parent, no evidence); any later generation has a parent. The
     * root may be zero after a coast from a prior, so it is not required. */
    if (est_digest_is_zero(&b->model)) return EST_ERR_MODEL;
    if (b->generation == 0u) {
        if (!est_digest_is_zero(&b->parent) || !est_digest_is_zero(&b->evidence_root)) return EST_ERR_STALE;
    } else if (est_digest_is_zero(&b->parent)) {
        return EST_ERR_STALE;
    }
    return est_check_covariance(b->P, b->n);
}

est_status est_check_prediction(const est_prediction *p)
{
    if (!p) return EST_ERR_NULL;
    if (!in_dim(p->n) || !in_dim(p->m)) return EST_ERR_DIM;
    if (p->horizon < 1u || p->horizon > EST_MAX_HORIZON) return EST_ERR_TIME;
    if (p->has_control > 1u) return EST_ERR_ENCODING;
    est_status st = check_finite(p->x, p->n);
    if (st != EST_OK) return st;
    if (p->has_control) {
        st = check_finite(p->Bu, p->n);
        if (st != EST_OK) return st;
    }
    st = check_finite(p->y_mean, p->m);
    if (st != EST_OK) return st;
    st = est_check_covariance(p->P, p->n);
    if (st != EST_OK) return st;
    return est_check_covariance(p->S, p->m);
}

est_status est_check_innovation(const est_innovation *iv)
{
    if (!iv) return EST_ERR_NULL;
    if (!in_dim(iv->m)) return EST_ERR_DIM;
    est_status st = check_finite(iv->nu, iv->m);
    if (st != EST_OK) return st;
    if (!isfinite(iv->nis)) return EST_ERR_NONFINITE;
    if (iv->nis < 0.0) return EST_ERR_NOT_PSD;
    return est_check_covariance(iv->S, iv->m);
}

/* -------------------------------------------------------- byte plumbing */

typedef struct { uint8_t *p; size_t cap, len; int bad; } wr;

static void w_raw(wr *w, const void *s, size_t n)
{
    if (w->bad || n > w->cap - w->len) { w->bad = 1; return; }
    memcpy(w->p + w->len, s, n);
    w->len += n;
}
static void w_u32(wr *w, uint32_t v)
{
    uint8_t t[4];
    for (int i = 0; i < 4; i++) t[i] = (uint8_t)(v >> (8 * i));
    w_raw(w, t, 4);
}
static void w_u64(wr *w, uint64_t v)
{
    uint8_t t[8];
    for (int i = 0; i < 8; i++) t[i] = (uint8_t)(v >> (8 * i));
    w_raw(w, t, 8);
}
static void w_i64(wr *w, int64_t v) { w_u64(w, (uint64_t)v); }
static void w_f64(wr *w, double d)
{
    uint64_t u = 0; /* +0.0 and -0.0 both encode as all-zero bits */
    if (d != 0.0) memcpy(&u, &d, 8);
    w_u64(w, u);
}
static void w_dig(wr *w, const est_digest *d) { w_raw(w, d->b, EST_DIGEST_SIZE); }
static void w_hdr(wr *w, est_kind k)
{
    static const uint8_t magic[4] = { 'O', 'E', 'S', 'T' };
    w_raw(w, magic, 4);
    uint8_t t[4] = { (uint8_t)k, (uint8_t)EST_FORMAT_VERSION, 0, 0 };
    w_raw(w, t, 4);
}
static void w_farr(wr *w, const double *a, size_t n) { for (size_t i = 0; i < n; i++) w_f64(w, a[i]); }
static void w_uarr(wr *w, const est_unit *u, uint32_t n) { for (uint32_t i = 0; i < n; i++) w_u32(w, (uint32_t)u[i]); }

typedef struct { const uint8_t *p; size_t len, pos; est_status st; } rd;

static void r_fail(rd *r, est_status s) { if (r->st == EST_OK) r->st = s; }
static int r_need(rd *r, size_t n)
{
    if (r->st != EST_OK) return 0;
    if (n > r->len - r->pos) { r_fail(r, EST_ERR_ENCODING); return 0; }
    return 1;
}
static uint32_t r_u32(rd *r)
{
    if (!r_need(r, 4)) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)r->p[r->pos + (size_t)i] << (8 * i);
    r->pos += 4;
    return v;
}
static uint64_t r_u64(rd *r)
{
    if (!r_need(r, 8)) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)r->p[r->pos + (size_t)i] << (8 * i);
    r->pos += 8;
    return v;
}
static int64_t r_i64(rd *r) { return (int64_t)r_u64(r); }
static double r_f64(rd *r)
{
    uint64_t u = r_u64(r);
    double d;
    memcpy(&d, &u, 8);
    return d;
}
static void r_dig(rd *r, est_digest *d)
{
    if (!r_need(r, EST_DIGEST_SIZE)) return;
    memcpy(d->b, r->p + r->pos, EST_DIGEST_SIZE);
    r->pos += EST_DIGEST_SIZE;
}
static void r_farr(rd *r, double *a, size_t n) { for (size_t i = 0; i < n; i++) a[i] = r_f64(r); }
static void r_uarr(rd *r, est_unit *u, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = r_u32(r);
        if (v >= (uint32_t)EST_UNIT_MAX_) { r_fail(r, EST_ERR_UNIT); v = 0; }
        u[i] = (est_unit)v;
    }
}
static uint32_t r_dim(rd *r)
{
    uint32_t d = r_u32(r);
    if (r->st == EST_OK && !in_dim(d)) { r_fail(r, EST_ERR_DIM); return 0; }
    return d;
}
/* Header check. Wrong magic, version: ENCODING. Wrong kind: KIND. */
static est_status r_hdr(rd *r, est_kind want)
{
    if (r->len < EST_HDR) return EST_ERR_ENCODING;
    if (memcmp(r->p, "OEST", 4) != 0) return EST_ERR_ENCODING;
    if (r->p[4] != (uint8_t)want) return EST_ERR_KIND;
    if (r->p[5] != (uint8_t)EST_FORMAT_VERSION || r->p[6] != 0 || r->p[7] != 0) return EST_ERR_ENCODING;
    r->pos = EST_HDR;
    return EST_OK;
}
static est_status r_done(rd *r)
{
    if (r->st != EST_OK) return r->st;
    return r->pos == r->len ? EST_OK : EST_ERR_ENCODING;
}

/* ------------------------------------------------------------- encoders */

/* Model layout after the header:
 *   u32 n, u32 m, u32 estimator, u32 meaning,
 *   u32 state_unit[n], u32 obs_unit[m],
 *   f64 F[n*n], f64 Q[n*n], f64 H[m*n], f64 R[m*m] (row-major), i64 step_ns. */
static est_status enc_model(const est_model *o, uint8_t *buf, size_t cap, size_t *len)
{
    est_status st = est_check_model(o);
    if (st != EST_OK) return st;
    wr w = { buf, cap, 0, 0 };
    w_hdr(&w, EST_KIND_MODEL);
    w_u32(&w, o->n); w_u32(&w, o->m);
    w_u32(&w, (uint32_t)o->estimator); w_u32(&w, (uint32_t)o->meaning);
    w_uarr(&w, o->state_unit, o->n); w_uarr(&w, o->obs_unit, o->m);
    w_farr(&w, o->F, (size_t)o->n * o->n); w_farr(&w, o->Q, (size_t)o->n * o->n);
    w_farr(&w, o->H, (size_t)o->m * o->n); w_farr(&w, o->R, (size_t)o->m * o->m);
    w_i64(&w, o->step_ns);
    if (w.bad) return EST_ERR_ENCODING;
    *len = w.len;
    return EST_OK;
}

/* Observation layout after the header:
 *   u32 m, u32 unit[m], f64 z[m], f64 R[m*m], i64 t_ns, u64 seq,
 *   32 bytes source, 32 bytes evidence. */
static est_status enc_observation(const est_observation *o, uint8_t *buf, size_t cap, size_t *len)
{
    est_status st = est_check_observation(o);
    if (st != EST_OK) return st;
    wr w = { buf, cap, 0, 0 };
    w_hdr(&w, EST_KIND_OBSERVATION);
    w_u32(&w, o->m); w_uarr(&w, o->unit, o->m);
    w_farr(&w, o->z, o->m); w_farr(&w, o->R, (size_t)o->m * o->m);
    w_i64(&w, o->t_ns); w_u64(&w, o->seq);
    w_dig(&w, &o->source); w_dig(&w, &o->evidence);
    if (w.bad) return EST_ERR_ENCODING;
    *len = w.len;
    return EST_OK;
}

/* Belief layout after the header:
 *   u32 n, u32 unit[n], f64 x[n], f64 P[n*n], u64 generation, i64 t_ns,
 *   32 bytes model, 32 bytes parent, 32 bytes evidence_root. */
static est_status enc_belief(const est_belief *o, uint8_t *buf, size_t cap, size_t *len)
{
    est_status st = est_check_belief(o);
    if (st != EST_OK) return st;
    wr w = { buf, cap, 0, 0 };
    w_hdr(&w, EST_KIND_BELIEF);
    w_u32(&w, o->n); w_uarr(&w, o->unit, o->n);
    w_farr(&w, o->x, o->n); w_farr(&w, o->P, (size_t)o->n * o->n);
    w_u64(&w, o->generation); w_i64(&w, o->t_ns);
    w_dig(&w, &o->model); w_dig(&w, &o->parent); w_dig(&w, &o->evidence_root);
    if (w.bad) return EST_ERR_ENCODING;
    *len = w.len;
    return EST_OK;
}

/* Prediction layout after the header:
 *   32 bytes prior, 32 bytes model, u32 horizon, u64 generation, i64 t_ns,
 *   u32 n, u32 m, f64 x[n], f64 P[n*n], f64 y_mean[m], f64 S[m*m],
 *   u32 has_control (0 or 1), then f64 Bu[n] only if has_control == 1. */
static est_status enc_prediction(const est_prediction *o, uint8_t *buf, size_t cap, size_t *len)
{
    est_status st = est_check_prediction(o);
    if (st != EST_OK) return st;
    wr w = { buf, cap, 0, 0 };
    w_hdr(&w, EST_KIND_PREDICTION);
    w_dig(&w, &o->prior); w_dig(&w, &o->model);
    w_u32(&w, o->horizon); w_u64(&w, o->generation); w_i64(&w, o->t_ns);
    w_u32(&w, o->n); w_u32(&w, o->m);
    w_farr(&w, o->x, o->n); w_farr(&w, o->P, (size_t)o->n * o->n);
    w_farr(&w, o->y_mean, o->m); w_farr(&w, o->S, (size_t)o->m * o->m);
    w_u32(&w, o->has_control);
    if (o->has_control) w_farr(&w, o->Bu, o->n);
    if (w.bad) return EST_ERR_ENCODING;
    *len = w.len;
    return EST_OK;
}

/* Innovation layout after the header:
 *   32 bytes prediction, 32 bytes observation, 32 bytes model, u32 m,
 *   f64 nu[m], f64 S[m*m], f64 nis, i64 t_ns. */
static est_status enc_innovation(const est_innovation *o, uint8_t *buf, size_t cap, size_t *len)
{
    est_status st = est_check_innovation(o);
    if (st != EST_OK) return st;
    wr w = { buf, cap, 0, 0 };
    w_hdr(&w, EST_KIND_INNOVATION);
    w_dig(&w, &o->prediction); w_dig(&w, &o->observation); w_dig(&w, &o->model);
    w_u32(&w, o->m);
    w_farr(&w, o->nu, o->m); w_farr(&w, o->S, (size_t)o->m * o->m);
    w_f64(&w, o->nis); w_i64(&w, o->t_ns);
    if (w.bad) return EST_ERR_ENCODING;
    *len = w.len;
    return EST_OK;
}

#define PUB_ENC(NAME, TYPE)                                                              \
    est_status est_encode_##NAME(const TYPE *o, uint8_t *buf, size_t cap, size_t *len)   \
    {                                                                                    \
        if (!o || !buf || !len) return EST_ERR_NULL;                                     \
        return enc_##NAME(o, buf, cap, len);                                             \
    }
PUB_ENC(model, est_model)
PUB_ENC(observation, est_observation)
PUB_ENC(belief, est_belief)
PUB_ENC(prediction, est_prediction)
PUB_ENC(innovation, est_innovation)

/* ------------------------------------------------------------- decoders */

est_status est_decode_model(const uint8_t *buf, size_t len, est_model *out)
{
    if (!buf || !out) return EST_ERR_NULL;
    rd r = { buf, len, 0, EST_OK };
    est_status st = r_hdr(&r, EST_KIND_MODEL);
    if (st != EST_OK) return st;
    est_model t;
    memset(&t, 0, sizeof t);
    t.n = r_dim(&r); t.m = r_dim(&r);
    uint32_t e = r_u32(&r), mn = r_u32(&r);
    if (r.st == EST_OK && (e != (uint32_t)EST_ESTIMATOR_LINEAR_KALMAN ||
                           mn != (uint32_t)EST_UNCERTAINTY_GAUSSIAN_COVARIANCE)) r_fail(&r, EST_ERR_UNIT);
    t.estimator = (est_estimator_kind)e; t.meaning = (est_uncertainty_meaning)mn;
    if (r.st != EST_OK) return r.st;
    r_uarr(&r, t.state_unit, t.n); r_uarr(&r, t.obs_unit, t.m);
    r_farr(&r, t.F, (size_t)t.n * t.n); r_farr(&r, t.Q, (size_t)t.n * t.n);
    r_farr(&r, t.H, (size_t)t.m * t.n); r_farr(&r, t.R, (size_t)t.m * t.m);
    t.step_ns = r_i64(&r);
    st = r_done(&r);
    if (st != EST_OK) return st;
    st = est_check_model(&t);
    if (st != EST_OK) return st;
    *out = t;
    return EST_OK;
}

est_status est_decode_observation(const uint8_t *buf, size_t len, est_observation *out)
{
    if (!buf || !out) return EST_ERR_NULL;
    rd r = { buf, len, 0, EST_OK };
    est_status st = r_hdr(&r, EST_KIND_OBSERVATION);
    if (st != EST_OK) return st;
    est_observation t;
    memset(&t, 0, sizeof t);
    t.m = r_dim(&r);
    if (r.st != EST_OK) return r.st;
    r_uarr(&r, t.unit, t.m);
    r_farr(&r, t.z, t.m); r_farr(&r, t.R, (size_t)t.m * t.m);
    t.t_ns = r_i64(&r); t.seq = r_u64(&r);
    r_dig(&r, &t.source); r_dig(&r, &t.evidence);
    st = r_done(&r);
    if (st != EST_OK) return st;
    st = est_check_observation(&t);
    if (st != EST_OK) return st;
    *out = t;
    return EST_OK;
}

est_status est_decode_belief(const uint8_t *buf, size_t len, est_belief *out)
{
    if (!buf || !out) return EST_ERR_NULL;
    rd r = { buf, len, 0, EST_OK };
    est_status st = r_hdr(&r, EST_KIND_BELIEF);
    if (st != EST_OK) return st;
    est_belief t;
    memset(&t, 0, sizeof t);
    t.n = r_dim(&r);
    if (r.st != EST_OK) return r.st;
    r_uarr(&r, t.unit, t.n);
    r_farr(&r, t.x, t.n); r_farr(&r, t.P, (size_t)t.n * t.n);
    t.generation = r_u64(&r); t.t_ns = r_i64(&r);
    r_dig(&r, &t.model); r_dig(&r, &t.parent); r_dig(&r, &t.evidence_root);
    st = r_done(&r);
    if (st != EST_OK) return st;
    st = est_check_belief(&t);
    if (st != EST_OK) return st;
    *out = t;
    return EST_OK;
}

est_status est_decode_prediction(const uint8_t *buf, size_t len, est_prediction *out)
{
    if (!buf || !out) return EST_ERR_NULL;
    rd r = { buf, len, 0, EST_OK };
    est_status st = r_hdr(&r, EST_KIND_PREDICTION);
    if (st != EST_OK) return st;
    est_prediction t;
    memset(&t, 0, sizeof t);
    r_dig(&r, &t.prior); r_dig(&r, &t.model);
    t.horizon = r_u32(&r); t.generation = r_u64(&r); t.t_ns = r_i64(&r);
    t.n = r_dim(&r); t.m = r_dim(&r);
    if (r.st != EST_OK) return r.st;
    r_farr(&r, t.x, t.n); r_farr(&r, t.P, (size_t)t.n * t.n);
    r_farr(&r, t.y_mean, t.m); r_farr(&r, t.S, (size_t)t.m * t.m);
    t.has_control = r_u32(&r);
    if (r.st == EST_OK && t.has_control > 1u) r_fail(&r, EST_ERR_ENCODING);
    if (t.has_control) r_farr(&r, t.Bu, t.n);
    st = r_done(&r);
    if (st != EST_OK) return st;
    st = est_check_prediction(&t);
    if (st != EST_OK) return st;
    *out = t;
    return EST_OK;
}

est_status est_decode_innovation(const uint8_t *buf, size_t len, est_innovation *out)
{
    if (!buf || !out) return EST_ERR_NULL;
    rd r = { buf, len, 0, EST_OK };
    est_status st = r_hdr(&r, EST_KIND_INNOVATION);
    if (st != EST_OK) return st;
    est_innovation t;
    memset(&t, 0, sizeof t);
    r_dig(&r, &t.prediction); r_dig(&r, &t.observation); r_dig(&r, &t.model);
    t.m = r_dim(&r);
    if (r.st != EST_OK) return r.st;
    r_farr(&r, t.nu, t.m); r_farr(&r, t.S, (size_t)t.m * t.m);
    t.nis = r_f64(&r); t.t_ns = r_i64(&r);
    st = r_done(&r);
    if (st != EST_OK) return st;
    st = est_check_innovation(&t);
    if (st != EST_OK) return st;
    *out = t;
    return EST_OK;
}

/* -------------------------------------------------------------- digests */

static void hash_domain(const char *domain, const uint8_t *enc, size_t len, est_digest *out)
{
    sha256_ctx c;
    static const uint8_t zero = 0;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)domain, strlen(domain));
    sha256_update(&c, &zero, 1);
    sha256_update(&c, enc, len);
    sha256_final(&c, out->b);
}

#define PUB_DIGEST(NAME, TYPE, DOMAIN)                                        \
    est_status est_digest_##NAME(const TYPE *o, est_digest *out)              \
    {                                                                         \
        if (!o || !out) return EST_ERR_NULL;                                  \
        uint8_t tmp[EST_TMP_MAX];                                             \
        size_t len = 0;                                                       \
        est_status st = enc_##NAME(o, tmp, sizeof tmp, &len);                 \
        if (st != EST_OK) return st;                                          \
        hash_domain(DOMAIN, tmp, len, out);                                   \
        return EST_OK;                                                        \
    }
PUB_DIGEST(model, est_model, EST_DOMAIN_MODEL)
PUB_DIGEST(observation, est_observation, EST_DOMAIN_OBSERVATION)
PUB_DIGEST(belief, est_belief, EST_DOMAIN_BELIEF)
PUB_DIGEST(prediction, est_prediction, EST_DOMAIN_PREDICTION)
PUB_DIGEST(innovation, est_innovation, EST_DOMAIN_INNOVATION)

est_status est_evidence_root_extend(const est_digest *root, const est_digest *obs, est_digest *out)
{
    if (!root || !obs || !out) return EST_ERR_NULL;
    uint8_t tmp[2u * EST_DIGEST_SIZE];
    memcpy(tmp, root->b, EST_DIGEST_SIZE);
    memcpy(tmp + EST_DIGEST_SIZE, obs->b, EST_DIGEST_SIZE);
    est_digest r;
    hash_domain(EST_DOMAIN_BELIEF "-root", tmp, sizeof tmp, &r);
    *out = r;
    return EST_OK;
}
