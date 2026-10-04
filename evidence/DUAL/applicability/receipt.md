# Receipt: DUAL applicability experiment (DUAL-APPLIC-V1)

**Verdict: EXPERIMENTAL; recommends a future regime classifier; nothing inserted into production.**

| field | value |
|---|---|
| repository | aien-dev/omega (fresh clone, `~/workspace/dual/omega-applic`) |
| base (GitHub `main` at clone) | `cb7cc216147664549484fcca8e8ceadbd6f6cdbf` |
| candidate (code, docs, results) | `2a582fded48102a713a2790a613e1936a5beb032` on branch `dual/applicability-experiment` |
| tree state at run | clean (`git status --porcelain` empty before and after the run) |
| host | spark-b87b, aarch64, Linux 7.0.0-1019-nvidia; host-only, no accelerator, no chip time |
| compiler | `cc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0`, GNU Make 4.3 |
| flags | `EST_CFLAGS` (`-std=c11 -Wall -Wextra -Werror -pedantic -O2 -ffp-contract=off -fno-fast-math`), ASan build adds `-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all` |
| workstream | ADR 0031 / ARCH-0031 (DUAL constraint pricing), offline applicability lane; no DUAL-N gate token claimed |
| scope | experiment only; no production gate, no production classifier; `src/runtime/` untouched; no dependency on `src/dual/` |

## Pre-registration

- Parameters: `src/dual_experiment/applic_params.h`, rendered canonically by `ap_params_text()`; written to `params.txt`.
- **Parameter digest (SHA-256 of params.txt):** `e489d25f5f0d23cfe8624a5cc82a5f770f04cb03fd3b441aac627e1a0087ecea`, pinned in `tests/dual_experiment/test_applic.c` (`AP_PARAMS_DIGEST_PIN`). Any parameter change fails the test: new experiment version, no rescoring.
- Predictor formulas and error measures: `src/dual_experiment/applic_predict.c` header comment and `docs/dual/APPLICABILITY_EXPERIMENT.md` §1.2. A v1 draft that counted running items as offered work was replaced before any result was recorded; no v1 number is reported.
- Primary measure: completions NRMSE. Regime grid thresholds: cv in {0.4, 0.8, 1.2}, top-5 % share in {0.10, 0.20, 0.30}, arrival_cv 0.5.

## Commands and results (run at candidate SHA, clean tree)

```text
$ make test-dual-applicability
dual-applic-purity: applic_sim.o, applic_predict.o and applic_metrics.o reference no forbidden symbol
./build/dual-applicability/test_applic
results digest: 912fb51b13f981c5a5e4cce28211d6eac62d5e9149e9bc32e44eb9f1acb0c45a
params digest:  e489d25f5f0d23cfe8624a5cc82a5f770f04cb03fd3b441aac627e1a0087ecea
shuffled worse than real in 192 / 192 runs
test_applic: 13020 checks, 0 failures
./build/dual-applicability/test_applic_asan
results digest: 912fb51b13f981c5a5e4cce28211d6eac62d5e9149e9bc32e44eb9f1acb0c45a
params digest:  e489d25f5f0d23cfe8624a5cc82a5f770f04cb03fd3b441aac627e1a0087ecea
shuffled worse than real in 192 / 192 runs
test_applic: 13020 checks, 0 failures
test-dual-applicability: determinism, ledger, predictor and negative-control checks pass in plain and ASan/UBSan builds

$ make dual-applic-run
wrote evidence/DUAL/applicability/results.csv, params.txt, results.md

$ sha256sum evidence/DUAL/applicability/results.csv params.txt results.md
912fb51b13f981c5a5e4cce28211d6eac62d5e9149e9bc32e44eb9f1acb0c45a  results.csv   (193 lines: header + 192 runs)
e489d25f5f0d23cfe8624a5cc82a5f770f04cb03fd3b441aac627e1a0087ecea  params.txt
c4803da3fa8844b991f3cfea63352a61e8dddadfb975192e8c54892c1d3aa9d5  results.md
```

Determinism: the results digest printed by the plain build, by the ASan/UBSan build and the SHA-256 of the written `results.csv` are the same value. Re-running `make dual-applic-run` on the committed files left the tree clean (byte-identical rewrite).

Purity: `nm -u` on `applic_sim.o`, `applic_predict.o`, `applic_metrics.o` against `EST_FORBIDDEN` (`rx_|aienos_|argus_|forge_|aegis|mmap|mprotect|fork|exec|dlopen|system|socket|fopen|open$`): no hit.

## Results table (mean over 24 seeds; full tables with sd in `results.md`)

