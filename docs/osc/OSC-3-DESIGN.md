# OSC-3 design: the third Omega Systems Core compiler slice

OSC-3 slice; not a general Omega compiler; no self-hosting.

OSC-3 builds on OSC-2 (docs/osc/OSC-2-DESIGN.md; omega #148..#151) under
ADR OMEGA-SYSTEMS-CORE-0000. Each item is its own pull request, merged before
the next, with a content-addressed receipt in `evidence/OSC-3/receipts/`
written by `make osc3-receipt ITEM=<item> PHYSICS_DIR=<pinned physics>`.

Every OSC-3 receipt records: `make test-compiler-full` PASS (plain and
ASan/UBSan), contract/struct/arena fuzz with 0 mismatches, runtime model replay
with 0 rejected runs, the full legacy writer differential, golden and negative
counts, the golden corpus digests compared program by program with the OSC-2
encoder receipt (identical unless the item says why), cross-process
determinism, and the M6/M9/M14 gate logs compared by sha256 with the OSC-2
encoder receipt. Item-specific evidence lines start with `osc3 <item>:`.

## Item 0: owed external review of OSC-2

OSC-2 (#148..#151) was reviewed only by the Gemini fallback because Codex was
at its usage limit. The owed Codex review ran on 2026-10-01: first attempt
11:30Z refused (usage limit, reset 12:03Z); second attempt 12:04Z,
`codex exec -s read-only -c model="gpt-6-astra"` over
`git diff 0abdb08^..7e713e3 -- src/compiler src/language tests/compiler mk/compiler.mk src/aarch64_encoder.c`,
asked for correctness bugs only (miscompilation, interpreter/native
disagreement, missed contract traps, unsound ownership/borrow/arena checks,
compiler memory safety, layout, encoder ranges) with concrete failing inputs.
Result: **NO FINDINGS**. No item 0 fix PR was needed; the result is recorded
here and in the item 1 pull request.

## Item 1: quick and full compiler suites

`make test-compiler` had grown to 1m32..2m23 single core, mostly the legacy
writer's exhaustive differential and the full fuzz counts, against a 3-minute
budget. It is split:

- `make test-compiler-full`: everything, at the full counts. This is the old
  `test-compiler` (kept as an alias with the same meaning). Receipts use it.
  CI runs it when `src/compiler/` or `src/aarch64_*` changes.
- `make test-compiler-quick`: the same binaries, every golden and negative
  program, determinism, the legacy caller allowlist; smaller sweeps (model
  10^5 sequences, compiler fuzz 120 per entry, plain and under ASan/UBSan (smaller counts miss coverage checks),
  legacy writer differential in its sampled mode). Target under 60 s single
  core. CI runs it on every change to the compiler paths. It is a smoke run
  and is never receipt evidence.

The quick wall time is measured inside every OSC-3 receipt
(`test_compiler_quick.wall_seconds`, binaries already built).

CI: the `compiler` job in `.github/workflows/host-suites.yml` runs quick on
every pull request touching the compiler paths and full when `src/compiler/`
or `src/aarch64_*` changes. Since omega #154 (Lane 34) the workflow also runs
on every push to main, and the compiler job then runs quick and full, so main
has its own post-merge result (the 2026-10-01 audit found the compiler suites
never ran after merge). An earlier draft of this item added a separate
`compiler-main.yml` for this; it was dropped as redundant after #154.

## Item 2: versioned handles

The OSC-0B model (`src/compiler/model/osc_model.h`) gives slots u64
generations with `SLOT_ALLOC` / `SLOT_FREE` / `HANDLE_USE` events: a use or
free with the wrong generation is `STALE_GENERATION`, and a slot freed at
`gen_max` is retired, never wrapped (`GENERATION_WRAP`). Item 2 compiles that
discipline.

### Subset

- A pool is a fixed-capacity block of K slots (1..16) of one scalar type,
  local to one function. It is declared as a block statement and closed at
  the end of that block (also on a `return` from inside it), like an arena.
- A handle is a (slot, u64 generation) pair of one pool. Handles are
  copyable values and never owners: copying a handle does not move
  anything, a handle going out of scope frees nothing, and closing the pool
  ends every slot whatever handles exist.
- Pools and handles do not cross function boundaries (parameters, returns),
  are not struct fields or array elements, and are never values in
  arithmetic, comparisons or `return`.
- The subset has no durable encoder: no handle (and no borrow) can be
  written to durable state. The model's `LIVE_HANDLE_DURABLE` refusal is
  therefore unreachable from compiled code; a durable writer for handles
  must name that refusal before it exists.

### Grammar

```
stmt   += 'pool' NAME ':' '[' scalar ';' INT ']' ( 'gen' INT )? block
        | 'let' 'mut'? NAME ':' 'handle' NAME '=' hexpr ';'
        | NAME '.' 'free' '(' NAME ')' ';'
        | NAME '[' NAME ']' '=' expr ';'      (pool write)
        | NAME '=' hexpr ';'                  (handle rebind, NAME mut)
hexpr  := NAME '.' 'alloc' '(' expr ')' | NAME   (a handle of the same pool)
expr   += NAME '[' NAME ']'                   (pool read)
```

`gen INT` declares the generation base (default 0): every slot starts at
that generation. It exists so tests can reach exhaustion with a few
allocations; the base is part of the unit's canonical encoding.

### Semantics

- `p.alloc(e)` takes the lowest free slot, stores `e`, and yields
  (slot, current generation of that slot).
- `p.free(h)` checks h, then advances the slot's generation; at
  2^64 - 1 the slot is retired instead and is never allocated again.
- `p[h]` and `p[h] = e` check h: the slot must be live and its generation
  must equal h's.
- Run-time traps (identical in the interpreter and native AArch64, which
  share the runtime through fixed function-pointer offsets 48..96):
  `STALE` (wrong generation or slot not live, on use or free),
  `RETIRED` (alloc with no free slot while some slot is retired) and
  `POOL_FULL` (alloc with every slot live).
- Every pool operation is logged (`POOL_OPEN`, `SLOT_ALLOC`, `SLOT_FREE`,
  `HANDLE_USE` with rights 1 = read / 2 = write, `POOL_CLOSE`) with its
  generation folded into the event hash. The test harness replays each
  native run's log through `osc_model` (one model per pool, base = declared
  base, max = 2^64 - 1) and requires rejected = 0; for a trapping run the
  prefix replays clean and the trap itself is confirmed by the model
  (`STALE` as `STALE_GENERATION`, `RETIRED` as `GENERATION_WRAP` on every
  slot, `POOL_FULL` as `PROTOCOL` on every slot).

### Static refusals (named)

The checker tracks a provenance per allocation; copies share it. A
provenance is freed when every path freed it (branch merge intersects).

| Diagnostic | Transition | When |
|---|---|---|
| `STALE_HANDLE` | read / write after free | use of a handle whose slot was freed on every path |
| `STALE_HANDLE` | ... via a copy | the same through a copy made before the free |
| `STALE_HANDLE` | free after free | double free (also via a copy) |
| `GENERATION_EXHAUSTED` | retired slot reused | definite allocations exceed K x (2^64 - base) |
| `POOL_CAPACITY` | pool full | definite live allocations exceed K |
| `POOL_CAPACITY` | pool size outside 1..16 | K outside 1..16 |
| `WRONG_POOL` | handle of another pool | handle of pool p used with pool q, or `q.alloc` bound as a handle of p |
| `TYPE_MISMATCH` | handle / pool used as a value; pool index not a handle | type confusion |
| `UNSUPPORTED` | handle crosses a function boundary | pool or handle parameter or return type |
| `IMMUTABLE_ASSIGN` | | rebinding a handle declared without `mut` |

"Definite" means at the pool's own loop and branch depth. Allocations inside
loops or branches are not counted; once a free happens behind a branch or
loop, the live count is no longer tracked statically (the generation count
still is). What the checker cannot decide is checked at run time (the traps
above).

### Limits

- 16 slots per pool, 8 pools open at once per run, scalar elements only.
- No handle parameters or returns, no handles in structs or arrays, no
  durable encoding (see Subset).
- The model replay has 64 handle ids; the harness re-windows the model
  (fresh model seeded with the live slots and their generations) past that.
- Serial counters (audit finding): the runtime's object/region serial
  (`next_serial`) and the pool instance serial (`pool_serial`) stay `uint32_t`
  (they are hashed into the event log as 4 bytes, so widening them would
  change every logged run digest). Instead they never wrap: an allocation,
  arena open, arena allocation or pool open with its counter at `UINT32_MAX`
  traps `RUNTIME` before any state changes, identically in the interpreter and
  native code (both call the same runtime). The handle fuzz leg checks all
  four sites directly. Handle generations themselves are `u64` and retire at
  2^64 - 1 (above).

### Evidence

Measured on the Spark, single core, `nice` (full counts = `make
test-compiler-full`, identical plain and under ASan/UBSan):

```
osc3 handles: units=125 compiled=102 static_refused=23 runs=4896 ok=2705 stale_traps=1616 pool_full=530 retired=45 overflow=0 mismatches=0
osc3 handles: model replay pool_events=48807 stale_confirmed=1616 wrap_confirmed=45 full_confirmed=530 windows=0 (handle fuzz runs only)
osc3 handles: serial counters at UINT32_MAX trap RUNTIME, never wrap: 4 of 4
runtime model replay: ... rejected=0
```

- Every run is executed by the interpreter and natively; outcomes (return
  value or trap, runtime state, event log with generations) are identical.
- Every native run's event log replays through `osc_model`; every STALE,
  RETIRED and POOL_FULL trap is confirmed by the model on the replayed
  prefix (counts equal the trap counts above). The `cycle` golden (100
  allocations from one slot) exercises the model re-window.
- Static refusals in the fuzz are only `STALE_HANDLE` (any other refusal
  fails the leg).
- Goldens: `handles_basic`, `handles_stale`, `handles_exhaust` (13
  `expect-run` lines). Negatives: 15 `h_*` programs, one per refusal above.
- OSC-1/OSC-2 golden programs compile byte-identically (IR and code digests
  checked by the item 2 receipt against the OSC-2 baseline).
- The exact receipt is under `evidence/OSC-3/receipts/osc3-handles-*.json`.

## Item 3: drop flags (conditional moves)

OSC-1 and OSC-2 refused any move of an owner inside an `if`
(`CONDITIONAL_MOVE`, OSC-1-DESIGN "no drop flags"). Item 3 accepts it: an
owner that is moved on some paths and not on others gets a hidden drop flag,
and its scope-end release runs only when the flag says live. This supersedes
the OSC-1 rule; `CONDITIONAL_MOVE` keeps its number but is no longer emitted.

### Subset

- Owners are unique array owners (`own [T; N]`) and unique struct owners
  (`own S`), declared by `let` (alloc, struct literal or let-move) or as an
  `own` parameter. Arena owners (`in r`) are unchanged: they cannot be moved
  (`ARENA_MOVE`), so they never need a flag. Handles (item 2) own nothing
  and are out of scope.
- A move is a let-move (`let b: own T = a;`) or passing the owner as an
  `own` argument. Either may now sit inside one branch of `if` / `else` /
  `else if`, at any nesting depth.
- After an `if`, each owner declared before it is in one of three states:
  live (moved on no path that falls through), moved (moved on every path
  that falls through) or maybe-moved (moved on some). A branch that ends in
  `return` does not count: its state never reaches the join.
- Only maybe-moved owners get a flag. Programs without a conditional move
  compile to byte-identical IR and code (no `OSC3_CODE_CHANGE_OK`).

### Grammar

No new syntax. The grammar of OSC-2 is unchanged; programs that OSC-2
refused with `CONDITIONAL_MOVE` are now accepted when the rules below hold.

### Semantics

- Merge rule at the end of an `if` (both arms falling through): equal
  states stay; any difference (live vs moved, live vs maybe, moved vs maybe)
  becomes maybe-moved. If one arm returns, the other arm's state is taken.
- Flag: one `bool` virtual register per flagged declaration, allocated by
  the lowerer when the checker marks the declaration (`OscNode.dflag`). It
  is set to 1 (`CONST 1`) right after the declaration (for an `own`
  parameter: first thing in the entry block), and set to 0 (`CONST 0`) at
  every move of that owner (before the `CALL` for an own argument, before
  the new binding for a let-move), on every path.
- Release: every scope-end release of a flagged owner (block end and every
  `return` it is live or maybe-moved at) becomes
  `CBR flag -> release, join; release: RELEASE owner; BR join`. Owners that
  are definitely moved are not released (as in OSC-2); live owners without
  a flag are released unconditionally (as in OSC-2). Release order stays
  reverse declaration order; a skipped release simply drops out.
- Loop-local owners: an owner declared inside a loop body may be moved on
  one path of that body; its declaration sets the flag again each
  iteration, so each iteration's object is released at most once.
- Frame layout: the flag is an ordinary vreg, so it has the ordinary stack
  slot of the native frame, `[sp, #8*(v+1)]` (`osc_cg.c slot()`), holding
  0 or 1 in a 64-bit cell; non-parameter slots are zeroed by the prologue
  and the declaration's `CONST 1` sets it. No new IR op, no interpreter or
  backend change: `CONST`, `CBR`, `BR` and `RELEASE` already exist, so both
  engines run the same IR and agree by construction.
- Static model trace: around the `if` the checker emits `SAVE`/`RESTORE`
  as before. A definitely moved owner gets its `MOVE obj -> 0` re-emitted
  after the `if` (unchanged). A maybe-moved owner emits nothing after the
  `if`: on the straight-line trace it stays live and its guarded scope-end
  `RELEASE` replays as a plain release (the trace follows the path on which
  the flag is set). Each run's runtime log replays exactly (a release that
  did not run is simply absent).
