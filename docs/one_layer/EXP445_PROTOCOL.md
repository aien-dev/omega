# EXP445: machine-first representation experiment, pre-registered protocol

Issue: aien-spark/omega#445 (stage 2 of the one-layer plan,
`~/workspace/omega-vision/OMEGA-ONE-LAYER-PLAN.md`). This file is committed BEFORE any generation of
the experiment (revision r1), and revised (r2, section 12) before any held-out generation, and again (r3, section 13: Omegese against text, staged, own engines only). Every setting below is frozen by that commit; a change needs a new protocol revision
committed before the generation it affects, and every run records the commit it used (`meta.txt`).

## 1. Question and gate

Can a model write Omega programs as reliably in a machine-first form (raw binary IR, or a checked
op-list) as in human-readable OSC text? The gate, verbatim from #445:

> Gate: structured emission reaches 80% held-out functional success within ten points of text; raw bytes win only at equal success and lower cost; otherwise improve builders and repeat once. Text never becomes the production source.

How it is evaluated (section 8 gives the numbers): per model configuration, on the primary measure
(section 4), with point estimates; the Wilson intervals are reported beside every number.

## 2. Arms

Every arm gets the same shared rules (`tools/exp445/arms/common.md`: types, wrapping, traps, "one
fenced block"), the same task message and the same budget. They differ only in the form of the
program and the reader that turns the answer into a candidate.

| arm | the model writes | reader (candidate) | harness kind |
|---|---|---|---|
| `text` (control) | OSC text (`arms/text.md`: grammar and static rules from `docs/osc/OSC-1-DESIGN.md`) | the answer itself, compiled by `oscc` | `osc` |
| `bytes` | OSC binary IR spelled as hex (`arms/bytes.md`: the wire format of `spec/one-layer-format.md`) | `x445 hex`: strict, two hex digits per byte, only whitespace between bytes; then the strict decoder `osc_ir_decode` | `oscir` |
| `structured` | an op-list, JSON (`arms/structured.md` + `tools/omega_emit/SCHEMA.md`) | `omega_emit emit`: checks every op as read, refuses the first invalid one, emits canonical IR | `oscir` |

All three arm documents show the same worked example (`clamp_sub`); `make test-exp445` checks that the
three spellings produce byte-identical IR. "Constrained emission" here means the op-list is checked
op by op by `omega_emit` and refused with a reason; it is NOT decoding-time masking at the model
server (the served engine offers no grammar option that this protocol relies on).

Known differences inherent to the forms: the `bytes` and `structured` arms can write an explicit
`trap` op, the `text` arm cannot; the `text` arm's refusals come from the OSC compiler, the `bytes`
arm's from the hex reader or the decoder (a class name and its meaning), the `structured` arm's from
`omega_emit` (function, block, op and reason).

## 3. Tasks

- **Held-out (the only evidence):** the 60 sealed tasks in `~/workspace/one-layer-sealed-445/tasks.tar`,
  seal `~/workspace/omega-vision/stage2/TASKS.sha256`, written by a separate author against
  `tests/one_layer/acceptance/HARNESS_INTERFACE_V2.md` section 3. The builders of the tools never read
  them. The driver sees only `requirement` and `prompt` (`ola.sh tasks-public`).
- **Dev (never evidence):** the 10 public tasks in `tests/one_layer/exp445/dev/`, packed and sealed by
  `pack_dev.sh` in the same layout. Used to size the budget (section 5), develop the prompts, and
  smoke-test the driver. Their results are never pooled with held-out results.
- Before generation: `ola.sh tasks-check` on the held-out pack must report every reference accepted.

## 4. Success

- A **generation** is one task x arm x attempt: round 1, plus the repair round when round 1 is
  refused (section 6). Its **final candidate** is the last round's candidate.
- **Success** = the sealed harness (`ola.sh experiment`, kind `osc` or `oscir`) gives `ACCEPT` for the
  final candidate on the task's hidden inputs. Anything else (any REJECT reason, `no_candidate`) is a
  failure. Results count only from a harness run that ends `RUN VALID` (every reference accepted,
  zero errors, identical replay, all isolation probes PASS, clean tree).
- **Primary measure:** success of attempt 1, per configuration x arm, over the 60 held-out tasks.
- **Secondary:** success on any of the 3 attempts; generations accepted out of 180; accepted without
  needing the repair round; refusal and REJECT reasons by class.

## 5. Budget (equal in every arm)

- Per round, output cap `max_tokens = 3072` (`tools/exp445/PARAMS`), the same in every arm, task and
  attempt. A generation has at most two rounds, so at most 6144 output tokens.
- Sized on the dev references as the served model counts them (`tools/exp445/measure.sh`, config A
  server, 2026-10-14; prompt_tokens of the spelling minus the empty-message baseline of 8):

  | task | text | bytes (hex) | structured |
  |---|---|---|---|
  | dev01 | 64 | 281 | 309 |
  | dev02 | 60 | 256 | 286 |
  | dev03 | 81 | 567 | 624 |
  | dev04 | 82 | 441 | 507 |
  | dev05 | 27 | 118 | 91 |
  | dev06 | 27 | 112 | 91 |
  | dev07 | 120 | 594 | 750 |
  | dev08 | 103 | 807 | 926 |
  | dev09 | 75 | 488 | 547 |
  | dev10 | 22 | 98 | 79 |
  | total | 661 | 3762 | 4210 |

  The longest reference in any arm is 926 tokens (structured, dev08). 3072 is 3.3 times that: room
  for the fence, a short preamble, and a model spelling less compact than `omega_emit lift`. The cap
  is the same number for every arm; an arm that needs more tokens for the same program pays for it
  in cost (section 7) and in truncation risk (`finish_reason=length` is logged).
- The held-out tasks were written to the same contract as the dev tasks; their reference lengths
  are not measured before generation (the builders do not open the sealed tarball).

## 6. Generation procedure (`tools/exp445/run.sh`)

1. System message: `arms/common.md` + the arm document (+ `SCHEMA.md` for `structured`). Every request also carries the configuration's frozen request extra (section 9). User message:
   `Write the function described below.`, the requirement's `entry`, `params`, `ret` lines, the task
   prompt. Prompts are written to `prompts/<arm>.system.txt` and their sha256 to `meta.txt`.
2. One POST to `<base-url>/chat/completions` (OpenAI-compatible, `stream: false`).
3. The first fenced block is the payload (whole answer if none; an unclosed block runs to the end).
   The arm's reader makes the candidate and checks it with public data only: the reader itself, then
   entry name, parameter and return types, then the back end (`x445 check`). No hidden input is run.
4. **Exactly one repair round** if round 1 is refused, identical in every arm:
   the conversation plus the assistant answer, then the user message
   `Your program was refused: <reason>` + newline + `Reply with the corrected complete program in one fenced code block.`
   where `<reason>` is the reader's one-line refusal (`compiler: ...`, `hex: ...`, `decoder: CLASS (meaning)`,
   `step: ...`, `signature: ...`, `backend: ...`). The repair round's candidate is final, accepted
   by the check or not. No second repair.
