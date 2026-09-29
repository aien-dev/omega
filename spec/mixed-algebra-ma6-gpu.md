# Omega mixed algebra MA-6: GPU realizations of Omega-X on GB10

Stage MA-6 of ADR 0019 (GPU realizations). Operation Omega-X is unchanged from
`spec/mixed-algebra-ma2.md`: y = W . x, W in {-1,0,+1}^(m x n), x int8, y
int32, **exact** (bit-identical to `oma_rz_oracle`). Contract cited, not
minted: Turing PROVISIONAL `contract_digest`
`75772afe8a98f73064f2933d76bc58f7bfe5b9353eb40b703e173b4cd3f22b5f`
(docs/turing/TURING_W0_PROPOSAL.md). This stage mints no ids and makes no
selection. Any selection among these receipts belongs to the Turing record
layer (`turing.decision.v1` via `turing_rank_min_cost`), not to this stage.

**Section 1-7 are the pre-registration. They were committed before the first
silicon run.** Results are appended in section 8 and do not change 1-7.

## 1. Path

Everything runs through Omega's own GPU path: Omega IR + sm_121 encoder
(`src/omega_blackwell_codegen.{h,c}`), Omega QMD and cbank builders
(`src/omega_blackwell_qmd.c`), and the physics M16 native channel
(`m16_native_submit_methods` / `m16_native_wait_marker`, physics pinned by
`physics.lock`). There is no CUDA toolkit, no vendor compiler, no vendor
runtime and no Python.

Encoder ops added for MA-6 (a separate commit, "Blackwell encoder: add
LOP3_LUT, POPC, IADD3_R3, LDG_E_OFF"). They are appended after `BW_IR_BRA`,
so earlier opcodes keep their values and bytes:

| op | form | derived from |
|---|---|---|
| `BW_IR_LOP3_LUT` | LOP3.LUT Rd, Ra, Rb, Rc, lut | LOP3_XOR words, LUT + Rc bytes from the instruction |
| `BW_IR_POPC` | POPC Rd, Rb | new (opcode 0x309 register form) |
| `BW_IR_IADD3_R3` | IADD3 Rd, Ra, Rb, Rc | IADD3 words with Rc |
| `BW_IR_LDG_E_OFF` | LDG.E Rd, desc[UR][Ra.64 + imm24] | LDG_E words with the offset field |

Host test `make test-bw-encoder-ma6` has three parts. It checks that the
existing fixtures are unchanged. It checks that each new op at its neutral
setting reproduces the existing op's words. It checks that each extra field
lands only in its bits. The silicon check is `make ma6-gpu-selftest`
(section 3, gate S).

Control words (stall, scoreboards, waits) come from a scheduler in
`src/algebra/gpu/oma_gpu.c`. It passes them through `insn->control`, the field
the existing kernels use. Ops whose encoder ignores `control` (ISETP.GE
immediate, EXIT) keep their built-in control. The scheduler honours it and
refuses to build if it would need anything else. Nothing is patched after
encoding.

## 2. Realizations (separate table, not the CPU `oma_rz` registry)

| id | weights on device (bits/weight) | activation per call | kernel inner loop |
|---|---|---|---|
| G1_int8_imad | int8 offset binary (w+128), 4 per word, k-major interleave (8) | x widened to int32 (4 B/elem) | extract byte, IMAD with x, sum(x); y = acc - 128 sum(x) |
| G2A_crumb_imad | 2-bit code w+1, 16 per word (2) | x widened to int32 | extract crumb, IMAD with x, sum(x); y = acc - sum(x). Uses multiplies |
| G2B_bitplane_popc | P plane (w=+1) and M plane (w=-1), 32 per word each (2) | x as its 8 two's-complement bit planes (1 B/elem) | LOP3 AND + POPC per plane; y = sum_j c_j (popc(P&X_j) - popc(M&X_j)), c_j = 2^j, c_7 = -128. No multiplies in the loop |
| G3_bf16_hmma | BF16 {-1,0,+1} in HMMA m16n8k16 fragment order, rows padded to 16 (16) | x as BF16 (2 B/elem) | HMMA.16816.F32.BF16, FP32 accumulators start at 1.5*2^23 |

Common to all: split-K. Each thread (or warp, for G3) owns one row (or a 16-row
tile) and one k slice. Slice results are added into y with integer
ATOMG.ADD.STRONG.SYS, so their order cannot change the result. y is zeroed by
the host before each call. Shapes are powers of two, 1024 <= n <= 2^20,
m <= 65536. Other shapes are refused (no tail handling in MA-6).

**G3 exact domain (declared).** Every int8 value is exact in BF16 (8
significand bits), and every product w*x is exact in FP32. An accumulator
starting at M = 1.5*2^23 holds M + s exactly while |s| < 2^22. Each
accumulator's partial s is bounded by 128 * (number of k it sums) <= 128 n,
so the realization is declared exact for **n <= 32,768** and refuses larger
n at plan time. The int32 result is `bits(acc) - 0x4B400000`, taken with
integer adds (no float-to-int op is needed). The exactness of the tensor
core's FP32 accumulation of integers in that range is an empirical claim,
checked by gates X and E. It is not assumed.

## 3. Gates (pass/fail, fixed here)

- **S (silicon self-test, `make ma6-gpu-selftest`)**: 16 checks: LOP3_LUT with
  8 LUTs (0xC0, 0xFC, 0x3C, 0x96, 0xE8, 0x80, 0x0F, 0xCA) on random and
  all-ones/zero operands, LOP3 0xC0 with Rc = RZ, POPC on two operands,
  IADD3 third operand, LDG immediate offset, ATOMG.ADD (256 adds into 8
  words), two %globaltimer reads ordered and < 1 ms apart, thread-id round
  trip; 256 threads. PASS = all 16. If a check fails, every realization that
  uses that op is recorded NOT REALIZABLE and is not timed. G2B needs
  LOP3_LUT and POPC. All four need LDG_E_OFF, IADD3_R3 and ATOMG.ADD.
- **X (exactness)**: every launch in E and in the timed runs is compared
  word for word with `oma_rz_oracle` (padding rows must read 0). One
  mismatching launch = FAIL for that realization at that cell. It is
  recorded, not tuned away, and there is no rerun to clear it.
- **E (edges, `make ma6-gpu-edges`)**: x = -128 everywhere, W all +1 and all
  -1, (m, n) = (64, 16384), (1, 16384) for all four (|y| = 2^21), and G3 at its
  domain edge (16, 32768) (|y| = 2^22). PASS = all exact.

## 4. Timed runs (pre-registered)

- Grid: n in {1024, 4096, 16384} x m in {1, 64, 4096} x sparsity (fraction of
  zero weights) in {0, 0.6}, x uniform int8. Same grid values as MA-2, minus
  sparsity 0.3/0.9.
- Two runs: `run1` seed 20260929 and `run2` seed 20260930
  (`make ma6-gpu-bench MA6_LABEL=runK MA6_SEED=...`). Per cell, the order of
  the four realizations is shuffled with the seed and recorded
  (`run_position`). Per realization: build, pack once, upload once, then 3
  warm-up + **21 timed launches** (N >= 20). Two alternating x vectors,
  each with its own oracle result.
- Preconditions: `~/workspace/.spark-quiet` absent (the target refuses to
  start otherwise; its state is recorded at start and end), no other
  GPU/silicon test running, run detached with output to a log, never
  killed, one run at a time.
- Recorded per receipt: commit, dirty-file count, binary SHA-256, seed and
  run positions, CPU cur-freq per core and thermal zones at start and end,
  load average, quiet-flag state. GPU clock, power and temperature
  samples come from the existing `tools/r15_machine_state.sh` (no new
  sensor reader), stored next to the receipt. Per realization and cell:
  kernel code SHA-256 (a plain content digest, not an identity),
  instruction count, registers, S/U/threads/grid, weight/x/y bytes, build
  time, weight pack time, weight upload time. Per timed launch statistics
  (min/q25/median/q75/max) cover GPU time, host time, x pack, x upload, y
  zeroing, y read-back and the per-call total.
- Timing definitions: **GPU time** = max(end) - min(start) over per-warp
  `%globaltimer` stamps written by lane 0 of every warp. It includes the
  atomics and excludes launch overhead. **Host time** = submit to completion
  marker seen via `m16_native_wait_marker`, which sleeps 50 us between reads,
  so host time is quantized to roughly 50-100 us. **Per-call total** = x pack
  + x upload + y zero + host time + y read-back. Device buffers are
  CPU-mapped, uncached system memory (the physics allocator). Upload is a
  memcpy into that mapping.
- Receipts: `evidence/MIXED_ALGEBRA/ma6_gpu_<label>.<sha256>.json` and
  `ma6_gpu_<label>_machine_state.<sha256>.ndjson`. They are content-addressed,
  written once, and existing evidence files are never overwritten.
- Energy is not measured in MA-6. No energy claim is made.

## 5. Noise band and claim wording

- Primary figure: median GPU time. Band: [q25, q75] of the 21 timed
  launches. Two candidates on a cell are **not distinguishable** if their
  bands overlap in either run, or if their median order differs between
  the runs.
- No speed threshold is pre-registered, and no superiority claim is made.
  Every speed statement uses the form: "under profile P (GB10, Omega path,
  shape m x n, sparsity s, weights packed once), candidate C: median GPU
  time t [q25-q75], per-call total t' (receipt E)". Comparisons with the
  CPU numbers of MA-2 name both profiles and say "different runs, different
  resources" (the MA-2 figures are one pinned Cortex-X925 core; MA-6 uses the
  whole GPU plus one host thread).
