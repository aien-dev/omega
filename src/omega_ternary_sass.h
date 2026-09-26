/* =========================================================================
 * TERNARY SEMANTICS EXPERIMENT - PHYSICS BLACKWELL LOWERING (ADDITIVE)
 *
 * Decision: lower at the IR boundary. Ternary ops expand into ordinary
 * binary SASS operations before the Blackwell encoder; the encoder and the
 * BlackwellIR opcode set stay binary and unchanged. HMMA stays binary at the
 * tensor level; ternary lives above it.
 *
 * Status: STATIC. Sequences are counted and register-allocated here but not
 * yet encoded or executed on GB10: LOP3 with arbitrary LUT, SEL, ISETP.EX,
 * SHF.L and IMAD.WIDE variants need oracle-checked encodings first.
 * ========================================================================= */

#ifndef OMEGA_TERNARY_SASS_H
#define OMEGA_TERNARY_SASS_H

#include "omega_ternary_a64.h"

typedef struct {
    bool supported;
    uint32_t fixed_insns;     /* straight-line instructions */
    uint32_t loop_insns;      /* instructions per loop iteration (0 = no loop) */
    uint32_t peak_regs;       /* peak live 32-bit GPRs, inputs and outputs included */
    uint32_t preds;           /* predicate registers used */
} TSassCost;

/* Boundary conversions, as pseudo-ops accepted wherever a TernaryOp is.
 * Trit-wise logic in the INT realization is lowered through them. */
#define TSASS_TO_PLANES   ((TernaryOp)0x60)
#define TSASS_FROM_PLANES ((TernaryOp)0x61)

/* Lowered cost of one op (constant operand in registers, shifts immediate). */
int omega_t_sass_op_cost(TernaryOp op, TernaryRep rep, TSassCost *out);

/* 64-bit binary counterpart on the same SASS ISA. */
int omega_t_sass_binary64_cost(TernaryOp op, TSassCost *out);

/* Loop iterations for one lane, and the warp-level expectation:
 * mean over warps of max over 32 lanes, from seeded random operands. */
double omega_t_sass_mean_iters(TernaryOp op, TernaryRep rep, uint32_t operand_trits, bool warp_max);

/* Dumps the lowered sequence as text (for evidence). */
int omega_t_sass_listing(TernaryOp op, TernaryRep rep, char *out, size_t out_len);

#endif /* OMEGA_TERNARY_SASS_H */
