/*
 * rx_argus.c -- ARGUS producer (per-thread rings + use tables) and optional
 * consumer (ordered merge -> argus_core) for the reaction runtime, ABI v1.1.
 * See rx_argus.h for the rules. Compiled only when RX_ARGUS >= 1.
 */
#include "rx_argus.h"

#if RX_ARGUS

#include "sha256.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if RX_ARGUS >= 2
#define RX_ARGUS_HAS_CONSUMER 1
#else
#define RX_ARGUS_HAS_CONSUMER 0
#endif

#define BATCH        64u
#define RING_MEM_MAX ((size_t)RX_ARGUS_RING_CAPACITY * ARGUS_EVENT_SIZE + 4096u)
#define AUTH_MAX     8u

/* Relaxed single-writer counter bump (the owner thread is the only writer). */
#define BUMP(x, n) atomic_store_explicit(&(x), atomic_load_explicit(&(x), memory_order_relaxed) + (n), memory_order_relaxed)
#define LOAD(x) atomic_load_explicit(&(x), memory_order_relaxed)

typedef struct {
    RxArgusProducer p;                     /* first: rx_argus_tls points here */
    /* owner side */
    uint64_t seq;                          /* next sequence of this stream */
    uint64_t leftover_key;                 /* stamp of refused, still-counted uses (0 = none) */
    uint16_t stream;                       /* 1..RX_ARGUS_PRODUCERS */
    _Atomic uint64_t emitted, pushed, refused, malformed;
    _Atomic uint64_t emitted_by_kind[ARGUS_EV_KIND_MAX + 1];
    _Atomic uint64_t uses_flushed, summaries, summary_refused, order_slow, mixed_keys;
    uint64_t keys[RX_ARGUS_KEYS];          /* order key by sequence & (KEYS-1) */
    /* shared */
    _Atomic int owned, inited;
    ArgusRing *ring;
    _Alignas(64) uint8_t ring_mem[RING_MEM_MAX];
    /* consumer side */
    ArgusEvent buf[BATCH];
    uint32_t n, pos;
    uint64_t stall_pub;                    /* pub value the consumer stopped waiting for */
} Slot;

_Thread_local RxArgusProducer *rx_argus_tls;
_Atomic uint64_t rx_argus_stamp = 1;

static struct {
    Slot slots[RX_ARGUS_PRODUCERS];
    ArgusCore *core;
    ArgusFinding findings[RX_ARGUS_FINDINGS_MAX];
    _Alignas(64) uint8_t core_mem[1u << 20];
    int active, mode;
    pid_t pid;
    uint8_t machine_id[ARGUS_MACHINE_ID_LEN];
    pthread_key_t exit_key;
    _Atomic uint64_t unregistered_uses, lost_transitions;
    _Atomic uint32_t claimed, live, live_max;
    /* authority observer bookkeeping */
    pthread_mutex_t obs_mu;
    const AienosCapView *auth_view[AUTH_MAX];
    uint64_t auth_live[AUTH_MAX][RX_ARGUS_USE_SLOTS];     /* live generation, 0 = none */
    uint32_t auth_subject[AUTH_MAX][RX_ARGUS_USE_SLOTS];
    uint32_t n_auth;
    _Atomic uint64_t obs_calls[9];
    /* consumer */
    pthread_t thread;
    int thread_started;
    char consumer_cpu[64];                 /* consumer placement as applied (see consumer_place) */
    int consumer_pin_rc;                   /* pthread_setaffinity_np result (0 = ok) */
    _Atomic int stop;
    uint64_t consumer_seq, last_key;
    uint64_t received, synthesized, batches, stream_records, ingest_errors, late, stall_breaks;
    uint64_t hold_ns_max, hold_start;
    uint64_t received_by_kind[ARGUS_EV_KIND_MAX + 1];
    uint64_t findings_total, findings_by_code[ARGUS_F_MAX + 1];
    uint32_t lag_max;
    FILE *stream;
    size_t ring_bytes, core_bytes;
} g;

static inline Slot *slot_of(RxArgusProducer *t) { return (Slot *)(void *)t; }

static inline void cpu_relax(void) {
#if defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__)
    __asm__ __volatile__("pause" ::: "memory");
#endif
}

static __attribute__((unused)) uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- producer slots --------------------------------------------------------- */

static void reset_use(RxArgusUse *u) {
    u->count = 0;
    u->min_gen = UINT64_MAX;
    u->max_gen = 0;
}

static void thread_exit(void *arg);

static RxArgusProducer *claim(void) {
    for (uint32_t i = 0; i < RX_ARGUS_PRODUCERS; i++) {
        Slot *s = &g.slots[i];
        int expect = 0;
        if (atomic_load_explicit(&s->owned, memory_order_relaxed) ||
            !atomic_compare_exchange_strong(&s->owned, &expect, 1))
            continue;
        if (!atomic_load_explicit(&s->inited, memory_order_acquire)) {
            if (argus_ring_init(&s->ring, s->ring_mem, sizeof s->ring_mem, RX_ARGUS_RING_CAPACITY) != ARGUS_OK) {
                atomic_store(&s->owned, 0);
                return NULL;
            }
            s->stream = (uint16_t)(i + 1);
            s->seq = 1;
            for (uint32_t k = 0; k < RX_ARGUS_USE_SLOTS; k++) reset_use(&s->p.use[k]);
            s->p.flush_at = ARGUS_USE_FLUSH_OPS;
            atomic_store_explicit(&s->p.pub, RX_ARGUS_IDLE, memory_order_relaxed);
            atomic_fetch_add(&g.claimed, 1);
            atomic_store_explicit(&s->inited, 1, memory_order_release);
        }
        s->p.epoch = 0;
        rx_argus_tls = &s->p;
        pthread_setspecific(g.exit_key, s);
        uint32_t live = atomic_fetch_add(&g.live, 1) + 1;
        uint32_t mx = atomic_load(&g.live_max);
        while (live > mx && !atomic_compare_exchange_weak(&g.live_max, &mx, live)) {}
        return &s->p;
    }
    return NULL;
}

