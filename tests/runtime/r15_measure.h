/*
 * r15_measure.h -- R15 physical measurement: CPU PMU (spec §16 C1 item 8),
 * SPBM package energy and NVML graphics energy (§17 C2), latency histograms,
 * and the JSON Lines evidence writer (§12).
 *
 * Nothing here decides anything about the body; it only observes.
 */
#ifndef R15_MEASURE_H
#define R15_MEASURE_H

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

/* ---- PMU ------------------------------------------------------------------
 * Per task (pid 0, cpu -1), inherit, both CPU PMUs, six events, no groups,
 * user + kernel, exclude_hv. Open from the main thread before any thread
 * whose work is to be counted is created. Raw sums over the PMUs; never
 * scaled. */
#define R15_PMU_EVENTS 6
#define R15_PMU_MAX 2
extern const char *const r15_pmu_event_name[R15_PMU_EVENTS];

typedef struct {
    int n_pmu;
    uint32_t type[R15_PMU_MAX];
    char name[R15_PMU_MAX][32];
    char cpus[R15_PMU_MAX][64];
    int fd[R15_PMU_MAX][R15_PMU_EVENTS];
} R15Pmu;

typedef struct {
    uint64_t value[R15_PMU_MAX][R15_PMU_EVENTS];
    uint64_t enabled[R15_PMU_MAX][R15_PMU_EVENTS];
    uint64_t running[R15_PMU_MAX][R15_PMU_EVENTS];
    uint64_t sum[R15_PMU_EVENTS];   /* raw sum over PMUs */
    int multiplexed;                /* running(A)+running(X) != enabled beyond 0.1% */
    int ok;
} R15PmuRead;

int  r15_pmu_open(R15Pmu *p, char *why, size_t n);
void r15_pmu_start(R15Pmu *p);                 /* RESET + ENABLE */
void r15_pmu_stop(R15Pmu *p, R15PmuRead *out); /* DISABLE + read */
void r15_pmu_close(R15Pmu *p);

/* ---- energy -------------------------------------------------------------- */
#define R15_SPBM_ENERGY 5   /* pkg, cpu_e, cpu_p, gpc_unverified, gpm */
#define R15_SPBM_POWER 5    /* sys_total, soc_pkg, cpu_e, cpu_p, gpu */

typedef struct {
    uint64_t t_ns;                          /* CLOCK_MONOTONIC after the reads */
    uint64_t read_span_ns;
    uint64_t energy_uj[R15_SPBM_ENERGY];
    uint64_t power_uw[R15_SPBM_POWER];
    uint64_t overflow[R15_SPBM_ENERGY];
    uint64_t nvml_mj;
    uint32_t nvml_mw;
    int spbm_ok, nvml_ok;
} R15EnergySample;

typedef struct {
    char hwmon[128];            /* empty: no SPBM reader (metric 17 unavailable) */
    void *nvml_lib, *nvml_dev;
    int (*nvml_energy)(void *, unsigned long long *);
    int (*nvml_power)(void *, unsigned int *);
    int (*nvml_shutdown)(void);
    /* 10 Hz sampler (spec §7): started before the PMU is opened, so its own
     * reads are not counted as the body's work. */
    pthread_t thread;
    int running;
    volatile int stop;
    pthread_mutex_t mu;
    R15EnergySample *ring;
    uint64_t cap, n;            /* samples taken since start (may exceed cap) */
} R15Energy;

int  r15_energy_open(R15Energy *e, char *why, size_t n);
int  r15_energy_read(R15Energy *e, R15EnergySample *s);
int  r15_energy_start_sampler(R15Energy *e, uint64_t cap);
/* Samples taken in [t0, t1]; returns count copied (<= max). */
uint64_t r15_energy_samples(R15Energy *e, uint64_t t0, uint64_t t1,
                            R15EnergySample *out, uint64_t max);
void r15_energy_close(R15Energy *e);

/* ---- histogram ------------------------------------------------------------
 * Log-linear buckets: exact below 64 ns, then 64 sub-buckets per octave
 * (relative bucket width <= 1/64). Stored sparsely in evidence. */
#define R15_HIST_SUB 64
#define R15_HIST_BUCKETS (64 + 58 * R15_HIST_SUB)
typedef struct {
    uint64_t count[R15_HIST_BUCKETS];
    uint64_t n, min, max, sum;
} R15Hist;
void     r15_hist_add(R15Hist *h, uint64_t v);
uint64_t r15_hist_lower(uint32_t bucket);
uint32_t r15_hist_bucket(uint64_t v);
void     r15_hist_json(FILE *f, const R15Hist *h);

/* ---- evidence ------------------------------------------------------------- */
typedef struct {
    FILE *f;
    char run[64], level[16], config[16];
    int round, trial;
} R15Out;
int  r15_out_open(R15Out *o, const char *path, const char *run, const char *level,
                  const char *config, int round, int trial);
/* Starts a record with the common keys; the caller appends ",\"k\":v..."
 * and ends it with r15_rec_end. */
void r15_rec_begin(R15Out *o, const char *kind);
void r15_rec_end(R15Out *o);
void r15_json_energy(FILE *f, const char *key, const R15EnergySample *s);
void r15_json_pmu(FILE *f, const char *key, const R15Pmu *p, const R15PmuRead *r);
int  r15_out_close(R15Out *o);

uint64_t r15_now_ns(void);
uint64_t r15_thread_cpu_ns(pthread_t t);   /* 0 if unavailable */

#endif
