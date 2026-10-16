/* gpu_tgemm_test.c (trainer lane E1, 2026-10-16): device-resident bf16 training GEMM (omega_blackwell_codegen_tgemm.h,
 * omega_gpu_tgemm_api.h). C[M][N] f32 = A[M][K] bf16 x B[K][N] bf16, row-major, NN.
 *   --host-only  budgets of the production specs: instructions, allocator peak and declared registers (refused past 1000 / 128)
 *   --sim        the kernel in the warp simulator (both fragment orders) bit-identical to the C emulation of the HMMA chain
 *                (tests/bw_hmma_model.h, GB10 member) over the whole output, tile K ranges (kb0, nkb), mutants 1-4 caught
 *   --chip       (caller holds the GB10 quiet lock) every output against a double reference over the same bf16-rounded inputs
 *                (|got - ref| / max(sum |terms|, 1e-6) < 1e-5: the rule of gpu_matmul_api_test.c), first with uncached (host)
 *                buffers, then with GPU-cached device buffers (trainer rule R-8); bit-identical repeats, poison scan (every output
 *                written), negative control (mutant 4 differs), medians of chip time and whole-call time, TFLOPS and percent of
 *                the measured bf16 HMMA peak (--peak, default 120.64: evidence TGEMM-CUT1 peak.log, gpu_tc_peak_test)
 * Verdict line last: "GPU_TGEMM: PASS|FAIL". */
#include "omega_blackwell_codegen_tgemm.h"
#include "omega_gpu_tgemm_api.h"
#include "omega_gpu_prefill.h"
#include "bw_hmma_model.h"
#include "bw_warp_sim_tc.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const HmmaModel GB10 = { 16, 2, 0, 2, 0 };
static uint64_t rng = 0x243f6a8885a308d3ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }
static float frnd(void) { return ((float)(rnd() & 0xffffff) / 16777216.0f) * 2.0f - 1.0f; }
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static uint16_t bf16_rn(float f) { uint32_t u = f2u(f); if ((u & 0x7fffffffu) > 0x7f800000u) return 0x7fc0u; return (uint16_t)((u + 0x7fffu + ((u >> 16) & 1u)) >> 16); }
static float bf(uint16_t h) { return u2f((uint32_t)h << 16); }
static double now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1e9 + (double)t.tv_nsec; }

typedef struct { uint32_t M, N, K; uint16_t *a, *b; } Prob;
static void prob_make(Prob *p, uint32_t M, uint32_t N, uint32_t K) {
    p->M = M; p->N = N; p->K = K;
    p->a = malloc((size_t)M * K * 2); p->b = malloc((size_t)K * N * 2);
    for (size_t i = 0; i < (size_t)M * K; i++) p->a[i] = bf16_rn(frnd() * (rnd() % 16 == 0 ? 8.0f : 1.0f));
    for (size_t i = 0; i < (size_t)K * N; i++) p->b[i] = bf16_rn(frnd() * (rnd() % 16 == 0 ? 8.0f : 1.0f));
}
static void prob_free(Prob *p) { free(p->a); free(p->b); }

/* the kernel's arithmetic for output (i, c) over blocks kb0 .. kb0 + nkb - 1: one fused 16-term HMMA per (32-k group, step) */
static float emu(const Prob *p, uint32_t i, uint32_t c, uint32_t kb0, uint32_t nkb) {
    float acc = 0.0f;
    for (uint32_t G = kb0 * 4u; G < (kb0 + nkb) * 4u; G++) for (uint32_t s = 0; s < 2; s++) {
        float a[16], b[16]; int m = 0;
        for (uint32_t t = 0; t < 4; t++) for (uint32_t j = 0; j < 4; j++) {
            const uint32_t k = G * 32u + 8u * t + 4u * s + j;
            a[m] = bf(p->a[(size_t)i * p->K + k]); b[m] = bf(p->b[(size_t)k * p->N + c]); m++;
        }
        acc = hm_dot16(&GB10, a, b, acc);
    }
    return acc;
}

