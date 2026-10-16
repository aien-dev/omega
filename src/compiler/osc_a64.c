/*
 * osc_a64.c -- hardened AArch64 encoder + mirror decoder (OSC-1 back end).
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 * See osc_a64.h for the rules. Nothing here masks a field: every field is
 * range-checked and an out-of-range or wrong-kind operand is refused.
 */
#include "osc_a64.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    CLS_NONE = 0, CLS_RRR_SH, CLS_RI, CLS_R4, CLS_MULH, CLS_R3, CLS_BFM, CLS_CSINC,
    CLS_MOV16, CLS_MEM, CLS_PAIR, CLS_B26, CLS_BCOND, CLS_CB, CLS_BREG, CLS_BRK,
    /* OSC-4 scalar FP: type (S/D) in imm2 -> bit 22 (data processing) or bit 30 (loads/stores) */
    CLS_FP3, CLS_FP2, CLS_FCMP, CLS_FMOV_GP, CLS_FCVT_FF, CLS_CVT, CLS_FMEM_UOFF, CLS_FMEM_REG
} Cls;

/* operand kinds for rd / rn / rm / ra positions; K_V (OSC-4) is a V register 0..31 */
enum { K_NONE = 0, K_ZR, K_SP, K_X, K_V };

typedef struct {
    const char *name;
    uint32_t base;
    uint8_t cls;
    uint8_t kd, kn, km, ka;
} OpInfo;

