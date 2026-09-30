# OMEGA — Sovereign Semantic Substrate

```text
Milestone:       MILESTONE 4 — OMEGA_SEMANTICS
Lineage:         SILICON -> ATLAS (M1) -> PHYSICS (M2/M3) -> OMEGA (M4+) -> AIEN
Qualification:   QUALIFIED / 12 OF 12 GATES PASS
Receipt:         evidence/omega_qualification_receipt.json
Repository:      https://github.com/aien-dev/omega
```

---

## 1. Executive Summary

Milestone 4 establishes the first executable semantic substrate of OMEGA:

> **OMEGA DEFINES WHAT COMPUTATION MEANS WITHOUT COMMITTING THAT MEANING TO A PARTICULAR MACHINE REPRESENTATION.**

OMEGA is the reaction runtime and the compiler for Omega Systems Core. It is the smallest sovereign semantic system capable of representing computation independently of the physical machine that will eventually realize it, and it compiles Omega Systems Core programs into that machine. (Decided by Drake, 2026-09-29, in [OMEGA-SYSTEMS-CORE-0000](docs/adr/OMEGA-SYSTEMS-CORE-0000.md); this retires the earlier "not a programming language or compiler" doctrine. The Milestone 6 "self-host" result was a fixed-output self-copy check, not compilation.)

### The Sovereign Axioms
- **`SMART ≠ TRUSTED`**: Trust is not earned through cognitive capability, model scale, or heuristic brilliance. Trust is binary and verified through auditable invariant enforcement.
- **`INTELLIGENCE ≠ AUTHORITY`**: AIEN cognitive agents will explore semantic spaces and propose abstractions, but intelligence possesses zero authority. Physical effects are exclusively mediated by PHYSICS via capability-bounded, receipt-producing transactions.
- **`MEANING IS PERMANENT; REPRESENTATION IS DISPOSABLE`**: Non-semantic representation variations (textual syntax, builder order, memory addresses, serialization formats) collapse into a unique content-addressed semantic identity (`SEMANTIC_ID`).

---

## 2. Primary Invariant

$$\text{SEMANTICALLY\_EQUIVALENT}(A, B) \implies \text{SEMANTIC\_ID}(A) = \text{SEMANTIC\_ID}(B)$$
$$\text{SEMANTICALLY\_DIFFERENT}(A, B) \implies \text{SEMANTIC\_ID}(A) \neq \text{SEMANTIC\_ID}(B)$$

Derived strictly through the canonicalization pipeline:

```text
RAW REPRESENTATION
       ↓
PARSE / CONSTRUCT
       ↓
VALIDATE
       ↓
CANONICALIZE SEMANTICS
       ↓
CANONICAL SEMANTIC ENCODING ("OMG0")
       ↓
SEMANTIC_ID = SHA256(canonical_bytes)
```

---

## 3. Repository Structure

```text
omega/
    doctrine/
        README.md             - Core sovereign axioms and canonical stack
    spec/
        semantic-object.md    - The 11 first-class semantic categories
        type-system.md        - The bounded type system
        canonical-encoding.md - Deterministic binary serialization and hashing rules
    src/
        sha256.h / sha256.c   - Standalone NIST FIPS 180-4 SHA-256 implementation
        omega_types.h         - Bootstrap C types (marked non-permanent definition)
        omega_canonical.h/.c  - Lexicographical sorting and canonical encoding
        omega_validate.h/.c   - Graph validation, bounds, DAG cycle checking
        omega_core.h/.c       - Semantic builders, relations, constraints, evaluation
        omega_codec.h/.c      - Binary wire codec and non-canonical text parser
    tools/
        omegatool.c           - Qualification harness and demonstration runner
    tests/
        identity/             - Representation independence tests
        malformed/            - Malformed corpus and cycle refusal tests
        canonicalization/     - Binary canonicalization tests
        relations/            - Directed relation tests
        constraints/          - Invariant envelope and authority law tests
        run_m4_gates.sh       - Master qualification runner
    evidence/
        test_vectors/         - Binary and textual test vectors
        corpus_digests.txt    - SHA-256 digests of all specs, source files, and vectors
        omega_qualification_receipt.json - Formal cryptographic qualification receipt
    Makefile                  - POSIX C99 build rules (-Wall -Wextra -Werror -pedantic)
    README.md
```

