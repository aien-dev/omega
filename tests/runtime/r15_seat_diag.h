/* r15_seat_diag.h -- per-sample diagnostics for the 1 ms GPU-seat residency
 * sampler (spec/r15-performance-proof.md section 6.10, gate G15).
 *
 * G15 counts a sample "live" when the seat's heartbeat word moved since the
 * previous 1 ms sample. A not-live sample can mean the seat really stopped
 * (residency loss), or that the sampler ran late or not at all, or that the
 * heartbeat read was stale. The counts alone cannot tell these apart, which is
 * why CAND-1 window 2 (trial SEQ-06, 16348 of 19253 live) could only be
 * explained from outside the evidence. These fields travel with the existing
 * "sampler"/"total" objects (additive keys; seat_live and intervals, and so
 * G15, are computed exactly as before).
 *
 * Wake-up lateness is the time between consecutive counted samples (nominal
 * 1 ms plus the read); a starved sampler shows large gaps AND fewer samples,
 * a stopped seat shows a long run of not-live samples with normal gaps.
 */
#ifndef R15_SEAT_DIAG_H
#define R15_SEAT_DIAG_H
#include <stdint.h>

typedef struct {
    uint64_t last_t, last_change_t;         /* monotonic ns of previous sample / last heartbeat move */
    uint64_t late_2ms, late_10ms, max_gap_ns;
    uint64_t dead_runs, dead_longest, dead_cur;   /* runs of consecutive not-live samples */
    uint64_t first_dead_t, last_live_t;     /* monotonic ns; 0 = never */
    uint64_t stale_max_ns;                  /* longest time the heartbeat had not moved, seen at a sample */
    uint64_t t_first;                       /* time of the first counted sample */
    uint32_t hb_last;
    int have;
} SeatDiag;

static inline void seat_diag_sample(SeatDiag *d, uint64_t t_ns, int live, uint32_t hb) {
    if (!d->have) { d->have = 1; d->t_first = t_ns; d->last_change_t = t_ns; }
    else {
        uint64_t gap = t_ns - d->last_t;
        if (gap > d->max_gap_ns) d->max_gap_ns = gap;
        if (gap > 2000000u) d->late_2ms++;
        if (gap > 10000000u) d->late_10ms++;
    }
    d->last_t = t_ns;
    d->hb_last = hb;
    if (live) {
        d->last_change_t = t_ns;
        d->last_live_t = t_ns;
        d->dead_cur = 0;
    } else {
        if (d->dead_cur == 0) {
            d->dead_runs++;
            if (!d->first_dead_t) d->first_dead_t = t_ns;
        }
        d->dead_cur++;
        if (d->dead_cur > d->dead_longest) d->dead_longest = d->dead_cur;
        uint64_t age = t_ns - d->last_change_t;
        if (age > d->stale_max_ns) d->stale_max_ns = age;
    }
}
#endif
