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

## 3b. Schedule rule SR-1 (proposed replacement for the seed rule of v6 section 3; v6 stays NOT FROZEN)

Cause of the collision, from the code and the text, not from data: `est-v6.md` section 3 (lines 117 to 125) says a seed
that fails the balance rule is replaced by "the next seed (+1)". That chain starts at each declared seed and ignores
which run it is for, and H1 and H2 are declared one apart (0xD6E6C7, 0xD6E6C8), so their chains meet. Reproduced with
`est6dev plan6 --rule old 2700 F=0xE6C6D1 H1=0xD6E6C7 H2=0xD6E6C8`: H1 takes k = 4 and H2 takes k = 3, both ending
at 0xD6E6CB (41 segments; idle 522, L6 572, L12 857, L18 749). The 0xD6E6CD "skip used seeds" patch of section 3 would
work but makes H2 depend on what H1 happened to draw.

Rule SR-1 (implemented in `est6dev plan6`, default rule):
1. Derivation input is the pair (run label, declared seed). `base = splitmix64( declared XOR fnv1a64("est6-sched-v1/" label) )`,
   one output of the same splitmix64 used by `est_load.c`.
2. Candidate k is the `est_load` schedule of seed `base + k`, k = 0, 1, 2, ...; the effective seed is the first k whose
   schedule passes the balance rule. The only test is the schedule's own balance (idle, L6, L12, L18 each 400 to 900 s);
   no data and no score enter. The search is bounded at k = 255 and fails closed (`NO_PASSING_SEED`).
3. Labels are `F`, `H1`, `H2`; a recollection after an INCONCLUSIVE window uses the labels `F.r1`, `H1.r1`, `H2.r1`, `H1.r2`, ...
   with the same declared seed (this replaces "seed + 0x10 x n").
4. A plan is refused (exit 1, `COLLISION`) if any two labels have the same effective seed or the same segment list. This is a
   refusal, not a trigger for more searching: a refused plan is fixed by changing the declared text before freeze.
5. The effective seed (64 bit) is what is passed to `est_load` (it reads it with `strtoull`, `est_load.c:73`), and the
   whole segment list is written into the frozen file.

Required properties (checked by `make test-est6dev`, 43 checks in total, of which the schedule ones are in the block
"2b"): the segments of each label sum to exactly 2700 s; every segment is 20 to 120 s except that the last may be
clipped; each level totals 400 to 900 s; H1 and H2 (and F) have different effective seeds and different segment lists;
the same declared seed under two labels gives two schedules; the same input gives byte-identical output; a label's
schedule does not depend on the other labels in the plan. Red/green: the old rule reproduces the collision
(`--rule old` exits 1 with `COLLISION H1 H2 effective 0xd6e6cb`, asserted by the test), SR-1 exits 0 with `distinct PASS`.
A deliberately broken SR-1 (label removed from the derivation) fails the test "same declared seed, two labels".

No schedule was chosen by trying alternatives: SR-1 was written down above and run once on the declared seeds, and its
output is the one recorded here. The v5 schedule tools (`sched`, `pick`) and `est_load.c` are unchanged.

Result of SR-1 on the declared seeds (`est6dev plan6 2700 F=0xE6C6D1 H1=0xD6E6C7 H2=0xD6E6C8`; segment list
level/seconds is printed by the same command):

| label | declared | k | effective seed | segments | idle | L6 | L12 | L18 |
|---|---|---|---|---|---|---|---|---|
| F | 0xE6C6D1 | 1 | 0xc6165660a97249c0 | 46 | 622 | 620 | 669 | 789 |
| H1 | 0xD6E6C7 | 1 | 0x0fe5afc2c2177267 | 36 | 452 | 732 | 634 | 882 |
| H2 | 0xD6E6C8 | 0 | 0xb47c8fc0335c4b5f | 40 | 891 | 676 | 473 | 660 |

Whether these three are the schedules at freeze is a freeze decision; they are listed so the rule's output is auditable.
The balance rule is a property of the schedule only. It does not remove the section 2 finding that load level
matters to calibration, and it does not claim any schedule is representative.

## 4. Item 2: G1S fit with block cross-validation (REDUCED, not the full item)

(Superseded in part: the full item was done afterwards, section 4b. This section is kept as the record of the reduced check.)

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

## 4b. Item 2 completed: full G1S fit with block cross-validation (DEVELOPMENT RESULT)

