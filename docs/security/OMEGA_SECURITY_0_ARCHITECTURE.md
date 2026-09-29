# OMEGA SECURITY-0: exploit containment

**Status:** Reviewable architecture. No runtime change.

**Baseline:** `aien-dev/omega` commit `c0edef0337e7d27150eb7274ff13bbb10cba3021` (`origin/main` on 2026-09-29).

**Decision:** [OMEGA-0001](../adr/0001-exploit-containment-and-execution-envelopes.md).

**Boundaries:** [OMEGA_SECURITY_TRUST_BOUNDARIES.md](OMEGA_SECURITY_TRUST_BOUNDARIES.md).

**Threats:** [OMEGA_SECURITY_THREAT_MODEL.md](OMEGA_SECURITY_THREAT_MODEL.md).

Pins read from this tree:

| Lock | Commit | Role |
|---|---|---|
| `aienos.lock` | `d39dd5bc3deb1a24e26a5059477a1342c110c71b` | Native capability authority. On `aienos` `origin/main`. |
| `physics.lock` | `fecbedb1b9cea9e4de0fa3c88fe795934fb361f9` | Physical machine pin. The host CI workflow checks out the same commit. |
| `argus.lock` | `b375dcaa2887d53cda407d773c2e4e489a0b1365` | ARGUS ABI used when `RX_ARGUS` is not 0. The comment in the lock says this is `feat/argus-0`, not a claim that observation is on by default. |

R16 is not in this commit. `docs/r15-handoff.md` still names it as later work. The production law used below is the one written on branch `r16/w2-w4` (`docs/r16-operator-brief.md`): publication, readiness, admission, authority validation, execution, atomic publication. This document does not implement that retirement.

## 1. Current code truth

Inspection was the sources and the tests they name, not the status lines in READMEs.

The resident world (`src/runtime/rx_world.h`) is one object table. A reaction declares triggers, reads, writes, a subject, a resource need, and up to eight capability needs. Workers run whatever admission has marked ready. They do not pick the semantic order. `run_one` checks that inputs are live, checks capabilities, runs the body with the world lock dropped, then checks generations and capabilities again and publishes only inside the write set. Each outcome appends a crumb.

The capability root (`src/runtime/rx_caproot.h`) is a Linux process with a sealed read-only table. The world holds `RxCapRoot` or a native view (`aienos_cap.h`). It does not hold `RxCapAdmin`. Rights are read, write, effect, delegate, and the non-delegable set: mint, revoke, reclaim, epoch, clock, promote (`0x200`). Generations on capabilities are 64 bits and do not wrap.

Object generations in the world are 32 bits. `rx_world_retire` refuses `UINT32_MAX` instead of wrapping. Lineage ids in `rx_generation.h` are 64-bit generation-store ids. The M19 accelerator registry (`omega_accelerator_world.h`) has another 32-bit world epoch and another per-slot generation. Those three counters are not interchangeable.

Programs (`omega_program.h`) hash body and contract into `program_id` under the domain `omega.program.v2`. Name, cost, and realization are outside that hash. A realization has its own id (`omega_realize.h`). The library (`omega_library.h`) stores up to eight dependency ids per verified program and can look a program up by realization id. Insert fails when `is_verified` is false. That flag is not a capability.

Verification (`omega_verify.h`) implements V0 structural, V1 differential, and V2 property checks. V3 adversarial, V4 symbolic, and V5 proof-carrying are enum values only. Resident Omega (`rx_omega.h`) runs unverified matvec bytes in a forked child and, after a pass, maps the same bytes executable in the parent.

Effects exist as canonical objects. `KIND_EFFECT` version 2 is a 178-byte big-endian payload with a 64-bit capability generation (`omega_types.h`, `spec/effect-cap64-migration.md`). The Visor builds a request whose `authorized` field is always false and exports no setter (`visor_effect_request.h`). The older accelerator client struct `OmegaEffectIntent` still stores `capability_generation` as `uint32_t` (`omega_accelerator.h`).

