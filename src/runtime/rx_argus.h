/*
 * rx_argus.h -- the first live ARGUS producer inside the reaction runtime.
 *
 * ARGUS (aien-dev/aienos native/argus, ABI argus_abi.h) observes authority
 * decisions; it never authorizes. The runtime copies each decision the
 * authority or AEGIS already made into one fixed 128-byte ArgusEvent and
 * pushes it onto one bounded ring. Nothing here re-validates a capability.
 *
 * Compile-time mode, RX_ARGUS:
 *   0  (default) no code at all: every RX_ARGUS_EMIT(...) expands to nothing
 *      and this header does not include argus_abi.h.
 *   1  emit only: events are filled and pushed, no consumer thread. The ring
 *      fills to capacity and every later push is refused and counted.
 *   2  emit + consumer thread: batches are popped and, in INGEST mode, fed to
 *      an argus_core (detectors + shadow state); findings are kept in a
 *      bounded buffer. DISCARD mode pops and drops (perf baseline for the
 *      consumer's presence without the core).
 *
 * Hot path rules: an emit never allocates, never logs, never makes a system
 * call of its own and never waits unboundedly. The ring is single-producer
 * (argus_ring.c), the runtime has many worker threads, so pushes are
 * serialized by a bounded try-lock: after RX_ARGUS_SPIN failed attempts the
 * event is dropped, counted, and reported to the core as TELEMETRY_DROPPED.
 *
 * Sequence: ONE process-wide counter, assigned under the push lock, so ring
 * order == sequence order. (The core keys sequence streams by machine id, so
 * a per-thread counter under one machine id would be a SEQUENCE_ANOMALY.)
 *
 * Generation width: the pinned authority (aienos.lock c8ab65e) and RxCapRef
 * carry a 32-bit generation; ArgusEvent carries 64 bits. The helpers take
 * uint64_t and callers widen explicitly.
 */
#ifndef RX_ARGUS_H
#define RX_ARGUS_H

#ifndef RX_ARGUS
#define RX_ARGUS 0
#endif

#if RX_ARGUS

#include "aienos_cap.h"
#include "argus_abi.h"

#include <stddef.h>
#include <stdint.h>

#define RX_ARGUS_RING_CAPACITY 4096u
#define RX_ARGUS_FINDINGS_MAX  1024u
#define RX_ARGUS_SPIN          256u

enum { RX_ARGUS_CONSUMER_OFF = 0, RX_ARGUS_CONSUMER_DISCARD = 1, RX_ARGUS_CONSUMER_INGEST = 2 };

typedef struct {
    /* producer side */
    uint64_t emitted;                          /* emit calls */
    uint64_t pushed;                           /* accepted by the ring */
    uint64_t ring_refused;                     /* argus_ring_push returned FULL */
    uint64_t ring_malformed;                   /* argus_ring_push returned MALFORMED/ARG */
    uint64_t lock_dropped;                     /* try-lock gave up (contention) */
    uint64_t emitted_by_kind[ARGUS_EV_KIND_MAX + 1];
    /* consumer side */
    uint64_t received;                         /* events popped from the ring */
    uint64_t synthesized;                      /* TELEMETRY_DROPPED made by the consumer */
    uint64_t received_by_kind[ARGUS_EV_KIND_MAX + 1];
    uint64_t findings_total;
    uint64_t findings_by_code[ARGUS_F_MAX + 1];
    uint64_t findings_kept;                    /* min(total, RX_ARGUS_FINDINGS_MAX) */
    uint64_t ingest_errors;                    /* argus_core_ingest != OK */
    uint32_t lag_max;                          /* deepest ring depth seen by the consumer */
    uint64_t batches;
    uint64_t stream_records;                   /* 128-byte records written to the stream file */
    int consumer_mode;
    size_t ring_bytes, core_bytes, findings_bytes;
    ArgusRingStats ring;
    ArgusCoreHealth core;                      /* zero unless INGEST */
} RxArgusStats;

/* Bytes of memory rx_argus_init needs (64-byte aligned): ring + core + findings. */
size_t rx_argus_footprint(void);

/* Initialize from caller memory. run_id feeds the provisional machine id
 * (SHA-256 of "ARGUS-PROVISIONAL-MACHINE-v1" || run_id). consumer_mode is
 * ignored (forced OFF) when RX_ARGUS == 1. stream_path, if not NULL, gets
 * every event the consumer sees as encoded 128-byte records. Pushes
 * MACHINE_JOINED (sequence 1) so later events come from a joined machine. */
int rx_argus_init(void *memory, size_t bytes, const char *run_id, int consumer_mode,
                  const char *stream_path);
/* Stop the consumer, drain what is left, close the stream. Idempotent. */
void rx_argus_shutdown(void);
int  rx_argus_active(void);
void rx_argus_stats(RxArgusStats *out);
size_t rx_argus_findings(ArgusFinding *out, size_t max);
/* Write a JSON summary (stats + kept findings) to path. */
int  rx_argus_write_summary(const char *path, const char *suite);

/* Producer helpers. view may be NULL (tick 0). code is the authority's (or
 * AEGIS's) result code; outcome is derived from it. */
void rx_argus_emit_cap_granted(const AienosCapView *view, uint32_t subject, uint32_t cap_id,
                               uint64_t cap_generation, uint64_t resource, uint32_t rights,
                               int code);
/* A policy or root refusal: no reference exists (cap_id 0) or it was refused. */
void rx_argus_emit_cap_denied(const AienosCapView *view, uint32_t subject, uint32_t cap_id,
                              uint64_t cap_generation, uint64_t resource, uint32_t rights,
                              int code);
void rx_argus_emit_cap_revoked(const AienosCapView *view, uint32_t subject, uint32_t cap_id,
                               uint64_t cap_generation, int code);
/* A validate of a presented reference: code 0 -> USED/OK, else USED/DENIED. */
void rx_argus_emit_cap_used(const AienosCapView *view, uint32_t subject, uint32_t cap_id,
                            uint64_t cap_generation, uint64_t resource, uint32_t rights,
                            int code);
void rx_argus_emit_world_committed(uint32_t subject, uint32_t cap_id, uint64_t cap_generation,
                                   uint64_t world_generation, uint64_t candidate_id,
                                   const uint8_t digest[32], int code);

#define RX_ARGUS_EMIT(call) call
/* Mint/revoke emits at the AEGIS root. Compiled out when the test-only
 * authority wrap (RX_ARGUS_AUTHORITY_WRAP, rx_argus.c) emits them instead. */
#ifdef RX_ARGUS_AUTHORITY_WRAP
#define RX_ARGUS_EMIT_AUTH(call) ((void)0)
#else
#define RX_ARGUS_EMIT_AUTH(call) call
#endif

#else /* RX_ARGUS == 0 */

#define RX_ARGUS_EMIT(call) ((void)0)
#define RX_ARGUS_EMIT_AUTH(call) ((void)0)

#endif
#endif /* RX_ARGUS_H */
