/* test_visor_semantic.c -- Omega Visor V1 lane 1: read-only semantic API tests. */
#include "visor_semantic.h"
#include "visor.h"
#include "omega_core.h"
#include "omega_canonical.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_total = 0;

#define CHECK(cond) do { \
    g_total++; \
    if (cond) g_pass++; \
    else fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
} while (0)

static char txt1[65536], txt2[65536], js1[65536], js2[65536];
static VisorObjectView v1, v2;
static OmegaGraph snapshot;

static void recompute(OmegaObject *o) { omega_compute_semantic_id(o); }

int main(void) {
    OmegaGraph *g = omega_graph_create();
    if (!g) return 2;

    OmegaObject *t64 = omega_build_type_uint(g, 64);
    OmegaObject *tu8 = omega_build_type_uint(g, 8);
    OmegaObject *tbool = omega_build_type_bool(g);
    OmegaObject *tbv = omega_build_type_bitvector(g, 32);
    OmegaObject *tseq = omega_build_type_sequence(g, &tu8->id, 16);
    OmegaObject *v7 = omega_build_val_uint(g, &t64->id, 64, 7);
    OmegaObject *v11 = omega_build_val_uint(g, &t64->id, 64, 11);
    OmegaObject *vmax = omega_build_val_uint(g, &t64->id, 64, 0xFFFFFFFFFFFFFFFFULL);
    OmegaObject *v1o = omega_build_val_uint(g, &t64->id, 64, 1);
    OmegaObject *vtrue = omega_build_val_bool(g, &tbool->id, true);
    OmegaObject *vbv = omega_build_val_uint(g, &tbv->id, 32, 0xDEADBEEF);
    OmegaObject *add = omega_build_op_binary(g, OP_ADD, OVERFLOW_WRAP, &t64->id);
    OmegaObject *addfc = omega_build_op_binary(g, OP_ADD, OVERFLOW_FAIL_CLOSED, &t64->id);
    OmegaObject *ap = omega_build_apply(g, &add->id, &v7->id, &v11->id);
    OmegaObject *apwrap = omega_build_apply(g, &add->id, &vmax->id, &v1o->id);
    OmegaObject *apnest = omega_build_apply(g, &add->id, &ap->id, &v7->id);
    OmegaObject *apfc = omega_build_apply(g, &addfc->id, &vmax->id, &v1o->id);
    OmegaObject *apbad = omega_build_apply(g, &add->id, &vtrue->id, &v7->id);

    /* annotated value: attribute, relation, constraint (then re-id as a builder would) */
    OmegaObject *ann = omega_build_val_uint(g, &t64->id, 64, 42);
    omega_object_add_attribute(ann, "name", (const uint8_t *)"answer", 6);
    uint8_t raw[3] = { 0x00, 0xff, 0x10 };
    omega_object_add_attribute(ann, "blob", raw, 3);
    omega_object_add_relation(ann, REL_DERIVED_FROM, &v7->id);
    uint8_t range[8] = { 0, 0, 0, 0, 0, 0, 0, 100 };
    omega_object_add_constraint(ann, CONST_RANGE, range, 8);
    recompute(ann);

    /* dangling relation */
    SemanticId ghost;
    memset(ghost.bytes, 0xAB, sizeof(ghost.bytes));
    OmegaObject *dang = omega_build_val_uint(g, &t64->id, 64, 5);
    omega_object_add_relation(dang, REL_DEPENDS_ON, &ghost);
    recompute(dang);

    /* depth chain: c0 = 7, c_k = add(c_{k-1}, 1) */
    SemanticId chain[72];
    chain[0] = v7->id;
    for (int k = 1; k < 72; ++k) {
        OmegaObject *c = omega_build_apply(g, &add->id, &chain[k - 1], &v1o->id);
        if (!c) { fprintf(stderr, "FAIL: graph full building chain\n"); return 1; }
        chain[k] = c->id;
    }

    memcpy(&snapshot, g, sizeof(OmegaGraph));
    SemanticId ap_id_before = ap->id;

    /* 1. known type */
    CHECK(visor_semantic_inspect(g, &t64->id, &v1) == 0);
    CHECK(strcmp(v1.kind_name, "TYPE") == 0 && strcmp(v1.type_text, "u64") == 0);
    CHECK(v1.canonical_id_matches && v1.canonical_len > 0);
    CHECK(visor_semantic_inspect(g, &tseq->id, &v1) == 0 && strcmp(v1.type_text, "seq<u8,16>") == 0);
    CHECK(v1.dep_count == 1 && strcmp(v1.deps[0].role, "elem") == 0);
    CHECK(visor_semantic_inspect(g, &tbv->id, &v1) == 0 && strcmp(v1.type_text, "bitvector<32>") == 0);
    CHECK(visor_semantic_inspect(g, &tbool->id, &v1) == 0 && strcmp(v1.type_text, "bool") == 0);
    CHECK(visor_semantic_type_of(g, &t64->id, NULL, NULL, 0) == -1);

    /* 2. known value */
    CHECK(visor_semantic_inspect(g, &v7->id, &v1) == 0);
    CHECK(v1.shape == VISOR_SHAPE_VALUE && v1.has_u64 && v1.value_u64 == 7);
    CHECK(strcmp(v1.type_text, "u64") == 0 && v1.has_type_id && memcmp(&v1.type_id, &t64->id, 32) == 0);
    uint64_t u = 0;
    CHECK(visor_semantic_value_u64(g, &v11->id, &u) == 0 && u == 11);
    CHECK(visor_semantic_value_u64(g, &vbv->id, &u) == 0 && u == 0xDEADBEEF);
    CHECK(visor_semantic_inspect(g, &vtrue->id, &v1) == 0 && v1.value_is_bool && v1.value_u64 == 1);
    char tt[64];
    SemanticId tid;
    CHECK(visor_semantic_type_of(g, &v7->id, &tid, tt, sizeof(tt)) == 0 && strcmp(tt, "u64") == 0 &&
          memcmp(&tid, &t64->id, 32) == 0);
    CHECK(visor_semantic_value_u64(g, &add->id, &u) == -1);

    /* 3. known operation */
    CHECK(visor_semantic_inspect(g, &add->id, &v1) == 0);
    CHECK(v1.shape == VISOR_SHAPE_OPERATION && strcmp(v1.opcode_name, "ADD") == 0 &&
          strcmp(v1.overflow_name, "WRAP") == 0 && v1.arity == 2);
    CHECK(strcmp(v1.type_text, "fn(u64,u64)->u64") == 0);
    CHECK(v1.dep_count == 1);   /* type/input/output all the same id: deduped */
    CHECK(visor_semantic_type_of(g, &add->id, NULL, tt, sizeof(tt)) == 0 && strcmp(tt, "u64") == 0);
    CHECK(visor_semantic_format_text(&v1, txt1, sizeof(txt1)) > 0 && strstr(txt1, "fn(u64,u64)->u64") != NULL &&
          strstr(txt1, "input0: sha256:") != NULL);
    CHECK(visor_semantic_format_json(&v1, js1, sizeof(js1)) > 0 && strstr(js1, "\"inputs\":[") != NULL);
    CHECK(visor_semantic_inspect(g, &t64->id, &v1) == 0 && visor_semantic_format_text(&v1, txt1, sizeof(txt1)) > 0 &&
          strstr(txt1, "tag: 3\n") != NULL && strstr(txt1, "width: 64\n") != NULL);
    CHECK(visor_semantic_format_json(&v1, js1, sizeof(js1)) > 0 && strstr(js1, "\"width\":64") != NULL);

    /* 4. apply inspection */
    CHECK(visor_semantic_inspect(g, &ap->id, &v1) == 0);
    CHECK(strcmp(v1.kind_name, "OPERATION") == 0 && v1.shape == VISOR_SHAPE_APPLY &&
          strcmp(v1.shape_name, "APPLY") == 0);
    CHECK(memcmp(&v1.apply_op, &add->id, 32) == 0 && v1.operand_count == 2);
    CHECK(memcmp(&v1.operands[0], &v7->id, 32) == 0 && memcmp(&v1.operands[1], &v11->id, 32) == 0);
    CHECK(strcmp(v1.opcode_name, "ADD") == 0 && strcmp(v1.type_text, "u64") == 0);
    CHECK(v1.dep_count == 3 && strcmp(v1.deps[0].role, "op") == 0 && strcmp(v1.deps[2].role, "operand1") == 0);
    SemanticId opid, operands[4];
    size_t cnt = 0;
    CHECK(visor_semantic_apply_parts(g, &ap->id, &opid, operands, &cnt) == 0 && cnt == 2 &&
          memcmp(&opid, &add->id, 32) == 0 && memcmp(&operands[1], &v11->id, 32) == 0);
    CHECK(visor_semantic_apply_parts(g, &v7->id, &opid, operands, &cnt) == -1);
    CHECK(visor_semantic_type_of(g, &ap->id, &tid, tt, sizeof(tt)) == 0 && strcmp(tt, "u64") == 0);

    /* 5. relation traversal + attributes + 6. constraint display */
    CHECK(visor_semantic_inspect(g, &ann->id, &v1) == 0);
    CHECK(v1.attr_count == 2 && strcmp(v1.attrs[0].key, "name") == 0 && v1.attrs[0].is_utf8 &&
          strcmp(v1.attrs[1].key, "blob") == 0 && !v1.attrs[1].is_utf8);
    CHECK(v1.rel_count == 1 && strcmp(v1.rels[0].kind_name, "DERIVED_FROM") == 0 && v1.rels[0].resolved);
    CHECK(v1.dep_count == 2 && memcmp(&v1.deps[0].id, &v7->id, 32) == 0 &&
          strcmp(v1.deps[0].role, "rel:DERIVED_FROM") == 0 && strcmp(v1.deps[1].role, "type") == 0);
    CHECK(v1.const_count == 1 && strcmp(v1.consts[0].kind_name, "RANGE") == 0 && v1.consts[0].len == 8);
    CHECK(visor_semantic_format_text(&v1, txt1, sizeof(txt1)) > 0);
    CHECK(strstr(txt1, "RANGE len=8 hex:0000000000000064") != NULL);
    CHECK(strstr(txt1, "name = hex:616e73776572 utf8:\"answer\"") != NULL);
    CHECK(strstr(txt1, "blob = hex:00ff10\n") != NULL);
    CHECK(strstr(txt1, "DERIVED_FROM sha256:") != NULL);
    CHECK(visor_semantic_format_json(&v1, js1, sizeof(js1)) > 0);
    CHECK(strstr(js1, "\"constraints\":[{\"kind\":\"RANGE\",\"len\":8,\"hex\":\"0000000000000064\"}]") != NULL);
    CHECK(visor_semantic_graph_text(g, &ann->id, txt2, sizeof(txt2)) > 0);
    {
        char h7[65], h64[65];
        omega_hex_semantic_id(&v7->id, h7);
        omega_hex_semantic_id(&t64->id, h64);
        char *p7 = strstr(txt2, h7), *p64 = strstr(txt2, h64);
        CHECK(p7 && p64 && p7 < p64);                   /* relation target before payload type */
        CHECK(strstr(p64 + 1, h64) == NULL);             /* t64 reachable twice, printed once */
        CHECK(strstr(txt2, "  VALUE sha256:") != NULL);  /* indented child */
    }
    CHECK(visor_semantic_graph_text(g, &apnest->id, txt2, sizeof(txt2)) > 0);
    CHECK(strncmp(txt2, "OPERATION sha256:", 17) == 0 && strstr(txt2, "apply ADD(") != NULL &&
          strstr(txt2, "op ADD WRAP arity=2 -> u64") != NULL && strstr(txt2, "value 11 : u64") != NULL);

    /* dangling relation: reported, not an error for inspect; MISSING in graph text */
    CHECK(visor_semantic_inspect(g, &dang->id, &v1) == 0 && v1.dangling_count == 1 && !v1.rels[0].resolved);
    CHECK(visor_semantic_format_text(&v1, txt1, sizeof(txt1)) > 0 && strstr(txt1, "(DANGLING)") != NULL);
    CHECK(visor_semantic_graph_text(g, &dang->id, txt2, sizeof(txt2)) > 0 && strstr(txt2, "MISSING sha256:abab") != NULL);

    /* 7. missing SemanticId fails closed */
    CHECK(visor_semantic_inspect(g, &ghost, &v1) == -1);
    CHECK(visor_semantic_type_of(g, &ghost, &tid, tt, sizeof(tt)) == -1);
    CHECK(visor_semantic_eval_u64(g, &ghost, &u) == -1);
    CHECK(visor_semantic_graph_text(g, &ghost, txt2, sizeof(txt2)) == -1);
    CHECK(visor_semantic_apply_parts(g, &ghost, &opid, operands, &cnt) == -1);
    CHECK(visor_semantic_value_u64(g, &ghost, &u) == -1);
    CHECK(visor_semantic_inspect(NULL, &v7->id, &v1) == -1 && visor_semantic_inspect(g, NULL, &v1) == -1);

    /* 8. stable ordering: inspect twice -> identical text and json */
    CHECK(visor_semantic_inspect(g, &ann->id, &v1) == 0 && visor_semantic_inspect(g, &ann->id, &v2) == 0);
    CHECK(visor_semantic_format_text(&v1, txt1, sizeof(txt1)) > 0 && visor_semantic_format_text(&v2, txt2, sizeof(txt2)) > 0);
    CHECK(strcmp(txt1, txt2) == 0);
    CHECK(visor_semantic_format_json(&v1, js1, sizeof(js1)) > 0 && visor_semantic_format_json(&v2, js2, sizeof(js2)) > 0);
    CHECK(strcmp(js1, js2) == 0 && js1[0] == '{' && js1[strlen(js1) - 1] == '}');
    CHECK(visor_semantic_inspect(g, &ap->id, &v1) == 0 && visor_semantic_format_json(&v1, js1, sizeof(js1)) > 0);
    CHECK(visor_semantic_inspect(g, &ap->id, &v2) == 0 && visor_semantic_format_json(&v2, js2, sizeof(js2)) > 0);
    CHECK(strcmp(js1, js2) == 0);
    /* too-small buffer: -1, never a truncated result */
    CHECK(visor_semantic_format_text(&v1, txt1, 40) == -1 && txt1[0] == '\0');
    CHECK(visor_semantic_format_json(&v1, js1, 40) == -1);

    /* 10. eval */
    CHECK(visor_semantic_eval_u64(g, &ap->id, &u) == 0 && u == 18);
    CHECK(visor_semantic_eval_u64(g, &apwrap->id, &u) == 0 && u == 0);
    CHECK(visor_semantic_eval_u64(g, &apnest->id, &u) == 0 && u == 25);
    CHECK(visor_semantic_eval_u64(g, &v11->id, &u) == 0 && u == 11);
    CHECK(visor_semantic_eval_u64(g, &apfc->id, &u) == -3);      /* FAIL_CLOSED overflow refused */
    CHECK(visor_semantic_eval_u64(g, &apbad->id, &u) == -1);     /* bool operand into u64 ADD */
    CHECK(visor_semantic_eval_u64(g, &add->id, &u) == -1);       /* bare operation not evaluable */
    CHECK(visor_semantic_eval_u64(g, &chain[60], &u) == 0 && u == 67);
    CHECK(visor_semantic_eval_u64(g, &chain[71], &u) == -1);     /* depth > 64 fails closed */

    /* 9. canonical id unchanged by inspection; whole graph unchanged */
    CHECK(memcmp(&ap->id, &ap_id_before, sizeof(SemanticId)) == 0);
    CHECK(memcmp(&snapshot, g, sizeof(OmegaGraph)) == 0);

    /* 11. malformed objects: reported, never dereferenced */
    OmegaGraph *m = omega_graph_create();
    OmegaObject *mt = omega_build_type_uint(m, 64);
    OmegaObject *mv = omega_build_val_uint(m, &mt->id, 64, 9);
    OmegaObject *mk = omega_build_val_uint(m, &mt->id, 64, 10);
    OmegaObject *mid = omega_build_val_uint(m, &mt->id, 64, 12);
    OmegaObject *mop = omega_build_op_binary(m, OP_ADD, OVERFLOW_WRAP, &mt->id);
    OmegaObject *mbig = omega_build_val_uint(m, &mt->id, 64, 13);
    OmegaObject *map = omega_build_apply(m, &mop->id, &mv->id, &mv->id);
    OmegaObject *mdef = omega_build_op_binary(m, OP_ADD, OVERFLOW_DEFAULT, &mt->id);
    CHECK(visor_semantic_inspect(m, &mdef->id, &v1) == -2 && strstr(v1.malformed_reason, "overflow") != NULL);
    mv->payload_len = 3;                 /* wrong payload_len */
    mk->kind = (SemanticKind)0x7F;       /* invalid kind */
    mid->payload[40] ^= 0x01;            /* tampered after id computed */
    mbig->payload_len = 5000;            /* beyond OMEGA_MAX_PAYLOAD_LEN */
    CHECK(visor_semantic_inspect(m, &mv->id, &v1) == -2 && v1.status == -2 &&
          strstr(v1.malformed_reason, "VALUE payload_len 3") != NULL);
    CHECK(visor_semantic_format_text(&v1, txt1, sizeof(txt1)) > 0 && strstr(txt1, "status: MALFORMED") != NULL);
    CHECK(visor_semantic_format_json(&v1, js1, sizeof(js1)) > 0 && strstr(js1, "\"status\":\"malformed\"") != NULL);
    CHECK(visor_semantic_inspect(m, &mk->id, &v1) == -2 && strstr(v1.malformed_reason, "invalid kind 0x7f") != NULL);
    CHECK(visor_semantic_inspect(m, &mid->id, &v1) == -2 && strstr(v1.malformed_reason, "stored id") != NULL);
    CHECK(visor_semantic_inspect(m, &mbig->id, &v1) == -2 && strstr(v1.malformed_reason, "exceeds") != NULL);
    CHECK(visor_semantic_value_u64(m, &mv->id, &u) == -2);
    CHECK(visor_semantic_type_of(m, &mk->id, &tid, tt, sizeof(tt)) == -2);
    CHECK(visor_semantic_eval_u64(m, &map->id, &u) == -2);      /* operand malformed -> fails closed */
    CHECK(visor_semantic_graph_text(m, &map->id, txt2, sizeof(txt2)) > 0 && strstr(txt2, "MALFORMED sha256:") != NULL);
    omega_graph_destroy(m);

    /* shared helpers coexist with visor.c */
    char idt[72];
    visor_format_id(&v7->id, idt);
    CHECK(visor_semantic_inspect(g, &v7->id, &v1) == 0 && strcmp(idt, v1.id_text) == 0);

    omega_graph_destroy(g);
    printf("PASS %d/%d\n", g_pass, g_total);
    return g_pass == g_total ? 0 : 1;
}
