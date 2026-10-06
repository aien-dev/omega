/* Host tests for the native GB10 prime sieve (no device is opened).
 *
 *   gb10_sieve_host_test            golden words of the three new ops, codegen budget, nvdisasm
 *                                   listing + every branch target, then the allocated IR program
 *                                   run by a host IR simulator over the limits sweep against an
 *                                   independent Eratosthenes oracle; the mutant must be caught
 *   gb10_sieve_host_test --big      also 1e7 through the simulator (exercises the magic correction)
 *   gb10_sieve_host_test --limits   print the DESIGN limits sweep, one per line (chip script input)
 *   gb10_sieve_host_test --oracle LIMIT PATH [K]
 *                                   write the canonical bitmap of LIMIT (K copies, default 1)
 *
 * Simulator: executes the post-allocation IR (physical registers, so the loop liveness
 * fix of the register allocator is under test), one thread at a time (the kernel has no
 * barrier or shuffle), host pointers as addresses with a bounds check on every access.
 * It models the ISA as the IR documents it; it is not chip evidence.
 */
#include "prime_race_impl.h"
#include "omega_blackwell_codegen.h"
#include "omega_gpu_elementwise_api.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_checks, g_failed;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_failed++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ------------------------------------------------------------- oracle */
/* canonical words for limit: bit k set iff 2k+1 prime (plain Eratosthenes on odd numbers) */
static uint64_t *oracle_bitmap(uint64_t limit) {
    uint64_t odd = pr_odd_count(limit);
    size_t nw = pr_words64(limit);
    uint64_t *w = calloc(nw ? nw : 1, 8);
    uint8_t *comp = calloc(odd ? odd : 1, 1);
    if (!w || !comp) { free(w); free(comp); return NULL; }
    if (odd) comp[0] = 1; /* 1 */
    for (uint64_t i = 1; i < odd; i++) {
        uint64_t p = 2 * i + 1;
        if (comp[i]) continue;
        if (p * p > 2 * odd) continue;
        for (uint64_t c = p * p; (c - 1) / 2 < odd; c += 2 * p) comp[(c - 1) / 2] = 1;
    }
    for (uint64_t k = 0; k < odd; k++) if (!comp[k]) w[k / 64] |= 1ull << (k % 64);
    free(comp);
    return w;
}

static uint64_t count_primes(const uint64_t *w, uint64_t limit) {
    uint64_t n = limit >= 2;
    for (size_t i = 0; i < pr_words64(limit); i++) n += (uint64_t)__builtin_popcountll(w[i]);
    return n;
}

/* ------------------------------------------------------------- limits sweep */
static int cmp_u64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static size_t sweep_limits(uint64_t *out, size_t cap, int full) {
    static const uint64_t fixed[] = { 0, 1, 2, 3, 4, 5, 8, 9, 15, 25, 31, 32, 33, 34, 49, 63, 64, 65, 66, 95, 96, 97,
        121, 127, 128, 129, 130, 169, 255, 256, 257, 258, 289, 361, 961, 1023, 1024, 1025, 1999, 2000, 2001, 4095, 4096, 4097 };
    /* words of 32 odd numbers: limit 64k has exactly 32k odd numbers. k = 128 is one CTA of
     * words, k = 8192 one full grid-stride round (64 CTAs x 128 threads). */
    static const uint64_t ks[] = { 1, 2, 3, 4, 127, 128, 129, 256, 8191, 8192, 8193, 16384 };
    size_t n = 0;
#define ADD(x) do { if (n < cap) out[n++] = (x); } while (0)
    for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++) ADD(fixed[i]);
    for (size_t i = 0; i < sizeof ks / sizeof ks[0]; i++) { ADD(64 * ks[i] - 1); ADD(64 * ks[i]); ADD(64 * ks[i] + 1); }
    uint64_t pmax = full ? 997 : 113;
    for (uint64_t p = 3; p <= pmax; p += 2) {
        int prime = 1;
        for (uint64_t d = 3; d * d <= p; d += 2) if (p % d == 0) { prime = 0; break; }
        if (prime) { ADD(p * p - 1); ADD(p * p); ADD(p * p + 1); }
    }
    if (!full) { ADD(994009 - 1); ADD(994009); ADD(994009 + 1); } /* 997^2 */
    for (uint64_t l = 999983; l <= 1000003; l++) if (full || l == 999983 || l == 1000000 || l == 1000003) ADD(l);