static const OpInfo OPS[OSC_A64_NOPS] = {
    [OSC_A64_ADD_REG]  = {"add",   0x8B000000u, CLS_RRR_SH, K_ZR, K_ZR, K_ZR, K_NONE},
    [OSC_A64_SUB_REG]  = {"sub",   0xCB000000u, CLS_RRR_SH, K_ZR, K_ZR, K_ZR, K_NONE},
    [OSC_A64_ADDS_REG] = {"adds",  0xAB000000u, CLS_RRR_SH, K_ZR, K_ZR, K_ZR, K_NONE},
    [OSC_A64_SUBS_REG] = {"subs",  0xEB000000u, CLS_RRR_SH, K_ZR, K_ZR, K_ZR, K_NONE},
    [OSC_A64_ADD_IMM]  = {"add",   0x91000000u, CLS_RI,     K_SP, K_SP, K_NONE, K_NONE},
    [OSC_A64_SUB_IMM]  = {"sub",   0xD1000000u, CLS_RI,     K_SP, K_SP, K_NONE, K_NONE},
    [OSC_A64_ADDS_IMM] = {"adds",  0xB1000000u, CLS_RI,     K_ZR, K_SP, K_NONE, K_NONE},
    [OSC_A64_SUBS_IMM] = {"subs",  0xF1000000u, CLS_RI,     K_ZR, K_SP, K_NONE, K_NONE},
    [OSC_A64_MADD]     = {"madd",  0x9B000000u, CLS_R4,     K_ZR, K_ZR, K_ZR, K_ZR},
    [OSC_A64_MSUB]     = {"msub",  0x9B008000u, CLS_R4,     K_ZR, K_ZR, K_ZR, K_ZR},
    [OSC_A64_SMULH]    = {"smulh", 0x9B407C00u, CLS_MULH,   K_ZR, K_ZR, K_ZR, K_NONE},
    [OSC_A64_UMULH]    = {"umulh", 0x9BC07C00u, CLS_MULH,   K_ZR, K_ZR, K_ZR, K_NONE},
    [OSC_A64_SDIV]     = {"sdiv",  0x9AC00C00u, CLS_R3,     K_ZR, K_ZR, K_ZR, K_NONE},
    [OSC_A64_UDIV]     = {"udiv",  0x9AC00800u, CLS_R3,     K_ZR, K_ZR, K_ZR, K_NONE},
    [OSC_A64_LSLV]     = {"lslv",  0x9AC02000u, CLS_R3,     K_ZR, K_ZR, K_ZR, K_NONE},
    [OSC_A64_LSRV]     = {"lsrv",  0x9AC02400u, CLS_R3,     K_ZR, K_ZR, K_ZR, K_NONE},
    [OSC_A64_ASRV]     = {"asrv",  0x9AC02800u, CLS_R3,     K_ZR, K_ZR, K_ZR, K_NONE},
    [OSC_A64_AND_REG]  = {"and",   0x8A000000u, CLS_RRR_SH, K_ZR, K_ZR, K_ZR, K_NONE},
    [OSC_A64_ORR_REG]  = {"orr",   0xAA000000u, CLS_RRR_SH, K_ZR, K_ZR, K_ZR, K_NONE},
    [OSC_A64_EOR_REG]  = {"eor",   0xCA000000u, CLS_RRR_SH, K_ZR, K_ZR, K_ZR, K_NONE},
    [OSC_A64_ORN_REG]  = {"orn",   0xAA200000u, CLS_RRR_SH, K_ZR, K_ZR, K_ZR, K_NONE},
    [OSC_A64_SBFM]     = {"sbfm",  0x93400000u, CLS_BFM,    K_ZR, K_ZR, K_NONE, K_NONE},
    [OSC_A64_UBFM]     = {"ubfm",  0xD3400000u, CLS_BFM,    K_ZR, K_ZR, K_NONE, K_NONE},
    [OSC_A64_CSINC]    = {"csinc", 0x9A800400u, CLS_CSINC,  K_ZR, K_ZR, K_ZR, K_NONE},
    [OSC_A64_MOVZ]     = {"movz",  0xD2800000u, CLS_MOV16,  K_ZR, K_NONE, K_NONE, K_NONE},
    [OSC_A64_MOVN]     = {"movn",  0x92800000u, CLS_MOV16,  K_ZR, K_NONE, K_NONE, K_NONE},
    [OSC_A64_MOVK]     = {"movk",  0xF2800000u, CLS_MOV16,  K_ZR, K_NONE, K_NONE, K_NONE},
    [OSC_A64_LDR_UOFF] = {"ldr",   0xF9400000u, CLS_MEM,    K_ZR, K_SP, K_NONE, K_NONE},
    [OSC_A64_STR_UOFF] = {"str",   0xF9000000u, CLS_MEM,    K_ZR, K_SP, K_NONE, K_NONE},
    [OSC_A64_STP_OFF]  = {"stp",   0xA9000000u, CLS_PAIR,   K_ZR, K_SP, K_NONE, K_ZR},
    [OSC_A64_STP_PRE]  = {"stp!",  0xA9800000u, CLS_PAIR,   K_ZR, K_SP, K_NONE, K_ZR},
    [OSC_A64_STP_POST] = {"stp+",  0xA8800000u, CLS_PAIR,   K_ZR, K_SP, K_NONE, K_ZR},
    [OSC_A64_LDP_OFF]  = {"ldp",   0xA9400000u, CLS_PAIR,   K_ZR, K_SP, K_NONE, K_ZR},
    [OSC_A64_LDP_PRE]  = {"ldp!",  0xA9C00000u, CLS_PAIR,   K_ZR, K_SP, K_NONE, K_ZR},
    [OSC_A64_LDP_POST] = {"ldp+",  0xA8C00000u, CLS_PAIR,   K_ZR, K_SP, K_NONE, K_ZR},
    [OSC_A64_B]        = {"b",     0x14000000u, CLS_B26,    K_NONE, K_NONE, K_NONE, K_NONE},
    [OSC_A64_BL]       = {"bl",    0x94000000u, CLS_B26,    K_NONE, K_NONE, K_NONE, K_NONE},
    [OSC_A64_BCOND]    = {"b.cond",0x54000000u, CLS_BCOND,  K_NONE, K_NONE, K_NONE, K_NONE},
    [OSC_A64_CBZ]      = {"cbz",   0xB4000000u, CLS_CB,     K_ZR, K_NONE, K_NONE, K_NONE},
    [OSC_A64_CBNZ]     = {"cbnz",  0xB5000000u, CLS_CB,     K_ZR, K_NONE, K_NONE, K_NONE},
    [OSC_A64_BLR]      = {"blr",   0xD63F0000u, CLS_BREG,   K_NONE, K_X, K_NONE, K_NONE},
    [OSC_A64_RET]      = {"ret",   0xD65F0000u, CLS_BREG,   K_NONE, K_X, K_NONE, K_NONE},
    [OSC_A64_BRK]      = {"brk",   0xD4200000u, CLS_BRK,    K_NONE, K_NONE, K_NONE, K_NONE},
    [OSC_A64_LDRB_REG] = {"ldrb",  0x38606800u, CLS_R3,     K_ZR, K_SP, K_ZR, K_NONE},
    /* OSC-4 Slice A (docs/osc/OSC-4-DESIGN.md 1.6 table; S-form bases, D adds bit 22 or bit 30) */
    [OSC_A64_FMOV_RR]  = {"fmov",  0x1E204000u, CLS_FP2,    K_V, K_V, K_NONE, K_NONE},
    [OSC_A64_FMOV_XD]  = {"fmov",  0x9E670000u, CLS_FMOV_GP, K_V, K_ZR, K_NONE, K_NONE},
    [OSC_A64_FMOV_DX]  = {"fmov",  0x9E660000u, CLS_FMOV_GP, K_ZR, K_V, K_NONE, K_NONE},
    [OSC_A64_FMOV_WS]  = {"fmov",  0x1E270000u, CLS_FMOV_GP, K_V, K_ZR, K_NONE, K_NONE},
    [OSC_A64_FMOV_SW]  = {"fmov",  0x1E260000u, CLS_FMOV_GP, K_ZR, K_V, K_NONE, K_NONE},
    [OSC_A64_FADD]     = {"fadd",  0x1E202800u, CLS_FP3,    K_V, K_V, K_V, K_NONE},
    [OSC_A64_FSUB]     = {"fsub",  0x1E203800u, CLS_FP3,    K_V, K_V, K_V, K_NONE},
    [OSC_A64_FMUL]     = {"fmul",  0x1E200800u, CLS_FP3,    K_V, K_V, K_V, K_NONE},
    [OSC_A64_FDIV]     = {"fdiv",  0x1E201800u, CLS_FP3,    K_V, K_V, K_V, K_NONE},
    [OSC_A64_FMAXNM]   = {"fmaxnm",0x1E206800u, CLS_FP3,    K_V, K_V, K_V, K_NONE},
    [OSC_A64_FMINNM]   = {"fminnm",0x1E207800u, CLS_FP3,    K_V, K_V, K_V, K_NONE},
    [OSC_A64_FSQRT]    = {"fsqrt", 0x1E21C000u, CLS_FP2,    K_V, K_V, K_NONE, K_NONE},
    [OSC_A64_FNEG]     = {"fneg",  0x1E214000u, CLS_FP2,    K_V, K_V, K_NONE, K_NONE},
    [OSC_A64_FABS]     = {"fabs",  0x1E20C000u, CLS_FP2,    K_V, K_V, K_NONE, K_NONE},
    [OSC_A64_FCMP]     = {"fcmp",  0x1E202000u, CLS_FCMP,   K_NONE, K_V, K_V, K_NONE},
    [OSC_A64_FCVT_SD]  = {"fcvt",  0x1E624000u, CLS_FCVT_FF, K_V, K_V, K_NONE, K_NONE},
    [OSC_A64_FCVT_DS]  = {"fcvt",  0x1E22C000u, CLS_FCVT_FF, K_V, K_V, K_NONE, K_NONE},
    [OSC_A64_SCVTF_X]  = {"scvtf", 0x9E220000u, CLS_CVT,    K_V, K_ZR, K_NONE, K_NONE},
    [OSC_A64_UCVTF_X]  = {"ucvtf", 0x9E230000u, CLS_CVT,    K_V, K_ZR, K_NONE, K_NONE},
    [OSC_A64_FCVTZS_X] = {"fcvtzs",0x9E380000u, CLS_CVT,    K_ZR, K_V, K_NONE, K_NONE},
    [OSC_A64_FCVTZU_X] = {"fcvtzu",0x9E390000u, CLS_CVT,    K_ZR, K_V, K_NONE, K_NONE},
    [OSC_A64_LDR_F_UOFF] = {"ldr", 0xBD400000u, CLS_FMEM_UOFF, K_V, K_SP, K_NONE, K_NONE},
    [OSC_A64_STR_F_UOFF] = {"str", 0xBD000000u, CLS_FMEM_UOFF, K_V, K_SP, K_NONE, K_NONE},
    [OSC_A64_LDR_F_REG]  = {"ldr", 0xBC607800u, CLS_FMEM_REG,  K_V, K_SP, K_ZR, K_NONE},
    [OSC_A64_STR_F_REG]  = {"str", 0xBC207800u, CLS_FMEM_REG,  K_V, K_SP, K_ZR, K_NONE},
};

