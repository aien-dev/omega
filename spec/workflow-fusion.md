# Omega verified workflow fusion (OMEGA_WORKFLOW_FUSION)

Code: `src/runtime/rx_fusion.{c,h}`, plus the `AG_META` node kind in
`src/runtime/rx_graph.{c,h}`. Test: `tests/runtime/rx_workflow_fusion.c`,
`make test-workflow-fusion`. Receipt: `evidence/WORKFLOW_FUSION/`.

## Purpose

Omega runs the same verified action-graph fragments again and again. When a
fragment `A -> B -> C -> D` keeps coming back with the same semantic
dependencies, the same authority pattern and the same result contract, a low
failure rate and stable evidence, Omega may propose it as a **MetaSkill**:
one node that does what the steps did, with fewer reactions, crumbs and
scheduling rounds per run.

A MetaSkill never replaces behaviour silently. It starts as a candidate and
only becomes usable after verification, measurement, a canary, promotion
through the generation barrier and publication into the Skill Net table.
`rx_fusion_compile` applies only published MetaSkills and reports each fusion
it made; everything else in its library is refused and counted.

## Pipeline

| Stage | What happens | State after |
|---|---|---|
| observe | `rx_fusion_observe` takes a run only when the lowered result equals the sequential reference on every node (status, value, evidence, step evidence), the outcome is decided, and every evidence node of a successful run has its word. Fragments are enumerated and grouped by signature. | pattern |
| judge | too small; evidence unstable (a repeated input gave a different result or step evidence); failure rate over the policy; too few occurrences; seen in too few graphs | candidate or not |
| build | `rx_fusion_build`: ancestry, contracts derived from the fragment, realization, reference graph, observed samples | CANDIDATE |
| verify | `rx_fusion_verify` against the reference fragment | VERIFIED / REJECTED |
| measure | original and fused side by side; `rx_fusion_accept_measure` | MEASURED / SLOWER |
| canary | fused graph in shadow beside production; `rx_fusion_canary` | CANARY_PASSED / QUARANTINED |
| promote | `rx_fusion_promote`: an R9 generation carrying the realization bytes, identities, contracts and figures; the promotion right is checked on the native authority; the proposer cannot promote its own candidate | PROMOTED |
| publish | `rx_fusion_publish`: the promoted generation must be active and its bytes must equal the MetaSkill as it is now | PUBLISHED |

REJECTED, SLOWER and QUARANTINED are final.

## What a fragment may hold

So that one node means exactly what the steps meant:

- steps: CONST, PURE, WORLD_READ, RECALL, PHYSICAL, SKILL, RETRY,
  EFFECT_PROPOSE, and VERIFY as the exit;
- never an effect boundary (WORLD_PUBLISH, EFFECT_PERFORM). The effect chain
  stays as it was, by construction;
- no BRANCH or JOIN, no control dependency touching a step, no ON_FAIL edge
  between steps; an outside source may feed the fragment by ON_FAIL, and one
  source never feeds it in both modes;
- one exit: the only step whose result leaves the fragment; every step is an
  ancestor of the exit; no step but the exit is an evidence or condition node;
- the authority of the steps is READ on the objects they read, nothing else;
- it fits one reaction: ports + objects + run token + own cell + one
  authority-slot wake ≤ `RX_MAX_DEPS` (8), at most 4 ports and 2 objects.
  Larger fragments are counted (`fragments_over_budget`) and never proposed.

Enumeration grows each fragment backwards from its exit, adding a producer
only when every consumer of that producer is already inside, so every set is
convex and single-exit.

## The MetaSkill

```
AgMetaSkill {
    id, state, identity
    ancestry[]              graph digest, run, exit origin of the runs it came from
    input_contract          type and edge mode per port
    output_contract         type, may fail
    authority_contract      READ per object slot, nothing else
    resource_contract       merged need of the steps
    effect_contract         performs 0, publishes 0, proposals n
    realization             COMPILED | LEARNED | HYBRID | HARDWARE
    reference               the fragment as observed (a step program)
    samples[]               observed inputs and what the reference produced
    verdict, measure, canary counts, generation
}
```

Contracts are derived from the fragment, never chosen. The identity is
SHA-256 over the id, contracts, realization identity, reference signature and
ancestry.

### Realizations

A realization is not assumed to be text or code.

- **Compiled**: the step program — steps in canonical order (post-order from
  the exit over data inputs by port), inputs resolved to a port or an earlier
  step — run in one reaction. Each step runs through the same node semantics
  the graph uses (`semantics()` in rx_graph.c).
- **Learned**: a table learned from verified observations (inputs, the World
  fields read, result, step evidence). Nothing else: outside what it saw it
  cannot run, so verification rejects it (`not_runnable`). It is kept as a
  contract kind, not promoted.
- **Hybrid**: the learned table, and the step program on a miss.
- **Hardware**: a contract slot. No hardware realization exists on this host;
  verification rejects it (`not_runnable`).

### The AG_META node

`AG_META` is an action-graph node kind. `op` is the skill id; `imm`/`imm2`
hold the first 128 bits of the realization identity the node was built for,
and the node runs only a published realization with that identity. It carries
its input contract (`in_type`) and its object slots (`mobj`). Lowering gives
it one reaction reading its ports, its objects, the run token and its cell;
authority is derived from its objects (READ each). Its cell field 5 holds the
**step evidence**: the chain `H(acc, local step identity, status, value,
inputs)` over the steps that ran, which is exactly what the original steps
would have recorded. Identity, validation, dependency budgets, CSE, the
effect-ordering rule and the sequential reference all know the kind.