#undef ADD
    qsort(out, n, sizeof *out, cmp_u64);
    size_t m = 0;
    for (size_t i = 0; i < n; i++) if (m == 0 || out[i] != out[m - 1]) out[m++] = out[i];
    return m;
}

/* ------------------------------------------------------------- simulator */
typedef struct { uint32_t r[256]; int pc, p0, done; } Thr;
static uint32_t g_cb[256]; /* constant bank 0 words 0..255 (byte offset / 4) */
typedef struct { uintptr_t lo, hi; } Range;
static Range g_ranges[2];
static int g_sim_err;

static uint32_t rd(const Thr *t, int v, const BlackwellIRProgram *p) { if (v < 0) return 0; return t->r[p->regalloc.vreg_to_phys[v] & 0xff]; }
static uint64_t rd64(const Thr *t, int v, const BlackwellIRProgram *p) { if (v < 0) return 0; int ph = p->regalloc.vreg_to_phys[v] & 0xff; return (uint64_t)t->r[ph] | ((uint64_t)t->r[ph + 1] << 32); }
static void wr(Thr *t, int v, uint32_t x, const BlackwellIRProgram *p) { if (v >= 0) t->r[p->regalloc.vreg_to_phys[v] & 0xff] = x; }
static void wr64(Thr *t, int v, uint64_t x, const BlackwellIRProgram *p) { if (v < 0) return; int ph = p->regalloc.vreg_to_phys[v] & 0xff; t->r[ph] = (uint32_t)x; t->r[ph + 1] = (uint32_t)(x >> 32); }
static uint32_t *mem_at(uint64_t a) {
    for (int i = 0; i < 2; i++) if (a >= g_ranges[i].lo && a + 4 <= g_ranges[i].hi && (a & 3) == 0) return (uint32_t *)(uintptr_t)a;
    g_sim_err = 3; return NULL;
}
static uint32_t lut3(uint32_t lut, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r = 0;
    for (int i = 0; i < 8; i++) if (lut >> i & 1) r |= ((i & 4) ? a : ~a) & ((i & 2) ? b : ~b) & ((i & 1) ? c : ~c);
    return r;
}

