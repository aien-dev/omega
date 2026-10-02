/*
 * E1 WP-D GB10 realization of the frozen reduction order: SUM, MAX, MIN, MEAN.
 *
 * Each level of OMEGA_REDUCE_DECLARED_ORDER is a tile-of-32 lane tree. The
 * host pads a level with the op's identity (SUM/MEAN -0.0, MAX/MIN quiet NaN)
 * to a multiple of 32, launches one warp kernel per chunk of at most
 * OMEGA_NUMERIC_MAX_COUNT values (multiples of 32, so no tile straddles a
 * chunk) and gathers lane 0 of every warp as the next level. The gather moves
 * bits only; every combine runs on the chip.
 *
 *   SUM   the chip-proven REDUCE_SUM patch words of the WP-C table
 *         (omega_numeric_patch_words), checked by omega_numeric_check_patch,
 *         launched by this file's executor (see below).
 *   MAX   this file's warp patch: five SHFL.DOWN (deltas 16, 8, 4, 2, 1,
 *   MIN   clamp 0x1f) each followed by FMNMX R2, R2, R9, !PT (MAX) or PT
 *         (MIN), the last one writing R9, then STG, EXIT. The words are the
 *         registry's chip-proven FMNMX_MAX/FMNMX_MIN form (registers changed)
 *         and the REDUCE_SUM SHFL.DOWN words and control words; every word was
 *         decoded with nvdisasm 13.0 -b SM121 (make test-numeric-reduce-nvdisasm).
 *
 * Every op runs through this file's executor run_chunk: a copy of the vecadd
 * launch of omega_numeric_gb10.c (which this lane does not edit) plus a wait on
 * the QMD release semaphore and a dsb before outputs are read. The shared
 * executor reads after the host marker only, which PR #141 showed can precede
 * the last stores.
 *   MEAN  SUM levels on the chip, then the one final division
 *         omega_math_div(SUM, u2f(n)) on the chip: the whole-program GB10 DIV
 *         kernel of E1 row 7 (omega_ds_gb10_run, one element), bit-identical to
 *         the CPU division. No host step remains.
 *
 * Every pre-submission check carries a CHECK: marker. With
 * -DOMEGA_NUMERIC_CPU_ONLY the checks still run but no device is touched
 * (omega_reduce_gb10 returns OMEGA_NUMERIC_ERR_DEVICE after the checks).
 */
#include "omega_numeric_reduce.h"
#include "omega_numeric.h"
#include "omega_numeric_divsqrt_gb10.h"
#include "omega_blackwell_encoder.h"
#include "omega_blackwell_qmd.h"
#ifndef OMEGA_NUMERIC_CPU_ONLY
#include "omega_blackwell_submit.h"
#include "omega_numeric_native.h"
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

static unsigned g_last_launches;

unsigned omega_reduce_gb10_last_launches(void) { return g_last_launches; }

static int refuse(char *err, size_t err_len, int code, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int refuse(char *err, size_t err_len, int code, const char *fmt, ...) {
    if (err && err_len) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, err_len, fmt, ap);
        va_end(ap);
    }
    return code;
}

/* ---- MAX/MIN warp patch ----------------------------------------------------
 * Register map of the vecadd baseline: R2 = a[i] (LDG, SB4), R6:R7 = &out[i],
 * R9 = stored value. Control words are those of REDUCE_SUM: the first
 * SHFL waits SB4 and sets SB0, later SHFLs set SB0 without waiting, each
 * FMNMX waits SB0 (stall 5; FMNMX is fixed latency like FADD, and the
 * registry's FMNMX ops are followed by a no-wait STG after the same stall). */
#define RED_CTRL_SHFL_FIRST 0x010e2800U
#define RED_CTRL_SHFL_NEXT  0x000e2400U
#define RED_CTRL_FMNMX_SB0  0x001fca00U
#define RED_FMNMX_W2_MIN    0x03800000U /* predicate PT  -> min */
#define RED_FMNMX_W2_MAX    0x07800000U /* predicate !PT -> max */

int omega_reduce_gb10_minmax_patch(OmegaReduceOp op, OmegaNumericPatchInsn out[OMEGA_NUMERIC_PATCH_MAX]) {
    if (op != OMEGA_RED_MAX && op != OMEGA_RED_MIN) return OMEGA_NUMERIC_ERR_NOT_ENCODED;
    static const uint32_t SHFL_W1[5] = { 0x0a001f00U, 0x09001f00U, 0x08801f00U, 0x08401f00U, 0x08201f00U };
    static const char *const SHFL_TXT[5] = {
        "SHFL.DOWN PT, R9, R2, 0x10, 0x1f", "SHFL.DOWN PT, R9, R2, 0x8, 0x1f", "SHFL.DOWN PT, R9, R2, 0x4, 0x1f",
        "SHFL.DOWN PT, R9, R2, 0x2, 0x1f", "SHFL.DOWN PT, R9, R2, 0x1, 0x1f" };
    const bool mx = (op == OMEGA_RED_MAX);
    int n = 0;
    for (int s = 0; s < 5; s++) {
        out[n].w[0] = 0x02097f89U; out[n].w[1] = SHFL_W1[s]; out[n].w[2] = 0x000e0000U;
        out[n].w[3] = s == 0 ? RED_CTRL_SHFL_FIRST : RED_CTRL_SHFL_NEXT;
        out[n].text = SHFL_TXT[s]; out[n].provenance_key = "RED_MINMAX_SHFL_DOWN"; n++;
        out[n].w[0] = s == 4 ? 0x02097209U : 0x02027209U; out[n].w[1] = 0x00000009U;
        out[n].w[2] = mx ? RED_FMNMX_W2_MAX : RED_FMNMX_W2_MIN; out[n].w[3] = RED_CTRL_FMNMX_SB0;
        out[n].text = s == 4 ? (mx ? "FMNMX R9, R2, R9, !PT" : "FMNMX R9, R2, R9, PT")
                             : (mx ? "FMNMX R2, R2, R9, !PT" : "FMNMX R2, R2, R9, PT");
        out[n].provenance_key = "RED_MINMAX_FMNMX"; n++;
    }
    static const OmegaNumericPatchInsn STG = { { 0x06007986U, 0x00000009U, 0x0c101904U, 0x000fe200U },
                                               "STG.E desc[UR4][R6.64], R9", NULL };
    static const OmegaNumericPatchInsn EXIT = { { 0x0000794dU, 0x00000000U, 0x03800000U, 0x000fea00U }, "EXIT", NULL };
    out[n++] = STG;
    out[n++] = EXIT;
    return n;
}

