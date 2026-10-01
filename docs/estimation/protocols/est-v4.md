# ESTIMATION v4 protocol: persistence-aware calibration under the declared load period (pre-registered)

Workstream: ARCH-0020 (belief / estimation layer), ESTIMATION v4. This protocol
is frozen by the commit that adds it, BEFORE its fit data D1 is collected. Its
SHA-256 is compiled into the v4 tool (`tools/estimation/est4.c`) and written
into params.txt and every v4 receipt. Any change after that commit is a new
protocol version. Protocols v1, v2 and v3 and their receipts
(`receipts/est23-v1/`, `receipts/est23-v2/`, `receipts/est3c-v3/`) stay on
record unchanged; none is rescored.

## 1. Why v4 exists, and what it changes

v3 (`EST3C_PROTOCOL_V3.md`, `receipts/est3c-v3/RESULT.md`) ended PHASE_A_FAIL.
Its adaptive-scale Student-t family F5 met every one-step rule on v3 D1 and
failed only the ten-step rule (95 % coverage 0.809). Lesson recorded there:
multi-step changes persist (variance ratio VR(10) = 2.14), and a predictive
whose h-step change is a sum of independent one-step changes cannot represent
that.

v4 changes exactly that, and keeps everything else of v3:

- Each candidate is a small linear state-space model of the reading whose
  **multi-step predictive propagates the model** (mean and variance through
  h applications of the transition matrix), so persistence and mean reversion
  appear in the h-step law.
- The h-step law has its **own** tail and scale parameters (nu_h, c_h) and a
  scale-reversion rate phi, fitted on the ten-step log score in a second
  stage (section 5). They never touch the filter or the one-step law.
- Unchanged from v3: the discrete 100 mC predictive, fractional coverage,
  mid-PIT z, all pass bands (section 6, copied verbatim), burn-in, tick and
  gap rules, the E0 and F1 baselines, the selection rule, INCONCLUSIVE rules,
  the load generator and the collector (only the folder tag and flag text
  differ).

## 2. Candidate families (`src/estimation/est_v4.c`)

Notation: y_t is the zone-0 reading (mC), q = 100 mC the quantum,
r = q^2 / 12 = 833.33 mC^2 (quantization variance, fixed), Q the process
noise, H the observation row. All three families are Kalman filters with
unscaled Q and r; an adaptive scale g multiplies only the predictive.

| id | family | state x | transition A | Q | H | dyn parameter |
|---|---|---|---|---|---|---|
| G1 | first-order lag toward a random-walk equilibrium | [T, U] | [[a, 1-a], [0, 1]], a = exp(-1/tau) | q on U | [1, 0] | tau (s) |
| G2 | AR(1) change (persistent increments) | [Y, D] | [[1, rho], [0, rho]] | q [[1, 1], [1, 1]] | [1, 0] | rho |
| G3 | two time constants, one equilibrium | [Tf, Ts, U] | [[af, 0, (1-af) w], [0, as, (1-as)(1-w)], [0, 0, 1]], af = exp(-1/tau_f), as = exp(-1/(10 tau_f)), w = 0.5 | q on U | [1, 1, 0] | tau_f (s) |

First valid observation y: G1 x = [y, y], P = diag(r, r + q); G2 x = [y, 0],
P = diag(r, q); G3 x = [y/2, y/2, y], P = diag(r, r, r + q) (prior reading
variance 2r, no Tf/Ts correlation; immaterial after the 30-tick burn-in);
g = gl = 1.

Per valid tick: time update x = A x, P = A P A' + Q; predicted mean
m = H x, S1 = H P H' + r; e = y - m; standard Kalman measurement update; then
g <- lambda g + (1 - lambda) max(e^2, r) / S1 and
gl <- 0.995 gl + 0.005 g. A missing tick does the time update only, leaves g
unchanged, updates gl the same way and increments the gap.

