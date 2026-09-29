/*
 * rx_argus.c -- ARGUS producer + optional consumer for the reaction runtime.
 * See rx_argus.h. Compiled only when RX_ARGUS >= 1.
 */
#include "rx_argus.h"

#if RX_ARGUS

#include "sha256.h"

#include <pthread.h>
#include <stdatomic.h>
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

#define BATCH 256u

/* Weakest class per kind. argus_event_min_class is declared in argus_abi.h
 * (4cd73cc) but not yet defined on feat/argus-0 (lane B's table is pending);
 * this mirrors lane B's table for the five kinds this producer emits. Switch
 * to argus_event_min_class once it lands. */
static uint8_t kind_class(uint16_t kind) {
    switch (kind) {
    case ARGUS_EV_CAPABILITY_REVOKED:    return ARGUS_CLASS_CRITICAL;
    case ARGUS_EV_CAPABILITY_GRANTED:
    case ARGUS_EV_CAPABILITY_DENIED:
    case ARGUS_EV_MACHINE_JOINED:
    case ARGUS_EV_WORLD_COMMITTED:       return ARGUS_CLASS_SECURITY;
    case ARGUS_EV_CAPABILITY_USED:       return ARGUS_CLASS_AUDIT;
    default:                             return ARGUS_CLASS_CRITICAL;
    }
}

static struct {
    ArgusRing *ring;
    ArgusCore *core;
    ArgusFinding *findings;
    int active, mode;
    pid_t pid;
    uint8_t machine_id[ARGUS_MACHINE_ID_LEN];

    /* producer: guarded by lock. Own cache lines (no false sharing with the consumer). */
    _Alignas(128) atomic_flag lock;
    uint64_t next_seq;
    _Atomic uint64_t emitted, pushed, ring_refused, ring_malformed, lock_dropped;
    _Atomic uint64_t emitted_by_kind[ARGUS_EV_KIND_MAX + 1];
    _Atomic uint64_t dropped_by_kind[ARGUS_EV_KIND_MAX + 1];   /* lock drops (RMW, outside the lock) */
    _Atomic uint64_t lock_pending[ARGUS_CLASS_MAX + 1];

    /* consumer: written by the consumer thread only (or after join) */
    _Alignas(128) pthread_t thread;
    int thread_started;
    _Atomic int stop;
    uint64_t consumer_seq;
    uint64_t received, synthesized, batches, stream_records, ingest_errors;
    uint64_t received_by_kind[ARGUS_EV_KIND_MAX + 1];
    uint64_t findings_total, findings_by_code[ARGUS_F_MAX + 1];
    uint32_t lag_max;
    FILE *stream;
    size_t ring_bytes, core_bytes, findings_bytes;
} g = { .lock = ATOMIC_FLAG_INIT };

static inline void cpu_relax(void) {
#if defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__)
    __asm__ __volatile__("pause" ::: "memory");
#endif
}

static size_t align64(size_t n) { return (n + 63u) & ~(size_t)63u; }

size_t rx_argus_footprint(void) {
    size_t n = align64(argus_ring_footprint(RX_ARGUS_RING_CAPACITY));
#if RX_ARGUS_HAS_CONSUMER
    n += align64(argus_core_footprint());
    n += align64(sizeof(ArgusFinding) * RX_ARGUS_FINDINGS_MAX);
#endif
    return n;
}

/* ---- producer (hot path) ----------------------------------------------- */

/* Counters written only under the push lock use load+store (no RMW): the lock
 * already orders them, and readers only need a relaxed snapshot. */
#define BUMP_LOCKED(x) atomic_store_explicit(&(x), atomic_load_explicit(&(x), memory_order_relaxed) + 1, memory_order_relaxed)

