# GEN: compositional generalization from verified abstractions, pre-registered protocol (v1)

Issue: aien-spark/omega#476 (GEN-1), program aien-spark/omega#475. This file is committed BEFORE any GEN
generation and before the custodian authors any held-out task. Every setting below is frozen by its
commit; the machine-checked copy is `tests/one_layer/generalization/protocol.json`, validated by
`tools/gen/protocol/gencheck` (`make test-gen-protocol`). A change follows section 15. GEN is a separate
experiment from EXP445 (`docs/one_layer/EXP445_PROTOCOL.md`): it reuses EXP445's tools by calling them,
never edits EXP445's protocol, PARAMS, seals or data, and never opens EXP445's sealed tasks.

## 1. Question

Does retrieving **checker-verified reusable abstractions** (helper functions extracted from earlier
verified solutions) help a model write correct Omegese programs for **unfamiliar combinations of known
primitives**, compared with one example, a fixed set of many examples, and retrieval of earlier raw
solutions under the same budget?

A positive answer is claimed only when the abstraction condition beats every control, including raw
retrieval, on held-out task families that no example, dev task or retrieved record shares (section 9).
FAIL and INCONCLUSIVE are legitimate outcomes and are reported as such.

## 2. Tasks and splits

Three splits, each made of composition **families** defined by the family contract
`tests/one_layer/generalization/families/FAMSIG_V1.md` (frozen first; tool `tools/gen/protocol/famsig`).
A family is the primitive multiset plus nesting shape the task SPECIFICATION asks for, over the
primitives `arith branch loop array call nested`; it is declared from the prompt, never from a solution.
Renaming identifiers or changing constants never makes a new family.

| split | count | author | visibility | role |
|---|---|---|---|---|
| dev | 10: `devgen01`..`devgen10` | protocol author (GEN-1) | public: `tests/one_layer/generalization/dev/` | prompt and tool development, gates, negative controls; never evidence |
| corpus (exemplars) | about 48 | abstraction builder (GEN-3) | public: `tests/one_layer/generalization/corpus/` | the B prefix (section 3) and the only source of retrieved records |
| held-out | 60, in at least 6 families | custodian (Unix user `gencust`), who builds no corpus or tool | sealed | the only evidence |

- Disjointness (FAMSIG_V1 section 4, checked mechanically by `famsig rule`): corpus, dev and held-out
  families are pairwise disjoint by signature (R3); every primitive a held-out task uses is used by some
  corpus task (R4); every held-out family combines at least two distinct primitives (R5); coarse
  signatures (duplicate siblings collapsed, `nested` read as `arith`, a top-level `seq` wrapper around one
  container plus leaves dropped) are pairwise disjoint across the three splits (R6); every held-out family
  has at least one coarse composition edge that no corpus shape has, `novel_edges >= 1` (R7).
- Held-out family metadata lives only in the sealed `SPLIT` file (FAMSIG_V1 section 6), never inside
  `tasks.tar`. Before any held-out generation the custodian publishes `HELDOUT.pub` (FAMSIG_V1 section 7):
  task ids, the salted family-list digest, the split digest, counts. The independent reviewer verifies
  SPLIT against HELDOUT.pub and R1..R7 after the seal (`gencheck split`) and audits semantic overlap with
  the corpus using `famsig rule --show-heldout` (`novel_edges` per family). The reviewer who runs any
  `--show-heldout` or `--show` command is never a corpus, record or tool builder.
- **Blind re-declaration (before the seal is opened):** the reviewer reads every held-out prompt and
  requirement read-only, as `gencust`, without seeing SPLIT, and declares each task's metadata line in
  its own `gen-split 1` file (own salt). `famsig compare SPLIT REDECLARED` (as `gencust`; prints only
  `tasks=<n> mismatches=<k>`) must give `mismatches=0`. Any mismatch forces the custodian to fix the
  declaration or the task and re-seal (new SPLIT, HELDOUT.pub and pack digests, R1..R7 again) before the
  seal is opened. The compare line is published with the run meta.
