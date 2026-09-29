# OMEGA-SYSTEMS-CORE-0000: Contract and machine semantics freeze (OSC-0 / OSC-0B)

- Status: **Proposed, awaiting review.** Not frozen until accepted. The items marked "needs Drake" stay open until he decides.
- Date: 2026-09-29
- Location: this ADR lives in Omega, next to the language it governs. The cross-repo doctrine ADRs stay in aien-architecture; this record points to them and does not duplicate them.
- Full text and evidence: [OMEGA_SYSTEMS_CORE_CODE_AUDIT.md](../../OMEGA_SYSTEMS_CORE_CODE_AUDIT.md), with appendices in [docs/osc/audit/](../osc/audit/).

## Context

Omega, PHYSICS, and AIENOS rely on about 98k lines of C and assembly whose safety depends on convention. Omega Systems Core is to replace them. The goal is C-class control without C's unsafe responsibility model. Drake set the sequence: audit, then a hard machine-semantics freeze, then the compiler slice, then the substrate and first migration.

## Decision (proposed)

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
- **Would amend (needs Drake):**
  - AIENOS ADR 0013, u32 generations → u64;
  - the README statement that Omega "is not a programming language/compiler".

## Consequences

- There will be identity breaks: the canonical payloads, the R9 store rows, and the crumb digests. Each one gets a version bump and a separate reviewed change (OSC-2+).
- Small C fixes are listed in audit I.8 and are not done under this ADR.
- The OSC-1 slice may start only after this ADR is accepted and the II.11 model passes.
