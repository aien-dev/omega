/*
 * cl_program.h -- Crumbline candidate programs as step chains over Omega's
 * existing unary operations, plus the operation bank the search draws from.
 *
 * A step chain is an ordered list of (OpCode, immediate) applied to one u64.
 * It is the canonical, content-addressed form of a candidate. It is realized
 * into a real OmegaProgram only through omega_program_build_unary_op +
 * omega_program_compose, and it is checked with Omega's own verifier
 * (omega_program_verify: V0 structural + V2 properties) plus a differential
 * check of native execution against the reference semantics
 * (omega_eval_pure_binary_uint).
 *
 * Shared by the learner and the sealed teacher. Contains nothing sealed.
 */
#ifndef CL_PROGRAM_H
#define CL_PROGRAM_H

#include "cl_common.h"
#include "omega_program.h"
#include "omega_library.h"
#include "omega_vc_bridge.h"

#define CL_MAX_STEPS 24
#define CL_BANK_MAX 96
#define CL_PROBE_COUNT 32

typedef struct {
    uint8_t op; /* OpCode: OP_ADD, OP_SUB, OP_MUL, OP_AND, OP_OR */
    uint64_t imm;
} ClStep;

typedef struct {
    uint8_t n;
    ClStep s[CL_MAX_STEPS];
} ClSteps;

bool cl_step_op_valid(uint8_t op);

/* Reference semantics (wrapping u64), independent of machine code. */
uint64_t cl_steps_eval(const ClSteps *p, uint64_t x);

/* Instruction count of the realized program (matches omega_program_compose). */
uint32_t cl_steps_insns(const ClSteps *p);

/* a then b. Returns -1 if the result would exceed CL_MAX_STEPS. */
int cl_steps_concat(const ClSteps *a, const ClSteps *b, ClSteps *out);

/* Canonical encoding: u8 n, n x (u8 op, u64 imm). */
int cl_steps_encode(const ClSteps *p, ClWriter *w);
int cl_steps_decode(ClReader *r, ClSteps *out);

/* Program digest: SHA-256("CRUMBLINE-V1-PROGRAM" || encoding). */
void cl_steps_digest(const ClSteps *p, uint8_t out[CL_DIGEST_BYTES]);

/* Behaviour signature: outputs on the fixed public probe inputs. Two chains
 * with the same signature agree on every probe (not necessarily everywhere). */
void cl_steps_behavior(const ClSteps *p, uint8_t out[CL_DIGEST_BYTES]);
const uint64_t *cl_probe_inputs(void);

/* Realize through Omega's builders. The program is named "op_<digest16>" so
 * its SemanticId is determined by the step chain. Caller destroys `out`. */
int cl_steps_build_program(const ClSteps *p, OmegaProgram *out);

/* Native execution vs reference semantics on `inputs` plus the probes.
 * Returns the number of disagreements (0 = differentially consistent),
 * or -1 if the realization could not be executed. */
int cl_steps_differential(const ClSteps *p, const OmegaProgram *prog, const uint64_t *inputs, size_t n,
                          uint32_t *exec_count);

/* ---- operation bank ------------------------------------------------- */

typedef enum { CL_OP_BASE = 0, CL_OP_LIBRARY = 1 } ClOpOrigin;

typedef struct {
    ClSteps steps;
    uint8_t digest[CL_DIGEST_BYTES]; /* learner-internal program digest */
    uint8_t origin;                  /* ClOpOrigin */
    uint32_t op_ref;                 /* ADMIT reference for library ops, 0 for base */
} ClOp;

typedef struct {
    ClOp ops[CL_BANK_MAX];
    size_t count;
} ClBank;

/* Base vocabulary: exactly omega_synth_base_prim_defs(). */
int cl_bank_init_base(ClBank *bank);
int cl_bank_add(ClBank *bank, const ClSteps *steps, ClOpOrigin origin, uint32_t op_ref);
void cl_bank_digest(const ClBank *bank, uint8_t out[CL_DIGEST_BYTES]);

/* ---- learner library: OmegaLibrary plus each entry's step chain ------- */

typedef struct {
    OmegaLibrary lib;                          /* the canonical semantic-object store */
    OmegaVcBridge bridge;                      /* the Verified Crumb Store and receipts behind lib */
    ClSteps steps[OMEGA_LIB_MAX_PROGRAMS];     /* step chain of lib.entries[i] */
    uint8_t scope_bits[OMEGA_LIB_MAX_PROGRAMS]; /* widest input magnitude (bits) it was verified over */
    uint32_t op_ref[OMEGA_LIB_MAX_PROGRAMS];   /* sealed-side admission reference */
} ClLearnerLibrary;

int cl_library_init(ClLearnerLibrary *l);
void cl_library_destroy(ClLearnerLibrary *l);

/* Build, verify (V0+V2 + differential), then admit through omega_vc_bridge_admit: the program
 * enters the Verified Crumb Store through omega_resolve_admit with a (self-minted HOST_TEST)
 * receipt, and the library entry carries that receipt id (ADR 0029 Decision 11). The evidence
 * digest binds the admission reference and the program. */
int cl_library_admit(ClLearnerLibrary *l, const ClSteps *steps, uint8_t scope_bits, uint32_t op_ref);

/* Add every entry whose verified scope covers inputs of `input_bits` bits.
 * Returns how many were added. Entries outside scope are withheld. */
int cl_library_export(const ClLearnerLibrary *l, uint8_t input_bits, ClBank *bank);

/* ---- Candidate Program v1 (CPG1, owned by the crumbs crate) ---------- */

/* Omega OpCode -> CPG1 op byte (0 if the op has no CPG1 form). */
uint8_t cl_cpg1_op(uint8_t omega_op);
uint8_t cl_omega_op(uint8_t cpg1_op);
int cl_steps_encode_cpg1(const ClSteps *p, ClWriter *w);
int cl_steps_decode_cpg1(ClReader *r, ClSteps *out);

#endif /* CL_PROGRAM_H */
