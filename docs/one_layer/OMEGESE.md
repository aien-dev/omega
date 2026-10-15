# Omegese: a program language built for the AI writer

Status: v1 (nested values, 2026-10-15). Phase 1 done (language, translator, checks, token measurement);
phase 2 started (section 7).
Tool: `tools/omegese/omegese.c`; tests: `make test-omegese`. Named Glyph until 2026-10-15 (the name was
taken); evidence recorded before then says glyph. Home: omegese.com (owned by Drake since 2026-10-15).

## 1. Why

exp445 (`docs/one_layer/EXP445_PROTOCOL.md`) asks AIs to write Omega programs as OSC text, as a
JSON op list (`tools/omega_emit/SCHEMA.md`) and as hex bytes of the binary IR. The cost and the
errors come from the same places: punctuation and keys (JSON), two characters per byte (hex), and
bookkeeping the writer must do in its head: numbered registers with declared types, numbered blocks,
byte offsets. Text avoids the bookkeeping but needs a compiler for a human-readable language.

Omegese keeps the machine-first path (no human source language in production) and removes the
bookkeeping and the punctuation. Its words are chosen from the writer model's own vocabulary.

## 2. Design rules

1. **One word, one token.** Every fixed word (ops, types, keywords) is exactly one token of the
   target tokenizer when written with its leading space (Qwen3; the 8B, 30B and 4B tokenizers share
   one vocabulary). `make test-omegese` checks this when given a tokenizer (section 6). There is no
   punctuation besides `=` (also one token, and optional) and the optional brackets `( )`; no
   commas, no indentation. Values nest in prefix order (every op takes a fixed number of values), so
   brackets are never needed. Numbers cost one token for the space plus one per digit (Qwen3 splits
   digits).
2. **Types are inferred.** Only parameters, the return type, conversions, typed numbers
   (`uint 5`) and new arrays name a type. A value's type comes from the op that makes it, bottom-up
   through nested values; a number takes the type of the other value in its op. This removes the
   "register declared one type, written with another" error class.
3. **Names, not numbers.** Values are named; registers, blocks and branch targets are numbered by
   the translator. Control is structured (`if`/`else`/`end`, `while`, `for`).
4. **Scope gives definite assignment.** A name first written inside an `if`, `else`, `while` or
   `for` dies at its `end`; to carry a value out, write it before the block. So every read is
   written on every path by construction.
   Every name written must be read before its scope ends (parameters and loop counters excepted), so a
   misspelled word (the short form turns `rte a` into a new name) or a forgotten value is refused.
   No name hides a visible one (loop names included). Parameter names are distinct. Calls go to earlier functions only, with each argument checked
   against the parameter (arrays: own from own, edit from own or edit, view from any).
5. **Loops are proven to end.** Every loop lowers to the shape checker2 proves terminating
   (`src/checker2/c2_check.c`, "loop bounds"; the shape oscc emits for `while ... bound N`).
6. **Prefix-decidable.** After any prefix the checker knows which words may come next
   (`omegese next`): the mask an inference engine can apply while the model writes, so an invalid
   program cannot be written.
7. **Same trust chain.** Omegese -> omega_emit op list -> canonical OSC binary IR -> checker2 ->
   signed receipt. Omegese changes nothing after the op list.

## 3. The language

A program is one or more functions. A line holds one statement; words are separated by spaces.

```
fn <name> (<param> <type>)* to <type> | void      a function; `end` closes it
<name> = <value>          write a value (a new name, or an existing one of the same type);
<name> <value>            the same, with `=` left out
if <cond> / else / end
while <cond> [bound N] / end      N 1..1000000, default 1000000; past N iterations: trap 4
for <name> <type> <count> / end   name = 0 .. count-1 (count a number, or a value of that type)
                          (any other count: trap 4 after 1000000 passes, as a while with no bound)
break                     leave the innermost loop
ret [<value>]
put <array> <index> <value>      store
free <array>                     release an own array
trap <code 1..14>         stop with that code; like ret, it ends the path (lines after it in the
                          same block can never run and are refused)
<function> <args>         call a function that returns nothing
```

Values are prefix expressions. Every op takes a fixed number of values, and each of those values may
itself be an op, so no brackets are needed: `add mul r r 4` is add(mul(r, r), 4), and
`eq rem n 2 0` is eq(rem(n, 2), 0).

