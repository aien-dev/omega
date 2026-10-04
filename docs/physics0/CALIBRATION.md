# PD-0 threshold calibration (G1, spec section 11 step 3)

**Spec:** aien-dev/physics `docs/PD0_HIDDEN_EQUATION_BENCHMARK.md` at `2f881b1` (revision 2; byte-identical to branch commit `80d7808`). Nothing in that file is edited here. Every number below was produced by `make physics0-test` on this commit (receipts `evidence/physics0/pd0-oracle-L<n>.json`, `evidence/physics0/pd0-g0-receipt.txt`); the generators are `src/physics0/pd0_gen.c`, the calibration code `tests/physics0/pd0_calib.c`, `pd0_oracle.c`, `pd0_sparse.c`.

> **Revision 3 supersedes the numbers below.** Sections 1 to 4 and the L6 estimator note record the revision 2 runs and stay as history. The revision 3 numbers are in the last section ("Revision 3, substrate cut 3"); old receipts in `evidence/physics0/` are untouched.

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
| L6 | PASS: in-box <= 3e-6, extrap <= 3e-6 | PASS (latent estimator, see L6 detail below): in-box <= 3e-6, extrap <= 4e-6 | latent-free: in-box 0.021, 0.028, 0.033, 0.035, 0.072 | 0.000 | PASS |

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
- `oracle_fit` for L6 uses the latent estimator of substrate cut 2 (detail below). The h scale is not identifiable (spec 4.1), so it is fixed to q = 1 and only the product m*q is compared; the oracle also checks that the fitted k, w, m*q lie in the generator ranges, which is what catches a wrong latent sign.
- The sparse reference solver is deterministic and has no tuning knobs beyond the spec's `description_bits` rule; it is a reference, not a learner.
- Nothing here ran a ladder, a planner, a negative control over the ladder or a learner. `PD0_CONTROLS_PASS` is not claimed.
- Floating point enters only in the calibration code (least squares, NRMSE); every world value, record byte and relation coefficient is an integer.

## L6 latent estimator (substrate cut 2)

Method: the generator form is known to the oracle. With a = 1 - w*dt the latent is g' = a*g + dt*s0 (g = 0 at the first step of every episode, h = q*g), so (ds1 - u*dt) = (-k*dt)*s0 + (m*q*dt)*g is linear in (k, m*q) for each candidate w. The estimator solves that 2-parameter least squares over a w grid (0.2 to 4.0, step 0.05) and refines the best w by golden section. FIT and SELECT data only; thresholds untouched; scored by the section 6.1 rule (20 episodes of 20 steps, half in-box, half extrapolation, h = 0 at start).

| seed | in-box NRMSE | extrap NRMSE | k err (ppm) | w err (ppm) | m*q err (ppm) |
| --- | --- | --- | --- | --- | --- |
| 1 | 2e-6 | 2e-6 | 2 | 16 | 15 |
| 2 | 2e-6 | 1e-6 | 0 | 3 | 28 |
| 3 | 3e-6 | 4e-6 | 3 | 9 | 6 |
| 4 | 2e-6 | 2e-6 | 1 | 17 | 12 |
| 5 | 2e-6 | 2e-6 | 2 | 6 | 6 |

Bounds 0.03 in-box and 0.08 extrapolation: met by a wide margin; `oracle_fit` L6 PASS on 5 of 5 seeds. Mutant: with the sign of m*h flipped in the generator (`PD0_MUTANT_LATENT_SIGN`), `oracle_fit` FAILs on 5 of 5 seeds because the fitted m*q comes out negative (about -0.4 to -0.9, declared range 0.25 to 1). Note the refitted model still predicts the mutant world well; it is the declared-range check that fails, not the prediction bound. Numbers above come from evidence/physics0/pd0-oracle-L6-fit.json (cut 2; the cut 1 file pd0-oracle-L6.json is immutable and keeps oracle_fit NOT_RUN). Cut 2 G0 receipt: evidence/physics0/pd0-g0-receipt-cut2.txt.

## Revision 3, substrate cut 3

