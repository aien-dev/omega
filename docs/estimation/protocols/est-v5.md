# ESTIMATION v5 protocol: the v4 estimator under a machine-enforced collection window (pre-registered)

Workstream: ARCH-0020 (belief / estimation layer), ESTIMATION v5. This protocol
is frozen by the commit that adds it, BEFORE its fit data D1 is collected. Its
SHA-256 is compiled into the v5 tool (`tools/estimation/est5.c`) and written
into params.txt and every v5 receipt. Any change after that commit is a new
protocol version. Protocols v1, v2, v3 and v4 and their receipts
(`receipts/est23-v1/`, `receipts/est3c-v3/`, `receipts/est-v4/`) stay on
record unchanged; none is rescored.

## 1. Why v5 exists, and what it changes

v4 (`protocols/est-v4.md`, sha256
`7baff66f1baf13f648b38ea04516b423e34e4c4b4d7528701f15272ca441f116`,
`receipts/est-v4/RESULT.md`) ended INCONCLUSIVE: its selected family G1 met every
rule on both complete sealed runs, but every collection window was spoiled by
another lane's host builds and tests (a courtesy quiet flag did not keep them
out), so no attempt counts. Its finding: the blocker was measurement
isolation, not the model.

v5 keeps the v4 model, grids, fit, statistics, pass bands and baselines
**exactly** and changes only window protection:

- Unchanged and not re-derived: the families and the library
  `src/estimation/est_v4.c` (sha256
  `c88a8f0794a9074be53019c180e593dcc35e5b54b2221a61190f5f9f4474e0cb`, header
  `est_v4.h` sha256
  `2ea789c068bda32eee9bc7922a0893eb08e73ff9ae2779bde1a01fd068c95bcb`), sections 2, 5
  and 6 of v4 (copied verbatim below), burn-in, tick and gap rules, the load
  generator and its binary (section 3), the v3 pre-check thresholds, and F1 and
  E0. The `est5` tool is the `est4` tool with v5 paths, tags and identities
  only, plus the refusal of v4 held-out paths.
- Changed, window protection only (sections 3, 4, 7, 8): fresh seeds; a
  machine-wide enforced quiet window (the Claude Code `quiet-guard` hook blocks
  builds, tests and QEMU in every session while `~/workspace/.spark-quiet`
  exists, and the flag is held for the whole of each collection); a collector
  that refuses to run without the flag, refuses to start when a foreign
  build/test/gate process is already running, and records a process monitor;
  and a new INCONCLUSIVE rule: any foreign build, test or gate process in the
  process monitor voids the window (section 7). Adding a way to void data
  cannot move a score, and the voiding rule is declared here, before any data.
- The model and the pass rules were never tuned on any v4 score: the v4 void
  numbers (G1 held-out coverage 0.9476 and 0.9570, ten-step 0.9321 and 0.9534)
  are not used for any v5 choice.

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
| v3 D1 | **development only** | `evidence/EST3C/raw/20261001T025159Z-est3c-fit-silicon/` | read by v4 design work; never fit or held-out data for v4 or v5 |
| v4 attempts 1 to 3 | **void, never used** | `evidence/EST4/raw/` | collected under foreign load; not fit, not scored for v5, no v5 choice depends on them |
| D1 | **fit** | `evidence/EST5/raw/<UTC>-est5-fit-silicon/`, seed 0xE5C5D1, 2700 s | collected only after this freeze |
| D2 | **sealed held-out** | `evidence/EST5/raw/<UTC>-est5-heldout-silicon/`, seed 0xD2E5C6, 2700 s, separate run | collected only after a Phase A PASS |

Collector: `tools/estimation/est5_collect.sh <fit|heldout> 2700 <seed>
evidence/EST5/raw <est_load>` (the v4 collector with tag `est5`, the enforced
window of section 4, and a process monitor): 1 Hz reads of
`/sys/class/thermal`, `/proc/loadavg` and the aggregate `/proc/stat` cpu
line, while `tools/estimation/est_load.c` runs the schedule. The est_load
binary is built by `cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic -pthread`
from est_load.c sha256
7353532b9082b468734a46122e75c610a7c4b45a8f300ab7faaa8a15e6cd76cf and has binary
sha256 8972b0202a95900f3f401a342789e9ffdc8d7c8c477ee7d68ae8683846547f5e
(the v3 binary, bit for bit). Signal: zone 0, the first `thermal_mc` field.
Ticks, gaps, burn-in (30 ticks from the first valid reading), the unscorable
rule (effective horizon above 64) and the observation range are v3's
(`est3c_common.h`).