5. A failed try (transport error, any HTTP status other than 200, or a 200 whose body has no usable
   answer) is retried up to `retries=2` times with the same request, after a pause of K x 5 seconds
   before try K; a request that still fails ends the generation with no candidate (`no_candidate`, a
   failure). (This sentence once said "transport errors and HTTP 5xx"; the driver has always retried
   every failed try, and every run on record retried that way. The pause was added on 2026-10-15;
   runs before that retried at once.)
6. Every raw request and response is saved and hashed (`gen/<set>/<task>/SHA256SUMS`, run-level
   `SHA256SUMS`); `log.tsv` holds per generation: rounds, prompt tokens, completion tokens per round,
   finish reasons, wall seconds, check results, candidate sha256.

Order: task, then arm, then attempt. A run can be resumed only with identical settings (the driver
refuses otherwise); finished generations are never redone. The commit may differ only when the tool
files (`TOOL_PATHS` in `tools/exp445/reader.sh`, the same set score.sh compares) are identical between
the run's commit and the resuming one and both trees are clean, as when the G0 ledger and the interim
scores are committed between attempt 1 and the `--futility` resume (added 2026-10-15; before that the
resume was refused at any other commit).

## 7. Sampling and cost

- Fixed sampling, identical in every arm: `temperature=0.7`, `top_p=0.8`,
  `seed = 445000 + 1000 x attempt + task index` (task index = 1-based position in the sorted task list; the same seed for the three arms of one task and
  attempt), `timeout_s=900` per request. `seed` is honoured by config B's server (verified, section 9) and
  UNVERIFIED for config A; whether `top_p` is honoured is UNVERIFIED for both. The values are sent
  and logged either way.
