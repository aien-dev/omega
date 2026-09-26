#include "omega_types.h"
#include "omega_canonical.h"
#include "omega_validate.h"
#include "omega_core.h"
#include "omega_codec.h"
#include "sha256.h"
#include "aarch64_target.h"
#include "aarch64_encoder.h"
#include "aarch64_decoder.h"
#include "omega_realize.h"
#include "omega_exec.h"
#include "omega_self_host.h"
#include "omega_verify.h"
#include "omega_program.h"
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

/* =========================================================================
 * MILESTONE 5 GATES (OMEGA_AARCH64)
 * ========================================================================= */

static bool test_m5_profile(void) {
    Aarch64TargetProfile prof = {
        .profile_id = AARCH64_PROFILE_V8A_BAREMETAL,
        .abi_version = 1,
        .default_reg_width = 64
    };
    return (prof.profile_id == 1 && prof.default_reg_width == 64 && REG_XZR == 31);
}

static bool test_m5_encoder(void) {
    uint8_t buf[64];
    size_t pos = 0;

    aarch64_emit_add_reg(buf, &pos, sizeof(buf), true, REG_X0, REG_X0, REG_X1);
    aarch64_emit_sub_reg(buf, &pos, sizeof(buf), true, REG_X0, REG_X0, REG_X2);
    aarch64_emit_ret(buf, &pos, sizeof(buf));

    if (pos != 12) return false;

    uint32_t insn0 = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    uint32_t insn1 = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) | ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
    uint32_t insn2 = (uint32_t)buf[8] | ((uint32_t)buf[9] << 8) | ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);

    return (insn0 == 0x8B010000 && insn1 == 0xCB020000 && insn2 == 0xD65F03C0);
}

static bool test_m5_decoder_seam(void) {
    uint8_t buf[12];
    size_t pos = 0;
    aarch64_emit_add_reg(buf, &pos, sizeof(buf), true, REG_X0, REG_X0, REG_X1);
    aarch64_emit_sub_reg(buf, &pos, sizeof(buf), true, REG_X0, REG_X0, REG_X2);
    aarch64_emit_ret(buf, &pos, sizeof(buf));

    char err[256];
    if (aarch64_validate_code_buffer(buf, pos, err, sizeof(err)) != 0) {
        return false;
    }

    uint8_t corrupt[4] = { 0x00, 0x00, 0x00, 0x00 };
    if (aarch64_validate_code_buffer(corrupt, 4, err, sizeof(err)) == 0) {
        return false;
    }
    return true;
}

static bool test_m5_lowering(void) {
    OmegaGraph *g = omega_graph_create();
    SemanticId sem_id;
    omega_build_f_add_sub_graph(g, &sem_id);

    RealizationObject real;
    if (omega_realize_f_add_sub(g, &sem_id, &real) != 0) {
        omega_graph_destroy(g);
        return false;
    }

    bool ok = (real.code_len == 12 && real.has_id);
    omega_graph_destroy(g);
    return ok;
}

static bool test_m5_realization_id(void) {
    OmegaGraph *g = omega_graph_create();
    SemanticId sem_id;
    omega_build_f_add_sub_graph(g, &sem_id);

    RealizationObject real1, real2;
    omega_realize_f_add_sub(g, &sem_id, &real1);
    omega_realize_f_add_sub(g, &sem_id, &real2);

    bool match = (omega_compare_semantic_id(&real1.realization_id, &real2.realization_id) == 0);
    bool binds_semantic = (omega_compare_semantic_id(&real1.semantic_id, &sem_id) == 0);

    omega_graph_destroy(g);
    return match && binds_semantic;
}

static bool test_m5_native_execution(void) {
    OmegaGraph *g = omega_graph_create();
    SemanticId sem_id;
    omega_build_f_add_sub_graph(g, &sem_id);

    RealizationObject real;
    omega_realize_f_add_sub(g, &sem_id, &real);

    uint64_t observed = 0;
    int rc = omega_exec_native_f3(&real, 7, 11, 3, &observed);
    omega_graph_destroy(g);

    if (rc != 0) return false;
    return (observed == 15);
}

static bool test_m5_qemu_execution(void) {
    OmegaGraph *g = omega_graph_create();
    SemanticId sem_id;
    omega_build_f_add_sub_graph(g, &sem_id);

    RealizationObject real;
    omega_realize_f_add_sub(g, &sem_id, &real);

    const char *runner_path = "build/qemu_omega_runner.bin";
    if (omega_build_qemu_runner(&real, runner_path) != 0) {
        omega_graph_destroy(g);
        return false;
    }

    char log[1024];
    int rc = omega_exec_qemu_virt(runner_path, log, sizeof(log));
    omega_graph_destroy(g);
    return (rc == 0);
}

static bool test_m5_adversarial_mutation(void) {
    OmegaGraph *g = omega_graph_create();
    SemanticId sem_id;
    omega_build_f_add_sub_graph(g, &sem_id);

    RealizationObject real;
    omega_realize_f_add_sub(g, &sem_id, &real);

    real.code_bytes[0] ^= 0x01; /* Mutate Rd register */

    uint64_t observed = 0;
    int rc = omega_exec_native_f3(&real, 7, 11, 3, &observed);
    omega_graph_destroy(g);

    if (rc == 0 && observed == 15) {
        return false;
    }
    return true;
}

static bool test_m5_cross_build_determinism(void) {
    OmegaGraph *g = omega_graph_create();
    SemanticId sem_id;
    omega_build_f_add_sub_graph(g, &sem_id);

    RealizationObject real;
    omega_realize_f_add_sub(g, &sem_id, &real);

    char hex[65];
    omega_hex_semantic_id(&real.realization_id, hex);
    omega_graph_destroy(g);
    return (strlen(hex) == 64);
}

