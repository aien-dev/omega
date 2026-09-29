# Effect capability generations: 64-bit canonical migration

```text
Document ID:  SPEC-OMEGA-EFFECT-CAP64
Amends:       SPEC-OMEGA-CANON-M4 (spec/canonical-encoding.md), spec/visor-authority.md
Gates:        OMEGA_EFFECT_CAP64_ROUNDTRIP_PASS, OMEGA_EFFECT_CAP64_IDENTITY_PASS,
              OMEGA_EFFECT_CAP64_STALE_REJECT_PASS, OMEGA_EFFECT_CAP64_AUTHORITY_PASS,
              OMEGA_EFFECT_CAP64_REGRESSION_PASS
Authority:    AIENOS 4c213863c840f77264e6851f5f6f393e80cfdb95 (aienos.lock; aienos#158)
```

## 1. Problem

AIENOS capability references are `(uint32 cap_id, uint64 generation)`. Since
aienos#158 a restarted table starts above every generation the old table used,
and every table seeds its generations from `CLOCK_BOOTTIME` ns << 8, so live
generations are normally above 2^32. Omega's runtime view (`aienos_cap.h`,
`RxCapRef`) is already 64-bit.

Omega's canonical effect object (`KIND_EFFECT`) was not. Its payload was the
host C struct `EffectPayload` copied byte for byte (`memcpy`, 176 bytes on
LP64: little-endian on the hosts we run, two bytes of tail padding), with
`uint32_t capability_generation`. `omega_build_effect` took a `uint32_t`, so a
live generation was cut to its low 32 bits before it was hashed. The authority
refused every such reference as `STALE_GEN`: fail closed, but an honest effect
could never be expressed, and the object's identity bound only half of the
generation. The same struct layout also contradicted SPEC-OMEGA-CANON-M4 §2
rule 4 ("payload big-endian").

## 2. Decision

Effect objects move to **object encoding version 0x02** with an explicit,
fixed, big-endian payload that carries the whole 64-bit generation. Every
other kind stays at version 0x01 and keeps its bytes and its SemanticIds.

### 2.1 Canonical header version rule

The header byte at offset +0x04 of every canonical object encoding is the
object encoding version of that object's kind:

| kind | version written | version accepted by the decoder |
|---|---|---|
| `KIND_EFFECT` (0x08) | 0x02 | 0x02 only |
| every other kind | 0x01 | 0x01 only |

The decoder (`omega_graph_deserialize_binary`) calls
`omega_canonical_check_header(version, kind)` for each object and refuses the
whole graph on any mismatch:

- `(0x01, KIND_EFFECT)` -> `OMEGA_CANON_ERR_EFFECT_V1` (legacy effect, refused);
- `(0x02, other kind)` and any other version -> `OMEGA_CANON_ERR_VERSION`.

`(0x02, other kind)` is refused so that nobody can mint a second identity for
an unchanged object by writing a new version byte in front of old bytes.

### 2.2 Effect payload v2 (178 bytes, big-endian)

```text
Offset  Len  Field                   Encoding / rule
------------------------------------------------------------------------------
+0x00   2    resource_class          u16 BE; must be != 0
+0x02   2    operation_code          u16 BE
+0x04   4    capability_slot         u32 BE (AIENOS cap_id)
+0x08   8    capability_generation   u64 BE; all 64 bits significant, no reserved bits
+0x10   32   capability_ref          SemanticId bytes; all-zero = unused
+0x30   2    param_len               u16 BE; must be <= 128
+0x32   128  param_bytes             bytes [0, param_len) meaningful; bytes
                                     [param_len, 128) must be 0x00
------------------------------------------------------------------------------
total   0xB2 = 178 bytes (OMEGA_EFFECT_PAYLOAD_LEN); payload_len must equal it
```

- `omega_effect_payload_encode` writes this layout from the in-memory
  `EffectPayload` (whose `capability_generation` is now `uint64_t`). It is a
  plain serializer: it writes every field as given, so a test can still build
  a malformed object on purpose.
- `omega_effect_payload_decode` is strict: exact length 178, resource != 0,
  param_len <= 128, zero padding after the parameters. Anything else is
  refused with a specific code. A 176-byte payload (the size of the old host
  struct) is reported separately as `OMEGA_EFFECT_ERR_LEGACY_V1`.
- `omega_validate_object`, the codec decoder, the Visor effect request, and
  the Visor verifier read effects only through the decoder. Nothing casts
  payload bytes to `EffectPayload` any more.
- The encoding is canonical: one meaning has one byte string (fixed length,
  fixed byte order, no padding bytes, zero fill required), so one meaning has
  one SemanticId.

### 2.3 Old bytes: refused, not reinterpreted

Old (version 0x01) effect bytes are **refused explicitly**; they are not
decoded with their old meaning and not accepted for inspection.

Why refuse rather than "decode with the original meaning and refuse for
authority use":

1. The only thing the old format could say about a live AIENOS reference was
   a truncated generation. There is no correct authority representation to
   preserve, and the brief forbids keeping a wrong one alive.
2. Old bytes were a host-struct image (host byte order, padding). Their
   meaning depended on the machine that wrote them, so there is no single
   "original meaning" to decode on another host.
3. No old effect object is stored anywhere in this repository (evidence,
   test vectors, corpora, session goldens): effects were only built in
   memory by tests. Refusal costs nothing real.
4. Refusal is at the version byte, so no old byte string can ever be read as a
   new one, whatever its payload looks like. (A length-only boundary would not
   be enough: the old validator accepted any payload of at least 176 bytes, so
   a 178-byte old payload was "valid" too.)

Old SemanticIds cannot collide with new ones: the header byte differs, so the
hashed byte strings differ.

