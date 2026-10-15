# Omegese: a program language built for the AI writer

Status: v1 (nested values, exact next oracle, 2026-10-15). Phase 1 done (language, translator, checks, token measurement);
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

## 6. The next oracle and the checks (`make test-omegese`)

### 6.1 What `omegese next` promises

`omegese next <prefix>` reads a prefix of a program (any text, cut anywhere, even inside a word) and
prints what may come next. It is **exact**: an item is printed if and only if some text that starts
with it turns the prefix into a program `omegese emit` accepts. Nothing that can still lead to an
accepted program is missing (within the limits in 6.4), and nothing printed leads to a dead end. When no text can, it prints one
`REFUSED omegese: ...` line and exits 1: a defect in its complete words gets the reason `emit` would
give; otherwise (also when the word being written cannot be finished) the line says `no text after this prefix makes a program emit accepts` with the reason the
nearest attempt failed.

How it decides. The prefix is split into its complete lines, the complete words of the last line, and
the word still being written (empty after a space, a bracket or at a line start). For each candidate
item a witness is built: the line is finished with the words it still needs (a `ret` line also reads
the names the ending path would otherwise leave unread), every open block is closed (`x = x` for each
unread name, a `ret` of a zero where a value is owed, `end`), and the real parser that `emit` runs
re-reads that line and the closing lines from the state after the complete lines, with every
end-of-input check. An item is printed only when its witness is accepted, so every printed item is
legal by construction. Completeness rests on the witness finder; the test fuzz below checks it.

Names stay open at the character level. When a line binds a new name, any identifier is legal except
a reserved word (`omegese words` and the IR type names `u8` .. `i64`) or one equal to a visible
name; that is printed as one class item. When a line reads a name, the names printed are the
visible names (and earlier functions) of the right type and kind.
Numbers come as intervals: the parser only compares a number with fixed bounds (type ranges, array
lengths 1..64, trap codes 1..14, loop bounds and counts up to 1000000) and with the lengths of the
arrays in scope, so one test per breakpoint and per gap between breakpoints decides every number, and
neighbouring legal pieces merge. Numbers are written as decimal digits with an optional minus sign and
no leading zeros; `-0` is refused by `emit` as well (it is not a number).

### 6.2 `omegese next --machine`: the fixed grammar

One item per line, in this order, nothing else on stdout. Exit 0 with at least one item, or exit 1
with exactly one line starting `REFUSED omegese: `. Let U be the word still being written.

| line | meaning |
|---|---|
| `word X` | finish the current word as X (X starts with U): a fixed word, a visible name, a function, or a name written earlier on the same line (a new parameter) |
| `newname E1 E2 ...` | finish the current word as any new name: a letter or `_`, then letters, digits or `_`, at most 63 characters, starting with U, not a reserved word, and not one of E1 E2 ... (the visible names, functions and names earlier on the line that start with U; the list may be empty) |
| `number LO HI` | finish the current word as a number v with LO <= v <= HI whose decimal text starts with U |
| `(` | an opening bracket (it also ends the current word) |
| `)` | a closing bracket (it also ends the current word) |
| `space` | a space or tab: ends the current word, or adds blank space between words |
| `newline` | ends the current word and the line |
| `eof` | the input may end here: the prefix is already an accepted program |

