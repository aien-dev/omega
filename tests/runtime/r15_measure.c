/* r15_measure.c -- see r15_measure.h. Observation only. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "r15_measure.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/perf_event.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

const char *const r15_pmu_event_name[R15_PMU_EVENTS] = {
    "cpu_cycles", "inst_retired", "bus_access", "ll_cache_miss_rd",
    "l2d_cache_refill", "mem_access"};
static const uint64_t k_pmu_event[R15_PMU_EVENTS] = {0x11, 0x08, 0x19, 0x37, 0x17, 0x13};

uint64_t r15_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t r15_thread_cpu_ns(pthread_t t) {
    clockid_t c;
    struct timespec ts;
    if (pthread_getcpuclockid(t, &c) != 0 || clock_gettime(c, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int read_line(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char *ok = fgets(buf, (int)n, f);
    fclose(f);
    if (!ok) return -1;
    buf[strcspn(buf, "\n")] = 0;
    return 0;
}

/* ---- PMU ------------------------------------------------------------------ */

int r15_pmu_open(R15Pmu *p, char *why, size_t n) {
    memset(p, 0, sizeof *p);
    for (int i = 0; i < R15_PMU_MAX; i++)
        for (int e = 0; e < R15_PMU_EVENTS; e++) p->fd[i][e] = -1;
    DIR *d = opendir("/sys/bus/event_source/devices");
    if (!d) { snprintf(why, n, "no event_source devices"); return -1; }
    struct dirent *de;
    char names[R15_PMU_MAX][32];
    int found = 0;
    while ((de = readdir(d)) && found < R15_PMU_MAX)
        if (strncmp(de->d_name, "armv8_pmuv3_", 12) == 0)
            snprintf(names[found++], 32, "%.31s", de->d_name);
    closedir(d);
    if (found == 0) { snprintf(why, n, "no armv8_pmuv3 PMU"); return -1; }
    /* stable order: _0 then _1 */
    if (found == 2 && strcmp(names[0], names[1]) > 0) {
        char t[32];
        memcpy(t, names[0], 32); memcpy(names[0], names[1], 32); memcpy(names[1], t, 32);
    }
    for (int i = 0; i < found; i++) {
        char path[256], buf[128];
        memcpy(p->name[i], names[i], sizeof p->name[i]);
        snprintf(path, sizeof path, "/sys/bus/event_source/devices/%s/type", names[i]);
        if (read_line(path, buf, sizeof buf) != 0) { snprintf(why, n, "%s: no type", names[i]); return -1; }
        p->type[i] = (uint32_t)strtoul(buf, NULL, 10);
        snprintf(path, sizeof path, "/sys/bus/event_source/devices/%s/cpus", names[i]);
        if (read_line(path, p->cpus[i], sizeof p->cpus[i]) != 0) p->cpus[i][0] = 0;
        for (int e = 0; e < R15_PMU_EVENTS; e++) {
            struct perf_event_attr a;
            memset(&a, 0, sizeof a);
            a.size = sizeof a;
            a.type = p->type[i];
            a.config = k_pmu_event[e];
            a.disabled = 1;
            a.inherit = 1;
            a.exclude_hv = 1;
            a.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
            int fd = (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, PERF_FLAG_FD_CLOEXEC);
            if (fd < 0) {
                snprintf(why, n, "perf_event_open %s %s: %s", names[i], r15_pmu_event_name[e],
                         strerror(errno));
                r15_pmu_close(p);
                return -1;
            }
            p->fd[i][e] = fd;
        }
    }
    p->n_pmu = found;
    return 0;
}

void r15_pmu_start(R15Pmu *p) {
    for (int i = 0; i < p->n_pmu; i++)
        for (int e = 0; e < R15_PMU_EVENTS; e++) {
            ioctl(p->fd[i][e], PERF_EVENT_IOC_RESET, 0);
            ioctl(p->fd[i][e], PERF_EVENT_IOC_ENABLE, 0);
        }
}

