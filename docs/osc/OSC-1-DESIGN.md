# OSC-1: the first compiler slice (design, normative)

**OSC-1 slice; not a general Omega compiler; no self-hosting.**

Status: implemented as a host-tested slice (Lane 22, 2026-10-01). Governing record:
[OMEGA-SYSTEMS-CORE-0000](../adr/OMEGA-SYSTEMS-CORE-0000.md) and the audit
sections it cites ([OMEGA_SYSTEMS_CORE_CODE_AUDIT.md](../../OMEGA_SYSTEMS_CORE_CODE_AUDIT.md)
III.6 pipeline, III.7 OSC-1 definition, III.8 acceptance tests, II.9 determinism,
II.11 executable model).

## 1. What OSC-1 is

OSC-0 III.7 defines OSC-1 as "the smallest vertical slice: source -> parser ->
typed value -> unique allocation -> borrow -> bounds check -> deterministic
destruction -> native execution". This slice implements exactly that path, for
a small language that extends V0 (spec/omega-language-v0.md), plus the integer
and control-flow core needed to write programs that exercise it:

- integer scalars u8 u16 u32 u64 (wrap, V0 compatible) and i8 i16 i32 i64
  (checked: overflow is a deterministic trap, OSC-0 III.3), and bool;
- `let` bindings (immutable by default, `let mut` for reassignment);
- arithmetic, bitwise, shift, comparison, logical operators;
- `if` / `else`, `while ... bound N` and `for i in A .. B` loops whose trip count
  has a static bound;
- calls to functions defined earlier in the same unit (no recursion);
- unique allocation `own [T; N]`, moves, shared and mutable borrows `&[T; N]` /
  `&mut [T; N]`, indexing with a bounds check, deterministic destruction at
  scope end;
- `requires` / `ensures` clauses parsed and recorded as text, **not checked** (superseded by OSC-2 item 1: `docs/osc/OSC-2-DESIGN.md` section 1)
  (OSC-0 decision 3, III.6). Enforcement is OSC-2 and mandatory before any
  production C migrates.

Pipeline (III.6 steps realised here): lexer/parser -> typed AST (disposable) ->
name/type resolution -> ownership + borrow analysis (lexical lifetimes) -> typed
IR (`src/compiler/osc_ir.h`, canonical encoding + SHA-256 digest) -> realization
to AArch64 through an in-repo encoder whose every emitted word is mirrored in a
decoder. Not in OSC-1: effects/capabilities (III.6 step 6), structs, strings,
generics, generations, arenas, unsafe physical, FFI, Flow IR optimisation.

## 2. Surface grammar (EBNF)

