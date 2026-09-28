/*
 * rx_aien.h -- AIEN as a resident cognitive faculty (ADR 0016 §44, R11).
 *
 * AIEN is not a service. Nothing calls AIEN and waits, and AIEN calls no
 * other faculty. Its cognition is a set of reactions on the shared world.
 * It observes what other faculties publish, keeps its own beliefs and
 * predictions as world objects, and publishes hypotheses and plans. Omega,
 * or any other faculty holding a read capability, may react to a plan.
 *
 *   aien.observe   demand interval, selection      -> belief
 *   aien.predict   belief                          -> prediction
 *   aien.explain   prediction state                -> hypothesis
 *   aien.assess    goal, prediction state          -> assessment
 *   aien.plan      hypothesis state, assessment    -> plan, memory, hypothesis
 *
 * What it reasons about here: the cost of one operation the body runs.
 * Omega's selection record is a claim ("this realization costs this much").
 * AIEN learns what production actually pays under that record, predicts it
 * keeps paying that, and notices when the prediction fails. It explains the
 * failure from what else it observed (a change of core class, or nothing),
 * and publishes one experiment per untried condition. It does not re-plan a
 * condition it has already had explored.
 *
 * The inputs are object references only. This file includes no Omega header
 * and calls no Omega function; the build checks that rx_aien.o has no
 * undefined omega_* or rx_omega_* symbol.
 *
 * The reasoning is a small explicit statistical model (interval means,
 * tolerance bands, streaks). It is not a neural model. The claim is the
 * faculty's form: when cognition happens, what it reads, what it publishes,
 * and that the body's behaviour changes because of it.
 */
#ifndef RX_AIEN_H
#define RX_AIEN_H

#include "rx_world.h"

#include <stdint.h>

/* Object types (RxObject.type). */
enum {
    RX_OT_PLACEMENT = 0x5311u, RX_OT_GOAL, RX_OT_BELIEF, RX_OT_PREDICTION,
    RX_OT_HYPOTHESIS, RX_OT_PLAN, RX_OT_ASSESSMENT, RX_OT_MEMORY
};

/* placement:  0 seq, 1 core class (MIDR part number, 0 = unknown), 2 cpus    (external)
 * goal:       0 seq, 1 regime, 2 target ns per call                          (external, human)
 * belief:     0 selection epoch, 1 realization word 0, 2 demand calls at last
 *             interval, 3 demand ns at last interval, 4 baseline mean ns,
 *             5 intervals seen, 6 last interval mean ns, 7 miss streak
 * prediction: 0 seq, 1 selection epoch, 2 regime, 3 hit streak, 4 core class,
 *             5 expected ns per call, 6 state, 7 observed ns when it failed
 * hypothesis: 0 seq, 1 kind, 2 prediction seq, 3 class then, 4 class now,
 *             5 expected ns, 6 observed ns, 7 state
 * plan:       0 seq, 1 action, 2 regime, 3 condition (core class),
 *             4 hypothesis seq (0 = none), 5 reason, 6 goal seq (0 = none)
 * assessment: 0 goal seq, 1 regime, 2 target ns, 3 expected ns, 4 status,
 *             5 core class, 6 prediction seq
 * memory:     0 count, 1..7 explored condition keys (rx_aien_key)
 *
 * The demand and selection objects are Omega's (rx_omega.h):
 *   demand    0 regime calls, 1 regime ns, 4 M, 5 N, 6 realization in use
 *   selection 0 epoch, 1 realization word 0, 6 regime
 * regime = (M << 32) | N. */

enum { RX_AIEN_PRED_HOLDING = 1, RX_AIEN_PRED_CONFIRMED, RX_AIEN_PRED_FAILED };

enum { RX_AIEN_HYP_CORE_CLASS = 1,   /* the record was made on another core class */
       RX_AIEN_HYP_DRIFT };          /* nothing observed changed; the record may be stale */

enum { RX_AIEN_HYP_OPEN = 1,
       RX_AIEN_HYP_TESTING,          /* a plan is out for it */
       RX_AIEN_HYP_SUPPORTED,        /* the new record under the condition holds */
       RX_AIEN_HYP_UNSUPPORTED,      /* a prediction under the condition failed again */
       RX_AIEN_HYP_EXHAUSTED };      /* the condition was already explored; no new plan */

enum { RX_AIEN_ACT_RESEARCH = 1 };   /* re-evaluate realizations for a regime, where it runs now */

enum { RX_AIEN_WHY_CORE_CLASS = 1, RX_AIEN_WHY_DRIFT, RX_AIEN_WHY_GOAL };

enum { RX_AIEN_GOAL_UNKNOWN = 1,     /* no confirmed prediction for the regime yet */
       RX_AIEN_GOAL_MET,
       RX_AIEN_GOAL_UNMET,           /* and the condition is unexplored: plan wakes */
       RX_AIEN_GOAL_UNMET_EXPLORED };/* already explored here; nothing to try */

#define RX_AIEN_MEMORY_SLOTS 7u

typedef struct {
    uint32_t baseline_intervals;     /* intervals averaged into the baseline */
    uint32_t tolerance_pct;          /* an interval outside +/- this is a miss */
    uint32_t misses_to_fail;         /* consecutive misses that fail a prediction */
    uint32_t confirm_intervals;      /* consecutive hits that confirm it */
    uint32_t min_calls;              /* fewer calls in an interval: too little evidence */
} RxAienConfig;

/* Objects other faculties own that AIEN observes. */
typedef struct {
    RxObjRef demand, selection;
} RxAienInputs;

typedef struct {
    RxObjRef placement, goal;                      /* written from outside */
    RxObjRef belief, prediction, hypothesis, plan, assessment, memory;
} RxAienObjects;

/* Resources: RX_AIEN_RES_BASE + index; the caller mints against them. */
#define RX_AIEN_RES_BASE 0x5311000ull
enum {
    RX_AIEN_RES_PLACEMENT = 0, RX_AIEN_RES_GOAL, RX_AIEN_RES_BELIEF, RX_AIEN_RES_PREDICTION,
    RX_AIEN_RES_HYPOTHESIS, RX_AIEN_RES_PLAN, RX_AIEN_RES_ASSESSMENT, RX_AIEN_RES_MEMORY,
    RX_AIEN_RES_COUNT
};

enum { RX_AIEN_SUBJ = 31 };

/* One capability reference per AIEN-owned resource, plus the two read-only
 * references to what AIEN observes. The root decides at run time whether each
 * still grants what the reaction declares. */
typedef struct {
    RxCapRef own[RX_AIEN_RES_COUNT];
    RxCapRef demand, selection;
} RxAienCaps;

typedef struct {
    RxWorld *w;
    RxAienConfig cfg;
    RxAienInputs in;
    RxAienObjects o;
    uint32_t r_observe, r_predict, r_explain, r_assess, r_plan;
} RxAienFaculty;

void rx_aien_default_config(RxAienConfig *cfg);

int rx_aien_create_objects(RxAienFaculty *f, RxWorld *w, const RxAienConfig *cfg,
                           const RxAienInputs *in);

int rx_aien_register(RxAienFaculty *f, const RxAienCaps *caps);

static inline uint64_t rx_aien_regime(uint64_t M, uint64_t N) { return (M << 32) | N; }

/* One explored condition: core class and regime. */
static inline uint64_t rx_aien_key(uint64_t core_class, uint64_t regime) {
    uint64_t M = regime >> 32, N = regime & 0xffffffffu;
    return ((core_class & 0xfffu) << 40) | ((M & 0xfffffu) << 20) | (N & 0xfffffu);
}

#endif /* RX_AIEN_H */