static uint64_t g_steps;
static void sim_thread(const BlackwellIRProgram *p, uint32_t tid, uint32_t cta) {
    Thr t; memset(&t, 0, sizeof t);
    for (uint64_t guard = 0; guard < 400000000ull; guard++) {
        if (t.pc < 0 || (size_t)t.pc >= p->count) { g_sim_err = 1; return; }
        const BlackwellIRInsn *in = &p->insns[t.pc];
        uint32_t a = rd(&t, in->src1_vreg, p), b = rd(&t, in->src2_vreg, p), c = rd(&t, in->src3_vreg, p);
        g_steps++;
        switch (in->op) {
        case BW_IR_EXIT: if (!in->predicate_p0 || t.p0) return; break;
        case BW_IR_BRA: {
            int take = !in->predicate_p0 || (in->predicate_not ? !t.p0 : t.p0);
            if (take) { if ((int32_t)in->imm == 0) { g_sim_err = 2; return; } t.pc += (int32_t)in->imm; continue; }
            break; }
        case BW_IR_S2R: wr(&t, in->dst_vreg, in->imm == BW_SR_TID_X ? tid : in->imm == BW_SR_CTAID_X ? cta : 0, p); break;
        case BW_IR_LDC: wr(&t, in->dst_vreg, g_cb[(in->imm / 4) & 0xff], p); break;
        case BW_IR_LDC64: wr64(&t, in->dst_vreg, (uint64_t)g_cb[(in->imm / 4) & 0xff] | ((uint64_t)g_cb[(in->imm / 4 + 1) & 0xff] << 32), p); break;
        case BW_IR_LDCU64: break; /* memory descriptor: not modelled */
        case BW_IR_MOV_IMM: wr(&t, in->dst_vreg, in->imm, p); break;
        case BW_IR_MOV_RZ: wr(&t, in->dst_vreg, 0, p); break;
        case BW_IR_IMAD: wr(&t, in->dst_vreg, in->src2_vreg >= 0 ? a * b + c : a * in->imm + c, p); break;
        case BW_IR_IMAD_WIDE: wr64(&t, in->dst_vreg, (uint64_t)a * in->imm + rd64(&t, in->src3_vreg, p), p); break;
        case BW_IR_IMAD_HI_U32: wr(&t, in->dst_vreg, (uint32_t)(((uint64_t)a * b) >> 32) + c, p); break;
        case BW_IR_IADD3: wr(&t, in->dst_vreg, a + b, p); break; /* the encoder's IADD3 has Rc = RZ */
        case BW_IR_SHF_R: wr(&t, in->dst_vreg, a >> (in->imm & 31), p); break;
        case BW_IR_SHF_L_U32: if (b >= 32) { g_sim_err = 8; return; } wr(&t, in->dst_vreg, a << b, p); break;
        case BW_IR_LOP3_XOR: wr(&t, in->dst_vreg, a ^ b, p); break;
        case BW_IR_LOP3_LUT: wr(&t, in->dst_vreg, lut3(in->imm & 0xff, a, b, c), p); break;
        case BW_IR_ISETP_GE_U32: t.p0 = a >= b; break;
        case BW_IR_LDG_E: { uint32_t *m = mem_at(rd64(&t, in->src1_vreg, p)); if (!m) return; wr(&t, in->dst_vreg, *m, p); break; }
        case BW_IR_STG_E: { uint32_t *m = mem_at(rd64(&t, in->src1_vreg, p)); if (!m) return; *m = b; break; }
        default: g_sim_err = 100 + (int)in->op; return;
        }
        t.pc++;
    }
    g_sim_err = 4;
}

/* Mirrors gb10_native.c's pass on the host: same table, same argument words. */
static int sim_sieve(const BlackwellIRProgram *p, uint64_t limit, uint64_t *words) {
    uint64_t odd = pr_odd_count(limit);
    uint32_t nwords = (uint32_t)((odd + 31) / 32);
    uint32_t blocks = (nwords + OMEGA_GPU_EW_THREADS - 1) / OMEGA_GPU_EW_THREADS;
    uint32_t ctas = blocks == 0 ? 1 : (blocks > OMEGA_GPU_EW_MAX_CTAS ? OMEGA_GPU_EW_MAX_CTAS : blocks);
    uint32_t stride = ctas * OMEGA_GPU_EW_THREADS, lastw = nwords ? nwords - 1 : 0;
    uint32_t valid = nwords ? (uint32_t)(odd - 32ull * lastw) : 32;
    uint32_t tailinv = ~(valid >= 32 ? 0xffffffffu : ((1u << valid) - 1u));
    uint32_t root = 0; while ((uint64_t)(root + 1) * (root + 1) <= limit) root++;
    size_t cap = root / 2 + 2;
    uint32_t *tab = calloc(cap * 3, 4), *out = calloc(nwords ? nwords : 1, 4);
    if (!tab || !out) { free(tab); free(out); return -1; }
    uint32_t n = 0;
    for (uint32_t q = 3; q <= root; q += 2) {
        int prime = 1;
        for (uint32_t d = 3; d * d <= q; d += 2) if (q % d == 0) { prime = 0; break; }
        if (!prime) continue;
        tab[3 * n] = q; tab[3 * n + 1] = (uint32_t)(((1ull << 32) + q - 1) / q); tab[3 * n + 2] = (uint32_t)(((uint64_t)q * q - 1) / 2); n++;
    }
    tab[3 * n] = 1; tab[3 * n + 1] = 0; tab[3 * n + 2] = 0xffffffffu;
    for (uint32_t i = 0; i < nwords; i++) out[i] = 0xffbadbadu;
    memset(g_cb, 0, sizeof g_cb);
    uint64_t ta = (uintptr_t)tab, oa = (uintptr_t)out;
    g_cb[0x380 / 4] = (uint32_t)ta; g_cb[0x380 / 4 + 1] = (uint32_t)(ta >> 32);
    g_cb[0x390 / 4] = (uint32_t)oa; g_cb[0x390 / 4 + 1] = (uint32_t)(oa >> 32);
    g_cb[0x398 / 4] = nwords; g_cb[0x39c / 4] = stride; g_cb[0x3a0 / 4] = lastw; g_cb[0x3a4 / 4] = tailinv;
    g_ranges[0] = (Range){ (uintptr_t)tab, (uintptr_t)tab + cap * 12 };
    g_ranges[1] = (Range){ (uintptr_t)out, (uintptr_t)out + (size_t)nwords * 4 };
    g_sim_err = 0;
    for (uint32_t cta = 0; cta < ctas && !g_sim_err; cta++)
        for (uint32_t tid = 0; tid < OMEGA_GPU_EW_THREADS && !g_sim_err; tid++) sim_thread(p, tid, cta);
    size_t nw64 = pr_words64(limit);
    memset(words, 0, nw64 * 8);
    memcpy(words, out, (size_t)nwords * 4);
    free(tab); free(out);
    return g_sim_err ? -1 : 0;
}

