# DUAL applicability experiment: results (DUAL-APPLIC-V1)

EXPERIMENTAL. Synthetic workloads. Not a production gate, not a production classifier. Nothing inserted into production.

- parameter digest (SHA-256 of params.txt): `e489d25f5f0d23cfe8624a5cc82a5f770f04cb03fd3b441aac627e1a0087ecea`
- results digest (SHA-256 of results.csv text): `912fb51b13f981c5a5e4cce28211d6eac62d5e9149e9bc32e44eb9f1acb0c45a`
- runs: 192 (8 workloads x 24 seeds), 100 windows of 20 steps each, 5 warm-up windows unscored

## 1. Aggregate predictor error per workload (mean over seeds, sd in parentheses)

| workload | completions NRMSE (primary) | known-arrivals completions NRMSE | queue RMSE | blocked RMSE | composite | known-arrivals composite | shuffled composite | cv_demand | top5 share | max item share | class entropy | arrival cv | rejected/run |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| a_homogeneous | 0.110 (0.007) | 0.078 (0.005) | 0.009 (0.004) | 0.391 (0.020) | 0.170 (0.007) | 0.146 (0.008) | 0.231 (0.015) | 0.068 | 0.054 | 0.0002 | 1.000 | 0.147 | 0.0 |
| b_heterogeneous | 0.220 (0.020) | 0.158 (0.027) | 0.006 (0.004) | 0.275 (0.066) | 0.167 (0.027) | 0.146 (0.031) | 0.212 (0.032) | 1.698 | 0.372 | 0.0200 | 0.804 | 0.203 | 0.0 |
| c_step | 0.099 (0.007) | 0.069 (0.006) | 0.029 (0.004) | 0.061 (0.026) | 0.063 (0.009) | 0.048 (0.009) | 0.566 (0.214) | 0.068 | 0.054 | 0.0003 | 1.000 | 0.524 | 91.0 |
| d_square_wave | 0.174 (0.009) | 0.068 (0.005) | 0.062 (0.003) | 0.217 (0.023) | 0.151 (0.010) | 0.102 (0.010) | 0.476 (0.081) | 0.068 | 0.054 | 0.0003 | 1.000 | 0.618 | 0.0 |
| e_bursty | 0.171 (0.038) | 0.136 (0.032) | 0.150 (0.017) | 0.320 (0.060) | 0.213 (0.027) | 0.137 (0.027) | 0.426 (0.081) | 0.068 | 0.054 | 0.0002 | 1.000 | 0.648 | 23.9 |
| f_capacity_drop | 0.100 (0.007) | 0.074 (0.006) | 0.022 (0.003) | 0.277 (0.024) | 0.133 (0.009) | 0.112 (0.009) | 0.498 (0.119) | 0.068 | 0.054 | 0.0003 | 1.000 | 0.150 | 271.3 |
| g_regime_change | 0.151 (0.009) | 0.108 (0.010) | 0.006 (0.003) | 0.312 (0.044) | 0.157 (0.015) | 0.133 (0.015) | 0.273 (0.035) | 1.013 | 0.268 | 0.0126 | 0.975 | 0.335 | 0.0 |
| h_hetero_sweep | 0.197 (0.044) | 0.122 (0.058) | 0.005 (0.002) | 0.253 (0.048) | 0.152 (0.028) | 0.120 (0.036) | 0.183 (0.033) | 0.970 | 0.266 | 0.0081 | 0.800 | 0.177 | 0.0 |

## 2. Correlation of aggregate prediction error with heterogeneity measurements (Pearson r over all 192 runs)

| measurement | r vs completions NRMSE (primary) | r vs known-arrivals completions NRMSE | r vs queue RMSE | r vs blocked RMSE | r vs composite | covariance vs completions NRMSE | note |
|---|---|---|---|---|---|---|---|
| cv_demand | +0.696 | +0.719 | -0.432 | +0.151 | +0.218 | +0.021948 |  |
| top5_share | +0.689 | +0.688 | -0.462 | +0.149 | +0.203 | +0.004346 |  |
| max_item_share | +0.606 | +0.656 | -0.377 | +0.177 | +0.224 | +0.000250 |  |
| class_entropy | -0.673 | -0.550 | +0.407 | -0.022 | -0.120 | -0.002776 |  |
| arrival_cv | +0.068 | -0.060 | +0.765 | -0.370 | +0.017 | +0.000678 |  |
| broken_const (control) | +0.000 | +0.000 | +0.000 | +0.000 | +0.000 | +0.000000 | zero variance: covariance exactly 0, r reported as 0 |
| broken_noise (control) | +0.103 | +0.149 | +0.034 | +0.108 | +0.132 | +0.001434 | independent of the run: expected near 0 |

## 2b. Heterogeneity sweep (workload h only, 24 runs, offered load held at 0.80, spread rises with seed index)

- r(cv_demand, completions NRMSE) = +0.882; r(cv_demand, known-arrivals completions NRMSE) = +0.916
- r(top5_share, completions NRMSE) = +0.903; r(top5_share, known-arrivals completions NRMSE) = +0.932

