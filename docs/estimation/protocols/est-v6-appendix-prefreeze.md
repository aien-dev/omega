# ESTIMATION v6 pre-freeze development appendix (D1 only, NOT FROZEN)

Companion to `est-v6.md` (a PROPOSAL, sha256 in section 7). This appendix is
the development work of section 9 of that file, items 1 to 4. It is not a freeze, not a v6 result and
says nothing about held-out performance. It changes no v5 threshold, parameter, receipt or tool and
no v6 protocol text. Item 5 (hashes, freeze commit) is not done here.

Tool commit for every command below: `ec0b815ad4d475d640781ee9a95bd45621037ecf` (adds
`tools/estimation/est6dev.c`, `est6opchar.c`, `est6opchar_table.sh`, `test_est6dev.sh`,
`mk/estimation_v6dev.mk`; sources sha256 are in section 7). Machine: quiet flag free at every run
(`quietlock check` returned 0 before each build and long run); no collection, no chip window, no
`est5_collect`.

## 1. Data used and not used

Read (development data only):

| input | sha256 |
|---|---|
| v5 D1 `evidence/EST5/raw/20261001T153156Z-est5-fit-silicon/machine-state.ndjson` | `7da6e4950c333bae65e5663c32c9cfe9de6657f0ff88c55e9b9e767152634465` |
| v5 D1 `machine-state-marks.txt` | `6cd7fda33d877a07e6fd21423f0ebc425c616c4d029f3575165ed6e32a02f748` |
| v5 D1 `schedule.txt` | `061e287c76b6878caa675833225c98df601aa15fe4231254a1bf1ab086fcb417` |
| v3 D1 `evidence/EST3C/raw/20261001T025159Z-est3c-fit-silicon/machine-state.ndjson` | `9d5473b56dbb3765e5fc5335ef74aec47a11485651bcffc75272062ab48dc230` |
| v3 D1 `machine-state-marks.txt` | `4dfd06ec3f07c96eefe9697da2809634a0d4db2bfe4718909f2d0ca2c23d7a72` |

Also read: `docs/estimation/protocols/est-v5.md`, `est-v6.md`, `receipts/est-v5/params.txt` (D1 fit
numbers), `receipts/est-v5/d1.sha256`, and, for the `est5` build only, the hash text of
`receipts/est-v5/d2.sha256` that `mk/estimation_v5.mk` compiles into the binary (hashes, not data).
`docs/estimation/RECEIPT_JSON.md` is not on main (the receipt JSON fix is on another branch) and was
not read.

Never read: any v5 D2 file (raw, marks, schedule), any held-out folder of any version. Check: every
data-tool run below was executed under `strace -f -e trace=openat`; the opened `evidence/` paths were
exactly the five D1 and v3 D1 files above plus their `SHA256SUMS`, and `grep -c "20261005T112838Z\|heldout"`
over the four tool traces is 0. The one literal `est5 fit` rerun (section 2) also lists D1 `WINDOW` and
`process-monitor.log`. Its trace shows the `git status` that `est5` runs on itself stat-ing and
listing the names inside the D2 folder (directory entries only); no D2 file was opened or read. The
est6dev tool and its test refuse held-out path tags and the D2 date tag before opening anything and refuse
an input whose SHA-256 is the D2 raw SHA.

## 2. Item 1: v5 G1 on v5 D1 and v3 D1

2a. Existing tool rerun. `make est5` (clean tree at e9c2829), then
`est5 fit --raw <v5 D1 ndjson> --marks <v5 D1 marks> --out /tmp/est5-rerun-params.txt` (not written to the repo).
Result: `EST5_FIT selected=G1 phase_a=PASS`. The new params file differs from
`receipts/est-v5/params.txt` in exactly one line, `tool_commit` (e9c2829 against d7fbc5c); every
parameter, score, screen value and grid-edge flag is identical. So the v5 fit reproduces
bit for bit on v5 D1 with the existing tool.

2b. PIT bins, lower and upper miss rates, block bootstrap. The existing tool has no mode that scores
fixed parameters and prints per-step PIT, so `est6dev g1pit` runs the unchanged v5 library
(`src/estimation/est_v4.c`) with the selected G1 parameters (dyn 1, q 10000, lam 0.1, nu 1.25, c 0.7), same
classification, burn-in and scoring loop as `est5.c`, and reproduces `params.txt` exactly (n 2652, pit_bin0
0.075981, coverage95 0.952950, mean z 0.046045; checked by `make test-est6dev`).

