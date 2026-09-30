/*
 * rx_world.c -- resident reaction runtime, simple human reference. See rx_world.h.
 */
#include "rx_world.h"
#include "rx_argus.h"
#include "sha256.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <time.h>

/* ---- helpers ------------------------------------------------------------ */

/* The causal log is one address range reserved for crumb_cap records and
 * backed only as records are written (MAP_NORESERVE; zero pages until
 * touched). Records never move, so a crumb pointer stays valid for the life
 * of the world, and a large capacity costs nothing until an episode uses it.
 * The log is still bounded: a full log refuses the publication (no action
 * without evidence) and counts crumb_overflow. */
static RxCrumb *crumb_log_map(uint64_t cap) {
    if (cap > UINT64_MAX / sizeof(RxCrumb)) return NULL;
    void *p = mmap(NULL, (size_t)cap * sizeof(RxCrumb), PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : (RxCrumb *)p;
}

static void crumb_log_unmap(RxCrumb *log, uint64_t cap) {
    if (log) munmap(log, (size_t)cap * sizeof(RxCrumb));
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* CPU time consumed by the calling thread (R15 scheduler CPU, spec §6.4). */
static uint64_t thread_cpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void put32(sha256_ctx *c, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    sha256_update(c, b, 4);
}

static void put64(sha256_ctx *c, uint64_t v) {
    put32(c, (uint32_t)v);
    put32(c, (uint32_t)(v >> 32));
}

static void object_digest(RxObject *o) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)"AIEN_RX_OBJECT_V1", 17);
    put32(&c, o->type);
    for (uint32_t f = 0; f < RX_MAX_FIELDS; f++) put64(&c, o->field[f]);
    sha256_final(&c, o->digest);
}

/* ---- lifecycle ---------------------------------------------------------- */

bool rx_state_transition_legal(RxState from, RxState to) {
    switch (from) {
    case RX_DORMANT:
        return to == RX_READY || to == RX_BLOCKED_RESOURCE;
    case RX_READY:
        return to == RX_RUNNING || to == RX_BLOCKED_AUTHORITY ||
               to == RX_BLOCKED_RESOURCE || to == RX_INVALIDATED;
    case RX_RUNNING:
        /* RUNNING -> READY only when a deferred effect is resumed
         * (RX_FN_DEFER, rx_world_resume): the same admitted activation runs
         * again. */
        return to == RX_PUBLISHING || to == RX_FAILED || to == RX_CANCELLED ||
               to == RX_INVALIDATED || to == RX_READY;
    case RX_PUBLISHING:
        /* Stale input is detected while validating the publication, so the
         * explicit exit is PUBLISHING -> INVALIDATED. */
        return to == RX_COMMITTED || to == RX_CONFLICT || to == RX_REJECTED ||
               to == RX_INVALIDATED;
    case RX_BLOCKED_RESOURCE:
        return to == RX_DORMANT || to == RX_READY;
    case RX_COMMITTED: case RX_INVALIDATED: case RX_FAILED: case RX_CANCELLED:
    case RX_CONFLICT: case RX_REJECTED: case RX_BLOCKED_AUTHORITY:
        return to == RX_DORMANT;
    default:
        return false;
    }
}

const char *rx_state_name(RxState s) {
    static const char *names[RX_STATE_COUNT] = {
        "DORMANT", "READY", "BLOCKED_AUTHORITY", "BLOCKED_RESOURCE", "RUNNING",
        "PUBLISHING", "COMMITTED", "INVALIDATED", "FAILED", "CANCELLED", "CONFLICT",
        "REJECTED"
    };
    return (unsigned)s < RX_STATE_COUNT ? names[s] : "?";
}

const char *rx_crumb_kind_name(RxCrumbKind k) {
    switch (k) {
    case RX_CRUMB_CREATE: return "CREATE";
    case RX_CRUMB_EXTERNAL: return "EXTERNAL";
    case RX_CRUMB_COMMIT: return "COMMIT";
    case RX_CRUMB_INVALIDATED: return "INVALIDATED";
    case RX_CRUMB_BLOCKED_AUTHORITY: return "BLOCKED_AUTHORITY";
    case RX_CRUMB_REJECTED: return "REJECTED";
    case RX_CRUMB_FAILED: return "FAILED";
    case RX_CRUMB_NOOP: return "NOOP";
    case RX_CRUMB_RETIRE: return "RETIRE";
    case RX_CRUMB_QUARANTINE: return "QUARANTINE";
    default: return "?";
    }
}

static void set_state(RxWorld *w, RxReaction *r, RxState to) {
    if (!rx_state_transition_legal(r->state, to)) w->stats.illegal_transitions++;
    w->stats.transitions[r->state][to]++;
    r->state = to;
}

/* ---- causal crumbs ------------------------------------------------------ */

static void crumb_hash(const RxWorld *w, const RxCrumb *k, uint8_t out[32]) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)"AIEN_RX_CAUSAL_V1", 17);
    put64(&c, k->id);
    put32(&c, (uint32_t)k->kind);
    put32(&c, k->reaction);
    put32(&c, k->faculty);
    put64(&c, k->wake_cause);
    put64(&c, k->coalesced_wakes);
    put32(&c, k->n_inputs);
    for (uint32_t i = 0; i < k->n_inputs; i++) {
        put32(&c, k->inputs[i].obj.id);
        put32(&c, k->inputs[i].obj.generation);
        put64(&c, k->inputs[i].version);
        put64(&c, k->inputs[i].mask);
    }
    put32(&c, k->n_caps);
    for (uint32_t i = 0; i < k->n_caps; i++) {
        put32(&c, k->caps[i].cap_id);
        put64(&c, k->caps[i].generation);   /* full 64-bit AIENOS generation */
        put32(&c, k->cap_issuer[i]);
    }
    put32(&c, k->n_outputs);
    for (uint32_t i = 0; i < k->n_outputs; i++) {
        put32(&c, k->outputs[i].obj.id);
        put32(&c, k->outputs[i].obj.generation);
        put64(&c, k->outputs[i].version);
        put64(&c, k->outputs[i].mask);
    }
    put32(&c, (uint32_t)k->reason);
    put32(&c, k->n_parents);
    for (uint32_t i = 0; i < k->n_parents; i++) {
        uint64_t p = k->parents[i];
        put64(&c, p);
        /* Parents always precede children, so their digests are final. */
        if (p >= 1 && p <= w->n_crumbs && p < k->id)
            sha256_update(&c, w->crumbs[p - 1].digest, 32);
    }
    /* Timing and worker placement are recorded but are not identity: the
     * same causal history must hash the same on any schedule. */
    sha256_final(&c, out);
}

static void add_parent(RxCrumb *k, uint64_t p) {
    if (p == 0) return;
    for (uint32_t i = 0; i < k->n_parents; i++)
        if (k->parents[i] == p) return;
    if (k->n_parents < RX_MAX_PARENTS) k->parents[k->n_parents++] = p;
}

/* Append a crumb; returns its id (0 if the log is full). Caller holds mu. */
static uint64_t crumb_append(RxWorld *w, RxCrumb *k) {
    if (w->n_crumbs >= w->crumb_cap) {
        w->stats.crumb_overflow++;
        return 0;
    }
    k->id = w->n_crumbs + 1;
    /* Canonical parent order: ascending. */
    for (uint32_t i = 1; i < k->n_parents; i++) {
        uint64_t v = k->parents[i];
        uint32_t j = i;
        while (j > 0 && k->parents[j - 1] > v) { k->parents[j] = k->parents[j - 1]; j--; }
        k->parents[j] = v;
    }
#ifndef RX_MEASURE_NO_CAUSAL_DIGEST
    crumb_hash(w, k, k->digest);
#else
    memset(k->digest, 0, sizeof(k->digest));
#endif
    w->stats.crumb_bytes += sizeof(*k);
    if (k->kind == RX_CRUMB_EXTERNAL) k->episode = k->id;
    else if (k->wake_cause >= 1 && k->wake_cause < k->id)
        k->episode = w->crumbs[k->wake_cause - 1].episode;
    else k->episode = 0;
    w->crumbs[w->n_crumbs++] = *k;
    if (k->reaction < w->n_reactions) w->reactions[k->reaction].last_crumb = k->id;
    return k->id;
}

/* R6 containment engaged. The crumb names the reaction, the limit and the
 * crumb that caused it (so its episode). Wakes the quarantine suppresses
 * later leave nothing, so the log stays bounded. Caller holds mu. */
static void contain(RxWorld *w, uint32_t rid, uint32_t why, uint64_t cause) {
    RxReaction *r = &w->reactions[rid];
    RxCrumb k;
    memset(&k, 0, sizeof(k));
    k.kind = RX_CRUMB_QUARANTINE;
    k.reaction = rid;
    k.faculty = r->desc.faculty;
    k.worker = UINT32_MAX;
    k.wake_cause = cause;
    add_parent(&k, cause);
    k.reason = (int32_t)why;
    k.t_start_ns = k.t_end_ns = now_ns();
    crumb_append(w, &k);
}

#ifndef RX_MEASURE_NO_CAUSAL_DIGEST
const int rx_world_causal_digest_enabled = 1;
#else
const int rx_world_causal_digest_enabled = 0;
#endif

const RxCrumb *rx_world_crumb(const RxWorld *w, uint64_t id) {
    if (id == 0 || id > w->n_crumbs) return NULL;
    return &w->crumbs[id - 1];
}

int rx_world_crumb_origin(RxWorld *w, uint64_t id, uint32_t *reaction, uint32_t *subject,
                          uint32_t *faculty) {
    pthread_mutex_lock(&w->mu);
    int rc = RX_ERR_NOT_FOUND;
    if (id != 0 && id <= w->n_crumbs) {
        const RxCrumb *c = &w->crumbs[id - 1];
        *reaction = c->reaction;
        *faculty = c->faculty;
        *subject = c->reaction < w->n_reactions ? w->reactions[c->reaction].desc.subject
                                                : w->external_subject;
        rc = c->kind == RX_CRUMB_COMMIT || c->kind == RX_CRUMB_EXTERNAL ? RX_OK
           : c->kind == RX_CRUMB_CREATE                                  ? 1
                                                                         : RX_ERR_ARG;
    }
    pthread_mutex_unlock(&w->mu);
    return rc;
}

void rx_world_crumb_digest(const RxWorld *w, const RxCrumb *k, uint8_t out[32]) {
    crumb_hash(w, k, out);
}

int rx_world_verify_crumbs(RxWorld *w, uint64_t *out_checked) {
    pthread_mutex_lock(&w->mu);
    int rc = 0;
    uint64_t n = 0;
    for (uint64_t i = 0; i < w->n_crumbs; i++) {
        const RxCrumb *k = &w->crumbs[i];
        uint8_t d[32];
        if (k->id != i + 1) { rc = -1; break; }
        for (uint32_t p = 0; p < k->n_parents; p++)
            if (k->parents[p] == 0 || k->parents[p] >= k->id) { rc = -2; break; }
        if (rc) break;
        crumb_hash(w, k, d);
        if (memcmp(d, k->digest, 32) != 0) { rc = -3; break; }
        uint64_t ep = k->kind == RX_CRUMB_EXTERNAL ? k->id
                    : k->wake_cause >= 1 && k->wake_cause < k->id
                        ? w->crumbs[k->wake_cause - 1].episode : 0;
        if (k->episode != ep) { rc = -4; break; }
        n++;
    }
    pthread_mutex_unlock(&w->mu);
    if (out_checked) *out_checked = n;
    return rc;
}

/* ---- wakeups and admission ---------------------------------------------- */

static bool cause_is_external(const RxWorld *w, uint64_t cause) {
    if (cause == 0 || cause > w->n_crumbs) return false;
    return w->crumbs[cause - 1].kind == RX_CRUMB_EXTERNAL;
}

