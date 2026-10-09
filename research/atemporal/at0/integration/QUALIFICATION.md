# AT-0 qualification report (Agent 5, first run)

**Status: FIRST RUN RECORDED. G4 FAIL (11 of 12 positive cases). G2 INCONCLUSIVE (runner checks pass; Agent 4 sign-off pending, D5). G5 PASS (8 of 8). G6 INCONCLUSIVE (19 of 19 produced results verified; P5 produced none). G7 NOT_RUN (Agent 6).**
Program status moves from NOT_RUN to "first run recorded, one candidate defect open". This is software conformance of the
AT-0 implementation to its frozen contracts. It is not a result about time, gravity or quantum mechanics, and no PASS here may be
cited as one (charter section 1; omega#358 "Decision sought").

Evidence: `evidence/AT0/20261009T234832Z-a4ff532/` (62 case copies, 19 results per pass over two passes, 24 mutant files, 39 component files, 19 verifier JSONs,
receipts, `run.log`). Runner: `research/atemporal/at0/integration/run.sh`. Reproduce: `make at0-check` (or `sh research/atemporal/at0/integration/run.sh --evidence`).

## 1. What was inspected

| Item | Pin |
|---|---|
| omega start | `216b817` (main after Agent 4's #362); the run itself is on branch `at0/agent5-integration` commit `72ddbca` (runner only; agent directories byte-identical to `216b817`) |
| contracts | aien-architecture `fe86e43` (AT0_CASE_V1 `d90af74b...`, AT0_RESULT_V2 `bd0f9eb8...`; `c7a7181` holds the same bytes), pinned in `contract.lock`; spec `AT0_SPEC.md` `68f47e2`; charter read at `68f47e2` |
| model (Agent 3) | `research/atemporal/at0/model/`, C11, built with its own `build.sh` (plain and ASan/UBSan) |
| oracle (Agent 2) | `research/atemporal/at0/oracle/`, Rust std only, built with its own `build.sh` (direct `rustc`, no Cargo) |
| evaluator (Agent 4) | `research/atemporal/at0/evaluator/`, C11, `make`; its `run.sh` also executed with this runner as candidate |
| not touched | the evaluator's six withheld hidden cases (`AT0_HIDDEN_DIR` pointed at a nonexistent path); every directory outside the fresh clone |

The three agent directories were read and executed, never edited. Every build happens in a fresh `git clone --no-hardlinks` of HEAD in a temp directory.

## 2. Toolchain and flags (receipts/toolchain.txt, flags.txt)

`gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0`; `rustc 1.98.1 (48a229cea 2026-09-01)`, host `aarch64-unknown-linux-gnu`, LLVM 22.1.8 (pin 1.98.1 checked, control C0); GNU Make 4.3; GNU nm 2.42; Linux 7.0.0-1019-nvidia aarch64, host `spark-b87b`.
C flags (charter section 3): `-std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L`, `-lm` only (model also `-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all`).
Rust: `rustc --edition 2021 -C opt-level=2 -C codegen-units=1 -C debuginfo=0 -C panic=abort -D warnings`. Binary digests are in `receipts/binaries.sha256`; two independent clean builds in different temp directories produced
byte-identical model, oracle, evaluator and assembler binaries (compared digests of the earlier dry run and this run).

## 3. How a result file is made (and what this runner decides)

`candidate_run.sh <case>`: `at0-model <case>` writes the engine component output; `at0-oracle emit <case>` writes the oracle record; `at0-assemble` takes the case's two blocks verbatim from the case file, the
engine's `numerics` and values lines unchanged, **only the `reference` lines** of the oracle record (it refuses an engine output that already contains `reference` lines, an oracle record for another case, an oracle record not marked `oracle ...`, and any identity mismatch: control C18), then decides the ten section 4 checks itself and writes `verdict`, `verdict_id`, provenance and `evidence_digest`.
The judge (`at0_assemble.c`, C11) lifts every binary64 and scaled decimal to an integer multiple of 2^-1074 x 10^-40 in a 2304-bit integer and compares exactly. It is a third implementation of section 4 beside the oracle's and the evaluator's; it shares only `src/sha256.c`.
Judge rulings used: trivial kernel follows the fixed list of AT0_RESULT_V2 section 4 (checks 4, 6 to 10 NOT_EVALUATED); otherwise checks 7, 8, 10 run per input on DEFINED labels (Agent 0 ruling on omega#358); check 7 covers clock, Pauli and reference probabilities (evaluator A3, same reading).

## 4. Gates (charter section 6, quoted at `68f47e2`) and results

| AT0-G0 contract freeze | this charter and both contracts merged; `AT0_FREEZE.md` digests match the merged files; `AT0_SPEC.md` merged | Agents 0, 1 
| AT0-G1 codec conformance | each of the oracle, the model and the evaluator parses and re-emits the AT0_CASE_V1 section 6 example byte for byte and reproduces its published digests; every refusal case gives its exact code; C code clean under ASan/UBSan | Agents 2, 3, 4 (each for its own parser) 
| AT0-G2 isolation | symbol check passes on model objects and fails on the hidden-clock mutant; the same check, adapted for Rust (`nm` on the oracle's compiled objects for clock, random, socket, process and thread symbols outside the thin I/O layer; build log proves `std` only and no crate) passes on the oracle and fails on a Rust hidden-clock mutant, and the adaptation is verified independently by Agent 4, never skipped; `mk/at0.mk` builds with `PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0`; `make -n all` and `make -n test` print the same commands with and without `mk/at0.mk` | Agent 5 
| AT0-G3 oracle calibration | the oracle reproduces the `AT0_SPEC.md` hand tables within its stated bounds on all Pauli values and clock probabilities; oracle shares no source, object or numerical routine with the model | Agent 2 
| AT0-G4 positive arm | every P case: `outcome PASS`, `expectation_met YES` | Agent 5 runs; Agent 3 answers 
| AT0-G5 negative arm | every N case: `outcome FAIL` with exactly its expected codes, `expectation_met YES`; the axis-swap and hidden-clock mutants are caught | Agent 5 runs; Agent 4 answers 
| AT0-G6 independent verification | the evaluator's verifier, sharing no code with `model/` or `oracle/` except omega `src/sha256.c`, re-derives every check, outcome, `verdict_id` and `evidence_digest` from the result files alone and agrees; the wall-clock independence control holds; evidence is committed to a new `evidence/AT0/` folder with source and toolchain digests | Agent 5 runs; Agent 4 answers 
| AT0-G7 scientific review | `AT0_RESULTS.md` merged with PASS, FAIL or INCONCLUSIVE per claim, limitations stated, and a decision on whether an AT-1 proposal is justified | Agent 6 

Agent 5 owns G2 and runs G4, G5, G6 (section 7 and 9 of the charter). Results of this run:

| Gate | Result | Command / evidence | Detail |
|---|---|---|---|
| AT0-G2 | **INCONCLUSIVE** | controls C7-C13 in controls.tsv (isolation_check.sh and evaluator/gates/isolation.sh on model and Rust oracle objects incl. mutants; make -n hygiene; make parses with PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0) | all runner checks C7-C13 pass, but the charter requires Agent 4 independent sign-off of the Rust adaptation (not yet given) and Agent 4 gate misses the Rust std::time mutant (C10, D5): status stays INCONCLUSIVE until both are resolved |
| AT0-G4 | **FAIL** | every positive case: outcome PASS and expectation_met YES | 11/12 |
| AT0-G5 | **PASS** | every negative case: outcome FAIL with exactly its expected codes and expectation_met YES; axis-swap and Y-sign mutants caught (C5, C6); hidden-clock mutants caught (C8, C10) | 8/8 negative cases |
| AT0-G6 | **INCONCLUSIVE** | verifier agreed with every result produced, but not every valid case produced a result | evaluator PASS on 19 of 20 valid cases; 1 produced no result (see G4); evaluator self-tests, mutants and gates with this runner as candidate: 0 FAIL |
| AT0-G7 | **NOT_RUN** | - | Agent 6 (scientific review) owns G7; Agent 5 does not run it |

G0 (contract freeze), G1 (codec conformance, Agents 2 to 4 each for their parser) and G3 (oracle calibration, Agent 2) are not Agent 5 gates and were not claimed; the runner witnessed parts of them (controls C1, C17 and the refusal table below).
G2 is INCONCLUSIVE by rule: every runner check C7-C13 passes, but the charter requires Agent 4 to sign off the Rust adaptation independently ("never skipped"), that sign-off is not given, and Agent 4 gate itself misses the Rust std::time mutant (D5). Requested on omega#358.
G6 is INCONCLUSIVE: the verifier agreed with every result the runner could assemble (19 of 19), but P5 produced no result file because the model stopped (D1), so not every valid case was re-derived.

## 5. Controls (receipts/controls.tsv; every one has its command in that file)

| Control | Result | What it shows |
|---|---|---|
| C0-CONTRACT-DIGESTS | NOT_RUN | no local aien-architecture clone given; digests only recorded in contract.lock |
| C0-RUSTC-PIN | PASS | rustc 1.98.1 (48a229cea 2026-09-01) (pin 1.98.1) |
| C1-MODEL-TESTS | PASS | plain rc=0, ASan/UBSan rc=0 (at0-tests: 12/12 passed) |
| C1-ORACLE-TESTS | PASS | rc=0 (test result: ok. 14 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out; finished in 0.01s) |
| C1-ORACLE-OWN-ISOLATION | PASS | Agent 2's own gate rc=0 (ISOLATION PASS) |
| C2-REPEATABILITY | PASS | 19 results: values blocks byte-identical 19/19, verdict_id equal 19/19, case_id/acceptance_id equal, evidence_digest differs 19/19 |
| C3-WALLCLOCK-INDEPENDENCE | PASS | equal case_id, acceptance_id, verdict_id and values block; different evidence_digest |
| C4-EVALUATION-ORDER | PASS | 19/19 bit-identical (labels evaluated in reverse order) |
| C5-AXIS-SWAP-MUTANT | PASS | 11/11 caught; every P1 case caught except: none |
| C6-Y-SIGN-MUTANT | PASS | 8/11 caught; not caught (swap invisible on that case, see QUALIFICATION.md): P1f-y-axis-rotation P2-tilted-h0 P4-degenerate-identity-h |
| C7-ISOLATION-MODEL-CLEAN | PASS | 10 compute objects clean under both scanners; at0_case.o clean apart from the case-file reader (strict scan rc=1: HIT /tmp/at0-run.KXC1UZ/iso/clean/at0_case.o: fopen ), which the charter wants in a thin outer layer (see QUALIFICAT... |
| C8-ISOLATION-MODEL-HIDDEN-CLOCK-MUTANT | PASS | caught by both: HIT /tmp/at0-run.KXC1UZ/iso/mut/at0_engine_hidden_clock.o: clock_gettime  |
| C9-ISOLATION-ORACLE-CLEAN | PASS | 1 rlib members clean under both scanners (compute crate src/at0 only; main.rs is the I/O layer and is excluded by design) |
| C10-ISOLATION-ORACLE-HIDDEN-CLOCK-MUTANTS | PASS | own scanner flags both; evaluator gate flags std-time mutant rc=0 and extern-clock mutant rc=1 (0 would mean not flagged) |
| C11-MAKE-N-HYGIENE | PASS | byte-identical in all four comparisons (all: 64 lines, test: 64 lines; exit 2 both ways because the physics checkout is absent, a pre-existing condition) |
| C12-MAKE-DATABASE | PASS | empty diff: no variable, rule, target or prerequisite of the existing build changes (SRCS, all, test, clean untouched) |
| C13-MAKE-PARSES | PASS | both parse and plan: sh research/atemporal/at0/integration/run.sh --out build/at0/check |
| C14-EVALUATOR-HARNESS | FAIL | rc=1 ---- summary: pass=34 fail=7 inconclusive=9 blocked_no_candidate=0 not_run=0  (self-test overall FAIL; isolation UNISOLATED) CAND-CODEC-CONFORMANCE                       candidate FAIL                 5 of 62 differ;CAND-RUN-... |
| C15-EVALUATOR-WALLCLOCK | PASS | Agent 4's own wall-clock control passes on this runner |
| C16-EVALUATOR-ISOLATION-CAND | PASS | passes |
| C17-JUDGE-CALIBRATION | FAIL | identical 20/21; differ: n1-uncovered (see QUALIFICATION.md discrepancies) |
| C18-ASSEMBLER-REFUSALS | PASS | all refused with exit 3 and the expected message |
| C19-REFUSAL-AGREEMENT | FAIL | 5 of 42 refuse cases differ (or a refuse case was accepted); see cases.tsv and QUALIFICATION.md D2, D3 |

Charter "postulate-derived controls": hidden clock read (C8 model, C10 oracle: caught), wall-clock independence (C3: 19 of 19 results equal `case_id`, `acceptance_id`, `verdict_id` and values block; `evidence_digest` differs 19 of 19; second pass ran 2 s later with `TZ=Asia/Tokyo`),
evaluation order (C4: `--reversed` output bit-identical on 19 of 19), axis swap (C5: caught on every positive case, 11 of 11), plus a Y-sign mutant (C6: caught on 8 of 11; the 3 uncaught are P1f, P2, P4, where Agent 4 documents that <Y> stays 0 at every sampled reading; the KAT, P1e and P1g catch it).
Mutants are copies built in the temp directory from edited copies of `at0_observable.c` / `at0_engine.c`; the model sources are untouched.

## 6. Cases: expected vs got

Expected outcome and codes are the ones written into each case's acceptance block by Agent 4 (taken from AT0_SPEC section 13). I checked them against section 13.3 at class level:
P cases PASS with no codes; N1 `TRIVIAL_PHYSICAL_STATE`; N2 `SCHRODINGER_DEVIATION_EXCEEDED`; N3 `POVM_NORMALIZATION_EXCEEDED`; N4 `POVM_NORMALIZATION_EXCEEDED,PROBABILITY_SUM_EXCEEDED` (N4b w=5 also `PROBABILITY_OUT_OF_RANGE`, as section 13.3 says for w > N); N5 `BOUND_KIND_INSUFFICIENT`. N6 and N7 are Agent 4 additions (N7 expected `CONDITIONAL_UNDEFINED` per Agent 0's per-input ruling).
The evaluator's instances differ from the spec's numeric instances in places (for example its P1b is N=4, tau 1/8, M=8, the spec's is N=2, tau 1/3); I did not re-derive the numeric tables of section 13: numeric truth here rests on check 10 against the oracle's independent reference at tolerance 1e-12, the evaluator's long-double shadow oracle, and the oracle's own hand-table tests (14 pass).

Valid cases (20): outcome, codes, `expectation_met`, evaluator verdict on the assembled file, oracle's own record outcome.

| Case | Expected | Got | Met | Evaluator (verify of assembled file) | Oracle own record |
|---|---|---|---|---|---|
| N1-uncovered-spectrum | FAIL TRIVIAL_PHYSICAL_STATE | FAIL TRIVIAL_PHYSICAL_STATE | YES | PASS | FAIL |
| N2-half-covered | FAIL SCHRODINGER_DEVIATION_EXCEEDED | FAIL SCHRODINGER_DEVIATION_EXCEEDED | YES | PASS | FAIL |
| N3-broken-clock-tau3 | FAIL POVM_NORMALIZATION_EXCEEDED | FAIL POVM_NORMALIZATION_EXCEEDED | YES | PASS | FAIL |
| N4-wrong-weight-2 | FAIL POVM_NORMALIZATION_EXCEEDED,PROBABILITY_SUM_EXCEEDED | FAIL POVM_NORMALIZATION_EXCEEDED,PROBABILITY_SUM_EXCEEDED | YES | PASS | FAIL |
| N4b-wrong-weight-5 | FAIL POVM_NORMALIZATION_EXCEEDED,PROBABILITY_OUT_OF_RANGE,PROBABILITY_SUM_EXCEEDED | FAIL POVM_NORMALIZATION_EXCEEDED,PROBABILITY_OUT_OF_RANGE,PROBABILITY_SUM_EXCEEDED | YES | PASS | FAIL |
| N5-precision-demand | FAIL BOUND_KIND_INSUFFICIENT | FAIL BOUND_KIND_INSUFFICIENT | YES | PASS | FAIL |
| N6-zero-kernel-component | FAIL TRIVIAL_PHYSICAL_STATE | FAIL TRIVIAL_PHYSICAL_STATE | YES | PASS | FAIL |
| N7-unreachable-labels | FAIL CONDITIONAL_UNDEFINED | FAIL CONDITIONAL_UNDEFINED | YES | PASS | FAIL |
| P1-kat-ideal-qubit-n4 | PASS none | PASS none | YES | PASS | PASS |
| P1b-ideal-n4-tau8-m8 | PASS none | PASS none | YES | PASS | PASS |
| P1c-ideal-n6-m6 | PASS none | PASS none | YES | PASS | PASS |
| P1d-ref-offset-t2 | PASS none | PASS none | YES | PASS | PASS |
| P1e-x-axis-rotation | PASS none | PASS none | YES | PASS | PASS |
| P1f-y-axis-rotation | PASS none | PASS none | YES | PASS | PASS |
| P1g-complex-psi-yplus | PASS none | PASS none | YES | PASS | PASS |
| P2-tilted-h0 | PASS none | PASS none | YES | PASS | PASS |
| P2b-h0-shift-control | PASS none | PASS none | YES | PASS | PASS |
| P3-spec-reference-model | PASS none | PASS none | YES | PASS | PASS |
| P4-degenerate-identity-h | PASS none | PASS none | YES | PASS | PASS |
| P5-large-rationals | (expected PASS) | ERROR model failed rc=1: AT0_ENGINE_ERROR ERR_OVERFLOW | NO | NOT_RUN | PASS |

Count: positive 11 of 12 got PASS with expectation met (P5 ERROR); negative 8 of 8 got FAIL with exactly the expected codes. Component outputs are in `components/`, results in `results/pass1/` and `results/pass2/`, verifier JSON in `verify/`.

Refusal cases (42): the model and the oracle each answered every file with `AT0_CASE_REFUSED <code>`; both agree with the evaluator's manifest on **37 of 42**. The five that differ:

| File | Manifest (evaluator) | Model | Oracle |
|---|---|---|---|
| R17-noncanonical-integer | CASE_NONCANONICAL | CASE_PARSE_ERROR | CASE_NONCANONICAL |
| R30-negative-zero | CASE_NONCANONICAL | CASE_PARSE_ERROR | CASE_NONCANONICAL |
| R31-control-kind-bad | CASE_INVALID_PARAMETER | CASE_PARSE_ERROR | CASE_PARSE_ERROR |
| R36-label-count-mismatch | CASE_INVALID_PARAMETER | CASE_PARSE_ERROR | CASE_PARSE_ERROR |
| R37-model-family-bad | CASE_INVALID_PARAMETER | CASE_INVALID_PARAMETER | CASE_PARSE_ERROR |

## 7. Discrepancies between the agents' artifacts and the contracts (recorded, not normalized away)

- **D1 (blocks G4). Model `ERR_OVERFLOW` on P5 (large rationals).** `at0-model` exits 1 with `AT0_ENGINE_ERROR ERR_OVERFLOW` on a case the contract accepts and the evaluator and oracle handle (oracle's own record: PASS). The model README says no valid case within the contract limits is known to hit this. Owner: Agent 3.
- **D2. Model refusal code for non-canonical integers.** R17 (`noncanonical integer`) and R30 (`-0`) are refused as `CASE_PARSE_ERROR` by the model; the oracle and the evaluator say `CASE_NONCANONICAL` (AT0_CASE_V1 section 4 rule 1: "parsed, but some token is not in canonical form"). Owner: Agent 3.
- **D3. R31 (bad `control_kind` value) and R36 (label count mismatch).** Model and oracle independently answer `CASE_PARSE_ERROR`; the evaluator's manifest says `CASE_INVALID_PARAMETER`. Section 4 rule 1 ("shape") against rule 3 ("ranges and fixed values") is genuinely ambiguous for these two; two of three implementations agree on rule 1. For Agent 0 to rule, then Agent 4 or Agents 2 and 3 adjust.
- **D4. Oracle fixture `n1-uncovered`.** The oracle's trivial-kernel record evaluates check 7 (`probability_range`) as PASS; AT0_RESULT_V2 section 4 fixes check 7 as NOT_EVALUATED for a trivial kernel. The assembler and the evaluator follow V2. Calibration C17: 20 of 21 oracle fixtures reproduce the oracle's own verdict block and `verdict_id` byte for byte; `n1-uncovered` is the single difference. This does not affect any assembled result (the runner uses only the oracle's `reference` lines). Owner: Agent 2.
- **D5. Evaluator isolation gate, Rust blind spot.** `evaluator/gates/isolation.sh` reads symbols with `nm` without demangling and tests `std\.\.time`; on the Rust mutant that calls `std::time::SystemTime::now` (rlib member) it returned PASS (exit 0), while this runner's independent scanner flags the legacy-mangled `3std4time` symbol. The same gate does flag the `extern "C" clock_gettime` mutant. The oracle's own `isolation.sh` catches the std-time mutant only by its source grep, not by `nm`. Owner: Agent 4 (gate), Agent 2.
- **D6. File reader inside the parser object.** `model/at0_case.c` contains `at0_case_read_file` (`fopen`, `fread`, `fclose`, `ferror`), so the object that holds the AT0_CASE_V1 parser imports file symbols; the charter wants file I/O in a thin outer layer. C7 therefore scans `at0_case.o` with only those four symbols exempt (strict scan: 1 hit, `fopen`) and the other 10 compute objects (incl. `sha256.c`) clean. Owner: Agent 3 (optional split).
- **D7. Oracle provenance.** `oracle emit` writes `contract_commit 044c9d1`, the commit before `AT0_RESULT_V2` existed; the evaluator allowlists only `c7a7181` and `fe86e43`. Oracle records are marked as oracle output as Agent 0 required, and standalone they grade FAIL (`E4-PROV-ORACLE-IS-ENGINE`) by design; nothing in an assembled result takes provenance from the oracle record. Owner: Agent 2 (follow-up).
- **D8. Spec pin.** Model and oracle READMEs still cite the spec draft head `0efd1a14`; `AT0_SPEC.md` merged as `68f47e2` (squash). No numeric or code-set difference was found against section 13 at class level (section 6 above); the model's hand-table tests were not re-verified by me against `68f47e2`.
- **D9. Make coupling (informational).** `build/polyglot/manifest.tsv` depends on `$(MAKEFILE_LIST)` (`mk/polyglot.mk:144`), so every `mk/*.mk` fragment, including `mk/at0.mk`, becomes a prerequisite of that manifest. That is the only difference in the make database with the fragment present (C12), and it is inherent in the `-include mk/*.mk` design.
- **D10. Pre-existing.** `make -n all` and `make -n test` end with exit 2 with and without the fragment here, because the physics checkout (`../physics`) is absent; the comparison is of the printed commands and exit codes (C11), run with `-k` so all non-physics rules print.

## 8. UNKNOWN and limits

- **Hidden set.** Not run. Agent 4 holds six withheld cases; they are for Agent 4 or Agent 6 at candidate freeze. INCONCLUSIVE from the evaluator's own run (`HIDDEN-COMMITMENT`).
- **Isolation status.** The evaluator reports UNISOLATED (same host, same account); this run did not change that. Nothing here is a blinded qualification.
- **Symbol scans see symbols only.** Inline `svc`, raw counter reads (CNTVCT_EL0), or a clock reached through a function pointer would pass `nm`. C8 and C10 prove the scanners catch a named clock call, not every way to read time.
- **One platform.** aarch64 Linux, glibc, gcc 13.3, rustc 1.98.1. The model's `cos`/`sin` depend on the C library; its bounds assume 1 ulp. No other platform was run.
- **Bounds are ESTIMATED.** The model and oracle both state ESTIMATED bounds; no RIGOROUS claim exists (N5 passes as designed).
- **Judge independence.** The assembler's judge was written by this agent after reading the contract; it agrees with the evaluator on 19 of 19 assembled results and with the oracle's verdicts on 20 of 21 fixtures (D4). It is evidence of consistent reading, not a proof of the contract reading (A3, A6 are readings; the A6 reading is Agent 0's ruling).
- **C3 note.** evidence_digest differing is guaranteed by construction (the runner writes the timestamps); the informative part is that values blocks and verdict_id are equal.
- **C11 limit.** With the physics checkout absent, make -k stops before the test recipe, so the comparison of the test recipe is empty; it proves the build commands are unchanged, not the test recipe.
- **Evidence paths.** Verifier JSON and some logs carry absolute temp paths, so their digests differ on a re-run elsewhere; values blocks and verdict_id do not.
- **Single clone.** All numbers come from one clean-clone run plus a second pass; run-to-run differences in values blocks were zero (C2).
- **Not claimed.** The physical emergence of time, gravity, or any new law; `make at0-check` exits non-zero while any gate or control FAILs (now G4, C14, C17, C19).

## 9. Files

`mk/at0.mk` (targets `at0-check`, `at0-clean`; names prefixed `AT0_`/`at0`), `research/atemporal/at0/integration/{run.sh, candidate_run.sh, case_tool.sh, isolation_check.sh, at0_assemble.c, contract.lock, QUALIFICATION.md}`, `evidence/AT0/20261009T234832Z-a4ff532/`.
