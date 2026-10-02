# Commit a reaction: design note

Status: DESIGN NOTE, docs only. No runtime code changes. Nothing here is implemented, qualified or measured.
Evidence base: omega `origin/main` at 2dc9dd8. Every line number below was checked against that commit.
Vocabulary (module, interface, seam, adapter, depth) follows the codebase-design skill.
Owner of `src/runtime`: the M20 program. This note touches no file there.

## 1. The chain today, with evidence

There is no single "commit a reaction" function. The chain is spread over `rx_compose.c` (the only
module that wires it, 1258 lines), `rx_world.c`, `rx_jspace.c` and `rx_cortex_record.c`. AEGIS in this
chain is a reaction named `compose.verify`, not a call into `rx_aegis.c`.

| Step | What happens | Evidence |
|------|--------------|----------|
| Routing | Alternatives are discovered; routing mints nothing | `rx_compose.c:1130-1131` (`sr_route_alternatives`) |
| World: goal published | Goal fields are written as an external publish, then the run waits for quiet | `rx_compose.c:1150` calls `rx_world_publish_external` (`rx_world.c:2053`); wait at `rx_compose.c:1155` via `rx_world_wait_quiescent` (`rx_world.c:2405`) or `wait_own` (`rx_compose.c:908`) |
| J-Space: two candidates | `compose.candidate.0/1` fork a staged branch from the current state branch, run the Skill (a CPU procedure, `rx_compose.c:162`), derive the result into the branch | `candidate_fn` `rx_compose.c:126-188`; fork `:157` (`js_branch_fork_staged`, `rx_jspace.c:1241`); derive `:169` (`js_branch_derive`) |
| AEGIS: verify | `compose.verify` (faculty `RX_FACULTY_AEGIS`) waits for both candidates, applies the contract, picks the lowest-numbered passer, writes the verdict | reaction desc `rx_compose.c:771-787`; `verify_fn` `rx_compose.c:191-220`; the only subject allowed to write the verdict holds the write cap (`:747`) |
| Commit: name the winner | `commit_fn` proposes state.REF = winner branch; proposes nothing if there is no winner | `rx_compose.c:223-232` (no-winner refusal `:226`) |
| Commit: World binder | Before the World accepts the new REF, `bind_check` validates, then `bind_fn` seals the branch (point of no return) | `bind_check` `rx_compose.c:236-249`; `bind_fn` `:251-268` (`js_branch_seal`, `rx_jspace.c:1255`); `bind_abort` `:270`; installed by `rx_world_set_binder` (`rx_world.c:982`, call `rx_compose.c:706`) and `rx_world_bind_field` (`rx_world.c:1010`, call `:803`) |
| Settle | Reclaim losers, make J-Space durable, write Cortex records, release the old branch, make durable again | `rx_compose.c:1175` (`js_space_reclaim_staged`, `rx_jspace.c:1273`), `:1181` (`js_space_commit`, `rx_jspace.c:1516`), `:1189` (`compose_records`), `:1194` release, `:1195` commit |
| Cortex | Candidate claims, evidence ref, promotion of the winner, loser admissions; resumable | `compose_records` `rx_compose.c:340-500`; Cortex attached to the World at open by `rx_cortex_attach` or `rx_cortex_attach_scoped` (`rx_cortex_record.c:209`, `:223`; calls `rx_compose.c:805-809`) |

### Ordering rules (all in `rx_compose.c` unless noted)

1. Binder is installed before objects, caps and reactions (`:705-706`).
2. Objects are created in fixed slot order GOAL, CAND0, CAND1, VERDICT, STATE; own-World mode requires ids 0..4 (`:718-725`, refusal `RX_ERR_BAD_DESC` at `:724`).
3. Caps are minted before reactions (`:735-750`); Cortex is attached after the field bind (`:803-811`).
4. Open order: machine.id check, `cx_open`, `cx_verify_chain`, J-Space open, `recover()`, then the World is built (`:844-871`, `:961-964`).
5. Verify waits for BOTH candidates to be done for this goal (`:198-199`).
6. A pending, incomplete composition record is completed BEFORE a new goal starts (`:1116-1128`).
7. Settle order is reclaim, durable commit, Cortex records, release old, durable commit (`:1175-1199`).
8. The binder seals, then clears branch ownership so a loser or outsider cannot reuse it (`:265-266`).

### Refusal rules

Error codes are defined at `rx_world.h:112-127`.

