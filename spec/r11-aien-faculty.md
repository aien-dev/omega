# R11: AIEN as a resident cognitive faculty

ADR 0016 §44. Gate `R11_CONTINUOUS_COGNITION_PASS`.

## Rule

AIEN is not a service. Nothing calls AIEN and waits, and AIEN calls no other
faculty. The rule is the one R10 proved for Omega:

```
world state changes
      ↓
AIEN dependencies become ready
      ↓
AIEN reaction forms a belief, prediction, hypothesis or plan
      ↓
publishes it into the world
      ↓
Omega / authority / effects react to it, or do not
```

`rx_aien.o` is compiled on its own and the build fails if it has any
undefined `omega` symbol. AIEN reaches Omega only through objects, under
capabilities the native AIENOS authority checks.

## What runs

Code: `src/runtime/rx_aien.{c,h}`, plus `omega.reconsider` in
`src/runtime/rx_omega.c`. Test: `tests/runtime/rx_r11_aien.c`, `make test-r11`.

| Reaction | Wakes on | Publishes |
|---|---|---|
| `aien.observe` | Omega `demand` window, Omega `selection` | `belief` |
| `aien.predict` | `belief` interval count | `prediction` |
| `aien.explain` | `prediction` sequence or state | `hypothesis` |
| `aien.assess` | `goal` (human), `prediction` state | `assessment` |
| `aien.plan` | `hypothesis` state, `assessment` | `plan`, `memory`, `hypothesis` state |
| `omega.reconsider` | `plan`, Omega `selection` | Omega `search` (new epoch) |

What AIEN reasons about: Omega's selection is a claim that a realization
costs so much. AIEN checks that claim against what production pays.

- **Belief.** The cost of each interval is the difference of two demand
  snapshots, so it is exact. The first interval after a new record is
  discarded, and so is any interval served before production switched to the
  record. The next 4 intervals form the baseline.
- **Prediction.** "Production keeps paying the baseline, ±20%, under this
  record on this core class." Four hits in a row confirm it. One miss resets
  the hit streak but fails nothing (uncertainty). Two misses in a row fail it.
- **Hypothesis.** For a failed prediction, AIEN looks at what else it
  observed. If the core class changed since the prediction, the hypothesis is
  `CORE_CLASS`. Otherwise it is `DRIFT`. A placement change on its own is not
  evidence and produces nothing.
- **Plan.** One experiment per condition (core class, regime): re-search the
  regime where the work runs now. `memory` records the conditions AIEN has had
  explored. A hypothesis about an explored condition becomes `EXHAUSTED`, with
  no plan.
- **Test of the hypothesis.** The next prediction made under the
  hypothesized condition decides it. `CONFIRMED` makes the hypothesis
  `SUPPORTED`; `FAILED` makes it `UNSUPPORTED`.
- **Goal.** A human publishes a target cost for a regime. With no confirmed
  prediction the assessment is `UNKNOWN` and AIEN does nothing. `MET` does
  nothing. `UNMET` on an unexplored condition wakes one plan.
  `UNMET_EXPLORED` does not.
- **Omega's side.** `omega.reconsider` reads the plan with a read-only
  capability and decides for itself. It skips a regime it cannot realize. It
  waits while a search is in flight. Otherwise it opens a new search epoch,
  and the ordinary R10 chain (synthesize → verify → measure → select) runs on
  the cores the world runs on now. R10 alone does not register it.
- **Authority.** AIEN holds read-only references to `demand` and
  `selection`. An AIEN reaction that tries to write `selection` is blocked.
  Revoking one AIEN capability stops that reaction; the world carries on.

## The living run (DGX Spark)

1. The whole process, world workers included, is placed on the ten
   Cortex-A725 cores, and the placement is published.
2. Omega's own search runs. With a 10% margin, the reference stays the record
   on A725, because quad4 is only about 3% faster there.
3. AIEN confirms a prediction of about 6.7 µs per call.
4. The process is moved to the ten Cortex-X925 cores and the new placement is
   published. Nothing else is published, and nothing asks for a
   re-optimization.
5. Production gets faster. AIEN's prediction fails, and AIEN forms a
   `CORE_CLASS` hypothesis (A725 → X925) and publishes a plan.
6. Omega takes the plan up: search epoch 2 is measured on X925, and quad4 is
   selected.
7. Production picks the new record up. AIEN predicts again under X925,
   confirms it, and marks the hypothesis `SUPPORTED`.

A control world makes the same move without AIEN. Its search epoch stays 1,
and it stays on the reference.

Eight consecutive local runs before the candidate was bound, ns per call. The
bound receipt is `evidence/R11/6aa6b3b3…json` (candidate 381828f): 6597 →
4913 → 2802 ns (1.75×); control 5044 ns, search epoch 1.

| | A725, reference | X925, old record | X925, after AIEN's plan | Control X925 |
|---|---|---|---|---|
| range over 8 runs | 6617–6812 | 4872–4948 | 2778–2859 | 4927–5105 (3 runs) |
| speedup from AIEN's plan | | | 1.70–1.77× | none |

The receipt checks the causal chain crumb by crumb: selection 2 ←
`omega.reconsider` ← `aien.plan` ← hypothesis ← failed prediction ← demand
evidence and the placement observation. Production commits happen between
the plan and the new selection, so production never waited. `omega.watch`
committed once, so Omega did not re-search on its own.

## Not claimed / limits

- **Not a neural model.** The cognition is an explicit statistical model
  (interval means, a tolerance band, streaks). R11 claims the faculty's
  form. It does not claim the sophistication of what runs inside it.
- **One domain.** AIEN reasons only about the cost of Omega's matvec
  regimes. It does not reason about goals in general, language, or other
  faculties' objects.
- **Triggers covered:** new observation and evidence, failed prediction,
  uncertainty, resource change, and human input or goal change. The ADR's
  "new memory" and "unexpected result" (other than cost) are not triggers
  here.
- **Beliefs live only in memory.** They do not survive a restart and do not go
  through the R9 generation barrier.
- **Omega still keeps one selection object.** Moving back to A725 would make
  AIEN plan again (A725 was never explored by AIEN). A record per core class
  is still an R10 follow-up.
- **After `UNSUPPORTED`, AIEN does not form a fresh hypothesis** for that same
  failed prediction.
- **Placement is published by the test,** which stands in for the scheduler.
  AIEN does not sense cores itself.
- **Hosted CI** has no mix of core types. There, part A runs and the receipt
  records `HOST_PASS_LIVING_RUN_NOT_EXERCISED`. The gate is claimed only by a
  clean, candidate-bound run on the DGX Spark.
