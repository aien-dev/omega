/* ANS record encodings and digests (see ans.h). One field-visitor per record
 * serves both directions, so encoder and decoder cannot drift apart.
 *
 * Layout of every record: "OANS", kind byte, version byte, two zero bytes,
 * then the fields in declaration order: u32/u64 little-endian, doubles as
 * binary64 bit patterns (-0.0 as +0.0), digests as 32 raw bytes, and the
 * embedded EST belief as u32 length + est_encode_belief bytes. */
#include "ans.h"

#include <math.h>
#include <string.h>

#include "sha256.h"

typedef struct {
    uint8_t *p;
    size_t cap, len, pos;
    int dec, bad;
} cd;

enum { K_REF = 1, K_ACTION, K_MEAS, K_STATE, K_VERDICT, K_PROMO };

static void c_raw(cd *c, void *buf, size_t n)
{
    if (c->bad) return;
    if (c->dec) {
        if (n > c->len - c->pos) { c->bad = 1; return; }
        memcpy(buf, c->p + c->pos, n);
        c->pos += n;
    } else {
        if (n > c->cap - c->len) { c->bad = 1; return; }
        memcpy(c->p + c->len, buf, n);
        c->len += n;
    }
}
static void c_u64(cd *c, uint64_t *v)
{
    uint8_t t[8] = {0};
    if (!c->dec) for (int i = 0; i < 8; i++) t[i] = (uint8_t)(*v >> (8 * i));
    c_raw(c, t, 8);
    if (c->dec && !c->bad) {
        uint64_t r = 0;
        for (int i = 0; i < 8; i++) r |= (uint64_t)t[i] << (8 * i);
        *v = r;
    }
}
static void c_u32(cd *c, uint32_t *v)
{
    uint8_t t[4] = {0};
    if (!c->dec) for (int i = 0; i < 4; i++) t[i] = (uint8_t)(*v >> (8 * i));
    c_raw(c, t, 4);
    if (c->dec && !c->bad) {
        uint32_t r = 0;
        for (int i = 0; i < 4; i++) r |= (uint32_t)t[i] << (8 * i);
        *v = r;
    }
}
static void c_i64(cd *c, int64_t *v)
{
    uint64_t u = (uint64_t)*v;
    c_u64(c, &u);
    if (c->dec) *v = (int64_t)u;
}
static void c_u8(cd *c, uint8_t *v) { c_raw(c, v, 1); }
static void c_f64(cd *c, double *d)
{
    uint64_t u = 0;
    if (!c->dec && *d != 0.0) memcpy(&u, d, 8);
    c_u64(c, &u);
    if (c->dec && !c->bad) memcpy(d, &u, 8);
}
static void c_farr(cd *c, double *a, size_t n) { for (size_t i = 0; i < n; i++) c_f64(c, &a[i]); }
static void c_dig(cd *c, ans_digest *d) { c_raw(c, d->b, EST_DIGEST_SIZE); }
static void c_hdr(cd *c, int kind)
{
    uint8_t h[8] = { 'O', 'A', 'N', 'S', (uint8_t)kind, (uint8_t)ANS_FORMAT_VERSION, 0, 0 };
    uint8_t g[8];
    memcpy(g, h, 8);
    c_raw(c, g, 8);
    if (c->dec && !c->bad && memcmp(g, h, 8) != 0) c->bad = 1;
}
static void c_belief(cd *c, est_belief *b)
{
    uint8_t tmp[EST_ENCODED_MAX];
    uint32_t l = 0;
    if (!c->dec) {
        size_t ll = 0;
        if (est_encode_belief(b, tmp, sizeof tmp, &ll) != EST_OK) { c->bad = 1; return; }
        l = (uint32_t)ll;
    }
    c_u32(c, &l);
    if (c->bad || l > sizeof tmp) { c->bad = 1; return; }
    c_raw(c, tmp, l);
    if (c->dec && !c->bad && est_decode_belief(tmp, l, b) != EST_OK) c->bad = 1;
}

