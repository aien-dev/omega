/* ty_energy_window: measure ONE TY-5 window (docs/turing/TURING_YIELD_ENERGY_PROTOCOL_V0.md).
 *
 * The rounds, the idle wait and the pause between windows live in the shell
 * driver (tools/ty_energy_run.sh); this tool measures one window per call and
 * exits, so it holds no timed loop of its own.
 *
 * Meter: tests/runtime/r15_measure linked unchanged. r15_energy_open is NOT
 * called (it loads NVML); meter_attach() below does the SPBM half of it: find
 * the aien_spbm hwmon, check all ten channel labels, initialise the mutex.
 * Every later use (r15_energy_read, the 10 Hz sampler, r15_energy_samples) is
 * the frozen code, and the NVML pointers stay NULL, so it is never touched.
 *
 * usage:
 *   ty_energy_window --plan SEED ROUNDS            print "round pos cond" lines
 *   ty_energy_window --out FILE --run ID --config S1 --cond AB --round R --pos K
 *       --secs 30 --workload BIN --a R1_plain:5,6 --b R2c_crumb:7,8
 *       [--m 8192 --n 16384 --seed-a 1 --seed-b 2 --settle-ms 2000 --tool-cpu 0]
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "r15_measure.h"

#define TYE_NCPU 20
#define TYE_NZONE 16
#define TYE_MAXS 2048

/* Same tables as r15_measure.c (static there). */
static const char *const tye_elabel[R15_SPBM_ENERGY] = {"pkg", "cpu_e", "cpu_p", "gpc_unverified", "gpm"};
static const char *const tye_plabel[R15_SPBM_POWER] = {"sys_total", "soc_pkg", "cpu_e", "cpu_p", "gpu"};

static int line_of(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char *ok = fgets(buf, (int)n, f);
    fclose(f);
    if (!ok) return -1;
    buf[strcspn(buf, "\n")] = 0;
    return 0;
}

static int meter_attach(R15Energy *e, char *why, size_t n) {
    memset(e, 0, sizeof *e);
    pthread_mutex_init(&e->mu, NULL);
    DIR *d = opendir("/sys/class/hwmon");
    struct dirent *de;
    char path[512], buf[64], dir[160] = "";
    for (de = d ? readdir(d) : NULL; de; de = readdir(d)) {
        snprintf(path, sizeof path, "/sys/class/hwmon/%.200s/name", de->d_name);
        if (!dir[0] && line_of(path, buf, sizeof buf) == 0 && !strcmp(buf, "aien_spbm"))
            snprintf(dir, sizeof dir, "/sys/class/hwmon/%.100s", de->d_name);
    }
    if (d) closedir(d);
    if (!dir[0]) { snprintf(why, n, "no aien_spbm hwmon"); return -1; }
    for (int i = 0; i < R15_SPBM_ENERGY; i++) {
        snprintf(path, sizeof path, "%s/energy%d_label", dir, i + 1);
        if (line_of(path, buf, sizeof buf) || strcmp(buf, tye_elabel[i])) {
            snprintf(why, n, "energy%d label is not %s", i + 1, tye_elabel[i]);
            return -1;
        }
    }
    for (int i = 0; i < R15_SPBM_POWER; i++) {
        snprintf(path, sizeof path, "%s/power%d_label", dir, i + 1);
        if (line_of(path, buf, sizeof buf) || strcmp(buf, tye_plabel[i])) {
            snprintf(why, n, "power%d label is not %s", i + 1, tye_plabel[i]);
            return -1;
        }
    }
    snprintf(e->hwmon, sizeof e->hwmon, "%s", dir);
    return 0;
}

/* ---- machine snapshot ---- */
typedef struct {
    uint64_t busy[TYE_NCPU], total[TYE_NCPU];
    uint64_t freq_khz[TYE_NCPU];
    long temp_mc[TYE_NZONE];
    int nzone;
    char load[96];
} snap;

