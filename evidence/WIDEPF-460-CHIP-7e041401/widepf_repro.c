/* widepf_repro: omega #460 on the chip. The wide matmul (N 151936, lm_head width) at row counts whose
 * 16-row tile count is even. Pre-fix (df9bd9c4) the shape rule picked rt > 1, which the blocked generator
 * refuses (C store offset past the 24-bit STG immediate) -> CODEGEN_FAIL. Post-fix (7e041401) rt steps down.
 *
 * Per shape: shape rule (rt x ct), call 1 (rc, grid, launches, chip ns), call 2 (cache hit, bit-identical
 * to call 1), host reference (bf16-rounded inputs, double accumulation, rel to sum|terms|, bad if >= 1e-5:
 * the documented contract of omega_gpu_matmul_api.h:100-102), poison survivors.
 * Instrument negative controls: (a) one perturbed output element must be flagged by the same checker;
 * (b) --mutant shapes (API mutant kernel: last K step dropped) must be flagged by the checker.
 * The API's own single-thread oracle is switched off (minutes per shape); the reference here is the same
 * formula, threaded. Links the candidate's build/libomega_gpu.a unchanged.
 * usage: widepf_repro "m,k,n;..." [--mutant "m,k,n;..."] */
#include "omega_gpu_matmul_api.h"
#include "omega_blackwell_codegen.h"
#include "omega_blackwell_matmul.h"
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s; }
static void fill(float *a, size_t n, uint32_t *seed) { for (size_t i = 0; i < n; i++) a[i] = ((float)(lcg(seed) % 2001) - 1000.0f) / 1000.0f; }
static float bf16r(float f) { uint16_t h = omega_fp32_to_bf16(f); uint32_t u = (uint32_t)h << 16; float o; memcpy(&o, &u, 4); return o; }
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

typedef struct { uint32_t m, k, n, r0, r1; const float *ar, *bt, *c; double worst; size_t bad, poison; } Job;

static void *ref_rows(void *p) {
    Job *j = p;
    for (uint32_t i = j->r0; i < j->r1; i++) {
        const float *ai = j->ar + (size_t)i * j->k;
        for (uint32_t c = 0; c < j->n; c++) {
            const float *bc = j->bt + (size_t)c * j->k;
            double acc = 0.0, scale = 0.0;
            for (uint32_t t = 0; t < j->k; t++) { double pr = (double)ai[t] * (double)bc[t]; acc += pr; scale += fabs(pr); }
            float got = j->c[(size_t)i * j->n + c];
            if (got == -12345.0f) j->poison++;
            double rel = fabs(acc - (double)got) / (scale > 1e-6 ? scale : 1e-6);
            if (!(rel < 1e-5)) j->bad++;
            if (rel > j->worst || rel != rel) j->worst = rel;
        }
    }
    return NULL;
}

#define NT 16
static void check(uint32_t m, uint32_t k, uint32_t n, const float *ar, const float *bt, const float *c, double *worst, size_t *bad, size_t *poison) {
    pthread_t th[NT]; Job jb[NT];
    for (int t = 0; t < NT; t++) {
        jb[t] = (Job){ m, k, n, (uint32_t)((uint64_t)m * t / NT), (uint32_t)((uint64_t)m * (t + 1) / NT), ar, bt, c, 0.0, 0, 0 };
        pthread_create(&th[t], NULL, ref_rows, &jb[t]);
    }
    *worst = 0; *bad = 0; *poison = 0;
    for (int t = 0; t < NT; t++) { pthread_join(th[t], NULL); if (jb[t].worst > *worst) *worst = jb[t].worst; *bad += jb[t].bad; *poison += jb[t].poison; }
}

static int failed;

