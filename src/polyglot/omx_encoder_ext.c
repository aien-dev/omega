/* POLYGLOT-0 lane B2: AArch64 encodings missing from src/aarch64_encoder.c.
 * See omx_encoder_ext.h. Field layouts follow the Arm A64 encoding tables;
 * the fixed bits of each class are built from the named fields below, not
 * from disassembler output.
 *
 * DEBT: fold into src/aarch64_encoder.c + src/aarch64_decoder.c later.
 */
#include "polyglot/omx_encoder_ext.h"

#include <stddef.h>

#define REG_OK(r) ((r) <= 31u)
#define FAIL_IF(c) do { if (c) return -1; } while (0)

/* ---------- scalar data processing ---------- */

/* ADD/SUB (immediate): sf op S 1 0 0 0 1 0 sh imm12 Rn Rd */
static int addsub_imm(uint32_t *out, unsigned op, unsigned rd, unsigned rn, unsigned imm12, unsigned lsl12) {
    FAIL_IF(!out || !REG_OK(rd) || !REG_OK(rn) || imm12 > 0xFFFu || lsl12 > 1u);
    *out = (1u << 31) | (op << 30) | (0u << 29) | (0x22u << 23) | (lsl12 << 22) | (imm12 << 10) |
           (rn << 5) | rd;
    return 0;
}
int oxe_add_imm_x(uint32_t *out, unsigned rd, unsigned rn, unsigned imm12, unsigned lsl12) {
    return addsub_imm(out, 0, rd, rn, imm12, lsl12);
}
int oxe_sub_imm_x(uint32_t *out, unsigned rd, unsigned rn, unsigned imm12, unsigned lsl12) {
    return addsub_imm(out, 1, rd, rn, imm12, lsl12);
}

/* ADD (shifted register): sf 0 0 0 1 0 1 1 shift 0 Rm imm6 Rn Rd, shift LSL = 00 */
int oxe_add_lsl_x(uint32_t *out, unsigned rd, unsigned rn, unsigned rm, unsigned amount) {
    FAIL_IF(!out || !REG_OK(rd) || !REG_OK(rn) || !REG_OK(rm) || amount > 63u);
    *out = (1u << 31) | (0x0Bu << 24) | (0u << 22) | (rm << 16) | (amount << 10) | (rn << 5) | rd;
    return 0;
}

/* UBFM: sf opc=10 1 0 0 1 1 0 N immr imms Rn Rd; 64-bit requires N = 1 */
int oxe_ubfm_x(uint32_t *out, unsigned rd, unsigned rn, unsigned immr, unsigned imms) {
    FAIL_IF(!out || !REG_OK(rd) || !REG_OK(rn) || immr > 63u || imms > 63u);
    *out = (1u << 31) | (2u << 29) | (0x26u << 23) | (1u << 22) | (immr << 16) | (imms << 10) | (rn << 5) | rd;
    return 0;
}
int oxe_lsr_imm_x(uint32_t *out, unsigned rd, unsigned rn, unsigned shift) {
    FAIL_IF(shift > 63u);
    return oxe_ubfm_x(out, rd, rn, shift, 63u);
}
int oxe_lsl_imm_x(uint32_t *out, unsigned rd, unsigned rn, unsigned shift) {
    FAIL_IF(shift > 63u);
    return oxe_ubfm_x(out, rd, rn, (64u - shift) & 63u, 63u - shift);
}
int oxe_ubfx_x(uint32_t *out, unsigned rd, unsigned rn, unsigned lsb, unsigned width) {
    FAIL_IF(width < 1u || lsb > 63u || width > 64u - lsb);
    return oxe_ubfm_x(out, rd, rn, lsb, lsb + width - 1u);
}

/* Logical immediate: a 2,4,...,64-bit element, replicated, that is a rotated
 * run of 1..e-1 ones. Encoded as N:immr:imms (imms high bits give e). */
int oxe_bitmask_imm64(uint64_t imm, unsigned *n, unsigned *immr, unsigned *imms) {
    FAIL_IF(!n || !immr || !imms || imm == 0 || imm == ~(uint64_t)0);
    unsigned size = 64, lg = 6;
    while (size > 2) {
        unsigned half = size / 2;
        uint64_t mask = ((uint64_t)1 << half) - 1;
        if ((imm & mask) != ((imm >> half) & mask)) break;
        size = half;
        lg--;
    }
    uint64_t emask = size == 64 ? ~(uint64_t)0 : (((uint64_t)1 << size) - 1);
    uint64_t elt = imm & emask;
    unsigned ones = 0;
    for (unsigned i = 0; i < size; i++) ones += (unsigned)((elt >> i) & 1u);
    FAIL_IF(ones == 0 || ones == size);
    uint64_t run = ((uint64_t)1 << ones) - 1; /* ones < 64 here */
    /* element = ROR(run, r)  <=>  ROL(element, r) = run */
    for (unsigned r = 0; r < size; r++) {
        uint64_t rol = r == 0 ? elt : (((elt << r) | (elt >> (size - r))) & emask);
        if (rol == run) {
            *n = size == 64 ? 1u : 0u;
            *immr = r;
            *imms = ((0x3Fu << (lg + 1)) & 0x3Fu) | (ones - 1u);
            return 0;
        }
    }
    return -1; /* not a single rotated run */
}

