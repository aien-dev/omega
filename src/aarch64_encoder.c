/* Legacy AArch64 instruction writer (OSC-2 item 4, docs/osc/OSC-2-DESIGN.md section 4).
 *
 * Every emitter validates every field against its exact architectural range BEFORE
 * encoding. An in-range call produces the same word as the pre-OSC-2 writer (the
 * encoding formulas below are unchanged; their masks are now no-ops). An out-of-range
 * field is REFUSED, never trimmed: the emitter writes nothing, prints
 *     A64_LEGACY_FIELD_OUT_OF_RANGE <emitter> <field>=<value>
 * on stderr and aborts the process (SIGABRT). Abort is deliberate: several existing
 * callers ignore the int result, so a soft error would silently drop an instruction.
 * The -1 result remains only for "buffer full" (unchanged).
 * Ranges: registers 0..31 (31 = XZR/SP where the instruction allows it); sf is a C bool;
 * imm16 is uint16_t (0..65535); movz/movk shift in {0,16,32,48} (sf=1) or {0,16} (sf=0);
 * b imm26 in [-2^25, 2^25); b.cond/cbz/cbnz imm19 in [-2^18, 2^18); cond 0..15;
 * adr imm21 in [-2^20, 2^20); ldr/str (unsigned offset) uoff a multiple of 8 (sf=1) or
 * 4 (sf=0) with uoff/size <= 4095; ldrb/strb uoff 0..4095; post-index simm9 in [-256, 255];
 * subs imm12 0..4095.
 * New code must not use this writer: the set of files that include this header or call
 * aarch64_emit_* is frozen (tests/compiler/legacy_a64_allowlist.txt, checked by
 * `make test-compiler`). New code uses the validating encoder src/compiler/osc_a64.c. */
#include "aarch64_encoder.h"
#include <stdio.h>
#include <stdlib.h>

static void refuse(const char *emitter, const char *field, long long value) __attribute__((noreturn));
static void refuse(const char *emitter, const char *field, long long value) {
    fprintf(stderr, "A64_LEGACY_FIELD_OUT_OF_RANGE %s %s=%lld\n", emitter, field, value);
    fflush(stderr);
    abort();
}

