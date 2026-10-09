# Choosing the next noisy measurement: BRW-ACT development results

Status 2026-10-09. DEVELOPMENT EVIDENCE ONLY. Nothing here qualifies EXP-003, discovery, or any TURING level.
Profiles: `BRW_ACT_DEV0_PROFILE.md` (sha256 7ef3ba42...), successor `BRW_ACT_DEV1_PROFILE.md` (sha256 85a3b565...).
Candidate: hand-coded reference (exact Bayesian update over given hypothesis classes, greedy expected information per
unit time). Not learned, not AI-discovered. Written without reading any evaluator-only material.

## 1. What already existed (code map, omega main 22f7fb7)

| piece | kind | what it does | what it does not do |
|---|---|---|---|
| `src/turing/ty_qcont.*`, `ty_prd.*` | reusable library | qint.v1 quantised scoring in bits, PRD1 Gaussian predictions | mixtures, choice |
| `src/turing/ty_prd2.*` | reusable library | PRD2 mixture predictions (K <= 8), validation, bits | choice of measurement |
| `tools/brownian/brw_tps_adapter.*` + `tests/brownian/test_brw_tps.c` | test machinery | turns Gaussian predictions into a binary stream for the TPS coder and checks it against real compression | choice, posterior |
| `tests/turing/test_ty_prd2.c`, `evidence/EXP-002D/` | test + receipts | row 28 mixture rule (R28-v2) and controls N1-N4 | sealed verdicts (evaluator side) |
| `src/physics0/planner/` + `tests/physics0/planner/pd0_plan_eff*.c` | library + dev experiments | proposes a schedule that makes deterministic rival relations disagree most; passive baseline | probabilities, noise models, uncertainty; marked "NOT an EXP-003 claim" |
| Brownian simulation, hidden worlds, hostile tests, dev runner | evaluator side (private) | generates and judges EXP-002 worlds | not visible to candidate sessions |

Observations entered Brownian tests only as evaluator-generated paths scored against frozen predictions. Model
complexity was charged through the L(M) rider of the description-length rules. No component kept a posterior over
competing noisy explanations or chose a measurement. Random numbers and Gaussian sampling existed in many places;
they are simulation, not probabilistic inference and not stochastic planning.

No runtime consumer: no file in omega `src/` outside `src/turing` calls the TURING scorer, and INTERPLANE main has no
TURING or Brownian call site (grep 2026-10-09). `src/estimation/est_types.h` only reuses a naming convention.

## 2. What was added

- `tools/brownian/brw_active.{c,h}`: world-agnostic candidate library: 528-hypothesis grid over three explanations,
  exact log-space update, mixture entropy, I(Y;M) and I(Y;H), the two-phase choice rule, prequential 99% adequacy
  check, verdicts INADEQUATE / M0 / M1 / M2 / UNDETERMINED, PRD2 reduction to K <= 8 (moment matched, validated by
  `ty_prd2_validate`), held-out scoring through the existing `ty_qcont2_bits`. Purity check: no world symbols.
- `tests/brownian/test_brw_active.c`: unit tests (plain and ASan/UBSan). `tests/brownian/brw_active_dev0.c`: harness-
  side runner with the world generator, four strategies, paired bootstrap, pass rules, O_EXCL receipts.
  `mk/brownian_active.mk`: `test-brownian-active`, `brownian-active-dev0`, `brownian-active-dev1-power`,
  `brownian-active-dev1`.
- Independent check of the information computation (scratch Monte Carlo, 200000 draws per wait time, at the prior):
  library and Monte Carlo agree within Monte Carlo error for every wait time.

## 3. Results (all four strategies use the same updater; only the choice of wait differs)

| run | worlds | outcome |
|---|---|---|
| DEV0 dev | 200 | FAIL P1: active vs every baseline interval crosses 0; P2-P5 pass. Design defects found (below) |
| DEV0 hold | 500, run once | PASS P1-P5. Active reaches the right model 7.5 / 7.5 / 7.3 time units sooner on average than random / fixed8 / cycle (horizon 400; 95% intervals about -12 to -3). Gain comes from mean-reversion worlds (15% resolved vs 0-5%); diffusion worlds unresolvable for all |
| DEV1 power | 80, cycle only | B = 2000 (cycle success 0.45 / 0.66 / 0.78 at 1000 / 2000 / 4000) |
| DEV1 dev1 | 200 | FAIL P1 narrowly: active vs random -90.8 [-184.1, +1.3]; vs fixed8 and cycle below 0; P2-P5 pass |
| DEV1 hold1 | 500, run once | PASS P1-P5. Active vs random -116 [-175, -58], vs cycle -160 [-222, -100], vs fixed8 -388 [-470, -310] time units (horizon 2000) |

