# ESTIMATION v6 protocol PROPOSAL (not frozen)

Workstream: ARCH-0020 (belief / estimation layer), ESTIMATION v6.

**Status: PROPOSAL.** Merging this file is not a freeze. v6 freezes only by a later
commit that (a) fills the items marked `FILL AT FREEZE` from the pre-freeze development
work of section 9, (b) changes this status line to `FROZEN`, and (c) is made before any
v6 data is collected. The SHA-256 of the frozen file is then compiled into the v6 tool
(`tools/estimation/est6.c`, not written yet) and written into params.txt and every v6
receipt. Any edit after the freeze is a new protocol version (v7).

No data is collected, no tool is built and no threshold is chosen from any held-out
number by this proposal. The only data it reads are the development sets named in
section 3 and the summary numbers already published in
`receipts/est-v5/RESULT.md`, `params.txt` and the receipt.

## 1. v5 stays a closed failure

v5 is `ESTIMATION_CALIBRATION (v5) = HELD_OUT_FAIL`, first failing statistic `pit_bin0`
(0.06812500964404912 against a lower limit of 0.07, 2651 scored steps), receipt
`receipt-7f0bb7018d066bc438e7a8a9bf87c0d45615f60be2d029b6ab5194168e3db3d1.json`
(protocol sha256 `275cd5b7...187af5`, params sha256 `6be2b579...c2aa3a`). Nothing here
reopens it. v5 is not rescored, its bands are not reinterpreted as a pass, "within
noise" is not a v5 finding, and v6 does not inherit a v5 pass. EST-3 stays FAILED
(v1 to v3), v4 stays INCONCLUSIVE, EST-4 and EST-5 stay blocked until a v6 PASS.

The v5 held-out set D2 (`evidence/EST5/raw/20261005T112838Z-est5-heldout-silicon/`,
raw sha256 `7dda21f664587c4401326c3082f28039e133ba5627c8564678f259e2d7036ff0`) is
**forbidden** in v6 for every role: fitting, selection, cross-validation, threshold or
design-effect calibration, power analysis, or a held-out claim. The v6 tool refuses its
path and its SHA-256, as the v5 tool refused the v4 held-out paths. Reading the
published v5 summary numbers (quoted in section 2) to describe a problem is the only
use.

## 2. Findings that motivate v6

Data used for sections 2a and 2b: v5 D1 (`evidence/EST5/raw/20261001T153156Z-est5-fit-silicon/`,
development data of a closed protocol, never held out) and the in-sample numbers in
`params.txt`; plus the published D2 summary numbers listed. D2 raw data was not opened.
No filter was run on any data (the machine was under a chip measurement window); the
checks below are arithmetic and one light pass over D1 readings.

### 2a. The lower-tail shortfall is probably real, not noise

Published facts (PROVEN, they are recorded numbers):
- v5 D1 in-sample, G1: pit_bin0 0.0760 (the smallest of the ten bins; next smallest
  0.0839), pit_bin9 0.1225, bias mean z +0.046 (`params.txt`).
- v5 D2, G1: pit_bin0 0.0681, pit_bin9 0.1100, bias mean z +0.068, lag-1 z +0.104,
  coverage50 0.5326, ten-step coverage95 0.9227 (receipt).
- The same direction on two independent runs: lowest decile under-filled, bias
  positive.

Sampling arithmetic (PROVEN as arithmetic, assumptions stated):
- With independent steps and a correct model, a bin share at 0.10 has standard error
  sqrt(0.1 x 0.9 / 2651) = 0.0058. The v5 limit 0.07 is 5.2 standard errors away, and
  0.0681 is 5.5 away. D1's 0.0760 is 4.1 away.
- Steps are not independent. The published D2 lag-1 z of 0.104 is 5.4 standard errors
  from zero under independence, so there is serial dependence. A first-order
  correction (1 + rho) / (1 - rho) is about 1.23, standard error about 0.0065, and
  0.0681 is still about 4.9 away. For the shortfall to be noise, the effective sample
  would need to be about 7 times smaller than n (design effect about 7.5, which would
  put 0.0681 only 1.9 standard errors low). Nothing published supports that, and it
  has not been measured: whole-segment clustering of the load schedule (39 and 40
  segments) is the one thing the lag-1 check cannot see. **Design effect from
  clustering: UNVERIFIED** (section 9 measures it on development data).

Conclusion on strength: the bin0 deficit is **LIKELY systematic** (two runs, same sign,
4 to 5.5 independent standard errors on each, only a large unmeasured clustering effect could
rescue it). Its cause is a separate question (2b).

