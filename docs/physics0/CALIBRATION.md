# PD-0 threshold calibration (G1, spec section 11 step 3)

**Spec:** aien-dev/physics `docs/PD0_HIDDEN_EQUATION_BENCHMARK.md` at `2f881b1` (revision 2; byte-identical to branch commit `80d7808`). Nothing in that file is edited here. Every number below was produced by `make physics0-test` on this commit (receipts `evidence/physics0/pd0-oracle-L<n>.json`, `evidence/physics0/pd0-g0-receipt.txt`); the generators are `src/physics0/pd0_gen.c`, the calibration code `tests/physics0/pd0_calib.c`, `pd0_oracle.c`, `pd0_sparse.c`.

**Scope (Direction 2, substrate):** this is the oracle solver and the reference sparse solver scored against the frozen section 6 bounds. No learner was scored, no ladder was run. Scoring here is the calibration code of a test binary, not the scorer component (another lane).

## 1. Method

- Instances: development seeds 1..5 per level (section 2.4), each with its own constants from the `"const"` stream.
- Data: a uniform random schedule from a `"gather"` stream of the seed (random reset in the reset box, one random channel value per step, episodes of `episode_max_steps`) until a budget is spent; only `status = OK` transitions are kept. `FIT` = episodes with `episode mod 5` in {0,1,2}, `SELECT` = 3, `HOLDOUT-0` = 4 (never read).
- Three relations per instance, all in PDLAW1 delta form with `dt` folded into the coefficients:
  - `oracle_exact`: the true map's coefficients (what a perfect learner would write).
  - `oracle_fit`: the spec's oracle solver, the true form with constants re-fitted by least squares on `FIT` (section 4.2 V1).
  - `sparse_ref`: the reference sparse solver (section 11 step 3): greedy forward selection over every monomial of degree <= 3 in the observed variables plus each channel linearly; term count chosen on `SELECT` by minimum `description_bits + n * log2(RSS/n)`.
- Held-out scoring exactly as section 6.1: 20 episodes of 20 steps from the `"score"` stream, 10 reset in [-2, 2], 10 in [-3, 3], noise-free initial state (rev 2), uniform random interventions, compared with the noise-free true trajectory; `NRMSE = max_j RMSE_j / std_j`. One-step NRMSE predicts each step from the true state. When the true trajectory leaves |v| <= 10 inside a scoring episode the remaining steps are dropped and the episode is counted in `truth_oob_episodes`.
- V3: breach rate = `OUT_OF_BOUNDS` episodes / episodes under the random schedule, seeds 1..20, full budget.

## 2. Results against the frozen bounds

Bounds (section 6.1 to 6.3): in-box 0.02 (L6 0.03), extrapolation 0.06 (L6 0.08), one-step 0.01 (L6 not scored), coefficients within 5 % (L5 10 %, L6 not scored), size <= S* + 2.

| Level | V1 oracle_exact | V1 oracle_fit | sparse_ref (5 seeds) | V3 worst breach (seeds 1..20) | V3 |
| --- | --- | --- | --- | --- | --- |
| L0 | PASS, NRMSE 0 everywhere | PASS, 0 | PASS, exact support, size 2 | **0.95** (0.93 to 0.95 on every seed) | **FAIL** |
| L1 | PASS, <= 6e-6 | PASS, <= 6e-6, coef 0 ppm | PASS, size 3, coef <= 20 ppm | 0.000 | PASS |
| L2 | PASS, <= 1.3e-5 | PASS, coef <= 20 ppm | PASS, size 4 (one seed 5), coef <= 20 ppm | 0.000 | PASS |
| L3 | PASS, <= 5e-6 | PASS, coef <= 35 ppm | PASS, size 8, exact support | 0.032 (one seed, 1 of 31 episodes) | PASS |
| L4 | PASS, <= 5e-6 | PASS, coef <= 35 ppm | PASS, size 4 | **0.57** (0.00 to 0.57; 14 of 20 seeds above 5 %) | **FAIL** |
| L5 | PASS | PASS: in-box 0.002 to 0.010, extrap 0.002 to 0.007, one-step 0.0002 to 0.0013, coef 0.9 % to 1.9 % | in-box 0.002 to 0.014, coef 0.6 % to 2.4 %, size 4 (one seed 5) | 0.000 | PASS |
| L6 | PASS: in-box <= 3e-6, extrap <= 3e-6 | NOT_RUN (latent estimation not in this cut) | latent-free: in-box 0.021, 0.028, 0.033, 0.035, 0.072 | 0.000 | PASS |

Mutant control: with the L1 spring sign flipped in the generator (`-DPD0_MUTANT_SIGN`) the oracle check reports `PHYSICS0_G1_V1_L1-mutant: FAIL`, so the check bites (`build/physics0/mutant.out`, copied into the G0 receipt).

## 3. Findings: bounds or levels that are unreachable, trivial or broken as frozen

