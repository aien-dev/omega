# PD-0 learner (Physics-0 Discovery Engine, Direction 4)

The first genuine Physics-0 learner. It never sees the generators, the world
source, the oracle, the constants or the evidence files. It receives the
describe record and the PD0REC1 observation records of FIT and SELECT
episodes only (the harness withholds HOLDOUT, TRIAL and REP records, and the
learner refuses them if offered), and it emits evidence for the independent
ladder checker of `src/physics0/ladder`. It never declares a ladder state.

Spec: aien-dev/physics `docs/PD0_HIDDEN_EQUATION_BENCHMARK.md` rev 2
(2f881b1); world parameters follow rev 3 (omega #248, merged); the rev 5 rules
(requested-reset preregistration hash, no T2 self pairs) and the rev 6 rules
(rollout error over the pooled FIT sd, EPISODE_END records count as
observations) and the rev 7 rule (a TRIAL or REP episode ended OUT_OF_BOUNDS
before the horizon is void, replaced from the same stream, never in p or f;
a fourth void in a batch voids the batch) are implemented in the checker
and the harness.

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

## Development results (seeds 1..5, rev 3 world, rev 7 checker rules)

Learner never saw HOLDOUT/TRIAL/REP. "state" is the checker's verdict;
"scorer" is `pd0_score` against the truth. Earlier receipts (rev 2 world;
rev 5 rules) are kept beside the new one.

| level | state reached (seeds) | scorer | size / bound | bits | notes |
|---|---|---|---|---|---|
| L0 | PROVISIONAL_LAW 5/5 | PASS 5/5 (exact) | 2 / 4 | 96 | seed 5: one replication episode left the box, void and replaced (rev 7) |
| L1 | PROVISIONAL_LAW 5/5 | PASS 5/5 | 3 / 5 | 144 | exact, one-step error 0 |
| L2 | PROVISIONAL_LAW 5/5 | PASS 5/5 | 4 / 6 | 192 | exact |
| L3 | PROVISIONAL_LAW 5/5 | PASS 5/5 | 8 / 10 | 576 | four variables, two channels, exact |
| L4 | PROVISIONAL_LAW 5/5 | PASS 5/5 (in-box 0.008 to 0.023) | 4 / 6 | 192 | cubic term found |
| L5 | PROVISIONAL_LAW 5/5 | PASS 5/5 (in-box 0.004 to 0.018, constants within 10%) | 4 / 6 | 192 | no refutations once the trial error is pooled-sd normalised |
| L6 | CANDIDATE 5/5 (3 refutations each) | FAIL 5/5 (307 latent missing, 308 reference too good) | 5 to 6 / 9 | 240 to 288 | reported, not claimed: no latent mechanism |
| null | OBSERVATION 5/5 | not scored | 2 / 6 | 96 | no correlation evidence, no candidate; NC-1 holds |

Numbers per instance: `evidence/physics0/learner/pd0-learner-dev-<commit>.txt`.

## Failure analysis

- **L5 (noise).** Reaches PROVISIONAL_LAW on every seed with the rev 6
  normalisation; the rev 5 failure (quiet trials judged against their own
  spread) is gone.
- **L6 (hidden variable).** Candidates are latent-free fits; on 1 of 5
  seeds the ladder still replicates one (the checker has no latent
  requirement; the scorer does and rejects all five). On the other seeds the
  trials or batches exceed eps, which is the honest outcome for a wrong
  model class. `hidden_state_suspected` did not fire on L6 because the
  latent-free fit's one-step residuals are small (0.001 to 0.005); the flag
  needs a rollout-based test, left for the next cut.
- **L0.** PROVISIONAL_LAW on four seeds. Seed 5 lost one replication
  episode to an out-of-bounds step from a random schedule, leaving that batch
  at nine episodes; the harness does not top batches up (next cut).
- **Null world.** With self pairs excluded (rev 5) no correlation evidence
  exists and the ladder stays at OBSERVATION. The MDL ranking still puts
  "pull to the mean" above the empty relation, which is the correct
  predictor of an i.i.d. world; its one-step error is 1.0 sd, far above eps.

## Spec items that proved unimplementable or ambiguous as written

1. **L6 pass rule** (factor-3 reference margin) is still open with Drake;
   the learner reports L6 and claims nothing.
2. **Requested reset on noisy levels** (rev 5 rule as implemented): the
   ledger record does not carry the requested reset vector, so on noisy
   levels the checker binds the reset through the PD0EXP1 entry and the
   steps only; the trial error bound covers a mismatched reset. A reset
   record that carries the requested values would close this.
Resolved by rev 5, rev 6 and rev 7: noisy-reset preregistration hash, T2
self pairs, within-trial error normalisation, EPISODE_END outcomes, void
out-of-bounds trial and replication episodes (replaced from the same
stream; at most three voids per batch, a fourth voids and redraws the
batch; voids never count toward p or f).

## Not implemented

Latent-variable fitting, local refinement in the planner (Direction 5),
multi-step consistency in candidate ranking, PD0DESC2 parsing beyond the
assumed per-variable layout.

## Running against an external world (PD-0b harness)

The harness has an external-world mode: `pd0-harness <world> - <level|null> <seed> <out-dir>`
takes play AND truth (relation shape, noise-free scoring trajectories) from the one world
binary over protocol v2; no `pd0-truth` process. Protocol: `docs/physics0/PD0_PROTOCOL_V2.md`.

- `make physics0-pd0b-run PD0_WORLD_BIN=<world> PD0B_SEEDS=<file>`: seeds file has lines
  `level_index seed` (0..6 or `null`), exactly five per level listed. Runs the real ladder
  checker and scorer, writes one new receipt `evidence/physics0/pd0b/PD0B_RUN-<commit>-<world sha12>.txt`
  and refuses to overwrite. A dirty worktree is refused. Revision 2: the harness sends `final` before it asks for shape or score; the receipt records the final hash and the world's refusal counters per instance and says VALID or INVALID.
- `make physics0-pd0b-freeze`: prints the git tree hash of the four frozen directories, the
  sha256 of every file under them and of the harness binary (print only).

Runs with the omega `pd0-world` are stand-in verification, not PD-0b results.