### 2b. Why the lower tail is under-covered (model behaviour)

G1 puts a symmetric Student-t (nu 1.25, c 0.7 at one step) around the one-step mean
(`protocols/est-v5.md` section 2). Observed one-step changes in v5 D1 (2681 changes, in
quanta of 100 mC; one pass over the readings, development data only):
- 756 up, 812 down, 1113 zero. Mean up 3.98, mean down 3.56. Changes of at least +3:
  240; at most -3: 211. Mean change +0.043.
- After an up change: up 0.42, down 0.35. After a down change: down 0.47, up 0.25.

PROVEN as facts about D1: moves are not symmetric. Up moves are fewer but larger,
down moves more frequent but smaller, and there is a small positive drift.

Reasoning (LIKELY, not tested by a fit): a symmetric predictive centred on the filter's
mean puts as much tail on large drops as on large rises. If real large drops are
rarer and smaller, too many forecasts say "a big drop is plausible", so the lowest
decile of the PIT is under-filled, and the positive mean z (+0.046, +0.068) points the
same way (forecasts centred a little low). A rough check supports the bias part
alone: a mean shift of 0.068 z-units moves a normal bin 9 to about 0.1125 (observed
0.1100) but bin 0 to only about 0.0886 (observed 0.0681), so bias explains bin 9 and
about a third of the bin 0 gap, and the rest needs lower-tail shape. Strength: bias
sign and bin 9 fit **LIKELY**; the lower-tail shape explanation **LIKELY but UNTESTED**.

Other candidates, ranked and unverified:
- Scale adaptation at the grid edge (lambda 0.1 is on the edge of the grid and was
  flagged in `params.txt`): the scale chases the last squared error, so a large up move
  widens the next forecast on both sides. UNVERIFIED.
- Very heavy tail (nu 1.25): sound for log score on D1, but it spends probability on
  distant low outcomes. UNVERIFIED.
- Fit objective: parameters maximise mean log score, not tail balance. D1's own screen
  passed pit_bin0 by only 0.006, the thinnest margin of any gated statistic on D1
  (bins 7 and 8 were 0.014 above), so the v5 selection already carried a fragile pass
  into D2. PROVEN as a margin fact; that fragility predicted trouble out of sample.

One thing the data cannot say: whether the 95 % interval misses are lower or upper
misses. v5 reports total coverage only. v6 reports and gates them separately.

## 3. Data identities (all fresh, plus declared development data)

| set | role | identity | status |
|---|---|---|---|
| v3 A, B, C1, v3 D1 | development only | as v5 section 3 | spent, never held out |
| v5 D1 | **development, and part of the fit pool** | `evidence/EST5/raw/20261001T153156Z-est5-fit-silicon/`, raw sha256 `7da6e4950c333bae65e5663c32c9cfe9de6657f0ff88c55e9b9e767152634465` | fit data of closed v5; never held out; allowed in v6 fit |
| v5 D2 | **FORBIDDEN** | see section 1 | closed held-out, never reused |
| v6 F | **fit** | `evidence/EST6/raw/<UTC>-est6-fit-silicon/`, seed 0xE6C6D1, 2700 s | collected only after freeze |
| v6 H1, H2 | **sealed held-out** (two separate runs, pooled for the one scoring) | `.../<UTC>-est6-heldout-silicon/`, seeds 0xD6E6C7 and 0xD6E6C8, 2700 s each | collected only after a Phase A PASS |

Seed rule. Every v6 seed is declared here, before any data. Each differs from every
earlier seed in the estimation docs (0xE5C3D1, 0xE5C4D1/2/3, 0xE5C5D1/2, 0xD2E5C3 to
0xD2E5C6). The schedule is derived from the seed alone (splitmix64, `est_load.c`) and
its exact segment list is written into the frozen file (`FILL AT FREEZE`, computed by
the existing load generator, no measurement needed). Balance rule, checked on the
derived schedule before any collection: each of idle, L6, L12, L18 totals between 400
and 900 s. If a declared seed fails the balance rule, the next seed (+1) is used; this
is decided from the schedule alone and recorded at freeze, never after seeing data.
Recollection after an INCONCLUSIVE window uses seed + 0x10 times n (n = 1, 2), same rule.
All v6 raw data is committed with its SHA-256 identity (`f.sha256`, `h1.sha256`,
`h2.sha256`) before it is fitted or scored, as in v5 section 3.

