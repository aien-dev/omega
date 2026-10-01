#include "omega_tensor_reduce_seam.h"

#include <stdlib.h>
#include <string.h>

#include "omega_numeric.h"

#ifdef OMEGA_TENSOR_USE_E1_REDUCE
#include "omega_numeric_reduce.h"

_Static_assert(sizeof(OMEGA_REDUCE_DECLARED_ORDER) == sizeof(OMEGA_TENSOR_REDUCE_DECLARED_ORDER),
               "tensor reduce order must equal the E1 reduction order");

int omega_tensor_seam_reduce_cpu(OmegaTensorReduceOp op, const float *x, size_t n, float *out) {
    if (strcmp(OMEGA_REDUCE_DECLARED_ORDER, OMEGA_TENSOR_REDUCE_DECLARED_ORDER) != 0)
        return OMEGA_NUMERIC_ERR_BAD_ARGS;
    OmegaReduceOp r;
    switch (op) {
    case OMEGA_TR_SUM:  r = OMEGA_RED_SUM;  break;
    case OMEGA_TR_MAX:  r = OMEGA_RED_MAX;  break;
    case OMEGA_TR_MIN:  r = OMEGA_RED_MIN;  break;
    case OMEGA_TR_MEAN: r = OMEGA_RED_MEAN; break;
    default: return OMEGA_NUMERIC_ERR_BAD_ARGS;
    }
    if (n == 0) return OMEGA_NUMERIC_ERR_BAD_ARGS;
    return omega_reduce_cpu(r, x, n, out);
}

const char *omega_tensor_seam_reduce_source(void) { return "E1_WP_D_OMEGA_REDUCE_CPU"; }

#else

#define SEAM_SUM_PAD_BITS    0x80000000U  /* -0.0 */ /* MUT:SUM_PAD */
#define SEAM_MINMAX_PAD_BITS 0x7fc00000U  /* quiet NaN */
#define SEAM_MEAN_MAX_N      ((size_t)1 << 24)

/* One 32-wide tile: lane i combines with lane i+d, d = 16, 8, 4, 2, 1. */
static int tile_tree(OmegaNumericOp nop, float v[32]) {
    float tmp[16];
    for (size_t d = 16; d >= 1; d >>= 1) { /* MUT:TREE_DELTAS */
        int rc = omega_numeric_cpu_realize(nop, v, v + d, NULL, tmp, d);
        if (rc != OMEGA_NUMERIC_OK) return rc;
        memcpy(v, tmp, d * sizeof(float));
    }
    return OMEGA_NUMERIC_OK;
}

int omega_tensor_seam_reduce_cpu(OmegaTensorReduceOp op, const float *x, size_t n, float *out) {
    if (!x || !out || n == 0) return OMEGA_NUMERIC_ERR_BAD_ARGS;
    OmegaNumericOp nop;
    uint32_t pad_bits;
    switch (op) {
    case OMEGA_TR_SUM:
    case OMEGA_TR_MEAN: nop = OMEGA_NOP_FADD;      pad_bits = SEAM_SUM_PAD_BITS; break;
    case OMEGA_TR_MAX:  nop = OMEGA_NOP_FMNMX_MAX; pad_bits = SEAM_MINMAX_PAD_BITS; break;
    case OMEGA_TR_MIN:  nop = OMEGA_NOP_FMNMX_MIN; pad_bits = SEAM_MINMAX_PAD_BITS; break;
    default: return OMEGA_NUMERIC_ERR_BAD_ARGS;
    }
    if (op == OMEGA_TR_MEAN && n > SEAM_MEAN_MAX_N) return OMEGA_NUMERIC_ERR_OPERANDS;
    if (!omega_numeric_fpenv_ok()) return OMEGA_NUMERIC_ERR_FPENV;
    float *cur = malloc(n * sizeof(float));
    if (!cur) return OMEGA_NUMERIC_ERR_BAD_ARGS;
    memcpy(cur, x, n * sizeof(float));
    const float pad = omega_bits_to_float(pad_bits);
    size_t len = n;
    int rc = OMEGA_NUMERIC_OK;
    do { /* at least one level */
        size_t tiles = (len + 31u) / 32u;
        for (size_t j = 0; j < tiles && rc == OMEGA_NUMERIC_OK; j++) {
            float v[32];
            for (size_t l = 0; l < 32u; l++) {
                size_t src = 32u * j + l;
                v[l] = src < len ? cur[src] : pad;
            }
            rc = tile_tree(nop, v);
            cur[j] = v[0];
        }
        len = tiles;
    } while (len > 1 && rc == OMEGA_NUMERIC_OK);
    float r = cur[0];
    free(cur);
    if (rc != OMEGA_NUMERIC_OK) return rc;
    if (op == OMEGA_TR_MEAN) {
        float nf = (float)n; /* exact: n <= 2^24 */
        rc = omega_numeric_cpu_realize(OMEGA_NOP_DIV, &r, &nf, NULL, &r, 1);
        if (rc != OMEGA_NUMERIC_OK) return rc;
    }
    *out = r;
    return OMEGA_NUMERIC_OK;
}

const char *omega_tensor_seam_reduce_source(void) { return "LOCAL_E1_CPU_TIER"; }

#endif