### 2.4 Identity binds the whole generation

`SemanticId = SHA256(canonical encoding)` and all eight generation bytes are
in the encoding, so two effects that differ in any generation bit (including
only above bit 31) have different ids. Changing the generation of a named
object without renaming it makes `visor_effect_request_build` refuse it
(digest != stored id).

### 2.5 No truncation anywhere

The generation is `uint64_t` in: `omega_build_effect(..., uint64_t cap_gen)`,
`EffectPayload`, the wire payload, `VisorEffectRequest`, the Visor text and
JSON output (`PRIu64`), and the authority side (`AienosCapRef`, `RxCapRef`).
Checked by `grep` (recorded in the receipt): no `(uint32_t)` cast, `uint32_t`
parameter or `%u` format is applied to an effect capability generation in
`src/`, `tools/omega.c` or `tests/`. The only other `(uint32_t)` casts of a
capability generation are the low halves of split lo/hi stores in
`src/runtime/rx_coherent.c` and `src/runtime/rx_generation.c` (the high word
is stored next to them) and a deliberate truncation probe in
`tests/runtime/rx_r7_native.c`.

### 2.6 Incidental decoder hardening

`omega_graph_deserialize_binary` copied counts and lengths from the stream
into fixed arrays without bounds. Since this change feeds it hostile bytes, it
now refuses (returns -1) an object count above `OMEGA_MAX_GRAPH_OBJECTS`, and
attribute, relation and constraint counts, key, value, constraint-payload and
object-payload lengths above their array sizes. No well-formed stream is
affected.

## 3. Changed identities

Every `KIND_EFFECT` SemanticId changes (new header byte, new payload bytes).
No SemanticId of any other kind changes. No effect id was pinned in a golden
vector, receipt or corpus. The two effects built in-tree:

| Where | Builder call | Old id (193a7e7) | New id |
|---|---|---|---|
| `tools/omegatool.c` gate 10 (validate only) | `omega_build_effect(g, 1, 1, 0, 1)` | `ff7aa9701f39e226ac7a8babaafd74c052b169e986f03bac8555d4d6bf4fc46e` | `13f2c858be4dd0dcde1f458cf6e87d468a4d2bac2f8355254c32a977bc4692eb` |
| `tests/visor/test_visor_verify.c` case 11 | `omega_build_effect(fg, 1, 1, 3, 1)` | `94d5b66158ff57354be51e44b3867b810c7a2afde8749a95722877ed01195bfb` | `d3873a85a31eaa1b33018edebbf7e6dfce8a36a49703217505555a884fde7988` |

Why: version byte 0x01 -> 0x02, payload 176-byte LE host struct -> 178-byte
big-endian layout with a 64-bit generation. Neither test compares the id to a
constant; both still pass. The new ids are pinned by `test-effect-cap64`
(golden check).

### 3.1 How the new ids were checked

The new ids above were computed twice: by `omega_build_effect` and,
independently, by hashing the §2.2 byte layout written out by hand
(`4f4d4730 02 08 0000 0000 0000 000000b2` + payload). The test pins both new
ids and also checks that the hand-built v1 byte strings hash to the old ids,
so the refusal tests refuse the real old format.

## 4. Tests (`make test-effect-cap64`)

`tests/effect/test_effect_cap64.c`, linked with the pinned AIENOS
`libaienos_capability.a` (4c21386; `EFFECT_CAP64_CAP_LIB`, default
`$(AIENOS_CAP_LIB)`). Physics-free. Prints one line per gate:

- **ROUNDTRIP**: generations 0, 1, UINT32_MAX, UINT32_MAX+1, 1<<63,
  0xFFFFFFFFFFFFFFFE, UINT64_MAX, and live AIENOS generations, survive
  build -> canonical encode -> OMGG serialize -> decode -> validate -> Visor
  request (text and JSON print the exact decimal) with every field equal.
- **IDENTITY**: ids differ for generations that differ only in the high word
  or only in the low word; changing the generation after naming makes the
  Visor refuse; re-naming yields a new id; golden ids (§3.1).
- **STALE_REJECT**: hand-crafted bytes: v1 header on an effect, v2 header on
  a non-effect, old 176-byte struct payload, generation with the high 32 bits
  cut, non-zero padding, resource 0, param_len > 128, wrong length. All
  refused with the specific code; a v1 non-effect object in the same stream
  still decodes (control).
- **AUTHORITY** (real AIENOS 4c21386): live references forced to
  UINT32_MAX, UINT32_MAX+1 and 1<<63 validate after the round trip; the
  same reference with the high 32 bits cut is `STALE_GEN`; revoke ->
  `REVOKED`; reclaim -> `STALE_GEN`; wrong slot -> refused; wrong right ->
  `RIGHTS`; wrong resource -> `RESOURCE`; restart -> every pre-restart
  reference (carried in an effect object) is `STALE_GEN` against the new
  table while a newly minted one validates; 0xFFFFFFFFFFFFFFFE makes
  restart `EXHAUSTED` (by design of aienos#158, recorded, not a failure).
- `test-visor-authority`: the GEN_FITS / CHECK_IF_REPR gating that existed
  only because of this defect is removed; every Visor-path check is counted.

## 5. Non-claims

- The accelerator `OmegaEffectIntent` / `OmegaEffectReceipt`
  (`src/omega_accelerator.h`) still carry a 32-bit `capability_generation`.
  They belong to the physics accelerator port model (its own capability
  table), not to AIENOS, and are not changed here.
- The Visor JSON prints the generation as an exact decimal integer. JSON
  consumers that parse numbers as IEEE doubles lose precision above 2^53.
- No effect is executed; the mapping from `operation_code` / `resource_class`
  to AIENOS rights / resources is still a test convention.
