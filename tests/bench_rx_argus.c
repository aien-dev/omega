/*
 * bench_rx_argus.c -- ARGUS_PERFORMANCE_GATE micro-benchmark (CPU only, no GPU).
 *
 * One op = one AEGIS policy evaluation (rx_aegis_evaluate, the pure decide
 * core) + one authority validate on the reaction path (rx_world_validate_cap
 * on a world bound to the native AIENOS view, i.e. rx_native_bind's
 * native_validate, which is where the ARGUS CAPABILITY_USED emit sits).
 * Built identically for RX_ARGUS=0/1/2; the consumer mode for RX_ARGUS=2 is
 * chosen at run time by RX_ARGUS_CONSUMER=discard|ingest (rx_argus.c).
 *
 * Phases:
 *   throughput  N ops, one clock read around the whole loop -> ops/s
 *   latency     S ops, each timed with CLOCK_MONOTONIC -> p50/p99 (includes
 *               the clock read overhead, reported separately)
 *   emit        RX_ARGUS>=1: rx_argus_emit_cap_used alone, each call timed;
 *               with the native view (tick = aienos_cap_clock, a mutex) and
 *               without (tick 0)
 *
 *   bench_rx_argus [N] [S]
 */
#include "runtime/aienos_cap.h"
#include "runtime/rx_aegis.h"
#include "runtime/rx_argus.h"
#include "runtime/rx_world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SUBJ 7u
#define RES  0x5810100ull
#define BENCH_RIGHTS 0x1u

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static uint64_t pct(uint64_t *v, size_t n, double p) {
    qsort(v, n, sizeof *v, cmp_u64);
    size_t i = (size_t)(p * (double)(n - 1));
    return v[i];
}

static RxWorld g_world;

