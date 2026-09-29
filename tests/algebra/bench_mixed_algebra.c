/* MA-2 benchmark: Omega-X (ternary W x int8 x -> int32 y) realizations on one
 * pinned Cortex-X925 core of the Grace CPU. spec/mixed-algebra-ma2.md
 *
 * Grid: n in {1024, 4096, 16384}, m in {1, 64, 4096}, weight sparsity
 * (fraction of zeros) in {0, 0.3, 0.6, 0.9}. For each cell and realization:
 * pack cost (separate), per-call latency samples (best and median of >= 9
 * interleaved blocks, CLOCK_MONOTONIC), bytes read, footprint, and the
 * roofline floor = bytes moved / read bandwidth measured by the stream kernel
 * in this binary at the matching working-set size (hot, repeated calls).
 * Shared machine: each block reads /proc/thread-self/schedstat run_delay and
 * is retried when the thread waited for a CPU more than 2% of the block.
 * Output: one JSON receipt (argv[1]); every run output is checked bit-exact
 * against the naive oracle before timing.
 *
 * Usage: bench_mixed_algebra OUT.json   (env OMA_BENCH_CPU pins a cpu id;
 * OMA_BENCH_QUICK=1 shrinks the grid for a smoke run). */
#define _GNU_SOURCE
#include "algebra/realize_common.h"

#include <arm_neon.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define NSAMPLES 9
#define MAX_RETRY 8
#define TARGET_BLOCK_NS 300000.0 /* 0.3 ms per timed block */
#define MAX_RZ 16

static uint64_t g_rng = 0x6d61332d62656e63ULL;
static uint64_t rnd(void) {
    uint64_t z = (g_rng += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static uint64_t run_delay_ns(void) {
    FILE *f = fopen("/proc/thread-self/schedstat", "r");
    unsigned long long a = 0, b = 0;
    if (f) {
        if (fscanf(f, "%llu %llu", &a, &b) != 2) b = 0;
        fclose(f);
    }
    return (uint64_t)b;
}

static void read_loadavg(char *buf, size_t cap) {
    FILE *f = fopen("/proc/loadavg", "r");
    buf[0] = 0;
    if (f) {
        double a, b, c;
        if (fscanf(f, "%lf %lf %lf", &a, &b, &c) == 3) snprintf(buf, cap, "%.2f %.2f %.2f", a, b, c);
        fclose(f);
    }
}

static int read_thermal_c(void) {
    FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    long v = -1000;
    if (f) {
        if (fscanf(f, "%ld", &v) != 1) v = -1000;
        fclose(f);
    }
    return (int)(v / 1000);
}

/* ---- core selection: X925 = MIDR part 0xd85, A725 = 0xd87 ---- */
static int cpu_part[256];
static int ncpu_seen;

static void read_cpu_parts(void) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    char line[256];
    int cur = -1;
    for (int i = 0; i < 256; i++) cpu_part[i] = -1;
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        int v;
        unsigned part;
        if (sscanf(line, "processor : %d", &v) == 1) cur = v;
        else if (sscanf(line, "CPU part : %x", &part) == 1 && cur >= 0 && cur < 256) {
            cpu_part[cur] = (int)part;
            if (cur + 1 > ncpu_seen) ncpu_seen = cur + 1;
        }
    }
    fclose(f);
}

static int read_cpu_idle(unsigned long long idle[256], unsigned long long total[256]) {
    FILE *f = fopen("/proc/stat", "r");
    char line[512];
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        int c;
        unsigned long long u, ni, s, id, io, irq, sirq, st;
        if (sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu", &c, &u, &ni, &s, &id, &io, &irq,
                   &sirq, &st) == 9 && c >= 0 && c < 256) {
            idle[c] = id + io;
            total[c] = u + ni + s + id + io + irq + sirq + st;
        }
    }
    fclose(f);
    return 0;
}

