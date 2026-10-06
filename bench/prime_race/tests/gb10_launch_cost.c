/* DIAGNOSTIC, not a benchmark result: where does a GB10 native launch spend its time?
 *
 *   gb10_launch_cost empty N GRID BLOCK        EXIT-only kernel, N launches
 *   gb10_launch_cost sieve N GRID BLOCK LIMIT  prime sieve kernel at LIMIT, N launches
 *
 * Uses the session API exactly as bench/prime_race/gb10_native.c does (lock, open, uncached
 * allocations, omega_gpu_session_launch with gpr 64 and a 600 s marker wait). Per launch it
 * records the session's submit->marker2 time and the host wall time of the launch call, then
 * prints median/min/max of each. The sieve kernel family assumes 128-thread CTAs (OMEGA_GPU_EW_THREADS), so
 * BLOCK must be 128 for sieve; only the grid varies. The sieve run is checked: the bitmap of the last launch must
 * hold pi(LIMIT) - 1 set bits (odd primes; bit i is 2i+1). Needs GB10_CHIP_RUN=1 and the GPU
 * lock (/tmp/aien-gb10.lock) held by the caller. No CUDA. */
#include "omega_gpu_elementwise_api.h"
#include "omega_gpu_session.h"
#include "omega_blackwell_codegen.h"
#include "omega_blackwell_qmd.h" /* OMEGA_BW_CBANK_MATMUL_ARGS_WORDS */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAXN 100000

static uint64_t now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y;
}
static void stats(const char *what, uint64_t *v, int n) {
    qsort(v, (size_t)n, sizeof *v, cmp_u64);
    uint64_t med = n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
    printf("  %-22s median=%8.1f us  min=%8.1f us  max=%8.1f us\n", what, med / 1e3, v[0] / 1e3, v[n - 1] / 1e3);
}