static void v_ref(cd *c, ans_reference *r)
{
    c_hdr(c, K_REF);
    c_u32(c, &r->version);
    c_farr(c, r->limit, ANS_ATOMS);
    c_farr(c, r->q, ANS_ACT_KINDS);
    c_farr(c, r->r, ANS_SENSOR_KINDS);
    c_farr(c, r->tier_sigma, 3);
    c_f64(c, &r->nis_limit);
    c_f64(c, &r->promote_limit);
    c_u8(c, &r->frozen);
}
static void v_action(cd *c, ans_action *a)
{
    c_hdr(c, K_ACTION);
    uint32_t k = (uint32_t)a->kind;
    c_u32(c, &k);
    if (c->dec) a->kind = (ans_act_kind)k;
    c_dig(c, &a->id);
    c_f64(c, &a->reversibility_risk);
    for (uint32_t i = 0; i < ANS_ATOMS; i++) c_u32(c, &a->touches[i]);
}
static void v_meas(cd *c, ans_measurement *m)
{
    c_hdr(c, K_MEAS);
    uint32_t k = (uint32_t)m->cls;
    c_u32(c, &k);
    if (c->dec) m->cls = (ans_sensor_class)k;
    c_farr(c, m->z, ANS_DIM);
    c_dig(c, &m->source);
    c_dig(c, &m->evidence);
    c_dig(c, &m->reference);
    c_i64(c, &m->t_ns);
    c_u64(c, &m->seq);
}
static void v_state_body(cd *c, ans_state *s)
{
    c_dig(c, &s->goal);
    c_dig(c, &s->constraints);
    c_dig(c, &s->intent);
    c_dig(c, &s->evidence_root);
    c_belief(c, &s->drift);
    c_f64(c, &s->risk);
    c_dig(c, &s->provenance);
    c_u64(c, &s->generation);
    c_u64(c, &s->since_fix);
    c_dig(c, &s->last_reference_digest_seen);
    c_f64(c, &s->last_nis);
    c_u8(c, &s->disagreement);
    c_u64(c, &s->self_mod_steps);
    c_u64(c, &s->self_mod_unfixed);
}
static void v_state(cd *c, ans_state *s) { c_hdr(c, K_STATE); v_state_body(c, s); }
static void v_verdict(cd *c, ans_verdict *v)
{
    c_hdr(c, K_VERDICT);
    uint32_t t = (uint32_t)v->tier;
    c_u32(c, &t);
    if (c->dec) v->tier = (ans_tier)t;
    c_f64(c, &v->D);
    c_f64(c, &v->mean_norm);
    c_f64(c, &v->sigma);
    c_u64(c, &v->since_fix);
    c_u8(c, &v->disagreement);
    c_dig(c, &v->state);
    c_dig(c, &v->reference);
    c_dig(c, &v->digest);
}
static void v_promo(cd *c, ans_promotion_record *p)
{
    c_hdr(c, K_PROMO);
    uint32_t t = (uint32_t)p->result;
    c_u32(c, &t);
    if (c->dec) p->result = (ans_promo)t;
    c_f64(c, &p->D);
    c_dig(c, &p->candidate);
    c_dig(c, &p->reference);
    c_dig(c, &p->tests);
    c_dig(c, &p->state);
    c_dig(c, &p->digest);
}

/* ---- semantic checks shared by encode, decode and digest ---- */
static int fin(double d) { return isfinite(d); }
static int zero_dig(const ans_digest *d) { return est_digest_is_zero(d); }