#define P_OP(p)   ((p).w[0] & 0xffffu)
#define P_DST(p)  (((p).w[0] >> 16) & 0xffu)
#define P_SRCA(p) (((p).w[0] >> 24) & 0xffu)
#define P_SRCB(p) ((p).w[1] & 0xffu)
#define P_WBAR(p) (((p).w[3] >> 14) & 7u)
#define P_WAIT(p) (((p).w[3] >> 20) & 0x3fu)
#define P_SHFL_DELTA(p) (((p).w[1] >> 21) & 0x1fu)

int omega_reduce_gb10_check_minmax_patch(OmegaReduceOp op, const OmegaNumericPatchInsn *p, int n,
                                         const uint32_t *qmd1, char *err, size_t err_len) {
    if (err && err_len) err[0] = '\0';
    if (op != OMEGA_RED_MAX && op != OMEGA_RED_MIN)
        return refuse(err, err_len, OMEGA_NUMERIC_ERR_BAD_ARGS, "minmax patch check: op %d is not MAX or MIN", (int)op); /* CHECK:mm_op */
    /* Generic rules of the numeric executor (length, scoreboard waits, SHFL.DOWN
     * form and clamp, STG form) under the matching single-FMNMX op. */
    int rc = omega_numeric_check_patch(op == OMEGA_RED_MAX ? OMEGA_NOP_FMNMX_MAX : OMEGA_NOP_FMNMX_MIN,
                                       p, n, qmd1, err, err_len);
    if (rc != OMEGA_NUMERIC_OK) return rc; /* CHECK:mm_generic_rules */
    const char *name = omega_reduce_op_name(op);
    static const uint32_t DELTA[5] = { 16u, 8u, 4u, 2u, 1u };
    if (n != 12) return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS, "%s: expected five SHFL.DOWN + FMNMX pairs, STG, EXIT (got %d instructions)", name, n); /* CHECK:mm_shape */
    const uint32_t want_w2 = op == OMEGA_RED_MAX ? RED_FMNMX_W2_MAX : RED_FMNMX_W2_MIN;
    const uint32_t acc = 2u;
    for (int s = 0; s < 5; s++) {
        const OmegaNumericPatchInsn *sh = &p[2 * s], *mm = &p[2 * s + 1];
        if (P_OP(*sh) != 0x7f89u || P_SHFL_DELTA(*sh) != DELTA[s]) return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS, "%s: step %d must be SHFL.DOWN by %u (declared order 16,8,4,2,1)", name, s, DELTA[s]); /* CHECK:mm_delta_order */
        if (P_SRCA(*sh) != acc) return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS, "%s: step %d shuffles R%u, not the running value R2", name, s, P_SRCA(*sh)); /* CHECK:mm_shfl_src */
        if (P_DST(*sh) != 9u) return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS, "%s: step %d shuffles into R%u, not the scratch register R9 (R2 is the running value, R6:R7 the store address)", name, s, P_DST(*sh)); /* CHECK:mm_shfl_dst */
        if (s == 0 && !(P_WAIT(*sh) & (1u << 4))) return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS, "%s: first SHFL does not wait on SB4 (the input load of R2)", name); /* CHECK:mm_first_wait_load */
        if (P_OP(*mm) != 0x7209u || (mm->w[1] & ~0xffu) != 0) return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS, "%s: step %d is not followed by a register FMNMX (no negate, no abs)", name, s); /* CHECK:mm_fmnmx_form */
        if (mm->w[2] != want_w2) return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS, "%s: FMNMX of step %d has w2 0x%08x, not the %s form 0x%08x (wrong min/max predicate or modifier)", name, s, mm->w[2], name, want_w2); /* CHECK:mm_fmnmx_pred */
        if (P_WBAR(*sh) > 5u || !(P_WAIT(*mm) & (1u << P_WBAR(*sh)))) return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS, "%s: FMNMX of step %d does not wait on the shuffle", name, s); /* CHECK:mm_fmnmx_wait */
        if (P_SRCA(*mm) != acc || P_SRCB(*mm) != P_DST(*sh)) return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS, "%s: FMNMX of step %d does not combine the running value (left) with the shuffled value (right)", name, s); /* CHECK:mm_fmnmx_srcs */
        if (P_DST(*mm) != (s == 4 ? 9u : acc)) return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS, "%s: FMNMX of step %d writes R%u", name, s, P_DST(*mm)); /* CHECK:mm_fmnmx_dst */
        if (((mm->w[3] >> 9) & 0xfu) < 5u) return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS, "%s: FMNMX of step %d stalls %u cycles, under the 5 the next reader needs", name, s, (mm->w[3] >> 9) & 0xfu); /* CHECK:mm_fmnmx_stall */
    }
    if (p[10].w[0] != 0x06007986u || p[10].w[1] != 0x00000009u || p[10].w[2] != 0x0c101904u) return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS, "%s: R9 is not stored by STG.E desc[UR4][R6.64] (unpredicated, 32-bit)", name); /* CHECK:mm_store */
    if (P_OP(p[11]) != 0x794du) return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS, "%s: last instruction is not EXIT", name); /* CHECK:mm_exit */
    return OMEGA_NUMERIC_OK;
}

