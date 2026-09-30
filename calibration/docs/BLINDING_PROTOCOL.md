# Blinding protocol: Turing-profile-v1.0, EXP-001 (and Turing-profile-v1.1, EXP-001R, see the end of section 4)

This document says who may see which data, when the sealed test data comes into
existence, how the machine enforces the separation, and how anyone checks
afterwards that it held. It covers gate CAL-0's `blinding_method`,
`holdout_commitment`, `test_generation_seed_commitment`, `sealed_test_split` and
`overlap_audit` fields.

The guarantee has three layers:

1. **The data does not exist yet.** Sealed seeds are derived from the freeze
   commit hash. Nobody can compute them, or generate the data, before that commit
   exists. That commit already contains the hashed candidate artifacts.
2. **The machine hides the data.** After generation, only processes started through
   the Evaluator wrapper can see the sealed directory. Processes started through
   the Candidate wrapper have no such path at all: the kernel builds their file
   system view without it.
3. **Anyone can check it afterwards.** `verify_holdout_separation.sh` re-derives
   every seed, re-hashes every file, and checks file times against the freeze
   commit. It also checks, at the digest and record level, that no sealed crumb
   also appears in development data.

## 1. Roles and environments

| Environment | May read | May write | Network | How it runs |
|---|---|---|---|---|
| Candidate | development data (`~/aien-data/crumbline`, read-only), the public profile and task definition, its own working tree, the git history (read-only) | its working tree | off (on only with `--net`) | `calibration/scripts/candidate_env.sh` |
| Generator | the pinned generator binary and learner (read-only) | one sealed group directory | off | inside `generate_sealed_data.sh` |
| Evaluator | the frozen evaluation tree (a clean clone checked out at C_f, step 7: candidate artifacts, reference coders, verifier, independent scorer; read-only), one sealed set (read-only) | one output directory | off, always | `calibration/scripts/evaluator_env.sh` |
| Auditor | development data and the sealed set, both read-only | the audit output | off | `evaluator_env.sh --ro ~/aien-data/crumbline` running `verify_holdout_separation.sh` |

The Candidate cannot query the Evaluator. The Evaluator runs offline, exposes no
socket or service, and starts only after the candidate freeze.

Paths:

* sealed root: `~/aien-data/turing-cal/sealed/` (mode 0700), one directory per freeze
  commit: `sealed/<commit>/`
* evaluator outputs: `~/aien-data/turing-cal/eval/` (also hidden from the Candidate)
* trial runs (feasibility only, never sealed): `~/aien-data/turing-cal/trial/`

## 2. Lifecycle, in order

Each step names who runs it and where. A step may not start before the one above
it has finished.

1. **Protocol designed; candidate families declared; power simulation; sample size
   fixed.** Lane A, ordinary worktree. The profile states `sealed_seeds_per_group = N`.
   `generate_sealed_data.sh` reads that exact key.
2. **Candidate development.** Candidate side, on development data only. Vocabulary
   (same as the profile `development_split`): rep10 seeds 1-7 are the development
   seeds used for fitting; rep10 seeds 8-10 and everything in `final-20260927` were
   never used for fitting but are burned (seen during earlier work), so they are
   development data too and can never be sealed data. The seed check refuses 0-10
   and 20260927. Any computation that touches data should run through
   `candidate_env.sh` so the sealed root is out of reach even after it exists.
3. **Freeze, step 1: the freeze commit C_f.** `freeze_candidate.sh --freeze` writes
   `calibration/experiments/EXP-001/candidate_manifest.json` with `"status": "frozen"`:
   the hash of every candidate artifact, the profile hash, every shared-background
   hash, `independent_scorer_source_sha256` (the independent scorer's source tree,
   rule in `indep_source_digest.sh`) and `runtime_sha256` (the seven binaries,
   `runtime_digest.sh`). The profile `runtime_digest` value, the sidecar and
   `preregistration.json` `"status": "frozen"` (set before `--freeze`, which refuses
   otherwise) are in the same commit. The manifest names **no commit**: a file cannot hold
   the hash of the commit that contains it. The commit that adds these files is the
   **freeze commit C_f**. It reaches `origin/main` by a real merge or a fast-forward,
   **never a squash or rebase merge** (those make a new commit and C_f would not be
   on main). The binaries hashed at freeze are built from the tree that becomes C_f;
   C_f only adds the manifest, the profile value and the sidecar, none of which is
   compiled into any binary. From here on no candidate artifact, profile byte,
   evaluator or independent-scorer source may change; a change is a new
   experiment version, reported beside this one.
