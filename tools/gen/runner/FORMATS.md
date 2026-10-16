# GEN run row and decision receipt, format v1 (omega#477, GEN-2)

Status: FROZEN v1, 2026-10-15 (Agent C). A change to anything below is a new version with a new
schema string; readers refuse a schema they do not know. Producers: `tools/gen/runner/run.sh` (run
directory, run rows, prompt manifests) and `tools/gen/reducer/reduce.sh` (decision receipt).
Consumers: the reducer, GEN-6 operator evidence (#480), RSI (spark-rsi#57).
Retrieval goes only through the provider CLI `genret-cli-v1` (Agent E, `tools/gen/retrieval/CONTRACT.md`
at c0c55920, sha256 2689ffb7b5e8903f4e8f0a3200ffe7b2a890056a96c601610a346193c229bea2), called for C
(`--kind raw`) and D (`--kind abstraction`) with the same k, cap and mode; never for A and B.

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

A generation with no candidate leaves its task directory absent, so the harness gives
`REJECT no_candidate`. Attempt 1 is greedy (temperature 0). Attempts 2 and 3 exist only when the
G0 sampling gate passed; otherwise pass@3 is `UNAVAILABLE`.

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
cands/<set>/<task>/candidate.oscir
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
provider_contract=<genret-cli-v1|->              - for A and B (no provider call)
provider_contract_sha256=<sha|->                 sha256 of tools/gen/retrieval/CONTRACT.md that was pinned
provider_sha256=<sha|->  the provider program that ran
corpus_manifest_sha256=<sha|->                   genret --manifest-sha256 (the pinned record set)
kind=<raw|abstraction|->                         C = raw, D = abstraction
mode=<normal|irrelevant|empty|length-matched|->
k=<n|->
cap_bytes=<n|->
target_bytes=<n|->
exclude_family=<comma list|->
query_sha256=<sha|->     genret receipt query_sha256 (public requirement + prompt only)
retrieval_sha256=<sha|-> sha256 of the genret output SHA256SUMS file
retrieved=<list|->       rank:id:digest:role from genret selected.tsv joined by ","; - when none
```

Requests: `messages = [{system: prefix ++ suffix}, {user: user text}]`, built with
`reader.sh first_msgs` and `request_json`; round 2 is `reader.sh repair_msgs` of round 1.
Retrieved text appears only in the suffix, after the shared prefix. Nothing hidden (inputs,
expected values, references, grader output) is ever an input to a manifest.

## 4. Run row v1 (`log.tsv`)

One header line, then one row per generation (set = `<c>-<attempt>`), tab-separated, sorted by
nothing (append order = generation order: attempt, task, condition). Columns 1-12 are the EXP445
row (`reader.sh gen_row`, unchanged meaning); 13-31 are GEN columns.

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
| 27 | candidate_r1_sha256 | ROUND-1 candidate, `-` when none |
| 28 | manifest_sha256 | the prompt manifest used |
| 29 | retrieval_sha256 | as the manifest |
| 30 | retrieved | as the manifest |
| 31 | suffix_bytes | as the manifest |

Header (exact): the 31 column names above joined by TAB.

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
pin.provider_contract_sha256=  pin.corpus_manifest_sha256=  pin.retrieval_mode=  pin.k=  pin.cap_bytes=
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
tasks=<n>
arm.<X>.success_a1=<k>/<n>              X = A, B, C, D; harness ACCEPT of set <x>-1
arm.<X>.p_a1=  arm.<X>.wilson95_lo=  arm.<X>.wilson95_hi=
arm.<X>.checker_ok_a1=<k>/<n>           public checker OK in round 1 (never counted as success)
arm.<X>.success_a1_final=<k>/<n>        secondary: set <x>-1-final
arm.<X>.pass3=<k>/<n>|UNAVAILABLE       any of <x>-1, <x>-2, <x>-3 accepted
arm.<X>.prompt_tokens_a1=  arm.<X>.completion_tokens_a1=      round 1 of attempt 1, summed
arm.<X>.completion_tokens_per_correct_a1=  arm.<X>.tokens_per_correct_a1=     NA when 0 correct
arm.<X>.suffix_bytes_mean=
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
```

`receipt.txt` appends:

```
time.<X>.wall_s_a1=  time.<X>.wall_s_per_correct_a1=
input.run_sha256sums=  input.run_meta_sha256=  input.log_sha256=
input.harness_score_sha256=  input.harness_candidates_sha256=
input.harness_set.<set>=<sha256 of sets/<set>/verdicts.txt>   every set, name order
input.grading_receipt_sha256=<sha|->  input.pins_sha256=<sha|->
aggregate_sha256=<sha256 of aggregate.txt>
```

Verdict rule (plan, Fable review): INVALID when any validity check fails (incomplete paired matrix,
harness RUN INVALID, greedy gate not PASS, provenance or pin mismatch, sanity A - B > 2). PASS when
for every X in {A, B, C}: lift_ok and p_ok. FAIL when newcombe95_hi of D-C < 0.10. Otherwise
INCONCLUSIVE. When the verdict is INVALID the `arm.`, `pair.`, `paired.`, `fail.` and `check.` lines
are still written when the matrix is complete, and omitted otherwise.
