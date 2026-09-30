# EXP-001R preregistration

## Amendment (successor run of EXP-001; written before any EXP-001R data exist)

**What happened.** EXP-001 ended as a terminal FAIL of criterion S2 (freeze order), step "freeze order", code OVERLAP
(calibration/experiments/EXP-001/final_receipt.json). Its overlap audit
(`calibration/scripts/verify_holdout_separation.sh`, gate G6 crumb_digest) found exactly 1 crumb_digest shared between
the sealed data and the development data. Every other gate (G1 to G5, G7 sealed_digest, G8 trace_stream, G9
crumb_block) passed. No sealed data were scored. The EXP-001 files stay as published, byte-identical.

**Cause.** `crumb_digest` hashes only the visible part of a crumb (crumbs-v1 `session.rs`: `crumb_digest:
visible.digest()`). The shared crumb was population "Ambiguous" (families insufficient-evidence-n2, -n3 and
no-structure, which by design show too few observations to determine the hidden answer), and its `sealed_digest`
(the hidden held-out set) differed between the sealed and the development record. So the visible space of such
crumbs is small and chance matches are expected; a rerun with fresh seeds would fail the same way with roughly even
odds. The match was a visible-only coincidence, not shared held-out data.

**Amended G6 (EXP-001R only).** A sealed crumb_digest shared with any development or burned crumb_digest FAILS,
EXCEPT when the sealed record's population is Ambiguous AND its sealed_digest differs from the sealed_digest of every
development or burned record carrying that crumb_digest. Exempt matches are counted and listed (crumb_digest,
population, both sealed_digests) in overlap_audit.json (`g6_exempt_matches`); the G6 detail reads "N shared, M exempt
(Ambiguous, different hidden set)". G7 (sealed_digest), G8 (trace_stream) and G9 (crumb_block) are unchanged for all
populations. There is no numeric tolerance. The amendment was written before any EXP-001R sealed data exist and does
not depend on any EXP-001R outcome. EXP-001 keeps its original strict G6 (`EXP_ID=EXP-001`, the default).

**Burned data added.** The six EXP-001 sealed seeds (2 groups x 3) are burned: generate_sealed_data.sh and gate G3
refuse them (`calibration/scripts/burned_seeds_exp001.txt`). The EXP-001 sealed traces and ledgers
(`~/aien-data/turing-cal/sealed/d3cba292b9282116d1e374db22344bca4d47717e`) join the development and burned
comparison list, in addition to the whole of `~/aien-data/crumbline` (the list is not narrowed).

**Unchanged.** Same seven candidates with byte-identical frozen files and digests (no refit; the committed files in
`calibration/experiments/EXP-001/candidates/` and the two large files under `~/aien-data/turing-cal/candidates/` are
used as they are):

| candidate | bytes | file_sha256 | model_digest |
|---|---|---|---|
| B0_uniform | 29 | 0d8109c204e90aa33d2992cec3ca40b79c3957ac1f85623e45e2360570b21742 | 83c04bdfc10614febd634092355ef03997ed255fde5bdec9a80fe2d8964998ce |
| B1_order0 | 29 | 9ddcd6a4ff2dfab1a8d015a031bce87b52a4ffbc513974e764e9dcd4352e8227 | a0962e1d516a71eb07bdd385fea01af79a3cd587ae2d6ed0ac710e26996e356a |
| B2_order1 | 145 | 27d470845551fd7e9bed8c478a6cbd611abe90fdf13b541a5209ef4a3c06375d | 59ae9398da24e809d92ab39e7456382a0a4b7e93e203862be6d2ca55450dfd04 |
| B3_heuristic | 293 | 5bba575a752636b8b5eb088aa2f72d9e075f40ff58d61dfe4a4ca79ce84b313b | e729818b8289263e6819b21fc55f944f9d5113e7cfc4419d0badcc27f589f2ae |
| M_candidate | 22605 | 42ad299da082caf31bc982df9778b630e62075a4e2ff85450bee959b815c0eb4 | 64a57ba9cf6f8b6e4558751ef2145ae2aa729c5214bfa29373eabfa220a57bc7 |
| M_mem | 75979815 | 5a2ca3f457cc75b4f49fee99f054a0c32d15e3bfd442886c40e4a5f4945bab81 | cbd6944c6618f508e5377e4698b0d472042cdf3b4592cb80e5427767e232c140 |
| M_mem_seed1 | 44541937 | 34df83a19756d1bd7e73f2b218576cefedad40ac2fc0e7a7a3da10a2a7fd206f | 5dfeff7b643178807b547fbd62388319b9a407109e345614679e4bf20ccfd652 |

