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
written (doorbell/ring store), completion notices taken, host blocking waits
(SEQ), fences (the seat's `MEMBAR.SC.SYS` per result, counted per
completion), explicit barriers (`rx_world_wait_quiescent`, seat hold). Per
GPU result. Host polls that found nothing (channel waits) are NOT
synchronization events; they are reported separately as **host spin cost**
(empty polls per GPU result, both configurations, not gated). See amendment
A1 (§21).

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
8. **CPU↔GPU synchronization count**: L2 definition (synchronization
   events, amendment A1), per GPU result; host spin cost (empty polls per
   GPU result) reported alongside, not gated.
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

> **Amendment A1 (2026-09-28, after qualification attempt 1, decided by
> Drake): G10 changed.** G10 now counts CPU↔GPU synchronization EVENTS per
> GPU result (claim notices, completion notices, fences, explicit barriers,
> host blocking waits); empty completion polls are excluded from the gate
> and reported, not gated, as "host spin cost: empty polls per GPU result"
> for both configurations. Why, what the data showed, and the limit (a
> multi-claim-in-flight phase, Option B, is deferred to a later gate) are in
> §21. Attempt 1 (run 20260929T020536Z-ad8e1f2ea4e4-silicon) stays FAIL as
> recorded; the whole qualification is rerun under this amendment.

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
| G10 | CPU↔GPU synchronization EVENTS per GPU result (L2), RES-1 vs SEQ: claim notices, completion notices, fences, explicit barriers, host blocking waits; empty polls excluded (amendment A1, §21) | RES-1 ≤ SEQ | dependency-driven completion must not need more synchronization than a central wait |
| G11 | Adaptation amortizes (L3, RES-4) | median crossover ≤ 10,000,000 production operations | an adaptation that does not repay its cost within the order of minutes of production at the measured rate is not accepted |
| G12 | Wake amplification, ADAPT+AFTER, RES-4 vs SEQ | RES-4 wake attempts per consequential stimulus ≤ SEQ readiness polls per consequential stimulus | the dependency index must do less wake work than polling |
| G13 | Wasted reactions per consequential stimulus, RES-1 vs SEQ | median ratio ≤ 1.10 | reactions may not waste more activations than the same stages driven centrally |
| G14 | Conflict rate, RES-4, ADAPT window | median ≤ 25% of activations | concurrency may not turn into mostly re-done work |
| G15 | GPU residency during silicon trials | seat established and live in ≥ 99% of samples | the seat is resident, not relaunched per use |
| G16 | Every mandatory metric 1-17 present in the summary, reproduced by the reducer from raw evidence | all present | ADR §48 |

Reported, not gated (must be present): propagation cost by fanout, scheduler
CPU/wall split, publication cost, serialization bytes, memory traffic,
resource utilization, invalidation rate, host spin cost (L2 empty polls per
GPU result, RES-1 and SEQ, amendment A1), BEFORE-window comparisons, RES-4 vs
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

## 16. Clarification C1 (2026-09-28, before any qualification data)

Committed after a recovery audit of the branch and the machine and before
any R15 qualification data exists. It corrects measurement definitions that
were ambiguous or internally contradictory. It does not change G1–G16, their
thresholds, the configurations, the workloads or the statistics (§3–§11).
Sections 1–15 above are unchanged and remain as pre-registered at `a4c6123`.
An unpushed local commit (`f714c08`) had rewritten §7 in place; that rewrite
is not adopted. Its telemetry reader and preflight data are kept under
`research/m15/spbm/`; the energy method is settled only by C2 (item 10).

1. **Scheduler CPU and wall time (§5 L1-B, L1-C; §6.3, §6.4).** Wall time is
   `CLOCK_MONOTONIC`. CPU time is `CLOCK_THREAD_CPUTIME_ID` read on the thread
   that runs the measured code, both ends of an interval on one thread. They
   are recorded side by side and never substituted for one another. §5 L1-B
   says "CPU ns … measured with `CLOCK_MONOTONIC`"; that is wall time, so
   L1-B reports both per wave. Scheduling code is: wake (`demand`),
   admission and ready-ring insertion, a worker's ready-ring pop, and the
   post-activation release/admission/deferred/parked wakes (RES); readiness
   polling of every stage and admission (SEQ). Per-activation values are in
   `RxTiming.sched_ns` / `sched_cpu_ns`; process totals in
   `RxStats.sched_wall_ns` / `sched_cpu_ns`, counted once however deeply the
   code nests. Metric 4 per activation = total / activations.
