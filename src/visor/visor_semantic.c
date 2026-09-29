/*
 * visor_semantic.c -- Omega Visor V1, lane 1: read-only semantic inspection.
 * See visor_semantic.h for the contract. Nothing here writes to the graph.
 */
#include "visor_semantic.h"
#include "visor.h"
#include "omega_core.h"
#include "omega_canonical.h"
#include "sha256.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ names */

const char *visor_semantic_opcode_name(unsigned op) {
    static const char *const names[] = {
        "INVALID", "IDENTITY", "CONSTANT", "ADD", "SUB", "MUL", "DIV", "EQUAL",
        "LESS_THAN", "AND", "OR", "NOT", "SELECT", "CONCAT", "SLICE", "COMPILE"
    };
    return op < sizeof(names) / sizeof(names[0]) ? names[op] : "UNKNOWN";
}

const char *visor_semantic_overflow_name(unsigned ov) {
    static const char *const names[] = { "DEFAULT", "WRAP", "SATURATE", "FAIL_CLOSED" };
    return ov < sizeof(names) / sizeof(names[0]) ? names[ov] : "UNKNOWN";
}

const char *visor_semantic_relation_name(unsigned kind) {
    static const char *const names[] = {
        "INVALID", "EQUAL", "NOT_EQUAL", "LESS_THAN", "CONTAINS", "SUBSET_OF",
        "DEPENDS_ON", "DERIVED_FROM", "SATISFIES", "EQUIVALENT_TO"
    };
    return kind < sizeof(names) / sizeof(names[0]) ? names[kind] : "UNKNOWN";
}

const char *visor_semantic_constraint_name(unsigned kind) {
    static const char *const names[] = {
        "INVALID", "EQUALITY", "INEQUALITY", "RANGE", "TYPE", "LENGTH",
        "CONTAINMENT", "PRECONDITION", "POSTCONDITION", "INVARIANT"
    };
    return kind < sizeof(names) / sizeof(names[0]) ? names[kind] : "UNKNOWN";
}

const char *visor_semantic_shape_name(VisorShape s) {
    switch (s) {
        case VISOR_SHAPE_TYPE: return "TYPE";
        case VISOR_SHAPE_VALUE: return "VALUE";
        case VISOR_SHAPE_OPERATION: return "OPERATION";
        case VISOR_SHAPE_APPLY: return "APPLY";
        case VISOR_SHAPE_OPAQUE: return "OPAQUE";
        default: return "NONE";
    }
}

static void copy_name(char *dst, size_t n, const char *src) {
    snprintf(dst, n, "%s", src);
}

static bool id_is_zero(const SemanticId *id) {
    for (int i = 0; i < OMEGA_ID_BYTES; ++i)
        if (id->bytes[i]) return false;
    return true;
}

static bool id_eq(const SemanticId *a, const SemanticId *b) {
    return memcmp(a->bytes, b->bytes, OMEGA_ID_BYTES) == 0;
}

/* ------------------------------------------------ bounded structural parse */

typedef struct {
    VisorShape shape;
    TypePayload tp;
    ValuePayload vp;
    OperationPayload op;
    ApplyPayload ap;
    size_t canonical_len;
    uint8_t canonical_hash[32];
} Parsed;

/* Checks every bound BEFORE any field is trusted, decodes the kind payload by
 * memcpy, and recomputes (without storing) the canonical hash. 0 ok, -2 malformed. */
