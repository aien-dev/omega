# Specification: Omega Self-Hosting Compiler (`OMEGA_SELF_HOST`)

```text
Document ID:     SPEC-OMEGA-M6
Milestone:       Milestone 6 (OMEGA_SELF_HOST)
Classification:  Sovereign Machine Canonical Specification
Target Substrate: Native AArch64 Machine Code Reproducing Compiler from Semantic Graph
Lineage:         SILICON -> ATLAS (M1) -> PHYSICS (M2/M3) -> OMEGA (M4/M5/M6) -> AIEN
```

---

## 1. Executive Summary & Foundational Invariants

Milestone 6 establishes the closed-loop reproduction of the realization compiler itself:

> **OMEGA REPRODUCES ITS MINIMAL REALIZATION COMPILER THROUGH ITS OWN SEMANTIC GRAPH.**

The system reaches self-hosting closure when the compiler is an explicit semantic object $G_C \in \text{OmegaGraph}$ that compiles itself into native machine code, achieving an exact fixed point:

```text
C0 = temporary reference C implementation
G_C = OMEGA semantic definition of minimal compiler

C0(G_C)
   ↓
  C1 (native AArch64 machine code)

C1(G_C)
   ↓
  C2 (native AArch64 machine code)

C2(G_C)
   ↓
  C3 (native AArch64 machine code)

Required Fixed Point:
C1 == C2 == C3  (bit-for-bit, byte-for-byte identity)
```

---

## 2. Inviolable Governance Principles at M6

1. **No Foreign Compiler Dependency**:
   Neither LLVM, Clang, GCC, nor any external assembler is used to generate $C_1$, $C_2$, or $C_3$.

2. **Deterministic Bit-for-Bit Fixed Point**:
   $$\text{SHA256}(C_1) == \text{SHA256}(C_2) == \text{SHA256}(C_3)$$
   $$\text{REALIZATION\_ID}(C_1) == \text{REALIZATION\_ID}(C_2) == \text{REALIZATION\_ID}(C_3)$$

3. **Compiler Capability Completeness**:
   The semantic compiler $G_C$ contains the full semantic pipeline:
   - OMG0 decoding
   - Type validation
   - Semantic lowering & register allocation
   - AArch64 instruction synthesis
   - Branch layout & fixups
   - `REALIZATION_ID` cryptographic binding

4. **Preservation of Qualified Semantics (M5 Parity)**:
   $C_1(G_S) == C_0(G_S) == \text{00 00 01 8B 00 00 02 CB C0 03 5F D6}$, executing to 15.
