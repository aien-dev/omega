/*
 * visor_realization.c -- Omega Visor V1, lane 5: realization lab.
 * See visor_realization.h. Reuses existing realizers only; never invents code,
 * ids or costs. No physics headers. Fixed storage. Fails closed.
 */
#include "visor_realization.h"
#include "visor_machine.h"
#include "omega_core.h"
#include "omega_realize.h"
#include "omega_realize_synth.h"
#include "omega_exec.h"
#include "aarch64_decoder.h"
#include "aarch64_target.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- small helpers ------------------------------------------------------- */

typedef struct { char *buf; size_t n, pos; bool trunc; } Out;

static void outf(Out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void outf(Out *o, const char *fmt, ...) {
    if (o->trunc) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(o->buf + o->pos, o->n - o->pos, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= o->n - o->pos) { o->trunc = true; o->buf[o->n - 1] = '\0'; return; }
    o->pos += (size_t)w;
}

static void json_str(Out *o, const char *s) {
    outf(o, "\"");
    for (; s && *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') outf(o, "\\%c", c);
        else if (c < 0x20) outf(o, "\\u%04x", c);
        else outf(o, "%c", c);
    }
    outf(o, "\"");
}

static bool id_is_zero(const SemanticId *id) {
    for (size_t i = 0; i < OMEGA_ID_BYTES; ++i) if (id->bytes[i]) return false;
    return true;
}

static bool id_eq(const SemanticId *a, const SemanticId *b) {
    return memcmp(a->bytes, b->bytes, OMEGA_ID_BYTES) == 0;
}

static const char *opcode_name(OpCode op) {
    switch (op) {
        case OP_ADD: return "ADD";
        case OP_SUB: return "SUB";
        case OP_MUL: return "MUL";
        case OP_DIV: return "DIV";
        case OP_AND: return "AND";
        case OP_OR: return "OR";
        case OP_NOT: return "NOT";
        case OP_EQUAL: return "EQUAL";
        case OP_LESS_THAN: return "LESS_THAN";
        case OP_IDENTITY: return "IDENTITY";
        case OP_CONSTANT: return "CONSTANT";
        case OP_SELECT: return "SELECT";
        case OP_CONCAT: return "CONCAT";
        case OP_SLICE: return "SLICE";
        case OP_COMPILE: return "COMPILE";
        case OP_CONVERT: return "CONVERT";
        default: return "INVALID";
    }
}

static const char *cost_cls_name(VisorCostClass c) {
    switch (c) {
        case VISOR_COST_PREDICTED: return "predicted";
        case VISOR_COST_ESTIMATED: return "estimated";
        case VISOR_COST_MEASURED: return "measured";
        case VISOR_COST_QUALIFIED: return "qualified";
    }
    return "unknown";
}

/* Short target name for a canonical machine, else "" */
static void machine_target_name(const OmegaMachineGraph *mg, char out[32]) {
    OmegaMachineGraph c;
    out[0] = '\0';
    if (!mg) { snprintf(out, 32, "%s", VISOR_TARGET_AARCH64); return; }
    if (omega_machine_build_dgx_spark(&c) == 0 && id_eq(&c.machine_id, &mg->machine_id)) {
        snprintf(out, 32, "%s", VISOR_TARGET_DGX_SPARK); return;
    }
    if (omega_machine_build_qemu_virt(&c) == 0 && id_eq(&c.machine_id, &mg->machine_id)) {
        snprintf(out, 32, "%s", VISOR_TARGET_QEMU_VIRT); return;
    }
    snprintf(out, 32, "%s", VISOR_TARGET_AARCH64);
}

/* ---- disassembly --------------------------------------------------------- */

static const char *reg(bool sf, uint8_t r, char buf[8]) {
    if (r == 31) snprintf(buf, 8, "%s", sf ? "xzr" : "wzr");
    else snprintf(buf, 8, "%c%u", sf ? 'x' : 'w', r);
    return buf;
}

size_t visor_disasm(const uint8_t *code, size_t len, char lines[][48], size_t max) {
    if (!code || !lines) return 0;
    size_t count = 0;
    for (size_t off = 0; off + 4 <= len && count < max; off += 4) {
        uint32_t w = (uint32_t)code[off] | ((uint32_t)code[off + 1] << 8) |
                     ((uint32_t)code[off + 2] << 16) | ((uint32_t)code[off + 3] << 24);
        DecodedInsn d;
        char a[8], b[8], c[8];
        char *L = lines[count++];
        if (aarch64_decode_instruction(w, &d) != 0) { snprintf(L, 48, "%08x  .word 0x%08x", w, w); continue; }
        switch (d.op) {
            case DECODED_ADD: case DECODED_SUB: case DECODED_MUL: case DECODED_AND:
            case DECODED_ORR: case DECODED_EOR: {
                const char *m = d.op == DECODED_ADD ? "add" : d.op == DECODED_SUB ? "sub" :
                                d.op == DECODED_MUL ? "mul" : d.op == DECODED_AND ? "and" :
                                d.op == DECODED_ORR ? "orr" : "eor";
                uint32_t imm6 = (w >> 10) & 0x3F, sh = (w >> 22) & 3;
                if (d.op != DECODED_MUL && imm6)
                    snprintf(L, 48, "%08x  %s %s, %s, %s, %s #%u", w, m, reg(d.sf, d.rd, a), reg(d.sf, d.rn, b),
                             reg(d.sf, d.rm, c), sh == 0 ? "lsl" : sh == 1 ? "lsr" : sh == 2 ? "asr" : "ror", imm6);
                else
                    snprintf(L, 48, "%08x  %s %s, %s, %s", w, m, reg(d.sf, d.rd, a), reg(d.sf, d.rn, b),
                             reg(d.sf, d.rm, c));
                break;
            }
            case DECODED_MOVZ: case DECODED_MOVK: {
                uint32_t hw = (w >> 21) & 3;
                if (hw)
                    snprintf(L, 48, "%08x  %s %s, #0x%x, lsl #%u", w, d.op == DECODED_MOVZ ? "movz" : "movk",
                             reg(d.sf, d.rd, a), d.imm16, hw * 16);
                else
                    snprintf(L, 48, "%08x  %s %s, #0x%x", w, d.op == DECODED_MOVZ ? "movz" : "movk",
                             reg(d.sf, d.rd, a), d.imm16);
                break;
            }
            case DECODED_RET: snprintf(L, 48, "%08x  ret", w); break;
            case DECODED_B: snprintf(L, 48, "%08x  b imm26=0x%x", w, (unsigned)d.branch_imm); break;
            case DECODED_B_COND: snprintf(L, 48, "%08x  b.cond %u imm19=0x%x", w, (unsigned)d.cond, (unsigned)d.branch_imm); break;
            case DECODED_CBZ: snprintf(L, 48, "%08x  cbz %s imm19=0x%x", w, reg(d.sf, d.rd, a), (unsigned)d.branch_imm); break;
            case DECODED_CBNZ: snprintf(L, 48, "%08x  cbnz %s imm19=0x%x", w, reg(d.sf, d.rd, a), (unsigned)d.branch_imm); break;
            case DECODED_ADR: snprintf(L, 48, "%08x  adr", w); break;
            case DECODED_LDR: snprintf(L, 48, "%08x  ldr", w); break;
            case DECODED_STR: snprintf(L, 48, "%08x  str", w); break;
            case DECODED_LDRB: snprintf(L, 48, "%08x  ldrb", w); break;
            case DECODED_STRB: snprintf(L, 48, "%08x  strb", w); break;
            case DECODED_SUBS: snprintf(L, 48, "%08x  subs", w); break;
            default: snprintf(L, 48, "%08x  .word 0x%08x", w, w); break;
        }
    }
    return count;
}

/* ---- run gate ------------------------------------------------------------ */

static bool target_is_aarch64(const char *t) {
    return strcmp(t, VISOR_TARGET_AARCH64) == 0 || strcmp(t, VISOR_TARGET_DGX_SPARK) == 0 ||
           strcmp(t, VISOR_TARGET_QEMU_VIRT) == 0;
}

/* 0 = runnable; -3 = refused with reason. */
static int run_check(const VisorRealizationEntry *e, char *why, size_t n) {
    if (!e) { snprintf(why, n, "no realization"); return -3; }
    SemanticId bw;
    if (strcmp(e->target_name, VISOR_TARGET_BLACKWELL) == 0 ||
        (e->has_machine_id && visor_machine_blackwell_id(&bw) == 0 && id_eq(&bw, &e->machine_id))) {
        snprintf(why, n, "Blackwell target: requires GPU submit authority; not linked in Visor V1");
        return -3;
    }
    if (!target_is_aarch64(e->target_name)) {
        snprintf(why, n, "target '%.31s' is not an AArch64 target", e->target_name);
        return -3;
    }
    if (e->real.target_profile != AARCH64_PROFILE_V8A_BAREMETAL) {
        snprintf(why, n, "profile 0x%02x is not AArch64 v8a (0x01)", e->real.target_profile);
        return -3;
    }
    size_t len = e->real.code_len;
    if (len == 0 || len % 4 || len > AARCH64_MAX_CODE_BYTES || e->real.entry_offset != 0) {
        snprintf(why, n, "code length %zu / entry offset %u not runnable", len, e->real.entry_offset);
        return -3;
    }
    char err[96] = {0};
    if (aarch64_validate_code_buffer(e->real.code_bytes, len, err, sizeof(err)) != 0) {
        snprintf(why, n, "code validation failed: %.80s", err);
        return -3;
    }
    for (size_t off = 0; off < len; off += 4) {
        const uint8_t *p = &e->real.code_bytes[off];
        uint32_t w = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        DecodedInsn d;
        if (aarch64_decode_instruction(w, &d) != 0) {
            snprintf(why, n, "undecodable instruction 0x%08x at +%zu", w, off);
            return -3;
        }
        bool last = off + 4 == len;
        switch (d.op) {
            case DECODED_ADD: case DECODED_SUB: case DECODED_MUL: case DECODED_AND:
            case DECODED_ORR: case DECODED_EOR: case DECODED_MOVZ: case DECODED_MOVK:
                if (last) { snprintf(why, n, "code does not end in RET"); return -3; }
                break;
            case DECODED_RET:
                if (!last) { snprintf(why, n, "RET before end of code at +%zu", off); return -3; }
                break;
            default:
                snprintf(why, n, "non-pure instruction 0x%08x at +%zu (branch/load/store not allowed)", w, off);
                return -3;
        }
    }
#if !defined(__aarch64__)
    snprintf(why, n, "host is not aarch64");
    return -3;
#else
    snprintf(why, n, "pure AArch64 register-only code ending in RET; executes natively on host");
    return 0;
#endif
}

int visor_realization_run_pure(const VisorRealizationEntry *e, const uint64_t *args, size_t argc, uint64_t *out) {
    if (!e || !out || (argc > 0 && !args)) return -1;
    if (argc > 3) return -3;
    if (e->verdict_known && !e->compatible) return -3;
    char why[160];
    if (run_check(e, why, sizeof(why)) != 0) return -3;
    uint64_t a = argc > 0 ? args[0] : 0, b = argc > 1 ? args[1] : 0, c = argc > 2 ? args[2] : 0;
    return omega_exec_native_f3(&e->real, a, b, c, out) == 0 ? 0 : -1;
}

/* ---- cost ---------------------------------------------------------------- */

static void cost_slot(VisorCostItem *it, VisorCostClass cls) {
    memset(it, 0, sizeof(*it));
    it->cls = cls;
    it->cls_name = cost_cls_name(cls);
}

int visor_realization_cost(const VisorRealizationEntry *e, const OmegaMachineGraph *mg, VisorCostView *out) {
    if (!e || !out) return -1;
    memset(out, 0, sizeof(*out));
    cost_slot(&out->predicted, VISOR_COST_PREDICTED);
    cost_slot(&out->estimated, VISOR_COST_ESTIMATED);
    cost_slot(&out->measured, VISOR_COST_MEASURED);
    cost_slot(&out->qualified, VISOR_COST_QUALIFIED);
    uint32_t insns = (uint32_t)(e->real.code_len / 4);
    uint32_t bytes = (uint32_t)e->real.code_len;

    if (insns > 0) {
        out->predicted.present = true;
        out->predicted.cycles = insns;
        out->predicted.insn_count = insns;
        out->predicted.code_bytes = bytes;
        snprintf(out->predicted.source, sizeof(out->predicted.source),
                 "static: code_len/4 = %u instructions x 1 cycle; no machine model", insns);
    } else {
        snprintf(out->predicted.source, sizeof(out->predicted.source), "absent: empty code");
    }

    if (mg && insns > 0) {
        out->estimated.present = true;
        out->estimated.cycles = omega_machine_estimate_latency(mg, &e->real);
        out->estimated.insn_count = insns;
        out->estimated.code_bytes = bytes;
        snprintf(out->estimated.source, sizeof(out->estimated.source),
                 "omega_machine_estimate_latency(%.63s) (assumed profile, not measured)", mg->name);
    } else if (e->has_estimate) {
        out->estimated.present = true;
        out->estimated.cycles = e->estimated_cycles;
        out->estimated.insn_count = insns;
        out->estimated.code_bytes = bytes;
        snprintf(out->estimated.source, sizeof(out->estimated.source),
                 "machine-model estimate recorded when realized (assumed profile, not measured)");
    } else {
        snprintf(out->estimated.source, sizeof(out->estimated.source), "absent: no machine model");
    }

    snprintf(out->measured.source, sizeof(out->measured.source),
             "absent: Visor V1 ingests no execution receipts");
    snprintf(out->qualified.source, sizeof(out->qualified.source),
             "absent: no qualified gate result for this realization in V1");
    return 0;
}

/* ---- view ---------------------------------------------------------------- */

int visor_realization_view(const VisorRealizationEntry *e, const OmegaMachineGraph *mg, VisorRealizationView *out) {
    if (!e || !out) return -1;
    memset(out, 0, sizeof(*out));
    if (e->real.has_id) visor_format_id(&e->real.realization_id, out->realization_id);
    else snprintf(out->realization_id, sizeof(out->realization_id), "none");
    visor_format_id(&e->subject_id, out->subject_id);
    if (id_is_zero(&e->real.semantic_id))
        snprintf(out->realized_id, sizeof(out->realized_id), "none");
    else visor_format_id(&e->real.semantic_id, out->realized_id);
    if (e->has_machine_id) visor_format_id(&e->machine_id, out->machine_id);
    else snprintf(out->machine_id, sizeof(out->machine_id), "none");
    if (e->real.has_machine_id) visor_format_id(&e->real.machine_id, out->bound_machine_id);
    else snprintf(out->bound_machine_id, sizeof(out->bound_machine_id), "none");
    snprintf(out->machine_name, sizeof(out->machine_name), "%s", mg ? mg->name : "none");
    snprintf(out->target, sizeof(out->target), "%s", e->target_name);
    out->profile = e->real.target_profile;
    out->code_len = (uint32_t)e->real.code_len;
    out->insn_count = (uint32_t)(e->real.code_len / 4);
    out->disasm_count = visor_disasm(e->real.code_bytes, e->real.code_len, out->disasm, VISOR_DISASM_MAX);

    bool profile_ok = mg ? (e->real.target_profile == mg->target_profile) : false;
    const char *bind = "realizer bound no machine";
    bool bind_ok = true;
    if (e->real.has_machine_id) {
        bind_ok = mg && id_eq(&e->real.machine_id, &mg->machine_id);
        bind = bind_ok ? "bound machine == model" : "bound machine != model";
    }
    bool not_bw = strcmp(e->target_name, VISOR_TARGET_BLACKWELL) != 0;
    bool verdict_ok = !e->verdict_known || e->compatible;
    out->compatible = profile_ok && bind_ok && not_bw && verdict_ok;
    char runwhy[160];
    out->runnable = verdict_ok && run_check(e, runwhy, sizeof(runwhy)) == 0;
    char vw[192] = "";
    if (!verdict_ok)
        snprintf(vw, sizeof(vw), "; realizer verdict INCOMPATIBLE: %.127s", e->incompatible_reason);
    if (mg)
        snprintf(out->why, sizeof(out->why), "profile 0x%02x vs machine 0x%02x %s; %s; run %s%s",
                 e->real.target_profile, mg->target_profile, profile_ok ? "match" : "MISMATCH",
                 bind, out->runnable ? "allowed" : "refused", vw);
    else
        snprintf(out->why, sizeof(out->why), "profile 0x%02x; no machine model (not compatible); %s; run %s%s",
                 e->real.target_profile, bind, out->runnable ? "allowed" : "refused", vw);
    return visor_realization_cost(e, mg, &out->cost);
}

/* ---- realize ------------------------------------------------------------- */

static void entry_base(VisorRealizationEntry *e, const OmegaMachineGraph *mg) {
    if (mg) { e->machine_id = mg->machine_id; e->has_machine_id = true; }
    if (mg && e->real.code_len > 0) {
        e->estimated_cycles = omega_machine_estimate_latency(mg, &e->real);
        e->has_estimate = true;
    }
}

static int fail_view(VisorRealizationView *v, const char *why) {
    if (v) {
        memset(v, 0, sizeof(*v));
        snprintf(v->why, sizeof(v->why), "%s", why);
        snprintf(v->realization_id, sizeof(v->realization_id), "none");
        snprintf(v->subject_id, sizeof(v->subject_id), "none");
        snprintf(v->realized_id, sizeof(v->realized_id), "none");
        snprintf(v->machine_id, sizeof(v->machine_id), "none");
        snprintf(v->bound_machine_id, sizeof(v->bound_machine_id), "none");
        snprintf(v->machine_name, sizeof(v->machine_name), "none");
        snprintf(v->target, sizeof(v->target), "none");
        VisorCostItem *it[4] = { &v->cost.predicted, &v->cost.estimated, &v->cost.measured, &v->cost.qualified };
        for (int i = 0; i < 4; ++i) {
            cost_slot(it[i], (VisorCostClass)i);
            snprintf(it[i]->source, sizeof(it[i]->source), "absent: not realized");
        }
    }
    return -2;
}

int visor_realize_apply(const OmegaGraph *g, const SemanticId *apply_id, const OmegaMachineGraph *mg,
                        VisorRealizationEntry *out_entry, VisorRealizationView *out_view) {
    if (!g || !apply_id || !out_entry || !out_view) return -1;
    memset(out_entry, 0, sizeof(*out_entry));
    char why[256];
    const OmegaObject *obj = omega_graph_find_object_const(g, apply_id);
    if (!obj) return fail_view(out_view, "unknown id: not in graph");
    if (obj->kind != KIND_OPERATION)
        return fail_view(out_view, "not an APPLY or OPERATION object; only pure binary u64 applies realize in V1");

    SemanticId op_id;
    if (obj->payload_len == sizeof(ApplyPayload)) {
        ApplyPayload ap;
        memcpy(&ap, obj->payload, sizeof(ap));
        if (ap.operand_count != 2) {
            snprintf(why, sizeof(why), "apply has %u operands; pure binary realizer needs 2", ap.operand_count);
            return fail_view(out_view, why);
        }
        op_id = ap.op_id;
    } else if (obj->payload_len >= sizeof(OperationPayload)) {
        op_id = *apply_id;   /* an OPERATION itself */
    } else {
        return fail_view(out_view, "OPERATION-kind object with unrecognised payload size");
    }

    const OmegaObject *op = omega_graph_find_object_const(g, &op_id);
    if (!op || op->kind != KIND_OPERATION || op->payload_len < sizeof(OperationPayload))
        return fail_view(out_view, "apply references an operation that is not in the graph");
    OperationPayload opp;
    memcpy(&opp, op->payload, sizeof(opp));
    switch (opp.opcode) {
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_AND: case OP_OR: break;
        default:
            snprintf(why, sizeof(why), "opcode %s not supported by omega_realize_pure_binary (ADD SUB MUL AND OR only)",
                     opcode_name(opp.opcode));
            return fail_view(out_view, why);
    }
    const OmegaObject *ty = omega_graph_find_object_const(g, &opp.type_id);
    if (!ty || ty->kind != KIND_TYPE || ty->payload_len < sizeof(TypePayload))
        return fail_view(out_view, "operation type not found in graph");
    TypePayload tp;
    memcpy(&tp, ty->payload, sizeof(tp));
    if (tp.tag != TYPE_UNSIGNED_INT || tp.width != 64) {
        snprintf(why, sizeof(why), "type tag 0x%02x width %u unsupported: realizer emits 64-bit unsigned code only",
                 (unsigned)tp.tag, tp.width);
        return fail_view(out_view, why);
    }
    if ((opp.opcode == OP_ADD || opp.opcode == OP_SUB || opp.opcode == OP_MUL) &&
        opp.overflow != OVERFLOW_WRAP && opp.overflow != OVERFLOW_DEFAULT) {
        snprintf(why, sizeof(why), "overflow policy 0x%02x unsupported: realized code wraps mod 2^64",
                 (unsigned)opp.overflow);
        return fail_view(out_view, why);
    }

    if (omega_realize_pure_binary(g, &op_id, &out_entry->real) != 0)
        return fail_view(out_view, "omega_realize_pure_binary failed");
    out_entry->subject_id = *apply_id;
    out_entry->subject_is_program = false;
    out_entry->program_index = -1;
    snprintf(out_entry->target_name, sizeof(out_entry->target_name), "%s", VISOR_TARGET_AARCH64);
    out_entry->verdict_known = true;
    out_entry->compatible = true;
    entry_base(out_entry, mg);
    return visor_realization_view(out_entry, mg, out_view);
}

int visor_realize_program(const OmegaProgram *p, const OmegaMachineGraph *mg,
                          VisorRealizationEntry *out_entry, VisorRealizationView *out_view) {
    if (!p || !out_entry || !out_view) return -1;
    memset(out_entry, 0, sizeof(*out_entry));
    /* Always compile from the canonical body (omega_program_realize); never trust code the
     * program happens to carry. The V0 gate's reason is shown when it refuses. */
    OmegaProgram c = *p;
    c.graph = NULL;
    char why[192];
    if (omega_program_realize_ex(&c, why, sizeof why) != 0) {
        char msg[240];
        snprintf(msg, sizeof msg, "omega_program_realize refused: %s", why[0] ? why : "unknown");
        return fail_view(out_view, msg);
    }
    out_entry->real = c.realization;
    out_entry->subject_id = p->program_id;
    out_entry->subject_is_program = true;
    out_entry->program_index = -1;
    snprintf(out_entry->target_name, sizeof(out_entry->target_name), "%s", VISOR_TARGET_AARCH64);
    out_entry->verdict_known = true;
    out_entry->compatible = true;
    entry_base(out_entry, mg);
    return visor_realization_view(out_entry, mg, out_view);
}

/* ---- alternatives -------------------------------------------------------- */

static const uint64_t k_diff_inputs[] = { 0, 1, 2, 5, 10, 50, 100, 0xFFFFFFFFULL };

static void set_verdict(VisorRealizationEntry *e, bool compatible, const char *reason) {
    e->verdict_known = true;
    e->compatible = compatible;
    snprintf(e->incompatible_reason, sizeof(e->incompatible_reason), "%s", compatible ? "" : reason);
}

static void synth_alt(const OmegaProgram *p, const OmegaMachineGraph *m, const char *tname,
                      VisorRealizationEntry *e, VisorRealizationView *v) {
    memset(e, 0, sizeof(*e));
    e->subject_id = p->program_id;
    e->subject_is_program = true;
    e->program_index = -1;
    snprintf(e->target_name, sizeof(e->target_name), "%s", tname);
#if defined(__aarch64__)
    /* Program-driven realization for machine m: program body -> schedule chosen from m -> bytes ->
     * triple id -> verification (native == semantic evaluator). */
    RealizationSynthesisTask task;
    RealizationSynthesisResult res;
    omega_realize_task_init(&task, p, m);
    task.optimize_latency = true;
    int rc = omega_synthesize_realization(&task, &res);
    if (res.realization.code_len == 0) {
        char r[sizeof(e->incompatible_reason)];
        snprintf(r, sizeof(r), "realization refused (rc=%d): %.96s", rc, res.why);
        set_verdict(e, false, r);
        visor_realization_view(e, m, v);
        v->compatible = false;
        v->runnable = false;
        snprintf(v->why, sizeof(v->why), "omega_synthesize_realization refused (rc=%d): %.160s", rc, res.why);
        return;
    }
    e->real = res.realization;
    e->machine_id = m->machine_id;
    e->has_machine_id = true;
    e->estimated_cycles = res.estimated_cycles;
    e->has_estimate = true;
    /* cross-check vs the canonical (direct) realization of the same program */
    char diff[160];
    OmegaProgram c = *p;
    c.graph = NULL;
    bool match = omega_program_realize(&c) == 0;
    snprintf(diff, sizeof(diff), "program has no direct realization to compare");
    if (match) {
        snprintf(diff, sizeof(diff), "matches direct on %zu inputs",
                 sizeof(k_diff_inputs) / sizeof(k_diff_inputs[0]));
        for (size_t i = 0; i < sizeof(k_diff_inputs) / sizeof(k_diff_inputs[0]); ++i) {
            uint64_t yd = 0, ys = 0, x = k_diff_inputs[i];
            if (omega_exec_native_f3(&c.realization, x, 0, 0, &yd) != 0 ||
                omega_exec_native_f3(&e->real, x, 0, 0, &ys) != 0 || yd != ys) {
                match = false;
                snprintf(diff, sizeof(diff), "MISMATCH x=%" PRIu64 ": direct=%" PRIu64 " synth=%" PRIu64, x, yd, ys);
                break;
            }
        }
    }
    if (rc != 0 || !res.solved) match = false;
    if (match) {
        set_verdict(e, true, "");
    } else {
        char r[sizeof(e->incompatible_reason)];
        snprintf(r, sizeof(r), "synth rc=%d solved=%d; %.40s; %.60s", rc, res.solved ? 1 : 0, diff, res.why);
        set_verdict(e, false, r);
    }
    visor_realization_view(e, m, v);
    v->compatible = v->compatible && match;
    v->runnable = v->runnable && match;
    snprintf(v->why, sizeof(v->why),
             "compiled from the program body, %s schedule for this machine; native == semantic evaluator on %u inputs; %.60s; synth rc=%d",
             res.schedule == OMEGA_SCHED_PRELOAD ? "preload" : "sequential", res.inputs_checked, diff, rc);
#else
    set_verdict(e, false, "synthesis not attempted: host is not aarch64");
    visor_realization_view(e, m, v);
    v->compatible = false;
    v->runnable = false;
    snprintf(v->why, sizeof(v->why), "synthesis not attempted: its V1 check executes AArch64 code; host is not aarch64");
#endif
}

int visor_realization_alternatives_ex(const OmegaProgram *p, const OmegaMachineGraph *mg,
                                      VisorRealizationEntry *entries, VisorRealizationView *out,
                                      size_t max, size_t *count) {
    if (!p || !mg || !out || !count) return -1;
    *count = 0;
    static OmegaMachineGraph other;   /* fixed storage; single-threaded Visor */
    OmegaMachineGraph spark;
    if (omega_machine_build_dgx_spark(&spark) != 0) return -1;
    if (id_eq(&spark.machine_id, &mg->machine_id)) {
        if (omega_machine_build_qemu_virt(&other) != 0) return -1;
    } else {
        other = spark;
    }
    const OmegaMachineGraph *ms[2] = { mg, &other };
    char tn[2][32];
    machine_target_name(mg, tn[0]);
    machine_target_name(&other, tn[1]);

    VisorRealizationEntry tmp;
    for (size_t k = 0; k < 4 && *count < max; ++k) {
        VisorRealizationEntry *e = entries ? &entries[*count] : &tmp;
        VisorRealizationView *v = &out[*count];
        const OmegaMachineGraph *m = ms[k % 2];
        const char *t = tn[k % 2];
        if (k < 2) {
            if (visor_realize_program(p, m, e, v) != 0) {
                set_verdict(e, false, "direct realization failed");
                v->compatible = false;
                v->runnable = false;
            }
            snprintf(e->target_name, sizeof(e->target_name), "%s", VISOR_TARGET_AARCH64);
            snprintf(v->label, sizeof(v->label), "direct@%s", t);
        } else {
            synth_alt(p, m, t, e, v);
            snprintf(v->label, sizeof(v->label), "synth@%s", t);
        }
        (*count)++;
    }
    return 0;
}

int visor_realization_alternatives(const OmegaProgram *p, const OmegaMachineGraph *mg,
                                   VisorRealizationView *out, size_t max, size_t *count) {
    return visor_realization_alternatives_ex(p, mg, NULL, out, max, count);
}

/* ---- text ---------------------------------------------------------------- */

static void cost_item_text(Out *o, const VisorCostItem *it) {
    if (it->present)
        outf(o, "  %-9s cycles=%" PRIu64 " insns=%u bytes=%u  [%s]\n", it->cls_name, it->cycles,
             it->insn_count, it->code_bytes, it->source);
    else
        outf(o, "  %-9s ABSENT  [%s]\n", it->cls_name, it->source);
}

int visor_cost_format_text(const VisorCostView *c, char *out, size_t n) {
    if (!c || !out || n == 0) return -1;
    Out o = { out, n, 0, false };
    out[0] = '\0';
    outf(&o, "cost (four separate classes; never merged)\n");
    cost_item_text(&o, &c->predicted);
    cost_item_text(&o, &c->estimated);
    cost_item_text(&o, &c->measured);
    cost_item_text(&o, &c->qualified);
    return o.trunc ? -1 : (int)o.pos;
}

static void cost_item_json(Out *o, const char *key, const VisorCostItem *it) {
    outf(o, "\"%s\":{\"present\":%s", key, it->present ? "true" : "false");
    if (it->present)
        outf(o, ",\"cycles\":%" PRIu64 ",\"insn_count\":%u,\"code_bytes\":%u", it->cycles, it->insn_count,
             it->code_bytes);
    outf(o, ",\"source\":");
    json_str(o, it->source);
    outf(o, "}");
}

int visor_cost_format_json(const VisorCostView *c, char *out, size_t n) {
    if (!c || !out || n == 0) return -1;
    Out o = { out, n, 0, false };
    out[0] = '\0';
    outf(&o, "{");
    cost_item_json(&o, "predicted", &c->predicted); outf(&o, ",");
    cost_item_json(&o, "estimated", &c->estimated); outf(&o, ",");
    cost_item_json(&o, "measured", &c->measured); outf(&o, ",");
    cost_item_json(&o, "qualified", &c->qualified);
    outf(&o, "}");
    return o.trunc ? -1 : (int)o.pos;
}

int visor_realization_why(const VisorRealizationView *v, char *out, size_t n) {
    if (!v || !out || n == 0) return -1;
    Out o = { out, n, 0, false };
    out[0] = '\0';
    outf(&o, "why %s%s\n", v->label[0] ? v->label : "realization", v->label[0] ? "" : "");
    outf(&o, "  compatibility: %s\n", v->compatible ? "compatible" : "NOT compatible");
    outf(&o, "    profile: realization 0x%02x, machine %s; V1 realizers and both canonical profiles use 0x01,"
             " so a match is expected, not a hardware check\n", v->profile, v->machine_name);
    outf(&o, "    machine binding: realizer bound %s; model %s\n", v->bound_machine_id, v->machine_id);
    outf(&o, "    target: %s; run %s\n", v->target, v->runnable ? "allowed (pure AArch64, native)" : "refused");
    outf(&o, "  constraints: %s\n", v->why);
    outf(&o, "  cost sources:\n");
    const VisorCostItem *it[4] = { &v->cost.predicted, &v->cost.estimated, &v->cost.measured, &v->cost.qualified };
    for (size_t i = 0; i < 4; ++i)
        outf(&o, "    %-9s %s: %s\n", it[i]->cls_name, it[i]->present ? "present" : "ABSENT", it[i]->source);
    return o.trunc ? -1 : (int)o.pos;
}

int visor_realization_format_text(const VisorRealizationView *v, char *out, size_t n) {
    if (!v || !out || n == 0) return -1;
    Out o = { out, n, 0, false };
    out[0] = '\0';
    if (v->label[0]) outf(&o, "realization %s\n", v->label);
    else outf(&o, "realization\n");
    outf(&o, "  id           %s\n", v->realization_id);
    outf(&o, "  subject      %s\n", v->subject_id);
    outf(&o, "  realized     %s\n", v->realized_id);
    outf(&o, "  machine      %s (%s)\n", v->machine_id, v->machine_name);
    outf(&o, "  bound        %s\n", v->bound_machine_id);
    outf(&o, "  target       %s profile=0x%02x\n", v->target, v->profile);
    outf(&o, "  code         %u bytes, %u instructions\n", v->code_len, v->insn_count);
    for (size_t i = 0; i < v->disasm_count; ++i) outf(&o, "    %s\n", v->disasm[i]);
    outf(&o, "  compatible   %s\n", v->compatible ? "yes" : "no");
    outf(&o, "  runnable     %s\n", v->runnable ? "yes" : "no");
    outf(&o, "  why          %s\n", v->why);
    if (!o.trunc) {
        int w = visor_cost_format_text(&v->cost, out + o.pos, n - o.pos);
        if (w < 0) o.trunc = true; else o.pos += (size_t)w;
    }
    return o.trunc ? -1 : (int)o.pos;
}

int visor_realization_format_json(const VisorRealizationView *v, char *out, size_t n) {
    if (!v || !out || n == 0) return -1;
    Out o = { out, n, 0, false };
    out[0] = '\0';
    outf(&o, "{\"label\":"); json_str(&o, v->label);
    outf(&o, ",\"realization_id\":"); json_str(&o, v->realization_id);
    outf(&o, ",\"subject_id\":"); json_str(&o, v->subject_id);
    outf(&o, ",\"realized_id\":"); json_str(&o, v->realized_id);
    outf(&o, ",\"machine_id\":"); json_str(&o, v->machine_id);
    outf(&o, ",\"machine_name\":"); json_str(&o, v->machine_name);
    outf(&o, ",\"bound_machine_id\":"); json_str(&o, v->bound_machine_id);
    outf(&o, ",\"target\":"); json_str(&o, v->target);
    outf(&o, ",\"profile\":%u,\"code_len\":%u,\"insn_count\":%u,\"disasm\":[", v->profile, v->code_len, v->insn_count);
    for (size_t i = 0; i < v->disasm_count; ++i) { if (i) outf(&o, ","); json_str(&o, v->disasm[i]); }
    outf(&o, "],\"compatible\":%s,\"runnable\":%s,\"why\":", v->compatible ? "true" : "false",
         v->runnable ? "true" : "false");
    json_str(&o, v->why);
    outf(&o, ",\"cost\":");
    if (!o.trunc) {
        int w = visor_cost_format_json(&v->cost, out + o.pos, n - o.pos);
        if (w < 0) o.trunc = true; else o.pos += (size_t)w;
    }
    outf(&o, "}");
    return o.trunc ? -1 : (int)o.pos;
}

static void cmp_cost(Out *o, const char *name, const VisorCostItem *a, const VisorCostItem *b) {
    if (a->present && b->present)
        outf(o, "  %-9s A=%" PRIu64 " B=%" PRIu64 " (%s)  [A: %s] [B: %s]\n", name, a->cycles, b->cycles,
             a->cycles == b->cycles ? "equal" : (a->cycles < b->cycles ? "A lower" : "B lower"), a->source, b->source);
    else
        outf(o, "  %-9s A=%s B=%s  (not compared)\n", name, a->present ? "present" : "ABSENT",
             b->present ? "present" : "ABSENT");
}

int visor_realization_compare(const VisorRealizationView *a, const VisorRealizationView *b, char *out, size_t n) {
    if (!a || !b || !out || n == 0) return -1;
    Out o = { out, n, 0, false };
    out[0] = '\0';
    outf(&o, "compare A=%s B=%s\n", a->label[0] ? a->label : a->realization_id,
         b->label[0] ? b->label : b->realization_id);
    outf(&o, "  realization_id %s\n", strcmp(a->realization_id, b->realization_id) == 0 ? "same" : "different");
    bool same_code = a->code_len == b->code_len && a->disasm_count == b->disasm_count;
    for (size_t i = 0; same_code && i < a->disasm_count; ++i)
        if (strcmp(a->disasm[i], b->disasm[i]) != 0) same_code = false;
    outf(&o, "  code           %s (A %u insns, B %u insns)\n", same_code ? "identical" : "different",
         a->insn_count, b->insn_count);
    outf(&o, "  profile        A=0x%02x (%s on %s) B=0x%02x (%s on %s)\n", a->profile, a->target, a->machine_name,
         b->profile, b->target, b->machine_name);
    outf(&o, "  compatible     A=%s B=%s; runnable A=%s B=%s\n", a->compatible ? "yes" : "no",
         b->compatible ? "yes" : "no", a->runnable ? "yes" : "no", b->runnable ? "yes" : "no");
    outf(&o, "  cost (per class; classes never mixed)\n");
    cmp_cost(&o, "predicted", &a->cost.predicted, &b->cost.predicted);
    cmp_cost(&o, "estimated", &a->cost.estimated, &b->cost.estimated);
    cmp_cost(&o, "measured", &a->cost.measured, &b->cost.measured);
    cmp_cost(&o, "qualified", &a->cost.qualified, &b->cost.qualified);
    return o.trunc ? -1 : (int)o.pos;
}
