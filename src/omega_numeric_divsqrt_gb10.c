/*
 * E1 row 7: correctly rounded FP32 DIV and SQRT realized on the GB10.
 * See omega_numeric_divsqrt_gb10.h for the contract and
 * docs/numeric/E1_DIVSQRT_GB10.md for the algorithm and the evidence.
 *
 * Every pre-submission check carries a CHECK: marker;
 * tools/divsqrt_check_sweep.sh deletes each one in a scratch copy and proves
 * a host test then fails. With -DOMEGA_NUMERIC_CPU_ONLY no device is touched.
 */
#include "omega_numeric_divsqrt_gb10.h"
#include "omega_numeric.h"
#include "omega_numeric_transc_tables.h"
#include "omega_blackwell_encoder.h"
#include "omega_blackwell_qmd.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#ifndef OMEGA_NUMERIC_CPU_ONLY
#include "omega_blackwell_submit.h"
#include "omega_numeric_native.h"
#include "omega_gpu_code_alloc.h"
#endif

/* Recorded after tools/divsqrt_nvdisasm_check.sh matched every word of the
 * emitted kernel against omega_ds_listing (nvdisasm 13.0.85 -b SM121). */
const char *const OMEGA_DS_KERNEL_SHA256[OMEGA_DS_OP_COUNT] = {
    "d847b9bd3ac4fe90405f86a2528ff952d47bdb95679d6d029e52ea7aa526675f", /* DIV, 264 words  */
    "222c9aa5cf7b2d66d48de3ee4b7ce346627e888ab0786713d8986815bdca10d7", /* SQRT, 336 words */
    "e72d5cbf556dce39c83ce7e76335a7eef7eda3ebccd36f250bdddb272db7474c", /* EXP2, 72 words  */
    "14e461ec628b685ea23d197c060f7d35a302c88e794934628eacd6098a44f394", /* LOG2, 576 words */
    "8607f5659b4d42a57fdb4ba2f26427a28d8e6e1497f3daa8529ae5da1f629b70", /* SIGMOID, 344 words */
    "4edc065511340d58d26ed8ef7c1464341b279caccd7abc18b62d2a9880694494", /* TANH, 352 words */
    "0fe5484675cca0667c95a286b65560f204013683aee4e9ad001dbcf1d78cfc10", /* SIN, 120 words */
    "ed7cde5ac1dd3ddccc1b78885a2227600cf1bc4a7d775f27cceb110ed37a2ac4", /* COS, 120 words */
    "db3a51ff06cc24dc749d98dbf91485f0ceb86ba2bfc8cc55e554f62e6f7edff5", /* ERF, 360 words */
    "94992a5fdbe00a7943dac997d6a59ddd6096ac2fec35d4ff286b6552e8cea36d", /* GELU, 424 words */
    "adb2864a0842a36fb9abf144d6654a69668506e2f8e98ac1c7ecb40ca4a1ff85", /* RSQRT, 344 words */
};

const char *omega_ds_op_name(OmegaDsOp op) {
    return op == OMEGA_DS_DIV ? "DIV" : op == OMEGA_DS_SQRT ? "SQRT" : op == OMEGA_DS_EXP2 ? "EXP2" : op == OMEGA_DS_LOG2 ? "LOG2" : op == OMEGA_DS_SIGMOID ? "SIGMOID" : op == OMEGA_DS_TANH ? "TANH" : op == OMEGA_DS_SIN ? "SIN" : op == OMEGA_DS_COS ? "COS" : op == OMEGA_DS_ERF ? "ERF" : op == OMEGA_DS_GELU ? "GELU" : op == OMEGA_DS_RSQRT ? "RSQRT" : "?";
}

/* ---- Forms ---------------------------------------------------------------
 * Opcode (w0 bits 0-11) and the fixed part of w2 per form. Field positions:
 * guard predicate w0 bits 12-15 (7 = PT), Rd w0 16-23, Ra w0 24-31, Rb or
 * the 32-bit immediate in w1 (IADD3 -Rb: w1 bit 31), Rc w2 0-7, IADD3 -Ra
 * w2 bit 8, LOP3 lut w2 8-15, ISETP cmp w2 12-14, signed w2 bit 9,
 * Pd w2 17-19, SEL Ps w2 23-25 and !Ps w2 bit 26. Templates come from
 * ptxas 13.0.88 -O0 oracle output for each class and from single-word probes;
 * the kernel is accepted only after tools/divsqrt_nvdisasm_check.sh shows that
 * nvdisasm 13.0.85 -b SM121 decodes every emitted word to the text
 * omega_ds_format gives for it. */
typedef struct {
    uint8_t kind;
    uint16_t opcode;
    uint32_t w2_fixed;
} DsForm;

static const DsForm FORMS[DSK_COUNT] = {
    { DSK_IADD3_R, 0x210, 0x07ffe000 },
    { DSK_IADD3_I, 0x810, 0x07ffe000 },
    { DSK_LOP3_R,  0x212, 0x078e0000 },
    { DSK_LOP3_I,  0x812, 0x078e0000 },
    { DSK_SHL_I,   0x819, 0x000006ff },
    { DSK_SHL_R,   0x219, 0x000006ff },
    { DSK_SHR_I,   0x819, 0x00011600 },
    { DSK_SHR_R,   0x219, 0x00011600 },
    { DSK_ISETP_R, 0x20c, 0x03f00070 },
    { DSK_ISETP_I, 0x80c, 0x03f00070 },
    { DSK_SEL_R,   0x207, 0x00000000 },
    { DSK_SEL_I,   0x807, 0x00000000 },
    { DSK_I2FP,    0x245, 0x00201400 },
    /* FP forms: the FADD/FSUB/FMUL/FFMA patch words of src/omega_numeric.c
     * (nvdisasm 13.0.88 -b SM121, Gate 5 chip parity), registers generalized. */
    { DSK_FADD_R,  0x221, 0x00000000 },
    { DSK_FMUL_R,  0x220, 0x00400000 },
    { DSK_FFMA_R,  0x223, 0x00000000 },
};

int omega_ds_encode(const OmegaDsInsn *in, uint32_t w[4]) {
    if (!in || !w || in->kind >= DSK_COUNT) return -1;
    const DsForm *f = &FORMS[in->kind];
    uint32_t w0 = f->opcode | 0x7000u, w1 = 0, w2 = f->w2_fixed;
    switch (in->kind) {
    case DSK_IADD3_R:
        w0 |= (uint32_t)in->d << 16 | (uint32_t)in->a << 24;
        w1 = in->b | (in->negb ? 0x80000000u : 0);
        w2 |= in->c | (in->nega ? 0x100u : 0);
        break;
    case DSK_IADD3_I:
        if (in->negb) return -1;
        w0 |= (uint32_t)in->d << 16 | (uint32_t)in->a << 24;
        w1 = in->imm;
        w2 |= in->c | (in->nega ? 0x100u : 0);
        break;
    case DSK_LOP3_R:
        w0 |= (uint32_t)in->d << 16 | (uint32_t)in->a << 24;
        w1 = in->b;
        w2 |= in->c | (uint32_t)in->lut << 8;
        break;
    case DSK_LOP3_I:
        w0 |= (uint32_t)in->d << 16 | (uint32_t)in->a << 24;
        w1 = in->imm;
        w2 |= in->c | (uint32_t)in->lut << 8;
        break;
    case DSK_SHL_I:
        w0 |= (uint32_t)in->d << 16 | (uint32_t)in->a << 24;
        w1 = in->imm;
        break;
    case DSK_SHL_R:
        w0 |= (uint32_t)in->d << 16 | (uint32_t)in->a << 24;
        w1 = in->b;
        break;
    case DSK_SHR_I:
        w0 |= (uint32_t)in->d << 16 | 0xffu << 24;
        w1 = in->imm;
        w2 |= in->c;
        break;
    case DSK_SHR_R:
        w0 |= (uint32_t)in->d << 16 | 0xffu << 24;
        w1 = in->b;
        w2 |= in->c;
        break;
    case DSK_ISETP_R:
    case DSK_ISETP_I:
        if (in->d > 6 || in->cmp < 1 || in->cmp > 6) return -1;
        w0 |= (uint32_t)in->a << 24;
        w1 = in->kind == DSK_ISETP_R ? in->b : in->imm;
        w2 |= (uint32_t)in->cmp << 12 | (in->is_signed ? 0x200u : 0) | (uint32_t)in->d << 17;
        break;
    case DSK_SEL_R:
    case DSK_SEL_I:
        if (in->ps > 6) return -1;
        w0 |= (uint32_t)in->d << 16 | (uint32_t)in->a << 24;
        w1 = in->kind == DSK_SEL_R ? in->b : in->imm;
        w2 |= (uint32_t)in->ps << 23 | (in->pneg ? 0x04000000u : 0);
        break;
    case DSK_I2FP:
        w0 |= (uint32_t)in->d << 16;
        w1 = in->b;
        break;
    case DSK_FADD_R:
        w0 |= (uint32_t)in->d << 16 | (uint32_t)in->a << 24;
        w1 = in->b | (in->negb ? 0x80000000u : 0);
        break;
    case DSK_FMUL_R:
        if (in->negb) return -1;
        w0 |= (uint32_t)in->d << 16 | (uint32_t)in->a << 24;
        w1 = in->b;
        break;
    case DSK_FFMA_R:
        if (in->negb) return -1;
        w0 |= (uint32_t)in->d << 16 | (uint32_t)in->a << 24;
        w1 = in->b;
        w2 |= in->c;
        break;
    default:
        return -1;
    }
    w[0] = w0; w[1] = w1; w[2] = w2; w[3] = OMEGA_DS_BODY_CTRL;
    return 0;
}

int omega_ds_decode(const uint32_t w[4], OmegaDsInsn *out) {
    OmegaDsInsn x;
    memset(&x, 0, sizeof(x));
    uint32_t opc = w[0] & 0xfffu;
    int kind = -1;
    for (int k = 0; k < DSK_COUNT; k++) {
        if (FORMS[k].opcode != opc) continue;
        /* SHF: L and R forms share the opcode; the w2 template tells them apart */
        if ((opc & 0xffu) == 0x19 && (w[2] & 0xffffff00u) != (FORMS[k].w2_fixed & 0xffffff00u)) continue;
        kind = k;
        break;
    }
    if (kind < 0) return -1;
    x.kind = (uint8_t)kind;
    x.d = (uint8_t)(w[0] >> 16);
    x.a = (uint8_t)(w[0] >> 24);
    switch (kind) {
    case DSK_IADD3_R: x.b = (uint8_t)w[1]; x.negb = (w[1] >> 31) & 1; x.c = (uint8_t)w[2]; x.nega = (w[2] >> 8) & 1; break;
    case DSK_IADD3_I: x.imm = w[1]; x.c = (uint8_t)w[2]; x.nega = (w[2] >> 8) & 1; break;
    case DSK_LOP3_R: x.b = (uint8_t)w[1]; x.c = (uint8_t)w[2]; x.lut = (uint8_t)(w[2] >> 8); break;
    case DSK_LOP3_I: x.imm = w[1]; x.c = (uint8_t)w[2]; x.lut = (uint8_t)(w[2] >> 8); break;
    case DSK_SHL_I: x.imm = w[1]; break;
    case DSK_SHL_R: x.b = (uint8_t)w[1]; break;
    case DSK_SHR_I: x.imm = w[1]; x.c = (uint8_t)w[2]; x.a = 0; break;
    case DSK_SHR_R: x.b = (uint8_t)w[1]; x.c = (uint8_t)w[2]; x.a = 0; break;
    case DSK_ISETP_R: case DSK_ISETP_I:
        if (kind == DSK_ISETP_R) x.b = (uint8_t)w[1]; else x.imm = w[1];
        x.cmp = (uint8_t)((w[2] >> 12) & 7); x.is_signed = (w[2] >> 9) & 1; x.d = (uint8_t)((w[2] >> 17) & 7);
        break;
    case DSK_SEL_R: case DSK_SEL_I:
        if (kind == DSK_SEL_R) x.b = (uint8_t)w[1]; else x.imm = w[1];
        x.ps = (uint8_t)((w[2] >> 23) & 7); x.pneg = (w[2] >> 26) & 1;
        break;
    case DSK_I2FP: x.b = (uint8_t)w[1]; x.a = 0; break;
    case DSK_FADD_R: x.b = (uint8_t)w[1]; x.negb = (w[1] >> 31) & 1; break;
    case DSK_FMUL_R: x.b = (uint8_t)w[1]; break;
    case DSK_FFMA_R: x.b = (uint8_t)w[1]; x.c = (uint8_t)w[2]; break;
    }
    uint32_t re[4];
    if (omega_ds_encode(&x, re) != 0) return -1;
    if (re[0] != w[0] || re[1] != w[1] || re[2] != w[2]) return -1;
    if (out) *out = x;
    return 0;
}

static void reg_name(uint8_t r, char *b) {
    if (r == OMEGA_DS_RZ) strcpy(b, "RZ"); else sprintf(b, "R%u", (unsigned)r);
}

static void simm(uint32_t v, char *b) {
    if (v & 0x80000000u) sprintf(b, "-0x%x", (unsigned)(0u - v)); else sprintf(b, "0x%x", (unsigned)v);
}

static const char *cmp_name(uint8_t c) {
    static const char *const N[8] = { "F", "LT", "EQ", "LE", "GT", "NE", "GE", "T" };
    return N[c & 7];
}

