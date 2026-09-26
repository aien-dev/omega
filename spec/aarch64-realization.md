# Specification: Omega Direct AArch64 Realization (`OMEGA_AARCH64`)

```text
Document ID:     SPEC-OMEGA-M5
Milestone:       Milestone 5 (OMEGA_AARCH64)
Classification:  Sovereign Machine Canonical Specification
Target Substrate: Native AArch64 Bare-Metal Machine Bytes (No LLVM, No Foreign Assembler)
Lineage:         SILICON -> ATLAS (M1) -> PHYSICS (M2/M3) -> OMEGA (M4/M5) -> AIEN
```

---

## 1. Executive Summary & Foundational Invariants

Milestone 4 established `OMEGA_SEMANTICS`: defining what computation means in a substrate-independent semantic graph ($G_S$) with canonical content-addressed identity (`SEMANTIC_ID`).

Milestone 5 establishes the first direct lowering from abstract semantic meaning into native physical machine code:

> **OMEGA SYNTHESIZES BARE-METAL AARCH64 MACHINE BYTES DIRECTLY FROM SEMANTIC MEANING WITHOUT LLVM, GCC, OR THIRD-PARTY ASSEMBLERS.**

The objective is to establish direct physical realization while preserving absolute semantic equivalence:

```text
M4 SEMANTIC OBJECT ($G_S$)
F(a, b, c) = (a + b) - c
         ↓
    SEMANTIC_ID S
         ↓
OMEGA AARCH64 REALIZER
         ↓
 RAW MACHINE BYTES B
         ↓
  REALIZATION_ID R (references S)
         ↓
PHYSICAL / QEMU EXECUTION
         ↓
 native_result == semantic_result
```

---

## 2. Inviolable Governance Principles at M5

1. **No Foreign Compiler Infrastructure**:
   LLVM, GCC, Clang, GNU `as`, NASM, and third-party JITs are strictly forbidden in the canonical realization path. Machine instructions must be synthesized directly as 32-bit AArch64 binary words.

2. **Dual-Seam Instruction Verification**:
   - **Seam 1 (Static Decoding Seam)**: An independent direct AArch64 decoder disassembles and validates every emitted instruction byte prior to execution, ensuring structural validity and absence of undefined opcodes.
   - **Seam 2 (Execution Parity Seam)**: The synthesized code is executed on AArch64 (both native DGX Spark and bare-metal QEMU) and verified to yield the bit-for-bit identical result as M4 pure semantic evaluation.

3. **Cryptographic Binding of Realization to Meaning**:
   $$\text{REALIZATION\_ID} = \text{SHA256}(\text{CANONICAL\_REALIZATION\_ENCODING})$$
   The canonical realization encoding explicitly embeds the `SEMANTIC_ID` of the source graph, binding physical machine bytes to abstract meaning.

4. **Adversarial Bit Mutation Gate**:
   Mutating any single bit in the generated machine code must either:
   - Cause immediate rejection by the independent instruction decoder, OR
   - Produce a detectable differential mismatch during execution.

---

## 3. Strict Initial Instruction Subset

Milestone 5 restricts instruction synthesis to a strictly bounded integer computational subset:

### Computational Primitives
1. **64-bit & 32-bit Integer ALU**:
   - `ADD` (register)
   - `SUB` (register)
   - `MUL` (register multiplication)
   - `AND`, `ORR`, `EOR` (bitwise logical operations)
2. **Immediate Loading & Register Movement**:
   - `MOV` / `MOVZ` (wide immediate)
3. **Control Flow & Return**:
   - `RET` (unconditional return to link register `x30`)
   - `B` (bounded relative branch)
   - `B.cond` (conditional branch on integer condition codes)
   - `CBZ`, `CBNZ` (compare and branch on zero / non-zero)

---

## 4. Canonical Realization Format

```text
REALIZATION_OBJECT {
    REALIZATION_ID:      [u8; 32]  (SHA-256 over canonical realization header + code)
    SEMANTIC_ID:         [u8; 32]  (Binds source M4 semantic object)
    TARGET_PROFILE:      "aarch64-baremetal-pure-reg"
    CODE_LENGTH:         size_t
    CODE_BYTES:          [u8; CODE_LENGTH]
}
```

Header layout hashed into `REALIZATION_ID`:
- `REAL` magic (4 bytes: `0x52, 0x45, 0x41, 0x4C`)
- Target Profile ID (u16)
- SEMANTIC_ID (32 bytes)
- Code Length (u32, big-endian)
- Raw Code Bytes
