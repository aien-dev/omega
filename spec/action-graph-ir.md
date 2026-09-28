# Omega action graph IR (OMEGA_ACTION_GRAPH_IR)

Code: `src/runtime/rx_graph.{c,h}`. Test: `tests/runtime/rx_action_graph.c`,
`make test-action-graph`. Receipt: `evidence/ACTION_GRAPH/`.

## Purpose

Routine multi-step behaviour should not return to deliberation at every
step. AIEN states a goal. Omega compiles the goal, the World, the capabilities
the principal already holds, and the goal's constraints into a typed action
graph. Omega checks the graph, optimizes it, and lowers it onto resident
reactions. After that the graph runs by readiness alone, every time its run
token moves.

This is not a tool-call DAG. Nodes are typed semantic operations over World
objects, Cortex memory, Skill Net procedures and effects. Nothing in the
graph is prompt text, JSON, shell or source code. A backend may later lower a
node into one of those. The graph stays typed Omega state.

## Boundaries

| Faculty | Role here |
|---|---|
| AIEN | states the goal (goal kind + World objects). Not changed by this work. |
| Omega | compiles, validates, optimizes, lowers. Has no admin handle. |
| AEGIS | decides policy when authority is missing (R8 slow path). |
| AIENOS | the only minter (native C capability authority). |
| World | cells, run token, effect objects; readiness drives execution. |

Omega gains no authority. `rx_graph.o` is linked alone first and the build
fails if it references any `aienos_cap_*` admin operation.

## Integration map (what is reused)

| Need | Existing mechanism |
|---|---|
| concurrency from dependencies | rx_world dependency index + worker pool (R3) |
| typed authority at start and publish | rx_world `validate_caps` twice per activation (R3/R7) |
| missing authority | AEGIS request/decision/slot, root.install mints (R8) |
| resource admission | rx_world `RxResourceNeed` / budget (R5) |
| stale references | object generations; `RX_ERR_STALE_GEN` invalidation (R3/R9) |
| causal record | content-addressed crumbs with parents and episodes (R4/R6) |
| pure operations | `omega_eval_pure_binary_uint` (Omega core) |

New: the IR, its validation and identity, compile, passes, lowering, and the
sequential reference. The engine is unchanged.

## The graph

```
AgGraph {
    nodes[]                     kind, out type, params, bound World object, cost
    data[]                      typed edges (DATA or ON_FAIL) into numbered ports
    deps[]                      GUARD (branch, polarity) and ORDER edges
    auth[]                      authority requirements (resource, rights)
    res[]                       resource requirements (RxResourceNeed)
    success[], failure[]        outcome conditions (node, status)
    evidence[]                  nodes whose evidence must exist after a run
    effects[]                   effect boundaries in their required total order
}
```

Node kinds: CONST, PURE (Omega op, possibly fused), WORLD_READ,
WORLD_PUBLISH, RECALL (Cortex memory), CAP_RESOLVE, PHYSICAL, SKILL,
EFFECT_PROPOSE, EFFECT_PERFORM, VERIFY, BRANCH, JOIN, RETRY.

Types: U64, BOOL, PROPOSAL, VERDICT, RECEIPT. EFFECT_PERFORM takes exactly a
PROPOSAL and a VERDICT; a raw number cannot be performed.

### Status per run

Every node ends each run OK, FAILED or SKIPPED, or stays PENDING (blocked).
Rules, in order: a failed or skipped guard, or a guard of the other polarity,
gives FAILED / SKIPPED; a FAILED order source gives FAILED; JOIN takes the one
OK input; otherwise a SKIPPED input gives SKIPPED, then a FAILED input gives
FAILED; an ON_FAIL input runs the node only when its source FAILED. One
function (`semantics()`) implements this for both the lowered reaction and the
reference.

### Validation

- types per kind; ports dense from 0; acyclic;
- effect boundaries (PUBLISH, PERFORM) form one total order;
- every node that reads an object some boundary writes is ordered against
  that boundary (no timing-dependent reads);
- the arms of every JOIN are mutually exclusive (opposite guards, or OK vs
  FAILED of the same node);
- at most one guard per node; the lowered dependency count fits
  `RX_MAX_DEPS`.

### Identity

Node identity is SHA-256 over kind, parameters, bound object and generation,
fused steps, input identities by port, and control dependencies sorted by
identity. A World read or recall also folds in every ancestor publication of
the same object, so reads separated by a publication are different. A
boundary folds in its position in the effect chain. The graph digest sorts
node identities and condition/evidence records, so builder order does not
change it; a semantic change does.

