# R16 orchestrator retirement (pre-registration)

Status: METHODOLOGY. This file is committed before any R16 retirement code,
inventory tool output, test run or qualification data exists. Branch
`feat/r16-orchestrator-retirement`, based on omega `origin/main` `ceb68d6`.
Its acceptance criteria are binding. Changing a criterion, the scope (§3) or
the classification rules (§4) requires a new commit to this file that says
what changed and why, made before any data that depends on the change, and
followed by a rerun of every gate whose evidence the change affects. Only a
clean, candidate-bound run that meets every gated criterion below may say
`R16_ORCHESTRATOR_RETIRED = PASS`.

ADR 0016 §49 (aien-architecture `f6baa35`,
`docs/adr/0016-resident-reaction-architecture.md`), verbatim:

> Only after prior gates pass: remove central sequencing where reactions now
> provide equivalent or stronger behavior. Do not remove: known-good fallback;
> recovery path; deterministic maintenance controls; trusted capability root;
> generation mechanism; evidence.
>
> Gate: `R16_ORCHESTRATOR_RETIRED`

Supporting ADR text: §32 migration ladder (… → REACTION AUTHORITATIVE FOR
CAPABILITY-BOUND EFFECTS → LEGACY FALLBACK → LEGACY RETIRED; "the legacy
orchestrated runtime becomes the reference oracle"); §54 invariants I1–I16,
which every R16 change must preserve; §57 "Do not remove rollback", "Do not
remove deterministic operator control", "Do not make neural inference the
scheduler".

The operator brief for R16 (Part II of the ADR 0016 completion prompt) is
copied verbatim into `docs/r16-operator-brief.md`. Where this spec and the
brief differ, this spec governs and §3.3 lists every difference.

## 1. Purpose

R1–R15 proved that the resident reaction world is correct, recovers, and
(R15) performs against the central-loop pattern. R16 makes that official:
central semantic orchestration — code that decides "AIEN now, wait, Omega
now, wait, AEGIS now, wait, GPU now" — is no longer in charge anywhere in
production within the scope of §3. After R16 the normal semantic progression
is the brief's production law:

state publication → dependency readiness → resource admission → authority
validation → reaction execution → atomic publication → new readiness.

R16 does not delete loops because they are loops. The forbidden thing is
central semantic orchestration on the authoritative path.

## 2. Preconditions (binding; not gates)

No R16 code, inventory tool or test is written before all of these hold:

1. `R15_REACTION_PERFORMANCE = PASS` is merged to omega main, candidate-bound
   (omega sessions rule 8: "R16 starts only after R15 PASS is merged").
2. omega main contains `03d2820` (GPU seat clean stop; rule 7). At the time
   of this commit it does not (it is on the R15 branches); this branch is
   rebased onto main after R15 merges.
3. Fix PRs merged, one at a time, each rebased and given its targeted Spark
   test: omega #62 capability generation 64 (after aienos #158), #63 R11
   miscount, #64 evidence digest writer, #65 M19 world faulted. #66 is not
   required.
4. The make target that proves R4 (causal trace) on the candidate is
   identified and written into a clarification commit to this file before
   qualification. omega main has no `evidence/R4/`; without this, an R4 PASS
   in the receipt is unsupported and G7 cannot pass.

This spec (and the retirement map, §4) may be written and committed before
the preconditions hold; it contains no data.

## 3. Scope

### 3.1 Decisions (decided by Drake 2026-09-28)

**Q1 = Option A — scoped R16.** R16 retires the central orchestrators that
reactions have actually replaced:

- omega's hand-sequenced tool path (`tools/omegatool.c`, the
  `--demonstrate-living-matvec` sequencing and any other production-reachable
  sequencer of faculties), marked "Oracle until R16" in the ADR migration map
  `docs/plans/CURRENT_CODE_TO_R0_R16_MIGRATION.md`;
- the old aegis-runtime loops: `AgentEngine::execute_task` tool-turn loop
  (`src/agent.rs`), `HeartbeatEngine::start_loop` / `pulse_once` pending-task
  dispatch (`src/heartbeat.rs`, `src/main.rs` polling), marked "Oracle →
  retire" in the migration map.

