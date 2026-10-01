/*
 * E1 row 10: GB10 realizations of the frozen transcendental sequences
 * (src/omega_numeric_transc.c), bit-identical to the CPU tier.
 *
 * Modes (one per run):
 *   (none)            host tier: each op's kernel builds, passes the
 *                     structural check and carries its recorded digest; the
 *                     host model of the body (src/omega_numeric_divsqrt_gb10.c)
 *                     equals omega_math_<op> on the edge set, on every 4093rd
 *                     32-bit input and on 10^6 random inputs. No device.
 *   --host-all OP     host model of OP on all 2^32 inputs (16 threads)
 *   --digest          SHA-256 of each transcendental kernel
 *   --chip [OP ...]   GB10: every 2^32 input of each OP (default: all
 *                     transcendental ops), 256 launches of 2^24, each output
 *                     compared bit for bit with omega_math_<op> (NaN results
 *                     included: the CPU tier writes 0x7fc00000). One RESULT
 *                     line per op, then a VERDICT line.
 * Output lines: [PASS] / [FAIL] ID, RESULT ..., VERDICT ...
 */
#include "omega_numeric.h"
#include "omega_numeric_divsqrt_gb10.h"
#include "omega_numeric_transc.h"
#include "omega_blackwell_encoder.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const OmegaDsOp OPS[] = { OMEGA_DS_EXP2, OMEGA_DS_LOG2, OMEGA_DS_SIGMOID, OMEGA_DS_TANH };
#define N_OPS (sizeof(OPS) / sizeof(OPS[0]))

static int g_pass, g_fail;
static void check(int ok, const char *id) {
    if (ok) { g_pass++; printf("[PASS] %s\n", id); }
    else { g_fail++; printf("[FAIL] %s\n", id); }
}

/* The CPU tier the GB10 must equal, as bits. */
static uint32_t want_bits(OmegaDsOp op, uint32_t a) {
    float x = omega_bits_to_float(a), r;
    switch (op) {
    case OMEGA_DS_EXP2: r = omega_math_exp2(x); break;
    case OMEGA_DS_LOG2: r = omega_math_log2(x); break;
    case OMEGA_DS_SIGMOID: r = omega_math_sigmoid(x); break;
    case OMEGA_DS_TANH: r = omega_math_tanh(x); break;
    default: return 0xffffffffu;
    }
    return omega_float_to_bits(r);
}

static int find_op(const char *name, OmegaDsOp *op) {
    for (size_t i = 0; i < N_OPS; i++)
        if (!strcmp(name, omega_ds_op_name(OPS[i]))) { *op = OPS[i]; return 0; }
    return -1;
}

static const uint32_t EDGE_MAG[] = {
    0x00000000u, 0x00000001u, 0x00000002u, 0x00400000u, 0x007fffffu, 0x00800000u, 0x00800001u,
    0x33800000u, 0x34000000u, 0x3effffffu, 0x3f000000u, 0x3f3504f3u, 0x3f3504f4u, 0x3f7fffffu,
    0x3f800000u, 0x3f800001u, 0x3fb504f3u, 0x3fb504f4u, 0x40000000u, 0x4a7fffffu, 0x4a800000u,
    0x42fe0000u, 0x42ffffffu, 0x43000000u, 0x43000001u, 0x43160000u, 0x4316ffffu, 0x43170000u,
    0x43170001u, 0x42fc0000u, 0x42fd0000u, 0x7f7fffffu, 0x7f800000u, 0x7f800001u, 0x7fc00000u,
    0x7fffffffu,
};
#define N_EDGE_MAG (sizeof(EDGE_MAG) / sizeof(EDGE_MAG[0]))

static uint64_t g_rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return g_rng;
}

static int model_ok(OmegaDsOp op, uint32_t a, uint64_t *bad) {
    uint32_t got = omega_ds_host_exec(op, a, 0), want = want_bits(op, a);
    if (got == want) return 1;
    if (*bad < 8) printf("    %s 0x%08x: model 0x%08x cpu 0x%08x\n", omega_ds_op_name(op), a, got, want);
    (*bad)++;
    return 0;
}