static void take_snap(snap *s) {
    memset(s, 0, sizeof *s);
    char line[512], path[160];
    FILE *f = fopen("/proc/stat", "r");
    for (; f && fgets(line, sizeof line, f);) {
        int cpu;
        unsigned long long v[8] = {0};
        if (sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu", &cpu, &v[0], &v[1], &v[2], &v[3],
                   &v[4], &v[5], &v[6], &v[7]) == 9 && cpu >= 0 && cpu < TYE_NCPU) {
            s->busy[cpu] = v[0] + v[1] + v[2] + v[5] + v[6] + v[7];
            s->total[cpu] = s->busy[cpu] + v[3] + v[4];
        }
    }
    if (f) fclose(f);
    line_of("/proc/loadavg", s->load, sizeof s->load);
    for (int c = 0; c < TYE_NCPU; c++) {
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", c);
        if (line_of(path, line, sizeof line) == 0) s->freq_khz[c] = strtoull(line, NULL, 10);
    }
    for (int z = 0; z < TYE_NZONE; z++) {
        snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", z);
        if (line_of(path, line, sizeof line) != 0) break;
        s->temp_mc[z] = strtol(line, NULL, 10);
        s->nzone = z + 1;
    }
}

static void json_snap(FILE *f, const char *key, const snap *s) {
    fprintf(f, ",\"%s\":{\"loadavg\":\"%s\",\"freq_khz\":[", key, s->load);
    for (int c = 0; c < TYE_NCPU; c++) fprintf(f, "%s%" PRIu64, c ? "," : "", s->freq_khz[c]);
    fputs("],\"temp_mc\":[", f);
    for (int z = 0; z < s->nzone; z++) fprintf(f, "%s%ld", z ? "," : "", s->temp_mc[z]);
    fputs("],\"busy_jiffies\":[", f);
    for (int c = 0; c < TYE_NCPU; c++) fprintf(f, "%s%" PRIu64, c ? "," : "", s->busy[c]);
    fputs("],\"total_jiffies\":[", f);
    for (int c = 0; c < TYE_NCPU; c++) fprintf(f, "%s%" PRIu64, c ? "," : "", s->total[c]);
    fputs("]}", f);
}