static inline void fill(ArgusEvent *ev, uint16_t kind, int code, uint8_t outcome) {
    memset(ev, 0, sizeof *ev);
    ev->version = ARGUS_ABI_VERSION;
    ev->kind = kind;
    ev->class_ = argus_event_min_class(kind);
    if (!ev->class_) ev->class_ = ARGUS_CLASS_CRITICAL;
    ev->effect_class = ARGUS_EFFECT_NONE;
    ev->outcome = outcome;
    ev->code = code;
    ev->cap_id = ARGUS_CAP_NONE;           /* callers set a real cap (0 = office) */
    memcpy(ev->machine_id, g.machine_id, ARGUS_MACHINE_ID_LEN);
}

/* Owner-only: push one event on this slot's ring with its order key. The
 * sequence is consumed only on success, so keys[seq & (KEYS-1)] never
 * overwrites an entry still in the ring (the ring holds <= KEYS/2). */
static int push(Slot *s, ArgusEvent *ev, uint64_t key) {
    ev->sequence = s->seq;
    ev->flags = (uint16_t)((ev->flags & ~ARGUS_FLAG_STREAM_MASK) |
                           ((uint32_t)s->stream << ARGUS_FLAG_STREAM_SHIFT));
    s->keys[s->seq & (RX_ARGUS_KEYS - 1)] = key;
    int rc = argus_ring_push(s->ring, ev);
    BUMP(s->emitted, 1);
    if (ev->kind <= ARGUS_EV_KIND_MAX) BUMP(s->emitted_by_kind[ev->kind], 1);
    if (rc == ARGUS_OK) {
        s->seq++;
        BUMP(s->pushed, 1);
    } else if (rc == ARGUS_ERR_FULL) {
        BUMP(s->refused, 1);
    } else {
        BUMP(s->malformed, 1);
    }
    return rc;
}

/* Owner-only: one summary per touched slot. Refused summaries keep their counts. */
static void flush_all(Slot *s) {
    RxArgusProducer *t = &s->p;
    uint64_t stamp = s->leftover_key ? s->leftover_key : t->epoch;
    int left = 0;
    for (uint32_t c = 0; c < RX_ARGUS_USE_SLOTS; c++) {
        RxArgusUse *u = &t->use[c];
        if (!u->count) continue;
        ArgusEvent ev;
        fill(&ev, ARGUS_EV_CAPABILITY_USE_SUMMARY, 0, ARGUS_OUTCOME_OK);
        ev.principal = u->principal;
        ev.cap_id = c;
        ev.resource = u->count;
        ev.cap_generation = u->max_gen;     /* MAX generation seen */
        ev.world_generation = u->min_gen;   /* MIN generation seen (v1.1, 398cfb9); object_id 0 */
        BUMP(s->summaries, 1);
        if (push(s, &ev, 2 * stamp) == ARGUS_OK) {
            BUMP(s->uses_flushed, u->count);
            reset_use(u);
        } else {
            BUMP(s->summary_refused, 1);
            left = 1;
        }
    }
    s->leftover_key = left ? stamp : 0;
    t->flush_at = t->ops + ARGUS_USE_FLUSH_OPS;
    atomic_store_explicit(&t->flush_req, 0, memory_order_relaxed);
}

void rx_argus_flush_self(void) {
    RxArgusProducer *t = rx_argus_tls;
    if (t) flush_all(slot_of(t));
}

/* Publish "idle": nothing pending, nothing in flight. */
static void go_idle(Slot *s) {
    s->p.epoch = 0;
    atomic_store_explicit(&s->p.pub, RX_ARGUS_IDLE, memory_order_release);
}

uint64_t rx_argus_use_begin_slow(uint64_t e) {
    if (!g.active) return e;
    RxArgusProducer *t = rx_argus_tls;
    if (!t && !(t = claim())) return e;
    Slot *s = slot_of(t);
    flush_all(s);                         /* pending uses keep their old key */
    if (s->leftover_key) BUMP(s->mixed_keys, 1);
    /* Dekker with the consumer: publish, fence, re-read. The key returned is
     * read before the caller's validate, and pub <= key. */
    atomic_store_explicit(&t->pub, e, memory_order_seq_cst);
    atomic_thread_fence(memory_order_seq_cst);
    uint64_t e2 = atomic_load_explicit(&rx_argus_stamp, memory_order_seq_cst);
    t->epoch = e2;
    return e2;
}

/* A transition: flush, take a stamp (unless reserved), push with key 2s+1, go idle. */
static void transition(ArgusEvent *ev, const AienosCapView *view) {
    if (!g.active) return;
    RxArgusProducer *t = rx_argus_tls;
    if (!t && !(t = claim())) {
        atomic_fetch_add(&g.lost_transitions, 1);
        return;
    }
    Slot *s = slot_of(t);
    flush_all(s);
    uint64_t st = atomic_fetch_add_explicit(&rx_argus_stamp, 1, memory_order_seq_cst);
    ev->tick = view ? aienos_cap_clock(view) : 0;
    atomic_store_explicit(&t->pub, st, memory_order_seq_cst);
    push(s, ev, 2 * st + 1);
    if (!s->leftover_key) go_idle(s);
}

