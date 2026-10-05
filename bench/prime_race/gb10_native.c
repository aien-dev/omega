/* AIEN Prime Drag Race: native GB10 implementation (no CUDA).
 *
 * Kernel: gen_prime_sieve in src/omega_gpu_elementwise_api.c (Omega's own Blackwell
 * codegen; word-parallel odd-only sieve, one writer per 32-bit word, no atomics).
 * Launch: the process-wide session (src/omega_gpu_session.h): shader-cache invalidate,
 * constant bank, QMD, marker 1, L2_FLUSH_DIRTY, marker 2 on uncached memory; every
 * buffer here is GPU-uncached (session rule), so host writes and host reads need no
 * extra flush beyond the dsb before the launch.
 *
 * One-time setup (untimed): session lock + open, kernel codegen + upload, persistent
 * output / prime-table buffers sized for the limit.
 * Pass (timed): reset the output words (poison), host sieve of the odd base primes
 * <= isqrt(limit) with their division magics written straight into the device-visible
 * table, one launch (<= OMEGA_GPU_EW_MAX_CTAS CTAs, grid-stride over words), wait for
 * both markers. Persistent buffers => faithful=no.
 *
 * CTA budget (optimization campaign C1, 2026-10-05): PR_GB10_CTA_BUDGET=N (1..4096) in the
 * environment lifts or lowers the launch cap; unset keeps OMEGA_GPU_EW_MAX_CTAS (the proven
 * I42 envelope). Any other value fails setup. The budget used is reported in the detail JSON.
 *
 * Labels (handoff 2026-10-05 DESIGN): algorithm=other, faithful=no, bits=1,
 * environment linux-hosted-gb10-native. Build with -DPR_GB10_MUTANT=1 only for the chip
 * test's negative control (the r == 0 fix is dropped; the oracle must catch it).
 */
#include "prime_race_impl.h"
#include "omega_gpu_elementwise_api.h"
#include "omega_gpu_session.h"
#include "omega_blackwell_qmd.h" /* OMEGA_BW_CBANK_MATMUL_ARGS_WORDS */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PR_GB10_MUTANT
#define PR_GB10_MUTANT 0
#endif

#define GB_THREADS OMEGA_GPU_EW_THREADS
#define GB_MAX_CTAS OMEGA_GPU_EW_MAX_CTAS
#define GB_GPR 64u              /* the QMD register count the elementwise launcher uses */
#define GB_WAIT_MS 600000ull    /* same marker wait as the elementwise launcher (omega#198) */
#define GB_POISON 0xffbadbadu
#define GB_MAX_LIMIT 0xffffffffull /* kernel range: base, k0 < 2^31 */
#define GB_BUDGET_MAX 4096u

/* PR_GB10_CTA_BUDGET or the I42 default; 0 on a malformed or out-of-range value. */
static uint32_t gb_cta_budget(void) {
    const char *v = getenv("PR_GB10_CTA_BUDGET");
    if (!v) return GB_MAX_CTAS;
    char *end = NULL;
    unsigned long n = strtoul(v, &end, 10);
    if (end == v || *end != 0 || v[0] == '-' || n == 0 || n > GB_BUDGET_MAX) return 0;
    return (uint32_t)n;
}

typedef struct {
    uint64_t limit, odd;
    uint32_t nwords, ctas, budget, stride, lastw, tailinv;
    uint32_t root;          /* isqrt(limit) */
    uint32_t nprimes;       /* odd primes in the table of the last pass (sentinel excluded) */
    size_t table_cap;       /* entries, sentinel included */
    OmegaBlackwellKernel k;
    NvrmMem code, out, table;
    uint64_t diag_ns, launches;
    int locked, opened;
} gb_state;

/* floor(sqrt(n)). The root of any u64 is below 2^32, so the top trial bit is 2^31: with that start
 * (r + bit) <= 2^32 - 1 and its square cannot wrap. (A 2^32 start squared to 2^64 = 0 on the first
 * step and returned 0 for every n, so no base primes were ever sieved; caught on chip at limit 9.) */
static uint32_t isqrt_u64(uint64_t n) {
    uint64_t r = 0;
    for (uint64_t bit = 1ull << 31; bit; bit >>= 1)
        if ((r + bit) * (r + bit) <= n) r += bit;
    return (uint32_t)r;
}