Everything in this section is a DEVELOPMENT RESULT on v5 D1 (and, separately, v3 D1). It is not a held-out result, it
changes no v5 file and no v6 protocol text, and v6 stays NOT FROZEN. Raw outputs are committed unedited:
`docs/estimation/receipts/est-v6-dev/g1sfull-v5d1.txt` and `g1sfull-v3d1.txt`. Command (about 6 minutes each):

```
est6dev g1sfull --raw <D1>/machine-state.ndjson --marks <D1>/machine-state-marks.txt
```

What it does, as `est-v6.md` section 5 specifies. Stage 1: the whole G1 grid (dyn 6 x q 9 x lam 7 x nu 7 x c 21) times kappa
in {0.6, 0.7, 0.8, 0.9, 1.0}: 55566 (dyn, q, lam, nu, c) points, each scored at five kappa values. The lam grid is extended
downwards by 0.05 as v6 section 5 item 5 says (v5 had 0.1 to 0.9). Folds: the rank of each block (whole load segment, or idle
stretch between marked segments) modulo 5; the grid is searched on 4 folds and the held-out fold is scored. Selection key: the
pooled out-of-fold one-step mean log score. Stage 2, per fold on the training folds with that fold's stage 1 frozen:
(phi, nu_h, c_h, kappa_h) over 5 x 10 x 31 x 5, scored on the ten-step log score. A control with kappa fixed at 1.0 (G1 exactly)
is fitted under the same folds, grid and rules. The final whole-pool refit is also printed.

Implementation notes and checks (each from `make test-est6dev`).
- The two-piece Student-t has side masses 1/(1+kappa) and kappa/(1+kappa), scale c on the upper side and c kappa on the lower
  side, so the density is continuous at the location. `est6dev twopiece-selfcheck` checks, over 5 kappa x 3 nu x 3 scale x
  3 location, that the bin probabilities sum to 1 (max error 1.1e-14), that the floored CDF has no jump between bins (0) and is
  monotone, and that the two-piece formula at kappa 1.0 agrees with `est4_fast_logp` to 1.2e-11.
- At kappa = 1.0 the code path calls the unchanged `est4_fast_logp`, so the control is G1 exactly (bit for bit by construction).
  The formula check above is therefore only to rounding (1.2e-11), not bit for bit. A bit-for-bit `est_v6.c` library is still OPEN.
- Whole-pool refit on v5 D1 returns dyn 1, q 10000, lam 0.1, nu 1.25, c 0.70, kappa 1.0 with mean log score -1.962811: the v5 G1
  selection and score of `receipts/est-v5/params.txt`, now found by a grid that also contains the new lam value and all kappa.
  The extended lam value 0.05 and kappa below 1 do not win in sample on v5 D1.
- Same 2652 steps as `est6dev g1pit` (grid line `steps 2652`); a test asserts it.

Out-of-fold result, pooled over the 5 folds (DEVELOPMENT RESULT, v5 D1, n 2652):

| quantity | G1S (kappa free) | control G1 (kappa 1.0) |
|---|---|---|
| out-of-fold one-step mean log score | -1.969622 | -1.968162 |
| kappa chosen in folds 0 to 4 | 1.0, 1.0, 0.9, 0.9, 1.0 | 1.0 in all |
| pit_bin0 / pit_bin9 | 0.0853 / 0.1199 | 0.0766 / 0.1233 |
| lower / upper miss 2.5 % | 0.0138 / 0.0340 | 0.0126 / 0.0356 |
| coverage 50 / 80 / 95 | 0.4943 / 0.7947 / 0.9522 | 0.5003 / 0.8001 / 0.9518 |
| mean z (mid-PIT) / lag-1 z | 0.022 / -0.021 | 0.047 / -0.024 |
| ten-step coverage 95 (stage 2 per fold) | 0.9410 | 0.9402 |

Same procedure on v3 D1 (separate development data, n 2651, not pooled with v5 D1):

| quantity | G1S | control G1 |
|---|---|---|
| out-of-fold one-step mean log score | -1.593744 | -1.596005 |
| kappa chosen in folds | 0.9 in all 5 | 1.0 in all |
| pit_bin0 / pit_bin9 | 0.0929 / 0.1041 | 0.0830 / 0.1190 |
| lower / upper miss | 0.0179 / 0.0304 | 0.0166 / 0.0344 |
| ten-step coverage 95 | 0.9551 | 0.9684 |

