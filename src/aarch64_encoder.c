#include "aarch64_encoder.h"

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
    uint32_t insn = ((uint32_t)sf << 31) | (0x0B << 24) |
                    ((uint32_t)(rm & 0x1f) << 16) |
                    ((uint32_t)(rn & 0x1f) << 5) |
                    (rd & 0x1f);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_sub_reg(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint8_t rn, uint8_t rm) {
    uint32_t insn = ((uint32_t)sf << 31) | (1U << 30) | (0x0B << 24) |
                    ((uint32_t)(rm & 0x1f) << 16) |
                    ((uint32_t)(rn & 0x1f) << 5) |
                    (rd & 0x1f);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_mul_reg(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint8_t rn, uint8_t rm) {
    /* MADD Xd, Xn, Xm, XZR */
    uint32_t insn = ((uint32_t)sf << 31) | (0x1B << 24) |
                    ((uint32_t)(rm & 0x1f) << 16) |
                    (0x1f << 10) |
                    ((uint32_t)(rn & 0x1f) << 5) |
                    (rd & 0x1f);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_and_reg(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint8_t rn, uint8_t rm) {
    uint32_t insn = ((uint32_t)sf << 31) | (0x0A << 24) |
                    ((uint32_t)(rm & 0x1f) << 16) |
                    ((uint32_t)(rn & 0x1f) << 5) |
                    (rd & 0x1f);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_orr_reg(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint8_t rn, uint8_t rm) {
    uint32_t insn = ((uint32_t)sf << 31) | (0x2A << 24) |
                    ((uint32_t)(rm & 0x1f) << 16) |
                    ((uint32_t)(rn & 0x1f) << 5) |
                    (rd & 0x1f);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_eor_reg(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint8_t rn, uint8_t rm) {
    uint32_t insn = ((uint32_t)sf << 31) | (0x4A << 24) |
                    ((uint32_t)(rm & 0x1f) << 16) |
                    ((uint32_t)(rn & 0x1f) << 5) |
                    (rd & 0x1f);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_mov_reg(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint8_t rm) {
    return aarch64_emit_orr_reg(buf, pos, max_len, sf, rd, REG_XZR, rm);
}

int aarch64_emit_movz(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint16_t imm16, uint8_t shift) {
    uint32_t hw = (shift / 16) & 3;
    uint32_t insn = ((uint32_t)sf << 31) | (0x52800000) | (hw << 21) |
                    ((uint32_t)imm16 << 5) | (rd & 0x1f);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_ret(uint8_t *buf, size_t *pos, size_t max_len) {
    return emit_u32_le(buf, pos, max_len, 0xD65F03C0);
}

int aarch64_emit_b(uint8_t *buf, size_t *pos, size_t max_len, int32_t imm26) {
    uint32_t insn = 0x14000000 | ((uint32_t)imm26 & 0x03FFFFFF);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_b_cond(uint8_t *buf, size_t *pos, size_t max_len, Aarch64Cond cond, int32_t imm19) {
    uint32_t insn = 0x54000000 | (((uint32_t)imm19 & 0x7FFFF) << 5) | ((uint32_t)cond & 0x0F);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_cbz(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rt, int32_t imm19) {
    uint32_t insn = ((uint32_t)sf << 31) | 0x34000000 | (((uint32_t)imm19 & 0x7FFFF) << 5) | (rt & 0x1F);
    return emit_u32_le(buf, pos, max_len, insn);
}

int aarch64_emit_cbnz(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rt, int32_t imm19) {
    uint32_t insn = ((uint32_t)sf << 31) | 0x35000000 | (((uint32_t)imm19 & 0x7FFFF) << 5) | (rt & 0x1F);
    return emit_u32_le(buf, pos, max_len, insn);
}