/* variable-field mask per class (bits not fixed by the opcode) */
static uint32_t cls_varmask(int cls) {
    switch (cls) {
    case CLS_RRR_SH: return 0x00DFFFFFu;  /* shift 23:22, Rm 20:16, imm6 15:10, Rn, Rd */
    case CLS_RI:     return 0x007FFFFFu;  /* sh 22, imm12 21:10, Rn, Rd */
    case CLS_R4:     return 0x001F7FFFu;  /* Rm, Ra 14:10, Rn, Rd */
    case CLS_MULH:   return 0x001F03FFu;
    case CLS_R3:     return 0x001F03FFu;
    case CLS_BFM:    return 0x003FFFFFu;
    case CLS_CSINC:  return 0x001FF3FFu;  /* Rm, cond 15:12, Rn, Rd */
    case CLS_MOV16:  return 0x007FFFFFu;  /* hw 22:21, imm16 20:5, Rd */
    case CLS_MEM:    return 0x003FFFFFu;
    case CLS_PAIR:   return 0x003FFFFFu;  /* imm7 21:15, Rt2 14:10, Rn, Rt */
    case CLS_B26:    return 0x03FFFFFFu;
    case CLS_BCOND:  return 0x00FFFFEFu;  /* imm19 23:5, cond 3:0 (bit 4 fixed 0) */
    case CLS_CB:     return 0x00FFFFFFu;
    case CLS_BREG:   return 0x000003E0u;
    case CLS_BRK:    return 0x001FFFE0u;
    /* OSC-4 scalar FP */
    case CLS_FP3:    return 0x005F03FFu;  /* type 22, Rm 20:16, Rn, Rd */
    case CLS_FP2:    return 0x004003FFu;  /* type 22, Rn, Rd */
    case CLS_FCMP:   return 0x005F03E0u;  /* type 22, Rm, Rn (Rd field fixed 00000, E bit 4 fixed 0) */
    case CLS_FMOV_GP: return 0x000003FFu; /* Rn, Rd (one fixed width each) */
    case CLS_FCVT_FF: return 0x000003FFu;
    case CLS_CVT:    return 0x004003FFu;  /* type 22, Rn, Rd */
    case CLS_FMEM_UOFF: return 0x403FFFFFu; /* size bit 30, imm12 21:10, Rn, Rt */
    case CLS_FMEM_REG:  return 0x401F03FFu; /* size bit 30, Rm 20:16, Rn, Rt */
    default:         return 0;
    }
}