static int parse_object(const OmegaObject *o, Parsed *p, char *reason, size_t rn) {
    memset(p, 0, sizeof(*p));
    unsigned kind = (unsigned)o->kind;
    if (kind < KIND_VALUE || kind > KIND_PROOF) {
        snprintf(reason, rn, "invalid kind 0x%02x", kind);
        return -2;
    }
    if (o->attr_count > OMEGA_MAX_ATTRIBUTES) { snprintf(reason, rn, "attr_count %u exceeds %d", o->attr_count, OMEGA_MAX_ATTRIBUTES); return -2; }
    if (o->rel_count > OMEGA_MAX_RELATIONS) { snprintf(reason, rn, "rel_count %u exceeds %d", o->rel_count, OMEGA_MAX_RELATIONS); return -2; }
    if (o->const_count > OMEGA_MAX_CONSTRAINTS) { snprintf(reason, rn, "const_count %u exceeds %d", o->const_count, OMEGA_MAX_CONSTRAINTS); return -2; }
    if (o->payload_len > OMEGA_MAX_PAYLOAD_LEN) { snprintf(reason, rn, "payload_len %u exceeds %d", (unsigned)o->payload_len, OMEGA_MAX_PAYLOAD_LEN); return -2; }
    for (uint16_t i = 0; i < o->attr_count; ++i) {
        if (!memchr(o->attributes[i].key, 0, OMEGA_MAX_KEY_LEN)) { snprintf(reason, rn, "attribute %u key not terminated", i); return -2; }
        if (o->attributes[i].val_len > OMEGA_MAX_VAL_LEN) { snprintf(reason, rn, "attribute %u val_len %u too long", i, o->attributes[i].val_len); return -2; }
    }
    for (uint16_t i = 0; i < o->const_count; ++i) {
        if (o->constraints[i].payload_len > sizeof(o->constraints[i].payload)) {
            snprintf(reason, rn, "constraint %u payload_len %u too long", i, o->constraints[i].payload_len);
            return -2;
        }
    }

    switch (kind) {
        case KIND_TYPE:
            if (o->payload_len < sizeof(TypePayload)) { snprintf(reason, rn, "TYPE payload_len %u < %zu", (unsigned)o->payload_len, sizeof(TypePayload)); return -2; }
            memcpy(&p->tp, o->payload, sizeof(TypePayload));
            if ((unsigned)p->tp.tag == TYPE_INVALID || (unsigned)p->tp.tag > TYPE_EFFECT_RECEIPT_REF) { snprintf(reason, rn, "invalid type tag 0x%02x", (unsigned)p->tp.tag); return -2; }
            p->shape = VISOR_SHAPE_TYPE;
            break;
        case KIND_VALUE:
            if (o->payload_len < sizeof(ValuePayload)) { snprintf(reason, rn, "VALUE payload_len %u < %zu", (unsigned)o->payload_len, sizeof(ValuePayload)); return -2; }
            memcpy(&p->vp, o->payload, sizeof(ValuePayload));
            if (p->vp.byte_len > sizeof(p->vp.bytes)) { snprintf(reason, rn, "VALUE byte_len %u > %zu", p->vp.byte_len, sizeof(p->vp.bytes)); return -2; }
            p->shape = VISOR_SHAPE_VALUE;
            break;
        case KIND_OPERATION:
            if (o->payload_len >= sizeof(OperationPayload)) {
                memcpy(&p->op, o->payload, sizeof(OperationPayload));
                if ((unsigned)p->op.opcode == OP_INVALID || (unsigned)p->op.opcode > OP_COMPILE) { snprintf(reason, rn, "invalid opcode 0x%02x", (unsigned)p->op.opcode); return -2; }
                if ((unsigned)p->op.overflow == OVERFLOW_DEFAULT || (unsigned)p->op.overflow > OVERFLOW_FAIL_CLOSED) { snprintf(reason, rn, "invalid overflow policy 0x%02x", (unsigned)p->op.overflow); return -2; }
                if (p->op.arity > 4) { snprintf(reason, rn, "arity %u > 4", p->op.arity); return -2; }
                p->shape = VISOR_SHAPE_OPERATION;
            } else if (o->payload_len >= sizeof(ApplyPayload)) {
                memcpy(&p->ap, o->payload, sizeof(ApplyPayload));
                if (p->ap.operand_count > 4) { snprintf(reason, rn, "apply operand_count %u > 4", p->ap.operand_count); return -2; }
                p->shape = VISOR_SHAPE_APPLY;
            } else {
                snprintf(reason, rn, "OPERATION payload_len %u < %zu (apply) ", (unsigned)o->payload_len, sizeof(ApplyPayload));
                return -2;
            }
            break;
        default:
            p->shape = VISOR_SHAPE_OPAQUE;
            break;
    }

    uint8_t buf[4096];
    if (omega_canonical_encode(o, buf, sizeof(buf), &p->canonical_len) != 0) {
        snprintf(reason, rn, "canonical encoding failed");
        return -2;
    }
    sha256_hash(buf, p->canonical_len, p->canonical_hash);
    return 0;
}

static const OmegaObject *find(const OmegaGraph *g, const SemanticId *id) {
    return omega_graph_find_object_const(g, id);
}

/* Lookup + parse + id consistency. 0 ok, -1 missing, -2 malformed. */
static int load(const OmegaGraph *g, const SemanticId *id, const OmegaObject **obj, Parsed *p,
                char *reason, size_t rn) {
    if (!g || !id) return -1;
    const OmegaObject *o = find(g, id);
    if (!o) return -1;
    *obj = o;
    int rc = parse_object(o, p, reason, rn);
    if (rc != 0) return rc;
    if (memcmp(p->canonical_hash, o->id.bytes, OMEGA_ID_BYTES) != 0) {
        snprintf(reason, rn, "stored id does not match canonical encoding");
        return -2;
    }
    return 0;
}

/* -------------------------------------------------------------- type text */

static int type_text_of(const OmegaGraph *g, const SemanticId *type_id, char *out, size_t n, int depth) {
    const OmegaObject *o;
    Parsed p;
    char reason[VISOR_SEM_REASON_LEN];
    if (depth > 8 || load(g, type_id, &o, &p, reason, sizeof(reason)) != 0 || p.shape != VISOR_SHAPE_TYPE) {
        snprintf(out, n, "?");
        return -1;
    }
    const TypePayload *t = &p.tp;
    switch ((unsigned)t->tag) {
        case TYPE_UNIT: snprintf(out, n, "unit"); break;
        case TYPE_BOOL: snprintf(out, n, "bool"); break;
        case TYPE_UNSIGNED_INT: snprintf(out, n, "u%u", t->width); break;
        case TYPE_SIGNED_INT: snprintf(out, n, "i%u", t->width); break;
        case TYPE_BITVECTOR: snprintf(out, n, "bitvector<%u>", t->width); break;
        case TYPE_BYTE: snprintf(out, n, "byte"); break;
        case TYPE_SEQUENCE: {
            char elem[VISOR_SEM_TYPE_TEXT];
            type_text_of(g, &t->elem_type, elem, sizeof(elem), depth + 1);
            snprintf(out, n, "seq<%.64s,%u>", elem, (unsigned)t->length);
            break;
        }
        case TYPE_TUPLE: snprintf(out, n, "tuple"); break;
        case TYPE_ADDRESS: snprintf(out, n, "addr<%u>", t->width); break;
        case TYPE_RESOURCE: snprintf(out, n, "resource"); break;
        case TYPE_CAPABILITY_REF: snprintf(out, n, "cap_ref"); break;
        case TYPE_EFFECT_INTENT_REF: snprintf(out, n, "effect_intent_ref"); break;
        case TYPE_EFFECT_RECEIPT_REF: snprintf(out, n, "effect_receipt_ref"); break;
        default: snprintf(out, n, "?"); return -1;
    }
    return 0;
}