static int pick_x925_cpu(char *why, size_t cap) {
    const char *env = getenv("OMA_BENCH_CPU");
    if (env && *env) {
        int c = atoi(env);
        snprintf(why, cap, "OMA_BENCH_CPU=%d", c);
        return c;
    }
    unsigned long long i0[256] = {0}, t0[256] = {0}, i1[256] = {0}, t1[256] = {0};
    read_cpu_idle(i0, t0);
    struct timespec ts = {0, 300000000};
    nanosleep(&ts, NULL);
    read_cpu_idle(i1, t1);
    int best = -1;
    double best_idle = -1;
    for (int c = 0; c < ncpu_seen; c++) {
        if (cpu_part[c] != 0xd85) continue;
        double dt = (double)(t1[c] - t0[c]);
        double idle = dt > 0 ? (double)(i1[c] - i0[c]) / dt : 0;
        if (idle > best_idle) { best_idle = idle; best = c; }
    }
    snprintf(why, cap, "most idle Cortex-X925 over 0.3 s (%.0f%% idle)", best_idle * 100.0);
    return best;
}

/* ---- stream-read bandwidth ---- */
typedef struct { size_t bytes; double gbs; } bw_level;
#define NBW 12
static bw_level g_bw[NBW];

/* Three read kernels; the floor uses the best of them at each size.
 * A: ld1 x4 groups, B: independent q loads, C: four concurrent streams. */
#define XOR8_REDUCE()                                                                              \
    uint8x16_t s = veorq_u8(veorq_u8(veorq_u8(a0, a1), veorq_u8(a2, a3)),                          \
                            veorq_u8(veorq_u8(a4, a5), veorq_u8(a6, a7)));                         \
    return vgetq_lane_u64(vreinterpretq_u64_u8(s), 0) ^ vgetq_lane_u64(vreinterpretq_u64_u8(s), 1)

static uint64_t stream_a(const uint8_t *p, size_t bytes) {
    uint8x16_t a0 = vdupq_n_u8(0), a1 = a0, a2 = a0, a3 = a0, a4 = a0, a5 = a0, a6 = a0, a7 = a0;
    for (size_t i = 0; i < bytes; i += 128) {
        uint8x16x4_t u = vld1q_u8_x4(p + i), v = vld1q_u8_x4(p + i + 64);
        a0 = veorq_u8(a0, u.val[0]); a1 = veorq_u8(a1, u.val[1]);
        a2 = veorq_u8(a2, u.val[2]); a3 = veorq_u8(a3, u.val[3]);
        a4 = veorq_u8(a4, v.val[0]); a5 = veorq_u8(a5, v.val[1]);
        a6 = veorq_u8(a6, v.val[2]); a7 = veorq_u8(a7, v.val[3]);
    }
    XOR8_REDUCE();
}

static uint64_t stream_b(const uint8_t *p, size_t bytes) {
    uint8x16_t a0 = vdupq_n_u8(0), a1 = a0, a2 = a0, a3 = a0, a4 = a0, a5 = a0, a6 = a0, a7 = a0;
    for (size_t i = 0; i < bytes; i += 128) {
        a0 = veorq_u8(a0, vld1q_u8(p + i)); a1 = veorq_u8(a1, vld1q_u8(p + i + 16));
        a2 = veorq_u8(a2, vld1q_u8(p + i + 32)); a3 = veorq_u8(a3, vld1q_u8(p + i + 48));
        a4 = veorq_u8(a4, vld1q_u8(p + i + 64)); a5 = veorq_u8(a5, vld1q_u8(p + i + 80));
        a6 = veorq_u8(a6, vld1q_u8(p + i + 96)); a7 = veorq_u8(a7, vld1q_u8(p + i + 112));
    }
    XOR8_REDUCE();
}

static uint64_t stream_c(const uint8_t *p, size_t bytes) {
    size_t q = bytes / 4;
    const uint8_t *p1 = p + q, *p2 = p + 2 * q, *p3 = p + 3 * q;
    uint8x16_t a0 = vdupq_n_u8(0), a1 = a0, a2 = a0, a3 = a0, a4 = a0, a5 = a0, a6 = a0, a7 = a0;
    for (size_t i = 0; i < q; i += 32) {
        a0 = veorq_u8(a0, vld1q_u8(p + i)); a1 = veorq_u8(a1, vld1q_u8(p + i + 16));
        a2 = veorq_u8(a2, vld1q_u8(p1 + i)); a3 = veorq_u8(a3, vld1q_u8(p1 + i + 16));
        a4 = veorq_u8(a4, vld1q_u8(p2 + i)); a5 = veorq_u8(a5, vld1q_u8(p2 + i + 16));
        a6 = veorq_u8(a6, vld1q_u8(p3 + i)); a7 = veorq_u8(a7, vld1q_u8(p3 + i + 16));
    }
    XOR8_REDUCE();
}

