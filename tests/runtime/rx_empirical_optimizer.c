/*
 * OMEGA_EMPIRICAL_OPTIMIZER -- realization choice from measured experience.
 *
 * Arms are the ways Omega can realize its integer matvec on this machine:
 * the compiled semantic reference, the three M12 emitters, quad4 (R10), and
 * one planted wrong realization. Each is verified in a forked child before
 * the parent may run it. The planted one must be refused and never run.
 *
 * The learner (rx_costmodel) is trained here by its own runs only, under a
 * bounded exploration budget. A candidate model is proposed as the `model`
 * blob of an R9 generation; an independent judge re-checks it on validation
 * work and a separate subject holding the promotion right promotes it. The
 * learner cannot promote. After a restart the durable model is read back and
 * faces held-out workloads it never saw:
 *
 *   H1  unseen shapes
 *   H2  a thin-matrix-heavy mix (where the best single realization loses)
 *   H3  H1's shapes under memory pressure, a condition absent from training
 *
 * Every policy is charged from the same per-job cost table: each eligible
 * arm timed in rotating order, best of 5 rounds. The learner never sees the
 * table; it learns only from the runs it chose to make. The online learner's
 * cost includes every extra run it made to measure. Every run of every arm
 * is checked against the reference digest for that job.
 *
 * A second, independent comparison replays each policy's choices end to end
 * in alternating blocks and reads wall time and the cluster energy meter.
 */
#include "runtime/aienos_cap.h"
#include "runtime/rx_caproot.h"
#include "runtime/rx_costmodel.h"
#include "runtime/rx_generation.h"
#include "omega_evidence.h"
#include "omega_machine.h"
#include "omega_matvec.h"
#include "omega_matvec_quad.h"
#include "sha256.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_checks;
static int g_fail;

#define CHECK(cond, ...) do {                                            \
        g_checks++;                                                      \
        if (!(cond)) {                                                   \
            g_fail++;                                                    \
            fprintf(stderr, "  FAIL %s:%d ", __FILE__, __LINE__);        \
            fprintf(stderr, __VA_ARGS__);                                \
            fputc('\n', stderr);                                         \
        }                                                                \
    } while (0)

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- arms ---- */

enum { ARM_REF = 0, ARM_SCALAR, ARM_U2, ARM_U4D, ARM_QUAD4, ARM_PLANTED, N_ARMS };
static const char *const ARM_NAME[N_ARMS] = { "reference", "scalar", "unroll2",
                                              "unroll4_dual", "quad4", "planted_wrong" };
typedef void (*MvFn)(const uint64_t *, const uint64_t *, uint64_t *, uint64_t, uint64_t);

static MvFn g_fn[N_ARMS];
static int g_verified[N_ARMS];
static uint64_t g_parent_runs[N_ARMS];
static uint64_t g_refused_runs;
static uint32_t g_eligible;

static void arm_reference(const uint64_t *A, const uint64_t *x, uint64_t *y, uint64_t M,
                          uint64_t N) {
    omega_matvec_reference(A, x, y, (uint32_t)M, (uint32_t)N);
}

/* Planted: drops the last N % 4 columns. Right whenever N % 4 == 0. */
static void arm_planted(const uint64_t *A, const uint64_t *x, uint64_t *y, uint64_t M,
                        uint64_t N) {
    uint64_t n4 = N - (N % 4u);
    for (uint64_t i = 0; i < M; i++) {
        uint64_t s = 0;
        for (uint64_t j = 0; j < n4; j++) s += A[i * N + j] * x[j];
        y[i] = s;
    }
}

/* The only way the parent runs an arm. */
static int exec_arm(uint32_t arm, const uint64_t *A, const uint64_t *x, uint64_t *y, uint32_t M,
                    uint32_t N) {
    if (arm >= N_ARMS || !g_verified[arm]) {
        g_refused_runs++;
        return -1;
    }
    g_parent_runs[arm]++;
    g_fn[arm](A, x, y, M, N);
    return 0;
}

/* ---- buffers and jobs ---- */

#define MAX_ELEMS (1u << 19)
#define MAX_N     8192u
#define MAX_M     8192u
#define GUARD     8u

static uint64_t *g_A, *g_x, *g_y;

typedef struct {
    uint32_t M, N, calls;
    uint64_t seed;
    uint64_t ref_digest;
} Job;

static uint64_t xs(uint64_t *s) {
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

static uint64_t digest_y(const uint64_t *y, uint32_t M) {
    uint8_t d[32];
    sha256_hash((const uint8_t *)y, (size_t)M * 8u, d);
    uint64_t v;
    memcpy(&v, d, 8);
    return v;
}

static void job_load(Job *j) {
    uint64_t s = j->seed | 1u;
    for (uint64_t i = 0; i < (uint64_t)j->M * j->N; i++) g_A[i] = xs(&s);
    for (uint32_t i = 0; i < j->N; i++) g_x[i] = xs(&s);
    omega_matvec_reference(g_A, g_x, g_y, j->M, j->N);
    j->ref_digest = digest_y(g_y, j->M);
}

static uint32_t calls_for(uint32_t M, uint32_t N) {
    uint64_t c = 600000ull / ((uint64_t)M * N + 64u);
    if (c < 3) c = 3;
    if (c > 3000) c = 3000;
    return (uint32_t)c;
}

static struct {
    uint64_t runs_checked, mismatches;
} K;

/* Time `calls` calls of one arm on the loaded job; ps per call. Checks the
 * output against the reference digest. 0 if the arm may not run. */
static uint64_t time_arm(uint32_t arm, const Job *j) {
    memset(g_y, 0, (size_t)j->M * 8u);
    uint64_t t0 = now_ns();
    for (uint32_t c = 0; c < j->calls; c++)
        if (exec_arm(arm, g_A, g_x, g_y, j->M, j->N) != 0) return 0;
    uint64_t dt = now_ns() - t0;
    K.runs_checked++;
    if (digest_y(g_y, j->M) != j->ref_digest) K.mismatches++;
    uint64_t ps = dt * 1000u / j->calls;
    return ps ? ps : 1;
}

/* The learner's own measurement of `calls` calls: the job's calls in three
 * chunks, fastest chunk kept (another tenant on the core inflates a chunk,
 * not the floor). Checks the output like time_arm. */
static uint64_t time_arm_chunked(uint32_t arm, const Job *j, uint32_t calls) {
    if (calls < 3) calls = 3;
    uint32_t per = calls / 3u, best = 0;
    uint64_t best_ps = UINT64_MAX;
    memset(g_y, 0, (size_t)j->M * 8u);
    for (uint32_t k = 0; k < 3; k++) {
        uint64_t t0 = now_ns();
        for (uint32_t c = 0; c < per; c++)
            if (exec_arm(arm, g_A, g_x, g_y, j->M, j->N) != 0) return 0;
        uint64_t ps = (now_ns() - t0) * 1000u / per;
        if (ps < best_ps) best_ps = ps;
        best++;
    }
    (void)best;
    K.runs_checked++;
    if (digest_y(g_y, j->M) != j->ref_digest) K.mismatches++;
    return best_ps ? best_ps : 1;
}

/* The harness's cost table: every eligible arm, rotating order, best of 5. */
static void truth_table(const Job *j, uint64_t table[N_ARMS]) {
    uint32_t arms[N_ARMS], n = 0;
    for (uint32_t a = 0; a < N_ARMS; a++) {
        table[a] = UINT64_MAX;
        if (g_eligible & (1u << a)) arms[n++] = a;
    }
    for (uint32_t r = 0; r < 5; r++)
        for (uint32_t i = 0; i < n; i++) {
            uint32_t a = arms[(i + r) % n];
            uint64_t t = time_arm(a, j);
            if (t && t < table[a]) table[a] = t;
        }
}

/* ---- placement, thermal, energy, pressure ---- */

typedef struct {
    int cpus[64];
    int n;
    int timed;
    const char *name;
    const char *meter;      /* spbm label of the cluster, NULL if none */
    uint64_t cache_bytes;   /* private L2 per core (omega_machine: X925 2 MiB, A725 512 KiB) */
} CoreClass;

static CoreClass g_class[RX_CM_CORES];
static char g_cpus_desc[256];

static void discover(void) {
    g_class[RX_CM_CORE_X925] = (CoreClass){ .name = "Cortex-X925", .meter = "cpu_p",
                                            .cache_bytes = 2u << 20 };
    g_class[RX_CM_CORE_A725] = (CoreClass){ .name = "Cortex-A725", .meter = "cpu_e",
                                            .cache_bytes = 512u << 10 };
    g_class[RX_CM_CORE_OTHER] = (CoreClass){ .name = "other", .meter = NULL,
                                             .cache_bytes = 1u << 20 };
    long n = sysconf(_SC_NPROCESSORS_CONF);
    for (long c = 0; c < n && c < 64; c++) {
        char path[128];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%ld/regs/identification/midr_el1", c);
        FILE *fp = fopen(path, "r");
        unsigned long long midr = 0;
        int ok = fp && fscanf(fp, "%llx", &midr) == 1;
        if (fp) fclose(fp);
        uint32_t part = ok ? (uint32_t)((midr >> 4) & 0xfffu) : 0;
        uint32_t k = part == 0xd85u ? RX_CM_CORE_X925 : part == 0xd87u ? RX_CM_CORE_A725
                                                                       : RX_CM_CORE_OTHER;
        g_class[k].cpus[g_class[k].n++] = (int)c;
    }
    char *p = g_cpus_desc;
    for (uint32_t k = 0; k < RX_CM_CORES; k++) {
        CoreClass *cc = &g_class[k];
        if (!cc->n) continue;
        cc->timed = cc->cpus[cc->n - 1];
        p += snprintf(p, sizeof g_cpus_desc - (size_t)(p - g_cpus_desc), "%s%s x%d timed on cpu%d",
                      p == g_cpus_desc ? "" : "; ", cc->name, cc->n, cc->timed);
    }
}

/* Busy fraction of each cpu over ~150 ms, from /proc/stat. */
static void cpu_busy(double *busy, int ncpu) {
    unsigned long long a[2][64][2];
    memset(a, 0, sizeof a);
    for (int s = 0; s < 2; s++) {
        FILE *fp = fopen("/proc/stat", "r");
        char line[512];
        while (fp && fgets(line, sizeof line, fp)) {
            int c;
            unsigned long long u, n, sy, id, io, irq, sirq, st;
            if (sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu", &c, &u, &n, &sy, &id,
                       &io, &irq, &sirq, &st) == 9 && c >= 0 && c < 64) {
                a[s][c][0] = u + n + sy + irq + sirq + st;
                a[s][c][1] = u + n + sy + irq + sirq + st + id + io;
            }
        }
        if (fp) fclose(fp);
        if (s == 0) {
            struct timespec ts = { 0, 150000000 };
            nanosleep(&ts, NULL);
        }
    }
    for (int c = 0; c < ncpu && c < 64; c++) {
        unsigned long long tot = a[1][c][1] - a[0][c][1];
        busy[c] = tot ? (double)(a[1][c][0] - a[0][c][0]) / (double)tot : 1.0;
    }
}

