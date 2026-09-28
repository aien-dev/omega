# Cognitive state projection

Gate `OMEGA_STATE_PROJECTION_PASS`.

## Rule

Cognition does not get "everything we know". A cognitive operation states
what it needs (`CognitiveNeed`). Omega turns that need into the smallest state
that is enough for the operation (`StateProjection`), using typed queries to
Cortex. Cortex never hands over its history.

```
CognitiveNeed { goal, operation_class, required_fact_types[],
                required_temporal_scope, required_world_scope,
                uncertainty_requirement, evidence_requirement,
                memory_scope, resource_budget }
        ↓  pj_compile (typed Cortex queries)
StateProjection { canonical_world_refs[], cortex_refs[], derived_features[],
                  temporal_windows[], hypotheses[], uncertainty[],
                  evidence_refs[], excluded_state[], recoverability_manifest }
```

Nothing here is text or tokens. A projection holds object references,
typed summaries and derived numbers. "Compile" means selecting and shaping
state from a declared need. It generates no code. It is not an Omega
compiler (see the M6 note in the project memory).

## What runs

Code:

- `src/runtime/rx_cortex.{c,h}`: Cortex, the typed append-only store.
- `src/runtime/rx_projection.{c,h}`: the need, the projection, the
  compiler, compression, recall, recovery and the audit.

Test: `tests/runtime/rx_state_projection.c` with
`tests/runtime/rx_sp_workloads.{c,h}`. Run it with `make test-state-projection`.
The build compiles `rx_cortex.o` and `rx_projection.o` on their own. It fails
if either one references the world, a faculty or Omega realization code.

### Cortex

Every object is a fixed header plus a payload of 64-bit words:

- class
- kind
- subject
- time
- generation
- branch
- protection bits
- tag
- up to four links

It carries a SHA-256 digest of its canonical encoding. The store keeps a
running hash chain over every digest. Two axes are kept apart:

- **Content class.** What the object is. These are the nine memory classes of
  doctrine AIEN.md §8.2: entity, claim, observation, execution, failure,
  evidence, plan, realization, relationship.
- **Treatment.** How one operation may use the object. It is decided per
  operation, never stored.

The indexes are kept per subject and in time order. A query costs the objects
in its range, not the size of the store.

Scope: one in-memory tier. The doctrine's L1/L2/L3 tiers (accelerator memory,
host memory, NVMe) are not implemented.

### Treatments

| Treatment | Meaning | Form |
|---|---|---|
| PINNED | always present; never dropped for budget | full |
| LIVE | the current value (with LATEST it is canonical world state) | full |
| DERIVED | replaced by one computed feature (count, sum, sum of squares, min, max); the sources can be recovered | feature |
| RECALLABLE | reference; the body can be recalled on demand, digest-checked | ref |
| COMPRESSIBLE | lossy summary of a sample series; the source can be recovered | summary |
| REFERENCE_ONLY | identity, header and digest only | ref |
| EPHEMERAL | valid for this operation only; outside the recoverability contract | full |
| BRANCH_LOCAL | only from the need's hypothetical branch; the main line never sees it | full |
| PROTECTED_EVIDENCE | evidence and protected state; never summarized | full, or ref with exact digest |

### Protected state

Five kinds of state are protected:

- authority
- commit receipts
- verification evidence
- generation identity
- effect receipts

These carry protection bits in Cortex. A need that asks to summarize, derive
or make ephemeral any protected object does not compile. The whole projection
is refused (`PJ_ERR_PROTECTED`). Nothing is silently weakened. Protected state
appears in full, or as a reference that keeps its exact digest.

A budget smaller than the pinned and protected state is also refused
(`PJ_ERR_BUDGET`). The compiler never trims silently.

### Excluded state and recovery

Excluded objects are not copied. The projection records three things:

- the Cortex object count and chain head it was compiled against
- how many objects it left out, per in-scope subject
- how many it left out from all other subjects

`pj_audit` goes through Cortex object by object. It gives each excluded object
exactly one reason (subject, type, time, branch, superseded, derived source).
It checks that each one is still intact, and that the projection's counts agree.

`pj_recover_all` checks every entry against Cortex:

- the digest of every entry
- that every summary is what its verified source summarizes to
- that every derived feature recomputes to the same numbers from verified
  sources

## Gate

Consumer: an explicit, rule-based reasoner in the style of R11's
statistical AIEN (part A), and the R11 AIEN faculty itself (part B). **Neither
is a neural model.** The consumer reads a projection and the whole store with
the same code, and filters for relevance itself. It therefore pays for every
object it is handed.

Histories are generated with **planted answers**. The generator decides the
cause, verdict or choice first, then writes a history in which it is true.
Both runs are scored against the planted answer, not against each other.

**Part A** has five operations, each run on 24 histories of 64 subsystems
(about 43,000 Cortex objects and 6.8 MB each):

