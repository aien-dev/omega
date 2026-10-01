/*
 * Gate 5: OMEGA-NUMERIC-0 qualification suite.
 *
 * Two builds:
 *   -DOMEGA_NUMERIC_CPU_ONLY  host tiers only (reference and CPU realization).
 *                             Opens no device. Gate items that need GB10 print
 *                             [SKIP], never [PASS]. The hardware descriptor
 *                             line is a marked fake.
 *   (default)                 adds the GB10 tier through
 *                             omega_gb10_execute_simt_op and a FORGE hardware
 *                             probe. Run only through tests/run_numeric_gates.sh.
 *
 * Machine-readable lines (parsed by tests/run_numeric_gates.sh):
 *   [PASS] ID / [FAIL] ID / [SKIP] ID
 *   OMEGA_NUMERIC_HWDESC_JSON:{...}
 *   OMEGA_NUMERIC_PARITY_JSON:{...}   one per op per tier
 *   OMEGA_NUMERIC_FINDING_JSON:{...}
 *   OMEGA_NUMERIC_ORACLE_JSON:{...}    CPU tier against the independent oracle
 *                                     in tests/numeric_oracle.h (host only)
 */
#include "omega_numeric.h"
#include "omega_numeric_provenance.h"
#include "omega_blackwell_codegen.h"
#include "omega_blackwell_qmd.h"

#ifndef OMEGA_NUMERIC_CPU_ONLY
#include "forge_descriptor.h"
#endif

#include "sha256.h"
#include "numeric_oracle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_total = 0, g_passed = 0, g_failed = 0, g_skipped = 0;

static void report(const char *name, int ok) {
    g_total++;
    if (ok) { g_passed++; printf("[PASS] %s\n", name); }
    else    { g_failed++; printf("[FAIL] %s\n", name); }
}

/* Test IDs that only a GB10 chip can decide. The CPU-only build (make
 * test-numeric-cpu) must SKIP exactly these and nothing else. */
static const char *const CHIP_ONLY_IDS[] = {
    "CPU_GB10_BIT_PARITY", "SUBNORMALS_PRESERVED_NO_FTZ", "MUFU_SEED_ONLY_NOT_COMPARED",
    "EDGE_CLASS_BEHAVIOR_VERIFIED", "HARDWARE_DESCRIPTOR_PROBED",
};
#define CHIP_ONLY_COUNT (sizeof(CHIP_ONLY_IDS) / sizeof(CHIP_ONLY_IDS[0]))
static bool g_chip_only_skipped[CHIP_ONLY_COUNT];
static int g_undeclared_skips = 0;

static void skip(const char *name, const char *why) {
    g_skipped++;
    bool declared = false;
    for (size_t k = 0; k < CHIP_ONLY_COUNT; k++) {
        if (strcmp(name, CHIP_ONLY_IDS[k]) == 0 && !g_chip_only_skipped[k]) {
            g_chip_only_skipped[k] = true;
            declared = true;
        }
    }
    if (!declared) g_undeclared_skips++;
    printf("[SKIP] %s (%s)\n", name, why);
}

static uint32_t lcg_next(uint32_t *state) {
    *state = (*state * 1664525u + 1013904223u);
    return *state;
}

static float F(uint32_t u) { return omega_bits_to_float(u); }
static uint32_t U(float f) { return omega_float_to_bits(f); }

/* ---- Corpus ---------------------------------------------------------------- */

#define N 4096u
#define EDGE_COUNT 32u

static const uint32_t EDGE[EDGE_COUNT] = {
    0x00000000u, 0x80000000u, 0x7f800000u, 0xff800000u, /* +0 -0 +inf -inf        */
    0x7fc00000u, 0xffc00000u, 0x7f800001u, 0x7fbfffffu, /* +qNaN -qNaN sNaN sNaN   */
    0x00000001u, 0x80000001u, 0x00000002u, 0x00000003u, /* subnormals              */
    0x0000ffffu, 0x007fffffu, 0x807fffffu, 0x00400000u,
    0x00800000u, 0x80800000u, 0x00800001u, 0x7f7fffffu, /* min normal, max normal  */
    0xff7fffffu, 0x3f800000u, 0xbf800000u, 0x3f000000u, /* +-max, +-1, 0.5         */
    0x40000000u, 0x40400000u, 0x0d800000u, 0x2b800000u, /* 2, 3, 2^-100, 2^-40     */
    0x71800000u, 0x7e800000u, 0x3f7fffffu, 0x4f000000u, /* 2^100, 2^126, 1-ulp, 2^31 */
};

enum { CL_ZERO, CL_INF, CL_NAN, CL_SUB, CL_NORMAL, CL_COUNT };
static const char *CL_NAME[CL_COUNT] = { "zero", "inf", "nan", "subnormal", "normal" };

static int classify(float x) {
    if (omega_iszero(x)) return CL_ZERO;
    if (omega_isinf(x)) return CL_INF;
    if (omega_isnan(x)) return CL_NAN;
    if (omega_issubnormal(x)) return CL_SUB;
    return CL_NORMAL;
}

static float g_a[N], g_b[N];

static void build_corpus(void) {
    uint32_t s = 0x19283746u;
    size_t i = 0;
    /* 0..1023: edge class cross product (a from EDGE[i/32], b from EDGE[i%32]) */
    for (; i < 1024; i++) { g_a[i] = F(EDGE[i / 32]); g_b[i] = F(EDGE[i % 32]); }
    /* 1024..1535: subnormal pairs with random signs: sums and differences stay subnormal */
    for (; i < 1536; i++) {
        uint32_t r1 = lcg_next(&s), r2 = lcg_next(&s);
        g_a[i] = F((r1 & 0x80000000u) | ((r1 >> 3) & 0x003fffffu) | 1u);
        g_b[i] = F((r2 & 0x80000000u) | ((r2 >> 3) & 0x003fffffu));
    }
    /* 1536..2047: products that underflow into the subnormal range */
    for (; i < 2048; i++) {
        uint32_t r1 = lcg_next(&s), r2 = lcg_next(&s);
        uint32_t e1 = 61u + (r1 >> 28) % 6u, e2 = 61u + (r2 >> 28) % 6u; /* 2^-66 .. 2^-61 */
        g_a[i] = F((r1 & 0x80000000u) | (e1 << 23) | (r1 & 0x007fffffu));
        g_b[i] = F((r2 & 0x80000000u) | (e2 << 23) | (r2 & 0x007fffffu));
    }
    /* 2048..3071: raw random bit patterns */
    for (; i < 3072; i++) { g_a[i] = F(lcg_next(&s)); g_b[i] = F(lcg_next(&s)); }
    /* 3072..3199: a = -(1 - k 2^-24), b = 2^-126: with c = min normal, FFMA lands subnormal */
    for (uint32_t k = 1; i < 3200; i++, k++) { g_a[i] = F(0xbf800000u - k); g_b[i] = F(0x00800000u); }
    /* 3200..3455: conversion boundaries */
    static const uint32_t CONV[] = {
        0x4f000000u, 0xcf000000u, 0x4effffffu, 0xceffffffu, 0x4f000001u, 0xcf000001u,
        0x4b800000u, 0xcb800000u, 0x4b800001u, 0x3f000000u, 0xbf000000u, 0x3fc00000u,
        0xbfc00000u, 0x3f7fffffu, 0xbf7fffffu, 0x7fffffffu,
    };
    for (size_t k = 0; i < 3456; i++, k++) {
        g_a[i] = F(CONV[k % (sizeof(CONV) / sizeof(CONV[0]))]);
        uint32_t r = lcg_next(&s);
        g_b[i] = F((r & 0x807fffffu) | (0x7fu << 23));
    }
    /* 3456..4095: ordinary finite values, mixed magnitudes */
    for (; i < N; i++) {
        uint32_t r1 = lcg_next(&s), r2 = lcg_next(&s);
        g_a[i] = F((r1 & 0x807fffffu) | ((100u + (r1 >> 25) % 56u) << 23));
        g_b[i] = F((r2 & 0x807fffffu) | ((100u + (r2 >> 25) % 56u) << 23));
    }
}

static const uint32_t FFMA_C[] = {
    0x3f800000u, 0x80000000u, 0x00800000u, 0xbf800000u,
    0x7fc00000u, 0x7f800000u, 0x00000003u, 0x80800000u,
};
#define FFMA_C_COUNT (sizeof(FFMA_C) / sizeof(FFMA_C[0]))

static bool ftz_sensitive(OmegaNumericOp op) {
    return op == OMEGA_NOP_FADD || op == OMEGA_NOP_FSUB || op == OMEGA_NOP_FMUL ||
           op == OMEGA_NOP_FFMA || op == OMEGA_NOP_FMNMX_MIN || op == OMEGA_NOP_FMNMX_MAX;
}

/* Per-class parity on top of omega_numeric_parity (class of input a). */
typedef struct { size_t n[CL_COUNT]; size_t bad[CL_COUNT]; } ClassTrace;

static void class_trace(OmegaNumericOp op, const float *a, const float *expect, const float *got,
                        size_t count, ClassTrace *ct) {
    const OmegaNumericOpInfo *info = omega_numeric_op_at(op);
    memset(ct, 0, sizeof(*ct));
    for (size_t i = 0; i < count; i++) {
        if (!omega_numeric_element_checked(op, i)) continue;
        int c = classify(a[i]);
        ct->n[c]++;
        bool same = omega_numeric_compare_equal(info->compare, U(expect[i]), U(got[i]));
        if (!same) ct->bad[c]++;
    }
}

static void print_parity_json(const char *op, const char *tier, const char *compare, uint32_t c_bits,
                              bool has_c, size_t n, const OmegaParityTrace *t, const ClassTrace *ct) {
    printf("OMEGA_NUMERIC_PARITY_JSON:{\"op\":\"%s\",\"tier\":\"%s\",\"compare\":\"%s\",", op, tier, compare);
    if (has_c) printf("\"c_bits\":\"0x%08x\",", c_bits);
    if (strcmp(op, "REDUCE_SUM") == 0) printf("\"reduction_order\":\"%s\",", OMEGA_WARP_REDUCTION_DECLARED_ORDER);
    printf("\"n\":%zu,\"checked\":%zu,\"mismatches\":%zu,\"subnormal_expected\":%zu,",
           n, t->checked, t->mismatches, t->subnormal_expected);
    if (t->first_index >= 0) {
        printf("\"first_mismatch\":{\"index\":%ld,\"expect\":\"0x%08x\",\"got\":\"0x%08x\"},",
               t->first_index, t->first_expect, t->first_got);
    } else {
        printf("\"first_mismatch\":null,");
    }
    printf("\"by_input_class\":{");
    for (int c = 0; c < CL_COUNT; c++) {
        printf("%s\"%s\":{\"n\":%zu,\"mismatches\":%zu}", c ? "," : "", CL_NAME[c],
               ct ? ct->n[c] : 0, ct ? ct->bad[c] : 0);
    }
    printf("}}\n");
}

static __attribute__((unused)) void print_bound_json(const char *op, const char *tier, size_t n, const OmegaParityTrace *t) {
    printf("OMEGA_NUMERIC_PARITY_JSON:{\"op\":\"%s\",\"tier\":\"%s\",\"compare\":\"SEED_BOUND\","
           "\"bound\":\"rel<=2^-20 vs IEEE, finite normal in and out\",\"n\":%zu,\"checked\":%zu,"
           "\"out_of_bound\":%zu,\"skipped\":%zu,", op, tier, n, t->checked, t->out_of_bound, t->bound_skipped);
    if (t->first_index >= 0)
        printf("\"first_violation\":{\"index\":%ld,\"expect\":\"0x%08x\",\"got\":\"0x%08x\"}}\n",
               t->first_index, t->first_expect, t->first_got);
    else
        printf("\"first_violation\":null}\n");
}

static void fill_c(float *c, uint32_t bits) {
    for (size_t i = 0; i < N; i++) c[i] = F(bits);
}

/* ---- Tier checks -------------------------------------------------------------- */

typedef struct {
    bool ok;               /* zero mismatches everywhere it applies           */
    bool subnormals_seen;  /* FTZ-sensitive ops: subnormal expected results    */
} TierResult;

static float g_c[N], g_ref[N], g_cpu[N], g_dev[N];