int omega_reduce_gb10_build_minmax_kernel(OmegaReduceOp op, uint8_t *code, size_t code_len, size_t *out_len) {
    OmegaNumericPatchInsn patch[OMEGA_NUMERIC_PATCH_MAX];
    int n = omega_reduce_gb10_minmax_patch(op, patch);
    if (n <= 0) return n < 0 ? n : OMEGA_NUMERIC_ERR_BAD_ARGS;
    size_t len = 0;
    if (omega_blackwell_encode_vecadd(code, code_len, &len) != 0) return OMEGA_NUMERIC_ERR_BAD_ARGS;
    if (len < OMEGA_NUMERIC_PATCH_OFFSET + (size_t)n * 16u) return OMEGA_NUMERIC_ERR_BAD_ARGS;
    for (int i = 0; i < n; i++) memcpy(code + OMEGA_NUMERIC_PATCH_OFFSET + (size_t)i * 16u, patch[i].w, 16);
    if (out_len) *out_len = len;
    return OMEGA_NUMERIC_OK;
}

/* QMD for a launch of count values (same shape as the numeric executor). */
static int launch_qmd(size_t count, uint32_t qmd1[OMEGA_BW_QMD_WORDS]) {
    OmegaBlackwellQmdConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.num_elements = (uint32_t)count;
    omega_numeric_launch_shape(count, &cfg.threads_per_block, &cfg.grid_width);
    return omega_blackwell_build_qmd1(qmd1, &cfg);
}

/* ---- plan and pre-submission checks --------------------------------------- */

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

/* Each distinct chunk size the plan submits, checked as it will be launched. */
static int check_each_chunk(OmegaReduceOp op, size_t n, char *err, size_t err_len) {
    size_t len = n;
    char serr[256];
    static float dummy;
    OmegaNumericPatchInsn patch[OMEGA_NUMERIC_PATCH_MAX];
    int np = 0;
    if (op == OMEGA_RED_MAX || op == OMEGA_RED_MIN) np = omega_reduce_gb10_minmax_patch(op, patch);
    while (len > 0) {
        size_t padded = (len + 31) & ~(size_t)31;
        size_t full = padded / OMEGA_NUMERIC_MAX_COUNT, rest = padded % OMEGA_NUMERIC_MAX_COUNT;
        size_t sizes[2] = { full ? OMEGA_NUMERIC_MAX_COUNT : 0, rest };
        for (int k = 0; k < 2; k++) {
            if (!sizes[k]) continue;
            int rc;
            if (op == OMEGA_RED_SUM || op == OMEGA_RED_MEAN) {
                rc = omega_numeric_submit_check("REDUCE_SUM", &dummy, &dummy, NULL, &dummy, sizes[k], serr, sizeof(serr));
            } else {
                uint32_t qmd1[OMEGA_BW_QMD_WORDS];
                if (sizes[k] % 32u != 0 || sizes[k] > OMEGA_NUMERIC_MAX_COUNT)
                    return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS, "%s chunk of %zu is not whole warps within %u", omega_reduce_op_name(op), sizes[k], OMEGA_NUMERIC_MAX_COUNT); /* CHECK:red_mm_chunk_shape */
                rc = launch_qmd(sizes[k], qmd1) != 0 ? OMEGA_NUMERIC_ERR_BAD_ARGS
                                                     : omega_reduce_gb10_check_minmax_patch(op, patch, np, qmd1, serr, sizeof(serr));
            }
            if (rc != OMEGA_NUMERIC_OK)
                return refuse(err, err_len, rc, "%s chunk of %zu refused: %s", omega_reduce_op_name(op), sizes[k], serr); /* CHECK:red_submit_each_chunk */
        }
        if (padded == 32) break;
        len = padded / 32;
    }
    return OMEGA_NUMERIC_OK;
}