static volatile uint64_t g_sink;

static void measure_bandwidth(void) {
    static const size_t sizes[NBW] = {16u << 10, 48u << 10, 256u << 10, 1u << 20, 1536u << 10, 4u << 20,
                                      8u << 20, 16u << 20, 32u << 20, 64u << 20, 128u << 20, 256u << 20};
    uint64_t (*const kern[3])(const uint8_t *, size_t) = {stream_a, stream_b, stream_c};
    uint8_t *buf = oma_rz_alloc(sizes[NBW - 1]);
    if (!buf) { fprintf(stderr, "bandwidth buffer oom\n"); exit(2); }
    for (size_t i = 0; i < sizes[NBW - 1]; i++) buf[i] = (uint8_t)i;
    for (int l = 0; l < NBW; l++) {
        size_t sz = sizes[l];
        size_t passes = (size_t)((64u << 20) / sz);
        if (passes < 1) passes = 1;
        double best = 1e30;
        for (int k = 0; k < 3; k++) {
            g_sink ^= kern[k](buf, sz);
            for (int s = 0, tries = 0; s < 7; s++) {
                uint64_t d0 = run_delay_ns();
                double t0 = now_ns();
                for (size_t p = 0; p < passes; p++) g_sink ^= kern[k](buf, sz);
                double t = now_ns() - t0;
                uint64_t d = run_delay_ns() - d0;
                if ((double)d * 50.0 > t && ++tries < 20) { s--; continue; }
                if (t < best) best = t;
            }
        }
        g_bw[l].bytes = sz;
        g_bw[l].gbs = (double)sz * (double)passes / best;
    }
    free(buf);
}

static int bw_level_for(size_t ws) {
    for (int l = 0; l < NBW; l++)
        if (g_bw[l].bytes >= ws) return l;
    return NBW - 1;
}

/* ---- energy (aien_spbm cpu_p = the X925 cluster, microjoules) ---- */
static char g_energy_path[640];

static void find_energy_meter(void) {
    DIR *d = opendir("/sys/class/hwmon");
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[600], name[64] = {0};
        snprintf(p, sizeof p, "/sys/class/hwmon/%s/name", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (!fgets(name, sizeof name, f)) name[0] = 0;
        fclose(f);
        if (strncmp(name, "aien_spbm", 9) != 0) continue;
        for (int k = 1; k <= 8; k++) {
            char lab[64] = {0};
            snprintf(p, sizeof p, "/sys/class/hwmon/%s/energy%d_label", e->d_name, k);
            f = fopen(p, "r");
            if (!f) continue;
            if (!fgets(lab, sizeof lab, f)) lab[0] = 0;
            fclose(f);
            if (strncmp(lab, "cpu_p", 5) == 0) {
                snprintf(g_energy_path, sizeof g_energy_path, "/sys/class/hwmon/%s/energy%d_input", e->d_name, k);
                break;
            }
        }
    }
    closedir(d);
}

static int read_energy_uj(double *uj) {
    if (!g_energy_path[0]) return -1;
    FILE *f = fopen(g_energy_path, "r");
    unsigned long long v;
    if (!f) return -1;
    int ok = fscanf(f, "%llu", &v) == 1;
    fclose(f);
    if (!ok) return -1;
    *uj = (double)v;
    return 0;
}

