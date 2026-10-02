/*
 * E1 row 7: correctly rounded FP32 DIV and SQRT on the GB10.
 *
 * Modes (one per run):
 *   (none)          host tier: encoder round trips, structural refusals,
 *                   host model of the body vs omega_math_* and the AArch64
 *                   FDIV/FSQRT on the hard corpus, edge classes and random
 *                   samples. Exit 0 only when every [PASS] holds. No device.
 *   --dump DIR      writes DIR/{div,sqrt,exp2,log2}.bin and .lst (the
 *                   expected nvdisasm text) for tools/divsqrt_nvdisasm_check.sh
 *   --digest        prints the SHA-256 of each emitted kernel
 *   --host-sqrt-all host model of SQRT on all 2^32 inputs
 *   --host-div N    host model of DIV on N random pairs plus the edge set
 *   --chip          GB10: SQRT on all 2^32 inputs and DIV on 6 * 2^24 random
 *                   pairs plus the corpus and the edge set. Prints one
 *                   RESULT line per op and a VERDICT line.
 * Output lines: [PASS] / [FAIL] / [SKIP] ID, RESULT ..., VERDICT ...
 */
#include "omega_numeric.h"
#include "omega_numeric_divsqrt_gb10.h"
#include "omega_blackwell_encoder.h"
#include "omega_blackwell_qmd.h"

#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int g_pass, g_fail;
static void check(int ok, const char *id) {
    if (ok) { g_pass++; printf("[PASS] %s\n", id); }
    else { g_fail++; printf("[FAIL] %s\n", id); }
}

/* Reference: the AArch64 instruction, NaN results canonical. */
static uint32_t want_bits(OmegaDsOp op, uint32_t a, uint32_t b) {
    float r = op == OMEGA_DS_DIV ? omega_ieee_div(omega_bits_to_float(a), omega_bits_to_float(b))
                                 : omega_ieee_sqrt(omega_bits_to_float(a));
    return isnan(r) ? 0x7fc00000u : omega_float_to_bits(r);
}

static const uint32_t DIV_HARD[][2] = {
    { 0x3f800000u, 0x40400000u }, { 0x7f7fffffu, 0x00000001u }, { 0xff7fffffu, 0x00000001u },
    { 0x00000001u, 0x7f7fffffu }, { 0x00800000u, 0x40000000u }, { 0x00000003u, 0x40000000u },
    { 0x00000001u, 0x40000000u }, { 0x00000001u, 0x3f7fffffu }, { 0x007fffffu, 0x3f800001u },
    { 0x7f7fffffu, 0x3f7fffffu }, { 0x3f7fffffu, 0x7f7fffffu }, { 0x00400000u, 0x00000003u },
};
#define N_DIV_HARD (sizeof(DIV_HARD) / sizeof(DIV_HARD[0]))
static const uint32_t SQRT_HARD[] = {
    0x00800000u, 0x00000001u, 0x00000002u, 0x007fffffu, 0x7f7fffffu, 0x3f7fffffu, 0x40000000u,
};

/* Edge magnitudes; each is used with both signs. */
static const uint32_t EDGE_MAG[] = {
    0x00000000u, 0x00000001u, 0x00000002u, 0x00000003u, 0x00400000u, 0x007ffffeu, 0x007fffffu,
    0x00800000u, 0x00800001u, 0x00ffffffu, 0x01000000u, 0x0c800000u, 0x33800000u, 0x34000000u,
    0x3f000000u, 0x3f7fffffu, 0x3f800000u, 0x3f800001u, 0x3fc00000u, 0x40000000u, 0x40400000u,
    0x40490fdbu, 0x4b000000u, 0x4b800000u, 0x7e800000u, 0x7effffffu, 0x7f000000u, 0x7f7ffffeu,
    0x7f7fffffu, 0x7f800000u, 0x7f800001u, 0x7fbfffffu, 0x7fc00000u, 0x7fc00001u, 0x7fffffffu,
};
#define N_EDGE_MAG (sizeof(EDGE_MAG) / sizeof(EDGE_MAG[0]))

