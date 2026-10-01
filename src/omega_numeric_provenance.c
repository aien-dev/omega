#include "omega_numeric_provenance.h"
#include "omega_numeric.h"
#include "omega_blackwell_encoder.h"

#include <stdio.h>
#include <string.h>

#define ORACLE "nvdisasm 13.0.88 -b SM121 decode of these words (2026-09-30); " \
               "scoreboard pattern from ptxas 13.0.88 sm_121; GB10 parity: receipt only"
#define ORACLE_E1 "nvdisasm 13.0.88 -b SM121 decode of these words (2026-09-30, E1 WP-C); " \
               "scoreboard pattern from ptxas 13.0.88 sm_121; GB10 parity: receipt only"

static const OmegaOpcodeProvenance PROVENANCE_TABLE[] = {
    { "FADD", "FADD R9, R2, R5", 0x7221, "FP32 add, RNE, subnormals kept", ORACLE,
      0x02097221, 0x00000005, 0x00000000, false },
    { "FSUB", "FADD R9, R2, -R5", 0x7221, "FP32 subtract (FADD, src2 negate bit 63)", ORACLE,
      0x02097221, 0x80000005, 0x00000000, false },
    { "FMUL", "FMUL R9, R2, R5", 0x7220, "FP32 multiply, RNE, subnormals kept", ORACLE,
      0x02097220, 0x00000005, 0x00400000, false },
    { "FFMA", "FFMA R9, R2, R5, R1", 0x7223, "FP32 fused multiply-add, c in R1", ORACLE,
      0x02097223, 0x00000005, 0x00000001, false },
    { "FSETP_GE_R2_R5", "FSETP.GE.AND P0, PT, R2, R5, PT", 0x720b,
      "ordered >= (false on NaN); the earlier 0x03f0e000 form decodes as GEU", ORACLE,
      0x0200720b, 0x00000005, 0x03f06000, false },
    { "FSEL_R2_R5_P0", "FSEL R9, R2, R5, P0", 0x7208, "select a if P0 else b", ORACLE,
      0x02097208, 0x00000005, 0x00000000, false },
    { "FSETP_GE_R2_RZ", "FSETP.GE.AND P0, PT, R2, RZ, PT", 0x720b, "ordered a >= +0", ORACLE,
      0x0200720b, 0x000000ff, 0x03f06000, false },
    { "FSEL_R5_R2_P0", "FSEL R9, R5, R2, P0", 0x7208, "select b if P0 else a", ORACLE,
      0x05097208, 0x00000002, 0x00000000, false },
    { "FMNMX_MIN", "FMNMX R9, R2, R5, PT", 0x7209, "FP32 minimum", ORACLE,
      0x02097209, 0x00000005, 0x03800000, false },
    { "FMNMX_MAX", "FMNMX R9, R2, R5, !PT", 0x7209, "FP32 maximum", ORACLE,
      0x02097209, 0x00000005, 0x07800000, false },
    { "I2FP", "I2FP.F32.S32 R9, R2", 0x7245, "int32 to FP32, RNE", ORACLE,
      0x00097245, 0x00000002, 0x00201400, false },
    { "F2I", "F2I.TRUNC.NTZ R9, R2", 0x7305, "FP32 to int32, truncate", ORACLE,
      0x00097305, 0x00000002, 0x0020f100, true },
    { "MUFU_RCP", "MUFU.RCP R9, R2", 0x7308, "reciprocal seed (never bit-compared)", ORACLE,
      0x00097308, 0x00000002, 0x00001000, true },
    { "MUFU_RSQ", "MUFU.RSQ R9, R2", 0x7308, "reciprocal square root seed (never bit-compared)", ORACLE,
      0x00097308, 0x00000002, 0x00001400, true },
    { "SHFL_DOWN_1", "SHFL.DOWN PT, R9, R2, 0x1, 0x1f", 0x7f89, "warp shuffle down by one lane", ORACLE,
      0x02097f89, 0x08201f00, 0x000e0000, true },
    { "SHFL_DOWN_16", "SHFL.DOWN PT, R9, R2, 0x10, 0x1f", 0x7f89, "declared-order reduction step 1: lane i + 16", ORACLE,
      0x02097f89, 0x0a001f00, 0x000e0000, true },
    { "SHFL_DOWN_8", "SHFL.DOWN PT, R9, R2, 0x8, 0x1f", 0x7f89, "declared-order reduction step 2: lane i + 8", ORACLE,
      0x02097f89, 0x09001f00, 0x000e0000, true },
    { "SHFL_DOWN_4", "SHFL.DOWN PT, R9, R2, 0x4, 0x1f", 0x7f89, "declared-order reduction step 3: lane i + 4", ORACLE,
      0x02097f89, 0x08801f00, 0x000e0000, true },
    { "SHFL_DOWN_2", "SHFL.DOWN PT, R9, R2, 0x2, 0x1f", 0x7f89, "declared-order reduction step 4: lane i + 2", ORACLE,
      0x02097f89, 0x08401f00, 0x000e0000, true },
    { "FADD_R2_R2_R9", "FADD R2, R2, R9", 0x7221, "running sum += shuffled value (RNE, subnormals kept)", ORACLE,
      0x02027221, 0x00000009, 0x00000000, false },
    { "FADD_R9_R2_R9", "FADD R9, R2, R9", 0x7221, "last reduction step, result into R9 for the STG", ORACLE,
      0x02097221, 0x00000009, 0x00000000, false },
    { "SHF_L_R8_R0_2", "SHF.L.U32 R8, R0, 0x2, RZ", 0x7819, "shared byte offset of word tid.x (tid * 4)", ORACLE,
      0x00087819, 0x00000002, 0x000006ff, false },
    { "LOP3_R10_R8_XOR_FC", "LOP3.LUT R10, R8, 0xfc, RZ, 0x3c, !PT", 0x7812,
      "partner offset (tid ^ 63) * 4, bounded by the 64-thread CTA", ORACLE,
      0x080a7812, 0x000000fc, 0x078e3cff, false },
    { "STS_R8_R2", "STS [R8+URZ], R2", 0x7988,
      "32-bit shared store at raw offset R8 (NVK/NAK form; ptxas bases on SR_CgaCtaId + 0x400, "
      "first suspect if the chip disagrees)", ORACLE,
      0x08007988, 0x00000002, 0x080008ff, false },
    { "BAR_SYNC_0", "BAR.SYNC.DEFER_BLOCKING 0x0", 0x7b1d, "CTA barrier 0 between the store and the load", ORACLE,
      0x00007b1d, 0x00000000, 0x00010000, false },
    { "LDS_R9_R10", "LDS R9, [R10+URZ]", 0x7984, "32-bit shared load of the partner word", ORACLE,
      0x0a097984, 0x000000ff, 0x08000800, true },
    /* ---- E1 scalar contract ops (E1 WP-C) ---- */
#define FSETP_ROW(key_, txt_, code_, desc_) \
    { key_, txt_, 0x720b, desc_, ORACLE_E1, 0x0200720b, 0x00000005, 0x03f00000u | ((code_) << 12), false }
    FSETP_ROW("FSETP_LT_R2_R5",  "FSETP.LT.AND P0, PT, R2, R5, PT",  0x1u, "ordered a < b (false on NaN)"),
    FSETP_ROW("FSETP_EQ_R2_R5",  "FSETP.EQ.AND P0, PT, R2, R5, PT",  0x2u, "ordered a == b (false on NaN)"),
    FSETP_ROW("FSETP_LE_R2_R5",  "FSETP.LE.AND P0, PT, R2, R5, PT",  0x3u, "ordered a <= b (false on NaN)"),
    FSETP_ROW("FSETP_GT_R2_R5",  "FSETP.GT.AND P0, PT, R2, R5, PT",  0x4u, "ordered a > b (false on NaN)"),
    FSETP_ROW("FSETP_NE_R2_R5",  "FSETP.NE.AND P0, PT, R2, R5, PT",  0x5u, "ordered a != b (false on NaN)"),
    FSETP_ROW("FSETP_NUM_R2_R5", "FSETP.NUM.AND P0, PT, R2, R5, PT", 0x7u, "neither a nor b is NaN"),
    FSETP_ROW("FSETP_NAN_R2_R5", "FSETP.NAN.AND P0, PT, R2, R5, PT", 0x8u, "a or b is NaN"),
    FSETP_ROW("FSETP_LTU_R2_R5", "FSETP.LTU.AND P0, PT, R2, R5, PT", 0x9u, "unordered a < b (true on NaN)"),
    FSETP_ROW("FSETP_EQU_R2_R5", "FSETP.EQU.AND P0, PT, R2, R5, PT", 0xau, "unordered a == b (true on NaN)"),
    FSETP_ROW("FSETP_LEU_R2_R5", "FSETP.LEU.AND P0, PT, R2, R5, PT", 0xbu, "unordered a <= b (true on NaN)"),
    FSETP_ROW("FSETP_GTU_R2_R5", "FSETP.GTU.AND P0, PT, R2, R5, PT", 0xcu, "unordered a > b (true on NaN)"),
    FSETP_ROW("FSETP_NEU_R2_R5", "FSETP.NEU.AND P0, PT, R2, R5, PT", 0xdu, "unordered a != b (true on NaN)"),
    FSETP_ROW("FSETP_GEU_R2_R5", "FSETP.GEU.AND P0, PT, R2, R5, PT", 0xeu, "unordered a >= b (true on NaN)"),
#undef FSETP_ROW
    { "F2I_FLOOR", "F2I.FLOOR.NTZ R9, R2", 0x7305, "FP32 -> S32, round toward -inf, NaN -> 0, saturating", ORACLE_E1,
      0x00097305, 0x00000002, 0x00207100, true },
    { "F2I_CEIL", "F2I.CEIL.NTZ R9, R2", 0x7305, "FP32 -> S32, round toward +inf, NaN -> 0, saturating", ORACLE_E1,
      0x00097305, 0x00000002, 0x0020b100, true },
    { "F2I_RNI", "F2I.NTZ R9, R2", 0x7305, "FP32 -> S32, round to nearest even (no rounding suffix = RN)", ORACLE_E1,
      0x00097305, 0x00000002, 0x00203100, true },
    { "F2U", "F2I.U32.TRUNC.NTZ R9, R2", 0x7305, "FP32 -> U32, truncate, NaN and negatives -> 0, saturating", ORACLE_E1,
      0x00097305, 0x00000002, 0x0020f000, true },
    { "I2FP_U32", "I2FP.F32.U32 R9, R2", 0x7245, "U32 -> FP32, round to nearest even (fixed latency)", ORACLE_E1,
      0x00097245, 0x00000002, 0x00201000, false },
    { "F2F_F16_F32", "F2F.F16.F32 R9, R2", 0x7304,
      "FP32 -> binary16 RNE in [15:0]; [31:16] expected zero (ptxas stores it unmasked), checked by the receipt", ORACLE_E1,
      0x00097304, 0x00000002, 0x00200800, true },
    { "F2F_BF16_F32", "F2F.BF16.F32 R9, R2", 0x7304,
      "FP32 -> bfloat16 RNE in [15:0]; [31:16] expected zero (ptxas stores it unmasked), checked by the receipt", ORACLE_E1,
      0x00097304, 0x00000002, 0x00202000, true },
    { "HADD2_F32_R2_H0", "HADD2.F32 R9, -RZ, R2.H0_H0", 0x7230, "binary16 [15:0] -> FP32 (-0 + h), ptxas form of cvt.f32.f16", ORACLE_E1,
      0xff097230, 0x20000002, 0x00004100, false },
    { "SHF_L_R9_R2_16", "SHF.L.U32 R9, R2, 0x10, RZ", 0x7819,
      "bfloat16 [15:0] -> FP32 by shift (ptxas form of cvt.f32.bf16); not a hardware conversion unit", ORACLE_E1,
      0x02097819, 0x00000010, 0x000006ff, false },
    { "LDC64_R10_C3A0", "LDC.64 R10, c[0x0][0x3a0]", 0x7b82, "c pointer from kernel argument words 8..9", ORACLE_E1,
      0xff0a7b82, 0x0000e800, 0x00000a00, true },
    { "IMAD_WIDE_R10_R9_4", "IMAD.WIDE.U32 R10, R9, 0x4, R10", 0x7825, "&c[i] = c + index * 4", ORACLE_E1,
      0x090a7825, 0x00000004, 0x078e000a, false },
    { "LDG_R11_R10", "LDG.E R11, desc[UR4][R10.64]", 0x7981, "32-bit global load of c[i]", ORACLE_E1,
      0x0a0b7981, 0x00000004, 0x0c1e1900, true },
    { "FFMA_R9_R2_R5_R11", "FFMA R9, R2, R5, R11", 0x7223, "FP32 fused multiply-add, c[i] per element", ORACLE_E1,
      0x02097223, 0x00000005, 0x0000000b, false },
};