/* ------------------------------------------------------------- nvdisasm */
/* every word decodes, the new ops appear, and every BRA lands where the IR says */
static void nvdisasm_check(const OmegaBlackwellKernel *k, const BlackwellIRProgram *p) {
    const char *nv = "/usr/local/cuda/bin/nvdisasm";
    if (access(nv, X_OK) != 0) { printf("nvdisasm: not present, listing check SKIPPED\n"); return; }
    char bin[64], lst[64], cmd[256];
    snprintf(bin, sizeof bin, "/tmp/pr_gb10_sieve_%d.bin", (int)getpid());
    snprintf(lst, sizeof lst, "/tmp/pr_gb10_sieve_%d.sass", (int)getpid());
    FILE *f = fopen(bin, "wb");
    if (!f) { CHECK(0, "cannot write %s", bin); return; }
    fwrite(k->code, 1, k->code_size, f); fclose(f);
    snprintf(cmd, sizeof cmd, "%s -b SM121 %s > %s 2>&1", nv, bin, lst);
    int rc = system(cmd);
    unlink(bin);
    CHECK(rc == 0, "nvdisasm rc %d", rc);
    FILE *l = fopen(lst, "r");
    if (!l) { CHECK(0, "cannot read %s", lst); return; }
    static const char *must[] = { "LOP3.LUT", "SHF.L.U32", "IMAD.HI.U32", "@P0 BRA", "@!P0 BRA", "@P0 EXIT", "STG.E", "LDG.E" };
    int seen[8] = {0}, insns = 0, bad = 0, bra_ok = 0, bra_bad = 0;
    char line[512];
    while (fgets(line, sizeof line, l)) {
        char *s = strstr(line, "/*");
        if (!s) continue;
        unsigned addr;
        if (sscanf(s, "/*%x*/", &addr) != 1 || s[6] != '*') continue;
        insns++;
        if (strchr(line, '?') || strstr(line, "error")) bad++;
        for (int i = 0; i < 8; i++) if (strstr(line, must[i])) seen[i] = 1;
        char *br = strstr(line, "BRA ");
        if (br && addr / 16 < p->count) {
            const BlackwellIRInsn *in = &p->insns[addr / 16];
            unsigned long target = strtoul(br + 4, NULL, 16);
            unsigned long want = (unsigned long)((int)(addr / 16) + (int32_t)in->imm) * 16;
            if (in->op == BW_IR_BRA && target == want) bra_ok++;
            else { bra_bad++; printf("  branch at 0x%04x: nvdisasm target 0x%lx, IR target 0x%lx\n", addr, target, want); }
        }
    }
    fclose(l);
    for (int i = 0; i < 8; i++) CHECK(seen[i], "nvdisasm listing lacks %s (%s)", must[i], lst);
    CHECK(bad == 0, "%d undecodable words (%s)", bad, lst);
    CHECK(bra_bad == 0 && bra_ok >= 6, "branch targets: %d match, %d differ", bra_ok, bra_bad);
    printf("nvdisasm: %d words, %d undecodable, %d branch targets checked (%s)\n", insns, bad, bra_ok, lst);
    if (g_failed == 0) unlink(lst);
}

