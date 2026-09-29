/* Lane 4: visor_verify tests. Prints "PASS n/n" on success. */
#include <stdio.h>
#include <string.h>

#include "omega_core.h"
#include "omega_machine.h"
#include "omega_program.h"
#include "omega_realize.h"
#include "visor_verify.h"

static int g_total, g_pass;
#define CHECK(cond, msg) do { g_total++; if (cond) g_pass++; else fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } while (0)

static char txt_a[8192], txt_b[8192], json_buf[8192];

typedef struct { OmegaGraph *g; SemanticId t, a, b, op, ap; } AddGraph;

static int build_add(AddGraph *x, OpCode opc) {
    x->g = omega_graph_create();
    if (!x->g) return -1;
    OmegaObject *t = omega_build_type_uint(x->g, 64);
    x->t = t->id;
    x->a = omega_build_val_uint(x->g, &x->t, 64, 7)->id;
    x->b = omega_build_val_uint(x->g, &x->t, 64, 11)->id;
    x->op = omega_build_op_binary(x->g, opc, OVERFLOW_WRAP, &x->t)->id;
    x->ap = omega_build_apply(x->g, &x->op, &x->a, &x->b)->id;
    return 0;
}

int main(void) {
    VisorVerifyReport r;
    AddGraph x;
    CHECK(build_add(&x, OP_ADD) == 0, "build 7+11 graph");

    /* 1. No realization: V0 PASS, V1 NOT_RUN (not an error), overall PASS. */
    CHECK(visor_verify_object(x.g, &x.ap, NULL, NULL, &r) == 0, "verify apply rc");
    CHECK(r.row_count == 6, "six rows");
    CHECK(strcmp(r.rows[0].name, "STRUCTURAL") == 0 && strcmp(r.rows[5].name, "MACHINE") == 0, "row order");
    CHECK(r.rows[VISOR_ROW_STRUCTURAL].status == VISOR_CHECK_PASS && r.rows[VISOR_ROW_STRUCTURAL].checks > 0, "V0 pass");
    CHECK(r.rows[VISOR_ROW_TYPE].status == VISOR_CHECK_PASS, "type pass");
    CHECK(r.rows[VISOR_ROW_INVARIANTS].status == VISOR_CHECK_PASS, "V2 pass");
    CHECK(r.rows[VISOR_ROW_REALIZATION].status == VISOR_CHECK_NOT_RUN &&
          strcmp(r.rows[VISOR_ROW_REALIZATION].detail, "no realization built") == 0, "V1 not run w/o realization");
    CHECK(r.rows[VISOR_ROW_AUTHORITY].status == VISOR_CHECK_PASS && !r.authority_required &&
          strncmp(r.rows[VISOR_ROW_AUTHORITY].detail, "NONE", 4) == 0, "authority NONE");
    CHECK(r.rows[VISOR_ROW_MACHINE].status == VISOR_CHECK_NOT_RUN, "machine not run");
    CHECK(r.passed && r.first_violation[0] == 0, "passed without realization");

    /* Deterministic text: two independent runs render identically. */
    CHECK(visor_verify_format_text(&r, txt_a, sizeof(txt_a)) > 0, "format text");
    VisorVerifyReport r2;
    visor_verify_object(x.g, &x.ap, NULL, NULL, &r2);
    CHECK(visor_verify_format_text(&r2, txt_b, sizeof(txt_b)) > 0 && strcmp(txt_a, txt_b) == 0, "text deterministic");
    CHECK(strncmp(txt_a, "STRUCTURAL      PASS", 20) == 0, "aligned STRUCTURAL line");
    CHECK(visor_verify_format_json(&r, json_buf, sizeof(json_buf)) > 0 && json_buf[0] == '{' &&
          strstr(json_buf, "\"failure\":null") && strstr(json_buf, "\"granted\":false"), "json render");
    CHECK(visor_verify_format_text(&r, txt_b, 16) == -1, "truncation fails closed");

    /* 2. With a pure-binary ADD realization and a machine model: V1 PASS, MACHINE PASS. */
    RealizationObject real;
    OmegaMachineGraph mg;
    CHECK(omega_realize_pure_binary(x.g, &x.op, &real) == 0, "realize ADD");
    CHECK(omega_machine_build_dgx_spark(&mg) == 0, "machine model");
    visor_verify_object(x.g, &x.ap, &real, &mg, &r);
    CHECK(r.rows[VISOR_ROW_REALIZATION].status == VISOR_CHECK_PASS &&
          r.rows[VISOR_ROW_REALIZATION].checks >= 11, "V1 differential pass with realization");
    CHECK(r.rows[VISOR_ROW_MACHINE].status == VISOR_CHECK_PASS, "machine compat pass");
    CHECK(r.passed, "passed with realization");

    /* 3. Corrupted realization: V1 (which runs V0 on the realization) fails closed. */
    RealizationObject bad = real;
    bad.code_bytes[0] ^= 0x01;
    visor_verify_object(x.g, &x.ap, &bad, &mg, &r);
    CHECK(r.rows[VISOR_ROW_REALIZATION].status == VISOR_CHECK_FAIL && !r.passed, "corrupt realization fails");
    CHECK(r.first_violation[0] != 0 && r.object_involved[0] != 0, "failure block filled");
    CHECK(visor_verify_format_text(&r, txt_a, sizeof(txt_a)) > 0 && strstr(txt_a, "first violation:"), "text failure block");

    /* 4. Machine profile mismatch. */
    OmegaMachineGraph mg2 = mg;
    mg2.target_profile = 0x02;
    visor_verify_object(x.g, &x.ap, &real, &mg2, &r);
    CHECK(r.rows[VISOR_ROW_MACHINE].status == VISOR_CHECK_FAIL && !r.passed && r.expected[0] && r.observed[0],
          "machine mismatch fails with expected/observed");

    /* 5. Realization for a different object is refused. */
    visor_verify_object(x.g, &x.a, &real, NULL, &r);
    CHECK(r.rows[VISOR_ROW_REALIZATION].status == VISOR_CHECK_FAIL && !r.passed, "binding mismatch fails");

    /* 6. Structural failure: object not in graph. */
    SemanticId missing;
    memset(&missing, 0xAB, sizeof(missing));
    CHECK(visor_verify_object(x.g, &missing, NULL, NULL, &r) == 0, "missing id rc 0");
    CHECK(r.rows[VISOR_ROW_STRUCTURAL].status == VISOR_CHECK_FAIL && !r.passed &&
          strcmp(r.observed, "absent") == 0, "missing object fails structural");

    /* 7. Invalid arguments. */
    CHECK(visor_verify_object(NULL, &x.ap, NULL, NULL, &r) == -1 && !r.passed, "null graph -1");

    /* 8. SUB realization: the V1 reference does not model SUB -> NOT_RUN, not a fake verdict. */
    AddGraph s;
    CHECK(build_add(&s, OP_SUB) == 0, "build sub graph");
    RealizationObject sreal;
    CHECK(omega_realize_pure_binary(s.g, &s.op, &sreal) == 0, "realize SUB");
    visor_verify_object(s.g, &s.ap, &sreal, NULL, &r);
    CHECK(r.rows[VISOR_ROW_REALIZATION].status == VISOR_CHECK_NOT_RUN &&
          strstr(r.rows[VISOR_ROW_REALIZATION].detail, "not applicable"), "SUB V1 not applicable");
    CHECK(r.passed && r.rows[VISOR_ROW_REALIZATION].checks > 0, "SUB realization still V0-checked");
    RealizationObject sbad = sreal;
    sbad.code_bytes[0] ^= 0x01;
    visor_verify_object(s.g, &s.ap, &sbad, NULL, &r);
    CHECK(!r.passed && r.rows[VISOR_ROW_REALIZATION].status == VISOR_CHECK_FAIL &&
          strstr(r.first_violation, "REALIZATION_ID"), "tampered SUB realization fails closed");
    /* SUB code re-bound (with a valid realization id) to the ADD op: V1 catches the divergence. */
    RealizationObject lie = sreal;
    lie.semantic_id = x.op;
    omega_compute_realization_id(&lie);
    visor_verify_object(x.g, &x.ap, &lie, NULL, &r);
    CHECK(r.rows[VISOR_ROW_REALIZATION].status == VISOR_CHECK_FAIL && !r.passed &&
          strstr(r.first_violation, "Mismatch") && r.expected[0] && r.observed[0] &&
          strcmp(r.expected, r.observed) != 0, "V1 mismatch fills expected/observed");
    omega_graph_destroy(s.g);

    /* 9. Malformed graph: a dangling relation makes V0 fail. */
    OmegaObject *ap = omega_graph_find_object(x.g, &x.ap);
    ap->relations[0].kind = REL_DEPENDS_ON;
    memset(&ap->relations[0].target_id, 0xCD, sizeof(SemanticId));
    ap->rel_count = 1;
    CHECK(visor_verify_object(x.g, &x.ap, NULL, NULL, &r) == 0, "malformed rc 0");
    CHECK(r.rows[VISOR_ROW_STRUCTURAL].status == VISOR_CHECK_FAIL && !r.passed && r.first_violation[0],
          "malformed graph fails V0");
    omega_graph_destroy(x.g);

    /* 10. f(a,b,c)=(a+b)-c apply realization: V1 default vectors apply directly. */
    OmegaGraph *fg = omega_graph_create();
    SemanticId root;
    RealizationObject freal;
    CHECK(fg && omega_build_f_add_sub_graph(fg, &root) == 0 && omega_realize_f_add_sub(fg, &root, &freal) == 0,
          "build f_add_sub");
    visor_verify_object(fg, &root, &freal, NULL, &r);
    CHECK(r.rows[VISOR_ROW_REALIZATION].status == VISOR_CHECK_PASS && r.passed, "f_add_sub V1 pass");

    /* 11. Effect object: authority requirement reported, never granted. */
    OmegaObject *eff = omega_build_effect(fg, 1, 1, 3, 1);
    SemanticId eid = eff->id;
    visor_verify_object(fg, &eid, NULL, NULL, &r);
    CHECK(r.authority_required && r.effect_count == 1 &&
          strncmp(r.rows[VISOR_ROW_AUTHORITY].detail, "REQUIRED", 8) == 0, "effect -> REQUIRED");
    visor_verify_format_json(&r, json_buf, sizeof(json_buf));
    CHECK(strstr(json_buf, "\"required\":true") && strstr(json_buf, "\"granted\":false"), "json authority");
    omega_graph_destroy(fg);

    /* 12. Program verify. omega_program_realize is declared in omega_program.h
     * but has no definition in the tree; omega_program_build_unary_op already
     * emits the realization and sets is_realized, so that is what we use. */
    OmegaProgram p;
    CHECK(omega_program_build_unary_op(&p, "add5", OP_ADD, 5) == 0 && p.is_realized, "build unary program (realized)");
    CHECK(visor_verify_program(&p, &r) == 0 && r.passed, "program verify pass");
    CHECK(r.rows[VISOR_ROW_STRUCTURAL].status == VISOR_CHECK_PASS &&
          r.rows[VISOR_ROW_INVARIANTS].status == VISOR_CHECK_PASS &&
          r.rows[VISOR_ROW_REALIZATION].status == VISOR_CHECK_NOT_RUN, "program rows");
    /* Agreement with the existing program verifier. */
    OmegaProgram copy = p;
    VerifyReport vr;
    CHECK(omega_program_verify(&copy, &vr) == 0, "omega_program_verify agrees");
    OmegaProgram unreal = p;
    unreal.is_realized = false;
    visor_verify_program(&unreal, &r);
    CHECK(!r.passed && strcmp(r.first_violation, "program not realized") == 0, "unrealized program fails closed");
    OmegaProgram corrupt = p;
    corrupt.realization.code_bytes[0] ^= 0x01;
    visor_verify_program(&corrupt, &r);
    CHECK(!r.passed && r.rows[VISOR_ROW_STRUCTURAL].status == VISOR_CHECK_FAIL, "corrupt program fails V0");
    omega_program_destroy(&p);

    printf("%s %d/%d\n", g_pass == g_total ? "PASS" : "FAIL", g_pass, g_total);
    return g_pass == g_total ? 0 : 1;
}