- `RX_ERR_BINDING` from the binder (`:239-247`, `:259`, `:265`): wrong object or field; caller is not the commit subject; branch not live; not staged or not local; owner is not a candidate; parent is not the branch the World names now; seal refused.
- `RX_ERR_REPLAY` on open: Cortex file will not open (`:858`), chain does not verify (`:859`), J-Space will not open (`:869`).
- `RX_ERR_REPLAY` in `recover()`: missing payload (`:514`); state recorded but none durable (`:530`); a promoted state is missing from the checkpoint (`:540`); admission append fails (`:553`); durable history with no state record (`:563`); genesis root fails (`:569`); durable commit fails (`:574`).
- `RX_ERR_REPLAY` in run: previous run left `pending == 2` (`:1116`); completing the previous record fails (`:1120`, `:1125`).
- Outcomes returned as `RX_OK` with a state in the result: `NOT_DURABLE` (`:1181-1184`, then every run refuses until reopened), `RECORD_FAILED` (`:1189`, `:1195`, `:1206`), `NO_WINNER` (`:1211`), `NOT_COMMITTED` (`:1214`); enum at `rx_compose.h:158-162`.

### What the four named mutants guard

`tools/c4_requal_mutants.sh` rows M05, M06, M12, M16 (script lines 24, 25, 31, 35). Each sed pattern was
checked to match exactly one line. All four guard the recovery path on open, not the live commit above.

| Mutant | Guarded line | Plain meaning |
|--------|--------------|---------------|
| M06 | `rx_compose.c:509` | Recovery walks state records newest first, so it picks the latest durable state |
| M05 | `rx_compose.c:535` | Only records newer than the chosen durable state receive a rollback admission |
| M16 | `rx_compose.c:540` | Open refuses if Cortex promoted a state that the J-Space checkpoint lost |
| M12 | `rx_compose.c:574` | The recovered state is made durable before the World starts; a failed save refuses the open |

This note does not claim which mutants guard the live commit order; that mapping was not done (UNVERIFIED).

## 2. Where FORGE exists today

- `js_forge_choose`: `rx_jspace.c:996` (decl `rx_jspace.h:407`). Picks keep, move or drop for one branch under memory pressure.
- `js_forge_enforce`: `rx_jspace.c:1195` (decl `rx_jspace.h:408`). Applies those choices to stay in a byte budget.
- `omega_forge_realize` `src/forge_realization.c:22`; also `omega_realization_request_init` `:5`, `omega_forge_submit` `:37`. Note this file is in `src/`, not `src/runtime/`.

Proof it is not on the reaction path (grep over the checkout, `.git` excluded):

- `js_forge_choose`: no callers outside `rx_jspace.c`.
- `js_forge_enforce`: callers are `tests/runtime/rx_branch_reuse.c:540` and `tests/runtime/rx_jspace_prod.c:238` only. No caller in `src/`.
- `grep -c js_forge` returns 0 in `rx_compose.c`, `rx_world.c`, `rx_aegis.c` and `rx_cortex.c`.
- `omega_forge_realize`, `omega_forge_submit`, `omega_realization_request_init`: no callers in `src/` or `tests/*.c` beyond their definitions. The file appears only in two test compile lists (`tests/run_reduce_chip.sh:67`, `tests/run_numeric_gates.sh:160`) and one include (`src/aegis_verification.h:4`).
- The repo already says so: `tools/c4_requal.sh:84` ("FORGE is not on the composed path ... omega_forge_realize has no caller in src/runtime").

So there are two different things called FORGE here. In J-Space it is a memory-pressure policy. In
`forge_realization.c` it is the realizer for hardware work. Neither runs when a reaction commits.

## 3. What the architecture records say

Read with `gh` at `aien-dev/aien-architecture` main, commit 26daeedb23c5.

ADR 0016, `docs/adr/0016-resident-reaction-architecture.md` (Accepted by operator, 2026-09-27):
- It sets no fixed World, J-Space, AEGIS, commit, Cortex order. J-Space is not mentioned.
- L59-63: "ATLAS AWAKENS ... FORGE REALIZES. AEGIS VERIFIES ..." is "not a runtime turn order". "OMEGA defines" and "FORGE realizes" are reactions that become ready when proposals exist. "Verification reactions gate promotion. AEGIS is not a synchronous call on every step."
- L51: FORGE realizes; it is not a security gatekeeper. L1484: FORGE is not the kernel and not the policy authority.
- L78: the generation barrier must commit through the ADR 0015 ordered commit protocol.

ADR 0024, `docs/adr/0024-rust-scaffolding-omega-destination.md` (Accepted 2026-10-01):
- Physics and FORGE are class B, hardware substrate, "C and asm justified" (table row 26).
- L38: use the best proven realization regardless of orchestration language. Rule 4: the four-question test before new C.
- It names no commit module.