static int pin(int cpu) {
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    return sched_setaffinity(0, sizeof s, &s);
}

/* Time on the quietest core of the class right now: this machine is shared. */
static char g_timed_log[512];
static void place_quiet(uint32_t k) {
    double busy[64];
    cpu_busy(busy, 64);
    CoreClass *cc = &g_class[k];
    int best = cc->cpus[cc->n - 1];
    for (int i = 0; i < cc->n; i++)
        if (busy[cc->cpus[i]] < busy[best]) best = cc->cpus[i];
    cc->timed = best;
    pin(best);
    size_t l = strlen(g_timed_log);
    if (l < sizeof g_timed_log - 16)
        snprintf(g_timed_log + l, sizeof g_timed_log - l, "%s%d", l ? "," : "", best);
}

static void loadavg(char *out, size_t n) {
    FILE *fp = fopen("/proc/loadavg", "r");
    double a = 0, b = 0, c = 0;
    if (fp) {
        if (fscanf(fp, "%lf %lf %lf", &a, &b, &c) != 3) a = b = c = 0;
        fclose(fp);
    }
    snprintf(out, n, "%.2f %.2f %.2f", a, b, c);
}

static uint32_t thermal_c(void) {
    uint32_t best = 0;
    for (int h = 0; h < 16; h++) {
        char path[128], name[64] = { 0 };
        snprintf(path, sizeof path, "/sys/class/hwmon/hwmon%d/name", h);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        int ok = fscanf(fp, "%63s", name) == 1;
        fclose(fp);
        if (!ok || strcmp(name, "acpitz") != 0) continue;
        for (int t = 1; t <= 8; t++) {
            snprintf(path, sizeof path, "/sys/class/hwmon/hwmon%d/temp%d_input", h, t);
            fp = fopen(path, "r");
            if (!fp) continue;
            long v = 0;
            if (fscanf(fp, "%ld", &v) == 1 && v / 1000 > (long)best) best = (uint32_t)(v / 1000);
            fclose(fp);
        }
    }
    return best;
}

static char g_meter_dir[128];

static void find_meter(void) {
    for (int h = 0; h < 32; h++) {
        char path[128], name[64] = { 0 };
        snprintf(path, sizeof path, "/sys/class/hwmon/hwmon%d/name", h);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        int ok = fscanf(fp, "%63s", name) == 1;
        fclose(fp);
        if (ok && strcmp(name, "aien_spbm") == 0) {
            snprintf(g_meter_dir, sizeof g_meter_dir, "/sys/class/hwmon/hwmon%d", h);
            return;
        }
    }
}

/* Cumulative microjoules of the cluster meter with this label; 0 if none. */
static uint64_t meter_uj(const char *label) {
    if (!label || !g_meter_dir[0]) return 0;
    for (int i = 1; i <= 8; i++) {
        char path[192], lab[64] = { 0 };
        snprintf(path, sizeof path, "%s/energy%d_label", g_meter_dir, i);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        int ok = fscanf(fp, "%63s", lab) == 1;
        fclose(fp);
        if (!ok || strcmp(lab, label) != 0) continue;
        snprintf(path, sizeof path, "%s/energy%d_input", g_meter_dir, i);
        fp = fopen(path, "r");
        unsigned long long v = 0;
        if (fp) {
            if (fscanf(fp, "%llu", &v) != 1) v = 0;
            fclose(fp);
        }
        return v;
    }
    return 0;
}

#define PRESS_THREADS 6
#define PRESS_BYTES   (64u << 20)

static atomic_int g_press_stop;
static pthread_t g_press[PRESS_THREADS];
static int g_press_n;
static uint64_t g_press_sweeps[PRESS_THREADS];

typedef struct {
    int cpu;
    int idx;
} PressArg;
static PressArg g_press_arg[PRESS_THREADS];

static void *press_main(void *arg) {
    PressArg *pa = arg;
    pin(pa->cpu);
    uint64_t *buf = malloc(PRESS_BYTES);
    if (!buf) return NULL;
    size_t n = PRESS_BYTES / 8u;
    for (size_t i = 0; i < n; i++) buf[i] = i;
    uint64_t sweeps = 0;
    while (!atomic_load(&g_press_stop)) {
        for (size_t i = 0; i < n; i += 8) buf[i] = buf[i] * 3u + 1u;
        sweeps++;
    }
    g_press_sweeps[pa->idx] = sweeps;
    free(buf);
    return NULL;
}

static void pressure_on(int timed) {
    atomic_store(&g_press_stop, 0);
    long n = sysconf(_SC_NPROCESSORS_CONF);
    g_press_n = 0;
    for (long c = n - 1; c >= 0 && g_press_n < PRESS_THREADS; c--) {
        if ((int)c == timed) continue;
        g_press_arg[g_press_n] = (PressArg){ (int)c, g_press_n };
        if (pthread_create(&g_press[g_press_n], NULL, press_main, &g_press_arg[g_press_n]) == 0)
            g_press_n++;
    }
    struct timespec ts = { 0, 50000000 };
    nanosleep(&ts, NULL);
}

static void pressure_off(void) {
    atomic_store(&g_press_stop, 1);
    for (int i = 0; i < g_press_n; i++) pthread_join(g_press[i], NULL);
    g_press_n = 0;
}

/* ---- synthesis and verification ---- */

static RxCostModel g_meta;   /* verify/synth/code/power figures, copied into every model */

static int map_code(const uint8_t *code, size_t len, MvFn *out) {
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) return -1;
    memcpy(p, code, len);
    __builtin___clear_cache((char *)p, (char *)p + len);
    if (mprotect(p, 4096, PROT_READ | PROT_EXEC) != 0) return -1;
    union { void *v; MvFn f; } u = { p };
    *out = u.f;
    return 0;
}

static const uint32_t VM[] = { 1, 2, 3, 4, 5, 7, 8, 16, 31, 64 };
static const uint32_t VN[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65,
                               255, 256, 257 };

/* Differential against the semantic reference, in the child. 0 pass, 1
 * wrong, 2 wrote outside y or changed A or x. */
static int differential(MvFn fn) {
    static uint64_t A[64 * 257], A0[64 * 257], x[257], x0[257], y[64 + GUARD], yr[64];
    const uint64_t CANARY = 0xC0FFEE0DDBA11AD5ull;
    int wrong = 0;
    for (unsigned mi = 0; mi < sizeof VM / sizeof VM[0]; mi++)
        for (unsigned ni = 0; ni < sizeof VN / sizeof VN[0]; ni++)
            for (int kind = 0; kind < 3; kind++) {
                uint32_t M = VM[mi], N = VN[ni];
                uint64_t s = 0x1234567ull + mi * 131u + ni * 7u + (uint64_t)kind;
                for (uint32_t i = 0; i < M * N; i++)
                    A[i] = kind == 0 ? xs(&s) : kind == 1 ? UINT64_MAX : (1ull << 63) + i;
                for (uint32_t i = 0; i < N; i++)
                    x[i] = kind == 0 ? xs(&s) : kind == 1 ? UINT64_MAX : (1ull << 63) - i;
                memcpy(A0, A, sizeof(uint64_t) * M * N);
                memcpy(x0, x, sizeof(uint64_t) * N);
                for (uint32_t i = 0; i < M + GUARD; i++) y[i] = CANARY;
                omega_matvec_reference(A, x, yr, M, N);
                fn(A, x, y, M, N);
                for (uint32_t i = M; i < M + GUARD; i++)
                    if (y[i] != CANARY) return 2;
                if (memcmp(A, A0, sizeof(uint64_t) * M * N) || memcmp(x, x0, sizeof(uint64_t) * N))
                    return 2;
                if (memcmp(y, yr, sizeof(uint64_t) * M)) wrong = 1;
            }
    return wrong;
}

/* Fork, run the differential with a deadline, report. */
static int verify_in_child(MvFn fn, uint64_t *ns) {
    uint64_t t0 = now_ns();
    pid_t pid = fork();
    if (pid == 0) {
        alarm(10);
        _exit(differential(fn));
    }
    int st = 0;
    if (pid < 0 || waitpid(pid, &st, 0) != pid) return -1;
    *ns = now_ns() - t0;
    if (!WIFEXITED(st)) return 3;
    return WEXITSTATUS(st);
}

static struct {
    int verdict[N_ARMS];
} V;