Predictive s steps ahead of the current belief (missing ticks are already
absorbed by their time updates): m_s and V_s are the mean and variance of
H x + noise after s further time updates (V_s = H P_s H' + r). The
effective horizon recorded on the pmf is gap + s and must be at most 64
(otherwise the step is unscorable, as v3).

- s = 1 (including the first reading after a gap): location m_1, scale
  c sqrt(g V_1), Student-t with nu degrees of freedom.
- s >= 2: location m_s, scale c_h sqrt(gbar_s V_s), Student-t with nu_h, where
  gbar_s = (1/s) sum_{j=1..s} (gl + (g - gl) phi^(j-1)).

The continuous law is discretised on the 801-bin grid of v3 (offsets
k = -400..400 quanta from the last reading, edge bins take the tails,
1e-12 uniform floor, `est_pmf_floor`), so the v3 scorer and statistics apply
unchanged. Parameters per family: dyn, q, lambda, nu, c (stage 1) and phi,
nu_h, c_h (stage 2). est_v4.o has no authority (est-v4-purity nm -u check).

## 3. Data

| set | role | folder | status |
|---|---|---|---|
| A, B, C1 | development only | as v3 section 3 | spent |
| v3 D1 | **development only** | `evidence/EST3C/raw/20261001T025159Z-est3c-fit-silicon/` | read by v4 design work; never fit or held-out data for v4 |
| D1 | **fit** | `evidence/EST4/raw/<UTC>-est4-fit-silicon/`, seed 0xE5C4D1, 2700 s | collected only after this freeze |
| D2 | **sealed held-out** | `evidence/EST4/raw/<UTC>-est4-heldout-silicon/`, seed 0xD2E5C4, 2700 s, separate run | collected only after a Phase A PASS |

Collector: `tools/estimation/est4_collect.sh <fit|heldout> 2700 <seed>
evidence/EST4/raw <est_load>` (the v3 collector with tag `est4`): 1 Hz reads
of `/sys/class/thermal`, `/proc/loadavg` and the aggregate `/proc/stat` cpu
line, while `tools/estimation/est_load.c` runs the schedule. est_load binary
built by `cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic -pthread` from
est_load.c sha256 7353532b9082b468734a46122e75c610a7c4b45a8f300ab7faaa8a15e6cd76cf
gives binary sha256 8972b0202a95900f3f401a342789e9ffdc8d7c8c477ee7d68ae8683846547f5e
(the v3 binary, bit for bit). Signal: zone 0, the first `thermal_mc` field.
Ticks, gaps, burn-in (30 ticks from the first valid reading), the unscorable
rule (effective horizon above 64) and the observation range are v3's
(`est3c_common.h`).

D1 identity: after collection, `receipts/est-v4/d1.sha256` (SHA-256 of
machine-state.ndjson, machine-state-marks.txt and schedule.txt) is committed
with the raw folder; the binding fit is built from that clean tree with those
SHAs compiled in and refuses any other input. D2 the same with
`receipts/est-v4/d2.sha256`, after params.txt is committed.

Development evidence (v3 D1, declared here; it informed the design and is
spent): with this tool's full grid, all three families meet every one-step,
PIT, bias, lag-1, quarter and ten-step rule (G1: 95 % coverage 0.951, ten-step
0.968, lag-1 z -0.045) and all three fail the idle-regime rule narrowly
(G1 0.896, G2 0.895, G3 0.892 against 0.90, 235 idle steps; misses are
isolated single-step spikes during idle, not transitions). Earlier dev
variants with a constant scale, or one tail law for every horizon, gave
ten-step coverage 0.82 to 0.87. No v4 parameter or grid was changed after the
idle-regime result; it is a declared risk of this version (D1's schedule has
567 idle seconds against 235 in v3 D1).

## 4. Declared loaded period and machine coordination

The declared regime is the est_load schedule, and only that: CPU heat from
integer-arithmetic busy threads, levels {0, 6, 12, 18}, segments of 20 to
120 s, deterministic from the seed (splitmix64, est_load.c). Total 2700 s.
Exact schedules (level/seconds, in order), derivable from the seed alone:

- D1, seed 0xE5C4D1 (40 segments; idle 567 s, L6 1058 s, L12 576 s, L18 499 s):
  6/71, 18/115, 6/65, 6/116, 6/107, 6/81, 0/30, 6/33, 0/58, 0/42, 18/96,
  6/64, 0/78, 0/80, 12/88, 0/23, 6/117, 0/118, 12/88, 0/70, 12/80, 6/56,
  12/49, 12/84, 6/49, 6/104, 6/45, 18/32, 6/70, 18/44, 18/90, 18/69, 12/62,
  12/34, 12/91, 6/32, 6/48, 0/68, 18/36, 18/17.
- D2, seed 0xD2E5C4 (40 segments; idle 565 s, L6 865 s, L12 456 s, L18 814 s):
  12/89, 0/22, 18/101, 6/84, 18/57, 12/28, 6/47, 6/108, 0/54, 6/100, 0/106,
  18/46, 0/72, 6/119, 6/59, 6/23, 18/29, 18/22, 6/63, 0/91, 12/77, 18/49,
  18/30, 18/120, 18/53, 18/25, 18/20, 18/60, 0/35, 6/90, 18/107, 6/65, 18/24,
  6/107, 0/101, 12/77, 12/101, 18/71, 12/84, 0/84.

Coordination: one heavy run machine-wide. The collector refuses to start while
`~/workspace/.spark-quiet` exists or an `est_load` process runs; it writes the
flag with the text "lane15 est v4 load period" for its own run and removes it
afterwards. It is started only when the flag is absent and no est_load or GPU
gate is running; it never stops another lane's run. Foreign load is measured
as in v3 section 4.

## 5. Development procedure (D1 only; mechanical; `est4 fit`)

Grids (q = 10^k):

- dyn: G1 tau in {0.5, 1, 2, 4, 10, 30}; G2 rho in {0, 0.2, 0.4, 0.6, 0.8, 0.9};
  G3 tau_f in {0.5, 1, 2, 4, 10}.
- q: k = 2.0, 2.5, ..., 6.0.
- lambda in {0.1, 0.2, 0.3, 0.5, 0.7, 0.9}; nu in {0.8, 1, 1.25, 1.5, 2, 3, 5};
  c = 0.20, 0.25, ..., 1.20.
- phi in {1, 0.95, 0.9, 0.8, 0.6}; nu_h in {0.5, 0.6, 0.7, 0.8, 1, 1.25, 1.5, 2, 3, 5};
  c_h = 0.10, 0.15, ..., 1.60.

Stage 1: (dyn, q, lambda, nu, c) maximise the D1 mean one-step discrete log
score. Stage 2: with stage 1 fixed, (phi, nu_h, c_h) maximise the D1 mean
ten-step discrete log score (ten-step origins and targets as section 6 rule 6).
Loops run in the listed order; ties keep the first grid point (strict
improvement only). A (dyn, q, lambda) point whose filter returns a numeric
error is skipped and counted in params.txt. Grid search uses a closed-form
score of the same discrete predictive (outcome bin, edge-bin tails and the
1e-12 floor included); the fit then rescores each family with the full pmf path and
marks the family unavailable if the two mean log scores differ by more than
1e-6 nats/step or score different step counts.

Baselines refitted on v4 D1 with v3's definitions: F1 (Gaussian random-walk
Kalman, v3 grid q_proc = 10^k, k = 1.0..7.0 by 0.25; r = 10^k, k = 2.0..6.0 by
0.25) and E0 (D1 histogram of horizon-1 changes, v3 section 6).