int omega_reduce_gb10_check(OmegaReduceOp op, const float *x, size_t n, const float *out,
                            char *err, size_t err_len) {
    if (err && err_len) err[0] = '\0';
    if ((unsigned)op >= OMEGA_RED_COUNT)
        return refuse(err, err_len, OMEGA_NUMERIC_ERR_BAD_ARGS, "unknown reduction op %d", (int)op); /* CHECK:red_op_known */
    if (!out || (n > 0 && !x))
        return refuse(err, err_len, OMEGA_NUMERIC_ERR_BAD_ARGS, "missing input or output buffer"); /* CHECK:red_buffers */
    if (n > OMEGA_REDUCE_GB10_MAX_N)
        return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS, "n=%zu exceeds %zu", n, OMEGA_REDUCE_GB10_MAX_N); /* CHECK:red_max_n */
    if (op == OMEGA_RED_MEAN && n > OMEGA_REDUCE_MEAN_MAX_N)
        return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS, "MEAN n=%zu exceeds 2^24 (n not exact in FP32)", n); /* CHECK:red_mean_max_n */
    if (strcmp(OMEGA_WARP_REDUCTION_DECLARED_ORDER, "PAIRWISE_TREE_LANE_DELTA_16_8_4_2_1") != 0 ||
        strstr(OMEGA_REDUCE_DECLARED_ORDER, OMEGA_WARP_REDUCTION_DECLARED_ORDER) == NULL)
        return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS,
                      "warp kernel order %s is not the tile order of %s",
                      OMEGA_WARP_REDUCTION_DECLARED_ORDER, OMEGA_REDUCE_DECLARED_ORDER); /* CHECK:red_order_string */
    if (op == OMEGA_RED_SUM || op == OMEGA_RED_MEAN) {
        const OmegaNumericOpInfo *info = omega_numeric_op_find("REDUCE_SUM");
        if (!info || info->op != OMEGA_NOP_REDUCE_SUM || !info->gb10_encoded || info->compare != OMEGA_CMP_BIT_EXACT)
            return refuse(err, err_len, OMEGA_NUMERIC_ERR_NOT_ENCODED,
                          "REDUCE_SUM is not a GB10-encoded BIT_EXACT op in the registry"); /* CHECK:red_kernel_registered */
        if (omega_float_to_bits(omega_reduce_identity(op)) != OMEGA_REDUCE_SUM_PAD_BITS)
            return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS, "%s padding is not -0.0", omega_reduce_op_name(op)); /* CHECK:red_pad_identity */
    } else {
        /* the single-pair FMNMX form this patch reuses is chip-proven BIT_EXACT */
        const OmegaNumericOpInfo *info = omega_numeric_op_find(op == OMEGA_RED_MAX ? "FMNMX_MAX" : "FMNMX_MIN");
        if (!info || !info->gb10_encoded || info->compare != OMEGA_CMP_BIT_EXACT)
            return refuse(err, err_len, OMEGA_NUMERIC_ERR_NOT_ENCODED,
                          "%s: FMNMX is not a GB10-encoded BIT_EXACT op in the registry", omega_reduce_op_name(op)); /* CHECK:red_mm_fmnmx_registered */
        if (omega_float_to_bits(omega_reduce_identity(op)) != OMEGA_REDUCE_MINMAX_PAD_BITS)
            return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS, "%s padding is not quiet NaN", omega_reduce_op_name(op)); /* CHECK:red_mm_pad_identity */
    }
    unsigned levels = 0;
    (void)plan_launches(n, &levels);
    if (levels != omega_reduce_levels(n))
        return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS, "launch plan has %u levels, contract says %u",
                      levels, omega_reduce_levels(n)); /* CHECK:red_levels */
    if ((OMEGA_NUMERIC_MAX_COUNT % 32u) != 0)
        return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS, "chunk limit is not whole warps"); /* CHECK:red_chunk_whole_warps */
    return check_each_chunk(op, n, err, err_len);
}

/* ---- MAX/MIN executor ------------------------------------------------------ */
#ifndef OMEGA_NUMERIC_CPU_ONLY
static const uint32_t RED_SETUP_WORDS[18] = {
    0x20012061, 0x0000cec0, 0x20012092, 0x00000001, 0x200120a8, 0x0000000f, 0x2001255d, 0x00000003,
    0x2001255e, 0x20000000, 0x2001255f, 0x000fffff, 0x20012557, 0x00000003, 0x20012558, 0x22000000,
    0x20012559, 0x00000000,
};

/* Patch words for one level launch of op: REDUCE_SUM's (WP-C table) for SUM
 * and MEAN, the reduce-owned FMNMX patch for MAX and MIN. */
static int chunk_patch(OmegaReduceOp op, OmegaNumericPatchInsn patch[OMEGA_NUMERIC_PATCH_MAX]) {
    if (op == OMEGA_RED_MAX || op == OMEGA_RED_MIN) return omega_reduce_gb10_minmax_patch(op, patch);
    if (op == OMEGA_RED_SUM || op == OMEGA_RED_MEAN) return omega_numeric_patch_words(OMEGA_NOP_REDUCE_SUM, patch);
    return OMEGA_NUMERIC_ERR_BAD_ARGS;
}

static int chunk_check(OmegaReduceOp op, const OmegaNumericPatchInsn *patch, int np, const uint32_t *qmd1,
                       char *err, size_t err_len) {
    if (op == OMEGA_RED_MAX || op == OMEGA_RED_MIN)
        return omega_reduce_gb10_check_minmax_patch(op, patch, np, qmd1, err, err_len);
    return omega_numeric_check_patch(OMEGA_NOP_REDUCE_SUM, patch, np, qmd1, err, err_len);
}

/* One level chunk on GB10, every op. Like the SIMT launcher, require both
 * the host marker and the QMD release semaphore before reading results. */
/* Device-failure diagnostics (M20 GB10 instrumentation). Every
 * OMEGA_NUMERIC_ERR_DEVICE return below goes through gb10_devfail, which
 * prints one GB10_DEVFAIL line to stderr: the step that failed, the driver
 * return code, errno, the nvrm error text, the wait value and the marker word
 * for waits, and the elapsed ms since the launch began. The lifecycle wrapper
 * retains resources after uncertain completion and refuses later numeric
 * launches. There is no retry and no wait value changes. */
static double gb10_ms_since(const struct timespec *t0) {
    struct timespec t;
    timespec_get(&t, TIME_UTC); /* C11; no feature macro needed */
    return (double)(t.tv_sec - t0->tv_sec) * 1e3 + (double)(t.tv_nsec - t0->tv_nsec) / 1e6;
}