```
est6dev g1pit --raw <D1>/machine-state.ndjson --marks <D1>/machine-state-marks.txt \
  --dyn 1 --q 10000 --lam 0.1 --nu 1.25 --c 0.7 --blocks
```

Lower miss rate = expected share of outcomes below the 2.5 % quantile of the predictive (fractional,
as the v5 bins); upper the same above 97.5 %. Blocks = whole load segments: each marked load segment,
and each idle stretch between marked segments (adjacent idle schedule segments leave no marks and are
one block). Resampling: whole blocks with replacement, 2000 draws, seed 0xE6B007, ratio estimator.
Design effect = bootstrap variance over the independent-step variance p(1-p)/n.

v5 D1, n 2652, 35 blocks (27 loaded, 8 idle):

| statistic | value | iid SE | block SE | design effect | 95 % interval | 99.5 % interval |
|---|---|---|---|---|---|---|
| pit_bin0 | 0.0760 | 0.0051 | 0.0089 | 3.0 | 0.0577 to 0.0933 | 0.0508 to 0.0977 |
| pit_bin1 | 0.1151 | 0.0062 | 0.0054 | 0.8 | 0.1036 to 0.1250 | 0.0983 to 0.1291 |
| pit_bin2 to bin8 | 0.0839 to 0.1081 | | 0.0031 to 0.0053 | 0.3 to 0.9 | | |
| pit_bin9 | 0.1225 | 0.0064 | 0.0087 | 1.9 | 0.1061 to 0.1403 | 0.1001 to 0.1480 |
| lower miss 2.5 % | 0.0122 | 0.0021 | 0.0028 | 1.7 | 0.0072 to 0.0179 | 0.0049 to 0.0204 |
| upper miss 2.5 % | 0.0349 | 0.0036 | 0.0038 | 1.1 | 0.0276 to 0.0426 | 0.0248 to 0.0456 |
| coverage95 | 0.9530 | 0.0041 | 0.0046 | 1.3 | 0.9436 to 0.9616 | 0.9400 to 0.9654 |

v3 D1 (same v5 G1 parameters, not refitted; v3 D1 was never fit data for v4 or v5), n 2651, 39 blocks:

| statistic | value | iid SE | block SE | design effect | 99.5 % interval |
|---|---|---|---|---|---|
| pit_bin0 | 0.0602 | 0.0046 | 0.0094 | 4.2 | 0.0359 to 0.0881 |
| pit_bin9 | 0.0920 | 0.0056 | 0.0077 | 1.9 | 0.0702 to 0.1125 |
| lower miss 2.5 % | 0.0132 | 0.0022 | 0.0026 | 1.4 | 0.0070 to 0.0214 |
| upper miss 2.5 % | 0.0234 | 0.0029 | 0.0034 | 1.4 | 0.0143 to 0.0335 |
| coverage95 | 0.9634 | 0.0036 | 0.0043 | 1.4 | 0.9503 to 0.9739 |

Findings (each from the numbers above; tags as in v6 section 2).
- PROVEN, bin 0 clustering: design effect 3.0 on v5 D1 and 4.2 on v3 D1, measured with 35 and 39 blocks
  (so themselves noisy). Section 2a asked whether it is near 7.5 (the value needed for the shortfall to be
  noise). It is not, on either set. With these design effects one run's bin share at 0.10 has a standard
  error of 0.010 to 0.012, and the 0.07 limit is 2.5 to 3 standard errors away (5.2 under independence).
  Pooling two runs (n about 5300, about 75 blocks) the standard error falls to about 0.007 to 0.008.
- PROVEN, G1 with the v5 parameters puts bin 0 below 0.07 on v3 D1 (0.0602) where the parameters
  were not fitted. The shortfall is not specific to the D2 run: v5 D1 in sample 0.0760, v3 D1 0.0602.
- PROVEN, the lower tail is not light in the 2.5 % sense: lower miss 0.0122 and 0.0132 are just above the
  proposed P4 floor of 0.010 on both sets, upper miss 0.0349 and 0.0234. Lower miss is thin on v5 D1
  (0.0122 against a floor of 0.010; 99.5 % interval 0.0049 to 0.0204 reaches below it).
- PROVEN, the PIT is regime dependent, which a global skew cannot reproduce. By declared load level
  (descriptive, from `est6dev g1pit`):