- Soundness argument: the flag equals "this owner's object has not been
  moved" on every executed path, because it is set at the (re)declaration
  and cleared at each move, and every release of a flagged owner tests it.
  So no path releases twice (no `RUNTIME` double-release trap) and no path
  leaks (live count 0 after a normal return); both are checked on every
  fuzz run in both engines.

### Static refusals (named)

| Diagnostic | Transition | When |
|---|---|---|
| `USE_AFTER_MOVE` | use after maybe-move | read, write, field access or call use of a maybe-moved owner ("used after it may have been moved at line N (moved on some path of an 'if')") |
| `USE_AFTER_MOVE` | use after maybe-move | borrow (`&` / `&mut`) of a maybe-moved owner |
| `USE_AFTER_MOVE` | use after maybe-move | moving a maybe-moved owner again (double move across branches / two ifs) |
| `USE_AFTER_MOVE` | use after move (move inside loop) | an owner declared outside a loop moved inside it, also inside an `if` in the loop (unchanged from OSC-1) |
| `IMMUTABLE_ASSIGN` | assign to immutable binding | re-assigning an owner, maybe-moved or not ("owners are never reassigned"); item 3 does not add a set-live rule |

For the model the maybe-move refusals are use-after-move: the checker emits
an accepted `MOVE obj -> 0` (the path that moved it) right before the refused
event, so the negative harness replays them as `use-after-move`.

