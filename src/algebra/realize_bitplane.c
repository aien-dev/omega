/* MA-2 R2: packed ternary bitplanes (2 bits per weight).
 *
 * R2_bitplane: per row, chunks of 128 weights stored as two 16-byte planes
 *   P (pos) and N (neg). Bit k of byte j <-> weight (chunk + 16k + j), so the
 *   16 byte masks for weights chunk+16k .. chunk+16k+15 come from one
 *   vtstq_u8(P, 1<<k). Weight bytes are rebuilt as w = Nmask - Pmask
 *   (-1 - 0 = -1 for neg, 0 - (-1) = +1 for pos) and fed to SDOT.
 *   4 SIMD ops per 16 weights per row (2 tst, 1 sub, 1 sdot); x loads are
 *   shared by a 4-row tile.
 *
 * R2b_lut (T-MAC style lookup): activations in groups of 4; per call, for
 *   every group g a 16-entry table T_g[s] = sum of x over the subset s of the
 *   group (|T| <= 512, int16) is built and split into lo = T & 0xFF (uint8)
 *   and hi = T >> 8 (int8 in [-2,1]); T = 256*hi + lo exactly.
 *   Weights: per row and group one byte (pos nibble | neg nibble << 4), i.e.
 *   2 bits per weight. y = sum_g T_g[pos] - T_g[neg].
 *   Kernel: 4 groups (16 activations) x 4 rows per vqtbl4q lookup (64-byte
 *   tables = 4 groups); USDOT/SDOT with +1/-1 vectors sum each row's 4 lanes.
 *   16 rows per table load. Table build (depends on x) is part of every run.
 *
 * R2c_crumb (below): 2-bit two's-complement codes, shift-pair decode.
 */
#include "algebra/realize_common.h"

#include <arm_neon.h>
#include <stdlib.h>
#include <string.h>

/* ================= R2_bitplane ================= */
#define BP_CHUNK 128u

static int pack_bitplane(oma_rz_plan *p, const int8_t *w, size_t m, size_t n) {
    int rc = oma_rz_check_shape(m, n, OMA_RZ_MAX_N);
    if (rc) return rc;
    if (!p || !w) return OMA_RZ_E_ARG;
    rc = oma_rz_validate(w, m, n);
    if (rc) return rc;
    memset(p, 0, sizeof *p);
    size_t chunks = (n + BP_CHUNK - 1) / BP_CHUNK;
    size_t row_bytes = chunks * 32u;
    uint8_t *buf = oma_rz_alloc(m * row_bytes);
    if (!buf) return OMA_RZ_E_NOMEM;
    uint64_t nnz = 0;
    for (size_t i = 0; i < m; i++) {
        const int8_t *row = w + i * n;
        uint8_t *dst = buf + i * row_bytes;
        for (size_t c = 0; c < chunks; c++) {
            uint8_t *P = dst + c * 32u, *N = P + 16;
            for (unsigned k = 0; k < 8; k++)
                for (unsigned j = 0; j < 16; j++) {
                    size_t col = c * BP_CHUNK + 16u * k + j;
                    if (col >= n) continue;
                    int8_t v = row[col];
                    if (v > 0) P[j] |= (uint8_t)(1u << k);
                    else if (v < 0) N[j] |= (uint8_t)(1u << k);
                    nnz += (v != 0);
                }
        }
    }
    p->m = m;
    p->n = n;
    p->mem = buf;
    p->weight_bytes = m * row_bytes;
    p->footprint_bytes = m * row_bytes;
    p->nnz = nnz;
    return OMA_RZ_OK;
}

#define BP_STEP(k)                                                             \
    do {                                                                       \
        const uint8x16_t bit = vdupq_n_u8((uint8_t)(1u << (k)));               \
        const int8x16_t xv = X[k];                                             \
        a0 = vdotq_s32(a0, vsubq_s8(vreinterpretq_s8_u8(vtstq_u8(N0, bit)),    \
                                    vreinterpretq_s8_u8(vtstq_u8(P0, bit))), xv); \
        a1 = vdotq_s32(a1, vsubq_s8(vreinterpretq_s8_u8(vtstq_u8(N1, bit)),    \
                                    vreinterpretq_s8_u8(vtstq_u8(P1, bit))), xv); \
        a2 = vdotq_s32(a2, vsubq_s8(vreinterpretq_s8_u8(vtstq_u8(N2, bit)),    \
                                    vreinterpretq_s8_u8(vtstq_u8(P2, bit))), xv); \
        a3 = vdotq_s32(a3, vsubq_s8(vreinterpretq_s8_u8(vtstq_u8(N3, bit)),    \
                                    vreinterpretq_s8_u8(vtstq_u8(P3, bit))), xv); \
    } while (0)