/* ------------------------------------------------------------- magic model */
/* The kernel's offset step as plain C: counts how often q = hi32(d * ceil(2^32/p)) is one
 * too big (r wraps) over the workload, so the --big run is known to exercise that case, and
 * checks the kernel's j = (p - r) - p*((r-1)>>31) against (p - d % p) % p on every pair. */
static void magic_model(uint64_t limit) {
    uint64_t odd = pr_odd_count(limit), nwords = (odd + 31) / 32, over = 0, wrong = 0, pairs = 0;
    uint32_t root = 0; while ((uint64_t)(root + 1) * (root + 1) <= limit) root++;
    for (uint32_t p = 3; p <= root; p += 2) {
        int prime = 1;
        for (uint32_t d = 3; d * d <= p; d += 2) if (p % d == 0) { prime = 0; break; }
        if (!prime) continue;
        uint32_t magic = (uint32_t)(((1ull << 32) + p - 1) / p), k0 = (uint32_t)(((uint64_t)p * p - 1) / 2);
        for (uint64_t w = 0; w < nwords; w++) {
            uint32_t base = (uint32_t)(32 * w);
            if (k0 >= base + 32 || k0 > base) continue;
            uint32_t d = base - k0, q = (uint32_t)(((uint64_t)d * magic) >> 32), r = d - q * p;
            pairs++;
            if (r >> 31) over++;
            uint32_t j = p - r;
            j -= p * ((r - 1) >> 31); /* the kernel's single fix */
            if (j != (p - d % p) % p) wrong++;
        }
    }
    printf("magic model limit %" PRIu64 ": %" PRIu64 " word/prime pairs, %" PRIu64 " needed the correction, %" PRIu64 " wrong\n", limit, pairs, over, wrong);
    CHECK(wrong == 0, "magic division wrong on %" PRIu64 " pairs", wrong);
    if (limit >= 10000000ull) CHECK(over > 0, "the 1e7 workload never needs the correction (test would not exercise it)");
}

static int write_oracle(uint64_t limit, const char *path, unsigned k) {
    uint64_t *w = oracle_bitmap(limit);
    FILE *f = fopen(path, "wb");
    if (!w || !f) { free(w); if (f) fclose(f); return 2; }
    size_t nw = pr_words64(limit);
    for (unsigned i = 0; i < k; i++) if (nw && fwrite(w, 8, nw, f) != nw) { fclose(f); free(w); return 2; }
    free(w);
    return fclose(f) == 0 ? 0 : 2;
}

