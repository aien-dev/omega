#ifndef OMEGA_NUMERIC_DIVSQRT_GB10_H
#define OMEGA_NUMERIC_DIVSQRT_GB10_H
/*
 * E1 gap-table row 7: correctly rounded FP32 DIV and SQRT on the GB10
 * (Blackwell sm_121), bit-identical to omega_math_div / omega_math_sqrt
 * (round to nearest even, subnormals kept, no FTZ, canonical quiet NaN
 * 0x7fc00000 for every NaN result).
 *
 * The kernel is straight-line integer code (no MUFU, no FP arithmetic, no
 * branch): the vecadd prologue (thread index, bounds, LDG a and b) and
 * epilogue (STG, EXIT) from omega_blackwell_encode_vecadd, and a body built
 * as a small instruction list (OmegaDsInsn) that is
 *   - encoded into sm_121 words by omega_ds_encode (form templates recorded
 *     in OMEGA_DS_FORMS, every form decoded by nvdisasm offline),
 *   - executed on the host by omega_ds_host_exec (same list, same order),
 *   - checked structurally before submission (omega_ds_check_kernel).
 * The body mirrors the integer sequences of src/omega_numeric.c:
 * restoring long division (27 quotient bits), digit-by-digit square root
 * (27 result bits, 32-bit remainder form), one shared rounding block.
 * The only non-integer instruction is I2FP.F32.S32 on a subnormal's
 * fraction (< 2^23, so the conversion is exact) to find its leading bit.
 *
 * E1 row 10 adds the transcendentals (EXP2, LOG2) to the same frame. Their
 * bodies issue the frozen CPU sequence of src/omega_numeric_transc.c in the
 * written order: FADD (FSUB = FADD with -Rb), FMUL and FFMA (single
 * rounding) in place of the AArch64 FADD/FSUB/FMUL/FMADD, the integer DIV
 * body above in place of FDIV, integer bit operations for sign flips,
 * classification and powers of two, and every branch computed and chosen
 * with ISETP/SEL. Constants are loaded with IADD3 Rd, RZ, imm, RZ. No MUFU.
 * The host model runs the FP forms with the AArch64 instructions themselves
 * and turns any NaN result into 0x7fffffff (the GB10 canonical NaN); the
 * chip run, not the model, establishes GB10 parity.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* DIV and SQRT (E1 row 7) and the transcendentals of E1 row 10, each a
 * GB10 realization of the frozen CPU sequence in src/omega_numeric_transc.c. */
typedef enum { OMEGA_DS_DIV = 0, OMEGA_DS_SQRT = 1, OMEGA_DS_EXP2 = 2, OMEGA_DS_LOG2 = 3, OMEGA_DS_SIGMOID = 4, OMEGA_DS_TANH = 5, OMEGA_DS_SIN = 6, OMEGA_DS_COS = 7, OMEGA_DS_ERF = 8, OMEGA_DS_GELU = 9, OMEGA_DS_OP_COUNT = 10 } OmegaDsOp;

typedef enum {
    DSK_IADD3_R = 0, /* IADD3 Rd, PT, PT, [-]Ra, [-]Rb, Rc            */
    DSK_IADD3_I,     /* IADD3 Rd, PT, PT, [-]Ra, imm, Rc              */
    DSK_LOP3_R,      /* LOP3.LUT Rd, Ra, Rb, Rc, lut, !PT             */
    DSK_LOP3_I,      /* LOP3.LUT Rd, Ra, imm, Rc, lut, !PT            */
    DSK_SHL_I,       /* SHF.L.U32 Rd, Ra, imm, RZ                     */
    DSK_SHL_R,       /* SHF.L.U32 Rd, Ra, Rb, RZ                      */
    DSK_SHR_I,       /* SHF.R.U32.HI Rd, RZ, imm, Rc                  */
    DSK_SHR_R,       /* SHF.R.U32.HI Rd, RZ, Rb, Rc                   */
    DSK_ISETP_R,     /* ISETP.cmp[.U32].AND Pd, PT, Ra, Rb, PT        */
    DSK_ISETP_I,     /* ISETP.cmp[.U32].AND Pd, PT, Ra, imm, PT       */
    DSK_SEL_R,       /* SEL Rd, Ra, Rb, [!]Ps                         */
    DSK_SEL_I,       /* SEL Rd, Ra, imm, [!]Ps                        */
    DSK_I2FP,        /* I2FP.F32.S32 Rd, Rb                           */
    DSK_FADD_R,      /* FADD Rd, Ra, [-]Rb   (RN, subnormals kept)    */
    DSK_FMUL_R,      /* FMUL Rd, Ra, Rb      (RN, subnormals kept)    */
    DSK_FFMA_R,      /* FFMA Rd, Ra, Rb, Rc  (one rounding, RN)       */
    DSK_COUNT
} OmegaDsKind;

/* ISETP comparison codes (w2 bits 12-14, nvdisasm-decoded). */
enum { DS_CMP_LT = 1, DS_CMP_EQ = 2, DS_CMP_LE = 3, DS_CMP_GT = 4, DS_CMP_NE = 5, DS_CMP_GE = 6 };

#define OMEGA_DS_RZ 255u
#define OMEGA_DS_PT 7u