static bool res_fits(const RxWorld *w, const RxReaction *r) {
    const RxResourceNeed *n = &r->desc.need;
    if (w->used_slots >= w->budget.slots) return false;
    if (w->used_memory > w->budget.memory_bytes) return false;
    if (w->used_energy > w->budget.energy_budget) return false;
    if (n->memory_bytes > w->budget.memory_bytes - w->used_memory) return false;
    if (n->energy_cost > w->budget.energy_budget - w->used_energy) return false;
    if (n->locality && (n->locality & w->budget.offered_locality) != n->locality) return false;
    if (n->accelerator_features &&
        (n->accelerator_features & w->budget.offered_accel) != n->accelerator_features)
        return false;
    if (n->compute_class &&
        (w->budget.compute_mask & (1u << (n->compute_class & 31u))) == 0)
        return false;
    return true;
}

static void enqueue(RxWorld *w, uint32_t rid) {
    uint32_t p = w->reactions[rid].desc.priority;
    if (p >= RX_PRIORITY_CLASSES) p = RX_PRIO_BACKGROUND;
    if (w->ready_len[p] >= RX_MAX_REACTIONS) {
        w->stats.suppressed_wakes++;
        return;
    }
    uint32_t tail = (w->ready_head[p] + w->ready_len[p]) % RX_MAX_REACTIONS;
    w->ready_q[p][tail] = rid;
    w->ready_len[p]++;
    w->stats.ready_inserts++;
    if (w->timing) w->reactions[rid].t_ready = now_ns();
    w->in_flight++;
    pthread_cond_signal(&w->work_cv);
}

static void gauge_blocked(RxWorld *w) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < w->n_reactions; i++)
        if (w->reactions[i].state == RX_BLOCKED_RESOURCE) n++;
    if (n > w->peak_blocked) w->peak_blocked = n;
}

static int better_admit(const RxReaction *a, const RxReaction *b) {
    if (a->desc.priority != b->desc.priority) return a->desc.priority < b->desc.priority;
    if (a->wait_seq != b->wait_seq) return a->wait_seq < b->wait_seq;
    return a->service < b->service;
}

static void charge(RxWorld *w, RxReaction *r) {
    r->holding = true;
    r->memory_held = r->desc.need.memory_bytes;
    r->energy_held = r->desc.need.energy_cost;
    w->used_slots++;
    w->used_memory += r->memory_held;
    w->used_energy += r->energy_held;
    r->service++;
    if (w->used_slots > w->peak_slots) w->peak_slots = w->used_slots;
    if (r->desc.need.deadline && w->budget.logical_tick > r->desc.need.deadline)
        w->stats.deadline_overdue++;
}

static void uncharge(RxWorld *w, RxReaction *r) {
    if (!r->holding) return;
    if (w->used_slots) w->used_slots--;
    if (w->used_memory >= r->memory_held) w->used_memory -= r->memory_held;
    else w->used_memory = 0;
    if (w->used_energy >= r->energy_held) w->used_energy -= r->energy_held;
    else w->used_energy = 0;
    r->holding = false;
    r->memory_held = 0;
    r->energy_held = 0;
}

static void admit_one(RxWorld *w, uint32_t rid) {
    RxReaction *r = &w->reactions[rid];
    if (r->state != RX_BLOCKED_RESOURCE || !res_fits(w, r)) return;
    set_state(w, r, RX_READY);
    charge(w, r);
    enqueue(w, rid);
}

static bool waiting_fit(const RxReaction *r, const RxWorld *w) {
    return r->state == RX_BLOCKED_RESOURCE && !r->quarantined && !r->parked && res_fits(w, r);
}

static bool class_blocked(const RxWorld *w, uint32_t prio) {
    for (uint32_t i = 0; i < w->n_reactions; i++) {
        const RxReaction *r = &w->reactions[i];
        if (r->desc.priority != prio || r->quarantined || r->parked) continue;
        if (r->state == RX_BLOCKED_RESOURCE) return true;
    }
    return false;
}

static int best_waiting(const RxWorld *w, int only_class) {
    int best = -1;
    for (uint32_t i = 0; i < w->n_reactions; i++) {
        const RxReaction *r = &w->reactions[i];
        if (only_class >= 0 && r->desc.priority != (uint32_t)only_class) continue;
        if (!waiting_fit(r, w)) continue;
        if (best < 0 || better_admit(r, &w->reactions[best])) best = (int)i;
    }
    return best;
}

/* Higher class wins. After starvation_bound admissions of something more
 * urgent, the highest class that has been waiting that long is served once.
 * Every class is protected, not only the lowest one. Adapted from the AIENOS
 * run queue's bounded background interval. */
static int pick_admit(RxWorld *w) {
    uint32_t bound = w->budget.starvation_bound;
    int chosen = -1;
    if (bound) {
        for (uint32_t p = 0; p < RX_PRIORITY_CLASSES; p++) {
            if (!class_blocked(w, p) || w->admit_debt[p] < bound) continue;
            chosen = best_waiting(w, (int)p);
            if (chosen >= 0) break;
        }
    }
    if (chosen < 0) chosen = best_waiting(w, -1);
    if (chosen < 0) return -1;
    uint32_t served = w->reactions[chosen].desc.priority;
    w->admit_debt[served] = 0;
    if (bound) {
        for (uint32_t p = served + 1; p < RX_PRIORITY_CLASSES; p++) {
            if (!class_blocked(w, p)) w->admit_debt[p] = 0;
            else if (w->admit_debt[p] < UINT32_MAX) w->admit_debt[p]++;
        }
    }
    return chosen;
}

static void try_admit(RxWorld *w) {
    for (;;) {
        int id = pick_admit(w);
        if (id < 0) break;
        admit_one(w, (uint32_t)id);
    }
    gauge_blocked(w);
}

static void demand_inner(RxWorld *w, uint32_t rid, uint64_t cause);

static void demand(RxWorld *w, uint32_t rid, uint64_t cause) {
    if (!w->timing) { demand_inner(w, rid, cause); return; }
    RxReaction *r = &w->reactions[rid];
    uint64_t c0 = thread_cpu_ns();
    uint64_t t0 = now_ns();
    RxState before = r->state;
    bool outer = w->sched_nest++ == 0;
    demand_inner(w, rid, cause);
    w->sched_nest--;
    uint64_t t1 = now_ns();
    uint64_t c1 = thread_cpu_ns();
    if (outer) {
        w->stats.sched_wall_ns += t1 - t0;
        w->stats.sched_cpu_ns += c1 - c0;
    }
    if (before == RX_DORMANT && r->state != RX_DORMANT) {
        r->t_demand = t0;
        r->sched_ns = t1 - t0;
        r->sched_cpu_ns = c1 - c0;
    } else {
        r->sched_ns += t1 - t0;
        r->sched_cpu_ns += c1 - c0;
    }
}

static void demand_inner(RxWorld *w, uint32_t rid, uint64_t cause) {
    RxReaction *r = &w->reactions[rid];
    w->stats.wakes++;
    if (r->quarantined && !cause_is_external(w, cause)) {
        r->suppressed++;
        w->stats.suppressed_wakes++;
        return;
    }
    if (cause_is_external(w, cause)) {
        r->quarantined = false;
        r->parked = false;
        r->yield_left = 0;
        r->osc_streak = 0;
        r->noop_streak = 0;
        r->conflict_streak = 0;
        r->episode_activations = 0;
    }
    /* The budget is per causal episode: everything one outside publication
     * set off, however far down the chain this reaction sits. */
    uint64_t ep = cause >= 1 && cause <= w->n_crumbs ? w->crumbs[cause - 1].episode : 0;
    if (ep && ep != r->episode) {
        r->episode = ep;
        r->episode_activations = 0;
    }
    r->parked = false;
    if (w->stability.activation_budget &&
        r->episode_activations >= w->stability.activation_budget) {
        if (!r->quarantined) {
            r->quarantined = true;
            w->stats.quarantines++;
            contain(w, rid, RX_CONTAIN_BUDGET, cause);
        }
        r->suppressed++;
        w->stats.suppressed_wakes++;
        return;
    }
    /* Sequential reference: the orchestrator decides what runs. A wake that
     * reaches here (a re-arm after invalidation) is a debt it will see. */
    if (w->sequential) {
        r->wake_cause = cause;
        r->seq_pending = true;
        return;
    }
    switch (r->state) {
    case RX_DORMANT:
        w->stats.wakes_accepted++;
        set_state(w, r, RX_BLOCKED_RESOURCE);
        r->wake_cause = cause;
        r->coalesced = 0;
        r->wait_seq = ++w->admit_seq;
        w->stats.blocked_resource++;
        try_admit(w);
        break;
    case RX_BLOCKED_RESOURCE:
        r->wake_cause = cause;
        r->coalesced++;
        w->stats.coalesced_wakes++;
        break;
    case RX_READY:
        r->wake_cause = cause;
        r->coalesced++;
        w->stats.coalesced_wakes++;
        break;
    default:
        r->wake_cause = cause;
        r->rearm = true;
        w->stats.coalesced_wakes++;
        break;
    }
}

/* Fan-out limits bound one propagation wave, not semantic delivery. Excess
 * dependents are queued and released in later waves. Allocation failure falls
 * back to immediate demand so a valid dependency change is never dropped. */
static int defer_wake(RxWorld *w, uint32_t rid, uint64_t cause) {
    if (w->deferred_head + w->deferred_len >= w->deferred_cap) {
        if (w->deferred_head && w->deferred_len) {
            memmove(w->deferred, w->deferred + w->deferred_head,
                    w->deferred_len * sizeof(*w->deferred));
            w->deferred_head = 0;
        }
        if (w->deferred_len >= w->deferred_cap) {
            uint32_t next = w->deferred_cap ? w->deferred_cap * 2u : RX_DEFERRED_INITIAL;
            if (next < w->deferred_cap) return -1;
            void *p = realloc(w->deferred, (size_t)next * sizeof(*w->deferred));
            if (!p) return -1;
            w->deferred = p;
            w->deferred_cap = next;
        }
    }
    uint32_t at = w->deferred_head + w->deferred_len++;
    w->deferred[at].reaction = rid;
    w->deferred[at].cause = cause;
    w->stats.deferred_wakes++;
    if (w->deferred_len > w->stats.deferred_peak)
        w->stats.deferred_peak = w->deferred_len;
    return 0;
}

static void drain_deferred(RxWorld *w, uint32_t quota) {
    if (quota == 0) return;
    for (;;) {
        uint32_t left = quota;
        while (left && w->deferred_len) {
            uint32_t rid = w->deferred[w->deferred_head].reaction;
            uint64_t cause = w->deferred[w->deferred_head].cause;
            w->deferred_head++;
            w->deferred_len--;
            if (w->deferred_len == 0) w->deferred_head = 0;
            demand(w, rid, cause);
            left--;
        }
        /* If a wave produced runnable work, let that work make progress before
         * releasing another wave. If every wake was suppressed or merely
         * parked on an unavailable resource, nothing else can drain the queue,
         * so continue here until the queue is empty. */
        if (w->in_flight != 0 || w->deferred_len == 0) break;
    }
}

static int prio_less(const RxWorld *w, uint32_t a, uint32_t b) {
    uint32_t pa = w->reactions[a].desc.priority;
    uint32_t pb = w->reactions[b].desc.priority;
    if (pa != pb) return pa < pb;
    return a < b;
}

/* Only the subscribers of this object, filtered by field mask and generation.
 * Fanout above the configured limit is deferred, highest priority first.
 * Limiting wake concurrency must never make a valid dependent miss a change. */
static void propagate_inner(RxWorld *w, uint32_t obj, uint64_t changed, uint64_t cause);

