# AT-0 reference oracle (Rust, standard library only)

Independent cross-check for the AT-0 Page-Wootters qualification. Given an `AT0_CASE_V1`
file it rebuilds the finite model from scratch, conditions the stationary state on each
clock outcome by direct complex linear algebra, computes X/Y/Z probabilities two more ways
without any clock matrix, judges every check in exact arithmetic, and writes an
`AT0_RESULT_V2` record. Reproducing the known mathematics here is a calibration result,
not a discovery.

Owner: Agent 2 (charter `aien-dev/aien-architecture` ADR-0038, `omega#358`). Everything
lives in this directory. Nothing here is the candidate engine and nothing here is used by
it: `research/atemporal/at0/model/` (Agent 3, C11, merged in `omega#360`) and this
directory share no source, no parser, no arithmetic and no build step.

## Rulings this code follows

- Drake, 2026-10-09 (relayed on `omega#358`): Rust, standard library only. No crates, no
  Cargo, no Python anywhere. Built by `rustc` directly with pinned flags.
- Agent 0, `omega#358` comment `6090404007`: result records are `AT0_RESULT_V2`
  (`OMEGA-AT0-RESULT v2`, `domain omega.at0.result.v2`, verdict tag `omega.at0.verdict.v2`,
  evidence tag `omega.at0.evidence.v2`). On a trivial kernel every label is `UNDEFINED` and
  `constraint_residual`, `clock_probability` and `pauli` lines read `undefined 0@0`;
  `povm_residual` and `reference` lines stay real. The parser refuses `undefined` anywhere
  else. Case files stay `AT0_CASE_V1`.
- Agent 0, `omega#358` (component outputs): the oracle's output is a component output, not
  a third interface. Its `reference <k> <AXIS> <SIGN> <value> <bound>` lines are byte-valid
  per the V2 values-block grammar and order (k ascending, then X Y Z, then PLUS MINUS) so
  Agent 5's runner can splice them next to the engine block verbatim. This oracle emits no
  wrapper format of its own: the whole file is one `AT0_RESULT_V2` record. Any future
  wrapper line would be versioned here and would need Agent 5 re-review.

Contracts: `AT0_CASE_V1` frozen at `aien-architecture` `044c9d1` (`CASE_CONTRACT_COMMIT`),
`AT0_RESULT_V2` added at `c7a7181` and recorded in `AT0_FREEZE` at
`fe86e43aae63370b3084f78e971a33b81ce75a9d`, which is the `contract_commit` every record
writes (`CONTRACT_COMMIT`; Agent 5 finding D7, evaluator allowlist). The hand-table tests
and the `spec-*` fixtures target the mathematical contract `AT0_SPEC.md`, merged as
`68f47e26764a9f0b46d91194bf073824d04333bb` (`SPEC_COMMIT` in `src/at0/mod.rs`; Agent 5
finding D8). Re-verified against that commit on 2026-10-09: section 13.2 P1 and P1c, and
13.3 N1 to N5 agree with the fixtures below, and 13.2 P2 was added as a fixture.

## Layout

```
build.sh            exact rustc invocation (build | test | lib)
isolation.sh        symbol-isolation gate for the compute crate (source rule + nm -u + mutant)
rust-toolchain.toml rustc 1.98.1 pin (informational; build.sh records the real version)
src/main.rs         the only file that touches files, git, the process table or the wall clock;
                    CLI (emit | fixtures | check) and the test suite
src/at0/mod.rs      compute crate root; SPEC_COMMIT
src/at0/rational.rs i128 rationals (exact spectrum, kernel pairs, identities)
src/at0/sha256.rs   own SHA-256 (case_id, acceptance_id, verdict_id, evidence_digest)
src/at0/exact.rs    fixed-point big integers: exact comparison of f64 tokens with scaled decimals
src/at0/complex.rs  complex numbers and dense complex matrices (Vec<Vec<C>>)
src/at0/case.rs     AT0_CASE_V1 parser/validator in contract order, case builder for tests
src/at0/matrix_path.rs  H_C, H_S, H_total, kernel projector, Psi, POVM, conditioning, rho_k, Pauli
src/at0/reference.rs    analytic paths: Schrodinger closed form and Bloch/Rodrigues rotation
src/at0/result.rs   values block, exact judge, verdict block, AT0_RESULT_V2 writer and values parser
src/at0/fixtures.rs the 22 calibration fixtures and their hand-derived expectations
fixtures/           <name>.case, <name>.values, CALIBRATION.md (all generated, deterministic)
NUMERICAL_ERROR.md  stated bounds, measured errors, limitations
```

## Build

Exact command (`./build.sh build`):

```
rustc --edition 2021 -C opt-level=2 -C codegen-units=1 -C debuginfo=0 -C panic=abort -D warnings \
      --crate-name at0_oracle src/main.rs -o target/at0-oracle
```

