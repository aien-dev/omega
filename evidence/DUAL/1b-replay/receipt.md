# DUAL-1b receipt: deterministic replay of the reference controller over digest-bound traces (ADR 0031 s5, s8 DUAL-1)

Gate: `DUAL_PRICE_REFERENCE`. Host-only, no GPU, no production influence.

EVERY input and EVERY replayed price in this receipt is UNCALIBRATED: EST-3 has not passed for any signal on main, no
calibration receipt exists, every synthetic trace carries calibrated = 0, and every recorded trace carries calibrated = 0.
No price computed here may influence any production decision. The recorded traces are perf cycle counters, which are
exact counts but are not calibrated estimates of any resource pressure; they are used only as input streams.

| Field | Value |
|---|---|
| repository | aien-dev/omega |
| base commit | 0b3dcc0efe4210e533fd5cfcc675aada371c9fc9 (branch dual/0b-1a-binding-controller, PR #267, on dual/0a-records PR #266, on main cb7cc216147664549484fcca8e8ceadbd6f6cdbf) |
| pre-registration commit | the commit that adds this file with the sections "Pre-registered" filled and "Results" empty |
| candidate commit | filled in the Results section |
| compiler | cc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0 |
| flags | EST_CFLAGS (-std=c11 -Wall -Wextra -Werror -pedantic -O2 -ffp-contract=off -fno-fast-math) and EST_ASAN |
| host | Spark aarch64 Linux, host-only |

## Pre-registered: trace format
Kind byte 8, version 1, digest domain `omega.dual.trace.v1`: u32 trace_kind, u64 seed, u32 n_ticks, then per tick
u64 generation, f64 estimate, f64 uncertainty, u32 calibrated, u32 evidence_verified, u32 regime_change. Little-endian,
-0.0 written as +0.0, NaN/Inf refused. Trace digest = SHA-256(domain || 0x00 || encoding). Generators use xorshift64*
from the declared seed. Replay log = the RxDualConstraintState digest of every tick, in order; replay digest =
SHA-256("omega.dual.replay.v1" || 0x00 || controller_id || trace_digest || u32 n || the n state digests).
Recorded traces: trace_kind RECORDED, seed 0, estimate = the perf cycle count of one (CPU, event) series, uncertainty 0,
calibrated 0, evidence_verified 1, regime_change 0, generation = row index + 1; the raw CSV bytes are SHA-256 digested
and listed below. The R15 machine-perf.csv and preflight-perf.csv files contain ONLY perf-stat cycle counters
(armv8_pmuv3_*/cycles/ per CPU and smmuv3_pmcg_*/cycles/); they contain no thermal, power or clock-frequency column,
so the per-CPU cycles-per-second series is the only stream available and is used as a clock-activity proxy.

## Pre-registered: common parameters
- Resource: unit BYTES (synthetic) or DIMENSIONLESS (recorded; the unit registry has no cycles unit), scale 1000 (synthetic), 1e9 (recorded).
- Budget: 5000 (synthetic, except where stated); 1e9 cycles per second (recorded).
- Class SOFT. budget_contract = fixed stand-in digest 0x22 repeated (0x23 repeated for the second chain of scenario 7).
- Reference controller: eta 0.1, rho 0.05, k_sigma 2.0, max_age 4, cadence 1, lambda_max 10 per resource; n = 1 except
  scenario 5/6 (n = 2, ids 1, 2) and scenario 10 (n = 32, ids 1..32).
- Hostile controller (negative control): eta 5.0, rho 0.0, k_sigma 2.0, max_age 4, cadence 1, lambda_max 10, n = 1.
- now_generation = tick generation (no staleness is injected in these scenarios). n_ticks = 200 unless stated.
- K (recovery ticks) = ceil(ln(100) / (-ln(1 - rho))) = ceil(89.78) = 90 for rho 0.05.

## Pre-registered: scenarios (seeds are part of the trace identity)
| # | Scenario | Trace kind | Seed | Definition |
|---|---|---|---|---|
| 1 | constant low load | 1 | 0x1001 | estimate 4000, sd 100, all ticks |
| 2 | single binding constraint | 2 | 0x1002 | estimate 5500, sd 100, all ticks |
| 3 | abrupt step load | 3 | 0x1003 | estimate 4000 for t < 100, 6000 for t >= 100, sd 100 |
| 4 | square-wave congestion | 4 | 0x1004 | period 50: 25 ticks at 6000 then 25 at 4000, 4 periods, sd 100 |
| 5 | competing: memory | 5 | 0x1005 | resource 1: estimate 6000, sd 50 (one controller n = 2 prices both chains) |
| 6 | competing: latency | 6 | 0x1005 | resource 2: estimate 4900, sd 50 (inside the deadband: pressure -0.1, band 0.1) |
| 7 | sudden capacity reduction | 7 | 0x1007 | estimate 4500, sd 100; budget 5000 for t < 100 (chain A), 3000 for t >= 100 (chain B, new budget_contract, new chain at tick 0). A budget is declared by a contract (budget_contract), so a budget change is a new contract and a new constraint chain; the engine does not mutate a budget inside a chain |
| 8 | workload regime change | 8 | 0x1008 | estimate 5500, sd 100; regime_change = 1 for 80 <= t < 120 |
| 9 | bursty queue arrivals | 9 | 0x1009 | estimate 4200 + burst; each tick with probability 1/8 (xorshift) a burst uniform in [800, 2400]; sd 100 |
| 10 | high branch fan-out | 10 | 0x100A + r | 32 resources (r = 0..31), each a constant estimate 5500 + offset_r, offset_r uniform in [-50, 50] from seed 0x100A + r; sd 100; one controller n = 32 |
| 11 | KV saturation approach | 11 | 0x100B | estimate 3000 + 4000 * t / 199 (crosses budget + band = 5200 near t = 109.5), sd 100 |
| 12 | recovery after pressure | 12 | 0x100C | estimate 6000 for t < 100, 5000 for t >= 100 (inside the band: leak only), sd 100 |
| 13 | recorded R15 | 13 | 0 | machine-perf.csv CPU0 armv8_pmuv3_0/cycles/ and CPU7 armv8_pmuv3_1/cycles/; preflight-perf.csv CPU7 armv8_pmuv3_1/cycles/; both R15 runs (6 traces) |

## Pre-registered: measures and thresholds (frozen before the first replay run)
Reversal count R(x) of a series x: number of sign changes between consecutive nonzero first differences.
| Id | Measure | Threshold | Applies to |
|---|---|---|---|
| M1 | hard bounds: lambda finite, >= 0, <= lambda_max at every tick | every tick | all scenarios, both controllers |
| M2 | constant-load tail: R(lambda) over the final 50 ticks | = 0 | 1, 2, 3, 5, 6, 7 (chain B), 8, 10 (each chain), 12 |
| M3a | square wave reversals: R(lambda) over the whole trace | <= 2 * periods = 8 | 4 |
| M3b | square wave amplitude: max(lambda) - min(lambda) over the final full period (t 150..199) | <= 0.25 * lambda_max = 2.5 | 4 |
| M4 | step response: lambda(t+1) >= lambda(t) - 1e-12 * (1 + lambda(t)) for every t at or after the step | all post-step ticks | 3 (t >= 100), 7 chain B (all), 11 (t >= first tick with estimate > 5200), 2 (all) |
| M5 | recovery: lambda[t_off - 1 + K] <= 0.01 * lambda[t_off - 1], t_off = 100, K = 90 | holds | 12 |
| M6 | regime: every tick with regime_change = 1 is FROZEN with lambda, estimate and generation equal to the previous tick | every such tick | 8 |
| M7 | uncalibrated: every tick with calibrated = 0 has state UNCALIBRATED (never FRESH) | every tick | all scenarios (all ticks are calibrated = 0), recorded |
| M8 | no input-free oscillation: R(lambda) <= R(estimate) + 1 per chain | holds | all scenarios incl. recorded, both controllers |
| M9 | competing: final lambda(memory) > 0 and final lambda(latency) = 0 | holds | 5, 6 |
| M10 | fan-out spread: max - min of the 32 final lambdas | <= 0.25 (eta * max band-shifted gradient spread / rho = 0.1 * 0.1 / 0.05 = 0.2, plus margin) | 10 |
| M11 | KV: lambda = 0 for every tick with estimate <= 5200 | holds | 11 |
| D1 | determinism: two replays of the same trace with the same controller give identical replay digests | identical | all |
| D2 | golden: trace digest and replay digest of scenarios 2 and 4 equal hex constants committed in the test (cross-process) | identical | 2, 4 |
| D3 | tamper: flipping one bit of the encoded estimate at tick 17 changes the trace digest and the replay digest | both differ | 2 |
| D4 | calibrated variant: scenario 2 with calibrated = 1 (stand-in digest, a FORMAT exercise, not a calibration claim) gives identical lambdas, FRESH states, and a different replay digest | holds | 2 |
| H1 | hard constraint through the replay entry point (class INVARIANT, class UNDECLARED) | refused, no record | 2 |

Expected analytic values for the reference controller (from the closed form in tests/dual/dual_ref.c): scenario 2 fixed
point eta*g/rho = 0.1*0.3/0.05 = 0.6; scenario 3 post-step fixed point 1.6; scenario 4 peak per period
1.6 * (1 - 0.95^25) = 1.156 (M3b margin 2.5 - 1.16); scenario 7 chain B fixed point 2.6; scenario 12 peak 1.6 * (1 - 0.95^100) = 1.59.

Note on M3a/M8 (stated before running): for the update lambda' = clip((1 - rho) lambda + eta g) the first difference
d(t) = eta g(t) - rho lambda(t) obeys d(t+1) = (1 - rho) d(t) + eta (g(t+1) - g(t)) while unclipped, so within a run of
same-sign changes of g the sign of d changes at most once, and clipping cannot add a reversal under monotone g. Hence
R(lambda) <= R(g) + 1 <= R(estimate) + 1 for EVERY valid controller, including the hostile one. Reversal counts therefore
cannot distinguish a good controller from a bad one on a noise-free wave; the discriminating oscillation measure is the
amplitude bound M3b, which the hostile controller is expected to fail.

## Pre-registered: negative control (expected outcomes)
| Controller | Scenario | Expected |
|---|---|---|
| hostile (eta 5.0, rho 0.0) | 4 square wave | M1 PASS, M3a PASS (2 per period), M3b FAIL: lambda slams to lambda_max within 3 ticks of each rising edge and to 0 within 3 ticks of each falling edge, amplitude 10 > 2.5. Gate verdict for this configuration: FAIL |
| reference | 4 square wave | M1, M3a, M3b, M8 PASS |

Verdict rule: `DUAL_PRICE_REFERENCE = PASS` only if every applicable measure passes for the reference controller on every
scenario AND the negative control fails M3b as declared. Otherwise FAIL with the numbers. A threshold is never changed
after this commit; a surprise is recorded as FAIL.

## Results
(filled after the run; nothing above this line changes)
