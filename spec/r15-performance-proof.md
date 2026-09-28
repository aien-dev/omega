# R15 performance proof (pre-registration)

Status: METHODOLOGY. This file is committed before any R15 qualification data
is collected. Its acceptance criteria are binding. Changing a criterion after
qualification data exists requires a new commit to this file that says what
changed and why, followed by a rerun of the whole qualification. Only a clean,
candidate-bound GB10 run that meets every gated criterion below may say
`R15_REACTION_PERFORMANCE = PASS`. R16 is not claimed by R15.

ADR 0016 §48: compare against the existing pipeline; measure the seventeen
listed quantities; do not accept architecture on aesthetic grounds.

## 1. The resident path

The resident path is the merged R14 organism (omega `f70ae10`):

- `src/runtime/rx_world.c`: one object world, dependency index
  (`propagate`), per-priority ready rings, R5 admission (`try_admit`), worker
  threads, snapshot compute, re-validation and atomic publication, SHA-256
  causal crumbs;
- faculties `rx_omega.c` (R10), `rx_aien.c` (R11), `rx_aegis.c` (R8),
  `rx_living.c` (R13/R14), R9 `rx_generation.c`, R12 `rx_resident_gpu.c`;
- native C authority from aienos `c8ab65e` (`native/capability`), linked as in
  R13;
- the GB10 resident seat through physics `fecbedb`.

Work progresses because a publication makes dependents ready. No harness code
calls a faculty.

## 2. The existing pipeline, and why the old Rust loops are not the baseline

The prompt names three live central loops. Each was read at the current main
(sovereign-core `63fe7a7`, aegis-runtime `2bbce76`):

| Loop | What it actually computes | Same semantic work as R13? |
|---|---|---|
| sovereign-core `AienRuntimeSpine::run_until_complete` / `step` (`crates/aien-runtime/src/spine.rs:112-180`) | one batched LLM forward pass per step: prefill chunks and one decode token per sequence over a paged KV cache (`AienInferenceBackend::execute_step`) | no. There is no goal, plan, candidate realization, verification, measurement, selection, authority grant, GPU experiment or generation promotion. "Generation" there is sequence-id reuse. |
| aegis-runtime `AgentEngine::execute_task` (`src/agent.rs:95-236`) | an OpenAI-style tool-calling chat loop against an HTTP LLM server (`127.0.0.1:18006`), running shell/file tools | no. Its unit of work is an LLM chat turn. |
| aegis-runtime `HeartbeatEngine::pulse_once` / `start_loop` (`src/heartbeat.rs:94-235`) | every interval, scan SQLite pending tasks and run a chore (health GET, shell command, crumb, fixed string) | no. |

These are reported as **NOT DIRECTLY COMPARABLE**. Timing them against R13
would compare an LLM decode or a chat turn with a matrix-vector realization
search. That is a different algorithm, not an orchestration difference. They
are also Rust, which AIEN is retiring. Their orchestration *pattern* is what
R15 must compare against, so the baseline is built from it.

### 2.1 The sequential control (SEQ)

SEQ is the narrowest fair sequential reference: the same process, the same
objects, the same reaction bodies, the same authority, the same crumbs, the
same R9 store and the same GPU seat. Only orchestration differs.

- The dependency index does not wake anything (`propagate` is not consulted)
  and there are no worker threads or ready rings.
- One orchestrator thread runs the classic central loop of both legacy
  designs: `run_until_complete` over `pulse` passes. A pulse walks a fixed
  faculty order (§2.2) and, for each stage, polls semantic readiness the way a
  heartbeat scans pending tasks: a stage is ready when a field it declares as a
  trigger has a newer version than the stage last saw. A ready stage is run to
  completion on the orchestrator thread before the next stage is considered.
- A stage run is the engine's own activation (`run_one`): liveness check,
  capability validation, snapshot, the reaction function, stale-read check,
  capability re-validation at publication, write-set check, atomic
  publication, crumb. Nothing is skipped. The crumb's wake cause is the
  writer of the newest trigger field that made the stage ready, so the causal
  record has the same meaning.
- A GPU stage posts its claim on the same ring and then the orchestrator
  waits for the completion (host wait) before continuing, as "dispatch GPU,
  wait" does in a central loop.
- R6 stability accounting, quarantine and budgets apply unchanged.
- When a full pulse runs nothing, the orchestrator polls again without
  sleeping (busy poll with `yield`). The legacy spine sleeps 5 ms when idle and
  the heartbeat sleeps for its interval; SEQ does neither, so it is the
  strongest honest version of the pattern, not a deliberately bad one.