void rx_argus_use_slow(uint64_t key, const AienosCapView *view, uint32_t subject, uint32_t cap_id,
                       uint64_t gen, uint64_t resource, int code) {
    if (!g.active) return;
    RxArgusProducer *t = rx_argus_tls;
    if (!t) {
        atomic_fetch_add(&g.unregistered_uses, 1);
        return;
    }
    Slot *s = slot_of(t);
    if (code == 0 && cap_id < RX_ARGUS_USE_SLOTS && key == t->epoch) {
        /* principal changed on this cap: close the old summary first */
        RxArgusUse *u = &t->use[cap_id];
        flush_all(s);
        if (u->count == 0) {
            u->principal = subject;
            u->count = 1;
            u->min_gen = u->max_gen = gen;
            t->ops++;
            return;
        }
    } else if (code == 0 && key != t->epoch) {
        BUMP(s->order_slow, 1);
    }
    /* failed validate, cap outside the table, or no table room: one full USED
     * event. It is keyed like the use it is (2*key, tick 0, no stamp), so a
     * denial never forces other threads to flush; the price is that a USED/DENIED
     * caused by a revoke may be ordered just before that REVOKED (no detector
     * reads outcome-DENIED uses). */
    ArgusEvent ev;
    fill(&ev, ARGUS_EV_CAPABILITY_USED, code, code == 0 ? ARGUS_OUTCOME_OK : ARGUS_OUTCOME_DENIED);
    ev.principal = subject;
    ev.cap_id = cap_id;
    ev.cap_generation = gen;
    ev.resource = resource;
    if (key == t->epoch && !s->leftover_key) {
        (void)view;
        push(s, &ev, 2 * key);
        return;
    }
    transition(&ev, view);
}

int rx_argus_idle_pending(void) {
    RxArgusProducer *t = rx_argus_tls;
    return t && g.active && (t->epoch != 0 || slot_of(t)->leftover_key != 0);
}

void rx_argus_idle(void) {
    RxArgusProducer *t = rx_argus_tls;
    if (!t || !g.active) return;
    Slot *s = slot_of(t);
    if (t->epoch == 0 && !s->leftover_key) return;
    flush_all(s);
    if (!s->leftover_key) go_idle(s);
}

static void thread_exit(void *arg) {
    Slot *s = arg;
    if (!s) return;
    if (g.active && g.pid == getpid()) {
        flush_all(s);
        go_idle(s);
    }
    rx_argus_tls = NULL;
    atomic_fetch_sub(&g.live, 1);
    atomic_store_explicit(&s->owned, 0, memory_order_release);
}

/* ---- transitions ---------------------------------------------------------------- */

void rx_argus_emit_cap_granted(const AienosCapView *view, uint32_t subject, uint32_t cap_id,
                               uint64_t cap_generation, uint64_t resource, uint32_t rights,
                               int code) {
    ArgusEvent ev;
    fill(&ev, ARGUS_EV_CAPABILITY_GRANTED, code, code == 0 ? ARGUS_OUTCOME_OK : ARGUS_OUTCOME_ERROR);
    ev.principal = subject;
    ev.cap_id = cap_id;
    ev.cap_generation = cap_generation;
    ev.object_id = rights;
    ev.resource = resource;
    transition(&ev, view);
}

void rx_argus_emit_cap_denied(const AienosCapView *view, uint32_t subject, uint32_t cap_id,
                              uint64_t cap_generation, uint64_t resource, uint32_t rights,
                              int code) {
    ArgusEvent ev;
    fill(&ev, ARGUS_EV_CAPABILITY_DENIED, code, ARGUS_OUTCOME_DENIED);
    ev.principal = subject;
    ev.cap_id = cap_id;
    ev.cap_generation = cap_generation;
    ev.object_id = rights;
    ev.resource = resource;
    transition(&ev, view);
}

void rx_argus_emit_cap_revoked(const AienosCapView *view, uint32_t subject, uint32_t cap_id,
                               uint64_t cap_generation, int code) {
    ArgusEvent ev;
    fill(&ev, ARGUS_EV_CAPABILITY_REVOKED, code, code == 0 ? ARGUS_OUTCOME_OK : ARGUS_OUTCOME_ERROR);
    ev.principal = subject;
    ev.cap_id = cap_id;
    ev.cap_generation = cap_generation;
    transition(&ev, view);
}

void rx_argus_emit_world_committed(uint32_t store_id, uint32_t subject, uint32_t cap_id,
                                   uint64_t cap_generation, uint64_t world_generation,
                                   const uint8_t digest[32], int code) {
    ArgusEvent ev;
    fill(&ev, ARGUS_EV_WORLD_COMMITTED, code, code == 0 ? ARGUS_OUTCOME_OK : ARGUS_OUTCOME_ERROR);
    ev.principal = subject;
    ev.cap_id = cap_id;
    ev.cap_generation = cap_generation;
    ev.world_generation = world_generation;
    ev.object_id = store_id;
    if (digest) memcpy(ev.evidence_digest, digest, ARGUS_DIGEST_LEN);
    transition(&ev, NULL);
}