In-sample screen: each family scored on D1 with the section 6 rules.
Selection: among screen passers, the highest D1 one-step mean log score; any
family within 0.01 nats/step of it that comes earlier in G1 < G2 < G3 is
preferred. If no family passes in sample, Phase A ends PHASE_A_FAIL with each
family's first failing rule and its value, and **D2 is NOT_RUN** (not
collected). The fit writes `receipts/est-v4/params.txt` (all parameters, D1
scores, grid-edge flags, screen results, F1, E0, selection, phase_a); it is
committed before any D2 collection and is binding.

## 6. Statistics and pass rules (identical to v3 section 6)

For a predictive and outcome, F_lo, F_hi, fractional coverage, PIT and
mid-PIT probit z = Phi^-1((F_lo + F_hi)/2) clipped to [-8, 8] are as in v3
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

No band is loosened. Rule 6 as implemented (v3 `c3_score`, mirrored by
`est4`): from the belief after each scored valid tick t, the horizon-10
predictive is scored against tick t + 10 if that tick is valid. Implementation: `c3_judge` / `c3_stats` (v3 code,
unchanged). Sharpness (central 80 % width) is reported for S, F1 and E0.

`ESTIMATION_CALIBRATION (v4) = PASS` when the selected family S is
calibrated on D2 AND its D2 mean one-step log score is no worse than F1's and
no worse than E0's by more than 0.01 nats/step each, on the same scored steps.
Calibrated but losing a comparison: HELD_OUT_FAIL (not better than a
baseline). Not calibrated: HELD_OUT_FAIL with the first failing statistic.

