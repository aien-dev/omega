# TURING Yield energy protocol V0 (TY-4 measurement reuse, TY-5 concurrent attribution)

Status: PRE-REGISTERED. Committed and pushed before any timed run of stage 1 or stage 2.
Any change after data is seen is a dated amendment at the end of this file, committed before the run it affects.
Branch `feat/turing-yield-ty45`, base omega `a927bd6`. Date 2026-09-29.

## 1. Question

When two CPU workloads run at the same time, is the package energy of the pair different from the sum of their
separate costs, and can an inspectable model built from independent signals predict the pair's energy without being
fitted on it?

The identity `I = E_AB - E_A - E_B + E_0` is algebra. Writing it down proves nothing. The evidence is:

- (a) a confidence interval on `I` over repeated, interleaved rounds, with a statement of whether `I` is
  distinguishable from zero;
- (b) an attribution model fitted from per-process PMU counters (independent of the energy meter) on idle and solo
  windows only, which predicts the energy of a held-out coalition (A+B) within stated uncertainty.

## 2. What is reused, what is new

- Meter and counters: `tests/runtime/r15_measure.{c,h}` linked unchanged (frozen under R16; not edited).
  `r15_energy_open` is NOT called because it loads NVML (GPU driver library). The harness fills `R15Energy.hwmon`
  itself after checking all ten `aien_spbm` channel labels, initialises the mutex, and uses `r15_energy_read`,
  `r15_energy_start_sampler` (10 Hz) and `r15_energy_samples` as they are. PMU: `r15_pmu_open/start/stop` per
  workload process (pid 0, inherit), so each process counts only its own threads.
- Bad-window refusal: `tools/r15_reduce.c` `energy_ok` (spbm_ok and every overflow flag zero) and its
  monotonic rule (end reading >= start reading, else refuse; counters are never unwrapped). The TY reducer compiles
  `tools/r15_reduce.c` into its own translation unit (its `main` renamed) and calls those functions; it does not
  copy them. The raw window records use the r15 JSONL writer (`r15_rec_begin`, `r15_json_energy`, `r15_json_pmu`).
- New (C only): `tests/turing/ty_workload.c` (workload process), `tests/turing/ty_energy_window.c` (measures one
  window per call), `tools/ty_energy_reduce.c` (reducer), `src/turing/ty_energy.{c,h}` (records, checks),
  `tests/turing/test_ty_energy.c` + fixtures (hostile tests), `tools/ty_energy_run.sh` (rounds, idle wait, quiet
  flag), `mk/turing_yield_energy.mk`.
- No GPU, no NVML, no src/runtime, no src/algebra edits (read-only use of `oma_rz_get`/`oma_rz_find`).

## 3. Workloads (fixed)

- Operation: MA-2 ternary GEMV (Omega-X), shape m = 8192 rows, n = 16384 columns, weight sparsity 0.3, x uniform
  int8. Weights and x are generated from a seed; every thread's plan is checked bit-exact against `oma_rz_oracle`
  once before the window. A failed check aborts the window.
- Workload A: `R1_plain`, seed 1. Packed weights 128 MiB per thread.
- Workload B: `R2c_crumb`, seed 2. Packed weights 32 MiB per thread.
- Both are larger than the 8 MiB L3 of each cluster, so both stream from DRAM. A and B are different realizations so
  their counter mixes differ (untimed 2 s calibration on cpu 5: A retires about 3.3 instructions per cycle, B about
  5; both move about the same bytes per second). This calibration was the only run before registration and set the
  shape; it was not metered.
- One process per workload, one pinned thread per listed core, each thread with its own packed copy. Threads call
  `run` back to back until the process deadline (planned window length after GO).
- X925 cores only: cpus 5-9 (cluster 0, L3 shared with A725 cpus 0-4) and 15-19 (cluster 1, L3 with 10-14). The
  window tool and its 10 Hz sampler are pinned to A725 cpu 0 so they never run on a measured core.
- Meter scope: all X925 cores are under one channel (`cpu_p`). `cpu_p` therefore cannot separate A from B. The
  per-participant signal for check (b) is the per-process PMU.

## 4. Conditions and order

Conditions per round: IDLE (no workload), A, B, AB (A and B started together). IDLE is a shuffled condition in every
round, so each round has its own baseline; there is no fixed idle measured once.

Order: the four conditions are shuffled within each round by a seeded splitmix64 Fisher-Yates shuffle
(`ty_energy_window --plan`). The seed is recorded in the run manifest. Seeds: stage 1 `0x7459510001`,
stage 2 `0x7459520001`.

