# AT-1 reference oracle (Rust, standard library only)

Independent reference for the AT-1 interacting Page-Wootters qualification. Given an
`AT1_CASE_V1` file it validates the case in contract order, decides the kernel of
`H_total = H_C (x) I + I (x) H_S + V` exactly level by level, computes every written quantity
by two separate routes, judges the twelve checks in exact arithmetic and writes one
`AT1_RESULT_V1` oracle record. A PASS here means conformance with the frozen contracts and
AT1_SPEC; it says nothing about physics. Reproducing the hand tables is a calibration
result, not a discovery.

Owner: Agent 2 (`AT1_CHARTER.md` section 7, `aien-dev/omega#371`). Everything lives in this
directory. Nothing here is the candidate engine and nothing here is used by it; no source is
imported from `research/atemporal/at0/` (two leaf files, `sha256.rs` and `complex.rs`, were
copied from the AT-0 oracle as a starting point, as charter section 4 allows).

## Contracts and readings this code follows

- Contracts: `AT1_CASE_V1` (sha256 `e62018d8deec97fffca5cacaf6452d8fc45c395596517665cd73eb0c777a3e8f`)
  and `AT1_RESULT_V1` (`3e6efd7e641a89bf0e425267525afabc51fe7a5497d3d539cdeaf09ddb6fa92d`),
  frozen at `aien-architecture` `cbe4c8ed88d28bb96d5209327ecd30aa9cddaf7a`, the
  `contract_commit` every record writes (`CONTRACT_COMMIT` in `src/at1/case.rs`).
  `AT0_RESULT_V2` sections 2, 4, 5 and 7 are normative through `AT1_RESULT_V1`.
- Mathematics: `AT1_SPEC.md`, merged as `81047f52850f4bd14c2fc5772ec2ac833ac694b9`
  (`SPEC_COMMIT` in `src/at1/mod.rs`). Every section 13 table is a fixture and a test.
- Charter readings (`AT1_CHARTER.md` at `ea91d7c`): (b) shape before range, non-canonical
  decided after the whole shape pass; (c) the record carries all four `reference_*`
  families in values-block order; (d) the first token of a version line is its key, so an
  `OMEGA-AT0-CASE` header is `CASE_PARSE_ERROR` and a wrong version value is
  `CASE_UNSUPPORTED_VERSION`; (e) index tokens are compared by parsed value, `01` is
  `CASE_NONCANONICAL`, a non-integer or wrong value is `CASE_PARSE_ERROR`.
- Declared token readings (same as the qualified AT-0 C engine, not stated in the frozen
  text; listed so a reviewer can object): integers are `-?[0-9]+` (so `+4` is
  `CASE_PARSE_ERROR`); a fixed-arity list with the wrong arity (`system_hamiltonian_pauli` 4,
  `interaction_pauli` 3, `reference_system_state` 2) and a count mismatch of
  `clock_energies`, `interaction_pauli` or `clock_label` lines are `CASE_PARSE_ERROR`; a
  rational with denominator below 1 and a negative part of a scaled token are
  `CASE_NONCANONICAL`; a rational over the token limit and a scaled exponent above 40 are
  `CASE_INVALID_PARAMETER`.
- Phase sign frozen in `AT1_CASE_V1` section 3: `chi_k = sum_j exp(+2 pi i E_j (k - r) tau) u_j`.

## What the record contains

- The record's own values (`physical_state_kernel_dim`, `constraint_residual`,
  `povm_residual`, `label`, `clock_probability`, `pauli`) come from the literal matrix route
  (`src/at1/matrix.rs`): dense `H_total`, the orthogonal projector onto its kernel built
  from the exact kernel pairs (never from a numerical eigen-decomposition, AT1_SPEC section
  8.4 item 3), `Psi = P_0 (|t_r> (x) psi_0)`, `phi_k` by the literal bra `<t_k|`, `rho_k`
  and `Tr(rho_k Pi)`.
- `reference_clock_probability` and `reference_interacting` come from the closed form of
  AT1_SPEC Theorem 3 (`src/at1/closed.rs`), from the exact `u_j` and `S`.
- `reference_ideal` comes from a right-handed Bloch rotation of `psi_0` about `h / |h|` by
  `2 pi * 2 |h| (k - r) tau` (no `V`, no kernel).
- The oracle's own verdict judges the matrix route against the two reference routes, so on
  every section 13 case it reproduces the expected outcome and codes.
- Marking (`AT1_RESULT_V1` section 1): `build_cc` begins with `oracle `, `engine_sha256`
  equals `oracle_sha256`; the writer refuses anything else. A refused case prints exactly
  `AT1_CASE_REFUSED <code>` on stderr and exits 2.

## Layout

