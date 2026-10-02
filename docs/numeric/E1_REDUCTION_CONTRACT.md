# E1 reduction contract (WP-D)

Closes gap table row 5 (`docs/numeric/E1_GAP_TABLE.md`) on the CPU tiers and
on GB10 for SUM, MAX, MIN and MEAN (MEAN = chip SUM levels plus one chip division). Code: `src/omega_numeric_reduce.h`, `src/omega_numeric_reduce.c`
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
- GB10, all four ops: `omega_reduce_gb10`. Every level is padded on the host
  with the op's identity (SUM/MEAN `-0.0`, MAX/MIN quiet NaN `0x7fc00000`) to
  whole warps and run in chunks of at most `OMEGA_NUMERIC_MAX_COUNT` (65536,
  whole warps, so no tile straddles a launch). The host gathers lane 0 of each
  warp between launches: bit moves only, every FADD/FMNMX runs on the chip.
  One launch per chunk per level (n = 10^6: 16 + 1 + 1 + 1 = 19).
  - SUM and MEAN levels: the existing chip-proven REDUCE_SUM patch words
    (`omega_numeric_patch_words`), checked with `omega_numeric_check_patch`
    before submission (`omega_numeric_submit_check`) and again at launch.
  - MAX/MIN levels: a reduce-owned patch written at `OMEGA_NUMERIC_PATCH_OFFSET`
    of the vecadd kernel (`omega_reduce_gb10_minmax_patch`), 12 instructions:
    five pairs `SHFL.DOWN PT, R9, R2, d, 0x1f` + `FMNMX R2, R2, R9, !PT|PT`
    for d = 16, 8, 4, 2, 1 (the last FMNMX writes R9), then `STG.E [R6], R9`
    and `EXIT`. `!PT` selects max, `PT` min. FMNMX returns the non-NaN operand
    when one is NaN and orders `-0 < +0`, which is the contract's
    `omega_ref_fmax`/`omega_ref_fmin` rule; a NaN pad is therefore the identity.
    The words were checked against nvdisasm 13.0.85 (`-b SM121`) offline
    (`make test-numeric-reduce-nvdisasm`).
  - `omega_reduce_gb10_check_minmax_patch` runs the generic
    `omega_numeric_check_patch` rules and then `CHECK:` markers `mm_shape`,
    `mm_delta_order` (16, 8, 4, 2, 1), `mm_shfl_src`, `mm_shfl_dst` (R9),
    `mm_first_wait_load` (first shuffle waits on SB4), `mm_fmnmx_form`,
    `mm_fmnmx_pred` (MAX vs MIN), `mm_fmnmx_wait`, `mm_fmnmx_srcs`,
    `mm_fmnmx_dst`, `mm_fmnmx_stall`, `mm_store`, `mm_exit`. It runs before
    submission for every chunk size, and again inside the launch against the
    exact QMD and code image submitted.
  - Every level launch (all four ops) uses the reduce-owned executor, which
    waits for the host completion marker and then for the
    QMD release semaphore (`OMEGA_BW_SEMAPHORE_INTERMEDIATE_DONE`) plus
    `dsb sy` before reading results (the stores-after-marker hazard found by
    the DIV/SQRT lane, omega#141).
  - MEAN: the chip computes the SUM levels, then the final
    `omega_math_div(SUM, u2f(n))` on the chip as one launch of the whole-program
    GB10 DIV kernel (E1 row 7, `omega_ds_gb10_run`). The bits are the same as
    the former host division because both are correctly rounded. Receipts say
    `GB10_DIV` for MEAN. Mutant: `-DOMEGA_REDUCE_MUTATE_MEAN_DIV` divides by n+1
    and must FAIL chip parity.
  - Pre-submission checks carry `CHECK:` markers (op, buffers, size, MEAN
    `n <= 2^24`, order string, kernel registered as BIT_EXACT, padding bits,
    level count, chunk shape, per-chunk submit check).
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
| `RED_GB10_PRESUBMIT_CHECKS` | refusals (NULL, too large, MEAN `n > 2^24`) and acceptance of every tested n for all four ops, without a device |
| `RED_CRAFTED_TABLE` | 14 crafted vectors (all `-0`, signed zeros, overflow, inf, subnormal, all NaN, NaN with one number, sNaN, negative NaN, `+-FLT_MAX` cancel) with hand-written SUM, MAX, MIN and MEAN bits on reference and CPU |
| `RED_GB10_MINMAX_PATCH` | the MAX and MIN patches pass their checks; 30 mutants (reversed or swapped deltas, delta 3, shuffle into R2 or R6, first shuffle not waiting for the input load, swapped shuffle pairs, MAX and MIN predicate flipped, swapped FMNMX operands, wrong last destination, missing wait, short stall, wrong shuffle clamp, stray modifier bit, and the other op's patch) are each refused; a host model that decodes the patch words equals the reference on 110 cases |
| `RED_WRONG_PAD_CAUGHT` | wrong pads (`-inf` or `+0` for MAX, `+inf` or `-0` for MIN) give different bits on crafted inputs and are caught; MEAN worked example n = 33 is `0x48F83E2F` and differs from the sequential order |
| `RED_GB10_PARITY_<OP>` | chip only, per op SUM, MAX, MIN, MEAN: GB10 bits equal the reference for 21 lengths (0, 1, 2, 31, 32, 33, 63, 64, 65, 97, 1000, 1023, 1024, 1025, 4097, 32768, 32769, 65536, 65537, 10^6, 1000003) x 4 input classes (`n > 70000`: 2), plus the crafted table and the worked examples. NaN equals NaN (payload not semantic) |
| `RED_GB10_PARITY` | chip only: overall line, PASS only when every op has 0 mismatches |
| `make test-numeric-reduce-nvdisasm` | offline: the MAX and MIN patch words disassemble (nvdisasm, `-b SM121`) to exactly the text the code declares |
