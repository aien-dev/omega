# PD-0 learner (Physics-0 Discovery Engine, Direction 4)

The first genuine Physics-0 learner. It never sees the generators, the world
source, the oracle, the constants or the evidence files. It receives the
describe record and the PD0REC1 observation records of FIT and SELECT
episodes only (the harness withholds HOLDOUT, TRIAL and REP records, and the
learner refuses them if offered), and it emits evidence for the independent
ladder checker of `src/physics0/ladder`. It never declares a ladder state.

Spec: aien-dev/physics `docs/PD0_HIDDEN_EQUATION_BENCHMARK.md` rev 2
(2f881b1); world parameters follow rev 3 (omega #248, merged); the two rev 5
rules (requested-reset preregistration hash, no T2 self pairs) are
implemented in the checker and the learner ahead of the spec text.

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
(PD0DESC2 with per-variable reset bounds; PD0DESC1 is refused).

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

## Development results (seeds 1..5, rev 3 world, rev 5 checker rules)

Learner never saw HOLDOUT/TRIAL/REP. "state" is the checker's verdict;
"scorer" is `pd0_score` against the truth. The previous receipt (rev 2
world, DESC1) is kept beside the new one.

| level | state reached (seeds) | scorer | size / bound | bits | notes |
|---|---|---|---|---|---|
| L0 | PREDICTED 5/5 | PASS 5/5 (exact) | 2 / 4 | 96 | T6 "trial missing": the rev 3 L0 episode is 20 steps, the 20th record is EPISODE_END not OK, so no trial has 20 OK steps; see spec item 2 |
| L1 | PROVISIONAL_LAW 5/5 | PASS 5/5 | 3 / 5 | 144 | exact, one-step error 0 |
| L2 | PROVISIONAL_LAW 5/5 | PASS 5/5 | 4 / 6 | 192 | exact |
| L3 | PROVISIONAL_LAW 5/5 | PASS 5/5 | 8 / 10 | 576 | four variables, two channels, exact |
| L4 | PROVISIONAL_LAW 5/5 | PASS 5/5 (in-box 0.008 to 0.023 on the rev 3 box) | 4 / 6 | 192 | cubic term found |
| L5 | INTERVENED 1/5, CANDIDATE 4/5 (1 to 3 refutations) | PASS 5/5 (in-box 0.004 to 0.015, constants within 10%) | 4 / 6 | 192 | preregistration now matches (rev 5 rule); trials fail on in-trial NRMSE, see spec item 1 |
| L6 | CANDIDATE 5/5 (3 refutations each) | FAIL 5/5 (307 latent missing, 308 reference too good) | 5 to 6 / 9 | 240 to 288 | reported, not claimed: no latent mechanism; rev 3 constants make the hidden variable matter more (in-box 0.05 to 0.08) |
| null | OBSERVATION 5/5 | not scored | 2 / 6 | 96 | no correlation evidence once self pairs are excluded; no candidate admitted; NC-1 holds |

Numbers per instance: `evidence/physics0/learner/pd0-learner-dev-<commit>.txt`.

## Failure analysis

- **L5 (noise).** The relation is right (scorer PASS on all seeds, constants
  within 10%) and trials now match their preregistration. They fail T6 because
  the trial error is an NRMSE normalised by the within-trial spread of the
  observed trajectory: a quiet 20-step trial has a small spread, so 0.02
  observation noise alone exceeds the 0.05 bound. Same mechanism refutes
  REP episodes (code 266). Not a learner defect; see spec item 1.
- **L6 (hidden variable).** Candidates are latent-free fits; on 1 of 5
  seeds the ladder still replicates one (the checker has no latent
  requirement; the scorer does and rejects all five). On the other seeds the
  trials or batches exceed eps, which is the honest outcome for a wrong
  model class. `hidden_state_suspected` did not fire on L6 because the
  latent-free fit's one-step residuals are small (0.001 to 0.005); the flag
  needs a rollout-based test, left for the next cut.
- **L0.** Exact relation on every seed; blocked at T6 because a 20-step
  episode yields 19 OK records plus one EPISODE_END record (spec item 2).
- **Null world.** With self pairs excluded (rev 5) no correlation evidence
  exists and the ladder stays at OBSERVATION. The MDL ranking still puts
  "pull to the mean" above the empty relation, which is the correct
  predictor of an i.i.d. world; its one-step error is 1.0 sd, far above eps.

## Spec items that proved unimplementable or ambiguous as written

1. **Noisy-level trial error.** T6/T7 normalise the trial error by the
   within-trial spread of the observed trajectory. On L5 (sigma 0.02) any
   trial whose trajectory spread is below about 0.4 units fails the 0.05
   bound even for the true relation. The spec should normalise by the
   declared reset-box scale, or set the noisy eps relative to sigma. Spec
   owner decision needed; the learner claims nothing on L5.
2. **L0 episode length.** With rev 3 the L0 episode is exactly 20 steps and
   the world marks the 20th step EPISODE_END, so no episode has the 20 OK
   steps T5/T6/T7 require. Either the world returns OK on step 20 and
   refuses step 21, or the checker counts an EPISODE_END step as observed.
3. **L6 pass rule** (factor-3 reference margin) is still open with Drake;
   the learner reports L6 and claims nothing.
4. **Requested reset on noisy levels** (rev 5 rule as implemented): the
   ledger record does not carry the requested reset vector, so on noisy
   levels the checker binds the reset through the PD0EXP1 entry and the
   steps only; the trial error bound covers a mismatched reset. A reset
   record that carries the requested values would close this.

## Not implemented

Latent-variable fitting, local refinement in the planner (Direction 5),
multi-step consistency in candidate ranking, PD0DESC2 parsing beyond the
assumed per-variable layout.