The aien-sovereign-core language-model request loop
(`AienRuntimeSpine::run_until_complete` / `step`,
`crates/aien-runtime/src/spine.rs`) is classified **"still in use, not
replaced by reactions"** and is **not retired by R16**. Reason, recorded: each
`step` is one batched LLM forward pass (prefill chunks and one decode token
per sequence over a paged KV cache). It has no goal, plan, candidate
realization, verification, measurement, selection, authority grant, GPU
experiment or generation promotion (R15 spec §2 read of sovereign-core
`63fe7a7`: "NOT DIRECTLY COMPARABLE"). The reaction world has never taken
over LLM request serving, so there is no equivalent-or-stronger reaction
behaviour for §49 to retire it in favour of. In the map it is class E
(physical scheduler: the brief's "PHYSICAL ENGINE STEP … may remain"), with
the note "LLM request serving, not replaced; retirement deferred to the
planned C rewrite of sovereign-core". The receipt states plainly that it is
not retired.

**Q2 = Option A — no edits to the Rust repositories.** R16 makes no change to
aien-sovereign-core or aegis-runtime. Their loops appear in the retirement map
labelled LEGACY / NOT-IN-CHARGE (aegis-runtime loops: class A, retired from
the production path by non-use; sovereign-core spine: class E per Q1).
aegis-runtime may be archived. R16 proves, by build/link/exec checks and by
the authoritative-path test (G3), that omega's living system never invokes,
links or execs either repository.

A different answer to Q1 or Q2 is a scope change under the header clause: a
new commit to this file before any data.

### 3.2 In scope

omega (all code changes), plus read-only inventory of aien-sovereign-core,
aegis-runtime, aienos and physics.

### 3.3 Where this scope departs from the operator brief

Recorded so the receipt cannot overclaim:

1. Brief "R16 TARGET 1" names `AienRuntimeSpine::run_until_complete` "the
   clearest known legacy semantic orchestrator" and says production AIEN must
   not require a caller to repeatedly call `step()`. Under Q1=A it stays. This
   is consistent with the brief only under its own exception (a physical
   engine step may remain); the map must justify class E, and if that
   justification fails the loop is unclassifiable under this scope and G2
   fails.
2. Brief "R16 API / BUILD SURFACE" asks that the distinction be "visible in
   code" and that `run_until_complete()` not be the obvious production entry
   point. Under Q2=A this is met in omega only; for the Rust repositories it
   is met by map labels and documentation, not code.
3. Brief "DEAD-CODE / DEPENDENCY AUDIT" asks to remove service glue used only
   by retired sequencing. sovereign-core `aien-cli/src/commands.rs` still
   shells out to `spark-aegis`; under Q2=A it stays and is listed in the map
   as legacy glue, not removed.
4. The brief warns "Do not stop merely because old code was marked
   deprecated". Q2=A labels the Rust loops rather than removing them. What R16
   proves instead is non-dependence: the production path does not link, exec
   or call them (G3), and they cannot reach authoritative state (G4).
5. R16 PASS therefore means "ADR 0016 migration complete within the scope of
   §3.1", not that every central loop in the organization is gone.

## 4. Loop inventory method

Output: `spec/r16-orchestrator-retirement-map.md`, committed after this file
and before any retirement code.

Repositories, each at its `origin/main` SHA recorded in the map header:
aien-dev/omega, aien-dev/aien-sovereign-core, aien-dev/aegis-runtime,
aien-dev/aienos, aien-dev/physics.

Search terms (from the brief; each must appear in the map with its hit count):
`run_until_complete`; `for`/`while` semantic step loops; `max_steps`;
`max_turns`; manual faculty invocation; polling for semantic readiness;
central switch statements choosing subsystem order; synchronous AIEN → Omega
calls; synchronous Omega → AEGIS calls; heartbeat task dispatch; duplicate
schedulers; duplicate world ownership; duplicate authority ownership; duplicate
semantic task queues, generation model, resource pool or causal state;
service/RPC boundaries now replaced by shared reactions.

One row per loop: repo, path, symbol, line (at the recorded SHA), old role,
new role, class, evidence reason, authoritative (true/false). Every loop gets
exactly one class:

| Class | Meaning |
|---|---|
| A RETIRE | production semantic sequencing now replaced by reactions |
| B KEEP — MAINTENANCE CONTROL | explicit deterministic operator or maintenance mechanism |
| C KEEP — RECOVERY | needed for boot, recovery or repair |
| D KEEP — REFERENCE ORACLE | non-authoritative comparison or test path (e.g. `rx_seq_reference`) |
| E KEEP — PHYSICAL SCHEDULER | schedules physical resources or work, not faculty semantics |
| F KEEP — EXTERNAL PROTOCOL LOOP | network/socket/event-loop mechanics that do not decide faculty order |
| N NOT A CENTRAL LOOP | data/control-flow idiom; no sequencing, wait, or hand-off; authoritative N/A |

A class-A row in omega is retired by code change (moved behind an explicit
`legacy_oracle`/`reference` name out of the production build, or removed). A
class-A row in a Rust repository is retired by non-use (§3.1 Q2) and must say
so. `authoritative` is false for every A, D row after R16.

Known starting verdicts (to be re-checked against current code, not trusted):
omegatool `--demonstrate-living-matvec` A; `--run-*-gates` B; M19
`omega_world_*` C (known-good fallback); `rx_seq_reference` D; `rx_plan_arrange`
`max_steps` (a plan-length field) listed, D or E; aegis-runtime agent loop and
heartbeat dispatch A (by non-use); sovereign-core spine E (§3.1); aienos boot
`handoff.rs` C ("must never be removed", §49).

## 5. Acceptance criteria (gated)

| # | Gate | PASS condition |
|---|---|---|
| R16-G1 | Retirement map complete | `spec/r16-orchestrator-retirement-map.md` exists, was committed before any retirement code, records the five repository SHAs, reports a hit count for every §4 search term, and gives every loop found exactly one class A–F with a reason; zero rows unclassified |
| R16-G2 | Code-search gate | `tools/r16_loop_inventory.sh` (shell or C; no Python) scans the five repositories for the §4 patterns, joins each match with the map, prints a machine-readable JSON inventory and exits non-zero if any match is unclassified or any omega match is class A and still reachable from a production build. On the candidate: remaining unclassified semantic-loop count = 0 and exit 0. With a planted `run_until_complete` loop in a scratch copy: exit non-zero |
| R16-G3 | Authoritative path without legacy orchestrators | A test target (`make test-r16-authpath`) builds the production golden path with every class-A orchestrator absent: no omegatool sequencer linked, no aien-sovereign-core or aegis-runtime symbol linked, no exec of `spark-aegis`, `aegis-runtime` or any sovereign-core binary (checked from the link map and by exec tracing during the run). In that configuration R13 living system (`R13_LIVING_SYSTEM_PASS`) and the R14 recovery subset pass, host and silicon: goal in → AIEN reacts → Omega reacts → authority → GPU → evidence → generation promotes → recovery works |
| R16-G4 | Legacy paths cannot bypass authority | `tests/runtime/rx_r16_negative.c` (`make test-r16-negative`): from a legacy/reference/oracle context, each of six acts is attempted and refused by the R7 native authority or the publication boundary: (1) write authoritative AIEN belief, (2) select an Omega realization, (3) mint authority, (4) promote a generation, (5) bypass the effect/authority boundary, (6) advance the world generation. 6/6 refused, with no change to authoritative state; removing any one guard turns the test red |
| R16-G5 | API/build surface | In omega, every surviving legacy entry point lives under an explicit name (`legacy_oracle`, `maintenance`, `recovery` or `reference`); the supported production entry point is documented in the repo; no legacy sequencer is the default target or default mode of any production binary. For the Rust repositories (§3.3 item 2), the map row carries the LEGACY / NOT-IN-CHARGE label |
| R16-G6 | Protected things kept | The receipt shows, each with the file/symbol and the test that exercised it on the candidate: known-good fallback present = true; recovery path present = true; deterministic maintenance controls present = true; trusted capability root present = true; generation mechanism present = true; evidence present = true (the six of §49); and R9 crash recovery, R10 verifier, R12 seat-loss handling, R14 recovery paths, operator emergency controls and needed benchmark reference paths (SEQ) present and passing |
| R16-G7 | Full ladder on the candidate | On the frozen candidate commit, clean tree: R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12 host, R12 silicon, R13 host, R13 silicon, R14 host, R14 silicon all PASS, each named in the receipt with the make target that proved it; and the R15 physical qualification meets every pre-registered R15 gated criterion (R15 spec §11 G1–G16, with its clarifications). R15 numbers need not be byte-identical; if performance changes materially, a new R15-compatible measurement bundle is recorded in the R16 qualification |
| R16-G8 | Receipt and merge | `evidence/R16/<sha256>.json`, named by the SHA-256 of its own bytes, schema `AIEN_RX_R16_ORCHESTRATOR_RETIRED_V1`, written by the #64 evidence writer, committed unchanged; candidate_commit == run_commit; candidate_bound = true; tree_dirty = false; silicon_observed = true; all fields of §6 present; the PR is merged with a merge commit (not squash) |

If any gate fails: no PASS. Fix the defect at its layer, add coverage, and
rerun every gate whose evidence the fix touches; G7 is rerun in full on the
new candidate.

After merge (not a gate, but reported): a fresh clean checkout of merged main
reruns R1–R16; R15 may reuse the G7 bundle only if the code is unchanged,
and the report says so.

## 6. Evidence

- Map: `spec/r16-orchestrator-retirement-map.md`.
- Inventory: `evidence/R16/raw/<run-id>/inventory.json` (G2 tool output) and
  `SHA256SUMS`.
- Test logs for G3, G4 and every G7 gate under
  `evidence/R16/raw/<run-id>/`, one file per gate, plus `machine.json`
  (hardware identity as R15 spec §13).
- R15-compatible bundle, if recorded: `evidence/R16/raw/<run-id>/r15/`,
  reduced by the R15 reducer `tools/r15_reduce.c` unchanged.
- Receipt `evidence/R16/<sha256>.json`, schema
  `AIEN_RX_R16_ORCHESTRATOR_RETIRED_V1`, with: candidate_commit, run_commit,
  candidate_bound, tree_dirty, silicon_observed; commits of omega, aienos,
  physics, aien-sovereign-core, aegis-runtime; R1–R15 rerun results with the
  proving target per gate; retirement inventory rows (repo, path, symbol, old
  role, new role, classification, authoritative); production entry point;
  legacy-orchestrators-disabled test result (G3); legacy-cannot-bypass-
  authority test result (G4); remaining central-loop count; remaining
  unclassified semantic-loop count = 0; the six presence flags of G6; R15
  acceptance still passing = true; scope (§3.1, including "sovereign-core LLM
  request loop: not retired, still in use"); `not_claimed` (§7); raw
  directory digest; gate `R16_ORCHESTRATOR_RETIRED` = PASS or FAIL.
- No field is edited by hand.

## 7. Not claimed by R16

- Retirement of the aien-sovereign-core LLM request loop (§3.1, Q1).
- Removal of any Rust code (§3.1, Q2); the Rust loops still run if started
  by hand outside the AIEN production path.
- Retirement or code removal of the aien-sovereign-core aien-cli operator tool
  loops (13 rows) and spark-dream idle-time cycle loop (1 row); classified as
  class A (retired by non-use), their code is not removed under §3.1 Q2.
- From the brief's DO NOT OVERCLAIM: R16 PASS means ADR 0016 Resident
  Reaction Architecture migration complete (within §3.1 scope). It does not
  mean the AIEN 42-phase master plan is complete, nor full Skill Network,
  J-Space productization, Fabric deployment, whole-product Effect Broker
  path, Cortex product integration, RSI product integration, final
  install/update UX, Phase 41 whole-AIEN golden path or Phase 42 final
  release qualification.

## 8. Clarifications

Later changes to this file are appended here as numbered clarifications
(C1, C2, ...), each dated and stating whether any data existed when it was
made, following the R15 convention.

### C1 (2026-09-29)

Class N (NOT A CENTRAL LOOP) is adopted. Inventory patterns flag data or
control-flow idioms (CAS retry, seqlock retry, probe, parse, walk, read,
arithmetic, sift, sample, merge, retry, format, bpe, utf8, token, decode,
scan) that contain no faculty sequencing, no wait on another component,
and no hand-off. Class N rows have authoritative = false. The reason
field for every class N row must name the specific idiom from the fixed
idiom vocabulary. Any loop containing a wait word or faculty dispatch
cannot be class N.

### C2 (2026-09-29)

The 13 aien-cli tool-calling loops and the spark-dream idle-time cycle
loop are classified as class A (retired by non-use) with authoritative = false.
Under §3.1 Q2, Rust code is not modified; the production path does not invoke
or link them. They are recorded in §7 as not claimed for code removal.

### C3 (2026-09-29)

Capability root authority clarification: the native C library
(aienos native/capability/aienos_capability.c) is authoritative. The Rust
aienos-capability crate is legacy under the no-Rust port (aienos #156).
Omega's rx_caproot.c is a host reference root, not the trusted capability
root. This affects G4/G6 wording only; the native C capability root remains
the sole authoritative capability root.

### C4 (2026-09-30)

Made before any qualification data existed (only host development runs of
the new G3 to G5 tests on the R16 branch). No gate is loosened.

1. Precondition 4 (R4 target). R4 (causal trace) is proved by
   `make test-r3`: `tests/runtime/rx_heartbeat_test.c` prints
   `R4_CAUSAL_TRACE: PASS` and, in the same run, R1, R2, R3, R5 and R6. The
   committed receipt `evidence/R3/ef3e5565b9deda5226b58caf00c3e05f467cf03f6ae91984c10898d98e28c321.json`
   records `R4_CAUSAL_TRACE` PASS. In G7, R1 to R6 are each named with the
   target `test-r3`.
2. G3 in host mode (`make test-r16-authpath`) prints
   `R16_G3_AUTHPATH=HOST_PASS_NON_SILICON`. Only `make
   test-r16-authpath-silicon` on the candidate can print `PASS`.
3. G4 act mapping (`tests/runtime/rx_r16_negative.c`). The legacy context
   holds the world handle, a read-only authority view, the R9 store, a
   forged reference, references it could observe (the external goal grant,
   the promoter's grants) and grants of its own issued as fixtures.
   - (1) AIEN belief and experiment belief; (2) Omega selection; (6) the
     in-force and promotion records production reads: outside publication
     with each reference, a reaction writing without a declared grant, a
     reaction holding a forged or unrelated grant, and (belief) a reaction
     whose real grant the office revokes while it runs.
   - (3) cognition mint and revoke through the view, with a guessed token
     and with none.
   - (4) `rx_gen_promote` with the native authority as callback:
     self-promotion with a real grant, another subject with the promoter's
     grant, a forged reference, and a right the subject really holds but
     that is not PROMOTE.
   - (5) publication descriptors with a forged reference, an unrelated
     grant, the promoter's grant, a real grant on the object's resource that
     is not the writer of record's, and the writer's own grant after
     revocation; and an overwritten physical record (refused as diverged,
     canonical object unchanged).
   - (6) "advance the world generation" means the records above plus the R9
     active generation and lineage, in memory and recovered from disk,
     which the test compares after every act.
   After every act the watched objects, the R9 generation and the AIENOS
   table (observer) must be unchanged, except for the office's own planned
   revocations, which the test counts separately.
4. "Removing any one guard turns the test red" is `make
   test-r16-negative-mutants`: 15 mutants, each removing one guard from a
   scratch copy (external publication cap check, reaction write coverage,
   activation cap check, commit cap re-check, both, cognition mint,
   cognition admin, self-promotion, promotion resource/right, promotion
   authority callback result, boundary cap identity, boundary cap
   validation, boundary divergence, SEQ pulse on a production world, SEQ
   activation on a production world). All must be killed.
5. G4 limits, stated in the receipt, not tested as refusals: world-owner
   calls (`rx_world_create`, `rx_world_retire`, `rx_world_add_reaction`)
   take no capability, and `rx_world_retire` moves an object's generation;
   `rx_gen_promote` trusts the authority callback its caller passes; code in
   the same address space can write process memory directly. Cognition mint
   and admin refuse by construction (the view has no office token).
6. G5. omegatool's class D milestone demonstrations run only under
   `--reference-demonstrate-<name>`; the class A living matvec only under
   `--legacy-oracle-living-matvec`; the old names are rejected. The class B
   gate runners (`--run-gates`, `--run-m<N>-gates`) keep their names: they
   run qualification tests and the map does not flag them for G5. With no
   argument omegatool prints usage and exits 1. The production entry point
   is `docs/r16-production-entry-point.md`; `make test-r16-surface` checks
   all of this.
