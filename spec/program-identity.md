# Specification: Omega Program Identity v2 (`OMEGA_PROGRAM_ID_*`)

```text
Document ID:     SPEC-OMEGA-PROGRAM-ID-V2
Amends:          SPEC-OMEGA-M8 (spec/program.md), spec/omega-language-v0.md identity law 6,
                 spec/visor.md section 6a (program id finding)
Status:          IMPLEMENTED (branch fix/program-id-body)
Domain tag:      "omega.program.v2"
```

## 1. The defect this fixes

`omega_program_compute_id` (v1) hashed `"PROG"`, the program name, the contract type
tags and the low byte of each width, the two contract constraint ids, and the low byte of
`cost.insn_count` and `cost.latency_cycles`. It did **not** hash the body. Two programs
with different meaning therefore shared an id whenever they shared a name, a contract
and (by accident) a cost low byte. Reproduced in Visor: after `clear`,
`fn t(x: u64) -> u64 { x + 1 }` and `fn t(x: u64) -> u64 { x + 2 }` got the same
`program_id` (reproduced on origin/main 8e7a445: both `87a9e0e1...`). Discovery gives every
machine-code slice candidate the same name and contract, so equal-length slices would also
have shared one id. That breaks content addressing: one id named two meanings.

## 2. Identity law (v2)

```
program_id = SHA256( "omega.program.v2" || 0x00
                     || body_root_id                     (32 bytes)
                     || input_type_id                    (32 bytes)
                     || output_type_id                   (32 bytes)
                     || contract.precondition_id         (32 bytes)
                     || contract.postcondition_id )      (32 bytes)
```

Every 32-byte component is an existing canonical `SemanticId` computed by
`omega_compute_semantic_id` (spec/canonical-encoding.md). No second canonical encoder
exists: the body is lowered into a scratch `OmegaGraph` with the existing builders and
only the root object's id is taken. The 17-byte domain tag is the version boundary: a
v1 preimage began with `"PROG"` and a name, so a v1 id and a v2 id can never come from
the same preimage, and a reader that sees the tag knows which law produced the id.

### 2.1 The canonical semantic body

A program body is the ordered step list `OmegaProgramBody` (innermost step first):
`f(x) = op_n( ... op_2( op_1(x, c_1), c_2) ..., c_n)`, each step one of
`OP_ADD OP_SUB OP_MUL OP_AND OP_OR` with one constant. It is lowered as:

| object | builder | notes |
|---|---|---|
| `T`  | `omega_build_type_uint(w)` (or `_signed_int` / `_bitvector` for those tags) | `w` = contract input width; the operation type |
| `P`  | `omega_build_param(T, 0)` | the program's parameter (section 2.2) |
| `C_i` | `omega_build_val_uint(T, w, c_i)` | step constant, big-endian `ceil(w/8)` bytes |
| `O_i` | `omega_build_op_binary(op_i, OVERFLOW_WRAP, T)` | the step's operation; wrap is what the AArch64 realization does |
| `A_i` | `omega_build_apply(O_i, A_{i-1}, C_i)` with `A_0 = P` | running value is operand 0, constant operand 1 |

`body_root_id = id(A_n)` (and `id(P)` for an empty body, the identity function). These are
the same object shapes the Visor language already builds for expressions (`x + y` lowers
to `omega_build_apply(omega_build_op_binary(OP_ADD, OVERFLOW_WRAP, u64), x, y)`).

`input_type_id` / `output_type_id` are the ids of the contract's TYPE objects built the same
way from `(tag, width)`, so a width change changes the id through a canonical type object,
not through a raw byte.

### 2.2 The parameter object (new canonical shape)

The object model had no free variable. `omega_build_param(type_id, index)` builds:

- kind `KIND_VALUE`
- one attribute `omega.param` whose value is `index` as a big-endian u16 (V0: always 0)
- payload: the existing `ValuePayload` layout with `type_id` set and `byte_len = 0`

A literal value always has `byte_len >= 1` and no attributes, so a parameter can never
equal a literal. The parameter carries its **position**, never its source name, so
`fn f(x)` and `fn f(n)` with the same body are the same program (alpha-equivalence).

### 2.3 Programs without a body

`has_body = false` means the program's meaning is unknown to Omega (for example a raw
machine-code slice nothing could lift). Such a program has **no identity**:
`omega_program_compute_id` returns -1 and sets `program_id` to all zero bytes, and
`omega_library_insert` refuses an all-zero id. Two unknown programs never share an id.

## 3. Field classification

