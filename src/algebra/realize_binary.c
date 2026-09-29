/* MA-2 R1: conventional binary realizations. W stored as int8 (8 bits per
 * weight), int8 x int8 -> int32 multiply-accumulate.
 *   R1_plain : plain C loop at -O2 (labelled weak baseline).
 *   R1_sdot  : hand NEON SDOT (vdotq_s32), 4 rows x 64 bytes per step,
 *              8 independent accumulators, x loaded once per 4 rows.
 *   R1_sdot_il: same SDOT, rows stored in 4-row interleaved tiles so a tile
 *              is one sequential stream (best R1 for m >= 64).
 *   R1_smmla : I8MM SMMLA (vmmlaq_s32). Row pairs interleaved in 8-byte
 *              pieces; half of each 2x2 product is unused for GEMV (one x),
 *              so useful work per instruction equals SDOT.
 * SVE sdot is not a separate realization: this chip's SVE vector length is
 * 128 bits (svcntb() == 16), so SVE SDOT does the same work as NEON SDOT.
 */
#include "algebra/realize_common.h"

#include <arm_neon.h>
#include <string.h>

/* ---- shared int8 pack (validate + copy, row-major, no padding) ---- */
static int pack_rowmajor(oma_rz_plan *p, const int8_t *w, size_t m, size_t n) {
    int rc = oma_rz_check_shape(m, n, OMA_RZ_MAX_N);
    if (rc) return rc;
    if (!p || !w) return OMA_RZ_E_ARG;
    rc = oma_rz_validate(w, m, n);
    if (rc) return rc;
    memset(p, 0, sizeof *p);
    int8_t *buf = oma_rz_alloc(m * n);
    if (!buf) return OMA_RZ_E_NOMEM;
    memcpy(buf, w, m * n);
    uint64_t nnz = 0;
    for (size_t i = 0; i < m * n; i++) nnz += (w[i] != 0);
    p->m = m;
    p->n = n;
    p->mem = buf;
    p->weight_bytes = m * n;
    p->footprint_bytes = m * n;
    p->nnz = nnz;
    return OMA_RZ_OK;
}

/* ---- R1_plain ---- */
static int run_plain(const oma_rz_plan *p, const int8_t *x, int32_t *y) {
    if (!p || !p->mem || !x || !y) return OMA_RZ_E_ARG;
    const int8_t *w = p->mem;
    size_t m = p->m, n = p->n;
    for (size_t i = 0; i < m; i++) {
        const int8_t *row = w + i * n;
        int32_t acc = 0;
        for (size_t j = 0; j < n; j++) acc += (int32_t)row[j] * (int32_t)x[j];
        y[i] = acc;
    }
    return OMA_RZ_OK;
}

/* ---- R1_sdot ---- */
static inline int32_t tail_dot(const int8_t *row, const int8_t *x, size_t j0, size_t n) {
    int32_t acc = 0;
    for (size_t j = j0; j < n; j++) acc += (int32_t)row[j] * (int32_t)x[j];
    return acc;
}

