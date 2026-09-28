/*
 * rx_semcomm.c -- semantic projections, their canonical encoding, delta
 * delivery and the receiver's view. See rx_semcomm.h.
 */
#include "rx_semcomm.h"

#include "../omega_core.h"
#include "../sha256.h"

#include <stdlib.h>
#include <string.h>

#define SC_MAGIC 0x314D4353u   /* 'SCM1' LE */

/* Record tags. */
enum {
    R_OBJECT   = 'O',   /* id u16, gen u32, type u32, resource u64, flags u8 */
    R_FIELD    = 'F',   /* id u16, field u8, version u64, value u64 */
    R_EVIDENCE = 'E',   /* id u16, field u8, crumb u64 */
    R_EV_DIG   = 'D',   /* id u16, field u8, crumb u64, digest[32] */
    R_DERIVED  = 'V',   /* slot u8, op u8, field u8, value u64, arg id u16, arg gen u32, n u32 */
    R_DROP_OBJ = 'X',   /* id u16, gen u32, reason u8 */
    R_DROP_FLD = 'Y'    /* id u16, field u8, reason u8 */
};

#define HDR_LEN 21u      /* magic u32, kind u8, receiver u32, seq u64, records u32 */

/* ---- helpers ---- */

static const RxSemType *schema_of(const RxSemSchema *s, uint32_t type) {
    if (!s) return NULL;
    for (uint32_t i = 0; i < s->n; i++)
        if (s->t[i].type == type) return &s->t[i];
    return NULL;
}

/* Validate READ on `resource` with any capability the receiver holds for it. */
static bool may_read(const RxWorld *w, const RxSemCaps *caps, uint64_t resource,
                     uint32_t *validations) {
    for (uint32_t i = 0; i < caps->n; i++) {
        if (caps->cap[i].resource != resource || !(caps->cap[i].rights & RX_RIGHT_READ)) continue;
        (*validations)++;
        if (rx_world_validate_cap(w, caps->cap[i].ref, caps->subject, resource, RX_RIGHT_READ,
                                  NULL) == RX_CAP_OK)
            return true;
    }
    return false;
}

static bool in_time(const RxInformationNeed *n, uint64_t crumb) {
    if (crumb < n->relevant_time_range.from_crumb) return false;
    return n->relevant_time_range.to_crumb == 0 || crumb <= n->relevant_time_range.to_crumb;
}

static bool type_wanted(const RxInformationNeed *n, uint32_t type) {
    if (n->n_types == 0) return true;
    for (uint32_t i = 0; i < n->n_types && i < SC_MAX_TYPES; i++)
        if (n->required_types[i] == type) return true;
    return false;
}

static bool object_wanted(const RxInformationNeed *n, RxObjRef r) {
    if (n->n_objects == 0) return true;
    for (uint32_t i = 0; i < n->n_objects && i < SC_MAX_RELEVANT; i++)
        if (n->relevant_objects[i].id == r.id && n->relevant_objects[i].generation == r.generation)
            return true;
    return false;
}

/* ---- projection ---- */