/* ---- per-cell measurement ---- */
typedef struct {
    int ok, verified;
    double pack_min, pack_med, pack_noise;
    int pack_reps;
    double samples[NSAMPLES];
    double tmin, tmed, q25, q75;
    int retries, forced;
    long batch;
    size_t weight_bytes, footprint, working_set, scratch;
    double floor_ns, floor_gbs;
    size_t floor_level;
    double pct_floor;
    uint64_t nnz;
} cell_res;

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double quantile(const double *sorted, int n, double q) {
    double pos = q * (n - 1);
    int i = (int)pos;
    double f = pos - i;
    return i + 1 < n ? sorted[i] * (1 - f) + sorted[i + 1] * f : sorted[i];
}

static int timed_block(const oma_rz_impl *im, const oma_rz_plan *p, const int8_t *x, int32_t *y, long batch,
                       double *per_call, uint64_t *delay) {
    uint64_t d0 = run_delay_ns();
    double t0 = now_ns();
    for (long b = 0; b < batch; b++) im->run(p, x, y);
    double t = now_ns() - t0;
    *delay = run_delay_ns() - d0;
    *per_call = t / (double)batch;
    return (double)*delay * 50.0 > t; /* contended: waited > 2% of the block */
}

static unsigned long long g_retries_total, g_forced_total, g_mismatch_total, g_runs_checked;