- **Near-duplicate check (before the seal is opened):** `tools/gen/protocol/genjac scan` (as `gencust`)
  of every held-out prompt against every corpus prompt and every corpus reference solution: word
  3-gram Jaccard (genret tokenization: lowercase `[a-z0-9]+` words, set of consecutive word triples);
  the number of held-out prompts with any Jaccard strictly above 0.5 (`above=`) must be 0, compared
  exactly. Only the summary line (`heldout= corpus= threshold= max_jaccard= above=`) is published. A
  non-zero count forces a rewrite and re-seal as above.
- Task layout: exactly `tests/one_layer/acceptance/HARNESS_INTERFACE_V2.md` section 3 (requirement,
  prompt, `hidden/inputs`, `hidden/expected`, `hidden/reference.osc`), reused unchanged. The prompt
  states every trap kind (HARNESS_INTERFACE_V2 section 3), so a held-out trap line is `TRAP <code>` with
  that code whenever the code is unambiguous (a bare `TRAP` accepts any code). Held-out ids
  must not start with `dev`, must not reveal family or difficulty. Expected values are computed
  independently of the reference (an oracle written from the prompt, as
  `tests/one_layer/generalization/dev/gen_expected.c`), and every reference must be ACCEPTed by
  `ola.sh tasks-check` (RUN VALID) before the seal is published. The custodian publishes in the run meta
  `oracle_match=<k>/60` (tasks whose `hidden/expected` equals the oracle's output byte for byte; must be
  60/60) and the sha256 of the oracle source file.
- Every held-out `hidden/reference.osc` carries one comment line `// canary <32 lowercase hex>` with a
  fresh random value per task, used by the source-leak probe (section 8).
- Dev fixtures: packed and sealed by `tests/one_layer/generalization/dev/pack_dev.sh`; `ola.sh tasks-check`
  on that pack gives references_accepted=10/10, RUN VALID (dev pack sha256 in protocol.json).
- Entry names: no corpus task may share an entry name with a held-out task; checked by the custodian
  against the public view, printing only the count (as EXP445 13.2).

## 3. Conditions

Every condition writes Omegese (`docs/one_layer/OMEGESE.md`); the reader is the EXP445 omegese chain
(`tools/exp445/reader.sh`: `omegese emit`, `omega_emit emit`, `x445 check`); harness kind `oscir`.
Decode-time masking is OFF in every condition (the mask is not qualified); a masked sub-experiment needs
its own revision and is never pooled with these results.

| condition | system message = prefix ++ suffix |
|---|---|
| A (1-shot) | prefix: `arms/common.md` + `arms/omegese.md` + 1 fixed example; suffix empty |
| B (many-shot) | prefix: the same two documents + 14 fixed corpus examples; suffix empty |
| C (B + raw retrieval) | B's prefix byte for byte + suffix: top-k verified raw corpus solutions |
| D (B + abstraction retrieval) | B's prefix byte for byte + suffix: top-k verified abstraction records |

- **B prefix pick (deterministic, diverse):** order the corpus tasks by family signature (byte order),
  then task id; walk the families round robin in signature order, taking each family's lowest unused
  task id, until 14 tasks are taken. Every task line of corpus META is eligible, GEN-3 transfer-probe tasks
  included (probes are withheld only from abstraction extraction). A's single example is the first task
  taken. The 14 ids (`record_sets.prefix_ids`, in pick order) and `record_sets.prefix_sha256` = sha256 of
  `tests/one_layer/generalization/records/PREFIX` (those ids, one per line) are pinned in protocol.json when
  the corpus is frozen. If the prefix reuses EXP445
  examples, they are corpus tasks with corpus META lines and must pass `famsig rule`.
- **Retrieval (C and D):** the file-backed provider `genret` (`tools/gen/retrieval/CONTRACT.md`,
  contract `genret-cli-v3`, pinned in protocol.json `interfaces` at commit `4a48b613` with CONTRACT.md
  sha256 `48006271d18394c40daf96120eaa20fbe3908d9a12b08cab7994ee02a6351e9d`; it validates every record
  with the GEN-3 `recheck`), the same `--k`, `--cap-bytes`, mode `normal` and lexical ranker for C and D;
  only `--kind raw` (C) versus `--kind abstraction` (D) and the record manifest differ. k = 3,
  cap_bytes = 4096. **Query** = the task's public `requirement` and `prompt` files passed as
  `--requirement` and `--prompt`: genret's query text is the `entry`, `params` and `ret` values plus the
  whole prompt, the same bytes `tools/exp445/reader.sh` `user_msg` shows the model (CONTRACT.md section
  3). Retrieved text appears only in the suffix, after the shared prefix. A and B never call the provider.
