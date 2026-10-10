# AT-1 qualification report (Agent 5): first run

**Status: FIRST RUN RECORDED. Results: G1 FAIL (refusals 71/72, the X7 row only, discrepancy D1, ruled by Agent 0, oracle fix pending); G2 PASS; G3 PASS; G4 PASS (12/12); G5 PASS (12/12); G6 PASS; G0 REFERENCE; G7 and G8 NOT_RUN. Controls C0 to C16 (23 rows): all PASS, none failing.**

This is software conformance of the AT-1 implementation to its frozen contracts (AT1_CHARTER.md section 1). It is not a result about time or physics. PASS means spec conformance only. The run covers one platform (aarch64 Linux, glibc, gcc 13.3.0, rustc 1.98.1, GNU Make 4.3, dash). The evaluator runs UNISOLATED (same host and same account as the candidates), so nothing here is blinded.

**Evidence:** `evidence/AT1/20261010T043603Z-3e18bd7/` (378 files, 2.6 MB). It holds:
- the 96 public cases with their manifest;
- `results/pass1` and `results/pass2` (engine, oracle and spliced results, plus TABLE.tsv and VERDICT_IDS.tsv per pass);
- the 24 oracle records the engine consumed (`components/`);
- the engine mutant results (`mutants/`);
- `receipts/` (controls.tsv and gates.tsv with every command, contract digests, sha256 of sources, binaries, cases and outputs, toolchain and flags, every isolation and make-hygiene log);
- `run.log`.