`build.sh` exports the compiler's own `rustc --version` line, host triple and the flag
string into the binary (`option_env!`), which is where `build_cc` and `build_flags` in the
provenance block come from. The test build uses the same flags minus `-C panic=abort`
(the test harness needs unwinding) plus `--test`. `./build.sh lib` builds the compute crate
alone as an rlib for the isolation gate.

Toolchain the fixtures and numbers in this directory were produced with:

```
rustc 1.98.1 (48a229cea 2026-09-01)
binary: rustc
commit-hash: 48a229ceaefd4985c50990b14116b6d856af0985
commit-date: 2026-09-01
host: aarch64-unknown-linux-gnu
release: 1.98.1
LLVM version: 22.1.8
```

No Cargo, no crates, no `build.rs`, no network, no C library beyond what Rust's standard
library links itself.

## Use

```
./build.sh                         # target/at0-oracle
./build.sh test                    # 14 tests (4 unit, 10 contract/physics)
./isolation.sh                     # ISOLATION PASS expected
target/at0-oracle check   <case>   # validate; prints case_id, acceptance_id, file sha256
target/at0-oracle emit    <case> [--dephased] [-o out]   # AT0_RESULT_V2 record
target/at0-oracle fixtures <dir>   # regenerate fixtures/ (45 files)
```

A refused case prints exactly `AT0_CASE_REFUSED <code>` on stderr, writes nothing and exits
2. `--dephased` replaces the pure physical state by its kernel-branch dephasing (negative
control) and marks `build_cc` with `(dephased control)`; such a record is a control, never
a qualification result.

