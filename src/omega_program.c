#include "omega_program.h"
#include "omega_core.h"
#include "aarch64_encoder.h"
#include "aarch64_decoder.h"
#include "omega_canonical.h"
#include "omega_exec.h"
#include "sha256.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void omega_program_init(OmegaProgram *prog, const char *name) {
    if (!prog) return;
    memset(prog, 0, sizeof(OmegaProgram));
    if (name) {
        snprintf(prog->name, sizeof(prog->name), "%.60s", name);
    }
}

void omega_program_destroy(OmegaProgram *prog) {
    if (!prog) return;
    if (prog->graph) {
        omega_graph_destroy(prog->graph);
        prog->graph = NULL;
    }
    memset(prog, 0, sizeof(OmegaProgram));
}

int omega_build_constraint_id(ConstraintKind kind, const char *annotation, SemanticId *out_id) {
    if (!out_id) return -1;
    OmegaObject obj;
    memset(&obj, 0, sizeof(OmegaObject));
    obj.kind = KIND_CONSTRAINT;
    uint16_t len = annotation ? (uint16_t)strlen(annotation) : 0;
    omega_object_add_constraint(&obj, kind, (const uint8_t*)annotation, len);
    if (omega_compute_semantic_id(&obj) != 0) return -1;
    *out_id = obj.id;
    return 0;
}

/* ---- program identity v2 (spec/program-identity.md) ---------------------- */

static bool step_op_valid(uint8_t op) {
    return op == OP_ADD || op == OP_SUB || op == OP_MUL || op == OP_AND || op == OP_OR;
}

static OmegaObject *build_type_for(OmegaGraph *g, TypeTag tag, uint16_t width) {
    switch (tag) {
        case TYPE_UNSIGNED_INT: return omega_build_type_uint(g, width);
        case TYPE_SIGNED_INT:   return omega_build_type_signed_int(g, width);
        case TYPE_BITVECTOR:    return omega_build_type_bitvector(g, width);
        default:                return NULL;
    }
}

/* Lower the body into g with the existing builders; returns the root object. */
static const OmegaObject *lower_body(OmegaGraph *g, const OmegaProgram *prog) {
    const OmegaProgramBody *b = &prog->body;
    if (!b->has_body || b->step_count > OMEGA_PROGRAM_MAX_STEPS) return NULL;
    uint16_t w = prog->contract.input_width;
    OmegaObject *t = build_type_for(g, prog->contract.input_type, w);
    if (!t || !t->has_id) return NULL;
    SemanticId tid = t->id;
    OmegaObject *cur = omega_build_param(g, &tid, 0);
    if (!cur || !cur->has_id) return NULL;
    SemanticId cur_id = cur->id;
    for (uint16_t i = 0; i < b->step_count; ++i) {
        if (!step_op_valid(b->steps[i].op)) return NULL;
        OmegaObject *c = omega_build_val_uint(g, &tid, w, b->steps[i].imm);
        if (!c || !c->has_id) return NULL;
        SemanticId cid = c->id;
        OmegaObject *op = omega_build_op_binary(g, (OpCode)b->steps[i].op, OVERFLOW_WRAP, &tid);
        if (!op || !op->has_id) return NULL;
        SemanticId opid = op->id;
        cur = omega_build_apply(g, &opid, &cur_id, &cid);
        if (!cur || !cur->has_id) return NULL;
        cur_id = cur->id;
    }
    return cur;
}

int omega_program_body_root_id(const OmegaProgram *prog, SemanticId *out_root) {
    if (!prog || !out_root) return -1;
    OmegaGraph *g = omega_graph_create();
    if (!g) return -1;
    const OmegaObject *root = lower_body(g, prog);
    int rc = -1;
    if (root) { *out_root = root->id; rc = 0; }
    omega_graph_destroy(g);
    return rc;
}

static int type_id_for(TypeTag tag, uint16_t width, SemanticId *out) {
    OmegaGraph *g = omega_graph_create();
    if (!g) return -1;
    OmegaObject *t = build_type_for(g, tag, width);
    int rc = -1;
    if (t && t->has_id) { *out = t->id; rc = 0; }
    omega_graph_destroy(g);
    return rc;
}

