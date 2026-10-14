# EXP445: machine-first representation experiment, pre-registered protocol

Issue: aien-spark/omega#445 (stage 2 of the one-layer plan,
`~/workspace/omega-vision/OMEGA-ONE-LAYER-PLAN.md`). This file is committed BEFORE any generation of
the experiment. Every setting below is frozen by that commit; a change needs a new protocol revision
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

1. System message: `arms/common.md` + the arm document (+ `SCHEMA.md` for `structured`). User message:
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
5. Transport errors and HTTP 5xx are retried up to `retries=2` times with the same request; a request
   that still fails ends the generation with no candidate (`no_candidate`, a failure).
6. Every raw request and response is saved and hashed (`gen/<set>/<task>/SHA256SUMS`, run-level
   `SHA256SUMS`); `log.tsv` holds per generation: rounds, prompt tokens, completion tokens per round,
   finish reasons, wall seconds, check results, candidate sha256.

Order: task, then arm, then attempt. A run can be resumed only with identical settings (the driver
refuses otherwise); finished generations are never redone.

## 7. Sampling and cost

- Fixed sampling, identical in every arm: `temperature=0.7`, `top_p=0.8`,
  `seed = 445000 + 1000 x attempt + task index` (task index = 1-based position in the sorted task list; the same seed for the three arms of one task and
  attempt), `timeout_s=900` per request. Whether the served engine honours `seed` and `top_p` is
  UNVERIFIED; the values are sent and logged either way.
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
- **Bytes rule:** bytes "wins" over structured only if bytes attempt-1 success is at least
  structured's minus 5 points (3 of 60 tasks: equal success) AND bytes mean completion tokens per
  generation are lower than structured's (lower cost). Otherwise structured is the production path.
- **Otherwise:** improve the builders (`omega_emit`, readers, arm documents) and repeat the
  experiment once, under a revision of this protocol committed before the repeat. Improvements use
  only aggregate results and refusal classes; no held-out task content is opened by a builder.
- Text is the research control only; it never becomes the production source.

## 9. Configurations and scale

- **Config A:** `qwen3-30b-a3b-instruct-2507`, local server `http://127.0.0.1:18091/v1`.
- **Config B:** a second free or local model, named (model id and endpoint) in a revision of this file
  committed before its first generation. Paid models need Drake's approval with an estimated cost.
- Scale: 60 tasks x 3 arms x 3 attempts x 2 configurations = 1080 generations (plus repair rounds).
- Set ids: `<config>-<arm>-<attempt>`, e.g. `a-structured-1`. Scoring: `tools/exp445/score.sh
  <tasks.tar> <TASKS.sha256> <run dirs...>` on a clean tree.

## 10. Commands

```
make omega-emit build/compiler/oscc
tests/one_layer/acceptance/ola.sh tasks-check <tasks.tar> <TASKS.sha256>
tests/one_layer/acceptance/ola.sh tasks-public <tasks.tar> <TASKS.sha256> <pub>
tools/exp445/run.sh --base-url URL --model NAME --config A --tasks <pub> --out evidence/ONE-LAYER-2/generations/<run>
tools/exp445/score.sh <tasks.tar> <TASKS.sha256> evidence/ONE-LAYER-2/generations/<run> [...]
```

Dry-run tests of the driver (fake local endpoint, no model): `make test-exp445`.
