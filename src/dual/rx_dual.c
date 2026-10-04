/* DUAL-0a: typed records, registry, validation, canonical encoding and
 * digests (ADR 0031 section 4). See rx_dual.h for the contract. */
#include "rx_dual.h"
#include "sha256.h"

#include <math.h>
#include <string.h>

/* ---------------------------------------------------------------- helpers */
int rx_dual_digest_is_zero(const RxDualDigest *d)
{
    uint8_t acc = 0;
    if (!d) return 1;
    for (size_t i = 0; i < RX_DUAL_DIGEST_SIZE; i++) acc |= d->b[i];
    return acc == 0;
}
int rx_dual_digest_eq(const RxDualDigest *a, const RxDualDigest *b)
{
    if (!a || !b) return 0;
    return memcmp(a->b, b->b, RX_DUAL_DIGEST_SIZE) == 0;
}
int rx_dual_unit_valid(RxDualUnit u)
{
    return (uint32_t)u > (uint32_t)RX_DUAL_UNIT_NONE && (uint32_t)u < (uint32_t)RX_DUAL_UNIT_MAX_;
}
static int fin(double d) { return isfinite(d) != 0; }
static int ids_ascending(const uint32_t *ids, uint32_t n)
{
    for (uint32_t i = 1; i < n; i++) if (ids[i] <= ids[i - 1]) return 0;
    return 1;
}

/* ----------------------------------------------------------------- writer */
typedef struct { uint8_t *p; size_t cap, pos; int overflow; } wr;
static void w_raw(wr *w, const uint8_t *src, size_t n)
{
    if (w->pos + n > w->cap) { w->overflow = 1; w->pos += n; return; }
    memcpy(w->p + w->pos, src, n);
    w->pos += n;
}
static void w_u8(wr *w, uint8_t v) { w_raw(w, &v, 1); }
static void w_u32(wr *w, uint32_t v)
{
    uint8_t b[4];
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (8 * i));
    w_raw(w, b, 4);
}
static void w_u64(wr *w, uint64_t v)
{
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
    w_raw(w, b, 8);
}
static void w_f64(wr *w, double d)
{
    uint64_t u = 0; /* +0.0 and -0.0 both encode as all-zero bits */
    if (d != 0.0) memcpy(&u, &d, 8);
    w_u64(w, u);
}
static void w_dig(wr *w, const RxDualDigest *d) { w_raw(w, d->b, RX_DUAL_DIGEST_SIZE); }
static void w_hdr(wr *w, RxDualKind k) { w_u8(w, (uint8_t)k); w_u8(w, (uint8_t)RX_DUAL_FORMAT_VERSION); }

/* ----------------------------------------------------------------- reader */
typedef struct { const uint8_t *p; size_t len, pos; RxDualStatus st; } rd;
static void r_fail(rd *r, RxDualStatus s) { if (r->st == RX_DUAL_OK) r->st = s; }
static int r_need(rd *r, size_t n)
{
    if (r->st != RX_DUAL_OK) return 0;
    if (r->pos + n > r->len) { r_fail(r, RX_DUAL_ERR_ENCODING); return 0; }
    return 1;
}
static uint8_t r_u8(rd *r)
{
    if (!r_need(r, 1)) return 0;
    return r->p[r->pos++];
}
static uint32_t r_u32(rd *r)
{
    uint32_t v = 0;
    if (!r_need(r, 4)) return 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)r->p[r->pos + (size_t)i] << (8 * i);
    r->pos += 4;
    return v;
}
static uint64_t r_u64(rd *r)
{
    uint64_t v = 0;
    if (!r_need(r, 8)) return 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)r->p[r->pos + (size_t)i] << (8 * i);
    r->pos += 8;
    return v;
}
static double r_f64(rd *r)
{
    uint64_t u = r_u64(r);
    double d;
    memcpy(&d, &u, 8);
    return d;
}
static void r_dig(rd *r, RxDualDigest *d)
{
    if (!r_need(r, RX_DUAL_DIGEST_SIZE)) { memset(d, 0, sizeof *d); return; }
    memcpy(d->b, r->p + r->pos, RX_DUAL_DIGEST_SIZE);
    r->pos += RX_DUAL_DIGEST_SIZE;
}
static void r_hdr(rd *r, RxDualKind want)
{
    uint8_t k = r_u8(r), v = r_u8(r);
    if (r->st != RX_DUAL_OK) return;
    if (k != (uint8_t)want) { r_fail(r, RX_DUAL_ERR_KIND); return; }
    if (v != (uint8_t)RX_DUAL_FORMAT_VERSION) r_fail(r, RX_DUAL_ERR_ENCODING);
}
static RxDualStatus r_end(rd *r)
{
    if (r->st != RX_DUAL_OK) return r->st;
    if (r->pos != r->len) return RX_DUAL_ERR_ENCODING; /* trailing bytes */
    return RX_DUAL_OK;
}

