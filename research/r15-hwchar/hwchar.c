/*
 * hwchar.c -- R15 hardware characterization (2026-09-28).
 *
 * Question: did the physical machine change after the morning of 2026-09-28,
 * or only the benchmark ratio? Times the exact code R10-R15 compare -- Omega's
 * reference matvec and the quad4 realization, 64x256, the R13 regime -- pinned
 * to one core, while reading that core's own PMU (cycles, instructions,
 * L2 refill, last-level miss, memory access), CPU migrations, SPBM package
 * energy and the ACPI temperatures. JSON Lines on stdout. Research tool, not
 * part of any gate.
 *
 *   hwchar freq                         effective GHz + IPC of a dependent-add
 *                                       loop on every CPU (300 ms each)
 *   hwchar bench <cpu> <ref|quad4> <cold|warm>
 *   hwchar sustain <cpu> <ref|quad4> <seconds>   one record per second
 *
 * Needs kernel.perf_event_paranoid <= 2 for the PMU; without it the timing
 * still runs and the counters are reported as absent.
 */
#include "omega_machine.h"
#include "omega_matvec.h"
#include "omega_matvec_quad.h"

#include <errno.h>
#include <inttypes.h>
#include <linux/perf_event.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define M 64u
#define N 256u

typedef void (*MatVecFn)(const uint64_t *, const uint64_t *, uint64_t *, uint64_t, uint64_t);

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int pin(int cpu) {
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    return sched_setaffinity(0, sizeof s, &s);
}

/* ---- the core's own PMU ------------------------------------------------- */

enum { EV_CYCLES, EV_INST, EV_L2_REFILL, EV_LL_MISS, EV_MEM, EV_MIGR, EV_N };
static const char *const ev_name[EV_N] = {
    "cycles", "instructions", "l2d_refill", "ll_miss_rd", "mem_access", "migrations" };
static const uint64_t ev_code[EV_MIGR] = { 0x11, 0x08, 0x17, 0x37, 0x13 };
static int ev_fd[EV_N] = { -1, -1, -1, -1, -1, -1 };

static int midr_part(int cpu) {
    char p[160];
    snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/regs/identification/midr_el1", cpu);
    FILE *f = fopen(p, "r");
    unsigned long long v = 0;
    if (!f) return -1;
    int ok = fscanf(f, "%llx", &v);
    fclose(f);
    return ok == 1 ? (int)((v >> 4) & 0xfff) : -1;
}

/* The PMU whose "cpus" list holds this cpu. */
static int pmu_type_for(int cpu) {
    for (int i = 0; i < 4; i++) {
        char p[160], buf[256];
        snprintf(p, sizeof p, "/sys/bus/event_source/devices/armv8_pmuv3_%d/cpus", i);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (!fgets(buf, sizeof buf, f)) { fclose(f); continue; }
        fclose(f);
        int in = 0;
        for (char *t = strtok(buf, ",\n"); t; t = strtok(NULL, ",\n")) {
            int a, b;
            if (sscanf(t, "%d-%d", &a, &b) == 2) { if (cpu >= a && cpu <= b) in = 1; }
            else if (atoi(t) == cpu) in = 1;
        }
        if (!in) continue;
        snprintf(p, sizeof p, "/sys/bus/event_source/devices/armv8_pmuv3_%d/type", i);
        f = fopen(p, "r");
        int type = -1;
        if (f) { if (fscanf(f, "%d", &type) != 1) type = -1; fclose(f); }
        return type;
    }
    return -1;
}