static void gb_release(gb_state *s) {
    if (s->locked) {
        omega_gpu_session_free(&s->table);
        omega_gpu_session_free(&s->out);
        omega_gpu_session_free(&s->code);
        omega_gpu_session_unlock();
        s->locked = 0;
    }
    if (s->opened) { omega_gpu_session_close(); s->opened = 0; } /* refused (no-op) after a latch */
    free(s->k.code); s->k.code = NULL;
}

static int gb_setup(void **state, uint64_t limit) {
    if (limit > GB_MAX_LIMIT) { fprintf(stderr, "gb10-native: limit %" PRIu64 " above %llu\n", limit, GB_MAX_LIMIT); return -1; }
    gb_state *s = calloc(1, sizeof *s);
    if (!s) return -1;
    *state = s;
    s->limit = limit;
    s->odd = pr_odd_count(limit);
    s->nwords = (uint32_t)((s->odd + 31) / 32);
    s->budget = gb_cta_budget();
    if (s->budget == 0) { fprintf(stderr, "gb10-native: PR_GB10_CTA_BUDGET must be 1..%u\n", GB_BUDGET_MAX); return -1; }
    uint32_t blocks = (s->nwords + GB_THREADS - 1) / GB_THREADS;
    s->ctas = blocks == 0 ? 1 : (blocks > s->budget ? s->budget : blocks);
    s->stride = s->ctas * GB_THREADS;
    s->lastw = s->nwords ? s->nwords - 1 : 0;
    uint32_t valid = s->nwords ? (uint32_t)(s->odd - 32ull * s->lastw) : 32; /* 1..32 */
    uint32_t tailmask = valid >= 32 ? 0xffffffffu : ((1u << valid) - 1u);
    s->tailinv = ~tailmask;
    s->root = isqrt_u64(limit);
    if ((uint64_t)s->root * s->root > limit || ((uint64_t)s->root + 1) * ((uint64_t)s->root + 1) <= limit) {
        fprintf(stderr, "gb10-native: isqrt(%" PRIu64 ") = %u is wrong\n", limit, s->root); return -1; /* fail closed */
    }
    s->table_cap = (size_t)s->root / 2 + 2; /* every odd number <= root, plus the sentinel */

    if (omega_gpu_elementwise_codegen_ir(OMEGA_GPU_EW_PRIME_SIEVE, PR_GB10_MUTANT, NULL, &s->k) != OMEGA_GPU_EW_OK) {
        fprintf(stderr, "gb10-native: kernel codegen failed\n"); return -1;
    }
    omega_gpu_session_lock();
    s->locked = 1;
    if (!omega_gpu_session_open()) { fprintf(stderr, "gb10-native: session open failed: %s\n", omega_gpu_session_last_error()); return -1; }
    s->opened = 1;
    size_t out_bytes = (size_t)(s->nwords ? s->nwords : 1) * 4;
    if (omega_gpu_session_alloc(s->k.code_size, &s->code) != 0 ||
        omega_gpu_session_alloc(out_bytes, &s->out) != 0 ||
        omega_gpu_session_alloc(s->table_cap * 12, &s->table) != 0) {
        fprintf(stderr, "gb10-native: alloc failed: %s\n", omega_gpu_session_last_error()); return -1;
    }
    memcpy(s->code.cpu, s->k.code, s->k.code_size);
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

static int gb_pass(void *state, uint64_t limit) {
    gb_state *s = state;
    if (!s || limit != s->limit || omega_gpu_session_is_blocked()) return -1;
    /* reset the sieve state: every output word poisoned, the kernel must overwrite all */
    uint32_t *out = s->out.cpu;
    for (uint32_t i = 0; i < s->nwords; i++) out[i] = GB_POISON;
    /* odd base primes <= isqrt(limit): flags[i] marks 2i+1 composite */
    uint32_t half = s->root / 2 + 1;
    uint8_t *flags = calloc(half, 1);
    if (!flags) return -1;
    uint32_t *tab = s->table.cpu, n = 0;
    for (uint32_t i = 1; i < half; i++) {
        uint32_t p = 2 * i + 1;
        if (p > s->root) break;
        if (flags[i]) continue;
        for (uint64_t c = (uint64_t)p * p; c <= s->root; c += 2ull * p) flags[c / 2] = 1;
        tab[3 * n + 0] = p;
        tab[3 * n + 1] = (uint32_t)(((1ull << 32) + p - 1) / p); /* ceil(2^32 / p) */
        tab[3 * n + 2] = (uint32_t)(((uint64_t)p * p - 1) / 2);
        n++;
    }
    free(flags);
    tab[3 * n + 0] = 1; tab[3 * n + 1] = 0; tab[3 * n + 2] = 0xffffffffu; /* sentinel: ends every scan */
    s->nprimes = n;
    __asm__ volatile("dsb sy" ::: "memory");

    uint32_t args[OMEGA_BW_CBANK_MATMUL_ARGS_WORDS] = {0};
    args[0] = (uint32_t)s->table.va; args[1] = (uint32_t)(s->table.va >> 32);
    args[4] = (uint32_t)s->out.va;   args[5] = (uint32_t)(s->out.va >> 32);
    args[6] = s->nwords; args[7] = s->stride; args[8] = s->lastw; args[9] = s->tailinv;
    OmegaGpuLaunch L = {
        .code_va = s->code.va, .gpr_count = GB_GPR, .threads_x = GB_THREADS, .threads_y = 1,
        .grid_x = s->ctas, .grid_y = 1, .num_elements = s->nwords ? s->nwords : 1,
        .args = args, .n_args = OMEGA_BW_CBANK_MATMUL_ARGS_WORDS, .timeout_ms = GB_WAIT_MS,
    };
    uint64_t ns = 0; uint32_t marker = 0;
    if (omega_gpu_session_launch(&L, &ns, &marker) != 0) {
        fprintf(stderr, "gb10-native: launch failed: %s\n", omega_gpu_session_last_error());
        return -1;
    }
    s->diag_ns += ns;
    s->launches++;
    return 0;
}

static int gb_export(void *state, uint64_t limit, uint64_t *words) {
    gb_state *s = state;
    if (!s || limit != s->limit) return -1;
    size_t bytes64 = pr_words64(limit) * 8, bytes32 = (size_t)s->nwords * 4;
    if (bytes32 > bytes64) return -1;
    memcpy(words, s->out.cpu, bytes32);
    memset((uint8_t *)words + bytes32, 0, bytes64 - bytes32);
    return 0;
}

static uint64_t gb_threads(void *state) { gb_state *s = state; return s ? s->stride : 0; }

static int gb_detail(void *state, FILE *f) {
    gb_state *s = state;
    if (!s) return -1;
    fprintf(f, "{\"grid\": %u, \"cta_budget\": %u, \"block\": %u, \"words\": %u, \"nprimes\": %u, \"kernel_code_sha256\": \"",
            s->ctas, s->budget, GB_THREADS, s->nwords, s->nprimes);
    for (int i = 0; i < 32; i++) fprintf(f, "%02x", s->k.code_digest[i]);
    fprintf(f, "\", \"kernel_insns\": %zu, \"kernel_gpr\": %u, \"mutant\": %d, \"launches\": %" PRIu64
               ", \"diag_kernel_ns_total\": %" PRIu64
               ", \"diag_note\": \"sum of session elapsed_ns (submit to second marker), diagnostic only\"}",
            s->k.insn_count, s->k.gpr_count, PR_GB10_MUTANT, s->launches, s->diag_ns);
    return ferror(f) ? -1 : 0;
}

static void gb_teardown(void *state) {
    gb_state *s = state;
    if (!s) return;
    gb_release(s);
    free(s);
}

static const pr_impl GB10_NATIVE = {
    .name = PR_GB10_MUTANT ? "gb10-native-mutant" : "gb10-native",
    .algorithm = "other", .faithful = "no", .bits = 1,
    .environment = "linux-hosted-gb10-native",
    .setup = gb_setup, .pass = gb_pass, .export_bitmap = gb_export,
    .threads = gb_threads, .detail_json = gb_detail, .teardown = gb_teardown,
};

#ifndef PR_NO_MAIN
int main(int argc, char **argv) { return pr_main(argc, argv, &GB10_NATIVE); }
#endif
