# OSC-2: the second compiler slice (design, normative)

OSC-2 slice; not a general Omega compiler; no self-hosting.

OSC-2 extends the OSC-1 slice (`docs/osc/OSC-1-DESIGN.md`) item by item. Everything
OSC-1 states stays in force unless a section below changes it. Each item ships with
its own golden programs, named negatives, fuzz coverage and a receipt
(`make osc2-receipt ITEM=<name>`, schema `omega.osc2.compiler.receipt.v1`, written to
`evidence/OSC-2/receipts/osc2-<item>-<sha256>.json`).

## 1. Contracts

### 1.1 Subset statement

OSC-1 parsed `requires` / `ensures` and recorded them as text only. OSC-2 item 1 makes
them enforced contracts:

- a clause is a pure `bool` expression of the OSC-1 expression grammar;
- a clause that constant folding decides false is refused at compile time;
- every other clause is checked at run time, by a check lowered into the IR, so
  native code and the reference interpreter run the same check and report the same trap;
- there is no symbolic proof: anything constant folding cannot decide is left to the
  run-time check.

Functions without clauses compile to byte-identical IR and machine code compared with
OSC-1 (checked for every program in `tests/compiler/progs/` that existed before OSC-2).

### 1.2 Grammar change

```
fn_decl   = "fn" ident "(" [ params ] ")" [ "->" type ]
            [ "requires" expr ] [ "ensures" expr ] block ;
```

`clause = { token }` from OSC-1 section 2 is replaced by `expr`. The clause must be
followed by `ensures` (after `requires`) or by the function's `{`. An empty clause is a
SYNTAX error. The source text is still recorded in `OscFunc.requires_text` /
`ensures_text` (it is part of the canonical IR encoding, unchanged from OSC-1), rebuilt
from the clause tokens with the OSC-1 spacing rule. Longer than 255 bytes is CAPACITY.

### 1.3 Names a clause may use

| clause    | may use |
|-----------|---------|
| `requires` | parameters; element reads `p[i]` through any array parameter (`own`, `&`, `&mut`) |
| `ensures`  | parameters; `result` (the return value) in a non-void function; element reads only through shared `&` parameters |

- Contracts are pure. A call inside a clause is refused: CONTRACT_INVALID, transition
  `call in contract`, object = callee. (This rules out side effects and keeps the
  evaluation order of a contract trivially the same in both engines.)
- `result` in `requires`: CONTRACT_INVALID, object `result`, transition `result in requires`.
- `result` in the `ensures` of a void function: CONTRACT_INVALID, object `result`,
  transition `result in void function`.
- An element read in `ensures` through an `own` or `&mut` parameter: CONTRACT_INVALID,
  object = the array, transition `array read in ensures`. (The body may have changed or
  moved that array; a shared borrow cannot change while the call runs.)
- `result` is a name only inside an `ensures` clause. A parameter named `result` in a
  function that has an `ensures` clause is REDEFINED_NAME (transition
  `parameter named result with ensures`).
- Any other name is UNDEFINED_NAME (locals of the body are not in scope).
- A clause that is not `bool` is TYPE_MISMATCH with object `requires` / `ensures`. All
  other OSC-1 expression rules (AMBIGUOUS_WIDTH, static OVERFLOW folding, ...) apply
  unchanged.
- Parameters are immutable, so `ensures` sees the values the function was called with.
- An element read in `requires` is traced as a read at function entry (ownership trace).

### 1.4 Static enforcement (constant folding only)

The checker evaluates clauses with the same arithmetic as the interpreter (wrap for
unsigned, checked for signed). A sub-expression that would trap, an element read and a
call are "unknown". `&&` and `||` are decided left to right, so `false && x` is false and
`true || x` is true. Three refusals, all CONTRACT_VIOLATION with `origin_line` = the line
of the clause and `other` = the clause text:

| transition | line | object | when |
|------------|------|--------|------|
| `requires is constant false` / `ensures is constant false` | clause | function | the clause folds to false on its own |
| `requires false at call` | call | callee | the arguments of a call fold to constants that make the callee's `requires` fold to false |
| `ensures false at return` | `return` | function | a `return` with a constant value makes `ensures` fold to false |

### 1.5 Run-time checks

- `requires` is checked once, on the callee side, at function entry, before the first
  statement. False: TRAP REQUIRES (code 9).
- `ensures` is checked at every `return` (and at the fall-off end of a void function)
  after the return value is computed and before the releases of the scopes being left.
  False: TRAP ENSURES (code 10).
- A contract trap ends the run like every OSC-1 trap: no destructors run; owners that were
  live stay live in the pool log.
- A clause obeys the checked semantics: signed overflow inside a clause traps OVERFLOW,
  a dynamic out-of-range element index traps BOUNDS, and so on, before the contract's own
  verdict.