uint32_t rx_argus_store_id(const char *dir) {
    uint8_t d[32];
    sha256_ctx h;
    sha256_init(&h);
    sha256_update(&h, (const uint8_t *)"ARGUS-STORE-v1", 14);
    if (dir) sha256_update(&h, (const uint8_t *)dir, strlen(dir));
    sha256_final(&h, d);
    uint32_t id = (uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24;
    return id ? id : 1u;
}

/* ---- consumer ------------------------------------------------------------------------ */

#if RX_ARGUS_HAS_CONSUMER
static void consume_one(const ArgusEvent *ev, uint64_t key) {
    g.received++;
    if (key < g.last_key) g.late++;
    else g.last_key = key;
    if (ev->kind <= ARGUS_EV_KIND_MAX) g.received_by_kind[ev->kind]++;
    if (g.stream) {
        uint8_t rec[ARGUS_EVENT_SIZE];
        argus_event_encode(ev, rec);
        if (fwrite(rec, sizeof rec, 1, g.stream) == 1) g.stream_records++;
    }
    if (g.mode != RX_ARGUS_CONSUMER_INGEST) return;
    ArgusFinding f[32];
    size_t n = 0;
    int rc = argus_core_ingest(g.core, ev, f, 32, &n);
    if (rc != ARGUS_OK) g.ingest_errors++;
    for (size_t i = 0; i < n; i++) {
        if (g.findings_total < RX_ARGUS_FINDINGS_MAX) g.findings[g.findings_total] = f[i];
        g.findings_total++;
        if (f[i].code <= ARGUS_F_MAX) g.findings_by_code[f[i].code]++;
    }
}

static inline int inited(uint32_t i) {
    return atomic_load_explicit(&g.slots[i].inited, memory_order_acquire);
}

static void refill(Slot *s) {
    if (s->pos < s->n) return;
    ArgusRingStats st;
    argus_ring_stats(s->ring, &st);
    if (st.depth > g.lag_max) g.lag_max = st.depth;
    s->n = (uint32_t)argus_ring_pop_batch(s->ring, s->buf, BATCH);
    s->pos = 0;
    if (s->n) g.batches++;
}

static inline uint64_t head_key(const Slot *s) {
    return s->keys[s->buf[s->pos].sequence & (RX_ARGUS_KEYS - 1)];
}

/* One merge pass. final = every producer is quiescent (shutdown): no bounds. */
static size_t merge(int final) {
    size_t done = 0;
    for (uint32_t i = 0; i < RX_ARGUS_PRODUCERS; i++)
        if (inited(i)) refill(&g.slots[i]);
    for (;;) {
        uint32_t best = UINT32_MAX;
        uint64_t best_key = UINT64_MAX;
        for (uint32_t i = 0; i < RX_ARGUS_PRODUCERS; i++) {
            if (!inited(i)) continue;
            Slot *s = &g.slots[i];
            if (s->pos < s->n && head_key(s) < best_key) { best_key = head_key(s); best = i; }
        }
        if (best == UINT32_MAX) break;
        /* lower bound of every producer without a buffered head: read pub, then re-poll */
        uint64_t bound = UINT64_MAX;
        uint32_t blocker = UINT32_MAX;
        int refilled = 0;
        for (uint32_t i = 0; i < RX_ARGUS_PRODUCERS && !final; i++) {
            if (!inited(i)) continue;
            Slot *s = &g.slots[i];
            if (s->pos < s->n) continue;
            uint64_t pub = atomic_load_explicit(&s->p.pub, memory_order_seq_cst);
            refill(s);
            if (s->pos < s->n) { refilled = 1; break; }
            if (pub == RX_ARGUS_IDLE || pub == s->stall_pub) continue;
            if (2 * pub < bound) { bound = 2 * pub; blocker = i; }
        }
        if (refilled) continue;
        if (best_key <= bound) {
            Slot *s = &g.slots[best];
            consume_one(&s->buf[s->pos], best_key);
            s->pos++;
            done++;
            if (g.hold_start) {
                uint64_t h = now_ns() - g.hold_start;
                if (h > g.hold_ns_max) g.hold_ns_max = h;
                g.hold_start = 0;
            }
            if (s->pos == s->n) refill(s);
            continue;
        }
        /* blocked by a producer that may still push a smaller key */
        uint64_t now = now_ns();
        if (!g.hold_start) g.hold_start = now;
        if (now - g.hold_start >= RX_ARGUS_STALL_NS) {
            g.slots[blocker].stall_pub = atomic_load(&g.slots[blocker].p.pub);
            g.stall_breaks++;
            continue;
        }
        break;
    }
    /* ring refusals -> TELEMETRY_DROPPED on the consumer stream */
    for (uint32_t i = 0; i < RX_ARGUS_PRODUCERS; i++) {
        if (!inited(i)) continue;
        ArgusEvent drops[ARGUS_CLASS_MAX + 1];
        size_t d = argus_ring_drain_drops(g.slots[i].ring, drops, ARGUS_CLASS_MAX + 1, &g.consumer_seq);
        g.synthesized += d;
        for (size_t k = 0; k < d; k++) consume_one(&drops[k], g.last_key);
        done += d;
    }
    return done;
}

static void *consumer_main(void *arg) {
    (void)arg;
    unsigned idle = 0;
    uint64_t next_flush = now_ns() + RX_ARGUS_FLUSH_NS;
    while (!atomic_load_explicit(&g.stop, memory_order_acquire)) {
        size_t n = merge(0);
        uint64_t now = (idle & 15) == 0 ? now_ns() : 0;
        if (now >= next_flush) {
            for (uint32_t i = 0; i < RX_ARGUS_PRODUCERS; i++)
                if (inited(i) && atomic_load_explicit(&g.slots[i].p.pub, memory_order_relaxed) != RX_ARGUS_IDLE)
                    atomic_store_explicit(&g.slots[i].p.flush_req, 1, memory_order_relaxed);
            next_flush = now + RX_ARGUS_FLUSH_NS;
        }
        if (n) { idle = 0; continue; }
        if (++idle < 64) { cpu_relax(); continue; }
        /* idle back-off: 20 us doubling to 160 us (summaries are already
         * flush-bounded at RX_ARGUS_FLUSH_NS; a busy poll only burns a core) */
        unsigned shift = idle - 64 < 3 ? idle - 64 : 3;
        long ns = 20000L << shift;
        struct timespec ts = { 0, ns };
        nanosleep(&ts, NULL);
    }
    while (merge(1)) {}
    return NULL;
}
#endif

/* ---- lifecycle ------------------------------------------------------------------------ */

#if RX_ARGUS_HAS_CONSUMER
/* Consumer placement (speed2 fix a). R8 measured the consumer's cost as core sharing:
 * unpinned it rides the workload's cores (+5% wall), pinned off them +3%.
 *   RX_ARGUS_CONSUMER_CPU unset/"auto" (default): if the process affinity mask at start
 *       leaves some online CPUs out, pin the consumer to those CPUs (a core the workload
 *       does not use); if the process may use every CPU, leave the consumer unpinned.
 *   RX_ARGUS_CONSUMER_CPU="none": never pin.
 *   RX_ARGUS_CONSUMER_CPU="N" or "N-M,K": pin to exactly those CPUs (may lie outside the
 *       process mask; that is the point).
 * Deployment requirement: give the consumer a core the workload does not use (a taskset
 * workload gets that automatically). A bad list or a failed pin leaves the consumer
 * unpinned and is reported (stderr + summary consumer_pin_rc), never fatal. */
static int consumer_auto_set(cpu_set_t *out) {
    cpu_set_t mask;
    CPU_ZERO(out);
    if (sched_getaffinity(0, sizeof mask, &mask) != 0) return 0;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    int any = 0;
    for (long c = 0; c < n && c < CPU_SETSIZE; c++)
        if (!CPU_ISSET((int)c, &mask)) { CPU_SET((int)c, out); any = 1; }
    return any;
}

static int consumer_pin(pthread_t th, const char *spec) {
    cpu_set_t set;
    CPU_ZERO(&set);
    const char *p = spec;
    int any = 0;
    while (*p) {
        char *e;
        long a = strtol(p, &e, 10), b;
        if (e == p || a < 0 || a >= CPU_SETSIZE) return -1;
        b = a;
        if (*e == '-') {
            p = e + 1;
            b = strtol(p, &e, 10);
            if (e == p || b < a || b >= CPU_SETSIZE) return -1;
        }
        for (long c = a; c <= b; c++) CPU_SET((int)c, &set);
        any = 1;
        if (*e == ',') e++;
        else if (*e) return -1;
        p = e;
    }
    if (!any) return -1;
    return pthread_setaffinity_np(th, sizeof set, &set);
}

static void consumer_place(pthread_t th) {
    const char *cpu = getenv("RX_ARGUS_CONSUMER_CPU");
    if (cpu && strcmp(cpu, "none") == 0) {
        snprintf(g.consumer_cpu, sizeof g.consumer_cpu, "none");
        return;
    }
    if (!cpu || !*cpu || strcmp(cpu, "auto") == 0) {
        cpu_set_t set;
        if (!consumer_auto_set(&set)) {
            snprintf(g.consumer_cpu, sizeof g.consumer_cpu, "auto:unpinned");
            return;
        }
        size_t o = (size_t)snprintf(g.consumer_cpu, sizeof g.consumer_cpu, "auto:");
        for (int c = 0; c < CPU_SETSIZE && o < sizeof g.consumer_cpu - 8; c++) {
            if (!CPU_ISSET(c, &set)) continue;
            int e = c;
            while (e + 1 < CPU_SETSIZE && CPU_ISSET(e + 1, &set)) e++;
            o += (size_t)snprintf(g.consumer_cpu + o, sizeof g.consumer_cpu - o,
                                  e > c ? "%s%d-%d" : "%s%d", o > 5 ? "," : "", c, e);
            c = e;
        }
        g.consumer_pin_rc = pthread_setaffinity_np(th, sizeof set, &set);
    } else {
        snprintf(g.consumer_cpu, sizeof g.consumer_cpu, "%s", cpu);
        g.consumer_pin_rc = consumer_pin(th, cpu);
    }
    if (g.consumer_pin_rc != 0)
        fprintf(stderr, "rx_argus: consumer placement %s not applied (%d); consumer unpinned\n",
                g.consumer_cpu, g.consumer_pin_rc);
}
#endif

static int rx_argus_start(const char *run_id, int consumer_mode, const char *stream_path) {
    if (g.active) return ARGUS_ERR_STATE;
    if (argus_ring_footprint(RX_ARGUS_RING_CAPACITY) > RING_MEM_MAX) return ARGUS_ERR_ARG;
    g.ring_bytes = argus_ring_footprint(RX_ARGUS_RING_CAPACITY);
    pthread_mutex_init(&g.obs_mu, NULL);
    if (pthread_key_create(&g.exit_key, thread_exit) != 0) return ARGUS_ERR_STATE;
#if RX_ARGUS_HAS_CONSUMER
    g.mode = consumer_mode;
    if (g.mode == RX_ARGUS_CONSUMER_INGEST) {
        g.core_bytes = argus_core_footprint();
        if (g.core_bytes > sizeof g.core_mem ||
            argus_core_init(&g.core, g.core_mem, sizeof g.core_mem) != ARGUS_OK)
            return ARGUS_ERR_ARG;
    }
    if (stream_path && g.mode != RX_ARGUS_CONSUMER_OFF) {
        g.stream = fopen(stream_path, "wb");
        if (!g.stream) return ARGUS_ERR_ARG;
    }
#else
    (void)consumer_mode;
    (void)stream_path;
    g.mode = RX_ARGUS_CONSUMER_OFF;
#endif
    {   /* Provisional machine id: SHA-256("ARGUS-PROVISIONAL-MACHINE-v1" || run_id). */
        static const char tag[] = "ARGUS-PROVISIONAL-MACHINE-v1";
        sha256_ctx h;
        sha256_init(&h);
        sha256_update(&h, (const uint8_t *)tag, sizeof tag - 1);
        if (run_id) sha256_update(&h, (const uint8_t *)run_id, strlen(run_id));
        sha256_final(&h, g.machine_id);
    }
    g.consumer_seq = 1;
    g.pid = getpid();
    g.active = 1;

    ArgusEvent ev;
    fill(&ev, ARGUS_EV_MACHINE_JOINED, 0, ARGUS_OUTCOME_OK);
    ev.object_id = ARGUS_TRUST_OBSERVED;
    transition(&ev, NULL);

#if RX_ARGUS_HAS_CONSUMER
    if (g.mode != RX_ARGUS_CONSUMER_OFF) {
        atomic_store(&g.stop, 0);
        if (pthread_create(&g.thread, NULL, consumer_main, NULL) != 0) {
            g.active = 0;
            return ARGUS_ERR_STATE;
        }
        g.thread_started = 1;
        consumer_place(g.thread);
    }
#endif
    return ARGUS_OK;
}

int rx_argus_active(void) { return g.active; }

/* Assumes producers are quiescent (workers joined): flushes every slot's
 * table from this thread, then stops the consumer after a final drain. */
void rx_argus_shutdown(void) {
    if (!g.active || g.pid != getpid()) return;
    for (uint32_t i = 0; i < RX_ARGUS_PRODUCERS; i++) {
        Slot *s = &g.slots[i];
        if (!atomic_load(&s->inited)) continue;
        flush_all(s);
        go_idle(s);
    }
#if RX_ARGUS_HAS_CONSUMER
    if (g.thread_started) {
        atomic_store_explicit(&g.stop, 1, memory_order_release);
        pthread_join(g.thread, NULL);
        g.thread_started = 0;
    }
    if (g.stream) {
        fclose(g.stream);
        g.stream = NULL;
    }
#endif
    g.active = 0;
}

void rx_argus_stats(RxArgusStats *o) {
    memset(o, 0, sizeof *o);
    for (uint32_t i = 0; i < RX_ARGUS_PRODUCERS; i++) {
        Slot *s = &g.slots[i];
        if (!atomic_load(&s->inited)) continue;
        o->emitted += LOAD(s->emitted);
        o->pushed += LOAD(s->pushed);
        o->ring_refused += LOAD(s->refused);
        o->ring_malformed += LOAD(s->malformed);
        o->uses_counted += LOAD(s->uses_flushed);
        o->summaries_emitted += LOAD(s->summaries);
        o->summary_refused += LOAD(s->summary_refused);
        o->order_slow += LOAD(s->order_slow);
        o->mixed_keys += LOAD(s->mixed_keys);
        for (unsigned k = 0; k <= ARGUS_EV_KIND_MAX; k++) o->emitted_by_kind[k] += LOAD(s->emitted_by_kind[k]);
    }
    o->unregistered_uses = atomic_load(&g.unregistered_uses);
    o->producers_claimed = atomic_load(&g.claimed);
    o->producers_max_live = atomic_load(&g.live_max);
    for (unsigned k = 0; k <= ARGUS_EV_KIND_MAX; k++) o->received_by_kind[k] = g.received_by_kind[k];
    o->received = g.received;
    o->synthesized = g.synthesized;
    o->batches = g.batches;
    o->stream_records = g.stream_records;
    o->ingest_errors = g.ingest_errors;
    o->findings_total = g.findings_total;
    for (unsigned c = 0; c <= ARGUS_F_MAX; c++) o->findings_by_code[c] = g.findings_by_code[c];
    o->findings_kept = g.findings_total < RX_ARGUS_FINDINGS_MAX ? g.findings_total : RX_ARGUS_FINDINGS_MAX;
    o->late_events = g.late;
    o->stall_breaks = g.stall_breaks;
    o->hold_ns_max = g.hold_ns_max;
    o->lag_max = g.lag_max;
    for (unsigned k = 0; k < 9; k++) o->authority_obs[k] = atomic_load(&g.obs_calls[k]);
    o->consumer_mode = g.mode;
    o->ring_bytes = g.ring_bytes;
    o->core_bytes = g.core_bytes;
    o->findings_bytes = sizeof g.findings;
    o->producer_bytes = sizeof(Slot);
#if RX_ARGUS_HAS_CONSUMER
    if (g.core) argus_core_health(g.core, &o->core);
#endif
}

size_t rx_argus_findings(ArgusFinding *out, size_t max) {
    size_t n = g.findings_total < RX_ARGUS_FINDINGS_MAX ? g.findings_total : RX_ARGUS_FINDINGS_MAX;
    if (n > max) n = max;
    if (n) memcpy(out, g.findings, n * sizeof *out);
    return n;
}

static const char *kind_name(unsigned k) {
    switch (k) {
    case ARGUS_EV_CAPABILITY_GRANTED: return "CAPABILITY_GRANTED";
    case ARGUS_EV_CAPABILITY_USED: return "CAPABILITY_USED";
    case ARGUS_EV_CAPABILITY_DENIED: return "CAPABILITY_DENIED";
    case ARGUS_EV_CAPABILITY_REVOKED: return "CAPABILITY_REVOKED";
    case ARGUS_EV_MACHINE_JOINED: return "MACHINE_JOINED";
    case ARGUS_EV_WORLD_COMMITTED: return "WORLD_COMMITTED";
    case ARGUS_EV_TELEMETRY_DROPPED: return "TELEMETRY_DROPPED";
    case ARGUS_EV_CAPABILITY_USE_SUMMARY: return "CAPABILITY_USE_SUMMARY";
    default: return NULL;
    }
}

#define U(x) ((unsigned long long)(x))

int rx_argus_write_summary(const char *path, const char *suite) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    RxArgusStats s;
    rx_argus_stats(&s);
    const char *auth =
#if defined(RX_ARGUS_AUTHORITY_OBSERVER)
        "observer";
#elif defined(RX_ARGUS_AUTHORITY_WRAP)
        "wrap";
#else
        "rx_aegis";
#endif
    fprintf(f, "{\n  \"suite\": \"%s\",\n  \"rx_argus\": %d,\n  \"consumer_mode\": %d,\n"
               "  \"grant_source\": \"%s\",\n", suite ? suite : "", RX_ARGUS, s.consumer_mode, auth);
    fprintf(f, "  \"consumer_cpu\": \"%s\",\n  \"consumer_pin_rc\": %d,\n", g.consumer_cpu, g.consumer_pin_rc);
    fprintf(f, "  \"machine_id\": \"");
    for (unsigned i = 0; i < ARGUS_MACHINE_ID_LEN; i++) fprintf(f, "%02x", g.machine_id[i]);
    fprintf(f, "\",\n  \"producers_claimed\": %u,\n  \"producers_max_live\": %u,\n",
            s.producers_claimed, s.producers_max_live);
    fprintf(f, "  \"emitted\": %llu,\n  \"pushed\": %llu,\n  \"ring_refused\": %llu,\n"
               "  \"ring_malformed\": %llu,\n  \"uses_counted\": %llu,\n  \"summaries\": %llu,\n"
               "  \"summary_refused\": %llu,\n  \"unregistered_uses\": %llu,\n  \"lost_transitions\": %llu,\n"
               "  \"order_slow\": %llu,\n  \"mixed_keys\": %llu,\n",
            U(s.emitted), U(s.pushed), U(s.ring_refused), U(s.ring_malformed), U(s.uses_counted),
            U(s.summaries_emitted), U(s.summary_refused), U(s.unregistered_uses),
            U(atomic_load(&g.lost_transitions)), U(s.order_slow), U(s.mixed_keys));
    fprintf(f, "  \"received\": %llu,\n  \"synthesized\": %llu,\n  \"stream_records\": %llu,\n"
               "  \"lag_max_depth\": %u,\n  \"hold_ns_max\": %llu,\n  \"late_events\": %llu,\n"
               "  \"stall_breaks\": %llu,\n  \"batches\": %llu,\n  \"ingest_errors\": %llu,\n",
            U(s.received), U(s.synthesized), U(s.stream_records), s.lag_max, U(s.hold_ns_max),
            U(s.late_events), U(s.stall_breaks), U(s.batches), U(s.ingest_errors));
    fprintf(f, "  \"authority_observer_calls\": { \"mint\": %llu, \"revoke\": %llu, \"reclaim\": %llu, "
               "\"epoch\": %llu, \"clock\": %llu, \"kill\": %llu, \"restart\": %llu },\n",
            U(s.authority_obs[1]), U(s.authority_obs[2]), U(s.authority_obs[3]), U(s.authority_obs[4]),
            U(s.authority_obs[5]), U(s.authority_obs[6]), U(s.authority_obs[7]));
    fprintf(f, "  \"by_kind\": {");
    int first = 1;
    for (unsigned k = 0; k <= ARGUS_EV_KIND_MAX; k++) {
        if (!s.received_by_kind[k] && !s.emitted_by_kind[k]) continue;
        const char *nm = kind_name(k);
        fprintf(f, "%s\n    \"%s%s%u\": { \"emitted\": %llu, \"received\": %llu }", first ? "" : ",",
                nm ? nm : "", nm ? "_" : "kind_", k, U(s.emitted_by_kind[k]), U(s.received_by_kind[k]));
        first = 0;
    }
    fprintf(f, "\n  },\n  \"core\": { \"events_received\": %llu, \"events_rejected\": %llu, "
               "\"findings_emitted\": %llu, \"incidents_open\": %llu, \"events_not_applied\": %llu, "
               "\"tables_full\": %llu, \"producers_untracked\": %llu },\n",
            U(s.core.events_received), U(s.core.events_rejected), U(s.core.findings_emitted),
            U(s.core.incidents_open), U(s.core.events_not_applied), U(s.core.tables_full),
            U(s.core.producers_untracked));
    fprintf(f, "  \"memory_bytes\": { \"per_producer_slot\": %zu, \"ring_per_producer\": %zu, "
               "\"producers_touched\": %zu, \"core\": %zu, \"findings\": %zu, \"static_reserved\": %zu },\n",
            s.producer_bytes, s.ring_bytes, (size_t)s.producers_claimed * s.producer_bytes, s.core_bytes,
            s.findings_bytes, sizeof g);
    fprintf(f, "  \"findings_total\": %llu,\n  \"findings_by_code\": {", U(s.findings_total));
    first = 1;
    for (unsigned c = 0; c <= ARGUS_F_MAX; c++) {
        if (!s.findings_by_code[c]) continue;
        fprintf(f, "%s \"%u\": %llu", first ? "" : ",", c, U(s.findings_by_code[c]));
        first = 0;
    }
    fprintf(f, " },\n  \"findings\": [");
    for (uint64_t i = 0; i < s.findings_kept; i++) {
        const ArgusFinding *x = &g.findings[i];
        fprintf(f, "%s\n    { \"code\": %u, \"severity\": %u, \"sequence\": %llu, "
                   "\"prior_sequence\": %llu, \"principal\": %u, \"cap_id\": %u, \"cap_generation\": %llu }",
                i ? "," : "", x->code, x->severity, U(x->sequence), U(x->prior_sequence), x->principal,
                x->cap_id, U(x->cap_generation));
    }
    fprintf(f, "\n  ]\n}\n");
    fclose(f);
    return 0;
}