| seed_index | sigma | cv_demand | top5 share | max item share | completions NRMSE | known-arrivals completions NRMSE | blocked RMSE |
|---|---|---|---|---|---|---|---|
| 0 | 0.000 | 0.000 | 0.132 | 0.0005 | 0.172 | 0.068 | 0.196 |
| 1 | 0.065 | 0.074 | 0.137 | 0.0006 | 0.155 | 0.075 | 0.230 |
| 2 | 0.130 | 0.139 | 0.145 | 0.0008 | 0.168 | 0.076 | 0.231 |
| 3 | 0.196 | 0.204 | 0.153 | 0.0009 | 0.138 | 0.071 | 0.197 |
| 4 | 0.261 | 0.275 | 0.165 | 0.0014 | 0.154 | 0.074 | 0.216 |
| 5 | 0.326 | 0.340 | 0.167 | 0.0012 | 0.183 | 0.084 | 0.268 |
| 6 | 0.391 | 0.408 | 0.180 | 0.0014 | 0.160 | 0.088 | 0.219 |
| 7 | 0.457 | 0.466 | 0.186 | 0.0019 | 0.157 | 0.080 | 0.206 |
| 8 | 0.522 | 0.568 | 0.200 | 0.0033 | 0.191 | 0.087 | 0.279 |
| 9 | 0.587 | 0.651 | 0.206 | 0.0027 | 0.167 | 0.081 | 0.205 |
| 10 | 0.652 | 0.714 | 0.219 | 0.0025 | 0.184 | 0.096 | 0.298 |
| 11 | 0.717 | 0.844 | 0.240 | 0.0035 | 0.187 | 0.101 | 0.296 |
| 12 | 0.783 | 0.906 | 0.247 | 0.0085 | 0.163 | 0.088 | 0.218 |
| 13 | 0.848 | 1.111 | 0.268 | 0.0054 | 0.161 | 0.098 | 0.215 |
| 14 | 0.913 | 1.109 | 0.284 | 0.0080 | 0.215 | 0.115 | 0.271 |
| 15 | 0.978 | 1.248 | 0.314 | 0.0151 | 0.189 | 0.109 | 0.273 |
| 16 | 1.043 | 1.279 | 0.314 | 0.0109 | 0.218 | 0.131 | 0.302 |
| 17 | 1.109 | 1.402 | 0.344 | 0.0262 | 0.239 | 0.169 | 0.320 |
| 18 | 1.174 | 1.627 | 0.385 | 0.0127 | 0.219 | 0.145 | 0.230 |
| 19 | 1.239 | 1.721 | 0.368 | 0.0103 | 0.206 | 0.148 | 0.224 |
| 20 | 1.304 | 1.886 | 0.385 | 0.0104 | 0.262 | 0.214 | 0.359 |
| 21 | 1.370 | 2.015 | 0.444 | 0.0217 | 0.283 | 0.251 | 0.364 |
| 22 | 1.435 | 2.054 | 0.412 | 0.0184 | 0.256 | 0.213 | 0.216 |
| 23 | 1.500 | 2.247 | 0.482 | 0.0260 | 0.309 | 0.272 | 0.235 |

## 3. Negative control: shuffled-window predictor must be worse than the real predictor

| workload | runs where shuffled composite > real composite | mean real composite | mean shuffled composite |
|---|---|---|---|
| a_homogeneous | 24 / 24 | 0.170 | 0.231 |
| b_heterogeneous | 24 / 24 | 0.167 | 0.212 |
| c_step | 24 / 24 | 0.063 | 0.566 |
| d_square_wave | 24 / 24 | 0.151 | 0.476 |
| e_bursty | 24 / 24 | 0.213 | 0.426 |
| f_capacity_drop | 24 / 24 | 0.133 | 0.498 |
| g_regime_change | 24 / 24 | 0.157 | 0.273 |
| h_hetero_sweep | 24 / 24 | 0.152 | 0.183 |

shuffled worse in 192 / 192 runs: CONTROL PASS (majority)

## 4. Regime grid (pre-registered thresholds): PRIMARY measure = completions NRMSE of runs with cv_demand < Y and top5 share < Z (composite beside it)

| Y (cv) | Z (top5) | runs | mean completions NRMSE | p90 | max | mean composite | p90 composite | runs also arrival_cv < 0.50 | p90 completions NRMSE (that subset) |
|---|---|---|---|---|---|---|---|---|---|
| 0.4 | 0.10 | 120 | 0.131 | 0.183 | 0.254 | 0.146 | 0.218 | 49 | 0.116 |
| 0.4 | 0.20 | 126 | 0.132 | 0.183 | 0.254 | 0.145 | 0.218 | 55 | 0.138 |
| 0.4 | 0.30 | 126 | 0.132 | 0.183 | 0.254 | 0.145 | 0.218 | 55 | 0.138 |
| 0.8 | 0.10 | 120 | 0.131 | 0.183 | 0.254 | 0.146 | 0.218 | 49 | 0.116 |
| 0.8 | 0.20 | 129 | 0.133 | 0.183 | 0.254 | 0.145 | 0.217 | 58 | 0.155 |
| 0.8 | 0.30 | 132 | 0.134 | 0.184 | 0.254 | 0.145 | 0.217 | 61 | 0.164 |
| 1.2 | 0.10 | 120 | 0.131 | 0.183 | 0.254 | 0.146 | 0.218 | 49 | 0.116 |
| 1.2 | 0.20 | 129 | 0.133 | 0.183 | 0.254 | 0.145 | 0.217 | 58 | 0.155 |
| 1.2 | 0.30 | 155 | 0.137 | 0.184 | 0.254 | 0.146 | 0.210 | 84 | 0.166 |

complement (cv >= 1.2 or top5 >= 0.30): 37 runs, mean completions NRMSE 0.218, p10 0.189, p90 0.256
temporal complement (arrival_cv >= 0.50): 71 runs, mean completions NRMSE 0.148, p10 0.095, p90 0.195

Verdict line: EXPERIMENTAL; recommends a future regime classifier; nothing inserted into production.
