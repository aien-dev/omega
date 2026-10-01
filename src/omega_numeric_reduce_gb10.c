/*
 * E1 WP-D GB10 realization of the frozen reduction order (SUM only).
 *
 * Each level of OMEGA_REDUCE_DECLARED_ORDER is a tile-of-32 lane tree, which
 * is exactly the chip-proven REDUCE_SUM warp kernel
 * (OMEGA_WARP_REDUCTION_DECLARED_ORDER). The host pads a level with -0.0 to a
 * multiple of 32, launches REDUCE_SUM through the public executor
 * omega_gb10_execute_simt_op in chunks of at most OMEGA_NUMERIC_MAX_COUNT
 * values (multiples of 32, so no tile straddles a chunk), and gathers lane 0
 * of every warp as the next level. The gather moves bits only; every FADD
 * runs on the chip. No new instruction encoding is introduced here.
 *
 * Every pre-submission check carries a CHECK: marker. With
 * -DOMEGA_NUMERIC_CPU_ONLY the checks still run but no device is touched
 * (omega_reduce_gb10 returns OMEGA_NUMERIC_ERR_DEVICE after the checks).
 */
#include "omega_numeric_reduce.h"
#include "omega_numeric.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned g_last_launches;

unsigned omega_reduce_gb10_last_launches(void) { return g_last_launches; }

static int refuse(char *err, size_t err_len, int code, const char *fmt, ...) {
    if (err && err_len) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, err_len, fmt, ap);
        va_end(ap);
    }
    return code;
}

/* Launch plan: per level, padded length and chunk sizes. Returns launches. */
static size_t plan_launches(size_t n, unsigned *levels_out) {
    size_t launches = 0;
    unsigned levels = 0;
    size_t len = n;
    if (n == 0) { *levels_out = 0; return 0; }
    do {
        size_t padded = (len + 31) & ~(size_t)31;
        launches += (padded + OMEGA_NUMERIC_MAX_COUNT - 1) / OMEGA_NUMERIC_MAX_COUNT;
        len = padded / 32;
        levels++;
    } while (len > 1);
    *levels_out = levels;
    return launches;
}

int omega_reduce_gb10_check(OmegaReduceOp op, const float *x, size_t n, const float *out,
                            char *err, size_t err_len) {
    if (err && err_len) err[0] = '\0';
    if ((unsigned)op >= OMEGA_RED_COUNT)
        return refuse(err, err_len, OMEGA_NUMERIC_ERR_BAD_ARGS, "unknown reduction op %d", (int)op); /* CHECK:red_op_known */
    if (op != OMEGA_RED_SUM)
        return refuse(err, err_len, OMEGA_NUMERIC_ERR_NOT_ENCODED,
                      "reduction %s has no GB10 kernel (only SUM: warp FMNMX tree and DIV are not encoded)",
                      omega_reduce_op_name(op)); /* CHECK:red_sum_only */
    if (!out || (n > 0 && !x))
        return refuse(err, err_len, OMEGA_NUMERIC_ERR_BAD_ARGS, "missing input or output buffer"); /* CHECK:red_buffers */
    if (n > OMEGA_REDUCE_GB10_MAX_N)
        return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS, "n=%zu exceeds %zu", n, OMEGA_REDUCE_GB10_MAX_N); /* CHECK:red_max_n */
    if (strcmp(OMEGA_WARP_REDUCTION_DECLARED_ORDER, "PAIRWISE_TREE_LANE_DELTA_16_8_4_2_1") != 0 ||
        strstr(OMEGA_REDUCE_DECLARED_ORDER, OMEGA_WARP_REDUCTION_DECLARED_ORDER) == NULL)
        return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS,
                      "warp kernel order %s is not the tile order of %s",
                      OMEGA_WARP_REDUCTION_DECLARED_ORDER, OMEGA_REDUCE_DECLARED_ORDER); /* CHECK:red_order_string */
    const OmegaNumericOpInfo *info = omega_numeric_op_find("REDUCE_SUM");
    if (!info || info->op != OMEGA_NOP_REDUCE_SUM || !info->gb10_encoded || info->compare != OMEGA_CMP_BIT_EXACT)
        return refuse(err, err_len, OMEGA_NUMERIC_ERR_NOT_ENCODED,
                      "REDUCE_SUM is not a GB10-encoded BIT_EXACT op in the registry"); /* CHECK:red_kernel_registered */
    if (omega_float_to_bits(omega_reduce_identity(op)) != 0x80000000U)
        return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS, "SUM padding is not -0.0"); /* CHECK:red_pad_identity */
    unsigned levels = 0;
    size_t launches = plan_launches(n, &levels);
    if (levels != omega_reduce_levels(n))
        return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS, "launch plan has %u levels, contract says %u",
                      levels, omega_reduce_levels(n)); /* CHECK:red_levels */
    if ((OMEGA_NUMERIC_MAX_COUNT % 32u) != 0)
        return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS, "chunk limit is not whole warps"); /* CHECK:red_chunk_whole_warps */
    (void)launches;
    /* The executor's own structural check (patch words, SHFL deltas 16..1,
     * FADD chain, QMD) for every distinct chunk size this plan submits. */
    size_t len = n;
    char serr[256];
    static float dummy;
    while (len > 0) {
        size_t padded = (len + 31) & ~(size_t)31;
        size_t full = padded / OMEGA_NUMERIC_MAX_COUNT, rest = padded % OMEGA_NUMERIC_MAX_COUNT;
        size_t sizes[2] = { full ? OMEGA_NUMERIC_MAX_COUNT : 0, rest };
        for (int k = 0; k < 2; k++) {
            if (!sizes[k]) continue;
            int rc = omega_numeric_submit_check("REDUCE_SUM", &dummy, &dummy, NULL, &dummy, sizes[k], serr, sizeof(serr));
            if (rc != OMEGA_NUMERIC_OK)
                return refuse(err, err_len, rc, "REDUCE_SUM chunk of %zu refused: %s", sizes[k], serr); /* CHECK:red_submit_each_chunk */
        }
        if (padded == 32) break;
        len = padded / 32;
    }
    return OMEGA_NUMERIC_OK;
}

