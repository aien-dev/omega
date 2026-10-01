/*
 * GB10 realization of M20 OMEGA_TENSOR (docs/tensor/M20_OMEGA_TENSOR.md,
 * declared in omega_tensor.h). Every arithmetic step runs on the GB10 through
 * an E1 entry point; this file only maps ops, checks, pads and moves data.
 *
 *   elementwise  omega_gb10_execute_simt_op (E1 SIMT ops), chunks of at most
 *                OMEGA_NUMERIC_MAX_COUNT elements. The op map below lists only
 *                the ops the tensor layer submits AND that have a GB10 E1
 *                kernel; every other op is refused with
 *                OMEGA_NUMERIC_ERR_NOT_ENCODED before a device is opened.
 *   reduce       omega_reduce_gb10 (E1 WP-D), n <= OMEGA_REDUCE_GB10_MAX_N,
 *                after the declared-order strcmp the CPU seam also makes.
 *   reduce_rows  SUM: every tree level of every row in one pass of E1
 *                REDUCE_SUM warp launches (each row padded with -0.0 to whole
 *                tiles of 32, lane 0 of each warp gathered on the host: data
 *                movement only). Same patch words and order as
 *                omega_reduce_gb10 SUM. MAX/MIN/MEAN: omega_reduce_gb10 per row.
 *
 * There is no CPU fallback: this file never calls omega_numeric_cpu_realize,
 * omega_reduce_cpu or any reference tier (mk/tensor.mk checks with nm).
 * Matmul is the semantic layer's FMUL_RNE products + the E1 tree, so it is
 * bit-exact with the CPU table. A Tensor Core matmul is not provided: MMA
 * accumulates with fused, differently ordered additions and would need its
 * own declared bounded contract (doc row stays MISSING_IMPLEMENTATION).
 */
#include "omega_tensor.h"

#include <stdlib.h>
#include <string.h>

#include "omega_numeric.h"
#include "omega_numeric_reduce.h"

_Static_assert(sizeof(OMEGA_REDUCE_DECLARED_ORDER) == sizeof(OMEGA_TENSOR_REDUCE_DECLARED_ORDER),
               "tensor reduce order must equal the E1 reduction order");
_Static_assert(OMEGA_NUMERIC_MAX_COUNT % 32u == 0, "chunks must be whole warps");

/* Ops the semantic layer submits (omega_tensor.c: unary SQRT, BINARY_MAP,
 * fma FFMA_V, matmul FMUL, cast conversions) -> E1 GB10 op name. Ops that
 * are absent (EXP, LOG, uniform-c FFMA, MUFU seeds, LDS/SHFL, ...) are
 * refused. */
static const char *const GB10_OP_NAME[OMEGA_NOP_COUNT] = {
    [OMEGA_NOP_FADD] = "FADD",               [OMEGA_NOP_FSUB] = "FSUB",
    [OMEGA_NOP_FMUL] = "FMUL",               [OMEGA_NOP_DIV] = "DIV",
    [OMEGA_NOP_SQRT] = "SQRT",               [OMEGA_NOP_FFMA_V] = "FFMA_V",
    [OMEGA_NOP_FMNMX_MIN] = "FMNMX_MIN",     [OMEGA_NOP_FMNMX_MAX] = "FMNMX_MAX",
    [OMEGA_NOP_FSETP_SEL] = "FSETP_SEL",     [OMEGA_NOP_FSETP_LT_SEL] = "FSETP_LT_SEL",
    [OMEGA_NOP_FSETP_LE_SEL] = "FSETP_LE_SEL", [OMEGA_NOP_FSETP_GT_SEL] = "FSETP_GT_SEL",
    [OMEGA_NOP_FSETP_EQ_SEL] = "FSETP_EQ_SEL", [OMEGA_NOP_FSETP_NE_SEL] = "FSETP_NE_SEL",
    [OMEGA_NOP_FSETP_NUM_SEL] = "FSETP_NUM_SEL", [OMEGA_NOP_FSETP_NAN_SEL] = "FSETP_NAN_SEL",
    [OMEGA_NOP_FSETP_LTU_SEL] = "FSETP_LTU_SEL", [OMEGA_NOP_FSETP_LEU_SEL] = "FSETP_LEU_SEL",
    [OMEGA_NOP_FSETP_GTU_SEL] = "FSETP_GTU_SEL", [OMEGA_NOP_FSETP_GEU_SEL] = "FSETP_GEU_SEL",
    [OMEGA_NOP_FSETP_EQU_SEL] = "FSETP_EQU_SEL", [OMEGA_NOP_FSETP_NEU_SEL] = "FSETP_NEU_SEL",
    [OMEGA_NOP_F32_TO_F16] = "F32_TO_F16",   [OMEGA_NOP_F32_TO_BF16] = "F32_TO_BF16",
    [OMEGA_NOP_F16_TO_F32] = "F16_TO_F32",   [OMEGA_NOP_BF16_TO_F32] = "BF16_TO_F32",
};

