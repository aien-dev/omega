# Specification: OSC binary IR, canonical format and strict reader

```text
Document ID:   SPEC-ONE-LAYER-FORMAT
Issue:         aien-spark/omega#444 (one-layer plan, stage 2 "Open the binary front door"); omega#451 (version 6)
Status:        IMPLEMENTED for format versions 1..6 (reader + writer); version 6 in section 10
Code:          src/compiler/osc_ir.h (osc_ir_encode, osc_ir_encode_v6, osc_ir_decode, OscIrRefusal)
               src/compiler/osc_ir.c (encode_unit, encode_unit_v6, unit_version, decode_unit, decode_v6)
Tests:         tests/compiler/test_osc_ir_decode.c; make test-osc-ir-decode(-quick)
Data layout:   src/compiler/osc_ir.h is normative for OscUnit; meaning: docs/osc/OSC-1-DESIGN.md
```

## 1. What this format is

A compiled OSC unit (`OscUnit`, `src/compiler/osc_ir.h`) has one canonical byte form, written by
`osc_ir_encode`. Its SHA-256 is the unit's identity (`osc_ir_digest`; docs/osc/OSC-1-DESIGN.md
section 4). Until this change the bytes could only be written; `osc_ir_decode` now reads them back
without the text front end, so a machine can hand Omega executable meaning as bytes.

## 2. Canonical law

The canonical set is exactly

    { osc_ir_encode(u) : osc_ir_validate(u) == 0 }  ∪  { osc_ir_encode_v6(u) : osc_ir_validate(u) == 0 }

(the second set is format version 6, section 10; `osc_ir_encode` refuses a unit that only
version 6 can carry). `osc_ir_decode(buf, len, out)` returns `OSC_IR_OK` only when `buf[0..len)` is in that set, and then
fills `out` with the unit `u` that encodes to it. Every other input is refused with one named class
(section 7) and `out` is left all zero. The reader never repairs, skips, pads, truncates or guesses.

Consequences, each checked by `tests/compiler/test_osc_ir_decode.c`:

- encode -> decode -> encode is byte-identical for every admitted input;
- an admitted input re-encodes to exactly itself (no second spelling of a unit exists);
- the decoded unit passes `osc_ir_validate`, so the interpreter and back ends accept it.

## 3. Wire layout

All integers little-endian, no padding, no alignment, no pointers.

### 3.1 Header

| bytes | field |
|---|---|
| 7 | magic `4F 53 43 31 49 52 00` ("OSC1IR\0") |
| 1 | version, 1..5 (section 4) |
| versions 2..5 only: 1 | `nstructs` 0..16, then `nstructs` struct records (3.2) |
| 2 | `nfuncs` 0..32, then `nfuncs` function records (3.3) |

Nothing may follow the last function record.

### 3.2 Struct record (versions 2..5)

| bytes | field |
|---|---|
| 1 + n | name: length n 0..63, then n bytes, no NUL |
| 1 | `nfields` 0..16 (validation requires 1..16) |
| 2 | `ncells` |
| per field: 1 + n | field name (as struct name) |
| per field: 1 | scalar tag (3.4) |
| per field: 2 | `alen` (0 = scalar field) |
| per field: 2 | `off` |

### 3.3 Function record

| bytes | field |
|---|---|
| 1 + n | name: length n 0..63, then n bytes, no NUL |
| 1 | `nparams` |
| type | return type (3.4) |
| 2 | `nvregs` 0..480, then `nvregs` types (3.4) |
| 2 | `nblocks` 0..512, then per block: 4 `first`, 4 `count` |
| 4 | `ninsns` 0..4096, then `ninsns` instructions (3.5) |
| 2 + n | `requires` text: length n 0..255, then n bytes, no NUL |
| 2 + n | `ensures` text: as `requires` |

### 3.4 Type

One byte scalar tag: 0 void, 1 bool, 2 u8, 3 u16, 4 u32, 5 u64, 6 i8, 7 i16, 8 i32, 9 i64,
10 ref, 11 bytes, 12 cells. Tags above 12 are refused. For every tag except 10 that byte is the
whole type (the decoded `ref`, `elem`, `len`, `sid` are 0). Tag 10 (ref) continues:

| bytes | field |
|---|---|
| 1 | ref kind: 0 none, 1 own, 2 shared, 3 mut (above 3 refused) |
| 1 | element scalar tag |
| 2 | `len` |
| 1, only when the element tag is 0 (void) | `sid`: struct reference to `structs[sid-1]` |

