# EST-2 / EST-3 protocol v1: real-signal replay and calibration (pre-registered)

Workstream: ARCH-0020 (belief / estimation layer). This protocol is frozen by the
commit that adds it. The freeze commit hash is written into every EST-2/EST-3
receipt. Any change after that commit is a new protocol version, and the
result under v1 stays on record.

## 1. Signal and evidence

- Signal: CPU thermal zone 0, `thermal_mc` first field, milli-degrees Celsius,
  from `machine-state.ndjson` (1 Hz, `tools/r15_machine_state.sh`).
- Fit run (A): `evidence/R15/raw/20260929T020536Z-ad8e1f2ea4e4-silicon/machine-state.ndjson`.
- Held-out run (B): `evidence/R15/raw/20260929T025735Z-3e9e53be3358-silicon/machine-state.ndjson`.
- Before use, each file's SHA-256 must equal its line in that run's
  `raw/SHA256SUMS`; otherwise the replay refuses to start.
- Disclosure: both runs were recorded on 2026-09-29 during R15, before this
  workstream existed. As of the freeze no one has computed temperature
  statistics from them for estimation purposes (the orchestrator looked at the
  first line of run A to learn the field names). A read-only survey worker on
  2026-09-30 also reported descriptive facts: line counts 1953 / 2009, largest
  gap about 1.13 s, zone 0 range 35.4 to 53.8 C, readings in 100 mC steps,
  `spbm_uj` cumulative. No dynamics, residuals or noise levels were examined.
  They are pre-existing evidence that has not been examined for this purpose,
  not blind data in the CAL-0 sense.
- Load regime: a sample is "in trial" when its `t` lies inside a
  begin/end pair of that run's `machine-state-marks.txt`, otherwise "idle".

## 2. Replay rules (EST-2)

- One strictly causal forward pass per file, in file order. Each step uses only
  observations at or before its own time. Test: running on the first k lines
  must reproduce the first k outputs bit for bit, for k = 100, 500, 1000.
- Time. The raw `t` is parsed as integer seconds plus exactly nine fractional
  digits (no floating-point parse) and is used only to compute the gap to the
  previous sample. The gap is rounded to whole steps of 1 s, minimum 1
  (horizon = max(1, round(gap / 1 s))). The filter runs on a logical grid:
  prior t_ns = 0, prediction t_ns = belief t_ns + horizon * 1e9, and the
  observation record carries that same grid time. The wall-clock `t` stays in
  the raw line, which the observation's evidence digest binds.
- A line that fails to parse, or has a non-finite or missing value, is a
  missing observation. The filter coasts through it, and the replay counts and
  reports it. It is never dropped silently.
  Coasted steps contribute nothing to the fit likelihood or to any run B statistic.
- Every raw observation is preserved. The output stream records, for each
  step, the observation record, the prediction record and the innovation record,
  plus their digests. The raw input file is only read.
- Observation noise: the model's R (fitted, section 3) is the declared
  observation noise. The source digest is SHA-256 of the text
  `R15.machine-state.thermal_mc[0]`. The evidence digest is SHA-256 of the raw
  line bytes, without the newline.

## 3. Models and fitting (on run A only)

- M0, random walk: 1 state, temperature in mC. F = 1, H = 1,
  Q = q * dt, R = r.
- M1, constant velocity: 2 states, temperature in mC and rate in mC/s.
  F = [[1, dt], [0, 1]], H = [1, 0],
  Q = q * [[dt^3/3, dt^2/2], [dt^2/2, dt]], R = r.
- Prior: x0 = [first valid observation, 0], P0 = diag(r, 1e6 mC^2/s^2)
  (M0: P0 = r).
- Fit: maximise the Gaussian innovation log-likelihood on run A, excluding the
  first 30 steps, over a fixed log grid. q runs over 10^k for
  k = -3.0, -2.5, ..., 6.0 (19 values). r runs over 10^k for
  k = 0.0, 0.25, ..., 6.0 (25 values). Ties go to the smaller q, then the
  smaller r. The chosen (q, r) per model are written to a parameter file.
  The parameter file's digest goes into the receipt.