2. **Dependency propagation (§6.3, L1-B).** A wave's counters
   (`RxPropWave`): subscriptions inspected, subscriptions matched
   (generation and field mask), wake attempts, wakes accepted (a DORMANT
   reaction made waiting), **new ready insertions**, coalesced, deferred and
   suppressed wakes, and wall and CPU time. "Dependents inserted into the
   ready ring" is `ready_inserts`; cost per dependent = wave time /
   `ready_inserts`. An L1-B sample in which `ready_inserts` differs from the
   fanout is a failed sample, not a data point. Metric 15's RES wake
   attempts remain `wakes` (every `demand` call), as §6.15 defines.
3. **Serialization bytes (§6.7).** "Bytes" means **copied bytes**: bytes
   processor code writes into a destination as a copy or serialization of
   data that exists elsewhere. Counted per path, each its own counter:
   snapshot entries filled into `RxCtx`; current fields copied into the
   publication staging buffer; crumb records appended; coherent table entry
   members and field windows written; descriptors written to and copied out
   of the processor→seat ring (publications, claims, shutdown) and the
   seat→processor ring; window relocations; the coherent-bind copy; R9 bytes
   passed to `write()` (item 5). Not counted as copies, and reported as
   such: the reaction bodies' own data structures (identical in every
   configuration); one-time seat launch images (made before any window);
   the graphics chip's own reads and writes of descriptors and windows,
   which are not processor copies. Chip-side traffic is reported separately
   as **transferred bytes, derived**: claims posted × 128 B descriptor read
   + results taken × 128 B descriptor written + per-claim window and
   heartbeat bytes loaded and stored, where the per-claim byte counts are
   enumerated from the seat program's load and store instructions
   (`rx_resident_gpu.c`) and listed in the receipt beside the derivation. No path is assumed zero-copy because of the architecture.
4. **Timing buffers.** `rx_world_timing_status` returns `RX_ERR_FULL` when any
   sample was dropped. A measure whose buffer dropped a sample fails; its
   statistics are not reduced.
5. **R9 I/O (L1-G, §6.7).** Option A: each store counts the bytes it passes
   to `write()` and the `fsync` calls it makes (`rx_gen_store_io`). Each
   window records before/after snapshots of the trial's own store, and of
   the process total; if the process-total delta differs from the sum of
   the trial's stores' deltas, the window fails (unrelated store activity).
   Directory creation, `rename` and `unlink` are metadata operations, counted
   by the sync that follows them, not as bytes.
6. **Instrumentation parity (§8).** Configurations that are compared run
   the same instrumentation. Counters in `RxStats` are always on in every
   build. The per-activation timing buffer is off in every W-EPISODE trial
   of every configuration (activation latency in trials comes from crumb
   timestamps, §6.2) and on in both RES-1 and SEQ for L1-A, L1-B and L1-C.
   Instrumentation overhead is measured, not assumed: an L1 W-PROD run of
   RES-1 and of SEQ with the timing buffer on and off (5 runs each,
   interleaved), reported as a throughput ratio, not gated.
   `RX_MEASURE_NO_CAUSAL_DIGEST` is used only for RES-1-NODIGEST (metric 16).
7. **`perf_event_paranoid`.** The spec (§6.9) says 1; the Codex handoff said
   0. 1 is correct for the PMU model in item 8 (per-task counting of the
   process's own threads including kernel mode needs ≤ 1; 0 would only be
   needed for per-CPU counting, which is not used). Values: **original 4**
   (Ubuntu boot default, `CONFIG_SECURITY_PERF_EVENTS_RESTRICT=y`;
   this machine was found at 0 on 2026-09-28 after an unlogged change and
   restored to 4 at the audit), **qualification 1**, **restored 4**. The
   qualification script records all three in `machine.json` and fails the
   run if the restore does not read back 4.