Readings, each from the numbers above (no tuning to them was done: grids and rules were fixed by the proposal before the run).
- PROVEN as numbers: G1S is worse than G1 by 0.0015 nats out of fold on v5 D1 and better by 0.0023 nats on v3 D1. Both gaps are
  far below the 0.01 nat tie rule of v6 section 5 item 3, so that rule keeps G1 on both sets. The log score does not support G1S.
- PROVEN as numbers: G1S moves bin 0 up (0.0766 to 0.0853 on v5 D1, 0.0830 to 0.0929 on v3 D1, about +0.009 and +0.010) and bin 9 down
  (0.1233 to 0.1199, and 0.1190 to 0.1041), as hypothesised.
  This is a calibration-shape effect that costs no log score, not evidence of better forecasting. G2 and G3 were not run here.
- Inner-band screen of v6 section 5 item 2 (each outcome is in the raw files; the `gate` lines). Read literally, the screen
  cannot be passed by any model, which is a **defect in the proposal to fix before freeze**: (i) P3 coverage 95 has band
  [0.93, 0.97] shrunk by 0.02 on both sides, a zero-width band at exactly 0.95; (ii) P4 and P5, if the 0.01 shrink applies to the
  miss rates, become [0.020, 0.030], which rejects a lower miss of 0.0126 to 0.0179 and an upper miss of 0.030 to 0.036 on every
  fit here, G1 and G1S alike. If the shrink is read as applying only to P1, P2, P6 and P7: on v5 D1 G1S passes P6 bin 0 (0.0853)
  and bin 9 (0.1199, margin 0.0001) and G1 fails both (0.0766, 0.1233); on v3 D1 both pass P6 and G1 fails the inner P7 (0.9684
  against 0.96). So under that reading G1S would pass the screen where G1 does not, on v5 D1, and the 0.01 nat rule applies only
  among screen passers (v6 section 5 item 3), so G1S would then be selected on v5 D1. That outcome rests on a bin 9 margin of 0.0001.
- Whether to adopt G1S is therefore OPEN and depends on the freeze lane's reading of the inner-band rule.

### Load-dependent calibration bias (DEVELOPMENT RESULT; v5 D1 and v3 D1, out of fold)

Out-of-fold PIT by declared load level. Block bootstrap within each level (whole blocks, 2000 draws, seed 0xE6B007 + level index),
standard errors in brackets (few blocks per level, so the standard errors are themselves rough). `m` is the mean of (u - 0.5): zero
for an unbiased forecaster, negative when outcomes land below the forecast median (forecast too warm), positive when above.

v5 D1:

| level | steps | blocks | G1S bin0 | G1S bin9 | G1S m | G1 bin0 | G1 bin9 | G1 m |
|---|---|---|---|---|---|---|---|---|
| idle | 818 | 8 | 0.129 (0.007) | 0.075 | -0.070 (0.008) | 0.115 (0.010) | 0.077 | -0.061 (0.010) |
| L6 | 739 | 10 | 0.129 (0.009) | 0.109 | -0.027 (0.010) | 0.114 (0.007) | 0.118 | -0.016 (0.009) |
| L12 | 970 | 14 | 0.026 (0.007) | 0.158 | +0.063 (0.010) | 0.025 (0.007) | 0.159 | +0.065 (0.011) |
| L18 | 125 | 3 | 0.001 (0.001) | 0.183 | +0.121 (0.016) | 0.000 (0.000) | 0.184 | +0.127 (0.018) |

v3 D1:

| level | steps | blocks | G1S bin0 | G1S bin9 | G1S m | G1 bin0 | G1 bin9 | G1 m |
|---|---|---|---|---|---|---|---|---|
| idle | 235 | 4 | 0.160 (0.017) | 0.084 | -0.088 (0.015) | 0.149 (0.020) | 0.089 | -0.070 (0.015) |
| L6 | 672 | 12 | 0.175 (0.015) | 0.116 | -0.046 (0.015) | 0.158 (0.013) | 0.134 | -0.030 (0.015) |
| L12 | 1079 | 16 | 0.056 (0.008) | 0.101 | +0.029 (0.008) | 0.049 (0.007) | 0.117 | +0.041 (0.009) |
| L18 | 665 | 7 | 0.046 (0.005) | 0.105 | +0.033 (0.009) | 0.040 (0.005) | 0.118 | +0.046 (0.010) |

