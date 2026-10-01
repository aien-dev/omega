# EST-3c protocol v3: quantization-aware calibration under a declared load schedule (pre-registered)

Workstream: ARCH-0020 (belief / estimation layer), ESTIMATION-2. This protocol
is frozen by the commit that adds it. Its SHA-256 is compiled into the v3
evaluator and written into every v3 receipt. Any change after that commit is a
new protocol version. Protocol v1 (`EST23_PROTOCOL_V1.md`, FAIL, receipts in
`receipts/est23-v1/`) and protocol v2 (`EST23_PROTOCOL_V2.md`, C1 pre-check
FAIL, `receipts/est23-v2/`) stay on record unchanged. Neither result is rescored.

## 1. Why v3 exists, and what it changes

Forensics: `EST3C_FORENSICS.md` (reproduced from committed artifacts only).
Two named failures enter v3:

1. v1, loaded regime: heavy-tailed one-step changes with volatility
   clustering; a single Gaussian scale was wrong in the middle and the tails
   (model misspecification class).
2. v2, idle regime: about 80 % of one-step changes were exactly zero on the
   100 mC reading grid, so a central interval of any continuous predictive
   cannot have 50 % coverage (quantization class). This is a fault of the
   experiment's statistic, not of an estimator.

v3 therefore changes both the predictive and the statistic, and nothing else
silently:

- Every candidate predicts a **discrete** distribution of the next reading on
  the 100 mC grid (quantization-aware observation model):
  P(Y = y_last + k q) = G((k + 1/2) q) - G((k - 1/2) q), q = 100 mC, where G
  is the candidate's continuous distribution of the change.
- Calibration is scored with **fractional (randomized-PIT) coverage**: for an
  outcome with F_lo = P(Y < y) and F_hi = P(Y <= y), the coverage credit of the
  central alpha interval is |[(1 - alpha)/2, (1 + alpha)/2] ∩ [F_lo, F_hi]| /
  (F_hi - F_lo). It is the expectation of randomized-PIT coverage with no
  random draw, so it is deterministic and has no seed to choose. For a
  calibrated discrete predictive it averages to alpha.
- The target regime is **declared and generated**: a CPU load schedule
  (section 4) with load trials and idle gaps, so both regimes are present and
  marked.

## 2. Candidate families (considered, implemented in `src/estimation/est_pred.c`)

All share one contract (assumption, belief state, discrete predictive,
innovation, calibration evidence). None has authority (est-v3-purity nm -u
check). An estimate is never an observation.

| id | family | why it is a candidate | parameters (fitted on D1 only) |
|---|---|---|---|
| F1 | Gaussian random-walk Kalman (reference) | v1 baseline, linear-Gaussian reference | q_proc, r |
| F2 | Huber-robust Kalman | robust to outliers without changing the predictive shape | q_proc, r, c |
| F3 | Student-t change, fixed scale | heavy tails (v1 failure class) | nu, s |
| F4 | Gaussian scale mixture, k = 3 (EM) | mixture noise, v2's M2 | weights, variances (EM) |
| F5 | adaptive-scale Student-t (EWMA of squared changes) | heavy tails plus volatility clustering (v1 Ljung-Box) | lambda, nu, c, floor |

Considered and not implemented: a bounded-support change distribution
(truncated at a dev maximum). Rejected before any fit: an outcome outside the
support gets probability 0 and an infinite log loss, which is exactly the
"confident wrong" behaviour this layer must not have. A load-aware model that
reads the schedule as a control input is out of scope for v3 (it would no
longer be an estimator of the signal alone).

## 3. Data

| set | role | file | status |
|---|---|---|---|
| A | development only | `evidence/R15/raw/20260929T020536Z-ad8e1f2ea4e4-silicon/` | GPU-trial load; used by v1 fit |
| B | development only | `evidence/R15/raw/20260929T025735Z-3e9e53be3358-silicon/` | v1 held-out, now spent |
| C1 | development only | `evidence/EST3B/raw/20261001T020412Z-est3b-fit-silicon/` | idle; v2 pre-check |
| D1 | **fit** | `evidence/EST3C/raw/20261001T025159Z-est3c-fit-silicon/` | v3 schedule, seed 0xE5C3D1, 2700 s |
| D2 | **sealed held-out** | collected only after this freeze, seed 0xD2E5C3, 2700 s | not yet collected |

A, B and C1 are development artifacts. They are never a held-out set again.
D1 was collected (2026-10-01 02:51:59Z to about 03:37Z) before this freeze;
its thermal values had not been read when this document was written, and D1 is
the only file the fit program opens.

