# E1 scalar contract (WP-A)

What each E1 scalar op must return, and where it runs today. Closes rows 1, 3
and 4 of `E1_GAP_TABLE.md` for the CPU tier only.

## Rules for every op

- Inputs and outputs are 32-bit words. Rounding is round to nearest, ties to
  even. Subnormals are kept on input and output (no flush to zero).
- The FPCR must have FZ, DN, AH, FIZ, NEP, FZ16, AHP clear and RMode = RNE
  (`OMEGA_NUMERIC_FPCR_REQUIRED_CLEAR`). The host tiers refuse otherwise.
- A NaN result is compared by class (any NaN equals any NaN). Payload and
  the quiet bit are not part of the contract.
- Host `libm` defines nothing. The reference is integer code on bit patterns.

## The ops

| Op | Result | Compared as |
|---|---|---|
| `FSETP_<P>_SEL` | `P(a, b) ? a : b`, the chosen word moved unchanged (NaN payload and the sign of zero kept) | `INT_EXACT` |
| `F2I_FLOOR` / `F2I_CEIL` / `F2I_RNI` | int32 of `a` rounded down / up / to nearest even; NaN gives 0; out of range saturates to `INT32_MIN` / `INT32_MAX` | `INT_EXACT` |
| `F2U` | uint32 of `a` toward zero; NaN and every negative give 0; above range gives `0xffffffff` | `INT_EXACT` |
| `I2FP_U32` | the bits of `a` read as uint32, converted to FP32 (RNE) | `BIT_EXACT` |
| `F32_TO_F16` | binary16 of `a` in bits [15:0], bits [31:16] zero; RNE; 65520 and above round to inf | `F16_BITS` |
| `F32_TO_BF16` | bfloat16 of `a` in bits [15:0], bits [31:16] zero; RNE; overflow rounds to inf | `BF16_BITS` |
| `F16_TO_F32` / `BF16_TO_F32` | exact FP32 of the 16-bit value in bits [15:0] of `a`; bits [31:16] ignored | `BIT_EXACT` |
| `FFMA_V` | `a * b + c` with one rounding, `c` read per element; exact zero sums are +0 unless both terms are -0 | `BIT_EXACT` |

The 13 predicates `P`, with U meaning "also true when a or b is NaN"
(-0 equals +0 in all of them):

| P | LT | LE | GT | EQ | NE | NUM | NAN | LTU | LEU | GTU | GEU | EQU | NEU |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| a < b | 1 | 1 | 0 | 0 | 1 | 1 | 0 | 1 | 1 | 0 | 0 | 0 | 1 |
| a == b | 0 | 1 | 0 | 1 | 0 | 1 | 0 | 0 | 1 | 0 | 1 | 1 | 0 |
| a > b | 0 | 0 | 1 | 0 | 1 | 1 | 0 | 0 | 0 | 1 | 1 | 0 | 1 |
| unordered | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 1 | 1 | 1 | 1 | 1 | 1 |

`F16_BITS` / `BF16_BITS`: both words must have bits [31:16] zero; a 16-bit NaN
equals any 16-bit NaN; everything else must match bit for bit
(`omega_numeric_compare_equal`).

## Status

| Op | CPU tier | Independent of the reference? | GB10 |
|---|---|---|---|
| 13 `FSETP_*_SEL` | `fcmp` + `fcsel` (NE, EQU: two `fcsel`) | yes | not encoded, refused |
| `F2I_FLOOR` / `CEIL` / `RNI` | `fcvtms` / `fcvtps` / `fcvtns` | yes | not encoded, refused |
| `F2U` | `fcvtzu` | yes | not encoded, refused |
| `I2FP_U32` | `ucvtf` | yes | not encoded, refused |
| `F32_TO_F16` / `F16_TO_F32` | `fcvt` between `s` and `h` registers | yes | not encoded, refused |
| `F32_TO_BF16` | `bfcvt` (refused at run time if the CPU lacks BF16) | yes | not encoded, refused |
| `BF16_TO_F32` | 16-bit shift | **no**: AArch64 has no scalar BF16 widening instruction, so the CPU tier is the same shift the definition is; the oracle is the separate check | not encoded, refused |
| `FFMA_V` | `fmadd` with per-element `c` | yes (the reference is a 128-bit integer fma) | not encoded, refused (the GB10 FFMA kernel carries `c` uniform) |

"Not encoded" means `omega_numeric_submit_check` and the GB10 executor return
`OMEGA_NUMERIC_ERR_NOT_ENCODED` before anything is submitted. The GB10 parity
manifest stays at 15 ops.

## How it is checked

Three derivations per element: the integer reference (`src/omega_numeric.c`),
the CPU tier (AArch64 instructions, same file) and a second integer oracle
written differently (`tests/numeric_oracle.h`).

- `E1_SCALAR_BOUNDARY_VALUES`: hand-worked cases (ties, saturation, subnormal
  edges, overflow to inf, the fused-rounding cases of `FFMA_V`) plus every
  predicate on 12 pairs covering less, equal (including -0 and +0), greater
  and unordered. Expected bits are written out, not computed.
- `E1_SCALAR_CPU_EQUALS_REFERENCE`: 2^22 structured random inputs per op (about 96 million in all)
  (random bits, integer and F16/BF16 ties, subnormals, near-max, NaN and inf,
  equal and cancelling partners) and all 65536 inputs of the widening ops.
- `E1_SCALAR_INDEPENDENT_ORACLE`: the same inputs, oracle against both tiers.
- `make test-numeric-e1-exhaustive`: every 2^32 input of the nine unary ops
  through all three derivations. Not part of Gate 5.
- `tools/numeric_oracle_mutations.sh` breaks the RNI tie rule in the
  reference, the GTU condition in the CPU tier and the sticky bit of the
  integer fma, and proves each is caught.
