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

## 3. Arenas

### 3.1 Subset statement

OSC-2 item 3 wires the OSC-0B region rules (model events REGION_OPEN, ALLOC with a region,
REGION_DESTROY) into the compiler. An arena is a bounded block of cells. Objects allocated
in it are never released one by one: the whole arena is destroyed when its block ends.
OSC-2 slice; not a general Omega compiler; no self-hosting.

### 3.2 Grammar

```
stmt     := ... | "arena" NAME "bound" INT block        (INT = K, 1..64 cells)
let      := ... | "let" NAME ":" "own" "[" scalar ";" INT "]" "in" NAME "=" "alloc" "(" expr ")" ";"
              | "let" NAME ":" "own" NAME "in" NAME "=" NAME "{" init ("," init)* "}" ";"
```

- `arena` is now a keyword. `in` (already a keyword for `for`) also names the arena of an
  allocation.
- The arena name is visible only inside its block, so allocating from an arena outside its
  block is impossible: the name is undefined there (UNDEFINED_NAME).
- An arena name is not a value. Any other use of it is TYPE_MISMATCH (`arena used as a
  value`). It cannot be passed to a function or returned.

### 3.3 Semantics

- **Capacity.** K counts 8-byte cells. `[T; N]` uses N cells and a struct uses its cell
  count (section 2.3). An allocation is *definite* when it is in the arena's own
  straight-line code: same loop depth and same branch depth as the `arena` statement
  (nested arena blocks count as straight-line code). The checker adds up definite
  allocations; going over K is refused (ARENA_CAPACITY). Every other allocation (inside a
  `while` / `for` body or an `if` / `else` branch) is checked at run time: past K the run
  traps with TRAP ARENA_FULL (11) in both the interpreter and native code.
- **Runtime.** An arena takes one slot of the 64-slot pool (the lowest free one, like any
  allocation; no free slot is TRAP OOM) and bump-allocates its objects inside that slot's
  cells. Cells are filled with the initial value at allocation and zeroed at destroy.
- **Destruction order (deterministic).** At the end of an arena block (normal end, or a
  `return` from inside it):
  1. the unique owners declared inside the block are released, in reverse declaration
     order (OSC-1 rule);
  2. then the arena is destroyed: one REGION_DESTROY frees all its objects at once.
  Nested arenas: the inner block ends first, so the inner arena is destroyed before the
  outer one. A `return` destroys every enclosing arena, innermost first, interleaved with
  the releases of each scope. `tests/compiler/progs/arena_dtor.osc` asserts the exact event
  sequence (alloc 1, open 2, arena alloc 3, open 4, arena alloc 5, alloc 6, release 6,
  destroy 4, alloc 7, release 7, destroy 2, release 1) in both engines.
- **Borrows.** `&x` and `&mut x` of an arena object follow the OSC-1 lexical rules
  (MUTABLE_ALIAS, READ_ONLY_BORROW, `&` / `&mut` parameters). A borrow of an arena object
  may not outlive the arena: assigning it to a borrow binding declared outside the arena
  block, or returning it, is ARENA_ESCAPE. The model rejects the same situation (a
  REGION_DESTROY while a borrow of one of its objects is live) as `arena-escape`.
- **Moves (slice restriction).** Every move that involves an arena object is refused with
  ARENA_MOVE: `let y: own .. = x;` with `x` in an arena (`move out of arena`), passing `x`
  to an `own` parameter (`move into own parameter`), and `let x: own .. in r = y;`
  (`move into arena`). The OSC-0B model allows a move inside a region (the new owner
  inherits the region); this slice is stricter than the model and refuses all of them.
- **Model trace.** The checker trace gains REGION_OPEN (at `arena`), ALLOC with the region
  id (for `in r`) and REGION_DESTROY (at block end), with per-function region ids 1..16.
  Arena objects get no RELEASE event. Golden and fuzz traces replay through the model with
  ACCEPT; an ARENA_ESCAPE refusal ends its trace in the refused REGION_DESTROY, which the
  model rejects with `arena-escape` (model-agreement test).
- **Runtime event log.** `osc_rt` logs REGION_OPEN (slot, K, region serial), ARENA_ALLOC
  (slot, length, object serial) and REGION_DESTROY next to ALLOC / RELEASE. Interpreter and
  native code produce identical logs (checked on every differential run). Releasing an arena
  slot with RELEASE is a runtime invariant violation (TRAP RUNTIME, never seen for checked
  code).
- **IR.** Three instructions are added. `AOPEN dst, K` puts a u64 arena handle in `dst`.
  `AALLOC dst, init, h` allocates `dst` (an own reference or struct) from handle `h`.
  `ADESTROY h` destroys the arena. The validator enforces a handle discipline: a handle
  register is defined once, by AOPEN only, is never a parameter, and is used only as the
  handle operand of AALLOC / ADESTROY. Native code calls the runtime through three new
  OscRt entry points at offsets 24, 32 and 40 (`arena_open`, `arena_alloc`,
  `arena_destroy`).