SEQ is selected at world creation (`rx_world_init_sequential`). It is a
measurement reference, never a production mode; the receipt records the
symbol that selects it.

### 2.2 Fixed faculty order (SEQ pulse)

production (`workload.serve`) → AEGIS (`living.experiment.ask`, `aegis.decide`,
`root.install`) → AIEN (`observe`, `predict`, `explain`, `assess`, `plan`) →
Omega (`omega.watch`, `omega.reconsider`, `omega.synthesize.k`,
`omega.verify.k`, `omega.measure.k` for k = 0..slots-1, `omega.select`) →
experiment (`living.experiment.prepare`, `living.blackwell.add` [GPU, wait],
`living.experiment.evidence`) → AIEN (`aien.experiment`) → generation
(`generation.prepare`, `generation.promote`, `generation.restore`). This is
the order the R13 golden path narrates (goal → AIEN → Omega → authority → GPU
→ evidence → promotion) with production first in every pulse. Reactions not
in this list, if any are registered, are polled last in registration order.

## 3. Configurations

| Name | Orchestration | Threads doing semantic work | Build |
|---|---|---|---|
| RES-4 | resident | 4 workers (the R13 production configuration) | production |
| RES-1 | resident | 1 worker | production |
| SEQ | sequential control | 1 orchestrator | production |
| RES-1-NODIGEST | resident | 1 worker | measurement build `-DRX_MEASURE_NO_CAUSAL_DIGEST` |

RES-1 vs SEQ isolates the orchestration mechanism (one thread each).
RES-4 vs SEQ compares the architecture as deployed against the central loop.
RES-1-NODIGEST exists only to price causal evidence: crumbs are still
appended, their SHA-256 digests are not computed. It never runs a correctness
gate; the receipt records that production binaries do not define the macro.

All configurations use the same compiler (gcc 13.3.0), `-O2`, the same
sources, and the same producer (one closed-loop production client thread).

## 4. Workloads and semantic operations

### W-PROD: production matvec (the semantic operation of R13)

One **production operation** = one request (M=64, N=256, seed = seq ·
0x2545F4914F6CDD1D) published by the producer through
`rx_world_publish_external`, answered by `workload.serve` with a result whose
digest equals the reference digest recomputed by the producer, and read back
by the producer. Throughput = production operations per second of wall time.
A wrong digest is a correctness failure, not a slow operation.

### W-EPISODE: the R13 living episode

One **semantic result** of adaptation = goal MET: human goal → AIEN plan →
Omega epoch → verification → 16 GPU trials on the existing R8 grant →
evidence → AIEN belief → selection → R9 candidate → promotion → in-force
record → production runs it → AIEN assesses the goal MET. Same objects,
policy, `TARGET_PCT` 55, `hot_calls` 64, `hot_ns` 200000, `margin_pct` 10 as
`rx_r13_living.c`.

### One trial

One trial is one fresh process running one configuration through:

1. build the body (authority, R8 grant acquisition, objects, reactions);
2. placement A725, threads on the A725 cores, serve until Omega's first
   selection and AIEN's confirmed prediction exist (warm-up, not measured);
3. **BEFORE window**: 2 s untimed warm-up of production, then a 5 s measured
   window of steady production on the A725 cores;
4. threads to the X925 cores, placement X925, goal published: **ADAPT
   window** from the goal crumb to the promotion crumb, production running
   throughout;
5. wait until AIEN assesses the goal MET on the in-force record;
6. **AFTER window**: 2 s untimed warm-up, 5 s measured window of steady
   production on the X925 cores with the promoted realization in force.

## 5. Benchmark levels

### Level 1 — substrate microbenchmarks (RES-1 and SEQ where meaningful)

Pinned single process, X925 cpu 7 (driver) and 8 (worker/orchestrator),
authority = native C, evidence on. 5 runs × 10,000 samples after 1,000
warm-up samples per measure.

- **A activation latency**: external publication timestamp (taken inside
  `rx_world_publish_external` before the lock is released) → the reaction's
  `run_one` start. RES-1: through the dependency index, ready ring and worker
  wake. SEQ: through the orchestrator's readiness poll. p50/p90/p95/p99/p99.9/max.