### Limits

- Flags exist only for owners moved inside `if` arms; moves of an outer
  owner inside a loop stay refused (no re-initialisation rule).
- No reassignment of owners (refused), so a flag only goes live -> moved
  within one declaration's lifetime.
- One flag per declaration (up to the checker's symbol limit); flags are not
  packed into bits.
- The static trace checks the path on which each maybe-moved owner is still
  live; the moved path is checked by every executed run's runtime replay
  (fuzz and goldens), not statically.

### Evidence

EVIDENCE_PLACEHOLDER

## Generation width: remaining 32-bit sites and fix plan

ADR OMEGA-SYSTEMS-CORE-0000 decision 1 fixes capability and object generations
at u64 end to end, a slot retired at the maximum, never wrapped. The 2026-10-01
audit (track 2, s.F.3) found these sites still 32-bit; checked against main
07004a8. A u64 generation stored into any of them is silently truncated, and a
truncated generation can make a stale handle look live again.

| Site (main 07004a8) | Today | Owner | Fix plan |
|---|---|---|---|
| `src/runtime/rx_world.h:177` `RxObjRef{uint32_t id; uint32_t generation}` | u32 | runtime lane (src/runtime) | widen `generation` to u64; retire the object id at UINT64_MAX; versioned like omega#71 |
| `src/runtime/rx_world.h:305` object `generation` | u32 | runtime lane | same change, same PR as :177 |
| `src/runtime/rx_world.h:328` `RxSub{reaction; generation; mask}` | u32 | runtime lane | widen with :177 (subscriptions compare against object generations) |
| `src/runtime/rx_world.h:599` `seat_generation` | u32 | runtime lane | widen; retire the seat at max |
| `src/runtime/rx_graph.c:517, :529` crumb/graph hash writes object generations with `put32` | u32 on the wire | runtime lane | `put64` together with a crumb/graph record version bump (old records refused, not reinterpreted), in the identity-break sequence (item 5) |
| `src/runtime/rx_jspace.c:1406, :1413, :1414` J-Space body writes branch/slot generations with `put32` | u32 on the wire | runtime lane | `put64` with a J-Space record version bump, same rule |
| `src/omega_accelerator.h:67, :87` `uint32_t capability_generation` | u32 | accelerator owner (not OSC) | widen to u64, retire at max; ADR decision 1 names this file |
| `src/compiler/osc_rt.h` `slot_serial[]`, `next_serial`, `pool_serial` | u32, never wraps (item 2) | OSC (this lane) | done in item 2: handle generations are u64 with retire-at-max; the serials stay u32 (hashed into the event log as 4 bytes) and an allocation, arena open, arena allocation or pool open with its counter at UINT32_MAX traps RUNTIME before any state changes (item 2, Limits) |
| AIENOS ADR 0013 (`index u32; generation u32`, retire at u32::MAX) | u32 | aienos lane | request only, see item 5 (`docs/osc/OSC-3-ADR0013-REQUEST.md`); not edited from this repository |

Only the `osc_rt` row is inside this lane's files. The others are listed so the
owning lanes can schedule them; each is a versioned change with old records
refused, never reinterpreted. Only the `osc_rt` row is applied by OSC-3 (item 2).

## Slice order after item 4

Per the audit (III.7 and the 2026-10-01 review), the first real migration is
crumbline (`src/crumbline/`) once effects and capabilities exist:
item 4 (effects/capabilities), then item 5 (identity break + ADR 0013
request), then item 6 (crumbline as the first production C module under
contracts; proposal in docs/osc first).
