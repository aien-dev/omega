/*
 * rx_argus.h -- the live ARGUS producer inside the reaction runtime (ABI v1.1).
 *
 * ARGUS (aien-dev/aienos native/argus, ABI argus_abi.h) observes authority
 * decisions; it never authorizes. The runtime copies each decision the
 * authority or AEGIS already made into ARGUS events. Nothing here
 * re-validates a capability.
 *
 * Compile-time mode, RX_ARGUS:
 *   0  (default) no code at all: every hook expands to nothing and this
 *      header does not include argus_abi.h.
 *   1  emit only: events are built and pushed, no consumer thread.
 *   2  emit + consumer thread (ingest into an argus_core, or discard).
 *
 * v1.1 hot-path rules ("emit on transition, count on use"):
 *   - Each producer thread owns one producer slot (up to RX_ARGUS_PRODUCERS):
 *     its own SPSC ring (RX_ARGUS_RING_CAPACITY), its own stream id (flag
 *     bits 2-15) and its own per-stream sequence. No push lock.
 *   - A successful validate updates the thread's 256-slot use table
 *     {count, min gen, max gen, principal} indexed by cap_id: a few stores,
 *     no lock, no clock, no ring access. The table is flushed as
 *     CAPABILITY_USE_SUMMARY events every ARGUS_USE_FLUSH_OPS uses, on the
 *     consumer's periodic flush request (next use after it), when the worker
 *     goes idle, at thread exit and at shutdown. A refused flush keeps the
 *     counts (the next summary carries the total).
 *   - Full events only for GRANTED/DENIED/REVOKED/WORLD_COMMITTED/JOINED and
 *     failed validates. The authority tick is read only for the transitions;
 *     a failed validate is a full USED/DENIED keyed like a use (tick 0, no
 *     stamp), so denials never force other threads to flush.
 *   - A thread flushes its whole table before any transition it emits
 *     (398cfb9: flush before GRANTED/REVOKED); for transitions announced on
 *     another thread the consumer's ordered merge gives the same order.
 *
 * Cross-ring order. One ring per thread loses the global push order that the
 * old push lock gave. The consumer restores a causal order with one shared
 * counter (the "stamp") that only transitions advance:
 *   - every transition takes a stamp s (atomic fetch_add) and gets order key
 *     2s+1; the GRANTED stamp is taken before the new reference can reach
 *     anyone, the REVOKED stamp after the revoke took effect;
 *   - a use reads the stamp e BEFORE the authority validate (rx_argus_use_begin)
 *     and is keyed 2e: "after every transition stamped < e, before every
 *     transition stamped >= e"; a thread flushes its table whenever e changes,
 *     so one summary covers one stamp interval;
 *   - each thread publishes a lower bound of the keys it may still push; the
 *     consumer merges the rings by key and only ingests an event when no
 *     producer can still push a smaller key. A thread publishes "idle" when it
 *     parks (rx_argus_idle), after a transition and at exit.
 * The order key travels in a side array next to each ring, not in the event.
 */
#ifndef RX_ARGUS_H
#define RX_ARGUS_H

#ifndef RX_ARGUS
#define RX_ARGUS 0
#endif

#if RX_ARGUS

#include "aienos_cap.h"
#include "argus_abi.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define RX_ARGUS_PRODUCERS     32u
#define RX_ARGUS_RING_CAPACITY 1024u
#define RX_ARGUS_KEYS          (2u * RX_ARGUS_RING_CAPACITY)
#define RX_ARGUS_USE_SLOTS     256u     /* == ARGUS_CAP_MAX: indexed by cap_id */
#define RX_ARGUS_FINDINGS_MAX  1024u
#define RX_ARGUS_FLUSH_NS      10000000ull   /* consumer flush request period (10 ms) */
#define RX_ARGUS_STALL_NS      2000000ull    /* consumer stops waiting for a silent producer */
#define RX_ARGUS_IDLE          UINT64_MAX

enum { RX_ARGUS_CONSUMER_OFF = 0, RX_ARGUS_CONSUMER_DISCARD = 1, RX_ARGUS_CONSUMER_INGEST = 2 };

typedef struct {
    uint64_t count;
    uint64_t min_gen, max_gen;
    uint32_t principal;
    uint32_t pad;
} RxArgusUse;

typedef struct RxArgusProducer {
    /* owner-only hot line(s) */
    uint64_t epoch;          /* stamp that keys the pending uses; 0 = idle (next use re-publishes) */
    uint64_t ops, flush_at;  /* uses since start; next flush threshold */
    RxArgusUse use[RX_ARGUS_USE_SLOTS];
    /* consumer -> producer: periodic flush request (own line) */
    _Alignas(128) _Atomic uint32_t flush_req;
    /* producer -> consumer: lower bound of the stamp of anything still to come */
    _Alignas(128) _Atomic uint64_t pub;
} RxArgusProducer;

extern _Thread_local RxArgusProducer *rx_argus_tls;
extern _Atomic uint64_t rx_argus_stamp;

uint64_t rx_argus_use_begin_slow(uint64_t e);
void rx_argus_use_slow(uint64_t key, const AienosCapView *view, uint32_t subject, uint32_t cap_id,
                       uint64_t cap_generation, uint64_t resource, int code);
void rx_argus_flush_self(void);

/* Before the authority validate: returns the use's order key. */
static inline uint64_t rx_argus_use_begin(void) {
    RxArgusProducer *t = rx_argus_tls;
    uint64_t e = atomic_load_explicit(&rx_argus_stamp, memory_order_acquire);
    if (__builtin_expect(!t || t->epoch != e, 0)) return rx_argus_use_begin_slow(e);
    return e;
}

