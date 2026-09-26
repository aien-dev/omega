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

int omega_program_compute_id(OmegaProgram *prog) {
    if (!prog) return -1;
    uint8_t buf[256];
    size_t pos = 0;

    /* Magic "OMG_PROG" */
    buf[pos++] = 'P'; buf[pos++] = 'R'; buf[pos++] = 'O'; buf[pos++] = 'G';
    size_t nlen = strlen(prog->name);
    if (nlen > 60) nlen = 60;
    buf[pos++] = (uint8_t)nlen;
    memcpy(&buf[pos], prog->name, nlen); pos += nlen;

    buf[pos++] = (uint8_t)prog->contract.input_type;
    buf[pos++] = (uint8_t)(prog->contract.input_width & 0xFF);
    buf[pos++] = (uint8_t)prog->contract.output_type;
    buf[pos++] = (uint8_t)(prog->contract.output_width & 0xFF);

    buf[pos++] = (uint8_t)(prog->cost.insn_count & 0xFF);
    buf[pos++] = (uint8_t)(prog->cost.latency_cycles & 0xFF);

    sha256_hash(buf, pos, prog->program_id.bytes);
    return 0;
}

int omega_program_validate_contract(const OmegaProgram *prog, char *err_msg, size_t err_msg_len) {
    if (!prog) return -1;
    if (prog->contract.input_type == TYPE_INVALID) {
        snprintf(err_msg, err_msg_len, "Program contract has invalid input type");
        return -1;
    }
    if (prog->contract.output_type == TYPE_INVALID) {
        snprintf(err_msg, err_msg_len, "Program contract has invalid output type");
        return -1;
    }
    if (prog->contract.input_width == 0 || prog->contract.output_width == 0) {
        snprintf(err_msg, err_msg_len, "Program contract has 0-width type");
        return -1;
    }
    return 0;
}

int omega_program_build_unary_op(OmegaProgram *prog, const char *name, OpCode op, uint64_t imm) {
    if (!prog) return -1;
    omega_program_init(prog, name);

    prog->contract.input_type = TYPE_UNSIGNED_INT;
    prog->contract.input_width = 64;
    prog->contract.output_type = TYPE_UNSIGNED_INT;
    prog->contract.output_width = 64;
    snprintf(prog->contract.precondition, sizeof(prog->contract.precondition), "x >= 0");

    size_t pos = 0;
    uint8_t *code = prog->realization.code_bytes;
    size_t max_len = sizeof(prog->realization.code_bytes);

    /* 1. MOVZ X1, imm */
    aarch64_emit_movz(code, &pos, max_len, true, REG_X1, (uint16_t)(imm & 0xFFFF), 0);
    if ((imm >> 16) != 0) {
        aarch64_emit_movk(code, &pos, max_len, true, REG_X1, (uint16_t)((imm >> 16) & 0xFFFF), 16);
    }

    /* 2. Compute op */
    switch (op) {
        case OP_ADD:
            aarch64_emit_add_reg(code, &pos, max_len, true, REG_X0, REG_X0, REG_X1);
            snprintf(prog->contract.postcondition, sizeof(prog->contract.postcondition), "x + %lu", (unsigned long)imm);
            break;
        case OP_SUB:
            aarch64_emit_sub_reg(code, &pos, max_len, true, REG_X0, REG_X0, REG_X1);
            snprintf(prog->contract.postcondition, sizeof(prog->contract.postcondition), "x - %lu", (unsigned long)imm);
            break;
        case OP_MUL:
            aarch64_emit_mul_reg(code, &pos, max_len, true, REG_X0, REG_X0, REG_X1);
            snprintf(prog->contract.postcondition, sizeof(prog->contract.postcondition), "x * %lu", (unsigned long)imm);
            break;
        case OP_AND:
            aarch64_emit_and_reg(code, &pos, max_len, true, REG_X0, REG_X0, REG_X1);
            snprintf(prog->contract.postcondition, sizeof(prog->contract.postcondition), "x & %lu", (unsigned long)imm);
            break;
        case OP_OR:
            aarch64_emit_orr_reg(code, &pos, max_len, true, REG_X0, REG_X0, REG_X1);
            snprintf(prog->contract.postcondition, sizeof(prog->contract.postcondition), "x | %lu", (unsigned long)imm);
            break;
        default:
            return -1;
    }

    /* 3. RET */
    aarch64_emit_ret(code, &pos, max_len);

    prog->realization.code_len = pos;
    prog->realization.target_profile = AARCH64_PROFILE_V8A_BAREMETAL;
    prog->realization.entry_offset = 0;

    prog->cost.insn_count = (uint32_t)(pos / 4);
    prog->cost.reg_pressure = 2;
    prog->cost.memory_bytes = 0;
    prog->cost.latency_cycles = (op == OP_MUL) ? 3 : 2;

    omega_compute_realization_id(&prog->realization);
    omega_program_compute_id(prog);
    prog->is_realized = true;

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
    snprintf(out_c->contract.precondition, sizeof(out_c->contract.precondition), "%.60s", a->contract.precondition);
    snprintf(out_c->contract.postcondition, sizeof(out_c->contract.postcondition),
             "(%.25s)o(%.25s)", b->contract.postcondition, a->contract.postcondition);

    /* 3. Cost Derivation: Monotonic cost accumulation */
    out_c->cost.insn_count = a->cost.insn_count + b->cost.insn_count - 1; /* Ret eliminated */
    out_c->cost.reg_pressure = (a->cost.reg_pressure > b->cost.reg_pressure) ? a->cost.reg_pressure : b->cost.reg_pressure;
    out_c->cost.memory_bytes = a->cost.memory_bytes + b->cost.memory_bytes;
    out_c->cost.latency_cycles = a->cost.latency_cycles + b->cost.latency_cycles;

    /* 4. Realization Fusion:
     * Body of A (excluding final RET) concatenated with entire body of B (including RET)
     */
    if (a->is_realized && b->is_realized && a->realization.code_len >= 4) {
        size_t a_body_len = a->realization.code_len - 4;
        if (a_body_len + b->realization.code_len > sizeof(out_c->realization.code_bytes)) {
            if (err_msg) snprintf(err_msg, err_msg_len, "Composite code buffer overflow");
            return -1;
        }

        memcpy(out_c->realization.code_bytes, a->realization.code_bytes, a_body_len);
        memcpy(out_c->realization.code_bytes + a_body_len, b->realization.code_bytes, b->realization.code_len);
        out_c->realization.code_len = a_body_len + b->realization.code_len;
        out_c->realization.target_profile = AARCH64_PROFILE_V8A_BAREMETAL;
        out_c->realization.entry_offset = 0;

        omega_compute_realization_id(&out_c->realization);
        out_c->is_realized = true;
    }

    omega_program_compute_id(out_c);
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