static void propagate(RxWorld *w, uint32_t obj, uint64_t changed, uint64_t cause) {
    /* The sequential reference polls readiness instead of being told. */
    if (w->sequential) return;
    if (!w->timing) { propagate_inner(w, obj, changed, cause); return; }
    RxStats s0 = w->stats;
    uint64_t c0 = thread_cpu_ns();
    uint64_t t0 = now_ns();
    propagate_inner(w, obj, changed, cause);
    uint64_t t1 = now_ns();
    uint64_t c1 = thread_cpu_ns();
    const RxStats *s1 = &w->stats;
    RxPropWave *p = &w->last_prop;
    p->wall_ns = t1 - t0;
    p->cpu_ns = c1 - c0;
    p->inspected = s1->subscriptions_checked - s0.subscriptions_checked;
    p->matched = s1->subscriptions_matched - s0.subscriptions_matched;
    p->wake_attempts = s1->wakes - s0.wakes;
    p->wakes_accepted = s1->wakes_accepted - s0.wakes_accepted;
    p->ready_inserts = s1->ready_inserts - s0.ready_inserts;
    p->coalesced = s1->coalesced_wakes - s0.coalesced_wakes;
    p->deferred = s1->deferred_wakes - s0.deferred_wakes;
    p->suppressed = s1->suppressed_wakes - s0.suppressed_wakes;
}

static void propagate_inner(RxWorld *w, uint32_t obj, uint64_t changed, uint64_t cause) {
    if (!w->objects[obj].live) return;
    uint32_t gen = w->objects[obj].generation;
    uint32_t hits[RX_MAX_REACTIONS];
    uint32_t nh = 0;
    for (uint32_t i = 0; i < w->n_subs[obj]; i++) {
        const RxSub *s = &w->subs[obj][i];
        w->stats.subscriptions_checked++;
        if (s->generation != gen) continue;
        if (!(s->mask & changed)) continue;
        w->stats.subscriptions_matched++;
        if (nh < RX_MAX_REACTIONS) hits[nh++] = s->reaction;
    }
    for (uint32_t i = 1; i < nh; i++) {
        uint32_t v = hits[i];
        uint32_t j = i;
        while (j > 0 && prio_less(w, v, hits[j - 1])) {
            hits[j] = hits[j - 1];
            j--;
        }
        hits[j] = v;
    }
    uint32_t allow = nh;
    if (w->stability.fanout_limit && allow > w->stability.fanout_limit)
        allow = w->stability.fanout_limit;
    for (uint32_t i = 0; i < allow; i++) demand(w, hits[i], cause);
    for (uint32_t i = allow; i < nh; i++) {
        if (defer_wake(w, hits[i], cause) != 0)
            demand(w, hits[i], cause);
    }
    if (w->in_flight == 0 && w->deferred_len) {
        uint32_t quota = w->stability.fanout_limit ? w->stability.fanout_limit : w->deferred_len;
        drain_deferred(w, quota);
    }
}

static bool pop_ready(RxWorld *w, uint32_t *rid) {
    int high = -1;
    for (uint32_t p = 0; p < RX_PRIORITY_CLASSES; p++) {
        if (w->ready_len[p] == 0) w->run_debt[p] = 0;
        else if (high < 0) high = (int)p;
    }
    if (high < 0) return false;
    uint32_t chosen = (uint32_t)high;
    uint32_t bound = w->budget.starvation_bound;
    if (bound) {
        for (uint32_t p = chosen + 1; p < RX_PRIORITY_CLASSES; p++) {
            if (w->ready_len[p] && w->run_debt[p] >= bound) {
                chosen = p;
                break;
            }
        }
    }
    *rid = w->ready_q[chosen][w->ready_head[chosen]];
    w->ready_head[chosen] = (w->ready_head[chosen] + 1) % RX_MAX_REACTIONS;
    w->ready_len[chosen]--;
    w->run_debt[chosen] = 0;
    if (bound) {
        for (uint32_t p = chosen + 1; p < RX_PRIORITY_CLASSES; p++)
            if (w->ready_len[p] && w->run_debt[p] < UINT32_MAX) w->run_debt[p]++;
    }
    return true;
}

/* ---- validation --------------------------------------------------------- */

static bool ref_live(const RxWorld *w, RxObjRef ref) {
    return ref.id < RX_MAX_OBJECTS && w->objects[ref.id].live &&
           w->objects[ref.id].generation == ref.generation;
}

int rx_world_validate_cap(const RxWorld *w, RxCapRef ref, uint32_t subject,
                          uint64_t resource, uint32_t rights, RxCapEntry *out) {
    if (!w) return RX_CAP_ERR_STATE;
    if (w->auth_validate) return w->auth_validate(w->auth_ctx, ref, subject, resource, rights, out);
    if (!w->root) return RX_CAP_ERR_STATE;
    return rx_caproot_validate(w->root, ref, subject, resource, rights, out);
}

int rx_world_inspect_cap(const RxWorld *w, RxCapRef ref, RxCapEntry *out) {
    if (!w) return RX_CAP_ERR_STATE;
    if (w->auth_inspect) return w->auth_inspect(w->auth_ctx, ref, out);
    if (!w->root) return RX_CAP_ERR_STATE;
    return rx_caproot_inspect(w->root, ref, out);
}

/* The reference a reaction presents for caps[i]. Caller holds the world lock. */
static RxCapRef need_ref(const RxWorld *w, const RxReactionDesc *d, uint32_t i) {
    if (!d->cap_slotted[i]) return d->caps[i].ref;
    RxObjRef s = d->cap_slot[i];
    if (s.id >= RX_MAX_OBJECTS) return (RxCapRef){ UINT32_MAX, 0 };
    const RxObject *o = &w->objects[s.id];
    if (!o->live || o->generation != s.generation) return (RxCapRef){ UINT32_MAX, 0 };
    if (o->field[0] > UINT32_MAX) return (RxCapRef){ UINT32_MAX, 0 };
    return (RxCapRef){ (uint32_t)o->field[0], o->field[1] };
}

/* ---- R16 C5 caller credentials (rx_caller.h) ------------------------------ */

static void caller_digest(uint32_t subject, uint64_t generation, const uint8_t *secret,
                          uint8_t out[32]) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)"AIEN_RX_CALLER_V1", 17);
    put32(&c, subject);
    put64(&c, generation);
    sha256_update(&c, secret, RX_CALLER_SECRET_LEN);
    sha256_final(&c, out);
}

/* Caller holds callers_mu. */
static int caller_slot(const RxWorld *w, uint32_t subject) {
    for (uint32_t i = 0; i < w->n_callers; i++)
        if (w->callers[i].subject == subject) return (int)i;
    return -1;
}

/* Full check of a presented credential. Caller holds callers_mu. */
static int caller_check_locked(const RxWorld *w, uint32_t subject, const RxCallerCred *cred) {
    if (!cred || cred->generation == 0) return RX_CALLER_ERR_ABSENT;
    int s = caller_slot(w, subject);
    if (s < 0) return RX_CALLER_ERR_UNKNOWN;
    if (!w->callers[s].live) return RX_CALLER_ERR_REVOKED;
    if (cred->generation != w->callers[s].generation) return RX_CALLER_ERR_STALE;
    uint8_t d[32];
    caller_digest(subject, cred->generation, cred->secret, d);
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= (uint8_t)(d[i] ^ w->callers[s].digest[i]);
    return diff ? RX_CALLER_ERR_FORGED : RX_CALLER_OK;
}

/* The enrollment a registered reaction was admitted under is still live at
 * the same generation (its secret was checked at registration and is not
 * kept). Caller holds mu. */
static int caller_still_live(const RxWorld *w, uint32_t subject, uint64_t generation) {
    RxWorld *mw = (RxWorld *)w;
    pthread_mutex_lock(&mw->callers_mu);
    int s = caller_slot(w, subject);
    int rc = s < 0 ? RX_CALLER_ERR_UNKNOWN
           : !w->callers[s].live ? RX_CALLER_ERR_REVOKED
           : w->callers[s].generation != generation ? RX_CALLER_ERR_STALE : RX_CALLER_OK;
    pthread_mutex_unlock(&mw->callers_mu);
    return rc;
}

int rx_world_enroll_caller(RxWorld *w, uint32_t subject, RxCallerCred *out) {
    if (!w || !out) return RX_ERR_ARG;
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&w->callers_mu);
    int rc = RX_CALLER_OK;
    if (w->callers_bound) { rc = RX_CALLER_ERR_CLOSED; goto out; }
    if (caller_slot(w, subject) >= 0) { rc = RX_CALLER_ERR_EXISTS; goto out; }
    if (w->n_callers >= RX_CALLER_MAX) { rc = RX_CALLER_ERR_FULL; goto out; }
    uint8_t secret[RX_CALLER_SECRET_LEN];
    size_t n = 0;
    while (n < sizeof secret) {
        ssize_t g = getrandom(secret + n, sizeof secret - n, 0);
        if (g < 0) {
            if (errno == EINTR) continue;
            rc = RX_CALLER_ERR_ENTROPY;
            goto out;
        }
        n += (size_t)g;
    }
    uint32_t s = w->n_callers++;
    w->callers[s].subject = subject;
    w->callers[s].live = true;
    w->callers[s].generation = ++w->caller_generation;
    caller_digest(subject, w->callers[s].generation, secret, w->callers[s].digest);
    out->generation = w->callers[s].generation;
    memcpy(out->secret, secret, sizeof secret);
    for (size_t i = 0; i < sizeof secret; i++) ((volatile uint8_t *)secret)[i] = 0;
out:
    pthread_mutex_unlock(&w->callers_mu);
    return rc;
}

int rx_world_bind_callers(RxWorld *w) {
    if (!w) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    int rc = RX_OK;
    /* R16 C7: a reaction admitted without a credential while the world was
     * unbound would run in the bound world under a subject nobody proved.
     * Refuse, and leave the world unbound. */
    for (uint32_t i = 0; i < w->n_reactions; i++)
        if (w->reactions[i].desc.caller.generation == 0) rc = RX_ERR_IDENTITY;
    if (rc == RX_OK) {
        pthread_mutex_lock(&w->callers_mu);
        w->callers_bound = true;
        pthread_mutex_unlock(&w->callers_mu);
    }
    pthread_mutex_unlock(&w->mu);
    return rc;
}

int rx_world_revoke_caller(RxWorld *w, uint32_t subject, const RxCallerCred *cred) {
    if (!w) return RX_ERR_ARG;
    /* R16 C7: the world lock first (the order every path uses: mu, then
     * callers_mu). A commit and a seat completion hold mu from their identity
     * check to their publish, so a revocation precedes the check or follows
     * the publish, never lands between them. Never call with mu held. */
    const int serialize = 1;
    if (serialize) pthread_mutex_lock(&w->mu);
    pthread_mutex_lock(&w->callers_mu);
    int rc = caller_check_locked(w, subject, cred);
    if (rc == RX_CALLER_OK) {
        int s = caller_slot(w, subject);
        w->callers[s].live = false;
        memset(w->callers[s].digest, 0, sizeof w->callers[s].digest);
    }
    pthread_mutex_unlock(&w->callers_mu);
    if (serialize) pthread_mutex_unlock(&w->mu);
    return rc;
}

int rx_world_check_caller(RxWorld *w, uint32_t subject, const RxCallerCred *cred) {
    if (!w) return RX_ERR_ARG;
    pthread_mutex_lock(&w->callers_mu);
    int rc = caller_check_locked(w, subject, cred);
    pthread_mutex_unlock(&w->callers_mu);
    return rc;
}

int rx_world_caller_check_fn(void *world, uint32_t subject, const RxCallerCred *cred, int op) {
    RxWorld *w = world;
    if (!w) return RX_ERR_ARG;
    if (op == RX_CALLER_OP_RELEASE) {
        pthread_mutex_unlock(&w->callers_mu);
        return RX_CALLER_OK;
    }
    pthread_mutex_lock(&w->callers_mu);
    int rc = caller_check_locked(w, subject, cred);
    /* HOLD: a pass keeps the table locked until RELEASE (no revocation). */
    if (op != RX_CALLER_OP_HOLD || rc != RX_CALLER_OK) pthread_mutex_unlock(&w->callers_mu);
    return rc;
}

