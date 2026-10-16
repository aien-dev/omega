# Ternary engine T0 on the GB10: KT1, our own W2A8 ternary GEMV, plus the BitNet-shape baselines

KT1 multiplies ternary weights (each -1, 0 or +1, packed in omega's 2-bit trit code) by int8 activations and gives
exact int32 sums, or f32 with one weight scale per output segment (q|k|v and gate|up, explicit row boundaries).
Written by our own Blackwell code generator (`src/omega_blackwell_codegen_tern.c`); no CUDA, no PTX, no third-party
engine. Shapes are those of BitNet b1.58 2B4T (hidden 2560, 20 query / 5 KV heads, inter 6912, vocab 128256).

Pins: omega 18185404 (branch tern/t0-kt1, clean), physics 6d7cf0d4, driver 580.173.02, GB10, kernel
7.0.0-1019-nvidia (`device.txt`). Every chip run held the quiet flag (`quietlock hold --owner tern-t0`, windows of
10 to 15 minutes). Script: `tools/run_gpu_tern_chip.sh base|red|main|sweep <this dir>`.
Earlier records, kept: `../TERN-T0-KT1-f202e87a-20261016T0102Z/` (first commit: fixed stall 6, block layout) and
`../TERN-T0-KT1-29e74bf5-20261016T0131Z/` (before review fixes; holds the informational sweep).

Speeds are stated only as fractions of the physical limit: 273 GB/s (spec) and 237.2 GB/s (practical).

## Verdicts

| gate | result | receipt |
|---|---|---|
| KT1 bit-equal to `oma_rz_oracle`: 20,000 random rows, MA grid 36 points (49,932 rows), 4 BitNet shapes x 2 layouts | PASS, 0 rows differ of 115,500 | `receipt-main.json` |
| f32 q\|k\|v with three different segment scales (expected boundaries written out in the test) | PASS, 0 of 3840 rows differ | `receipt-main.json` |
| Negative controls: mutant 1 (flipped plane), 3 (dropped pair), 2 (segments mapped as r / (n / 3)) | all caught on the chip (512/512, 511/512, 1919/3840); each forced run exits FAIL | `receipt-red-mutant{1,3,2}.json` |
| Cold bandwidth, L2 defeated | FAIL, 0 of 4 shapes reach the gate (below) | `receipt-main.json`, `receipt-bw-repeat{2,3}.json` |
| Word table 128256 x 2560, bf16 GEMV | 0.924 of 237.2 (calc:4812b209); parity and 3 mutants PASS | `receipt-table-bf16.json` |
| Word table 128256 x 2560, FP8 GEMV | 0.911 of 237.2 (calc:b6f77acd); 3 mutants caught | `receipt-table-fp8.json` |
| Decode attention at 20 / 5 heads, head dim 128 | REFUSED (exit 3): the kernel is fixed at 32 / 4 heads with q/k norm; BitNet has neither | `receipt-attn-20-5.json` |
| Launch floor | `gpu_launch_cost_test` receipt; in-list floor (a real 8 x 256 launch) median 3.424 us | `receipt-launch-cost.json`, `receipt-main.json` |

Cold bandwidth (default kernel: row-major, 1 row per half-warp, 8 warps, 160 CTAs). Copies are rotated to >= 96 MiB
per command list (L2 is 24 MiB); each timed launch writes its own output slice, poisoned before the list and checked
against `oma_rz_oracle` after it. Gate thresholds as fractions of 237.2: 180 GB/s = 0.759 (calc:501d0e52),
200 GB/s = 0.843 (calc:81854901).

| shape | per launch, floor counted | floor taken out (main run) | repeats 2 and 3 | gate |
|---|---|---|---|---|
| 3840 x 2560 (q\|k\|v) | 0.414 | 0.480 (calc:01113a9f) | 0.463, 0.504 | FAIL (0.759) |
| 2560 x 2560 (o) | 0.426 | 0.540 (calc:c2753f9e) | 0.533, 0.574 | FAIL (0.759) |
| 13824 x 2560 (gate\|up) | 0.716 | 0.766 (calc:0dcb5de4) | 0.744, 0.764 | FAIL (0.843) |
| 2560 x 6912 (down) | 0.634 | 0.718 (calc:4f3a94f7) | 0.715, 0.707 | FAIL (0.759) |

Gate interpretation (a decision, flagged for review): the gate is judged on the per-launch time minus the in-list
launch floor (receipt fields `minus_floor_*`). The floor is a real small launch (setup, x staging, barrier, one small
weight read), so this is a marginal rate, not an isolated kernel time. With the floor counted, even a kernel at the
full 273 GB/s would reach only 173.8 GB/s at 2560 x 2560 with this run's floor (calc:afd29c30), under the 180 gate.
Both figures are in the receipt. Either way, no shape passes at this commit.

## What changed since the first commit

1. A stall-scheduling pass after register allocation keeps every consumer at least 6 cycles behind each
   fixed-latency write (the repo rule, `omega_gpu_elementwise_api.c:13`) instead of stalling 6 on every instruction.
   The host test checks every distance, and a shortened-stall control must be caught.
2. The inner loop interleaves rows and trit pairs so independent work fills those stalls.
3. Default shape from the sweeps: row-major layout, 1 row per half-warp, 160 CTAs.
4. Review fixes: one output slice per timed launch, written-out q|k|v boundaries, floor-subtracted rate named as such.

## UNKNOWN

- What holds cold speed at or under 0.77 of 237.2. Diagnostic hot runs (weights resident in L2, not gated) are much
  faster, and uncached weights change nothing (investigation diagnostics, not in this folder). Hypothesis, not
  tested: the time follows the number of 4 KiB pages touched (GPU address translation); physics `nvrm` allocates
  without a page-size attribute. Testing it means a physics driver change, outside T0.
- Why 2560 x 6912 read 0.776 of 237.2 in the single 29e74bf5 run and 0.707 to 0.718 in the three runs here. The only
  change in the measured path is the separate output slice per launch. One run is not a rate.
- GB10 GPU clock during the runs (not sampled).
- Attention cross-reference: omega#470 (hd-128 gqa_f32 kernels at 72 / 84 registers against a 64 bound) is a
  different path, `gpu_attention_test`. This receipt uses `gpu_decode_attn_test`, which refuses the geometry before
  any kernel is built, so no register count or occupancy exists for 20 / 5 here.

A run at 2300f164 measured the block layout by mistake (the test restored the old default after its correctness
loop); that record is kept outside the repo labelled INVALID, and 29e74bf5 fixes the test.