int omega_ds_format(const OmegaDsInsn *in, char *buf, size_t len) {
    char d[8], a[8], b[8], c[8], im[16];
    if (!in || !buf || in->kind >= DSK_COUNT) return -1;
    reg_name(in->d, d); reg_name(in->a, a); reg_name(in->b, b); reg_name(in->c, c);
    switch (in->kind) {
    case DSK_IADD3_R:
        snprintf(buf, len, "IADD3 %s, PT, PT, %s%s, %s%s, %s", d, in->nega ? "-" : "", a, in->negb ? "-" : "", b, c); break;
    case DSK_IADD3_I:
        simm(in->imm, im);
        snprintf(buf, len, "IADD3 %s, PT, PT, %s%s, %s, %s", d, in->nega ? "-" : "", a, im, c); break;
    case DSK_LOP3_R:
        snprintf(buf, len, "LOP3.LUT %s, %s, %s, %s, 0x%x, !PT", d, a, b, c, in->lut); break;
    case DSK_LOP3_I:
        snprintf(buf, len, "LOP3.LUT %s, %s, 0x%x, %s, 0x%x, !PT", d, a, (unsigned)in->imm, c, in->lut); break;
    case DSK_SHL_I:
        snprintf(buf, len, "SHF.L.U32 %s, %s, 0x%x, RZ", d, a, (unsigned)in->imm); break;
    case DSK_SHL_R:
        snprintf(buf, len, "SHF.L.U32 %s, %s, %s, RZ", d, a, b); break;
    case DSK_SHR_I:
        snprintf(buf, len, "SHF.R.U32.HI %s, RZ, 0x%x, %s", d, (unsigned)in->imm, c); break;
    case DSK_SHR_R:
        snprintf(buf, len, "SHF.R.U32.HI %s, RZ, %s, %s", d, b, c); break;
    case DSK_ISETP_R:
        snprintf(buf, len, "ISETP.%s%s.AND P%u, PT, %s, %s, PT", cmp_name(in->cmp), in->is_signed ? "" : ".U32",
                 (unsigned)in->d, a, b); break;
    case DSK_ISETP_I:
        simm(in->imm, im);
        snprintf(buf, len, "ISETP.%s%s.AND P%u, PT, %s, %s, PT", cmp_name(in->cmp), in->is_signed ? "" : ".U32",
                 (unsigned)in->d, a, im); break;
    case DSK_SEL_R:
        snprintf(buf, len, "SEL %s, %s, %s, %sP%u", d, a, b, in->pneg ? "!" : "", (unsigned)in->ps); break;
    case DSK_SEL_I:
        snprintf(buf, len, "SEL %s, %s, 0x%x, %sP%u", d, a, (unsigned)in->imm, in->pneg ? "!" : "", (unsigned)in->ps); break;
    case DSK_I2FP:
        snprintf(buf, len, "I2FP.F32.S32 %s, %s", d, b); break;
    case DSK_FADD_R:
        snprintf(buf, len, "FADD %s, %s, %s%s", d, a, in->negb ? "-" : "", b); break;
    case DSK_FMUL_R:
        snprintf(buf, len, "FMUL %s, %s, %s", d, a, b); break;
    case DSK_FFMA_R:
        snprintf(buf, len, "FFMA %s, %s, %s, %s", d, a, b, c); break;
    }
    return 0;
}

/* ---- Body builder ---------------------------------------------------------- */

typedef struct { OmegaDsInsn *v; size_t n, max; int bad; } Bld;

static void emit(Bld *B, OmegaDsInsn x) {
    if (B->n >= B->max) { B->bad = 1; return; }
    B->v[B->n++] = x;
}
#define RZ OMEGA_DS_RZ
static void iadd(Bld *B, int d, int a, int b, int c) { emit(B, (OmegaDsInsn){ .kind = DSK_IADD3_R, .d = d, .a = a, .b = b, .c = c }); }
static void isub(Bld *B, int d, int a, int b) { emit(B, (OmegaDsInsn){ .kind = DSK_IADD3_R, .d = d, .a = a, .b = b, .c = RZ, .negb = 1 }); }
static void iaddi(Bld *B, int d, int a, int32_t imm, int c) { emit(B, (OmegaDsInsn){ .kind = DSK_IADD3_I, .d = d, .a = a, .imm = (uint32_t)imm, .c = c }); }
static void rsubi(Bld *B, int d, int a, int32_t imm) { emit(B, (OmegaDsInsn){ .kind = DSK_IADD3_I, .d = d, .a = a, .imm = (uint32_t)imm, .c = RZ, .nega = 1 }); }
static void lop(Bld *B, int d, int a, int b, int c, int lut) { emit(B, (OmegaDsInsn){ .kind = DSK_LOP3_R, .d = d, .a = a, .b = b, .c = c, .lut = lut }); }
static void lopi(Bld *B, int d, int a, uint32_t imm, int lut) { emit(B, (OmegaDsInsn){ .kind = DSK_LOP3_I, .d = d, .a = a, .imm = imm, .c = RZ, .lut = lut }); }
static void shli(Bld *B, int d, int a, uint32_t s) { emit(B, (OmegaDsInsn){ .kind = DSK_SHL_I, .d = d, .a = a, .imm = s, .b = 0, .c = RZ }); }
static void shlr(Bld *B, int d, int a, int s) { emit(B, (OmegaDsInsn){ .kind = DSK_SHL_R, .d = d, .a = a, .b = s, .c = RZ }); }
static void shri(Bld *B, int d, int c, uint32_t s) { emit(B, (OmegaDsInsn){ .kind = DSK_SHR_I, .d = d, .a = 0, .imm = s, .c = c }); }
static void shrr(Bld *B, int d, int c, int s) { emit(B, (OmegaDsInsn){ .kind = DSK_SHR_R, .d = d, .a = 0, .b = s, .c = c }); }
static void setp(Bld *B, int p, int cmp, int sgn, int a, int b) { emit(B, (OmegaDsInsn){ .kind = DSK_ISETP_R, .d = p, .a = a, .b = b, .cmp = cmp, .is_signed = sgn }); }
static void setpi(Bld *B, int p, int cmp, int sgn, int a, uint32_t imm) { emit(B, (OmegaDsInsn){ .kind = DSK_ISETP_I, .d = p, .a = a, .imm = imm, .cmp = cmp, .is_signed = sgn }); }
static void sel(Bld *B, int d, int a, int b, int p, int neg) { emit(B, (OmegaDsInsn){ .kind = DSK_SEL_R, .d = d, .a = a, .b = b, .ps = p, .pneg = neg }); }
static void seli(Bld *B, int d, int a, uint32_t imm, int p, int neg) { emit(B, (OmegaDsInsn){ .kind = DSK_SEL_I, .d = d, .a = a, .imm = imm, .ps = p, .pneg = neg }); }
static void i2fp(Bld *B, int d, int s) { emit(B, (OmegaDsInsn){ .kind = DSK_I2FP, .d = d, .a = 0, .b = s }); }

#define LUT_AND 0xc0   /* a & b      */
#define LUT_OR  0xfc   /* a | b      */
#define LUT_XOR 0x3c   /* a ^ b      */
#define LUT_A_AND_B_OR_C 0xe0 /* a & (b | c) */

/* Registers. R2 = a bits and R5 = b bits from the prologue; R6:R7 is the
 * output address and R1 is never written; the result goes to R9. */
enum { A_ = 2, B_ = 5, AX = 8, BY = 10, SGN = 11, MA = 12, EA = 13, MB = 14, EB = 15,
       T0 = 16, T1 = 17, T2 = 18, REM = 19, Q = 20, BIT = 21, EXP2 = 22, PB = 23, BE = 24,
       S = 25, MANT = 26, T3 = 27, RES = 9 };

/* Finite nonzero |x| (in ax) = m * 2^e, m in [2^23, 2^24) (omega_unpack).
 * Subnormal: the leading bit of ax comes from I2FP (ax < 2^23, exact). */
static void unpack(Bld *B, int ax, int m, int e) {
    shri(B, T0, ax, 23);                    /* biased exponent             */
    lopi(B, m, ax, 0x007fffffu, LUT_AND);
    lopi(B, m, m, 0x00800000u, LUT_OR);     /* normal: f | 2^23            */
    iaddi(B, e, T0, -150, RZ);              /* normal: be - 150            */
    i2fp(B, T1, ax);
    shri(B, T1, T1, 23);                    /* 127 + msb(ax)               */
    rsubi(B, T2, T1, 150);                  /* n = 23 - msb                */
    shlr(B, T2, ax, T2);                    /* subnormal m = ax << n       */
    iaddi(B, T1, T1, -299, RZ);             /* subnormal e = -149 - n      */
    setp(B, 0, DS_CMP_EQ, 0, T0, RZ);
    sel(B, m, T2, m, 0, 0);
    sel(B, e, T1, e, 0, 0);
}

/* omega_round_pack(sign, sig, exp2, sticky) for sig in [2^25, 2^27):
 * result into RES. sticky != 0 means nonzero bits below sig. */
static void round_pack(Bld *B, int sign, int sig, int exp2, int sticky) {
    setpi(B, 2, DS_CMP_GE, 0, sig, 0x04000000u);
    seli(B, PB, RZ, 1, 2, 1);               /* p - 25                       */
    iaddi(B, BE, exp2, 152, PB);            /* be = p + exp2 + 127          */
    iaddi(B, T0, PB, 2, RZ);                /* normal: s = p - 23           */
    rsubi(B, T1, exp2, -149);               /* subnormal: s = -149 - exp2   */
    setp(B, 3, DS_CMP_GT, 1, BE, RZ);       /* P3: be >= 1                  */
    sel(B, S, T0, T1, 3, 0);
    iaddi(B, T2, PB, 26, RZ);               /* p + 1                        */
    setp(B, 4, DS_CMP_GT, 1, S, T2);        /* P4: s > p + 1 -> zero        */
    seli(B, S, S, 2, 4, 1);                 /* keep shifts in range         */
    shrr(B, MANT, sig, S);                  /* mant = sig >> s              */
    iaddi(B, T0, S, -1, RZ);                /* s - 1 in [1, 27]             */
    shrr(B, T1, sig, T0);
    lopi(B, T1, T1, 1, LUT_AND);            /* guard                        */
    rsubi(B, T2, T0, 32);
    shlr(B, T2, sig, T2);                   /* bits below the guard, on top */
    lop(B, T2, T2, sticky, RZ, LUT_OR);
    setp(B, 5, DS_CMP_NE, 0, T2, RZ);
    seli(B, T2, RZ, 1, 5, 1);               /* rest                         */
    lop(B, T1, T1, T2, MANT, LUT_A_AND_B_OR_C); /* guard & (rest | lsb)     */
    iadd(B, MANT, MANT, T1, RZ);
    iaddi(B, T3, BE, -1, RZ);
    shli(B, T3, T3, 23);
    sel(B, T3, T3, RZ, 3, 0);               /* normal: (be - 1) << 23       */
    iadd(B, T3, T3, MANT, RZ);
    setpi(B, 6, DS_CMP_GE, 0, T3, 0x7f800000u);
    seli(B, T3, T3, 0x7f800000u, 6, 1);     /* rounded up to infinity       */
    setpi(B, 6, DS_CMP_GT, 1, BE, 254);
    seli(B, T3, T3, 0x7f800000u, 6, 1);     /* be >= 255                    */
    sel(B, T3, RZ, T3, 4, 0);               /* below half the min subnormal */
    lop(B, RES, T3, sign, RZ, LUT_OR);
}

/* Correctly rounded ra / rb into RES (omega_math_div). Reads ra and rb only
 * in its first three instructions; writes R8, R9 (RES), R10-R27 and P0-P6. */
static void div_core(Bld *B, int ra, int rb) {
    lopi(B, AX, ra, 0x7fffffffu, LUT_AND);
    lopi(B, BY, rb, 0x7fffffffu, LUT_AND);
    lop(B, SGN, ra, rb, RZ, LUT_XOR);
    lopi(B, SGN, SGN, 0x80000000u, LUT_AND);
    unpack(B, AX, MA, EA);
    unpack(B, BY, MB, EB);
    /* restoring long division, 27 quotient bits (omega_math_div) */
    for (int i = 0; i < 27; i++) {
        isub(B, T0, MA, MB);
        setp(B, 1, DS_CMP_GE, 0, MA, MB);
        sel(B, MA, T0, MA, 1, 0);
        seli(B, BIT, RZ, 1, 1, 1);
        if (i == 0) iadd(B, Q, RZ, RZ, BIT); else iadd(B, Q, Q, Q, BIT);
        iadd(B, MA, MA, MA, RZ);
    }
    isub(B, T0, EA, EB);
    iaddi(B, EXP2, T0, -26, RZ);
    round_pack(B, SGN, Q, EXP2, MA);
    /* special operands, lowest priority first (omega_math_div order) */
    setp(B, 0, DS_CMP_EQ, 0, AX, RZ);
    sel(B, RES, SGN, RES, 0, 0);                    /* 0 / y          */
    setpi(B, 0, DS_CMP_EQ, 0, BY, 0x7f800000u);
    sel(B, RES, SGN, RES, 0, 0);                    /* x / inf        */
    lopi(B, T0, SGN, 0x7f800000u, LUT_OR);
    setp(B, 0, DS_CMP_EQ, 0, BY, RZ);
    sel(B, RES, T0, RES, 0, 0);                     /* x / 0          */
    setpi(B, 0, DS_CMP_EQ, 0, AX, 0x7f800000u);
    sel(B, RES, T0, RES, 0, 0);                     /* inf / y        */
    lop(B, T0, AX, BY, RZ, LUT_OR);
    setp(B, 0, DS_CMP_EQ, 0, T0, RZ);
    seli(B, RES, RES, 0x7fc00000u, 0, 1);           /* 0 / 0          */
    lop(B, T0, AX, BY, RZ, LUT_AND);
    setpi(B, 0, DS_CMP_EQ, 0, T0, 0x7f800000u);
    seli(B, RES, RES, 0x7fc00000u, 0, 1);           /* inf / inf (or NaN) */
    setpi(B, 0, DS_CMP_GT, 0, AX, 0x7f800000u);
    seli(B, RES, RES, 0x7fc00000u, 0, 1);           /* NaN / y        */
    setpi(B, 0, DS_CMP_GT, 0, BY, 0x7f800000u);
    seli(B, RES, RES, 0x7fc00000u, 0, 1);           /* x / NaN        */
}

static void body_div(Bld *B) { div_core(B, A_, B_); }

/* ---- Transcendentals (E1 row 10) -------------------------------------------
 * Each body issues the frozen sequence of src/omega_numeric_transc.c in its
 * written order. FP constants are loaded with IADD3 Rd, RZ, imm, RZ from
 * their exact binary32 bit patterns (written below as the same hex literals
 * the CPU source uses). fneg(v) is LOP3 v ^ 0x80000000 as on the CPU. */
static void ffadd(Bld *B, int d, int a, int b) { emit(B, (OmegaDsInsn){ .kind = DSK_FADD_R, .d = d, .a = a, .b = b }); }
static void ffsub(Bld *B, int d, int a, int b) { emit(B, (OmegaDsInsn){ .kind = DSK_FADD_R, .d = d, .a = a, .b = b, .negb = 1 }); }
static void ffmul(Bld *B, int d, int a, int b) { emit(B, (OmegaDsInsn){ .kind = DSK_FMUL_R, .d = d, .a = a, .b = b }); }
static void fffma(Bld *B, int d, int a, int b, int c) { emit(B, (OmegaDsInsn){ .kind = DSK_FFMA_R, .d = d, .a = a, .b = b, .c = c }); }
static void movi(Bld *B, int d, uint32_t imm) { iaddi(B, d, RZ, (int32_t)imm, RZ); }
static void movf(Bld *B, int d, float v) { movi(B, d, omega_float_to_bits(v)); }
static void mov(Bld *B, int d, int s) { iadd(B, d, s, RZ, RZ); }
/* p = ffma(p, f, c) with c loaded into tmp first. */
static void horner(Bld *B, int p, int f, float c, int tmp) { movf(B, tmp, c); fffma(B, p, p, f, tmp); }

