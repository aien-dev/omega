# EXP-001 primary evaluator: turing-cal-eval

Source: `tools/turing_cal_eval.c` (C11 + POSIX). Build: `make turing-cal-eval` ->
`build/turing-exp001-eval/turing-cal-eval`. Its SHA-256 is part of the frozen runtime digest
(`calibration/scripts/runtime_digest.sh`, six binaries). Statistics: `UNCERTAINTY_PROTOCOL.md`. Coders and
the TPS1 / TSY1 / TCR1 / TCA1 byte layouts: `CODER_SPEC.md`. Verdict rule: `preregistration/EXP-001.md`
section 5a. All decisions use int64 micro-bits (ub; 1 bit = 1,000,000 ub).

## 1. Commands

```
turing-cal-eval run  [--dry-run] --repo DIR --manifest CANDIDATE_MANIFEST --cand-dir DIR [--cand-dir DIR ...]
                     --dataset DATASET_MANIFEST [--overlap OVERLAP_AUDIT] --out BUNDLE_DIR --work WORK_DIR
                     [--only NAME,NAME,...]
turing-cal-eval gate [--dry-run] --bundle BUNDLE_DIR --independent SCORER_INDEPENDENT_JSON
```

- `--repo`: the checkout at the freeze commit (profile, sidecar, preregistration, shared-background files).
- `--manifest`: `calibration/experiments/EXP-001/candidate_manifest.json` (written by `freeze_candidate.sh`).
- `--cand-dir`: where the `.tym` candidate files are found (first match wins); every file is re-hashed.
- `--dataset`: a dataset manifest from `calibration/scripts/make_dataset_manifest.sh` (section 4).
- `--overlap`: `overlap_audit.json`; required outside a dry run.
- `--only`: score a subset of candidates; dry run only.
- `--dry-run`: development data only; output may not be under `calibration/experiments/EXP-001`; the receipt
  says `kind = dry_run` and `EXP_001_COMPRESSION_BRIDGE = DRY_RUN_NOT_EVIDENCE`, and S2 is `NOT_CHECKED`.

Exit codes: 0 finished (whatever the verdict), 2 refused (the run is VOID), anything else is a crash and
is also VOID.

## 2. What `run` does, in order

1. Profile: SHA-256 of the TOML must equal the sidecar; outside a dry run the TOML may not contain
   `FILL_AT_FREEZE`.
2. Candidate manifest: its `profile_sha256` must equal the profile; outside a dry run `status = frozen` and
   `frozen_at` present. Every `shared_background_sha256` entry is re-hashed in `--repo`. The evaluator's own
   SHA-256 must equal the frozen `turing-cal-eval` runtime digest (sealed mode). Each candidate file must match
   `file_sha256`; after loading, its model digest and exact L(M) bits must match the manifest.
3. Dataset manifest: `profile_digest` must equal the profile; split must be `sealed_test` (sealed mode) or
   `development` (dry run); `payload_digest` must equal the file list; files in group then index order, both
   groups present; every trace re-hashed.
4. Freeze order (sealed mode): overlap audit present and PASS; the manifest `git_head`, the sealed
   `freeze_commit` and the audit commit are one commit; candidate `frozen_at` strictly before the data release
   time.
5. For each sealed file and candidate: produce the TPS1 probability stream and TSY1 symbol stream, write them,
   re-read and parse them, encode with coder A (TCR1 range) and coder B (TCA1 rANS) from the same parsed TPS1,
   write, re-read, decode, compare with the symbols (S1). Coded headers must carry the TPS1 digest (S4,
   `BINDING`). All candidates must see the same symbol stream (S3, `SYMBOL_STREAM`). Per crumb: ideal length,
   each coder on the crumb alone (56-byte header included), round trip.
6. Per-crumb bootstrap (B = 10000, seed 0x4558503030315543 + group, common random numbers across candidates),
   envelope per file and per crumb, reversal audit, criteria S1-S7 and S9.
7. Write the bundle and the pending receipt and report (`final_receipt.pending.json`, `REPORT.pending.md`).

## 3. Refusal codes

Every refusal prints `REFUSED <step>: <code>: <reason>`, exits 2 and writes `<out>/void_receipt_<n>.json`
(schema `schemas/void_receipt.schema.json`; n = first free number, created exclusively, never overwritten).