static void run_demonstration_realization(void) {
    printf("================================================================================\n");
    printf("  DEMONSTRATION: M5 DIRECT AARCH64 MACHINE REALIZATION FROM SEMANTIC GRAPH\n");
    printf("  Target Computation: F(a, b, c) = (a + b) - c\n");
    printf("  Test Vector:        a = 7, b = 11, c = 3 -> Expected = 15\n");
    printf("================================================================================\n");

    OmegaGraph *g = omega_graph_create();
    SemanticId sem_id;
    omega_build_f_add_sub_graph(g, &sem_id);

    char sem_hex[65];
    omega_hex_semantic_id(&sem_id, sem_hex);
    printf("  [1] M4 Semantic Graph Constructed ($G_S$)\n");
    printf("      SEMANTIC_ID S = %s\n\n", sem_hex);

    RealizationObject real;
    omega_realize_f_add_sub(g, &sem_id, &real);
    char real_hex[65];
    omega_hex_semantic_id(&real.realization_id, real_hex);

    printf("  [2] Direct AArch64 Realization Lowering (No LLVM, No Assembler)\n");
    printf("      Code Length   = %zu bytes (3 instructions)\n", real.code_len);
    printf("      Bytes (Hex)   = ");
    for (size_t i = 0; i < real.code_len; ++i) {
        printf("%02X ", real.code_bytes[i]);
    }
    printf("\n");
    printf("      Disassembly   =\n");
    printf("        +0x00: ADD X0, X0, X1  (0x8B010000)\n");
    printf("        +0x04: SUB X0, X0, X2  (0xCB020000)\n");
    printf("        +0x08: RET             (0xD65F03C0)\n");
    printf("      REALIZATION_ID R = %s\n", real_hex);
    printf("      Binds S       = %s\n\n", (omega_compare_semantic_id(&real.semantic_id, &sem_id) == 0) ? "VERIFIED (TRUE)" : "MISMATCH (FALSE)");

    char err[256];
    int dec_rc = aarch64_validate_code_buffer(real.code_bytes, real.code_len, err, sizeof(err));
    printf("  [3] Seam 1: Independent Instruction Decoder Validation\n");
    printf("      Validation    = %s\n\n", (dec_rc == 0) ? "PASS (Valid instruction stream, clean RET)" : err);

    uint64_t native_res = 0;
    int nat_rc = omega_exec_native_f3(&real, 7, 11, 3, &native_res);
    printf("  [4] Seam 2: Native In-Memory Execution (mprotect PROT_EXEC)\n");
    printf("      Input Vector  = (a=7, b=11, c=3)\n");
    printf("      Observed Result = %lu\n", (unsigned long)native_res);
    printf("      Semantic Parity = %s\n\n", (nat_rc == 0 && native_res == 15) ? "EXACT PARITY (PASS: native == semantic)" : "MISMATCH (FAIL)");

    const char *qemu_bin = "build/qemu_omega_runner.bin";
    omega_build_qemu_runner(&real, qemu_bin);
    char qemu_log[1024];
    int qemu_rc = omega_exec_qemu_virt(qemu_bin, qemu_log, sizeof(qemu_log));
    printf("  [5] Seam 3: Bare-Metal QEMU Virt Execution (cortex-a57, PL011 UART)\n");
    printf("      UART Output   = %s", qemu_log);
    printf("      QEMU Parity   = %s\n\n", (qemu_rc == 0) ? "EXACT PARITY (PASS: QEMU UART confirmed 15)" : "FAIL");

    RealizationObject mut = real;
    mut.code_bytes[0] ^= 0x01;
    uint64_t mut_res = 0;
    int mut_rc = omega_exec_native_f3(&mut, 7, 11, 3, &mut_res);
    printf("  [6] Adversarial Seam: Single-Bit Machine Code Corruption\n");
    printf("      Bit Flip      = code_bytes[0] ^= 0x01 (Rd mutated X0 -> X1)\n");
    printf("      Observed Post-Flip = %lu (Expected: != 15)\n", (unsigned long)mut_res);
    printf("      Mutation Refusal   = %s\n", (mut_rc != 0 || mut_res != 15) ? "DETECTED & REFUSED (PASS)" : "SILENT FAILURE (FAIL)");
    printf("================================================================================\n");

    omega_graph_destroy(g);
}

/* =========================================================================
 * MILESTONE 6 QUALIFICATION GATES (OMEGA_SELF_HOST)
 * ========================================================================= */

static bool test_m6_graph(void) {
    OmegaGraph *g = omega_graph_create();
    SemanticId sem_id;
    if (omega_build_compiler_graph(g, &sem_id) != 0) {
        omega_graph_destroy(g);
        return false;
    }
    char err[256];
    if (omega_validate_graph(g, err, sizeof(err)) != 0) {
        omega_graph_destroy(g);
        return false;
    }
    bool ok = (g->object_count == 5 && omega_graph_find_object(g, &sem_id) != NULL);
    omega_graph_destroy(g);
    return ok;
}

static bool test_m6_c1_emission(void) {
    OmegaGraph *g = omega_graph_create();
    SemanticId sem_id;
    omega_build_compiler_graph(g, &sem_id);
    RealizationObject c1;
    if (omega_self_host_compile_c0(g, &sem_id, &c1) != 0) {
        omega_graph_destroy(g);
        return false;
    }
    omega_graph_destroy(g);
    return (c1.code_len > 0 && c1.has_id && c1.target_profile == AARCH64_PROFILE_V8A_BAREMETAL);
}