const char *omega_tensor_gb10_op_name(OmegaNumericOp op) {
    if ((unsigned)op >= OMEGA_NOP_COUNT) return NULL;
    return GB10_OP_NAME[op];
}

static int gb10_elementwise(OmegaNumericOp op, const float *a, const float *b, const float *c,
                            float *out, size_t n) {
    const char *name = omega_tensor_gb10_op_name(op);
    if (!name) return OMEGA_NUMERIC_ERR_NOT_ENCODED;   /* no silent CPU fallback; MUT:GB10_NO_CPU_FALLBACK */
    const OmegaNumericOpInfo *info = omega_numeric_op_find(name);
    if (!info || info->op != op || !info->gb10_encoded) return OMEGA_NUMERIC_ERR_NOT_ENCODED;
    if (!a || !out || n == 0) return OMEGA_NUMERIC_ERR_BAD_ARGS;
    for (size_t base = 0; base < n; base += OMEGA_NUMERIC_MAX_COUNT) {
        size_t cnt = n - base < OMEGA_NUMERIC_MAX_COUNT ? n - base : OMEGA_NUMERIC_MAX_COUNT;
        int rc = omega_gb10_execute_simt_op(name, a + base, b ? b + base : NULL, c ? c + base : NULL,
                                            out + base, cnt);
        if (rc != OMEGA_NUMERIC_OK) return rc;
    }
    return OMEGA_NUMERIC_OK;
}

static int map_reduce_op(OmegaTensorReduceOp op, OmegaReduceOp *r) {
    switch (op) {
    case OMEGA_TR_SUM:  *r = OMEGA_RED_SUM;  return OMEGA_NUMERIC_OK;
    case OMEGA_TR_MAX:  *r = OMEGA_RED_MAX;  return OMEGA_NUMERIC_OK;
    case OMEGA_TR_MIN:  *r = OMEGA_RED_MIN;  return OMEGA_NUMERIC_OK;
    case OMEGA_TR_MEAN: *r = OMEGA_RED_MEAN; return OMEGA_NUMERIC_OK;
    default: return OMEGA_NUMERIC_ERR_BAD_ARGS;
    }
}

static int gb10_reduce(OmegaTensorReduceOp op, const float *x, size_t n, float *out) {
    if (strcmp(OMEGA_REDUCE_DECLARED_ORDER, OMEGA_TENSOR_REDUCE_DECLARED_ORDER) != 0)
        return OMEGA_NUMERIC_ERR_BAD_ARGS;
    /* size limit first, so it is refused even before the buffers are looked at */
    if (n > OMEGA_REDUCE_GB10_MAX_N) return OMEGA_NUMERIC_ERR_OPERANDS;   /* MUT:GB10_REDUCE_MAX_N */
    OmegaReduceOp r;
    if (map_reduce_op(op, &r) != OMEGA_NUMERIC_OK || !x || !out || n == 0) return OMEGA_NUMERIC_ERR_BAD_ARGS;
    return omega_reduce_gb10(r, x, n, out);
}

