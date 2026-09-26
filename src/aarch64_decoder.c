#include "aarch64_decoder.h"
#include <string.h>
#include <stdio.h>

int aarch64_decode_instruction(uint32_t insn, DecodedInsn *out_dec) {
    if (!out_dec) return -1;
    memset(out_dec, 0, sizeof(DecodedInsn));

    /* 1. RET */
    if (insn == 0xD65F03C0) {
        out_dec->op = DECODED_RET;
        out_dec->rn = 30;
        return 0;
    }

    /* 2. B (unconditional) */
    if ((insn & 0xFC000000) == 0x14000000) {
        out_dec->op = DECODED_B;
        out_dec->branch_imm = (int32_t)(insn & 0x03FFFFFF);
        return 0;
    }

    /* 3. B.cond */
    if ((insn & 0xFF000010) == 0x54000000) {
        out_dec->op = DECODED_B_COND;
        out_dec->cond = (Aarch64Cond)(insn & 0x0F);
        out_dec->branch_imm = (int32_t)((insn >> 5) & 0x7FFFF);
        return 0;
    }

    /* 4. CBZ */
    if ((insn & 0x7F000000) == 0x34000000) {
        out_dec->op = DECODED_CBZ;
        out_dec->sf = (insn >> 31) & 1;
        out_dec->rd = (uint8_t)(insn & 0x1F);
        out_dec->branch_imm = (int32_t)((insn >> 5) & 0x7FFFF);
        return 0;
    }

    /* 5. CBNZ */
    if ((insn & 0x7F000000) == 0x35000000) {
        out_dec->op = DECODED_CBNZ;
        out_dec->sf = (insn >> 31) & 1;
        out_dec->rd = (uint8_t)(insn & 0x1F);
        out_dec->branch_imm = (int32_t)((insn >> 5) & 0x7FFFF);
        return 0;
    }

    /* 6. MOVZ */
    if ((insn & 0x7F800000) == 0x52800000) {
        out_dec->op = DECODED_MOVZ;
        out_dec->sf = (insn >> 31) & 1;
        out_dec->rd = (uint8_t)(insn & 0x1F);
        out_dec->imm16 = (uint16_t)((insn >> 5) & 0xFFFF);
        return 0;
    }

    /* 7. MUL (MADD Xd, Xn, Xm, XZR) */
    if ((insn & 0x7FE07C00) == 0x1B007C00) {
        out_dec->op = DECODED_MUL;
        out_dec->sf = (insn >> 31) & 1;
        out_dec->rd = (uint8_t)(insn & 0x1F);
        out_dec->rn = (uint8_t)((insn >> 5) & 0x1F);
        out_dec->rm = (uint8_t)((insn >> 16) & 0x1F);
        return 0;
    }

    /* 8. Logical: AND, ORR, EOR */
    if ((insn & 0x7F200000) == 0x0A000000) {
        out_dec->op = DECODED_AND;
        out_dec->sf = (insn >> 31) & 1;
        out_dec->rd = (uint8_t)(insn & 0x1F);
        out_dec->rn = (uint8_t)((insn >> 5) & 0x1F);
        out_dec->rm = (uint8_t)((insn >> 16) & 0x1F);
        return 0;
    }
    if ((insn & 0x7F200000) == 0x2A000000) {
        out_dec->op = DECODED_ORR;
        out_dec->sf = (insn >> 31) & 1;
        out_dec->rd = (uint8_t)(insn & 0x1F);
        out_dec->rn = (uint8_t)((insn >> 5) & 0x1F);
        out_dec->rm = (uint8_t)((insn >> 16) & 0x1F);
        return 0;
    }
    if ((insn & 0x7F200000) == 0x4A000000) {
        out_dec->op = DECODED_EOR;
        out_dec->sf = (insn >> 31) & 1;
        out_dec->rd = (uint8_t)(insn & 0x1F);
        out_dec->rn = (uint8_t)((insn >> 5) & 0x1F);
        out_dec->rm = (uint8_t)((insn >> 16) & 0x1F);
        return 0;
    }

    /* 9. Arithmetic: ADD, SUB (shifted register) */
    if ((insn & 0x7F200000) == 0x0B000000) {
        out_dec->op = DECODED_ADD;
        out_dec->sf = (insn >> 31) & 1;
        out_dec->rd = (uint8_t)(insn & 0x1F);
        out_dec->rn = (uint8_t)((insn >> 5) & 0x1F);
        out_dec->rm = (uint8_t)((insn >> 16) & 0x1F);
        return 0;
    }
    if ((insn & 0x7F200000) == 0x4B000000) {
        out_dec->op = DECODED_SUB;
        out_dec->sf = (insn >> 31) & 1;
        out_dec->rd = (uint8_t)(insn & 0x1F);
        out_dec->rn = (uint8_t)((insn >> 5) & 0x1F);
        out_dec->rm = (uint8_t)((insn >> 16) & 0x1F);
        return 0;
    }

    /* 10. MOVK */
    if ((insn & 0x7F800000) == 0x72800000) {
        out_dec->op = DECODED_MOVK;
        out_dec->sf = (insn >> 31) & 1;
        out_dec->rd = (uint8_t)(insn & 0x1F);
        out_dec->imm16 = (uint16_t)((insn >> 5) & 0xFFFF);
        return 0;
    }

    /* 11. ADR */
    if ((insn & 0x9F000000) == 0x10000000) {
        out_dec->op = DECODED_ADR;
        out_dec->rd = (uint8_t)(insn & 0x1F);
        return 0;
    }

    /* 12. LDR / STR (unsigned offset) */
    if ((insn & 0xBF400000) == 0xB9400000) {
        out_dec->op = DECODED_LDR;
        out_dec->sf = (insn >> 30) & 1;
        out_dec->rd = (uint8_t)(insn & 0x1F);
        out_dec->rn = (uint8_t)((insn >> 5) & 0x1F);
        return 0;
    }
    if ((insn & 0xBF400000) == 0xB9000000) {
        out_dec->op = DECODED_STR;
        out_dec->sf = (insn >> 30) & 1;
        out_dec->rd = (uint8_t)(insn & 0x1F);
        out_dec->rn = (uint8_t)((insn >> 5) & 0x1F);
        return 0;
    }

    /* 13. LDR / STR (post-indexed 32-bit) */
    if ((insn & 0xFFE00400) == 0xB8400400) {
        out_dec->op = DECODED_LDR;
        out_dec->sf = false;
        out_dec->rd = (uint8_t)(insn & 0x1F);
        out_dec->rn = (uint8_t)((insn >> 5) & 0x1F);
        return 0;
    }
    if ((insn & 0xFFE00400) == 0xB8000400) {
        out_dec->op = DECODED_STR;
        out_dec->sf = false;
        out_dec->rd = (uint8_t)(insn & 0x1F);
        out_dec->rn = (uint8_t)((insn >> 5) & 0x1F);
        return 0;
    }

    /* 14. LDRB / STRB */
    if ((insn & 0xFFC00000) == 0x39400000) {
        out_dec->op = DECODED_LDRB;
        out_dec->rd = (uint8_t)(insn & 0x1F);
        out_dec->rn = (uint8_t)((insn >> 5) & 0x1F);
        return 0;
    }
    if ((insn & 0xFFC00000) == 0x39000000) {
        out_dec->op = DECODED_STRB;
        out_dec->rd = (uint8_t)(insn & 0x1F);
        out_dec->rn = (uint8_t)((insn >> 5) & 0x1F);
        return 0;
    }

    /* 15. SUBS (immediate & reg) */
    if ((insn & 0x7F800000) == 0x71000000) {
        out_dec->op = DECODED_SUBS;
        out_dec->sf = (insn >> 31) & 1;
        out_dec->rd = (uint8_t)(insn & 0x1F);
        out_dec->rn = (uint8_t)((insn >> 5) & 0x1F);
        return 0;
    }
    if ((insn & 0x7F200000) == 0x6B000000) {
        out_dec->op = DECODED_SUBS;
        out_dec->sf = (insn >> 31) & 1;
        out_dec->rd = (uint8_t)(insn & 0x1F);
        out_dec->rn = (uint8_t)((insn >> 5) & 0x1F);
        out_dec->rm = (uint8_t)((insn >> 16) & 0x1F);
        return 0;
    }

    out_dec->op = DECODED_INVALID;
    return -1;
}

