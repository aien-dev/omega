# KT1 speed: why the ternary GEMV missed its bandwidth gates, and what the fix moved

Follow-up to `../TERN-T0-KT1-18185404-20261016T0151Z/` (KT1 correct, 0 of 4 shapes at the cold bandwidth gate).
Investigation folder: `investigation/` (the plan with every prediction and verdict, the run scripts, every run
receipt; method `~/.claude/skills/investigate`, one variable per run, negative control before every speed run).

Pins: omega d58a46a0 (branch tern/kt1-speed, clean: `device.txt`), physics 6d7cf0d4, driver 580.173.02, GB10. Every chip
run held the quiet flag (`quietlock hold --owner kt1-speed-fable`, windows of 6 to 10 minutes).
Script: `tools/run_gpu_tern_chip.sh base|red|main <this dir>`, then five `gpu_tern_test --bw-only --reps 5` runs
(`receipt-cold-{1..5}.json`). Speeds are fractions of the physical limit (273 GB/s spec, 237.2 GB/s practical).

## Cause, with evidence

1. **Per-row 4-byte stores to GPU-uncached memory** (the main cost). A probe that parked the results in shared memory
   instead of storing them (`investigation/probe-nostore-1.json`, results unchecked, diagnostic only) cut per-launch
   time 25.1 -> 15.7 us at 3840x2560, 53.2 -> 42.0 at 13824x2560, 30.4 -> 25.1 at 2560x6912, 16.2 -> 13.5 at 2560x2560.
   The lead came from a per-CTA timeline camera whose own 640 stamp stores slowed launches by 5 to 10 us
   (`timeline-1` against `timeline-nostore-1`).
2. **The x round trip ran before any weight moved**: x staging took 2.3 to 3.2 us of every CTA (timeline camera), and a
   probe that skipped the x load (`probe-nox-1.json`) saved 0.6 to 2.6 us.
3. **Not the GPU clock.** An in-kernel camera (SR_CLOCKLO against the global timer, `clockcam-1-cam.csv`) read about
   2560 MHz during every launch, including the 8x256 floor launches; nvidia-smi's 208 MHz is a slow sample that misses
   microsecond bursts. Longer command lists did not change the fractions. Astra's clock-ramp lead is closed.

## The fix (commit d58a46a0)

Single-chunk kernels write each round's 2RW results into shared slots (two slot sets alternating by round, one CTA
barrier per round) and warp 0 stores the round with one coalesced STG. The warp's first unit's weight loads are issued
before the x load, which no longer waits for them; the K loop skips its loads once. Row-major weight loads use LDG.E
immediate offsets (the encoder's LDG.E.64/128 offset field, identical words when 0; simulator mirrors it), so the
register count stays at 54 peak / 56 declared. Host battery 56/56, simulator 34/34 (mutants 1, 2, 3 caught).

## Verdicts

| gate | result | receipt |
|---|---|---|
| KT1 bit-equal to `oma_rz_oracle`: 20,000 random rows, MA grid (49,932 rows), 4 BitNet shapes x 2 layouts | PASS, 0 rows differ of 115,500 | `receipt-main.json` |
| f32 q\|k\|v segments | PASS, 0 rows differ | `receipt-main.json` |
| Negative controls: mutants 1, 3, 2 forced | each exits FAIL | `receipt-red-mutant{1,3,2}.json` |
| Cold bandwidth, L2 defeated, floor taken out | 3 of 4 shapes PASS; 2560x2560 FAIL | `receipt-main.json`, `receipt-cold-{1..5}.json` |
| Word table, bf16 and FP8 GEMVs; launch floor; attention refusal | as before (PASS, PASS, recorded) | `receipt-table-*.json`, `receipt-launch-cost.json`, `receipt-attn-20-5.json` |

Cold fractions of 237.2, in-list launch floor taken out (3.4 to 3.7 us). Before = the 18185404 receipt (main, repeats 2, 3).

| shape | before | after: five cold runs | gate |
|---|---|---|---|
| 3840 x 2560 (q\|k\|v) | 0.480, 0.463, 0.504 | 0.820, 0.907, 0.899, 0.854, 0.822 | PASS (0.759) |
| 2560 x 2560 (o) | 0.540, 0.533, 0.574 | 0.722, 0.732, 0.717, 0.712, 0.705 | FAIL (0.759) |
| 13824 x 2560 (gate\|up) | 0.766, 0.744, 0.764 | 0.955, 0.952, 0.944, 0.969, 0.946 | PASS (0.843) |
| 2560 x 6912 (down) | 0.718, 0.715, 0.707 | 0.813, 0.768, 0.841, 0.851, 0.856 | PASS (0.759) |

Store fix alone (prefetch not yet in): 0.816 / 0.687 / 0.933 / 0.738 (`investigation/fix-1.json`); the prefetch added
0.7 to 0.9 us per launch on three shapes and nothing at 2560x2560 (prediction P10 partly wrong, recorded in the plan).

## Gate measurement: a proposal, not a change

The present gate judges one launch with the in-list floor subtracted. At 2560x2560 that launch is a single burst
(1280 units over 1280 warps, every warp issues its ten loads at once); 9.7 us remain for 1.64 MB where the practical
limit needs 6.9 us, and the floor itself is 3.7 of 13.4 us. Real decode runs about 150 GEMVs back to back in one
command list, each one's x the previous output (GPU-produced, so it may live in GPU-cached memory), so the honest
figure is the time of that list. Proposal: a decode-like receipt, 150 launches in layer order (q|k|v, o, gate|up,
down) with weights rotated past L2, timed as one list and stated as bytes moved over list time against 237.2, floor
counted (in a dependent chain the floor is real cost), beside the design-doc end-to-end gate of 170 tok/s. The
180 / 200 GB/s single-launch thresholds stay as they are until that receipt exists; this folder does not move them.

## UNKNOWN

- The 2.8 us left at 2560x2560 (1.3 us at 3840, 1.8 us at 13824): mechanism not identified (one-burst fill and drain,
  first-touch translation, or other). Removing the x round trip did not move it.
- 2560 x 6912 runs in waves (CTA start spread about 20 us in the timeline camera; shared 13.9 KB per CTA) and swings
  0.77 to 0.86 between runs. The launch descriptor's SM shared-memory carveout fields (word 36, kept from a captured
  descriptor) were not touched; the documentation for them was not read. Next variable, not this PR.
- Resident CTA capacity and SM count were not measured; the 16x-list camera run failed its oracle check (cause not found).
- Why uncached 4-byte stores cost 9 to 11 us per launch (measured, mechanism not).
