# ARGUS producer speed round 2 (feat/argus-producer, 2026-09-29)

Base: 20e84b8 (producer v1.1 746b30b + H2 evidence). Result: 265b1c5 + this evidence.
Host: GB10 Spark, X925 = CPUs 5-9,15-19, A725 = 0-4,10-14; gcc 13.3.0; kernel 7.0.0-1019-nvidia;
perf_event_paranoid=1. ARGUS pinned at aienos feat/argus-0 b375dca (header unchanged, not edited).
Every benchmark ran under `flock ~/workspace/.argus-bench.lock`; `.spark-quiet` was absent before each run.
Caveat: another session was compiling (a cc1 process at about 100% CPU) during the final R8 runs.
Interleaving with rotated order spreads that load over all configs, but absolute ms are slightly inflated.

## Bar (fixed before measuring, unchanged)
PASS = mean <= 5% loss on the micro-op median throughput AND mean <= 5% on R8 wall clock. If the 95% CI
straddles 5%, add rounds until the half-width is <= 1%. Reported against the lane's 5% and the ADR 0017 2% target.

## Profile (pre-fix build 20e84b8; tools/argus/r8_perthread.sh, r8_leaf.awk, r8_offcpu.awk)
Q1. Does the consumer share cores with the workload? (perf record -s per-thread counts, 20 interleaved rounds,
raw/perthread-base*.) All threads ran on the X925 cores (the A725 PMU counted 0).
| per run | off | discard | ingest |
|---|---|---|---|
| main thread cycles / instructions | 57.2M / 127M | 61.5M / 148M | 60.6M / 147M |
| main thread CPU time | 15.75 ms | 16.83 ms | 16.61 ms |
| worker cycles (all worker threads) | 27.8M | 26.4M | 28.9M |
| worker migrations | 14.3 | 25.4 | 20.6 |
| consumer cycles / CPU time | - | 4.5M / 1.4 ms | 8.7M / 2.5 ms |
Worker CPU is flat, so the consumer does not steal CPU from the workers. It does disturb scheduling:
worker migrations rise, and so does lock waiting on the critical path (below).

