#include "omega_program_fp32.h"
#include "omega_numeric.h"
#include <stdio.h>
#include <string.h>

static int refuse(char *why, size_t n, const char *fmt, unsigned a, const char *s) {
    if (why && n) snprintf(why, n, fmt, a, s);
    return -1;
}

static const char *op_name(uint8_t op) {
    switch (op) {
    case OP_ADD: return "ADD";
    case OP_SUB: return "SUB";
    case OP_MUL: return "MUL";
    case OP_DIV: return "DIV";
    case OP_AND: return "AND";
    case OP_OR: return "OR";
    case OP_CONVERT: return "CONVERT";
    default: return "an opcode";
    }
}

/* Walk the body as the two-state machine of omega_program.c lower_body_fp32 and say why it is refused. */
static int walk(const OmegaProgram *prog, char *why, size_t n) {
    const OmegaContract *c = &prog->contract;
    if (!prog->body.has_body) return refuse(why, n, "no semantic body%.0u%s", 0, "");
    if (prog->body.step_count > OMEGA_PROGRAM_MAX_STEPS) return refuse(why, n, "more than 64 steps%.0u%s", 0, "");
    if (c->input_width != 32 || c->output_width != 32) return refuse(why, n, "FP32 programs are width 32%.0u%s", 0, "");
    if ((c->input_type != TYPE_FP32 && c->input_type != TYPE_UNSIGNED_INT) ||
        (c->output_type != TYPE_FP32 && c->output_type != TYPE_UNSIGNED_INT))
        return refuse(why, n, "contract types must be FP32 or u32%.0u%s", 0, "");
    TypeTag cur = c->input_type;
    for (uint16_t i = 0; i < prog->body.step_count; ++i) {
        uint8_t op = prog->body.steps[i].op;
        uint64_t imm = prog->body.steps[i].imm;
        if (op == OP_CONVERT) {
            if (imm != TYPE_FP32 && imm != TYPE_UNSIGNED_INT) return refuse(why, n, "step %u: CONVERT target is neither FP32 nor u32%s", i, "");
            if ((TypeTag)imm == cur) return refuse(why, n, "step %u: CONVERT to the type the value already has%s", i, "");
            cur = (TypeTag)imm;
            continue;
        }
        if (cur != TYPE_FP32)
            return refuse(why, n, "step %u: %s applied to a u32 value in an FP32 program: mixed types need an explicit CONVERT", i, op_name(op));
        if (op != OP_ADD && op != OP_SUB && op != OP_MUL && op != OP_DIV)
            return refuse(why, n, "step %u: %s is not an FP32 operation (ADD SUB MUL DIV CONVERT)", i, op_name(op));
        if (imm > 0xFFFFFFFFull) return refuse(why, n, "step %u: constant wider than 32 bits%s", i, "");
    }
    if (cur != c->output_type) return refuse(why, n, "body ends as a different type than the contract output%.0u%s", 0, "");
    return 0;
}

int omega_program_fp32_check(const OmegaProgram *prog, char *why, size_t why_len) {
    if (!prog) return refuse(why, why_len, "no program%.0u%s", 0, "");
    if (walk(prog, why, why_len) != 0) return -1;
    OmegaProgram chk = *prog;
    chk.graph = NULL;
    if (omega_program_compute_id(&chk) != 0 || memcmp(chk.program_id.bytes, prog->program_id.bytes, OMEGA_ID_BYTES) != 0)
        return refuse(why, why_len, "program_id is not the v2 id of this body and contract (stale or forged)%.0u%s", 0, "");
    if (why && why_len) why[0] = '\0';
    return 0;
}

int omega_program_build_fp32(OmegaProgram *prog, const char *name, TypeTag in_type, TypeTag out_type,
                             const OmegaProgramStep *steps, uint16_t count) {
    if (!prog || (count && !steps) || count > OMEGA_PROGRAM_MAX_STEPS) return -1;
    omega_program_init(prog, name);
    prog->contract.input_type = in_type;
    prog->contract.input_width = 32;
    prog->contract.output_type = out_type;
    prog->contract.output_width = 32;
    prog->body.has_body = true;
    prog->body.step_count = count;
    if (count) memcpy(prog->body.steps, steps, count * sizeof(OmegaProgramStep));
    if (walk(prog, NULL, 0) != 0) return -1;
    snprintf(prog->contract.precondition, sizeof(prog->contract.precondition), "x is a binary32 or u32 value");
    omega_build_constraint_id(CONST_PRECONDITION, prog->contract.precondition, &prog->contract.precondition_id);
    snprintf(prog->contract.postcondition, sizeof(prog->contract.postcondition), "fp32 program of %u steps", (unsigned)count);
    omega_build_constraint_id(CONST_POSTCONDITION, prog->contract.postcondition, &prog->contract.postcondition_id);
    return omega_program_compute_id(prog) == 0 ? 0 : -1;
}

int omega_program_build_convert(OmegaProgram *prog, const char *name, TypeTag from, TypeTag to) {
    OmegaProgramStep s = { OP_CONVERT, (uint64_t)to };
    return omega_program_build_fp32(prog, name, from, to, &s, 1);
}

int omega_program_fp32_eval(const OmegaProgram *prog, const uint32_t *xs, size_t n, uint32_t *ys) {
    if (!prog || (n && (!xs || !ys))) return -1;
    if (omega_program_fp32_check(prog, NULL, 0) != 0) return -1;
    for (size_t k = 0; k < n; ++k) {
        float v = omega_bits_to_float(xs[k]);   /* a u32 travels as its bit pattern, the library's convention */
        for (uint16_t i = 0; i < prog->body.step_count; ++i) {
            uint8_t op = prog->body.steps[i].op;
            float kc = omega_bits_to_float((uint32_t)prog->body.steps[i].imm), r = 0.0f;
            OmegaNumericOp nop;
            if (op == OP_CONVERT) {
                nop = prog->body.steps[i].imm == TYPE_FP32 ? OMEGA_NOP_I2FP_U32 : OMEGA_NOP_F2U;
                if (omega_numeric_cpu_realize(nop, &v, &v, NULL, &r, 1) != OMEGA_NUMERIC_OK) return -1;
            } else {
                nop = op == OP_ADD ? OMEGA_NOP_FADD : op == OP_SUB ? OMEGA_NOP_FSUB : op == OP_MUL ? OMEGA_NOP_FMUL : OMEGA_NOP_DIV;
                if (omega_numeric_cpu_realize(nop, &v, &kc, NULL, &r, 1) != OMEGA_NUMERIC_OK) return -1;
            }
            v = r;
        }
        ys[k] = omega_float_to_bits(v);
    }
    return 0;
}