4. **Freeze, step 2: the freeze receipt.** Orchestrator, ordinary shell, after
   `git fetch origin`:

   ```
   calibration/scripts/freeze_receipt.sh <C_f>
   ```

   It reads C_f through git and refuses unless: C_f is an ancestor of `origin/main`;
   the manifest there is frozen; the profile there has no `FILL_AT_FREEZE` and
   matches its sidecar and the manifest; the profile `runtime_digest` equals the
   manifest's; the independent-scorer source at C_f hashes to the manifest value;
   every shared-background file at C_f matches; no sealed data exists for C_f. It
   writes `calibration/experiments/EXP-001/freeze_receipt.json` with
   `TURING_PROFILE_V1_FROZEN = PASS` and C_f. That receipt is committed in a later
   commit (the first place C_f is written down); nothing the seeds or the evaluator
   use changes in it. It is pushed to `origin/main` before step 5: sealed generation refuses to
   start until that receipt, naming C_f and its profile digest, is on `origin/main`.
5. **Sealed data generated.** Orchestrator, ordinary shell (the script jails the
   generator itself):

   ```
   calibration/scripts/generate_sealed_data.sh --commit <C_f> \
       --profile-digest <digest in Turing-profile-v1.0.sha256 at C_f>
   ```

   The seeds are derived from C_f (section 3). The script refuses unless all of these
   hold: C_f is an ancestor of `origin/main` (it fetches first); `origin/main` holds
   `calibration/experiments/EXP-001/freeze_receipt.json` with `TURING_PROFILE_V1_FROZEN = PASS`, naming this C_f
   and this profile digest (step 4 is committed before any sealed byte exists); the candidate
   manifest at C_f is `"status": "frozen"`, the profile at C_f has no
   `FILL_AT_FREEZE` and hashes to the given digest, which the sidecar also carries.
   It also refuses if the generator and learner hashes differ from the pins, or if
   `sealed/<C_f>/` already exists, or if EXP-001 has already ended (three void receipts or a final receipt in
   `~/aien-data/turing-cal/eval/<C_f>/run/bundle`). It records C_f's commit time. Failed attempts
   follow section 3 ("If generation fails"); each is a void receipt in that one EXP-001 counter.
6. **Sealed data released to the Evaluator.** The Evaluator reads `sealed/<C_f>/`
   through `evaluator_env.sh --sealed` only. The release time is the
   `started_utc` of `seed_commitment.json` (the start of generation, the earliest
   moment sealed bytes exist), copied into the dataset manifest as `released_utc`; the
   evaluator refuses unless C_f's commit time and the manifest `frozen_at` are both
   before it.