`doctrine/ARCHITECTURE.md` (ratified): section 2.6 L191-200 puts FORGE at lowering (step 3) and dispatch
(step 6). L229 says `rx_world.c` is the only code that orders faculty work. L245: other substrates attach as FORGE capability providers.

Reading: the doctrine says FORGE is a reaction that realizes work, which is closer to Option A than to
Option B. But it also says FORGE realizes hardware work, and the composed path today runs CPU procedures
only, so nothing in it needs realizing. No doctrine sentence says FORGE must sit on the commit path.

## 4. Proposed "commit a reaction" module (words only)

Today a caller must understand 15 `rx_world_*` entries, the binder, the J-Space branch calls and the Cortex
record calls to get one correct commit, and the correct order lives in comments and in `rx_compose.c`
(deletion test: delete `rx_compose.c` and the ordering rules reappear in every caller).

One interface: "propose a result for a goal, and either commit it or refuse it". The caller supplies the
goal input, the requirement, and the contract that a result must meet. It gets back one of the existing
outcomes (committed, no winner, not committed, not durable, record failed), the old and new state
references, and the winner. The caller never sees a branch, a binder, a verdict object or a Cortex record.

The order it enforces (unchanged from section 1): route, publish the goal, candidates fork from the
current state, verify against the contract, name the winner, seal through the binder, make durable, write
Cortex records, release the old state, make durable again. A previous incomplete record is always finished
first.

Refusal rules it keeps (unchanged): every code in section 1. After `NOT_DURABLE` it refuses everything
until reopened. It refuses to open over durable history with no state record.

Seams it would keep inside: (a) candidate realizer, where today a CPU Skill runs and where FORGE would
plug in if Option A is chosen; (b) the contract check; (c) the durable store (J-Space); (d) the Cortex
record writer; (e) the fault points that tests use (`RXC_FP_*`). The binder, verdict object and slot
layout become private. This is largely what `rx_compose.h` already is, so the first step would be to
rename and narrow it, not to rewrite it. The recovery rules (M05, M06, M12, M16) move with it unchanged.

Not decided here: whether open and recovery are part of the same module or a second one.

## 5. Decision for Drake (plain English)

Where we are: the review found that the part of Omega that decides "this result is accepted and
remembered" works as one well-guarded sequence, but the piece called FORGE, which the design documents
describe as the thing that makes work real, is not part of that sequence. It exists in two places and
neither runs when a result is accepted.

The decision: should FORGE be part of how a result is accepted?

**Option A: wire FORGE into the path.** Candidates would be produced through FORGE instead of through plain
CPU procedures.
- Upside: the code would match what the architecture documents say. GPU work could join the same
  accept-or-refuse sequence with the same safety rules.
- Downside: it touches the most guarded code in the runtime, and today there is nothing for FORGE to do on
  that path, because the work is CPU only. It needs a chip receipt before it can be believed, and the
  runtime files are owned by the M20 program.
- If chosen: a follow-up plan, built behind the module in section 4, with FORGE as one swappable piece. The
  existing recovery tests would have to keep passing unchanged.
- Reversible: yes, as long as FORGE stays a swappable piece. Putting the old CPU path back is small.

**Option B: amend the doctrine so FORGE is only a memory policy inside J-Space.** The documents would be
changed to match the code.
- Upside: no code risk, the records stop promising something the code does not do. Cheapest.
- Downside: it closes the door on FORGE realizing hardware work through this path. The architecture
  document, ADR 0016 and the roadmap would each need an edit, and ADR 0024 treats FORGE as the hardware
  substrate, so the two meanings of FORGE would have to be named separately.
- If chosen: a documents-only change in the architecture repository.
- Reversible: yes, but a reversal later means writing the doctrine again and breaking anything that relied on it.

**Option C: leave as is.** Keep the gap, with the known-gap line already in the receipt.
- Upside: zero cost and zero risk now.
- Downside: the documents and the code stay in disagreement, and anyone reading them will believe FORGE is
  involved when it is not. The gap becomes harder to fix as more code is built around the current path.
- If chosen: nothing changes; the next review will raise it again.
- Reversible: yes, it can be decided later at no loss except time.

Separately, this note recommends doing the section 4 narrowing under any option, since it changes no
behavior and makes the order easier to protect. That is an engineering suggestion, not part of this decision.

One question: do you want FORGE to be part of how Omega accepts a result (A), only a memory policy with the
documents corrected (B), or to leave it for now (C)?
