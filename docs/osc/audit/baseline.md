> Appendix to OMEGA_SYSTEMS_CORE_CODE_AUDIT.md (I.10). The counting shim and aggregation script are committed in docs/osc/audit/baseline/ (the job-tmp paths below no longer exist). Reference snapshots are regenerable by SHA: `git -C <aienos checkout> archive 8706fb8 | tar -x -C <dir>` and `git -C <physics checkout> archive fecbedb | tar -x -C <dir>`. Worker report (Sonnet 5.5 trial), omega 193a7e7 built in an exported copy with PHYSICS_LOCK_CHECK=0 against the physics fecbedb / aienos 8706fb8 snapshots; machine not quiet. Orchestrator re-check: sizes of omegatool, rx_heartbeat_test, rx_state_projection_test, rx_typed_results_test match exactly; heartbeat peak RSS 5456 kB vs 5568 reported; heartbeat allocations 399 vs 396 reported. Note added by reviewer: allocation counts are SUMMED over every process a test starts (heartbeat runs 2); the report does not say so. Build times not re-measured. rx_plan_reuse_test uses a perf-event ioctl (CPU counter, not GPU), so treat its rows as informational.

# Omega C baseline (commit 193a7e7 export), host-only, CPU-side

Machine: aarch64, 20 cores, NOT quiet (other sessions share it). `test -e /home/drakestapleton/workspace/.spark-quiet` was checked before every build/run loop: never present.
uptime at start: `06:43:47 load average: 1.08, 0.95, 0.72`; runs start `06:46:55 load 2.33, 1.62, 1.02`; runs end `07:14:23 load 2.48, 1.92, 1.58`; final `07:14:29 load 2.18, 1.87, 1.56`.
Compiler: `cc` = gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0 (`cc --version`). Flags (Makefile CFLAGS): `-std=gnu11 -Wall -Wextra -Werror -MMD -MP -D_GNU_SOURCE -O2 -Isrc` + physics include dirs + `-DOMEGA_PHYSICS_DIR=...`; test binaries add `-pthread`, most link `-lm` and libaienos_capability.a. No LTO/-march/strip. Binaries are dynamically linked, not stripped (`file build/rx_action_graph_test`).

## Provenance caveats (read first)
- The working copy has no `../physics` or `../aienos-r9`. I built with `PHYSICS_DIR=<scratch>/physics PHYSICS_LOCK_CHECK=0 AIENOS_R7_DIR=<scratch>/aienos` (script: `bench-scratch/mk.sh`). physics = copy of tmp `physics-snap`; aienos = copy of tmp `aienos-snap/native/capability`. Neither was verified against physics.lock (`fecbedb1...`) / aienos.lock (`4c21386...`) (git not runnable there). Physics only affects `omegatool` (m16_native.c, nvrm.c); the test binaries here do not link it. The aienos capability lib was built once beforehand (`make` in native/capability: 0.113 s real) and is EXCLUDED from build times.
- `make -j4` was used, but each test binary is one `cc` invocation, so -j4 only helps `all` (39 objects).
- Timing via `/usr/bin/time -v` (exists at /usr/bin/time); wall clock has 10 ms resolution. 5 runs each; median / min / max.
- Tests write receipts under `omega-bench/build/qual-runs/` (inside the working copy only).
- `rx_plan_reuse_test` was run before I noticed the ioctl rule: its source `tests/runtime/rx_plan_reuse.c` contains `ioctl(g_perf, PERF_EVENT_IOC_ENABLE, 0)` (line 88) and `#include <sys/ioctl.h>`, a Linux perf-event CPU counter, NOT /dev/nvidia and no GPU. Under the strict ioctl rule it should count as "skipped", so its rows are INFORMATIONAL; raw data in `bench-scratch/runs/rx_plan_reuse_test.*`. A grep for `/dev/nvidia|ioctl|nvrm` over the other test sources, `src/runtime`, `src/crumbline`, sha256/omega_evidence/omega_core/omega_canonical, rx_cog_engines.[ch], rx_sp_workloads.h found nothing.