| value | takes | meaning | IR |
|---|---|---|---|
| `<name>` / number | 0 | copy | mov / const |
| `add sub mul div rem and or xor shl shr` a b | 2 | arithmetic, bitwise, shifts (shift amount any whole-number type) | bin |
| `eq ne lt le gt ge` a b | 2 | comparison, gives bool | cmp |
| `neg` a / `not` a | 1 | negate (signed) / logical not (bool) or bitwise not (number) | un |
| `<type>` a | 1 | a typed number (`uint 5`) or a conversion (`ulong x`, `ulong add a b`) | const / cast |
| `get` arr i | 2 | load (arr is an array name) | load |
| `new` type len init | 3 | a new own array of len (1..64) elements; only as the whole value of `<name> =` | alloc |
| `own` / `edit` / `view` arr | 1 | move / mutable borrow / shared borrow (arr is an array name) | mov |
| `<function>` args | its parameter count | call an earlier function | call |

Brackets are optional grouping: `( <value> )` holds exactly one complete value and changes nothing
else (`sub diff (i64 b)` = `sub diff i64 b`). A `(` without its `)`, a `)` without its `(`, an empty
`( )`, or a `( )` holding more than one value is refused. `(` and `)` are separate words even when
written against a name.

Typing is bottom-up. A value's type comes from its op; a bare number takes the type of the other
value of its op (`add x 1`, `sub 15 i`, `shl 1 sh`), or the type the place needs when it stands
alone (`ret 0`, `x = 5` for an existing x, a call argument, an index is uint). Two bare numbers in one
op are refused (`add 1 2`, also when nested: `add a mul 2 3`). Comparisons give bool. In a shift a
bare number on the left takes the type of the shift amount (`shl 1 sh` with sh uchar shifts a uchar);
write its type when you mean another (`shl ulong 1 sh`). A typed number inside a conversion is
converted (`long int 5` is the int 5 converted to long; `uchar int 300` traps 5).

A condition is any value that gives bool (`while ne b 0`, `if or lt p -32768 gt p 32767`). Values may
be nested wherever a value goes: after `=`, `ret`, `if`, `while`, `put` (index and value), call
arguments, a `for` count, inside conversions. `and`, `or` evaluate both sides (no short cut).

A wrong number of values is refused with its place: `line 3: le has 3 values after it; it takes 2`,
`line 2: sub has 1 value after it; it takes 2`, `extra words at the end of the line: 'b'`.

Types: `bool uchar ushort uint ulong byte short int long` = bool u8 u16 u32 u64 i8 i16 i32 i64
(the IR names `u8` .. `i64` are accepted too, as the task signature writes them; they cost 2 or 3 tokens)
(`u32` is three Qwen3 tokens; `uint` is one). Arrays: `own|edit|view <type> <len>`.

Example (dev04):

```
fn gcd a uint b uint to uint
while ne b 0 bound 100
t = rem a b
a = b
b = t
end
ret a
end
```

## 4. Lowering

Numbers are hoisted into the entry block, one CONST per distinct value and type (so the loop bound
is defined exactly once, as checker2 requires). Blocks are numbered in the order they start.

- `while c bound N`: hidden ulong counter = 0; head: c -> guard | exit; guard: `ge counter N` ->
  trap block (trap 4, br exit) | body; the body starts with `counter = add counter 1`.
- `for i T K` (K a number): i = 0; head: `lt i K` -> body | exit; latch: `i = add i 1`.
- `for i T n` (n a name or any value that is not a number): as `while lt i n`, with the hidden guard,
  plus `i = add i 1` at the latch.
- `if c` -> cbr then | else-or-join; a join block exists only if some branch reaches it.
- Nested values: each op inside a value writes a fresh hidden register (a local with the op's type,
  never a name, so the unread-name rule does not apply), in evaluation order: innermost first, left
  to right (`x = add mul a b c` -> `t = mul a b; x = add t c`). Only the outermost op writes the
  target name, after every operand has been read. Call arguments are evaluated left to right before
  the call; a `put` evaluates its index, then its value, then stores.
- A typed number used as a value (`uint 5`) is the hoisted CONST itself; a bare name or number used
  as an operand is read in place (no copy).
- `while`: the condition's ops are emitted in the head block, so the whole nested condition is
  computed again before every pass.
- `for i T <value>` (a count that is not a number): a name is read at every pass (as v0); any other
  value is computed once, before the loop.
- `trap N`: `trap N`, then a `ret` that is never reached (with a zero of the return type), because
  the IR block still needs a terminator.

## 5. Measured (phase 1)

The 10 public dev tasks (`tests/one_layer/exp445/dev`), reference solutions in each spelling,
tokens counted offline with the Qwen3 tokenizer (the measure.sh method: text = reference.osc;
bytes = hex of the compiled IR; structured = `omega_emit lift` of that IR; Omegese =
`tests/one_layer/omegese/dev/*.omegese`; Omegese short = the same with `=` left out).