static int fail(char *err, size_t n, const char *fmt, ...) {
    if (err && n) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, n, fmt, ap);
        va_end(ap);
    }
    return -1;
}

/* map typed operand -> 5-bit field, refusing the wrong kind */
static int regfield(int kind, int r, uint32_t *f, const char *pos, const char *op, char *err, size_t n) {
    if (kind == K_V) { /* OSC-4: V0..V31; the XZR / SP spellings are not V registers */
        if (r >= 0 && r <= 31) { *f = (uint32_t)r; return 0; }
        if (r == OSC_A64_SP) return fail(err, n, "%s: %s cannot be SP (V register expected)", op, pos);
        if (r == OSC_A64_XZR) return fail(err, n, "%s: %s cannot be XZR (V register expected)", op, pos);
        return fail(err, n, "%s: %s V register %d out of range", op, pos, r);
    }
    if (r >= 0 && r <= 30) { *f = (uint32_t)r; return 0; }
    if (kind == K_ZR && r == OSC_A64_XZR) { *f = 31; return 0; }
    if (kind == K_SP && r == OSC_A64_SP) { *f = 31; return 0; }
    if (r == OSC_A64_SP) return fail(err, n, "%s: %s cannot be SP", op, pos);
    if (r == OSC_A64_XZR) return fail(err, n, "%s: %s cannot be XZR", op, pos);
    return fail(err, n, "%s: %s register %d out of range", op, pos, r);
}

static int unreg(int kind, uint32_t f) {
    if (f < 31 || kind == K_V) return (int)f;
    if (kind == K_ZR) return OSC_A64_XZR;
    if (kind == K_SP) return OSC_A64_SP;
    return 255; /* not encodable: re-encode check refuses */
}

static int is_sp(int r) { return r == OSC_A64_SP; }