## 1. Clean build wall time (rm -rf build before each; 5 reps; seconds; `bench-scratch/buildtime.sh`, output `buildtimes.txt`)
Per rep: `rm -rf build; ./mk.sh <target>` (= `make -j4 <target> PHYSICS_DIR=... PHYSICS_LOCK_CHECK=0 AIENOS_R7_DIR=...`)
| target | 5 reps | median | min/max |
|---|---|---|---|
| all (omegatool, 39 objs) | 1.228 1.189 1.201 1.220 1.164 | 1.201 | 1.164 / 1.228 |
| rx_heartbeat_test (test-r3) | 1.837 1.851 1.926 1.881 1.853 | 1.853 | 1.837 / 1.926 |
| rx_action_graph_test | 2.104 2.081 2.083 2.017 1.975 | 2.081 | 1.975 / 2.104 |
| rx_state_projection_test | 2.297 2.256 2.277 2.241 2.263 | 2.263 | 2.241 / 2.297 |
| rx_plan_reuse_test | 1.975 2.065 2.028 2.063 2.066 | 2.063 | 1.975 / 2.066 |
| rx_sem_incremental_test | 1.478 1.473 1.446 1.493 1.426 | 1.473 | 1.426 / 1.493 |
| crumbline-learner | 0.341 0.356 0.344 0.356 0.369 | 0.356 | 0.341 / 0.369 |
| rx_capability_query_test | 2.097 2.164 2.126 2.115 2.130 | 2.126 | 2.097 / 2.164 |
| rx_semantic_comm_test | 1.563 1.503 1.454 1.448 1.554 | 1.503 | 1.448 / 1.563 |
| rx_cognitive_routing_test | 1.089 1.052 1.075 1.094 1.080 | 1.080 | 1.052 / 1.094 |
| rx_workflow_fusion_test | 2.407 2.423 2.381 2.434 2.407 | 2.407 | 2.381 / 2.434 |
| rx_typed_results_test | 2.155 2.260 2.162 2.107 2.189 | 2.162 | 2.107 / 2.260 |

## 2. Binary size (`size <bin>`; `stat -c%s`; `bench-scratch/sizes.sh`, `sizes.txt`)
| binary | text | data | bss | file bytes |
|---|---|---|---|---|
| omegatool | 329194 | 2512 | 35680 | 431472 |
| rx_heartbeat_test | 188775 | 1336 | 8104 | 215704 |
| rx_action_graph_test | 180269 | 1544 | 321984 | 219720 |
| rx_state_projection_test | 148603 | 1452 | 19488 | 217344 |
| rx_plan_reuse_test | 194430 | 1712 | 431168 | 287224 |
| rx_sem_incremental_test | 137871 | 1664 | 37432 | 214968 |
| crumbline-learner | 80235 | 1312 | 1048600 | 148888 |
| rx_capability_query_test | 194591 | 1680 | 358296 | 286752 |
| rx_semantic_comm_test | 129472 | 1416 | 6552 | 217440 |
| rx_cognitive_routing_test | 114475 | 1384 | 456576 | 148680 |
| rx_workflow_fusion_test | 230404 | 1856 | 14903376 | 291304 |
| rx_typed_results_test | 202185 | 1744 | 3249584 | 286136 |
Large bss (fusion 14.9 MB, typed_results 3.2 MB, crumbline 1.0 MB) is zero-fill static arrays; it counts toward RSS only when touched.

## 3. Wall time and peak RSS (5 runs, `/usr/bin/time -v -o F <bin> >out 2>err`; `bench-scratch/runall.sh`, aggregated by `agg.sh` -> `agg.txt`; raw in `runs/`)
Format median / min / max. All 55 runs exit 0 (`runs/rc.txt`); each test printed PASS (`tail runs/*.out.1`). Args: `rx_typed_results_test 400`; crumbline = `build/crumbline-learner --decode tests/crumbline/vectors/v001.crb` (ONE decode, not the `test-crumbline` conformance sweep, which was not run).
| binary | wall s | peak RSS kB | user+sys s |
|---|---|---|---|
| rx_heartbeat_test | 1.73 / 1.72 / 1.76 | 5568 / 5272 / 6040 | 0.23 / 0.22 / 0.24 |
| rx_action_graph_test | 0.27 / 0.26 / 0.27 | 4212 / 4092 / 4456 | 0.40 / 0.40 / 0.41 |
| rx_state_projection_test | 19.85 / 19.78 / 19.88 | 120480 / 120224 / 121000 | 19.88 / 19.81 / 19.92 |
| rx_plan_reuse_test (ioctl note) | 11.96 / 11.93 / 11.98 | 5584 / 5552 / 5660 | 8.99 / 8.97 / 8.99 |
| rx_sem_incremental_test | 151.7 / 151.65 / 151.87 | 28580 / 28436 / 28592 | 83.78 / 83.76 / 83.87 |
| rx_capability_query_test | 0.50 / 0.50 / 0.51 | 119628 / 119520 / 119636 | 0.49 / 0.49 / 0.51 |
| rx_semantic_comm_test | 56.67 / 56.57 / 57.10 | 123512 / 123436 / 123612 | 55.98 / 55.77 / 56.35 |
| rx_cognitive_routing_test | 20.08 / 19.98 / 20.20 | 7192 / 7192 / 7192 | 19.16 / 19.11 / 19.17 |
| rx_workflow_fusion_test | 10.99 / 10.86 / 11.00 | 88376 / 78240 / 94980 | 3.82 / 3.61 / 3.87 |
| rx_typed_results_test | 0.61 / 0.56 / 1.20 | 21284 / 21028 / 21508 | 0.39 / 0.37 / 0.52 |
| crumbline-learner --decode v001 | <0.01 (below 10 ms resolution) | 1328 / 1320 / 1460 | <0.01 |
Several tests contain deliberate sleeps/timed windows (heartbeat, sem_incremental, fusion), so wall >> CPU there; some are multithreaded (action_graph CPU > wall).