7. **Frozen tree and overlap audit** (Auditor). First the orchestrator prepares the
   frozen tree in an ordinary shell. It is a clone checked out at C_f (not a
   `git archive` export), because the evaluator checks C_f against `origin/main`
   and the clean-tree state through git; the jail binds it read-only and nothing
   can be built inside it:

   ```
   F=~/aien-data/turing-cal/frozen/<C_f>
   git clone -q https://github.com/aien-dev/omega $F && git -C $F checkout -q --detach <C_f>
   mkdir -p $F/build/no-physics
   make -C $F PHYSICS_DIR=$F/build/no-physics crumbline-learner turing-exp001-a-build \
       turing-cal-overlap turing-coder turing-cal-eval turing-verify-indep
   sh $F/calibration/scripts/runtime_digest.sh      # must print the frozen runtime_digest
   ```

   These six make targets build every binary of the runtime listing except `crumbs` (runtime_digest.sh names the
   file of each): `crumbline-learner` is built here from C_f exactly as generate_sealed_data.sh builds it (an
   empty `PHYSICS_DIR`; the build is byte-reproducible, and runtime_digest.sh refuses unless it hashes to the
   pinned `LEARNER_SHA256`); `crumbs` is never built here: it is the prebuilt, pinned binary at
   `~/workspace/hive-worktrees/crumbs-v1/target/release/crumbs` (or `$TC_CRUMBS`), section 3, and
   runtime_digest.sh refuses unless it hashes to `CRUMBS_SHA256`. `build/` is ignored by git, so the clone stays
   clean. A failed build or test here, or a runtime_digest that differs, is a VOID of the stage "tests": record
   it with `calibration/scripts/record_void.sh --commit <C_f> --step 7 --code <BUILD|TEST|RUNTIME_DIGEST>
   --reason <text> --command <the exact command>` (FAILURE_REPORTING.md section 2).

   Then the audit runs in the jail:

   ```
   $F/calibration/scripts/evaluator_env.sh --frozen $F \
       --sealed ~/aien-data/turing-cal/sealed/<C_f> \
       --ro ~/aien-data/crumbline \
       --out ~/aien-data/turing-cal/eval/<C_f>/audit -- \
       $F/calibration/scripts/verify_holdout_separation.sh \
       --sealed-dir ~/aien-data/turing-cal/sealed/<C_f> \
       --out ~/aien-data/turing-cal/eval/<C_f>/audit/overlap_audit.json \
       --repo $F
   ```

   The same audit (with `git archive` and `--repo ~/workspace/omega/.git`, which
   works the same way for the audit) was run on 2026-09-29 against a throwaway
   freeze commit and the full default development set (54 trace files, 86 million
   records). It returned PASS in 3 min 44 s.

   The result is copied to `calibration/experiments/EXP-001/overlap_audit.json`. An
   audit that runs to the end and finds an overlap or a freeze-order break is an
   **S2 FAIL, final**: it is reported with the verdict FAIL (FAILURE_REPORTING.md
   section 2); the set is never quietly replaced. An audit that cannot run (exit 2)
   is an infrastructure failure and follows the void rule: record it with `record_void.sh --commit <C_f> --step 7
   --code AUDIT ...` as above.
8. **Evaluation.** Orchestrator, inside `evaluator_env.sh`, with run root
   `R=~/aien-data/turing-cal/eval/<C_f>/run` (layout: DATA_FORMAT.md section 4).
   First, in an ordinary shell (reads the sealed set only through the script's own
   hash checks, writes one file):

   ```
   mkdir -p $R && cp $F/calibration/experiments/EXP-001/candidate_manifest.json $R/
   $F/calibration/scripts/make_dataset_manifest.sh --sealed ~/aien-data/turing-cal/sealed/<C_f> $R/dataset_manifest.json
   cp ~/aien-data/turing-cal/eval/<C_f>/audit/overlap_audit.json $R/
   mkdir -p $R/docs/profiles && cp $F/calibration/profiles/Turing-profile-v1.0.toml $R/docs/profiles/
   cp $F/calibration/experiments/EXP-001/candidate_manifest.json $R/docs/
   ```

   Then the run, in the jail. The two memorization controls (76 MB and 45 MB, not in
   git) are bound read-only from `~/aien-data/turing-cal/candidates/`; they are the
   files hashed at freeze and are never regenerated at evaluation time (the
   evaluator re-checks every hash before scoring):

   ```
   $F/calibration/scripts/evaluator_env.sh --frozen $F \
       --sealed ~/aien-data/turing-cal/sealed/<C_f> \
       --ro ~/aien-data/turing-cal/candidates \
       --out $R -- sh -c "cd $R && $F/build/turing-exp001-eval/turing-cal-eval run --repo $F \
         --manifest candidate_manifest.json --cand-dir $F/calibration/experiments/EXP-001/candidates \
         --cand-dir ~/aien-data/turing-cal/candidates --dataset dataset_manifest.json \
         --overlap overlap_audit.json --out bundle --work work"
   ```

   Then the independent scorer, in the same jail and the same run root, reading only
   the published docs at C_f, the bundle and the sealed CTR1 files named in the
   dataset manifest (it never includes or links omega `src/`):

   ```
   ... -- sh -c "cd $R && $F/build/turing-verify-indep/indep-scorer --docs docs \
         --bundle-root . --cand-dir $F/calibration/experiments/EXP-001/candidates \
         --cand-dir ~/aien-data/turing-cal/candidates \
         --out bundle/scorer_independent.json --details indep_details.json"
   ```

   (`docs/` holds the profile and the candidate manifest from C_f, copied above.)
   Finally the gate, in the jail:

   ```
   ... -- sh -c "cd $R && $F/build/turing-exp001-eval/turing-cal-eval gate --bundle bundle \
         --independent bundle/scorer_independent.json"
   ```

   The bundle is `$R/bundle` = `~/aien-data/turing-cal/eval/<C_f>/run/bundle`, outside git. In sealed mode
   the evaluator refuses any other bundle location (OUT_PATH), because this directory also holds the one EXP-001
   void counter. The gate writes `final_receipt.json` there exactly once (exclusive create); nothing in the run
   writes into the git tree.

   The independent scorer is frozen at C_f (its source hash is in the manifest). It
   may not be revised after the sealed release: a revision is a new scorer version,
   and for this experiment S8 = FAIL. Group 2 (second-seed replication) is scored in
   the same run. The dev dry run (`make turing-exp001-eval-dry`) runs exactly this
   sequence on development seeds 7 and 6.