DEV1 hold1 by class (success fraction at P >= 0.99 within 2000): mean reversion: active 0.61, random 0.44, cycle 0.45,
fixed8 0.00; drift: active 0.75, random 0.72, cycle 0.71, fixed8 0.67. No wrong discriminations by active. Controls:
pure noise 0/100 false structure claims, pure diffusion (nested null) 0/100, mismatch 0/100 confident verdicts.
Held-out prediction: bits per reading equal to random and cycle within +-0.004 (intervals cross 0), better than fixed8
by 0.008. Calibration: central 90% intervals held 0.895 of held-out readings, 50% intervals 0.497.
Compute: active used 4.7e10 quadrature evaluations over the 500 hold1 worlds, the baselines none; likelihood
evaluations are similar for all (4.3e7 to 5.3e7). The comparison is in measurement time only; compute is not
converted to time.

Provenance: the `omega_commit` values in the receipts (3ddaa6d, 58bd6a1, 596cc49, 8ca24dd, 4c408be) are the original branch history, kept reachable by tag `brw-act-dev-provenance-2026-10-09` (the branch was later rebased and will be squash-merged).

Replay: every run was executed twice; table digests match (DEV0 hold 588b87a0..., DEV1 hold1 3365c8e8...). Cross-machine
replay is not guaranteed (libm `exp`, `log`, `sqrt`, `cos`).

## 4. Design defects found and how they were handled

DEV0 set a 0.99 target that pure diffusion can never reach under nested grids (P(M0) bounded near 0.84) and a 400
budget too small for mean reversion. DEV0 was kept as written and its held-out run was still done once, as its profile
said. DEV1 changes only class roles, the budget (by a baseline-only power rule) and fresh world streams, and was
committed before DEV0's held-out output was read.

## 5. Limits

The hypothesis classes are given. The worlds are simple and mostly Gaussian. The advantage is concentrated in
mean-reversion worlds; on drift worlds active is level with random. Held-out prediction quality does not improve
over random or cycle at the end of the budget: the benefit is faster discrimination, not better final prediction.
Known reporting issues (display only, no result affected), as found at merge: the Wilson coverage intervals treated the 60 held-out readings of a world as independent, so they were too narrow (P3 uses the point estimate); in DEV1 output the P4 noise line printed the combined noise-and-nested-null verdict (both controls passed on hold1, so no printed verdict was wrong). Both fixed after merge, see section 7; the receipts above are unchanged.
These are development results on a self-chosen world family by the same session that wrote the profile; an
independent evaluator, sealing and EXP-003's contract are all still required.

## 6. Runtime decision

Not integrated. There is no consumer whose decision this would improve today (section 1). If one appears, the first
connection must be observational: TURING measures and does not authorise actions.

## 7. Follow-up 2026-10-09: report fixes and computation charged

Added after the results above were read; nothing here changes a verdict and no world was re-run.

- Report fixes (omega PR "BRW-ACT report fixes"): coverage intervals now use the world as the independent unit
  (`tests/brownian/brw_act_report.h`), and the P4 noise line prints the noise verdict alone. Regression test
  `test_brw_act_report` (in `make test-brownian-active`): 3 of 7 checks fail on the old code, 7 of 7 pass after.
  The per-world table is byte-identical (DEV0 regress digest 373dc7d7... unchanged).
- Re-analysis of the committed held-out tables (`make brownian-active-reanalyse`, reads `table.tsv` only):
  `evidence/BRW-ACT-DEV0/hold-reanalysis/` and `evidence/BRW-ACT-DEV1/hold1-reanalysis/`, each with source commit,
  input and tool digests; generated twice, identical. Source commit kept reachable by tag
  `brw-act-reanalysis-provenance-2026-10-09`.
- Corrected coverage, DEV1 hold1 active: 90% intervals held 0.8949 [0.8892, 0.9006] of held-out readings, 50% held
  0.4969 [0.4871, 0.5068]. Barely wider than the Wilson figures: the clustering was real but small.
- Computation charged. Extra evaluations (likelihood, quadrature and cdf counted alike) of active per discoverable
  world, against the time it saved, give a break-even exchange rate: active is ahead only while one time unit of
  measurement is worth more than this many evaluations.

| run | vs | extra evaluations per world | time saved per world [normal 95%] | break-even evaluations per time unit (at the low end of the saving) |
|---|---|---|---|---|
| DEV1 hold1 | random | 1.12e8 | 115.9 [55.9, 176.0] | 9.7e5 (2.0e6) |
| DEV1 hold1 | cycle | 1.12e8 | 160.0 [98.7, 221.3] | 7.0e5 (1.1e6) |
| DEV1 hold1 | fixed8 | 1.12e8 | 387.9 [308.1, 467.8] | 2.9e5 (3.6e5) |
| DEV0 hold | random | 3.9e7 | 7.5 [2.7, 12.3] | 5.2e6 (1.5e7) |

  Reading: when a measurement is slow or costly (a lab reading, a long benchmark, a GPU hold), a few million cheap
  arithmetic evaluations per time unit saved is a good trade; when measurements are as cheap as the arithmetic, the
  active chooser does not pay for itself. No wall-clock or energy figure was measured, so no speed or energy claim
  follows. Treating the three evaluation kinds as equal is a simplification (quadrature evaluations dominate).

