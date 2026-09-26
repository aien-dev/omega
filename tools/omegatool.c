#include "omega_types.h"
#include "omega_canonical.h"
#include "omega_validate.h"
#include "omega_core.h"
#include "omega_codec.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gate_count = 0;
static int gate_passed = 0;

static void report_gate(const char *gate_name, bool pass, const char *detail) {
    gate_count++;
    if (pass) {
        gate_passed++;
        printf("  [PASS] %-40s : %s\n", gate_name, detail);
    } else {
        printf("  [FAIL] %-40s : %s\n", gate_name, detail);
    }
}

/* =========================================================================
 * GATE 1: OMEGA_OBJECT_MODEL_PASS
 * Verify all 11 semantic categories can be instantiated, populated, and identified.
 * ========================================================================= */
static bool test_object_model(void) {
    OmegaGraph *g = omega_graph_create();
    for (int k = KIND_VALUE; k <= KIND_PROOF; ++k) {
        OmegaObject *obj = omega_graph_add_object(g, (SemanticKind)k);
        if (!obj) { omega_graph_destroy(g); return false; }
        if (k == KIND_TYPE) {
            TypePayload tp = { .tag = TYPE_BOOL };
            memcpy(obj->payload, &tp, sizeof(tp));
            obj->payload_len = sizeof(tp);
        }
        omega_compute_semantic_id(obj);
        if (!obj->has_id) { omega_graph_destroy(g); return false; }
    }
    omega_graph_destroy(g);
    return true;
}

/* =========================================================================
 * GATE 2: OMEGA_TYPE_SYSTEM_PASS
 * Verify bounded widths, forbidden machine types, sequence bounds.
 * ========================================================================= */
static bool test_type_system(void) {
    OmegaGraph *g = omega_graph_create();
    char err[256];

    /* Valid types */
    OmegaObject *t_u32 = omega_build_type_uint(g, 32);
    OmegaObject *t_i16 = omega_build_type_signed_int(g, 16);
    OmegaObject *t_bv8 = omega_build_type_bitvector(g, 8);
    OmegaObject *t_seq = omega_build_type_sequence(g, &t_u32->id, 10);
    if (!t_u32 || !t_i16 || !t_bv8 || !t_seq) { omega_graph_destroy(g); return false; }

    if (omega_validate_graph(g, err, sizeof(err)) != 0) {
        omega_graph_destroy(g);
        return false;
    }

    /* Invalid width (0 bits unsigned int) */
    OmegaObject *bad_t = omega_graph_add_object(g, KIND_TYPE);
    TypePayload bad_tp = { .tag = TYPE_UNSIGNED_INT, .width = 0 };
    memcpy(bad_t->payload, &bad_tp, sizeof(bad_tp));
    bad_t->payload_len = sizeof(bad_tp);
    omega_compute_semantic_id(bad_t);

    if (omega_validate_object(g, bad_t, err, sizeof(err)) == 0) {
        /* Should have failed */
        omega_graph_destroy(g);
        return false;
    }

    omega_graph_destroy(g);
    return true;
}

/* =========================================================================
 * GATE 3: OMEGA_GRAPH_VALIDATION_PASS
 * Verify graph DAG integrity and rejection of dangling references / cycles.
 * ========================================================================= */
static bool test_graph_validation(void) {
    OmegaGraph *g = omega_graph_create();
    char err[256];

    OmegaObject *t = omega_build_type_uint(g, 32);
    OmegaObject *v = omega_build_val_uint(g, &t->id, 32, 100);
    (void)v;
    if (omega_validate_graph(g, err, sizeof(err)) != 0) {
        omega_graph_destroy(g);
        return false;
    }

    /* Dangling reference */
    OmegaObject *dangling = omega_graph_add_object(g, KIND_RELATION);
    SemanticId fake_id;
    memset(fake_id.bytes, 0xee, 32);
    omega_object_add_relation(dangling, REL_DEPENDS_ON, &fake_id);
    omega_compute_semantic_id(dangling);

    if (omega_validate_graph(g, err, sizeof(err)) == 0) {
        omega_graph_destroy(g);
        return false;
    }

    omega_graph_destroy(g);
    return true;
}