## 4/5. Allocation accounting (LD_PRELOAD shim `bench-scratch/shim.c` -> `shim.so`; one run per binary: `LD_PRELOAD=shim.so /usr/bin/time -v <bin>`)
The shim wraps malloc/calloc/realloc/free/memalign/posix_memalign/aligned_alloc via glibc `__libc_*`, tracks live bytes with `malloc_usable_size` (usable size: allocator rounding included, chunk headers not), and prints totals from a destructor. `free(NULL)` is counted separately and excluded from "free". The shim also loaded into `/usr/bin/time` itself, so each .shimerr has a second 3-malloc report; ignored (first group is the test). All binaries exit normally, so the destructor ran for all.
`allocs` = malloc + calloc + memalign (new blocks); `realloc` is listed separately. **Leak figure = live blocks at exit** (handles realloc(NULL,n) and realloc(p,0)). `allocs - frees` is unreliable with heavy realloc (negative for state_projection, semantic_comm) so is not used. Peak = maximum simultaneous live usable bytes (allocated, not necessarily touched, so it can exceed RSS, e.g. sem_incremental 194 MB peak vs 28 MB RSS).
| binary | malloc | calloc | realloc | memalign | free | free(NULL) | live blocks at exit (bytes) | peak live usable bytes | total requested bytes |
|---|---|---|---|---|---|---|---|---|---|
| rx_heartbeat_test | 21 | 301 | 3 | 74 | 386 | 23565 | 13 (7640) | 1575920 | 25134634 |
| rx_action_graph_test | 97 | 330 | 3 | 18 | 435 | 5619 | 13 (7704) | 563272 | 8337616 |
| rx_state_projection_test | 49973 | 33037 | 66772 | 16 | 105057 | 6458 | 13 (7656) | 117807416 | 8300594728 |
| rx_plan_reuse_test (ioctl note) | 190055 | 9357 | 4 | 283 | 199683 | 84545 | 15 (2596344) | 3553560 | 3829810447 |
| rx_sem_incremental_test | 479 | 369 | 3 | 92 | 933 | 25104 | 10 (6800) | 194344120 | 17879567066 |
| rx_capability_query_test | 19853 | 3833 | 80 | 1 | 23699 | 280 | 11 (7112) | 158911752 | 399263460 |
| rx_semantic_comm_test | 28326 | 124355 | 255535 | 13685 | 378767 | 7446606 | 14 (302424) | 115351552 | 231240883578 |
| rx_cognitive_routing_test | 1968 | 20 | 3 | 0 | 1977 | 199 | 14 (94944) | 5659632 | 12898804 |
| rx_workflow_fusion_test | 237 | 531 | 3 | 33 | 791 | 10137 | 13 (7704) | 1287200 | 14254883 |
| rx_typed_results_test | 19914 | 17 | 3 | 1 | 19922 | 258 | 13 (7704) | 496408 | 511398698 |
| crumbline-learner --decode | 3 | 1 | 0 | 0 | 3 | 0 | 1 (4104) | 12336 | 16896 |