9. **Results published whether pass or fail**, together with every void receipt and
   every failed generation attempt. A later publication commit copies, byte for byte, the files listed in
   FAILURE_REPORTING.md section 6 from `$R` into `calibration/experiments/EXP-001/` (same relative paths) and
   writes `calibration/experiments/EXP-001/published_manifest.sha256` over them
   (format in FAILURE_REPORTING.md section 6). Files derived byte for byte from the
   sealed data stay outside git.

## 3. Sealed data: seeds, generator, layout

**Seed rule `turing.cal.sealed.v1`.** For group `g` in {1, 2} and index `j` in
0..N-1:

```
msg  = ASCII "turing.cal.sealed.v1|<commit>|<profile_digest>|g<g>|<j>"   (no newline;
       commit = 40 lowercase hex, digest = 64 lowercase hex, j decimal)
seed = first 16 hex chars of SHA-256(msg), top bit cleared, as a decimal integer
```

Anyone can recompute a seed with:
`printf '%s' 'turing.cal.sealed.v1|<commit>|<digest>|g1|0' | sha256sum`. Take the
first 16 hex characters, subtract 8 from the first one if it is 8 or more, and read
the result as a hexadecimal number. `generate_sealed_data.sh --derive-only --commit C
--profile-digest D --n N` prints the whole table. A seed that equals a burned
development seed (0-10 or 20260927), by EXP-001R also one of the six EXP-001 sealed seeds, or that repeats, stops the script. The chance
is about 2^-59.

**Generator (pinned).**

* `crumbs` = `~/workspace/hive-worktrees/crumbs-v1/target/release/crumbs`, SHA-256
  `72e396b15532aa93afb1f215521e04dca8470f779f3de35c10b04b07bff96638`. It is used
  prebuilt; nothing in this protocol builds Rust.
* The learner is part of the generator. The traces record the learner's own search,
  so a different learner build gives different `trace.ctr` bytes. The script builds
  `crumbline-learner` with `make crumbline-learner` from a clean `git archive` of
  the freeze commit. The result must hash to the pinned
  `8159bdff248efa67fa2cf75bb0b506bc359f4a2269139d03479753c0fc393416`, which is the
  build of omega `1b75aa8`. The same bytes come out whether it is built in a
  worktree or in a `/tmp` export.
* Command per seed, inside the generation jail:
  `crumbs experiment --learner <learner> --seed <seed> --out <group dir>/seed-<seed>`.
  `experiment` always runs both conditions: control first (library off, independent
  of learning), then learning. The Turing profile uses the control condition. The
  learning files are hashed into `manifest.json` and then deleted, unless
  `--keep-learning` is given.
* The generator appends to existing files, so every seed directory must be new. The
  script refuses otherwise.

