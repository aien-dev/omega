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
| GB10 transcendental tensor ops | NOT_RUN | GB10 transcendental tensor ops: NOT_RUN (follow-up). No GB10 `transc` entry; a table without one refuses these ops with `OMEGA_TENSOR_ERR_REALIZATION` |
| Unary ops EXP, LOG, RELU (M20 cut ops, LT-M21 CR-1..CR-5) | NOT_RUN | Host NOT_RUN until the forge reports (`test-tensor` + `test-tensor-mutations`). EXP / LOG: `OMEGA_TU_EXP` / `OMEGA_TU_LOG` call the E1 Omega-defined polynomials `omega_math_exp` / `omega_math_log` (`src/omega_numeric.c:458`, `:501`) once per element through the CPU `transc` entry; the result must equal a direct E1 call bit for bit. NOT correctly rounded: E1 checks them only against binary128 within EXP 40 ulp, LOG 4 ulp (`tests/test_omega_numeric.c:376-384`; `docs/numeric/E1_GAP_TABLE.md` row 8 and the `CPU_TIER_INDEPENDENT_ORACLE` line). RELU: no E1 op; tensor-layer bit select with no float arithmetic (`src/tensor/omega_tensor.c` `relu_bits`): x > +0 (incl. +subnormals, +inf) -> x; +0, -0, negatives, -inf -> +0.0; any NaN -> canonical qNaN `OMEGA_QNAN_BITS` 0x7fc00000 (`src/omega_numeric.h:48`, as E1 returns, e.g. `src/omega_numeric.c:459`). Tests: section 7b runs EXP / LOG / RELU raw-bit vs direct E1 (RELU vs an independent field-classifying reference) on dense, transposed, strided-slice, transpose-of-slice, broadcast row/scalar and rank-0 views incl. specials (+-0, +-inf, NaN payloads, subnormals, +-2^31, exp cutoffs 88.7228 / -104); section 7c: RELU explicit bit table (-0 -> +0, NaN payloads -> 0x7fc00000), served without a `transc` entry, F16 / stale refused. Mutants `TRANSC_EXP_LOG_SWAP`, `RELU_NEG_ZERO`, `RELU_NAN_PAYLOAD` |
| GB10 EXP / LOG tensor ops | MISSING_IMPLEMENTATION | E1 itself has no GB10 kernel for the Omega exp / log polynomials (`src/omega_numeric.c:585-588`, `gb10_encoded` false (`src/omega_numeric.h:250`); `docs/numeric/E1_GAP_TABLE.md` row 10) |
| GB10 RELU tensor op | NOT_RUN | RELU is a tensor-layer bit select with no realization call, so a GB10 context would run the same host code; no GB10 run in this work |
| General matmul | PASS (CPU) | 1x1x1, 7x13x5, 64x64x64, 129x3x257, 3x1025x2, 1x33x1, strided views, batched broadcast [2,1,3,4]x[5,4,6] |
| Reductions | PASS (CPU) | lengths 1..32769 across tile/level edges, every axis of [3,37,5], keepdims, view vs copy, -0 sum |
| Mutation / refusal | PASS (CPU) | `test-tensor-mutations`: 7/7 source mutations caught; 2 in-process mutant realizations (sequential sum, FFMA chain) caught; wrong order string refused |
| Determinism | PASS (CPU) | repeated run same value id; frozen KAT value id in the test |
| Test-only hook not in library | PASS (CPU) | `omega_tensor_test_set_storage_generation` exists only under `-DOMEGA_TENSOR_TEST_HOOKS` (test builds in `mk/tensor.mk`); `test-tensor-no-hooks` (run by `test-tensor`) builds the default objects and fails if `nm` shows the symbol |
| CI | WIRED | `host-suites-2.yml` job `tensor`: `test-tensor` + `test-tensor-mutations` on the AArch64 hosted runner (not the Spark, not a qualification) |
| GB10 parity | NOT_RUN | no GB10 realization table yet; no chip run in this work |
| GB10 general matmul (Tensor Core beyond narrow K/tile) | MISSING_IMPLEMENTATION | |
| Crash-safe storage lifetime | PASS (CPU) | host PASS, forge log HIVE-M20-lifetime-light-182200 (`test-tensor-store` 314/0 plain + ASan/UBSan, 6/6 store mutants caught, hook absent from default build). `src/tensor/omega_tensor_store.{c,h}`: content-addressed payloads + checksummed journal, temp/fsync/rename/fsync-dir commit; recovery replays the journal, re-hashes every payload, refuses torn/short/corrupt records (typed errors), keeps generations monotonic; `test-tensor-store` (round trip, fork crash at all 8 commit phases, journal/payload truncated at every byte and bit-flipped, crafted journals) + 6 store mutants in `test-tensor-mutations` |
| Reproducible receipt | PASS (CPU) | writer implemented and host-tested: `tools/m20_receipt.sh` writes `<evidence-dir>/<sha256>.json` (0444, exclusive, never overwritten), QUALIFIED only with GB10 parity GB10 PASS + matching chip evidence; `test-m20-receipt` + 11 receipt mutants in `test-tensor-mutations`, forge log HIVE-M20-receipt-175835. No real M20 receipt has been written yet because the GB10 parity evidence does not exist. |
| E1 numeric closure | PASS (on main) | E1 PASSED on main 4863803; receipt merged in omega#177 |

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