Provenance: `source_commit`/`source_tree_clean` come from `git` run in this directory.
Every record is marked as oracle output so it can never be cited as the candidate engine's
result (Agent 0 condition on the omega#359 approval): `build_cc` begins with the literal
`oracle ` and `engine_sha256` equals `oracle_sha256`, both the SHA-256 of the running
oracle executable (the matrix path and the analytic path are two modules of one binary).
The writer refuses to produce a record that violates either rule (tested). `verdict_id` depends
only on the case and the values; `evidence_digest` covers the timestamps too, so two runs
agree on `verdict_id` and differ in `evidence_digest` (tested).

## What is independent, and how that is enforced

- Own parser and validator for `AT0_CASE_V1` reproducing the contract's known-answer
  digests (`case_id 3cf4ca4f...`, `acceptance_id a13fb02d...`, file sha `ed16c95c...`).
- Own SHA-256, own rationals, own complex/matrix arithmetic, own exact decimal comparison.
  No line is shared with, derived from or linked against the C11 engine.
- Three routes to every Pauli probability: dense matrix conditioning (`rho_k`), the
  closed-form Schrodinger evolution, and a Bloch-vector rotation in real arithmetic. All
  three must agree to `1e-12` in the tests; they agree to about `2e-16`.
- Two routes inside the matrix path: condition the vector, or condition the density matrix
  `|Psi><Psi|` and renormalize. Both must agree (tested, Frobenius `<= 1e-12`).
- The compute crate (`src/at0/`) may not read the clock, files, environment, processes,
  threads, network or randomness. `isolation.sh` enforces this in three steps: a source rule
  (grep for the forbidden facilities), an object rule (`nm -u` on the compute rlib for
  `clock_gettime`, `getrandom`, `pthread_create`, `socket`, `open64`, `getenv`, ... ), and a
  negative control that plants a `std::time` read in a copy and checks that the source rule
  and the object rule each catch it on their own. The object rule bans two symbol families:
  libc entry points and mangled Rust std paths (`3std4time`, `3std2fs`, ...), because a
  `std::time` call leaves no libc symbol in the rlib (Agent 5 finding D5).
- Exact judging: every check compares the written `f64:` token, converted exactly, against
  the exact scaled-decimal tolerance. No float comparison decides a verdict.

## Tests (`./build.sh test`)

`known_answer_digests_and_kernel`, `refusals_in_contract_order` (version, parse, canonical
form, duplicate keys, CRLF, invalid parameters, irrational spectrum, identity mismatch,
acceptance rules, size limit), `case_name_outside_identity_and_tokens`,
`reference_model_hand_table` (exact constraint, POVM completeness, uniform `1/N`, the X/Y/Z
table), `density_matrix_properties_and_routes` (Hermitian, unit trace, eigenvalues in
`[0,1]`, probabilities in `[0,1]`, vector route = density route),
`multiple_n_offsets_phases_tilts` (`N` in 3,4,5,6,8,12; reference label `t1..t3`; tilted
and `h0`-shifted fields; global phases `i`, `-1`, `2`, `-3i`), `negative_controls`
(dephased mixture, uncovered and half-covered spectra, wrong weight, broken clock, rigorous
demand), `spec_draft_reference_model` (draft theorems, T4 eigenstate, T5 relational phase,
N3, N4), `record_shape_and_wall_clock_independence`, `fixtures_render_and_meet_expectations`,
plus unit tests for SHA-256 (FIPS vectors), rationals, complex algebra and exact comparison.

## Fixtures and how their expectations were derived

`fixtures/` holds 22 cases, their `.values` (values block, verdict block, `verdict_id`) and
`CALIBRATION.md`. All of it is written by `target/at0-oracle fixtures fixtures/` with no
wall clock, so re-running it reproduces the files byte for byte. The `.case` files are
identical to the ones first published on this branch (checked with `cmp`).

Expectations are hand-derived exact rationals, not numbers copied from a run:

- Ideal clock (`E_j = j - (N-1)/2`, `H_S = sigma_z/2`, `psi_0 = |+>`, `tau = 1/N`, `w = 1`):
  the kernel is spanned by `|E=1/2>|1>` and `|E=-1/2>|0>`, so every `p(k) = 1/N` exactly,
  and with `theta_k = 2 pi k / N` the table is `P(X+) = (1 + cos theta)/2`,
  `P(Y+) = (1 - sin theta)/2`, `P(Z+) = 1/2`. Expectations are recorded only at labels where
  `cos`/`sin` are rational (`0`, `+-1/2`, `+-1`), which is why `N = 3, 5` check 7 values and
  `N = 4, 6, 8` check 14 or 28.
- Offset reference `t2`: same table shifted by two labels. Global phase `i` and the
  unnormalized state `(2, 2)`: same table as the ideal case (phase and norm cancel).
- Draft spec model (`E = -1, 0`, `H_S = diag(0, 1)` written as `h0 = 1/2, hz = -1/2`,
  `w = 1/2`): the spec's own table; `hz = -1/2` flips the sign of `sin`, so
  `P(Y+) = (1 - sin theta)/2`. T4 (`psi_0 = |0>`): `P(Z+) = 1`, X and Y `1/2` at every label.
  T5 (`psi_0 = (1, i)`): the table shifted one label backwards, i.e. `theta -> theta - pi/2`.
- Spec section 13.2 P2 (`spec-p2-tilted-h0`: `h0 = 1/10`, `h = (3/10, 0, 2/5)`, clock
  `E = -3/5, 2/5`, `psi_0 = |0>`, `w = 1/2`): the Bloch vector `(0, 0, 1)` rotated about
  `n = (3/5, 0, 4/5)` by `k pi / 2` gives `(12/25, -3/5, 16/25)`, `(24/25, 0, 7/25)`,
  `(12/25, 3/5, 16/25)`, so `P(X+) = 1/2, 37/50, 49/50, 37/50`, `P(Y+) = 1/2, 1/5, 1/2, 4/5`,
  `P(Z+) = 1, 41/50, 16/25, 41/50`, `p(k) = 1/4`. Fully rational, so all 16 values are checked.
- Negative controls carry no rational expectations; their expectation is the failure code
  set declared in the case's own acceptance block, and `expectation_met YES` means the
  oracle produced exactly those codes.

`CALIBRATION.md` lists per fixture the number of exact values checked and the largest
deviation seen; the worst is `2.2e-16` (one unit in the last place of `1/2`).

## Findings worth knowing (calibration, not discovery)

1. A broken clock (`tau = 1/3` with `N = 4`) and the correct weight `w = N/M` fails only
   `povm_normalization`; the clock probabilities still sum to one. A wrong weight fails both
   `povm_normalization` and `probability_sum`.
2. In the draft spec's convention the sign of the Y row depends on the sign of `hz`; the
   closed form and the matrix path agree, so any disagreement with another implementation is
   a convention error, not numerics.
3. The relational phase `(1, i)` shifts the pattern one label backwards, not forwards.
4. On a trivial kernel (fixture `n1-uncovered`) V2 section 4 fixes check 3 `FAIL` and checks
   4, 6, 7, 8, 9, 10 `NOT_EVALUATED` even though the `reference` lines are still written;
   the first release of this oracle evaluated check 7 on those lines (Agent 5 finding D4),
   fixed and tested since.
5. Dephasing the kernel branches keeps `p(k) = 1/N` and Hermitian unit-trace `rho_k` but
   flattens every X and Y probability to `1/2`: the clock correlation survives, the
   interference pattern does not. This is the `nc-dephased-control` fixture.

## What this is not

Not the candidate engine, not a verifier of Agent 3's internals, not a RIGOROUS bound
(see `NUMERICAL_ERROR.md`), and not a statement about any physical experiment. It never
reads evaluator-private held-out data or hidden qualification parameters; every input is a
public `.case` file in this directory or one handed to it on the command line.
