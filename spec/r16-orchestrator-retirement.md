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
| R16-G4 | Legacy paths cannot bypass authority | `tests/runtime/rx_r16_negative.c` (`make test-r16-negative`): from a legacy/reference/oracle context, each of six acts is attempted and refused by the R7 native authority or the publication boundary: (1) write authoritative AIEN belief, (2) select an Omega realization, (3) mint authority, (4) promote a generation, (5) bypass the effect/authority boundary, (6) advance the world generation. 6/6 refused, with no change to authoritative state; removing any one guard turns the test red. Since C6 a subject is bound to a runtime-issued caller credential, and the gate also needs the promoter-subject probes, the C6 identity probes and the promotion control |
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
   all of this. The 16 reference modes live in one table (`reference_modes`)
   that both the dispatcher and the usage text read; the check reads the
   table, requires usage to list exactly those modes, requires every retired
   `--demonstrate-<name>` (16 plus living-matvec) to be rejected, requires
   every new name to be accepted by the real dispatcher through
   `--reference-dispatch-dry-run <name>` (resolved, not run: several modes
   need the chip), and runs `--reference-demonstrate-arithmetic` (CPU only).

### C5 (2026-09-30): G4 is open (caller-named subjects); closed by C6

Recorded after a review of the G3 to G5 branch found it; host data existed
(the G4 host test). This records a discrepancy, it does not loosen G4.

1. `rx_gen_promote` takes the subject from the request, a value the caller
   supplies, and the authority callback checks the grant against that subject.
   Nothing binds the subject to the caller. A legacy context that can read the
   promoter's grant reference (`promotion_authority`, an id and a generation)
   and names `RX_LIVING_PROMOTE_SUBJ` as the subject is accepted by the
   unchanged native authority. Observed on host at 6d1ff1d plus this branch:
   rc 0, R9 active generation 1 -> 3 in memory and on disk.
   The same flaw is on the reaction path (found by review, confirmed on host):
   `validate_caps` (rx_world.c) checks each grant against `d->subject`, the
   subject the descriptor names, and `rx_world_add_reaction` does not check
   who registers. A legacy reaction naming `RX_LIVING_PROMOTE_SUBJ` and
   carrying the promoter's in-force grant was registered (rc 0), activated
   once, committed once and wrote its marker 0xBADBAD into the authoritative
   in-force record. So act (6), and by the same route acts (1) and (2) for any
   writer whose grant reference the legacy context can read, are open too.
   (The probe's wake uses a harness-minted scratch read grant for the promoter
   subject; in normal operation the promoter's own candidate trigger would
   wake it.)
