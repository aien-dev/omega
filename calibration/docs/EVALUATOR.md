# EXP-001 primary evaluator: turing-cal-eval

Source: `tools/turing_cal_eval.c` (C11 + POSIX). Build: `make turing-cal-eval` ->
`build/turing-exp001-eval/turing-cal-eval`. Its SHA-256 is part of the frozen runtime digest
(`calibration/scripts/runtime_digest.sh`, seven binaries, including the independent scorer). Statistics: `UNCERTAINTY_PROTOCOL.md`. Coders and
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

Exit codes: 0 finished (whatever the verdict); 2 refused before any sealed score existed (the attempt is VOID,
`void_receipt_<n>.json`); 1 a terminal failure (a criterion FAILs, `final_receipt.json` with verdict FAIL, see
section 3). A crash (any other exit) before `run` has started scoring is VOID and follows the same retry rule. A
crash after scoring started is a terminal FAIL of the criterion whose step crashed, recorded by hand in
`final_receipt.json` with the stderr tail; it is never retried (FAILURE_REPORTING.md section 2).

## 2. What `run` does, in order

1. Profile: SHA-256 of the TOML must equal the sidecar; outside a dry run the TOML may not contain
   `FILL_AT_FREEZE`.
2. Candidate manifest: its `profile_sha256` must equal the profile; outside a dry run `status = frozen` and
   `frozen_at` present. Every `shared_background_sha256` entry is re-hashed in `--repo`. The evaluator's own
   SHA-256 must equal the frozen `turing-cal-eval` runtime digest (sealed mode). In sealed mode
   `independent_scorer_source_sha256` must equal `calibration/scripts/indep_source_digest.sh` run in `--repo`
   (INDEP_SOURCE). Each candidate file must match `file_sha256`; after loading, its model digest and exact L(M)
   bits must match the manifest (this pass loads, checks and frees every candidate before any scoring).
3. Dataset manifest: `profile_digest` must equal the profile; split must be `sealed_test` (sealed mode) or
   `development` (dry run); `payload_digest` must equal the file list; files in group then index order, both
   groups present; every trace re-hashed.
4. Freeze order (sealed mode; two-step freeze, BLINDING_PROTOCOL.md section 2). C_f is the dataset manifest's
   `freeze_commit`. Checks, in order: overlap audit present (void if not) and PASS; C_f is 40 hex; the audit
   names C_f; `origin/main` exists in `--repo` and C_f is its ancestor (fetch first; void if not); the candidate
   manifest and the profile given to the run hash to the files at C_f (`git show C_f:<path>`); C_f's commit time
   is before `released_utc`; every file's seed equals the `turing.cal.sealed.v1` seed for (C_f, profile digest,
   g, j). In both modes the manifest `frozen_at` must be before `released_utc` when both exist.
5. For each sealed file and candidate: produce the TPS1 probability stream and TSY1 symbol stream, write them,
   re-read and parse them, encode with coder A (TCR1 range) and coder B (TCA1 rANS) from the same parsed TPS1,
   write, re-read, decode, compare with the symbols (S1). Coded headers must carry the TPS1 digest (S3,
   `BINDING`). All candidates must see the same symbol stream (S3, `SYMBOL_STREAM`). Per crumb (crumb boundary:
   DATA_FORMAT.md section 1.2): ideal length, each coder on the crumb alone (56-byte header included), round trip.
6. Per-crumb bootstrap (B = 10000, seed 0x4558503030315543 + group, common random numbers across candidates),
   envelope per file and per crumb for every candidate, reversal audit, criteria S1-S7 and S9.
7. Write the bundle and the pending receipt and report (`final_receipt.pending.json`, `REPORT.pending.md`).

Before step 5 the evaluator records the notebook: operator (`git config user.name`), host (hostname), kernel
(`uname` system, release, machine), commit (`git rev-parse HEAD` of `--repo`) and whether the tree is clean
(`git status --porcelain` empty). All five go into the receipt under `notebook`. Outside a dry run a tree that is
not clean is refused (DIRTY_TREE) before any measurement; a dry run records the state and continues.

## 3. Refusal codes, void attempts and terminal failures

Three outcomes (FAILURE_REPORTING.md section 2 is the rule; this is how the evaluator applies it):

