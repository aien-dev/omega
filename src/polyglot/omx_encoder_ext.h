/* POLYGLOT-0 lane B2: AArch64 encodings that Omega's own encoder
 * (src/aarch64_encoder.c, scalar only) does not have yet, needed to emit the
 * B1 kernels (asm_sdot, asm_crumb) as Omega-owned instruction bytes.
 * spec/polyglot-0.md section 4.
 *
 * Every function is written from the Arm A64 encoding rules (Arm ARM, "A64
 * instruction set encoding": field layouts, not bytes copied from a
 * disassembler). Each validates every operand and fails closed: on any
 * out-of-range register, immediate, alignment or register list it returns -1
 * and leaves *out untouched; on success it stores the 32-bit instruction word
 * in *out and returns 0.
 *
 * Register numbers are 0..31. For GP registers, 31 means SP where the
 * instruction's field is an SP-capable base/destination (noted "SP") and
 * XZR/WZR elsewhere. Vector registers are v0..v31.
 *
 * DEBT: these belong in src/aarch64_encoder.c (and the matching decoder
 * cases in src/aarch64_decoder.c). That file is frozen for this program; fold
 * these back in a later, separately reviewed change.
 */
#ifndef OMX_ENCODER_EXT_H
#define OMX_ENCODER_EXT_H

#include <stdint.h>

/* ---- scalar data processing ---- */
/* ADD/SUB (immediate), 64-bit. rd, rn: SP. imm12 0..4095, lsl12 0 or 1. */
int oxe_add_imm_x(uint32_t *out, unsigned rd, unsigned rn, unsigned imm12, unsigned lsl12);
int oxe_sub_imm_x(uint32_t *out, unsigned rd, unsigned rn, unsigned imm12, unsigned lsl12);
/* ADD (shifted register), 64-bit, LSL #amount (0..63). rd, rn, rm: XZR. */
int oxe_add_lsl_x(uint32_t *out, unsigned rd, unsigned rn, unsigned rm, unsigned amount);
/* UBFM 64-bit and its aliases. */
int oxe_ubfm_x(uint32_t *out, unsigned rd, unsigned rn, unsigned immr, unsigned imms);
int oxe_lsr_imm_x(uint32_t *out, unsigned rd, unsigned rn, unsigned shift);          /* 0..63 */
int oxe_lsl_imm_x(uint32_t *out, unsigned rd, unsigned rn, unsigned shift);          /* 0..63 */
int oxe_ubfx_x(uint32_t *out, unsigned rd, unsigned rn, unsigned lsb, unsigned width); /* lsb+width <= 64 */
/* AND / ANDS (immediate), 64-bit; imm must be a valid bitmask immediate.
 * AND: rd SP, rn XZR. ANDS: rd XZR, rn XZR. */
int oxe_and_imm_x(uint32_t *out, unsigned rd, unsigned rn, uint64_t imm);
int oxe_ands_imm_x(uint32_t *out, unsigned rd, unsigned rn, uint64_t imm);
/* Bitmask-immediate encoder (N:immr:imms) for 64-bit logical immediates. */
int oxe_bitmask_imm64(uint64_t imm, unsigned *n, unsigned *immr, unsigned *imms);
/* CSINC 64-bit; CINC rd, rn, cond = CSINC rd, rn, rn, invert(cond). cond 0..13 for CINC. */
int oxe_csinc_x(uint32_t *out, unsigned rd, unsigned rn, unsigned rm, unsigned cond);
int oxe_cinc_x(uint32_t *out, unsigned rd, unsigned rn, unsigned cond);
/* MADD 32-bit: wd = wa + wn * wm. */
int oxe_madd_w(uint32_t *out, unsigned rd, unsigned rn, unsigned rm, unsigned ra);
/* MOVN 32-bit: wd = ~(imm16 << (16*hw)), hw 0..1. */
int oxe_movn_w(uint32_t *out, unsigned rd, unsigned imm16, unsigned hw);
/* ADRP: rd = page(pc) + page_delta * 4096; page_delta in [-2^20, 2^20). */
int oxe_adrp(uint32_t *out, unsigned rd, int64_t page_delta);

/* ---- scalar loads/stores ---- */
/* LDP 64-bit, signed offset; rn SP; offset multiple of 8 in [-512, 504]. */
int oxe_ldp_x_off(uint32_t *out, unsigned rt, unsigned rt2, unsigned rn, int offset);
/* Post-index byte loads/stores (simm9 in [-256, 255]); rn SP. */
int oxe_ldrsb_w_post(uint32_t *out, unsigned rt, unsigned rn, int simm9);
int oxe_ldrb_w_post(uint32_t *out, unsigned rt, unsigned rn, int simm9);
int oxe_strb_w_post(uint32_t *out, unsigned rt, unsigned rn, int simm9);

/* ---- SIMD&FP loads/stores ---- */
/* LDR/STR Qt post-index, simm9 in [-256, 255]; rn SP. */
int oxe_ldr_q_post(uint32_t *out, unsigned qt, unsigned rn, int simm9);
int oxe_str_q_post(uint32_t *out, unsigned qt, unsigned rn, int simm9);
/* STR St post-index, simm9 in [-256, 255]; rn SP. */
int oxe_str_s_post(uint32_t *out, unsigned st, unsigned rn, int simm9);
/* LDR Qt unsigned offset: offset multiple of 16 in [0, 65520]; rn SP. */
int oxe_ldr_q_uoff(uint32_t *out, unsigned qt, unsigned rn, unsigned offset);
/* STP Qt1, Qt2, signed offset: multiple of 16 in [-1024, 1008]; rn SP. */
int oxe_stp_q_off(uint32_t *out, unsigned qt, unsigned qt2, unsigned rn, int offset);
/* LD1 {vt.16b - v(t+3).16b}, [rn] and [rn], #64 (four consecutive registers,
 * modulo 32). imm must be exactly 64 for the post-index form. rn SP. */
int oxe_ld1_4x16b(uint32_t *out, unsigned vt, unsigned rn);
int oxe_ld1_4x16b_post(uint32_t *out, unsigned vt, unsigned rn, unsigned imm);

/* ---- Advanced SIMD ---- */
/* MOVI vd.16b, #imm8 (LSL #0). */
int oxe_movi_16b(uint32_t *out, unsigned vd, unsigned imm8);
/* SDOT vd.4s, vn.16b, vm.16b (FEAT_DotProd). */
int oxe_sdot_4s(uint32_t *out, unsigned vd, unsigned vn, unsigned vm);
/* AND vd.16b, vn.16b, vm.16b */
int oxe_and_16b(uint32_t *out, unsigned vd, unsigned vn, unsigned vm);
/* ADD / ADDP vd.4s, vn.4s, vm.4s */
int oxe_add_4s(uint32_t *out, unsigned vd, unsigned vn, unsigned vm);
int oxe_addp_4s(uint32_t *out, unsigned vd, unsigned vn, unsigned vm);
/* ADDV sd, vn.4s */
int oxe_addv_4s(uint32_t *out, unsigned sd, unsigned vn);
/* SHL vd.16b, vn.16b, #shift (0..7); SSHR vd.16b, vn.16b, #shift (1..8). */
int oxe_shl_16b(uint32_t *out, unsigned vd, unsigned vn, unsigned shift);
int oxe_sshr_16b(uint32_t *out, unsigned vd, unsigned vn, unsigned shift);

/* 29 new instruction encodings (34 entry points including aliases). */

#endif /* OMX_ENCODER_EXT_H */
