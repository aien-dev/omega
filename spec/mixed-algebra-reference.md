# Omega mixed algebra: CPU reference (parity oracle)

Status: reference implementation, correctness first. Code: `src/algebra/`
(prefix `oma_`). Tests: `tests/algebra/test_oma.c`. Build and run:
`make test-algebra` (and `make test-algebra-asan` for the same suite under
AddressSanitizer + UndefinedBehaviorSanitizer). Plain C11
(`-std=c11 -Wall -Wextra -Werror -pedantic`), no dependencies beyond libc/libm.

This library is the correctness yardstick for any faster realization (SIMD,
GPU, analog). A fast path is correct only if it matches these functions bit
for bit (exact ops) or within the documented metric (approximate ops).

## Encoding (H1)

| trit value | pos bit | neg bit | 2-bit code (bit0=pos, bit1=neg) |
|-----------:|:-------:|:-------:|:-------------------------------:|
| -1         | 0       | 1       | 2                               |
|  0         | 0       | 0       | 0                               |
| +1         | 1       | 0       | 1                               |
| INVALID    | 1       | 1       | 3                               |

value = pos - neg. A block (`oma_block`) is 64 trits as two `uint64_t`
planes `{pos, neg}`; lane i is bit i of each plane.

**Invalid-state rule.** (pos=1, neg=1) is never a value. Every decoder,
validator and op checks its inputs and returns an explicit error code; no
function reads an invalid lane as 0 or any other value. Outputs of valid
inputs are always valid (tested).

## Return codes

`OMA_OK = 0`; errors are negative: `OMA_E_INVALID_TRIT` (int8 not in
{-1,0,1}), `OMA_E_INVALID_CODE` (code 3 or > 3), `OMA_E_INVALID_PLANES`
(block with `pos & neg != 0`), `OMA_E_OVERFLOW`, `OMA_E_INVALID_BYTE` (dense
byte >= 243 or non-zero padding), `OMA_E_INVALID_Z3`, `OMA_E_ARG`,
`OMA_E_UNDERFLOW` (absmean of a non-zero input whose mean is below
`FLT_MIN`). `oma_strerror(rc)` names them.

**On error, outputs are left untouched.** Every function validates its whole
input (all trits, all blocks, all bytes, padding, capacity, finiteness)
before it writes any output, so a failed call never leaves a half-written
buffer. The suite checks this on every error path by prefilling each output
with a sentinel byte and comparing after the call.

**Scalar inputs are `int`.** The per-trit, per-code and Z3 scalar functions
(`oma_code_*`, `oma_trit_to_code`, `oma_trit_neg/add/mul`, `oma_z3_*`,
`oma_trit_to_z3`, `oma_dense_byte_decode`) take `int`, so an out-of-range
value such as 256 or -129 reaches the range check and is rejected; it is
never silently wrapped to a valid 8-bit value at the call.

## API

`oma_trit.h`
- `oma_trit_make` (checked constructor), `oma_trit_to_code`, `oma_code_to_trit`.
- Per-trit, on codes and on values: `neg`, `add` (returns sum trit and carry
  trit with a + b = sum + 3*carry), `mul`.
- Block: `oma_block_validate`, `oma_block_encode`/`decode` (int8[64]),
  `oma_block_neg` (swap planes), `oma_block_add` (lane-wise sum + carry
  blocks, no propagation between lanes), `oma_block_mul` (H2:
  pos=(ap&bp)|(an&bn), neg=(ap&bn)|(an&bp)).
- `oma_block_dot`: popcount(pos) - popcount(neg) of the H2 product.
- `oma_dot_tw_i8` (H3): ternary weights (ceil(n/64) blocks) x int8
  activations by add/sub only, exact int32. Lanes >= n must be zero
  (`OMA_E_ARG`); n <= 16,777,215 so |result| <= 128n fits int32
  (`OMA_E_OVERFLOW` above).
- `oma_int_to_bt` (canonical: little-endian, no leading zero digit, 0 has
  zero digits, at most 41 digits for any int64 including INT64_MIN),
  `oma_int_to_bt_fixed` (exactly n digits, `OMA_E_OVERFLOW` if the value needs
  more), `oma_bt_to_int` (rejects bad digits; `OMA_E_OVERFLOW` outside int64;
  accepts leading zeros).