/* CPU tier against reference for one op (all c values for FFMA). */
static TierResult cpu_tier(OmegaNumericOp op) {
    TierResult tr = { true, false };
    const OmegaNumericOpInfo *info = omega_numeric_op_at(op);
    size_t launches = (op == OMEGA_NOP_FFMA) ? FFMA_C_COUNT : 1;
    size_t sub_total = 0;
    for (size_t l = 0; l < launches; l++) {
        const float *c = NULL;
        if (op == OMEGA_NOP_FFMA) { fill_c(g_c, FFMA_C[l]); c = g_c; }
        if (op == OMEGA_NOP_FFMA_V) { for (size_t i = 0; i < N; i++) g_c[i] = g_a[N - 1 - i]; c = g_c; }
        if (omega_numeric_reference(op, g_a, g_b, c, g_ref, N) != 0 ||
            omega_numeric_cpu_realize(op, g_a, g_b, c, g_cpu, N) != 0) {
            tr.ok = false;
            continue;
        }
        OmegaParityTrace t;
        if (info->compare == OMEGA_CMP_SEED_BOUND) {
            /* Host tier has no MUFU; its realization is the IEEE value itself. */
            memset(&t, 0, sizeof(t)); t.first_index = -1;
            for (size_t i = 0; i < N; i++) {
                t.checked++;
                if (!omega_numeric_bits_equal(g_ref[i], g_cpu[i])) {
                    if (!t.mismatches) { t.first_index = (long)i; t.first_expect = U(g_ref[i]); t.first_got = U(g_cpu[i]); }
                    t.mismatches++;
                }
            }
            /* Reference and CPU tier both run the host fdiv/fsqrt here, so this line
             * only shows they agree with each other. The independent check is
             * the integer oracle (CPU_TIER_INDEPENDENT_ORACLE). */
            print_parity_json(info->name, "cpu", "SELF_CONSISTENT_HOST_FDIV", 0, false, N, &t, NULL);
        } else {
            if (omega_numeric_parity(op, g_ref, g_cpu, N, &t) != 0) { tr.ok = false; continue; }
            ClassTrace ct;
            class_trace(op, g_a, g_ref, g_cpu, N, &ct);
            /* EXP and LOG: reference and CPU tier call the same omega_math_* sequence,
             * so agreement is self-consistency only. Accuracy is checked against the
             * binary128 oracle with a stated ulp bound. */
            bool self_only = op == OMEGA_NOP_EXP || op == OMEGA_NOP_LOG;
            print_parity_json(info->name, "cpu", self_only ? "SELF_CONSISTENT" : omega_numeric_compare_name(info->compare),
                              op == OMEGA_NOP_FFMA ? FFMA_C[l] : 0, op == OMEGA_NOP_FFMA, N, &t, &ct);
        }
        if (t.mismatches) tr.ok = false;
        sub_total += t.subnormal_expected;
    }
    tr.subnormals_seen = sub_total > 0;
    return tr;
}

#ifndef OMEGA_NUMERIC_CPU_ONLY
/* GB10 tier against reference for one encoded op. */
static TierResult gb10_tier(OmegaNumericOp op, bool *class_ok) {
    TierResult tr = { true, false };
    const OmegaNumericOpInfo *info = omega_numeric_op_at(op);
    size_t launches = (op == OMEGA_NOP_FFMA) ? FFMA_C_COUNT : 1;
    size_t sub_total = 0;
    for (size_t l = 0; l < launches; l++) {
        const float *c = NULL;
        if (op == OMEGA_NOP_FFMA) { fill_c(g_c, FFMA_C[l]); c = g_c; }
        if (op == OMEGA_NOP_FFMA_V) { for (size_t i = 0; i < N; i++) g_c[i] = g_a[N - 1 - i]; c = g_c; }
        int rc = omega_numeric_reference(op, g_a, g_b, c, g_ref, N);
        if (rc != 0) {
            printf("OMEGA_NUMERIC_PARITY_JSON:{\"op\":\"%s\",\"tier\":\"gb10\",\"error\":%d,\"stage\":\"reference\"}\n", info->name, rc);
            tr.ok = false;
            *class_ok = false;
            continue;
        }
        rc = omega_gb10_execute_simt_op(info->name, g_a, info->arity >= 2 ? g_b : NULL, c, g_dev, N);
        if (rc != 0) {
            printf("OMEGA_NUMERIC_PARITY_JSON:{\"op\":\"%s\",\"tier\":\"gb10\",\"error\":%d}\n", info->name, rc);
            tr.ok = false;
            *class_ok = false;
            continue;
        }
        OmegaParityTrace t;
        if (info->compare == OMEGA_CMP_SEED_BOUND) {
            rc = omega_numeric_seed_bound(op, g_a, g_dev, N, &t);
            if (rc != 0) {
                printf("OMEGA_NUMERIC_PARITY_JSON:{\"op\":\"%s\",\"tier\":\"gb10\",\"error\":%d,\"stage\":\"seed_bound\"}\n", info->name, rc);
                tr.ok = false;
                continue;
            }
            print_bound_json(info->name, "gb10", N, &t);
            if (t.out_of_bound || t.checked == 0) tr.ok = false;
            continue;
        }
        rc = omega_numeric_parity(op, g_ref, g_dev, N, &t);
        if (rc != 0) {
            printf("OMEGA_NUMERIC_PARITY_JSON:{\"op\":\"%s\",\"tier\":\"gb10\",\"error\":%d,\"stage\":\"parity\"}\n", info->name, rc);
            tr.ok = false;
            *class_ok = false;
            continue;
        }
        ClassTrace ct;
        class_trace(op, g_a, g_ref, g_dev, N, &ct);
        print_parity_json(info->name, "gb10", omega_numeric_compare_name(info->compare),
                          op == OMEGA_NOP_FFMA ? FFMA_C[l] : 0, op == OMEGA_NOP_FFMA, N, &t, &ct);
        for (int k = 0; k < CL_COUNT; k++) if (ct.bad[k]) *class_ok = false;
        if (t.mismatches) tr.ok = false;
        sub_total += t.subnormal_expected;
    }
    tr.subnormals_seen = sub_total > 0;
    return tr;
}

static int print_hw_descriptor(void) {
    ForgeMachineDescriptor d;
    memset(&d, 0, sizeof(d));
    uint8_t dig[FORGE_DESC_DIGEST_LEN];
    char hex[65];
    if (forge_probe_hardware(&d) != 0 || forge_descriptor_normalize(&d) != 0 ||
        forge_descriptor_compute_digest(&d, dig) != 0) {
        printf("OMEGA_NUMERIC_HWDESC_JSON:{\"source\":\"FORGE_PROBE_FAILED\"}\n");
        return -1;
    }
    forge_descriptor_digest_hex(dig, hex);
    const char *alias = forge_descriptor_derived_alias(&d);
    printf("OMEGA_NUMERIC_HWDESC_JSON:{\"source\":\"FORGE_PROBE\",\"descriptor_digest\":\"%s\","
           "\"derived_alias\":\"%s\"}\n", hex, alias ? alias : "");
    return 0;
}
#endif

/* Binds the log to this run and this binary: the run id the qualifier set in
 * OMEGA_NUMERIC_RUN_ID and the SHA-256 of /proc/self/exe, which the qualifier
 * compares with the file it built. */
static void print_run_line(void) {
    const char *id = getenv("OMEGA_NUMERIC_RUN_ID");
    size_t n = id ? strlen(id) : 0;
    if (n == 0 || n > 128) id = "";
    for (size_t i = 0; id[0] && i < n; i++) {
        char ch = id[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == '-' || ch == '_' || ch == '.')) { id = ""; break; }
    }
    char hex[65] = "";
    FILE *f = fopen("/proc/self/exe", "rb");
    if (f) {
        sha256_ctx ctx;
        uint8_t buf[65536], dig[SHA256_DIGEST_SIZE];
        size_t got;
        sha256_init(&ctx);
        while ((got = fread(buf, 1, sizeof(buf), f)) > 0) sha256_update(&ctx, buf, got);
        if (!ferror(f)) {
            sha256_final(&ctx, dig);
            for (int i = 0; i < SHA256_DIGEST_SIZE; i++) snprintf(hex + 2 * i, 3, "%02x", dig[i]);
        }
        fclose(f);
    }
    printf("OMEGA_NUMERIC_RUN_JSON:{\"run_id\":\"%s\",\"binary_sha256\":\"%s\"}\n", id, hex);
}

/* ---- Main ----------------------------------------------------------------------- */

/* ---- Independent oracle (tests/numeric_oracle.h) ---------------------------- */

/* Ulp bounds for the Omega EXP and LOG polynomials against the binary128
 * value rounded once to FP32. They are accuracy bounds, not bit-exactness:
 * the degree-5 exp polynomial is not correctly rounded. Measured 2026-09-30
 * on every 257th FP32 bit pattern (16.7 million inputs): EXP worst 39 ulp
 * (normal results, near |r| = ln2/2) and 19 ulp (subnormal results, near
 * x = -87.68); LOG worst 3 ulp. On the 4096-element Gate 5 corpus: EXP 37,
 * LOG 1. The bounds below are those sweep maxima plus one. */
#define NUM_ORACLE_EXP_ULP 40u
#define NUM_ORACLE_LOG_ULP 4u

static uint32_t oracle_value(OmegaNumericOp op, size_t i, const float *c) {
    uint32_t a = U(g_a[i]), b = U(g_b[i]);
    switch (op) {
    case OMEGA_NOP_FADD: return or_add(a, b);
    case OMEGA_NOP_FSUB: return or_sub(a, b);
    case OMEGA_NOP_FMUL: return or_mul(a, b);
    case OMEGA_NOP_FFMA: return or_fma(a, b, U(c[i]));
    case OMEGA_NOP_I2FP: return or_i2f((int32_t)a);
    case OMEGA_NOP_DIV: return or_div(a, b);
    case OMEGA_NOP_SQRT: return or_sqrt(a);
    case OMEGA_NOP_MUFU_RCP: return or_rcp(a);
    case OMEGA_NOP_MUFU_RSQ: return or_rsq(a);
    default: return OR_QNAN;
    }
}

static void print_oracle_json(const char *op, const char *against, bool has_c, uint32_t c_bits,
                              size_t n, size_t bad, long first, uint32_t want, uint32_t got) {
    printf("OMEGA_NUMERIC_ORACLE_JSON:{\"op\":\"%s\",\"oracle\":\"integer_softfloat_rne\",\"against\":\"%s\",",
           op, against);
    if (has_c) printf("\"c_bits\":\"0x%08x\",", c_bits);
    printf("\"n\":%zu,\"mismatches\":%zu,", n, bad);
    if (first >= 0)
        printf("\"first_mismatch\":{\"index\":%ld,\"oracle\":\"0x%08x\",\"got\":\"0x%08x\"}}\n", first, want, got);
    else
        printf("\"first_mismatch\":null}\n");
}

/* Bit-exact ops: the reference and the CPU realization must both equal the
 * integer soft-float answer on every element. */
static bool oracle_exact(OmegaNumericOp op) {
    const OmegaNumericOpInfo *info = omega_numeric_op_at(op);
    size_t launches = (op == OMEGA_NOP_FFMA) ? FFMA_C_COUNT : 1;
    bool ok = true;
    for (size_t l = 0; l < launches; l++) {
        const float *c = NULL;
        if (op == OMEGA_NOP_FFMA) { fill_c(g_c, FFMA_C[l]); c = g_c; }
        if (omega_numeric_reference(op, g_a, g_b, c, g_ref, N) != 0 ||
            omega_numeric_cpu_realize(op, g_a, g_b, c, g_cpu, N) != 0) {
            printf("OMEGA_NUMERIC_ORACLE_JSON:{\"op\":\"%s\",\"error\":\"tier refused\"}\n", info->name);
            ok = false;
            continue;
        }
        const float *tier[2] = { g_ref, g_cpu };
        static const char *const tname[2] = { "reference", "cpu" };
        for (int t = 0; t < 2; t++) {
            size_t bad = 0; long first = -1; uint32_t fw = 0, fg = 0;
            for (size_t i = 0; i < N; i++) {
                uint32_t want = oracle_value(op, i, c), got = U(tier[t][i]);
                if (!or_same(want, got)) {
                    if (!bad) { first = (long)i; fw = want; fg = got; }
                    bad++;
                }
            }
            print_oracle_json(info->name, tname[t], op == OMEGA_NOP_FFMA,
                              op == OMEGA_NOP_FFMA ? FFMA_C[l] : 0, N, bad, first, fw, fg);
            if (bad) ok = false;
        }
    }
    return ok;
}