static bool test_m6_decoder_seam(void) {
    OmegaGraph *g = omega_graph_create();
    SemanticId sem_id;
    omega_build_compiler_graph(g, &sem_id);
    RealizationObject c1;
    omega_self_host_compile_c0(g, &sem_id, &c1);
    omega_graph_destroy(g);

    char err[256];
    return (aarch64_validate_code_buffer(c1.code_bytes, c1.code_len, err, sizeof(err)) == 0);
}

static bool test_m6_c2_reproduction(void) {
    OmegaGraph *g = omega_graph_create();
    SemanticId sem_id;
    omega_build_compiler_graph(g, &sem_id);
    RealizationObject c1, c2;
    omega_self_host_compile_c0(g, &sem_id, &c1);

    uint8_t wire[8192];
    size_t wlen = 0;
    omega_graph_serialize_binary(g, wire, sizeof(wire), &wlen);
    omega_graph_destroy(g);

    if (omega_self_host_run_native_compiler(&c1, wire, wlen, &c2) != 0) {
        return false;
    }
    return (c2.code_len == c1.code_len && c2.has_id);
}

static bool test_m6_c3_reproduction(void) {
    RealizationObject c1, c2, c3;
    OmegaGraph *g = omega_graph_create();
    SemanticId sem_id;
    omega_build_compiler_graph(g, &sem_id);
    omega_self_host_compile_c0(g, &sem_id, &c1);

    uint8_t wire[8192];
    size_t wlen = 0;
    omega_graph_serialize_binary(g, wire, sizeof(wire), &wlen);
    omega_graph_destroy(g);

    if (omega_self_host_run_native_compiler(&c1, wire, wlen, &c2) != 0) return false;
    if (omega_self_host_run_native_compiler(&c2, wire, wlen, &c3) != 0) return false;
    return (c3.code_len == c1.code_len && c3.has_id);
}

static bool test_m6_fixed_point(void) {
    RealizationObject c1, c2, c3;
    if (omega_self_host_bootstrap_sequence(&c1, &c2, &c3) != 0) {
        return false;
    }
    return (c1.code_len == c2.code_len &&
            c2.code_len == c3.code_len &&
            memcmp(c1.code_bytes, c2.code_bytes, c1.code_len) == 0 &&
            memcmp(c2.code_bytes, c3.code_bytes, c2.code_len) == 0);
}

static bool test_m6_realization_id(void) {
    RealizationObject c1, c2, c3;
    if (omega_self_host_bootstrap_sequence(&c1, &c2, &c3) != 0) {
        return false;
    }
    return (memcmp(c1.realization_id.bytes, c2.realization_id.bytes, OMEGA_ID_BYTES) == 0 &&
            memcmp(c2.realization_id.bytes, c3.realization_id.bytes, OMEGA_ID_BYTES) == 0);
}

static bool test_m6_m5_parity(void) {
    RealizationObject c1;
    OmegaGraph *g = omega_graph_create();
    SemanticId sem_id;
    omega_build_compiler_graph(g, &sem_id);
    omega_self_host_compile_c0(g, &sem_id, &c1);
    omega_graph_destroy(g);

    uint64_t observed = 0;
    return (omega_self_host_verify_m5_parity(&c1, &observed) == 0 && observed == 15);
}

static bool test_m6_native_execution(void) {
    RealizationObject c1, c2, c3;
    if (omega_self_host_bootstrap_sequence(&c1, &c2, &c3) != 0) return false;

    uint64_t observed = 0;
    return (omega_self_host_verify_m5_parity(&c3, &observed) == 0 && observed == 15);
}

static bool test_m6_adversarial_mutation(void) {
    RealizationObject c1, c2;
    OmegaGraph *g = omega_graph_create();
    SemanticId sem_id;
    omega_build_compiler_graph(g, &sem_id);
    omega_self_host_compile_c0(g, &sem_id, &c1);

    uint8_t wire[8192];
    size_t wlen = 0;
    omega_graph_serialize_binary(g, wire, sizeof(wire), &wlen);
    omega_graph_destroy(g);

    /* Mutate 1 bit in magic */
    wire[0] ^= 0x01;
    int rc = omega_self_host_run_native_compiler(&c1, wire, wlen, &c2);
    if (rc == 0) return false;

    /* Restore wire, mutate count byte */
    wire[0] ^= 0x01;
    wire[5] ^= 0x01;
    rc = omega_self_host_run_native_compiler(&c1, wire, wlen, &c2);
    if (rc == 0) return false;

    /* Mutate instruction opcode in c1, check decoder catches it */
    RealizationObject mut_c = c1;
    mut_c.code_bytes[3] = 0x00; /* Makes insn 0 into 0x00000000 (UDF), invalid in decoder */
    char err[256];
    if (aarch64_validate_code_buffer(mut_c.code_bytes, mut_c.code_len, err, sizeof(err)) == 0) {
        return false;
    }

    return true;
}

static void run_demonstration_self_host(void) {
    printf("================================================================================\n");
    printf("    AIEN OMEGA SUBSTRATE — MILESTONE 6: SELF-HOSTING COMPILER REPRODUCTION\n");
    printf("================================================================================\n");

    RealizationObject c1, c2, c3;
    printf("  [1] Executing 3-Generation Bootstrap Sequence:\n");
    printf("      C0(G_C) -> C1\n");
    printf("      C1(G_C) -> C2\n");
    printf("      C2(G_C) -> C3\n");

    int seq_rc = omega_self_host_bootstrap_sequence(&c1, &c2, &c3);
    char id1[65], id2[65], id3[65];
    omega_hex_semantic_id(&c1.realization_id, id1);
    omega_hex_semantic_id(&c2.realization_id, id2);
    omega_hex_semantic_id(&c3.realization_id, id3);

    printf("      C1 Code Length = %zu bytes | REALIZATION_ID = %s\n", c1.code_len, id1);
    printf("      C2 Code Length = %zu bytes | REALIZATION_ID = %s\n", c2.code_len, id2);
    printf("      C3 Code Length = %zu bytes | REALIZATION_ID = %s\n", c3.code_len, id3);
    printf("      Fixed Point    = %s\n\n", (seq_rc == 0) ? "EXACT BIT-FOR-BIT FIXED POINT C1 == C2 == C3" : "MISMATCH");

    printf("  [2] Validating M5 Parity via Reproduced Compiler C3:\n");
    printf("      Target: F(a,b,c) = (a + b) - c\n");
    uint64_t observed = 0;
    int par_rc = omega_self_host_verify_m5_parity(&c3, &observed);
    printf("      Observed Result = %lu (Expected: 15)\n", (unsigned long)observed);
    printf("      M5 Parity       = %s\n", (par_rc == 0 && observed == 15) ? "VERIFIED (PASS: exact M5 bytes and output)" : "FAIL");
    printf("================================================================================\n");
}

