# AT-1 G8 follow-up item 2: Agent 4's withheld set, revealed and run on both machines

Operator: AT-1 Agent 0 (Claude Code session 85679b2b on the Spark; the Mac leg over ssh). Date 2026-10-10.
Frozen candidate: omega `ff814662a19cf7da6b183f3e667c6c921e0ead21` (the G7/G8 freeze), built from a fresh clone on each machine. No contract, oracle, engine or evaluator file was edited. The frozen record under `evidence/AT1/` is untouched; this folder is additive (AT1_RESULTS.md section 5 item 2).

## What was done

1. Reveal. Agent 4's 27 case files (`cases/H01.case` to `H27.case`) and the 256-bit seed (`seed.hex`) come from `~/at1-private/agent4/` on the Spark, outside every repository, where Agent 4 left them on 2026-10-09. The manifest recomputed from the revealed files (`HIDDEN_MANIFEST.txt`, AT1_SPEC 12.3 item 3 form) hashes to the committed value in `research/atemporal/at1/evaluator/HIDDEN_COMMITMENT.txt`:
   `91ad47bfb3d411268f64523dace7eef03a5b8915ee2df821ffa1822701745c38` (recomputed independently by `hidden_run.sh` on each machine and by the evaluator's own `run.sh`, which refuses on a mismatch).
2. Regeneration. `at1-eval hidden-gen <seed> <dir>` on the Spark reproduces all 27 files byte for byte (`REGEN_MANIFEST.txt` equals `HIDDEN_MANIFEST.txt`).
3. Run. `hidden_run.sh` writes the engine's static provenance lines exactly as `integration/run.sh` sections 3 and 4 do, then calls the evaluator's `run.sh` with the two integration shims: corpus check, selftest, mutant receipt comparison, the public corpus (24 cases), then the hidden directory. Each machine built its own binaries from the frozen source.

| leg | host | toolchain | binaries (at1-model, at1-oracle, at1-eval) | run.sh exit |
|---|---|---|---|---|
| Spark | spark-b87b, Linux 7.0.0-1019-nvidia aarch64 | cc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0; rustc 1.98.1 (48a229cea 2026-09-01) | c1876340a6ea a0f0db96c8d8 694f53a75ad3  | 0 |
| Mac | Macbook.local, macOS 26.6.2 arm64 | Apple clang version 17.0.0 (clang-1700.6.4.2); rustc 1.97.1 (8bab26f4f 2026-07-14) | 92d2755cec6e 6348fbb5f717 a95eb1d556fd  | 0 |

## Result

- Hidden set: 27/27 cases agree between the two machines on derived outcome, codes, case verdict and `verdict_id` (`COMPARISON.tsv`; the two `qualify-hidden/TABLE.tsv` files are byte-identical). 0 cases derive PASS (the POSITIVE arm) and 0 derive FAIL with their codes (the NEGATIVE arm); every case verdict is PASS, meaning the candidate did on each case exactly what the specification requires, including the specified failures of the ideal prediction.
- Public corpus, same binaries, same run: 24/24 `verdict_id` identical across machines (`qualify/VERDICT_IDS.tsv`), tables byte-identical.
- Gates printed by the evaluator on both machines: G1 PASS, G3 PASS, G4 PASS, G5 PASS; G2 (isolation) is not judged by this runner and was judged at the freeze; T9 not run here (the hidden directory holds no worked example, by construction).
- `verdict_id` covers `case_id`, `acceptance_id` and the verdict block only (`evaluator/src/result.rs`, `compute_verdict_id`), so it can agree across hosts while provenance lines differ; that is what makes the cross-machine comparison meaningful.

## Control kept on purpose

`mac-run1-dirty-tree/`: the first Mac run had the case files, this script and its output inside the clone, so `source_tree_clean NO` went into the provenance and the evaluator marked all 51 cases INDETERMINATE with finding `E4-PROV-DIRTY-TREE` and exited 1, while outcomes, codes and `verdict_id` were already identical to the Spark. The rerun from a clean clone (`mac/`) is the leg graded above. The failed run shows the provenance check firing on a real impurity; it is recorded, not hidden.

## Claims, tagged

- OBSERVED: commitment match, regeneration match, 27/27 and 24/24 cross-machine agreement, exit codes, the E4 control.
- INFERRED (supported): the withheld-case gap in AT1_RESULTS.md section 4 item 10 is closed in the reproducibility sense; a second platform derives the same discrete verdicts on cases the candidate authors never saw before the freeze.
- NOT CLAIMED: anything about nature. PASS means spec conformance of a finite clock-diagonal Page-Wootters model (AT1_RESULTS.md C12 stays NOT TESTED). Separation is by machine, commitment and folder, not by identity: the same human operator's orchestrator ran both legs under one GitHub account, so C10 stays INCONCLUSIVE (section 5 item 5 is Drake's call).

## Limits

- Values were judged by the evaluator's exact shadow model (G3 PASS on every oracle record); no separate exact table file like Agent 7's DETAILS.txt exists for this set, so there is no third numeric cross-check here.
- The Mac leg used stock `/bin/sh`, Apple clang 17 and GNU Make 3.81 with no wrappers; the sanitizer leg was not run on either machine in this folder.
- Both platforms are arm64.

Files: `SHA256SUMS` covers every file in this folder except itself.
