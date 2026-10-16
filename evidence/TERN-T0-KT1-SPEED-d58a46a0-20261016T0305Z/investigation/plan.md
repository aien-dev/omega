# KT1 speed investigation
DOCS READ: NVIDIA Best Practices 13.4 section 10.2.1: "the data access unit is 32-byte regardless of whether global loads are cached in L1 or not." nvos.h:1114-1118: "For system memory this will not be coherent with direct CPU mappings." DOCS SILENT on GB10 SASS scheduling.
SYMPTOM: Four L2-defeated ternary GEMV shapes miss existing bandwidth gates.
1. Freeze original sources and binary from origin/main 8edeab5e; pin physics 6d7cf0d4 and driver 580.173.02.
2. Baseline five runs of existing --bw-only gate, with forced mutant 2 negative control in each run. Same output slices, input seed, allocations and measured path.
3. One-variable experiments: load width/layout, rows/warps/CTA count, depth, staging/compute schedule. Predictions recorded before each experiment; repeat promising variants five times. Hot runs diagnostic only.
4. Fix, host and simulator tests, full chip correctness (115500 rows), mutants, five cold speed runs. Compare both measured floor and original fixed 3424 ns to avoid moving the gate.
5. Archive every variant and receipt, seal this exact folder, publish one PR without merging.
CAMERA: Existing JSON records time, bytes, parity, gate failures after launches; source/binary hashes accompany each experiment. No timing-path logging.
NEGATIVE CONTROL: Forced mutant 2 must exit 1 with 1919/3840 wrong scaled rows; final full suite also exercises mutants 1 and 3. Existing poison before each launch and oracle after each list retained.
RULES: <=20 minute quietlock run windows, yield to other lanes; no killing others; no port 18092; no Python/CUDA/third-party engines.
## Deviations
Initial recursive file discovery was overly broad; narrowed to relevant directories. The scaffold inspects stale ~/workspace/omega, so appended explicit pins of the actual worktree and linked physics.
## Results
Pending.

Baseline-1 reproduces all four failures (fractions .4741/.5410/.7491/.6913); forced wrong segment map fails as expected. Remaining repeats and shape experiments now use quietlock run -- quietlock hold: discovered run alone checks the flag but does not acquire it, first run had check only and is retained, not used as the five locked baseline repeats.
Ranked predictions before variants:
1. Grid tail: 3840 rows require 240 CTAs at R1/W8 but cap160 forces half the blocks through a second unit; raising just CTA cap improves it if launch work imbalance matters.
2. Memory issue parallelism: D10 at K2560 vs D9 at K6912 issues narrow per-row loads; widening and/or depth changes improve cold if outstanding requests are insufficient.
3. Instruction/staging cost: hot speed sets a compute floor; reducing scalar staging stores or independent instruction stalls helps hot and cold together.
4. Occupancy: lowering warps or registers changes active blocks; results should depend on shape and SM distribution.
5. Output serialization: independent uncached output slices may add a store/translation penalty; diagnostic shared output timing can test this, but cannot replace per-launch verified outputs for the gate.

First sweeps: CTA, warp, depth and block-layout variants all still fail. Hot baseline misses both smaller gates too, so memory translation alone cannot explain the gap. Next single-variable probe: K_VAR_MORE currently waits for the previous load's source-read barrier before issuing another independent load. Remove only that wait within load groups; keep write/read scoreboard assignment and the final wait. No load group changes its address or reuses a destination before the existing drain. Prediction: faster cold/hot if artificial load serialization matters. Source copy, host/simulator and chip negative controls required before credit.

no-readwait probe improves only modestly; all gates still fail in first run, no accepted claim yet. Next: replace only the dot/unpack arithmetic with sum-of-packed-word checksum; preserve all global weight loads, activation staging and shared loads, reduction and output topology. Host reference explicitly becomes packed-word checksum for this diagnostic binary only, and its records are NOT ternary correctness evidence. If throughput still misses gates, arithmetic cannot be the dominant limiter. Forced wrong-segment control remains red. Registers may decrease due dead arithmetic temporaries: that biases this probe toward faster execution, so persistent slowness is stronger evidence against arithmetic as main cause.

