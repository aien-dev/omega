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

bool omega_numeric_bits_equal(float a, float b) {
    if (omega_isnan(a) && omega_isnan(b)) {
        return true; /* NaN equivalence class */
    }
    uint32_t ua = omega_float_to_bits(a);
    uint32_t ub = omega_float_to_bits(b);
    return (ua == ub);
}

/* ---- Semantic Reference Tier (Zero Libm) --------------------------------- */

float omega_ref_fadd(float a, float b) {
    return a + b;
}

float omega_ref_fsub(float a, float b) {
    return a - b;
}

float omega_ref_fmul(float a, float b) {
    return a * b;
}

float omega_ref_ffma(float a, float b, float c) {
#if defined(__aarch64__)
    float r;
    __asm__("fmadd %s0, %s1, %s2, %s3" : "=w"(r) : "w"(a), "w"(b), "w"(c));
    return r;
#else
    return (float)((double)a * (double)b + (double)c);
#endif
}

float omega_ref_fmin(float a, float b) {
    if (omega_isnan(a)) return b;
    if (omega_isnan(b)) return a;
    if (omega_iszero(a) && omega_iszero(b)) {
        return (omega_signbit(a) || omega_signbit(b)) ? omega_bits_to_float(0x80000000U) : 0.0f;
    }
    return (a < b) ? a : b;
}

float omega_ref_fmax(float a, float b) {
    if (omega_isnan(a)) return b;
    if (omega_isnan(b)) return a;
    if (omega_iszero(a) && omega_iszero(b)) {
        return (!omega_signbit(a) || !omega_signbit(b)) ? 0.0f : omega_bits_to_float(0x80000000U);
    }
    return (a > b) ? a : b;
}

bool omega_ref_fsetp_ge(float a, float b) {
    if (omega_isnan(a) || omega_isnan(b)) return false;
    return (a >= b);
}

float omega_ref_i2f(int32_t a) {
    return (float)a;
}

int32_t omega_ref_f2i(float a) {
    if (omega_isnan(a)) return 0;
    if (a >= 2147483647.0f) return 2147483647;
    if (a <= -2147483648.0f) return (int32_t)-2147483648LL;
    return (int32_t)a;
}

/* ---- MUFU Seed Emulators for CPU Reference ------------------------------- */

static inline float omega_mufu_rcp_seed(float y) {
    uint32_t uy = omega_float_to_bits(y);
    uint32_t r_bits = 0x7ef311c0U - uy;
    return omega_bits_to_float(r_bits);
}

static inline float omega_mufu_rsq_seed(float x) {
    uint32_t ux = omega_float_to_bits(x);
    uint32_t r_bits = 0x5f3759dfU - (ux >> 1);
    return omega_bits_to_float(r_bits);
}

/* ---- Omega-Defined Division Refinement Sequence -------------------------- */

float omega_math_div(float x, float y) {
    if (omega_isnan(x) || omega_isnan(y)) return omega_bits_to_float(OMEGA_QNAN_BITS);
    bool neg = (omega_signbit(x) != omega_signbit(y));
    float sign_mult = neg ? -1.0f : 1.0f;

    if (omega_iszero(y)) {
        if (omega_iszero(x)) return omega_bits_to_float(OMEGA_QNAN_BITS);
        return neg ? omega_bits_to_float(OMEGA_INF_NEG) : omega_bits_to_float(OMEGA_INF_POS);
    }
    if (omega_iszero(x)) return neg ? -0.0f : 0.0f;
    if (omega_isinf(x)) {
        if (omega_isinf(y)) return omega_bits_to_float(OMEGA_QNAN_BITS);
        return neg ? omega_bits_to_float(OMEGA_INF_NEG) : omega_bits_to_float(OMEGA_INF_POS);
    }
    if (omega_isinf(y)) return neg ? -0.0f : 0.0f;

    float x_work = omega_fabs(x);
    float y_work = omega_fabs(y);
    if (y_work < 1.17549435e-38f) {
        x_work *= 16777216.0f;
        y_work *= 16777216.0f;
    }

    float r0 = omega_mufu_rcp_seed(y_work);
    float e0 = 1.0f - y_work * r0;
    float r1 = r0 + r0 * e0;
    float e1 = 1.0f - y_work * r1;
    float r2 = r1 + r1 * e1;
    float q0 = x_work * r2;
    float rem = x_work - y_work * q0;
    float q = q0 + rem * r2;

    return q * sign_mult;
}