static int host_only(void) {
    int fail = 0;
    static const struct { OmegaTgemmSpec s; int production; } specs[] = {
        { { 2304, 768, 8, 2, 16, 32, 1, 0, 0, 0 }, 1 }, { { 768, 3072, 8, 2, 16, 32, 1, 0, 0, 0 }, 1 }, { { 50304, 768, 4, 2, 8, 32, 1, 0, 0, 0 }, 1 },
        { { 768, 2304, 4, 2, 16, 32, 2, 0, 0, 0 }, 0 }, { { 768, 768, 4, 2, 16, 32, 1, 0, 0, 0 }, 0 }, { { 768, 768, 4, 2, 8, 32, 1, 0, 0, 0 }, 0 }, { { 768, 768, 2, 2, 8, 32, 1, 0, 0, 0 }, 0 },
        { { 768, 768, 8, 2, 8, 32, 1, 0, 0, 0 }, 0 },
    };
    static BlackwellIRProgram prog;
    for (size_t i = 0; i < sizeof specs / sizeof specs[0]; i++) {
        const OmegaTgemmSpec *s = &specs[i].s;
        int rc = omega_blackwell_codegen_tgemm_prog(s, &prog);
        uint32_t peak = rc == 0 ? prog.regalloc.peak_gpr_usage : 0, decl = omega_bw_gpr_declared(peak);
        int ok = rc == 0 && prog.count <= OMEGA_TGEMM_MAX_INSNS && decl <= 128u && 32u * s->warps * decl <= 65536u;
        printf("spec k %u n %u rt %u ct %u warps %u kc %u aqn %u: %s, %zu instructions, peak %u, declared %u, shared %u B -> %s%s\n", s->k, s->n, s->rt, s->ct, s->warps, s->kc, s->aqn,
               rc ? "REFUSED" : "built", rc ? (size_t)0 : prog.count, peak, decl, omega_tgemm_shared_bytes(s), ok ? "fits" : "DOES NOT FIT", specs[i].production ? " (production)" : "");
        if (!ok && specs[i].production) fail++;
    }
    return fail;
}