static void sdot_rows4(const int8_t *w, size_t n, const int8_t *x, int32_t *y) {
    const int8_t *r0 = w, *r1 = w + n, *r2 = w + 2 * n, *r3 = w + 3 * n;
    int32x4_t a0 = vdupq_n_s32(0), a1 = a0, a2 = a0, a3 = a0;
    int32x4_t b0 = a0, b1 = a0, b2 = a0, b3 = a0;
    size_t j = 0;
    for (; j + 64 <= n; j += 64) {
        int8x16_t x0 = vld1q_s8(x + j), x1 = vld1q_s8(x + j + 16);
        int8x16_t x2 = vld1q_s8(x + j + 32), x3 = vld1q_s8(x + j + 48);
        a0 = vdotq_s32(a0, vld1q_s8(r0 + j), x0);
        a1 = vdotq_s32(a1, vld1q_s8(r1 + j), x0);
        a2 = vdotq_s32(a2, vld1q_s8(r2 + j), x0);
        a3 = vdotq_s32(a3, vld1q_s8(r3 + j), x0);
        b0 = vdotq_s32(b0, vld1q_s8(r0 + j + 16), x1);
        b1 = vdotq_s32(b1, vld1q_s8(r1 + j + 16), x1);
        b2 = vdotq_s32(b2, vld1q_s8(r2 + j + 16), x1);
        b3 = vdotq_s32(b3, vld1q_s8(r3 + j + 16), x1);
        a0 = vdotq_s32(a0, vld1q_s8(r0 + j + 32), x2);
        a1 = vdotq_s32(a1, vld1q_s8(r1 + j + 32), x2);
        a2 = vdotq_s32(a2, vld1q_s8(r2 + j + 32), x2);
        a3 = vdotq_s32(a3, vld1q_s8(r3 + j + 32), x2);
        b0 = vdotq_s32(b0, vld1q_s8(r0 + j + 48), x3);
        b1 = vdotq_s32(b1, vld1q_s8(r1 + j + 48), x3);
        b2 = vdotq_s32(b2, vld1q_s8(r2 + j + 48), x3);
        b3 = vdotq_s32(b3, vld1q_s8(r3 + j + 48), x3);
    }
    for (; j + 16 <= n; j += 16) {
        int8x16_t x0 = vld1q_s8(x + j);
        a0 = vdotq_s32(a0, vld1q_s8(r0 + j), x0);
        a1 = vdotq_s32(a1, vld1q_s8(r1 + j), x0);
        a2 = vdotq_s32(a2, vld1q_s8(r2 + j), x0);
        a3 = vdotq_s32(a3, vld1q_s8(r3 + j), x0);
    }
    y[0] = vaddvq_s32(vaddq_s32(a0, b0)) + tail_dot(r0, x, j, n);
    y[1] = vaddvq_s32(vaddq_s32(a1, b1)) + tail_dot(r1, x, j, n);
    y[2] = vaddvq_s32(vaddq_s32(a2, b2)) + tail_dot(r2, x, j, n);
    y[3] = vaddvq_s32(vaddq_s32(a3, b3)) + tail_dot(r3, x, j, n);
}

static int32_t sdot_row1(const int8_t *r, size_t n, const int8_t *x) {
    int32x4_t a0 = vdupq_n_s32(0), a1 = a0, a2 = a0, a3 = a0;
    size_t j = 0;
    for (; j + 64 <= n; j += 64) {
        a0 = vdotq_s32(a0, vld1q_s8(r + j), vld1q_s8(x + j));
        a1 = vdotq_s32(a1, vld1q_s8(r + j + 16), vld1q_s8(x + j + 16));
        a2 = vdotq_s32(a2, vld1q_s8(r + j + 32), vld1q_s8(x + j + 32));
        a3 = vdotq_s32(a3, vld1q_s8(r + j + 48), vld1q_s8(x + j + 48));
    }
    for (; j + 16 <= n; j += 16) a0 = vdotq_s32(a0, vld1q_s8(r + j), vld1q_s8(x + j));
    return vaddvq_s32(vaddq_s32(vaddq_s32(a0, a1), vaddq_s32(a2, a3))) + tail_dot(r, x, j, n);
}

static int run_sdot(const oma_rz_plan *p, const int8_t *x, int32_t *y) {
    if (!p || !p->mem || !x || !y) return OMA_RZ_E_ARG;
    const int8_t *w = p->mem;
    size_t m = p->m, n = p->n, i = 0;
    for (; i + 4 <= m; i += 4) sdot_rows4(w + i * n, n, x, y + i);
    for (; i < m; i++) y[i] = sdot_row1(w + i * n, n, x);
    return OMA_RZ_OK;
}

/* ---- R1_sdot_il: 4-row interleaved tiles ----
 * Rows padded to a multiple of 4, columns to a multiple of 16 (np). Tile q
 * (rows 4q..4q+3), 16-column chunk c: 64 contiguous bytes
 *   [r0 c..c+15][r1 c..c+15][r2 c..c+15][r3 c..c+15]
 * so one tile is a single sequential stream; x is loaded once per tile. */