static void setup_arms(void) {
    OmegaMachineGraph mg;
    MatVecSemanticSpec spec;
    CHECK(omega_machine_build_dgx_spark(&mg) == 0, "machine graph");
    CHECK(omega_matvec_spec_init(&spec, "empirical_matvec", MAX_M, MAX_N) == 0, "spec");
    MatVecLivingKernel kernel;
    memset(&kernel, 0, sizeof kernel);
    kernel.spec = spec;
    kernel.machine = &mg;
    rx_cm_init(&g_meta, N_ARMS);
    g_fn[ARM_REF] = arm_reference;
    g_fn[ARM_PLANTED] = arm_planted;
    for (uint32_t a = ARM_SCALAR; a <= ARM_QUAD4; a++) {
        MatVecRealization r;
        uint64_t t0 = now_ns();
        int rc = a == ARM_QUAD4 ? omega_matvec_synthesize_quad4(&spec, &mg, &r)
                                : omega_matvec_synthesize(&kernel, (MatVecRealizationKind)(a - 1), &r);
        g_meta.synth_ns[a] = now_ns() - t0;
        CHECK(rc == 0, "synthesize %s", ARM_NAME[a]);
        g_meta.code_bytes[a] = r.realization.code_len;
        CHECK(map_code(r.realization.code_bytes, r.realization.code_len, &g_fn[a]) == 0, "map %s",
              ARM_NAME[a]);
    }
    for (uint32_t a = 0; a < N_ARMS; a++) {
        uint64_t ns = 0;
        V.verdict[a] = verify_in_child(g_fn[a], &ns);
        g_meta.verify_ns[a] = ns;
        g_verified[a] = V.verdict[a] == 0;
        if (g_verified[a]) g_eligible |= 1u << a;
        printf("    %-13s code %4llu B  synth %6llu ns  verify %8llu ns  %s\n", ARM_NAME[a],
               (unsigned long long)g_meta.code_bytes[a], (unsigned long long)g_meta.synth_ns[a],
               (unsigned long long)ns, g_verified[a] ? "verified" : "REFUSED");
    }
    for (uint32_t a = 0; a < ARM_PLANTED; a++) CHECK(g_verified[a], "%s verified", ARM_NAME[a]);
    CHECK(V.verdict[ARM_PLANTED] == 1, "planted arm refused as wrong (%d)", V.verdict[ARM_PLANTED]);
    CHECK(g_parent_runs[ARM_PLANTED] == 0, "planted arm never ran in the parent");
}

/* Power while an arm runs, by core class, from the cluster meter, minus the
 * cluster's idle draw. Each window is long enough for the ~0.1 s meter. */
static double g_idle_mw[RX_CM_CORES];

static double window_mw(const char *label, uint32_t arm, const Job *j, uint64_t ns_target) {
    uint64_t e0 = meter_uj(label), t0 = now_ns(), t = t0;
    if (arm == UINT32_MAX) {
        struct timespec ts = { (time_t)(ns_target / 1000000000ull), (long)(ns_target % 1000000000ull) };
        nanosleep(&ts, NULL);
        t = now_ns();
    } else {
        while (t - t0 < ns_target) {
            for (uint32_t c = 0; c < j->calls; c++) exec_arm(arm, g_A, g_x, g_y, j->M, j->N);
            t = now_ns();
        }
    }
    uint64_t e1 = meter_uj(label);
    if (!e0 || e1 <= e0) return 0.0;
    return (double)(e1 - e0) / ((double)(t - t0) / 1e9) / 1000.0;   /* uJ/s = uW -> mW */
}

static void measure_power(void) {
    Job j = { 256, 256, 0, 77, 0 };
    j.calls = calls_for(j.M, j.N);
    job_load(&j);
    for (uint32_t k = 0; k < RX_CM_CORES; k++) {
        CoreClass *cc = &g_class[k];
        if (!cc->n || !cc->meter || !g_meter_dir[0]) continue;
        pin(cc->timed);
        g_idle_mw[k] = window_mw(cc->meter, UINT32_MAX, &j, 500000000ull);
        for (uint32_t a = 0; a < N_ARMS; a++) {
            if (!g_verified[a]) continue;
            double mw = window_mw(cc->meter, a, &j, 500000000ull) - g_idle_mw[k];
            g_meta.power_mw[k][a] = mw > 1.0 ? mw : 1.0;
        }
        printf("    power on %s: idle %.0f mW; running above idle:", cc->name, g_idle_mw[k]);
        for (uint32_t a = 0; a < N_ARMS; a++)
            if (g_verified[a]) printf(" %s %.0f", ARM_NAME[a], g_meta.power_mw[k][a]);
        printf(" mW\n");
    }
}

static void model_fresh(RxCostModel *m) {
    rx_cm_init(m, N_ARMS);
    memcpy(m->power_mw, g_meta.power_mw, sizeof m->power_mw);
    memcpy(m->verify_ns, g_meta.verify_ns, sizeof m->verify_ns);
    memcpy(m->synth_ns, g_meta.synth_ns, sizeof m->synth_ns);
    memcpy(m->code_bytes, g_meta.code_bytes, sizeof m->code_bytes);
    /* Verification outcomes are observations too: the refusal counts as a failure. */
    RxCmFeatures f;
    memset(&f, 0, sizeof f);
    f.M = f.N = 1;
    for (uint32_t a = 0; a < N_ARMS; a++) {
        RxCmObservation o = { 1, V.verdict[a] != 0 };
        if (o.failed) rx_cm_observe(m, &f, a, &o);
    }
}

/* ---- policies ---- */

static uint32_t dispatch_rule(uint32_t N) {
    /* omega_matvec_dispatch's fixed rule, over the pre-mapped arms. */
    return N <= 8 ? ARM_U2 : ARM_U4D;
}

static RxCmFeatures features(const Job *j, uint32_t core, uint32_t pressure, uint32_t current) {
    RxCmFeatures f;
    memset(&f, 0, sizeof f);
    f.op = RX_CM_OP_MATVEC;
    f.M = j->M;
    f.N = j->N;
    f.state_bytes = ((uint64_t)j->M * j->N + j->M + j->N) * 8u;
    f.cache_bytes = g_class[core].cache_bytes;
    f.core = core;
    f.pressure = pressure;
    f.thermal_c = thermal_c();
    f.current_arm = current;
    f.eligible = g_eligible;
    f.need_evidence = 1;
    return f;
}

typedef struct {
    uint64_t measures, selects, fallbacks, touched_ineligible;
    uint64_t explore_ps;
    uint64_t max_extra_ps;      /* largest single exploration charge */
} LearnStats;

/* One job for an online learner: decide, run what it decided, learn from
 * those runs only. Returns the policy's cost in table terms (ps). */
static double learner_job(RxCostModel *m, const RxCmPolicy *p, const Job *j, uint32_t core,
                          uint32_t pressure, uint32_t *current, const uint64_t *table,
                          LearnStats *s) {
    RxCmFeatures f = features(j, core, pressure, *current);
    RxCmDecision d;
    rx_cm_decide(m, p, &f, 1, &d);
    if (!(g_eligible & (1u << d.arm)) ||
        (d.action == RX_CM_MEASURE &&
         (!(g_eligible & (1u << d.measure[0])) || !(g_eligible & (1u << d.measure[1])))))
        s->touched_ineligible++;
    if (d.action == RX_CM_MEASURE) {
        /* The job runs on the chosen arm; the competitor is probed on a
         * quarter of the calls. The probe is the exploration cost. */
        uint32_t a = d.measure[0], b = d.measure[1];
        uint32_t probe = j->calls / 4u < 3u ? 3u : j->calls / 4u;
        uint64_t ta = time_arm_chunked(a, j, j->calls), tb = time_arm_chunked(b, j, probe);
        RxCmObservation oa = { ta, ta == 0 }, ob = { tb, tb == 0 };
        rx_cm_observe(m, &f, a, &oa);
        rx_cm_observe(m, &f, b, &ob);
        rx_cm_charge(m, ta * j->calls, tb * probe);
        if (tb * probe > s->max_extra_ps) s->max_extra_ps = tb * probe;
        *current = ta <= tb ? a : b;
        s->measures++;
        s->explore_ps += table[b] * probe;
        return (double)table[a] * j->calls + (double)table[b] * probe;
    }
    uint64_t t = time_arm_chunked(d.arm, j, j->calls);
    RxCmObservation o = { t, t == 0 };
    rx_cm_observe(m, &f, d.arm, &o);
    rx_cm_charge(m, t * j->calls, 0);
    *current = d.arm;
    if (d.action == RX_CM_SELECT) s->selects++;
    else s->fallbacks++;
    return (double)table[d.arm] * j->calls;
}

static uint32_t frozen_choice(const RxCostModel *m, const RxCmPolicy *p, const Job *j,
                              uint32_t core, uint32_t pressure, RxCmDecision *out) {
    RxCmFeatures f = features(j, core, pressure, ARM_REF);
    rx_cm_decide(m, p, &f, 0, out);
    return out->arm;
}

/* ---- workloads ---- */

static const uint32_t TRAIN_M[] = { 1, 4, 16, 64, 256, 1024 };
static const uint32_t TRAIN_N[] = { 1, 2, 3, 5, 8, 12, 16, 33, 64, 100, 256, 512, 1024 };
static const uint32_t VAL_M[] = { 2, 8, 32, 128, 512 };
static const uint32_t VAL_N[] = { 4, 6, 10, 24, 48, 200, 700 };
#define NA(a) (sizeof(a) / sizeof((a)[0]))

static int in_set(uint32_t v, const uint32_t *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (s[i] == v) return 1;
    return 0;
}

#define MAX_JOBS 128

static uint32_t grid(Job *out, const uint32_t *Ms, size_t nm, const uint32_t *Ns, size_t nn,
                     uint64_t seed) {
    uint32_t n = 0;
    for (size_t i = 0; i < nm; i++)
        for (size_t k = 0; k < nn; k++) {
            if ((uint64_t)Ms[i] * Ns[k] > MAX_ELEMS || n >= MAX_JOBS) continue;
            out[n] = (Job){ Ms[i], Ns[k], calls_for(Ms[i], Ns[k]), seed + n * 7919u, 0 };
            n++;
        }
    return n;
}

static uint32_t log_uniform(uint64_t *s, double lo, double hi) {
    double u = (double)(xs(s) >> 11) / 9007199254740992.0;
    return (uint32_t)llround(exp2(lo + (hi - lo) * u));
}

static int seen(uint32_t M, uint32_t N) {
    return in_set(M, TRAIN_M, NA(TRAIN_M)) || in_set(M, VAL_M, NA(VAL_M)) ||
           in_set(N, TRAIN_N, NA(TRAIN_N)) || in_set(N, VAL_N, NA(VAL_N));
}