## Verification

`rx_fusion_verify` first checks, in order, and rejects on the first failure:

1. identity: resealed reference and realization, realization identity and
   MetaSkill identity equal what it carries;
2. contracts: input/output contracts equal those derived from the reference;
   the realization's program signature equals the reference's;
3. authority equivalence: READ per slot, equal to the reference's;
4. effect equivalence: no boundary in reference or realization; effect
   contract equal;
5. resource contract equal.

Then it runs vectors. Both sides run in the sequential reference: the
original fragment step by step as ordinary graph nodes, the candidate as one
`AG_META` node, behind the same port drivers (each port ends OK with a value,
FAILED, or SKIPPED):

- every observed sample: the fragment must reproduce what was recorded, and
  the candidate must agree with the fragment;
- every combination of OK / FAILED / SKIPPED over the ports, each with value
  and World sets (zeros, ones, all-ones, three random ranges, and the bounds
  of the steps' checks).

Per vector: exit status, value and attempts (semantic equivalence; failure
behaviour when a port was not OK or the exit was not OK), and step evidence
(evidence equivalence). Any mismatch rejects.

What each check catches (all exercised by the test): narrowed or widened
authority → authority; a claimed effect → effect; a change without a new
identity → identity; a changed program step → contract; a learned wrong value
→ semantic; a learned right value with wrong evidence → evidence; a learned
failure turned into success → failure behaviour; the reference and the
realization rewritten together → semantic (it no longer reproduces the
observations), and it matches no real workflow anyway.

Tampering with contracts cannot widen authority at run time either: a fused
node's authority is derived from its objects, and `rx_fusion_apply` checks
that it equals exactly the union of the replaced steps' authority.

## Measurement

Original and fused graphs run side by side in two worlds with the same inputs,
41 interleaved runs each at 4 workers, pinned to one core class:

- cognitive operations avoided: reactions that computed per run, crumbs per
  run;
- latency: median run time;
- resource use: process CPU time per run, reactions registered, World objects;
- success and failure counts;
- energy: the `aien_spbm` package and performance-core accumulators, two
  rounds of idle / original / idle / fused / idle (1 s each), idle power
  subtracted, the spread between rounds reported. A difference inside twice
  the spread is reported as not resolved. On a machine without the meter the
  receipt says `unavailable`.

Accepted when fewer reactions compute per run, the median is within the
original leg's own spread ((p75 − p25) / median, never less than 5 %) of the
original, and success and failure counts are equal. The receipt records the
spread and the tolerance used. A fragment of two
independent heavy steps is refused (SLOWER): fused, they no longer run side
by side.

## Canary

The fused graph runs in shadow beside the original on production traffic,
including inputs never observed. Per run: exit status, value and attempts,
outcome, the out and effect objects, and the fused node's step evidence
against the evidence recomputed from the original run. One divergence
quarantines the MetaSkill for good. The test's poisoned hybrid (one learned
entry for an input verification never generates) verifies and is then
quarantined by the canary.

## Promotion and publication

`rx_fusion_promote` proposes an R9 generation carrying the realization's
canonical bytes (`rx_graph_realization_encode`), the realization and
MetaSkill identities, the authority contract, the verification, measurement
and canary figures, and the ancestry, then promotes it with the caller's
request. The barrier checks the promotion right on the native authority and
refuses a proposer promoting its own candidate. `rx_fusion_publish` then
requires the generation to be active and re-reads the bytes: a MetaSkill
changed after promotion is not published.

`rx_fusion.o` references no AIENOS admin operation (checked by the build).

## Gate evidence (what the test proves)

| Claim | How |
|---|---|
| observation of verified runs only | a run that differs from its reference is refused |
| repeated fragments found across graphs | the workflow found in three procedures, with its failure count |
| unstable / flaky / too-big / single-graph fragments not proposed | judge reasons, budget counter |
| semantic, authority, effect, evidence, failure-behaviour equivalence | verification of compiled and hybrid; nine tampering cases caught |
| learned alone and hardware not promoted | both rejected `not_runnable` |
| never silent | an unpublished MetaSkill (verified, measured, canary-passed, promoted) is not applied and the graph is unchanged; after publication, three unpublished library entries are still refused |
| benchmark is a real gate | the parallel fragment is SLOWER |
| canary is a real gate | the poisoned hybrid is QUARANTINED |
| promotion needs authority | refused without the right and for the proposer |
| fused graphs equal the originals | every run: lowered == reference (incl. step evidence), original vs fused (exit, outcome, out and effect objects, step evidence), causal ancestry, evidence present, no bypass; determinism at 1, 2, 4, 8 workers |
| authority removed (golden path N3) | both sides block with BLOCKED_AUTHORITY and perform nothing; the fused node needs READ on A and nothing else; missing authority is reported on it |

## Limits

- Observation and proposal are host calls, not resident reactions.
- Fragments never contain effect boundaries, branches, joins or control
  dependencies; one exit; at most 8 steps, 4 ports, 2 objects.
- A node blocked for authority blocks the whole fused node (the steps before
  the blocked one do not run either) — conservative, as in the action graph.
- The compiled realization is a typed step program run in one reaction, not
  machine code. The learned realization is a table, not a neural module. No
  hardware realization is built.
- Energy saved per run is only claimed where it exceeds the meter's
  round-to-round spread.
- Host processor only. The authority is the native C library, not the AIENOS
  kernel.