typedef struct { uint32_t M, N, K, rt, warps, aqn, kb0, nkb; } SimCase;
static int sim_run(const Prob *p, const SimCase *c, int mutant, int order, float *y, char *msg, size_t msgn) {
    static BlackwellIRProgram prog;
    OmegaTgemmSpec sp = { p->K, p->N, c->rt, 2, c->warps, 32, c->aqn, OMEGA_TGEMM_LAYOUT_NN, OMEGA_TGEMM_EPI_F32, mutant };
    if (omega_blackwell_codegen_tgemm_prog(&sp, &prog) != 0) { snprintf(msg, msgn, "codegen refused (spec_ok %d)", omega_tgemm_spec_ok(&sp)); return -1; }
    const uint32_t bm = omega_tgemm_bm(&sp), span = omega_tgemm_span(&sp), nt = p->M / bm;
    OmegaTgemmTile *tiles = calloc(nt, sizeof *tiles);
    for (uint32_t t = 0; t < nt; t++) { tiles[t].c_va = (uintptr_t)y; tiles[t].row0 = t * bm; tiles[t].kb0 = c->kb0; tiles[t].nkb = c->nkb; }
    memset(y, 0xee, (size_t)p->M * p->N * 4);
    uint8_t *smem = malloc(omega_tgemm_shared_bytes(&sp));
    static WsSim s; memset(&s, 0, sizeof s);
    s.p = &prog; s.threads = 32u * sp.warps; s.smem = smem; s.smem_bytes = omega_tgemm_shared_bytes(&sp); s.order = order;
    const uint64_t a[3] = { (uintptr_t)tiles, (uintptr_t)p->a, (uintptr_t)p->b };
    for (int i = 0; i < 3; i++) { s.cbank[(0x380 + 8 * i) / 4] = (uint32_t)a[i]; s.cbank[(0x380 + 8 * i) / 4 + 1] = (uint32_t)(a[i] >> 32); }
    s.ranges[0].lo = (uintptr_t)tiles; s.ranges[0].hi = (uintptr_t)(tiles + nt);
    s.ranges[1].lo = (uintptr_t)p->a; s.ranges[1].hi = (uintptr_t)(p->a + (size_t)p->M * p->K);
    s.ranges[2].lo = (uintptr_t)p->b; s.ranges[2].hi = (uintptr_t)(p->b + (size_t)p->K * p->N);
    s.ranges[3].lo = (uintptr_t)y; s.ranges[3].hi = (uintptr_t)(y + (size_t)p->M * p->N);
    s.nranges = 4;
    int err = ws_run(&s, p->N / span, nt);
    if (err) snprintf(msg, msgn, "simulator %s", s.msg);
    free(tiles); free(smem);
    return err ? -1 : 0;
}
static size_t sim_compare(const Prob *p, const float *y, uint32_t kb0, uint32_t nkb) {
    size_t bad = 0;
    for (uint32_t i = 0; i < p->M; i++) for (uint32_t c = 0; c < p->N; c++) {
        const float want = emu(p, i, c, kb0, nkb);
        if (f2u(y[(size_t)i * p->N + c]) != f2u(want)) { if (getenv("TGDBG") && bad < 8) printf("  row %u col %u got %.9g want %.9g\n", i, c, y[(size_t)i * p->N + c], want); bad++; }
    }
    return bad;
}
static int sim_tests(void) {
    int fail = 0; char msg[256];
    static const SimCase cs[] = {
        { 128, 512, 256, 4, 16, 1, 0, 2 }, { 64, 256, 384, 2, 8, 1, 1, 2 }, { 128, 256, 128, 4, 16, 2, 0, 1 }, { 128, 256, 256, 4, 8, 2, 0, 2 }, { 256, 512, 128, 8, 16, 1, 0, 1 },
    };
    for (size_t c = 0; c < sizeof cs / sizeof cs[0]; c++) {
        Prob p; prob_make(&p, cs[c].M, cs[c].N, cs[c].K);
        float *y = malloc((size_t)p.M * p.N * 4);
        for (int order = 0; order < 2; order++) {
            int rc = sim_run(&p, &cs[c], 0, order, y, msg, sizeof msg);
            size_t bad = rc ? 0 : sim_compare(&p, y, cs[c].kb0, cs[c].nkb);
            printf("sim case %zu (M %u N %u K %u rt %u warps %u aqn %u blocks %u..%u) order %d: %s, %zu of %zu differ\n", c, p.M, p.N, p.K, cs[c].rt, cs[c].warps, cs[c].aqn,
                   cs[c].kb0, cs[c].kb0 + cs[c].nkb - 1, order, rc ? msg : "ran", bad, (size_t)p.M * p.N);
            if (rc || bad) fail++;
        }
        if (c == 0) for (int m = 1; m <= 4; m++) {
            int rc = sim_run(&p, &cs[c], m, 0, y, msg, sizeof msg);
            size_t bad = rc ? 0 : sim_compare(&p, y, cs[c].kb0, cs[c].nkb);
            printf("sim case %zu mutant %d: %s\n", c, m, rc ? msg : bad ? "caught" : "NOT CAUGHT");
            if (rc || !bad) fail++;
        }
        free(y); prob_free(&p);
    }
    return fail;
}