int osc_a64_encode(const OscA64Insn *in, uint32_t *word, char *err, size_t n) {
    if (!in || !word) return fail(err, n, "null");
    int op = in->op;
    if (op <= OSC_A64_INVALID || op >= OSC_A64_NOPS || !OPS[op].name)
        return fail(err, n, "unknown op %d", op);
    const OpInfo *oi = &OPS[op];
    const char *nm = oi->name;
    uint32_t w = oi->base, rd = 0, rn = 0, rm = 0, ra = 0;
    /* unused register / field positions must be zero (canonical form) */
    if (oi->kd == K_NONE && in->rd) return fail(err, n, "%s: rd unused but set", nm);
    if (oi->kn == K_NONE && in->rn) return fail(err, n, "%s: rn unused but set", nm);
    if (oi->km == K_NONE && in->rm) return fail(err, n, "%s: rm unused but set", nm);
    if (oi->ka == K_NONE && in->ra) return fail(err, n, "%s: ra unused but set", nm);
    if (oi->kd != K_NONE && regfield(oi->kd, in->rd, &rd, "rd", nm, err, n)) return -1;
    if (oi->kn != K_NONE && regfield(oi->kn, in->rn, &rn, "rn", nm, err, n)) return -1;
    if (oi->km != K_NONE && regfield(oi->km, in->rm, &rm, "rm", nm, err, n)) return -1;
    if (oi->ka != K_NONE && regfield(oi->ka, in->ra, &ra, "ra", nm, err, n)) return -1;
    int uses_shift = oi->cls == CLS_RRR_SH;
    int uses_cond = oi->cls == CLS_CSINC || oi->cls == CLS_BCOND;
    int fp_typed = oi->cls == CLS_FP3 || oi->cls == CLS_FP2 || oi->cls == CLS_FCMP || oi->cls == CLS_CVT ||
                   oi->cls == CLS_FMEM_UOFF || oi->cls == CLS_FMEM_REG;
    int uses_imm2 = oi->cls == CLS_RI || oi->cls == CLS_BFM || oi->cls == CLS_MOV16 || fp_typed;
    int uses_imm = !(oi->cls == CLS_R4 || oi->cls == CLS_MULH || oi->cls == CLS_R3 ||
                     oi->cls == CLS_CSINC || oi->cls == CLS_BREG || oi->cls == CLS_FP3 || oi->cls == CLS_FP2 ||
                     oi->cls == CLS_FCMP || oi->cls == CLS_FMOV_GP || oi->cls == CLS_FCVT_FF || oi->cls == CLS_CVT ||
                     oi->cls == CLS_FMEM_REG);
    if (!uses_shift && in->shift) return fail(err, n, "%s: shift unused but set", nm);
    if (!uses_cond && in->cond) return fail(err, n, "%s: cond unused but set", nm);
    if (!uses_imm2 && in->imm2) return fail(err, n, "%s: imm2 unused but set", nm);
    if (!uses_imm && in->imm) return fail(err, n, "%s: imm unused but set", nm);
    int64_t imm = in->imm, imm2 = in->imm2;

    switch (oi->cls) {
    case CLS_RRR_SH:
        if (in->shift > OSC_A64_ASR) return fail(err, n, "%s: shift type %u refused", nm, in->shift);
        if (imm < 0 || imm > 63) return fail(err, n, "%s: shift amount %lld out of range", nm, (long long)imm);
        w |= ((uint32_t)in->shift << 22) | (rm << 16) | ((uint32_t)imm << 10) | (rn << 5) | rd;
        break;
    case CLS_RI:
        if (imm < 0 || imm > 4095) return fail(err, n, "%s: imm12 %lld out of range", nm, (long long)imm);
        if (imm2 != 0 && imm2 != 1) return fail(err, n, "%s: lsl12 flag %lld invalid", nm, (long long)imm2);
        w |= ((uint32_t)imm2 << 22) | ((uint32_t)imm << 10) | (rn << 5) | rd;
        break;
    case CLS_R4:
        w |= (rm << 16) | (ra << 10) | (rn << 5) | rd;
        break;
    case CLS_MULH:
    case CLS_R3:
        w |= (rm << 16) | (rn << 5) | rd;
        break;
    case CLS_BFM:
        if (imm < 0 || imm > 63) return fail(err, n, "%s: immr %lld out of range", nm, (long long)imm);
        if (imm2 < 0 || imm2 > 63) return fail(err, n, "%s: imms %lld out of range", nm, (long long)imm2);
        w |= ((uint32_t)imm << 16) | ((uint32_t)imm2 << 10) | (rn << 5) | rd;
        break;
    case CLS_CSINC:
        if (in->cond > OSC_A64_AL) return fail(err, n, "%s: cond %u refused", nm, in->cond);
        w |= (rm << 16) | ((uint32_t)in->cond << 12) | (rn << 5) | rd;
        break;
    case CLS_MOV16:
        if (imm < 0 || imm > 0xFFFF) return fail(err, n, "%s: imm16 %lld out of range", nm, (long long)imm);
        if (imm2 != 0 && imm2 != 16 && imm2 != 32 && imm2 != 48)
            return fail(err, n, "%s: shift %lld invalid", nm, (long long)imm2);
        w |= ((uint32_t)(imm2 / 16) << 21) | ((uint32_t)imm << 5) | rd;
        break;
    case CLS_MEM:
        if (imm < 0 || imm > 4095 * 8 || (imm & 7))
            return fail(err, n, "%s: offset %lld not a multiple of 8 in 0..32760", nm, (long long)imm);
        w |= ((uint32_t)(imm / 8) << 10) | (rn << 5) | rd;
        break;
    case CLS_PAIR: {
        if (imm < -512 || imm > 504 || (imm & 7))
            return fail(err, n, "%s: offset %lld not a multiple of 8 in -512..504", nm, (long long)imm);
        int wb = op == OSC_A64_STP_PRE || op == OSC_A64_STP_POST || op == OSC_A64_LDP_PRE || op == OSC_A64_LDP_POST;
        int load = op == OSC_A64_LDP_OFF || op == OSC_A64_LDP_PRE || op == OSC_A64_LDP_POST;
        if (wb && !is_sp(in->rn) && (in->rn == in->rd || in->rn == in->ra))
            return fail(err, n, "%s: writeback base equals a transfer register (unpredictable)", nm);
        if (load && in->rd == in->ra)
            return fail(err, n, "%s: Rt == Rt2 (unpredictable)", nm);
        uint32_t i7 = (uint32_t)((imm / 8) & 0x7F);
        w |= (i7 << 15) | (ra << 10) | (rn << 5) | rd;
        break;
    }
    case CLS_B26:
        if ((imm & 3) || imm < -(1LL << 27) || imm > (1LL << 27) - 4)
            return fail(err, n, "%s: offset %lld out of range or misaligned", nm, (long long)imm);
        w |= (uint32_t)((imm / 4) & 0x03FFFFFF);
        break;
    case CLS_BCOND:
        if (in->cond > 13) return fail(err, n, "%s: cond %u refused (AL/NV not used)", nm, in->cond);
        if ((imm & 3) || imm < -(1LL << 20) || imm > (1LL << 20) - 4)
            return fail(err, n, "%s: offset %lld out of range or misaligned", nm, (long long)imm);
        w |= ((uint32_t)((imm / 4) & 0x7FFFF) << 5) | in->cond;
        break;
    case CLS_CB:
        if ((imm & 3) || imm < -(1LL << 20) || imm > (1LL << 20) - 4)
            return fail(err, n, "%s: offset %lld out of range or misaligned", nm, (long long)imm);
        w |= ((uint32_t)((imm / 4) & 0x7FFFF) << 5) | rd;
        break;
    case CLS_BREG:
        w |= rn << 5;
        break;
    case CLS_BRK:
        if (imm < 0 || imm > 0xFFFF) return fail(err, n, "%s: imm16 %lld out of range", nm, (long long)imm);
        w |= (uint32_t)imm << 5;
        break;
    /* OSC-4 scalar FP: imm2 is the type, 0 = S, 1 = D */
    case CLS_FP3:
        if (imm2 != 0 && imm2 != 1) return fail(err, n, "%s: type %lld refused (0 = S, 1 = D)", nm, (long long)imm2);
        w |= ((uint32_t)imm2 << 22) | (rm << 16) | (rn << 5) | rd;
        break;
    case CLS_FP2:
    case CLS_CVT:
        if (imm2 != 0 && imm2 != 1) return fail(err, n, "%s: type %lld refused (0 = S, 1 = D)", nm, (long long)imm2);
        w |= ((uint32_t)imm2 << 22) | (rn << 5) | rd;
        break;
    case CLS_FCMP:
        if (imm2 != 0 && imm2 != 1) return fail(err, n, "%s: type %lld refused (0 = S, 1 = D)", nm, (long long)imm2);
        w |= ((uint32_t)imm2 << 22) | (rm << 16) | (rn << 5);
        break;
    case CLS_FMOV_GP:
    case CLS_FCVT_FF:
        w |= (rn << 5) | rd; /* one fixed form each; imm2 was refused above if set */
        break;
    case CLS_FMEM_UOFF: {
        if (imm2 != 0 && imm2 != 1) return fail(err, n, "%s: type %lld refused (0 = S, 1 = D)", nm, (long long)imm2);
        int64_t sc = imm2 ? 8 : 4;
        if (imm < 0 || imm > 4095 * sc || (imm % sc))
            return fail(err, n, "%s: offset %lld not a multiple of %lld in 0..%lld", nm, (long long)imm, (long long)sc,
                        (long long)(4095 * sc));
        w |= ((uint32_t)imm2 << 30) | ((uint32_t)(imm / sc) << 10) | (rn << 5) | rd;
        break;
    }
    case CLS_FMEM_REG:
        if (imm2 != 0 && imm2 != 1) return fail(err, n, "%s: type %lld refused (0 = S, 1 = D)", nm, (long long)imm2);
        w |= ((uint32_t)imm2 << 30) | (rm << 16) | (rn << 5) | rd;
        break;
    default:
        return fail(err, n, "bad class");
    }
    *word = w;
    return 0;
}