Same design: 2 groups x 3 fresh sealed control seeds, seeds derived by the same rule (turing.cal.sealed.v1) from the
new freeze commit and the new profile digest. Same criteria S1 to S9 and the same thresholds, same verdict rule. The
profile is `calibration/profiles/Turing-profile-v1.1.toml`: v1.0 with only the EXP-001R names and paths and the
overlap_audit text changed. The remainder of this file is the EXP-001 preregistration with EXP-001 renamed EXP-001R
and the profile renamed v1.1; no number, criterion or threshold in it changed. Where a section names
`calibration/experiments/EXP-001R/`, that is this run's directory (created at its freeze); the power simulation
output and the candidate files are the EXP-001 ones.


## 0. B3 fixed heuristic predictor (declared before any candidate is fit)

Declared 2026-09-29, before `tools/turing_cal_candidates.c` was written or run. No number in B3 comes from data.
Every number comes from the outcome alphabet in `src/turing/ty_ctr1.h` (symbols 0..6 are EXPAND outcomes, 7..8 are
SUBMIT outcomes, op feature 15 marks SUBMIT events) and the 16-bit quantization rule of the profile.

- Feature mask: `TY_F_OP` (= 1). Key = op feature (0..14 EXPAND, 15 SUBMIT). Key space 16, key bits 4.
- Rows: 16 rows, keys 0..15, all stored.
  - Keys 0..14 (EXPAND): symbols 0..6 get 9362 each, symbols 7 and 8 get the floor 1. Sum 7 x 9362 + 2 = 65536.
    Meaning: an EXPAND event is one of the seven EXPAND outcomes, each equally likely; a SUBMIT outcome is
    impossible except for the floor.
  - Key 15 (SUBMIT): symbols 0..6 get the floor 1, symbol 7 gets 32765, symbol 8 gets 32764. Sum 7 + 65529 = 65536.
    Meaning: a SUBMIT is accepted or rejected with equal odds (the odd unit goes to the lower symbol index, the same
    tie rule as `ty_quantize_kt`).
  - Default row (never reached, since every key has a row, but the TYM0 format requires one): the quantized uniform
    row, symbols 0..6 = 7282, symbols 7..8 = 7281 (1 + floor(65527 / 9) = 7281 each, 7 leftover units to the lowest
    indices). Sum 65536.
- L(M) = 104 + 16 x 8 + 16 x (4 + 16 x 8) = 2,344 bits.

## 1. Question

Does a model that describes the crumbline control traces (M_candidate, mask 93) compress fresh, never-seen control
traces better than the order-1 baseline B2 after paying for its own description, measured with real entropy
coders as well as ideal code lengths, while two memorizers that cannot generalize are correctly scored as losing?
EXP-001R is a calibration: it tests that the Turing-profile-v1.1 instrument gives the right sign to a known-good
model and to known-bad models before the instrument is used on anything new.

Profile: calibration/profiles/Turing-profile-v1.1.toml (digest in the .sha256 sidecar). Model code:
calibration/docs/MODEL_DESCRIPTION_ENCODING.md. Statistics: calibration/docs/UNCERTAINTY_PROTOCOL.md. Failures:
calibration/docs/FAILURE_REPORTING.md. Coders: calibration/docs/CODER_SPEC.md (lane B). Sealed data and blinding:
calibration/docs/BLINDING_PROTOCOL.md (lane C).

## 2. Candidate set (frozen before any sealed seed exists)

All fitted on dev seeds 1-7 of exp-20260927-rep10 control only (manifest sha256 6efc04b5...). Seeds 8-10 and
final-20260927 are burned and never used. Digests and L(M) are in candidate_manifest.json.

| name | what it is | mask | rows | L(M) bits | role |
|---|---|---|---|---|---|
| B0_uniform | quantized uniform over 9 symbols | 0 | 0 | 232 | floor reference |
| B1_order0 | symbol frequencies, MDL | 0 | 0 | 232 | weak baseline |
| B2_order1 | previous symbol, MDL (ty2_baseline.tym, unchanged bytes) | 4 | 7 | 1,156 | **the baseline of the verdict** |
| B3_heuristic | fixed rule above, no data | 1 | 16 | 2,344 | data-free baseline |
| M_candidate | op, prev1, prev2, prev3, prev4, MDL (ty2_candidate.tym, unchanged bytes) | 93 | 1,237 | 180,834 | **the candidate** |
| M_mem | position table, all 7 dev seeds, keep all rows | 32 | 3,706,331 | 607,838,516 | memorizer control |
| M_mem_seed1 | position table of dev seed 1 alone, each row sharpened to its most frequent symbol | 32 | 2,172,776 | 356,335,496 | exact memorizer control |