| Code | Meaning |
|---|---|
| ARG | bad or missing argument; `--only` outside a dry run |
| IO | a file could not be read or written |
| DRY_RUN_TARGET | a dry run tried to write under calibration/experiments/EXP-001 |
| EXISTS | final_receipt.json already exists; a verdict is never replaced |
| PROFILE_DIGEST | profile differs from its sidecar, or a manifest names another profile |
| NOT_FROZEN | FILL_AT_FREEZE left, manifest not frozen, or no frozen_at (sealed mode) |
| FORMAT | malformed manifest, dataset manifest, stream or criterion |
| DIGEST | a candidate file or shared-background file differs from the manifest |
| RUNTIME_DIGEST | the evaluator binary is not the frozen one |
| MODEL_DIGEST | a loaded model's digest differs from the manifest |
| LM_BITS | a loaded model's L(M) differs from the manifest |
| SPLIT | sealed mode with development data, or a dry run with sealed data |
| DATASET_DIGEST | payload digest or a trace file differs from the dataset manifest |
| OVERLAP | overlap audit missing, unreadable or not PASS |
| FREEZE_COMMIT | manifest, sealed data and audit name different commits |
| FREEZE_AFTER_RELEASE | candidates frozen at or after the data release, or no release time |
| BINDING | a coded header is not bound to the TPS1 digest |
| SYMBOL_STREAM | candidates see different symbol streams |
| VOID | a group has no crumbs |
| MODE | gate `--dry-run` disagrees with the bundle |
| NOT_INDEPENDENT | sealed-mode gate given a byte copy of scorer_primary.json |

## 4. Dataset manifest

`make_dataset_manifest.sh --sealed SEALED_ROOT OUT.json` (after `generate_sealed_data.sh`; checks COMPLETE,
seed_commitment.json and manifest.json, re-hashes every control trace) or
`make_dataset_manifest.sh --dev RUN_DIR "G1 SEEDS" "G2 SEEDS" PROFILE_SHA256 OUT.json` (dry run only;
`released_utc = NOT_SEALED`). Schema `turing.cal.dataset_manifest.v1`; one file entry per line; `payload_digest`
= SHA-256 over the LF-terminated lines `<g> <index> <seed> <sha256> <bytes>` in group then index order.

## 5. Bundle and work directory

Bundle (`--out`; for the sealed run `calibration/experiments/EXP-001`): `profile.digest`, `ideal_lengths.json`
(per file and per crumb), `scorer_primary.json`, `uncertainty.json`, `probability_streams/INDEX`,
`arithmetic/results.json`, `ans/results.json`, `encoded_artifacts/INDEX`, `decoder_receipts/`,
`final_receipt.pending.json`, `REPORT.pending.md`, and after `gate` `scorer_independent.json`,
`final_receipt.json`, `REPORT.md`. Work dir (`--work`, outside git): the large files
`probability_streams/*.tps`, `symbols/*.tsy`, `arithmetic/*.tcr`, `ans/*.tca`. The INDEX files and results
records carry every large file's SHA-256 and byte count, so the bundle pins them.

Roots in the receipt: `candidate_root` = SHA-256 of the candidate manifest; `dataset_root` = the dataset
`payload_digest`; `probability_root`, `coding_root` and `analysis_root` = SHA-256 over the sorted lines
`<sha256>  <relpath>\n` (sha256sum format) of, respectively, `probability_streams/INDEX`; `arithmetic/results.json`,
`ans/results.json`, `encoded_artifacts/INDEX`; `ideal_lengths.json`, `scorer_primary.json`,
`scorer_independent.json`, `uncertainty.json`.

## 6. scorer_primary.json and the independent scorer (S8)

Schema `turing.cal.scorer.v1`. The `values` array has one object per line, `{"key": K, "value": V}`, V an
int64. Keys, for g in 1, 2 and each candidate C: `g<g>.crumbs`, `g<g>.events`, `g<g>.<C>.lm_bits`,
`g<g>.<C>.ideal_ub`, `g<g>.<C>.range_bits`, `g<g>.<C>.rans_bits`, and for X in ideal, range, rans:
`g<g>.<C>.T_<X>_vs_B2.point_ub`, `.lo_ub`, `.hi_ub`. The independent scorer (lane D) writes the same shape;
key order is free. `gate` sets S8 = PASS only if both files have the same key set and every value is equal.
In sealed mode a byte-identical file is refused (NOT_INDEPENDENT); a dry run allows the self-comparison and
labels it.

`gate` then applies the verdict rule (FAIL if any criterion FAIL or NOT_REACHED, else INCONCLUSIVE if any is
INCONCLUSIVE, else PASS), computes `analysis_root`, and writes `final_receipt.json` and `REPORT.md` exactly
once (exclusive create).

## 7. Tests

`make test-turing-exp001-eval` runs `tests/turing/test_tc_eval.sh` with the plain and the ASan build on the
small committed fixture: every row of the refusal table that a tampered input can trigger (dry-run target,
tampered candidate, wrong model digest, profile edited, stale dataset manifest, shared background changed,
dataset file or trace changed, gate mode mismatch, write-once, draft manifest in sealed mode, runtime digest,
development split in sealed mode, missing or failing overlap audit, freeze commit mismatch, frozen after
release, FILL_AT_FREEZE, self-comparison) must exit 2 with the named code; the positive dry run and gate
must finish, and plain and ASan output must agree. `make turing-exp001-eval-dry TE_DATA=<dev run dir>` runs
the full development dry run (refuses to start when `~/workspace/.spark-quiet` exists).
