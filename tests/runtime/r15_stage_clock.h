/* r15_stage_clock.h -- named time stamps for the teardown after a trial's
 * measured windows (spec/r15-performance-proof.md section 6.10, gate G15).
 *
 * CAND-1 window 2: trials SEQ-06 and RES4-11 each took 122.7 s against about
 * 21.6 s. The record showed the episode ended on time (20.4 s) and the whole
 * extra ~102 s sat between the residency record and the result record, which is
 * r15_stop() (producers, transport, orchestrator, seat shutdown and finish,
 * world destroy). The record could not say which step waited. These stamps are
 * taken AFTER the measured windows and the residency sampler are closed, so
 * they add nothing to what G15 or any other gate measures. */
#ifndef R15_STAGE_CLOCK_H
#define R15_STAGE_CLOCK_H
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define R15_STAGE_MAX 12

typedef struct {
    uint64_t t[R15_STAGE_MAX];
    const char *name[R15_STAGE_MAX];
    int n;
} R15StageClock;

static inline uint64_t r15_stage_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Stamp "the step called name has just finished". The first stamp (n == 0) is
 * the start reference. A full clock drops further stamps and never overruns. */
static inline void r15_stage_mark(R15StageClock *c, const char *name) {
    if (c->n >= R15_STAGE_MAX) return;
    c->t[c->n] = r15_stage_now_ns();
    c->name[c->n] = name;
    c->n++;
}

/* Writes ,"teardown":{"total_ns":T,"stages":[["name",ns since the previous stamp],...]}.
 * Returns the number of bytes written; 0 if nothing was stamped. */
static inline int r15_stage_json(const R15StageClock *c, char *buf, size_t cap) {
    if (c->n < 2 || cap < 64) return 0;
    int w = snprintf(buf, cap, ",\"teardown\":{\"total_ns\":%llu,\"stages\":[",
                     (unsigned long long)(c->t[c->n - 1] - c->t[0]));
    for (int i = 1; i < c->n && w > 0 && (size_t)w < cap; i++)
        w += snprintf(buf + w, cap - (size_t)w, "%s[\"%s\",%llu]", i > 1 ? "," : "", c->name[i],
                      (unsigned long long)(c->t[i] - c->t[i - 1]));
    if (w > 0 && (size_t)w + 3 < cap) w += snprintf(buf + w, cap - (size_t)w, "]}");
    return (w > 0 && (size_t)w < cap) ? w : 0;
}
#endif