static ans_status chk_action(const ans_action *a)
{
    if ((unsigned)a->kind >= ANS_ACT_KINDS) return ANS_ERR_RANGE;
    if (!fin(a->reversibility_risk)) return ANS_ERR_NONFINITE;
    if (a->reversibility_risk < 0.0 || a->reversibility_risk > 1.0) return ANS_ERR_RANGE;
    for (uint32_t i = 0; i < ANS_ATOMS; i++) if (a->touches[i] > 1u) return ANS_ERR_RANGE;
    if (zero_dig(&a->id)) return ANS_ERR_EVIDENCE;
    return ANS_OK;
}
static ans_status chk_meas(const ans_measurement *m)
{
    for (uint32_t i = 0; i < ANS_DIM; i++) if (!fin(m->z[i])) return ANS_ERR_NONFINITE;
    if ((unsigned)m->cls >= ANS_SENSOR_KINDS) return ANS_ERR_SENSOR;
    return ANS_OK;
}
static ans_status chk_state(const ans_state *s)
{
    if (est_check_belief(&s->drift) != EST_OK || s->drift.n != ANS_DIM) return ANS_ERR_EST;
    if (!fin(s->risk) || !fin(s->last_nis)) return ANS_ERR_NONFINITE;
    if (s->risk < 0.0 || s->risk > 1.0 || s->disagreement > 1u) return ANS_ERR_RANGE;
    return ANS_OK;
}
static ans_status chk_verdict(const ans_verdict *v)
{
    if ((unsigned)v->tier > ANS_TIER_HALT_REQUEST_FIX || v->disagreement > 1u) return ANS_ERR_RANGE;
    if (!fin(v->D) || !fin(v->mean_norm) || !fin(v->sigma)) return ANS_ERR_NONFINITE;
    return ANS_OK;
}
static ans_status chk_promo(const ans_promotion_record *p)
{
    if ((unsigned)p->result > ANS_PROMOTE_REFUSED_SELF_MOD) return ANS_ERR_RANGE;
    if (!fin(p->D)) return ANS_ERR_NONFINITE;
    return ANS_OK;
}

#define CODEC(NAME, TYPE, VISIT, CHECK)                                                        \
    ans_status ans_encode_##NAME(const TYPE *o, uint8_t *buf, size_t cap, size_t *len)         \
    {                                                                                          \
        if (!o || !buf || !len) return ANS_ERR_NULL;                                           \
        TYPE t = *o;                                                                           \
        ans_status st = CHECK(&t);                                                             \
        if (st != ANS_OK) return st;                                                           \
        cd c = { buf, cap, 0, 0, 0, 0 };                                                       \
        VISIT(&c, &t);                                                                         \
        if (c.bad) return ANS_ERR_ENCODING;                                                    \
        *len = c.len;                                                                          \
        return ANS_OK;                                                                         \
    }                                                                                          \
    ans_status ans_decode_##NAME(const uint8_t *buf, size_t len, TYPE *out)                    \
    {                                                                                          \
        if (!buf || !out) return ANS_ERR_NULL;                                                 \
        TYPE t;                                                                                \
        memset(&t, 0, sizeof t);                                                               \
        uint8_t tmp[ANS_ENCODED_MAX];                                                          \
        if (len > sizeof tmp) return ANS_ERR_ENCODING;                                         \
        memcpy(tmp, buf, len);                                                                 \
        cd c = { tmp, 0, len, 0, 1, 0 };                                                       \
        VISIT(&c, &t);                                                                         \
        if (c.bad || c.pos != len) return ANS_ERR_ENCODING;                                    \
        ans_status st = CHECK(&t);                                                             \
        if (st != ANS_OK) return st == ANS_ERR_REFERENCE ? st : ANS_ERR_ENCODING;              \
        *out = t;                                                                              \
        return ANS_OK;                                                                         \
    }

static ans_status chk_ref(const ans_reference *r)
{
    if (r->frozen > 1u) return ANS_ERR_RANGE;
    return ans_reference_validate(r);
}

CODEC(reference, ans_reference, v_ref, chk_ref)
CODEC(action, ans_action, v_action, chk_action)
CODEC(measurement, ans_measurement, v_meas, chk_meas)
CODEC(state, ans_state, v_state, chk_state)
CODEC(verdict, ans_verdict, v_verdict, chk_verdict)
CODEC(promotion_record, ans_promotion_record, v_promo, chk_promo)