```
unit      = { fn_def } EOF ;
fn_def    = "fn" NAME "(" [ param { "," param } ] ")" [ "->" type ]
            [ "requires" clause ] [ "ensures" clause ] block ;
param     = NAME ":" ptype ;                        (* at most 6 params *)
ptype     = type | arr_type ;
arr_type  = "own" "[" type ";" INT "]"              (* owner, moved into the callee *)
          | "&" "[" type ";" INT "]"                (* shared borrow *)
          | "&" "mut" "[" type ";" INT "]" ;        (* mutable borrow *)
type      = "u8" | "u16" | "u32" | "u64" | "i8" | "i16" | "i32" | "i64" | "bool" ;
clause    = { token } ;          (* up to "ensures" / "{" at paren depth 0; recorded text *)
block     = "{" { stmt } "}" ;
stmt      = "let" [ "mut" ] NAME ":" type "=" expr ";"
          | "let" NAME ":" "own" "[" type ";" INT "]" "=" "alloc" "(" expr ")" ";"
          | "let" NAME ":" "own" "[" type ";" INT "]" "=" NAME ";"          (* move *)
          | "let" [ "mut" ] NAME ":" ( "&" | "&" "mut" ) "[" type ";" INT "]" "=" borrow ";"
          | NAME "=" ( expr | borrow ) ";"                (* mut scalar, or mut borrow binding *)
          | NAME "[" expr "]" "=" expr ";"
          | "if" expr block [ "else" ( block | if_stmt ) ]
          | "while" expr "bound" INT block
          | "for" NAME "in" INT ".." INT block             (* i: i64, half-open, literal ends *)
          | "return" [ expr ] ";"
          | call ";"
          | block ;
borrow    = "&" NAME | "&" "mut" NAME ;
expr      = lor ;
lor       = land { "||" land } ;                (* short-circuit *)
land      = cmp { "&&" cmp } ;                  (* short-circuit *)
cmp       = bor [ cmp_op bor ] ;                (* non-associative *)
bor       = bxor { "|" bxor } ;
bxor      = band { "^" band } ;
band      = shift { "&" shift } ;
shift     = add { ( "<<" | ">>" ) add } ;
add       = mul { ( "+" | "-" ) mul } ;
mul       = unary { ( "*" | "/" | "%" ) unary } ;
unary     = ( "-" | "~" | "!" ) unary | postfix ;
postfix   = primary [ "as" type ] ;
primary   = INT | "true" | "false" | NAME | NAME "[" expr "]" | call | "(" expr ")" ;
call      = NAME "(" [ arg { "," arg } ] ")" ;
arg       = expr | borrow ;                     (* an owner NAME as an `own` arg is a move *)
cmp_op    = "==" | "!=" | "<" | "<=" | ">" | ">=" ;
```

Literals, comments and identifiers are spelled as in V0 (decimal, `0x`, `0b`;
`//` and `/* */`). Precedence differs from V0 only by the added levels; every V0
operator keeps its V0 meaning. `free`, `drop`, `delete`, `dealloc` are reserved
and are a syntax error: destruction has no surface syntax, so a forgotten free
is inexpressible (III.8).

## 3. Static rules (all refusals are typed diagnostics, section 6)

Types. No implicit conversion: both operands of a binary operator have the
identical type (shift: the amount may be any integer type), comparison operands
have the identical type, `if`/`while` conditions and `&& || !` operands are
`bool`. `as` converts between integer types only (checked at run time). The
array element type is an integer type or bool; N is 1..64.

Literals. A literal gets its type from the context (let annotation, the other
operand, the parameter / return / element type it is passed to, the `as`
source is refused for literals). A literal with no context is refused
(ambiguous width, as in V0); a literal that does not fit its type is refused
(`overflow-unsafe`).

Overflow-unsafe, refused at compile time: a constant expression of a signed
type whose value overflows; a literal shift amount >= width; a literal zero
divisor; a negative literal for an unsigned type.

Names. Every name is defined before use, in an enclosing scope. Shadowing in
an inner scope is refused (keeps diagnostics unambiguous). Functions are called
only after their definition (no recursion, no forward reference).

Loops. `while` without `bound N` is refused (`unbounded loop`). `for` ends must
be integer literals with A <= B. N and B - A are at most 1_000_000. A `while`
that would start iteration N+1 traps with OSC_TRAP_LOOP_BOUND.

Return. Every path of a non-void function ends in `return expr;`. A function
returns a scalar only (no `->` arr_type in OSC-1).

### 3.1 Ownership and borrows (lexical lifetimes)

Objects: each `alloc(...)` creates one unique allocation owned by the binding.
An owner binding is in one of: `owned`, `moved`. A borrow binding (or a borrow
argument for the duration of a call) holds a borrow of one or more owners.

- Move: `let b: own[..] = a;` or passing owner `a` to an `own` parameter. After
  a move, any use of `a` (read, index, borrow, move) is refused: **use after
  move**. A move inside a loop body of an owner declared outside that loop is
  refused (use after move on the next iteration). A move on only some paths of
  an `if` is refused (`conditional move`): OSC-1 has no drop flags.