/* ---- chip: double reference over the bf16 inputs, threaded over rows ---- */
typedef struct { const Prob *p; const float *y; uint32_t r0, r1; size_t bad, emu_same; double worst; size_t bc4[4], br8[2], brt[4], bw[16], nan, shown; } RefJob;
static void *ref_rows(void *arg) {
    RefJob *j = arg; const Prob *p = j->p;
    double *col = malloc((size_t)p->N * 16);
    for (uint32_t i = j->r0; i < j->r1; i++) {
        memset(col, 0, (size_t)p->N * 16);
        const uint16_t *ar = p->a + (size_t)i * p->K;
        for (uint32_t k = 0; k < p->K; k++) {
            const double av = bf(ar[k]); const uint16_t *br = p->b + (size_t)k * p->N;
            for (uint32_t c = 0; c < p->N; c++) { const double t = av * (double)bf(br[c]); col[2 * c] += t; col[2 * c + 1] += fabs(t); }
        }
        for (uint32_t c = 0; c < p->N; c++) {
            const float got = j->y[(size_t)i * p->N + c];
            const double scale = col[2 * c + 1] > 1e-6 ? col[2 * c + 1] : 1e-6, rel = fabs((double)got - col[2 * c]) / scale;
            if (!(rel < 1e-5)) { j->bad++; j->bc4[c % 4]++; j->br8[(i >> 3) & 1]++; j->brt[(i >> 4) & 3]++; j->bw[(c % 256) / 16]++; j->nan += isnan(got) || isinf(got);
                if (getenv("TGDBG") && j->shown < 6) { j->shown++; printf("  bad row %u col %u got %.9g ref %.9g emu %.9g\n", i, c, got, col[2 * c], emu(p, i, c, 0, p->K / 128u)); } }
            if (rel > j->worst) j->worst = rel;
            if (f2u(got) == f2u(emu(p, i, c, 0, p->K / 128u))) j->emu_same++;
        }
    }
    free(col);
    return NULL;
}
static size_t ref_check(const Prob *p, const float *y, size_t *emu_same, double *worst) {
    long nproc = sysconf(_SC_NPROCESSORS_ONLN); int nt = nproc < 1 ? 1 : nproc > 32 ? 32 : (int)nproc;
    pthread_t th[32]; RefJob jobs[32]; memset(jobs, 0, sizeof jobs);
    for (int t = 0; t < nt; t++) { jobs[t].p = p; jobs[t].y = y; jobs[t].r0 = (uint32_t)((uint64_t)p->M * t / nt); jobs[t].r1 = (uint32_t)((uint64_t)p->M * (t + 1) / nt); pthread_create(&th[t], NULL, ref_rows, &jobs[t]); }
    size_t bad = 0; *emu_same = 0; *worst = 0;
    for (int t = 0; t < nt; t++) { pthread_join(th[t], NULL); bad += jobs[t].bad; *emu_same += jobs[t].emu_same; if (jobs[t].worst > *worst) *worst = jobs[t].worst; }
    if (getenv("TGDBG") && bad) { size_t c4[4] = { 0 }, r8[2] = { 0 }, rt[4] = { 0 }, w[16] = { 0 }, nn = 0;
        for (int t = 0; t < nt; t++) { for (int q = 0; q < 4; q++) { c4[q] += jobs[t].bc4[q]; rt[q] += jobs[t].brt[q]; } for (int q = 0; q < 2; q++) r8[q] += jobs[t].br8[q]; for (int q = 0; q < 16; q++) w[q] += jobs[t].bw[q]; nn += jobs[t].nan; }
        printf("  bad by col%%4: %zu %zu %zu %zu; by row half: %zu %zu; by row tile: %zu %zu %zu %zu; nan/inf %zu\n  bad by warp:", c4[0], c4[1], c4[2], c4[3], r8[0], r8[1], rt[0], rt[1], rt[2], rt[3], nn);
        for (int q = 0; q < 16; q++) printf(" %zu", w[q]);
        printf("\n"); }
    return bad;
}
static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