int osc_a64_reencode(const OscA64Insn *in, uint32_t *word) {
    return osc_a64_encode(in, word, NULL, 0);
}

static int64_t sext(uint32_t v, unsigned bits) {
    uint64_t m = 1ULL << (bits - 1);
    return (int64_t)(((uint64_t)v ^ m) - m);
}

int osc_a64_decode(uint32_t w, OscA64Insn *out) {
    for (int op = 1; op < OSC_A64_NOPS; op++) {
        const OpInfo *oi = &OPS[op];
        if (!oi->name) continue;
        if ((w & ~cls_varmask(oi->cls)) != oi->base) continue;
        OscA64Insn d;
        memset(&d, 0, sizeof d);
        d.op = (uint8_t)op;
        uint32_t f_rd = w & 31, f_rn = (w >> 5) & 31, f_rm = (w >> 16) & 31, f_ra = (w >> 10) & 31;
        if (oi->kd != K_NONE) d.rd = (uint8_t)unreg(oi->kd, f_rd);
        if (oi->kn != K_NONE) d.rn = (uint8_t)unreg(oi->kn, f_rn);
        if (oi->km != K_NONE) d.rm = (uint8_t)unreg(oi->km, f_rm);
        if (oi->ka != K_NONE) d.ra = (uint8_t)unreg(oi->ka, f_ra);
        switch (oi->cls) {
        case CLS_RRR_SH: d.shift = (uint8_t)((w >> 22) & 3); d.imm = (w >> 10) & 63; break;
        case CLS_RI: d.imm = (w >> 10) & 0xFFF; d.imm2 = (w >> 22) & 1; break;
        case CLS_BFM: d.imm = (w >> 16) & 63; d.imm2 = (w >> 10) & 63; break;
        case CLS_CSINC: d.cond = (uint8_t)((w >> 12) & 15); break;
        case CLS_MOV16: d.imm = (w >> 5) & 0xFFFF; d.imm2 = ((w >> 21) & 3) * 16; break;
        case CLS_MEM: d.imm = (int64_t)((w >> 10) & 0xFFF) * 8; break;
        case CLS_PAIR: d.imm = sext((w >> 15) & 0x7F, 7) * 8; break;
        case CLS_B26: d.imm = sext(w & 0x03FFFFFF, 26) * 4; break;
        case CLS_BCOND: d.imm = sext((w >> 5) & 0x7FFFF, 19) * 4; d.cond = (uint8_t)(w & 15); break;
        case CLS_CB: d.imm = sext((w >> 5) & 0x7FFFF, 19) * 4; break;
        case CLS_BRK: d.imm = (w >> 5) & 0xFFFF; break;
        /* OSC-4 scalar FP */
        case CLS_FP3: case CLS_FP2: case CLS_FCMP: case CLS_CVT: d.imm2 = (w >> 22) & 1; break;
        case CLS_FMEM_UOFF:
            d.imm2 = (w >> 30) & 1;
            d.imm = (int64_t)((w >> 10) & 0xFFF) * (d.imm2 ? 8 : 4);
            break;
        case CLS_FMEM_REG: d.imm2 = (w >> 30) & 1; break;
        default: break;
        }
        uint32_t re;
        if (osc_a64_encode(&d, &re, NULL, 0) != 0 || re != w) continue;
        if (out) *out = d;
        return 0;
    }
    return -1;
}