- **Candidate pool:** C's raw-record manifest holds the verified corpus solutions EXCEPT the 14 prefix
  tasks (already in every B, C, D prompt; retrieving them would waste C's budget). D's manifest holds the
  abstraction records produced by the deterministic GEN-3 tool from the corpus solutions, EXCEPT every
  abstraction whose ancestry lies entirely within the 14 prefix tasks (the same rule as C: nothing the
  prefix already shows). Both are filtered views of the GEN-3 `records/MANIFEST`, written in the genret
  manifest format inside `records/`. Their paths (`record_sets.raw_manifest`,
  `record_sets.abstraction_manifest`) and sha256 (`record_sets.raw_manifest_sha256`,
  `record_sets.abstraction_manifest_sha256`) are frozen in protocol.json and re-hashed by `gencheck`
  BEFORE the seal is opened for the run; no record is added, removed or edited afterwards. Every genret
  receipt's `manifest_sha256` must equal the frozen digest of its condition.
- **Shared record skeleton:** C and D records follow one schema (GEN-3
  `tools/gen/abstract/RECORD_SCHEMA.md`, `gen-record 1`). An abstraction record is a callable Omegese
  helper with a one-line pre/post, one example call, a test receipt, ancestry of at least 2 distinct
  corpus solutions, and an IR fingerprint different from every corpus solution. No pattern templates.
- **What the model sees (rendering, genret-cli-v3, approved here):** the C and D renderings are
  byte-identical except for record content. Both start with the one neutral heading, the same 66 bytes
  for C and D: `"\nVerified reference code from earlier tasks, most relevant first.\n"` (approved
  verbatim). Then per record `Record <i>.`, the `entry`, `params` and `ret` lines (the signature), the
  one-line description, and the body in a fenced block. Only those record fields differ between C and D
  (a whole raw solution versus a factored helper `fn`). The omission of D's pre/post, example call, test
  receipt, ancestry and fingerprint from the prompt is approved: they are validated by `recheck` and
  logged in the retrieval output, never shown. A heading or any other framing text that names the record
  kind is a confound and is not allowed (genret-cli-v2 used kind-specific headings and is not used).
- **Selection cap report:** per task and condition the number of records genret selected (`selected`)
  is logged; the dev and held-out reports give, for C and D separately, how many tasks got fewer than k
  = 3 records (cap or pool exhausted), so a lift cannot hide behind D simply receiving fewer, shorter
  records.
- Realised prompt tokens and suffix bytes are logged per condition and task (run row, GEN-2 FORMATS.md).

## 4. Model and configuration

- Config A only: the model, endpoint, request extra, server pin and weight list of EXP445 config A, read
  from `tools/exp445/PARAMS` (sha256 pinned in protocol.json): Qwen3-30B-A3B-Instruct-2507 FP8 on Omega's
  own `aien-serve-http` (`--engine omega`, :18091). Read-only inference; no weights change. Config B and
  OWN-8B need a later revision.
- The GPU run happens after EXP445 Stage 1 attempt 1, under the quiet flag and GPU lock, never
  concurrently with any other use of :18091.
- No paid model, new hardware or external service.

## 5. Generation procedure

1. Requests: `messages = [{system: prefix ++ suffix}, {user: reader.sh user_msg(requirement, prompt)}]`,
   one POST to `<base_url>/chat/completions`, `stream: false`, the configuration's request extra merged.
   `base_url` is PARAMS `config.A.base_url` and already ends in `/v1` (`http://127.0.0.1:18091/v1`);
   `sampling-gate.sh` takes the base WITHOUT `/v1` (it posts to `<base>/v1/chat/completions`), so the
   gate is called with `config.A.base_url` minus its trailing `/v1`. Both reach the same endpoint.
2. **One request in flight:** gates and run send exactly one request at a time to the server (no
   concurrency, no batching across tasks or conditions); the next request starts only after the previous
   one has returned or failed.
3. **Budget:** `max_tokens = 3072` per round in every condition, task and attempt (EXP445 section 5:
   3.3 times the longest dev reference in the most verbose spelling; Omegese references are shorter
   than OSC text, OMEGESE.md section 5, so the cap is not binding for a correct answer).
   `finish_reason=length` is logged.
4. **Greedy gate (before attempt 1):** the 10 dev tasks in condition B, each sent twice at temperature 0:
   both replies byte-identical in content and every reply reports `mode=greedy`. No gate PASS, no
   primary result (verdict INVALID).
5. **Attempt 1 = PRIMARY:** temperature 0 (greedy), round 1 only. Its candidate is set `<c>-1`
   (`a-1` .. `d-1`). Every attempt-1 response must report `usage.aien_sampling.mode = greedy`; a single
   attempt-1 response reporting anything else (or nothing) makes the run INVALID (section 8).
6. **Attempts 2 and 3 (pass@3, secondary):** temperature 0.7, top_p 0.8, seeded, only when the G0 sampling
   gate passes: the Stage S gate vendored as `tools/exp445/sampling-gate.sh` (sha256 in protocol.json)
   with its repro requests set to `devgen01` in condition B at the eight seeds s1..s8 = 476901, 476902,
   476903, 476904, 476905, 476906, 476907, 476908 (protocol.json `gates.g0.seed_values`; disjoint from
   every run seed), plus one repeat of s1 = 476901, PASS rules as EXP445 13.4. Otherwise pass@3 is
   reported `UNAVAILABLE`. Sets `<c>-2`, `<c>-3`.
   **pass@3** of a condition = the tasks with an `ACCEPT` in the union of the round-1 sets `<c>-1`,
   `<c>-2`, `<c>-3` only (never a `-final` set), labelled "pass@3 (1 greedy + 2 sampled)".
7. **Repair (secondary only):** exactly one public-check repair round when round 1 is refused, as EXP445
   section 6 step 4; the final candidate is set `<c>-<attempt>-final`. Never part of the primary or of
   pass@3.
8. **Seeds:** `seed = 476000 + 1000 x attempt + task index` (1-based position in the sorted task list),
   the same for the four conditions of one task and attempt; sent and logged for attempt 1 too.
9. **Transport retry:** a try fails at transport level when there is a transport error, an HTTP status
   other than 200, or a 200 with **no usable answer**, defined mechanically: the body is not valid JSON,
   or it has no `choices` array, or `choices[0].message.content` is missing or not a string. A failed try
   is retried up to 2 times with the byte-identical request (same seed, same temperature, same top_p)
   after K x 5 s before try K. A retry repeats the same sample; it is never an extra sample. Still
   failing after the third try = no candidate (`no_candidate`, a failure). A well-formed 200 whose
   content is empty, a refusal or not a program is NOT a transport failure: that content is the
   candidate and is graded (it fails). Attempt-1 `no_candidate` is counted per condition; any count
   above 0 makes the run INVALID (section 8), because a transport failure that hits one condition more
   than another biases the paired comparison.
10. **Order:** attempt, then task, then condition (A, B, C, D), so attempt 1 of every task in all four
    conditions finishes first. A run resumes only with identical settings; finished generations are
    never redone.
11. Everything is hashed and logged as `tools/gen/runner/FORMATS.md` (GEN-2) specifies: raw requests and
    responses, prompt manifests, retrieval outputs, run rows, SHA256SUMS.

## 6. Success

- **Functional success** of a candidate = the sealed harness (`ola.sh experiment`, kind `oscir`) gives
  `ACCEPT` on the task's hidden inputs. Anything else (any REJECT reason, `no_candidate`) is failure.
- **Primary measure:** attempt-1, round-1 functional success per condition over the 60 held-out tasks,
  paired by task.
- **Checker acceptance** (the public check or checker2 accepting a candidate) is NOT success: a program
  that compiles and passes the checker can compute the wrong function. It is reported separately
  (`checker_ok`) and never pooled with, or substituted for, functional success.
- The grader runs as `gencust` from a clean tree and publishes a public grading receipt that the reducer
  consumes.
- **Outcome classes (decision input):** the grading receipt gives, for every held-out task x condition
  (attempt 1, round 1), exactly one class: `ACCEPT`, `REJECT:<reason>` (the harness reason, e.g.
  `REJECT:wrong_result`), `no_candidate` (section 5 step 9) or `MISSING` (no row for that task and
  condition). Only `ACCEPT` is success. The reducer refuses to reduce (no verdict at all, not even
  INVALID) when any cell is `MISSING`, when the number of distinct held-out tasks n is not 60, or when a
  task x condition cell appears more than once.

## 7. Secondary measures (reported, never decisive)

Checker acceptance; pass@3 (1 greedy + 2 sampled, or UNAVAILABLE); success after the repair round;
refusal and REJECT reasons by class; prompt and completion tokens per condition; realised suffix bytes;
records selected per task and the number of tasks with fewer than 3 records, for C and D separately
(section 3); wall time; tokens and wall seconds per correct program; Wilson 95% intervals for every
proportion; the 90% Newcombe bounds.

## 8. Validity (any failure: verdict INVALID)

- complete paired matrix: every held-out task x A, B, C, D x attempt 1, exactly once (60 x 4);
- every attempt-1 response reports `mode=greedy`, and attempt-1 `no_candidate` is 0 in each of A, B, C
  and D (section 5 steps 5 and 9; the per-condition counts are in the receipt);
- the harness run ends `RUN VALID` (every reference accepted, zero errors, identical replay, probes
  PASS, clean tree);
- the greedy gate PASSed before attempt 1; every request was sent one at a time (section 5 step 2);
- every pin of section 13 equals protocol.json; every run SHA256SUMS verifies; every candidate re-derives
  from its hashed response; protocol.json passes `gencheck manifest` and `gencheck run-ready` (no null
  field, every interface file present, `genret-cli-v3` pinned; there is no "pending merge" exception);
- **seal-open receipt:** before the first `tasks-public` of the sealed pack for generation (= the seal is
  opened), the custodian runs its published seal-open script, which runs `gencheck open` and writes
  `OPEN`, five lines exactly:
  `gen-open 1` / `omega_commit <40 hex>` / `protocol_sha256 <sha256 of protocol.json at that commit>` /
  `tasks_sha256 <64 hex>` / `heldout_pub_sha256 <64 hex>`, commits it as
  `evidence/GEN/OPEN` and pushes that commit to origin. The run is valid only if that push happened
  before the first `tasks-public`, and protocol.json at `omega_commit` passes `gencheck open` with that
  OPEN: the record-set manifests (paths and digests), corpus META, prefix, `splits.heldout.pub_sha256`
  and `splits.heldout.tasks_sha256` all set (none null) and matching. The reducer refuses a run whose
  OPEN commit has a null or different digest;
- `gencheck split` PASSed on SPLIT and HELDOUT.pub against protocol.json at the OPEN commit (it fails
  closed when either held-out digest is null), the blind re-declaration gave `mismatches=0` and the
  near-duplicate scan gave `above=0` (section 2), all before the seal was opened;
- `oracle_match=60/60` published by the custodian (section 2);
- clean generation tree whose tool files equal the scoring commit's;
- **sanity flag:** B's attempt-1 successes are not lower than A's by more than 2 tasks
  (A - B > 2 = the many-shot prefix is broken: INVALID);
- **source-leak probe:** no request, retrieval output or record contains any held-out canary
  (section 2) or the bytes of any held-out `hidden/inputs` file; checked by the custodian as `gencust`
  after generation, printing only a count (must be 0);
- isolation audit clean (section 12), including no model request by the custodian.

## 9. Statistics and decision (exact)

Notation for condition D against a control X in {A, B, C}, over the n held-out tasks (n = 60): `both`,
`d_only`, `x_only`, `neither` = tasks where both / only D / only X / neither succeed (attempt 1, round 1).

- **Lift:** `diff = (d_only - x_only) / n`; `lift_ok` iff `10 x (d_only - x_only) >= n` (exact integers;
  delta = 10 percentage points = 6 tasks at n = 60).
- **Exact one-sided McNemar** (H1: D better): `p = P(Y >= d_only)`, Y ~ Binomial(d_only + x_only, 1/2),
  computed exactly as `p_num / 2^(d_only + x_only)` (p = 1 when there are no discordant tasks);
  `p_ok` iff `20 x p_num < 2^(d_only + x_only)` (p < 0.05, exact).
- **Newcombe paired interval for D - X**, method 10 of Newcombe (1998), Statist. Med. 17:2635-2650:
  a = both, b = d_only, c = x_only, d = neither, p1 = (a+b)/n, p2 = (a+c)/n, theta = (b - c)/n.
  (l1, u1), (l2, u2) = Wilson score intervals (no continuity correction) of p1, p2, with lower limit
  exactly 0 at a count of 0 and upper exactly 1 at a count of n. phi = N / sqrt((a+b)(c+d)(a+c)(b+d)),
  N = a d - b c, replaced by max(a d - b c - n/2, 0) when a d > b c; phi = 0 when the denominator is 0.
  delta = sqrt((p1-l1)^2 - 2 phi (p1-l1)(u2-p2) + (u2-p2)^2), eps = sqrt((u1-p1)^2 - 2 phi (u1-p1)(p2-l2) +
  (p2-l2)^2) (a negative radicand from rounding is taken as 0); interval [theta - delta, theta + eps],
  clamped to [-1, 1]. Constants: z = 1.959963984540054 (95%), z = 1.644853626951472 (90%, two-sided;
  its upper limit is a one-sided 95% bound). IEEE double arithmetic.
- **Verdict**, in this order:
  1. **INVALID** when any validity condition (section 8) fails.
  2. **INCONCLUSIVE, reason `floor`** when max(successes of A, B, C, D) <= 3, and **INCONCLUSIVE, reason
     `ceiling`** when min(successes of A, B, C, D) >= 57 (both reasons when both hold): with at most 3
     successes everywhere, or at least 57 everywhere, the paired comparison cannot measure anything, and
     neither PASS nor FAIL is claimed. Integers, exact (protocol.json `decision.floor_max = 3`,
     `decision.ceiling_min = 57`).
  3. **PASS** when, for every X in {A, B, C}: `lift_ok` and `p_ok` (intersection-union test: each control
     at level 0.05, no multiplicity correction needed). PASS is reported in exactly these words: "D beats
     every control at one-sided 0.05 with observed lift >= 10pp". It never means "a lift of 10pp or more
     is established": the observed lift is a point estimate, and the Newcombe intervals are reported
     beside it.
  4. **FAIL** when the upper limit of the 95% Newcombe interval for D - C, as the full IEEE double (never
     the 6-decimal value printed in the receipt), is below 0.10. C (raw retrieval) is the designated FAIL
     control: the strongest fair baseline by design.
  5. **INCONCLUSIVE** otherwise (reasons: each failing `lift_<X>` and `p_<X>`).
  PASS and FAIL cannot both hold (PASS needs theta(D-C) >= 0.10, and the upper limit is at least theta).
  The receipt names the verdict and its reason tags.
- At n = 60 this test can only separate large effects: most outcomes that are not a clear win will be
  **INCONCLUSIVE**, and that is the honest report, not a failure of the experiment. The 90% upper limit is
  reported beside the 95% one and is not decisive.
- **Test vectors:** `tests/one_layer/generalization/stats_vectors.tsv`, generated by
  `tools/gen/protocol/genstats vectors` (never by hand), with Wilson, McNemar, Newcombe and full verdict
  records (including the floor and ceiling boundaries) and length-match records (section 10). `genstats selftest` reproduces methods 8 and 10 on all 18 rows of the paper's Table III to the
  printed 4 decimals (one method-10 cell, row 1/97/1/1, is printed 0.8736 where the paper's own method-8
  cell for the identical formula is 0.8737; the computed value is 0.873672). The GEN reducer must
  reproduce every vector (reals within 1e-9; counts, p_num, p_den, yes/no and verdicts exactly).
- The receipt (`omega.gen.decision.v1`, GEN-2 FORMATS.md) reports for each condition the attempt-1
  successes with Wilson 95% intervals and checker acceptance, and for each D-X pair the four counts, diff,
  p, both Newcombe intervals, lift_ok and p_ok, and the verdict with its reasons.

## 10. Negative controls (pre-registered; run on the dev tasks; diagnostics, not decisive)

| control | how | expectation if retrieval works as intended |
|---|---|---|
| no retrieval | C and D with genret mode `empty` | equals B up to sampling noise |
| irrelevant records | genret mode `irrelevant` | no better than B |
| length-matched C | C plus whole extra raw records (rank k+1, ...) until at least D's realised suffix bytes for the same task (genret `length-matched`, `--target-bytes` = D's `suffix_bytes`) | separates "more context" from "better content" on matched tasks only (see below) |
| cross-family | C and D with `--exclude-family <the dev task's own family token>` | shows how much a lift depends on same-family records (on dev, own-family records do not exist in the corpus by R3, so this must change nothing: a check of the split) |
| source-leak probe | canaries and hidden inputs searched in every request (section 8) | 0 hits |

