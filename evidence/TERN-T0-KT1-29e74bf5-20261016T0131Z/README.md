# Ternary engine T0 on the GB10: KT1, our own W2A8 ternary GEMV, plus the BitNet-shape baselines

KT1 multiplies ternary weights (each -1, 0 or +1, packed in omega's 2-bit trit code) by int8 activations and gives
exact int32 sums, or f32 with one weight scale per output segment (q|k|v and gate|up, explicit row boundaries).
Written by our own Blackwell code generator (`src/omega_blackwell_codegen_tern.c`); no CUDA, no PTX, no third-party
engine. Shapes are those of BitNet b1.58 2B4T (hidden 2560, 20 query / 5 KV heads, inter 6912, vocab 128256).

Pins: omega 29e74bf5 (branch tern/t0-kt1, clean), physics 6d7cf0d4, driver 580.173.02, GB10, kernel
7.0.0-1019-nvidia (`device.txt`). Every chip phase ran under `quietlock hold --owner tern-t0` (15-minute windows).
Script: `tools/run_gpu_tern_chip.sh base|red|main|sweep <this dir>`. Baseline record before the speed work:
`../TERN-T0-KT1-f202e87a-20261016T0102Z/` (first commit, fixed stall 6, block layout).

Speeds are stated only as fractions of the physical limit: 273 GB/s (spec) and 237.2 GB/s (practical).

## Verdicts

| gate | result | receipt |
|---|---|---|
| KT1 bit-equal to `oma_rz_oracle`: 20,000 random rows, MA grid 36 points (49,932 rows), 4 BitNet shapes x 2 layouts | PASS, 0 rows differ of 115,500 | `receipt-main.json` |
| f32 q\|k\|v with three different segment scales | PASS, 0 of 3840 rows differ | `receipt-main.json` |
| Negative controls: mutant 1 (flipped plane), 3 (dropped pair), 2 (segments mapped as r / (n / 3)) | all caught on the chip (512/512, 511/512, 1919/3840) and each forced run exits FAIL | `receipt-red-mutant{1,3,2}.json` |
| Cold bandwidth, L2 defeated, kernel time (launch floor taken out) | 1 of 4 PASS, see below | `receipt-main.json` |
| Word table 128256 x 2560, bf16 GEMV | 0.944 of 237.2 (calc:db6b847e), parity and 3 mutants PASS | `receipt-table-bf16.json` |
| Word table 128256 x 2560, FP8 GEMV | 0.912 of 237.2 (calc:eb4a7274), 3 mutants caught | `receipt-table-fp8.json` |
| Decode attention at 20 / 5 heads, head dim 128 | REFUSED (exit 3): the kernel is fixed at 32 / 4 heads with q/k norm; BitNet has neither | `receipt-attn-20-5.json` |
| Launch floor | `receipt-launch-cost.json`; in-list floor (8 x 256 tensor) median 3.456 us | `receipt-launch-cost.json`, `receipt-main.json` |

Cold bandwidth (default kernel: row-major, 1 row per half-warp, 8 warps, 160 CTAs; copies rotated to >= 96 MiB per
command list, L2 is 24 MiB). Gate thresholds as fractions of 237.2: 180 GB/s = 0.759 (calc:501d0e52), 200 GB/s = 0.843
(calc:81854901).

| shape | per launch, with floor, of 237.2 | kernel time, of 237.2 | gate |
|---|---|---|---|
| 3840 x 2560 (q\|k\|v) | 0.404 | 0.467 (calc:17afbdeb) | FAIL (needs 0.759) |
| 2560 x 2560 (o) | 0.431 | 0.549 (calc:953943f4) | FAIL (needs 0.759) |
| 13824 x 2560 (gate\|up) | 0.696 | 0.744 (calc:942ae831) | FAIL (needs 0.843) |
| 2560 x 6912 (down) | 0.678 | 0.776 (calc:e12f222c) | PASS (needs 0.759) |

Gate interpretation (a decision, flagged for review): the gate is judged on kernel time. With the launch floor
counted, even a kernel at the full 273 GB/s spec would reach only 173.2 GB/s at 2560 x 2560 with this run's 3.456 us
floor (calc:54c00990), below the 180 gate; the floor is 0.216 of that launch (calc:b70017b7). Both figures are in the
receipt (`gbs` with the floor, `kernel_gbs` without).

Sweep (informational, `receipt-sweep-*.json`, one variable each against the default): block rows 4, row-major rows 2,
warps 4 and 16, CTAs 96 and 256. None beats the default on all four shapes. Warps 16 and CTAs 256 are faster at 3840 x 2560 and slower on the
two large shapes.

## What changed between the baseline and this run

1. A stall-scheduling pass after register allocation keeps every consumer at least 6 cycles behind each
   fixed-latency write (the repo rule, `omega_gpu_elementwise_api.c:13`) instead of stalling 6 on every instruction.
   The host test checks every distance, and a shortened-stall control must be caught.
2. The inner loop interleaves rows and trit pairs so independent work fills those stalls.
3. Default shape from the sweeps: row-major layout, 1 row per half-warp, 160 CTAs.

## UNKNOWN

- What limits cold speed below 0.78 of 237.2. Diagnostic hot runs (weights resident in L2, not gated) are much faster, and
  uncached weights change nothing (investigation diagnostics, not in this folder). Hypothesis, not tested: the time follows the number of 4 KiB pages touched (GPU address translation);
  physics `nvrm` allocates without a page-size attribute. Testing it means a physics driver change, outside T0.
- GB10 GPU clock during the runs (nvidia-smi reads 208 MHz idle, max 3003 MHz; not sampled during the runs).
- Attention cross-reference: omega#470 (hd-128 gqa_f32 kernels at 72 / 84 registers against a 64 bound) is a
  different path, `gpu_attention_test`; this receipt uses `gpu_decode_attn_test`, which refuses the geometry before
  any kernel is built, so no register count or occupancy exists for 20 / 5 here.

A first run at 2300f164 measured the block layout by mistake (the test restored the old default after its correctness loop); that record is kept outside the repo labelled INVALID, and 29e74bf5 fixes the test.