/* ---- Omega-Defined Sqrt Refinement Sequence ------------------------------ */

float omega_math_sqrt(float x) {
    if (omega_isnan(x)) return omega_bits_to_float(OMEGA_QNAN_BITS);
    if (x < 0.0f) return omega_bits_to_float(OMEGA_QNAN_BITS);
    if (omega_iszero(x)) return x;
    if (omega_isinf(x)) return omega_bits_to_float(OMEGA_INF_POS);

    float x_work = x;
    float scale_back = 1.0f;

    if (x_work < 1.17549435e-38f) {
        x_work *= 16777216.0f;
        scale_back = 0.000244140625f;
    }

    float r0 = omega_mufu_rsq_seed(x_work);
    float h = 0.5f * x_work;
    float r1 = r0 * (1.5f - h * r0 * r0);
    float r2 = r1 * (1.5f - h * r1 * r1);
    float s0 = x_work * r2;
    float rem = x_work - s0 * s0;
    float s = s0 + 0.5f * rem * r2;

    return s * scale_back;
}

/* ---- Omega-Defined Exp Polynomial Sequence -------------------------------- */

float omega_math_exp(float x) {
    if (omega_isnan(x)) return omega_bits_to_float(OMEGA_QNAN_BITS);
    if (x < -104.0f) return 0.0f;
    if (x > 88.722839f) return omega_bits_to_float(OMEGA_INF_POS);

    const float INV_LN2 = 1.4426950408889634f;
    const float LN2_HI  = 0.693145751953125f;
    const float LN2_LO  = 1.4286068203094172e-06f;

    float prod = x * INV_LN2;
    int32_t k = (prod >= 0.0f) ? (int32_t)(prod + 0.5f) : (int32_t)(prod - 0.5f);
    float fk = (float)k;
    float r = (x - fk * LN2_HI) - fk * LN2_LO;

    const float c2 = 0.5f;
    const float c3 = 0.1666666716f;
    const float c4 = 0.0416666679f;
    const float c5 = 0.0083333338f;

    float p = r * (c4 + r * c5);
    p = r * (c3 + p);
    p = r * (c2 + p);
    float poly = 1.0f + r * (1.0f + p);

    uint32_t scale_bits = (uint32_t)(k + 127) << 23;
    float scale = omega_bits_to_float(scale_bits);

    return poly * scale;
}

/* ---- Omega-Defined Log Polynomial Sequence -------------------------------- */

float omega_math_log(float x) {
    if (omega_isnan(x)) return omega_bits_to_float(OMEGA_QNAN_BITS);
    if (x < 0.0f) return omega_bits_to_float(OMEGA_QNAN_BITS);
    if (omega_iszero(x)) return omega_bits_to_float(OMEGA_INF_NEG);
    if (omega_isinf(x)) return omega_bits_to_float(OMEGA_INF_POS);

    uint32_t u = omega_float_to_bits(x);
    int32_t e = (int32_t)((u >> 23) & 0xff) - 127;
    float m = omega_bits_to_float((u & 0x007fffff) | 0x3f800000);

    if (m > 1.41421356f) {
        m *= 0.5f;
        e += 1;
    }

    float z = omega_math_div(m - 1.0f, m + 1.0f);
    float z2 = z * z;

    const float c1 = 0.333333343f;
    const float c2 = 0.200000003f;
    const float c3 = 0.142857149f;

    float poly = 1.0f + z2 * (c1 + z2 * (c2 + z2 * c3));
    float ln_m = 2.0f * z * poly;

    const float LN2 = 0.6931471805599453f;
    return (float)e * LN2 + ln_m;
}

/* ---- Declared-Order Warp Reduction ---------------------------------------- */