---

## 4. Central Demonstrations

### Demonstration 1: Pure Arithmetic Equivalence Across 4 Forms
Computation: $\text{ADD}(a = \text{U32}(7), b = \text{U32}(11)) \implies \text{U32}(18)$
- Builder Ordering A: `674c6d710c35de930e49454618b3bbc63d5d1d0ed0506fc0181365bd050e9870`
- Builder Ordering B: `674c6d710c35de930e49454618b3bbc63d5d1d0ed0506fc0181365bd050e9870`
- Binary Wire Decoder: `674c6d710c35de930e49454618b3bbc63d5d1d0ed0506fc0181365bd050e9870`
- Textual Parser: `674c6d710c35de930e49454618b3bbc63d5d1d0ed0506fc0181365bd050e9870`
- Result: **All 4 representations yield identical SEMANTIC_ID**.
- Operator Mutation (`ADD` $\to$ `SUB`): yields `54e676c014745193a2ec7ec3672c57b584a0653ea8bfb44710e6cbc718eccf23` (**Distinct SEMANTIC_ID confirmed**).

### Demonstration 2: Physics Authority Semantics
The Milestone 3 authority law:
$$\text{child.bounds} \subseteq \text{parent.bounds} \quad \land \quad \text{child.rights} \subseteq \text{parent.rights}$$
- Construction 1 (Bounds then Rights): `e9b290e4505c41cbe09d25a6f4406c81a9a2d932996bb176a89e2c37ec6a403e`
- Construction 2 (Rights then Bounds): `e9b290e4505c41cbe09d25a6f4406c81a9a2d932996bb176a89e2c37ec6a403e`
- Result: **Identical canonical semantic ID**.

---

## 5. Canonical Qualification Gates

```text
[PASS] OMEGA_OBJECT_MODEL_PASS                : 11 first-class categories instantiated and typed
[PASS] OMEGA_TYPE_SYSTEM_PASS                 : Bounded widths enforced, invalid widths refused
[PASS] OMEGA_GRAPH_VALIDATION_PASS            : DAG validation and dangling reference refusal
[PASS] OMEGA_CANONICAL_ENCODING_PASS          : OMG0 wire header and lexicographical attribute sorting
[PASS] OMEGA_SEMANTIC_ID_DETERMINISM_PASS     : Cross-allocation bit-for-bit SHA-256 identity
[PASS] OMEGA_REPRESENTATION_INDEPENDENCE_PASS : 4 independent representations collapse to identical ID
[PASS] OMEGA_SEMANTIC_DIFFERENCE_PASS         : ADD -> SUB produces distinct SEMANTIC_ID
[PASS] OMEGA_RELATION_PASS                    : Relation ordering independence confirmed
[PASS] OMEGA_CONSTRAINT_PASS                  : Constraint ordering independence confirmed
[PASS] OMEGA_PURE_EFFECT_SEPARATION_PASS      : Pure computation decoupled from physical effect tokens
[PASS] OMEGA_MALFORMED_OBJECT_REFUSAL_PASS    : Structural cycles and corrupt wire packets rejected
[PASS] OMEGA_CROSS_BUILD_DETERMINISM_PASS     : Known-answer test evaluation matches specification
```

---

## 6. Build and Verification Instructions

```bash
# Build omegatool
make clean && make

# Run master qualification gate suite
./tests/run_m4_gates.sh

# Run individual demonstrations
./build/omegatool --reference-demonstrate-arithmetic
./build/omegatool --reference-demonstrate-physics
```
