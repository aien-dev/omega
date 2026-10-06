# CAND-3: declared attempt A7c (R15 silicon) and the R16 G7/G8 attempt, written 2026-10-06T11:37:09Z, before either runs

`DECLARED-ATTEMPTS.md` (sha256 9f4b8b91713226038143ec0ec17425d6dba51845aceb75207dcabaf5bcbdf184) row A7 says R15 is "a separate, later, separately declared attempt
(its own DECLARED-ATTEMPT-A7c.md, written before it runs)". This is that file. A7 stays NOT_RUN in windows W1/W2 (energy reader not loaded); those
results are not edited. R16 G7 and G8 stay NOT_RUN in W2 (`evidence-out/CAND3-LIVING-f816473-w2`); this file declares their new, separate attempt.

## Candidate and harness (unchanged)
Omega code f816473df391bc4fbcf99df722edd70ed26ebef1; harness checkout `~/workspace/cand3-campaign/cand3/wt-omega-chip` at 97ee27584cda0d8eaca136f31a44293cd36c9b02
(map-only descendant, guard checks code identity); physics 6d7cf0d4d8eb2cda7b512100ff6058e25dbb3ddf; aienos bbad5e4250e57f8cbd1be4cf1390109aa65ef92c
(`cand3/survey/aienos`, passed as AIENOS_R7_DIR). Script: `cand3_r15c.sh` (copied into the evidence directory with its sha256). Evidence:
`~/workspace/evidence-out/CAND3-LIVING-f816473-A7c/`.

## Deviation from R15 spec clarification C2 (stated openly)
C2 says Secure Boot and integrity lockdown stay on. **Secure Boot is OFF on this boot, by Drake's explicit decision** (checkpoint cc-0021). The signed
SPBM reader loaded with the kernel notice "module verification failed: signature and/or required key missing - tainting kernel" and then "firmware contract
verified; read-only SPBM telemetry ready" (both lines are recorded from the kernel log in `00-reader/reader-facts.txt`). The kernel is tainted for this run.
This run therefore cannot claim C2 conformance. Whether R15 can be PASS with this deviation is decided by the receipt writer's rules below, never by me; the deviation
is a stated limit in every receipt notes file and in the gates page.

## Instrument facts (observed 2026-10-06 ~11:35Z, before this file)
Reader `aien_spbm_readonly` loaded at /sys/class/hwmon/hwmon3 (name aien_spbm); module sha256
2d37f51a90a74a7af2aae5e0f7171df357cb194d11cef614d37d6b43aa0dd8b0, srcversion D7345BB5C0CCFCB7B177335, vermagic 7.0.0-1019-nvidia; machine restarted 11:32Z (counter
reset by the restart is UNVERIFIED as a general rule, the preflight checks it). `tools/r15_machine_state.sh energy-preflight` from this checkout: "energy preflight ok:
/sys/class/hwmon/hwmon3 pkg 5182975000 -> 5188268000 uJ over 3 samples, 5 energy + 5 power labels verified". The reader is not unloaded, the machine is not rebooted.

## Machine conditions seen before the run (recorded, not corrected)
Boot-time services of other users are running and are NOT stopped by this attempt (stopping them is outside this task): atlas `dedicated-image_server.py` (pid 3632,
about 18 GB GPU memory, GPU utilisation 0 % when sampled) and a caption server; `atlas-max-coder` and `atlas-max-qwen` in auto-restart state; a user MAX serve of Llama-3.2-1B.
R15 section 9 asks for a machine with nothing heavy; the qualification script itself only warns. The run starts when the 1-minute load is below 1.5 and no qemu runs (as W2). Per-trial
and before/after snapshots (processes, GPU processes, Xid count) are in the evidence. If these conditions affect the result, that is reported, the run is not rerun.

## Attempts

| # | Attempt | Command (once) | Pass rule (fixed now) |
|---|---|---|---|
| A7c | R15 silicon on CAND-3 | `cand3_r15c.sh phase1`: energy preflight, `make r15-perf-silicon`, the 14 section-14 correctness reruns (test-r3 r7 r8 r9 r10 r11 r12 r12-silicon r13-host r13-silicon r14-host r14-silicon r15-parity-host r15-parity-silicon, sequential, none retried) with `OMEGA_CANDIDATE_COMMIT` = harness sha, then `tools/r15_qualify.sh silicon` (default 12 rounds, 5 + 5 runs; raw output outside the repo via R15_OUT_BASE), then `tools/r15_receipt.sh` | R15 PASS only if the receipt writer says outcome PASS: all 16 gates G1-G16 PASS, raw digests verify, candidate-bound (run commit == 97ee275, clean tree), silicon observed, aienos and physics equal their locks, all 13 required reruns PASS. SEQ-06 residency 99 % (G15) not lowered, no trial discarded, no best-of-runs, no gate or threshold changed, no rerun of a failed process (section 10). A failed gate is FAIL and is kept. A BLOCKED_INSTRUMENT refusal is recorded as such, never as performance. |
| G7 | R16 G7 (R1-R15 ladder plus R15 acceptance) on CAND-3 | `cand3_r15c.sh phase2 <R15 receipt>`: `tools/r16_qualify.sh` once with R16_R15_RECEIPT set, R16_EXPECT_COMMIT = harness sha. Run only if phase 1 produced a receipt with outcome PASS (otherwise G7 stays NOT_RUN: no valid R15 receipt exists, and the script's own rule makes it FAIL on a non-PASS receipt, so it is not run) | G7 is the script's value: PASS only if every ladder rung PASS and the R15 receipt is named by its own sha256, outcome PASS, candidate commit equal, candidate_bound true, tree_dirty false, silicon_observed true. A skipped or not-exercised rung is NOT_RUN. |
| G8 | R16 G8 (receipt and merge) | not run by any script | NOT_RUN. The spec's G8 requires the PR merged with a merge commit; no merge exists and none is made by this attempt (the taskmaster merges). `r16_qualify.sh` reports only whether the receipt preconditions hold. I will not mark G8 PASS from a script. |

The final R15 receipt (receipt-B) may add the human-written regression and limit notes (spec section 11) from summary.json values copied by hand, from the same raw directory; receipt-A (no notes)
and receipt-B are both kept. The outcome rule above does not depend on the notes. Phase 2 uses receipt-B.

## Rules
Runs once. Never killed, no timeout wrapper, no retry loop. Run under `quietlock hold` (owner cand3-campaign) with Drake's approval token (max 120 minutes). A failed run is a valid result
and is recorded as it is. Evidence is added only in new directories; CAND-2, W1, W2 receipts and EST data are not edited.