/* ------------------------------------------------------------- validation */
RxDualStatus rx_dual_check_resource(const RxDualResource *r)
{
    if (!r) return RX_DUAL_ERR_NULL;
    if (!rx_dual_unit_valid(r->unit)) return RX_DUAL_ERR_UNIT;
    if (!fin(r->scale)) return RX_DUAL_ERR_NONFINITE;
    if (!(r->scale > 0.0)) return RX_DUAL_ERR_SCALE;
    if (rx_dual_digest_is_zero(&r->contract)) return RX_DUAL_ERR_DIGEST;
    return RX_DUAL_OK;
}

RxDualStatus rx_dual_check_constraint(const RxDualConstraintState *s)
{
    if (!s) return RX_DUAL_ERR_NULL;
    if (!rx_dual_unit_valid(s->unit)) return RX_DUAL_ERR_UNIT;
    /* INVARIANT is unconstructible; UNDECLARED is never defaulted. */
    if (s->cls != RX_DUAL_CLASS_CAPACITY && s->cls != RX_DUAL_CLASS_SOFT) return RX_DUAL_ERR_CLASS;
    if (!fin(s->budget) || !fin(s->estimate) || !fin(s->uncertainty) || !fin(s->lambda))
        return RX_DUAL_ERR_NONFINITE;
    if (s->uncertainty < 0.0 || s->lambda < 0.0) return RX_DUAL_ERR_RANGE;
    if ((uint32_t)s->estimate_kind < 1u || (uint32_t)s->estimate_kind >= (uint32_t)RX_DUAL_EST_MAX_)
        return RX_DUAL_ERR_KIND;
    if ((uint32_t)s->lambda_state < 1u || (uint32_t)s->lambda_state >= (uint32_t)RX_DUAL_LAMBDA_MAX_)
        return RX_DUAL_ERR_RANGE;
    if (rx_dual_digest_is_zero(&s->budget_contract) || rx_dual_digest_is_zero(&s->estimate_ref) ||
        rx_dual_digest_is_zero(&s->controller_id) || rx_dual_digest_is_zero(&s->evidence_root))
        return RX_DUAL_ERR_DIGEST;
    if (s->estimate_kind == RX_DUAL_EST_MEASURED && rx_dual_digest_is_zero(&s->observation_ref))
        return RX_DUAL_ERR_DIGEST;
    if (rx_dual_digest_is_zero(&s->calibration_ref) && s->lambda_state == RX_DUAL_LAMBDA_FRESH)
        return RX_DUAL_ERR_CALIBRATION;
    if ((s->tick == 0) != (rx_dual_digest_is_zero(&s->parent) != 0)) return RX_DUAL_ERR_DIGEST;
    return RX_DUAL_OK;
}

RxDualStatus rx_dual_check_constraint_against(const RxDualConstraintState *s, const RxDualResource *r)
{
    RxDualStatus st;
    if (!s || !r) return RX_DUAL_ERR_NULL;
    if ((st = rx_dual_check_constraint(s)) != RX_DUAL_OK) return st;
    if ((st = rx_dual_check_resource(r)) != RX_DUAL_OK) return st;
    if (s->resource_id != r->resource_id) return RX_DUAL_ERR_RESOURCE;
    if (s->unit != r->unit) return RX_DUAL_ERR_UNIT;
    return RX_DUAL_OK;
}

RxDualStatus rx_dual_check_controller(const RxDualController *c)
{
    if (!c) return RX_DUAL_ERR_NULL;
    if (!fin(c->eta) || !fin(c->rho) || !fin(c->k_sigma)) return RX_DUAL_ERR_NONFINITE;
    if (c->eta < 0.0 || c->rho < 0.0 || c->rho > 1.0 || c->k_sigma < 0.0) return RX_DUAL_ERR_RANGE;
    if (c->max_age == 0 || c->cadence == 0) return RX_DUAL_ERR_RANGE;
    if (c->n == 0 || c->n > RX_DUAL_MAX_RESOURCES) return RX_DUAL_ERR_RANGE;
    if (!ids_ascending(c->resource_id, c->n)) return RX_DUAL_ERR_RESOURCE;
    for (uint32_t i = 0; i < c->n; i++) {
        if (!fin(c->lambda_max[i])) return RX_DUAL_ERR_NONFINITE;
        if (!(c->lambda_max[i] > 0.0)) return RX_DUAL_ERR_RANGE;
    }
    return RX_DUAL_OK;
}

