# BRW-ACT-DEV0: choosing the next noisy measurement (development profile)

Status: DEVELOPMENT DEMONSTRATION PROFILE. This is NOT EXP-003 and cannot close it. EXP-003 needs the Wave 2
contract (not accepted, not frozen) and must seal after BRN-10 (see `TURING_SCIENTIFIC_QUALIFICATION_STATE.md`).
This profile was written from public material only: the EXP-003 statistics section of
`protocols/turing-laboratory-execution-protocol-v1-0.tex` (primary outcome observations-to-discrimination, frozen
passive baselines on matched worlds, paired bootstrap of cost-to-threshold, survival treatment of runs that never
reach threshold, failed runs kept). No evaluator-only material (aien-sealed, Wave 2 contract text, hidden worlds,
thresholds, seeds) was read by the session that wrote it. The world family below is invented for this profile.

Question: can a candidate that keeps probabilities over competing noisy explanations pick a measurement that
tells them apart faster than simple baselines, under the same measurement budget, while staying calibrated?

Candidate identity: a HAND-CODED REFERENCE (exact Bayesian update over a fixed hypothesis grid, greedy expected
information per unit time). It is not learned and it does not discover its hypothesis classes; they are given.
Its success shows the measurement-choice machinery works; it is not autonomous discovery.

## 1. World (harness side only)

A particle starts at x = 0 after each reset. One experiment: choose a wait time tau from the menu
`{1, 2, 4, 8, 16, 32}`, wait, read the position once. The instrument adds Gaussian noise with sd `sigma_m = 1`
(published instrument spec, known to the candidate). Each experiment costs `tau + 2` time units (the 2 is reset
and readout). Experiments are independent given the world.

World classes (parameters drawn per world, continuous, not on the candidate's grid; logU = log-uniform):

| class | role | truth of one reading y at wait tau |
|---|---|---|
| `diffusion` | discoverable | y ~ N(0, 2 D tau + 1), D ~ logU(0.05, 2) |
| `drift` | discoverable | y ~ N(v tau, 2 D tau + 1), D ~ logU(0.05, 2), v = +-logU(0.05, 0.5), sign fair |
| `ou` | discoverable | y ~ N(0, (D/theta)(1 - exp(-2 theta tau)) + 1), D ~ logU(0.05, 2), theta ~ logU(0.03, 0.5) |
| `noise` | control: pure noise | y ~ N(0, 1): the particle does not move |
| `mismatch` | control: outside every class | diffusion D ~ logU(0.05, 2) plus Poisson(0.05 tau) jumps, each N(0, 3^2) |

Hidden from the candidate: the class, the parameters, the generator, the held-out readings.
Visible to the candidate: the menu, the costs, sigma_m, its own readings, its remaining budget.

## 2. Candidate (library `tools/brownian/brw_active.{c,h}`, no world symbols)

Explanations: M0 diffusion (D), M1 drift-diffusion (D, v), M2 mean reversion (D, theta), each giving a Gaussian
reading at every tau with the formulas above. Grids: D log-spaced 16 values on [0.001, 10]; theta log-spaced 16
on [0.01, 1]; v 16 values `+-` log-spaced 8 on [0.02, 1]. 528 hypotheses. Prior: 1/3 per model, uniform over
that model's grid (more parameters spread the same mass thinner: the model-complexity charge is the Bayesian
marginal likelihood). The update is exact in log space with log-sum-exp normalisation.

Choice rule (greedy, information per unit time), computed only over affordable menu entries:
- while max_m P(m) < 0.99: maximise I(Y; M) / (tau + 2), the expected information about WHICH MODEL;
- after that: maximise I(Y; H) / (tau + 2), the expected information about the full hypothesis (sharpens
  predictions);
- ties go to the smaller tau; stop when nothing is affordable.
I(Y; .) = H(Y) - sum P(.) H(Y | .), mixture entropies by a fixed 512-point trapezoid rule on [lo, hi], where lo and
hi are the min and max of mu - 8 s and mu + 8 s over hypotheses of posterior weight above 1e-12.

Verdict after the budget, in this order:
1. INADEQUATE if the prequential check fails: count of the candidate's own readings that fell outside the central
   99% interval of the prediction it made BEFORE seeing them exceeds the smallest c with
   P(Binomial(n, 0.01) >= c) <= 0.001;
2. the model m if P(m) >= 0.99;
3. UNDETERMINED otherwise.

