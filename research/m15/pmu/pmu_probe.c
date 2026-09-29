/* Probe: per-thread, inherited counters opened on both GB10 CPU PMUs.
 * A worker thread created after the counters runs a fixed loop on an A725
 * core, then the same loop on an X925 core. Prints per-PMU counts and the
 * enabled/running times (multiplexing). Read-only; changes no setting. */
#define _GNU_SOURCE
#include <linux/perf_event.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

static const uint64_t ev[] = { 0x11, 0x08, 0x19, 0x37, 0x17, 0x13 };
static const char *nm[] = { "cpu_cycles", "inst_retired", "bus_access", "ll_cache_miss_rd",
                            "l2d_cache_refill", "mem_access" };
static volatile uint64_t sink;

static void pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    sched_setaffinity(0, sizeof s, &s);
}
static void work(void) {
    uint64_t x = 1;
    for (uint64_t i = 0; i < 200000000ull; i++) x = x * 6364136223846793005ull + i;
    sink = x;
}
static void *worker(void *a) {
    (void)a;
    pin(1); work(); int c1 = sched_getcpu();
    pin(6); work(); int c2 = sched_getcpu();
    printf("worker ran on cpu %d then cpu %d\n", c1, c2);
    return NULL;
}

int main(void) {
    int types[2] = { 10, 11 }, fd[2][6];
    for (int t = 0; t < 2; t++)
        for (int i = 0; i < 6; i++) {
            struct perf_event_attr a;
            memset(&a, 0, sizeof a);
            a.size = sizeof a;
            a.type = types[t];
            a.config = ev[i];
            a.disabled = 1;
            a.inherit = 1;
            a.exclude_hv = 1;
            a.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
            fd[t][i] = (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
            if (fd[t][i] < 0) { perror("perf_event_open"); return 1; }
        }
    for (int t = 0; t < 2; t++) for (int i = 0; i < 6; i++) ioctl(fd[t][i], PERF_EVENT_IOC_ENABLE, 0);
    pin(0);
    pthread_t th;
    pthread_create(&th, NULL, worker, NULL);
    pthread_join(th, NULL);
    for (int t = 0; t < 2; t++) for (int i = 0; i < 6; i++) ioctl(fd[t][i], PERF_EVENT_IOC_DISABLE, 0);
    for (int i = 0; i < 6; i++) {
        uint64_t v[2][3];
        for (int t = 0; t < 2; t++) {
            if (read(fd[t][i], v[t], sizeof v[t]) != sizeof v[t]) { perror("read"); return 1; }
        }
        printf("%-17s A725=%-12llu X925=%-12llu sum=%-12llu run/en A=%.4f X=%.4f\n", nm[i],
               (unsigned long long)v[0][0], (unsigned long long)v[1][0],
               (unsigned long long)(v[0][0] + v[1][0]),
               v[0][1] ? (double)v[0][2] / v[0][1] : 0, v[1][1] ? (double)v[1][2] / v[1][1] : 0);
    }
    return 0;
}