- **B dependency propagation**: one committed field → dependents inserted in
  the ready ring, at fanout 1, 2, 4, 8, 16, 32, 64, 128, 256 (one object, n
  subscribed reactions). Cost per wave and per dependent (CPU ns inside
  `propagate`, measured with `CLOCK_MONOTONIC` around the call under the
  lock).
- **C scheduler overhead**: ready → admitted → running, split into
  uncontended; resource-blocked (slots = 1, a holder releases); priority
  contention (7 classes, starvation bound 4). Reported as CPU time spent in
  admission/queue code per activation (thread CPU clock) and wall time
  ready→running.
- **D publication cost**: reaction return → atomic publication committed →
  dependents visible (propagate done), per commit.
- **E capability validation**: the R7/R8 fast path. `rx_world_validate_cap`
  on an existing valid capability through the native authority view, 1e6
  checks, plus the whole activation cost of a reaction holding an existing
  grant. No policy evaluation, no mint.
- **F causal evidence overhead**: the same W-PROD loop, RES-1 vs
  RES-1-NODIGEST: CPU cycles per operation (PMU `cpu_cycles`), latency,
  crumb bytes per operation, throughput.
- **G generation barrier**: `rx_gen_promote` on a prepared candidate while a
  production thread keeps publishing: candidate prepared → barrier begins →
  durable flip → receipt complete → live work continues. 30 barriers per run,
  5 runs. Reports latency distribution, production commits during the
  barrier, bytes written to the store, `fsync`/`fdatasync` calls (counted by
  the store's I/O wrapper).

### Level 2 — CPU/GPU cooperation (GB10 seat, RES-1 and SEQ)

The R12/R13 resident seat, fixed add of fields 0 and 1 (a physical
cooperation benchmark, not a matvec). 5 runs × 256 claims.

Timestamps per claim: CPU input publication; claim posted on the ring; chip
pickup and chip completion (the chip's `%globaltimer`, written by the seat to
its heartbeat block; the difference is GPU execution time); host observes the
completion; world publication committed; dependent (`living.experiment.evidence`)
starts. Reported separately: host enqueue/claim time, GPU execution time,
publication time, dependent wake time, end to end.

**CPU↔GPU synchronization** is counted by instrumentation: claim notices
written (doorbell/ring store), completion notices taken, host polls that found
nothing (channel waits), host blocking waits (SEQ), fences (the seat's
`MEMBAR.SC.SYS` per result, counted per completion), explicit barriers
(`rx_world_wait_quiescent`, seat hold). Per GPU result.

Also: serialization/copy bytes on the rings (descriptor bytes) and the
coherent projection; claim-ring occupancy (max and mean in-flight); GPU
residency (§6.10); backpressure (claims waiting on R5 admission).

### Level 3 — the R13 living workload (W-EPISODE, RES-4)

From the crumbs of each trial's ADAPT window: cognitive reaction time (goal →
AIEN plan), Omega search/verification time (plan → selection), GPU
experiment time (first claim → evidence), promotion time (candidate →
in-force), production throughput during adaptation, BEFORE and AFTER steady
production, adaptation cost, time to benefit, and the crossover:

- **adaptation cost** = Σ (t_end − t_start) over every non-production
  activation crumb from the goal crumb to the in-force crumb (worker time
  occupied by adaptation, including sandboxed verification and GPU trials),
  in ns;
- **gain per operation** = reference ps per call on X925 − selected ps per
  call (selection fields 7 and 5), in ns;
- **crossover** = adaptation cost / gain per operation, in production
  operations; and in seconds at the AFTER throughput.

### Level 4 — resident vs sequential (W-EPISODE trials, all configurations)

BEFORE/AFTER throughput, ADAPT-window production rate, episode wall time
(goal → goal MET), energy per production operation and per episode, wake
amplification, wasted reactions, invalidation and conflict rates.

## 6. Mandatory metrics: definitions

1. **Semantic operations/s**: production operations per second (§4) in
   BEFORE, ADAPT and AFTER windows; episodes per hour.
2. **Activation latency**: L1-A distribution; also, per trial, crumb
   `t_start` minus the timestamp of the wake-cause crumb for every
   activation (distribution).
3. **Dependency propagation cost**: L1-B, per fanout.
4. **Scheduler overhead**: L1-C (CPU and wall separately); per trial, CPU ns
   in scheduling code per activation (instrumented counters).
5. **AEGIS fast-path cost**: L1-E.
6. **Generation barrier latency**: L1-G and the promotion in every trial,
   with production commits during the barrier.
