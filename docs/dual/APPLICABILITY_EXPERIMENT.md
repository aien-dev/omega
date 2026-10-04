# DUAL applicability experiment: when is aggregate load control predictive?

**Status:** EXPERIMENTAL. Offline evidence lane of ADR 0031 (ARCH-0031, DUAL constraint pricing). Not a production gate, not a production classifier. Nothing in this experiment is linked by `src/runtime/` or read by any production path. It recommends a future regime classifier; it does not insert one.

**Question.** DUAL prices scarcity from aggregate densities (used slots over capacity, used memory over budget, queue density, arrival and service rates). When does such a macroscopic view predict what a discrete scheduler will actually do next, and when does the identity of individual items dominate so that per-item scheduling must stay in charge?

**Where things live.** Library `src/dual_experiment/` (simulator, aggregate predictor, measurements; pure: no I/O, no allocation, no globals, `nm -u` purity check against `EST_FORBIDDEN`), runner `tools/dual_experiment/applic_run.c` (writes CSV and Markdown), tests `tests/dual_experiment/test_applic.c`, make fragment `mk/dual_applicability.mk` (not in `all` or `test`), outputs `evidence/DUAL/applicability/`. Layout follows the estimation and ANS modules (`src/<module>`, `tests/<module>`, `tools/<module>`, one `mk/*.mk`). The name `dual_experiment` is deliberate: the DUAL record module (`src/dual/`) is written in parallel and this experiment does not depend on it.

## 1. Method

### 1.1 Discrete workload simulator (exact outcome)

Bounded pool of work items, each with a service demand (steps), a slot need, a memory need and a queue class (0 = highest priority). Fixed capacity: 32 slots, 1024 memory units, queue of 256 (overflow is REJECTED and ledgered), 4 classes. One step: arrivals; admission in class-then-FIFO order with backfill against slot and memory capacity; a step is **blocked** if work still waits after admission; one unit of service for every running item; completion frees slots and memory. The exact ledger `arrived = completed + queued + running + rejected` is checked after every step (test) and summed per window.

Eight seeded workloads, 2000 steps each (100 windows of 20 steps, first 5 windows unscored):

| id | workload | what changes |
|---|---|---|
| a | homogeneous high-fan-out | near-identical items (demand 11..13, 1 slot), Poisson 2.2/step, offered load about 0.82 |
| b | heterogeneous tool-heavy | log-normal demand (median 6, sigma 1.2, cap 400), slots 1/2/4, skewed memory and classes, Poisson 1.2/step |
| c | step load | a-items, rate 1.0 then 3.0 after step 1000 (sustained overload, rejections) |
| d | square-wave congestion | a-items, rate alternates 0.8 / 3.2 every 100 steps |
| e | bursty arrivals | a-items, burst of 15..35 items with probability 0.08 per step, else Poisson 0.4 |
| f | capacity reduction | a-items at 2.2/step, slot capacity 32 then 20 after step 1000 |
| g | regime change | a-items at 2.2/step, then b-items at 1.2/step after step 1000 |
| h | heterogeneity sweep | b-family with sigma = 1.5 * seed_index / 23, arrival rate set so offered load stays 0.80 |

24 seeds per workload (192 runs). Every constant is in `src/dual_experiment/applic_params.h`; `ap_params_text()` renders them canonically and SHA-256 of that text is the **parameter digest**, pinned in the test and written into every results file. Changing a parameter breaks the pin on purpose: that is a new experiment version, not a rescoring.

### 1.2 Aggregate predictor (flow balance v2)

