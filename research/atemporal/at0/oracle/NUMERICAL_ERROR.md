# Numerical error documentation for the AT-0 reference oracle

Every record this oracle writes says `arithmetic BINARY64`, `bound_kind ESTIMATED`,
`threads 1`. ESTIMATED means: the bounds below are stated numerical estimates backed by
the analysis and the measurements here, not proven enclosures. A RIGOROUS claim would need
directed-rounding interval arithmetic with enclosed `cos`, `sin` and `exp`; Rust's
standard library does not provide that and this oracle pulls in nothing else, so it does
not claim it. A case that demands `min_bound_kind RIGOROUS` therefore fails
`bound_kind_sufficient` here by design (fixture `n5-rigorous-demand`).

`eps` below is `2^-52` (about `2.2e-16`), the binary64 unit roundoff for values of order 1.

## What is exact

- Every validity decision, both identities, the kernel pairs and the kernel dimension:
  exact rational arithmetic on `i128` numerators and denominators (`src/at0/rational.rs`),
  no float involved. Inputs are bounded by the contract (`|n|, d <= 1048576`, `N <= 64`), so
  no intermediate exceeds `i128`.
- Every acceptance check: decided on the written `f64:` tokens converted exactly to
  fixed-point big integers with unit `2^-1074 * 10^-40` (`src/at0/exact.rs`), against exact
  scaled-decimal tolerances and bounds (AT0_RESULT_V2 section 4). The checker never
  compares floats. Unit tests confirm `0.5 == 5@1`, `0.1 > 1@1` and the smallest denormal.
- Input conversion: every case rational is converted once as `n as f64 / d as f64`; with
  `|n|, d <= 2^20` both operands are exact and the quotient is correctly rounded, so each
  input carries at most `eps/2` relative error. The ideal fixtures use dyadic rationals
  (`-3/2`, `1/4`, ...), which convert exactly.

## Stated bounds

| Quantity | Path | Stated absolute bound | Reasoning |
|---|---|---|---|
| `constraint_residual` | matrix | `8 (N + M) eps` | `H_total Psi` is a sum of at most `2N` products of inputs of size `max|E|, |h|` by amplitudes of size `<= 1`; for the ideal cases every product is exactly representable and the result is exactly `0`. The factor `8 (N + M)` covers the matrix-vector sums and the normalization. |
| `povm_residual` | matrix | `8 (N + M) eps` | `M` rank-one terms of size `w/N`, each with error about `eps` per complex exponential (`cos`/`sin` from the platform libm, accurate to a few ulp), summed in a fixed order, then a Frobenius norm. Absolute error grows at most linearly in `M`. |
| `clock_probability k` | matrix | `8 (N + M) eps` | `||phi_k||^2 / ||Psi||^2` with `phi_k` an `N`-term sum; error linear in `N`. |
| `pauli k AXIS SIGN` | matrix | `8 (N + M) eps` | `Tr(rho_k Pi)` with `rho_k` normalized by `||phi_k||^2`; two `2 x 2` products on top of the `phi_k` error. |
| `reference k AXIS SIGN` | analytic | `64 eps (1 + |t| (|h0| + |h|))` | one `cos`, one `sin`, one unit-modulus phase of argument up to `|h0 t|`, a handful of complex multiplies; the argument-size term covers the absolute error of trig functions at large arguments. |

Bounds are written as scaled decimals at the twentieth decimal place, rounded up and then
increased by one further unit (`bound_token`), so the written bound is never smaller than
the float estimate even if the decimal scaling itself rounded.

At the contract maximum `N = 64`, `M = 256` the matrix bound is `8 * 320 * eps`, about
`5.7e-13`, under the frozen example's tolerances `1@12`. For the ideal fixtures
(`N = M = 4`) it is `1421087@20`, about `1.4e-14`.

## Measured errors (fixtures, rustc 1.98.1, aarch64-unknown-linux-gnu, glibc libm)

| Measurement | Value | Where |
|---|---|---|
| Largest deviation of any matrix-path or analytic-path Pauli probability from its hand-derived exact rational, over the 12 fixtures that carry expectations (N in 3..8, offsets, phases, spec section 13 tables incl. the tilted-field P2 table) | `2.2e-16` | `fixtures/CALIBRATION.md` |
| Same for the clock probabilities `1/N` | `<= 2.2e-16` | same |
| `constraint_residual`, ideal cases | exactly `0` | `*.values` |
| `constraint_residual`, tilted field (`p2-tilted-3-10-2-5`, `p2-h0-shift`) | `9.8e-18` | `*.values` |
| `povm_residual`, `N = 4` | `2.6e-16` | `*.values` |
| `povm_residual`, `N = 8` | `1.8e-15` | `*.values` |
| Agreement between the three independent routes (matrix, Schrodinger closed form, Bloch rotation) | `<= 1e-12` asserted, `~1e-16` observed | tests |
| Agreement between the vector route and the density-matrix route inside the matrix path | `<= 1e-12` asserted | tests |
| Bit-for-bit agreement of every `f64:` value token with the earlier Python/CPython 3.12 version of this oracle on the same machine | all 21 fixtures of the first release identical | checked with `cmp` before that version was removed |

Observed errors sit one to two orders of magnitude below the stated bounds and three to
four below the `1@12` tolerances. The margin is intentional: ESTIMATED bounds must not be
tight.

## Known limitations

1. **Portability of the last bits.** `f64::cos`, `f64::sin` and the derived phases come
   from the platform C library. Two machines may differ in the last bit of a `reference` or
   `pauli` token, which changes `verdict_id`. The verdict outcome does not change unless a
   value sits within one ulp of a tolerance edge, which none of the fixtures does.
2. **ESTIMATED, not RIGOROUS.** The bounds are analysis plus measurement, not interval
   enclosures.
3. **Dense arithmetic.** `O(N^2 M)` complex operations; at the contract maximum this is
   about a million operations, well under a second, and the dimension limits are enforced by
   the parser.
4. **`i128` rationals.** Exact arithmetic relies on the contract's `1048576` limit on
   numerators and denominators; the parser refuses anything larger, so overflow cannot be
   reached from a valid case.