/* After the authority validate: count a success, emit a failure. */
static inline void rx_argus_use_end(uint64_t key, const AienosCapView *view, uint32_t subject,
                                    uint32_t cap_id, uint64_t gen, uint64_t resource, int code) {
    RxArgusProducer *t = rx_argus_tls;
    if (__builtin_expect(code != 0 || cap_id >= RX_ARGUS_USE_SLOTS || !t || key != t->epoch, 0)) {
        rx_argus_use_slow(key, view, subject, cap_id, gen, resource, code);
        return;
    }
    RxArgusUse *u = &t->use[cap_id];
    if (__builtin_expect(u->principal != subject && u->count != 0, 0)) {
        rx_argus_use_slow(key, view, subject, cap_id, gen, resource, code);
        return;
    }
    u->principal = subject;
    u->count++;
    u->min_gen = gen < u->min_gen ? gen : u->min_gen;
    u->max_gen = gen > u->max_gen ? gen : u->max_gen;
    if (__builtin_expect(++t->ops >= t->flush_at ||
                         atomic_load_explicit(&t->flush_req, memory_order_relaxed), 0))
        rx_argus_flush_self();
}

typedef struct {
    /* producer side */
    uint64_t emitted, pushed, ring_refused, ring_malformed;
    uint64_t emitted_by_kind[ARGUS_EV_KIND_MAX + 1];
    uint64_t uses_counted;        /* successful validates folded into the use tables */
    uint64_t summaries_emitted;   /* summary pushes attempted */
    uint64_t summary_refused;     /* summary pushes refused (counts kept, retried) */
    uint64_t unregistered_uses;   /* uses by a thread that found no free producer slot */
    uint64_t order_slow;          /* use keyed off its thread's epoch (should be 0) */
    uint64_t mixed_keys;          /* refused leftovers carried across a stamp change */
    uint32_t producers_claimed, producers_max_live;
    /* consumer side */
    uint64_t received, synthesized, batches, stream_records, ingest_errors;
    uint64_t received_by_kind[ARGUS_EV_KIND_MAX + 1];
    uint64_t findings_total, findings_by_code[ARGUS_F_MAX + 1], findings_kept;
    uint64_t late_events;         /* ingested with a key below one already ingested */
    uint64_t stall_breaks;        /* consumer stopped waiting for a silent producer */
    uint64_t hold_ns_max;         /* longest time the head event waited for the merge */
    uint32_t lag_max;             /* deepest ring depth seen by the consumer */
    uint64_t authority_obs[9];    /* observer calls by AIENOS_CAP_OBS_* op */
    int consumer_mode;
    size_t ring_bytes, core_bytes, findings_bytes, producer_bytes;
    ArgusCoreHealth core;         /* zero unless INGEST */
} RxArgusStats;

int  rx_argus_active(void);
void rx_argus_shutdown(void);
void rx_argus_stats(RxArgusStats *out);
size_t rx_argus_findings(ArgusFinding *out, size_t max);
int  rx_argus_write_summary(const char *path, const char *suite);

/* Transitions. view may be NULL (tick 0). */
void rx_argus_emit_cap_granted(const AienosCapView *view, uint32_t subject, uint32_t cap_id,
                               uint64_t cap_generation, uint64_t resource, uint32_t rights,
                               int code);
/* A policy or root refusal; cap_id ARGUS_CAP_NONE when no reference exists. */
void rx_argus_emit_cap_denied(const AienosCapView *view, uint32_t subject, uint32_t cap_id,
                              uint64_t cap_generation, uint64_t resource, uint32_t rights,
                              int code);
void rx_argus_emit_cap_revoked(const AienosCapView *view, uint32_t subject, uint32_t cap_id,
                               uint64_t cap_generation, int code);
/* store_id: the RxGenStore identity (hash of its directory). */
void rx_argus_emit_world_committed(uint32_t store_id, uint32_t subject, uint32_t cap_id,
                                   uint64_t cap_generation, uint64_t world_generation,
                                   const uint8_t digest[32], int code);
/* The worker is about to park: flush, publish idle. */
void rx_argus_idle(void);
/* Stable u32 identity for a store directory (never 0). */
uint32_t rx_argus_store_id(const char *dir);

#define RX_ARGUS_EMIT(call) call
#define RX_ARGUS_USE_BEGIN() rx_argus_use_begin()
#define RX_ARGUS_USE_END(key, view, subj, cap, gen, res, code) \
    rx_argus_use_end((key), (view), (subj), (cap), (gen), (res), (code))
/* Mint/revoke emits at the AEGIS root. Compiled out when the authority itself
 * announces them (RX_ARGUS_AUTHORITY_OBSERVER) or the test-only link wrap does. */
#if defined(RX_ARGUS_AUTHORITY_WRAP) || defined(RX_ARGUS_AUTHORITY_OBSERVER)
#define RX_ARGUS_EMIT_AUTH(call) ((void)0)
#else
#define RX_ARGUS_EMIT_AUTH(call) call
#endif

#else /* RX_ARGUS == 0 */

#define RX_ARGUS_EMIT(call) ((void)0)
#define RX_ARGUS_EMIT_AUTH(call) ((void)0)
#define RX_ARGUS_USE_BEGIN() 0
#define RX_ARGUS_USE_END(key, view, subj, cap, gen, res, code) ((void)(key))

#endif
#endif /* RX_ARGUS_H */