Per window: workloads are started and packed, all report READY, then a fixed 2 s settle, then the start reading,
GO, the planned window, the end reading after every workload has exited. IDLE uses the same settle and length.
Between windows the shell waits 3 s.

## 5. Stage 1: contention sweep (exploratory, selects the case)

Configurations (A cores | B cores):

| id | A | B | meaning |
|---|---|---|---|
| S1 | 5 | 6 | same L3, 1 + 1 core |
| S2 | 5 | 15 | cross L3, 1 + 1 |
| S3 | 5,6 | 7,8 | same L3, 2 + 2 |
| S4 | 5,6 | 15,16 | cross L3, 2 + 2 |
| S5 | 5,6,7 | 8,9 | same L3, 3 + 2 (all X925 of cluster 0) |
| S6 | 5-9 | 15-19 | cross L3, 5 + 5 (all X925) |

Window 10 s, 3 rounds per configuration, configurations in seeded shuffled order, rounds of one configuration
together.

Contention score per configuration: for each round, slowdown `s_X = 1 - calls_X(AB) / calls_X(solo)` for X in {A, B};
round score `(s_A + s_B) / 2`; configuration score = mean over rounds.

Selection rule (fixed now):
- Contention case = the configuration with the highest score. It is called a REAL contention case only if its score
  is >= 0.05 AND every one of its rounds scores >= 0.02. Otherwise the result states "no contention case found" and
  stage 2 still runs on the highest-scoring configuration.
- Control case = the configuration with the lowest absolute score (a near-additive reference).
- Stage 1 energy numbers are reported as exploratory only.

## 6. Stage 2: confirmatory runs

For the contention case and the control case, in that order: 10 rounds of {IDLE, A, B, AB}, window 30 s (the audit's
error budget needs >= 19 s for 1% resolution of I; 30 s gives margin). Duration about 24 min per case.

## 7. Recorded per window

Written as one r15-format JSONL record per window (`kind":"ty_window"`), raw files never overwritten:

- meter: start and end `R15EnergySample` (energy_uj pkg, cpu_e, cpu_p, gpc_unverified, gpm; overflow_raw; power_uw;
  spbm_ok), window duration, all 10 Hz samples inside the window (t_ns, pkg, cpu_e, cpu_p), sample count, largest gap;
- per workload: realization, cores, pinned, oracle check, planned and actual interval (GO, first and last thread
  times, exit), completed calls per thread, the six raw PMU counts per PMU with enabled and running times;
- machine: /proc/loadavg at both ends, per-cpu busy time deltas from /proc/stat (busy on cores not used by the window
  is the background load), scaling_cur_freq of all 20 cpus at both ends, all thermal zone temperatures at both ends,
  quiet flag present and whose, headroom of each energy counter to the 32-bit mJ wrap.

Meter facts recorded with the results: resolution 1 mJ step (readings end in 000 µJ) is not accuracy; sampling
10 Hz, sensor update about 0.1 s; 32-bit mJ counters wrap at about 4.29 MJ, flagged by overflow_raw; no external
calibration, so absolute accuracy is not claimed; `gpc_unverified` always reads 0 and is unused; observed variance is
reported per condition.

## 8. Validity (a window is refused, never repaired)

A window is refused if any of: SPBM unavailable or not ok at either edge; any overflow flag set; any energy counter
decreased (r15_reduce rule, reused); a counter within one window's worth of the wrap; a gap between consecutive 10 Hz
samples > 300 ms or fewer than 90% of the expected samples; a workload that failed the oracle check, was not pinned,
exited early, or ran < 98% of the planned length (partial run); a workload interval not inside the meter window
(interval mismatch); two windows whose meter intervals overlap; PMU running < 99.9% of enabled on the X925 PMU
(multiplexed counts). A round with any refused window, or without its IDLE window, gives no I.

## 9. Normalisation and uncertainty

- Window energy `E_w` per channel = (end - start) scaled to the planned length: `E_w = dE * T_plan / dt`.
  Channels: pkg, cpu_p, cpu_e, and rest = pkg - cpu_p - cpu_e (uncore and DRAM side).
- Analytic edge bound per window: each edge is uncertain by up to one sensor period of power, `P_w * 0.1 s`, uniform,
  so sd `P_w * 0.1 / sqrt(3)` per edge; for I, four windows and two edges each, combined in quadrature.
- Empirical: the SD of I over valid rounds. This covers edges, drift and other tenants and is the one used for
  intervals. The analytic bound is reported next to it.

## 10. Check (a): interval on I