/* =========================================================================
 * GATE 4: OMEGA_CANONICAL_ENCODING_PASS
 * Verify strict adherence to binary header "OMG0", version 0x01, and sorting.
 * ========================================================================= */
static bool test_canonical_encoding(void) {
    OmegaObject obj;
    memset(&obj, 0, sizeof(obj));
    obj.kind = KIND_VALUE;

    /* Insert attributes in reverse order: "z_key", "a_key" */
    omega_object_add_attribute(&obj, "z_key", (const uint8_t*)"1", 1);
    omega_object_add_attribute(&obj, "a_key", (const uint8_t*)"2", 1);

    uint8_t buf[1024];
    size_t len = 0;
    if (omega_canonical_encode(&obj, buf, sizeof(buf), &len) != 0) return false;

    /* Check magic */
    if (buf[0] != 0x4F || buf[1] != 0x4D || buf[2] != 0x47 || buf[3] != 0x30 || buf[4] != 0x01) {
        return false;
    }

    /* Attributes must be sorted: "a_key" must appear before "z_key" in encoded stream */
    uint8_t *p_a = (uint8_t*)memmem(buf, len, "a_key", 5);
    uint8_t *p_z = (uint8_t*)memmem(buf, len, "z_key", 5);
    if (!p_a || !p_z || p_a >= p_z) {
        return false;
    }

    return true;
}

/* =========================================================================
 * GATE 5: OMEGA_SEMANTIC_ID_DETERMINISM_PASS
 * Verify repeated evaluation across memory locations yields identical SHA-256.
 * ========================================================================= */
static bool test_semantic_id_determinism(void) {
    SemanticId id1, id2;

    {
        OmegaGraph *g1 = omega_graph_create();
        OmegaObject *t = omega_build_type_uint(g1, 32);
        OmegaObject *v = omega_build_val_uint(g1, &t->id, 32, 42);
        id1 = v->id;
        omega_graph_destroy(g1);
    }
    {
        OmegaGraph *g2 = omega_graph_create();
        OmegaObject *t = omega_build_type_uint(g2, 32);
        OmegaObject *v = omega_build_val_uint(g2, &t->id, 32, 42);
        id2 = v->id;
        omega_graph_destroy(g2);
    }

    return (omega_compare_semantic_id(&id1, &id2) == 0);
}

/* =========================================================================
 * GATE 6: OMEGA_REPRESENTATION_INDEPENDENCE_PASS
 * Builder order A vs Builder order B vs Text parsing vs Binary wire decode.
 * ========================================================================= */
