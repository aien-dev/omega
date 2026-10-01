# EST-3c protocol v3 result: Phase A FAIL (no family passed the in-sample screen; D2 not collected)

`ESTIMATION_CALIBRATION (v3) = FAIL` at Phase A. Per protocol section 5, when
no family passes the in-sample screen on D1, D2 is not collected and nothing is
scored on held-out data. The sealed run is NOT_RUN. EST-4 and EST-5 stay
blocked (section 10): no estimator output may be treated as calibrated.

Binding record: `params.txt` in this folder (sha256
6be9c829e10db259a8f87c888b0dfa68d9ab72a45e62357a1e0af029ab174026), written by
`est3c_fit` built clean at tool commit 93b3579 (freeze commit 4b079ed, protocol
`EST3C_PROTOCOL_V3.md` sha256 2f639332...6eade), on D1
`evidence/EST3C/raw/20261001T025159Z-est3c-fit-silicon/` (ndjson 9d5473b5...dc230,
marks 4dfd06ec...d7a72; 2681 lines, 35 load trials, 0 gaps above 1.5 s,
foreign busy cores mean 0.81). Rerun: `make est3c-fit` on a clean checkout of
that commit, then the command in section 8 step 2 (refuses to overwrite).

## In-sample screen (D1, 2651 scored one-step steps)

| family | fitted parameters | D1 log score (nats/step) | first failing rule | all failing rules |
|---|---|---|---|---|
| F1 Gaussian KF | q_proc 5.6e4, r 100 (grid floor) | -2.817 | cov50 0.760 | cov50, cov80, 9 PIT bins, lag1 z 0.437, idle cov95 0.710, ten-step 0.866 |
| F2 Huber KF | q_proc 5.6e5, r 100, c 3 | -3.446 | cov50 0.938 | cov50, cov80, 9 PIT bins, lag1 z 0.725, idle cov95 0.819 |
| F3 Student-t | nu 1.5 (grid floor), s 63 mC | -1.927 | cov50 0.561 | cov50/80/95, PIT bins 1, 8, 9, lag1 z 0.284, Q4, idle cov95 0.641, ten-step 0.883 |
| F4 scale mixture | w .483/.465/.052, sd 29/214/2390 mC | -1.868 | lag1 z 0.280 | lag1 z, idle cov95 0.747 |
| F5 adaptive t | lambda 0.5 (grid edge), nu 2, c 0.6, floor 100^2/12 | -1.629 | ten-step cov95 0.809 | ten-step cov95 only |

F5 met every one-step rule (cov 0.506 / 0.797 / 0.943, all ten PIT bins,
bias, lag-1 z 0.128, all quarters, both regimes) and failed only the ten-step
95 % coverage (0.809 against [0.90, 0.99]). It is still a FAIL; the protocol
has no partial pass.

## What failed and why (descriptive, D1 only, after the binding fit)

- The failure class of the best-scoring family (F5) is **dependence
  (persistence of changes)**. On D1 the variance of 10-step changes is 2.14
  times ten one-step variances (VR(10) = 2.137, computed descriptively from D1
  after the binding fit; not in params.txt). Every v3 candidate forecasts the
  10-step change as a sum of independent one-step changes (F3-F5 by
  convolution, F1/F2 as a random walk). F5, the only family to meet every
  one-step rule, was too narrow at 10 steps (0.809). F2 (0.922) and F4 (0.918)
  passed the ten-step rule, but only with one-step predictives that failed
  other rules (F2 far too wide at the centre, cov50 0.938; F4 lag-1 z and idle
  regime), so they do not show that the independence assumption holds.
- Observation, not a pass: with discrete predictives and fractional
  coverage, F4 and F5 met the one-step coverage bands on data with 54 %
  exact-zero one-step changes (also a descriptive D1 count), which was v2's
  blocker. v3 as a whole still FAILED.
- The fixed-scale families (F3, F4) failed the idle regime (235 steps) or
  lag-1 z; the adaptive-scale F5 met both.
- Grid edges were hit (F1/F2 r = 100, F3 nu = 1.5, F5 lambda = 0.5). This is
  recorded, not acted on: changing a grid now would be fitting to D1 after it
  was read, which section 9 forbids.

## What this does not say

Nothing about held-out calibration (never run), GPU-trial load, or other
workloads. No rescoring, refit or rerun of v3 is allowed; a successor must be
a new protocol version with a fresh fit set and a fresh sealed set. A v4
would need a model of change persistence (for example an AR term on changes or
a load-aware level model with the schedule as an exogenous input, which v3
explicitly excluded) and must keep the ten-step rule.

Records preserved: v1 (`../est23-v1/`, FAIL), v2 (`../est23-v2/`, C1 pre-check FAIL), v3 (this folder, Phase A FAIL).
