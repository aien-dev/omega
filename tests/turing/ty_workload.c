/* ty_workload: one TY-5 workload process (docs/turing/TURING_YIELD_ENERGY_PROTOCOL_V0.md).
 *
 * Runs one MA-2 realization (oma_rz_get(i)->run, read-only use of src/algebra)
 * back to back on one fixed shape, one thread per listed cpu, each thread
 * pinned and holding its own packed plan (so every thread streams its own
 * weights from DRAM). The process opens its own CPU PMU counters (tests/runtime/
 * r15_measure, linked unchanged) before the worker threads exist, so the
 * counts cover exactly this workload's threads.
 *
 * Handshake with the window tool (tests/turing/ty_energy_window.c):
 *   stdout "READY <pid> <weight_bytes> <footprint_bytes>\n" after packing and
 *   one oracle check, then a blocking read of one byte on stdin (GO), then the
 *   timed run of --secs seconds, then one JSON line with the result.
 * No sleeping, no polling: the only waits are the blocking read and the joins.
 *
 * usage: ty_workload --tag A --rz R1_plain --m 16384 --n 16384 --cpus 5,6
 *                    --secs 30 --seed 1 [--sparsity-milli 300]
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "algebra/realize_common.h"
#include "r15_measure.h"

#define TYW_MAX_CPUS 10

typedef struct {
    const oma_rz_impl *im;
    oma_rz_plan plan;
    int8_t *x;
    int32_t *y;
    int cpu;
    uint64_t deadline_ns;
    uint64_t t0_ns, t1_ns, calls;
    int pin_ok;
} tyw_thread;

static uint64_t tyw_rng;
static uint64_t tyw_next(void) {
    uint64_t z = (tyw_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static int pin_to(int cpu) {
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    return sched_setaffinity(0, sizeof s, &s) == 0;
}

static void *worker(void *arg) {
    tyw_thread *t = arg;
    t->pin_ok = pin_to(t->cpu);
    t->t0_ns = r15_now_ns();
    uint64_t now = t->t0_ns;
    while (now < t->deadline_ns) {
        t->im->run(&t->plan, t->x, t->y);
        t->calls++;
        now = r15_now_ns();
    }
    t->t1_ns = now;
    return NULL;
}

static void die(const char *m) {
    fprintf(stderr, "ty_workload: %s\n", m);
    exit(2);
}

int main(int argc, char **argv) {
    const char *tag = "A", *rz = "R1_plain", *cpus = "5";
    size_t m = 16384, n = 16384;
    unsigned secs = 30, sp_milli = 300;
    uint64_t seed = 1;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--tag")) tag = argv[i + 1];
        else if (!strcmp(argv[i], "--rz")) rz = argv[i + 1];
        else if (!strcmp(argv[i], "--m")) m = strtoull(argv[i + 1], NULL, 10);
        else if (!strcmp(argv[i], "--n")) n = strtoull(argv[i + 1], NULL, 10);
        else if (!strcmp(argv[i], "--cpus")) cpus = argv[i + 1];
        else if (!strcmp(argv[i], "--secs")) secs = (unsigned)strtoul(argv[i + 1], NULL, 10);
        else if (!strcmp(argv[i], "--seed")) seed = strtoull(argv[i + 1], NULL, 10);
        else if (!strcmp(argv[i], "--sparsity-milli")) sp_milli = (unsigned)strtoul(argv[i + 1], NULL, 10);
        else die("unknown argument");
    }
    const oma_rz_impl *im = oma_rz_find(rz);
    if (!im) die("unknown realization id");
    int cpu[TYW_MAX_CPUS], ncpu = 0;
    for (const char *p = cpus; *p && ncpu < TYW_MAX_CPUS;) {
        char *end;
        long c = strtol(p, &end, 10);
        if (end == p || c < 0) die("bad --cpus");
        cpu[ncpu++] = (int)c;
        p = *end == ',' ? end + 1 : end;
    }
    if (ncpu == 0 || secs == 0) die("need cpus and secs");

    /* Canonical weights and input, generated once from the seed. */
    int8_t *w = malloc(m * n);
    int8_t *x = malloc(n);
    if (!w || !x) die("out of memory");
    tyw_rng = seed;
    for (size_t i = 0; i < m * n; i++) {
        uint64_t r = tyw_next();
        w[i] = (int8_t)((r % 1000) < sp_milli ? 0 : ((r >> 32) & 1 ? 1 : -1));
    }
    for (size_t i = 0; i < n; i++) x[i] = (int8_t)(tyw_next() & 0xff);

    /* Oracle check once, then one plan per thread (packed on that thread's cpu
     * so first touch lands where it will be read). */
    int32_t *yo = malloc(m * sizeof *yo);
    if (!yo || oma_rz_oracle(w, m, n, x, yo) != OMA_RZ_OK) die("oracle failed");
    tyw_thread th[TYW_MAX_CPUS];
    memset(th, 0, sizeof th);
    int oracle_ok = 1;
    for (int k = 0; k < ncpu; k++) {
        th[k].im = im;
        th[k].cpu = cpu[k];
        pin_to(cpu[k]);
        if (im->pack(&th[k].plan, w, m, n) != OMA_RZ_OK) die("pack failed");
        th[k].x = x;
        th[k].y = calloc(m, sizeof(int32_t));
        if (!th[k].y) die("out of memory");
        im->run(&th[k].plan, x, th[k].y);
        if (memcmp(th[k].y, yo, m * sizeof *yo) != 0) oracle_ok = 0;
    }
    free(w);
    free(yo);
    if (!oracle_ok) die("realization disagrees with oracle");

    R15Pmu pmu;
    char why[256] = "";
    int pmu_ok = r15_pmu_open(&pmu, why, sizeof why) == 0;
    printf("READY %d %zu %zu\n", (int)getpid(), th[0].plan.weight_bytes, th[0].plan.footprint_bytes);
    fflush(stdout);

    char go;
    if (read(0, &go, 1) != 1) die("no GO byte");
    if (pmu_ok) r15_pmu_start(&pmu);
    uint64_t t_go = r15_now_ns(), deadline = t_go + (uint64_t)secs * 1000000000ull;
    pthread_t tid[TYW_MAX_CPUS];
    for (int k = 0; k < ncpu; k++) {
        th[k].deadline_ns = deadline;
        if (pthread_create(&tid[k], NULL, worker, &th[k]) != 0) die("pthread_create");
    }
    for (int k = 0; k < ncpu; k++) pthread_join(tid[k], NULL);
    R15PmuRead pr;
    memset(&pr, 0, sizeof pr);
    if (pmu_ok) r15_pmu_stop(&pmu, &pr);
    uint64_t t_done = r15_now_ns();

    uint64_t calls = 0, t0 = th[0].t0_ns, t1 = th[0].t1_ns;
    int pins = 1;
    for (int k = 0; k < ncpu; k++) {
        calls += th[k].calls;
        if (th[k].t0_ns < t0) t0 = th[k].t0_ns;
        if (th[k].t1_ns > t1) t1 = th[k].t1_ns;
        pins &= th[k].pin_ok;
    }
    printf("{\"tag\":\"%s\",\"rz\":\"%s\",\"m\":%zu,\"n\":%zu,\"sparsity_milli\":%u,\"seed\":%" PRIu64
           ",\"cpus\":\"%s\",\"threads\":%d,\"pinned\":%d,\"oracle_ok\":%d,\"weight_bytes\":%zu"
           ",\"planned_ns\":%" PRIu64 ",\"go_ns\":%" PRIu64 ",\"t0_ns\":%" PRIu64 ",\"t1_ns\":%" PRIu64
           ",\"done_ns\":%" PRIu64 ",\"calls\":%" PRIu64 ",\"thread_calls\":[",
           tag, rz, m, n, sp_milli, seed, cpus, ncpu, pins, oracle_ok, th[0].plan.weight_bytes,
           (uint64_t)((uint64_t)secs * 1000000000u), t_go, t0, t1, t_done, calls);
    for (int k = 0; k < ncpu; k++) printf("%s%" PRIu64, k ? "," : "", th[k].calls);
    printf("],\"pmu_open\":%d,\"pmu_why\":\"%s\"", pmu_ok, pmu_ok ? "" : why);
    if (pmu_ok) r15_json_pmu(stdout, "pmu", &pmu, &pr);
    printf("}\n");
    fflush(stdout);
    if (pmu_ok) r15_pmu_close(&pmu);
    for (int k = 0; k < ncpu; k++) {
        oma_rz_free(&th[k].plan);
        free(th[k].y);
    }
    free(x);
    return 0;
}