static int validate_caps(const RxWorld *w, const RxReactionDesc *d, int *first_err) {
    /* R16 C5: the subject the capabilities are checked against is the one
     * this reaction was admitted under, and that admission still stands. */
    if (w->callers_bound) {
        int irc = caller_still_live(w, d->subject, d->caller.generation);
        if (irc != RX_CALLER_OK) { if (first_err) *first_err = RX_ERR_IDENTITY; return -1; }
    }
    for (uint32_t i = 0; i < d->n_caps; i++) {
        int rc = rx_world_validate_cap(w, need_ref(w, d, i), d->subject,
                                       d->caps[i].resource, d->caps[i].rights, NULL);
        if (rc != RX_CAP_OK) {
            if (first_err) *first_err = rc;
            return -1;
        }
    }
    return 0;
}

/* Static coverage: every object read needs a READ capability need on its
 * resource and every object written a WRITE one. */
static bool covered(const RxWorld *w, const RxReactionDesc *d, RxObjRef o, uint32_t right) {
    uint64_t res = w->objects[o.id].resource;
    for (uint32_t i = 0; i < d->n_caps; i++)
        if (d->caps[i].resource == res && (d->caps[i].rights & right) == right) return true;
    return false;
}

/* Collect triggers + reads into one deduplicated dependency list. */
static uint32_t gather_deps(const RxReactionDesc *d, RxDep out[RX_MAX_DEPS]) {
    uint32_t n = 0;
    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t cnt = pass ? d->n_reads : d->n_triggers;
        const RxDep *src = pass ? d->reads : d->triggers;
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t j;
            for (j = 0; j < n; j++)
                if (out[j].obj.id == src[i].obj.id && out[j].obj.generation == src[i].obj.generation) {
                    out[j].mask |= src[i].mask;
                    break;
                }
            if (j == n && n < RX_MAX_DEPS) out[n++] = src[i];
        }
    }
    return n;
}

/* ---- publication -------------------------------------------------------- */

typedef struct {
    RxObjRef obj;
    uint64_t changed;
    uint64_t proposed;          /* every field the publication named */
    uint64_t value[RX_MAX_FIELDS];
} PendingWrite;

/* Apply mutations atomically (caller holds mu). Returns the number of objects
 * written into pw/n_pw, or a negative error with nothing applied. */
static int stage_mutations(RxWorld *w, const RxMutation *m, uint32_t n,
                           const RxDep *write_set, uint32_t n_ws,
                           PendingWrite pw[RX_MAX_WRITES], uint32_t *n_pw) {
    *n_pw = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (m[i].field >= RX_MAX_FIELDS) return RX_ERR_WRITE_SET;
        if (!ref_live(w, m[i].obj)) return RX_ERR_STALE_GEN;
        if (write_set) {
            bool ok = false;
            for (uint32_t k = 0; k < n_ws; k++)
                if (write_set[k].obj.id == m[i].obj.id &&
                    write_set[k].obj.generation == m[i].obj.generation &&
                    (write_set[k].mask & RX_FIELD(m[i].field))) ok = true;
            if (!ok) return RX_ERR_WRITE_SET;
        }
        uint32_t j;
        for (j = 0; j < *n_pw; j++)
            if (pw[j].obj.id == m[i].obj.id) break;
        if (j == *n_pw) {
            if (*n_pw >= RX_MAX_WRITES) return RX_ERR_FULL;
            pw[j].obj = m[i].obj;
            pw[j].changed = 0;
            pw[j].proposed = 0;
            memcpy(pw[j].value, w->objects[m[i].obj.id].field, sizeof(pw[j].value));
            w->stats.stage_bytes += sizeof(pw[j].value);
            (*n_pw)++;
        }
        pw[j].value[m[i].field] = m[i].value;
        pw[j].proposed |= RX_FIELD(m[i].field);
    }
    for (uint32_t j = 0; j < *n_pw; j++) {
        const RxObject *o = &w->objects[pw[j].obj.id];
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++)
            if (pw[j].value[f] != o->field[f]) pw[j].changed |= RX_FIELD(f);
    }
    return RX_OK;
}

static void commit_writes(RxWorld *w, PendingWrite *pw, uint32_t n_pw, RxCrumb *k) {
    k->n_outputs = 0;
    for (uint32_t j = 0; j < n_pw; j++) {
        if (!pw[j].changed) continue;
        RxObject *o = &w->objects[pw[j].obj.id];
        o->version++;
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++) {
            if (!(pw[j].changed & RX_FIELD(f))) continue;
            o->field[f] = pw[j].value[f];
            o->field_version[f] = o->version;
        }
        object_digest(o);
        rx_coherent_project(w, pw[j].obj.id);
        k->outputs[k->n_outputs].obj = pw[j].obj;
        k->outputs[k->n_outputs].version = o->version;
        k->outputs[k->n_outputs].mask = pw[j].changed;
        k->n_outputs++;
    }
}

/* After the crumb id is known: stamp writers, then wake subscribers.
 * With stamp_proposed, a field the publication named but did not change is
 * stamped too (on an object it did change), so the whole record is this
 * commit's. Only changed fields wake anyone. */
static void finish_writes(RxWorld *w, PendingWrite *pw, uint32_t n_pw, uint64_t cid,
                          bool stamp_proposed) {
    for (uint32_t j = 0; j < n_pw; j++) {
        if (!pw[j].changed) continue;
        RxObject *o = &w->objects[pw[j].obj.id];
        uint64_t mask = stamp_proposed ? pw[j].changed | pw[j].proposed : pw[j].changed;
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++)
            if (mask & RX_FIELD(f)) o->field_writer[f] = cid;
    }
    for (uint32_t j = 0; j < n_pw; j++) {
        if (!pw[j].changed) continue;
        rx_coherent_publish(w, pw[j].obj.id, cid);
        propagate(w, pw[j].obj.id, pw[j].changed, cid);
    }
}

/* ---- worker ------------------------------------------------------------- */

static void arm_backoff(RxReaction *r) {
    uint32_t n = r->yield_left ? r->yield_left * 2u : 1u;
    if (n < r->yield_left || n > 8u) n = 8u;
    r->yield_left = n;
}

static void note_conflict(RxWorld *w, RxReaction *r) {
    r->conflict_streak++;
    w->stats.churn++;
    arm_backoff(r);
    if (w->stability.conflict_limit && r->conflict_streak >= w->stability.conflict_limit &&
        !r->quarantined) {
        r->quarantined = true;
        w->stats.quarantines++;
        r->contain_pending = RX_CONTAIN_CONFLICT;
    }
}

static void note_value(RxWorld *w, RxReaction *r, int progressed, uint64_t value) {
    if (!progressed) {
        r->noop_streak++;
        w->stats.churn++;
        arm_backoff(r);
        if (w->stability.livelock_limit && r->noop_streak >= w->stability.livelock_limit &&
            !r->quarantined) {
            r->quarantined = true;
            w->stats.quarantines++;
            w->stats.livelock_trips++;
            r->contain_pending = RX_CONTAIN_LIVELOCK;
        }
        return;
    }
    r->noop_streak = 0;
    r->conflict_streak = 0;
    int flipped = r->have_two && value == r->prev_out && value != r->last_out;
    if (flipped && r->desc.loop_kind == RX_LOOP_PERIODIC) {
        r->osc_streak = 0;
        r->yield_left = 0;
        w->stats.periodic_commits++;
        w->stats.useful_commits++;
    } else if (flipped) {
        r->osc_streak++;
        w->stats.churn++;
        arm_backoff(r);
    } else {
        r->osc_streak = 0;
        r->yield_left = 0;
        w->stats.useful_commits++;
    }
    if (r->have_last) {
        r->prev_out = r->last_out;
        r->have_two = true;
    }
    r->last_out = value;
    r->have_last = true;
    if (w->stability.oscillation_limit && r->osc_streak >= w->stability.oscillation_limit &&
        !r->quarantined) {
        r->quarantined = true;
        w->stats.quarantines++;
        w->stats.oscillation_trips++;
        r->contain_pending = RX_CONTAIN_OSCILLATION;
    }
}

static bool others_busy(const RxWorld *w, uint32_t self) {
    for (uint32_t i = 0; i < w->n_reactions; i++) {
        if (i == self) continue;
        RxState s = w->reactions[i].state;
        if (s == RX_BLOCKED_RESOURCE || s == RX_READY || s == RX_RUNNING || s == RX_PUBLISHING)
            return true;
    }
    return false;
}

static void release_parked(RxWorld *w) {
    for (uint32_t i = 0; i < w->n_reactions; i++) {
        RxReaction *r = &w->reactions[i];
        if (!r->parked) continue;
        bool busy = others_busy(w, i);
        if (r->yield_left && busy) continue;
        r->parked = false;
        r->yield_left = 0;
        uint64_t cause = r->wake_cause;
        demand(w, i, cause);
    }
}

static void tick_parked(RxWorld *w) {
    for (uint32_t i = 0; i < w->n_reactions; i++) {
        RxReaction *r = &w->reactions[i];
        if (!r->parked || r->yield_left == 0) continue;
        r->yield_left--;
    }
    release_parked(w);
}

static void record_timing(RxWorld *w, RxReaction *r, uint32_t rid) {
    uint64_t i = w->n_timing++;
    if (i >= w->timing_cap) return;
    const RxCrumb *k = r->last_crumb ? &w->crumbs[r->last_crumb - 1] : NULL;
    RxTiming *t = &w->timing[i];
    t->reaction = rid;
    t->outcome = k && k->reaction == rid ? (uint32_t)k->kind : 0;
    t->cause = k && k->reaction == rid ? k->wake_cause : 0;
    t->t_demand = r->t_demand;
    t->t_ready = r->t_ready;
    t->t_run = r->t_run;
    t->t_fn_end = r->t_fn_end;
    t->t_visible = now_ns();
    t->sched_ns = r->sched_ns;
    t->sched_cpu_ns = r->sched_cpu_ns;
    r->t_demand = r->t_ready = r->t_run = r->t_fn_end = r->sched_ns = r->sched_cpu_ns = 0;
}

static void end_activation_inner(RxWorld *w, uint32_t rid);

/* Everything after the timing record is scheduling work for other reactions
 * (release, admission, deferred and parked wakes): it counts toward the
 * world's scheduler totals, once, however deeply it nests. */
static void end_activation(RxWorld *w, uint32_t rid) {
    if (!w->timing) { end_activation_inner(w, rid); return; }
    record_timing(w, &w->reactions[rid], rid);
    uint64_t c0 = thread_cpu_ns();
    uint64_t t0 = now_ns();
    bool outer = w->sched_nest++ == 0;
    end_activation_inner(w, rid);
    w->sched_nest--;
    if (outer) {
        w->stats.sched_wall_ns += now_ns() - t0;
        w->stats.sched_cpu_ns += thread_cpu_ns() - c0;
    }
}

static void end_activation_inner(RxWorld *w, uint32_t rid) {
    RxReaction *r = &w->reactions[rid];
    /* A limit engaged during this activation: recorded after its crumb. */
    if (r->contain_pending) {
        contain(w, rid, r->contain_pending, r->last_crumb);
        r->contain_pending = 0;
    }
    tick_parked(w);
    uncharge(w, r);
    if (r->quarantined) r->rearm = false;
    set_state(w, r, RX_DORMANT);
    if (w->in_flight) w->in_flight--;
    if (r->rearm && r->yield_left && others_busy(w, rid)) {
        r->yield_left--;
        r->parked = true;
        r->rearm = false;
        w->stats.backoffs++;
        try_admit(w);
    } else if (r->rearm) {
        uint64_t cause = r->wake_cause;
        r->rearm = false;
        demand(w, rid, cause);
    } else {
        try_admit(w);
    }
    if (w->deferred_len) {
        uint32_t quota = w->stability.fanout_limit ? w->stability.fanout_limit : w->deferred_len;
        drain_deferred(w, quota);
    }
    if (w->in_flight == 0) {
        release_parked(w);
        if (w->in_flight == 0 && w->deferred_len == 0)
            pthread_cond_broadcast(&w->idle_cv);
    }
}