#define PROVENANCE_COUNT (sizeof(PROVENANCE_TABLE) / sizeof(PROVENANCE_TABLE[0]))

size_t omega_numeric_get_opcode_count(void) { return PROVENANCE_COUNT; }

const OmegaOpcodeProvenance *omega_numeric_get_opcode(size_t index) {
    return index < PROVENANCE_COUNT ? &PROVENANCE_TABLE[index] : NULL;
}

static unsigned ctrl_wbar(uint32_t w3) { return (w3 >> 14) & 7u; }
static unsigned ctrl_wait(uint32_t w3) { return (w3 >> 20) & 0x3fu; }

#define LOAD_BARRIER_MASK (1u << 4) /* both LDG.E of the vecadd baseline set SB4 */

int omega_numeric_verify_fixture_table(const OmegaOpcodeProvenance *table, size_t n, bool verbose) {
    int problems = 0;
#define PROBLEM(...) do { problems++; if (verbose) { fprintf(stderr, "provenance: " __VA_ARGS__); fputc('\n', stderr); } } while (0)
    if (!table || n == 0 || n > 64) {
        PROBLEM("empty or oversized table");
        return problems;
    }
    bool used[64] = { false };

    for (size_t i = 0; i < n; i++) {
        const OmegaOpcodeProvenance *p = &table[i];
        if (!p->key || !p->mnemonic || !p->description || !p->evidence_source || p->opcode == 0) {
            PROBLEM("entry %zu incomplete", i);
            continue;
        }
        if (p->opcode != (p->fixture_w0 & 0xffffu)) PROBLEM("%s: opcode 0x%04x != w0 low half", p->key, p->opcode);
        for (size_t j = 0; j < i; j++) {
            if (table[j].key && strcmp(table[j].key, p->key) == 0) PROBLEM("%s: duplicate key", p->key);
        }
    }

    uint8_t base[0x200];
    size_t base_len = 0;
    if (omega_blackwell_encode_vecadd(base, sizeof(base), &base_len) != 0 || base_len < 0x140) {
        PROBLEM("vecadd baseline could not be encoded");
        return problems;
    }
    uint32_t stg[4], ex[4];
    memcpy(stg, base + 0x120, 16);
    memcpy(ex, base + 0x130, 16);

    for (size_t k = 0; k < omega_numeric_op_count(); k++) {
        const OmegaNumericOpInfo *info = omega_numeric_op_at(k);
        OmegaNumericPatchInsn patch[OMEGA_NUMERIC_PATCH_MAX];
        int m = omega_numeric_patch_words(info->op, patch);
        if (omega_numeric_op_whole_kernel(info->op)) {
            /* DIV/SQRT: whole-program kernels (src/omega_numeric_divsqrt_gb10.c,
             * own structural check + nvdisasm digest); no vecadd patch. */
            if (m != OMEGA_NUMERIC_ERR_NOT_ENCODED) PROBLEM("%s: whole-program op has patch words", info->name);
            continue;
        }
        if (!info->gb10_encoded) {
            if (m != OMEGA_NUMERIC_ERR_NOT_ENCODED) PROBLEM("%s: not encoded but has patch words", info->name);
            continue;
        }
        if (m <= 0) {
            PROBLEM("%s: encoded but no patch words", info->name);
            continue;
        }
        if (!(ctrl_wait(patch[0].w[3]) & LOAD_BARRIER_MASK))
            PROBLEM("%s: first instruction does not wait on the load barrier SB4", info->name);
        int pending_bar = -1;
        for (int t = 0; t < m; t++) {
            const OmegaNumericPatchInsn *w = &patch[t];
            if (pending_bar >= 0) {
                if (!(ctrl_wait(w->w[3]) & (1u << pending_bar)))
                    PROBLEM("%s: instruction %d does not wait on SB%d set by the variable-latency op before it",
                            info->name, t, pending_bar);
                pending_bar = -1;
            }
            if (!w->provenance_key) {
                bool is_stg = w->w[0] == stg[0] && w->w[1] == stg[1] && w->w[2] == stg[2];
                bool is_exit = w->w[0] == ex[0] && w->w[1] == ex[1] && w->w[2] == ex[2];
                if (!is_stg && !is_exit) PROBLEM("%s: unkeyed word %d is neither baseline STG nor EXIT", info->name, t);
                continue;
            }
            const OmegaOpcodeProvenance *p = NULL;
            for (size_t i = 0; i < n; i++) {
                if (table[i].key && strcmp(table[i].key, w->provenance_key) == 0) { p = &table[i]; used[i] = true; }
            }
            if (!p) {
                PROBLEM("%s: no provenance entry for %s", info->name, w->provenance_key);
                continue;
            }
            if (p->fixture_w0 != w->w[0] || p->fixture_w1 != w->w[1] || p->fixture_w2 != w->w[2])
                PROBLEM("%s: executor words for %s differ from the provenance fixture", info->name, p->key);
            if (p->variable_latency) {
                unsigned bar = ctrl_wbar(w->w[3]);
                if (bar > 5) PROBLEM("%s: variable-latency %s sets no write barrier", info->name, p->key);
                else pending_bar = (int)bar;
            }
        }
        if (pending_bar >= 0) {
            /* Single-instruction patch: the baseline STG at 0x120 follows. */
            if (m == 1 && !(ctrl_wait(stg[3]) & (1u << pending_bar)))
                PROBLEM("%s: baseline STG does not wait on SB%d", info->name, pending_bar);
            else if (m != 1)
                PROBLEM("%s: variable-latency result never waited on", info->name);
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (!used[i]) PROBLEM("%s: entry not submitted by any encoded op", table[i].key ? table[i].key : "?");
    }
#undef PROBLEM
    return problems;
}

int omega_numeric_verify_all_fixtures(void) {
    return omega_numeric_verify_fixture_table(PROVENANCE_TABLE, PROVENANCE_COUNT, true);
}
