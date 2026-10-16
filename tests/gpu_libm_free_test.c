/* gpu_libm_free_test: omega#471. libomega_gpu.a must not reference libm. The FP8 host packer used ldexpf(1.0f, e)
 * for its exact powers of two; it now uses omega_hostmath_pow2f(e), built from the bit pattern. This test links -lm
 * and proves the replacement changes no output bit:
 *   1. omega_hostmath_pow2f(e) == ldexpf(1.0f, e) bit for bit, for every e in -400..400 and INT_MIN, INT_MAX
 *      (covers +inf, every normal, every subnormal and the round-to-zero tail).
 *   2. omega_fp8_e4m3_ref(b) == the ldexpf reference for all 256 bytes.
 *   3. omega_fp8_pack_scales, both fold modes, == the ldexpf reference (return code and bits) for all 65536 bf16 codes.
 *   4. omega_fp8_pack_x_bf16 == the ldexpf reference for all 65536 bf16 codes.
 *   5. omega_fp8_pack_x_f32 == the ldexpf reference for all 2^32 f32 bit patterns.
 * The reference functions below are the pre-#471 code, verbatim except for their names.
 * The archive symbol check (no libm-exported name among the archive's undefined symbols) is the shell half of
 * `make test-gpu-libm-free`. */
#include "omega_gpu_fp8_api.h"
#include "omega_gpu_hostmath.h"
#include "omega_blackwell_codegen_moe.h"
#include "omega_blackwell_matmul.h"
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static float ref_e4m3(uint8_t b) {
    int exponent = (b >> 3) & 0x0f;
    float mantissa = (float)(b & 0x07);
    float sign = (b & 0x80) ? -1.0f : 1.0f;
    if (exponent == 0x0f && (b & 0x07) == 0x07) return NAN; /* 0x7f / 0xff */
    if (exponent == 0) return sign * mantissa * ldexpf(1.0f, -9);
    return sign * (1.0f + mantissa / 8.0f) * ldexpf(1.0f, exponent - 7);
}

static int ref_pack_scales(const uint16_t *bf16, size_t count, float *out, int mutant_no_fold) {
    const float fold = mutant_no_fold ? 1.0f : ldexpf(1.0f, OMEGA_FP8_SCALE_EXP);
    for (size_t i = 0; i < count; i++) {
        float s = omega_bf16_to_fp32(bf16[i]);
        if (!(fabsf(s) <= 3.0e38f)) return -1; /* NaN or inf scale */
        float f = s * fold;
        if (!(fabsf(f) <= 3.0e38f)) return -1; /* fold overflow */
        out[i] = f;
    }
    return 0;
}

static void ref_pack_x_f32(const float *x, uint32_t k, float *out) {
    const float fold = ldexpf(1.0f, OMEGA_FP8_X_EXP);
    for (uint32_t i = 0; i < k; i++) out[i] = x[i] * fold;
}

static void ref_pack_x_bf16(const uint16_t *x, uint32_t k, float *out) {
    const float fold = ldexpf(1.0f, OMEGA_FP8_X_EXP);
    for (uint32_t i = 0; i < k; i++) out[i] = omega_bf16_to_fp32(x[i]) * fold;
}

static int failures = 0;
static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

int main(void) {
    /* 1. the helper itself, over its whole behaviour range */
    {
        int bad = 0, n = 0;
        for (int e = -400; e <= 400; e++, n++) {
            volatile int ve = e; /* keep ldexpf a real libm call, not a folded constant */
            if (fbits(omega_hostmath_pow2f(e)) != fbits(ldexpf(1.0f, ve))) {
                if (bad < 5) fprintf(stderr, "pow2f(%d) = %08x, ldexpf = %08x\n", e,
                                     fbits(omega_hostmath_pow2f(e)), fbits(ldexpf(1.0f, ve)));
                bad++;
            }
        }
        const int edge[2] = {INT_MIN, INT_MAX};
        for (int i = 0; i < 2; i++, n++) {
            volatile int ve = edge[i];
            if (fbits(omega_hostmath_pow2f(edge[i])) != fbits(ldexpf(1.0f, ve))) bad++;
        }
        char msg[128];
        snprintf(msg, sizeof msg, "pow2f == ldexpf(1, e) for e in -400..400, INT_MIN, INT_MAX (%d exponents)", n);
        check(bad == 0, msg);
    }
    /* 2. e4m3 reference conversion, all 256 bytes */
    {
        int bad = 0;
        for (int b = 0; b < 256; b++)
            if (fbits(omega_fp8_e4m3_ref((uint8_t)b)) != fbits(ref_e4m3((uint8_t)b))) bad++;
        check(bad == 0, "omega_fp8_e4m3_ref bit-identical for all 256 e4m3 bytes");
    }
    /* 3 and 4. every bf16 code, one at a time so each refusal is compared on its own */
    {
        int bad_s = 0, bad_x = 0, refused = 0;
        for (uint32_t c = 0; c < 65536; c++) {
            uint16_t v = (uint16_t)c;
            for (int mutant = 0; mutant < 2; mutant++) {
                float a = 0.0f, r = 0.0f;
                int ra = omega_fp8_pack_scales(&v, 1, &a, mutant);
                int rr = ref_pack_scales(&v, 1, &r, mutant);
                if (ra != rr || (ra == 0 && fbits(a) != fbits(r))) bad_s++;
                if (ra != 0) refused++;
            }
            float a = 0.0f, r = 0.0f;
            omega_fp8_pack_x_bf16(&v, 1, &a);
            ref_pack_x_bf16(&v, 1, &r);
            if (fbits(a) != fbits(r)) bad_x++;
        }
        char msg[160];
        snprintf(msg, sizeof msg,
                 "omega_fp8_pack_scales identical (code and bits) for all 65536 bf16 codes x 2 fold modes (%d refusals, same in both)",
                 refused);
        check(bad_s == 0, msg);
        check(bad_x == 0, "omega_fp8_pack_x_bf16 bit-identical for all 65536 bf16 codes");
    }
    /* 5. every f32 bit pattern through the f32 stager, in chunks */
    {
        enum { CHUNK = 1 << 16 };
        float *in = malloc(CHUNK * sizeof(float)), *a = malloc(CHUNK * sizeof(float)), *r = malloc(CHUNK * sizeof(float));
        if (!in || !a || !r) { fprintf(stderr, "out of memory\n"); return 2; }
        uint64_t bad = 0;
        for (uint64_t base = 0; base < (1ull << 32); base += CHUNK) {
            for (uint32_t i = 0; i < CHUNK; i++) {
                uint32_t u = (uint32_t)(base + i);
                memcpy(&in[i], &u, 4);
            }
            omega_fp8_pack_x_f32(in, CHUNK, a);
            ref_pack_x_f32(in, CHUNK, r);
            if (memcmp(a, r, CHUNK * sizeof(float)) != 0)
                for (uint32_t i = 0; i < CHUNK; i++) bad += fbits(a[i]) != fbits(r[i]);
        }
        free(in); free(a); free(r);
        check(bad == 0, "omega_fp8_pack_x_f32 bit-identical for all 2^32 f32 bit patterns");
    }
    printf("gpu_libm_free_test: %s (%d failing checks)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
