# AT-1 G7 replication on the MacBook (Agent 7)

PASS here means conformance to AT1_CASE_V1, AT1_RESULT_V1 and AT1_SPEC only. Nothing in this folder
is a physics claim.

- Freeze: omega `ff814662a19cf7da6b183f3e667c6c921e0ead21` (tree `298bae1846b4c2237b6dad7461a8a5ee825bad2e`,
  equal to GitHub's tree for that commit), fetched alone into a fresh repository on the Mac (1 commit, clean).
  Contract `cbe4c8e`, spec `81047f5` (contract.lock); aien-architecture clone `ea91d7c`.
- Host: MacBook Air (Mac14,2, Apple M2, arm64), macOS 26.6.2 (25G83). Toolchain: HOST-TOOLCHAIN.txt.

## 1. Verdict per gate

| gate | Mac result | basis |
|---|---|---|
| AT1-G0 | REFERENCE | C0-CONTRACT-DIGESTS PASS against arch `ea91d7c` |
| AT1-G1 | PASS | public: 72/72 refusal rows; hidden: 6/6 refusals exact in codec, engine and oracle |
| AT1-G2 | PASS (with substitution 3) | C9 to C12 PASS; all six isolation mutants caught; evaluator UNISOLATED (same host and account) |
| AT1-G3 | PASS | public: 24 oracle records; hidden: 16 (15 hidden + public KAT) |
| AT1-G4 | PASS | public 12/12; hidden 8/8 (7 hidden + KAT) |
| AT1-G5 | PASS | public 12/12 + MUTANT_RECEIPT 244 cells, 0 unexpected; hidden 8/8 |
| AT1-G6 | PASS | run.sh rc 0, SELFTEST 550/0, corpus-check 96 files 0 differ, T9 PASS (public and hidden) |
| hidden set | 21/21 match | every outcome, code, twelve check statuses, case_id, acceptance_id and value agrees with the committed set |
| commitment | recomputes | `63b2edf061ffe88f4560a31391ed4945e71ed3e607aab87c2b932c7e2438a2e6`; seed regenerates 23/23 files byte-identical |

**G7: PASS** (spec conformance only), with three recorded build substitutions and one declared limit.

## 2. Commands and exit codes (full log: receipt-commands.log)

| # | command (in the Mac clone) | exit |
|---|---|---|
| 1 | `git init; git fetch --depth 1 origin ff81466...; git checkout --detach FETCH_HEAD` | 0, 0, 0 |
| 2 | run 1: `AT1_ARCH_DIR=... make at1-check` | none: blocked in `make asan` (section 4), left running, never killed |
| 3 | run 2: `CC=cc-nosan ... make at1-check AT1_OUT=build/at1-nosan` | 2 (run.sh line 338 syntax error under /bin/sh) |
| 4 | run 3: `PATH=shwrap:... CC=cc-nosan ... make at1-check AT1_OUT=build/at1-dash` | 2 (3 controls fail, G2 FAIL: Apple nm cannot read rustc objects) |
| 5 | run 4: `PATH=shwrap:nmwrap:... CC=cc-nosan ... make at1-check AT1_OUT=build/at1-dashnm` | **0**; controls failing 0, gates failing 0 |
| 6 | hidden binaries (separate fresh clone): `sh build.sh`; `make CC=cc`; `make all` | 0, 0, 0; binaries byte-identical to run 4 |
| 7 | `at1-eval qualify --cases hidden-run/cases --engine 'sh engine_shim.sh ..' --oracle 'sh oracle_shim.sh ..'` | **0** |
| 8 | `cmpvals DETAILS.txt hidden-run/out/spliced 1e-12` | **0** (15/15 AGREE) |
| 9 | `hidden-table.sh hidden-run EXPECTED.tsv` | 0 (21/21 match) |
| 10 | manifest recompute, `shasum -a 256 MANIFEST`, seed regeneration, selftest | 0, 0, 0 (23 files, 0 differ; selftest 0 failures) |

Run 4 controls: C0 digests PASS, C0-COMPONENTS NOT_RUN (runner design), C1 to C16 all PASS
(at1-check/run4-out/receipts/controls.tsv). Model plain tests 1390 passed 0 failed; oracle tests 26/0.

## 3. Build substitutions (no source file edited)

1. `sh` = `/bin/dash` through a PATH shim. macOS `/bin/sh` is bash 3.2.57, which rejects
   integration/run.sh line 338 (a `case` statement inside `$( )`) and aborts the run (run 2).
   The Spark's `/bin/sh` is dash. toolchain.txt still prints `/bin/sh`; dash is what ran.
2. `CC` = `tools/cc-nosan`, which refuses any `-fsanitize` flag and otherwise runs `/usr/bin/cc`.
   The model's own probe then prints "asan ... skipped" (section 4).
3. `nm` = llvm-nm 22.1.6 from rustup `llvm-tools` through a PATH shim. Apple nm (LLVM 17) cannot read
   the LLVM 22 bitcode that rustc 1.97.1 embeds in the oracle's objects ("Unknown attribute kind (102)"),
   so the oracle scans failed closed in run 3 (C1-ORACLE-OWN-ISOLATION, C11, C12). With the matching nm
   all three oracle mutants and all three engine mutants are caught.

## 4. Declared limits

- ASan/UBSan leg of C1-MODEL-TESTS not run on the Mac. An empty `main` built with
  `-fsanitize=address,undefined` by Apple clang 17 spins forever in `__asan::InitializeShadowMemory` on
  macOS 26.6.2 (at1-check/run1-asan-probe-hang.sample.txt). The plain model tests ran: 1390 passed, 0 failed.
- The coordinator's example limit (Rust raw-syscall mutant on an Intel Mac) does not apply: this Mac is
  arm64, and both raw-syscall mutants were caught (C10, C12).
- G2 judges compute objects only; the evaluator ran on the same host and account as the candidates.
- Mac rustc 1.97.1 versus Spark 1.98.1: binary digests are expected to differ from the Spark's. Not compared.

## 5. Discrepancies and findings for the owners

1. Folder: the coordinator asked for `evidence/AT1/<UTC>-<sha>-mac/`; charter section 7 gives Agent 7 only
   `evidence/AT1/replication-<host>/`, so the run folder is nested: `replication-macbook/20261010T051932Z-ff81466-mac/`.
2. run.sh is not portable to macOS `/bin/sh` (line 338). Finding for Agent 5.
3. The oracle isolation scripts need an nm whose LLVM can read rustc's bitcode; with Apple nm they fail
   closed (correct direction, but not runnable on a stock Mac). toolchain.txt records nm without a version.
   Finding for Agents 2 and 5.
4. `make asan` has no time limit, so a hanging sanitizer runtime blocks `make at1-check` with no exit
   (run 1 is still hung, never killed: pid 31753 `./build/probe` and its parents). Finding for Agents 3 and 5.
5. The hidden-run folder also holds the public `positive/P2-kat-rotated-level-n4.case`, so the evaluator's
   KAT digest check and T9 probe run. It is public, not part of the hidden set.

## 6. Hidden cases (HIDDEN-TABLE.tsv has every column)

| # | class | expected | Mac result | checks 1-12 | ids | values |
|---|---|---|---|---|---|---|
| 01 p1-zero-coupling-ideal | P1 | PASS | PASS | PPPPPPPPPPPP | same | AGREE |
| 02 p1c-spectator-ideal | P1c | PASS | PASS | PPPPPPPPPPPP | same | AGREE |
| 03 p2-one-rotated-level | P2 | PASS | PASS | PPPPPPPPPPNP | same | AGREE |
| 04 p3-every-level-coupled | P3 | PASS | PASS | PPPPPPPPPPNP | same | AGREE |
| 05 p3b-degenerate-level | P3b | PASS | PASS | PPPPPPPPPPNP | same | AGREE |
| 06 p4-complex-psi-ref-nonzero | P4 | PASS | PASS | PPPPPPPPPPNP | same | AGREE |
| 07 p6-near-miss-level | P6 | PASS | PASS | PPPPPPPPPPNP | same | AGREE |
| 08 n2-trivial-kernel | N2 | FAIL TRIVIAL_PHYSICAL_STATE | same | PPFNPNNNNNNN | same | AGREE |
| 09 n3-zero-label | N3 | FAIL CONDITIONAL_UNDEFINED | same | PPPPPPPPFPNP | same | AGREE |
| 10 n4a-wrong-weight | N4a | FAIL POVM_NORMALIZATION_EXCEEDED,PROBABILITY_SUM_EXCEEDED | same | PPPPFFPPPPNP | same | AGREE |
| 11 n4b-broken-clock | N4b | FAIL POVM_NORMALIZATION_EXCEEDED | same | PPPPFPPPPPNP | same | AGREE |
| 12 n6-zero-projection | N6 | FAIL TRIVIAL_PHYSICAL_STATE | same | PPFNPNNNNNNN | same | AGREE |
| 13 n1a-p2-question-ideal-target | N1 | FAIL SCHRODINGER_DEVIATION_EXCEEDED | same | PPPPPPPPPFPP | same | AGREE |
| 14 n1b-p4-question-ideal-target | N1 | FAIL SCHRODINGER_DEVIATION_EXCEEDED | same | PPPPPPPPPFPP | same | AGREE |
| 15 n1c-p6-question-ideal-target | N1 | FAIL SCHRODINGER_DEVIATION_EXCEEDED | same | PPPPPPPPPFPP | same | AGREE |
| 16 r1-per-level-irrational | R | CASE_IRRATIONAL_SPECTRUM | refused exactly | - | - | - |
| 17 r2-coupling-index-01 | R | CASE_NONCANONICAL | refused exactly | - | - | - |
| 18 r3-at0-header | R | CASE_PARSE_ERROR | refused exactly | - | - | - |
| 19 r4-domain-at0 | R | CASE_UNSUPPORTED_VERSION | refused exactly | - | - | - |
| 20 r5-coupling-count | R | CASE_PARSE_ERROR | refused exactly | - | - | - |
| 21 r6-label-index-value | R | CASE_PARSE_ERROR | refused exactly | - | - | - |

Values: 1760 engine and oracle values compared with the exact rational tables in DETAILS.txt;
largest deviation 6.106e-16 (engine) and 3.331e-16 (oracle), tolerance 1e-12, 0 status mismatches
(hidden-run/VALUES_VS_DETAILS.tsv). Refusals: engine and oracle both refused with exactly the expected code.

## 7. Reveal

hidden-set-reveal/ holds the 21 cases, EXPECTED.tsv, DETAILS.txt, PROCEDURE.txt, the generator at1h.rs,
MANIFEST, COMMITMENT and SEED. To recompute: in `hidden-set-reveal/cases`, run
`for f in $(ls | sort); do shasum -a 256 "$f"; done` and compare with MANIFEST, then `shasum -a 256 MANIFEST`.
To regenerate: `rustc -O --edition 2021 at1h.rs -o at1h && ./at1h gen $(cat ../SEED) out/` (Rust std only).
RECOMPUTE.log is the Mac run of both.
