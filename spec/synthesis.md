# Specification: Omega Synthesis V0 (`OMEGA_SYNTHESIS_V0`)

```text
Document ID:     SPEC-OMEGA-M9
Milestone:       Milestone 9 (OMEGA_SYNTHESIS_V0)
Classification:  Sovereign Machine Canonical Specification
Target Substrate: Deterministic Typed Program Synthesis over Base Primitives
Status:          SPECIFIED / IN PROGRESS (aien-dev/omega#15)
Lineage:         SILICON -> ATLAS (M1) -> PHYSICS (M2/M3) -> OMEGA (M4-M9) -> AIEN
```

---

## 1. Executive Summary

Milestone 9 introduces the first sovereign synthesis engine:
- Searches over base primitives (arithmetic, bitwise, constants) using bottom-up enumerative search.
- Search ordering is strictly monotonic across depth and program cost.
- Fail-closed type pruning rejects ill-typed compositions at contract unification.
- Canonical observational equivalence pruning deduplicates semantically identical candidates via behavioral probe vectors.
- Candidate programs are untrusted until verified through M7 tiers ($V_0$ structural, $V_1$ input/output test suite, $V_2$ invariant checks).
- Discovers target transformations (such as $f(x) = 2x + 1$ and $f(x) = 3x - 2$) from examples without receiving implementations.
- Generates an immutable cryptographic qualification receipt.