int omega_program_compute_id(OmegaProgram *prog) {
    if (!prog) return -1;
    memset(prog->program_id.bytes, 0, OMEGA_ID_BYTES);
    SemanticId root, tin, tout;
    if (omega_program_body_root_id(prog, &root) != 0 ||
        type_id_for(prog->contract.input_type, prog->contract.input_width, &tin) != 0 ||
        type_id_for(prog->contract.output_type, prog->contract.output_width, &tout) != 0)
        return -1;

    static const char domain[] = OMEGA_PROGRAM_ID_DOMAIN;
    uint8_t buf[sizeof(domain) + 5 * OMEGA_ID_BYTES];
    size_t pos = 0;
    memcpy(buf, domain, sizeof(domain));   /* includes the terminating 0x00 */
    pos += sizeof(domain);
    memcpy(&buf[pos], root.bytes, OMEGA_ID_BYTES); pos += OMEGA_ID_BYTES;
    memcpy(&buf[pos], tin.bytes, OMEGA_ID_BYTES); pos += OMEGA_ID_BYTES;
    memcpy(&buf[pos], tout.bytes, OMEGA_ID_BYTES); pos += OMEGA_ID_BYTES;
    memcpy(&buf[pos], prog->contract.precondition_id.bytes, OMEGA_ID_BYTES); pos += OMEGA_ID_BYTES;
    memcpy(&buf[pos], prog->contract.postcondition_id.bytes, OMEGA_ID_BYTES); pos += OMEGA_ID_BYTES;
    sha256_hash(buf, pos, prog->program_id.bytes);
    return 0;
}

/* ---- V0 realization compiler core (spec/program-realization.md) ---------- */

static void set_why(char *why, size_t n, const char *fmt, const char *a) {
    if (why && n) snprintf(why, n, fmt, a ? a : "");
}

static const char *step_op_name(uint8_t op) {
    switch (op) {
        case OP_ADD: return "ADD"; case OP_SUB: return "SUB"; case OP_MUL: return "MUL";
        case OP_AND: return "AND"; case OP_OR: return "OR"; case OP_DIV: return "DIV";
        default: return "unknown";
    }
}

static uint64_t width_mask(uint16_t w) {
    return w >= 64 ? ~0ull : ((1ull << w) - 1ull);
}

int omega_program_realize_check(const OmegaProgram *prog, uint16_t *out_width, char *why, size_t why_len) {
    char tmp[160];
    if (!prog) { set_why(why, why_len, "no program%s", NULL); return -1; }
    const OmegaContract *c = &prog->contract;
    const OmegaProgramBody *b = &prog->body;
    if (!b->has_body) { set_why(why, why_len, "program has no semantic body: meaning unknown, nothing to compile%s", NULL); return -1; }
    if (b->step_count > OMEGA_PROGRAM_MAX_STEPS) { set_why(why, why_len, "body has more than 64 steps%s", NULL); return -1; }
    if (c->input_type != TYPE_UNSIGNED_INT || c->output_type != TYPE_UNSIGNED_INT) {
        snprintf(tmp, sizeof tmp, "type tags in=0x%02x out=0x%02x: V0 realization compiles unsigned integers only "
                 "(signed/bitvector result representation is not specified)", (unsigned)c->input_type, (unsigned)c->output_type);
        set_why(why, why_len, "%s", tmp);
        return -1;
    }
    if (c->input_width != c->output_width) {
        snprintf(tmp, sizeof tmp, "input width %u != output width %u: V0 never converts", c->input_width, c->output_width);
        set_why(why, why_len, "%s", tmp);
        return -1;
    }
    uint16_t w = c->input_width;
    if (w != 8 && w != 16 && w != 32 && w != 64) {
        snprintf(tmp, sizeof tmp, "width %u unsupported: V0 AArch64 realization supports u8 u16 u32 u64", w);
        set_why(why, why_len, "%s", tmp);
        return -1;
    }
    for (uint16_t i = 0; i < b->step_count; ++i) {
        uint8_t op = b->steps[i].op;
        if (!step_op_valid(op)) {
            if (op == OP_DIV)
                snprintf(tmp, sizeof tmp, "step %u: DIV fails closed: not in the V0 body op set, no UDIV emitter, "
                         "and evaluator (x/0 = error) and UDIV (x/0 = 0) disagree", i);
            else
                snprintf(tmp, sizeof tmp, "step %u: opcode 0x%02x (%s) not in V0 body op set ADD SUB MUL AND OR",
                         i, op, step_op_name(op));
            set_why(why, why_len, "%s", tmp);
            return -1;
        }
        if ((b->steps[i].imm & ~width_mask(w)) != 0) {
            snprintf(tmp, sizeof tmp, "step %u: constant 0x%llx does not fit declared width u%u", i,
                     (unsigned long long)b->steps[i].imm, w);
            set_why(why, why_len, "%s", tmp);
            return -1;
        }
    }
    /* The program id must be the id of this body + contract: never compile under a stale id. */
    OmegaProgram chk = *prog;
    chk.graph = NULL;
    if (omega_program_compute_id(&chk) != 0 ||
        omega_compare_semantic_id(&chk.program_id, &prog->program_id) != 0) {
        set_why(why, why_len, "program_id is not the v2 id of this body and contract (stale or forged)%s", NULL);
        return -1;
    }
    if (out_width) *out_width = w;
    if (why && why_len) why[0] = '\0';
    return 0;
}

