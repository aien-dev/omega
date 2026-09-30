# CAL-0 review resolution (EXP-001, Turing-profile-v1.0)

Inputs: the CAL-0 fresh-reader review (verdict NO: blocker Q1, major Q2-Q6, minor Q7-Q20) and the lane D
independent scorer's spec-gap list (G1-G13, `tools/turing_verify_indep/SPEC_GAPS.md`). Every item is either fixed
(the text or code now says one thing, and a test checks it where a test can) or accepted with a stated reason.
Nothing here is frozen yet; the freeze follows BLINDING_PROTOCOL.md section 2.

## Review items

| Id | Finding (short) | Resolution | Where |
|---|---|---|---|
| Q1 | The freeze commit could never pass the evaluator's commit check (a file cannot name its own commit) | Fixed. Two-step freeze: C_f holds the frozen manifest, profile value and sidecar and names no commit; seeds derive from C_f; C_f reaches main by a real merge or fast-forward; a later commit adds `freeze_receipt.json` (TURING_PROFILE_V1_FROZEN = PASS). The evaluator checks C_f against origin/main, the manifest and profile at C_f, the commit time and every seed. `git_head` removed from the manifest | BLINDING_PROTOCOL.md s2 steps 3-4; `freeze_receipt.sh`; `turing_cal_eval.c` step 4; EVALUATOR.md s2; profile test_generation_seed_commitment, holdout_commitment, creation_timestamp; tests in `test_tc_eval.sh` |
| Q2 | Which failures are FAIL and which VOID | Fixed. VOID only for infrastructure or operator failure before any sealed score exists; afterwards the verdict is final and a failure is a terminal FAIL of its criterion (S1 coder, S2 freeze order, S3 stream/binding, S4 crumb sum, S6 no crumbs, S8 lane D). Freeze-order violations are S2 FAIL (exit 1, `final_receipt.json` kind terminal_fail). Doc/code disagreement is never a void. Deviations are listed by the operator and checked by the reviewer; adversarial tests before freeze and on the frozen tree | FAILURE_REPORTING.md s2; EVALUATOR.md s1, s3 (code-to-outcome table); prereg s5a; MODEL_DESCRIPTION_ENCODING.md header; profile failure_reporting_policy, pass_fail_rule; `terminal_receipt.schema.json`; tests `expect_final` |
| Q3 | Three contradictory generation-failure and retry rules | Fixed. One rule: a retry uses the same C_f, same seeds, same frozen tree and a byte-identical command, nothing edited; at most 3 attempts; the third failure is INCONCLUSIVE INFRA (final); every failed attempt published. Enforced by `generate_sealed_data.sh` (`.failed-k`, `.INCONCLUSIVE_INFRA`) and the evaluator (RETRY_DIFFERS, third void writes inconclusive_infra) | profile stopping_rule; prereg s7; FAILURE_REPORTING.md s2; BLINDING_PROTOCOL.md s2; tests |
| Q4 | S4 scope: which candidates | Fixed. All seven candidates, per coder, per file and per crumb. Measured: 0 violations for every candidate and coder over 1,305 units each, closest unit about 32 bits inside | profile coder_envelope; CODER_SPEC.md s9 table; `envelope_summary.sh`; evaluator step 6 |
| Q5 | Lane D independent scorer undefined | Fixed. `tools/turing_verify_indep` (C, docs only, grep check for no omega identifiers); source pinned by `independent_scorer_source_sha256` (rule turing.cal.indep_source.v1); binary in runtime_digest (seven binaries); runs in the evaluator environment from the frozen tree; a source revision after release is S8 FAIL; evaluator refuses INDEP_SOURCE in sealed mode | EVALUATOR.md s6; profile independent_verification_requirements, runtime_digest_rule, scorer_digest; prereg s11; BLINDING_PROTOCOL.md s2 step 8; `indep_source_digest.sh`; `mk/turing_exp001_indep.mk` |
| Q6 | Ideal length not bit-exact | Fixed together with G2 (below) | profile ideal_codelength_method; CODER_SPEC.md s8; EVALUATOR.md s6.2 |
| Q7 | Lineage claim "can only hurt the candidates" unsupported | Fixed. Claim withdrawn; direction unknown (B2 fit on the same data), size negligible (16 of 2,172,776 records) | profile dataset_generator; prereg s10 |
| Q8 | Profile frozen together with built candidates, against CVP order | Accepted as deviation D10, with the reason (no sealed data exist before C_f) | PROTOCOL_CONFORMANCE.md row 1, D10 |
| Q9 | Row 18 cites the envelope as S2 | Fixed (S4) | PROTOCOL_CONFORMANCE.md row 18 |
| Q10 | D7 refers to tags the prereg never names | Fixed. No git tags; the freeze receipt and seed commitment are the committed records | PROTOCOL_CONFORMANCE.md row 36, D7 |
| Q11 | Prereg "except as listed in section 5" lists nothing | Fixed. No prediction enters any criterion | prereg s4 |
| Q12 | Evaluation command and binds not verbatim | Fixed. Step 8 gives the exact run, independent-scorer and gate commands in evaluator_env with run root R, docs copies and read-only candidates | BLINDING_PROTOCOL.md s2 steps 7-8; DATA_FORMAT.md s4 |
| Q13 | Pool order ambiguous | Fixed. File index j (seed-commitment order), then crumb ordinal | UNCERTAINTY_PROTOCOL.md s3; DATA_FORMAT.md s3 |
| Q14 | coded_bits of a group | Fixed. 8 x summed bytes of the three whole-file coded files (three headers); also used for coded DL in S5 | UNCERTAINTY_PROTOCOL.md; FAILURE_REPORTING.md s5 |
| Q15 | S5 pairs, ties and sign of zero | Fixed. All 21 pairs per group per coder; three-valued sign, 0 on one side only is a reversal; T checks cover the six non-B2 candidates. Reversal log enlarged so every reversal is published | FAILURE_REPORTING.md s5; `turing_cal_eval.c` (revlog) |
| Q16 | preregistration.json stays "draft" | Fixed. Set to "frozen" in C_f; `freeze_candidate.sh --freeze` and `freeze_receipt.sh` refuse otherwise; its hash is in the manifest shared background | BLINDING_PROTOCOL.md s2 step 3; prereg s11; `freeze_candidate.sh`; `freeze_receipt.sh` check 2 |
| Q17 | "L(M) doubled" for whom | Fixed. Both the candidate and B2 (as the evaluator computes: T moves by -(L(M) - L(B2))) | profile model_code_sensitivity_plan; prereg s9 |
| Q18 | CODER_SPEC s9 still a "Proposal" with a live alternative | Fixed. Heading says frozen with the profile; the alternative bound is removed | CODER_SPEC.md s9 |
| Q19 | Notebook fields open, no schema field | Already closed before this round (O4): receipt `notebook` with operator, host, kernel, commit, tree_clean; DIRTY_TREE refusal | `measurement_receipt.schema.json`; PROTOCOL_CONFORMANCE.md row 51 |
| Q20 | Burned-seed vocabulary differs | Fixed. One vocabulary: seeds 1-7 development, 8-10 burned, all refused as sealed seeds | BLINDING_PROTOCOL.md s2 step 2 |

