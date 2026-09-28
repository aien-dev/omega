# Omega semantic variables and incremental recomputation

Gate: `OMEGA_INCREMENTAL_SEMANTICS_PASS`
Code: `src/runtime/rx_semantic.{h,c}`
Test: `tests/runtime/rx_sem_incremental.c` (`make test-sem-incremental`)

## Objective

Do not recompute cognition or derived state when nothing relevant changed.

## Model

**SemanticValue** (`RxSemValue`) has these fields:
- `semantic_id`
- `type`
- `value_version`
- `dependencies[]`
- `generation`
- `derivation_id`
- `confidence` (ppm)
- `evidence_refs[]`
- the value words and their content digest.

Semantic values come in two kinds:
- **Source.** One field of one world object slot. The id is
  `(kind, object id, field)`. The generation is the world object's generation.
  The evidence is the causal crumb that last wrote the field.
- **Derived.** One pure function over named inputs. The id is structural:
  `H(derivation, inputs)`, so the same question has the same id in every
  branch. The generation is the function's implementation generation.

**DerivedSemanticValue** (`RxSemDerived`) is one immutable cache entry:
- `function_id`
- `input_versions[]`: for each input, its semantic id, generation, value digest,
  branch-local version, and evidence
- `result`
- producer provenance: branch, agent, task, J-Space candidate, and measured
  compute time
- an entry digest over all of the above.

## Rule

A result is reused if and only if all of these match:

```
derivation_id                 = H(name, out_type, implementation generation, implementation digest)
for every input:  semantic_id, generation, digest(type, value words)
```

The key is SHA-256 over exactly these values. A cache hit also compares the
full stored input record, not only the digest.

The key leaves out the following, on purpose:
- branch-local versions;
- which branch, agent, task or candidate asked;
- evidence;
- wall-clock time.

None of these change meaning.

## Three layers

Each layer is cheaper than the next one.

1. **Branch memo.** A CLEAN node is served with no work.
2. **Early cutoff.** A DIRTY node rebuilds its key. If the key is unchanged,
   the memo is still exact and nothing runs.
3. **Content-addressed cache.** The cache is shared by every branch, agent,
   task and J-Space candidate in the engine. A hit reuses a result computed
   elsewhere from identical inputs. A miss runs the function once and files
   the result.

## Invalidation (field-granular)

Writing a source changes one field. It marks DIRTY only the nodes that
declared that field, plus their transitive readers, and only in the branch
that was written. Two kinds of change mark nothing:
- a write of the same content and generation;
- a change to any other field or object.

Example: a temperature change dirties thermal placement and the plan that
reads it. It does not dirty any of these:
- repository analysis
- memory retrieval
- capability resolution
- fan health, which reads another field of the same machine object.

The test measures an object-granular baseline for comparison.

`rx_sem_sync_world` pulls bound fields from `rx_world`. It skips any field
whose world `field_version` did not move. Retiring an object has two effects:
- Its sources become stale. Everything downstream fails with
  `RX_SEM_ERR_STALE` until `rx_sem_rebind`.
- The re-created object carries a new generation. Results from the old object
  are therefore never served for it, even when the bytes are identical.

## Branches and J-Space

A branch is a forked semantic view with its own source values and its own
memo. A J-Space candidate is a branch that has a candidate id (see
aien-architecture `docs/06-jspace.md`). A write in one branch never reaches
the parent, a sibling, or the world. The cache can be shared safely because
its keys are the complete content of what each result was derived from.

## Provenance

Every PRODUCE and REUSE is a hash-chained event:
- A REUSE names the entry it served, the consumer (branch, agent, task,
  candidate), and a digest of the consumer's own input evidence.
- A served value's `evidence_refs` point to its producing entry.
- A source input's evidence is the digest of the world causal crumb that wrote
  it.

`rx_sem_verify` checks all of the following:
- It recomputes every key, every entry digest and every event link.
- Every CLEAN memo in every live branch equals its entry's result.
- Every entry's inputs equal what that branch holds now. This is the
  no-stale-memo invariant.

## Qualification

The qualification has two parts: PROVE, which checks correctness, and MEASURE,
which replays the same workload four ways.

### PROVE

All of these run against a real `rx_world` with capabilities:

