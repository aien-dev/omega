/* EST-3c (protocol v3) declared CPU load schedule.
 *
 *   est_load <seed> <seconds> <marks-file>
 *
 * Generates a deterministic schedule of segments from <seed> (splitmix64):
 * level in {0, 6, 12, 18} busy threads (uniform), duration uniform in
 * [20, 120] s. A level-0 segment is "idle"; a segment with level > 0 is a
 * load trial and is written to <marks-file> in the R15 marks shape:
 *   <t s.ns> begin load-L<level>-<index>
 *   <t s.ns> end load-L<level>-<index> exit 0
 * Busy threads spin on integer arithmetic only (no memory traffic of note),
 * so the load is CPU heat, nothing else. The schedule is printed to stdout
 * before it starts ("schedule <index> <level> <seconds>"), so it is part of
 * the record and can be re-derived from the seed alone.
 *
 * No Python, no root, no files besides <marks-file> and stdout. Signals: on
 * SIGTERM/SIGINT the current trial gets its end mark and the program exits. */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define MAX_THREADS 18
#define MAX_SEGS 4096

static atomic_int g_level;   /* threads with index < level spin */
static atomic_int g_quit;
static volatile sig_atomic_t g_stop;

static uint64_t splitmix64(uint64_t *s)
{
    uint64_t z = (*s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static void *spin(void *arg)
{
    int idx = (int)(intptr_t)arg;
    volatile uint64_t x = (uint64_t)idx + 1u;
    struct timespec nap = {0, 20000000}; /* 20 ms when parked */
    while (!atomic_load(&g_quit)) {
        if (idx < atomic_load(&g_level)) {
            for (int i = 0; i < 1000000; i++)
                x = x * 6364136223846793005ull + 1442695040888963407ull;
        } else {
            nanosleep(&nap, NULL);
        }
    }
    return NULL;
}

static void on_sig(int s) { (void)s; g_stop = 1; }

static void now(char *buf, size_t n)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(buf, n, "%lld.%09ld", (long long)ts.tv_sec, ts.tv_nsec);
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: est_load <seed> <seconds> <marks-file>\n");
        return 2;
    }
    uint64_t seed = strtoull(argv[1], NULL, 0);
    long total = strtol(argv[2], NULL, 10);
    if (total <= 0) { fprintf(stderr, "est_load: seconds must be > 0\n"); return 2; }
    static const int levels[4] = {0, 6, 12, 18};
    int seg_level[MAX_SEGS];
    long seg_secs[MAX_SEGS];
    int nseg = 0;
    long acc = 0;
    uint64_t s = seed;
    while (acc < total && nseg < MAX_SEGS) {
        int lv = levels[splitmix64(&s) % 4u];
        long d = 20 + (long)(splitmix64(&s) % 101u);
        if (acc + d > total) d = total - acc;
        seg_level[nseg] = lv;
        seg_secs[nseg] = d;
        acc += d;
        nseg++;
    }
    printf("est_load seed %llu seconds %ld segments %d\n", (unsigned long long)seed, total, nseg);
    for (int i = 0; i < nseg; i++)
        printf("schedule %d %d %ld\n", i, seg_level[i], seg_secs[i]);
    fflush(stdout);

    FILE *m = fopen(argv[3], "a");
    if (!m) { perror("est_load: marks"); return 1; }
    struct sigaction sa = {0};
    sa.sa_handler = on_sig;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    pthread_t th[MAX_THREADS];
    for (int i = 0; i < MAX_THREADS; i++)
        if (pthread_create(&th[i], NULL, spin, (void *)(intptr_t)i) != 0) {
            fprintf(stderr, "est_load: pthread_create failed\n");
            return 1;
        }
    char tb[64];
    for (int i = 0; i < nseg && !g_stop; i++) {
        if (seg_level[i] > 0) {
            now(tb, sizeof tb);
            fprintf(m, "%s begin load-L%d-%03d\n", tb, seg_level[i], i);
            fflush(m);
        }
        atomic_store(&g_level, seg_level[i]);
        struct timespec end, cur;
        clock_gettime(CLOCK_MONOTONIC, &end);
        end.tv_sec += seg_secs[i];
        for (;;) {
            if (g_stop) break;
            clock_gettime(CLOCK_MONOTONIC, &cur);
            if (cur.tv_sec > end.tv_sec || (cur.tv_sec == end.tv_sec && cur.tv_nsec >= end.tv_nsec))
                break;
            struct timespec nap = {0, 100000000};
            nanosleep(&nap, NULL);
        }
        atomic_store(&g_level, 0);
        if (seg_level[i] > 0) {
            now(tb, sizeof tb);
            fprintf(m, "%s end load-L%d-%03d exit 0\n", tb, seg_level[i], i);
            fflush(m);
        }
    }
    atomic_store(&g_quit, 1);
    for (int i = 0; i < MAX_THREADS; i++) pthread_join(th[i], NULL);
    fclose(m);
    printf("est_load done%s\n", g_stop ? " (stopped by signal)" : "");
    return 0;
}
