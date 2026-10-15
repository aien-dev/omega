# omega #460 on the GB10: wide matmul (N 151936) at even 16-row tile counts

#460 (effb0bc5) changed `omega_matmul_frag_block_shape`: an rt whose last C tile store offset does not fit the
24-bit STG immediate steps down (lm_head width: to the single-tile kernel). #460 proved it on the host only.
This folder proves it on the chip at omega main 7e041401, with the pre-fix commit df9bd9c4 (effb0bc5^) as the red.

Pins: omega 7e041401 (green) and df9bd9c4 (red), detached worktrees, clean; physics 6d7cf0d4
(`~/workspace/pkv514-pins/physics`); driver 580.173.02, GB10; kernel 7.0.0-1019-nvidia (`PINS`).
GPU shared with a MAX server on :18097 (not touched). Every chip run under `quietlock hold --owner WIDEPF`.

Scope note: the changed rule serves the generic bf16-in / f32-out matmul (`omega_gpu_matmul_*`,
`omega_gpu_matmul_prepare`) and `omega_gpu_moe_api.c`. The FP8 prefill GEMM (pfgemm/qgemm) does not call it.

## 1. Smallest repro (`widepf_repro.c`)

Built per commit: `make PHYSICS_DIR=<physics> build/libomega_gpu.a`, then
`gcc -std=gnu11 -Wall -Wextra -Werror -O2 -D_GNU_SOURCE -Isrc -o widepf_repro widepf_repro.c build/libomega_gpu.a -lpthread -lm`.
Shapes (`shapes.txt`): m in 16 (odd tile count, control), 32, 64, 128, 2048 x k 2048 x n 151936, plus 128x2048x65536
(rt 4 -> 2 step). Per shape: `omega_gpu_matmul_f32` twice (second call must hit the kernel cache and be bit-identical
to the first), then a host reference on the same bf16-rounded inputs in double, error relative to the sum of |terms|,
element bad if >= 1e-5 (the contract in `src/omega_gpu_matmul_api.h:100-102`). The API's own single-thread oracle is
off (minutes per shape); the reference is the same formula, threaded. Instrument controls in every shape: one output
element perturbed by +1 must be flagged (`negctl_bad=1`); at main, the API mutant kernel (last K step dropped) at
32 and 128 rows must be flagged.

| commit | 16 | 32 | 64 | 128 | 128 x 65536 | 2048 | runs |
|---|---|---|---|---|---|---|---|
| df9bd9c4 (red) | PASS (rt 1) | CODEGEN_FAIL (rule 2x4) | CODEGEN_FAIL (4x4) | CODEGEN_FAIL (4x4) | CODEGEN_FAIL (4x4) | CODEGEN_FAIL (4x4) | 3/3 same verdicts |
| 7e041401 (green) | PASS (1x1) | PASS (1x1) | PASS (1x1) | PASS (1x1) | PASS (2x4) | PASS (1x1, 2 launches) | 5/5 PASS |

Green, every shape and run: ref_bad 0, max rel error 3.5e-7 to 4.8e-7, poison left 0, repeat mismatch 0, cache hit on
the second call, negative control flagged. Mutants caught: 4,854,407 bad elements at 32 rows, 19,417,886 at 128.
Chip time at 2048 x 2048 x 151936: 0.91 to 0.94 s over 2 launches in 5 runs (marker timeout 20 s per launch).
Logs: `red-df9bd9c4/prefix-red*.log`, `green-7e041401/main-green*.log`.

## 2. Serving-alloc test (host, fake device): `make PHYSICS_DIR=<physics> -k test-gpu-serving-alloc`

- 7e041401: all eight modes PASS (`green-7e041401/serving-alloc-omega-main.log`).
- df9bd9c4: bounds FAIL 2 (stops the target); run alone, sweep FAIL 130 (128 of them `prepare m=.. 2560x151936 rc=-3`,
  CODEGEN_FAIL) and pin FAIL 3 (`red-df9bd9c4/`). Same counts as the #460 description.

## 3. Existing chip gates at 7e041401 (`gates.sh`, under `flock /tmp/aien-gb10.lock`)

`gates/summary.txt`: GPU_MATMUL_API_CHIP PASS; GPU_PREFILL_ATTN_CHIP PASS (main 164/0, main-fused 121/0);
L1G_CHIP PASS (qmma probe x2, pfgemm, qgemm, qgemm cached, wait-0 gateup and down: all PASS).

## 4. OWN-30B check-full: skipped (would not touch the fix)

Memory was available (68 GB). Skipped because the run could not exercise #460: aien-sovereign-core main
(bbca84b1) links omega from `omega.lock` = 62833658, which predates #460; and OWN-30B calls the lm_head with one row only
(`qwen3_moe_omega.rs` `logits`: `matmul_f32(1, ..)` or the bf16 streaming GEMV), so its row tile count is 1 and the
rule never picked rt > 1 there. The 151936-wide prepare path is `gb10_serving` (aien-cli), covered by section 2.

## Limits

- One machine, one driver; 5 green runs and 3 red runs, not a rate.
- The reference uses the documented tolerance, not bit identity (chip f32 accumulation vs host double).
- m = 2048 runs as two 1024-row launches (CTA budget 64); a single launch of 2048 rows was not tested.
- The MoE path (`omega_gpu_moe_api.c`) shares the rule and was not run here.