/* EXP / LOG: ulp distance from the binary128 answer rounded once. NaN
 * results must match as a class; every other result must be within bound. */
static bool oracle_ulp(OmegaNumericOp op, uint32_t bound) {
    const OmegaNumericOpInfo *info = omega_numeric_op_at(op);
    if (omega_numeric_reference(op, g_a, NULL, NULL, g_ref, N) != 0) {
        printf("OMEGA_NUMERIC_ORACLE_JSON:{\"op\":\"%s\",\"error\":\"reference refused\"}\n", info->name);
        return false;
    }
    uint32_t max_norm = 0, max_sub = 0;
    size_t n_norm = 0, n_sub = 0, nan_bad = 0, over = 0;
    long first = -1; uint32_t fw = 0, fg = 0;
    for (size_t i = 0; i < N; i++) {
        uint32_t x = U(g_a[i]);
        uint32_t want = (op == OMEGA_NOP_EXP) ? or_exp(x) : or_log(x);
        uint32_t got = U(g_ref[i]);
        if (or_is_nan(want) || or_is_nan(got)) {
            if (!(or_is_nan(want) && or_is_nan(got))) {
                nan_bad++;
                if (first < 0) { first = (long)i; fw = want; fg = got; }
            }
            continue;
        }
        uint32_t d = or_ulp_distance(want, got);
        bool sub = (want & 0x7f800000u) == 0;   /* subnormal or zero result */
        if (sub) { n_sub++; if (d > max_sub) max_sub = d; }
        else     { n_norm++; if (d > max_norm) max_norm = d; }
        if (d > bound) {
            over++;
            if (first < 0) { first = (long)i; fw = want; fg = got; }
        }
    }
    printf("OMEGA_NUMERIC_ORACLE_JSON:{\"op\":\"%s\",\"oracle\":\"binary128_series_rounded_once\","
           "\"against\":\"reference\",\"compare\":\"ULP_BOUND\",\"bound_ulp\":%u,\"n\":%u,"
           "\"normal_or_inf_results\":%zu,\"max_ulp_normal_or_inf\":%u,"
           "\"subnormal_or_zero_results\":%zu,\"max_ulp_subnormal_or_zero\":%u,"
           "\"nan_class_mismatches\":%zu,\"over_bound\":%zu,",
           info->name, bound, N, n_norm, max_norm, n_sub, max_sub, nan_bad, over);
    if (first >= 0)
        printf("\"first_violation\":{\"index\":%ld,\"input\":\"0x%08x\",\"oracle\":\"0x%08x\",\"got\":\"0x%08x\"}}\n",
               first, U(g_a[first]), fw, fg);
    else
        printf("\"first_violation\":null}\n");
    return nan_bad == 0 && over == 0 && n_norm > 0 && n_sub > 0;
}

/* LDS_STS: a host has no shared memory, so check the declared permutation
 * itself on index-tagged data: out[i] carries tag (i ^ 63), and applying the
 * op twice gives the input back. */
static bool oracle_lds(void) {
    static float tag[N], once[N], twice[N], cpu[N];
    for (size_t i = 0; i < N; i++) tag[i] = F(0x3f800000u + (uint32_t)i);
    if (omega_numeric_reference(OMEGA_NOP_LDS_STS, tag, NULL, NULL, once, N) != 0 ||
        omega_numeric_reference(OMEGA_NOP_LDS_STS, once, NULL, NULL, twice, N) != 0 ||
        omega_numeric_cpu_realize(OMEGA_NOP_LDS_STS, tag, NULL, NULL, cpu, N) != 0) return false;
    size_t bad = 0, fixed = 0, not_inv = 0;
    for (size_t i = 0; i < N; i++) {
        size_t base = i & ~(size_t)63, want = base + (63 - (i & 63));
        if (U(once[i]) != 0x3f800000u + (uint32_t)want || U(cpu[i]) != U(once[i])) bad++;
        if (U(once[i]) == U(tag[i])) fixed++;
        if (U(twice[i]) != U(tag[i])) not_inv++;
    }
    printf("OMEGA_NUMERIC_ORACLE_JSON:{\"op\":\"LDS_STS\",\"oracle\":\"index_tagged_mirror\",\"n\":%u,"
           "\"wrong_source\":%zu,\"fixed_points\":%zu,\"not_involution\":%zu}\n", N, bad, fixed, not_inv);
    return bad == 0 && fixed == 0 && not_inv == 0;
}

static bool oracle_tier(void) {
    static const OmegaNumericOp EXACT[] = {
        OMEGA_NOP_FADD, OMEGA_NOP_FSUB, OMEGA_NOP_FMUL, OMEGA_NOP_FFMA, OMEGA_NOP_I2FP,
        OMEGA_NOP_DIV, OMEGA_NOP_SQRT, OMEGA_NOP_MUFU_RCP, OMEGA_NOP_MUFU_RSQ,
    };
    bool ok = true;
    for (size_t k = 0; k < sizeof(EXACT) / sizeof(EXACT[0]); k++)
        if (!oracle_exact(EXACT[k])) { ok = false; printf("    oracle mismatch: %s\n", omega_numeric_op_at(EXACT[k])->name); }
    if (!oracle_ulp(OMEGA_NOP_EXP, NUM_ORACLE_EXP_ULP)) { ok = false; printf("    oracle bound: EXP\n"); }
    if (!oracle_ulp(OMEGA_NOP_LOG, NUM_ORACLE_LOG_ULP)) { ok = false; printf("    oracle bound: LOG\n"); }
    if (!oracle_lds()) { ok = false; printf("    oracle: LDS_STS permutation\n"); }
    return ok;
}



/* ---- E1 scalar contract (docs/numeric/E1_SCALAR_CONTRACT.md) ------------------------
 * Three derivations per element: the integer reference (src), the CPU tier
 * (AArch64 instructions, src) and the second integer oracle (numeric_oracle.h).
 * Results are compared with omega_numeric_compare_equal under the op's mode. */

#define E1_FIRST OMEGA_NOP_FSETP_LT_SEL
#define E1_PRED_LAST OMEGA_NOP_FSETP_NEU_SEL
#define E1_BATCH 65536u
#ifndef E1_RANDOM_PER_OP
#define E1_RANDOM_PER_OP (1u << 22)
#endif

/* Predicate per FSETP_*_SEL op, in enum order; checked against the op name. */
static const char *const E1_PRED[] = { "LT", "LE", "GT", "EQ", "NE", "NUM", "NAN",
                                       "LTU", "LEU", "GTU", "GEU", "EQU", "NEU" };

static bool e1_is_pred(OmegaNumericOp op) { return op >= E1_FIRST && op <= E1_PRED_LAST; }

static bool e1_pred_table_ok(void) {
    if (sizeof(E1_PRED) / sizeof(E1_PRED[0]) != (size_t)(E1_PRED_LAST - E1_FIRST + 1)) return false;
    for (int op = E1_FIRST; op <= E1_PRED_LAST; op++) {
        char want[32];
        snprintf(want, sizeof(want), "FSETP_%s_SEL", E1_PRED[op - E1_FIRST]);
        if (strcmp(want, omega_numeric_op_at((size_t)op)->name) != 0) return false;
    }
    return true;
}

static uint32_t e1_oracle(OmegaNumericOp op, uint32_t a, uint32_t b, uint32_t c) {
    if (e1_is_pred(op)) return or_pred(E1_PRED[op - E1_FIRST], a, b) ? a : b;
    switch (op) {
    case OMEGA_NOP_F2I_FLOOR: return (uint32_t)(int32_t)or_to_int(a, OR_FLOOR, INT32_MIN, INT32_MAX);
    case OMEGA_NOP_F2I_CEIL:  return (uint32_t)(int32_t)or_to_int(a, OR_CEIL, INT32_MIN, INT32_MAX);
    case OMEGA_NOP_F2I_RNI:   return (uint32_t)(int32_t)or_to_int(a, OR_RNE, INT32_MIN, INT32_MAX);
    case OMEGA_NOP_F2U:       return (uint32_t)or_to_int(a, OR_RTZ, 0, UINT32_MAX);
    case OMEGA_NOP_I2FP_U32:  return or_u2f(a);
    case OMEGA_NOP_F32_TO_F16:  return or_f32_to_f16(a);
    case OMEGA_NOP_F32_TO_BF16: return or_f32_to_bf16(a);
    case OMEGA_NOP_F16_TO_F32:  return or_f16_to_f32(a & 0xffffu);
    case OMEGA_NOP_BF16_TO_F32: return or_bf16_to_f32(a & 0xffffu);
    case OMEGA_NOP_FFMA_V:      return or_fma(a, b, c);
    default: return OR_QNAN;
    }
}

typedef struct {
    size_t n, cpu_vs_ref, oracle_vs_ref, oracle_vs_cpu, refused;
    long first; uint32_t fa, fb, fc, fref, fcpu, forc;
} E1Count;

static float g_e1a[E1_BATCH], g_e1b[E1_BATCH], g_e1c[E1_BATCH], g_e1r[E1_BATCH], g_e1p[E1_BATCH];

/* Runs n (<= E1_BATCH) elements of g_e1a/b/c through all three derivations. */
static void e1_batch(OmegaNumericOp op, size_t n, E1Count *k) {
    const OmegaNumericOpInfo *info = omega_numeric_op_at(op);
    const float *b = info->arity >= 2 ? g_e1b : NULL, *c = info->arity >= 3 ? g_e1c : NULL;
    if (omega_numeric_reference(op, g_e1a, b, c, g_e1r, n) != 0 ||
        omega_numeric_cpu_realize(op, g_e1a, b, c, g_e1p, n) != 0) { k->refused++; return; }
    for (size_t i = 0; i < n; i++) {
        uint32_t ua = U(g_e1a[i]), ub = U(g_e1b[i]), uc = U(g_e1c[i]);
        uint32_t r = U(g_e1r[i]), p = U(g_e1p[i]), o = e1_oracle(op, ua, ub, uc);
        bool bad = false;
        if (!omega_numeric_compare_equal(info->compare, r, p)) { k->cpu_vs_ref++; bad = true; }
        if (!omega_numeric_compare_equal(info->compare, o, r)) { k->oracle_vs_ref++; bad = true; }
        if (!omega_numeric_compare_equal(info->compare, o, p)) { k->oracle_vs_cpu++; bad = true; }
        if (bad && k->first < 0) {
            k->first = (long)(k->n + i); k->fa = ua; k->fb = ub; k->fc = uc;
            k->fref = r; k->fcpu = p; k->forc = o;
        }
    }
    k->n += n;
}

static void e1_print(const char *what, OmegaNumericOp op, const E1Count *k) {
    printf("OMEGA_NUMERIC_E1_JSON:{\"op\":\"%s\",\"set\":\"%s\",\"n\":%zu,\"cpu_vs_ref\":%zu,"
           "\"oracle_vs_ref\":%zu,\"oracle_vs_cpu\":%zu,\"refused\":%zu,",
           omega_numeric_op_at(op)->name, what, k->n, k->cpu_vs_ref, k->oracle_vs_ref, k->oracle_vs_cpu, k->refused);
    if (k->first >= 0)
        printf("\"first\":{\"index\":%ld,\"a\":\"0x%08x\",\"b\":\"0x%08x\",\"c\":\"0x%08x\","
               "\"ref\":\"0x%08x\",\"cpu\":\"0x%08x\",\"oracle\":\"0x%08x\"}}\n",
               k->first, k->fa, k->fb, k->fc, k->fref, k->fcpu, k->forc);
    else
        printf("\"first\":null}\n");
}

/* Structured random FP32 pattern: half plain random bits, half aimed at the
 * places the E1 ops round, saturate or change class. */