/* Logical (immediate): sf opc 1 0 0 1 0 0 N immr imms Rn Rd */
static int logic_imm(uint32_t *out, unsigned opc, unsigned rd, unsigned rn, uint64_t imm) {
    unsigned n, immr, imms;
    FAIL_IF(!out || !REG_OK(rd) || !REG_OK(rn));
    FAIL_IF(oxe_bitmask_imm64(imm, &n, &immr, &imms));
    *out = (1u << 31) | (opc << 29) | (0x24u << 23) | (n << 22) | (immr << 16) | (imms << 10) | (rn << 5) | rd;
    return 0;
}
int oxe_and_imm_x(uint32_t *out, unsigned rd, unsigned rn, uint64_t imm) { return logic_imm(out, 0u, rd, rn, imm); }
int oxe_ands_imm_x(uint32_t *out, unsigned rd, unsigned rn, uint64_t imm) { return logic_imm(out, 3u, rd, rn, imm); }

/* Conditional select: sf op=0 S=0 1 1 0 1 0 1 0 0 Rm cond op2=01 Rn Rd (CSINC) */
int oxe_csinc_x(uint32_t *out, unsigned rd, unsigned rn, unsigned rm, unsigned cond) {
    FAIL_IF(!out || !REG_OK(rd) || !REG_OK(rn) || !REG_OK(rm) || cond > 15u);
    *out = (1u << 31) | (0xD4u << 21) | (rm << 16) | (cond << 12) | (1u << 10) | (rn << 5) | rd;
    return 0;
}
int oxe_cinc_x(uint32_t *out, unsigned rd, unsigned rn, unsigned cond) {
    FAIL_IF(cond > 13u); /* AL/NV are not allowed for CINC */
    return oxe_csinc_x(out, rd, rn, rn, cond ^ 1u);
}

/* Data processing (3 source): sf 0 0 1 1 0 1 1 op31=000 Rm o0=0 Ra Rn Rd (MADD) */
int oxe_madd_w(uint32_t *out, unsigned rd, unsigned rn, unsigned rm, unsigned ra) {
    FAIL_IF(!out || !REG_OK(rd) || !REG_OK(rn) || !REG_OK(rm) || !REG_OK(ra));
    *out = (0u << 31) | (0x1Bu << 24) | (rm << 16) | (ra << 10) | (rn << 5) | rd;
    return 0;
}

/* Move wide: sf opc=00 (MOVN) 1 0 0 1 0 1 hw imm16 Rd */
int oxe_movn_w(uint32_t *out, unsigned rd, unsigned imm16, unsigned hw) {
    FAIL_IF(!out || !REG_OK(rd) || imm16 > 0xFFFFu || hw > 1u);
    *out = (0u << 31) | (0u << 29) | (0x25u << 23) | (hw << 21) | (imm16 << 5) | rd;
    return 0;
}

/* PC-rel: op=1 immlo 1 0 0 0 0 immhi Rd (ADRP) */
int oxe_adrp(uint32_t *out, unsigned rd, int64_t page_delta) {
    FAIL_IF(!out || !REG_OK(rd) || page_delta < -(1ll << 20) || page_delta >= (1ll << 20));
    uint32_t imm = (uint32_t)page_delta & 0x1FFFFFu;
    *out = (1u << 31) | ((imm & 3u) << 29) | (0x10u << 24) | ((imm >> 2) << 5) | rd;
    return 0;
}

/* ---------- scalar loads/stores ---------- */

/* Load/store pair (offset): opc 1 0 1 V 0 1 0 L imm7 Rt2 Rn Rt */
int oxe_ldp_x_off(uint32_t *out, unsigned rt, unsigned rt2, unsigned rn, int offset) {
    FAIL_IF(!out || !REG_OK(rt) || !REG_OK(rt2) || !REG_OK(rn) || rt == 31u || rt2 == 31u || rt == rt2);
    FAIL_IF(offset % 8 != 0 || offset < -512 || offset > 504);
    uint32_t imm7 = (uint32_t)(offset / 8) & 0x7Fu;
    *out = (2u << 30) | (5u << 27) | (0u << 26) | (2u << 23) | (1u << 22) | (imm7 << 15) | (rt2 << 10) |
           (rn << 5) | rt;
    return 0;
}