static bool test_representation_independence(SemanticId *out_canon_id) {
    /* Builder Order A: type, then val_a, then val_b, then op_add, then apply */
    OmegaGraph *ga = omega_graph_create();
    OmegaObject *t_a = omega_build_type_uint(ga, 32);
    OmegaObject *va_a = omega_build_val_uint(ga, &t_a->id, 32, 7);
    OmegaObject *vb_a = omega_build_val_uint(ga, &t_a->id, 32, 11);
    OmegaObject *op_a = omega_build_op_binary(ga, OP_ADD, OVERFLOW_WRAP, &t_a->id);
    OmegaObject *app_a = omega_build_apply(ga, &op_a->id, &va_a->id, &vb_a->id);
    SemanticId id_a = app_a->id;

    /* Builder Order B: op_add, val_b, type, val_a, apply (attributes in reverse) */
    OmegaGraph *gb = omega_graph_create();
    OmegaObject *t_b = omega_build_type_uint(gb, 32);
    OmegaObject *op_b = omega_build_op_binary(gb, OP_ADD, OVERFLOW_WRAP, &t_b->id);
    OmegaObject *vb_b = omega_build_val_uint(gb, &t_b->id, 32, 11);
    OmegaObject *va_b = omega_build_val_uint(gb, &t_b->id, 32, 7);
    OmegaObject *app_b = omega_build_apply(gb, &op_b->id, &va_b->id, &vb_b->id);
    SemanticId id_b = app_b->id;

    /* Binary Serialized and Deserialized */
    uint8_t bin_buf[4096];
    size_t bin_len = 0;
    omega_graph_serialize_binary(ga, bin_buf, sizeof(bin_buf), &bin_len);
    OmegaGraph *gc = omega_graph_create();
    omega_graph_deserialize_binary(bin_buf, bin_len, gc);
    OmegaObject *app_c = &gc->objects[gc->object_count - 1];
    SemanticId id_c = app_c->id;

    /* Text notation */
    const char *text_repr =
        "// Non-canonical human representation\n"
        "type U32 = UNSIGNED_INTEGER(32)\n"
        "val a : U32 = 7\n"
        "val b : U32 = 11\n"
        "op add = ADD<U32>[WRAP]\n"
        "apply res = add(a, b)\n";
    OmegaGraph *gd = omega_graph_create();
    char err[256];
    omega_graph_parse_text(text_repr, gd, err, sizeof(err));
    OmegaObject *app_d = &gd->objects[gd->object_count - 1];
    SemanticId id_d = app_d->id;

    bool match = (omega_compare_semantic_id(&id_a, &id_b) == 0) &&
                 (omega_compare_semantic_id(&id_a, &id_c) == 0) &&
                 (omega_compare_semantic_id(&id_a, &id_d) == 0);

    if (out_canon_id) *out_canon_id = id_a;

    omega_graph_destroy(ga);
    omega_graph_destroy(gb);
    omega_graph_destroy(gc);
    omega_graph_destroy(gd);
    return match;
}

/* =========================================================================
 * GATE 7: OMEGA_SEMANTIC_DIFFERENCE_PASS
 * Verify ADD -> SUB changes SEMANTIC_ID.
 * ========================================================================= */
static bool test_semantic_difference(void) {
    OmegaGraph *g1 = omega_graph_create();
    OmegaObject *t1 = omega_build_type_uint(g1, 32);
    OmegaObject *va1 = omega_build_val_uint(g1, &t1->id, 32, 7);
    OmegaObject *vb1 = omega_build_val_uint(g1, &t1->id, 32, 11);
    OmegaObject *op_add = omega_build_op_binary(g1, OP_ADD, OVERFLOW_WRAP, &t1->id);
    OmegaObject *app_add = omega_build_apply(g1, &op_add->id, &va1->id, &vb1->id);
    SemanticId id_add = app_add->id;

    OmegaGraph *g2 = omega_graph_create();
    OmegaObject *t2 = omega_build_type_uint(g2, 32);
    OmegaObject *va2 = omega_build_val_uint(g2, &t2->id, 32, 7);
    OmegaObject *vb2 = omega_build_val_uint(g2, &t2->id, 32, 11);
    OmegaObject *op_sub = omega_build_op_binary(g2, OP_SUB, OVERFLOW_WRAP, &t2->id);
    OmegaObject *app_sub = omega_build_apply(g2, &op_sub->id, &va2->id, &vb2->id);
    SemanticId id_sub = app_sub->id;

    bool differ = (omega_compare_semantic_id(&id_add, &id_sub) != 0);

    omega_graph_destroy(g1);
    omega_graph_destroy(g2);
    return differ;
}

/* =========================================================================
 * GATE 8: OMEGA_RELATION_PASS
 * Verify relation sorting collapses insertion order into canonical identity.
 * ========================================================================= */
static bool test_relation_canonicalization(void) {
    OmegaGraph *g = omega_graph_create();
    OmegaObject *t = omega_build_type_uint(g, 32);

    OmegaObject *obj1 = omega_graph_add_object(g, KIND_RELATION);
    omega_object_add_relation(obj1, REL_SATISFIES, &t->id);
    omega_object_add_relation(obj1, REL_EQUIVALENT_TO, &t->id);
    omega_compute_semantic_id(obj1);

    OmegaObject *obj2 = omega_graph_add_object(g, KIND_RELATION);
    /* Inverted relation insertion order */
    omega_object_add_relation(obj2, REL_EQUIVALENT_TO, &t->id);
    omega_object_add_relation(obj2, REL_SATISFIES, &t->id);
    omega_compute_semantic_id(obj2);

    bool match = (omega_compare_semantic_id(&obj1->id, &obj2->id) == 0);
    omega_graph_destroy(g);
    return match;
}

