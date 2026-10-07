/* omega#323 camera: replay hd128 gqa_f32 32q8kv ctx 1, 17, 256 in one process (the failing order) and
 * record every chip allocation (link-time wrap of nvrm_alloc / nvrm_alloc_gpu_uncached / nvrm_free).
 * The code buffer is found by content (the kernel bytes from codegen). Nothing in the launch path changes.
 * usage: va_camera <variant-label> [ctx ...]   (default ctx list: 1 17 256) */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nvrm.h"
#include "omega_gpu_attention_api.h"

int __real_nvrm_alloc(Nvrm *rm, uint64_t size, NvrmMem *out);
int __real_nvrm_alloc_gpu_uncached(Nvrm *rm, uint64_t size, NvrmMem *out);
int __real_nvrm_free(Nvrm *rm, NvrmMem *m);

typedef struct { uint64_t va, size; void *cpu; int live; const char *kind; } Rec;
static Rec g_rec[4096]; static int g_n;
static void rec_add(const NvrmMem *m, const char *kind) { if (g_n < 4096) g_rec[g_n++] = (Rec){ m->va, m->size, m->cpu, 1, kind }; }
int __wrap_nvrm_alloc(Nvrm *rm, uint64_t size, NvrmMem *out) { int rc = __real_nvrm_alloc(rm, size, out); if (rc == 0) rec_add(out, "cached"); return rc; }
int __wrap_nvrm_alloc_gpu_uncached(Nvrm *rm, uint64_t size, NvrmMem *out) { int rc = __real_nvrm_alloc_gpu_uncached(rm, size, out); if (rc == 0) rec_add(out, "uncached"); return rc; }
int __wrap_nvrm_free(Nvrm *rm, NvrmMem *m) { for (int i = 0; i < g_n; i++) if (g_rec[i].live && g_rec[i].va == m->va) g_rec[i].live = 0; return __real_nvrm_free(rm, m); }

static void utc(char *b, size_t n) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); struct tm tm; gmtime_r(&t.tv_sec, &tm); strftime(b, n, "%Y-%m-%dT%H:%M:%SZ", &tm); }
static int live_covering(uint64_t va) { for (int i = 0; i < g_n; i++) if (g_rec[i].live && va >= g_rec[i].va && va < g_rec[i].va + g_rec[i].size) return i; return -1; }

static void snap(const char *site, const uint8_t *kcode, size_t ksize) {
    char ts[32]; utc(ts, sizeof ts);
    printf("SNAP site=%s utc=%s allocations=%d\n", site, ts, g_n);
    for (int i = 0; i < g_n; i++) {
        int is_code = g_rec[i].live && g_rec[i].size >= ksize && g_rec[i].cpu && memcmp(g_rec[i].cpu, kcode, ksize) == 0;
        printf("  ALLOC #%d va=0x%llx size=0x%llx end=0x%llx %s %s%s\n", i, (unsigned long long)g_rec[i].va, (unsigned long long)g_rec[i].size,
               (unsigned long long)(g_rec[i].va + g_rec[i].size), g_rec[i].kind, g_rec[i].live ? "live" : "freed", is_code ? " CODE" : "");
        if (is_code) {
            uint64_t end = g_rec[i].va + g_rec[i].size, code_end = g_rec[i].va + ksize;
            int nxt = live_covering(end), nxt2 = live_covering(code_end + 2048 - 1);
            printf("  CODE va=0x%llx code_bytes=%zu alloc=0x%llx slack_in_alloc=%llu page_after_alloc=0x%llx mapped_by=%s%d code_end+2047 mapped=%s\n",
                   (unsigned long long)g_rec[i].va, ksize, (unsigned long long)g_rec[i].size, (unsigned long long)(g_rec[i].size - ksize),
                   (unsigned long long)end, nxt >= 0 ? "#" : "NONE", nxt, nxt2 >= 0 ? "yes" : "NO");
        }
    }
    fflush(stdout);
}