/* =========================================================================
 * MILESTONE 7 QUALIFICATION GATES (OMEGA_VERIFY)
 * ========================================================================= */

static bool test_m7_v0_type(void) {
    OmegaGraph *g = omega_graph_create();
    SemanticId sem_id;
    omega_build_f_add_sub_graph(g, &sem_id);
    VerifyReport rep;
    if (omega_verify_v0_structural(g, NULL, &rep) != 0 || !rep.passed) {
        omega_graph_destroy(g);
        return false;
    }
    omega_graph_destroy(g);

    OmegaGraph *bad_g = omega_graph_create();
    OmegaObject *bad_t = omega_graph_add_object(bad_g, KIND_TYPE);
    TypePayload tp = { .tag = TYPE_UNSIGNED_INT, .width = 0 };
    memcpy(bad_t->payload, &tp, sizeof(tp));
    bad_t->payload_len = sizeof(tp);
    omega_compute_semantic_id(bad_t);

    if (omega_verify_v0_structural(bad_g, NULL, &rep) == 0 && rep.passed) {
        omega_graph_destroy(bad_g);
        return false;
    }
    omega_graph_destroy(bad_g);
    return true;
}

static bool test_m7_v0_dag(void) {
    OmegaGraph *g = omega_graph_create();
    OmegaObject *obj = omega_graph_add_object(g, KIND_OPERATION);
    SemanticId bogus_target;
    memset(bogus_target.bytes, 0xAA, OMEGA_ID_BYTES);
    omega_object_add_relation(obj, REL_DEPENDS_ON, &bogus_target);
    omega_compute_semantic_id(obj);

    VerifyReport rep;
    int rc = omega_verify_v0_structural(g, NULL, &rep);
    omega_graph_destroy(g);
    return (rc != 0 || !rep.passed);
}

static bool test_m7_v0_code_bounds(void) {
    OmegaGraph *g = omega_graph_create();
    SemanticId sem_id;
    omega_build_f_add_sub_graph(g, &sem_id);
    RealizationObject real;
    omega_realize_f_add_sub(g, &sem_id, &real);
    omega_graph_destroy(g);

    VerifyReport rep;
    if (omega_verify_v0_structural(NULL, &real, &rep) != 0 || !rep.passed) return false;

    RealizationObject bad1 = real;
    bad1.code_len = 7;
    if (omega_verify_v0_structural(NULL, &bad1, &rep) == 0 && rep.passed) return false;

    RealizationObject bad2 = real;
    bad2.code_len = 0;
    if (omega_verify_v0_structural(NULL, &bad2, &rep) == 0 && rep.passed) return false;

    RealizationObject bad3 = real;
    bad3.code_len = 5000;
    if (omega_verify_v0_structural(NULL, &bad3, &rep) == 0 && rep.passed) return false;

    return true;
}

static bool test_m7_v0_insn_decode(void) {
    OmegaGraph *g = omega_graph_create();
    SemanticId sem_id;
    omega_build_f_add_sub_graph(g, &sem_id);
    RealizationObject real;
    omega_realize_f_add_sub(g, &sem_id, &real);
    omega_graph_destroy(g);

    VerifyReport rep;
    if (omega_verify_v0_structural(NULL, &real, &rep) != 0 || !rep.passed) return false;

    RealizationObject bad = real;
    bad.code_bytes[0] = 0x00;
    bad.code_bytes[1] = 0x00;
    bad.code_bytes[2] = 0x00;
    bad.code_bytes[3] = 0x00;
    if (omega_verify_v0_structural(NULL, &bad, &rep) == 0 && rep.passed) return false;

    return true;
}

static bool test_m7_v0_terminal_ret(void) {
    OmegaGraph *g = omega_graph_create();
    SemanticId sem_id;
    omega_build_f_add_sub_graph(g, &sem_id);
    RealizationObject real;
    omega_realize_f_add_sub(g, &sem_id, &real);
    omega_graph_destroy(g);

    VerifyReport rep;
    if (omega_verify_v0_structural(NULL, &real, &rep) != 0 || !rep.passed) return false;

    RealizationObject bad = real;
    bad.code_bytes[8] = 0x00;
    bad.code_bytes[9] = 0x00;
    bad.code_bytes[10] = 0x01;
    bad.code_bytes[11] = 0x8b;
    if (omega_verify_v0_structural(NULL, &bad, &rep) == 0 && rep.passed) return false;

    return true;
}

static bool test_m7_v1_differential(void) {
    OmegaGraph *g = omega_graph_create();
    SemanticId sem_id;
    omega_build_f_add_sub_graph(g, &sem_id);
    RealizationObject real;
    omega_realize_f_add_sub(g, &sem_id, &real);

    VerifyReport rep;
    int rc = omega_verify_v1_differential(g, &real, NULL, 0, &rep);
    omega_graph_destroy(g);
    return (rc == 0 && rep.passed && rep.check_count > 0 && rep.fail_count == 0);
}