| level | v5 D1 steps | bin0 | bin9 | lower miss | upper miss | v3 D1 steps | bin0 | bin9 |
|---|---|---|---|---|---|---|---|---|
| idle | 818 | 0.116 | 0.077 | 0.030 | 0.036 | 235 | 0.152 | 0.086 |
| L6 | 739 | 0.111 | 0.114 | 0.010 | 0.017 | 672 | 0.127 | 0.102 |
| L12 | 970 | 0.025 | 0.159 | 0.000 | 0.041 | 1079 | 0.025 | 0.091 |
| L18 | 125 | 0.000 | 0.184 | 0.000 | 0.085 | 665 | 0.018 | 0.087 |

  Bin 0 is over-filled when idle or at L6 (above 0.11) and almost empty at L12 and L18. The forecast under-reads
  heating under load (outcomes land above the median) and over-reads cooling. The pooled 0.076 is a mixture. LIKELY
  (not tested by a fit): the dominant cause of the pooled bin 0 and bin 9 deviations is regime-dependent bias
  (the model lags load changes), not a symmetric-tail problem. The section 2b hypothesis (asymmetric tails) is at
  best a partial explanation (section 4 below).

## 3. Item 4: exact schedules for the declared seeds and the balance check

`est6dev sched <seed> 2700` implements the `est_load.c` draw order (splitmix64, level = draw mod 4 over
{0,6,12,18}, length = 20 + draw mod 101, last segment clipped), without running any load. Validation:
`est6dev sched 0xE5C5D1 2700` reproduces the schedule printed in `est-v5.md` section 4 (39 segments; idle 406,
L6 929, L12 655, L18 710; first segment 0/74), and `est6dev sched 0xE5C5D2 2700` reproduces the recorded
`schedule.txt` of the v5 D1 folder line for line (checked in `make test-est6dev`). Side finding: the v5 D1
folder that was fitted was collected with seed 0xE5C5D2 (the seed + 1 recollection of v5 section 7), not the
0xE5C5D1 whose schedule `est-v5.md` section 4 prints.

Balance rule of v6 section 3 (each of idle, L6, L12, L18 between 400 and 900 s), applied literally and
repeatedly (+1 until a seed passes), command `est6dev pick <seed> 2700`:

| role | declared seed | schedule at declared seed (segments; idle, L6, L12, L18 s) | balance | seed after rule |
|---|---|---|---|---|
| v6 F | 0xE6C6D1 | 36; 368, 518, 1271, 543 | FAIL (idle, L6, L12) | 0xE6C6D2 fails (938, 738, 374, 650), **0xE6C6D3** passes (36 segments; 460, 896, 606, 738) |
| v6 H1 | 0xD6E6C7 | 38; 257, 909, 836, 698 | FAIL (idle, L6) | 0xD6E6C8, C9, CA fail, **0xD6E6CB** passes (41 segments; 522, 572, 857, 749) |
| v6 H2 | 0xD6E6C8 | 36; 411, 686, 543, 1060 | FAIL (L18) | 0xD6E6C9, CA fail, **0xD6E6CB** passes |

Exact segment lists (level/seconds) for the seeds that pass, from `est6dev sched`:
- F, 0xE6C6D3: 18/106 12/107 18/84 12/118 6/36 6/90 18/58 0/90 18/48 18/25 6/84 12/51 6/116 18/88 6/65
  12/117 18/47 0/41 18/116 6/65 6/62 6/35 0/34 18/87 18/79 12/67 12/54 0/104 6/94 0/97 0/86 6/95 6/36 6/118
  12/92 0/8
- H1, 0xD6E6CB: 12/90 0/22 6/62 12/93 18/71 6/97 6/26 18/45 18/52 0/43 18/22 0/64 12/29 0/75 6/79 12/53
  18/112 6/62 18/107 12/102 0/23 18/68 6/23 18/37 6/67 18/91 0/20 0/82 18/115 0/45 0/96 12/50 12/97 12/83 6/95
  12/109 12/98 12/53 0/52 6/61 18/29

**Defect in the proposal, to be fixed at freeze (not decided here).** All three declared seeds fail the balance
rule. Applied literally, H1 and H2 both land on 0xD6E6CB, so the two "independent" held-out runs would have the
identical schedule. A freeze text must say, for example, that each later seed in a chain skips seeds already
used, in which case H2 becomes the next passing seed after 0xD6E6CB, which is **0xD6E6CD** (38 segments; idle 640,
L6 810, L12 569, L18 681; `est6dev pick 0xD6E6CC 2700`). Alternatively choose declared seeds that pass. The
schedule alone decides either way; no data is needed.