AEGIS in this tree (`rx_aegis.h`) is a pair of reactions: `aegis.decide` writes a decision object, `root.install` mints only after origin and domain checks. ARGUS (`rx_argus.h`) copies decisions onto rings when the build enables it. The default heartbeat sources in the Makefile do not link it.

Python does not appear under `src/`.

## 2. Existing security mechanisms

- **Reference validation.** Forged, stale, revoked, expired, wrong-subject, wrong-resource, and amplified references fail in the root. The world calls that check before the body and again before commit.
- **Attenuation.** Delegation cannot add rights. Privileged rights, including promote, are not delegable.
- **Slots.** A capability need may read `{id, generation}` from an object at check time. The slot is not permission. The root still validates the pair against the reaction's subject.
- **Promotion barrier.** `rx_gen_promote` rejects a promoter whose subject equals the candidate's proposer, requires the promotion resource and the promote right, requires `proofs_ok`, and requires observed object generations to match.
- **Admission and quarantine.** `RxResourceNeed` and `RxResourceBudget` gate memory, locality, accelerator bits, and energy. `RxStabilityBudget` quarantines budget, oscillation, livelock, and conflict. Quarantine stops the reaction. It does not revoke a capability.
- **Resident graphics claim.** A reaction that asks for the Blackwell bit posts a claim on objects in this world. Completion still goes through publication checks. The seat does not own a second semantic id space. The M19 registry is a separate stack that has not been removed.
- **Effect classification in the Visor.** `visor_effect_request_classify` reports whether a graph walk reaches an effect or a capability, intent, or receipt type. A failure and a positive result are both refusals to run inside the Visor.
- **Crumbs.** The crumb records inputs, the capability references presented, their issuer when inspect succeeds, outputs, and parent crumbs.

## 3. Missing mechanisms

- One computed view that names program, realization, subject, generations, objects, capabilities, effects, bounds, class, dependency root, and verify policy together.
- A safety class that can only narrow or refuse.
- An address-space boundary between a verified native body and the world process.
- A foreign-runtime contract. Python is unspecified in code because it is absent.
- Credential custody. This repository has no vault and no Effect Broker call.
- A reachability index from an outside artifact digest to realizations, programs, worlds, live reactions, rights, and effects.
- Retirement of `OmegaAcceleratorWorld` as a second dispatch registry.
- A fix for `omega_accel_port_bind_machine_graph`, which sets `is_physics_authorized` after a digest compare. The compare comment in `omega_accelerator.c` says the digest does not authenticate the producer.
- Width alignment: the accelerator intent's capability generation is 32 bits; the canonical effect and the authority are 64.

## 4. Trusted boundaries

The nucleus is the capability check, promotion, publication, `root.install`, and the canonical effect decoder. Detail, including which handles exist in which process, is in `OMEGA_SECURITY_TRUST_BOUNDARIES.md`.

The world mutex is not a trust boundary. It is a lock around the tables. The body runs outside it.

## 5. Untrusted boundaries

Reaction bodies, models, synthesized bytes, parsers, the Visor, ARGUS, the graphics worker, and any future Python interpreter are untrusted. They become able to change canonical state only by a publication the root still allows.

`is_verified`, a Visor compatibility flag, an ARGUS finding, and `is_physics_authorized` are not in that set.

## 6. Execution envelope

The envelope is a view, defined by OMEGA-0001. It is filled from the reaction descriptor, the capability view, the active lineage id, the library edges, and the verdict. It is not stored as a new `SemanticKind` and it is not a new `RxObject` type.

```text
what may execute     program_id and realization_id, hashes of different inputs
under whose authority  subject plus RxCapNeed, re-checked by the root
against which generation  object generations on the read and write set,
                         and the lineage id if this work is in a promotion
over which objects   triggers, reads, writes
with which effects   KIND_EFFECT objects reachable from those reads,
                     and only with RX_RIGHT_EFFECT on that resource
within which limits  RxResourceNeed and the capability lease
under which verification  the tier that actually ran, named on the verdict
```

