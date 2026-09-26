/* =========================================================================
 * TERNARY SEMANTICS EXPERIMENT - PHYSICS AArch64 LOWERING (ADDITIVE)
 * Lowers ternary ops onto binary AArch64. Self-contained instruction
 * encoders; src/aarch64_encoder.c is not modified.
 * ========================================================================= */

#ifndef OMEGA_TERNARY_A64_H
#define OMEGA_TERNARY_A64_H

#include "omega_ternary.h"
#include "omega_realize.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Two binary realizations of one T32 word, both in a single X register:
 *   TREP_INT    - the value as two's-complement int64 in [-TW_MAX, TW_MAX]
 *   TREP_PLANES - bits 0..31 = positive trits, bits 32..63 = negative trits */
typedef enum {
    TREP_INT = 0,
    TREP_PLANES = 1
} TernaryRep;

#define TREP_COUNT 2

/* One chain step: op applied to the running value with constant operand k
 * (a word for binary ops, a trit count for TSHL/TSHR, ignored for unary). */
typedef struct {
    TernaryOp op;
    int64_t k;
} TStep;

#define TCHAIN_MAX_STEPS 6

typedef struct {
    TStep steps[TCHAIN_MAX_STEPS];
    size_t n;
} TChain;

/* Per-step lowering facts proven by interval analysis (INT rep only). */
typedef struct {
    bool elide_clamp; /* result provably within [-TW_MAX, TW_MAX] */
    bool elide_mulh;  /* int64 product provably exact */
} TStepFlags;

/* Whether op has a direct lowering in rep (TMUL has none in PLANES). */
bool omega_t_a64_supported(TernaryOp op, TernaryRep rep);

/* Lower a whole chain. Input: int64 value in X0; output: int64 value in X0.
 * reps[i] selects the realization of step i; conversions are inserted at
 * every representation change and at the integer boundary. */
int omega_t_a64_lower_chain(const TChain *chain, const TernaryRep *reps,
                            const TStepFlags *flags, RealizationObject *out);

/* Lower one op for per-op expansion measurement.
 * Input a in X0 and b in X1, both already encoded in rep; result in X0 (rep).
 * For TSHL/TSHR the shift is the immediate k. out_body_insns receives the
 * static instruction count of the op body excluding hoisted constants. */
int omega_t_a64_lower_op(TernaryOp op, TernaryRep rep, int64_t k,
                         RealizationObject *out, uint32_t *out_body_insns,
                         uint32_t *out_prologue_insns);

/* Exact dynamic instruction count predicted for executing one step body
 * (no conversions) on runtime input a with constant operand b. */
uint64_t omega_t_a64_step_dyn(TernaryOp op, TernaryRep rep, int64_t a, int64_t b,
                              const TStepFlags *flags);

/* Dynamic counts of the boundary conversions for value v. */
uint64_t omega_t_a64_to_planes_dyn(int64_t v);
uint64_t omega_t_a64_from_planes_dyn(int64_t v);

/* Exact dynamic count predicted for a whole lowered chain on input x
 * (prologue + conversions + bodies + RET). */
uint64_t omega_t_a64_chain_dyn(const TChain *chain, const TernaryRep *reps,
                               const TStepFlags *flags, int64_t x);

/* Iterations the bitsliced PLANES adder takes for a + b. */
uint32_t omega_t_planes_add_iters(int64_t a, int64_t b);

/* Binary-counterpart instruction count for the expansion-factor table. */
uint32_t omega_t_binary_counterpart_insns(TernaryOp op);
const char *omega_t_binary_counterpart_name(TernaryOp op);

/* Binary control primitives for the augmented binary bank:
 * x0 = -x0, x0 = x0 << imm, x0 = x0 >> imm (arithmetic); then RET. */
typedef enum { TBIN_NEG = 0, TBIN_LSL = 1, TBIN_ASR = 2 } TBinaryExtra;
int omega_t_a64_binary_extra(TBinaryExtra kind, int imm, RealizationObject *out);

/* Emits a short sample of every extension encoder into buf and returns the
 * words; used by the gate to cross-check encodings against fixtures. */
size_t omega_t_a64_encoder_samples(uint32_t *out_words, size_t max_words);

#endif /* OMEGA_TERNARY_A64_H */