static inline void push(ArgusEvent *ev) {
    unsigned spins = 0;
    while (atomic_flag_test_and_set_explicit(&g.lock, memory_order_acquire)) {
        if (++spins >= RX_ARGUS_SPIN) {
            atomic_fetch_add_explicit(&g.lock_dropped, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&g.dropped_by_kind[ev->kind], 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&g.lock_pending[ev->class_], 1, memory_order_relaxed);
            return;
        }
        cpu_relax();
    }
    ev->sequence = g.next_seq++;
    int rc = argus_ring_push(g.ring, ev);
    BUMP_LOCKED(g.emitted);
    BUMP_LOCKED(g.emitted_by_kind[ev->kind]);
    if (rc == ARGUS_OK) BUMP_LOCKED(g.pushed);
    else if (rc == ARGUS_ERR_FULL) BUMP_LOCKED(g.ring_refused);
    else BUMP_LOCKED(g.ring_malformed);
    atomic_flag_clear_explicit(&g.lock, memory_order_release);
}

static inline void fill(ArgusEvent *ev, uint16_t kind, int code, uint8_t outcome_ok_or_denied,
                        const AienosCapView *view) {
    memset(ev, 0, sizeof *ev);
    ev->version = ARGUS_ABI_VERSION;
    ev->kind = kind;
    ev->class_ = kind_class(kind);
    ev->effect_class = ARGUS_EFFECT_NONE;
    ev->outcome = outcome_ok_or_denied;
    ev->code = code;
    ev->tick = view ? aienos_cap_clock(view) : 0;
    memcpy(ev->machine_id, g.machine_id, ARGUS_MACHINE_ID_LEN);
}

static inline uint8_t outcome_of(int code) {
    return code == 0 ? ARGUS_OUTCOME_OK : ARGUS_OUTCOME_DENIED;
}

void rx_argus_emit_cap_granted(const AienosCapView *view, uint32_t subject, uint32_t cap_id,
                               uint64_t cap_generation, uint64_t resource, uint32_t rights,
                               int code) {
    if (!g.active) return;
    ArgusEvent ev;
    fill(&ev, ARGUS_EV_CAPABILITY_GRANTED, code,
         code == 0 ? ARGUS_OUTCOME_OK : ARGUS_OUTCOME_ERROR, view);
    ev.principal = subject;
    ev.cap_id = cap_id;
    ev.cap_generation = cap_generation;
    ev.object_id = rights;
    ev.resource = resource;
    push(&ev);
}

void rx_argus_emit_cap_denied(const AienosCapView *view, uint32_t subject, uint32_t cap_id,
                              uint64_t cap_generation, uint64_t resource, uint32_t rights,
                              int code) {
    if (!g.active) return;
    ArgusEvent ev;
    fill(&ev, ARGUS_EV_CAPABILITY_DENIED, code, ARGUS_OUTCOME_DENIED, view);
    ev.principal = subject;
    ev.cap_id = cap_id;
    ev.cap_generation = cap_generation;
    ev.object_id = rights;
    ev.resource = resource;
    push(&ev);
}

void rx_argus_emit_cap_revoked(const AienosCapView *view, uint32_t subject, uint32_t cap_id,
                               uint64_t cap_generation, int code) {
    if (!g.active) return;
    ArgusEvent ev;
    fill(&ev, ARGUS_EV_CAPABILITY_REVOKED, code,
         code == 0 ? ARGUS_OUTCOME_OK : ARGUS_OUTCOME_ERROR, view);
    ev.principal = subject;
    ev.cap_id = cap_id;
    ev.cap_generation = cap_generation;
    push(&ev);
}

void rx_argus_emit_cap_used(const AienosCapView *view, uint32_t subject, uint32_t cap_id,
                            uint64_t cap_generation, uint64_t resource, uint32_t rights,
                            int code) {
    if (!g.active) return;
    ArgusEvent ev;
    fill(&ev, ARGUS_EV_CAPABILITY_USED, code, outcome_of(code), view);
    ev.principal = subject;
    ev.cap_id = cap_id;
    ev.cap_generation = cap_generation;
    ev.resource = resource;
    (void)rights;   /* USED has no rights field in ABI v1; object_id stays 0 */
    push(&ev);
}

