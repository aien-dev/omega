# AT-0 candidate relational quantum engine (research model)

**Status:** candidate implementation, research directory. Not a gate, not evidence, not a contract.
**Program:** AT-0 Clock-Free Universe, tracking issue aien-dev/omega#358. Pinned to: frozen contracts `AT0_CASE_V1` at aien-architecture `044c9d1` and `AT0_RESULT_V2` (Agent 0 ruling on omega#358; `AT0_RESULT_V1` superseded, same section numbers); charter (ownership, gates) at aien-architecture `d390939`; `AT0_SPEC.md` at aien-architecture `68f47e2` (PR #176 merged 2026-10-09; the hand tables the tests use were re-verified against it: section 13.2 P1/P1b and T1/T2 agree with `test_pauli_probabilities` and `test_schrodinger_general_n_phase_omega`).
**Built from:** omega `19f73c7`. **Language:** portable C11 (`__int128` via `__extension__` for exact rationals),
`-lm` only, single thread, no GPU, no network, no Python.

## What it does

Given an `AT0_CASE_V1` case file, the engine builds a finite Page-Wootters universe (a clock of dimension
N and one qubit), projects the declared reference product state onto the exact kernel of the total
constraint `H_total = H_C (x) I + I (x) H_S`, measures the clock with the covariant discrete POVM
`F_k = w |t_k><t_k|`, conditions the qubit on each clock label by the Born rule, and reports the Pauli
X, Y, Z outcome probabilities of the conditional state. No external time parameter enters the model:
the clock phase is a declared measurement parameter (`povm_tau_turns`, label index `k`), never an
execution timestamp. The compute objects import no clock, timer, random, file, process, socket or
thread symbol (checked with `nm -u`).

**This demonstrates a particular relational quantum construction. It does not demonstrate the physical
emergence of time** (charter section 1, draft spec section 11).

## Components (one file each, explicit objects, no global state)

