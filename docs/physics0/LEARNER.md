# PD-0 learner (Physics-0 Discovery Engine, Direction 4)

The first genuine Physics-0 learner. It never sees the generators, the world
source, the oracle, the constants or the evidence files. It receives the
describe record and the PD0REC1 observation records of FIT and SELECT
episodes only (the harness withholds HOLDOUT, TRIAL and REP records, and the
learner refuses them if offered), and it emits evidence for the independent
ladder checker of `src/physics0/ladder`. It never declares a ladder state.

Spec: aien-dev/physics `docs/PD0_HIDDEN_EQUATION_BENCHMARK.md` rev 2
(2f881b1), with the rev 3 world parameters pending in omega #248.

## Layout

| path | role |
|---|---|
| `src/physics0/learner/pd0_learner.{h,c}` | the learner library (C11, no file I/O, no process calls) |
| `src/physics0/learner/pd0_learner_main.c` | `pd0-learner`: deterministic offline fit over a raw PD0REC1 stream |
| `tests/physics0/learner/pd0_harness.c` | development harness: drives `pd0-world`, tags the split, builds the PD0LEDG1 ledger, runs the checker and the scorer |
| `tests/physics0/learner/pd0_truth_main.c` | test-only truth process (the only binary here that links `pd0_gen.c`) |
| `tests/physics0/learner/test_pd0_learner.c` | unit tests |
| `mk/physics0-learner.mk` | `test-physics0-learner`, `physics0-learner-dev`, `physics0-learner-evidence`, `p0l-isolation` |

The substrate headers (`src/physics0/pd0_wire.h` family) and the verifier
headers (`src/physics0/ladder/pd0_fmt.h`) define the same names with
different limits, so one binary cannot include both. The learner and the
harness are built on the verifier side only; the world and the truth run as
separate processes over pipes. The harness reads the describe record itself
(PD0DESC1 today; PD0DESC2 with per-variable reset bounds when #248 lands,
the learner already takes per-variable bounds).

## Mechanisms

1. **Numerical prediction.** One-step change of every observed variable is
   regressed on a monomial vocabulary of total degree at most 3 over the
   observed variables and the intervention channels (constant included, 20
   monomials for two variables and one channel). Per target: forward
   selection with backward pruning under a description-length score, least
   squares by normal equations in doubles on unit values. Floats live only
   here; coefficients are rounded to micro-units and every evaluation after
   that uses the checker's integer `pd0_rel_step`.
2. **Symbolic sparse search and description length.** Score per equation =
   residual code length `n * log2(max(rmse_micro, 1))` plus the spec 6.2
   term bits `8 * (n_vars + n_channels) + 24` per term. The candidate set is
   the MDL-best relation, plus one alternative per target (its runner-up
   equation), re-ranked on SELECT by residual bits plus description bits.
   Candidates serialise as PDLAW1 with `description_bits` from the spec rule.
3. **Model comparison.** Up to 8 candidates, always including the explicit
   null (no change) and at least one rival. The harness registers the best as
   CANDIDATE, the next two as RIVAL, the null as NULL, and the falsifier names
   them all. A lag-1 residual autocorrelation per variable flags suspected
   hidden state (`hidden_state_suspected`).

Also in the library: the T2 correlation evidence (strongest pair over
visible OK step records with a seeded 2000-shuffle permutation test,
Bonferroni over all pairs), seeded exploration schedules, a planner stand-in
(Direction 5 owns the real one: among 256 seeded schedules inside the bounds
and not in the forbidden hash list, the one where the hypotheses disagree
most in units of pooled residual sd; `-1` below 3 sd), and `rollout` /
`onestep_nrmse` prediction interfaces. Everything is deterministic from the
seed; the determinism test compares bytes.

## Harness protocol (development, seeds 1..20 allowed, 1..5 run)