void rx_argus_emit_world_committed(uint32_t subject, uint32_t cap_id, uint64_t cap_generation,
                                   uint64_t world_generation, uint64_t candidate_id,
                                   const uint8_t digest[32], int code) {
    if (!g.active) return;
    ArgusEvent ev;
    fill(&ev, ARGUS_EV_WORLD_COMMITTED, code,
         code == 0 ? ARGUS_OUTCOME_OK : ARGUS_OUTCOME_ERROR, NULL);
    ev.principal = subject;
    ev.cap_id = cap_id;
    ev.cap_generation = cap_generation;
    ev.world_generation = world_generation;
    ev.resource = candidate_id;
    if (digest) memcpy(ev.evidence_digest, digest, ARGUS_DIGEST_LEN);
    push(&ev);
}

/* ---- consumer ------------------------------------------------------------ */

#if RX_ARGUS_HAS_CONSUMER
static void consume_one(const ArgusEvent *ev) {
    g.received++;
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

/* Contention drops never reached the ring: report them like ring refusals. */
static size_t drain_lock_drops(ArgusEvent *out, size_t max) {
    size_t n = 0;
    for (unsigned cls = ARGUS_CLASS_CRITICAL; cls <= ARGUS_CLASS_MAX && n < max; cls++) {
        uint64_t k = atomic_exchange_explicit(&g.lock_pending[cls], 0, memory_order_relaxed);
        if (!k) continue;
        ArgusEvent *e = &out[n++];
        memset(e, 0, sizeof *e);
        e->version = ARGUS_ABI_VERSION;
        e->class_ = cls == ARGUS_CLASS_CRITICAL ? ARGUS_CLASS_CRITICAL : ARGUS_CLASS_SECURITY;
        e->kind = ARGUS_EV_TELEMETRY_DROPPED;
        e->outcome = ARGUS_OUTCOME_ERROR;
        e->flags = ARGUS_FLAG_CONSUMER;
        if (g.consumer_seq == 0) g.consumer_seq = 1;
        e->sequence = g.consumer_seq++;
        e->code = ARGUS_ERR_FULL;
        e->object_id = cls;
        e->resource = k;
    }
    return n;
}

static size_t pump(ArgusEvent *batch) {
    ArgusRingStats st;
    argus_ring_stats(g.ring, &st);
    if (st.depth > g.lag_max) g.lag_max = st.depth;
    size_t n = argus_ring_pop_batch(g.ring, batch, BATCH);
    if (n) g.batches++;
    for (size_t i = 0; i < n; i++) consume_one(&batch[i]);
    ArgusEvent drops[2 * ARGUS_CLASS_MAX];
    size_t d = argus_ring_drain_drops(g.ring, drops, ARGUS_CLASS_MAX, &g.consumer_seq);
    d += drain_lock_drops(drops + d, ARGUS_CLASS_MAX);
    g.synthesized += d;
    for (size_t i = 0; i < d; i++) consume_one(&drops[i]);
    return n + d;
}

static void *consumer_main(void *arg) {
    (void)arg;
    static ArgusEvent batch[BATCH];
    unsigned idle = 0;
    while (!atomic_load_explicit(&g.stop, memory_order_acquire)) {
        if (pump(batch)) { idle = 0; continue; }
        if (++idle < 64) { cpu_relax(); continue; }
        struct timespec ts = { 0, 20000 };   /* 20 us back-off when idle */
        nanosleep(&ts, NULL);
    }
    while (pump(batch)) {}
    return NULL;
}
#endif

/* ---- lifecycle ------------------------------------------------------------ */

int rx_argus_init(void *memory, size_t bytes, const char *run_id, int consumer_mode,
                  const char *stream_path) {
    if (g.active || !memory || bytes < rx_argus_footprint() || ((uintptr_t)memory & 63u))
        return ARGUS_ERR_ARG;
    uint8_t *m = memory;
    g.ring_bytes = align64(argus_ring_footprint(RX_ARGUS_RING_CAPACITY));
    if (argus_ring_init(&g.ring, m, g.ring_bytes, RX_ARGUS_RING_CAPACITY) != ARGUS_OK)
        return ARGUS_ERR_ARG;
    m += g.ring_bytes;
#if RX_ARGUS_HAS_CONSUMER
    g.mode = consumer_mode;
    if (g.mode == RX_ARGUS_CONSUMER_INGEST) {
        g.core_bytes = align64(argus_core_footprint());
        if (argus_core_init(&g.core, m, g.core_bytes) != ARGUS_OK) return ARGUS_ERR_ARG;
    }
    m += align64(argus_core_footprint());
    g.findings = (ArgusFinding *)(void *)m;
    g.findings_bytes = sizeof(ArgusFinding) * RX_ARGUS_FINDINGS_MAX;
    if (stream_path && g.mode != RX_ARGUS_CONSUMER_OFF) {
        g.stream = fopen(stream_path, "wb");
        if (!g.stream) return ARGUS_ERR_ARG;
    }
#else
    (void)consumer_mode;
    (void)stream_path;
    g.mode = RX_ARGUS_CONSUMER_OFF;
#endif
    /* Provisional machine id: SHA-256("ARGUS-PROVISIONAL-MACHINE-v1" || run_id). */
    {
        static const char tag[] = "ARGUS-PROVISIONAL-MACHINE-v1";
        sha256_ctx h;
        sha256_init(&h);
        sha256_update(&h, (const uint8_t *)tag, sizeof tag - 1);
        if (run_id) sha256_update(&h, (const uint8_t *)run_id, strlen(run_id));
        sha256_final(&h, g.machine_id);
    }
    g.next_seq = 1;
    g.consumer_seq = 1;
    g.pid = getpid();
    g.active = 1;

    ArgusEvent ev;
    fill(&ev, ARGUS_EV_MACHINE_JOINED, 0, ARGUS_OUTCOME_OK, NULL);
    ev.object_id = ARGUS_TRUST_OBSERVED;
    push(&ev);

#if RX_ARGUS_HAS_CONSUMER
    if (g.mode != RX_ARGUS_CONSUMER_OFF) {
        atomic_store(&g.stop, 0);
        if (pthread_create(&g.thread, NULL, consumer_main, NULL) != 0) {
            g.active = 0;
            return ARGUS_ERR_STATE;
        }
        g.thread_started = 1;
    }
#endif
    return ARGUS_OK;
}

int rx_argus_active(void) { return g.active; }

void rx_argus_shutdown(void) {
    if (!g.active || g.pid != getpid()) return;
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
    o->emitted = atomic_load(&g.emitted) + atomic_load(&g.lock_dropped);
    o->pushed = atomic_load(&g.pushed);
    o->ring_refused = atomic_load(&g.ring_refused);
    o->ring_malformed = atomic_load(&g.ring_malformed);
    o->lock_dropped = atomic_load(&g.lock_dropped);
    for (unsigned k = 0; k <= ARGUS_EV_KIND_MAX; k++) {
        o->emitted_by_kind[k] = atomic_load(&g.emitted_by_kind[k]) + atomic_load(&g.dropped_by_kind[k]);
        o->received_by_kind[k] = g.received_by_kind[k];
    }
    o->received = g.received;
    o->synthesized = g.synthesized;
    o->findings_total = g.findings_total;
    for (unsigned c = 0; c <= ARGUS_F_MAX; c++) o->findings_by_code[c] = g.findings_by_code[c];
    o->findings_kept = g.findings_total < RX_ARGUS_FINDINGS_MAX ? g.findings_total
                                                                 : RX_ARGUS_FINDINGS_MAX;
    o->ingest_errors = g.ingest_errors;
    o->lag_max = g.lag_max;
    o->batches = g.batches;
    o->stream_records = g.stream_records;
    o->consumer_mode = g.mode;
    o->ring_bytes = g.ring_bytes;
    o->core_bytes = g.core_bytes;
    o->findings_bytes = g.findings_bytes;
    if (g.ring) argus_ring_stats(g.ring, &o->ring);
#if RX_ARGUS_HAS_CONSUMER
    if (g.core) argus_core_health(g.core, &o->core);
#endif
}

size_t rx_argus_findings(ArgusFinding *out, size_t max) {
    size_t n = g.findings_total < RX_ARGUS_FINDINGS_MAX ? g.findings_total
                                                        : RX_ARGUS_FINDINGS_MAX;
    if (n > max) n = max;
    if (n && g.findings) memcpy(out, g.findings, n * sizeof *out);
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
    default: return NULL;
    }
}

