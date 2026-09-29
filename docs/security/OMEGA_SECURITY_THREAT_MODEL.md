# Omega security threat model

SECURITY-0. Baseline `c0edef0337e7d27150eb7274ff13bbb10cba3021`.

Decision: OMEGA-0001. Boundaries: `OMEGA_SECURITY_TRUST_BOUNDARIES.md`.

## 1. Assets

- The capability table and the office token.
- The promotion right and the active lineage pointer.
- Canonical object fields and their generations.
- Effect credentials, which Omega does not store. The asset lives with the Effect Broker and the vault.
- Realization bytes and the identity those bytes must hash to.
- Crumbs, generation blobs, and receipt seals.
- The graphics seat's claim on a placed object.

## 2. Adversaries

Each of these is assumed able to run attacker-chosen bytes inside its own component. None is assumed to start with the capability admin.

| Adversary | Examples | Intended blast radius |
|---|---|---|
| Managed runtime | A future Python interpreter, a Skill runtime | Objects and effects named on its envelope. No mint, no promotion, no credential bytes. |
| Generated native code | Resident matvec candidate, `omega_program_exec` | The write set of the reaction that invoked it, or the tool process if it is on the tool path. |
| Parser | Canonical decoder, Visor command parser | Refuse or produce an object. No grant. |
| Accelerator worker | Resident seat, M19 channel | The claimed object, or the M19 buffer the handle names. A reset does not retarget object identity. |
| Model | Synthesis, a plan object | Candidate and plan objects it can write. Selection and promotion stay on other subjects. |
| Compromised Omega faculty | A reaction linked into the world process | Semantic damage is the write set. Memory damage is the whole process, because the host reference has no second address space for a faculty. |
| ARGUS consumer | Findings, rings | Observations. No table writes. |

Out of scope for this model: a hostile kernel, a hostile CPU, or physical access to the machine. Opaque firmware is named as a safety class so a later milestone can refuse it. This milestone does not analyze device firmware.

## 3. Invariants the design is held to

```text
exploit != authority
authority != effect
effect != committed state
```

In the mechanisms that exist today:

- A reaction that returns proposals still fails publication when the capability check fails or the write is outside the set (`run_one`).
- A live capability reference is not an effect. `RX_RIGHT_EFFECT` is a bit the root must accept. The Visor request built from a `KIND_EFFECT` object stays unauthorized.
- A submitted accelerator intent and a drained M19 dispatch are not a lineage promotion. `rx_gen_promote` is the promotion path, and the proposer is refused.

## 4. Current refusals

These are implemented on main. This document does not re-certify the qualification receipts.

| Attack | Refusal |
|---|---|
| Forged, stale, wrong-subject, wrong-resource, amplified, or revoked reference | `rx_world_validate_cap` / native validate |
| Privileged right delegated | Root refuses; the right includes promote |
| Candidate promotes itself | `rx_gen_promote` returns `RX_GEN_ERR_AUTHORITY` when the subject is the proposer |
| Policy grants outside the client domain | `root.install` refuses with the domain reason and does not mint |
| Decision or request written by the wrong faculty | Origin check on field writers |
| Slot stuffed with another subject's reference | Client subject does not match; the reaction stays blocked |
| Stale object generation, torn descriptor, unbound physical window | Publication and descriptor checks |
| Legacy 176-byte effect payload | Canonical decoder refuses version 1 effects |
| Visor treats a request as granted | `authorized` is constant false; no setter is exported |
| Unverified matvec disagrees, crashes, hangs, or writes outside the output | Forked differential; parent does not mark the bytes verified |
| Oscillation, livelock, conflict, or activation budget | Quarantine crumb. Quarantine does not revoke the capability. |

## 5. Gaps