/* Integer width of a type usable by the u64 evaluator (uint/bitvector/byte, 1..64). */
static int int_width_of(const OmegaGraph *g, const SemanticId *type_id, uint16_t *w, bool *is_bool) {
    const OmegaObject *o;
    Parsed p;
    char reason[VISOR_SEM_REASON_LEN];
    if (load(g, type_id, &o, &p, reason, sizeof(reason)) != 0 || p.shape != VISOR_SHAPE_TYPE) return -1;
    *is_bool = false;
    switch ((unsigned)p.tp.tag) {
        case TYPE_UNSIGNED_INT:
        case TYPE_BITVECTOR: *w = p.tp.width; break;
        case TYPE_BYTE: *w = 8; break;
        case TYPE_BOOL: *w = 1; *is_bool = true; break;
        default: return -1;
    }
    if (*w < 1 || *w > 64) return -1;
    return 0;
}

/* Decode a parsed VALUE against its type. 0 ok, -1 not decodable. */
static int decode_value(const OmegaGraph *g, const ValuePayload *vp, uint64_t *out, bool *is_bool) {
    uint16_t w;
    if (int_width_of(g, &vp->type_id, &w, is_bool) != 0) return -1;
    if (*is_bool) {
        if (vp->byte_len != 1 || vp->bytes[0] > 1) return -1;
        *out = vp->bytes[0];
        return 0;
    }
    if (vp->byte_len != (uint16_t)((w + 7) / 8)) return -1;
    uint64_t v = 0;
    for (uint16_t i = 0; i < vp->byte_len; ++i) v = (v << 8) | vp->bytes[i];
    uint64_t mask = (w >= 64) ? ~0ULL : ((1ULL << w) - 1ULL);
    if (v & ~mask) return -1;
    *out = v;
    return 0;
}

/* Result type id per shape. 0 ok, -1 none. */
static int result_type_id(const OmegaGraph *g, const Parsed *p, SemanticId *out) {
    switch (p->shape) {
        case VISOR_SHAPE_VALUE: *out = p->vp.type_id; break;
        case VISOR_SHAPE_OPERATION: *out = p->op.output_type; break;
        case VISOR_SHAPE_APPLY: {
            const OmegaObject *o;
            Parsed op;
            char reason[VISOR_SEM_REASON_LEN];
            if (load(g, &p->ap.op_id, &o, &op, reason, sizeof(reason)) != 0 || op.shape != VISOR_SHAPE_OPERATION) return -1;
            *out = op.op.output_type;
            break;
        }
        default: return -1;
    }
    return id_is_zero(out) ? -1 : 0;
}

/* ----------------------------------------------------------- dependencies */

typedef struct {
    uint16_t count;
    SemanticId ids[VISOR_SEM_MAX_DEPS];
    char roles[VISOR_SEM_MAX_DEPS][VISOR_SEM_NAME_LEN];
} DepList;

static void dep_push(DepList *d, const SemanticId *id, const char *role) {
    if (id_is_zero(id) || d->count >= VISOR_SEM_MAX_DEPS) return;
    for (uint16_t i = 0; i < d->count; ++i)
        if (id_eq(&d->ids[i], id)) return;
    d->ids[d->count] = *id;
    copy_name(d->roles[d->count], VISOR_SEM_NAME_LEN, role);
    d->count++;
}

/* Requires a successfully parsed object. */
static void collect_deps(const OmegaObject *o, const Parsed *p, DepList *d) {
    char role[VISOR_SEM_NAME_LEN];
    d->count = 0;
    for (uint16_t i = 0; i < o->rel_count; ++i) {
        snprintf(role, sizeof(role), "rel:%s", visor_semantic_relation_name(o->relations[i].kind));
        dep_push(d, &o->relations[i].target_id, role);
    }
    switch (p->shape) {
        case VISOR_SHAPE_TYPE:
            if ((unsigned)p->tp.tag == TYPE_SEQUENCE) dep_push(d, &p->tp.elem_type, "elem");
            break;
        case VISOR_SHAPE_VALUE:
            dep_push(d, &p->vp.type_id, "type");
            break;
        case VISOR_SHAPE_OPERATION:
            dep_push(d, &p->op.type_id, "type");
            for (uint8_t i = 0; i < p->op.arity; ++i) {
                snprintf(role, sizeof(role), "input%u", i);
                dep_push(d, &p->op.input_types[i], role);
            }
            dep_push(d, &p->op.output_type, "output");
            break;
        case VISOR_SHAPE_APPLY:
            dep_push(d, &p->ap.op_id, "op");
            for (uint8_t i = 0; i < p->ap.operand_count; ++i) {
                snprintf(role, sizeof(role), "operand%u", i);
                dep_push(d, &p->ap.operands[i], role);
            }
            break;
        default:
            break;
    }
}

/* ---------------------------------------------------------------- inspect */

static bool printable(const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (b[i] < 0x20 || b[i] > 0x7e) return false;
    return true;
}