**Length-matched C (secondary, dev-only diagnostic).** It never feeds the PASS / FAIL / INCONCLUSIVE
verdict or run validity. genret-cli-v3 pads C with whole extra raw records, never truncated, in rank order
after the k selected (rank k+1, k+2, ...), until C's suffix is at least D's realised `suffix_bytes` for
the same task (`--target-bytes`): `pad_reason=reached` (it may overshoot by up to one record),
`exhausted` (candidates ran out, still short) or `c_not_shorter` (C was already at least as long; nothing
padded). Its receipt logs `length_gap` = padded C `suffix_bytes` - target, signed. Because padding moves
in whole solutions, it cannot hit a fine target, so each dev task is classified:

- **matched** when |length_gap| <= max(256 bytes, 15% of the target), compared exactly as
  `100 x |gap| <= max(25600, 15 x target)` (protocol.json `retrieval.length_match_abs_bytes = 256`,
  `retrieval.length_match_frac = 0.15`);
- **unmatched** otherwise: listed with its gap and pad_reason, never silently dropped.

The report gives, per dev task, C's and D's `suffix_bytes`, the padded C `suffix_bytes`, pad count,
pad_reason, length_gap and class; then the matched and unmatched counts; then the attempt-1 success of
length-matched C on the matched tasks only, next to normal C and D on those same tasks. When fewer than
50% of the dev tasks are matched (`retrieval.length_match_min_matched_frac = 0.5`; fewer than 5 of 10),
the result is reported plainly as `length-matched ablation: UNAVAILABLE (<m> of <n> dev tasks matched)`
and no conclusion about prompt length is drawn. The matched tasks are a selected subset (tasks with
short solutions match more easily), so the report states that the length-matched result applies to the
matched subset only, gives its size, and is not an estimate for the whole dev set. The comparison is
read in both directions: when C's suffix is longer than D's (gap > 0, including `c_not_shorter`) and C
still does worse than D, that is evidence against the "D only wins because its prompt is longer"
explanation and is reported as such; when C is shorter and loses, length is not ruled out. Reference
values: `genstats lenmatch <target>:<suffix> ...`, and the `lenmatch` / `lenset` records in
`stats_vectors.tsv`.
The held-out decision uses only conditions A, B, C, D in mode `normal`.