7. **Serialization bytes**: instrumented, never inferred: snapshot bytes
   copied into `RxCtx`, crumb bytes appended, coherent-projection bytes
   written, ring descriptor bytes, R9 bytes written. Per operation.
8. **CPU↔GPU synchronization count**: L2 definition, per GPU result.
9. **Memory traffic**: ARM PMU per-thread counters inherited by all threads
   of the process, over each window: `ll_cache_miss_rd` (last-level read
   misses; × 64 B = DRAM read bytes, derivation stated), `bus_access`
   (× 64 B = bus bytes, derivation stated), `l2d_cache_refill`,
   `mem_access`. Per operation. `perf_event_paranoid` is set to 1 for the run
   and restored; the receipt records both values.
10. **GPU residency**: fraction of 1 ms samples during silicon runs in which
    the seat is established and its live counter (`RX_SEAT_HB_LIVE`) advanced
    since the previous sample; plus physical occupancy = Σ GPU execution time
    / wall time. KV/memory allocation percentage is never used.
11. **Resource utilization**: process CPU time (user+sys, `getrusage`) per
    window, per-thread CPU of workers/orchestrator, R5 slot peak and mean
    held slots, resident claim peak, GPU occupancy (10).
12. **Wasted reactions**: activations that published nothing, by class:
    NOOP, INVALIDATED (stale input), REJECTED (authority or write set at
    publication), BLOCKED_AUTHORITY, FAILED, QUARANTINE, replays (an
    activation re-run under the same cause after invalidation). Fraction of
    all activations and per consequential stimulus.
13. **Invalidation rate**: INVALIDATED / activations.
14. **Conflict rate**: (INVALIDATED + REJECTED for write-set/stale) /
    activations.
15. **Wake amplification**: reaction wake attempts (RES: `demand` calls; SEQ:
    stage readiness polls) / consequential external stimuli (outside
    publications that led to at least one commit). Also successful
    activations (commits) per stimulus.
16. **Causal-trace overhead**: L1-F.
17. **Energy per semantic result**: §7.

## 7. Energy

Physical telemetry only. Sources inspected on this machine (2026-09-28):

- GB10 graphics energy counter (`nvmlDeviceGetTotalEnergyConsumption`, mJ,
  hardware accumulator) and power (`nvmlDeviceGetPowerUsage`). Verified to
  respond to GPU load and to barely respond to full load on all 20 CPU cores
  (4.1 → 4.7 W), so it covers the graphics domain only.
- The SoC power manager's telemetry block `SPBM` (ACPI `NVDA8800` "MTEL",
  memory window `0x1c238000`, 4 KiB) exposes hardware energy accumulators for
  the package (`SPBM_PKG_ENERGY_VALUE_ACCUMULATE`, +0x344), CPU P-cores
  (+0x35c), CPU E-cores (+0x350), GPC (+0x368) and GPM (+0x374), and
  instantaneous telemetry for total system power (+0x300) and SoC package
  (+0x304). With Secure Boot on, kernel lockdown (integrity) forbids
  `/dev/mem`; no in-tree driver binds `NVDA8800`; no machine-owner key is
  enrolled. It is therefore not readable today without an owner decision.

Method (binding): energy per semantic result = Δ(package energy accumulator)
over the window / production operations in the window (and / episodes for
W-EPISODE), sampled at the window boundaries and at 10 Hz within it (to
detect overflow and to report power). Idle baseline: 5 s of package energy
with the process started and quiescent immediately before BEFORE; reported
gross and net (gross − idle power × window). CPU P/E and GPU domain energies
reported alongside. GPU-domain energy from NVML is recorded as a second,
independent source. Uncertainty: accumulator resolution and sampling jitter at
window edges (±1 sample period of power) are reported per window.

**If package energy cannot be read by a physical hardware source at
qualification time, metric 17 is incomplete and R15 cannot PASS.** GPU-domain
energy alone is not accepted as energy per semantic result. How the package
accumulators become readable (owner-key signed read-only telemetry reader, or
an external logging meter) is an owner decision; the chosen method will be
recorded in a new commit to this section before qualification.

## 8. Fairness controls

- Same sources, compiler, flags, authority, R8 grant, objects, reactions,
  checks, crumbs and R9 store in every configuration (§2.1). SEQ runs
  `run_one` itself.
- Same producer, same seeds, same M, N, same closed-loop concurrency (one
  outstanding request).
