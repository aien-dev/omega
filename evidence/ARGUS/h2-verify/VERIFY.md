# ARGUS producer v1.1 (746b30b) -- H2 verification, 2026-09-29

Candidate: omega feat/argus-producer 746b30b (producer), 71180e2 (argus.lock -> aienos feat/argus-0 b375dca).
Host: GB10 (Spark), gcc 13.3.0, kernel 7.0.0-1019-nvidia. X925 cores = 5-9,15-19. All benches ran under
`flock ~/workspace/.argus-bench.lock`; no .spark-quiet flag present.

## Pins
- ARGUS: aienos feat/argus-0 b375dca (local clone, not yet on origin). native/argus/argus_abi.h sha256 prefix 5ff168ea,
  byte-identical at 398cfb9, 03b9f06, be2814d, b375dca.
- Authority: aienos.lock c8ab65e + tools/argus/aienos-cap-observer-c8ab65e.patch
  (sha256 baf8f9d867840c297efbdfdb0abb6460709df77c78f1288e452826a769eed9fb; `patch --dry-run` clean on c8ab65e).
  The patch backports the observer of feat/capability-observer 12add16. 12add16 itself was not adopted because it sits
  on 64-bit generations and Omega's pinned authority c8ab65e is 32-bit.

## Default build unchanged
RX_ARGUS undefined: objdump -d of rx_aegis.o, rx_native_bind.o, rx_generation.o, rx_world.o is identical to main bba3bd3.
Default test-r7/r8/r9 and RX_ARGUS=0 builds pass (R8 checks 116 failures 0). RX_ARGUS=1 and =2 build with -Werror.

## Correctness (RX_ARGUS=2, ingest, ARGUS_AUTH=observer = successor of ARGUS_WRAP=1; detectors b375dca)
| suite | events | findings | notes |
|---|---|---|---|
| R7 | 320 | 2 (code 13 AUTHORITY_REPLAY) | harness: two authority instances in one process (see below) |
| R8 | 1,989 / 2,393 | 0 | 205,082 uses -> 1,463 summaries; 0 refused, 0 lost, 0 late |
| R9 | 37 | 0 | the old code-9 findings (three stores) are gone: store id on WORLD_COMMITTED |

Determinism: every stream replayed twice gives the same state digest (raw/replay-digests.txt). R7 is byte-identical
across two live runs. R8/R9 live streams differ run to run (the flush points of multi-threaded use tables depend on
timing), and each is deterministic on replay.
R7 code 13: corpus() does pair_stop + pair_start (rx_r7_native.c:204-205), so a second authority instance begins at
boot generation 2 for every slot. Cap 3 already reached generation 2 in the first instance (reclaim; seq 7) and was
revoked (seq 20). The new instance grants 3/2 (seq 28) and revokes it (seq 42). ARGUS keys capabilities by
(machine, cap_id, generation), so it sees a replay. R7 exposes this through two authority instances in one test
process, but the gap is ABI-level and not only in the harness: aienos_cap_restart also rebases every slot to a fresh
boot generation, which can reissue a (cap_id, generation) that ARGUS has already seen. The producer cannot fix it
under v1.1. Fix: capability identity needs an authority-instance (boot generation) component (ABI v2 field, or the
producer opening a new capability namespace per authority start/restart). Status: OPEN, root-caused.
Lane H's earlier R7 = 0 was measured against ARGUS 4cd73cc, whose core has no code-13 detector (added in 57a7bea).
So 0 -> 2 comes from a stricter detector, not a producer regression.

RUNTIME_INTEGRATION: mechanics PASS (builds, streams, 0 refused/lost/late, replay deterministic, R8 0, R9 0);
R7 = 2 findings OPEN (above). Not an unqualified PASS.

## Performance
Criterion, fixed before measuring: PASS iff the RX_ARGUS=2 micro-op median throughput is >= 95% of RX_ARGUS=0,
AND the RX_ARGUS=2 R8 mean wall clock is <= 105% of RX_ARGUS=0 (>= 10 interleaved runs).

