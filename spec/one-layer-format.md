# Specification: OSC binary IR, canonical format and strict reader

```text
Document ID:   SPEC-ONE-LAYER-FORMAT
Issue:         aien-spark/omega#444 (one-layer plan, stage 2 "Open the binary front door")
Status:        IMPLEMENTED for format versions 1..6 (reader + writer); numeric-identifier version OPEN (section 9)
Code:          src/compiler/osc_ir.h (osc_ir_encode, osc_ir_decode, OscIrRefusal)
               src/compiler/osc_ir.c (encode_unit, unit_version, decode_unit)
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

    { osc_ir_encode(u) : osc_ir_validate(u) == 0 }

`osc_ir_decode(buf, len, out)` returns `OSC_IR_OK` only when `buf[0..len)` is in that set, and then
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
| 1 | version, 1..6 (section 4) |
| versions 2..6 only: 1 | `nstructs` 0..16, then `nstructs` struct records (3.2) |
| 2 | `nfuncs` 0..32, then `nfuncs` function records (3.3) |

Nothing may follow the last function record.

### 3.2 Struct record (versions 2..6)

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
10 ref, 11 bytes, 12 cells, 13 f32, 14 f64 (version 6, OSC-4 Slice A). Tags above 14 are refused.
For every tag except 10 that byte is the
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
| 30 | FBIN | 1 sub (1 ADD, 2 SUB, 3 MUL, 4 DIV, 5 MIN, 6 MAX), r dst, r a, r b |
| 31 | FUN | 1 sub (1 NEG, 2 ABS, 3 SQRT), r dst, r a |
| 32 | FCMP | 1 sub (condition, as CMP), r dst, r a, r b |
| 33 | FCVT | r dst, r a |
| 34 | ITOF | 1 sub (1 signed, 2 unsigned: the source), r dst, r a |
| 35 | FTOI | 1 sub (1 signed, 2 unsigned: the destination), r dst, r a |
| 36 | FBITS | r dst, r a |

Opcode 0 and opcodes above 36 are refused (RANGE): their length is unknown. Opcodes 30..36 are the
OSC-4 Slice A float instructions (docs/osc/OSC-4-DESIGN.md 1.5); a CONST whose register is f32 or f64
carries the IEEE bit pattern in `imm` (f32: low 32 bits, upper 32 zero). The numbers are pinned
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

The version byte is a function of the content (`unit_version`, `src/compiler/osc_ir.c`): 6 if any
register, return, struct field or pool element is f32 or f64 or any opcode 30..36 occurs, else 5 if any
register is a byte slice (bytes/cells), else 4 if any pool instruction (POPEN..HSTORE) occurs,
else 3 if any arena instruction (AOPEN, AALLOC, ADESTROY) occurs, else 2 if the unit has structs,
else 1. Versions 2..6 share one layout (struct table present, possibly empty); version 1 has no
struct table. Every version 1..6 is read. A version byte that is not the one the content requires
is refused (VERSION_MISMATCH), so one unit never has two encodings. Versions are only ever added;
an existing version's bytes are never reinterpreted.

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
(`omega.program.v2`, spec/program-identity.md); this change does not alter either law.

## 7. Refusal classes

`osc_ir_decode` returns the first defect met while reading front to back; the checks after parsing
run in the order listed. `osc_ir_refusal_name` gives the names.

| code | class | meaning | counterexample in the test |
|---|---|---|---|
| -1 | ARG | NULL input or output pointer | `osc_ir_decode(NULL, 8, u)` |
| -2 | MAGIC | first 7 bytes are not the magic | "OSC2IR..." |
| -3 | UNKNOWN_VERSION | version byte 0 or above 6 | versions 0 and 7 |
| -4 | TRUNCATED | input ends inside a field | empty input; last byte missing; magic only |
| -5 | OVERSIZED | input over `OSC_IR_ENCODED_MAX`, or a count or length over section 5 | 64-byte name; 481 registers; 256-byte requires text; 2,620,252-byte input |
| -6 | TRAILING | bytes after the last function | one extra byte |
| -7 | RANGE | unknown opcode, scalar tag or ref kind; a register/block/callee field 0x8000..0xFFFE | opcodes 0 and 37; tag 15; register 0x8000 |
| -8 | TEXT | a NUL byte inside a name or contract text (it would read back shorter: ambiguous) | name "f\0g" |
| -9 | INVALID | well-formed, but `osc_ir_validate` refuses the unit | u8 constant 256; register out of range |
| -10 | VERSION_MISMATCH | version byte differs from the one the content requires (section 4) | version 2, 5 or 6 for a plain unit |
| -11 | NONCANONICAL | the unit re-encodes to different bytes | none: unreachable (below) |

NONCANONICAL is a backstop. Given the checks above it, every field read back writes back to the
same bytes, so no input reaches it; the sweeps assert it is never returned. It is reached only when
a check above it is removed: decoder mutants 1, 2, 5 and 8 (section 8) fall through to it.

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

Measured again on the Spark, 2026-10-16, full mode, after version 6 (OSC-4 Slice A floats) was added:
64 corpus units round-trip (32 v1, 7 v2, 6 v3, 3 v4, 11 v5, 5 v6); 10,000 distinct valid objects
round-trip byte-exactly (2,000 under ASan); 223,489 malformed inputs: 195,293 refused with a named
class, 28,196 admitted as another canonical unit, NONCANONICAL 0, no crash (ASan/UBSan leg: 223,496
cases, 195,299 refused, 28,197 admitted, no sanitizer report); all 23 counterexample cases as
expected, including version 7 (UNKNOWN_VERSION), version 6 on a unit without floats
(VERSION_MISMATCH), tag 15 and opcode 37 (RANGE); 8 of 8 decoder mutants caught.

## 9. Open (asked by #444, not in this change)

- **Numeric identifiers.** Versions 1..5 still carry function, struct and field names as text, and
  names are part of identity. A later version that identifies functions, structs and fields by index
  only (calls and field accesses already do) is not specified here. (Version 6 was taken by the
  OSC-4 Slice A floats, 2026-10-16: same layout as 2..5 plus tags 13 and 14 and opcodes 30..36.)
- **Structured requirement objects.** `requires` / `ensures` are carried as source text; their
  checks are lowered into the instruction stream by the front end. A structured, machine-readable
  requirement object (and its place in identity) needs a design decision first.
- **Domain tag string.** Identity is versioned by the magic and version byte inside the hashed
  bytes (section 6), not by a separate `omega.osc.vN` tag string in the style of
  spec/program-identity.md. Adopting such a tag would change every existing OSC unit identity.