int visor_semantic_inspect(const OmegaGraph *g, const SemanticId *id, VisorObjectView *out) {
    if (!g || !id || !out) return -1;
    const OmegaObject *o = find(g, id);
    if (!o) return -1;

    memset(out, 0, sizeof(*out));
    out->id = o->id;
    visor_format_id(&o->id, out->id_text);
    out->kind = (uint8_t)o->kind;
    copy_name(out->kind_name, sizeof(out->kind_name), visor_kind_name(o->kind));

    Parsed p;
    const OmegaObject *dummy;
    int rc = load(g, id, &dummy, &p, out->malformed_reason, sizeof(out->malformed_reason));
    if (rc != 0) {
        out->status = -2;
        copy_name(out->shape_name, sizeof(out->shape_name), visor_semantic_shape_name(VISOR_SHAPE_NONE));
        return -2;
    }
    out->status = 0;
    out->shape = p.shape;
    copy_name(out->shape_name, sizeof(out->shape_name), visor_semantic_shape_name(p.shape));

    switch (p.shape) {
        case VISOR_SHAPE_TYPE:
            out->type_tag = (uint8_t)p.tp.tag;
            out->type_width = p.tp.width;
            out->type_length = p.tp.length;
            if ((unsigned)p.tp.tag == TYPE_SEQUENCE && !id_is_zero(&p.tp.elem_type)) {
                out->has_elem_type = true;
                out->elem_type = p.tp.elem_type;
            }
            type_text_of(g, &o->id, out->type_text, sizeof(out->type_text), 0);
            break;
        case VISOR_SHAPE_VALUE:
            out->value_byte_len = p.vp.byte_len;
            memcpy(out->value_bytes, p.vp.bytes, p.vp.byte_len);
            type_text_of(g, &p.vp.type_id, out->type_text, sizeof(out->type_text), 0);
            if (decode_value(g, &p.vp, &out->value_u64, &out->value_is_bool) == 0) out->has_u64 = true;
            else out->value_is_bool = false;
            break;
        case VISOR_SHAPE_OPERATION: {
            out->opcode = (uint8_t)p.op.opcode;
            copy_name(out->opcode_name, sizeof(out->opcode_name), visor_semantic_opcode_name(p.op.opcode));
            out->overflow = (uint8_t)p.op.overflow;
            copy_name(out->overflow_name, sizeof(out->overflow_name), visor_semantic_overflow_name(p.op.overflow));
            out->arity = p.op.arity;
            out->op_type_id = p.op.type_id;
            memcpy(out->input_types, p.op.input_types, sizeof(out->input_types));
            out->output_type = p.op.output_type;
            char sig[VISOR_SEM_TYPE_TEXT] = "fn(";
            size_t len = 3;
            for (uint8_t i = 0; i < p.op.arity; ++i) {
                char t[24];
                type_text_of(g, &p.op.input_types[i], t, sizeof(t), 0);
                len += (size_t)snprintf(sig + len, sizeof(sig) - len, "%s%s", i ? "," : "", t);
                if (len >= sizeof(sig)) len = sizeof(sig) - 1;
            }
            char t[24];
            type_text_of(g, &p.op.output_type, t, sizeof(t), 0);
            snprintf(sig + len, sizeof(sig) - len, ")->%s", t);
            copy_name(out->type_text, sizeof(out->type_text), sig);
            break;
        }
        case VISOR_SHAPE_APPLY: {
            out->apply_op = p.ap.op_id;
            out->operand_count = p.ap.operand_count;
            memcpy(out->operands, p.ap.operands, sizeof(out->operands));
            const OmegaObject *op_obj;
            Parsed op;
            char reason[VISOR_SEM_REASON_LEN];
            if (load(g, &p.ap.op_id, &op_obj, &op, reason, sizeof(reason)) == 0 && op.shape == VISOR_SHAPE_OPERATION) {
                out->opcode = (uint8_t)op.op.opcode;
                copy_name(out->opcode_name, sizeof(out->opcode_name), visor_semantic_opcode_name(op.op.opcode));
                out->overflow = (uint8_t)op.op.overflow;
                copy_name(out->overflow_name, sizeof(out->overflow_name), visor_semantic_overflow_name(op.op.overflow));
                out->arity = op.op.arity;
            }
            SemanticId rt;
            if (result_type_id(g, &p, &rt) == 0) type_text_of(g, &rt, out->type_text, sizeof(out->type_text), 0);
            else copy_name(out->type_text, sizeof(out->type_text), "?");
            break;
        }
        default:
            copy_name(out->type_text, sizeof(out->type_text), "-");
            break;
    }
    SemanticId rt;
    if (result_type_id(g, &p, &rt) == 0) {
        out->has_type_id = true;
        out->type_id = rt;
    }

    out->attr_count = o->attr_count;
    for (uint16_t i = 0; i < o->attr_count; ++i) {
        const OmegaAttribute *a = &o->attributes[i];
        copy_name(out->attrs[i].key, sizeof(out->attrs[i].key), a->key);
        out->attrs[i].len = a->val_len;
        memcpy(out->attrs[i].value, a->value, a->val_len);
        out->attrs[i].is_utf8 = printable(a->value, a->val_len);
    }
    out->rel_count = o->rel_count;
    for (uint16_t i = 0; i < o->rel_count; ++i) {
        out->rels[i].kind = o->relations[i].kind;
        copy_name(out->rels[i].kind_name, sizeof(out->rels[i].kind_name), visor_semantic_relation_name(o->relations[i].kind));
        out->rels[i].target = o->relations[i].target_id;
        out->rels[i].resolved = find(g, &o->relations[i].target_id) != NULL;
    }
    out->const_count = o->const_count;
    for (uint16_t i = 0; i < o->const_count; ++i) {
        out->consts[i].kind = o->constraints[i].kind;
        copy_name(out->consts[i].kind_name, sizeof(out->consts[i].kind_name), visor_semantic_constraint_name(o->constraints[i].kind));
        out->consts[i].len = o->constraints[i].payload_len;
        memcpy(out->consts[i].payload, o->constraints[i].payload, o->constraints[i].payload_len);
    }

    out->canonical_len = p.canonical_len;
    {
        SemanticId h;
        memcpy(h.bytes, p.canonical_hash, OMEGA_ID_BYTES);
        omega_hex_semantic_id(&h, out->canonical_sha256);
        out->canonical_id_matches = id_eq(&h, &o->id);
    }

    DepList d;
    collect_deps(o, &p, &d);
    out->dep_count = d.count;
    for (uint16_t i = 0; i < d.count; ++i) {
        out->deps[i].id = d.ids[i];
        copy_name(out->deps[i].role, sizeof(out->deps[i].role), d.roles[i]);
        out->deps[i].resolved = find(g, &d.ids[i]) != NULL;
        if (!out->deps[i].resolved) out->dangling_count++;
    }
    return 0;
}

