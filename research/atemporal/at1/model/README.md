# AT-1 candidate engine (Agent 3)

The candidate relational quantum engine for AT-1, tracked on aien-dev/omega#371. It reads one `AT1_CASE_V1` case, validates it, builds the literal constraint matrix `H_total = H_C (x) I_S + I_C (x) H_S + V`, finds its exact nullspace, applies the covariant clock POVM and writes one `AT1_RESULT_V1` result.

Portable C11, libc and libm only (plus the repository's own `src/sha256.c`). No Python, no clock, no randomness, no threads, no network. A PASS from this engine means conformance with the frozen contracts and `AT1_SPEC.md`; it says nothing about physics.

Contracts (aien-architecture `docs/plans/atemporal`, main `ea91d7c`): `AT1_CASE_V1.md` (sha256 `e62018d8...`), `AT1_RESULT_V1.md` (sha256 `3e6efd7e...`), `AT0_RESULT_V2.md` sections 2, 4, 5, 7 (sha256 `bd0f9eb8...`), `AT1_SPEC.md` (merged at `81047f5`), charter readings (c), (d), (e).

## Build and run

```
make                 # build/at1-model and build/test_model
make test            # every test below; exit status 0 only if all pass
make asan            # the same tests under -fsanitize=address,undefined, if the compiler supports it
make CC=clang test   # another compiler
```

The Makefile is plain POSIX make (works with GNU Make 3.81 on macOS). Flags: `-std=c11 -O2 -ffp-contract=off -Wall -Wextra -Werror -pedantic`. `-ffp-contract=off` keeps compilers from fusing multiply-adds, so gcc and clang round the same way. Without make:

```
cc -std=c11 -O2 -ffp-contract=off -I../../../../src -o at1-model \
   at1_bn.c at1_case.c at1_exact.c at1_numeric.c at1_result.c at1_io.c ../../../../src/sha256.c at1_main.c -lm
```

### Command line

```
at1-model result <case> <oracle-record|none> <provenance> [--reversed]
at1-model values <case> [--reversed]
```

- `result` writes the complete `AT1_RESULT_V1` file to standard output.
  - The `reference_label`, `reference_clock_probability`, `reference_ideal` and `reference_interacting` lines are copied verbatim from the oracle record (charter reading (c)): every line whose key is one of those four, in the contract order. If the record has a `begin values` block, only lines inside it count.
  - `case_id` / `acceptance_id` lines in the record must match the case. Count, order, index, label, axis, sign and token grammar are checked.
  - The placement of `undefined` is checked against section 1: `reference_ideal` is never `undefined`; the six `reference_interacting` lines of a label are `undefined` exactly when its `reference_label` is not `DEFINED`; `reference_clock_probability` is `undefined` only in the trivial shape (all labels at once, every `reference_label` `UNDEFINED`); and every written `reference_label` must follow the status rule applied to its own `reference_clock_probability` value and bound. A record that breaks any of these is refused (exit 1), so a blanked-out record can never reach the judge.
  - `none`, or an oracle record file that cannot be read, writes an `ERROR` result with `error_code ORACLE_UNAVAILABLE`, with the oracle provenance fields set to `none`.
- The case is parsed and validated before the oracle record and the provenance file are opened, so a refusal is never masked by a missing file.
- The provenance file holds the provenance lines in contract order, `source_repo` through `run_finished_utc`, then optional `artifact` lines. The runner supplies them, so the engine never reads a clock. The writer refuses `build_cc` starting with `oracle ` and `engine_sha256` equal to `oracle_sha256` (oracle-written records are never candidate results).
- `values` prints only the engine's own values lines. It is component output for debugging, not a contract file.
- `--reversed` evaluates the labels in reverse order. The output is bit-identical (test T8).
- Exit status:
  - `0`: written.
  - `2`: case refused. Exactly one line `AT1_CASE_REFUSED <code>` on stderr, nothing on stdout.
  - `1`: engine error. One line `AT1_ENGINE_ERROR <reason>` on stderr, nothing on stdout. Causes: an unreadable case or provenance file, a malformed or inconsistent oracle record, a malformed provenance file, or an engine limit hit while parsing.
  - A failure after the case was accepted (an internal consistency check) is written as an `ERROR` result with `INTERNAL_ERROR`.

## Method

| File | Role |
|---|---|
| `at1_bn.c` | Arbitrary-precision integers, reduced rationals, Gaussian rationals; correctly rounded rational to binary64. |
| `at1_case.c` | `AT1_CASE_V1` parser. Rules 1 to 6 of section 4 in order, first failure wins: shape, then non-canonical forms (decided after the whole shape pass), versions, ranges, per-level rational spectrum (exact integer square roots of the reduced `\|h + v_j\|^2`), identities, acceptance codes. Readings (d) (version lines: key in step 1, value in step 2) and (e) (index tokens compared by value; `01` is `CASE_NONCANONICAL`) are applied. |
| `at1_exact.c` | Builds `H_total` term by term with explicit Kronecker products as a `2N x 2N` matrix of exact Gaussian rationals, assuming no block structure. Exact Gauss-Jordan elimination gives rank and kernel basis `B`. Checks `H_total B = 0` exactly. Forms `P_0 = B (B^dagger B)^-1 B^dagger` exactly. Computes `y_j = P_0 (\|E_j> (x) psi_0)` exactly. Exact zero test: supports of the nonzero `y_j` checked pairwise disjoint, then `Psi = 0` iff every `y_j = 0`. |
| `at1_numeric.c` | Binary64 physics (formulas below). |
| `at1_result.c` | Oracle splice, provenance checks, the 12 checks in exact rational arithmetic on the written tokens, the writer, `verdict_id`, `evidence_digest`. |
| `at1_io.c`, `at1_main.c` | File reading (the only file access) and the tool. |

### The projected state

The brief asks for the projection from the literal matrix, independent of the oracle's closed form. `AT1_SPEC.md` 8.4 item 3 warns that a *numerical* projector (eigen-decomposition with a threshold) loses accuracy like `1/delta_min` and fails P6. This engine does neither:

- The kernel and `P_0` are exact, from the literal matrix.
- `Psi = P_0 (|t_r> (x) psi_0) = sum_j <E_j|t_r> y_j` by linearity. Each entry of `Psi` is one clock amplitude times one exact `y_j` entry rounded once.
- So there is no `1/delta_min` loss (P6 measures like every other case) and no cancellation, even when `psi_0` is nearly orthogonal to the kernel (control C1).

The closed forms of `AT1_SPEC.md` sections 2 to 4 appear only in the tests, as the expected answers.

### Numerics

| Quantity | How it is computed |
|---|---|
| Clock amplitudes `<E_j\|t_k>` | Phase `exp(-2 pi i E_j k tau)` from the turn count `E_j k tau`, reduced modulo 1 exactly in int64 (8.4 item 1), then `cos`, `sin` and `N^(-1/2)`. |
| `phi_k` | `(<t_k\| (x) I) Psi` (the bra conjugates the coefficients, phase `exp(+2 pi i E_j (k - r) tau)` overall). |
| `p(k)` | `w \|\|phi_k\|\|^2 / \|\|Psi\|\|^2`. |
| Each Pauli outcome | Its own trace `Tr(rho_k Pi)` against its own projector `Pi`, never `1 - other`. |
| `constraint_residual` | `\|\|H_total Psi_hat\|\|` with the literal matrix (rounded once). |
| `povm_residual` | `\|\|sum_k w \|t_k><t_k\| - I\|\|_F`, pairwise summation over `k` for each entry, then pairwise summation of the `N^2` squared moduli. |

All bounds are `bound_kind ESTIMATED`: first-order operation counts with a safety factor 2, not a proof. They assume `cos`, `sin` and `sqrt` within a few ulp.

Symbols:
- `eps = 2^-53`.
- `L` = number of clock levels that carry `Psi` (exact).
- `c = L + 12`.
- `delta = 16 eps` (relative error of `Psi`: one rounded `y_j`, `cos`/`sin`, `N^(-1/2)`, one complex product, about 8 eps, times 2).
- `eta = sqrt(L/N) (delta + c eps)` (error of `phi_k` relative to `||Psi||`: the `L`-term sum and the conjugated amplitudes).
- `Hpsi` = the largest row sum of `|H_total|` over the columns where `Psi` is nonzero.

| Quantity | Bound |
|---|---|
| `constraint_residual` | `2 [Hpsi delta + 8 eps (Hpsi + 1)] + 4 eps value` |
| `povm_residual` | `2 [(ceil(log2 M) + 8) eps (M w + 1) + (2 ceil(log2 N) + 4) eps value]` |
| `clock_probability` | `2 [2 sqrt(w p) eta + (2 delta + (2L + 12) eps) p] + 4 w eta^2` |
| `pauli` | `2 [2 eta sqrt(w / p) + 16 eps]` |

The `p` and Pauli forms follow 8.4 item 4: an error `Delta phi` moves `p` by about `2 sqrt(w p) |Delta phi| / ||Psi||` and a conditional probability by about `2 |Delta phi| / ||phi_k||`.

Each bound is written as the smallest scaled decimal with at most 7 significant digits that is at least the binary64 bound. That `>=` is verified exactly. Minimality is tested: one unit less in the seventh significant digit is below the binary64 bound. The label status rule (section 3) is decided exactly on the binary64 value and the bound as written.

The judge compares the written tokens in exact rational arithmetic, never in floating point:
- Absolute form: PASS if `|v| + b <= tol`, FAIL if `|v| - b > tol`, else INDETERMINATE. Check 7 uses the signed form.
- Check 10 uses the exact rational `w/N` as the ideal clock marginal.
- Aggregation is per-input `NOT_EVALUATED`. Any INDETERMINATE check or label adds `PRECISION_INSUFFICIENT`.

### Measured (this host only)

Host: Linux aarch64 (DGX Spark), gcc 13.3.0, `make test`, 2026-10-09. Every number below comes from that run.

| Quantity | Measured | Ratio to its own bound |
|---|---|---|
| `\|p(k) - exact\|`, all section 13 cases and controls | at most `1.78e-16` | at most `0.0129` |
| `\|pauli - exact\|`, all DEFINED labels | at most `3.89e-16` | at most `0.00973` |
| `constraint_residual` | at most `5.72e-17` | at most `0.007` |
| `povm_residual`, complete clocks | at most `4e-16` | |
| `povm_residual`, N4a and N4b | `1` and `sqrt(42)/4` within their bounds | |
| Large case N = 64, M = 256 | `sum p - 1 = 0`, `constraint_residual 0` (bound `8.17e-15`), `povm_residual 1.33e-15` (bound `2.31e-13`) | |
| Review reproducer N = 64, M = 1, `w = 1/3`, exact `sqrt(571)/3` | value `7.9652020968990138`, equal to the binary64 `sqrt(571.0)/3.0`; inside `[exact - bound, exact + bound]` in exact arithmetic, bound `3066656@20` | |

For comparison, `AT1_SPEC.md` 8.4 item 5 reports `5.6e-16` from its scratch builds. The bounds are 75 to 150 times the largest errors seen. The whole test program runs in about 0.1 s on this host.

## Tests (`make test`)

`tests/test_model.c` currently passes 1390 assertions, 0 failures. Clean under `-fsanitize=address,undefined` with leak detection (`make asan`, gcc 13.3).

1. **Worked example** `tests/cases/at1-kat-rotated-level-n4.case`, byte-identical to `AT1_CASE_V1` section 6.
   - File sha256 `76e282fe...`, `case_id 890980a4...`, `acceptance_id d63246c3...`.
   - The test generator reproduces the file byte for byte.
   - Kernel dimension 2, `p = 5/28, 1/4, 9/28, 1/4`.
2. **Every `AT1_SPEC.md` section 13 class**, generated with recomputed identities:
   - Positive: P1a, P1b, P1c, P2, P3, P3b, P4, P5, P6.
   - N1 to N1f: the positive cases with target IDEAL, giving `SCHRODINGER_DEVIATION_EXCEEDED`.
   - Negative: N2, N3, N4a, N4b, N5, N6.
   - Each one checks:
     - the kernel dimension;
     - every `p(k)` and Pauli value against the exact table, `|value - exact| <= written bound`, in exact arithmetic;
     - label status and the trivial-kernel rule;
     - a full result written with an oracle record built from the exact tables;
     - the 12-check vector, outcome, failure codes and `expectation_met YES` against section 13;
     - line count, byte range, `verdict_id` and `evidence_digest`.
3. **Controls added here:**
   - C1: `psi_0` nearly orthogonal to the only kernel vector. PASS, exact values met.
   - C2: energies shifted by 1000, cancelled by `h0`. P2's table, PASS.
   - C3: a kernel level with `|E_j + h0| = R_j = 1025/2` on a rotated axis. Check 4 INDETERMINATE, `PRECISION_INSUFFICIENT` (the declared limit below, shown working).
4. **Refusals:** 67 files with exact codes.
   - R0 to R14 of section 13.5, including R2b, R2c, R3b, R5b, R6b, R12b.
   - All 42 inherited AT-0 rows re-expressed on P2 (R01 to R39, R06b to R06d, CRLF, missing final LF, empty file).
   - Readings (d) and (e): `OMEGA-AT0-CASE v1` gives `CASE_PARSE_ERROR`; `domain omega.at0.case.v1` gives `CASE_UNSUPPORTED_VERSION`; index `01` gives `CASE_NONCANONICAL`; index `x` or `5` gives `CASE_PARSE_ERROR`.
   - A 5001-digit token is an engine limit error, not a refusal.
5. **Invariance (spec section 9.1)**, on P2 and P4:
   - T4 phase times `i`; T5 scale times 2; T6 purity;
   - T7 shift of `r` by one shifts the labels;
   - T8 reversed order bit-identical; T9 second run bit-identical.
6. **Large case:** N = 64, M = 256, two kernel levels, full result PASS.
7. **Judge and tool rules:**
   - Status disagreement gives check 12 FAIL; an INDETERMINATE reference label gives `PRECISION_INSUFFICIENT`.
   - A wrong oracle marginal gives `INTERACTING_DEVIATION_EXCEEDED`; a wrong `reference_interacting` under IDEAL gives check 11 `ORACLE_DISAGREEMENT`; a `nonfinite` reference gives `NONFINITE_VALUE`.
   - Malformed oracle records are rejected: missing line, wrong label, `-0.0`, foreign `case_id`.
   - Refused provenance: `build_cc oracle ...`, equal engine and oracle digests, duplicate artifact path, bad timestamp.
   - `ORACLE_UNAVAILABLE` and `INTERNAL_ERROR` result shapes.
   - Through the CLI: output equals the library output, `--reversed`, `none`, refusal exit 2 with the exact stderr line, bad provenance exit 1 with empty stdout.
   - Oracle `undefined` placement: one clock line undefined, a status that contradicts its own clock value, an all-`undefined` record with `DEFINED` labels, a status change without its lines, `reference_ideal` undefined, `reference_interacting` undefined on a `DEFINED` label: all refused. A consistent trivial-shape oracle record against a nontrivial engine is accepted and gives check 12 FAIL.
8. **Independent review findings** (46 assertions; 28 of them fail on the pre-review code, all pass now):
   - The review reproducer above for the POVM bound (the pre-review code was 58 times over its bound).
   - Minimal bound tokens: `1` is `1@0`, `0.25` is `25@2`, plus the minimality property on 12 values from `3e-41` to `123456789`.
   - Label status rule at its exact boundaries (`p + b == tol` is `UNDEFINED`, `p - b == tol` is `INDETERMINATE`).
   - Absolute-form boundary on check 10: `|d| + b == tol` is PASS; one ulp more is INDETERMINATE with `PRECISION_INSUFFICIENT`.
   - An oracle record that is a full result file: reference-like lines outside its `begin values` block are ignored.
   - Through the CLI: an unreadable oracle record gives `ERROR ORACLE_UNAVAILABLE` with exit 0; a refused case with unreadable oracle and provenance still exits 2 with its refusal line; an unreadable provenance file exits 1.

## Declared limits

- **`bound_kind ESTIMATED`, not RIGOROUS.** Cases with `min_bound_kind RIGOROUS` fail check 1 by design (N5).
- **Large matrix entries.** `constraint_residual` is evaluated in binary64. When `|H_total|` acting on `Psi` has row sums `Hpsi` above about 187 (where `48 eps Hpsi` reaches `1e-12`), its bound exceeds `tol_constraint_residual 1@12`, and check 4 is INDETERMINATE (`PRECISION_INSUFFICIENT`) even though the measured residual stays small. C3 (`Hpsi` near 1230) shows this. A shift common to `E_j` and `h0` does not count, because the matrix entries are exact sums (C2).
- **Small marginals.** Bounds on conditional probabilities grow like `1/sqrt(p(k))`. `AT1_SPEC.md` 8.3 keeps `p(k) >= 1e-3` in every case.
- **Engine limits.** Case, oracle and provenance files up to 1 MiB; numeric tokens up to 4096 digits (the contract's own limits are far below). Beyond these the tool exits 1 with `AT1_ENGINE_ERROR RESOURCE_LIMIT`, which is not a refusal code.
- **Exact arithmetic cost.** The exact elimination of the `2N x 2N` matrix is cheap at contract sizes (N = 64, M = 256 runs in milliseconds here). It was not timed beyond the contract limits.
- **ERROR results.** The labels are written `UNDEFINED`, the same reading as the trivial-kernel rule; the contract fixes values and bounds for `ERROR` but not the label status token.
- **Platforms.** Built and tested only on Linux aarch64 with gcc 13.3. Not yet built here with clang. macOS arm64 with Apple clang 17 and GNU Make 3.81 is the stated replication target: no GNU-make-4 features, no glibc-only calls, no Linux-only headers in the engine, no `long double`, digests computed in C. The sanitizer target probes the compiler first and skips if unsupported. Values may differ in the last bits across libm implementations (`cos`, `sin`); verdicts are designed not to (spec 8.3 margins).