static void bench_cell(size_t n, size_t m, double sp, FILE *out, int *first_row, int quick) {
    size_t nr = oma_rz_count();
    int8_t *w = malloc(m * n), *x = malloc(n);
    int32_t *yref = malloc(m * 4), *y = malloc(m * 4);
    if (!w || !x || !yref || !y) { fprintf(stderr, "oom\n"); exit(2); }
    for (size_t i = 0; i < m * n; i++) {
        uint64_t r = rnd();
        double u = (double)(r >> 11) * (1.0 / 9007199254740992.0);
        w[i] = u < sp ? 0 : ((r & 1) ? 1 : -1);
    }
    for (size_t j = 0; j < n; j++) x[j] = (int8_t)(uint8_t)rnd();
    oma_rz_oracle(w, m, n, x, yref);

    oma_rz_plan plans[MAX_RZ];
    cell_res res[MAX_RZ];
    memset(plans, 0, sizeof plans);
    memset(res, 0, sizeof res);
    for (size_t r = 0; r < nr; r++) {
        const oma_rz_impl *im = oma_rz_get(r);
        if (n > im->max_n) continue;
        double pk[8];
        int npk = 0;
        double t0 = now_ns();
        int rc = im->pack(&plans[r], w, m, n);
        pk[npk++] = now_ns() - t0;
        if (rc) { fprintf(stderr, "%s pack rc %d\n", im->id, rc); continue; }
        int extra = quick ? 2 : (pk[0] < 5e6 ? 6 : 2);
        for (int e = 0; e < extra; e++) {
            oma_rz_plan tmp;
            memset(&tmp, 0, sizeof tmp);
            t0 = now_ns();
            im->pack(&tmp, w, m, n);
            pk[npk++] = now_ns() - t0;
            oma_rz_free(&tmp);
        }
        qsort(pk, (size_t)npk, sizeof pk[0], cmp_d);
        res[r].pack_min = pk[0];
        res[r].pack_reps = npk;
        res[r].pack_med = pk[npk / 2];
        res[r].pack_noise = npk >= 4 ? (quantile(pk, npk, 0.75) - quantile(pk, npk, 0.25)) / res[r].pack_med
                                      : (pk[npk - 1] - pk[0]) / res[r].pack_med;
        /* correctness before timing */
        memset(y, 0x5a, m * 4);
        im->run(&plans[r], x, y);
        g_runs_checked++;
        res[r].verified = memcmp(y, yref, m * 4) == 0;
        if (!res[r].verified) { g_mismatch_total++; fprintf(stderr, "MISMATCH %s n=%zu m=%zu\n", im->id, n, m); continue; }
        res[r].ok = 1;
        res[r].weight_bytes = plans[r].weight_bytes;
        res[r].footprint = plans[r].footprint_bytes;
        res[r].working_set = plans[r].weight_bytes + plans[r].scratch_bytes + n + m * 4;
        res[r].nnz = plans[r].nnz;
        res[r].scratch = plans[r].scratch_bytes;
        /* calibrate batch */
        im->run(&plans[r], x, y);
        t0 = now_ns();
        im->run(&plans[r], x, y);
        double one = now_ns() - t0;
        long batch = (long)(TARGET_BLOCK_NS / (one > 1 ? one : 1));
        if (batch < 1) batch = 1;
        if (batch > 200000) batch = 200000;
        res[r].batch = batch;
    }
    /* interleaved rounds */
    for (int s = 0; s < NSAMPLES; s++)
        for (size_t r = 0; r < nr; r++) {
            if (!res[r].ok) continue;
            const oma_rz_impl *im = oma_rz_get(r);
            double pc;
            uint64_t dl;
            int tries = 0;
            while (timed_block(im, &plans[r], x, y, res[r].batch, &pc, &dl)) {
                res[r].retries++;
                g_retries_total++;
                if (++tries >= MAX_RETRY) { res[r].forced++; g_forced_total++; break; }
            }
            res[r].samples[s] = pc;
        }
    /* post-timing re-verification */
    for (size_t r = 0; r < nr; r++) {
        if (!res[r].ok) continue;
        const oma_rz_impl *im = oma_rz_get(r);
        memset(y, 0x5a, m * 4);
        im->run(&plans[r], x, y);
        g_runs_checked++;
        if (memcmp(y, yref, m * 4) != 0) { res[r].verified = 0; g_mismatch_total++; }
    }
    const char *best_id = "-";
    double best_med = 1e300;
    for (size_t r = 0; r < nr; r++) {
        const oma_rz_impl *im = oma_rz_get(r);
        if (!res[r].ok) {
            fprintf(out, "%s    {\"n\": %zu, \"m\": %zu, \"sparsity\": %.2f, \"rz\": \"%s\", \"eligible\": false, "
                         "\"verified\": %s, \"why\": \"%s\"}",
                    *first_row ? "" : ",\n", n, m, sp, im->id, res[r].verified ? "true" : "false",
                    n > im->max_n ? "n above the realization's max_n" : "failed verification");
            *first_row = 0;
            continue;
        }
        cell_res *c = &res[r];
        double srt[NSAMPLES];
        memcpy(srt, c->samples, sizeof srt);
        qsort(srt, NSAMPLES, sizeof srt[0], cmp_d);
        c->tmin = srt[0];
        c->tmed = quantile(srt, NSAMPLES, 0.5);
        c->q25 = quantile(srt, NSAMPLES, 0.25);
        c->q75 = quantile(srt, NSAMPLES, 0.75);
        int lv = bw_level_for(c->working_set);
        c->floor_level = g_bw[lv].bytes;
        c->floor_gbs = g_bw[lv].gbs;
        c->floor_ns = (double)c->working_set / c->floor_gbs; /* bytes / (bytes per ns) */
        c->pct_floor = 100.0 * c->floor_ns / c->tmin;
        double noise = (c->q75 - c->q25) / c->tmed;
        if (!im->weak_baseline && c->tmed < best_med) { best_med = c->tmed; best_id = im->id; }
        fprintf(out,
                "%s    {\"n\": %zu, \"m\": %zu, \"sparsity\": %.2f, \"rz\": \"%s\", \"family\": \"%s\", \"eligible\": true, "
                "\"verified\": %s, \"exact\": %d, \"weak_baseline\": %d, \"nnz\": %llu, \"weight_bytes\": %zu, "
                "\"bits_per_weight\": %.3f, \"footprint_bytes\": %zu, \"working_set_bytes\": %zu, "
                "\"floor_level_bytes\": %zu, \"floor_gbs\": %.2f, \"floor_ps\": %.0f, \"min_ps\": %.0f, "
                "\"median_ps\": %.0f, \"q25_ps\": %.0f, \"q75_ps\": %.0f, \"noise_rel\": %.4f, "
                "\"pct_floor_best\": %.1f, \"pct_floor_median\": %.1f, \"calls_per_s\": %.1f, \"gmac_per_s\": %.3f, "
                "\"pack_min_ps\": %.0f, \"pack_ps\": %.0f, \"pack_noise_rel\": %.4f, \"pack_reps\": %d, \"per_call_total_ps\": %.0f, \"batch\": %ld, "
                "\"samples\": %d, \"retries\": %d, \"forced\": %d}",
                *first_row ? "" : ",\n", n, m, sp, im->id, im->family, c->verified ? "true" : "false", im->exact,
                im->weak_baseline, (unsigned long long)c->nnz, c->weight_bytes,
                8.0 * (double)c->weight_bytes / ((double)m * (double)n), c->footprint, c->working_set,
                c->floor_level, c->floor_gbs, c->floor_ns * 1e3, c->tmin * 1e3, c->tmed * 1e3, c->q25 * 1e3,
                c->q75 * 1e3, noise, c->pct_floor, 100.0 * c->floor_ns / c->tmed, 1e9 / c->tmed,
                (double)m * (double)n / c->tmed, c->pack_min * 1e3, c->pack_med * 1e3, c->pack_noise, c->pack_reps,
                (c->pack_med + c->tmed) * 1e3, c->batch, NSAMPLES, c->retries, c->forced);
        *first_row = 0;
    }
    fprintf(stderr, "n=%5zu m=%4zu sp=%.1f  best(amortized)=%-12s %10.0f ns", n, m, sp, best_id, best_med);
    for (size_t r = 0; r < nr; r++)
        if (res[r].ok) fprintf(stderr, "  %s=%.0f(%.0f%%)", oma_rz_get(r)->id, res[r].tmed, res[r].pct_floor);
    fputc('\n', stderr);
    for (size_t r = 0; r < nr; r++) oma_rz_free(&plans[r]);
    free(w); free(x); free(yref); free(y);
}