void r15_pmu_stop(R15Pmu *p, R15PmuRead *out) {
    memset(out, 0, sizeof *out);
    for (int i = 0; i < p->n_pmu; i++)
        for (int e = 0; e < R15_PMU_EVENTS; e++) ioctl(p->fd[i][e], PERF_EVENT_IOC_DISABLE, 0);
    out->ok = p->n_pmu > 0;
    for (int i = 0; i < p->n_pmu; i++)
        for (int e = 0; e < R15_PMU_EVENTS; e++) {
            uint64_t v[3];
            if (read(p->fd[i][e], v, sizeof v) != (ssize_t)sizeof v) { out->ok = 0; continue; }
            out->value[i][e] = v[0];
            out->enabled[i][e] = v[1];
            out->running[i][e] = v[2];
            out->sum[e] += v[0];
        }
    /* C1 item 8: running(A)+running(X) must equal enabled within 0.1%. */
    for (int e = 0; e < R15_PMU_EVENTS && p->n_pmu > 0; e++) {
        uint64_t run = 0, en = out->enabled[0][e];
        for (int i = 0; i < p->n_pmu; i++) run += out->running[i][e];
        uint64_t diff = run > en ? run - en : en - run;
        if (en && diff * 1000u > en) out->multiplexed = 1;
    }
}

void r15_pmu_close(R15Pmu *p) {
    for (int i = 0; i < R15_PMU_MAX; i++)
        for (int e = 0; e < R15_PMU_EVENTS; e++)
            if (p->fd[i][e] >= 0) { close(p->fd[i][e]); p->fd[i][e] = -1; }
    p->n_pmu = 0;
}

/* ---- energy --------------------------------------------------------------- */

static const char *const k_elabel[R15_SPBM_ENERGY] = {"pkg", "cpu_e", "cpu_p", "gpc_unverified", "gpm"};
static const char *const k_plabel[R15_SPBM_POWER] = {"sys_total", "soc_pkg", "cpu_e", "cpu_p", "gpu"};

static int read_u64(const char *dir, const char *kind, int i, const char *suffix, uint64_t *v) {
    char path[256], buf[64], *end;
    snprintf(path, sizeof path, "%s/%s%d_%s", dir, kind, i, suffix);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t got = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (got <= 0) return -1;
    buf[got] = 0;
    errno = 0;
    *v = strtoull(buf, &end, 10);
    return errno || end == buf || buf[0] == '-' ? -1 : 0;
}

int r15_energy_open(R15Energy *e, char *why, size_t n) {
    memset(e, 0, sizeof *e);
    pthread_mutex_init(&e->mu, NULL);
    DIR *d = opendir("/sys/class/hwmon");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            char path[256], buf[64];
            snprintf(path, sizeof path, "/sys/class/hwmon/%.200s/name", de->d_name);
            if (read_line(path, buf, sizeof buf) == 0 && strcmp(buf, "aien_spbm") == 0) {
                snprintf(e->hwmon, sizeof e->hwmon, "/sys/class/hwmon/%.100s", de->d_name);
                break;
            }
        }
        closedir(d);
    }
    if (e->hwmon[0]) {
        for (int i = 0; i < R15_SPBM_ENERGY; i++) {
            char path[256], buf[64];
            snprintf(path, sizeof path, "%s/energy%d_label", e->hwmon, i + 1);
            if (read_line(path, buf, sizeof buf) != 0 || strcmp(buf, k_elabel[i]) != 0) {
                snprintf(why, n, "SPBM energy%d label is not %s", i + 1, k_elabel[i]);
                e->hwmon[0] = 0;
                break;
            }
        }
        for (int i = 0; i < R15_SPBM_POWER && e->hwmon[0]; i++) {
            char path[256], buf[64];
            snprintf(path, sizeof path, "%s/power%d_label", e->hwmon, i + 1);
            if (read_line(path, buf, sizeof buf) != 0 || strcmp(buf, k_plabel[i]) != 0) {
                snprintf(why, n, "SPBM power%d label is not %s", i + 1, k_plabel[i]);
                e->hwmon[0] = 0;
            }
        }
    } else {
        snprintf(why, n, "no aien_spbm hwmon: package energy unavailable");
    }
    e->nvml_lib = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
    if (e->nvml_lib) {
        int (*init)(void) = (int (*)(void))dlsym(e->nvml_lib, "nvmlInit_v2");
        int (*handle)(unsigned int, void **) =
            (int (*)(unsigned int, void **))dlsym(e->nvml_lib, "nvmlDeviceGetHandleByIndex_v2");
        e->nvml_energy = (int (*)(void *, unsigned long long *))
            dlsym(e->nvml_lib, "nvmlDeviceGetTotalEnergyConsumption");
        e->nvml_power = (int (*)(void *, unsigned int *))dlsym(e->nvml_lib, "nvmlDeviceGetPowerUsage");
        e->nvml_shutdown = (int (*)(void))dlsym(e->nvml_lib, "nvmlShutdown");
        if (!init || !handle || !e->nvml_energy || !e->nvml_power || init() != 0 ||
            handle(0, &e->nvml_dev) != 0)
            e->nvml_dev = NULL;
    }
    return e->hwmon[0] ? 0 : -1;
}