int rx_sem_project(RxWorld *w, const RxSemSchema *schema, const RxInformationNeed *need,
                   const RxSemCaps *caps, RxSemanticProjection *out) {
    if (!w || !need || !caps || !out || need->receiver != caps->subject) return SC_E_ARG;
    const RxSemOperation *op = &need->operation;
    if (op->kind < SC_OP_INSPECT || op->kind > SC_OP_MAX) return SC_E_ARG;
    if (op->kind != SC_OP_INSPECT && op->field >= RX_MAX_FIELDS) return SC_E_ARG;
    if (need->n_types > SC_MAX_TYPES || need->n_objects > SC_MAX_RELEVANT) return SC_E_ARG;

    out->n_objects = out->n_fields = out->n_derived = out->n_evidence = 0;
    out->withheld_authority = out->withheld_private = out->excluded_uncertain = 0;
    out->validations = 0;
    out->evidence_mode = need->evidence_requirement;

    const bool derived = op->kind != SC_OP_INSPECT;
    RxSemDerived d;
    memset(&d, 0, sizeof d);
    d.op = op->kind;
    d.field = derived ? op->field : 0;

    pthread_mutex_lock(&w->mu);
    for (uint32_t id = 0; id < RX_MAX_OBJECTS; id++) {
        const RxObject *o = &w->objects[id];
        if (!o->live) continue;
        RxObjRef ref = { id, o->generation };
        if (!type_wanted(need, o->type) || !object_wanted(need, ref)) continue;
        const RxSemType *T = schema_of(schema, o->type);

        /* Fields this operation needs from this object. */
        uint64_t want = derived ? RX_FIELD(op->field) : (op->fields & RX_ALL_FIELDS);
        uint64_t sel = 0;
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++)
            if ((want & RX_FIELD(f)) && in_time(need, o->field_writer[f])) sel |= RX_FIELD(f);
        if (!sel) continue;                             /* nothing of it is in scope */

        if (!may_read(w, caps, o->resource, &out->validations)) {
            out->withheld_authority++;
            continue;
        }
        if (T && T->uncertainty_field >= 0 && need->uncertainty_requirement.max_ppm &&
            o->field[T->uncertainty_field] > need->uncertainty_requirement.max_ppm) {
            out->excluded_uncertain++;
            continue;
        }
        uint64_t unc = 0;
        if (T && T->uncertainty_field >= 0 && need->uncertainty_requirement.deliver)
            unc = RX_FIELD((uint32_t)T->uncertainty_field);
        uint64_t priv = T ? T->private_mask : 0;
        if ((sel | unc) & priv) {
            if (!may_read(w, caps, rx_sem_private_resource(o->resource), &out->validations)) {
                for (uint32_t f = 0; f < RX_MAX_FIELDS; f++)
                    if (((sel | unc) & priv) & RX_FIELD(f)) out->withheld_private++;
                sel &= ~priv;
                unc &= ~priv;
                if (!sel) continue;
            }
        }

        uint32_t k = out->n_objects++;
        RxSemObject *so = &out->object_refs[k];
        so->ref = ref;
        so->type = o->type;
        so->resource = o->resource;
        so->conclusion = T && T->conclusion;
        so->mask = (derived ? 0 : sel) | unc;

        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++) {
            if (!(so->mask & RX_FIELD(f))) continue;
            RxSemField *sf = &out->fields[out->n_fields++];
            sf->obj = (uint16_t)k;
            sf->field = (uint8_t)f;
            sf->value = o->field[f];
            sf->version = o->field_version[f];
        }
        if (need->evidence_requirement != SC_EV_NONE) {
            /* Evidence for every field the operation used, delivered or folded. */
            for (uint32_t f = 0; f < RX_MAX_FIELDS; f++) {
                if (!((sel | unc) & RX_FIELD(f))) continue;
                RxSemEvidence *e = &out->evidence_refs[out->n_evidence++];
                e->obj = (uint16_t)k;
                e->field = (uint8_t)f;
                e->crumb = o->field_writer[f];
                memset(e->digest, 0, sizeof e->digest);
                if (need->evidence_requirement == SC_EV_DIGEST) {
                    const RxCrumb *c = rx_world_crumb(w, e->crumb);
                    if (c) memcpy(e->digest, c->digest, 32);
                }
            }
        }
        if (derived) {
            uint64_t v = o->field[op->field], r = 0;
            if (op->kind == SC_OP_SUM) {
                omega_eval_pure_binary_uint(OP_ADD, OVERFLOW_WRAP, 64, d.value, v, &r);
                d.value = r;
            } else {
                omega_eval_pure_binary_uint(OP_LESS_THAN, OVERFLOW_WRAP, 64, d.value, v, &r);
                if (d.n_inputs == 0 || r) { d.value = v; d.arg = ref; }
            }
            d.n_inputs++;
        }
    }
    pthread_mutex_unlock(&w->mu);
    if (derived) out->derived_values[out->n_derived++] = d;
    return SC_OK;
}

/* ---- buffers and records ---- */

void rx_sem_buf_reset(RxSemBuf *b) { b->len = 0; }

void rx_sem_buf_free(RxSemBuf *b) {
    free(b->buf);
    b->buf = NULL;
    b->len = b->cap = 0;
}

static int reserve(RxSemBuf *b, size_t more) {
    if (b->len + more <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + more) cap *= 2;
    uint8_t *nb = realloc(b->buf, cap);
    if (!nb) return -1;
    b->buf = nb;
    b->cap = cap;
    return 0;
}