/* -------------------------------------------------------------- formatting */

typedef struct {
    char *p;
    size_t n;
    size_t len;
    bool overflow;
} Buf;

static void bprintf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void bprintf(Buf *b, const char *fmt, ...) {
    if (b->overflow) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(b->p + b->len, b->n - b->len, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= b->n - b->len) {
        b->overflow = true;
        b->p[b->len] = '\0';
        return;
    }
    b->len += (size_t)w;
}

static void bhex(Buf *b, const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n && !b->overflow; ++i) bprintf(b, "%02x", d[i]);
}

static void bid(Buf *b, const SemanticId *id) {
    char t[72];
    visor_format_id(id, t);
    bprintf(b, "%s", t);
}

static int bfinish(Buf *b) {
    if (b->overflow) {
        if (b->n) b->p[0] = '\0';
        return -1;
    }
    return (int)b->len;
}

int visor_semantic_format_text(const VisorObjectView *v, char *out, size_t n) {
    if (!v || !out || n == 0) return -1;
    Buf b = { out, n, 0, false };
    out[0] = '\0';
    bprintf(&b, "id: %s\n", v->id_text);
    bprintf(&b, "kind: %s\n", v->kind_name);
    if (v->status != 0) {
        bprintf(&b, "status: MALFORMED (%s)\n", v->malformed_reason);
        return bfinish(&b);
    }
    bprintf(&b, "status: ok\n");
    bprintf(&b, "shape: %s\n", v->shape_name);
    bprintf(&b, "type: %s\n", v->type_text);
    if (v->has_type_id) { bprintf(&b, "type_id: "); bid(&b, &v->type_id); bprintf(&b, "\n"); }
    switch (v->shape) {
        case VISOR_SHAPE_TYPE:
            bprintf(&b, "tag: %u\nwidth: %u\nlength: %u\n", v->type_tag, v->type_width, (unsigned)v->type_length);
            if (v->has_elem_type) { bprintf(&b, "elem_type: "); bid(&b, &v->elem_type); bprintf(&b, "\n"); }
            break;
        case VISOR_SHAPE_VALUE:
            bprintf(&b, "value_bytes: ");
            bhex(&b, v->value_bytes, v->value_byte_len);
            bprintf(&b, "\n");
            if (v->has_u64) {
                if (v->value_is_bool) bprintf(&b, "value: %s\n", v->value_u64 ? "true" : "false");
                else bprintf(&b, "value: %llu\n", (unsigned long long)v->value_u64);
            } else {
                bprintf(&b, "value: (not decodable as u64)\n");
            }
            break;
        case VISOR_SHAPE_OPERATION:
            bprintf(&b, "opcode: %s\noverflow: %s\narity: %u\n", v->opcode_name, v->overflow_name, v->arity);
            bprintf(&b, "op_type: "); bid(&b, &v->op_type_id); bprintf(&b, "\n");
            for (uint8_t i = 0; i < v->arity && i < 4; ++i) {
                bprintf(&b, "input%u: ", i); bid(&b, &v->input_types[i]); bprintf(&b, "\n");
            }
            bprintf(&b, "output: "); bid(&b, &v->output_type); bprintf(&b, "\n");
            break;
        case VISOR_SHAPE_APPLY:
            bprintf(&b, "op: "); bid(&b, &v->apply_op); bprintf(&b, "\n");
            bprintf(&b, "opcode: %s\noverflow: %s\n", v->opcode_name[0] ? v->opcode_name : "?",
                    v->overflow_name[0] ? v->overflow_name : "?");
            bprintf(&b, "operands: %u\n", v->operand_count);
            for (uint8_t i = 0; i < v->operand_count && i < 4; ++i) {
                bprintf(&b, "  [%u] ", i); bid(&b, &v->operands[i]); bprintf(&b, "\n");
            }
            break;
        default:
            break;
    }
    bprintf(&b, "attributes: %u\n", v->attr_count);
    for (uint16_t i = 0; i < v->attr_count; ++i) {
        const VisorAttrView *a = &v->attrs[i];
        bprintf(&b, "  [%u] %s = hex:", i, a->key);
        bhex(&b, a->value, a->len);
        if (a->is_utf8) bprintf(&b, " utf8:\"%.*s\"", (int)a->len, (const char *)a->value);
        bprintf(&b, "\n");
    }
    bprintf(&b, "relations: %u\n", v->rel_count);
    for (uint16_t i = 0; i < v->rel_count; ++i) {
        bprintf(&b, "  [%u] %s ", i, v->rels[i].kind_name);
        bid(&b, &v->rels[i].target);
        bprintf(&b, "%s\n", v->rels[i].resolved ? "" : " (DANGLING)");
    }
    bprintf(&b, "constraints: %u\n", v->const_count);
    for (uint16_t i = 0; i < v->const_count; ++i) {
        bprintf(&b, "  [%u] %s len=%u hex:", i, v->consts[i].kind_name, v->consts[i].len);
        bhex(&b, v->consts[i].payload, v->consts[i].len);
        bprintf(&b, "\n");
    }
    bprintf(&b, "canonical_len: %zu\ncanonical_sha256: %s\ncanonical_id_matches: %s\n",
            v->canonical_len, v->canonical_sha256, v->canonical_id_matches ? "yes" : "no");
    bprintf(&b, "dependencies: %u (dangling %u)\n", v->dep_count, v->dangling_count);
    for (uint16_t i = 0; i < v->dep_count; ++i) {
        bprintf(&b, "  [%u] %s ", i, v->deps[i].role);
        bid(&b, &v->deps[i].id);
        bprintf(&b, "%s\n", v->deps[i].resolved ? "" : " (DANGLING)");
    }
    return bfinish(&b);
}

