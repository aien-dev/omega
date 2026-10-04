/* DUAL-1b: deterministic replay of the reference controller over digest-bound
 * traces (ADR 0031 section 5 and section 8, gate DUAL_PRICE_REFERENCE).
 *
 * A trace is a bounded, canonically encoded sequence of controller inputs
 * (kind byte 8, domain omega.dual.trace.v1). Its digest binds the inputs a
 * replay consumed. The replay engine feeds the trace through
 * rx_dual_init_state / rx_dual_update and records the digest of every
 * resulting RxDualConstraintState; the digest of that log, bound to the
 * controller_id and the trace digest, is the replay digest. Same trace, same
 * controller, same resource and budget => identical replay digest.
 *
 * Bounded, no allocation, no globals, no I/O: the recorded-trace importer
 * takes bytes the caller already read. Every price produced by a replay is as
 * calibrated as its inputs: with calibrated = 0 on every tick (the only honest
 * value while EST-3 has not passed) every state is UNCALIBRATED and may never
 * influence production. Replay is measurement of the controller, nothing else. */
#ifndef OMEGA_RX_DUAL_REPLAY_H
#define OMEGA_RX_DUAL_REPLAY_H

#include "rx_dual_update.h"

#define RX_DUAL_KIND_TRACE 8
#define RX_DUAL_DOMAIN_TRACE  "omega.dual.trace.v1"
#define RX_DUAL_DOMAIN_REPLAY "omega.dual.replay.v1"
#define RX_DUAL_TRACE_MAX_TICKS 2048u
#define RX_DUAL_TRACE_TICK_ENCODED 36u                                   /* u64 f64 f64 u32 u32 u32 */
#define RX_DUAL_TRACE_HEADER_ENCODED 18u                                 /* kind ver u32 u64 u32 */
#define RX_DUAL_TRACE_ENCODED_MAX (RX_DUAL_TRACE_HEADER_ENCODED + RX_DUAL_TRACE_MAX_TICKS * RX_DUAL_TRACE_TICK_ENCODED)

typedef enum {
    RX_DUAL_TRACE_CONSTANT_LOW = 1,
    RX_DUAL_TRACE_BINDING = 2,
    RX_DUAL_TRACE_STEP = 3,
    RX_DUAL_TRACE_SQUARE = 4,
    RX_DUAL_TRACE_COMPETING_MEMORY = 5,
    RX_DUAL_TRACE_COMPETING_LATENCY = 6,
    RX_DUAL_TRACE_CAPACITY_REDUCTION = 7,
    RX_DUAL_TRACE_REGIME_CHANGE = 8,
    RX_DUAL_TRACE_BURSTY = 9,
    RX_DUAL_TRACE_FANOUT = 10,
    RX_DUAL_TRACE_KV_SATURATION = 11,
    RX_DUAL_TRACE_RECOVERY = 12,
    RX_DUAL_TRACE_RECORDED = 13,
    RX_DUAL_TRACE_MAX_ = 14
} RxDualTraceKind;

typedef struct {
    uint64_t generation;        /* World generation the input claims; also used as now_generation */
    double estimate;            /* finite */
    double uncertainty;         /* finite, >= 0 */
    uint32_t calibrated;        /* 0 or 1; 0 = no EST-3 receipt: the state can only be UNCALIBRATED */
    uint32_t evidence_verified; /* 0 or 1 */
    uint32_t regime_change;     /* 0 or 1 */
} RxDualTraceTick;

typedef struct {
    uint32_t trace_kind;        /* RxDualTraceKind */
    uint64_t seed;              /* generator seed (0 for recorded) */
    uint32_t n_ticks;           /* 1..RX_DUAL_TRACE_MAX_TICKS */
    RxDualTraceTick t[RX_DUAL_TRACE_MAX_TICKS];
} RxDualTrace;