RxDualStatus rx_dual_controller_lambda_max(const RxDualController *c, uint32_t resource_id, double *out)
{
    RxDualStatus st;
    if (!c || !out) return RX_DUAL_ERR_NULL;
    if ((st = rx_dual_check_controller(c)) != RX_DUAL_OK) return st;
    for (uint32_t i = 0; i < c->n; i++)
        if (c->resource_id[i] == resource_id) { *out = c->lambda_max[i]; return RX_DUAL_OK; }
    return RX_DUAL_ERR_RESOURCE;
}

RxDualStatus rx_dual_check_price_vector(const RxDualPriceVector *v)
{
    if (!v) return RX_DUAL_ERR_NULL;
    if (rx_dual_digest_is_zero(&v->context)) return RX_DUAL_ERR_DIGEST;
    if (v->n == 0 || v->n > RX_DUAL_MAX_RESOURCES) return RX_DUAL_ERR_RANGE;
    if (!ids_ascending(v->resource_id, v->n)) return RX_DUAL_ERR_RESOURCE;
    for (uint32_t i = 0; i < v->n; i++)
        if (rx_dual_digest_is_zero(&v->state[i])) return RX_DUAL_ERR_DIGEST;
    return RX_DUAL_OK;
}

RxDualStatus rx_dual_check_site(const RxDualSite *s)
{
    size_t i, end;
    if (!s) return RX_DUAL_ERR_NULL;
    if (s->name[0] == 0 || s->name[RX_DUAL_SITE_NAME_SIZE - 1] != 0) return RX_DUAL_ERR_SITE;
    for (end = 0; end < RX_DUAL_SITE_NAME_SIZE && s->name[end] != 0; end++)
        if (s->name[end] < 0x21 || s->name[end] > 0x7E) return RX_DUAL_ERR_SITE;
    for (i = end; i < RX_DUAL_SITE_NAME_SIZE; i++)
        if (s->name[i] != 0) return RX_DUAL_ERR_SITE; /* canonical NUL padding */
    if (rx_dual_digest_is_zero(&s->owner)) return RX_DUAL_ERR_DIGEST;
    if (s->max_alternatives == 0 || s->max_alternatives > RX_DUAL_MAX_CANDIDATES) return RX_DUAL_ERR_RANGE;
    return RX_DUAL_OK;
}

static RxDualStatus check_consequence(const RxDualConsequence *c)
{
    if (!rx_dual_unit_valid(c->unit)) return RX_DUAL_ERR_UNIT;
    if (!fin(c->predicted) || !fin(c->predicted_sd) || !fin(c->measured) || !fin(c->error))
        return RX_DUAL_ERR_NONFINITE;
    if (c->predicted_sd < 0.0) return RX_DUAL_ERR_RANGE;
    if (c->has_measured > 1u) return RX_DUAL_ERR_RANGE;
    if (c->has_measured) {
        double e;
        if (!(c->predicted_sd > 0.0)) return RX_DUAL_ERR_RANGE;
        e = (c->measured - c->predicted) / c->predicted_sd;
        if (!fin(e)) return RX_DUAL_ERR_NONFINITE;
        if (!(e == c->error)) return RX_DUAL_ERR_RANGE; /* error must be the recomputed value */
    } else if (c->measured != 0.0 || c->error != 0.0) {
        return RX_DUAL_ERR_RANGE;
    }
    return RX_DUAL_OK;
}