static int perf_open(uint32_t type, uint64_t config) {
    struct perf_event_attr a;
    memset(&a, 0, sizeof a);
    a.size = sizeof a;
    a.type = type;
    a.config = config;
    a.exclude_kernel = 1;
    a.exclude_hv = 1;
    return (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
}

static void pmu_open(int cpu) {
    int type = pmu_type_for(cpu);
    for (int e = 0; e < EV_MIGR; e++) ev_fd[e] = type >= 0 ? perf_open((uint32_t)type, ev_code[e]) : -1;
    ev_fd[EV_MIGR] = perf_open(PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_MIGRATIONS);
}

static void pmu_read(uint64_t v[EV_N]) {
    for (int e = 0; e < EV_N; e++) {
        v[e] = UINT64_MAX;
        if (ev_fd[e] >= 0 && read(ev_fd[e], &v[e], sizeof v[e]) != (ssize_t)sizeof v[e]) v[e] = UINT64_MAX;
    }
}

static void print_pmu(const uint64_t a[EV_N], const uint64_t b[EV_N], uint64_t calls, uint64_t ns) {
    for (int e = 0; e < EV_N; e++) {
        if (a[e] == UINT64_MAX || b[e] == UINT64_MAX) { printf(",\"%s\":null", ev_name[e]); continue; }
        printf(",\"%s\":%" PRIu64, ev_name[e], b[e] - a[e]);
    }
    if (a[EV_CYCLES] != UINT64_MAX && b[EV_CYCLES] != UINT64_MAX && ns) {
        double cyc = (double)(b[EV_CYCLES] - a[EV_CYCLES]);
        printf(",\"eff_ghz\":%.4f", cyc / (double)ns);
        if (a[EV_INST] != UINT64_MAX && cyc > 0)
            printf(",\"ipc\":%.4f", (double)(b[EV_INST] - a[EV_INST]) / cyc);
        if (calls) printf(",\"cycles_per_call\":%.1f", cyc / (double)calls);
    }
}

/* ---- machine state ------------------------------------------------------ */

static uint64_t read_u64(const char *path) {
    FILE *f = fopen(path, "r");
    unsigned long long v = 0;
    if (!f) return UINT64_MAX;
    int ok = fscanf(f, "%llu", &v);
    fclose(f);
    return ok == 1 ? v : UINT64_MAX;
}

static char g_spbm[128];

static void find_spbm(void) {
    for (int i = 0; i < 16; i++) {
        char p[128], name[64] = "";
        snprintf(p, sizeof p, "/sys/class/hwmon/hwmon%d/name", i);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (fgets(name, sizeof name, f) && strncmp(name, "aien_spbm", 9) == 0)
            snprintf(g_spbm, sizeof g_spbm, "/sys/class/hwmon/hwmon%d/energy1_input", i);
        fclose(f);
    }
}

static uint64_t spbm_uj(void) { return g_spbm[0] ? read_u64(g_spbm) : UINT64_MAX; }

static int64_t max_temp_mc(void) {
    int64_t best = -1;
    for (int i = 0; i < 16; i++) {
        char p[96];
        snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/temp", i);
        uint64_t t = read_u64(p);
        if (t != UINT64_MAX && (int64_t)t > best) best = (int64_t)t;
    }
    return best;
}

static uint64_t cur_khz(int cpu) {
    char p[128];
    snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
    return read_u64(p);
}

/* ---- the two realizations ----------------------------------------------- */

static void ref_fn(const uint64_t *A, const uint64_t *x, uint64_t *y, uint64_t m, uint64_t n) {
    omega_matvec_reference(A, x, y, (uint32_t)m, (uint32_t)n);
}

static MatVecFn quad4_fn(void) {
    MatVecSemanticSpec spec;
    OmegaMachineGraph mg;
    MatVecRealization mv;
    /* Exactly as rx_omega_faculty_init prepares them (rx_omega.c). */
    if (omega_machine_build_dgx_spark(&mg) != 0 ||
        omega_matvec_spec_init(&spec, "rx_omega_matvec", 1u << 16, 1u << 16) != 0 ||
        omega_matvec_synthesize_quad4(&spec, &mg, &mv) != 0)
        return NULL;
    size_t len = mv.realization.code_len;
    void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) return NULL;
    memcpy(page, mv.realization.code_bytes, len);
    __builtin___clear_cache((char *)page, (char *)page + len);
    if (mprotect(page, 4096, PROT_READ | PROT_EXEC) != 0) return NULL;
    union { void *p; MatVecFn fn; } u;
    u.p = page;
    return u.fn;
}

static uint64_t g_A[M * N], g_x[N], g_y[M], g_yr[M];

static void fill(uint64_t seed) {
    uint64_t s = seed * 0x9e3779b97f4a7c15ull + 1;
    for (uint32_t i = 0; i < M * N; i++) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; g_A[i] = s; }
    for (uint32_t i = 0; i < N; i++) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; g_x[i] = s; }
}

static void header(const char *mode, int cpu, const char *kind) {
    printf("{\"mode\":\"%s\",\"cpu\":%d,\"part\":\"0x%x\",\"kind\":\"%s\",\"t_ns\":%" PRIu64,
           mode, cpu, midr_part(cpu), kind, now_ns());
}

/* ---- modes -------------------------------------------------------------- */

static int do_freq(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    for (int cpu = 0; cpu < n; cpu++) {
        if (pin(cpu) != 0) continue;
        pmu_open(cpu);
        uint64_t a[EV_N], b[EV_N];
        pmu_read(a);
        uint64_t t0 = now_ns(), iters = 0;
        while (now_ns() - t0 < 300000000ull) {
            uint64_t r = 0;
            for (int i = 0; i < 100000; i++) __asm__ volatile("add %0, %0, #1" : "+r"(r));
            iters++;
        }
        uint64_t t1 = now_ns();
        pmu_read(b);
        header("freq", cpu, "dep_add");
        printf(",\"ns\":%" PRIu64 ",\"sched_cpu\":%d,\"cur_khz\":%" PRIu64, t1 - t0, sched_getcpu(),
               cur_khz(cpu));
        print_pmu(a, b, 0, t1 - t0);
        printf("}\n");
        for (int e = 0; e < EV_N; e++) if (ev_fd[e] >= 0) { close(ev_fd[e]); ev_fd[e] = -1; }
    }
    return 0;
}