static uint32_t e1_pattern(uint32_t *s) {
    uint32_t r = lcg_next(s), q = lcg_next(s), sign = q & OR_SIGN;
    switch (r & 15u) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
        return q ^ (lcg_next(s) >> 7);                          /* any bits */
    case 8:  return sign | ((118u + (r >> 4) % 44u) << 23) | (lcg_next(s) & 0x7fffffu); /* |x| ~ 2^-9 .. 2^34 */
    case 9:  return sign | ((96u + (r >> 4) % 48u) << 23) | (lcg_next(s) & 0x7fe000u) | 0x1000u; /* F16 ties */
    case 10: return (lcg_next(s) & 0xffff0000u) | 0x8000u;     /* BF16 ties */
    case 11: {                                                   /* k + 1/2: integer ties */
        int32_t k = (int32_t)(lcg_next(s) >> ((r >> 4) % 31u)) * ((q & 1u) ? -1 : 1);
        return U((float)k + 0.5f);
    }
    case 12: return sign | (lcg_next(s) & 0x00ffffffu);        /* subnormals and the smallest normals */
    case 13: return sign | ((254u - (r >> 4) % 4u) << 23) | (lcg_next(s) & 0x7fffffu); /* near max */
    case 14: return (r & 0x100u) ? (sign | 0x7f800000u | (lcg_next(s) & 0x7fffffu)) : (sign | 0x7f800000u); /* NaN / inf */
    default: return U((float)(lcg_next(s) >> ((r >> 4) % 32u)));   /* integers, exactly or rounded */
    }
}

/* Second and third operand, aimed at equality, sign and cancellation. */
static void e1_partners(uint32_t *s, uint32_t a, uint32_t *b, uint32_t *c) {
    uint32_t r = lcg_next(s);
    switch (r & 7u) {
    case 0: *b = a; break;
    case 1: *b = a ^ OR_SIGN; break;
    case 2: *b = a + 1u; break;
    case 3: *b = a - 1u; break;
    default: *b = e1_pattern(s); break;
    }
    switch ((r >> 3) & 3u) {
    case 0: *c = U(-(F(a) * F(*b))); break;                     /* near-total cancellation */
    case 1: *c = U(F(a) * F(*b)) ^ ((r >> 5) & 1u); break;           /* same sign, last-bit nudge */
    default: *c = e1_pattern(s); break;
    }
}

/* Random differential over every E1 op. Returns the two verdicts. */
static void e1_random(bool *cpu_ok, bool *oracle_ok) {
    *cpu_ok = *oracle_ok = true;
    for (int op = E1_FIRST; op < OMEGA_NOP_COUNT; op++) {
        E1Count k; memset(&k, 0, sizeof(k)); k.first = -1;
        uint32_t s = 0x9e3779b9u ^ (uint32_t)op * 0x85ebca6bu;
        for (size_t done = 0; done < E1_RANDOM_PER_OP; done += E1_BATCH) {
            for (size_t i = 0; i < E1_BATCH; i++) {
                uint32_t a = e1_pattern(&s), b, c;
                e1_partners(&s, a, &b, &c);
                g_e1a[i] = F(a); g_e1b[i] = F(b); g_e1c[i] = F(c);
            }
            e1_batch((OmegaNumericOp)op, E1_BATCH, &k);
        }
        e1_print("random", (OmegaNumericOp)op, &k);
        if (k.cpu_vs_ref || k.refused) *cpu_ok = false;
        if (k.oracle_vs_ref || k.oracle_vs_cpu || k.refused) *oracle_ok = false;
    }
    /* The widening ops read 16 bits: all 65536 patterns, with junk above. */
    for (int op = OMEGA_NOP_F16_TO_F32; op <= OMEGA_NOP_BF16_TO_F32; op++) {
        E1Count k; memset(&k, 0, sizeof(k)); k.first = -1;
        for (uint32_t h = 0; h < 65536u; h++) g_e1a[h] = F(h | ((h * 0x9e37u) << 16));
        e1_batch((OmegaNumericOp)op, 65536u, &k);
        e1_print("exhaustive16", (OmegaNumericOp)op, &k);
        if (k.cpu_vs_ref || k.refused) *cpu_ok = false;
        if (k.oracle_vs_ref || k.oracle_vs_cpu || k.refused) *oracle_ok = false;
    }
}

/* Hand-worked boundary values (expected bits written out, not computed). */
typedef struct { OmegaNumericOp op; uint32_t a, b, c, want; } E1Case;
static const E1Case E1_CASES[] = {
    { OMEGA_NOP_F2I_FLOOR, 0x80000001u, 0, 0, 0xffffffffu },  /* -min subnormal -> -1 */
    { OMEGA_NOP_F2I_FLOOR, 0xbfc00000u, 0, 0, 0xfffffffeu },  /* -1.5 -> -2 */
    { OMEGA_NOP_F2I_FLOOR, 0x4f000000u, 0, 0, 0x7fffffffu },  /* 2^31 saturates */
    { OMEGA_NOP_F2I_FLOOR, 0xcf000000u, 0, 0, 0x80000000u },  /* -2^31 exact */
    { OMEGA_NOP_F2I_FLOOR, 0xff800000u, 0, 0, 0x80000000u },  /* -inf */
    { OMEGA_NOP_F2I_FLOOR, 0x7fc00000u, 0, 0, 0x00000000u },  /* NaN -> 0 */
    { OMEGA_NOP_F2I_CEIL,  0x00000001u, 0, 0, 0x00000001u },  /* min subnormal -> 1 */
    { OMEGA_NOP_F2I_CEIL,  0xbf000000u, 0, 0, 0x00000000u },  /* -0.5 -> 0 */
    { OMEGA_NOP_F2I_CEIL,  0x3fc00000u, 0, 0, 0x00000002u },  /* 1.5 -> 2 */
    { OMEGA_NOP_F2I_RNI,   0x40200000u, 0, 0, 0x00000002u },  /* 2.5 -> 2 */
    { OMEGA_NOP_F2I_RNI,   0xc0200000u, 0, 0, 0xfffffffeu },  /* -2.5 -> -2 */
    { OMEGA_NOP_F2I_RNI,   0x3fc00000u, 0, 0, 0x00000002u },  /* 1.5 -> 2 */
    { OMEGA_NOP_F2I_RNI,   0x3f000000u, 0, 0, 0x00000000u },  /* 0.5 -> 0 */
    { OMEGA_NOP_F2I_RNI,   0x4affffffu, 0, 0, 0x00800000u },  /* 8388607.5 -> 8388608 */
    { OMEGA_NOP_F2U,       0xbf800000u, 0, 0, 0x00000000u },  /* -1 -> 0 */
    { OMEGA_NOP_F2U,       0x4f800000u, 0, 0, 0xffffffffu },  /* 2^32 saturates */
    { OMEGA_NOP_F2U,       0x4f7fffffu, 0, 0, 0xffffff00u },  /* largest float below 2^32 */
    { OMEGA_NOP_F2U,       0x4079999au, 0, 0, 0x00000003u },  /* 3.9 -> 3 */
    { OMEGA_NOP_F2U,       0xffc00000u, 0, 0, 0x00000000u },  /* NaN -> 0 */
    { OMEGA_NOP_I2FP_U32,  0x01000001u, 0, 0, 0x4b800000u },  /* 2^24+1: tie to even, down */
    { OMEGA_NOP_I2FP_U32,  0x01000003u, 0, 0, 0x4b800002u },  /* 2^24+3: tie to even, up */
    { OMEGA_NOP_I2FP_U32,  0xffffff80u, 0, 0, 0x4f800000u },  /* tie up to 2^32 */
    { OMEGA_NOP_I2FP_U32,  0x80000000u, 0, 0, 0x4f000000u },  /* unsigned, not -2^31 */
    { OMEGA_NOP_F32_TO_F16, 0x477ff000u, 0, 0, 0x7c00u },     /* 65520: tie, rounds to inf */
    { OMEGA_NOP_F32_TO_F16, 0x477fefffu, 0, 0, 0x7bffu },     /* just below: 65504 */
    { OMEGA_NOP_F32_TO_F16, 0x33000000u, 0, 0, 0x0000u },     /* 2^-25: tie to 0 */
    { OMEGA_NOP_F32_TO_F16, 0x33000001u, 0, 0, 0x0001u },     /* above the tie */
    { OMEGA_NOP_F32_TO_F16, 0x33c00000u, 0, 0, 0x0002u },     /* 1.5 * 2^-24: tie to even */
    { OMEGA_NOP_F32_TO_F16, 0x3f801000u, 0, 0, 0x3c00u },     /* 1 + half ulp: even, down */
    { OMEGA_NOP_F32_TO_F16, 0x3f803000u, 0, 0, 0x3c02u },     /* 1 + 1.5 ulp: even, up */
    { OMEGA_NOP_F32_TO_F16, 0x80000000u, 0, 0, 0x8000u },     /* -0 */
    { OMEGA_NOP_F32_TO_F16, 0x38800000u, 0, 0, 0x0400u },     /* 2^-14: smallest normal */
    { OMEGA_NOP_F32_TO_F16, 0x7fc00000u, 0, 0, 0x7e00u },     /* NaN stays NaN */
    { OMEGA_NOP_F32_TO_BF16, 0x3f808000u, 0, 0, 0x3f80u },    /* tie, even, down */
    { OMEGA_NOP_F32_TO_BF16, 0x3f818000u, 0, 0, 0x3f82u },    /* tie, even, up */
    { OMEGA_NOP_F32_TO_BF16, 0x7f7fffffu, 0, 0, 0x7f80u },    /* max float rounds to inf */
    { OMEGA_NOP_F32_TO_BF16, 0x00018000u, 0, 0, 0x0002u },    /* subnormal tie, kept */
    { OMEGA_NOP_F32_TO_BF16, 0xff800001u, 0, 0, 0xffc0u },    /* NaN stays NaN */
    { OMEGA_NOP_F16_TO_F32, 0x0001u, 0, 0, 0x33800000u },     /* 2^-24 */
    { OMEGA_NOP_F16_TO_F32, 0x03ffu, 0, 0, 0x387fc000u },     /* largest subnormal */
    { OMEGA_NOP_F16_TO_F32, 0x7bffu, 0, 0, 0x477fe000u },     /* 65504 */
    { OMEGA_NOP_F16_TO_F32, 0xfc00u, 0, 0, 0xff800000u },     /* -inf */
    { OMEGA_NOP_F16_TO_F32, 0xabcd3c00u, 0, 0, 0x3f800000u }, /* bits [31:16] ignored */
    { OMEGA_NOP_BF16_TO_F32, 0x0001u, 0, 0, 0x00010000u },    /* subnormal kept */
    { OMEGA_NOP_BF16_TO_F32, 0x7f80u, 0, 0, 0x7f800000u },    /* inf */
    { OMEGA_NOP_BF16_TO_F32, 0x12343f80u, 0, 0, 0x3f800000u },/* bits [31:16] ignored */
    { OMEGA_NOP_FFMA_V, 0x3f800800u, 0x3f800800u, 0xbf800000u, 0x3a000400u }, /* one rounding */
    { OMEGA_NOP_FFMA_V, 0x7f7fffffu, 0x40000000u, 0xff7fffffu, 0x7f7fffffu }, /* no overflow inside */
    { OMEGA_NOP_FFMA_V, 0x1a000000u, 0x1a000000u, 0x80000000u, 0x00000000u }, /* 2^-150 + -0 -> +0 */
    { OMEGA_NOP_FFMA_V, 0x0d800000u, 0x2b800000u, 0x00000000u, 0x00000200u }, /* subnormal 2^-140 */
    { OMEGA_NOP_FFMA_V, 0x7f800000u, 0x00000000u, 0x3f800000u, 0x7fc00000u }, /* inf * 0 -> NaN */
    { OMEGA_NOP_FFMA_V, 0x3f800800u, 0x3f800800u, 0x00000001u, 0x3f801001u }, /* 1+2^-11+2^-24 tie broken up by c = 2^-149 (sticky bit) */
    { OMEGA_NOP_FFMA_V, 0x3f800800u, 0x3f800800u, 0x80000001u, 0x3f801000u }, /* the same tie broken down by c = -2^-149 */
};