static inline void load_xchunk(const int8_t *x, size_t n, size_t c0, int8x16_t X[8]) {
    if (c0 + BP_CHUNK <= n) {
        for (int k = 0; k < 8; k++) X[k] = vld1q_s8(x + c0 + 16u * (unsigned)k);
    } else {
        int8_t t[BP_CHUNK];
        memset(t, 0, sizeof t);
        memcpy(t, x + c0, n - c0);
        for (int k = 0; k < 8; k++) X[k] = vld1q_s8(t + 16 * k);
    }
}

static int run_bitplane(const oma_rz_plan *p, const int8_t *x, int32_t *y) {
    if (!p || !p->mem || !x || !y) return OMA_RZ_E_ARG;
    const uint8_t *w = p->mem;
    size_t m = p->m, n = p->n;
    size_t chunks = (n + BP_CHUNK - 1) / BP_CHUNK, row_bytes = chunks * 32u;
    size_t i = 0;
    for (; i + 4 <= m; i += 4) {
        const uint8_t *r0 = w + i * row_bytes, *r1 = r0 + row_bytes;
        const uint8_t *r2 = r1 + row_bytes, *r3 = r2 + row_bytes;
        int32x4_t a0 = vdupq_n_s32(0), a1 = a0, a2 = a0, a3 = a0;
        for (size_t c = 0; c < chunks; c++) {
            int8x16_t X[8];
            load_xchunk(x, n, c * BP_CHUNK, X);
            size_t o = c * 32u;
            uint8x16_t P0 = vld1q_u8(r0 + o), N0 = vld1q_u8(r0 + o + 16);
            uint8x16_t P1 = vld1q_u8(r1 + o), N1 = vld1q_u8(r1 + o + 16);
            uint8x16_t P2 = vld1q_u8(r2 + o), N2 = vld1q_u8(r2 + o + 16);
            uint8x16_t P3 = vld1q_u8(r3 + o), N3 = vld1q_u8(r3 + o + 16);
            BP_STEP(0); BP_STEP(1); BP_STEP(2); BP_STEP(3);
            BP_STEP(4); BP_STEP(5); BP_STEP(6); BP_STEP(7);
        }
        y[i] = vaddvq_s32(a0);
        y[i + 1] = vaddvq_s32(a1);
        y[i + 2] = vaddvq_s32(a2);
        y[i + 3] = vaddvq_s32(a3);
    }
    for (; i < m; i++) {
        const uint8_t *r0 = w + i * row_bytes;
        int32x4_t a0 = vdupq_n_s32(0), b0 = a0;
        for (size_t c = 0; c < chunks; c++) {
            int8x16_t X[8];
            load_xchunk(x, n, c * BP_CHUNK, X);
            uint8x16_t P0 = vld1q_u8(r0 + c * 32u), N0 = vld1q_u8(r0 + c * 32u + 16);
            for (unsigned k = 0; k < 8; k += 2) {
                uint8x16_t bit = vdupq_n_u8((uint8_t)(1u << k));
                uint8x16_t bit2 = vdupq_n_u8((uint8_t)(1u << (k + 1)));
                a0 = vdotq_s32(a0, vsubq_s8(vreinterpretq_s8_u8(vtstq_u8(N0, bit)),
                                            vreinterpretq_s8_u8(vtstq_u8(P0, bit))), X[k]);
                b0 = vdotq_s32(b0, vsubq_s8(vreinterpretq_s8_u8(vtstq_u8(N0, bit2)),
                                            vreinterpretq_s8_u8(vtstq_u8(P0, bit2))), X[k + 1]);
            }
        }
        y[i] = vaddvq_s32(vaddq_s32(a0, b0));
    }
    return OMA_RZ_OK;
}

/* ================= R2b_lut ================= */
/* Weight layout: row blocks of 16 rows; per block and super-group G (16
 * activations = 4 groups): 64 bytes = 4 tiles x (4 rows x 4 groups).
 * Byte for (row r, group j) = pos_nibble | neg_nibble << 4, where bit b of a
 * nibble is weight 16G + 4j + b. Tables: per super-group 128 bytes:
 * lo[64] (entry 16j + s) then hi[64]. */
#define LUT_ROWS 16u

