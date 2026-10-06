# CAND-3: declared replacement attempt G7b (R16 G7 and G8), written 2026-10-06T14:05:01Z, before it runs

A7c (`DECLARED-ATTEMPT-A7c.md`, sha256 70824bb8b6c116a72a047c61e424169338b7dc8eb0ded9c8b65260e73ecdeec9; omega#317 f210d27, aien-architecture#146 fea3169) stays recorded as it is and is never edited.
Its results: R15 PASS 16/16 (receipt 90fdebb8bd2c8ff988c6d93afb764c3466bd6c3575cd8176fc605378fdb53fdd); R16 G7 NOT_RUN because the R11 living part refused to start at 1-minute load 2.11 (limit 2, `tests/runtime/rx_r11_aien.c` other_load); G8 NOT_RUN.

## Why a replacement
The A7c G7 result was NOT_RUN for a load refusal, not a failure of any rung (stdout.log: every other rung PASS, R15 receipt PASS). Since then, at ~14:02Z, Drake approved turning off all other AI models; the GPU has zero compute apps (nvidia-smi query-compute-apps empty; record `~/workspace/r15-practice/ai-services-off-20261006.txt`). 1-minute load at ~14:10Z was 1.37 and at writing is 0.70.
This is a new, separate attempt. It replaces nothing already recorded.

## Candidate and harness (unchanged from A7c)
Omega code f816473df391bc4fbcf99df722edd70ed26ebef1, harness checkout wt-omega-chip at 97ee27584cda0d8eaca136f31a44293cd36c9b02, physics 6d7cf0d4d8eb2cda7b512100ff6058e25dbb3ddf, aienos bbad5e4250e57f8cbd1be4cf1390109aa65ef92c. Script `cand3_g7b.sh` is `cand3_r15c.sh` with phase 1 removed and the evidence directory renamed to `...-G7b` (diff recorded in the evidence). R15 is NOT rerun: its A7c receipt-B is consumed by sha256 (the script checks outcome, commit, candidate_bound, tree_dirty, silicon_observed).

## Deviation (stated openly)
Secure Boot is OFF on this boot by Drake's decision (mokutil: SecureBoot disabled); the R15 spec clarification C2 conformance is not claimed. Energy reader aien_spbm still loaded (hwmon3), not unloaded, no reboot.
Known risk, stated before the run: the R11 rung runs after R1-R10 host tests inside the same ladder, so the ladder's own earlier rungs may push the 1-minute load over 2 and cause the same refusal again. The harness is not changed to avoid that; if it happens, G7 is NOT_RUN again and that is the result.

## Command (once)
`quietlock hold --owner cand3-campaign --minutes 20 -- ./cand3_g7b.sh phase2 ~/workspace/evidence-out/CAND3-LIVING-f816473-A7c/R15-receipt-B/90fdebb8bd2c8ff988c6d93afb764c3466bd6c3575cd8176fc605378fdb53fdd.json`
(inner: `tools/r16_qualify.sh` with R16_R15_RECEIPT set, R16_EXPECT_COMMIT = harness sha). Started only after 1-minute load is below 1.8 (waiting up to 10 minutes; otherwise NOT_RUN with the load trace and top CPU processes).

## Pass rules (fixed now)
- G7: the script's value. PASS only if every ladder rung R1-R15 is PASS (including R11 living, not "not exercised") and the R15 receipt is accepted (named by its own sha256, outcome PASS, commit equal, candidate_bound true, tree_dirty false, silicon_observed true). Any rung NOT_RUN keeps G7 NOT_RUN. A FAIL is a valid result and is kept.
- G8: NOT_RUN unless the spec still requires only what exists; it requires the PR merged with a merge commit. No merge commit exists and none is invented; the taskmaster merges.
- Runs once, never killed, no timeout wrapper, no retry. Evidence only in new directories; A7c, W1, W2, CAND-2 receipts and EST data are not edited.