static void bjstr(Buf *b, const char *s, size_t n) {
    bprintf(b, "\"");
    for (size_t i = 0; i < n && !b->overflow; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"') bprintf(b, "\\\"");
        else if (c == '\\') bprintf(b, "\\\\");
        else if (c < 0x20 || c > 0x7e) bprintf(b, "\\u%04x", c);
        else bprintf(b, "%c", c);
    }
    bprintf(b, "\"");
}

static void bjs(Buf *b, const char *s) { bjstr(b, s, strlen(s)); }

static void bjid(Buf *b, const SemanticId *id) {
    char t[72];
    visor_format_id(id, t);
    bprintf(b, "\"%s\"", t);
}

static void bjhex(Buf *b, const uint8_t *d, size_t n) {
    bprintf(b, "\"");
    bhex(b, d, n);
    bprintf(b, "\"");
}

int visor_semantic_format_json(const VisorObjectView *v, char *out, size_t n) {
    if (!v || !out || n == 0) return -1;
    Buf b = { out, n, 0, false };
    out[0] = '\0';
    bprintf(&b, "{\"id\":\"%s\",\"kind\":", v->id_text);
    bjs(&b, v->kind_name);
    if (v->status != 0) {
        bprintf(&b, ",\"status\":\"malformed\",\"reason\":");
        bjs(&b, v->malformed_reason);
        bprintf(&b, "}");
        return bfinish(&b);
    }
    bprintf(&b, ",\"status\":\"ok\",\"shape\":");
    bjs(&b, v->shape_name);
    bprintf(&b, ",\"type\":");
    bjs(&b, v->type_text);
    bprintf(&b, ",\"type_id\":");
    if (v->has_type_id) bjid(&b, &v->type_id); else bprintf(&b, "null");
    switch (v->shape) {
        case VISOR_SHAPE_TYPE:
            bprintf(&b, ",\"tag\":%u,\"width\":%u,\"length\":%u,\"elem_type\":", v->type_tag, v->type_width, (unsigned)v->type_length);
            if (v->has_elem_type) bjid(&b, &v->elem_type); else bprintf(&b, "null");
            break;
        case VISOR_SHAPE_VALUE:
            bprintf(&b, ",\"value_bytes\":");
            bjhex(&b, v->value_bytes, v->value_byte_len);
            bprintf(&b, ",\"value\":");
            if (!v->has_u64) bprintf(&b, "null");
            else if (v->value_is_bool) bprintf(&b, "%s", v->value_u64 ? "true" : "false");
            else bprintf(&b, "\"%llu\"", (unsigned long long)v->value_u64); /* string: u64 exceeds JSON-safe ints */
            break;
        case VISOR_SHAPE_OPERATION:
            bprintf(&b, ",\"opcode\":"); bjs(&b, v->opcode_name);
            bprintf(&b, ",\"overflow\":"); bjs(&b, v->overflow_name);
            bprintf(&b, ",\"arity\":%u,\"op_type\":", v->arity);
            bjid(&b, &v->op_type_id);
            bprintf(&b, ",\"inputs\":[");
            for (uint8_t i = 0; i < v->arity && i < 4; ++i) { if (i) bprintf(&b, ","); bjid(&b, &v->input_types[i]); }
            bprintf(&b, "],\"output\":");
            bjid(&b, &v->output_type);
            break;
        case VISOR_SHAPE_APPLY:
            bprintf(&b, ",\"op\":"); bjid(&b, &v->apply_op);
            bprintf(&b, ",\"opcode\":"); if (v->opcode_name[0]) bjs(&b, v->opcode_name); else bprintf(&b, "null");
            bprintf(&b, ",\"overflow\":"); if (v->overflow_name[0]) bjs(&b, v->overflow_name); else bprintf(&b, "null");
            bprintf(&b, ",\"operands\":[");
            for (uint8_t i = 0; i < v->operand_count && i < 4; ++i) { if (i) bprintf(&b, ","); bjid(&b, &v->operands[i]); }
            bprintf(&b, "]");
            break;
        default:
            break;
    }
    bprintf(&b, ",\"attributes\":[");
    for (uint16_t i = 0; i < v->attr_count; ++i) {
        const VisorAttrView *a = &v->attrs[i];
        if (i) bprintf(&b, ",");
        bprintf(&b, "{\"key\":");
        bjs(&b, a->key);
        bprintf(&b, ",\"hex\":");
        bjhex(&b, a->value, a->len);
        if (a->is_utf8) { bprintf(&b, ",\"utf8\":"); bjstr(&b, (const char *)a->value, a->len); }
        bprintf(&b, "}");
    }
    bprintf(&b, "],\"relations\":[");
    for (uint16_t i = 0; i < v->rel_count; ++i) {
        if (i) bprintf(&b, ",");
        bprintf(&b, "{\"kind\":");
        bjs(&b, v->rels[i].kind_name);
        bprintf(&b, ",\"target\":");
        bjid(&b, &v->rels[i].target);
        bprintf(&b, ",\"resolved\":%s}", v->rels[i].resolved ? "true" : "false");
    }
    bprintf(&b, "],\"constraints\":[");
    for (uint16_t i = 0; i < v->const_count; ++i) {
        if (i) bprintf(&b, ",");
        bprintf(&b, "{\"kind\":");
        bjs(&b, v->consts[i].kind_name);
        bprintf(&b, ",\"len\":%u,\"hex\":", v->consts[i].len);
        bjhex(&b, v->consts[i].payload, v->consts[i].len);
        bprintf(&b, "}");
    }
    bprintf(&b, "],\"canonical\":{\"len\":%zu,\"sha256\":\"%s\",\"id_matches\":%s}",
            v->canonical_len, v->canonical_sha256, v->canonical_id_matches ? "true" : "false");
    bprintf(&b, ",\"dependencies\":[");
    for (uint16_t i = 0; i < v->dep_count; ++i) {
        if (i) bprintf(&b, ",");
        bprintf(&b, "{\"role\":");
        bjs(&b, v->deps[i].role);
        bprintf(&b, ",\"id\":");
        bjid(&b, &v->deps[i].id);
        bprintf(&b, ",\"resolved\":%s}", v->deps[i].resolved ? "true" : "false");
    }
    bprintf(&b, "],\"dangling\":%u}", v->dangling_count);
    return bfinish(&b);
}

