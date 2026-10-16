# GEN run row and decision receipt, format v1 (omega#477, GEN-2)

Status: FROZEN v1, 2026-10-15 (Agent C); revised 2026-10-16 for GEN_PROTOCOL_V1 at gen/protocol 7eb8534c
(protocol.json sha256 a28142baeeb3d7218ddd3baea6bcaf2ae48de5e6eb1975a38845c008737ee0b8) before any run existed. A change to anything below is a new version with a new
schema string; readers refuse a schema they do not know. Producers: `tools/gen/runner/run.sh` (run
directory, run rows, prompt manifests) and `tools/gen/reducer/reduce.sh` (decision receipt).
Consumers: the reducer, GEN-6 operator evidence (#480), RSI (spark-rsi#57).
Retrieval goes only through the provider CLI named by protocol.json `retrieval.contract` (Agent E,
`tools/gen/retrieval/CONTRACT.md`): `genret-cli-v3`, CONTRACT.md sha256
48006271d18394c40daf96120eaa20fbe3908d9a12b08cab7994ee02a6351e9d (gen/retrieval 4a48b613), pinned in
tools/gen/runner/PARAMS, called with `--recheck-sha256` for C (`--kind raw`) and D (`--kind abstraction`)
with the same k, cap and mode; never for A and B.

Provider receipts are checked against the call before any generation; the run fails closed (no log
rows, no generation, exit 2) when a receipt has `contract=` other than `genret-cli-v3` (e.g.
`cortex-interop-v1`), a `provider=` line other than `genret` (the Cortex adapter writes
`provider=cortex-interop`), `recheck_source_sha256=unverified` (the genret_anyrecheck test build, the
Cortex adapter) or any value other than the pinned recheck.c sha256
05cec285f7b7b2be68b1db1287661513e7e4715da44909065bdd0829daac02b6, or a `recheck_sha256` other than the
pinned recheck binary. The reducer repeats these checks (INVALID).

Before any request the runner requires `--open <OPEN>` (the custodian's seal-open receipt, 5 lines:
`gen-open 1`, `omega_commit <40 hex>`, `protocol_sha256 <sha of protocol.json>`, `tasks_sha256`,
`heldout_pub_sha256`), `gencheck run-ready <protocol.json> <root>` and
`gencheck open <protocol.json> <root> <OPEN>` to pass, and (non-test) protocol.json at the OPEN commit
to equal the pinned one. The reducer runs both gencheck commands again (`--open`, `--gencheck`) and
refuses (no receipt) unless they pass and the OPEN is the run's own copy.

All files are ASCII, `\n` line ends, no time stamps in anything hashed except the transport
records (`time_s=`) that EXP445 already keeps. sha256 values are 64 lowercase hex.

## 1. Conditions and candidate set names

Conditions `A B C D` (plan, decision 4). Every condition writes Omegese; the reader is the EXP445
omegese chain (`tools/exp445/reader.sh reader ... omegese`); masking is OFF in every condition.

Candidate sets handed to `tests/one_layer/acceptance/ola.sh experiment`, `c` = lowercase condition:

| set | content |
|---|---|
| `<c>-<attempt>` | the ROUND-1 candidate of that generation. `<c>-1` is the primary measure. |
| `<c>-<attempt>-final` | the final candidate after at most one public-check repair round (secondary only). Equal to the round-1 candidate when round 1 passed the public check. |

Submitted candidate of a round (GEN_PROTOCOL_V1 section 5 step 9): the reader's candidate when it made
one; else, when the round got a usable reply, the reply content itself (`jq -r` of
`choices[0].message.content`, trailing newlines dropped, as `rN.content`): an empty, refused or
non-program reply IS the candidate, is graded and fails. Only a round with no usable reply after three
byte-identical tries (transport error, HTTP status other than 200, body not JSON, no `choices` array,
`choices[0].message.content` missing or not a string) has no candidate: its task directory is absent and
the harness gives `REJECT no_candidate`. Any attempt-1 `no_candidate`, and any attempt-1 reply whose
`usage.aien_sampling.mode` is not `greedy`, makes the run INVALID: the runner stops sending at once.
One request is in flight per endpoint (an flock on the endpoint).

Attempt 1 is greedy (temperature 0). Attempts 2 and 3 exist only when the G0 sampling gate passed
(seeds = protocol.json `gates.g0.seed_values` s1..s8, the repeat = s1); otherwise pass@3 is
`UNAVAILABLE`. pass@3 = the union of `<c>-1`, `<c>-2`, `<c>-3` only (never `-final`), labelled
"pass@3 (1 greedy + 2 sampled)".

## 2. Run directory

```
meta.txt                 identity of the run, key=value; a resume refuses when any line would change
blobs/<sha256>           content-addressed prompt parts: system prefix, retrieved suffix, user text, query
manifests/<sha256>       prompt manifests (section 3); file name = sha256 of the file
manifests.tsv            condition<TAB>task<TAB>manifest_sha256, sorted
retrieval/<c>/<task>/    provider call: argv (one argument per line) and out/ = the genret output directory
                         (selected.tsv, excluded.tsv, suffix.txt, query.txt, receipt.txt, SHA256SUMS)
gate/greedy/             greedy gate: <task>/rN.* raw rounds (N = 1, 2) and verdict.txt
gate/g0/                 G0 sampling gate (EXP445 layout: rK.req.json, gate/, verdict.txt); absent when
                         attempt 2 was never reached
gen/<set>/<task>/        raw rounds exactly as EXP445: rN.req.json, rN.transport, rN.resp.json,
                         rN.err.K.json, done
cands/<set>/<task>/candidate.oscir   the submitted candidate (section 1)
protocol.json, OPEN      copies of the pinned protocol.json and the seal-open receipt
log.tsv                  run rows (section 4)
SHA256SUMS               every file above except SHA256SUMS itself and the run lock
```

## 3. Prompt manifest v1

One per condition x task, independent of attempt. Key=value lines, exactly this order:

```
schema=omega.gen.prompt_manifest.v1
condition=<A|B|C|D>
task=<task id>
prefix_sha256=<sha>      system prefix: arms/common.md + arms/omegese.md + worked examples
                         (A: the one fixed example; B, C, D: the same 14-example prefix, byte-identical)
suffix_sha256=<sha>      retrieved material = genret suffix.txt bytes; sha256 of zero bytes for A and B
suffix_bytes=<n>
system_sha256=<sha>      sha256(prefix bytes ++ suffix bytes) = the system message
user_sha256=<sha>        the user message: reader.sh user_msg(public requirement, public prompt)
provider_contract=<genret-cli-vN|->              - for A and B (no provider call)
provider_contract_sha256=<sha|->                 sha256 of tools/gen/retrieval/CONTRACT.md that was pinned
provider_sha256=<sha|->  the provider program that ran
corpus_manifest_sha256=<sha|->                   genret --manifest-sha256 (the pinned record set of this kind)
kind=<raw|abstraction|->                         C = raw, D = abstraction
mode=<normal|irrelevant|empty|length-matched|->
k=<n|->
cap_bytes=<n|->
target_bytes=<n|->        length-matched control: C's target = D's realised suffix_bytes of the task
exclude_family=<comma list|->                  the dev task's own famsig token in a cross-family control run, else -
query_sha256=<sha|->     genret receipt query_sha256 (public requirement + prompt only)
retrieval_sha256=<sha|-> sha256 of the genret output SHA256SUMS file
retrieved=<list|->       rank:id:digest:role from genret selected.tsv joined by ","; - when none
selected=<n|->          genret receipt selected (records in the suffix)
length_gap=<n|->         length-matched control (C only): genret receipt length_gap = suffix - target
```

Requests: `messages = [{system: prefix ++ suffix}, {user: user text}]`, built with
`reader.sh first_msgs` and `request_json`; round 2 is `reader.sh repair_msgs` of round 1.
Retrieved text appears only in the suffix, after the shared prefix. Nothing hidden (inputs,
expected values, references, grader output) is ever an input to a manifest.

## 4. Run row v1 (`log.tsv`)

One header line, then one row per generation (set = `<c>-<attempt>`), tab-separated, sorted by
nothing (append order = generation order: attempt, task, condition). Columns 1-12 are the EXP445
row (`reader.sh gen_row`, unchanged meaning); 13-32 are GEN columns.

| # | column | value |
|---|---|---|
| 1 | set | `<c>-<attempt>` |
| 2 | task | task id |
| 3 | rounds | 0, 1 or 2 rounds that got a response |
| 4 | prompt_tokens | round 1 + round 2, from usage |
| 5 | completion_tokens_r1 | from usage |
| 6 | completion_tokens_r2 | `-` when no round 2 |
| 7 | finish_r1 | finish_reason, `-` |
| 8 | finish_r2 | finish_reason, `-` |
| 9 | wall_s | sum of every transport try of both rounds |
| 10 | check_r1 | public check line of round 1 (`OK`, `REFUSED ...`, `REQUEST FAILED`) |
| 11 | check_r2 | same for round 2, `-` |
| 12 | candidate_sha256 | FINAL candidate, `-` when none |
| 13 | condition | `A`..`D` |
| 14 | attempt | 1..3 |
| 15 | seed | request seed |
| 16 | temperature | request temperature |
| 17 | top_p | request top_p |
| 18 | max_tokens | request max_tokens (per round) |
| 19 | prompt_tokens_r1 | from usage |
| 20 | prompt_tokens_r2 | `-` when no round 2 |
| 21 | tries_r1 | transport tries of round 1 (retries are transport, never new samples) |
| 22 | tries_r2 | `-` when no round 2 request |
| 23 | mode_r1 | `usage.aien_sampling.mode` as the server reported it; `-` absent; `BAD` malformed |
| 24 | mode_r2 | same for round 2 |
| 25 | seed_reported_r1 | `usage.aien_sampling.seed`; `-` absent |
| 26 | wall_s_r1 | transport time of round 1 |
| 27 | candidate_r1_sha256 | the submitted ROUND-1 candidate (section 1), `-` only when round 1 got no usable reply |
| 28 | manifest_sha256 | the prompt manifest used |
| 29 | retrieval_sha256 | as the manifest |
| 30 | retrieved | as the manifest |
| 31 | suffix_bytes | as the manifest |
| 32 | selected | as the manifest |

Header (exact): the 32 column names above joined by TAB.

## 5. Decision receipt v1

The reducer writes `aggregate.txt`, `receipt.txt` and `receipt.sha256` (`<sha>  receipt.txt`).
Key=value, one per line, keys unique, in the order below. Numbers: fixed point with 6 decimals;
counts `k/n`; `NA` when undefined (a rate over zero). Values never hold tabs or newlines.

`aggregate.txt` depends only on the verdicts, the run rows' tokens and the pins: an identical rerun
gives identical bytes. `receipt.txt` = `aggregate.txt` + the `time.` and `input.` lines + `aggregate_sha256`.

```
schema=omega.gen.decision.v1
verdict=PASS|FAIL|INCONCLUSIVE|INVALID
test_mode=no|yes                        yes = made with test knobs: TEST MODE, NOT EVIDENCE
reasons=<n>
reason.<i>=<text>                       i = 1..n: why this verdict (INVALID: every validity failure)
pin.revision=  pin.protocol_sha256=  pin.params_sha256=  pin.exp445_params_sha256=
pin.config=  pin.model=  pin.base_url=  pin.server=  pin.request_extra=  pin.weights_list_sha256=
pin.mask=off  pin.commit=  pin.source_sha256=  pin.tasks_public_sha256=  pin.tasks_corpus_sha256=
pin.prefix_sha256.A=  pin.prefix_sha256.B=  pin.examples_sha256=  pin.provider_sha256=
pin.provider_contract_sha256=  pin.corpus_manifest_sha256=<raw sha>,<abstraction sha>  pin.retrieval_mode=  pin.k=  pin.cap_bytes=
pin.manifests_sha256=                   sha256 of manifests.tsv
rule.primary=attempt1_round1_functional
rule.controls=A,B,C
rule.delta=0.100000
rule.alpha=0.050000
rule.fail_control=C
rule.ci=newcombe10
rule.sanity_ab_max=2
gate.greedy=PASS|FAIL
gate.g0=PASS|FAIL|NOT_RUN
pass3=AVAILABLE|UNAVAILABLE
pass3.label=pass@3 (1 greedy + 2 sampled)
tasks=<n>
no_candidate_a1.<X>=<count>            X = A, B, C, D; any count above 0 = INVALID
arm.<X>.success_a1=<k>/<n>              X = A, B, C, D; harness ACCEPT of set <x>-1
arm.<X>.p_a1=  arm.<X>.wilson95_lo=  arm.<X>.wilson95_hi=
arm.<X>.checker_ok_a1=<k>/<n>           public checker OK in round 1 (never counted as success)
arm.<X>.success_a1_final=<k>/<n>        secondary: set <x>-1-final
arm.<X>.pass3=<k>/<n>|UNAVAILABLE       any of <x>-1, <x>-2, <x>-3 accepted
arm.<X>.prompt_tokens_a1=  arm.<X>.completion_tokens_a1=      round 1 of attempt 1, summed
arm.<X>.completion_tokens_per_correct_a1=  arm.<X>.tokens_per_correct_a1=     NA when 0 correct
arm.<X>.suffix_bytes_mean=
arm.<X>.selected_mean=  arm.<X>.tasks_selected_below_k=<count>   X = C, D only
pair.D-<X>.both=  .d_only=  .x_only=  .neither=               X = A, B, C
pair.D-<X>.diff=                        (d_only - x_only) / n
pair.D-<X>.mcnemar_p_one_sided=         exact, H1: D better; P(Bin(d_only + x_only, 1/2) >= d_only)
pair.D-<X>.newcombe95_lo=  .newcombe95_hi=  .newcombe90_lo=  .newcombe90_hi=
pair.D-<X>.lift_ok=yes|no               100 (d_only - x_only) >= 10 n
pair.D-<X>.p_ok=yes|no                  p < 0.05
sanity.a_minus_b=<int>                  success_a1(A) - success_a1(B) in tasks
paired.<task>=<bits>                    attempt-1 round-1 success of A B C D, e.g. 0101
fail.<X>.<reason>=<count>               harness reasons of set <x>-1, sorted by reason
check.<X>.<stage>=<count>               public check outcome of round 1 (OK, compiler, decoder, ...)
lenmatch.task.<task>=target <t> suffix <s> gap <g> tol <bytes.cc> matched|unmatched   length-matched control only
lenmatch.matched=<m>/<n>  lenmatch.status=AVAILABLE|UNAVAILABLE  lenmatch.unusable=<text> (only when a record is unusable)
lenmatch.C.success_a1_matched=<k>/<m>  lenmatch.D.success_a1_matched=<k>/<m>   only when AVAILABLE
lenmatch.caveat=<text>                  dev-only secondary ablation on the matched subset; never decisive
```

`receipt.txt` appends:

```
time.<X>.wall_s_a1=  time.<X>.wall_s_per_correct_a1=
input.run_sha256sums=  input.run_meta_sha256=  input.log_sha256=
input.harness_score_sha256=  input.harness_candidates_sha256=
input.harness_set.<set>=<sha256 of sets/<set>/verdicts.txt>   every set, name order
input.grading_receipt_sha256=<sha|->  input.pins_sha256=<sha|->
outcomes_sha256=<sha256 of outcomes.tsv>
aggregate_sha256=<sha256 of aggregate.txt>
```

`outcomes.tsv` (written next to the receipt): header `task<TAB>A<TAB>B<TAB>C<TAB>D`, then one row per task
in task order. Each cell is exactly one class for attempt 1, round 1: `ACCEPT`, `REJECT:<harness reason>`,
`no_candidate` or `MISSING`.

Refusals (exit 2, no receipt, no verdict, not even INVALID): any `MISSING` cell (no harness verdict for a
task x condition of attempt 1); n != 60 distinct tasks in a decision run (control = none, not test mode);
protocol.json `splits.heldout.count` != 60; a duplicated cell (two log rows for one generation, or a
task graded twice in a `<c>-1` set); gencheck run-ready or open failing; an OPEN that is not the run's.

Verdict rule (GEN_PROTOCOL_V1 section 9, stats_vectors v2), in this order: INVALID when any validity check
fails (incomplete paired matrix, attempt-1 no_candidate or non-greedy reply, harness RUN INVALID, greedy
gate not PASS, provenance or pin mismatch, a provider receipt that is not genret-cli-v3 / genret / the
pinned recheck source, sanity A - B > 2); INCONCLUSIVE (floor) when the largest attempt-1 success count
of A..D is <= 3, INCONCLUSIVE (ceiling) when the smallest is >= 57; PASS when for every X in {A, B, C}:
lift_ok and p_ok; FAIL when the full-double newcombe95_hi of D-C < 0.10; otherwise INCONCLUSIVE. When
the verdict is INVALID the `arm.`, `pair.`, `paired.`, `fail.` and `check.` lines are still written when
the matrix is complete, and omitted otherwise. The `lenmatch.` lines never change the verdict or validity:
a task is matched iff |length_gap| <= max(256 B, 15% of D's realised bytes); under 50% matched = UNAVAILABLE.