/* scale2(m, k) of omega_numeric_transc.c into out: all three branches are
 * computed, then chosen (k > 127 first, else k < -126, else the plain one).
 * Writes t, q, ra, rb, rc and P1. */
static void scale2_core(Bld *B, int out, int m, int k, int t, int q, int ra, int rb, int rc) {
    iaddi(B, t, k, 127, RZ);
    shli(B, t, t, 23);                      /* pow2i(k)                     */
    ffmul(B, rc, m, t);
    movi(B, t, 0x7f000000u);                /* pow2i(127)                   */
    ffmul(B, q, m, t);
    shli(B, t, k, 23);                      /* pow2i(k - 127)               */
    ffmul(B, ra, q, t);
    iaddi(B, t, k, 253, RZ);
    shli(B, t, t, 23);                      /* pow2i(k + 126)               */
    ffmul(B, q, m, t);
    movi(B, t, 0x00800000u);                /* pow2i(-126)                  */
    ffmul(B, rb, q, t);
    setpi(B, 1, DS_CMP_LT, 1, k, (uint32_t)-126);
    sel(B, out, rb, rc, 1, 0);
    setpi(B, 1, DS_CMP_GT, 1, k, 127);
    sel(B, out, ra, out, 1, 0);
}

/* omega_math_exp2. x in R2. */
static void body_exp2(Bld *B) {
    enum { X = 2, AX_ = 8, C = 10, T = 11, KF = 12, K = 13, F = 14, P = 15, M = 16,
           U = 17, Q2 = 18, RA = 19, RB = 20, RC = 21, T4 = 22 };
    movi(B, C, 0x4b400000u);                /* MAGIC = 1.5 * 2^23           */
    ffadd(B, T, X, C);                      /* t = x + MAGIC                */
    ffsub(B, KF, T, C);                     /* kf = t - MAGIC               */
    isub(B, K, T, C);                       /* k = tb(t) - tb(MAGIC)        */
    ffsub(B, F, X, KF);                     /* f = x - kf                   */
    movf(B, P, 0xB16011p-43f);
    horner(B, P, F, 0xFFE5FEp-40f, C);
    horner(B, P, F, 0xA18489p-36f, C);
    horner(B, P, F, 0xAEC3FFp-33f, C);
    horner(B, P, F, 0x9D955Bp-30f, C);
    horner(B, P, F, 0xE35847p-28f, C);
    horner(B, P, F, 0xF5FDF0p-26f, C);
    horner(B, P, F, 0xB17218p-24f, C);
    movf(B, C, 1.0f);
    fffma(B, M, P, F, C);                   /* m = ffma(p, f, 1)            */
    scale2_core(B, RES, M, K, U, Q2, RA, RB, RC);
    /* special inputs, lowest priority first */
    lopi(B, AX_, X, 0x7fffffffu, LUT_AND);
    setpi(B, 0, DS_CMP_GT, 0, AX_, 0x43170000u);   /* |x| > 151           */
    setpi(B, 2, DS_CMP_LT, 1, X, 0);               /* sign bit set        */
    sel(B, T4, RZ, RES, 0, 0);
    sel(B, RES, T4, RES, 2, 0);                    /* x < -151: +0        */
    setpi(B, 0, DS_CMP_GE, 1, X, 0x43000000u);
    seli(B, RES, RES, 0x7f800000u, 0, 1);          /* x >= 128: +inf      */
    setpi(B, 0, DS_CMP_GT, 0, AX_, 0x7f800000u);
    seli(B, RES, RES, 0x7fc00000u, 0, 1);          /* NaN                 */
}

/* omega_math_log2. x in R2, kept in R3; R0, R4, R5, R28-R31 hold the values
 * that live across the two divisions (div_core writes R8-R27). */
static void body_log2(Bld *B) {
    enum { X = 3, Z = 0, NZ = 4, RZL = 5, NUM = 28, DEN = 29, DL = 30, E = 31, ZL = 2 };
    mov(B, X, A_);
    /* subnormal: x * 2^23 (exact), eadj = -23 */
    movi(B, 10, 0x4b000000u);
    ffmul(B, 11, A_, 10);
    lopi(B, 12, A_, 0x7f800000u, LUT_AND);
    setp(B, 0, DS_CMP_EQ, 0, 12, RZ);
    sel(B, 13, 11, A_, 0, 0);                      /* u                    */
    seli(B, 14, RZ, (uint32_t)-23, 0, 1);          /* eadj                 */
    shri(B, 15, 13, 23);
    lopi(B, 15, 15, 0xffu, LUT_AND);
    iaddi(B, E, 15, -127, 14);                     /* e                    */
    lopi(B, 16, 13, 0x007fffffu, LUT_AND);
    lopi(B, 16, 16, 0x3f800000u, LUT_OR);          /* m in [1, 2)          */
    setpi(B, 1, DS_CMP_GT, 0, 16, omega_float_to_bits(0xB504F3p-23f));
    movf(B, 17, 0.5f);
    ffmul(B, 18, 16, 17);
    sel(B, 16, 18, 16, 1, 0);                      /* m > sqrt2: m * 0.5   */
    seli(B, 19, RZ, 1, 1, 1);
    iadd(B, E, E, 19, RZ);                         /*            e + 1     */
    movf(B, 20, 1.0f);
    ffsub(B, NUM, 16, 20);                         /* num = m - 1          */
    ffadd(B, DEN, 16, 20);                         /* two_sum(m, 1): den   */
    ffsub(B, 21, DEN, 16);                         /* bv                   */
    ffsub(B, 22, DEN, 21);                         /* av                   */
    ffsub(B, 23, 16, 22);                          /* m - av               */
    ffsub(B, 24, 20, 21);                          /* 1 - bv               */
    ffadd(B, DL, 23, 24);                          /* dl                   */
    div_core(B, NUM, DEN);                         /* z = num / den        */
    mov(B, Z, RES);
    lopi(B, NZ, Z, 0x80000000u, LUT_XOR);          /* -z                   */
    fffma(B, RZL, NZ, DEN, NUM);                   /* rz = -z den + num    */
    fffma(B, RZL, NZ, DL, RZL);                    /* rz = -z dl + rz      */
    div_core(B, RZL, DEN);                         /* zl = rz / den        */
    mov(B, ZL, RES);
    movf(B, 10, 0xB8AA3Bp-22f);                    /* C_HI = 2/ln2         */
    ffmul(B, 11, Z, Z);                            /* z2                   */
    movf(B, 12, 0x864D42p-25f);
    horner(B, 12, 11, 0xA4258Ap-25f, 13);
    horner(B, 12, 11, 0xD30BB1p-25f, 13);
    horner(B, 12, 11, 0x93BB63p-24f, 13);
    horner(B, 12, 11, 0xF6384Fp-24f, 13);          /* p                    */
    ffmul(B, 14, Z, 11);                           /* z3 = z * z2          */
    ffmul(B, 15, Z, 10);                           /* ph = z * C_HI        */
    lopi(B, 16, 15, 0x80000000u, LUT_XOR);
    fffma(B, 17, Z, 10, 16);                       /* pl = z C_HI - ph     */
    ffmul(B, 18, ZL, 10);                          /* zl * C_HI            */
    movf(B, 19, 0xA57060p-48f);                    /* C_LO                 */
    fffma(B, 18, Z, 19, 18);                       /* z C_LO + zl C_HI     */
    fffma(B, 20, 14, 12, 18);                      /* lo = z3 p + ...      */
    i2fp(B, 21, E);                                /* (float)e, exact      */
    ffadd(B, 22, 21, 15);                          /* two_sum(e, ph): s    */
    ffsub(B, 23, 22, 21);                          /* bv                   */
    ffsub(B, 24, 22, 23);                          /* av                   */
    ffsub(B, 25, 21, 24);                          /* e - av               */
    ffsub(B, 26, 15, 23);                          /* ph - bv              */
    ffadd(B, 27, 25, 26);                          /* se                   */
    ffadd(B, 25, 17, 20);                          /* pl + lo              */
    ffadd(B, 26, 27, 25);                          /* se + (pl + lo)       */
    ffadd(B, RES, 22, 26);                         /* s + (...)            */
    /* special inputs, lowest priority first (omega_math_log2 order) */
    lopi(B, 8, X, 0x7fffffffu, LUT_AND);
    setpi(B, 0, DS_CMP_EQ, 0, X, 0x7f800000u);
    seli(B, RES, RES, 0x7f800000u, 0, 1);          /* +inf                 */
    setpi(B, 0, DS_CMP_LT, 1, X, 0);
    seli(B, RES, RES, 0x7fc00000u, 0, 1);          /* x < 0, incl. -inf    */
    setp(B, 0, DS_CMP_EQ, 0, 8, RZ);
    seli(B, RES, RES, 0xff800000u, 0, 1);          /* +-0: -inf            */
    setpi(B, 0, DS_CMP_GT, 0, 8, 0x7f800000u);
    seli(B, RES, RES, 0x7fc00000u, 0, 1);          /* NaN                  */
}


/* exp_core(ah, +0) of omega_numeric_transc.c: e^ah = m * 2^k (Cody-Waite,
 * degree-8 Horner). ah must not be one of R10-R17 (read after they are
 * written).
 * Writes m, k and R10-R17. */
static void exp_core_body(Bld *B, int ah, int m, int k) {
    enum { C = 10, T = 11, KF = 12, NKF = 13, R = 14, P = 15, MG = 16, ZERO = 17 };
    movf(B, C, 0xB8AA3Bp-23f);              /* INVLN2                       */
    ffmul(B, T, ah, C);
    movi(B, MG, 0x4b400000u);               /* MAGIC                        */
    ffadd(B, T, T, MG);                     /* t = ah INVLN2 + MAGIC        */
    ffsub(B, KF, T, MG);                    /* kf = t - MAGIC               */
    isub(B, k, T, MG);                      /* k = tb(t) - tb(MAGIC)        */
    lopi(B, NKF, KF, 0x80000000u, LUT_XOR); /* -kf                          */
    movf(B, C, 0xB17218p-24f);              /* LN2_HI                       */
    fffma(B, R, NKF, C, ah);                /* r = -kf LN2_HI + ah          */
    movf(B, C, -0x82E308p-52f);             /* LN2_LO                       */
    fffma(B, R, NKF, C, R);                 /* r = -kf LN2_LO + r           */
    movi(B, ZERO, 0);
    ffadd(B, R, R, ZERO);                   /* r = r + al (al = +0)         */
    movf(B, P, 0xD00D01p-39f);              /* 1/8!                         */
    horner(B, P, R, 0xD00D01p-36f, C);
    horner(B, P, R, 0xB60B61p-33f, C);
    horner(B, P, R, 0x888889p-30f, C);
    horner(B, P, R, 0xAAAAABp-28f, C);
    horner(B, P, R, 0xAAAAABp-26f, C);
    horner(B, P, R, 0.5f, C);
    horner(B, P, R, 1.0f, C);
    movf(B, C, 1.0f);
    fffma(B, m, P, R, C);                   /* m = ffma(p, r, 1)            */
}

/* omega_math_sigmoid. x in R2 (never written). m, k, t, d and 1.0 live
 * across the division in R28-R31, R0, R3, R4. */
static void body_sigmoid(Bld *B) {
    enum { X = 2, M = 28, K = 29, TT = 30, ONE = 31, D = 0, Q = 3, NUMR = 4 };
    lopi(B, 10, X, 0x80000000u, LUT_OR);           /* fneg(fabs(x))        */
    mov(B, 18, 10);
    exp_core_body(B, 18, M, K);
    scale2_core(B, TT, M, K, 10, 11, 12, 13, 14);  /* t = scale2(m, k)     */
    movf(B, ONE, 1.0f);
    ffadd(B, D, ONE, TT);                          /* d = 1 + t            */
    setpi(B, 0, DS_CMP_LT, 1, X, 0);               /* sign bit set         */
    sel(B, NUMR, M, ONE, 0, 0);                    /* x < 0: m, else 1     */
    div_core(B, NUMR, D);                          /* q = num / d          */
    mov(B, Q, RES);
    scale2_core(B, NUMR, Q, K, 10, 11, 12, 13, 14);/* x < 0: scale2(q, k)  */
    setpi(B, 0, DS_CMP_LT, 1, X, 0);
    sel(B, RES, NUMR, Q, 0, 0);
    /* special inputs, lowest priority first */
    lopi(B, AX, X, 0x7fffffffu, LUT_AND);
    setpi(B, 0, DS_CMP_GE, 0, AX, 0x42d00000u);    /* |x| >= 104           */
    setpi(B, 2, DS_CMP_LT, 1, X, 0);               /* sign bit set         */
    sel(B, 12, RZ, RES, 0, 0);
    sel(B, RES, 12, RES, 2, 0);                    /* x <= -104: +0        */
    setpi(B, 0, DS_CMP_GE, 1, X, 0x41900000u);
    seli(B, RES, RES, 0x3f800000u, 0, 1);          /* x >= 18: 1           */
    setpi(B, 0, DS_CMP_GT, 0, AX, 0x7f800000u);
    seli(B, RES, RES, 0x7fc00000u, 0, 1);          /* NaN                  */
}

/* omega_math_tanh. x in R2 (never written); sign, |x| and the series
 * result live across the division in R0, R3, R4. */