/* ------------------------------------------------------------------ type_of */

int visor_semantic_type_of(const OmegaGraph *g, const SemanticId *id, SemanticId *out_type_id,
                           char *type_text, size_t n) {
    const OmegaObject *o;
    Parsed p;
    char reason[VISOR_SEM_REASON_LEN];
    int rc = load(g, id, &o, &p, reason, sizeof(reason));
    if (rc != 0) return rc;
    SemanticId t;
    if (result_type_id(g, &p, &t) != 0) return -1;
    char txt[VISOR_SEM_TYPE_TEXT];
    if (type_text_of(g, &t, txt, sizeof(txt), 0) != 0) return -1;  /* unresolved type: fail closed */
    if (out_type_id) *out_type_id = t;
    if (type_text && n) snprintf(type_text, n, "%s", txt);
    return 0;
}

/* ------------------------------------------------------------- apply/value */

int visor_semantic_apply_parts(const OmegaGraph *g, const SemanticId *apply_id, SemanticId *out_op,
                               SemanticId *out_operands, size_t *out_count) {
    const OmegaObject *o;
    Parsed p;
    char reason[VISOR_SEM_REASON_LEN];
    int rc = load(g, apply_id, &o, &p, reason, sizeof(reason));
    if (rc != 0) return rc;
    if (p.shape != VISOR_SHAPE_APPLY) return -1;
    if (out_op) *out_op = p.ap.op_id;
    if (out_operands) memcpy(out_operands, p.ap.operands, sizeof(SemanticId) * p.ap.operand_count);
    if (out_count) *out_count = p.ap.operand_count;
    return 0;
}

int visor_semantic_value_u64(const OmegaGraph *g, const SemanticId *value_id, uint64_t *out) {
    const OmegaObject *o;
    Parsed p;
    char reason[VISOR_SEM_REASON_LEN];
    if (!out) return -1;
    int rc = load(g, value_id, &o, &p, reason, sizeof(reason));
    if (rc != 0) return rc;
    if (p.shape != VISOR_SHAPE_VALUE) return -1;
    bool is_bool;
    uint64_t v;
    if (decode_value(g, &p.vp, &v, &is_bool) != 0) return -1;
    *out = v;
    return 0;
}

/* -------------------------------------------------------------------- eval */

static int eval_rec(const OmegaGraph *g, const SemanticId *id, uint64_t *out, int depth) {
    if (depth > VISOR_SEM_EVAL_DEPTH) return -1;
    const OmegaObject *o;
    Parsed p;
    char reason[VISOR_SEM_REASON_LEN];
    int rc = load(g, id, &o, &p, reason, sizeof(reason));
    if (rc != 0) return rc;
    if (p.shape == VISOR_SHAPE_VALUE) {
        bool is_bool;
        return decode_value(g, &p.vp, out, &is_bool) == 0 ? 0 : -1;
    }
    if (p.shape != VISOR_SHAPE_APPLY) return -1;

    const OmegaObject *op_obj;
    Parsed op;
    rc = load(g, &p.ap.op_id, &op_obj, &op, reason, sizeof(reason));
    if (rc != 0) return rc;
    if (op.shape != VISOR_SHAPE_OPERATION) return -1;
    if (op.op.arity != 2 || p.ap.operand_count != 2) return -1;

    uint16_t width;
    bool is_bool;
    if (int_width_of(g, &op.op.type_id, &width, &is_bool) != 0 || is_bool) return -1;

    uint64_t args[2];
    for (int i = 0; i < 2; ++i) {
        /* operand type must be the operation's declared input type */
        const OmegaObject *arg_obj;
        Parsed arg;
        rc = load(g, &p.ap.operands[i], &arg_obj, &arg, reason, sizeof(reason));
        if (rc != 0) return rc;
        SemanticId at;
        if (result_type_id(g, &arg, &at) != 0 || !id_eq(&at, &op.op.input_types[i])) return -1;
        rc = eval_rec(g, &p.ap.operands[i], &args[i], depth + 1);
        if (rc != 0) return rc;
    }
    uint64_t r;
    rc = omega_eval_pure_binary_uint(op.op.opcode, op.op.overflow, width, args[0], args[1], &r);
    if (rc != 0) return -3;
    *out = r;
    return 0;
}

