# M20 OMEGA_TENSOR: semantic layer and CPU realization

Status 2026-10-01: **M20 NOT QUALIFIED.** The semantic layer and the CPU
realization exist and pass their CPU tests. M20 qualification is blocked on
E1 (numeric closure) being closed, and on GB10 parity, which has not run.

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
- **Elementwise**: SQRT; ADD, SUB, MUL, DIV (omega_math_div), MIN, MAX, the
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

Default build: a local implementation of the frozen order in which every
combine is an E1 CPU-tier op (`omega_numeric_cpu_realize` FADD / FMNMX /
DIV). With `-DOMEGA_TENSOR_USE_E1_REDUCE` the same seam calls
`omega_reduce_cpu` from `src/omega_numeric_reduce.h` (`make
test-tensor-e1-reduce` once PR #134 is on main); a `_Static_assert` and a
runtime check pin the order string. Checked on 2026-10-01 against PR #134 head
6da80bf (files taken into a scratch directory, not committed): 324/324 pass,
same KAT value id as the local build.

## Qualification checklist (plan E2)

| Item | Status | Evidence |
|---|---|---|
| Shape / type checks | PASS (CPU) | `test-tensor`: rank, zero dim, size overflow, dtype, K mismatch, axis, perm, unknown op refusals |
| Broadcasting | PASS (CPU) | 8 shape cases incl. 3 refusals; 5 value cases x 20 binary ops vs index-arithmetic reference; broadcast_to zero strides |
| Views / strides | PASS (CPU) | permute, slice with steps, transpose of slice == slice of transpose, view of view of view, zero-stride transpose, reshape refusals, bounds |
| Generation safety | PASS (CPU) | use after release, double release, stale view of released storage (read, info, op, matmul, view, id), no ABA on slot reuse, UINT64_MAX retirement |
| Immutability / no in-place | PASS (CPU) | no write API exists; input value ids unchanged by every op; outputs never alias inputs |
| CPU parity | PASS (CPU) | every op bit-exact vs independent reference (omega_ref_*, omega_ieee_div/sqrt, omega_ref_ffma_int, own tree copy), random + all special pairs |
| General matmul | PASS (CPU) | 1x1x1, 7x13x5, 64x64x64, 129x3x257, 3x1025x2, 1x33x1, strided views, batched broadcast [2,1,3,4]x[5,4,6] |
| Reductions | PASS (CPU) | lengths 1..32769 across tile/level edges, every axis of [3,37,5], keepdims, view vs copy, -0 sum |
| Mutation / refusal | PASS (CPU) | `test-tensor-mutations`: 7/7 source mutations caught; 2 in-process mutant realizations (sequential sum, FFMA chain) caught; wrong order string refused |
| Determinism | PASS (CPU) | repeated run same value id; frozen KAT value id in the test |
| GB10 parity | NOT_RUN | no GB10 realization table yet; no chip run in this work |
| GB10 general matmul (Tensor Core beyond narrow K/tile) | MISSING_IMPLEMENTATION | |
| Crash-safe storage lifetime | MISSING_IMPLEMENTATION | storage is in-process memory only; no persistence or crash recovery |
| Reproducible receipt | MISSING_IMPLEMENTATION | no content-addressed receipt writer for this gate yet |
| E1 numeric closure | BLOCKED (other lane) | E1 WP-B #127 and WP-D #134 open |

**M20 verdict: not qualified.** Blocked on E1 close, GB10 parity, crash-safe
lifetime and a receipt.

## Open seams

1. Reductions: switch the seam to `omega_reduce_cpu` once #134 merges
   (`-DOMEGA_TENSOR_USE_E1_REDUCE`, already verified bit-identical).
2. Transcendentals (#127): add EXP / LOG / ... to `OmegaTensorUnaryOp` once
   the E1 bounded contract is merged.
3. GB10 realization table: elementwise through the E1 SIMT ops, reduce
   through `omega_reduce_gb10`, matmul either products + tree (bit-exact to
   this contract) or a Tensor Core path with its own declared bounded
   contract.
