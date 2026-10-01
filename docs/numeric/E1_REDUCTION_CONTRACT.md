# E1 reduction contract (WP-D)

Closes gap table row 5 (`docs/numeric/E1_GAP_TABLE.md`) on the CPU tiers and,
for SUM, on GB10. Code: `src/omega_numeric_reduce.h`, `src/omega_numeric_reduce.c`
(reference + CPU realization), `src/omega_numeric_reduce_gb10.c` (GB10),
tests `tests/test_omega_reduce.c`, chip runner `tests/run_reduce_chip.sh`.

## Declared order

`RECURSIVE_TILE32_PAIRWISE_TREE_LANE_DELTA_16_8_4_2_1_PAD_IDENTITY_MIN_ONE_LEVEL`

The order depends only on `n` and the element index. Scheduling, thread
count, CTA count and launch chunking cannot change it.

1. Level 0 is `x[0..n)`.
2. One level maps `len` values to `ceil(len/32)` values. Value `j` of the next
   level is the existing warp tree `PAIRWISE_TREE_LANE_DELTA_16_8_4_2_1`
   (`OMEGA_WARP_REDUCTION_DECLARED_ORDER`) over values `[32j, 32j+32)`:
   for `d = 16, 8, 4, 2, 1`, lane `i` becomes `lane[i] (op) lane[i+d]`, the
   lower lane on the left; the tile result is lane 0. Positions `>= len` hold
   the identity of the op.
3. At least one level is applied, and levels repeat until one value is left.
   The number of levels is the smallest `L >= 1` with `32^L >= n`
   (1..32 -> 1, 33..1024 -> 2, 1025..32768 -> 3, up to 2^20 -> 4).

Why tiles of 32 at every level: each level is exactly what the chip-proven
REDUCE_SUM warp kernel computes per warp, and two levels are exactly what a
single 1024-thread CTA (32 warps: warp tree, lane 0 to shared memory,
barrier, warp 0 runs the same tree over the 32 warp results) computes. A
later shared-memory CTA kernel can therefore realize the same order without
changing any bits.

## Ops

| Op | Combine | Identity (padding) | n = 0 |
|---|---|---|---|
| SUM | IEEE `a + b`, RNE, subnormals kept, no FTZ | `-0.0` (`x + -0 == x` bit for bit for every non-NaN `x`, including `+0 + -0 = +0`) | `+0.0` (defined, not computed) |
| MAX | FMNMX_MAX rule: NaN is missing data (the other operand wins; both NaN gives NaN), `-0 < +0` | quiet NaN `0x7fc00000` | quiet NaN |
| MIN | FMNMX_MIN rule, same NaN and zero rules | quiet NaN | quiet NaN |
| MEAN | `omega_math_div(SUM, u2f(n))`, correctly rounded, for `1 <= n <= 2^24` (n exact in FP32) | as SUM | quiet NaN; `n > 2^24` refused (`OMEGA_NUMERIC_ERR_OPERANDS`) |

Notes:
- `-0.0` is the true additive identity, so padding never changes a value; a
  sum of all `-0` stays `-0`. The empty sum is defined as `+0.0`, the usual
  convention for an empty sum, rather than the `-0` the padding would give.
- NaN is the true identity of the MAX/MIN rule (`max(NaN, x) = x`). Padding
  with `-inf` would instead turn an all-NaN input into `-inf`.
- MAX and MIN under these rules are associative and commutative, so their
  result does not depend on the order at all; they still use the declared
  tree. NaN payloads are not semantic (any NaN equals any NaN in parity, as
  `omega_numeric_bits_equal`).
- Signaling NaN inputs to MAX/MIN are treated like quiet NaN (missing data).
  The CPU tier quiets them before FMAXNM/FMINNM, as the FMNMX CPU tier does.
- Overflow goes to infinity, `inf + -inf` is NaN, subnormal partial sums are kept.
- Batched: `omega_reduce_rows_*(op, x, rows, n, row_stride, out)` reduces the
  last axis of `[rows, n]`, each row independently with the same order;
  `row_stride >= n` when `rows > 1`.
- Lengths so large that padding would overflow the address range are refused
  (`OMEGA_NUMERIC_ERR_OPERANDS`); the GB10 path is limited to `n <= 2^26`.

## Worked examples (expected bits written by hand in the test)