static int pack_il(oma_rz_plan *p, const int8_t *w, size_t m, size_t n) {
    int rc = oma_rz_check_shape(m, n, OMA_RZ_MAX_N);
    if (rc) return rc;
    if (!p || !w) return OMA_RZ_E_ARG;
    rc = oma_rz_validate(w, m, n);
    if (rc) return rc;
    memset(p, 0, sizeof *p);
    size_t np = (n + 15u) & ~(size_t)15u, mp = (m + 3u) & ~(size_t)3u;
    int8_t *buf = oma_rz_alloc(mp * np);
    if (!buf) return OMA_RZ_E_NOMEM;
    uint64_t nnz = 0;
    for (size_t row = 0; row < m; row++) {
        size_t q = row / 4, r = row % 4;
        for (size_t col = 0; col < n; col++) {
            int8_t v = w[row * n + col];
            buf[q * 4 * np + (col / 16) * 64 + r * 16 + (col % 16)] = v;
            nnz += (v != 0);
        }
    }
    p->m = m;
    p->n = n;
    p->mem = buf;
    p->weight_bytes = mp * np;
    p->footprint_bytes = mp * np;
    p->nnz = nnz;
    return OMA_RZ_OK;
}

static int run_il(const oma_rz_plan *p, const int8_t *x, int32_t *y) {
    if (!p || !p->mem || !x || !y) return OMA_RZ_E_ARG;
    const int8_t *w = p->mem;
    size_t m = p->m, n = p->n;
    size_t np = (n + 15u) & ~(size_t)15u, mp = (m + 3u) & ~(size_t)3u;
    size_t nfull = n & ~(size_t)15u;
    int8_t xt[16] = {0};
    if (nfull < n) memcpy(xt, x + nfull, n - nfull);
    for (size_t q = 0; q < mp / 4; q++) {
        const int8_t *t = w + q * 4 * np;
        int32x4_t a0 = vdupq_n_s32(0), a1 = a0, a2 = a0, a3 = a0;
        int32x4_t b0 = a0, b1 = a0, b2 = a0, b3 = a0;
        size_t c = 0;
        for (; c + 32 <= nfull; c += 32) {
            const int8_t *u = t + c * 4;
            int8x16_t x0 = vld1q_s8(x + c), x1 = vld1q_s8(x + c + 16);
            a0 = vdotq_s32(a0, vld1q_s8(u), x0);
            a1 = vdotq_s32(a1, vld1q_s8(u + 16), x0);
            a2 = vdotq_s32(a2, vld1q_s8(u + 32), x0);
            a3 = vdotq_s32(a3, vld1q_s8(u + 48), x0);
            b0 = vdotq_s32(b0, vld1q_s8(u + 64), x1);
            b1 = vdotq_s32(b1, vld1q_s8(u + 80), x1);
            b2 = vdotq_s32(b2, vld1q_s8(u + 96), x1);
            b3 = vdotq_s32(b3, vld1q_s8(u + 112), x1);
        }
        for (; c < np; c += 16) {
            const int8_t *u = t + c * 4;
            int8x16_t x0 = c < nfull ? vld1q_s8(x + c) : vld1q_s8(xt);
            a0 = vdotq_s32(a0, vld1q_s8(u), x0);
            a1 = vdotq_s32(a1, vld1q_s8(u + 16), x0);
            a2 = vdotq_s32(a2, vld1q_s8(u + 32), x0);
            a3 = vdotq_s32(a3, vld1q_s8(u + 48), x0);
        }
        /* pairwise reduce 4 rows into one vector: lanes = rows */
        int32x4_t s01 = vpaddq_s32(vaddq_s32(a0, b0), vaddq_s32(a1, b1));
        int32x4_t s23 = vpaddq_s32(vaddq_s32(a2, b2), vaddq_s32(a3, b3));
        int32x4_t s = vpaddq_s32(s01, s23);
        size_t r0 = 4 * q;
        if (r0 + 4 <= m) vst1q_s32(y + r0, s);
        else {
            int32_t tmp[4];
            vst1q_s32(tmp, s);
            memcpy(y + r0, tmp, (m - r0) * sizeof(int32_t));
        }
    }
    return OMA_RZ_OK;
}

