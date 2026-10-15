# One-layer stage 2: harness interface (v2, frozen 2026-10-14)

Contract between the sealed author (writes 60 held-out experiment tasks and at least 10 planted OSC
binary IR defects, sealed) and the harness (`tests/one_layer/acceptance/`, aien-spark/omega#445,
stage 2 of the one-layer plan). It EXTENDS v1 (`HARNESS_INTERFACE.md`, sha256 pinned in `RULES`):
v1 stays valid and unchanged, `ola.sh score` keeps its v1 meaning, and everything v2 does not
restate (file formats, member rules, sealing steps, sandbox, verdict reasons, receipt keys, no time
stamps, receipts never rewritten) is exactly as in v1. The harness builder never reads
`~/workspace/one-layer-sealed-445/` before scoring. If this file must change it gets a new version
line; v2 stays valid.

## 1. New candidate kind `oscir`

| kind | candidate file | requirement keys | what the harness does |
|---|---|---|---|
| `oscir` | `candidate.oscir`: OSC binary IR bytes (spec/one-layer-format.md), raw binary, not hex | the `kind osc` keys: `entry`, `params`, `ret` (v1 section 2) | `osc_ir_decode` (src/compiler/osc_ir.h), then exactly the kind `osc` path: entry lookup, signature check, back end, native run |

- Decoding happens inside the sandbox. Any decoder refusal is `REJECT decode_refused` with detail
  `class=<NAME>` (`osc_ir_refusal_name`, e.g. `class=TRUNCATED`). After a successful decode the
  reasons are the kind `osc` ones (`signature_mismatch`, `compile_refused` for a back-end refusal,
  `realize_refused`, `wrong_result`, traps, `timeout`, ...).
- Max 1 MiB, as every candidate (`oversize`).
- `hidden/inputs` and `hidden/expected` are as for kind `osc`.

## 2. Planted-defect corpus (kind `oscir` cases)

Same tarball layout, member rules and LABELS format as v1 section 1, with `candidate.oscir` added to
the allowed candidate files (exactly one candidate per case, matching `kind`). Cases of kinds `osc`
and `omegaprog` stay allowed. Counts are no longer fixed: the harness counts from LABELS.

- Tarball: `~/workspace/one-layer-sealed-445/corpus.tar`; top-level directory `corpus/`.
- Sealing: v1 section 3 with the stage-2 hash file:
  `cd ~/workspace/one-layer-sealed-445 && sha256sum corpus.tar > ~/workspace/omega-vision/stage2/SEALED.sha256.tmp && mv ~/workspace/omega-vision/stage2/SEALED.sha256.tmp ~/workspace/omega-vision/stage2/SEALED.sha256`
- Command: `tests/one_layer/acceptance/ola.sh score-v2 ~/workspace/one-layer-sealed-445/corpus.tar ~/workspace/omega-vision/stage2/SEALED.sha256`
- Evidence: `evidence/ONE-LAYER-2/runs/<run_id>/` (verdicts.txt, replay/verdicts.txt, probes.log,
  score.txt, meta.txt) and receipts under `evidence/ONE-LAYER-2/receipts/`, as v1 section 4.
- `score.txt`: `defects_rejected=<x>/<D> controls_accepted=<y>/<C> errors=<n>`, the v1 detail
  lines, and `oscir_defects=<d> oscir_controls=<c>`, then `GATE PASS|FAIL`.
  Gate: every DEFECT rejected, every CONTROL accepted, zero errors, at least 10 DEFECT cases and at
  least 5 CONTROL cases of kind `oscir`, every isolation probe blocked and logged, identical
  verdicts on two clean replays, clean tree.

## 3. Experiment task corpus (held-out tasks)

One tarball, `~/workspace/one-layer-sealed-445/tasks.tar` (plain POSIX tar, no compression), with
one top-level directory and NO candidates and NO LABELS:

```
tasks/
  cases/<task_id>/
    requirement               # public: kind task + the signature (below)
    prompt                    # public: the plain-English task text shown to every model
    hidden/inputs             # hidden inputs (v1 format, kind osc rules)
    hidden/expected           # independently computed expected results (v1 format)
    hidden/reference.osc      # the author's own OSC text solution; must ACCEPT (section 3.3)
```

- `task_id`: `[a-z0-9_-]{1,40}`, must not start with `dev` (reserved for the public dev tasks).
  60 cases in the held-out tarball. Ids must not reveal the expected behaviour's difficulty class.
- Member rules as v1 (regular files and directories only, no links, no absolute paths, no `..`,
  no member outside this layout: the whole tarball is refused otherwise).
- **requirement**: exactly the lines `kind task`, `entry <function_name>`, `params <k1> ... <kn>`,
  `ret <kind>` (kinds and limits as v1 kind `osc`; comments and blank lines allowed).
