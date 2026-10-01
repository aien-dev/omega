# M20 OMEGA_TENSOR: semantic layer and CPU realization

Status 2026-10-01: **M20 NOT QUALIFIED.** The semantic layer and the CPU
realization exist and pass their CPU tests. E1 (numeric closure) PASSED on
main 4863803 (receipt merged in omega#177). M20 qualification is blocked on
GB10 parity, which has not run, crash-safe storage and a receipt.

Plan source: `aien-architecture/CURRENT_EXECUTION_PLAN.md`, section
"E2. M20 OMEGA_TENSOR". "M20" here means OMEGA_TENSOR only; the runtime
composition program is COMPOSITION-1/2, not M20.

## Files

| File | Role |
|---|---|
| `src/tensor/omega_tensor.h` | The meaning: dtypes, shapes, strides, views, identity, storage lifetime, every op's definition, the realization table |
| `src/tensor/omega_tensor.c` | Semantic layer: checks, descriptor arithmetic, data movement. No floating-point arithmetic |
| `src/tensor/omega_tensor_cpu.c` | CPU realization table (E1 CPU tier) |
| `src/tensor/omega_tensor_reduce_seam.{h,c}` | The single seam to the E1 reduction contract (PR #134) |
| `tests/test_omega_tensor.c` | CPU tests against an independent naive reference |
| `tools/tensor_mutations.sh` | Source mutation sweep |
| `mk/tensor.mk` | `make test-tensor`, `make test-tensor-mutations`, `make test-tensor-e1-reduce` |

## Definitions

- **Tensor**: immutable. dtype, rank 0..8, every dimension >= 1, at most
  2^30 elements. No API writes into an existing tensor: creation copies the
  caller's data, every op returns a new tensor, views share storage
  read-only.
- **Dtypes**: FP32 is the arithmetic dtype. F16 and BF16 are storage dtypes;
  arithmetic on them is refused. `omega_tensor_cast` converts with the E1 ops
  (F32_TO_F16 / F32_TO_BF16 RNE, F16_TO_F32 / BF16_TO_F32 exact).
  F16 <-> BF16 directly is refused.
- **Strides**: in elements, >= 0. Zero only in broadcast views. Negative
  strides are not defined.
- **Value identity** (`omega_tensor_value_id`): SHA-256 over
  `OMEGA_TENSOR_VALUE_ID_V1`, dtype, rank, shape and the elements in logical
  row-major order, every NaN canonicalized (NaN payload is not semantic),
  -0 and +0 distinct. Strides, offset and storage are not part of it: a
  transposed view and its dense copy have the same value id.
- **Storage identity**: storage slot + 64-bit generation.
- **View identity** (`omega_tensor_view_id`): storage identity + parent
  tensor + dtype + offset + shape + strides.
- **Storage lifetime**: releasing bumps the slot generation; any access
  through an older handle, including every view of released storage, returns
  `OMEGA_TENSOR_ERR_STALE` and computes nothing. A reused slot always has a
  new generation (no ABA). A slot whose generation reaches UINT64_MAX is
  retired forever instead of wrapping. Releasing a view releases only the
  view descriptor; releasing the owning tensor releases its storage too.
- **Broadcasting** (numpy rules): align at the trailing axis, missing leading
  axes count as 1, per axis sizes must be equal or one of them 1; result is
  the larger. Otherwise `OMEGA_TENSOR_ERR_SHAPE`.
- **Views**: permute / transpose (last two axes), slice (start < stop <= dim,
  step >= 1), reshape (view only if row-major contiguous, else
  `OMEGA_TENSOR_ERR_NOT_CONTIGUOUS`; `omega_tensor_contiguous` makes the copy
  explicit), broadcast_to (zero strides). Every view is bounds checked
  against its storage when made.
- **Elementwise**: SQRT; EXP2, LOG2, SIGMOID, TANH (E1 bounded contract, not
  correctly rounded: bit-exact with the E1 CPU sequences per element, max ulp
  EXP2 2, LOG2 2, SIGMOID 3, TANH 3); ADD, SUB, MUL, DIV (omega_math_div), MIN, MAX, the
  14 compare-select predicates (out = P(a, b) ? a : b); FMA (a*b+c rounded
  once). All E1 scalar ops, all operands broadcast together.
- **Reductions** along one axis, SUM / MAX / MIN / MEAN, keepdims optional,
  order `RECURSIVE_TILE32_PAIRWISE_TREE_LANE_DELTA_16_8_4_2_1_PAD_IDENTITY_MIN_ONE_LEVEL`
  (identical string to E1 WP-D, PR #134).
- **Matmul**: `[M,K] x [K,N]`, any M, N, K >= 1, and batched
  `[...,M,K] x [...,K,N]` with broadcast batch axes. Declared order
  `OMEGA_TENSOR_MATMUL_DECLARED_ORDER`: each product rounded once (FMUL RNE,
  no fusion), then the E1 SUM tree above over k. A Tensor Core / FFMA-chain
  realization computes different bits and must declare its own bounded
  contract.
- **Meaning vs realization**: `OmegaTensorRealization` is a function table
  (elementwise E1 op over dense buffers, reduce over a dense row, declared
  order string). The semantic layer refuses a table whose order string is not
  the declared one. CPU fills it now; GB10 SIMT / Tensor Core fill it later.
  No GB10 code is referenced by the semantic layer.

### What "beyond the old narrow kernel" means

The old M18 tensor matmul (`src/omega_blackwell_matmul.*`) takes raw
contiguous buffers, caps M, K, N at 1024, supports INT32 / FP16 / BF16 inputs
with no declared FP32 accumulation order, and has no strides, views,
broadcast or batch. `src/omega_matvec.*` is a uint64 matrix-vector with fixed
maximum shapes. M20 here: any shape up to 2^30 elements, FP32 with a named
accumulation order, strided / transposed / sliced / broadcast operands,
broadcast batches. The GB10 realization of this general matmul does not
exist yet.

## Reduction seam

The seam calls `omega_reduce_cpu` from `src/omega_numeric_reduce.h` (E1 WP-D,
omega #134, merged c54d492); a `_Static_assert` and a runtime check pin the
order string. Before #134 merged, a local copy of the order was cross-checked
bit-identical against #134 head 6da80bf (same KAT value id); that copy is now
removed. Order and padding mutations are caught by the E1 WP-D suite.

## Qualification checklist (plan E2)

| Item | Status | Evidence |
|---|---|---|
| Shape / type checks | PASS (CPU) | `test-tensor`: rank, zero dim, size overflow, dtype, K mismatch, axis, perm, unknown op refusals |
| Broadcasting | PASS (CPU) | 8 shape cases incl. 3 refusals; 5 value cases x 20 binary ops vs index-arithmetic reference; broadcast_to zero strides |
| Views / strides | PASS (CPU) | permute, slice with steps, transpose of slice == slice of transpose, view of view of view, zero-stride transpose, reshape refusals, bounds |
| Generation safety | PASS (CPU) | use after release, double release, stale view of released storage (read, info, op, matmul, view, id), no ABA on slot reuse, UINT64_MAX retirement |
| Immutability / no in-place | PASS (CPU) | no write API exists; input value ids unchanged by every op; outputs never alias inputs |
| CPU parity | PASS (CPU) | every op bit-exact vs independent reference (omega_ref_*, omega_ieee_div/sqrt, omega_ref_ffma_int, own tree copy), random + all special pairs |
| Transcendental unary ops (EXP2, LOG2, SIGMOID, TANH) | PASS (CPU) | Host PASS, CPU tier: forge log `HIVE-M20-transc-175026` (`test-tensor` + `test-tensor-mutations`, 7/7 mutants caught). Bounded contract, NOT correctly rounded (max ulp EXP2 2, LOG2 2, SIGMOID 3, TANH 3; `docs/numeric/E1_TRANSCENDENTAL_CONTRACT.md`). `test-tensor` section 7b: raw-bit equality with direct `omega_math_*` calls on dense, transposed, strided-slice-with-offset, transpose-of-slice, broadcast row/scalar and rank-0 operands incl. specials; canonical qNaN; unknown op / F16 / stale / missing `transc` entry refused. Mutants `TRANSC_EXP2_LOG2_SWAP`, `UNARY_VIEW_STRIDE` in `test-tensor-mutations` |
| Constants and NEG (`omega_tensor_full`/`zeros`/`ones`, `OMEGA_TU_NEG`; answers LT-M21 CR-2) | PASS (CPU) | Host: PASS, CPU tier: forge log `HIVE-M20-const-light-193345` at pre-stack head 049611c (488 pass, 0 fail plain and ASan/UBSan; 11/11 mutants caught); re-run at stacked head queued. GB10: NOT_RUN (no GB10 tensor realization exists; row GB10 parity). Declared result, one per op: `full(v)` fills every element with the exact F32 bits of `v` (-0.0 and NaN payload kept, no canonicalization; value ids still treat all NaNs as one); `zeros` = bits `0x00000000`; `ones` = `0x3f800000`; shape rules as `from_f32` (rank <= 8, dims >= 1, elements <= 2^30, else typed error; the 2^30 boundary itself is not allocated in tests, only 2^30+1 and 2^31 are refused). `NEG` = flip bit 31 with no arithmetic and no E1 call, so neg(+0) = -0 and neg(-0) = +0 (`0 - x` would give +0, wrong); any NaN in gives the canonical quiet NaN `0x7fc00000`, matching the E1 convention "NaN results are the canonical quiet NaN" (`src/omega_numeric.h:137`); F16/BF16 refused; works with a realization that has no `transc` entry. Tests (`test-tensor` section 7c): bit-exact contents, rank 0 and 8, refusals, NEG on the 20-value special grid vs an independent bit rule, NEG through transpose, strided slice and broadcast views. Mutants `ZEROS_NEG0`, `NEG_SUB`, `NEG_NAN`, `FULL_CANON` in `test-tensor-mutations` |
| GB10 transcendental tensor ops | NOT_RUN | GB10 transcendental tensor ops: NOT_RUN (follow-up). No GB10 `transc` entry; a table without one refuses these ops with `OMEGA_TENSOR_ERR_REALIZATION` |
| Unary ops EXP, LOG, RELU (M20 cut ops, LT-M21 CR-1..CR-5) | PASS (CPU) | Host PASS, CPU tier: forge log `HIVE-M20-ops-light-185351` at pre-stack head 1e16a8d, re-run at stacked head queued (`test-tensor` 438 pass / 0 fail plain and ASan/UBSan; `test-tensor-mutations` 10/10 tensor mutants caught). EXP / LOG: `OMEGA_TU_EXP` / `OMEGA_TU_LOG` call the E1 Omega-defined polynomials `omega_math_exp` / `omega_math_log` (`src/omega_numeric.c:458`, `:501`) once per element through the CPU `transc` entry; the result must equal a direct E1 call bit for bit. NOT correctly rounded: E1 checks them only against binary128 within EXP 40 ulp, LOG 4 ulp (`tests/test_omega_numeric.c:376-384`; `docs/numeric/E1_GAP_TABLE.md` row 8 and the `CPU_TIER_INDEPENDENT_ORACLE` line). RELU: no E1 op; tensor-layer bit select with no float arithmetic (`src/tensor/omega_tensor.c` `relu_bits`): x > +0 (incl. +subnormals, +inf) -> x; +0, -0, negatives, -inf -> +0.0; any NaN -> canonical qNaN `OMEGA_QNAN_BITS` 0x7fc00000 (`src/omega_numeric.h:48`, as E1 returns, e.g. `src/omega_numeric.c:459`). Tests: section 7b runs EXP / LOG / RELU raw-bit vs direct E1 (RELU vs an independent field-classifying reference) on dense, transposed, strided-slice, transpose-of-slice, broadcast row/scalar and rank-0 views incl. specials (+-0, +-inf, NaN payloads, subnormals, +-2^31, exp cutoffs 88.7228 / -104); section 7c: RELU explicit bit table (-0 -> +0, NaN payloads -> 0x7fc00000), served without a `transc` entry, F16 / stale refused. Mutants `TRANSC_EXP_LOG_SWAP`, `RELU_NEG_ZERO`, `RELU_NAN_PAYLOAD` |
| GB10 EXP / LOG tensor ops | MISSING_IMPLEMENTATION | E1 itself has no GB10 kernel for the Omega exp / log polynomials (`src/omega_numeric.c:585-588`, `gb10_encoded` false (`src/omega_numeric.h:250`); `docs/numeric/E1_GAP_TABLE.md` row 10) |
| GB10 RELU tensor op | NOT_RUN | RELU is a tensor-layer bit select with no realization call, so a GB10 context would run the same host code; no GB10 run in this work |
| Unary ops RSQRT, ERF, GELU (LT-M21 CR-1 part) | PASS (CPU) | Host PASS, CPU tier: forge log `HIVE-M20-unary2-light-193221` at pre-stack head cc0c66d; re-run at stacked head queued (job `HIVE-M20-unary2`). `OMEGA_TU_RSQRT`, `OMEGA_TU_ERF`, `OMEGA_TU_GELU` appended after `OMEGA_TU_RELU` in the stack (no renumbering); the CPU `transc` entry calls `omega_math_rsqrt` / `omega_math_erf` / `omega_math_gelu` (`src/omega_numeric_transc.h:47,50,53`) once per element, no other arithmetic. Declared result: bit-identical to a direct E1 call on each logical element (raw bits, canonical qNaN `0x7fc00000` per `src/omega_numeric_transc.h:20`). Accuracy is E1's, not the tensor layer's: RSQRT correctly rounded (`OMEGA_TRANSC_MAX_ULP_RSQRT 0`, `src/omega_numeric_transc.h:34`; `docs/numeric/E1_TRANSCENDENTAL_CONTRACT.md:45,99`); ERF bounded contract, max 3 ulp (`transc.h:37`; contract `:48,102`); GELU (erf form, `x Phi(x)`) bounded contract, max 3 ulp (`transc.h:40`; contract `:50,105`). Tests: section 7b cases (dense, transpose, strided slice with offset, transpose of slice, broadcast row and scalar, rank 0, F16 / stale / missing-entry refusals) and section 7c (contract edge inputs on dense and step-2 strided slice vs direct E1, plus contract-stated values such as RSQRT(-0) = -inf, ERF(4) = 1, GELU(-inf) = -0). Mutants `TRANSC_RSQRT_ERF_SWAP`, `GELU_VIEW_STRIDE` (9 source mutants in the sweep with these two) |
| GB10 unary ops RSQRT, ERF, GELU | MISSING_IMPLEMENTATION | E1 has no GB10 realization of these sequences (`docs/numeric/E1_TRANSCENDENTAL_CONTRACT.md:4`, `:197`: GB10 NOT_RUN) and main has no GB10 `OmegaTensorRealization` table (only `src/tensor/omega_tensor_cpu.c`). A table without a `transc` entry refuses these ops with `OMEGA_TENSOR_ERR_REALIZATION` (`src/tensor/omega_tensor.c:676`). CPU tier only in this cut |
| Trig unary ops (SIN, COS) | PASS (CPU) | Answers LT-M21 CR-1 (trig part); branch `hive/M20-trig`, forge job `HIVE-M20-trig`. `OMEGA_TU_SIN`, `OMEGA_TU_COS` appended after `OMEGA_TU_GELU` in the stack (no renumbering); the CPU `transc` entry calls `omega_math_sin` / `omega_math_cos` (`src/omega_numeric_transc.h:51-52`) once per element. Declared result: bit-identical to the direct E1 call on the logical element. Domain (E1 rule, `src/omega_numeric_transc.h:43` `OMEGA_TRANSC_TRIG_MAX_ABS`, `src/omega_numeric_transc.c:517` sincos_core): admitted `abs(x) <= 2^22` inclusive; outside it, +-inf and NaN give the canonical qNaN `0x7fc00000`; sin(+-0) = +-0, cos(+-0) = +1 (`docs/numeric/E1_TRANSCENDENTAL_CONTRACT.md:49`). Accuracy: bounded contract, NOT correctly rounded, max 2 ulp each inside the domain (`src/omega_numeric_transc.h:38-39`, observed 1; `docs/numeric/E1_TRANSCENDENTAL_CONTRACT.md:103-104`). Tests: SIN/COS join section 7b (dense, transpose, strided slice, transpose of slice, broadcast, rank 0, refusals) and new section 7c (grid incl. +-0, +-2^22, +-nextup(2^22), +-nextdown(2^22), inf, NaN, sNaN, through dense, broadcast and strided-slice views: bit-exact vs E1 plus an independent domain-rule check). Mutants `TRIG_SIN_COS_SWAP`, `TRIG_DOMAIN_NUMBER` (out-of-domain NaN replaced by a number). Host PASS, CPU tier: forge log `HIVE-M20-trig-light-193303` at pre-stack head 2a7da1a (425 pass, 0 fail, ASan/UBSan clean, sweep 9/9); re-run at stacked head queued |
| GB10 trig tensor ops (SIN, COS) | MISSING_IMPLEMENTATION | E1 has no GB10 kernel for SIN or COS (`docs/numeric/E1_GAP_TABLE.md:27`: "RSQRT, ERF, SIN, COS, GELU have no GB10 kernel"). No chip work in this cut |
| Mask compare CMP_EQ/NE/LT/LE/GT/GE + `omega_tensor_where` (CR-3) | NOT_RUN | Host: NOT_RUN until the forge reports (job `HIVE-M20-mask`). Declared bits: CMP_P(a, b) = +1.0 (0x3f800000) if the ordered E1 predicate of FSETP_EQ/NE/LT/LE/GT_SEL or FSETP_SEL (GE) holds, else +0.0 (0x00000000), never -0.0; predicate = `omega_ref_fsetp_pred` (`src/omega_numeric.c:322`, masks at :293-308): false if either operand is NaN (CMP_NE included, unlike IEEE !=), -0 == +0. CPU realization: optional `compare` entry, FCMP + FCSEL(+1.0, +0.0) on the same AArch64 conditions as the E1 CPU compare-select (`src/omega_numeric.c:1396-1418`); a table without it refuses CMP_* with `OMEGA_TENSOR_ERR_REALIZATION`. `where(cond, a, b)`: bit copy of a or b (no arithmetic), cond/a/b broadcast together; any cond element other than 0x3f800000 / 0x00000000 (incl. -0.0, NaN, 0.5) refused with new `OMEGA_TENSOR_ERR_MASK` (-12), nothing made. Tests: every predicate on the 20x20 special grid + random pairs vs independent loop (strict bits), where(CMP_P) == SEL_P, broadcast + transposed/sliced views, refusals. Mutants: CMP_FALSE_NEG_ZERO, CMP_EQ_NAN_TRUE, WHERE_ACCEPTS_HALF |
| GB10 mask compare / where (CR-3) | NOT_RUN | No GB10 `compare` entry. E1 has GB10 encodings for the FSETP_<P>_SEL compare-select ops (`src/omega_numeric.c:915-934`) but no compare-to-mask kernel; where() is data movement only. CPU tier only in this cut |
| General matmul | PASS (CPU) | 1x1x1, 7x13x5, 64x64x64, 129x3x257, 3x1025x2, 1x33x1, strided views, batched broadcast [2,1,3,4]x[5,4,6] |
| Reductions | PASS (CPU) | lengths 1..32769 across tile/level edges, every axis of [3,37,5], keepdims, view vs copy, -0 sum |
| Mutation / refusal | PASS (CPU) | `test-tensor-mutations`: 10/10 tensor source mutations caught (forge log `HIVE-M20-ops-light-185351`); 2 in-process mutant realizations (sequential sum, FFMA chain) caught; wrong order string refused |
| Determinism | PASS (CPU) | repeated run same value id; frozen KAT value id in the test |
| Test-only hook not in library | PASS (CPU) | `omega_tensor_test_set_storage_generation` exists only under `-DOMEGA_TENSOR_TEST_HOOKS` (test builds in `mk/tensor.mk`); `test-tensor-no-hooks` (run by `test-tensor`) builds the default objects and fails if `nm` shows the symbol |
| CI | WIRED | `host-suites-2.yml` job `tensor`: `test-tensor` + `test-tensor-mutations` on the AArch64 hosted runner (not the Spark, not a qualification) |
| GB10 parity | NOT_RUN | no GB10 realization table yet; no chip run in this work |
| GB10 general matmul (Tensor Core beyond narrow K/tile) | MISSING_IMPLEMENTATION | |
| Crash-safe storage lifetime | PASS (CPU) | host PASS, forge log HIVE-M20-lifetime-light-182200 (`test-tensor-store` 314/0 plain + ASan/UBSan, 6/6 store mutants caught, hook absent from default build). `src/tensor/omega_tensor_store.{c,h}`: content-addressed payloads + checksummed journal, temp/fsync/rename/fsync-dir commit; recovery replays the journal, re-hashes every payload, refuses torn/short/corrupt records (typed errors), keeps generations monotonic; `test-tensor-store` (round trip, fork crash at all 8 commit phases, journal/payload truncated at every byte and bit-flipped, crafted journals) + 6 store mutants in `test-tensor-mutations` |
| Reproducible receipt | PASS (CPU) | writer implemented and host-tested: `tools/m20_receipt.sh` writes `<evidence-dir>/<sha256>.json` (0444, exclusive, never overwritten), QUALIFIED only with GB10 parity GB10 PASS + matching chip evidence; `test-m20-receipt` + 11 receipt mutants in `test-tensor-mutations`, forge log HIVE-M20-receipt-175835. No real M20 receipt has been written yet because the GB10 parity evidence does not exist. |
| E1 numeric closure | PASS (on main) | E1 PASSED on main 4863803; receipt merged in omega#177 |
| Placement: embed + concat (CR-4, answers LT-M21 CR-4) | PASS (CPU) | Host PASS, CPU tier: forge log `HIVE-M20-scatter-light-193431` at pre-stack head d99af69; re-run at stacked head queued (job HIVE-M20-scatter). `omega_tensor_embed(ctx, src, rank, out_shape, start, step, out)` is the exact inverse placement of `omega_tensor_slice` (`src/tensor/omega_tensor.h` slice: per axis start < stop <= dim, step >= 1, out dim = ceil((stop-start)/step)). Declared bits: a new dense tensor, all-zero bits (+0.0) everywhere except src element i copied bit for bit (NaN payload and -0 kept, any dtype) to start + i*step per axis; so slice(embed(x), start, start+(n-1)*step+1, step) == x. `omega_tensor_concat(ctx, n, tensors, axis, out)`: parts joined in array order along axis, bit for bit. Both are pure semantic-layer copies (gather + scatter in `src/tensor/omega_tensor.c`), no realization call, no arithmetic. Typed refusals, nothing created: rank mismatch / rank 0 `ERR_RANK`, step 0 `ERR_BAD_ARGS` (as slice), a placed position outside out_shape or start/step overflow `ERR_BOUNDS`, zero dim `ERR_SHAPE`, > MAX_ELEMS `ERR_CAPACITY`, concat dtype mismatch `ERR_DTYPE`, other-axis mismatch `ERR_SHAPE`, axis >= rank `ERR_AXIS`, n = 0 / NULL `ERR_BAD_ARGS`, stale handle `ERR_STALE`. `test-tensor` test_placement: 60 random embed trials vs an independent index-arithmetic reference, slice(embed(x)) == x, embed(slice(y)) zero off the sliced set, transposed / broadcast / F16 sources, 40 random concat trials vs reference and slice recovery, view parts, every refusal. Mutants `EMBED_START`, `EMBED_STEP`, `CONCAT_AXIS_OFFSET` in `test-tensor-mutations`. Gather is OUT OF SCOPE: M20 has no index tensor op, so gather / gather-backward is not provided |
| GB10 placement (embed, concat) | NOT_RUN | No GB10 realization table exists (row "GB10 parity"); embed and concat call no E1 op, so there is no E1 GB10 kernel to cite. CPU tier only in this work |

**M20 verdict: not qualified.** GB10 parity NOT_RUN; receipt writer host PASS,
no M20 receipt written yet.

Known minor risks of the crash-safe store (neither ever returns wrong data): a
failed partial save can make a handle read as stale after reopen, and a
hand-deleted journal restarts the generation numbers.

## Open seams

1. Reductions: DONE, the seam calls `omega_reduce_cpu` (#134 merged).
2. Transcendentals (#127): CPU tier host PASS (CPU), forge log
   `HIVE-M20-transc-175026`. `OmegaTensorUnaryOp` gained
   `OMEGA_TU_EXP2`, `OMEGA_TU_LOG2`, `OMEGA_TU_SIGMOID`, `OMEGA_TU_TANH`
   (appended after SQRT, no renumbering). They are bounded-contract ops, not
   correctly rounded: the CPU realization's optional `transc` entry calls
   `omega_math_exp2/log2/sigmoid/tanh` (`src/omega_numeric_transc.c`) once
   per element, so tensor output is bit-identical to calling E1 directly;
   error bounds are E1's (`docs/numeric/E1_TRANSCENDENTAL_CONTRACT.md`).
   GB10 transcendental tensor ops: NOT_RUN (follow-up).
3. GB10 realization table: elementwise through the E1 SIMT ops, reduce
   through `omega_reduce_gb10`, matmul either products + tree (bit-exact to
   this contract) or a Tensor Core path with its own declared bounded
   contract.
