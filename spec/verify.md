# Specification: Omega Trusted Verification Engine (`OMEGA_VERIFY`)

```text
Document ID:     SPEC-OMEGA-M7
Milestone:       Milestone 7 (OMEGA_VERIFY)
Classification:  Sovereign Machine Canonical Specification
Target Substrate: Multi-Tier Trusted Verification Ladder (V0, V1, V2)
Status:          SPECIFIED / IN PROGRESS (aien-dev/omega#13)
Lineage:         SILICON -> ATLAS (M1) -> PHYSICS (M2/M3) -> OMEGA (M4/M5/M6/M7) -> AIEN
```

---

## 1. Executive Summary & Verification Law

> **AIEN MAY PROPOSE. OMEGA MUST VERIFY. PHYSICS MUST AUTHORIZE. EVIDENCE DECIDES WHAT SURVIVES.**

All program candidates, lowering sequences, and realization artifacts produced by search procedures, heuristics, or external models are treated as **strictly untrusted**.
No candidate artifact is retained, evaluated, or committed to the persistent state ledger without passing the mandatory verification ladder:

```text
V0: Structural Verification (types, DAG, bounds, opcode validity, terminal RET)
V1: Differential Verification (semantic reference eval vs native realization execution)
V2: Property / Invariant Verification (commutativity, identity, associativity, overflow wrapping)
```

---

## 2. API Contract

The verification engine exposes a unified interface:

```c
typedef enum {
    VERIFY_TIER_V0 = 0, /* Structural */
    VERIFY_TIER_V1 = 1, /* Differential */
    VERIFY_TIER_V2 = 2, /* Property / Invariant */
    VERIFY_TIER_V3 = 3, /* Adversarial (stub) */
    VERIFY_TIER_V4 = 4, /* Symbolic (stub) */
    VERIFY_TIER_V5 = 5  /* Proof-Carrying (stub) */
} VerifyTier;

typedef struct {
    bool passed;
    VerifyTier tier;
    uint32_t check_count;
    uint32_t fail_count;
    char error_detail[256];
} VerifyReport;

/* V0 Structural Verification */
int omega_verify_v0_structural(const OmegaGraph *graph, const RealizationObject *real, VerifyReport *report);

/* V1 Differential Verification */
int omega_verify_v1_differential(const OmegaGraph *graph, const RealizationObject *real,
                                 const uint64_t *test_inputs, size_t input_count,
                                 VerifyReport *report);

/* V2 Property Verification */
int omega_verify_v2_properties(const OmegaGraph *graph, const RealizationObject *real, VerifyReport *report);

/* Master Verification Pipeline */
int omega_verify_pipeline(const OmegaGraph *graph, const RealizationObject *real, VerifyTier max_tier, VerifyReport *report);
```