static uint32_t unseen_shapes(Job *out, uint32_t count, uint64_t seed) {
    uint64_t s = seed;
    uint32_t n = 0;
    while (n < count) {
        uint32_t M = log_uniform(&s, 1.5, 11.0), N = log_uniform(&s, 1.5, 12.0);
        if (seen(M, N) || (uint64_t)M * N > MAX_ELEMS || M > MAX_M || N > MAX_N) continue;
        out[n] = (Job){ M, N, calls_for(M, N), seed + n * 104729u, 0 };
        n++;
    }
    return n;
}

static uint32_t thin_mix(Job *out, uint32_t count, uint64_t seed) {
    uint64_t s = seed;
    uint32_t n = 0;
    while (n < count) {
        uint32_t M, N;
        if (xs(&s) % 10u < 7u) {
            N = 1u + (uint32_t)(xs(&s) % 7u);
            M = log_uniform(&s, 4.0, 12.5);
            if (in_set(M, TRAIN_M, NA(TRAIN_M)) || in_set(M, VAL_M, NA(VAL_M))) continue;
        } else {
            M = log_uniform(&s, 1.5, 11.0);
            N = log_uniform(&s, 1.5, 12.0);
            if (seen(M, N)) continue;
        }
        if ((uint64_t)M * N > MAX_ELEMS || M > MAX_M || N > MAX_N) continue;
        out[n] = (Job){ M, N, calls_for(M, N), seed + n * 15485863u, 0 };
        n++;
    }
    return n;
}

static void shuffle(Job *j, uint32_t n, uint64_t seed) {
    uint64_t s = seed;
    for (uint32_t i = n; i > 1; i--) {
        uint32_t k = (uint32_t)(xs(&s) % i);
        Job t = j[i - 1];
        j[i - 1] = j[k];
        j[k] = t;
    }
}

/* ---- receipt figures ---- */

enum { D_H1 = 0, D_H2, D_H3, N_DIST };
static const char *const DIST_NAME[N_DIST] = { "H1_unseen_shapes", "H2_thin_heavy_mix",
                                               "H3_memory_pressure" };
enum { P_DISPATCH = 0, P_REFERENCE, P_BEST_STATIC, P_FROZEN, P_ONLINE, P_ORACLE, N_POL };
static const char *const POL_NAME[N_POL] = { "fixed_dispatch_rule", "always_reference",
                                             "best_static_from_training", "learned_frozen",
                                             "learned_online", "oracle" };

typedef struct {
    uint32_t jobs;
    double cost[N_POL];             /* ps, table terms */
    uint64_t measures, fallbacks, explore_ps;
    uint32_t cov_n, cov_in80, cov_in95;
    double wall_ns[4], energy_uj[4], wait_ns[4];
    int contended_blocks_only;      /* no block ran without waiting for its core */   /* replay: dispatch, reference, static, frozen */
    int replayed;
    double contention_median;       /* H3 only: pressure / no pressure, same arm and shape */
} DistResult;

static struct {
    int present[RX_CM_CORES];
    DistResult d[RX_CM_CORES][N_DIST];
    double train_cost[RX_CM_CORES][N_ARMS];
    uint32_t best_static[RX_CM_CORES];
    uint32_t train_jobs[RX_CM_CORES], train_measures[RX_CM_CORES];
    double val_learned, val_dispatch, val_reference, val_poison;
    uint32_t val_jobs, val_cov_n, val_cov_in80;
    int proofs_ok;
    /* promotion */
    int learner_promote_rc, learner_with_promoter_cap_rc, poison_judge_ok, poison_promote_rc,
        promote_rc, recover_coherent, durable_digest_match, torn_rc, torn_fallback_ok,
        online_not_durable;
    uint64_t active_id;
    char model_digest[65];
    size_t model_bytes;
    uint64_t explore_budget_violations;
    uint64_t touched_ineligible;
    uint32_t thermal_start, thermal_end;
    double sd_scale;
    char load_start[48], load_end[48];
} R;

static void hex(const uint8_t *d, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) sprintf(out + i * 2, "%02x", d[i]);
}

/* ---- phases ---- */

static RxCmPolicy g_train_policy, g_online_policy, g_frozen_policy;

static void train_class(RxCostModel *m, uint32_t k) {
    Job jobs[MAX_JOBS];
    uint32_t n = grid(jobs, TRAIN_M, NA(TRAIN_M), TRAIN_N, NA(TRAIN_N), 1000u + k);
    LearnStats s;
    memset(&s, 0, sizeof s);
    uint32_t current = ARM_REF;
    for (int pass = 0; pass < 2; pass++) {
        shuffle(jobs, n, 99u + (uint64_t)pass + k);
        for (uint32_t i = 0; i < n; i++) {
            job_load(&jobs[i]);
            uint64_t table[N_ARMS];
            truth_table(&jobs[i], table);
            for (uint32_t a = 0; a < N_ARMS; a++)
                if (g_eligible & (1u << a)) R.train_cost[k][a] += (double)table[a] * jobs[i].calls;
            learner_job(m, &g_train_policy, &jobs[i], k, 0, &current, table, &s);
        }
    }
    R.train_jobs[k] = 2 * n;
    R.train_measures[k] = (uint32_t)s.measures;
    R.touched_ineligible += s.touched_ineligible;
    uint32_t best = ARM_REF;
    for (uint32_t a = 0; a < N_ARMS; a++)
        if ((g_eligible & (1u << a)) && R.train_cost[k][a] < R.train_cost[k][best]) best = a;
    R.best_static[k] = best;
    printf("    trained on %s: %u jobs, measured twice in %llu of them; best single arm on "
           "training: %s\n",
           g_class[k].name, 2 * n, (unsigned long long)s.measures, ARM_NAME[best]);
}

static void coverage(const RxCostModel *m, const Job *j, uint32_t core, uint32_t pressure,
                     const uint64_t *table, uint32_t *n, uint32_t *in80, uint32_t *in95) {
    RxCmFeatures f = features(j, core, pressure, ARM_REF);
    for (uint32_t a = 0; a < N_ARMS; a++) {
        if (!(g_eligible & (1u << a)) || table[a] == UINT64_MAX) continue;
        RxCmPrediction p;
        rx_cm_predict(m, &f, a, &p);
        if (!p.n) continue;
        double z = fabs(log2((double)table[a]) - p.mean_log2_ps);
        (*n)++;
        *in80 += z <= rx_cm_t_quantile(0.80, p.dof) * p.sd_log2;
        if (in95) *in95 += z <= rx_cm_t_quantile(0.95, p.dof) * p.sd_log2;
    }
}

/* The judge: evaluates a candidate model on validation work it measures
 * itself. Returns the candidate's cost; also the fixed rule's. */
typedef struct {
    Job jobs[RX_CM_CORES][MAX_JOBS];
    uint64_t table[RX_CM_CORES][MAX_JOBS][N_ARMS];
    uint32_t n[RX_CM_CORES];
} ValSet;

static ValSet g_val;

static void build_validation(void) {
    for (uint32_t k = 0; k < RX_CM_CORES; k++) {
        if (!R.present[k]) continue;
        place_quiet(k);
        g_val.n[k] = grid(g_val.jobs[k], VAL_M, NA(VAL_M), VAL_N, NA(VAL_N), 5000u + k);
        for (uint32_t i = 0; i < g_val.n[k]; i++) {
            job_load(&g_val.jobs[k][i]);
            truth_table(&g_val.jobs[k][i], g_val.table[k][i]);
        }
    }
}

static void judge(const RxCostModel *m, double *learned, double *dispatch, double *reference,
                  uint32_t *cov_n, uint32_t *cov_in80) {
    *learned = *dispatch = *reference = 0;
    for (uint32_t k = 0; k < RX_CM_CORES; k++)
        for (uint32_t i = 0; i < g_val.n[k]; i++) {
            if (i % 2u == 0) continue;   /* even jobs are the calibration split */
            const Job *j = &g_val.jobs[k][i];
            const uint64_t *t = g_val.table[k][i];
            RxCmDecision d;
            uint32_t arm = frozen_choice(m, &g_frozen_policy, j, k, 0, &d);
            *learned += (double)t[arm] * j->calls;
            *dispatch += (double)t[dispatch_rule(j->N)] * j->calls;
            *reference += (double)t[ARM_REF] * j->calls;
            if (cov_n) coverage(m, j, k, 0, t, cov_n, cov_in80, NULL);
        }
}

/* Calibration split: the even validation jobs, handed to the learner. The
 * judge grades on the odd ones only. */
static double calibrate_learned(RxCostModel *m) {
    static RxCmFeatures cf[RX_CM_CORES * MAX_JOBS * N_ARMS];
    static uint32_t ca[RX_CM_CORES * MAX_JOBS * N_ARMS];
    static uint64_t cps[RX_CM_CORES * MAX_JOBS * N_ARMS];
    uint32_t n = 0;
    for (uint32_t k = 0; k < RX_CM_CORES; k++)
        for (uint32_t i = 0; i < g_val.n[k]; i += 2)
            for (uint32_t a = 0; a < N_ARMS; a++) {
                if (!(g_eligible & (1u << a)) || g_val.table[k][i][a] == UINT64_MAX) continue;
                cf[n] = features(&g_val.jobs[k][i], k, 0, ARM_REF);
                ca[n] = a;
                cps[n] = g_val.table[k][i][a];
                n++;
            }
    return rx_cm_calibrate(m, cf, ca, cps, n, 0.80);
}

/* The judge accepts a candidate only if it beats the fixed rule by 5%, and
 * is no worse than the known-good fallback (always the reference) and no
 * worse than the model in force, if there is one. Beating a bad rule is not
 * enough: a model with its knowledge scrambled can still do that. */
static int judge_accepts(double learned, double dispatch, double reference, double incumbent) {
    return learned <= 0.95 * dispatch && learned <= reference && (incumbent <= 0 || learned <= incumbent);
}

/* ---- promotion through the R9 generation barrier ---- */

enum { ISSUER = 3, SUBJ_LEARNER = 71, SUBJ_PROMOTER = 72 };

typedef struct {
    AienosCapAdmin *admin;
    AienosCapView *view;
} Auth;

