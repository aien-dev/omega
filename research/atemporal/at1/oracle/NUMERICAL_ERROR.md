# Numerical error documentation for the AT-1 reference oracle

Every record this oracle writes says `arithmetic BINARY64`, `bound_kind ESTIMATED`,
`threads 1`. ESTIMATED means the bounds below are first-order estimates backed by the
accounting and the measurements here. They are not proven enclosures. A RIGOROUS claim would
need directed-rounding interval arithmetic with enclosed `cos` and `sin` (AT1_SPEC section
8.4 item 6). Rust's standard library does not provide that and this oracle pulls in nothing
else, so it never claims it. A case that demands `min_bound_kind RIGOROUS` therefore fails
`bound_kind_sufficient` here by design (fixture `at1-n5-rigorous-demand`).

`U` below is `2^-53`, the unit roundoff of binary64 (half an ulp at 1).

## What is exact

- Every validity decision, both identities, the per-level spectrum test `|h + v_j|^2` a
  rational square, the kernel pairs `E_j + h0 + s R_j = 0`, the degenerate-matched level,
  the kernel dimension, the level eigenvectors `f`, the projected components
  `u_j = f (f^dagger psi_0) / ||f||^2` and `S = sum_j ||u_j||^2`: arbitrary-precision
  rationals and Gaussian rationals (`src/at1/big.rs`), no float involved.
- Every phase: the turn count `E_j (k - r) tau` (and `2 |h| (k - r) tau` when `|h|` is
  rational) is reduced modulo 1 exactly, split into the nearest eighth turn and a remainder
  in `[-1/16, 1/16]`; quarter turns are exact (AT1_SPEC section 8.4 item 1).
- Every acceptance check: decided on the written `f64:` tokens converted exactly to
  rationals, against the exact scaled-decimal tolerances and bounds. The judge never compares
  floats.
- Input conversion: every exact rational is converted to binary64 once, correctly rounded
  (round half to even on the exact quotient), so each input carries at most `U` relative
  error. A unit test checks the rounding against values with known bit patterns.

## Per-operation accounting (standard rounding model, first order)

| Ingredient | Error | Where |
|---|---|---|
| one phase value `cis_turns` (modulus 1) | `CIS_ERR = 8 U` absolute: remainder conversion and `2 pi r` (3 U relative on an angle of at most `pi/8`), `cos` and `sin` at most one ulp each, the binary64 `sqrt(1/2)` of odd eighths, one complex product (`sqrt(5) U`); about 7 U | `src/at1/model.rs` |
| clock state entry `t_j = cis / sqrt(N)` | `(CIS_ERR + 3) U` relative | matrix route |
| `x_j = t_j psi_0` | `(CIS_ERR + 7) U \|\|x_j\|\|` | matrix route |
| kernel block of `P_0` | `b b^dagger` with `b` a converted, normalized eigenvector: about `13.3 U` per entry, Frobenius norm 1; a degenerate-matched block is the exact identity | matrix route |
| `Psi_j = P_0 x` on one block | `\|\|dPsi_j\|\| <= (CIS_ERR + 28) U \|\|x_j\|\|`, `\|\|x_j\|\| = \|\|psi_0\|\| / sqrt(N)`; blocks outside the kernel are exact zeros | matrix route |
| `H_total` block entries | each 2 x 2 diagonal block `(E_j + h0) I + n_j . sigma`, `n_j = h + v_j`, is formed in exact rationals and converted once: `U` relative per entry, so a large `h` cancelling a large `v_j` costs nothing | matrix route |
| `phi_k = sum_j conj(t_j) Psi_j` | compensated (Neumaier) sum: `\|\|dphi\|\| <= sum_j \|\|dPsi_j\|\| / sqrt(N) + (CIS_ERR + 8) U sum_j \|\|Psi_j\|\| / sqrt(N)` (triangle inequality on `(\|Psi_j0\|, \|Psi_j1\|)`; the sum error no longer grows with `N`) | matrix route |
| POVM entry `(a, b)` | `sum_k (w / N) cis(-(E_a - E_b) k tau)`, one exact turn count per term, compensated over the `M` terms: `(CIS_ERR + 4) U w M / N` | matrix route |
| `chi_k = sum_j cis(E_j (k - r) tau) u_j` | compensated sum: `\|\|dchi\|\| <= (CIS_ERR + 6) U sum_j \|\|u_j\|\|` (conversion `U`, phase, product `sqrt(5) U`, compensation `2 U`) | closed form |

## Stated bounds

`SAFETY = 2` multiplies every estimate. The operation counts above are worst-case sums of
absolute values, so the factor only covers second-order terms and a platform `cos`/`sin`
that misses its ulp.

