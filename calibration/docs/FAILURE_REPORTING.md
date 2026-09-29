# Failure reporting (Turing-profile-v1.0, EXP-001)

## 1. Rule

Every outcome is kept. PASS, FAIL, a void run, a refused step and a failed adversarial test are all committed
with their receipt. Nothing is deleted, rerun to replace an earlier result, or reinterpreted with a changed
threshold, baseline, candidate or seed set. A FAIL is a valid scientific result and is reported with the same
detail as a PASS.

## 2. Outcome classes

| class | meaning | what is written |
|---|---|---|
| PASS | all success criteria S1-S9 of the prereg hold | final_receipt.json verdict PASS |
| FAIL | the run completed as specified and at least one criterion does not hold | final_receipt.json verdict FAIL, with each failed criterion and its numbers |
| VOID | the run could not be completed as specified (a check refused, a digest mismatched, a seed failed to generate, the per-crumb sum did not equal the file sum, an overlap was found) | void_receipt_<n>.json naming the step, the refusal code and the time; no verdict |
| DEVIATION | anything done differently from the prereg or profile | listed in REPORT.md; makes the run FAIL unless the profile itself allows it |

A VOID run is not a FAIL of the hypothesis and not a PASS. Its sealed data are burned: they are never reused as
sealed data. A new attempt needs a new freeze commit, new sealed seeds under the same rule, and it cites every
earlier VOID receipt.

## 3. What every receipt contains

Receipts follow calibration/schemas/measurement_receipt.schema.json: experiment id, profile SHA-256 and sidecar,
freeze commit, candidate manifest SHA-256, sealed manifest SHA-256 (never the sealed data), runtime binary
digests, for each group and candidate L(M), ideal and coded code lengths, T with interval, the criteria table,
and every refusal or deviation with its reason. Numbers are integers in ub or bits; ratios are derived, never
used in a decision.

## 4. Forbidden actions

- Rerunning with a different bootstrap seed, a different number of resamples, or a different seed selection after
  seeing a result.
- Dropping a crumb, seed, group or candidate after seeing a result.
- Changing a candidate, its L(M) accounting, a coder or the baseline after the freeze commit.
- Reporting only the ideal or only one coder when the others disagree.
- Describing a FAIL as "inconclusive" or "partial PASS". If a criterion fails, the verdict is FAIL; the report
  may explain why, but the verdict stays.

## 5. Reversals

A material reversal is a sign change between the ideal and a coded result, or between group 1 and group 2, where
the two intervals exclude each other's point estimates. Every reversal is listed. A reversal explained by the
coder envelope (the coded shift o(M) - o(B2) is inside the lane B envelope and accounts for the change) is
reported as explained. Any other reversal fails criterion S5.

## 6. Where results go

calibration/experiments/EXP-001/: final_receipt.json or void receipts, REPORT.md, overlap_audit.json,
candidate_manifest.json, the independent scorer's receipt. The sealed data themselves stay outside git in the
lane C location, and their bytes are never committed.
