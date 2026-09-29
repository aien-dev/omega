# Failure reporting (Turing-profile-v1.0, EXP-001)

## 1. Rule

Every outcome is kept. PASS, FAIL, a void run, a refused step and a failed adversarial test are all committed
with their receipt. Nothing is deleted, rerun to replace an earlier result, or reinterpreted with a changed
threshold, baseline, candidate or seed set. A FAIL is a valid scientific result and is reported with the same
detail as a PASS.

## 2. Outcome classes

| class | meaning | what is written |
|---|---|---|
| PASS | every success criterion S1-S9 of the prereg is PASS | final_receipt.json verdict PASS |
| FAIL | the run completed as specified and at least one criterion is FAIL or NOT_REACHED | final_receipt.json verdict FAIL, with each failed criterion and its numbers |
| INCONCLUSIVE | the run completed, no criterion is FAIL or NOT_REACHED, and at least one of S6, S7, S9 is INCONCLUSIVE by the trigger below | final_receipt.json verdict INCONCLUSIVE, with the straddling intervals |
| VOID | the run could not be completed as specified (a check refused, a digest mismatched, a seed failed to generate, the per-crumb sum did not equal the file sum, an overlap was found) | void_receipt_<n>.json naming the step, the refusal code and the time; no verdict |
| DEVIATION | anything done differently from the prereg or profile | listed in REPORT.md; makes the run FAIL unless the profile itself allows it |

INCONCLUSIVE trigger (declared 2026-09-29, before any sealed data exist; the evaluator implements exactly this).
S1, S2, S3, S4, S5 and S8 are two-valued: PASS or FAIL. S6, S7 and S9 are three-valued, judged on the 95%
percentile-bootstrap interval [lo, hi] of T against B2 (UNCERTAINTY_PROTOCOL.md), for the ideal code length and for
both coders:

- S6 (M_candidate, group 1): PASS if lo > 0 for all three measures; FAIL if hi < 0 for any measure; otherwise
  INCONCLUSIVE (some interval contains 0 and none lies wholly below it).
- S7 (M_mem and M_mem_seed1, group 1): PASS if hi < 0 for all six intervals; FAIL if lo > 0 for any of them;
  otherwise INCONCLUSIVE.
- S9 (group 2): the S6 and S7 rules applied to group 2; S9 is the worse of the two (FAIL < INCONCLUSIVE < PASS).
- A criterion that could not be evaluated (a required candidate missing) is NOT_REACHED and counts as FAIL.

Verdict: FAIL if any criterion is FAIL or NOT_REACHED; else INCONCLUSIVE if any criterion is INCONCLUSIVE; else
PASS. The receipt field EXP_001_COMPRESSION_BRIDGE carries the verdict. An INCONCLUSIVE verdict means the sealed
sample could not separate the effect from zero; it is not a PASS, it is published with the same detail, and it is
never converted into PASS or FAIL by more data, other seeds or a changed interval (section 4). A development dry
run carries EXP_001_COMPRESSION_BRIDGE = DRY_RUN_NOT_EVIDENCE whatever its criteria say, and S2 = NOT_CHECKED.

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
- Describing a FAIL as "inconclusive" or "partial PASS". If a criterion is FAIL, the verdict is FAIL; the report
  may explain why, but the verdict stays. INCONCLUSIVE is assigned only by the preregistered trigger in section 2,
  never by judgement.
- Turning an INCONCLUSIVE into PASS or FAIL by adding seeds, rerunning, or changing the interval level.

## 5. Reversals

Every reversal is listed in uncertainty.json. Let band(g) = sum over the files f of group g of
(64,000,000 + 1,000 x N_f) ub, the summed half-width of the coder envelope (N_f = events in file f).

- DL ordering reversal: two candidates a, b whose order by L(M) + L(D|.) differs between the ideal code length and
  a coder, within one group. Explained if |DL_ideal(a) - DL_ideal(b)| <= 2 x band(g) (the ideal gap is small
  enough that the two coders' envelopes can swap it); otherwise unexplained.
- T sign reversal, ideal vs coded: T_ideal and T_coded of one candidate (same group, baseline B2) have different
  signs AND each interval excludes the other's point estimate (material). Explained if
  |T_ideal - T_coded| <= 2 x band(g); otherwise unexplained.
- T sign reversal, group 1 vs group 2: the same measure of one candidate changes sign between the groups AND each
  interval excludes the other's point estimate. Always unexplained (the coder envelope cannot account for it).

S5 is FAIL if any reversal is unexplained, else PASS. The Spearman rank correlation of the DL ordering, ideal vs
each coder, is reported and not used in the verdict.

## 6. Where results go

calibration/experiments/EXP-001/: final_receipt.json (written once, exclusive create, by turing-cal-eval gate; see EVALUATOR.md) or void receipts, REPORT.md, overlap_audit.json,
candidate_manifest.json, the independent scorer's receipt. The sealed data themselves stay outside git in the
lane C location, and their bytes are never committed.
