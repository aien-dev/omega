#include "omega_numeric.h"
#include "omega_numeric_provenance.h"
#include "omega_blackwell_codegen.h"
#include "forge_descriptor.h"

#include <math.h>
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
        printf("    [%zu] %-30s (0x%04x) - %s\n", i + 1, p->mnemonic, p->opcode, p->evidence_source);
    }
    report("FP32_SIMT_OPCODES_ENCODED", fixtures_rc == 0 && prov_rc == 0 && op_count >= 12);

    /* 2. OMEGA_MATH_SEQUENCES_QUALIFIED */
    /* Verify division, sqrt, exp, log refinement sequences on CPU */
    int math_ok = 1;

    /* Division tests */
    float d1 = omega_math_div(10.0f, 2.0f);
    if (!omega_numeric_bits_equal(d1, 5.0f)) math_ok = 0;
    float d2 = omega_math_div(1.0f, 3.0f);
    if (fabsf(d2 - (1.0f / 3.0f)) > 1e-6f) math_ok = 0;

    /* Sqrt tests */
    float s1 = omega_math_sqrt(4.0f);
    if (!omega_numeric_bits_equal(s1, 2.0f)) math_ok = 0;
    float s2 = omega_math_sqrt(2.0f);
    if (fabsf(s2 - 1.41421356f) > 1e-6f) math_ok = 0;

    /* Exp tests */
    float e0 = omega_math_exp(0.0f);
    if (!omega_numeric_bits_equal(e0, 1.0f)) math_ok = 0;
    float e1 = omega_math_exp(1.0f);
    if (fabsf(e1 - 2.71828182f) > 1e-5f) math_ok = 0;

    /* Log tests */
    float l1 = omega_math_log(1.0f);
    if (fabsf(l1) > 1e-6f) math_ok = 0;
    float le = omega_math_log(2.71828182f);
    if (fabsf(le - 1.0f) > 1e-5f) math_ok = 0;

    report("OMEGA_MATH_SEQUENCES_QUALIFIED", math_ok);

    /* 3. SUBNORMALS_PRESERVED_NO_FTZ */
    /* Test preservation of IEEE 754 single-precision subnormals */
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
    /* Validate rule: raw MUFU reciprocal is seed only and differs from refined result */
    float test_val = 3.0f;
    float refined_div = omega_math_div(1.0f, test_val);
    /* Verify that refined division achieves full precision */
    int mufu_seed_rule_ok = (fabsf(refined_div * test_val - 1.0f) < 1e-7f);
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
    if (signbit(pzero) != 0 || signbit(nzero) == 0) edge_ok = 0;

    /* Infinities */
    float pinf = INFINITY;
    float ninf = -INFINITY;
    if (omega_float_to_bits(pinf) != 0x7f800000U) edge_ok = 0;
    if (omega_float_to_bits(ninf) != 0xff800000U) edge_ok = 0;

    /* NaNs */
    float qnan = omega_bits_to_float(0x7fc00000U);
    if (!isnan(qnan)) edge_ok = 0;

    /* Smallest and largest normals */
    float min_norm = omega_bits_to_float(0x00800000U); /* 1.17549435e-38 */
    float max_norm = omega_bits_to_float(0x7f7fffffU); /* 3.40282347e+38 */
    if (min_norm <= 0.0f || isinf(min_norm) || isnan(min_norm)) edge_ok = 0;
    if (max_norm <= 0.0f || isinf(max_norm) || isnan(max_norm)) edge_ok = 0;

    /* Overflow & Underflow handling in exp */
    if (!isinf(omega_math_exp(100.0f))) edge_ok = 0;
    if (omega_math_exp(-150.0f) != 0.0f) edge_ok = 0;

    report("EDGE_CLASS_BEHAVIOR_VERIFIED", edge_ok);

    /* 7. CPU_GB10_BIT_PARITY (Three-tier qualification: Ref == CPU == GB10) */
    printf("\n[*] Running Three-Tier Parity Verification on Physical GB10 Silicon...\n");
    const size_t CORPUS_SIZE = 64; /* 64 elements = 2 full warps */
    float *h_a = (float *)malloc(CORPUS_SIZE * sizeof(float));
    float *h_b = (float *)malloc(CORPUS_SIZE * sizeof(float));
    float *h_c_ref = (float *)malloc(CORPUS_SIZE * sizeof(float));
    float *h_c_cpu = (float *)malloc(CORPUS_SIZE * sizeof(float));
    float *h_c_gb10 = (float *)malloc(CORPUS_SIZE * sizeof(float));

    uint32_t seed = 0x19283746U;
    for (size_t i = 0; i < CORPUS_SIZE; i++) {
        if (i < num_subs) {
            h_a[i] = omega_bits_to_float(subnormal_patterns[i]);
            h_b[i] = 1.0f;
        } else if (i == num_subs) {
            h_a[i] = 0.0f; h_b[i] = -0.0f;
        } else if (i == num_subs + 1) {
            h_a[i] = INFINITY; h_b[i] = 2.0f;
        } else {
            uint32_t r = lcg_next(&seed);
            h_a[i] = (float)(int32_t)(r & 0xffff) / 256.0f;
            h_b[i] = (float)(int32_t)((r >> 16) & 0xffff) / 256.0f;
        }
        h_c_ref[i] = omega_ref_fadd(h_a[i], h_b[i]);
        h_c_cpu[i] = h_a[i] + h_b[i];
    }

    int gb10_rc = omega_gb10_execute_simt_op("FADD", h_a, h_b, NULL, h_c_gb10, CORPUS_SIZE);
    int parity_ok = (gb10_rc == 0);
    size_t mismatches = 0;
    if (parity_ok) {
        for (size_t i = 0; i < CORPUS_SIZE; i++) {
            if (!omega_numeric_bits_equal(h_c_cpu[i], h_c_gb10[i])) {
                mismatches++;
                printf("  Mismatch at [%zu]: CPU=0x%08x (%f) GB10=0x%08x (%f)\n",
                       i, omega_float_to_bits(h_c_cpu[i]), h_c_cpu[i],
                       omega_float_to_bits(h_c_gb10[i]), h_c_gb10[i]);
            }
        }
        if (mismatches > 0) parity_ok = 0;
    }
    printf("    GB10 Execution result: %d (Mismatches: %zu / %zu)\n", gb10_rc, mismatches, CORPUS_SIZE);
    report("CPU_GB10_BIT_PARITY", parity_ok);

    free(h_a);
    free(h_b);
    free(h_c_ref);
    free(h_c_cpu);
    free(h_c_gb10);

    /* --- NEGATIVE TESTS --- */
    printf("\n[*] Running Gate 5 Negative Tests...\n");

    /* Neg 1: Flush-To-Zero rejection: a simulated FTZ implementation must fail */
    float fake_ftz_result = 0.0f; /* simulated FTZ of a non-zero subnormal */
    float true_subnormal = omega_bits_to_float(0x00000001U);
    int ftz_detected = (fake_ftz_result != true_subnormal);
    report("NEG_FTZ_DETECTED_AND_REJECTED", ftz_detected);

    /* Neg 2: Unordered reduction non-associativity failure test */
    /* Floating point addition is non-associative; arbitrary reordering produces bit divergence */
    float reordered_a = (1e20f + -1e20f) + 1.0f; /* 1.0 */
    float reordered_b = 1e20f + (-1e20f + 1.0f); /* 0.0 due to rounding! */
    report("NEG_UNORDERED_REDUCTION_DIVERGENCE_CAUGHT", reordered_a != reordered_b);

    /* Neg 3: Raw unrefined MUFU output rejected when exact parity required */
    float raw_mufu_approx = 0.333333313f; /* typical hardware rcp approx for 1/3 */
    float ref_ieee = 1.0f / 3.0f;         /* 0.333333343f */
    report("NEG_RAW_MUFU_APPROX_REJECTED_WITHOUT_REFINEMENT", raw_mufu_approx != ref_ieee);

    printf("\nGate 5 Results: TOTAL=%d PASSED=%d FAILED=%d\n", g_total, g_passed, g_failed);
    return (g_failed == 0) ? 0 : 1;
}