int omega_reduce_gb10(OmegaReduceOp op, const float *x, size_t n, float *out) {
    char err[320];
    g_last_launches = 0;
    int rc = omega_reduce_gb10_check(op, x, n, out, err, sizeof(err));
    if (rc != OMEGA_NUMERIC_OK) {
        fprintf(stderr, "omega_reduce_gb10: refused before submission: %s\n", err);
        return rc;
    }
    if (n == 0) { *out = omega_bits_to_float(OMEGA_REDUCE_EMPTY_SUM_BITS); return OMEGA_NUMERIC_OK; }
#ifdef OMEGA_NUMERIC_CPU_ONLY
    return OMEGA_NUMERIC_ERR_DEVICE;
#else
    size_t cap = (n + 31) & ~(size_t)31;
    float *cur = malloc(cap * sizeof(float));
    float *res = malloc(OMEGA_NUMERIC_MAX_COUNT * sizeof(float));
    if (!cur || !res) { free(cur); free(res); return OMEGA_NUMERIC_ERR_BAD_ARGS; }
    memcpy(cur, x, n * sizeof(float));
    size_t len = n;
    for (;;) {
        size_t padded = (len + 31) & ~(size_t)31;
        for (size_t i = len; i < padded; i++) cur[i] = omega_bits_to_float(OMEGA_REDUCE_SUM_PAD_BITS);
        size_t next_len = padded / 32;
        for (size_t base = 0; base < padded; base += OMEGA_NUMERIC_MAX_COUNT) {
            size_t count = padded - base;
            if (count > OMEGA_NUMERIC_MAX_COUNT) count = OMEGA_NUMERIC_MAX_COUNT;
            rc = omega_gb10_execute_simt_op("REDUCE_SUM", cur + base, NULL, NULL, res, count);
            g_last_launches++;
            if (rc != OMEGA_NUMERIC_OK) { free(cur); free(res); return rc; }
            /* lane 0 of warp w holds tile (base/32 + w); base/32 + w <= base + 32w */
            for (size_t w = 0; w < count / 32; w++) cur[base / 32 + w] = res[w * 32];
        }
        len = next_len;
        if (len == 1) break;
    }
    *out = cur[0];
    free(cur);
    free(res);
    return OMEGA_NUMERIC_OK;
#endif
}