RxDualStatus rx_dual_check_recommendation(const RxDualRecommendation *r)
{
    if (!r) return RX_DUAL_ERR_NULL;
    if (r->authority != RX_DUAL_AUTHORITY_NONE) return RX_DUAL_ERR_AUTHORITY;
    if (rx_dual_digest_is_zero(&r->decision_site) || rx_dual_digest_is_zero(&r->price_vector))
        return RX_DUAL_ERR_DIGEST;
    if (r->n_candidates == 0 || r->n_candidates > RX_DUAL_MAX_CANDIDATES) return RX_DUAL_ERR_RANGE;
    for (uint32_t i = 0; i < r->n_candidates; i++)
        if (rx_dual_digest_is_zero(&r->candidates[i])) return RX_DUAL_ERR_DIGEST;
    if (r->actual >= r->n_candidates || r->recommended >= r->n_candidates) return RX_DUAL_ERR_RANGE;
    if (r->n_consequences > RX_DUAL_MAX_RESOURCES) return RX_DUAL_ERR_RANGE;
    for (uint32_t i = 0; i < r->n_consequences; i++) {
        RxDualStatus st = check_consequence(&r->c[i]);
        if (st != RX_DUAL_OK) return st;
        if (i > 0 && r->c[i].resource_id <= r->c[i - 1].resource_id) return RX_DUAL_ERR_RESOURCE;
    }
    return RX_DUAL_OK;
}

RxDualStatus rx_dual_check_outcome(const RxDualOutcome *o)
{
    if (!o) return RX_DUAL_ERR_NULL;
    if (rx_dual_digest_is_zero(&o->recommendation) || rx_dual_digest_is_zero(&o->before_vector) ||
        rx_dual_digest_is_zero(&o->after_vector))
        return RX_DUAL_ERR_DIGEST;
    if (o->generation_after < o->generation_before) return RX_DUAL_ERR_GENERATION;
    if (!fin(o->lambda_before) || !fin(o->lambda_after)) return RX_DUAL_ERR_NONFINITE;
    if (o->lambda_before < 0.0 || o->lambda_after < 0.0) return RX_DUAL_ERR_RANGE;
    return RX_DUAL_OK;
}

/* --------------------------------------------------------------- registry */
void rx_dual_registry_init(RxDualRegistry *reg) { if (reg) memset(reg, 0, sizeof *reg); }

RxDualStatus rx_dual_registry_add(RxDualRegistry *reg, const RxDualResource *r)
{
    RxDualStatus st;
    uint32_t pos;
    if (!reg) return RX_DUAL_ERR_NULL;
    if ((st = rx_dual_check_resource(r)) != RX_DUAL_OK) return st;
    for (pos = 0; pos < reg->n && reg->r[pos].resource_id < r->resource_id; pos++) {}
    if (pos < reg->n && reg->r[pos].resource_id == r->resource_id) return RX_DUAL_ERR_RESOURCE;
    if (reg->n >= RX_DUAL_MAX_RESOURCES) return RX_DUAL_ERR_FULL;
    for (uint32_t i = reg->n; i > pos; i--) reg->r[i] = reg->r[i - 1];
    reg->r[pos] = *r;
    reg->n++;
    return RX_DUAL_OK;
}

const RxDualResource *rx_dual_registry_find(const RxDualRegistry *reg, uint32_t resource_id)
{
    if (!reg) return NULL;
    for (uint32_t i = 0; i < reg->n; i++)
        if (reg->r[i].resource_id == resource_id) return &reg->r[i];
    return NULL;
}

/* --------------------------------------------------------- encode / decode */
static RxDualStatus finish(const wr *w, size_t *len)
{
    if (w->overflow) return RX_DUAL_ERR_ENCODING;
    if (len) *len = w->pos;
    return RX_DUAL_OK;
}

RxDualStatus rx_dual_encode_resource(const RxDualResource *r, uint8_t *buf, size_t cap, size_t *len)
{
    wr w = { buf, buf ? cap : 0, 0, 0 };
    RxDualStatus st = rx_dual_check_resource(r);
    if (st != RX_DUAL_OK) return st;
    if (!buf) return RX_DUAL_ERR_NULL;
    w_hdr(&w, RX_DUAL_KIND_RESOURCE);
    w_u32(&w, r->resource_id);
    w_u32(&w, (uint32_t)r->unit);
    w_f64(&w, r->scale);
    w_dig(&w, &r->contract);
    return finish(&w, len);
}
RxDualStatus rx_dual_decode_resource(const uint8_t *buf, size_t len, RxDualResource *out)
{
    rd r = { buf, len, 0, RX_DUAL_OK };
    RxDualStatus st;
    if (!buf || !out) return RX_DUAL_ERR_NULL;
    memset(out, 0, sizeof *out);
    r_hdr(&r, RX_DUAL_KIND_RESOURCE);
    out->resource_id = r_u32(&r);
    out->unit = (RxDualUnit)r_u32(&r);
    out->scale = r_f64(&r);
    r_dig(&r, &out->contract);
    if ((st = r_end(&r)) != RX_DUAL_OK) return st;
    return rx_dual_check_resource(out);
}

