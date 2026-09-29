/* Omega Visor V1 - lane 4: structured verification report over the existing
 * verifier. See visor_verify.h for the row contract. */
#include "visor_verify.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "omega_canonical.h"
#include "omega_core.h"
#include "omega_validate.h"
#include "omega_verify.h"

static const char *const k_row_names[VISOR_ROW_COUNT] = {
    "STRUCTURAL", "TYPE", "INVARIANTS", "REALIZATION", "AUTHORITY", "MACHINE"
};

const char *visor_check_status_name(VisorCheckStatus s) {
    switch (s) {
        case VISOR_CHECK_PASS: return "PASS";
        case VISOR_CHECK_FAIL: return "FAIL";
        case VISOR_CHECK_NOT_RUN: return "NOT_RUN";
    }
    return "UNKNOWN";
}

static const char *visor_kind_name_local(SemanticKind k) {
    static const char *const names[] = {
        "INVALID", "VALUE", "TYPE", "OPERATION", "RELATION", "CONSTRAINT", "MEMORY",
        "MACHINE", "EFFECT", "REALIZATION", "EVIDENCE", "PROOF"
    };
    return ((unsigned)k < sizeof(names) / sizeof(names[0])) ? names[k] : "INVALID";
}

static void fmt_id(const SemanticId *id, char out[72]) {
    char hex[65];
    omega_hex_semantic_id(id, hex);
    snprintf(out, 72, "sha256:%s", hex);
}

static void report_reset(VisorVerifyReport *out) {
    memset(out, 0, sizeof(*out));
    out->row_count = VISOR_ROW_COUNT;
    for (size_t i = 0; i < VISOR_ROW_COUNT; ++i) {
        out->rows[i].name = k_row_names[i];
        out->rows[i].status = VISOR_CHECK_NOT_RUN;
    }
}

static void row_set(VisorVerifyReport *out, int row, VisorCheckStatus st,
                    uint32_t checks, uint32_t fails, const char *fmt, ...) {
    VisorVerifyRow *r = &out->rows[row];
    r->status = st;
    r->checks = checks;
    r->fails = fails;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->detail, sizeof(r->detail), fmt, ap);
    va_end(ap);
}

/* Record the first violation only; later failures keep their row detail. */
static void note_violation(VisorVerifyReport *out, const char *violation,
                           const SemanticId *object_or_null,
                           const char *expected, const char *observed) {
    if (out->first_violation[0]) return;
    snprintf(out->first_violation, sizeof(out->first_violation), "%.255s", violation ? violation : "");
    if (object_or_null) fmt_id(object_or_null, out->object_involved);
    snprintf(out->expected, sizeof(out->expected), "%.127s", expected ? expected : "");
    snprintf(out->observed, sizeof(out->observed), "%.127s", observed ? observed : "");
}

static void finalize(VisorVerifyReport *out) {
    bool any_ran = false, any_fail = false;
    for (size_t i = 0; i < out->row_count; ++i) {
        if (out->rows[i].status == VISOR_CHECK_PASS) any_ran = true;
        if (out->rows[i].status == VISOR_CHECK_FAIL) any_fail = true;
    }
    out->passed = any_ran && !any_fail;
    if (!out->passed && !out->first_violation[0])
        note_violation(out, "no check ran", NULL, "at least one executed check", "none");
}

/* --- graph shape helpers (codec convention: >= OperationPayload is an
 *     operation, else >= ApplyPayload is an apply) --- */