int rx_argus_write_summary(const char *path, const char *suite) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    RxArgusStats s;
    rx_argus_stats(&s);
    fprintf(f, "{\n  \"suite\": \"%s\",\n  \"rx_argus\": %d,\n  \"consumer_mode\": %d,\n",
            suite ? suite : "", RX_ARGUS, s.consumer_mode);
    fprintf(f, "  \"machine_id\": \"");
    for (unsigned i = 0; i < ARGUS_MACHINE_ID_LEN; i++) fprintf(f, "%02x", g.machine_id[i]);
    fprintf(f, "\",\n  \"emitted\": %llu,\n  \"pushed\": %llu,\n  \"ring_refused\": %llu,\n"
               "  \"ring_malformed\": %llu,\n  \"lock_dropped\": %llu,\n"
               "  \"critical_overflow\": %llu,\n",
            (unsigned long long)s.emitted, (unsigned long long)s.pushed,
            (unsigned long long)s.ring_refused, (unsigned long long)s.ring_malformed,
            (unsigned long long)s.lock_dropped, (unsigned long long)s.ring.critical_overflow);
    fprintf(f, "  \"received\": %llu,\n  \"synthesized\": %llu,\n  \"stream_records\": %llu,\n"
               "  \"lag_max\": %u,\n  \"batches\": %llu,\n  \"ingest_errors\": %llu,\n",
            (unsigned long long)s.received, (unsigned long long)s.synthesized,
            (unsigned long long)s.stream_records, s.lag_max, (unsigned long long)s.batches,
            (unsigned long long)s.ingest_errors);
    fprintf(f, "  \"received_by_kind\": {");
    int first = 1;
    for (unsigned k = 0; k <= ARGUS_EV_KIND_MAX; k++) {
        if (!s.received_by_kind[k] && !s.emitted_by_kind[k]) continue;
        const char *nm = kind_name(k);
        fprintf(f, "%s\n    \"%s%s%u\": { \"emitted\": %llu, \"received\": %llu }",
                first ? "" : ",", nm ? nm : "", nm ? "_" : "kind_", k,
                (unsigned long long)s.emitted_by_kind[k], (unsigned long long)s.received_by_kind[k]);
        first = 0;
    }
    fprintf(f, "\n  },\n  \"core\": { \"events_received\": %llu, \"events_rejected\": %llu, "
               "\"findings_emitted\": %llu, \"incidents_open\": %llu, \"events_not_applied\": %llu, "
               "\"tables_full\": %llu, \"producers_untracked\": %llu },\n",
            (unsigned long long)s.core.events_received, (unsigned long long)s.core.events_rejected,
            (unsigned long long)s.core.findings_emitted, (unsigned long long)s.core.incidents_open,
            (unsigned long long)s.core.events_not_applied, (unsigned long long)s.core.tables_full,
            (unsigned long long)s.core.producers_untracked);
    fprintf(f, "  \"memory_bytes\": { \"ring\": %zu, \"core\": %zu, \"findings\": %zu },\n",
            s.ring_bytes, s.core_bytes, s.findings_bytes);
    fprintf(f, "  \"findings_total\": %llu,\n  \"findings_by_code\": {",
            (unsigned long long)s.findings_total);
    first = 1;
    for (unsigned c = 0; c <= ARGUS_F_MAX; c++) {
        if (!s.findings_by_code[c]) continue;
        fprintf(f, "%s \"%u\": %llu", first ? "" : ",", c, (unsigned long long)s.findings_by_code[c]);
        first = 0;
    }
    fprintf(f, " },\n  \"findings\": [");
    for (uint64_t i = 0; i < s.findings_kept; i++) {
        const ArgusFinding *x = &g.findings[i];
        fprintf(f, "%s\n    { \"code\": %u, \"severity\": %u, \"sequence\": %llu, "
                   "\"prior_sequence\": %llu, \"principal\": %u, \"cap_id\": %u, "
                   "\"cap_generation\": %llu }",
                i ? "," : "", x->code, x->severity, (unsigned long long)x->sequence,
                (unsigned long long)x->prior_sequence, x->principal, x->cap_id,
                (unsigned long long)x->cap_generation);
    }
    fprintf(f, "\n  ]\n}\n");
    fclose(f);
    return 0;
}