An array reference always has a value element type and `sid` 0; a struct reference always has
element void and `sid` >= 1 (`type_is_ref`, `src/compiler/osc_ir.c`). The element byte therefore
decides, without ambiguity, whether the `sid` byte is present.

### 3.5 Instruction

One opcode byte, then the opcode's fields in this order. `r` is a 2-byte register, block or callee
field: 0x0000..0x7FFF, or 0xFFFF for "none" (-1); 0x8000..0xFFFE are refused (RANGE).

| op | name | fields after the opcode |
|---|---|---|
| 1 | CONST | r dst, 8 imm |
| 2 | MOV | r dst, r a |
| 3 | BIN | 1 sub, r dst, r a, r b |
| 4 | UN | 1 sub, r dst, r a |
| 5 | CMP | 1 sub, r dst, r a, r b |
| 6 | CAST | r dst, r a |
| 7 | ALLOC | r dst, r a |
| 8 | RELEASE | r a |
| 9 | LOAD | r dst, r a, r b |
| 10 | STORE | r a, r b, r c |
| 11 | CALL | r dst, r callee, 1 nargs (0..6; above 6 refused OVERSIZED), nargs x r arg |
| 12 | TRAP | 1 imm (trap code) |
| 13 | BR | r blk_t |
| 14 | CBR | r a, r blk_t, r blk_f |
| 15 | RET | r a |
| 16 | FLOAD | r dst, r a, r b, 1 imm (field index) |
| 17 | FSTORE | r a, r b, r c, 1 imm (field index) |
| 18 | AOPEN | r dst, 1 imm (cells) |
| 19 | AALLOC | r dst, r a, r b |
| 20 | ADESTROY | r a |
| 21 | POPEN | r dst, 1 sub (element tag), 1 nargs (slots), 8 imm (generation) |
| 22 | PCLOSE | r a |
| 23 | HALLOC | r dst, r a, r b |
| 24 | HGEN | r dst, r a, r b |
| 25 | HFREE | r a, r b, r c |
| 26 | HLOAD | r dst, r a, r b, r c |
| 27 | HSTORE | r a, r b, r c, r args[0] |
| 28 | SLOAD | r dst, r a, r b |
| 29 | SSTORE | r a, r b, r c |

Opcode 0 and opcodes above 29 are refused (RANGE): their length is unknown. The numbers are pinned
by `_Static_assert`s in the test. The meaning of every field and every value range that
`osc_ir_validate` enforces (types agree, registers in range, blocks partition the instructions,
calls only to earlier functions, trap codes 1..14, and the rest) is in `osc_ir.h` and
docs/osc/OSC-1-DESIGN.md.

### 3.6 Fields the format does not carry

`OscInsn.line` (debug information; source layout never reaches identity), and per opcode every
field not listed in 3.5. The reader sets them as the front end's emitter does
(`emit`, `src/compiler/osc_lower.c`): `line` 0; `dst`, `a`, `b`, `c`, `blk_t`, `blk_f`, `callee`
-1; everything else 0; HSTORE `nargs` 1. All array entries past a count are 0. The test compares
each decoded unit with the unit the compiler produced on every carried field.

## 4. Versions and historical readers

The version byte is a function of the content (`unit_version`, `src/compiler/osc_ir.c`): 5 if any
register is a byte slice (bytes/cells), else 4 if any pool instruction (POPEN..HSTORE) occurs,
else 3 if any arena instruction (AOPEN, AALLOC, ADESTROY) occurs, else 2 if the unit has structs,
else 1. Versions 2..5 share one layout (struct table present, possibly empty); version 1 has no
struct table. Every version 1..5 is read. A version byte that is not the one the content requires
is refused (VERSION_MISMATCH), so one unit never has two encodings among versions 1..5. Versions
are only ever added; an existing version's bytes are never reinterpreted.

Version 6 (section 10) is not chosen by content: only `osc_ir_encode_v6` writes it, and its
domain tag (10.1) keeps it a separate identity domain. `unit_version` and `OSC_IR_VERSION_MAX` (5)
are unchanged, so `osc_ir_encode` writes exactly the bytes it wrote before.

## 5. Limits