- **Encoding.** A unit with no arena instruction encodes exactly as before (version byte 1
  without structs, 2 with structs), so every existing program keeps byte-identical IR and
  machine code (checked: all 31 earlier golden programs and all 82 earlier negatives give
  the same oscc output as origin/main). A unit with arena instructions uses version byte 3,
  always writes the struct table, and encodes the new instructions.

### 3.4 Diagnostics

New kinds (appended; existing kinds unchanged):

| Kind | Transition | Cause |
|---|---|---|
| ARENA_ESCAPE | `outer borrow assigned a borrow of an arena object` / `borrow of an arena object returned` / `borrow live at arena end` | a borrow of an arena object outlives the arena |
| ARENA_MOVE | `move out of arena` / `move into own parameter` / `move into arena` | any move of an arena object (slice restriction) |
| ARENA_CAPACITY | `arena bound outside 1..64` / `arena capacity exceeded` | K not in 1..64; definite allocations over K |

`borrow live at arena end` is a defence in depth: with lexical borrows the two other
ARENA_ESCAPE checks catch every escape first, so no test reaches it.

Existing kinds used by arenas:

- **UNDEFINED_NAME:** `allocate in undefined arena` (unknown name, or an arena used after
  its block).
- **TYPE_MISMATCH:** `in a non-arena` (`in x` where `x` is not an arena), `arena used as a
  value`.
- **REDEFINED_NAME:** an arena name that shadows a visible name (no shadowing, OSC-1 rule).

New trap: TRAP ARENA_FULL = 11 (`OSC_TRAP_ARENA_FULL`, now `OSC_TRAP_MAX`).

### 3.5 Limits

- K is 1..64 cells (one pool slot). An arena uses one of the 64 pool slots while open.
- The static model trace holds 16 arenas per function (the model's region capacity). A function
  with more still compiles, but its trace is marked overflow and is not replayed (same rule
  as the 64-object and 64-borrow limits).
- Objects are never moved into, out of, or between arenas.
- Arena objects cannot be released early; there is no per-object free.
- Arena names are not values: no arena parameters, returns, or arenas outliving their block.

### 3.6 Evidence

- Golden programs:
  - `arena_basic.osc`: basic allocation, `return` from inside an arena, unique owners mixed
    with arena objects.
  - `arena_nested.osc`: nested arenas, allocation into the outer arena from the inner
    block, sibling arenas reusing a slot.
  - `arena_structs.osc`: struct literals in an arena.
  - `arena_borrows.osc`: `&` / `&mut` borrows and parameters on arena objects.
  - `arena_full.osc`: allocations in a loop and in a branch, TRAP ARENA_FULL.
  - `arena_dtor.osc`: destruction order (event sequence above).
- Negatives in `tests/compiler/neg/ar_*.osc` (14), each asserting kind, object, line, and
  transition: escape through an outer borrow, escape by return, move out, move into, move
  into an own parameter, static capacity, bound 0, bound 65, undefined arena, `in` a
  non-arena, arena shadowing, allocation after the block, arena object used after the
  block, arena used as a value. The two ARENA_ESCAPE negatives also pass the
  model-agreement check (`ARENA_ESCAPE` maps to the model's `arena-escape`).
- `test_osc_backend` adds the arena validator cases (bad bound, handle type, double
  definition, allocation or destroy from a non-handle, handle used as a value or redefined).
- `test_osc_compiler` adds:
  - the arena destruction-order event check (`arena destruction order: ...`);
  - **arena fuzz:** generated units with nested arenas of random bound, array and struct
    allocations into any visible arena, unique owners, borrows passed to `&` / `&mut`
    parameters, and allocations in loops and branches. Every unit compiles, native and
    interpreter outcomes are identical (`arena fuzz: ... mismatches=0`) in the plain and
    ASan/UBSan runs, and the fuzz must hit TRAP ARENA_FULL;
  - **runtime model replay:** the runtime event log of every native run in the test
    (golden fuzz, expect-run lines, contract, struct and arena fuzz, destruction-order
    tests) is replayed through the OSC-0B model. A run that returns replays fully; a run
    that traps replays the prefix logged before the trap. Object and region ids are
    windowed when a run exceeds 64 objects or 16 regions (a new window re-opens the open
    regions and re-allocates the live objects). The line
    `runtime model replay: runs=.. events=.. accepted=.. rejected=0` must report
    `rejected=0`. **This checks the runs that were executed. It is not a proof that every
    program's runtime behaviour satisfies the model.**
- `make osc2-receipt ITEM=arenas` records the arena fuzz lines, the destruction order, the
  runtime model replay lines (plain and ASan) and the ARENA_FULL run count.