- **Void** (exit 2): an infrastructure or operator problem found before any sealed score exists. Prints
  `REFUSED <step>: <code>: <reason>` and writes `<out>/void_receipt_<n>.json` (schema
  `schemas/void_receipt.schema.json`; n = attempt number, created exclusively, never overwritten) with the exact
  `command` and `inputs` (SHA-256 of the evaluator, candidate manifest, dataset manifest and overlap audit). A
  retry must use the same `--out`, and its command and inputs must be byte-identical to attempt 1
  (RETRY_DIFFERS otherwise). In sealed mode the third void attempt also writes `final_receipt.json` (schema
  `schemas/terminal_receipt.schema.json`, kind `inconclusive_infra`): verdict INCONCLUSIVE, reason INFRA. Every
  void receipt is published.
- **Terminal FAIL** (exit 1): a criterion fails in a way retrying cannot change. Prints `FAILED <step>: <code>:
  ...` and writes `final_receipt.json` (kind `terminal_fail`, verdict FAIL, the named criterion FAIL, the others
  NOT_REACHED). This covers every failure after scoring started (the model-check pass has finished) and, in
  sealed mode, the freeze-order violations marked S2 below. In a dry run the S2 cases are voids.
- Refusals by `gate` before its comparison (ARG, EXISTS, IO of the pending files, MODE, NOT_INDEPENDENT) write no
  receipt at all: they are operator errors on a finished run and do not count as attempts. After that point any
  gate failure is a terminal FAIL.

| Code | Meaning | Outcome |
|---|---|---|
| ARG | bad or missing argument; `--only` outside a dry run | void |
| IO | a file could not be read or written | void before scoring, terminal FAIL after |
| DRY_RUN_TARGET | a dry run tried to write under calibration/experiments/EXP-001 | void |
| EXISTS | final_receipt.json already exists; a verdict is never replaced | no receipt |
| RETRY_DIFFERS | a retry's command or inputs differ from void attempt 1 | void |
| PROFILE_DIGEST | profile differs from its sidecar, or a manifest names another profile | void |
| NOT_FROZEN | FILL_AT_FREEZE left, manifest not frozen, or no frozen_at (sealed mode) | void |
| FORMAT | malformed manifest, dataset manifest, stream or criterion | void before scoring; S3 terminal FAIL for a stream that fails to serialize or parse |
| DIGEST | a candidate file or shared-background file differs from the manifest | void |
| RUNTIME_DIGEST | the evaluator binary is not the frozen one | void |
| INDEP_SOURCE | the independent scorer source in `--repo` differs from the frozen hash | void |
| MODEL_DIGEST | a loaded model's digest differs from the manifest | void |
| LM_BITS | a loaded model's L(M) differs from the manifest | void |
| SPLIT | sealed mode with development data, or a dry run with sealed data | void |
| DATASET_DIGEST | payload digest or a trace file differs from the dataset manifest | void |
| OVERLAP | overlap audit missing or unreadable (void); audit result not PASS (S2 terminal FAIL) | see meaning |
| FREEZE_COMMIT | C_f malformed, audit names another commit, no origin/main or C_f not its ancestor (void); candidate manifest or profile differ from the files at C_f (S2 terminal FAIL) | see meaning |
| FREEZE_AFTER_RELEASE | no release time (void); C_f committed, or candidates frozen, at or after the data release (S2 terminal FAIL) | see meaning |
| SEED_DERIVATION | a sealed seed is not the rule's seed for C_f | S2 terminal FAIL |
| BINDING | a coded header is not bound to the TPS1 digest | S3 terminal FAIL |
| SYMBOL_STREAM | candidates see different symbol streams | S3 terminal FAIL |
| CODER | a coder fails to encode a whole file or a crumb | S1 terminal FAIL |
| CRUMB_SUM | per-crumb ideal lengths do not add up to the whole-file ideal length | S4 terminal FAIL |
| NO_CRUMBS | a group has no crumbs | S6 terminal FAIL |
| MODE | gate `--dry-run` disagrees with the bundle | no receipt |
| NOT_INDEPENDENT | sealed-mode gate given a byte copy of scorer_primary.json | no receipt |
| DIRTY_TREE | outside a dry run, `--repo` is not a git checkout at its top level, or `git status --porcelain` is not empty | void |

A decoder mismatch (CORRUPT, TRAIL or a round-trip difference) is not a refusal: it is recorded and makes S1
FAIL through the normal criterion path.

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

## 6. scorer_primary.json and the independent scorer (S8): the field contract

This section is the whole contract for `scorer_independent.json`. The independent scorer reads only the
dry-run or sealed bundle and the CTR1 trace files named in its dataset manifest; it never reads omega source.
The data formats it needs are in `DATA_FORMAT.md` (CTR1 layout, crumb boundary, profile digest, run root and
bundle layout).