RxDualStatus rx_dual_encode_constraint(const RxDualConstraintState *s, uint8_t *buf, size_t cap, size_t *len)
{
    wr w = { buf, buf ? cap : 0, 0, 0 };
    RxDualStatus st = rx_dual_check_constraint(s);
    if (st != RX_DUAL_OK) return st;
    if (!buf) return RX_DUAL_ERR_NULL;
    w_hdr(&w, RX_DUAL_KIND_CONSTRAINT);
    w_u32(&w, s->resource_id);
    w_u32(&w, (uint32_t)s->unit);
    w_u32(&w, (uint32_t)s->cls);
    w_f64(&w, s->budget);
    w_dig(&w, &s->budget_contract);
    w_dig(&w, &s->observation_ref);
    w_dig(&w, &s->estimate_ref);
    w_u32(&w, (uint32_t)s->estimate_kind);
    w_f64(&w, s->estimate);
    w_f64(&w, s->uncertainty);
    w_dig(&w, &s->calibration_ref);
    w_f64(&w, s->lambda);
    w_u32(&w, (uint32_t)s->lambda_state);
    w_dig(&w, &s->controller_id);
    w_u64(&w, s->generation);
    w_u64(&w, s->tick);
    w_dig(&w, &s->evidence_root);
    w_dig(&w, &s->parent);
    return finish(&w, len);
}
RxDualStatus rx_dual_decode_constraint(const uint8_t *buf, size_t len, RxDualConstraintState *out)
{
    rd r = { buf, len, 0, RX_DUAL_OK };
    RxDualStatus st;
    if (!buf || !out) return RX_DUAL_ERR_NULL;
    memset(out, 0, sizeof *out);
    r_hdr(&r, RX_DUAL_KIND_CONSTRAINT);
    out->resource_id = r_u32(&r);
    out->unit = (RxDualUnit)r_u32(&r);
    out->cls = (RxDualClass)r_u32(&r);
    out->budget = r_f64(&r);
    r_dig(&r, &out->budget_contract);
    r_dig(&r, &out->observation_ref);
    r_dig(&r, &out->estimate_ref);
    out->estimate_kind = (RxDualEstimateKind)r_u32(&r);
    out->estimate = r_f64(&r);
    out->uncertainty = r_f64(&r);
    r_dig(&r, &out->calibration_ref);
    out->lambda = r_f64(&r);
    out->lambda_state = (RxDualLambdaState)r_u32(&r);
    r_dig(&r, &out->controller_id);
    out->generation = r_u64(&r);
    out->tick = r_u64(&r);
    r_dig(&r, &out->evidence_root);
    r_dig(&r, &out->parent);
    if ((st = r_end(&r)) != RX_DUAL_OK) return st;
    return rx_dual_check_constraint(out);
}

RxDualStatus rx_dual_encode_controller(const RxDualController *c, uint8_t *buf, size_t cap, size_t *len)
{
    wr w = { buf, buf ? cap : 0, 0, 0 };
    RxDualStatus st = rx_dual_check_controller(c);
    if (st != RX_DUAL_OK) return st;
    if (!buf) return RX_DUAL_ERR_NULL;
    w_hdr(&w, RX_DUAL_KIND_CONTROLLER);
    w_f64(&w, c->eta);
    w_f64(&w, c->rho);
    w_f64(&w, c->k_sigma);
    w_u64(&w, c->max_age);
    w_u64(&w, c->cadence);
    w_u32(&w, c->n);
    for (uint32_t i = 0; i < c->n; i++) { w_u32(&w, c->resource_id[i]); w_f64(&w, c->lambda_max[i]); }
    return finish(&w, len);
}
RxDualStatus rx_dual_decode_controller(const uint8_t *buf, size_t len, RxDualController *out)
{
    rd r = { buf, len, 0, RX_DUAL_OK };
    RxDualStatus st;
    if (!buf || !out) return RX_DUAL_ERR_NULL;
    memset(out, 0, sizeof *out);
    r_hdr(&r, RX_DUAL_KIND_CONTROLLER);
    out->eta = r_f64(&r);
    out->rho = r_f64(&r);
    out->k_sigma = r_f64(&r);
    out->max_age = r_u64(&r);
    out->cadence = r_u64(&r);
    out->n = r_u32(&r);
    if (r.st != RX_DUAL_OK) return r.st;
    if (out->n == 0 || out->n > RX_DUAL_MAX_RESOURCES) return RX_DUAL_ERR_RANGE;
    for (uint32_t i = 0; i < out->n; i++) { out->resource_id[i] = r_u32(&r); out->lambda_max[i] = r_f64(&r); }
    if ((st = r_end(&r)) != RX_DUAL_OK) return st;
    return rx_dual_check_controller(out);
}