`safety_class` is the one new label. It is an attribute of the realization or of the foreign runtime, not a field the body can raise.

Allowed classes:

```text
Verified
MemorySafe
ManagedRuntime
SandboxedBytecode
NativeConstrained
NativeUnsafe
PhysicalUnsafe
OpaqueFirmware
```

A class may leave the right set as it is, intersect it, require a higher tier, require isolation, or deny the run. The combine rule is intersection and refusal. There is no union with a class-supplied right.

Mapping onto today's code, as documentation of the gap, not as a behavior change:

| Class | Closest fact on main |
|---|---|
| Verified | Parent execution after `verify_stored` passes and the bytes still hash to the id. |
| MemorySafe | No marker in tree. |
| ManagedRuntime | No Python runtime in tree. This is the class a future interpreter must wear. |
| SandboxedBytecode | The forked matvec differential. Not a general bytecode VM. |
| NativeConstrained | A reaction body whose writes are clipped to the descriptor. Same process as the world. |
| NativeUnsafe | `omega_program_exec` and any parent mapping of native bytes without a reaction write set. |
| PhysicalUnsafe | M19 submit and DMA window mapping. |
| OpaqueFirmware | Not represented. A later milestone refuses this class. |

## 7. Authority flow

```text
slot or direct RxCapRef
    -> rx_world_validate_cap
        -> native aienos_cap_validate or rx_caproot_validate
    -> body or seat claim
    -> rx_world_validate_cap again
    -> stage_mutations inside the write set
    -> commit_writes
    -> crumb
```

New authority enters only through `root.install`, which calls the native mint after the origin and domain checks in `rx_aegis.h`. AEGIS policy (`rx_aegis_evaluate`) chooses grant, deny, escalate, or revoke. The root drops anything outside the client ceiling, including every privileged right.

ARGUS, when compiled in, emits after those decisions. `aienos_cap_validate` does not call the observer. The observer is set on the admin handle.

## 8. Security state flow

```text
bytes
    -> realization id
    -> verdict (passed or refused, with a reason)
    -> measure
    -> selection
    -> serve record, when R13 points production at it
    -> promotion request by a different subject
    -> active lineage
    -> crumb and generation blobs
```

`omega.select` publishes a selection. It does not flip the lineage. Production follows the selection only when no serve record is installed. With a serve record, the record is what production reads, and that record is written by the promotion side.

An effect receipt is a record of an external or physical attempt. Writing the receipt object, if a reaction is allowed to write it, is a world publish. It does not mint the next capability.

## 9. CVE reachability

The query shape:

```text
known vulnerable artifact digest
    -> realization ids whose bytes or recorded dependency name that digest
    -> program ids that use those realizations (library lookup and program body)
    -> world objects and generation blobs that carry those ids
    -> live reactions whose read set includes those objects
    -> capability needs those reactions present
    -> KIND_EFFECT objects reachable from that read set
```

Report five booleans or sets, never one score:

| Layer | Meaning |
|---|---|
| Dependency affected | A library edge or `REL_DEPENDS_ON` / `REL_DERIVED_FROM` names the artifact or a program that names it. |
| Code reachable | A realization that hashes to the vulnerable bytes is present in a store or a generation blob. |
| Attacker input reachable | An externally publishable object is in the read set of a reaction that runs that realization. |
| Authority reachable | The rights and resources `validate_caps` would accept for that reaction's subject. |
| Effect reachable | Effect objects on that read set for which the subject also has the effect right. |

`visor_effect_request_classify` is the existing walk for "an effect is reachable." It returns one boolean. The later query returns the ids. On main the library dependencies are program ids, not package digests, so the first hop for a CVE does not exist yet.

## 10. Python and other foreign runtimes

Target invariant:

```text
Python RCE != AIEN compromise
```

That invariant is not true of a Python interpreter linked into the world process, because no such split exists. The design target is:

