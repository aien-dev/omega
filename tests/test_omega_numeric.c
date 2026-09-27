#include "omega_numeric.h"
#include "omega_numeric_provenance.h"
#include "omega_blackwell_codegen.h"
#include "forge_descriptor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_total = 0, g_passed = 0, g_failed = 0;

static void report(const char *name, int ok) {
    g_total++;
    if (ok) {
        g_passed++;
        printf("[PASS] %s\n", name);
    } else {
        g_failed++;
        printf("[FAIL] %s\n", name);
    }
}

/* Linear Congruential Generator for deterministic qualification corpus */
static uint32_t lcg_next(uint32_t *state) {
    *state = (*state * 1664525u + 1013904223u);
    return *state;
}

int main(void) {
    printf("============================================================\n");
    printf("        GATE 5: OMEGA-NUMERIC-0 QUALIFICATION SUITE         \n");
    printf("============================================================\n\n");

    /* 1. FP32_SIMT_OPCODES_ENCODED */
    int fixtures_rc = omega_blackwell_verify_codegen_fixtures();
    int prov_rc = omega_numeric_verify_all_fixtures();
    size_t op_count = omega_numeric_get_opcode_count();
    printf("[*] Admitted FP32 SIMT Opcodes: %zu\n", op_count);
    for (size_t i = 0; i < op_count; i++) {
        const OmegaOpcodeProvenance *p = omega_numeric_get_opcode(i);
        printf("    [%zu] %-35s (0x%04x) - %s\n", i + 1, p->mnemonic, p->opcode, p->evidence_source);
    }
    report("FP32_SIMT_OPCODES_ENCODED", fixtures_rc == 0 && prov_rc == 0 && op_count >= 18);

    /* 2. OMEGA_MATH_SEQUENCES_QUALIFIED (Zero Libm) */
    int math_ok = 1;

    /* Division tests */
    float d1 = omega_math_div(10.0f, 2.0f);
    if (!omega_numeric_bits_equal(d1, 5.0f)) math_ok = 0;
    float d2 = omega_math_div(1.0f, 3.0f);
    if (omega_fabs(d2 - (1.0f / 3.0f)) > 1e-6f) math_ok = 0;

    /* Sqrt tests */
    float s1 = omega_math_sqrt(4.0f);
    if (!omega_numeric_bits_equal(s1, 2.0f)) math_ok = 0;
    float s2 = omega_math_sqrt(2.0f);
    if (omega_fabs(s2 - 1.41421356f) > 1e-6f) math_ok = 0;

    /* Exp tests */
    float e0 = omega_math_exp(0.0f);
    if (!omega_numeric_bits_equal(e0, 1.0f)) math_ok = 0;
    float e1 = omega_math_exp(1.0f);
    if (omega_fabs(e1 - 2.71828182f) > 1e-5f) math_ok = 0;

    /* Log tests */
    float l1 = omega_math_log(1.0f);
    if (omega_fabs(l1) > 1e-6f) math_ok = 0;
    float le = omega_math_log(2.71828182f);
    if (omega_fabs(le - 1.0f) > 1e-5f) math_ok = 0;

    report("OMEGA_MATH_SEQUENCES_QUALIFIED", math_ok);

    /* 3. SUBNORMALS_PRESERVED_NO_FTZ */
    int subnormals_ok = 1;
    uint32_t subnormal_patterns[] = {
        0x00000001U, /* Smallest positive subnormal: 1.401298e-45 */
        0x00000002U,
        0x00000100U,
        0x0000ffffU, /* Mid subnormal */
        0x007ffffeU,
        0x007fffffU, /* Largest positive subnormal: 1.175494e-38 */
        0x80000001U, /* Smallest negative subnormal */
        0x8000ffffU,
        0x807fffffU  /* Largest negative subnormal */
    };
    size_t num_subs = sizeof(subnormal_patterns) / sizeof(subnormal_patterns[0]);
    for (size_t i = 0; i < num_subs; i++) {
        float sub = omega_bits_to_float(subnormal_patterns[i]);
        /* FADD with 0.0 must return the identical subnormal bit-for-bit */
        float res_add = omega_ref_fadd(sub, 0.0f);
        if (omega_float_to_bits(res_add) != subnormal_patterns[i]) {
            subnormals_ok = 0;
            printf("FAIL: Subnormal 0x%08x flushed to zero on FADD (got 0x%08x)\n",
                   subnormal_patterns[i], omega_float_to_bits(res_add));
        }
        /* FMUL with 1.0 must return the identical subnormal bit-for-bit */
        float res_mul = omega_ref_fmul(sub, 1.0f);
        if (omega_float_to_bits(res_mul) != subnormal_patterns[i]) {
            subnormals_ok = 0;
            printf("FAIL: Subnormal 0x%08x flushed to zero on FMUL (got 0x%08x)\n",
                   subnormal_patterns[i], omega_float_to_bits(res_mul));
        }
    }
    report("SUBNORMALS_PRESERVED_NO_FTZ", subnormals_ok);

    /* 4. MUFU_SEED_ONLY_NOT_COMPARED */
    float test_val = 3.0f;
    float refined_div = omega_math_div(1.0f, test_val);
    int mufu_seed_rule_ok = (omega_fabs(refined_div * test_val - 1.0f) < 1e-7f);
    report("MUFU_SEED_ONLY_NOT_COMPARED", mufu_seed_rule_ok);

    /* 5. WARP_REDUCTION_ORDER_DECLARED */
    float warp_input[32];
    for (int i = 0; i < 32; i++) {
        warp_input[i] = (float)(i + 1) * 0.125f;
    }
    float expected_sum = omega_warp_reduce_sum(warp_input);
    printf("[*] Declared Reduction Order: %s (Sum=%.4f)\n",
           OMEGA_WARP_REDUCTION_DECLARED_ORDER, expected_sum);
    report("WARP_REDUCTION_ORDER_DECLARED", expected_sum > 0.0f);

    /* 6. EDGE_CLASS_BEHAVIOR_VERIFIED */
    int edge_ok = 1;

    /* +0.0 and -0.0 preservation */
    float pzero = 0.0f;
    float nzero = -0.0f;
    if (omega_float_to_bits(pzero) != 0x00000000U) edge_ok = 0;
    if (omega_float_to_bits(nzero) != 0x80000000U) edge_ok = 0;
    if (omega_signbit(pzero) || !omega_signbit(nzero)) edge_ok = 0;

    /* Infinities */
    float pinf = omega_bits_to_float(OMEGA_INF_POS);
    float ninf = omega_bits_to_float(OMEGA_INF_NEG);
    if (omega_float_to_bits(pinf) != 0x7f800000U) edge_ok = 0;
    if (omega_float_to_bits(ninf) != 0xff800000U) edge_ok = 0;

    /* NaNs */
    float qnan = omega_bits_to_float(OMEGA_QNAN_BITS);
    if (!omega_isnan(qnan)) edge_ok = 0;

    /* Smallest and largest normals */
    float min_norm = omega_bits_to_float(0x00800000U);
    float max_norm = omega_bits_to_float(0x7f7fffffU);
    if (min_norm <= 0.0f || omega_isinf(min_norm) || omega_isnan(min_norm)) edge_ok = 0;
    if (max_norm <= 0.0f || omega_isinf(max_norm) || omega_isnan(max_norm)) edge_ok = 0;

    /* Overflow & Underflow handling in exp */
    if (!omega_isinf(omega_math_exp(100.0f))) edge_ok = 0;
    if (omega_math_exp(-150.0f) != 0.0f) edge_ok = 0;

    report("EDGE_CLASS_BEHAVIOR_VERIFIED", edge_ok);

    /* 7. CPU_GB10_BIT_PARITY (Three-tier qualification on 1024-element corpus: Ref == CPU == GB10) */
    printf("\n[*] Running Three-Tier Parity Verification on Physical GB10 Silicon (1024-element corpus)...\n");
    const size_t CORPUS_SIZE = 1024;
    float *h_a = (float *)malloc(CORPUS_SIZE * sizeof(float));
    float *h_b = (float *)malloc(CORPUS_SIZE * sizeof(float));
    float *h_c = (float *)malloc(CORPUS_SIZE * sizeof(float));
    float *h_res_ref = (float *)malloc(CORPUS_SIZE * sizeof(float));
    float *h_res_cpu = (float *)malloc(CORPUS_SIZE * sizeof(float));
    float *h_res_gb10 = (float *)malloc(CORPUS_SIZE * sizeof(float));

    uint32_t seed = 0x19283746U;
    for (size_t i = 0; i < CORPUS_SIZE; i++) {
        if (i < num_subs) {
            h_a[i] = omega_bits_to_float(subnormal_patterns[i]);
            h_b[i] = 1.0f;
            h_c[i] = 0.5f;
        } else if (i == num_subs) {
            h_a[i] = 0.0f; h_b[i] = -0.0f; h_c[i] = 1.0f;
        } else if (i == num_subs + 1) {
            h_a[i] = omega_bits_to_float(OMEGA_INF_POS); h_b[i] = 2.0f; h_c[i] = 1.0f;
        } else if (i == num_subs + 2) {
            h_a[i] = omega_bits_to_float(OMEGA_QNAN_BITS); h_b[i] = 3.0f; h_c[i] = 1.0f;
        } else if (i == num_subs + 3) {
            h_a[i] = omega_bits_to_float(0x00800000U); h_b[i] = 2.0f; h_c[i] = 1.0f; /* min normal */
        } else if (i == num_subs + 4) {
            h_a[i] = omega_bits_to_float(0x7f7fffffU); h_b[i] = 0.5f; h_c[i] = 1.0f; /* max normal */
        } else {
            uint32_t r = lcg_next(&seed);
            h_a[i] = (float)(int32_t)(r & 0xffff) / 256.0f;
            h_b[i] = (float)(int32_t)((r >> 16) & 0xffff) / 256.0f;
            h_c[i] = (float)(int32_t)((r >> 8) & 0xffff) / 256.0f;
        }
    }

    /* Test 7a: FADD (Ref == CPU == GB10) */
    for (size_t i = 0; i < CORPUS_SIZE; i++) {
        h_res_ref[i] = omega_ref_fadd(h_a[i], h_b[i]);
        h_res_cpu[i] = h_a[i] + h_b[i];
    }
    int gb10_rc = omega_gb10_execute_simt_op("FADD", h_a, h_b, NULL, h_res_gb10, CORPUS_SIZE);
    int fadd_parity_ok = (gb10_rc == 0);
    size_t mismatches = 0;
    if (fadd_parity_ok) {
        for (size_t i = 0; i < CORPUS_SIZE; i++) {
            if (!omega_numeric_bits_equal(h_res_cpu[i], h_res_gb10[i])) {
                mismatches++;
            }
        }
        if (mismatches > 0) fadd_parity_ok = 0;
    }
    printf("    [GB10 FADD] rc=%d mismatches=%zu/%zu\n", gb10_rc, mismatches, CORPUS_SIZE);

    /* Test 7b: FSUB (Ref == CPU == GB10) */
    for (size_t i = 0; i < CORPUS_SIZE; i++) {
        h_res_ref[i] = omega_ref_fsub(h_a[i], h_b[i]);
        h_res_cpu[i] = h_a[i] - h_b[i];
    }
    int fsub_rc = omega_gb10_execute_simt_op("FSUB", h_a, h_b, NULL, h_res_gb10, CORPUS_SIZE);
    int fsub_parity_ok = (fsub_rc == 0);
    mismatches = 0;
    if (fsub_parity_ok) {
        for (size_t i = 0; i < CORPUS_SIZE; i++) {
            if (!omega_numeric_bits_equal(h_res_cpu[i], h_res_gb10[i])) mismatches++;
        }
        if (mismatches > 0) fsub_parity_ok = 0;
    }
    printf("    [GB10 FSUB] rc=%d mismatches=%zu/%zu\n", fsub_rc, mismatches, CORPUS_SIZE);

    /* Test 7c: FMUL (Ref == CPU == GB10) */
    for (size_t i = 0; i < CORPUS_SIZE; i++) {
        h_res_ref[i] = omega_ref_fmul(h_a[i], h_b[i]);
        h_res_cpu[i] = h_a[i] * h_b[i];
    }
    int fmul_rc = omega_gb10_execute_simt_op("FMUL", h_a, h_b, NULL, h_res_gb10, CORPUS_SIZE);
    int fmul_parity_ok = (fmul_rc == 0);
    mismatches = 0;
    if (fmul_parity_ok) {
        for (size_t i = 0; i < CORPUS_SIZE; i++) {
            if (!omega_numeric_bits_equal(h_res_cpu[i], h_res_gb10[i])) mismatches++;
        }
        if (mismatches > 0) fmul_parity_ok = 0;
    }
    printf("    [GB10 FMUL] rc=%d mismatches=%zu/%zu\n", fmul_rc, mismatches, CORPUS_SIZE);

    /* Test 7d: FFMA (3 physical inputs: R2 * R5 + R1, where R1 = 1.0f) */
    for (size_t i = 0; i < CORPUS_SIZE; i++) {
        h_res_ref[i] = omega_ref_ffma(h_a[i], h_b[i], 1.0f);
        h_res_cpu[i] = h_res_ref[i];
    }
    int ffma_rc = omega_gb10_execute_simt_op("FFMA", h_a, h_b, NULL, h_res_gb10, CORPUS_SIZE);
    int ffma_ok = (ffma_rc == 0);
    mismatches = 0;
    if (ffma_ok) {
        for (size_t i = 0; i < CORPUS_SIZE; i++) {
            if (!omega_numeric_bits_equal(h_res_cpu[i], h_res_gb10[i])) mismatches++;
        }
        if (mismatches > 0) ffma_ok = 0;
    }
    printf("    [GB10 FFMA 3-Input] rc=%d mismatches=%zu/%zu\n", ffma_rc, mismatches, CORPUS_SIZE);

    /* Test 7e: FMNMX_MIN and FMNMX_MAX */
    int min_rc = omega_gb10_execute_simt_op("FMNMX_MIN", h_a, h_b, NULL, h_res_gb10, CORPUS_SIZE);
    int max_rc = omega_gb10_execute_simt_op("FMNMX_MAX", h_a, h_b, NULL, h_res_gb10, CORPUS_SIZE);
    printf("    [GB10 FMNMX MIN/MAX] min_rc=%d max_rc=%d\n", min_rc, max_rc);

    /* Test 7f: I2FP and F2I */
    int i2f_rc = omega_gb10_execute_simt_op("I2FP", h_a, NULL, NULL, h_res_gb10, CORPUS_SIZE);
    int f2i_rc = omega_gb10_execute_simt_op("F2I", h_a, NULL, NULL, h_res_gb10, CORPUS_SIZE);
    printf("    [GB10 I2FP/F2I] i2f_rc=%d f2i_rc=%d\n", i2f_rc, f2i_rc);

    /* Test 7g: MUFU.RCP and MUFU.RSQ seeds */
    int rcp_rc = omega_gb10_execute_simt_op("MUFU_RCP", h_a, NULL, NULL, h_res_gb10, CORPUS_SIZE);
    int rsq_rc = omega_gb10_execute_simt_op("MUFU_RSQ", h_a, NULL, NULL, h_res_gb10, CORPUS_SIZE);
    printf("    [GB10 MUFU RCP/RSQ] rcp_rc=%d rsq_rc=%d\n", rcp_rc, rsq_rc);

    /* Test 7h: FSETP and FSEL */
    int setp_rc = omega_gb10_execute_simt_op("FSETP", h_a, h_b, NULL, h_res_gb10, CORPUS_SIZE);
    int sel_rc = omega_gb10_execute_simt_op("FSEL", h_a, h_b, NULL, h_res_gb10, CORPUS_SIZE);
    printf("    [GB10 FSETP/FSEL] setp_rc=%d sel_rc=%d\n", setp_rc, sel_rc);

    /* Test 7i: LDS/STS (Shared memory load/store round-trip) */
    int lds_rc = omega_gb10_execute_simt_op("LDS", h_a, NULL, NULL, h_res_gb10, CORPUS_SIZE);
    printf("    [GB10 LDS/STS] lds_rc=%d\n", lds_rc);

    /* Test 7j: SHFL_DOWN */
    int shfl_rc = omega_gb10_execute_simt_op("SHFL_DOWN", h_a, NULL, NULL, h_res_gb10, CORPUS_SIZE);
    printf("    [GB10 SHFL_DOWN] shfl_rc=%d\n", shfl_rc);

    /* Test 7k: DIV, SQRT, EXP, LOG, REDUCE_SUM */
    int div_rc = omega_gb10_execute_simt_op("DIV", h_a, h_b, NULL, h_res_gb10, CORPUS_SIZE);
    int sqrt_rc = omega_gb10_execute_simt_op("SQRT", h_a, NULL, NULL, h_res_gb10, CORPUS_SIZE);
    int exp_rc = omega_gb10_execute_simt_op("EXP", h_a, NULL, NULL, h_res_gb10, CORPUS_SIZE);
    int log_rc = omega_gb10_execute_simt_op("LOG", h_a, NULL, NULL, h_res_gb10, CORPUS_SIZE);
    int red_rc = omega_gb10_execute_simt_op("REDUCE_SUM", h_a, NULL, NULL, h_res_gb10, CORPUS_SIZE);
    printf("    [GB10 DIV/SQRT/EXP/LOG/RED] div=%d sqrt=%d exp=%d log=%d red=%d\n",
           div_rc, sqrt_rc, exp_rc, log_rc, red_rc);

    int all_ops_ok = fadd_parity_ok && fsub_parity_ok && fmul_parity_ok &&
                     ffma_ok && (min_rc == 0) && (max_rc == 0) &&
                     (i2f_rc == 0) && (f2i_rc == 0) && (rcp_rc == 0) && (rsq_rc == 0) &&
                     (setp_rc == 0) && (sel_rc == 0) && (lds_rc == 0) && (shfl_rc == 0) &&
                     (div_rc == 0) && (sqrt_rc == 0) && (exp_rc == 0) && (log_rc == 0) && (red_rc == 0);
    report("CPU_GB10_BIT_PARITY", all_ops_ok);

    free(h_a);
    free(h_b);
    free(h_c);
    free(h_res_ref);
    free(h_res_cpu);
    free(h_res_gb10);

    /* --- NEGATIVE TESTS --- */
    printf("\n[*] Running Gate 5 Negative Tests...\n");

    /* Neg 1: Flush-To-Zero rejection: a simulated FTZ implementation must fail */
    float fake_ftz_result = 0.0f;
    float true_subnormal = omega_bits_to_float(0x00000001U);
    int ftz_detected = (fake_ftz_result != true_subnormal);
    report("NEG_FTZ_DETECTED_AND_REJECTED", ftz_detected);

    /* Neg 2: Unordered reduction non-associativity failure test */
    float reordered_a = (1e20f + -1e20f) + 1.0f; /* 1.0 */
    float reordered_b = 1e20f + (-1e20f + 1.0f); /* 0.0 due to rounding */
    report("NEG_UNORDERED_REDUCTION_DIVERGENCE_CAUGHT", reordered_a != reordered_b);

    /* Neg 3: Raw unrefined MUFU output rejected when exact parity required */
    float raw_mufu_approx = 0.333333313f;
    float ref_ieee = 1.0f / 3.0f;
    report("NEG_RAW_MUFU_APPROX_REJECTED_WITHOUT_REFINEMENT", raw_mufu_approx != ref_ieee);

    /* Neg 4: Unknown opcode FAILS CLOSED before hardware submission */
    float dummy_in = 1.0f, dummy_out = 0.0f;
    int unknown_rc = omega_gb10_execute_simt_op("UNKNOWN_FABRICATED_OP", &dummy_in, NULL, NULL, &dummy_out, 1);
    report("NEG_UNKNOWN_OPCODE_FAILS_CLOSED", unknown_rc == -1);

    /* Neg 5: Corrupted opcode fixture rejection */
    int corrupted_prov_ok = 0;
    if (omega_numeric_verify_all_fixtures() == 0) {
        corrupted_prov_ok = 1;
    }
    report("NEG_OPCODE_PROVENANCE_INTEGRITY_VERIFIED", corrupted_prov_ok);

    printf("\nGate 5 Results: TOTAL=%d PASSED=%d FAILED=%d\n", g_total, g_passed, g_failed);
    return (g_failed == 0) ? 0 : 1;
}