/* ---- R1_smmla ----
 * Layout: rows padded to an even count, columns to a multiple of 16
 * (np). For row pair (2q, 2q+1) and 16-column chunk c, 32 bytes:
 *   [r0 c..c+7][r1 c..c+7][r0 c+8..c+15][r1 c+8..c+15]
 * With X = [x c..c+7 | x c+8..c+15]:
 *   A = smmla(A, piece0, X): A[0][0] = r0[lo].x[lo], A[1][0] = r1[lo].x[lo]
 *   B = smmla(B, piece1, X): B[0][1] = r0[hi].x[hi], B[1][1] = r1[hi].x[hi]
 * int32x4 lanes are (row, col) = (0,0),(0,1),(1,0),(1,1).
 * y0 = A.lane0 + B.lane1, y1 = A.lane2 + B.lane3. */
static int pack_smmla(oma_rz_plan *p, const int8_t *w, size_t m, size_t n) {
    int rc = oma_rz_check_shape(m, n, OMA_RZ_MAX_N);
    if (rc) return rc;
    if (!p || !w) return OMA_RZ_E_ARG;
    rc = oma_rz_validate(w, m, n);
    if (rc) return rc;
    memset(p, 0, sizeof *p);
    size_t np = (n + 15u) & ~(size_t)15u, mp = (m + 1u) & ~(size_t)1u;
    int8_t *buf = oma_rz_alloc(mp * np);
    if (!buf) return OMA_RZ_E_NOMEM;
    uint64_t nnz = 0;
    for (size_t q = 0; q < mp / 2; q++) {
        int8_t *dst = buf + q * 2 * np;
        for (size_t c = 0; c < np; c += 16) {
            int8_t *blk = dst + c * 2;
            for (int half = 0; half < 2; half++)
                for (int r = 0; r < 2; r++) {
                    size_t row = 2 * q + (size_t)r;
                    for (int k = 0; k < 8; k++) {
                        size_t col = c + (size_t)half * 8 + (size_t)k;
                        int8_t v = (row < m && col < n) ? w[row * n + col] : 0;
                        blk[half * 16 + r * 8 + k] = v;
                        nnz += (v != 0);
                    }
                }
        }
    }
    p->m = m;
    p->n = n;
    p->mem = buf;
    p->stride = (int)0;
    p->weight_bytes = mp * np;
    p->footprint_bytes = mp * np;
    p->nnz = nnz;
    return OMA_RZ_OK;
}