static void put8(RxSemBuf *b, uint8_t v) { b->buf[b->len++] = v; }
static void put16(RxSemBuf *b, uint16_t v) { put8(b, (uint8_t)v); put8(b, (uint8_t)(v >> 8)); }
static void put32(RxSemBuf *b, uint32_t v) { put16(b, (uint16_t)v); put16(b, (uint16_t)(v >> 16)); }
static void put64(RxSemBuf *b, uint64_t v) { put32(b, (uint32_t)v); put32(b, (uint32_t)(v >> 32)); }

typedef struct {
    RxSemBuf *b;
    uint32_t records;
    int err;
} Enc;

static void enc_begin(Enc *e, RxSemBuf *b, uint32_t kind, uint32_t receiver, uint64_t seq) {
    e->b = b;
    e->records = 0;
    e->err = 0;
    rx_sem_buf_reset(b);
    if (reserve(b, HDR_LEN)) { e->err = 1; return; }
    put32(b, SC_MAGIC);
    put8(b, (uint8_t)kind);
    put32(b, receiver);
    put64(b, seq);
    put32(b, 0);
}

static void enc_end(Enc *e) {
    if (e->err) return;
    size_t at = HDR_LEN - 4u;
    for (int i = 0; i < 4; i++) e->b->buf[at + (size_t)i] = (uint8_t)(e->records >> (8 * i));
}

static int rec(Enc *e, size_t len) {
    if (e->err || reserve(e->b, len)) { e->err = 1; return -1; }
    e->records++;
    return 0;
}

static void rec_object(Enc *e, RxObjRef r, uint32_t type, uint64_t resource, bool conclusion) {
    if (rec(e, 20)) return;
    put8(e->b, R_OBJECT); put16(e->b, (uint16_t)r.id); put32(e->b, r.generation);
    put32(e->b, type); put64(e->b, resource); put8(e->b, conclusion ? 1 : 0);
}

static void rec_field(Enc *e, uint32_t id, uint32_t f, uint64_t version, uint64_t value) {
    if (rec(e, 20)) return;
    put8(e->b, R_FIELD); put16(e->b, (uint16_t)id); put8(e->b, (uint8_t)f);
    put64(e->b, version); put64(e->b, value);
}

static void rec_evidence(Enc *e, uint32_t id, uint32_t f, uint64_t crumb, const uint8_t *digest) {
    if (rec(e, digest ? 44 : 12)) return;
    put8(e->b, digest ? R_EV_DIG : R_EVIDENCE); put16(e->b, (uint16_t)id);
    put8(e->b, (uint8_t)f); put64(e->b, crumb);
    if (digest) { memcpy(e->b->buf + e->b->len, digest, 32); e->b->len += 32; }
}

static void rec_derived(Enc *e, uint32_t slot, const RxSemDerived *d) {
    if (rec(e, 22)) return;
    put8(e->b, R_DERIVED); put8(e->b, (uint8_t)slot); put8(e->b, (uint8_t)d->op);
    put8(e->b, (uint8_t)d->field); put64(e->b, d->value); put16(e->b, (uint16_t)d->arg.id);
    put32(e->b, d->arg.generation); put32(e->b, d->n_inputs);
}

static void rec_drop_obj(Enc *e, RxObjRef r, uint32_t reason) {
    if (rec(e, 8)) return;
    put8(e->b, R_DROP_OBJ); put16(e->b, (uint16_t)r.id); put32(e->b, r.generation);
    put8(e->b, (uint8_t)reason);
}

static void rec_drop_field(Enc *e, uint32_t id, uint32_t f, uint32_t reason) {
    if (rec(e, 5)) return;
    put8(e->b, R_DROP_FLD); put16(e->b, (uint16_t)id); put8(e->b, (uint8_t)f);
    put8(e->b, (uint8_t)reason);
}

/* ---- full state (broadcast baseline) ---- */

