# OMEGA-SYSTEMS-CORE-0000: Contract and machine semantics freeze (OSC-0 / OSC-0B)

- Status: **Accepted 2026-09-29 (Drake). Frozen.** The four items that needed Drake are decided below ("Decisions 2026-09-29").
- Date: 2026-09-29
- Location: this ADR lives in Omega, next to the language it governs. The cross-repo doctrine ADRs stay in aien-architecture; this record points to them and does not duplicate them.
- Full text and evidence: [OMEGA_SYSTEMS_CORE_CODE_AUDIT.md](../../OMEGA_SYSTEMS_CORE_CODE_AUDIT.md), with appendices in [docs/osc/audit/](../osc/audit/).

## Context

Omega, PHYSICS, and AIENOS rely on about 98k lines of C and assembly whose safety depends on convention. Omega Systems Core is to replace them. The goal is C-class control without C's unsafe responsibility model. Drake set the sequence: audit, then a hard machine-semantics freeze, then the compiler slice, then the substrate and first migration.

## Decision

1. **The replacement for the pointer is an object relationship.** Lifetime, authority, and placement are tracked independently (audit II.12).
2. **The reference taxonomy is fixed.** `Handle<T>` (live) and `SemanticId<T>` / `PersistentRef<T>` (durable) are distinct types. Live handles are statically barred from canonical and durable encoders (II.6).
3. **Concurrency.** No data races in safe code. Publication is only a release/acquire pair. Device completion is a linear `Fence` value, and reclaim requires a `Quiesced` proof (two-phase revocation) (II.1).
4. **Ordering.** Omega owns its ordering vocabulary, with explicit AArch64 lowerings. `volatile` is banned from safe code (II.2).
5. **Four layouts: Semantic, Runtime, Wire, Device.** Raw struct hashing is forbidden, and every hashed byte comes from a declared encoder (II.3).
6. **Failures.** Typed `Result` for expected failures; deterministic traps for invariant violations; no unwinding. A per-context table covers OOM, bounds, stale generations, device loss, failed construction, and cleanup (II.4).
7. **Ownership is a forest.** Graphs are owned by pools, and their edges are handles, weak references, or ids. Reference counting is never the default (II.5).
8. **Movable by default.** `pin<T>` is rare and physical (II.7). MMIO, DMA, and cache visibility go through typed primitives inside declared `unsafe physical` blocks. The `ctx(irq)` / `ctx(trap)` contexts forbid allocation and blocking (II.8).
9. **Reproducible compiler.** Build twice and compare, and bootstrap equivalence is checked on a fixed corpus. The M6 self-copy is never evidence (II.9).
10. **Backend.** The syntax extends V0. The backend is direct AArch64 through the hardened in-repo encoder. C output compiled by system cc is only a differential oracle (III.6).
11. **Exit gate.** OSC-0B exits through an executable C reference model of the memory rules, driven by seeded random sequences (II.11).

## Relation to settled records

- **Follows:** ADR 0014, where authority lives in AIENOS and PHYSICS/FORGE does realization only. So the PHYSICS M3 ledger becomes legacy.
- **Follows:** native-frame-authority.md §6.4 (reclaim requires quiescence).
- **Follows:** canonical-encoding.md (big-endian identity) and the shared-world ABI (little-endian Zero Serialization) as two different layouts.
- **Records:** omega#71 (u64 effect generations, v2 encoder).
- **Amends (Drake, 2026-09-29):**
  - AIENOS ADR 0013, u32 generations → u64 (decision 1; amendment note below);
  - the README statement that Omega "is not a programming language/compiler" (decision 2; retired).

## Decisions 2026-09-29

Drake accepted the recommendations on 2026-09-29. These close audit items C1, C8, III.6, and II.6.

1. **C1, capability generation width: u64 end to end.** A slot whose generation reaches the maximum is retired, never wrapped. This covers the Rust kernel remnants, the shared-world descriptor (:130/132), `src/omega_accelerator.h`, and PHYSICS M2/M3/M15, on top of the Omega effects already moved by omega#71.
   - **Amendment note to AIENOS ADR 0013.** ADR 0013 specifies u32 capability generations. This decision amends it to u64 end to end, slot retired at max. The edit to the AIENOS repository is follow-up work **OSC-2**; AIENOS is not changed by this ADR.
2. **C8, README doctrine: retired.** Omega is the reaction runtime **and** the compiler for Omega Systems Core. README.md now says so. The M6 "self-host" result was a fixed-output self-copy check, not compilation, and is never evidence of a compiler (II.9).
3. **III.6, `requires` / `ensures`: text only in OSC-1.** They are parsed and recorded, not checked. Enforcement is OSC-2 and is **MANDATORY**. **Rule: no production C migrates into Omega Systems Core until contract enforcement is on.**
4. **II.6, identity break: scheduled for OSC-2, right after the compiler slice.** It is a versioned transition: a new canonical version byte, and old records are refused, never reinterpreted (the omega#71 effect-cap64 pattern).
   - **Constraint (TURING records).** TURING records (`src/turing`, domains `turing.contract.v0.provisional`, `turing.spec.v0`, `turing.evidence.v0`, `turing.decision.v1`, `turing.citeset.v0`) hash `omega_canonical_encode` OMG0 bytes. Therefore any OMG0 encoder change, including the C2 big-endian move, must ship together with a TURING record version bump and a verification path for existing golden digests: the `tests/turing` golden and rebuild tests must keep passing on old records. See TURING brief sections 9 and 49.

**Additional rule (R16 ordering).** The OSC-1 compiler slice may not edit `src/runtime/` until the R16 gate is done. As of 2026-09-29, R16 is IN PROGRESS with only G1/G2 evidenced.

## Consequences

- There will be identity breaks: the canonical payloads, the R9 store rows, and the crumb digests. Each one gets a version bump and a separate reviewed change (OSC-2+).
- Small C fixes are listed in audit I.8 and are not done under this ADR.
- The OSC-1 slice may start only after this ADR is accepted and the II.11 model passes.
