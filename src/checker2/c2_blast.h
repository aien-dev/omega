/* c2_blast: checker2's bit-blaster for bounded integer programs (omega#446).
 *
 * Property NO-TRAP for function F of a unit checker2 has accepted:
 *   for every argument value, a run of F that passes its `requires` clauses (trap code 9 is the
 *   caller's fault, OSC-2) ends without any trap: no overflow, shift, cast, loop-bound or
 *   `ensures` (code 10) trap, and no explicit TRAP instruction is reached.
 * The CNF is satisfiable exactly when some argument value reaches such a trap, so an LRAT proof
 * of unsatisfiability, accepted by c2_lrat_check, proves the property.
 *
 * Semantics bit-blasted (docs/osc/OSC-1-DESIGN.md section 5): unsigned results reduce modulo 2^w;
 * signed ADD/SUB/MUL/NEG/SHL trap OVERFLOW when the result is not representable; a shift amount
 * outside 0..w-1 traps SHIFT; CAST traps CAST when the value is not representable in the target;
 * CMP is signed for signed operands; bool is one bit.
 *
 * Supported subset (anything else is refused, never approximated): scalar parameters and
 * registers only; CONST MOV BIN(ADD SUB MUL AND OR XOR SHL, SHR on unsigned) UN CMP CAST BR CBR
 * RET TRAP. Refused: CALL, memory, handles, slices, DIV, REM, SHR on signed operands. Every path is
 * executed symbolically (paths whose branch condition is a constant do not fork, so loops with a
 * constant counter unroll exactly); more than C2_BLAST_MAX_PATHS paths or C2_BLAST_MAX_STEPS
 * steps is refused as too large. */
#ifndef C2_BLAST_H
#define C2_BLAST_H

#include "c2_ir.h"
#include "c2_lrat.h"

#define C2_BLAST_MAX_PATHS 4096
#define C2_BLAST_MAX_STEPS 4000000L
#define C2_PROPERTY_NOTRAP "no-trap-except-requires"

typedef struct {
    const char *rule;      /* "BLAST-OK" or the refusal reason */
    char detail[160];
    int inputs;            /* argument bits (CNF variables 2..inputs+1) */
    int paths, trap_sites; /* explored paths; path points where a trap can fire */
} C2BlastInfo;

/* 0: cnf built (caller frees with c2_cnf_free); -1: refused (info->rule). The unit must already be
 * accepted by c2_check_unit; c2_blast_notrap runs it again and refuses otherwise. */
int c2_blast_notrap(const C2Unit *u, int fi, C2Cnf *cnf, C2BlastInfo *info);
/* index of the function called name, or -1 */
int c2_func_index(const C2Unit *u, const char *name);

#endif /* C2_BLAST_H */
