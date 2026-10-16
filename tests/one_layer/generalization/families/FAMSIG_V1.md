# GEN family signature and split contract (v1, frozen 2026-10-15)

Issue: aien-spark/omega#476 (GEN-1), program aien-spark/omega#475. This contract is frozen BEFORE the
custodian authors any held-out task. Its only implementation is `tools/gen/protocol/famsig_lib.h`
(tools `famsig` and `gencheck`). A change to the signature or token definition needs a new version (`FAMSIG_V2.md`, signature prefix
`gs2:`). Additive amendments made before any held-out task exists (the token, the coarse form, rules R6 and R7, `famsig compare`) are dated in place. After the custodian seals, nothing changes. `docs/one_layer/GEN_PROTOCOL_V1.md` pins this file by sha256.

## 1. What a family is

A **family** is a composition template: which primitives a task's SPECIFICATION combines, and how they
nest. It is declared by the task author from the task's prompt (what the task asks for), never computed
from a reference solution. Identifiers, constants, types, edge cases and trap rules do not enter the
signature: renaming a function or changing a constant never makes a new family.

## 2. Primitives and the nesting shape

Six primitives. Declare each from the prompt:

| primitive | kind | declare it when the specification asks for |
|---|---|---|
| `arith` | leaf | one arithmetic, bitwise, shift, comparison or conversion step stated as a single operation ("add x to the total", "is n even") |
| `nested` | leaf | one formula combining at least three operands with at least two operations, computed as a single value ("3*x*x + 2*x + 1"); declared instead of `arith` for such a formula |
| `array` | leaf | a fixed-length sequence (length 1..64) of values written or read by index (inputs packed into an array, a table, a buffer); one `array` per distinct use |
| `branch(X)` | container | a choice between behaviours on a condition; X is what the chosen arm(s) do |
| `loop(X)` | container | a step repeated a bounded number of times (count from a parameter or constant, or until a condition with a stated bound); X is the repeated body |
| `call(X)` | container | a helper computation the prompt names (its own parameters and result) that the solution must define and use; X is the helper's own body. The functional check cannot see helper use; the family describes the specification |

`seq(A,B,...)` writes steps one after another. It is grouping only: not a primitive, never counted.

Grammar (ASCII, no spaces, 1..512 bytes, at most 8 levels of nesting, at most 64 nodes):

```
node      := leaf | container "(" list ")" | "seq(" node "," list ")"
list      := node ("," node)*
leaf      := "arith" | "array" | "nested"
container := "branch" | "call" | "loop"
```

A container needs at least one child, a leaf takes none, a `seq` needs at least two children.

## 3. Signature

Canonical form: (1) a `seq` directly inside a container or inside another `seq` is replaced by its
children (`loop(seq(a,b))` = `loop(a,b)`, `seq(a,seq(b,c))` = `seq(a,b,c)`); (2) every child list is
sorted by byte order (C locale `strcmp`) of the children's canonical strings, duplicates kept. Sibling
order therefore never matters; nesting does (`loop(branch(arith))` and `branch(loop(arith))` differ).

**Signature** = `gs1:` + canonical form, e.g. `seq(loop(branch(arith),array),array)` has signature
`gs1:seq(array,loop(array,branch(arith)))`. `famsig canon <shape>` prints it.

**Family token** (for fields that allow only `[a-z0-9_-]`, such as the record `family` field of
`tools/gen/retrieval/CONTRACT.md` and its `--exclude-family`): `gs1-` + the first 16 lowercase hex digits of
sha256 of the signature bytes (`gs1:` included, no newline). `famsig token <shape>` prints it. A record or
task carries the token of the family of the task it was written for. (Added 2026-10-15, before any
consumer used v1: additive, no rule changed.)

**Composition edges** of a shape: `P>C` for every container P and every child C of P after flattening,
where C is the child's head primitive (`famsig edges <shape>`). **Coarse edges**: the same with a child
`nested` read as `arith`, duplicates removed. Coarse edges gate through R7 (section 4).

