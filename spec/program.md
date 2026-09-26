# Specification: Omega Program Core (`OMEGA_PROGRAM_CORE`)

```text
Document ID:     SPEC-OMEGA-M8
Milestone:       Milestone 8 (OMEGA_PROGRAM_CORE)
Classification:  Sovereign Machine Canonical Specification
Target Substrate: Explicit Program Objects, Contracts, Cost Accounting, and Composition
Status:          SPECIFIED / IN PROGRESS (aien-dev/omega#14)
Lineage:         SILICON -> ATLAS (M1) -> PHYSICS (M2/M3) -> OMEGA (M4-M8) -> AIEN
```

---

## 1. Executive Summary

Milestone 8 introduces the first-class `OMEGA_PROGRAM` and `SYNTHESIS_TASK` objects:
- Every program has an explicit input contract (types, bounds), output contract, and formal cost.
- Programs compose algebraically: $C = A \circ B$.
- Intermediate types are unified: $\text{Type}(A_{\text{out}}) == \text{Type}(B_{\text{in}})$.
- Costs compose monotonically: $\text{Cost}(C) = \text{Cost}(A) + \text{Cost}(B)$.
- Lowering produces verified native AArch64 code verified through M7 tiers ($V_0, V_1, V_2$).