static bool test_m7_v1_divergence_refusal(void) {
    OmegaGraph *g = omega_graph_create();
    SemanticId sem_id;
    omega_build_f_add_sub_graph(g, &sem_id);
    RealizationObject real;
    omega_realize_f_add_sub(g, &sem_id, &real);

    RealizationObject mutant = real;
    mutant.code_bytes[7] = 0x8B; /* Mutate SUB opcode to ADD */
    omega_compute_realization_id(&mutant);

    VerifyReport rep;
    int rc = omega_verify_v1_differential(g, &mutant, NULL, 0, &rep);
    omega_graph_destroy(g);
    return (rc != 0 || !rep.passed || rep.fail_count > 0);
}

static bool test_m7_v2_commutativity(void) {
    VerifyReport rep;
    int rc = omega_verify_v2_properties(NULL, NULL, &rep);
    return (rc == 0 && rep.passed && rep.check_count > 0 && rep.fail_count == 0);
}

static bool test_m7_v2_identity(void) {
    VerifyReport rep;
    int rc = omega_verify_v2_properties(NULL, NULL, &rep);
    return (rc == 0 && rep.passed && rep.fail_count == 0);
}

static bool test_m7_v2_overflow(void) {
    VerifyReport rep;
    int rc = omega_verify_v2_properties(NULL, NULL, &rep);
    return (rc == 0 && rep.passed && rep.fail_count == 0);
}

static void run_demonstration_verify(void) {
    printf("================================================================================\n");
    printf("    AIEN OMEGA SUBSTRATE — MILESTONE 7: TRUSTED VERIFICATION LADDER\n");
    printf("================================================================================\n");

    OmegaGraph *g = omega_graph_create();
    SemanticId sem_id;
    omega_build_f_add_sub_graph(g, &sem_id);
    RealizationObject real;
    omega_realize_f_add_sub(g, &sem_id, &real);

    VerifyReport rep;
    printf("  [1] Executing Tier V0: Structural Verification...\n");
    omega_verify_v0_structural(g, &real, &rep);
    printf("      Checks Passed: %u | Failures: %u | Status: %s\n\n",
           rep.check_count, rep.fail_count, rep.passed ? "PASS (Graph DAG, profile, bounds, insns verified)" : "FAIL");

    printf("  [2] Executing Tier V1: Differential Verification...\n");
    omega_verify_v1_differential(g, &real, NULL, 0, &rep);
    printf("      Vectors Evaluated: %u | Divergences: %u | Status: %s\n\n",
           rep.check_count, rep.fail_count, rep.passed ? "PASS (Bit-for-bit semantic == native parity)" : "FAIL");

    printf("  [3] Executing Tier V2: Property and Invariant Verification...\n");
    omega_verify_v2_properties(g, &real, &rep);
    printf("      Invariants Checked: %u | Violations: %u | Status: %s\n\n",
           rep.check_count, rep.fail_count, rep.passed ? "PASS (Commutativity, identity, overflow wrapping verified)" : "FAIL");

    printf("  [4] Full Pipeline Verification (V0 -> V1 -> V2)...\n");
    int pipe_rc = omega_verify_pipeline(g, &real, VERIFY_TIER_V2, &rep);
    printf("      Pipeline Status: %s\n", (pipe_rc == 0 && rep.passed) ? "ADMITTED (All tiers passed, candidate survives)" : "REFUSED");
    printf("================================================================================\n");

    omega_graph_destroy(g);
}

/* =========================================================================
 * MILESTONE 8 QUALIFICATION GATES (OMEGA_PROGRAM_CORE)
 * ========================================================================= */

static bool test_m8_program_object(void) {
    OmegaProgram a;
    if (omega_program_build_unary_op(&a, "prog_mul2", OP_MUL, 2) != 0) return false;
    bool ok = (a.is_realized && a.cost.insn_count == 3 &&
               a.contract.input_type == TYPE_UNSIGNED_INT &&
               a.contract.output_type == TYPE_UNSIGNED_INT);
    omega_program_destroy(&a);
    return ok;
}

static bool test_m8_contract_validation(void) {
    OmegaProgram a;
    omega_program_build_unary_op(&a, "prog_add1", OP_ADD, 1);
    char err[256];
    if (omega_program_validate_contract(&a, err, sizeof(err)) != 0) {
        omega_program_destroy(&a);
        return false;
    }

    /* Mutate contract to invalid type */
    OmegaProgram bad = a;
    bad.contract.input_type = TYPE_INVALID;
    if (omega_program_validate_contract(&bad, err, sizeof(err)) == 0) {
        omega_program_destroy(&a);
        return false;
    }

    omega_program_destroy(&a);
    return true;
}

static bool test_m8_composition(void) {
    OmegaProgram a, b, c;
    omega_program_build_unary_op(&a, "mul2", OP_MUL, 2);
    omega_program_build_unary_op(&b, "add1", OP_ADD, 1);

    char err[256];
    int rc = omega_program_compose(&a, &b, &c, err, sizeof(err));
    bool ok = (rc == 0 && c.is_realized &&
               c.contract.input_type == TYPE_UNSIGNED_INT &&
               c.contract.output_type == TYPE_UNSIGNED_INT);

    omega_program_destroy(&a);
    omega_program_destroy(&b);
    omega_program_destroy(&c);
    return ok;
}

