# OWN-30B cuts 0 and 2: decode-shape matmul baseline, attention gqa8 hd128, column-window matmul (2026-10-13)

Builder OWN30B-A (Fable 5.1). Branch own30b/moe-window. Library and Cut 0 binaries at omega 25dd7c10; the Cut 2 gate
binary v2 (two standalone-tensor cases added to the test only, library byte-identical, see manifest.txt) at 6fa6ccb1.
Physics at the physics.lock commit 6d7cf0d4 (worktree ~/workspace/physics-own30b). GB10, driver 580.173.02 (device.txt).
Settings: marker spin 2000 us, CTA budget 1024 (the serving daemon defaults). One GPU lock hold per script
(run_chip.sh; the v2 re-run held the lock again, same memory state: free -g in device.txt). Raw outputs: *.log, *.json.

## Cut 0a: m = 1 resident bf16 matmul at the Qwen3-30B-A3B decode shapes (gpu_matmul_api_test --timing, 100 calls, median)
| shape (k x n) | MB | r1 ms (GB/s) | r2 | r3 | note |
|---|---|---|---|---|---|
| 2048 x 5120 (qkv) | 21.0 | 0.149 (141) | 0.209 (100) | 0.105 (200) | |
| 4096 x 2048 (o) | 16.8 | 0.167 (100) | 0.162 (104) | 0.109 (154) | |
| 2048 x 1536 (one expert gate+up) | 6.3 | 0.130 (48) | 0.107 (59) | 0.078 (81) | standalone small tensor: noisy, 1.7x between repeats |
| 768 x 2048 (one expert down) | 3.1 | 0.077 (41) | 0.058 (54) | 0.044 (71) | same |
| 2048 x 12288 (8 experts gate+up) | 50.3 | 0.249 (202) | 0.224 (225) | 0.212 (237) | |
| 768 x 16384 (8 experts down) | 25.2 | 0.127 (198) | 0.169 (149) | 0.121 (208) | |
| 2048 x 151936 (lm_head) | 622 | 3.062 (203) | 3.167 (197) | 3.059 (203) | stable |
All 21 shape runs PASS (parity 1e-5 vs the double oracle, repeat bit-identical). chip_ns_mean equals the median within
a few us at every shape: the time is on the chip, not in the host copies. Bytes = k x n x 2.

## Cut 0b: attention at head_dim 128, 32 q heads / 4 kv heads (gqa8) and 32/8 (gpu_attention_test --hd 128)
OMEGA_GPU_ATTENTION_PASS: 225 checks, 0 failed (221 in the JSON battery + 4 hd128 checks). 21 gqa8 cases (contiguous f32
and paged bf16 at ctx 1, 17, 256, 2048, 127, 128, 129, 300, 1000; the battery's mutant kernels are caught). U2 retired.

## Cut 2: omega_gpu_matmul_resident_window_f32/bf16 and the multi-window call (gpu_moe_window_test, 6 runs)
Every run OMEGA_GPU_MOE_WINDOW_PASS (115 checks v1, 143 v2). Per case: every window equals the whole-tensor
(omega_gpu_matmul_resident_bf16) columns bit for bit (window_mismatches_vs_whole 0), within 3.4e-7 of the double oracle,
repeat call is a kernel-cache hit and bit-identical, the f32 activation path is bit-identical to bf16, the 8-window call
equals the single windows. Negative controls caught in every run: wrong window (B base skewed one column tile) differs in
every element (skew_control_differ = all m x cols); wrong kernel (last K pass dropped) fails the oracle. Alignment rule
refusals with a tensor present (col0 = 4, window past n, cols = 0) return BAD_ARGS with the stage named.
Timing (m = 1, 100 calls, median; chip time within 2 us of it):
| case | window | per window | GB/s | repeats |
|---|---|---|---|---|
| gate+up window inside the 128-expert tensor (k 2048, n 196608) | 6.3 MB | 56 us | 110 to 113 | 6/6 within 1 us |
| down window inside the 128-expert tensor (k 768, n 262144) | 3.1 MB | 27 us | 112 to 118 | 6/6 |
| gate+up window inside a 16-expert tensor | 6.3 MB | 45 to 56 us | 111 to 139 | |
| standalone one-expert tensor (k 2048, n 1544), v2 only | 6.3 MB | 52 to 66 us | 96 to 122 | 3 runs |
| standalone down tensor (k 768, n 2056), v2 only | 3.1 MB | 40 us | 78 to 80 | 3 runs |
| gate+up windows at m = 16 (prefill rows) | 6.3 MB | 54 to 71 us | 88 to 116 | |
A one-token MoE layer (8 gate+up + 8 down windows): 0.45 + 0.21 = 0.66 ms per layer, 32 ms over 48 layers (INFERRED
from the measured medians). U1 answer: windows run at 110 to 118 GB/s, stable; about half the 200 to 237 GB/s the
50 MB shapes reach, so the grouped 8-expert kernel (Cut 5) is worth about 2x on the expert half of the token.

## Limits
Medians of 100 back-to-back calls on an otherwise idle chip (no other GPU process during the lock hold); not a serving
profile. The small standalone-tensor variance (41 to 81 GB/s at 3 to 6 MB across repeats, both harnesses) is OBSERVED,
its cause UNKNOWN (all buffers are 4 KB pages, physics nvrm.c:433; GPU clock state not read). Window timing used the
f32 activation path; the bf16 path was parity-checked, not timed.
