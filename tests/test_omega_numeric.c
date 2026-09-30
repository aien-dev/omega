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
 */
#include "omega_numeric.h"
#include "omega_numeric_provenance.h"
#include "omega_blackwell_codegen.h"

#ifndef OMEGA_NUMERIC_CPU_ONLY
#include "forge_descriptor.h"
#endif

#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_total = 0, g_passed = 0, g_failed = 0, g_skipped = 0;

static void report(const char *name, int ok) {
    g_total++;
    if (ok) { g_passed++; printf("[PASS] %s\n", name); }
    else    { g_failed++; printf("[FAIL] %s\n", name); }
}

static void skip(const char *name, const char *why) {
    g_skipped++;
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
        bool same = info->compare == OMEGA_CMP_INT_EXACT ? U(expect[i]) == U(got[i])
                                                         : omega_numeric_bits_equal(expect[i], got[i]);
        if (!same) ct->bad[c]++;
    }
}

static void print_parity_json(const char *op, const char *tier, const char *compare, uint32_t c_bits,
                              bool has_c, size_t n, const OmegaParityTrace *t, const ClassTrace *ct) {
    printf("OMEGA_NUMERIC_PARITY_JSON:{\"op\":\"%s\",\"tier\":\"%s\",\"compare\":\"%s\",", op, tier, compare);
    if (has_c) printf("\"c_bits\":\"0x%08x\",", c_bits);
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
            print_parity_json(info->name, "cpu", "BIT_EXACT_IEEE_TARGET", 0, false, N, &t, NULL);
        } else {
            omega_numeric_parity(op, g_ref, g_cpu, N, &t);
            ClassTrace ct;
            class_trace(op, g_a, g_ref, g_cpu, N, &ct);
            print_parity_json(info->name, "cpu", omega_numeric_compare_name(info->compare),
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
        omega_numeric_reference(op, g_a, g_b, c, g_ref, N);
        int rc = omega_gb10_execute_simt_op(info->name, g_a, info->arity >= 2 ? g_b : NULL, c, g_dev, N);
        if (rc != 0) {
            printf("OMEGA_NUMERIC_PARITY_JSON:{\"op\":\"%s\",\"tier\":\"gb10\",\"error\":%d}\n", info->name, rc);
            tr.ok = false;
            *class_ok = false;
            continue;
        }
        OmegaParityTrace t;
        if (info->compare == OMEGA_CMP_SEED_BOUND) {
            omega_numeric_seed_bound(op, g_a, g_dev, N, &t);
            print_bound_json(info->name, "gb10", N, &t);
            if (t.out_of_bound || t.checked == 0) tr.ok = false;
            continue;
        }
        omega_numeric_parity(op, g_ref, g_dev, N, &t);
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

int main(void) {
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
           fixtures_rc == 0 && prov_problems == 0 && build_ok && encoded == 13 &&
           omega_numeric_get_opcode_count() == 15);

    /* Refusal before submission: never a silent wrong instruction. */
    int refuse_ok = 1;
    static const char *NOT_ENCODED[] = { "DIV", "SQRT", "EXP", "LOG", "REDUCE_SUM", "LDS", "STS", "LDS_STS" };
    for (size_t i = 0; i < sizeof(NOT_ENCODED) / sizeof(NOT_ENCODED[0]); i++) {
        char err[256];
        int rc = omega_numeric_submit_check(NOT_ENCODED[i], g_a, g_b, NULL, g_dev, N, err, sizeof(err));
#ifndef OMEGA_NUMERIC_CPU_ONLY
        int xrc = omega_gb10_execute_simt_op(NOT_ENCODED[i], g_a, g_b, NULL, g_dev, N);
        if (xrc != OMEGA_NUMERIC_ERR_NOT_ENCODED) refuse_ok = 0;
#endif
        if (rc != OMEGA_NUMERIC_ERR_NOT_ENCODED || err[0] == '\0') refuse_ok = 0;
        OmegaNumericPatchInsn p[OMEGA_NUMERIC_PATCH_MAX];
        if (omega_numeric_patch_words(omega_numeric_op_find(NOT_ENCODED[i])->op, p) != OMEGA_NUMERIC_ERR_NOT_ENCODED)
            refuse_ok = 0;
        printf("    refused %-10s rc=%d: %s\n", NOT_ENCODED[i], rc, err);
    }
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
    report("NOT_ENCODED_OPS_REFUSED_BEFORE_SUBMISSION", refuse_ok);

    /* Gate item: the spec's vocabulary includes shared-memory load/store and a
     * warp reduction primitive. Neither has a GB10 kernel yet. */
    {
        bool lds = omega_numeric_op_find("LDS_STS")->gb10_encoded;
        bool red = omega_numeric_op_find("REDUCE_SUM")->gb10_encoded;
        printf("OMEGA_NUMERIC_FINDING_JSON:{\"id\":\"VOCABULARY_INCOMPLETE\",\"shared_load_store_encoded\":%s,"
               "\"warp_reduction_encoded\":%s}\n", lds ? "true" : "false", red ? "true" : "false");
        report("FP32_SIMT_OPCODES_ENCODED",
               fixtures_rc == 0 && prov_problems == 0 && build_ok && lds && red);
    }

    /* ---- Math sequences --------------------------------------------------------- */
    {
        int seq_ok = 1;
        OmegaParityTrace t;
        size_t div_bad = 0, sqrt_bad = 0;
        if (omega_numeric_reference(OMEGA_NOP_DIV, g_a, g_b, NULL, g_ref, N) == 0 &&
            omega_numeric_cpu_realize(OMEGA_NOP_DIV, g_a, g_b, NULL, g_cpu, N) == 0) {
            omega_numeric_parity(OMEGA_NOP_DIV, g_ref, g_cpu, N, &t);
            ClassTrace ct; class_trace(OMEGA_NOP_DIV, g_a, g_ref, g_cpu, N, &ct);
            print_parity_json("DIV", "cpu", "BIT_EXACT", 0, false, N, &t, &ct);
            div_bad = t.mismatches;
        } else seq_ok = 0;
        if (omega_numeric_reference(OMEGA_NOP_SQRT, g_a, NULL, NULL, g_ref, N) == 0 &&
            omega_numeric_cpu_realize(OMEGA_NOP_SQRT, g_a, NULL, NULL, g_cpu, N) == 0) {
            omega_numeric_parity(OMEGA_NOP_SQRT, g_ref, g_cpu, N, &t);
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
            omega_numeric_parity(OMEGA_NOP_REDUCE_SUM, g_ref, g_cpu, N, &t);
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

    /* ---- GB10 tier ----------------------------------------------------------------- */
    if (!gb10) {
        skip("CPU_GB10_BIT_PARITY", "needs GB10");
        skip("SUBNORMALS_PRESERVED_NO_FTZ", "needs GB10");
        skip("MUFU_SEED_ONLY_NOT_COMPARED", "needs GB10");
        skip("EDGE_CLASS_BEHAVIOR_VERIFIED", "needs GB10");
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
        omega_numeric_reference(OMEGA_NOP_FADD, g_a, g_b, NULL, g_ref, N);
        memcpy(g_cpu, g_ref, sizeof(g_cpu));
        g_cpu[3500] = F(U(g_cpu[3500]) ^ 1u);
        g_cpu[3600] = F(0x7fc00000u);
        OmegaParityTrace t;
        omega_numeric_parity(OMEGA_NOP_FADD, g_ref, g_cpu, N, &t);
        report("NEG_COMPARATOR_CATCHES_ONE_BIT", t.mismatches == 2 && t.first_index == 3500);
    }

    printf("\nGate 5 Results: TOTAL=%d PASSED=%d FAILED=%d SKIPPED=%d\n", g_total, g_passed, g_failed, g_skipped);
    return g_failed == 0 ? 0 : 1;
}