Block add sum formula (a+b in {-2..2}, `z` = neither plane set):
sum.pos = (ap&bz)|(az&bp)|(an&bn), sum.neg = (an&bz)|(az&bn)|(ap&bp),
carry = {ap&bp, an&bn}.

`oma_z3.h`: Z3 values {0,1,2}: `make`, `add`, `mul`, `neg`, and the map
0<->0, 1<->+1, 2<->-1 (`oma_z3_to_trit`, `oma_trit_to_z3`). The map is a ring
isomorphism, so Z3 block ops run on the same bitplanes carry-free:
`oma_z3_block {one, two}` is bit-identical to `oma_block {pos, neg}`;
Z3 add = balanced-add sum with carry dropped, Z3 mul = H2, Z3 neg = swap.

`oma_pack.h`
- Bitplane form: `oma_pack_bitplane` / `oma_unpack_bitplane` (n trits <->
  ceil(n/64) blocks, unused lanes zero and checked on unpack);
  `oma_block_serialize` / `deserialize` (16 bytes: pos then neg, little-endian;
  deserialize rejects (1,1)).
- Dense form: 5 trits per byte, byte = sum_{i<5} (t_i + 1) * 3^i, least
  significant trit first, valid bytes 0..242. `oma_pack_dense`,
  `oma_unpack_dense` (rejects byte >= 243 and non-zero padding trits in the
  last byte, so every trit string has exactly one encoding),
  `oma_dense_byte_decode`.

`oma_quant.h`: `oma_quant_absmean`, `oma_quant_rel_l2` (see below).

## Exact vs approximate

Exact (bit-for-bit, integer-defined): all trit, block, dot, Z3, integer
conversion and pack/unpack operations.

**Approximate:** `oma_quant_absmean` (BitNet b1.58 style). It is a lossy
transform, not a realization of the float values.

- mean = sum|w_i| / n, with the sum and the division in **double**;
  scale = (float)mean.
- q_i = clamp(round(w_i / scale), -1, 1), round half away from zero. The
  reference computes w_i / scale in **double**. Because q_i only depends on
  whether |w_i| >= scale/2, this is the same as the exact rule
  "q_i = sign(w_i) if |w_i| >= scale/2, else 0". A correctly rounded float32
  division gives the same q: w_i and scale are floats with scale >= FLT_MIN,
  so an exact quotient below 1/2 sits more than 2^-26 below it and cannot
  round up to 1/2 (checked on 200,000,000 near-tie pairs, 0 disagreements).
  Fast paths that use an approximate reciprocal or flush-to-zero are not
  covered by that argument and must be checked against this reference.
- All-zero (or empty) input: `OMA_OK`, scale 0, q = 0.
- Non-zero input whose double mean is below `FLT_MIN` (so scale would be
  subnormal or 0): `OMA_E_UNDERFLOW`, outputs untouched. A successful call
  therefore returns scale 0 (input all zero) or a normal float, never a
  subnormal, and never quantizes non-zero input to all zero silently.
- NaN/Inf input: `OMA_E_ARG`. No epsilon is added (BitNet adds a tiny eps;
  here the zero case is explicit).

The loss is measured by `oma_quant_rel_l2` = ||w - q*scale||_2 / ||w||_2,
computed in double. It returns `OMA_E_ARG` for NaN/Inf w_i, for a NaN, Inf
or negative scale, and when ||w|| = 0 but the reconstruction is not zero
(relative error undefined); scale 0 is accepted. On `OMA_OK` the error is
finite.

Properties that hold exactly:
- q of already-ternary input equals the input (scale = fraction of
  non-zero entries).
- Re-quantizing q*scale returns the same q, except that it returns
  `OMA_E_UNDERFLOW` when k*scale/n < `FLT_MIN` (k = number of non-zero q).
  Proof sketch: q_i*scale is exact; the new scale s2 = (float)(k*scale/n) <=
  scale, so every non-zero lane has |q_i*scale| >= s2/2 and keeps its sign.
## Test coverage (one run: 25,672,462 checks, 0 failures)