static int do_bench(int cpu, MatVecFn fn, const char *kind, const char *how) {
    fill(0x15);
    pmu_open(cpu);
    if (strcmp(how, "cold") == 0) {
        /* First calls of a fresh process on a fresh page, one at a time. */
        uint64_t t[10];
        for (int i = 0; i < 10; i++) {
            uint64_t t0 = now_ns();
            fn(g_A, g_x, g_y, M, N);
            t[i] = now_ns() - t0;
        }
        header("cold", cpu, kind);
        printf(",\"first_calls_ns\":[");
        for (int i = 0; i < 10; i++) printf("%s%" PRIu64, i ? "," : "", t[i]);
        printf("]}\n");
        return 0;
    }
    /* Warm: fn_measure's procedure -- rounds of ~200 us, best of 7. */
    fn(g_A, g_x, g_y, M, N);
    omega_matvec_reference(g_A, g_x, g_yr, M, N);
    if (memcmp(g_y, g_yr, sizeof g_y) != 0) { fprintf(stderr, "wrong result\n"); return 1; }
    uint64_t t0 = now_ns();
    fn(g_A, g_x, g_y, M, N);
    uint64_t one = now_ns() - t0;
    uint32_t iters = one ? (uint32_t)(200000u / one) : 1000u;
    if (iters < 4) iters = 4;
    uint64_t best = UINT64_MAX, round_ns[7];
    uint64_t a[EV_N], b[EV_N];
    pmu_read(a);
    uint64_t w0 = now_ns();
    for (int r = 0; r < 7; r++) {
        uint64_t s = now_ns();
        for (uint32_t i = 0; i < iters; i++) fn(g_A, g_x, g_y, M, N);
        round_ns[r] = now_ns() - s;
        if (round_ns[r] < best) best = round_ns[r];
    }
    uint64_t w1 = now_ns();
    pmu_read(b);
    header("warm", cpu, kind);
    printf(",\"iters\":%u,\"best_ps_per_call\":%" PRIu64 ",\"rounds_ps_per_call\":[", iters,
           best * 1000u / iters);
    for (int r = 0; r < 7; r++) printf("%s%" PRIu64, r ? "," : "", round_ns[r] * 1000u / iters);
    printf("]");
    print_pmu(a, b, (uint64_t)iters * 7u, w1 - w0);
    printf("}\n");
    return 0;
}

static int do_sustain(int cpu, MatVecFn fn, const char *kind, int seconds) {
    fill(0x15);
    pmu_open(cpu);
    fn(g_A, g_x, g_y, M, N);
    for (int s = 0; s < seconds; s++) {
        uint64_t a[EV_N], b[EV_N];
        uint64_t e0 = spbm_uj();
        pmu_read(a);
        uint64_t t0 = now_ns(), calls = 0;
        while (now_ns() - t0 < 1000000000ull) {
            for (int i = 0; i < 64; i++) fn(g_A, g_x, g_y, M, N);
            calls += 64;
        }
        uint64_t t1 = now_ns();
        pmu_read(b);
        uint64_t e1 = spbm_uj();
        header("sustain", cpu, kind);
        printf(",\"second\":%d,\"calls\":%" PRIu64 ",\"ps_per_call\":%" PRIu64
               ",\"cur_khz\":%" PRIu64 ",\"max_temp_mc\":%" PRId64,
               s, calls, (t1 - t0) * 1000u / (calls ? calls : 1), cur_khz(cpu), max_temp_mc());
        if (e0 != UINT64_MAX && e1 != UINT64_MAX && e1 >= e0)
            printf(",\"pkg_w\":%.3f", (double)(e1 - e0) / ((double)(t1 - t0) / 1e3));
        print_pmu(a, b, calls, t1 - t0);
        printf("}\n");
        fflush(stdout);
    }
    return 0;
}

int main(int argc, char **argv) {
    find_spbm();
    if (argc >= 2 && strcmp(argv[1], "freq") == 0) return do_freq();
    if (argc < 5) {
        fprintf(stderr, "usage: hwchar freq | bench <cpu> <ref|quad4> <cold|warm> | "
                        "sustain <cpu> <ref|quad4> <seconds>\n");
        return 2;
    }
    int cpu = atoi(argv[2]);
    if (pin(cpu) != 0) { perror("pin"); return 2; }
    MatVecFn fn = strcmp(argv[3], "quad4") == 0 ? quad4_fn() : ref_fn;
    if (!fn) { fprintf(stderr, "quad4 synthesis failed\n"); return 2; }
    if (strcmp(argv[1], "bench") == 0) return do_bench(cpu, fn, argv[3], argv[4]);
    if (strcmp(argv[1], "sustain") == 0) return do_sustain(cpu, fn, argv[3], atoi(argv[4]));
    return 2;
}
