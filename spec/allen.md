# ALLEN v0: the durable subject inside the organism (omega side)

Status: SPECIFIED + IMPLEMENTED + TESTED (host). Architecture: ARCH-0035
(aien-architecture, ACCEPTED 2026-10-06; see the dated note below). Format: aienos ADR 0018 (PROPOSED), continuity
kind 24. Nothing here is QUALIFIED on hardware, and no OS reboot or machine
migration is claimed.

> Note, 2026-10-08: this file originally read "ARCH-0035 PROPOSED". [ADR 0035](https://github.com/aien-dev/aien-architecture/blob/main/docs/adr/0035-persistent-cognitive-entity-boundary-allen.md)
> was ACCEPTED on 2026-10-06; only the architecture label changed. Aienos ADR 0018 (the kind-24 format) is still
> PROPOSED and unfrozen, and nothing here is QUALIFIED on hardware. User-editable personalization (display name, tone,
> working preferences) is a separate host record, not part of the subject format: see the personalization contract,
> aien-architecture `docs/plans/allen-personalization/CONTRACT-v1.md` (pending PR, aien-dev/aien-architecture#160).

## 1. What ALLEN is here

AIEN is the organism (the resident World, its faculties, its Cortex) and the
cognitive faculty inside it (`src/runtime/rx_aien.*`, "AIEN PROPOSES"). ALLEN
is the durable subject the organism sustains: the same agent identity, the
same standing intents and the same memory lineage, resolved from the AIENOS
Store after any restart. ALLEN is state. It has no cognition, no authority and
no loop.

Omega holds the three bindings and nothing else (`src/allen/allen_bind.{c,h}`):

1. **identity**: the subject object carries the LogicalAgentId and the AgentRoot
   id the Store already has. Omega recomputes the object's content id and
   records the World's external subject number against that agent (the
   binding record). No `AllenId` exists.
2. **memory**: the subject's Cortex lineage reference must equal the digest of
   record 1 of the Cortex journal it is run with (ARCH-0022 journal), or
   nothing is published. Fail closed.
3. **intent**: each ACTIVE standing intent of kind GOAL_LATENCY becomes the goal
   mutation the resident AIEN faculty already reads (`rx_aien.h`: goal =
   {seq, regime, target ns}; field 3 = the intent id's first 64 bits, so the
   EXTERNAL crumb names the intent it came from).

## 2. R16 (binding)

ALLEN publishes typed state through `rx_world_publish_external` under the
World's external subject capability. The World's dependency machinery wakes
`aien.assess` because the goal object changed. ALLEN never calls a faculty,
never registers a reaction, never polls, never sleeps, has no thread, no
timer, no `max_turns`. Gate G5 checks the symbol tables of `allen_bind.o` and
`allen.o` and the source for loop words; the cross-repo loop inventory
(R16-G2, `make test-r16-inventory`) covers the committed tree.

## 3. The host tool (`tools/allen.c`)

Inspection and qualification only: not a chat, not an agent runtime, not a
planner, not a daemon, not a Visor, not an operator shell. Verbs: `make`
(test fixture writer; in production the object is committed by the AIENOS
kernel), `inspect`, `digest`, `lineage`, `seed-journal`, `publish` (the
R11 part-A rig: capability root, World, resident AIEN faculty; checks the
memory binding, publishes every ACTIVE intent, waits for quiescence, prints
what AIEN did), `probe` (the same rig started **without** ALLEN).

The subject codec is compiled from the aienos tree at `aienos.lock`
(`mk/allen.mk`); omega never redefines the format.

## 4. Gates (`tests/allen/run.sh`, `make test-allen`)

| gate | claim | how |
|---|---|---|
| G1 identity binding | the subject id is content-addressed and bound to the LogicalAgentId; another agent gives another id | two makes with the same content give one id; agent B gives another; the binding line names agent A |
| G2 restart persistence | a second process restores the same subject id, intent id and goal, and AIEN assess wakes again | publish, exit, publish |
| G3 memory binding | a subject acts only with its own journal | another journal, an unbound subject and an empty journal are refused before any publish |
| G4 model independence | two model digests and no model at all give the same subject, intent and goal; the object bytes do not change and contain no model | `--model X`, `--model Y`, none |
| G5 no authority, no orchestrator | no thread/wait/timer/signal/reaction/mint symbols in `allen_bind.o`; none of thread/timer/signal/reaction/fork in `allen.o`; no loop words in source | `nm -u`, grep |
| G6 negative control | without ALLEN the restarted World holds 0 goals although the journal holds every record of the publish; with ALLEN the goal is back | publish, probe, publish. **If probe shows a goal, ALLEN is redundant and the gate FAILS ALLEN.** |
| FORMAT | unknown version, corrupted agent/intent, truncation, trailing bytes, unknown origin refused; nothing published | byte patches |
| REPLAY | the same bytes are the same subject every time | ids compared across runs |
| SUPERSEDE | two intents in one slot: the old one SUPERSEDED, only the new one published | `--intent 7,1000 --intent 7,2000` |
| MISMATCH | another agent's object binds as another subject | agent B |

The aienos half (foreign root/agent refusal at the Store resolver, two-process
restart over one sealed Store image, mutants) runs in the aienos tree
(`make -C native/kernel test test-continuity-subject-restart continuity-subject-mutants`)
and is recorded, not re-run, here.

## 5. Status table

| | status |
|---|---|
| same subject identity across process restart | TESTED (host) |
| same standing intent across process restart | TESTED (host) |
| same Cortex binding across process restart | TESTED (host) |
| model replacement (X, Y, none) | TESTED (host) |
| OS reboot | NOT_RUN |
| machine migration | NOT_RUN (not claimed) |
| hardware qualification | NOT_RUN |
| multi-subject | FUTURE, NOT IMPLEMENTED (format does not preclude) |
| intents other than GOAL_LATENCY | FUTURE |
| production path (kernel commits the object; organism boot reads it) | PLANNED; v0 proves the contract on the host rig |

INTERPLANE IMPACT: NONE (no wire change; ALLEN is Store state and World
publication, not a protocol).
