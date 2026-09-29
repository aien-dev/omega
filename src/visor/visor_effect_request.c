/*
 * visor_effect_request.c -- Omega Visor V1, lane 7. See visor_effect_request.h.
 *
 * THE VISOR CAN ASK. THE VISOR CANNOT GRANT.
 *
 * Pure over OmegaGraph. References no runtime (rx_*), AIENOS (aienos_cap_*)
 * or publish-gate (rc_*) symbol; tests/visor/check_authority_link.sh enforces
 * that on the object file. The only write to `authorized` in this file is the
 * zero-initialisation in request_reset(); the field is never set true.
 */
#include "visor_effect_request.h"

#include <stdio.h>
#include <string.h>

#include "omega_canonical.h"
#include "omega_core.h"
#include "omega_validate.h"
#include "sha256.h"

static void request_reset(VisorEffectRequest *out) {
    memset(out, 0, sizeof(*out));
    out->authorized = false;
    snprintf(out->status, sizeof out->status, "%s", VISOR_EFFECT_STATUS_UNAUTHORIZED);
    snprintf(out->route, sizeof out->route, "%s", VISOR_EFFECT_ROUTE);
}

int visor_effect_request_build(const OmegaGraph *g, const SemanticId *effect_object_id,
                               VisorEffectRequest *out) {
    if (!out) return -1;
    request_reset(out);
    if (!g || !effect_object_id) return -1;

    const OmegaObject *obj = omega_graph_find_object_const(g, effect_object_id);
    if (!obj || !obj->has_id || obj->kind != KIND_EFFECT) return -1;

    /* Existing structural validation first (payload too short, resource 0). */
    char err[160];
    if (omega_validate_object(g, obj, err, sizeof err) != 0) return -1;
    if (obj->payload_len != sizeof(EffectPayload)) return -1;

    EffectPayload eff;
    memcpy(&eff, obj->payload, sizeof eff);
    if (eff.param_len > VISOR_EFFECT_PARAM_MAX) return -1;

    /* Digest = sha256(canonical encoding). Computed into a local buffer so the
     * graph is never written. It must equal the stored SemanticId: an object
     * edited after it was named (resource, operation, capability fields or
     * parameters changed) no longer matches and is refused. */
    uint8_t canon[4096];
    size_t canon_len = 0;
    if (omega_canonical_encode(obj, canon, sizeof canon, &canon_len) != 0) return -1;
    uint8_t digest[32];
    sha256_hash(canon, canon_len, digest);
    if (memcmp(digest, obj->id.bytes, sizeof digest) != 0) return -1;

    out->effect_id = obj->id;
    out->resource_class = eff.resource_class;
    out->operation_code = eff.operation_code;
    out->capability_slot = eff.capability_slot;
    out->capability_generation = eff.capability_generation;
    out->capability_ref = eff.capability_ref;
    out->param_len = eff.param_len;
    memcpy(out->param_bytes, eff.param_bytes, eff.param_len);
    memcpy(out->request_digest, digest, sizeof digest);
    return 0;
}

/* ---- classification ---- */

static bool id_is_zero(const SemanticId *id) {
    for (size_t i = 0; i < OMEGA_ID_BYTES; i++)
        if (id->bytes[i]) return false;
    return true;
}

static int index_of(const OmegaGraph *g, const SemanticId *id) {
    const OmegaObject *o = omega_graph_find_object_const(g, id);
    if (!o) return -1;
    return (int)(o - g->objects);
}

typedef struct {
    const OmegaGraph *g;
    uint16_t stack[OMEGA_MAX_GRAPH_OBJECTS];
    uint16_t depth;
    uint8_t seen[OMEGA_MAX_GRAPH_OBJECTS];
    bool missing;
} Walk;

static void push_ref(Walk *w, const SemanticId *id) {
    if (id_is_zero(id)) return; /* zero id = field unused */
    int i = index_of(w->g, id);
    if (i < 0) { w->missing = true; return; }
    if (w->seen[i]) return;
    w->seen[i] = 1;
    w->stack[w->depth++] = (uint16_t)i;
}

static bool object_needs_authority(const OmegaObject *o) {
    if (o->kind == KIND_EFFECT) return true;
    if (o->kind == KIND_TYPE && o->payload_len >= sizeof(TypePayload)) {
        TypePayload tp;
        memcpy(&tp, o->payload, sizeof tp);
        if (tp.tag == TYPE_CAPABILITY_REF || tp.tag == TYPE_EFFECT_INTENT_REF ||
            tp.tag == TYPE_EFFECT_RECEIPT_REF)
            return true;
    }
    return false;
}

