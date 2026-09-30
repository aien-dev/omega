/* BOOTSTRAP / REFERENCE: PATH-1. Byte laws and classification in rx_path.h.
 * No heap, no threads, no system calls, no runtime headers. */
#include "rx_path.h"
#include "sha256.h"
#include <string.h>

static const uint8_t path_magic[4] = {0x4F, 0x4D, 0x47, 0x30}; /* "OMG0" */

static void put_u16(uint8_t *b, uint16_t v) {
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)v;
}
static void put_u32(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)v;
}
static uint16_t get_u16(const uint8_t *b) {
    return (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
}
static uint32_t get_u32(const uint8_t *b) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

/* Byte sink: writes to a buffer, feeds a hash, or both. Callers check the size
 * against capacity before emitting, so the buffer bound is never reached. */
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t pos;
    sha256_ctx *hash;
} Emit;

static void emit(Emit *e, const void *data, size_t n) {
    if (e->buf && e->pos + n <= e->cap) memcpy(e->buf + e->pos, data, n);
    if (e->hash) sha256_update(e->hash, (const uint8_t *)data, n);
    e->pos += n;
}
static void emit_u8(Emit *e, uint8_t v) { emit(e, &v, 1); }
static void emit_u16(Emit *e, uint16_t v) { uint8_t b[2]; put_u16(b, v); emit(e, b, 2); }
static void emit_u32(Emit *e, uint32_t v) { uint8_t b[4]; put_u32(b, v); emit(e, b, 4); }
static void emit_id(Emit *e, const SemanticId *id) { emit(e, id->bytes, 32); }

/* Roles per family (spec 4.1): the high byte of a role is its family. */
static const uint8_t roles_in_family[6] = {0, 4, 4, 7, 7, 6};

static int valid_role(uint16_t family, uint16_t role) {
    if (family < STEP_FAM_RELATION || family > STEP_FAM_DISCOVERY) return 0;
    if ((role >> 8) != family) return 0;
    return (role & 0xFFu) >= 1u && (role & 0xFFu) <= roles_in_family[family];
}

static int check_step(const RxPathStep *s) {
    if (!valid_role(s->step_family, s->step_role)) return RX_PATH_ERR_MALFORMED;
    if (s->input_count > RX_PATH_MAX_INPUTS_PER_STEP) return RX_PATH_ERR_INPUT_LIMIT;
    if (s->output_count > RX_PATH_MAX_OUTPUTS_PER_STEP) return RX_PATH_ERR_OUTPUT_LIMIT;
    if (s->param_len > RX_PATH_MAX_PARAM_LEN) return RX_PATH_ERR_PARAM_LIMIT;
    return RX_PATH_OK;
}

/* Callers pass only steps that passed check_step, so no term can wrap. */
static size_t step_size(const RxPathStep *s) {
    return (size_t)8u + (size_t)32u * s->input_count + 32u + 2u +
           (size_t)32u * s->output_count + 4u + (size_t)s->param_len;
}

static int key_cmp(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b), n = la < lb ? la : lb;
    int c = memcmp(a, b, n);
    if (c) return c;
    return la < lb ? -1 : (la > lb ? 1 : 0);
}

static int constraint_cmp(const OmegaConstraint *a, const OmegaConstraint *b) {
    if (a->kind != b->kind) return a->kind < b->kind ? -1 : 1;
    uint16_t n = a->payload_len < b->payload_len ? a->payload_len : b->payload_len;
    int c = memcmp(a->payload, b->payload, n);
    if (c) return c;
    return a->payload_len < b->payload_len ? -1 : (a->payload_len > b->payload_len ? 1 : 0);
}

/* Logical step i. Iterative walk; each hop moves to a strictly shorter prefix
 * and the hop count is bounded, so corrupted parent links cannot loop. */
static const RxPathStep *resolve(const RxPath *p, uint32_t i) {
    for (unsigned hops = 0; p && hops <= RX_PATH_MAX_FORK_DEPTH; hops++) {
        if (p->parent && i < p->divergence_step_index) {
            p = p->parent;
            continue;
        }
        uint32_t t = i - p->divergence_step_index;
        if (t >= p->tail_count || !p->arena || p->tail[t] >= p->arena->used) return NULL;
        return &p->arena->steps[p->tail[t]];
    }
    return NULL;
}