static const OperationPayload *as_operation(const OmegaObject *o) {
    if (!o || o->kind != KIND_OPERATION || o->payload_len < sizeof(OperationPayload)) return NULL;
    return (const OperationPayload *)o->payload;
}
static const ApplyPayload *as_apply(const OmegaObject *o) {
    if (!o || o->kind != KIND_OPERATION || o->payload_len >= sizeof(OperationPayload) ||
        o->payload_len < sizeof(ApplyPayload)) return NULL;
    return (const ApplyPayload *)o->payload;
}
static bool id_eq(const SemanticId *a, const SemanticId *b) {
    return memcmp(a->bytes, b->bytes, OMEGA_ID_BYTES) == 0;
}
static bool id_zero(const SemanticId *a) {
    for (size_t i = 0; i < OMEGA_ID_BYTES; ++i) if (a->bytes[i]) return false;
    return true;
}
/* Value object -> uint64 (little-endian bytes, up to 8), if it is one. */
static bool value_u64(const OmegaGraph *g, const SemanticId *id, uint64_t *out) {
    const OmegaObject *o = omega_graph_find_object_const(g, id);
    if (!o || o->kind != KIND_VALUE || o->payload_len < sizeof(ValuePayload)) return false;
    const ValuePayload *vp = (const ValuePayload *)o->payload;
    if (vp->byte_len == 0 || vp->byte_len > 8) return false;
    uint64_t v = 0;
    for (uint16_t i = 0; i < vp->byte_len; ++i) v |= (uint64_t)vp->bytes[i] << (8u * i);
    *out = v;
    return true;
}

/* --- AUTHORITY: requirement walk (reports, never grants) --- */
static int slot_of(const OmegaGraph *g, const SemanticId *id) {
    if (id_zero(id)) return -1;
    for (uint16_t i = 0; i < g->object_count && i < OMEGA_MAX_GRAPH_OBJECTS; ++i)
        if (g->objects[i].has_id && id_eq(&g->objects[i].id, id)) return (int)i;
    return -1;
}

static void authority_walk(const OmegaGraph *g, int root, uint32_t *walked,
                           uint32_t *effects, uint32_t *caprefs) {
    bool visited[OMEGA_MAX_GRAPH_OBJECTS];
    int stack[OMEGA_MAX_GRAPH_OBJECTS];
    size_t sp = 0;
    memset(visited, 0, sizeof(visited));
    *walked = *effects = *caprefs = 0;
    if (root < 0) return;
    stack[sp++] = root;
    visited[root] = true;
    while (sp > 0) {
        const OmegaObject *o = &g->objects[stack[--sp]];
        (*walked)++;
        SemanticId next[OMEGA_MAX_RELATIONS + 8];
        size_t nn = 0;
        if (o->kind == KIND_EFFECT) {
            (*effects)++;
            EffectPayload ep;
            if (omega_effect_read(o, &ep) == OMEGA_EFFECT_OK && !id_zero(&ep.capability_ref)) {
                (*caprefs)++;
                next[nn++] = ep.capability_ref;
            }
        } else if (o->kind == KIND_TYPE && o->payload_len >= sizeof(TypePayload)) {
            const TypePayload *tp = (const TypePayload *)o->payload;
            if (tp->tag == TYPE_CAPABILITY_REF || tp->tag == TYPE_EFFECT_INTENT_REF ||
                tp->tag == TYPE_EFFECT_RECEIPT_REF) (*caprefs)++;
            if (tp->tag == TYPE_SEQUENCE) next[nn++] = tp->elem_type;
        } else if (o->kind == KIND_VALUE && o->payload_len >= sizeof(ValuePayload)) {
            next[nn++] = ((const ValuePayload *)o->payload)->type_id;
        } else if (as_operation(o)) {
            const OperationPayload *op = as_operation(o);
            next[nn++] = op->type_id;
            next[nn++] = op->output_type;
            for (int i = 0; i < 4; ++i) next[nn++] = op->input_types[i];
        } else if (as_apply(o)) {
            const ApplyPayload *ap = as_apply(o);
            next[nn++] = ap->op_id;
            for (int i = 0; i < 4; ++i) next[nn++] = ap->operands[i];
        }
        for (uint16_t i = 0; i < o->rel_count && i < OMEGA_MAX_RELATIONS && nn < sizeof(next) / sizeof(next[0]); ++i)
            next[nn++] = o->relations[i].target_id;
        for (size_t i = 0; i < nn; ++i) {
            int s = slot_of(g, &next[i]);
            if (s >= 0 && !visited[s] && sp < OMEGA_MAX_GRAPH_OBJECTS) { visited[s] = true; stack[sp++] = s; }
        }
    }
}