/* ---- automatic process-wide instance -----------------------------------------------
 * Existing suites are linked unchanged: a constructor starts ARGUS unless
 * RX_ARGUS_AUTO=0. Environment:
 *   RX_ARGUS_CONSUMER  ingest (default) | discard | off
 *   RX_ARGUS_STREAM    path for the raw 128-byte record stream (ingest order)
 *   RX_ARGUS_SUMMARY   path for the JSON summary written at exit
 *   RX_ARGUS_SUITE     label in the summary
 *   RX_ARGUS_RUN_ID    run id for the machine id (default: pid-based)
 *   RX_ARGUS_CONSUMER_CPU  consumer placement: auto (default: the CPUs outside the
 *                      process mask, if any) | none | a CPU list such as "3" or "0-2,4" */
__attribute__((constructor)) static void rx_argus_auto_start(void) {
    const char *a = getenv("RX_ARGUS_AUTO");
    if (a && strcmp(a, "0") == 0) return;
    const char *c = getenv("RX_ARGUS_CONSUMER");
    int mode = RX_ARGUS_CONSUMER_INGEST;
    if (c && strcmp(c, "discard") == 0) mode = RX_ARGUS_CONSUMER_DISCARD;
    else if (c && strcmp(c, "off") == 0) mode = RX_ARGUS_CONSUMER_OFF;
    const char *run = getenv("RX_ARGUS_RUN_ID");
    char fallback[64];
    if (!run) {
        snprintf(fallback, sizeof fallback, "pid-%ld", (long)getpid());
        run = fallback;
    }
    if (rx_argus_start(run, mode, getenv("RX_ARGUS_STREAM")) != ARGUS_OK)
        fprintf(stderr, "rx_argus: start failed\n");
}

