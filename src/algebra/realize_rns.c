/* MA-2 R4: residue number system (RNS).
 * Moduli {256, 255, 253} (pairwise coprime), M = 16,515,840. Every centered
 * residue fits int8: x mod 256 is x itself, x mod 255 in [-127,127],
 * x mod 253 in [-126,126]; ternary weights are their own residues.
 * Per call: build the three residue vectors of x (O(n)); per row, load the
 * int8 weights ONCE and run three SDOT channels on them (fused channels, the
 * cheapest RNS layout on this chip), reduce each channel sum mod p, then
 * reconstruct y by mixed-radix CRT (included in the cost):
 *   a0 = r256, a1 = (r255 - a0) mod 255          (256 == 1 mod 255)
 *   a2 = (r253 - a0 - 256 a1) * 211 mod 253      (256*255 == 6, 6^-1 == 211)
 *   Y  = a0 + 256 a1 + 65280 a2 in [0, M); y = Y - M if Y >= M/2.
 * Exactness needs |y| <= 128 n < M/2, so n <= 64,515 (max_n = 64,512).
 * Honest limit: each channel accumulates in int32 lanes (SDOT forces that);
 * a narrower channel accumulator is not cheaper on this chip, so RNS pays
 * 3x the multiply-accumulate work of R1 plus residue build and CRT. */
#include "algebra/realize_common.h"

#include <arm_neon.h>
#include <stdlib.h>
#include <string.h>

#define RNS_MAX_N ((size_t)64512u)
#define RNS_M 16515840

static int pack_rns(oma_rz_plan *p, const int8_t *w, size_t m, size_t n) {
    int rc = oma_rz_check_shape(m, n, RNS_MAX_N);
    if (rc) return rc;
    if (!p || !w) return OMA_RZ_E_ARG;
    rc = oma_rz_validate(w, m, n);
    if (rc) return rc;
    memset(p, 0, sizeof *p);
    size_t np = (n + 15u) & ~(size_t)15u;
    int8_t *buf = oma_rz_alloc(m * np);
    int8_t *res = oma_rz_alloc(3u * np);
    if (!buf || !res) {
        free(buf);
        free(res);
        return OMA_RZ_E_NOMEM;
    }
    uint64_t nnz = 0;
    for (size_t i = 0; i < m; i++) {
        memcpy(buf + i * np, w + i * n, n);
        for (size_t j = 0; j < n; j++) nnz += (w[i * n + j] != 0);
    }
    p->m = m;
    p->n = n;
    p->mem = buf;
    p->scratch = res;
    p->weight_bytes = m * np;
    p->scratch_bytes = 3u * np;
    p->footprint_bytes = m * np + 3u * np;
    p->nnz = nnz;
    return OMA_RZ_OK;
}

static inline int32_t modp(int32_t v, int32_t p) {
    int32_t r = v % p;
    return r < 0 ? r + p : r;
}

static int run_rns(const oma_rz_plan *p, const int8_t *x, int32_t *y) {
    if (!p || !p->mem || !p->scratch || !x || !y) return OMA_RZ_E_ARG;
    size_t m = p->m, n = p->n, np = (n + 15u) & ~(size_t)15u;
    int8_t *x256 = p->scratch, *x255 = x256 + np, *x253 = x255 + np;
    /* residue build (padding lanes stay 0 from pack) */
    for (size_t j = 0; j < n; j++) {
        int v = x[j];
        x256[j] = (int8_t)v;
        x255[j] = (int8_t)(v == -128 ? 127 : v);
        x253[j] = (int8_t)(v > 126 ? v - 253 : (v < -126 ? v + 253 : v));
    }
    const int8_t *w = p->mem;
    for (size_t i = 0; i < m; i++) {
        const int8_t *row = w + i * np;
        int32x4_t c0 = vdupq_n_s32(0), c1 = c0, c2 = c0, d0 = c0, d1 = c0, d2 = c0;
        size_t j = 0;
        for (; j + 32 <= np; j += 32) {
            int8x16_t wa = vld1q_s8(row + j), wb = vld1q_s8(row + j + 16);
            c0 = vdotq_s32(c0, wa, vld1q_s8(x256 + j));
            c1 = vdotq_s32(c1, wa, vld1q_s8(x255 + j));
            c2 = vdotq_s32(c2, wa, vld1q_s8(x253 + j));
            d0 = vdotq_s32(d0, wb, vld1q_s8(x256 + j + 16));
            d1 = vdotq_s32(d1, wb, vld1q_s8(x255 + j + 16));
            d2 = vdotq_s32(d2, wb, vld1q_s8(x253 + j + 16));
        }
        for (; j < np; j += 16) {
            int8x16_t wa = vld1q_s8(row + j);
            c0 = vdotq_s32(c0, wa, vld1q_s8(x256 + j));
            c1 = vdotq_s32(c1, wa, vld1q_s8(x255 + j));
            c2 = vdotq_s32(c2, wa, vld1q_s8(x253 + j));
        }
        int32_t r256 = vaddvq_s32(vaddq_s32(c0, d0)) & 255;
        int32_t r255 = modp(vaddvq_s32(vaddq_s32(c1, d1)), 255);
        int32_t r253 = modp(vaddvq_s32(vaddq_s32(c2, d2)), 253);
        int32_t a0 = r256;
        int32_t a1 = modp(r255 - a0, 255);
        int32_t a2 = modp((int32_t)(((int64_t)modp(r253 - a0 - 256 * a1, 253) * 211) % 253), 253);
        int32_t Y = a0 + 256 * a1 + 65280 * a2;
        y[i] = Y >= RNS_M / 2 ? Y - RNS_M : Y;
    }
    return OMA_RZ_OK;
}

const oma_rz_impl oma_rz_r4_rns = {
    "R4_rns", "RNS {256,255,253}, 3 fused SDOT channels + mixed-radix CRT", "rns", 1, 0,
    RNS_MAX_N, pack_rns, run_rns};