The predictor sees one window record only (densities, counts in system, last window's arrival count, running means of demand, slots and memory over completed items). It never sees the item list.

```text
resident_cap = min(slots / mean_slots, memory / mean_mem)
cap_W        = resident_cap * W / mean_demand
frac         = min(1, W / mean_demand)
completions  = min(cap_W, running*frac + queue*frac + A * max(0, W - mean_demand) / W)
in_next      = in_system + A - completions
queue        = max(0, in_next - resident_cap) / queue_max
blocked      = clamp((in_next / resident_cap - 0.75) / 0.25, 0, 1)
```

`A` is the arrival forecast. Three variants are scored: **real** (A = last window's count), **known arrivals** (A = next window's true count; still aggregate-only, isolates aggregation error from forecast error) and **shuffled** (record taken from a different window; negative control).

Error per run: completions NRMSE (RMSE over scored windows divided by the mean actual completions per window; **primary measure**), queue-density RMSE and blocked-fraction RMSE (both already in [0, 1]), and their mean (composite).

A first draft of the predictor (v1) counted items already running as newly offered work and therefore always predicted full capacity and full blocking. It was replaced before any result was recorded; no v1 number appears anywhere.

### 1.3 Heterogeneity measurements

Per run, on the realized item list: coefficient of variation of service demand (`cv_demand`); load share of the top ceil(5 %) items by load, load = demand * slots (`top5_share`); load share of the single largest item (`max_item_share`); normalized Shannon entropy of the class mix (`class_entropy`); and the coefficient of variation of per-window arrival counts (`arrival_cv`, temporal heterogeneity, added so time-varying load can be told apart from item heterogeneity). Two deliberately broken "measurements" are carried as negative controls: a constant (0.5) and seeded noise independent of the run.

### 1.4 Tests (`make test-dual-applicability`)

Purity (`nm -u` on every library object against `EST_FORBIDDEN`); determinism (same seed gives byte-identical windows, items and campaign results digest; a different seed gives a different digest); ledger at every step for every workload and three seeds, plus the window sums and capacity bounds, plus a hostile corrupted-ledger check; predictor finite and bounded on every window of every workload; shuffled control worse than real in the majority of runs; constant metric has exactly zero covariance; parameter digest equals the pin; plain and ASan/UBSan builds.

## 2. Results (parameter digest `e489d25f...87ecea`, results digest `912fb51b...b0c45a`, full tables in `evidence/DUAL/applicability/results.md`)

Primary measure, completions NRMSE, mean over 24 seeds (known-arrivals variant in parentheses):

| workload | completions NRMSE | blocked RMSE | cv_demand | top5 share |
|---|---|---|---|---|
| a homogeneous | 0.110 (0.078) | 0.391 | 0.07 | 0.05 |
| b heterogeneous | 0.220 (0.158) | 0.275 | 1.70 | 0.37 |
| c step | 0.099 (0.069) | 0.061 | 0.07 | 0.05 |
| d square wave | 0.174 (0.068) | 0.217 | 0.07 | 0.05 |
| e bursty | 0.171 (0.136) | 0.320 | 0.07 | 0.05 |
| f capacity drop | 0.100 (0.074) | 0.277 | 0.07 | 0.05 |
| g regime change | 0.151 (0.108) | 0.312 | 1.01 | 0.27 |
| h sweep (mean) | 0.197 (0.122) | 0.253 | 0.97 | 0.27 |

Correlations over all 192 runs (Pearson r with completions NRMSE): cv_demand +0.70, top5_share +0.69, max_item_share +0.61, class_entropy -0.67, arrival_cv +0.07; constant control 0 (zero covariance), noise control +0.10. Within the sweep alone (one variable, constant offered load): r(cv_demand) = +0.88 real, +0.92 with arrivals known; r(top5_share) = +0.90 / +0.93. Throughput error rises monotonically from 0.17 (sigma 0) to 0.31 (sigma 1.5), and with arrivals known from 0.07 to 0.27, so the growth is aggregation error, not forecast noise.

Negative controls: shuffled predictor worse than real in 192 / 192 runs (CONTROL PASS); constant metric covariance exactly 0 on every error axis.

Regime grid (pre-registered thresholds, primary measure): runs with cv_demand < 0.8 and top5_share < 0.20 (129 runs): mean 0.133, p90 0.183, max 0.254. Complement cv >= 1.2 or top5_share >= 0.30 (37 runs): mean 0.218, p10 0.189, p90 0.256. Temporal complement arrival_cv >= 0.5 (71 runs): mean 0.148, p90 0.195.

## 3. Reading the results

1. **Throughput is where aggregate pressure is useful.** With near-identical items the aggregate flow model predicts next-window completions within about 11 % (8 % once the arrival count is known, which is close to the Poisson floor). Step load, capacity drop and even sustained overload stay near 10 %: the aggregate view handles changes in *how much* work arrives and *how much* capacity exists.
2. **Item heterogeneity is where it fails, and the failure is in the aggregation itself.** Error roughly doubles for the long-tailed family and grows monotonically with the spread of service demand even when arrivals are known exactly. The running mean demand is estimated from completions, so short items bias it low while a few long items hold slots the aggregate cannot see: concentration (top-5 % share) tracks the error as well as the coefficient of variation does.
3. **Time-varying load is a forecasting problem, not an aggregation problem.** Square wave and bursts carry large error with last-window arrivals (0.17) and small error with known arrivals (0.07, 0.14). `arrival_cv` is uncorrelated with throughput error overall and strongly correlated with queue error (+0.77).
4. **Waiting is not predictable from aggregates in any regime short of sustained overload.** Blocked-fraction RMSE is 0.2 to 0.4 everywhere except the step workload, where the system is saturated and blocking is trivially 1. Whether an arriving item waits a step is decided by discrete slot availability, which the flow model has no representation of. Any use of aggregate pressure for per-item admission or latency promises would be unsupported by this evidence.

## 4. Candidate regime rule (PROPOSAL ONLY, inserted nowhere)

```text
PROPOSED, NOT ADOPTED:
  aggregate throughput prediction is usable (p90 NRMSE <= ~0.18)
      when cv_demand < 0.8 AND top-5% load share < 0.20;
  discrete per-item scheduling must dominate (p10 NRMSE >= ~0.19)
      when cv_demand >= 1.2 OR top-5% load share >= 0.30;
  the band between is a transition region, not a decision;
  blocked / waiting fractions are out of scope for aggregate prediction in every regime
      except sustained overload.
```

The thresholds come from the pre-registered grid, but the choice of which grid cell to quote was made after seeing the numbers; they are a hypothesis for a future, separately pre-registered regime classifier (DUAL-3 analysis, ADR 0031 §8), not a rule in any code path. A future classifier should take at least two inputs (an item-heterogeneity measure and a temporal-change measure) because the two failure modes here are different and need different remedies (better per-item representation versus better arrival forecasting).

## 5. Limits

- Synthetic workloads with pre-registered generators; **not qualified against any real trace** (no serving-scheduler trace, no J-Space residency series). Real workloads may be more or less heterogeneous than any family here.
- One admission policy (class-then-FIFO with backfill), one capacity shape, one predictor (a deliberately simple flow balance, not a tuned one). A better aggregate predictor would lower the absolute numbers; the claim is about the *trend* with heterogeneity, which the known-arrivals variant isolates.
- The sweep confounds spread with seed (one seed per sigma); the whole-campaign correlation and the per-workload seeds carry the replication.
- The regime thresholds are quoted after seeing the data (section 4) and the grid is coarse (3 x 3).
- The blocked-fraction heuristic (linear ramp from 75 % predicted occupancy) is pre-registered but arbitrary; its poor performance is a finding about the quantity, not a tuned verdict.
- No energy, no thermal state, no uncertainty bands: this experiment measures predictability of a discrete scheduler from aggregates, nothing about ADR 0031 prices themselves.

Verdict: EXPERIMENTAL; recommends a future regime classifier; nothing inserted into production.