- Lowering: `v = clause; CBR v ok bad; bad: TRAP code; BR ok; ok: ...`. The interpreter
  executes the same instructions, so both engines report the identical trap.

Trap codes 9 and 10 are appended to the OSC-1 list; existing codes are never renumbered.
`OSC_TRAP_MAX` = 10. The back end emits a trap stub only for codes a unit uses, so units
without contracts produce the same bytes as OSC-1.

### 1.6 Elision

- A clause that folds to true on its own gets no run-time check at all.
- A `return` whose constant value makes `ensures` fold to true gets no check at that return.

### 1.7 Evidence

- Golden programs: `tests/compiler/progs/contracts_basic.osc`, `contracts_requires.osc`,
  `contracts_ensures.osc` (passing calls, TRAP REQUIRES, TRAP ENSURES, OVERFLOW and
  BOUNDS inside a clause, void `ensures`, elided clauses), each with hand-computed
  `expect-run` lines checked in both engines.
- Negatives in `tests/compiler/neg/`: `cv_requires_false_at_call`, `cv_constant_false_clause`,
  `cv_ensures_static`, `cv_ensures_constant_false`, `ci_result_in_requires`,
  `ci_result_in_void`, `ci_call_in_contract`, `ci_array_read_ensures`,
  `tm_non_bool_clause`, `undefined_in_clause`.
- Contract fuzz (`test_osc_compiler`): generated units whose functions carry random
  `requires` / `ensures`; each unit compiles or is refused with CONTRACT_VIOLATION only;
  compiled units run native vs interpreter with 0 mismatches and must hit both
  TRAP REQUIRES and TRAP ENSURES.

## 2. Structs

### 2.1 Subset statement

OSC-2 item 2 adds fixed-layout structs to the OSC-1 subset. A struct is a named group of
fields that lives in one pool allocation and is handled exactly like an owned array: it is
created by its owner, moved, borrowed with `&` / `&mut`, and released at scope end. A struct
is never a value: there is no struct copy, compare, or return. OSC-2 slice; not a general
Omega compiler; no self-hosting.

### 2.2 Grammar

```
unit     := (struct | fn)*
struct   := "struct" NAME "{" field ("," field)* "}"
field    := NAME ":" ftype
ftype    := scalar | "[" scalar ";" INT "]"            scalar = integer type or bool
type     := ... | "own" NAME | "&" NAME | "&mut" NAME  (NAME = a declared struct)
let      := ... | "let" NAME ":" "own" NAME "=" NAME "{" init ("," init)* "}" ";"
init     := NAME ":" expr                              (scalar field)
          | NAME ":" "[" expr ";" INT "]"              (array field, INT = its length)
expr     := ... | NAME "." NAME | NAME "." NAME "[" expr "]"
stmt     := ... | NAME "." NAME "=" expr ";" | NAME "." NAME "[" expr "]" "=" expr ";"
```

- `struct` declarations sit at unit level, before any function that names them.
  `struct` is now a keyword and `.` a token.
- A struct literal appears only as the initialiser of `let x: own S = S { ... }`.
  Fields may be listed in any order. The values are evaluated in source order.
- A local, parameter, or binding of struct type is always `own S`, `&S`, or `&mut S`.

### 2.3 Layout (fixed, part of the IR digest)

- Every field takes whole 8-byte cells, in declaration order. A scalar takes 1 cell and
  `[T; N]` takes N cells. Field offsets are prefix sums, with no padding and no reordering.
  Example: `struct L { a: u8, b: [i16; 3], c: bool, d: [u64; 2], e: i64 }` has offsets
  0 1 4 5 7 and uses 8 cells. `test_osc_compiler` asserts this.
- A struct owner is one pool allocation of `ncells` cells. This uses the same `alloc` /
  `release` events and the same 64-slot pool as arrays.
- IR: `OscUnit.structs[]` records each struct's name, fields (name, scalar type, array length,
  offset) and cell count. `osc_ir_validate` re-checks the prefix sums. A struct reference
  type is `OSC_T_REF` with `sid` = struct index + 1. Two new instructions are added,
  `FLOAD dst, a, idx|-1, field` and `FSTORE a, idx|-1, value, field`. FSTORE needs an own or
  `&mut` reference.
- Encoding: a unit with no structs encodes exactly as in OSC-1 (version byte 1), so every
  existing program keeps byte-identical IR and machine code. A unit with structs uses version
  byte 2 and adds the struct table (names, field types, lengths, offsets, cell counts), so
  the digest binds the layout. Reordering two fields changes the digest.

### 2.4 Semantics

- **Creation.** `let s: own S = S { f: e, ... }` allocates one object and stores every
  field. An array field `[e; N]` fills all N elements with `e`.