**Pins:**
- **Qualified omega commit:** `29e3c373e9a21f861a0914b424c7fbde6994c6ce` (main after #374; builder PRs #372 oracle `790e1ef`, #373 engine `cda98d2`, #374 evaluator `29e3c37`).
- **Commit the run executed:** this PR's `3e18bd71b2d13641be56ccab42cacc447541362c`. It is 29e3c37 plus `mk/at1.mk` and this directory only. Control C0-COMPONENTS-AT-QUALIFIED-COMMIT checks that `git diff --quiet 29e3c37 HEAD` holds over every component path. The run used a fresh clone of that commit, and both the starting checkout and the clone were clean (`source_tree_clean YES`, `starting_checkout_clean YES`).
- **Contract:** aien-architecture `cbe4c8ed88d28bb96d5209327ecd30aa9cddaf7a` (AT1_CASE_V1, AT1_RESULT_V1, AT0_RESULT_V2), spec `81047f52850f4bd14c2fc5772ec2ac833ac694b9`, charter readings (c), (d), (e) at `ea91d7c`. The digests are pinned in `contract.lock`.

## Q1. Commands and exit status
| Command | Exit | Output tail |
|---|---|---|
| `quietlock check` (before the run) | 0 | no hold |
| `setsid nohup env AT1_ARCH_DIR=<aien-architecture clone at ea91d7c> AT1_QUALIFY_COMMIT=29e3c37... sh research/atemporal/at1/integration/run.sh --evidence` (the recorded run, 2026-10-10 04:36Z) | 1 (from G1 alone; the script's last line is the test `gates failing = 0 and controls failing = 0`) | `controls failing: 0; gates failing: 1` |
| `make at1-check` (same commit, after the recorded run; no architecture clone, so C0-CONTRACT-DIGESTS is NOT_RUN) | make 2 (`[mk/at1.mk:26: at1-check] Error 1`, that is, run.sh exit 1) | `controls failing: 0; gates failing: 1` |

Every command the runner executed is in `receipts/controls.tsv`, `receipts/gates.tsv` and `run.log`. The runner itself runs these:
- `sh oracle/build.sh` (with `test` and `lib`);
- `make` / `make test` / `make asan` in `model/`, with `CC` passed;
- `make all` / `make test` in `evaluator/`;
- the evaluator's `run.sh '<engine shim> {case} {out}' '<oracle shim> {case} {out}' cases <out>` twice. It is never given a hidden folder, and no hidden case is read.

## Q2. Gates (charter section 6)
| Gate | Result | Read off | Detail |
|---|---|---|---|
| AT1-G0 | REFERENCE | AT0_FREEZE.md AT-1 rows (Agent 0) | C0-CONTRACT-DIGESTS PASS: CASE_V1 `e62018d8...`, RESULT_V1 `3e6efd7e...`, RESULT_V2 `bd0f9eb8...` via `git show cbe4c8e:`, SPEC `9a4c420b...` via `git show 81047f5:`. These equal contract.lock and the AT0_FREEZE.md rows read with `git show ea91d7c:` |
| AT1-G1 | **FAIL** | evaluator TABLE.tsv `# AT1-G1` | refusal PASS 71 FAIL 1. Failing row: `refuse/X7-clock-dim-0-none.case`, where the engine refuses exactly and the oracle prints `AT1_CASE_REFUSED CASE_PARSE_ERROR` (want CASE_INVALID_PARAMETER). See D1 |
| AT1-G2 | PASS | controls C9 to C16 and C1-ORACLE-OWN-ISOLATION | Agent 4's `gates/isolation.sh` (nm symbol scan plus `at1-eval scan-clock` instruction scan) on the engine and oracle compute objects. Clock-symbol, counter-read and raw-syscall mutants are caught for both C and Rust. The gate's own controls behave as intended. Make hygiene is byte-identical |
| AT1-G3 | PASS | evaluator TABLE.tsv `# AT1-G3` | every oracle record agrees with the evaluator's exact shadow on the 24 valid cases |
| AT1-G4 | PASS | evaluator TABLE.tsv `# AT1-G4` | positive PASS 12 FAIL 0 INDETERMINATE 0 |
| AT1-G5 | PASS | evaluator TABLE.tsv `# AT1-G5`, plus `MUTANT_RECEIPT: PASS` | negative PASS 12 FAIL 0 INDETERMINATE 0. The evaluator's mutant receipt: 244 cells, 0 unexpected |
| AT1-G6 | PASS | evaluator lines on this host | `corpus-check: 96 files, 0 differ`, `SELFTEST: PASS (550 assertions, 0 failed)`, `MUTANT_RECEIPT: PASS`, G3, G4 and G5 PASS, `T9 PASS`. The evaluator's run.sh exits 1 because of the G1 row alone |
| AT1-G7 | NOT_RUN | - | Agent 7 (replication on the MacBook) |
| AT1-G8 | NOT_RUN | - | Agent 6 (scientific review) |

G1 needs one rerun on the omega main that carries the oracle's one-reading fix for D1. Nothing else depends on it.

## Q3. Counts
| Class | Cases | Result |
|---|---|---|
| positive | 12 | 12 PASS (engine result, splice, evaluator verify) |
| negative | 12 | 12 FAIL with exactly their codes (judged PASS by the evaluator) |
| refusals | 72 | 71 exact for engine, oracle and evaluator codec; X7 fails on the oracle only (D1) |
| engine results per pass | 24 (plus one T9 rerun of the KAT) | pass 2 equals pass 1 on 73/73 result files, outside the three fields the contract lets differ |
| engine mutant drop_v (H loses the clock-energy term) | 18 valid cases | 15 killed, 3 blind (P1a, P1b, P1c). Each cell equals Agent 4's predicted KILLED/BLIND in `evaluator/MUTANT_RECEIPT.tsv` |
| engine mutant wrong_level (energy taken from level j-1) | 18 valid cases | 16 killed, 2 blind (P1a, P1b), matching the receipt |
| isolation mutants | 3 C (clock_gettime, inline counter read, inline svc) + 3 Rust (std::time, inline cntvct_el0 read, inline svc) | 6/6 caught |

## Q4. Controls (receipts/controls.tsv holds each command and detail)
All 23 rows PASS. They are:
- C0-CONTRACT-DIGESTS and C0-COMPONENTS-AT-QUALIFIED-COMMIT.
- C1:
  - build leaves the tree clean;
  - model `make test` 1390/0 plain and under ASan;
  - oracle 26 tests;
  - the oracle's own isolation gate;
  - evaluator `make test`.
- C2-REPEATABILITY: two passes, 2 s apart, the second under TZ=Asia/Tokyo. Only `run_started_utc`, `run_finished_utc` and `evidence_digest` differ (0/73 byte-identical, 73/73 identical outside those). TABLE.tsv and VERDICT_IDS.tsv are byte-identical.
- C3-SPLICE-AGREEMENT: the evaluator's independent splice equals the engine's, 24/24 byte for byte.
- C4-SPLICE-READING-C: all four `reference_*` families are copied verbatim in values-block order, and nothing else (24/24).
- C5-PROVENANCE-PIN: 49/49.
- C6-EVALUATION-ORDER: `--reversed` is byte-identical (24/24).
- C7-DIRECT-VERIFY: `at1-eval verify` gives 24/24 PASS, matching TABLE.tsv.
- C8 engine mutants (Q3).
- C9 to C13 isolation:
  - the strict engine set `at1_case.o at1_exact.o at1_numeric.o at1_result.o sha256.o` is clean;
  - `at1_bn.o` has only `fwrite` exempt (D2) and `at1_io.o` only `fopen`/`fread`;
  - `at1_main.o` is recorded only;
  - the oracle rlib member is clean;
  - mutants are caught, and the gate controls behave as intended.
- C14 to C16 make hygiene (Q5).

## Q5. Make hygiene (mk/at1.mk)
- `make -n -k all` and `make -n -k test`, with the fragment and in a clone with the fragment removed and committed, are byte-identical in all four comparisons. That covers stdout, stderr and exit status, with `PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0` and with defaults. Each has 64 lines and exits 2 in both trees; the exit 2 is pre-existing and unrelated to the fragment.
- The `make -pn` database diff is empty over 4429 lines once the fragment's own names are exempted: `AT1_*` variables, the `at1-check`/`at1-clean` rules, its file entry, `MAKEFILE_LIST` and `.PHONY`. `SRCS`, `all` and `test` are present.
- `make -n --warn-undefined-variables at1-check at1-clean` shows no undefined `AT1_` variable. The only warnings come from `mk/polyglot.mk` (`POLYGLOT_LANE_OBJS`), which is pre-existing.
- The fragment uses `ifndef`, `:=`, `?=` and `.PHONY` only. It has no `$(shell)`, no GNU-4 functions and nothing added to existing targets. `at1-clean` deletes only a folder whose last path component is `at1`.

## Q6. Discrepancies
| D | Status | What | Evidence |
|---|---|---|---|
| D1 | **OPEN** (ruled; fix pending outside my files) | Refusal row X7 (`clock_dim 0`, `clock_energies none`). Engine and evaluator manifest say CASE_INVALID_PARAMETER; the oracle says CASE_PARSE_ERROR. Agent 0 ruled CASE_INVALID_PARAMETER on omega#371 (issuecomment-6093816127), so the oracle's reading changes. The orchestrator lands that fix; I did not patch the oracle | G1 row above. G1 needs one rerun on the main that contains the oracle fix |
| D2 | OPEN, informational | `model/at1_bn.o` references `fwrite` (gcc lowers its out-of-memory `fputs` to stderr to `fwrite`). Agent 4's gate bans `fwrite`, so a strict scan of that object fails. C9 scans it with only `fwrite` exempt and every other engine compute object strictly | `receipts/isolation-engine-bn.log`. Posted for Agents 3 and 4 on omega#371 (issuecomment-6093822760) |

Also recorded on omega#371 by Agent 0 (no action here): Agent 3's ERROR label token is `UNDEFINED`, and Agent 1's spec 13.6 items 1 to 3 are closed by readings (c), (d) and (e). C4 confirms that all four families are spliced.

## Q7. Hidden set
Not read by this run. The evaluator is given only the public `cases/` copy, and no fifth argument. Agent 7's hidden-set commitment is on omega#371.

## Q8. UNKNOWN and declared limits
- **One platform.** The run covers aarch64 Linux only, with gcc 13.3.0, rustc 1.98.1, GNU Make 4.3, dash and sha256sum. GNU Make 3.81, Apple clang and `shasum -a 256` were NOT exercised, because the MacBook was unreachable over ssh. The runner and fragment use only constructs those support (`shasum -a 256` when sha256sum is absent). [UNKNOWN] whether they pass there; G7 (Agent 7) covers it.
- **Intel Mac.** The Rust raw-syscall isolation mutant (C12) has inline-asm forms for aarch64 Linux, aarch64 macOS and x86_64 Linux only (the counter mutant covers aarch64 and x86_64), so C12 fails on x86_64 macOS, as does Agent 4's C mutant file there.
- **UNISOLATED evaluator.** It runs on the same host and account as the candidates: not a blinded qualification.
- **Bounds.** The engine's numeric bounds are ESTIMATED by the builders' design. No RIGOROUS claim is made.
- **Isolation scope.** Scans cover compute objects only: symbol names plus the instruction scan for counter reads and system calls. Function pointers, dynamic loading and code outside the scanned objects are not seen. The I/O layers are excluded by design: engine `at1_main.o` and `at1_io.o`, and oracle `main.rs`.
- **Provenance times.** The engine reads no clock. The engine shim writes `run_started_utc` and `run_finished_utc` around the oracle call, before the engine runs, so `run_finished_utc` precedes the engine's own computation.
- **Exit status of the recorded run.** It is not written into the evidence folder. It follows from the log's final line and the script's last test, and the `make at1-check` exit above confirms it.
- **Recorded path.** `run.log` names the local clone path the run was cloned from. The temporary work directory is written `<work>`.
- **Engine mutants.** Only the two mutants in Agent 4's receipt (drop_v, wrong_level) are compiled from the real engine source. Their blind cells are predicted by the receipt and confirmed.