static void stamp_cap(RxWorld *w, RxCrumb *k, uint32_t i, RxCapRef ref) {
    k->caps[i] = ref;
    k->cap_issuer[i] = 0;
    RxCapEntry e;
    if (rx_world_inspect_cap(w, ref, &e) == RX_CAP_OK)
        k->cap_issuer[i] = e.minted_by_id ? e.minted_by_id : e.issuer;
}

static void fill_inputs(RxCrumb *k, const RxCtx *ctx, const RxDep *deps, uint32_t n) {
    k->n_inputs = n;
    for (uint32_t i = 0; i < n; i++) {
        k->inputs[i].obj = deps[i].obj;
        k->inputs[i].version = ctx ? ctx->in[i].version : 0;
        k->inputs[i].mask = deps[i].mask;
    }
}

static void resume_locked(RxWorld *w, uint32_t rid);

static void run_one(RxWorld *w, uint32_t rid, uint32_t worker) {
    RxReaction *r = &w->reactions[rid];
    const RxReactionDesc *d = &r->desc;
    RxCrumb k;
    memset(&k, 0, sizeof(k));
    k.reaction = rid;
    k.faculty = d->faculty;
    k.worker = worker;
    k.wake_cause = r->wake_cause;
    k.coalesced_wakes = r->coalesced;
    k.t_start_ns = now_ns();
    r->t_run = k.t_start_ns;
    w->stats.activations++;
    k.n_caps = d->n_caps;
    for (uint32_t i = 0; i < d->n_caps; i++) stamp_cap(w, &k, i, need_ref(w, d, i));
    add_parent(&k, r->wake_cause);
    r->activations++;
    r->episode_activations++;

    RxDep deps[RX_MAX_DEPS];
    uint32_t n_deps = gather_deps(d, deps);

    /* Semantic readiness: required inputs exist. */
    for (uint32_t i = 0; i < n_deps; i++) {
        if (!ref_live(w, deps[i].obj)) {
            set_state(w, r, RX_INVALIDATED);
            fill_inputs(&k, NULL, deps, n_deps);
            k.kind = RX_CRUMB_INVALIDATED;
            k.reason = RX_ERR_STALE_GEN;
            k.t_end_ns = now_ns();
            crumb_append(w, &k);
            w->stats.invalidations++;
            note_conflict(w, r);
            end_activation(w, rid);
            return;
        }
    }
    /* Required authority exists. */
    int cap_err = 0;
    if (validate_caps(w, d, &cap_err) != 0) {
        set_state(w, r, RX_BLOCKED_AUTHORITY);
        fill_inputs(&k, NULL, deps, n_deps);
        k.kind = RX_CRUMB_BLOCKED_AUTHORITY;
        k.reason = cap_err;
        k.t_end_ns = now_ns();
        crumb_append(w, &k);
        w->stats.blocked_authority++;
        end_activation(w, rid);
        return;
    }

    set_state(w, r, RX_RUNNING);
    if (d->need.accelerator_features & RX_ACCEL_BLACKWELL) {
        uint32_t in = d->triggers[0].obj.id;
        uint32_t id = d->writes[0].obj.id;
        const RxObject *a = &w->objects[in];
        const RxObject *o = &w->objects[id];
        if (!w->resident_enabled || !a->placed || !a->live || !o->placed || !o->live ||
            a->generation != d->triggers[0].obj.generation ||
            o->generation != d->writes[0].obj.generation) {
            set_state(w, r, RX_INVALIDATED);
            fill_inputs(&k, NULL, deps, n_deps);
            k.kind = RX_CRUMB_INVALIDATED;
            k.reason = w->resident_enabled ? RX_ERR_UNPLACED : RX_ERR_ARG;
            k.t_end_ns = now_ns();
            crumb_append(w, &k);
            w->stats.invalidations++;
            end_activation(w, rid);
            return;
        }
        /* R8: an object whose authority this reaction reads from a slot is
         * claimed under what the slot holds now, which validate_caps just
         * accepted. A renewal revokes the reference bound earlier. */
        for (uint32_t i = 0; i < d->n_caps; i++) {
            if (!d->cap_slotted[i]) continue;
            if (d->caps[i].resource == w->objects[in].resource)
                w->objects[in].cap = need_ref(w, d, i);
            if (d->caps[i].resource == w->objects[id].resource)
                w->objects[id].cap = need_ref(w, d, i);
        }
        uint64_t seq = 0;
        int pr = rx_resident_post_claim(w, in, id, r->wake_cause, &seq);
        if (pr != RX_OK) {
            set_state(w, r, RX_FAILED);
            fill_inputs(&k, NULL, deps, n_deps);
            k.kind = RX_CRUMB_FAILED;
            k.reason = pr;
            k.t_end_ns = now_ns();
            crumb_append(w, &k);
            w->stats.failed++;
            end_activation(w, rid);
            return;
        }
        r->resident_seat = true;
        r->resident_seq = seq;
        r->resident_parent = r->wake_cause;
        pthread_cond_broadcast(&w->claim_cv);
        return;
    }
    RxCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.user = d->user;
    ctx.worker = worker;
    ctx.n_in = n_deps;
    for (uint32_t i = 0; i < n_deps; i++) {
        const RxObject *o = &w->objects[deps[i].obj.id];
        ctx.in[i].obj = deps[i].obj;
        ctx.in[i].mask = deps[i].mask;
        ctx.in[i].version = o->version;
        memcpy(ctx.in[i].field, o->field, sizeof(o->field));
        memcpy(ctx.in[i].field_version, o->field_version, sizeof(o->field_version));
        memcpy(ctx.in[i].field_writer, o->field_writer, sizeof(o->field_writer));
    }
    w->stats.snapshot_bytes += (uint64_t)n_deps * sizeof(RxSnapshotDep);

    /* Compute against the snapshot, outside the world lock. */
    pthread_mutex_unlock(&w->mu);
    int frc = d->fn(&ctx);
    pthread_mutex_lock(&w->mu);
    if (w->timing) r->t_fn_end = now_ns();
    if (frc == RX_FN_DEFER) {
        if (w->sequential) {
            frc = RX_ERR_ARG;
        } else {
            /* The effect is on the durable executor. Keep the admission, free
             * the worker; rx_world_resume runs this activation again. */
            r->deferred = true;
            w->n_deferred++;
            w->stats.deferrals++;
            if (r->resume_pending) resume_locked(w, rid);
            return;
        }
    }

    fill_inputs(&k, &ctx, deps, n_deps);
    /* Data ancestry: whoever last wrote each field this reaction read. */
    for (uint32_t i = 0; i < n_deps; i++)
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++)
            if (deps[i].mask & RX_FIELD(f)) add_parent(&k, ctx.in[i].field_writer[f]);

    if (frc < 0) {
        set_state(w, r, RX_FAILED);
        k.kind = RX_CRUMB_FAILED;
        k.reason = frc;
        k.t_end_ns = now_ns();
        crumb_append(w, &k);
        w->stats.failed++;
        end_activation(w, rid);
        return;
    }

    set_state(w, r, RX_PUBLISHING);
    /* 1. Versioned reads still valid? */
    bool stale = false;
    for (uint32_t i = 0; i < n_deps && !stale; i++) {
        const RxObject *o = &w->objects[deps[i].obj.id];
        if (!ref_live(w, deps[i].obj)) { stale = true; break; }
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++)
            if ((deps[i].mask & RX_FIELD(f)) && o->field_version[f] != ctx.in[i].field_version[f]) {
                stale = true;
                break;
            }
    }
    if (stale) {
        set_state(w, r, RX_INVALIDATED);
        k.kind = RX_CRUMB_INVALIDATED;
        k.reason = RX_ERR_STALE_GEN;
        k.t_end_ns = now_ns();
        crumb_append(w, &k);
        w->stats.invalidations++;
        note_conflict(w, r);
        /* The wake this activation answered is still unanswered. A change to a
         * trigger has already re-armed it; a change to a read-only input has
         * not, and would otherwise drop the stimulus. Run again on a fresh
         * snapshot under the same cause. Conflict limits still apply. */
        r->rearm = true;
        end_activation(w, rid);
        return;
    }
    /* 2. Authority still valid (revocation during the run must not execute). */
    if (validate_caps(w, d, &cap_err) != 0) {
        set_state(w, r, RX_REJECTED);
        k.kind = RX_CRUMB_REJECTED;
        k.reason = cap_err;
        k.t_end_ns = now_ns();
        crumb_append(w, &k);
        w->stats.rejected++;
        note_conflict(w, r);
        end_activation(w, rid);
        return;
    }
    /* 3. Mutations inside the declared write set, targets not stale. */
    PendingWrite pw[RX_MAX_WRITES];
    uint32_t n_pw = 0;
    int src = stage_mutations(w, ctx.out, ctx.n_out, d->writes, d->n_writes, pw, &n_pw);
    if (src != RX_OK) {
        set_state(w, r, RX_REJECTED);
        k.kind = RX_CRUMB_REJECTED;
        k.reason = src;
        k.t_end_ns = now_ns();
        crumb_append(w, &k);
        w->stats.rejected++;
        note_conflict(w, r);
        end_activation(w, rid);
        return;
    }
    /* 4. Atomic publish. */
    commit_writes(w, pw, n_pw, &k);
    k.kind = k.n_outputs ? RX_CRUMB_COMMIT : RX_CRUMB_NOOP;
    if (k.n_outputs) {
        uint64_t sample = 0;
        uint64_t mask = k.outputs[0].mask;
        const RxObject *o = &w->objects[k.outputs[0].obj.id];
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++)
            if (mask & RX_FIELD(f)) { sample = o->field[f]; break; }
        note_value(w, r, 1, sample);
    } else {
        note_value(w, r, 0, 0);
    }
    k.t_end_ns = now_ns();
    uint64_t cid = crumb_append(w, &k);
    set_state(w, r, RX_COMMITTED);
    if (k.n_outputs) {
        w->stats.commits++;
        r->commits++;
    } else {
        w->stats.noops++;
    }
    finish_writes(w, pw, n_pw, cid, d->stamp_proposed);
    end_activation(w, rid);
}

/* Caller holds mu. The activation is still admitted (slot charged, counted in
 * in_flight): put it back on its ready ring without admitting it again. */
static void resume_locked(RxWorld *w, uint32_t rid) {
    RxReaction *r = &w->reactions[rid];
    r->resume_pending = false;
    r->deferred = false;
    if (w->n_deferred) w->n_deferred--;
    uint32_t p = r->desc.priority;
    if (p >= RX_PRIORITY_CLASSES) p = RX_PRIO_BACKGROUND;
    uint32_t tail = (w->ready_head[p] + w->ready_len[p]) % RX_MAX_REACTIONS;
    w->ready_q[p][tail] = rid;
    w->ready_len[p]++;
    set_state(w, r, RX_READY);
    if (w->timing) r->t_ready = now_ns();
    pthread_cond_signal(&w->work_cv);
    if (w->n_deferred == 0) pthread_cond_broadcast(&w->idle_cv);
}