Findings.
- PROVEN as numbers, on both development sets: the bias is monotone in load. The forecast is too warm when idle (m -0.06 to -0.09)
  and too cool at L12 and L18 (m +0.03 to +0.13). Idle against L12, in units of the larger of the two bootstrap standard errors:
  v5 D1 G1S (0.063 + 0.070) / 0.010 is about 13; v3 D1 G1S (0.029 + 0.088) / 0.015 is about 8. Even allowing for rough standard
  errors, this is not noise. Bin 0 is over-filled at idle and L6 (0.11 to 0.18) and under-filled at L12 and L18 (0.00 to 0.06).
- PROVEN as numbers: it is not stable in size across runs. L18 bin 0 is 0.001 on v5 D1 (125 steps, 3 blocks) and 0.046 on v3 D1
  (665 steps, 7 blocks). So per-level shares measured on one run cannot be assumed on the next.
- PROVEN as numbers: G1S does **not** remove it. It has the same sign at every level and the same order of size as G1. On v5 D1
  idle m is -0.070 against -0.061 and L12 is +0.063 against +0.065; on v3 D1 it trims the high levels (L12 +0.029 against +0.041,
  L18 +0.033 against +0.046) and enlarges the low ones (idle -0.088 against -0.070, L6 -0.046 against -0.030). Bin 0 stays
  above 0.11 at idle and L6 and below 0.06 at L12 and L18 in both. The pooled m of G1S on v5 D1 (-0.0006, G1 +0.0064) is closer to
  zero only because the opposite per-level biases cancel in the pool.
- No load covariate is permitted by the proposal (v6 section 5 "Not adopted": regime-conditioned scale and anything that feeds the
  declared schedule to the estimator), so none was fitted here. The bias is therefore **not resolved** by anything v6 proposes. It
  is LIKELY a model-structure limit (the filter lags load steps; not tested here) and would need a v7-class remedy or a changed claim.