Per round r: `I_r = E_AB - E_A - E_B + E_0` on each channel. Primary statistic: mean over valid rounds with 95%
Student-t interval (n - 1 degrees of freedom). Secondary: 95% bootstrap percentile interval of the mean
(splitmix64 seed 0x15, 10,000 resamples, same generator as r15_reduce). I is distinguishable from zero iff the
primary pkg interval excludes zero.

Check (a) PASSES (the measurement answers the question) iff >= 8 of 10 rounds are valid and the primary pkg interval
half-width is <= 2% of the mean `E_AB` pkg. The finding (zero, positive, negative) is stated separately from pass/fail.

## 11. Check (b): held-out coalition prediction

Model (fixed now), per window, pkg channel:

    E_w = p0 * T_plan + a * CYC_w + b * INST_w

`CYC_w`, `INST_w` = sum over the window's workload processes of X925 PMU `cpu_cycles` and `inst_retired`
(0 for IDLE). Fit: ordinary least squares on every valid IDLE, A and B window of the case. Held out: every AB window
(never used in the fit). The fit reports its coefficients, residual SD `s` (n - 3 degrees of freedom) and the
condition number of the column-scaled design; above 1e4 the fit is DEGENERATE and check (b) FAILS.

Prediction for an AB window uses the counters measured in that AB window. Per-window 95% prediction interval
`+- t * s * sqrt(1 + h)`, h the leverage.

Check (b) PASSES iff the fit is not degenerate, the 95% t-interval of the mean held-out error (observed - predicted)
contains zero, AND at least 80% of held-out windows fall inside their prediction intervals.

Reported next to it (not pass/fail): the naive additive model `E_A + E_B - E_0` (its error is exactly -I); the same
counter model on `cpu_p`; a variant with `l2d_cache_refill` in place of `inst_retired`.

## 12. Attribution records and conservation

Per valid AB window two attributions are built and checked:

1. Solo counterfactual: idle share `E_0` (COUNTERFACTUAL), A `E_A - E_0`, B `E_B - E_0` (COUNTERFACTUAL), remainder
   = measured - allocated. A remainder >= 0 is recorded UNATTRIBUTED. If allocated exceeds measured by more than the
   tolerance (1.96 x empirical SD of I, floored at the analytic edge bound) the attribution is REFUSED.
2. Counter model (only if check (b) passes): idle `p0 * T_plan` (MODELLED), A and B from their own AB-window counters
   (COUNTER_DERIVED), remainder UNATTRIBUTED, same refusal rule.

The measured window total is DIRECT. Confidence scale and its mapping to physics FORGE V2 `energy_source`
(`forge/v2/forge_substrate_v2.h`, 3 levels), fixed in `src/turing/ty_energy.h`:

| TY confidence | meaning | FORGE V2 energy_source |
|---|---|---|
| DIRECT | read from the meter for exactly this interval and boundary | MEASURED (1) |
| COUNTER_DERIVED | a measured total split by measured per-participant counters | MEASURED (1) |
| CAUSAL_RUNTIME | split by the runtime's own causal record of who ran | ESTIMATED (2) |
| COUNTERFACTUAL | taken from a different window (solo or idle) | ESTIMATED (2) |
| MODELLED | from a fitted or cost model, not read for this interval | ESTIMATED (2) |
| UNATTRIBUTED | measured but assigned to nobody | NOT_MEASURED (0) |

COUNTER_DERIVED maps to MEASURED because FORGE has no finer level; the TY record keeps the finer label. FORGE is not
changed.

Records use new digest domains `turing.resource_interval.v0` and `turing.resource_attribution.v0`
(SHA-256(domain || 0x00 || OMG0 bytes), as in `src/turing/field.c`), integers only (µJ, ns). No existing `turing.*`
encoding or golden changes.

## 13. Quiet machine and safety

`tools/ty_energy_run.sh` runs detached (setsid nohup). Before each stage it waits until load1 < 2, no other `make`,
bench or qualify process runs, and `~/workspace/.spark-quiet` is absent; then it creates its own quiet flag, runs
the stage, and removes the flag only if it is still its own. It never kills anything. Other tenants that start
later are recorded per window (background busy time), not hidden.

## 14. R16 loop inventory

The rounds, idle waits and the between-window pause live in shell (outside the R16 scanner's file types). The C files
hold only a deadline-bounded call loop and bounded for loops with none of the scanner's wait words, so they add no
loop-inventory site. Checked with `tools/r16_loop_inventory.sh` after coding; the result is recorded in the result
document.

## Amendments

(none)