static uint64_t g_rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return g_rng;
}

/* Random pair, stratified over five shapes so that rare classes appear:
 * uniform bits; both operands near 1; quotients near the subnormal range;
 * quotients near overflow; subnormal operands. */
static void rnd_pair(uint32_t *a, uint32_t *b) {
    uint64_t r = rnd();
    uint32_t x = (uint32_t)r, y = (uint32_t)(r >> 32);
    uint32_t s = (uint32_t)rnd();
    switch (s % 5) {
    case 0: break;
    case 1: x = (x & 0x807fffffu) | ((uint32_t)(120 + (s >> 3) % 16) << 23);
            y = (y & 0x807fffffu) | ((uint32_t)(120 + (s >> 7) % 16) << 23); break;
    case 2: { /* exponent difference puts the quotient at 2^-118 .. 2^-155 */
        int ea = 1 + (int)((s >> 3) % 100), d = 118 + (int)((s >> 10) % 38);
        int eb = ea + d; if (eb > 254) { ea -= eb - 254; eb = 254; } if (ea < 1) ea = 1;
        x = (x & 0x807fffffu) | ((uint32_t)ea << 23); y = (y & 0x807fffffu) | ((uint32_t)eb << 23); break; }
    case 3: { /* quotient at 2^124 .. 2^130 */
        int eb = 1 + (int)((s >> 3) % 120), ea = eb + 124 + (int)((s >> 10) % 7); if (ea > 254) ea = 254;
        x = (x & 0x807fffffu) | ((uint32_t)ea << 23); y = (y & 0x807fffffu) | ((uint32_t)eb << 23); break; }
    case 4: if (s & 8) x &= 0x807fffffu; if (s & 16) y &= 0x807fffffu; break;
    }
    *a = x; *b = y;
}

/* Fills the DIV edge set: corpus, all signed edge pairs. Returns the count. */
static size_t div_edges(uint32_t *a, uint32_t *b, size_t max) {
    size_t n = 0;
    for (size_t k = 0; k < N_DIV_HARD && n < max; k++) { a[n] = DIV_HARD[k][0]; b[n] = DIV_HARD[k][1]; n++; }
    for (size_t i = 0; i < 2 * N_EDGE_MAG; i++)
        for (size_t j = 0; j < 2 * N_EDGE_MAG && n < max; j++) {
            a[n] = EDGE_MAG[i / 2] | (i & 1 ? 0x80000000u : 0);
            b[n] = EDGE_MAG[j / 2] | (j & 1 ? 0x80000000u : 0);
            n++;
        }
    /* x and y near 2^126 and 2^-126, all sign-free neighbours */
    static const uint32_t NEAR[] = { 0x7e7fffffu, 0x7e800000u, 0x7e800001u, 0x00800000u, 0x007fffffu, 0x00800001u,
                                     0x3f800000u, 0x3f7fffffu, 0x3f800001u, 0x40000000u, 0x3effffffu };
    for (size_t i = 0; i < 11; i++)
        for (size_t j = 0; j < 11 && n < max; j++) { a[n] = NEAR[i]; b[n] = NEAR[j]; n++; }
    return n;
}

/* ---- host tier -------------------------------------------------------------- */

static int host_model_ok(OmegaDsOp op, uint32_t a, uint32_t b, int verbose) {
    uint32_t got = omega_ds_host_exec(op, a, b), want = want_bits(op, a, b);
    uint32_t sem = omega_ds_cpu_semantic(op, a, b);
    if (got == want && sem == want) return 1;
    if (verbose) printf("    %s a=0x%08x b=0x%08x model=0x%08x ieee=0x%08x omega_math=0x%08x\n",
                        omega_ds_op_name(op), a, b, got, want, sem);
    return 0;
}

static int build(OmegaDsOp op, uint8_t *code, size_t *len) {
    return omega_ds_build_kernel(op, code, OMEGA_DS_MAX_CODE_BYTES, len);
}