| section | checks | what |
|---|---:|---|
| constructors + codes | 1,718 | every int in [-300,300], all 256 codes, pinned H1 table |
| per-trit truth tables | 161 | all 9 valid pairs vs integer oracle and a literal table; invalid code 3 (and 4, 255) rejected for neg/add/mul; value-level ops over [-3,3]^2 |
| invalid (1,1) rejection | 380,256 | 20,000 random blocks with one (1,1) lane through every op, decoder, serializer and Z3 path |
| exhaustive 3^k, k=1..6 | 14,210,967 | all 3^k x 3^k pairs: add/carry, mul, neg, dot, Z3 add/mul vs naive; per vector: dense and bitplane round trip, dense byte value, digits<->int canonical round trip, ternary x int8 dot with 4 activation sets (incl. -128/127 extremes) |
| dense bytes | 1,727 | all 256 byte values: 243 decode + repack, 13 rejected (alone and mid-stream), padding canonicality for n=1..4 |
| integer <-> ternary | 3,482,793 | all 65,536 int16 values vs an independent offset-base-3 oracle (canonical, fixed-width, overflow on n-1 digits); (3^k-1)/2 boundaries k=1..40; INT64_MIN/MAX and 1,000,000 random int64 round trips; decode overflow and bad digits |
| random block properties | 6,298,255 | 1,000,000 random valid block pairs (varied density) vs lane oracle; 100,000 multi-block ternary x int8 dots, n in 0..512, stray-lane rejection |
| pack/unpack random | 307,024 | 20,000 random lengths 0..1000, both forms + serialize round trip, bad-trit and capacity rejection |
| Z3 | 866,314 | all 256x256 scalar inputs (valid vs mod-3 oracle and isomorphism, invalid rejected); 200,000 random Z3 blocks |
| errors leave outputs | 6,254 | every error path of every function with outputs prefilled with a sentinel and checked unchanged: bad trit / (1,1) block / bad byte / bad padding at fixed and 6,000 random positions (pack/unpack bitplane and dense), short buffers, serialize, block and Z3 ops, dots (stray lane, n > max), integer conversion, quantization; 14 wide ints (INT_MIN, -129, 256, 258, INT_MAX, ...) through every scalar function |
| absmean quantization | 116,993 | hand-computed vectors (exact ties, largest float below a tie, errors sqrt(1.5/6.5) and sqrt(3/11)); zero, NaN, Inf; denormal and FLT_MIN/FLT_MAX edges; rel_l2 argument rules; 20,000 ternary vectors (idempotence, scale); 20,000 dyadic vectors w_i = k_i*2^-e (half near FLT_MIN) against an exact integer oracle for scale rounding, the |w_i| >= scale/2 rule and the underflow rule; requantization (same q or the stated underflow) |

Fixed seed (splitmix64); runs are reproducible. The same suite passes under
`-fsanitize=address,undefined -fno-sanitize-recover=all`. A mutation check
(dropping one term from the add or mul formula, or accepting byte 243) makes
the suite fail, and so does restoring the old write-as-you-go unpack/pack
functions or the old quantizer (silent zero scale, unchecked rel_l2).

## Relation to the parked branch `experiment/ternary-semantics` (ae1e2a3)

That branch is parked and was not modified or merged. It was read for data
ideas only; this library is written fresh.

Agrees:
- Same per-trit meaning of the two planes (pos, neg; value = pos - neg).
- Same dense tryte rule: byte = sum (t_i+1)*3^i, LSB trit first, bytes < 243,
  padding trits must be zero. Bytes are interchangeable between the two.
- Same balanced digit convention (little-endian, digits in {-1,0,1}).

Differences and one conflict:
- **Conflict (invalid state):** the parked `omega_t_from_planes` decodes a
  (1,1) lane as 0 without checking; validity is a separate
  `omega_t_planes_valid` the caller may skip. Under H1 that is a silent
  misread. Here every decoder validates. Anything reusing the parked decoder
  must call the validator first.
- Container layout: parked packs 32 trits in one `uint64_t` (bits 0..31 pos,
  32..63 neg); here a block is 64 trits in two separate 64-bit planes. Same
  semantics, different memory layout: convert lane by lane, not by casting.
- Integer model: parked T32 words saturate symmetrically at +/-(3^32-1)/2;
  here conversion is exact over int64 and overflow is an explicit error
  (no saturation).
- Parked Kleene ops (min/max/xor), compare, shifts and the OMG1 object
  encoding are out of scope here.