```
build.sh            exact rustc invocation (build | test | lib), POSIX sh
isolation.sh        symbol-isolation gate for the compute crate (source rule + nm -u + mutant)
src/main.rs         the only file that touches files, git, the process table or the wall clock
src/tests.rs        the test suite
src/at1/mod.rs      compute crate root; SPEC_COMMIT
src/at1/big.rs      arbitrary-precision naturals, integers, rationals, Gaussian rationals
src/at1/sha256.rs   SHA-256 (case_id, acceptance_id, verdict_id, evidence_digest, case_file_sha256)
src/at1/complex.rs  complex numbers and dense complex matrices
src/at1/case.rs     AT1_CASE_V1 parser and validator in contract order; case builder
src/at1/model.rs    exact level decisions, kernel pairs, u_j, S; exact phase reduction
src/at1/matrix.rs   literal matrix route (the record's own values)
src/at1/closed.rs   closed form (Theorem 3) and Bloch rotation (the reference families)
src/at1/result.rs   values block, values parser, exact judge, verdict block, record writer
src/at1/fixtures.rs AT1_SPEC section 13 cases with their exact hand tables
fixtures/           <name>.case, <name>.values, CALIBRATION.md (generated, deterministic)
NUMERICAL_ERROR.md  accounting, stated bounds, measured errors, limits
```

## Build and run (no network, no Cargo, no crates)

```
./build.sh build          # target/at1-oracle
./build.sh test           # target/at1-oracle-test, runs the suite
./isolation.sh            # isolation gate, prints ISOLATION PASS
./target/at1-oracle emit <case-file> [-o <out-file>]
./target/at1-oracle check <case-file>       # prints case_id, acceptance_id, case_file_sha256
./target/at1-oracle fixtures fixtures       # regenerates fixtures/ byte for byte
```

The build is one command:

```
rustc --edition 2021 -C opt-level=2 -C codegen-units=1 -C debuginfo=0 -C panic=abort -D warnings \
      --crate-name at1_oracle src/main.rs -o target/at1-oracle
```

The test build uses the same flags without `-C panic=abort`, plus `--test`. `build.sh`
records `rustc -V`, the host triple and the flag string in the binary, which is where
`build_cc` and `build_flags` come from. Every digest is computed in Rust; no script calls
`sha256sum` or `shasum`. No Makefile and no toolchain pin file are shipped, so `rustup`
never tries to download anything.

MacBook (macOS arm64, rustc 1.97.1): the same three commands. `isolation.sh` accepts the
leading underscore BSD `nm` puts on C symbols, and the host name falls back to `uname -n`
when `/etc/hostname` is missing. Verified here with rustc 1.97.1 and 1.98.1 on
`aarch64-unknown-linux-gnu`: both build with `-D warnings`, both pass the suite, and their
fixtures are byte-identical. It has not been run on macOS by this agent.

## Results (rustc 1.98.1, aarch64-unknown-linux-gnu)

- Worked example `at1-kat-rotated-level-n4` reproduces `case_file_sha256 76e282fe...9ba4`,
  `case_id 890980a4...25f1`, `acceptance_id d63246c3...4d8f`, kernel dimension 2 and
  `p = 5/28, 1/4, 9/28, 1/4`; its oracle record has
  `verdict_id a918558b4b1e99e4015813eaefaab18973861b7b7d797ef89bddc9f41637eabb`.
- Test suite: 24 tests pass (20 suite tests, 4 unit tests of the arithmetic, SHA-256 and
  complex layers).
- Section 13 tables covered: 13.2 P1 (a, b), P1c, P2, P3, P3b, P4, P5, P6; 13.3 deviation
  classes through N1 to N1f; 13.4 N1 to N1f, N2, N3, N4a, N4b, N5, N6; 13.5 R0 to R14 and
  all 42 inherited AT-0 refusal rows re-expressed on P2. 1409 values compared with the exact
  tables, largest deviation `6.7e-16`, no value outside its written bound, largest bound
  `1.1e-13` (see `fixtures/CALIBRATION.md`). Every case meets its expectation.
- AT1_SPEC section 9: T1, T3 (P1c under IDEAL), T4, T5, T6, T7 and T9 are tests; mutants
  M1 (drop `V`, including the blind P1 and P1c cases and the N1 route to
  `ORACLE_DISAGREEMENT`), M3 and M4 are judge tests; a status mismatch fails check 12.
- Isolation gate: PASS (source rule, `nm -u` object rule, both catch the `std::time` mutant).

## Declared limits

- Bounds are ESTIMATED, not enclosures; RIGOROUS is never claimed (N5 fails by design).
  `NUMERICAL_ERROR.md` gives the accounting and its limits, including the small-`p(k)` case
  where the oracle's own verdict can turn INDETERMINATE.
- Not tested here: T2 against the AT-0 tables beyond the P1 table they share, T8 (label
  order reversed), T10 (cross-platform; gate G7 belongs to Agent 7), mutant M2 (a flipped `V`
  on P2 has an irrational spectrum, so it is not a valid case; it stays with Agent 4's
  engine mutants), M5 to M7.
- One thread, binary64, platform `cos`/`sin`. Not run on macOS by this agent.