## 11. Dev pilot (before the held-out run, never evidence)

Config A, the 10 dev tasks, all four conditions, attempt 1, plus the negative controls of section 10.
GO when: the greedy gate passes; every generation is logged and re-derives; the dev harness run ends
RUN VALID; B is not worse than A by more than 2 tasks. Builders may change prompts, records or tools
using dev results only, then rerun, at most twice; each change before the seal is opened is a protocol
revision under section 15.

## 12. Isolation

- The custodian is the dedicated Unix user `gencust`; the sealed directory is owned by it, mode 0700.
  The custodian authors, seals and grades through `sudo -u gencust`. Builders and the runner never read
  it; the runner sees only `ola.sh tasks-public` output.
- Every sudo call is in the system journal. Before the run, and again before the receipt is accepted,
  the operator audits the journal (`journalctl _COMM=sudo` for the run window) and lists every command
  run as `gencust`; any command not in the custodian's published script list is an isolation failure
  (INVALID).
- **The custodian never queries any model.** No custodian script and no command run as `gencust` sends
  a request to a model endpoint (:18091, :18097 or any other). The journal audit above also searches
  every `gencust` command line for `18091`, `18097`, `chat/completions` and `/v1/`; any hit is an
  isolation failure (INVALID). The audit sees command lines only, not traffic inside an allowed script,
  so every script in the custodian's published list is reviewed before the run to contain no network
  request (stated as a limit of the audit).