const char *osc_a64_op_name(int op) {
    if (op <= 0 || op >= OSC_A64_NOPS || !OPS[op].name) return "?";
    return OPS[op].name;
}

int osc_a64_equal(const OscA64Insn *x, const OscA64Insn *y) {
    return x->op == y->op && x->rd == y->rd && x->rn == y->rn && x->rm == y->rm &&
           x->ra == y->ra && x->shift == y->shift && x->cond == y->cond &&
           x->imm == y->imm && x->imm2 == y->imm2;
}

/* ---- constructors ----------------------------------------------------- */
static OscA64Insn mk(int op) {
    OscA64Insn i;
    memset(&i, 0, sizeof i);
    i.op = (uint8_t)op;
    return i;
}
/* registers are passed as int; values outside uint8 are clamped to 255
 * (never a valid operand) so the encoder refuses them rather than wrapping */
static uint8_t r8(int r) { return (r < 0 || r > 254) ? 255 : (uint8_t)r; }

OscA64Insn osc_a64_r3(OscA64Op op, int rd, int rn, int rm) {
    OscA64Insn i = mk(op); i.rd = r8(rd); i.rn = r8(rn); i.rm = r8(rm); return i;
}
OscA64Insn osc_a64_r3s(OscA64Op op, int rd, int rn, int rm, OscA64Shift sh, int amount) {
    OscA64Insn i = osc_a64_r3(op, rd, rn, rm); i.shift = (uint8_t)sh; i.imm = amount; return i;
}
OscA64Insn osc_a64_ri(OscA64Op op, int rd, int rn, int64_t imm12, int lsl12) {
    OscA64Insn i = mk(op); i.rd = r8(rd); i.rn = r8(rn); i.imm = imm12; i.imm2 = lsl12; return i;
}
OscA64Insn osc_a64_r4(OscA64Op op, int rd, int rn, int rm, int ra) {
    OscA64Insn i = osc_a64_r3(op, rd, rn, rm); i.ra = r8(ra); return i;
}
OscA64Insn osc_a64_bfm(OscA64Op op, int rd, int rn, int immr, int imms) {
    OscA64Insn i = mk(op); i.rd = r8(rd); i.rn = r8(rn); i.imm = immr; i.imm2 = imms; return i;
}
OscA64Insn osc_a64_csinc(int rd, int rn, int rm, OscA64Cond c) {
    OscA64Insn i = osc_a64_r3(OSC_A64_CSINC, rd, rn, rm); i.cond = (uint8_t)c; return i;
}
OscA64Insn osc_a64_mov16(OscA64Op op, int rd, int64_t imm16, int shift) {
    OscA64Insn i = mk(op); i.rd = r8(rd); i.imm = imm16; i.imm2 = shift; return i;
}
OscA64Insn osc_a64_mem(OscA64Op op, int rt, int rn, int64_t off) {
    OscA64Insn i = mk(op); i.rd = r8(rt); i.rn = r8(rn); i.imm = off; return i;
}
OscA64Insn osc_a64_pair(OscA64Op op, int rt, int rt2, int rn, int64_t off) {
    OscA64Insn i = mk(op); i.rd = r8(rt); i.ra = r8(rt2); i.rn = r8(rn); i.imm = off; return i;
}
OscA64Insn osc_a64_br(OscA64Op op, int64_t off) { OscA64Insn i = mk(op); i.imm = off; return i; }
OscA64Insn osc_a64_bcond(OscA64Cond c, int64_t off) {
    OscA64Insn i = mk(OSC_A64_BCOND); i.cond = (uint8_t)c; i.imm = off; return i;
}
OscA64Insn osc_a64_cb(OscA64Op op, int rt, int64_t off) { OscA64Insn i = mk(op); i.rd = r8(rt); i.imm = off; return i; }
OscA64Insn osc_a64_breg(OscA64Op op, int rn) { OscA64Insn i = mk(op); i.rn = r8(rn); return i; }
OscA64Insn osc_a64_brk(int64_t imm16) { OscA64Insn i = mk(OSC_A64_BRK); i.imm = imm16; return i; }

