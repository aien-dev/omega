/* MA-3 R5: dense 5 trits per byte (1.6 bits per weight), decode then SDOT.
 * Byte rule is the reference dense rule (oma_pack.h): byte = sum_{i<5}
 * (t_i + 1) * 3^i, valid bytes 0..242. Only the assignment of trits to bytes
 * differs ("strided dense"): per row, chunks of 80 weights in 16 bytes; byte j
 * of a chunk holds weights chunk + 16 i + j for i = 0..4. Digit plane i
 * (d_i = t_i + 1 in {0,1,2}) is then directly the 16-lane weight vector for
 * x[chunk + 16 i ..]. Decode: q = b / 3 = (b * 171) >> 9 (exact for b < 256),
 * d = b - 3 q, repeated; d_4 = b / 81. Since d = t + 1:
 *   y = sum_i SDOT(d_i, x_i) - sum_j x_j   (sum x computed once per call;
 *   padding trits are 0 -> d = 1 and meet zero-padded x). */
#include "algebra/realize_common.h"

#include <arm_neon.h>
#include <string.h>

#define D5_CHUNK 80u

static int pack_dense5(oma_rz_plan *p, const int8_t *w, size_t m, size_t n) {
    int rc = oma_rz_check_shape(m, n, OMA_RZ_MAX_N);
    if (rc) return rc;
    if (!p || !w) return OMA_RZ_E_ARG;
    rc = oma_rz_validate(w, m, n);
    if (rc) return rc;
    memset(p, 0, sizeof *p);
    size_t chunks = (n + D5_CHUNK - 1) / D5_CHUNK, row_bytes = chunks * 16u;
    uint8_t *buf = oma_rz_alloc(m * row_bytes);
    if (!buf) return OMA_RZ_E_NOMEM;
    uint64_t nnz = 0;
    for (size_t r = 0; r < m; r++)
        for (size_t c = 0; c < chunks; c++)
            for (unsigned j = 0; j < 16; j++) {
                unsigned byte = 0, pw = 1;
                for (unsigned i = 0; i < 5; i++, pw *= 3u) {
                    size_t col = c * D5_CHUNK + 16u * i + j;
                    int v = col < n ? w[r * n + col] : 0;
                    nnz += (v != 0);
                    byte += (unsigned)(v + 1) * pw;
                }
                buf[r * row_bytes + c * 16u + j] = (uint8_t)byte;
            }
    p->m = m;
    p->n = n;
    p->mem = buf;
    p->weight_bytes = m * row_bytes;
    p->footprint_bytes = m * row_bytes;
    p->nnz = nnz;
    return OMA_RZ_OK;
}

static inline uint8x16_t div3(uint8x16_t b) {
    const uint8x16_t k171 = vdupq_n_u8(171);
    uint16x8_t lo = vmull_u8(vget_low_u8(b), vget_low_u8(k171));
    uint16x8_t hi = vmull_high_u8(b, k171);
    uint8x16_t h8 = vuzp2q_u8(vreinterpretq_u8_u16(lo), vreinterpretq_u8_u16(hi)); /* >> 8 */
    return vshrq_n_u8(h8, 1);                                                         /* >> 9 */
}

static int run_dense5(const oma_rz_plan *p, const int8_t *x, int32_t *y) {
    if (!p || !p->mem || !x || !y) return OMA_RZ_E_ARG;
    size_t m = p->m, n = p->n;
    size_t chunks = (n + D5_CHUNK - 1) / D5_CHUNK, row_bytes = chunks * 16u;
    size_t nfull = (n / D5_CHUNK) * D5_CHUNK;
    int8_t xt[D5_CHUNK];
    memset(xt, 0, sizeof xt);
    if (nfull < n) memcpy(xt, x + nfull, n - nfull);
    int32_t sumx = 0;
    for (size_t j = 0; j < n; j++) sumx += x[j];
    const uint8_t *w = p->mem;
    const uint8x16_t three = vdupq_n_u8(3);
    for (size_t r = 0; r < m; r++) {
        const uint8_t *row = w + r * row_bytes;
        int32x4_t a0 = vdupq_n_s32(0), a1 = a0;
        for (size_t c = 0; c < chunks; c++) {
            const int8_t *xc = (c * D5_CHUNK < nfull) ? x + c * D5_CHUNK : xt;
            uint8x16_t b = vld1q_u8(row + c * 16u);
            uint8x16_t q1 = div3(b);
            uint8x16_t d0 = vmlsq_u8(b, q1, three);
            uint8x16_t q2 = div3(q1);
            uint8x16_t d1 = vmlsq_u8(q1, q2, three);
            uint8x16_t q3 = div3(q2);
            uint8x16_t d2 = vmlsq_u8(q2, q3, three);
            uint8x16_t q4 = div3(q3);
            uint8x16_t d3 = vmlsq_u8(q3, q4, three);
            a0 = vdotq_s32(a0, vreinterpretq_s8_u8(d0), vld1q_s8(xc));
            a1 = vdotq_s32(a1, vreinterpretq_s8_u8(d1), vld1q_s8(xc + 16));
            a0 = vdotq_s32(a0, vreinterpretq_s8_u8(d2), vld1q_s8(xc + 32));
            a1 = vdotq_s32(a1, vreinterpretq_s8_u8(d3), vld1q_s8(xc + 48));
            a0 = vdotq_s32(a0, vreinterpretq_s8_u8(q4), vld1q_s8(xc + 64));
        }
        y[r] = vaddvq_s32(vaddq_s32(a0, a1)) - sumx;
    }
    return OMA_RZ_OK;
}

const oma_rz_impl oma_rz_r5_dense5 = {
    "R5_dense5", "5 trits/byte (1.6 b/w), NEON divide-by-3 decode then SDOT", "dense5", 1, 0,
    OMA_RZ_MAX_N, pack_dense5, run_dense5};
