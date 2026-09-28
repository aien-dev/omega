/* rx_seq_reference.c -- R15 sequential control. REFERENCE ORACLE ONLY.
 * See rx_seq_reference.h. */
#include "rx_seq_reference.h"

#include <sched.h>
#include <time.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Poll one stage: has any field it declares as a trigger moved past what it
 * last saw? The cause is the writer of the newest such field. Caller holds
 * the world lock. */
static int poll_ready(RxWorld *w, RxReaction *r, uint64_t *cause) {
    int ready = r->seq_pending;
    uint64_t newest = 0;
    *cause = r->seq_pending ? r->wake_cause : 0;
    for (uint32_t t = 0; t < r->desc.n_triggers; t++) {
        const RxDep *d = &r->desc.triggers[t];
        const RxObject *o = &w->objects[d->obj.id];
        if (!o->live || o->generation != d->obj.generation) continue;
        uint64_t seen = r->seq_seen[t];
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++) {
            if (!(d->mask & RX_FIELD(f))) continue;
            uint64_t v = o->field_version[f];
            if (v > r->seq_seen[t]) r->seq_seen[t] = v;
            if (v > seen) {
                ready = 1;
                if (v > newest) { newest = v; *cause = o->field_writer[f]; }
            }
        }
    }
    return ready;
}

int rx_seq_pulse(RxWorld *w, const RxSeqPlan *plan, uint32_t *ran) {
    if (!w || !w->sequential || !plan) return RX_ERR_ARG;
    uint32_t n_ran = 0;
    int rc = RX_OK;
    pthread_mutex_lock(&w->mu);
    uint64_t pulse = ++w->seq_pulse_started;
    w->stats.seq_pulses++;
    for (uint32_t i = 0; i < plan->n && rc == RX_OK; i++) {
        uint32_t rid = plan->order[i];
        if (rid >= w->n_reactions) continue;
        RxReaction *r = &w->reactions[rid];
        w->stats.seq_polls++;
        if (r->state != RX_DORMANT) continue;
        uint64_t cause = 0;
        if (!poll_ready(w, r, &cause)) continue;
        if (rx_world_seq_activate_locked(w, rid, cause) != 1) continue;
        n_ran++;
        w->stats.seq_runs++;
        if (!(r->resident_seat && r->state == RX_RUNNING)) continue;
        /* Dispatch GPU, wait. */
        w->stats.gpu_host_waits++;
        uint64_t limit = now_ns() + (uint64_t)(plan->gpu_timeout_ms ? plan->gpu_timeout_ms
                                                                      : 5000) * 1000000ull;
        while (r->state != RX_DORMANT) {
            pthread_mutex_unlock(&w->mu);
            int g = plan->gpu_step ? plan->gpu_step(plan->gpu_ctx) : RX_ERR_ARG;
            if (g == 0) sched_yield();
            pthread_mutex_lock(&w->mu);
            if (g < 0 && g != RX_ERR_NOT_FOUND) { rc = g; break; }
            if (now_ns() > limit) { rc = RX_ERR_TIMEOUT; break; }
        }
    }
    if (n_ran == 0 && rc == RX_OK) {
        w->seq_idle_start = pulse;
        pthread_cond_broadcast(&w->idle_cv);
    }
    pthread_mutex_unlock(&w->mu);
    if (ran) *ran = n_ran;
    return rc;
}

int rx_seq_run_until_complete(RxWorld *w, const RxSeqPlan *plan, uint32_t max_pulses,
                              uint32_t *pulses) {
    uint32_t p = 0;
    int rc = RX_OK;
    for (; p < max_pulses; p++) {
        uint32_t ran = 0;
        rc = rx_seq_pulse(w, plan, &ran);
        if (rc != RX_OK || ran == 0) { p++; break; }
    }
    if (pulses) *pulses = p;
    return rc;
}