The independent scorer used for S8 is `tools/turing_verify_indep` (lane D: written from these documents only,
C, built by `make turing-verify-indep`). Its source is pinned by `independent_scorer_source_sha256` in the
candidate manifest (rule turing.cal.indep_source.v1, `calibration/scripts/indep_source_digest.sh`) and its binary
is one of the seven binaries of the runtime digest. It runs in the evaluator environment, from the frozen tree
at C_f, with the command in BLINDING_PROTOCOL.md section 2 step 8. Any change to its source after the sealed data
is released makes S8 FAIL (a scorer that can be revised after seeing the data is not independent).

### 6.1 File layout

```
{
  "schema": "turing.cal.scorer.v1",
  "scorer": "<free text naming the scorer>",
  "dry_run": true | false,
  "profile_sha256": "<64 hex, copied from profile.digest>",
  "candidate_manifest_sha256": "<64 hex, SHA-256 of the candidate manifest file bytes>",
  "dataset_manifest_sha256": "<64 hex, SHA-256 of the dataset manifest file bytes>",
  "values": [
    {"key": "g1.crumbs", "value": 1234},
    ...
  ]
}
```

The gate reads the file line by line. Rules the writer must follow:

1. Each header field is on its own line as `"name": "value"` (any number of spaces after the colon).
   `schema`, `profile_sha256`, `candidate_manifest_sha256` and `dataset_manifest_sha256` must equal the
   primary file's values exactly (lower-case hex). `scorer` and `dry_run` are not compared.
2. Each value is one object on one line: `{"key": "<key>", "value": <integer>}`. Spaces after a colon are
   free; the order of lines is free.
3. Every value is a base-10 signed 64-bit integer. There are no float fields. A value written with a
   fraction or exponent (`1.0`, `1e6`) is not an integer and never matches.
4. The key set must be exactly the primary key set: no missing key, no extra key, no repeated key.
5. Anything else in the file (for example a `criteria` object) is ignored.

### 6.2 Keys and definitions

Units: `_ub` = micro-bits (1 bit = 1,000,000 ub); `_bits` = bits; `_bytes` = bytes. Groups g = 1, 2.
Candidates C are the seven manifest names (`B0_uniform`, `B1_order0`, `B2_order1`, `B3_heuristic`,
`M_candidate`, `M_mem`, `M_mem_seed1`), in candidate-manifest order. Files j = the dataset-manifest `index` within group g.

- A **crumb** starts at every CTR1 record with event_index 0 and runs to the record before the next one
  (DATA_FORMAT.md section 1.2). Crumbs are listed in trace order; group g's pool is its files in index order,
  crumbs in trace order.
- `ideal(event)` = `ub(q)` micro-bits, where q (1..65536) is the candidate's 16-bit probability of the observed
  symbol in its TPS1 stream (65536 stands for probability 1). `ub(q)` is this exact integer rule and no other
  (it is `ty_ubits_q16`; the independent scorer implements the same steps):
  1. `log2(q)` in Q32: `ip` = index of the highest set bit of q; `m = q << (62 - ip)` (a Q62 mantissa in
     [1, 2)); `frac = 0`; repeat for i = 1..32: `m = (m * m) >> 62` (128-bit product, truncated); if
     `m >= 2^63` then set bit `32 - i` of `frac` and `m = m >> 1`. The result is `(ip << 32) | frac`.
  2. `x = (16 << 32) - log2(q)` (that is -log2(q/65536) in Q32, truncated).
  3. `ub(q) = (x * 1000000 + 2^31) >> 32` (unsigned 64-bit).
  Test values: ub(1) = 16000000, ub(32768) = 1000000, ub(65536) = 0, ub(43481) = 591903, ub(46819) = 485194.
  At exactly those two q the result is one micro-bit above the correctly rounded value of
  `1e6 * -log2(q/65536)` (591902 and 485193); the rule above is the definition and wins. Every other q agrees
  with correct rounding.
  Use the TPS1/TSY1 files in the bundle; they are the candidates' exact predictions.

