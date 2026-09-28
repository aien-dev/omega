/*
 * rx_plan_arrange.h -- the arrangement planning domain and AIEN's planner
 * for it (OMEGA_PLAN_REUSE, spec/plan-reuse.md §6).
 *
 * The classic arrangement ("blocks world") benchmark lifted onto World
 * objects. A unit (PL_OT_UNIT) has field 0 = what it rests on (packed
 * reference of another unit, 0 = the floor), field 1 = a label, field 2 =
 * its own packed reference. A unit is clear when no unit rests on it. A move
 * puts a clear unit on the floor or on another clear unit; it is one
 * publication of field 0, checked by the engine against a capability the
 * principal holds for that unit's resource.
 *
 * A goal is a World object (PL_OT_GOAL: 0 seq, 1 kind, 2 max steps,
 * 3 energy budget, 4 packed ref of a facts object). The facts object holds
 * up to eight packed facts "x rests on t" or "x rests on the floor".
 *
 * The planner is A* over the World projection of every live unit, with an
 * admissible heuristic (goal objects not yet in their final place, plus
 * outsiders resting on protected units). It is a small explicit search model
 * over World predicates, not a neural model. It reads the World and writes
 * nothing; the plan runs as an action graph.
 */
#ifndef RX_PLAN_ARRANGE_H
#define RX_PLAN_ARRANGE_H

#include "rx_plan.h"

#define PLA_MAX_UNITS    14u
#define PLA_MAX_FACTS    8u
#define PLA_MAX_MOVES    24u
#define PLA_MOVE_ENERGY  5u

enum { PLA_GOAL_ARRANGE = 1 };
enum { PLA_F_SUPPORT = 0, PLA_F_LABEL = 1, PLA_F_SELF = 2 };

/* Packed fact: x (id 8 bits, generation 24 bits), target likewise, bit 62 =
 * floor, bit 63 = present. */
uint64_t pla_fact(RxObjRef x, const RxObjRef *target);

typedef struct {
    uint64_t seq, kind, max_steps, energy;
    uint32_t n_facts;
    RxObjRef x[PLA_MAX_FACTS], t[PLA_MAX_FACTS];
    uint8_t floor[PLA_MAX_FACTS];
    /* Goal shape: chains of goal objects, bottom to top, sorted by code. */
    uint32_t n_obj;
    RxObjRef obj[2 * PLA_MAX_FACTS];            /* canonical order */
    uint32_t n_chains;
    struct { uint32_t start, len, grounded; } chain[2 * PLA_MAX_FACTS];
    uint8_t shape[32];
} PlaGoal;

/* Read the goal and its facts from the view and derive the goal shape.
 * Negative when the goal is malformed (cycle, two units on one, unknown). */
int pla_goal_read(const PlView *v, const PlEnv *env, PlaGoal *g, PlCost *cost);

/* Binding attempt `attempt` (permutations of interchangeable chains). Fills
 * slots and returns 1; returns 0 when there is no such attempt; -1 when this
 * attempt leaves an involved slot unbound. */
int pla_bind(const PlanTemplate *t, const PlView *v, const PlaGoal *g, uint32_t attempt,
             RxObjRef slots[PL_MAX_SLOTS], PlCost *cost);

typedef struct {
    uint32_t n;
    RxObjRef mover[PLA_MAX_MOVES];
    RxObjRef dest[PLA_MAX_MOVES];               /* ignored when to_floor */
    uint8_t to_floor[PLA_MAX_MOVES];
} PlaPlan;

/* What the planner must reach: facts, and units no outsider may rest on
 * (outsider = a unit not in `insiders`). Built from a goal, or from a
 * template's required state (adaptation). */
typedef struct {
    uint32_t n_facts;
    RxObjRef x[PL_MAX_SLOTS * 2], t[PL_MAX_SLOTS * 2];
    uint8_t floor[PL_MAX_SLOTS * 2];
    uint32_t n_insiders;
    RxObjRef insider[PL_MAX_SLOTS * 2];
    uint32_t n_protect;
    RxObjRef protect[PL_MAX_SLOTS * 2];
} PlaTarget;

void pla_target_from_goal(const PlaGoal *g, PlaTarget *tg);
/* Required state of t under `slots` as a target. Negative when the required
 * state has a predicate a search cannot aim at (a slot resting on an
 * unnamed non-slot). */
int  pla_target_from_template(const PlanTemplate *t, const RxObjRef *slots, PlaTarget *tg);

/* A*. 0 found (optimal length), negative: no plan within max_expand. */
int pla_plan(const PlView *v, const PlaGoal *g, const PlaTarget *tg, uint64_t max_expand,
             PlaPlan *out, PlCost *cost);

/* Apply a plan to a view (no World change). Negative on an illegal move. */
int pla_apply(PlView *v, const PlaPlan *p);

/* Generalise an executed-to-be plan found in view v0 (the World before it
 * ran) into a template: slots = goal objects plus the closure of units
 * resting on slots; required state, invariants, success, the action graph.
 * slots_out receives the binding of this origin. Negative when the plan
 * does not fit (slots, graph nodes). */
int pla_generalize(const PlView *v0, const PlaGoal *g, const PlaPlan *p, uint32_t origin,
                   const uint8_t *parent, uint64_t cog_gen, PlanTemplate *t,
                   RxObjRef slots_out[PL_MAX_SLOTS]);

/* The template's moves under a binding, in plan order (read from its graph). */
int pla_template_moves(const PlanTemplate *t, const RxObjRef *slots, PlaPlan *out);

/* Domain legality: every live unit rests on the floor or on a live unit, no
 * unit carries two, no cycle, and its self reference is intact. */
int pla_legal(const PlView *v);

#endif /* RX_PLAN_ARRANGE_H */
