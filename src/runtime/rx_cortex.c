/*
 * rx_cortex.c -- Cortex host reference. See rx_cortex.h.
 */
#include "rx_cortex.h"

#include "sha256.h"

#include <stdlib.h>
#include <string.h>

static void put_u64(sha256_ctx *c, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
    sha256_update(c, b, 8);
}

void cx_digest(const CxObject *o, const uint64_t *payload, uint8_t out[32]) {
    static const uint8_t domain[] = "AIEN.CORTEX.OBJECT.V1";
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, domain, sizeof domain - 1);
    put_u64(&c, o->id);
    put_u64(&c, ((uint64_t)o->cls << 32) | o->kind);
    put_u64(&c, o->subject);
    put_u64(&c, o->t);
    put_u64(&c, o->generation);
    put_u64(&c, ((uint64_t)o->branch << 32) | o->protect);
    put_u64(&c, o->tag);
    for (uint32_t i = 0; i < CX_LINKS; i++) put_u64(&c, o->links[i]);
    put_u64(&c, o->n);
    for (uint32_t i = 0; i < o->n; i++) put_u64(&c, payload[i]);
    sha256_final(&c, out);
}

static void chain_step(uint8_t chain[32], const uint8_t digest[32]) {
    uint8_t buf[64];
    memcpy(buf, chain, 32);
    memcpy(buf + 32, digest, 32);
    sha256_hash(buf, sizeof buf, chain);
}

int cx_init(CxStore *s, uint64_t n_subjects) {
    memset(s, 0, sizeof(*s));
    s->by_subject = calloc(n_subjects ? n_subjects : 1, sizeof(CxIdList));
    if (!s->by_subject) return CX_ERR_NOMEM;
    s->n_subjects = n_subjects;
    return CX_OK;
}

void cx_free(CxStore *s) {
    for (uint64_t i = 0; i < s->n_subjects; i++) free(s->by_subject[i].ids);
    free(s->by_subject);
    free(s->obj);
    free(s->arena);
    memset(s, 0, sizeof(*s));
}

static int grow(void **p, uint64_t *cap, uint64_t need, size_t elem) {
    if (need <= *cap) return 0;
    uint64_t c = *cap ? *cap : 1024;
    while (c < need) c *= 2;
    void *q = realloc(*p, c * elem);
    if (!q) return -1;
    *p = q;
    *cap = c;
    return 0;
}

int cx_append(CxStore *s, const CxHeader *h, const uint64_t *payload, uint32_t n, uint64_t *out_id) {
    if (!h || h->cls == 0 || h->cls >= CX_CLASS_END || h->subject >= s->n_subjects ||
        (n && !payload))
        return CX_ERR_ARG;
    CxIdList *l = &s->by_subject[h->subject];
    if (l->n && s->obj[l->ids[l->n - 1] - 1].t > h->t) return CX_ERR_ORDER;
    if (grow((void **)&s->obj, &s->cap, s->n + 1, sizeof(CxObject)) ||
        grow((void **)&s->arena, &s->arena_cap, s->arena_n + n, sizeof(uint64_t)))
        return CX_ERR_NOMEM;
    if (l->n == l->cap) {
        uint32_t c = l->cap ? l->cap * 2 : 64;
        uint64_t *q = realloc(l->ids, (size_t)c * sizeof(uint64_t));
        if (!q) return CX_ERR_NOMEM;
        l->ids = q;
        l->cap = c;
    }
    CxObject *o = &s->obj[s->n];
    memset(o, 0, sizeof(*o));
    o->id = s->n + 1;
    o->cls = h->cls;
    o->kind = h->kind;
    o->subject = h->subject;
    o->t = h->t;
    o->generation = h->generation;
    o->branch = h->branch;
    o->protect = h->protect & CX_PROT_ALL;
    o->tag = h->tag;
    memcpy(o->links, h->links, sizeof o->links);
    o->n = n;
    o->off = s->arena_n;
    if (n) memcpy(s->arena + s->arena_n, payload, (size_t)n * sizeof(uint64_t));
    s->arena_n += n;
    cx_digest(o, s->arena + o->off, o->digest);
    chain_step(s->chain, o->digest);
    l->ids[l->n++] = o->id;
    s->n++;
    if (out_id) *out_id = o->id;
    return CX_OK;
}