| Gap | Why it matters |
|---|---|
| No envelope view | The allowed objects, effects, and bounds are spread across the descriptor, the lease, and the verdict. A reviewer cannot read one record. |
| No safety class | Verified parent execution, forked differential, and a future Python runtime are not distinguished by a label that can only shrink rights. |
| Verified native code runs in the world process | A memory fault after a passing verdict can reach pointers the semantic checks do not see. |
| `OmegaAcceleratorWorld` is a second registry | Its epoch and generation are not the resident object's generation. An `OmegaHandle` can be mistaken for authority. |
| `omega_accel_port_bind_machine_graph` sets `is_physics_authorized` | The receipt digest check in that file does not authenticate the producer. The flag is not an AEGIS decision. |
| `OmegaEffectIntent.capability_generation` is 32 bits | A live AIENOS generation does not fit. The canonical effect object already uses 64 bits. |
| Library dependencies are program ids | `OmegaLibraryEntry.dependency_ids` does not name a package, a file digest, or a CVE. `REL_DEPENDS_ON` is used by a few builders, not as a vulnerability index. |
| No query from a live reaction to the effects it could reach | Crumbs record the capability references a run presented. They are not indexed by artifact. |
| ARGUS is off unless the build asks for it | Default `RX_SRCS` do not include `rx_argus.c`. Absence of findings is not a clean bill. |
| Tool execution | `omega_program_exec` does not pass through `RxWorld`. |
| Credentials | No vault and no broker in this repository. Secret resolution in other repositories is a separate defect and is not fixed here. |

## 6. Reachability questions

For a known-vulnerable artifact the graph must eventually answer each line on its own. A single score is not an answer.

| Question | Where the facts are today | What is missing |
|---|---|---|
| Dependency affected | Library `dependency_ids`; relation kind `REL_DEPENDS_ON` | A digest for the foreign artifact, and a walk that records depth |
| Code reachable | Realization id hashed from bytes; program id hashed from body and contract; library lookup by realization id | A link from a third-party package to those ids |
| Attacker input reachable | External publication (`rx_world_publish_external`) and objects the realization reads | A recorded path from that publication to the realization's read set |
| Authority reachable | Caps stamped on the crumb; validate needs subject, resource, and rights | A query that unions the rights a subject could still validate |
| Effect reachable | `visor_effect_request_classify` walks to `KIND_EFFECT` and capability or effect types | That walk is a boolean ("needs authority"), not the list of effects and not a grant |

Maximum authority blast radius is the union of the last two rows for subjects whose running realizations fall in the code-reachable set. It is a set of resources, rights, and effect ids. It is not a number.

## 7. Negative tests the design fails if it requires them

The architecture is rejected if the only way to satisfy a later milestone is:

1. A new thread or loop that chooses semantic order across AIEN, Omega, and AEGIS.
2. An ARGUS finding that mints, revokes, or widens a reference.
3. A model, Skill, or plan object that holds `RxCapAdmin`, the office token, or `RX_RIGHT_PROMOTE`.
4. A Python workload whose environment contains a credential, a vault token, or an ambient home directory used as authority.
5. A numeric risk score that adds or removes a right.
6. A vulnerability scan that is treated as `validate_caps`.
7. A CPU pointer or a GPU virtual address stored as a `SemanticId` or an `RxObjRef`.
8. A second object table that can commit a canonical field without `commit_writes` on `RxWorld`.
9. An external call that runs from a realization without an AEGIS decision and a capability check.
10. A promotion where `request->subject` equals the candidate proposer.

## 8. Acceptance tests for later milestones

Not run in SECURITY-0. No C type was added.

| Id | Pass condition |
|---|---|
| ENV-1 | A pure function builds the view from a reaction descriptor, a verdict, and a resource need. The rights in the view are a subset of the rights `validate_caps` would check. |
| ENV-2 | Each safety class, applied to a fixed descriptor, produces a right mask that is equal to or narrower than the input mask. The test includes a class that refuses `NativeUnsafe` and `OpaqueFirmware`. |
| ENV-3 | A view whose program id was computed with a realization mixed into the hash does not match `omega_program_compute_id`. Realization stays outside program identity. |
| ENV-4 | Promotion still fails when the promoter subject equals the proposer, with the view present and ignored as authority. |
| ENV-5 | A `VisorEffectRequest` built after the view exists still has `authorized == false`. |
| ENV-6 | Five queries on one fixture return five answers: dependency, code, attacker input, authority, effect. No query returns a score. |
| ENV-7 | A managed-runtime subject whose descriptor asks for mint or promote fails the view check before `fn` runs. |
| ENV-8 | The accelerator intent used by any new code carries a 64-bit generation. A 32-bit generation does not compare equal to a live reference. |
| NUC-1 | Property tests cover stale generation, amplification, revoked ancestor, exhausted generation, and self-promotion, by calling the existing root and `rx_gen_promote`. |