/* SUM of rows rows of n values, all rows per level in shared launches.
 * Level: each row's len values are padded with -0.0 to a whole number of
 * 32-tiles; REDUCE_SUM leaves tile t's warp-tree sum in lane 0 of warp t. */
static int gb10_sum_rows(const float *x, size_t rows, size_t n, float *out) {
    size_t padded = (n + 31u) & ~(size_t)31u;
    if (rows > SIZE_MAX / padded) return OMEGA_NUMERIC_ERR_OPERANDS;
    float *cur = malloc(rows * n * sizeof(float));
    float *lvl = malloc(rows * padded * sizeof(float));
    float *res = malloc(rows * padded * sizeof(float));
    int rc = OMEGA_NUMERIC_OK;
    if (!cur || !lvl || !res) { rc = OMEGA_NUMERIC_ERR_BAD_ARGS; goto done; }
    memcpy(cur, x, rows * n * sizeof(float));
    const float pad = omega_bits_to_float(OMEGA_REDUCE_SUM_PAD_BITS);
    size_t len = n;
    do {                                   /* at least one level, as declared */
        size_t pl = (len + 31u) & ~(size_t)31u, tiles = pl / 32u, total = rows * pl;
        for (size_t r = 0; r < rows; r++) {
            memcpy(lvl + r * pl, cur + r * len, len * sizeof(float));
            for (size_t i = len; i < pl; i++) lvl[r * pl + i] = pad;
        }
        for (size_t base = 0; base < total && rc == OMEGA_NUMERIC_OK; base += OMEGA_NUMERIC_MAX_COUNT) {
            size_t cnt = total - base < OMEGA_NUMERIC_MAX_COUNT ? total - base : OMEGA_NUMERIC_MAX_COUNT;
            rc = omega_gb10_execute_simt_op("REDUCE_SUM", lvl + base, NULL, NULL, res + base, cnt);
        }
        if (rc != OMEGA_NUMERIC_OK) goto done;
        for (size_t r = 0; r < rows; r++)
            for (size_t t = 0; t < tiles; t++) cur[r * tiles + t] = res[r * pl + t * 32u];
        len = tiles;
    } while (len > 1);
    memcpy(out, cur, rows * sizeof(float));
done:
    free(cur);
    free(lvl);
    free(res);
    return rc;
}

static int gb10_reduce_rows(OmegaTensorReduceOp op, const float *x, size_t rows, size_t n, float *out) {
    if (strcmp(OMEGA_REDUCE_DECLARED_ORDER, OMEGA_TENSOR_REDUCE_DECLARED_ORDER) != 0)
        return OMEGA_NUMERIC_ERR_BAD_ARGS;
    if (n > OMEGA_REDUCE_GB10_MAX_N) return OMEGA_NUMERIC_ERR_OPERANDS;   /* MUT:GB10_ROWS_MAX_N */
    OmegaReduceOp r;
    if (map_reduce_op(op, &r) != OMEGA_NUMERIC_OK || !x || !out || n == 0 || rows == 0)
        return OMEGA_NUMERIC_ERR_BAD_ARGS;
    if (op == OMEGA_TR_SUM) return gb10_sum_rows(x, rows, n, out);
    for (size_t i = 0; i < rows; i++) {
        int rc = omega_reduce_gb10(r, x + i * n, n, &out[i]);
        if (rc != OMEGA_NUMERIC_OK) return rc;
    }
    return OMEGA_NUMERIC_OK;
}

static const OmegaTensorRealization GB10_REALIZATION = {
    .name = "GB10_E1_SIMT",
    .reduce_order = OMEGA_TENSOR_REDUCE_DECLARED_ORDER,
    .elementwise = gb10_elementwise,
    .reduce = gb10_reduce,
    .reduce_rows = gb10_reduce_rows,
};

const OmegaTensorRealization *omega_tensor_gb10_realization(void) { return &GB10_REALIZATION; }
