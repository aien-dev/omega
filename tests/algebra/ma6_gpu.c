/* MA-6 silicon driver (GB10). spec/mixed-algebra-ma6-gpu.md
 *
 *   ma6_gpu --selftest
 *       silicon checks of the encoder ops MA-6 relies on (omg_selftest).
 *   ma6_gpu --edges
 *       exactness at the magnitude edges on silicon (x = -128, W all +1 or
 *       all -1, n = 16384; G3 at its declared domain edge n = 32768).
 *   ma6_gpu --bench <dir> <label> <seed> <reps>
 *       the pre-registered grid: n {1024,4096,16384} x m {1,64,4096} x
 *       sparsity {0,0.6}; per cell the realization order is shuffled with
 *       the seed; 3 warm-up + <reps> timed launches per realization, every
 *       launch checked bit-exact against oma_rz_oracle. Writes
 *       <dir>/ma6_gpu_<label>.<sha256>.json (content-addressed, never
 *       overwrites) and prints its path.
 *
 * Timing: gpu_ns from per-warp %globaltimer stamps (max end - min start);
 * host_ns = submit to completion marker seen (m16_native_wait_marker, which
 * sleeps 50 us between reads, so host_ns is quantized). Pack, upload,
 * y-zeroing and read-back are timed separately with CLOCK_MONOTONIC.
 */
#include "algebra/gpu/oma_gpu.h"
#include "algebra/realize_common.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static uint64_t rs;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 11); }

static const omg_kind KINDS[4] = { OMG_G1_I8, OMG_G2A_CRUMB, OMG_G2B_PLANE, OMG_G3_BF16 };

