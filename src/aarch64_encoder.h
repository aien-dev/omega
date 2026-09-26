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
int aarch64_emit_movk(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint16_t imm16, uint8_t shift);
int aarch64_emit_adr(uint8_t *buf, size_t *pos, size_t max_len, uint8_t rd, int32_t imm21);
int aarch64_emit_ldr_uoff(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rt, uint8_t rn, uint16_t uoff);
int aarch64_emit_str_uoff(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rt, uint8_t rn, uint16_t uoff);
int aarch64_emit_ldrb_uoff(uint8_t *buf, size_t *pos, size_t max_len, uint8_t rt, uint8_t rn, uint16_t uoff);
int aarch64_emit_strb_uoff(uint8_t *buf, size_t *pos, size_t max_len, uint8_t rt, uint8_t rn, uint16_t uoff);
int aarch64_emit_ldr_post(uint8_t *buf, size_t *pos, size_t max_len, uint8_t rt, uint8_t rn, int16_t simm9);
int aarch64_emit_str_post(uint8_t *buf, size_t *pos, size_t max_len, uint8_t rt, uint8_t rn, int16_t simm9);
int aarch64_emit_subs_imm(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint8_t rn, uint16_t imm12);
int aarch64_emit_subs_reg(uint8_t *buf, size_t *pos, size_t max_len, bool sf, uint8_t rd, uint8_t rn, uint8_t rm);

#endif /* AARCH64_ENCODER_H */