/* ---- digests ---- */
static void hash_domain(const char *domain, const uint8_t *enc, size_t len, ans_digest *out)
{
    sha256_ctx c;
    static const uint8_t zero = 0;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)domain, strlen(domain));
    sha256_update(&c, &zero, 1);
    sha256_update(&c, enc, len);
    sha256_final(&c, out->b);
}

/* provenance' = SHA-256(ANS_DOMAIN_PROVENANCE || 0x00 || prev || tag || record digest) */
void ans_provenance_extend(const ans_digest *prev, const char *tag, const ans_digest *rec, ans_digest *out)
{
    sha256_ctx c;
    static const uint8_t zero = 0;
    ans_digest r;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)ANS_DOMAIN_PROVENANCE, strlen(ANS_DOMAIN_PROVENANCE));
    sha256_update(&c, &zero, 1);
    sha256_update(&c, prev->b, EST_DIGEST_SIZE);
    sha256_update(&c, (const uint8_t *)tag, strlen(tag));
    sha256_update(&c, rec->b, EST_DIGEST_SIZE);
    sha256_final(&c, r.b);
    *out = r;
}

ans_status ans_digest_reference(const ans_reference *ref, ans_digest *out)
{
    if (!ref || !out) return ANS_ERR_NULL;
    ans_reference t = *ref;
    t.frozen = 0; /* the digest names the content; frozen is the seal over it */
    uint8_t buf[ANS_ENCODED_MAX];
    size_t len = 0;
    ans_status st = ans_encode_reference(&t, buf, sizeof buf, &len);
    if (st != ANS_OK) return st;
    hash_domain(ANS_DOMAIN_REFERENCE, buf, len, out);
    return ANS_OK;
}

#define PUB_DIGEST(NAME, TYPE, DOMAIN)                                           \
    ans_status ans_digest_##NAME(const TYPE *o, ans_digest *out)                 \
    {                                                                            \
        if (!o || !out) return ANS_ERR_NULL;                                     \
        uint8_t buf[ANS_ENCODED_MAX];                                            \
        size_t len = 0;                                                          \
        ans_status st = ans_encode_##NAME(o, buf, sizeof buf, &len);             \
        if (st != ANS_OK) return st;                                             \
        hash_domain(DOMAIN, buf, len, out);                                      \
        return ANS_OK;                                                           \
    }
PUB_DIGEST(action, ans_action, ANS_DOMAIN_ACTION)
PUB_DIGEST(measurement, ans_measurement, ANS_DOMAIN_MEASUREMENT)
PUB_DIGEST(state, ans_state, ANS_DOMAIN_STATE)

/* verdict and promotion record carry their own digest field: it is zeroed in
 * the encoding that is hashed. */
ans_status ans_digest_verdict(const ans_verdict *v, ans_digest *out)
{
    if (!v || !out) return ANS_ERR_NULL;
    ans_verdict t = *v;
    memset(&t.digest, 0, sizeof t.digest);
    uint8_t buf[ANS_ENCODED_MAX];
    size_t len = 0;
    ans_status st = ans_encode_verdict(&t, buf, sizeof buf, &len);
    if (st != ANS_OK) return st;
    hash_domain(ANS_DOMAIN_VERDICT, buf, len, out);
    return ANS_OK;
}
ans_status ans_digest_promotion_record(const ans_promotion_record *p, ans_digest *out)
{
    if (!p || !out) return ANS_ERR_NULL;
    ans_promotion_record t = *p;
    memset(&t.digest, 0, sizeof t.digest);
    uint8_t buf[ANS_ENCODED_MAX];
    size_t len = 0;
    ans_status st = ans_encode_promotion_record(&t, buf, sizeof buf, &len);
    if (st != ANS_OK) return st;
    hash_domain(ANS_DOMAIN_PROMOTION, buf, len, out);
    return ANS_OK;
}
