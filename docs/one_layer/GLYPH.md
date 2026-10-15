# Glyph: a program language built for the AI writer (v0)

Status: v0, phase 1 (language, translator, checks, token measurement). No model has written Glyph
yet; that is phase 2 (below). Tool: `tools/glyph/glyph.c`; tests: `make test-glyph`.

## 1. Why

exp445 (`docs/one_layer/EXP445_PROTOCOL.md`) asks AIs to write Omega programs as OSC text, as a
JSON op list (`tools/omega_emit/SCHEMA.md`) and as hex bytes of the binary IR. The cost and the
errors come from the same places: punctuation and keys (JSON), two characters per byte (hex), and
bookkeeping the writer must do in its head: numbered registers with declared types, numbered blocks,
byte offsets. Text avoids the bookkeeping but needs a compiler for a human-readable language.

Glyph keeps the machine-first path (no human source language in production) and removes the
bookkeeping and the punctuation. Its words are chosen from the writer model's own vocabulary.

## 2. Design rules

1. **One word, one token.** Every fixed word (ops, types, keywords) is exactly one token of the
   target tokenizer when written with its leading space (Qwen3; the 8B, 30B and 4B tokenizers share
   one vocabulary). `make test-glyph` checks this when given a tokenizer (section 6). There is no
   punctuation besides `=` (also one token, and optional), no brackets, no commas, no indentation.
   Numbers cost one token for the space plus one per digit (Qwen3 splits digits).
2. **Types are inferred.** Only parameters, the return type, conversions, typed numbers
   (`uint 5`) and new arrays name a type. A value's type comes from the op that makes it; a number
   takes the type of the other value in its op. This removes the "register declared one type,
   written with another" error class.
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
   (`glyph next`): the mask an inference engine can apply while the model writes, so an invalid
   program cannot be written.
7. **Same trust chain.** Glyph -> omega_emit op list -> canonical OSC binary IR -> checker2 ->
   signed receipt. Glyph changes nothing after the op list.

## 3. The language

A program is one or more functions. A line holds one statement; words are separated by spaces.

```
fn <name> (<param> <type>)* to <type> | void      a function; `end` closes it
<name> = <value>          write a value (a new name, or an existing one of the same type);
<name> <value>            the same, with `=` left out
if <cond> / else / end
while <cond> [bound N] / end      N 1..1000000, default 1000000; past N iterations: trap 4
for <name> <type> <count> / end   name = 0 .. count-1 (count a number or a name of that type)
                          (a name as count: trap 4 after 1000000 passes, as a while with no bound)
break                     leave the innermost loop
ret [<value>]
put <array> <index> <value>      store
free <array>                     release an own array
trap <code 1..14>
                          (a trap stops the program; lines after it are allowed, as in the IR)
<function> <args>         call a function that returns nothing
```

Values (one op each; operands are names or numbers):

| value | meaning | IR |
|---|---|---|
| `<name>` / number | copy | mov / const |
| `add sub mul div rem and or xor shl shr` a b | arithmetic, bitwise, shifts (shift amount any whole-number type) | bin |
| `eq ne lt le gt ge` a b | comparison, gives bool | cmp |
| `neg` a / `not` a | negate (signed) / logical not (bool) or bitwise not (number) | un |
| `<type>` number / `<type>` name | a typed number / a conversion | const / cast |
| `get` arr i | load | load |
| `new` type len init | a new own array of len (1..64) elements | alloc |
| `own` / `edit` / `view` arr | move / mutable borrow / shared borrow | mov |
| `<function>` args | call an earlier function | call |

A condition is a bool name, `true`/`false`, or one op giving bool (`while ne b 0`).

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
- `for i T n` (n a name): as `while lt i n`, with the hidden guard, plus `i = add i 1` at the latch.
- `if c` -> cbr then | else-or-join; a join block exists only if some branch reaches it.

## 5. Measured (phase 1)

The 10 public dev tasks (`tests/one_layer/exp445/dev`), reference solutions in each spelling,
tokens counted offline with the Qwen3 tokenizer (the measure.sh method: text = reference.osc;
bytes = hex of the compiled IR; structured = `omega_emit lift` of that IR; Glyph =
`tests/one_layer/glyph/dev/*.glyph`; Glyph short = the same with `=` left out).

| task | text | bytes | structured | Glyph | Glyph short |
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

Glyph is 9.4x fewer tokens than the structured op list and 32% fewer than OSC text, for the same
IR shape (all 10: harness `ola.sh experiment` set glyph-1 accepted 10/10, RUN VALID, run
`20261015T094000Z-e27a33ee052d-562014`; checker2 ACCEPT on all 10).

Limits: the Glyph programs were written by the designer, not by a model, and in places use the
shortest form (writing to parameters instead of copies), so the text column is not an equal-effort
rewrite. Whether models write Glyph reliably is not measured yet.

## 6. Checks (`make test-glyph`)

- the 10 dev programs: glyph -> omega_emit -> checker2 ACCEPT;
- 5 coverage programs (IR type names, break, nested loops, else-if, a for with a name as count, arrays and calls,
  calls without values, a bool function as a condition);
- 21 refusals, each with its expected reason, each with a fixed control that is accepted;
- `glyph next` on 6 prefixes (only `end` after ret; a binary op's second operand of the first's type;
  bool functions and `get` after `if`; a line after ret, and a line before `fn`, refused);
- loop guard negative control: the dev04 op list without the hidden counter increment passes
  omega_emit and is REJECTED by checker2 (`C2-LOOP-UNBOUNDED`);
- tokenizer gate, when `GLYPH_TOKCOUNT` (a counter: `<tool> <tokenizer.json> words <word>...`
  printing the token count of " word" first) and `GLYPH_TOKENIZER` are set.

## 7. Phase 2 (not done)

1. Prompt-only trial on the dev tasks, no mask, against the structured arm.
2. The mask inside our own engine: `glyph next` as a table on the GPU, one lookup per token;
   positions with one legal word need no model step.
3. A pre-registered held-out comparison under a new protocol revision.

## 8. Prior art (quick search, not exhaustive)

Grammar-constrained decoding (Outlines, XGrammar, llguidance, SynCode), grammar-aligned decoding
(NeurIPS 2024), type-constrained decoding for TypeScript (Mündler et al., PLDI 2025), and a grammar
redesigned for fewer tokens (SimPy, 2024). Not found: a lexicon chosen so one terminal is one vocab
token, with type-aware prefix checking feeding an independently checked, signed IR. "New" is
UNVERIFIED.
