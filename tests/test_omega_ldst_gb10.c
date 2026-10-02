/*
 * E1 row 2: general load/store on the GB10 (src/omega_numeric_ldst_gb10.h).
 *
 * Modes (one per run):
 *   (none)          host tier: spec rules, kernel build/check, host model equals an independent
 *                   byte-level oracle on every spec of the table, negative tests. No device.
 *   --dump DIR      write each table kernel as DIR/kNNN.bin and its expected listing as DIR/kNNN.lst
 *                   (consumed by tools/ldst_nvdisasm_check.sh)
 *   --chip          GB10: every spec of the table, chip output compared byte for byte with the host
 *                   model. One RESULT line per spec, then a VERDICT line. A byte still equal to the
 *                   fill pattern where the model wrote something is counted as "unwritten" (the C3
 *                   unwritten-output signature), separately from wrong values.
 */
#include "omega_numeric.h"
#include "omega_numeric_ldst_gb10.h"
#include "sha256.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SPECS 400
#define COUNT 1000u
#define FILL 0xA5u

static OmegaLdstSpec g_specs[MAX_SPECS];
static size_t g_nspecs;
static int g_pass, g_fail;
static void check(int ok, const char *id) {
    if (ok) { g_pass++; printf("[PASS] %s\n", id); } else { g_fail++; printf("[FAIL] %s\n", id); }
}

static uint64_t rng_state = 0x243f6a8885a308d3ull;
static uint32_t rnd(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17; return (uint32_t)(rng_state >> 16); }

/* Every valid (ld, st) pair; for each, offsets and strides chosen from a fixed set so negative and
 * positive offsets, packed and sparse strides, and 24-bit-field extremes are all covered. */
static void build_table(void) {
    static const int soff[] = { 0, 1, -1, 2, -3, 5 };          /* multiplied by the width */
    static const unsigned smul[] = { 1, 2, 3, 5 };
    g_nspecs = 0;
    for (unsigned l = 0; l < OMEGA_LD_COUNT; l++)
        for (unsigned t = 0; t < OMEGA_ST_COUNT; t++) {
            size_t lb = omega_ld_bytes(l), sb = omega_st_bytes(t);
            OmegaLdstSpec probe = { (uint8_t)l, (uint8_t)t, (uint32_t)lb, (uint32_t)sb, 0, 0 };
            if (omega_ldst_check_spec(&probe, 1, NULL, 0) != 0) continue;
            for (unsigned v = 0; v < 6; v++) {
                OmegaLdstSpec s;
                s.ld = (uint8_t)l; s.st = (uint8_t)t;
                s.in_stride = (uint32_t)(lb * smul[(v + l) % 4]);
                s.out_stride = (uint32_t)(sb * smul[(v + t) % 4]);
                s.in_off = (int32_t)lb * soff[(v + t) % 6];
                s.out_off = (int32_t)sb * soff[(v + l + 1) % 6];
                if (g_nspecs < MAX_SPECS) g_specs[g_nspecs++] = s;
            }
        }
    /* 24-bit offset extremes and a large stride (one wide spec each) */
    OmegaLdstSpec e1 = { OMEGA_LD_B32, OMEGA_ST_B32, 4, 4, 0, 0 };
    e1.in_off = 0;  /* extremes of the field are exercised by the encoder tests below; the chip uses the pad */
    g_specs[g_nspecs++] = e1;
    OmegaLdstSpec e2 = { OMEGA_LD_B128, OMEGA_ST_B128, 4096, 64, 16, -16 };
    g_specs[g_nspecs++] = e2;
    OmegaLdstSpec e3 = { OMEGA_LD_S8, OMEGA_ST_B32, 1, 4, 0, 0 };
    g_specs[g_nspecs++] = e3;
    OmegaLdstSpec e4 = { OMEGA_LD_S16, OMEGA_ST_B32, 2, 4, -2, 4 };
    g_specs[g_nspecs++] = e4;
}

