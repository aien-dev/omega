/* =========================================================================
 * TERNARY SEMANTICS EXPERIMENT - AEGIS TERNARY INVARIANTS (ADDITIVE)
 * Ternary counterparts of verification tiers V0-V2. The binary tiers in
 * omega_verify.c are unchanged; these extend them for the ternary profile.
 * ========================================================================= */

#ifndef OMEGA_TERNARY_VERIFY_H
#define OMEGA_TERNARY_VERIFY_H

#include "omega_ternary_a64.h"
#include "omega_verify.h"
#include "omega_ternary_measure.h"

/* A ternary program: meaning (chain + precondition |x| <= domain_bound)
 * plus one chosen realization. */
typedef struct {
    TChain chain;
    TernaryRep reps[TCHAIN_MAX_STEPS];
    TStepFlags flags[TCHAIN_MAX_STEPS];
    int64_t domain_bound;
    RealizationObject real;
    SemanticId program_id; /* from OMG1 bytes of the meaning; realization-independent */
    bool is_realized;
    bool is_verified;
} TProgram;

/* Evaluates the chain on the reference model. */
int omega_t_chain_eval(const TChain *chain, int64_t x, int64_t *out);

/* Interval analysis over [-bound, bound]: fills flags with the clamps and
 * overflow checks that are provably unnecessary. Returns 0 on success. */
int omega_t_chain_intervals(const TChain *chain, int64_t bound, TStepFlags *flags);

/* Semantic id: SHA-256 over the OMG1 encodings of the steps and the bound. */
int omega_t_program_compute_id(TProgram *prog);

/* Lowers prog->chain with prog->reps / prog->flags into prog->real. */
int omega_t_program_realize(TProgram *prog);

/* T-V0: structural. Instruction-class whitelist (register-only ALU, moves,
 * local branches, one terminal RET), no memory or system access, no SP or
 * callee-saved register writes, all branch targets inside the region. */
int omega_t_verify_v0(const RealizationObject *real, VerifyReport *report);

/* T-V1: native differential against the model over the contract domain:
 * exhaustive for |x| <= 3280, the domain edges, and seeded random points. */
int omega_t_verify_v1(const TProgram *prog, VerifyReport *report);

/* T-V2: ternary invariants checked on the realization itself:
 *  - symmetric range: every output within [-TW_MAX, TW_MAX]
 *  - odd symmetry:   f(-x) == -f(x) whenever every step is odd
 *  - direction:      monotone / antitone whenever every step is */
int omega_t_verify_v2(const TProgram *prog, VerifyReport *report);

int omega_t_verify_pipeline(const TProgram *prog, VerifyReport *report);

/* Same tiers against an already-mapped realization (no syscalls), so the
 * mutation campaign can run them inside a seccomp-strict sandbox. */
int omega_t_verify_pipeline_mapped(const TProgram *prog, const TJit *jit, VerifyReport *report);

#endif /* OMEGA_TERNARY_VERIFY_H */