static void body_tanh(Bld *B) {
    enum { X = 2, SG = 0, AXX = 3, RP = 4, M = 28, K = 29, DEN = 30, TWO = 31 };
    lopi(B, SG, X, 0x80000000u, LUT_AND);
    lopi(B, AXX, X, 0x7fffffffu, LUT_AND);
    /* |x| < 0.5625: odd Taylor series */
    ffmul(B, 10, AXX, AXX);                        /* x2                   */
    movf(B, 11, 0xCB3F0Cp-37f);
    horner(B, 11, 10, -0xFABEBCp-36f, 12);
    horner(B, 11, 10, 0x9AAC12p-34f, 12);
    horner(B, 11, 10, -0xBED1B2p-33f, 12);
    horner(B, 11, 10, 0xEB69E8p-32f, 12);
    horner(B, 11, 10, -0x91371Bp-30f, 12);
    horner(B, 11, 10, 0xB327A4p-29f, 12);
    horner(B, 11, 10, -0xDD0DD1p-28f, 12);
    horner(B, 11, 10, 0x888889p-26f, 12);
    horner(B, 11, 10, -0xAAAAABp-25f, 12);
    ffmul(B, 13, AXX, 10);                         /* ax x2                */
    fffma(B, RP, 13, 11, AXX);                     /* r = ax x2 p + ax     */
    /* otherwise: 1 - 2/(e^(2|x|) + 1) */
    ffadd(B, 18, AXX, AXX);
    exp_core_body(B, 18, M, K);
    scale2_core(B, 19, M, K, 10, 11, 12, 13, 14);  /* e2                   */
    movf(B, 10, 1.0f);
    ffadd(B, DEN, 19, 10);                         /* e2 + 1               */
    movf(B, TWO, 2.0f);
    div_core(B, TWO, DEN);
    movf(B, 10, 1.0f);
    ffsub(B, 11, 10, RES);                         /* 1 - 2/(e2+1)         */
    setpi(B, 0, DS_CMP_LT, 0, AXX, 0x3f100000u);   /* |x| < 0.5625         */
    sel(B, 11, RP, 11, 0, 0);
    setpi(B, 0, DS_CMP_GE, 0, AXX, 0x41180000u);   /* |x| >= 9.5: 1        */
    seli(B, 11, 11, 0x3f800000u, 0, 1);
    lop(B, RES, 11, SG, RZ, LUT_OR);               /* r | sign             */
    setpi(B, 0, DS_CMP_GT, 0, AXX, 0x7f800000u);
    seli(B, RES, RES, 0x7fc00000u, 0, 1);          /* NaN                  */
}
static void body_sqrt(Bld *B) {
    lopi(B, AX, A_, 0x7fffffffu, LUT_AND);
    unpack(B, AX, MA, EA);
    lopi(B, T0, EA, 1, LUT_AND);
    shlr(B, MB, MA, T0);                    /* mm: exponent made even      */
    isub(B, EA, EA, T0);
    /* digit-by-digit square root of M = mm * 2^28 (27 bit pairs, the first
     * 13 from mm as a 26-bit number, then 14 zero pairs): s = floor(sqrt M),
     * REM = M - s^2 < 2^28. */
    for (int i = 0; i < 27; i++) {
        if (i < 13) {
            shri(B, T0, MB, (uint32_t)(24 - 2 * i));
            lopi(B, T0, T0, 3, LUT_AND);
            if (i == 0) iadd(B, REM, T0, RZ, RZ);
            else { shli(B, REM, REM, 2); iadd(B, REM, REM, T0, RZ); }
        } else {
            shli(B, REM, REM, 2);
        }
        if (i == 0) iaddi(B, T1, RZ, 1, RZ);
        else { shli(B, T1, S, 2); lopi(B, T1, T1, 1, LUT_OR); }   /* 4s + 1 */
        setp(B, 1, DS_CMP_GE, 0, REM, T1);
        isub(B, T2, REM, T1);
        sel(B, REM, T2, REM, 1, 0);
        seli(B, BIT, RZ, 1, 1, 1);
        if (i == 0) iadd(B, S, RZ, RZ, BIT); else iadd(B, S, S, S, BIT);
    }
    /* exp2 = e / 2 - 14 (e even, >= -172) */
    iaddi(B, T0, EA, 512, RZ);
    shri(B, T0, T0, 1);
    iaddi(B, EXP2, T0, -270, RZ);
    /* round_pack writes T0..T3, PB, BE, MANT; sig = S, sticky = REM */
    {
        /* S is overwritten inside round_pack (shift amount); copy sig first */
        iadd(B, Q, S, RZ, RZ);
        round_pack(B, RZ, Q, EXP2, REM);
    }
    setpi(B, 0, DS_CMP_EQ, 0, A_, 0x7f800000u);
    seli(B, RES, RES, 0x7f800000u, 0, 1);           /* +inf           */
    setpi(B, 0, DS_CMP_GT, 0, A_, 0x7f800000u);
    seli(B, RES, RES, 0x7fc00000u, 0, 1);           /* NaN, x < 0     */
    setp(B, 0, DS_CMP_EQ, 0, AX, RZ);
    sel(B, RES, A_, RES, 0, 0);                     /* +-0            */
}


/* ---- SIN / COS (E1 row 10) -------------------------------------------------
 * sincos_core of omega_numeric_transc.c: the reduction x = k pi/2 + (rh + rl) is
 * computed for every input, the |x| <= pi/4 branch is chosen with a predicate,
 * both polynomials run, and the quadrant picks one and its sign. x in R2. */
static void two_sum_b(Bld *B, int s, int e, int a, int b, int t1, int t2) {
    ffadd(B, s, a, b);                      /* x = a + b                    */
    ffsub(B, t1, s, a);                     /* bv = x - a                   */
    ffsub(B, t2, s, t1);                    /* av = x - bv                  */
    ffsub(B, t2, a, t2);                    /* a - av                       */
    ffsub(B, t1, b, t1);                    /* b - bv                       */
    ffadd(B, e, t2, t1);
}

static void body_sincos(Bld *B, int qoff) {
    enum { X = 2, AX_ = 3, C = 4, T = 10, KF = 11, K = 12, NKF = 13, R1 = 14, TH = 15, TL = 16,
           S1 = 17, E1 = 18, S2 = 19, E2 = 20, LO = 21, RH = 22, RL = 23, U = 24, V = 25,
           MG = 26, Q = 27, R2 = 28, SP = 29, CP = 30, W = 31, HR = 32, EH = 33, R2L = 34, NR = 35 };
    lopi(B, AX_, X, 0x7fffffffu, LUT_AND);
    /* reduction (computed for every input; used when |x| > pi/4) */
    movf(B, C, 0xA2F983p-24f);              /* 2/pi                         */
    ffmul(B, T, X, C);
    movi(B, MG, 0x4b400000u);               /* MAGIC                        */
    ffadd(B, T, T, MG);
    ffsub(B, KF, T, MG);                    /* kf                           */
    isub(B, K, T, MG);                      /* k                            */
    lopi(B, NKF, KF, 0x80000000u, LUT_XOR); /* -kf                          */
    movf(B, C, 0xC90FDBp-23f);              /* P1                           */
    fffma(B, R1, NKF, C, X);                /* r1 = -kf P1 + x              */
    movf(B, C, -0xBBBD2Ep-48f);             /* P2                           */
    ffmul(B, TH, KF, C);                    /* th = kf P2                   */
    lopi(B, U, TH, 0x80000000u, LUT_XOR);   /* -th                          */
    fffma(B, TL, KF, C, U);                 /* tl = kf P2 - th              */
    two_sum_b(B, S1, E1, R1, U, V, W);      /* (s1, e1) = r1 + (-th)        */
    lopi(B, U, TL, 0x80000000u, LUT_XOR);   /* -tl                          */
    two_sum_b(B, S2, E2, S1, U, V, W);      /* (s2, e2) = s1 + (-tl)        */
    movf(B, C, -0xF72CEDp-73f);             /* P3                           */
    ffmul(B, T, KF, C);                     /* kf P3                        */
    ffadd(B, U, E1, E2);
    ffsub(B, LO, U, T);                     /* lo = (e1 + e2) - kf P3       */
    ffadd(B, RH, S2, LO);                   /* rh                           */
    ffsub(B, U, RH, S2);
    ffsub(B, RL, LO, U);                    /* rl = lo - (rh - s2)          */
    /* |x| <= pi/4 (0xC90FDBp-24 rounded): k = 0, rh = x, rl = 0 */
    setpi(B, 0, DS_CMP_LE, 0, AX_, omega_float_to_bits(0xC90FDBp-24f));
    sel(B, RH, X, RH, 0, 0);
    sel(B, RL, RZ, RL, 0, 0);
    sel(B, K, RZ, K, 0, 0);
    iaddi(B, Q, K, qoff, RZ);
    lopi(B, Q, Q, 3, LUT_AND);              /* q = (k + qoff) & 3           */
    /* sin_poly(rh, rl) -> SP */
    ffmul(B, R2, RH, RH);                   /* r2                           */
    movf(B, SP, 0xB09231p-56f);
    horner(B, SP, R2, -0xD7322Bp-49f, C);
    horner(B, SP, R2, 0xB8EF1Dp-42f, C);
    horner(B, SP, R2, -0xD00D01p-36f, C);
    horner(B, SP, R2, 0x888889p-30f, C);
    horner(B, SP, R2, -0xAAAAABp-26f, C);
    ffmul(B, T, RH, R2);                    /* t = rh r2                    */
    movf(B, C, -0.5f);
    ffmul(B, U, R2, C);                     /* r2 (-0.5)                    */
    fffma(B, U, U, RL, RL);                 /* rlc = (r2 -0.5) rl + rl      */
    fffma(B, U, T, SP, U);                  /* t S + rlc                    */
    ffadd(B, SP, RH, U);                    /* sin                          */
    /* cos_poly(rh, rl) -> CP */
    lopi(B, NR, R2, 0x80000000u, LUT_XOR);
    fffma(B, R2L, RH, RH, NR);              /* r2l = rh rh - r2 (exact)     */
    movf(B, CP, -0xC9CBA5p-60f);
    horner(B, CP, R2, 0x8F76C7p-52f, C);
    horner(B, CP, R2, -0x93F27Ep-45f, C);
    horner(B, CP, R2, 0xD00D01p-39f, C);
    horner(B, CP, R2, -0xB60B61p-33f, C);
    horner(B, CP, R2, 0xAAAAABp-28f, C);
    movf(B, C, 0.5f);
    ffmul(B, HR, R2, C);                    /* hr = r2 0.5                  */
    movf(B, C, 1.0f);
    ffsub(B, U, C, HR);                     /* h = 1 - hr                   */
    ffsub(B, V, C, U);
    ffsub(B, EH, V, HR);                    /* eh = (1 - h) - hr            */
    movf(B, C, -0.5f);
    fffma(B, V, R2L, C, EH);                /* c1 = r2l (-0.5) + eh         */
    lopi(B, W, RL, 0x80000000u, LUT_XOR);   /* -rl                          */
    fffma(B, V, W, RH, V);                  /* c2 = (-rl) rh + c1           */
    ffmul(B, T, R2, R2);                    /* r4                           */
    fffma(B, V, T, CP, V);                  /* r4 C + c2                    */
    ffadd(B, CP, U, V);                     /* cos = h + (...)              */
    /* quadrant: odd q -> cos, even -> sin; q & 2 flips the sign */
    lopi(B, T, Q, 1, LUT_AND);
    setpi(B, 0, DS_CMP_NE, 0, T, 0);
    sel(B, U, CP, SP, 0, 0);
    lopi(B, V, U, 0x80000000u, LUT_XOR);
    lopi(B, T, Q, 2, LUT_AND);
    setpi(B, 1, DS_CMP_NE, 0, T, 0);
    sel(B, RES, V, U, 1, 0);
    /* special inputs, lowest priority first */
    setpi(B, 0, DS_CMP_GT, 0, AX_, 0x4a800000u /* 2^22 */);
    seli(B, RES, RES, 0x7fc00000u, 0, 1);   /* NaN, +-inf, |x| > 2^22: NaN  */
    if (qoff == 0) {
        setp(B, 0, DS_CMP_EQ, 0, AX_, RZ);
        sel(B, RES, X, RES, 0, 0);          /* sin(+-0) = +-0               */
    }
}
static void body_sin(Bld *B) { body_sincos(B, 0); }
static void body_cos(Bld *B) { body_sincos(B, 1); }

/* ---- ERF / GELU (E1 row 10) ------------------------------------------------
 * Shared pieces of omega_math_erf and omega_math_gelu (omega_numeric_transc.c),
 * issued in the written order. The 17-interval erfcx table is chosen by a
 * chain of ISETP/SEL (the largest j with y >= ERFCX_LO[j] wins, the order of
 * the CPU's while loop); the constants come from omega_numeric_transc_tables.h,
 * the same header the CPU sequence reads. */

/* exp_core2(ah, al) -> m, k, ml. Temps R10-R17; ah and al must not be there. */
static void exp_core2_body(Bld *B, int ah, int al, int m, int k, int ml) {
    enum { C = 10, T = 11, KF = 12, NKF = 13, R = 14, P = 15, MG = 16, Q = 17 };
    movf(B, C, 0xB8AA3Bp-23f);              /* INVLN2                       */
    ffmul(B, T, ah, C);
    movi(B, MG, 0x4b400000u);               /* MAGIC                        */
    ffadd(B, T, T, MG);
    ffsub(B, KF, T, MG);                    /* kf                           */
    isub(B, k, T, MG);                      /* k = tb(t) - tb(MAGIC)        */
    lopi(B, NKF, KF, 0x80000000u, LUT_XOR); /* -kf                          */
    movf(B, C, 0xB17218p-24f);              /* LN2_HI                       */
    fffma(B, R, NKF, C, ah);
    movf(B, C, -0x82E308p-52f);             /* LN2_LO                       */
    fffma(B, R, NKF, C, R);
    ffadd(B, R, R, al);                     /* r += al                      */
    movf(B, P, 0xD00D01p-39f);              /* 1/8!                         */
    horner(B, P, R, 0xD00D01p-36f, C);
    horner(B, P, R, 0xB60B61p-33f, C);
    horner(B, P, R, 0x888889p-30f, C);
    horner(B, P, R, 0xAAAAABp-28f, C);
    horner(B, P, R, 0xAAAAABp-26f, C);
    horner(B, P, R, 0.5f, C);
    horner(B, P, R, 1.0f, C);
    movf(B, C, 1.0f);
    fffma(B, m, P, R, C);                   /* m = ffma(p, r, 1)            */
    ffsub(B, Q, C, m);                      /* 1 - m (exact)                */
    fffma(B, ml, P, R, Q);                  /* ml = ffma(p, r, 1 - m)       */
}