static void push_payload_refs(Walk *w, const OmegaObject *o) {
    switch (o->kind) {
    case KIND_TYPE:
        if (o->payload_len >= sizeof(TypePayload)) {
            TypePayload tp;
            memcpy(&tp, o->payload, sizeof tp);
            push_ref(w, &tp.elem_type);
        }
        break;
    case KIND_VALUE:
        if (o->payload_len >= sizeof(ValuePayload)) {
            ValuePayload vp;
            memcpy(&vp, o->payload, sizeof vp);
            push_ref(w, &vp.type_id);
        }
        break;
    case KIND_OPERATION:
        /* Same discrimination as omega_validate_object. */
        if (o->payload_len >= sizeof(OperationPayload)) {
            OperationPayload op;
            memcpy(&op, o->payload, sizeof op);
            push_ref(w, &op.type_id);
            for (int i = 0; i < 4; i++) push_ref(w, &op.input_types[i]);
            push_ref(w, &op.output_type);
        } else if (o->payload_len >= sizeof(ApplyPayload)) {
            ApplyPayload ap;
            memcpy(&ap, o->payload, sizeof ap);
            push_ref(w, &ap.op_id);
            uint8_t n = ap.operand_count > 4 ? 4 : ap.operand_count;
            for (uint8_t i = 0; i < n; i++) push_ref(w, &ap.operands[i]);
        }
        break;
    case KIND_EFFECT:
        if (o->payload_len >= sizeof(EffectPayload)) {
            EffectPayload ep;
            memcpy(&ep, o->payload, sizeof ep);
            push_ref(w, &ep.capability_ref);
        }
        break;
    default:
        break;
    }
}

int visor_effect_request_classify(const OmegaGraph *g, const SemanticId *id,
                                  bool *requires_authority) {
    if (!requires_authority) return -1;
    *requires_authority = true; /* fail closed until proven otherwise */
    if (!g || !id) return -1;
    if (g->object_count > OMEGA_MAX_GRAPH_OBJECTS) return -1;

    Walk w;
    memset(&w, 0, sizeof w);
    w.g = g;
    int root = index_of(g, id);
    if (root < 0) return -1;
    w.seen[root] = 1;
    w.stack[w.depth++] = (uint16_t)root;

    bool needs = false;
    while (w.depth > 0) {
        const OmegaObject *o = &g->objects[w.stack[--w.depth]];
        if (object_needs_authority(o)) needs = true;
        uint16_t nrel = o->rel_count > OMEGA_MAX_RELATIONS ? OMEGA_MAX_RELATIONS : o->rel_count;
        for (uint16_t r = 0; r < nrel; r++) push_ref(&w, &o->relations[r].target_id);
        push_payload_refs(&w, o);
    }
    if (w.missing) return -1;
    *requires_authority = needs;
    return 0;
}

/* ---- formatting ---- */

static int hex_bytes(const uint8_t *b, size_t n, char *out, size_t cap) {
    static const char digits[] = "0123456789abcdef";
    if (cap < n * 2 + 1) return -1;
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[b[i] >> 4];
        out[2 * i + 1] = digits[b[i] & 15];
    }
    out[n * 2] = '\0';
    return 0;
}

typedef struct {
    char effect[65], cap[65], digest[65], params[VISOR_EFFECT_PARAM_MAX * 2 + 1];
} Hexes;

static int make_hexes(const VisorEffectRequest *r, Hexes *h) {
    if (r->param_len > VISOR_EFFECT_PARAM_MAX) return -1;
    if (hex_bytes(r->effect_id.bytes, 32, h->effect, sizeof h->effect)) return -1;
    if (hex_bytes(r->capability_ref.bytes, 32, h->cap, sizeof h->cap)) return -1;
    if (hex_bytes(r->request_digest, 32, h->digest, sizeof h->digest)) return -1;
    if (hex_bytes(r->param_bytes, r->param_len, h->params, sizeof h->params)) return -1;
    return 0;
}

/* The printed authorization state is a constant. Whatever the struct holds,
 * the Visor never renders a request as authorized. */
int visor_effect_request_format_text(const VisorEffectRequest *r, char *out, size_t n) {
    if (!r || !out || n == 0) return -1;
    Hexes h;
    if (make_hexes(r, &h)) return -1;
    int k = snprintf(out, n,
                     "effect request (UNAUTHORIZED_REQUEST)\n"
                     "  effect:     sha256:%s\n"
                     "  resource:   class %u\n"
                     "  operation:  code %u\n"
                     "  capability: slot %u generation %u ref sha256:%s (not validated by the Visor)\n"
                     "  params:     %u bytes %s\n"
                     "  digest:     sha256:%s\n"
                     "  authorized: false\n"
                     "  route:      %s\n",
                     h.effect, (unsigned)r->resource_class, (unsigned)r->operation_code,
                     (unsigned)r->capability_slot, (unsigned)r->capability_generation, h.cap,
                     (unsigned)r->param_len, h.params, h.digest, VISOR_EFFECT_ROUTE);
    if (k < 0 || (size_t)k >= n) return -1;
    return 0;
}

int visor_effect_request_format_json(const VisorEffectRequest *r, char *out, size_t n) {
    if (!r || !out || n == 0) return -1;
    Hexes h;
    if (make_hexes(r, &h)) return -1;
    int k = snprintf(out, n,
                     "{\"effect_id\":\"sha256:%s\",\"resource_class\":%u,\"operation_code\":%u,"
                     "\"capability_slot\":%u,\"capability_generation\":%u,"
                     "\"capability_ref\":\"sha256:%s\",\"param_len\":%u,\"param_bytes\":\"%s\","
                     "\"request_digest\":\"sha256:%s\",\"authorized\":false,"
                     "\"status\":\"%s\",\"route\":\"%s\"}",
                     h.effect, (unsigned)r->resource_class, (unsigned)r->operation_code,
                     (unsigned)r->capability_slot, (unsigned)r->capability_generation, h.cap,
                     (unsigned)r->param_len, h.params, h.digest,
                     VISOR_EFFECT_STATUS_UNAUTHORIZED, VISOR_EFFECT_ROUTE);
    if (k < 0 || (size_t)k >= n) return -1;
    return 0;
}
