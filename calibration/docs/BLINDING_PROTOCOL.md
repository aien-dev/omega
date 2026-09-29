# Blinding protocol: Turing-profile-v1.0, EXP-001

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
| Evaluator | the frozen evaluation tree (a clean export of the freeze commit: candidate artifacts, reference coders, verifier; read-only), one sealed set (read-only) | one output directory | off, always | `calibration/scripts/evaluator_env.sh` |
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
2. **Candidate development.** Candidate side, on development data only (rep10 seeds
   1-7 for fitting; seeds 1-10 of `exp-20260927-rep10` and everything in
   `final-20260927` are burned development data). Any computation that touches data
   should run through `candidate_env.sh` so the sealed root is out of reach even
   after it exists.
3. **Candidate artifacts hashed and profile frozen.** `freeze_candidate` writes
   `calibration/experiments/EXP-001/candidate_manifest.json` (hashes of every
   candidate artifact). The profile `.toml` and its `.sha256` sidecar are
   committed in the same commit. That commit is merged to `origin/main`: it is the
   **freeze commit**. From here on no candidate artifact may change; any change
   means a new freeze commit and a new sealed set, and both sets get reported.
4. **Sealed data generated.** Orchestrator, ordinary shell (the script jails the
   generator itself):

   ```
   calibration/scripts/generate_sealed_data.sh --commit <freeze commit> \
       --profile-digest <contents of Turing-profile-v1.0.sha256>
   ```

   The script refuses unless all of these hold: the commit is an ancestor of
   `origin/main`, `candidate_manifest.json` and the profile exist at that commit,
   and the profile bytes at the commit hash to the given digest, which the sidecar
   also carries. It also refuses if the generator and learner hashes differ from
   the pins, or if `sealed/<commit>/` already exists. Details in section 3.
5. **Sealed data released to the Evaluator.** The Evaluator reads
   `sealed/<commit>/` through `evaluator_env.sh --sealed` only.
6. **Overlap audit** (Auditor):

   ```
   calibration/scripts/evaluator_env.sh --frozen <export of freeze commit> \
       --sealed ~/aien-data/turing-cal/sealed/<commit> --ro ~/aien-data/crumbline \
       --out <audit dir> -- calibration/scripts/verify_holdout_separation.sh \
       --sealed-dir ~/aien-data/turing-cal/sealed/<commit> --out <audit dir>/overlap_audit.json
   ```

   The result is copied to `calibration/experiments/EXP-001/overlap_audit.json`.
   A FAIL stops the experiment. The failure is reported (FAILURE_REPORTING.md);
   the set is never quietly replaced.
7. **Evaluation.** Evaluator, inside `evaluator_env.sh`: frozen candidates emit
   probability streams over group 1 (primary). Coders A and B encode and decode
   them, and the verifier and scorers run. Group 2 is the second-seed replication
   and is scored the same way.
8. **Results published whether pass or fail.**

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
development seed (0-10 or 20260927), or that repeats, stops the script. The chance
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

**If generation fails** part way, the directory lacks `COMPLETE` and must not be
used. Rename it to `sealed/<commit>.failed-<UTC time>`, keep it, report it, and run
the script again. The seeds are fixed by the commit, so a rerun produces the same
data and gives no room to pick a better sample.

## 4. Overlap audit (`verify_holdout_separation.sh`)

It writes `overlap_audit.json`. PASS requires every gate:

| Gate | Check |
|---|---|
| complete | `COMPLETE` equals SHA-256 of `seed_commitment.json` |
| integrity | every kept file re-hashes to its `manifest.json` value |
| seeds | every seed re-derives from the rule; none is burned |
| after_freeze | every sealed file's modification time is later than the freeze commit's committer time |
| single_link | no sealed file has a second hard link |
| crumb_digest | no ledger `crumb_digest` appears in both development and sealed data |
| sealed_digest | no ledger `sealed_digest` (hidden held-out set) appears in both |
| trace_stream | no ledger `trace_stream_digest` appears in both, except degenerate stream digests: a digest repeated across different crumbs inside one ledger file belongs to a content-empty stream. The audit lists these; it does not hide them. |
| crumb_block | no crumb's CTR1 record-body block (SHA-256 over bytes 0..214 of each of its records; bytes 215..246 are the position-dependent chain digest) appears in both (`tools/turing_cal_overlap.c`) |

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
sealed_digest (188), trace_stream (186) and crumb_block (186), as it should.

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
the sealed root. The scripts work unchanged under either arrangement. EXP-001 does
not depend on this upgrade; the profile records which arrangement was used.

## 6. Files

* `calibration/scripts/generate_sealed_data.sh`: seed derivation, freeze checks, jailed generation, manifest, commitment
* `calibration/scripts/candidate_env.sh`, `evaluator_env.sh`, `tc_jail_lib.sh`: the jails
* `calibration/scripts/verify_holdout_separation.sh` + `tools/turing_cal_overlap.c`: the overlap audit
* `calibration/scripts/test_blinding.sh`: the enforcement self-test
* `mk/turing_exp001_c.mk`: `make turing-cal-overlap`, `make test-turing-cal-blinding`