- Same core class per phase: A725 for warm-up and BEFORE, X925 for ADAPT and
  AFTER, in every configuration. All process threads are confined to that
  class's cores (A725: 0-4, 10-14; X925: 5-9, 15-19). Level 1 pins single
  cores on X925.
- Same GPU (one GB10 seat), same claim ring.
- Warm state: a window starts only after the §4 warm-up steps; no
  configuration is measured cold against another warm.
- Realization: AFTER windows are compared only between trials whose in-force
  realization identity is the same; a pair that differs is reported and not
  entered into the AFTER ratio. At least 10 valid pairs are required.
- No safety check, provenance or authority is disabled in any configuration
  that is compared for throughput, except RES-1-NODIGEST, which exists only
  for metric 16.

## 9. Machine preparation

- CPU governor `performance` on all cores (recorded), frequency boost as
  reported by the kernel (recorded), scaling_cur_freq of the used cores
  recorded before and after each trial.
- Thermal: before each trial the harness waits (up to 120 s) until every
  `thermal_zone` reads ≤ 55 °C, and records all zone temperatures and GPU
  temperature before and after. A trial that starts hotter after 120 s is run
  and flagged, not dropped.
- Nothing else heavy runs: the harness records the load average and the top
  CPU consumers before the qualification; `aien-astrosage` or any other model
  server must not be serving.
- Seat: one GB10 seat per trial process (begin before step 2, finish at the
  end).

## 10. Trials, interleaving, statistics

- W-EPISODE: 12 rounds. Each round runs RES-4, RES-1, SEQ, RES-1-NODIGEST as
  separate processes, in an order rotated by round (Latin square), so no
  configuration always runs first or last. Pairs are (RES-1, SEQ),
  (RES-4, SEQ) and (RES-1, RES-1-NODIGEST) within a round.
- Level 1: 5 runs per measure, interleaved RES-1/SEQ where both exist.
- Level 2: 5 runs × 256 claims per configuration, interleaved.
- Fixed seeds everywhere (production seeds as §4; bootstrap seed 0x15).
- Every raw observation is recorded (§12). Reported: median, p95/p99 for
  latencies, and 95% bootstrap confidence intervals (percentile method,
  10,000 resamples) of the median of paired ratios for primary comparisons.
- Outliers: nothing is discarded. A trial that crashes, times out or fails a
  correctness check is a failed trial; any failed trial in a compared
  configuration fails the qualification (it is not rerun away). Level 1
  samples are never trimmed; max and p99.9 are reported.
- No best-of-N is reported as representative.

## 11. Acceptance criteria (gated)

Budgets are justified by the production goal (a steady matvec service that
must keep serving while the organism adapts), by the measured legacy dispatch
units already in evidence (R8: legacy `spark-aegis` process-spawn floor
221 µs per authority decision; sovereign-core spine idle sleep 5 ms), and by
the ADR requirement that evidence and authority stay on.

