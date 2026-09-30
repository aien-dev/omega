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
| VOID | an infrastructure or operator failure found before any sealed score exists: a check refused (digest, runtime, split, dirty tree, missing overlap audit, C_f not on origin/main), a sealed seed failed to generate, a crash before scoring | void_receipt_<n>.json naming the step, the refusal code, the exact command, the input digests and the time; no verdict |
| INCONCLUSIVE INFRA | the third VOID attempt of the same step | final_receipt.json kind inconclusive_infra, verdict INCONCLUSIVE, reason INFRA; final |
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

VOID versus FAIL (the one rule; EVALUATOR.md section 3 maps every refusal code to it):

- VOID is only for an infrastructure or operator failure found before any sealed score exists. "A score exists"
  from the moment `run` finishes checking the candidates and starts writing the first sealed probability stream.
- After that moment the verdict is final. A failure then is a terminal FAIL of the criterion whose step failed:
  a coder or decoder failure is S1; a freeze-order violation (overlap audit FAIL, manifest or profile differing
  from C_f, a seed not derived from C_f, C_f committed or candidates frozen at or after the release) is S2 even
  though the evaluator checks it before scoring, because retrying cannot change it; a stream, binding or
  serialization failure is S3; a per-crumb sum mismatch is S4; a group with no crumbs is S6; a lane D mismatch is
  S8; a crash after scoring started is FAIL of the criterion whose step crashed; a read or write failure after
  scoring started that belongs to no single criterion is verdict FAIL with every criterion NOT_REACHED. It is written as
  final_receipt.json kind terminal_fail.
- Every criterion that can FAIL is able to FAIL: each has at least one path above or in the criteria rules that
  sets it to FAIL, and the tests exercise each terminal path (EVALUATOR.md section 7).
- A retry after a VOID uses the same freeze commit C_f, the same sealed seeds, the same frozen tree and a
  byte-identical command and inputs (the evaluator refuses a different one with RETRY_DIFFERS; sealed generation
  refuses a different command). Nothing may be edited between attempts. At most three attempts are made; the
  third VOID ends EXP-001 as INCONCLUSIVE INFRA, which is final. Any later attempt is a new experiment with its
  own preregistration and its own seeds, and it cites every EXP-001 receipt.
- Sealed data generation follows the same rule: a failed seed moves the set to `sealed/<C_f>.failed-<k>` with a
  FAILED note (reason, command, attempt, time), the same command is run again (same seeds, same data), and the
  third failure writes `sealed/<C_f>.INCONCLUSIVE_INFRA` (BLINDING_PROTOCOL.md section 2).
- Every VOID receipt and every failed generation note is published with the final result.
- Deviations: the operator lists every action not written in the prereg, the profile or BLINDING_PROTOCOL.md
  section 2 in REPORT.md; the reviewer checks the list against the evaluation log. An unlisted deviation found
  later makes the run FAIL.
- Adversarial and unit tests run before the freeze (any failure is fixed before C_f; no sealed data exist) and
  again on the frozen tree at step 7. A failure at step 7 is VOID with the same retry rule; the frozen code cannot
  be changed, so a real defect ends in INCONCLUSIVE INFRA.

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

S5 is FAIL if any reversal is unexplained, else PASS. The only preregistered explanation is the coder-band rule above; no cause label
(CVP 1006-1025: decision boundary, sample size, baseline, model code, quantization, statistical instability,
implementation defect, fundamental metric sensitivity) was preregistered for EXP-001, so an unexplained reversal is
FAIL whatever cause is later found. A cause label written after the result is a diagnosis in the report
and never changes S5 or the verdict. The Spearman rank correlation of the DL ordering, ideal vs
each coder, is reported and not used in the verdict.

Scope and ties (the evaluator implements exactly this). The DL ordering check covers all 21 unordered pairs of
the seven candidates, in each group, for each coder (range and rANS) against the ideal. DL under the ideal is
L(M) x 1e6 + ideal_ub; DL under a coder is (L(M) + coded_bits) x 1e6, where coded_bits is 8 x the summed byte
size of the group's whole-file coded files (one 56-byte header per file). The sign of a difference is -1, 0 or
+1; a pair whose sign differs between the ideal and the coder (including 0 on one side only) is a reversal. The
T sign checks cover the six candidates other than B2, with the same three-valued sign; a T of exactly 0 has sign
0. Every reversal found is written to uncertainty.json.

## 6. Where results go

calibration/experiments/EXP-001/: final_receipt.json (written once, exclusive create, by turing-cal-eval gate; see EVALUATOR.md) or void receipts, REPORT.md, overlap_audit.json,
candidate_manifest.json, the independent scorer's receipt. The sealed data themselves stay outside git in the
lane C location, and their bytes are never committed.