typedef struct { uint8_t *in, *out; size_t in_len, out_len; } Bufs;

static int alloc_bufs(const OmegaLdstSpec *s, Bufs *b) {
    int64_t il, ih, ol, oh;
    if (omega_ldst_extent(s, COUNT, &il, &ih, &ol, &oh) != 0) return -1;
    size_t in_need = (size_t)(ih > (int64_t)COUNT * 4 ? ih : (int64_t)COUNT * 4) + OMEGA_LDST_PAD;
    b->in_len = (in_need + 15) & ~15ull;
    b->out_len = ((size_t)oh + OMEGA_LDST_PAD + 15) & ~15ull;
    b->in = aligned_alloc(16, b->in_len);
    b->out = aligned_alloc(16, b->out_len);
    if (!b->in || !b->out) return -1;
    for (size_t i = 0; i < b->in_len; i++) b->in[i] = (uint8_t)rnd();
    memset(b->out, FILL, b->out_len);
    return 0;
}

/* Independent oracle: typed C loads and stores, no shared code with the host model. */
static void oracle(const OmegaLdstSpec *s, const Bufs *b, uint8_t *out) {
    for (size_t i = 0; i < COUNT; i++) {
        const uint8_t *src = b->in + OMEGA_LDST_PAD + (int64_t)s->in_off + (int64_t)i * s->in_stride;
        uint8_t *dst = out + OMEGA_LDST_PAD + (int64_t)s->out_off + (int64_t)i * s->out_stride;
        uint32_t lo = 0, w[4] = { 0, 0, 0, 0 };
        switch (s->ld) {
        case OMEGA_LD_U8: lo = *src; break;
        case OMEGA_LD_S8: lo = (uint32_t)(int32_t)(int8_t)*src; break;
        case OMEGA_LD_U16: { uint16_t v; memcpy(&v, src, 2); lo = v; break; }
        case OMEGA_LD_S16: { int16_t v; memcpy(&v, src, 2); lo = (uint32_t)(int32_t)v; break; }
        case OMEGA_LD_B32: memcpy(&lo, src, 4); break;
        case OMEGA_LD_B64: memcpy(w, src, 8); lo = w[0]; break;
        case OMEGA_LD_B128: memcpy(w, src, 16); lo = w[0]; break;
        }
        if (s->ld <= OMEGA_LD_B32) w[0] = lo;   /* B64 and B128 already filled w */
        switch (s->st) {
        case OMEGA_ST_B8: *dst = (uint8_t)w[0]; break;
        case OMEGA_ST_B16: { uint16_t v = (uint16_t)w[0]; memcpy(dst, &v, 2); break; }
        case OMEGA_ST_B32: memcpy(dst, &w[0], 4); break;
        case OMEGA_ST_B64: memcpy(dst, w, 8); break;
        case OMEGA_ST_B128: memcpy(dst, w, 16); break;
        }
    }
}

static int dump(const char *dir) {
    for (size_t k = 0; k < g_nspecs; k++) {
        uint8_t code[OMEGA_LDST_CODE_BYTES];
        static char lst[8192];
        size_t cl = 0;
        char path[512];
        if (omega_ldst_build_kernel(&g_specs[k], code, sizeof code, &cl) != 0 || omega_ldst_listing(&g_specs[k], lst, sizeof lst) != 0) return 1;
        snprintf(path, sizeof path, "%s/k%03zu.bin", dir, k);
        FILE *f = fopen(path, "wb"); if (!f) return 1; fwrite(code, 1, cl, f); fclose(f);
        snprintf(path, sizeof path, "%s/k%03zu.lst", dir, k);
        f = fopen(path, "w"); if (!f) return 1; fputs(lst, f); fclose(f);
    }
    printf("dumped %zu kernels\n", g_nspecs);
    return 0;
}