Episodes are 20 steps. Exploration tags per five episodes: FIT FIT FIT
SELECT HOLDOUT (60/20/20). After exploration: CORR, RELATION entries,
FALSIFIER (eps = the level's in-box bound, 5 trials), five PREREG + TRIAL
episodes, then three BATCH entries of ten REP episodes (planner and random
halves alternate). On a refutation the trial records become FIT and the
learner refits (at most three attempts). The checker runs after each stage;
the harness never proceeds to trials without a HYPOTHESIS state. The scorer
runs on 20 noise-free truth episodes (10 in the reset box, 10 at 1.5x).

## Development results (seeds 1..5, commit of this PR)

Learner never saw HOLDOUT/TRIAL/REP. "state" is the checker's verdict;
"scorer" is `pd0_score` against the truth.

| level | state reached (seeds) | scorer | size / bound | bits | notes |
|---|---|---|---|---|---|
| L0 | INTERVENED 5/5 | PASS 5/5 (exact) | 2 / 4 | 96 | T7 cannot run: rev 2 L0 world has 60 episodes of 50 steps, 18 to 31 OUT_OF_BOUNDS per run; rev 3 (#248) pending |
| L1 | REPLICATED (PROVISIONAL_LAW) 5/5 | PASS 5/5 | 3 / 5 | 144 | exact relation, one-step error 0 |
| L2 | REPLICATED 5/5 | PASS 5/5 | 4 / 6 | 192 | exact |
| L3 | REPLICATED 5/5 | PASS 5/5 | 8 / 10 | 576 | four variables, two channels, exact |
| L4 | REPLICATED 5/5 | PASS 5/5 | 4 / 6 | 192 | cubic term found, exact |
| L5 | HYPOTHESIS 5/5 | PASS 5/5 (in-box 0.004 to 0.018, constants within 10%) | 4 / 6 | 192 | T6 code 252 on every trial: see spec item 1 |
| L6 | REPLICATED 1/5, INTERVENED 2/5, CANDIDATE 2/5 | FAIL 5/5 (307 latent missing, 308 reference too good) | 4 to 6 / 9 | 192 to 288 | reported, not claimed: the learner has no latent-variable mechanism yet |
| null | CORRELATION 5/5 | not scored | 2 / 6 | 96 | no candidate admitted (T3 code 222, one-step error 1.0 sd); NC-1 holds |

Numbers per instance: `evidence/physics0/learner/pd0-learner-dev-<commit>.txt`.

## Failure analysis

- **L5 (noise).** The learner's relation is right (scorer PASS on all
  seeds) and the ladder admits it as HYPOTHESIS, but no preregistered trial
  is ever matched: the checker hashes the schedule from the reset record's
  observed `after` values, which on L5 carry observation noise, so the hash
  never equals the preregistered one. Not a learner defect; see spec item 1.
- **L6 (hidden variable).** Candidates are latent-free fits; on 1 of 5
  seeds the ladder still replicates one (the checker has no latent
  requirement; the scorer does and rejects all five). On the other seeds the
  trials or batches exceed eps, which is the honest outcome for a wrong
  model class. `hidden_state_suspected` did not fire on L6 because the
  latent-free fit's one-step residuals are small (0.001 to 0.005); the flag
  needs a rollout-based test, left for the next cut.
- **L0.** Blocked by the rev 2 world budget, not by the learner.
- **Null world.** The MDL ranking puts "pull to the mean" above the empty
  relation, which is the correct predictor of an i.i.d. world, and T2 finds
  regression to the mean as a significant self-pair. Neither reaches a
  candidate: one-step error is 1.0 sd, far above eps.

## Spec items that proved unimplementable or ambiguous as written

1. **L5 preregistration.** The T5/T6 schedule hash is computed from the
   reset record's `after` observation. With observation noise the observed
   reset differs from the requested one, so no honest learner can match a
   preregistered schedule on L5. The hash should bind the requested reset
   (the `before`/request values) or the world should report the requested
   reset in the reset record. Spec owner decision needed.
2. **L0 at rev 2** cannot complete T7 inside its budget; rev 3 (#248) is the
   fix and nothing in the learner depends on those values.
3. **L6 pass rule** (factor-3 reference margin) is still open with Drake;
   the learner reports L6 and claims nothing.
4. **T2 self-pairs.** The checker accepts `var_a == var_b`, which the null
   world satisfies through regression to the mean. Harmless here (T3 stops
   it) but worth a spec note.

## Not implemented

Latent-variable fitting, local refinement in the planner (Direction 5),
multi-step consistency in candidate ranking, PD0DESC2 parsing beyond the
assumed per-variable layout.