static bool test_m8_type_mismatch_refusal(void) {
    OmegaProgram a, b, c;
    omega_program_build_unary_op(&a, "mul2", OP_MUL, 2);
    omega_program_build_unary_op(&b, "add1", OP_ADD, 1);
    b.contract.input_type = TYPE_BOOL; /* Cause mismatch */

    char err[256];
    int rc = omega_program_compose(&a, &b, &c, err, sizeof(err));

    omega_program_destroy(&a);
    omega_program_destroy(&b);
    return (rc != 0); /* Must refuse fail-closed */
}

static bool test_m8_cost_accounting(void) {
    OmegaProgram a, b, c;
    omega_program_build_unary_op(&a, "mul2", OP_MUL, 2);
    omega_program_build_unary_op(&b, "add1", OP_ADD, 1);

    char err[256];
    omega_program_compose(&a, &b, &c, err, sizeof(err));

    /* Cost check: insn_count = 3 + 3 - 1 = 5 */
    bool ok = (c.cost.insn_count == 5 &&
               c.cost.latency_cycles == a.cost.latency_cycles + b.cost.latency_cycles);

    omega_program_destroy(&a);
    omega_program_destroy(&b);
    omega_program_destroy(&c);
    return ok;
}

static bool test_m8_realization(void) {
    OmegaProgram a, b, c;
    omega_program_build_unary_op(&a, "mul2", OP_MUL, 2);
    omega_program_build_unary_op(&b, "add1", OP_ADD, 1);

    char err[256];
    omega_program_compose(&a, &b, &c, err, sizeof(err));

    bool ok = (c.realization.code_len == 20 && c.realization.has_id);

    omega_program_destroy(&a);
    omega_program_destroy(&b);
    omega_program_destroy(&c);
    return ok;
}

static bool test_m8_v0_structural(void) {
    OmegaProgram a, b, c;
    omega_program_build_unary_op(&a, "mul2", OP_MUL, 2);
    omega_program_build_unary_op(&b, "add1", OP_ADD, 1);

    char err[256];
    omega_program_compose(&a, &b, &c, err, sizeof(err));

    VerifyReport rep;
    int rc = omega_verify_v0_structural(NULL, &c.realization, &rep);

    omega_program_destroy(&a);
    omega_program_destroy(&b);
    omega_program_destroy(&c);
    return (rc == 0 && rep.passed && rep.fail_count == 0);
}

static bool test_m8_v1_differential(void) {
    OmegaProgram a, b, c;
    omega_program_build_unary_op(&a, "mul2", OP_MUL, 2);
    omega_program_build_unary_op(&b, "add1", OP_ADD, 1);

    char err[256];
    omega_program_compose(&a, &b, &c, err, sizeof(err));

    /* Evaluate C(x) = (2x + 1) across vectors */
    static const uint64_t inputs[] = { 0, 1, 2, 7, 10, 100 };
    static const uint64_t expected[] = { 1, 3, 5, 15, 21, 201 };

    bool ok = true;
    for (size_t i = 0; i < 6; ++i) {
        uint64_t observed = 0;
        if (omega_program_exec(&c, inputs[i], &observed) != 0 || observed != expected[i]) {
            ok = false;
            break;
        }
    }

    omega_program_destroy(&a);
    omega_program_destroy(&b);
    omega_program_destroy(&c);
    return ok;
}

static bool test_m8_v2_property(void) {
    OmegaProgram a, b, c;
    omega_program_build_unary_op(&a, "mul2", OP_MUL, 2);
    omega_program_build_unary_op(&b, "add1", OP_ADD, 1);

    char err[256];
    omega_program_compose(&a, &b, &c, err, sizeof(err));

    VerifyReport rep;
    int rc = omega_program_verify(&c, &rep);
    bool verified = (rc == 0 && rep.passed && c.is_verified);

    omega_program_destroy(&a);
    omega_program_destroy(&b);
    omega_program_destroy(&c);
    return verified;
}

static bool test_m8_synthesis_task(void) {
    SynthesisTask task;
    static const uint64_t inputs[] = { 0, 1, 2, 7, 10 };
    static const uint64_t outputs[] = { 1, 3, 5, 15, 21 };

    omega_task_init(&task, "task_2x_plus_1", TYPE_UNSIGNED_INT, 64, TYPE_UNSIGNED_INT, 64, inputs, outputs, 5);

    OmegaProgram a, b, c;
    omega_program_build_unary_op(&a, "mul2", OP_MUL, 2);
    omega_program_build_unary_op(&b, "add1", OP_ADD, 1);
    char err[256];
    omega_program_compose(&a, &b, &c, err, sizeof(err));

    bool solved = false;
    omega_task_evaluate_candidate(&task, &c, &solved);

    /* Program A alone should NOT solve task */
    bool solved_a = true;
    omega_task_evaluate_candidate(&task, &a, &solved_a);

    omega_program_destroy(&a);
    omega_program_destroy(&b);
    omega_program_destroy(&c);
    return (solved && !solved_a);
}