- Negative results (FAIL gates, slower realizations, refusals) are reported
  with the same weight as positive ones.

## 6. Evidence tiers (ADR 0019 section 8)

- G1, G2A, G2B on the tested grid: **E2** (differential against
  `oma_rz_oracle` with the declared coverage: 18 cells x 2 x-vectors x 24
  launches, plus gate E edges). Only powers of two are covered.
- G3: **E2 inside its declared domain n <= 32,768**. Outside it, the plan
  refuses.
- Encoder additions: host layout tests plus silicon self-test on random
  and edge operands, **E2** (not exhaustive).
- Layout models: `make test-ma6-gpu-host` checks that pack + a CPU model of
  each kernel's index arithmetic equals the oracle on the whole grid. This
  checks layouts only, not the silicon.

## 7. Limits (declared before results)

- Power-of-two shapes only. No tail handling.
- Only 32-bit loads. The encoder has no 64/128-bit LDG, no shared-memory
  ops, no IMMA/DP4A (int8 tensor), FP8, FP4 or tcgen05, so none is used.
  "Missing from the encoder" is not a measured result about the hardware.
- The G3 tensor-core use is GEMV: 1 of the 8 HMMA output columns carries
  information, and B is x replicated.
- One kernel launch per call through the full QMD + push-buffer recipe.
  Nothing is resident or persistent.
- G1/G2A widen x to int32 on the host per call. G2B transposes x into bit
  planes on the host per call. G3 converts x to BF16 on the host per call.
  All three are timed as x pack.

## 8. Results

(Appended after the runs; sections 1-7 unchanged.)
