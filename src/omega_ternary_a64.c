#include "omega_ternary_a64.h"
#include <string.h>

/* =========================================================================
 * Extension encoders (A64). Register 31 is XZR/WZR in every form used here.
 * ========================================================================= */

typedef struct {
    uint8_t *buf;
    size_t pos;
    size_t max;
    int err;
} TEmit;

enum { SH_LSL = 0, SH_LSR = 1, SH_ASR = 2 };
enum { C_EQ = 0, C_NE = 1, C_HI = 8, C_LS = 9, C_GE = 10, C_LT = 11, C_GT = 12, C_LE = 13 };
#define ZR 31
#define C_LO 3

static void emit(TEmit *e, uint32_t w) {
    if (e->pos + 4 > e->max) { e->err = -1; return; }
    e->buf[e->pos++] = (uint8_t)w;
    e->buf[e->pos++] = (uint8_t)(w >> 8);
    e->buf[e->pos++] = (uint8_t)(w >> 16);
    e->buf[e->pos++] = (uint8_t)(w >> 24);
}

static void patch(TEmit *e, size_t at, uint32_t w) {
    e->buf[at] = (uint8_t)w;
    e->buf[at + 1] = (uint8_t)(w >> 8);
    e->buf[at + 2] = (uint8_t)(w >> 16);
    e->buf[at + 3] = (uint8_t)(w >> 24);
}

static uint32_t sfb(bool sf) { return sf ? 0x80000000u : 0; }