| field | class | in the id? | reason |
|---|---|---|---|
| `body` (step list) | **semantic** | yes, as `body_root_id` | what the program computes |
| `contract.input_type`, `input_width` | **semantic** | yes, as `input_type_id` | the domain |
| `contract.output_type`, `output_width` | **semantic** | yes, as `output_type_id` | the codomain |
| `contract.precondition_id`, `postcondition_id` | **semantic** | yes | the promise the program makes; the text strings are the source of these ids and enter only through them |
| `name` | metadata | **no (removed in v2)** | a human label; renaming does not change meaning. Two names for one body now share one id (dedupe), exactly as `let a = 7` / `let b = 7` share one VALUE id |
| `cost.insn_count`, `reg_pressure`, `memory_bytes` | metadata | **no (removed in v2)** | properties of one realization (the same body realized differently has a different count) |
| `cost.latency_cycles` | metadata | **no (removed in v2)** | a model or measurement of one realization on one machine |
| `realization` (code bytes, realization id, target) | metadata | no | a realization of the meaning, identified separately by `realization_id` and the (program, machine, realization) triple id |
| `graph` pointer | metadata | no | not used for identity; pointer values never enter an id |
| `is_realized`, `is_verified` | state | no | lifecycle, not meaning |
| source whitespace, comments, literal spelling, parameter name, commutative operand side | not stored | no | normalized away by the parser/lowering before the body exists |
| parser node allocation order, builder call order | not stored | no | the body is an ordered step list; the scratch graph is rebuilt from it in one fixed order |

## 4. Where bodies come from

- `omega_program_build_unary_op` records one step. It now refuses a constant above
  `0xFFFFFFFF` (fail closed): it loads immediates with MOVZ + one MOVK, so a wider
  constant used to be silently truncated in the code while the body claimed the full value.
- `omega_program_compose(a, b)` concatenates `a`'s steps then `b`'s (capacity 64 steps;
  refuses beyond). Composition therefore gives the same body as the equivalent directly
  written chain.
- Visor Language V0 lowering (`omega_language_lower_program`) builds through the two above.
- `omega_discover_abstractions` builds an abstraction from a machine-code slice. It now
  **lifts** the slice back to a body (`omega_program_lift_body`: decode the rigid
  `movz x1[, movk x1 lsl 16]; op x0, x0, x1` template, then re-emit the steps and require
  byte equality). A slice that does not lift has no body, no id, and is not verified, so it
  cannot be selected or admitted.
- `omega_refactor_program` copies the original's body: a refactoring that preserves meaning
  keeps the original's identity (its cost differs; cost is metadata).
- Programs assembled by hand (tests) must set a body or they have no identity.

## 5. Consequences for consumers

- **Library** (`omega_library_insert` duplicate refusal, `find_by_id`, dependency DAG): keyed
  by the v2 id. Two programs with one body and one contract but different names are now one
  program: the second insert is refused as a duplicate. `find_by_name` is unchanged.
  An all-zero (no body) id is refused.
- **Synthesis**: candidate ids now separate different bodies; the equivalence pruning uses
  behaviour signatures and is unchanged.
- **Realization synthesis**: `(program_id, machine_id, realization)` triple ids change
  because the program id changed; the law is unchanged.
- **Discovery**: candidates with different slices no longer share an id.
- **Crumbline** (`cl_steps_build_program`): ids change; a step constant above 32 bits now
  fails the build instead of producing a program whose code disagrees with its steps.
- **MetaSkills / workflow fusion, plan cache, action graph**: they identify by action-graph
  digests (`rx_graph_identify`, `rx_fusion`), never by `program_id`. Unaffected.
- **Visor**: `fn` redefinition with a different body now yields a distinct id and simply
  rebinds the name (section 6). The session's collision guard stays as an internal invariant
  check (same id but different realization code means a builder bug), with a new message.

## 6. Visor behaviour change

Before: `fn t(x: u64) -> u64 { x + 1 }` then `fn t(x: u64) -> u64 { x + 2 }` failed with
"identity collision ... the program id does not cover the body".
Now: both succeed, the ids differ, the second definition is added to the program table and
`t` is rebound to it. `fn other` with `transform`'s exact body and contract returns
`transform`'s id and reuses its table entry (the entry keeps its first name).

## 7. Changed golden ids (old v1 -> new v2)

Every program id changes (new preimage). The ids pinned in tests, and why:

| where | program | old (v1) | new (v2) |
|---|---|---|---|
| `tests/language/golden/v0.txt` P rows 1-3, `tests/visor/sessions/e2e.expected` (2 lines) | `fn transform(x: u64) -> u64 requires true ensures result >= 1 { x * 2 + 1 }` (and its two respellings) | `beedf7f6fc3853c0aed82e0b35e165b14878a82b0ebac64aa5bd9430800a8c4f` | `83bd39597742d3bb83e5140c662532c0a2ecb59e5f868e38be16276981644543` |
| `tests/language/golden/v0.txt` P row 4 | `fn mask(x: u64) -> u64 { x & 0xff }` | `7f6104b6cbf7b1e87055458fa9de9794067893186a84991b35b2a435c06ee244` | `e180baf4807d890dc1ba22472e847da76e877ee04d59b0762f6dfc26b2af62ff` |