int r15_energy_read(R15Energy *e, R15EnergySample *s) {
    memset(s, 0, sizeof *s);
    uint64_t t0 = r15_now_ns();
    if (e->hwmon[0]) {
        s->spbm_ok = 1;
        for (int i = 0; i < R15_SPBM_ENERGY; i++) {
            if (read_u64(e->hwmon, "energy", i + 1, "input", &s->energy_uj[i]) != 0 ||
                read_u64(e->hwmon, "energy", i + 1, "overflow_raw", &s->overflow[i]) != 0)
                s->spbm_ok = 0;
        }
        for (int i = 0; i < R15_SPBM_POWER; i++)
            if (read_u64(e->hwmon, "power", i + 1, "input", &s->power_uw[i]) != 0) s->spbm_ok = 0;
    }
    if (e->nvml_dev) {
        unsigned long long mj = 0;
        unsigned int mw = 0;
        s->nvml_ok = e->nvml_energy(e->nvml_dev, &mj) == 0 && e->nvml_power(e->nvml_dev, &mw) == 0;
        s->nvml_mj = mj;
        s->nvml_mw = mw;
    }
    s->t_ns = r15_now_ns();
    s->read_span_ns = s->t_ns - t0;
    return s->spbm_ok ? 0 : -1;
}

static void *sampler_main(void *arg) {
    R15Energy *e = arg;
    uint64_t next = r15_now_ns();
    while (!__atomic_load_n(&e->stop, __ATOMIC_ACQUIRE)) {
        R15EnergySample s;
        r15_energy_read(e, &s);
        pthread_mutex_lock(&e->mu);
        e->ring[e->n % e->cap] = s;
        e->n++;
        pthread_mutex_unlock(&e->mu);
        next += 100000000ull;   /* 10 Hz */
        uint64_t now = r15_now_ns();
        if (next > now) {
            struct timespec ts = {(time_t)((next - now) / 1000000000ull),
                                  (long)((next - now) % 1000000000ull)};
            nanosleep(&ts, NULL);
        } else {
            next = now;
        }
    }
    return NULL;
}

int r15_energy_start_sampler(R15Energy *e, uint64_t cap) {
    e->ring = calloc(cap, sizeof *e->ring);
    if (!e->ring) return -1;
    e->cap = cap;
    e->n = 0;
    e->stop = 0;
    if (pthread_create(&e->thread, NULL, sampler_main, e) != 0) return -1;
    e->running = 1;
    return 0;
}

uint64_t r15_energy_samples(R15Energy *e, uint64_t t0, uint64_t t1, R15EnergySample *out,
                            uint64_t max) {
    uint64_t got = 0;
    pthread_mutex_lock(&e->mu);
    uint64_t first = e->n > e->cap ? e->n - e->cap : 0;
    for (uint64_t i = first; i < e->n && got < max; i++) {
        const R15EnergySample *s = &e->ring[i % e->cap];
        if (s->t_ns >= t0 && s->t_ns <= t1) out[got++] = *s;
    }
    pthread_mutex_unlock(&e->mu);
    return got;
}

void r15_energy_close(R15Energy *e) {
    if (e->running) {
        __atomic_store_n(&e->stop, 1, __ATOMIC_RELEASE);
        pthread_join(e->thread, NULL);
        e->running = 0;
    }
    free(e->ring);
    e->ring = NULL;
    if (e->nvml_dev && e->nvml_shutdown) e->nvml_shutdown();
    if (e->nvml_lib) dlclose(e->nvml_lib);
    e->nvml_lib = e->nvml_dev = NULL;
}

/* ---- histogram ------------------------------------------------------------ */

uint32_t r15_hist_bucket(uint64_t v) {
    if (v < 64) return (uint32_t)v;
    uint32_t e = 63u - (uint32_t)__builtin_clzll(v);   /* 6..63 */
    uint32_t sub = (uint32_t)(v >> (e - 6)) & 63u;
    return 64u + (e - 6u) * R15_HIST_SUB + sub;
}

uint64_t r15_hist_lower(uint32_t b) {
    if (b < 64) return b;
    uint32_t e = (b - 64u) / R15_HIST_SUB + 6u, sub = (b - 64u) % R15_HIST_SUB;
    return (uint64_t)(64u + sub) << (e - 6u);
}

void r15_hist_add(R15Hist *h, uint64_t v) {
    h->count[r15_hist_bucket(v)]++;
    if (h->n == 0 || v < h->min) h->min = v;
    if (v > h->max) h->max = v;
    h->n++;
    h->sum += v;
}

