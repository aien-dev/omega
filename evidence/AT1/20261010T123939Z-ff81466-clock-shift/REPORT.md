# AT-1 G8 follow-up item 4: shifted-system-clock control (and the sanitizer leg, by record)

Operator: AT-1 Agent 0 (Claude Code session 85679b2b), Spark only, 2026-10-10. Frozen candidate omega `ff814662a19cf7da6b183f3e667c6c921e0ead21`, the same fresh clone and the same three binaries as the item 2 run (`baseline/binaries.sha256` equals `clock-2028/binaries.sha256` and `clock-2023/binaries.sha256`). Evidence only, additive; nothing frozen was edited.

## Shifted clock: what was done

AT1_RESULTS.md section 5 item 4 asks for "the same inputs under a clock set years apart". The system clock of the Spark was not changed (it serves other agents and TLS). Instead the whole qualification step (evaluator `run.sh`, the two shims, engine, oracle and evaluator binaries, the shims' `date` calls) ran under libfaketime, which interposes the C library clock calls (`clock_gettime`, `time`, `gettimeofday`) for every process in that tree, with the wall clock set to 2028-10-10 and, in a second run, to 2023-10-10. The provenance and toolchain queries of `hidden_run.sh` ran on the real clock (`rustc --version` deadlocks under libfaketime; the first attempt hung there and was stopped, nothing written). Tool: .

Proof the shift reached the components: every engine result file carries the shifted date (`run_started_utc` baseline `2026-10-10T12:30:06Z`, 2028 run `2028-10-10T17:00:03Z`, 2023 run `2023-10-10T17:00:03Z`), and every oracle record likewise (`clock-*/qualify-hidden/oracle/`).

## Result (COMPARISON.txt, produced by compare_clock.sh)

| run | hidden verdict_id | hidden values block | public verdict_id | public values block | evidence_digest | run_started year |
|---|---|---|---|---|---|---|
| clock 2028 vs real | identical 27/27 | identical 27/27 | identical 24/24 | identical 24/24 | differs 51/51 | shifted 51/51 |
| clock 2023 vs real | identical 27/27 | identical 27/27 | identical 24/24 | identical 24/24 | differs 51/51 | shifted 51/51 |

Oracle records: identical apart from their two time lines and the digest, 27/27 in both runs. Evaluator gates printed in both runs: G1, G3, G4, G5 PASS, same counts as the real-clock run (positive 12, negative 12 public; positive 12, negative 15 hidden). This is the T9 rule of AT1_SPEC section 11 (identical `case_id`, `acceptance_id`, `verdict_id` and values block; `evidence_digest` must differ whenever the covered evidence bytes differ) measured under a clock years away rather than under a time-zone change only.

## Sanitizer leg

The frozen Spark record already ran it: control `C1-MODEL-TESTS` in `evidence/AT1/20261010T043603Z-3e18bd7/receipts/controls.tsv` and `evidence/AT1/20261010T044344Z-18e1786/receipts/controls.tsv` reads "plain rc=0 (1390 passed, 0 failed); asan rc=0 (1390 passed, 0 failed)" (`make test; make asan` on the engine). The platform where it does not work is macOS 26.6.2 (AT1_RESULTS.md section 4 item 5); that remains a Mac limit, not a gap in coverage.

## Claims, tagged

- OBSERVED: the table above; binaries unchanged across the three runs; run.sh exit 0 in all three.
- INFERRED (supported): the engine, oracle and evaluator derive nothing from the wall clock on these 51 cases; a clock five years apart in either direction changes only the timestamp lines and, through them, the evidence digests.
- LIMITS: libfaketime is an interposition on dynamically linked clock calls, not a change of the hardware clock; a program reading the clock through a raw system call or the virtual counter would not see the shift. Gate G2 (isolation scans for `clock_gettime`, the counter instruction and raw syscalls in the compute objects) covers that side and was judged at the freeze. Spark only, arm64 only. Nothing here is evidence about nature; PASS is spec conformance of the finite clock-diagonal model.