static int native_auth(void *ctx, uint32_t cap_id, uint64_t cap_generation, uint32_t subject,
                       uint64_t resource, uint32_t rights) {
    Auth *a = ctx;
    AienosCapRef ref = { cap_id, cap_generation };
    AienosCapEntry e;
    return aienos_cap_validate(a->view, ref, subject, resource, rights, &e);
}

static AienosCapRef mint(Auth *a, uint32_t subject, uint32_t rights) {
    AienosCapRef office, out = { UINT32_MAX, 0 };
    aienos_cap_office(a->admin, &office);
    AienosCapMint m = { ISSUER, subject, RX_GEN_RES_PROMOTION, rights, 0, { UINT32_MAX, 0 }, office };
    if (aienos_cap_mint(a->admin, &m, &out) != 0) out = (AienosCapRef){ UINT32_MAX, 0 };
    return out;
}

static RxPromotionRequest request(uint64_t id, uint32_t subject, AienosCapRef cap) {
    RxPromotionRequest r;
    memset(&r, 0, sizeof r);
    r.candidate_id = id;
    r.subject = subject;
    r.cap_id = cap.cap_id;
    r.cap_generation = cap.generation;
    r.resource = RX_GEN_RES_PROMOTION;
    r.rights = RX_GEN_RIGHT_PROMOTE;
    return r;
}

static uint8_t g_blob[65536], g_blob2[65536];
static char g_store_dir[512];

static int propose(RxGenStore *st, const RxCostModel *m, int proofs_ok, const char *evidence,
                   uint64_t *id) {
    size_t n = 0;
    if (rx_cm_serialize(m, g_blob, sizeof g_blob, &n) != RX_CM_OK) return -1;
    static const uint8_t config[] = "rx_costmodel v1; ei_min 0.08 log2; min_obs 12; unseen shift 1.0 log2; explore 5% + 0.5 ms";
    static const uint8_t provenance[] = "OMEGA_EMPIRICAL_OPTIMIZER host run";
    char real[256];
    int rl = snprintf(real, sizeof real, "arms: reference scalar unroll2 unroll4_dual quad4 "
                      "planted_wrong; eligible mask 0x%x", g_eligible);
    RxGenDraft d;
    memset(&d, 0, sizeof d);
    d.authority_epoch = 1;
    d.authority_generation = 1;
    d.proofs_ok = proofs_ok;
    d.evidence = (const uint8_t *)evidence;
    d.evidence_len = strlen(evidence);
    d.model = g_blob;
    d.model_len = n;
    d.realization = (const uint8_t *)real;
    d.realization_len = (size_t)rl;
    d.config = config;
    d.config_len = sizeof config - 1;
    d.provenance = provenance;
    d.provenance_len = sizeof provenance - 1;
    return rx_gen_propose(st, SUBJ_LEARNER, &d, id);
}

/* Load the durable model, or report why not. */
static int load_durable(const char *dir, RxCostModel *m, uint64_t *active) {
    RxGenStore *st = NULL;
    if (rx_gen_open(dir, &st) != RX_GEN_OK) return RX_GEN_ERR_IO;
    uint64_t id = 0, lineage = 0;
    int rc = rx_gen_active(st, &id, &lineage);
    size_t n = 0;
    if (rc == RX_GEN_OK) rc = rx_gen_read_blob(st, id, "model", g_blob2, sizeof g_blob2, &n);
    rx_gen_close(st);
    if (rc != RX_GEN_OK) return rc;
    if (rx_cm_deserialize(m, g_blob2, n) != RX_CM_OK) return RX_GEN_ERR_TORN;
    if (active) *active = id;
    return RX_GEN_OK;
}

static void poison(RxCostModel *p, const RxCostModel *m) {
    /* Swap what the model learned about quad4 and scalar in every cell. */
    *p = *m;
    for (uint32_t c = 0; c < RX_CM_CELLS; c++) {
        RxCmArmStats t = p->cell[c][ARM_QUAD4];
        p->cell[c][ARM_QUAD4] = p->cell[c][ARM_SCALAR];
        p->cell[c][ARM_SCALAR] = t;
    }
}

static int promote_phase(const RxCostModel *m, RxCostModel *durable) {
    Auth a;
    memset(&a, 0, sizeof a);
    CHECK(aienos_cap_start(&a.admin, &a.view) == 0, "native authority");
    AienosCapRef learner_cap = mint(&a, SUBJ_LEARNER, RX_RIGHT_READ);
    AienosCapRef promoter_cap = mint(&a, SUBJ_PROMOTER, RX_GEN_RIGHT_PROMOTE);
    CHECK(learner_cap.cap_id != UINT32_MAX && promoter_cap.cap_id != UINT32_MAX, "mint");

    if (omega_evidence_path("EMPIRICAL/generations/.keep", g_store_dir, sizeof g_store_dir) == 0) {
        char *slash = strrchr(g_store_dir, '/');
        if (slash) *slash = 0;
    }
    mkdir(g_store_dir, 0755);
    RxGenStore *st = NULL;
    CHECK(rx_gen_open(g_store_dir, &st) == RX_GEN_OK, "open generation store %s", g_store_dir);
    if (!st) return -1;
    uint64_t active0 = 0, lin0 = 0;
    rx_gen_active(st, &active0, &lin0);

    /* Poisoned candidate: its maker claims proofs; the judge disagrees. */
    RxCostModel bad;
    poison(&bad, m);
    double bl, bd, br;
    judge(&bad, &bl, &bd, &br, NULL, NULL);
    R.val_poison = bl;
    R.poison_judge_ok = judge_accepts(bl, bd, br, 0);
    CHECK(!R.poison_judge_ok, "judge rejects the poisoned model (%.3g; rule %.3g, reference %.3g)", bl, bd, br);
    uint64_t bad_id = 0;
    CHECK(propose(st, &bad, 0, "{\"validation\":\"lost to the fixed rule\"}", &bad_id) == RX_GEN_OK,
          "propose poisoned");
    RxPromotionRequest rq = request(bad_id, SUBJ_PROMOTER, promoter_cap);
    R.poison_promote_rc = rx_gen_promote(st, &rq, native_auth, &a, NULL, NULL, NULL, NULL);
    CHECK(R.poison_promote_rc == RX_GEN_ERR_VERIFY, "poisoned candidate without proofs refused (%d)",
          R.poison_promote_rc);

    char ev[512];
    snprintf(ev, sizeof ev,
             "{\"validation_jobs\":%u,\"learned_ps\":%.0f,\"fixed_rule_ps\":%.0f,"
             "\"mismatches\":%llu,\"coverage80\":%.3f}",
             R.val_jobs, R.val_learned, R.val_dispatch, (unsigned long long)K.mismatches,
             R.val_cov_n ? (double)R.val_cov_in80 / R.val_cov_n : 0.0);
    uint64_t id = 0;
    CHECK(propose(st, m, R.proofs_ok, ev, &id) == RX_GEN_OK, "propose candidate model");

    /* The learner may not promote: not with its own capability, and not
     * with the promoter's capability under its own name. */
    rq = request(id, SUBJ_LEARNER, learner_cap);
    R.learner_promote_rc = rx_gen_promote(st, &rq, native_auth, &a, NULL, NULL, NULL, NULL);
    CHECK(R.learner_promote_rc == RX_GEN_ERR_AUTHORITY, "learner promotion refused (%d)",
          R.learner_promote_rc);
    rq = request(id, SUBJ_LEARNER, promoter_cap);
    R.learner_with_promoter_cap_rc = rx_gen_promote(st, &rq, native_auth, &a, NULL, NULL, NULL, NULL);
    CHECK(R.learner_with_promoter_cap_rc == RX_GEN_ERR_AUTHORITY,
          "learner presenting the promoter's capability refused (%d)",
          R.learner_with_promoter_cap_rc);
    uint64_t act = 0, lin = 0;
    CHECK(rx_gen_active(st, &act, &lin) == RX_GEN_OK && act == active0, "refusals left the active generation as it was");

    rq = request(id, SUBJ_PROMOTER, promoter_cap);
    R.promote_rc = rx_gen_promote(st, &rq, native_auth, &a, NULL, NULL, NULL, NULL);
    if (!R.proofs_ok) {
        /* The judge rejected the learned model on this machine. Where the best
         * static arm is the reference (a CI runner with one core class), the
         * learned model can at best tie it on validation and timing noise
         * decides. That is a no-win result, not a broken barrier: the barrier
         * must refuse the candidate and the generation in force stays. The
         * gate then reports no win (main). Recovery and the torn copy need a
         * promoted generation and are not run; the receipt records why. */
        CHECK(R.promote_rc == RX_GEN_ERR_VERIFY, "barrier refuses the model the judge rejected (%d)",
              R.promote_rc);
        CHECK(rx_gen_active(st, &act, &lin) == RX_GEN_OK && act == active0,
              "the refused promotion left the active generation as it was");
        rx_gen_close(st);
        aienos_cap_stop(a.admin, a.view);
        printf("    judge rejected the learned model (learned %.4f of always-reference): nothing promoted\n",
               R.val_reference > 0 ? R.val_learned / R.val_reference : 0.0);
        model_fresh(durable);
        return 0;
    }
    CHECK(R.promote_rc == RX_GEN_OK, "promoter promotes the judged model (%d)", R.promote_rc);
    rx_gen_close(st);
    aienos_cap_stop(a.admin, a.view);

    /* Restart: recover, reopen, read the model back. */
    RxRecoveryRecord rec;
    memset(&rec, 0, sizeof rec);
    CHECK(rx_gen_recover(g_store_dir, &rec) == RX_GEN_OK, "recover");
    R.recover_coherent = rec.coherent && rec.active_id == id;
    CHECK(R.recover_coherent, "recovery selects the promoted generation");
    uint64_t active = 0;
    CHECK(load_durable(g_store_dir, durable, &active) == RX_GEN_OK && active == id,
          "durable model read back");
    R.active_id = active;
    uint8_t d1[32], d2[32];
    rx_cm_digest(m, d1);
    rx_cm_digest(durable, d2);
    R.durable_digest_match = memcmp(d1, d2, 32) == 0;
    CHECK(R.durable_digest_match, "durable model is the judged model");
    hex(d2, 32, R.model_digest);
    R.model_bytes = rx_cm_blob_size();

    /* A torn copy: the store refuses the blob and Omega runs the known-good
     * fallback instead of anything the torn model says. */
    char torn[600], cmd[2048];
    snprintf(torn, sizeof torn, "%s-torn", g_store_dir);
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && cp -r '%s' '%s'", torn, g_store_dir, torn);
    CHECK(system(cmd) == 0, "copy store");
    char mp[700];
    snprintf(mp, sizeof mp, "%s/g/%llu/model", torn, (unsigned long long)id);
    FILE *fp = fopen(mp, "r+b");
    CHECK(fp != NULL, "open torn model %s", mp);
    if (fp) {
        fseek(fp, 200, SEEK_SET);
        int c = fgetc(fp);
        fseek(fp, 200, SEEK_SET);
        fputc(c ^ 0x5a, fp);
        fclose(fp);
    }
    RxCostModel tm;
    R.torn_rc = load_durable(torn, &tm, NULL);
    CHECK(R.torn_rc == RX_GEN_ERR_TORN, "torn model refused (%d)", R.torn_rc);
    RxCostModel empty;
    model_fresh(&empty);
    Job j = { 64, 256, 1, 1, 0 };
    RxCmDecision d;
    uint32_t arm = frozen_choice(&empty, &g_frozen_policy, &j, RX_CM_CORE_X925, 0, &d);
    R.torn_fallback_ok = d.action == RX_CM_FALLBACK && arm == ARM_REF;
    CHECK(R.torn_fallback_ok, "without a durable model, the known-good fallback runs");
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", torn);
    if (system(cmd) != 0) fprintf(stderr, "  could not remove %s\n", torn);
    return 0;
}

