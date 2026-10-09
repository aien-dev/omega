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
Update 2026-09-30 (Brownian Wave 1 reconciliation), based on omega origin/main `8a56ace` (the rest of this page is still the audit at `62f5ba5`): EXP-002A to 002D, Brownian interventional replication, EXP-003 and the claim ladder were updated
from the sealed Wave 1 records (section 2 row "EXP-002A to D", section 7). No evaluator-only material is quoted here.

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
| EXP-002A, 002B, 002C | PASS (Brownian Wave 1, sealed) | certification of the scorer on known stochastic cases; 002A PASS under a successor profile; 002B PASS without its Occam curve; the Occam curve itself PASS separately under profile v1.3; 002C repeated PASS on fresh sealed worlds under v1.3 |
| EXP-002D (Brownian) | INCOMPLETE | one hostile rule (row 28, mixture weights) was not evaluable with the Wave 1 prediction format; 0 wrong verdicts on the rules that were evaluated. Stage A (2026-10-04): PRD2 makes it evaluable, omega-side R28-v2 MATCH, receipts `evidence/EXP-002D/01..04`; sealed harness re-pointed at PRD2 and re-run under pin 2564f57 (aien-sealed PR #2): row 28 MATCH; still INCOMPLETE because the dev runner keeps 36 match 1 mismatch (HC-meas-lag1-control block 0 edge, 0.00313 vs null upper 0.00308). Diagnosis 2026-10-09 (evaluator side, aien-sealed draft PR #3, reproduced at aien-sealed 019c64d with omega 2564f57): no implementation fault; the mismatch is a null-control line landing just above its interval edge, which fresh independent null blocks show happens at roughly the expected tail rate, so the expectation "inside on every block" was stated too strictly. Status stays INCOMPLETE: the original result is kept, and a successor development rule with a binomial allowance on control lines is proposed (draft, not adopted). The "stop-27 TPS1-adapter gate PENDING" note in the Stage A receipt is superseded: the evaluator receipt clarification records 30 passed, 0 failed at the current pin |
| interventional replication (Brownian) | PASS | profile v1.3, fresh sealed worlds |
| EXP-003 | BLOCKED | needs a separate Wave 2 contract (draft under review, not accepted, not frozen) and must seal after BRN-10 |
| BRW-ACT-DEV0 / DEV1 (development only, not EXP-003) | DEV0: dev FAIL (P1), held-out PASS; DEV1 (successor, design defects of DEV0 fixed): dev FAIL (P1 vs random, narrowly), held-out PASS | hand-coded reference candidate chooses the next noisy measurement by expected information; on DEV1 held-out it reaches the right explanation 116 / 160 / 388 time units sooner than random / cycle / fixed (horizon 2000), gain concentrated in mean-reversion worlds, final held-out prediction not better than random or cycle; with its extra computation charged it stays ahead of random only while one time unit of measurement is worth more than about 1e6 arithmetic evaluations, 2e6 at the low end of the saving (2026-10-09 re-analysis, no rerun). Self-run by the profile author, no seal, no independent evaluator: it does not close EXP-003 or raise the level ceiling. `docs/turing/BRW_ACT_RESULTS.md` |
| R16 (orchestrator retirement) | CLOSED at candidate `850fc545`; living build IMPLEMENTED / NOT QUALIFIED | omega#112 merged (`3dd5eaa`), gates G1 to G8 PASS at that candidate (update 2026-09-30); merged with evidence-immutable FAILED, re-qualification owed (see the 2026-10-01 reconciliation section at the end) |
| TURING wired into the live runtime | NOT STARTED | the runtime edit freeze was lifted (aien-architecture #70); no wiring work exists yet |

Highest claim level supported (protocol claim ladder, section 6): **Level 2** (Brownian scope, instrument certification only; see section 6). Outside the Brownian scope the ceiling stays Level 1.

## 2. Gate table

| Gate | Commit | Receipt | Tests present (not re-run here) | Reproduce | Limitation |
|---|---|---|---|---|---|
| CAL-0 for EXP-001R | freeze commit `8e6c5ac`; receipt commit `dee3eda` | `calibration/experiments/EXP-001R/freeze_receipt.json` (profile sha256 `0267bffc...`, candidate manifest `7edcd62e...`) | `calibration/scripts/check_profile.sh`, `freeze_candidate.sh`, `freeze_receipt.sh`, `test_blinding.sh`; target `test-turing-cal-blinding` (`mk/turing_exp001_c.mk`) | scripts in `calibration/scripts/` | freeze is recorded by receipt, not by git tag (declared deviation D7) |
| EXP-001 | freeze `d3cba29`; result `d5c97ce` | `calibration/experiments/EXP-001/final_receipt.json`: verdict FAIL, S2 FAIL, S1 and S3 to S9 NOT_REACHED; `overlap_audit.json`: `crumb_digest` pass false, "1 shared" | `tests/turing/test_tc_eval.sh` (fail-closed refusal cases) | `tools/turing_cal_eval.c`, `tools/turing_cal_overlap.c` | failed before scoring, so no measurement came out of it; the result is kept, not deleted |
| EXP-001R | freeze `8e6c5ac`; result `190ff89` | `calibration/experiments/EXP-001R/final_receipt.json` (verdict PASS, `EXP_001_COMPRESSION_BRIDGE = PASS`); `REPORT.md`; `scorer_independent.json` (problems 0); `uncertainty.json` (10,000 crumb bootstrap resamples) | `tests/turing/test_tc.c`, `test_tc_produce.c` (target `test-turing-exp001-coders`, plain and ASan/UBSan builds); `test_tc_eval_dry.sh` (target `turing-exp001-eval-dry`); target `test-turing-verify-indep` | `tools/turing_cal_eval.c`, coders `src/turing/tc_range.c` and `tc_rans.c`, independent scorer `tools/turing_verify_indep/` | two sealed groups, 6,168,907 and 6,199,000 events (12,367,907 total); per-crumb bootstrap instead of trajectory-level (D4); receipts unsigned, git commit is the record (D6); candidate and evaluator run as the same OS user in separate jails (D8) |
| H5 info (TY-2) | see `git log -- evidence/TURING_YIELD` | `evidence/TURING_YIELD/ty2_heldout_receipt.txt` (`t_microbits=2559679824815`, verdict PASS); `brn_p_ty2_verify_2026-09-29.log` ("VERIFY OK"); `docs/turing/TURING_YIELD_TY2_RESULT.md` | `tests/turing/test_ty_math.c` etc. (target `test-turing-yield`, `mk/turing_yield_math.mk`) | `tools/turing_yield.c` (`heldout`, `verify`) | measured under Turing Yield profile V0, not calibration profile v1.1; verification is `turing-yield verify`, the same tool as the primary scorer, so it is a self re-derivation, not an independent scorer like EXP-001R lane D |
| H4 energy | protocol `99c1f69`; boundaries `4386897` | none in `evidence/` | `tests/turing/test_ty_energy.c`, `test_ty_energy_fixtures.sh`, `ty_energy_fixture.c`, `ty_energy_window.c` (target `test-turing-energy`, `mk/turing_yield_energy.mk`) | driver `tools/ty_energy_run.sh` (uses `tests/turing/ty_workload.c`), reducer `tools/ty_energy_reduce.c` | protocol status is PRE-REGISTERED; no stage 1 or stage 2 timed run is recorded in the repo |
| H3 | none | none standalone | bootstrap inside EXP-001R evaluation; `src/turing/replay.c` | n/a | no H3 qualification receipt |
| EXP-002A to D (Brownian Wave 1) | profile commitments published before sealed data: v1.0 omega#100, v1.1 omega#101, v1.2 omega#102, v1.3 omega#103; append-only history `docs/brownian/PROFILE_COMMITMENT_HISTORY.txt` (omega#104) | sealed results and verdicts live in the PRIVATE repo aien-dev/aien-sealed: decision record R3-49 and results-v1.3 at commit `6a90109`, erratum and archived run procedure at `6b893de` (the erratum corrects record text; no verdict changed) | in aien-sealed (evaluator code, hostile tests, verdict tests); omega carries only the TPS1 adapter `tools/brownian/brw_tps_adapter.c` and `tests/brownian/test_brw_tps.c` | not publicly reproducible yet: the profile bytes and evaluator tree are published into omega only after BRN-10 seals, and must hash to the committed digests | public summary: aien-architecture `docs/brownian-calibration-explainer.md` (merged #67, `76a77e7`); EXP-002D INCOMPLETE (one hostile rule not evaluable with the Wave 1 prediction format); this omega doc does not re-verify the sealed records |
| interventional replication (Brownian) | profile v1.3 commitment omega#103 | same private records as the row above (R3-49) | in aien-sealed | not publicly reproducible until after BRN-10, as above | fresh sealed worlds under v1.3; repeats the EXP-002C intervention phase |
| EXP-003 | none | none | none | n/a | specified in the same protocol, section starting line 760; Wave 2 contract is a draft in aien-sealed, not accepted, not frozen, and must seal after BRN-10 |

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
- No file in `src/runtime/*.c` mentions turing; TURING code stays outside the runtime (the runtime was frozen at the time of this audit).

## 4. R16 and the runtime freeze

- Update 2026-09-30: R16 CLOSED (omega#112, `3dd5eaa`, G1 to G8 PASS) and the runtime freeze LIFTED
  (aien-architecture #70, `e89ba94`). The bullets below record the state at the original audit.
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
| 2 | EXP-002A/B/C | discriminates supported from unsupported stochastic structure | met for the Brownian scope (scorer certification on known cases, sealed in aien-sealed; public replay after BRN-10) |
| 3 | interventions + EXP-002D | invariant or unifying explanatory structure | not met: interventional replication PASS, EXP-002D INCOMPLETE (last development mismatch diagnosed 2026-10-09 as a statistical edge, not a code fault; successor rule proposed, not adopted) |
| 4 | EXP-003 | explanatory models improve experimental choice | blocked (Wave 2 contract not accepted or frozen; must seal after BRN-10) |
| 5 | H4 | physical energy attribution validated | partial (no timed run) |
| 6 | H5 | explanatory efficiency measurable as T/J in tested regime | blocked on H4 |
| 7 | sensitivity | conclusion stable across reasonable profile choices | not attempted; Brownian scope: not evaluable for Wave 1 (variants were not named before sealed data), to be named in advance for Wave 2 |
| 8 | independent replication | independently reproducible metrology | not attempted (the EXP-001R lane D scorer is an independent scorer, not an independent replication); Brownian scope: BLOCKED until BRN-10 seals |
| 9 | multiple unrelated domains | broader applicability | not attempted |

Current ceiling: **Level 2**, for the Brownian scope only, and only as instrument certification: it says the scorer
behaves as specified on known stochastic cases, not that AIEN discovered anything. Level 3 is not met (EXP-002D
INCOMPLETE). The TY-2 bits result does not change the ceiling.

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
- Brownian Wave 1 reconciliation (2026-09-30): this file said EXP-002A to D NOT STARTED while the aien-architecture
  explainer (#67) reported them run. The explainer was right; the rows above now follow the sealed Wave 1 records.
  Naming caution: in the Brownian program, "H4" means GPU rows (INCOMPLETE, none in Wave 1) and "H5" means the
  six-build rescoring check (PASS); "H3" means replay and the preregistered statistical signs (both PASS). The H3,
  H4 and H5 rows in section 1 use the TURING qualification meanings (H3 replay and statistical robustness, H4 physical energy attribution, H5 information part T in bits and H5 physical part T/J) and are not changed by Wave 1.
- The Brownian independent replication (Level 8) is BLOCKED until BRN-10 seals. An in-house independent implementation is
  planned; on its own it would be an independent implementation, not an external replication, and may not be enough for Level 8 (decided when it runs).
- Section 5 is a planned order of work for TURING. Brownian Wave 1 ran as its own program, so its results above
  do not mean the earlier steps in section 5 are complete.

## Reconciliation 2026-10-01 (omega main `07004a8`)

Rows verified against merged PRs and open PR state. Nothing here upgrades a status.

| Item | State | Evidence |
|---|---|---|
| E1 numerical closure | CLOSED 2026-10-02 on the fb36109 chip campaign (merged #225 `40d1ea37`), exclusions recorded (natural-base EXP/LOG not on GB10) | `evidence/E1-CLOSURE/e7851c69d34ac777a9436af16d0bdd5d69264c8528c62d562b600f5d89153061.json`; `docs/numeric/E1_GAP_TABLE.md` closure section |
| OSC-1 and OSC-2 | IMPLEMENTED / NOT QUALIFIED; host receipts only; not self-hosting; no general compiler | #148 `0abdb08`, #149 `845dce4`, #150 `c773622`, #151 `7e713e3`; addendum in `docs/osc/OSC-1-SELF-HOST-STATEMENT.md` |
| R16 | CLOSED at candidate `850fc545` only; living build IMPLEMENTED / NOT QUALIFIED, re-qualification owed | #112 `3dd5eaa` merged with evidence-immutable FAILED (run 36799862685); `evidence/R16/inventory.json` edited in place by `1edb56b` and `6831117`; receipt `evidence/R16/22d7a79a...json`; all R13 to R16 candidate-bound receipts predate #126 `4f8485b` |
| EST-3 | FAILED on main: v1 (#105), v2 (#118), v3 (#129 `d78fd11`, binding fit Phase A FAIL, sealed run NOT_RUN) | v4 attempt open as #153; attempts 1 and 2 void, no verdict yet |
| M20 OMEGA_TENSOR | Not merged; open draft #136, not qualified; its E1 prerequisite closed 2026-10-02 | PR #136 |
| M22 substrate | NOT QUALIFIED per its own receipt and doc | #140 `54826a3`; `docs/train/M22_SUBSTRATE.md`; `evidence/M22/receipts/eaa0cdea...json` |
| PATH-1 | FAILED EXPERIMENT (M1) | see `spec/path-semantic-object.md`, note 2026-10-01 |