uint32_t rx_path_step_count(const RxPath *p) {
    return p ? (uint32_t)p->divergence_step_index + p->tail_count : 0u;
}

static int fork_depth(const RxPath *p) {
    int d = 0;
    while (p && p->parent) {
        p = p->parent;
        if (++d > (int)RX_PATH_MAX_FORK_DEPTH) return -1;
    }
    return d;
}

void rx_path_arena_init(RxPathStepArena *arena) {
    if (arena) arena->used = 0;
}

int rx_path_init(RxPath *p, RxPathStepArena *arena, const SemanticId *start_anchor,
                 const SemanticId *end_anchor, const SemanticId *context) {
    if (!p || !arena || !start_anchor || !end_anchor || !context) return RX_PATH_ERR_ARG;
    memset(p, 0, sizeof(*p));
    p->arena = arena;
    p->start_anchor_id = *start_anchor;
    p->end_anchor_id = *end_anchor;
    p->context_id = *context;
    return RX_PATH_OK;
}

/* Shape check of the caller-visible fields and of every stored step. The path
 * and arena structs are public, so a record changed after insertion must not
 * reach the encoder, the hash or a comparison with out-of-range counts. Every
 * reader (encode, size, identity, equal) runs this first. */
static int check_shape(const RxPath *p) {
    if (p->attr_count > RX_PATH_MAX_ATTR_COUNT) return RX_PATH_ERR_ATTR_LIMIT;
    if (p->constraint_count > RX_PATH_MAX_CONSTRAINT_COUNT) return RX_PATH_ERR_CONST_LIMIT;
    if (p->tail_count > RX_PATH_MAX_STEPS) return RX_PATH_ERR_STEP_LIMIT;
    if (rx_path_step_count(p) > RX_PATH_MAX_STEPS) return RX_PATH_ERR_STEP_LIMIT;
    for (uint16_t i = 0; i < p->attr_count; i++) {
        const OmegaAttribute *a = &p->attributes[i];
        const char *z = memchr(a->key, 0, sizeof(a->key));
        if (!z || z == a->key || a->val_len > OMEGA_MAX_VAL_LEN) return RX_PATH_ERR_MALFORMED;
    }
    for (uint16_t i = 0; i < p->constraint_count; i++)
        if (p->constraints[i].payload_len > RX_PATH_MAX_CONSTRAINT_PAYLOAD)
            return RX_PATH_ERR_MALFORMED;
    uint32_t count = rx_path_step_count(p);
    for (uint32_t i = 0; i < count; i++) {
        const RxPathStep *s = resolve(p, i);
        if (!s) return RX_PATH_ERR_MALFORMED;
        int rc = check_step(s);
        if (rc) return rc;
    }
    return RX_PATH_OK;
}

int rx_path_encoded_size(const RxPath *p, size_t *size) {
    if (!p || !size) return RX_PATH_ERR_ARG;
    int rc = check_shape(p);
    if (rc) return rc;
    size_t n = RX_PATH_HEADER_BYTES;
    for (uint16_t i = 0; i < p->attr_count; i++)
        n += 1u + strlen(p->attributes[i].key) + 2u + p->attributes[i].val_len;
    n += 2u;
    for (uint16_t i = 0; i < p->constraint_count; i++)
        n += 4u + p->constraints[i].payload_len;
    n += 4u;
    uint32_t count = rx_path_step_count(p);
    for (uint32_t i = 0; i < count; i++) n += step_size(resolve(p, i));
    *size = n;
    return RX_PATH_OK;
}

int rx_path_set_end_anchor(RxPath *p, const SemanticId *end_anchor) {
    if (!p || !end_anchor) return RX_PATH_ERR_ARG;
    if (p->frozen) return RX_PATH_ERR_FROZEN;
    p->end_anchor_id = *end_anchor;
    return RX_PATH_OK;
}