/* ---- held-out ---- */

static Job g_h1[MAX_JOBS], g_h2[MAX_JOBS];
static uint64_t g_h1_table[RX_CM_CORES][MAX_JOBS][N_ARMS];
static uint32_t g_choice[RX_CM_CORES][2][4][MAX_JOBS];   /* [dist H1/H2][policy] */

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void heldout(const RxCostModel *durable, uint32_t k, uint32_t dist, Job *jobs, uint32_t n,
                    RxCostModel *online) {
    DistResult *D = &R.d[k][dist];
    uint32_t pressure = dist == D_H3 ? 1u : 0u;
    LearnStats s;
    memset(&s, 0, sizeof s);
    uint32_t current = ARM_REF;
    uint64_t explore_before = online->explore_ps, work_before = online->work_ps;
    double contention[MAX_JOBS];
    uint32_t n_cont = 0;
    if (pressure) pressure_on(g_class[k].timed);
    for (uint32_t i = 0; i < n; i++) {
        Job *j = &jobs[i];
        job_load(j);
        uint64_t table[N_ARMS];
        truth_table(j, table);
        if (dist == D_H1) memcpy(g_h1_table[k][i], table, sizeof table);
        RxCmDecision d;
        uint32_t fz = frozen_choice(durable, &g_frozen_policy, j, k, pressure, &d);
        if (!(g_eligible & (1u << fz))) s.touched_ineligible++;
        uint32_t oracle = ARM_REF;
        for (uint32_t a = 0; a < N_ARMS; a++)
            if ((g_eligible & (1u << a)) && table[a] < table[oracle]) oracle = a;
        uint32_t choice[4] = { dispatch_rule(j->N), ARM_REF, R.best_static[k], fz };
        for (uint32_t p = 0; p < 4; p++) D->cost[p] += (double)table[choice[p]] * j->calls;
        D->cost[P_ORACLE] += (double)table[oracle] * j->calls;
        D->cost[P_ONLINE] += learner_job(online, &g_online_policy, j, k, pressure, &current, table, &s);
        if (dist < D_H3)
            for (uint32_t p = 0; p < 4; p++) g_choice[k][dist][p][i] = choice[p];
        coverage(durable, j, k, pressure, table, &D->cov_n, &D->cov_in80, &D->cov_in95);
        if (dist == D_H3 && n_cont < MAX_JOBS)   /* same job as H1 i: same shape, same data */
            contention[n_cont++] = (double)table[oracle] / (double)g_h1_table[k][i][oracle];
    }
    if (pressure) pressure_off();
    D->jobs = n;
    D->measures = s.measures;
    D->fallbacks = s.fallbacks;
    D->explore_ps = s.explore_ps;
    R.touched_ineligible += s.touched_ineligible;
    (void)explore_before;
    (void)work_before;
    /* The rule the learner spends by: a measurement starts only while spent
     * exploration is under explore_frac of chosen work plus the allowance, so
     * the total can exceed that line by at most one measurement. */
    double line = g_online_policy.explore_frac * (double)online->work_ps +
                  (double)g_online_policy.explore_allow_ps;
    if ((double)online->explore_ps > line + (double)s.max_extra_ps) R.explore_budget_violations++;
    if (n_cont) {
        qsort(contention, n_cont, sizeof(double), cmp_d);
        D->contention_median = contention[n_cont / 2];
    }
}

/* Nanoseconds this thread has spent runnable but waiting for its cpu: time
 * another task held the core. */
static uint64_t run_delay_ns(void) {
    FILE *fp = fopen("/proc/thread-self/schedstat", "r");
    unsigned long long run = 0, wait = 0;
    if (fp) {
        if (fscanf(fp, "%llu %llu", &run, &wait) != 2) wait = 0;
        fclose(fp);
    }
    return wait;
}

/* Replay each policy's choices end to end, alternating blocks, with the
 * cluster meter. Policies: dispatch, reference, best static, learned frozen. */
static void replay(uint32_t k, uint32_t dist, Job *jobs, uint32_t n) {
    DistResult *D = &R.d[k][dist];
    const char *label = g_class[k].meter;
    /* Correctness pass: every policy's choice on every job, real data. */
    for (uint32_t p = 0; p < 4; p++)
        for (uint32_t i = 0; i < n; i++) {
            job_load(&jobs[i]);
            exec_arm(g_choice[k][dist][p][i], g_A, g_x, g_y, jobs[i].M, jobs[i].N);
            K.runs_checked++;
            if (digest_y(g_y, jobs[i].M) != jobs[i].ref_digest) K.mismatches++;
        }
    /* Timed and metered blocks. Values do not change the work, so the
     * buffers are left as they are and nothing but the realizations runs. */
    uint64_t t0 = now_ns();
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t c = 0; c < jobs[i].calls; c++)
            exec_arm(g_choice[k][dist][0][i], g_A, g_x, g_y, jobs[i].M, jobs[i].N);
    uint64_t once = now_ns() - t0;
    uint32_t reps = (uint32_t)(600000000ull / (once ? once : 1)) + 1u;
    if (reps > 400) reps = 400;
    enum { ROUNDS = 7 };
    double wall[4][ROUNDS], en[4][ROUNDS], wait[4][ROUNDS];
    for (uint32_t round = 0; round < ROUNDS; round++)
        for (uint32_t q = 0; q < 4; q++) {
            uint32_t p = (q + round) % 4;
            uint64_t e0 = meter_uj(label), w0 = now_ns(), q0 = run_delay_ns();
            for (uint32_t r = 0; r < reps; r++)
                for (uint32_t i = 0; i < n; i++)
                    for (uint32_t c = 0; c < jobs[i].calls; c++)
                        exec_arm(g_choice[k][dist][p][i], g_A, g_x, g_y, jobs[i].M, jobs[i].N);
            uint64_t w1 = now_ns(), e1 = meter_uj(label), q1 = run_delay_ns();
            wall[p][round] = (double)(w1 - w0);
            wait[p][round] = (double)(q1 - q0);
            en[p][round] = e1 > e0 ? (double)(e1 - e0) : 0.0;
            if (getenv("EMPIRICAL_DEBUG"))
                fprintf(stderr, "replay core%u dist%u round%u policy%u reps %u wall %.1f ms waited %.2f ms energy %.0f uJ\n", k,
                        dist, round, p, reps, wall[p][round] / 1e6, wait[p][round] / 1e6, en[p][round]);
        }
    /* The least disturbed block per policy: among blocks where the thread
     * waited for its core under 1% of the block, the fastest; if none, the
     * fastest of all. Energy is the meter reading of that same block. */
    for (uint32_t p = 0; p < 4; p++) {
        int b = -1;
        for (uint32_t r = 0; r < ROUNDS; r++)
            if (wait[p][r] < 0.01 * wall[p][r] && (b < 0 || wall[p][r] < wall[p][b])) b = (int)r;
        if (b < 0) {
            b = 0;
            for (uint32_t r = 1; r < ROUNDS; r++)
                if (wall[p][r] < wall[p][b]) b = (int)r;
            D->contended_blocks_only = 1;
        }
        D->wall_ns[p] = wall[p][b];
        D->energy_uj[p] = en[p][b];
        D->wait_ns[p] = wait[p][b];
    }
    D->replayed = 1;
}

/* ---- receipt ---- */

static void binary_digest(char out[65]) {
    strcpy(out, "unavailable");
    FILE *fp = fopen("/proc/self/exe", "rb");
    if (!fp) return;
    sha256_ctx c;
    sha256_init(&c);
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, fp)) > 0) sha256_update(&c, buf, n);
    fclose(fp);
    uint8_t d[32];
    sha256_final(&c, d);
    hex(d, 32, out);
}

typedef struct {
    int win;
    char why[512];
} Verdict;