8. **PMU model (§6.9).** GB10 has two CPU PMUs: `armv8_pmuv3_0` (type 10,
   Cortex-A725, MIDR part 0xd87, cpus 0-4,10-14) and `armv8_pmuv3_1` (type
   11, Cortex-X925, part 0xd85, cpus 5-9,15-19); types and cpu lists are
   read from sysfs at run time and recorded. Events (raw config, identical
   on both): `cpu_cycles` 0x11, `inst_retired` 0x08, `bus_access` 0x19,
   `ll_cache_miss_rd` 0x37, `l2d_cache_refill` 0x17, `mem_access` 0x13.
   - Source and scope: per task (`pid = 0`, `cpu = -1`), opened by the
     trial process's main thread before any other thread is created, with
     `inherit = 1`, so every thread of the process is counted and nothing
     else is. One event per (event, PMU) pair, 12 descriptors, no groups
     (group reads are not relied on with `inherit`). `exclude_hv = 1`; user
     and kernel mode are both counted.
   - Boundaries: opened `disabled`; each window does `RESET` + `ENABLE` at its
     start timestamp and `DISABLE` at its end timestamp, then reads.
   - Read format `TOTAL_TIME_ENABLED | TOTAL_TIME_RUNNING`. The event count
     is the **raw sum over the two PMUs**. It is **never scaled** by
     enabled/running: on a heterogeneous machine a task's event on one PMU is
     not running while the task is on the other class, so running < enabled
     does not mean multiplexing. Multiplexing test: for every event,
     running(A725) + running(X925) must equal enabled within 0.1%; a window
     that fails it reports its PMU values as multiplexed, unscaled, with the
     ratio, and metric 9 does not use it.
   - Attribution after migration was checked on this machine before this
     commit: one thread ran a fixed loop on cpu 1 (A725) then on cpu 6
     (X925); `inst_retired` counted 802.2M on the A725 PMU and 801.6M on the
     X925 PMU (sum 1.604G for 2 × 800M), running fractions 0.5827 + 0.4173
     = 1.0000, and all six events fitted without multiplexing.
   - Not counted: kernel threads acting for the process (for example
     writeback) and other processes. Metric 9 is stated as the process's
     own traffic.
9. **SEQ parity before SEQ is a baseline.** SEQ is used as a baseline only
   after a semantic parity gate passes on the candidate: RES-1 and SEQ from
   the same initial state and stimulus reach equivalent goal, plan, search
   epoch, verified selected realization, GPU experiment evidence, AIEN
   belief, generation identity, in-force realization and authority outcome,
   both crumb graphs verify, and no trigger is lost; repeated runs. The gate
   and its runs are recorded in the receipt with the correctness reruns
   (§14).
10. **Energy.** §7 stands as written. The owner enrolled a machine-owner key
    on 2026-09-28 and a signed read-only SPBM reader exists
    (`research/m15/spbm/`). Before it may supply metric 17, a further
    clarification **C2** will record: accumulator width, scale and rollover
    behaviour; monotonicity; idle, CPU, GPU and combined-load comparisons;
    the GPU-domain comparison with NVML; and the stated uncertainty. No R15
    qualification data is collected before C2 is committed. If C2 cannot
    make the method defensible, metric 17 is incomplete and R15 does not
    PASS (§7).

## 17. Clarification C2 (2026-09-28, before any qualification data)

Settles the energy method promised in §16 item 10. G1–G16 and §7's binding
method are unchanged; this names the physical source and its limits.

- **Source.** Package energy for metric 17 is the SPBM package accumulator
  (`SPBM_PKG_ENERGY_VALUE_ACCUMULATE`, +0x344) read through the owner-key
  signed read-only reader `research/m15/spbm/aien_spbm_readonly.c` as hwmon
  `energy1_input` (label `pkg`). Secure Boot and integrity lockdown stay on.
  The loaded module's srcversion and `.ko` hash are recorded per run and
  must match the source hashes in `research/m15/spbm/runs/*/source-binary.sha256`.
- **Width, scale, reset.** 32-bit register read, 1 mJ per count (exposed as
  µJ). On 2026-09-28 the package counter read 196,494 J after 10,514 s of
  uptime (18.7 W mean), matching measured 16–21 W: it counts from zero at
  boot and did not wrap. It wraps at 2^32 mJ ≈ 4.29 MJ (~54 h of uptime at
  22 W). Rollover and the overflow register's behaviour were never observed;
  they are not unwrapped. A window is refused if any overflow flag is set,
  any counter decreases, or headroom is below the window length at a
  600 W bound (`sample.c`).
- **Validation** (`research/m15/spbm/runs/c2-20260928-01`, 3 rounds × idle,
  CPU = 4 X925 cores, GPU = resident seat claims, both; 101 samples per
  10 s window, all monotonic, no overflow): package accumulator rate minus
  integrated SoC-package power = +0.1…+0.5 W in all 12 windows (16.0–21.4 W),
  so the 1 mJ scale agrees with the firmware's power telemetry to ~2–3%.
  CPU load raised CPU-P by ~4.7 W; GPU load raised GPM by ~2.5 W and NVML
  energy by ~1.5 W; combined load was not additive (the two loads shared
  cores; seat claim rate halved) and is reported as observed.