- Shared borrow `&a`: allowed while `a` is owned and not mutably borrowed.
- Mutable borrow `&mut a`: `a` must be owned and not borrowed at all; through a
  borrow binding `r: &mut`, `&mut r` is a reborrow of the same target.
- While a borrow of `a` is live (borrow binding in scope, or a call argument
  during the call): moving `a`, storing into `a[i]` directly, or taking
  `&mut a` with any other live borrow is refused: **mutable alias**. A call
  `f(&mut a, &a)` or `f(&mut a, &mut a)` is a mutable alias. While a `&mut`
  borrow of `a` is live, any direct use of `a` is refused (mutable alias).
- **Borrow outlives owner**: a borrow binding declared in an outer scope is
  assigned a borrow of an owner declared in an inner scope (the owner is
  destroyed while the borrow is still in scope); also refused: returning or
  storing a borrow in any other place (OSC-1 has nowhere else to put one).
- Indexing: `a[i]` reads through an owner or any borrow; `a[i] = v` writes
  through an owner (not borrowed) or a `&mut` borrow binding. A shared borrow is
  read only. **Statically known out-of-bounds**: an index that is an integer
  literal (or a constant expression) outside 0..N-1 is refused at compile time.
  Any other index is checked at run time: out of range is a deterministic trap
  OSC_TRAP_BOUNDS.
- Destruction: at the end of each scope, every owner declared in it that was
  not moved is destroyed (OSC_I_RELEASE) in reverse declaration order,
  including on early `return`. An `own` parameter is an owner of the function
  scope. There is no other way to free.

The ownership analysis records, per function, the event sequence it checked
(alloc, borrow_shared, borrow_mut, end_borrow, move, use, release, scope end).
The tests replay these sequences through the II.11 executable model
(`src/compiler/model/`) and require the model's verdict to agree with the
compiler's (accept/accept, or the same named invalid transition).

## 4. IR

`src/compiler/osc_ir.h` is the data definition. Notes:

- vregs 0..nparams-1 are the parameters. Each named local gets one vreg;
  temporaries get fresh vregs. Every vreg has one type.
- The front end lowers `&&`/`||` to branches, loops to blocks with an explicit
  counter vreg, CMP and CBR on the bound, and `OSC_I_TRAP 4` when exceeded.
- Release instructions are emitted by the front end at every scope exit.
- The IR has a canonical little-endian encoding with no padding
  (`osc_ir_encode`); the SHA-256 of that encoding is the unit's identity.
  Source spelling, comments and whitespace never reach it.

## 5. Semantics shared by interpreter and native code

Values: 64-bit canonical (unsigned zero-extended, signed sign-extended, bool
0/1). Unsigned results are reduced modulo 2^w. Signed results must be
representable or the run traps with OSC_TRAP_OVERFLOW. Division truncates
toward zero; remainder has the sign of the dividend; divisor 0 traps DIV0.
Shift amount must be in 0..w-1 or the run traps SHIFT. Signed `<<` traps
OVERFLOW if the result is not representable. `as` traps CAST if the value is
not representable in the target type. Array elements are 8-byte canonical
cells; allocation initialises every element to the given value; release zeroes
the cells. Allocation takes the lowest free pool slot; pool exhaustion traps
OOM. A trap ends the whole run: no destructors run (no unwinding, OSC-0
decision 6); the harness resets the pool.

Observable outcome of a run = (trap code, return value if no trap, the pool
event log). The interpreter and native code must agree on all three.

## 6. Diagnostics