static int pack_lut(oma_rz_plan *p, const int8_t *w, size_t m, size_t n) {
    int rc = oma_rz_check_shape(m, n, OMA_RZ_MAX_N);
    if (rc) return rc;
    if (!p || !w) return OMA_RZ_E_ARG;
    rc = oma_rz_validate(w, m, n);
    if (rc) return rc;
    memset(p, 0, sizeof *p);
    size_t sg = (n + 15u) / 16u;                 /* super-groups */
    size_t mb = (m + LUT_ROWS - 1) / LUT_ROWS;   /* row blocks */
    size_t wbytes = mb * sg * 64u;
    uint8_t *buf = oma_rz_alloc(wbytes);
    uint8_t *tab = oma_rz_alloc(sg * 128u);
    if (!buf || !tab) {
        free(buf);
        free(tab);
        return OMA_RZ_E_NOMEM;
    }
    uint64_t nnz = 0;
    for (size_t b = 0; b < mb; b++)
        for (size_t G = 0; G < sg; G++) {
            uint8_t *dst = buf + (b * sg + G) * 64u;
            for (unsigned r = 0; r < LUT_ROWS; r++) {
                size_t row = b * LUT_ROWS + r;
                for (unsigned j = 0; j < 4; j++) {
                    unsigned pn = 0, nn = 0;
                    for (unsigned q = 0; q < 4; q++) {
                        size_t col = G * 16u + 4u * j + q;
                        if (row >= m || col >= n) continue;
                        int8_t v = w[row * n + col];
                        if (v > 0) pn |= 1u << q;
                        else if (v < 0) nn |= 1u << q;
                        nnz += (v != 0);
                    }
                    /* tile = r/4, lane in tile = (r%4)*4 + j */
                    dst[(r / 4u) * 16u + (r % 4u) * 4u + j] = (uint8_t)(pn | (nn << 4));
                }
            }
        }
    p->m = m;
    p->n = n;
    p->mem = buf;
    p->scratch = tab;
    p->weight_bytes = wbytes;
    p->scratch_bytes = sg * 128u;
    p->footprint_bytes = wbytes + sg * 128u;
    p->nnz = nnz;
    return OMA_RZ_OK;
}

/* subset-sum bit patterns for s = 0..15, bit b of s */
static const int16_t k_bit[4][16] = {
    {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1},
    {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1},
    {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1},
};

static void build_tables(const int8_t *x, size_t n, uint8_t *tab, size_t sg) {
    const int16x8_t b0l = vld1q_s16(k_bit[0]), b0h = vld1q_s16(k_bit[0] + 8);
    const int16x8_t b1l = vld1q_s16(k_bit[1]), b1h = vld1q_s16(k_bit[1] + 8);
    const int16x8_t b2l = vld1q_s16(k_bit[2]), b2h = vld1q_s16(k_bit[2] + 8);
    const int16x8_t b3l = vld1q_s16(k_bit[3]), b3h = vld1q_s16(k_bit[3] + 8);
    for (size_t G = 0; G < sg; G++) {
        int8_t xs[16];
        const int8_t *xp;
        if (G * 16u + 16u <= n) {
            xp = x + G * 16u;
        } else {
            memset(xs, 0, sizeof xs);
            memcpy(xs, x + G * 16u, n - G * 16u);
            xp = xs;
        }
        uint8_t *lo = tab + G * 128u, *hi = lo + 64;
        for (unsigned j = 0; j < 4; j++) {
            int16_t a = xp[4 * j], b = xp[4 * j + 1], c = xp[4 * j + 2], d = xp[4 * j + 3];
            int16x8_t tl = vmulq_n_s16(b0l, a), th = vmulq_n_s16(b0h, a);
            tl = vmlaq_n_s16(tl, b1l, b); th = vmlaq_n_s16(th, b1h, b);
            tl = vmlaq_n_s16(tl, b2l, c); th = vmlaq_n_s16(th, b2h, c);
            tl = vmlaq_n_s16(tl, b3l, d); th = vmlaq_n_s16(th, b3h, d);
            int8x16_t lov = vcombine_s8(vmovn_s16(tl), vmovn_s16(th));
            int8x16_t hiv = vcombine_s8(vshrn_n_s16(tl, 8), vshrn_n_s16(th, 8));
            vst1q_u8(lo + 16u * j, vreinterpretq_u8_s8(lov));
            vst1q_s8((int8_t *)hi + 16u * j, hiv);
        }
    }
}