/* {"n":..,"min":..,"max":..,"sum":..,"b":[[lower,count],...]} */
void r15_hist_json(FILE *f, const R15Hist *h) {
    fprintf(f, "{\"n\":%" PRIu64 ",\"min\":%" PRIu64 ",\"max\":%" PRIu64 ",\"sum\":%" PRIu64 ",\"b\":[",
            h->n, h->min, h->max, h->sum);
    int first = 1;
    for (uint32_t b = 0; b < R15_HIST_BUCKETS; b++)
        if (h->count[b]) {
            fprintf(f, "%s[%" PRIu64 ",%" PRIu64 "]", first ? "" : ",", r15_hist_lower(b), h->count[b]);
            first = 0;
        }
    fputs("]}", f);
}

/* ---- evidence ------------------------------------------------------------- */

int r15_out_open(R15Out *o, const char *path, const char *run, const char *level,
                 const char *config, int round, int trial) {
    memset(o, 0, sizeof *o);
    o->f = fopen(path, "wx");   /* never overwrite raw evidence */
    if (!o->f) return -1;
    snprintf(o->run, sizeof o->run, "%s", run);
    snprintf(o->level, sizeof o->level, "%s", level);
    snprintf(o->config, sizeof o->config, "%s", config);
    o->round = round;
    o->trial = trial;
    return 0;
}

void r15_rec_begin(R15Out *o, const char *kind) {
    fprintf(o->f, "{\"run\":\"%s\",\"level\":\"%s\",\"config\":\"%s\",\"round\":%d,\"trial\":%d,"
                  "\"kind\":\"%s\",\"t_ns\":%" PRIu64,
            o->run, o->level, o->config, o->round, o->trial, kind, r15_now_ns());
}

void r15_rec_end(R15Out *o) {
    fputs("}\n", o->f);
    fflush(o->f);
}

void r15_json_energy(FILE *f, const char *key, const R15EnergySample *s) {
    fprintf(f, ",\"%s\":{\"t_ns\":%" PRIu64 ",\"span_ns\":%" PRIu64 ",\"spbm_ok\":%d,\"energy_uj\":[",
            key, s->t_ns, s->read_span_ns, s->spbm_ok);
    for (int i = 0; i < R15_SPBM_ENERGY; i++) fprintf(f, "%s%" PRIu64, i ? "," : "", s->energy_uj[i]);
    fputs("],\"overflow\":[", f);
    for (int i = 0; i < R15_SPBM_ENERGY; i++) fprintf(f, "%s%" PRIu64, i ? "," : "", s->overflow[i]);
    fputs("],\"power_uw\":[", f);
    for (int i = 0; i < R15_SPBM_POWER; i++) fprintf(f, "%s%" PRIu64, i ? "," : "", s->power_uw[i]);
    fprintf(f, "],\"nvml_ok\":%d,\"nvml_mj\":%" PRIu64 ",\"nvml_mw\":%u}", s->nvml_ok, s->nvml_mj,
            s->nvml_mw);
}

void r15_json_pmu(FILE *f, const char *key, const R15Pmu *p, const R15PmuRead *r) {
    fprintf(f, ",\"%s\":{\"ok\":%d,\"multiplexed\":%d,\"sum\":{", key, r->ok, r->multiplexed);
    for (int e = 0; e < R15_PMU_EVENTS; e++)
        fprintf(f, "%s\"%s\":%" PRIu64, e ? "," : "", r15_pmu_event_name[e], r->sum[e]);
    fputs("},\"pmu\":[", f);
    for (int i = 0; i < p->n_pmu; i++) {
        fprintf(f, "%s{\"name\":\"%s\",\"type\":%u,\"cpus\":\"%s\",\"value\":[", i ? "," : "",
                p->name[i], p->type[i], p->cpus[i]);
        for (int e = 0; e < R15_PMU_EVENTS; e++) fprintf(f, "%s%" PRIu64, e ? "," : "", r->value[i][e]);
        fputs("],\"enabled\":[", f);
        for (int e = 0; e < R15_PMU_EVENTS; e++) fprintf(f, "%s%" PRIu64, e ? "," : "", r->enabled[i][e]);
        fputs("],\"running\":[", f);
        for (int e = 0; e < R15_PMU_EVENTS; e++) fprintf(f, "%s%" PRIu64, e ? "," : "", r->running[i][e]);
        fputs("]}", f);
    }
    fputs("]}", f);
}

int r15_out_close(R15Out *o) {
    if (!o->f) return 0;
    int rc = fclose(o->f);
    o->f = NULL;
    return rc;
}
