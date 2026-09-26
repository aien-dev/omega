#ifndef AARCH64_DECODER_H
#define AARCH64_DECODER_H

#include "aarch64_target.h"
#include <stddef.h>

typedef enum {
    DECODED_INVALID = 0,
    DECODED_ADD,
    DECODED_SUB,
    DECODED_MUL,
    DECODED_AND,
    DECODED_ORR,
    DECODED_EOR,
    DECODED_MOVZ,
    DECODED_RET,
    DECODED_B,
    DECODED_B_COND,
    DECODED_CBZ,
    DECODED_CBNZ,
    DECODED_MOVK,
    DECODED_ADR,
    DECODED_LDR,
    DECODED_STR,
    DECODED_LDRB,
    DECODED_STRB,
    DECODED_SUBS
} DecodedOp;

typedef struct {
    DecodedOp op;
    bool sf;
    uint8_t rd;
    uint8_t rn;
    uint8_t rm;
    uint16_t imm16;
    int32_t branch_imm;
    Aarch64Cond cond;
} DecodedInsn;

int aarch64_decode_instruction(uint32_t insn, DecodedInsn *out_dec);
int aarch64_validate_code_buffer(const uint8_t *code, size_t len, char *err_msg, size_t err_msg_len);

#endif /* AARCH64_DECODER_H */