static int host_tier(void) {
    char why[200], d[96];
    int all_check = 1, all_model = 1, all_build = 1;
    for (size_t k = 0; k < g_nspecs; k++) {
        const OmegaLdstSpec *s = &g_specs[k];
        uint8_t code[OMEGA_LDST_CODE_BYTES];
        size_t cl = 0;
        if (omega_ldst_check_spec(s, COUNT, why, sizeof why) != 0) { all_check = 0; printf("  spec %zu (%s) refused: %s\n", k, omega_ldst_describe(s, d, sizeof d), why); continue; }
        if (omega_ldst_build_kernel(s, code, sizeof code, &cl) != 0 || omega_ldst_check_kernel(s, code, cl, why, sizeof why) != 0) { all_build = 0; printf("  spec %zu build/check: %s\n", k, why); }
        Bufs b;
        if (alloc_bufs(s, &b) != 0) { all_model = 0; continue; }
        uint8_t *want = malloc(b.out_len);
        memcpy(want, b.out, b.out_len);
        oracle(s, &b, want);
        if (omega_ldst_host_run(s, b.in, b.in_len, b.out, b.out_len, OMEGA_LDST_PAD, COUNT) != 0 || memcmp(want, b.out, b.out_len) != 0) {
            all_model = 0; printf("  spec %zu (%s): host model != oracle\n", k, omega_ldst_describe(s, d, sizeof d));
        }
        free(want); free(b.in); free(b.out);
    }
    char id[96];
    snprintf(id, sizeof id, "LDST_TABLE_SPECS_VALID (%zu specs)", g_nspecs); check(all_check, id);
    check(all_build, "LDST_KERNELS_BUILD_AND_PASS_STRUCTURAL_CHECK");
    check(all_model, "LDST_HOST_MODEL_EQUALS_BYTE_ORACLE");

    /* every (ld, st) pair of the contract appears in the table */
    int seen[OMEGA_LD_COUNT][OMEGA_ST_COUNT] = { { 0 } }, miss = 0;
    for (size_t k = 0; k < g_nspecs; k++) seen[g_specs[k].ld][g_specs[k].st] = 1;
    for (unsigned l = 0; l < OMEGA_LD_COUNT; l++) for (unsigned t = 0; t < OMEGA_ST_COUNT; t++) {
        OmegaLdstSpec p = { (uint8_t)l, (uint8_t)t, (uint32_t)omega_ld_bytes(l), (uint32_t)omega_st_bytes(t), 0, 0 };
        if (omega_ldst_check_spec(&p, 1, NULL, 0) == 0 && !seen[l][t]) miss++;
    }
    check(miss == 0, "LDST_TABLE_COVERS_EVERY_VALID_LOAD_STORE_PAIR");

    /* offset field extremes: the encoder carries the full signed 24-bit range */
    OmegaLdstSpec x = { OMEGA_LD_B32, OMEGA_ST_B32, 4, 4, 0x7ffffc, -0x800000 };
    uint8_t code[OMEGA_LDST_CODE_BYTES];
    size_t cl = 0;
    check(omega_ldst_build_kernel(&x, code, sizeof code, &cl) == 0 && omega_ldst_check_kernel(&x, code, cl, why, sizeof why) == 0, "LDST_OFFSET_24BIT_EXTREMES_ENCODE");

    /* negative: each refusal rule */
    struct { OmegaLdstSpec s; size_t n; const char *id; } neg[] = {
        { { OMEGA_LD_B32, OMEGA_ST_B64, 4, 8, 0, 0 }, 4, "NEG_STORE_WIDER_THAN_LOAD_REFUSED" },
        { { OMEGA_LD_U8, OMEGA_ST_B64, 1, 8, 0, 0 }, 4, "NEG_NARROW_LOAD_WIDE_STORE_REFUSED" },
        { { OMEGA_LD_B32, OMEGA_ST_B32, 2, 4, 0, 0 }, 4, "NEG_STRIDE_BELOW_WIDTH_REFUSED" },
        { { OMEGA_LD_B32, OMEGA_ST_B32, 6, 4, 0, 0 }, 4, "NEG_STRIDE_NOT_MULTIPLE_REFUSED" },
        { { OMEGA_LD_B32, OMEGA_ST_B32, 4, 4, 2, 0 }, 4, "NEG_INPUT_OFFSET_MISALIGNED_REFUSED" },
        { { OMEGA_LD_B32, OMEGA_ST_B32, 4, 4, 0, 6 }, 4, "NEG_OUTPUT_OFFSET_MISALIGNED_REFUSED" },
        { { OMEGA_LD_B32, OMEGA_ST_B32, 4, 4, 0x800000, 0 }, 4, "NEG_OFFSET_OUTSIDE_24BIT_REFUSED" },
        { { OMEGA_LD_B32, OMEGA_ST_B32, 4, 4, -0x800004, 0 }, 4, "NEG_NEGATIVE_OFFSET_OUTSIDE_24BIT_REFUSED" },
        { { OMEGA_LD_B32, OMEGA_ST_B32, 0, 4, 0, 0 }, 4, "NEG_ZERO_STRIDE_REFUSED" },
        { { OMEGA_LD_B32, OMEGA_ST_B32, 1u << 24, 4, 0, 0 }, 4, "NEG_STRIDE_2_POW_24_REFUSED" },
        { { 9, OMEGA_ST_B32, 4, 4, 0, 0 }, 4, "NEG_UNKNOWN_LOAD_KIND_REFUSED" },
        { { OMEGA_LD_B32, 9, 4, 4, 0, 0 }, 4, "NEG_UNKNOWN_STORE_KIND_REFUSED" },
        { { OMEGA_LD_B32, OMEGA_ST_B32, 4, 4, 0, 0 }, 0, "NEG_ZERO_COUNT_REFUSED" },
    };
    for (size_t i = 0; i < sizeof neg / sizeof neg[0]; i++) check(omega_ldst_check_spec(&neg[i].s, neg[i].n, why, sizeof why) != 0, neg[i].id);

    /* negative: a mutated kernel word is caught, each patched field */
    OmegaLdstSpec ok = { OMEGA_LD_B64, OMEGA_ST_B64, 16, 24, 8, -16 };
    uint8_t good[OMEGA_LDST_CODE_BYTES];
    omega_ldst_build_kernel(&ok, good, sizeof good, &cl);
    struct { size_t byte; uint8_t xor; const char *id; } mut[] = {
        { 16 * 13 + 5, 0x02, "NEG_LDG_OFFSET_BIT_FLIPPED_CAUGHT" },
        { 16 * 13 + 9, 0x02, "NEG_LDG_SIZE_FIELD_CHANGED_CAUGHT" },
        { 16 * 13 + 2, 0x04, "NEG_LDG_DEST_REGISTER_CHANGED_CAUGHT" },
        { 16 * 18 + 9, 0x04, "NEG_STG_SIZE_FIELD_CHANGED_CAUGHT" },
        { 16 * 18 + 5, 0x01, "NEG_STG_OFFSET_BIT_FLIPPED_CAUGHT" },
        { 16 * 12 + 4, 0x01, "NEG_INPUT_STRIDE_WORD_CHANGED_CAUGHT" },
        { 16 * 16 + 4, 0x01, "NEG_OUTPUT_STRIDE_WORD_CHANGED_CAUGHT" },
        { 16 * 17 + 15, 0x10, "NEG_IADD3_CONTROL_WORD_CHANGED_CAUGHT" },
        { 16 * 5 + 1, 0x01, "NEG_PROLOGUE_WORD_CHANGED_CAUGHT" },
        { 16 * 20 + 0, 0x01, "NEG_EPILOGUE_WORD_CHANGED_CAUGHT" },
    };
    for (size_t i = 0; i < sizeof mut / sizeof mut[0]; i++) {
        uint8_t m[OMEGA_LDST_CODE_BYTES];
        memcpy(m, good, sizeof m);
        m[mut[i].byte] ^= mut[i].xor;
        check(omega_ldst_check_kernel(&ok, m, sizeof m, why, sizeof why) != 0, mut[i].id);
    }
    check(omega_ldst_check_kernel(&ok, good, sizeof good - 16, why, sizeof why) != 0, "NEG_SHORT_KERNEL_CAUGHT");

    /* negative: host model refuses an access outside the buffer */
    uint8_t tiny_in[64] = { 0 }, tiny_out[64] = { 0 };
    OmegaLdstSpec small = { OMEGA_LD_B32, OMEGA_ST_B32, 4, 4, 0, 0 };
    check(omega_ldst_host_run(&small, tiny_in, sizeof tiny_in, tiny_out, sizeof tiny_out, 0, 16) == 0, "LDST_HOST_RUN_INSIDE_BUFFER_OK");
    check(omega_ldst_host_run(&small, tiny_in, sizeof tiny_in, tiny_out, sizeof tiny_out, 0, 17) != 0, "NEG_HOST_RUN_PAST_END_REFUSED");
    OmegaLdstSpec neg_off = { OMEGA_LD_B32, OMEGA_ST_B32, 4, 4, -4, 0 };
    check(omega_ldst_host_run(&neg_off, tiny_in, sizeof tiny_in, tiny_out, sizeof tiny_out, 0, 4) != 0, "NEG_HOST_RUN_BEFORE_START_REFUSED");
    check(omega_ldst_gb10_run(&small, tiny_in, sizeof tiny_in, tiny_out, sizeof tiny_out, 0, 17) != 0, "NEG_GB10_RUN_PAST_END_REFUSED_BEFORE_DEVICE");

    printf("Gate LDST host: PASSED=%d FAILED=%d\n", g_pass, g_fail);
    printf("VERDICT %s\n", g_fail ? "HOST_REGRESSION" : "PASS_HOST");
    return g_fail ? 1 : 0;
}