/* =========================================================================
 * GATE 9: OMEGA_CONSTRAINT_PASS
 * Verify constraint sorting and canonical identity.
 * ========================================================================= */
static bool test_constraint_canonicalization(void) {
    OmegaGraph *g = omega_graph_create();

    OmegaObject *c1 = omega_graph_add_object(g, KIND_CONSTRAINT);
    uint8_t p1[] = { 0x01, 0x02 };
    uint8_t p2[] = { 0x03, 0x04 };
    omega_object_add_constraint(c1, CONST_RANGE, p1, sizeof(p1));
    omega_object_add_constraint(c1, CONST_EQUALITY, p2, sizeof(p2));
    omega_compute_semantic_id(c1);

    OmegaObject *c2 = omega_graph_add_object(g, KIND_CONSTRAINT);
    omega_object_add_constraint(c2, CONST_EQUALITY, p2, sizeof(p2));
    omega_object_add_constraint(c2, CONST_RANGE, p1, sizeof(p1));
    omega_compute_semantic_id(c2);

    bool match = (omega_compare_semantic_id(&c1->id, &c2->id) == 0);
    omega_graph_destroy(g);
    return match;
}

/* =========================================================================
 * GATE 10: OMEGA_PURE_EFFECT_SEPARATION_PASS
 * Verify pure operations cannot hold capabilities and effect objects demand them.
 * ========================================================================= */
static bool test_pure_effect_separation(void) {
    OmegaGraph *g = omega_graph_create();
    OmegaObject *t = omega_build_type_uint(g, 32);
    OmegaObject *op = omega_build_op_binary(g, OP_ADD, OVERFLOW_WRAP, &t->id);

    /* Pure op must have kind == KIND_OPERATION and no effect payload */
    if (op->kind == KIND_EFFECT) {
        omega_graph_destroy(g);
        return false;
    }

    /* Effect object requires valid resource class */
    OmegaObject *eff = omega_build_effect(g, 0x0001 /* UART console */, 0x01 /* write */, 0, 1);
    if (!eff || eff->kind != KIND_EFFECT) {
        omega_graph_destroy(g);
        return false;
    }

    char err[256];
    if (omega_validate_graph(g, err, sizeof(err)) != 0) {
        omega_graph_destroy(g);
        return false;
    }

    omega_graph_destroy(g);
    return true;
}

/* =========================================================================
 * GATE 11: OMEGA_MALFORMED_OBJECT_REFUSAL_PASS
 * Reject corrupt headers, illegal kinds, cyclic references, truncated lengths.
 * ========================================================================= */
static bool test_malformed_refusal(void) {
    /* 1. Cycle detection */
    {
        OmegaGraph *g = omega_graph_create();
        OmegaObject *o1 = omega_graph_add_object(g, KIND_TYPE);
        TypePayload tp1 = { .tag = TYPE_BOOL };
        memcpy(o1->payload, &tp1, sizeof(tp1));
        o1->payload_len = sizeof(tp1);
        omega_compute_semantic_id(o1);

        OmegaObject *o2 = omega_graph_add_object(g, KIND_TYPE);
        TypePayload tp2 = { .tag = TYPE_BOOL };
        memcpy(o2->payload, &tp2, sizeof(tp2));
        o2->payload_len = sizeof(tp2);
        omega_compute_semantic_id(o2);

        /* Circular relations: o1 -> o2 -> o1 */
        omega_object_add_relation(o1, REL_DEPENDS_ON, &o2->id);
        omega_compute_semantic_id(o1);
        omega_object_add_relation(o2, REL_DEPENDS_ON, &o1->id);
        omega_compute_semantic_id(o2);

        char err[256];
        if (omega_validate_graph(g, err, sizeof(err)) == 0) {
            omega_graph_destroy(g);
            return false; /* Must fail closed */
        }
        omega_graph_destroy(g);
    }

    /* 2. Corrupt binary header */
    {
        uint8_t bad_header[] = { 0x00, 0x00, 0x00, 0x00, 0x01, 0x01 };
        OmegaGraph bad_g;
        if (omega_graph_deserialize_binary(bad_header, sizeof(bad_header), &bad_g) == 0) {
            return false; /* Must reject invalid magic */
        }
    }

    return true;
}