| Quantity | Route | Stated absolute bound |
|---|---|---|
| `constraint_residual` | matrix | `SAFETY gmax (dPsi / \|\|Psi\|\| + 8 U)`, `dPsi = (CIS_ERR + 28) U K \|\|psi_0\|\| / sqrt(N)` over the `K` kernel levels, `gmax = max 2 R_j` over the levels that carry a kernel pair (the norm of `H_total` on the blocks where `Psi` lives; a degenerate-matched block is an exact zero): the exact `Psi` has `H_total Psi = 0` |
| `povm_residual` | matrix | `SAFETY ((CIS_ERR + 4) U w M + 8 U value)` |
| `clock_probability k` | matrix | `SAFETY (w (2 \|\|phi_k\|\| dphi + dphi^2) / \|\|Psi\|\|^2 + p (2 dPsi / \|\|Psi\|\| + (4N + 12) U))` |
| `reference_clock_probability k` | closed form | `SAFETY (w (2 \|\|chi_k\|\| dchi + dchi^2) / (N S) + 16 U p)` |
| each `pauli` and `reference_interacting` value | matrix / closed form | with `a = asin(rel)`, `rel = dphi / \|\|phi_k\|\|` (or `dchi / \|\|chi_k\|\|`): `SAFETY (2 sqrt(P (1 - P)) a + 3 a^2 + 16 U)` |
| each `reference_ideal` value | Bloch rotation | `SAFETY (64 U + angle error)`; the angle error is 0 when `|h|` is rational (exact turn reduction) and `2 pi 8 U |2 |h| (k - r) tau|` otherwise |

The Pauli bound is per value. The ray of `phi + dphi` lies within the angle `a = asin(rel)`
of the ray of `phi`. `P = cos^2(beta)` has `|dP / dbeta| = 2 sqrt(P (1 - P))` and
`|d^2P / dbeta^2| <= 2`, so `|dP| <= 2 sqrt(P (1 - P)) a + a^2`. Evaluating `sqrt(P (1 - P))`
at the computed `P` moves it by at most `a`, which gives `2 s a + 3 a^2`. This is the
Cauchy-Schwarz sharpening of AT1_SPEC section 8.4 item 4 (`2 ||Delta chi|| / ||chi||`); it
gives exact zeros and ones (a state orthogonal to an eigenvector) the second-order bound
they actually have.

Bounds are written as scaled decimals at the twentieth decimal place, rounded up and then
increased by one further unit (`bound_token`), so the written bound is never smaller than
the float estimate. The rounding is exact big-integer arithmetic on the exact binary value of the
estimate, with no upper clip; a non-finite estimate is written as the largest binary64. A unit test checks this on exact rationals.

## Measured (fixtures, rustc 1.98.1 and 1.97.1, aarch64-unknown-linux-gnu, glibc libm)

| Measurement | Value | Where |
|---|---|---|
| Values compared with the exact AT1_SPEC section 13 tables, 21 cases | 1577 | `fixtures/CALIBRATION.md` |
| Largest deviation, matrix route (the record's own values) | `6.7e-16` (P3b, N1c) | same |
| Largest deviation, closed form (`reference_clock_probability`, `reference_interacting`) | `2.2e-16` | same |
| Largest deviation, Bloch rotation (`reference_ideal`) | `2.2e-16` (N4b, phases not quarter turns) | same |
| Values whose actual error exceeds the written bound | 0 | same |
| Largest written bound on any compared value | `1.1e-13` (P3b, label `p(2) = 1/80`) | same |
| `constraint_residual`, nontrivial kernels | `0` (P1, P1c, N3) to `9.5e-17`, P6 included (`delta_min = 2^-20`) | `*.values` |
| `povm_residual`, complete clocks | `0` on every one | `*.values` |
| `povm_residual`, N4a and N4b | `1` and `1.620185174601965` (`sqrt(42) / 4`) | `*.values` |
| Fixture bytes, rustc 1.97.1 build versus rustc 1.98.1 build | identical (`diff -r`) | both builds |
| Review case: `h_z = 1048573/15` cancelling `v_z = -1048567/15` (before the exact blocks the residual was `8.7e-12` against a bound of `2.6e-14`, reviewer measurement) | residual `6.8e-17`, bound `1.8e-14` | test `review_finding_1_...` |
| Review cases: `N = M = 64` and `N = 64, M = 256, w = 1/4` (before, `N = M = 64` turned INDETERMINATE, reviewer measurement) | constraint and POVM residuals `0`, POVM bound `1.7e-13`, constraint bound `1.8e-14`, PASS | test `review_finding_2_large_clocks` |

Observed errors sit two to three orders of magnitude below the stated bounds, and the
largest bound sits an order of magnitude below the tolerances `1e-12` of AT1_SPEC section
8.3.

## Limits

- Bounds grow like `1 / sqrt(p(k))` (AT1_SPEC section 8.4 item 4) and like
  `||psi_0|| / ||Psi||` on the matrix route. AT1_SPEC section 8.3 rule (b) keeps
  `p(k) >= 10^-3`; at that floor, on a case where the projection keeps only a small part of
  `psi_0`, the oracle's own verdict can turn INDETERMINATE (`PRECISION_INSUFFICIENT`) where
  a tighter engine would PASS. No section 13 case comes near this.
- The irrational-`|h|` angle error of `reference_ideal` is an estimate (AT1_SPEC section 8.4
  item 2 states it as a hypothesis); no section 13 case has an irrational `|h|`.
- Largest sizes the contract allows (`N = 64`, `M = 256`) are covered by the review cases
  above; the POVM bound grows like `w M` (`N` on a complete clock), still `1.7e-13` there.
- The Neumaier sums carry a second-order term of order `n U^2` for `n` terms, negligible for
  `n <= 256`, the largest sum here.
- `cos` and `sin` come from the platform libm; the accounting assumes at most one ulp each.
  Nothing here was measured on macOS.