| workload | completions NRMSE (primary) | known-arrivals NRMSE | queue RMSE | blocked RMSE | shuffled composite vs real | cv_demand | top5 share | arrival cv | rejected/run |
|---|---|---|---|---|---|---|---|---|---|
| a homogeneous | 0.110 | 0.078 | 0.009 | 0.391 | 0.231 vs 0.170 | 0.068 | 0.054 | 0.147 | 0 |
| b heterogeneous | 0.220 | 0.158 | 0.006 | 0.275 | 0.212 vs 0.167 | 1.698 | 0.372 | 0.203 | 0 |
| c step | 0.099 | 0.069 | 0.029 | 0.061 | 0.566 vs 0.063 | 0.068 | 0.054 | 0.524 | 91.0 |
| d square wave | 0.174 | 0.068 | 0.062 | 0.217 | 0.476 vs 0.151 | 0.068 | 0.054 | 0.618 | 0 |
| e bursty | 0.171 | 0.136 | 0.150 | 0.320 | 0.426 vs 0.213 | 0.068 | 0.054 | 0.648 | 23.9 |
| f capacity drop | 0.100 | 0.074 | 0.022 | 0.277 | 0.498 vs 0.133 | 0.068 | 0.054 | 0.150 | 271.3 |
| g regime change | 0.151 | 0.108 | 0.006 | 0.312 | 0.273 vs 0.157 | 1.013 | 0.268 | 0.335 | 0 |
| h heterogeneity sweep | 0.197 | 0.122 | 0.005 | 0.253 | 0.183 vs 0.152 | 0.970 | 0.266 | 0.177 | 0 |

Correlation with completions NRMSE, Pearson r over 192 runs: cv_demand +0.696, top5_share +0.689, max_item_share +0.606, class_entropy -0.673, arrival_cv +0.068. Sweep only (24 runs, offered load fixed at 0.80): r(cv_demand) +0.882 (known arrivals +0.916), r(top5_share) +0.903 (known arrivals +0.932); completions NRMSE rises monotonically 0.172 -> 0.309 with sigma 0 -> 1.5 (known arrivals 0.068 -> 0.272).

Regime grid, primary measure: cv < 0.8 and top5 < 0.20: 129 runs, mean 0.133, p90 0.183, max 0.254. Complement cv >= 1.2 or top5 >= 0.30: 37 runs, mean 0.218, p10 0.189, p90 0.256. arrival_cv >= 0.5: 71 runs, mean 0.148, p90 0.195.

## Negative controls

| control | expectation | result |
|---|---|---|
| shuffled-window predictor (densities from a different window) | worse than the real predictor | worse in **192 / 192** runs; per workload 24/24 each; CONTROL PASS |
| constant "heterogeneity" metric (0.5) | no correlation | covariance exactly 0.000000 on every error axis, r reported 0 |
| seeded noise metric independent of the run (extra) | near zero | r = +0.103 (primary), +0.149 (known arrivals), +0.034 (queue), +0.108 (blocked) |
| hostile corrupted ledger (test) | detected | `ap_sim_ledger_ok` returns 0 after `completed++`, 1 after restoring |

## Conclusion

Aggregate pressure predicts **throughput** well when items are near-identical, including under step load, capacity loss and sustained overload (about 10 % error, 7 to 8 % once the arrival count is known). It degrades monotonically with item heterogeneity, and the degradation survives knowing the arrivals exactly, so it is aggregation error: a few long items hold slots the aggregate cannot see. Time-varying load is a forecasting problem (large error with last-window arrivals, small with known arrivals), not an aggregation problem. **Waiting (blocked fraction) is not predictable from aggregates** in any regime short of saturation. Candidate regime rule, PROPOSAL only, inserted nowhere: aggregate throughput prediction usable (p90 NRMSE <= about 0.18) when cv_demand < 0.8 and top-5 % load share < 0.20; discrete per-item scheduling must dominate (p10 NRMSE >= about 0.19) when cv_demand >= 1.2 or top-5 % share >= 0.30; the band between is a transition region; blocked fractions are out of scope.

## Limits

Synthetic workloads only, not qualified against any real trace; one admission policy, one capacity shape, one deliberately simple predictor; the sweep confounds spread with seed; the quoted thresholds were chosen from the pre-registered grid after seeing the numbers; the blocked-fraction heuristic is arbitrary (pre-registered). No claim about ADR 0031 prices, energy, thermal state or uncertainty.

Verdict: EXPERIMENTAL; recommends a future regime classifier; nothing inserted into production.