static uint32_t shreg(uint32_t base, bool sf, int rd, int rn, int rm, int sh, int amt) {
    return base | sfb(sf) | ((uint32_t)sh << 22) | ((uint32_t)rm << 16) |
           ((uint32_t)amt << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static void add_r(TEmit *e, int rd, int rn, int rm) { emit(e, shreg(0x0B000000, true, rd, rn, rm, 0, 0)); }
static void sub_r(TEmit *e, int rd, int rn, int rm) { emit(e, shreg(0x4B000000, true, rd, rn, rm, 0, 0)); }
static void and_r(TEmit *e, bool sf, int rd, int rn, int rm) { emit(e, shreg(0x0A000000, sf, rd, rn, rm, 0, 0)); }
static void bic_r(TEmit *e, bool sf, int rd, int rn, int rm) { emit(e, shreg(0x0A200000, sf, rd, rn, rm, 0, 0)); }
static void orr_rs(TEmit *e, bool sf, int rd, int rn, int rm, int sh, int amt) {
    emit(e, shreg(0x2A000000, sf, rd, rn, rm, sh, amt));
}
static void orr_r(TEmit *e, bool sf, int rd, int rn, int rm) { orr_rs(e, sf, rd, rn, rm, SH_LSL, 0); }
static void eor_r(TEmit *e, int rd, int rn, int rm) { emit(e, shreg(0x4A000000, true, rd, rn, rm, 0, 0)); }
static void mov_r(TEmit *e, bool sf, int rd, int rm) { orr_r(e, sf, rd, ZR, rm); }
static void neg_r(TEmit *e, int rd, int rm) { sub_r(e, rd, ZR, rm); }
static void cmp_rs(TEmit *e, int rn, int rm, int sh, int amt) { emit(e, shreg(0x6B000000, true, ZR, rn, rm, sh, amt)); }
static void cmp_r(TEmit *e, int rn, int rm) { cmp_rs(e, rn, rm, SH_LSL, 0); }

static uint32_t immop(uint32_t base, bool sf, int rd, int rn, uint32_t imm12) {
    return base | sfb(sf) | ((imm12 & 0xFFF) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
static void add_i(TEmit *e, int rd, int rn, uint32_t imm) { emit(e, immop(0x11000000, true, rd, rn, imm)); }
static void sub_i(TEmit *e, int rd, int rn, uint32_t imm) { emit(e, immop(0x51000000, true, rd, rn, imm)); }
static void subs_i(TEmit *e, bool sf, int rd, int rn, uint32_t imm) { emit(e, immop(0x71000000, sf, rd, rn, imm)); }
static void cmp_i(TEmit *e, int rn, uint32_t imm) { subs_i(e, true, ZR, rn, imm); }
static void cmn_i(TEmit *e, int rn, uint32_t imm) { emit(e, immop(0x31000000, true, ZR, rn, imm)); }

static uint32_t csop(uint32_t base, int rd, int rn, int rm, int cond) {
    return base | 0x80000000u | ((uint32_t)rm << 16) | ((uint32_t)cond << 12) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
static void csel(TEmit *e, int rd, int rn, int rm, int cond) { emit(e, csop(0x1A800000, rd, rn, rm, cond)); }
static void csinc(TEmit *e, int rd, int rn, int rm, int cond) { emit(e, csop(0x1A800400, rd, rn, rm, cond)); }
static void csinv(TEmit *e, int rd, int rn, int rm, int cond) { emit(e, csop(0x5A800000, rd, rn, rm, cond)); }
static void cset(TEmit *e, int rd, int cond) { csinc(e, rd, ZR, ZR, cond ^ 1); }

static void ubfm(TEmit *e, bool sf, int rd, int rn, int immr, int imms) {
    uint32_t base = sf ? 0xD3400000u : 0x53000000u;
    emit(e, base | ((uint32_t)immr << 16) | ((uint32_t)imms << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}
static void lsl_i(TEmit *e, bool sf, int rd, int rn, int s) {
    int w = sf ? 64 : 32;
    ubfm(e, sf, rd, rn, (w - s) % w, w - 1 - s);
}
static void lsr_i(TEmit *e, bool sf, int rd, int rn, int s) { ubfm(e, sf, rd, rn, s, sf ? 63 : 31); }
static void ubfx(TEmit *e, int rd, int rn, int lsb, int width) { ubfm(e, true, rd, rn, lsb, lsb + width - 1); }
static void asr_i(TEmit *e, int rd, int rn, int s) {
    emit(e, 0x93400000u | ((uint32_t)s << 16) | (63u << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}
static void ror_i(TEmit *e, int rd, int rn, int s) {
    emit(e, 0x93C00000u | ((uint32_t)rn << 16) | ((uint32_t)s << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}
static void sdiv(TEmit *e, int rd, int rn, int rm) {
    emit(e, 0x9AC00C00u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd);
}
static void lslv_w(TEmit *e, int rd, int rn, int rm) {
    emit(e, 0x1AC02000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd);
}
static void madd(TEmit *e, int rd, int rn, int rm, int ra) {
    emit(e, 0x9B000000u | ((uint32_t)rm << 16) | ((uint32_t)ra << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}
static void msub(TEmit *e, int rd, int rn, int rm, int ra) {
    emit(e, 0x9B008000u | ((uint32_t)rm << 16) | ((uint32_t)ra << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}
static void mul(TEmit *e, int rd, int rn, int rm) { madd(e, rd, rn, rm, ZR); }
static void smulh(TEmit *e, int rd, int rn, int rm) {
    emit(e, 0x9B407C00u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd);
}
static void clz_w(TEmit *e, int rd, int rn) { emit(e, 0x5AC01000u | ((uint32_t)rn << 5) | (uint32_t)rd); }
static void movz(TEmit *e, int rd, uint16_t imm, int hw) {
    emit(e, 0xD2800000u | ((uint32_t)hw << 21) | ((uint32_t)imm << 5) | (uint32_t)rd);
}
static void movk(TEmit *e, int rd, uint16_t imm, int hw) {
    emit(e, 0xF2800000u | ((uint32_t)hw << 21) | ((uint32_t)imm << 5) | (uint32_t)rd);
}
static void movn(TEmit *e, int rd, uint16_t imm, int hw) {
    emit(e, 0x92800000u | ((uint32_t)hw << 21) | ((uint32_t)imm << 5) | (uint32_t)rd);
}
static void movn_w(TEmit *e, int rd, uint16_t imm) { emit(e, 0x12800000u | ((uint32_t)imm << 5) | (uint32_t)rd); }
static void ret(TEmit *e) { emit(e, 0xD65F03C0u); }

/* Branches: emitted with a zero offset, patched once the target is known. */
static size_t cbz_x(TEmit *e, int rt) { size_t at = e->pos; emit(e, 0xB4000000u | (uint32_t)rt); return at; }
static size_t cbnz_x(TEmit *e, int rt) { size_t at = e->pos; emit(e, 0xB5000000u | (uint32_t)rt); return at; }
static size_t b_cond(TEmit *e, int cond) { size_t at = e->pos; emit(e, 0x54000000u | (uint32_t)cond); return at; }
static size_t b_uncond(TEmit *e) { size_t at = e->pos; emit(e, 0x14000000u); return at; }

static void link19(TEmit *e, size_t at, size_t target) {
    if (e->err) return;
    int32_t off = (int32_t)((int64_t)target - (int64_t)at) / 4;
    uint32_t w = (uint32_t)e->buf[at] | ((uint32_t)e->buf[at + 1] << 8) |
                 ((uint32_t)e->buf[at + 2] << 16) | ((uint32_t)e->buf[at + 3] << 24);
    patch(e, at, w | (((uint32_t)off & 0x7FFFF) << 5));
}
static void link26(TEmit *e, size_t at, size_t target) {
    if (e->err) return;
    int32_t off = (int32_t)((int64_t)target - (int64_t)at) / 4;
    patch(e, at, 0x14000000u | ((uint32_t)off & 0x3FFFFFF));
}

/* Minimal MOVZ/MOVK or MOVN/MOVK materialization of a 64-bit constant. */
static uint32_t mov_imm64_count(uint64_t v) {
    uint32_t nz = 0, nf = 0;
    for (int hw = 0; hw < 4; ++hw) {
        uint16_t h = (uint16_t)(v >> (16 * hw));
        if (h != 0) nz++;
        if (h != 0xFFFF) nf++;
    }
    if (nz == 0) nz = 1;
    if (nf == 0) nf = 1;
    return nz <= nf ? nz : nf;
}

static void mov_imm64(TEmit *e, int rd, uint64_t v) {
    uint32_t nz = 0, nf = 0;
    for (int hw = 0; hw < 4; ++hw) {
        uint16_t h = (uint16_t)(v >> (16 * hw));
        if (h != 0) nz++;
        if (h != 0xFFFF) nf++;
    }
    bool use_n = (nz == 0 ? 1u : nz) > (nf == 0 ? 1u : nf);
    uint16_t skip = use_n ? 0xFFFF : 0x0000;
    bool first = true;
    for (int hw = 0; hw < 4; ++hw) {
        uint16_t h = (uint16_t)(v >> (16 * hw));
        if (h == skip) continue;
        if (first) {
            if (use_n) movn(e, rd, (uint16_t)~h, hw); else movz(e, rd, h, hw);
            first = false;
        } else {
            movk(e, rd, h, hw);
        }
    }
    if (first) {
        if (use_n) movn(e, rd, 0, 0); else movz(e, rd, 0, 0);
    }
}

/* =========================================================================
 * Register conventions
 *   X0 value, X1 operand, X2-X8/X11/X15-X17 scratch, X12 spill,
 *   X9 = +TW_MAX, X10 = -TW_MAX            (INT clamp constants)
 *   X13 = all +1 trits, X14 = all -1 trits (PLANES saturation constants)
 * ========================================================================= */

enum { R_TMAX = 9, R_NTMAX = 10, R_SPILL = 12, R_PMAX = 13, R_NMAX = 14 };

/* Static layout constants; the gate proves these against ptrace counts. */
#define TOP_DYN_SETUP 4u
#define TOP_DYN_ITER 21u
#define TOP_DYN_TAIL 2u
#define FROMP_DYN_SETUP 11u
#define FROMP_DYN_ITER 8u
#define PADD_DYN_SETUP 4u
#define PADD_DYN_ITER 16u
#define PADD_DYN_TAIL 7u
#define CLAMP_INSNS 4u
#define MULH_INSNS 7u

static uint32_t hoist_int_count(void) { return mov_imm64_count((uint64_t)TW_MAX) + 1; }
static void hoist_int(TEmit *e) { mov_imm64(e, R_TMAX, (uint64_t)TW_MAX); neg_r(e, R_NTMAX, R_TMAX); }
static const uint32_t HOIST_PLANES_COUNT = 2;
static void hoist_planes(TEmit *e) { movn_w(e, R_PMAX, 0); ror_i(e, R_NMAX, R_PMAX, 32); }

/* X0: int -> planes. Clobbers X2-X8. */
static void emit_to_planes(TEmit *e) {
    mov_r(e, true, 2, ZR);
    mov_r(e, true, 3, ZR);
    movz(e, 4, 1, 0);
    movz(e, 5, 3, 0);
    size_t loop = e->pos;
    size_t exit_br = cbz_x(e, 0);
    sdiv(e, 6, 0, 5);          /* q = v / 3 (truncating) */
    msub(e, 7, 6, 5, 0);       /* r = v - 3q, r in [-2, 2] */
    cmp_i(e, 7, 1);
    csinc(e, 6, 6, 6, C_LE);   /* q += (r > 1) */
    sub_i(e, 8, 7, 3);
    csel(e, 7, 8, 7, C_GT);    /* r -= 3 when r > 1 */
    cmn_i(e, 7, 1);
    sub_i(e, 8, 6, 1);
    csel(e, 6, 8, 6, C_LT);    /* q -= 1 when r < -1 */
    add_i(e, 8, 7, 3);
    csel(e, 7, 8, 7, C_LT);    /* r += 3 when r < -1 */
    cmp_i(e, 7, 1);
    csel(e, 8, 4, ZR, C_EQ);
    orr_r(e, true, 2, 2, 8);   /* pos |= bit when r == +1 */
    cmn_i(e, 7, 1);
    csel(e, 8, 4, ZR, C_EQ);
    orr_r(e, true, 3, 3, 8);   /* neg |= bit when r == -1 */
    lsl_i(e, true, 4, 4, 1);
    mov_r(e, true, 0, 6);
    size_t back = b_uncond(e);
    link26(e, back, loop);
    link19(e, exit_br, e->pos);
    orr_rs(e, true, 0, 2, 3, SH_LSL, 32);
}

/* X0: planes -> int. Clobbers X2-X8. */
static void emit_from_planes(TEmit *e) {
    mov_r(e, false, 2, 0);          /* pos */
    lsr_i(e, true, 3, 0, 32);       /* neg */
    orr_r(e, false, 4, 2, 3);
    clz_w(e, 4, 4);
    lslv_w(e, 2, 2, 4);             /* align the top trit to bit 31 */
    lslv_w(e, 3, 3, 4);
    movz(e, 5, 32, 0);
    sub_r(e, 5, 5, 4);              /* significant trit count */
    movz(e, 0, 0, 0);
    movz(e, 6, 3, 0);
    size_t exit_br = cbz_x(e, 5);
    size_t loop = e->pos;
    lsr_i(e, false, 7, 2, 31);
    lsr_i(e, false, 8, 3, 31);
    madd(e, 0, 0, 6, 7);            /* acc = 3 * acc + pos_top */
    sub_r(e, 0, 0, 8);              /*       - neg_top */
    lsl_i(e, false, 2, 2, 1);
    lsl_i(e, false, 3, 3, 1);
    subs_i(e, true, 5, 5, 1);
    size_t back = b_cond(e, C_NE);
    link19(e, back, loop);
    link19(e, exit_br, e->pos);
}

static void emit_clamp(TEmit *e) {
    cmp_r(e, 0, R_TMAX);
    csel(e, 0, R_TMAX, 0, C_GT);
    cmp_r(e, 0, R_NTMAX);
    csel(e, 0, R_NTMAX, 0, C_LT);
}

/* X0 * X1 with exact overflow detection, before clamping. */
static void emit_int_mul(TEmit *e, bool elide_mulh) {
    if (elide_mulh) { mul(e, 0, 0, 1); return; }
    mul(e, 2, 0, 1);
    smulh(e, 3, 0, 1);
    eor_r(e, 4, 0, 1);
    cmp_i(e, 4, 0);
    csel(e, 4, R_NTMAX, R_TMAX, C_LT);  /* saturation direction = sign(a ^ b) */
    cmp_rs(e, 3, 2, SH_ASR, 63);        /* high half must be sign extension */
    csel(e, 0, 4, 2, C_NE);
}

/* PLANES add: X0 + X1 -> X0 with symmetric saturation. Clobbers X2-X8, X11, X15-X17. */
static void emit_planes_add(TEmit *e) {
    mov_r(e, false, 2, 0);
    lsr_i(e, true, 3, 0, 32);
    mov_r(e, false, 4, 1);
    lsr_i(e, true, 5, 1, 32);
    size_t loop = e->pos;
    orr_r(e, true, 6, 4, 5);      /* b non-zero */
    orr_r(e, true, 7, 2, 3);      /* a non-zero */
    and_r(e, true, 8, 2, 4);      /* +1 + +1: digit -1, carry +1 */
    and_r(e, true, 11, 3, 5);     /* -1 + -1: digit +1, carry -1 */
    bic_r(e, true, 15, 2, 6);
    bic_r(e, true, 16, 4, 7);
    bic_r(e, true, 17, 3, 6);
    bic_r(e, true, 6, 5, 7);
    orr_r(e, true, 2, 15, 16);
    orr_r(e, true, 2, 2, 11);     /* sum positive plane */
    orr_r(e, true, 3, 17, 6);
    orr_r(e, true, 3, 3, 8);      /* sum negative plane */
    lsl_i(e, true, 4, 8, 1);
    lsl_i(e, true, 5, 11, 1);
    orr_r(e, true, 6, 8, 11);
    size_t back = cbnz_x(e, 6);
    link19(e, back, loop);
    /* Planes are 64 bits wide: a 33rd trit means overflow. */
    orr_r(e, true, 6, 2, 3);
    lsr_i(e, true, 6, 6, 32);
    cmp_r(e, 2, 3);
    csel(e, 7, R_PMAX, R_NMAX, C_HI);
    orr_rs(e, true, 0, 2, 3, SH_LSL, 32);
    cmp_i(e, 6, 0);
    csel(e, 0, 7, 0, C_NE);
}

static void emit_planes_logic(TEmit *e, TernaryOp op) {
    if (op == TOP_TAND || op == TOP_TOR) {
        if (op == TOP_TAND) and_r(e, true, 2, 0, 1); else orr_r(e, true, 2, 0, 1);
        mov_r(e, false, 2, 2);
        if (op == TOP_TAND) orr_r(e, true, 3, 0, 1); else and_r(e, true, 3, 0, 1);
        lsr_i(e, true, 3, 3, 32);
        orr_rs(e, true, 0, 2, 3, SH_LSL, 32);
    } else { /* TXOR: pos = (ap&bn)|(an&bp), neg = (ap&bp)|(an&bn) */
        ror_i(e, 2, 1, 32);
        and_r(e, true, 3, 0, 2);
        orr_rs(e, true, 3, 3, 3, SH_LSR, 32);
        mov_r(e, false, 3, 3);
        and_r(e, true, 4, 0, 1);
        orr_rs(e, true, 4, 4, 4, SH_LSR, 32);
        orr_rs(e, true, 0, 3, 4, SH_LSL, 32);
    }
}

static const uint32_t PLANES_LOGIC_INSNS[3] = { 5, 5, 7 }; /* TAND, TOR, TXOR */

/* Three-way sign of (G vs L) as planes: G > L -> +1, G < L -> -1. */
static void emit_planes_cmp(TEmit *e, bool vs_zero) {
    mov_r(e, false, 2, 0);
    lsr_i(e, true, 3, 0, 32);
    if (vs_zero) {
        cmp_r(e, 2, 3);
        cset(e, 4, C_HI);
        cset(e, 5, C_LO);
        orr_rs(e, true, 0, 4, 5, SH_LSL, 32);
        return;
    }
    mov_r(e, false, 4, 1);
    lsr_i(e, true, 5, 1, 32);
    bic_r(e, true, 6, 2, 4);
    bic_r(e, true, 7, 5, 3);
    orr_r(e, true, 6, 6, 7);      /* trits where a > b */
    bic_r(e, true, 7, 4, 2);
    bic_r(e, true, 8, 3, 5);
    orr_r(e, true, 7, 7, 8);      /* trits where a < b */
    cmp_r(e, 6, 7);
    cset(e, 2, C_HI);
    cset(e, 3, C_LO);
    orr_rs(e, true, 0, 2, 3, SH_LSL, 32);
}

static void emit_planes_shl(TEmit *e, int k) {
    lsl_i(e, false, 2, 0, k);             /* positive plane << k, 32-bit */
    lsr_i(e, true, 3, 0, 32);
    orr_rs(e, true, 2, 2, 3, SH_LSL, 32 + k);
    ubfx(e, 4, 0, 32 - k, k);             /* positive trits shifted out */
    lsr_i(e, true, 5, 0, 64 - k);         /* negative trits shifted out */
    orr_r(e, true, 4, 4, 5);
    mov_r(e, false, 6, 0);
    cmp_r(e, 6, 3);
    csel(e, 7, R_PMAX, R_NMAX, C_HI);
    cmp_i(e, 4, 0);
    csel(e, 0, 7, 2, C_NE);
}

static void emit_planes_shr(TEmit *e, int k) {
    ubfx(e, 2, 0, k, 32 - k);
    lsr_i(e, true, 3, 0, 32 + k);
    orr_rs(e, true, 0, 2, 3, SH_LSL, 32);
}

static int64_t pow3(int k) {
    int64_t d = 1;
    for (int i = 0; i < k; ++i) d *= 3;
    return d;
}

/* INT round-to-nearest division by 3^k; divisor already in X1. */
static void emit_int_shr_body(TEmit *e, int k) {
    int64_t h = (pow3(k) - 1) / 2;
    sdiv(e, 3, 0, 1);
    msub(e, 4, 3, 1, 0);
    if (h <= 4095) cmp_i(e, 4, (uint32_t)h); else cmp_r(e, 4, 5);
    csinc(e, 3, 3, 3, C_LE);
    if (h <= 4095) cmn_i(e, 4, (uint32_t)h); else { add_r(e, 6, 4, 5); cmp_i(e, 6, 0); }
    sub_i(e, 6, 3, 1);
    csel(e, 0, 6, 3, C_LT);
}

static bool shift_ok(TernaryOp op, int64_t k) {
    if (op == TOP_TSHL) return k >= 1 && k <= 8;
    if (op == TOP_TSHR) return k >= 1 && k <= 31;
    return true;
}

bool omega_t_a64_supported(TernaryOp op, TernaryRep rep) {
    if (op < TOP_TNEG || op > TOP_TSHR) return false;
    if (rep == TREP_PLANES && op == TOP_TMUL) return false;
    return true;
}

static bool op_takes_operand(TernaryOp op) {
    return !omega_t_op_is_unary(op);
}

/* Constant operand materialization into X1 (and X5 for large TSHR halves). */
static void emit_step_const(TEmit *e, TernaryOp op, TernaryRep rep, int64_t k) {
    if (!op_takes_operand(op)) return;
    if (op == TOP_TSHL) {
        if (rep == TREP_INT) mov_imm64(e, 1, (uint64_t)pow3((int)k));
        return;
    }
    if (op == TOP_TSHR) {
        if (rep == TREP_INT) {
            int64_t h = (pow3((int)k) - 1) / 2;
            mov_imm64(e, 1, (uint64_t)pow3((int)k));
            if (h > 4095) mov_imm64(e, 5, (uint64_t)h);
        }
        return;
    }
    if (rep == TREP_PLANES) {
        /* Subtraction of a constant becomes addition of its negation. */
        int64_t v = (op == TOP_TSUB) ? -k : k;
        mov_imm64(e, 1, omega_t_to_planes(v));
    } else if (op == TOP_TAND || op == TOP_TOR || op == TOP_TXOR) {
        mov_imm64(e, 1, omega_t_to_planes(k));
    } else {
        mov_imm64(e, 1, (uint64_t)k);
    }
}

static uint32_t step_const_count(TernaryOp op, TernaryRep rep, int64_t k) {
    if (!op_takes_operand(op)) return 0;
    if (op == TOP_TSHL) return rep == TREP_INT ? mov_imm64_count((uint64_t)pow3((int)k)) : 0;
    if (op == TOP_TSHR) {
        if (rep != TREP_INT) return 0;
        int64_t h = (pow3((int)k) - 1) / 2;
        return mov_imm64_count((uint64_t)pow3((int)k)) + (h > 4095 ? mov_imm64_count((uint64_t)h) : 0);
    }
    if (rep == TREP_PLANES) return mov_imm64_count(omega_t_to_planes(op == TOP_TSUB ? -k : k));
    if (op == TOP_TAND || op == TOP_TOR || op == TOP_TXOR) return mov_imm64_count(omega_t_to_planes(k));
    return mov_imm64_count((uint64_t)k);
}

/* Step body with the operand already in X1. const_form: TSUB operand in
 * PLANES was pre-negated. */
static int emit_step_body(TEmit *e, TernaryOp op, TernaryRep rep, int64_t k,
                          const TStepFlags *fl, bool const_form) {
    bool clamp = !(fl && fl->elide_clamp);
    bool elide_mulh = fl && fl->elide_mulh;
    if (!shift_ok(op, k)) return -1;
    if (rep == TREP_INT) {
        switch (op) {
            case TOP_TNEG: neg_r(e, 0, 0); return 0;
            case TOP_TADD: add_r(e, 0, 0, 1); if (clamp) emit_clamp(e); return 0;
            case TOP_TSUB: sub_r(e, 0, 0, 1); if (clamp) emit_clamp(e); return 0;
            case TOP_TMUL: emit_int_mul(e, elide_mulh); if (clamp) emit_clamp(e); return 0;
            case TOP_TSHL: emit_int_mul(e, true); if (clamp) emit_clamp(e); return 0;
            case TOP_TSHR: emit_int_shr_body(e, (int)k); return 0;
            case TOP_TSIGN: cmp_i(e, 0, 0); cset(e, 2, C_GT); csinv(e, 0, 2, ZR, C_GE); return 0;
            case TOP_TCMP: cmp_r(e, 0, 1); cset(e, 2, C_GT); csinv(e, 0, 2, ZR, C_GE); return 0;
            case TOP_TAND: case TOP_TOR: case TOP_TXOR:
                /* No integer lowering of trit-wise logic: round-trip through planes. */
                if (!const_form) {
                    mov_r(e, true, R_SPILL, 0);
                    mov_r(e, true, 0, 1);
                    emit_to_planes(e);
                    mov_r(e, true, 1, 0);
                    mov_r(e, true, 0, R_SPILL);
                }
                emit_to_planes(e);
                emit_planes_logic(e, op);
                emit_from_planes(e);
                return 0;
            default: return -1;
        }
    }
    switch (op) {
        case TOP_TNEG: ror_i(e, 0, 0, 32); return 0;
        case TOP_TADD: emit_planes_add(e); return 0;
        case TOP_TSUB: if (!const_form) ror_i(e, 1, 1, 32); emit_planes_add(e); return 0;
        case TOP_TAND: case TOP_TOR: case TOP_TXOR: emit_planes_logic(e, op); return 0;
        case TOP_TSIGN: emit_planes_cmp(e, true); return 0;
        case TOP_TCMP: emit_planes_cmp(e, false); return 0;
        case TOP_TSHL: emit_planes_shl(e, (int)k); return 0;
        case TOP_TSHR: emit_planes_shr(e, (int)k); return 0;
        default: return -1;
    }
}

static bool step_needs_int_hoist(TernaryOp op, TernaryRep rep, const TStepFlags *fl) {
    if (rep != TREP_INT) return false;
    bool clamp = !(fl && fl->elide_clamp);
    switch (op) {
        case TOP_TADD: case TOP_TSUB: case TOP_TSHL: return clamp;
        case TOP_TMUL: return clamp || !(fl && fl->elide_mulh);
        default: return false;
    }
}

static bool step_needs_planes_hoist(TernaryOp op, TernaryRep rep) {
    return rep == TREP_PLANES && (op == TOP_TADD || op == TOP_TSUB || op == TOP_TSHL);
}

static void finish(TEmit *e, RealizationObject *out) {
    out->code_len = e->pos;
    out->target_profile = AARCH64_PROFILE_V8A_BAREMETAL;
    out->entry_offset = 0;
    omega_compute_realization_id(out);
}

int omega_t_a64_lower_chain(const TChain *chain, const TernaryRep *reps,
                            const TStepFlags *flags, RealizationObject *out) {
    if (!chain || !reps || !out || chain->n > TCHAIN_MAX_STEPS) return -1;
    memset(out, 0, sizeof(*out));
    TEmit e = { out->code_bytes, 0, sizeof(out->code_bytes), 0 };

    bool need_int = false, need_planes = false;
    for (size_t i = 0; i < chain->n; ++i) {
        const TStep *s = &chain->steps[i];
        if (!omega_t_a64_supported(s->op, reps[i]) || !shift_ok(s->op, s->k)) return -1;
        need_int |= step_needs_int_hoist(s->op, reps[i], flags ? &flags[i] : NULL);
        need_planes |= step_needs_planes_hoist(s->op, reps[i]);
    }
    if (need_int) hoist_int(&e);
    if (need_planes) hoist_planes(&e);

    TernaryRep cur = TREP_INT;
    for (size_t i = 0; i < chain->n; ++i) {
        const TStep *s = &chain->steps[i];
        if (reps[i] != cur) {
            if (reps[i] == TREP_PLANES) emit_to_planes(&e); else emit_from_planes(&e);
            cur = reps[i];
        }
        emit_step_const(&e, s->op, cur, s->k);
        if (emit_step_body(&e, s->op, cur, s->k, flags ? &flags[i] : NULL, true) != 0) return -1;
    }
    if (cur == TREP_PLANES) emit_from_planes(&e);
    ret(&e);
    if (e.err) return -1;
    finish(&e, out);
    return 0;
}

int omega_t_a64_lower_op(TernaryOp op, TernaryRep rep, int64_t k,
                         RealizationObject *out, uint32_t *out_body_insns,
                         uint32_t *out_prologue_insns) {
    if (!out || !omega_t_a64_supported(op, rep) || !shift_ok(op, k)) return -1;
    memset(out, 0, sizeof(*out));
    TEmit e = { out->code_bytes, 0, sizeof(out->code_bytes), 0 };
    TStepFlags fl = { false, false };
    size_t pro_start = e.pos;
    if (step_needs_int_hoist(op, rep, &fl)) hoist_int(&e);
    if (step_needs_planes_hoist(op, rep)) hoist_planes(&e);
    size_t body_start = e.pos;
    if (op == TOP_TSHL || op == TOP_TSHR) emit_step_const(&e, op, rep, k);
    if (emit_step_body(&e, op, rep, k, &fl, false) != 0) return -1;
    size_t body_end = e.pos;
    ret(&e);
    if (e.err) return -1;
    if (out_prologue_insns) *out_prologue_insns = (uint32_t)((body_start - pro_start) / 4);
    if (out_body_insns) *out_body_insns = (uint32_t)((body_end - body_start) / 4);
    finish(&e, out);
    return 0;
}

/* =========================================================================
 * Dynamic instruction-count model
 * ========================================================================= */

uint32_t omega_t_planes_add_iters(int64_t a, int64_t b) {
    uint64_t pa = omega_t_to_planes(a), pb = omega_t_to_planes(b);
    uint64_t ap = (uint32_t)pa, an = pa >> 32, bp = (uint32_t)pb, bn = pb >> 32;
    uint32_t iters = 0;
    uint64_t pp, nn;
    do {
        uint64_t bz = bp | bn, az = ap | an;
        pp = ap & bp;
        nn = an & bn;
        uint64_t sp = (ap & ~bz) | (bp & ~az) | nn;
        uint64_t sn = (an & ~bz) | (bn & ~az) | pp;
        bp = pp << 1;
        bn = nn << 1;
        ap = sp;
        an = sn;
        iters++;
    } while ((pp | nn) != 0);
    return iters;
}

uint64_t omega_t_a64_to_planes_dyn(int64_t v) {
    return TOP_DYN_SETUP + TOP_DYN_ITER * (uint64_t)omega_t_trit_len(v) + TOP_DYN_TAIL;
}

uint64_t omega_t_a64_from_planes_dyn(int64_t v) {
    return FROMP_DYN_SETUP + FROMP_DYN_ITER * (uint64_t)omega_t_trit_len(v);
}

static uint64_t planes_add_dyn(int64_t a, int64_t b) {
    return PADD_DYN_SETUP + PADD_DYN_ITER * (uint64_t)omega_t_planes_add_iters(a, b) + PADD_DYN_TAIL;
}

static uint32_t int_shr_body_count(int k) {
    int64_t h = (pow3(k) - 1) / 2;
    return h <= 4095 ? 7 : 8;
}

/* Body dynamics for the constant form used inside chains. */
uint64_t omega_t_a64_step_dyn(TernaryOp op, TernaryRep rep, int64_t a, int64_t b,
                              const TStepFlags *fl) {
    bool clamp = !(fl && fl->elide_clamp);
    bool elide_mulh = fl && fl->elide_mulh;
    if (rep == TREP_INT) {
        switch (op) {
            case TOP_TNEG: return 1;
            case TOP_TADD: case TOP_TSUB: return 1 + (clamp ? CLAMP_INSNS : 0);
            case TOP_TMUL: return (elide_mulh ? 1 : MULH_INSNS) + (clamp ? CLAMP_INSNS : 0);
            case TOP_TSHL: return 1 + (clamp ? CLAMP_INSNS : 0);
            case TOP_TSHR: return int_shr_body_count((int)b);
            case TOP_TSIGN: case TOP_TCMP: return 3;
            case TOP_TAND: case TOP_TOR: case TOP_TXOR: {
                int64_t r = 0;
                omega_t_eval(op, a, b, &r, NULL);
                return omega_t_a64_to_planes_dyn(a) + PLANES_LOGIC_INSNS[op - TOP_TAND] +
                       omega_t_a64_from_planes_dyn(r);
            }
            default: return 0;
        }
    }
    switch (op) {
        case TOP_TNEG: return 1;
        case TOP_TADD: return planes_add_dyn(a, b);
        case TOP_TSUB: return planes_add_dyn(a, -b);
        case TOP_TAND: case TOP_TOR: case TOP_TXOR: return PLANES_LOGIC_INSNS[op - TOP_TAND];
        case TOP_TSIGN: return 6;
        case TOP_TCMP: return 14;
        case TOP_TSHL: return 11;
        case TOP_TSHR: return 3;
        default: return 0;
    }
}

uint64_t omega_t_a64_chain_dyn(const TChain *chain, const TernaryRep *reps,
                               const TStepFlags *flags, int64_t x) {
    uint64_t n = 0;
    bool need_int = false, need_planes = false;
    for (size_t i = 0; i < chain->n; ++i) {
        need_int |= step_needs_int_hoist(chain->steps[i].op, reps[i], flags ? &flags[i] : NULL);
        need_planes |= step_needs_planes_hoist(chain->steps[i].op, reps[i]);
    }
    if (need_int) n += hoist_int_count();
    if (need_planes) n += HOIST_PLANES_COUNT;
    TernaryRep cur = TREP_INT;
    int64_t v = x;
    for (size_t i = 0; i < chain->n; ++i) {
        const TStep *s = &chain->steps[i];
        if (reps[i] != cur) {
            n += (reps[i] == TREP_PLANES) ? omega_t_a64_to_planes_dyn(v) : omega_t_a64_from_planes_dyn(v);
            cur = reps[i];
        }
        n += step_const_count(s->op, cur, s->k);
        n += omega_t_a64_step_dyn(s->op, cur, v, s->k, flags ? &flags[i] : NULL);
        int64_t r = 0;
        omega_t_eval(s->op, v, s->k, &r, NULL);
        v = r;
    }
    if (cur == TREP_PLANES) n += omega_t_a64_from_planes_dyn(v);
    return n + 1; /* RET */
}

uint32_t omega_t_binary_counterpart_insns(TernaryOp op) {
    return (op == TOP_TCMP || op == TOP_TSIGN) ? 3 : 1;
}

const char *omega_t_binary_counterpart_name(TernaryOp op) {
    switch (op) {
        case TOP_TNEG: return "neg";
        case TOP_TADD: return "add";
        case TOP_TSUB: return "sub";
        case TOP_TMUL: return "mul";
        case TOP_TAND: return "and";
        case TOP_TOR: return "orr";
        case TOP_TXOR: return "eor";
        case TOP_TCMP: return "cmp+cset+csinv";
        case TOP_TSIGN: return "cmp+cset+csinv";
        case TOP_TSHL: return "lsl";
        case TOP_TSHR: return "asr";
        default: return "?";
    }
}

int omega_t_a64_binary_extra(TBinaryExtra kind, int imm, RealizationObject *out) {
    if (!out || imm < 0 || imm > 63) return -1;
    memset(out, 0, sizeof(*out));
    TEmit e = { out->code_bytes, 0, sizeof(out->code_bytes), 0 };
    switch (kind) {
        case TBIN_NEG: neg_r(&e, 0, 0); break;
        case TBIN_LSL: lsl_i(&e, true, 0, 0, imm); break;
        case TBIN_ASR: asr_i(&e, 0, 0, imm); break;
        default: return -1;
    }
    ret(&e);
    if (e.err) return -1;
    finish(&e, out);
    return 0;
}

size_t omega_t_a64_encoder_samples(uint32_t *out_words, size_t max_words) {
    uint8_t buf[512];
    TEmit e = { buf, 0, sizeof(buf), 0 };
    add_r(&e, 0, 1, 2);
    sub_r(&e, 3, 4, 5);
    and_r(&e, true, 6, 7, 8);
    and_r(&e, false, 6, 7, 8);
    bic_r(&e, true, 15, 2, 6);
    orr_rs(&e, true, 0, 2, 3, SH_LSL, 32);
    orr_rs(&e, true, 3, 3, 3, SH_LSR, 32);
    eor_r(&e, 4, 0, 1);
    mov_r(&e, false, 2, 0);
    neg_r(&e, 10, 9);
    cmp_rs(&e, 3, 2, SH_ASR, 63);
    cmp_r(&e, 0, 9);
    add_i(&e, 8, 7, 3);
    sub_i(&e, 8, 6, 1);
    cmp_i(&e, 7, 1);
    cmn_i(&e, 7, 1);
    subs_i(&e, true, 5, 5, 1);
    csel(&e, 0, 9, 0, C_GT);
    csinc(&e, 6, 6, 6, C_LE);
    csinv(&e, 0, 2, ZR, C_GE);
    cset(&e, 4, C_HI);
    cset(&e, 5, C_LO);
    lsl_i(&e, true, 4, 4, 1);
    lsl_i(&e, false, 2, 0, 3);
    lsr_i(&e, true, 3, 0, 32);
    lsr_i(&e, false, 7, 2, 31);
    ubfx(&e, 2, 0, 3, 29);
    asr_i(&e, 1, 2, 63);
    ror_i(&e, 0, 0, 32);
    sdiv(&e, 6, 0, 5);
    msub(&e, 7, 6, 5, 0);
    madd(&e, 0, 0, 6, 7);
    mul(&e, 2, 0, 1);
    smulh(&e, 3, 0, 1);
    clz_w(&e, 4, 4);
    lslv_w(&e, 2, 2, 4);
    movz(&e, 5, 3, 0);
    movk(&e, 9, 0x34AA, 2);
    movn(&e, 1, 0x1234, 1);
    movn_w(&e, 13, 0);
    ret(&e);
    size_t n = e.pos / 4;
    if (n > max_words) n = max_words;
    for (size_t i = 0; i < n; ++i) {
        out_words[i] = (uint32_t)buf[4 * i] | ((uint32_t)buf[4 * i + 1] << 8) |
                       ((uint32_t)buf[4 * i + 2] << 16) | ((uint32_t)buf[4 * i + 3] << 24);
    }
    return n;
}