/* ---- automatic process-wide instance -------------------------------------
 * Existing suites are linked unchanged: a constructor starts ARGUS from
 * static memory unless RX_ARGUS_AUTO=0. Environment:
 *   RX_ARGUS_CONSUMER  ingest (default) | discard | off
 *   RX_ARGUS_STREAM    path for the raw 128-byte record stream
 *   RX_ARGUS_SUMMARY   path for the JSON summary written at exit
 *   RX_ARGUS_SUITE     label in the summary
 *   RX_ARGUS_RUN_ID    run id for the machine id (default: pid-based) */
static _Alignas(64) uint8_t g_auto_mem[(4096u + 16u) * 128u + (1u << 20)];

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
    if (rx_argus_footprint() > sizeof g_auto_mem) {
        fprintf(stderr, "rx_argus: static memory too small (%zu)\n", rx_argus_footprint());
        return;
    }
    if (rx_argus_init(g_auto_mem, sizeof g_auto_mem, run, mode, getenv("RX_ARGUS_STREAM")) != ARGUS_OK)
        fprintf(stderr, "rx_argus: init failed (footprint %zu)\n", rx_argus_footprint());
}

__attribute__((destructor)) static void rx_argus_auto_stop(void) {
    if (!g.active || g.pid != getpid()) return;
    rx_argus_shutdown();
    const char *p = getenv("RX_ARGUS_SUMMARY");
    if (p) rx_argus_write_summary(p, getenv("RX_ARGUS_SUITE"));
}