int rx_path_add_attribute(RxPath *p, const char *key, const uint8_t *value,
                          uint16_t value_len) {
    if (!p || !key || (value_len && !value)) return RX_PATH_ERR_ARG;
    if (p->frozen) return RX_PATH_ERR_FROZEN;
    size_t klen = strlen(key);
    if (klen == 0 || klen > RX_PATH_MAX_KEY_BYTES || value_len > OMEGA_MAX_VAL_LEN)
        return RX_PATH_ERR_ARG;
    if (p->attr_count >= RX_PATH_MAX_ATTR_COUNT) return RX_PATH_ERR_ATTR_LIMIT;
    size_t size;
    int rc = rx_path_encoded_size(p, &size);
    if (rc) return rc;
    if (size + 1u + klen + 2u + value_len > RX_PATH_MAX_TOTAL_SERIALIZATION)
        return RX_PATH_ERR_BUFFER_OVERFLOW;
    uint16_t at = 0;
    while (at < p->attr_count && key_cmp(p->attributes[at].key, key) < 0) at++;
    if (at < p->attr_count && key_cmp(p->attributes[at].key, key) == 0)
        return RX_PATH_ERR_MALFORMED; /* duplicate key (SPEC-OMEGA-CANON-M4 rule 1) */
    memmove(&p->attributes[at + 1], &p->attributes[at],
            (size_t)(p->attr_count - at) * sizeof(p->attributes[0]));
    OmegaAttribute *a = &p->attributes[at];
    memset(a, 0, sizeof(*a));
    memcpy(a->key, key, klen);
    if (value_len) memcpy(a->value, value, value_len);
    a->val_len = value_len;
    p->attr_count++;
    return RX_PATH_OK;
}

int rx_path_add_constraint(RxPath *p, uint16_t kind, const uint8_t *payload,
                           uint16_t payload_len) {
    if (!p || (payload_len && !payload)) return RX_PATH_ERR_ARG;
    if (p->frozen) return RX_PATH_ERR_FROZEN;
    if (payload_len > RX_PATH_MAX_CONSTRAINT_PAYLOAD) return RX_PATH_ERR_ARG;
    if (p->constraint_count >= RX_PATH_MAX_CONSTRAINT_COUNT) return RX_PATH_ERR_CONST_LIMIT;
    size_t size;
    int rc = rx_path_encoded_size(p, &size);
    if (rc) return rc;
    if (size + 4u + payload_len > RX_PATH_MAX_TOTAL_SERIALIZATION)
        return RX_PATH_ERR_BUFFER_OVERFLOW;
    OmegaConstraint c;
    memset(&c, 0, sizeof(c));
    c.kind = kind;
    c.payload_len = payload_len;
    if (payload_len) memcpy(c.payload, payload, payload_len);
    /* Equal constraints are kept, as in the M4 canonicalizer (omega_canonical.c):
     * SPEC-OMEGA-CANON-M4 rule 3 sorts constraints and forbids no duplicates. */
    uint16_t at = 0;
    while (at < p->constraint_count && constraint_cmp(&p->constraints[at], &c) <= 0) at++;
    memmove(&p->constraints[at + 1], &p->constraints[at],
            (size_t)(p->constraint_count - at) * sizeof(p->constraints[0]));
    p->constraints[at] = c;
    p->constraint_count++;
    return RX_PATH_OK;
}

int rx_path_append(RxPath *p, const RxPathStep *step) {
    if (!p || !step || !p->arena) return RX_PATH_ERR_ARG;
    if (p->frozen) return RX_PATH_ERR_FROZEN;
    if (rx_path_step_count(p) >= RX_PATH_MAX_STEPS) return RX_PATH_ERR_STEP_LIMIT;
    int rc = check_step(step);
    if (rc) return rc;
    size_t size;
    rc = rx_path_encoded_size(p, &size);
    if (rc) return rc;
    if (size + step_size(step) > RX_PATH_MAX_TOTAL_SERIALIZATION)
        return RX_PATH_ERR_BUFFER_OVERFLOW;
    if (p->arena->used >= RX_PATH_ARENA_STEPS) return RX_PATH_ERR_CAPACITY;
    p->arena->steps[p->arena->used] = *step;
    p->tail[p->tail_count++] = p->arena->used++;
    return RX_PATH_OK;
}

int rx_path_get_step(const RxPath *p, uint32_t i, RxPathStep *out) {
    if (!p || !out || i >= rx_path_step_count(p)) return RX_PATH_ERR_ARG;
    const RxPathStep *s = resolve(p, i);
    if (!s) return RX_PATH_ERR_MALFORMED;
    *out = *s;
    return RX_PATH_OK;
}