RxDualStatus rx_dual_encode_price_vector(const RxDualPriceVector *v, uint8_t *buf, size_t cap, size_t *len)
{
    wr w = { buf, buf ? cap : 0, 0, 0 };
    RxDualStatus st = rx_dual_check_price_vector(v);
    if (st != RX_DUAL_OK) return st;
    if (!buf) return RX_DUAL_ERR_NULL;
    w_hdr(&w, RX_DUAL_KIND_PRICE_VECTOR);
    w_u64(&w, v->generation);
    w_dig(&w, &v->context);
    w_u32(&w, v->n);
    for (uint32_t i = 0; i < v->n; i++) { w_u32(&w, v->resource_id[i]); w_dig(&w, &v->state[i]); }
    return finish(&w, len);
}
RxDualStatus rx_dual_decode_price_vector(const uint8_t *buf, size_t len, RxDualPriceVector *out)
{
    rd r = { buf, len, 0, RX_DUAL_OK };
    RxDualStatus st;
    if (!buf || !out) return RX_DUAL_ERR_NULL;
    memset(out, 0, sizeof *out);
    r_hdr(&r, RX_DUAL_KIND_PRICE_VECTOR);
    out->generation = r_u64(&r);
    r_dig(&r, &out->context);
    out->n = r_u32(&r);
    if (r.st != RX_DUAL_OK) return r.st;
    if (out->n == 0 || out->n > RX_DUAL_MAX_RESOURCES) return RX_DUAL_ERR_RANGE;
    for (uint32_t i = 0; i < out->n; i++) { out->resource_id[i] = r_u32(&r); r_dig(&r, &out->state[i]); }
    if ((st = r_end(&r)) != RX_DUAL_OK) return st;
    return rx_dual_check_price_vector(out);
}

RxDualStatus rx_dual_encode_site(const RxDualSite *s, uint8_t *buf, size_t cap, size_t *len)
{
    wr w = { buf, buf ? cap : 0, 0, 0 };
    RxDualStatus st = rx_dual_check_site(s);
    if (st != RX_DUAL_OK) return st;
    if (!buf) return RX_DUAL_ERR_NULL;
    w_hdr(&w, RX_DUAL_KIND_SITE);
    w_u32(&w, s->site_id);
    w_raw(&w, s->name, RX_DUAL_SITE_NAME_SIZE);
    w_dig(&w, &s->owner);
    w_u32(&w, s->max_alternatives);
    return finish(&w, len);
}
RxDualStatus rx_dual_decode_site(const uint8_t *buf, size_t len, RxDualSite *out)
{
    rd r = { buf, len, 0, RX_DUAL_OK };
    RxDualStatus st;
    if (!buf || !out) return RX_DUAL_ERR_NULL;
    memset(out, 0, sizeof *out);
    r_hdr(&r, RX_DUAL_KIND_SITE);
    out->site_id = r_u32(&r);
    if (r_need(&r, RX_DUAL_SITE_NAME_SIZE)) { memcpy(out->name, r.p + r.pos, RX_DUAL_SITE_NAME_SIZE); r.pos += RX_DUAL_SITE_NAME_SIZE; }
    r_dig(&r, &out->owner);
    out->max_alternatives = r_u32(&r);
    if ((st = r_end(&r)) != RX_DUAL_OK) return st;
    return rx_dual_check_site(out);
}