**Determinism (measured 2026-09-29, lane C).** One trial seed
(9000000000000000001) was generated twice outside the jail and once inside it.
`trace.ctr`, `samples.cts`, `evaluations.jsonl` and `promotion.json` came out
byte-identical every time (trace.ctr SHA-256 `7e1509f5...`). `ledger.jsonl`,
`report.json` and `*-outcomes.json` differ from run to run, because they carry
time-based UUIDs and wall-clock times. `manifest.json` marks those three as
`"volatile": true`. Generation reads no bank, ledger or state file: its inputs
are the seed, the generator binary, the learner binary and the fixed experiment
design compiled into the generator. Seeds up to 2^64-1 are accepted. Cost: about
10 s of wall time per seed and 0.93 GB with both conditions, or 0.50 GB with
control only.

**Lineage note.** Regenerating development seed 1 with today's generator and
learner reproduces `final-20260927/rep10/seed-1` byte-for-byte. It does not
reproduce `exp-20260927-rep10/seed-1`: 16 of 2,172,776 records differ in
`result_class`. That set was produced with an earlier generator build and an
earlier learner (its ledger records a different learner digest). Sealed data
therefore follows the `final-20260927` lineage. The profile states which lineage
its development data comes from.

**Layout** of `sealed/<commit>/`: `group-<g>/seed-<S>/control/{trace.ctr,
samples.cts, evaluations.jsonl, ledger.jsonl, promotion.json}`,
`group-<g>/seed-<S>/{report.json, control-outcomes.json}`, `seed_commitment.json`
(rule, inputs, all seeds, SHA-256 of the seed list, generator and learner hashes,
times, manifest hash), `manifest.json` (every file with its SHA-256, bytes, record
count (`trace.ctr` bytes/247, `samples.cts` bytes/268, `.jsonl` lines), volatile
flag and kept flag), `generation.log`, and `COMPLETE`, which is written last and
holds the SHA-256 of `seed_commitment.json`. Files end mode 0400 and directories
0500. No sealed file may have a second hard link.

**If generation fails** part way (before any score exists, so it is an infrastructure
failure), the script itself renames the partial directory to
`sealed/<commit>.failed-<k>` (k = 1, 2, 3) with a `FAILED` note (reason, exact command,
time), writes the next void receipt (stage "generation") into the one EXP-001 void counter
`~/aien-data/turing-cal/eval/<commit>/run/bundle` (calibration/scripts/tc_void_lib.sh), and stops. It is kept
and published, never used. The retry must be the byte-identical command under the same C_f: the seeds are fixed by
C_f, so a retry produces the same data and gives no room to pick a better sample. Nothing may change between
attempts. The limit is three voids for all of EXP-001, generation, frozen-tree tests and evaluation counted
together: the void that is the third of the experiment writes `final_receipt.json` (INCONCLUSIVE, reason INFRA) in
that counter, generation also writes `sealed/<commit>.INCONCLUSIVE_INFRA`, and every later start is refused:
EXP-001 ends INCONCLUSIVE with reason INFRA (FAILURE_REPORTING.md section 2). A new freeze commit
is never used to get around a failed generation.

## 4. Overlap audit (`verify_holdout_separation.sh`)

It writes `overlap_audit.json`. PASS requires every gate:

| Gate | Check |
|---|---|
| complete | `COMPLETE` equals SHA-256 of `seed_commitment.json` |
| integrity | every kept file re-hashes to its `manifest.json` value |
| seeds | every seed re-derives from the rule; none is burned |
| after_freeze | every sealed file's modification time is later than the freeze commit's committer time, and the candidate manifest at that commit is `"status": "frozen"` (so the commit is C_f) |
| single_link | no sealed file has a second hard link |
| crumb_digest | no ledger `crumb_digest` appears in both development and sealed data |
| sealed_digest | no ledger `sealed_digest` (hidden held-out set) appears in both |
| trace_stream | no ledger `trace_stream_digest` appears in both, except degenerate stream digests: a digest repeated across different crumbs inside one ledger file belongs to a content-empty stream. The audit lists these; it does not hide them. |
| crumb_block | no crumb's CTR1 record-body block (SHA-256 over bytes 0..214 of each of its records; bytes 215..246 are the position-dependent chain digest) appears in both (`tools/turing_cal_overlap.c`). The tool checks only CTR1 framing (magic, version 1, event index rising within a crumb), so learning traces with a different origin field are read too; a malformed file stops the audit with exit 2. |