- `n = 33`, `x0 = 2^24`, `x1..x32 = 1`: tile 0 gives `2^24 + 30`, tile 1
  gives `1`, level 2 gives `2^24 + 31`, a tie that rounds to even:
  `2^24 + 32 = 0x4B800010` (the exact sum). Left-to-right gives `2^24 = 0x4B800000`.
- `n = 32`, `x0 = 2^24`, `x1 = x17 = 1`, rest `-0`: lanes 1 and 17 meet at
  `d = 16` (`2`), then `2^24 + 2 = 0x4B800001`. Adjacent-pairs-first
  (deltas 1..16) loses both ones: `0x4B800000`.
- `n = 64`, `x0 = 2^24`, `x1 = x33 = 1`, rest `-0`: the ones sit in
  different tiles and each is lost to a tie: `0x4B800000`. A flat fold over
  64 (lane `i` with `i + 32`) pairs them: `0x4B800001`.

## Tiers

- Reference: `omega_reduce_reference`, written from the definition (copy each
  tile into 32 slots, pad, run the lane tree; SUM calls
  `omega_warp_reduce_sum`, the existing declared warp tree).
- CPU realization: `omega_reduce_cpu`, independent code (one buffer compacted
  in place bottom-up, explicit AArch64 `fadd`, `fmaxnm`/`fminnm`, `ucvtf`,
  `fdiv`).
- GB10 (SUM only): `omega_reduce_gb10`. Every level is padded with `-0.0` to
  whole warps on the host and run through the existing REDUCE_SUM kernel via
  `omega_gb10_execute_simt_op`, in chunks of at most `OMEGA_NUMERIC_MAX_COUNT`
  (65536, whole warps, so no tile straddles a launch). The host gathers lane 0
  of each warp between launches: bit moves only, every FADD runs on the chip.
  Pre-submission checks carry `CHECK:` markers (op, buffers, size, order
  string, kernel registered as BIT_EXACT, padding bits, level count, chunk
  shape, and the executor's own `omega_numeric_submit_check` for every chunk
  size). One launch per chunk per level (n = 10^6: 16 + 1 + 1 + 1 = 19).
- GB10 MAX/MIN: MISSING_IMPLEMENTATION. Needs a warp patch of five
  `SHFL.DOWN` + `FMNMX` pairs (deltas 16..1) in the op registry and patch
  table (owned by the scalar lane), then the same host level loop.
- GB10 MEAN: MISSING_IMPLEMENTATION (DIV has no GB10 encoding).
- Single-launch CTA tree (shared memory) and on-device partial pass: not
  built. The order above is chosen so it can be added later without changing bits.

## Tests (`make test-numeric-reduce-cpu`, chip: `tests/run_reduce_chip.sh`)

| ID | What it proves |
|---|---|
| `RED_ORDER_DECLARED` | order string, level counts, hand-derived worked-example bits on both tiers |
| `RED_EMPTY_IDENTITY_SPECIAL_VALUES` | n = 0 results, identities exact, signed zeros, subnormal sums, overflow, inf/NaN rules, MEAN rounding and bounds, bad-argument refusals |
| `RED_CPU_EQUALS_REFERENCE` | randomized differential, 61 lengths (0, 1, 2, primes, 31/32/33 ... 32769, 65537, 1000003, 10^6, 24 random) x 5 input classes x 4 ops, plus batched rows |
| `RED_INDEPENDENT_ORACLE` | exact integer sums, MEAN of exact sums, MAX/MIN against a sequential scan |
| `RED_DIFFERENT_ORDER_CAUGHT` | left-to-right, reversed deltas and flat fold each give different bits on crafted inputs and are flagged; left-to-right also differs on random data |
| `RED_DETERMINISM` | n = 1000003, repeated runs and shifted buffers give identical bits |
| `RED_GB10_PRESUBMIT_CHECKS` | refusals (MAX/MIN/MEAN not encoded, NULL, too large) and acceptance of every tested n, without a device |
| `RED_GB10_PARITY` | chip only: GB10 SUM bits equal the reference for 18 lengths x 3 input classes (`n > 70000`: 1 class), 7 crafted special-value vectors with hand-written expected bits (all `-0`, one `+0`, overflow, `inf + -inf`, `-inf`, subnormal sum) and the three worked examples |
