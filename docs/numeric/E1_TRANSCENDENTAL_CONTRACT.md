# E1 WP-B: FP32 transcendental contract (CPU tier, bounded)

Status: CPU tier implemented and checked against all 2^32 inputs per op.
GPU (GB10) realization: NOT_RUN. These ops are not registered in the
OMEGA_NOP table yet.

Sources: `src/omega_numeric_transc.c`, `src/omega_numeric_transc.h`.
Test: `tests/test_omega_transc.c`. Make targets: `test-numeric-transc`
(fast, about 1.1M inputs per op plus 10 mutants), `test-numeric-transc-full`
(exhaustive), `test-numeric-transc-digest` (frozen full-domain digests).

## 1. What the contract promises

For each op `f` and every FP32 input `x` (all 2^32 bit patterns):

1. **Fixed sequence.** The result is produced by the exact sequence of
   binary32 operations written in `omega_numeric_transc.c`: FADD, FSUB,
   FMUL, FMADD (fused, one rounding), FDIV and FSQRT (correctly rounded),
   integer and bit operations, exact comparisons, and exact int-to-float
   conversion of small integers. Every floating-point step is an explicit
   AArch64 instruction, so the compiler cannot contract, reassociate,
   constant-fold or widen it. No libm, no double, no long double.
2. **Environment.** FPCR: round to nearest even, no flush to zero, no
   default-NaN, no FEAT_AFP controls (`OMEGA_NUMERIC_FPCR_REQUIRED_CLEAR`).
   The test refuses to run otherwise.
3. **Determinism.** Same input, same output bits, every run, any thread
   count and order (checked: two full passes, different thread counts,
   reversed order, identical digests).
4. **Bounded error.** `d(f_seq(x), CR(f(x))) <= OMEGA_TRANSC_MAX_ULP_<OP>`
   for every finite result (Section 3), where CR is the correctly rounded
   FP32 value of the exact real function.
5. **Special values exact.** NaN in gives the canonical quiet NaN
   `0x7fc00000`; results that are NaN, +-inf or a zero match the correctly
   rounded result exactly, including the sign of zero.
6. **Parity target.** A realization on any other substrate must issue the
   same operations in the same order and reproduce the full-domain digests
   of Section 6 bit for bit.

## 2. Ops, algorithms, domains