RxDualStatus rx_dual_encode_recommendation(const RxDualRecommendation *rc, uint8_t *buf, size_t cap, size_t *len)
{
    wr w = { buf, buf ? cap : 0, 0, 0 };
    RxDualStatus st = rx_dual_check_recommendation(rc);
    if (st != RX_DUAL_OK) return st;
    if (!buf) return RX_DUAL_ERR_NULL;
    w_hdr(&w, RX_DUAL_KIND_RECOMMENDATION);
    w_dig(&w, &rc->decision_site);
    w_dig(&w, &rc->price_vector);
    w_u64(&w, rc->generation);
    w_u32(&w, rc->n_candidates);
    for (uint32_t i = 0; i < rc->n_candidates; i++) w_dig(&w, &rc->candidates[i]);
    w_u32(&w, rc->actual);
    w_u32(&w, rc->recommended);
    w_u32(&w, rc->n_consequences);
    for (uint32_t i = 0; i < rc->n_consequences; i++) {
        const RxDualConsequence *c = &rc->c[i];
        w_u32(&w, c->resource_id);
        w_u32(&w, (uint32_t)c->unit);
        w_f64(&w, c->predicted);
        w_f64(&w, c->predicted_sd);
        w_u32(&w, c->has_measured);
        w_f64(&w, c->measured);
        w_f64(&w, c->error);
    }
    w_u32(&w, rc->authority);
    return finish(&w, len);
}
RxDualStatus rx_dual_decode_recommendation(const uint8_t *buf, size_t len, RxDualRecommendation *out)
{
    rd r = { buf, len, 0, RX_DUAL_OK };
    RxDualStatus st;
    if (!buf || !out) return RX_DUAL_ERR_NULL;
    memset(out, 0, sizeof *out);
    r_hdr(&r, RX_DUAL_KIND_RECOMMENDATION);
    r_dig(&r, &out->decision_site);
    r_dig(&r, &out->price_vector);
    out->generation = r_u64(&r);
    out->n_candidates = r_u32(&r);
    if (r.st != RX_DUAL_OK) return r.st;
    if (out->n_candidates == 0 || out->n_candidates > RX_DUAL_MAX_CANDIDATES) return RX_DUAL_ERR_RANGE;
    for (uint32_t i = 0; i < out->n_candidates; i++) r_dig(&r, &out->candidates[i]);
    out->actual = r_u32(&r);
    out->recommended = r_u32(&r);
    out->n_consequences = r_u32(&r);
    if (r.st != RX_DUAL_OK) return r.st;
    if (out->n_consequences > RX_DUAL_MAX_RESOURCES) return RX_DUAL_ERR_RANGE;
    for (uint32_t i = 0; i < out->n_consequences; i++) {
        RxDualConsequence *c = &out->c[i];
        c->resource_id = r_u32(&r);
        c->unit = (RxDualUnit)r_u32(&r);
        c->predicted = r_f64(&r);
        c->predicted_sd = r_f64(&r);
        c->has_measured = r_u32(&r);
        c->measured = r_f64(&r);
        c->error = r_f64(&r);
    }
    out->authority = r_u32(&r);
    if ((st = r_end(&r)) != RX_DUAL_OK) return st;
    return rx_dual_check_recommendation(out);
}

RxDualStatus rx_dual_encode_outcome(const RxDualOutcome *o, uint8_t *buf, size_t cap, size_t *len)
{
    wr w = { buf, buf ? cap : 0, 0, 0 };
    RxDualStatus st = rx_dual_check_outcome(o);
    if (st != RX_DUAL_OK) return st;
    if (!buf) return RX_DUAL_ERR_NULL;
    w_hdr(&w, RX_DUAL_KIND_OUTCOME);
    w_dig(&w, &o->recommendation);
    w_dig(&w, &o->before_vector);
    w_dig(&w, &o->after_vector);
    w_u32(&w, o->target_resource_id);
    w_u64(&w, o->generation_before);
    w_u64(&w, o->generation_after);
    w_f64(&w, o->lambda_before);
    w_f64(&w, o->lambda_after);
    return finish(&w, len);
}
RxDualStatus rx_dual_decode_outcome(const uint8_t *buf, size_t len, RxDualOutcome *out)
{
    rd r = { buf, len, 0, RX_DUAL_OK };
    RxDualStatus st;
    if (!buf || !out) return RX_DUAL_ERR_NULL;
    memset(out, 0, sizeof *out);
    r_hdr(&r, RX_DUAL_KIND_OUTCOME);
    r_dig(&r, &out->recommendation);
    r_dig(&r, &out->before_vector);
    r_dig(&r, &out->after_vector);
    out->target_resource_id = r_u32(&r);
    out->generation_before = r_u64(&r);
    out->generation_after = r_u64(&r);
    out->lambda_before = r_f64(&r);
    out->lambda_after = r_f64(&r);
    if ((st = r_end(&r)) != RX_DUAL_OK) return st;
    return rx_dual_check_outcome(out);
}