**Coarse form** (rule R6; added 2026-10-15 at review of PR #486, before any held-out task existed): (1)
`nested` is read as `arith`; (2) `seq` is flattened as in the canonical form; (3) every child list is
sorted and duplicate siblings are collapsed to one, and a `seq` left with one child is that child; (4) a
top-level `seq` whose children are exactly one container plus only leaves is replaced by that container.
**Coarse signature** = `gc1:` + coarse form; `famsig coarse <shape>` prints it. Examples:
`loop(branch(nested))`, `loop(branch(arith,arith))` and `seq(arith,loop(branch(arith)))` all have
`gc1:loop(branch(arith))`; `loop(branch(arith),arith)` has `gc1:loop(arith,branch(arith))` (a leaf added
inside a container is a different coarse family; R7 decides whether it adds an edge the corpus lacks).

## 4. Metadata lines and the disjointness rule

One line per task, fields separated by exactly one space:

```
<task_id> <split> <arith> <branch> <loop> <array> <call> <nested> <shape>
```

- `task_id`: `[a-z0-9_-]{1,40}`; `split`: `corpus` | `dev` | `heldout`; the six counts: decimal 0..64,
  no leading zeros, in exactly this order; `shape`: section 2.
- **R1** every line parses; each declared count equals that primitive's number of occurrences in the
  shape (the multiset is declared and cross-checked, so a typo in either is refused).
- **R2** task ids are unique across every split.
- **R3** exemplar (corpus), dev and held-out families are pairwise disjoint: one signature never
  appears in two splits.
- **R4** every primitive used by a held-out task is used by at least one corpus task.
- **R5** every held-out shape uses at least two distinct primitives (a held-out family is a combination).
- **R6** coarse signatures (section 3) are pairwise disjoint across corpus, dev and held-out: two tasks
  of different splits with different signatures never share a coarse signature (equal signatures are R3).
  This refuses a held-out family that is a corpus or dev family with a duplicated sibling, a `nested`
  for an `arith`, or a top-level `seq` wrapper around it plus leaves.
- **R7** every held-out shape has at least one coarse composition edge (section 3) that no corpus shape
  has (`novel_edges >= 1`). Wrapping a corpus shape in `seq(...)` or relabelling a leaf adds no edge, so
  it fails R7.
  (R6 and R7 added 2026-10-15 at review of PR #486, before any held-out task existed.)
- **R8** no top-level step of a dev or held-out shape is a corpus family in coarse form: after the
  top-level `seq` is flattened, every step that is a container (`branch`, `loop`, `call`) has a coarse
  form (section 3) that differs from the coarse signature of every corpus task. Leaf steps are not
  compared (R4 requires leaves to be reused); a shape whose whole coarse signature equals a corpus one is
  R3/R6. `seq(loop(branch(arith)),loop(array))` next to a corpus `loop(branch(arith))` fails R8.
  (R8 added 2026-10-16 at review of PR #486, before any held-out task existed; dev/META passes it against
  the GEN-3 corpus META.)

Known limit: R6, R7 and R8 are syntactic. A corpus family nested below the top level and given a leaf
sibling, such as `loop(branch(arith),arith)` next to a corpus `loop(branch(arith))`, passes R6 (different
coarse form) and R8 (not a top-level step), and passes R7 only if no corpus shape has the edge
`loop>arith`. What the shape cannot show (the same algorithm declared under a different shape) is the job
of the reviewer's blind re-declaration and the near-duplicate check (GEN_PROTOCOL_V1 section 2).

`famsig rule <file>...` applies R1..R8 over every task of every file given, prints per split
`split <s> tasks=<n> families=<m>`, then one `family <split> <signature> tasks=<k>` line per family
(held-out families only with `--show-heldout`, adding `novel_edges=<e>`: coarse edges no corpus shape
has), and ends `RULE PASS` (exit 0) or `FAIL R<k> ...` lines and `RULE FAIL` (exit 1). A failure that
involves a held-out task hides the held-out signature unless `--show-heldout` is given. Counts (60
held-out tasks, at least 6 held-out families, 10 dev tasks) are checked by `gencheck` against
`protocol.json`, not by `famsig`.

`famsig compare <SPLIT> <REDECLARED>` (blind re-declaration, GEN_PROTOCOL_V1 section 2): both files are in
the `gen-split 1` format of section 6 (any salt; the reviewer draws its own); it prints only `tasks=<n> mismatches=<k>`, where a mismatch
is a task whose signature differs between the two files, and exits 1 when k > 0 or the two files do not
name exactly the same task ids. It never prints a signature.

## 5. Where metadata lives

| split | file | visibility |
|---|---|---|
| dev | `tests/one_layer/generalization/dev/META` | public, in the repo |
| corpus | `tests/one_layer/generalization/corpus/META` (Agent D, GEN-3) | public, in the repo |
| heldout | `SPLIT` in the custodian's sealed directory (owner `gencust`, mode 0700) | sealed |

Public files (`gen-meta 1`): first line exactly `gen-meta 1`; then metadata lines; `#` comment lines
and blank lines are allowed; a `heldout` line is refused. Bytes: `\n` and 0x20..0x7E only, final `\n`.

Held-out metadata NEVER goes inside `tasks.tar` (whose member rules, HARNESS_INTERFACE_V2.md section 3,
refuse any extra file) and is never published before the run.

## 6. The sealed split file (`SPLIT`, written by the custodian)

```
gen-split 1
salt <64 lowercase hex: 32 random bytes the custodian draws once>
<metadata line, split heldout>          (one per held-out task)
```

Rules: line 1 exactly `gen-split 1`; line 2 the salt; then only `heldout` lines, task ids strictly
ascending in byte order; no comments, no blank lines; `\n` and 0x20..0x7E only; final `\n`. The task ids
are exactly the case directories of the sealed `tasks.tar`.

- `split_sha256` = sha256 of the SPLIT file's bytes (commits every task's family membership).
- **FAMLIST** = the bytes `gen-famlist 1\n`, `salt <salt>\n`, then every distinct held-out signature
  sorted by byte order, one per line with `\n`. `famlist_sha256` = sha256 of FAMLIST. The salt makes
  both digests unguessable by enumerating the (small) space of shapes.
- `famsig split SPLIT` checks the format and prints `split_sha256=`, `famlist_sha256=`, `tasks=`,
  `families=` (nothing else); `famsig famlist SPLIT` prints FAMLIST.

Before sealing, the custodian runs (as `gencust`) `famsig rule dev/META corpus/META SPLIT` (R1..R8) and seals
only on `RULE PASS`. It must use the corpus META at the commit the run pins.

## 7. The public commitment (`HELDOUT.pub`, published by the custodian before any held-out generation)

```
gen-heldout-pub 1
tasks_sha256 <64 hex: sha256 of tasks.tar, as in TASKS.sha256>
split_sha256 <64 hex>
famlist_sha256 <64 hex>
tasks <n>
families <m>
task <task_id>                            (one per task, strictly ascending, n lines)
```

Exactly these lines in this order, single spaces, final `\n`. Nothing about any family is published
except `families <m>` and the two salted digests. After the run (or for the independent review, after
the seal), anyone holding SPLIT verifies with `gencheck split` that SPLIT matches HELDOUT.pub
(digests, ids, counts) and that R1..R8 pass against the pinned dev and corpus META.

## 8. Reserved dev families (v1): signature and token

The 10 dev fixtures `devgen01..devgen10` hold these four signatures; no corpus or held-out task may use
them (R3):

```
gs1:branch(loop(arith),nested)              gs1-e05d1c5dffd08b06
gs1:loop(call(nested))                      gs1-bd9c105e5ae45397
gs1:loop(loop(branch(arith)))               gs1-f63e09ec03a454ee
gs1:seq(array,loop(array,branch(arith)))    gs1-30938c5c7504ef0e
```

Their coarse signatures (R6: no corpus or held-out task may have these either):

```
gc1:branch(arith,loop(arith))
gc1:loop(call(arith))
gc1:loop(loop(branch(arith)))
gc1:loop(array,branch(arith))
```