/* Load/store register (immediate post-indexed):
 * size 1 1 1 V 0 0 opc 0 imm9 0 1 Rn Rt */
static int ldst_post(uint32_t *out, unsigned size, unsigned v, unsigned opc, unsigned rt, unsigned rn, int simm9) {
    FAIL_IF(!out || !REG_OK(rt) || !REG_OK(rn) || simm9 < -256 || simm9 > 255);
    uint32_t imm9 = (uint32_t)simm9 & 0x1FFu;
    *out = (size << 30) | (7u << 27) | (v << 26) | (opc << 22) | (imm9 << 12) | (1u << 10) | (rn << 5) | rt;
    return 0;
}
/* writeback base must differ from the transfer register for GP loads (UNPREDICTABLE otherwise) */
int oxe_ldrsb_w_post(uint32_t *out, unsigned rt, unsigned rn, int simm9) {
    FAIL_IF(rt == rn && rn != 31u);
    return ldst_post(out, 0u, 0u, 3u, rt, rn, simm9);
}
int oxe_ldrb_w_post(uint32_t *out, unsigned rt, unsigned rn, int simm9) {
    FAIL_IF(rt == rn && rn != 31u);
    return ldst_post(out, 0u, 0u, 1u, rt, rn, simm9);
}
int oxe_strb_w_post(uint32_t *out, unsigned rt, unsigned rn, int simm9) {
    FAIL_IF(rt == rn && rn != 31u);
    return ldst_post(out, 0u, 0u, 0u, rt, rn, simm9);
}

/* ---------- SIMD&FP loads/stores ---------- */

/* 128-bit: size=00, opc=11 load / 10 store; 32-bit S: size=10, opc=01 / 00 */
int oxe_ldr_q_post(uint32_t *out, unsigned qt, unsigned rn, int simm9) { return ldst_post(out, 0u, 1u, 3u, qt, rn, simm9); }
int oxe_str_q_post(uint32_t *out, unsigned qt, unsigned rn, int simm9) { return ldst_post(out, 0u, 1u, 2u, qt, rn, simm9); }
int oxe_str_s_post(uint32_t *out, unsigned st, unsigned rn, int simm9) { return ldst_post(out, 2u, 1u, 0u, st, rn, simm9); }

/* Load/store register (unsigned immediate): size 1 1 1 V 0 1 opc imm12 Rn Rt */
int oxe_ldr_q_uoff(uint32_t *out, unsigned qt, unsigned rn, unsigned offset) {
    FAIL_IF(!out || !REG_OK(qt) || !REG_OK(rn) || offset % 16u != 0 || offset / 16u > 0xFFFu);
    *out = (0u << 30) | (7u << 27) | (1u << 26) | (1u << 24) | (3u << 22) | ((offset / 16u) << 10) | (rn << 5) | qt;
    return 0;
}

/* STP Q (signed offset): opc=10 1 0 1 V=1 0 1 0 L=0 imm7 Rt2 Rn Rt, scale 16 */
int oxe_stp_q_off(uint32_t *out, unsigned qt, unsigned qt2, unsigned rn, int offset) {
    FAIL_IF(!out || !REG_OK(qt) || !REG_OK(qt2) || !REG_OK(rn));
    FAIL_IF(offset % 16 != 0 || offset < -1024 || offset > 1008);
    uint32_t imm7 = (uint32_t)(offset / 16) & 0x7Fu;
    *out = (2u << 30) | (5u << 27) | (1u << 26) | (2u << 23) | (0u << 22) | (imm7 << 15) | (qt2 << 10) |
           (rn << 5) | qt;
    return 0;
}

/* Advanced SIMD load/store multiple structures:
 *   no offset : 0 Q 0 0 1 1 0 0 0 L 0 0 0 0 0 0 opcode size Rn Rt
 *   post-index: 0 Q 0 0 1 1 0 0 1 L 0 Rm      opcode size Rn Rt (Rm = 11111: immediate = total bytes)
 * LD1 with four registers: opcode = 0010; .16b: Q = 1, size = 00. */
static int ld1_4(uint32_t *out, unsigned vt, unsigned rn, unsigned post) {
    FAIL_IF(!out || !REG_OK(vt) || !REG_OK(rn));
    *out = (1u << 30) | (0x0Cu << 24) | (post << 23) | (1u << 22) | (post ? (31u << 16) : 0u) | (2u << 12) |
           (0u << 10) | (rn << 5) | vt;
    return 0;
}
int oxe_ld1_4x16b(uint32_t *out, unsigned vt, unsigned rn) { return ld1_4(out, vt, rn, 0u); }
int oxe_ld1_4x16b_post(uint32_t *out, unsigned vt, unsigned rn, unsigned imm) {
    FAIL_IF(imm != 64u); /* immediate form: must equal 4 regs x 16 bytes */
    return ld1_4(out, vt, rn, 1u);
}

