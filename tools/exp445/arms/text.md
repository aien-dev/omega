REPRESENTATION: text

Write the program as OSC text, the language below (from docs/osc/OSC-1-DESIGN.md).

Grammar (EBNF):


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


Static rules:


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
