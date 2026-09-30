/*
 * OMEGA-NUMERIC-0 GB10 executor. Every request passes
 * omega_numeric_submit_check before any device is opened: unknown ops,
 * ops without a GB10 encoding (DIV, SQRT, EXP, LOG and the refused variants
 * such as LDS.U8 or SHFL_UP), operand shapes the kernel cannot carry, and a
 * patch that fails the structural check (shared-memory order and bounds,
 * barriers, reduction order) are refused here with a message on stderr. The
 * structural check runs again on the exact QMD that is submitted. The kernel is the calibrated vecadd with the op's patch words from
 * omega_numeric_patch_words; there is no fallback instruction.
 */
#include "omega_numeric.h"
#include "omega_blackwell_codegen.h"
#include "omega_blackwell_qmd.h"
#include "omega_blackwell_submit.h"
#include "m16_native.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OMEGA_BW_SETUP_WORDS_COUNT 18
static const uint32_t NUMERIC_SETUP_WORDS[OMEGA_BW_SETUP_WORDS_COUNT] = {
    0x20012061, 0x0000cec0, 0x20012092, 0x00000001, 0x200120a8, 0x0000000f, 0x2001255d, 0x00000003,
    0x2001255e, 0x20000000, 0x2001255f, 0x000fffff, 0x20012557, 0x00000003, 0x20012558, 0x22000000,
    0x20012559, 0x00000000,
};