- Hidden inputs, expected values, references and grader output are never inputs to generation or
  retrieval (GEN-2 manifests record every input). No grader feedback reaches the treatment during
  generation; the repair round uses only the public check.

## 13. Pins recorded with the run (all in protocol.json or the run's meta)

Model id, endpoint, request extra, server binary sha256 and source commits (via `tools/exp445/PARAMS`);
weight file hashes (`tools/exp445/weights.A.sha256`, re-checked by the operator before the run);
tokenizer files (in the weight list); Omega commit of the run and of scoring; harness contract
(HARNESS_INTERFACE_V2.md) sha256; reader chain (`tools/exp445/reader.sh`) and arm documents sha256;
B and A prefix sha256 and the 14 prefix ids; C and D manifest paths and sha256 (record-set digests); the OPEN
receipt and its commit; genret contract
and binary sha256; dev pack sha256; held-out `TASKS.sha256`, `split_sha256`, `famlist_sha256`;
family contract and famsig sha256; the blind re-declaration line (`tasks= mismatches=`), the genjac summary
line and `oracle_match` with the oracle source sha256; stats vectors sha256; seeds (run and G0), k,
cap_bytes, max_tokens; per-condition attempt-1 `no_candidate` counts; hardware
(DGX Spark GB10) and the GPU-lock / quiet-flag state at run time; this protocol's sha256.

