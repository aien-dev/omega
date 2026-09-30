# EXP-001 data and bundle format (docs-only reference)

This document is enough to read every EXP-001 input and output without the omega or crumbs source. It closes
the independent scorer's gaps G1 (CTR1 layout), G3 (crumb boundary), G5 (bundle layout), G6 (profile digest),
G9 (event-index gaps) and G10 (crumb saturation). It is part of the shared background hashed into the candidate
manifest. Where it and the code disagree, the code is wrong and the run is a terminal FAIL of the criterion that
used the value (FAILURE_REPORTING.md section 2).

All integers are little-endian and unsigned unless stated. "SHA-256" means lowercase hex.

## 1. CTR1 trace file (`control/trace.ctr`)

A headerless stream of 247-byte records. The file size must be a multiple of 247, otherwise the file is refused.
Each record is a 215-byte body followed by a 32-byte BLAKE3 chain digest. The chain is not verified by EXP-001
(no BLAKE3 in the tree); file integrity is pinned by the SHA-256 in the dataset manifest instead (G12).

| Offset | Size | Field | Used by EXP-001 |
|---:|---:|---|---|
| 0 | 4 | magic, ASCII `CTR1` | refused if different |
| 4 | 2 | version u16 | must be 1 |
| 6 | 1 | kind u8: 1 = EXPAND, 2 = SUBMIT | symbol, refusal |
| 7 | 1 | result_class u8: 1 Pruned, 2 Failed, 3 Partial, 4 Improved, 5 Verified, 6 RejectedHidden, 7 RejectedRobust | symbol |
| 8 | 4 | event_index u32 | crumb boundary |
| 12 | 4 | parent u32 | no |
| 16 | 4 | child u32 | no |
| 20 | 2 | op_index u16 | context (EXPAND) |
| 22 | 1 | op_origin u8 | must be 0 (control arm: base ops only) |
| 23 | 1 | prune u8: 0 none, 1 EQUIV, 2 COST, 3 FRONTIER_CAP, 4 STEP_CAP | symbol |
| 24 | 1 | verify u8 | must be <= 4 |
| 25 | 1 | fit u8 | must be <= 2 |
| 26 | 1 | hidden u8 | no |
| 27 | 1 | improved u8 | no |
| 28 | 1 | contributed u8 | no |
| 29 | 2 | depth u16 | context, clipped to min(depth, 7) |
| 31 | 32 | op_id | no |
| 63 | 32 | state_digest | no |
| 95 | 32 | child_state_digest | no |
| 127 | 32 | program_digest | no |
| 159 | 2 | mismatch_before u16 | no |
| 161 | 2 | mismatch_after u16 | no |
| 163 | 4 | hamming_before u32 | no |
| 167 | 4 | hamming_after u32 | no |
| 171 | 4 | exec_cost u32 | no |
| 175 | 4 | search_cost u32 | no |
| 179 | 2 | program_steps u16 | no |
| 181 | 2 | oracle_index u16 | no |
| 183 | 32 | residual u64[4] | no |
| 215 | 32 | BLAKE3 chain digest | no (see above) |

The overlap audit's crumb-block hash (verify_holdout_separation.sh G9) covers bytes 0..214 of every record.

### 1.1 Symbol mapping (alphabet K = 9)

| Symbol | Name | Condition |
|---:|---|---|
| 0 | P_EQUIV | kind 1, prune 1 (result_class must be 1) |
| 1 | P_COST | kind 1, prune 2 (result_class must be 1) |
| 2 | P_FRONTIER | kind 1, prune 3 (result_class must be 1) |
| 3 | P_STEPCAP | kind 1, prune 4 (result_class must be 1) |
| 4 | FAILED | kind 1, prune 0, result_class 2 |
| 5 | PARTIAL | kind 1, prune 0, result_class 3 |
| 6 | IMPROVED | kind 1, prune 0, result_class 4 |
| 7 | ACCEPT | kind 2, prune 0, result_class 5 |
| 8 | REJECT | kind 2, prune 0, result_class 6 or 7 |

The context feature `op` is op_index (0..14) for EXPAND and 15 for SUBMIT. Refusals (the evaluator reads and validates every CTR1 file before it scores any file, so a bad file always
refuses before any score exists: FORMAT, a void; EVALUATOR.md section 3): bad magic; version != 1; op_origin != 0; verify > 4; fit > 2; kind not 1 or 2;
EXPAND with op_index >= 15 or prune > 4; pruned EXPAND with result_class != 1; unpruned EXPAND with result_class
outside 2..4; SUBMIT with prune != 0 or result_class outside 5..7.

### 1.2 Crumb boundary (G3)