int rx_world_resume(RxWorld *w, uint32_t reaction) {
    if (!w) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    if (reaction >= w->n_reactions) {
        pthread_mutex_unlock(&w->mu);
        return RX_ERR_ARG;
    }
    RxReaction *r = &w->reactions[reaction];
    if (r->deferred) resume_locked(w, reaction);
    else r->resume_pending = true;
    pthread_mutex_unlock(&w->mu);
    return RX_OK;
}

static void *worker_main(void *arg) {
    RxWorld *w = arg;
    pthread_mutex_lock(&w->mu);
    uint32_t me = UINT32_MAX;
    for (uint32_t i = 0; i < w->n_workers; i++)
        if (pthread_equal(w->workers[i], pthread_self())) me = i;
    for (;;) {
        uint32_t rid;
        uint64_t t0 = w->timing ? now_ns() : 0;
        uint64_t c0 = w->timing ? thread_cpu_ns() : 0;
#if RX_ARGUS
        int argus_parked = 0;
#endif
        while (!w->stopping && !pop_ready(w, &rid)) {
#if RX_ARGUS
            /* ARGUS: before the first wait of a park, flush this worker's use table
             * and publish idle. The flush touches only this thread's own ARGUS slot,
             * so it runs outside w->mu (speed2: under the lock it lengthened every
             * hold that submitters wait on). Only when a flush is due, once per park;
             * after relocking, re-check stopping/work before waiting. */
            if (!argus_parked) {
                argus_parked = 1;
                if (rx_argus_idle_pending()) {
                    pthread_mutex_unlock(&w->mu);
                    rx_argus_idle();
                    pthread_mutex_lock(&w->mu);
                    continue;
                }
            }
#endif
            pthread_cond_wait(&w->work_cv, &w->mu);
            t0 = w->timing ? now_ns() : 0;
            c0 = w->timing ? thread_cpu_ns() : 0;
        }
        if (w->stopping) break;
        if (w->timing) {
            uint64_t c1 = thread_cpu_ns();
            uint64_t t1 = now_ns();
            RxReaction *r = &w->reactions[rid];
            r->sched_ns += t1 - t0;
            r->sched_cpu_ns += c1 - c0;
            w->stats.sched_wall_ns += t1 - t0;
            w->stats.sched_cpu_ns += c1 - c0;
        }
        run_one(w, rid, me);
    }
    pthread_mutex_unlock(&w->mu);
    return NULL;
}

/* ---- public API ---------------------------------------------------------- */

int rx_world_init(RxWorld *w, RxCapRoot *root, uint32_t n_workers, uint64_t crumb_cap) {
    if (!root) return RX_ERR_ARG;
    return rx_world_init_with_auth(w, root, NULL, NULL, NULL, n_workers, crumb_cap);
}

static int init_common(RxWorld *w, RxCapRoot *root, const void *auth_ctx,
                       RxAuthValidateFn validate, RxAuthInspectFn inspect,
                       uint32_t n_workers, uint64_t crumb_cap, bool sequential);

int rx_world_init_with_auth(RxWorld *w, RxCapRoot *root, const void *auth_ctx,
                            RxAuthValidateFn validate, RxAuthInspectFn inspect,
                            uint32_t n_workers, uint64_t crumb_cap) {
    if (n_workers == 0) return RX_ERR_ARG;
    return init_common(w, root, auth_ctx, validate, inspect, n_workers, crumb_cap, false);
}

/* The R15 sequential reference: the same world with no workers. Only
 * rx_seq_reference.c drives it. Never a production configuration. */
int rx_world_init_sequential_reference(RxWorld *w, const void *auth_ctx,
                                       RxAuthValidateFn validate, RxAuthInspectFn inspect,
                                       uint64_t crumb_cap) {
    return init_common(w, NULL, auth_ctx, validate, inspect, 0, crumb_cap, true);
}

void rx_world_set_timing(RxWorld *w, RxTiming *buf, uint64_t cap) {
    pthread_mutex_lock(&w->mu);
    w->timing = buf;
    w->timing_cap = buf ? cap : 0;
    w->n_timing = 0;
    pthread_mutex_unlock(&w->mu);
}

int rx_world_timing_status(RxWorld *w, uint64_t *stored, uint64_t *attempted) {
    if (!w) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    uint64_t n = w->n_timing, cap = w->timing_cap;
    pthread_mutex_unlock(&w->mu);
    if (stored) *stored = n < cap ? n : cap;
    if (attempted) *attempted = n;
    return n > cap ? RX_ERR_FULL : RX_OK;
}

static int init_common(RxWorld *w, RxCapRoot *root, const void *auth_ctx,
                       RxAuthValidateFn validate, RxAuthInspectFn inspect,
                       uint32_t n_workers, uint64_t crumb_cap, bool sequential) {
    if (!w || n_workers > RX_MAX_WORKERS || crumb_cap == 0 || (n_workers == 0) != sequential)
        return RX_ERR_ARG;
    if (!root && !validate) return RX_ERR_ARG;
    memset(w, 0, sizeof(*w));
    w->root = root;
    pthread_mutex_init(&w->callers_mu, NULL);
    w->sequential = sequential;
    w->auth_ctx = auth_ctx;
    w->auth_validate = validate;
    w->auth_inspect = inspect;
    w->crumbs = crumb_log_map(crumb_cap);
    if (!w->crumbs) return RX_ERR_FULL;
    w->deferred = calloc(RX_DEFERRED_INITIAL, sizeof(*w->deferred));
    if (!w->deferred) {
        crumb_log_unmap(w->crumbs, crumb_cap);
        w->crumbs = NULL;
        return RX_ERR_FULL;
    }
    w->deferred_cap = RX_DEFERRED_INITIAL;
    w->crumb_cap = crumb_cap;
    if (rx_coherent_format(w) != RX_OK) {
        free(w->deferred);
        crumb_log_unmap(w->crumbs, crumb_cap);
        w->deferred = NULL;
        w->crumbs = NULL;
        return RX_ERR_FULL;
    }
    w->budget.slots = RX_MAX_REACTIONS;
    w->budget.memory_bytes = UINT64_MAX;
    w->budget.energy_budget = UINT64_MAX;
    w->budget.offered_locality = UINT32_MAX;
    w->budget.offered_accel = UINT32_MAX;
    w->budget.compute_mask = UINT32_MAX;
    pthread_mutex_init(&w->mu, NULL);
    pthread_cond_init(&w->work_cv, NULL);
    pthread_cond_init(&w->idle_cv, NULL);
    pthread_cond_init(&w->claim_cv, NULL);
    pthread_mutex_lock(&w->mu);
    w->n_workers = n_workers;
    for (uint32_t i = 0; i < n_workers; i++)
        pthread_create(&w->workers[i], NULL, worker_main, w);
    pthread_mutex_unlock(&w->mu);
    return RX_OK;
}

void rx_world_set_resources(RxWorld *w, const RxResourceBudget *budget) {
    if (!w || !budget) return;
    pthread_mutex_lock(&w->mu);
    /* The setter is exact. Zero is a real zero-resource condition, not
     * shorthand for unlimited. rx_world_init installs the permissive default. */
    w->budget = *budget;
    try_admit(w);
    if (w->in_flight == 0) pthread_cond_broadcast(&w->idle_cv);
    pthread_mutex_unlock(&w->mu);
}

void rx_world_set_stability(RxWorld *w, const RxStabilityBudget *stability) {
    if (!w || !stability) return;
    pthread_mutex_lock(&w->mu);
    w->stability = *stability;
    pthread_mutex_unlock(&w->mu);
}

void rx_world_destroy(RxWorld *w) {
    pthread_mutex_lock(&w->mu);
    /* A deferred activation's executor will still call rx_world_resume: let
     * it land before the world goes away. */
    while (w->n_deferred) pthread_cond_wait(&w->idle_cv, &w->mu);
    w->stopping = true;
    pthread_cond_broadcast(&w->work_cv);
    pthread_mutex_unlock(&w->mu);
    for (uint32_t i = 0; i < w->n_workers; i++) pthread_join(w->workers[i], NULL);
    for (uint32_t i = 0; i < RX_MAX_OBJECTS; i++) free(w->subs[i]);
    rx_coherent_free(w);
    free(w->deferred);
    crumb_log_unmap(w->crumbs, w->crumb_cap);
    w->crumbs = NULL;
    pthread_cond_destroy(&w->work_cv);
    pthread_cond_destroy(&w->idle_cv);
    pthread_cond_destroy(&w->claim_cv);
    pthread_mutex_destroy(&w->mu);
    pthread_mutex_destroy(&w->callers_mu);
}

int rx_world_create(RxWorld *w, uint32_t type, RxPersist persist, uint64_t resource,
                    const uint64_t init[RX_MAX_FIELDS], RxObjRef *out) {
    pthread_mutex_lock(&w->mu);
    for (uint32_t i = 0; i < RX_MAX_OBJECTS; i++) {
        RxObject *o = &w->objects[i];
        if (o->live) continue;
        o->live = true;
        o->type = type;
        o->persist = persist;
        o->resource = resource;
        o->placed = false;
        o->window = 0;
        o->region_offset = 0;
        o->size_bytes = 0;
        o->placement = 0;
        o->locality = 0;
        o->coherency = 0;
        o->cap = (RxCapRef){ 0, 0 };
        o->version = 1;
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++) {
            o->field[f] = init ? init[f] : 0;
            o->field_version[f] = 1;
        }
        object_digest(o);
        RxCrumb k;
        memset(&k, 0, sizeof(k));
        k.kind = RX_CRUMB_CREATE;
        k.reaction = UINT32_MAX;
        k.faculty = RX_FACULTY_EXTERNAL;
        k.n_outputs = 1;
        k.outputs[0].obj = (RxObjRef){ i, o->generation };
        k.outputs[0].version = 1;
        k.outputs[0].mask = RX_ALL_FIELDS;
        k.t_start_ns = k.t_end_ns = now_ns();
        uint64_t cid = crumb_append(w, &k);
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++) o->field_writer[f] = cid;
        rx_coherent_project(w, i);
        out->id = i;
        out->generation = o->generation;
        pthread_mutex_unlock(&w->mu);
        return RX_OK;
    }
    pthread_mutex_unlock(&w->mu);
    return RX_ERR_FULL;
}

int rx_world_retire(RxWorld *w, RxObjRef ref) {
    pthread_mutex_lock(&w->mu);
    if (!ref_live(w, ref)) {
        pthread_mutex_unlock(&w->mu);
        return RX_ERR_STALE_GEN;
    }
    RxObject *o = &w->objects[ref.id];
    /* Object generations stay 32 bits: an exhausted one retires the slot
     * rather than wrap, the same rule capability generations follow. */
    if (o->generation == UINT32_MAX) {
        pthread_mutex_unlock(&w->mu);
        return RX_ERR_FULL;
    }
    uint32_t next_gen = o->generation + 1u;
    o->live = false;
    o->generation = next_gen;
    o->cap = (RxCapRef){ 0, 0 };
    rx_coherent_project(w, ref.id);
    RxCrumb k;
    memset(&k, 0, sizeof(k));
    k.kind = RX_CRUMB_RETIRE;
    k.reaction = UINT32_MAX;
    k.n_inputs = 1;
    k.inputs[0].obj = ref;
    k.inputs[0].version = o->version;
    k.inputs[0].mask = RX_ALL_FIELDS;
    k.t_start_ns = k.t_end_ns = now_ns();
    crumb_append(w, &k);
    pthread_mutex_unlock(&w->mu);
    return RX_OK;
}

