/* Host-only: omega_matmul_frag_pack_row0 over a zeroed buffer equals omega_matmul_frag_pack_a at
 * m = 1 for every k shape the engine pads (k not a multiple of 16 included), and the comparator can
 * say no (one perturbed halfword must be reported). No chip. */
#include "../src/omega_blackwell_codegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng = 0x9e3779b9u;
static uint16_t next16(void) { rng = rng * 1664525u + 1013904223u; return (uint16_t)(rng >> 16); }

static size_t diff_count(const uint16_t *x, const uint16_t *y, size_t n) {
    size_t d = 0;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) d++;
    return d;
}

int main(void) {
    const uint32_t ks[] = { 16, 17, 100, 1024, 4096, 4097, 12288 };
    int fails = 0;
    for (size_t t = 0; t < sizeof ks / sizeof ks[0]; t++) {
        const uint32_t k = ks[t], kp = (k + 15u) / 16u * 16u, mp = 16u;
        const size_t words = (size_t)mp * kp;
        uint16_t *a = malloc(k * sizeof *a), *ref = malloc(words * sizeof *ref), *got = calloc(words, sizeof *got);
        if (!a || !ref || !got) return 2;
        for (uint32_t i = 0; i < k; i++) a[i] = next16();
        omega_matmul_frag_pack_a(a, 1, k, mp, kp, ref);
        omega_matmul_frag_pack_row0(a, k, kp, got);
        size_t d = diff_count(ref, got, words);
        /* a second call over the same buffer with new values must fully replace the old ones */
        for (uint32_t i = 0; i < k; i++) a[i] = next16();
        omega_matmul_frag_pack_a(a, 1, k, mp, kp, ref);
        omega_matmul_frag_pack_row0(a, k, kp, got);
        size_t d2 = diff_count(ref, got, words);
        /* negative control: one perturbed halfword inside row 0's lanes must be seen */
        got[(size_t)(kp / 16u - 1u) * 32u * 8u + 4u] ^= 0x8000u;
        size_t d3 = diff_count(ref, got, words);
        int ok = d == 0 && d2 == 0 && d3 == 1;
        printf("%s k=%u kp=%u diff=%zu rewrite_diff=%zu perturbed_diff=%zu (want 0 0 1)\n", ok ? "PASS" : "FAIL", k, kp, d, d2, d3);
        if (!ok) fails++;
        free(a); free(ref); free(got);
    }
    printf("%s pack_row0 %d shape(s) failed\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
