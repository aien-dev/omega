/*
 * osc_a64.h -- hardened AArch64 encoder + mirror decoder for the OSC-1 back end.
 *
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 *
 * Rules (docs/osc/OSC-1-DESIGN.md section 7, audit III.6):
 *  - every field is range-checked; an out-of-range field is refused, never masked;
 *  - SP and XZR are distinct operand kinds (OSC_A64_SP / OSC_A64_XZR). Raw
 *    register number 31 is not a valid operand: callers must say which one;
 *    each operand position accepts only the kind the architecture gives it;
 *  - only 64-bit (X) forms are produced;
 *  - osc_a64_decode is the mirror: it accepts exactly the words this encoder
 *    can produce and returns the same fields, so encode(decode(w)) == w and
 *    decode(encode(i)) == i (for i in canonical form, see below).
 *
 * Canonical form of OscA64Insn: unused fields are 0.
 */
#ifndef OSC_A64_H
#define OSC_A64_H

#include <stddef.h>
#include <stdint.h>

/* Register operands: 0..30 = X0..X30; the two meanings of encoding 31. */
#define OSC_A64_XZR 32
#define OSC_A64_SP  33

typedef enum {
    OSC_A64_INVALID = 0,
    /* shifted-register arithmetic: rd rn rm, shift (LSL/LSR/ASR), imm = amount 0..63 */
    OSC_A64_ADD_REG, OSC_A64_SUB_REG, OSC_A64_ADDS_REG, OSC_A64_SUBS_REG,
    /* immediate arithmetic: rd rn, imm = imm12 0..4095, imm2 = 1 for LSL #12 */
    OSC_A64_ADD_IMM, OSC_A64_SUB_IMM, OSC_A64_ADDS_IMM, OSC_A64_SUBS_IMM,
    /* rd = ra +/- rn*rm */
    OSC_A64_MADD, OSC_A64_MSUB,
    /* rd = high 64 bits of rn*rm */
    OSC_A64_SMULH, OSC_A64_UMULH,
    /* two-source: rd rn rm */
    OSC_A64_SDIV, OSC_A64_UDIV, OSC_A64_LSLV, OSC_A64_LSRV, OSC_A64_ASRV,
    /* logical shifted register: rd rn rm, shift (LSL/LSR/ASR), imm = amount */
    OSC_A64_AND_REG, OSC_A64_ORR_REG, OSC_A64_EOR_REG, OSC_A64_ORN_REG,
    /* bitfield: rd rn, imm = immr 0..63, imm2 = imms 0..63 */
    OSC_A64_SBFM, OSC_A64_UBFM,
    /* rd = cond ? rn : rm + 1 ; cond 0..14 */
    OSC_A64_CSINC,
    /* wide immediates: rd, imm = imm16, imm2 = shift 0/16/32/48 */
    OSC_A64_MOVZ, OSC_A64_MOVN, OSC_A64_MOVK,
    /* rd = Rt, rn = base (SP kind), imm = byte offset 0..32760, multiple of 8 */
    OSC_A64_LDR_UOFF, OSC_A64_STR_UOFF,
    /* rd = Rt, ra = Rt2, rn = base (SP kind), imm = byte offset -512..504, multiple of 8 */
    OSC_A64_STP_OFF, OSC_A64_STP_PRE, OSC_A64_STP_POST,
    OSC_A64_LDP_OFF, OSC_A64_LDP_PRE, OSC_A64_LDP_POST,
    /* imm = byte offset relative to this instruction, multiple of 4 */
    OSC_A64_B, OSC_A64_BL,      /* +-128 MiB */
    OSC_A64_BCOND,              /* cond 0..14, +-1 MiB */
    OSC_A64_CBZ, OSC_A64_CBNZ,  /* rd = Rt, +-1 MiB */
    /* rn = target / return register X0..X30 */
    OSC_A64_BLR, OSC_A64_RET,
    /* imm = imm16 */
    OSC_A64_BRK,
    OSC_A64_NOPS
} OscA64Op;

typedef enum { OSC_A64_LSL = 0, OSC_A64_LSR = 1, OSC_A64_ASR = 2 } OscA64Shift;

typedef enum {
    OSC_A64_EQ = 0, OSC_A64_NE, OSC_A64_HS, OSC_A64_LO, OSC_A64_MI, OSC_A64_PL,
    OSC_A64_VS, OSC_A64_VC, OSC_A64_HI, OSC_A64_LS, OSC_A64_GE, OSC_A64_LT,
    OSC_A64_GT, OSC_A64_LE, OSC_A64_AL
} OscA64Cond;

typedef struct {
    uint8_t op;      /* OscA64Op */
    uint8_t rd, rn, rm, ra;
    uint8_t shift;   /* OscA64Shift */
    uint8_t cond;    /* OscA64Cond */
    int64_t imm;
    int64_t imm2;
} OscA64Insn;

/* 0 ok (word written), -1 refused (reason in err if err != NULL). */
int osc_a64_encode(const OscA64Insn *in, uint32_t *word, char *err, size_t n);
/* 0 ok, -1 the word is not one this encoder can produce. */
int osc_a64_decode(uint32_t word, OscA64Insn *out);
/* Re-encode a decoded instruction (alias of osc_a64_encode, no message). */
int osc_a64_reencode(const OscA64Insn *in, uint32_t *word);
const char *osc_a64_op_name(int op);
/* 1 if the two instructions are field-identical. */
int osc_a64_equal(const OscA64Insn *x, const OscA64Insn *y);

/* Constructors (canonical form); validation happens in osc_a64_encode. */
OscA64Insn osc_a64_r3(OscA64Op op, int rd, int rn, int rm);
OscA64Insn osc_a64_r3s(OscA64Op op, int rd, int rn, int rm, OscA64Shift sh, int amount);
OscA64Insn osc_a64_ri(OscA64Op op, int rd, int rn, int64_t imm12, int lsl12);
OscA64Insn osc_a64_r4(OscA64Op op, int rd, int rn, int rm, int ra);
OscA64Insn osc_a64_bfm(OscA64Op op, int rd, int rn, int immr, int imms);
OscA64Insn osc_a64_csinc(int rd, int rn, int rm, OscA64Cond c);
OscA64Insn osc_a64_mov16(OscA64Op op, int rd, int64_t imm16, int shift);
OscA64Insn osc_a64_mem(OscA64Op op, int rt, int rn, int64_t off);
OscA64Insn osc_a64_pair(OscA64Op op, int rt, int rt2, int rn, int64_t off);
OscA64Insn osc_a64_br(OscA64Op op, int64_t off);
OscA64Insn osc_a64_bcond(OscA64Cond c, int64_t off);
OscA64Insn osc_a64_cb(OscA64Op op, int rt, int64_t off);
OscA64Insn osc_a64_breg(OscA64Op op, int rn);
OscA64Insn osc_a64_brk(int64_t imm16);

#endif /* OSC_A64_H */