static void authority_row(const OmegaGraph *g, int root, VisorVerifyReport *out) {
    uint32_t walked = 0, eff = 0, cap = 0;
    authority_walk(g, root, &walked, &eff, &cap);
    out->effect_count = eff;
    out->capability_ref_count = cap;
    out->authority_required = (eff + cap) > 0;
    if (out->authority_required)
        row_set(out, VISOR_ROW_AUTHORITY, VISOR_CHECK_PASS, walked, 0,
                "REQUIRED: %u effect(s), %u capability ref(s) reachable; visor grants nothing",
                eff, cap);
    else
        row_set(out, VISOR_ROW_AUTHORITY, VISOR_CHECK_PASS, walked, 0,
                "NONE: no effect or capability ref reachable (%u object(s) walked)", walked);
}

/* Pull expected=/observed= numbers out of an existing V1 error_detail. */
static void v1_expected_observed(const char *detail, char exp[128], char obs[128]) {
    const char *e = strstr(detail, "expected=");
    const char *o = strstr(detail, "observed=");
    unsigned long ev = 0, ov = 0;
    if (e && sscanf(e, "expected=%lu", &ev) == 1) snprintf(exp, 128, "%lu", ev); else exp[0] = 0;
    if (o && sscanf(o, "observed=%lu", &ov) == 1) snprintf(obs, 128, "%lu", ov); else obs[0] = 0;
}

