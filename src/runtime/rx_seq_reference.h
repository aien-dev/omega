/*
 * rx_seq_reference.h -- R15 sequential control. REFERENCE ORACLE ONLY.
 *
 * This is the central-orchestration pattern ADR 0016 retires, rebuilt on the
 * same world so R15 can price it fairly (spec/r15-performance-proof.md §2.1):
 * a master loop walks a fixed faculty order, polls each stage for new input
 * the way a heartbeat scans pending tasks, and runs a ready stage to
 * completion on its own thread before looking at the next. A graphics stage
 * posts its claim and the loop waits for the completion.
 *
 * It runs the engine's own activation (validation, snapshot, reaction body,
 * re-validation, atomic publication, causal crumb). Only who decides what
 * runs next differs. It works only on a world made with
 * rx_world_init_sequential_reference; a production world refuses it.
 * Nothing in production links this file.
 */
#ifndef RX_SEQ_REFERENCE_H
#define RX_SEQ_REFERENCE_H

#include "rx_world.h"

/* One attempt at moving graphics completions into the world (the caller's
 * transport: the chip ring or the processor stand-in). 0 or a negative code. */
typedef int (*RxSeqGpuStep)(void *ctx);

typedef struct {
    const uint32_t *order;      /* reaction ids in the fixed faculty order */
    uint32_t n;
    RxSeqGpuStep gpu_step;
    void *gpu_ctx;
    uint32_t gpu_timeout_ms;    /* give up waiting on one claim after this */
} RxSeqPlan;

/* Declared in rx_world.c; refused on any world that is not the reference. */
int rx_world_init_sequential_reference(RxWorld *w, const void *auth_ctx,
                                       RxAuthValidateFn validate, RxAuthInspectFn inspect,
                                       uint64_t crumb_cap);
int rx_world_seq_activate_locked(RxWorld *w, uint32_t rid, uint64_t cause);
/* The same, carrying the readiness poll that found the stage ready (its
 * wall start, wall and thread-CPU duration) into the activation's timing. */
int rx_world_seq_activate_timed_locked(RxWorld *w, uint32_t rid, uint64_t cause,
                                       uint64_t poll_start, uint64_t poll_wall,
                                       uint64_t poll_cpu);
struct AienosCapView;
int rx_world_init_native_sequential_reference(RxWorld *w, const struct AienosCapView *view,
                                              uint64_t crumb_cap);

/* One pulse over the plan. *ran receives the number of stages run.
 * Returns RX_OK, or RX_ERR_TIMEOUT if a graphics claim never completed.
 * A pulse that runs nothing marks the world quiescent only when the plan
 * covers every registered reaction (plan->n >= w->n_reactions); a stale
 * plan must be rebuilt before the world can be called idle. */
int rx_seq_pulse(RxWorld *w, const RxSeqPlan *plan, uint32_t *ran);

/* The legacy loop shape: pulse until a pulse runs nothing or max_pulses. */
int rx_seq_run_until_complete(RxWorld *w, const RxSeqPlan *plan, uint32_t max_pulses,
                              uint32_t *pulses);

#endif