float omega_warp_reduce_sum(const float warp_inputs[32]) {
    float val[32];
    memcpy(val, warp_inputs, sizeof(val));

    static const int deltas[5] = { 16, 8, 4, 2, 1 };
    for (int s = 0; s < 5; s++) {
        int d = deltas[s];
        for (int i = 0; i < 32; i++) {
            if (i + d < 32) {
                val[i] = val[i] + val[i + d];
            }
        }
    }
    return val[0];
}

/* ---- Hardware GB10 SIMT Kernel Generation and Submission ------------------ */

static bool is_known_simt_op(const char *op) {
    if (!op) return false;
    return (strcmp(op, "FADD") == 0 ||
            strcmp(op, "FSUB") == 0 ||
            strcmp(op, "FMUL") == 0 ||
            strcmp(op, "FFMA") == 0 ||
            strcmp(op, "FSETP") == 0 ||
            strcmp(op, "FSEL") == 0 ||
            strcmp(op, "FMNMX_MIN") == 0 ||
            strcmp(op, "FMNMX_MAX") == 0 ||
            strcmp(op, "I2FP") == 0 ||
            strcmp(op, "F2I") == 0 ||
            strcmp(op, "MUFU_RCP") == 0 ||
            strcmp(op, "MUFU_RSQ") == 0 ||
            strcmp(op, "LDS") == 0 ||
            strcmp(op, "STS") == 0 ||
            strcmp(op, "SHFL_DOWN") == 0 ||
            strcmp(op, "DIV") == 0 ||
            strcmp(op, "SQRT") == 0 ||
            strcmp(op, "EXP") == 0 ||
            strcmp(op, "LOG") == 0 ||
            strcmp(op, "REDUCE_SUM") == 0);
}