static int run_lut(const oma_rz_plan *p, const int8_t *x, int32_t *y) {
    if (!p || !p->mem || !p->scratch || !x || !y) return OMA_RZ_E_ARG;
    size_t m = p->m, n = p->n;
    size_t sg = (n + 15u) / 16u, mb = (m + LUT_ROWS - 1) / LUT_ROWS;
    uint8_t *tab = p->scratch; /* per-call scratch, rebuilt from x every run */
    build_tables(x, n, tab, sg);
    const uint8_t *w = p->mem;
    static const uint8_t k_offs[16] = {0, 16, 32, 48, 0, 16, 32, 48, 0, 16, 32, 48, 0, 16, 32, 48};
    const uint8x16_t offs = vld1q_u8(k_offs), lowm = vdupq_n_u8(0x0F);
    const int8x16_t one = vdupq_n_s8(1), mone = vdupq_n_s8(-1);
    for (size_t b = 0; b < mb; b++) {
        int32x4_t L0 = vdupq_n_s32(0), L1 = L0, L2 = L0, L3 = L0;
        int32x4_t H0 = L0, H1 = L0, H2 = L0, H3 = L0;
        const uint8_t *wb = w + b * sg * 64u;
        for (size_t G = 0; G < sg; G++) {
            const uint8_t *t = tab + G * 128u;
            uint8x16x4_t LO = vld1q_u8_x4(t);
            int8x16x4_t HI = vld1q_s8_x4((const int8_t *)t + 64);
            const uint8_t *wg = wb + G * 64u;
#define LUT_TILE(T, Lacc, Hacc)                                                     \
            do {                                                                    \
                uint8x16_t wv = vld1q_u8(wg + 16u * (T));                           \
                uint8x16_t pi = vorrq_u8(vandq_u8(wv, lowm), offs);                 \
                uint8x16_t ni = vorrq_u8(vshrq_n_u8(wv, 4), offs);                  \
                Lacc = vusdotq_s32(Lacc, vqtbl4q_u8(LO, pi), one);                  \
                Lacc = vusdotq_s32(Lacc, vqtbl4q_u8(LO, ni), mone);                 \
                Hacc = vdotq_s32(Hacc, vqtbl4q_s8(HI, pi), one);                    \
                Hacc = vdotq_s32(Hacc, vqtbl4q_s8(HI, ni), mone);                   \
            } while (0)
            LUT_TILE(0, L0, H0);
            LUT_TILE(1, L1, H1);
            LUT_TILE(2, L2, H2);
            LUT_TILE(3, L3, H3);
#undef LUT_TILE
        }
        int32_t out[16];
        vst1q_s32(out, vaddq_s32(vshlq_n_s32(H0, 8), L0));
        vst1q_s32(out + 4, vaddq_s32(vshlq_n_s32(H1, 8), L1));
        vst1q_s32(out + 8, vaddq_s32(vshlq_n_s32(H2, 8), L2));
        vst1q_s32(out + 12, vaddq_s32(vshlq_n_s32(H3, 8), L3));
        size_t r0 = b * LUT_ROWS, cnt = m - r0 < LUT_ROWS ? m - r0 : LUT_ROWS;
        memcpy(y + r0, out, cnt * sizeof(int32_t));
    }
    return OMA_RZ_OK;
}

const oma_rz_impl oma_rz_r2_bitplane = {
    "R2_bitplane", "pos/neg bitplanes (2 b/w), vtst byte masks -> SDOT", "bitplane", 1, 0,
    OMA_RZ_MAX_N, pack_bitplane, run_bitplane};
const oma_rz_impl oma_rz_r2b_lut = {
    "R2b_lut", "T-MAC style 4-activation subset-sum LUT (2 b/w), vqtbl4q + USDOT", "lut", 1, 0,
    OMA_RZ_MAX_N, pack_lut, run_lut};

/* ================= R2c_crumb =================
 * 2-bit two's-complement "crumbs" (2 bits per weight, same footprint as the
 * bitplanes, different code): +1 = 01, 0 = 00, -1 = 11 (10 never written).
 * Per row, chunks of 64 weights in 16 bytes; bits 2k..2k+1 of byte j hold
 * weight chunk + 16k + j. Decode is a sign-extending shift pair:
 *   w_k = (int8)(b << (6 - 2k)) >> 6   (k = 3: one shift)
 * so 7 shifts + 4 SDOT per 64 weights per row (2.75 SIMD ops per 16
 * weights, against 4 for the H1 bitplanes). Conversion from the H1 code is
 * part of pack. */
