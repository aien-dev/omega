# Omega surface language V0

Status: V0, Omega Visor V1 lane 3. Code: `src/language/omega_lex.[ch]`, `omega_parse.[ch]`,
`omega_lower.[ch]`. Tests: `tests/language/test_language_v0.c` + `tests/language/golden/v0.txt`
(`make test-language`, run from the repo root).

## Purpose

A small typed line language for the Visor console. Source text is lexed, parsed into a
**disposable** syntax tree, and lowered into the **existing** canonical Omega objects through
the existing builders. The tree is never hashed, stored or used as identity. Lowering builds
objects only; it never evaluates them. Evaluation, realization and verification stay with the
existing graph/realizer/verifier code.

## Grammar (EBNF, one statement per line)

```
line      = [ stmt ] ;                                (* blank / comment-only line = no-op *)
stmt      = let_stmt | fn_def | expr_line ;
let_stmt  = "let" NAME ":" type "=" expr ;
expr_line = expr [ ":" type ] ;
fn_def    = "fn" NAME "(" NAME ":" type ")" "->" type
            [ "requires" clause ] [ "ensures" clause ] "{" expr "}" ;
clause    = { clause_token } ;                        (* up to "ensures" / "{" at paren depth 0 *)
expr      = or_expr [ cmp_op or_expr ] ;              (* comparisons parse, then fail closed *)
or_expr   = and_expr { "|" and_expr } ;
and_expr  = add_expr { "&" add_expr } ;
add_expr  = mul_expr { ("+" | "-") mul_expr } ;
mul_expr  = primary  { ("*" | "/") primary } ;
primary   = INT | "true" | "false" | NAME | "(" expr [ ":" type ] ")" ;
type      = "u8" | "u16" | "u32" | "u64" | "bool" ;
cmp_op    = "==" | "!=" | "<" | "<=" | ">" | ">=" ;
INT       = DEC | "0x" HEX { HEX } | "0b" BIN { BIN } ;   (* DEC may have leading zeros; not octal *)
NAME      = ( letter | "_" ) { letter | digit | "_" } ;    (* <= 63 chars; "_" = last result *)
comments  = "//" to end of line | "/*" ... "*/" ;
```

Precedence, loosest to tightest: comparisons, `|`, `&`, `+ -`, `* /`. All binary operators are
left-associative. Nesting depth is limited to 32 parenthesis levels, a line to 256 tokens and
1024 bytes, the tree to 128 nodes.

## Literal typing rule

V0 never guesses a width. An integer literal gets its type from exactly one of:

1. an explicit ascription: `7: u64` at the end of an expression line, or `(7: u64)` anywhere;
2. the annotation of the enclosing `let` (`let x: u64 = 7`);
3. a typed sibling operand in the same arithmetic expression (`x + 1` where `x` is `u64`;
   `(1: u64) + 2`), including through parentheses (`(1 + 2) + x`);
4. inside a `fn` body, the parameter type (always `u64` in V0).

A literal with none of these (`7`, `1 + 2`) is rejected: "ambiguous width". A literal that does
not fit its width (`256: u8`) is rejected, and a literal wider than 64 bits is rejected by the
lexer. `true`/`false` are always `bool`. There are no suffixes (`7u64` is a syntax error) and no
implicit conversions: every operand of an operator must have the identical type.

## Overflow policy

All V0 integer arithmetic lowers with `OVERFLOW_WRAP` (modulo 2^width), matching the policy the
existing realizer uses for u64 binary ops (`src/omega_realize.c`). The policy is part of the
OPERATION payload, so it is part of the SemanticId: an expression lowered with any other policy
would have a different identity.

## Lowering (how each construct maps to existing builders)

| Source | Builders | Result |
|---|---|---|
| `7: uN` | `omega_build_type_uint(N)`, `omega_build_val_uint(type, N, 7)` | VALUE id |
| `true` / `false` | `omega_build_type_bool`, `omega_build_val_bool` | VALUE id |
| `a op b` (`+ - * / & \|`) | `omega_build_op_binary(OP_ADD/SUB/MUL/DIV/AND/OR, OVERFLOW_WRAP, T)`, `omega_build_apply(op, a, b)` | APPLY id |
| `name` | none (id of the existing binding) | that id |
| `let x: T = e` | lowering of `e`, then `visor_binding_set(x, OBJECT, id)` | binding |
| `fn f(x: u64) -> u64 requires P ensures Q { chain }` | `omega_program_build_unary_op` (innermost step), `omega_program_compose` per further step, then name := `f`, pre/post text := P/Q, ids via `omega_build_constraint_id(CONST_PRECONDITION/POSTCONDITION, …)`, `omega_program_compute_id` | OmegaProgram, stored via `visor_program_add`, bound as PROGRAM |