static int chip(void) {
    int bad_specs = 0;
    for (size_t k = 0; k < g_nspecs; k++) {
        const OmegaLdstSpec *s = &g_specs[k];
        Bufs b;
        char d[96];
        if (alloc_bufs(s, &b) != 0) return 2;
        uint8_t *want = malloc(b.out_len), *got = malloc(b.out_len);
        memcpy(want, b.out, b.out_len);
        memcpy(got, b.out, b.out_len);
        if (omega_ldst_host_run(s, b.in, b.in_len, want, b.out_len, OMEGA_LDST_PAD, COUNT) != 0) return 2;
        int rc = omega_ldst_gb10_run(s, b.in, b.in_len, got, b.out_len, OMEGA_LDST_PAD, COUNT);
        uint64_t mism = 0, unwritten = 0;
        if (rc == 0)
            for (size_t i = 0; i < b.out_len; i++)
                if (got[i] != want[i]) { mism++; if (got[i] == FILL && want[i] != FILL) unwritten++; }
        int ok = rc == 0 && mism == 0;
        if (!ok) bad_specs++;
        printf("RESULT chip spec=%zu %s count=%u rc=%d mismatches=%" PRIu64 " unwritten=%" PRIu64 " verdict=%s\n", k,
               omega_ldst_describe(s, d, sizeof d), COUNT, rc, mism, unwritten, ok ? "PASS" : "FAIL");
        fflush(stdout);
        free(want); free(got); free(b.in); free(b.out);
    }
    printf("VERDICT %s\n", bad_specs ? "FAIL" : "PASS");
    return bad_specs ? 1 : 0;
}

int main(int argc, char **argv) {
    build_table();
    if (argc >= 3 && !strcmp(argv[1], "--dump")) return dump(argv[2]);
    if (argc >= 2 && !strcmp(argv[1], "--chip")) return chip();
    return host_tier();
}