## Independent-scorer spec gaps

| Id | Gap (short) | Resolution | Where |
|---|---|---|---|
| G1 | CTR1 byte layout not in the docs | Fixed. 247-byte record table, symbol mapping, refusals | DATA_FORMAT.md s1 |
| G2 | Ideal-length rounding contradicts itself at q = 43481, 46819 | Fixed. The written integer rule (32 truncating squarings on a Q62 mantissa, then (x * 1e6 + 2^31) >> 32) is the definition; ub(43481) = 591903, ub(46819) = 485194, one micro-bit above correct rounding; the "round(1e6 * -log2)" text deleted. Production code and the independent scorer agree (dev dry run S8 PASS; both self-tests check the two values) | profile ideal_codelength_method; CODER_SPEC.md s8; EVALUATOR.md s6.2; `tests/turing/test_tc.c`; indep `selftest.c` |
| G3 | Crumb boundary ("same crumb id" vs "first flag") | Fixed. A crumb starts at event_index 0 | DATA_FORMAT.md s1.2; EVALUATOR.md s6.2; CODER_SPEC.md s7 |
| G4 | B1, B2, M_candidate model files exist only as data | Accepted. The frozen model bytes are the object under test; they are hash-pinned (file SHA-256, model digest, L(M)) and decoded by the independent TYM0 reader | candidate_manifest.json |
| G5 | Bundle layout undocumented | Fixed. Run root, work paths, INDEX column formats | DATA_FORMAT.md s4 |
| G6 | Meaning of "profile digest" | Fixed. SHA-256 of the TOML bytes (the sidecar's 64-hex value), 32 raw bytes at TPS1 offset 28 | DATA_FORMAT.md s2; CODER_SPEC.md s2 |
| G7 | Output contract open in the old snapshot | Closed before this round by EVALUATOR.md s6 v2 | EVALUATOR.md s6 |
| G8 | Per-crumb coded sizes not reproducible independently | Accepted. They are published but outside the S8 key set; S4 is checked by the primary evaluator | DATA_FORMAT.md s4 |
| G9 | Event-index gaps: refuse or count | Fixed. Counted and reported, never refused; a non-increase inside a crumb is refused | DATA_FORMAT.md s1.2 |
| G10 | TPS1 crumb field saturation | Fixed. min(ordinal, 65535) | DATA_FORMAT.md s1.2 |
| G11 | TPS1 flags and reserved fields | Already specified: a reader refuses non-zero values (HEADER) | CODER_SPEC.md s2 refusal list |
| G12 | BLAKE3 chain not checked | Accepted. Integrity is pinned by the dataset manifest SHA-256 of every trace file; no BLAKE3 in the tree | DATA_FORMAT.md s1 |
| G13 | Small models cannot be rebuilt by an outsider | Accepted, same reason as G4: scoring is independent, model bytes are hash-pinned | candidate_manifest.json |

## Notes and follow-up fixes

- runtime_digest stays FILL_AT_FREEZE (seven binaries); it is filled only in C_f.
- No sealed data were generated and nothing was frozen in this round.
- A read or write failure after scoring started that belongs to no single criterion is recorded as verdict FAIL
  with failed_criterion NONE and every criterion NOT_REACHED (EVALUATOR.md s3, FAILURE_REPORTING.md s2), not as a
  FAIL of a named criterion.
- The independent scorer's `dry_run` field is derived from the dataset manifest split (`sealed_test` gives false).

## CAL-0 review 2 (Q1 to Q19)

| Id | Finding (short) | Resolution | Where |
|---|---|---|---|
| Q1 | Three-attempt limit: per step or whole experiment | Fixed. One counter for EXP-001: void receipts in `$TC_EVAL_ROOT/<C_f>/run/bundle`, shared by generation, frozen-tree tests (`record_void.sh`) and evaluation; the third void writes the INCONCLUSIVE (INFRA) final receipt; nothing starts after that | `scripts/tc_void_lib.sh`, `record_void.sh`, `generate_sealed_data.sh`, evaluator ATTEMPTS; FAILURE_REPORTING.md s2; profile stopping_rule |
| Q2 | Blanket "VOID" wording contradicts the refusal table | Fixed. EVALUATOR.md s3 is the one classification; VOID only before any sealed score; CRUMB_SUM is S4, BINDING S3, freeze order S2 | UNCERTAINTY_PROTOCOL.md; preregistration.json verdict and stopping_rule |
| Q3 | What lane D recomputes is undefined | Fixed. Lane D recomputes L(M), probabilities, its own TPS1 (compared byte for byte), TSY1, ideal lengths, crumb split, bootstrap and T; it checks every coded file (header, binding, own decode, INDEX sha and size) and reports `problems` | `tools/turing_verify_indep`; EVALUATOR.md s6 |
| Q4 | Pinned coder spec not in the bundle | Fixed. `coders.spec_sha256` is the SHA-256 of CODER_SPEC.md at this commit; CODER_SPEC.md is in the docs bundle | preregistration.json |
| Q5 | Void and terminal receipts carry no profile digest | Fixed. Every receipt carries profile_digest, freeze_commit, candidate_manifest_sha256 and (void) stage; a dataset freeze_commit that is not 40 hex refuses with no receipt | evaluator; schemas; tc_void_lib.sh |
| Q6 | Bundle location contradictory | Fixed. One location (sealed `--out` must end in `/<C_f>/run/bundle`, OUT_PATH); later publication copies a fixed file list with `published_manifest.sha256`; stream and coded bytes stay outside git | FAILURE_REPORTING.md s6; BLINDING_PROTOCOL.md steps 8, 9 |
| Q7 | Scorer binary name differs | Fixed. Listed as turing-verify-indep, file build/turing-verify-indep/indep-scorer | profile; EXP-001.md s11; EVALUATOR.md s6 |
| Q8 | Manifest runtime value vs FILL_AT_FREEZE | Stated. The manifest values are development values, regenerated at freeze | EXP-001.md s11 |
| Q9 | Step 7 builds five targets, runtime lists seven | Fixed. Step 7 builds all six make targets (seven binaries) with an empty PHYSICS_DIR; failures are voids via record_void.sh | BLINDING_PROTOCOL.md step 7 |
| Q10 | Generation does not check the freeze receipt | Fixed. generate_sealed_data.sh refuses without a PASS freeze receipt for C_f on origin/main | generate_sealed_data.sh; profile holdout_commitment; BLINDING_PROTOCOL.md step 5 |
| Q11 | Bad CTR1 mid-run: VOID or FAIL | Clarified. Every CTR1 file is validated before the first score, so it is a FORMAT void | DATA_FORMAT.md s1; EVALUATOR.md s3 |
| Q12 | Lane D crash or no output | Fixed. INDEP_MISSING: S8 terminal FAIL, no retry; problems not 0 also fails S8 | gate; EVALUATOR.md s3, s6.3 |
| Q13 | NO_CRUMBS maps to S6 for group 2 | Fixed. S9 for group 2 | evaluator; EVALUATOR.md s3 |
| Q14 | Schema failed_criterion list incomplete | Fixed. Lists S1, S2, S3, S4, S6, S8, S9 and NONE | `schemas/terminal_receipt.schema.json` |
| Q15 | Crumb ordinal saturation | Accepted and stated: cannot occur at this design (about 185 crumbs per file) | DATA_FORMAT.md G10 |
| Q16 | A3 x2.04 interval rounding | Fixed. Exact integer rule per side; report-only | UNCERTAINTY_PROTOCOL.md s8 |
| Q17 | CODER_SPEC s9 "proposal"; PC row 9 S4 | Fixed. s9 is the measured envelope frozen as coder_envelope; row 9 says S3 | CODER_SPEC.md; PROTOCOL_CONFORMANCE.md |
| Q18 | Resolution file not in the reviewed bundle | Stated. This file is a history record, not part of the reviewed bundle; nothing normative depends on it | EXP-001.md s11 |
| Q19 | Per-crumb coding stream not spelled out; q = 65536 | Fixed. Per-crumb coding slices the checked file TPS1 rows, writes no file, counts +56 header bytes; q = 65536 cannot occur for a coded symbol. D4 unchanged | CODER_SPEC.md s8, s9 |

## CAL-0 review 3 (R1 to R3, all MINOR, no blocker)

| # | Finding | Resolution | Where |
|---|---|---|---|
| R1 | `model_cost_method` named `ty_model_decode` and contradicted itself on padding | Fixed. It names `ty_model_encode` and says once that the zero padding of the `.tym` file (at most 7 bits) is not part of L(M) and is not in any T total; the scorer takes L(M) from the exact bit count. Numbers unchanged | `profiles/Turing-profile-v1.0.toml` model_cost_method |
| R2 | Scope of N in the S4 envelope not explicit | Fixed. A table in CODER_SPEC.md gives what N counts for each scope (per coder, per file, per crumb), taken from the evaluator code | `CODER_SPEC.md` s9 |
| R3 | Scorer reproducibility from the docs alone untested | Covered. Lane D is an independent scorer built from the docs only (`tools/turing_verify_indep/`); on the fixed dry-run bundle it matches the primary scorer exactly, 228 of 228 values and the 4 compared header fields. The dry-run bundle is the known-answer check | `tools/turing_verify_indep/SPEC_GAPS.md`; `make turing-exp001-eval-dry` |

The profile and CODER_SPEC digests changed with these fixes; the profile sidecar, candidate manifest and preregistration were re-pinned in the same commit.