D1 identity: after collection, `receipts/est-v5/d1.sha256` (SHA-256 of
machine-state.ndjson, machine-state-marks.txt and schedule.txt) is committed
with the raw folder; the binding fit is built from that clean tree with those
SHAs compiled in and refuses any other input. D2 the same with
`receipts/est-v5/d2.sha256`, after params.txt is committed. The raw folders
also carry `process-monitor.log` and `WINDOW` (section 4); their SHA-256 are in
the folder's `SHA256SUMS`.

Development evidence (v3 D1, declared here; it informed the v4 design and is
spent): with this tool's full grid, all three families meet every one-step,
PIT, bias, lag-1, quarter and ten-step rule (G1: 95 % coverage 0.951, ten-step
0.968, lag-1 z -0.045) and all three fail the idle-regime rule narrowly
(G1 0.896, G2 0.895, G3 0.892 against 0.90, 235 idle steps; misses are
isolated single-step spikes during idle, not transitions). No v5 parameter or
grid was changed after the idle-regime result; it is a declared risk of this
version, unchanged from v4. The idle seconds of the v5 schedules are in
section 4.

## 4. Declared loaded period and machine coordination (the v5 change)

The declared regime is the est_load schedule, and only that: CPU heat from
integer-arithmetic busy threads, levels {0, 6, 12, 18}, segments of 20 to
120 s, deterministic from the seed (splitmix64, est_load.c). Total 2700 s.
Exact schedules (level/seconds, in order), derivable from the seed alone:

- D1, seed 0xE5C5D1 (39 segments; idle 406 s, L6 929 s, L12 655 s, L18 710 s):
  0/74, 6/111, 18/25, 6/62, 6/52, 0/28, 6/48, 12/50, 6/99, 18/32, 12/38,
  18/76, 18/28, 0/104, 0/81, 12/59, 12/117, 12/71, 6/22, 6/104, 18/32,
  18/57, 0/119, 12/86, 6/109, 18/95, 18/47, 6/78, 6/94, 18/95, 6/114,
  18/119, 12/120, 12/51, 18/57, 18/47, 12/63, 6/29, 6/7.

- D2, seed 0xD2E5C6 (40 segments; idle 676 s, L6 627 s, L12 857 s, L18 540 s):
  0/66, 12/81, 18/79, 18/38, 0/46, 12/119, 6/46, 18/34, 12/88, 6/107, 0/72,
  6/53, 12/22, 18/97, 12/31, 0/89, 18/84, 0/32, 18/52, 12/100, 0/102, 6/43,
  6/21, 12/113, 0/45, 18/116, 6/49, 12/72, 6/43, 12/100, 0/60, 6/102,
  12/41, 18/40, 0/47, 12/90, 0/107, 6/84, 6/79, 0/10.

Enforced window. Each collection (D1, D2, and any recollection) happens only
inside a window with all of the following, and the collector checks the ones it
can:

1. The collecting session holds `~/workspace/.spark-quiet` for the whole run,
   in the machine's standard key-value format (owner, gate `est-v5`, start,
   expected_end, pid) with one line `allow est_|estimation` so its own commands
   pass. The flag is taken only when it is absent and no other lane's heavy
   run or chip gate is running; it never stops another lane's run. The flag
   is released the moment the collection ends (a re-take is needed for D2).
2. The Claude Code `quiet-guard` hook (installed machine-wide before v5)
   refuses make, cmake, ninja, ctest, cargo build/test/run, qemu, gcc, clang, cc
   and test commands in every session that does not hold or pass the flag, for as
   long as the flag exists. It cannot stop a process started outside a Claude Code
   session; that is what item 3 detects.
3. `tools/estimation/est5_collect.sh` refuses to start unless the flag file
   contains the line `gate est-v5` and no est_load process runs; refuses to
   start while a foreign build, test or gate process is running
   (`est5_window_scan.sh`: make, cc, gcc, cc1, clang, ld, as, cmake, ninja,
   ctest, cargo, rustc, qemu-system-*, test_*, est*, verify_all*, run_gate*, `t`,
   other than its own load generator); and records `process-monitor.log` with
   one sample line every 2 s, a FOREIGN line for every such process, and
   informational HIGHCPU lines for any process above 25 % CPU. It never
   creates or removes the flag. At the end `est5_window_check.sh` writes
   `WINDOW`: WINDOW_CLEAN when the log has no FOREIGN line and at least 90 %
   of the 1350 expected samples, otherwise WINDOW_VOID with the reason.
4. Foreign CPU load is also measured by the v3 pre-check (section 7), unchanged.

The window check measures the collector's own interval only. The window
evidence committed with each folder is `process-monitor.log` and `WINDOW`.