int main(int argc, char **argv) {
    int big = 0;
    if (argc >= 2 && strcmp(argv[1], "--limits") == 0) {
        static uint64_t l[4096];
        size_t n = sweep_limits(l, 4096, 1);
        for (size_t i = 0; i < n; i++) printf("%" PRIu64 "\n", l[i]);
        return 0;
    }
    if (argc >= 4 && strcmp(argv[1], "--oracle") == 0)
        return write_oracle(strtoull(argv[2], NULL, 10), argv[3], argc >= 5 ? (unsigned)atoi(argv[4]) : 1u);
    if (argc >= 2 && strcmp(argv[1], "--big") == 0) big = 1;

    /* 1. golden words */
    int fx = omega_blackwell_verify_codegen_fixtures_intops();
    CHECK(fx == 0, "golden words LOP3_LUT / SHF_L_U32 / IMAD_HI_U32 (rc %d)", fx);

    /* 2. oracle self-check against pi(x) */
    static const struct { uint64_t x, pi; } pis[] = { {10, 4}, {100, 25}, {1000, 168}, {10000, 1229}, {100000, 9592}, {1000000, 78498} };
    for (size_t i = 0; i < sizeof pis / sizeof pis[0]; i++) {
        uint64_t *w = oracle_bitmap(pis[i].x);
        CHECK(w && count_primes(w, pis[i].x) == pis[i].pi, "oracle pi(%" PRIu64 ")", pis[i].x);
        free(w);
    }

    /* 3. codegen */
    BlackwellIRProgram *prog = calloc(1, sizeof *prog), *mprog = calloc(1, sizeof *mprog);
    OmegaBlackwellKernel k, mk;
    memset(&k, 0, sizeof k); memset(&mk, 0, sizeof mk);
    int rc = omega_gpu_elementwise_codegen_ir(OMEGA_GPU_EW_PRIME_SIEVE, 0, prog, &k);
    int mrc = omega_gpu_elementwise_codegen_ir(OMEGA_GPU_EW_PRIME_SIEVE, 1, mprog, &mk);
    CHECK(rc == OMEGA_GPU_EW_OK && mrc == OMEGA_GPU_EW_OK, "codegen rc %d mutant rc %d", rc, mrc);
    if (rc != OMEGA_GPU_EW_OK || mrc != OMEGA_GPU_EW_OK) { printf("VERDICT FAIL (codegen)\n"); return 1; }
    printf("kernel: %zu IR insns, %zu encoded words, %u GPRs, sha256 ", prog->count, k.insn_count, k.gpr_count);
    for (int i = 0; i < 32; i++) printf("%02x", k.code_digest[i]);
    printf("\n");
    CHECK(k.gpr_count <= 64, "GPR budget %u > 64", k.gpr_count);
    CHECK(memcmp(k.code_digest, mk.code_digest, 32) != 0, "mutant kernel differs from production");
    nvdisasm_check(&k, prog);

    /* 4. simulator sweep */
    static uint64_t lims[4096];
    size_t nl = sweep_limits(lims, 4096, 0);
    int sim_fail = 0; uint64_t first_bad = 0;
    for (size_t i = 0; i < nl + (size_t)big; i++) {
        uint64_t L = i < nl ? lims[i] : 10000000ull;
        uint64_t *want = oracle_bitmap(L), *got = calloc(pr_words64(L) + 1, 8);
        if (!want || !got) { CHECK(0, "alloc"); break; }
        int src = sim_sieve(prog, L, got);
        size_t nw = pr_words64(L), bad = nw;
        for (size_t j = 0; j < nw; j++) if (got[j] != want[j]) { bad = j; break; }
        if (src != 0 || bad != nw) {
            if (!sim_fail++) first_bad = L;
            printf("  sim limit %" PRIu64 ": sim rc %d err %d, first differing u64 word %zu (got %016" PRIx64 " want %016" PRIx64 ")\n",
                   L, src, g_sim_err, bad, bad < nw ? got[bad] : 0, bad < nw ? want[bad] : 0);
        }
        if (L == 1000000) CHECK(count_primes(got, L) == 78498, "sim pi(1e6) = %" PRIu64, count_primes(got, L));
        free(want); free(got);
    }
    CHECK(sim_fail == 0, "simulator differs from the oracle at %d limits (first %" PRIu64 ")", sim_fail, first_bad);
    printf("simulator: %zu limits%s, %d differ, %" PRIu64 " IR steps\n", nl + (size_t)big, big ? " (+1e7)" : "", sim_fail, g_steps);

    /* 5. mutant must be caught */
    {
        uint64_t L = 100000, *want = oracle_bitmap(L), *got = calloc(pr_words64(L) + 1, 8);
        int src = want && got ? sim_sieve(mprog, L, got) : -1;
        CHECK(src == 0 && want && memcmp(got, want, pr_words64(L) * 8) != 0, "mutant kernel not caught at 1e5 (sim rc %d)", src);
        free(want); free(got);
    }

    /* 6. division model */
    magic_model(1000000);
    if (big) magic_model(10000000);

    free(k.code); free(mk.code); free(prog); free(mprog);
    printf("checks=%d failed=%d\nVERDICT %s\n", g_checks, g_failed, g_failed ? "FAIL" : "PASS");
    return g_failed ? 1 : 0;
}
