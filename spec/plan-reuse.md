# Omega plan IR and verified plan cache (OMEGA_PLAN_REUSE)

Code: `src/runtime/rx_plan.{c,h}` (plan IR, identity, cache, applicability,
lifecycle), `src/runtime/rx_plan_arrange.{c,h}` (one planning domain and the
AIEN planner for it). Test: `tests/runtime/rx_plan_reuse.c`,
`make test-plan-reuse`. Receipt: `evidence/PLAN_REUSE/`.

This file was committed before the implementation. The gate criteria in §7
are binding; the receipt reports measured values against them and never
relabels a miss.

## 1. Purpose

Successful reasoning structure should be reusable. A plan is semantic
computation: a typed Omega action graph over World objects (`rx_graph.h`),
with the conditions under which it is valid and the evidence it must leave.
It is not prose and not a hidden chain of thought.

The action graph IR (merged before this work) states its own limit:
"procedures are fixed templates; no learned planner". This work supplies the
other half: AIEN produces a plan by search when none applies; the plan is
executed as an action graph, verified, generalised into a slot-parameterised
template and stored. A later goal of the same shape retrieves the template,
checks that it applies, binds its slots and runs it without searching.

## 2. PlanTemplate

```
PlanTemplate {
    semantic_id              SHA-256 of the canonical semantic bytes (§3)
    goal_shape               SHA-256 of the canonical goal shape (§4)
    preconditions[]          predicates over the goal object (kind, constraints)
    required_state[]         predicates over bound World objects (slots)
    required_capabilities[]  (slot, rights) the principal must already hold
    required_resources       RxResourceNeed per step and total energy
    action_graph             kind + digest; kind 1 = rx_graph AgGraph template
                             whose World objects are parameters 1..n (the slots)
    variable_slots[]         typed slots; the role each plays (goal / involved)
    invariants[]             predicates that must hold before and after the run
    evidence_requirements    evidence nodes of the graph; crumbs must verify
    success_predicate        predicates over slots after the run
    failure_predicate        graph FAILED, invariant broken, or evidence missing
    applicability_predicate  shape + preconditions + required state +
                             capabilities + resources + environment assumptions
                             + world generation + cognitive generation
    ancestry                 origin (search, adapted, re-derived), parent ids,
                             producing goal, producing cognitive generation
    verification_status      CANDIDATE, VERIFIED, STALE
}
```

Environment assumptions (for example "the station's arm is ready") are
predicates over named environment objects and are part of the plan's meaning.

## 3. Identity

`semantic_id` = SHA-256 over `OMEGA_PLAN_TEMPLATE_V1` followed by canonical
bytes of: goal kind, goal shape, slots, every predicate list (each list sorted
by its canonical record bytes, so builder order does not matter), required
capabilities, required resources, action graph kind and digest
(`rx_graph_identify` over the unbound template; builder-order independent),
evidence requirements, failure predicate flags.

Excluded: ancestry, verification status, counters, and everything about one
physical realization. A **realization** is one binding of the template in one
World: slot objects (id, generation), the capability references used, the
digest of the compiled and optimised graph, the world generation and the core
class the run was placed on. `realization_id` = SHA-256 over
`OMEGA_PLAN_REALIZATION_V1`, the semantic id and those fields. Many
realizations share one semantic id.

The verification record (world generation and cognitive generation the
template was last verified under) belongs to the cache record, not to the
semantic id. A template re-derived under a new World generation has the same
semantic id; its record is re-verified, not duplicated.

Canonical bytes round-trip: decode(encode(t)) re-encodes to identical bytes
and the same id.

## 4. Flow

```
goal object (World)
  -> derive goal shape                  (canonical, object identities abstracted)
  -> retrieve candidate templates       (cache index by goal shape)
  -> check applicability                (every axis of §5; first failing axis recorded)
  -> bind variables                     (goal slots from the goal, involved slots by
                                         following World references)
  -> verify capability/resource conditions
  -> execute                            (rx_graph compile, optimise, lower, run)
  -> verify                             (success predicate, invariants, evidence, crumbs)
```

When no template applies: AIEN plans by search over World predicates,
executes, verifies, and stores the generalised plan as a CANDIDATE.

Every retrieval decision (accept, or refuse with its axis) is published to a
World decision object by the principal, so it leaves a causal crumb.

## 5. Applicability axes (no blind replay)

| axis | refused when |
|---|---|
| shape | goal shape digest differs |
| binding | a slot cannot be bound, or a bound object is not live at that generation |
| goal constraints | goal kind differs, max steps below the plan's steps, energy budget below the plan's energy |
| required state | any required World predicate fails under the binding |
| authority | the principal's table has no capability that the World's authority view validates for a slot's required rights (never minted) |
| resources | a step's need does not fit the World's current budget |
| environment | an environment assumption predicate fails |
| world generation | the World generation differs from the one the template was verified under |
| cognitive generation | AIEN's cognitive generation differs from the one it was verified under |