1. **L0 is not runnable as specified (spec defect, V3 FAIL).** With `dt = 1.0`, reset box [-2, 2] on both variables and `s0' = s0 + s1`, a reset with |s1| near 2 leaves |s0| <= 10 within 4 to 5 ticks regardless of the pushes (the rev 2 push bound 0.1 does not help: the velocity is set by the reset, not by the pushes). Measured: 93 % to 95 % of 50-step episodes end `OUT_OF_BOUNDS` on every development seed, and 14 to 18 of the 20 scoring episodes are truncated. The oracle "passes" only because the integer map is exact on the few surviving steps. A learner cannot gather 100 usable records from 5 episodes of this level under the stated budget without choosing resets near zero velocity. Candidate fixes for the spec owner (not applied): a separate reset box for `s1` (for example [-0.2, 0.2]), a smaller `dt`, or a larger bound for L0. UNVERIFIED which the owner prefers.
2. **L4 is unstable under the random schedule (V3 FAIL).** Explicit Euler on `s1' = s1 + (-k s0 - b s0^3 + u) dt` with `k` up to 4, `b` up to 2 and `dt = 0.05` gains amplitude every tick near |s0| = 2 (effective stiffness `k + 3 b s0^2` up to 28); 14 of 20 seeds breach more than 5 % of episodes, the worst 57 %. A smaller `dt`, a smaller `b` range or a shorter episode would fix it. Rev 2 capped L1 for the same reason (review B1) but did not cap L4. The oracle passes on the surviving episodes.
3. **L6 validity check V2 fails on 1 of 5 development seeds.** The best latent-free model of the library (the reference sparse solver, 4 terms) reaches in-box NRMSE 0.0209 on seed 4, inside the L6 bound of 0.03. By section 4.2 V2 "failure means the hidden variable is not necessary and L6 is invalid". On the other four seeds it fails the bound (0.028 to 0.072), but the 6.3 condition "fail by at least a factor of 3" (>= 0.09) is met by none of the five. Both the `m*q/w` ranges and the 0.03 bound need the owner's attention; UNVERIFIED whether a longer scoring horizon would separate them (confidence: medium).
4. **The L5 one-step bound 0.01 is reachable** now that rev 2 scores against the noise-free truth from a noise-free initial state: the fitted oracle sits at 0.0002 to 0.0013. The earlier worry that 0.01 sits below the noise floor (~0.014) applied to a noisy comparison and no longer holds. The L5 constant tolerance of 10 % is loose: the fitted oracle is within 1.9 % and the sparse reference within 2.4 % on 8000 steps.
5. **The 5 % coefficient tolerance on L0 to L4 is wide for the oracle (<= 35 ppm) and for the sparse reference (<= 20 ppm).** Not a defect; a learner with less data will need it. Recorded so a later owner can tighten with evidence.
6. **Trivial by construction:** oracle_exact NRMSE on L0 is exactly 0 because the L0 map has no constants and no `mul` rounding. The L0 in-box and extrapolation bounds cannot discriminate anything on that level.
7. **Spec gaps found while implementing the records (amendments owed to the spec owner, implemented here as stated):**
   - `PDLAW1` `exceptions` and `experiments` are "lists" with no count type; this implementation prefixes each with a `u16` count.
   - `claim_text` is "length-prefixed bytes" with no width; `u16` here.
   - `confidence_ppm` example: `floor(1e6 * 35 / 36) = 972222` matches the illustrative JSON.
   - `description_bits` for the L1 example relation is 144 (3 terms, 2 vars + 1 channel).
   - `PD0REC1` of section 2.5 for `n_obs = 2` is 152 bytes; the `reserved` bytes are checked to be zero on decode.
   - Reset records: `vars_before = vars_after =` the (noisy, L5) reset observation; `OUT_OF_BOUNDS` records carry `vars_after = vars_before` (nothing revealed); `REFUSED_RANGE` steps carry `applied = 0`.
   - After a refused reset no episode is open; a `step` then returns `BUDGET_EXHAUSTED` (the spec names no status for "no episode open"). UNVERIFIED that this is the intended reading (confidence: medium).

## 4. Limits of this calibration

- Seeds 1..5 for V1, 1..20 for V3 (as the spec's development set). No scored-seed commitments were made.
- `oracle_fit` for L6 is NOT_RUN: fitting constants with a hidden state needs a latent estimator, not written in this cut. `oracle_exact` covers the "is the bound reachable by the true map" question for L6.
- The sparse reference solver is deterministic and has no tuning knobs beyond the spec's `description_bits` rule; it is a reference, not a learner.
- Nothing here ran a ladder, a planner, a negative control over the ladder or a learner. `PD0_CONTROLS_PASS` is not claimed.
- Floating point enters only in the calibration code (least squares, NRMSE); every world value, record byte and relation coefficient is an integer.