| Key | Value |
|---|---|
| `g<g>.crumbs` | number of crumbs over all files in group g |
| `g<g>.events` | number of events over all files in group g |
| `g<g>.<C>.lm_bits` | exact L(M) in bits (the TYM0 bit count; equals the candidate manifest `lm_bits`) |
| `g<g>.<C>.ideal_ub` | sum of `ideal(event)` over group g |
| `g<g>.<C>.range_bits` | 8 x byte size of the TCR1 files of group g (56-byte header included), summed |
| `g<g>.<C>.rans_bits` | 8 x byte size of the TCA1 files of group g (56-byte header included), summed |
| `g<g>.<C>.T_ideal_vs_B2.point_ub` | `ideal_ub(B2) - ideal_ub(C) - (lm_bits(C) - lm_bits(B2)) x 1e6` |
| `g<g>.<C>.T_ideal_vs_B2.lo_ub`, `.hi_ub` | bootstrap interval, below |
| `g<g>.<C>.T_range_vs_B2.point_ub` | `(range_bits(B2) - range_bits(C) - lm_bits(C) + lm_bits(B2)) x 1e6` |
| `g<g>.<C>.T_rans_vs_B2.point_ub` | same with `rans_bits` |
| `g<g>.<C>.T_range_vs_B2.lo_ub`, `.hi_ub` (and rans) | the ideal lo/hi shifted by `-(o(C) - o(B2))`, where `o(M) = coded_bits(M) x 1e6 - ideal_ub(M)` for that coder |
| `g<g>.f<j>.<C>.ideal_ub` | sum of `ideal(event)` over file j of group g |
| `g<g>.f<j>.<C>.range_bytes` | byte size of that file's TCR1 file (header included) |
| `g<g>.f<j>.<C>.rans_bytes` | byte size of that file's TCA1 file (header included) |

For C = `B2_order1` every T value is 0.

**Bootstrap interval (per group g).** Let the pool be the group's crumbs, Cn of them, and `u(C, k)` the
ideal ub of crumb k under candidate C. Seed `st = 0x4558503030315543 + g` (unsigned 64-bit). Draw with
splitmix64 (`st += 0x9E3779B97F4A7C15; z = st; z = (z ^ z>>30) * 0xBF58476D1CE4E5B9;
z = (z ^ z>>27) * 0x94D049BB133111EB; return z ^ z>>31`). Rejection limit
`lim = UINT64_MAX - ((UINT64_MAX % Cn) + 1) % Cn`; draw r until `r <= lim`, index = `r % Cn`. For
b = 0..9999, draw Cn indices in order; one index is shared by all candidates (common random numbers) and
adds `u(C, index)` to `S(C, b)`. Then `t_b = S(B2, b) - S(C, b) - (lm_bits(C) - lm_bits(B2)) x 1e6`; sort the
10000 values ascending; `lo_ub` = element 250, `hi_ub` = element 9749 (0-based). All arithmetic is signed
64-bit integer.

### 6.3 The comparison rule

`gate` sets S8 = PASS only when all of these hold; otherwise S8 = FAIL:

- the four compared header fields are equal;
- the independent file has no repeated key;
- both files have the same number of values and every primary key is present in the independent file;
- every value is bit-exactly equal (zero tolerance; there are no float fields).

In sealed mode a byte-identical copy of `scorer_primary.json` is refused (NOT_INDEPENDENT); a dry run allows
the self-comparison and labels it in the receipt notes.

`gate` then applies the verdict rule (FAIL if any criterion FAIL or NOT_REACHED, else INCONCLUSIVE if any is
INCONCLUSIVE, else PASS), computes `analysis_root`, and writes `final_receipt.json` and `REPORT.md` exactly
once (exclusive create).

## 7. Tests

`make test-turing-exp001-eval` runs `tests/turing/test_tc_eval.sh` with the plain and the ASan build on the
small committed fixture: every row of the refusal table that a tampered input can trigger (dry-run target,
tampered candidate, wrong model digest, profile edited, stale dataset manifest, shared background changed,
dataset file or trace changed, gate mode mismatch, write-once, draft manifest in sealed mode, runtime digest,
development split in sealed mode, missing overlap audit, audit naming another commit, C_f not on origin/main,
FILL_AT_FREEZE, self-comparison, dirty or non-git tree, independent scorer source changed, retry
with a different command) must exit 2 with the named code; three void attempts give INCONCLUSIVE INFRA and a
fourth is refused; the sealed freeze-order failures (overlap FAIL, manifest edited after C_f, seed not derived
from C_f, released before C_f, frozen after release) and a gate failure after scoring must exit 1 with a
terminal-fail final_receipt.json and no void receipt; freeze_receipt.sh must refuse an unfilled runtime
digest and existing sealed data and PASS otherwise; the sealed
pending receipt must carry the notebook record; S8 must PASS for a reordered, re-spaced copy and FAIL for a
float value, a different profile digest and a repeated key; the positive dry run and gate
must finish, and plain and ASan output must agree. `make turing-exp001-eval-dry TE_DATA=<dev run dir>` runs
the full development dry run with the independent scorer, which must give S8 PASS and verdict PASS (refuses to start when `~/workspace/.spark-quiet` exists).