- **prompt**: bytes `\n` and 0x20..0x7E only, 1..4096 bytes. It must state everything a correct
  solution needs: the computation, the behaviour on every edge case that `hidden/expected` checks,
  and, for every `TRAP` line, the condition in words and the trap kind (overflow = 1, division by
  zero = 2, index out of bounds = 3, conversion out of range = 5, shift amount too large = 7). It
  must not name a representation (no OSC syntax, no IR, no hex) and must not list the hidden inputs.
  The entry name and the parameter and return kinds come from `requirement`; the prompt may repeat
  them. Write traps only that a natural solution produces from checked arithmetic or conversion in
  every representation (signed overflow, division by zero, out-of-range `as` conversion, shift);
  prefer `TRAP` (any code) unless the prompt names the code.
- **hidden/inputs**: at least 8 lines.
- **hidden/reference.osc**: OSC text (kind `osc` rules) whose verdict against this case is `ACCEPT ok`.

### 3.1 Sealing

As v1 section 3, with the task tarball and its own hash file, published LAST and atomically:
`cd ~/workspace/one-layer-sealed-445 && sha256sum tasks.tar > ~/workspace/omega-vision/stage2/TASKS.sha256.tmp && mv ~/workspace/omega-vision/stage2/TASKS.sha256.tmp ~/workspace/omega-vision/stage2/TASKS.sha256`
(content: `<64 hex>  tasks.tar`). Nothing in the sealed directory changes after this.

### 3.2 Public view for the driver

`tests/one_layer/acceptance/ola.sh tasks-public <tasks.tar> <TASKS.sha256> <outdir>` checks the
seal and the whole layout, then extracts ONLY `requirement` and `prompt` of every case into
`<outdir>/tasks/cases/<task_id>/`. This is all the generation driver and the models ever see.

### 3.3 Experiment mode (merge generated candidates, score without LABELS)

```
tests/one_layer/acceptance/ola.sh experiment <tasks.tar> <TASKS.sha256> <candidates_dir>
```

`<candidates_dir>/<set_id>/<task_id>/candidate.osc` or `.../candidate.oscir`: one directory per
candidate set (`set_id` `[a-z0-9_-]{1,40}`, e.g. `a-structured-1` = config A, arm structured,
attempt 1), at most one candidate file per task. Only regular files and directories; a `task_id`
that is not in the corpus, a second candidate file, or any other file refuses the whole run. A task
with no candidate file (directory absent or empty) gets `REJECT no_candidate`.

Refuses a dirty git tree. Steps: seal check; private extraction (never inside the repo, never
visible to a candidate); every task's `hidden/reference.osc` scored as set `references`; for each
set, each task's candidate merged into a private copy of the case (the requirement's `kind task`
takes the candidate's kind: `candidate.osc` = `osc`, `candidate.oscir` = `oscir`) and scored exactly
as a v1/v2 case; a second clean extraction and execution of every set (verdicts must be
byte-identical); the v1 isolation probes (the `read_labels` probe aims at a `hidden/reference.osc`).

Evidence: `evidence/ONE-LAYER-2/runs/<run_id>/` (never overwritten) with `meta.txt`, `probes.log`,
`score.txt`, `sets/<set_id>/verdicts.txt` and `sets/<set_id>/replay/verdicts.txt` (including
`references`). Verdict lines and receipts exactly as v1 section 4 (`kind` = `osc` or `oscir`), plus
the reason `no_candidate` (its receipt has `candidate_sha256=-`). Per-case result
is ACCEPT or REJECT (ERROR only for a malformed case, which also invalidates the run). No LABELS
exist and no gate is computed here: success statistics are the experiment protocol's job.

`score.txt`, one line per set in name order, then three lines:

```
set=<set_id> accepted=<a>/<n> rejected=<r> errors=<e> no_candidate=<k>
references_accepted=<x>/<n>
replay_identical=yes|no probes=PASS|FAIL clean_tree=yes|no
RUN VALID|INVALID
```

`RUN VALID` requires every reference accepted, zero errors in any set, identical replays, every
probe PASS and a clean tree.

### 3.4 Reference check before generation

`tests/one_layer/acceptance/ola.sh tasks-check <tasks.tar> <TASKS.sha256>` is experiment mode with
no candidate sets: only `references`, replay and probes. Run it once after sealing; a task whose
reference is not accepted is reported (by id and reason, nothing hidden is printed).

## 4. Public dev tasks

The experiment's public dev tasks live in the repo (`tests/one_layer/exp445/dev/`) in exactly the
section 3 layout with ids starting `dev`, are packed and sealed by a script there with a local hash
file, and run through the same commands. They are never held-out evidence.