Counts are checked before anything under them is read: `nstructs` <= 16, `nfields` <= 16,
`nfuncs` <= 32, `nvregs` <= 480, `nblocks` <= 512, `ninsns` <= 4096, CALL `nargs` <= 6, names
<= 63 bytes, contract texts <= 255 bytes (the `OSC_MAX_*`, `OSC_NAME_MAX`, `OSC_CLAUSE_MAX`
constants of `osc_ir.h`). Input longer than `OSC_IR_ENCODED_MAX` (2,620,251 bytes: every count at
its maximum, every instruction at the widest form, CALL with six arguments, 18 bytes) is refused
before parsing.

## 6. Identity

`identity = SHA-256(canonical bytes)`. The magic and version byte are inside the hashed bytes, so
they act as the domain tag that spec/program-identity.md section 2 requires of an identity law: a
reader that sees the bytes knows which layout produced them, and two versions can never share a
preimage. Function names and the `requires` / `ensures` texts are inside the hashed bytes, so a
change to a requirement changes the identity (tested: 514 one-character changes over the corpus,
each a new digest, each round-trips). OSC unit identity is separate from Omega program identity v2
(`omega.program.v2`, spec/program-identity.md); this change does not alter either law. Version 6
has its own identity law (section 10.7): names excluded, structured contracts included, under the
tag `omega.osc.ir.v6`. The law above for versions 1..5 is unchanged.

## 7. Refusal classes

`osc_ir_decode` returns the first defect met while reading front to back; the checks after parsing
run in the order listed. `osc_ir_refusal_name` gives the names. Classes -12..-15 exist for
version 6 only; section 10.8 gives their meaning, check order and counterexamples.

| code | class | meaning | counterexample in the test |
|---|---|---|---|
| -1 | ARG | NULL input or output pointer | `osc_ir_decode(NULL, 8, u)` |
| -2 | MAGIC | first 7 bytes are not the magic | "OSC2IR..." |
| -3 | UNKNOWN_VERSION | version byte 0 or above 6 | versions 0 and 7 |
| -4 | TRUNCATED | input ends inside a field | empty input; last byte missing; magic only |
| -5 | OVERSIZED | input over `OSC_IR_ENCODED_MAX`, or a count or length over section 5 | 64-byte name; 481 registers; 256-byte requires text; 2,620,252-byte input |
| -6 | TRAILING | bytes after the last function | one extra byte |
| -7 | RANGE | unknown opcode, scalar tag or ref kind; a register/block/callee field 0x8000..0xFFFE | opcodes 0 and 30; tag 13; register 0x8000 |
| -8 | TEXT | a NUL byte inside a name or contract text (it would read back shorter: ambiguous) | name "f\0g" |
| -9 | INVALID | well-formed, but `osc_ir_validate` refuses the unit | u8 constant 256; register out of range |
| -10 | VERSION_MISMATCH | version byte differs from the one the content requires (section 4) | version 2 or 5 for a plain unit |
| -11 | NONCANONICAL | the unit re-encodes to different bytes | none: unreachable (below) |
| -12 | DOMAIN | version 6 without its exact domain tag (10.8) | tag "omega.osc.ir.v7" |
| -13 | IDENT | version 6: a numeric identifier or name entry names no object (10.8) | name for function 1 of 1 |
| -14 | ORDER | version 6: name table not in canonical order, or an empty entry (10.8) | two names for function 0 |
| -15 | CONTRACT | version 6: a malformed requires/ensures object (10.8) | result in a requires clause |

NONCANONICAL is a backstop. Given the checks above it, every field read back writes back to the
same bytes, so no input reaches it; the sweeps assert it is never returned. It is reached only when
a check above it is removed: decoder mutants 1, 2, 5 and 8 (section 8), and for version 6
mutants 9, 10 and 11 (section 10.9), fall through to it.

## 8. Qualification

`make test-osc-ir-decode` (part of `test-compiler`, the full CI leg) and
`make test-osc-ir-decode-quick` (part of `test-compiler-quick`, run on every compiler-path change)
build `tests/compiler/test_osc_ir_decode.c` plain and with ASan/UBSan. Measured on the Spark,
2026-10-14, full mode:

- corpus: all 59 units the front end compiles from `tests/compiler/progs`, `tests/compiler/slices`,
  `src/osh`, `tests/physics0/osc` (32 v1, 7 v2, 6 v3, 3 v4, 11 v5) round-trip byte-exactly and
  equal the compiled unit on every carried field;
