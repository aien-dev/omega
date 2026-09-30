# Protocol conformance: CAL-0 and EXP-001

Governing documents (on main, `docs/turing/protocols/`):
- **CVP** = `turing-instrument-calibration-validation-protocol-v1-0.tex`
- **LEP** = `turing-laboratory-execution-protocol-v1-0.tex`

Only CAL-0 and EXP-001 are in scope. Line numbers are those of the .tex files at 737f8b3. Status is one of
**met**, **fixed now** (closed on branch feat/turing-exp001 in this pass) or **deviation Dn** (reason in the
deviation table, which the receipt cites as D1-D9). Items that are required but not done yet are **open**;
they block the freeze or the verdict and are listed at the end.

House rules that force deviations: C and shell only (no Python, no Rust), the omega repository layout, and
Drake's brief, which is more specific than the protocols in places.

## Requirements

| # | Requirement (doc + line) | Our artifact | Status |
|---|---|---|---|
| 1 | Freeze order: protocol, families, power sim, n fixed, profile frozen, candidates hashed, sealed data, evaluation (CVP 123-149) | `preregistration/EXP-001.md` sections 2, 6, 11; `freeze_candidate.sh`; `generate_sealed_data.sh` (needs C_f on origin/main); `freeze_receipt.sh`; evaluator checks NOT_FROZEN, FREEZE_COMMIT, FREEZE_AFTER_RELEASE, SEED_DERIVATION | met with deviation D10 |
| 2 | Profile field list, identity through failure policy (CVP 155-277) | `profiles/Turing-profile-v1.0.toml`; `check_profile.sh` section 5c requires every field name in CVP 155-277 | fixed now (56 missing names added) |
| 3 | Profile digest is an input to every receipt (brief; CVP 1083) | sidecar `Turing-profile-v1.0.sha256`; `profile.digest`; receipt `profile.sha256`; checked against sidecar, candidate manifest and dataset manifest | met |
| 4 | Sample size from a published simulation, not invented (CVP 279-291) | `scripts/power_simulation.c`, `experiments/EXP-001/power_simulation_output.txt`; n = 3 seeds per group | met |
| 5 | Power output: assumed effects, n, interval width, false-positive and false-negative rates, seed, code digest (LEP 488-496) | same files; new table of false-positive rate at T = 0, FAIL and INCONCLUSIVE rates and mean interval width by n; source and output pinned in candidate manifest; the wall-clock runtime line goes to stderr, so the pinned output is byte-reproducible | fixed now (earlier lines unchanged) |
| 6 | Four principals: candidate, evaluator, verifier, replicator; candidate cannot read sealed data (LEP 163-188; CVP 293-344) | `docs/BLINDING_PROTOCOL.md`; bwrap candidate and evaluator jails; `verify_holdout_separation.sh`; `test-turing-cal-blinding` | met, with D8 |
| 7 | EXP-001 chain probability -> ideal -> coder -> bits -> T (CVP 346-381) | `tools/turing_cal_eval.c` run step 5; `docs/EVALUATOR.md` | met |
| 8 | Candidate set B0, B1, B2, B3, M_candidate, M_mem (CVP 346-381; brief) | candidate manifest: 7 entries incl. second memorizer M_mem_seed1 | met |
| 9 | Canonical immutable probability stream, hashed, both coders read the same stream and never call the model (CVP 383-394) | TPS1 (`docs/CODER_SPEC.md`); evaluator re-reads the written stream and encodes both coders from the one parsed copy; S4 binding check | met |
| 10 | Stream record fields: observation_index, context_digest, alphabet, vector, quantized vector, normalization receipt (LEP 322-358) | TPS1 per-step record; context_digest = 8-byte context key; raw vector = the quantized TYM0 table entry (the model has no other) | met (mapping in CODER_SPEC.md) |
| 11 | Two coders, arithmetic/range and ANS; lossless round trip (CVP 396-410) | `src/turing/tc_range.c` (TCR1), `tc_rans.c` (TCA1); S1 per file and per crumb | met |
| 12 | Coder termination, header and metadata accounted (CVP 396-410; profile fields) | 56-byte header counted in every coded length; envelope centred on 448 bits | met |
| 13 | Measurements L(M), ideal, coded A and B, overhead, T_ideal, T_A, T_B (CVP 412-423) | `scorer_primary.json`, `ideal_lengths.json`, `arithmetic/`, `ans/results.json`, REPORT table | met |
| 14 | Adversarial controls fail closed: corrupt stream, truncated bits, wrong model / profile / dataset digest, bad normalization, zero probability, overlap, metadata or header omission, memorizer hiding data (CVP 426-443) | coder tests (`test-turing-exp001-coders`, 1026 + 60 cases) and evaluator tests (`test-turing-exp001-eval`, refusal codes in EVALUATOR.md section 3) | met |
| 15 | Acceptance criteria S1-S9 (CVP 445-477) | prereg section 5, `preregistration.json` criteria | met |
| 16 | Gate value set (CVP 445-477 says PASS or FAIL; LEP 586-599 adds INCONCLUSIVE) | prereg section 5a; `FAILURE_REPORTING.md` section 2; schema enum; evaluator `gate` | fixed now, D9 |
| 17 | INCONCLUSIVE needs an exact preregistered trigger (LEP 597-599; brief) | S6/S7/S9 interval contains 0 on the relevant side, no criterion FAIL or NOT_REACHED; `preregistration.json` verdict_rule; profile `verdict_inconclusive_trigger` | fixed now |
| 18 | Actual vs ideal within preregistered overhead (CVP 445-477) | envelope abs(overhead - 448 bits) <= 64 bits + 0.001 bit per event, per file and per crumb, all seven candidates (S4) | met |
| 19 | No unexplained material ordering reversal (CVP 445-477) | S5 exact rule (band, 2 x band, group-1 vs group-2 T sign never explained); Spearman rho reported only | fixed now (rule made exact) |
| 20 | Rank reversals classified by cause (CVP 1006-1025) | S5 classifies explained (inside the preregistered coder band) vs unexplained. No cause label was preregistered, so any unexplained reversal is FAIL whatever cause is found later; a cause label written afterwards is a diagnosis in REPORT and never changes S5 or the verdict (prereg S5, `preregistration.json`, FAILURE_REPORTING.md section 5) | fixed now |
| 21 | Memorizer gets no spurious positive T (CVP 445-477; LEP 654-660) | S7, both memorizers, L(M) charges every stored count | met |
| 22 | Independent scorer, separate package, no production code (CVP 445-477; LEP 584) | S8 compares `scorer_independent.json` key by key, integers only, zero tolerance, same key set, no repeated key, same schema and input digests; field contract in EVALUATOR.md section 6; NOT_INDEPENDENT refusal; lane D is `tools/turing_verify_indep` (C, written from the docs only, source pinned by independent_scorer_source_sha256, binary in runtime_digest) | fixed now |
| 23 | New-seed replication preserves the conclusion (CVP 445-477; LEP 1615-1629) | group 2 (3 further sealed seeds), S9 | met |
| 24 | Sensitivity: vary baseline, model code, precision, quantization, coder, length, holdout, seed, uncertainty method (CVP 989-1004) | `uncertainty.json`: T vs B0, B1, B3; L(M) byte-rounded and doubled; data-only gain; x2.04 interval; two coders; two seed groups | met for EXP-001 scope; full sweep belongs to gate TURING_PROFILE_SENSITIVITY (not in scope) |
| 25 | Experiment bundle layout (CVP 1057-1081; brief) | `experiments/EXP-001/` as in EVALUATOR.md section 5 | met, with D2 and D5 |
| 26 | Receipt fields: ids, digests, times, T, uncertainty, result, gate digest, artifact root (CVP 1083-1122) | `schemas/measurement_receipt.schema.json`; run_id, started/scored, 5 roots, runtime sha, prereg sha | fixed now |
| 27 | Receipt energy and energy_uncertainty (CVP 1083-1122) | profile `energy_instrumentation`: not measured in EXP-001 (code lengths are hardware independent); no T/J claim until EXP-001 PASS (brief) | met (declared not applicable) |
| 28 | Receipt verifier_signature (CVP 1083-1122; LEP 409-430) | `signed_by` field: unsigned, git commit is the record | deviation D6 |
| 29 | Statistics: trajectory-level resampling, no within-trajectory independence (CVP 1125-1153; LEP 603-631) | per-crumb bootstrap; crumb context resets, so crumbs are independent coding units | deviation D4 |
| 30 | Report P(delta T > 0) (LEP 603-631) | `bootstrap_replicates_T_ideal_vs_B2_gt0` in `uncertainty.json` | fixed now |
| 31 | Multiple comparisons defined before evaluation (CVP 1125-1153) | one primary contrast per criterion (M_candidate vs B2; each memorizer vs B2), all must hold; others reported only | met |
| 32 | Failed runs excluded only by preregistered technical criteria; no optional stopping; seeds committed first (CVP 1125-1153) | `FAILURE_REPORTING.md`; prereg section 7; seed commitment from freeze commit in `generate_sealed_data.sh` | met |
| 33 | Failure policy: FAIL receipt, raw evidence, diagnosis, next version is a new experiment (CVP 1155-1173) | `FAILURE_REPORTING.md` sections 2-6; void receipts never overwritten; gate writes once | met |
| 34 | Repository layout `turing-lab/` with Cargo crates (LEP 56-161) | `calibration/` in omega; C sources under `src/turing/tc_*`, `tools/turing_cal_*`, `calibration/scripts/` | deviation D1, D2 |
| 35 | Helper scripts in Python (LEP 121-126) | shell and C only | deviation D1 |
| 36 | Git tags for freeze and release (LEP 190-208) | no tags: `freeze_receipt.json` records C_f, `seed_commitment.json` records the release time | deviation D7 |
| 37 | Digest rule: canonical JSON, media_type, created_by (LEP 212-231) | SHA-256 over exact file bytes; one schema id per JSON file | deviation D3 |
| 38 | Profile schema (LEP 233-276) | TOML + sidecar + `check_profile.sh` | met |
| 39 | Candidate manifest fields (LEP 278-299) | `candidate_manifest.json`: name, file sha, model digest, K, key bits, rows, lm_bits, location, shared background, profile sha, status, frozen_at | met |
| 40 | Dataset manifest fields (LEP 301-320) | `scripts/make_dataset_manifest.sh`, schema `turing.cal.dataset_manifest.v1` | met |
| 41 | Coder result record (LEP 360-380) | `arithmetic/results.json`, `ans/results.json` with source_dataset_digest, stream digest, bytes, round trip | fixed now (source_dataset_digest) |
| 42 | Score record (LEP 382-406) | `scorer_primary.json` values (EVALUATOR.md section 6) | met |
| 43 | CLI `turing ...` (LEP 435-599) | `turing-cal-eval run / gate`, `turing-coder`, make targets | deviation D1 |
| 44 | Refuse a freeze after data release (LEP 500-513) | FREEZE_AFTER_RELEASE refusal and test | met |
| 45 | EXP-001 statistics and memorizer rule (LEP 633-660) | prereg sections 5, 5a, 6 | met |
| 46 | Phase 001-A: engineering qualification incl. independent-scorer parity on fixtures (LEP 808-894) | coder and evaluator tests pass; independent-scorer parity on the development dry run (S8 PASS, `make turing-exp001-eval-dry`) | fixed now |
| 47 | Report sections, preregistration through gate decision (LEP 896-935) | `REPORT.pending.md` "Report sections" block | fixed now |
| 48 | Clean qualification build (LEP 1381-1407) | clean `make` of the seven runtime binaries; `runtime_digest.sh` | met, with D1 |
| 49 | EXP-001 command sequence (LEP 1410-1455) | EVALUATOR.md sections 1-6, prereg section 11 | met, with D1 |
| 50 | CAL-0 checklist incl. blinding verified and digest frozen (LEP 1599-1612; brief CAL-0 gate) | all drafted; fresh-reader review done and resolved (`CAL0_REVIEW_RESOLUTION.md`); profile still has 1 FILL_AT_FREEZE (runtime digest) | open (O3) |
| 51 | Notebook record per run: operator, host, dirty tree (LEP 1701-1721) | receipt `notebook`: operator (git user.name), host, kernel (uname), commit, tree_clean (git status --porcelain); outside a dry run a dirty or non-git tree is refused (DIRTY_TREE); schema `measurement_receipt.schema.json` | fixed now |
| 52 | Deviation record fields (LEP 1723-1736) | deviation table below | met |
| 53 | Publication package (LEP 1740-1758) | bundle + large files outside git with SHA-256 in INDEX files | met, with D5 |