static int host_tier(void) {
    char id[160], err[256];
    for (size_t i = 0; i < N_OPS; i++) {
        OmegaDsOp op = OPS[i];
        const char *on = omega_ds_op_name(op);
        static uint8_t code[OMEGA_DS_MAX_CODE_BYTES];
        size_t len = 0;
        OmegaDsInsn body[OMEGA_DS_MAX_BODY];
        size_t nb = omega_ds_body(op, body, OMEGA_DS_MAX_BODY);
        snprintf(id, sizeof(id), "TG_%s_BODY_BUILDS (%zu insns)", on, nb);
        check(nb > 0, id);
        int fp = 0;
        for (size_t k = 0; k < nb; k++) fp += body[k].kind == DSK_FADD_R || body[k].kind == DSK_FMUL_R || body[k].kind == DSK_FFMA_R;
        snprintf(id, sizeof(id), "TG_%s_BODY_HAS_FP_FORMS (%d FADD/FMUL/FFMA)", on, fp);
        check(fp > 0, id);
        snprintf(id, sizeof(id), "TG_%s_KERNEL_BUILDS", on);
        check(omega_ds_build_kernel(op, code, sizeof(code), &len) == 0, id);
        int rc = omega_ds_check_kernel(op, code, len, OMEGA_DS_GPR_COUNT, err, sizeof(err));
        if (rc) printf("    %s\n", err);
        snprintf(id, sizeof(id), "TG_%s_KERNEL_PASSES_STRUCTURAL_CHECK", on);
        check(rc == OMEGA_NUMERIC_OK, id);
        rc = omega_ds_check_digest(op, code, len, err, sizeof(err));
        if (rc) printf("    %s\n", err);
        snprintf(id, sizeof(id), "TG_%s_KERNEL_DIGEST_IS_RECORDED", on);
        check(rc == OMEGA_NUMERIC_OK, id);

        uint64_t bad = 0, n = 0;
        for (size_t k = 0; k < 2 * N_EDGE_MAG; k++, n++) model_ok(op, EDGE_MAG[k / 2] | (k & 1 ? 0x80000000u : 0), &bad);
        snprintf(id, sizeof(id), "TG_%s_HOST_MODEL_EDGE_SET (%" PRIu64 " inputs)", on, n);
        check(bad == 0, id);
        bad = 0; n = 0;
        for (uint64_t u = 0; u < (1ull << 32); u += 4093, n++) model_ok(op, (uint32_t)u, &bad);
        snprintf(id, sizeof(id), "TG_%s_HOST_MODEL_STRIDE_4093 (%" PRIu64 " inputs)", on, n);
        check(bad == 0, id);
        bad = 0;
        for (n = 0; n < 1000000; n++) model_ok(op, (uint32_t)rnd(), &bad);
        snprintf(id, sizeof(id), "TG_%s_HOST_MODEL_RANDOM_1M", on);
        check(bad == 0, id);
    }
    printf("SUMMARY passed=%d failed=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

static int digests(void) {
    for (size_t i = 0; i < N_OPS; i++) {
        static uint8_t code[OMEGA_DS_MAX_CODE_BYTES];
        uint8_t dg[32];
        size_t len = 0;
        if (omega_ds_build_kernel(OPS[i], code, sizeof(code), &len) != 0 ||
            omega_blackwell_compute_code_digest(code, len, dg) != 0) return 1;
        printf("%s %zu ", omega_ds_op_name(OPS[i]), len / 16);
        for (int k = 0; k < 32; k++) printf("%02x", dg[k]);
        printf("\n");
    }
    return 0;
}

/* ---- exhaustive comparisons (threads) ---------------------------------------- */

#define T 16
typedef struct {
    OmegaDsOp op;
    uint64_t lo, hi;          /* inputs [lo, hi)                          */
    const uint32_t *chip;     /* chip outputs for inputs base..; NULL: model */
    uint64_t base;
    uint64_t bad, fill;
    uint32_t first_in[4], first_got[4], first_want[4];
} Slice;

static void *slice_run(void *p) {
    Slice *s = p;
    for (uint64_t u = s->lo; u < s->hi; u++) {
        uint32_t got = s->chip ? s->chip[u - s->base] : omega_ds_host_exec(s->op, (uint32_t)u, 0);
        uint32_t want = want_bits(s->op, (uint32_t)u);
        if (got != want) {
            if (s->bad < 4) { s->first_in[s->bad] = (uint32_t)u; s->first_got[s->bad] = got; s->first_want[s->bad] = want; }
            s->bad++;
            s->fill += s->chip && got == 0x55555555u;
        }
    }
    return NULL;
}

/* Compares [lo, hi) with T threads; adds to *bad and *fill. */
static int compare_range(OmegaDsOp op, uint64_t lo, uint64_t hi, const uint32_t *chip, uint64_t *bad, uint64_t *fill, uint64_t shown) {
    pthread_t th[T];
    Slice s[T];
    for (int i = 0; i < T; i++) {
        memset(&s[i], 0, sizeof(s[i]));
        s[i].op = op; s[i].chip = chip; s[i].base = lo;
        s[i].lo = lo + (hi - lo) * i / T; s[i].hi = lo + (hi - lo) * (i + 1) / T;
        if (pthread_create(&th[i], NULL, slice_run, &s[i]) != 0) {
            for (int j = 0; j < i; j++) pthread_join(th[j], NULL);   /* never leave a worker on this stack */
            return -1;
        }
    }
    for (int i = 0; i < T; i++) {
        if (pthread_join(th[i], NULL) != 0) return -1;
        for (uint64_t k = 0; k < s[i].bad && k < 4 && shown + k < 16; k++)
            printf("    %s 0x%08x: %s 0x%08x cpu 0x%08x\n", omega_ds_op_name(op), s[i].first_in[k],
                   chip ? "chip" : "model", s[i].first_got[k], s[i].first_want[k]);
        shown += s[i].bad;
        *bad += s[i].bad; *fill += s[i].fill;
    }
    return 0;
}

static int host_all(OmegaDsOp op) {
    uint64_t bad = 0, fill = 0;
    omega_ds_host_exec(op, 0, 0); /* fill the body cache before threads */
    if (compare_range(op, 0, 1ull << 32, NULL, &bad, &fill, 0) != 0) { printf("[FAIL] threads\n"); return 2; }
    printf("RESULT host %s checked=4294967296 mismatches=%" PRIu64 "\n", omega_ds_op_name(op), bad);
    return bad ? 1 : 0;
}

#ifndef OMEGA_NUMERIC_CPU_ONLY
#define BATCH (1u << 24)
static int chip(OmegaDsOp *ops, size_t nops) {
    uint32_t *a = malloc((size_t)BATCH * 4), *o = malloc((size_t)BATCH * 4);
    if (!a || !o) return 2;
    int any_fail = 0, any_notrun = 0;
    for (size_t i = 0; i < nops; i++) {
        OmegaDsOp op = ops[i];
        uint64_t checked = 0, bad = 0, fill = 0;
        int dev_err = 0;
        for (uint32_t bi = 0; bi < 256 && !dev_err; bi++) {
            for (uint32_t k = 0; k < BATCH; k++) a[k] = (bi << 24) | k;
            int rc = omega_ds_gb10_run(op, a, NULL, o, BATCH);
            if (rc != OMEGA_NUMERIC_OK) { printf("    %s batch %u: device rc %d\n", omega_ds_op_name(op), bi, rc); dev_err = 1; break; }
            if (compare_range(op, (uint64_t)bi << 24, ((uint64_t)bi + 1) << 24, o, &bad, &fill, bad) != 0) { dev_err = 1; break; }
            checked += BATCH;
            if (bi % 16 == 15) { printf("    %s batch %u/256 done, mismatches so far %" PRIu64 "\n", omega_ds_op_name(op), bi + 1, bad); fflush(stdout); }
        }
        const char *v = bad ? "FAIL" : (dev_err || checked != (1ull << 32)) ? "NOT_RUN" : "PASS";
        any_fail |= !strcmp(v, "FAIL");
        any_notrun |= !strcmp(v, "NOT_RUN");
        printf("RESULT chip %s checked=%" PRIu64 " exhaustive=%s mismatches=%" PRIu64 " unwritten=%" PRIu64 " verdict=%s\n",
               omega_ds_op_name(op), checked, checked == (1ull << 32) ? "true" : "false", bad, fill, v);
    }
    const char *v = any_fail ? "FAIL" : any_notrun ? "NOT_RUN" : "PASS";
    printf("VERDICT %s\n", v);
    free(a); free(o);
    return strcmp(v, "PASS") ? 1 : 0;
}
#endif

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc >= 2 && !strcmp(argv[1], "--digest")) return digests();
    if (argc >= 3 && !strcmp(argv[1], "--host-all")) {
        OmegaDsOp op;
        if (find_op(argv[2], &op) != 0) { printf("unknown op %s\n", argv[2]); return 2; }
        return host_all(op);
    }
    if (argc >= 2 && !strcmp(argv[1], "--chip")) {
#ifdef OMEGA_NUMERIC_CPU_ONLY
        printf("VERDICT NOT_RUN (CPU-only build)\n");
        return 2;
#else
        OmegaDsOp ops[N_OPS];
        size_t n = 0;
        for (int i = 2; i < argc; i++) {
            if (n >= N_OPS || find_op(argv[i], &ops[n]) != 0) { printf("unknown op %s\nVERDICT NOT_RUN\n", argv[i]); return 2; }
            n++;
        }
        if (n == 0) { memcpy(ops, OPS, sizeof(OPS)); n = N_OPS; }
        return chip(ops, n);
#endif
    }
    return host_tier();
}