int rx_world_add_reaction(RxWorld *w, const RxReactionDesc *d, uint32_t *out_id) {
    int seat = d && (d->need.accelerator_features & RX_ACCEL_BLACKWELL) != 0;
    if (!d || (!d->fn && !seat) || d->priority >= RX_PRIORITY_CLASSES || d->n_triggers == 0 ||
        d->n_triggers > RX_MAX_DEPS || d->n_reads > RX_MAX_DEPS ||
        d->n_writes > RX_MAX_WRITES || d->n_caps > RX_MAX_CAPS)
        return RX_ERR_ARG;
    /* The seat reads one object and writes field 0 of a different one. */
    /* A seat still has one data input. R8 may add one capability-slot wake
     * so a blocked seat can resume when a real grant appears. That wake is
     * authority state, never a second GPU operand. */
    int seat_slot_wake = seat && d->n_triggers == 2 &&
        d->triggers[1].mask == (RX_FIELD(0) | RX_FIELD(1) | RX_FIELD(2)) &&
        d->triggers[1].obj.id != d->triggers[0].obj.id &&
        d->triggers[1].obj.id != d->writes[0].obj.id;
    int slot_matches = 0;
    if (seat_slot_wake)
        for (uint32_t i = 0; i < d->n_caps; i++)
            if (d->cap_slotted[i] &&
                d->cap_slot[i].id == d->triggers[1].obj.id &&
                d->cap_slot[i].generation == d->triggers[1].obj.generation)
                slot_matches = 1;
    if (seat && (d->n_writes != 1 || d->writes[0].mask != RX_FIELD(0) ||
                 (d->n_triggers != 1 && (!seat_slot_wake || !slot_matches)) ||
                 d->triggers[0].obj.id == d->writes[0].obj.id ||
                 (d->triggers[0].mask & ~(RX_FIELD(0) | RX_FIELD(1))) != 0 ||
                 d->triggers[0].mask == 0))
        return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    int rc = RX_OK;
    /* R16 C5: the subject is the caller's own only with its credential.
     * C7: an unbound world checks a named credential too, so a guessed
     * generation never survives binding; a reaction that names none is kept
     * as unauthenticated (generation 0), which bind refuses. */
    if (w->callers_bound || d->caller.generation != 0) {
        pthread_mutex_lock(&w->callers_mu);
        int irc = caller_check_locked(w, d->subject, &d->caller);
        pthread_mutex_unlock(&w->callers_mu);
        if (irc != RX_CALLER_OK) { rc = RX_ERR_IDENTITY; goto out; }
    }
    if (w->n_reactions >= RX_MAX_REACTIONS) { rc = RX_ERR_FULL; goto out; }
    /* Declared dependencies must exist now, and every read and write must be
     * covered by a declared capability need on that object's resource. The
     * needs themselves are only validated against the root at run time. */
    for (uint32_t i = 0; i < d->n_triggers; i++)
        if (!ref_live(w, d->triggers[i].obj)) { rc = RX_ERR_STALE_GEN; goto out; }
    for (uint32_t i = 0; i < d->n_reads; i++)
        if (!ref_live(w, d->reads[i].obj)) { rc = RX_ERR_STALE_GEN; goto out; }
    for (uint32_t i = 0; i < d->n_writes; i++)
        if (!ref_live(w, d->writes[i].obj)) { rc = RX_ERR_STALE_GEN; goto out; }
    for (uint32_t i = 0; i < d->n_triggers; i++)
        if (!covered(w, d, d->triggers[i].obj, RX_RIGHT_READ)) { rc = RX_ERR_AUTHORITY; goto out; }
    for (uint32_t i = 0; i < d->n_reads; i++)
        if (!covered(w, d, d->reads[i].obj, RX_RIGHT_READ)) { rc = RX_ERR_AUTHORITY; goto out; }
    for (uint32_t i = 0; i < d->n_writes; i++)
        if (!covered(w, d, d->writes[i].obj, RX_RIGHT_WRITE)) { rc = RX_ERR_AUTHORITY; goto out; }
    /* Index triggers only: a read-only dependency is observed, not a wake. */
    for (uint32_t i = 0; i < d->n_triggers; i++) {
        uint32_t o = d->triggers[i].obj.id;
        if (!w->subs[o]) {
            w->subs[o] = calloc(RX_MAX_SUBS, sizeof(RxSub));
            if (!w->subs[o]) { rc = RX_ERR_FULL; goto out; }
        }
        if (w->n_subs[o] >= RX_MAX_SUBS) { rc = RX_ERR_FULL; goto out; }
    }
    uint32_t rid = w->n_reactions++;
    RxReaction *r = &w->reactions[rid];
    memset(r, 0, sizeof(*r));
    r->desc = *d;
    /* Keep the admitted generation, never the secret. */
    for (size_t i = 0; i < sizeof r->desc.caller.secret; i++)
        ((volatile uint8_t *)r->desc.caller.secret)[i] = 0;
    r->state = RX_DORMANT;
    /* Sequential reference: changes before registration are not work. */
    for (uint32_t i = 0; i < d->n_triggers; i++) {
        const RxObject *o = &w->objects[d->triggers[i].obj.id];
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++)
            if ((d->triggers[i].mask & RX_FIELD(f)) && o->field_version[f] > r->seq_seen[i])
                r->seq_seen[i] = o->field_version[f];
    }
    for (uint32_t i = 0; i < d->n_triggers; i++) {
        uint32_t o = d->triggers[i].obj.id;
        w->subs[o][w->n_subs[o]++] = (RxSub){ rid, d->triggers[i].obj.generation, d->triggers[i].mask };
    }
    if (out_id) *out_id = rid;
out:
    pthread_mutex_unlock(&w->mu);
    return rc;
}

int rx_world_add_reaction_keyed(RxWorld *w, const RxCallerKeyring *keys,
                                const RxReactionDesc *d, uint32_t *out_id) {
    if (!d) return RX_ERR_ARG;
    RxReactionDesc k = *d;
    const RxCallerCred *c = rx_caller_find(keys, d->subject);
    if (c) k.caller = *c;
    int rc = rx_world_add_reaction(w, &k, out_id);
    rx_caller_wipe(&k.caller);
    return rc;
}

int64_t rx_world_publish_external(RxWorld *w, RxCapRef cap, const RxMutation *muts, uint32_t n) {
    if (!muts || n == 0 || n > RX_MAX_MUTATIONS) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    for (uint32_t i = 0; i < n; i++) {
        if (!ref_live(w, muts[i].obj)) { pthread_mutex_unlock(&w->mu); return RX_ERR_STALE_GEN; }
        int rc = rx_world_validate_cap(w, cap, w->external_subject,
                                       w->objects[muts[i].obj.id].resource, RX_RIGHT_WRITE, NULL);
        if (rc != RX_CAP_OK) { pthread_mutex_unlock(&w->mu); return RX_ERR_AUTHORITY; }
    }
    PendingWrite pw[RX_MAX_WRITES];
    uint32_t n_pw = 0;
    int rc = stage_mutations(w, muts, n, NULL, 0, pw, &n_pw);
    if (rc != RX_OK) { pthread_mutex_unlock(&w->mu); return rc; }
    RxCrumb k;
    memset(&k, 0, sizeof(k));
    w->stats.externals++;
    k.kind = RX_CRUMB_EXTERNAL;
    k.reaction = UINT32_MAX;
    k.faculty = RX_FACULTY_EXTERNAL;
    k.n_caps = 1;
    stamp_cap(w, &k, 0, cap);
    k.t_start_ns = now_ns();
    commit_writes(w, pw, n_pw, &k);
    k.t_end_ns = now_ns();
    uint64_t cid = crumb_append(w, &k);
    finish_writes(w, pw, n_pw, cid, false);
    pthread_mutex_unlock(&w->mu);
    return (int64_t)cid;
}

static uint64_t read_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int find_seat(const RxWorld *w, uint32_t seq) {
    for (uint32_t i = 0; i < w->n_reactions; i++) {
        const RxReaction *r = &w->reactions[i];
        if (r->resident_seat && r->state == RX_RUNNING && (uint32_t)r->resident_seq == seq)
            return (int)i;
    }
    return -1;
}

static void seat_fail(RxWorld *w, uint32_t rid, RxCrumb *k, RxCrumbKind kind, int reason) {
    RxReaction *r = &w->reactions[rid];
    if (r->state == RX_RUNNING) set_state(w, r, RX_PUBLISHING);
    if (kind == RX_CRUMB_INVALIDATED) set_state(w, r, RX_INVALIDATED);
    else if (kind == RX_CRUMB_REJECTED) set_state(w, r, RX_REJECTED);
    else set_state(w, r, RX_FAILED);
    k->kind = kind;
    k->reason = reason;
    k->t_end_ns = now_ns();
    crumb_append(w, k);
    r->resident_seat = false;
    w->stats.resident_closed++;
    if (kind == RX_CRUMB_INVALIDATED) w->stats.invalidations++;
    else if (kind == RX_CRUMB_REJECTED) w->stats.rejected++;
    else w->stats.failed++;
    end_activation(w, rid);
}