Objects are **deduplicated by SemanticId**: after each builder call, if an identical object
already exists in the graph the new copy is dropped and the existing id is reused. On any error
the graph's `object_count` is restored to its value before the line, so no object is left
dangling. The type of an existing binding is read from the graph (VALUE → its type; APPLY → its
operation's `output_type`).

A `fn` body is accepted only in the shape the existing program model supports: a chain
`((x op c1) op c2) ...` where each op is one of `+ - * & |` and each `c` is a constant. For
`+ * & |` the constant may be written on either side (`2 * x + 1` is the same program as
`x * 2 + 1`); `-` needs the constant on the right. Contract clauses are canonicalized before
hashing: tokens re-joined with single spaces (none after `(` or before `)`), comments dropped,
numbers in decimal, the parameter renamed to `x`. An omitted clause is the text `true`.

## What fails closed, and why

Return codes: `-1` syntax, `-2` unsupported / unrepresentable, `-3` capacity. Every error is a
single line starting `column N:` and leaves the session unchanged (graph, bindings, programs).

| Construct | Code | Why |
|---|---|---|
| bare / all-literal integer (`7`, `1 + 2`) | -2 | ambiguous width |
| literal > width, literal > 64 bits | -2 | unrepresentable |
| mixed widths / bool vs int (`(1: u32) + (1: u64)`) | -2 | V0 never converts |
| arithmetic on bool | -2 | the existing evaluator is integer-only |
| signed types `i8..i64` | -2 | no signed value builder; evaluator and program model are unsigned-only |
| comparisons `== != < <= > >=` in expressions | -2 | the only builder (`omega_build_op_binary`) would type the result as the operand type, not `bool`; a mistyped result is worse than a missing feature |
| division by the constant `0` | -2 | undefined |
| unary minus, `!`, strings, chars, `[...]`, `{...}` in expressions | -2 | not in V0 |
| function calls `f(x)`, using a program name as a value | -2 | V0 has no calls |
| effect / authority / control-flow words (`effect cap capability mint revoke grant publish promote authority admin syscall io print import extern unsafe asm if else while for loop return match struct enum mut var const self rec`) | -2 | V0 cannot express effects or authority; these words are reserved so they never become names |
| unknown name | -2 | nothing to lower |
| `fn` not `(x: u64) -> u64`, more than one parameter | -2 | `omega_program_build_unary_op` is fixed at u64 → u64 |
| `fn` body not a constant-step chain (`x * x`, `x * (2 + 1)`, just `x`, constants only, other names) | -2 | the program model only has unary-op steps + composition; folding constants would be evaluation |
| `/` in a `fn` body | -2 | not in the program builder's op set |
| `fn` body constant > 0xFFFFFFFF | -2 | the builder loads immediates with MOVZ + one MOVK (32 bits); a wider constant would be silently truncated in the code while the contract text carried the full value |
| recursion (`f` in `f`'s body) | -2 | not supported |
| `result` in `requires`, other names in clauses, clause > 63 chars | -2 | the contract text field is 64 bytes; no silent truncation |
| a `fn` whose program id equals an existing program's but whose realization differs | -2 | internal builder-invariant violation (cannot happen from source since program identity v2; see identity laws) |
| nesting > 32, > 256 tokens, > 128 nodes, graph full (256 objects), binding table full (64), program table full (16), > 32 fn steps | -3 | capacity |

## Identity laws (locked by tests + golden vectors)

1. Literal spelling never matters: `7: u64`, `07: u64`, `0x07: u64`, `0b111: u64`, `(7: u64)`
   are one SemanticId, equal to `omega_build_val_uint(u64, 7)`.
2. Whitespace and comments never matter: `x + y`, `x+y`, `x + /* c */ y` are one APPLY id,
   equal to `omega_build_apply(omega_build_op_binary(OP_ADD, OVERFLOW_WRAP, u64), x, y)`.
3. Binding names never enter an object id: `let a: u64 = 7` and `let b: u64 = 7` bind the same
   VALUE id, and the second adds no object.
4. Width is identity: `7: u32` and `7: u64` differ.
5. Determinism: the same line lowered into two fresh graphs gives the same id.
6. Programs (program identity v2, `spec/program-identity.md`): the program id binds the canonical
   semantic body and the contract. The parameter name, the **fn name**, contract spacing/comments,
   literal spelling, and the side of a commutative constant do not change it; a different body
   (`x + 1` vs `x + 2`) or a different contract always does. Redefining a name with a different
   body adds a new program and rebinds the name; an identical program (under any name) is reused
   (dedupe). Cost and realization code are not part of the id.

Golden vectors (`tests/language/golden/v0.txt`): `kind<TAB>source<TAB>encoding_hex<TAB>id_hex`.
`E` rows are the canonical encoding (`omega_canonical_encode`) of the root object in a fresh
graph; `P` rows are the program's realization code bytes and `program_id`. Regenerate only on a
deliberate semantic change: `./build/tests-language/test_language_v0 --regen`.

The payload bytes come from memcpy of host C structs (existing bootstrap representation), so the
golden bytes are pinned to this ABI (aarch64/x86-64 LP64, 4-byte enums).

## Non-claims

- No evaluator: lowering never computes a value. `x * 2 + 1` is an object, not `7`.
- `fn` lowering does not call `omega_program_verify`. `omega_program_build_unary_op`/`compose`
  fill `realization` through `omega_program_realize` (spec/program-realization.md) and set
  `is_realized = true`; that is the realization's claim, not this lane's. `is_verified` stays false.
- u8/u16/u32 lower to correct canonical objects; whether they realize/execute correctly is the
  realizer's claim, not this lane's (V0 realization handles u8..u64 program bodies; Language V0
  `fn` is u64 only).
- Contract clauses are canonical **text**; nothing checks that the body satisfies them.
- Comparisons, signed integers, bool operators, calls, recursion, containers, strings, effects
  and authority are not in V0. No capability is ever minted, published or promoted.
- Identifiers are `[A-Za-z_][A-Za-z0-9_]*`; the Visor session also accepts `.` in names, the
  language does not.
