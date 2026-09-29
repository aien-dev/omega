# R16 operator brief (Part II, verbatim)

Source: operator prompt "FINISH ADR 0016 — R15 PERFORMANCE PROOF, THEN R16
ORCHESTRATOR RETIREMENT", Part II, decoded 2026-09-28 from the Codex session log
`~/.codex/sessions/2026/09/27/rollout-2026-09-27T21-37-13-01a0e5df-a2e0-7100-852c-53a7e4ce2b34.jsonl`.
It was not in any repository before this commit. Text below is unedited.
The binding acceptance criteria are in `spec/r16-orchestrator-retirement.md`.

```text
PART II — R16 RETIRE CENTRAL ORCHESTRATION
============================================================

BEGIN ONLY AFTER:

R15_REACTION_PERFORMANCE_PASS

is merged and candidate-bound.

First perform an organization-wide CODE audit.

At minimum inspect:

aien-dev/omega
aien-dev/aien-sovereign-core
aien-dev/aegis-runtime
aien-dev/aienos
aien-dev/physics

Search for:

run_until_complete
for/while semantic step loops
max_steps
max_turns
manual faculty invocation
polling for semantic readiness
central switch statements choosing subsystem order
synchronous AIEN → Omega calls
synchronous Omega → AEGIS calls
heartbeat task dispatch
duplicate schedulers
duplicate world ownership
duplicate authority ownership
service/RPC boundaries now replaced by shared reactions

Produce:

spec/r16-orchestrator-retirement-map.md

Classify every central loop as exactly one of:

A. RETIRE
Production semantic sequencing now replaced by reactions.

B. KEEP — MAINTENANCE CONTROL
Explicit deterministic operator or maintenance mechanism.

C. KEEP — RECOVERY
Needed for boot/recovery/repair.

D. KEEP — REFERENCE ORACLE
Non-authoritative comparison/test path.

E. KEEP — PHYSICAL SCHEDULER
Schedules physical resources/work, not faculty semantics.

F. KEEP — EXTERNAL PROTOCOL LOOP
Network/socket/event-loop mechanics that do not decide semantic faculty order.

Do not delete a loop merely because it is a loop.

The forbidden thing is CENTRAL SEMANTIC ORCHESTRATION.

============================================================
R16 TARGET 1 — aien-sovereign-core
============================================================

Current known path:

crates/aien-runtime/src/spine.rs

contains:

AienRuntimeSpine::run_until_complete()

which loops:

for _ in 0..max_steps {
    self.step(backend).await
}

This is the clearest known legacy semantic orchestrator.

Determine whether the new resident reaction path now provides equivalent or
stronger production behavior.

If yes:

remove this path from production semantic authority.

Possible acceptable end states include:

- move behind an explicit legacy/reference build feature;
- restrict to tests/benchmark oracle;
- preserve an operator-only deterministic compatibility mode;
- remove entirely if nothing legitimately needs it.

Do not choose the mechanism based on aesthetics.

Production AIEN must no longer require a caller to repeatedly call `step()` to
decide semantic progress.

If `step()` remains because a physical inference backend needs a primitive
advance operation, clearly separate:

PHYSICAL ENGINE STEP

from

SEMANTIC SYSTEM SCHEDULING.

A physical engine step may remain.

A master semantic loop deciding when the system thinks/realizes/authorizes may
not.

============================================================
R16 TARGET 2 — aegis-runtime
============================================================

Current live code contains:

src/agent.rs
AgentEngine::execute_task()

for turn in 1..=max_turns {
    model
    tool
    model
    tool
}

and:

src/heartbeat.rs
start_loop()
pulse_once()

which periodically scans SQLite pending tasks and dispatches actions.

The resident R8 AEGIS faculty is already a different authority model.

Audit whether these legacy components remain needed for:

- compatibility;
- operator maintenance;
- reference testing;
- old standalone tool-agent mode.

If they remain, explicitly mark them LEGACY/NON-AUTHORITATIVE and ensure the
production resident architecture does not depend on them for policy or
semantic progression.

Do not rename the old runtime and pretend it became the resident faculty.

============================================================
R16 TARGET 3 — DUPLICATE OWNERSHIP
============================================================

Find places where old subsystems still independently own:

- scheduler;
- world state;
- authority state;
- semantic task queues;
- model of generation;
- resource pool;
- causal state.

Where the reaction architecture is authoritative, eliminate or demote duplicate
production ownership.

There should not be:

AIEN scheduler
Omega scheduler
AEGIS scheduler

independently deciding semantic order.

There may still be physical worker pools.

============================================================
R16 PRODUCTION LAW
============================================================

After R16, the normal semantic progression must be:

state publication
    ↓
dependency readiness
    ↓
resource admission
    ↓
authority validation
    ↓
reaction execution
    ↓
atomic publication
    ↓
new readiness

No production function may encode:

call AIEN
wait
call Omega
wait
call AEGIS
wait
dispatch GPU
wait

as the authoritative path.

============================================================
DO NOT REMOVE THESE
============================================================

Per ADR 0016, retain:

- known-good fallback;
- recovery path;
- deterministic maintenance controls;
- trusted capability root;
- generation mechanism;
- evidence.

Also preserve:

- R9 crash recovery;
- R10 verifier;
- R12 seat-loss handling;
- R14 recovery paths;
- operator emergency controls;
- benchmark reference paths where needed.

Do not optimize the architecture by deleting the evidence that proved it.

============================================================
R16 AUTHORITATIVE-PATH TEST
============================================================

Create an explicit test proving that the production golden path works with
legacy semantic orchestrators unavailable.

For example:

build/run configuration where:

AienRuntimeSpine::run_until_complete
legacy AEGIS AgentEngine loop
legacy AEGIS heartbeat task dispatcher

are unavailable as semantic authorities.

Then run:

R13 living system
R14 recovery subset/full qualification as appropriate

The organism must still:

receive goal
AIEN reacts
Omega reacts
authority works
GPU works
evidence returns
generation promotes
recovery works

If disabling a legacy orchestrator breaks the new production path, R16 is not
done.

============================================================
R16 NEGATIVE TEST
============================================================

Also prove the reverse:

legacy/reference paths cannot mutate authoritative resident state except
through the same normal capability/publication boundaries.

A legacy oracle must not be able to:

- write authoritative AIEN belief directly;
- select Omega realization directly;
- mint authority;
- promote generation;
- bypass Effect/authority boundaries;
- manually advance world generation.

============================================================
R16 API / BUILD SURFACE
============================================================

Make the distinction visible in code.

Avoid ambiguous APIs where a future developer can accidentally re-enable the
old architecture.

Use explicit naming/features/modules such as:

legacy_oracle
maintenance
recovery
reference

where appropriate.

Do not leave:

run_until_complete()

as the obvious default production entry point if it is no longer authoritative.

Document the supported production entry point.

============================================================
R16 DEAD-CODE / DEPENDENCY AUDIT
============================================================

After retiring orchestration:

- remove unreachable central scheduling code where safe;
- remove service glue used only by retired sequencing;
- remove duplicate queues where safe;
- remove duplicate state ownership;
- remove stale dependencies.

But do this only where evidence proves replacement.

Do not perform a giant unrelated cleanup.

============================================================
R16 CORRECTNESS REGRESSION
============================================================

R16 must rerun EVERY R gate.

At minimum:

R1
R2
R3
R4
R5
R6
R7
R8
R9
R10
R11
R12 host
R12 silicon
R13 host
R13 silicon
R14 host
R14 silicon
R15 physical performance qualification

R15 numbers do not have to be byte-identical after code removal, but the
pre-registered R15 acceptance policy must still pass.

If performance changes materially, record a new R15-compatible measurement
bundle in the R16 qualification.

============================================================
R16 CODE SEARCH GATE
============================================================

Create a machine-readable inventory of all remaining central loops matching
patterns such as:

run_until_complete
max_steps
max_turns
manual step loops
heartbeat semantic task dispatch
faculty service sequencing

For each surviving match, attach classification:

maintenance
recovery
reference
physical scheduler
external protocol

No unclassified production semantic orchestrator may remain.

Gate fails if one remains.

============================================================
R16 RECEIPT
============================================================

Create:

spec/r16-orchestrator-retirement.md

evidence/R16/<sha256>.json

Suggested schema:

AIEN_RX_R16_ORCHESTRATOR_RETIRED_V1

Include:

candidate_commit
run_commit
candidate_bound
tree_dirty
silicon_observed

R1–R15 rerun results

retirement inventory:
repo
path
symbol
old role
new role
classification
authoritative = true/false

production entry point

legacy orchestrators disabled test

legacy cannot bypass authority test

remaining central-loop count
remaining unclassified semantic-loop count = 0

known-good fallback present = true
recovery present = true
maintenance controls present = true
trusted capability root present = true
generation mechanism present = true
evidence present = true

R15 acceptance still passing = true

gate:
R16_ORCHESTRATOR_RETIRED = PASS

============================================================
R16 QUALIFICATION DISCIPLINE
============================================================

1. complete retirement map first;
2. commit map/spec;
3. make minimal code changes;
4. add explicit authoritative-path tests;
5. commit candidate;
6. clean tree;
7. run entire R1–R15 ladder;
8. run physical GB10 qualification;
9. verify R15 acceptance remains satisfied;
10. candidate == run commit;
11. generate receipt;
12. commit evidence unchanged;
13. open PR;
14. merge with merge commit.

============================================================
FINAL ADR 0016 COMPLETION CHECK
============================================================

After R16 merge, perform one fresh clean checkout of the merged tree.

From that checkout verify:

R1  PASS
R2  PASS
R3  PASS
R4  PASS
R5  PASS
R6  PASS
R7  PASS
R8  PASS
R9  PASS
R10 PASS
R11 PASS
R12 PASS
R13 PASS
R14 PASS
R15 PASS
R16 PASS

Then inspect production code again.

The final architecture must truthfully be describable as:

AIEN is a persistent resident computational system in which cognition,
realization, authority, resource arbitration, CPU/GPU execution, evidence,
recovery and durable promotion operate over one generation-addressed object
world.

Work progresses because dependencies become ready, not because a master loop
chooses which faculty goes next.

============================================================
DO NOT OVERCLAIM
============================================================

Completion of R16 means:

ADR 0016 / Resident Reaction Architecture migration COMPLETE.

It does NOT mean the entire AIEN 42-phase master plan is complete.

Do not claim:

- full Skill Network completion;
- complete J-Space productization;
- complete Fabric deployment;
- complete whole-product Effect Broker path;
- full Cortex product integration;
- full RSI product integration;
- final install/update UX;
- Phase 41 whole-AIEN golden path;
- Phase 42 final release qualification;

unless separately proven by their own current evidence.

============================================================
FINAL REPORT TO OPERATOR
============================================================

When both gates are merged, report:

R15:
- implementation commit
- evidence commit
- receipt SHA
- raw evidence location
- exact hardware
- benchmark methodology
- legacy vs resident table
- every mandatory ADR metric
- statistically supported wins
- regressions/tradeoffs
- energy method
- acceptance criteria and result

R16:
- implementation commit
- evidence commit
- receipt SHA
- every retired orchestrator
- every surviving loop and its classification
- production entry point
- proof that legacy loops are unnecessary for the resident golden path
- proof legacy paths cannot bypass authority
- full R1–R16 rerun
- remaining limitations

Finish with exactly one architectural status statement:

ADR 0016 RESIDENT REACTION MIGRATION:
COMPLETE

Only say that if both:

R15_REACTION_PERFORMANCE_PASS
and
R16_ORCHESTRATOR_RETIRED

are genuinely proven.

============================================================
FIRST ACTION NOW
============================================================

Fetch current main in every relevant repository.

Do not trust README claims.

Inspect actual code.

Produce the R15 benchmark/instrumentation map.

Commit the R15 methodology BEFORE looking at final qualification results.

Then execute R15.

If and only if R15 passes, proceed immediately to R16 and finish the
migration.

Do not stop merely because one benchmark looks impressive.
Do not stop merely because old code was marked deprecated.
Finish both gates with candidate-bound evidence.
```