#define CHECK_REG(r) do { if ((unsigned)(r) > 31u) refuse(__func__, #r, (long long)(r)); } while (0)
#define CHECK_SIGNED(v, bits) \
    do { if ((long long)(v) < -(1LL << ((bits) - 1)) || (long long)(v) >= (1LL << ((bits) - 1))) \
             refuse(__func__, #v, (long long)(v)); } while (0)
#define CHECK_UMAX(v, max) do { if ((unsigned long long)(v) > (unsigned long long)(max)) refuse(__func__, #v, (long long)(v)); } while (0)
#define CHECK_COND(c) do { if ((int)(c) < 0 || (int)(c) > 15) refuse(__func__, #c, (long long)(int)(c)); } while (0)
#define CHECK_HW_SHIFT(sf, shift) \
    do { if ((shift) % 16u != 0 || (shift) > ((sf) ? 48u : 16u)) refuse(__func__, #shift, (long long)(shift)); } while (0)
#define CHECK_SCALED(sf, uoff) \
    do { unsigned sz_ = (sf) ? 8u : 4u; \
         if ((uoff) % sz_ != 0 || (uoff) / sz_ > 0xFFFu) refuse(__func__, #uoff, (long long)(uoff)); } while (0)
#define CHECK3(rd, rn, rm) do { CHECK_REG(rd); CHECK_REG(rn); CHECK_REG(rm); } while (0)

static int emit_u32_le(uint8_t *buf, size_t *pos, size_t max_len, uint32_t insn) {
    if (*pos + 4 > max_len) return -1;
    buf[*pos + 0] = (uint8_t)(insn & 0xff);
    buf[*pos + 1] = (uint8_t)((insn >> 8) & 0xff);
    buf[*pos + 2] = (uint8_t)((insn >> 16) & 0xff);
    buf[*pos + 3] = (uint8_t)((insn >> 24) & 0xff);
    *pos += 4;
    return 0;
}

int aarch64_emit_add_reg(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint8_t rn, uint8_t rm) {
    CHECK3(rd, rn, rm);
    uint32_t insn = ((uint32_t)sf << 31) | (0x0B << 24) |
                    ((uint32_t)(rm & 0x1f) << 16) |
                    ((uint32_t)(rn & 0x1f) << 5) |
                    (rd & 0x1f);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_sub_reg(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint8_t rn, uint8_t rm) {
    CHECK3(rd, rn, rm);
    uint32_t insn = ((uint32_t)sf << 31) | (1U << 30) | (0x0B << 24) |
                    ((uint32_t)(rm & 0x1f) << 16) |
                    ((uint32_t)(rn & 0x1f) << 5) |
                    (rd & 0x1f);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_mul_reg(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint8_t rn, uint8_t rm) {
    CHECK3(rd, rn, rm);
    /* MADD Xd, Xn, Xm, XZR */
    uint32_t insn = ((uint32_t)sf << 31) | (0x1B << 24) |
                    ((uint32_t)(rm & 0x1f) << 16) |
                    (0x1f << 10) |
                    ((uint32_t)(rn & 0x1f) << 5) |
                    (rd & 0x1f);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_and_reg(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint8_t rn, uint8_t rm) {
    CHECK3(rd, rn, rm);
    uint32_t insn = ((uint32_t)sf << 31) | (0x0A << 24) |
                    ((uint32_t)(rm & 0x1f) << 16) |
                    ((uint32_t)(rn & 0x1f) << 5) |
                    (rd & 0x1f);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_orr_reg(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint8_t rn, uint8_t rm) {
    CHECK3(rd, rn, rm);
    uint32_t insn = ((uint32_t)sf << 31) | (0x2A << 24) |
                    ((uint32_t)(rm & 0x1f) << 16) |
                    ((uint32_t)(rn & 0x1f) << 5) |
                    (rd & 0x1f);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_eor_reg(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint8_t rn, uint8_t rm) {
    CHECK3(rd, rn, rm);
    uint32_t insn = ((uint32_t)sf << 31) | (0x4A << 24) |
                    ((uint32_t)(rm & 0x1f) << 16) |
                    ((uint32_t)(rn & 0x1f) << 5) |
                    (rd & 0x1f);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_mov_reg(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint8_t rm) {
    CHECK_REG(rd); CHECK_REG(rm);
    return aarch64_emit_orr_reg(buf, pos, max_len, sf, rd, REG_XZR, rm);
}

int aarch64_emit_movz(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint16_t imm16, uint8_t shift) {
    CHECK_REG(rd); CHECK_HW_SHIFT(sf, shift);
    uint32_t hw = (shift / 16) & 3;
    uint32_t insn = ((uint32_t)sf << 31) | (0x52800000) | (hw << 21) |
                    ((uint32_t)imm16 << 5) | (rd & 0x1f);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_ret(uint8_t *buf, size_t *pos, size_t max_len) {
    return emit_u32_le(buf, pos, max_len, 0xD65F03C0);
}

int aarch64_emit_b(uint8_t *buf, size_t *pos, size_t max_len, int32_t imm26) {
    CHECK_SIGNED(imm26, 26);
    uint32_t insn = 0x14000000 | ((uint32_t)imm26 & 0x03FFFFFF);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_b_cond(uint8_t *buf, size_t *pos, size_t max_len, Aarch64Cond cond, int32_t imm19) {
    CHECK_COND(cond); CHECK_SIGNED(imm19, 19);
    uint32_t insn = 0x54000000 | (((uint32_t)imm19 & 0x7FFFF) << 5) | ((uint32_t)cond & 0x0F);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_cbz(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rt, int32_t imm19) {
    CHECK_REG(rt); CHECK_SIGNED(imm19, 19);
    uint32_t insn = ((uint32_t)sf << 31) | 0x34000000 | (((uint32_t)imm19 & 0x7FFFF) << 5) | (rt & 0x1F);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_cbnz(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rt, int32_t imm19) {
    CHECK_REG(rt); CHECK_SIGNED(imm19, 19);
    uint32_t insn = ((uint32_t)sf << 31) | 0x35000000 | (((uint32_t)imm19 & 0x7FFFF) << 5) | (rt & 0x1F);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_movk(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint16_t imm16, uint8_t shift) {
    CHECK_REG(rd); CHECK_HW_SHIFT(sf, shift);
    uint32_t hw = (shift / 16) & 3;
    uint32_t insn = ((uint32_t)sf << 31) | 0x72800000 | (hw << 21) |
                    ((uint32_t)imm16 << 5) | (rd & 0x1f);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_adr(uint8_t *buf, size_t *pos, size_t max_len, uint8_t rd, int32_t imm21) {
    CHECK_REG(rd); CHECK_SIGNED(imm21, 21);
    uint32_t immlo = ((uint32_t)imm21 & 3) << 29;
    uint32_t immhi = (((uint32_t)imm21 >> 2) & 0x7FFFF) << 5;
    uint32_t insn = 0x10000000 | immlo | immhi | (rd & 0x1F);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_ldr_uoff(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rt, uint8_t rn, uint16_t uoff) {
    CHECK_REG(rt); CHECK_REG(rn); CHECK_SCALED(sf, uoff);
    uint32_t scale = sf ? 3 : 2;
    uint32_t imm12 = ((uint32_t)uoff >> scale) & 0xFFF;
    uint32_t base = sf ? 0xF9400000 : 0xB9400000;
    uint32_t insn = base | (imm12 << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rt & 0x1F);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_str_uoff(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rt, uint8_t rn, uint16_t uoff) {
    CHECK_REG(rt); CHECK_REG(rn); CHECK_SCALED(sf, uoff);
    uint32_t scale = sf ? 3 : 2;
    uint32_t imm12 = ((uint32_t)uoff >> scale) & 0xFFF;
    uint32_t base = sf ? 0xF9000000 : 0xB9000000;
    uint32_t insn = base | (imm12 << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rt & 0x1F);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_ldrb_uoff(uint8_t *buf, size_t *pos, size_t max_len, uint8_t rt, uint8_t rn, uint16_t uoff) {
    CHECK_REG(rt); CHECK_REG(rn); CHECK_UMAX(uoff, 0xFFF);
    uint32_t insn = 0x39400000 | (((uint32_t)uoff & 0xFFF) << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rt & 0x1F);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_strb_uoff(uint8_t *buf, size_t *pos, size_t max_len, uint8_t rt, uint8_t rn, uint16_t uoff) {
    CHECK_REG(rt); CHECK_REG(rn); CHECK_UMAX(uoff, 0xFFF);
    uint32_t insn = 0x39000000 | (((uint32_t)uoff & 0xFFF) << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rt & 0x1F);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_ldr_post(uint8_t *buf, size_t *pos, size_t max_len, uint8_t rt, uint8_t rn, int16_t simm9) {
    CHECK_REG(rt); CHECK_REG(rn); CHECK_SIGNED(simm9, 9);
    uint32_t insn = 0xB8400400 | (((uint32_t)simm9 & 0x1FF) << 12) | ((uint32_t)(rn & 0x1F) << 5) | (rt & 0x1F);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_str_post(uint8_t *buf, size_t *pos, size_t max_len, uint8_t rt, uint8_t rn, int16_t simm9) {
    CHECK_REG(rt); CHECK_REG(rn); CHECK_SIGNED(simm9, 9);
    uint32_t insn = 0xB8000400 | (((uint32_t)simm9 & 0x1FF) << 12) | ((uint32_t)(rn & 0x1F) << 5) | (rt & 0x1F);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_ldr_x_post(uint8_t *buf, size_t *pos, size_t max_len, uint8_t rt, uint8_t rn, int16_t simm9) {
    CHECK_REG(rt); CHECK_REG(rn); CHECK_SIGNED(simm9, 9);
    uint32_t insn = 0xF8400400 | (((uint32_t)simm9 & 0x1FF) << 12) | ((uint32_t)(rn & 0x1F) << 5) | (rt & 0x1F);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_str_x_post(uint8_t *buf, size_t *pos, size_t max_len, uint8_t rt, uint8_t rn, int16_t simm9) {
    CHECK_REG(rt); CHECK_REG(rn); CHECK_SIGNED(simm9, 9);
    uint32_t insn = 0xF8000400 | (((uint32_t)simm9 & 0x1FF) << 12) | ((uint32_t)(rn & 0x1F) << 5) | (rt & 0x1F);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_subs_imm(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint8_t rn, uint16_t imm12) {
    CHECK_REG(rd); CHECK_REG(rn); CHECK_UMAX(imm12, 0xFFF);
    uint32_t base = sf ? 0xF1000000 : 0x71000000;
    uint32_t insn = base | (((uint32_t)imm12 & 0xFFF) << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_subs_reg(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint8_t rn, uint8_t rm) {
    CHECK3(rd, rn, rm);
    uint32_t base = sf ? 0xEB000000 : 0x6B000000;
    uint32_t insn = base | ((uint32_t)(rm & 0x1F) << 16) | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
    return emit_u32_le(buf, pos, max_len, insn);
}