## Deviations

Each record: what differs, when found, whether any outcome was visible (none: no sealed data exists), impact,
decision. All were found on 2026-09-29 while drafting, before any sealed data; decision for all: continue.

| ID | Differs from | What we do and why | Impact |
|---|---|---|---|
| D1 | LEP 56-161, 121-126, 435-599 (Rust crates, Python scripts, `turing` CLI) | C tools and shell scripts (house rule: C and shell only) | none on numbers; same checks |
| D2 | LEP 56-161, CVP 1057-1081 (`turing-lab/` tree) | `calibration/` inside omega, bundle `experiments/EXP-001/` (brief names these paths) | layout only |
| D3 | LEP 212-231 (canonical JSON, media_type, created_by) | SHA-256 over exact file bytes, because every artifact is byte-deterministic (fixed writer code, integer values, no timestamps in pinned files) | simpler; any byte change is detected. **Accepted by orchestrator 2026-09-29.** |
| D4 | LEP 603-631, CVP 1125-1153 (trajectory-level bootstrap) | per-crumb bootstrap (brief; UNCERTAINTY_PROTOCOL.md); context resets at each crumb so code lengths add exactly; x2.04 inflated interval reported | narrower intervals than seed-level; reported, not used for verdict |
| D5 | CVP 1057-1081 (streams and coded files inside the bundle) | large files kept outside git; SHA-256 and bytes in INDEX files and results records | same pinning, smaller repo |
| D6 | CVP 1083-1122, LEP 409-430 (verifier signature) | `signed_by` = unsigned for profile v1.0; the git commit and its push are the record | no cryptographic signature. **Accepted by orchestrator 2026-09-29**; signing receipts with the owner key is a candidate for a later profile version. |
| D7 | LEP 190-208 (git tags) | no git tags. The freeze is recorded by `freeze_receipt.json` (C_f, TURING_PROFILE_V1_FROZEN = PASS) in a commit after C_f; the release by `seed_commitment.json` started_utc. Both are committed files with SHA-256 pins, which a tag would only name | none on numbers; a tag can be moved, a committed receipt cannot be changed without a new commit |
| D8 | LEP 163-188 (separate principals) | same OS user in bwrap jails; temporal non-access (sealed data generated only after the freeze commit is on main) is the primary barrier | stated limit in BLINDING_PROTOCOL.md |
| D9 | CVP 445-477 (gate PASS or FAIL only) | adds INCONCLUSIVE with an exact trigger, per LEP 597-599 and the brief | three-valued verdict; never converted after the result |
| D10 | CVP 125-148 (profile frozen before candidate development is completed) | the profile is frozen together with the candidates in one commit C_f and holds numbers measured on the built candidates (model digests, L(M), the coder envelope measured on all seven) | the order is profile and candidates together, then sealed data. No sealed data exist before C_f, so nothing in the profile can depend on sealed data; the dev envelope is fitted to dev data only and stated as such |

## Open before freeze or verdict

- O3: runtime digest (last FILL_AT_FREEZE), then sidecar, then `freeze_candidate.sh --freeze` (row 50).

Closed: O1 (lane D independent scorer, rows 22 and 46) and O2 (CAL-0 fresh-reader review, row 50; resolution table `CAL0_REVIEW_RESOLUTION.md`) in the CAL-0 fix round. Earlier: O4 (notebook record and DIRTY_TREE refusal, row 51) and O5 (unexplained reversal is FAIL, row 20).

## Counts

53 requirements: 24 met, 6 met with a deviation, 2 met by declaration or scope (energy not measured in
EXP-001; full sensitivity sweep belongs to a later gate), 13 fixed now (one of them with D9), 7 deviations only,
1 open. Ten deviations in all (D1-D10); D3 and D6 accepted by the orchestrator on 2026-09-29.