static Verdict judge_gate(void) {
    Verdict v = { 1, "" };
    double frozen_all = 0, static_all = 0;
    for (uint32_t k = 0; k < RX_CM_CORES; k++) {
        if (!R.present[k]) continue;
        for (uint32_t d = 0; d < N_DIST; d++) {
            DistResult *D = &R.d[k][d];
            frozen_all += D->cost[P_FROZEN];
            static_all += D->cost[P_BEST_STATIC];
            const char *fail = NULL;
            if (D->cost[P_FROZEN] > 0.95 * D->cost[P_DISPATCH]) fail = "frozen not 5% under the fixed rule";
            else if (D->cost[P_ONLINE] > 0.95 * D->cost[P_DISPATCH]) fail = "online not 5% under the fixed rule";
            else if (D->cost[P_FROZEN] > 1.03 * D->cost[P_BEST_STATIC]) fail = "frozen >3% over best static";
            else if (D->replayed && D->wall_ns[3] >= D->wall_ns[0]) fail = "replay: frozen not faster than the fixed rule";
            /* Coverage is gated where the model has seen the condition. Under H3
             * (never trained on) it rests on the unseen-shift prior; reported. */
            else if (d != D_H3 && D->cov_n && (double)D->cov_in80 / D->cov_n < 0.6)
                fail = "80% intervals cover < 60%";
            if (fail && v.win) {
                v.win = 0;
                snprintf(v.why, sizeof v.why, "%s, %s: %s", g_class[k].name, DIST_NAME[d], fail);
            }
        }
    }
    if (v.win && frozen_all >= static_all) {
        v.win = 0;
        snprintf(v.why, sizeof v.why, "frozen not under best static over all held-out work");
    }
    return v;
}

