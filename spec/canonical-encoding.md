# SPECIFICATION: OMEGA Canonical Encoding and Identity Derivation

```text
Document ID:     SPEC-OMEGA-CANON-M4
Classification:  Sovereign Semantic Specification
Target Substrate: Deterministic Byte Serialization & Cryptographic Identification
Status:          RATIFIED FOR M4 (OMEGA_SEMANTICS)
```

---

## 1. The Canonical Identity Law

$$\text{SEMANTIC\_ID} = \text{SHA256}(\text{CANONICAL\_SERIALIZATION}(\text{object}))$$

The canonical serialization is defined such that:
1. Every semantic object has exactly one canonical binary encoding.
2. Every semantically equivalent object produces the identical canonical binary encoding regardless of surface syntax, builder call order, or attribute insertion sequence.
3. Any semantic difference produces a distinct canonical binary encoding, and by the collision-resistance of SHA-256, a distinct `SEMANTIC_ID`.

---

## 2. Binary Encoding Grammar

An encoded OMEGA canonical chunk adheres strictly to this format:

```text
Offset  Length  Field                  Type / Description
---------------------------------------------------------------------------------
+0x00   4       magic                  0x4F, 0x4D, 0x47, 0x30 ("OMG0")
+0x04   1       version                0x01
+0x05   1       kind                   Semantic category tag (0x01..0x0B)
+0x06   2       attr_count             u16 big-endian, count of attributes
+0x08   ...     attributes             Sorted lexicographically by UTF-8 key
                [ key_len: u8, key_bytes, val_len: u16, val_bytes ]
...     2       rel_count              u16 big-endian, count of relations
...     ...     relations              Sorted by (rel_type: u16, target_id: [u8; 32])
...     2       constraint_count       u16 big-endian, count of constraints
...     ...     constraints            Sorted by (constraint_kind: u16, payload)
...     4       payload_len            u32 big-endian, length of kind-specific payload
...     ...     payload                Big-endian payload bytes
```

### Sorting & Normalization Rules
1. **Attribute Keys**: Sorted strictly in ascending order using unsigned byte-wise comparison (`memcmp`). Duplicate keys are prohibited.
2. **Relations**: Sorted primarily by relation kind (`u16`), secondarily by target `SemanticId` (`32-byte memcmp`). Duplicate edges are collapsed.
3. **Constraints**: Sorted primarily by constraint kind (`u16`), secondarily by constraint parameter bytes.
4. **Integers & Payload**: Always encoded in network byte order (big-endian), zero-padded to $\lceil \text{width} / 8 \rceil$.

---

## 3. Reference Resolution & Acyclicity

In Milestone 4, expression graphs are directed acyclic graphs (DAGs). References point backward or horizontally to existing canonical objects by `SemanticId`. A graph containing an unresolved reference or a circular reference cycle fails validation and is rejected prior to canonical identity derivation.

---

## 4. Amendment: per-kind object encoding version (SPEC-OMEGA-EFFECT-CAP64)

The `version` byte at +0x04 is the object encoding version of the object's
kind. `KIND_EFFECT` objects are version `0x02` and carry the explicit
178-byte big-endian effect payload with a 64-bit capability generation; every
other kind is version `0x01`, unchanged. A decoder accepts only that pair:
a version `0x01` effect (legacy, truncated authority) and a version `0x02`
non-effect are refused. See `spec/effect-cap64-migration.md`.