static void realization_row(const OmegaGraph *g, const SemanticId *id, const OmegaObject *obj,
                            const RealizationObject *real, VisorVerifyReport *out) {
    if (!real) {
        row_set(out, VISOR_ROW_REALIZATION, VISOR_CHECK_NOT_RUN, 0, 0, "no realization built");
        return;
    }
    const ApplyPayload *subject_apply = as_apply(obj);
    bool bound = id_eq(&real->semantic_id, id) ||
                 (subject_apply && id_eq(&real->semantic_id, &subject_apply->op_id));
    if (!bound) {
        char exp[72], obs[72];
        fmt_id(id, exp);
        fmt_id(&real->semantic_id, obs);
        row_set(out, VISOR_ROW_REALIZATION, VISOR_CHECK_FAIL, 1, 1,
                "realization is bound to a different semantic object");
        note_violation(out, "realization is bound to a different semantic object", id, exp, obs);
        return;
    }
    const OmegaObject *target = omega_graph_find_object_const(g, &real->semantic_id);
    const OperationPayload *top = as_operation(target);
    const ApplyPayload *tap = as_apply(target);

    uint64_t vec[3 * 16];
    size_t triples = 0;
    const char *model = NULL;
    if (top && top->opcode == OP_ADD) {
        /* Pure-binary ADD realization computes X0+X1; the existing V1 reference
         * is (a+b)-c, so every triple carries c=0 and both sides mean a+b. */
        static const uint64_t base[][2] = {
            {0, 0}, {0, 1}, {1, 0}, {1, 1}, {UINT64_MAX, 0}, {UINT64_MAX, 1},
            {1, UINT64_MAX}, {UINT64_MAX, UINT64_MAX}, {7, 11}
        };
        for (size_t i = 0; i < sizeof(base) / sizeof(base[0]); ++i) {
            vec[triples * 3] = base[i][0]; vec[triples * 3 + 1] = base[i][1]; vec[triples * 3 + 2] = 0;
            triples++;
        }
        uint64_t a = 0, b = 0;
        if (subject_apply && value_u64(g, &subject_apply->operands[0], &a) &&
            value_u64(g, &subject_apply->operands[1], &b)) {
            vec[triples * 3] = a; vec[triples * 3 + 1] = b; vec[triples * 3 + 2] = 0; triples++;
            vec[triples * 3] = b; vec[triples * 3 + 1] = a; vec[triples * 3 + 2] = 0; triples++;
        }
        model = "V1 differential, OP_ADD with c=0";
    } else if (tap) {
        /* Only SUB(ADD(a,b),c) matches the V1 reference model as-is. */
        const OperationPayload *outer = as_operation(omega_graph_find_object_const(g, &tap->op_id));
        const ApplyPayload *inner = as_apply(omega_graph_find_object_const(g, &tap->operands[0]));
        const OperationPayload *inner_op = inner ? as_operation(omega_graph_find_object_const(g, &inner->op_id)) : NULL;
        if (outer && outer->opcode == OP_SUB && inner_op && inner_op->opcode == OP_ADD)
            model = "V1 differential, (a+b)-c default vectors";
    }
    /* Always check the supplied realization's structure (profile, bounds,
     * decode, RET, realization id) so tampered code never passes unchecked. */
    VerifyReport v0r;
    if (omega_verify_v0_structural(g, real, &v0r) != 0 || !v0r.passed) {
        row_set(out, VISOR_ROW_REALIZATION, VISOR_CHECK_FAIL, v0r.check_count,
                v0r.fail_count ? v0r.fail_count : 1, "%.250s", v0r.error_detail);
        note_violation(out, v0r.error_detail, &real->semantic_id, "realization passes V0 structural", v0r.error_detail);
        return;
    }
    if (!model) {
        if (top)
            row_set(out, VISOR_ROW_REALIZATION, VISOR_CHECK_NOT_RUN, v0r.check_count, 0,
                    "V0 realization structure PASS (%u checks); V1 NOT_RUN: existing reference is (a+b)-c, "
                    "not applicable to op 0x%02x", v0r.check_count, (unsigned)top->opcode);
        else
            row_set(out, VISOR_ROW_REALIZATION, VISOR_CHECK_NOT_RUN, v0r.check_count, 0,
                    "V0 realization structure PASS (%u checks); V1 NOT_RUN: existing reference is (a+b)-c, "
                    "subject shape not covered", v0r.check_count);
        return;
    }
    VerifyReport v1;
    int rc = omega_verify_v1_differential(g, real, triples ? vec : NULL, triples, &v1);
    if (rc == 0 && v1.passed) {
        row_set(out, VISOR_ROW_REALIZATION, VISOR_CHECK_PASS, v1.check_count, v1.fail_count,
                "%s (%u check(s), incl. V0 realization id)", model, v1.check_count);
    } else {
        char exp[128], obs[128];
        v1_expected_observed(v1.error_detail, exp, obs);
        row_set(out, VISOR_ROW_REALIZATION, VISOR_CHECK_FAIL, v1.check_count,
                v1.fail_count ? v1.fail_count : 1, "%.250s", v1.error_detail);
        note_violation(out, v1.error_detail, &real->semantic_id, exp, obs);
    }
}