- **GPU domain.** GPC reads 0 in every window and is not used. GPM and NVML
  both respond to GPU load but differ by ~1 W under it; they are reported
  side by side as different measurement boundaries, never substituted.
- **Uncertainty stated with every energy result:** 1 mJ resolution; ±1
  sample period (100 ms) of power at each window edge; the observed
  accumulator-versus-power discrepancy of up to 0.5 W; no external meter
  calibration, so absolute accuracy is not claimed. Metric 17's primary use
  is the paired RES-1/SEQ ratio (G9) on the same sensor.
- **Machine noise.** LM Studio and the NVIDIA Personal AI Router were
  resident during this validation; §9's "nothing else heavy runs" applies
  to qualification, and the qualification script records the process list.

## 18. Clarification C3 (2026-09-28, before any qualification data)

Adds recording of the machine's physical state. G1–G16, TARGET_PCT 55, the
workloads, the statistics and every acceptance threshold are unchanged. This
is observability, not a performance exemption.

- **Why.** On 2026-09-28, before a full power-off, the X925 cores could not
  sustain their clock: under sustained load they fell from ~3.89 GHz to
  ~3.0–3.5 GHz while package power stayed around 19.5–21.5 W. The A725 was
  unaffected. A full power-off removed it: the X925 then held 3.891 GHz for
  2 minutes and package power rose to 29–32 W when required. In that state
  the R13 workload's optimized/incumbent ratio is ~0.38 against 0.55. The
  cause of the power-limited state is not established. Diagnostic evidence:
  branch `research/r15-hwchar`, `research/r15-hwchar/RESULT-postboot.md`. It
  is not qualification evidence.
- **Why cycle counters.** The kernel's reported frequency did not show the
  cut (`scaling_cur_freq` stayed at 3.9 GHz). The effective clock is each
  core's cycle counter, read system-wide from outside the measured processes
  (`perf stat -a -A -e cycles -I 1000`), over the core's busy time from
  `/proc/stat`.
- **Recorded every run** (`tools/r15_machine_state.sh`, from
  `tools/r15_qualify.sh`): per-core cycles each second (`machine-perf.csv`);
  each second the per-core busy time, SPBM package energy, thermal zones and
  load average, and every 5 s the GPU SM clock, power, utilization,
  temperature and clock-event reasons (`machine-state.ndjson`); the governor
  and max frequency of every core at start; a mark at the start and end of
  every trial process (`machine-state-marks.txt`); an informative summary.
  The sampler runs pinned to cpu 0 (`R15_STATE_CPU`). All of these files are
  listed in SHA256SUMS. The reducer does not read them.
- **Preflight (silicon only).** Before the first trial, one X925 core runs
  20 s of sustained load while its cycles are counted. If the median
  effective clock over seconds 13–20 is below 3.75 GHz, the run stops before
  any trial and reports a machine-state failure (`preflight.txt`, `done` =
  `machine-state-failure`). Normal is ~3.89 GHz; the power-limited state read
  3.0–3.6 GHz.
- **During the run nothing is stopped, dropped or rerun** because of machine
  state. A recurrence during qualification is visible in the raw evidence and
  is reported with the result. The machine is not power-cycled between or
  during trials to obtain better results.

## 19. Clarification C4 (2026-09-28, before any qualification data)

Corrects a rig defect in how the W-EPISODE decisive goal status is sampled.
G1–G16, TARGET_PCT 55 and every threshold are unchanged. No trial result is
reinterpreted; there are no qualification data yet.

- **Defect.** After promotion, `r15_episode` (tests/runtime/rx_r15_rig.c)
  waited until AIEN's prediction was confirmed for the new epoch on X925 and
  the assessment named that prediction (assessment field 6 = prediction
  field 0), then took the assessment status as decisive. Confirming a
  prediction does not change its sequence (field 0), so an assessment AIEN
  made just before the confirmation also matches. That assessment correctly
  reads UNKNOWN ("no confirmed prediction for the regime yet", `fn_assess` in
  src/runtime/rx_aien.c). AIEN then re-assesses and reads MET, but the rig had
  already recorded UNKNOWN as the decisive status. AIEN behaved correctly;
  the rig sampled an intermediate result.