A writer (an engine's mask) may append text s to the current word exactly when U followed by s is
still the start of some `word`, `newname` or `number` item; the soundness fuzz checks that a word
stays legal after each of its first characters.

Example, after `fn f a uint to uint` and a newline: `word if`, `word while`, `word for`, `word ret`,
`word trap`, `word a`, `newname a f`, `space`, `newline`.

### 6.3 Human output and `omegese complete`

`omegese next` without `--machine` prints the same set for a person, one labelled line per kind,
empty lines left out: `assign:` (at a line start, the names that may be assigned) or `names:`,
`number: LO..HI, ...`, `new-name: any name not in use`, and `words:` (fixed words, functions, `(`,
`)`, `(space)` inside a word, `(newline)`, `(end of input)`).

`omegese complete <prefix>` prints one text that, appended to the prefix, gives a program `emit`
accepts (finish the word and the line, then close everything); nothing when the prefix is already a
program; exit 1 (`REFUSED`) when there is none.

### 6.4 Limits (where exact can miss)

There is no over-approximation: every printed item was accepted by the real parser. An item can be
missed only when the witness finder fails although a completion exists, which needs one of these
limits: a line longer than 96 words (a witness can be longer than the shortest completion), more than
480 registers, 256 distinct constants or the block and frame limits in one program, more than 64
tries when several names of the right kind could be read, or a new name within a few characters of
the 63-character limit (the class is decided by one representative name). None of these is reached
by any program in the suite or the fuzz.

**Ownership (checker2's C2-OWN rules), folded into `emit` on 2026-10-15.** checker2 follows every own
array along every path (it holds an array, holds nothing yet, or was freed or moved) and rejects a use,
a move or a free where it holds nothing on every path (C2-OWN-USE-DEAD, C2-OWN-DOUBLE-RELEASE), and a
new array or a return where it still holds one on every path (C2-OWN-LEAK). `emit` keeps the same
states along the lines it reads and refuses such a line where it is written, so `next` leaves out the
word that leads there. Inside an open loop a state that includes the loop head is not known until the
loop's `end` line (a later line of the body can change it); such a check waits and is decided at
`end`. `complete` (and every witness) settles what it can: before a loop's `end` it gives a name the
loop head needs an array (`x = new ...`) or frees one that would still hold its array there, and at the
function's end it frees each array that still holds one on every path (or ends the path with `trap 1`
when that array's name is out of scope).

Measured with the residual walk in 6.5 (1000 walks from each of two starts, seed 4453): completed
walks that `emit` accepts and omega_emit or checker2 then rejects.

| build | rejected later | by rule |
|---|---|---|
| before (2026-10-15, `f6b44356`) | 154 of 2000 (7.7%) | 136 C2-OWN-LEAK, 15 C2-OWN-USE-DEAD, 3 C2-OWN-DOUBLE-RELEASE |
| ownership states only, no loop checks | 2 of 2000 (0.1%) | 2 C2-OWN-USE-DEAD (a use in a loop head of an array freed before it) |
| after | 0 of 2000 | none |

Not folded, so a program `emit` accepts can still be rejected later:
- C2-BORROW-OUTLIVES-OWNER: checker2 ties each view to the array it was taken from over the whole
  function (a view filled from two arrays loses its origin); `emit` does not follow borrows (0 of
  2000 walks met it);
- more than 64 own arrays in one function: the ones past 64 are not tracked;
- C2-OWN-ARENA-MOVE and C2-OWN-RELEASE-ARENA: Omegese has no arenas, so no program reaches them.

The other way, `emit` refuses a function with more than 256 distinct ownership checks waiting for loops to end,
which checker2 might accept. And where exact can miss: a waiting check whose name is out of scope (or
hidden by a newer name) at the loop's `end` gets no settling line, so the word that led there is left
out; a `ret` whose value moves the array into a call is not tried by the witness.

### 6.5 The checks

- the 10 dev programs: omegese -> omega_emit -> checker2 ACCEPT;
- nested values: 6 dev programs rewritten with nested values (`tests/one_layer/omegese/nested`) and 5
  trial 2 answers kept verbatim (`trial2`), all ACCEPT; the trial 2 isqrt answer (dev07) is still refused
  with `line 3: le has 3 values after it; it takes 2` (it has one value too many);
- 8 coverage programs (IR type names, break, nested loops, else-if, a for with a name or a value as count,
  arrays and calls, calls without values, a bool function as a condition, nested values);
- 46 refusals, each with its expected reason, each with a fixed control that is accepted (arity, brackets,
  two numbers in one op, extra words, `new` inside a value, lines after trap, and 12 ownership cases: a
  leak at a return or at a second `new`, a second free, a use after a move, an out-of-scope array at a
  return, and five decided at a loop's `end`, one of them through a nested loop);
- review fixes: a conversion of a typed number converts it (`ret long int 5`, `uchar int 300`); a for
  count in brackets lowers exactly as the bare count and has the same limit;
- `omegese next` on 69 prefixes and cases, red before green (the open-type list after a binary op, a typed second
  value, a bracket waiting for `)`, a `ret` that must read the names its path leaves unread, a cut
  inside a word, the exact number range of every scalar type and of trap codes, array lengths, loop
  bounds and counts, a minus sign where none fits, `-0`, the `--machine` lines including the 6.2
  example and the new-name exclusion list, `complete`, refusals as soon as a prefix is wrong, and
  ownership: no `ret` or second free while it would be refused, `end` left out of a loop whose head
  needs an array, `complete` settling a loop and freeing at the function's end, a loop opened at the
  nesting limit refused, 300 uses of one array in a loop accepted);
- next oracle: at all 2636 word boundaries of the 75 accepted programs in the suite, the real next
  word, and the end of input at the end, are legal;
- completeness: 150 single-word mutants of those programs (a word dropped, doubled, or replaced by a
  number, a name, a fresh name or a fixed word): `emit` accepts a mutant if and only if every word of
  it was legal when it was written (2906 word boundaries);
- soundness fuzz: 40 random walks of up to 25 legal items from random cut points (a number item is
  tried at its ends and at a random value inside); the legal set is
  never empty, a word picked stays legal after each of its first characters, and `omegese complete`
  then finishes the walk into a program `emit` accepts within a bounded number of lines;
- checker2 residual: 40 walks from each of two starts (an empty file and a cut point in a suite
  program), each closed with `complete` and sent through omega_emit and checker2; at most
  `OMEGESE_RESIDUAL_MAX` (default 0) may be rejected there (`OMEGESE_RESIDUAL_WALKS` sets the count,
  `OMEGESE_RESIDUAL_KEEP` a folder that keeps the rejected programs; the table in 6.4 used 1000);
- truncation fuzz: all 2799 byte prefixes of the nested, trial and coverage programs through `emit`,
  `next` and `next --machine`, which must answer (accept or refuse with a reason) and never crash; at
  the 2698 prefixes of accepted programs, `next` must also allow the byte that follows (the whole
  word it belongs to) and `complete` must finish the prefix into a program `emit` accepts;
- loop guard negative control: the dev04 op list without the hidden counter increment passes
  omega_emit and is REJECTED by checker2 (`C2-LOOP-UNBOUNDED`);
- tokenizer gate, when `OMEGESE_TOKCOUNT` (a counter: `<tool> <tokenizer.json> words <word>...`
  printing the token count of " word" first) and `OMEGESE_TOKENIZER` are set.

The same suite then runs again on a build with AddressSanitizer and UndefinedBehaviorSanitizer
(memory and undefined-behaviour checks), with 10 walks and one mutant per program; every sanitizer
finding fails the run. Its negative control: two canary builds (`-DOMEGESE_CANARY=1`, a read after
free; `=2`, a signed overflow) must each be caught first. Clean. A stress run with 25 mutants per
program (1575 mutants) and 500 walks of up to 40 items (17386 items) found no failure.

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
3. A pre-registered held-out comparison: exp445 revision r3 (`docs/one_layer/EXP445_PROTOCOL.md`
   section 13), Omegese against OSC text. The `omegese` arm's system prompt is
   `tools/exp445/arms/common.md`, then `tools/exp445/arms/omegese.md` (the language and a word card:
   every fixed word on one line with what it does), then the worked examples
   (`tools/exp445/arms/examples/`, built by `reader.sh system_prompt`): 14 dev-style tasks in the
   many-shot mode, the r2 single example (`clamp_sub`) in the one-shot mode (Stage 0 only). The `text`
   arm gets the same tasks with the program in OSC. `make test-exp445` checks every example in both
   forms (reader, back end, the sealed harness on its own inputs), that the word card is exactly
   `omegese words`, the one-token gate on every word of the examples, and that no example shares an
   entry name with a held-out task.
4. Where the r3 Stage 1 answers failed, by reader stage (config A, attempt 1, 60 held-out tasks per
   arm; aggregate counts and refusal categories from the score output only, no task text read):

   | arm | round | passed the reader | refused | where refused, by category |
   |---|---|---|---|---|
   | Omegese | first answer | 35 | 25 | all at `omegese emit`: type 13, structure 6, arity 4, scope 1, range 1 |
   | Omegese | repair round | 4 | 21 | all at `omegese emit`: type 11, structure 6, arity 3, scope 1 |
   | text | first answer | 44 | 16 | the OSC compiler |
   | text | repair round | 5 | 11 | the OSC compiler |

   No Omegese answer was refused at omega_emit, at the signature check or by the back end. Final
   verdicts: Omegese 23 ACCEPT, 21 no candidate, 16 wrong behaviour (7 unexpected trap, 8 wrong result,
   1 wrong trap); text 34 ACCEPT, 11 compile refused, 15 wrong behaviour (6 missing trap, 8 unexpected
   trap, 1 wrong result).
   What a legal-word mask (`omegese next` in the engine) prevents: every one of those Omegese refusals,
   since each was an `emit` refusal and the mask only lets through words that some accepted program
   continues. With the ownership rules folded in, what the mask lets through also passes checker2's
   ownership rules, within the limits in 6.4.
   What it does not prevent: an answer whose function has the wrong name, parameters or return type is
   still an accepted program, so only a forced signature header (the task's `fn` line written for the
   model) prevents a signature mismatch; none was seen, but every Omegese refusal stopped at `emit`
   before the signature check, so mismatches could be hidden behind them. Neither prevents the 16
   wrong-behaviour answers. The 11 text refusals would need a mask for OSC text.

## 8. Prior art (quick search, not exhaustive)

Grammar-constrained decoding (Outlines, XGrammar, llguidance, SynCode), grammar-aligned decoding
(NeurIPS 2024), type-constrained decoding for TypeScript (Mündler et al., PLDI 2025), and a grammar
redesigned for fewer tokens (SimPy, 2024). Not found: a lexicon chosen so one terminal is one vocab
token, with type-aware prefix checking feeding an independently checked, signed IR. "New" is
UNVERIFIED.
