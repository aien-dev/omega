# TURING scientific qualification state

Date: 2026-09-30
Repository: omega, origin/main at `62f5ba5` (Merge PR #107).
Cross-repo reference: aien-architecture origin/main at `b63297b`.
Method: read files, grep, git log. Nothing was built or run for this audit (a timing-sensitive hardware
measurement was running on the machine). Every "test" cell below therefore says what exists and what receipt or
log records a past result; it does not claim a fresh pass.
Rule: implementation and evidence beat planning claims. Where they disagree, the disagreement is written down.
Origin: a first draft was machine-written (Gemini) and then checked claim by claim against the repository. Claims
that could not be confirmed were removed or corrected; section 7 lists them.

## 1. Summary

| Item | State | One-line reason |
|---|---|---|
| CAL-0 (freeze of apparatus, profile, candidates, blinding) | PASS for EXP-001R | freeze receipt `TURING_PROFILE_V1_FROZEN = PASS` for freeze commit `8e6c5ac` |
| EXP-001 (compression bridge, first attempt) | FAIL, preserved | overlap audit gate `crumb_digest` failed ("1 shared"), terminal criterion S2 |
| EXP-001R (compression bridge, successor experiment) | PASS | S1 to S9 PASS; independent scorer compared 312 values, 0 mismatches |
| H5 information part (TY-2, T in bits) | PASS, with a limit | +2,559,679.825 bits vs order-1 baseline on held-out seeds; re-verify used the same tool |
| H4 (physical energy attribution) | PARTIAL | protocol pre-registered, code and fixture tests present; no timed run evidence in `evidence/` |
| H5 physical part (T/J) | NOT STARTED | needs H4 |
| H3 (replay, statistical robustness) | PARTIAL | bootstrap and replay code exist; no standalone H3 receipt |
| EXP-002A, 002B, 002C, 002D | NOT STARTED | protocol text only |
| EXP-003 | NOT STARTED | protocol text only |
| R16 (orchestrator retirement) | IN PROGRESS | on main only the G1/G2 inventory; G3 to G5 work sits on an unmerged branch |
| TURING wired into the live runtime | BLOCKED | runtime edit freeze until R16 closes |

Highest claim level supported (protocol claim ladder, section 6): **Level 1**.

## 2. Gate table

| Gate | Commit | Receipt | Tests present (not re-run here) | Reproduce | Limitation |
|---|---|---|---|---|---|
| CAL-0 for EXP-001R | freeze commit `8e6c5ac`; receipt commit `dee3eda` | `calibration/experiments/EXP-001R/freeze_receipt.json` (profile sha256 `0267bffc...`, candidate manifest `7edcd62e...`) | `calibration/scripts/check_profile.sh`, `freeze_candidate.sh`, `freeze_receipt.sh`, `test_blinding.sh`; target `test-turing-cal-blinding` (`mk/turing_exp001_c.mk`) | scripts in `calibration/scripts/` | freeze is recorded by receipt, not by git tag (declared deviation D7) |
| EXP-001 | freeze `d3cba29`; result `d5c97ce` | `calibration/experiments/EXP-001/final_receipt.json`: verdict FAIL, S2 FAIL, S1 and S3 to S9 NOT_REACHED; `overlap_audit.json`: `crumb_digest` pass false, "1 shared" | `tests/turing/test_tc_eval.sh` (fail-closed refusal cases) | `tools/turing_cal_eval.c`, `tools/turing_cal_overlap.c` | failed before scoring, so no measurement came out of it; the result is kept, not deleted |
| EXP-001R | freeze `8e6c5ac`; result `190ff89` | `calibration/experiments/EXP-001R/final_receipt.json` (verdict PASS, `EXP_001_COMPRESSION_BRIDGE = PASS`); `REPORT.md`; `scorer_independent.json` (problems 0); `uncertainty.json` (10,000 crumb bootstrap resamples) | `tests/turing/test_tc.c`, `test_tc_produce.c` (target `test-turing-exp001-coders`, plain and ASan/UBSan builds); `test_tc_eval_dry.sh` (target `turing-exp001-eval-dry`); target `test-turing-verify-indep` | `tools/turing_cal_eval.c`, coders `src/turing/tc_range.c` and `tc_rans.c`, independent scorer `tools/turing_verify_indep/` | two sealed groups, 6,168,907 and 6,199,000 events (12,367,907 total); per-crumb bootstrap instead of trajectory-level (D4); receipts unsigned, git commit is the record (D6); candidate and evaluator run as the same OS user in separate jails (D8) |
| H5 info (TY-2) | see `git log -- evidence/TURING_YIELD` | `evidence/TURING_YIELD/ty2_heldout_receipt.txt` (`t_microbits=2559679824815`, verdict PASS); `brn_p_ty2_verify_2026-09-29.log` ("VERIFY OK"); `docs/turing/TURING_YIELD_TY2_RESULT.md` | `tests/turing/test_ty_math.c` etc. (target `test-turing-yield`, `mk/turing_yield_math.mk`) | `tools/turing_yield.c` (`heldout`, `verify`) | measured under Turing Yield profile V0, not calibration profile v1.1; verification is `turing-yield verify`, the same tool as the primary scorer, so it is a self re-derivation, not an independent scorer like EXP-001R lane D |
| H4 energy | protocol `99c1f69`; boundaries `4386897` | none in `evidence/` | `tests/turing/test_ty_energy.c`, `test_ty_energy_fixtures.sh`, `ty_energy_fixture.c`, `ty_energy_window.c` (target `test-turing-energy`, `mk/turing_yield_energy.mk`) | driver `tools/ty_energy_run.sh` (uses `tests/turing/ty_workload.c`), reducer `tools/ty_energy_reduce.c` | protocol status is PRE-REGISTERED; no stage 1 or stage 2 timed run is recorded in the repo |
| H3 | none | none standalone | bootstrap inside EXP-001R evaluation; `src/turing/replay.c` | n/a | no H3 qualification receipt |
| EXP-002A to D | none | none | none (one `test_tc_eval.sh` case only checks that the evaluator refuses an `EXP-002` id) | n/a | specified in `docs/turing/protocols/turing-instrument-calibration-validation-protocol-v1-0.tex` sections starting at lines 479, 577, 643, 709 |
| EXP-003 | none | none | none | n/a | specified in the same protocol, section starting line 760 |

## 3. What exists in code

All paths below exist on `62f5ba5`. States are about evidence, not code quality.

- `src/turing/field.{h,c}`, `field_select.c`, `select.h`, `history_selector.c`, `replay.c`: Field records, control
  selection, replay. Tested by `tests/turing/test_turing.c` (target `test-turing`, plus ASan build). No receipt in
  `evidence/`. `field_select_v0_retired.c` is the retired V0 selector.
- `src/turing/tc_pstream`, `tc_produce`, `tc_range`, `tc_rans`, `tc_tool.c`: probability streams, producer, the two
  reference coders and the `turing-coder` tool. Their outputs are pinned by the EXP-001R receipt
  (`probability_streams/INDEX`, `arithmetic/results.json`, `ans/results.json`).
- `src/turing/ty_math`, `ty_model`, `ty_record`, `ty_ctr1` (247-byte CTR1 trace record), `ty_qrecord`, `ty_qcont`,
  `ty_prd`, `ty_profile.c`: Turing Yield math, models and records. Used by TY-2 (candidate table
  `evidence/TURING_YIELD/ty2_candidate.tym`, 5,834,845 held-out events).
- `src/turing/ty_energy.{h,c}`: H4 energy attribution. Fixture tests only (see H4 row).
- `tools/`: `turing_cal_eval.c`, `turing_cal_candidates.c`, `turing_cal_overlap.c`, `turing_cal_dev.h`,
  `turing_field.c`, `turing_yield.c`, `turing_verify_indep/`, `ty_energy_reduce.c`, `ty_energy_run.sh`.
- `calibration/`: profiles v1.0 (used by failed EXP-001) and v1.1 (EXP-001R); `schemas/`; `scripts/` (16 shell
  scripts, one C helper `power_simulation.c`, one seed list); `docs/`; `preregistration/`; `experiments/EXP-001`,
  `experiments/EXP-001R`.
- `calibration/docs/PROTOCOL_CONFORMANCE.md`: 53 protocol requirements mapped; declared deviations D1 to D10.
- There is no top-level `turing/` directory. Code lives in `src/turing/`, `tests/turing/`, `tools/`, `calibration/`,
  `docs/turing/`.
- No file in `src/runtime/*.c` mentions turing; TURING code stays outside the frozen runtime.

## 4. R16 and the runtime freeze

- On main: `evidence/R16/` holds only `inventory.json`, added in `6fdc4c3` (PR #68, R16 spec pre-registration).
  `tests/runtime/rx_r16_negative.c` is not on main. No `AIEN_RX_R16_ORCHESTRATOR_RETIRED_V1` receipt.
- aien-architecture `CURRENT_EXECUTION_PLAN.md` (at `b63297b`) line 45: R16 "IN PROGRESS ... G3-G8 qualification
  and final receipt are absent from `main`". Line 553: "No edits to `omega/src/runtime/` (or other R16-mapped
  files) until R16 (`aien-dev/omega#68`) closes."
- Not on main, not counted toward closure: branch `feat/r16-g3-g5-host` contains `tests/runtime/rx_r16_negative.c`,
  and a host run log from that branch reports `R16_G4_LEGACY_REFUSED=PASS`. The silicon G3 run was still in progress
  when this was written. Until that branch is merged with receipts, R16 stays IN PROGRESS.
- Effect on TURING: wiring Field or Turing Yield into the runtime cost model or event plane is BLOCKED until R16
  closes.

## 5. Order of work (Drake directive, 2026-09-30)

TURING measures, never authorizes. Order: re-audit, close R16 G4, finish R16, TURING runtime observation through
ARGUS (observational only), silicon T/J with T and joules separately auditable, EXP-002A, 002B, 002C, 002D,
EXP-003, then Cortex evidence, J-Space vector objectives, Evolution Arena, Physics Zero later. New TURING features
are frozen until EXP-003 qualifies. Failures are kept; a new protocol gets a new experiment id. An independent
scorer must not reuse the primary scorer.

## 6. Claim ladder

From the `Claim Ladder` section of the calibration protocol (`.tex` line 1175).

| Level | Required evidence | Permitted claim | State |
|---|---|---|---|
| 0 | definition only | mathematically specified statistic | met |
| 1 | EXP-001 | reproducibly connected to actual lossless compression in tested regime | met by EXP-001R |
| 2 | EXP-002A/B/C | discriminates supported from unsupported stochastic structure | not started |
| 3 | interventions + EXP-002D | invariant or unifying explanatory structure | not started |
| 4 | EXP-003 | explanatory models improve experimental choice | not started |
| 5 | H4 | physical energy attribution validated | partial (no timed run) |
| 6 | H5 | explanatory efficiency measurable as T/J in tested regime | blocked on H4 |
| 7 | sensitivity | conclusion stable across reasonable profile choices | not attempted |
| 8 | independent replication | independently reproducible metrology | not attempted (the EXP-001R lane D scorer is an independent scorer, not an independent replication) |
| 9 | multiple unrelated domains | broader applicability | not attempted |

Current ceiling: **Level 1**. The TY-2 bits result is real but does not lift the ceiling, because Level 2 needs
EXP-002A to C.

## 7. Known limits and corrections

- Nothing here was rebuilt or re-run. Test targets exist; past results come only from the receipts and logs named.
- `calibration/docs/PROTOCOL_CONFORMANCE.md` is stale on one row: item 50 (CAL-0 checklist) says "open (O3)"
  because the profile still had a FILL_AT_FREEZE value. The file was last changed in `c803862`, before the v1.1
  freeze. Profile v1.1 now has no FILL_AT_FREEZE and the freeze receipt is PASS.
- Corrected from the draft: base commit was `6d1ff1d`, now `62f5ba5`; "12.3 million events" is 12,367,907;
  receipts are unsigned (D6), not "signed"; deviations D1 to D10 are declared deviations, not pre-registered ones;
  EXP-001 failed on the `crumb_digest` gate with one shared digest (the draft called it training data leaking into
  the test set, which the receipt does not state); `ty_workload.c` is used by `tools/ty_energy_run.sh`, not
  "not wired in"; the runtime freeze line in the plan is 553, not 540; the claim ladder has levels 7 to 9 too.
- Removed from the draft because they could not be confirmed without running anything: "compiles with standard
  C11", "ASan clean", "1,026 round-trip cases", "429-line harness" (file has 428 lines), "zero byte divergences",
  "all refusals exit 2", "enforces 56 profile parameters", "18 shell scripts", "never committed" for `turing/`,
  and per-file QUALIFIED labels.