static void machine_row(const RealizationObject *real, const OmegaMachineGraph *mg,
                        VisorVerifyReport *out) {
    if (!real) { row_set(out, VISOR_ROW_MACHINE, VISOR_CHECK_NOT_RUN, 0, 0, "no realization built"); return; }
    if (!mg) { row_set(out, VISOR_ROW_MACHINE, VISOR_CHECK_NOT_RUN, 0, 0, "no machine model"); return; }
    uint32_t checks = 1;
    if (real->target_profile != mg->target_profile) {
        char e[128], o[128];
        snprintf(e, sizeof(e), "target profile 0x%02x (%.60s)", mg->target_profile, mg->name);
        snprintf(o, sizeof(o), "realization profile 0x%02x", real->target_profile);
        row_set(out, VISOR_ROW_MACHINE, VISOR_CHECK_FAIL, 1, 1, "profile mismatch: %s vs %s", o, e);
        note_violation(out, "realization target profile does not match machine model", NULL, e, o);
        return;
    }
    if (real->has_machine_id) {
        checks++;
        if (!id_eq(&real->machine_id, &mg->machine_id)) {
            char e[72], o[72];
            fmt_id(&mg->machine_id, e);
            fmt_id(&real->machine_id, o);
            row_set(out, VISOR_ROW_MACHINE, VISOR_CHECK_FAIL, checks, 1,
                    "realization bound to a different machine id");
            note_violation(out, "realization bound to a different machine id", NULL, e, o);
            return;
        }
    }
    row_set(out, VISOR_ROW_MACHINE, VISOR_CHECK_PASS, checks, 0,
            "profile 0x%02x matches %.60s%s", real->target_profile, mg->name,
            real->has_machine_id ? "; machine id bound and equal" : "; realization not bound to a machine id");
}

static void invariants_row(VisorVerifyReport *out) {
    VerifyReport v2;
    if (omega_verify_v2_properties(NULL, NULL, &v2) == 0 && v2.passed) {
        row_set(out, VISOR_ROW_INVARIANTS, VISOR_CHECK_PASS, v2.check_count, v2.fail_count,
                "V2 reference-evaluator algebra (commutativity, identity, wrap, range); not object-specific");
    } else {
        row_set(out, VISOR_ROW_INVARIANTS, VISOR_CHECK_FAIL, v2.check_count,
                v2.fail_count ? v2.fail_count : 1, "%.250s", v2.error_detail);
        note_violation(out, v2.error_detail, NULL, "V2 properties hold", v2.error_detail);
    }
}

int visor_verify_object(const OmegaGraph *g, const SemanticId *id,
                        const RealizationObject *real_or_null,
                        const OmegaMachineGraph *mg_or_null,
                        VisorVerifyReport *out) {
    if (!out) return -1;
    report_reset(out);
    if (!g || !id || g->object_count > OMEGA_MAX_GRAPH_OBJECTS) {
        note_violation(out, "invalid arguments or malformed graph header", NULL, "", "");
        return -1;
    }
    const OmegaObject *obj = omega_graph_find_object_const(g, id);
    if (!obj) {
        row_set(out, VISOR_ROW_STRUCTURAL, VISOR_CHECK_FAIL, 1, 1, "object not present in graph");
        note_violation(out, "object not present in graph", id, "object present", "absent");
        finalize(out);
        return 0;
    }

    /* STRUCTURAL: V0 over the graph (realization structure is covered by V1). */
    VerifyReport v0;
    if (omega_verify_v0_structural(g, NULL, &v0) == 0 && v0.passed) {
        row_set(out, VISOR_ROW_STRUCTURAL, VISOR_CHECK_PASS, v0.check_count, v0.fail_count,
                "V0 graph typing + DAG integrity (%u object(s))", (unsigned)g->object_count);
    } else {
        row_set(out, VISOR_ROW_STRUCTURAL, VISOR_CHECK_FAIL, v0.check_count,
                v0.fail_count ? v0.fail_count : 1, "%.250s", v0.error_detail);
        note_violation(out, v0.error_detail, NULL, "graph passes V0 structural", v0.error_detail);
    }

    /* TYPE: the queried object alone. */
    char err[256] = {0};
    if (omega_validate_object(g, obj, err, sizeof(err)) == 0) {
        row_set(out, VISOR_ROW_TYPE, VISOR_CHECK_PASS, 1, 0, "%s object validates",
                as_apply(obj) ? "OPERATION(apply)" : visor_kind_name_local(obj->kind));
    } else {
        row_set(out, VISOR_ROW_TYPE, VISOR_CHECK_FAIL, 1, 1, "%.250s", err);
        note_violation(out, err, id, "object validates", err);
    }

    invariants_row(out);
    realization_row(g, id, obj, real_or_null, out);
    authority_row(g, (int)(obj - g->objects), out);
    machine_row(real_or_null, mg_or_null, out);
    finalize(out);
    return 0;
}