Micro-op = rx_aegis_evaluate + rx_world_validate_cap, 20M ops/run, interleaved rounds (raw/micro-*.jsonl):
| cfg | core 7 only, n=22: median ops/s | vs 0 | mean vs 0 | cores 7,8, n=5: median | vs 0 | op p50/p99 |
|---|---|---|---|---|---|---|
| RX_ARGUS=0 | 41.57M | - | - | 41.46M | - | 48/64 ns |
| RX_ARGUS=1 (no consumer) | 43.75M | +5.2% | +5.0% | 46.02M | +11% | 48/64 ns |
| RX_ARGUS=2 discard | 38.92M | -6.4% | -3.1% | 42.02M | +1.4% | 48/64 ns |
| RX_ARGUS=2 ingest | 40.34M | -3.0% | -3.3% | 46.03M | +11% | 48/64 ns |
Emit cost alone (use-table update of a successful validate): p50 2.25 ns, p99 2.62-3.25 ns.
Reading: the single-core cost of RX_ARGUS=2 is about -3% mean in both consumer modes. Medians are -6.4%/-3.0%
(discard/ingest). Run-to-run spread (about +-7%) exceeds the 5% threshold, and RX_ARGUS=1 came out faster than
baseline, so this sub-criterion is not cleanly resolved at n=22. Ingest (the deployed mode, used for R8 and
correctness) passes on median. Both modes are reported. On one core the consumer thread shares the measured core;
with a second core there is no measurable loss.

R8 wall clock (whole test process, taskset 5-9,15-19; all runs passed):
| run | cfg 0 mean (sd) | cfg 2 ingest mean (sd) | change |
|---|---|---|---|
| r8wall.jsonl, 30 interleaved rounds, ms timer | 36.00 ms (1.39) | 38.23 ms (1.45) | +6.2% (95% CI about +4% to +8%) |
| r8decomp.txt, 20 interleaved rounds, us timer | 37.43 ms (1.71) | 39.73 ms (1.23) | +6.1% |
Decomposition (r8decomp): RX_ARGUS=1 37.62 ms (+0.5%), RX_ARGUS=2 discard 38.72 ms (+3.4%), ingest +6.1%.
CPU time 42.2 -> 48.0 ms. 746b30b reported +3.6%. That is within noise of these runs; both straddle the 5% line.
This measurement's point estimate is above it.

R8 use representation (RX_ARGUS=2 ingest): 205,082 successful uses -> 1,463 CAPABILITY_USE_SUMMARY events; 100%
represented, 0 summary_refused, 0 ring_refused (the first producer, 527a721, refused 184k of 204k USED). The 419
full CAPABILITY_USED events are failed validates, emitted in full by design.

VERDICT: micro-op sub-criterion met in ingest mode (not cleanly resolved, see above); R8 wall clock +6.1-6.2% > 5%
-> ARGUS_PERFORMANCE = FAIL.

Where the remaining cost is:
- Not the producer hooks: RX_ARGUS=1 costs +0.5% wall, ~2 ns/use.
- Not fixed start/stop: bench_rx_argus with 1,000 ops, ARGUS off vs ingest consumer: 4.707 ms vs 4.707 ms
  (20 interleaved rounds; raw/fixed-cost-bench1000.txt).
- So it arises while the workload runs, only when a consumer exists: discard +3.4%, ingest +6.1%.
- Leading hypothesis (unprofiled; perf is blocked by perf_event_paranoid=4): rx_world.c:1299 calls rx_argus_idle()
  under the world work mutex before pthread_cond_wait. With a consumer, the park-time flush pushes summaries into
  rings the consumer is reading (cache-line traffic), inside a lock that submitters need.
- Ingest adds SHA-256 chain work (~1 us/event) on a consumer thread that competes with the workload.

Next iteration: (1) move the park flush out from under w->mu (publish idle under the lock, flush after unlock, or
leave it to the consumer's 10 ms flush request). (2) Take ingest out of the runtime process: the consumer only
copies records, and the SHA-256 chain + core run in the ARGUS process (ARGUS-1). (3) Profile R8 with perf (needs
perf_event_paranoid lowered, which is Drake's call) and add a steady-state R8 variant. Then re-measure against the
same criterion.