static int cmpu(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
typedef struct { uint64_t min, q25, med, q75, max; } st_t;
static st_t stats(uint64_t *v, int n) {
    st_t s = { 0, 0, 0, 0, 0 };
    if (n <= 0) return s;
    qsort(v, (size_t)n, sizeof *v, cmpu);
    s.min = v[0]; s.q25 = v[n / 4]; s.med = v[n / 2]; s.q75 = v[(3 * n) / 4]; s.max = v[n - 1];
    return s;
}
static void jst(FILE *f, const char *k, st_t s) {
    fprintf(f, "\"%s\":{\"min\":%llu,\"q25\":%llu,\"median\":%llu,\"q75\":%llu,\"max\":%llu}", k,
            (unsigned long long)s.min, (unsigned long long)s.q25, (unsigned long long)s.med,
            (unsigned long long)s.q75, (unsigned long long)s.max);
}

typedef struct {
    int exact;           /* every launch bit-exact */
    int launches, mismatched_launches;
    st_t gpu, host, xpack, xup, yzero, yread, total;
    uint64_t wpack_ns, wup_ns, build_ns;
    omg_geom g;
    uint32_t insns, gpr;
    char code_sha[65];
    int failed;          /* plan/build/open/launch failure */
    char why[96];
} res_t;

/* One realization on one cell. xs[2] alternate between launches. */
static void run_one(omg_kind kind, uint32_t m, uint32_t n, const int8_t *w, const int8_t *const xs[2],
                    const int32_t *const ys[2], int warm, int reps, res_t *r) {
    memset(r, 0, sizeof *r);
    r->exact = 1;
    if (omg_plan(kind, m, n, &r->g) != 0) { r->failed = 1; snprintf(r->why, sizeof r->why, "plan refused"); return; }
    omg_geom *g = &r->g;
    omg_kernel k;
    uint64_t t0 = now();
    if (omg_build(g, &k) != 0) { r->failed = 1; snprintf(r->why, sizeof r->why, "kernel build failed"); return; }
    r->build_ns = now() - t0;
    r->insns = (uint32_t)k.insn_count;
    r->gpr = k.gpr;
    for (int i = 0; i < 32; i++) sprintf(r->code_sha + 2 * i, "%02x", k.sha256[i]);
    uint32_t *w0 = malloc(g->w0_bytes), *w1 = g->w1_bytes ? malloc(g->w1_bytes) : NULL, *xd = malloc(g->x_bytes);
    int32_t *yh = malloc(g->y_bytes);
    t0 = now();
    int prc = omg_pack_weights(g, w, w0, w1);
    r->wpack_ns = now() - t0;
    omg_session *s = NULL;
    if (prc != 0 || omg_open(&s, &k, g->w0_bytes, g->w1_bytes, g->x_bytes, g->y_bytes, g->ts_bytes) != 0) {
        r->failed = 1; snprintf(r->why, sizeof r->why, prc ? "pack failed" : "session open failed");
        goto out;
    }
    t0 = now();
    memcpy(omg_buf(s, 0), w0, g->w0_bytes);
    if (w1) memcpy(omg_buf(s, 1), w1, g->w1_bytes);
    r->wup_ns = now() - t0;
    int total = warm + reps;
    uint64_t *v[7];
    for (int i = 0; i < 7; i++) v[i] = calloc((size_t)total, sizeof(uint64_t));
    int nt = 0;
    for (int it = 0; it < total; it++) {
        int xi = it & 1;
        uint64_t a = now();
        omg_pack_x(g, xs[xi], xd);
        uint64_t b = now();
        memcpy(omg_buf(s, 2), xd, g->x_bytes);
        uint64_t c = now();
        memset(omg_buf(s, 3), 0, g->y_bytes);
        uint64_t d = now();
        uint64_t hn = 0, gn = 0;
        if (omg_launch(s, g->cta, g->grid, g->warps, &hn, &gn) != 0) {
            r->failed = 1; snprintf(r->why, sizeof r->why, "launch %d failed", it);
            break;
        }
        uint64_t e = now();
        memcpy(yh, omg_buf(s, 3), g->y_bytes);
        uint64_t f = now();
        int bad = 0;
        for (uint32_t i = 0; i < m; i++) bad += yh[i] != ys[xi][i];
        for (uint32_t i = m; i < g->m_pad; i++) bad += yh[i] != 0;
        r->launches++;
        if (bad) { r->exact = 0; r->mismatched_launches++; }
        if (it >= warm) {
            v[0][nt] = gn; v[1][nt] = hn; v[2][nt] = b - a; v[3][nt] = c - b; v[4][nt] = d - c; v[5][nt] = f - e;
            v[6][nt] = (b - a) + (c - b) + (d - c) + (e - d) + (f - e);
            nt++;
        }
    }
    r->gpu = stats(v[0], nt); r->host = stats(v[1], nt); r->xpack = stats(v[2], nt); r->xup = stats(v[3], nt);
    r->yzero = stats(v[4], nt); r->yread = stats(v[5], nt); r->total = stats(v[6], nt);
    for (int i = 0; i < 7; i++) free(v[i]);
    omg_close(s);
out:
    omg_kernel_free(&k);
    free(w0); free(w1); free(xd); free(yh);
}

static void gen(int8_t *w, size_t mn, double sp, int8_t *x, size_t n) {
    for (size_t i = 0; i < mn; i++) w[i] = (rnd() % 10000) < (uint32_t)(sp * 10000) ? 0 : ((rnd() & 1) ? 1 : -1);
    for (size_t i = 0; i < n; i++) x[i] = (int8_t)(rnd() & 0xff);
}

static int quiet_flag(void) {
    const char *h = getenv("HOME");
    char p[512];
    snprintf(p, sizeof p, "%s/workspace/.spark-quiet", h ? h : "");
    return access(p, F_OK) == 0;
}

static void sys_state(FILE *f, const char *key) {
    fprintf(f, "\"%s\":{\"cpu_khz\":[", key);
    for (int c = 0; c < 64; c++) {
        char p[128];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", c);
        FILE *q = fopen(p, "r");
        if (!q) break;
        long v = -1;
        if (fscanf(q, "%ld", &v) != 1) v = -1;
        fclose(q);
        fprintf(f, "%s%ld", c ? "," : "", v);
    }
    fprintf(f, "],\"thermal_mC\":[");
    for (int z = 0; z < 32; z++) {
        char p[128];
        snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/temp", z);
        FILE *q = fopen(p, "r");
        if (!q) break;
        long v = -1;
        if (fscanf(q, "%ld", &v) != 1) v = -1;
        fclose(q);
        fprintf(f, "%s%ld", z ? "," : "", v);
    }
    double la[3] = { 0, 0, 0 };
    if (getloadavg(la, 3) < 0) la[0] = -1;
    fprintf(f, "],\"loadavg1\":%.2f,\"spark_quiet_flag_present\":%s}", la[0], quiet_flag() ? "true" : "false");
}

static const char *env(const char *k) { const char *v = getenv(k); return v ? v : ""; }

static int bench(const char *dir, const char *label, uint64_t seed, int reps) {
    const uint32_t NS[3] = { 1024, 4096, 16384 }, MS[3] = { 1, 64, 4096 };
    const double SP[2] = { 0.0, 0.6 };
    const int warm = 3;
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s/.ma6_gpu_%s.tmp", dir, label);
    FILE *f = fopen(tmp, "w");
    if (!f) { perror(tmp); return 2; }
    fprintf(f, "{\"schema\":\"OMEGA_MIXED_ALGEBRA_MA6_GPU_BENCH_V1\",\"stage\":\"MA-6\",\"label\":\"%s\",\n", label);
    fprintf(f, "\"operation\":\"Omega-X: y = W.x, W in {-1,0,+1}^(m x n), x int8, y int32, exact vs oma_rz_oracle\",\n");
    fprintf(f, "\"contract_digest_provisional\":\"75772afe8a98f73064f2933d76bc58f7bfe5b9353eb40b703e173b4cd3f22b5f\",\n");
    fprintf(f, "\"contract_digest_note\":\"PROVISIONAL Turing companion digest cited from docs/turing/TURING_W0_PROPOSAL.md; not minted here, not an Omega semantic id\",\n");
    fprintf(f, "\"commit\":\"%s\",\"dirty_files\":\"%s\",\"binary_sha256\":\"%s\",\"machine_state_summary_sha256\":\"%s\",\n",
            env("MA6_COMMIT"), env("MA6_DIRTY"), env("MA6_BIN_SHA"), env("MA6_STATE_SHA"));
    fprintf(f, "\"device\":\"GB10 sm_121 via Omega encoder + QMD + physics M16 native channel (no CUDA toolkit)\",\n");
    fprintf(f, "\"seed\":%llu,\"warmup\":%d,\"reps\":%d,\"timing\":{\"gpu_ns\":\"per-warp %%globaltimer: max(end)-min(start)\","
               "\"host_ns\":\"submit to completion marker via m16_native_wait_marker (50 us sleep granularity)\","
               "\"per_call_total_ns\":\"x pack + x upload + y zero + host launch/wait + y read-back\"},\n",
            (unsigned long long)seed, warm, reps);
    fprintf(f, "\"code_sha256_note\":\"plain SHA-256 of emitted machine code bytes; content digest, not an identity\",\n");
    sys_state(f, "state_start");
    fprintf(f, ",\n\"cells\":[\n");
    rs = seed ? seed : 1;
    int first = 1, all_exact = 1, fails = 0;
    for (int ni = 0; ni < 3; ni++)
        for (int mi = 0; mi < 3; mi++)
            for (int si = 0; si < 2; si++) {
                uint32_t n = NS[ni], m = MS[mi];
                int8_t *w = malloc((size_t)m * n), *x0 = malloc(n), *x1 = malloc(n);
                int32_t *y0 = malloc((size_t)m * 4), *y1 = malloc((size_t)m * 4);
                gen(w, (size_t)m * n, SP[si], x0, n);
                for (uint32_t i = 0; i < n; i++) x1[i] = (int8_t)(rnd() & 0xff);
                oma_rz_oracle(w, m, n, x0, y0);
                oma_rz_oracle(w, m, n, x1, y1);
                int order[4] = { 0, 1, 2, 3 };
                for (int i = 3; i > 0; i--) { int j = (int)(rnd() % (uint32_t)(i + 1)); int t = order[i]; order[i] = order[j]; order[j] = t; }
                const int8_t *xs[2] = { x0, x1 };
                const int32_t *ys[2] = { y0, y1 };
                for (int oi = 0; oi < 4; oi++) {
                    omg_kind kind = KINDS[order[oi]];
                    res_t r;
                    run_one(kind, m, n, w, xs, ys, warm, reps, &r);
                    if (r.failed) fails++;
                    if (!r.exact || r.failed) all_exact = 0;
                    size_t wb = r.g.w0_bytes + r.g.w1_bytes;
                    fprintf(f, "%s{\"n\":%u,\"m\":%u,\"sparsity\":%.1f,\"realization\":\"%s\",\"run_position\":%d,"
                               "\"failed\":%s,\"why\":\"%s\",\"exact\":%s,\"launches\":%d,\"mismatched_launches\":%d,"
                               "\"S\":%u,\"U\":%u,\"threads\":%u,\"cta\":%u,\"grid\":%u,\"insns\":%u,\"gpr\":%u,"
                               "\"code_sha256\":\"%s\",\"weight_bytes\":%zu,\"x_bytes\":%zu,\"y_bytes\":%zu,"
                               "\"build_ns\":%llu,\"weight_pack_ns\":%llu,\"weight_upload_ns\":%llu,",
                            first ? "" : ",\n", n, m, SP[si], omg_kind_name(kind), oi, r.failed ? "true" : "false", r.why,
                            r.exact && !r.failed ? "true" : "false", r.launches, r.mismatched_launches, r.g.S, r.g.U,
                            r.g.threads, r.g.cta, r.g.grid, r.insns, r.gpr, r.code_sha, wb, r.g.x_bytes, r.g.y_bytes,
                            (unsigned long long)r.build_ns, (unsigned long long)r.wpack_ns, (unsigned long long)r.wup_ns);
                    jst(f, "gpu_ns", r.gpu); fputc(',', f); jst(f, "host_ns", r.host); fputc(',', f);
                    jst(f, "x_pack_ns", r.xpack); fputc(',', f); jst(f, "x_upload_ns", r.xup); fputc(',', f);
                    jst(f, "y_zero_ns", r.yzero); fputc(',', f); jst(f, "y_read_ns", r.yread); fputc(',', f);
                    jst(f, "per_call_total_ns", r.total);
                    fprintf(f, "}");
                    fflush(f);
                    first = 0;
                    fprintf(stderr, "n=%-5u m=%-4u sp=%.1f %-18s %s gpu_med=%llu ns host_med=%llu ns\n", n, m, SP[si],
                            omg_kind_name(kind), r.failed ? r.why : (r.exact ? "EXACT" : "MISMATCH"),
                            (unsigned long long)r.gpu.med, (unsigned long long)r.host.med);
                }
                free(w); free(x0); free(x1); free(y0); free(y1);
            }
    fprintf(f, "\n],\n");
    sys_state(f, "state_end");
    fprintf(f, ",\n\"all_exact\":%s,\"failures\":%d}\n", all_exact ? "true" : "false", fails);
    fclose(f);
    /* content-addressed final name */
    FILE *q = fopen(tmp, "rb");
    if (!q) return 2;
    fseek(q, 0, SEEK_END);
    long len = ftell(q);
    fseek(q, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)len);
    if (!buf || fread(buf, 1, (size_t)len, q) != (size_t)len) { fclose(q); return 2; }
    fclose(q);
    uint8_t dg[32];
    sha256_hash(buf, (size_t)len, dg);
    free(buf);
    char hex[65], fin[1200];
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", dg[i]);
    snprintf(fin, sizeof fin, "%s/ma6_gpu_%s.%s.json", dir, label, hex);
    if (access(fin, F_OK) == 0) { fprintf(stderr, "refusing to overwrite %s\n", fin); return 2; }
    if (rename(tmp, fin) != 0) { perror("rename"); return 2; }
    printf("%s\n", fin);
    return (all_exact && !fails) ? 0 : 1;
}

