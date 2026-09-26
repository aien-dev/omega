#ifndef AARCH64_ENCODER_H
#define AARCH64_ENCODER_H

#include "aarch64_target.h"
#include <stddef.h>

int aarch64_emit_add_reg(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint8_t rn, uint8_t rm);
int aarch64_emit_sub_reg(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint8_t rn, uint8_t rm);
int aarch64_emit_mul_reg(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint8_t rn, uint8_t rm);
int aarch64_emit_and_reg(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint8_t rn, uint8_t rm);
int aarch64_emit_orr_reg(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint8_t rn, uint8_t rm);
int aarch64_emit_eor_reg(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint8_t rn, uint8_t rm);
int aarch64_emit_mov_reg(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint8_t rm);
int aarch64_emit_movz(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint16_t imm16, uint8_t shift);
int aarch64_emit_ret(uint8_t *buf, size_t *pos, size_t max_len);
int aarch64_emit_b(uint8_t *buf, size_t *pos, size_t max_len, int32_t imm26);
int aarch64_emit_b_cond(uint8_t *buf, size_t *pos, size_t max_len, Aarch64Cond cond, int32_t imm19);
int aarch64_emit_cbz(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rt, int32_t imm19);
int aarch64_emit_cbnz(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rt, int32_t imm19);

#endif /* AARCH64_ENCODER_H */