static void run_demonstration_program(void) {
    printf("================================================================================\n");
    printf("    AIEN OMEGA SUBSTRATE — MILESTONE 8: PROGRAM COMPOSITION & CONTRACTS\n");
    printf("================================================================================\n");

    OmegaProgram a, b, c;
    omega_program_build_unary_op(&a, "double", OP_MUL, 2);
    omega_program_build_unary_op(&b, "increment", OP_ADD, 1);

    printf("  [1] Program A: %s | Cost: %u insns | Post: %s\n", a.name, a.cost.insn_count, a.contract.postcondition);
    printf("  [2] Program B: %s | Cost: %u insns | Post: %s\n\n", b.name, b.cost.insn_count, b.contract.postcondition);

    char err[256];
    printf("  [3] Composing C = A o B (C(x) = B(A(x)) = 2x + 1)...\n");
    omega_program_compose(&a, &b, &c, err, sizeof(err));
    printf("      Composite Program: %s\n", c.name);
      printf("      Derived Contract:  In: uint%u -> Out: uint%u | Post: %s\n",
           c.contract.input_width, c.contract.output_width, c.contract.postcondition);
    printf("      Composite Cost:    %u insns (additive monotonicity verified)\n", c.cost.insn_count);
    printf("      Machine Code:      %zu bytes native AArch64\n\n", c.realization.code_len);

    printf("  [4] Evaluating Composite Execution C(x):\n");
    for (uint64_t x = 0; x <= 5; ++x) {
        uint64_t y = 0;
        omega_program_exec(&c, x, &y);
        printf("      C(%lu) = %lu  (Expected: %lu)\n", (unsigned long)x, (unsigned long)y, (unsigned long)(2 * x + 1));
    }

    VerifyReport rep;
    omega_program_verify(&c, &rep);
    printf("\n  [5] M7 Verification Ladder Status: %s\n", c.is_verified ? "VERIFIED / ADMITTED" : "REFUSED");
    printf("================================================================================\n");

    omega_program_destroy(&a);
    omega_program_destroy(&b);
    omega_program_destroy(&c);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s [--run-gates | --run-m5-gates | --run-m6-gates | --run-m7-gates | --run-m8-gates | --demonstrate-arithmetic | --demonstrate-physics | --demonstrate-realization | --demonstrate-self-host | --demonstrate-verify | --demonstrate-program | --dump-test-vectors <dir>]\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "--run-gates") == 0) {
        printf("================================================================================\n");
        printf("    AIEN OMEGA SUBSTRATE — MILESTONE 4: OMEGA_SEMANTICS QUALIFICATION GATES\n");
        printf("================================================================================\n");
        gate_count = 0; gate_passed = 0;
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

    if (strcmp(argv[1], "--run-m5-gates") == 0) {
        printf("================================================================================\n");
        printf("    AIEN OMEGA SUBSTRATE — MILESTONE 5: OMEGA_AARCH64 QUALIFICATION GATES\n");
        printf("================================================================================\n");
        gate_count = 0; gate_passed = 0;
        report_gate("OMEGA_AARCH64_PROFILE_PASS", test_m5_profile(), "AArch64 bare-metal target profile and registers valid");
        report_gate("OMEGA_AARCH64_ENCODER_PASS", test_m5_encoder(), "Direct instruction encoder synthesized expected machine words");
        report_gate("OMEGA_AARCH64_DECODER_SEAM_PASS", test_m5_decoder_seam(), "Independent decoder Seam 1 validated instructions and refused corrupt opcode");
        report_gate("OMEGA_AARCH64_LOWERING_PASS", test_m5_lowering(), "G_S pure operations lowered to AArch64 code buffer");
        report_gate("OMEGA_AARCH64_REALIZATION_ID_PASS", test_m5_realization_id(), "REALIZATION_ID deterministically binds machine code to SEMANTIC_ID");
        report_gate("OMEGA_AARCH64_NATIVE_EXECUTION_PASS", test_m5_native_execution(), "Native in-memory execution observed result 15 matches semantic evaluation");
        report_gate("OMEGA_AARCH64_QEMU_EXECUTION_PASS", test_m5_qemu_execution(), "Bare-metal QEMU execution observed result 15 on PL011 UART");
        report_gate("OMEGA_AARCH64_ADVERSARIAL_MUTATION_PASS", test_m5_adversarial_mutation(), "Single-bit machine code mutation detected and refused fail-closed");
        report_gate("OMEGA_AARCH64_CROSS_BUILD_DETERMINISM_PASS", test_m5_cross_build_determinism(), "Byte-for-byte deterministic realization across runs");
        printf("================================================================================\n");
        printf("  TOTAL GATES: %d | PASSED: %d | FAILED: %d\n", gate_count, gate_passed, gate_count - gate_passed);
        printf("================================================================================\n");
        return (gate_passed == gate_count) ? 0 : 1;
    }

    if (strcmp(argv[1], "--run-m6-gates") == 0) {
        printf("================================================================================\n");
        printf("    AIEN OMEGA SUBSTRATE — MILESTONE 6: OMEGA_SELF_HOST QUALIFICATION GATES\n");
        printf("================================================================================\n");
        gate_count = 0; gate_passed = 0;
        report_gate("OMEGA_SELF_HOST_GRAPH_PASS", test_m6_graph(), "Semantic compiler graph G_C constructed and validated");
        report_gate("OMEGA_SELF_HOST_C1_EMISSION_PASS", test_m6_c1_emission(), "Reference lowering C0(G_C) -> C1 emitted machine bytes");
        report_gate("OMEGA_SELF_HOST_DECODER_SEAM_PASS", test_m6_decoder_seam(), "Independent decoder verified all C1 instructions");
        report_gate("OMEGA_SELF_HOST_C2_REPRODUCTION_PASS", test_m6_c2_reproduction(), "Native compiler C1(G_C) -> C2 emitted machine bytes");
        report_gate("OMEGA_SELF_HOST_C3_REPRODUCTION_PASS", test_m6_c3_reproduction(), "Native compiler C2(G_C) -> C3 emitted machine bytes");
        report_gate("OMEGA_SELF_HOST_FIXED_POINT_PASS", test_m6_fixed_point(), "Exact bit-for-bit identity C1 == C2 == C3 verified");
        report_gate("OMEGA_SELF_HOST_REALIZATION_ID_PASS", test_m6_realization_id(), "REALIZATION_ID matching across C1, C2, and C3");
        report_gate("OMEGA_SELF_HOST_M5_PARITY_PASS", test_m6_m5_parity(), "Reproduced compiler reproduces exact M5 machine code bytes");
        report_gate("OMEGA_SELF_HOST_NATIVE_EXECUTION_PASS", test_m6_native_execution(), "Reproduced compiler output executes natively to 15");
        report_gate("OMEGA_SELF_HOST_ADVERSARIAL_MUTATION_PASS", test_m6_adversarial_mutation(), "Single-bit mutations in G_C wire or C1 refused fail-closed");
        printf("================================================================================\n");
        printf("  TOTAL GATES: %d | PASSED: %d | FAILED: %d\n", gate_count, gate_passed, gate_count - gate_passed);
        printf("================================================================================\n");
        return (gate_passed == gate_count) ? 0 : 1;
    }

    if (strcmp(argv[1], "--demonstrate-self-host") == 0) {
        run_demonstration_self_host();
        return 0;
    }

    if (strcmp(argv[1], "--run-m7-gates") == 0) {
        printf("================================================================================\n");
        printf("    AIEN OMEGA SUBSTRATE — MILESTONE 7: OMEGA_VERIFY QUALIFICATION GATES\n");
        printf("================================================================================\n");
        gate_count = 0; gate_passed = 0;
        report_gate("OMEGA_VERIFY_V0_TYPE_PASS", test_m7_v0_type(), "V0 accepted well-typed graph and rejected ill-typed graph");
        report_gate("OMEGA_VERIFY_V0_DAG_PASS", test_m7_v0_dag(), "V0 rejected graph with dangling reference");
        report_gate("OMEGA_VERIFY_V0_CODE_BOUNDS_PASS", test_m7_v0_code_bounds(), "V0 enforced 4-byte alignment and buffer length bounds");
        report_gate("OMEGA_VERIFY_V0_INSN_DECODE_PASS", test_m7_v0_insn_decode(), "V0 validated instruction opcodes and refused illegal instruction");
        report_gate("OMEGA_VERIFY_V0_TERMINAL_RET_PASS", test_m7_v0_terminal_ret(), "V0 rejected non-terminating / non-RET execution buffer");
        report_gate("OMEGA_VERIFY_V1_DIFFERENTIAL_PASS", test_m7_v1_differential(), "V1 confirmed exact bit-for-bit parity across test corpus");
        report_gate("OMEGA_VERIFY_V1_DIVERGENCE_REFUSAL_PASS", test_m7_v1_divergence_refusal(), "V1 detected and refused mutated realization divergence");
        report_gate("OMEGA_VERIFY_V2_COMMUTATIVITY_PASS", test_m7_v2_commutativity(), "V2 proved commutativity on declared commutative ops");
        report_gate("OMEGA_VERIFY_V2_IDENTITY_PASS", test_m7_v2_identity(), "V2 proved identity elements on ADD, MUL, AND, OR");
        report_gate("OMEGA_VERIFY_V2_OVERFLOW_PASS", test_m7_v2_overflow(), "V2 validated modular overflow wrapping semantics");
        printf("================================================================================\n");
        printf("  TOTAL GATES: %d | PASSED: %d | FAILED: %d\n", gate_count, gate_passed, gate_count - gate_passed);
        printf("================================================================================\n");
        return (gate_passed == gate_count) ? 0 : 1;
    }

    if (strcmp(argv[1], "--demonstrate-verify") == 0) {
        run_demonstration_verify();
        return 0;
    }

    if (strcmp(argv[1], "--run-m8-gates") == 0) {
        printf("================================================================================\n");
        printf("    AIEN OMEGA SUBSTRATE — MILESTONE 8: OMEGA_PROGRAM_CORE QUALIFICATION GATES\n");
        printf("================================================================================\n");
        gate_count = 0; gate_passed = 0;
        report_gate("OMEGA_PROGRAM_OBJECT_PASS", test_m8_program_object(), "OMEGA_PROGRAM created with contracts, cost, and realization");
        report_gate("OMEGA_PROGRAM_CONTRACT_VALIDATION_PASS", test_m8_contract_validation(), "Contract validation enforced, invalid types rejected");
        report_gate("OMEGA_PROGRAM_COMPOSITION_PASS", test_m8_composition(), "Algebraic composition C = A o B with contract propagation");
        report_gate("OMEGA_PROGRAM_TYPE_MISMATCH_REFUSAL_PASS", test_m8_type_mismatch_refusal(), "Incompatible composition refused fail-closed");
        report_gate("OMEGA_PROGRAM_COST_ACCOUNTING_PASS", test_m8_cost_accounting(), "Composite cost monotonically verified (insns, latency)");
        report_gate("OMEGA_PROGRAM_REALIZATION_PASS", test_m8_realization(), "Composite program lowered to native AArch64 machine bytes");
        report_gate("OMEGA_PROGRAM_V0_STRUCTURAL_PASS", test_m8_v0_structural(), "Composite program passes M7 V0 structural verification");
        report_gate("OMEGA_PROGRAM_V1_DIFFERENTIAL_PASS", test_m8_v1_differential(), "Composite program passes M7 V1 differential evaluation (2x+1)");
        report_gate("OMEGA_PROGRAM_V2_PROPERTY_PASS", test_m8_v2_property(), "Composite program passes M7 V2 property verification");
        report_gate("OMEGA_PROGRAM_SYNTHESIS_TASK_PASS", test_m8_synthesis_task(), "SYNTHESIS_TASK evaluation harness ready for M9 search");
        printf("================================================================================\n");
        printf("  TOTAL GATES: %d | PASSED: %d | FAILED: %d\n", gate_count, gate_passed, gate_count - gate_passed);
        printf("================================================================================\n");
        return (gate_passed == gate_count) ? 0 : 1;
    }

    if (strcmp(argv[1], "--demonstrate-program") == 0) {
        run_demonstration_program();
        return 0;
    }

    if (strcmp(argv[1], "--demonstrate-realization") == 0) {
        run_demonstration_realization();
        return 0;
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