Reported but not gated, with their counts:
* `first_state_overlap`: different crumbs of small families can start from the same
  state. A trial seed against development seeds 1-7 showed 1 such crumb, whose block
  digest was different.
* `record_body_overlap`: single search events can be byte-identical across different
  crumbs. The trial run found 15 of 4.2 million.

Development data defaults to every `trace.ctr` under `~/aien-data/crumbline`, both
conditions and all runs, plus each one's sibling `ledger.jsonl`.

Self-checks, run 2026-09-29 on a throwaway freeze commit with N = 1: PASS against
development seeds 1-7. The same sealed set also got a planted overlap: one sealed
trace was added to the development list. That run FAILED on crumb_digest (188),
sealed_digest (188), trace_stream (186) and crumb_block (186), as it should. A second run used the full default development set (54 trace files, 86 million records) and the jailed Auditor command of section 2 step 7 (archive form): PASS in about 4 minutes.

### EXP-001 outcome and the EXP-001R amendment

EXP-001 used the table above unchanged. Its audit FAILED on `crumb_digest` (1 shared; every other gate passed) and
EXP-001 ended as a terminal FAIL of S2, published in `calibration/experiments/EXP-001/`. `crumb_digest` hashes only the
visible part of a crumb. The shared crumb was population Ambiguous (families that by design show too few observations
to determine the hidden answer) and its `sealed_digest`, the hidden held-out set, differed. Chance matches are
therefore expected for such crumbs.

EXP-001R (`EXP_ID=EXP-001R`, profile `Turing-profile-v1.1`, `calibration/preregistration/EXP-001R.md`) amends two
things, written before any EXP-001R data exist. With `EXP_ID` unset the scripts behave as for EXP-001. The C tools read the same variable: `turing-cal-eval` and `indep-scorer` (both `EXP_ID=EXP-001` by default, profile v1.0; `EXP_ID=EXP-001R`, profile v1.1) put it in every receipt (`experiment`, `run_id`), keep the bridge key name `EXP_001_COMPRESSION_BRIDGE` for both, and refuse an unknown id. The void counter is per experiment because it lives in the per-freeze-commit run directory. `make turing-exp001-eval-dry EXP_ID=EXP-001R` is the development dry run for the successor.

* **Amended G6.** A sealed `crumb_digest` shared with any development or burned `crumb_digest` FAILS, EXCEPT when the
  sealed record's population is Ambiguous AND its `sealed_digest` differs from the `sealed_digest` of every
  development or burned record carrying that `crumb_digest`. Exempt matches are counted and listed (crumb_digest,
  population, both sealed_digests) in `overlap_audit.json` under `g6_exempt_matches`; the G6 detail reads "N shared, M
  exempt (Ambiguous, different hidden set)". G7 (`sealed_digest`), G8 (`trace_stream`) and G9 (`crumb_block`) are
  unchanged for all populations. There is no numeric tolerance.
* **Burned data added.** The six EXP-001 sealed seeds are burned (`calibration/scripts/burned_seeds_exp001.txt`):
  `generate_sealed_data.sh` refuses them (`--is-burned SEED` prints the verdict) and gate G3 fails on them. The EXP-001
  sealed traces and ledgers (`~/aien-data/turing-cal/sealed/d3cba292b9282116d1e374db22344bca4d47717e`) join the
  development and burned comparison list (`--burned-dir DIR`, repeatable; EXP-001R adds this root by default,
  `TC_EXP001_SEALED` overrides the location) in addition to every `trace.ctr` under `~/aien-data/crumbline`, which is
  kept whole.

`make test-turing-cal-blinding` checks both on synthetic planted ledgers: an Ambiguous visible-only match with a
different `sealed_digest` passes with 1 exempt (against a development or a burned trace); the same match with an equal
`sealed_digest`, or with a non-Ambiguous population, fails; the EXP-001 strict rule still fails the Ambiguous match;
each burned seed is refused.

## 5. What the machine enforces, and its limit