static void flip_and_expect(const char *id, OmegaDsOp op, const uint8_t *code, size_t len,
                            size_t byte_off, uint8_t xor_mask, const char *reason) {
    uint8_t m[OMEGA_DS_MAX_CODE_BYTES];
    char err[256] = "";
    memcpy(m, code, len);
    m[byte_off] ^= xor_mask;
    int rc = omega_ds_check_kernel(op, m, len, OMEGA_DS_GPR_COUNT, err, sizeof(err));
    int ok = rc != OMEGA_NUMERIC_OK && strncmp(err, reason, strlen(reason)) == 0;
    if (!ok) printf("    %s: rc %d err \"%s\", wanted %s\n", id, rc, err, reason);
    check(ok, id);
}

/* Replaces body instruction i with an encoded instruction. */
static void put_insn(uint8_t *code, size_t i, const OmegaDsInsn *x) {
    uint32_t w[4];
    omega_ds_encode(x, w);
    for (int k = 0; k < 4; k++) memcpy(code + 16 * (OMEGA_DS_PROLOGUE_INSNS + i) + 4 * k, &w[k], 4);
}

static void insn_expect(const char *id, OmegaDsOp op, const uint8_t *code, size_t len, size_t at,
                        OmegaDsInsn x, const char *reason) {
    uint8_t m[OMEGA_DS_MAX_CODE_BYTES];
    char err[256] = "";
    memcpy(m, code, len);
    put_insn(m, at, &x);
    int rc = omega_ds_check_kernel(op, m, len, OMEGA_DS_GPR_COUNT, err, sizeof(err));
    int ok = rc != OMEGA_NUMERIC_OK && strncmp(err, reason, strlen(reason)) == 0;
    if (!ok) printf("    %s: rc %d err \"%s\", wanted %s\n", id, rc, err, reason);
    check(ok, id);
}