Why two held-out runs: with one 2700 s run the held-out sample is about 2650 steps; the
sampling error of any bin share is then about 0.006 to 0.016 depending on clustering
(2a). Two independent runs double the sample and allow a between-run consistency check.
Cost: one more quiet window of about 45 minutes. Alternative if Drake prefers a single
run: keep one run and apply the section 6 sample-size rule, which may then stop v6
before collection because the operating characteristics are inadequate.

## 4. Measurement isolation (quiet window rules)

All v5 section 4 rules apply unchanged (flag with `gate est-v6`, machine-wide
`quiet-guard` hook, collector refusal, `process-monitor.log`, `WINDOW`). Additions, each
from a v5 or v6-preparation lesson:

1. Scheduling. A v6 collection window is booked only when `quietlock check` shows the
   flag free and no other holder has an expected end inside the window. v6 collection
   never overlaps a chip measurement window (the machine currently holds one, which is
   why this proposal ran no tools).
2. Pre-window sweep, recorded in the folder: every other session acknowledges stopped
   builds, tests, QEMU and GPU work; idle `clangd` and other language servers are
   stopped; `ps` snapshot at start and end. A foreign process found at start refuses the
   run (v5 already refused for `clangd`).
3. v5 attempt 1 was voided because the hook only reaches sessions that load it.
   v6 therefore also counts foreign CPU from `/proc/stat` against the declared load
   (the v3 pre-check, unchanged thresholds) and treats a missing `WINDOW` file as
   WINDOW_VOID.
4. Thermal start state: 300 s of idle before the first load segment, start
   temperature recorded in `machine-state-start.txt`. Descriptive only; it moves no
   score. (v3 forensics found a thermal ramp that persists across a run.)
5. The same rule applies to every window: a void is decided from the monitor alone,
   before any thermal value is read.

## 5. Candidate families and model-selection rules

Reused unchanged from v5 (library `est_v4.c` hashes in v5 section 1): G1, G2, G3, and
baselines F1 and E0 refitted on the fit pool. New, one addition aimed at the finding in 2b:

- **G1S** (G1 with a skew parameter): same filter as G1; the predictive is a
  two-piece Student-t, scale c on the upper side and c x kappa on the lower side,
  for kappa in {0.6, 0.7, 0.8, 0.9, 1.0}. kappa = 1.0 is G1 exactly, so the extra
  freedom is one parameter. Same split at stage 2 for the ten-step law with its own
  kappa_h on the same grid. The location stays the filter mean. Grids otherwise v5
  section 5. The estimator code change (a new `est_v6.c`, hashes recorded at freeze)
  must reproduce G1 bit for bit at kappa = 1.0 on the v5 D1 (a development check, not
  a measurement).
- Not adopted, to limit change: regime-conditioned scale using declared load marks
  (it feeds the declared schedule to the estimator, which is a different claim), two-
  state sensor-stall models, any family with more than one new parameter. They may be
  proposed for v7 after a v6 result.

Fit pool: v5 D1 plus v6 F, fitted by the v5 two-stage grid (section 5 there), with
these changes and nothing else:

1. **Block cross-validation for selection.** The fit pool is split by whole load
   segment (the schedule's segments, never mid-segment) into 5 folds, fixed assignment
   by segment index modulo 5. For each family the grid is searched on 4 folds and the
   out-of-fold steps are scored; selection uses the pooled out-of-fold one-step mean
   log score, not the in-sample one. The final parameters are refitted on the whole
   pool once.
2. **Screen with margin.** A family passes the in-sample screen only if its pooled
   out-of-fold statistics pass every primary gate of section 6 against **inner bands**
   (each band shrunk by 0.01 on both sides for share and coverage-rate gates, 0.02 for
   the 95 % coverage gate, 0.03 on the ten-step gate). This is a development rule that
   rejects fragile fits like D1's 0.006 margin on pit_bin0. It does not change the
   held-out bands.
3. Selection among screen passers: highest out-of-fold one-step mean log score;
   an earlier family in G1 < G2 < G3 < G1S within 0.01 nats is preferred (v5 rule
   with G1S last, so the simpler model wins ties).
4. If no family passes: PHASE_A_FAIL, held-out NOT_RUN, nothing collected.
5. A lambda, q or other parameter on the edge of its grid is a recorded flag. At
   freeze the lambda grid is extended downwards to {0.05, 0.1, 0.2, ...}; a
   parameter still on the edge after the fit is reported and does not alter the
   rule.

The selected model, parameters and params.txt are committed before held-out collection
and are binding.

## 6. Scoring rules and thresholds, with justification

The v5 practical bands are kept. They are not loosened: a v6 PASS must clear the same
bar v5 set, so a failed bar is never answered by a lower bar. What changes is the number
of decisions, how serial dependence is handled, and what a near miss is called.

Primary gates (decide the verdict), all on the pooled H1 + H2 steps after the 30-tick
burn-in of each run (steps counted per v3 rules):

| id | statistic | pass band | why |
|---|---|---|---|
| P1 | one-step fractional coverage 50 % | [0.46, 0.54] | v5, unchanged |
| P2 | coverage 80 % | [0.76, 0.84] | v5, unchanged |
| P3 | coverage 95 % | [0.93, 0.97] | v5, unchanged |
| P4 | lower miss rate: share of outcomes below the lower 2.5 % quantile | [0.010, 0.040] | new; the v5 finding is a lower-tail one, total coverage hides which side misses |
| P5 | upper miss rate: share above the upper 2.5 % quantile | [0.010, 0.040] | new; same reason |
| P6 | each of 10 randomised-PIT bins | [0.07, 0.13] each | v5 bins, unchanged; 10 bins count as one gate P6 (all must pass) |
| P7 | ten-step coverage 95 % | [0.90, 0.99] | v5, unchanged |
| P8 | |mean z| <= 0.10 and |lag-1 z| <= 0.20 | as v5 | v5 bands, but computed on **randomised PIT** (below) |
| P9 | each quarter and each regime with >= 100 steps: coverage95 | [0.90, 0.99] | v5, unchanged |
| P10 | n scored one-step steps | >= 4000 (two runs) | the v5 2000 floor scaled to two runs |

Randomised PIT: u = F_lo + w x (F_hi - F_lo) with w drawn from a fixed-seed
generator declared at freeze (seed in the file). It is exactly uniform for a correct
discrete predictive, so z and the bins have no tie-shape artefact. This addresses the
review's note that mid-PIT z checks are only descriptive on a discrete predictive. The
v5 mid-PIT numbers are still reported.

Baseline comparison. S must have mean one-step log score at least F1's and no worse
than E0's by more than 0.01 nats on the same steps (v5 rule). New: the paired
difference S minus each baseline gets a block bootstrap (resampling whole load
segments, 2000 draws, fixed seed) and its 95 % lower bound is reported; the rule stays
point-estimate based. This answers the review's note that the F1 comparison had no
sampling allowance without making the bar harder.

Block bootstrap intervals. For every primary gate the tool reports a 99.5 % interval
by resampling whole load segments of H1 and H2 (about 79 blocks; seed fixed). 99.5 % is
Bonferroni at family error 0.05 over about 10 primary families.

Verdict (three outcomes, decided mechanically):
- `PASS`: every primary gate's point value is inside its band, S beats the baselines
  as above. (The same bar as v5, on more data and fewer, sharper gates.)
- `HELD_OUT_FAIL`: at least one primary gate's value is outside its band **and** the
  99.5 % block interval lies entirely outside the band edge. Calibration shown wrong.
- `NOT_ESTABLISHED`: a gate's value is outside its band but its 99.5 % interval still
  reaches into the band. The run neither shows nor refutes calibration. No downstream
  use, same as a failure. It exists so that a near miss like v5's is reported as
  "cannot tell, and too little data" instead of being rounded either way. (In v5 terms:
  0.0681 with an interval almost certainly overlapping 0.07 would have been
  NOT_ESTABLISHED under clustering and HELD_OUT_FAIL without; section 9 measures which.)

Operating characteristics before freeze (the review's main limit). Using only
development data, the freeze commit must contain a simulation table (false-pass and
false-fail rates under (a) a perfectly calibrated forecaster, (b) a nominal 95 % that
truly covers 92 %, (c) a forecaster with the v5 lower-tail shortfall), with the
clustering found in section 9. Required: false-fail under (a) at most 10 %, pass under
(b) at most 10 %. If the table fails either, the held-out run length is increased until
it meets them (the length is part of the freeze), or v6 does not start.

## 7. Claim v6 may make

A v6 PASS licenses exactly: on new runs from the specified load generator, the frozen
forecasting procedure met the declared accuracy, coverage, tail-balance and comparison
thresholds, at the stated operating characteristics. It does not license
"statistically validated 95 % calibration", other workloads, other ambient conditions
or boards, horizons other than 1 and 10 s, physically identified thermal parameters, or
safety guarantees. These are the review's stated limits (section 8).

## 8. The review's limits and what v6 does about each

Source: the external statistical review (Astra via OpenCode) quoted in
`receipts/est-v5/RESULT.md`, "Limits". Limits it states:

| # | limit stated by the review | v6 response |
|---|---|---|
| 1 | PASS is a predeclared operational check, not a certificate of calibration | section 7 claim wording kept; PASS wording unchanged in strength |
| 2 | overall false-pass and false-fail rates unknown; a true 92 % 95 % interval can pass about 88 % of the time | section 6 operating-characteristics table is required before freeze, with false-fail and pass-at-92 % limits of 10 % each |
| 3 | mid-PIT z checks (bias, lag-1) are descriptive on a discrete predictive | P8 on randomised PIT |
| 4 | the F1 comparison has no sampling allowance | block-bootstrap interval reported; rule stays point-based |
| 5 | scope: same generator only, 1 and 10 s horizons, one board and ambient | unchanged and stated in section 7; not addressed by data |

Limits found while preparing v6 (not in the review, labelled as such):

| # | limit | v6 response |
|---|---|---|
| 6 | 23 gates in v5 with no family-wise rule; the 10 bins and 4 quarters are dependent gates | 10 primary families (P1 to P10); bins and quarters grouped; 99.5 % intervals |
| 7 | independent-step standard errors are wrong; serial and segment dependence exist (D2 lag-1 z 0.104) | block bootstrap by load segment, clustering measured on development data before freeze (section 9) |
| 8 | the 0.07 bound is 5 independent standard errors from 0.10 at n 2651 yet fell at 0.0681; its meaning depends on unmeasured clustering | three-way verdict; two held-out runs; operating characteristics |
| 9 | the in-sample screen passed pit_bin0 with a 0.006 margin and was not out of sample | block cross-validation and inner bands (section 5) |
| 10 | selection used log score alone; tails were not part of the objective | tail-balance gates P4, P5 in the screen, and G1S for the observed asymmetry |
| 11 | total 95 % coverage hides which side misses | P4 and P5 |

## 9. Pre-freeze development work (D1 and earlier development data only)

To be done after the current chip window ends (no tool run is allowed during it), by a
lane that has read this file, and written into an appendix of the freeze commit:

1. Rerun the v5 G1 on v5 D1 with the existing tool; report the PIT bin shares and the
   lower and upper miss rates separately; block-bootstrap them by load segment to
   measure the clustering (design effect) of the bin 0 share. Repeat on v3 D1.
2. Fit G1S on v5 D1 alone with block cross-validation. Report whether kappa below 1
   fixes the lower bin out of fold, and by how much. If kappa stays at 1.0 and bin 0
   stays low, the lower-tail cause is not asymmetry and section 2b's main hypothesis
   is rejected; the freeze then takes a different remedy or v6 stops.
3. Produce the operating-characteristics simulation of section 6.
4. Derive and record the exact schedules for the declared seeds and the balance
   check.
5. Record the v6 tool, library and test hashes, `crumb compile && crumb verify`, then
   the freeze commit. Only then collect F.

## 10. Order of operations after the freeze

1. Take the flag (section 4); collect F; release; commit raw folder and `f.sha256`.
2. `est6 precheck` on F (v3 pre-check thresholds, plus WINDOW). If VALID, `est6 fit`
   once on the pool (block CV); commit `receipts/est-v6/params.txt`. PHASE_A_FAIL stops
   v6 with no held-out collection.
3. Phase A PASS: collect H1 and H2 in two separate quiet windows; commit folders and
   `h1.sha256`, `h2.sha256`; rebuild from the clean tree with identities compiled
   in; `est6 precheck`, then exactly one `est6 recorded`. The tool refuses a dirty
   tree, a second receipt, and any v5 D2 path or hash.
4. Receipts are written as strict JSON (no bare inf or nan; the v5 receipt carries
   them, and a parallel fix is in flight on `cand2/est-receipt-json`).
5. Write `receipts/est-v6/RESULT.md` by hand from params.txt and the receipt.

## 11. What this protocol does not allow after freeze

Changing families, grids, bands, inner bands, the verdict rule, the seeds, the
operating-characteristics requirement, the window rules or the voiding rule after F is
collected; fitting on any held-out data; using v5 D2 in any role; opening H1 or H2
thermal values before the recorded run; refitting after seeing H1 or H2; dropping
outliers; adding a family; a second scored run; tuning anything on H1 or H2. After a
FAIL or NOT_ESTABLISHED the v6 finding is stated from the recorded numbers without
refitting.

## 12. Downstream gating

EST-4 and EST-5 unblock only on a v6 `PASS`, using S with its frozen parameters; this
protocol does not start them. After HELD_OUT_FAIL, NOT_ESTABLISHED or INCONCLUSIVE
nothing downstream may treat any estimator output as calibrated.