- 10,000 distinct valid objects (seeded variants of the corpus: constants, names, contract text,
  operators, trap codes) round-trip byte-exactly (2,000 under ASan);
- 202,675 malformed inputs (every byte of every unit up to 2,000 bytes flipped three ways, every
  truncation, trailing bytes, seeded multi-byte edits; larger units sampled at an even stride):
  177,013 refused with a named class and a zeroed unit, 25,662 decoded to a different valid unit
  that re-encodes to exactly those bytes (for example a changed constant), none admitted otherwise,
  no crash. The ASan/UBSan build runs the same number of cases (177,177 refused, 25,498 admitted;
  the random edits differ because it draws fewer valid variants first) with no sanitizer report;
- one counterexample per reachable class (section 7), built by an independent writer in the test,
  whose canonical case is byte-identical to `osc_ir_encode`;
- negative controls: 5 planted defects caught by the test's own checks, and 8 decoder mutants
  (`OSC_IR_DECODE_MUTANT` = 1 no trailing check, 2 no version check, 3 no zeroing on refusal,
  4 no validation, 5 no NUL check, 6 no register range check, 7 no total size check, 8 truncation
  read as zeros) each make the test fail.

## 9. Resolved by version 6 (asked by #444, specified in section 10, omega#451)

- **Numeric identifiers.** Version 6 identifies functions, structs and fields by index only (calls,
  struct references and field accesses already did). Names move to an optional side table outside
  identity (10.5, 10.7).
- **Structured requirement objects.** Version 6 carries each `requires` / `ensures` clause as a typed
  postfix term list (10.4) that a checker reads without parsing source text. The objects are inside
  identity; the source texts move to the side table.
- **Domain tag string.** Version 6 writes the tag `omega.osc.ir.v6` after the version byte (10.1),
  inside the hashed bytes. Versions 1..5 keep their bytes and identities: the tag exists only in the
  new version, so no existing OSC unit identity changes.

## 10. Format version 6 (omega#451)

Written only by `osc_ir_encode_v6`; identity by `osc_ir_digest_v6`; read by the same
`osc_ir_decode`. Every integer is little-endian, as in section 3. Types (3.4) and instructions (3.5)
are written exactly as in versions 1..5.

### 10.1 Header

| bytes | field |
|---|---|
| 7 | magic `4F 53 43 31 49 52 00` ("OSC1IR\0") |
| 1 | version 6 |
| 16 | domain tag: the 15 bytes `omega.osc.ir.v6` and one NUL (`OSC_IR_V6_TAG`, `OSC_IR_V6_TAG_LEN`) |

The core (10.2) follows, then the name table (10.5). Nothing may follow the name table.
`osc_ir_encoding_version` returns 6 only when all 16 tag bytes are present and exact.

### 10.2 Core

| bytes | field |
|---|---|
| 1 | `nstructs` 0..16, then per struct: |
| 1 | `nfields` 0..16 (validation requires 1..16) |
| 2 | `ncells` |
| per field: 1 | scalar tag (3.4) |
| per field: 2 | `alen` (0 = scalar field) |
| per field: 2 | `off` |
| 2 | `nfuncs` 0..32, then `nfuncs` function records (10.3) |

The struct table is always present (possibly empty) and carries no names.

### 10.3 Function record

| bytes | field |
|---|---|
| 1 | `nparams` |
| type | return type (3.4) |
| 2 | `nvregs` 0..480, then `nvregs` types (3.4) |
| 2 | `nblocks` 0..512, then per block: 4 `first`, 4 `count` |
| 4 | `ninsns` 0..4096, then `ninsns` instructions (3.5) |
| 1 | `nreq` 0..64, then `nreq` requirement terms (10.4): the `requires` object |
| 1 | `nens` 0..64, then `nens` requirement terms (10.4): the `ensures` object |

Numeric identifiers in the core: a CALL `callee` is a function index below `nfuncs`; a type's `sid`
names `structs[sid-1]` and is at most `nstructs`; FLOAD / FSTORE `imm` is a field number of that
struct. Function names, struct names, field names and contract texts are not in the core.

### 10.4 Requirement terms

A requirement object (`OscCterm req[]` / `ens[]`, `osc_ir.h`) is a postfix program over a typed
value stack. Each term is one kind byte, then the kind's fields:

| kind | name | fields after the kind | effect and typing |
|---|---|---|---|
| 1 | PARAM | 1 `arg` | push parameter `arg` (< `nparams`; its type is bool or an integer scalar) |
| 2 | RESULT | none | push the return value (ensures only; the return type is bool or an integer scalar) |
| 3 | CONST | 1 `sub` (scalar tag 1..9), 8 `imm` | push `imm`, canonical for `sub` as for the CONST instruction |
| 4 | BIN | 1 `sub` (OscBinOp 1..10) | pop b, a; push a `sub` b with a's type. SHL / SHR: both integer; AND / OR / XOR: equal types; the rest: equal integer types |
| 5 | UN | 1 `sub` (OscUnOp 1..3) | pop a; push `sub` a. LNOT: bool; NEG / BNOT: integer |
| 6 | CMP | 1 `sub` (OscCmp 1..6) | pop b, a of equal type; push bool |
| 7 | CAST | 1 `sub` (integer tag 2..9) | pop an integer; push it converted to `sub` |

Kind 0 and kinds above 7 are refused (RANGE): their length is unknown. An object is well formed
when it is empty (no clause) or leaves exactly one bool on the stack with no step popping an empty
stack, and every term field its kind does not carry is 0 (one object has one form). The operator
meanings are those of the IR instruction with the same `sub` (`osc_ir.h`). The `requires` object
holds on entry, the `ensures` object on return. This change fixes the carried form, its typing and
its place in identity; no consumer evaluates the objects yet (10.9).

### 10.5 Name table

| bytes | field |
|---|---|
| 2 | `count` 0..368 (`OSC_IR_V6_NAMES_MAX` = 32 x 3 + 16 x (1 + 16)), then `count` entries: |
| 1 | kind: 1 function name, 2 struct name, 3 field name, 4 requires text, 5 ensures text (`OSC_IR_NAME_*`) |
| 1 | `index`: the function (kinds 1, 4, 5) or the struct (kinds 2, 3) |
| 1 | `sub`: the field number for kind 3; 0 for every other kind |
| 2 | length n: 1..63 for kinds 1..3, 1..255 for kinds 4 and 5 |
| n | the text, no NUL |

Every entry names an existing object (IDENT). Entries are strictly ascending by
(kind, `index`, `sub`), so no object has two entries (ORDER). An absent name has no entry; an entry
of length 0 is a second spelling of "absent" and is refused (ORDER). Names are optional: a version 6
unit may leave any function, struct or field unnamed. Present struct names stay unique within the
unit and present field names unique within their struct (`osc_ir_validate`). The texts of kinds 4
and 5 are the source spelling, kept for people; the checked meaning is the object in 10.4.

### 10.6 Limits

Section 5's counts apply to the core. In addition: at most 64 terms per object
(`OSC_CONTRACT_MAX`), at most 368 name-table entries. Input longer than `OSC_IR_V6_ENCODED_MAX`
(2,662,701 bytes: section 5's widest unit without names, every object at 64 ten-byte CONST terms,
and every possible name-table entry at its longest) is refused before parsing. For every version
byte other than 6, the section 5 bound (`OSC_IR_ENCODED_MAX`) is still applied, after the version
byte is read and before it is judged, so inputs of versions 1..5 are refused exactly as before.

### 10.7 Canonical law and identity

The version 6 canonical set is `{ osc_ir_encode_v6(u) : osc_ir_validate(u) == 0 }`. Every valid
unit has a version 6 form. A unit with a structured object or an unnamed struct or field has no
version 1..5 form: `osc_ir_encode` and `osc_ir_digest` refuse it (-1), and a version 1..5 input that
would decode to one is refused (INVALID). A unit that has both forms has one spelling in each; the
version byte and tag tell them apart and both decode to the same unit.

`osc_ir_digest_v6(u) = SHA-256(header || core)`: the 24 header bytes and the core, everything before
the name table's `count`. Consequences, each checked by the test:

- renaming a function, struct or field, or editing a contract text, never changes the identity
  (alpha-equivalence, as in program identity v2, spec/program-identity.md), while the bytes change;
- a change to a requirement object always changes the identity;
- the tag is inside the preimage, so a version 6 identity differs from the version 1..5 identity of
  the same unit.

Two units that differ only in names share an identity and differ in bytes. A consumer that must tell
them apart hashes the whole encoding.

### 10.8 Refusal classes for version 6