const CxObject *cx_get(const CxStore *s, uint64_t id) {
    return id >= 1 && id <= s->n ? &s->obj[id - 1] : NULL;
}

const uint64_t *cx_payload(const CxStore *s, const CxObject *o) { return s->arena + o->off; }

int cx_verify(const CxStore *s, uint64_t id) {
    const CxObject *o = cx_get(s, id);
    if (!o) return CX_ERR_ARG;
    uint8_t d[32];
    cx_digest(o, cx_payload(s, o), d);
    return memcmp(d, o->digest, 32) == 0 ? CX_OK : CX_ERR_DIGEST;
}

int cx_verify_chain(const CxStore *s) {
    uint8_t chain[32] = { 0 };
    for (uint64_t i = 0; i < s->n; i++) {
        uint8_t d[32];
        cx_digest(&s->obj[i], cx_payload(s, &s->obj[i]), d);
        if (memcmp(d, s->obj[i].digest, 32) != 0) return CX_ERR_DIGEST;
        chain_step(chain, d);
    }
    return memcmp(chain, s->chain, 32) == 0 ? CX_OK : CX_ERR_DIGEST;
}

static int matches(const CxObject *o, const CxFilter *f) {
    if (!f) return 1;
    if (f->cls && o->cls != f->cls) return 0;
    if (f->kind && o->kind != f->kind) return 0;
    if (!f->branch_any && o->branch != f->branch) return 0;
    return 1;
}

/* First index in the subject list with t >= t0. */
static uint32_t lower_bound(const CxStore *s, const CxIdList *l, uint64_t t0) {
    uint32_t lo = 0, hi = l->n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (s->obj[l->ids[mid] - 1].t < t0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

uint64_t cx_range(CxStore *s, uint64_t subject, uint64_t t0, uint64_t t1,
                  const CxFilter *f, CxVisit visit, void *user) {
    if (subject >= s->n_subjects) return 0;
    const CxIdList *l = &s->by_subject[subject];
    uint64_t visited = 0;
    for (uint32_t i = lower_bound(s, l, t0); i < l->n; i++) {
        const CxObject *o = &s->obj[l->ids[i] - 1];
        if (o->t > t1) break;
        s->examined++;
        if (!matches(o, f)) continue;
        visited++;
        if (visit && visit(user, o)) break;
    }
    return visited;
}

uint64_t cx_latest(CxStore *s, uint64_t subject, uint64_t t_max, const CxFilter *f) {
    if (subject >= s->n_subjects) return 0;
    const CxIdList *l = &s->by_subject[subject];
    uint32_t i = lower_bound(s, l, t_max == UINT64_MAX ? UINT64_MAX : t_max + 1);
    while (i > 0) {
        const CxObject *o = &s->obj[l->ids[--i] - 1];
        s->examined++;
        if (matches(o, f)) return o->id;
    }
    return 0;
}

uint64_t cx_count_before(const CxStore *s, uint64_t subject, uint64_t t0) {
    if (subject >= s->n_subjects) return 0;
    return lower_bound(s, &s->by_subject[subject], t0);
}

uint64_t cx_count_subject(const CxStore *s, uint64_t subject) {
    return subject < s->n_subjects ? s->by_subject[subject].n : 0;
}

void cx_tamper(CxStore *s, uint64_t id, uint32_t word, uint64_t value) {
    const CxObject *o = cx_get(s, id);
    if (o && word < o->n) s->arena[o->off + word] = value;
}