- **Evidence (pre-qualification practice, not qualification data).**
  Host dry run of the integrated candidate (2026-09-28 18:12 local): RES-4
  and RES-1 round 1 both `goal not MET (status 1)` with goal_status_final
  = 2 (MET), optimized/incumbent ≈ 0.405, episode end ~0.2 s after
  promotion (passing episodes run the ~8 s AFTER window). A 6-trial RES-1
  probe: 1 of 6 with the same signature (status 1, final 2, ratio 0.415,
  0.4 s). Every failing case had AIEN reaching MET moments later.
- **Scope.** Seen in RES-4 and RES-1. SEQ also runs AIEN outside the rig's
  thread, so it is not structurally immune; the window is narrower. The
  defect predates the 2026-09-28 main integration and the power-limited
  machine state. The afternoon practice misses (29 of 48) came partly from
  that machine state and partly from this defect; they cannot now be
  separated and no claim is made about the split.
- **Correction.** The wait additionally requires a decided assessment
  (status ≠ UNKNOWN). Still failing: UNMET or UNMET_EXPLORED, and UNKNOWN when
  the unchanged 60 s limit expires. `met_status`, the AFTER window and the
  limits are unchanged.

## 20. Clarification C5 (2026-09-28, after practice, before qualification)

Records a change to the resident runtime made because practice showed a real
weakness, and declares the host threads the resident configurations now run.
G1–G16, TARGET_PCT 55, the G7 threshold (≥ 90% of barriers with a production
commit), the G7 denominator (every L1-G and per-trial barrier) and the
definition of a qualifying barrier (a production commit whose end lies in
[barrier begin, receipt]) are unchanged. SEQ is unchanged. No practice
result is reinterpreted; practice runs are not qualification data.

- **Finding (practice 20260928T233124Z-6723fa0579a8, 4 rounds).** Per-trial
  barriers: RES-4 had production commits in every barrier (≈1,100 each);
  RES-1 and RES-1-NODIGEST had none in any (0 of 8); SEQ none, as designed.
  A high-resolution host trace of one RES-1 transition (`R15_BARRIER_TRACE`,
  below) showed why: `generation.promote` ran `rx_gen_promote` on the only
  worker for ~84 ms while one production request waited READY. Inside the
  barrier: checks < 0.1 ms; six blobs written and synced 37 ms; root and
  CANDIDATE journal 13 ms; root re-read 0.02 ms; **pointer flip 6.7 ms**;
  FLIPPED journal, receipt file, event log and RECEIPT journal 23.5 ms.
  `generation.prepare` also held the worker ~15–18 ms before the barrier
  (provenance plus the proposal's `nextid` sync). Only the pointer flip (on
  disk) and the in-force publication (in the world) must be observed as one
  step; nothing else in the barrier needs the semantic worker.
- **Change (production runtime, not the harness).**
  - `rx_generation`: a *durable executor*, one thread per store
    (`rx_gen_exec_start`). It runs `rx_gen_propose` or `rx_gen_promote`
    exactly as a caller would, with the caller's request and the caller's
    native-authority callback, one job at a time in post order, then calls
    the poster back. It holds no capability, reads no world object, chooses
    nothing and runs no reaction. Every fsync, the crash points, the lock
    file and the OLD/NEW recovery rules are unchanged.
  - `rx_world`: a reaction function may return `RX_FN_DEFER` after handing
    such an effect to the executor. The activation keeps its admission (its
    slot stays charged, `in_flight` counts it, quiescence waits for it) but
    frees the worker; no crumb is written for the deferring run. The
    executor's completion calls `rx_world_resume`, which puts the same
    activation back on its ready ring (a new lifecycle edge RUNNING → READY,
    used only here). It then runs on an ordinary worker on a fresh snapshot
    and publishes through the usual stale-read check, capability
    re-validation and write-set check. A sequential reference world treats
    `RX_FN_DEFER` as a failure and never starts an executor.
  - `rx_living`: in a resident world `generation.prepare` and
    `generation.promote` decide on the worker, post the store work, defer,
    and publish the executor's result when resumed. The candidate and
    in-force records are still written only by these reactions under their
    own authority; in-force is still published only after R9 returns OK
    (after flip and receipt). SEQ keeps the in-line path unchanged.
- **Threads.** RES-4 / RES-1 / RES-1-NODIGEST: the world's workers (4 / 1
  / 1, the only threads that run reaction functions), the seat completion
  transport, the seat keep-alive lease thread of 03d2820 (`rx-seat-lease`,
  5 ms tick; it moves no semantic state), the R9 durable executor (asleep
  except during a proposal or promotion) and the harness's producer and
  observers. SEQ: its one orchestrator thread (which runs every stage,
  including the store work, to completion), the lease thread and the
  harness threads. All of them are confined to the trial's core class and
  counted by process-wide CPU time, `getrusage(RUSAGE_SELF)` and the
  inherited PMU counters, so the executor's CPU is never hidden.