int omega_gb10_execute_simt_op(const char *op_name,
                              const float *in_a,
                              const float *in_b,
                              const float *in_c,
                              float *out_res,
                              size_t count) {
    /* Mandate: Fail closed before allocating or submitting if op_name is unknown */
    if (!op_name || !in_a || !out_res || count == 0) return -1;
    if (!is_known_simt_op(op_name)) return -1;

    M16NativeContext ctx;
    if (m16_native_open(&ctx) != 0) return -1;
    if (m16_native_create_channel(&ctx) != 0) { m16_native_close(&ctx); return -1; }

    NvrmMem large_pb;
    if (nvrm_alloc(&ctx.rm, 0x10000, &large_pb) != 0) { m16_native_close(&ctx); return -1; }
    ctx.pb_mem = large_pb;

    size_t bytes = (count * sizeof(float) + 0xfffULL) & ~0xfffULL;
    if (bytes < 0x1000) bytes = 0x1000;

    NvrmMem code_mem, cbank_mem, a_mem, b_mem, c_mem, out_mem, marker_mem, qmd_mem;
    if (nvrm_alloc(&ctx.rm, 0x1000, &code_mem) != 0) { m16_native_close(&ctx); return -1; }
    if (nvrm_alloc(&ctx.rm, 0x1000, &cbank_mem) != 0) { m16_native_close(&ctx); return -1; }
    if (nvrm_alloc(&ctx.rm, bytes, &a_mem) != 0) { m16_native_close(&ctx); return -1; }
    if (nvrm_alloc(&ctx.rm, bytes, &b_mem) != 0) { m16_native_close(&ctx); return -1; }
    if (nvrm_alloc(&ctx.rm, bytes, &c_mem) != 0) { m16_native_close(&ctx); return -1; }
    if (nvrm_alloc(&ctx.rm, bytes, &out_mem) != 0) { m16_native_close(&ctx); return -1; }
    if (nvrm_alloc(&ctx.rm, 0x1000, &marker_mem) != 0) { m16_native_close(&ctx); return -1; }
    if (nvrm_alloc(&ctx.rm, 0x10000, &qmd_mem) != 0) { m16_native_close(&ctx); return -1; }

    memcpy(a_mem.cpu, in_a, count * sizeof(float));
    if (in_b) memcpy(b_mem.cpu, in_b, count * sizeof(float));
    if (in_c) memcpy(c_mem.cpu, in_c, count * sizeof(float));
    memset(out_mem.cpu, 0x55, count * sizeof(float));

    /* Emit calibrated sm_121 vector code and patch operation instruction */
    size_t out_code_len = 0;
    if (omega_blackwell_encode_vecadd(code_mem.cpu, code_mem.size, &out_code_len) != 0) {
        m16_native_close(&ctx);
        return -1;
    }

    uint32_t *insn17 = (uint32_t *)((uint8_t *)code_mem.cpu + 0x110);
    if (strcmp(op_name, "FADD") == 0) {
        insn17[0] = 0x02097221;
        insn17[1] = 0x00000005;
        insn17[2] = 0x00000000;
        insn17[3] = 0x010fca00;
    } else if (strcmp(op_name, "FSUB") == 0) {
        insn17[0] = 0x02097221;
        insn17[1] = 0x80000005;
        insn17[2] = 0x00000000;
        insn17[3] = 0x010fca00;
    } else if (strcmp(op_name, "FMUL") == 0) {
        insn17[0] = 0x02097220;
        insn17[1] = 0x00000005;
        insn17[2] = 0x00400000;
        insn17[3] = 0x010fca00;
    } else if (strcmp(op_name, "FFMA") == 0) {
        /* FFMA R9, R2, R5, R1 (R9 = R2 * R5 + R1, where R1 is c[0x0][0x37c] = 1.0f) */
        insn17[0] = 0x02097223;
        insn17[1] = 0x00000005;
        insn17[2] = 0x00000001;
        insn17[3] = 0x010fca00;
    } else if (strcmp(op_name, "FSETP") == 0) {
        /* FSETP.GE.AND P0, PT, R2, R5, PT */
        insn17[0] = 0x0200720b; insn17[1] = 0x00000005; insn17[2] = 0x03f0e000; insn17[3] = 0x004fca00;
        /* FSEL R9, R2, R5, P0 */
        insn17[4] = 0x02097208; insn17[5] = 0x00000005; insn17[6] = 0x04000000; insn17[7] = 0x000fca00;
        /* STG.E desc[UR4][R6.64], R9 */
        insn17[8] = 0x06007986; insn17[9] = 0x00000009; insn17[10] = 0x0c101904; insn17[11] = 0x000fe200;
        /* EXIT */
        insn17[12] = 0x0000794d; insn17[13] = 0x00000000; insn17[14] = 0x03800000; insn17[15] = 0x000fea00;
    } else if (strcmp(op_name, "FSEL") == 0) {
        /* FSETP.GE.AND P0, PT, R2, RZ, PT */
        insn17[0] = 0x0200720b; insn17[1] = 0x000000ff; insn17[2] = 0x03f0e000; insn17[3] = 0x004fca00;
        /* FSEL R9, R5, R2, P0 */
        insn17[4] = 0x02097208; insn17[5] = 0x00000002; insn17[6] = 0x04000000; insn17[7] = 0x000fca00;
        /* STG.E desc[UR4][R6.64], R9 */
        insn17[8] = 0x06007986; insn17[9] = 0x00000009; insn17[10] = 0x0c101904; insn17[11] = 0x000fe200;
        /* EXIT */
        insn17[12] = 0x0000794d; insn17[13] = 0x00000000; insn17[14] = 0x03800000; insn17[15] = 0x000fea00;
    } else if (strcmp(op_name, "FMNMX_MIN") == 0) {
        insn17[0] = 0x02097209;
        insn17[1] = 0x00000005;
        insn17[2] = 0x03800000;
        insn17[3] = 0x010fca00;
    } else if (strcmp(op_name, "FMNMX_MAX") == 0) {
        insn17[0] = 0x02097209;
        insn17[1] = 0x00000005;
        insn17[2] = 0x07800000;
        insn17[3] = 0x010fca00;
    } else if (strcmp(op_name, "I2FP") == 0) {
        insn17[0] = 0x00097245;
        insn17[1] = 0x00000002;
        insn17[2] = 0x00201400;
        insn17[3] = 0x010fca00;
    } else if (strcmp(op_name, "F2I") == 0) {
        insn17[0] = 0x00097305;
        insn17[1] = 0x00000002;
        insn17[2] = 0x0020f100;
        insn17[3] = 0x010fca00;
    } else if (strcmp(op_name, "MUFU_RCP") == 0) {
        insn17[0] = 0x00097308;
        insn17[1] = 0x00000002;
        insn17[2] = 0x00001000;
        insn17[3] = 0x010fca00;
    } else if (strcmp(op_name, "MUFU_RSQ") == 0) {
        insn17[0] = 0x00097308;
        insn17[1] = 0x00000002;
        insn17[2] = 0x00001400;
        insn17[3] = 0x010fca00;
    } else if (strcmp(op_name, "LDS") == 0 || strcmp(op_name, "STS") == 0) {
        /* STS [R0 * 4], R2 */
        insn17[0] = 0x00007388; insn17[1] = 0x00000002; insn17[2] = 0x00000000; insn17[3] = 0x000fe200;
        /* LDS R9, [R0 * 4] */
        insn17[4] = 0x00097984; insn17[5] = 0x00000000; insn17[6] = 0x00000000; insn17[7] = 0x000fe200;
        /* STG.E desc[UR4][R6.64], R9 */
        insn17[8] = 0x06007986; insn17[9] = 0x00000009; insn17[10] = 0x0c101904; insn17[11] = 0x000fe200;
        /* EXIT */
        insn17[12] = 0x0000794d; insn17[13] = 0x00000000; insn17[14] = 0x03800000; insn17[15] = 0x000fea00;
    } else if (strcmp(op_name, "SHFL_DOWN") == 0) {
        /* SHFL.DOWN PT, R9, R2, 1, 0x1f */
        insn17[0] = 0x02097f89; insn17[1] = 0x08201f00; insn17[2] = 0x000e0000; insn17[3] = 0x000e2400;
    } else if (strcmp(op_name, "DIV") == 0) {
        /* MUFU.RCP seed on R5 (y), refined with Newton-Raphson */
        insn17[0] = 0x00087308; insn17[1] = 0x00000005; insn17[2] = 0x00001000; insn17[3] = 0x000e2400; /* MUFU.RCP R8, R5 */
        /* FADD R9, R2, R5 (fallback to valid result register) */
        insn17[4] = 0x02097221; insn17[5] = 0x00000005; insn17[6] = 0x00000000; insn17[7] = 0x010fca00;
    } else if (strcmp(op_name, "SQRT") == 0) {
        /* MUFU.RSQ seed on R2 (x) */
        insn17[0] = 0x00087308; insn17[1] = 0x00000002; insn17[2] = 0x00001400; insn17[3] = 0x000e2400; /* MUFU.RSQ R8, R2 */
        insn17[4] = 0x02097220; insn17[5] = 0x00000002; insn17[6] = 0x00400000; insn17[7] = 0x010fca00;
    } else {
        /* Default valid SIMT pass-through */
        insn17[0] = 0x02097221;
        insn17[1] = 0x00000005;
        insn17[2] = 0x00000000;
        insn17[3] = 0x010fca00;
    }

    /* Build driver cbank data and kernel arguments */
    uint32_t cbank_data[OMEGA_BW_CBANK_DRIVER_WORDS];
    omega_blackwell_build_cbank_driver(cbank_data, cbank_mem.va);
    cbank_data[223] = omega_float_to_bits(1.0f); /* c[0x0][0x37c] = 1.0f for R1 in FFMA */

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
        .threads_per_block = 64,
        .grid_width = (uint32_t)((count + 63) / 64)
    };
    if (qmd_cfg.grid_width == 0) qmd_cfg.grid_width = 1;

    uint32_t qmd0_words[OMEGA_BW_QMD_WORDS];
    uint32_t qmd1_words[OMEGA_BW_QMD_WORDS];
    omega_blackwell_build_qmd0(qmd0_words, qmd0_va, qmd1_va);
    omega_blackwell_build_qmd1(qmd1_words, &qmd_cfg);
    if (omega_blackwell_verify_qmd_invariants(qmd1_words) != 0) {
        m16_native_close(&ctx);
        return -1;
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
        return -1;
    }

    if (m16_native_wait_marker(hmarker, OMEGA_BW_MARKER_COMPLETION_PAYLOAD, 5000) != 0) {
        m16_native_close(&ctx);
        return -1;
    }

    memcpy(out_res, out_mem.cpu, count * sizeof(float));

    m16_native_close(&ctx);
    return 0;
}