| # | Criterion | Threshold | Why |
|---|---|---|---|
| G1 | Correctness in every trial of every compared configuration | 0 wrong results; 0 illegal transitions; crumbs verify; goal MET; generation advanced exactly once; 0 uses of a realization not in force; AEGIS/root not woken during the episode | performance of a wrong system is meaningless |
| G2 | Orchestration mechanism, AFTER throughput RES-1 / SEQ | median paired ratio ≥ 0.90 and 95% CI lower bound ≥ 0.85 | the reaction mechanism may cost a little per operation (≤ 10%) on the harshest case: a ~3-5 µs operation where per-activation orchestration is the largest share |
| G3 | Production during adaptation, ADAPT-window rate RES-4 / SEQ | 95% CI lower bound of the median paired ratio > 1.0 | the architectural claim: adaptation does not stop production |
| G4 | Production during adaptation, RES-4: ADAPT-window rate / BEFORE-window rate of the same process | median ≥ 0.50 | adapting may not halve the production service the body gave before the goal (BEFORE runs the reference on A725, the slower class, so this cannot be met by the faster cores alone hiding a stall) |
| G5 | Activation latency (L1-A, RES-1, uncontended) | p50 ≤ 50 µs and p99 ≤ 221 µs | a reaction activation must be cheaper than the legacy dispatch unit (R8 spawn floor) |
| G6 | AEGIS fast path (L1-E) | p50 ≤ 1 µs and p99 ≤ 5 µs | ≤ 20% of the cheapest semantic operation (~5 µs) |
| G7 | Generation barrier (L1-G and per-trial promotion) | production commits during the barrier in ≥ 90% of barriers; barrier latency p99 ≤ 500 ms | promotion must not be a global stop; durable flip at human scale |
| G8 | Causal evidence overhead, AFTER throughput RES-1 / RES-1-NODIGEST | median ≥ 0.50 | evidence may not cost more than the work it witnesses |
| G9 | Energy per production operation, AFTER window, RES-1 / SEQ (package, net) | median paired ratio ≤ 1.15 and 95% CI upper bound ≤ 1.25 | mechanism cost must be visible in energy and bounded like G2 |
| G10 | CPU↔GPU synchronizations per GPU result (L2), RES-1 vs SEQ | RES-1 ≤ SEQ | dependency-driven completion must not need more synchronization than a central wait |
| G11 | Adaptation amortizes (L3, RES-4) | median crossover ≤ 10,000,000 production operations | an adaptation that does not repay its cost within the order of minutes of production at the measured rate is not accepted |
| G12 | Wake amplification, ADAPT+AFTER, RES-4 vs SEQ | RES-4 wake attempts per consequential stimulus ≤ SEQ readiness polls per consequential stimulus | the dependency index must do less wake work than polling |
| G13 | Wasted reactions per consequential stimulus, RES-1 vs SEQ | median ratio ≤ 1.10 | reactions may not waste more activations than the same stages driven centrally |
| G14 | Conflict rate, RES-4, ADAPT window | median ≤ 25% of activations | concurrency may not turn into mostly re-done work |
| G15 | GPU residency during silicon trials | seat established and live in ≥ 99% of samples | the seat is resident, not relaunched per use |
| G16 | Every mandatory metric 1-17 present in the summary, reproduced by the reducer from raw evidence | all present | ADR §48 |

Reported, not gated (must be present): propagation cost by fanout, scheduler
CPU/wall split, publication cost, serialization bytes, memory traffic,
resource utilization, invalidation rate, BEFORE-window comparisons, RES-4 vs
RES-1, GPU-domain energy, episodes per hour, crossover in seconds.

The architecture need not win every microbenchmark. Any metric where the
resident path is worse than SEQ is listed in the receipt under
`regressions` with its size and the justification.

If a gated criterion fails: no PASS. Profile, fix the defect at its layer, add
coverage, rerun the correctness gates, and repeat the entire qualification.

## 12. Evidence

- Raw: `evidence/R15/raw/<run-id>/` — one JSON Lines file per process
  (`<level>-<config>-<round>.jsonl`), one record per observation (sample,
  window, crumb-derived interval, counter snapshot, telemetry sample), each
  with run id, configuration, round, trial, and monotonic timestamps; plus
  `machine.json` (§13) and `SHA256SUMS`.
- Reducer: `tools/r15_reduce.c` (C, no Python), deterministic, reads only the
  raw directory, recomputes every summary number and every gate, and writes
  `summary.json`. The receipt embeds the reducer source SHA-256 and the raw
  directory digest (SHA-256 of `SHA256SUMS`).
- Receipt: `evidence/R15/<sha256>.json`, schema
  `AIEN_RX_R15_REACTION_PERFORMANCE_V1`, named by the SHA-256 of its own
  bytes: candidate_commit, run_commit, candidate_bound, tree_dirty,
  silicon_observed, hardware identity, aienos and physics commits, benchmark
  binary SHA-256s, raw digest, reducer digest, all 17 metrics, legacy status
  (NOT DIRECTLY COMPARABLE, §2), resident and SEQ results, paired comparisons,
  CIs, thresholds and outcomes, correctness reruns, regressions, `not_claimed`.
- No headline number is edited by hand. The receipt's metric values are
  copied by the reducer.

## 13. Hardware identity (recorded per run)

Host name, `/etc/machine-id` SHA-256, kernel release, CPU MIDR per core and
core-class map, governor and frequencies, memory size, GPU name and PCI id,
driver version, Secure Boot and lockdown state, `perf_event_paranoid` before
and during, thermal readings, omega/aienos/physics commits, compiler version,
benchmark binary SHA-256.

## 14. Correctness reruns required before a PASS

R3, R7, R8, R9, R10, R11, R12 host, R12 silicon, R13 host, R13 silicon, R14
host, R14 silicon, on the candidate commit, clean tree, recorded in the
receipt.

## 15. Not claimed by R15

R16; whole-project performance beyond the measured workloads; LLM decode or
agent-loop performance (the legacy loops, §2); general cognition; open-ended
synthesis; energy of anything outside the measured windows.