| check | what it shows |
|---|---|
| dependency_tracking | Inputs match the declarations. The reverse index is right. Downstream of temperature is exactly {placement, plan}. Node ids are idempotent. A missing input is refused. |
| correct_invalidation | 400 unrelated world writes cause 0 invalidations and 0 runs. Temperature change: 2 nodes dirty, 4 stay clean, exactly 2 runs. Fan change: fan health only. A same-value write invalidates nothing. Early cutoff: the plan is not rerun when the placement comes back the same. |
| no_stale_reuse | 200,000 randomized operations: writes from a small value domain (so values recur), forks, drops and gets over up to 12 branches. Every get is compared with a from-scratch evaluator, and every hit is recomputed in audit mode. Also a directed A-B-A case. |
| generation_isolation | An object retired and re-created with the same bytes is refused while stale, then recomputed. A promoted function generation is recomputed and never served generation-1 results. |
| branch_isolation | Two candidates with different writes. The parent memo, the parent values and the world digest are unchanged. A sibling does not see the other's writes. A parent world change does not reach forked candidates. Dropping one candidate leaves the other intact. |
| cross_branch_valid_reuse | Another agent on another task gets the whole plan from the cache with 0 runs. 16 J-Space candidates with 4 distinct proposals: 8 runs and 24 cache hits, and duplicates are identical. |
| causal_provenance | A reused value explains back to its producer and to the world crumb that wrote its input. World crumbs verify. A REUSE event exists for the consumer. A tampered entry or event is detected. |

### MEASURE

One deterministic organism trace: 600 ticks.
- On every tick:
  - 8 noise objects are written;
  - the fan jitters;
  - the agent asks for a plan.
- Every 3 ticks the temperature moves; 80% of those moves stay inside the same
  5 °C bucket.
- Rarer changes:
  - the repository changes every 50 ticks;
  - the memory topic changes every 25 ticks;
  - the capability table changes every 100 ticks.
- Every 20 ticks, J-Space evaluates 6 candidate settings, each drawn from 3.

The four replay modes:

| mode | what it does |
|---|---|
| FULL | Recomputes every derivation on every query. |
| OBJECT | Memo plus cutoff, with object-granular invalidation. |
| FIELD | Memo plus cutoff, with field-granular invalidation. |
| ENGINE | FIELD plus the shared cache. |

Checks:
- Every mode's answer to every query equals FULL's answer.
- An audited ENGINE replay recomputes every hit, and the gate requires
  `false_hits == 0`.

The receipt reports:
- recomputation avoided;
- compute saved;
- latency saved, with p50 and p99;
- cache-hit rate;
- false-hit rate;
- unnecessary invalidation rate for each mode;
- energy.

**Energy method.** The measurement uses the on-device counters for package,
CPU-E and CPU-P from hwmon `aien_spbm`, as described in the R15 research
notes.
- Each pair is two windows of equal length, each running the trace 8 times
  (one replay saves only a few joules, inside the noise of a shared machine):
  - a FULL replay;
  - an ENGINE replay followed by idle time until its window is as long as the
    FULL one.
- Saved energy is the FULL window minus the ENGINE window.
- There are 5 repetitions, the order alternates, and the median is reported.
- Overflow indicators are checked before and after every read. A backwards
  counter invalidates the window.

This measures the marginal energy of the work. It is not a wall-outlet
measurement and not a quiet-machine R15 qualification.

### Gate values

| value | when |
|---|---|
| `PASS` | All checks pass, energy was measured and valid, and the run is bound to a clean candidate commit (`OMEGA_CANDIDATE_COMMIT` equals HEAD and the tree is clean). |
| `PASS_UNBOUND` | Everything passes, but the run is not bound to a clean candidate commit. |
| `HOST_PASS_ENERGY_UNMEASURED` | Everything passes on a host with no energy counter, such as CI. |
| `FAIL` | Anything else. |

## Limits

- Functions must be pure over their declared inputs. The engine cannot see an
  undeclared read; the audit replay is how that would show.
- A fork copies the branch tables. This is the simple reference, not
  copy-on-write.
- The cache is bounded and never evicts. Once it is 7/8 full, new results are
  not filed; this is counted.
- There is one mutex and one process. The cache does not survive a restart.
- Cache reuse is not gated by capabilities. A result is visible to any branch
  of the engine that presents the same inputs.
- The derivations in the test are stand-ins with real CPU cost. They are not
  the AIEN model.