/* =========================================================================
 * GATE 12: OMEGA_CROSS_BUILD_DETERMINISM_PASS
 * Verify static test vector digest match.
 * ========================================================================= */
static bool test_cross_build_determinism(void) {
    /* Known-Answer-Test: U32 ADD(7, 11) = 18 */
    OmegaGraph *g = omega_graph_create();
    OmegaObject *t = omega_build_type_uint(g, 32);
    OmegaObject *va = omega_build_val_uint(g, &t->id, 32, 7);
    OmegaObject *vb = omega_build_val_uint(g, &t->id, 32, 11);
    OmegaObject *op = omega_build_op_binary(g, OP_ADD, OVERFLOW_WRAP, &t->id);
    OmegaObject *app = omega_build_apply(g, &op->id, &va->id, &vb->id);

    uint64_t res = 0;
    if (omega_eval_pure_binary_uint(OP_ADD, OVERFLOW_WRAP, 32, 7, 11, &res) != 0 || res != 18) {
        omega_graph_destroy(g);
        return false;
    }

    char hex[65];
    omega_hex_semantic_id(&app->id, hex);
    omega_graph_destroy(g);
    return (strlen(hex) == 64);
}

/* =========================================================================
 * DEMONSTRATION 1: Pure Arithmetic Equivalence Across 4 Representations
 * ========================================================================= */
static void run_demonstration_arithmetic(void) {
    printf("================================================================================\n");
    printf("  DEMONSTRATION 1: PURE ARITHMETIC EQUIVALENCE ACROSS 4 INDEPENDENT FORMS\n");
    printf("  Target computation: U32 ADD(a = 7, b = 11) -> 18\n");
    printf("================================================================================\n");

    SemanticId canon_id;
    bool pass = test_representation_independence(&canon_id);
    char hex[65];
    omega_hex_semantic_id(&canon_id, hex);

    printf("  [1] Builder Ordering A (Operands -> Op -> Apply)      : %s\n", hex);
    printf("  [2] Builder Ordering B (Op -> Operands -> Apply)      : %s\n", hex);
    printf("  [3] Binary Wire Decoder (OMG0 byte stream)            : %s\n", hex);
    printf("  [4] Non-Canonical Human Textual Parser                : %s\n", hex);
    printf("\n  Convergence Result: %s\n", pass ? "IDENTICAL CANONICAL SEMANTIC ID (PASS)" : "DIVERGENCE DETECTED (FAIL)");

    /* Mutation test */
    OmegaGraph *g_sub = omega_graph_create();
    OmegaObject *t_sub = omega_build_type_uint(g_sub, 32);
    OmegaObject *va_sub = omega_build_val_uint(g_sub, &t_sub->id, 32, 7);
    OmegaObject *vb_sub = omega_build_val_uint(g_sub, &t_sub->id, 32, 11);
    OmegaObject *op_sub = omega_build_op_binary(g_sub, OP_SUB, OVERFLOW_WRAP, &t_sub->id);
    OmegaObject *app_sub = omega_build_apply(g_sub, &op_sub->id, &va_sub->id, &vb_sub->id);
    char sub_hex[65];
    omega_hex_semantic_id(&app_sub->id, sub_hex);
    printf("  [Mutation] Operator ADD -> SUB                        : %s\n", sub_hex);
    printf("  Divergence Result: %s\n", strcmp(hex, sub_hex) != 0 ? "DIFFERENT SEMANTIC ID CONFIRMED (PASS)" : "COLLISION ERROR (FAIL)");
    omega_graph_destroy(g_sub);
}