Why two memorizers: M_mem is the brief's memorizer, but positions are shared across seeds, so each row averages 7
different worlds and it does not memorize (dev in-sample 1.3097 bits/event, worse than B2). M_mem_seed1 is a true
memorizer of one world: 0.000176 bits/event on dev seed 1, 3.17 bits/event elsewhere. Both must lose.
Orchestrator decision 2026-09-29: both memorizers are kept. Their .tym files (76 MB and 45 MB) are not in git; they
are pinned by SHA-256 in candidate_manifest.json, rebuild deterministically, and a copy is kept at
~/aien-data/turing-cal/candidates/.

## 3. Dev results (in-sample, seeds 1-7, 14,376,405 events; not evidence, only for the predictions)

| name | L(M) bits | L(D|M) bits | DL bits | bits/event | seed 1 bits/event |
|---|---|---|---|---|---|
| B0_uniform | 232 | 45,571,493.300 | 45,571,725.300 | 3.1699 | 3.1699 |
| B1_order0 | 232 | 24,559,362.384 | 24,559,594.384 | 1.7083 | 1.7064 |
| B2_order1 | 1,156 | 18,379,920.294 | 18,381,076.294 | 1.2785 | 1.2753 |
| B3_heuristic | 2,344 | 40,358,094.761 | 40,360,438.761 | 2.8072 | 2.8073 |
| M_candidate | 180,834 | 11,632,692.544 | 11,813,526.544 | 0.8092 | 0.8066 |
| M_mem | 607,838,516 | 18,828,523.887 | 626,667,039.887 | 1.3097 | 1.3621 |
| M_mem_seed1 | 356,335,496 | 45,582,483.218 | 401,917,979.218 | 3.1706 | 0.0002 |

## 4. Predictions

- P1: DL order on each sealed group, ideal and both coders:
  M_candidate < B2 < B1 < B3 < B0 < M_mem_seed1 < M_mem.
- P2: M_mem does not beat B2 even on dev data (already observed: 1.3097 vs 1.2785 bits/event before L(M)).
- P3: M_mem_seed1 wins on its own seed (observed on dev seed 1) and loses on every sealed seed, by roughly L(M).
- P4: T(M_candidate) per event on each sealed group is near the dev validation gain of 0.4703 bits/event
  (seed 7 held out from a seeds 1-6 refit: B2 2,499,920.598 bits vs M_candidate 1,582,420.133 bits on seed 7).

Predictions P1-P4 are reported and are not part of the verdict: no prediction enters any criterion of section 5.

## 5. Success criteria (all must be PASS for PASS)

- S1 lossless: every coded stream decodes to the exact input symbols (both coders, every candidate, every seed).
- S2 freeze first: the freeze commit C_f (the commit holding candidate_manifest.json with status frozen, the
  filled profile and its sidecar) is an ancestor of origin/main, is committed before the sealed data release, the
  candidate manifest and profile used are the files at C_f, every sealed seed is the turing.cal.sealed.v1 seed for
  C_f, frozen_at is before the release, and overlap_audit.json names C_f with result PASS (BLINDING_PROTOCOL.md
  section 2).
- S3 same input: all candidates and coders score byte-identical symbol streams (one stream digest per sealed file,
  recorded before scoring and equal in every receipt).
- S4 envelope: for each coder, every whole sealed file and every crumb (coded as its own file) satisfies the
  two-sided band |overhead - 448| <= 64 + 1.0e-3 x N bits, integer form |overhead_ub - 448,000,000| <=
  64,000,000 + 1,000 x N (profile coder_envelope, CODER_SPEC.md section 9). Declared now: rANS codes below ideal on
  rows skewed toward low-index symbols, and the slope 1.0e-3 was measured on dev seeds 1-7, not proven.