- **Cost** per configuration x arm: completion tokens (total and mean per generation, both rounds),
  prompt tokens per generation, wall seconds (sum of request times). The **hex penalty** is reported
  as the ratio of mean completion tokens per generation, `bytes` / `text` and `structured` / `text`
  (`score.sh`, `results.txt`), next to the reference ratios above (bytes 5.69 x text, structured
  6.37 x text over the dev totals).

## 8. Statistics and decision

- Proportions with Wilson 95% score intervals (z = 1.959964; `x445 wilson`).
- **Structured gate** (each configuration): PASS when structured attempt-1 success is at least
  48/60 (80%) AND text attempt-1 success minus structured attempt-1 success is at most 10 percentage
  points. The experiment's gate is PASS when it passes in both configurations.
- **Bytes rule** (orchestrator decision, r2): bytes "wins" over structured only if, in BOTH
  configurations, bytes attempt-1 successes are at least structured attempt-1 successes (equal
  success: no tolerance) AND bytes mean completion tokens per generation are lower than
  structured's (lower cost). Otherwise structured is the production path.
- **Otherwise:** improve the builders (`omega_emit`, readers, arm documents) and repeat the
  experiment once, under a revision of this protocol committed before the repeat. Improvements use
  only aggregate results and refusal classes; no held-out task content is opened by a builder.
- Text is the research control only; it never becomes the production source.

## 9. Configurations (FROZEN before any held-out generation) and scale

Both configurations are named here and in `tools/exp445/PARAMS` (`config.<X>.model`, `.base_url`,
`.extra`). `run.sh` refuses any configuration not named in PARAMS; `score.sh` refuses any run whose
model id, endpoint or request extra differs from the frozen values. No held-out generation may start
before this revision is committed. Paid models need Drake's approval with an estimated cost.

| | Config A | Config B |
|---|---|---|
| model | Qwen3-30B-A3B-Instruct-2507, FP8 | Qwen3-8B, bf16 |
| weights | `~/models/qwen3-30b-a3b-instruct-2507-fp8` | `~/models/qwen3-8b` |
| server | Omega's own `aien-serve-http` (`~/handoffs/rc3-bin-0826a25`, sha256 `c83fef926696562f63975082e5360818feb8acfe6df92353cb32c9d1ecdd4bde`), `--engine omega --spec on` | MAX 26.5.0, `max serve --model /home/drakestapleton/models/qwen3-8b --port 18097 --served-model-name qwen3-8b` |
| endpoint | `http://127.0.0.1:18091/v1` | `http://127.0.0.1:18097/v1` |
| model id | `qwen3-30b-a3b-instruct-2507` | `qwen3-8b` |
| request extra | none (`{}`) | `{"chat_template_kwargs":{"enable_thinking":false}}` |
| seed honoured | UNVERIFIED | VERIFIED by the orchestrator's probe on MAX: the same seed gave byte-identical answers twice at temperature 0.7 / top_p 0.8, a different seed a different answer |