2. `tests/runtime/rx_r16_negative.c` now makes both attempts last (they move
   authoritative state) and expects refusal. Until a subject is bound to
   something the legacy path cannot supply (an authenticated caller or
   registration handle, or the sealed reaction context of the grant's owner),
   `make test-r16-negative` prints two `R16 G4 OPEN: ...` lines and
   `R16_G4_LEGACY_REFUSED=FAIL`. The fix is a runtime change (`rx_world.c`,
   `rx_generation.c`, `rx_living.c`, possibly the native capability library)
   and a design decision; it is not made on this branch.
3. The listed attempts of the six acts are still refused, and the test prints
   `R16 G4 core: six acts refused, state unchanged, for the counted per-act
   attempts only; this is NOT a claim that legacy cannot promote or write`. The mutant suite judges
   that core line, so it still answers whether each of the 15 guards is load
   bearing. No mutant exists for the missing binding, since there is no guard
   yet to remove; when one is added, a mutant removing it must be added too.
4. The G4 state comparison also covers the recovered lineage on disk, not
   only the recovered active generation.
5. C4 item 5's sentence on `rx_gen_promote` trusting its callback is
   superseded here: the problem is the caller-supplied subject, which even
   the real native callback accepts.

### C6 (2026-09-30): C5 closed by runtime-issued caller credentials

Records the runtime change C5 item 2 asked for. It narrows who can act as a
subject; it does not loosen any gate, and it does not make R16 complete
(G6 to G8 and the receipt remain blocked).

1. Mechanism (`src/runtime/rx_caller.h`, `rx_world.c`, `rx_generation.c`).
   - A subject acts only with a credential the runtime issued for it:
     `rx_world_enroll_caller(w, subject, &cred)` draws a 32-byte secret from
     `getrandom` and a world-unique generation (never 0; 0 means absent). The
     world keeps only `SHA-256("AIEN_RX_CALLER_V1", subject, generation,
     secret)` and compares in constant time; the secret exists only in the
     enrolling component's keyring (`RxCallerKeyring`).
   - `rx_world_bind_callers(w)` closes enrollment, one-way. From then on every
     subject is checked. Worlds that never bind (older tests) behave as before.
   - Errors (`RX_CALLER_ERR_*`): ABSENT (no credential, or generation 0),
     UNKNOWN (never enrolled), REVOKED, STALE (another generation), FORGED
     (secret does not match), CLOSED (enrollment after bind), EXISTS
     (subject enrolled twice), ENTROPY. At the authority-bearing calls they
     surface as `RX_ERR_IDENTITY` (world) and `RX_GEN_ERR_IDENTITY` (R9).
   - Revocation (`rx_world_revoke_caller`) needs the credential itself; it
     clears the stored digest. A revoked credential is refused everywhere,
     and enrollment is closed, so the subject cannot be re-issued in that
     world.
2. Where it is checked (every authority-bearing call that takes a subject):
   - `rx_world_add_reaction`: the descriptor's `caller` must be the named
     subject's credential. The reaction keeps only the generation, never the
     secret (it is zeroed on registration).
   - `validate_caps` (activation, commit re-check and the seat completion
     path): the enrollment the reaction was admitted under must still be live
     at that generation, so revoking an identity stops a running reaction's
     write and blocks later activations.
   - R9, when the store is bound (`rx_gen_bind_authority`): `rx_gen_propose_as`
     and the durable `rx_gen_post_propose_as` check the proposer's credential;
     `rx_gen_promote` checks `request->caller` against `request->subject`, then
     validates the promotion right with the store's own bound authority and
     ignores the caller's `RxGenAuthFn`. This also closes C4 item 5.
   - `rx_world_publish_external` is unchanged: its subject is the world's
     fixed `external_subject`, never a caller-supplied value.
3. Production wiring: the R15 rig (the production body for R13 to R16)
   enrolls every production subject (Omega 21, 22; AIEN 31; Aegis 41, 42;
   Living 61, 62, 63; promoter 64) into per-component keyrings right after the
   world is created, then binds the world, then binds the R9 store to the world
   and to the native authority (`rx_living_native_authority`, i.e.
   `aienos_cap_validate`). Faculties register with
   `rx_world_add_reaction_keyed`, which takes the credential from their own
   keyring. The production entry point (R13 living, R14 recovery; see
   `docs/r16-production-entry-point.md`) is wired the same way through
   `rx_living_enroll_callers` (enrolls the nine production subjects into an
   `RxLivingKeyrings`, does not bind). R14 also enrolls its lane, rogue and
   cycle subjects: the rogue is an enrolled in-process party whose
   capabilities are forged. Its B6 promotion forgery now meets the identity
   check first when it names the promoter subject (`RX_GEN_ERR_IDENTITY`),
   and the native authority when it names itself (`RX_GEN_ERR_AUTHORITY`).
4. Evidence (`make test-r16-negative`, host):
   - The pre-fix exploit is kept in the test. At 44d8c06 (before this change)
     it was ACCEPTED: promotion rc 0, generation 1 -> 3 in memory and on disk;
     the named-subject reaction committed and wrote 0xBADBAD into the in-force
     record; gate `R16_G4_LEGACY_REFUSED=FAIL`.
   - After: 6/6 promotion credential variants and 3/3 reaction variants are
     refused with the identity error, nothing moved; 40/40 C5 identity probes
     (spoofed subject, forged, stale, unknown, absent, collision, enrollment
     after bind, revocation, in-flight revocation, replay after revocation,
     permissive authority callback); positive control: the promoter with its
     own credential still promotes. Gate `R16_G4_LEGACY_REFUSED=PASS`.
   - `make test-r16-negative-mutants` judges the gate line (not only the core
     line) and adds 12 identity mutants, one per check above: 27 mutants,
     27 killed, 0 survived, 0 broken; gate `R16_G4_GUARDS_LOAD_BEARING=PASS`.
   - Host regressions with the bound worlds: R7, R8, R9, R10, R11, R13 host,
     R14 host (A to F PASS), R15 parity host, R15 G7 host, workflow fusion,
     G5 surface, visor authority check and the G1/G2 inventory all pass.
     Nothing was run on the graphics processor.
5. Limits:
   - Credentials are secrets in process memory. Code in the same address
     space that reads another component's keyring can act as that component;
     G3 (the production binary does not link or exec the legacy code) is what
     covers that, as it covers any memory write.
   - `rx_gen_bind_authority` takes no lock; it is called once, at startup,
     before any other thread uses the store. Since C7 the bound flag is
     published with release and read with acquire ordering.
   - Other world users outside the R15 rig and R13 / R14 (`rx_contract.c`,
     `rx_graph.c`, `rx_fusion.c`, `rx_route.c`) do not bind their worlds or
     stores yet, so they run unchecked as before. On a bound
     world their unkeyed registrations would be refused (default deny).

### C7 (2026-09-30): outside review of C6, three gaps closed

An outside review (Codex, Gemini) of the C6 commits found three real gaps.
Each is reproduced by a probe in `tests/runtime/rx_r16_negative.c` (section
"R16 C7", on scratch worlds and stores, so the rig is untouched), refused
after the fix, and has a mutant that removes the fix. This narrows C6; it
loosens no gate, and R16 is not complete (G6 to G8 and the receipt remain
blocked).

1. Promotion re-checks identity at the durable commit point.
   - Gap: `rx_gen_promote` checked the promoter's credential only on entry.
     A revocation in the live barrier or during the disk writes still moved
     the active pointer.
   - Fix: the caller check (`RxGenCallerFn`) now takes an `op`:
     `RX_CALLER_OP_CHECK`, or `RX_CALLER_OP_HOLD` then `RX_CALLER_OP_RELEASE`
     (`rx_caller.h`). Right before the pointer is written, the store checks
     the request subject again with HOLD; on failure it returns
     `RX_GEN_ERR_IDENTITY` and the pointer does not move. On success the
     world's enrollment table stays locked, so no revocation lands, until the
     new generation is active in memory; then RELEASE. The entry check stays,
     so a forged promoter never reaches the live barrier or the disk.
   - A refused flip leaves the candidate as the other pre-flip refusals do
     (written but not active; recovery keeps the old generation).
   - Probe P1: revoke the promoter from the live callback, then from the disk
     hook. Before: promote rc 0, generation 1 -> 2 and 2 -> 3, on disk too.
     After: rc `RX_GEN_ERR_IDENTITY`, generation unchanged in memory and on
     disk (`rx_gen_recover`). A live promoter still promotes (control), and a
     forged one is refused before the live barrier runs.
   - Mutant `c7_flip_recheck`.
2. Revocation is mutually exclusive with check-then-publish.
   - Gap: `rx_world_revoke_caller` took only the caller lock. A commit and a
     seat completion hold the world lock from `validate_caps` (identity
     check) to `commit_writes` (publish), so a revocation could complete in
     between and the write still landed.
   - Fix: revocation takes the world lock first, then the caller lock (the
     order every path uses). It now precedes the check or follows the
     publish. It must not be called with the world lock held (reaction
     bodies run without it).
   - Probe P2: the world's capability validator, called inside the commit's
     check after the identity check with the world lock held, starts a
     revocation on another thread and waits up to 300 ms. Before: the
     revocation finished inside that window and the write committed. After:
     the revocation waits until the publish, then succeeds, and the next
     activation is blocked. The seat completion path takes the same lock but
     needs the graphics processor, so it is covered by the same lock, not by
     a host probe.
   - Mutant `c7_revoke_serialized`.
3. Reactions registered before binding.
   - Gap: an unbound world skipped the credential check but kept the
     generation the caller claimed. Generations are sequential, so a reaction
     registered before binding with a guessed generation and any secret ran
     under that subject once the world was bound.
   - Fix: an unbound world fully checks any credential a reaction names; one
     that names none is kept as unauthenticated (generation 0).
     `rx_world_bind_callers` refuses (`RX_ERR_IDENTITY`, world left unbound)
     while any unauthenticated reaction is registered. Worlds that never bind
     behave as before.
   - Probe P3: before binding, a guessed generation with a zero or a random
     secret: before RX_OK, after `RX_ERR_IDENTITY`; the issued credential is
     still admitted and the world binds. With an unauthenticated reaction
     registered: bind before RX_OK, after `RX_ERR_IDENTITY` and unbound.
   - Mutants `c7_prebind_check`, `c7_bind_refuses_unauthenticated`.
4. Smaller review items (Gemini), verified one by one:
   - Fixed: enrollment past `RX_CALLER_MAX` returned `RX_ERR_FULL` (-2), the
     same number as `RX_CALLER_ERR_UNKNOWN`; it now returns
     `RX_CALLER_ERR_FULL` (-9).
   - Fixed: the living promoter's `promotion_caller` silently sent a zeroed
     credential when its keyring lacked the promoter's; it now fails with
     `RX_GEN_ERR_IDENTITY` before anything is posted to the executor (the
     store would refuse it anyway, so this has no mutant).
   - Fixed: the store's bound flag uses release/acquire atomics.
   - Fixed: `rx_gen_mutate_object` and `rx_gen_set_evidence` carry no
     credential; a bound store now refuses them (`RX_GEN_ERR_IDENTITY`).
     Only the R9 tests, on unbound stores, use them. Probe in P1; mutants
     `c7_mutate_object_bound`, `c7_set_evidence_bound`.
     `rx_gen_observe_object` and the work-accounting calls stay open: they
     can make a promotion fail (denial of service), never succeed.
   - Changed: the stored secret in a registered reaction is wiped with a
     volatile loop. The memory stays live, so the plain `memset` could not
     be removed by the compiler; this is belt and braces.
   - Not changed, by design: an unbound store accepts the caller's
     authority callback (the older tests; C6 says so). The credential
     comparison is constant-time on the secret digest; the generation is
     compared plainly because it is not secret.
5. Evidence (`make test-r16-negative`, host): before the fix the C7 section
   was 13/24 as expected (P1, P2 and P3 all failing; gate FAIL); after, 25/25
   (one probe added: forged promoter refused before the barrier), with the
   C5 section still 40/40, the exploit variants still refused and the control
   still promoting; gate `R16_G4_LEGACY_REFUSED=PASS`. Mutants: 33 total
   (27 + 6 C7), 33 killed, 0 survived, 0 broken.
   Host ladder rerun after the fix: R7, R8 (116/0), R9, R10 (198/0), R11
   (436/0), R13 host, R14 host (A to F PASS), R15 parity host (5 pairs, 0
   failures), R15 G7 host (57/0), workflow fusion (17957/0), G5 surface,
   visor authority check, G1/G2 inventory (280 sites, 0 unclassified) and
   the G3 authority path (`HOST_PASS_NON_SILICON`) all pass; the silicon
   binaries were compiled only. Nothing was run on the graphics processor.