Q2. Where does the main thread (R8's serial critical path) lose time? Samples of main-thread cycles and
instructions, and off-CPU time by blocking site (20 and 15 runs, raw/main-leaf-*, raw/main-offcpu-before-*):
- On-CPU +3.0-3.5M cycles (~0.9 ms). About 1.4M of it is R8's own receipt: binary_digest() SHA-256s
  /proc/self/exe, and the ARGUS build is 70 KB larger (149 KB -> 220 KB): 3.1M -> 4.5M cycles = ~1.1% of R8 wall.
  It is present in emit-only mode too. It is a harness cost and was NOT changed. The rest: mutex atomics
  (+0.7-1.1M), the use hook (+0.1M), kernel (+0.5M).
- Off-CPU +1.1 ms, mostly the main thread waiting for w->mu in rx_world_publish_external
  (4.27 ms off, 5.00 discard, 4.62 ingest; 507 -> 559 waits) and rx_argus_shutdown joining the sleeping
  consumer (+0.12 ms). The capability checks run under w->mu (validate_caps, publish_external). So did the
  park-time ARGUS flush (rx_world.c:1299).
- The 50 posix_spawn()s (9.5 ms) are unchanged (raw/r8-spawn-phase.txt). The growth is in the other ~26 ms.

Q3 (control, same session): emit-only / discard / ingest measured in every R8 run below, not inferred.

Verdict: the remaining cost does not come from lock work or SHA-256 on the workers. It is (1) the consumer
sharing the workload's cores, which shows up as worker migrations and longer w->mu waits on the main thread,
and (2) a fixed ~1.1% harness cost from hashing a larger executable. The in-process SHA-256 chain (ingest
vs discard) is about 0.5-0.8%.

## Fixes (cheapest first, per the ladder)
- b. Flush after unlock (110faaf): the park-time flush now runs outside w->mu, only when one is due, once
  per park, and re-checks work after relocking. Flush-before-transition is unchanged. On its own it gave no
  measurable gain: ingest +5.18% -> +5.04% (200 paired rounds, raw/r8-ab-200*). It is kept as a shorter
  critical section. The main-thread off-CPU rerun (raw/main-offcpu-afterb-*) was too noisy at 15 runs to show a change.
- a. Consumer placement (ec87160), tested after (b) failed. Q1 had first ruled it out on worker CPU, but the
  migration signal re-opened it. Discriminating run, 200 rounds: ingest with the consumer pinned outside R8's
  mask (CPU 2): +3.10% [+2.29, +3.91]; pinned onto an R8 core (CPU 9): +5.08%. So the cost is scheduling on
  shared cores. Shipped as RX_ARGUS_CONSUMER_CPU:
  - auto (default): pin to the online CPUs outside the process affinity mask at start, if any; otherwise unpinned.
  - none: never pin.
  - a CPU list: pin exactly there.
  Failures are reported and never fatal. The summary records consumer_cpu and consumer_pin_rc.
  Deployment requirement: give the consumer a core the workload does not use; a taskset workload gets that automatically.
  Mechanism check (30 rounds, raw/perthread-final*): workers off 18.9M cycles / 1,880 context switches /
  12.5 migrations; auto 22.9M / 2,175 / 13.5; unpinned 30.3M / 4,044 / 20.2. The consumer runs on the A725s.
- c. Batch the SHA-256 chain: not applicable. By the ABI every well-formed event extends the chain
  (argus_abi.h, argus_core.c step 6), and the hash runs inside argus_core_ingest in pinned ARGUS code. The
  consumer's own per-event path is already a copy, a stream write if enabled, and the ingest call. Ingest vs
  discard is ~0.5-0.8%.
- Not done (boundaries): consumer out of process (ADR change); sampling or dropping USED; raising ARGUS_USE_FLUSH_OPS.
  None was needed for 5%.

## Final numbers (build 265b1c5)
R8 wall clock (whole process, taskset 5-9,15-19, rotating interleave, paired per-round difference vs off, t-based 95% CI):
| config | run 1 (200) | replicate (200) | pooled (400) |
|---|---|---|---|
| off mean | 35.10 ms | 35.03 ms | 35.06 ms |
| emit only (RX_ARGUS=1) | +1.71% [+0.90, +2.52] | +2.46% [+1.56, +3.36] | +2.09% [+1.48, +2.69] |
| discard, auto placement | +3.53% [+2.60, +4.46] | - | - |
| **ingest, auto placement (shipped default)** | **+4.06% [+3.22, +4.91]** | **+3.68% [+2.83, +4.54]** | **+3.87% [+3.27, +4.47]** |
| ingest, unpinned (none) | +5.31% [+4.45, +6.17] | +5.31% [+4.54, +6.08] | +5.31% [+4.74, +5.89] |
| discard, unpinned | +4.99% [+4.13, +5.86] | - | - |
| ingest, pre-fix 20e84b8 | +4.93% [+3.87, +5.98] | - | - |
The replicate was decided before its result was seen, and both runs are reported. Every run passed (116 checks, 0 failures).
Rough decomposition of the shipped +3.9%: ~1.1% harness (larger executable hashed by the receipt), ~1% producer
hooks (emit-only minus harness), ~1.3% consumer presence even when off-core (cross-cluster cache traffic on the
ring and publish lines), ~0.5% ingest SHA-256 chain. These are ranges, not exact attributions.

Micro-op (rx_aegis_evaluate + rx_world_validate_cap, 20M ops/run, 30 interleaved rounds, raw/micro-*):
| config | core 7: median ops/s | mean vs 0 [95% CI] | cores 7,8: median | mean vs 0 [95% CI] |
|---|---|---|---|---|
| 0 | 41.47M | - | 41.66M | - |
| 1 emit only | 44.07M | +5.99% [+3.27, +8.72] | 43.26M | +3.62% [+0.17, +7.07] |
| 2 discard (auto) | 43.83M | +4.73% [+2.10, +7.35] | 44.79M | +4.67% [+1.27, +8.08] |
| 2 ingest (auto, shipped) | 44.64M | +5.96% [+3.60, +8.32] | 44.08M | +5.53% [+3.39, +7.67] |
| 2 discard, unpinned | 40.31M | -3.77% [-6.47, -1.06] | - | - |
| 2 ingest, unpinned | 41.31M | -3.51% [-6.00, -1.03] | - | - |
The op p50/p99 is 48/64 ns in every mode; the use update p50 is 2.25 ns. Every ARGUS build, emit-only included,
is 4-6% faster than the RX_ARGUS=0 build. That points to code layout, so this bench cannot resolve costs of a few
percent. In the shipped default the consumer leaves core 7 and there is no loss. Unpinned on one core (consumer
on the measured core) the loss is ~3.5%, with a CI that reaches past 5%. That is diagnostic only, not the shipped config.

## Verdict
- vs the lane's 5% bar: **PASS**. R8 +3.87% (pooled, CI +3.27..+4.47, half-width 0.60%; each run's CI is
  below 5%). Micro-op: no loss.
