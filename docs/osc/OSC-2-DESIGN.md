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