static int run_smmla(const oma_rz_plan *p, const int8_t *x, int32_t *y) {
    if (!p || !p->mem || !x || !y) return OMA_RZ_E_ARG;
    const int8_t *w = p->mem;
    size_t m = p->m, n = p->n;
    size_t np = (n + 15u) & ~(size_t)15u, mp = (m + 1u) & ~(size_t)1u;
    size_t nfull = n & ~(size_t)15u;
    int8_t xt[16] = {0};
    if (nfull < n) memcpy(xt, x + nfull, n - nfull);
    size_t q = 0;
    /* two row pairs at a time: 4 rows, 4 accumulators */
    for (; q + 2 <= mp / 2; q += 2) {
        const int8_t *p0 = w + q * 2 * np, *p1 = w + (q + 1) * 2 * np;
        int32x4_t A0 = vdupq_n_s32(0), B0 = A0, A1 = A0, B1 = A0;
        int32x4_t C0 = A0, D0 = A0, C1 = A0, D1 = A0;
        size_t c = 0;
        for (; c + 32 <= nfull; c += 32) {
            int8x16_t X0 = vld1q_s8(x + c), X1 = vld1q_s8(x + c + 16);
            A0 = vmmlaq_s32(A0, vld1q_s8(p0 + 2 * c), X0);
            B0 = vmmlaq_s32(B0, vld1q_s8(p0 + 2 * c + 16), X0);
            A1 = vmmlaq_s32(A1, vld1q_s8(p1 + 2 * c), X0);
            B1 = vmmlaq_s32(B1, vld1q_s8(p1 + 2 * c + 16), X0);
            C0 = vmmlaq_s32(C0, vld1q_s8(p0 + 2 * c + 32), X1);
            D0 = vmmlaq_s32(D0, vld1q_s8(p0 + 2 * c + 48), X1);
            C1 = vmmlaq_s32(C1, vld1q_s8(p1 + 2 * c + 32), X1);
            D1 = vmmlaq_s32(D1, vld1q_s8(p1 + 2 * c + 48), X1);
        }
        for (; c < np; c += 16) {
            int8x16_t X0 = c < nfull ? vld1q_s8(x + c) : vld1q_s8(xt);
            A0 = vmmlaq_s32(A0, vld1q_s8(p0 + 2 * c), X0);
            B0 = vmmlaq_s32(B0, vld1q_s8(p0 + 2 * c + 16), X0);
            A1 = vmmlaq_s32(A1, vld1q_s8(p1 + 2 * c), X0);
            B1 = vmmlaq_s32(B1, vld1q_s8(p1 + 2 * c + 16), X0);
        }
        A0 = vaddq_s32(A0, C0);
        B0 = vaddq_s32(B0, D0);
        A1 = vaddq_s32(A1, C1);
        B1 = vaddq_s32(B1, D1);
        size_t r = 2 * q;
        y[r] = vgetq_lane_s32(A0, 0) + vgetq_lane_s32(B0, 1);
        y[r + 1] = vgetq_lane_s32(A0, 2) + vgetq_lane_s32(B0, 3);
        if (r + 2 < m) y[r + 2] = vgetq_lane_s32(A1, 0) + vgetq_lane_s32(B1, 1);
        if (r + 3 < m) y[r + 3] = vgetq_lane_s32(A1, 2) + vgetq_lane_s32(B1, 3);
    }
    for (; q < mp / 2; q++) {
        const int8_t *p0 = w + q * 2 * np;
        int32x4_t A0 = vdupq_n_s32(0), B0 = A0;
        for (size_t c = 0; c < np; c += 16) {
            int8x16_t X0 = c < nfull ? vld1q_s8(x + c) : vld1q_s8(xt);
            A0 = vmmlaq_s32(A0, vld1q_s8(p0 + 2 * c), X0);
            B0 = vmmlaq_s32(B0, vld1q_s8(p0 + 2 * c + 16), X0);
        }
        size_t r = 2 * q;
        y[r] = vgetq_lane_s32(A0, 0) + vgetq_lane_s32(B0, 1);
        if (r + 1 < m) y[r + 1] = vgetq_lane_s32(A0, 2) + vgetq_lane_s32(B0, 3);
    }
    return OMA_RZ_OK;
}

const oma_rz_impl oma_rz_r1_plain = {
    "R1_plain", "int8 W, plain C loop (-O2), weak baseline", "binary", 1, 1,
    OMA_RZ_MAX_N, pack_rowmajor, run_plain};
const oma_rz_impl oma_rz_r1_sdot = {
    "R1_sdot", "int8 W, NEON SDOT 4 rows x 64 B, 8 accumulators", "binary", 1, 0,
    OMA_RZ_MAX_N, pack_rowmajor, run_sdot};
const oma_rz_impl oma_rz_r1_smmla = {
    "R1_smmla", "int8 W, I8MM SMMLA row-pair interleaved (half of each 2x2 unused)", "binary", 1, 0,
    OMA_RZ_MAX_N, pack_smmla, run_smmla};
const oma_rz_impl oma_rz_r1_sdot_il = {
    "R1_sdot_il", "int8 W in 4-row interleaved tiles (one stream), NEON SDOT, 8 accumulators", "binary", 1, 0,
    OMA_RZ_MAX_N, pack_il, run_il};