int rx_sem_encode_full_state(RxWorld *w, uint32_t receiver, uint64_t seq, RxSemBuf *out) {
    Enc e;
    enc_begin(&e, out, SC_MSG_FULL_STATE, receiver, seq);
    pthread_mutex_lock(&w->mu);
    for (uint32_t id = 0; id < RX_MAX_OBJECTS; id++) {
        const RxObject *o = &w->objects[id];
        if (!o->live) continue;
        rec_object(&e, (RxObjRef){ id, o->generation }, o->type, o->resource, false);
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++) rec_field(&e, id, f, o->field_version[f], o->field[f]);
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++) rec_evidence(&e, id, f, o->field_writer[f], NULL);
    }
    pthread_mutex_unlock(&w->mu);
    enc_end(&e);
    return e.err ? SC_E_FULL : SC_OK;
}

/* ---- projection and delta ---- */

static const uint8_t *ev_digest(const RxSemanticProjection *p, const RxSemEvidence *ev) {
    return p->evidence_mode == SC_EV_DIGEST ? ev->digest : NULL;
}

static void emit_all(Enc *e, const RxSemanticProjection *p) {
    uint32_t fi = 0, ei = 0;
    for (uint32_t k = 0; k < p->n_objects; k++) {
        const RxSemObject *so = &p->object_refs[k];
        rec_object(e, so->ref, so->type, so->resource, so->conclusion);
        for (; fi < p->n_fields && p->fields[fi].obj == k; fi++)
            rec_field(e, so->ref.id, p->fields[fi].field, p->fields[fi].version, p->fields[fi].value);
        for (; ei < p->n_evidence && p->evidence_refs[ei].obj == k; ei++)
            rec_evidence(e, so->ref.id, p->evidence_refs[ei].field, p->evidence_refs[ei].crumb,
                         ev_digest(p, &p->evidence_refs[ei]));
    }
    for (uint32_t i = 0; i < p->n_derived; i++) rec_derived(e, i, &p->derived_values[i]);
}

int rx_sem_encode_projection(const RxSemanticProjection *p, uint32_t receiver, uint64_t seq,
                             RxSemBuf *out) {
    Enc e;
    enc_begin(&e, out, SC_MSG_PROJECTION, receiver, seq);
    emit_all(&e, p);
    enc_end(&e);
    return e.err ? SC_E_FULL : SC_OK;
}

static void cursor_take(RxSemCursor *cur, const RxSemanticProjection *p) {
    memset(cur->present, 0, sizeof cur->present);
    memset(cur->mask, 0, sizeof cur->mask);
    memset(cur->crumb, 0, sizeof cur->crumb);
    uint32_t fi = 0, ei = 0;
    for (uint32_t k = 0; k < p->n_objects; k++) {
        uint32_t id = p->object_refs[k].ref.id;
        cur->present[id] = true;
        cur->gen[id] = p->object_refs[k].ref.generation;
        cur->mask[id] = p->object_refs[k].mask;
        for (; fi < p->n_fields && p->fields[fi].obj == k; fi++)
            cur->version[id][p->fields[fi].field] = p->fields[fi].version;
        for (; ei < p->n_evidence && p->evidence_refs[ei].obj == k; ei++)
            cur->crumb[id][p->evidence_refs[ei].field] = p->evidence_refs[ei].crumb;
    }
    cur->n_derived = p->n_derived;
    memcpy(cur->derived, p->derived_values, sizeof(RxSemDerived) * p->n_derived);
    cur->have = true;
}

static bool derived_eq(const RxSemDerived *a, const RxSemDerived *b) {
    return a->op == b->op && a->field == b->field && a->value == b->value &&
           a->arg.id == b->arg.id && a->arg.generation == b->arg.generation &&
           a->n_inputs == b->n_inputs;
}