static int pack_crumb(oma_rz_plan *p, const int8_t *w, size_t m, size_t n) {
    int rc = oma_rz_check_shape(m, n, OMA_RZ_MAX_N);
    if (rc) return rc;
    if (!p || !w) return OMA_RZ_E_ARG;
    rc = oma_rz_validate(w, m, n);
    if (rc) return rc;
    memset(p, 0, sizeof *p);
    size_t chunks = (n + 63u) / 64u, row_bytes = chunks * 16u;
    uint8_t *buf = oma_rz_alloc(m * row_bytes);
    if (!buf) return OMA_RZ_E_NOMEM;
    uint64_t nnz = 0;
    for (size_t i = 0; i < m; i++)
        for (size_t c = 0; c < chunks; c++)
            for (unsigned j = 0; j < 16; j++) {
                unsigned byte = 0;
                for (unsigned k = 0; k < 4; k++) {
                    size_t col = c * 64u + 16u * k + j;
                    int v = col < n ? w[i * n + col] : 0;
                    nnz += (v != 0);
                    byte |= ((unsigned)v & 3u) << (2u * k);
                }
                buf[i * row_bytes + c * 16u + j] = (uint8_t)byte;
            }
    p->m = m;
    p->n = n;
    p->mem = buf;
    p->weight_bytes = m * row_bytes;
    p->footprint_bytes = m * row_bytes;
    p->nnz = nnz;
    return OMA_RZ_OK;
}

static inline void load_x64(const int8_t *x, size_t n, size_t c0, int8x16_t X[4]) {
    if (c0 + 64u <= n) {
        for (int k = 0; k < 4; k++) X[k] = vld1q_s8(x + c0 + 16u * (unsigned)k);
    } else {
        int8_t t[64];
        memset(t, 0, sizeof t);
        memcpy(t, x + c0, n - c0);
        for (int k = 0; k < 4; k++) X[k] = vld1q_s8(t + 16 * k);
    }
}

#define CRUMB_ROW(bv, A, B)                                                   \
    do {                                                                      \
        A = vdotq_s32(A, vshrq_n_s8(vshlq_n_s8(bv, 6), 6), X[0]);             \
        B = vdotq_s32(B, vshrq_n_s8(vshlq_n_s8(bv, 4), 6), X[1]);             \
        A = vdotq_s32(A, vshrq_n_s8(vshlq_n_s8(bv, 2), 6), X[2]);             \
        B = vdotq_s32(B, vshrq_n_s8(bv, 6), X[3]);                            \
    } while (0)

static int run_crumb(const oma_rz_plan *p, const int8_t *x, int32_t *y) {
    if (!p || !p->mem || !x || !y) return OMA_RZ_E_ARG;
    const int8_t *w = p->mem;
    size_t m = p->m, n = p->n;
    size_t chunks = (n + 63u) / 64u, row_bytes = chunks * 16u;
    size_t i = 0;
    for (; i + 4 <= m; i += 4) {
        const int8_t *r0 = w + i * row_bytes;
        int32x4_t a0 = vdupq_n_s32(0), a1 = a0, a2 = a0, a3 = a0, b0 = a0, b1 = a0, b2 = a0, b3 = a0;
        for (size_t c = 0; c < chunks; c++) {
            int8x16_t X[4];
            load_x64(x, n, c * 64u, X);
            size_t o = c * 16u;
            int8x16_t w0 = vld1q_s8(r0 + o), w1 = vld1q_s8(r0 + row_bytes + o);
            int8x16_t w2 = vld1q_s8(r0 + 2 * row_bytes + o), w3 = vld1q_s8(r0 + 3 * row_bytes + o);
            CRUMB_ROW(w0, a0, b0);
            CRUMB_ROW(w1, a1, b1);
            CRUMB_ROW(w2, a2, b2);
            CRUMB_ROW(w3, a3, b3);
        }
        int32x4_t s01 = vpaddq_s32(vaddq_s32(a0, b0), vaddq_s32(a1, b1));
        int32x4_t s23 = vpaddq_s32(vaddq_s32(a2, b2), vaddq_s32(a3, b3));
        vst1q_s32(y + i, vpaddq_s32(s01, s23));
    }
    for (; i < m; i++) {
        const int8_t *r0 = w + i * row_bytes;
        int32x4_t a0 = vdupq_n_s32(0), b0 = a0;
        for (size_t c = 0; c < chunks; c++) {
            int8x16_t X[4];
            load_x64(x, n, c * 64u, X);
            int8x16_t w0 = vld1q_s8(r0 + c * 16u);
            CRUMB_ROW(w0, a0, b0);
        }
        y[i] = vaddvq_s32(vaddq_s32(a0, b0));
    }
    return OMA_RZ_OK;
}

const oma_rz_impl oma_rz_r2c_crumb = {
    "R2c_crumb", "2-bit two's-complement crumbs (2 b/w), shift-pair decode -> SDOT", "crumb2", 1, 0,
    OMA_RZ_MAX_N, pack_crumb, run_crumb};