| Op | Algorithm (frozen) | Domain and special behaviour |
|---|---|---|
| SIGMOID | `e^-abs(x) = m 2^k` (shared exp core); x >= 0: `1/(1 + m 2^k)`; x < 0: `(m/(1 + m 2^k)) 2^k` so the only rounding into subnormals is the last multiply | all x; x >= 18 -> 1; x <= -104 -> +0; +-0 -> 0.5; +inf -> 1; -inf -> +0 |
| TANH | `abs(x) < 0.5625`: odd Taylor to x^21 (Horner in x^2, FMADD); else `1 - 2/(e^(2abs(x)) + 1)`; sign restored by bit | all x; `abs(x) >= 9.5` -> +-1; +-0 -> +-0; subnormals return x |
| RSQRT | `y = 1/sqrt(x)` by FSQRT and FDIV (within 1 ulp), then an exact integer rounding step: move y to the neighbour while the midpoint test `N^2 X 2^s < 1` (128-bit integer) says the true value lies past the midpoint. Midpoints are never exact roots, so there are no ties. | correctly rounded for all x > 0 incl. subnormals (scaled by 2^24, result by 2^12, both exact); +0 -> +inf; -0 -> -inf; x < 0 -> NaN; +inf -> +0 |
| EXP2 | `k = nearest(x)` (1.5 2^23 add trick), `f = x - k` exact, `abs(f) <= 1/2`; `2^f = 1 + f p(f)` degree-8 Taylor in `ln2^n/n!`; `m 2^k` by two exact power-of-two multiplies (the last one rounds once into subnormals) | x >= 128 -> +inf; x < -151 -> +0; -inf -> +0; +-0 -> 1; x = -150 -> +0 (tie to even) |
| LOG2 | `x = m 2^e`, `m` in (0.707, 1.414]; `z = (m-1)/(m+1)` with its exact residue (2Sum, FMADD remainder); `log2 m = (2/ln2)(z + z^3/3 + ... + z^11/11)` with 2/ln2 split in two parts and the leading product kept exact; `e + log2 m` by 2Sum | x > 0 incl. subnormals (scaled by 2^23 exactly); +-0 -> -inf; x < 0 -> NaN; +inf -> +inf; 1 -> +0 |
| ERF | `abs(x) < 0.5`: `(2/sqrt pi) x (1 + x^2 q(x^2))`, q degree 6, 2/sqrt(pi) split in two parts; `0.5 <= abs(x) < 4`: `1 - erfc(abs(x))`, `erfc(y) = e^(-y^2) erfcx(y)`, `y^2` exact as two floats into the exp core, `erfcx` from 17 degree-10 Taylor pieces on [0.5, 11); sign restored by bit | all x; `abs(x) >= 4` -> +-1; +-0 -> +-0 |
| SIN, COS | `abs(x) <= pi/4`: no reduction; else `k = nearest(x 2/pi)`, `r = x - k pi/2` with pi/2 split in three (24 + 24 + 24 bits), the first product exact, the second as an exact 2Prod, carried as `rh + rl`; quadrant `(k + [cos]) mod 4` picks `sin_poly` (to r^13) or `cos_poly` (to r^14, `1 - r^2/2` kept exact) and the sign | **admitted domain `abs(x) <= 2^22`** (`OMEGA_TRANSC_TRIG_MAX_ABS`). Outside it, and for +-inf and NaN, the result is NaN (`0x7fc00000`). sin(+-0) = +-0; cos(+-0) = 1 |
| GELU (erf form) | `x Phi(x) = (x/2)(1 + erf(x/sqrt 2))`. `abs(y) < 0.5`: `x/2 + (x/2) erf_small(y)`; else `x^2/2` exact, `erfc(abs(y)) = e^(-y^2) erfcx(abs(y))` with the rounding of `y = x/sqrt2` put back to first order through `erfcx' = 2y erfcx - 2/sqrt pi`; `e^(-y^2)` and `erfcx` each carried as a high and a low float (the last rounding recovered by FMADD, the low part of the erfcx constant term from a second table) and multiplied with an exact 2Prod; x >= 0: `x - (x/2) erfc(y)`; x < 0: `(x/2) erfc(abs(y))`, one rounding into subnormals | all x; x >= 8 -> x (incl. +inf); x < -15.5 -> -0 (incl. -inf); +-0 -> +-0; the result always has the sign of x |

Why the domain limit on SIN/COS: the three-part pi/2 reduction is exact
enough only while `k` fits 22 bits. A Payne-Hanek reduction for the full
range is deferred; the contract states NaN instead of an unchecked value.

Saturation thresholds (18, 104, 9.5, 128, 151, 4, 8, 15.5) are points past
which the exact function rounds to the stated constant; the reasons are
in comments next to each threshold and the exhaustive check confirms each.

### Porting notes (GPU parity)

- Division and square root must be the IEEE correctly rounded forms
  (PTX `div.rn.f32`, `sqrt.rn.f32`), never `rcp`/`rsqrt.approx` or fast-math
  division; denormals preserved (`.ftz` off); FMADD as `fma.rn.f32`; no
  contraction of separate multiply and add.
- The RSQRT rounding step uses a 128-bit integer product (`N^2 X < 2^75`);
  a GPU port computes it with 64-bit multiply-high pairs. It is pure
  integer arithmetic, so parity does not depend on the FP unit.
- `scale2` is only ever called with a normal `m`; with a subnormal `m` it
  could round twice.

## 3. Error metric (ulp)

`ord(b)` maps FP32 bits to integers: `ord(b) = b` for sign 0 and
`ord(b) = -(b & 0x7fffffff)` for sign 1. Both zeros map to 0, the smallest
subnormals to +-1, and consecutive representable values to consecutive
integers, across the subnormal/normal boundary and through zero.

