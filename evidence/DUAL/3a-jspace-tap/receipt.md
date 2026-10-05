# DUAL-3a receipt: read-only observation tap at decision site `jspace.residency`

Lane: DUAL-3a (ADR 0031 section 7.3 and section 8 row DUAL-3; `DUAL_CURRENT_STATE.md` slice 5).
Repository: aien-dev/omega. Branch `dual/3a-jspace-tap`.
Host only (CPU); no GPU, no chip test touched.

| Field | Value |
|---|---|
| Base (GitHub main at clone, re-fetched) | `cb7cc216147664549484fcca8e8ceadbd6f6cdbf` |
| Candidate, implementation commit | `9f78d35` (parent `cb7cc216`) |
| Candidate, final | the commit carrying this receipt (PR head) |
| Working tree at each run | clean, `git status --short` empty (section 6) |
| Compiler | `gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0` |
| Flags, tap suite | `-std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 -Isrc -pthread`; ASan/UBSan leg adds `-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all` |
| Flags, existing suite | repository `CFLAGS` (`-std=gnu11 -Wall -Wextra -Werror -MMD -MP -D_GNU_SOURCE -O2 ...`) with `PHYSICS_DIR=$HOME/workspace/physics` |
| Verdict | **DUAL-3a tap IMPLEMENTED; DUAL_ADVISORY_SCHEDULER NOT CLAIMED** |

## 1. What changed

`src/runtime/rx_jspace.h`, `src/runtime/rx_jspace.c` only (production). Decision changes: none.