## 7. INCONCLUSIVE rule (before any D2 thermal value is scored)

v3 section 7, unchanged (`c3_precheck`): fewer than 2030 lines; more than 2 %
of gaps above 1.5 s; marks not exactly one begin/end pair per scheduled
level > 0 segment in order; mean foreign busy cores above 1.5 or more than
10 % of samples above 3.0; foreign load unmeasurable on more than 1 % of
samples. After an INCONCLUSIVE D2, a new D2 may be collected with seed
0xD2E5C4 + n (n = 1, 2) under this protocol; the inconclusive folder stays on
record unscored. A third INCONCLUSIVE ends v4 as INCONCLUSIVE. The same
pre-check is run on D1 before the fit; an INCONCLUSIVE D1 is recollected with
seed 0xE5C4D1 + n (n = 1, 2) and stays on record unfitted.

## 8. Order of operations

1. Commit the v4 models, tests, tool, collector and this protocol (freeze).
   Record this file's SHA-256 in the PR.
2. Collect D1 (section 4). Commit the raw folder and `receipts/est-v4/d1.sha256`.
3. `est4 precheck` on D1. If VALID, build `est4` from that clean tree and run
   `est4 fit` once; commit `receipts/est-v4/params.txt`.
4. PHASE_A_FAIL: D2 NOT_RUN; go to 6.
5. Phase A PASS: collect D2 in a fresh run; commit it and
   `receipts/est-v4/d2.sha256`; rebuild from the clean tree (D2, params SHAs
   compiled in); `est4 precheck`, then exactly one `est4 recorded` into
   `receipts/est-v4/`. The receipt is named by its SHA-256 and records tool
   commit, protocol, params and data SHAs; the tool refuses a dirty tree and a
   second receipt.
6. Write `receipts/est-v4/RESULT.md` by hand from params.txt / the receipt.

## 9. What this protocol does not allow

Changing families, formulas, grids, bands, burn-in, the interval rule, the
two-stage fit or the selection rule after D1 has been collected; fitting on
anything but D1; reading D2 thermal values before the recorded run; refitting
after seeing D2; dropping outliers; adding a family; a second scored run on
D2; tuning anything on D2. After a FAIL, the v5 finding is stated from the
recorded numbers without refitting.

## 10. Downstream gating

EST-4 (World belief state) and EST-5 (uncertainty-aware cost model) are
unblocked only by a v4 PASS, using S with its frozen parameters; this
protocol does not start them. After a FAIL or INCONCLUSIVE nothing downstream
may treat any estimator output as calibrated.