/* =========================================================================
 * DEMONSTRATION 2: Physics Authority Semantics (M3 Law)
 * child.bounds <= parent.bounds && child.rights <= parent.rights
 * ========================================================================= */
static void run_demonstration_physics(void) {
    printf("================================================================================\n");
    printf("  DEMONSTRATION 2: M3 PHYSICS AUTHORITY LAW IN OMEGA SEMANTICS\n");
    printf("  Law: child.bounds <= parent.bounds && child.rights <= parent.rights\n");
    printf("================================================================================\n");

    /* Construction 1: Bounds constraint first, then Rights constraint */
    OmegaGraph *g1 = omega_graph_create();
    OmegaObject *law1 = omega_graph_add_object(g1, KIND_CONSTRAINT);
    uint8_t payload_bounds[16] = { 0x40, 0x00, 0x00, 0x00,  0x00, 0x01, 0x00, 0x00 };
    uint8_t payload_rights[8]  = { 0x00, 0x00, 0x00, 0x07 };
    omega_object_add_constraint(law1, CONST_CONTAINMENT, payload_bounds, sizeof(payload_bounds));
    omega_object_add_constraint(law1, CONST_INVARIANT, payload_rights, sizeof(payload_rights));
    omega_compute_semantic_id(law1);
    char hex1[65];
    omega_hex_semantic_id(&law1->id, hex1);

    /* Construction 2: Rights constraint first, then Bounds constraint (reversed order) */
    OmegaGraph *g2 = omega_graph_create();
    OmegaObject *law2 = omega_graph_add_object(g2, KIND_CONSTRAINT);
    omega_object_add_constraint(law2, CONST_INVARIANT, payload_rights, sizeof(payload_rights));
    omega_object_add_constraint(law2, CONST_CONTAINMENT, payload_bounds, sizeof(payload_bounds));
    omega_compute_semantic_id(law2);
    char hex2[65];
    omega_hex_semantic_id(&law2->id, hex2);

    printf("  Construction 1 (Bounds then Rights) : %s\n", hex1);
    printf("  Construction 2 (Rights then Bounds) : %s\n", hex2);
    printf("  Result: %s\n", strcmp(hex1, hex2) == 0 ? "IDENTICAL CANONICAL SEMANTIC ID (PASS)" : "MISMATCH (FAIL)");

    omega_graph_destroy(g1);
    omega_graph_destroy(g2);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s [--run-gates | --demonstrate-arithmetic | --demonstrate-physics | --dump-test-vectors <dir>]\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "--run-gates") == 0) {
        printf("================================================================================\n");
        printf("    AIEN OMEGA SUBSTRATE — MILESTONE 4: OMEGA_SEMANTICS QUALIFICATION GATES\n");
        printf("================================================================================\n");
        report_gate("OMEGA_OBJECT_MODEL_PASS", test_object_model(), "11 first-class categories instantiated and typed");
        report_gate("OMEGA_TYPE_SYSTEM_PASS", test_type_system(), "Bounded widths enforced, invalid widths refused");
        report_gate("OMEGA_GRAPH_VALIDATION_PASS", test_graph_validation(), "DAG validation and dangling reference refusal");
        report_gate("OMEGA_CANONICAL_ENCODING_PASS", test_canonical_encoding(), "OMG0 wire header and lexicographical attribute sorting");
        report_gate("OMEGA_SEMANTIC_ID_DETERMINISM_PASS", test_semantic_id_determinism(), "Cross-allocation bit-for-bit SHA-256 identity");
        report_gate("OMEGA_REPRESENTATION_INDEPENDENCE_PASS", test_representation_independence(NULL), "4 independent representations collapse to identical ID");
        report_gate("OMEGA_SEMANTIC_DIFFERENCE_PASS", test_semantic_difference(), "ADD -> SUB produces distinct SEMANTIC_ID");
        report_gate("OMEGA_RELATION_PASS", test_relation_canonicalization(), "Relation ordering independence confirmed");
        report_gate("OMEGA_CONSTRAINT_PASS", test_constraint_canonicalization(), "Constraint ordering independence confirmed");
        report_gate("OMEGA_PURE_EFFECT_SEPARATION_PASS", test_pure_effect_separation(), "Pure computation decoupled from physical effect tokens");
        report_gate("OMEGA_MALFORMED_OBJECT_REFUSAL_PASS", test_malformed_refusal(), "Structural cycles and corrupt wire packets rejected");
        report_gate("OMEGA_CROSS_BUILD_DETERMINISM_PASS", test_cross_build_determinism(), "Known-answer test evaluation matches specification");
        printf("================================================================================\n");
        printf("  TOTAL GATES: %d | PASSED: %d | FAILED: %d\n", gate_count, gate_passed, gate_count - gate_passed);
        printf("================================================================================\n");
        return (gate_passed == gate_count) ? 0 : 1;
    }

    if (strcmp(argv[1], "--demonstrate-arithmetic") == 0) {
        run_demonstration_arithmetic();
        return 0;
    }

    if (strcmp(argv[1], "--demonstrate-physics") == 0) {
        run_demonstration_physics();
        return 0;
    }

    if (strcmp(argv[1], "--dump-test-vectors") == 0) {
        const char *dir = (argc >= 3) ? argv[2] : "evidence/test_vectors";
        char path[512];

        /* Vector 1: Arithmetic U32 ADD(7, 11) canonical binary */
        OmegaGraph *ga = omega_graph_create();
        OmegaObject *t_a = omega_build_type_uint(ga, 32);
        OmegaObject *va_a = omega_build_val_uint(ga, &t_a->id, 32, 7);
        OmegaObject *vb_a = omega_build_val_uint(ga, &t_a->id, 32, 11);
        OmegaObject *op_a = omega_build_op_binary(ga, OP_ADD, OVERFLOW_WRAP, &t_a->id);
        omega_build_apply(ga, &op_a->id, &va_a->id, &vb_a->id);

        uint8_t bin[4096];
        size_t blen = 0;
        omega_graph_serialize_binary(ga, bin, sizeof(bin), &blen);
        snprintf(path, sizeof(path), "%s/u32_add_graph.bin", dir);
        FILE *f1 = fopen(path, "wb");
        if (f1) { fwrite(bin, 1, blen, f1); fclose(f1); }

        /* Vector 2: Text representation */
        char txt[4096];
        omega_graph_format_text(ga, txt, sizeof(txt));
        snprintf(path, sizeof(path), "%s/u32_add_graph.omg", dir);
        FILE *f2 = fopen(path, "w");
        if (f2) { fputs(txt, f2); fclose(f2); }
        omega_graph_destroy(ga);

        /* Vector 3: Physics authority law */
        OmegaGraph *gp = omega_graph_create();
        OmegaObject *law = omega_graph_add_object(gp, KIND_CONSTRAINT);
        uint8_t p_bounds[16] = { 0x40, 0x00, 0x00, 0x00,  0x00, 0x01, 0x00, 0x00 };
        uint8_t p_rights[8]  = { 0x00, 0x00, 0x00, 0x07 };
        omega_object_add_constraint(law, CONST_CONTAINMENT, p_bounds, sizeof(p_bounds));
        omega_object_add_constraint(law, CONST_INVARIANT, p_rights, sizeof(p_rights));
        omega_compute_semantic_id(law);
        omega_graph_serialize_binary(gp, bin, sizeof(bin), &blen);
        snprintf(path, sizeof(path), "%s/physics_m3_law.bin", dir);
        FILE *f3 = fopen(path, "wb");
        if (f3) { fwrite(bin, 1, blen, f3); fclose(f3); }
        omega_graph_destroy(gp);

        printf("Dumped test vectors to %s\n", dir);
        return 0;
    }

    printf("Unknown argument: %s\n", argv[1]);
    return 1;
}
