# AT-0 Agent 4: independent adversarial evaluator

This directory is the independent evaluator for the AT-0 Clock-Free Universe
program (aien-architecture `docs/plans/atemporal/`). Its job is to try to
falsify the AT-0 implementation's claimed correctness, not to confirm it. It
trusts nothing a candidate reports: every number, every check verdict, every
digest and every provenance claim in a result file is recomputed or
re-derived here from the case file alone.

Pinned references (frozen bytes, verified by digest):

| document | commit |
|---|---|
| AT0_CASE_V1.md, AT0_RESULT_V1.md, AT0_FREEZE.md (frozen) | aien-architecture `044c9d11256d8642f80eedd42cbae8763faf63f5` |
| AT0_CHARTER.md (updated, contract bytes unchanged) | aien-architecture `00e9f2308666f74e58f524b177b2a31eccdc69ba` |
| AT0_SPEC.md draft (PR #176 head) | aien-architecture `0efd1a14cbdf117bc694b556bb61d031cf80c8f9` |
| AT0_RESULT_V2.md (frozen; digest bd0f9eb8cf3ef7226c3ea18c1c9fbd4db62e0c23a0481afa519830fc1f514b5e; AT0_CASE_V1.md unchanged, d90af74bf818598d28114a73e618f5073706797d7d92d472acc2df404cd663d9) | aien-architecture `c7a7181eda57e353dbff3b7d20cde33fb65faeb2` (#180) and `fe86e43aae63370b3084f78e971a33b81ce75a9d` (#181), both verified here by digest and allowlisted |

The evaluator reads only complete `AT0_RESULT_V2` files (Agent 0 ruling,
omega#358): the engine and the reference oracle emit component outputs, Agent
5's runner assembles the full result with verdict block and digests, and this
evaluator judges that assembled file against the case file. It never consumes
component outputs, so its independence from both engine and oracle holds.

No Python, no external libraries. C11 plus POSIX shell. The only file linked
from outside this directory is omega `src/sha256.c`.

## Layout

```
Makefile               build/at0-eval (and build/at0-eval-asan)
run.sh                 the qualification harness; writes results/qualification.json
src/                   evaluator sources (see "Design")
cases/positive/        P* cases expected to PASS the physics
cases/negative/        N* negative controls expected to FAIL with named codes
cases/refuse/          R* malformed or invalid cases expected to be REFUSED with a named code
cases/MANIFEST.tsv     path, class, exact expected response for all 62 public cases
cases/generate.sh      regenerates every public case from `at0-eval gen` plus byte edits
gates/isolation.sh     symbol-level isolation gate with positive and negative control objects
HIDDEN_COMMITMENT.txt  SHA-256 of the six hidden cases (contents withheld until candidate freeze)
FAILURE_TAXONOMY.md    every E4-* finding code, its severity and meaning
ISOLATION_REPORT.md    isolation status (UNISOLATED), reproducibility, toolchain
results/               qualification.json (machine-readable) and candidate result copies
```

## Usage

```
make                      build
sh run.sh --self-only     evaluator self-tests, corpus, mutants, gates, hidden commitment
sh run.sh                 the same plus the candidate section (BLOCKED_NO_CANDIDATE when unset)

AT0_CANDIDATE_CASE_TOOL=<cmd> AT0_CANDIDATE_RUN=<cmd> AT0_CANDIDATE_OBJECTS="<objs>" sh run.sh
```

Candidate hooks: `AT0_CANDIDATE_CASE_TOOL <case>` must print `AT0_CASE_OK <id>`
(exit 0) or `AT0_CASE_REFUSED <code>` on stderr (exit 2) exactly as the
contract requires; `AT0_CANDIDATE_RUN <case>` must write a complete
`AT0_RESULT_V2` file to standard output; `AT0_CANDIDATE_OBJECTS` lists the
compute objects or binaries for the isolation gate. `AT0_HIDDEN_DIR` points at
the private case directory (default `~/at0-private/agent4`). Candidate results
for hidden cases are written into the private directory, never into the repo.

Direct tool commands:

```
build/at0-eval case <file> [--detail]            independent AT0_CASE_V1 codec: AT0_CASE_OK <case_id> / AT0_CASE_REFUSED <code>
build/at0-eval result <file> [--case <case>] [--no-shadow] [--json]
                                                  verify a result: AT0E_VERIFY PASS|FAIL|INCONCLUSIVE plus findings
build/at0-eval shadow <case>                     shadow oracle table (kernel, POVM residual, p(k), Pauli, reference)
build/at0-eval synth <case> [mutant]             honest synthetic result, or one with a planted defect
build/at0-eval mutants                           list the 26 planted defects
build/at0-eval gen key=value ...                 emit a canonical case (name, E, h, ref, psi, tau, w, M, labels, control, expected, codes, minbk, tol*)
```

Exit codes: `case` 0 ok / 2 refused; `result` 0 PASS / 1 FAIL / 3 INCONCLUSIVE.

## Design

**Independent codec** (`at0e_text.c`, `at0e_case.c`). Byte rules, canonical
encodings, fixed line order and the six validity rules of AT0_CASE_V1 are
reimplemented from the contract text, not from any candidate code. The
known-answer example reproduces file digest, `case_id` and `acceptance_id`
exactly, and `gen` re-emits it byte for byte.

**Exact decision arithmetic** (`at0e_big.c`). Every reported value
(`f64:<hex>`), every bound and every tolerance (scaled decimals) is lifted to a
4096-bit integer over the common denominator 2^1100 x 10^80. The ten checks and
their tri-state comparison forms are then decided with no rounding at all, so
the checker can never disagree with the contract by a floating-point hair.

**Shadow oracle** (`at0e_shadow.c`). The physics is recomputed from the case:
exact kernel dimension and matched eigenvalues (rational arithmetic), exact
test for the POVM identity (w = N/M, and for every gap D: D tau not an integer,
D tau M an integer), then long double (113-bit mantissa on this machine)
evaluation of the POVM Frobenius residual, conditional states, clock
probabilities, Pauli probabilities and the Schroedinger reference, with exact
mod-1 angle reduction. The shadow's own bound is stated as ESTIMATED 1e-20, and
a reported value is contradicted only when it differs from the shadow by more
than the sum of both bounds.

Hand derivations behind the shadow: the conditional state is the Schroedinger
evolution restricted to the matched eigencomponents, phi_k = (1/N) sum over
matched s of exp(-2 pi i e_s (k-r) tau) c_s |e_s>; its norm is independent of k,
so the clock probabilities are flat and sum to wM/N exactly. A wrong tau
therefore shows up only in `povm_residual`, never in `probability_sum`; a wrong
weight shows up in both.

**Result verification** (`at0e_result.c`). Strict parse of the result file;
recomputation of `verdict_id` and `evidence_digest` with the version's tags;
binding of the embedded case to the supplied case file (file digest, both ids,
semantic and acceptance blocks, name); provenance rules (contract commit in
the verified allowlist, clean tree, oracle digest not equal to engine digest,
UTC time order, placeholder discipline); undefined-token discipline for the
trivial-kernel shape; independent re-derivation of all ten checks, outcome,
failure codes and `expectation_met`; shadow comparison of every value.

**Mutants** (`at0e_synth.c`). Twenty-six planted defects covering wrong axis,
wrong Y sign, conjugation bug, reversed reference, dephased state, hidden
weight error, hardcoded table, wrong kernel dimension, residual over bound,
hidden non-finite, forged verdict, corrupted evidence, corrupted verdict id,
altered case id, altered tolerance, rebound case, wrong contract commit, dirty
tree, reversed times, oracle equal to engine, NONE bound with non-zero value,
wrong label status (both directions), placeholder with values, CRLF, trailing space. `run.sh`
demands that every defect is caught on a case where it bites (red before
green) and records the pairs where a defect is invisible by construction as
INCONCLUSIVE rather than silently skipping them.

## Blind spots (by construction, documented, covered elsewhere)

- A Y-axis rotation of |0> (P1f) keeps <Y> = 0, so a conjugation bug is
  invisible there; it is caught on P1e, P1g and the KAT.
- A field whose rotation angles are multiples of pi at every sampled clock
  reading (P2, |h| = 1/2, tau = 1/2) keeps <Y> = 0 and hides a reversed
  reference; caught on P1e and P1g.
- Shifting h0 alone (P2b) leaves every probability unchanged, so a table
  hardcoded for the unshifted case is indistinguishable there; caught on P1b,
  P1c, P1d where tau, N and the reference reading differ.
- A forged verdict on a case whose honest verdict is PASS is a no-op; caught on
  N1, N2, N3, N6.
- The spec (PR #176) shows that flipping Hamiltonian signs inside the engine
  changes nothing observable: the engine depends only on Psi and the POVM. A
  phase-sign defect must therefore be a conjugation bug or a reversed
  reference, and only cases with <Y> != 0 can see it. Every case class used for
  qualification includes at least one such case, publicly and in the hidden set.

## Contract ambiguities found (recorded, not resolved here)

- A1 (resolved by V2): the trivial-kernel shape in V1 made check 9 FAIL with
  CONDITIONAL_UNDEFINED; V2 marks checks 4, 6, 7, 8, 9, 10 NOT_EVALUATED, keeps
  check 5 evaluated, and fails check 3 with TRIVIAL_PHYSICAL_STATE. The evaluator
  follows V2.
- A2: a rational beyond the +-1048576 limit is a parameter error
  (CASE_INVALID_PARAMETER), not a shape error. The contract text orders the
  rules that way; the evaluator implements that ordering and R26 exercises it.
- A3: `probability_range` is read as covering the reference probabilities as
  well as the reported ones.
- A4: N5 (minimum bound kind RIGOROUS) fails only because the engine states
  ESTIMATED bounds; an engine with rigorous interval arithmetic would pass it.
- A6: V2 says a check whose input contains `undefined` is NOT_EVALUATED "for that input". The evaluator reads this per input: with mixed label statuses, checks 7, 8 and 10 are evaluated on the DEFINED labels (and on the clock and reference probabilities, which are always real) and NOT_EVALUATED only when no input remains. N7 (all labels UNDEFINED with a nontrivial kernel, tol_zero_probability = 1) exercises the all-undefined shape; it fails with exactly CONDITIONAL_UNDEFINED.
- A5: the charter's N1 example spectrum (1/2, 1, 3/2, 2) is half-covered with a
  broken clock, not uncovered; the corpus uses 1, 2, 3, 4. Agent 0 redefined N1
  the same way independently.

## Statuses

Every test in `results/qualification.json` carries exactly one of PASS, FAIL,
INCONCLUSIVE, NOT_RUN, BLOCKED_NO_CANDIDATE with a specific reason. UNISOLATED
is a property of the whole run (see ISOLATION_REPORT.md), never a test status.
A PASS here is software conformance to the frozen contracts. It is not, and
must not be cited as, a scientific result about time or quantum mechanics.

## Independent review (2026-10-09)

Before merge the sources were reviewed by Opus 5.5 in a separate session with
none of this session's context (Codex was out of usage). It confirmed the
physics derivations and the exact arithmetic and found two blocking bugs, both
fixed and both now covered by public cases: fractions with parts above 2^63
were truncated before conversion to long double in the shadow oracle (P5,
large rationals, checks P(Z+) = 1/3 at the reference reading), and a 39-digit
integer overflowed the fraction parser so an out-of-range value could reach the
id check (R39, refused CASE_INVALID_PARAMETER). Its further points are applied:
a verification without `--case` is INCONCLUSIVE (E4-UNBOUND-NO-CASE-FILE), a
mutant counts as caught only on FAIL, gate control objects that fail to compile
record NOT_RUN instead of a false PASS, the isolation gate reads dynamic symbol
tables and forbids `syscall`, scaled-decimal bounds above 2^64 convert exactly,
and the shadow bound follows the long double width.

## Rulings applied after the first qualification run (omega#358, 2026-10-09)

- D3 (Agent 0): `control_kind` with a token outside POSITIVE/NEGATIVE, and a
  `clock_label_count` larger than the number of `clock_label` lines, are shape
  failures (section 1 line grammar), so step 1 fires first: CASE_PARSE_ERROR,
  not CASE_INVALID_PARAMETER. The codec and the manifest rows R31 and R36 now
  say so. Ambiguity A2 (rational beyond the limit is a range failure) is
  unaffected: a well-formed integer that is too large is not a grammar failure.
- D5 (Agent 5): the isolation gate grepped `std..time`, which never matches a
  real Rust symbol (legacy mangling writes `_ZN3std4time...`, v0 mangling
  `_RNv...3std4time...`). The gate now scans the raw and the demangled symbol
  tables for both mangling schemes and the `std::time`, `SystemTime`, `Instant`
  spellings, and `run.sh` builds three Rust controls from the oracle compute
  crate (clean, std::time mutant, extern "C" clock_gettime mutant) whenever
  rustc is available. See ISOLATION_REPORT.md for the G2 sign-off.