Order of checks: total size (OVERSIZED); magic (MAGIC); version byte; tag (TRUNCATED when fewer than
16 bytes remain, else DOMAIN); the core front to back (TRUNCATED, OVERSIZED for counts, RANGE for
tags, opcodes, register fields and term kinds); each name-table entry in the order kind (RANGE),
target (IDENT), order and `sub` (ORDER), length (OVERSIZED; 0 is ORDER), text (TRUNCATED, TEXT);
then TRAILING; core identifiers (IDENT); requirement objects (CONTRACT); `osc_ir_validate`
(INVALID); the NONCANONICAL backstop. VERSION_MISMATCH does not apply: the writer, not the content,
chooses version 6.

| code | class | meaning | counterexamples in the test |
|---|---|---|---|
| -12 | DOMAIN | version byte 6 without the exact 16-byte tag | tag "omega.osc.ir.v7"; tag without its NUL; tag missing |
| -13 | IDENT | a core identifier or a name entry names no object: `sid` above `nstructs`, `callee` not below `nfuncs`, an entry for a missing function, struct or field | struct id 2 in a unit without structs; name for function 1 of 1; field name for a missing struct; requires text for function 5 |
| -14 | ORDER | name entries not strictly ascending (including two for one object), `sub` nonzero outside kind 3, an entry of length 0 | descending entries; two names for function 0; empty entry; field number on a function name |
| -15 | CONTRACT | a requirement object that 10.4 refuses | parameter 0 of a function with none; result in a requires clause; clause ending in a u8; BIN with one operand; clause leaving two values; u8 constant 256; constant of type ref; BIN operator 0 |

The existing classes also have version 6 counterexamples: TRUNCATED (input ends inside the tag,
inside a term, inside the name table), UNKNOWN_VERSION (version 7 followed by the version 6 tag),
OVERSIZED (64-byte name, 256-byte requires text, 65 terms, 369 entries), RANGE (name kinds 0 and 6,
term kinds 0 and 8), TEXT (NUL inside a name), TRAILING (one byte after the name table), INVALID
(u8 constant 256 in the core). NONCANONICAL stays unreachable: the sweeps assert it is never
returned for version 6 input either.

### 10.9 Qualification and limits of this change

Measured on the Spark, 2026-10-17, `make test-osc-ir-decode`, full mode (section 8's sweeps of
versions 1..5 give the same case, refused and admitted totals as before this change; one of their
byte flips now lands on version 6 and is refused DOMAIN):

- corpus: all 59 units round-trip byte-exactly as version 6 three ways (as compiled; with every name
  and text removed; with random well-typed requires/ensures objects), and decode to the encoded
  unit on every carried field including every term;
- identity: for all 59 units a rename keeps the identity, the identity equals SHA-256 of the bytes
  before the name table, a changed requirement object gives a new identity, and the version 6
  identity differs from the version 1..5 identity; `osc_ir_encode` refuses all 67 units that only
  version 6 can carry (59 with objects, 8 with unnamed structs);
- 2,000 distinct valid version 6 objects (seeded variants with objects and with names removed or
  kept) round-trip byte-exactly (300 under ASan);
- 230,463 malformed version 6 inputs (byte flips, truncations, trailing bytes, seeded multi-byte
  edits of every corpus unit with objects): 199,894 refused with a named class (among them DOMAIN
  3,597, IDENT 1,079, ORDER 1,085, CONTRACT 21,153), 30,569 decoded to a different valid unit that
  re-encodes to exactly those bytes, NONCANONICAL never returned, no crash. The ASan/UBSan build
  runs 231,397 such cases with no sanitizer report;
- 35 counterexamples (10.8) from an independent writer in the test, whose canonical case is
  byte-identical to `osc_ir_encode_v6`; with section 7's 22 cases, 57 of 57 as expected and 14 of
  the 15 classes exhibited (all but the NONCANONICAL backstop);
- 6 more decoder mutants, each caught: 9 no domain check, 10 no name-table target check, 11 no
  order or empty-entry check, 12 no requirement-object check, 13 no core identifier check, 14 name
  table hashed into the identity.


Not in this change: the front end (`osc_lower`) still writes versions 1..5 and fills no requirement
objects; translating `requires` / `ensures` source into objects, evaluating them, and teaching the
independent checker decoder (`src/checker2`) to read version 6 are follow-up work.