/* EXIT plus the self-branch every kernel here ends with; nothing else. */
static int build_empty(OmegaBlackwellKernel *k) {
    static BlackwellIRProgram prog;
    omega_bw_ir_init(&prog);
    BlackwellIRInsn x; memset(&x, 0, sizeof x);
    x.dst_vreg = x.src1_vreg = x.src2_vreg = x.src3_vreg = x.ureg = -1;
    x.op = BW_IR_EXIT;
    if (omega_bw_ir_append(&prog, &x) < 0) return -1; /* returns the index */
    x.op = BW_IR_BRA; x.imm = 0;
    if (omega_bw_ir_append(&prog, &x) < 0) return -1; /* returns the index */
    if (omega_bw_regalloc_solve(&prog) != 0) return -1;
    enum { CAP = 32 * 16 }; /* the encoder pads every program to at least 32 instructions */
    k->code = malloc(CAP);
    size_t len = 0;
    if (!k->code || omega_bw_encode_program(&prog, k->code, CAP, &len) != 0) return -1;
    k->code_size = len; k->insn_count = prog.count; k->gpr_count = 1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 5 || (strcmp(argv[1], "empty") && strcmp(argv[1], "sieve")) || (!strcmp(argv[1], "sieve") && argc < 6)) {
        fprintf(stderr, "usage: %s empty N GRID BLOCK | sieve N GRID BLOCK LIMIT\n", argv[0]); return 64;
    }
    const int sieve = !strcmp(argv[1], "sieve");
    int n = atoi(argv[2]); uint32_t grid = (uint32_t)atoi(argv[3]), block = (uint32_t)atoi(argv[4]);
    uint64_t limit = sieve ? strtoull(argv[5], NULL, 10) : 0;
    if (n < 1 || n > MAXN || grid < 1 || grid > OMEGA_GPU_EW_MAX_CTAS || block < 32 || block > 1024 || block % 32 ||
        limit > 0xffffffffull || (sieve && block != OMEGA_GPU_EW_THREADS)) { fprintf(stderr, "argument out of range\n"); return 64; }

    OmegaBlackwellKernel k; memset(&k, 0, sizeof k);
    if (sieve ? omega_gpu_elementwise_codegen_ir(OMEGA_GPU_EW_PRIME_SIEVE, 0, NULL, &k) != OMEGA_GPU_EW_OK
              : build_empty(&k) != 0) { fprintf(stderr, "codegen failed\n"); return 2; }

    if (!getenv("GB10_CHIP_RUN")) { fprintf(stderr, "codegen ok (%zu insns, %zu bytes); refused: set GB10_CHIP_RUN=1 (chip launch)\n", k.insn_count, k.code_size); return 2; }

    /* sieve geometry as gb10_native computes it, with the grid and block taken from the caller */
    uint64_t odd = (limit + 1) / 2;
    uint32_t nwords = (uint32_t)((odd + 31) / 32), lastw = nwords ? nwords - 1 : 0;
    uint32_t valid = nwords ? (uint32_t)(odd - 32ull * lastw) : 32;
    uint32_t tailinv = ~(valid >= 32 ? 0xffffffffu : ((1u << valid) - 1u));
    uint32_t root = 0; while ((uint64_t)(root + 1) * (root + 1) <= limit) root++;

    omega_gpu_session_lock();
    if (!omega_gpu_session_open()) { fprintf(stderr, "open failed: %s\n", omega_gpu_session_last_error()); return 2; }
    NvrmMem code, out, tab;
    if (omega_gpu_session_alloc(k.code_size, &code) || omega_gpu_session_alloc((size_t)(nwords ? nwords : 1) * 4, &out) ||
        omega_gpu_session_alloc(((size_t)root / 2 + 2) * 12, &tab)) { fprintf(stderr, "alloc failed\n"); return 2; }
    memcpy(code.cpu, k.code, k.code_size);
    uint32_t *t = tab.cpu, np = 0;
    for (uint32_t p = 3; p <= root; p += 2) {
        int pr = 1; for (uint32_t d = 3; d * d <= p; d += 2) if (p % d == 0) { pr = 0; break; }
        if (!pr) continue;
        t[3 * np] = p; t[3 * np + 1] = (uint32_t)(((1ull << 32) + p - 1) / p); t[3 * np + 2] = (uint32_t)(((uint64_t)p * p - 1) / 2); np++;
    }
    t[3 * np] = 1; t[3 * np + 1] = 0; t[3 * np + 2] = 0xffffffffu;
    __asm__ volatile("dsb sy" ::: "memory");

    uint32_t args[OMEGA_BW_CBANK_MATMUL_ARGS_WORDS] = {0};
    args[0] = (uint32_t)tab.va; args[1] = (uint32_t)(tab.va >> 32);
    args[4] = (uint32_t)out.va; args[5] = (uint32_t)(out.va >> 32);
    args[6] = nwords; args[7] = grid * block; args[8] = lastw; args[9] = tailinv;
    OmegaGpuLaunch L = {
        .code_va = code.va, .gpr_count = 64, .threads_x = block, .threads_y = 1, .grid_x = grid, .grid_y = 1,
        .num_elements = nwords ? nwords : 1, .args = args, .n_args = OMEGA_BW_CBANK_MATMUL_ARGS_WORDS, .timeout_ms = 600000,
    };
    /* PR_GB10_SPIN_US: marker wait spin window, as in gb10_native.c (campaign C4) */
    if (getenv("PR_GB10_SPIN_US")) L.spin_us = (uint32_t)strtoul(getenv("PR_GB10_SPIN_US"), NULL, 10);
    static uint64_t dev[MAXN], wall[MAXN];
    for (int i = 0; i < n; i++) {
        uint64_t ns = 0; uint32_t marker = 0; /* elapsed_ns is added to, so reset per launch */
        uint64_t t0 = now_ns();
        if (omega_gpu_session_launch(&L, &ns, &marker) != 0) { fprintf(stderr, "launch %d failed: %s\n", i, omega_gpu_session_last_error()); return 2; }
        wall[i] = now_ns() - t0; dev[i] = ns;
    }
    long bits = 0;
    if (sieve) { uint32_t *o = out.cpu; for (uint32_t i = 0; i < nwords; i++) bits += __builtin_popcount(o[i]); }
    printf("DIAGNOSTIC %s launches=%d grid=%u block=%u insns=%zu limit=%" PRIu64 " words=%u base_primes=%u odd_primes_found=%ld spin_us=%u\n",
           argv[1], n, grid, block, k.insn_count, limit, nwords, np, bits, L.spin_us);
    stats("submit->marker2", dev, n);
    stats("host wall per launch", wall, n);
    omega_gpu_session_free(&tab); omega_gpu_session_free(&out); omega_gpu_session_free(&code);
    omega_gpu_session_unlock(); omega_gpu_session_close();
    free(k.code);
    return 0;
}