## 4. Item 2: G1S fit with block cross-validation (REDUCED, not the full item)

Size assessment of the full item. The protocol's G1S fit is a new model plus the whole fit machinery with
block CV: a two-piece Student-t discretiser that must equal `est_v4.c` bit for bit at kappa 1.0 (a new
`est_v6.c`, about 150 lines with its own t CDF, or an edited copy of `est_v4.c`), segment-fold bookkeeping,
the full G1 grid per fold for each of 5 kappa values (47628 stage-1 points per kappa, six fits), a full-pmf
rescoring path for PIT statistics, and agreement checks with the fast path. That is roughly 500 to 700 lines and
a long grid run, over the 300 line limit set for this session, so the full item is NOT implemented. Design
note for the full item: build `est_v6.c` as `est4_dpred` with side masses 1/(1+kappa) and kappa/(1+kappa)
(density continuous at the location), scale c on the upper side and c kappa on the lower side; reuse the `est5.c`
trace and fast-score structure with a per-step fold mask (fold = segment rank mod 5); search the v5 grid
(dyn, q, lam, nu, c) times kappa in {0.6..1.0} on 4 folds, score the held-out fold, pool; test kappa 1.0 against the
unchanged v5 library bit for bit before any other use.

What was done instead (about 110 lines in `est6dev g1s`): keep the v5 G1 filter parameters (dyn 1, q 10000,
lam 0.1, nu 1.25) fixed and search only kappa in {0.6, 0.7, 0.8, 0.9, 1.0} and c in 0.20 to 1.20 by 5 folds
over blocks (fold = rank of the block among load segments and idle stretches, modulo 5), the two-piece t built
from the unchanged symmetric kernel `est4_fast_logp`. Check: at kappa 1.0 it reproduces the G1 log score
-1.962811 (maximum absolute difference to `est4_fast_logp` 2.4e-11, so not bit for bit; not the final
estimator). Command:

```
est6dev g1s --raw <D1>/machine-state.ndjson --marks <D1>/machine-state-marks.txt --dyn 1 --q 10000 --lam 0.1 --nu 1.25
```

v5 D1, in sample (best c per kappa):

| kappa | c | mean log score | bin0 | bin9 | lower miss | upper miss |
|---|---|---|---|---|---|---|
| 0.6 | 0.90 | -1.9977 | 0.151 | 0.077 | 0.025 | 0.018 |
| 0.7 | 0.80 | -1.9782 | 0.132 | 0.093 | 0.022 | 0.024 |
| 0.8 | 0.75 | -1.9674 | 0.110 | 0.105 | 0.018 | 0.029 |
| 0.9 | 0.70 | -1.9630 | 0.095 | 0.117 | 0.016 | 0.033 |
| 1.0 (G1) | 0.70 | -1.9628 | 0.076 | 0.123 | 0.012 | 0.035 |

Out of fold on v5 D1 (pooled, 2652 steps):

| selection | mean log score | bin0 | bin9 | lower miss | upper miss | kappa chosen in folds 0 to 4 |
|---|---|---|---|---|---|---|
| kappa free, c refit by fold | -1.9649 | 0.0858 | 0.1203 | 0.0135 | 0.0342 | 1.0, 1.0, 0.9, 0.9, 1.0 |
| kappa 1.0, c refit by fold (control) | -1.9634 | 0.0770 | 0.1236 | 0.0122 | 0.0358 | 1.0 in all |

Same procedure on v3 D1 (extra development data, not "v5 D1 alone"): out of fold kappa free: log score
-1.6046, bin0 0.0936, bin9 0.1007, lower miss 0.0186, upper miss 0.0259; control kappa 1.0: -1.6072, bin0 0.0787, bin9
0.1109. In sample on v3 D1, kappa 0.9 is the best log score (-1.6025 against -1.6041 at 1.0).

Reading (PROVEN as numbers, interpretation LIKELY):
- Kappa below 1 does move bin 0 up and bin 9 down, as hypothesised. Out of fold on v5 D1 by +0.009 (0.077 to 0.086);
  on v3 D1 by +0.015 (0.079 to 0.094). Forcing kappa 0.8 to 0.9 gives bin 0 of 0.095 to 0.110 in sample on v5 D1.