**Mechanism.** `bubblewrap` (`/usr/bin/bwrap` 0.9.0, not setuid). AppArmor lets it
create user namespaces for an ordinary user; plain `unshare --user` is blocked on
this host. No sudo is needed. Every jail starts from an empty root and binds in
only the paths listed in section 1. The jail also gets its own user, pid, network,
IPC and UTS namespaces, cannot create further user namespaces
(`--disable-userns`), gets a fresh `/proc`, an empty `/tmp` and HOME, and dies
with its parent. A path that is not bound, such as the sealed root inside the
Candidate jail, does not exist there, and mounts made outside cannot be undone
from inside. The wrappers refuse any bind that equals, contains or sits inside a
hidden path (the sealed root and the evaluator root).

**Test** (`make test-turing-cal-blinding`, 17 checks, all PASS on 2026-09-29). The
test plants a random canary file in the real sealed root. The Candidate jail
cannot read it, cannot list the sealed root, cannot find it by name anywhere,
cannot reach it through `/proc/*/root`, cannot create a user namespace, cannot
mount, and has no network. It can read development data and write its own working
tree. The Candidate wrapper refuses to bind a parent of the sealed root. The
Evaluator jail can read the canary, cannot modify the frozen tree or the sealed
data, can write its output, has no network, and cannot see development data. The
Evaluator wrapper refuses an output directory inside the frozen tree.

**Limit.** Everything here runs as one Unix user (uid 1000). The kernel enforces
the jail on every process started through a wrapper. A process of the same user
started outside any wrapper can still open the sealed root, because file modes do
not separate a user from itself. Layer 1 (the data does not exist until the freeze
commit exists) and layer 3 (the audit) cover this for the candidates themselves,
since those are hashed before any seed can be computed. The uid upgrade below
closes the gap for everything else.

**Optional upgrade: a separate sealed user (needs sudo, which only Drake can
run).** Run once, in a terminal on the Spark, before step 4:

```
sudo useradd --system --no-create-home --shell /usr/sbin/nologin turing-sealed
sudo mkdir -p /home/drakestapleton/aien-data/turing-cal/sealed
sudo chown turing-sealed:turing-sealed /home/drakestapleton/aien-data/turing-cal/sealed
sudo chmod 0700 /home/drakestapleton/aien-data/turing-cal/sealed
```

Generation and evaluation would then run as that user, through
`sudo -u turing-sealed calibration/scripts/generate_sealed_data.sh ...` and
`sudo -u turing-sealed calibration/scripts/evaluator_env.sh ...`. Every process
belonging to `drakestapleton`, agents included, would get "permission denied" on
the sealed root. This should work but has not been tested. The `turing-sealed` user also needs read and execute access to the frozen export, the omega git directory under `~/workspace` and the pinned `crumbs` binary under `~/workspace/hive-worktrees`, so those parent directories must let it through. EXP-001 does
not depend on this upgrade; the profile records which arrangement was used.

## 6. Files

* `calibration/scripts/generate_sealed_data.sh`: seed derivation, freeze checks, jailed generation, manifest, commitment
* `calibration/scripts/candidate_env.sh`, `evaluator_env.sh`, `tc_jail_lib.sh`: the jails
* `calibration/scripts/verify_holdout_separation.sh` + `tools/turing_cal_overlap.c`: the overlap audit
* `calibration/scripts/test_blinding.sh`: the enforcement self-test
* `mk/turing_exp001_c.mk`: `make turing-cal-overlap`, `make test-turing-cal-blinding`
* `calibration/scripts/freeze_candidate.sh`: the candidate manifest (step 1; `--freeze` makes C_f's content)
* `calibration/scripts/freeze_receipt.sh`: step 4, the freeze receipt for C_f (TURING_PROFILE_V1_FROZEN)
* `calibration/scripts/indep_source_digest.sh`: the independent scorer source digest (turing.cal.indep_source.v1)
* `calibration/scripts/runtime_digest.sh`: the runtime digest over the seven binaries
* `tools/turing_verify_indep/` (`make turing-verify-indep`, `mk/turing_exp001_indep.mk`): the lane D independent scorer
* `calibration/docs/DATA_FORMAT.md`: the trace, dataset manifest, run root and bundle formats