static int edges(void) {
    int fails = 0;
    struct { uint32_t m, n; int wsign; } cs[] = { { 64, 16384, 1 }, { 64, 16384, -1 }, { 1, 16384, 1 }, { 16, 32768, 1 }, { 16, 32768, -1 } };
    for (size_t c = 0; c < sizeof cs / sizeof cs[0]; c++)
        for (int a = 0; a < 4; a++) {
            uint32_t m = cs[c].m, n = cs[c].n;
            omg_geom g;
            if (omg_plan(KINDS[a], m, n, &g) != 0) continue;
            int8_t *w = malloc((size_t)m * n), *x = malloc(n);
            int32_t *y = malloc((size_t)m * 4);
            for (size_t i = 0; i < (size_t)m * n; i++) w[i] = (int8_t)cs[c].wsign;
            for (uint32_t i = 0; i < n; i++) x[i] = -128;
            oma_rz_oracle(w, m, n, x, y);
            const int8_t *xs[2] = { x, x };
            const int32_t *ys[2] = { y, y };
            res_t r;
            run_one(KINDS[a], m, n, w, xs, ys, 0, 2, &r);
            int ok = !r.failed && r.exact;
            if (!ok) fails++;
            printf("%s edge m=%u n=%u W=%+d x=-128 (|y|=%d) %s\n", ok ? "PASS" : "FAIL", m, n, cs[c].wsign, abs(y[0]),
                   omg_kind_name(KINDS[a]));
            free(w); free(x); free(y);
        }
    return fails;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "--selftest")) {
        char rep[4096];
        int f = omg_selftest(rep, sizeof rep);
        fputs(rep, stdout);
        printf("%s: selftest %d failure(s)\n", f == 0 ? "PASS" : "FAIL", f);
        return f == 0 ? 0 : 1;
    }
    if (argc >= 2 && !strcmp(argv[1], "--edges")) {
        int f = edges();
        printf("%s: edges %d failure(s)\n", f ? "FAIL" : "PASS", f);
        return f ? 1 : 0;
    }
    if (argc >= 6 && !strcmp(argv[1], "--bench"))
        return bench(argv[2], argv[3], strtoull(argv[4], NULL, 10), atoi(argv[5]));
    fprintf(stderr, "usage: %s --selftest | --edges | --bench <dir> <label> <seed> <reps>\n", argv[0]);
    return 2;
}