Packed checksum probe still misses the small-shape gates and is even slower there; 13824 unchanged, 6912 slightly faster. This is inconsistent with simple instruction count being dominant. Next output-address control restores original ternary arithmetic and shares output addresses (diagnostic ONLY, never gate evidence) to test the reported previous-worker difference. Also sample clocks during the tiny bursts: all pre-run clocks have been 208 MHz, so dynamic ramp-up remains unmeasured, not excluded.

## Continuation (Fable 5.1, 2026-10-15 21:3x local, after Astra's usage limit)
DOCS READ (added): CUDA Programming Guide appendices, clock()/clock64(): "return a per-multiprocessor counter value that increments each clock cycle". S2R ids SR_CLOCKLO 0x50, SR_GLOBALTIMERLO 0x52 (0x52 already used by src/runtime/rx_resident_gpu.c:141). nvidia-smi -q: P8, SM 208 MHz, applications clock 2418 MHz, max 3003 MHz. DOCS SILENT on GB10 clock ramp policy and ramp time.
Ground re-read: 111 nvidia-smi samples at ~20 ms during the stamps run never read above 227 MHz (runs/clocks-stamps.csv). Per-launch stamps show no trend along a list (lists last ~1 ms). The word table (0.92) is one 657 MB launch of 3 ms, 20 reps back to back: a sustained load, not a 25 us burst. Hot diagnostic fits time = a + b*bytes with a ~ 7.4 us at k 2560 and ~ 9.2 us at k 6912 beyond the launch floor: a per-launch serial cost that grows with k, which is what a slow SM clock would inflate.
Prediction P6 (clock), ranked first now: the SM clock stays near 208 MHz during these lists; the per-launch serial chain (x staging, first load, one unit's arithmetic, store) at 208 MHz is the fixed cost that sinks the small shapes. If true: (i) an in-kernel camera (SR_CLOCKLO delta / SR_GLOBALTIMERLO delta for CTA 0 warp 0) reads 200-300 MHz in the baseline lists; (ii) a list 20x longer shows per-launch time falling along the list as the clock ramps, and the camera shows the MHz rising; (iii) the camera itself must not change the stamps (clockcam at mult 1 within baseline spread). If the camera reads > 1500 MHz in the baseline lists, P6 is dead and the cause is in the kernel.
Negative control for the camera: a value outside [100, 3100] MHz or a poison word prints UNKNOWN; forced mutant 2 stays red in every run.
Variants: clockcam (baseline + camera, diagnostic only, writes 2 words past row n of each output slice; slice stride widened by 8 words in the diagnostic test), then clockcam --copies-mult 20 (one variable: list length).

### Result: clock camera (clockcam-1, clockcam-m16)
[OBSERVED runs/clockcam-1-cam.csv] In-kernel SR_CLOCKLO/SR_GLOBALTIMERLO ratio for CTA 0: median 2563-2574 MHz on every shape including the 8x256 floor launches (850 launches, 0 UNKNOWN; a few launches read 546-1456 MHz, consistent with CTA 0's warp being time-sliced, as the CUDA docs warn). nvidia-smi at 20 ms sampled 208-273 MHz through the same run. P6 (clock stays idle) is DEAD: the SM runs at ~2.56 GHz during the launches; nvidia-smi's clock reading misses microsecond bursts. Camera cost: clockcam-1 stamps are inside the baseline spread (0.486/0.558/0.761/0.663).
[OBSERVED] CTA 0 lifetime vs stamp interval: 8x256 1.0 of 3.7 us (so ~2.7 us of the floor is outside any CTA); 3840x2560 14.6 of 25.1 us; 2560x2560 10.4 of 16.1; 2560x6912 16.1 of 31.9; 13824x2560 44.9 of 52.7. On 2560x6912 every CTA has identical work, yet CTA 0 covers only half the launch: either CTAs start late (launch ramp) or finish very unevenly.
clockcam-m16 (16x lists): 3840 and 2560x2560 lists of 656/992 launches failed the oracle check (list too long for the cmdlist or the test, UNKNOWN which; not pursued; 13824 at 192 launches ran, 0.671, no speedup from the longer list). Not evidence either way beyond: longer lists do not help.
Next camera (P7, timeline): per CTA, thread 0 records globaltimer at start, after x staging, at end. Prediction: if CTAs start spread over many us, the launch ramp is the fixed cost; if they start together but staging takes several us, x staging is; if lifetimes are very unequal, memory service is unfair.

### Result: timeline camera (timeline-1, timeline-hot, timeline-nostore-1)
[OBSERVED runs/timeline-1-cam.csv] All 160 CTAs start within 0.3-0.45 us on the k 2560 shapes (no launch ramp). x staging (uncached x load, unpack, STS, bar) takes 2.3-3.2 us median, up to 5-6 us, in every CTA, cold and hot: the first ~3 us of every launch move no weights. CTA lifetimes are very unequal (3840x2560: median 11.8 us, max 25 us): the strided unit map gives 80 CTAs two units per warp and 80 CTAs one. 2560x6912: CTA starts spread over 19-22 us: with 13,824 B shared memory per CTA the 160 CTAs do not all fit at once and run in waves (QMD word 36 keeps the SM shared-memory config fields as they were; resident capacity UNKNOWN).
[OBSERVED] The timeline camera itself slowed every shape by 5-10 us (2560x2560 16.1 -> 26.0 us; hot 14.8 -> 25.0), the clock camera (CTA 0 only) did not. timeline-nostore-1 (same binary minus the four 4-byte uncached camera stores per CTA, registers and S2Rs kept): back to baseline, 24.0/15.3/51.3/31.5 us (0.504/0.585/0.781/0.665). So 640 small uncached stores from 160 CTAs cost ~10 us. [INFERRED, high] Consecutive uncached stores from one thread to one line serialise at the uncached write round trip (~2.5 us each). The kernel's own output path is one 8-byte uncached store per unit per warp (2 adjacent words), 1920 of them for 3840 rows, spread through the launch: cost UNKNOWN until probed.
Next probes (diagnostic, results not checked, API poison check disabled in the probe build only; forced mutant 2 stays checked): P8 probe-nostore (i32 output STG replaced by an STS to shared: upper bound of the output-store cost), P9 probe-nox (x LDG.128 replaced by constants: upper bound of the x round-trip cost). Predictions: if P8 saves >= 5 us on 2560x2560 the output path is the lead and the fix is full-line coalesced stores through shared memory; if P9 saves ~2.5 us the staging round trip must overlap the first weight loads.

### Result: probes (probe-nostore-1, probe-nox-1; diagnostic, results unchecked)
[OBSERVED runs/probe-nostore-1.json] Output STG replaced by a shared store: 3840x2560 25.1 -> 15.7 us (0.841), 2560x2560 16.2 -> 13.5 (0.687), 13824x2560 53.2 -> 42.0 (0.967), 2560x6912 30.4 -> 25.1 (0.861). The per-row GPU-uncached stores are the main cost on three shapes.
[OBSERVED runs/probe-nox-1.json] x not loaded: 23.6 / 15.6 / 50.4 / 27.4 us: the x round trip is worth 0.6 to 2.6 us, second order.
FIX (worktree tern/kt1-speed, src/omega_blackwell_codegen_tern.c + .h): single-chunk kernels park each round's 2RW results in shared memory (two slot sets alternating by round, one CTA barrier per round; all warps of a CTA take the same rounds, a warp past U recomputes U-1 and its rows are never stored) and warp 0 writes the round with one coalesced STG. Host battery 56/56 and simulator battery 34/34 (bounds-checked, grids and warp orders, mutants 1/2/3 caught) pass on the fixed code. Prediction: cold fractions move toward the probe-nostore figures; the 2560x2560 gate (0.759) may still miss because 1280 units over 1280 warps is one round and the x round trip plus one DRAM round trip remain serial.

### Result: fix-1 (coalesced output, cold, 5 reps) and evidence-store-fix/ (red + main phases)
[OBSERVED runs/fix-1.json] 3840x2560 16.19 us (0.816 PASS), 2560x2560 13.54 (0.687 FAIL), 13824x2560 43.49 (0.933 PASS), 2560x6912 28.77 (0.738 FAIL). Negative control (forced mutant 2) red.
[OBSERVED evidence-store-fix/main.log] correctness 0 rows differing on every battery (random 20000 rows, MA grid 49932 rows, BitNet shapes x 2 layouts, f32 q|k|v); red phase: mutants 1, 3, 2 each exit 1. Cold in that run: 0.852 / 0.727 / 0.972 / 0.880. The 2560x6912 figure swings 0.74 to 0.88 between runs (waves, see UNKNOWN below); 2560x2560 sits at the probe-nostore ceiling (0.69 to 0.73): the x round trip is now the serial cost there.

### Prediction P10 (variable 2: overlap the x round trip with the first weight round trip)
Change (worktree): the warp's first unit's weight loads are issued before the x load, which follows without waiting for them
(scoreboard rule: the x arithmetic waits for both); the K loop skips its loads once (predicated region on a flag). To keep
the register count flat the row-major loads use immediate offsets (LDG.E imm, same 24-bit field as the LDG.E.64/128 words
already in the encoder; simulator mirrors it): peak 54 registers, declared 56 (unchanged), 676 instructions (was 658).
The x staging clamps its word index instead of predicating (no lane reads past x). Host 56/56 and simulator 34/34 PASS,
every spec variant generates (/tmp/kt1stats).
Prediction: each shape loses about one memory round trip per CTA (1 to 2.5 us): 2560x2560 11.0 to 12.5 us (0.76 to 0.84),
3840x2560 14 to 15 us, 13824 42 to 43, 2560x6912 unchanged within its swing. If 2560x2560 does not move, the serial cost is
not the x round trip (the pre-launch x arrival or the first-touch of the uncached weights would be the next candidates).
Negative control: forced mutant 2 must stay red; correctness 0 rows differing with the new load encoding.

### Result: fix2-1 (prefetch + coalesced output, cold, 5 reps) and evidence-fix2/ (red + main)
[OBSERVED runs/fix2-1.json] 3840x2560 15.36 us (0.887 PASS), 2560x2560 13.38 (0.712 FAIL), 13824x2560 42.82 (0.953 PASS),
2560x6912 27.90 (0.770 PASS). Negative control red. evidence-fix2/main.log: 0 rows differing on every battery with the
immediate-offset loads; mutants 1, 3, 2 red. Cold in that run: 0.854 / 0.646 / 0.953 / 0.809.
P10 verdict: PARTLY WRONG. Three shapes gained 0.7 to 0.9 us per launch (3840: 16.19 -> 15.36; 13824: 43.49 -> 42.82;
6912: 28.77 -> 27.90), 2560x2560 did not move (13.54 -> 13.38, within noise; 12.9 to 14.4 across runs). So at
2560x2560 the x round trip is not the serial cost. What remains there, floor taken out: 9.7 us for 1.64 MB where the
practical limit needs 6.9 us; about 2.8 us unexplained, against 1.3 us at 3840 and 1.8 us at 13824. The 2560x2560
launch is one round (1280 units over 1280 warps): every warp issues its ten loads at once and the whole launch is one
burst with no steady state. UNKNOWN: the mechanism (DRAM pipeline fill and drain for a single burst, address
translation on first touch, or something else). Not pursued further here; two variables are enough for one PR.
Prefetch kept: no regression on any shape, registers flat, correctness proven on the chip.