- **Diagnostic only.** `R15_BARRIER_TRACE=<file>` makes a trial append its
  barrier phases and every crumb around the barrier to that file. It is not
  part of the raw evidence and is never set by `tools/r15_qualify.sh`.
  `RxGenPhases` gains stamps for the inner phases (verified, blobs,
  candidate, reachable, flipped, receipt file, event); the four existing
  stamps and the G7 reduction are unchanged.
- **Regression.** `make test-r15-g7-host` (tests/runtime/rx_r15_g7.c):
  RES-1 has a production request READY and committing while the promotion's
  disk work is under way; production commits inside the barrier and before
  the flip; in-force is published once, by `generation.promote`, after the
  receipt, and names the generation on disk; stops before / during / after
  the flip recover OLD / OLD-or-NEW / NEW, never TORN; only
  `generation.promote` writes the promotion and in-force records; every
  crumb comes from a world worker or the seat; RES-4 unchanged; SEQ has no
  executor, no deferral and still pauses production for the whole barrier.
  With the executor disabled the RES-1 checks fail (0 commits in the
  barrier).

## 21. Amendment A1: G10 counts synchronization events (2026-09-28, after attempt 1)

This is a change to a gated criterion, made after qualification data
existed, so per the status line at the top it is recorded here and the
whole qualification is rerun.

**What changed.** G10 was "CPU↔GPU synchronizations per GPU result, RES-1
≤ SEQ", where the count included host completion polls that found nothing.
G10 is now "CPU↔GPU synchronization EVENTS per GPU result (claim notices,
completion notices, fences, explicit barriers, host blocking waits; empty
polls excluded), RES-1 ≤ SEQ". A new reported, not gated, metric "host spin
cost: empty polls per GPU result" is produced for both RES-1 and SEQ
(`host_spin_empty_polls_per_gpu_result_median` in `m8_cpu_gpu`), so the
spinning stays visible. `tools/r15_reduce.c` implements both.

**Why (attempt-1 data, run 20260929T020536Z-ad8e1f2ea4e4-silicon).** G10
failed, RES-1 80.8 vs SEQ 61.6 per result. Claims and results were fixed at
256 on both sides; all of the difference was `gpu_polls_empty` (RES-1
8.2k–24.8k, SEQ 12.7k–29.4k over 5 runs; the ranges overlap; practice-2 on
the same commit gave 40 vs 104, the other way round). The spin rate varied
about 6× (193–1242 empty polls per ms of GPU wait) with identical clocks
(2.808 / 3.9 GHz), temperatures and flat GPU execution time (73–75 µs on
the chip clock). Profiling showed both configurations spin: RES-1 polls
once per ring scan, SEQ counts one host wait per result and then spin-polls
the same progress routine; it does not block. L2 keeps exactly one claim in
flight, so results per wake = 1 on both sides by construction. The old
count therefore measured how long the host waited times how fast it spun,
not how many times CPU and GPU had to synchronize. Counted as events, the
structure is RES-1 3.0 (claim + completion + fence) vs SEQ 4.0 (the same
plus one host blocking wait) per result. Re-reducing attempt 1 with this
reducer gives G10 3.0 vs 4.0 and host spin 77.8 vs 57.6 empty polls per
result; attempt 1 is not relabelled and stays FAIL (it also had a blank
aienos commit).

**Explicit barriers.** The L2 rig calls `rx_world_wait_quiescent` once per
claim in both configurations to pace the benchmark; it is identical on both
sides (one per result) and is not an engine synchronization, so the
counters do not include it. Counting it would add 1.0 to both sides and
not change the comparison.

**Decided by** Drake (owner), 2026-09-28 22:40 CDT, Option A of three (A:
this amendment; B: A plus a new multi-claim phase; C: keep G10 unchanged,
for which no legitimate code fix exists: slowing the poll loop would be
gaming).

**Stated limit.** With one claim in flight, G10 cannot show the resident
path's intended advantage (one scan covering many completions). A
multi-claim-in-flight L2 phase (N > 1, e.g. 16, Option B) is deferred to a
later gate and is listed in the receipt's limits. Empty-poll spin cost is
a known host-side cost of both paths and is not claimed to be small.