## Compile

`rx_graph_compile(goal, world, caps, constraints, library, optimize)`:

1. choose the procedure for the goal kind (Skill Net template);
2. bind World parameters to the goal's objects; refuse stale references;
3. resolve CAP_RESOLVE nodes against the principal's table (a constant);
4. validate; derive authority from each target's resource and evidence for
   VERIFY and boundaries;
5. constraints: max effects, forbidden resources, critical-path deadline;
6. optimize and check preservation;
7. report missing authority and resource-blocked nodes.

Missing authority and missing resources do not refuse the graph; they are
reported and the lowered node waits. Stale references, violated constraints,
and type or structure errors refuse it.

## Passes

Passes 1–4 repeat until a round changes nothing; then 5.

1. **Constant propagation.** A PURE node whose inputs are unguarded CONSTs
   becomes a CONST (a fold that would fail is left alone).
2. **Common subexpressions / redundant World reads.** Identity-equal nodes
   merge. Never merged: boundaries, evidence nodes, condition nodes, BRANCH,
   JOIN.
3. **Pure fusion.** A unary PURE step whose producer is a PURE node with no
   other consumer folds into it.
4. **Dependency simplification.** An ORDER edge goes when a strict path
   (DATA into non-JOIN, or ORDER, through nodes that can never be skipped)
   already implies it, or when its source can neither fail nor skip and
   neither end is a boundary. This is the parallelization pass: it removes
   sequencing the plan stated but the semantics do not need.
5. **Dead nodes.** Everything not an ancestor of a boundary, evidence or
   condition node goes.

`rx_graph_check_preserved` then requires the same boundaries in the same
order, every evidence node, and every condition.

## Lowering

- one run token object (field 0 = run id; outside WRITE, principal READ);
- one cell per node: 0 run, 1 status, 2 value, 3 evidence, 4 attempts;
  every field is proposed on every run (`stamp_proposed`);
- one reaction per node, faculty OMEGA, subject = the principal. Triggers:
  the cells of its inputs, guard and order sources; source nodes trigger on
  the run token. Reads: run token, own cell, the World target. Writes: own
  cell, and the boundary target;
- a reaction whose inputs are not all in for the current run proposes
  nothing (NOOP). The last arriving input makes it compute. A node computes
  once per run.

No function sequences the nodes. Independent nodes are READY together and
run on whatever workers are free.

Authority per node: cell RW, run R, and the derived requirement. A held
capability is the resident fast path. A missing one lowers to an unusable
reference (the engine blocks and records BLOCKED_AUTHORITY), or, with an AEGIS
binding, to a slotted capability plus an `ag.ask` reaction that files the
request. AEGIS decides; the root mints into the slot; the slot wakes the
waiting node.

## Effects

EFFECT_PROPOSE is thought: a value, no authority, no change. EFFECT_PERFORM
needs R|W|EFFECT on the effect object, a passing verdict, and its place in the
chain. The effect object holds count, last value, an order chain
`H(chain, run, value)`, and the last run; the chain proves order.

## Gate evidence (what the test proves)

| Claim | How |
|---|---|
| same semantic result as sequential reference | every run: status, value and evidence of every node, outcome, effect chain |
| lower critical path for parallel graphs | 1 vs 4 workers, same lowered graph, median of 15 runs, pinned to one core class |
| zero authority bypass | every commit to a boundary target comes from a lowered boundary reaction; stolen, revoked, forged and tampered cases blocked; compile mints nothing |
| zero missing evidence | every evidence node of every successful run has its word |
| same causal ancestry | per node, crumb parents inside the run's episode reach exactly the graph's ancestors and the run token |
| deterministic semantic result | 8 runs × worker counts 1, 2, 4, 8, digest of all node results + effect object |

Required scenarios: linear, parallel, diamond, branch/join (both arms),
failure branch (+ bounded retry, + exhausted retry), effect boundary (+
proposal only, + constraints), authority-blocked (AEGIS + human approval),
resource-blocked (and resumed), stale-generation (compile-time and after
lowering with the slot reused).

## Limits

- Procedures are fixed templates; no learned planner.
- One AEGIS request per lowered graph.
- RETRY loops inside its node; attempts do not re-enter the World.
- A node blocked for authority or resources blocks its dependants even when
  it would have been SKIPPED (conservative).
- Up to 64 nodes, 4 data inputs per node.
- CAP_RESOLVE is decided at compile time; the engine still checks every use.
- The reference ignores resource budgets; resource scenarios compare after
  admission.
- Host processor only. The authority is the native C library, not the
  AIENOS kernel.