int rx_sem_delta(RxSemCursor *cur, const RxSemanticProjection *p, uint32_t receiver,
                 uint64_t seq, RxSemReasonFn reason_of, void *reason_ctx, RxSemBuf *out,
                 RxSemDeltaStats *st) {
    if (!cur || !p || !out) return SC_E_ARG;
    RxSemDeltaStats s;
    memset(&s, 0, sizeof s);
    Enc e;
    if (!cur->have) {
        enc_begin(&e, out, SC_MSG_PROJECTION, receiver, seq);
        emit_all(&e, p);
        enc_end(&e);
        for (uint32_t k = 0; k < p->n_objects; k++) {
            s.new_objects++;
            if (p->object_refs[k].conclusion) s.new_conclusions++;
        }
        s.changed_fields = p->n_fields;
        s.new_evidence = p->n_evidence;
        s.derived_changed = p->n_derived;
        cursor_take(cur, p);
        if (st) *st = s;
        return e.err ? SC_E_FULL : SC_OK;
    }

    enc_begin(&e, out, SC_MSG_DELTA, receiver, seq);
    /* Index the new projection by object slot. */
    int32_t at[RX_MAX_OBJECTS];
    for (uint32_t i = 0; i < RX_MAX_OBJECTS; i++) at[i] = -1;
    for (uint32_t k = 0; k < p->n_objects; k++) at[p->object_refs[k].ref.id] = (int32_t)k;

    /* Invalidations first: a slot reused by a new generation drops the old one. */
    for (uint32_t id = 0; id < RX_MAX_OBJECTS; id++) {
        if (!cur->present[id]) continue;
        int32_t k = at[id];
        if (k >= 0 && p->object_refs[k].ref.generation == cur->gen[id]) continue;
        RxObjRef gone = { id, cur->gen[id] };
        rec_drop_obj(&e, gone, reason_of ? reason_of(reason_ctx, gone) : SC_INV_SCOPE);
        s.invalidated_objects++;
        cur->present[id] = false;
    }

    uint32_t fi = 0, ei = 0;
    for (uint32_t k = 0; k < p->n_objects; k++) {
        const RxSemObject *so = &p->object_refs[k];
        uint32_t id = so->ref.id;
        uint32_t f0 = fi, e0 = ei;
        while (fi < p->n_fields && p->fields[fi].obj == k) fi++;
        while (ei < p->n_evidence && p->evidence_refs[ei].obj == k) ei++;
        if (!cur->present[id]) {
            rec_object(&e, so->ref, so->type, so->resource, so->conclusion);
            s.new_objects++;
            if (so->conclusion) s.new_conclusions++;
            for (uint32_t j = f0; j < fi; j++) {
                rec_field(&e, id, p->fields[j].field, p->fields[j].version, p->fields[j].value);
                s.changed_fields++;
            }
            for (uint32_t j = e0; j < ei; j++) {
                const RxSemEvidence *ev = &p->evidence_refs[j];
                rec_evidence(&e, id, ev->field, ev->crumb, ev_digest(p, ev));
                s.new_evidence++;
            }
            continue;
        }
        /* Same object, same generation: fields that left, then fields that changed. */
        uint64_t ev_new = 0;
        for (uint32_t j = e0; j < ei; j++) ev_new |= RX_FIELD(p->evidence_refs[j].field);
        uint64_t ev_old = 0;
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++)
            if (cur->crumb[id][f]) ev_old |= RX_FIELD(f);
        uint64_t left = (cur->mask[id] & ~so->mask) | (ev_old & ~ev_new);
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++) {
            if (!(left & RX_FIELD(f))) continue;
            rec_drop_field(&e, id, f, SC_INV_SCOPE);
            s.invalidated_fields++;
        }
        for (uint32_t j = f0; j < fi; j++) {
            uint32_t f = p->fields[j].field;
            bool had = (cur->mask[id] & RX_FIELD(f)) && !(left & RX_FIELD(f));
            if (had && cur->version[id][f] == p->fields[j].version) continue;
            rec_field(&e, id, f, p->fields[j].version, p->fields[j].value);
            s.changed_fields++;
        }
        for (uint32_t j = e0; j < ei; j++) {
            const RxSemEvidence *ev = &p->evidence_refs[j];
            bool had = (ev_old & RX_FIELD(ev->field)) && !(left & RX_FIELD(ev->field));
            if (had && cur->crumb[id][ev->field] == ev->crumb) continue;
            rec_evidence(&e, id, ev->field, ev->crumb, ev_digest(p, ev));
            s.new_evidence++;
        }
    }
    for (uint32_t i = 0; i < p->n_derived; i++) {
        if (i < cur->n_derived && derived_eq(&cur->derived[i], &p->derived_values[i])) continue;
        rec_derived(&e, i, &p->derived_values[i]);
        s.derived_changed++;
    }
    enc_end(&e);
    cursor_take(cur, p);
    if (st) *st = s;
    return e.err ? SC_E_FULL : SC_OK;
}