static int chip_shape(uint32_t M, uint32_t N, uint32_t K, int cached, int reps, double peak, double gate_pct) {
    Prob p; prob_make(&p, M, N, K);
    const size_t ab = (size_t)M * K * 2, bb = (size_t)K * N * 2, cb = (size_t)M * N * 4;
    OmegaGpuMem ad = { 0 }, bd = { 0 }, cd = { 0 };
    int rc = cached ? omega_gpu_pf_alloc_dev(ab, &ad) : omega_gpu_pf_alloc_host(ab, &ad);
    if (!rc) rc = cached ? omega_gpu_pf_alloc_dev(bb, &bd) : omega_gpu_pf_alloc_host(bb, &bd);
    if (!rc) rc = cached ? omega_gpu_pf_alloc_dev(cb, &cd) : omega_gpu_pf_alloc_host(cb, &cd);
    if (!rc) rc = omega_gpu_pf_upload(&ad, 0, p.a, ab);
    if (!rc) rc = omega_gpu_pf_upload(&bd, 0, p.b, bb);
    uint8_t *poison = malloc(cb); memset(poison, 0xff, cb);
    float *y1 = malloc(cb), *y2 = malloc(cb);
    if (!rc) rc = omega_gpu_pf_upload(&cd, 0, poison, cb);
    if (rc) { printf("shape %ux%ux%u setup: %s (%s)\n", M, N, K, omega_gpu_pf_rc_name(rc), omega_gpu_session_last_error()); return 1; }
    OmegaTgemmCall call = { OMEGA_TGEMM_LAYOUT_NN, OMEGA_TGEMM_EPI_F32, M, N, K, ad.va, bd.va, cd.va };
    OmegaTgemmSpec sp; if (omega_gpu_tgemm_spec(&call, &sp) != 0) { printf("shape %ux%ux%u: spec refused\n", M, N, K); return 1; }
    const char *kind = cached ? "cached" : "uncached";
    int fail = 0;
    if (reps > 64) reps = 64;
    double chip[64], wall[64];
    for (int r = 0; r < reps && !rc; r++) { uint64_t ns = 0; const double t0 = now_ns(); rc = omega_gpu_tgemm(&call, &ns); wall[r] = now_ns() - t0; chip[r] = (double)ns; }
    if (!rc) rc = omega_gpu_pf_download(&cd, 0, y1, cb);
    if (!rc) rc = omega_gpu_pf_upload(&cd, 0, poison, cb);
    if (!rc) rc = omega_gpu_tgemm(&call, NULL);
    if (!rc) rc = omega_gpu_pf_download(&cd, 0, y2, cb);
    if (rc) { printf("shape %ux%ux%u %s run: %s (%s)\n", M, N, K, kind, omega_gpu_pf_rc_name(rc), omega_gpu_session_last_error()); fail++; }
    else {
        size_t left = 0; for (size_t i = 0; i < (size_t)M * N; i++) left += f2u(y1[i]) == 0xffffffffu;
        const int same = !memcmp(y1, y2, cb);
        size_t emu_same = 0; double worst = 0;
        const double t0 = now_ns();
        const size_t bad = ref_check(&p, y1, &emu_same, &worst);
        qsort(chip, reps, 8, cmp_d); qsort(wall, reps, 8, cmp_d);
        const double fl = 2.0 * M * N * K, mc = chip[reps / 2], mw = wall[reps / 2], tc = fl / mc / 1e3, tw = fl / mw / 1e3;
        printf("TGEMM %ux%ux%u %s rt %u warps %u tiles %u grid_x %u: reference %zu of %zu outside 1e-5 (worst %.3g), %zu bit-identical to the emulation, repeats identical %s, poison left %zu (reference %.1f s)\n",
               M, N, K, kind, sp.rt, sp.warps, M / omega_tgemm_bm(&sp), N / omega_tgemm_span(&sp), bad, (size_t)M * N, worst, emu_same, same ? "yes" : "NO", left, (now_ns() - t0) / 1e9);
        printf("TGEMM %ux%ux%u %s: chip median %.3f ms (min %.3f) = %.2f TFLOPS = %.1f%% of peak %.2f; whole call median %.3f ms = %.2f TFLOPS = %.1f%%%s\n",
               M, N, K, kind, mc / 1e6, chip[0] / 1e6, tc, 100.0 * tc / peak, peak, mw / 1e6, tw, 100.0 * tw / peak, cached && gate_pct > 0 ? (100.0 * tc / peak >= gate_pct ? " GATE ok" : " GATE FAIL") : "");
        if (bad || !same || left) fail++;
        if (cached && gate_pct > 0 && !(100.0 * tc / peak >= gate_pct)) fail++; /* the gate is on device-resident (GPU-cached) buffers; the uncached pass is the correctness-first step (R-8) */
        if (!cached) { /* negative control: the parity-swapped kernel must give a different result (a comparison that cannot fail is no comparison) */
            for (int m = 4; m >= 1; m -= 3) {
                omega_gpu_tgemm_test_set_mutant(m);
                int mrc = omega_gpu_tgemm(&call, NULL);
                omega_gpu_tgemm_test_set_mutant(0);
                if (!mrc) mrc = omega_gpu_pf_download(&cd, 0, y2, cb);
                const int diff = !mrc && memcmp(y1, y2, cb) != 0;
                printf("TGEMM %ux%ux%u mutant %d: %s\n", M, N, K, m, mrc ? "launch failed" : diff ? "caught" : "NOT CAUGHT");
                if (!diff) fail++;
            }
        }
    }
    free(poison); free(y1); free(y2);
    omega_gpu_pf_free(&ad); omega_gpu_pf_free(&bd); omega_gpu_pf_free(&cd);
    prob_free(&p);
    return fail;
}