/* ---------------------------------------------------------------- digests */
static void domain_digest(const char *domain, const uint8_t *enc, size_t len, RxDualDigest *out)
{
    sha256_ctx ctx;
    uint8_t zero = 0;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)domain, strlen(domain));
    sha256_update(&ctx, &zero, 1);
    sha256_update(&ctx, enc, len);
    sha256_final(&ctx, out->b);
}
#define DIGEST_FN(fname, type, encfn, domain)                                   \
    RxDualStatus fname(const type *x, RxDualDigest *out)                        \
    {                                                                           \
        uint8_t buf[RX_DUAL_ENCODED_MAX];                                       \
        size_t len = 0;                                                         \
        RxDualStatus st;                                                        \
        if (!out) return RX_DUAL_ERR_NULL;                                      \
        if ((st = encfn(x, buf, sizeof buf, &len)) != RX_DUAL_OK) return st;    \
        domain_digest(domain, buf, len, out);                                   \
        return RX_DUAL_OK;                                                      \
    }
DIGEST_FN(rx_dual_digest_resource, RxDualResource, rx_dual_encode_resource, RX_DUAL_DOMAIN_RESOURCE)
DIGEST_FN(rx_dual_digest_constraint, RxDualConstraintState, rx_dual_encode_constraint, RX_DUAL_DOMAIN_CONSTRAINT)
DIGEST_FN(rx_dual_digest_controller, RxDualController, rx_dual_encode_controller, RX_DUAL_DOMAIN_CONTROLLER)
DIGEST_FN(rx_dual_digest_price_vector, RxDualPriceVector, rx_dual_encode_price_vector, RX_DUAL_DOMAIN_PRICE_VECTOR)
DIGEST_FN(rx_dual_digest_site, RxDualSite, rx_dual_encode_site, RX_DUAL_DOMAIN_SITE)
DIGEST_FN(rx_dual_digest_recommendation, RxDualRecommendation, rx_dual_encode_recommendation, RX_DUAL_DOMAIN_RECOMMENDATION)
DIGEST_FN(rx_dual_digest_outcome, RxDualOutcome, rx_dual_encode_outcome, RX_DUAL_DOMAIN_OUTCOME)

/* ----------------------------------------------------------- constructors */
RxDualStatus rx_dual_price_vector_build(const RxDualConstraintState *const *states, uint32_t n,
                                        const RxDualDigest *context, RxDualPriceVector *out)
{
    RxDualStatus st;
    if (!states || !context || !out) return RX_DUAL_ERR_NULL;
    if (n == 0 || n > RX_DUAL_MAX_RESOURCES) return RX_DUAL_ERR_RANGE;
    if (rx_dual_digest_is_zero(context)) return RX_DUAL_ERR_DIGEST;
    memset(out, 0, sizeof *out);
    for (uint32_t i = 0; i < n; i++) {
        if (!states[i]) return RX_DUAL_ERR_NULL;
        if ((st = rx_dual_check_constraint(states[i])) != RX_DUAL_OK) return st;
        if (i > 0 && states[i]->resource_id <= states[i - 1]->resource_id) return RX_DUAL_ERR_RESOURCE;
        if (states[i]->generation != states[0]->generation) return RX_DUAL_ERR_GENERATION;
        out->resource_id[i] = states[i]->resource_id;
        if ((st = rx_dual_digest_constraint(states[i], &out->state[i])) != RX_DUAL_OK) return st;
    }
    out->n = n;
    out->generation = states[0]->generation;
    out->context = *context;
    return rx_dual_check_price_vector(out);
}

RxDualStatus rx_dual_consequence_set(RxDualConsequence *c, uint32_t resource_id, RxDualUnit unit,
                                     double predicted, double predicted_sd,
                                     int has_measured, double measured)
{
    if (!c) return RX_DUAL_ERR_NULL;
    memset(c, 0, sizeof *c);
    c->resource_id = resource_id;
    c->unit = unit;
    c->predicted = predicted;
    c->predicted_sd = predicted_sd;
    c->has_measured = has_measured ? 1u : 0u;
    if (c->has_measured) {
        c->measured = measured;
        c->error = (fin(predicted_sd) && predicted_sd > 0.0) ? (measured - predicted) / predicted_sd : 0.0;
    }
    return check_consequence(c);
}