- The interpreter is `ManagedRuntime`.
- Its only object access is the envelope's read and write sets.
- Its only effects are the envelope's effect list, each still authorized by AEGIS at use.
- It does not receive `HOME` as a capability, `PATH` as a trust root, inherited credentials, a general filesystem, a general network, the TPM, the mint, the AEGIS admin, a writable world pointer, or the accelerator registry.
- Isolation is a later milestone. SECURITY-0 does not add a sandbox, a container supervisor, or a second world to hold the interpreter.

The forked matvec child is evidence that unverified native code can be run off the parent stack. It is not the Python design. The child inherits a copy of process memory. The Linux mint socket is created with `SOCK_CLOEXEC` in `rx_caproot_start`, so that socket is not inherited. The native table is `calloc` memory inside the parent; the child's copy is not the parent's table.

## 11. Secretless execution

```text
untrusted realization
    -> typed effect request (VisorEffectRequest or KIND_EFFECT, authorized false)
    -> AEGIS authorization (policy, then root.install / validate of the effect right)
    -> Effect Broker in aien-mcp, the only holder of the credential
    -> external operation
    -> receipt object, published back through the world if the broker's subject is allowed to write it
```

Ownership:

| Piece | Owner |
|---|---|
| Effect meaning, bounds, canonical id | Omega |
| Decision to allow this subject this effect | AEGIS, then the capability root |
| Credential bytes and the protocol call | Effect Broker (`aien-sovereign-core` crate `aien-mcp`), consistent with ARCH-0005 and ARCH-0006 |
| Receipt | Evidence, written by a subject that holds write on that object |

Omega does not gain a credential cache. A realization that needs an authenticated call presents an effect id. The broker performs the call in its own address space and returns a receipt. Replay uses the existing broker rule: a completed effect id is not sent twice. That rule is not implemented in Omega, and this milestone does not move it here.

`omega_accel_port_bind_machine_graph` is the counterexample to keep out of this path.

## 12. Security nucleus

Formal work, when it is funded, covers the rows below. It does not cover synthesis, the Visor formatter, matvec quality, or the GPU encoder.

| Portion | What to demand later | Why this portion |
|---|---|---|
| `aienos_cap_validate` and `rx_caproot_validate` | Formal spec and model checking: stale generation, subject, resource, rights subset, revoked ancestor, expired lease, no amplification, privileged rights not delegated, generation exhaustion | Every other check calls this. |
| `rx_gen_promote` subject rule and right match | Property tests plus a model of the proposer/promoter split | This is the self-promotion ban. |
| `run_one` publish sequence | Property tests: revoked during the run, stale field version, write outside the set, atomicity of `commit_writes` | This is the semantic commit. |
| `root.install` origin and ceiling | Model checking of writer identity and the privilege mask | This is how new rights appear. |
| Effect payload v2 decode | Property tests already named by `spec/effect-cap64-migration.md`, kept as the contract | A truncated generation becomes a stale or colliding name. |
| Crumb verify | Property tests on parent links | Evidence that does not check is not evidence. |

V5 stays a stub until one of these contracts is small enough to carry a proof. Candidate code does not prove itself. `proofs_ok` on a generation candidate is an input the promoter checks, not a right the candidate holds.

## 13. Resident reaction compatibility

The view is computed inside admission and inside `run_one`. It does not subscribe, wake, or order reactions. Readiness remains "inputs changed and admission fit."

The R16 order stays:

```text
state publication
    -> dependency readiness
    -> resource admission
    -> authority validation
    -> reaction execution
    -> atomic publication
    -> new readiness
```

Faculties stay reactions. AEGIS is not called per action on the fast path. ARGUS does not become a gate in front of validate. The graphics seat remains a worker on this world's objects. M19's registry is physical residue until a later milestone folds it into that seat. SECURITY-0 does not do that fold, and it does not add a supervisor that would itself be a second world.

Quarantine, recovery, the known-good realization, and the generation store stay. Containment that deletes evidence to look simpler is out of scope.

## 14. Non-goals

