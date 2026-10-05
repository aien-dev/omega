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

#define OMEGA_PROGRAM_MAX_STEPS 64

typedef struct {
    TypeTag input_type;
    uint16_t input_width;
    TypeTag output_type;
    uint16_t output_width;
    SemanticId precondition_id;
    SemanticId postcondition_id;
    char precondition[64];
    char postcondition[64];     /* display text; for a composition, derived and not canonical */
    /* Composition (spec/program-identity.md 2.4): the flattened, ordered list of the
     * component (leaf) postcondition ids, in application order. postcondition_id is then
     * the canonical sequence constraint over this list, so composition is associative.
     * post_leaf_count == 0 means the postcondition is a leaf. */
    uint16_t post_leaf_count;
    SemanticId post_leaves[OMEGA_PROGRAM_MAX_STEPS];
} OmegaContract;

/* Construct canonical constraint SemanticId from ConstraintKind and annotation */
int omega_build_constraint_id(ConstraintKind kind, const char *annotation, SemanticId *out_id);

/* Canonical semantic body (spec/program-identity.md 2.1): an ordered chain of
 * unary steps, innermost first: f(x) = op_n(...op_1(x, imm_1)..., imm_n).
 * Stored by value so OmegaProgram stays safe to copy. */
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

/* Verified Crumb contract_id (aien-protocols specs/verified-crumb/SPEC.md 4.2):
 *   SHA256("aien.vc1.contract.v1" 0x00 || in_type_id || out_type_id || pre_id || post_id)
 * the same four components the program id hashes, with the same type ids. 0 or -1. */
int omega_program_contract_id(const OmegaProgram *prog, uint8_t out[32]);


/* The canonical body root id alone (the SemanticId of the body's root object). */
int omega_program_body_root_id(const OmegaProgram *prog, SemanticId *out_root);

/* ---- V0 program realization (spec/program-realization.md) ----------------
 * Pipeline: canonical body -> realize_check (verified semantic op sequence) ->
 * schedule (machine-aware choice, never meaning) -> AArch64 bytes via the one encoder
 * (aarch64_encoder.c) -> verification -> RealizationId. */
typedef enum {
    OMEGA_SCHED_SEQUENTIAL = 0,   /* one scratch register X1, load then op, per step */
    OMEGA_SCHED_PRELOAD = 1       /* blocks of constants preloaded into X1..X15, then ops */
} OmegaRealizeSchedule;
#define OMEGA_PRELOAD_REGS 15      /* X1..X15: caller-saved scratch only (never X16-X30) */

/* Fail-closed V0 gate: body present, unsigned in==out type, width 8/16/32/64, every step
 * ADD SUB MUL AND OR with a constant that fits the width, program_id == v2 id of body +
 * contract. 0 ok (width in *out_width), -1 refused with a reason in why. */
int omega_program_realize_check(const OmegaProgram *prog, uint16_t *out_width, char *why, size_t why_len);

/* Emit a schedule for body at width (width < 64 appends one AND with the width mask).
 * Constants: MOVZ chunk0 then MOVK for each nonzero higher 16-bit chunk (full 64 bits). */
int omega_program_emit_schedule(const OmegaProgramBody *body, uint16_t width, OmegaRealizeSchedule sched,
                                uint8_t *code, size_t *code_len, size_t max_len);

/* Independent semantic evaluator: lowers the body to the canonical objects the program
 * id binds and evaluates them with omega_eval_pure_binary_uint (no code involved).
 * Every xs[i] must lie in the declared input domain [0, 2^w). 0 ok, -1 refused. */
int omega_program_eval(const OmegaProgram *prog, const uint64_t *xs, size_t n, uint64_t *ys);

/* omega_program_realize with a reason: 0 ok, -2 refused by the V0 gate (why set), -1 error. */
int omega_program_realize_ex(OmegaProgram *prog, char *why, size_t why_len);

/* Lift a realization in the builder's rigid unary template
 *   (movz x1, c0 [; movk x1, ck, lsl 16k for each nonzero chunk k] ; op x0, x0, x1)* ; ret
 * back to a body. Confirmed by re-emitting the steps and requiring byte equality.
 * Returns 0 and fills out_body on success, -1 if the code is not in the template. */
int omega_program_lift_body(const uint8_t *code, size_t code_len, OmegaProgramBody *out_body);

/* Emit the canonical realization of a u64 body (the template above = the SEQUENTIAL schedule). */
int omega_program_emit_body(const OmegaProgramBody *body, uint8_t *code, size_t *code_len, size_t max_len);

/* Canonical sequence postcondition id over an ordered leaf list (n >= 1): n == 1 is the
 * leaf itself; otherwise the right fold Seq(l1, Seq(l2, ... ln)) of KIND_CONSTRAINT
 * objects with attribute "omega.compose" = "seq" and a CONST_POSTCONDITION constraint
 * whose payload is (head id || tail id). */
int omega_contract_post_seq_id(const SemanticId *leaves, uint16_t n, SemanticId *out);

/* Validate contract conformance */
int omega_program_validate_contract(const OmegaProgram *prog, char *err_msg, size_t err_msg_len);

/* Compose two programs: C(x) = B(A(x))
 * Validates OutType(A) == InType(B)
 * Derives C's contract and cost: Cost(C) = Cost(A) + Cost(B)
 */
int omega_program_compose(const OmegaProgram *a, const OmegaProgram *b, OmegaProgram *out_c, char *err_msg, size_t err_msg_len);

/* Canonical machine-independent AArch64 realization compiled from the program body:
 * V0 gate, SEQUENTIAL schedule, realization.semantic_id = program_id, OMG_R0 id, V0
 * structural check. Sets is_realized (not is_verified). 0 ok, nonzero refused. The
 * machine-bound (triple id) path is omega_synthesize_realization. */
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