/* Materialize a constant: MOVZ chunk0, then MOVK for each nonzero higher 16-bit chunk. */
static int emit_const(uint8_t *code, size_t *pos, size_t max_len, uint8_t reg, uint64_t imm) {
    if (aarch64_emit_movz(code, pos, max_len, true, reg, (uint16_t)(imm & 0xFFFF), 0) != 0) return -1;
    for (unsigned k = 1; k < 4; ++k) {
        uint16_t chunk = (uint16_t)((imm >> (16 * k)) & 0xFFFF);
        if (chunk && aarch64_emit_movk(code, pos, max_len, true, reg, chunk, (uint8_t)(16 * k)) != 0) return -1;
    }
    return 0;
}

static int emit_op(uint8_t *code, size_t *pos, size_t max_len, uint8_t op, uint8_t rm) {
    switch (op) {
        case OP_ADD: return aarch64_emit_add_reg(code, pos, max_len, true, REG_X0, REG_X0, rm);
        case OP_SUB: return aarch64_emit_sub_reg(code, pos, max_len, true, REG_X0, REG_X0, rm);
        case OP_MUL: return aarch64_emit_mul_reg(code, pos, max_len, true, REG_X0, REG_X0, rm);
        case OP_AND: return aarch64_emit_and_reg(code, pos, max_len, true, REG_X0, REG_X0, rm);
        case OP_OR:  return aarch64_emit_orr_reg(code, pos, max_len, true, REG_X0, REG_X0, rm);
        default: return -1;
    }
}

int omega_program_emit_schedule(const OmegaProgramBody *body, uint16_t width, OmegaRealizeSchedule sched,
                                uint8_t *code, size_t *code_len, size_t max_len) {
    if (!body || !code || !code_len || !body->has_body || body->step_count > OMEGA_PROGRAM_MAX_STEPS) return -1;
    if (width != 8 && width != 16 && width != 32 && width != 64) return -1;
    if (sched != OMEGA_SCHED_SEQUENTIAL && sched != OMEGA_SCHED_PRELOAD) return -1;
    /* Plan: the body steps, then (width < 64) one AND with the width mask. Arithmetic is
     * done in 64-bit registers; ADD SUB MUL AND OR commute with reduction mod 2^w, so one
     * final mask gives the wrapped u<w> result. */
    OmegaProgramStep plan[OMEGA_PROGRAM_MAX_STEPS + 1];
    size_t n = 0;
    for (uint16_t i = 0; i < body->step_count; ++i) {
        if (!step_op_valid(body->steps[i].op) || (body->steps[i].imm & ~width_mask(width)) != 0) return -1;
        plan[n++] = body->steps[i];
    }
    if (width < 64) { plan[n].op = OP_AND; plan[n].imm = width_mask(width); n++; }

    size_t pos = 0;
    if (sched == OMEGA_SCHED_SEQUENTIAL) {
        for (size_t i = 0; i < n; ++i)
            if (emit_const(code, &pos, max_len, REG_X1, plan[i].imm) != 0 ||
                emit_op(code, &pos, max_len, plan[i].op, REG_X1) != 0)
                return -1;
    } else {
        /* Preload: in blocks of up to OMEGA_PRELOAD_REGS constants, load all of the block's
         * constants into X1..Xk first (independent, can issue together), then apply the
         * block's ops in body order. Only caller-saved scratch registers X1..X15 are used. */
        for (size_t base = 0; base < n; base += OMEGA_PRELOAD_REGS) {
            size_t k = n - base < OMEGA_PRELOAD_REGS ? n - base : OMEGA_PRELOAD_REGS;
            for (size_t j = 0; j < k; ++j)
                if (emit_const(code, &pos, max_len, (uint8_t)(REG_X1 + j), plan[base + j].imm) != 0) return -1;
            for (size_t j = 0; j < k; ++j)
                if (emit_op(code, &pos, max_len, plan[base + j].op, (uint8_t)(REG_X1 + j)) != 0) return -1;
        }
    }
    if (aarch64_emit_ret(code, &pos, max_len) != 0) return -1;
    *code_len = pos;
    return 0;
}