- `choose()` is now a wrapper over `decide()`, a pure extraction of the same body
  with its working set exposed (the alternatives built, in build order, each
  alternative's score when it frees bytes, the cheapest score). Same conditions,
  same arithmetic, same strict `<` tie rule, same keep test. The alternative
  table literal is untouched; scores go to a side array.
- `js_forge_enforce` / `enforce_u`: one NULL check per decision selects
  `observed_u()` when `s->dual_observer` is set, else `choose()` as before.
- `observed_u()` makes the same `decide()` call, builds one
  `JsDualObservation`, encodes it canonically, digests it, hands the `const`
  record to the observer and returns the chooser's own local result. The
  observer cannot change the decision by type; writing into the record (a
  copy) changes nothing (scenario `record-tamper`).
- `js_dual_set_observer(JsSpace *, JsDualObserver, const void *ctx)`; default
  NULL = today's behaviour; `dual_seq` advances only while an observer is set.
- `js_dual_observation_encode()`: canonical little-endian layout.
- No limit, error path, `JsPolicyReport` field, pruning or externalization
  changed. No wall clock anywhere in the record.

## 2. Record schema (version 1)

Kind byte `0xD3`, version `1`, digest domain `omega.dual.site.jspace_residency.v1`,
decision-site id `jspace.residency`. Canonical encoding is 302 bytes, fixed
little-endian layout, kind and version first; `digest = SHA-256(domain || 0x00 || canonical bytes)`.

| Offset | Bytes | Field |
|---|---|---|
| 0 | 1 | kind `0xD3` |
| 1 | 1 | version `1` |
| 2 | 1 | site id length (16) |
| 3 | 16 | site id `jspace.residency` |
| 19 | 8 | seq (u64, monotonically increasing per space) |
| 27 | 8 | pressure (f64 bits; the value the chooser used: `1e9` or `1e3`) |
| 35 | 1 | pressure case: 1 = CAPACITY (total residency over budget, stays hard), 2 = SOFT (hot arena over half the budget) |
| 36 | 1 | allow_move |
| 37 | 1 | chosen action (`JsAction`; 8 = keep) |
| 38 | 1 | number of priced alternatives |
| 39 | 4 x 34 | alternatives in chooser order MOVE, COMPRESS, SPILL, EVICT: action u8, status u8 (0 inadmissible, 1 built but frees nothing, 2 priced), now_ns f64, later_ns f64, freed_bytes f64, score f64 |
| 175 | 8 | best score (cheapest priced; 0 when none) |
| 183 | 8 | keep threshold = pressure x retain_ns_per_byte |
| 191 | 8 | expected reads |
| 199 | 4 | realization slot (`JsRealId.slot`) |
| 203 | 8 | realization generation (`JsRealId.gen`) |
| 211 | 3 | realization type, placement, recipe |
| 214 | 16 | unit bytes, held (resident) bytes |
| 230 | 8 | holders u32, refs u32 |
| 238 | 8 | last_use (J-Space logical clock) |
| 246 | 32 | semantic state id |
| 278 | 24 | budget bytes, resident bytes, hot-arena bytes at the instant of the decision |

## 3. R16 loop-inventory rule and how it is met

Rule (spec/r16-orchestrator-retirement.md section 4; scanner
`tools/r16_loop_inventory.c`, `--patterns`): every `while(`/`for(`/`do{` in a
tracked `.c/.h/.rs` file is a site when it is unbounded (`while(1)`, `for(;;)`,
...) or its condition/body contains a wait word (`sleep poll recv heartbeat tick
dispatch schedule orchestrate turn pulse yield epoll`), or when a line names
`run_until_complete`, `max_steps`, `max_turns`. New code under `src/runtime/`
must add no site.

Check: `./tools/r16_loop_inventory.sh` before: `sites=387 unclassified=60`
(pre-existing drift in other repositories, unrelated), zero sites in
`src/runtime/rx_jspace.c`. After (new test files tracked): `sites=387
unclassified=60`, zero sites in `src/runtime/rx_jspace.c`,
`tests/runtime/rx_dual_jspace_tap.c`, `tests/runtime/rx_dual_jspace_tap_bench.c`.
Every new loop is bounded and contains no wait word; the tap polls nothing,
sleeps nowhere and sequences nothing.

## 4. Pre-registered benchmark thresholds

Workload (`tests/runtime/rx_dual_jspace_tap_bench.c`, public API only, in
`9f78d35`): fan-out 256 branches over a depth-4 root, 4096-byte units, fixed
`JsCosts`, 40 rounds per trial, each round two timed `js_forge_enforce` calls
(SOFT case at budget 3/2 of residency, CAPACITY case at budget 1/2), 7 trials
per leg, legs interleaved, medians reported. Three legs: base commit
`rx_jspace.c` (observer absent from the code), candidate with observer NULL,
candidate with a recording observer that encodes every record and folds its
digest.

- T1 (cost when off): median wall of candidate/observer=NULL within +-10 % of
  the base commit's median. Below -10 % is noise, not a win claim.
- T2 (cost when on): added nanoseconds per record = (median on - median off) /
  records <= 10 000 ns.
- T3 (bookkeeping): records emitted == decisions considered; bytes emitted ==
  302 x records.
- The on/off wall ratio is reported as information, not a bar: the tap is off
  in production and the per-decision work in this synthetic workload is a few
  hundred nanoseconds, so the ratio measures SHA-256 against memcpy.

A miss is recorded as FAIL in section 6 and not re-run silently.

**Pre-registration caveat (honest record).** This section was written to this
file and staged before the benchmark ran, and the implementation commit
`9f78d35` was meant to carry it. It did not: `evidence/DUAL/` was gone from
the working tree and the index by the time that commit was made (cause
UNKNOWN; `crumb compile` was tested afterwards and does not remove it; no
Makefile rule touches `evidence/`). The thresholds were therefore written
before the run but first committed after it, in the commit carrying this
receipt. The workload and the bench program were committed before the run in
`9f78d35`; the threshold numbers rest on this statement. A reader who wants
a stronger pre-registration should treat T1-T3 as declared-then-run, not
committed-then-run.

## 5. Tests and commands

### 5.1 Existing suite covering rx_jspace.c

`make test-jspace-prod PHYSICS_DIR=$HOME/workspace/physics`

- before (base `cb7cc21`): `checks=40376 failed=0`, `M20_JSPACE_PROD=PASS`
- after (`9f78d35`): `checks=40376 failed=0`, `M20_JSPACE_PROD=PASS`

Other targets that compile rx_jspace.c:

- `build/rx_branch_reuse`: builds after the change (not run: it is the
  OMEGA_BRANCH_STATE_REUSE gate with energy counters and 128 forked
  processes, out of scope for a host-only slice).
- `build/rx_compose_test` (with `AIENOS_LOCK_REPO=$HOME/workspace/dual/aienos`):
  builds after the change, exit 0 (not run).
- `rx_compose_living` / R13 living and C4 requalification binaries: not built
  (chip-side or long-running gates; out of scope).

### 5.2 New targets (`mk/dual_jspace_tap.mk`, not part of `all` or `test`)

`make test-dual-3a` = purity + hygiene + parity (plain and ASan/UBSan) + hostile.

Parity suite `tests/runtime/rx_dual_jspace_tap.c` (includes `rx_jspace.c`
as one translation unit so the reference oracle can call the static cost
helpers). Per scenario three legs: silent, observer A, observer B. Asserted:
identical `js_forge_enforce` return codes, identical `JsPolicyReport`,
identical `JsStats`, identical final space digest (every branch, unit,
placement, extent, content check, then full content digests); A and B record
streams byte-identical; records == considered; seq 1..n; 302 bytes each;
digest recomputed equal; chosen == chooser rule over the record's own
alternatives; chosen == the placement transition that realization (slot,
generation) underwent in the silent run (the failed last decision of an
error-terminated round excepted, and asserted as such); pressure value and
case consistent with the recorded budget and residency.

What "identical sequence of chosen actions" means here: the silent run
exposes no per-decision sequence (that is the point of the tap), so sequence
identity is proven per realization against the silent run (every record's
chosen action equals what the silent run did to that very realization, keyed
by slot and generation) and as a byte-identical record stream between the two
observer legs.

Scenarios: under-budget (0 records), soft-hot-arena (1e3), over-total-capacity
(1e9, then SOFT in the same call once total residency is back under budget),
ties (COMPRESS == EVICT exactly, first built wins), exactly-at-budget,
one-byte-below, one-byte-above, spill-exhaustion (`max_spill_bytes` = 2 units,
rc `JS_ERR_FULL` (-2) in both legs), evict-drain (budget 0), fanout-500 (and
32 in the others), rep-null, record-tamper (observer writes into the const
record: decisions unchanged). Pure extraction: 1752 decisions over 3 cost sets
x 4 pressures x 2 allow_move on mixed placements, `choose()` == verbatim
pre-tap body, 0 mismatches; `js_forge_choose` agrees as well.

Result: `DUAL_3A_JSPACE_TAP_PARITY=PASS (failures=0)` in the plain and the
ASan/UBSan build (no leak, no UB report).

### 5.3 Negative control (REQUIRED)

`make test-dual-jspace-tap-hostile` builds the same suite with
`-DDUAL_TAP_HOSTILE_TEST`: the leg-A observer casts its ctx to the space and
sets `costs.retain_ns_per_byte = 0` on its third record, so the chooser keeps
everything afterwards. Output (abridged):

```
DUAL-3a jspace.residency tap: parity [HOSTILE NEGATIVE CONTROL: the observer mutates the space; this run must FAIL]
  FAIL: soft: a record chose something other than MOVE
  FAIL: soft-hot-arena round 0: JsPolicyReport differs with observer
  FAIL: soft-hot-arena round 1: JsPolicyReport differs with observer
  FAIL: soft-hot-arena: JsStats differ with observer (resident silent 151552 A 151552 B 151552)
  FAIL: soft-hot-arena: final space digest differs with observer
  ... (2035 FAIL lines in all, across every scenario, including per-realization
       "observer saw keep, silent run did move on slot N" lines)
DUAL_3A_JSPACE_TAP_PARITY=FAIL (failures=2035)
test-dual-jspace-tap-hostile: hostile observer detected (parity suite failed as required)
```

The normal build has no such seam: `grep -rn DUAL_TAP_HOSTILE src/` finds
nothing (`make dual-jspace-tap-hygiene`). An earlier hostile variant that
raised the retain price a million-fold was found ineffective (every
alternative was already far below the keep threshold, so no decision moved and
only 1 assertion fired); it was replaced by the retain = 0 variant before the
implementation commit. Recorded so the negative control is known to be a real
one.

### 5.4 Purity

`make dual-jspace-tap-purity`: `nm -u` on `rx_jspace.o` matches none of
`aienos_cap_`, `rx_gen_`, `aegis`. (The tap is inside rx_jspace.c; the object
was also clean before the change.)

## 6. Run log (in order; nothing edited after the fact)

### 6.1 Implementation commit

`9f78d35` on `dual/3a-jspace-tap` (parent `cb7cc216`), working tree clean
(`git status --short | wc -l` = 0) when the benchmark below started. See the
section 4 caveat about the receipt file.

### 6.2 Existing and new suites at `9f78d35`

- `make test-jspace-prod PHYSICS_DIR=$HOME/workspace/physics`: `checks=40376 failed=0`, `M20_JSPACE_PROD=PASS`
- `make test-dual-3a`: purity PASS, hygiene PASS, parity PASS (plain and ASan/UBSan, `failures=0`), hostile detected (`failures=2035`, exit 1 as required)
- `./tools/r16_loop_inventory.sh`: `sites=387 unclassified=60` before and after (pre-existing cross-repository drift; zero sites in the touched files)
- `make build/rx_compose_test PHYSICS_DIR=... AIENOS_LOCK_REPO=$HOME/workspace/dual/aienos`: exit 0

### 6.3 Benchmark (`make bench-dual-jspace-tap`, wall clock, medians of 7 trials)

Run 1 (the pre-registered run):

```
bench[base] observer=NULL  median_wall_ns=2520658 decisions=5410 records=0 bytes=0 (rounds 40, trials 7, fanout 256, unit 4096)
bench[tap] observer=NULL  median_wall_ns=2510132 decisions=5410 records=0 bytes=0 (rounds 40, trials 7, fanout 256, unit 4096)
bench[tap] observer=record median_wall_ns=13637213 decisions=5410 records=5410 bytes=1633820
bench[tap] ratio_on_off=5.433 added_ns_per_record=2056.8 records_equal_decisions=yes
```

Run 2 (repeated once, for noise; not used to pick a result):

```
bench[base] observer=NULL  median_wall_ns=2498835 ...
bench[tap] observer=NULL  median_wall_ns=2488819 ...
bench[tap] observer=record median_wall_ns=13628270 decisions=5410 records=5410 bytes=1633820
bench[tap] ratio_on_off=5.476 added_ns_per_record=2059.0 records_equal_decisions=yes
```

| Threshold | Measured (run 1) | Verdict |
|---|---|---|
| T1 observer NULL within +-10 % of base | 2 510 132 / 2 520 658 = -0.42 % (run 2: -0.40 %) | PASS |
| T2 added ns per record <= 10 000 | 2 056.8 ns (run 2: 2 059.0) | PASS |
| T3 records == decisions, bytes == 302 x records | 5410 == 5410; 1 633 820 == 302 x 5410 | PASS |
| ratio on/off (information only, not a bar) | 5.43x (run 2: 5.48x) | reported |

Reading: with the observer off the change costs nothing measurable (T1). With
a recording observer every decision pays about 2.1 microseconds, almost all
of it the plain-C SHA-256 over 302 + 37 bytes; on this synthetic workload a
decision itself is about 0.46 microseconds (memcpy-sized moves and frees), so
the ratio is large while the absolute cost is small. The ratio is what a
DUAL-3b campaign must budget for when it turns the tap on; it is not a
production cost today because production has no observer.

## 7. Limits

- Host only, synthetic workload, fixed costs; no claim about calibrated
  costs, real workloads or GPU residency.
- The record is this site's own format (kind `0xD3`, version 1). The DUAL
  record module (DUAL-0a, not merged) will reference these digests; no
  dependency on it was taken.
- `rx_branch_reuse`, `rx_compose_test` were built, not run.
- `DUAL_ADVISORY_SCHEDULER` (DUAL-3 exit) is not claimed: no
  `DualRecommendation`, no pre-registered workload set, no
  recommendation-versus-actual analysis. This slice is the tap only.