Prediction output: one PRD2 family-2 record per held-out reading (`docs/turing/PRD2_PREDICTION_FORMAT.md`). The
528-component posterior predictive is reduced to K <= 8: the 7 heaviest components keep their weight, mean and sd;
the rest are merged into one component with the same total weight, mean and variance (moment matching). Weights
are renormalised by construction (exact sum 1 within 1e-12) and validated by `ty_prd2_validate` before scoring.

## 3. Strategies (all use the identical candidate updater; only the choice differs)

| id | rule |
|---|---|
| `active` | the choice rule of section 2 |
| `random` | tau uniform over affordable menu entries, own stream |
| `fixed8` | tau = 8 always (geometric middle of the menu); if unaffordable, the largest affordable |
| `cycle` | 1, 2, 4, 8, 16, 32, 1, 2, ... skipping unaffordable entries |

Budget: 400 time units per run, identical for every strategy. Compute is not converted into time; it is reported
per strategy as likelihood evaluations and design-quadrature evaluations, so the extra cost of `active` is visible.

Matched worlds and common random numbers: the j-th reading of a run uses a substream that depends only on
(world, j), so every strategy faces the same world and the same noise draws in the same order; only tau differs.

## 4. Outcomes and analysis

Primary (discoverable classes pooled, held-out worlds): cost-to-correct-discrimination. The first cumulative cost at
which some model reaches P >= 0.99. If that model is the true class the run succeeds at that cost; if it is the
wrong class the run is a WRONG discrimination; if no model reaches 0.99 the run is censored. Wrong and censored
runs count as the full budget (restricted mean, horizon 400) and are reported separately. Statistic: per-world
paired difference active minus baseline, mean over worlds, paired bootstrap 95% percentile interval (10000
resamples, xorshift64* seed 0x0B5E55ED00000001).

Secondary: success, wrong and censored fractions; measurement count; eventual P(true class); held-out bits per
reading through `ty_qcont2_bits` (qint.v1, k = llround(y / 2^-20)), paired with the same bootstrap; held-out
calibration as central 50% and 90% interval coverage under the full (unreduced) posterior predictive with Wilson
95% intervals; compute counts. T gained: not measured here.

Held-out readings per world: 10 at each menu tau (60), from a substream no strategy reads.

## 5. Pass rules (fixed before any held-out run; failures are reported, never retuned)

| rule | criterion (held-out worlds) |
|---|---|
| P1 | on discoverable classes, the bootstrap 95% interval of (active - baseline) restricted mean cost lies entirely below 0, for each of `random`, `fixed8`, `cycle` |
| P2 | active wrong-discrimination fraction <= 0.05 and <= the largest baseline wrong fraction + 0.02 |
| P3 | active held-out coverage on discoverable classes: 90% interval in [0.85, 0.95], 50% interval in [0.45, 0.55] |
| P4 | pure noise control: active claims M1 or M2 in <= 5% of worlds |
| P5 | mismatch control: active gives a confident model verdict (not INADEQUATE or UNDETERMINED) in <= 20% of worlds |

DEV0 passes only if P1 to P5 all hold. Any later change to a rule, a constant or a grid makes a new profile id
(DEV1, ...); this profile and its result stay as written.

## 6. Data separation, provenance, replay

- Development worlds: stream tag `dev`, 40 per class (200). Used to find defects. Profile constants are not tuned on
  them; any change after seeing dev output is recorded as a deviation in the receipt.
- Held-out worlds: stream tag `hold`, 100 per class (500), run once after this file is committed. Its SHA-256 is
  written into the receipt.
- World seed: `splitmix64(tag_constant ^ (class << 32) ^ index)`; substreams for reading j and the held-out set are
  `splitmix64` of the world seed mixed with fixed tags. Generator: xorshift64* (Vigna), Box-Muller cosine branch,
  Poisson by inversion. These identities are recorded in the receipt.
- Receipt: omega commit, SHA-256 of this profile, every source file, the runner binary and the per-world result table,
  compiler version, flags (`-O2 -ffp-contract=off -fno-fast-math`), budgets, verdicts and every failure. The run is
  repeated and both table digests must match. Replay across machines is NOT guaranteed: results pass through libm
  `exp`, `log`, `sqrt`, `cos`, whose last bits may differ between C libraries; a seed alone is not a replay guarantee.

## 7. What this does not show

It does not qualify EXP-003, discovery, or any TURING level. The hypothesis classes are given, not found. The
world family is simple and Gaussian except for the mismatch control. It makes no claim about GPU speed, compiler
performance, energy or intelligence. TURING measures and does not authorise actions; nothing here is wired into a
runtime.