__attribute__((destructor)) static void rx_argus_auto_stop(void) {
    if (!g.active || g.pid != getpid()) return;
    rx_argus_shutdown();
    const char *p = getenv("RX_ARGUS_SUMMARY");
    if (p) rx_argus_write_summary(p, getenv("RX_ARGUS_SUITE"));
}

/* ---- authority-sourced GRANTED/REVOKED (RX_ARGUS_AUTHORITY_OBSERVER) -----------------
 * The authority announces its own transitions (aienos_cap_set_observer,
 * aienos feat/capability-observer 12add16, backported onto the pinned
 * c8ab65e authority by tools/argus/aienos-cap-observer-c8ab65e.patch).
 * Omega's src/runtime/aienos_cap.h mirrors c8ab65e and lacks the three
 * observer declarations, so they are declared here.
 *
 * The observer is installed on every authority the process starts: a link
 * wrap of aienos_cap_start (the admin handle is only known there). The wrap
 * also emits GRANTED for the office (cap 0), which is created at start and
 * never minted. Mapping:
 *   MINT ok       -> GRANTED            MINT refused -> DENIED (cap ARGUS_CAP_NONE)
 *   REVOKE ok     -> REVOKED (each descendant too)
 *   EPOCH ok      -> REVOKED for every cap this authority announced live
 *                    (an epoch bump kills every earlier reference, office included)
 *   RESTART ok    -> REVOKED for every live cap, then GRANTED for the new office
 *   RECLAIM/CLOCK/KILL, refused admin ops -> counted only (no v1.1 kind). */