#endif /* RX_ARGUS */

/* ---- optional authority-boundary coverage (RX_ARGUS_AUTHORITY_WRAP) -------
 * Test-only link option: ld --wrap=aienos_cap_mint --wrap=aienos_cap_revoke.
 * Every mint/revoke in the process (including test harnesses that call the
 * authority directly, not through rx_aegis) then emits GRANTED/REVOKED, and
 * the rx_aegis emits for those two kinds are compiled out (no duplicates).
 * It separates "ARGUS never saw the grant" from genuine false positives. */
#if RX_ARGUS && defined(RX_ARGUS_AUTHORITY_WRAP)
int __real_aienos_cap_mint(AienosCapAdmin *admin, const AienosCapMint *request, AienosCapRef *out);
int __real_aienos_cap_revoke(AienosCapAdmin *admin, AienosCapRef authority, AienosCapRef target);
int __wrap_aienos_cap_mint(AienosCapAdmin *admin, const AienosCapMint *request, AienosCapRef *out);
int __wrap_aienos_cap_revoke(AienosCapAdmin *admin, AienosCapRef authority, AienosCapRef target);

int __wrap_aienos_cap_mint(AienosCapAdmin *admin, const AienosCapMint *request, AienosCapRef *out) {
    int rc = __real_aienos_cap_mint(admin, request, out);
    if (rc == 0 && request && out)
        rx_argus_emit_cap_granted(NULL, request->subject, out->cap_id, (uint64_t)out->generation,
                                  request->resource, request->rights, 0);
    return rc;
}

int __wrap_aienos_cap_revoke(AienosCapAdmin *admin, AienosCapRef authority, AienosCapRef target) {
    int rc = __real_aienos_cap_revoke(admin, authority, target);
    if (rc == 0) rx_argus_emit_cap_revoked(NULL, 0, target.cap_id, (uint64_t)target.generation, rc);
    return rc;
}
#endif