Collector: `tools/estimation/est3c_collect.sh` (1 Hz shell reader of
`/sys/class/thermal`, `/proc/loadavg` and the aggregate `/proc/stat` cpu line)
running `tools/estimation/est_load.c` (deterministic schedule from a seed:
segments of {0, 6, 12, 18} busy integer-arithmetic threads, 20 to 120 s each,
schedule printed before it starts, load trials written as R15-shape marks).
Signal: zone 0, first `thermal_mc` field, mC; replay and gap rules as v1
section 2 (horizon = max(1, round(gap / 1 s))).

Fixed implementation choices (made before any tool read D1; code in
`tools/estimation/est3c_common.h` header): observation range lo -40000,
hi 150000 mC, quantum 100 mC; F1/F2 p0 = r; F5 s0 = sqrt(floor). One logical
tick per ndjson line plus round(gap) - 1 inserted MISSING ticks; burn-in is
counted in ticks from the first valid reading; a step whose effective horizon
exceeds 64 is unscorable. Horizon-1 changes (F4 EM data, E0 histogram) and
ten-step origins come only from ticks at or after burn-in. Quarters (rule 4)
split the scored one-step steps by index. The first failing statistic is
named in section 6 listing order.

Provenance of D1: collected 02:51:59Z to 03:36:59Z by `est3c_collect.sh fit 2700
0xE5C3D1`, 2681 lines, 35 load trials. est_load binary sha256 8972b020...47f5e,
reproduced bit for bit by `cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic
-pthread` from `tools/estimation/est_load.c` (sha256 7353532b...6cf). D1 raw
SHA-256: machine-state.ndjson 9d5473b56dbb3765e5fc5335ef74aec47a11485651bcffc75272062ab48dc230,
machine-state-marks.txt 4dfd06ec3f07c96eefe9697da2809634a0d4db2bfe4718909f2d0ca2c23d7a72.
No tool or person read a D1 thermal value before this freeze.

## 4. Declared loaded period and machine coordination

- The declared regime is the est_load schedule, and only that. It is CPU heat
  from a known schedule, not GPU load: a PASS says nothing about GPU-trial
  load (runs A and B) or other workloads.
- One heavy run at a time machine-wide. The collector refuses to start while
  `~/workspace/.spark-quiet` exists and holds that flag for its own run. D2 is
  scheduled only when no other lane's hardware gate or heavy test is running
  (checked with the process list and `~/handoffs/*report*` before start).
- Foreign load is measured, not assumed: per sample, busy cores =
  20 x (delta non-idle jiffies / delta total jiffies) from the logged cpu
  line (idle and iowait count as idle); foreign = busy - the load level in
  force over that sample interval, time-weighted from the marks pairs (the
  actual trial times, not schedule.txt offsets).

## 5. Development procedure (D1 only; mechanical; tool `est3c_fit`)

Burn-in 30 steps. Coasted (missing or bad) steps contribute nothing.
Each family's parameters maximise the D1 one-step mean discrete log score over
these fixed grids (ties: first grid point in the listed order):

- F1: q_proc = 10^k, k = 1.0, 1.25, ..., 7.0; r = 10^k, k = 2.0, 2.25, ..., 6.0.
- F2: F1 grid x c in {1.0, 1.345, 2.0, 3.0}.
- F3: nu in {1.5, 2, 2.5, 3, 4, 5, 7, 10, 15, 30}; s = 10^k, k = 1.0, 1.05, ..., 3.5.
- F4: EM, k = 3, variance floor 100^2/12 mC^2, 1000 iterations, deterministic
  initialisation (v2 `est_mix_fit_em`), on horizon-1 non-coasted D1 changes.
- F5: lambda in {0.5, 0.7, 0.8, 0.9, 0.95, 0.97, 0.99}; nu in {2, 3, 4, 5, 7, 10, 30};
  c = 0.50, 0.55, ..., 1.50; floor in {100^2/12, 50^2, 100^2} mC^2; s0^2 = floor.

In-sample screen: each fitted family is scored on D1 with the section 6 rules
(one-step, PIT, bias, lag-1, quarters, regimes, ten-step).
Selection: among families that pass the in-sample screen, the highest D1
mean log score; any family within 0.01 nats/step of it that comes earlier in
the order F1 < F2 < F3 < F4 < F5 is preferred (simplest wins ties).
If no family passes in sample, Phase A ends as FAIL with the failing
statistics; D2 is not collected.
The fit tool writes `receipts/est3c-v3/params.txt` (all families' parameters,
D1 log scores, screen results, the selected family). It is committed before
D2 is collected and is binding.

## 6. Statistics and pass rules (sealed run on D2)

For a predictive and outcome, F_lo, F_hi, fractional coverage, PIT and
mid-PIT probit z = Phi^-1((F_lo + F_hi)/2) clipped to [-8, 8] are as in
section 1. A family is **calibrated** on a data set when all of:

