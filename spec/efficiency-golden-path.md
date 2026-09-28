# Omega efficiency golden path (OMEGA_EFFICIENCY_GOLDEN_PATH)

Status: **pre-registered**. This file is committed before the
implementation and before any of its measurements exist. The criteria in §8
are binding. The receipt reports measured values against them and never
relabels a miss.

Test: `tests/runtime/rx_golden_path.c`, `make test-golden-path`.
Control leg alone (available before the prerequisites merge):
`make test-golden-path-control`. Receipt: `evidence/GOLDEN_PATH/`.

## 1. Purpose

The ten efficiency mechanisms below are each qualified alone by their own
gate. This gate qualifies them **as one system** on one realistic AIEN task,
repeated. The task starts novel and becomes familiar. Cost per result has to
fall as AIEN gains experience, and correctness must not fall with it.

| Prerequisite gate | Mechanism exercised here |
|---|---|
| OMEGA_ACTION_GRAPH_IR_PASS | the task runs as a typed action graph, by readiness, in parallel |
| OMEGA_STATE_PROJECTION_PASS | cognition gets a `StateProjection`, not the whole Cortex |
| OMEGA_PLAN_REUSE_PASS | the triage `PlanTemplate` is stored, then retrieved, checked and bound |
| OMEGA_INCREMENTAL_SEMANTICS_PASS | `DerivedSemanticValue`s are reused unless a relevant input changed |
| OMEGA_BRANCH_STATE_REUSE_PASS | hypothesis branches share their common prefix (`SharedStateRealization`) |
| OMEGA_CAPABILITY_QUERY_PASS | a `CapabilityNeed` query finds candidate realizations; AIEN does not read the catalog |
| OMEGA_COGNITIVE_ROUTING_PASS | each cognitive step goes to the cheapest `CognitiveRealization` that meets it |
| OMEGA_TYPED_RESULT_CONSTRAINTS_PASS | plans, hypotheses and effect proposals are checked against their contracts before publication |
| OMEGA_SEMANTIC_COMMUNICATION_PASS | branches exchange `SemanticProjection` deltas, not full state |
| OMEGA_WORKFLOW_FUSION_PASS | the repeated measure/compare/verify/record fragment becomes a `MetaSkill` |
| OMEGA_EMPIRICAL_OPTIMIZER_PASS | realization choices come from the learned cost model, not fixed rules |

The names in backticks are the objects each prerequisite gate defines. This
gate calls them through the interfaces those gates merge. It does not
reimplement any of them. Where an interface differs from what is written
here, the implementation follows the merged interface and this file records
the change in §10 before the qualifying run.

## 2. Scenario: regression triage

AIEN is given a goal: *operation O on subsystem S got slower; find out why,
requalify it, and publish a verified placement.* This is the domain the R11
faculty already reasons about (the cost of an operation the body runs, and
why a prediction about it failed). It contains every ingredient the gate
asks for:

| Ingredient | In this task |
|---|---|
| new goal | a regression goal object for (S, O), published by the outside |
| memory requirement | Cortex holds prior regressions, last verified realization, telemetry history; most of it is irrelevant to this (S, O) |
| capability discovery | which realizations of O exist, on which cores/devices, under which authority; a synthetic catalog of 10,000 entries, a handful relevant |
| several independent subtasks | measure each candidate realization; each is independent of the others |
| J-Space branching | three competing hypotheses explored as branches from one shared prefix: core class changed, input shape drifted, realization regressed |
| physical computation | the candidate realizations really run (host CPU cores; GB10 seat when present) with declared resource needs |
| verification | each candidate's output is checked against the reference result; the chosen placement is checked against the goal's target |
| reusable plan | the triage plan (projection → query → branch → measure → verify → decide → publish) |
| repeated subworkflow | measure → compare with reference → verify → record, once per candidate per branch |
| effect | the verified placement is published (an effect boundary with WRITE and EFFECT authority) |

### Encounter sequence

The sequence is fixed and seeded before the run. Each encounter is one goal.

| Phase | Encounters | What changes | Expected mechanism |
|---|---|---|---|
| P1 first encounter | E1 | novel (S1, O1) | full exploration: AIEN plans from scratch |
| P2 repeated encounter | E2–E3 | same (S1, O1), new telemetry | plan reuse; less cognition |
| P3 familiar family | E4–E9 | related (S2..S4, O1..O2); one unrelated World field changes between encounters; one relevant field changes once | incremental reuse, shared branch state, better routing, pruned communication |
| P4 stable workflow | E10–E16 | the family, repeated | fusion candidate verified, benchmarked, canaried, published through the generation barrier; later encounters run the MetaSkill |

Negative encounters, interleaved (their position is seeded):

- **N1 false applicability:** a goal of the same shape after the World
  generation of a slot object changed. The cached plan must be refused and
  re-planned or adapted. Blind replay fails the gate.
- **N2 relevant change:** the core class of the placement changes. Derived
  values that depend on it must be recomputed; the rest reused.
- **N3 authority removed:** the principal's effect capability is revoked
  before an encounter. Both legs must block at the effect and record it the
  same way. A fused MetaSkill must not carry authority it was not given.
- **N4 recovery:** a worker is killed mid-run (R14-style). Both legs must
  recover to the same result with verifying crumbs.

## 3. Configurations

- **Optimized**: all ten mechanisms on; the graph optimizer on; N workers.
- **Control**: the same goals, the same World, the same authority, the same
  physical computation. Every mechanism off: no projection (cognition gets
  full available state), no plan cache (AIEN plans every encounter), no
  derived-value reuse, independent recomputation per branch, catalog passed
  whole, the single most capable cognitive realization for every step,
  unconstrained generation followed by validation only, full-state broadcast
  between branches, no fusion, fixed-heuristic realization choice, graph
  optimizer off, one worker.