static int gb10_devfail(const char *fn, const char *step, M16NativeContext *ctx, int do_close, int drv_rc,
                        int saved_errno, long wait_ms, const volatile uint32_t *word, uint32_t want,
                        const struct timespec *t0) {
    double ms = gb10_ms_since(t0);
    fprintf(stderr, "GB10_DEVFAIL fn=%s step=%s drv_rc=%d errno=%d rm_err=\"%s\" live_allocs=%u faulted=%u",
            fn, step, drv_rc, saved_errno, ctx->rm.err, (unsigned)ctx->rm.live_count, (unsigned)ctx->rm.faulted);
    if (wait_ms >= 0)
        fprintf(stderr, " wait_ms=%ld word=0x%08x want=0x%08x", wait_ms, word ? (unsigned)*word : 0u, (unsigned)want);
    fprintf(stderr, " elapsed_ms=%.3f\n", ms);
    if (do_close) omega_numeric_native_close(ctx);
    return OMEGA_NUMERIC_ERR_DEVICE;
}
/* Slow-wait note: one stderr line when a successful wait finished more than 1000 ms after launch began. */
static void gb10_note_slow(const char *step, const struct timespec *t0) {
    double ms = gb10_ms_since(t0);
    if (ms > 1000.0) fprintf(stderr, "GB10_SLOW_WAIT step=%s elapsed_ms=%.3f\n", step, ms);
}
/* Diagnostic probe for the E1 stall (default build: none of this exists; every wait cap stays the
 * literal 600000 ms and marker_mem is a plain nvrm_alloc).
 *   -DOMEGA_STALL_PROBE        one stderr line per launch: GB10_PROBE n= inner= marker_ms= marker2_ms= sem_ms=
 *                              (ms from launch start to each wait success, -1 = not reached), a timeout
 *                              prints GB10_PROBE_TIMEOUT with all three words; the wait cap becomes the
 *                              env var OMEGA_PROBE_WAIT_MS (default 600000).
 *   -DOMEGA_MARKER_UNCACHED    marker_mem is allocated with nvrm_alloc_gpu_uncached (not cached in GPU L2). */
#ifdef OMEGA_STALL_PROBE
static unsigned long gb10_probe_wait_ms(void) {
    const char *e = getenv("OMEGA_PROBE_WAIT_MS");
    if (e && *e) {
        char *end = NULL;
        unsigned long v = strtoul(e, &end, 10);
        if (end && *end == 0 && v > 0) return v;
    }
    return 600000ul;
}
#define GB10_WAIT_MS gb10_probe_wait_ms()
static void gb10_probe_timeout(const char *step, size_t count, const volatile uint32_t *m1, const volatile uint32_t *m2,
                               const volatile uint32_t *sem, const struct timespec *t0) {
    fprintf(stderr, "GB10_PROBE_TIMEOUT step=%s n=%zu inner=%u marker=0x%08x marker2=0x%08x sem=0x%08x elapsed_ms=%.3f\n",
            step, count, g_last_launches + 1u, (unsigned)*m1, (unsigned)*m2, (unsigned)*sem, gb10_ms_since(t0));
}
#else
#define GB10_WAIT_MS 600000
#endif
#ifdef OMEGA_MARKER_UNCACHED
#define GB10_MARKER_ALLOC(rm_, size_, out_) nvrm_alloc_gpu_uncached((rm_), (size_), (out_))
#else
#define GB10_MARKER_ALLOC(rm_, size_, out_) nvrm_alloc((rm_), (size_), (out_))
#endif

/* Plain step: drv_rc is the value the call returned. */
#define GB10_FAIL(step, close_, rc_) gb10_devfail(__func__, (step), &ctx, (close_), (rc_), errno, -1, NULL, 0u, &t0)
/* Wait step: also log the wait value and the word read. */
#define GB10_FAIL_WAIT(step, rc_, ms_, w_, want_) \
    gb10_devfail(__func__, (step), &ctx, 1, (rc_), errno, (long)(ms_), (w_), (want_), &t0)