#if defined(RX_ARGUS_AUTHORITY_OBSERVER)
#define AIENOS_CAP_OBS_MINT 1u
#define AIENOS_CAP_OBS_REVOKE 2u
#define AIENOS_CAP_OBS_RECLAIM 3u
#define AIENOS_CAP_OBS_EPOCH 4u
#define AIENOS_CAP_OBS_CLOCK 5u
#define AIENOS_CAP_OBS_KILL 6u
#define AIENOS_CAP_OBS_RESTART 7u
typedef void (*AienosCapObserver)(void *ctx, uint32_t op, const AienosCapEntry *entry, int result);
int aienos_cap_set_observer(AienosCapAdmin *admin, AienosCapObserver fn, void *ctx);

int __real_aienos_cap_start(AienosCapAdmin **admin, AienosCapView **view);
int __wrap_aienos_cap_start(AienosCapAdmin **admin, AienosCapView **view);

static void kill_all_live(uint32_t a, const AienosCapView *view) {
    for (uint32_t c = 0; c < RX_ARGUS_USE_SLOTS; c++) {
        if (!g.auth_live[a][c]) continue;
        rx_argus_emit_cap_revoked(view, g.auth_subject[a][c], c, g.auth_live[a][c], 0);
        g.auth_live[a][c] = 0;
    }
}

