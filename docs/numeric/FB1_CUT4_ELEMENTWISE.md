# FB-1 cut 4: native rmsnorm, rope, swiglu (own codegen, no CUDA)

Written 2026-10-04. Program: `~/handoffs/2026-10-04-fb1-cuts-2-6-plan.md` section 4.
Code: `src/omega_gpu_elementwise_api.{h,c}`, `tests/gpu_elementwise_test.c`,
`tools/run_gpu_elementwise_chip.sh`; four additive IR ops in `src/omega_blackwell_codegen.{h,c}`.

## DOCS READ (gate record `docs-read omega`, 2026-10-04)
- nvdisasm 13.0 `-b SM121` decode of every instruction form the kernels use (the method of
  `tools/divsqrt_nvdisasm_check.sh`). The new words:
  - `0x7308 / w2 0x00000800` decodes `MUFU.EX2 R2, R4` (RCP is `0x1000`, RSQ `0x1400`).
  - `0x7b1d / 0 / 0x00010000 / 0x000fec00` decodes `BAR.SYNC.DEFER_BLOCKING 0x0`; the same words
    `src/omega_numeric.c:885` runs on the chip (LDS_STS op, 148/148 specs).
  - `0x7984 / 0xff / 0x08000800` decodes `LDS R9, [R10+URZ]` and `0x7988 / Rb / 0x080008ff`
    decodes `STS [R8+URZ], R2` (`src/omega_numeric.c:884-886`). The codegen's older `BW_IR_LDS` and
    `BW_IR_STS` decode as `LDS.U8` / `STS.U8` (byte wide); they are left untouched and the 32-bit
    forms are new ops `BW_IR_LDS32` / `BW_IR_STS32`.
- DOCS SILENT on the SASS-level error bound of MUFU.EX2; the PTX ISA bound for `ex2.approx.f32`
  (maximum relative error 2^-22.5 in the primary range) is used, with 2x margin.
- `nvos.h:1112` (nvidia-open-580.173.02): GPU-cached system memory is not coherent with CPU
  mappings. Every buffer in the launcher is `nvrm_alloc_gpu_uncached`, as in the matmul path.
- Scheduling rules come from the chip-learned checks in `src/omega_numeric.c`
  (`omega_numeric_check_patch`): scoreboard producers stall >= 2 and are waited on; fixed-latency
  producers stall >= 5 (we use 6); ISETP stalls 13 before a predicated consumer (codegen default).
  The emitter serialises every dependency (slow, safe); speed is a later cut.

## Kernels (one per op; shape read from the four argument words c[0x0][0x398..0x3a4])
| op | CTA | grid | math on the chip | oracle (tensor.rs, sovereign-core 5fdab70) | tolerance |
|---|---|---|---|---|---|
| ex2 probe | 128 | ceil(n/128) | `MUFU.EX2` | `exp2` in f64 | 1e-6 rel |
| xchg probe | 128 | n/128 | `STS`, `BAR.SYNC`, `LDS` partner word | `a[i ^ 127]` | exact |
| rmsnorm | 128 (4 warps) per row | rows | f32 `FFMA` strided sum of squares, `SHFL.DOWN` 16..1 tree, lane partials to shared, `BAR.SYNC`, 4 `LDS`, `MUFU.RSQ` + one Newton step, `(x*scale)*w` | f64 sum, `(1/sqrt(mean+eps)) as f32`, `x*scale*weight` | 1e-5 rel + 1e-6 abs |
| rope | head_dim/2 per head | heads | `FMUL, FMUL, FADD/FSUB` on (q0,q1) with host cos/sin | same f32 ops, same order (rotate_half, cos/sin cast from f64) | exact (bitwise) |
| swiglu | 128 | ceil(n/128) | `t=g*-log2e; e=EX2(t); d=e+1; r=RCP(d); (g*r)*up` | f64 `g/(1+exp(-g))*up` | 1e-5 rel + 1e-6 abs |

Rope note: the trait is in-place on q and k; the API takes a vector of heads and writes `out`
(which may alias the input). The host builds the per-head cos|sin table once per call.

Launch envelope: at most 64 CTAs per launch (matmul sweep 2026-10-03 lost tiles at 128,
investigation I42, cause UNKNOWN); the API splits rows / heads / elements on the host.

## Mutants (the same parity check must fail against them, in the same chip run)
EX2: `2^(x+1)`. XCHG: reads its own word. RMSNORM: warp tree drops the last `SHFL` step.
ROPE: `out0 = q0*cos + q1*sin`. SWIGLU: `exp(+g)`.

## Register-allocation rule the loops rely on
The IR allocator is a linear scan over textual intervals and knows nothing about back-edges.
`loop_end()` extends every value defined before a loop and read inside it to the back-branch;
`check_loop_invariant()` refuses a kernel that reads, after a loop, a value first defined inside
it without redefining it first. Both are in `omega_gpu_elementwise_api.c`.

## Status
Shader-cache invalidate: every launch emits `INVALIDATE_SHADER_CACHES` (instruction, data, constant) right after the channel setup words, shared constants `OMEGA_BW_MTHD_INVALIDATE_SHADER_CACHES` / `OMEGA_BW_INVALIDATE_SHADER_CACHES_ALL` in `omega_blackwell_submit.h` used by the matmul launcher too (cut 1b finding: stale SM instruction cache after code-address reuse; local driver clcec0.h is a stub, bits from NVIDIA open-gpu-doc, chip-proven FB1-CUT1B-4345406). Test case `icache_reuse_rotation` cycles 10 distinct kernels twice as a guard (red seen by cut 1b on matmul, not re-observed here: one chip run rule).

Host: `make test-gpu-elementwise` (fixtures, refusals, codegen, register budget, nvdisasm listing of
all five kernels) PASS at this head. Chip: see the PR title tags and the receipt folder
`~/workspace/evidence-out/FB1-CUT4-<sha>/` (receipt.json, device.txt, run.log, SHA256SUMS).

## Limits (what this does not prove)
- One chip run per op is a correctness receipt, not a rate; no load test, no repeat statistics.
- The xchg mutant proves the check can say no; it does not prove the barrier is necessary (a
  no-barrier variant might pass by timing luck and would be a flaky negative control).
- rmsnorm's f32 sum differs from the f64 oracle by design; the 1e-5 tolerance covers dims up to
  16384 on data of unit scale. Inputs with huge dynamic range would need an f64 or Kahan sum.
- Speed is not measured beyond `elapsed_ns`; the serialised schedule and one device open per
  launch are the known price until the resident-device work of cut 1b lands.