- vs the ADR 0017 2% target: **FAIL**. R8 +3.9% (CI excludes 2%). Even emit-only is +2.1%, and ~1.1% of R8's
  cost is the receipt hashing a larger binary. 2% cannot be reached on R8 as built; a steady-state R8 variant
  (without per-run fixed costs) is the way to test it. Noted, not built.
- The pass depends on the consumer having a core the workload does not use. Unpinned, R8 is +5.3% (FAIL).

## Correctness (RX_ARGUS=2 ingest, ARGUS_AUTH=observer, the successor of ARGUS_WRAP=1; detectors b375dca)
Streams: ~/workspace/argus-runtime-streams/speed2-run1, speed2-run2 (make test-argus-runtime: no mask, so the
consumer is "auto:unpinned") and speed2-pinned (taskset 5-9,15-19: "auto:0-4,10-14").
- R7: 2 findings, code 13 (known open: a second authority instance restarts at boot generation 2). 320 events. The
  stream is byte-identical in all three runs.
- R8: 0 findings. R9: 0 findings. 0 ring refused, 0 summary refused, 0 late in every run.
- Replay (raw/replay-digests.txt): every stream replayed twice gives the same state digest. R8/R9 live streams
  differ run to run (timing-dependent flush points), as in H2.

## Build checks
- RX_ARGUS undefined: objdump -d of rx_aegis.o, rx_native_bind.o, rx_generation.o, rx_world.o is identical to
  main bba3bd3 (raw/objdump-identity.txt). Default test-r7, test-r8 and test-r9 pass.
- Makefile (0c00c5a): AIENOS_R7_DIR now defaults to $(OUT_DIR)/aienos-authority/c8ab65e, extracted from
  AIENOS_LOCK_REPO at aienos.lock. AIENOS_R7_DIR=../aienos-r9 now stops with "has no native/capability"
  instead of silently building against nothing.

## Notes, not shipped
- Shutdown: rx_argus_shutdown waits out the consumer's nanosleep back-off (~0.1 ms/run). A condvar wake would remove it.
- claim() first-touches a producer slot under w->mu on a thread's first validate. R8's single-round-trip
  "slow path" rises 49 -> 64-71 us in every ARGUS mode; pre-claiming at start would move this off the lock.
- flush_all scans all 256 use slots on every park and on every wake (use_begin_slow). A touched-bitmap or an
  ops-since-flush check would skip empty scans.
- Out-of-process consumer: ADR boundary, not a speed fix.