typedef struct {
    uint8_t kind;
    uint8_t d, a, b, c;     /* registers (255 = RZ); ISETP: d = predicate */
    uint8_t nega, negb;     /* IADD3 operand negation                     */
    uint8_t lut;            /* LOP3                                        */
    uint8_t cmp, is_signed; /* ISETP                                       */
    uint8_t ps, pneg;       /* SEL predicate                               */
    uint32_t imm;
} OmegaDsInsn;

#define OMEGA_DS_MAX_BODY       1000u
#define OMEGA_DS_PROLOGUE_INSNS 17u   /* vecadd 0x000-0x100 */
#define OMEGA_DS_EPILOGUE_INSNS 3u    /* STG, EXIT, BRA self */
#define OMEGA_DS_MAX_CODE_BYTES 0x4000u
/* QMD register allocation. The hardware reserves the top two registers of the
 * allocation (Xid 13 "Out Of Range Register" when a 32-register QMD ran a body
 * using R30/R31, receipt 1dc85ef2), so a body may use R0..R(count-3). 48 is the
 * next multiple of 16 that covers R31. */
#define OMEGA_DS_GPR_COUNT      48u
#define OMEGA_DS_GPR_RESERVED   2u
#define OMEGA_DS_RESULT_REG     9u
/* Control word (w3 & ~0x1ff) of every body instruction: stall 15, no write or
 * read barrier, wait on SB4 (the two LDGs of the prologue). The fixed-latency
 * pattern of ptxas 13.0.88 -O0 for sm_121 (stall 15 everywhere) with the wait
 * mask moved to the LDG barrier the vecadd prologue uses. */
#define OMEGA_DS_BODY_CTRL      0x010fde00u

/* Elements per launch the executor accepts (one launch, one buffer each). */
#define OMEGA_DS_MAX_BATCH      (1u << 24)

/* SHA-256 of each kernel as emitted, after nvdisasm 13.0.85 -b SM121 decoded
 * every word to the listing omega_ds_listing prints
 * (tools/divsqrt_nvdisasm_check.sh). */
extern const char *const OMEGA_DS_KERNEL_SHA256[OMEGA_DS_OP_COUNT];

const char *omega_ds_op_name(OmegaDsOp op);

/* Body instruction list of op. Returns the count (0 on error). */
size_t omega_ds_body(OmegaDsOp op, OmegaDsInsn *out, size_t max);

/* Encode one body instruction (control word OMEGA_DS_BODY_CTRL). */
int omega_ds_encode(const OmegaDsInsn *in, uint32_t w[4]);
/* Decode words back into an instruction of a recorded form; -1 if the words
 * are not exactly one of the forms (round trip through omega_ds_encode). */
int omega_ds_decode(const uint32_t w[4], OmegaDsInsn *out);
/* nvdisasm text for one body instruction, without the trailing " ;". */
int omega_ds_format(const OmegaDsInsn *in, char *buf, size_t len);

/* Whole kernel (prologue, body, epilogue, NOP pad to 128 bytes). */
int omega_ds_build_kernel(OmegaDsOp op, uint8_t *code, size_t max, size_t *out_len);
/* Expected nvdisasm listing of the kernel, one line per instruction: 4-digit hex address, a space, the nvdisasm text. */
int omega_ds_listing(OmegaDsOp op, char *buf, size_t len);

/* Runs the body instruction list on the host (bit-exact model of the integer
 * forms; I2FP via the host's exact int->float conversion; FADD/FMUL/FFMA via the
 * AArch64 instructions, NaN results as 0x7fffffff). */
uint32_t omega_ds_host_exec(OmegaDsOp op, uint32_t a, uint32_t b);

/* The CPU semantic the chip must equal for DIV and SQRT: omega_math_div /
 * omega_math_sqrt (0x7fc00000 for any other op; the transcendental semantic
 * lives in src/omega_numeric_transc.c, which this module does not link). */
uint32_t omega_ds_cpu_semantic(OmegaDsOp op, uint32_t a, uint32_t b);

/* Structural pre-submission check of a kernel image (see CHECK: markers).
 * Returns 0 or a negative OMEGA_NUMERIC_ERR_* and writes "ds_<check>: ..." */
int omega_ds_check_kernel(OmegaDsOp op, const uint8_t *code, size_t len,
                          uint32_t gpr_count, char *err, size_t err_len);
/* Same plus the recorded nvdisasm-verified digest. */
int omega_ds_check_digest(OmegaDsOp op, const uint8_t *code, size_t len, char *err, size_t err_len);
/* Argument check: op, buffers, count. */
int omega_ds_check_args(OmegaDsOp op, const uint32_t *a, const uint32_t *b,
                        const uint32_t *out, size_t count, char *err, size_t err_len);
/* QMD check: invariants, register count, program address. */
int omega_ds_check_qmd(const uint32_t *qmd1, uint64_t code_va, char *err, size_t err_len);

/* Launch op on the GB10 over count elements (bit patterns). Every check above
 * runs first; with -DOMEGA_NUMERIC_CPU_ONLY the device is never touched and
 * OMEGA_NUMERIC_ERR_DEVICE is returned after the checks. */
int omega_ds_gb10_run(OmegaDsOp op, const uint32_t *a, const uint32_t *b, uint32_t *out, size_t count);

#endif