`d(s, o) = |ord(s) - ord(o)|`, where `o` is the exact result rounded once
to FP32 (round to nearest even, with gradual underflow). So `d` counts the
representable FP32 values between the two results: `d = 0` is correctly
rounded; `d = n` is n units of the result's binade (2^-149 in the subnormal
range). Near zero the metric does not blow up: a result off by one
subnormal step is `d = 1`. Special cases are not measured by `d`: NaN
results must be `0x7fc00000` exactly when the exact result is NaN, infinities
must match exactly, and a zero result must match the correctly rounded
zero including its sign. Any special mismatch fails the test.

## 4. Declared bounds

Declared maximum = observed maximum over all 2^32 inputs + a margin of 1
ulp (0 for RSQRT, which is correctly rounded by construction and observed
so). The test fails if any input exceeds the declared bound.

| Op | Observed max ulp (exhaustive) | Declared max ulp | Inputs with d > 0 |
|---|---|---|---|
| SIGMOID | 2 | 3 | 125,485,046 (196,747 at 2) |
| TANH | 2 | 3 | 6,116,832 (14 at 2) |
| RSQRT | 0 | 0 (correctly rounded) | 0 |
| EXP2 | 1 | 2 | 8,754,037 |
| LOG2 | 1 | 2 | 197,064 |
| ERF | 2 | 3 | 22,917,730 (2,290 at 2) |
| SIN (`abs(x) <= 2^22`) | 1 | 2 | 6,324,452 |
| COS (`abs(x) <= 2^22`) | 1 | 2 | 5,681,590 |
| GELU | 2 | 3 | 9,208,815 (1,993 at 2) |

The fast target (`make test-numeric-transc`) checks about 1.1M inputs per
op: 2^20 multiplicative-hash samples of the full bit space, +-64 ulp around
56 thresholds and special points (both signs), and NaN/inf/zero/subnormal
edges. The exhaustive target checks every input.

## 5. The oracle

The reference is an independent IEEE binary128 evaluation (`long double`
on AArch64 Linux: 113-bit significand, software arithmetic from the
compiler runtime, no libm) in `tests/test_omega_transc.c`. It shares no
code, constants or algorithms with the sequences:

- exp: `k = nearest(a/ln2)`, Taylor of `e^r - 1` on `r/256` run to
  2^-120, then 8 squarings in the form `e(2 + e)`; 2^k built from bits.
- log2: a table of `log2(1 + j 2^-23)` for all 2^23 mantissas from the
  atanh series run to 2^-120 (no sqrt(2) split).
- 1/sqrt: a table of the FP32-rounded `1/sqrt(v)` for all 2^24 (mantissa,
  exponent parity) pairs, from a double seed and three binary128 Newton
  steps; exact power-of-two rescale.
- erf: alternating Maclaurin series below 0.5, the all-positive Kummer
  series `e^(-y^2) sum y (2y^2)^n / (2n+1)!!` up to 4.5; erfc beyond 4.5 by
  the continued fraction evaluated backwards from depth 240.
- sin/cos: `x - k PA - k PB - k PC` with pi/2 = PA (80 bits, so `k PA` is
  exact for `k < 2^33`) + PB + PC, then Taylor series run to 2^-120.
- GELU: `(x/2) erfc(-x/sqrt 2)`; tanh: `expm1(2x)/(expm1(2x) + 2)` below
  0.5, else `1 - 2/(e^(2x) + 1)`.

Each value is rounded once to FP32 by the runtime conversion. Shortcuts
with their justification (the omitted terms are far below half an ulp):
`|x| < 2^-40` uses the first two or three series terms (next term below
2^-80 relative); sigmoid `x >= 40` -> 1, `x <= -110` -> 0; tanh `|x| >= 12`
-> +-1; erf `|x| >= 4.5` -> +-1 (erfc(4.5) < 2^-32); GELU `x >= 16` -> x,
`x <= -16` -> -0; EXP2 integer x -> exact `2^x`.