/* ---- plan: seeded splitmix64 Fisher-Yates per round ---- */
static uint64_t g_mix;
static uint64_t mix_next(void) {
    uint64_t z = (g_mix += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static int print_plan(uint64_t seed, int rounds) {
    static const char *const cond[4] = {"IDLE", "A", "B", "AB"};
    g_mix = seed;
    for (int r = 0; r < rounds; r++) {
        int o[4] = {0, 1, 2, 3};
        for (int i = 3; i > 0; i--) {
            int j = (int)(mix_next() % (uint64_t)(i + 1)), t = o[i];
            o[i] = o[j];
            o[j] = t;
        }
        for (int k = 0; k < 4; k++) printf("%d %d %s\n", r, k, cond[o[k]]);
    }
    return 0;
}

/* ---- workload processes ---- */
typedef struct {
    char tag[4];
    char spec[64]; /* rz:cpus */
    char rz[32], cpus[32];
    pid_t pid;
    int to_child;
    FILE *from_child;
    char ready[160];
    char result[16384];
    int status, ok;
} part;

static int spawn(part *p, const char *bin, const char *m, const char *n, const char *seed, unsigned secs) {
    char *colon = strchr(p->spec, ':');
    if (!colon) return -1;
    snprintf(p->rz, sizeof p->rz, "%.*s", (int)(colon - p->spec), p->spec);
    snprintf(p->cpus, sizeof p->cpus, "%s", colon + 1);
    int in[2], out[2];
    if (pipe(in) || pipe(out)) return -1;
    char secs_s[16];
    snprintf(secs_s, sizeof secs_s, "%u", secs);
    p->pid = fork();
    if (p->pid < 0) return -1;
    if (p->pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        close(in[1]);
        close(out[0]);
        execl(bin, bin, "--tag", p->tag, "--rz", p->rz, "--m", m, "--n", n, "--cpus", p->cpus, "--secs", secs_s,
              "--seed", seed, (char *)NULL);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    p->to_child = in[1];
    p->from_child = fdopen(out[0], "r");
    return p->from_child ? 0 : -1;
}

static void pin_self(int cpu) {
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    sched_setaffinity(0, sizeof s, &s);
}

static void pause_ns(uint64_t ns) {
    struct timespec ts = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
    nanosleep(&ts, NULL);
}

/* --shuffle SEED N: a seeded permutation of 0..N-1 (configuration order). */
static int print_shuffle(uint64_t seed, int n) {
    int o[64];
    if (n < 1 || n > 64) return 64;
    for (int i = 0; i < n; i++) o[i] = i;
    g_mix = seed;
    for (int i = n - 1; i > 0; i--) {
        int j = (int)(mix_next() % (uint64_t)(i + 1)), t = o[i];
        o[i] = o[j];
        o[j] = t;
    }
    for (int i = 0; i < n; i++) printf("%d\n", o[i]);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--plan")) return print_plan(strtoull(argv[2], NULL, 0), atoi(argv[3]));
    if (argc == 4 && !strcmp(argv[1], "--shuffle")) return print_shuffle(strtoull(argv[2], NULL, 0), atoi(argv[3]));
    const char *out = NULL, *run = "run", *config = "S0", *cond = "IDLE", *bin = NULL, *sa = NULL, *sb = NULL;
    const char *m = "8192", *n = "16384", *seed_a = "1", *seed_b = "2";
    int round = 0, pos = 0, tool_cpu = 0;
    unsigned secs = 30, settle_ms = 2000;
    for (int i = 1; i + 1 < argc; i += 2) {
        const char *k = argv[i], *v = argv[i + 1];
        if (!strcmp(k, "--out")) out = v;
        else if (!strcmp(k, "--run")) run = v;
        else if (!strcmp(k, "--config")) config = v;
        else if (!strcmp(k, "--cond")) cond = v;
        else if (!strcmp(k, "--round")) round = atoi(v);
        else if (!strcmp(k, "--pos")) pos = atoi(v);
        else if (!strcmp(k, "--secs")) secs = (unsigned)strtoul(v, NULL, 10);
        else if (!strcmp(k, "--workload")) bin = v;
        else if (!strcmp(k, "--a")) sa = v;
        else if (!strcmp(k, "--b")) sb = v;
        else if (!strcmp(k, "--m")) m = v;
        else if (!strcmp(k, "--n")) n = v;
        else if (!strcmp(k, "--seed-a")) seed_a = v;
        else if (!strcmp(k, "--seed-b")) seed_b = v;
        else if (!strcmp(k, "--settle-ms")) settle_ms = (unsigned)strtoul(v, NULL, 10);
        else if (!strcmp(k, "--tool-cpu")) tool_cpu = atoi(v);
        else { fprintf(stderr, "ty_energy_window: unknown argument %s\n", k); return 64; }
    }
    if (!out || !bin || !sa || !sb || !secs) {
        fprintf(stderr, "ty_energy_window: need --out --workload --a --b --secs\n");
        return 64;
    }
    int want_a = strcmp(cond, "IDLE") && strchr(cond, 'A'), want_b = strcmp(cond, "IDLE") && strchr(cond, 'B');
    part P[2];
    memset(P, 0, sizeof P);
    snprintf(P[0].tag, sizeof P[0].tag, "A");
    snprintf(P[0].spec, sizeof P[0].spec, "%s", sa);
    snprintf(P[1].tag, sizeof P[1].tag, "B");
    snprintf(P[1].spec, sizeof P[1].spec, "%s", sb);
    int np = 0;
    part *act[2];
    if (want_a) act[np++] = &P[0];
    if (want_b) act[np++] = &P[1];

    R15Out o;
    if (r15_out_open(&o, out, run, cond, config, round, pos) != 0) {
        fprintf(stderr, "ty_energy_window: cannot create %s (exists?)\n", out);
        return 2;
    }
    pin_self(tool_cpu); /* the sampler thread inherits this: never on a measured core */
    R15Energy e;
    char why[256] = "";
    int meter_ok = meter_attach(&e, why, sizeof why) == 0;
    if (meter_ok) r15_energy_start_sampler(&e, TYE_MAXS);

    int spawn_ok = 1;
    for (int i = 0; i < np; i++) {
        part *p = act[i];
        if (spawn(p, bin, m, n, p == &P[0] ? seed_a : seed_b, secs) != 0 ||
            !fgets(p->ready, sizeof p->ready, p->from_child) || strncmp(p->ready, "READY ", 6) != 0)
            spawn_ok = 0;
        p->ready[strcspn(p->ready, "\n")] = 0;
    }
    pause_ns((uint64_t)settle_ms * 1000000ull);
    snap s0, s1;
    take_snap(&s0);
    char quiet[256] = "";
    char qpath[512];
    snprintf(qpath, sizeof qpath, "%s/workspace/.spark-quiet", getenv("HOME") ? getenv("HOME") : "");
    int quiet_present = line_of(qpath, quiet, sizeof quiet) == 0;
    for (char *c = quiet; *c; c++)
        if (*c == '"' || *c == '\\' || (unsigned char)*c < 32) *c = ' ';

    R15EnergySample e0, e1;
    r15_energy_read(&e, &e0);
    uint64_t t_go = r15_now_ns();
    for (int i = 0; i < np; i++)
        if (act[i]->to_child >= 0 && write(act[i]->to_child, "g", 1) != 1) spawn_ok = 0;
    if (np == 0) pause_ns((uint64_t)secs * 1000000000ull);
    for (int i = 0; i < np; i++) {
        part *p = act[i];
        if (!p->from_child || !fgets(p->result, sizeof p->result, p->from_child)) p->result[0] = 0;
        p->result[strcspn(p->result, "\n")] = 0;
        if (p->pid > 0) waitpid(p->pid, &p->status, 0);
        p->ok = p->result[0] == '{' && WIFEXITED(p->status) && WEXITSTATUS(p->status) == 0;
    }
    r15_energy_read(&e, &e1);
    take_snap(&s1);

    static R15EnergySample smp[TYE_MAXS];
    uint64_t ns = meter_ok ? r15_energy_samples(&e, e0.t_ns, e1.t_ns, smp, TYE_MAXS) : 0;
    if (meter_ok) r15_energy_close(&e);

    r15_rec_begin(&o, "ty_window");
    FILE *f = o.f;
    fprintf(f, ",\"meter_ok\":%d,\"meter_why\":\"%s\",\"spawn_ok\":%d,\"planned_ns\":%" PRIu64
               ",\"settle_ns\":%" PRIu64 ",\"go_ns\":%" PRIu64 ",\"tool_cpu\":%d,\"m\":%s,\"n\":%s,"
               "\"spec_a\":\"%s\",\"spec_b\":\"%s\",\"quiet_present\":%d,\"quiet\":\"%s\"",
            meter_ok, why, spawn_ok, (uint64_t)secs * (uint64_t)1000000000u, (uint64_t)settle_ms * (uint64_t)1000000u, t_go,
            tool_cpu, m, n, sa, sb, quiet_present, quiet);
    r15_json_energy(f, "e0", &e0);
    r15_json_energy(f, "e1", &e1);
    json_snap(f, "s0", &s0);
    json_snap(f, "s1", &s1);
    fprintf(f, ",\"tel\":{\"n\":%" PRIu64 ",\"t_ns\":[", ns);
    for (uint64_t i = 0; i < ns; i++) fprintf(f, "%s%" PRIu64, i ? "," : "", smp[i].t_ns);
    fputs("],\"ok\":[", f);
    for (uint64_t i = 0; i < ns; i++) fprintf(f, "%s%d", i ? "," : "", smp[i].spbm_ok);
    for (int ch = 0; ch < 3; ch++) {
        fprintf(f, "],\"%s\":[", tye_elabel[ch]);
        for (uint64_t i = 0; i < ns; i++) fprintf(f, "%s%" PRIu64, i ? "," : "", smp[i].energy_uj[ch]);
    }
    fputs("]},\"participants\":[", f);
    for (int i = 0; i < np; i++)
        fprintf(f, "%s{\"ok\":%d,\"exit\":%d,\"ready\":\"%s\",\"result\":%s}", i ? "," : "", act[i]->ok,
                WIFEXITED(act[i]->status) ? WEXITSTATUS(act[i]->status) : -1, act[i]->ready,
                act[i]->result[0] == '{' ? act[i]->result : "null");
    fputc(']', f);
    r15_rec_end(&o);
    int rc = r15_out_close(&o);
    for (int i = 0; i < np; i++) {
        if (act[i]->to_child > 0) close(act[i]->to_child);
        if (act[i]->from_child) fclose(act[i]->from_child);
    }
    return rc == 0 && meter_ok && spawn_ok ? 0 : 1;
}