/* OSC-4 floats: dbl lands in imm2 unchanged so the encoder sees and refuses anything but 0 / 1 */
OscA64Insn osc_a64_f3(OscA64Op op, int vd, int vn, int vm, int dbl) {
    OscA64Insn i = osc_a64_r3(op, vd, vn, vm); i.imm2 = dbl; return i;
}
OscA64Insn osc_a64_f2(OscA64Op op, int rd, int rn, int dbl) {
    OscA64Insn i = mk(op); i.rd = r8(rd); i.rn = r8(rn); i.imm2 = dbl; return i;
}
OscA64Insn osc_a64_fcmp(int vn, int vm, int dbl) {
    OscA64Insn i = mk(OSC_A64_FCMP); i.rn = r8(vn); i.rm = r8(vm); i.imm2 = dbl; return i;
}
OscA64Insn osc_a64_fmem(OscA64Op op, int vt, int rn, int64_t off, int dbl) {
    OscA64Insn i = osc_a64_mem(op, vt, rn, off); i.imm2 = dbl; return i;
}
OscA64Insn osc_a64_fmem_reg(OscA64Op op, int vt, int rn, int rm, int dbl) {
    OscA64Insn i = osc_a64_r3(op, vt, rn, rm); i.imm2 = dbl; return i;
}