static int check_lineage(const RxPath *p);

/* Canonical serialization (spec 6.1 and 6.2) into a sink. */
static int emit_path(const RxPath *p, Emit *e) {
    size_t size;
    int rc = rx_path_encoded_size(p, &size);
    if (rc) return rc;
    if (size > RX_PATH_MAX_TOTAL_SERIALIZATION) return RX_PATH_ERR_BUFFER_OVERFLOW;
    uint32_t count = rx_path_step_count(p);
    uint32_t steps_len = 0;
    for (uint32_t i = 0; i < count; i++) steps_len += (uint32_t)step_size(resolve(p, i));

    emit(e, path_magic, 4);
    emit_u8(e, RX_PATH_VERSION);
    emit_u8(e, RX_PATH_KIND);
    emit_u16(e, (uint16_t)count);
    emit_id(e, &p->start_anchor_id);
    emit_id(e, &p->end_anchor_id);
    emit_id(e, &p->context_id);
    emit_u16(e, p->attr_count);
    for (uint16_t i = 0; i < p->attr_count; i++) {
        const OmegaAttribute *a = &p->attributes[i];
        size_t klen = strlen(a->key);
        emit_u8(e, (uint8_t)klen);
        emit(e, a->key, klen);
        emit_u16(e, a->val_len);
        emit(e, a->value, a->val_len);
    }
    emit_u16(e, p->constraint_count);
    for (uint16_t i = 0; i < p->constraint_count; i++) {
        const OmegaConstraint *c = &p->constraints[i];
        emit_u16(e, c->kind);
        emit_u16(e, c->payload_len);
        emit(e, c->payload, c->payload_len);
    }
    emit_u32(e, steps_len);
    for (uint32_t i = 0; i < count; i++) {
        const RxPathStep *s = resolve(p, i);
        emit_u16(e, (uint16_t)i);
        emit_u16(e, s->step_family);
        emit_u16(e, s->step_role);
        emit_u16(e, s->input_count);
        for (uint16_t k = 0; k < s->input_count; k++) emit_id(e, &s->input_ids[k]);
        emit_id(e, &s->operator_id);
        emit_u16(e, s->output_count);
        for (uint16_t k = 0; k < s->output_count; k++) emit_id(e, &s->output_ids[k]);
        emit_u32(e, s->param_len);
        emit(e, s->param_bytes, s->param_len);
    }
    return RX_PATH_OK;
}

int rx_path_encode(const RxPath *p, uint8_t *out, size_t capacity, size_t *length) {
    if (!p || !out || !length) return RX_PATH_ERR_ARG;
    size_t size;
    int rc = rx_path_encoded_size(p, &size);
    if (rc) return rc;
    rc = check_lineage(p); /* never serialize a prefix that changed after fork */
    if (rc) return rc;
    if (size > RX_PATH_MAX_TOTAL_SERIALIZATION || size > capacity)
        return RX_PATH_ERR_BUFFER_OVERFLOW;
    Emit e = {out, capacity, 0, NULL};
    rc = emit_path(p, &e);
    if (rc) return rc;
    *length = e.pos;
    return RX_PATH_OK;
}

/* SEMANTIC_PATH_ID = SHA256("omega.path.v1" || 0x00 || canonical bytes). */
static int raw_semantic_id(const RxPath *p, SemanticId *out) {
    static const char tag[] = RX_PATH_SEMANTIC_TAG;
    sha256_ctx h;
    sha256_init(&h);
    Emit e = {NULL, 0, 0, &h};
    emit(&e, tag, sizeof(tag)); /* sizeof includes the 0x00 terminator */
    int rc = emit_path(p, &e);
    if (rc) return rc;
    sha256_final(&h, out->bytes);
    return RX_PATH_OK;
}

/* Every ancestor must still hash to the parent_path_id recorded at fork time. */
static int check_lineage(const RxPath *p) {
    if (fork_depth(p) < 0) return RX_PATH_ERR_CAPACITY;
    for (const RxPath *q = p; q->parent; q = q->parent) {
        SemanticId pid;
        int rc = raw_semantic_id(q->parent, &pid);
        if (rc) return rc;
        if (memcmp(pid.bytes, q->parent_path_id.bytes, 32) != 0) return RX_PATH_ERR_ID_MISMATCH;
    }
    return RX_PATH_OK;
}