The request extra is merged into every request of that configuration (so it is in the hashed
request files) and recorded in `meta.txt`. Without it Qwen3-8B writes a thinking block first (about
600 tokens before the answer in the orchestrator's probe); with it the model answers directly, as
config A's non-thinking instruct model does.

Weight files, sha256 (computed 2026-10-14):

```
002ebe91531ec67011b3d0c923234ef708c7da7d3b246045c4475d90b3d94628  qwen3-30b-a3b-instruct-2507-fp8/config.json
270f54aead2ae634567310218340069621fd8dfa2ad80001b69888ea5cc03477  qwen3-30b-a3b-instruct-2507-fp8/model.safetensors.index.json
fed0a80c29eac54690eb6a39499c80aae9df06d04189479bb74b649411a9b419  qwen3-30b-a3b-instruct-2507-fp8/model-00001-of-00004.safetensors
c72b1debd3bc4e55e1c6db6919e82d22de2c9d5cbd6d46f89cb1ba51b05bdc90  qwen3-30b-a3b-instruct-2507-fp8/model-00002-of-00004.safetensors
84a55173edab143768a98307114b71dc51245124f116a893f310141d43bc4302  qwen3-30b-a3b-instruct-2507-fp8/model-00003-of-00004.safetensors
169d89f49b646a0a8329d9a2570650f5f5a0eb78740cb1952b76caab2f21c09e  qwen3-30b-a3b-instruct-2507-fp8/model-00004-of-00004.safetensors
aeb13307a71acd8fe81861d94ad54ab689df773318809eed3cbe794b4492dae4  qwen3-30b-a3b-instruct-2507-fp8/tokenizer.json
a62ff0a2472a0fa1b8eaabcb57c59b58afa42a22831dc141400b6e0cf2b65ce3  qwen3-30b-a3b-instruct-2507-fp8/tokenizer_config.json
19d306dd769db12a9d710b44cf7f83b635efbe5166b84fb4358a08fb7d88bb53  qwen3-30b-a3b-instruct-2507-fp8/generation_config.json
f7c4eadfbbf522470667b797a3c89be2524832d2d599797248dc304fff447c30  qwen3-8b/config.json
f9fdbcb91c23971c13ec5d5f2573d2349e8f61f2f049371ec699281748fdb1bc  qwen3-8b/model.safetensors.index.json
31d6a825ae35f11fb85b195b4c42c146c051e446433125a215336abdf95cbf5f  qwen3-8b/model-00001-of-00005.safetensors
5991236cea6fe21f3d43cab0f0e84448734fbbe0789816202989f2ddc9d18282  qwen3-8b/model-00002-of-00005.safetensors
c5185c4794be2d8a9784d5753c9922db38df478ce11f9ed0b415b7304d896836  qwen3-8b/model-00003-of-00005.safetensors
b5ee7de71fbf17db3d5704e0c8f2bc7d005ca9e1d7ca2aeb19827b0cfcaa917a  qwen3-8b/model-00004-of-00005.safetensors
20c2d6366ab85c90786ccdd829cd2b9e7d30ef3b2ebbb998280e7e4014b542ff  qwen3-8b/model-00005-of-00005.safetensors
aeb13307a71acd8fe81861d94ad54ab689df773318809eed3cbe794b4492dae4  qwen3-8b/tokenizer.json
d5d09f07b48c3086c508b30d1c9114bd1189145b74e982a265350c923acd8101  qwen3-8b/tokenizer_config.json
2325da0f15bb848e018c5ae071b7943332e9f871d6b60e2ed22ca97d4cb993d2  qwen3-8b/generation_config.json
```

The operator re-checks these hashes (`cd ~/models && sha256sum -c`) before each configuration's
held-out run and records the result with the run. The weight hashes and server identity are not
checked by `run.sh` itself (it only sees the endpoint); that is the operator's step.

- Scale: 60 tasks x 3 arms x 3 attempts x 2 configurations = 1080 generations (plus repair rounds).
- Set ids: `<config>-<arm>-<attempt>`, e.g. `a-structured-1`.

## 10. Validity of a run (checked by `score.sh` before any statistic)

`score.sh` prints no success table and exits non-zero (RUN INVALID) unless all of these hold:

- every run: `protocol=PARAMS` (no command-line override), `params_sha256` equal to the PARAMS in
  force, the configuration named in PARAMS with its frozen model id, endpoint and request extra, arm
  prompts equal to what the current arm documents give, the public view of the scored tarball, a
  clean generation commit whose `tools/`, `tests/one_layer/`, `docs/one_layer/`, `src/compiler/` and `src/sha256.c` (linked into the readers), `mk/omega-emit.mk` and `Makefile` equal the
  scoring commit's;
- every run's `SHA256SUMS` verifies and lists exactly its saved requests, responses and prompts;
- every generation's candidate, re-derived from its hashed final response with the same reader
  (`tools/exp445/reader.sh`), matches `log.tsv` and the file in `cands/`;
- the matrix is complete: every task of the tarball x every arm x attempts 1..3 x every scored
  configuration, exactly once (held-out tarball: exactly 60 tasks);
- the harness run (`ola.sh experiment`) ends `RUN VALID`.

The gate (section 8) is evaluated only when both configurations are scored together.

## 11. Commands

```
make omega-emit build/compiler/oscc
tests/one_layer/acceptance/ola.sh tasks-check <tasks.tar> <TASKS.sha256>
tests/one_layer/acceptance/ola.sh tasks-public <tasks.tar> <TASKS.sha256> <pub>
tools/exp445/run.sh --config A --tasks <pub> --out evidence/ONE-LAYER-2/generations/<run-a>
tools/exp445/run.sh --config B --tasks <pub> --out evidence/ONE-LAYER-2/generations/<run-b>
tools/exp445/score.sh <tasks.tar> <TASKS.sha256> evidence/ONE-LAYER-2/generations/<run-a> evidence/ONE-LAYER-2/generations/<run-b>
```

Revision r3 (section 13), per configuration and run directory:

```
tools/exp445/check_weights.sh A <run>
tools/exp445/g0.sh --config A --out <run>                      # G0 (13.4); run.sh refuses without a PASS
tools/exp445/run.sh --config A --tasks <pub> --out <run> [--prompt-mode many|one]   # Stage 0: both modes
# Stage 1 (held-out): run.sh stops after attempt 1; then
tools/exp445/score.sh --interim --configs A <tasks.tar> <TASKS.sha256> <run>        # writes futility.txt
tools/exp445/run.sh --config A --tasks <pub> --out <run> --futility <score dir>/futility.txt
tools/exp445/score.sh --configs A <tasks.tar> <TASKS.sha256> <run>
```

Dry-run tests of the driver and the validity check (fake local endpoint, no model): `make test-exp445`.

## 12. Revisions

- **r1** (commit d23d9c3c): first registration.
- **r2** (this revision, before any held-out generation), after the independent review of PR #452:
  both configurations frozen (section 9) with the per-configuration request extra; the bytes rule
  set by the orchestrator (section 8: at least as many successes as structured, no tolerance, and
  fewer tokens, in both configurations); the validity check (section 10). `omega_emit` now accepts
  moves between array registers that the decoder accepts (`mov` into `own:`, `mut:` or `shared:`,
  as for call arguments), so the `mov` row of `tools/omega_emit/SCHEMA.md`, which is part of the
  structured arm's system prompt, changed; the structured prompt's sha256 changes with it. The
  config A dev smoke of r1 used the old prompt and is not comparable; no held-out generation used it.
- **r3** (2026-10-15, before any r3 generation): section 13.

## 13. Revision r3 (2026-10-15): Omegese against text, staged, on our own engines

r3 is committed before any r3 generation. Where it differs from sections 1 to 11, r3 wins; anything
r3 does not mention stays as written above. No held-out generation was scored under r1 or r2.

### 13.1 Why the revision

- Drake's decisions of 2026-10-15: compare only the machine-first language against plain text; drop
  the JSON op list (`structured`) and hex (`bytes`) arms; run every configuration on our own engines,
  never on a third-party engine.
- The 302 config B generations made on MAX under r2 (`~/workspace/exp445-aside-20261014/generations/heldout-b`,
  record `~/handoffs/exp445-b-resume.txt`) are kept as they are and are NOT evidence of r3. They are
  never pooled, scored or quoted as a result.
- On the dev tasks, the op list cost 6.37 times the tokens of text (section 7) and passed 0 of 10
  (OMEGESE.md section 7); Omegese is the machine-first form now under test (`docs/one_layer/OMEGESE.md`).
- The research note `aien-architecture/docs/research/omegese-and-gb10-decode.md` (merged 2026-10-15)
  shows that 10-task, one-attempt trials cannot separate two arms (3 of 10 has a Wilson 95% interval
  of about 11% to 60%), and that the model's prompt should carry many worked examples, not one.

### 13.2 Arms and prompts

| arm | the model writes | reader (candidate) | harness kind |
|---|---|---|---|
| `text` (control) | OSC text | the answer, compiled by `oscc` | `osc` |
| `omegese` | Omegese (`docs/one_layer/OMEGESE.md`) | `omegese emit`, then `omega_emit emit`, then `x445 check` (`tools/exp445/reader.sh`) | `oscir` |

- Both arms get `arms/common.md`, their arm document, and the SAME set of 12 to 16 worked examples,
  each written in both forms (the program in the arm's own form; the task text identical). Every
  example is verified in both forms by `make test-exp445`: the reader accepts it, the back end
  accepts it, and its behaviour tests pass. The two forms of an example must compute the same
  function on the example's tests.
- Example tasks are dev-style tasks written by the builders. None may share an entry name with a
  held-out task; `make test-exp445` checks this mechanically against the public view of the sealed
  pack and prints only a count, so no builder reads a held-out task.
  Disclosure (r3, 2026-10-15): the first example set shared 3 entry names with held-out tasks; the
  builders saw those 3 names (no prompt text, requirement or test) and replaced the examples. No other
  held-out information was seen.
- One-shot prompts (the r2 single example) are used only in Stage 0 (13.4), on dev tasks.
- Decode-time masking is OFF in r3. A masked arm needs its own revision once our engine applies the
  `omegese next` mask.

### 13.3 Configurations

| | Config A | Config B |
|---|---|---|
| model | Qwen3-30B-A3B-Instruct-2507, FP8 (weights and hashes as section 9) | Qwen3-8B, bf16 (weights and hashes as section 9) |
| server | Omega's own `aien-serve-http`, `--engine omega`, on :18091. Frozen in `tools/exp445/PARAMS` by the commit that opens Stage 0 (2026-10-15), for Stage 0 and Stage 1: `/home/drakestapleton/handoffs/pin-bin-f881710a/aien-serve-http`, sha256 `663b85a506ec9bdffd09094cb2271d95efb46eeedfbfe017d31c2fe520c4b911`, built from aien-sovereign-core f881710a (Stage S merged: every reply reports mode, seed and p_repeat), omega 4799f9eb, `libomega_gpu.a` 0629ad9c | our own dense 8B engine (OWN-8B); frozen by a later revision once its requalification passes. Until then config B cannot run. |
| request extra | none | frozen with the server |

The experiment gate (13.6) is evaluated only when both configurations are scored. Config A alone
is reported as an interim result, labelled interim.

### 13.4 Stages and pre-registered stop rules

**G0, the sampling gate (before every stage, per configuration).** G0 runs the Stage S sampling gate
(aien-sovereign-core `scripts/sampling-gate.sh`, merged f881710a; in Stage S its ignore-seed and
force-greedy mutant builds were shown to FAIL it) against the frozen server, with its repro requests
set to dev01 in the omegese arm at seeds s1..s8, plus one exp445 check: a repeat of s1. PASS when the
sampling gate passes, the repeat is byte-identical, and every reply reports mode=sampled with its own
seed. Identical replies across seeds pass only when the server's reported p_repeat makes them
plausible (p_repeat^(k-1) >= 0.01, as in the Stage S gate). The distinct-answer count is recorded,
not gated. The script and its output are saved in the run directory before any generation. On FAIL
nothing in that configuration runs.

**Stage 0, pilot on the 10 dev tasks (never evidence).** Config A, both arms, one-shot and
many-shot prompts, 3 attempts each: 10 x 2 x 2 x 3 = 120 generations. GO to Stage 1 when all of:
1. tooling is clean: every generation is logged, every candidate re-derives (section 10), the dev
   harness run ends `RUN VALID`;
2. many-shot `omegese` attempt 1 produces a candidate the back end accepts on at least 7 of the 10
   tasks;
3. many-shot does not do worse than one-shot in either arm on attempt-1 success, by more than
   one task.
If (1) fails, fix the tooling and rerun Stage 0. If (2) or (3) fails, builders may change the
language, reader or examples using dev results only, then rerun Stage 0, at most twice. After a
third Stage 0 that does not GO, stop and report to Drake; no held-out generation happens.

**Stage 1, held-out (the evidence).** Config A, the 60 sealed tasks, both arms, many-shot prompts,
3 attempts: 60 x 2 x 3 = 360 generations. Order: attempt, then task, then arm, so attempt 1 of every
task in both arms finishes first. Stop rule: after attempt 1, if `omegese` attempt-1 success is at
most 30 of 60, attempts 2 and 3 are not run (the build needs work; more samples would not change the
decision), and the result is reported with the reason. Config B repeats Stage 1 once it can run.

**Stage 2 (later, its own revision):** the masked arm, and the repair loop studied separately.

### 13.5 Unchanged rules, stated for r3

- Repair: exactly one repair round when round 1 is refused, as section 6 step 4. The primary
  measure counts the final candidate; round-1-only success is reported beside it.
- Budget, sampling values, seeds, transport retries, hashing and validity: sections 5, 7 and 10,
  with the arms of 13.2.
- Attempts 2 and 3 may repeat attempt 1 when the model is confident; this is measured (distinct
  candidates per task are reported), not a fault.
- `top_p` and the seed on config A are no longer UNVERIFIED once G0 passes; G0's output is the record.

### 13.6 Decision

- **Primary measure:** attempt-1 success per configuration and arm over the 60 held-out tasks, with
  Wilson 95% intervals.
- **Gate (each configuration):** PASS when `omegese` attempt-1 success is at least 48 of 60 (80%)
  AND `text` attempt-1 success minus `omegese` attempt-1 success is at most 10 percentage points.
  The experiment passes when both configurations pass.
- **Cost, reported beside the gate:** mean completion tokens per generation, `omegese` / `text`,
  and prompt tokens per generation (many-shot prompts are long; the engine reuses the shared
  prefix).
- **Paired view:** for each task, attempt-1 success in both arms; the counts of tasks where only
  `omegese` succeeds and where only `text` succeeds, with an exact two-sided McNemar test.
- **If the gate fails:** improve the language, reader and examples using aggregate results and
  refusal classes only, and repeat Stage 1 once under a new revision.