- **Field access.** `s.f` reads and `s.f = v` writes. For an array field, `s.f[i]` reads and
  `s.f[i] = v` writes, with the same bounds checks as arrays: a constant index outside
  0..N-1 is STATIC_OUT_OF_BOUNDS, and a dynamic one traps with TRAP BOUNDS (3) in both the
  interpreter and native code. A field is used at its declared type. Arithmetic follows the
  OSC-1 rules (wrap for unsigned types, overflow trap for signed types).
- **Ownership (OSC-0B, as for arrays).**
  - `let t: own S = s;` and passing `s` to an `own S` parameter are moves. Any later use of
    `s` is USE_AFTER_MOVE.
  - `&s` and `&mut s` follow the same lexical borrow rules as arrays: no `&mut` together with
    any other live borrow (MUTABLE_ALIAS), no write through `&` (READ_ONLY_BORROW), and no
    borrow outliving its owner (BORROW_OUTLIVES_OWNER).
  - Struct borrows produce the same model trace events, which are replayed against the
    OSC-0B model.
- **Destruction.** Owners are released at scope end in reverse declaration order, structs
  and arrays alike. A moved-from owner is not released again.
  `tests/compiler/progs/structs_dtor.osc` asserts the event sequence alloc 1 2 3,
  release 3 2, alloc 4, release 4 1 in both engines.
- **Functions.** A struct passes as `own S`, `&S`, or `&mut S`. Returning a struct (any
  `-> S`, `-> own S`, `-> &S`) is refused as UNSUPPORTED, transition `struct return type`.
  Bare `S` as a parameter or local type is UNSUPPORTED (`struct by value`).
- **Contracts.**
  - `requires` may read fields (and array-field elements) through any struct parameter.
  - `ensures` may read them only through a shared `&S` parameter; otherwise the clause is
    CONTRACT_INVALID (`struct field read in ensures`). This is the same rule as array
    element reads (section 1.3): an `own` or `&mut` parameter may have changed by the time
    of the return.

### 2.5 Diagnostics

New kinds (appended; existing kinds unchanged):

| Kind | Transition | Cause |
|---|---|---|
| UNKNOWN_FIELD | `access unknown field` / `initialise unknown field` | `s.z` or `S { z: .. }` with no field `z` |
| DUPLICATE_FIELD | `field initialised twice` / `field declared twice` | a field given twice in a literal or declaration |
| MISSING_FIELD | `field not initialised` | a literal omits a field |
| UNDEFINED_TYPE | `undefined struct type` | a type name that is not a declared struct |
| RECURSIVE_STRUCT | `struct field of its own type` | a field whose type names the struct itself |

Existing kinds used by structs:

- **TYPE_MISMATCH:**
  - a field given a value of the wrong type
  - a literal of another struct
  - an array field set without `[e; N]`, or with the wrong N
  - indexing a struct, or `.f` on an array
  - indexing a scalar field, or using an array field without an index
  - a struct used as a scalar
  - a struct passed or moved where another struct type is needed
- **UNSUPPORTED:**
  - `empty struct`
  - `struct-typed field`
  - `owner or borrow field`
  - `struct by value`
  - `struct return type`
  - `alloc of a struct`
- **CAPACITY:** `struct capacity`, `field capacity`, `struct size`.
- **REDEFINED_NAME:** `struct declared twice`.
- **STATIC_OUT_OF_BOUNDS** on `s.f[k]`.
- **CONTRACT_INVALID:** `struct field read in ensures`.

### 2.6 Limits

- At most 16 structs per unit.
- Each struct has 1 to 16 fields and at most 64 cells.
- An array field is `[T; N]` of any integer type or bool, N >= 1.
- Fields cannot be structs, owners, or borrows.
- Struct literals appear only in `let own` initialisers.

### 2.7 Evidence

- Golden programs:
  - `structs_fields.osc`: layout use, field read/write, array fields, TRAP BOUNDS.
  - `structs_owner.osc`: moves, own / `&` / `&mut` parameters, borrow bindings.
  - `structs_contracts.osc`: TRAP REQUIRES and TRAP ENSURES on fields.
  - `structs_dtor.osc`: destruction order.
- Negatives in `tests/compiler/neg/st_*.osc` (36), each asserting kind, object, line, and
  transition. They include:
  - unknown field
  - duplicate field init
  - missing field init
  - field type mismatch
  - write through shared borrow
  - use after move
  - mutable alias
  - borrow outlives owner
  - recursive struct
  - undefined struct type
- `test_osc_compiler` adds:
  - the layout assertion, with the digest binding the layout and struct-free units still
    using version 1;
  - the destruction-order event check;
  - **struct fuzz:** generated units with random field types, literals, field writes,
    moves, and `&` / `&mut` / own calls with `requires` on fields. Every unit compiles,
    and native and interpreter outcomes are identical (`struct fuzz: ... mismatches=0`) in
    the plain and ASan/UBSan runs. The fuzz must hit TRAP BOUNDS and TRAP REQUIRES.