size_t rx_sem_cursor_bytes(const RxSemCursor *cur) {
    /* Per delivered object: generation and mask; per tracked field: version
     * and evidence crumb. Derived values as stored. */
    size_t n = 0;
    for (uint32_t id = 0; id < RX_MAX_OBJECTS; id++) {
        if (!cur->present[id]) continue;
        n += 4 + 8;
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++) {
            if (cur->mask[id] & RX_FIELD(f)) n += 8;
            if (cur->crumb[id][f]) n += 8;
        }
    }
    return n + cur->n_derived * sizeof(RxSemDerived);
}

/* ---- receiver ---- */

void rx_sem_view_init(RxSemView *v) {
    memset(v, 0, sizeof *v);
}

void rx_sem_view_free(RxSemView *v) {
    free(v->digest);
    v->digest = NULL;
}

static void view_clear(RxSemView *v) {
    void *dg = v->digest;
    uint32_t inv = v->invalidations_seen, con = v->conclusions_seen;
    memset(v, 0, sizeof *v);
    v->digest = dg;
    v->invalidations_seen = inv;
    v->conclusions_seen = con;
}

static void drop_object(RxSemView *v, uint32_t id) {
    v->present[id] = false;
    v->mask[id] = 0;
    v->ev_mask[id] = 0;
    memset(v->value[id], 0, sizeof v->value[id]);
    memset(v->version[id], 0, sizeof v->version[id]);
    memset(v->crumb[id], 0, sizeof v->crumb[id]);
    if (v->digest) memset(v->digest[id], 0, sizeof v->digest[id]);
}

typedef struct { const uint8_t *p; size_t left; } Dec;

static int need(Dec *d, size_t n) { return d->left >= n ? 0 : -1; }
static uint8_t get8(Dec *d) { d->left--; return *d->p++; }
static uint16_t get16(Dec *d) { uint16_t v = get8(d); return (uint16_t)(v | ((uint16_t)get8(d) << 8)); }
static uint32_t get32(Dec *d) { uint32_t v = get16(d); return v | ((uint32_t)get16(d) << 16); }
static uint64_t get64(Dec *d) { uint64_t v = get32(d); return v | ((uint64_t)get32(d) << 32); }