/* ---------- Advanced SIMD data processing ---------- */

/* Modified immediate: 0 Q op 0 1 1 1 1 0 0 0 0 0 a b c cmode o2 1 d e f g h Rd;
 * MOVI 8-bit: op = 0, cmode = 1110, o2 = 0 */
int oxe_movi_16b(uint32_t *out, unsigned vd, unsigned imm8) {
    FAIL_IF(!out || !REG_OK(vd) || imm8 > 0xFFu);
    *out = (1u << 30) | (0u << 29) | (0x1E0u << 19) | ((imm8 >> 5) << 16) | (0xEu << 12) | (0u << 11) |
           (1u << 10) | ((imm8 & 0x1Fu) << 5) | vd;
    return 0;
}

/* Three registers, same type: 0 Q U 0 1 1 1 0 size 1 Rm opcode 1 Rn Rd */
static int three_same(uint32_t *out, unsigned u, unsigned size, unsigned opcode, unsigned vd, unsigned vn, unsigned vm) {
    FAIL_IF(!out || !REG_OK(vd) || !REG_OK(vn) || !REG_OK(vm));
    *out = (1u << 30) | (u << 29) | (0x0Eu << 24) | (size << 22) | (1u << 21) | (vm << 16) | (opcode << 11) |
           (1u << 10) | (vn << 5) | vd;
    return 0;
}
int oxe_and_16b(uint32_t *out, unsigned vd, unsigned vn, unsigned vm) { return three_same(out, 0u, 0u, 0x03u, vd, vn, vm); }
int oxe_add_4s(uint32_t *out, unsigned vd, unsigned vn, unsigned vm) { return three_same(out, 0u, 2u, 0x10u, vd, vn, vm); }
int oxe_addp_4s(uint32_t *out, unsigned vd, unsigned vn, unsigned vm) { return three_same(out, 0u, 2u, 0x17u, vd, vn, vm); }

/* Three registers, extension (FEAT_DotProd):
 * 0 Q U 0 1 1 1 0 size 0 Rm 1 opcode(0010) 1 Rn Rd; SDOT: U = 0, size = 10 */
int oxe_sdot_4s(uint32_t *out, unsigned vd, unsigned vn, unsigned vm) {
    FAIL_IF(!out || !REG_OK(vd) || !REG_OK(vn) || !REG_OK(vm));
    *out = (1u << 30) | (0u << 29) | (0x0Eu << 24) | (2u << 22) | (0u << 21) | (vm << 16) | (1u << 15) |
           (0x2u << 11) | (1u << 10) | (vn << 5) | vd;
    return 0;
}

/* Across lanes: 0 Q U 0 1 1 1 0 size 1 1 0 0 0 opcode 1 0 Rn Rd; ADDV: opcode 11011 */
int oxe_addv_4s(uint32_t *out, unsigned sd, unsigned vn) {
    FAIL_IF(!out || !REG_OK(sd) || !REG_OK(vn));
    *out = (1u << 30) | (0x0Eu << 24) | (2u << 22) | (0x18u << 17) | (0x1Bu << 12) | (2u << 10) | (vn << 5) | sd;
    return 0;
}

/* Shift by immediate: 0 Q U 0 1 1 1 1 0 immh immb opcode 1 Rn Rd, immh != 0.
 * 8-bit elements: immh = 0001. SHL (opcode 01010): shift = immh:immb - 8.
 * SSHR (opcode 00000): shift = 16 - immh:immb. */
static int shift_imm(uint32_t *out, unsigned opcode, unsigned immhb, unsigned vd, unsigned vn) {
    FAIL_IF(!out || !REG_OK(vd) || !REG_OK(vn) || immhb < 8u || immhb > 15u);
    *out = (1u << 30) | (0u << 29) | (0x1Eu << 23) | (immhb << 16) | (opcode << 11) | (1u << 10) | (vn << 5) | vd;
    return 0;
}
int oxe_shl_16b(uint32_t *out, unsigned vd, unsigned vn, unsigned shift) {
    FAIL_IF(shift > 7u);
    return shift_imm(out, 0x0Au, 8u + shift, vd, vn);
}
int oxe_sshr_16b(uint32_t *out, unsigned vd, unsigned vn, unsigned shift) {
    FAIL_IF(shift < 1u || shift > 8u);
    return shift_imm(out, 0x00u, 16u - shift, vd, vn);
}
