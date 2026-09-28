/*
 * rx_sp_workloads.h -- representative AIEN cognitive operations for the
 * state projection gate.
 *
 * Each task builds a Cortex history with a planted answer: the generator
 * decides the cause, the verdict or the choice first and writes the history
 * so that it is true. The consumer (sp_reason) is then scored against the
 * planted answer, never against itself.
 *
 * The consumer is an explicit rule-based reasoner in the style of R11's
 * statistical AIEN. It is not a neural model. It reads whatever view it is
 * given, the whole store or a projection, with the same code, and filters
 * for relevance itself, so it pays for every object it is handed.
 */
#ifndef RX_SP_WORKLOADS_H
#define RX_SP_WORKLOADS_H

#include "runtime/rx_cortex.h"
#include "runtime/rx_projection.h"

#include <stdint.h>

/* Cortex kinds used by the workloads (unique across classes). */
enum {
    K_GENERATION = 1, K_GOAL, K_TELEMETRY, K_RESOURCE, K_REALIZATION, K_VERIFY, K_COMMIT,
    K_EFFECT, K_AUTHORITY, K_REGRESSION, K_HYPOTHESIS, K_COST_CLAIM, K_PLAN, K_DEPENDS
};

enum { W_EXPLAIN = 0, W_VERIFY, W_PLAN, W_BRANCH, W_ASSESS, W_COUNT };

/* W_EXPLAIN causes, in the reasoner's order of precedence. */
enum { CAUSE_RESOURCE = 1, CAUSE_DEPENDENCY, CAUSE_RECURRENCE, CAUSE_DRIFT };
enum { V_SUPPORTED = 1, V_REFUTED, V_UNVERIFIABLE };
enum { G_MET = 1, G_UNMET, G_UNCERTAIN };

typedef struct { uint64_t a[6]; } SpAnswer;

typedef struct {
    uint32_t subjects;       /* subsystems, excluding the machine (subject 0) */
    uint64_t t_end;          /* "now" */
} SpScale;

#define SP_MAX_NEEDED 512u

typedef struct {
    uint32_t workload, seed;
    uint64_t subject, focus, t_now, t0;
    uint32_t branch;
    SpAnswer truth;
    uint64_t needed[SP_MAX_NEEDED];  /* objects the planted answer rests on */
    uint32_t n_needed;
    uint64_t deps[2];
} SpTask;

typedef struct {
    uint64_t touched;        /* items the reasoner iterated over */
    uint64_t words;          /* payload words it read */
    uint8_t *used;           /* per view item: entered a decision */
} SpTrace;

extern const char *const sp_workload_names[W_COUNT];

int  sp_build(CxStore *s, uint32_t workload, uint32_t seed, const SpScale *sc, SpTask *t);
void sp_need(const SpTask *t, CognitiveNeed *n);
void sp_reason(const SpTask *t, const PjView *v, SpAnswer *out, SpTrace *tr);

#endif /* RX_SP_WORKLOADS_H */