/* Program: the same tiers omega_program_verify runs (V0 on the realization,
 * then V2), called individually so each row keeps its own counts, and
 * const-clean (omega_program_verify mutates is_verified). */
int visor_verify_program(const OmegaProgram *p, VisorVerifyReport *out) {
    if (!out) return -1;
    report_reset(out);
    if (!p) {
        note_violation(out, "invalid arguments", NULL, "", "");
        return -1;
    }
    if (!p->is_realized) {
        row_set(out, VISOR_ROW_STRUCTURAL, VISOR_CHECK_FAIL, 1, 1, "program not realized; nothing to verify");
        note_violation(out, "program not realized", &p->program_id, "realized program", "not realized");
        finalize(out);
        return 0;
    }

    VerifyReport v0;
    if (omega_verify_v0_structural(NULL, &p->realization, &v0) == 0 && v0.passed) {
        row_set(out, VISOR_ROW_STRUCTURAL, VISOR_CHECK_PASS, v0.check_count, v0.fail_count,
                "V0 realization: profile, bounds, decode, terminal RET, realization id (%zu bytes)",
                p->realization.code_len);
    } else {
        row_set(out, VISOR_ROW_STRUCTURAL, VISOR_CHECK_FAIL, v0.check_count,
                v0.fail_count ? v0.fail_count : 1, "%.250s", v0.error_detail);
        note_violation(out, v0.error_detail, &p->program_id, "realization passes V0 structural", v0.error_detail);
    }

    char err[256] = {0};
    if (omega_program_validate_contract(p, err, sizeof(err)) == 0) {
        row_set(out, VISOR_ROW_TYPE, VISOR_CHECK_PASS, 1, 0, "contract validates (pre \"%.60s\", post \"%.60s\")",
                p->contract.precondition, p->contract.postcondition);
    } else {
        row_set(out, VISOR_ROW_TYPE, VISOR_CHECK_FAIL, 1, 1, "%.250s", err);
        note_violation(out, err, &p->program_id, "contract validates", err);
    }

    invariants_row(out);
    row_set(out, VISOR_ROW_REALIZATION, VISOR_CHECK_NOT_RUN, 0, 0,
            "program verify runs V0+V2 only; V1 reference (a+b)-c does not model unary programs");
    if (p->graph && p->graph->object_count > 0 && p->graph->object_count <= OMEGA_MAX_GRAPH_OBJECTS) {
        /* No single root: count requirement-bearing objects across the whole program graph. */
        uint32_t eff = 0, cap = 0;
        for (uint16_t i = 0; i < p->graph->object_count; ++i) {
            const OmegaObject *o = &p->graph->objects[i];
            if (o->kind == KIND_EFFECT) eff++;
            if (o->kind == KIND_TYPE && o->payload_len >= sizeof(TypePayload)) {
                TypeTag t = ((const TypePayload *)o->payload)->tag;
                if (t == TYPE_CAPABILITY_REF || t == TYPE_EFFECT_INTENT_REF || t == TYPE_EFFECT_RECEIPT_REF) cap++;
            }
        }
        out->effect_count = eff;
        out->capability_ref_count = cap;
        out->authority_required = (eff + cap) > 0;
        row_set(out, VISOR_ROW_AUTHORITY, VISOR_CHECK_PASS, p->graph->object_count, 0,
                "%s: %u effect(s), %u capability ref(s) in program graph; visor grants nothing",
                out->authority_required ? "REQUIRED" : "NONE", eff, cap);
    } else {
        row_set(out, VISOR_ROW_AUTHORITY, VISOR_CHECK_NOT_RUN, 0, 0,
                "program has no semantic graph; requirements not derivable");
    }
    row_set(out, VISOR_ROW_MACHINE, VISOR_CHECK_NOT_RUN, 0, 0, "no machine model");
    finalize(out);
    return 0;
}