int rx_sem_view_apply(RxSemView *v, const uint8_t *msg, size_t len) {
    Dec d = { msg, len };
    if (need(&d, HDR_LEN) || get32(&d) != SC_MAGIC) return SC_E_DECODE;
    uint32_t kind = get8(&d);
    (void)get32(&d);
    uint64_t seq = get64(&d);
    uint32_t n = get32(&d);
    if (kind < SC_MSG_FULL_STATE || kind > SC_MSG_DELTA) return SC_E_DECODE;
    if (kind != SC_MSG_DELTA) view_clear(v);
    for (uint32_t r = 0; r < n; r++) {
        if (need(&d, 1)) return SC_E_DECODE;
        uint32_t tag = get8(&d);
        switch (tag) {
        case R_OBJECT: {
            if (need(&d, 19)) return SC_E_DECODE;
            uint32_t id = get16(&d);
            if (id >= RX_MAX_OBJECTS) return SC_E_DECODE;
            drop_object(v, id);
            v->present[id] = true;
            v->gen[id] = get32(&d);
            v->type[id] = get32(&d);
            v->resource[id] = get64(&d);
            v->conclusion[id] = get8(&d) != 0;
            if (kind == SC_MSG_DELTA && v->conclusion[id]) v->conclusions_seen++;
            break;
        }
        case R_FIELD: {
            if (need(&d, 19)) return SC_E_DECODE;
            uint32_t id = get16(&d), f = get8(&d);
            if (id >= RX_MAX_OBJECTS || f >= RX_MAX_FIELDS || !v->present[id]) return SC_E_DECODE;
            v->version[id][f] = get64(&d);
            v->value[id][f] = get64(&d);
            v->mask[id] |= RX_FIELD(f);
            break;
        }
        case R_EVIDENCE:
        case R_EV_DIG: {
            if (need(&d, tag == R_EV_DIG ? 43 : 11)) return SC_E_DECODE;
            uint32_t id = get16(&d), f = get8(&d);
            if (id >= RX_MAX_OBJECTS || f >= RX_MAX_FIELDS || !v->present[id]) return SC_E_DECODE;
            v->crumb[id][f] = get64(&d);
            v->ev_mask[id] |= RX_FIELD(f);
            if (tag == R_EV_DIG) {
                if (!v->digest) {
                    v->digest = calloc(RX_MAX_OBJECTS, sizeof *v->digest);
                    if (!v->digest) return SC_E_FULL;
                }
                memcpy(v->digest[id][f], d.p, 32);
                d.p += 32;
                d.left -= 32;
            }
            break;
        }
        case R_DERIVED: {
            if (need(&d, 21)) return SC_E_DECODE;
            uint32_t slot = get8(&d);
            if (slot >= SC_MAX_DERIVED) return SC_E_DECODE;
            RxSemDerived *x = &v->derived[slot];
            x->op = get8(&d);
            x->field = get8(&d);
            x->value = get64(&d);
            x->arg.id = get16(&d);
            x->arg.generation = get32(&d);
            x->n_inputs = get32(&d);
            if (slot + 1 > v->n_derived) v->n_derived = slot + 1;
            break;
        }
        case R_DROP_OBJ: {
            if (need(&d, 7)) return SC_E_DECODE;
            uint32_t id = get16(&d), gen = get32(&d);
            (void)get8(&d);
            if (id >= RX_MAX_OBJECTS) return SC_E_DECODE;
            if (v->present[id] && v->gen[id] == gen) drop_object(v, id);
            v->invalidations_seen++;
            break;
        }
        case R_DROP_FLD: {
            if (need(&d, 4)) return SC_E_DECODE;
            uint32_t id = get16(&d), f = get8(&d);
            (void)get8(&d);
            if (id >= RX_MAX_OBJECTS || f >= RX_MAX_FIELDS) return SC_E_DECODE;
            v->mask[id] &= ~RX_FIELD(f);
            v->ev_mask[id] &= ~RX_FIELD(f);
            v->value[id][f] = v->version[id][f] = v->crumb[id][f] = 0;
            if (v->digest) memset(v->digest[id][f], 0, 32);
            v->invalidations_seen++;
            break;
        }
        default:
            return SC_E_DECODE;
        }
    }
    if (d.left != 0) return SC_E_DECODE;
    v->last_seq = seq;
    return SC_OK;
}

size_t rx_sem_view_bytes(const RxSemView *v) {
    size_t n = 0;
    for (uint32_t id = 0; id < RX_MAX_OBJECTS; id++) {
        if (!v->present[id]) continue;
        n += 4 + 4 + 8 + 1;                             /* gen, type, resource, flag */
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++) {
            if (v->mask[id] & RX_FIELD(f)) n += 16;     /* value + version */
            if (v->ev_mask[id] & RX_FIELD(f)) n += 8 + (v->digest ? 32 : 0);
        }
    }
    return n + v->n_derived * sizeof(RxSemDerived);
}

void rx_sem_view_digest(const RxSemView *v, uint8_t out[32]) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)"AIEN_SEM_VIEW_V1", 16);
    for (uint32_t id = 0; id < RX_MAX_OBJECTS; id++) {
        if (!v->present[id]) continue;
        uint64_t hdr[6] = { id, v->gen[id], v->type[id], v->resource[id], v->conclusion[id],
                            v->mask[id] | (v->ev_mask[id] << 32) };
        sha256_update(&c, (const uint8_t *)hdr, sizeof hdr);
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++) {
            if (v->mask[id] & RX_FIELD(f)) {
                uint64_t fv[2] = { v->value[id][f], v->version[id][f] };
                sha256_update(&c, (const uint8_t *)fv, sizeof fv);
            }
            if (v->ev_mask[id] & RX_FIELD(f)) {
                sha256_update(&c, (const uint8_t *)&v->crumb[id][f], 8);
                if (v->digest) sha256_update(&c, v->digest[id][f], 32);
            }
        }
    }
    for (uint32_t i = 0; i < v->n_derived; i++) {
        const RxSemDerived *x = &v->derived[i];
        uint64_t dv[6] = { x->op, x->field, x->value, x->arg.id, x->arg.generation, x->n_inputs };
        sha256_update(&c, (const uint8_t *)dv, sizeof dv);
    }
    sha256_final(&c, out);
}