- S5 no unexplained material reversal. Exact rule in FAILURE_REPORTING.md section 5: a DL ordering reversal
  between the ideal and a coder is explained if the ideal DL gap is <= 2 x the summed envelope band of the group; a
  T sign reversal ideal vs coded (intervals exclude each other's point estimates) is explained if |T_ideal - T_coded|
  <= 2 x that band; a material T sign reversal between group 1 and group 2 is never explained. Spearman rho reported only.
  No cause label was preregistered, so any unexplained reversal is FAIL whatever cause is found later.
- S6 candidate wins: in group 1, the 95% interval lower bound of T(M_candidate) against B2 is > 0, for the ideal
  code length and for both coders.
- S7 memorizers lose: in group 1, the 95% interval upper bound of T(M_mem) and of T(M_mem_seed1) against B2 is < 0,
  ideal and both coders.
- S8 independent scorer: the lane D scorer recomputes L(M), every probability (its TPS1 bytes), ideal code lengths,
  T and the interval bounds from the traces and models, reads the coded sizes as the byte sizes of the coded files
  after decoding and checking each one, reports "problems": 0, and every value equals the primary exactly
  (EVALUATOR.md section 6 states what S8 can and cannot catch; profile independent_verification_requirements).
- S9 replication: S6 and S7 also hold in group 2.

## 5a. Verdict rule and INCONCLUSIVE trigger (declared before any sealed data exist)

S1-S5 and S8 are PASS or FAIL. S6, S7 and S9 are PASS, FAIL or INCONCLUSIVE on the 95% intervals of T against B2
(ideal, coder A, coder B):

- S6: PASS if every lower bound is > 0; FAIL if any upper bound is < 0; otherwise INCONCLUSIVE.
- S7: PASS if every upper bound (both memorizers) is < 0; FAIL if any lower bound is > 0; otherwise INCONCLUSIVE.
- S9: the S6 and S7 rules on group 2, taking the worse of the two.
- A criterion that cannot be evaluated is NOT_REACHED and counts as FAIL.

EXP_001_COMPRESSION_BRIDGE = FAIL if any criterion is FAIL or NOT_REACHED; else INCONCLUSIVE if any is
INCONCLUSIVE; else PASS.

VOID and FAIL (FAILURE_REPORTING.md section 2 is the rule, EVALUATOR.md section 3 maps every code). VOID (no
verdict, void receipt kept and published) is only for an infrastructure failure before any sealed score exists;
all voids of EXP-001R (sealed generation, frozen-tree tests, evaluation) share one counter, and the third ends
EXP-001R as INCONCLUSIVE INFRA. Once scoring has started the verdict is final and a failure is a terminal FAIL of its criterion: a
coder or decoder failure is S1; a freeze-order violation (overlap FAIL, manifest or profile not the files at C_f,
a seed not derived from C_f, C_f or frozen_at not before the release) is S2; a stream, binding or serialization
failure is S3; a per-crumb sum mismatch is S4; a group without crumbs is S6 (group 1) or S9 (group 2); a lane D mismatch,
crash or missing output is S8, with no retry. A crash after
scoring started is FAIL of the criterion whose step crashed. A development dry run
is labelled DRY_RUN_NOT_EVIDENCE. INCONCLUSIVE is final for this sealed set: no seeds are added and nothing is
rerun to resolve it (FAILURE_REPORTING.md sections 2 and 4).

Evaluator (calibration/docs/EVALUATOR.md): `turing-cal-eval run` computes everything and writes the bundle with
a pending receipt; lane D writes scorer_independent.json; `turing-cal-eval gate` compares it with
scorer_primary.json (S8) and writes final_receipt.json once.

## 6. Design, minimum effect and sample size

- 2 sealed groups x 3 sealed control seeds, generated by lane C after the freeze, never seen by any fit.
- Declared minimum effect: f = 0.1 of the dev validation gain, 0.047035 bits/event. Reason: at this size a
  model with L(M) = 180,834 bits pays for itself after 1.96 seeds, so a 3-seed group can see it; smaller effects of a
  model this size are not worth its description on a group this size (breakeven 3.92 seeds at f = 0.05).
- Sample size: power_simulation.c (seed 0x4558503030315053, 500 worlds, 1000 resamples, seed 7 per-crumb gains
  from a seeds 1-6 refit) finds joint power (S6 and S7 together) at f = 0.1 of 0.000 at n = 1, 0.062 at n = 2,
  1.000 at n = 3 (shift model); 0.092 / 1.000 at n = 2 / 3 (scale model); 0.028 / 1.000 with the spread inflated
  x2.04 (assumption A3). Chosen n = 3 seeds per group, the smallest n with power >= 0.95. Memorizers: interval below
  0 in 100% of simulated worlds at every n.
- Error rates at n = 3 (S6 alone, shift model, same output file): false-positive rate 0.018 when the true T is 0
  (effect fraction f0 = 0.0653); at f = 0.1, FAIL rate 0.000, INCONCLUSIVE rate 0.000, so the false-negative rate
  is 0.000; mean 95% interval width 42,982 bits. The simulation source and its output are pinned by SHA-256 in
  candidate_manifest.json (shared_background_sha256).
- Honest limit: below f = 0.05 no n <= 10 has power, because the fixed L(M) charge dominates. EXP-001R cannot detect
  a small real gain of a model this size. An S6 interval wholly below 0 is a FAIL; an interval that contains 0 is
  INCONCLUSIVE (section 5a) and only says the gain, if any, is not resolvable at about 0.047 bits/event with 3 seeds.

## 7. Stopping rule

Fixed design, no optional stopping (profile stopping_rule). Exactly 6 sealed seeds, derived from C_f, generated
once, each scored once. No seed added, dropped or replaced because of its result. An infrastructure failure
before any score exists (a seed failing to generate, a failed build or test of the frozen tree, a refused evaluator
check) is a VOID and is retried with the same C_f, the same seeds, the same frozen tree and a byte-identical
command, nothing edited in between. The limit is three voids for all of EXP-001R, stages counted together; the
third void ends EXP-001R as INCONCLUSIVE INFRA, which is final. Sealed generation starts only after the PASS freeze
receipt naming C_f is on origin/main. Every failed attempt is published. After
scoring has started there is no retry (section 5a).

## 8. Failure retention

Every outcome is committed and kept (FAILURE_REPORTING.md). The verdict is written once, as
final_receipt.json in the bundle ~/aien-data/turing-cal/eval/<C_f>/run/bundle (outside git), and never replaced. A
later publication commit copies it and the other files listed in FAILURE_REPORTING.md section 6 byte for byte into
calibration/experiments/EXP-001R/, with published_manifest.sha256.

## 9. Also reported, not part of the verdict

T of every candidate against B0, B1, B3 (profile baseline_sensitivity_plan); T with L(M) byte-rounded and doubled (the L(M) of both the candidate and B2 doubled, so T moves by -(L(M) - L(B2))),
and the data-only gain (model_code_sensitivity_plan); the x2.04 inflated interval (UNCERTAINTY_PROTOCOL.md A3);
P1-P4.

## 10. Data lineage and blinding

LINEAGE NOTE (orchestrator decision 2026-09-29). The pinned generator (crumbs 72e396b1..., crumbline-learner
8159bdff...) reproduces final-20260927/rep10 byte-exactly but not the development data exp-20260927-rep10: 16 of the
2,172,776 seed-1 records differ in result_class, because the learner was rebuilt at 12:00, after rep10 ran at 11:47.
Candidates stay fit on exp-20260927-rep10 unchanged. Sealed data use the pinned build (control condition only,
`crumbs experiment --learner L --seed S --out DIR`, fresh output directories). This is declared, before any sealed
data exist, as a known generator difference between development and sealed data. Its direction is not known: the
baseline B2 was fit on the same older data as the candidates and T is a difference against B2, so the shift can
move T either way. Its size is negligible (16 of 2,172,776 records, under 1e-5 of the data), far below the
minimum effect of section 6, so it cannot decide a verdict. The earlier wording ("can only hurt the candidates")
was not supported and is withdrawn.

Blinding (BLINDING_PROTOCOL.md): the primary non-access is temporal, since sealed seeds are derived from the freeze
commit and cannot exist before it. The bubblewrap jails are the second layer. All jails run as the same user id; the
optional separate sealed user is not used, and this limit is stated in the profile.

## 11. Open at freeze

Only runtime_digest (calibration/scripts/runtime_digest.sh over the seven binaries, including turing-cal-eval and
the independent scorer, listed as turing-verify-indep, file build/turing-verify-indep/indep-scorer, built from the
freeze commit) and the profile sidecar. The runtime_sha256 block and runtime_digest already in candidate_manifest.json
are development values for the current build; freeze_candidate.sh --freeze regenerates them from the build of the
freeze commit, and the evaluator checks its own binary against the frozen value (RUNTIME_DIGEST). The operator sets
preregistration.json to status frozen in the same commit. Then check_profile.sh --freeze and freeze_candidate.sh
--freeze (which records frozen_at) must pass; that commit is C_f, and freeze_receipt.sh C_f records it
(TURING_PROFILE_V1_FROZEN). Lane D is the independent scorer in tools/turing_verify_indep (written from the
calibration documents only, source pinned by independent_scorer_source_sha256; EVALUATOR.md section 6). The CAL-0
fresh-reader reviews were done; calibration/docs/CAL0_REVIEW_RESOLUTION.md is a history record of their findings
and fixes. It is not needed to reconstruct the evaluation: the profile, this document and the calibration docs it
cites are the complete specification.