A record with event_index 0 starts a new crumb (this is the "first" flag and the "same crumb id" of
EVALUATOR.md and CODER_SPEC.md). Every other record must have event_index strictly greater than the previous
record in the file, otherwise the file is refused. The first record of a file must have event_index 0. A jump of
more than one inside a crumb is a gap: it is counted and reported (`gaps`), never refused (G9). Crumbs are
numbered 0, 1, 2, ... in file order; the crumb ordinal stored in TPS1 and used by the POS context is
min(ordinal, 65535) (G10). A crumb is the unit of the per-crumb envelope (S4) and of the bootstrap pool.
Saturation is accepted (CAL-0 review 2 Q15): in a file with more than 65,535 crumbs, every crumb from ordinal 65535
on would share one ordinal, so the evaluator would treat them as a single crumb (one bootstrap unit, one per-crumb
envelope entry) and the POS context would see one value for them. It cannot happen at this design: the development
traces hold 181 to 188 crumbs per file, about 350 times below the limit, and the sealed traces come from the same
generator with the same trace length.

## 2. Profile digest (G6)

The profile digest is the SHA-256 of the bytes of `calibration/profiles/Turing-profile-v1.0.toml`. The sidecar
`Turing-profile-v1.0.sha256` and the bundle file `profile.digest` both hold one line `<digest>  Turing-profile-v1.0.toml`
+ LF; "the digest" is the 64-hex value on that line, not the hash of the sidecar file. The same 32 bytes sit at
TPS1 header offset 28 (CODER_SPEC.md section 2).

## 3. Dataset manifest (`dataset_manifest.json`, schema turing.cal.dataset_manifest.v1)

Written by `calibration/scripts/make_dataset_manifest.sh` (`--dev` for development data, `--sealed` for the sealed
set). Top-level keys: schema, dataset_id, experiment, split (`development` or `sealed_test`), profile_digest,
freeze_commit (C_f, or `NONE` for development data), generator_digest, learner_digest, parameter_commitment,
seed_commitment_sha256, sealed_manifest_sha256, released_utc, generation_finished_utc, trajectory_count,
payload_digest, files. Each `files` entry is one line
`{"group": g, "index": j, "seed": S, "sha256": ..., "bytes": ..., "path": ...}` with group 1 (primary) or 2
(replication) and index j = 0, 1, 2 within the group (j is the position in the seed-commitment order, which is the
derivation index; it is also the bootstrap pool order, UNCERTAINTY_PROTOCOL.md section 3).

## 4. Run root and bundle layout (G5)

A sealed run (and the dev dry run, `tests/turing/test_tc_eval_dry.sh`) uses one run root R and runs with the
working directory R:

```
R/candidate_manifest.json     copy of the frozen manifest (from C_f)
R/dataset_manifest.json       the sealed (or dev) dataset manifest, section 3
R/candidates/                 the seven .tym files (or --cand-dir)
R/bundle/                     the bundle (below); sealed R = $TC_EVAL_ROOT/<C_f>/run (FAILURE_REPORTING.md section 6)
R/work/                       large intermediate files, referenced by relative path "work/..."
R/work/probability_streams/g<g>_j<j>_<C>.tps   TPS1 stream per candidate and file
R/work/symbols/g<g>_j<j>_<C>.tsy               TSY1 symbol stream per candidate and file
R/work/arithmetic/g<g>_j<j>_<C>.tcr            range coder output (TCR1)
R/work/ans/g<g>_j<j>_<C>.tca                   rANS coder output (TCA1)
```

Bundle files (EVALUATOR.md section 5 describes their content):

| File | Content |
|---|---|
| `profile.digest` | section 2 |
| `probability_streams/INDEX` | one line per candidate and file: `<TPS1 file sha256> <TPS1 trailer digest> <TSY1 trailer digest> <g> <index> <candidate> <bytes> <path>`; first line is a `#` comment |
| `encoded_artifacts/INDEX` | one line per coder, candidate and file: `<coded file sha256> <coder: range or rans> <g> <index> <candidate> <bytes> <path>`; first line is a `#` comment |
| `ideal_lengths.json` | whole-file and per-crumb ideal lengths (ub), event counts, per-crumb coded bytes |
| `arithmetic/results.json`, `ans/results.json` | coded sizes and decode results per coder |
| `decoder_receipts/g<g>_j<j>_<C>.json` | lossless decode receipt per candidate and file |
| `scorer_primary.json` | the S8 key set (EVALUATOR.md section 6) |
| `scorer_independent.json` | the independent scorer's output (tools/turing_verify_indep) |
| `uncertainty.json` | bootstrap intervals |
| `final_receipt.pending.json`, `REPORT.pending.md` | before the gate |
| `final_receipt.json`, `REPORT.md` | after the gate (write-once) |
| `void_receipt_<n>.json` | one per void attempt (FAILURE_REPORTING.md section 2) |

`<path>` in both INDEX files is relative to R. TSY1 files are not listed in an INDEX; their path is fixed by the
naming above. The per-crumb coded sizes in `ideal_lengths.json` come from coding each crumb alone with its own
56-byte header; they are published but are outside the S8 key set (G8).