/* --- rendering --- */
typedef struct { char *p; size_t n, len; bool overflow; } VBuf;

static void vb_printf(VBuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void vb_printf(VBuf *b, const char *fmt, ...) {
    if (b->overflow) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(b->p + b->len, b->n - b->len, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= b->n - b->len) { b->overflow = true; b->p[b->len] = 0; return; }
    b->len += (size_t)w;
}

static void vb_json_str(VBuf *b, const char *s) {
    vb_printf(b, "\"");
    for (const unsigned char *c = (const unsigned char *)(s ? s : ""); *c && !b->overflow; ++c) {
        if (*c == '"' || *c == '\\') vb_printf(b, "\\%c", *c);
        else if (*c == '\n') vb_printf(b, "\\n");
        else if (*c < 0x20) vb_printf(b, "\\u%04x", *c);
        else vb_printf(b, "%c", *c);
    }
    vb_printf(b, "\"");
}

int visor_verify_format_text(const VisorVerifyReport *r, char *out, size_t n) {
    if (!r || !out || n == 0) return -1;
    VBuf b = { out, n, 0, false };
    out[0] = 0;
    for (size_t i = 0; i < r->row_count && i < 8; ++i) {
        const VisorVerifyRow *row = &r->rows[i];
        vb_printf(&b, "%-16s%-8s %s\n", row->name ? row->name : "?",
                  visor_check_status_name(row->status), row->detail);
    }
    vb_printf(&b, "%-16s%s\n", "VERDICT", r->passed ? "PASS" : "FAIL");
    if (!r->passed && r->first_violation[0]) {
        vb_printf(&b, "first violation: %s\n", r->first_violation);
        if (r->object_involved[0]) vb_printf(&b, "object:          %s\n", r->object_involved);
        if (r->expected[0]) vb_printf(&b, "expected:        %s\n", r->expected);
        if (r->observed[0]) vb_printf(&b, "observed:        %s\n", r->observed);
    }
    return b.overflow ? -1 : (int)b.len;
}

int visor_verify_format_json(const VisorVerifyReport *r, char *out, size_t n) {
    if (!r || !out || n == 0) return -1;
    VBuf b = { out, n, 0, false };
    out[0] = 0;
    vb_printf(&b, "{\"passed\":%s,\"rows\":[", r->passed ? "true" : "false");
    for (size_t i = 0; i < r->row_count && i < 8; ++i) {
        const VisorVerifyRow *row = &r->rows[i];
        vb_printf(&b, "%s{\"name\":", i ? "," : "");
        vb_json_str(&b, row->name);
        vb_printf(&b, ",\"status\":\"%s\",\"checks\":%u,\"fails\":%u,\"detail\":",
                  visor_check_status_name(row->status), row->checks, row->fails);
        vb_json_str(&b, row->detail);
        vb_printf(&b, "}");
    }
    vb_printf(&b, "],\"authority\":{\"required\":%s,\"effects\":%u,\"capability_refs\":%u,\"granted\":false}",
              r->authority_required ? "true" : "false", r->effect_count, r->capability_ref_count);
    if (r->first_violation[0]) {
        vb_printf(&b, ",\"failure\":{\"first_violation\":");
        vb_json_str(&b, r->first_violation);
        vb_printf(&b, ",\"object\":");
        vb_json_str(&b, r->object_involved);
        vb_printf(&b, ",\"expected\":");
        vb_json_str(&b, r->expected);
        vb_printf(&b, ",\"observed\":");
        vb_json_str(&b, r->observed);
        vb_printf(&b, "}");
    } else {
        vb_printf(&b, ",\"failure\":null");
    }
    vb_printf(&b, "}");
    return b.overflow ? -1 : (int)b.len;
}