int omega_program_emit_body(const OmegaProgramBody *body, uint8_t *code, size_t *code_len, size_t max_len) {
    return omega_program_emit_schedule(body, 64, OMEGA_SCHED_SEQUENTIAL, code, code_len, max_len);
}

static uint32_t read_insn(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int omega_program_lift_body(const uint8_t *code, size_t code_len, OmegaProgramBody *out_body) {
    if (!code || !out_body || code_len < 4 || code_len % 4 != 0 || code_len > AARCH64_MAX_CODE_BYTES) return -1;
    OmegaProgramBody b;
    memset(&b, 0, sizeof b);
    b.has_body = true;
    size_t n = code_len / 4, i = 0;
    while (i + 1 < n) {
        DecodedInsn d;
        if (aarch64_decode_instruction(read_insn(&code[i * 4]), &d) != 0 || d.op != DECODED_MOVZ ||
            d.rd != REG_X1 || d.hw != 0)
            return -1;
        uint64_t imm = d.imm16;
        i++;
        while (i < n && aarch64_decode_instruction(read_insn(&code[i * 4]), &d) == 0 && d.op == DECODED_MOVK) {
            if (d.rd != REG_X1 || d.hw == 0) return -1;
            imm |= (uint64_t)d.imm16 << (16 * d.hw);
            i++;
        }
        if (i >= n || aarch64_decode_instruction(read_insn(&code[i * 4]), &d) != 0) return -1;
        uint8_t op;
        switch (d.op) {
            case DECODED_ADD: op = OP_ADD; break;
            case DECODED_SUB: op = OP_SUB; break;
            case DECODED_MUL: op = OP_MUL; break;
            case DECODED_AND: op = OP_AND; break;
            case DECODED_ORR: op = OP_OR; break;
            default: return -1;
        }
        i++;
        if (b.step_count >= OMEGA_PROGRAM_MAX_STEPS) return -1;
        b.steps[b.step_count].op = op;
        b.steps[b.step_count].imm = imm;
        b.step_count++;
    }
    /* Confirm: the re-emitted template must be byte-identical (covers registers,
     * shifts, sf, chunk order, and the terminal RET that the loose decode above does not check). */
    uint8_t re[AARCH64_MAX_CODE_BYTES];
    size_t re_len = 0;
    if (omega_program_emit_body(&b, re, &re_len, sizeof re) != 0 || re_len != code_len ||
        memcmp(re, code, code_len) != 0)
        return -1;
    *out_body = b;
    return 0;
}

/* ---- independent semantic evaluator (over the canonical lowered graph) ------
 * Walks the canonical objects the program id binds (APPLY -> OPERATION payload
 * opcode/overflow/type, VALUE big-endian bytes, the PARAM), and applies the core
 * definition omega_eval_pure_binary_uint. It never looks at code or at the emitter. */

static int eval_node(const OmegaGraph *g, const SemanticId *id, uint64_t x, int depth, uint64_t *out) {
    if (depth > OMEGA_PROGRAM_MAX_STEPS + 2) return -1;
    const OmegaObject *o = omega_graph_find_object_const(g, id);
    if (!o) return -1;
    if (o->kind == KIND_VALUE) {
        if (o->payload_len != sizeof(ValuePayload)) return -1;
        ValuePayload vp;
        memcpy(&vp, o->payload, sizeof vp);
        for (uint16_t i = 0; i < o->attr_count; ++i)
            if (strcmp(o->attributes[i].key, "omega.param") == 0) {
                if (vp.byte_len != 0) return -1;
                *out = x;
                return 0;
            }
        if (vp.byte_len == 0 || vp.byte_len > 8) return -1;
        uint64_t v = 0;
        for (uint16_t i = 0; i < vp.byte_len; ++i) v = (v << 8) | vp.bytes[i];
        *out = v;
        return 0;
    }
    if (o->kind != KIND_OPERATION || o->payload_len != sizeof(ApplyPayload)) return -1;
    ApplyPayload ap;
    memcpy(&ap, o->payload, sizeof ap);
    if (ap.operand_count != 2) return -1;
    const OmegaObject *op = omega_graph_find_object_const(g, &ap.op_id);
    if (!op || op->kind != KIND_OPERATION || op->payload_len != sizeof(OperationPayload)) return -1;
    OperationPayload opp;
    memcpy(&opp, op->payload, sizeof opp);
    if (opp.overflow != OVERFLOW_WRAP) return -1;
    const OmegaObject *ty = omega_graph_find_object_const(g, &opp.type_id);
    if (!ty || ty->kind != KIND_TYPE || ty->payload_len != sizeof(TypePayload)) return -1;
    TypePayload tp;
    memcpy(&tp, ty->payload, sizeof tp);
    if (tp.tag != TYPE_UNSIGNED_INT || tp.width == 0 || tp.width > 64) return -1;
    uint64_t a, b;
    if (eval_node(g, &ap.operands[0], x, depth + 1, &a) != 0 ||
        eval_node(g, &ap.operands[1], x, depth + 1, &b) != 0)
        return -1;
    return omega_eval_pure_binary_uint(opp.opcode, opp.overflow, tp.width, a, b, out) == 0 ? 0 : -1;
}

int omega_program_eval(const OmegaProgram *prog, const uint64_t *xs, size_t n, uint64_t *ys) {
    if (!prog || (n && (!xs || !ys))) return -1;
    if (omega_program_realize_check(prog, NULL, NULL, 0) != 0) return -1;
    OmegaGraph *g = omega_graph_create();
    if (!g) return -1;
    const OmegaObject *root = lower_body(g, prog);
    int rc = root ? 0 : -1;
    SemanticId rid;
    if (root) rid = root->id;
    uint64_t mask = width_mask(prog->contract.input_width);
    for (size_t i = 0; rc == 0 && i < n; ++i) {
        if (xs[i] & ~mask) { rc = -1; break; }      /* outside the declared input domain */
        if (eval_node(g, &rid, xs[i], 0, &ys[i]) != 0) rc = -1;
    }
    omega_graph_destroy(g);
    return rc;
}

/* ---- omega_program_realize: canonical (machine-independent) realization ---- */

int omega_program_realize_ex(OmegaProgram *prog, char *why, size_t why_len) {
    if (!prog) return -1;
    uint16_t w = 0;
    if (omega_program_realize_check(prog, &w, why, why_len) != 0) return -2;
    RealizationObject r;
    memset(&r, 0, sizeof r);
    size_t len = 0;
    if (omega_program_emit_schedule(&prog->body, w, OMEGA_SCHED_SEQUENTIAL, r.code_bytes, &len,
                                    sizeof r.code_bytes) != 0) {
        set_why(why, why_len, "emitter refused the body (code buffer or encoding)%s", NULL);
        return -2;
    }
    r.code_len = len;
    r.target_profile = AARCH64_PROFILE_V8A_BAREMETAL;
    r.entry_offset = 0;
    r.semantic_id = prog->program_id;     /* the realization id binds the program */
    omega_compute_realization_id(&r);
    VerifyReport rep;
    memset(&rep, 0, sizeof rep);
    if (omega_verify_v0_structural(NULL, &r, &rep) != 0) {
        set_why(why, why_len, "%s", rep.error_detail);
        return -1;
    }
    prog->realization = r;
    prog->is_realized = true;
    prog->is_verified = false;            /* realization is not verification */
    return 0;
}

int omega_program_realize(OmegaProgram *prog) {
    return omega_program_realize_ex(prog, NULL, 0);
}

int omega_program_validate_contract(const OmegaProgram *prog, char *err_msg, size_t err_msg_len) {
    if (!prog) return -1;
    if (prog->contract.input_type == TYPE_INVALID) {
        if (err_msg) snprintf(err_msg, err_msg_len, "Program contract has invalid input type");
        return -1;
    }
    if (prog->contract.output_type == TYPE_INVALID) {
        if (err_msg) snprintf(err_msg, err_msg_len, "Program contract has invalid output type");
        return -1;
    }
    if (prog->contract.input_width == 0 || prog->contract.output_width == 0) {
        if (err_msg) snprintf(err_msg, err_msg_len, "Program contract has 0-width type");
        return -1;
    }
    bool non_zero_pre = false, non_zero_post = false;
    for (int i = 0; i < OMEGA_ID_BYTES; ++i) {
        if (prog->contract.precondition_id.bytes[i]) non_zero_pre = true;
        if (prog->contract.postcondition_id.bytes[i]) non_zero_post = true;
    }
    if (!non_zero_pre || !non_zero_post) {
        if (err_msg) snprintf(err_msg, err_msg_len, "Program contract has uninitialized constraint SemanticId");
        return -1;
    }
    return 0;
}

int omega_program_build_unary_op(OmegaProgram *prog, const char *name, OpCode op, uint64_t imm) {
    if (!prog) return -1;
    omega_program_init(prog, name);
    if (!step_op_valid((uint8_t)op)) return -1;
    /* The realization loads the constant with MOVZ + one MOVK (32 bits). A wider
     * constant would be truncated in the code while the body kept the full value:
     * refuse it (spec/program-identity.md section 4). */
    if (imm > 0xFFFFFFFFull) return -1;

    prog->contract.input_type = TYPE_UNSIGNED_INT;
    prog->contract.input_width = 64;
    prog->contract.output_type = TYPE_UNSIGNED_INT;
    prog->contract.output_width = 64;
    snprintf(prog->contract.precondition, sizeof(prog->contract.precondition), "x >= 0");
    omega_build_constraint_id(CONST_PRECONDITION, prog->contract.precondition, &prog->contract.precondition_id);

    const char *sym = "?";
    switch (op) {
        case OP_ADD: sym = "+"; break;
        case OP_SUB: sym = "-"; break;
        case OP_MUL: sym = "*"; break;
        case OP_AND: sym = "&"; break;
        case OP_OR:  sym = "|"; break;
        default: return -1;
    }
    snprintf(prog->contract.postcondition, sizeof(prog->contract.postcondition), "x %s %lu", sym, (unsigned long)imm);
    omega_build_constraint_id(CONST_POSTCONDITION, prog->contract.postcondition, &prog->contract.postcondition_id);

    /* Semantic body: one step. The realization is compiled from it (omega_program_realize). */
    prog->body.has_body = true;
    prog->body.step_count = 1;
    prog->body.steps[0].op = (uint8_t)op;
    prog->body.steps[0].imm = imm;

    if (omega_program_compute_id(prog) != 0) return -1;
    if (omega_program_realize(prog) != 0) return -1;

    prog->cost.insn_count = (uint32_t)(prog->realization.code_len / 4);
    prog->cost.reg_pressure = 2;
    prog->cost.memory_bytes = 0;
    prog->cost.latency_cycles = (op == OP_MUL) ? 3 : 2;

    return 0;
}

/* ---- canonical composed postcondition (spec/program-identity.md 2.4) ------ */

int omega_contract_post_seq_id(const SemanticId *leaves, uint16_t n, SemanticId *out) {
    if (!leaves || !out || n == 0 || n > OMEGA_PROGRAM_MAX_STEPS) return -1;
    OmegaObject *obj = (OmegaObject *)malloc(sizeof(OmegaObject));
    if (!obj) return -1;
    SemanticId acc = leaves[n - 1];
    int rc = 0;
    for (int i = (int)n - 2; i >= 0 && rc == 0; --i) {
        memset(obj, 0, sizeof(OmegaObject));
        obj->kind = KIND_CONSTRAINT;
        uint8_t pl[2 * OMEGA_ID_BYTES];
        memcpy(pl, leaves[i].bytes, OMEGA_ID_BYTES);
        memcpy(pl + OMEGA_ID_BYTES, acc.bytes, OMEGA_ID_BYTES);
        if (omega_object_add_attribute(obj, "omega.compose", (const uint8_t *)"seq", 3) != 0 ||
            omega_object_add_constraint(obj, CONST_POSTCONDITION, pl, sizeof pl) != 0 ||
            omega_compute_semantic_id(obj) != 0)
            rc = -1;
        else
            acc = obj->id;
    }
    free(obj);
    if (rc == 0) *out = acc;
    return rc;
}

/* The leaf list of a contract's postcondition. A contract whose stored leaves no longer
 * fold to its postcondition_id (someone replaced the postcondition afterwards) is a leaf. */
static int post_leaves_of(const OmegaContract *c, SemanticId *out, uint16_t *n) {
    if (c->post_leaf_count >= 2 && c->post_leaf_count <= OMEGA_PROGRAM_MAX_STEPS) {
        SemanticId f;
        if (omega_contract_post_seq_id(c->post_leaves, c->post_leaf_count, &f) == 0 &&
            omega_compare_semantic_id(&f, &c->postcondition_id) == 0) {
            memcpy(out, c->post_leaves, c->post_leaf_count * sizeof(SemanticId));
            *n = c->post_leaf_count;
            return 0;
        }
    }
    out[0] = c->postcondition_id;
    *n = 1;
    return 0;
}

int omega_program_compose(const OmegaProgram *a, const OmegaProgram *b, OmegaProgram *out_c,
                          char *err_msg, size_t err_msg_len) {
    if (!a || !b || !out_c) return -1;

    /* 1. Intermediate Type Unification: OutType(A) must match InType(B) */
    if (a->contract.output_type != b->contract.input_type ||
        a->contract.output_width != b->contract.input_width) {
        if (err_msg) {
            snprintf(err_msg, err_msg_len,
                     "Type mismatch in composition: A.out (tag 0x%02x, width %u) != B.in (tag 0x%02x, width %u)",
                     a->contract.output_type, a->contract.output_width,
                     b->contract.input_type, b->contract.input_width);
        }
        return -1;
    }

    char c_name[64];
    snprintf(c_name, sizeof(c_name), "(%.25s)o(%.25s)", b->name, a->name);
    omega_program_init(out_c, c_name);

    /* 2. Contract Derivation */
    out_c->contract.input_type = a->contract.input_type;
    out_c->contract.input_width = a->contract.input_width;
    out_c->contract.output_type = b->contract.output_type;
    out_c->contract.output_width = b->contract.output_width;
    out_c->contract.precondition_id = a->contract.precondition_id;
    snprintf(out_c->contract.precondition, sizeof(out_c->contract.precondition), "%.60s", a->contract.precondition);
    /* Postcondition: the flattened ordered list of leaf postconditions (A's then B's), so
     * (a;b);c and a;(b;c) get one canonical id. The text is display only. */
    {
        SemanticId la[OMEGA_PROGRAM_MAX_STEPS], lb[OMEGA_PROGRAM_MAX_STEPS];
        uint16_t na = 0, nb = 0;
        post_leaves_of(&a->contract, la, &na);
        post_leaves_of(&b->contract, lb, &nb);
        if ((size_t)na + nb > OMEGA_PROGRAM_MAX_STEPS) {
            if (err_msg) snprintf(err_msg, err_msg_len, "Composite postcondition overflow (more than %d parts)", OMEGA_PROGRAM_MAX_STEPS);
            return -1;
        }
        memcpy(out_c->contract.post_leaves, la, na * sizeof(SemanticId));
        memcpy(out_c->contract.post_leaves + na, lb, nb * sizeof(SemanticId));
        out_c->contract.post_leaf_count = (uint16_t)(na + nb);
        if (omega_contract_post_seq_id(out_c->contract.post_leaves, out_c->contract.post_leaf_count,
                                       &out_c->contract.postcondition_id) != 0) {
            if (err_msg) snprintf(err_msg, err_msg_len, "Composite postcondition id failed");
            return -1;
        }
        snprintf(out_c->contract.postcondition, sizeof(out_c->contract.postcondition),
                 "%.29s ; %.29s", a->contract.postcondition, b->contract.postcondition);
    }

    /* 2b. Body: A's steps then B's (innermost first). Without both bodies the
     * composite's meaning is unknown and it has no identity. */
    if (a->body.has_body && b->body.has_body) {
        if ((size_t)a->body.step_count + b->body.step_count > OMEGA_PROGRAM_MAX_STEPS) {
            if (err_msg) snprintf(err_msg, err_msg_len, "Composite body overflow (more than %d steps)", OMEGA_PROGRAM_MAX_STEPS);
            return -1;
        }
        out_c->body.has_body = true;
        out_c->body.step_count = (uint16_t)(a->body.step_count + b->body.step_count);
        memcpy(out_c->body.steps, a->body.steps, a->body.step_count * sizeof(OmegaProgramStep));
        memcpy(out_c->body.steps + a->body.step_count, b->body.steps, b->body.step_count * sizeof(OmegaProgramStep));
    }

    /* 3. Cost Derivation: Monotonic cost accumulation */
    out_c->cost.insn_count = a->cost.insn_count + b->cost.insn_count - 1; /* Ret eliminated */
    out_c->cost.reg_pressure = (a->cost.reg_pressure > b->cost.reg_pressure) ? a->cost.reg_pressure : b->cost.reg_pressure;
    out_c->cost.memory_bytes = a->cost.memory_bytes + b->cost.memory_bytes;
    out_c->cost.latency_cycles = a->cost.latency_cycles + b->cost.latency_cycles;

    /* 4. Realization: compiled from the composite body (omega_program_realize), never
     * spliced from the parts' code. For template parts this is byte-identical to the old
     * RET-strip-and-concatenate fusion. A body-less composite has no meaning and no
     * realization. A body the V0 compiler refuses leaves the composite unrealized. */
    omega_program_compute_id(out_c);
    if (out_c->body.has_body) (void)omega_program_realize(out_c);
    return 0;
}

int omega_program_exec(const OmegaProgram *prog, uint64_t in_val, uint64_t *out_val) {
    if (!prog || !out_val || !prog->is_realized || prog->realization.code_len == 0) return -1;
    return omega_exec_native_f3(&prog->realization, in_val, 0, 0, out_val);
}

int omega_program_verify(OmegaProgram *prog, VerifyReport *report) {
    if (!prog || !report || !prog->is_realized) return -1;

    /* V0: Structural check */
    if (omega_verify_v0_structural(NULL, &prog->realization, report) != 0) {
        return -1;
    }

    /* V2: Invariant check */
    if (omega_verify_v2_properties(NULL, &prog->realization, report) != 0) {
        return -1;
    }

    prog->is_verified = true;
    return 0;
}

int omega_task_init(SynthesisTask *task, const char *desc,
                    TypeTag in_type, uint16_t in_width,
                    TypeTag out_type, uint16_t out_width,
                    const uint64_t *inputs, const uint64_t *outputs, size_t count) {
    if (!task || count > 16) return -1;
    memset(task, 0, sizeof(SynthesisTask));

    if (desc) snprintf(task->description, sizeof(task->description), "%.120s", desc);
    task->target_contract.input_type = in_type;
    task->target_contract.input_width = in_width;
    task->target_contract.output_type = out_type;
    task->target_contract.output_width = out_width;

    snprintf(task->target_contract.precondition, sizeof(task->target_contract.precondition), "x >= 0");
    omega_build_constraint_id(CONST_PRECONDITION, task->target_contract.precondition, &task->target_contract.precondition_id);
    snprintf(task->target_contract.postcondition, sizeof(task->target_contract.postcondition), "%.60s", desc ? desc : "task_target");
    omega_build_constraint_id(CONST_POSTCONDITION, task->target_contract.postcondition, &task->target_contract.postcondition_id);

    task->cost_budget.insn_count = 10;
    task->cost_budget.reg_pressure = 4;
    task->cost_budget.latency_cycles = 10;

    task->example_count = count;
    for (size_t i = 0; i < count; ++i) {
        task->inputs[i] = inputs[i];
        task->expected_outputs[i] = outputs[i];
    }

    /* Hash task ID */
    sha256_hash((const uint8_t*)task, sizeof(SynthesisTask) - OMEGA_ID_BYTES, task->task_id.bytes);
    return 0;
}

int omega_task_evaluate_candidate(const SynthesisTask *task, const OmegaProgram *candidate, bool *out_solved) {
    if (!task || !candidate || !out_solved) return -1;
    *out_solved = false;

    if (!candidate->is_realized) return 0;

    /* Check type conformance */
    if (candidate->contract.input_type != task->target_contract.input_type ||
        candidate->contract.output_type != task->target_contract.output_type) {
        return 0;
    }

    /* Check cost budget */
    if (candidate->cost.insn_count > task->cost_budget.insn_count) {
        return 0;
    }

    /* Evaluate all input-output examples */
    for (size_t i = 0; i < task->example_count; ++i) {
        uint64_t observed = 0;
        if (omega_program_exec(candidate, task->inputs[i], &observed) != 0 ||
            observed != task->expected_outputs[i]) {
            return 0;
        }
    }

    *out_solved = true;
    return 0;
}