Self-checks run before every oracle mode (identities `e^ln2 = 2`,
`e^1 e^-1 = 1`, exp/log inversion, `sin^2 + cos^2 = 1`, `sin(pi/6) = 1/2`,
large-argument reduction, `rsqrt(2)^2 2 = 1`, erf(1) and erfc(5) known
values, continued fraction depth 240 vs 480, continued fraction vs
`1 - erf` at 4.5, series seam at 0.5). Any failure fails the run.

Hard-to-round inputs: a binary128 value within 2^-113 relative of an FP32
midpoint could round the wrong way. The sequences' errors are reported
against the oracle; a mis-rounded oracle could at most shift a measured
distance by 1, which the margin covers.

## 6. Determinism and parity digests

Full-domain digest per op: `D = sha256(C_0 .. C_255)`,
`C_c = sha256(B_c,0 .. B_c,1023)`, `B_c,b = sha256` of the 2^14 outputs
(FP32 bits, little-endian) for inputs `c 2^24 + b 2^14 ..` in increasing
order. The exhaustive run computes them twice (8 threads forward, 5 threads
reversed) and requires equality; `make test-numeric-transc-digest`
recomputes them from the sequences alone and compares them with the frozen
values in the test.

| Op | Full-domain digest (sha256) |
|---|---|
| SIGMOID | `b4e43cb75fb625314aa464a4b4cffd7bc398d75cb7a50ff23af786025af1886d` |
| TANH | `932c09ffe7f02125dc302bfe9c3666a4866b109627fd89cfca0827cf3b1e5daa` |
| RSQRT | `454fc9d5fb2e640acc8ba48422c3ae5e61d17fcee8cec20091cfb4a044ec51e4` |
| EXP2 | `227cb65782077d8224121eca527f8beba5d9fc80648ee57c38895466a8345bd8` |
| LOG2 | `fdf21ee2f13fa4ffec0c5c5ea1af4e1650ff9dbd91b138a608a68b43670a07f3` |
| ERF | `aef63f62db823726c45f88b6605263d614854595679edcb17c3a4492abc74aba` |
| SIN | `9e4debc75f832121cea47e9fad2e008329ccc62b291c3b68e6f60bb3f908cddc` |
| COS | `28d580ee2efe1256397cc8a3b2dcd4bfe397100a0f9663e1c010baa136dcceb3` |
| GELU | `5663383b26d1294963512e95290ec7a6cde61accde97ca8767a9f1d97f4fc727` |

## 7. Mutation evidence

`-DOMEGA_TRANSC_MUTATE=<id>` flips mantissa bit 12 (relative 2^-11) of one
coefficient per op (EXP2, LOG2, shared exp core, TANH, ERF small, one erfcx
piece, SIN, COS, GELU's 1/sqrt2) or, for RSQRT, removes the exact rounding
step. `make test-numeric-transc` builds all 10 mutants and requires each to
FAIL the bound check; it does.

## 8. Coefficients

All coefficients are frozen FP32 values written as exact hex literals
(24-bit integer times a power of two). They were computed with 60-digit
`bc` arithmetic and rounded once to FP32: Taylor coefficients of `e^r`,
`ln2^n/n!` for 2^f, odd Taylor coefficients of tanh (Bernoulli form), of
erf, sin and cos, the atanh series `2/((2n+1) ln2)` for log2, and the
erfcx Taylor coefficients about 17 centres from the exact recurrence
`(n+1) a_(n+1) = 2c a_n + 2 a_(n-1)` (with `-2/sqrt(pi)` in the first
step). Split constants (ln2, 2/ln2, 2/sqrt(pi), pi/2, 1/sqrt2) are given as
a high FP32 part and the FP32-rounded remainder. No coefficient was tuned
against the test.

## 9. Not done

- GB10 realization and parity run: NOT_RUN (this work package is the CPU
  tier only).
- SIN/COS beyond `|x| <= 2^22`: not admitted (NaN by contract).
- OMEGA_NOP registration: not done (out of scope for WP-B).