**Spec:** aien-dev/physics `docs/PD0_HIDDEN_EQUATION_BENCHMARK.md` at `5bd2b04` (revision 3). **Omega commit that produced every number and receipt below:** `9e5ea8b5b5c1afa7a28cbcb0b2c463e51c96b538` (branch `hive/pd0-rev3`, on main `d0ca8ce`, which already contains #243, #246 and the L6 latent estimator #247). Run with `make physics0-test` and `make physics0-evidence` on a clean tree. No threshold was changed; the L6 range check and the generator read one table (`pd0_gen_const_table` in `src/physics0/pd0_gen.c`).

New receipts (old ones untouched): `evidence/physics0/pd0-oracle-L0-rev3.json` to `pd0-oracle-L6-rev3.json` and `evidence/physics0/pd0-g0-receipt-cut3.txt`. Each G1 receipt now lists the reset box, the scoring boxes (in-box = reset box, extrapolation = 1.5x it, spec 6.1), the declared constant ranges and, per instance, the L6 redraw count.

### What the code does now (spec lines)

| Item | Value in code | Spec |
| --- | --- | --- |
| L0 episode length, budget | 20 steps, 3000 steps = 150 episodes | 2.2 table, 4 table, 4.1 L0 |
| L0 reset box | s0 [-2, 2], s1 [-0.2, 0.2]; push [-0.1, 0.1] | 4 (box notes), 4.1 L0 |
| L4 reset box | [-1, 1] both variables (k [1,4], b [0.5,2] unchanged) | 4 (box notes), 4.1 L4 |
| L6 ranges, draw order | k [3,5], w [0.5,1], m [1,2], q [1,2]; order k, w, m, q | 4.1 L6, 3 |
| L6 stability rule | while m*q/w > 0.7*k redraw m then q from the same stream; exact integer test `10*M*Q > 7*K*W` in micro-units; cap 1000 redraws, then the seed is invalid (`g.invalid`, `pd0_gen_init` returns -1, the oracle receipt records `"invalid": true`) | 4.1 L6 |
| Scoring boxes | in-box = reset box per variable, extrapolation = 1.5x (L0 s1 0.3, L4 1.5, others 3) | 6.1 last paragraph |
| Refused reset | status `REFUSED_RANGE`, costs one episode, no episode opened (a later step reports `BUDGET_EXHAUSTED`) | 2.3 |
| L6 `in_declared_ranges` | k, w and m*q (over [m_lo*q_lo, m_hi*q_hi] = [1, 4]) read from the same table the generator draws from | 4.1, task rule |

### Results (seeds 1..5 for V1/V2, seeds 1..20 for V3)

| Level | oracle_exact worst in-box / extrap | oracle_fit worst in-box / extrap | sparse_ref in-box range | V3 worst breach (seeds 1..20) | Verdict |
| --- | --- | --- | --- | --- | --- |
| L0 | 0 / 0 (truth leaves the box in at most 1 of 20 scoring episodes) | 0 / 0 | 0 | 0.0200 | V1 PASS, V3 PASS |
| L1 | 6e-6 / 5e-6 | 6e-6 / 5e-6 | 1e-6 to 1.7e-5 | 0.0000 | PASS |
| L2 | 5e-6 / 5e-6 | 1.3e-5 / 1.0e-5 | 3e-6 to 1.9e-5 | 0.0000 | PASS |
| L3 | 8e-6 / 5e-6 | 5e-6 / 4e-6 | 2e-6 to 5e-6 | 0.0323 | PASS |
| L4 | 1.2e-5 / 1.0e-5 | 1.3e-5 / 8e-6 | 6e-6 to 2.4e-5 | 0.0000 | PASS |
| L5 | 5e-6 / 5e-6 | 0.0104 / 0.0061 | 0.0018 to 0.0137 | 0.0000 | PASS |
| L6 (oracle_fit with the latent estimator) | 3e-6 / 4e-6 | 2e-6 / 3e-6 | latent-free, see below | 0.0000 | V1 PASS, V3 PASS |

V3 matches the table in spec section 4.2 (L0 0.020, L3 0.032, the rest 0).

### L6: V2 and the factor-3 condition (measured, not assumed)

Draws (micro, order k, w, m, q) and redraws: seed 1 (3368868, 905570, 1643841, 1169109) 0 redraws; seed 2 4128392, 885899, 1150045, 1388809 with 1; seed 3 3794478, 685972, 1193802, 1388242 with 1; seed 4 4403162, 522824, 1163776, 1276070 with 7; seed 5 3292448, 990681, 1016836, 1491901 with 3. No invalid seed.

| seed | latent-free in-box NRMSE (sparse_ref) | extrap | fails the 0.03 bound (V2) | >= 0.09 (factor 3, spec 6.3) |
| --- | --- | --- | --- | --- |
| 1 | 0.1422 | 0.0990 | yes | yes |
| 2 | 0.0740 | 0.0670 | yes | **no** |
| 3 | 0.1196 | 0.1001 | yes | yes |
| 4 | 0.0631 | 0.0671 | yes | **no** |
| 5 | 0.0941 | 0.1033 | yes | yes |

Result: V2 PASS on 5 of 5 (a latent-free model fails 0.03 everywhere). The factor-3 condition (>= 0.09) is **met on 3 of 5 seeds (1, 3, 5) and NOT met on seeds 2 and 4**, so the L6 additional condition of spec 6.3 FAILS as the thresholds stand and L6 cannot pass; nothing was changed to hide this. These values (range 0.063 to 0.142) differ slightly from the figures quoted in spec 6.3 (0.069 to 0.138); the pass/fail picture (3 of 5) is the same. UNVERIFIED why the quoted figures differ (the sweep ran with local override macros, this run uses the generator directly; confidence: medium that it is only a difference in how the sweep drew its ranges).

`oracle_fit` L6 PASS on 5 of 5, in declared ranges on all five; errors: k at most 2 ppm, w at most 16 ppm, m*q at most 11 ppm. Latent-sign mutant: `oracle_fit` FAILs on 5 of 5 (fitted m*q about -1.5 to -1.9, below the declared 1.0). Sign mutant (L1): caught.

### Limits

- The describe record (spec 2.2) carries one `reset_min`/`reset_max` pair. For L0 it holds the s0 box [-2, 2], so a learner is not told that s1 is limited to [-0.2, 0.2] and a reset with larger s1 is refused and charged (spec 2.3 "probing the bounds is not free"). The spec table does not say how to describe a per-variable box; the wire format was left unchanged. Spec owner: decide whether describe needs per-variable bounds.
- The scoring-box rule is implemented in `tests/physics0/pd0_calib.c` and written into the receipts. `src/physics0/score` (another lane) is not changed and must apply the same per-variable rule.
- The G0 receipt records the commit of the tree it ran on; this document and the receipts were added in the commit after `9e5ea8b`.