int visor_semantic_eval_u64(const OmegaGraph *g, const SemanticId *id, uint64_t *out) {
    if (!g || !id || !out) return -1;
    uint64_t r;
    int rc = eval_rec(g, id, &r, 0);
    if (rc == 0) *out = r;
    return rc;
}

/* ------------------------------------------------------------- graph text */

static void short_id(const SemanticId *id, char out[16]) {
    char hex[65];
    omega_hex_semantic_id(id, hex);
    snprintf(out, 16, "%.12s", hex);
}

static void summary_line(const OmegaGraph *g, const OmegaObject *o, const Parsed *p, Buf *b) {
    char t[VISOR_SEM_TYPE_TEXT];
    switch (p->shape) {
        case VISOR_SHAPE_TYPE:
            type_text_of(g, &o->id, t, sizeof(t), 0);
            bprintf(b, "type %s", t);
            break;
        case VISOR_SHAPE_VALUE: {
            uint64_t v;
            bool is_bool;
            type_text_of(g, &p->vp.type_id, t, sizeof(t), 0);
            if (decode_value(g, &p->vp, &v, &is_bool) == 0) {
                if (is_bool) bprintf(b, "value %s : %s", v ? "true" : "false", t);
                else bprintf(b, "value %llu : %s", (unsigned long long)v, t);
            } else {
                bprintf(b, "value hex:");
                bhex(b, p->vp.bytes, p->vp.byte_len);
                bprintf(b, " : %s", t);
            }
            break;
        }
        case VISOR_SHAPE_OPERATION:
            type_text_of(g, &p->op.output_type, t, sizeof(t), 0);
            bprintf(b, "op %s %s arity=%u -> %s", visor_semantic_opcode_name(p->op.opcode),
                    visor_semantic_overflow_name(p->op.overflow), p->op.arity, t);
            break;
        case VISOR_SHAPE_APPLY: {
            const OmegaObject *op_obj;
            Parsed op;
            char reason[VISOR_SEM_REASON_LEN];
            const char *name = "?";
            if (load(g, &p->ap.op_id, &op_obj, &op, reason, sizeof(reason)) == 0 && op.shape == VISOR_SHAPE_OPERATION)
                name = visor_semantic_opcode_name(op.op.opcode);
            bprintf(b, "apply %s(", name);
            for (uint8_t i = 0; i < p->ap.operand_count; ++i) {
                char s[16];
                short_id(&p->ap.operands[i], s);
                bprintf(b, "%s%s", i ? "," : "", s);
            }
            SemanticId rt;
            if (result_type_id(g, p, &rt) == 0) type_text_of(g, &rt, t, sizeof(t), 0);
            else snprintf(t, sizeof(t), "?");
            bprintf(b, ") : %s", t);
            break;
        }
        default:
            bprintf(b, "payload_len=%u", (unsigned)o->payload_len);
            break;
    }
}

static int graph_rec(const OmegaGraph *g, const SemanticId *id, int depth, bool *visited, Buf *b) {
    if (depth > VISOR_SEM_GRAPH_DEPTH) return -1;
    const OmegaObject *o = find(g, id);
    if (o && visited[(size_t)(o - g->objects)]) return 0;   /* dedupe: each object printed once */
    for (int i = 0; i < depth; ++i) bprintf(b, "  ");
    if (!o) {
        bprintf(b, "MISSING ");
        bid(b, id);
        bprintf(b, "\n");
        return 0;
    }
    size_t idx = (size_t)(o - g->objects);
    Parsed p;
    char reason[VISOR_SEM_REASON_LEN];
    const OmegaObject *dummy;
    if (load(g, id, &dummy, &p, reason, sizeof(reason)) != 0) {
        bprintf(b, "MALFORMED ");
        bid(b, id);
        bprintf(b, " %s\n", reason);
        visited[idx] = true;
        return 0;
    }
    bprintf(b, "%s ", visor_kind_name(o->kind));
    bid(b, id);
    bprintf(b, " ");
    summary_line(g, o, &p, b);
    bprintf(b, "\n");
    visited[idx] = true;
    DepList d;
    collect_deps(o, &p, &d);
    for (uint16_t i = 0; i < d.count; ++i) {
        int rc = graph_rec(g, &d.ids[i], depth + 1, visited, b);
        if (rc != 0) return rc;
    }
    return 0;
}

int visor_semantic_graph_text(const OmegaGraph *g, const SemanticId *root, char *out, size_t n) {
    if (!g || !root || !out || n == 0) return -1;
    out[0] = '\0';
    if (!find(g, root)) return -1;
    bool visited[OMEGA_MAX_GRAPH_OBJECTS];
    memset(visited, 0, sizeof(visited));
    Buf b = { out, n, 0, false };
    int rc = graph_rec(g, root, 0, visited, &b);
    if (rc != 0) {
        out[0] = '\0';
        return -1;
    }
    return bfinish(&b);
}