Every refusal is an `OscDiag` with: `kind` (enum, below), `line`, `col`,
`object` (the name involved), `origin_line` (where that object / borrow was
created), `other` (the conflicting borrow or name, if any), `transition` (the
attempted transition as text, e.g. "use after move", "borrow_mut while
borrow_shared live"). Kinds: SYNTAX, UNSUPPORTED, CAPACITY, UNDEFINED_NAME,
REDEFINED_NAME, TYPE_MISMATCH, AMBIGUOUS_WIDTH, OVERFLOW_UNSAFE,
UNBOUNDED_LOOP, MISSING_RETURN, USE_AFTER_MOVE, CONDITIONAL_MOVE,
MUTABLE_ALIAS, BORROW_OUTLIVES_OWNER, STATIC_OUT_OF_BOUNDS, IMMUTABLE_ASSIGN,
READ_ONLY_BORROW. Negative tests assert the kind and the fields.

## 7. Native ABI (AArch64, Linux host)

- Each function: params in x0..x5, return in x0 (bool 0/1, canonical
  extension), hidden runtime pointer `OscRt *` in x7.
- Frame: `stp x29, x30, [sp, #-16]!; mov x29, sp; sub sp, sp, #F` with F =
  8 * nvregs + 8 rounded up to 16. Slot 0 holds x7; vreg v lives at
  `[sp, #8 * (v + 1)]`. Every instruction loads its operands from slots into
  x9..x15 and stores its result to the destination slot (no register
  allocation in OSC-1; deterministic and simple).
- Calls between compiled functions use BL; runtime entry points are reached by
  `ldr x16, [x7_slot]`, `ldr x16, [x16, #off]`, `blr x16`.
- Encoder: `src/compiler/osc_a64.c`. Every emitter refuses an out-of-range
  field (no silent masking), SP and XZR are distinct operand kinds, and every
  emitted word decodes through `osc_a64_decode` back to the same fields.
  The existing `src/aarch64_encoder.c` is not changed.

## 8. Determinism (II.9)

Compiler output is a pure function of the source bytes: no timestamps, paths,
environment, pointer values or hash-order dependence. Gate: compile every
corpus program twice in separate processes and compare IR digests and machine
code bytes.

## 9. Self-hosting

See [OSC-1-SELF-HOST-STATEMENT.md](OSC-1-SELF-HOST-STATEMENT.md). Short form:
the OSC-1 compiler cannot compile any part of itself, and this slice makes no
self-hosting claim.

## 10. Implementation notes and evidence

- Files: `src/compiler/` (front end `osc_lex/parse/check/lower/front`, IR
  `osc_ir`, reference interpreter `osc_interp`, encoder/decoder `osc_a64`,
  codegen `osc_cg`, loader `osc_native`, bootstrap runtime `osc_rt`, CLI
  `oscc_main.c`), the II.11 model in `src/compiler/model/`, tests in
  `tests/compiler/` (OSC-0 III.8 names `tests/osc/`; this slice keeps its tests
  next to its sources under `tests/compiler/`).
- Encoder. `src/aarch64_encoder.c` (used by `omega_program_realize`, M5/M6/M9/M14)
  masks register fields silently (`rm & 0x1f`), which is exactly what OSC-0 III.6
  says must be hardened. Changing it would change the inputs of the existing
  gates, so it is left untouched and OSC-1 uses its own validating encoder
  `osc_a64.c` with a decoder mirror. The existing M6/M9/M14 gates were run
  before and after this work and are unchanged.
- `make test-compiler` (host, single core, about 70 s): OSC-0B model sweep
  (10^6 sequences), back end differential test, compiler golden / negative /
  model-agreement test, cross-process determinism; the three test binaries also
  run under ASan/UBSan. `make compiler-receipt` writes the content-addressed
  receipt `evidence/OSC-1/receipts/osc1-compiler-<sha256>.json`;
  `make osc0b-model-receipt` writes the OSC-0B exit receipt
  `evidence/OSC-1/receipts/osc0b-model-<sha256>.json`.
- The reference interpreter is a second implementation of section 5 (signed
  arithmetic computed exactly in 128 bits and range-checked), not a copy of the
  codegen's flag logic. It is a differential oracle; agreement between two
  implementations is evidence, not proof.