/* erf_small(y) -> out (|y| < 0.5 and tiny y). Temps R10-R16. */
static void erf_small_body(Bld *B, int y, int out) {
    enum { Y2 = 10, Q = 11, W = 12, A = 13, NA = 14, AL = 15, BB = 16, TMP = 17 };
    ffmul(B, Y2, y, y);
    movf(B, Q, -0xDDEBBDp-40f);             /* n = 7                        */
    horner(B, Q, Y2, 0xE00E01p-37f, TMP);
    horner(B, Q, Y2, -0xC6980Cp-34f, TMP);
    horner(B, Q, Y2, 0x97B426p-31f, TMP);
    horner(B, Q, Y2, -0xC30C31p-29f, TMP);
    horner(B, Q, Y2, 0xCCCCCDp-27f, TMP);
    horner(B, Q, Y2, -0xAAAAABp-25f, TMP);  /* n = 1: -1/3                  */
    ffmul(B, W, Y2, Q);                     /* w = y2 q                     */
    movf(B, TMP, 0x906EBBp-23f);            /* TSP_HI                       */
    ffmul(B, A, y, TMP);
    lopi(B, NA, A, 0x80000000u, LUT_XOR);
    fffma(B, AL, y, TMP, NA);               /* Al = y TSP_HI - A            */
    movf(B, TMP, -0xFBD649p-48f);           /* TSP_LO                       */
    fffma(B, BB, y, TMP, AL);               /* B = y TSP_LO + Al            */
    fffma(B, TMP, A, W, BB);                /* A w + B                      */
    ffadd(B, out, A, TMP);
}

/* Chooses the erfcx interval of y (>= 0) into the table registers
 * A[n] = R10 + n (n = 0..10), A0LO = R21, CC = R22; P0 is clobbered. */
static void erfcx_table_body(Bld *B, int y) {
    enum { A0 = 10, A0LO = 21, CC = 22 };
    for (int n = 0; n <= 10; n++) movf(B, A0 + n, ERFCX_A[0][n]);
    movf(B, A0LO, ERFCX_A0LO[0]);
    movf(B, CC, ERFCX_C[0]);
    for (int j = 1; j < ERFCX_N; j++) {
        setpi(B, 0, DS_CMP_GE, 0, y, omega_float_to_bits(ERFCX_LO[j]));
        for (int n = 0; n <= 10; n++) seli(B, A0 + n, A0 + n, omega_float_to_bits(ERFCX_A[j][n]), 0, 1);
        seli(B, A0LO, A0LO, omega_float_to_bits(ERFCX_A0LO[j]), 0, 1);
        seli(B, CC, CC, omega_float_to_bits(ERFCX_C[j]), 0, 1);
    }
}

/* erfcx_eval2(y) -> c, cl from the table registers (see erfcx_table_body).
 * t and p are scratch; writes c, cl, t, p. */
static void erfcx_eval_body(Bld *B, int y, int c, int cl, int t, int p, int want_cl) {
    enum { A0 = 10, A0LO = 21, CC = 22 };
    ffsub(B, t, y, CC);                     /* t = y - centre (exact)       */
    fffma(B, p, A0 + 10, t, A0 + 9);
    for (int n = 8; n >= 1; n--) fffma(B, p, p, t, A0 + n);
    fffma(B, c, p, t, A0);                  /* c = p t + a0                 */
    if (want_cl) {
        ffsub(B, cl, A0, c);                /* a0 - c                       */
        fffma(B, cl, p, t, cl);
        ffadd(B, cl, cl, A0LO);
    }
}

/* omega_math_erf. x in R2. */
static void body_erf(Bld *B) {
    enum { X = 2, AXR = 3, SG = 4, SMALLR = 23, H = 24, NH = 25, HL = 26, NHL = 27, M = 28, K = 29, ML = 30,
           T = 31, P = 32, C = 33, MC = 34, S = 35, ONE = 36, BIGR = 37 };
    lopi(B, AXR, X, 0x7fffffffu, LUT_AND);
    lopi(B, SG, X, 0x80000000u, LUT_AND);
    erf_small_body(B, AXR, SMALLR);
    ffmul(B, H, AXR, AXR);                  /* h = ax ax                    */
    lopi(B, NH, H, 0x80000000u, LUT_XOR);
    fffma(B, HL, AXR, AXR, NH);             /* hl = ax ax - h (exact)       */
    lopi(B, NHL, HL, 0x80000000u, LUT_XOR);
    exp_core2_body(B, NH, NHL, M, K, ML);   /* e^-(h + hl) = m 2^k          */
    erfcx_table_body(B, AXR);
    erfcx_eval_body(B, AXR, C, 0, T, P, 0);
    ffmul(B, MC, M, C);                     /* erfc = m erfcx               */
    scale2_core(B, S, MC, K, 10, 11, 12, 13, 14);
    movf(B, ONE, 1.0f);
    ffsub(B, BIGR, ONE, S);                 /* 1 - erfc                     */
    setpi(B, 0, DS_CMP_LT, 0, AXR, 0x3f000000u);   /* |x| < 0.5            */
    sel(B, RES, SMALLR, BIGR, 0, 0);
    setpi(B, 0, DS_CMP_GE, 0, AXR, 0x40800000u);   /* |x| >= 4: 1          */
    seli(B, RES, RES, 0x3f800000u, 0, 1);
    lop(B, RES, RES, SG, RZ, LUT_OR);       /* r | sign                     */
    setpi(B, 0, DS_CMP_GT, 0, AXR, 0x7f800000u);
    seli(B, RES, RES, 0x7fc00000u, 0, 1);   /* NaN                          */
}

/* omega_math_gelu (erf form). x in R2. Every path is computed, then the
 * CPU's branch order is replayed with selects, lowest priority first. */
static void body_gelu(Bld *B) {
    enum { X = 2, AXR = 3, SG = 4, Y = 23, AY = 24, HX = 25, SMALLR = 26, P = 27, PL = 28, H = 29, HL = 30,
           NH = 31, NHL = 32, ME = 33, K = 34, MEL = 35, T = 36, PP = 37, C = 38, CL = 39, U = 40, V = 41,
           W = 42, LARGE = 43, S = 44 };
    lopi(B, AXR, X, 0x7fffffffu, LUT_AND);
    lopi(B, SG, X, 0x80000000u, LUT_AND);
    movf(B, U, 0xB504F3p-24f);              /* INV_SQRT2                    */
    ffmul(B, Y, X, U);                      /* y = x INV_SQRT2              */
    lopi(B, AY, Y, 0x7fffffffu, LUT_AND);
    movf(B, V, 0.5f);
    ffmul(B, HX, X, V);                     /* hx = x 0.5                   */
    /* ay < 0.5: x/2 + x/2 erf(y) */
    erf_small_body(B, AY, V);               /* e = erf_small(ay)            */
    lopi(B, W, Y, 0x80000000u, LUT_AND);
    lop(B, V, V, W, RZ, LUT_OR);            /* e | sign(y)                  */
    fffma(B, W, HX, V, HX);                 /* r = hx e + hx                */
    lop(B, SMALLR, W, SG, RZ, LUT_OR);      /* r | sign(x)                  */
    /* large path */
    ffmul(B, P, X, X);                      /* P = x x                      */
    lopi(B, U, P, 0x80000000u, LUT_XOR);
    fffma(B, PL, X, X, U);                  /* Pl = x x - P                 */
    movf(B, U, 0.5f);
    ffmul(B, H, P, U);                      /* h = P 0.5                    */
    ffmul(B, HL, PL, U);                    /* hl = Pl 0.5                  */
    lopi(B, NH, H, 0x80000000u, LUT_XOR);
    lopi(B, NHL, HL, 0x80000000u, LUT_XOR);
    exp_core2_body(B, NH, NHL, ME, K, MEL); /* e^(-y^2) ~ (me + mel) 2^k    */
    erfcx_table_body(B, AY);
    erfcx_eval_body(B, AY, C, CL, T, PP, 1);/* erfcx(|y|) ~ c + cl          */
    /* yl = x/sqrt2 - y to first order; ayl = sign(x) ? -yl : yl */
    movf(B, U, 0xB504F3p-24f);
    lopi(B, V, Y, 0x80000000u, LUT_XOR);
    fffma(B, W, X, U, V);                   /* yl = x INV_SQRT2 - y (exact) */
    movf(B, U, 0xCFE77Ap-50f);              /* INV_SQRT2_LO                 */
    fffma(B, W, X, U, W);                   /* yl += x INV_SQRT2_LO         */
    setpi(B, 1, DS_CMP_NE, 0, SG, 0);       /* P1: x negative               */
    lopi(B, V, W, 0x80000000u, LUT_XOR);
    sel(B, W, V, W, 1, 0);                  /* ayl                          */
    /* dc = ffma(ay 2, c, -TSP_HI); cl = ffma(ayl, dc, cl) */
    movf(B, U, 2.0f);
    ffmul(B, V, AY, U);
    movf(B, U, -0x906EBBp-23f);
    fffma(B, V, V, C, U);                   /* dc                           */
    fffma(B, CL, W, V, CL);
    /* p = me c; pl = ffma(me, c, -p) + me cl + mel c */
    ffmul(B, PP, ME, C);
    lopi(B, U, PP, 0x80000000u, LUT_XOR);
    fffma(B, V, ME, C, U);                  /* pl = me c - p                */
    fffma(B, V, ME, CL, V);
    fffma(B, V, MEL, C, V);                 /* pl                           */
    /* x >= 0: ffma(-hx, scale2(p, k), x) */
    scale2_core(B, S, PP, K, 10, 11, 12, 13, 14);
    lopi(B, U, HX, 0x80000000u, LUT_XOR);   /* -hx                          */
    fffma(B, LARGE, U, S, X);
    /* x < 0: scale2(ffma(hx, p, hx pl), k) */
    ffmul(B, W, HX, V);                     /* hx pl                        */
    fffma(B, W, HX, PP, W);                 /* hx p + hx pl                 */
    scale2_core(B, S, W, K, 10, 11, 12, 13, 14);
    setpi(B, 1, DS_CMP_NE, 0, SG, 0);
    sel(B, LARGE, S, LARGE, 1, 0);          /* x < 0 -> negative form       */
    /* branches, lowest priority first */
    setpi(B, 0, DS_CMP_LT, 0, AY, 0x3f000000u);    /* ay < 0.5             */
    sel(B, RES, SMALLR, LARGE, 0, 0);
    /* |x| < 2^-125: integer only, tie broken toward +inf */
    iaddi(B, V, AXR, 1, RZ);
    shri(B, V, V, 1);                       /* (m + 1) >> 1                 */
    shri(B, W, AXR, 1);                     /* m >> 1                       */
    setpi(B, 1, DS_CMP_NE, 0, SG, 0);
    sel(B, V, W, V, 1, 0);                  /* sign: m >> 1, else (m+1) >> 1 */
    lop(B, V, V, SG, RZ, LUT_OR);
    setpi(B, 0, DS_CMP_LT, 0, AXR, 0x01000000u);
    sel(B, RES, V, RES, 0, 0);
    /* x < -15.5: -0 ; x >= 8: x ; zero is covered by the tiny branch */
    setpi(B, 0, DS_CMP_GT, 0, AXR, 0x41780000u);   /* |x| > 15.5           */
    movi(B, W, 0x80000000u);
    sel(B, V, W, RES, 0, 0);
    setpi(B, 1, DS_CMP_NE, 0, SG, 0);
    sel(B, RES, V, RES, 1, 0);              /* ... and x negative: -0       */
    setpi(B, 0, DS_CMP_GE, 1, X, 0x41000000u);
    sel(B, RES, X, RES, 0, 0);              /* x >= 8: x (signed compare)   */
    setpi(B, 0, DS_CMP_GT, 0, AXR, 0x7f800000u);
    seli(B, RES, RES, 0x7fc00000u, 0, 1);   /* NaN                          */
}

/* ---- RSQRT (E1 row 10) ---------------------------------------------------------
 * Correctly rounded 1/sqrt(x), bit-identical to omega_math_rsqrt. The CPU tier
 * decides the final rounding with a 128-bit integer midpoint test (mid_below);
 * this frame has no integer multiply, so the same decision is made with
 * binary32 FMA error-free transforms.
 *
 * |x| (positive, finite, nonzero) is normalized to x' in [1,4) (a subnormal by
 * an exact x 2^24, the parity of the exponent folded into x') so that
 * y = 1/sqrt(x') lies in (0.5, 1]; the result is y 2^k (2^-12 more for a
 * subnormal input), both exact FMULs.
 *   1. y0: magic-number seed, three FMA Newton steps, one FMA residual
 *      correction. y0 is within one ulp of the correctly rounded y.
 *   2. mu = y0 + 2^-25 and ml = y0 - 2^-25 are at most the midpoints to the
 *      neighbours of y0 (the gap on either side of y0 in [0.5, 1] is 2^-24 or
 *      2^-23 above 1... never below 2^-24), and 1/sqrt(x') = 1 only for x' = 1.
 *      The correctly rounded result is y0 + [mu^2 x' < 1] + [ml^2 x' < 1] - 1.
 *   3. The sign of m^2 x' - 1 is exact. For m = y0 + s 2^-25 it is the sign of
 *        (x' y0^2 - 1) + s x' y0 2^-24 + x' 2^-50.
 *      Products are split by FMA into (hi, lo) with no rounding error, the
 *      terms are summed exactly into a nonoverlapping expansion (Shewchuk
 *      GROW-EXPANSION over Knuth TwoSum) and the sign of an expansion is the
 *      sign of its highest nonzero component. No midpoint equals 1/sqrt(x'),
 *      so no expansion is zero.
 * The host model equals the CPU tier on every 32-bit input
 * (test_omega_numeric_transc_gb10 --host-all RSQRT). */
static void grow_expansion(Bld *B, const int *e, int n, int b, int qa, int qb, int t1, int t2) {
    /* e[0..n-1] += b exactly; e[0..n] on return (e[n] the highest component).
     * b is read but never written. */
    int q = b;
    for (int i = 0; i < n; i++) {
        int s = (i == n - 1) ? e[n] : (q == qa ? qb : qa);
        two_sum_b(B, s, e[i], q, e[i], t1, t2);
        q = s;
    }
}

/* out := the highest nonzero component of e[0..n-1] (its sign bit is the sign). */
static void expansion_sign(Bld *B, const int *e, int n, int out, int t) {
    mov(B, out, e[0]);
    for (int i = 1; i < n; i++) {
        shli(B, t, e[i], 1);
        setpi(B, 0, DS_CMP_NE, 0, t, 0);
        sel(B, out, e[i], out, 0, 0);
    }
}

