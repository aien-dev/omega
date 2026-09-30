# EST-3 protocol v1: recorded result (2026-09-30)

One recorded execution on held-out run B (`20260929T025735Z-3e9e53be3358-silicon`), tool commit 44f591b, protocol f96dc97, parameters fitted on run A only (`params.txt`). No rerun. `receipt.json` and both step streams sit beside this file.

```text
ESTIMATION_REAL_SIGNAL   = PASS   (replay complete, prefix invariance, raw unchanged, chain recorded for all 2009 steps)
ESTIMATION_CALIBRATION   = FAIL   (no model calibrated, none selected, none promoted)
```

## What failed (M0 random walk, the simpler model)
- 50% interval covered 0.727 (band 0.46 to 0.54): far too wide in the middle.
- 95% interval covered 0.921 (band 0.93 to 0.97): slightly too narrow in the tails.
- Quarter 1 95% coverage 0.893 (band 0.90 to 0.99).
- One-step error 1166 mC against persistence (repeat last reading) 1162 mC: no gain over persistence.
- Passed: 80% coverage, bias (-0.0006), lag-1 autocorrelation (0.094), ten-step coverage (0.983).
- M1 constant velocity failed the same checks plus ten-step coverage (0.991, too wide), and its error was larger (1388 mC).

## Failure class (recorded by hand, as the protocol requires)
Non-Gaussian, heavy-tailed one-step changes: most steps move very little and a few move a lot, so one Gaussian noise level cannot be right in both the middle and the tails. This is a wrong noise shape (with possible switching between calm and bursty behaviour), not a wrong mean.
- Quantization looks irrelevant (section 5a): the standard deviation of a one-step change is about 1159 mC against a 100 mC reading step. (Only 2.8% of innovations were exactly zero, but raw one-step changes are exactly zero 33% of the time, so that figure alone is weak evidence.)
- Not a wrong mean: bias and lag-1 autocorrelation passed. Autocorrelation beyond lag 1 is strong, though (Ljung-Box Q10 = 151 for M0 and 350 for M1, 5% cutoff about 18), so the noise shape and its persistence are both off. An independent recheck (fresh worker, own C recompute from the saved streams) reproduced all statistics and found no blocker or major issues.
- The regime split gave no information: every sample lies inside a load trial (idle n = 0).
- The M0 fit sat at the upper edge of the frozen q grid (q = 1e6), also on run A. The grid is frozen under v1 and was not widened.

## What this does and does not mean
- Under v1 rules the estimator is not promoted and nothing downstream may use its output as calibrated state.
- The failure stays as evidence. Bands, grid and models were not changed after seeing this result.
- Any follow-up is a new protocol version on fresh data (EST-3b, protocol section 7), for example a heavy-tailed noise model (Student-t or a two-level mixture) or a rate-aware model, entering only with this named failure class, as ADR 0020 section 2.4 requires.