int rx_resident_accept(RxWorld *w) {
    if (!w || !w->coherent) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    OmegaSharedWorldDesc d;
    int tr = rx_resident_take_result(w, &d);
    if (tr == RX_ERR_NOT_FOUND || tr == RX_ERR_ARG) {
        pthread_mutex_unlock(&w->mu);
        return tr;
    }
    /* A result from a seat that was declared lost. Its claims already ended;
     * nothing it wrote may publish. */
    if (d.producer_generation != w->seat_generation) {
        w->stats.desc_rejected++;
        pthread_mutex_unlock(&w->mu);
        return RX_ERR_STALE_GEN;
    }
    int rid = find_seat(w, d.arg_b);
    if (rid < 0) {
        pthread_mutex_unlock(&w->mu);
        return tr == RX_OK ? RX_ERR_NOT_FOUND : tr;
    }
    RxReaction *r = &w->reactions[rid];
    const RxReactionDesc *desc = &r->desc;
    RxCrumb k;
    memset(&k, 0, sizeof(k));
    k.reaction = (uint32_t)rid;
    k.faculty = desc->faculty;
    k.worker = RX_SEAT_BLACKWELL;
    k.wake_cause = r->resident_parent;
    k.t_start_ns = now_ns();
    k.n_caps = desc->n_caps;
    for (uint32_t i = 0; i < desc->n_caps; i++) stamp_cap(w, &k, i, need_ref(w, desc, i));
    add_parent(&k, r->resident_parent);
    uint32_t in = desc->triggers[0].obj.id;
    uint32_t id = desc->writes[0].obj.id;
    RxObject *a = &w->objects[in];
    RxObject *o = &w->objects[id];
    k.n_inputs = 1;
    k.inputs[0].obj = desc->triggers[0].obj;
    k.inputs[0].version = a->version;
    k.inputs[0].mask = RX_FIELD(0) | RX_FIELD(1);
    if (a->live) {
        add_parent(&k, a->field_writer[0]);
        add_parent(&k, a->field_writer[1]);
    }

    if (tr != RX_OK) {
        if (o->placed) rx_coherent_project(w, id);
        seat_fail(w, (uint32_t)rid, &k, RX_CRUMB_REJECTED, tr);
        pthread_mutex_unlock(&w->mu);
        return tr;
    }
    uint32_t out_id = UINT32_MAX, out_gen = 0;
    if (d.payload_len >= 40) {
        out_id = read_u32(d.payload + 24);
        out_gen = read_u32(d.payload + 28);
    }
    /* A fault notice, or any object that moved since the claim, is stale. */
    if (d.msg_type == RX_RING_FAULT || d.object_id != in || out_id != id ||
        !a->live || !o->live || !a->placed || !o->placed ||
        d.object_generation != a->generation || out_gen != o->generation ||
        a->generation != desc->triggers[0].obj.generation ||
        o->generation != desc->writes[0].obj.generation) {
        if (o->placed) rx_coherent_project(w, id);
        seat_fail(w, (uint32_t)rid, &k, RX_CRUMB_INVALIDATED, RX_ERR_STALE_GEN);
        pthread_mutex_unlock(&w->mu);
        return RX_ERR_STALE_GEN;
    }
    /* Authority is checked again at publication. The notice must echo the
     * capabilities bound at claim time, and both must still validate. */
    int cap_err = 0;
    RxCapRef in_cap = { UINT32_MAX, 0 }, out_cap = { UINT32_MAX, 0 };
    if (d.payload_len >= RX_CAP_PAYLOAD) {
        in_cap = (RxCapRef){ read_u32(d.payload),
                             read_u32(d.payload + 4) |
                                 (uint64_t)read_u32(d.payload + RX_CAP_GEN_HI_A) << 32 };
        out_cap = (RxCapRef){ read_u32(d.payload + 32),
                              read_u32(d.payload + 36) |
                                  (uint64_t)read_u32(d.payload + RX_CAP_GEN_HI_B) << 32 };
    }
    if (in_cap.cap_id != a->cap.cap_id || in_cap.generation != a->cap.generation ||
        out_cap.cap_id != o->cap.cap_id || out_cap.generation != o->cap.generation ||
        validate_caps(w, desc, &cap_err) != 0 ||
        rx_world_validate_cap(w, in_cap, desc->subject, a->resource, RX_RIGHT_READ, NULL) !=
            RX_CAP_OK ||
        rx_world_validate_cap(w, out_cap, desc->subject, o->resource, RX_RIGHT_WRITE, NULL) !=
            RX_CAP_OK) {
        rx_coherent_project(w, id);
        seat_fail(w, (uint32_t)rid, &k, RX_CRUMB_REJECTED, RX_ERR_AUTHORITY);
        pthread_mutex_unlock(&w->mu);
        return RX_ERR_AUTHORITY;
    }
    /* The chip finishing is not the commit. The input window must still
     * match the canonical input, and the output window must hold exactly the
     * sum in field 0 and the canonical values everywhere else. */
    uint64_t parent = read_u64(d.payload + 16);
    const uint8_t *ain = w->coherent + a->region_offset;
    const uint8_t *win = w->coherent + o->region_offset;
    uint64_t f1 = (uint64_t)((uint32_t)a->field[0] + (uint32_t)a->field[1]);
    int torn = parent != r->resident_parent || read_u64(d.payload + 8) != a->version ||
               read_u64(win) != f1;
    for (uint32_t f = 0; f < RX_MAX_FIELDS && !torn; f++)
        if (read_u64(ain + f * 8u) != a->field[f]) torn = 1;
    for (uint32_t f = 1; f < RX_MAX_FIELDS && !torn; f++)
        if (read_u64(win + f * 8u) != o->field[f]) torn = 1;
    if (torn) {
        rx_coherent_project(w, id);
        seat_fail(w, (uint32_t)rid, &k, RX_CRUMB_REJECTED, RX_ERR_TORN);
        pthread_mutex_unlock(&w->mu);
        return RX_ERR_TORN;
    }
    RxMutation mut = { desc->writes[0].obj, 0, f1 };
    PendingWrite pw[RX_MAX_WRITES];
    uint32_t n_pw = 0;
    int src = stage_mutations(w, &mut, 1, desc->writes, desc->n_writes, pw, &n_pw);
    if (src != RX_OK) {
        rx_coherent_project(w, id);
        seat_fail(w, (uint32_t)rid, &k, RX_CRUMB_REJECTED, src);
        pthread_mutex_unlock(&w->mu);
        return src;
    }
    set_state(w, r, RX_PUBLISHING);
    commit_writes(w, pw, n_pw, &k);
    o->coherency = RX_COHERENCY_SEAT;
    k.kind = RX_CRUMB_COMMIT;
    k.t_end_ns = now_ns();
    note_value(w, r, 1, f1);
    uint64_t cid = crumb_append(w, &k);
    set_state(w, r, RX_COMMITTED);
    w->stats.commits++;
    r->commits++;
    r->resident_seat = false;
    w->stats.resident_closed++;
    finish_writes(w, pw, n_pw, cid, false);
    end_activation(w, (uint32_t)rid);
    pthread_mutex_unlock(&w->mu);
    return RX_OK;
}

static void ring_discard(RxWorld *w, uint64_t off) {
    OmegaSharedWorldRing *ring = (OmegaSharedWorldRing *)(w->coherent + off);
    uint64_t t = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
    __atomic_store_n(&ring->head, t, __ATOMIC_RELEASE);
}

int rx_resident_seat_lost(RxWorld *w, int retry) {
    if (!w || !w->coherent) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    /* The old seat's notices go with it. Nothing it left unread is taken by
     * the next seat, and nothing it wrote is accepted. */
    ring_discard(w, rx_world_off_c2g());
    ring_discard(w, rx_world_off_g2c());
    w->seat_generation++;
    w->stats.seat_losses++;

    int lost = 0;
    for (uint32_t rid = 0; rid < w->n_reactions; rid++) {
        RxReaction *r = &w->reactions[rid];
        if (!r->resident_seat || r->state != RX_RUNNING) continue;
        const RxReactionDesc *desc = &r->desc;
        uint32_t in = desc->triggers[0].obj.id;
        uint32_t out = desc->writes[0].obj.id;
        RxCrumb k;
        memset(&k, 0, sizeof(k));
        k.reaction = rid;
        k.faculty = desc->faculty;
        k.worker = RX_SEAT_BLACKWELL;
        k.wake_cause = r->resident_parent;
        k.t_start_ns = now_ns();
        k.n_caps = desc->n_caps;
        for (uint32_t i = 0; i < desc->n_caps; i++) stamp_cap(w, &k, i, need_ref(w, desc, i));
        add_parent(&k, r->resident_parent);
        k.n_inputs = 1;
        k.inputs[0].obj = desc->triggers[0].obj;
        k.inputs[0].version = w->objects[in].version;
        k.inputs[0].mask = RX_FIELD(0) | RX_FIELD(1);
        /* Whatever the chip left in the output window is not the object. */
        if (w->objects[out].placed) rx_coherent_project(w, out);
        set_state(w, r, RX_FAILED);
        k.kind = RX_CRUMB_FAILED;
        k.reason = RX_ERR_SEAT_LOST;
        k.t_end_ns = now_ns();
        uint64_t fid = crumb_append(w, &k);
        r->resident_seat = false;
        w->stats.resident_closed++;
        w->stats.failed++;
        /* A newer wake already asks for another run; keep its cause. */
        if (retry && !r->rearm) {
            r->rearm = true;
            r->wake_cause = fid;
        }
        end_activation(w, rid);
        lost++;
    }
    pthread_mutex_unlock(&w->mu);
    return lost;
}

int rx_resident_wait_outstanding(RxWorld *w, int timeout_ms) {
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += timeout_ms / 1000;
    dl.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&w->mu);
    int rc = RX_OK;
    while (w->stats.resident_claims <= w->stats.resident_closed && !w->stopping) {
        if (pthread_cond_timedwait(&w->claim_cv, &w->mu, &dl) == ETIMEDOUT) {
            rc = RX_ERR_TIMEOUT;
            break;
        }
    }
    pthread_mutex_unlock(&w->mu);
    return rc;
}

/* R15 sequential reference only (rx_seq_reference.c). Caller holds mu. Runs
 * one activation of `rid` on the calling thread through the same wake
 * accounting, admission, validation, snapshot, publication and crumb as a
 * worker would. Refused unless the world is the sequential reference.
 * Returns 1 if it ran (a graphics claim may still be outstanding), 0 if the
 * wake was suppressed or does not fit the budget now, negative on error. */
int rx_world_seq_activate_timed_locked(RxWorld *w, uint32_t rid, uint64_t cause,
                                       uint64_t poll_start, uint64_t poll_wall,
                                       uint64_t poll_cpu);
int rx_world_seq_activate_locked(RxWorld *w, uint32_t rid, uint64_t cause) {
    return rx_world_seq_activate_timed_locked(w, rid, cause, 0, 0, 0);
}

/* As above, with the orchestrator's readiness poll for this stage (wall
 * start, wall and thread-CPU duration) carried into the activation's
 * scheduler time, so SEQ's polling is priced as RES's wake path is. */
int rx_world_seq_activate_timed_locked(RxWorld *w, uint32_t rid, uint64_t cause,
                                       uint64_t poll_start, uint64_t poll_wall,
                                       uint64_t poll_cpu) {
    if (!w || !w->sequential || rid >= w->n_reactions) return RX_ERR_ARG;
    RxReaction *r = &w->reactions[rid];
    if (r->state != RX_DORMANT) return RX_ERR_ARG;
    r->seq_pending = false;
    demand(w, rid, cause);          /* R6 budgets and quarantine, unchanged */
    if (!r->seq_pending) return 0;
    if (r->quarantined || !res_fits(w, r)) return 0;
    uint64_t c0 = w->timing ? thread_cpu_ns() : 0;
    uint64_t t0 = w->timing ? now_ns() : 0;
    if (w->timing) {
        /* The poll that found it ready is SEQ's wake: world totals already
         * hold every poll (rx_seq_pulse); this attributes this one. */
        r->t_demand = poll_start;
        r->sched_ns += poll_wall;
        r->sched_cpu_ns += poll_cpu;
    }
    r->seq_pending = false;
    w->stats.wakes_accepted++;
    set_state(w, r, RX_BLOCKED_RESOURCE);
    r->wake_cause = cause;
    r->coalesced = 0;
    r->wait_seq = ++w->admit_seq;
    w->stats.blocked_resource++;
    set_state(w, r, RX_READY);
    charge(w, r);
    w->in_flight++;
    if (w->timing) {
        /* Admission is scheduling work, as it is for a worker. */
        uint64_t t1 = now_ns();
        uint64_t c1 = thread_cpu_ns();
        r->t_ready = t1;
        r->sched_ns += t1 - t0;
        r->sched_cpu_ns += c1 - c0;
        w->stats.sched_wall_ns += t1 - t0;
        w->stats.sched_cpu_ns += c1 - c0;
    }
    run_one(w, rid, RX_SEQ_WORKER);
    return 1;
}

int rx_world_wait_quiescent(RxWorld *w, int timeout_ms) {
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += timeout_ms / 1000;
    dl.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&w->mu);
    int rc = RX_OK;
    /* Sequential reference: quiescent means a whole pulse that began after
     * this call found nothing ready. */
    uint64_t started = w->seq_pulse_started;
    while (w->in_flight != 0 || w->deferred_len != 0 ||
           (w->sequential && w->seq_idle_start <= started)) {
        if (pthread_cond_timedwait(&w->idle_cv, &w->mu, &dl) == ETIMEDOUT) {
            rc = RX_ERR_TIMEOUT;
            break;
        }
    }
    pthread_mutex_unlock(&w->mu);
    return rc;
}

int rx_world_read(RxWorld *w, RxObjRef ref, RxObject *out) {
    pthread_mutex_lock(&w->mu);
    int rc = ref_live(w, ref) ? RX_OK : RX_ERR_STALE_GEN;
    if (rc == RX_OK) *out = w->objects[ref.id];
    pthread_mutex_unlock(&w->mu);
    return rc;
}

uint64_t rx_world_explain(RxWorld *w, RxObjRef ref, uint32_t field) {
    pthread_mutex_lock(&w->mu);
    uint64_t id = (ref_live(w, ref) && field < RX_MAX_FIELDS)
                      ? w->objects[ref.id].field_writer[field] : 0;
    pthread_mutex_unlock(&w->mu);
    return id;
}

void rx_world_digest(RxWorld *w, uint8_t out[32]) {
    pthread_mutex_lock(&w->mu);
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)"AIEN_RX_WORLD_V1", 16);
    for (uint32_t i = 0; i < RX_MAX_OBJECTS; i++) {
        const RxObject *o = &w->objects[i];
        if (!o->live) continue;
        put32(&c, o->id);
        put32(&c, o->generation);
        sha256_update(&c, o->digest, 32);
    }
    sha256_final(&c, out);
    pthread_mutex_unlock(&w->mu);
}