- Consequence for the pooled held-out gates (a projection, UNVERIFIED, assuming each level's shares transfer from D1). Weight the
  per-level bin shares by the level seconds that rule SR-1 gives H1 + H2 (idle 1343, L6 1408, L12 1107, L18 1542 of 5400;
  weights 0.249, 0.261, 0.205, 0.286). With the v5 D1 per-level numbers the pooled expectation is bin 0 0.0713 (G1S) and 0.0637 (G1),
  bin 9 0.1318 (G1S) and 0.1350 (G1): bin 9 above 0.13 for both, bin 0 below 0.07 for G1. With the v3 D1 per-level numbers it is bin 0
  0.110 (G1S) and 0.100 (G1), bin 9 0.102 and 0.115: no gate broken. The two disagree, which is the instability above. The pooled
  held-out result depends on the realised level mix and on per-level shares nobody can predict from D1, so the balance rule alone
  does not make the pooled test insensitive to the regime bias.

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

## 5b. Item 3 extended: operating characteristics, SIMULATION only (not forecasting performance)

Everything here is SIMULATION. No data file is read (`est6opchar` has no input path), so no held-out file can be touched. The numbers
describe how the proposed three-way verdict of v6 section 6 behaves on synthetic PIT sequences under the stated assumptions, not how
any forecaster performs. Raw rows: `docs/estimation/receipts/est-v6-dev/opchar-table2.txt`; command `make est6-opchar-table2`
(20000 replicates per row, 1000 bootstrap draws, Monte Carlo standard error at most 0.0035 on every rate).

Definitions used below. False accept: the verdict is PASS although the simulated forecaster truly breaks a gated limit. False reject:
the verdict is not PASS although the forecaster is truly calibrated (reported both as not PASS and as HELD_OUT_FAIL). NOT_ESTABLISHED
is neither accept nor reject.

Assumptions (explicit, all as in section 5 unless new): two pooled 2700 s runs (n about 5300, P10 met); segments 20 to 120 s with a
uniformly random level; Gaussian copula with per-segment share w (0.08 and 0.12, the measured design-effect range 3.3 to 4.5) and
within-segment AR(1) 0.02; ten-step sequence with within-segment AR(1) 0.9; baseline comparison assumed met; lag-1 z, P9 quarters and
regimes use the point value (this can only increase HELD_OUT_FAIL). New in 5b: case (d), a forecaster whose PIT depends on the
declared load level, with the per-level shares measured for G1 on v5 D1 (section 2: bin 0 0.116, 0.111, 0.025, 0.000; bin 9 0.077,
0.114, 0.159, 0.184; lower miss 0.030, 0.010, 0.000, 0.000; upper miss 0.036, 0.017, 0.041, 0.085 for idle, L6, L12, L18) with bins 1
to 8 sharing the rest equally (ASSUMPTION: per-level middle bins were not tabulated) and random segment levels, so the level mix is
about equal (the "bin0 0.0760" in the printed header of a case d row is a default that case d does not use).

Results (SIMULATION, two runs unless stated; PASS / NOT_ESTABLISHED / HELD_OUT_FAIL):

| scenario | w | runs | PASS | NOT_EST | HELD_OUT_FAIL |
|---|---|---|---|---|---|
| (a) calibrated, replication seed 0xE6A777 | 0.08 | 2 | 0.9931 | 0.0069 | 0.0000 |
| (a) calibrated | 0.08 | 3 | 0.9988 | 0.0013 | 0.0000 |
| (b) nominal 95 % covers 92 %, replication | 0.08 | 2 | 0.0017 | 0.5644 | 0.4339 |
| (b) | 0.08 | 3 | 0.0003 | 0.3752 | 0.6244 |
| (d) regime-dependent PIT (D1 per-level shares) | 0.08 | 2 | 0.0656 | 0.8536 | 0.0809 |
| (d) | 0.08 | 3 | 0.0475 | 0.8518 | 0.1007 |
| (d) | 0.12 | 2 | 0.0726 | 0.8061 | 0.1214 |
| (d) | 0.12 | 3 | 0.0592 | 0.8327 | 0.1081 |

The replication under another seed agrees with table 5 (a: 0.9931 against 0.9928; b: PASS 0.0017 against 0.0025), so the
table-5 numbers are not seed luck.

False accept for a forecaster whose true bin 0 is below the 0.07 limit (case c cells, w 0.08; other cells scaled pro rata):

| true bin 0 | runs 2: PASS / NOT_EST / HELD_OUT_FAIL | runs 3: PASS / NOT_EST / HELD_OUT_FAIL |
|---|---|---|
| 0.070 (at the limit) | 0.403 / 0.587 / 0.010 | 0.442 / 0.552 / 0.007 |
| 0.068 | 0.294 / 0.685 / 0.021 | 0.300 / 0.682 / 0.019 |
| 0.065 | 0.152 / 0.793 / 0.055 | 0.125 / 0.807 / 0.068 |
| 0.060 | 0.027 / 0.761 / 0.212 | 0.012 / 0.677 / 0.311 |
| 0.055 | 0.0015 / 0.470 / 0.528 | 0.0004 / 0.282 / 0.718 |

Readings (each from the tables; interpretation labelled).
- Requirements of v6 section 6 are met by the SIMULATION at w 0.08 and 0.12 with two runs: false reject under (a) is 0.7 % (not PASS) and
  0 % (HELD_OUT_FAIL) at w 0.08 (table 5: 2.7 % and 0.4 % at w 0.12); pass under (b) is 0.2 %. This is conditional on the assumptions
  above; w 0.20 (table 5) still fails, and real clustering structure is not a Gaussian copula.
- False accept is controlled only for shortfalls that are not small: a forecaster 0.005 under the limit (bin 0 0.065) is accepted
  15 % of the time with two runs and 13 % with three, and one 0.002 under (0.068) 29 to 30 %. This is inherent to a point-value PASS
  at a hard edge: PASS needs only the point value inside the band, and an edge-sitting forecaster lands on either side by chance.
  The simulation says nothing that would make the PASS rule safer than a coin flip at the edge, and three runs do not help there.
- Definite rejection is slow: HELD_OUT_FAIL needs the 99.5 % interval entirely below the edge. A bin 0 of 0.060 is called a definite
  fail 21 % (two runs) or 31 % (three) of the time, and mostly reported NOT_ESTABLISHED. A 92 % forecaster is a definite fail 43 %
  (two runs) or 62 % (three) of the time.
- Regime-biased forecaster (d): it passes only 5 to 7 % of the time, is called a definite fail only 8 to 12 %, and is reported
  NOT_ESTABLISHED about 81 to 85 % of the time. So a forecaster with the D1 regime bias would very probably be neither accepted nor
  refuted. LIKELY reason (not decomposed here): several gates (bin 0, bin 9, the P9 regime coverages) each sit near or past an edge at once.
- Three runs lower false accept at moderately bad cells (0.060: 2.7 % to 1.2 %) and raise definite-fail power (0.21 to 0.31), at the
  cost of another quiet window of about 45 minutes. They do not help at the edge (0.070: 40 % to 44 %).
- Limits (INCONCLUSIVE where stated): the data-generating model is a Gaussian copula with a segment effect; the level structure
  matters (section 4b shows per-level shares are unstable across runs) and is only partly simulated in (d) with one set of D1 shares;
  P7 is crude; the baseline comparison is not simulated; whether a v6 PASS licenses the section 7 claim at stated operating
  characteristics is therefore **INCONCLUSIVE** from this simulation alone, since the required table has been produced but the
  simulation's assumptions have not been shown to hold for the real forecaster.

## 6. What this appendix does not do

Item 5 is not done (hashes, `est_v6.c`, freeze commit). The full G1S fit (item 2) is now done as a DEVELOPMENT RESULT (section 4b)
and its load-bias finding is unresolved; the operating-characteristics extension is section 5b (SIMULATION); the schedule defect has
rule SR-1 (section 3b). `est-v6.md` itself was not changed (it is a PROPOSAL; the freeze text must absorb 3b and the inner-band
fix of 4b), so the findings of this appendix (regime dependence, the G1S outcome, the weak definite-fail power of the verdict, the
inner-band definition) are for the freeze lane to act on. The checklist of what is still open is section 8.
Nothing here uses or concerns held-out data.

## 7. Reproduction, hashes

```
make test-est6dev            # tools tests, D1 and synthetic data only
make est6dev est6opchar
make est6-opchar-table       # section 5 table
make est6-opchar-table2      # section 5b SIMULATION table (20000 replicates per row, a few minutes)
est6dev plan6 2700 F=0xE6C6D1 H1=0xD6E6C7 H2=0xD6E6C8     # section 3b
est6dev g1sfull --raw <D1>/machine-state.ndjson --marks <D1>/machine-state-marks.txt   # section 4b (about 6 minutes)
```
- `docs/estimation/protocols/est-v6.md` sha256 `4fee44c729130cb37e5588b105077f4d8c17ea8c160dbfb310fdec720ff6316f` (commit e9c2829).
- Sections 1 to 5 hashes (tools as merged in #293): `tools/estimation/est6dev.c` sha256 `c020ee872be90f7cdd62a56a8bd0c8f09e20f3ae4cdd659ccbb2b18b90e81c4d`;
  `tools/estimation/est6opchar.c` sha256 `bab7a55043065ed689478f96420de6a698763c4eb4589881438562aff126f0c1`.
- `est5` rerun: tool commit e9c2829, protocol sha256 `275cd5b72570d5182ef73d635e16775026e9d911e818b8e12f8300a701187af5`;
  params.txt of the rerun differs from the committed one only by the tool_commit line.
- Sections 3b, 4b, 5b tools (this change): `tools/estimation/est6dev.c` sha256 `3b18c7d211bf7d34b9ea18b1f78adf42d7292569d46f86d42c44db9d962a6230` (adds `plan6`, `g1sfull`, `twopiece-selfcheck`;
  `g1sfull` was run from a binary built from exactly this file, sha256 of the binary `48251debc1680320ce2e79b933dfd04caba1881128168fa9c8592974c578a25b`),
  `est6opchar.c` sha256 `3baf2cc2011e5a17726260ea89f7b241ad69ac263a276e8a5893279b267d8fbc` (adds case d), `est6opchar_table2.sh` sha256 `09ff006d69658a56f735132d25f00295bcb3f8d51221facbf52f2cc90658c823`. Estimation suites on this change (counts in the PR):
  `make test-est6dev` 43 checks, PASS.
- Raw outputs: `docs/estimation/receipts/est-v6-dev/g1sfull-v5d1.txt` sha256 `d92b729dd439b064fa5a05438a26e1cae577d96f50b765eb4e5f174b4327ae23`,
  `g1sfull-v3d1.txt` sha256 `ef258d6d05c86902c6007594a3547ae2c6c427b4a3461b0969119567cf970529`,
  `opchar-table2.txt` sha256 `6749dc0eb14f9f79d618d9cce1377d75bd4f444dd026eb8511a13f04eb58aaef`.

## 8. Freeze-readiness checklist (v6 NOT FROZEN)

Status as of the commit that adds this section. DONE means the item exists and was checked as stated; OPEN means it does not exist
or is not decided. Nothing below is a freeze.

| # | item | status | evidence |
|---|---|---|---|
| 1 | Model family decided (G1S adopted or dropped) | OPEN | development fit done (section 4b): G1S not preferred by the 0.01 nat rule on v5 D1 (-0.0015) or v3 D1 (+0.0023); outcome of the inner-band screen depends on how the ambiguous shrink rule is read (4b); load bias unresolved |
| 2 | Estimator library for the model (`est_v6.c`) with kappa 1.0 bit for bit equal to `est_v4.c` | OPEN | no `src/estimation/est_v6.c`; `est6dev g1sfull` holds a development two-piece (`twopiece-selfcheck` PASS; equals `est4_fast_logp` to 1.2e-11; kappa 1.0 path calls it unchanged) |
| 3 | Protocol text (`est-v6.md`) corrected and frozen | OPEN | proposal sha256 in section 7; known text defects to fix: seed rule (section 3 and 3b), inner band of P3 is zero width and the shrink of P4/P5 is ambiguous (4b), schedule lists |
| 4 | Parameter fit rules defined and implemented in development | DONE (development) | `est6dev g1sfull`: 55566 grid points, 5 kappa, 5-fold block CV, stage 2 with kappa_h, control; final refit equals the v5 G1 (-1.962811); raw outputs in `receipts/est-v6-dev/`. The final v6 parameters need v6 F data, which does not exist |
| 5 | Parameter fit on v6 F and committed | OPEN | F not collected (correctly: collection only after freeze) |
| 6 | Schedule identity rule (H1 differs from H2, deterministic, balanced) | DONE | rule SR-1, `est6dev plan6`, `make test-est6dev` (old rule collision reproduced, SR-1 distinct, determinism, balance, structure); PR #297 |
| 7 | Schedule identities written into the frozen file (F, H1, H2 segment lists, effective seeds) | OPEN | section 3b lists the SR-1 output for the declared seeds; adoption into the frozen file is a freeze edit |
| 8 | Tool identity: `est6` binary, `tool_commit`, source and protocol hashes, binding modes | OPEN | only development tools `est6dev` and `est6opchar` exist; they have no binding mode and no receipt writer; no `mk/estimation_v6.mk` |
| 9 | Scoring implementation: randomised PIT with a declared generator seed, bin and miss-rate gates, 99.5 % block bootstrap, three-way verdict, P1 to P10, baseline comparison | OPEN | `est6dev g1pit` computes descriptive statistics and a block bootstrap; `est6opchar` simulates the verdict logic on synthetic PIT; neither is the scorer |
| 10 | Operating-characteristics table required by v6 section 6 | DONE (as SIMULATION) | section 5 and 5b; false reject under (a) 0.7 % and pass under (b) 0.2 % at w 0.08, two runs; but conditional on assumptions (5b), so claim adequacy INCONCLUSIVE |
| 11 | Load-dependent calibration bias remedy | OPEN | quantified, not removed by G1S (4b); no remedy within the proposal |
| 12 | Collector and window protocol changes (section 4: /proc/stat foreign-CPU count, WINDOW_VOID, 300 s idle start) | OPEN | no `est6_collect.sh` |
| 13 | Data identities: D1 hashes recorded; v6 F, H1, H2 hash files | DONE (D1) / OPEN (F, H1, H2) | section 1 hashes; v6 raw data does not exist yet |
| 14 | D2 never read; refusal guard and its test | DONE | `forbidden_path` and the two SHA backstops in `est6dev.c`; tests `1`, `1b`, `g1sfull` refusals in `test_est6dev.sh`; `strace -f -e trace=openat` of a `g1sfull` run (stride 6) opened only the v5 D1 raw, marks and SHA256SUMS files and 0 paths containing the D2 date tag or "heldout"; the full runs and v3 D1 runs were given D1 and v3 D1 paths only and were not traced (UNVERIFIED beyond that) |
| 15 | Freeze commit, hashes recorded, receipts directory | OPEN | none |

Not done by this appendix: items 1, 2, 3, 5, 7, 8, 9, 11, 12, 13 (F, H1, H2), 15.