- **Ablations**: optimized with exactly one mechanism off, one run per
  mechanism. This shows each part carries weight inside the whole and that
  none of the gain comes from one mechanism hiding a regression in another.

Each mechanism is switched by one flag in one configuration struct. There is
no code path that exists only in one leg other than the mechanisms
themselves.

## 4. Measurements (per encounter, per leg)

| Metric | Definition |
|---|---|
| time/result | wall ns from goal publication to the verified placement effect |
| energy/result | SPBM package energy over the encounter when the signed telemetry reader is loaded (R15); otherwise the declared-energy model from `RxResourceNeed`, labelled `model` in the receipt, never mixed with measured |
| memory/result | peak bytes of World objects, projections, cached values and branch state the encounter held |
| cognitive compute/result | work done by cognitive realizations: planner expansions, hypothesis evaluations, assessments, each weighted by its routed realization's measured cost |
| physical compute/result | busy ns of PHYSICAL / SKILL / MetaSkill nodes |
| bytes/result | bytes that crossed into cognition (projection + query results) plus bytes exchanged between branches |
| reasoning/planning activations | count of general-cognition planner and reasoning activations |
| reuse percentage | reused / (reused + computed) over plans, derived values and branch state |
| parallelism | total work / critical path, and measured 1-worker / N-worker time |
| task success | the placement published and verified against the goal target |
| verification failures | VERIFY nodes that failed, and typed-contract rejections before publication |

## 5. The invariant: efficiency never bought with correctness

For every encounter, the optimized leg is compared with the control leg.
This is a checker in the test, not a statement in this file. Each item must
be identical, or strictly stronger in the stated direction:

| Property | Check |
|---|---|
| semantic result | same placement, same measured-candidate verdict set, same semantic digest of the outcome |
| authority checks | every effect and publication validated at activation start and at publish in both legs; optimized count ≥ control count per effect node; 0 bypasses (engine audit) |
| effect semantics | same effect objects, same count, same order-chain value |
| causal evidence | every required evidence node present; crumb chains verify; the ancestry of each effect reaches its goal and its verification in both legs |
| generation integrity | same World object generations after the encounter; reused plans, values and branch states were bound to the generations they were derived under; the MetaSkill entered only through the generation barrier |
| recovery | N4 recovers in both legs to the same result |

Any mismatch is a gate failure. It is not averaged away.

## 6. Developmental curve (qualitative, never target numbers)

The receipt reports the per-phase medians. The criteria are orderings only.
No number in this file is a target.

- P1: optimized cognitive compute is within the same order as control (the
  first encounter is genuinely novel).
- P2 < P1 in planning activations and cognitive compute per result
  (plan reuse).
- P3 ≤ P2 in cognitive compute per result, and P3 reuse percentage > P2
  (incremental and branch reuse), with at least one routed step going to a
  cheaper realization than in P1.
- P4 < P3 in cognitive compute per result; after MetaSkill publication the
  fused fragment runs with zero general-cognition activations; the only
  cognition left is what the goal itself requires (assessment and routing).
- Reuse percentage non-decreasing across P1..P4.

## 7. Candidate binding

The receipt binds to one candidate SHA on `main` whose history contains the
evidence commit of all eleven prerequisite gates. It lists each
prerequisite's receipt digest. It records the binary digest, the host, the
cores used and whether energy was measured or modelled.

## 8. Gate criteria (binding)

OMEGA_EFFICIENCY_GOLDEN_PATH_PASS requires all of:

1. All eleven prerequisite receipts present on the candidate's history.
2. §5: zero invariant mismatches over all encounters, including N1–N4.
3. N1: the stale plan is refused (false applicability rate 0).
   N2: no stale derived value is reused (false-hit rate 0).
   N3: blocked in both legs, identically. N4: recovered in both legs.
4. §6: every ordering holds.
5. Optimized beats control on aggregate time/result, cognitive
   compute/result and bytes/result; is not worse on task success or
   verification failures; energy/result is lower when measured.
6. Each ablation is worse than the full stack on at least the metric its
   mechanism targets (so every mechanism contributes).
7. Each of the ten mechanisms records at least one counted use in the
   optimized leg.
8. ASan/UBSan clean; TSan clean for the multi-worker leg.

## 9. Not claimed

- No claim about neural or text-based cognition. AIEN's cognition here is
  the explicit models the prerequisite gates provide.
- The absolute figures depend on this scenario and host. The claim is the
  trajectory and the invariant, not the size of the gain.

## 10. Interface reconciliation log

(Filled in when the prerequisite gates merge, before the qualifying run.)

- 2026-09-28, before any optimized-leg measurement. **OMEGA_PLAN_REUSE.**
  Its receipt (`evidence/PLAN_REUSE/d8c8246c….json`) records the
  pre-registered verdict `OMEGA_PLAN_REUSE_PASS: FAIL` (G1: the
  reverse-tower workload measured 1.2x fewer cognitive operations, not 20x)
  and, beside it, that the task wording is met (merge-two-towers, 26.6x).
  The owner chose to judge that gate by the task wording and merge
  (`spec/plan-reuse.md` §9). For §8.1 this gate counts that prerequisite as
  **accepted by owner decision**, not as passed. The receipt carries both
  verdicts. Plan reuse is not taken on trust here: §6 P2 < P1 and §8.3 N1
  test it independently inside this scenario, and the golden path fails if
  either misses.
- **Rule 7 (owner, 2026-09-28).** The GB10 seat is not used: physical
  computation runs on host CPU cores only, N4 kills a host worker and never
  the seat, and no test that launches the seat is ever cut short. The
  receipt records `gpu_seat: not used (rule 7)`.
