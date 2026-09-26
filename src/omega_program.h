#ifndef OMEGA_PROGRAM_H
#define OMEGA_PROGRAM_H

#include "omega_types.h"
#include "omega_realize.h"
#include "omega_verify.h"
#include <stdbool.h>

typedef struct {
    uint32_t insn_count;
    uint32_t reg_pressure;
    uint32_t memory_bytes;
    uint32_t latency_cycles;
} OmegaCost;

typedef struct {
    TypeTag input_type;
    uint16_t input_width;
    TypeTag output_type;
    uint16_t output_width;
    SemanticId precondition_id;
    SemanticId postcondition_id;
    char precondition[64];
    char postcondition[64];
} OmegaContract;

/* Construct canonical constraint SemanticId from ConstraintKind and annotation */
int omega_build_constraint_id(ConstraintKind kind, const char *annotation, SemanticId *out_id);

typedef struct {
    SemanticId program_id;
    char name[64];
    OmegaContract contract;
    OmegaCost cost;
    OmegaGraph *graph;
    RealizationObject realization;
    bool is_realized;
    bool is_verified;
} OmegaProgram;

typedef struct {
    SemanticId task_id;
    char description[128];
    OmegaContract target_contract;
    OmegaCost cost_budget;
    size_t example_count;
    uint64_t inputs[16];
    uint64_t expected_outputs[16];
} SynthesisTask;

/* Initialize empty program */
void omega_program_init(OmegaProgram *prog, const char *name);

/* Free resources owned by program */
void omega_program_destroy(OmegaProgram *prog);

/* Compute semantic ID of program based on contract, cost, and name */
int omega_program_compute_id(OmegaProgram *prog);

/* Validate contract conformance */
int omega_program_validate_contract(const OmegaProgram *prog, char *err_msg, size_t err_msg_len);

/* Compose two programs: C(x) = B(A(x))
 * Validates OutType(A) == InType(B)
 * Derives C's contract and cost: Cost(C) = Cost(A) + Cost(B)
 */
int omega_program_compose(const OmegaProgram *a, const OmegaProgram *b, OmegaProgram *out_c, char *err_msg, size_t err_msg_len);

/* Direct AArch64 machine realization of program */
int omega_program_realize(OmegaProgram *prog);

/* Verify program using M7 verification engine (V0, V1, V2) */
int omega_program_verify(OmegaProgram *prog, VerifyReport *report);

/* Execute program natively: returns 0 on success, writes result to out_val */
int omega_program_exec(const OmegaProgram *prog, uint64_t in_val, uint64_t *out_val);

/* Create canonical linear unary program: f(x) = op(x, imm)
 * Supported: OP_ADD, OP_SUB, OP_MUL, OP_AND, OP_OR
 */
int omega_program_build_unary_op(OmegaProgram *prog, const char *name, OpCode op, uint64_t imm);

/* Create and evaluate a synthesis task */
int omega_task_init(SynthesisTask *task, const char *desc,
                    TypeTag in_type, uint16_t in_width,
                    TypeTag out_type, uint16_t out_width,
                    const uint64_t *inputs, const uint64_t *outputs, size_t count);

int omega_task_evaluate_candidate(const SynthesisTask *task, const OmegaProgram *candidate, bool *out_solved);

#endif /* OMEGA_PROGRAM_H */