int rx_path_semantic_id(const RxPath *p, SemanticId *out) {
    if (!p || !out) return RX_PATH_ERR_ARG;
    int rc = check_lineage(p);
    if (rc) return rc;
    return raw_semantic_id(p, out);
}

/* REALIZATION_ID = SHA256("omega.path.realization.v1" || 0x00 || semantic id ||
 * machine || engine profile || code digest || schedule u16 || ARGUS digest). */
int rx_path_realization_id(const RxPath *p, const RxPathRealization *r, SemanticId *out) {
    static const char tag[] = RX_PATH_REALIZATION_TAG;
    if (!p || !r || !out) return RX_PATH_ERR_ARG;
    SemanticId sem;
    int rc = rx_path_semantic_id(p, &sem);
    if (rc) return rc;
    sha256_ctx h;
    sha256_init(&h);
    Emit e = {NULL, 0, 0, &h};
    emit(&e, tag, sizeof(tag));
    emit_id(&e, &sem);
    emit_id(&e, &r->machine_id);
    emit_id(&e, &r->engine_profile_id);
    emit_id(&e, &r->code_digest);
    emit_u16(&e, r->schedule_kind);
    emit_id(&e, &r->argus_observation_digest);
    sha256_final(&h, out->bytes);
    return RX_PATH_OK;
}

int rx_path_fork(RxPath *child, RxPath *parent, uint16_t divergence_step_index) {
    if (!child || !parent || child == parent || !parent->arena) return RX_PATH_ERR_ARG;
    if (divergence_step_index > rx_path_step_count(parent)) return RX_PATH_ERR_ARG;
    int depth = fork_depth(parent);
    if (depth < 0 || depth + 1 > (int)RX_PATH_MAX_FORK_DEPTH) return RX_PATH_ERR_CAPACITY;
    /* The child storage must not be an ancestor of parent: overwriting it
     * would unfreeze a fork parent and close a cycle (spec 7.1 rule 3). */
    for (const RxPath *q = parent->parent; q; q = q->parent)
        if (q == child) return RX_PATH_ERR_ARG;
    SemanticId pid;
    int rc = rx_path_semantic_id(parent, &pid);
    if (rc) return rc;
    memset(child, 0, sizeof(*child));
    child->arena = parent->arena;
    child->parent = parent;
    child->parent_path_id = pid;
    child->divergence_step_index = divergence_step_index;
    child->start_anchor_id = parent->start_anchor_id;
    child->end_anchor_id = parent->end_anchor_id;
    child->context_id = parent->context_id;
    child->attr_count = parent->attr_count;
    child->constraint_count = parent->constraint_count;
    memcpy(child->attributes, parent->attributes, sizeof(child->attributes));
    memcpy(child->constraints, parent->constraints, sizeof(child->constraints));
    parent->frozen = 1; /* spec 7.1 rule 3: a fork parent is immutable */
    return RX_PATH_OK;
}

static int id_eq(const SemanticId *a, const SemanticId *b) {
    return memcmp(a->bytes, b->bytes, 32) == 0;
}

static int step_eq(const RxPathStep *a, const RxPathStep *b) {
    if (a->step_family != b->step_family || a->step_role != b->step_role ||
        a->input_count != b->input_count || a->output_count != b->output_count ||
        a->param_len != b->param_len || !id_eq(&a->operator_id, &b->operator_id))
        return 0;
    for (uint16_t k = 0; k < a->input_count; k++)
        if (!id_eq(&a->input_ids[k], &b->input_ids[k])) return 0;
    for (uint16_t k = 0; k < a->output_count; k++)
        if (!id_eq(&a->output_ids[k], &b->output_ids[k])) return 0;
    return memcmp(a->param_bytes, b->param_bytes, a->param_len) == 0;
}

