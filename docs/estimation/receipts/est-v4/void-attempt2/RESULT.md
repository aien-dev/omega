# ESTIMATION v4: result

**Verdict: PASS.** The selected family G1 (first-order lag model with a
quantization-aware Student-t predictive, adaptive scale and a persistence-aware
horizon law) is calibrated on the sealed held-out run D2, at one step and at ten
steps, and scores better than both baselines (F1 and E0).

Written by hand from `params.txt` and the receipt (protocol section 8 step 6). This is
**attempt 2**. Attempt 1 is void because another lane's heavy work overlapped both
of its collections; it is kept unchanged in `void-attempt1/` with `VOID.md`. Attempt 1
had also passed, so voiding it did not make the verdict easier to reach.

## Identity

| Item | Value |
|---|---|
| Protocol | `docs/estimation/protocols/est-v4.md`, sha256 `7baff66f1baf13f648b38ea04516b423e34e4c4b4d7528701f15272ca441f116`, frozen at `1a147e4`; unchanged |
| D1 (fit) | `evidence/EST4/raw/20261001T103123Z-est4-fit-silicon/`, recollection seed 0xE5C4D2 (section 7), 38 segments, 2681 lines, precheck VALID (foreign mean 0.5084, above-3 share 0) |
| Binding fit | tool `8469f7f`, clean tree; `params.txt` sha256 `c5899de7b4ca045ed27faa1138f45a92ca480ef44ebe678033966c4bb63b80b6` (committed `afe3567`) |
| D2 (held out) | `evidence/EST4/raw/20261001T112813Z-est4-heldout-silicon/`, recollection seed 0xD2E5C5, 39 segments, collected after params.txt was committed, 2681 lines, precheck VALID (foreign mean 0.3737, above-3 share 0) |
| Sealed run | tool `4fcb5e9`, clean tree, run once; receipt `receipt-20da3468eff2eac21dc1d9eb2ecca1505ad1961315579f24ffdec488609bc2d9.json` |

Validity of the attempt-2 collections was judged before any fit or score: section 7
pre-check VALID, plus a process watcher (top every ~5 s) that saw at most one foreign
process above 25 % CPU at any moment (single-core tests, short single compiles) and no
multi-core foreign suite. No refit and no second scored run on attempt-2 data.

## Phase A (D1 screen, in sample)

| Family | Screen | First fail | One-step log score | Ten-step log score | cov95 | Idle cov95 | Ten-step cov95 |
|---|---|---|---|---|---|---|---|
| G1 lag | PASS | none | -1.95967 | -4.02398 | 0.9514 | 0.9366 | 0.9460 |
| G2 AR change | PASS | none | -1.96012 | -4.02071 | 0.9464 | 0.9304 | 0.9462 |
| G3 two-tank | PASS | none | -1.97353 | -3.94801 | 0.9543 | 0.9424 | 0.9293 |
| F1 baseline | n/a | n/a | -3.52528 | n/a | n/a | n/a | n/a |

G1 has the best one-step log score and is selected. G1 parameters: tau 1, q 31622.8,
lambda 0.2, nu 1.25, c 0.6, phi 0.95, nu_h 0.8, c_h 0.2 (no parameter on a grid
edge). G3 sits on its tau and q grid edges (flagged in params.txt); it was not selected.

## Held-out (D2, sealed, scored once)

| Rule | G1 value | Band | Pass |
|---|---|---|---|
| coverage 50 % | 0.5262 | 0.46-0.54 | yes |
| coverage 80 % | 0.8220 | 0.76-0.84 | yes |
| coverage 95 % | 0.9570 | 0.93-0.97 | yes |
| PIT deciles (min / max) | 0.0747 / 0.1156 | 0.07-0.13 each | yes |
| mean z | 0.0441 | -0.10-0.10 | yes |
| lag-1 z | 0.0148 | -0.20-0.20 | yes |
| quarter cov95 (Q1..Q4) | 0.9488, 0.9705, 0.9525, 0.9564 | 0.90-0.99 | yes |
| load-trial cov95 | 0.9643 (n 1992) | 0.90-0.99 | yes |
| idle cov95 | 0.9352 (n 659) | 0.90-0.99 | yes |
| ten-step cov95 | 0.9534 (n 2641) | 0.90-0.99 | yes |
| scored points | 2651, 0 unscorable | at least 2000 | yes |

| Predictor | Mean one-step log score (nats) | Calibrated |
|---|---|---|
| G1 | -1.6590 | yes |
| E0 (empirical change distribution) | -2.0776 | no; first fail coverage50 |
| F1 (Gaussian Kalman) | -3.3221 | no; first fail coverage50 |

Same scored steps for all three (`same_steps: true`). G1's 80 % interval is 761 mC wide
on average (median 200 mC, two sensor steps).

## What this does and does not show

- It shows one temperature estimator whose stated uncertainty matched reality on one
  sealed 45-minute run of this Spark under a declared load schedule, at one-step and
  ten-step horizons, with every choice fixed before the held-out data existed.
- The v3 failure (ten-step cov95 0.809) is fixed: the persistence-aware horizon law
  gives 0.9534 held out. The v4 development risk (idle-regime coverage, 0.892-0.896 on
  v3 data) did not occur here (0.9352 held out).
- Scope is this machine, this sensor, this load pattern and these horizons. Other
  sensors, other machines or very different workloads need their own frozen check.
- The receipt writes `inf` and `nan` for unbounded bands and absent scores (same
  writer as v3), so it is not strict JSON. It is content-addressed and not rewritten.

## Downstream

Per protocol section 10, EST-4 (world belief state) and EST-5 (uncertainty-aware cost
model) are unblocked, using G1 with the frozen parameters above, in the scope above.
Neither is started here.