- On v5 D1 the log score never prefers kappa below 1 in sample (-1.9628 at 1.0 against -1.9630 at 0.9) and the
  out-of-fold log score is 0.0015 nats worse when kappa is free. On v3 D1 it prefers 0.9 by 0.0016 nats. The
  gains are small against the 0.01 nat tie rule of section 5, which would keep G1 over G1S in both cases.
- The proposed inner-band screen (bins within [0.08, 0.12]) would reject G1 on v5 D1 out of fold (bin 0 0.0770)
  and would pass G1S only barely on bin 0 (0.0858) while failing bin 9 by a hair (0.1203 against 0.12). So on this
  evidence neither family cleanly passes the v6 inner screen on v5 D1 alone.
- Section 9 item 2 asked whether kappa below 1 fixes the lower bin out of fold. Answer: it moves it in the right
  direction by about 0.01, not enough to settle the question, with the tail structure above (regime-dependent
  bias) still unexplained by kappa. The main-hypothesis rejection test ("kappa stays at 1.0 and bin 0 stays low") is not
  met cleanly: kappa 0.9 was chosen in 2 of 5 folds and bin 0 rose. A decision on G1S versus a regime-aware remedy
  (v6 section 5 excluded regime-conditioned scale) needs the full item and belongs to the freeze lane.

## 5. Item 3: operating characteristics of the v6 verdict

Tool: `est6opchar` (reads no data file; `make est6-opchar-table`, 4000 replicates, 1000 bootstrap draws each,
seed 0xE6A001; selftest and determinism in `make test-est6dev`). Simulates the pooled H1 + H2 scoring of P1 to
P10 and the three-way verdict of v6 section 6 on synthetic randomised-PIT sequences:
latent Gaussian z with a per-segment effect (variance share w) and AR(1) within segments (rho 0.02), u = G(Phi(z)).
Segments follow the est_load draw (20 to 120 s, random level), one step per second after 30 s burn-in, two runs of
2700 s (n about 5300, P10 met). Intervals: 99.5 % block-bootstrap percentiles for P1 to P7 and mean z; lag-1 of
z and the P9 quarters and regimes use the point value (this can only raise the chance of HELD_OUT_FAIL).
Ten-step P7 is a separate sequence with within-segment rho 0.9 (an assumption: overlapping windows). The F1/E0 baseline
comparison is assumed met. Clustering w: 0.08 gives a bin-0 design effect of 3.3 (measured 3.0 on v5 D1), 0.12 gives
4.5 (measured 4.2 on v3 D1), 0.20 gives about 7 (the value 2a called necessary for noise); `est6opchar deff`.

Case (a) calibrated forecaster; case (b) nominal 95 % that covers 92 % (scale 1.1195 on z); case (c) a forecaster
with the **v5 D1** PIT cell shares (bin 0 0.0760, bin 9 0.1225, lower miss 0.0122, upper miss 0.0349, from the table
above) and a bin 0 sweep. Case (c) is built from D1 numbers only; the D2 summary numbers are not used (the
protocol forbids D2 for power analysis).

| clustering | runs | (a) calibrated: PASS / NOT_EST / HELD_OUT_FAIL | (b) covers 92 %: PASS / NOT_EST / HELD_OUT_FAIL |
|---|---|---|---|
| w 0.08 (deff about 3.3) | 2 | 0.9928 / 0.0073 / 0.0000 | 0.0025 / 0.5697 / 0.4278 |
| w 0.08 | 3 | 0.9992 / 0.0008 / 0.0000 | 0.0008 / 0.3835 / 0.6158 |
| w 0.12 (deff about 4.5) | 2 | 0.9732 / 0.0230 / 0.0037 | 0.0050 / 0.6535 / 0.3415 |
| w 0.12 | 3 | 0.9932 / 0.0067 / 0.0000 | 0.0020 / 0.5170 / 0.4810 |
| w 0.20 (deff about 7, stress) | 2 | 0.3478 / 0.0250 / 0.6272 | 0.0190 / 0.3220 / 0.6590 |
| w 0.20 | 3 | 0.3262 / 0.0097 / 0.6640 | 0.0135 / 0.2993 / 0.6873 |

