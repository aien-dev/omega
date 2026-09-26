# SPECIFICATION: The OMEGA Semantic Object Model

```text
Document ID:     SPEC-OMEGA-OBJECT-M4
Classification:  Sovereign Semantic Specification
Target Substrate: Content-Addressed Directed Graph (G_S)
Status:          RATIFIED FOR M4 (OMEGA_SEMANTICS)
```

---

## 1. Semantic Universe Categories

Milestone 4 defines eleven first-class semantic object categories:

```text
VALUE
TYPE
OPERATION
RELATION
CONSTRAINT
MEMORY
MACHINE
EFFECT
REALIZATION
EVIDENCE
PROOF
```

These categories are ontological primitives of meaning. In any host implementation (such as this bootstrap C scaffolding), host data structures represent temporary computational vessels, not the semantic definition itself.

---

## 2. Canonical Object Model

Every entity in OMEGA is an `OMEGA_OBJECT` in the directed semantic graph $G_S = (V, E)$:

```text
OMEGA_OBJECT {
    semantic_id:  SemanticId (32 bytes SHA-256)
    kind:         SemanticKind (1 byte tag)
    attributes:   Map<String, Value> (Lexicographically sorted)
    relations:    SortedSet<SemanticRelation>
    constraints:  SortedSet<SemanticConstraint>
    payload:      SemanticPayload (Kind-specific content)
}
```

### Reference by Semantic Identity
- Pointers inside OMEGA do not reference host virtual memory addresses (`0x7fff...`).
- All cross-object references use the 32-byte cryptographic `SemanticId`.
- An object graph is fully relocatable across processes, machines, network fabrics, and storage media without pointer patching or swizzling.

---

## 3. Pure Computation vs Physical Effects

OMEGA maintains an unyielding division between pure mathematical meaning and physical effects:

1. **Pure Semantics**:
   Mathematical definitions whose evaluation involves zero interaction with the physical universe (e.g. integer arithmetic, boolean logic, bit-slicing). Pure semantic objects require zero capability authority.

2. **Physical Effects (`OMEGA_EFFECT`)**:
   Descriptions of intended physical interactions (e.g. console telemetry, physical memory frame grants, hardware telemetry readouts).
   An `OMEGA_EFFECT` object **does not execute the effect**. It defines:
   - Target resource class
   - Operation semantics
   - Pre- and post-conditions
   - Expected invariants
   - Required capability reference

Lowering from an `OMEGA_EFFECT` to execution is performed by submitting an `EFFECT_INTENT` to the Physics M3 Effect Broker, which validates the kernel-private `CAPABILITY` and records an authoritative `EFFECT_RECEIPT`.