static void body_rsqrt(Bld *B) {
    enum { AXR = 8, XB = 10, BE = 11, PAR = 12, X = 13, NX = 14, SC = 15, POST = 16, Y = 17,
           T = 18, NTS = 19, NTL = 20, E = 21, H = 22, ONE = 23, HALF = 24, P = 25, PL = 26, HH = 27,
           HL = 28, GG = 29, GL = 30, Q = 31, QL = 32, G = 33, TS = 34, TL = 35, W = 36,
           SU = 44, SL = 45 };
    const int SH[5] = { 37, 38, 39, 40, 41 };
    const int WK[7] = { 25, 26, 27, 28, 29, 30, 31 };
    const int T1 = 10, T2 = 11, QA = 12, QB = 14;   /* TwoSum temps, free after the setup */
    lopi(B, AXR, A_, 0x7fffffffu, LUT_AND);
    /* subnormal: x 2^24 exact; XB = normal float bits */
    movi(B, T, 0x4b800000u);
    ffmul(B, E, AXR, T);
    setpi(B, 0, DS_CMP_LT, 0, AXR, 0x00800000u);
    sel(B, XB, E, AXR, 0, 0);
    movi(B, T, 0x45800000u);
    seli(B, POST, T, 0x3f800000u, 0, 0);    /* 2^12 for a subnormal, else 1 */
    shri(B, BE, XB, 23);
    iaddi(B, T, BE, 1, RZ);
    lopi(B, PAR, T, 1, LUT_AND);            /* 1 when the exponent is even  */
    lopi(B, X, XB, 0x007fffffu, LUT_AND);
    lopi(B, X, X, 0x3f800000u, LUT_OR);
    shli(B, T, PAR, 23);
    iadd(B, X, X, T, RZ);                   /* x' in [1,4)                  */
    isub(B, T, BE, PAR);
    iaddi(B, T, T, 129, RZ);
    shri(B, T, T, 1);
    rsubi(B, T, T, 255);
    shli(B, SC, T, 23);                     /* 2^-(e' / 2)                  */
    lopi(B, NX, X, 0x80000000u, LUT_XOR);
    movf(B, ONE, 1.0f);
    movf(B, HALF, 0.5f);
    /* y0 */
    shri(B, T, X, 1);
    rsubi(B, Y, T, 0x5f3759dfu);
    for (int k = 0; k < 3; k++) {
        ffmul(B, T, Y, Y);
        fffma(B, E, NX, T, ONE);            /* 1 - x y^2                    */
        ffmul(B, H, Y, HALF);
        fffma(B, Y, H, E, Y);
    }
    ffmul(B, G, X, Y);
    ffmul(B, H, Y, HALF);
    lopi(B, T, G, 0x80000000u, LUT_XOR);
    fffma(B, E, T, H, HALF);                /* 1/2 - g h                    */
    fffma(B, G, G, E, G);
    fffma(B, H, H, E, H);
    ffadd(B, Y, H, H);
    /* error-free terms of x' (y0 + s 2^-25)^2 - 1 */
    ffmul(B, P, Y, Y);
    lopi(B, T, P, 0x80000000u, LUT_XOR);
    fffma(B, PL, Y, Y, T);                  /* y0^2 = P + PL                */
    ffmul(B, HH, X, P);
    lopi(B, T, HH, 0x80000000u, LUT_XOR);
    fffma(B, HL, X, P, T);
    ffmul(B, GG, X, PL);
    lopi(B, T, GG, 0x80000000u, LUT_XOR);
    fffma(B, GL, X, PL, T);
    ffsub(B, SH[0], HH, ONE);               /* HH - 1, exact (Sterbenz)     */
    ffmul(B, Q, X, Y);
    lopi(B, T, Q, 0x80000000u, LUT_XOR);
    fffma(B, QL, X, Y, T);                  /* x y0 = Q + QL                */
    movf(B, T, 0x1p-24f);
    ffmul(B, TS, Q, T);
    ffmul(B, TL, QL, T);
    movf(B, T, 0x1p-50f);
    ffmul(B, W, X, T);
    lopi(B, NTS, TS, 0x80000000u, LUT_XOR);
    lopi(B, NTL, TL, 0x80000000u, LUT_XOR);
    grow_expansion(B, SH, 1, HL, QA, QB, T1, T2);
    grow_expansion(B, SH, 2, GG, QA, QB, T1, T2);
    grow_expansion(B, SH, 3, GL, QA, QB, T1, T2);
    grow_expansion(B, SH, 4, W, QA, QB, T1, T2);
    /* mu: + s x y0 2^-24 with s = +1; r > mu exactly when the sum is negative */
    for (int i = 0; i < 5; i++) mov(B, WK[i], SH[i]);
    grow_expansion(B, WK, 5, TS, QA, QB, T1, T2);
    grow_expansion(B, WK, 6, TL, QA, QB, T1, T2);
    expansion_sign(B, WK, 7, SU, T);
    shri(B, SU, SU, 31);                    /* 1: round up                  */
    /* ml: s = -1; r < ml exactly when the sum is positive */
    for (int i = 0; i < 5; i++) mov(B, WK[i], SH[i]);
    grow_expansion(B, WK, 5, NTS, QA, QB, T1, T2);
    grow_expansion(B, WK, 6, NTL, QA, QB, T1, T2);
    expansion_sign(B, WK, 7, SL, T);
    shri(B, SL, SL, 31);                    /* 1: ml is not below r         */
    iadd(B, T, Y, SU, SL);
    iaddi(B, T, T, -1, RZ);                 /* y0 + up + (1 - down) - 1     */
    ffmul(B, T, T, SC);
    ffmul(B, RES, T, POST);
    /* special inputs, lowest priority first */
    setpi(B, 0, DS_CMP_GE, 0, A_, 0x80000000u);
    seli(B, RES, RES, 0x7fc00000u, 0, 1);   /* x < 0: NaN                   */
    lopi(B, T, A_, 0x7f800000u, LUT_OR);
    setp(B, 0, DS_CMP_EQ, 0, AXR, RZ);
    sel(B, RES, T, RES, 0, 0);              /* +-0: +-inf                   */
    setpi(B, 0, DS_CMP_EQ, 0, A_, 0x7f800000u);
    sel(B, RES, RZ, RES, 0, 0);             /* +inf: +0                     */
    setpi(B, 0, DS_CMP_GT, 0, AXR, 0x7f800000u);
    seli(B, RES, RES, 0x7fc00000u, 0, 1);   /* NaN                          */
}
size_t omega_ds_body(OmegaDsOp op, OmegaDsInsn *out, size_t max) {
    Bld B = { out, 0, max, 0 };
    if (!out) return 0;
    if (op == OMEGA_DS_DIV) body_div(&B);
    else if (op == OMEGA_DS_SQRT) body_sqrt(&B);
    else if (op == OMEGA_DS_EXP2) body_exp2(&B);
    else if (op == OMEGA_DS_LOG2) body_log2(&B);
    else if (op == OMEGA_DS_SIGMOID) body_sigmoid(&B);
    else if (op == OMEGA_DS_TANH) body_tanh(&B);
    else if (op == OMEGA_DS_SIN) body_sin(&B);
    else if (op == OMEGA_DS_COS) body_cos(&B);
    else if (op == OMEGA_DS_ERF) body_erf(&B);
    else if (op == OMEGA_DS_GELU) body_gelu(&B);
    else if (op == OMEGA_DS_RSQRT) body_rsqrt(&B);
    else return 0;
    return B.bad ? 0 : B.n;
}
#undef RZ

/* ---- Host model ------------------------------------------------------------ */

static uint32_t lut3(uint32_t a, uint32_t b, uint32_t c, uint8_t lut) {
    uint32_t r = 0;
    for (int i = 0; i < 8; i++) {
        if (!((lut >> i) & 1)) continue;
        r |= ((i & 4) ? a : ~a) & ((i & 2) ? b : ~b) & ((i & 1) ? c : ~c);
    }
    return r;
}

static bool icmp(uint8_t cmp, bool sgn, uint32_t a, uint32_t b) {
    int lt = sgn ? ((int32_t)a < (int32_t)b) : (a < b);
    int eq = a == b;
    switch (cmp) {
    case DS_CMP_LT: return lt;
    case DS_CMP_EQ: return eq;
    case DS_CMP_LE: return lt || eq;
    case DS_CMP_GT: return !lt && !eq;
    case DS_CMP_NE: return !eq;
    case DS_CMP_GE: return !lt;
    }
    return false;
}


/* FADD / FMUL / FFMA on the host: the AArch64 binary32 instructions (round to
 * nearest even, subnormals kept: FPCR is left at its Linux default), any NaN
 * result replaced by 0x7fffffff, the canonical NaN the GB10 writes. */
#if !defined(__aarch64__)
#error "the host model of the FP forms issues AArch64 FADD/FMUL/FMADD"
#endif
static uint32_t hfp(int kind, uint32_t a, uint32_t b, uint32_t c) {
    float x = omega_bits_to_float(a), y = omega_bits_to_float(b), z = omega_bits_to_float(c), r;
    if (kind == 0) __asm__ volatile("fadd %s0, %s1, %s2" : "=w"(r) : "w"(x), "w"(y));
    else if (kind == 1) __asm__ volatile("fmul %s0, %s1, %s2" : "=w"(r) : "w"(x), "w"(y));
    else __asm__ volatile("fmadd %s0, %s1, %s2, %s3" : "=w"(r) : "w"(x), "w"(y), "w"(z));
    uint32_t u = omega_float_to_bits(r);
    return (u & 0x7fffffffu) > 0x7f800000u ? 0x7fffffffu : u;
}
static uint32_t run_body(const OmegaDsInsn *v, size_t n, uint32_t a, uint32_t b) {
    uint32_t r[256];
    bool p[8];
    for (int i = 0; i < 256; i++) r[i] = 0xdeadbeefu;
    for (int i = 0; i < 8; i++) p[i] = false;
    r[OMEGA_DS_RZ] = 0; p[OMEGA_DS_PT] = true;
    r[2] = a; r[5] = b;
    for (size_t k = 0; k < n; k++) {
        const OmegaDsInsn *x = &v[k];
        uint32_t ra = r[x->a], rb = r[x->b], rc = r[x->c], res = 0;
        switch (x->kind) {
        case DSK_IADD3_R: res = (x->nega ? 0u - ra : ra) + (x->negb ? 0u - rb : rb) + rc; break;
        case DSK_IADD3_I: res = (x->nega ? 0u - ra : ra) + x->imm + rc; break;
        case DSK_LOP3_R: res = lut3(ra, rb, rc, x->lut); break;
        case DSK_LOP3_I: res = lut3(ra, x->imm, rc, x->lut); break;
        case DSK_SHL_I: res = x->imm >= 32 ? 0 : ra << x->imm; break;
        case DSK_SHL_R: res = rb >= 32 ? 0 : ra << rb; break;
        case DSK_SHR_I: res = x->imm >= 32 ? 0 : rc >> x->imm; break;
        case DSK_SHR_R: res = rb >= 32 ? 0 : rc >> rb; break;
        case DSK_ISETP_R: p[x->d] = icmp(x->cmp, x->is_signed, ra, rb); continue;
        case DSK_ISETP_I: p[x->d] = icmp(x->cmp, x->is_signed, ra, x->imm); continue;
        case DSK_SEL_R: res = (p[x->ps] ^ (x->pneg != 0)) ? ra : rb; break;
        case DSK_SEL_I: res = (p[x->ps] ^ (x->pneg != 0)) ? ra : x->imm; break;
        case DSK_I2FP: res = omega_float_to_bits((float)(int32_t)rb); break;
        case DSK_FADD_R: res = hfp(0, ra, x->negb ? rb ^ 0x80000000u : rb, 0); break;
        case DSK_FMUL_R: res = hfp(1, ra, rb, 0); break;
        case DSK_FFMA_R: res = hfp(2, ra, rb, rc); break;
        }
        if (x->d != OMEGA_DS_RZ) r[x->d] = res;
    }
    return r[OMEGA_DS_RESULT_REG];
}

uint32_t omega_ds_host_exec(OmegaDsOp op, uint32_t a, uint32_t b) {
    static OmegaDsInsn cache[OMEGA_DS_OP_COUNT][OMEGA_DS_MAX_BODY];
    static size_t cn[OMEGA_DS_OP_COUNT];
    if ((unsigned)op >= OMEGA_DS_OP_COUNT) return 0;
    if (!cn[op]) cn[op] = omega_ds_body(op, cache[op], OMEGA_DS_MAX_BODY);
    return run_body(cache[op], cn[op], a, b);
}

uint32_t omega_ds_cpu_semantic(OmegaDsOp op, uint32_t a, uint32_t b) {
    if (op == OMEGA_DS_DIV) return omega_float_to_bits(omega_math_div(omega_bits_to_float(a), omega_bits_to_float(b)));
    if (op != OMEGA_DS_SQRT) return 0x7fc00000u;
    return omega_float_to_bits(omega_math_sqrt(omega_bits_to_float(a)));
}

/* ---- Kernel image ------------------------------------------------------------ */

static const char *const PROLOGUE_TEXT[OMEGA_DS_PROLOGUE_INSNS] = {
    "LDC R1, c[0x0][0x37c]", "S2R R9, SR_CTAID.X", "LDCU UR4, c[0x0][0x360]", "S2R R0, SR_TID.X",
    "LDCU UR5, c[0x0][0x398]", "IMAD R9, R9, UR4, R0", "ISETP.GE.U32.AND P0, PT, R9, UR5, PT", "@P0 EXIT",
    "LDC.64 R2, c[0x0][0x380]", "LDCU.64 UR4, c[0x0][0x358]", "LDC.64 R4, c[0x0][0x388]",
    "LDC.64 R6, c[0x0][0x390]", "IMAD.WIDE.U32 R2, R9, 0x4, R2", "LDG.E R2, desc[UR4][R2.64]",
    "IMAD.WIDE.U32 R4, R9, 0x4, R4", "LDG.E R5, desc[UR4][R4.64]", "IMAD.WIDE.U32 R6, R9, 0x4, R6",
};
#define VA_STG  18u
#define VA_EXIT 19u
#define VA_BRA  20u
#define VA_NOP  21u

static void put_words(uint8_t *p, const uint32_t w[4]) {
    for (int i = 0; i < 4; i++) {
        p[4 * i] = (uint8_t)w[i]; p[4 * i + 1] = (uint8_t)(w[i] >> 8);
        p[4 * i + 2] = (uint8_t)(w[i] >> 16); p[4 * i + 3] = (uint8_t)(w[i] >> 24);
    }
}

static void get_words(const uint8_t *p, uint32_t w[4]) {
    for (int i = 0; i < 4; i++)
        w[i] = (uint32_t)p[4 * i] | (uint32_t)p[4 * i + 1] << 8 | (uint32_t)p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24;
}

static size_t padded_insns(size_t body) {
    size_t n = OMEGA_DS_PROLOGUE_INSNS + body + OMEGA_DS_EPILOGUE_INSNS;
    return (n + 7) & ~(size_t)7;
}