int rx_path_equal(const RxPath *a, const RxPath *b, int *equal) {
    if (!a || !b || !equal) return RX_PATH_ERR_ARG;
    *equal = 0;
    int rc = check_shape(a);
    if (rc) return rc;
    rc = check_shape(b);
    if (rc) return rc;
    uint32_t n = rx_path_step_count(a);
    if (n != rx_path_step_count(b) || a->attr_count != b->attr_count ||
        a->constraint_count != b->constraint_count ||
        !id_eq(&a->start_anchor_id, &b->start_anchor_id) ||
        !id_eq(&a->end_anchor_id, &b->end_anchor_id) || !id_eq(&a->context_id, &b->context_id))
        return RX_PATH_OK;
    for (uint16_t i = 0; i < a->attr_count; i++) {
        const OmegaAttribute *x = &a->attributes[i], *y = &b->attributes[i];
        if (key_cmp(x->key, y->key) || x->val_len != y->val_len ||
            memcmp(x->value, y->value, x->val_len))
            return RX_PATH_OK;
    }
    for (uint16_t i = 0; i < a->constraint_count; i++)
        if (constraint_cmp(&a->constraints[i], &b->constraints[i])) return RX_PATH_OK;
    for (uint32_t i = 0; i < n; i++) {
        const RxPathStep *x = resolve(a, i), *y = resolve(b, i);
        if (!x || !y) return RX_PATH_ERR_MALFORMED;
        if (!step_eq(x, y)) return RX_PATH_OK;
    }
    *equal = 1;
    return RX_PATH_OK;
}

/* Bounded reader for the decoder. */
typedef struct {
    const uint8_t *b;
    size_t n;
    size_t pos;
} Reader;

static const uint8_t *take(Reader *r, size_t k) {
    if (k > r->n - r->pos) return NULL;
    const uint8_t *q = r->b + r->pos;
    r->pos += k;
    return q;
}
static int take_u16(Reader *r, uint16_t *v) {
    const uint8_t *q = take(r, 2);
    if (!q) return 0;
    *v = get_u16(q);
    return 1;
}
static int take_u32(Reader *r, uint32_t *v) {
    const uint8_t *q = take(r, 4);
    if (!q) return 0;
    *v = get_u32(q);
    return 1;
}
static int take_id(Reader *r, SemanticId *id) {
    const uint8_t *q = take(r, 32);
    if (!q) return 0;
    memcpy(id->bytes, q, 32);
    return 1;
}