/* Predicate truth per relation class, bits L E G U (a<b, a==b, a>b, unordered). */
static const uint8_t E1_PRED_TRUTH[] = {
    0x8, 0xc, 0x2, 0x4, 0xa, 0xe, 0x1,      /* LT LE GT EQ NE NUM NAN */
    0x9, 0xd, 0x3, 0x7, 0x5, 0xb,           /* LTU LEU GTU GEU EQU NEU */
};
typedef struct { uint32_t a, b; uint8_t rel; } E1Pair;   /* rel: 8 L, 4 E, 2 G, 1 U */
static const E1Pair E1_PAIRS[] = {
    { 0x3f800000u, 0x40000000u, 8 }, { 0x40000000u, 0x3f800000u, 2 },
    { 0x80000000u, 0x00000000u, 4 }, { 0x00000000u, 0x80000000u, 4 },
    { 0x7fc00000u, 0x3f800000u, 1 }, { 0x3f800000u, 0xffc00001u, 1 },
    { 0x7f800001u, 0x7f800001u, 1 }, { 0xff800000u, 0x7f800000u, 8 },
    { 0x7f800000u, 0x7f800000u, 4 }, { 0x00000001u, 0x80000000u, 2 },
    { 0xc0000000u, 0xbf800000u, 8 }, { 0x80000001u, 0x00000001u, 8 },
};

static bool e1_boundary(void) {
    bool ok = e1_pred_table_ok();
    if (!ok) printf("    E1 predicate table does not match the op names\n");
    size_t cases = 0, bad = 0;
    float a[1], b[1], c[1], r[1], p[1];
    for (size_t t = 0; t < sizeof(E1_CASES) / sizeof(E1_CASES[0]) +
                           (sizeof(E1_PRED_TRUTH)) * (sizeof(E1_PAIRS) / sizeof(E1_PAIRS[0])); t++) {
        OmegaNumericOp op; uint32_t ua, ub = 0, uc = 0, want;
        if (t < sizeof(E1_CASES) / sizeof(E1_CASES[0])) {
            const E1Case *e = &E1_CASES[t];
            op = e->op; ua = e->a; ub = e->b; uc = e->c; want = e->want;
        } else {
            size_t k = t - sizeof(E1_CASES) / sizeof(E1_CASES[0]);
            size_t np = sizeof(E1_PAIRS) / sizeof(E1_PAIRS[0]);
            size_t pi = k / np; const E1Pair *pr = &E1_PAIRS[k % np];
            op = (OmegaNumericOp)(E1_FIRST + (int)pi);
            ua = pr->a; ub = pr->b;
            want = (E1_PRED_TRUTH[pi] & pr->rel) ? ua : ub;
        }
        const OmegaNumericOpInfo *info = omega_numeric_op_at(op);
        a[0] = F(ua); b[0] = F(ub); c[0] = F(uc);
        int rr = omega_numeric_reference(op, a, info->arity >= 2 ? b : NULL, info->arity >= 3 ? c : NULL, r, 1);
        int pr = omega_numeric_cpu_realize(op, a, info->arity >= 2 ? b : NULL, info->arity >= 3 ? c : NULL, p, 1);
        uint32_t o = e1_oracle(op, ua, ub, uc);
        cases++;
        if (rr || pr || !omega_numeric_compare_equal(info->compare, want, U(r[0])) ||
            !omega_numeric_compare_equal(info->compare, want, U(p[0])) ||
            !omega_numeric_compare_equal(info->compare, want, o)) {
            bad++;
            printf("    E1 boundary %s a=0x%08x b=0x%08x c=0x%08x want 0x%08x ref 0x%08x cpu 0x%08x oracle 0x%08x\n",
                   info->name, ua, ub, uc, want, U(r[0]), U(p[0]), o);
        }
    }
    printf("OMEGA_NUMERIC_E1_JSON:{\"set\":\"boundary\",\"cases\":%zu,\"bad\":%zu}\n", cases, bad);
    return ok && bad == 0;
}

/* --e1-exhaustive: every 2^32 input pattern of each unary E1 op through all
 * three derivations, one child process per op. Not part of the Gate 5 IDs. */
