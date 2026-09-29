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

/* Canonical semantic body (spec/program-identity.md 2.1): an ordered chain of
 * unary steps, innermost first: f(x) = op_n(...op_1(x, imm_1)..., imm_n).
 * Stored by value so OmegaProgram stays safe to copy. */
#define OMEGA_PROGRAM_MAX_STEPS 64
#define OMEGA_PROGRAM_ID_DOMAIN "omega.program.v2"

typedef struct {
    uint8_t op;      /* OpCode: OP_ADD OP_SUB OP_MUL OP_AND OP_OR */
    uint64_t imm;
} OmegaProgramStep;

typedef struct {
    bool has_body;   /* false: meaning unknown; the program has no identity */
    uint16_t step_count;
    OmegaProgramStep steps[OMEGA_PROGRAM_MAX_STEPS];
} OmegaProgramBody;

typedef struct {
    SemanticId program_id;
    char name[64];          /* metadata: not part of the identity */
    OmegaProgramBody body;  /* semantic: part of the identity */
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

/* Program identity v2 (spec/program-identity.md):
 *   SHA256("omega.program.v2" 0x00 || body_root_id || in_type_id || out_type_id || pre_id || post_id)
 * body_root_id is the id of the root APPLY of the body lowered with the existing
 * canonical builders. Name, cost and realization are metadata and not hashed.
 * Returns -1 and zeroes program_id when the program has no body (no identity). */
int omega_program_compute_id(OmegaProgram *prog);

/* The canonical body root id alone (the SemanticId of the body's root object). */
int omega_program_body_root_id(const OmegaProgram *prog, SemanticId *out_root);

/* Lift a realization in the builder's rigid unary template
 *   (movz x1, lo16 [; movk x1, hi16, lsl 16] ; op x0, x0, x1)* ; ret
 * back to a body. Confirmed by re-emitting the steps and requiring byte equality.
 * Returns 0 and fills out_body on success, -1 if the code is not in the template. */
int omega_program_lift_body(const uint8_t *code, size_t code_len, OmegaProgramBody *out_body);

/* Emit the canonical realization of a body (the template above, ending in RET). */
int omega_program_emit_body(const OmegaProgramBody *body, uint8_t *code, size_t *code_len, size_t max_len);

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
