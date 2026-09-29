/* MA-6 host checks (no GPU): for every realization and every pre-registered
 * shape, plan + pack + CPU model of the kernel's index arithmetic must equal
 * oma_rz_oracle bit for bit; every kernel must build (register allocation,
 * scheduler, encoder); refusals fail closed. spec/mixed-algebra-ma6-gpu.md */
#include "algebra/gpu/oma_gpu.h"
#include "algebra/realize_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static uint64_t rs = 0x243f6a8885a308d3ull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 11); }

static void gen(int8_t *w, size_t mn, double sparsity, int8_t *x, size_t n, int pattern) {
    for (size_t i = 0; i < mn; i++) {
        if (pattern == 1) w[i] = 1;
        else if (pattern == 2) w[i] = -1;
        else w[i] = (rnd() % 10000) < (uint32_t)(sparsity * 10000) ? 0 : ((rnd() & 1) ? 1 : -1);
    }
    for (size_t i = 0; i < n; i++) x[i] = pattern ? (int8_t)-128 : (int8_t)(rnd() & 0xff);
}

static int one(omg_kind kind, uint32_t m, uint32_t n, double sp, int pattern, int verbose) {
    omg_geom g;
    if (omg_plan(kind, m, n, &g) != 0) { CHECK(0, "%s plan m=%u n=%u", omg_kind_name(kind), m, n); return -1; }
    int8_t *w = malloc((size_t)m * n), *x = malloc(n);
    int32_t *yo = malloc((size_t)m * 4), *ym = malloc((size_t)g.m_pad * 4);
    uint32_t *w0 = malloc(g.w0_bytes), *w1 = g.w1_bytes ? malloc(g.w1_bytes) : NULL, *xd = malloc(g.x_bytes);
    gen(w, (size_t)m * n, sp, x, n, pattern);
    oma_rz_oracle(w, m, n, x, yo);
    CHECK(omg_pack_weights(&g, w, w0, w1) == 0, "pack");
    CHECK(omg_pack_x(&g, x, xd) == 0, "pack x");
    omg_model(&g, w0, w1, xd, ym);
    int bad = 0;
    for (uint32_t i = 0; i < m; i++) bad += ym[i] != yo[i];
    for (uint32_t i = m; i < g.m_pad; i++) bad += ym[i] != 0;
    CHECK(!bad, "%s m=%u n=%u sp=%.1f pattern=%d: %d rows differ from oracle", omg_kind_name(kind), m, n, sp, pattern, bad);
    omg_kernel k;
    int rc = omg_build(&g, &k);
    CHECK(rc == 0, "%s m=%u n=%u: kernel build", omg_kind_name(kind), m, n);
    if (rc == 0 && verbose)
        printf("  %-18s m=%-5u n=%-6u S=%-5u U=%u threads=%-6u cta=%-3u grid=%-4u insns=%-3zu gpr=%u\n",
               omg_kind_name(kind), m, n, g.S, g.U, g.threads, g.cta, g.grid, k.insn_count, k.gpr);
    if (rc == 0) omg_kernel_free(&k);
    free(w); free(x); free(yo); free(ym); free(w0); free(w1); free(xd);
    return bad ? -1 : 0;
}

int main(void) {
    const omg_kind kinds[4] = { OMG_G1_I8, OMG_G2A_CRUMB, OMG_G2B_PLANE, OMG_G3_BF16 };
    const uint32_t ns[3] = { 1024, 4096, 16384 }, ms[3] = { 1, 64, 4096 };
    const double sps[2] = { 0.0, 0.6 };
    for (int a = 0; a < 4; a++)
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                for (int s = 0; s < 2; s++) one(kinds[a], ms[j], ns[i], sps[s], 0, s == 0);
    /* magnitude edges: x = -128 everywhere, all +1 / all -1 weights, n = 16384 (|y| = 2^21) */
    for (int a = 0; a < 4; a++) { one(kinds[a], 64, 16384, 0, 1, 0); one(kinds[a], 64, 16384, 0, 2, 0); }
    /* G3 declared domain edge: n = 32768 with |y| = 2^22 must still be exact in the model */
    one(OMG_G3_BF16, 16, 32768, 0, 1, 0);
    /* refusals */
    omg_geom g;
    CHECK(omg_plan(OMG_G3_BF16, 16, 65536, &g) != 0, "G3 refuses n beyond its exact domain");
    CHECK(omg_plan(OMG_G1_I8, 3, 1024, &g) != 0, "non power of two m refused");
    CHECK(omg_plan(OMG_G1_I8, 4, 512, &g) != 0, "n < 1024 refused");
    omg_plan(OMG_G1_I8, 1, 1024, &g);
    int8_t *w = calloc(1024, 1);
    uint32_t *w0 = calloc(g.w0_bytes, 1);
    w[5] = 2;
    CHECK(omg_pack_weights(&g, w, w0, NULL) != 0, "pack refuses a weight outside {-1,0,+1}");
    omg_plan(OMG_G2B_PLANE, 1, 1024, &g);
    w[5] = 1;
    CHECK(omg_pack_weights(&g, w, w0, NULL) != 0, "G2B pack refuses a missing M plane");
    free(w); free(w0);
    printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