A floor of ~10-13 live blocks / ~7 KB at exit appears in nearly all tests; I did not attribute it (likely libc/stdio/thread internals; not investigated), so it is NOT proof of application leaks. Larger residuals worth a look, not confirmed leaks (may be deliberately retained until exit): plan_reuse 15 blocks / 2.6 MB (mallinfo2 hblkhd=2588672, one mmapped block), semantic_comm 302 KB, cognitive_routing 95 KB.

Fragmentation (mallinfo2 at exit, main arena only; `malloc_stats()` NOT added, so other thread arenas are not covered). arena / in-use (uordblks) / free (fordblks) bytes:
heartbeat 720896/18720/702176; action_graph 679936/44592/635344; state_projection 16453632/90192/16363440; plan_reuse 172032/17936/154096 (+hblkhd 2588672); sem_incremental 18305024/16320/18288704; capability_query 30105600/58352/30047248; semantic_comm 23973888/160944/23812944; cognitive_routing 5799936/101472/5698464; workflow_fusion 1056768/53824/1002944; typed_results 704512/20032/684480; crumbline 135168/5248/129920.
keepcost (trimmable top chunk) shows most free space is contiguous in several (sem_incremental keepcost 18254784 of fordblks 18288704); state_projection keepcost is 134912 of 16363440 free, i.e. about 16 MB of free but non-returnable fragments at exit. This is exit-state only, not peak fragmentation.

## 6. Throughput / latency numbers the existing tests already print (run 1 of 5, `runs/<name>.out.1`; they vary run to run; found with `grep -HiE 'ns/|us/|ms/|per second|throughput|latency|speedup|p99|p50' runs/*.out.1`)
- rx_action_graph_test: `median run: 1 worker 12.27 ms, 4 workers 3.25 ms, speedup 3.78x (critical path 3000 us of 12000 us work)`
- rx_capability_query_test: `100 capabilities: query median 848 ns (compile 736 + lookup 96) p99 1200 ns`; `1000: median 880 ns p99 3136 ns`; `10000: median 912 ns p99 15088 ns`; `100000: median 1440 ns p99 2849 ns; full scan median 148497 ns`
- rx_typed_results_test 400: `constrained: ... p50 28496 ns`; `free: ... p50 18624 ns`
- rx_cognitive_routing_test: e.g. `engine 11 class 2: accuracy 0.8286 ece 0.0149 mean 16 ns p99 320 ns 223 nJ/call` ... `engine 23 class 6: ... mean 28026 ns p99 66544 ns 329031 nJ/call` (test's own figures; not re-validated)
- rx_sem_incremental_test: `FULL query 1729.3 ms runs 3900 p50 2216.0 us p99 2234.7 us`; `OBJECT query 37.9 ms runs 647 p50 8.5 us p99 2227.3 us`; `FIELD query 36.0 ms runs 647 p50 8.5 us`; `ENGINE query 33.6 ms runs 401 p50 6.8 us p99 2061.2 us`; it also prints package/CPU energy in joules (e.g. `saved median 66.931 J`), not interpreted here.
- rx_plan_reuse_test (ioctl note): `merge_two_towers_3_3: ops 26.6x (G1 PASS), cpu 28.4x ... end to end 1.05x (G6 PASS)`
- rx_workflow_fusion_test: `hybrid: median 279.7 -> 81.7 us, CPU 317.7 -> 194.4 us/run`; `parallel: median 1672.2 -> 3142.2 us`; `compiled: median 483.9 -> 587.5 us` (its energy line was cut off in my grep view and is not copied).
Other tests print only pass/fail counts.

## 7. NOT MEASURED (gaps)
- Any GPU / accelerator workload: `omegatool` `--run-gates` / `--demonstrate-arithmetic`, `make test`, `test-m5`..`test-m19` (omegatool links nvrm.c and m16_native.c; built but never executed, so only its size and build time are recorded). All `*-silicon`, r12, r13, r14, r15 (instr/parity/g7), m17 targets.
- r7 (needs AIENOS native authority), r8, r9, r10, r11 (r11 known flaky under load), branch-reuse (`rx_branch_reuse`), visor targets in `mk/*.mk`, the `test-crumbline` conformance sweep (only one `--decode` measured): not run.
- Peak/steady-state fragmentation, per-thread arena stats (`malloc_stats()`), alternate allocators: not measured.
- Verification of physics/aienos snapshot versions against lock files: not done.
- Quiet-machine numbers: not obtained (shared machine, load 1-2.5 throughout).
- Per-object / link-only build time breakdown: not measured.