static int e1_exhaustive(void) {
    static const OmegaNumericOp OPS[] = { OMEGA_NOP_F2I_FLOOR, OMEGA_NOP_F2I_CEIL, OMEGA_NOP_F2I_RNI,
                                          OMEGA_NOP_F2U, OMEGA_NOP_I2FP_U32, OMEGA_NOP_F32_TO_F16,
                                          OMEGA_NOP_F32_TO_BF16, OMEGA_NOP_F16_TO_F32, OMEGA_NOP_BF16_TO_F32 };
    const size_t nops = sizeof(OPS) / sizeof(OPS[0]);
    pid_t pids[sizeof(OPS) / sizeof(OPS[0])];
    fflush(stdout);
    for (size_t j = 0; j < nops; j++) {
        pids[j] = fork();
        if (pids[j] < 0) { perror("fork"); return 1; }
        if (pids[j] == 0) {
            E1Count k; memset(&k, 0, sizeof(k)); k.first = -1;
            for (uint64_t base = 0; base < (1ull << 32); base += E1_BATCH) {
                for (uint32_t i = 0; i < E1_BATCH; i++) g_e1a[i] = F((uint32_t)(base + i));
                e1_batch(OPS[j], E1_BATCH, &k);
            }
            e1_print("exhaustive32", OPS[j], &k);
            fflush(stdout);
            _exit((k.cpu_vs_ref || k.oracle_vs_ref || k.oracle_vs_cpu || k.refused || k.n != (1ull << 32)) ? 1 : 0);
        }
    }
    int fails = 0;
    for (size_t j = 0; j < nops; j++) {
        int st = 0;
        if (waitpid(pids[j], &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) fails++;
    }
    printf("E1 exhaustive verdict: %s (%zu ops x 2^32 inputs, %d failed)\n", fails ? "FAIL" : "PASS", nops, fails);
    return fails ? 1 : 0;
}
int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--e1-exhaustive") == 0) return e1_exhaustive();
    if (argc > 1) { fprintf(stderr, "usage: %s [--e1-exhaustive]\n", argv[0]); return 2; }
#ifdef OMEGA_NUMERIC_CPU_ONLY
    const bool gb10 = false;
#else
    const bool gb10 = true;
#endif
    printf("============================================================\n");
    printf("        GATE 5: OMEGA-NUMERIC-0 QUALIFICATION SUITE         \n");
    printf("        mode: %s\n", gb10 ? "GB10 + CPU tiers" : "CPU tiers only (no device)");
    printf("============================================================\n\n");

#ifdef OMEGA_NUMERIC_CPU_ONLY
    printf("OMEGA_NUMERIC_HWDESC_JSON:{\"source\":\"FAKE_NON_HARDWARE_CPU_ONLY\",\"fake\":true,"
           "\"descriptor_digest\":\"0000000000000000000000000000000000000000000000000000000000000000\"}\n");
#else
    int hw_rc = print_hw_descriptor();
#endif

    print_run_line();
    build_corpus();

    /* The host tiers need round to nearest even with FZ (and DN, AH, FIZ, NEP,
     * FZ16) clear; otherwise both tiers can agree on the same wrong result. */
    uint64_t fpcr = omega_numeric_read_fpcr();
    printf("[*] FPCR 0x%016llx, required-clear mask 0x%016llx\n", (unsigned long long)fpcr,
           (unsigned long long)OMEGA_NUMERIC_FPCR_REQUIRED_CLEAR);
    report("FPCR_RNE_NO_FTZ_REQUIRED",
           (fpcr & OMEGA_NUMERIC_FPCR_REQUIRED_CLEAR) == 0 && omega_numeric_fpenv_ok());

    /* ---- Encoding and provenance ------------------------------------------------ */
    int fixtures_rc = omega_blackwell_verify_codegen_fixtures();
    int prov_problems = omega_numeric_verify_all_fixtures();
    size_t encoded = 0;
    int build_ok = 1;
    printf("[*] Op registry:\n");
    for (size_t i = 0; i < omega_numeric_op_count(); i++) {
        const OmegaNumericOpInfo *info = omega_numeric_op_at(i);
        printf("    %-11s %-10s %-10s %s\n", info->name, info->gb10_encoded ? "encoded" : "REFUSED",
               omega_numeric_compare_name(info->compare), info->reference);
        printf("OMEGA_NUMERIC_REGISTRY_JSON:{\"op\":\"%s\",\"encoded\":%s,\"compare\":\"%s\",\"launches\":%zu}\n",
               info->name, info->gb10_encoded ? "true" : "false", omega_numeric_compare_name(info->compare),
               info->op == OMEGA_NOP_FFMA ? (size_t)FFMA_C_COUNT : (size_t)1);
        if (!info->gb10_encoded) continue;
        encoded++;
        uint8_t code[0x200];
        size_t len = 0;
        if (omega_numeric_build_kernel(info->op, code, sizeof(code), &len) != 0) build_ok = 0;
    }
    printf("[*] Provenance entries: %zu, encoded ops: %zu\n", omega_numeric_get_opcode_count(), encoded);
    report("PROVENANCE_MATCHES_EXECUTOR",
           fixtures_rc == 0 && prov_problems == 0 && build_ok && encoded == 38 &&
           omega_numeric_get_opcode_count() == 52);

    /* Refusal before submission: never a silent wrong instruction. */
    int refuse_ok = 1;
    size_t refused_ops = 0;
    for (size_t oi = 0; oi < omega_numeric_op_count(); oi++) {
        const OmegaNumericOpInfo *ninfo = omega_numeric_op_at(oi);
        if (ninfo->gb10_encoded) continue;
        const char *nname = ninfo->name;
        const float *nc = ninfo->arity >= 3 ? g_c : NULL;
        char err[256];
        refused_ops++;
        int rc = omega_numeric_submit_check(nname, g_a, g_b, nc, g_dev, N, err, sizeof(err));
#ifndef OMEGA_NUMERIC_CPU_ONLY
        int xrc = omega_gb10_execute_simt_op(nname, g_a, g_b, nc, g_dev, N);
        if (xrc != OMEGA_NUMERIC_ERR_NOT_ENCODED) refuse_ok = 0;
#endif
        if (rc != OMEGA_NUMERIC_ERR_NOT_ENCODED || err[0] == '\0') refuse_ok = 0;
        OmegaNumericPatchInsn p[OMEGA_NUMERIC_PATCH_MAX];
        if (omega_numeric_patch_words(ninfo->op, p) != OMEGA_NUMERIC_ERR_NOT_ENCODED)
            refuse_ok = 0;
        printf("    refused %-14s rc=%d: %s\n", nname, rc, err);
    }
    /* only DIV SQRT EXP LOG remain refused (the 23 E1 scalar ops are encoded, E1 WP-C) */
    if (refused_ops != omega_numeric_op_count() - 38) refuse_ok = 0;
    {
        char err[256];
        fill_c(g_c, 0x3f800000u);
        g_c[7] = 2.0f;
        if (omega_numeric_submit_check("FFMA", g_a, g_b, g_c, g_dev, N, err, sizeof(err)) != OMEGA_NUMERIC_ERR_OPERANDS)
            refuse_ok = 0;
        printf("    refused FFMA non-uniform c: %s\n", err);
        if (omega_numeric_submit_check("SHFL_DOWN", g_a, NULL, NULL, g_dev, 33, err, sizeof(err)) != OMEGA_NUMERIC_ERR_OPERANDS)
            refuse_ok = 0;
        printf("    refused SHFL_DOWN count 33: %s\n", err);
        if (omega_numeric_submit_check("FADD", g_a, NULL, NULL, g_dev, N, err, sizeof(err)) != OMEGA_NUMERIC_ERR_BAD_ARGS)
            refuse_ok = 0;
    }
    {
        /* Variants of the shared-memory and warp ops that have no encoding:
         * refused by name, never mapped onto the 32-bit or SHFL.DOWN kernel. */
        size_t nv = omega_numeric_refused_variant_count();
        for (size_t v = 0; v < nv; v++) {
            char err[256];
            const char *name = omega_numeric_refused_variant_at(v);
            int rc = omega_numeric_submit_check(name, g_a, g_b, NULL, g_dev, N, err, sizeof(err));
#ifndef OMEGA_NUMERIC_CPU_ONLY
            if (omega_gb10_execute_simt_op(name, g_a, g_b, NULL, g_dev, N) != OMEGA_NUMERIC_ERR_NOT_ENCODED) refuse_ok = 0;
#endif
            if (rc != OMEGA_NUMERIC_ERR_NOT_ENCODED || !strstr(err, name)) refuse_ok = 0;
        }
        printf("    refused %zu unencoded variants (LDS.U8 ... REDUCE_SUM_BLOCK) by name\n", nv);
        if (nv < 12 || omega_numeric_refused_variant_at(nv) != NULL) refuse_ok = 0;
    }
    report("NOT_ENCODED_OPS_REFUSED_BEFORE_SUBMISSION", refuse_ok);

    /* Shapes the shared-memory and reduction kernels cannot carry, refused
     * before submission with the reason named. */
    {
        struct { const char *op; size_t count; const char *why; } SHAPES[] = {
            { "LDS_STS", 32, "CTA size 64" },     /* half a CTA: threads 32..63 never reach BAR.SYNC */
            { "LDS_STS", 96, "CTA size 64" },
            { "LDS_STS", 4097, "CTA size 64" },
            { "REDUCE_SUM", 33, "multiple of 32" },
            { "REDUCE_SUM", 4080, "multiple of 32" },
            { "SHFL_DOWN", 33, "multiple of 32" },
        };
        int shapes_ok = 1;
        for (size_t k = 0; k < sizeof(SHAPES) / sizeof(SHAPES[0]); k++) {
            char err[256];
            int rc = omega_numeric_submit_check(SHAPES[k].op, g_a, NULL, NULL, g_dev, SHAPES[k].count, err, sizeof(err));
            if (rc != OMEGA_NUMERIC_ERR_OPERANDS || !strstr(err, SHAPES[k].why)) shapes_ok = 0;
            printf("    refused %-10s count %-5zu rc=%d: %s\n", SHAPES[k].op, SHAPES[k].count, rc, err);
        }
        char err[256];
        if (omega_numeric_submit_check("LDS_STS", g_a, NULL, NULL, g_dev, N, err, sizeof(err)) != 0 ||
            omega_numeric_submit_check("REDUCE_SUM", g_a, NULL, NULL, g_dev, N, err, sizeof(err)) != 0 ||
            omega_numeric_submit_check("LDS_STS", g_a, NULL, NULL, g_dev, 64, err, sizeof(err)) != 0 ||
            omega_numeric_submit_check("REDUCE_SUM", g_a, NULL, NULL, g_dev, 32, err, sizeof(err)) != 0) {
            printf("    a valid shape was refused: %s\n", err);
            shapes_ok = 0;
        }
        report("NEG_BAD_SHARED_AND_WARP_SHAPES_REFUSED", shapes_ok);
    }

    /* Structural patch check: every encoded op passes on the launch QMD, and
     * one corruption per check is refused with that check's reason. */
    {
        int clean_ok = 1;
        uint32_t qmd[OMEGA_BW_QMD_WORDS];
        OmegaBlackwellQmdConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.num_elements = N;
        omega_numeric_launch_shape(N, &cfg.threads_per_block, &cfg.grid_width);
        omega_blackwell_build_qmd1(qmd, &cfg);
        for (size_t i = 0; i < omega_numeric_op_count(); i++) {
            const OmegaNumericOpInfo *info = omega_numeric_op_at(i);
            if (!info->gb10_encoded) continue;
            OmegaNumericPatchInsn p[OMEGA_NUMERIC_PATCH_MAX];
            int n = omega_numeric_patch_words(info->op, p);
            char err[256];
            if (omega_numeric_check_patch(info->op, p, n, qmd, err, sizeof(err)) != 0) {
                clean_ok = 0;
                printf("    clean %s refused: %s\n", info->name, err);
            }
        }
        int caught = 0, cases = 0;
        /* 32..43: E1 scalar ops (E1 WP-C) */
        static const OmegaNumericOp E1_NEG_OP[12] = {
            OMEGA_NOP_FSETP_LT_SEL, OMEGA_NOP_FSETP_LT_SEL, OMEGA_NOP_FSETP_EQ_SEL, OMEGA_NOP_F2I_FLOOR,
            OMEGA_NOP_F32_TO_F16, OMEGA_NOP_F2I_FLOOR, OMEGA_NOP_I2FP_U32, OMEGA_NOP_FFMA_V,
            OMEGA_NOP_FFMA_V, OMEGA_NOP_FFMA_V, OMEGA_NOP_FFMA_V, OMEGA_NOP_FFMA_V };
        for (int m = 0; m < 44; m++) {
            OmegaNumericOp op = OMEGA_NOP_LDS_STS;
            if (m >= 18 && m != 27 && m != 31) op = OMEGA_NOP_REDUCE_SUM;
            if (m == 17) op = OMEGA_NOP_SHFL_DOWN;
            if (m >= 32) op = E1_NEG_OP[m - 32];
            OmegaNumericPatchInsn p[OMEGA_NUMERIC_PATCH_MAX];
            int n = omega_numeric_patch_words(op, p);
            uint32_t q[OMEGA_BW_QMD_WORDS];
            memcpy(q, qmd, sizeof(q));
            const char *want = "";
            const char *what = "";
            OmegaNumericOp check_op = op;
            /* LDS_STS: 0 SHF, 1 LOP3, 2 STS, 3 BAR, 4 LDS, 5 STG, 6 EXIT
             * REDUCE: 2s SHFL, 2s+1 FADD (s = 0..4), 10 STG, 11 EXIT */
            switch (m) {
            case 0:  what = "empty patch"; n = 0; want = "patch length"; break;
            case 1:  what = "STG after LDS does not wait"; p[5].w[3] = 0x000fe200u; want = "does not wait on it"; break;
            case 2:  what = "STS word changed (not the 32-bit form)"; p[2].w[2] ^= 0x00000200u; want = "STS at"; break;
            case 3:  what = "LDS with an immediate offset"; p[4].w[1] = 0x000100ffu; want = "LDS at"; break;
            case 4:  what = "barrier other than BAR.SYNC 0"; p[3].w[1] = 1u; want = "barrier at"; break;
            case 5:  what = "FADD words submitted as LDS_STS"; check_op = OMEGA_NOP_LDS_STS;
                     n = omega_numeric_patch_words(OMEGA_NOP_FADD, p); want = "no STS/LDS pair"; break;
            case 6:  what = "BAR removed"; memmove(&p[3], &p[4], 3 * sizeof(p[0])); n = 6; want = "expected one STS"; break;
            case 7:  what = "BAR before STS"; { OmegaNumericPatchInsn t = p[2]; p[2] = p[3]; p[3] = t; } want = "order must be"; break;
            case 8:  what = "QMD declares no barrier"; q[35] &= ~(0x1fu << 17); want = "barriers"; break;
            case 9:  what = "QMD CTA width 32"; q[34] = (q[34] & ~0xffffu) | 32u; want = "CTA width"; break;
            case 10: what = "QMD shared size 128 bytes"; q[36] = (q[36] & ~0x7ffu) | 1u; want = "bytes of shared memory"; break;
            case 11: what = "address shift 3 (tid * 8)"; p[0].w[1] = 3u; want = "SHF.L.U32"; break;
            case 12: what = "STS address from R10"; p[2].w[0] = 0x0a007988u; want = "STS address register"; break;
            case 13: what = "LOP3 AND instead of XOR"; p[1].w[2] = 0x078ec0ffu; want = "LOP3 XOR"; break;
            case 14: what = "partner mask 0x1fc (out of bounds)"; p[1].w[1] = 0x1fcu; want = "(CTA-1)*4"; break;
            case 15: what = "LDS into R8"; p[4].w[0] = 0x0a087984u; want = "LDS must load"; break;
            case 16: what = "SHFL mode bits changed"; check_op = OMEGA_NOP_REDUCE_SUM;
                     n = omega_numeric_patch_words(OMEGA_NOP_REDUCE_SUM, p); p[0].w[2] ^= 0x1u; want = "SHFL.DOWN form"; break;
            case 17: what = "SHFL_DOWN by 2"; p[0].w[1] = 0x08401f00u; want = "SHFL.DOWN by 1"; break;
            case 18: what = "last pair dropped"; p[8] = p[10]; p[9] = p[11]; n = 10; want = "five SHFL.DOWN"; break;
            case 19: what = "deltas 16 and 8 swapped"; { uint32_t t = p[0].w[1]; p[0].w[1] = p[2].w[1]; p[2].w[1] = t; }
                     want = "declared order"; break;
            case 20: what = "clamp 0x0f"; p[4].w[1] = (p[4].w[1] & ~(0x1fu << 8)) | (0x0fu << 8); want = "clamp"; break;
            case 21: what = "shuffle of R5"; p[2].w[0] = 0x05097f89u; want = "shuffles R"; break;
            case 22: what = "FADD negates"; p[3].w[1] |= 0x80000000u; want = "plain FADD"; break;
            case 23: what = "shuffle sets no barrier"; p[6].w[3] |= (7u << 14); want = "does not wait on the shuffle"; break;
            case 24: what = "FADD adds R5"; p[5].w[1] = 5u; want = "does not add the shuffled"; break;
            case 25: what = "FADD writes R3"; p[1].w[0] = 0x02037221u; want = "writes R3"; break;
            case 26: what = "STG replaced by EXIT"; p[10] = p[11]; want = "is not stored"; break;
            case 27: what = "STS stores R5 instead of a[i] in R2"; p[2].w[1] = 5u; want = "not the input value R2"; break;
            case 28: what = "sum stored through address R8"; p[10].w[0] = 0x08007986u; want = "STG.E desc[UR4][R6.64]"; break;
            case 29: what = "sum store predicated on P0"; p[10].w[0] = 0x06000986u; want = "STG.E desc[UR4][R6.64]"; break;
            case 30: what = "sum store width changed"; p[10].w[2] ^= 0x00000200u; want = "STG.E desc[UR4][R6.64]"; break;
            case 31: what = "LDS_STS result stored through address R8"; p[5].w[0] = 0x08007986u; want = "STG.E desc[UR4][R6.64]"; break;
            case 32: what = "FSETP_LT_SEL with the GT compare code"; p[0].w[2] = 0x03f04000u; want = "FSETP compare code"; break;
            case 33: what = "FSETP compares a with R6"; p[0].w[1] = 6u; want = "patch is not FSETP"; break;
            case 34: what = "FSEL operands swapped"; p[1].w[0] = 0x05097208u; p[1].w[1] = 2u; want = "select is not FSEL"; break;
            case 35: what = "F2I_FLOOR submitted with CEIL rounding"; p[0].w[2] = 0x0020b100u; want = "conversion at 0"; break;
            case 36: what = "F32_TO_F16 submitted as F2F.BF16"; p[0].w[2] = 0x00202000u; want = "conversion at 0"; break;
            case 37: what = "F2I_FLOOR result not scoreboarded"; p[0].w[3] = 0x010fca00u; want = "must be a scoreboarded"; break;
            case 38: what = "I2FP_U32 with an extra STG, EXIT"; p[1] = p[0]; p[1].w[0] = 0x06007986u; p[1].w[1] = 9u; p[1].w[2] = 0x0c101904u;
                     p[1].w[3] = 0x000fe200u; p[2].w[0] = 0x0000794du; p[2].w[1] = 0; p[2].w[2] = 0x03800000u; p[2].w[3] = 0x000fea00u;
                     n = 3; want = "must be fixed latency"; break;
            case 39: what = "FFMA_V without the c load"; p[2] = p[3]; p[3] = p[4]; p[4] = p[5]; n = 5; want = "expected LDC.64"; break;
            case 40: what = "FFMA_V c pointer from c[0x0][0x3a8]"; p[0].w[1] = 0x0000ea00u; want = "c pointer is not"; break;
            case 41: what = "FFMA_V c address indexed by R2"; p[1].w[0] = 0x020a7825u; want = "c address is not"; break;
            case 42: what = "FFMA_V c loaded from R12"; p[2].w[0] = 0x0c0b7981u; want = "c[i] is not LDG"; break;
            case 43: what = "FFMA_V adds R1 instead of c[i]"; p[3].w[2] = 0x00000001u; want = "FFMA is not"; break;
            }
            char err[256];
            int rc = omega_numeric_check_patch(check_op, p, n, q, err, sizeof(err));
            bool ok = rc == OMEGA_NUMERIC_ERR_OPERANDS && strstr(err, want) != NULL;
            cases++;
            if (ok) caught++;
            else printf("    NOT CAUGHT (%s): rc=%d reason '%s', wanted '%s'\n", what, rc, err, want);
        }
        {   /* FFMA_V reads c per element: a submission without c is refused. */
            char e3[256];
            int rc3 = omega_numeric_submit_check("FFMA_V", g_a, g_b, NULL, g_dev, N, e3, sizeof(e3));
            cases++;
            if (rc3 == OMEGA_NUMERIC_ERR_BAD_ARGS && strstr(e3, "third input")) caught++;
            else printf("    NOT CAUGHT (FFMA_V without c): rc=%d reason '%s'\n", rc3, e3);
        }
        printf("    patch corruptions refused with the right reason: %d of %d; clean patches accepted: %s\n",
               caught, cases, clean_ok ? "yes" : "no");
        report("NEG_PATCH_STRUCTURE_CHECKED_BEFORE_SUBMISSION", caught == cases && clean_ok);
    }

    /* Gate item: the spec's vocabulary includes shared-memory load/store and a
     * warp reduction primitive. Both are encoded, keyed to provenance, and
     * pass the structural check on the launch QMD for the corpus size. Parity
     * on silicon is the GB10 tier's job, not this one. */
    {
        bool lds = omega_numeric_op_find("LDS_STS")->gb10_encoded;
        bool red = omega_numeric_op_find("REDUCE_SUM")->gb10_encoded;
        char e1[256], e2[256];
        bool lds_sub = omega_numeric_submit_check("LDS_STS", g_a, NULL, NULL, g_dev, N, e1, sizeof(e1)) == 0;
        bool red_sub = omega_numeric_submit_check("REDUCE_SUM", g_a, NULL, NULL, g_dev, N, e2, sizeof(e2)) == 0;
        if (!lds || !red || !lds_sub || !red_sub)
            printf("    FP32_SIMT_OPCODES_ENCODED: LDS_STS encoded %d accepted %d (%s); REDUCE_SUM encoded %d accepted %d (%s)\n",
                   lds, lds_sub, e1, red, red_sub, e2);
        report("FP32_SIMT_OPCODES_ENCODED",
               fixtures_rc == 0 && prov_problems == 0 && build_ok && lds && red && lds_sub && red_sub);
    }

    /* ---- Math sequences --------------------------------------------------------- */
    {
        int seq_ok = 1;
        OmegaParityTrace t;
        size_t div_bad = 0, sqrt_bad = 0;
        if (omega_numeric_reference(OMEGA_NOP_DIV, g_a, g_b, NULL, g_ref, N) == 0 &&
            omega_numeric_cpu_realize(OMEGA_NOP_DIV, g_a, g_b, NULL, g_cpu, N) == 0) {
            if (omega_numeric_parity(OMEGA_NOP_DIV, g_ref, g_cpu, N, &t) != 0) { seq_ok = 0; t.mismatches = 1; }
            ClassTrace ct; class_trace(OMEGA_NOP_DIV, g_a, g_ref, g_cpu, N, &ct);
            print_parity_json("DIV", "cpu", "BIT_EXACT", 0, false, N, &t, &ct);
            div_bad = t.mismatches;
        } else seq_ok = 0;
        if (omega_numeric_reference(OMEGA_NOP_SQRT, g_a, NULL, NULL, g_ref, N) == 0 &&
            omega_numeric_cpu_realize(OMEGA_NOP_SQRT, g_a, NULL, NULL, g_cpu, N) == 0) {
            if (omega_numeric_parity(OMEGA_NOP_SQRT, g_ref, g_cpu, N, &t) != 0) { seq_ok = 0; t.mismatches = 1; }
            ClassTrace ct; class_trace(OMEGA_NOP_SQRT, g_a, g_ref, g_cpu, N, &ct);
            print_parity_json("SQRT", "cpu", "BIT_EXACT", 0, false, N, &t, &ct);
            sqrt_bad = t.mismatches;
        } else seq_ok = 0;
        /* Named hard cases the review found (old Newton sequences failed each):
         * 1/3 was 1 ULP low, max/min-subnormal gave NaN, sqrt(min normal) was
         * 1 ULP low. Plus ties and results in the subnormal range. */
        static const uint32_t DIV_HARD[][2] = {
            { 0x3f800000u, 0x40400000u }, { 0x7f7fffffu, 0x00000001u }, { 0xff7fffffu, 0x00000001u },
            { 0x00000001u, 0x7f7fffffu }, { 0x00800000u, 0x40000000u }, { 0x00000003u, 0x40000000u },
            { 0x00000001u, 0x40000000u }, { 0x00000001u, 0x3f7fffffu }, { 0x007fffffu, 0x3f800001u },
            { 0x7f7fffffu, 0x3f7fffffu }, { 0x3f7fffffu, 0x7f7fffffu }, { 0x00400000u, 0x00000003u },
        };
        static const uint32_t SQRT_HARD[] = {
            0x00800000u, 0x00000001u, 0x00000002u, 0x007fffffu, 0x7f7fffffu, 0x3f7fffffu, 0x40000000u,
        };
        size_t hard_bad = 0;
        for (size_t k = 0; k < sizeof(DIV_HARD) / sizeof(DIV_HARD[0]); k++) {
            float x = F(DIV_HARD[k][0]), y = F(DIV_HARD[k][1]);
            float want = omega_ieee_div(x, y), got = omega_math_div(x, y);
            if (!omega_numeric_bits_equal(want, got)) {
                hard_bad++;
                printf("    DIV 0x%08x / 0x%08x: want 0x%08x got 0x%08x\n", U(x), U(y), U(want), U(got));
            }
        }
        for (size_t k = 0; k < sizeof(SQRT_HARD) / sizeof(SQRT_HARD[0]); k++) {
            float x = F(SQRT_HARD[k]);
            float want = omega_ieee_sqrt(x), got = omega_math_sqrt(x);
            if (!omega_numeric_bits_equal(want, got)) {
                hard_bad++;
                printf("    SQRT 0x%08x: want 0x%08x got 0x%08x\n", U(x), U(want), U(got));
            }
        }
        printf("OMEGA_NUMERIC_FINDING_JSON:{\"id\":\"DIV_SQRT_CORRECT_ROUNDING\",\"corpus\":%u,"
               "\"div_mismatches\":%zu,\"sqrt_mismatches\":%zu,\"hard_case_mismatches\":%zu}\n",
               N, div_bad, sqrt_bad, hard_bad);
        if (hard_bad) seq_ok = 0;
        if (div_bad || sqrt_bad) seq_ok = 0;

        /* exp / log: the Omega sequences are the definition; check edge
         * behavior and self-consistency (no libm available as an oracle). */
        int el_ok = 1;
        float ln2 = 0.693147182f;
        if (U(omega_math_exp(0.0f)) != 0x3f800000u) el_ok = 0;
        if (U(omega_math_exp(F(0xff800000u))) != 0) el_ok = 0;
        if (U(omega_math_exp(F(0x7f800000u))) != 0x7f800000u) el_ok = 0;
        if (!omega_isnan(omega_math_exp(F(0x7fc00000u)))) el_ok = 0;
        if (!omega_isinf(omega_math_exp(100.0f))) el_ok = 0;
        float e887 = omega_math_exp(88.7f); /* k = 128: needs the split scale */
        if (omega_isinf(e887) || omega_isnan(e887) || e887 < 3.0e38f) el_ok = 0;
        float em100 = omega_math_exp(-100.0f), em50 = omega_math_exp(-50.0f);
        if (!omega_issubnormal(em100)) el_ok = 0;
        if (omega_fabs(em100 - em50 * em50) > 1e-5f * em50 * em50 + F(0x00000002u)) el_ok = 0;
        for (int k = -120; k <= 120; k += 8) {
            float x = (float)k * ln2;
            float e = omega_math_exp(x);
            float want = F((uint32_t)(k + 127) << 23);
            /* x itself is rounded (half an ulp of k ln2), which alone moves exp by up to
             * |x| 2^-24 relative; allow that plus a few ulps. */
            if (omega_fabs(e - want) > (omega_fabs(x) * 6e-8f + 1e-6f) * want) el_ok = 0;
        }
        if (U(omega_math_log(1.0f)) != 0 && U(omega_math_log(1.0f)) != 0x80000000u) el_ok = 0;
        if (U(omega_math_log(0.0f)) != 0xff800000u) el_ok = 0;
        if (!omega_isnan(omega_math_log(-1.0f))) el_ok = 0;
        if (U(omega_math_log(F(0x7f800000u))) != 0x7f800000u) el_ok = 0;
        float lmin = omega_math_log(F(0x00000001u)); /* -149 ln2 = -103.27893 */
        if (omega_fabs(lmin - (-103.278929903f)) > 2e-5f) el_ok = 0;
        float lsub = omega_math_log(F(0x00400000u)); /* 2^-127 */
        if (omega_fabs(lsub - (-127.0f * ln2)) > 2e-5f) el_ok = 0;
        for (int k = -100; k <= 100; k += 5) {
            float x = (float)k * 0.37f;
            float r = omega_math_log(omega_math_exp(x));
            if (omega_fabs(r - x) > 2e-6f * (omega_fabs(x) + 1.0f)) el_ok = 0;
        }
        printf("[*] exp/log edge and consistency checks: %s\n", el_ok ? "ok" : "FAILED");
        if (!el_ok) seq_ok = 0;
        report("OMEGA_MATH_SEQUENCES_QUALIFIED", seq_ok);
    }

    /* ---- Declared reduction order ------------------------------------------------ */
    {
        int ord_ok = 1;
        if (omega_numeric_reference(OMEGA_NOP_REDUCE_SUM, g_a, NULL, NULL, g_ref, N) != 0 ||
            omega_numeric_cpu_realize(OMEGA_NOP_REDUCE_SUM, g_a, NULL, NULL, g_cpu, N) != 0) {
            ord_ok = 0;
        } else {
            OmegaParityTrace t;
            if (omega_numeric_parity(OMEGA_NOP_REDUCE_SUM, g_ref, g_cpu, N, &t) != 0) ord_ok = 0;
            print_parity_json("REDUCE_SUM", "cpu", "BIT_EXACT", 0, false, N, &t, NULL);
            if (t.mismatches || t.checked != N / 32) ord_ok = 0;
        }
        printf("[*] Declared Reduction Order: %s\n", OMEGA_WARP_REDUCTION_DECLARED_ORDER);
        report("WARP_REDUCTION_ORDER_DECLARED", ord_ok);
    }

    /* ---- CPU tier against the reference, every op ---------------------------------- */
    int cpu_ok = 1, cpu_sub_ok = 1;
    for (size_t i = 0; i < omega_numeric_op_count(); i++) {
        OmegaNumericOp op = omega_numeric_op_at(i)->op;
        if (op == OMEGA_NOP_DIV || op == OMEGA_NOP_SQRT || op == OMEGA_NOP_REDUCE_SUM) continue; /* above */
        TierResult tr = cpu_tier(op);
        if (!tr.ok) { cpu_ok = 0; printf("    CPU tier mismatch: %s\n", omega_numeric_op_at(i)->name); }
        if (ftz_sensitive(op) && !tr.subnormals_seen) cpu_sub_ok = 0;
    }
    report("CPU_TIER_EQUALS_REFERENCE", cpu_ok);
    report("CPU_TIER_SUBNORMALS_PRESERVED", cpu_ok && cpu_sub_ok);
    printf("\n[*] Independent oracle: integer soft-float, binary128 EXP/LOG, tagged LDS_STS\n");
    report("CPU_TIER_INDEPENDENT_ORACLE", oracle_tier());

    /* ---- E1 scalar contract: boundary values, random differential, oracle ------------ */
    printf("\n[*] E1 scalar contract: %u random inputs per op, 23 ops, three derivations\n", (unsigned)E1_RANDOM_PER_OP);
    report("E1_SCALAR_BOUNDARY_VALUES", e1_boundary());
    bool e1_cpu_ok = false, e1_oracle_ok = false;
    e1_random(&e1_cpu_ok, &e1_oracle_ok);
    report("E1_SCALAR_CPU_EQUALS_REFERENCE", e1_cpu_ok);
    report("E1_SCALAR_INDEPENDENT_ORACLE", e1_oracle_ok);

    /* ---- GB10 tier ----------------------------------------------------------------- */
    if (!gb10) {
        skip("CPU_GB10_BIT_PARITY", "needs GB10");
        skip("SUBNORMALS_PRESERVED_NO_FTZ", "needs GB10");
        skip("MUFU_SEED_ONLY_NOT_COMPARED", "needs GB10");
        skip("EDGE_CLASS_BEHAVIOR_VERIFIED", "needs GB10");
        skip("HARDWARE_DESCRIPTOR_PROBED", "needs GB10");
    }
#ifndef OMEGA_NUMERIC_CPU_ONLY
    else {
        printf("\n[*] GB10 tier: %u elements per launch\n", N);
        int bit_ok = 1, sub_ok = 1, mufu_ok = 1, class_ok_all = 1;
        for (size_t i = 0; i < omega_numeric_op_count(); i++) {
            const OmegaNumericOpInfo *info = omega_numeric_op_at(i);
            if (!info->gb10_encoded) continue;
            bool class_ok = true;
            TierResult tr = gb10_tier(info->op, &class_ok);
            if (info->compare == OMEGA_CMP_SEED_BOUND) {
                if (!tr.ok) mufu_ok = 0;
                continue;
            }
            if (!tr.ok) bit_ok = 0;
            if (!class_ok) class_ok_all = 0;
            if (ftz_sensitive(info->op) && (!tr.ok || !tr.subnormals_seen)) sub_ok = 0;
        }
        OmegaParityTrace t;
        mufu_ok = mufu_ok &&
                  omega_numeric_parity(OMEGA_NOP_MUFU_RCP, g_ref, g_dev, N, &t) == OMEGA_NUMERIC_ERR_OPERANDS &&
                  omega_numeric_parity(OMEGA_NOP_MUFU_RSQ, g_ref, g_dev, N, &t) == OMEGA_NUMERIC_ERR_OPERANDS;
        report("CPU_GB10_BIT_PARITY", bit_ok && cpu_ok);
        report("SUBNORMALS_PRESERVED_NO_FTZ", sub_ok && cpu_sub_ok);
        report("MUFU_SEED_ONLY_NOT_COMPARED", mufu_ok);
        report("EDGE_CLASS_BEHAVIOR_VERIFIED", class_ok_all && bit_ok);
        report("HARDWARE_DESCRIPTOR_PROBED", hw_rc == 0);
    }
#endif

    /* ---- Negative tests: each must catch a broken property ------------------------- */
    printf("\n[*] Negative tests\n");
    {
        /* FTZ model through the same comparator must mismatch for every FTZ-sensitive op */
        int ftz_ok = 1;
        for (size_t i = 0; i < omega_numeric_op_count(); i++) {
            OmegaNumericOp op = omega_numeric_op_at(i)->op;
            if (!ftz_sensitive(op)) continue;
            const float *c = NULL;
            if (op == OMEGA_NOP_FFMA) { fill_c(g_c, 0x00800000u); c = g_c; }
            OmegaParityTrace t;
            if (omega_numeric_reference(op, g_a, g_b, c, g_ref, N) != 0 ||
                omega_numeric_reference_ftz(op, g_a, g_b, c, g_cpu, N) != 0 ||
                omega_numeric_parity(op, g_ref, g_cpu, N, &t) != 0 || t.mismatches == 0) {
                ftz_ok = 0;
            }
            printf("    FTZ model vs reference, %-9s: %zu mismatches\n", omega_numeric_op_at(i)->name, t.mismatches);
        }
        report("NEG_FTZ_DETECTED_AND_REJECTED", ftz_ok);
    }
    {
        /* A sequential sum disagrees with the declared tree on some warp */
        float w[32];
        w[0] = 16777216.0f;
        for (int i = 1; i < 32; i++) w[i] = 1.0f;
        float seq = 0.0f;
        for (int i = 0; i < 32; i++) seq += w[i];
        float tree = omega_warp_reduce_sum(w);
        printf("    sequential %.1f vs declared tree %.1f\n", seq, tree);
        report("NEG_UNORDERED_REDUCTION_DIVERGENCE_CAUGHT", U(seq) != U(tree));
    }
    {
        /* Raw seed must fail bit parity against the IEEE reciprocal, and the
         * comparator must refuse any bit comparison of a MUFU op. */
        size_t bad = 0, n = 0;
        for (size_t i = 3456; i < N; i++) {
            float y = g_a[i];
            n++;
            if (!omega_numeric_bits_equal(omega_numeric_host_rcp_seed(y), omega_ieee_div(1.0f, y))) bad++;
        }
        OmegaParityTrace t;
        int refused = omega_numeric_parity(OMEGA_NOP_MUFU_RCP, g_ref, g_cpu, N, &t) == OMEGA_NUMERIC_ERR_OPERANDS &&
                      omega_numeric_parity(OMEGA_NOP_MUFU_RSQ, g_ref, g_cpu, N, &t) == OMEGA_NUMERIC_ERR_OPERANDS;
        printf("    raw seed vs IEEE reciprocal: %zu of %zu differ; MUFU bit compare refused: %s\n",
               bad, n, refused ? "yes" : "no");
        report("NEG_RAW_MUFU_APPROX_REJECTED_WITHOUT_REFINEMENT", bad > 0 && refused);
    }
    {
        char err[256];
        int rc = omega_numeric_submit_check("FDIV_TYPO", g_a, g_b, NULL, g_dev, N, err, sizeof(err));
        int ok = rc == OMEGA_NUMERIC_ERR_BAD_ARGS && omega_numeric_op_find("FDIV_TYPO") == NULL;
#ifndef OMEGA_NUMERIC_CPU_ONLY
        ok = ok && omega_gb10_execute_simt_op("FDIV_TYPO", g_a, g_b, NULL, g_dev, N) == OMEGA_NUMERIC_ERR_BAD_ARGS;
#endif
        report("NEG_UNKNOWN_OPCODE_FAILS_CLOSED", ok);
    }
    {
        /* Corrupted provenance copies must each be rejected */
        size_t n = omega_numeric_get_opcode_count();
        OmegaOpcodeProvenance copy[32];
        int caught = 0, cases = 0;
        for (int m = 0; m < 5; m++) {
            for (size_t i = 0; i < n; i++) copy[i] = *omega_numeric_get_opcode(i);
            size_t cn = n;
            for (size_t i = 0; i < n; i++) {
                if (m == 0 && strcmp(copy[i].key, "FSETP_GE_R2_R5") == 0) copy[i].fixture_w2 = 0x03f0e000u; /* GEU */
                if (m == 1 && strcmp(copy[i].key, "FADD") == 0) copy[i].opcode = 0;
                if (m == 2 && strcmp(copy[i].key, "FSEL_R2_R5_P0") == 0) copy[i].fixture_w2 = 0x04000000u; /* !P0 */
                if (m == 3 && strcmp(copy[i].key, "MUFU_RCP") == 0) copy[i].fixture_w1 = 0x00000005u;
            }
            if (m == 4) {
                copy[cn] = copy[0];
                copy[cn].key = "DIV";
                copy[cn].mnemonic = "DIV (claimed, never submitted)";
                cn++;
            }
            cases++;
            if (omega_numeric_verify_fixture_table(copy, cn, false) > 0) caught++;
        }
        for (size_t i = 0; i < n; i++) copy[i] = *omega_numeric_get_opcode(i);
        int clean = omega_numeric_verify_fixture_table(copy, n, false) == 0;
        printf("    corrupted provenance copies rejected: %d of %d; clean copy accepted: %s\n",
               caught, cases, clean ? "yes" : "no");
        report("NEG_OPCODE_PROVENANCE_INTEGRITY_VERIFIED", caught == cases && clean);
    }
    {
        /* A non-default FP environment must be refused by every host tier, not
         * used silently: under round-up, 1 + 2^-24 is the next float above 1
         * in both the reference and the CPU tier, and they would agree. */
        uint64_t saved = omega_numeric_read_fpcr();
        static const uint64_t BAD_ENV[] = { 1ULL << 22 /* RMode RP */, 3ULL << 22 /* RMode RZ */,
                                            1ULL << 24 /* FZ */ };
        static const char *BAD_NAME[] = { "round-up", "round-to-zero", "flush-to-zero" };
        int cases_ok = 0;
        for (size_t k = 0; k < 3; k++) {
            uint64_t v = (saved & ~OMEGA_NUMERIC_FPCR_REQUIRED_CLEAR) | BAD_ENV[k];
            __asm__ volatile("msr fpcr, %0" : : "r"(v) : "memory");
            volatile float one = 1.0f, tiny = F(0x33800000u), half = F(0x33000000u), sub = F(0x00000001u);
            volatile float up = one + tiny, down = one - half, flushed = sub * one;
            bool env_ok = omega_numeric_fpenv_ok();
            int r_ref = omega_numeric_reference(OMEGA_NOP_FADD, g_a, g_b, NULL, g_ref, N);
            int r_cpu = omega_numeric_cpu_realize(OMEGA_NOP_FADD, g_a, g_b, NULL, g_cpu, N);
            int r_ftz = omega_numeric_reference_ftz(OMEGA_NOP_FADD, g_a, g_b, NULL, g_cpu, N);
            OmegaParityTrace t;
            int r_seed = omega_numeric_seed_bound(OMEGA_NOP_MUFU_RCP, g_a, g_cpu, N, &t);
            __asm__ volatile("msr fpcr, %0" : : "r"(saved) : "memory");
            /* the environment really changed (else the probe proves nothing) */
            bool changed = (k == 0) ? U(up) == 0x3f800001u
                         : (k == 1) ? U(down) == 0x3f7fffffu : U(flushed) == 0u;
            bool refused = !env_ok && r_ref == OMEGA_NUMERIC_ERR_FPENV && r_cpu == OMEGA_NUMERIC_ERR_FPENV &&
                           r_ftz == OMEGA_NUMERIC_ERR_FPENV && r_seed == OMEGA_NUMERIC_ERR_FPENV;
            printf("    FPCR %-13s: environment changed %s, tiers refused %s (rc %d %d %d %d)\n", BAD_NAME[k],
                   changed ? "yes" : "no", refused ? "yes" : "no", r_ref, r_cpu, r_ftz, r_seed);
            if (changed && refused) cases_ok++;
        }
        bool restored = omega_numeric_read_fpcr() == saved && omega_numeric_fpenv_ok();
        report("NEG_NONDEFAULT_FPCR_REFUSED", cases_ok == 3 && restored);
    }
    {
        /* Comparator sanity: one flipped low bit, and NaN vs number, are caught */
        int ref_rc = omega_numeric_reference(OMEGA_NOP_FADD, g_a, g_b, NULL, g_ref, N);
        memcpy(g_cpu, g_ref, sizeof(g_cpu));
        g_cpu[3500] = F(U(g_cpu[3500]) ^ 1u);
        g_cpu[3600] = F(0x7fc00000u);
        OmegaParityTrace t;
        int par_rc = omega_numeric_parity(OMEGA_NOP_FADD, g_ref, g_cpu, N, &t);
        report("NEG_COMPARATOR_CATCHES_ONE_BIT", ref_rc == 0 && par_rc == 0 &&
               t.mismatches == 2 && t.first_index == 3500);
    }

    printf("\nGate 5 Results: TOTAL=%d PASSED=%d FAILED=%d SKIPPED=%d\n", g_total, g_passed, g_failed, g_skipped);
    /*
     * Exit status. 0: nothing failed and every SKIP is accounted for.
     * 1: a real failure (a FAIL, or a SKIP that is not allowed here).
     * CPU-only build: the SKIPs must be exactly the CHIP_ONLY_IDS (the known
     * gap a host cannot close). Chip build: no SKIP is allowed at all.
     */
    size_t chip_only_seen = 0;
    for (size_t k = 0; k < CHIP_ONLY_COUNT; k++) chip_only_seen += g_chip_only_skipped[k];
#ifdef OMEGA_NUMERIC_CPU_ONLY
    bool gap_ok = g_undeclared_skips == 0 && chip_only_seen == CHIP_ONLY_COUNT;
    const char *build = "cpu-only";
#else
    bool gap_ok = g_skipped == 0;
    const char *build = "chip";
#endif
    const char *verdict = g_failed ? "HOST_REGRESSION" : !gap_ok ? "UNDECLARED_SKIP"
                        : g_skipped ? "PASS_EXCEPT_DECLARED_CHIP_ONLY" : "PASS";
    printf("Gate 5 Verdict: %s (build %s, declared chip-only skips %zu of %zu, undeclared skips %d)\n",
           verdict, build, chip_only_seen, CHIP_ONLY_COUNT, g_undeclared_skips);
    return (g_failed == 0 && gap_ok) ? 0 : 1;
}
