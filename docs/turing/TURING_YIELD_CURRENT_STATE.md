# TURING YIELD (TY-0): current state audit

Copy of the TY-0 read-only audit (worker report, 2026-09-29, omega main 87938dc) committed with only this note added, as the pre-registration record for TURING_YIELD_PROFILE_V0.md. Since the audit, omega main moved to a927bd6: OSC-0/OSC-0B (#76) is now Accepted, so section 1 and section 9 item 6 read "accepted" where they say "proposed" or "draft".

Read-only. No repo edits, no builds, no benchmarks, no GPU. Audited 2026-09-29.
Paths are `origin/main` of the named repo unless stated. `UNVERIFIED` = not checked against source.

## 0. Heads

| repo | HEAD | note |
|---|---|---|
| omega | `87938dc` (merge #80, 2026-09-29 14:16 -0500) | fetched in `~/workspace/omega-polyglot` |
| aien-architecture | `1438799` (#59 "correct R16 status to IN PROGRESS", 19:28Z) | shallow clone was `e2bb8a8`; #59 read via `gh api` |
| physics | `f63a6ef` | shallow clone |
| aienos | `603c91d` | shallow clone |
| aien-sovereign-core | `63fe7a7` | shallow clone |
| aien-protocols | `7ac6fac` | shallow clone |
| benchmarks | `959dedd` | shallow clone |

PRs: omega #68 MERGED 16:49Z as `6fdc4c3` (title "R16 spec ... (pre-registration)"); #76 OSC-0/0B OPEN draft
(`5c4b707`, "Not for merge until Drake accepts the freeze"); #79 TURING Wave 1 MERGED 19:13Z; #80 POLYGLOT-0 MERGED
19:16Z; #60 empirical optimizer MERGED 12:56Z; #78 MA-2 MERGED. Only other open omega PR: #32 (numeric FP32 SIMT).
physics #16 FORGE V2 MERGED, #13 FORGE-0 MERGED. arch #58 ADR 0019 ACCEPTED, #59 MERGED. aienos ARGUS-1 lanes #165-#172
MERGED. No open PR anywhere mentions TURING yield, Evolution Arena runtime, or T/J.

## 1. Q1: is src/runtime frozen? Is tests/runtime?

- **Update 2026-09-30: no longer frozen.** R16 CLOSED (omega#112, `3dd5eaa`); freeze lifted (aien-architecture #70,
  `e89ba94`). The bullets below are the audit as of 2026-09-29. The TY-0..TY-7 non-touch rule for `rx_costmodel` and
  `rx_argus` (section 9 item 1) stays as a scope rule.
- **R16 is IN PROGRESS: yes, frozen.** arch `CURRENT_EXECUTION_PLAN.md:45` "R16 ... IN PROGRESS"; `:540` "No edits to
  `omega/src/runtime/` (or other R16-mapped files) until R16 (`aien-dev/omega#68`) closes." arch `doctrine/ROADMAP.md:282`
  (after #59) "IN PROGRESS ... only R16-G1/G2 loop-inventory gate PASS. Not yet evidenced: G3..G8 receipt".
  The earlier "PASS" (arch `8ec9b1d`, #57) is superseded by #59 and is no longer a conflict.
- Evidence agrees: omega `evidence/R16/` holds only `inventory.json`; no `AIEN_RX_R16_ORCHESTRATOR_RETIRED_V1`
  receipt (required by `spec/r16-orchestrator-retirement.md:199`); `tests/runtime/rx_r16_negative.c` (G4, `:195`) does not exist.
- **tests/runtime: editing is frozen, linking is allowed.** `r15_measure.c` is inventory row OM-048
  (`evidence/R16/inventory.json:59`, class D, authoritative=false), so it is an "R16-mapped file" under `:540`.
  Class D covers "Test, benchmark and example code ... by path" (`spec/r16-orchestrator-retirement-map.md:110`).
  Linking it unchanged into a new harness edits nothing.
- A new timed loop in a new `tests/turing/` or `tools/` file must stay clean of the loop-inventory patterns
  (`CURRENT_EXECUTION_PLAN.md:542`). If the R16 scanner flags it, it would be classified D by path. UNVERIFIED
  whether `tools/r16_loop_inventory.c` treats an unmapped new file as a gate failure. Check before TY-5 code lands.
- Other TURING-relevant freezes: `docs/turing/TURING_AGENT_WAVE_1.md:16` also forbids edits to `src/algebra/` and
  `src/polyglot/` for Turing work. OSC-0B identity freeze (#76) is proposed, not accepted. Turing Field digests are
  deliberately not Omega semantic ids (`src/turing/field.h:11-14`).

## 2. Component classification

| component | class | evidence |
|---|---|---|
| src/turing Field V0/V1 records | IMPLEMENTED | `src/turing/field.h:33-37` domains `turing.contract.v0.provisional`, `turing.spec.v0`, `turing.evidence.v0`, `turing.decision.v1`, `turing.citeset.v0`; digest = SHA-256(domain‖0x00‖OMG0 bytes) (`field.h:9-14`, `field.c:125`); goldens `tests/turing/test_turing.c:34-36` (contract, R2c_crumb spec, R1_plain spec); `make test-turing` (`Makefile:971-1002`) |
| turing_evidence energy | MISSING | `field.h:78-100`: timing (median/q25/q75/min ps), noise, bytes, build, thermal. No energy field |
| turing decision selector | OBSOLETE (V0 tie rule) / PARTIAL | `src/turing/field_select_v0_retired.c`; K.7 "Field keeps no ranking rule of its own" (`docs/turing/TURING_W0_PROPOSAL.md:43`) |
| rx_costmodel energy | PARTIAL (modelled) | `src/runtime/rx_costmodel.h:70` `energy_budget_pj`, `:86` predicted `energy_pj`, `:138` `power_mw[core][arm]`; `rx_costmodel.c:200` energy = exp2(mean log2 ps) × mW (MODELLED, not measured per call). In frozen runtime |
| rx_empirical_optimizer | IMPLEMENTED (test side) | `tests/runtime/rx_empirical_optimizer.c:560-575` measures per-arm power above idle from `aien_spbm` cpu_p/cpu_e over 0.5 s windows and feeds `power_mw`; `spec/empirical-optimizer.md:65,272-283` meter includes other tenants, updates about every 0.1 s, per-arm power noisy; receipts `evidence/EMPIRICAL/*.json` (2) |
| r15_measure (PMU + SPBM + NVML + 10 Hz) | IMPLEMENTED (test side, frozen for edits) | see §4 |
| spec/r15-performance-proof.md | IMPLEMENTED (spec + C2 validation) | energy method `:296-305`; C2 source/width/validation/uncertainty `:585-621` |
| spec/empirical-optimizer.md | IMPLEMENTED | `:59,65,187,272-283` |
| rx_argus | IMPLEMENTED (observer, compile-time RX_ARGUS 0/1/2) | `src/runtime/rx_argus.h:1-40`; no energy/entropy fields (grep empty) |
| rx_jspace | IMPLEMENTED | `src/runtime/rx_jspace.h:2-7` branch state, SHA-256 chain; no energy |
| rx_cortex | IMPLEMENTED | `src/runtime/rx_cortex.h:2-7` append-only typed memory; no energy |
| rx_world | PARTIAL (energy as budget units) | `src/runtime/rx_world.h:233,248,367,516` `energy_cost`, `energy_budget`, `energy_held`, `used_energy` (accounting units, not joules; UNVERIFIED unit) |
| rx_capq | PARTIAL (declared/expected) | `src/runtime/rx_capq.h:98` `energy_budget` µJ, `:116` `expected_energy` µJ, `:156` `energy_uj` |
| src/algebra (MA-2) | IMPLEMENTED | `src/algebra/realize_common.h:54-80` `oma_rz_impl{pack,run}`, 11 realizations; bench `tests/algebra/bench_mixed_algebra.c` has its own `aien_spbm` cpu_p reader (`:230-254`) |
| MA-2 energy | EXPERIMENTAL (indicative) | `spec/mixed-algebra-ma2.md:287-298` "energy is not used by the selector"; receipts `evidence/MIXED_ALGEBRA/ma3_bench_run{1,2}.json` carry `energy`, `cluster_idle_w` |
| "MA-3" | OBSOLETE label | `Makefile:921` "ma3_bench_run{1,2}.json are the historical runs made under the earlier MA-3 label". Not a separate workload |
| src/polyglot receipts energy | MISSING | `docs/polyglot/OMEGA_LANGUAGE_EVIDENCE_LEDGER.md:161` "no energy or power was measured"; `tests/polyglot/bench_polyglot.c` has no meter (grep) |
| FORGE V2 evidence (physics) | IMPLEMENTED (serializer + KAT, no hardware) | `physics/forge/v2/forge_substrate_v2.h:180-183` EVCAP mask {DURATION, ENERGY, TEMPERATURE, MEASURED_ERROR, REPEATS, HW_STATUS, FAULT}; `:192-194` energy source {NOT_MEASURED, MEASURED, ESTIMATED}; `:340-341` `energy_nj`, `energy_source` |
| Omega Discovery / M11 delta cost | PARTIAL | `src/omega_discovery.c:206-212` compression_score = occ×(L−1)−(L+1) in instruction counts, not bits; `spec/discovery.md:22,57` ΔCost>0; `tests/run_m11_gates.sh:83` "+7 instructions saved" is a literal string in the receipt heredoc, not computed there |
| DISCOVERY DESCRIPTION_LENGTH / PREDICTIVE_SCORE | PLANNED (doctrine only) | arch `doctrine/DISCOVERY.md:232-233`; no code in omega (grep 0 files) |
| Shannon entropy helper | IMPLEMENTED in Rust only (nats) | `aien-protocols/crates/aien-probe/src/math.rs:65-70` entropy in nats; `aien-sovereign-core/crates/aien-inference-abi/src/moe_plan.rs:64,201` `routing_entropy_bits` |
| Entropy helper in C | MISSING | omega `*.c/*.h`: no `entropy`/`shannon` (grep); only `log2` in selectors (`oma_select.c`, `rx_costmodel.c`, `field_select.c`) |
| Lossless entropy coder (arith/range/ANS/Huffman) in C | MISSING | none in omega, physics, aienos, aien-architecture, benchmarks, aien-protocols, sovereign-core C files (grep; vendor/third_party excluded) |
| SPBM resolution/accuracy docs | IMPLEMENTED | `research/m15/spbm/README.md:16-21,30,36,47` (1 mJ step is not 1 mJ accuracy; no external meter); `spec/r15-performance-proof.md:597-621` |
| benchmarks energy | PARTIAL, different boundary | `benchmarks/src/models.rs:121-122` `power_watts`, `joules_per_token`; `src/bin/bench_canonical_suite.rs:128-129` instantaneous `nvidia-smi power.draw`, GPU only, not an accumulator; Rust |
| C BLAKE3 | MISSING | omega grep 0 files; crumbs trace chain uses Rust `blake3 = "1.8"` (`crates/crumbs/Cargo.toml:13`) |

## 3. Q2: persisted Omega traces usable as TY-2 data

| candidate | where | size / count | format | chronology | verdict |
|---|---|---|---|---|---|
| **Crumbline search traces (CTR1)** | `~/aien-data/crumbline/{exp-20260927-a, exp-20260927-rep10, final-20260927, gates-20260927}/.../{control,learning}/trace.ctr` (outside git) | 54 files, 21,325,064,124 B total, exactly 86,336,292 records (bytes ÷ 247, no remainder: headerless record stream); rep10 alone: 20 files, 9.52 GB, seed-1/learning 470 MB, seed-1/control 537 MB | fixed 247-byte `CTR1` v1 records, BLAKE3 hash-chained (`aien-sovereign-core/crates/crumbs/src/trace.rs:13-14,26,51-80,406-436`); plus `ledger.jsonl` (1 line per crumb, `seq`), `evaluations.jsonl`, `samples.cts` (268 B `CTS1`), `promotion.json` | no wall-clock timestamps; `event_index` restarts per crumb (`trace.rs:198-200`); records appended per crumb in `seq` order (`crates/crumbs/src/session.rs:196-203`) | **RECOMMENDED** |
| R15 raw telemetry | `evidence/R15/raw/<run>/*.jsonl` (2 silicon runs × 139 files) | one run: 15,079 JSONL lines, 10,522 `telemetry`, 222 `window`, 240 `cwindow`, 48 `episode`/`adapt`/`result` | JSONL with `t_ns` | timestamped | measurement records, not decisions. Useful for TY-4 only |
| R15 machine perf | `evidence/R15/raw/<run>/machine-perf.csv` | 5.4 MB, 75,223 lines | `perf stat -I` CSV | timestamped | not Omega decisions |
| ARGUS raw | `evidence/ARGUS/speed2/raw/*.jsonl` | about 1,800 lines max per file | per-run summaries | per run | too small, not decisions |
| EMPIRICAL receipts | `evidence/EMPIRICAL/*.json` | 2 × about 9.7 KB | summary JSON | none | too small |
| TURING Field records | produced by `tools/turing_field.c` from `evidence/MIXED_ALGEBRA` | tens of records (UNVERIFIED exact count) | OMG0 records | none | too small |
| Crumbline conformance vectors | `tests/crumbline/vectors/v*.crb` | 123 files, 37 KB | hand-built test vectors | n/a | not traces |
| omega_synthesis stats | `src/omega_synthesis.c:127,169,183` counters | in memory only | none | none | not persisted |

What the CTR1 records contain. Each record is one Omega search event from `src/crumbline/cl_search.c:272-315`
(`ClEvent{kind, prune, verify, fit, op_index, parent, child, exec_cost, oracle_index}`, `src/crumbline/cl_search.h:45-62`).
The events come from the C learner `tools/crumbline_learner.c` (FT_EVENTS 0x84) and are expanded by the Rust sealed
side with `result_class` (Pruned..RejectedRobust, `trace.rs:31-45`), state and program digests, mismatch and Hamming
before/after, depth, costs, and residuals. So the data is Omega's real candidate transitions, prune reasons,
verification outcomes and fits.

Leak check: each session has 188 distinct `crumb_digest` values (`ledger.jsonl`, seq 0..187). Across all ten rep10 control seeds: **0** repeated digests.
Control and learning of the **same seed share all 188 crumbs** (checked seed 1; paired design). Control runs with `library_enabled=false` (`crates/crumbs/src/experiment.rs:529-532`).

**Recommendation for TY-2.**
- Dataset: `exp-20260927-rep10`, **control** condition. The learning condition changes as library admissions are
  added (`session.rs:205-215`), so it is non-stationary. If learning is used, declare that shift.
- Split, option A (default): whole seeds. Fit on seeds 1-7 control, SEALED held-out = seeds 8-10 control. Leak-free by
  crumb identity. Never pair control seed s with learning seed s across the split.
- Split, option B (chronological): within one session, fit on crumbs `seq < k` and hold out `seq ≥ k`. Crumbs are
  segmented by `event_index == 0`.
- Symbol stream: learner-side fields (`kind, prune, verify, fit, op_index, result_class`). Sealed-derived digests are
  context at most.
- Pin the corpus (outside git): record file list, sizes and SHA-256 of the chosen subset. Limits: a C reader can parse the fixed layout, but it cannot verify the BLAKE3 chain (no C BLAKE3). The data can only
  be regenerated by the Rust `crumbs` crate.

## 4. Q3: r15_measure reuse

- **Links unchanged.**
  - `tests/runtime/r15_measure.h` includes only `pthread.h`, `stdint.h`, `stdio.h`.
  - `r15_measure.c:5-18` includes only libc, `linux/perf_event.h`, `dlfcn.h` and its own header. No `rx_*` dependency.
  - Current build use: `Makefile:515-516`.
  - A new harness compiles it with `-Itests/runtime ... -lpthread -ldl` (UNVERIFIED by build, per brief). 393 lines.
- **PMU.**
  - Six raw events: cycles, inst_retired, bus_access, ll_cache_miss_rd, l2d_cache_refill, mem_access
    (`r15_measure.c:20-23`, codes 0x11/08/19/37/17/13).
  - Opened per task (pid 0, cpu −1), inherit, on both CPU PMUs, raw sums, never scaled, with a multiplex flag
    (`r15_measure.h:14-40`).
  - It counts the calling process tree. For A/B attribution, A and B must be separate processes, each opening its
    own PMU.
  - `perf_event_paranoid` = 1 on this Spark.
- **Energy.**
  - Finds hwmon named `aien_spbm` and checks the labels (`r15_measure.c:161-193`).
  - Present on this Spark: `/sys/class/hwmon/hwmon3` name `aien_spbm`.
  - Energy channels: pkg, cpu_e, cpu_p, gpc_unverified, gpm. Power channels: sys_total, soc_pkg, cpu_e, cpu_p, gpu
    (`:141-142`).
- **NVML.**
  - Always dlopens `libnvidia-ml.so.1` and calls `nvmlInit_v2` and device 0 (`:195-207`). The library is present:
    `/lib/aarch64-linux-gnu/libnvidia-ml.so.1`.
  - `r15_energy_open` dlopens it unconditionally. Every later NVML use is gated on `nvml_dev` (`r15_measure.c:224,291-292`), so a harness can skip
    `r15_energy_open`: zero-init `R15Energy`, fill `hwmon` itself (repeats ~30 lines of label checks), leave NVML NULL. No edit needed.
  - If `r15_energy_open` is used as is, NVML loads (conflicts with the no-NVIDIA stance, memory `sovereignty-no-outside-deps`)
    and nvmlInit touches the GPU driver. `LD_LIBRARY_PATH` cannot hide it (it is in the system library cache).
- **Sampling:** 10 Hz sampler thread (`:239-251`, next += 100 ms), ring buffer, and `r15_energy_samples(t0,t1)` window extraction. Sampler starts before the PMU opens, so its reads are not counted (`r15_measure.h:66-68`).
- **Overflow.**
  - `r15_measure.c` only records `overflow_raw` per channel (`:217-218,362-363`).
  - Refusal (overflow set, counter decreases, low headroom) lives in `tools/r15_reduce.c:442` and
    `research/m15/spbm/sample.c`. The spec says "not unwrapped" (`r15-performance-proof.md:600-603`).
  - A TY-4 harness must reuse or port that refusal logic. It is not inside r15_measure.
- **Resolution and accuracy** (`spec/r15-performance-proof.md:608-621`; `research/m15/spbm/README.md:30,36,47`): 1 mJ step (32-bit, wraps at about 4.29 MJ, about 54 h at 22 W).
  - ±1 sample period (100 ms) of power at each window edge.
  - Accumulator vs SoC power disagreement up to 0.5 W (about 2-3%).
  - No external calibration, so absolute accuracy is not claimed. GPC always reads 0 and is unused.
  - `cpu_p` is the whole 10-core X925 cluster (`spec/mixed-algebra-ma2.md:289`).
- **Duplicate readers.** Three independent `aien_spbm` readers already exist: r15_measure, `rx_empirical_optimizer.c`
  (`meter_uj`), and `bench_mixed_algebra.c:230-254`. The brief's "reuse" should pick r15_measure. Adding a fourth would
  violate "no new telemetry plane".

## 5. Q4: TY-5 workload (idle / A / B / A+B)

- **Best unit: an MA-2 realization `oma_rz_get(i)->run` on one fixed DRAM-bound shape.** Example: `R1_plain` or
  `R2c_crumb`, m=4096 (or 16384) × n=16384, weights of hundreds of MB. Each process is pinned to one X925 core (cpus
  5-9, 15-19 are X925; `spec/mixed-algebra-ma2.md:124-126`), in a fixed-duration loop.
  - It is deterministic, oracle-checked (`realize_common.h:73`), C only, and needs no runtime or GPU.
  - Measured single-core read bandwidth falls to about 34.6 GB/s at 256 MiB (`ma3_bench_run1.json` `bandwidth`).
- **Rejected:** POLYGLOT (only S3 4096×4096 is DRAM-bound, `spec/polyglot-0.md:61-64`; no meter; manifest grid) and
  R15 (resident runtime, GPU seat claims, many threads: attribution confounded).
- Both existing benches run full grids, not steady-state loops. TY-5 needs a new small driver. It can only live under
  `tests/` or `tools/`: `src/algebra/` is off-limits (`TURING_AGENT_WAVE_1.md:16`).
- **"Known contention" is UNVERIFIED.**
  - Nothing read shows that two single-core GEMVs saturate DRAM.
  - What is known: C2 found CPU+GPU load "not additive" (`r15-performance-proof.md:611-613`).
  - The contention knob is the number of cores per workload (1 vs 2 vs 5) and same cluster vs cross cluster (X925 vs A725).
  - Declare the knob in the profile and sweep it. Do not assume contention at one core each.
- **Meter boundaries.**
  - A and B on the same cluster are only separable at `cpu_p` as shared plus interaction.
  - DRAM energy appears only in `pkg`.
  - Exclusive attribution needs A and B on different clusters (cpu_p vs cpu_e), which changes the workload. Confidence:
    COUNTERFACTUAL for exclusive shares, DIRECT for window totals.
- **Error budget for I = E_AB − E_A − E_B + E_0** (derived, not measured).
  - Per window edge: up to P × 0.1 s, which is about 2 J at 20 W pkg.
  - Four windows × two edges, root-sum-square: about 2√8, roughly 5.7 J.
  - To resolve I to 1% of E_AB at about 30 W, windows need to be at least about 19 s. Plan on 30 s or more, with
    interleaved ABBA rounds.
  - The 0.5 W scale bias is shared by the same sensor and mostly cancels in I. State it anyway.

## 6. Q5: what work both yields T and consumes energy

POLYGLOT and MA-2 realizations compute identical outputs (bit-exact vs oracle), so there is no T to earn from them.
The honest pairing:

- **Work W = scoring the SEALED held-out split of the TY-2 corpus under the model M and under the baseline B**, in C.
  - L(D|M) = Σ −log2 P_M(x_t|ctx) over held-out symbols. Same for B.
  - L(M): for a model fit once then frozen, bits of its declared quantized parameters; for an online (prequential) model, bits of its declared spec only, with L(D|M) the prequential sum.
  - T = [L(B)+L(D|B)] − [L(M)+L(D|M)].
- **Energy:** each scoring pass runs in its own metered window (r15_measure pkg + cpu_p, idle-subtracted) on the same
  pinned core.
  - T/J uses a declared denominator: either E_M (gross cost of producing the M code length) or E_M − E_B (marginal cost
    of the better model).
  - The profile names which. **Fitting energy** (training M on the fit split) is reported as a separate record, never
    folded silently.
- The evidence then refers to the same work: the bits come from exactly the symbols scored inside the metered window.
- **Ideal code length only.** No C lossless coder exists (§2). T is Shannon code length, not a decodable bitstream.
  - Optional check later: a C arithmetic coder whose output size is within a few bytes of Σ −log2 P.
  - That is new in-house code. It is not an outside dependency.
- Numeric identity: T in bits is a float sum. Goldens must reproduce under any build flags (`test_turing.c:33`). So
  store T in digested records as fixed-point integers (for example millibits as u64, energy in µJ as u64, as in
  `energy_uj`). Fix the summation order.

## 7. Adding a companion record without changing TURING digests

- Existing digests cover domain‖0x00‖OMG0 bytes of the record fields (`field.c:114-126`). Adding a field to
  `turing_evidence` or `turing_decision` changes those bytes and breaks goldens (`test_turing.c:34-36`) and every cited
  digest.
- Precedent: the decision domain was bumped to `turing.decision.v1` with the old selector retired in a separate file
  (`field.h:36`, `field_select_v0_retired.c`). The citeset domain is separate (`field.c:288-301`).
- Companion pattern:
  - A new struct plus a new domain, for example `turing.yield.v0` (T record) and `turing.energy.v0` (window record).
  - It cites existing evidence, spec or decision digests by value. Existing records are never re-encoded.
  - Add new goldens only for the new domains.
  - The measurement profile digest (for example `turing.yprofile.v0`) is cited by both.

## 8. Q6: naming

- **"Turing" / "TURING"** is already the Field program: `src/turing/*`, `docs/turing/*`, `tools/turing_field.c`,
  `Makefile:971-1002`, domains `turing.*` (`field.h:33-37`).
  - A unit "Turing (T)" inside records named `turing_*` is ambiguous. Prefer domain `turing.yield.*` and field names
    `t_millibits`. Never reuse `turing_evidence`.
  - "Turing" is also an NVIDIA GPU generation (physics `third_party/nvidia-open-*` headers).
- **"yield"**: 23 omega files, all plain-English prose ("yields identical SEMANTIC_ID", `README.md:95-96`). No identifier conflict. "surprise": 0 hits.
- **"profile"**: collides with `spec/aarch64-realization.md:96` (Target Profile ID), `spec/machine.md:101` (machine
  profile) and `spec/cognitive-routing.md:14`. Use "measurement profile" or `yprofile` in full, never bare "profile".
- **"sealed"**: already meaningful in crumbs (`crates/crumbs/src/sealed.rs`, sealed verifier/held-outs in `ledger.jsonl`
  `sealed_digest`). The brief's "SEALED held-out split" is a different split (TY-2 fit/test), so name it distinctly.
- **DESCRIPTION_LENGTH / PREDICTIVE_SCORE** (arch `doctrine/DISCOVERY.md:232-233`) are the natural doctrine homes for
  L(M) and L(D|M). Cite them, don't redefine them.

## 9. Q7: constraints and brief conflicts

1. src/runtime is no longer frozen: R16 CLOSED (omega#112, `3dd5eaa`), freeze lifted (aien-architecture #70). Scope rule
   (unchanged): nothing in TY-0..TY-7 may touch `rx_costmodel`, `rx_argus` or the rest of the runtime. Joining T/J into the cost model or selector is out of scope anyway ("no new cost model, no new selector").
2. tests/runtime: the R16 edit freeze is lifted; by TY-0..TY-7 scope, r15_measure is still linked only as is. NVML avoidable only by bypassing `r15_energy_open` (§4).
3. ARGUS observes authority decisions (`rx_argus.h:1-8`). It carries no energy. "ARGUS observes" cannot supply joules
   today; energy must come from r15_measure windows.
4. TY-2 data is produced by Rust (`crumbs` crate). Reading is C-only and possible. Regenerating is not C-only, and chain
   verification needs a new in-house C BLAKE3.
5. No Python: omega main still has `tools/m19r_qualify.py` and `tools/test_m19r_qualify.py` (pre-existing violation,
   not ours).
6. OSC-0B (#76, draft): Field digests are not semantic ids (`field.h:11-14`). T records must not mint Omega semantic
   ids. Contract digest stays PROVISIONAL (`field.h:16-17`).
7. Confidence enum vs FORGE V2 `energy_source` {NOT_MEASURED, MEASURED, ESTIMATED}
   (`forge_substrate_v2.h:192-194`): the brief's six-level enum is finer. Keep a documented mapping
   (DIRECT/COUNTER_DERIVED to MEASURED, MODELLED/COUNTERFACTUAL to ESTIMATED, UNATTRIBUTED to NOT_MEASURED). Do not
   change FORGE.
8. Package energy includes other tenants (LM Studio and the AI router were resident during C2,
   `r15-performance-proof.md:620-622`). The E_0 idle window is essential and the process list must be recorded.
9. `rx_costmodel` energy is MODELLED (latency × power), not measured per call (`rx_costmodel.c:200`). Do not relabel it
   DIRECT.