- The fitting program opens run A only. The evaluation program takes the
  parameter file and run B only.

## 4. Calibration statistics (EST-3, on run B)

Burn-in: the first 30 steps are excluded. Then, for each model:

1. One-step coverage of the central 50 %, 80 % and 95 % predicted intervals
   (|nu| <= z * sqrt(S), with z = 0.674490, 1.281552, 1.959964). Each is
   reported with a Wilson 95 % interval.
2. Mean NIS (for m = 1, the expected value is 1).
3. Standardized bias: mean(nu) / sd(nu).
4. Lag-1 autocorrelation of the normalized innovation nu / sqrt(S). The
   Ljung-Box Q statistic at lag 10 is reported but is not a gate.
5. Drift: 95 % coverage in each quarter of run B, by time.
6. Regime: 95 % coverage for in-trial and for idle samples separately.
7. Ten-step coverage: from each belief at step k, predict 10 steps ahead and
   compare with the observation at step k + 10 when it exists. Report 95 %
   coverage.
8. Accuracy: mean log predictive density (Gaussian log score) and RMSE of the
   one-step prediction. For comparison, the persistence baseline predicts the
   last observation and reports its RMSE.

## 5. Pass rules

A model is **calibrated** when all of the following hold on run B:

- One-step coverage lies within these bands:
  - 50 %: [0.46, 0.54]
  - 80 %: [0.76, 0.84]
  - 95 %: [0.93, 0.97]
- Mean NIS lies in [0.80, 1.25].
- The absolute standardized bias is at most 0.10.
- The absolute lag-1 autocorrelation of the normalized innovation is at most 0.20.
- Every quarter's 95 % coverage lies in [0.90, 0.99].
- Each regime with at least 100 samples has 95 % coverage in [0.90, 0.99].
- Ten-step 95 % coverage lies in [0.90, 0.99].

The **selected** estimator is the simplest calibrated model (M0 before M1)
whose one-step log score is within 0.01 nats per step of the best calibrated
model's.

`ESTIMATION_REAL_SIGNAL = PASS` when:

- both files verify against SHA256SUMS;
- the replay completes;
- the prefix-invariance test holds;
- raw observations are preserved;
- the prediction then observation then innovation chain is recorded for every
  step.

`ESTIMATION_CALIBRATION = PASS` when at least one model is calibrated and its
one-step RMSE is no worse than persistence's. Otherwise the result is FAIL.
The FAIL is recorded with the statistics that failed and the most likely
failure class: wrong process noise, wrong observation noise, nonlinear
dynamics, nonstationarity, regime switching, quantization, sensor bias or
model misspecification. No estimator is promoted after a FAIL.

## 5a. Anticipated failure (stated before any run)

Readings come in 100 mC steps. In quiet stretches this produces runs of
exactly-zero innovation, which can break the 50 % band and the lag-1 test when
the filter is confident. If v1 fails for that reason, the FAIL is recorded as
the quantization class and accepted. It is not a reason to widen bands or to
add a quantization-aware R under v1.

## 6. What this protocol does not allow

- Developing or debugging the replay, fit or evaluation programs on run B. They
  are built and debugged on run A and on synthetic data only; their commit hash
  goes in the receipt. The first execution against run B is the recorded one.
  Any second execution on run B is a new protocol version.

- Refitting q or r on run B.
- Changing the grid, thresholds, burn-in, models or intervals after seeing any
  run B statistic.
- Dropping outliers.
- Adding a model after the fact. A new model is protocol v2, and it is
  evaluated on fresh data (section 7).

## 7. Stronger follow-up (EST-3b, optional)

Collect a new 1 Hz thermal series after this freeze. Do this with a shell
reader of `/sys/class/thermal` only, never during a heavy run owned by another
session unless that regime is the declared target. Then evaluate the frozen
v1 parameters on it with the same rules.