- A sandbox manager, container runtime, vulnerability daemon, or central security orchestrator.
- A security service loop or a second scheduler.
- Implementing the Python isolator.
- Moving credentials into Omega.
- Formally verifying all of Omega.
- Scoring risk.
- Claiming R8, R12 silicon, R16, or a host qualification receipt. Those gates have their own evidence. This document only describes the sources on the baseline commit.
- Editing `OmegaAcceleratorWorld`, the effect encoder, or `rx_omega` in this milestone.

## 15. Implementation sequence

Each step is a later milestone. None of them is authorized by merging this document.

| Step | Change | Holds |
|---|---|---|
| SECURITY-1 | A pure C function fills an envelope view from an existing descriptor, verdict, and budget. Tests ENV-1, ENV-2, ENV-3. | No new object kind. The function cannot mint. |
| SECURITY-2 | Safety class on the resident realization store. `NativeUnsafe` and `OpaqueFirmware` refuse parent execution. Unverified bytes keep using the existing fork. | Class narrows only. |
| SECURITY-3 | Reachability query over the library, relations, world fields, and crumbs. Five result sets. ENV-6. | No score, no permission change. |
| SECURITY-4 | Effect path audit: stop treating `is_physics_authorized` as authority; reject 32-bit generations on any new intent. ENV-5, ENV-8. | Broker and vault stay outside Omega. |
| SECURITY-5 | Foreign-runtime contract and ENV-7, still without a Python interpreter. | Managed runtime subject has no mint and no promote. |
| SECURITY-6 | Nucleus property tests called out as NUC-1, reusing the R7 and R9 oracles. | No new root. |
| Later, already sequenced by R12 and R16 | One graphics participant in `RxWorld`. Retire central semantic orchestration outside this repository. | Not a SECURITY milestone and not a second world. |

## 16. Acceptance tests

SECURITY-0 acceptance is review of these four documents against the baseline sources.

| Check | Result required before this milestone is called done |
|---|---|
| Envelope is a view | OMEGA-0001 says so, and no new `SemanticKind` or world type was added. |
| Class cannot grant | The combine rule is intersection and refusal. |
| Negative list | The ten rejections in the threat model are stated, and this design uses none of them. |
| Code matches the claims in section 1 | A reviewer can find each function named above on `c0edef0`. |
| No runtime edit | The diff is these documents and this ADR. |

Later behavioral tests are ENV-1 through ENV-8 and NUC-1 in the threat model. They were not added here, because adding the view type would start SECURITY-1.

## Unresolved contradictions

1. **Two accelerator worlds.** `rx_resident_gpu` participates in `RxWorld`. `OmegaAcceleratorWorld` still has its own epoch, generations, and handles. R12's direction is one identity space. Main still contains both. SECURITY-0 does not pick a patch.
2. **Two effect generations.** Canonical effects are 64-bit. `OmegaEffectIntent` is 32-bit. The cap64 migration spec did not change the accelerator client struct.
3. **`is_physics_authorized`.** A machine-graph flag is set true without AEGIS. Leaving it contradicts secretless, authorized effects. Changing it is SECURITY-4, not this commit.
4. **Verified code and the world share a process.** The semantic checks hold. A memory exploit in the parent does not. The documents record that gap and do not pretend the fork closes it for verified code.
5. **ADR home.** Cross-repo decisions live in `aien-architecture` as ARCH-00nn. This record is OMEGA-0001 so it can be reviewed with the Omega sources. It does not edit ARCH-0016. Promoting the text into the architecture repository is a separate acceptance.
6. **R16 is not on main.** The order in section 13 is taken from the R16 brief on `r16/w2-w4`, not from a merged gate.
7. **ARGUS pin versus default build.** The lock names an ARGUS commit. The default reaction test does not link ARGUS. "ARGUS observed nothing" is not evidence when the build left it out.
8. **Object generation width.** World objects retire at 32 bits. Capabilities and lineage ids are 64 bits. The envelope keeps them in separate fields so a later patch does not truncate one into the other.