static int run_chunk(OmegaReduceOp op, const float *in, float *out_res, size_t count) {
    OmegaNumericPatchInsn patch[OMEGA_NUMERIC_PATCH_MAX];
    int np = chunk_patch(op, patch);
    if (np <= 0 || count == 0 || count > OMEGA_NUMERIC_MAX_COUNT || count % 32u) return OMEGA_NUMERIC_ERR_BAD_ARGS;

    struct timespec t0;
    timespec_get(&t0, TIME_UTC);
    M16NativeContext ctx;
    int drc_;
    if ((drc_ = omega_numeric_native_open(&ctx)) != 0) return GB10_FAIL("open", 0, drc_);
    if ((drc_ = m16_native_create_channel(&ctx)) != 0) return GB10_FAIL("channel", 1, drc_);
    NvrmMem large_pb;
    if ((drc_ = nvrm_alloc(&ctx.rm, 0x10000, &large_pb)) != 0) return GB10_FAIL("alloc_pb", 1, drc_);
    ctx.pb_mem = large_pb;

    size_t bytes = (count * sizeof(float) + 0xfffULL) & ~0xfffULL;
    if (bytes < 0x1000) bytes = 0x1000;
    NvrmMem code_mem, cbank_mem, a_mem, b_mem, c_mem, out_mem, marker_mem, qmd_mem;
    if (nvrm_alloc(&ctx.rm, 0x1000, &code_mem) != 0 || nvrm_alloc(&ctx.rm, 0x1000, &cbank_mem) != 0 ||
        nvrm_alloc(&ctx.rm, bytes, &a_mem) != 0 || nvrm_alloc(&ctx.rm, bytes, &b_mem) != 0 ||
        nvrm_alloc(&ctx.rm, bytes, &c_mem) != 0 || nvrm_alloc(&ctx.rm, bytes, &out_mem) != 0 ||
        GB10_MARKER_ALLOC(&ctx.rm, 0x1000, &marker_mem) != 0 || nvrm_alloc(&ctx.rm, 0x10000, &qmd_mem) != 0) {
        return GB10_FAIL("alloc_buffers", 1, -1);
    }
    memcpy(a_mem.cpu, in, count * sizeof(float));
    memset(b_mem.cpu, 0, count * sizeof(float));
    memset(out_mem.cpu, 0x55, count * sizeof(float));

    size_t code_len = 0;
    if (omega_blackwell_encode_vecadd(code_mem.cpu, code_mem.size, &code_len) != 0 ||
        code_len < OMEGA_NUMERIC_PATCH_OFFSET + (size_t)np * 16u) {
        return GB10_FAIL("build_kernel", 1, -1);
    }
    for (int i = 0; i < np; i++) memcpy((uint8_t *)code_mem.cpu + OMEGA_NUMERIC_PATCH_OFFSET + (size_t)i * 16u, patch[i].w, 16);

    uint32_t cbank_data[OMEGA_BW_CBANK_DRIVER_WORDS];
    omega_blackwell_build_cbank_driver(cbank_data, cbank_mem.va);
    cbank_data[223] = omega_float_to_bits(1.0f); /* R1 at entry; unused by this patch */
    uint32_t cbank_args[10] = {
        (uint32_t)a_mem.va, (uint32_t)(a_mem.va >> 32), (uint32_t)b_mem.va, (uint32_t)(b_mem.va >> 32),
        (uint32_t)out_mem.va, (uint32_t)(out_mem.va >> 32), (uint32_t)count, 0,
        (uint32_t)c_mem.va, (uint32_t)(c_mem.va >> 32) };
    memcpy(cbank_mem.cpu, cbank_data, sizeof(cbank_data));
    memcpy((uint8_t *)cbank_mem.cpu + 0x380, cbank_args, sizeof(cbank_args));

    uint64_t qmd0_va = qmd_mem.va, qmd1_va = qmd_mem.va + 0x1000, sem_va = qmd_mem.va + 0x2000;
    OmegaBlackwellQmdConfig qmd_cfg = {
        .code_va = code_mem.va, .cbank_va = cbank_mem.va, .scratch_va = qmd_mem.va + 0x4000,
        .sem_va = sem_va, .qmd0_va = qmd0_va, .qmd1_va = qmd1_va, .num_elements = (uint32_t)count,
    };
    omega_numeric_launch_shape(count, &qmd_cfg.threads_per_block, &qmd_cfg.grid_width);
    uint32_t qmd0_words[OMEGA_BW_QMD_WORDS], qmd1_words[OMEGA_BW_QMD_WORDS];
    if (omega_blackwell_build_qmd0(qmd0_words, qmd0_va, qmd1_va) != 0 ||
        omega_blackwell_build_qmd1(qmd1_words, &qmd_cfg) != 0 ||
        omega_blackwell_verify_qmd_invariants(qmd1_words) != 0) {
        return GB10_FAIL("qmd", 1, -1);
    }
    { /* the structural check again, on the QMD actually submitted and the code image */
        char perr[256];
        if (chunk_check(op, patch, np, qmd1_words, perr, sizeof(perr)) != OMEGA_NUMERIC_OK) {
            fprintf(stderr, "omega_reduce_gb10: %s\n", perr);
            omega_numeric_native_close(&ctx);
            return OMEGA_NUMERIC_ERR_OPERANDS;
        }
        for (int i = 0; i < np; i++)
            if (memcmp((uint8_t *)code_mem.cpu + OMEGA_NUMERIC_PATCH_OFFSET + (size_t)i * 16u, patch[i].w, 16) != 0) {
                fprintf(stderr, "omega_reduce_gb10: code image differs from the checked patch at %d\n", i);
                omega_numeric_native_close(&ctx);
                return OMEGA_NUMERIC_ERR_OPERANDS;
            }
    }
    memcpy(qmd_mem.cpu, qmd0_words, sizeof(qmd0_words));
    memcpy((uint8_t *)qmd_mem.cpu + 0x1000, qmd1_words, sizeof(qmd1_words));

    volatile uint32_t *hsem = (volatile uint32_t *)((uint8_t *)qmd_mem.cpu + 0x2000);
    volatile uint32_t *hmarker = (volatile uint32_t *)marker_mem.cpu;
    *hsem = 0;
    *hmarker = 0;
#ifndef OMEGA_C3_PROTECT_OFF /* C3 hardening, same as omega_ds_gb10_run (-DOMEGA_C3_PROTECT_OFF = A/B control arm) */
    *(volatile uint32_t *)((uint8_t *)marker_mem.cpu + 0x10) = 0;
#endif
    __asm__ volatile("dsb sy" ::: "memory");

    uint32_t pb[1024];
    size_t pb_len = 0;
    memcpy(&pb[pb_len], RED_SETUP_WORDS, sizeof(RED_SETUP_WORDS));
    pb_len += sizeof(RED_SETUP_WORDS) / 4;
    pb[pb_len++] = nvrm_mthd(1, 0x0188, 2);
    pb[pb_len++] = (uint32_t)(cbank_mem.va >> 32);
    pb[pb_len++] = (uint32_t)cbank_mem.va;
    pb[pb_len++] = nvrm_mthd(1, 0x0180, 2);
    pb[pb_len++] = 0x00000380;
    pb[pb_len++] = 0x00000001;
    pb[pb_len++] = nvrm_mthd(1, 0x01b0, 1);
    pb[pb_len++] = 0x00000041;
    pb[pb_len++] = (224 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    memcpy(&pb[pb_len], cbank_data, 224 * 4);
    pb_len += 224;
    pb[pb_len++] = nvrm_mthd(1, 0x0188, 2);
    pb[pb_len++] = (uint32_t)((cbank_mem.va + 0x380) >> 32);
    pb[pb_len++] = (uint32_t)(cbank_mem.va + 0x380);
    pb[pb_len++] = nvrm_mthd(1, 0x0180, 2);
    pb[pb_len++] = 0x00000028;
    pb[pb_len++] = 0x00000001;
    pb[pb_len++] = nvrm_mthd(1, 0x01b0, 1);
    pb[pb_len++] = 0x00000041;
    pb[pb_len++] = (10 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    memcpy(&pb[pb_len], cbank_args, 10 * 4);
    pb_len += 10;
    pb[pb_len++] = (98 << 16) | (1 << 13) | (0x0318 >> 2) | (2u << 28);
    pb[pb_len++] = (1u << 30) | (uint32_t)((qmd0_va >> 40) & 0x1ff);
    pb[pb_len++] = (uint32_t)(qmd0_va >> 8);
    memcpy(&pb[pb_len], qmd0_words, 96 * 4);
    pb_len += 96;
    pb[pb_len++] = nvrm_mthd(1, 0x0188, 2);
    pb[pb_len++] = (uint32_t)(sem_va >> 32);
    pb[pb_len++] = (uint32_t)sem_va;
    pb[pb_len++] = nvrm_mthd(1, 0x0180, 2);
    pb[pb_len++] = 0x00000004;
    pb[pb_len++] = 0x00000001;
    pb[pb_len++] = nvrm_mthd(1, 0x01b0, 1);
    pb[pb_len++] = 0x00000041;
    pb[pb_len++] = (1 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    pb[pb_len++] = OMEGA_BW_SEMAPHORE_INTERMEDIATE_INIT;
    pb[pb_len++] = (98 << 16) | (1 << 13) | (0x0318 >> 2) | (2u << 28);
    pb[pb_len++] = (1u << 30) | (uint32_t)((qmd1_va >> 40) & 0x1ff);
    pb[pb_len++] = (uint32_t)(qmd1_va >> 8);
    memcpy(&pb[pb_len], qmd1_words, 96 * 4);
    pb_len += 96;
    pb[pb_len++] = nvrm_mthd(0, 0x005c, 5);
    pb[pb_len++] = (uint32_t)marker_mem.va;
    pb[pb_len++] = (uint32_t)(marker_mem.va >> 32);
    pb[pb_len++] = OMEGA_BW_MARKER_COMPLETION_PAYLOAD;
    pb[pb_len++] = 0;
    pb[pb_len++] = 0x1 | (1u << 20);
#ifndef OMEGA_C3_PROTECT_OFF
    /* C3 hardening, copied from omega_ds_gb10_run (src/omega_numeric_divsqrt_gb10.c): after the WFI
     * marker flush GPU L2 dirty lines to memory (NVC96F_MEM_OP_A..D = 0x28..0x34, D bits 31:27
     * OPERATION = L2_FLUSH_DIRTY 0x10), then a second WFI marker the host waits for before the readback. */
    pb[pb_len++] = nvrm_mthd(0, 0x0028, 4);
    pb[pb_len++] = 0;
    pb[pb_len++] = 0;
    pb[pb_len++] = 0;
    pb[pb_len++] = (0x10u << 27);
    pb[pb_len++] = nvrm_mthd(0, 0x005c, 5);
    pb[pb_len++] = (uint32_t)(marker_mem.va + 0x10);
    pb[pb_len++] = (uint32_t)((marker_mem.va + 0x10) >> 32);
    pb[pb_len++] = 0x46464646u;
    pb[pb_len++] = 0;
    pb[pb_len++] = 0x1 | (1u << 20);
#endif

    if ((drc_ = m16_native_submit_methods(&ctx, pb, pb_len)) != 0) return GB10_FAIL("submit", 1, drc_);
#ifdef OMEGA_STALL_PROBE
    volatile uint32_t *pmarker2 = (volatile uint32_t *)((uint8_t *)marker_mem.cpu + 0x10);
    double probe_m1 = -1.0, probe_m2 = -1.0, probe_sem = -1.0;
#define GB10_PROBE_TMO(step_) gb10_probe_timeout((step_), count, hmarker, pmarker2, hsem, &t0)
#else
#define GB10_PROBE_TMO(step_) ((void)0)
#endif
    if ((drc_ = omega_numeric_native_wait(hmarker, OMEGA_BW_MARKER_COMPLETION_PAYLOAD, GB10_WAIT_MS)) != 0) {
        GB10_PROBE_TMO("marker_wait");
        return GB10_FAIL_WAIT("marker_wait", drc_, GB10_WAIT_MS, hmarker, OMEGA_BW_MARKER_COMPLETION_PAYLOAD);
    }
    gb10_note_slow("marker", &t0);
#ifdef OMEGA_STALL_PROBE
    probe_m1 = gb10_ms_since(&t0);
#endif
#ifndef OMEGA_C3_PROTECT_OFF
    volatile uint32_t *hmarker2 = (volatile uint32_t *)((uint8_t *)marker_mem.cpu + 0x10);
    if ((drc_ = omega_numeric_native_wait(hmarker2, 0x46464646u, GB10_WAIT_MS)) != 0) {
        GB10_PROBE_TMO("marker2_wait");
        return GB10_FAIL_WAIT("marker2_wait", drc_, GB10_WAIT_MS, hmarker2, 0x46464646u);
    }
    gb10_note_slow("marker2", &t0);
#ifdef OMEGA_STALL_PROBE
    probe_m2 = gb10_ms_since(&t0);
#endif
#endif
    /* The host marker can land before the last CTAs' stores are visible (seen
     * by the DIV/SQRT gate, PR #141). Read only after the QMD's own release
     * semaphore, written after the grid completes, is DONE. */
    if ((drc_ = omega_numeric_native_wait(hsem, OMEGA_BW_SEMAPHORE_INTERMEDIATE_DONE, GB10_WAIT_MS)) != 0) {
        GB10_PROBE_TMO("sem_wait");
        return GB10_FAIL_WAIT("sem_wait", drc_, GB10_WAIT_MS, hsem, OMEGA_BW_SEMAPHORE_INTERMEDIATE_DONE);
    }
    gb10_note_slow("sem", &t0);
#ifdef OMEGA_STALL_PROBE
    probe_sem = gb10_ms_since(&t0);
    fprintf(stderr, "GB10_PROBE n=%zu inner=%u marker_ms=%.3f marker2_ms=%.3f sem_ms=%.3f\n", count,
            g_last_launches + 1u, probe_m1, probe_m2, probe_sem);
#endif
    __asm__ volatile("dsb sy" ::: "memory");
    memcpy(out_res, out_mem.cpu, count * sizeof(float));
    if (omega_numeric_native_close(&ctx) != 0) return OMEGA_NUMERIC_ERR_DEVICE;
    return OMEGA_NUMERIC_OK;
}
#endif

/* ---- level loop ------------------------------------------------------------ */

int omega_reduce_gb10(OmegaReduceOp op, const float *x, size_t n, float *out) {
    char err[320];
    g_last_launches = 0;
    int rc = omega_reduce_gb10_check(op, x, n, out, err, sizeof(err));
    if (rc != OMEGA_NUMERIC_OK) {
        fprintf(stderr, "omega_reduce_gb10: refused before submission: %s\n", err);
        return rc;
    }
    if (n == 0) {
        *out = omega_bits_to_float(op == OMEGA_RED_SUM ? OMEGA_REDUCE_EMPTY_SUM_BITS : OMEGA_REDUCE_EMPTY_OTHER_BITS);
        return OMEGA_NUMERIC_OK;
    }
#ifdef OMEGA_NUMERIC_CPU_ONLY
    return OMEGA_NUMERIC_ERR_DEVICE;
#else
    const bool minmax = (op == OMEGA_RED_MAX || op == OMEGA_RED_MIN);
    const uint32_t pad = minmax ? OMEGA_REDUCE_MINMAX_PAD_BITS : OMEGA_REDUCE_SUM_PAD_BITS;
    size_t cap = (n + 31) & ~(size_t)31;
    float *cur = malloc(cap * sizeof(float));
    float *res = malloc(OMEGA_NUMERIC_MAX_COUNT * sizeof(float));
    if (!cur || !res) { free(cur); free(res); return OMEGA_NUMERIC_ERR_BAD_ARGS; }
    memcpy(cur, x, n * sizeof(float));
    size_t len = n;
    for (;;) {
        size_t padded = (len + 31) & ~(size_t)31;
        for (size_t i = len; i < padded; i++) cur[i] = omega_bits_to_float(pad);
        size_t next_len = padded / 32;
        for (size_t base = 0; base < padded; base += OMEGA_NUMERIC_MAX_COUNT) {
            size_t count = padded - base;
            if (count > OMEGA_NUMERIC_MAX_COUNT) count = OMEGA_NUMERIC_MAX_COUNT;
            rc = run_chunk(op, cur + base, res, count);
            g_last_launches++;
            if (rc != OMEGA_NUMERIC_OK) {
                fprintf(stderr, "GB10_REDUCE_FAIL op=%d n=%zu level_len=%zu base=%zu count=%zu inner_launch=%u rc=%d\n",
                        (int)op, n, len, base, count, g_last_launches, rc);
                free(cur); free(res); return rc;
            }
            /* lane 0 of warp w holds tile (base/32 + w); base/32 + w <= base + 32w */
            for (size_t w = 0; w < count / 32; w++) cur[base / 32 + w] = res[w * 32];
        }
        len = next_len;
        if (len == 1) break;
    }
    float s = cur[0];
    free(cur);
    free(res);
    if (op == OMEGA_RED_MEAN) {
        /* MEAN: the one final division runs on the chip too: the GB10 DIV kernel of
         * E1 row 7 (correctly rounded, bit-identical to omega_math_div). */
        uint32_t sb, nb, ob;
        float nf = omega_ref_u2f((uint32_t)n);
#ifdef OMEGA_REDUCE_MUTATE_MEAN_DIV /* test builds only: divide by n + 1 (must FAIL chip parity) */
        nf = omega_ref_u2f((uint32_t)n + 1u);
#endif
        memcpy(&sb, &s, 4);
        memcpy(&nb, &nf, 4);
        rc = omega_ds_gb10_run(OMEGA_DS_DIV, &sb, &nb, &ob, 1);
        if (rc != OMEGA_NUMERIC_OK) return rc;
        memcpy(out, &ob, 4);
        return OMEGA_NUMERIC_OK;
    }
    *out = s;
    return OMEGA_NUMERIC_OK;
#endif
}