## 14. Commands

```
make test-gen-protocol                                     # famsig, gencheck, genstats, genjac, dev pack (CPU only)
tools/gen/protocol/gencheck manifest tests/one_layer/generalization/protocol.json .
tests/one_layer/generalization/dev/pack_dev.sh <out>
tests/one_layer/acceptance/ola.sh tasks-check <out>/tasks.tar <out>/TASKS.sha256
famsig rule tests/one_layer/generalization/dev/META tests/one_layer/generalization/corpus/META <SPLIT>   # custodian, as gencust
famsig compare <SPLIT> <REDECLARED>                                     # reviewer's blind re-declaration, as gencust
genjac scan <heldout-prompts.list> <corpus-prompts-and-solutions.list>  # near-duplicate check, as gencust
gencheck split tests/one_layer/generalization/protocol.json . <SPLIT> <HELDOUT.pub>                    # reviewer, after the seal
gencheck run-ready tests/one_layer/generalization/protocol.json .       # before seal-open and before the run
gencheck open tests/one_layer/generalization/protocol.json . <OPEN>     # custodian at seal-open; reducer before reducing
```

The runner, reducer, corpus builder and custodian scripts are specified by their own issues (GEN-2
#477, GEN-3 #478, GEN-4 #479, GEN-6 #480) and must implement sections 3 to 9 as written here.

## 15. Revisions

- Before the seal is opened for the held-out run, a change is a new revision: edit this file and
  protocol.json in one commit, add a line below, bump `revision`, and re-run `make test-gen-protocol`.
  A revision may use dev results only; no held-out content is ever opened to justify it.
- After the seal is opened, nothing in sections 2 to 9 changes. Any change (tasks, conditions, budget,
  decision rule) is a new protocol version (`GEN_PROTOCOL_V2.md`) with a NEW sealed held-out pack; the
  v1 result stands as reported. Negative or inconclusive results are never a reason to rewrite v1.

- **v1**: first registration (PR #486).
- **v1, amended at review of PR #486 before merge and before any held-out task existed** (revision stays
  1: v1 was never on main): record-set manifest paths re-hashed and the public OPEN seal-open receipt;
  R6 coarse disjointness and R7 novel edges; blind re-declaration (`famsig compare`) and the genjac
  near-duplicate check; floor and ceiling INCONCLUSIVE; PASS wording; mode=greedy and zero attempt-1
  `no_candidate` as validity conditions; mechanical "no usable answer", identical retries, one request in
  flight; pass@3 from round-1 sets; outcome classes and reducer refusal on MISSING or n != 60; D pool
  excludes prefix-only abstractions; FAIL on the full double; `gencheck run-ready`, `open`, split fail
  closed on null digests; custodian model-query ban; oracle_match; numeric G0 seeds; base_url and query
  bytes stated; genret-cli-v3 pinned with its heading approved; length-matched ablation made a dev-only
  diagnostic with matched/unmatched tasks and UNAVAILABLE below 50% matched.
- **v1, record sets pinned (before merge, before any held-out task):** corpus META, the 14 prefix ids,
  records/PREFIX and the C and D manifests from GEN-3 commit 856cc977 (prefix recomputed from the pick
  rule and matching); genret-cli-v3 re-pinned at 4a48b613.
