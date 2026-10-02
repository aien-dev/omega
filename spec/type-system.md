# SPECIFICATION: The OMEGA Type System

```text
Document ID:     SPEC-OMEGA-TYPE-M4
Classification:  Sovereign Semantic Specification
Target Substrate: Typed AST and Semantic Value Graphs
Status:          RATIFIED FOR M4 (OMEGA_SEMANTICS)
```

---

## 1. Type Invariants

The OMEGA type system enforces:
1. **Explicit Bounds**: Every type declares exact mathematical bit-widths or cardinalities.
2. **Substrate Independence**: Host machine words (`int`, `long`, `usize`, `uintptr_t`) are strictly forbidden.
3. **Finite Representation**: Infinite types or unbounded recursive structures are refused fail-closed.

---

## 2. Canonical Semantic Types

| Type Tag | Name | Parameters | Allowed Range / Constraints |
| :--- | :--- | :--- | :--- |
| `0x01` | `UNIT` | None | Singleton type. Value: `()` |
| `0x02` | `BOOL` | None | Boolean truth values: `false (0)`, `true (1)` |
| `0x03` | `UNSIGNED_INTEGER` | `width: u16` | Fixed width $w \in [1, 256]$ bits |
| `0x04` | `SIGNED_INTEGER` | `width: u16` | Fixed width $w \in [2, 256]$ bits, two's complement |
| `0x05` | `BITVECTOR` | `width: u16` | Bit sequence $w \in [1, 256]$ bits |
| `0x06` | `BYTE` | None | Exact 8-bit octet (isomorphic to `BITVECTOR(8)`) |
| `0x07` | `SEQUENCE` | `elem_type_id: SemanticId, len: u32` | Fixed-length homogeneous sequence ($L \le 65535$) |
| `0x08` | `TUPLE` | `element_types: List<SemanticId>` | Ordered heterogeneous elements ($N \le 255$) |
| `0x09` | `ADDRESS` | `width: u16` | Memory or register address index ($w \in [16, 64]$) |
| `0x0A` | `RESOURCE` | `resource_class: u16` | Abstract resource identifier class |
| `0x0B` | `CAPABILITY_REFERENCE` | `slot: u32, generation: u64` | OMEGA reference to an M3 capability token (64-bit generation: see `spec/effect-cap64-migration.md`) |
| `0x0C` | `EFFECT_INTENT_REFERENCE`| `intent_digest: [u8; 32]` | Reference to an intended physical effect |
| `0x0D` | `EFFECT_RECEIPT_REFERENCE`| `receipt_digest: [u8; 32]` | Reference to an authoritative execution receipt |
| `0x0E` | `FP32`| `width: u16 = 32` | IEEE binary32 value; amendment for E1 gap row 12 (spec/program-fp32.md); mixing with integers needs an explicit `CONVERT` (opcode `0x10`) |

---

## 3. Values

A value $v$ is a typed tuple $v = (\tau, \beta)$ where $\tau$ is the canonical `SemanticId` of its declared `SemanticType`, and $\beta$ is its canonical big-endian byte sequence.

### Integer Canonicalization
Non-semantic human syntactic variations collapse to identical binary representations:
- `4`, `04`, `0x04`, `0b00000100` with type `UNSIGNED_INTEGER(8)` all canonicalize to `[0x04]`.
- With type `UNSIGNED_INTEGER(32)`, all canonicalize to `[0x00, 0x00, 0x00, 0x04]`.
- Distinct types (`U8(4)` vs `U32(4)`) have distinct semantic types, hence distinct canonical encodings and distinct `SemanticId`s.
