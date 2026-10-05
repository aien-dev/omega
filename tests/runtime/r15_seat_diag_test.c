/* Host-only test of the G15 sampler diagnostics (r15_seat_diag.h). Feeds
 * synthetic sample streams and checks the diagnostics tell a stopped seat
 * from a late sampler from a healthy run. No chip, no threads. */
#define _POSIX_C_SOURCE 200809L
#include "r15_seat_diag.h"
#include "r15_stage_clock.h"
#include <string.h>
#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("[-] line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void) {
    SeatDiag d;
    uint64_t t;

    /* healthy: 1000 samples 1.05 ms apart, heartbeat moves every time */
    d = (SeatDiag){0}; t = 1000000000ull;
    for (int i = 0; i < 1000; i++, t += 1050000u) seat_diag_sample(&d, t, i > 0, (uint32_t)i);
    CHECK(d.dead_runs == 1 && d.dead_longest == 1);   /* only the first sample has no predecessor to compare */
    CHECK(d.late_2ms == 0 && d.late_10ms == 0 && d.max_gap_ns == 1050000u);

    /* seat stops: 600 live then 400 not live, sampler on time (the SEQ-06 shape) */
    d = (SeatDiag){0}; t = 1000000000ull;
    for (int i = 0; i < 1000; i++, t += 1050000u) seat_diag_sample(&d, t, i >= 1 && i < 600, 7u);
    CHECK(d.dead_runs == 2 && d.dead_longest == 400 && d.dead_cur == 400);
    CHECK(d.late_10ms == 0 && d.max_gap_ns == 1050000u);
    CHECK(d.first_dead_t == 1000000000ull);              /* the opening sample */
    CHECK(d.last_live_t == 1000000000ull + 599u * 1050000ull);
    CHECK(d.stale_max_ns == 399u * 1050000ull + 1050000ull);

    /* starved sampler, seat fine: every sample live but 50 ms apart */
    d = (SeatDiag){0}; t = 1000000000ull;
    for (int i = 0; i < 100; i++, t += 50000000ull) seat_diag_sample(&d, t, i > 0, (uint32_t)i);
    CHECK(d.late_10ms == 99 && d.late_2ms == 99 && d.max_gap_ns == 50000000ull);
    CHECK(d.dead_longest == 1);

    /* scattered: a not-live sample every 10th, never two in a row */
    d = (SeatDiag){0}; t = 1000000000ull;
    for (int i = 0; i < 1000; i++, t += 1050000u) seat_diag_sample(&d, t, i > 0 && i % 10 != 0, 1u);
    CHECK(d.dead_runs == 100 && d.dead_longest == 1 && d.late_10ms == 0);

    /* stage clock: stamps become per-stage deltas that sum to the total; an unstamped clock writes nothing */
    {
        R15StageClock c; char b[512];
        memset(&c, 0, sizeof c);
        CHECK(r15_stage_json(&c, b, sizeof b) == 0);
        c.t[0] = 1000; c.name[0] = "begin"; c.n = 1;
        CHECK(r15_stage_json(&c, b, sizeof b) == 0);
        c.t[1] = 1500; c.name[1] = "producers"; c.t[2] = 102000001500ull; c.name[2] = "seat_finish"; c.n = 3;
        int w = r15_stage_json(&c, b, sizeof b);
        CHECK(w > 0 && strcmp(b, ",\"teardown\":{\"total_ns\":102000000500,\"stages\":[[\"producers\",500],[\"seat_finish\",102000000000]]}") == 0);
        R15StageClock f; memset(&f, 0, sizeof f);
        for (int i = 0; i < R15_STAGE_MAX + 5; i++) r15_stage_mark(&f, "x");
        CHECK(f.n == R15_STAGE_MAX);
        CHECK(r15_stage_json(&f, b, 40) == 0);   /* too small a buffer is refused, never truncated */
    }

    printf("failures %d\n", fails);
    return fails != 0;
}