int main(int argc, char **argv) {
    size_t n_ops = argc > 1 ? strtoull(argv[1], NULL, 10) : 2000000u;
    size_t n_lat = argc > 2 ? strtoull(argv[2], NULL, 10) : 200000u;

    AienosCapAdmin *admin;
    AienosCapView *view;
    if (aienos_cap_start(&admin, &view) != 0) { fprintf(stderr, "authority start\n"); return 1; }
    AienosCapRef office, cap;
    aienos_cap_office(admin, &office);
    AienosCapMint m = { RX_AEGIS_SUBJ, SUBJ, RES, BENCH_RIGHTS, 0, { UINT32_MAX, 0 }, office };
    if (aienos_cap_mint(admin, &m, &cap) != 0) { fprintf(stderr, "mint\n"); return 1; }
    /* The bench is the harness that minted: tell ARGUS, so the shadow knows the grant. */
    RX_ARGUS_EMIT(rx_argus_emit_cap_granted(view, SUBJ, cap.cap_id, (uint64_t)cap.generation,
                                            RES, BENCH_RIGHTS, 0));
    if (rx_world_init_native(&g_world, view, 1, 1u << 12) != RX_OK) {
        fprintf(stderr, "world init\n");
        return 1;
    }
    RxAegisPolicy pol;
    memset(&pol, 0, sizeof pol);
    pol.n_rules = 1;
    pol.rules[0] = (RxAegisRule){ 1, SUBJ, RES, RES + 0xff, BENCH_RIGHTS, 0, 0 };
    RxCapRef ref = { cap.cap_id, cap.generation };

    volatile uint64_t sink = 0;
    /* warm-up */
    for (size_t i = 0; i < 100000; i++) {
        uint64_t l; uint32_t why, na;
        sink += rx_aegis_evaluate(&pol, SUBJ, RES + (i & 7), BENCH_RIGHTS, 0, &l, &why, &na);
        sink += (uint64_t)rx_world_validate_cap(&g_world, ref, SUBJ, RES, BENCH_RIGHTS, NULL);
    }

    /* throughput */
    uint64_t t0 = now_ns();
    for (size_t i = 0; i < n_ops; i++) {
        uint64_t l; uint32_t why, na;
        sink += rx_aegis_evaluate(&pol, SUBJ, RES + (i & 7), BENCH_RIGHTS, 0, &l, &why, &na);
        sink += (uint64_t)rx_world_validate_cap(&g_world, ref, SUBJ, RES, BENCH_RIGHTS, NULL);
    }
    uint64_t t1 = now_ns();
    double ops_s = (double)n_ops * 1e9 / (double)(t1 - t0);

    /* latency */
    uint64_t *lat = malloc(n_lat * sizeof *lat);
    uint64_t *clk = malloc(n_lat * sizeof *clk);
    for (size_t i = 0; i < n_lat; i++) {
        uint64_t a = now_ns();
        uint64_t b = now_ns();
        clk[i] = b - a;
    }
    for (size_t i = 0; i < n_lat; i++) {
        uint64_t l; uint32_t why, na;
        uint64_t a = now_ns();
        sink += rx_aegis_evaluate(&pol, SUBJ, RES + (i & 7), BENCH_RIGHTS, 0, &l, &why, &na);
        sink += (uint64_t)rx_world_validate_cap(&g_world, ref, SUBJ, RES, BENCH_RIGHTS, NULL);
        lat[i] = now_ns() - a;
    }
    uint64_t lat50 = pct(lat, n_lat, 0.50), lat99 = pct(lat, n_lat, 0.99);
    uint64_t clk50 = pct(clk, n_lat, 0.50);

    printf("{ \"rx_argus\": %d, \"ops\": %zu, \"ops_per_s\": %.0f, \"op_p50_ns\": %llu, "
           "\"op_p99_ns\": %llu, \"clock_read_p50_ns\": %llu",
           RX_ARGUS, n_ops, ops_s, (unsigned long long)lat50, (unsigned long long)lat99,
           (unsigned long long)clk50);

#if RX_ARGUS
    RxArgusStats before;
    rx_argus_stats(&before);
    /* emit alone, with and without the authority clock read */
    uint64_t e50[2], e99[2];
    for (int v = 0; v < 2; v++) {
        for (size_t i = 0; i < n_lat; i++) {
            uint64_t a = now_ns();
            rx_argus_emit_cap_used(v == 0 ? view : NULL, SUBJ, cap.cap_id, (uint64_t)cap.generation,
                                   RES, BENCH_RIGHTS, 0);
            lat[i] = now_ns() - a;
        }
        e50[v] = pct(lat, n_lat, 0.50);
        e99[v] = pct(lat, n_lat, 0.99);
    }
    RxArgusStats s;
    rx_argus_stats(&s);
    printf(", \"emit_p50_ns\": %llu, \"emit_p99_ns\": %llu, \"emit_noclock_p50_ns\": %llu, "
           "\"emit_noclock_p99_ns\": %llu, \"bytes_per_event\": %u",
           (unsigned long long)e50[0], (unsigned long long)e99[0], (unsigned long long)e50[1],
           (unsigned long long)e99[1], ARGUS_EVENT_SIZE);
    /* ops-phase ring outcome (before the emit phase) and overall */
    printf(", \"consumer_mode\": %d, \"ops_phase_emitted\": %llu, \"ops_phase_pushed\": %llu, "
           "\"ops_phase_refused\": %llu, \"lock_dropped\": %llu, \"total_emitted\": %llu, "
           "\"total_refused\": %llu",
           s.consumer_mode, (unsigned long long)before.emitted, (unsigned long long)before.pushed,
           (unsigned long long)before.ring_refused, (unsigned long long)s.lock_dropped,
           (unsigned long long)s.emitted, (unsigned long long)s.ring_refused);
    rx_argus_shutdown();
    rx_argus_stats(&s);
    printf(", \"received\": %llu, \"lag_max\": %u, \"findings\": %llu, \"ring_bytes\": %zu, "
           "\"core_bytes\": %zu, \"findings_bytes\": %zu",
           (unsigned long long)s.received, s.lag_max, (unsigned long long)s.findings_total,
           s.ring_bytes, s.core_bytes, s.findings_bytes);
#endif
    printf(", \"sink\": %llu }\n", (unsigned long long)(sink & 1));
    rx_world_destroy(&g_world);
    aienos_cap_stop(admin, view);
    free(lat);
    free(clk);
    return 0;
}