static void write_receipt(Verdict v, int allow_no_win) {
    char path[512];
    if (omega_evidence_path("EMPIRICAL/rx_empirical_optimizer_receipt.json", path, sizeof path) != 0)
        return;
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    char commit[41];
    memset(commit, 0, sizeof commit);
    if (!omega_evidence_run_commit(commit)) memcpy(commit, "unknown", 8);
    const char *candidate = getenv("OMEGA_CANDIDATE_COMMIT");
    int bound = candidate && candidate[0] && strcmp(candidate, commit) == 0 &&
                !omega_evidence_tree_dirty();
    const char *aienos = getenv("AIENOS_COMMIT");
    char digest[65];
    binary_digest(digest);
    struct utsname u;
    memset(&u, 0, sizeof u);
    uname(&u);
    int core_ok = g_fail == 0 && K.mismatches == 0 && g_parent_runs[ARM_PLANTED] == 0 &&
                  R.touched_ineligible == 0 && R.explore_budget_violations == 0;
    const char *gate = !core_ok ? "FAIL" : v.win ? "PASS" : allow_no_win ? "CHAIN_PASS_NO_WIN" : "FAIL";

    fprintf(fp, "{\n  \"schema\": \"OMEGA_EMPIRICAL_OPTIMIZER_V1\",\n");
    fprintf(fp, "  \"run_id\": \"%s\",\n", omega_evidence_run_id());
    fprintf(fp, "  \"candidate_commit\": %s%s%s,\n", candidate ? "\"" : "",
            candidate ? candidate : "null", candidate ? "\"" : "");
    fprintf(fp, "  \"candidate_bound\": %s,\n", bound ? "true" : "false");
    fprintf(fp, "  \"run_commit\": \"%s\",\n  \"tree_dirty\": %s,\n", commit,
            omega_evidence_tree_dirty() ? "true" : "false");
    fprintf(fp, "  \"aienos_commit\": %s%s%s,\n", aienos ? "\"" : "", aienos ? aienos : "null",
            aienos ? "\"" : "");
    fprintf(fp, "  \"checks\": %d,\n  \"failures\": %d,\n", g_checks, g_fail);
    fprintf(fp, "  \"test_binary_sha256\": \"%s\",\n", digest);
    fprintf(fp, "  \"host\": {\"sysname\": \"%s\", \"release\": \"%s\", \"machine\": \"%s\", "
                "\"cpus\": \"%s\", \"timed_cpus_by_phase\": \"%s\", \"loadavg_start\": \"%s\", \"loadavg_end\": \"%s\", "
                "\"thermal_c_start\": %u, \"thermal_c_end\": %u, \"shared_machine\": \"other sessions ran tests during this run; costs are floors (best of 5, fastest chunk, fastest replay block)\"},\n",
            u.sysname, u.release, u.machine, g_cpus_desc, g_timed_log, R.load_start, R.load_end,
            R.thermal_start, R.thermal_end);
    fprintf(fp, "  \"arms\": [");
    for (uint32_t a = 0; a < N_ARMS; a++)
        fprintf(fp, "%s\n    {\"name\": \"%s\", \"verified\": %s, \"code_bytes\": %llu, "
                    "\"synth_ns\": %llu, \"verify_ns\": %llu, \"parent_runs\": %llu}",
                a ? "," : "", ARM_NAME[a], g_verified[a] ? "true" : "false",
                (unsigned long long)g_meta.code_bytes[a], (unsigned long long)g_meta.synth_ns[a],
                (unsigned long long)g_meta.verify_ns[a], (unsigned long long)g_parent_runs[a]);
    fprintf(fp, "\n  ],\n");
    fprintf(fp, "  \"correctness\": {\"runs_checked_against_reference\": %llu, \"mismatches\": %llu, "
                "\"refused_runs_of_unverified_arms\": %llu, \"ineligible_arm_chosen_or_measured\": %llu},\n",
            (unsigned long long)K.runs_checked, (unsigned long long)K.mismatches,
            (unsigned long long)g_refused_runs, (unsigned long long)R.touched_ineligible);
    fprintf(fp, "  \"power_mw_above_idle\": {");
    int first = 1;
    for (uint32_t k = 0; k < RX_CM_CORES; k++) {
        if (!R.present[k] || !g_class[k].meter || !g_meter_dir[0]) continue;
        fprintf(fp, "%s\"%s\": {\"idle_cluster_mw\": %.0f", first ? "" : ", ", g_class[k].name,
                g_idle_mw[k]);
        for (uint32_t a = 0; a < N_ARMS; a++)
            if (g_verified[a]) fprintf(fp, ", \"%s\": %.0f", ARM_NAME[a], g_meta.power_mw[k][a]);
        fprintf(fp, "}");
        first = 0;
    }
    fprintf(fp, "},\n");
    fprintf(fp, "  \"training\": {");
    first = 1;
    for (uint32_t k = 0; k < RX_CM_CORES; k++) {
        if (!R.present[k]) continue;
        fprintf(fp, "%s\"%s\": {\"jobs\": %u, \"jobs_measured_twice\": %u, \"best_static_arm\": \"%s\"}",
                first ? "" : ", ", g_class[k].name, R.train_jobs[k], R.train_measures[k],
                ARM_NAME[R.best_static[k]]);
        first = 0;
    }
    fprintf(fp, "},\n");
    fprintf(fp, "  \"validation\": {\"jobs\": %u, \"learned_ps\": %.0f, \"fixed_rule_ps\": %.0f, "
                "\"always_reference_ps\": %.0f, \"poisoned_model_ps\": %.0f, \"coverage80\": %.3f, \"calibration_sd_scale\": %.3f, \"proofs_ok\": %s},\n",
            R.val_jobs, R.val_learned, R.val_dispatch, R.val_reference, R.val_poison,
            R.val_cov_n ? (double)R.val_cov_in80 / R.val_cov_n : 0.0, R.sd_scale, R.proofs_ok ? "true" : "false");
    fprintf(fp, "  \"promotion\": {\"model_bytes\": %zu, \"model_sha256\": \"%s\", \"active_generation\": %llu, "
                "\"learner_promote_rc\": %d, \"learner_with_promoter_cap_rc\": %d, "
                "\"poisoned_judged_better\": %s, \"poisoned_promote_rc\": %d, \"promote_rc\": %d, "
                "\"recovery_selects_promoted\": %s, \"durable_equals_judged\": %s, "
                "\"torn_blob_rc\": %d, \"torn_falls_back_to_reference\": %s, "
                "\"online_updates_not_durable\": %s},\n",
            R.model_bytes, R.model_digest, (unsigned long long)R.active_id, R.learner_promote_rc,
            R.learner_with_promoter_cap_rc, R.poison_judge_ok ? "true" : "false",
            R.poison_promote_rc, R.promote_rc, R.recover_coherent ? "true" : "false",
            R.durable_digest_match ? "true" : "false", R.torn_rc,
            R.torn_fallback_ok ? "true" : "false", R.online_not_durable ? "true" : "false");
    fprintf(fp, "  \"exploration\": {\"online_explore_frac\": %.2f, \"budget_violations\": %llu},\n",
            g_online_policy.explore_frac, (unsigned long long)R.explore_budget_violations);
    fprintf(fp, "  \"heldout\": {");
    first = 1;
    for (uint32_t k = 0; k < RX_CM_CORES; k++) {
        if (!R.present[k]) continue;
        fprintf(fp, "%s\n    \"%s\": {", first ? "" : ",", g_class[k].name);
        first = 0;
        for (uint32_t d = 0; d < N_DIST; d++) {
            DistResult *D = &R.d[k][d];
            fprintf(fp, "%s\n      \"%s\": {\"jobs\": %u, \"cost_ps\": {", d ? "," : "", DIST_NAME[d],
                    D->jobs);
            for (uint32_t p = 0; p < N_POL; p++)
                fprintf(fp, "%s\"%s\": %.0f", p ? ", " : "", POL_NAME[p], D->cost[p]);
            fprintf(fp, "},\n        \"frozen_vs_fixed_rule\": %.3f, \"online_vs_fixed_rule\": %.3f, "
                        "\"frozen_vs_best_static\": %.3f, \"frozen_vs_oracle\": %.3f,\n"
                        "        \"online_measure_jobs\": %llu, \"online_explore_ps\": %llu, "
                        "\"coverage80\": %.3f, \"coverage95\": %.3f",
                    D->cost[P_FROZEN] / D->cost[P_DISPATCH], D->cost[P_ONLINE] / D->cost[P_DISPATCH],
                    D->cost[P_FROZEN] / D->cost[P_BEST_STATIC], D->cost[P_FROZEN] / D->cost[P_ORACLE],
                    (unsigned long long)D->measures, (unsigned long long)D->explore_ps,
                    D->cov_n ? (double)D->cov_in80 / D->cov_n : 0.0,
                    D->cov_n ? (double)D->cov_in95 / D->cov_n : 0.0);
            if (d == D_H3) fprintf(fp, ", \"contention_median\": %.3f", D->contention_median);
            if (D->replayed)
                fprintf(fp, ",\n        \"replay\": {\"wall_ns\": {\"fixed_rule\": %.0f, \"reference\": %.0f, "
                            "\"best_static\": %.0f, \"learned_frozen\": %.0f}, \"cluster_energy_uj\": "
                            "{\"fixed_rule\": %.0f, \"reference\": %.0f, \"best_static\": %.0f, "
                            "\"learned_frozen\": %.0f}, \"waited_for_core_ns\": {\"fixed_rule\": %.0f, \"reference\": %.0f, "
                            "\"best_static\": %.0f, \"learned_frozen\": %.0f}, \"only_contended_blocks\": %s}",
                        D->wall_ns[0], D->wall_ns[1], D->wall_ns[2], D->wall_ns[3], D->energy_uj[0],
                        D->energy_uj[1], D->energy_uj[2], D->energy_uj[3], D->wait_ns[0], D->wait_ns[1],
                        D->wait_ns[2], D->wait_ns[3], D->contended_blocks_only ? "true" : "false");
            fprintf(fp, "}");
        }
        fprintf(fp, "\n    }");
    }
    fprintf(fp, "\n  },\n");
    fprintf(fp,
            "  \"measurability\": {\n"
            "    \"features_used_in_fit\": [\"input shape (M, N)\", \"state size / memory locality (working set over the core's L2)\", "
            "\"hardware state (core class)\", \"resource pressure (co-runner bucket)\"],\n"
            "    \"features_recorded_not_fit\": [\"thermal state (acpitz, degrees C)\", \"current realization\", "
            "\"evidence requirement (always: verified arms only)\", \"latency budget\", \"energy budget\"],\n"
            "    \"features_constant_here\": [\"semantic operation class (matvec only)\", \"branch count (0)\", "
            "\"cognitive requirement (none for matvec)\"],\n"
            "    \"targets_measured\": [\"latency (ps/call)\", \"energy (cluster meter, per arm power and per replay block)\", "
            "\"memory (code bytes + working set)\", \"throughput (derived: elements / latency)\", "
            "\"failure probability (verification refusals and wrong results)\", \"verification cost (ns)\", "
            "\"recompute cost (synthesis + verification ns)\", \"contention (H3 / H1 latency, same shape and arm)\", "
            "\"quality (exact: digest equal to the reference)\"],\n"
            "    \"targets_not_measured\": [\"transfer cost (one shared-memory CPU; no device transfer in this operation)\"]\n"
            "  },\n");
    fprintf(fp, "  \"gates\": {\n    \"OMEGA_EMPIRICAL_OPTIMIZER_PASS\": \"%s\",\n", gate);
    fprintf(fp, "    \"win_reason\": \"%s\",\n", v.win ? "all held-out criteria met" : v.why);
    fprintf(fp,
            "    \"criteria\": \"per core class and held-out distribution: learned (frozen and online, "
            "online including its exploration) at least 5%% under the fixed rule; frozen within 3%% of the "
            "best static arm and under it over all held-out work; replay wall time under the fixed rule; "
            "80%% intervals cover at least 60%% on H1 and H2 (H3 is a condition absent from training: "
            "reported, not gated); zero mismatches; planted arm never run\",\n");
    fprintf(fp,
            "    \"not_claimed\": [\"integration into the resident omega.select reaction (the R10 "
            "faculty still selects by its own measurement)\", \"operations other than integer matvec\", "
            "\"graphics-processor realizations\", \"thermal or cognitive features varied in a controlled way\", "
            "\"energy per call (the meter updates every ~0.1 s; energy is per arm window and per replay block)\", "
            "\"transfer cost\", \"AIENOS kernel (the authority runs as a host library)\"]\n  }\n}\n");
    fclose(fp);
    printf("receipt: %s\n", path);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);
    discover();
    find_meter();
    for (uint32_t k = 0; k < RX_CM_CORES; k++) R.present[k] = g_class[k].n > 0;
    printf("[*] cores: %s; energy meter: %s\n", g_cpus_desc, g_meter_dir[0] ? g_meter_dir : "none");
    R.thermal_start = thermal_c();
    loadavg(R.load_start, sizeof R.load_start);
    g_A = calloc(MAX_ELEMS + GUARD, 8);
    g_x = calloc(MAX_N + GUARD, 8);
    g_y = calloc(MAX_M + GUARD, 8);
    if (!g_A || !g_x || !g_y) return 2;

    rx_cm_default_policy(&g_online_policy);
    g_frozen_policy = g_online_policy;
    g_train_policy = g_online_policy;
    g_train_policy.explore_frac = 1.0;          /* training is the exploration phase */
    g_train_policy.explore_allow_ps = 200000000000ull;
    g_online_policy.explore_frac = 0.05;                /* held-out: 5% of chosen work */
    g_online_policy.explore_allow_ps = 500000000ull;    /* plus 0.5 ms to get started */

    printf("[*] arms: synthesize, verify in a forked child\n");
    setup_arms();
    printf("[*] power per arm from the cluster meter\n");
    measure_power();

    printf("[*] training: the learner runs and measures on its own\n");
    RxCostModel learned;
    model_fresh(&learned);
    for (uint32_t k = 0; k < RX_CM_CORES; k++) {
        if (!R.present[k]) continue;
        place_quiet(k);
        train_class(&learned, k);
    }

    printf("[*] validation: an independent judge measures its own work\n");
    build_validation();
    for (uint32_t k = 0; k < RX_CM_CORES; k++) R.val_jobs += g_val.n[k] / 2u;
    R.sd_scale = calibrate_learned(&learned);
    printf("    calibration on the other half: spread x%.2f\n", R.sd_scale);
    judge(&learned, &R.val_learned, &R.val_dispatch, &R.val_reference, &R.val_cov_n, &R.val_cov_in80);
    R.proofs_ok = judge_accepts(R.val_learned, R.val_dispatch, R.val_reference, 0) && K.mismatches == 0;
    printf("    learned %.3f of the fixed rule's time on validation; 80%% intervals cover %.3f\n",
           R.val_learned / R.val_dispatch, R.val_cov_n ? (double)R.val_cov_in80 / R.val_cov_n : 0.0);

    printf("[*] promotion through the generation barrier\n");
    RxCostModel durable;
    memset(&durable, 0, sizeof durable);
    promote_phase(&learned, &durable);

    printf("[*] held-out work with the durable model\n");
    for (uint32_t k = 0; k < RX_CM_CORES; k++) {
        if (!R.present[k]) continue;
        place_quiet(k);
        RxCostModel online = durable;
        online.explore_ps = online.work_ps = 0;
        uint32_t n1 = unseen_shapes(g_h1, 60, 7000u + k);
        uint32_t n2 = thin_mix(g_h2, 60, 8000u + k);
        heldout(&durable, k, D_H1, g_h1, n1, &online);
        place_quiet(k);
        heldout(&durable, k, D_H2, g_h2, n2, &online);
        place_quiet(k);
        heldout(&durable, k, D_H3, g_h1, n1, &online);
        if (g_class[k].n) {
            place_quiet(k);
            replay(k, D_H1, g_h1, n1);
            place_quiet(k);
            replay(k, D_H2, g_h2, n2);
        }
        for (uint32_t d = 0; d < N_DIST; d++) {
            DistResult *D = &R.d[k][d];
            printf("    %s %-19s frozen %.3f / online %.3f of the fixed rule; frozen %.3f of best "
                   "static (%s), %.3f of oracle; online measured %llu jobs; cover80 %.2f\n",
                   g_class[k].name, DIST_NAME[d], D->cost[P_FROZEN] / D->cost[P_DISPATCH],
                   D->cost[P_ONLINE] / D->cost[P_DISPATCH], D->cost[P_FROZEN] / D->cost[P_BEST_STATIC],
                   ARM_NAME[R.best_static[k]], D->cost[P_FROZEN] / D->cost[P_ORACLE],
                   (unsigned long long)D->measures, D->cov_n ? (double)D->cov_in80 / D->cov_n : 0.0);
            if (D->replayed)
                printf("        replay wall: frozen %.3f of the fixed rule, energy %.3f\n",
                       D->wall_ns[3] / D->wall_ns[0],
                       D->energy_uj[0] > 0 ? D->energy_uj[3] / D->energy_uj[0] : 0.0);
        }
        uint8_t a[32], b[32];
        rx_cm_digest(&online, a);
        if (!R.proofs_ok) continue;   /* nothing was promoted: no durable model to reload */
        RxCostModel again;
        CHECK(load_durable(g_store_dir, &again, NULL) == RX_GEN_OK, "reload durable");
        rx_cm_digest(&again, b);
        int changed = memcmp(a, b, 32) != 0;
        uint8_t c[32];
        rx_cm_digest(&durable, c);
        R.online_not_durable = changed && memcmp(b, c, 32) == 0;
        CHECK(R.online_not_durable, "online learning changed only the working copy");
    }
    R.thermal_end = thermal_c();
    loadavg(R.load_end, sizeof R.load_end);

    CHECK(K.mismatches == 0, "%llu runs disagreed with the reference", (unsigned long long)K.mismatches);
    CHECK(g_parent_runs[ARM_PLANTED] == 0, "planted arm ran %llu times",
          (unsigned long long)g_parent_runs[ARM_PLANTED]);
    CHECK(R.touched_ineligible == 0, "an ineligible arm was chosen or measured");
    CHECK(R.explore_budget_violations == 0, "exploration exceeded its budget");
    Verdict v = judge_gate();
    if (!R.proofs_ok) {
        v.win = 0;
        snprintf(v.why, sizeof v.why, "the judge rejected the learned model on validation; nothing promoted");
    }
    const char *env = getenv("EMPIRICAL_ALLOW_NO_WIN");
    int allow = env && strcmp(env, "1") == 0;
    printf("    checked %llu runs against the reference: %llu mismatches\n",
           (unsigned long long)K.runs_checked, (unsigned long long)K.mismatches);
    printf("    held-out verdict: %s%s%s\n", v.win ? "learned selection wins" : "no win: ",
           v.win ? "" : v.why, "");
    printf("checks %d failures %d\n", g_checks, g_fail);
    write_receipt(v, allow);
    if (g_fail) return 1;
    return v.win || allow ? 0 : 1;
}
