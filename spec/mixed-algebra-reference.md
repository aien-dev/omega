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
byte >= 243 or non-zero padding), `OMA_E_INVALID_Z3`, `OMA_E_ARG`.
`oma_strerror(rc)` names them. On error, outputs are left untouched.

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

`oma_quant.h`: `oma_quant_absmean`, `oma_quant_rel_l2`.

## Exact vs approximate

Exact (bit-for-bit, integer-defined): all trit, block, dot, Z3, integer
conversion and pack/unpack operations.

**Approximate:** `oma_quant_absmean` (BitNet b1.58 style). It is a lossy
transform, not a realization of the float values:
scale = mean|w_i| (summed in double, stored as float);
q_i = clamp(round(w_i / scale), -1, 1), round half away from zero. All-zero
input gives scale 0 and q = 0; NaN/Inf input is rejected. No epsilon is added
(BitNet adds a tiny eps; here the zero case is explicit). The loss is measured
by `oma_quant_rel_l2` = ||w - q*scale||_2 / ||w||_2 (double). Properties that
do hold exactly: q of already-ternary input equals the input (scale = fraction
of non-zero entries); re-quantizing q*scale returns the same q.

## Test coverage (one run: 25,709,261 checks, 0 failures)

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
| absmean quantization | 160,046 | two known vectors with exact scale/q/error; zero, NaN, Inf; 20,000 random vectors: ternary idempotence, scale = mean abs, q formula, requantization idempotence, finite error |

Fixed seed (splitmix64); runs are reproducible. The same suite passes under
`-fsanitize=address,undefined -fno-sanitize-recover=all`. A mutation check
(dropping one term from the add or mul formula, or accepting byte 243) makes
the suite fail.

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