int omega_ds_build_kernel(OmegaDsOp op, uint8_t *code, size_t max, size_t *out_len) {
    uint8_t va[OMEGA_BW_VECADD_CODE_SIZE];
    size_t vl = 0;
    OmegaDsInsn body[OMEGA_DS_MAX_BODY];
    if (!code) return -1;
    size_t nb = omega_ds_body(op, body, OMEGA_DS_MAX_BODY);
    if (nb == 0) return -1;
    if (omega_blackwell_encode_vecadd(va, sizeof(va), &vl) != 0 || vl != OMEGA_BW_VECADD_CODE_SIZE) return -1;
    size_t total = padded_insns(nb);
    if (total * 16 > max || total * 16 > OMEGA_DS_MAX_CODE_BYTES) return -1;
    size_t k = 0;
    for (; k < OMEGA_DS_PROLOGUE_INSNS; k++) memcpy(code + 16 * k, va + 16 * k, 16);
    for (size_t i = 0; i < nb; i++, k++) {
        uint32_t w[4];
        if (omega_ds_encode(&body[i], w) != 0) return -1;
        put_words(code + 16 * k, w);
    }
    memcpy(code + 16 * k++, va + 16 * VA_STG, 16);
    memcpy(code + 16 * k++, va + 16 * VA_EXIT, 16);
    memcpy(code + 16 * k++, va + 16 * VA_BRA, 16);
    for (; k < total; k++) memcpy(code + 16 * k, va + 16 * VA_NOP, 16);
    if (out_len) *out_len = total * 16;
    return 0;
}

int omega_ds_listing(OmegaDsOp op, char *buf, size_t len) {
    OmegaDsInsn body[OMEGA_DS_MAX_BODY];
    size_t nb = omega_ds_body(op, body, OMEGA_DS_MAX_BODY);
    if (!buf || nb == 0) return -1;
    size_t pos = 0, k = 0, total = padded_insns(nb);
    char t[96];
#define LINE(...) do { int w_ = snprintf(buf + pos, pos < len ? len - pos : 0, __VA_ARGS__); \
                       if (w_ < 0 || pos + (size_t)w_ >= len) { return -1; } \
                       pos += (size_t)w_; } while (0)
    for (; k < OMEGA_DS_PROLOGUE_INSNS; k++) LINE("%04zx %s ;\n", k * 16, PROLOGUE_TEXT[k]);
    for (size_t i = 0; i < nb; i++, k++) { omega_ds_format(&body[i], t, sizeof(t)); LINE("%04zx %s ;\n", k * 16, t); }
    LINE("%04zx STG.E desc[UR4][R6.64], R9 ;\n", k * 16); k++;
    LINE("%04zx EXIT ;\n", k * 16); k++;
    LINE("%04zx BRA 0x%zx;\n", k * 16, k * 16); k++;
    for (; k < total; k++) LINE("%04zx NOP;\n", k * 16);
#undef LINE
    return 0;
}

/* ---- Pre-submission checks ---------------------------------------------------- */

static int refuse(char *err, size_t err_len, int code, const char *fmt, ...) {
    if (err && err_len) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, err_len, fmt, ap);
        va_end(ap);
    }
    return code;
}

static bool writes_reg(const OmegaDsInsn *x) { return x->kind != DSK_ISETP_R && x->kind != DSK_ISETP_I; }

/* Source registers an instruction reads (RZ excluded). */
static int src_regs(const OmegaDsInsn *x, uint8_t out[3]) {
    int n = 0;
    switch (x->kind) {
    case DSK_IADD3_R: case DSK_LOP3_R: out[n++] = x->a; out[n++] = x->b; out[n++] = x->c; break;
    case DSK_IADD3_I: case DSK_LOP3_I: out[n++] = x->a; out[n++] = x->c; break;
    case DSK_SHL_I: out[n++] = x->a; break;
    case DSK_SHL_R: out[n++] = x->a; out[n++] = x->b; break;
    case DSK_SHR_I: out[n++] = x->c; break;
    case DSK_SHR_R: out[n++] = x->b; out[n++] = x->c; break;
    case DSK_ISETP_R: case DSK_SEL_R: out[n++] = x->a; out[n++] = x->b; break;
    case DSK_ISETP_I: case DSK_SEL_I: out[n++] = x->a; break;
    case DSK_I2FP: out[n++] = x->b; break;
    case DSK_FADD_R: case DSK_FMUL_R: out[n++] = x->a; out[n++] = x->b; break;
    case DSK_FFMA_R: out[n++] = x->a; out[n++] = x->b; out[n++] = x->c; break;
    }
    return n;
}

/* Each check is one line ending in its CHECK: marker, so the mutation sweep
 * can delete exactly that check. */
#define E_OP OMEGA_NUMERIC_ERR_OPERANDS
int omega_ds_check_kernel(OmegaDsOp op, const uint8_t *code, size_t len,
                          uint32_t gpr_count, char *err, size_t err_len) {
    if (err && err_len) err[0] = '\0';
    if ((unsigned)op >= OMEGA_DS_OP_COUNT) return refuse(err, err_len, OMEGA_NUMERIC_ERR_BAD_ARGS, "ds_op: unknown op %d", (int)op); /* CHECK:ds_op */
    if (!code) return refuse(err, err_len, OMEGA_NUMERIC_ERR_BAD_ARGS, "ds_len: no code");
    bool bad_len = len % 128 != 0 || len > OMEGA_DS_MAX_CODE_BYTES || len < 16 * (OMEGA_DS_PROLOGUE_INSNS + OMEGA_DS_EPILOGUE_INSNS + 1);
    if (bad_len) return refuse(err, err_len, E_OP, "ds_len: kernel length %zu", len); /* CHECK:ds_len */
    uint8_t va[OMEGA_BW_VECADD_CODE_SIZE];
    size_t vl = 0;
    if (omega_blackwell_encode_vecadd(va, sizeof(va), &vl) != 0) return refuse(err, err_len, E_OP, "ds_prologue: vecadd encoder failed");
    if (memcmp(code, va, 16 * OMEGA_DS_PROLOGUE_INSNS) != 0) return refuse(err, err_len, E_OP, "ds_prologue: prologue differs from the vecadd words"); /* CHECK:ds_prologue */
    /* the body ends at the first STG after the prologue */
    size_t n = len / 16, stg = n;
    for (size_t k = OMEGA_DS_PROLOGUE_INSNS; k < n && stg == n; k++)
        if (memcmp(code + 16 * k, va + 16 * VA_STG, 16) == 0) stg = k;
    bool no_tail = stg + 3 > n || memcmp(code + 16 * (stg + 1), va + 16 * VA_EXIT, 16) != 0 || memcmp(code + 16 * (stg + 2), va + 16 * VA_BRA, 16) != 0;
    if (no_tail) return refuse(err, err_len, E_OP, "ds_epilogue: STG, EXIT, BRA not found after the body"); /* CHECK:ds_epilogue */
    size_t nb = stg - OMEGA_DS_PROLOGUE_INSNS;
    if (nb == 0 || nb > OMEGA_DS_MAX_BODY) return refuse(err, err_len, E_OP, "ds_body: body of %zu instructions", nb); /* CHECK:ds_body */
    size_t bad_pad = 0;
    for (size_t k = stg + 3; k < n && !bad_pad; k++)
        if (memcmp(code + 16 * k, va + 16 * VA_NOP, 16) != 0) bad_pad = k;
    if (bad_pad) return refuse(err, err_len, E_OP, "ds_pad: word %zu after BRA is not NOP", bad_pad); /* CHECK:ds_pad */
    bool defr[256] = { false }, defp[8] = { false };
    defr[OMEGA_DS_RZ] = defr[2] = defr[5] = true;
    defp[OMEGA_DS_PT] = true;
    unsigned maxreg = 0;
    OmegaDsInsn last;
    memset(&last, 0, sizeof(last));
    last.kind = DSK_ISETP_R;
    for (size_t i = 0; i < nb; i++) {
        uint32_t w[4];
        OmegaDsInsn x;
        uint8_t s[3];
        get_words(code + 16 * (OMEGA_DS_PROLOGUE_INSNS + i), w);
        if (((w[0] >> 12) & 0xfu) != 0x7u) return refuse(err, err_len, E_OP, "ds_guard: body insn %zu is predicated", i); /* CHECK:ds_guard */
        if (w[3] != OMEGA_DS_BODY_CTRL) return refuse(err, err_len, E_OP, "ds_ctrl: body insn %zu control 0x%08x", i, w[3]); /* CHECK:ds_ctrl */
        memset(&x, 0, sizeof(x));
        int dec = omega_ds_decode(w, &x); /* decode kept off the CHECK line so deleting the check leaves x initialized */
        if (dec != 0) return refuse(err, err_len, E_OP, "ds_forms: body insn %zu is not a recorded form", i); /* CHECK:ds_forms */
        int ns = src_regs(&x, s);
        for (int j = 0; j < ns; j++) {
            if (!defr[s[j]]) return refuse(err, err_len, E_OP, "ds_def_use: body insn %zu reads R%u before any write", i, (unsigned)s[j]); /* CHECK:ds_def_use */
            if (s[j] != OMEGA_DS_RZ && s[j] > maxreg) maxreg = s[j];
        }
        bool sel = x.kind == DSK_SEL_R || x.kind == DSK_SEL_I;
        if (sel && !defp[x.ps]) return refuse(err, err_len, E_OP, "ds_pred_def_use: body insn %zu reads P%u before any write", i, (unsigned)x.ps); /* CHECK:ds_pred_def_use */
        bool wr = writes_reg(&x);
        if (wr && (x.d == 1 || x.d == 6 || x.d == 7)) return refuse(err, err_len, E_OP, "ds_reserved: body insn %zu writes R%u", i, (unsigned)x.d); /* CHECK:ds_reserved */
        if (wr) { defr[x.d] = true; if (x.d != OMEGA_DS_RZ && x.d > maxreg) maxreg = x.d; }
        else defp[x.d] = true;
        last = x;
    }
    if (maxreg + 1 + OMEGA_DS_GPR_RESERVED > gpr_count || gpr_count > 255) return refuse(err, err_len, E_OP, "ds_gpr: R%u used, %u registers allocated (top %u reserved)", maxreg, gpr_count, OMEGA_DS_GPR_RESERVED); /* CHECK:ds_gpr */
    if (!writes_reg(&last) || last.d != OMEGA_DS_RESULT_REG) return refuse(err, err_len, E_OP, "ds_result: last body insn does not write R9"); /* CHECK:ds_result */
    return OMEGA_NUMERIC_OK;
}

int omega_ds_check_digest(OmegaDsOp op, const uint8_t *code, size_t len, char *err, size_t err_len) {
    uint8_t dg[32];
    char hex[65];
    if ((unsigned)op >= OMEGA_DS_OP_COUNT || !code) return refuse(err, err_len, OMEGA_NUMERIC_ERR_BAD_ARGS, "ds_digest: bad op or code");
    if (omega_blackwell_compute_code_digest(code, len, dg) != 0) return refuse(err, err_len, E_OP, "ds_digest: digest failed");
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", dg[i]);
    if (strcmp(hex, OMEGA_DS_KERNEL_SHA256[op]) != 0) return refuse(err, err_len, E_OP, "ds_digest: %s kernel %s is not the nvdisasm-verified one", omega_ds_op_name(op), hex); /* CHECK:ds_digest */
    return OMEGA_NUMERIC_OK;
}

int omega_ds_check_args(OmegaDsOp op, const uint32_t *a, const uint32_t *b,
                        const uint32_t *out, size_t count, char *err, size_t err_len) {
    if (err && err_len) err[0] = '\0';
    if ((unsigned)op >= OMEGA_DS_OP_COUNT) return refuse(err, err_len, OMEGA_NUMERIC_ERR_BAD_ARGS, "ds_args_op: unknown op %d", (int)op); /* CHECK:ds_args_op */
    if (!a || !out || (op == OMEGA_DS_DIV && !b)) return refuse(err, err_len, OMEGA_NUMERIC_ERR_BAD_ARGS, "ds_buffers: missing buffer"); /* CHECK:ds_buffers */
    if (count == 0 || count > OMEGA_DS_MAX_BATCH) return refuse(err, err_len, E_OP, "ds_count: count %zu outside 1..%u", count, OMEGA_DS_MAX_BATCH); /* CHECK:ds_count */
    return OMEGA_NUMERIC_OK;
}

int omega_ds_check_qmd(const uint32_t *qmd1, uint64_t code_va, char *err, size_t err_len) {
    if (!qmd1) return refuse(err, err_len, OMEGA_NUMERIC_ERR_BAD_ARGS, "ds_qmd: no QMD");
    if (omega_blackwell_verify_qmd_invariants(qmd1) != 0) return refuse(err, err_len, E_OP, "ds_qmd: QMD invariants fail"); /* CHECK:ds_qmd */
    uint32_t gpr = (qmd1[35] >> 8) & 0x1ffu;
    if (gpr != OMEGA_DS_GPR_COUNT) return refuse(err, err_len, E_OP, "ds_qmd_gpr: QMD allocates %u registers, kernel needs %u", gpr, OMEGA_DS_GPR_COUNT); /* CHECK:ds_qmd_gpr */
    if (qmd1[32] != (uint32_t)(code_va >> 4) || (qmd1[33] & 0x1fffffu) != (uint32_t)((code_va >> 36) & 0x1fffffu)) return refuse(err, err_len, E_OP, "ds_qmd_code: QMD program address is not the kernel"); /* CHECK:ds_qmd_code */
    return OMEGA_NUMERIC_OK;
}
#undef E_OP

/* ---- Executor ------------------------------------------------------------------ */

#ifndef OMEGA_NUMERIC_CPU_ONLY
/* Device-failure diagnostics (M20 GB10 instrumentation). Every
 * OMEGA_NUMERIC_ERR_DEVICE return below goes through gb10_devfail, which
 * prints one GB10_DEVFAIL line to stderr: the step that failed, the driver
 * return code, errno, the nvrm error text, the wait value and the marker word
 * for waits, and the elapsed ms since the launch began. The lifecycle wrapper
 * retains resources after uncertain completion and refuses later numeric
 * launches. There is no retry and no wait value changes. */
static double gb10_ms_since(const struct timespec *t0) {
    struct timespec t;
    timespec_get(&t, TIME_UTC); /* C11; no feature macro needed */
    return (double)(t.tv_sec - t0->tv_sec) * 1e3 + (double)(t.tv_nsec - t0->tv_nsec) / 1e6;
}