int main(int argc, char **argv) {
    const char *variant = argc > 1 ? argv[1] : "unlabelled";
    uint32_t ctxs[16] = { 1, 17, 256 }; int nc = 3;
    if (argc > 2) { nc = 0; for (int i = 2; i < argc && nc < 16; i++) ctxs[nc++] = (uint32_t)atoi(argv[i]); }
    const uint32_t NQ = 32, NKV = 8, HD = 128, MAXC = 4096;
    OmegaBlackwellKernel kk; memset(&kk, 0, sizeof kk);
    if (omega_gpu_attention_codegen_hd(true, 12, 2, HD, &kk) != 0) { printf("codegen failed\n"); return 2; }
    printf("VARIANT %s kernel_bytes=%zu\n", variant, (size_t)kk.code_size);
    float *q = malloc(NQ * HD * 4), *k = malloc((size_t)MAXC * NKV * HD * 4), *v = malloc((size_t)MAXC * NKV * HD * 4), *o = malloc(NQ * HD * 4);
    uint32_t s = 12345u;
    for (size_t i = 0; i < NQ * HD; i++) { s = s * 1664525u + 1013904223u; q[i] = (float)((s >> 8) & 0xffff) / 32768.0f - 1.0f; }
    for (size_t i = 0; i < (size_t)MAXC * NKV * HD; i++) { s = s * 1664525u + 1013904223u; k[i] = (float)((s >> 8) & 0xffff) / 32768.0f - 1.0f;
                                                           s = s * 1664525u + 1013904223u; v[i] = (float)((s >> 8) & 0xffff) / 32768.0f - 1.0f; }
    int worst_rc = 0;
    for (int c = 0; c < nc; c++) {
        uint32_t ctx = ctxs[c]; char nm[32]; snprintf(nm, sizeof nm, "before_ctx%u", ctx);
        if (c > 0) snap(nm, kk.code, kk.code_size); else { char ts[32]; utc(ts, sizeof ts); printf("SNAP site=%s utc=%s allocations=%d (device not open yet)\n", nm, ts, g_n); }
        for (size_t i = 0; i < NQ * HD; i++) o[i] = NAN;
        OmegaGpuAttnInfo info; memset(&info, 0, sizeof info);
        int rc = omega_gpu_gqa_attention_f32(q, k, v, ctx, NQ, NKV, HD, o, &info);
        double worst = 0;
        for (uint32_t h = 0; h < NQ; h++) {
            uint32_t kvh = h / (NQ / NKV); double m = -INFINITY, *sc = malloc(ctx * sizeof(double)), l = 0;
            for (uint32_t t = 0; t < ctx; t++) { double d = 0; for (uint32_t x = 0; x < HD; x++) d += (double)q[h * HD + x] * k[((size_t)t * NKV + kvh) * HD + x]; sc[t] = d / sqrt((double)HD); if (sc[t] > m) m = sc[t]; }
            for (uint32_t t = 0; t < ctx; t++) { sc[t] = exp(sc[t] - m); l += sc[t]; }
            for (uint32_t x = 0; x < HD; x++) { double a = 0; for (uint32_t t = 0; t < ctx; t++) a += sc[t] * v[((size_t)t * NKV + kvh) * HD + x]; double e = fabs(a / l - o[h * HD + x]); if (!(e <= worst)) worst = isnan(e) ? INFINITY : e; }
            free(sc);
        }
        char ts[32]; utc(ts, sizeof ts);
        printf("RESULT variant=%s ctx=%u rc=%d utc_after=%s call_ns=%llu chip_ns=%llu unwritten=%llu max_abs_err=%.3g\n", variant, ctx, rc, ts,
               (unsigned long long)info.call_ns, (unsigned long long)info.elapsed_ns, (unsigned long long)info.unwritten_words, worst);
        snprintf(nm, sizeof nm, "after_ctx%u", ctx); snap(nm, kk.code, kk.code_size);
        if (rc != 0) { worst_rc = rc; break; }
    }
    return worst_rc == 0 ? 0 : 1;
}