/* xorshift64*: the one generator every synthetic trace uses. State must be nonzero. */
uint64_t rx_dual_xorshift64s(uint64_t *state);
/* Uniform double in [0, 1) from one generator step (53 bits). */
double rx_dual_xorshift_unit(uint64_t *state);

RxDualStatus rx_dual_check_trace(const RxDualTrace *tr);
RxDualStatus rx_dual_encode_trace(const RxDualTrace *tr, uint8_t *buf, size_t cap, size_t *len);
RxDualStatus rx_dual_decode_trace(const uint8_t *buf, size_t len, RxDualTrace *out);
/* SHA-256(domain || 0x00 || encoding), streamed; no buffer needed. */
RxDualStatus rx_dual_digest_trace(const RxDualTrace *tr, RxDualDigest *out);

/* Import one (cpu, event) series from perf-stat CSV bytes:
 *   <time>,<cpu>,<count>,<unit>,<event>,<run ns>,<pct>,,
 * Lines starting with '#' and empty lines are skipped. A row whose cpu and
 * event fields equal `cpu` and `event` yields one tick with estimate = count
 * (parsed as a decimal unsigned integer, no locale, no strtod), uncertainty 0,
 * calibrated 0, evidence_verified 1, regime_change 0, generation = index + 1.
 * A matching row whose count is not a plain decimal integer (for example
 * "<not counted>") is refused (RX_DUAL_ERR_ENCODING), never skipped. Zero
 * matching rows or more than RX_DUAL_TRACE_MAX_TICKS is refused. */
RxDualStatus rx_dual_trace_from_perf_csv(const uint8_t *bytes, size_t len, const char *cpu, const char *event,
                                         RxDualTrace *out);

/* Replay of one constraint chain. */
typedef struct {
    RxDualDigest trace;                       /* rx_dual_digest_trace of the input */
    RxDualDigest controller_id;
    uint32_t n;                               /* ticks replayed (= trace n_ticks) */
    double lambda[RX_DUAL_TRACE_MAX_TICKS];
    double estimate[RX_DUAL_TRACE_MAX_TICKS];      /* estimate held in the state (held under STALE/FROZEN/REFUSED) */
    uint64_t generation[RX_DUAL_TRACE_MAX_TICKS];  /* likewise */
    RxDualLambdaState state[RX_DUAL_TRACE_MAX_TICKS];
    RxDualDigest record[RX_DUAL_TRACE_MAX_TICKS];  /* constraint state digest per tick */
    RxDualConstraintState last;
    RxDualDigest replay;                      /* SHA-256(REPLAY domain || 0 || controller_id || trace || u32 n || records) */
} RxDualReplay;

/* Feed every tick of `tr` through the controller. Tick 0 goes through
 * rx_dual_init_state (lambda starts at 0), the rest through rx_dual_update.
 * now_generation for tick i is tr->t[i].generation. calibration_ref is the
 * digest written into inputs whose tick has calibrated = 1 (may be NULL when
 * no tick is calibrated; a calibrated tick with a NULL or zero digest is
 * refused). Any refusal from the controller aborts the replay with that
 * status and leaves `out` unspecified: a refusal is a finding, never patched. */
RxDualStatus rx_dual_replay_run(const RxDualTrace *tr, const RxDualController *ctl, const RxDualResource *res,
                                RxDualClass cls, double budget, const RxDualDigest *budget_contract,
                                const RxDualDigest *calibration_ref, RxDualReplay *out);

/* Measures used by the DUAL-1 gate (pure functions of a series). */
/* Sign changes between consecutive nonzero first differences of x[lo..hi). */
uint32_t rx_dual_reversals(const double *x, uint32_t lo, uint32_t hi);
/* max(x) - min(x) over x[lo..hi); 0 for an empty range. */
double rx_dual_peak_to_peak(const double *x, uint32_t lo, uint32_t hi);

#endif /* OMEGA_RX_DUAL_REPLAY_H */