static void run(const char *tok, int mutant) {
    unsigned m, k, n;
    if (sscanf(tok, "%u,%u,%u", &m, &k, &n) != 3) { printf("bad shape %s\n", tok); failed++; return; }
    size_t na = (size_t)m * k, nb = (size_t)k * n, nc = (size_t)m * n;
    float *a = malloc(na * 4), *b = malloc(nb * 4), *c = malloc(nc * 4), *c2 = malloc(nc * 4), *ar = malloc(na * 4), *bt = malloc(nb * 4);
    if (!a || !b || !c || !c2 || !ar || !bt) { printf("out of host memory %s\n", tok); failed++; return; }
    uint32_t seed = 0x5eed0000u ^ (m << 20) ^ (k << 10) ^ n;
    fill(a, na, &seed); fill(b, nb, &seed);
    for (size_t i = 0; i < na; i++) ar[i] = bf16r(a[i]);
    for (size_t t = 0; t < k; t++) for (size_t j = 0; j < n; j++) bt[j * k + t] = bf16r(b[t * n + j]);
    uint32_t mt = (m + 15u) / 16u, nt = (n + 7u) / 8u, rt = 0, ct = 0;
    omega_matmul_frag_block_shape(mt, nt, &rt, &ct);
    for (size_t i = 0; i < nc; i++) c[i] = c2[i] = -12345.0f;
    omega_gpu_matmul_test_set_mutant(mutant);
    OmegaGpuMatmulInfo i1, i2; memset(&i1, 0, sizeof i1); memset(&i2, 0, sizeof i2);
    double t0 = now_ms();
    int rc = omega_gpu_matmul_f32(m, k, n, a, b, c, &i1);
    double w1 = now_ms() - t0;
    char err1[200]; snprintf(err1, sizeof err1, "%s", rc ? omega_gpu_matmul_last_error() : "");
    int rc2 = rc == OMEGA_GPU_MATMUL_OK ? omega_gpu_matmul_f32(m, k, n, a, b, c2, &i2) : rc;
    size_t rep = 0;
    if (rc == OMEGA_GPU_MATMUL_OK && rc2 == OMEGA_GPU_MATMUL_OK) for (size_t i = 0; i < nc; i++) if (memcmp(&c[i], &c2[i], 4)) rep++;
    double worst = -1; size_t bad = 0, poison = 0, nbad = 0; double nworst = 0;
    double tr = now_ms();
    if (rc == OMEGA_GPU_MATMUL_OK) {
        check(m, k, n, ar, bt, c, &worst, &bad, &poison);
        /* negative control (a): perturb one element of a copy, re-check that row only */
        size_t r = m / 2, col = n / 2;
        float save = c[r * n + col]; c[r * n + col] = save + 1.0f;
        Job one = { m, k, n, (uint32_t)r, (uint32_t)r + 1, ar, bt, c, 0.0, 0, 0 };
        ref_rows(&one); nbad = one.bad; nworst = one.worst;
        c[r * n + col] = save;
    }
    double ref_ms = now_ms() - tr;
    int pass;
    if (mutant) pass = rc == OMEGA_GPU_MATMUL_OK && bad > 0;
    else pass = rc == OMEGA_GPU_MATMUL_OK && rc2 == OMEGA_GPU_MATMUL_OK && i2.kernel_cache_hit && rep == 0 && bad == 0 && poison == 0 && nbad == 1;
    if (!pass) failed++;
    printf("%s %s %ux%ux%u mt=%u nt=%u rule_rt=%u rule_ct=%u rc=%s err=\"%s\" launches=%u rows_per_call=%u grid=%ux%u chip_ns=%" PRIu64
           " call_ms=%.1f wall_ms=%.1f rc2=%s hit2=%d repeat_mismatch=%zu ref_bad=%zu ref_max_rel=%.3g poison_left=%zu negctl_bad=%zu negctl_rel=%.3g ref_ms=%.0f\n",
           mutant ? (pass ? "MUTANT_CAUGHT" : "MUTANT_MISSED") : (pass ? "PASS" : "FAIL"), mutant ? "mutant" : "normal", m, k, n, mt, nt, rt, ct,
           omega_gpu_matmul_rc_name(rc), err1, i1.chip_calls, i1.rows_per_call, i1.grid_x, i1.grid_y, i1.elapsed_ns, i1.call_ns / 1e6, w1,
           omega_gpu_matmul_rc_name(rc2), i2.kernel_cache_hit, rep, bad, worst, poison, nbad, nworst, ref_ms);
    fflush(stdout);
    omega_gpu_matmul_test_set_mutant(0);
    free(a); free(b); free(c); free(c2); free(ar); free(bt);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s \"m,k,n;...\" [--mutant \"m,k,n;...\"]\n", argv[0]); return 2; }
    omega_gpu_matmul_set_oracle(0);
    for (int a = 1; a < argc; a++) {
        int mutant = 0;
        if (strcmp(argv[a], "--mutant") == 0 && a + 1 < argc) { mutant = 1; a++; }
        char *copy = strdup(argv[a]), *save = NULL;
        for (char *tok = strtok_r(copy, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
            run(tok, mutant);
            if (omega_gpu_matmul_is_blocked()) { printf("device latched after an uncertain completion; stopping\n"); free(copy); return 3; }
        }
        free(copy);
    }
    printf("WIDEPF_REPRO: %s (failed %d)\n", failed ? "FAIL" : "PASS", failed);
    return failed ? 1 : 0;
}