| Component | File | Entry points |
|---|---|---|
| Exact arithmetic | `at0_exact.c` | reduced rationals with 128-bit intermediates and contract limits, scaled decimals, exact phase reduction mod one turn, exact integer comparison of a binary64 against decimal tolerances (`at0_exact_prob_status`), and overflow-free exact helpers for derived quantities whose reduced denominators exceed 2^62 (`at0_rat_norm_exact` for `|h|`, `at0_exact_norm_plus_hz`, `at0_exact_sum_is_zero`; checked 128-bit products, never wrapped) |
| Versioned input | `at0_case.c` | `at0_case_parse` (AT0_CASE_V1 rules 1 to 6 in contract order; a well-formed but noncanonical token such as `04` or `-0/1` is `CASE_NONCANONICAL`, judged after the whole shape pass, both within rule 1), `at0_case_emit` (byte-exact), `at0_case_identities` (domain-tagged SHA-256 via omega `src/sha256.c`); works on bytes only |
| File input | `at0_io.c` | `at0_case_read_file`: the only object that touches the file system (fopen/fread), hands the bytes to `at0_case_parse` |
| Quantum state | `at0_state.c` | bounds-checked vector in `C^N (x) C^2`, clock factor first; norms, inner products, finiteness checks |
| Hamiltonian construction | `at0_hamiltonian.c` | spectrum `h0 +/- |h|` (exact in the case's `h0`, `h_norm`; stored as binary64), binary64 eigenvectors, `H_total` applied to a state |
| Constraint verification | `at0_constraint.c` | exact kernel enumeration (`E_j + h0 = -/+ |h|` compared in rationals, the eigenvalues are never reduced), nullspace projection `P_0 (|t_r> (x) |psi_0>)`, residual `||H_total Psi_hat||`, exact decision whether `Psi = 0` |
| Internal clock POVM | `at0_povm.c` | `|t_k> = N^(-1/2) sum_j exp(-2 pi i E_j k tau) |E_j>` with the turn count reduced exactly before `cos`/`sin`; residual `||sum_k F_k - I||_F` |
| Conditional state | `at0_conditional.c` | `phi_k = (<t_k| (x) I) Psi`, `p(k) = w ||phi_k||^2 / ||Psi||^2`, `rho_k = phi phi^dagger / ||phi||^2` |
| Observables | `at0_observable.c` | `P(sigma = +/-1) = Tr(rho (I +/- sigma)/2)`, each sign computed directly from `rho`, never as one minus the other |
| Engine | `at0_engine.c` | orchestration, ESTIMATED bounds, label status by exact comparison; test-only reversed-order entry point |
| Versioned output | `at0_output.c` | `OMEGA-AT0-ENGINE v1` writer, `f64:` tokens with `-0.0` folded, `nonfinite` and `undefined` tokens |
| Tool | `at0_main.c` | `at0-model <case-file> [--reversed]` |

## Build and test

```
cd research/atemporal/at0/model
./build.sh            # plain:  -std=c11 -Wall -Wextra -Werror -pedantic -O2, into ./build
./build.sh asan       # sanitizer: -O1 -g -fsanitize=address,undefined, into ./build-asan
./build/at0-tests tests/cases
./build/at0-model tests/cases/kat-ideal-qubit-n4.case
```

Nothing is added to omega's `Makefile` or `mk/`; `make test` is unaffected. `tests/mkcase.sh <body>` stamps
`case_id` and `acceptance_id` onto a hand-written case body (shell and `sha256sum` only); it reproduces
the published digests of the contract's section 6 example.

Exit codes of `at0-model`: 0 success (engine text on stdout); 2 case refused (`AT0_CASE_REFUSED <code>`
on stderr, one line, per AT0_RESULT_V2 section 5); 1 any other error (`AT0_ENGINE_ERROR <code>`).

## Output format: `OMEGA-AT0-ENGINE v1`

Ruled a component output, not a third interface (Agent 0, omega#358): no identity or digest is defined over it. Its
values-block lines must stay byte-valid `AT0_RESULT_V2` values-block grammar so that Agent 5's runner can splice them
verbatim; the wrapper lines are owned here and versioned, and any change to them is a version bump plus Agent 5 re-review.
Header lines, then the case identities and `case_file_sha256`, then a `numerics` block
(`arithmetic BINARY64`, `bound_kind ESTIMATED`, `threads 1`), then a `values` block that is exactly the
AT0_RESULT_V2 section 1 values block **without** the oracle's `reference` lines. A runner can splice this
block, plus the oracle's reference lines, into a full result. Value tokens follow AT0_RESULT_V2 section 2.

## Mathematical assumptions

1. Units hbar = 1; clock energies and `h0, hx, hy, hz` are exact rationals from the case.
2. `|h|^2 = hx^2 + hy^2 + hz^2` is the square of a rational (validated; else `CASE_IRRATIONAL_SPECTRUM`),
   so the system eigenvalues `h0 +/- |h|` are rational and the kernel test is exact.
3. Kernel of `H_total` is spanned by `|E_j> (x) |e_s>` with `E_j + e_s = 0`; because energies are strictly
   increasing, each system eigenvalue is matched by at most one clock energy. When `|h| = 0` both system
   components of a matched energy are in the kernel.
4. Eigenvectors: for `|h| > 0` and `|h| + hz != 0`, `v_- = (-(hx - i hy), |h| + hz) / sqrt(2|h|(|h|+hz))` and
   `v_+ = (|h| + hz, hx + i hy) / same`; for `|h| + hz = 0` they are `|0>, |1>`; for `|h| = 0` the
   computational basis. The decision whether `Psi = P_0(|t_r> (x) |psi_0>)` is the zero vector is made
   exactly in rationals (overlaps `<e_s|psi_0>` with the unnormalized rational eigenvectors), never by a
   floating-point threshold.
5. Clock phase `exp(-2 pi i E_j k tau)`: the turn count `E_j k tau` is reduced exactly to `[-1/2, 1/2)` in
   128-bit integer arithmetic, converted to binary64 (exact, both numerator and denominator are below 2^53),
   then passed to `cos`/`sin`. No lookup table; special angles are not special-cased.
6. The POVM is not assumed to resolve the identity; the residual is measured and reported.
7. Conditional probabilities are Born-rule traces against `rho_k`; purity, pair sums and ranges are
   consequences, not inputs.

Derived consequence used by the tests, not assumed by the engine: when both system eigenvalues are matched,
`phi_k = (1/N) exp(-i H_S (t_k - t_r)) |psi_0>` restricted to the matched eigenspaces, so the conditional
state is the Schrodinger evolution read at `t_k - t_r` with `t_k = 2 pi k tau`, and `p(k) = w / N` for
every label whether or not the POVM is normalized.

## Error estimates (`bound_kind ESTIMATED`, not a proof)

With `eps = 2^-53`, N = `clock_dim`, M = `clock_label_count`, `w = povm_weight`,
`Hmax = max_j |E_j| + |h0| + |h|`, each bound is a first-order operation-count estimate with a safety factor,
rounded up to seven significant decimal digits plus one unit:

| Quantity | Estimate |
|---|---|
| `constraint_residual` | `32 eps N (Hmax + 1)` |
| `povm_residual` | `16 eps N (M w + 1)` |
| `clock_probability k` | `32 eps (N + 4) (p(k) + 1)` |
| `pauli k A s` | `32 eps (N + 8)` |

Assumes glibc `cos`/`sin`/`sqrt` within 1 ulp on the build target. The tests check that every measured
deviation from the hand table and from the test-side Schrodinger reference lies inside the stated bound.
A `RIGOROUS` bound would need directed-rounding interval arithmetic with enclosures of `cos` and `sin`; it is
not provided here, so a case with `min_bound_kind RIGOROUS` will fail `BOUND_KIND_INSUFFICIENT` by design.

## Supported and unsupported cases

**Supported:** every valid `AT0_CASE_V1` case: `2 <= N <= 64`, `1 <= M <= 256`, any rational energies,
field, weight and tau within the contract limits, any nonzero rational reference state, any reference label
(phase offset), any `|h|` (frequency `omega = 2|h|`), degenerate `|h| = 0`, trivial kernel, half-covered
spectrum, non-normalized POVMs, exactly-zero projections.

**Unsupported / not provided:** clock-system interaction (`interaction NONE` only); system dimension other
than 2; `RIGOROUS` bounds; shot sampling; the oracle's `reference` lines and the full `AT0_RESULT_V2` file
(provenance, verdict, digests belong to the runner/verifier); CI wiring; GPU or parallel execution.

## Behaviour in corner cases (to be confirmed against Agent 4's hand derivations)

- Trivial kernel or exactly-zero projection: `constraint_residual undefined 0@0`, every label `UNDEFINED`,
  every `clock_probability` and `pauli` value `undefined 0@0`. AT0_RESULT_V1 does not say which label status
  to write when `Psi = 0`; `UNDEFINED` is the choice here and is an open contract question for Agent 0.
- Contract limits on written tokens (|n|, d <= 2^20; scaled k <= 40) are judged in rule 3 as `CASE_INVALID_PARAMETER`,
  after the shape (rule 1) and version (rule 2) checks. A token longer than 18 digits is parsed as a saturation value
  (never an integer overflow) and refused the same way; a scaled numerator above 10^18 - 1 is refused as unsupported.
- Derived exact quantities (`|h|`, `|h| + hz`, kernel matches, exact overlaps) never pass through a reduced rational:
  with tokens at the rule-3 limit (2^20) and distinct denominators their reduced denominators reach 2^80 and beyond
  (qualification finding D1, case P5). They are decided on unreduced 128-bit integers over the common denominator
  `dx dy dz`, with every product bounded at the use site (largest: 2^124 in the overlap test) and checked with
  overflow-detecting arithmetic; an exceeded bound stops with `AT0_ENGINE_ERROR ERR_OVERFLOW` (explicit, never a wrong
  number), which no in-limit case can reach. `test_large_rationals_within_limits` covers P5, a case whose
  overlap with one eigenvector is exactly zero (the kernel then holds that eigenvector only and `Psi = 0` must be
  decided exactly), and a generic case with four distinct denominators.
- Trivial kernel ruling (Agent 0, omega#358): `povm_residual` is still computed, nothing is divided, every label is
  `UNDEFINED`; this engine already behaves that way. Result files move to `AT0_RESULT_V2`; the `contract` line of the
  engine output names it, and the values block is unchanged.
- Independent review (Claude Opus 5.5 over the full diff, 2026-10-09) found five arithmetic-limit defects, all fixed and
  covered by `test_review_findings`; it confirmed the eigenvectors, kernel projection, phase reduction, Born rule and
  identity hashing. Codex review was unavailable (usage limit).
- Agent 0 ruling (omega#358, third pass, R37): the fixed literals of AT0_CASE_V1 section 1 (`model_family`,
  `energy_unit`, `system_dim`, `interaction`, `constraint`, `physical_state`, `clock_povm`, `observables`) are bare
  grammar tokens, so a different token is a shape failure, `CASE_PARSE_ERROR`, judged in the shape pass; they are no
  longer rule-3 parameters. The ruling named `model_family`; the same reading is applied to all eight (the evaluator
  codec already does), stated here so it can be overruled.
- Qualification fixes (Agent 5 first run, omega#363): D1 `ERR_OVERFLOW` on P5 (above), D2 noncanonical integers
  (`clock_dim 04`, `-0/1`) now `CASE_NONCANONICAL` instead of `CASE_PARSE_ERROR`, D6 file reader moved to `at0_io.c`,
  D8 spec pin moved to the merged commit. D3 (R31, R36: rule 1 against rule 3) is an Agent 0 contract call and is unchanged. Second Opus review (2026-10-09)
  found no arithmetic defect; its test-strength and wording findings are applied.
- `tests/cases/kat-ideal-qubit-n4.case` is the contract's section 6 example byte for byte
  (`case_file_sha256 ed16c95c...`).

## Ownership

The charter at aien-architecture `d390939` (section 7, aligned with omega#358) places this candidate engine under
`research/atemporal/at0/model/` (Agent 3). The oracle (`at0/oracle/`, Agent 2, Rust, std only) and the evaluator share no
source, object or numerical routine with this directory; the only shared omega source is `src/sha256.c`.