int main(int argc, char **argv) {
    int host = 1, sim = 1, chip = 0, reps = 7, cached_only = 0, uncached_only = 0; double peak = 120.64, gate = 0; const char *shape = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host-only")) sim = 0;
        else if (!strcmp(argv[i], "--sim")) host = 0;
        else if (!strcmp(argv[i], "--chip")) { chip = 1; host = 0; sim = 0; }
        else if (!strcmp(argv[i], "--shape") && i + 1 < argc) shape = argv[++i];
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--peak") && i + 1 < argc) peak = atof(argv[++i]);
        else if (!strcmp(argv[i], "--gate") && i + 1 < argc) gate = atof(argv[++i]);
        else if (!strcmp(argv[i], "--cached-only")) cached_only = 1;
        else if (!strcmp(argv[i], "--mutant") && i + 1 < argc) omega_gpu_tgemm_test_set_mutant(atoi(argv[++i])); /* timing probes: results are wrong on purpose */
        else if (!strcmp(argv[i], "--uncached-only")) uncached_only = 1;
        else if (!strcmp(argv[i], "--shape-knob") && i + 3 < argc) { omega_gpu_tgemm_set_shape((uint32_t)atoi(argv[i + 1]), (uint32_t)atoi(argv[i + 2]), (uint32_t)atoi(argv[i + 3])); i += 3; }
    }
    int fail = 0;
    if (host) fail += host_only();
    if (sim) fail += sim_tests();
    if (chip) {
        static const char *gate_shapes[] = { "8192x768x2304", "8192x768x768", "8192x768x3072", "8192x3072x768" };
        for (size_t s = 0; s < (shape ? 1u : sizeof gate_shapes / sizeof gate_shapes[0]); s++) {
            unsigned M = 0, N = 0, K = 0;
            if (sscanf(shape ? shape : gate_shapes[s], "%ux%ux%u", &M, &N, &K) != 3) { printf("bad shape\n"); fail++; break; }
            if (!cached_only) fail += chip_shape(M, N, K, 0, reps, peak, gate);
            if (!uncached_only) fail += chip_shape(M, N, K, 1, reps, peak, gate);
        }
    }
    printf("GPU_TGEMM: %s\n", fail ? "FAIL" : "PASS");
    return fail ? 1 : 0;
}