Reason for all: v2 hashes the domain tag, the canonical body root and the contract type
object ids, and no longer hashes the name or cost. The realization code bytes in the same
rows (and every `E` row) are unchanged. Behaviour changes in tests:

- `tests/language/test_language_v0.c`: "different fn name -> different id" became "same id,
  table entry reused"; the table count after the name/ensures cases is 2 (was 3); the
  "different body is refused as a collision" case became "distinct id, name rebound"; the
  table-full probe uses `x + 1000` (the old probe `x + 1` now equals `fill1` and is reused).
- `tests/visor/qualification/run_qualification.sh`: `program-id-collision-refused` became
  `program-id[body-bound: x+1 != x+2 after clear]` and `program-id-redefine-distinct`.
- Discovery demo (`make test-m11`): the selected abstraction is unchanged (mul2;add1,
  f(50) = 101); its id changed `c1971e4a...` -> `52b6059b...`.

The omegatool M4-M14 gate suites needed no change.

## 8. Non-claims and deferred defects

- Identity is **intensional**: it names the canonical body, not the mathematical function.
  `x + 1 + 1` and `x + 2` compute the same function and have different ids. Proving
  extensional equality is out of scope.
- `omega_program_compose` derives the postcondition as the text `"(B)o(A)"`, so
  `compose(compose(a,b),c)` and `compose(a,compose(b,c))` have the same body but different
  postcondition ids, hence different program ids. The representation-invariance gate holds
  the contract fixed to isolate the body; the non-associative derived contract text is a
  deferred defect of M8 contract derivation, not of identity.
- Contract clauses are identified by their canonical **text** (`omega_build_constraint_id`);
  two logically equivalent clauses with different text differ.
- Historical receipts under `evidence/` carry v1 ids and are append-only; they are not
  rewritten. The `omega.program.v2` tag is what keeps them from being confused with v2 ids.
- The body model is the V0 unary step chain (u64 -> u64, five ops, one constant each). Richer
  bodies need a richer `OmegaProgramBody`; the identity law (root of the canonical body
  graph + contract) does not change.
- Payload bytes are the existing host-struct canonical payloads (spec/omega-language-v0.md),
  so ids are pinned to the LP64 / 4-byte-enum ABI, as before.

## 9. Qualification

`make test-program-id` (tests/program/test_program_id.c, physics-free, runs in the CI AArch64
job) prints five gates:

| gate | what it proves |
|---|---|
| `OMEGA_PROGRAM_ID_BODY_BOUND_PASS` | `x + 1` vs `x + 2` differ through `omega_program_build_unary_op`, through Visor lowering, and under an identical name + contract + cost (the v1 collision); composition order separates; a body-less program gets no id; a 33-bit constant is refused; the v2 id differs from the v1 recipe |
| `OMEGA_PROGRAM_ID_REPRESENTATION_INVARIANT_PASS` | name, cost, realization bytes and pointer values do not move the id; six Visor spellings (whitespace, comments, parameter name, operand side, literal spelling, fn name) share one id; library composition equals Visor lowering and re-association is invariant when the contract is held fixed; a hand-built body graph in 8 scrambled allocation orders has the same root; lift(realization) == body |
| `OMEGA_PROGRAM_ID_MUTATION_SEPARATION_PASS` | seeded differential over 3000 random programs (every pair, ~4.5M comparisons): id equal iff a hash-free structural oracle says body + contract equal; 2000 programs x 8 single mutations (op, constant, width, output width, pre, post, step added, step removed) always separate |
| `OMEGA_PROGRAM_ID_LIBRARY_REGRESSION_PASS` | library duplicate / admission / no-body refusal / DAG, discovery lift to mul2;add1 and admission, refactor keeps identity, realization triple id follows the program id, 64-step capacity |
| `OMEGA_PROGRAM_ID_VISOR_REGRESSION_PASS` | Visor session: distinct ids after `clear`, redefinition rebinds, aliases and respellings reuse the entry, stored realization == emit(body) |

`tools/program_id_receipt.sh` (shell only) runs these plus the Visor suites, the Visor V1
qualification campaign, the omegatool M8-M14 and Crumbline consumers and the non-energy host
regression suites on the committed clean tree, and writes a content-addressed receipt under
`evidence/PROGRAM_ID/`.