1. One-step fractional coverage: 50 % in [0.46, 0.54]; 80 % in [0.76, 0.84];
   95 % in [0.93, 0.97].
2. Each of the 10 PIT histogram bins (fractional allocation) holds a share in
   [0.07, 0.13].
3. |mean z| <= 0.10 (bias); |lag-1 autocorrelation of z| <= 0.20.
4. Every quarter (by step index) has 95 % fractional coverage in [0.90, 0.99].
5. Each regime (in-trial, idle; marks as v1 section 1) with at least 100
   steps has 95 % fractional coverage in [0.90, 0.99].
6. Ten-step: from each belief at step k, the horizon-10 predictive scored
   against the reading at step k + 10 when it exists; 95 % fractional
   coverage in [0.90, 0.99].
7. At least 2000 scored one-step steps.

Baselines evaluated in the same sealed run with D1-frozen parameters: F1
(Gaussian reference) and E0, the empirical-change climatology (the D1
histogram of horizon-1 grid changes over k in [-400, 400], smoothed by one
pseudo-observation spread uniformly: p(k) = (c_k + 1/801) / (N + 1);
persistence centre).

Sharpness: mean and median width (mC) of the central 80 % interval of the
discrete one-step predictive; reported for the selected family, F1 and E0.

`ESTIMATION_CALIBRATION (v3) = PASS` when the selected family S is
calibrated on D2 AND its D2 mean log score is no worse than F1's and no worse
than E0's by more than 0.01 nats/step each. If S is calibrated but loses
either comparison: FAIL (calibrated but not better than a baseline). If S is
not calibrated: FAIL, recorded with the first failing statistic and a failure
class (wrong noise shape, dependence, nonstationarity, regime switching,
quantization, sensor fault, or misspecification).

## 7. INCONCLUSIVE rule (decided before any D2 thermal value is scored)

The evaluator's first phase reads only D2 times, the loadavg / cpu fields,
marks and schedule. D2 is INCONCLUSIVE, and is not scored, if any of:

- fewer than 2030 lines (2000 scored steps plus burn-in);
- more than 2 % of gaps above 1.5 s;
- the marks file does not contain exactly one begin/end pair per scheduled
  load segment with level > 0, in schedule order with matching level and
  index (est_load ended early or was disturbed); level-0 segments have no marks;
- foreign load: mean foreign busy cores above 1.5, or more than 10 % of
  samples with foreign busy cores above 3.0;
- foreign load unmeasurable (cpu field missing or not monotonic) on more
  than 1 % of samples;
- any thermal_mc field missing on more than 1 % of lines (sensor fault) is
  NOT a reason for INCONCLUSIVE; it is scored as missing and reported.

After an INCONCLUSIVE D2, a new D2 may be collected with seed 0xD2E5C3 + n
(n = 1, 2) under this same frozen protocol; the inconclusive file stays on
record unscored. A third INCONCLUSIVE ends v3 as INCONCLUSIVE.

## 8. Order of operations

1. Commit forensics, the est_pred library with hostile tests, the tools and
   this protocol; D1 raw committed with this freeze.
2. Run `est3c_fit` on D1 (clean tree, tool commit recorded); commit
   `receipts/est3c-v3/params.txt`.
3. Collect D2 (section 4 coordination). Commit the D2 raw folder and
   `receipts/est3c-v3/d2.sha256` (SHA-256 of machine-state.ndjson,
   machine-state-marks.txt and schedule.txt); the evaluator is built from a
   clean tree with those SHAs and the SHA-256 of params.txt compiled in, and
   refuses any other D2, params file or receipt folder.
4. Run `est3c_eval --precheck` (section 7). If VALID, run exactly one
   `est3c_eval --recorded` into `receipts/est3c-v3/`. The receipt is named by
   its SHA-256, records tool commit, protocol SHA, params SHA and data SHAs,
   and the tool refuses a dirty tree. A second scored execution on D2 is a new
   protocol version.
5. Write `receipts/est3c-v3/RESULT.md` by hand from the receipt. PASS, FAIL
   and INCONCLUSIVE are all acceptable outcomes.
6. Informational (not gating, same receipt): the selected family with frozen
   parameters on A, B and C1.

## 9. What this protocol does not allow

Changing grids, bands, burn-in, families, the interval rule or the selection
rule after D1 has been read by any tool; fitting on anything but D1; reading
D2 thermal values before the recorded run (the collector prints no
statistic); dropping outliers; adding a family; a second scored run on D2;
using D2 to tune anything. The sealed-evaluator worker runs only the commands
in step 4.

## 10. Downstream gating

EST-4 (World belief state) and EST-5 (uncertainty-aware cost model) proceed
only after a PASS here, using the selected family with its frozen parameters.
After a FAIL or INCONCLUSIVE, nothing downstream may treat any estimator output
as calibrated.