static int host_tier(void) {
    char id[128], err[256];
    for (int op = 0; op < OMEGA_DS_OP_COUNT; op++) {
        const char *on = omega_ds_op_name((OmegaDsOp)op);
        OmegaDsInsn body[OMEGA_DS_MAX_BODY];
        size_t nb = omega_ds_body((OmegaDsOp)op, body, OMEGA_DS_MAX_BODY);
        snprintf(id, sizeof(id), "DS_%s_BODY_BUILDS (%zu insns)", on, nb);
        check(nb > 0, id);
        int rt = 1;
        for (size_t i = 0; i < nb; i++) {
            uint32_t w[4];
            OmegaDsInsn d;
            if (omega_ds_encode(&body[i], w) != 0 || omega_ds_decode(w, &d) != 0) { rt = 0; break; }
            char t1[96], t2[96];
            omega_ds_format(&body[i], t1, sizeof(t1)); omega_ds_format(&d, t2, sizeof(t2));
            if (strcmp(t1, t2) != 0) { rt = 0; printf("    %s != %s\n", t1, t2); break; }
        }
        snprintf(id, sizeof(id), "DS_%s_ENCODE_DECODE_ROUND_TRIP", on);
        check(rt, id);

        uint8_t code[OMEGA_DS_MAX_CODE_BYTES];
        size_t len = 0;
        snprintf(id, sizeof(id), "DS_%s_KERNEL_BUILDS", on);
        check(build((OmegaDsOp)op, code, &len) == 0, id);
        int rc = omega_ds_check_kernel((OmegaDsOp)op, code, len, OMEGA_DS_GPR_COUNT, err, sizeof(err));
        if (rc) printf("    %s\n", err);
        snprintf(id, sizeof(id), "DS_%s_KERNEL_PASSES_STRUCTURAL_CHECK", on);
        check(rc == OMEGA_NUMERIC_OK, id);
        rc = omega_ds_check_digest((OmegaDsOp)op, code, len, err, sizeof(err));
        if (rc) printf("    %s\n", err);
        snprintf(id, sizeof(id), "DS_%s_KERNEL_DIGEST_IS_RECORDED", on);
        check(rc == OMEGA_NUMERIC_OK, id);

        /* refusals, one per check */
        size_t body0 = 16 * OMEGA_DS_PROLOGUE_INSNS, stg = body0 + 16 * nb;
        snprintf(id, sizeof(id), "DS_%s_REFUSES_BAD_OP", on);
        check(omega_ds_check_kernel(OMEGA_DS_OP_COUNT, code, len, OMEGA_DS_GPR_COUNT, err, sizeof(err)) != 0 &&
              strncmp(err, "ds_op:", 6) == 0, id);
        snprintf(id, sizeof(id), "DS_%s_REFUSES_BAD_LENGTH", on);
        check(omega_ds_check_kernel((OmegaDsOp)op, code, len - 16, OMEGA_DS_GPR_COUNT, err, sizeof(err)) != 0 &&
              strncmp(err, "ds_len:", 7) == 0, id);
        snprintf(id, sizeof(id), "DS_%s_REFUSES_PROLOGUE_CHANGE", on);
        flip_and_expect(id, (OmegaDsOp)op, code, len, 16 * 13 + 2, 0x01, "ds_prologue:");
        snprintf(id, sizeof(id), "DS_%s_REFUSES_EPILOGUE_CHANGE", on);
        flip_and_expect(id, (OmegaDsOp)op, code, len, stg + 16 + 1, 0x10, "ds_epilogue:");
        snprintf(id, sizeof(id), "DS_%s_REFUSES_PAD_CHANGE", on);
        flip_and_expect(id, (OmegaDsOp)op, code, len, len - 16, 0x01, "ds_pad:");
        snprintf(id, sizeof(id), "DS_%s_REFUSES_PREDICATED_BODY", on);
        flip_and_expect(id, (OmegaDsOp)op, code, len, body0 + 16 * 5 + 1, 0x10, "ds_guard:");
        snprintf(id, sizeof(id), "DS_%s_REFUSES_CONTROL_CHANGE", on);
        flip_and_expect(id, (OmegaDsOp)op, code, len, body0 + 16 * 5 + 13, 0x02, "ds_ctrl:");
        snprintf(id, sizeof(id), "DS_%s_REFUSES_UNRECORDED_FORM", on);
        flip_and_expect(id, (OmegaDsOp)op, code, len, body0 + 16 * 5 + 0, 0x04, "ds_forms:");
        snprintf(id, sizeof(id), "DS_%s_REFUSES_READ_BEFORE_WRITE", on);
        insn_expect(id, (OmegaDsOp)op, code, len, 0,
                    (OmegaDsInsn){ .kind = DSK_IADD3_R, .d = 8, .a = 20, .b = 2, .c = OMEGA_DS_RZ }, "ds_def_use:");
        snprintf(id, sizeof(id), "DS_%s_REFUSES_PRED_READ_BEFORE_WRITE", on);
        insn_expect(id, (OmegaDsOp)op, code, len, 0,
                    (OmegaDsInsn){ .kind = DSK_SEL_R, .d = 8, .a = 2, .b = 5, .ps = 3 }, "ds_pred_def_use:");
        snprintf(id, sizeof(id), "DS_%s_REFUSES_WRITE_TO_ADDRESS_REG", on);
        insn_expect(id, (OmegaDsOp)op, code, len, 0,
                    (OmegaDsInsn){ .kind = DSK_IADD3_R, .d = 6, .a = 2, .b = 5, .c = OMEGA_DS_RZ }, "ds_reserved:");
        snprintf(id, sizeof(id), "DS_%s_REFUSES_REG_ABOVE_ALLOCATION", on);
        insn_expect(id, (OmegaDsOp)op, code, len, nb - 1,
                    (OmegaDsInsn){ .kind = DSK_IADD3_R, .d = 46, .a = 2, .b = 5, .c = OMEGA_DS_RZ }, "ds_gpr:");
        snprintf(id, sizeof(id), "DS_%s_REFUSES_RESULT_NOT_IN_R9", on);
        insn_expect(id, (OmegaDsOp)op, code, len, nb - 1,
                    (OmegaDsInsn){ .kind = DSK_IADD3_R, .d = 8, .a = 2, .b = 5, .c = OMEGA_DS_RZ }, "ds_result:");
        {
            uint8_t m[OMEGA_DS_MAX_CODE_BYTES];
            memcpy(m, code, len);
            memcpy(m + body0, m + stg, 16);  /* STG straight after the prologue: empty body */
            memcpy(m + body0 + 16, m + stg + 16, 32);
            memset(err, 0, sizeof(err));
            snprintf(id, sizeof(id), "DS_%s_REFUSES_EMPTY_BODY", on);
            check(omega_ds_check_kernel((OmegaDsOp)op, m, len, OMEGA_DS_GPR_COUNT, err, sizeof(err)) != 0 &&
                  strncmp(err, "ds_body:", 8) == 0, id);
        }
        {
            uint8_t m[OMEGA_DS_MAX_CODE_BYTES];
            memcpy(m, code, len);
            /* change an immediate: structurally fine, digest must refuse */
            size_t at = 0;
            for (size_t i = 0; i < nb; i++) if (body[i].kind == DSK_LOP3_I) { at = i; break; }
            m[body0 + 16 * at + 4] ^= 0x01;
            int s1 = omega_ds_check_kernel((OmegaDsOp)op, m, len, OMEGA_DS_GPR_COUNT, err, sizeof(err));
            int s2 = omega_ds_check_digest((OmegaDsOp)op, m, len, err, sizeof(err));
            snprintf(id, sizeof(id), "DS_%s_REFUSES_CHANGED_IMMEDIATE_BY_DIGEST", on);
            check(s1 == OMEGA_NUMERIC_OK && s2 != 0 && strncmp(err, "ds_digest:", 10) == 0, id);
        }
    }
    {
        uint32_t x[4] = { 0 }, o[4];
        check(omega_ds_check_args(OMEGA_DS_OP_COUNT, x, x, o, 4, err, sizeof(err)) != 0 && !strncmp(err, "ds_args_op:", 11),
              "DS_ARGS_REFUSE_BAD_OP");
        check(omega_ds_check_args(OMEGA_DS_DIV, x, NULL, o, 4, err, sizeof(err)) != 0 && !strncmp(err, "ds_buffers:", 11),
              "DS_ARGS_REFUSE_MISSING_B");
        check(omega_ds_check_args(OMEGA_DS_SQRT, x, NULL, o, 4, err, sizeof(err)) == 0, "DS_ARGS_SQRT_NEEDS_NO_B");
        check(omega_ds_check_args(OMEGA_DS_SQRT, x, NULL, o, 0, err, sizeof(err)) != 0 && !strncmp(err, "ds_count:", 9),
              "DS_ARGS_REFUSE_ZERO_COUNT");
        check(omega_ds_check_args(OMEGA_DS_SQRT, x, NULL, o, (size_t)OMEGA_DS_MAX_BATCH + 1, err, sizeof(err)) != 0 &&
              !strncmp(err, "ds_count:", 9), "DS_ARGS_REFUSE_OVERSIZE_COUNT");
        uint32_t q[OMEGA_BW_QMD_WORDS];
        OmegaBlackwellQmdConfig c = { .code_va = 0x200000000ull, .cbank_va = 0x200100000ull, .scratch_va = 0x200204000ull,
                                      .sem_va = 0x200202000ull, .qmd0_va = 0x200200000ull, .qmd1_va = 0x200201000ull,
                                      .num_elements = 1024, .threads_per_block = 64, .grid_width = 16,
                                      .gpr_count = OMEGA_DS_GPR_COUNT };
        omega_blackwell_build_qmd1(q, &c);
        check(omega_ds_check_qmd(q, c.code_va, err, sizeof(err)) == 0, "DS_QMD_ACCEPTS_BUILT_QMD");
        uint32_t q2[OMEGA_BW_QMD_WORDS];
        memcpy(q2, q, sizeof(q)); q2[37] = 1;
        check(omega_ds_check_qmd(q2, c.code_va, err, sizeof(err)) != 0 && !strncmp(err, "ds_qmd:", 7), "DS_QMD_REFUSES_INVARIANT_BREAK");
        c.gpr_count = 16; omega_blackwell_build_qmd1(q2, &c);
        check(omega_ds_check_qmd(q2, c.code_va, err, sizeof(err)) != 0 && !strncmp(err, "ds_qmd_gpr:", 11), "DS_QMD_REFUSES_16_REGISTERS");
        check(omega_ds_check_qmd(q, c.code_va + 0x1000, err, sizeof(err)) != 0 && !strncmp(err, "ds_qmd_code:", 12),
              "DS_QMD_REFUSES_OTHER_PROGRAM");
        memcpy(q2, q, sizeof(q)); q2[33] ^= 1u; /* program address bit 36: 64 GiB away */
        check(omega_ds_check_qmd(q2, c.code_va, err, sizeof(err)) != 0 && !strncmp(err, "ds_qmd_code:", 12), "DS_QMD_REFUSES_HIGH_ADDRESS_BITS");
    }

    /* host model of the exact body vs FDIV/FSQRT and omega_math_* */
    int ok = 1;
    for (size_t k = 0; k < N_DIV_HARD; k++) ok &= host_model_ok(OMEGA_DS_DIV, DIV_HARD[k][0], DIV_HARD[k][1], 1);
    for (size_t k = 0; k < sizeof(SQRT_HARD) / 4; k++) ok &= host_model_ok(OMEGA_DS_SQRT, SQRT_HARD[k], 0, 1);
    check(ok, "DS_HOST_MODEL_HARD_CORPUS");
    static uint32_t ea[1 << 13], eb[1 << 13];
    size_t ne = div_edges(ea, eb, 1 << 13);
    size_t bad = 0;
    for (size_t k = 0; k < ne; k++) bad += !host_model_ok(OMEGA_DS_DIV, ea[k], eb[k], bad < 8);
    for (size_t i = 0; i < 2 * N_EDGE_MAG; i++)
        bad += !host_model_ok(OMEGA_DS_SQRT, EDGE_MAG[i / 2] | (i & 1 ? 0x80000000u : 0), 0, bad < 8);
    snprintf(id, sizeof(id), "DS_HOST_MODEL_EDGE_CLASSES (%zu DIV pairs)", ne);
    check(bad == 0, id);
    bad = 0;
    for (size_t k = 0; k < 200000; k++) {
        uint32_t a, b;
        rnd_pair(&a, &b);
        bad += !host_model_ok(OMEGA_DS_DIV, a, b, bad < 8);
        bad += !host_model_ok(OMEGA_DS_SQRT, a, 0, bad < 8);
    }
    check(bad == 0, "DS_HOST_MODEL_RANDOM_200K");
    printf("SUMMARY passed=%d failed=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

/* ---- dumps and digests ---------------------------------------------------------- */

static int dump(const char *dir) {
    static char lst[1 << 20];
    for (int op = 0; op < OMEGA_DS_OP_COUNT; op++) {
        uint8_t code[OMEGA_DS_MAX_CODE_BYTES];
        size_t len = 0;
        char path[512];
        static const char *const NM[OMEGA_DS_OP_COUNT] = { "div", "sqrt", "exp2", "log2", "sigmoid", "tanh", "sin", "cos", "erf", "gelu", "rsqrt" };
        const char *nm = NM[op];
        if (build((OmegaDsOp)op, code, &len) != 0 || omega_ds_listing((OmegaDsOp)op, lst, sizeof(lst)) != 0) return 1;
        snprintf(path, sizeof(path), "%s/%s.bin", dir, nm);
        FILE *f = fopen(path, "wb");
        if (!f || fwrite(code, 1, len, f) != len) return 1;
        fclose(f);
        snprintf(path, sizeof(path), "%s/%s.lst", dir, nm);
        f = fopen(path, "w");
        if (!f || fputs(lst, f) < 0) return 1;
        fclose(f);
    }
    return 0;
}

static int digests(void) {
    for (int op = 0; op < OMEGA_DS_OP_COUNT; op++) {
        uint8_t code[OMEGA_DS_MAX_CODE_BYTES], dg[32];
        size_t len = 0;
        if (build((OmegaDsOp)op, code, &len) != 0 || omega_blackwell_compute_code_digest(code, len, dg) != 0) return 1;
        printf("%s %zu ", omega_ds_op_name((OmegaDsOp)op), len / 16);
        for (int i = 0; i < 32; i++) printf("%02x", dg[i]);
        printf("\n");
    }
    return 0;
}

/* ---- exhaustive host model ---------------------------------------------------------- */

typedef struct { uint64_t lo, hi, bad; } Slice;
static void *sqrt_slice(void *p) {
    Slice *s = p;
    for (uint64_t u = s->lo; u < s->hi; u++)
        if (omega_ds_host_exec(OMEGA_DS_SQRT, (uint32_t)u, 0) != want_bits(OMEGA_DS_SQRT, (uint32_t)u, 0)) s->bad++;
    return NULL;
}

static int host_sqrt_all(void) {
    enum { T = 16 };
    pthread_t th[T];
    Slice s[T];
    omega_ds_host_exec(OMEGA_DS_SQRT, 0, 0); /* fill the body cache before threads */
    for (int i = 0; i < T; i++) {
        s[i].lo = (1ull << 32) * i / T; s[i].hi = (1ull << 32) * (i + 1) / T; s[i].bad = 0;
        if (pthread_create(&th[i], NULL, sqrt_slice, &s[i]) != 0) { printf("[FAIL] host SQRT: thread %d not started\n", i); return 2; }
    }
    uint64_t bad = 0;
    for (int i = 0; i < T; i++) {
        if (pthread_join(th[i], NULL) != 0) { printf("[FAIL] host SQRT: thread %d not joined\n", i); return 2; }
        bad += s[i].bad;
    }
    printf("RESULT host SQRT checked=4294967296 mismatches=%" PRIu64 "\n", bad);
    return bad ? 1 : 0;
}

static int host_div(uint64_t n) {
    static uint32_t ea[1 << 13], eb[1 << 13];
    size_t ne = div_edges(ea, eb, 1 << 13);
    uint64_t bad = 0;
    for (size_t k = 0; k < ne; k++) bad += !host_model_ok(OMEGA_DS_DIV, ea[k], eb[k], bad < 8);
    for (uint64_t k = 0; k < n; k++) {
        uint32_t a, b;
        rnd_pair(&a, &b);
        bad += !host_model_ok(OMEGA_DS_DIV, a, b, bad < 8);
    }
    printf("RESULT host DIV checked=%" PRIu64 " mismatches=%" PRIu64 "\n", n + ne, bad);
    return bad ? 1 : 0;
}

/* ---- chip tier ------------------------------------------------------------------------- */
#ifndef OMEGA_NUMERIC_CPU_ONLY

#define BATCH (1u << 24)
#define DIV_RANDOM_BATCHES 6

static int chip(void) {
    uint32_t *a = malloc((size_t)BATCH * 4), *b = malloc((size_t)BATCH * 4), *o = malloc((size_t)BATCH * 4);
    if (!a || !b || !o) return 2;
    uint64_t dv_fill = 0, sq_fill = 0; /* mismatches that still hold the 0x55555555 fill: never written */
    uint64_t sq_checked = 0, sq_bad = 0, dv_checked = 0, dv_bad = 0, dv_edges = 0, dv_math_bad = 0, sq_math_bad = 0;
    int dev_err = 0;
    /* DIV: batch 0 starts with the corpus and the edge set */
    for (int bi = 0; bi < DIV_RANDOM_BATCHES + 1 && !dev_err; bi++) {
        size_t n0 = 0;
        if (bi == 0) { n0 = div_edges(a, b, BATCH); dv_edges = n0; }
        for (size_t k = n0; k < BATCH; k++) rnd_pair(&a[k], &b[k]);
        int rc = omega_ds_gb10_run(OMEGA_DS_DIV, a, b, o, BATCH);
        if (rc != OMEGA_NUMERIC_OK) { printf("    DIV batch %d: device rc %d\n", bi, rc); dev_err = 1; break; }
        for (size_t k = 0; k < BATCH; k++) {
            uint32_t w = want_bits(OMEGA_DS_DIV, a[k], b[k]);
            if (o[k] != w) { dv_fill += o[k] == 0x55555555u; if (dv_bad < 16) printf("    DIV 0x%08x / 0x%08x: chip 0x%08x want 0x%08x\n", a[k], b[k], o[k], w); dv_bad++; }
            if (o[k] != omega_ds_cpu_semantic(OMEGA_DS_DIV, a[k], b[k])) dv_math_bad++;
        }
        dv_checked += BATCH;
        printf("    DIV batch %d/%d done, mismatches so far %" PRIu64 "\n", bi + 1, DIV_RANDOM_BATCHES + 1, dv_bad);
        fflush(stdout);
    }
    /* SQRT: every 32-bit input, 256 batches of 2^24 */
    for (uint32_t bi = 0; bi < 256 && !dev_err; bi++) {
        for (uint32_t k = 0; k < BATCH; k++) a[k] = (bi << 24) | k;
        int rc = omega_ds_gb10_run(OMEGA_DS_SQRT, a, NULL, o, BATCH);
        if (rc != OMEGA_NUMERIC_OK) { printf("    SQRT batch %u: device rc %d\n", bi, rc); dev_err = 1; break; }
        for (uint32_t k = 0; k < BATCH; k++) {
            uint32_t w = want_bits(OMEGA_DS_SQRT, a[k], 0);
            if (o[k] != w) { sq_fill += o[k] == 0x55555555u; if (sq_bad < 16) printf("    SQRT 0x%08x: chip 0x%08x want 0x%08x\n", a[k], o[k], w); sq_bad++; }
            if (o[k] != omega_ds_cpu_semantic(OMEGA_DS_SQRT, a[k], 0)) sq_math_bad++;
        }
        sq_checked += BATCH;
        if (bi % 16 == 15) { printf("    SQRT batch %u/256 done, mismatches so far %" PRIu64 "\n", bi + 1, sq_bad); fflush(stdout); }
    }
    const char *dv = dev_err && dv_checked < (uint64_t)BATCH * (DIV_RANDOM_BATCHES + 1) ? "NOT_RUN" : dv_bad || dv_math_bad ? "FAIL" : "PASS";
    const char *sq = dev_err ? "NOT_RUN" : sq_bad || sq_math_bad ? "FAIL" : "PASS";
    printf("RESULT chip DIV checked=%" PRIu64 " edge_and_corpus=%" PRIu64 " mismatches=%" PRIu64 " mismatches_vs_omega_math=%" PRIu64 " unwritten=%" PRIu64 " seed=0x9e3779b97f4a7c15 verdict=%s\n",
           dv_checked, dv_edges, dv_bad, dv_math_bad, dv_fill, dv);
    printf("RESULT chip SQRT checked=%" PRIu64 " mismatches=%" PRIu64 " mismatches_vs_omega_math=%" PRIu64 " unwritten=%" PRIu64 " verdict=%s\n",
           sq_checked, sq_bad, sq_math_bad, sq_fill, sq);
    const char *v = (!strcmp(dv, "PASS") && !strcmp(sq, "PASS")) ? "PASS" : (!strcmp(dv, "FAIL") || !strcmp(sq, "FAIL")) ? "FAIL" : "NOT_RUN";
    printf("VERDICT %s\n", v);
    free(a); free(b); free(o);
    return strcmp(v, "PASS") ? 1 : 0;
}
#endif

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc >= 3 && !strcmp(argv[1], "--dump")) return dump(argv[2]);
    if (argc >= 2 && !strcmp(argv[1], "--digest")) return digests();
    if (argc >= 2 && !strcmp(argv[1], "--host-sqrt-all")) return host_sqrt_all();
    if (argc >= 3 && !strcmp(argv[1], "--host-div")) return host_div(strtoull(argv[2], NULL, 0));
    if (argc >= 2 && !strcmp(argv[1], "--chip")) {
#ifdef OMEGA_NUMERIC_CPU_ONLY
        printf("VERDICT NOT_RUN (CPU-only build)\n");
        return 2;
#else
        return chip();
#endif
    }
    return host_tier();
}