static int gb10_devfail(const char *fn, const char *step, M16NativeContext *ctx, int do_close, int drv_rc,
                        int saved_errno, long wait_ms, const volatile uint32_t *word, uint32_t want,
                        const struct timespec *t0) {
    double ms = gb10_ms_since(t0);
    fprintf(stderr, "GB10_DEVFAIL fn=%s step=%s drv_rc=%d errno=%d rm_err=\"%s\" live_allocs=%u faulted=%u",
            fn, step, drv_rc, saved_errno, ctx->rm.err, (unsigned)ctx->rm.live_count, (unsigned)ctx->rm.faulted);
    if (wait_ms >= 0)
        fprintf(stderr, " wait_ms=%ld word=0x%08x want=0x%08x", wait_ms, word ? (unsigned)*word : 0u, (unsigned)want);
    fprintf(stderr, " elapsed_ms=%.3f\n", ms);
    if (do_close) omega_numeric_native_close(ctx);
    return OMEGA_NUMERIC_ERR_DEVICE;
}
/* Plain step: drv_rc is the value the call returned. */
#define GB10_FAIL(step, close_, rc_) gb10_devfail(__func__, (step), &ctx, (close_), (rc_), errno, -1, NULL, 0u, &t0)
/* Wait step: also log the wait value and the word read. */
#define GB10_FAIL_WAIT(step, rc_, ms_, w_, want_) \
    gb10_devfail(__func__, (step), &ctx, 1, (rc_), errno, (long)(ms_), (w_), (want_), &t0)
#endif

int omega_ds_gb10_run(OmegaDsOp op, const uint32_t *a, const uint32_t *b, uint32_t *out, size_t count) {
    char err[256];
    int rc = omega_ds_check_args(op, a, b, out, count, err, sizeof(err));
    if (rc != OMEGA_NUMERIC_OK) { fprintf(stderr, "omega_ds_gb10_run: refused: %s\n", err); return rc; }
    uint8_t code[OMEGA_DS_MAX_CODE_BYTES];
    size_t code_len = 0;
    if (omega_ds_build_kernel(op, code, sizeof(code), &code_len) != 0) return OMEGA_NUMERIC_ERR_OPERANDS;
    if ((rc = omega_ds_check_kernel(op, code, code_len, OMEGA_DS_GPR_COUNT, err, sizeof(err))) != OMEGA_NUMERIC_OK ||
        (rc = omega_ds_check_digest(op, code, code_len, err, sizeof(err))) != OMEGA_NUMERIC_OK) {
        fprintf(stderr, "omega_ds_gb10_run: refused: %s\n", err);
        return rc;
    }
    {
        /* the QMD as it will be built, checked before any device work */
        uint32_t q[OMEGA_BW_QMD_WORDS];
        OmegaBlackwellQmdConfig c = { .code_va = 0x200000000ull, .cbank_va = 0x200100000ull,
                                      .scratch_va = 0x200204000ull, .sem_va = 0x200202000ull,
                                      .qmd0_va = 0x200200000ull, .qmd1_va = 0x200201000ull,
                                      .num_elements = (uint32_t)count, .gpr_count = OMEGA_DS_GPR_COUNT };
        omega_numeric_launch_shape(count, &c.threads_per_block, &c.grid_width);
        if (omega_blackwell_build_qmd1(q, &c) != 0 ||
            (rc = omega_ds_check_qmd(q, c.code_va, err, sizeof(err))) != OMEGA_NUMERIC_OK) {
            fprintf(stderr, "omega_ds_gb10_run: refused: %s\n", err);
            return rc ? rc : OMEGA_NUMERIC_ERR_OPERANDS;
        }
    }
#ifdef OMEGA_NUMERIC_CPU_ONLY
    return OMEGA_NUMERIC_ERR_DEVICE;
#else
    static const uint32_t SETUP[18] = {
        0x20012061, 0x0000cec0, 0x20012092, 0x00000001, 0x200120a8, 0x0000000f, 0x2001255d, 0x00000003,
        0x2001255e, 0x20000000, 0x2001255f, 0x000fffff, 0x20012557, 0x00000003, 0x20012558, 0x22000000,
        0x20012559, 0x00000000,
    };
    struct timespec t0;
    timespec_get(&t0, TIME_UTC);
    M16NativeContext ctx;
    int drc_;
    if ((drc_ = omega_numeric_native_open(&ctx)) != 0) return GB10_FAIL("open", 0, drc_);
    if ((drc_ = m16_native_create_channel(&ctx)) != 0) return GB10_FAIL("channel", 1, drc_);
    NvrmMem large_pb;
    if ((drc_ = nvrm_alloc(&ctx.rm, 0x10000, &large_pb)) != 0) return GB10_FAIL("alloc_pb", 1, drc_);
    ctx.pb_mem = large_pb;
    size_t bytes = (count * 4 + 0xfffULL) & ~0xfffULL;
    NvrmMem code_mem, cbank_mem, a_mem, b_mem, out_mem, marker_mem, qmd_mem;
    if (nvrm_alloc(&ctx.rm, omega_gpu_code_alloc_bytes(OMEGA_DS_MAX_CODE_BYTES), &code_mem) /* + 2 KB prefetch tail */ != 0 ||
        nvrm_alloc(&ctx.rm, 0x1000, &cbank_mem) != 0 ||
        nvrm_alloc(&ctx.rm, bytes, &a_mem) != 0 ||
        nvrm_alloc(&ctx.rm, bytes, &b_mem) != 0 ||
        nvrm_alloc(&ctx.rm, bytes, &out_mem) != 0 ||
        nvrm_alloc_gpu_uncached(&ctx.rm, 0x1000, &marker_mem) != 0 ||
        nvrm_alloc(&ctx.rm, 0x10000, &qmd_mem) != 0) {
        return GB10_FAIL("alloc_buffers", 1, -1);
    }
    memcpy(a_mem.cpu, a, count * 4);
    if (b) memcpy(b_mem.cpu, b, count * 4); else memset(b_mem.cpu, 0, count * 4);
    memset(out_mem.cpu, 0x55, count * 4);
    memcpy(code_mem.cpu, code, code_len);

    uint32_t cbank_data[OMEGA_BW_CBANK_DRIVER_WORDS];
    omega_blackwell_build_cbank_driver(cbank_data, cbank_mem.va);
    cbank_data[223] = 0;
    uint32_t args[10] = { (uint32_t)a_mem.va, (uint32_t)(a_mem.va >> 32), (uint32_t)b_mem.va, (uint32_t)(b_mem.va >> 32),
                          (uint32_t)out_mem.va, (uint32_t)(out_mem.va >> 32), (uint32_t)count, 0, 0, 0 };
    memcpy(cbank_mem.cpu, cbank_data, sizeof(cbank_data));
    memcpy((uint8_t *)cbank_mem.cpu + 0x380, args, sizeof(args));

    uint64_t qmd0_va = qmd_mem.va, qmd1_va = qmd_mem.va + 0x1000, sem_va = qmd_mem.va + 0x2000;
    OmegaBlackwellQmdConfig cfg = { .code_va = code_mem.va, .cbank_va = cbank_mem.va, .scratch_va = qmd_mem.va + 0x4000,
                                    .sem_va = sem_va, .qmd0_va = qmd0_va, .qmd1_va = qmd1_va,
                                    .num_elements = (uint32_t)count, .gpr_count = OMEGA_DS_GPR_COUNT };
    omega_numeric_launch_shape(count, &cfg.threads_per_block, &cfg.grid_width);
    uint32_t qmd0[OMEGA_BW_QMD_WORDS], qmd1[OMEGA_BW_QMD_WORDS];
    if (omega_blackwell_build_qmd0(qmd0, qmd0_va, qmd1_va) != 0 || omega_blackwell_build_qmd1(qmd1, &cfg) != 0 ||
        omega_ds_check_qmd(qmd1, code_mem.va, err, sizeof(err)) != OMEGA_NUMERIC_OK) {
        fprintf(stderr, "omega_ds_gb10_run: %s\n", err);
        omega_numeric_native_close(&ctx);
        return OMEGA_NUMERIC_ERR_OPERANDS;
    }
    /* the exact bytes in device memory pass the structural check again */
    if (omega_ds_check_kernel(op, code_mem.cpu, code_len, (qmd1[35] >> 8) & 0x1ffu, err, sizeof(err)) != OMEGA_NUMERIC_OK ||
        omega_ds_check_digest(op, code_mem.cpu, code_len, err, sizeof(err)) != OMEGA_NUMERIC_OK) {
        fprintf(stderr, "omega_ds_gb10_run: %s\n", err);
        omega_numeric_native_close(&ctx);
        return OMEGA_NUMERIC_ERR_OPERANDS;
    }
    memcpy(qmd_mem.cpu, qmd0, sizeof(qmd0));
    memcpy((uint8_t *)qmd_mem.cpu + 0x1000, qmd1, sizeof(qmd1));
    volatile uint32_t *hsem = (volatile uint32_t *)((uint8_t *)qmd_mem.cpu + 0x2000);
    volatile uint32_t *hmarker = (volatile uint32_t *)marker_mem.cpu;
    *hsem = 0;
    *hmarker = 0;
    *(volatile uint32_t *)((uint8_t *)marker_mem.cpu + 0x10) = 0;
    __asm__ volatile("dsb sy" ::: "memory");

    static uint32_t pb[1024];
    size_t n = 0;
    memcpy(&pb[n], SETUP, sizeof(SETUP)); n += 18;
    pb[n++] = nvrm_mthd(1, 0x0188, 2); pb[n++] = (uint32_t)(cbank_mem.va >> 32); pb[n++] = (uint32_t)cbank_mem.va;
    pb[n++] = nvrm_mthd(1, 0x0180, 2); pb[n++] = 0x00000380; pb[n++] = 0x00000001;
    pb[n++] = nvrm_mthd(1, 0x01b0, 1); pb[n++] = 0x00000041;
    pb[n++] = (224 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    memcpy(&pb[n], cbank_data, 224 * 4); n += 224;
    pb[n++] = nvrm_mthd(1, 0x0188, 2); pb[n++] = (uint32_t)((cbank_mem.va + 0x380) >> 32); pb[n++] = (uint32_t)(cbank_mem.va + 0x380);
    pb[n++] = nvrm_mthd(1, 0x0180, 2); pb[n++] = 0x00000028; pb[n++] = 0x00000001;
    pb[n++] = nvrm_mthd(1, 0x01b0, 1); pb[n++] = 0x00000041;
    pb[n++] = (10 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    memcpy(&pb[n], args, 10 * 4); n += 10;
    pb[n++] = (98 << 16) | (1 << 13) | (0x0318 >> 2) | (2u << 28);
    pb[n++] = (1u << 30) | (uint32_t)((qmd0_va >> 40) & 0x1ff);
    pb[n++] = (uint32_t)(qmd0_va >> 8);
    memcpy(&pb[n], qmd0, 96 * 4); n += 96;
    pb[n++] = nvrm_mthd(1, 0x0188, 2); pb[n++] = (uint32_t)(sem_va >> 32); pb[n++] = (uint32_t)sem_va;
    pb[n++] = nvrm_mthd(1, 0x0180, 2); pb[n++] = 0x00000004; pb[n++] = 0x00000001;
    pb[n++] = nvrm_mthd(1, 0x01b0, 1); pb[n++] = 0x00000041;
    pb[n++] = (1 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    pb[n++] = OMEGA_BW_SEMAPHORE_INTERMEDIATE_INIT;
    pb[n++] = (98 << 16) | (1 << 13) | (0x0318 >> 2) | (2u << 28);
    pb[n++] = (1u << 30) | (uint32_t)((qmd1_va >> 40) & 0x1ff);
    pb[n++] = (uint32_t)(qmd1_va >> 8);
    memcpy(&pb[n], qmd1, 96 * 4); n += 96;
    pb[n++] = nvrm_mthd(0, 0x005c, 5);
    pb[n++] = (uint32_t)marker_mem.va; pb[n++] = (uint32_t)(marker_mem.va >> 32);
    pb[n++] = OMEGA_BW_MARKER_COMPLETION_PAYLOAD; pb[n++] = 0; pb[n++] = 0x1 | (1u << 20);
    /* C3 FIX A: after the WFI marker, flush GPU L2 dirty lines to memory
     * (NVC96F_MEM_OP_A..D = 0x28..0x34, D bits 31:27 OPERATION = L2_FLUSH_DIRTY 0x10;
     * third_party/nvidia-open-580.173.02/src/common/sdk/nvidia/inc/class/clc96f.h:36-73),
     * then a second WFI marker that the host waits for before the readback. */
    pb[n++] = nvrm_mthd(0, 0x0028, 4); pb[n++] = 0; pb[n++] = 0; pb[n++] = 0; pb[n++] = (0x10u << 27);
    pb[n++] = nvrm_mthd(0, 0x005c, 5);
    pb[n++] = (uint32_t)(marker_mem.va + 0x10); pb[n++] = (uint32_t)((marker_mem.va + 0x10) >> 32);
    pb[n++] = 0x46464646u; pb[n++] = 0; pb[n++] = 0x1 | (1u << 20);

    if ((drc_ = m16_native_submit_methods(&ctx, pb, n)) != 0) return GB10_FAIL("submit", 1, drc_);
    /* A timeout is uncertain completion, even after a long wait. */
    if ((drc_ = omega_numeric_native_wait(hmarker, OMEGA_BW_MARKER_COMPLETION_PAYLOAD, 600000)) != 0) {
        return GB10_FAIL_WAIT("marker_wait", drc_, 600000, hmarker, OMEGA_BW_MARKER_COMPLETION_PAYLOAD);
    }
    volatile uint32_t *hmarker2 = (volatile uint32_t *)((uint8_t *)marker_mem.cpu + 0x10);
    if ((drc_ = omega_numeric_native_wait(hmarker2, 0x46464646u, 600000)) != 0) {
        return GB10_FAIL_WAIT("marker2_wait", drc_, 600000, hmarker2, 0x46464646u);
    }
    /* The host marker can land before the last CTAs' stores are visible: the
     * first chip run (receipt 88930d2f...) read the 0x55 fill pattern for
     * 49,359,680 SQRT outputs. Read only after the QMD's own release
     * semaphore (written after the grid completes, with its membar) is 6. */
    if ((drc_ = omega_numeric_native_wait(hsem, OMEGA_BW_SEMAPHORE_INTERMEDIATE_DONE, 600000)) != 0) {
        return GB10_FAIL_WAIT("sem_wait", drc_, 600000, hsem, OMEGA_BW_SEMAPHORE_INTERMEDIATE_DONE);
    }
    __asm__ volatile("dsb sy" ::: "memory");
    memcpy(out, out_mem.cpu, count * 4);
    if (omega_numeric_native_close(&ctx) != 0) return OMEGA_NUMERIC_ERR_DEVICE;
    return OMEGA_NUMERIC_OK;
#endif
}
