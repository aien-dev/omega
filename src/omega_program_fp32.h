#ifndef OMEGA_PROGRAM_FP32_H
#define OMEGA_PROGRAM_FP32_H
/*
 * FP32 in the program IR (E1 gap row 12; spec/program-fp32.md).
 *
 * An FP32 program is an OmegaProgram whose contract types are TYPE_FP32 or u32
 * (width 32) and whose body steps are, in the FP32 state, ADD SUB MUL DIV with a
 * binary32 constant (imm = the constant's bits) or OP_CONVERT to u32; in the u32
 * state the only step is OP_CONVERT (imm = TYPE_FP32). The type check refuses
 * anything else, so a u32 value never meets an FP32 operation, and two programs
 * compose only when OutType == InType, so FP32 and u32 programs join only through
 * a program that converts (omega_program_build_convert).
 *
 * FP32 programs have no AArch64 realization (omega_program_realize refuses them
 * on type); they are evaluated by the qualified numeric library tier
 * (omega_numeric_cpu_realize: FADD FSUB FMUL DIV, I2FP_U32, F2U) and carry a
 * program id like every other program.
 */
#include "omega_program.h"

/* Build name: in_type -> out_type over steps (<= 64). in_type/out_type are
 * TYPE_FP32 or TYPE_UNSIGNED_INT, width 32. 0 ok (id computed, not realized),
 * -1 refused. */
int omega_program_build_fp32(OmegaProgram *prog, const char *name, TypeTag in_type, TypeTag out_type,
                             const OmegaProgramStep *steps, uint16_t count);

/* The explicit conversion programs: u32 -> FP32 (RNE, I2FP_U32) and FP32 -> u32
 * (truncate toward zero, saturating, NaN -> 0 as omega_ref_f2u defines). */
int omega_program_build_convert(OmegaProgram *prog, const char *name, TypeTag from, TypeTag to);

/* Type and body check. 0 ok; -1 refused with the reason in why. */
int omega_program_fp32_check(const OmegaProgram *prog, char *why, size_t why_len);

/* Evaluate on the library tier. xs and ys are 32-bit patterns (binary32 bits for
 * an FP32 input or output, a u32 otherwise). 0 ok, -1 refused. */
int omega_program_fp32_eval(const OmegaProgram *prog, const uint32_t *xs, size_t n, uint32_t *ys);

#endif