### Lifecycle

- A plan found by search that executes and verifies is stored **CANDIDATE**.
- A CANDIDATE reuse first runs the action graph's sequential reference
  against a copy of the World (no effects). Only if the reference reaches
  the success predicate is it executed.
- A reuse that executes and verifies promotes to **VERIFIED**. A VERIFIED
  reuse skips the reference.
- A reuse that fails its success or failure predicate marks the template
  **STALE** and is counted as a false applicability. STALE is never reused.

## 6. Planning domain and planner (what is and is not claimed)

The domain is the classic arrangement ("blocks world") benchmark lifted onto
World objects. Unit objects (type 0x5B01) carry field 0 = what they rest on
(packed reference of another unit, 0 = floor) and field 2 = their own packed
reference. Moving a unit is a publication of field 0, authority-checked by
the engine under a capability the principal holds for that unit's resource.

The AIEN planner is A* over the World projection of every live unit, with an
admissible heuristic (unsatisfied goal facts). It is a small explicit search
model over World predicates, not a neural model. The claim is the reuse
structure, not the planner's intelligence.

A plan is generalised into a template: slots are goal objects plus every unit
the plan moves, moves onto, or that rests on a slot at the start (closure).
Required state: each slot's support relation (slot, floor, or a non-slot) and
"no non-slot unit rests on a slot". These are sufficient conditions for every
step of the plan to be legal and for the success predicate to follow.

Adaptation: when a template of the goal's shape fails only on required state,
the planner searches for a short prefix that reaches the template's required
state, then runs prefix and template. Adaptation overhead is reported.

## 7. Gate OMEGA_PLAN_REUSE_PASS (binding)

Workloads: two repeated goal shapes (reverse a tower; merge two towers), each
with at least 40 instances in fresh Worlds with different object identities
and distractor units. For every instance the first-use path (cache off, AIEN
search) runs in a mirror World built identically, and the reuse path runs in
its own World.

"Cognitive operations" = planner states expanded + successors generated +
heuristic evaluations (first use); goal-shape operations + candidates
retrieved + predicates evaluated + binding attempts (reuse). Cognition CPU
time = thread CPU time from goal read to a graph ready to compile.

| # | criterion |
|---|---|
| G1 | per shape, median cognitive operations: reuse <= first use / 20 |
| G2 | per shape, median cognition CPU time: reuse <= first use / 10 |
| G3 | every accepted reuse succeeds: success predicate, invariants, all evidence nodes carry evidence, crumbs verify; the outcome equals the first-use outcome (same goal facts hold, same number of steps, same number of evidence words) |
| G4 | false applicability = 0 over all accepted reuses, and at least 80 adversarial goals of a stored shape across the 9 axes of §5 (at least 8 per non-shape axis) are all refused, each refusal published as a World decision (crumb present) |
| G5 | identity: canonical round-trip; semantic id invariant under builder order and binding; a change to any semantic field changes it; two bindings give two realization ids and one semantic id; re-derivation under a new World generation yields the same semantic id |
| G6 | per shape, median end-to-end latency (goal to verified outcome): reuse < first use |

Reported, not gated: instructions retired (perf counter, when available),
package energy per cognition (SPBM hwmon, whole package, machine not quiet),
adaptation overhead versus full search, what blind replay (no applicability
check) does on each adversarial axis, lifecycle transitions.

## 8. Limits

- One planning domain. The plan IR and cache are domain-independent; the
  goal-shape derivation, planner and generaliser are per domain.
- Templates live in a host-side store; only the decision and index records
  are World objects. Durability through R9 is not claimed.
- A template's graph must fit the action graph's 64 nodes.
- Host processor only; no graphics processor run is claimed.
- Cognition (goal shape, retrieval, applicability, AIEN's search) reads a
  consistent host projection of the World taken under the world lock, not
  through capabilities. Execution is capability-checked by the engine.
- First-use cognition includes hashing the new template's identity, which a
  system without a cache would not do. The receipt also reports the CPU
  ratio with that hash excluded.
- Two workloads can share one goal shape (both build one six-unit tower);
  their templates differ in required state. Every candidate of the shape
  is checked and each refusal is recorded.

## 9. Owner decision (2026-09-28, after the qualification run)

The pre-registered verdict is FAIL: G1 required 20x fewer cognitive
operations for every workload, and the reverse-tower workload measured 1.2x
(its goal is too easy for the planner to leave reasoning to save). The
task's own gate asks for at least one repeated workload; merge-two-towers
meets it (26.6x fewer operations, outcome and evidence equal to first use,
0 false applicability). The owner chose to judge the gate by the task's
wording and merge. The receipt keeps both verdicts; neither is relabelled.