static int decode_body(RxPath *p, Reader *r) {
    RxPathStepArena *arena = p->arena;
    const uint8_t *q = take(r, 4);
    if (!q || memcmp(q, path_magic, 4) != 0) return RX_PATH_ERR_MALFORMED;
    q = take(r, 2);
    if (!q || q[0] != RX_PATH_VERSION || q[1] != RX_PATH_KIND) return RX_PATH_ERR_MALFORMED;
    uint16_t count;
    if (!take_u16(r, &count)) return RX_PATH_ERR_MALFORMED;
    if (count > RX_PATH_MAX_STEPS) return RX_PATH_ERR_STEP_LIMIT;
    if (!take_id(r, &p->start_anchor_id) || !take_id(r, &p->end_anchor_id) ||
        !take_id(r, &p->context_id))
        return RX_PATH_ERR_MALFORMED;

    uint16_t n;
    if (!take_u16(r, &n)) return RX_PATH_ERR_MALFORMED;
    if (n > RX_PATH_MAX_ATTR_COUNT) return RX_PATH_ERR_ATTR_LIMIT;
    for (uint16_t i = 0; i < n; i++) {
        OmegaAttribute *a = &p->attributes[i];
        q = take(r, 1);
        if (!q || q[0] == 0 || q[0] > RX_PATH_MAX_KEY_BYTES) return RX_PATH_ERR_MALFORMED;
        uint8_t klen = q[0];
        q = take(r, klen);
        if (!q || memchr(q, 0, klen)) return RX_PATH_ERR_MALFORMED;
        memcpy(a->key, q, klen);
        a->key[klen] = '\0';
        if (!take_u16(r, &a->val_len) || a->val_len > OMEGA_MAX_VAL_LEN) return RX_PATH_ERR_MALFORMED;
        q = take(r, a->val_len);
        if (!q) return RX_PATH_ERR_MALFORMED;
        memcpy(a->value, q, a->val_len);
        if (i > 0 && key_cmp(p->attributes[i - 1].key, a->key) >= 0) return RX_PATH_ERR_MALFORMED;
        p->attr_count = (uint16_t)(i + 1);
    }

    if (!take_u16(r, &n)) return RX_PATH_ERR_MALFORMED;
    if (n > RX_PATH_MAX_CONSTRAINT_COUNT) return RX_PATH_ERR_CONST_LIMIT;
    for (uint16_t i = 0; i < n; i++) {
        OmegaConstraint *c = &p->constraints[i];
        if (!take_u16(r, &c->kind) || !take_u16(r, &c->payload_len) ||
            c->payload_len > RX_PATH_MAX_CONSTRAINT_PAYLOAD)
            return RX_PATH_ERR_MALFORMED;
        q = take(r, c->payload_len);
        if (!q) return RX_PATH_ERR_MALFORMED;
        memcpy(c->payload, q, c->payload_len);
        if (i > 0 && constraint_cmp(&p->constraints[i - 1], c) > 0) return RX_PATH_ERR_MALFORMED;
        p->constraint_count = (uint16_t)(i + 1);
    }

    uint32_t steps_len;
    if (!take_u32(r, &steps_len)) return RX_PATH_ERR_MALFORMED;
    if (steps_len != r->n - r->pos) return RX_PATH_ERR_MALFORMED;
    for (uint32_t i = 0; i < count; i++) {
        if (arena->used >= RX_PATH_ARENA_STEPS) return RX_PATH_ERR_CAPACITY;
        RxPathStep *s = &arena->steps[arena->used];
        memset(s, 0, sizeof(*s));
        uint16_t index;
        if (!take_u16(r, &index) || index != i) return RX_PATH_ERR_MALFORMED;
        if (!take_u16(r, &s->step_family) || !take_u16(r, &s->step_role))
            return RX_PATH_ERR_MALFORMED;
        if (!valid_role(s->step_family, s->step_role)) return RX_PATH_ERR_MALFORMED;
        if (!take_u16(r, &s->input_count)) return RX_PATH_ERR_MALFORMED;
        if (s->input_count > RX_PATH_MAX_INPUTS_PER_STEP) return RX_PATH_ERR_INPUT_LIMIT;
        for (uint16_t k = 0; k < s->input_count; k++)
            if (!take_id(r, &s->input_ids[k])) return RX_PATH_ERR_MALFORMED;
        if (!take_id(r, &s->operator_id) || !take_u16(r, &s->output_count))
            return RX_PATH_ERR_MALFORMED;
        if (s->output_count > RX_PATH_MAX_OUTPUTS_PER_STEP) return RX_PATH_ERR_OUTPUT_LIMIT;
        for (uint16_t k = 0; k < s->output_count; k++)
            if (!take_id(r, &s->output_ids[k])) return RX_PATH_ERR_MALFORMED;
        if (!take_u32(r, &s->param_len)) return RX_PATH_ERR_MALFORMED;
        if (s->param_len > RX_PATH_MAX_PARAM_LEN) return RX_PATH_ERR_PARAM_LIMIT;
        q = take(r, s->param_len);
        if (!q) return RX_PATH_ERR_MALFORMED;
        memcpy(s->param_bytes, q, s->param_len);
        p->tail[p->tail_count++] = arena->used++;
    }
    if (r->pos != r->n) return RX_PATH_ERR_MALFORMED; /* no trailing bytes */
    return RX_PATH_OK;
}

int rx_path_decode(RxPath *p, RxPathStepArena *arena, const uint8_t *bytes,
                   size_t length, const SemanticId *expected_id) {
    if (!p || !arena || !bytes || !expected_id) return RX_PATH_ERR_ARG;
    if (length > RX_PATH_MAX_TOTAL_SERIALIZATION) return RX_PATH_ERR_BUFFER_OVERFLOW;
    uint32_t saved = arena->used;
    memset(p, 0, sizeof(*p));
    p->arena = arena;
    Reader r = {bytes, length, 0};
    int rc = decode_body(p, &r);
    if (rc == RX_PATH_OK) {
        SemanticId got;
        rc = raw_semantic_id(p, &got);
        if (rc == RX_PATH_OK && !id_eq(&got, expected_id)) rc = RX_PATH_ERR_ID_MISMATCH;
    }
    if (rc != RX_PATH_OK) {
        arena->used = saved;
        memset(p, 0, sizeof(*p));
    }
    return rc;
}
