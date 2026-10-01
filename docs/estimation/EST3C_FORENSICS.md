# EST-3c forensics: why EST-3 v1 and v2 failed (2026-09-30)

Purpose: explain the two recorded FAILs before protocol v3. Both stay FAIL;
nothing here rescores, refits or selects a v3 model. Every number is printed by
`tools/estimation/est3c_forensics.c` from committed files; `evidence/EST3C/` was not read.

Rebuild and rerun from the omega repo root (single-threaded, stdout only; two
runs gave byte-identical output):
```text
nice -n 19 cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic \
  -o /tmp/est3c_forensics tools/estimation/est3c_forensics.c -lm && nice -n 19 /tmp/est3c_forensics
```

Inputs (SHA-256). A = v1 fit run, B = v1 held-out run, C1 = v2 idle fit run.
The raw digests equal their runs' `SHA256SUMS` lines; the stream digests equal
those recorded in `receipt.json`.
```text
6df4a2b079482b79c42da84da5627b6a8fb9c7d3ce885aac53437b0199d517db  A  evidence/R15/raw/20260929T020536Z-ad8e1f2ea4e4-silicon/machine-state.ndjson
9a91027b26b972f48a126953eeb29915da6eea61ab2c517366bf29e5c468d842  A  .../machine-state-marks.txt
91a5fe34225926cd7aca2fca4be5ed51dc0772341a2fda5f9a39d0f72b8a6bb5  B  evidence/R15/raw/20260929T025735Z-3e9e53be3358-silicon/machine-state.ndjson
d2dcc89fb1f9bfff15e1dce6be5248ba6f285cfe1fb7dd051c09764aec844d04  B  .../machine-state-marks.txt
65252bae5f9d49d30a3b334fd2b9444c36db7627a45fcd9b6ef0bb4e5a7e5716  C1 evidence/EST3B/raw/20261001T020412Z-est3b-fit-silicon/machine-state.ndjson
3dcfc08c8b7dbb3dcd32a16a23e397cc9629a453e9a655cdd555ff084258b226  docs/estimation/receipts/est23-v1/receipt.json
2924c4cb64831c4c569dbc9a7fd485d19ce76c3f6ae713b74fbb55bc0a15f3f5  docs/estimation/receipts/est23-v1/receipt.json.M0.stream
5255d44ff09b53139985b936ec3d51f128d3a3d264d320bad3d487597727f0bc  docs/estimation/receipts/est23-v1/receipt.json.M1.stream
```

Definitions: one-step change d_L = z_L - z_{L-1} (zone 0, mC), kept for line
L >= 30 with rounded wall gap exactly 1 s (the v2 pre-check rule); none was
excluded for gap (kept: A 1923, B 1979, C1 2060). Excess kurtosis uses
population moments (Gaussian = 0). Ljung-Box Q10 5 % cutoff is about 18.3.

## 1. Reproduction
- v1, from the streams: every M0 and M1 statistic in `receipt.json` is reproduced
  bit for bit (difference 0.0). M0: coverage 50/80/95 % = 0.7266 / 0.8373 /
  0.9212, mean NIS 1.0214, lag-1 0.0939, Q10 151.1, RMSE 1166.35 vs persistence
  1162.11, zero innovations 0.0278. M1: 0.7034 / 0.8327 / 0.9227, Q10 350.4.
- v2, from raw C1: 1656 of 2060 changes exactly zero = 0.8039. Matches.

## 2. Quantization
| run | exact-zero changes | P(abs d <= 50) under N(0, sd) | sd (mC) |
|---|---|---|---|
| A | 631/1923 = 0.3281 | 0.0337 | 1182.5 |
| B | 648/1979 = 0.3274 | 0.0343 | 1162.1 |
| C1 | 1656/2060 = 0.8039 | 0.1005 | 396.1 |

Treating each B outcome as its reading cell (z-50, z+50] and scoring fractional
(randomized-PIT) coverage moves v1 M0 from 0.7266 to 0.7274 (50 %), 0.8373 to
0.8379 (80 %) and 0.9212 to 0.9204 (95 %). Quantization explains under 0.001 of
a 0.227 excess at 50 % and a 0.029 shortfall at 95 %: the cell is 100 mC against
a predictive sd near 1160 mC. The zero mass of raw changes is 10x what a
Gaussian of that spread allows, a shape fact rather than a grid fact.
M0 randomized-PIT deciles on B (ideal 0.100 each):
0.079 0.034 0.027 0.042 0.303 0.283 0.065 0.040 0.044 0.083. The central 20 %
of probability holds 0.586 of outcomes, the shoulders are under-filled and the
95 % band misses 7.9 % against 5 %: a peaked centre plus heavy outer tails at
a correct overall variance (mean NIS 1.02).