static void authority_observer(void *ctx, uint32_t op, const AienosCapEntry *e, int rc) {
    uint32_t a = (uint32_t)(uintptr_t)ctx;
    if (op < 9) atomic_fetch_add(&g.obs_calls[op], 1);
    if (!g.active || a >= AUTH_MAX) return;
    const AienosCapView *view = g.auth_view[a];
    pthread_mutex_lock(&g.obs_mu);
    switch (op) {
    case AIENOS_CAP_OBS_MINT:
        if (e && rc == 0) {
            rx_argus_emit_cap_granted(view, e->subject, e->cap_id, (uint64_t)e->generation,
                                      e->resource, e->rights, 0);
            if (e->cap_id < RX_ARGUS_USE_SLOTS) {
                g.auth_live[a][e->cap_id] = (uint64_t)e->generation;
                g.auth_subject[a][e->cap_id] = e->subject;
            }
        } else {
            rx_argus_emit_cap_denied(view, 0, ARGUS_CAP_NONE, 0, 0, 0, rc);
        }
        break;
    case AIENOS_CAP_OBS_REVOKE:
        if (e && rc == 0) {
            rx_argus_emit_cap_revoked(view, e->subject, e->cap_id, (uint64_t)e->generation, 0);
            if (e->cap_id < RX_ARGUS_USE_SLOTS) g.auth_live[a][e->cap_id] = 0;
        }
        break;
    case AIENOS_CAP_OBS_EPOCH:
        if (e && rc == 0) kill_all_live(a, view);
        break;
    case AIENOS_CAP_OBS_RESTART:
        if (e && rc == 0) {
            kill_all_live(a, view);
            rx_argus_emit_cap_granted(view, e->subject, e->cap_id, (uint64_t)e->generation,
                                      e->resource, e->rights, 0);
            if (e->cap_id < RX_ARGUS_USE_SLOTS) {
                g.auth_live[a][e->cap_id] = (uint64_t)e->generation;
                g.auth_subject[a][e->cap_id] = e->subject;
            }
        }
        break;
    default:
        break;
    }
    pthread_mutex_unlock(&g.obs_mu);
}

int __wrap_aienos_cap_start(AienosCapAdmin **admin, AienosCapView **view) {
    int rc = __real_aienos_cap_start(admin, view);
    if (rc != 0 || !g.active || !admin || !*admin || !view || !*view) return rc;
    pthread_mutex_lock(&g.obs_mu);
    uint32_t a = g.n_auth < AUTH_MAX ? g.n_auth++ : AUTH_MAX;
    if (a < AUTH_MAX) {
        g.auth_view[a] = *view;
        memset(g.auth_live[a], 0, sizeof g.auth_live[a]);
    }
    pthread_mutex_unlock(&g.obs_mu);
    if (a >= AUTH_MAX) return rc;
    aienos_cap_set_observer(*admin, authority_observer, (void *)(uintptr_t)a);
    AienosCapRef office;
    AienosCapEntry oe;
    if (aienos_cap_office(*admin, &office) == 0 && aienos_cap_inspect(*view, office, &oe) == 0) {
        pthread_mutex_lock(&g.obs_mu);
        rx_argus_emit_cap_granted(*view, oe.subject, office.cap_id, (uint64_t)office.generation,
                                  oe.resource, oe.rights, 0);
        if (office.cap_id < RX_ARGUS_USE_SLOTS) {
            g.auth_live[a][office.cap_id] = (uint64_t)office.generation;
            g.auth_subject[a][office.cap_id] = oe.subject;
        }
        pthread_mutex_unlock(&g.obs_mu);
    }
    return rc;
}
#endif /* RX_ARGUS_AUTHORITY_OBSERVER */

#endif /* RX_ARGUS */
