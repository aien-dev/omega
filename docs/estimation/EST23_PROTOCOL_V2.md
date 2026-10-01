# EST-3b protocol v2: heavy-tailed predictive (M2) on fresh data (pre-registered)

Workstream: ARCH-0020 (belief / estimation layer), M20 estimation repair.
This protocol is frozen by the commit that adds it. Its SHA-256 is compiled
into the evaluator (`EST_V2_PROTOCOL_DOC_SHA`) and written into every v2
receipt. Protocol v1 (`EST23_PROTOCOL_V1.md`, freeze f96dc97) and its recorded
FAIL (`receipts/est23-v1/`) stay on record unchanged. Run B is never read again.

## 1. Why v2 exists

v1 failed calibration on run B (see `receipts/est23-v1/RESULT.md`): one-step
temperature changes are heavy tailed, so a single Gaussian noise level is
wrong in the middle (50 % coverage 0.727) and in the tails (95 % coverage
0.921). The recorded failure class is model misspecification (noise shape).
v1 section 6 says a new model is protocol v2 on fresh data. That is this
document.

## 2. Model added: M2, persistence mean with a scale-mixture predictive

- State and mean: the M0 structure (F = 1, H = 1, P0 = r) with
  r = 1e-12 mC^2 and q = the total variance of the mixture below. With
  r this small the filter's mean is the last observation (exact persistence),
  so M2's one-step RMSE equals persistence's RMSE exactly. The evaluator checks
  this (`m2_mean_is_exact_persistence`) and fails ESTIMATION_REAL_SIGNAL if it
  does not hold.
- Predictive shape: the one-step change is a zero-mean Gaussian scale mixture
  with k = 3 components (weights w_c, variances v_c). An h-step change is the
  sum of h independent one-step changes. Central 50 / 80 / 95 % intervals are
  the exact quantiles of |sum|; the log score is the mixture log density.
- Fit: EM on the horizon-1, non-coasted one-step changes of fit run C1 at
  L >= 30, k = 3, variance floor 100^2 / 12 mC^2 (one reading step, uniform
  quantization), 1000 iterations, deterministic initialization. Tool:
  `est_fit --m2`, which opens C1 only and checks C1's SHA-256 against the
  compiled constant.
- M0 and M1 are not refitted. The frozen v1 parameter file
  (`receipts/est23-v1/params.txt`, fitted on run A) is evaluated on C2 in the
  same receipt, unchanged.

## 3. Why M2 is not fitted on run A

Stated reasons (no numerical diagnosis on A is claimed):

1. v1 section 6 sends any new model to fresh data.
2. Runs A and B are loaded-regime R15 data (every run B sample was inside a
   load trial). v2's declared regime is idle (section 4). A fit on A would be
   tested out of regime.
3. A has already been used to fit v1, and the v1 M0 fit sat at the q grid
   ceiling (1e6) there, so A is not a neutral fit set for a noise-shape model.

## 4. Data and declared regime

- Collector: `tools/estimation/est3b_collect.sh` (1 Hz shell reader of
  `/sys/class/thermal`, no root, no perf). Signal: zone 0, `thermal_mc` first
  field, mC, same line shape and replay rules as v1 sections 1 and 2.
- Declared target regime: **idle**. This worker has no load source of its own,
  heavy tests run one at a time across the program and belong to other
  sessions, and no load generator is started for this protocol. The marks file
  is empty, so every sample is classed idle. A PASS here says nothing about the
  loaded regime where v1 failed.
- Regime evidence: `/proc/loadavg` was logged every 10 s during collection
  (`load.txt` beside the raw run; not part of SHA256SUMS). Summary: see
  section 8.
- Fit run C1: `evidence/EST3B/raw/20261001T020412Z-est3b-fit-silicon/machine-state.ndjson` (2090 lines, 2026-10-01 02:04:12Z to 02:39:12Z), SHA-256 `65252bae5f9d49d30a3b334fd2b9444c36db7627a45fcd9b6ef0bb4e5a7e5716`.
- Held-out run C2: collected only if the C1 pre-check (section 5) passes.
- An earlier C1 attempt (20261001T012838Z) was discarded unread because
  `/dev/null` was deleted mid-run by another process and thermal fields went
  empty. It is not used.

### Pilot disclosure (before freeze)

A separate 180 s pilot (role `smoke`, 2026-10-01 02:06:43Z, /tmp, deleted
afterwards, not C1 or C2) gave, on zone 0 one-step changes: n = 178,
exact-zero fraction 0.826, standard deviation 221.6 mC. No other statistic
was computed. C1 was not read before this document was committed.

## 5. Order of operations and pre-check

1. Commit the raw C1 run (and C2 if collected).
2. Commit this document; compile its SHA and the data SHAs into the tools;
   commit, so the tree is clean.
3. C1 pre-check: exact-zero fraction of zone 0 horizon-1 one-step changes at
   L >= 30. If it is above 0.54, Phase A ends as FAIL with that number. Reason:
   with more than 54 % of changes exactly zero, the central 50 % interval of
   any continuous predictive covers every zero change, so 50 % coverage cannot
   fall inside [0.46, 0.54] (quantization class, v1 section 5a). C2 is then
   not collected or evaluated.
4. If the pre-check passes: fit M2 on C1 and screen it in sample on C1 with
   the section 6 rules. If M2 fails in sample, Phase A ends as FAIL with that
   statistic named. No fourth model.
5. Otherwise run exactly one `est_eval --protocol-v2 --recorded` on C2 into
   `receipts/est23-v2/`. A second execution on C2 is a new protocol version.

## 6. Statistics and pass rules (copied from v1 sections 4 and 5, unchanged)

Burn-in 30 steps. For M2, coverage uses the mixture's central quantiles in
place of z * sqrt(S); NIS, bias and lag-1 use nu / sqrt(S) with S = q + r.

A model is **calibrated** when all of the following hold on C2:

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

Selection: the simplest calibrated model (M0, then M1, then M2) whose one-step
log score is within 0.01 nats per step of the best calibrated model's.

`ESTIMATION_CALIBRATION = PASS` when at least one model is calibrated and its
one-step RMSE is no worse than persistence's. M2's RMSE equals persistence's
by construction (section 2), so for M2 this condition is a tie, which counts
as "no worse". Otherwise FAIL, recorded with the failing statistics.

## 7. What this protocol does not allow

Changing bands, burn-in, k, floor, iterations or interval rule after seeing
any C1 or C2 statistic; reading run B; refitting on C2; dropping outliers;
adding a model; a second recorded execution on C2.

## 8. Load during collection

`load.txt` in the C1 folder: 209 samples of `/proc/loadavg` over the C1 window, 1-minute load mean 0.47, maximum 1.03, on a 20-core machine. An idle regime. Root `perf stat` processes left from the discarded attempt were still running but wrote only into the discarded folder.
