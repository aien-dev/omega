# DUAL-1b receipt: deterministic replay of the reference controller over digest-bound traces (ADR 0031 s5, s8 DUAL-1)

Gate: `DUAL_PRICE_REFERENCE`. Host-only, no GPU, no production influence.

EVERY input and EVERY replayed price in this receipt is UNCALIBRATED: EST-3 has not passed for any signal on main, no
calibration receipt exists, every synthetic trace carries calibrated = 0, and every recorded trace carries calibrated = 0.
No price computed here may influence any production decision. The recorded traces are perf cycle counters, which are
exact counts but are not calibrated estimates of any resource pressure; they are used only as input streams.

| Field | Value |
|---|---|
| repository | aien-dev/omega |
| base commit | 0899c412c33eb37a0012170de7604e4dbd4bf963 (branch dual/0b-1a-binding-controller, PR #267, on dual/0a-records PR #266, on main after the AGPL relicense 559f6e5; the pre-registration was first committed on the pre-rebase base 0b3dcc0efe4210e533fd5cfcc675aada371c9fc9 and rebased unchanged) |
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

| Field | Value |
|---|---|
| pre-registration commit | d51d418577d14f5f8d1e922051597bfb089461d4 (receipt sections above, no replay code; b0626246f3b6a03326420fafba502c3b5398c4c8 before the base rebase, same content) |
| candidate commit | this commit (branch dual/1b-replay) |
| compiler | cc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0, aarch64 |
| reference controller_id | bcf4285a4830663a1c9f0548a0bd9fe1c96ca9852a8c90d7e45b13f56d820ec4 (eta 0.1, rho 0.05, k_sigma 2, max_age 4, cadence 1, n 1, lambda_max 10) |
| competing controller_id (n = 2) | e4a8317c4e867e8cb49f28aa8275d2b2da8abf910575d88f5bb89edaffc41b11 |
| fan-out controller_id (n = 32) | b1af2f7e5df739fdb93534ac1ace836ade213f21e03bb20d1b28abad4bcddb27 |
| hostile controller_id | 1d08ce016bbbc47a506145813cd59d5f57bd687929f0a89ad3e1dec4d4d6a1c1 (eta 5.0, rho 0.0, k_sigma 2, max_age 4, cadence 1, n 1, lambda_max 10) |

### Commands and results (run from the repository root)
| Command | Result |
|---|---|
| make dual-purity | PASS: rx_dual.o rx_dual_bind.o rx_dual_update.o rx_dual_replay.o reference no authority, promotion or process symbol (nm -u of rx_dual_replay.o: memcmp memcpy memset strlen, rx_dual_*, sha256_*, stack protector only; no stdio: the CSV importer takes bytes the test read) |
| make test-dual-replay | plain: 456 checks, 0 failures, PASS; ASan/UBSan: 456 checks, 0 failures, PASS |
| make test-dual | test-dual-types 14107/1667, test-dual-bind 85/85, test-dual-update 355348/35837, test-dual-replay 456/456 checks, 0 failures |
| make all / make test | unchanged: DUAL is not part of them; nothing under src/runtime/ changed (`git diff --quiet HEAD -- src/runtime` clean) |

### Trace digests and replay digests (reference controller unless marked HOST)
| Chain | n | trace digest (omega.dual.trace.v1) | replay digest (omega.dual.replay.v1) | final lambda |
|---|---|---|---|---|
| 1 constant low | 200 | 0e0c2f9f833a47158fa885af34d2fc5cbea250124f891e56af31c7cf0d7a4ab1 | e3c9fbfc4e73e1054d8739f47c7ad56a5b97ad6efe57b2ef12ece689e67e32d2 | 0 |
| 2 binding (golden) | 200 | f9c3944ab952dd5e2beefebbb2499f98ec7be28157b571a47e406ca40e4d93fb | 46345146d1213fdd2888fd0ace3f7ab31287565fd42adf950ad58edeb11bf5ae | 0.599978968 |
| 3 step | 200 | 966e602e16db9f3a61187f0135c45657dd7b3f22b22d910e3eda9ede19eed175 | 3539d9d349ee9f789a833d95a011351b99807d3f85896996e35e6defa255f489 | 1.59052715 |
| 4 square (golden) | 200 | 0a463faef33ab2a59da6c854eabfa2ec9e189f3e5e6b00f07c773e490e52edd5 | 64a30ce77dc7decfedae15c6c8e8e66fa75693df049916deab7ca2abbfaaa888 | 0 |
| 4 square HOST | 200 | 0a463faef33ab2a59da6c854eabfa2ec9e189f3e5e6b00f07c773e490e52edd5 | 34ac4fde2f4f92a676c619201e0cea590a06522bc1a3d5e40040c4d4bd6b46bb | 0 |
| 5 competing memory | 200 | c988bffd193292402cd853f8453454f128bbc7c5a0b6c89e50fad8df5b0a4c6a | 46afcfa907a0393ccbff84ec62d844fda2af0aa7739f7fbbccdf70b0cde60064 | 1.79993691 |
| 6 competing latency | 200 | 92298884577ea85d0a133da92a2ae73b49f78e189df585af03bd8dbf6016abe4 | 65379174831235554c91a3194dd32a43a3285f5046b4f2096e29a1c22f5a287a | 0 |
| 7A capacity (budget 5000) | 100 | e3f2c5e796ede019b78fcc1b3ff584a2a8028236332d361b356fe0883c3da337 | 714c2438d01a68bf180af03906c50dbac6168757ea25ca41de9c4de85e580325 | 0 |
| 7B capacity (budget 3000, new contract) | 100 | 7f7d9ccdddf47321a545c49340eb48d32d0d595b695e82cdca0995be2a86a28c | 5a041ac1ff7390cd63428b87bcfd0380dec105ecbd48563f94965bf4a81aed62 | 2.58460662 |
| 8 regime change | 200 | 0b17c31469fd478c24ec6acf2318caadab124045431c986af6e985fa50c1245b | 8a4eabfde2d1d51e2457fba6c5f9174c06b320c5c6e22f85aebd0ba2317d0be7 | 0.599836345 |
| 9 bursty (26 bursts) | 200 | 30162f09ee84b7b1bd56a88f9f72426e3c9a476d90201f921c92e63cee1271bd | 723b8d1eb96e1647414cd376f79d87c4f7c035048388a9e41d2d46222caf3b3d | 0.120999564 |
| 10 fan-out r = 0 (of 32; all 32 digests in the test output) | 200 | df3ebf9e65ea2c9bb6e7df3e372edaf43f876f6c2d85916e38d84c21f7ee0d68 | 4746925bb0c1c76b8b2b87ae155644b4b13c009622df3f06f71837bb7ed99e07 | 0.538262214 |
| 11 KV saturation | 200 | f0308e3fa8e549f4a18a3fb57535ade611587f5720582ac057cd60698ae5794a | e49c22a48ec0d7e6dff654017a39e138db6aeff8dd426d2ad3730973ed39510d | 2.84391271 |
| 12 recovery | 200 | dca6d3cb19658bb4b2f9b33386c3c77a70d765e7bc81c994f4488565d1204826 | 356cd1f1f68d3ac3763fec9d9c6efa27e1e90b3ff48a9cab7dfb357d1b7bc5f8 | 0.00941676249 |
| 13 R15 020536Z machine CPU0 | 1976 | 8f5f9b1f92064ba93ce22685a55d6403645a53e9a0397f04db6dc868f5bd9345 | 175bd2cc5950145e7f0b371a8a0994d2fc22bc1c489eca470e942ad15af64bea | 0 |
| 13 R15 020536Z machine CPU7 | 1976 | fe2ca66182ae4f8891165d5b44a635e467efb5d30bd4e77d418dc76cd7869d94 | 908ed8bdef115365fb74392dfe160c7668cf80e31ddc271838fbd16f43333e3a | 0 |
| 13 R15 020536Z preflight CPU7 | 20 | 3a1cf368c4b0f376ecfea21864ce7f39a3cce12ffa96c457212d9e48a4af13c8 | 0e37c660a3e004755517657f85012b11fe5ec8306bb9acd8f6fd1a913cc30373 | 3.70425459 |
| 13 R15 025735Z machine CPU0 | 2033 | 83e92c15ae0cd3a2cc7b747559fbcd3786619180697d1721c910f435b098777b | 2f5387d79988db0bfdbf8272ba28bba19dcad766c79252d7af817e312a168725 | 0 |
| 13 R15 025735Z machine CPU7 | 2033 | 12ee87a420de376a852dbebda58a4ac8a4639e7523545dcc5ddda8c537763b7e | fbbc9e42ee76495260f91495efbcd27620de7a8d1264aeb5bbe744b717ab1e96 | 0 |
| 13 R15 025735Z preflight CPU7 | 20 | 5a262e15b8d85ade33f4d028a052292ed8e26946b837e196cf5e1b30a605c586 | 2b3f0da6e2c063a6594adba2b24b2474e9d3fe2fe04628f82889e4f402c6fa1d | 3.70285023 |

Recorded raw files (SHA-256 of the bytes as read; `SUM` = equals the entry in that run's own SHA256SUMS):
| File | bytes | sha256 | SUM |
|---|---|---|---|
| evidence/R15/raw/20260929T020536Z-ad8e1f2ea4e4-silicon/machine-perf.csv | 5254065 | 22663a037a7f557634c1786e0427a669925c99d0e6c436309139a362e94a43af | PASS |
| evidence/R15/raw/20260929T020536Z-ad8e1f2ea4e4-silicon/preflight-perf.csv | 1469 | 2078a6412767ca5bbae118fe22486d0846e5e25b83cbd73231468e7736e70943 | not listed in SHA256SUMS; recorded here only |
| evidence/R15/raw/20260929T025735Z-3e9e53be3358-silicon/machine-perf.csv | 5400978 | 36e0cd936e783a054c82a47444219f1266ddb47a1ae33a14fadaaaac23adb3b2 | PASS |
| evidence/R15/raw/20260929T025735Z-3e9e53be3358-silicon/preflight-perf.csv | 1469 | ff14887fd894d5902f948942eecfb1f936b598a5f9986f5beb2c544244b8cf51 | not listed in SHA256SUMS; recorded here only |

Recorded-trace facts: machine CPU0 cycles exceed the 1e9 budget on 53 / 60 of 1976 / 2033 ticks (bursts), CPU7 on 669 / 694
ticks; the preflight CPU7 series is above budget on all 20 ticks. These are counter streams, not calibrated estimates.

### Measures (reference controller; thresholds frozen above)
| Chain | M1 | M2 | M3a | M3b | M4 | M5 | M6 | M7 | M8 (R lambda / R est / bound) | other |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 constant low | PASS | PASS (0) | | | | | | PASS | PASS 0/0/1 | |
| 2 binding | PASS | PASS (0) | | | PASS | | | PASS | PASS 0/0/1 | A PASS final 0.599978968 vs 0.6; D2 PASS; D3 PASS; D4 PASS; H1 PASS |
| 3 step | PASS | PASS (0) | | | PASS (from 100) | | | PASS | PASS 0/0/1 | lambda[99] = 0, lambda[100] > 0 |
| 4 square | PASS | | PASS 7 <= 8 | PASS 1.15617668 <= 2.5 | | | | PASS | PASS 7/6/7 | D2 PASS |
| 5 memory | PASS | PASS (0) | | | | | | PASS | PASS 0/0/1 | M9 PASS: memory 1.79993691 > 0 |
| 6 latency | PASS | PASS (0) | | | | | | PASS | PASS 0/0/1 | M9 PASS: latency 0 |
| 7A capacity | PASS | | | | | | | PASS | PASS 0/0/1 | lambda 0 throughout |
| 7B capacity | PASS | PASS (0) | | | PASS (from 0) | | | PASS | PASS 0/0/1 | generation 101..200, tick 0..99 |
| 8 regime | PASS | PASS (0) | | | | | PASS | PASS | PASS 0/0/1 | ticks 80..119 FROZEN, lambda/estimate/generation held; UNCALIBRATED resumes at 120 |
| 9 bursty | PASS | | | | | | | PASS | PASS 32/38/39 | |
| 10 fan-out (32 chains) | PASS x32 | PASS (0) x32 | | | | | | PASS x32 | PASS 0/0/1 x32 | M10 PASS: finals in [0.501392531, 0.697240739], spread 0.195848208 <= 0.25 |
| 11 KV | PASS | | | | PASS (from 110) | | | PASS | PASS 0/0/1 | M11 PASS: lambda 0 while estimate <= 5200; first crossing tick 110 |
| 12 recovery | PASS | PASS (0) | | | | PASS K=90: lambda[189] = 0.0157277126 <= 0.0159052715 | | PASS | PASS 1/0/1 | peak 1.59052715 |
| 13 020536Z machine CPU0 | PASS | | | | | | | PASS | PASS 81/1238/1239 | D1 PASS |
| 13 020536Z machine CPU7 | PASS | | | | | | | PASS | PASS 245/1175/1176 | D1 PASS |
| 13 020536Z preflight CPU7 | PASS | | | | | | | PASS | PASS 0/9/10 | D1 PASS |
| 13 025735Z machine CPU0 | PASS | | | | | | | PASS | PASS 91/1269/1270 | D1 PASS |
| 13 025735Z machine CPU7 | PASS | | | | | | | PASS | PASS 197/1228/1229 | D1 PASS |
| 13 025735Z preflight CPU7 | PASS | | | | | | | PASS | PASS 0/13/14 | D1 PASS |

D1 (two replays, identical replay digest) PASS on every chain above. D2: golden hex constants for scenarios 2 and 4 are
committed in tests/dual/test_dual_replay.c and matched in both the plain and the ASan process. Trace codec: exact round
trip, kind byte 2 / version 2 / truncation / trailing byte refused, NaN / negative uncertainty / flag > 1 / kind 0 / n 0 /
n > 2048 refused, -0.0 digests as +0.0. Importer: "<not counted>" refused (RX_DUAL_ERR_ENCODING), no matching row
refused, non-matching event rows skipped, generation = row index + 1.

### Negative control
| Controller | Scenario | M1 | M3a | M3b | M8 | Verdict for this configuration |
|---|---|---|---|---|---|---|
| hostile eta 5.0, rho 0.0 | 4 square | PASS (max 10 = clip) | PASS 7 <= 8 | FAIL peak-to-peak 10 > 2.5 | PASS 7/6/7 | FAIL, exactly as declared |
| reference | 4 square | PASS | PASS 7 | PASS 1.156 | PASS 7/6/7 | PASS |

Every declared expectation for the negative control held: the hostile controller is bounded (clip works), reverses no more
often than the reference (the pre-stated theorem: reversal counts cannot exceed R(estimate) + 1 for any valid controller),
and fails the amplitude bound by slamming between 0 and lambda_max.

### Verdict
`DUAL_PRICE_REFERENCE = PASS` on host: every pre-registered measure passed for the reference controller on every scenario and
on every recorded trace, and the negative control failed M3b as declared. No threshold was changed after the
pre-registration commit.

Scope and limits (stated, not hidden):
- ALL inputs are UNCALIBRATED. Every replayed state is UNCALIBRATED (or FROZEN on regime ticks); none is FRESH except in the
  D4 format exercise, which uses a stand-in digest and makes no calibration claim. No price from this gate, and no price from
  this module, may influence any production decision until EST-3 passes for the signal and the later DUAL gates pass.
- The recorded traces are perf cycle counters (the R15 CSVs contain no thermal, power or clock-frequency column); the budget
  1e9 cycles per second is a pre-registered stand-in, not a declared resource budget of any contract.
- The regime-change scenario exercises the FROZEN path with an injected flag; it does not establish that EST-9 innovation is a
  usable regime detector (ADR 0031 s5.3 leaves that open).
- Capacity reduction is modelled as a new constraint chain under a new budget_contract because a budget is declared by a
  contract; the price history of the old chain is not carried into the new one.
- Host-only. No GPU, no runtime linkage, no src/runtime change.