Requirement check (v6 section 6: false-fail under (a) at most 10 %, pass under (b) at most 10 %), two runs:
- w 0.08 and 0.12 (the measured range): false-fail, read as not PASS, 0.7 % and 2.7 %; as HELD_OUT_FAIL 0.0 % and
  0.4 %. Pass under (b) 0.25 % and 0.5 %. Both requirements are met with two runs; no length increase is needed
  at these settings.
- w 0.20: fails. Cause: z lag-1 correlation about equals w, so the P8 bound of 0.20 on lag-1 z fails by
  construction (the measured lag-1 z is -0.012 on v5 D1 in sample, so w this large is not supported by D1). The row
  is a stress case, not a finding. It shows the gate set, not the interval rule, fails under very strong
  serial dependence.
- Weak point of (b): a 92 % forecaster is called HELD_OUT_FAIL only 34 to 43 % of the time and NOT_ESTABLISHED
  the rest (it never passes). It is flagged, not hidden, by the three-way verdict; a freeze that wants a
  definite failure for a 92 % forecaster needs more data (three runs: 48 to 62 %).

Case (c), power against a lower-tail shortfall (bin 0 swept, two runs):

| true bin 0 | w 0.08: PASS / NOT_EST / HELD_OUT_FAIL | w 0.12: PASS / NOT_EST / HELD_OUT_FAIL |
|---|---|---|
| 0.100 | 0.872 / 0.128 / 0.000 | 0.820 / 0.177 / 0.004 |
| 0.090 | 0.836 / 0.164 / 0.000 | 0.788 / 0.208 / 0.004 |
| 0.080 | 0.755 / 0.244 / 0.001 | 0.701 / 0.295 / 0.004 |
| 0.076 (v5 D1 in sample) | 0.678 / 0.321 / 0.001 | 0.619 / 0.377 / 0.005 |
| 0.070 | 0.411 / 0.581 / 0.009 | 0.392 / 0.594 / 0.014 |
| 0.065 | 0.152 / 0.798 / 0.051 | 0.171 / 0.780 / 0.049 |
| 0.060 | 0.025 / 0.768 / 0.208 | 0.034 / 0.806 / 0.161 |

Reading. A forecaster whose true bin 0 is 0.076 passes only 62 to 68 % of the time, and one at 0.070 only about 40 %,
because the other P gates (P4 floor on the lower miss, bin 9, P9) fail at the same time; it is called a definite fail
almost never. The verdict gives HELD_OUT_FAIL for a bin 0 as low as 0.065 in only 5 % of replicates; a shortfall of
that size would mostly be reported NOT_ESTABLISHED. That is the intended behaviour of the third outcome, and it also
means v6 as drafted rarely certifies a bin-0 shortfall as wrong. (c) with the D1 cells also embeds D1's lower miss
rate of 0.0122, which sits near the P4 floor of 0.010.

Limits of the simulation: the data-generating model is a Gaussian copula with a segment effect, not the
forecaster; real clustering is partly a regime bias (section 2) that is not noise and not random across replicates;
P7 is crude; the baseline comparison and the real est_load level structure are not simulated.

## 6. What this appendix does not do

Item 5 is not done (hashes, `est_v6.c`, freeze commit). The full G1S fit (item 2) is not done; the design note is in
section 4. No v6 text was changed, so the findings above (seed collision, balance rule applied by chaining, regime
dependence, the G1S inner-band outcome, the weak definite-fail power of the verdict) are for the freeze lane to act on.
Nothing here uses or concerns held-out data.

## 7. Reproduction, hashes

```
make test-est6dev            # tools tests, D1 and synthetic data only
make est6dev est6opchar
make est6-opchar-table       # section 5 table
```
- `docs/estimation/protocols/est-v6.md` sha256 `4fee44c729130cb37e5588b105077f4d8c17ea8c160dbfb310fdec720ff6316f` (commit e9c2829).
- `tools/estimation/est6dev.c` sha256 `c020ee872be90f7cdd62a56a8bd0c8f09e20f3ae4cdd659ccbb2b18b90e81c4d`;
  `tools/estimation/est6opchar.c` sha256 `bab7a55043065ed689478f96420de6a698763c4eb4589881438562aff126f0c1`.
- `est5` rerun: tool commit e9c2829, protocol sha256 `275cd5b72570d5182ef73d635e16775026e9d911e818b8e12f8300a701187af5`;
  params.txt of the rerun differs from the committed one only by the tool_commit line.
