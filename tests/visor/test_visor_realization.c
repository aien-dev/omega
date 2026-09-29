/* Lane 5: realization lab tests. Prints "PASS n/n". */
#include "visor_realization.h"
#include "visor_machine.h"
#include "visor.h"
#include "omega_core.h"
#include "aarch64_encoder.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_pass, g_total;
#define CHECK(cond, msg) do { g_total++; if (cond) g_pass++; else fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } while (0)

static char b1[16384], b2[16384];
static VisorRealizationView alt1[4], alt2[4], altv[4];
static VisorRealizationEntry alte[4];

static SemanticId make_apply(OmegaGraph *g, OpCode op, OverflowPolicy ov, uint16_t width, SemanticId *op_out) {
    OmegaObject *t = omega_build_type_uint(g, width);
    OmegaObject *a = omega_build_val_uint(g, &t->id, width, 7);
    OmegaObject *b = omega_build_val_uint(g, &t->id, width, 11);
    OmegaObject *o = omega_build_op_binary(g, op, ov, &t->id);
    OmegaObject *ap = omega_build_apply(g, &o->id, &a->id, &b->id);
    if (op_out) *op_out = o->id;
    return ap->id;
}

int main(void) {
    OmegaMachineGraph spark, qemu;
    omega_machine_build_dgx_spark(&spark);
    omega_machine_build_qemu_virt(&qemu);
    OmegaGraph *g = omega_graph_create();
    CHECK(g != NULL, "graph");

    /* 7 + 11 apply */
    SemanticId add_op;
    SemanticId add_ap = make_apply(g, OP_ADD, OVERFLOW_WRAP, 64, &add_op);
    VisorRealizationEntry e;
    VisorRealizationView v;
    CHECK(visor_realize_apply(g, &add_ap, &spark, &e, &v) == 0, "realize 7+11 apply");
    CHECK(e.verdict_known && e.compatible, "direct apply entry verdict compatible");
    CHECK(memcmp(&e.subject_id, &add_ap, sizeof(SemanticId)) == 0, "subject = apply id");
    CHECK(memcmp(&e.real.semantic_id, &add_op, sizeof(SemanticId)) == 0, "realized = op id");
    CHECK(strcmp(v.subject_id, v.realized_id) != 0, "subject and realized both shown, distinct");
    CHECK(e.has_machine_id && e.has_estimate && strcmp(e.target_name, VISOR_TARGET_AARCH64) == 0, "entry machine/target");
    CHECK(v.disasm_count == 2 && strstr(v.disasm[0], "add x0, x0, x1") && strstr(v.disasm[1], "ret"), "disasm");
    uint64_t args[3] = { 7, 11, 0 }, r = 0;
    CHECK(visor_realization_run_pure(&e, args, 2, &r) == 0 && r == 18, "run_pure(7,11)==18");
    args[0] = UINT64_MAX; args[1] = 1;
    CHECK(visor_realization_run_pure(&e, args, 2, &r) == 0 && r == 0, "(2^64-1)+1 wraps to 0");
    CHECK(visor_realization_run_pure(&e, args, 4, &r) == -3, "argc>3 refused");
    CHECK(v.compatible && v.runnable, "compatible + runnable");

    /* fail closed */
    SemanticId div_ap = make_apply(g, OP_DIV, OVERFLOW_WRAP, 64, NULL);
    VisorRealizationEntry ebad;
    VisorRealizationView vbad;
    CHECK(visor_realize_apply(g, &div_ap, &spark, &ebad, &vbad) == -2 && strstr(vbad.why, "DIV"), "DIV unsupported");
    CHECK(visor_realization_format_text(&vbad, b1, sizeof(b1)) > 0 && visor_realization_why(&vbad, b2, sizeof(b2)) > 0 &&
          strstr(b1, "not realized") && !vbad.cost.measured.present, "refused view formats safely");
    SemanticId sat_ap = make_apply(g, OP_ADD, OVERFLOW_SATURATE, 64, NULL);
    CHECK(visor_realize_apply(g, &sat_ap, &spark, &ebad, &vbad) == -2 && strstr(vbad.why, "overflow"), "SATURATE refused");
    SemanticId u32_ap = make_apply(g, OP_ADD, OVERFLOW_WRAP, 32, NULL);
    CHECK(visor_realize_apply(g, &u32_ap, &spark, &ebad, &vbad) == -2 && strstr(vbad.why, "width 32"), "u32 refused");
    SemanticId junk;
    memset(&junk, 0x5A, sizeof(junk));
    CHECK(visor_realize_apply(g, &junk, &spark, &ebad, &vbad) == -2 && vbad.why[0], "unknown id refused");
    OmegaObject *t64 = omega_build_type_uint(g, 64);
    CHECK(visor_realize_apply(g, &t64->id, &spark, &ebad, &vbad) == -2, "TYPE object refused");

    /* cost: four separate slots */
    VisorCostView c;
    CHECK(visor_realization_cost(&e, &spark, &c) == 0, "cost");
    CHECK(c.predicted.present && c.predicted.cls == VISOR_COST_PREDICTED && c.predicted.insn_count == 2, "predicted present");
    CHECK(c.estimated.present && c.estimated.cls == VISOR_COST_ESTIMATED && strstr(c.estimated.source, "omega_machine_estimate_latency"),
          "estimated present from machine model");
    CHECK(c.measured.present == false && c.measured.cls == VISOR_COST_MEASURED, "measured ABSENT");
    CHECK(c.qualified.present == false && c.qualified.cls == VISOR_COST_QUALIFIED, "qualified ABSENT");
    CHECK(strcmp(c.predicted.source, c.estimated.source) != 0 && strcmp(c.predicted.cls_name, "predicted") == 0 &&
          strcmp(c.estimated.cls_name, "estimated") == 0, "classes never conflated");
    CHECK(c.measured.cycles == 0 && c.qualified.cycles == 0, "absent slots carry no numbers");
    CHECK(visor_cost_format_json(&c, b1, sizeof(b1)) > 0 && strstr(b1, "\"measured\":{\"present\":false"), "cost json");
    CHECK(visor_cost_format_text(&c, b1, sizeof(b1)) > 0 && strstr(b1, "measured  ABSENT") && strstr(b1, "qualified ABSENT"),
          "cost text");

    /* program realize + run */
    OmegaProgram p;
    CHECK(omega_program_build_unary_op(&p, "add5", OP_ADD, 5) == 0, "build add5");
    VisorRealizationEntry ep;
    VisorRealizationView vp;
    CHECK(visor_realize_program(&p, &spark, &ep, &vp) == 0 && ep.subject_is_program, "realize program");
    CHECK(ep.verdict_known && ep.compatible && ep.incompatible_reason[0] == '\0', "direct program entry verdict compatible");
    args[0] = 10;
    CHECK(visor_realization_run_pure(&ep, args, 1, &r) == 0 && r == 15, "program run 10+5==15");
    OmegaProgram unreal;
    omega_program_init(&unreal, "empty");
    CHECK(visor_realize_program(&unreal, &spark, &ep, &vp) == -2 && vp.why[0], "unrealized program refused");

    /* alternatives: deterministic, labelled, synth checked against direct */
    size_t n1 = 0, n2 = 0;
    CHECK(visor_realization_alternatives(&p, &spark, alt1, 4, &n1) == 0 && n1 == 4, "alternatives count");
    CHECK(visor_realization_alternatives(&p, &spark, alt2, 4, &n2) == 0 && n2 == 4, "alternatives again");
    bool same = true;
    for (size_t i = 0; i < 4; ++i) {
        visor_realization_format_text(&alt1[i], b1, sizeof(b1));
        visor_realization_format_text(&alt2[i], b2, sizeof(b2));
        if (strcmp(b1, b2) != 0) same = false;
    }
    CHECK(same, "alternatives deterministic");
    CHECK(strcmp(alt1[0].label, "direct@dgx-spark") == 0 && strcmp(alt1[1].label, "direct@qemu-virt") == 0 &&
          strcmp(alt1[2].label, "synth@dgx-spark") == 0 && strcmp(alt1[3].label, "synth@qemu-virt") == 0, "labels/order");
    CHECK(alt1[0].compatible && alt1[1].compatible, "direct alternatives compatible");
    CHECK(!alt1[2].compatible && !alt1[2].runnable && strstr(alt1[2].why, "MISMATCH"), "synth for add5 flagged mismatch");
    /* the stored entry carries the verdict: re-viewing it later stays incompatible, run refused */
    size_t ne = 0;
    CHECK(visor_realization_alternatives_ex(&p, &spark, alte, altv, 4, &ne) == 0 && ne == 4, "alternatives_ex count");
    CHECK(alte[0].verdict_known && alte[0].compatible && alte[1].verdict_known && alte[1].compatible,
          "direct alternative entries compatible");
    CHECK(alte[2].verdict_known && !alte[2].compatible && alte[2].incompatible_reason[0], "synth entry carries incompatible verdict");
    VisorRealizationView rv;
    CHECK(visor_realization_view(&alte[2], &spark, &rv) == 0 && !rv.compatible && !rv.runnable &&
          strstr(rv.why, "INCOMPATIBLE") && strstr(rv.why, alte[2].incompatible_reason),
          "re-viewed incompatible entry: not compatible, not runnable, reason in why");
    args[0] = 10;
    CHECK(visor_realization_run_pure(&alte[2], args, 1, &r) == -3, "incompatible synth entry run refused");
    CHECK(visor_realization_view(&alte[0], &spark, &rv) == 0 && rv.compatible && !strstr(rv.why, "INCOMPATIBLE"),
          "re-viewed direct entry unchanged");
    CHECK(visor_realization_run_pure(&alte[0], args, 1, &r) == 0 && r == 15, "direct alternative entry runs 10+5==15");
    VisorRealizationEntry unk = alte[0];
    unk.verdict_known = false; unk.compatible = false;
    CHECK(visor_realization_run_pure(&unk, args, 1, &r) == 0 && r == 15, "verdict not evaluated treated as compatible");
    /* 3x-2 program: synth agrees */
    OmegaProgram m3, s2, aff;
    omega_program_build_unary_op(&m3, "mul3", OP_MUL, 3);
    omega_program_build_unary_op(&s2, "sub2", OP_SUB, 2);
    char err[128];
    CHECK(omega_program_compose(&m3, &s2, &aff, err, sizeof(err)) == 0, "compose 3x-2");
    CHECK(visor_realization_alternatives(&aff, &spark, alt2, 4, &n2) == 0 && n2 == 4, "alternatives 3x-2");
    CHECK(alt2[2].compatible && strstr(alt2[2].why, "matches direct"), "synth for 3x-2 matches direct");
    CHECK(alt2[2].insn_count == 5 && alt2[2].cost.estimated.present && !alt2[2].cost.measured.present, "synth cost view");
    size_t n3 = 0;
    CHECK(visor_realization_alternatives(&aff, &spark, alt2, 2, &n3) == 0 && n3 == 2, "max respected");

    /* compare / why */
    CHECK(visor_realization_compare(&alt1[0], &alt1[1], b1, sizeof(b1)) > 0 && strstr(b1, "profile") &&
          strstr(b1, "omega_machine_estimate_latency") && strstr(b1, "code           identical"), "compare");
    CHECK(strstr(b1, "measured  A=ABSENT B=ABSENT"), "compare never compares absent measured");
    CHECK(visor_realization_why(&v, b1, sizeof(b1)) > 0 && strstr(b1, "profile") && strstr(b1, "cost sources") &&
          strstr(b1, "ABSENT"), "why");

    /* format json/text */
    CHECK(visor_realization_format_json(&v, b1, sizeof(b1)) > 0 && b1[0] == '{' && strstr(b1, "\"disasm\":["), "json");
    CHECK(visor_realization_format_text(&v, b1, 64) == -1, "text truncation fails closed");

    /* Blackwell entries: run refused */
    VisorRealizationEntry bw = e;
    snprintf(bw.target_name, sizeof(bw.target_name), "%s", VISOR_TARGET_BLACKWELL);
    CHECK(visor_realization_run_pure(&bw, args, 2, &r) == -3, "blackwell target run refused");
    VisorRealizationView vbw;
    CHECK(visor_realization_view(&bw, &spark, &vbw) == 0 && !vbw.runnable && !vbw.compatible, "blackwell view not runnable");
    VisorRealizationEntry bw2 = e;
    CHECK(visor_machine_blackwell_id(&bw2.machine_id) == 0, "blackwell id");
    bw2.has_machine_id = true;
    CHECK(visor_realization_run_pure(&bw2, args, 2, &r) == -3, "blackwell machine id run refused");

    /* non-pure code refused */
    VisorRealizationEntry br = e;
    size_t pos = 0;
    aarch64_emit_b(br.real.code_bytes, &pos, sizeof(br.real.code_bytes), 0);
    aarch64_emit_ret(br.real.code_bytes, &pos, sizeof(br.real.code_bytes));
    br.real.code_len = pos;
    CHECK(visor_realization_run_pure(&br, args, 2, &r) == -3, "branch code refused");
    VisorRealizationEntry bp = e;
    bp.real.target_profile = 0x02;
    CHECK(visor_realization_run_pure(&bp, args, 2, &r) == -3, "non-v8a profile refused");
    VisorRealizationEntry nr = e;
    nr.real.code_len = 4; /* ADD without RET */
    CHECK(visor_realization_run_pure(&nr, args, 2, &r) == -3, "missing RET refused");

    visor_realization_format_text(&v, b1, sizeof(b1));
    fputs(b1, stdout);
    omega_program_destroy(&p);
    omega_graph_destroy(g);
    printf("%s %d/%d\n", g_pass == g_total ? "PASS" : "FAIL", g_pass, g_total);
    return g_pass == g_total ? 0 : 1;
}