/* Energy per call on the X925 cluster meter: idle window then run window
 * per realization; dynamic energy = E_run - P_idle * t_run. The meter
 * covers the whole cluster (10 cores) and updates about every 0.1 s, so
 * values are indicative on a shared machine. */
static void energy_shape(size_t n, size_t m, double sp, FILE *out, int *first) {
    int8_t *w = malloc(m * n), *x = malloc(n);
    int32_t *y = malloc(m * 4);
    if (!w || !x || !y) exit(2);
    for (size_t i = 0; i < m * n; i++) {
        uint64_t r = rnd();
        double u = (double)(r >> 11) * (1.0 / 9007199254740992.0);
        w[i] = u < sp ? 0 : ((r & 1) ? 1 : -1);
    }
    for (size_t j = 0; j < n; j++) x[j] = (int8_t)(uint8_t)rnd();
    for (size_t r = 0; r < oma_rz_count(); r++) {
        const oma_rz_impl *im = oma_rz_get(r);
        oma_rz_plan p;
        memset(&p, 0, sizeof p);
        if (n > im->max_n || im->pack(&p, w, m, n)) continue;
        double e0, e1, e2;
        struct timespec ts = {0, 400000000};
        if (read_energy_uj(&e0)) { oma_rz_free(&p); break; }
        double ti0 = now_ns();
        nanosleep(&ts, NULL);
        read_energy_uj(&e1);
        double ti = now_ns() - ti0;
        double tr0 = now_ns(), tr;
        long calls = 0;
        do {
            im->run(&p, x, y);
            calls++;
            tr = now_ns() - tr0;
        } while (tr < 8e8);
        read_energy_uj(&e2);
        double p_idle_w = (e1 - e0) / (ti / 1e3);        /* uJ per us = W */
        double dyn_uj = (e2 - e1) - p_idle_w * (tr / 1e3);
        fprintf(out, "%s    {\"n\": %zu, \"m\": %zu, \"sparsity\": %.2f, \"rz\": \"%s\", \"calls\": %ld, "
                     "\"run_s\": %.3f, \"cluster_idle_w\": %.3f, \"cluster_run_w\": %.3f, "
                     "\"dynamic_nj_per_call\": %.1f, \"total_nj_per_call\": %.1f}",
                *first ? "" : ",\n", n, m, sp, im->id, calls, tr / 1e9, p_idle_w, (e2 - e1) / (tr / 1e3),
                dyn_uj * 1e3 / (double)calls, (e2 - e1) * 1e3 / (double)calls);
        *first = 0;
        oma_rz_free(&p);
    }
    free(w); free(x); free(y);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s OUT.json\n", argv[0]); return 2; }
    int quick = getenv("OMA_BENCH_QUICK") && atoi(getenv("OMA_BENCH_QUICK")) > 0;
    read_cpu_parts();
    char why[128];
    int cpu = pick_x925_cpu(why, sizeof why);
    if (cpu < 0) { fprintf(stderr, "no Cortex-X925 cpu found\n"); return 2; }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof set, &set) != 0) { perror("sched_setaffinity"); return 2; }
    int part = cpu_part[cpu];
    if (part != 0xd85) { fprintf(stderr, "cpu %d is not a Cortex-X925 (part 0x%x)\n", cpu, part); return 2; }
    char la0[64], la1[64];
    read_loadavg(la0, sizeof la0);
    int th0 = read_thermal_c();
    double wall0 = now_ns();
    fprintf(stderr, "pinned to cpu %d (Cortex-X925, %s), loadavg %s\n", cpu, why, la0);
    measure_bandwidth();
    for (int l = 0; l < NBW; l++) fprintf(stderr, "  stream read %8zu KiB: %.1f GB/s\n", g_bw[l].bytes >> 10, g_bw[l].gbs);
    find_energy_meter();

    char tmp_path[1024];
    snprintf(tmp_path, sizeof tmp_path, "%s.rows.tmp", argv[1]);
    FILE *rows = fopen(tmp_path, "w+");
    if (!rows) { perror(tmp_path); return 2; }
    static const size_t ns_full[] = {1024, 4096, 16384}, ms_full[] = {1, 64, 4096};
    static const double sps_full[] = {0.0, 0.3, 0.6, 0.9};
    size_t nn = quick ? 2 : 3, nm = quick ? 2 : 3, nsp = quick ? 2 : 4;
    int first = 1;
    for (size_t a = 0; a < nn; a++)
        for (size_t b = 0; b < nm; b++)
            for (size_t c = 0; c < nsp; c++) bench_cell(ns_full[a], ms_full[b], sps_full[c], rows, &first, quick);
    double grid_s = (now_ns() - wall0) / 1e9;

    FILE *en = tmpfile();
    int efirst = 1;
    double ec;
    int energy_ok = g_energy_path[0] && read_energy_uj(&ec) == 0;
    if (energy_ok && !quick) {
        energy_shape(16384, 4096, 0.3, en, &efirst);
        energy_shape(1024, 4096, 0.3, en, &efirst);
    }
    read_loadavg(la1, sizeof la1);
    int th1 = read_thermal_c();

    time_t now = time(NULL);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    char run_id[64];
    strftime(run_id, sizeof run_id, "%Y%m%dT%H%M%SZ", &tmv);
    const char *commit = getenv("OMA_BENCH_COMMIT"), *dirty = getenv("OMA_BENCH_DIRTY"),
               *bsha = getenv("OMA_BENCH_BIN_SHA");
    struct utsname un;
    uname(&un);
    FILE *o = fopen(argv[1], "w");
    if (!o) { perror(argv[1]); return 2; }
    fprintf(o, "{\n  \"schema\": \"OMEGA_MIXED_ALGEBRA_MA2_BENCH_V1\",\n  \"run_id\": \"%s-%s\",\n", run_id,
            commit ? commit : "unknown");
    fprintf(o, "  \"operation\": \"Omega-X: y = W.x, W in {-1,0,+1}^(m x n), x int8^n, y int32^m, exact\",\n");
    fprintf(o, "  \"run_commit\": \"%s\",\n  \"tree_dirty\": %s,\n  \"bench_binary_sha256\": \"%s\",\n",
            commit ? commit : "unknown", (dirty && atoi(dirty) > 0) ? "true" : "false", bsha ? bsha : "unknown");
    fprintf(o, "  \"compiler\": \"gcc %s, -std=c11 -O2 -march=armv8.6-a+dotprod+i8mm+sve\",\n", __VERSION__);
    fprintf(o, "  \"host\": {\"sysname\": \"%s\", \"release\": \"%s\", \"machine\": \"%s\", \"timed_cpu\": %d, "
               "\"timed_core\": \"Cortex-X925 (MIDR part 0x%x)\", \"cpu_choice\": \"%s\", \"loadavg_start\": \"%s\", "
               "\"loadavg_end\": \"%s\", \"thermal_c_start\": %d, \"thermal_c_end\": %d, \"grid_wall_s\": %.1f, "
               "\"shared_machine\": \"other sessions may run; blocks with run_delay > 2%% of the block were retried "
               "(up to %d times), costs are best-of and median of %d interleaved blocks\"},\n",
            un.sysname, un.release, un.machine, cpu, part, why, la0, la1, th0, th1, grid_s, MAX_RETRY, NSAMPLES);
    fprintf(o, "  \"bandwidth\": [");
    for (int l = 0; l < NBW; l++)
        fprintf(o, "%s{\"bytes\": %zu, \"read_gbs\": %.2f}", l ? ", " : "", g_bw[l].bytes, g_bw[l].gbs);
    fprintf(o, "],\n  \"bandwidth_method\": \"best of three NEON xor-reduce read kernels (ld1x4, independent q loads, 4 concurrent streams), hot buffer of the given size, best of 7 each, >= 64 MiB read per sample; roofline floor = working_set_bytes / read_gbs at the smallest measured size >= working set\",\n");
    fprintf(o, "  \"realizations\": [\n");
    for (size_t r = 0; r < oma_rz_count(); r++) {
        const oma_rz_impl *im = oma_rz_get(r);
        fprintf(o, "    {\"id\": \"%s\", \"family\": \"%s\", \"exact\": %d, \"weak_baseline\": %d, \"max_n\": %zu, "
                   "\"label\": \"%s\"}%s\n",
                im->id, im->family, im->exact, im->weak_baseline, im->max_n, im->label,
                r + 1 < oma_rz_count() ? "," : "");
    }
    fprintf(o, "  ],\n  \"correctness\": {\"runs_checked_against_oracle\": %llu, \"mismatches\": %llu},\n",
            g_runs_checked, g_mismatch_total);
    /* top-level copies of the oracle counts (the MA-3-label runs carry them only in \"correctness\") */
    fprintf(o, "  \"oracle_checks\": %llu,\n  \"oracle_mismatches\": %llu,\n", g_runs_checked, g_mismatch_total);
    fprintf(o, "  \"contention\": {\"blocks_retried\": %llu, \"blocks_forced_after_%d_retries\": %llu},\n",
            g_retries_total, MAX_RETRY, g_forced_total);
    fprintf(o, "  \"cost_table\": [\n");
    fflush(rows);
    rewind(rows);
    char buf[8192];
    size_t k;
    while ((k = fread(buf, 1, sizeof buf, rows)) > 0) fwrite(buf, 1, k, o);
    fclose(rows);
    remove(tmp_path);
    fprintf(o, "\n  ],\n");
    fprintf(o, "  \"energy\": {\"meter\": \"%s\", \"scope\": \"aien_spbm cpu_p = whole Cortex-X925 cluster (10 cores), "
               "updates about every 0.1 s; idle 0.4 s then run 0.8 s per realization; dynamic = run minus idle power; "
               "indicative only on a shared machine\", \"rows\": [\n",
            energy_ok ? g_energy_path : "not readable");
    fflush(en);
    rewind(en);
    while ((k = fread(buf, 1, sizeof buf, en)) > 0) fwrite(buf, 1, k, o);
    fclose(en);
    fprintf(o, "\n  ]},\n  \"label\": \"MA-2 cost table; consumed by the stand-in selector (oma_select), not wired to rx_costmodel\"\n}\n");
    fclose(o);
    fprintf(stderr, "wrote %s (grid %.1f s, total %.1f s, mismatches %llu, retried blocks %llu)\n", argv[1], grid_s,
            (now_ns() - wall0) / 1e9, g_mismatch_total, g_retries_total);
    return g_mismatch_total ? 1 : 0;
}