| task | text | bytes | structured | Omegese | Omegese short |
|---|---|---|---|---|---|
| dev01 | 64 | 281 | 309 | 48 | 45 |
| dev02 | 60 | 256 | 286 | 43 | 40 |
| dev03 | 81 | 567 | 624 | 57 | 53 |
| dev04 | 82 | 441 | 507 | 41 | 38 |
| dev05 | 27 | 118 | 91 | 17 | 17 |
| dev06 | 27 | 112 | 91 | 17 | 17 |
| dev07 | 120 | 594 | 750 | 72 | 65 |
| dev08 | 103 | 807 | 926 | 84 | 77 |
| dev09 | 75 | 488 | 547 | 55 | 50 |
| dev10 | 22 | 98 | 79 | 15 | 15 |
| **total** | **661** | **3762** | **4210** | **449** | **417** |

Omegese is 9.4x fewer tokens than the structured op list and 32% fewer than OSC text, for the same
IR shape (all 10: harness `ola.sh experiment` set omegese-1 accepted 10/10, RUN VALID, run
`20261015T094000Z-e27a33ee052d-562014`; checker2 ACCEPT on all 10).

Limits: the Omegese programs were written by the designer, not by a model, and in places use the
shortest form (writing to parameters instead of copies), so the text column is not an equal-effort
rewrite. Whether models write Omegese reliably is not measured yet.

## 6. Checks (`make test-omegese`)

- the 10 dev programs: omegese -> omega_emit -> checker2 ACCEPT;
- nested values: 6 dev programs rewritten with nested values (`tests/one_layer/omegese/nested`) and 5
  trial 2 answers kept verbatim (`trial2`), all ACCEPT; the trial 2 isqrt answer (dev07) is still refused
  with `line 3: le has 3 values after it; it takes 2` (it has one value too many);
- 8 coverage programs (IR type names, break, nested loops, else-if, a for with a name or a value as count,
  arrays and calls, calls without values, a bool function as a condition, nested values);
- 34 refusals, each with its expected reason, each with a fixed control that is accepted (arity, brackets,
  two numbers in one op, extra words, `new` inside a value, lines after trap);
- review fixes: a conversion of a typed number converts it (`ret long int 5`, `uchar int 300`); a for
  count in brackets lowers exactly as the bare count and has the same limit;
- `omegese next` on 26 prefixes (the open-type list after a binary op, a typed second value, a bracket
  waiting for `)`, `end` withheld while a value function can still reach its end, refusals as soon as
  a prefix is wrong);
- next oracle: at every word boundary of every accepted program in the suite, the real next word is
  in the list `omegese next` gives;
- truncation fuzz: every byte prefix of the nested, trial and coverage programs through `emit` and `next`, which
  must answer (accept or refuse with a reason) and never crash;
- loop guard negative control: the dev04 op list without the hidden counter increment passes
  omega_emit and is REJECTED by checker2 (`C2-LOOP-UNBOUNDED`);
- tokenizer gate, when `OMEGESE_TOKCOUNT` (a counter: `<tool> <tokenizer.json> words <word>...`
  printing the token count of " word" first) and `OMEGESE_TOKENIZER` are set.

The same suite also runs against a build with AddressSanitizer and UndefinedBehaviorSanitizer
(memory and undefined-behaviour checks): clean.

## 7. Phase 2

1. Prompt-only trial on the dev tasks, no mask (done once, 2026-10-15): the 30B on our own engine, one
   attempt, harness run `20261015T105049Z-e27a33ee052d-1500714` (RUN VALID): Omegese 3/10, text 3/10,
   structured 0/10; completion tokens 1015 / 1346 / 12027. 8 of 10 first answers were refused only for
   writing the task signature's type names (`u8`, `u32`); those are now accepted.
   Trial 2 (same model, one attempt plus one repair round): Omegese 2/10, text 3/10. 6 of the 8 Omegese
   failures wrote nested values (`n = add (mul (uint 3) n) 1`), which v0 refused. Values now nest (section 3): 5 of
   those 6 answers, kept verbatim in `tests/one_layer/omegese/trial2`, now pass checker2; the sixth
   (dev07) has one value too many and is still refused, with its place. Rerun owed.
2. The mask inside our own engine: `omegese next` as a table on the GPU, one lookup per token;
   positions with one legal word need no model step.
3. A pre-registered held-out comparison under a new protocol revision.

## 8. Prior art (quick search, not exhaustive)

Grammar-constrained decoding (Outlines, XGrammar, llguidance, SynCode), grammar-aligned decoding
(NeurIPS 2024), type-constrained decoding for TypeScript (Mündler et al., PLDI 2025), and a grammar
redesigned for fewer tokens (SimPy, 2024). Not found: a lexicon chosen so one terminal is one vocab
token, with type-aware prefix checking feeding an independently checked, signed IR. "New" is
UNVERIFIED.
