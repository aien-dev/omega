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

## Not changed

- runtime_digest stays FILL_AT_FREEZE (seven binaries); it is filled only in C_f.
- No sealed data were generated and nothing was frozen in this round.
- A read or write failure after scoring started that belongs to no single criterion is recorded as verdict FAIL
  with failed_criterion NONE and every criterion NOT_REACHED (EVALUATOR.md s3, FAILURE_REPORTING.md s2), not as a
  FAIL of a named criterion.
- The independent scorer's `dry_run` field is derived from the dataset manifest split (`sealed_test` gives false).