| Operation | Need (abridged) |
|---|---|
| explain a regression | window since the last verified realization; own telemetry COMPRESSIBLE; resource changes PINNED; realizations of self and dependencies; prior regressions REFERENCE_ONLY (all time); hypotheses RECALLABLE; evidence as references |
| verify a cost claim | the claim (focus, PINNED) with its receipt in full; telemetry since the claim COMPRESSIBLE |
| plan the next experiment | latest authority grant PINNED; every past plan REFERENCE_ONLY (episodic memory) |
| what-if on a branch | branch-local changes; recent telemetry COMPRESSIBLE; the goal LIVE |
| assess a goal with uncertainty | window = current generation; telemetry DERIVED into one feature with second moments; goal LIVE |

Measured in a development run, as medians over 24 tasks. The committed receipt
is from candidate run `20260928T211355Z`, which has the same deterministic
columns.
Correctness, bytes and items touched are deterministic for the fixed seeds.
Latencies vary from run to run. The committed evidence receipt is the
authority.

| Operation | Correct (whole / projection) | Bytes | Items the consumer touched | End-to-end |
|---|---|---|---|---|
| explain regression | 24/24, 24/24 | 6,816,128 → 18,696 | 86,008 → 276 | 1.90 ms → 35 µs |
| verify cost claim | 24/24, 24/24 | 6,818,688 → 20,824 | 43,024 → 154 | 1.95 ms → 18 µs |
| plan next experiment | 24/24, 24/24 | 6,820,928 → 1,024 | 43,038 → 8 | 1.74 ms → 7 µs |
| branch what-if | 24/24, 24/24 | 6,818,992 → 1,872 | 43,025 → 14 | 1.79 ms → 4 µs |
| assess goal | 24/24, 24/24 | 6,819,376 → 336 | 86,052 → 3 | 1.84 ms → 19 µs |

End-to-end for the projection includes the compile. For the whole store it
includes building the whole-store view. The gate also requires the projection
end-to-end to beat the whole-store *reasoning alone*, so the result does not
rest on the cost of handing the store over.

**Part B** uses the real R11 faculty (`rx_aien.o`, native AIENOS authority).
Each of 8 histories covers one regime's lifetime (24 records under changing
placements), interleaved with three other regimes. It ends in a failed
prediction whose planted cause is a core-class change or drift. The history is
fed to the faculty twice:

- once whole: 1,217 objects, 703 AIEN activations
- once as the projection for "why did this prediction fail": 18 objects, 36
  activations

The projection is the last verified record, its receipt as a reference, the
demand since the record, and the placement carried in from before the window.
Both runs produce the planted hypothesis and the same plan in 8 of 8 cases.

**Part C** is scale. For "explain a regression" at 16, 64 and 256 subsystems,
the whole store grows from 1.7 MB to 27 MB. The projection stays at about
18 KB. Cortex objects examined stay at about 2,780.

### Definitions

- **Recall precision.** The fraction of provided items that passed the
  consumer's relevance filters and entered a decision (from its access trace).
- **Recall.** The fraction of the objects the planted answer rests on that the
  projection carries. An object counts as carried if it is an entry, or if it
  is a source of a derived feature.
- **Unnecessary-state ratio.** One minus recall precision (trace-based).
  Leave-one-out is also reported: remove each projected item alone and count
  the ones whose removal changes the answer.
  - Leave-one-out undercounts necessity for redundant statistical evidence. For
    example, no single telemetry interval out of 120 changes a mean-based
    verdict. So it reads high for explain (895 per 1,000) and verify (981 per
    1,000), while the trace-based ratio is 27 and 0.
  - Both are in the receipt.

### Controls that must fail

- **Leave-one-out.** Every task has at least one item whose removal changes the
  answer.
- **The newest objects in Cortex, at the same byte size as the projection.**
  0 of 120 tasks correct.
- **Everything about the subject, all time, in full.** It fails every
  dependency-caused regression (18 of 24 correct) and every goal assessment
  (0 of 24: the generation identity is machine state).
- **Under-specified needs.** Each one fails:
  - no dependency scope: 18 of 24
  - receipt as a reference instead of in full: 8 of 24
  - working memory instead of episodic: 9 of 24
  - main line instead of the branch: 0 of 24
  - whole lifetime instead of the current generation: 0 of 24
- **Protected state.** All 75 requests to summarize, derive or make ephemeral
  protected state were refused: 5 protections × 3 treatments × 5 operations.
- **Other refusals.**
  - A budget below the pinned state is refused.
  - BRANCH_LOCAL without a branch is refused.
- **Tampering.** Changing one word of a source the projection summarizes,
  references or derives from is caught by recovery and by recall, in 120 of
  120 tasks.

### Not claimed

- Neural cognition.
- Cortex storage tiers.
- Energy measurement.
- Live production histories. The part A histories are generated, and part B
  replays generated R11 objects.
- Code generation.
- Latency on any accelerator. Everything runs on the host processor.