int omega_gb10_execute_simt_op(const char *op_name,
                              const float *in_a,
                              const float *in_b,
                              const float *in_c,
                              float *out_res,
                              size_t count) {
    char err[256];
    int check = omega_numeric_submit_check(op_name, in_a, in_b, in_c, out_res, count, err, sizeof(err));
    if (check != OMEGA_NUMERIC_OK) {
        fprintf(stderr, "omega_gb10_execute_simt_op: refused before submission: %s\n", err);
        return check;
    }
    const OmegaNumericOpInfo *info = omega_numeric_op_find(op_name);

    M16NativeContext ctx;
    if (m16_native_open(&ctx) != 0) return OMEGA_NUMERIC_ERR_DEVICE;
    if (m16_native_create_channel(&ctx) != 0) { m16_native_close(&ctx); return OMEGA_NUMERIC_ERR_DEVICE; }

    NvrmMem large_pb;
    if (nvrm_alloc(&ctx.rm, 0x10000, &large_pb) != 0) { m16_native_close(&ctx); return OMEGA_NUMERIC_ERR_DEVICE; }
    ctx.pb_mem = large_pb;

    size_t bytes = (count * sizeof(float) + 0xfffULL) & ~0xfffULL;
    if (bytes < 0x1000) bytes = 0x1000;

    NvrmMem code_mem, cbank_mem, a_mem, b_mem, c_mem, out_mem, marker_mem, qmd_mem;
    if (nvrm_alloc(&ctx.rm, 0x1000, &code_mem) != 0 ||
        nvrm_alloc(&ctx.rm, 0x1000, &cbank_mem) != 0 ||
        nvrm_alloc(&ctx.rm, bytes, &a_mem) != 0 ||
        nvrm_alloc(&ctx.rm, bytes, &b_mem) != 0 ||
        nvrm_alloc(&ctx.rm, bytes, &c_mem) != 0 ||
        nvrm_alloc(&ctx.rm, bytes, &out_mem) != 0 ||
        nvrm_alloc(&ctx.rm, 0x1000, &marker_mem) != 0 ||
        nvrm_alloc(&ctx.rm, 0x10000, &qmd_mem) != 0) {
        m16_native_close(&ctx);
        return OMEGA_NUMERIC_ERR_DEVICE;
    }

    memcpy(a_mem.cpu, in_a, count * sizeof(float));
    if (in_b) memcpy(b_mem.cpu, in_b, count * sizeof(float));
    else memset(b_mem.cpu, 0, count * sizeof(float));
    if (in_c) memcpy(c_mem.cpu, in_c, count * sizeof(float));
    memset(out_mem.cpu, 0x55, count * sizeof(float));

    /* Calibrated sm_121 vecadd with this op's patch; no default instruction. */
    size_t out_code_len = 0;
    if (omega_numeric_build_kernel(info->op, code_mem.cpu, code_mem.size, &out_code_len) != OMEGA_NUMERIC_OK) {
        m16_native_close(&ctx);
        return OMEGA_NUMERIC_ERR_DEVICE;
    }

    /* Driver constant bank. Word 223 (c[0x0][0x37c]) is loaded into R1 at
     * kernel entry; FFMA reads its c operand from R1, so c travels here. */
    uint32_t cbank_data[OMEGA_BW_CBANK_DRIVER_WORDS];
    omega_blackwell_build_cbank_driver(cbank_data, cbank_mem.va);
    cbank_data[223] = omega_float_to_bits((info->op == OMEGA_NOP_FFMA) ? in_c[0] : 1.0f);

    uint32_t cbank_args[10];
    memset(cbank_args, 0, sizeof(cbank_args));
    cbank_args[0] = (uint32_t)a_mem.va;
    cbank_args[1] = (uint32_t)(a_mem.va >> 32);
    cbank_args[2] = (uint32_t)b_mem.va;
    cbank_args[3] = (uint32_t)(b_mem.va >> 32);
    cbank_args[4] = (uint32_t)out_mem.va;
    cbank_args[5] = (uint32_t)(out_mem.va >> 32);
    cbank_args[6] = (uint32_t)count;
    cbank_args[7] = 0;
    cbank_args[8] = (uint32_t)c_mem.va;
    cbank_args[9] = (uint32_t)(c_mem.va >> 32);

    memcpy(cbank_mem.cpu, cbank_data, sizeof(cbank_data));
    memcpy((uint8_t *)cbank_mem.cpu + 0x380, cbank_args, sizeof(cbank_args));

    uint64_t qmd0_va = qmd_mem.va;
    uint64_t qmd1_va = qmd_mem.va + 0x1000;
    uint64_t sem_va  = qmd_mem.va + 0x2000;
    uint64_t scratch_va = qmd_mem.va + 0x4000;

    OmegaBlackwellQmdConfig qmd_cfg = {
        .code_va = code_mem.va,
        .cbank_va = cbank_mem.va,
        .scratch_va = scratch_va,
        .sem_va = sem_va,
        .qmd0_va = qmd0_va,
        .qmd1_va = qmd1_va,
        .num_elements = (uint32_t)count,
    };
    omega_numeric_launch_shape(count, &qmd_cfg.threads_per_block, &qmd_cfg.grid_width);

    uint32_t qmd0_words[OMEGA_BW_QMD_WORDS];
    uint32_t qmd1_words[OMEGA_BW_QMD_WORDS];
    if (omega_blackwell_build_qmd0(qmd0_words, qmd0_va, qmd1_va) != 0 ||
        omega_blackwell_build_qmd1(qmd1_words, &qmd_cfg) != 0 ||
        omega_blackwell_verify_qmd_invariants(qmd1_words) != 0) {
        m16_native_close(&ctx);
        return OMEGA_NUMERIC_ERR_DEVICE;
    }
    {
        /* same structural check as submit_check, on the QMD actually submitted */
        OmegaNumericPatchInsn patch[OMEGA_NUMERIC_PATCH_MAX];
        int np = omega_numeric_patch_words(info->op, patch);
        char perr[256];
        if (omega_numeric_check_patch(info->op, patch, np, qmd1_words, perr, sizeof(perr)) != OMEGA_NUMERIC_OK) {
            fprintf(stderr, "omega_gb10_execute_simt_op: %s\n", perr);
            m16_native_close(&ctx);
            return OMEGA_NUMERIC_ERR_OPERANDS;
        }
    }

    memcpy(qmd_mem.cpu, qmd0_words, sizeof(qmd0_words));
    memcpy((uint8_t *)qmd_mem.cpu + 0x1000, qmd1_words, sizeof(qmd1_words));

    volatile uint32_t *hsem = (volatile uint32_t *)((uint8_t *)qmd_mem.cpu + 0x2000);
    volatile uint32_t *hmarker = (volatile uint32_t *)marker_mem.cpu;
    *hsem = 0;
    *hmarker = 0;
    __asm__ volatile("dsb sy" ::: "memory");

    uint32_t pb[1024];
    size_t pb_len = 0;

    memcpy(&pb[pb_len], NUMERIC_SETUP_WORDS, sizeof(NUMERIC_SETUP_WORDS));
    pb_len += sizeof(NUMERIC_SETUP_WORDS) / 4;

    pb[pb_len++] = nvrm_mthd(1, 0x0188, 2);
    pb[pb_len++] = (uint32_t)(cbank_mem.va >> 32);
    pb[pb_len++] = (uint32_t)cbank_mem.va;
    pb[pb_len++] = nvrm_mthd(1, 0x0180, 2);
    pb[pb_len++] = 0x00000380;
    pb[pb_len++] = 0x00000001;
    pb[pb_len++] = nvrm_mthd(1, 0x01b0, 1);
    pb[pb_len++] = 0x00000041;
    pb[pb_len++] = (224 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    memcpy(&pb[pb_len], cbank_data, 224 * 4);
    pb_len += 224;

    pb[pb_len++] = nvrm_mthd(1, 0x0188, 2);
    pb[pb_len++] = (uint32_t)((cbank_mem.va + 0x380) >> 32);
    pb[pb_len++] = (uint32_t)(cbank_mem.va + 0x380);
    pb[pb_len++] = nvrm_mthd(1, 0x0180, 2);
    pb[pb_len++] = 0x00000028; /* 10 words = 40 bytes */
    pb[pb_len++] = 0x00000001;
    pb[pb_len++] = nvrm_mthd(1, 0x01b0, 1);
    pb[pb_len++] = 0x00000041;
    pb[pb_len++] = (10 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    memcpy(&pb[pb_len], cbank_args, 10 * 4);
    pb_len += 10;

    pb[pb_len++] = (98 << 16) | (1 << 13) | (0x0318 >> 2) | (2u << 28);
    pb[pb_len++] = (1u << 30) | (uint32_t)((qmd0_va >> 40) & 0x1ff);
    pb[pb_len++] = (uint32_t)(qmd0_va >> 8);
    memcpy(&pb[pb_len], qmd0_words, 96 * 4);
    pb_len += 96;

    pb[pb_len++] = nvrm_mthd(1, 0x0188, 2);
    pb[pb_len++] = (uint32_t)(sem_va >> 32);
    pb[pb_len++] = (uint32_t)sem_va;
    pb[pb_len++] = nvrm_mthd(1, 0x0180, 2);
    pb[pb_len++] = 0x00000004;
    pb[pb_len++] = 0x00000001;
    pb[pb_len++] = nvrm_mthd(1, 0x01b0, 1);
    pb[pb_len++] = 0x00000041;
    pb[pb_len++] = (1 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    pb[pb_len++] = OMEGA_BW_SEMAPHORE_INTERMEDIATE_INIT;

    pb[pb_len++] = (98 << 16) | (1 << 13) | (0x0318 >> 2) | (2u << 28);
    pb[pb_len++] = (1u << 30) | (uint32_t)((qmd1_va >> 40) & 0x1ff);
    pb[pb_len++] = (uint32_t)(qmd1_va >> 8);
    memcpy(&pb[pb_len], qmd1_words, 96 * 4);
    pb_len += 96;

    pb[pb_len++] = nvrm_mthd(0, 0x005c, 5);
    pb[pb_len++] = (uint32_t)marker_mem.va;
    pb[pb_len++] = (uint32_t)(marker_mem.va >> 32);
    pb[pb_len++] = OMEGA_BW_MARKER_COMPLETION_PAYLOAD;
    pb[pb_len++] = 0;
    pb[pb_len++] = 0x1 | (1u << 20);

    if (m16_native_submit_methods(&ctx, pb, pb_len) != 0) {
        m16_native_close(&ctx);
        return OMEGA_NUMERIC_ERR_DEVICE;
    }

    if (m16_native_wait_marker(hmarker, OMEGA_BW_MARKER_COMPLETION_PAYLOAD, 5000) != 0) {
        m16_native_close(&ctx);
        return OMEGA_NUMERIC_ERR_DEVICE;
    }

    memcpy(out_res, out_mem.cpu, count * sizeof(float));

    m16_native_close(&ctx);
    return OMEGA_NUMERIC_OK;
}