## 5. Development procedure (D1 only; mechanical; `est5 fit`)

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

Baselines refitted on v5 D1 with v3's definitions: F1 (Gaussian random-walk
Kalman, v3 grid q_proc = 10^k, k = 1.0..7.0 by 0.25; r = 10^k, k = 2.0..6.0 by
0.25) and E0 (D1 histogram of horizon-1 changes, v3 section 6).

In-sample screen: each family scored on D1 with the section 6 rules.
Selection: among screen passers, the highest D1 one-step mean log score; any
family within 0.01 nats/step of it that comes earlier in G1 < G2 < G3 is
preferred. If no family passes in sample, Phase A ends PHASE_A_FAIL with each
family's first failing rule and its value, and **D2 is NOT_RUN** (not
collected). The fit writes `receipts/est-v5/params.txt` (all parameters, D1
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
`est5`): from the belief after each scored valid tick t, the horizon-10
predictive is scored against tick t + 10 if that tick is valid. Implementation: `c3_judge` / `c3_stats` (v3 code,
unchanged). Sharpness (central 80 % width) is reported for S, F1 and E0.

`ESTIMATION_CALIBRATION (v5) = PASS` when the selected family S is
calibrated on D2 AND its D2 mean one-step log score is no worse than F1's and
no worse than E0's by more than 0.01 nats/step each, on the same scored steps.
Calibrated but losing a comparison: HELD_OUT_FAIL (not better than a
baseline). Not calibrated: HELD_OUT_FAIL with the first failing statistic.

## 7. INCONCLUSIVE rule (before any D2 thermal value is scored)

v3 section 7, unchanged (`c3_precheck`): fewer than 2030 lines; more than 2 %
of gaps above 1.5 s; marks not exactly one begin/end pair per scheduled
level > 0 segment in order; mean foreign busy cores above 1.5 or more than
10 % of samples above 3.0; foreign load unmeasurable on more than 1 % of
samples.

v5 addition (window protection only): a collection whose `WINDOW` is
WINDOW_VOID, or that has no `WINDOW` and `process-monitor.log` (a collection
by any other collector), is INCONCLUSIVE before any thermal value is read; the
est5 pre-check refuses such a folder. The decision is made from the process
monitor alone and is recorded before the fit or the recorded run.

After an INCONCLUSIVE D2, a new D2 may be collected with seed 0xD2E5C6 + n
(n = 1, 2) under this protocol; the inconclusive folder stays on record
unscored. A third INCONCLUSIVE ends v5 as INCONCLUSIVE. The same pre-check is
run on D1 before the fit; an INCONCLUSIVE D1 is recollected with seed
0xE5C5D1 + n (n = 1, 2) and stays on record unfitted.

## 8. Order of operations

1. Commit the v5 tool, collector, window scripts, tests and this protocol
   (freeze). Record this file's SHA-256 in the PR.
2. Take the flag (section 4), collect D1 under the enforced window, release the
   flag. Commit the raw folder and `receipts/est-v5/d1.sha256`.
3. `est5 precheck` on D1. If VALID, build `est5` from that clean tree and run
   `est5 fit` once; commit `receipts/est-v5/params.txt`.
4. PHASE_A_FAIL: D2 NOT_RUN; go to 6.
5. Phase A PASS: take the flag again, collect D2 in a fresh run under the
   enforced window, release the flag; commit the folder and
   `receipts/est-v5/d2.sha256`; rebuild from the clean tree (D2, params SHAs
   compiled in); `est5 precheck`, then exactly one `est5 recorded` into
   `receipts/est-v5/`. The receipt is named by its SHA-256 and records tool
   commit, protocol, params and data SHAs; the tool refuses a dirty tree and a
   second receipt.
6. Write `receipts/est-v5/RESULT.md` by hand from params.txt / the receipt.

## 9. What this protocol does not allow

Changing families, formulas, grids, bands, burn-in, the interval rule, the window rules, the process-monitor patterns, the voiding rule,
the two-stage fit or the selection rule after D1 has been collected; fitting on
anything but D1; reading D2 thermal values before the recorded run; refitting
after seeing D2; dropping outliers; adding a family; a second scored run on
D2; tuning anything on D2. After a FAIL, the v5 finding is stated from the
recorded numbers without refitting.

## 10. Downstream gating

EST-4 (World belief state) and EST-5 (uncertainty-aware cost model) are
unblocked only by a v5 PASS, using S with its frozen parameters; this
protocol does not start them. After a FAIL or INCONCLUSIVE nothing downstream
may treat any estimator output as calibrated.
