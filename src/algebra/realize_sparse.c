/* MA-3 R3: sparse ternary. Per row, a list of +1 column indices followed by a
 * list of -1 column indices (uint16, so n <= 65536); row offsets uint32.
 * y_i = sum x[pos idx] - sum x[neg idx], scalar gather with 4 accumulators
 * (NEON has no gather; SVE gather at VL=128 is 4 lanes and not faster than
 * scalar loads here). Weight bytes read per call = 2 * nnz + 4 * (2m + 1). */
#include "algebra/realize_common.h"

#include <stdlib.h>
#include <string.h>

#define SP_MAX_N ((size_t)65536u)

static int pack_sparse(oma_rz_plan *p, const int8_t *w, size_t m, size_t n) {
    int rc = oma_rz_check_shape(m, n, SP_MAX_N);
    if (rc) return rc;
    if (!p || !w) return OMA_RZ_E_ARG;
    rc = oma_rz_validate(w, m, n);
    if (rc) return rc;
    memset(p, 0, sizeof *p);
    uint64_t nnz = 0;
    for (size_t i = 0; i < m * n; i++) nnz += (w[i] != 0);
    if (nnz > UINT32_MAX) return OMA_RZ_E_OVERFLOW;
    uint16_t *idx = oma_rz_alloc((size_t)nnz * sizeof(uint16_t));
    uint32_t *off = oma_rz_alloc((2u * m + 1u) * sizeof(uint32_t));
    if (!idx || !off) {
        free(idx);
        free(off);
        return OMA_RZ_E_NOMEM;
    }
    uint32_t k = 0;
    for (size_t i = 0; i < m; i++) {
        const int8_t *row = w + i * n;
        off[2 * i] = k;
        for (size_t j = 0; j < n; j++)
            if (row[j] > 0) idx[k++] = (uint16_t)j;
        off[2 * i + 1] = k;
        for (size_t j = 0; j < n; j++)
            if (row[j] < 0) idx[k++] = (uint16_t)j;
    }
    off[2 * m] = k;
    p->m = m;
    p->n = n;
    p->mem = idx;
    p->aux = off;
    p->nnz = nnz;
    p->weight_bytes = (size_t)nnz * 2u + (2u * m + 1u) * 4u;
    p->footprint_bytes = p->weight_bytes;
    return OMA_RZ_OK;
}

static inline int32_t gather_sum(const int8_t *x, const uint16_t *ix, uint32_t cnt) {
    int32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    uint32_t k = 0;
    for (; k + 8 <= cnt; k += 8) {
        s0 += x[ix[k]] + x[ix[k + 4]];
        s1 += x[ix[k + 1]] + x[ix[k + 5]];
        s2 += x[ix[k + 2]] + x[ix[k + 6]];
        s3 += x[ix[k + 3]] + x[ix[k + 7]];
    }
    for (; k < cnt; k++) s0 += x[ix[k]];
    return (s0 + s1) + (s2 + s3);
}

static int run_sparse(const oma_rz_plan *p, const int8_t *x, int32_t *y) {
    if (!p || !p->mem || !p->aux || !x || !y) return OMA_RZ_E_ARG;
    const uint16_t *idx = p->mem;
    const uint32_t *off = p->aux;
    for (size_t i = 0; i < p->m; i++) {
        uint32_t a = off[2 * i], b = off[2 * i + 1], c = off[2 * i + 2];
        y[i] = gather_sum(x, idx + a, b - a) - gather_sum(x, idx + b, c - b);
    }
    return OMA_RZ_OK;
}

const oma_rz_impl oma_rz_r3_sparse = {
    "R3_sparse", "uint16 +1/-1 index lists per row, scalar gather add/sub", "sparse", 1, 0,
    SP_MAX_N, pack_sparse, run_sparse};