int aarch64_validate_code_buffer(const uint8_t *code, size_t len, char *err_msg, size_t err_msg_len) {
    if (!code || len == 0) {
        snprintf(err_msg, err_msg_len, "Empty or null code buffer");
        return -1;
    }
    if (len % 4 != 0) {
        snprintf(err_msg, err_msg_len, "Code buffer length %zu is not a multiple of 4 bytes", len);
        return -1;
    }

    size_t insn_count = len / 4;
    DecodedInsn dec;

    for (size_t i = 0; i < insn_count; ++i) {
        size_t offset = i * 4;
        uint32_t insn = (uint32_t)code[offset] |
                        ((uint32_t)code[offset + 1] << 8) |
                        ((uint32_t)code[offset + 2] << 16) |
                        ((uint32_t)code[offset + 3] << 24);

        if (aarch64_decode_instruction(insn, &dec) != 0 || dec.op == DECODED_INVALID) {
            snprintf(err_msg, err_msg_len, "Undefined/unrecognized AArch64 opcode 0x%08x at offset 0x%zx", insn, offset);
            return -1;
        }

        /* Check that terminal instruction is RET */
        if (i == insn_count - 1 && dec.op != DECODED_RET) {
            snprintf(err_msg, err_msg_len, "Function does not terminate with RET (terminates with op %d)", dec.op);
            return -1;
        }
    }

    return 0;
}