## 3. Tails and the trial structure
| run, subset | n | excess kurtosis | max abs d | share abs d >= 1000 |
|---|---|---|---|---|
| A all / within one trial / spans a mark | 1923 / 1819 / 104 | 5.11 / 5.56 / 1.51 | 6300 / 6300 / 6000 | 0.232 / 0.217 / 0.500 |
| B all / within one trial / spans a mark | 1979 / 1875 / 104 | 5.41 / 5.99 / 1.33 | 7100 / 7100 / 5600 | 0.234 / 0.221 / 0.471 |
| C1 all (no marks) | 2060 | 65.00 | 5000 | 0.025 |

"Spans a mark" means a begin or end time lies in (t_{L-1}, t_L]. Trials run back
to back (gaps of a few ms), so a between-trials sample set is empty: 2 of 1953
samples on A and 1 of 2009 on B lie outside every begin..end pair (consistent
with v1's idle n = 0). Edge changes are larger (half reach 1000 mC) but are 5 % of changes; the heavy tail lives inside trials.

## 4. Dependence
| run | Q10 of d | acf d lags 1, 2, 4 | Q10 of abs d | acf abs d lags 1, 2, 5 |
|---|---|---|---|---|
| A | 103.1 | -0.036, -0.111, -0.109 | 1114.6 | 0.429, 0.357, 0.231 |
| B | 102.0 | -0.022, -0.129, -0.077 | 1263.8 | 0.449, 0.376, 0.227 |
| C1 | 132.3 | -0.006, -0.129, -0.163 | 852.1 | 0.505, 0.217, 0.105 |

Changes are negatively correlated at lags 1 to 7 on A and B (up to -0.13,
partial mean reversion), and abs d shows strong volatility clustering to about
lag 6 on every run, idle included. v1's Q10 of 151 / 350 was on normalized
innovations, a different series, but points the same way.

## 5. Rolling scale (descriptive only, A and B, not a v3 model choice)
Strictly causal EWMA variance of d (init = mean d^2 over L < 30; scale floor
100^2/12 or 100^2 mC^2). Excess kurtosis of d / scale, then cov50 / cov95:

| run | fixed sd | EWMA 0.97 (either floor) | EWMA 0.90, floor 100^2/12 | EWMA 0.90, floor 100^2 |
|---|---|---|---|---|
| A | 5.11; 0.730 / 0.9215 | 27.43; 0.734 / 0.921 | 665.50; 0.697 / 0.910 | 95.26; 0.697 / 0.911 |
| B | 5.41; 0.733 / 0.919 | 13.22; 0.742 / 0.923 | 553.74; 0.687 / 0.914 | 56.17; 0.687 / 0.916 |

A rolling Gaussian scale does not remove the tail excess; it makes it worse.
After runs of exact-zero changes the scale collapses and the next jump
standardizes to as much as 67 (max abs z 9.3 to 67). Clustering is real, but a smooth variance does not describe "no change, then jump".

## Classification
| failure | assumption that failed | statistic | class | at fault |
|---|---|---|---|---|
| v1 50 % cov 0.727 | Gaussian one-step shape | PIT deciles 5-6 hold 0.586; zeros 0.33 vs 0.034 | misspecification (shape) | estimator |
| v1 95 % cov 0.921 | Gaussian tails | excess kurtosis 5.4 on B, 6.0 within trials | tails | estimator |
| v1 Q10 151 / 350 | independent innovations | Q10 of abs d 1264 on B | dependence | estimator |
| v1 RMSE 1166 vs 1162 | a filter can beat persistence | lag-1 acf of d -0.022 on B | misspecification (gate design) | experiment |
| v1 regime gate | B has idle samples | 1 of 2009 samples outside trials | untestable | experiment |
| v2 pre-check 0.8039 | continuous predictive fits outcome | 1656/2060 exact zeros | quantization | experiment |

v1 verdict: primarily the ESTIMATOR (not bias: standardized bias -0.0006; not
overall variance: mean NIS 1.02). One Gaussian cannot match changes with 33 %
exact zeros, heavy tails and clustered magnitudes. Quantization, set aside in
the v1 RESULT, is confirmed irrelevant (fractional coverage moves under 0.001).
Experiment-side contributors: the RMSE gate asks for a gain over persistence on
changes nearly uncorrelated at lag 1 (M0 also sat at the q grid edge 1e6, as
recorded), and B has no idle regime, so the regime gate could not run.

v2 verdict: the EXPERIMENT. The estimator never ran. The idle regime has 80 %
exact zeros, and point coverage of a continuous interval cannot land in
[0.46, 0.54] on such data; the pre-check did its job. C1 also shows excess
kurtosis 65 and Q10(abs d) 852, so a Gaussian would likely be misspecified
there too (descriptive, not tested on held-out data). Not computed here, so not
claimed: whether a heavy-tailed, mixture or hold-or-jump model would pass, or
whether the zero runs come from a sensor refresh slower than 1 Hz.
