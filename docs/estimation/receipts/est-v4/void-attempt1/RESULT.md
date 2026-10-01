# ESTIMATION v4: result

**Verdict: PASS.** The selected family G1 (first-order lag plus quantization-aware
Student-t with an adaptive scale and a persistence-aware horizon law) is calibrated
on the sealed held-out run D2 and scores better than both baselines (F1 and E0).

Written by hand from `params.txt` and the receipt, as protocol section 8 step 6 says.

## Identity

| Item | Value |
|---|---|
| Protocol | `docs/estimation/protocols/est-v4.md`, sha256 `7baff66f1baf13f648b38ea04516b423e34e4c4b4d7528701f15272ca441f116`, frozen at `1a147e4` |
| D1 (fit) | `evidence/EST4/raw/20261001T084508Z-est4-fit-silicon/`, seed 0xE5C4D1, 2680 lines, precheck VALID (foreign mean 1.2555, above-3 share 0.0336) |
| Binding fit | tool `7ce0ec2`, clean tree; `params.txt` sha256 `0a2c58df4dbad5766b507367df958a05432b2db2f4a9455811ba75061bcb0a2b` |
| D2 (held out) | `evidence/EST4/raw/20261001T093559Z-est4-heldout-silicon/`, seed 0xD2E5C4, collected after params.txt was committed (`d6b76c3`), 2680 lines, precheck VALID (foreign mean 0.5929, above-3 share 0.0030) |
| Sealed run | tool `129dc6a`, clean tree, run once; receipt `receipt-acb6fc1ea4ced9350a4b72e1f53767c05bec776a2543ad595512da9f19329227.json` |

Both collections ran under the declared est_load schedule (section 4), checked
segment by segment against the protocol. No recollection was needed. No refit and no
second scored run were made.

## Phase A (D1 screen, in sample)

Every family passed every screen rule. Selection: best D1 one-step log score among
screen-passers, earlier family preferred within 0.01 nats.

| Family | Screen | First fail | One-step log score | Ten-step log score | Idle-regime cov95 | Ten-step cov95 |
|---|---|---|---|---|---|---|
| G1 lag | PASS | none | -2.31799 | -3.87562 | 0.9356 | 0.9455 |
| G2 AR change | PASS | none | -2.31772 | -3.87561 | 0.9361 | 0.9458 |
| G3 two-tank | PASS | none | -2.32886 | -3.85867 | 0.9326 | 0.9267 |
| F1 baseline | n/a | n/a | -3.43405 | n/a | n/a | n/a |

G2 beat G1 by 0.00027 nats, inside the 0.01-nat tie window, so G1 was selected.
G1 parameters: tau 0.5, q 31622.8, lambda 0.3, nu 1.5, c 0.7, phi 0.95, nu_h 1, c_h 0.3
(tau is on its grid edge, flagged in params.txt; no other edge).

## Held-out (D2, sealed, scored once)

| Rule | G1 value | Band | Pass |
|---|---|---|---|
| coverage 50 % | 0.5397 | 0.46-0.54 | yes |
| coverage 80 % | 0.8244 | 0.76-0.84 | yes |
| coverage 95 % | 0.9476 | 0.93-0.97 | yes |
| PIT deciles (min / max) | 0.0712 / 0.1182 | 0.07-0.13 each | yes |
| mean z | 0.0514 | -0.10-0.10 | yes |
| lag-1 z | 0.0559 | -0.20-0.20 | yes |
| quarter cov95 (Q1..Q4) | 0.9374, 0.9454, 0.9648, 0.9426 | 0.90-0.99 | yes |
| load-trial cov95 | 0.9537 (n 2092) | 0.90-0.99 | yes |
| idle cov95 | 0.9245 (n 558) | 0.90-0.99 | yes |
| ten-step cov95 | 0.9321 (n 2640) | 0.90-0.99 | yes |
| scored points | 2650, 0 unscorable | at least 2000 | yes |

| Predictor | Mean one-step log score (nats) | Calibrated |
|---|---|---|
| G1 | -2.0350 | yes |
| E0 (empirical change distribution) | -2.3978 | not required; first fail coverage50 |
| F1 (Gaussian Kalman) | -3.5025 | no; first fail coverage50 |

Same scored steps for all three (`same_steps: true`).

## What this does and does not show

- It shows one temperature estimator whose stated uncertainty matched reality on one
  sealed 45-minute run of this Spark under the declared load schedule, at one-step and
  ten-step horizons, after the fit was fixed in advance.
- Two margins are thin: coverage 50 % (0.5397 against a 0.54 ceiling) and the lowest
  PIT decile (0.0712 against a 0.07 floor). The bands were not changed; they pass.
- Scope is this machine, this sensor, this load pattern and these horizons. Other
  sensors, other machines or very different workloads need their own frozen check.
- The receipt writes `inf` and `nan` for unbounded bands and absent scores (same
  writer as v3), so it is not strict JSON. The content is complete; the file is
  content-addressed and is not rewritten.

## Downstream

Per protocol section 10, EST-4 (world belief state) and EST-5 (uncertainty-aware cost
model) are now unblocked for the G1 estimator in the scope above. Neither is started here.
