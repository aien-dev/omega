# Zig lessons: bounded compile-time evaluation and incremental compilation

- Status: Proposed. This is a decision record. It changes no code.
- Date: 2026-10-13
- Scope: the OSC compiler slice (`src/compiler/`), the Verified Crumb store and resolver, and the realization identity layer.
- Origin: an outside review of Omega against Zig suggested (3) a restricted, fuel-bounded compile-time evaluator and (4) a content-addressed incremental compile cache.
- Method: every claim about the code carries a `file:line` that was read on `origin/main` at 5d3c36c9. Anything not checked is marked UNVERIFIED. Zig facts come only from pages fetched for this record (listed at the end).

## Decisions in short

1. Compile-time evaluation: build a small first step (named integer constants that can stand where the parser today demands a literal) on top of the folding the checker already has. Do not build a compile-time function interpreter now. If one is ever built, it must meet the contract in "Decision for comptime", including an explicit step budget with its own refusal kind and no way to see the machine.
2. Incremental compilation: do not build it now. The OSC compiler finishes a program in milliseconds. The build and test time is spent in sanitizer test binaries, which an incremental compiler would not touch. Define the cache key and the measurement that would justify a cache later, and build nothing until that measurement says so.

## Context

Zig's compiler evaluates ordinary code at compile time under a budget, and its 0.17.0 release (2026-10-01) makes incremental rebuilds usable for most x86_64-linux projects. The review asked whether Omega should copy either idea. The right first question for each is what Omega already has and what it costs today. This record answers from the repository and from measurements, not from Zig's reputation.

## What exists today

### Constant folding in the checker

- `fold` (`src/compiler/osc_check.c:623-720`) evaluates integer constant expressions at a known scalar type: literals (632-638), unary minus and `~` (639-668), and `+ - * / % & | ^ << >>` (669-702). It refuses bool (627), any other operator (701) and any non-constant leaf (713). On success it stores the result on the node (`is_const`, `cval`, 715-717; fields in `osc_parse.h:105-106`).
- It is reached only through `chk` and only when every leaf is a literal: `constexpr_` (605-614) accepts `ON_INT`, `ON_UN`, `ON_BIN` and nothing else, so a name never folds (`osc_check.c:1192-1196`).
- Its refusals reuse existing kinds: `OVERFLOW_UNSAFE` for signed overflow, divide by zero and shift range (`ovf`, 616-620; 679, 686-688, 703-707).
- What bounds it: it is a plain tree walk with no loops and no calls. Parser nesting is capped at 64 (`osc_parse.h:19`, enforced `osc_parse.c:74-77`), nodes at 16384 (`osc_parse.h:18`, `osc_parse.c:54-56`), tokens at 32768 (`osc_lex.h:14`). It needs no fuel today and has none.
- A second, separate evaluator decides contract clauses: `ceval` (`osc_check.c:756-`, environment `CEnv` at 732-739). Names are known only when the caller supplies a literal argument; array reads and calls are unknown (comment 724-731). A clause that folds true is not emitted at run time (`req_elide`, 1465-1473; `ens_elide`, 1511-1519); one that folds false is refused as `CONTRACT_VIOLATION` (1467, 1513).

### Where a literal is still required

The parser asks for an `OT_INT` token, with no names and no expressions, at: array length (`osc_parse.c:118`, range check 120-126, array length over 64 is `CAPACITY`), `while ... bound N` (908, range 1..1000000 at 910-915, limit defined at line 11), `for i in A .. B` (931-941), arena `bound K` (874-876), pool slot count (844) and pool generation base (860). Array lengths become numbers inside `OscType` while parsing (`osc_parse.c:112-126`), before the checker runs. `const` is a reserved word in the lexer (`osc_lex.c:26`), so no constant item syntax exists.

### The language already terminates

There is no recursion and no forward reference (`docs/osc/OSC-1-DESIGN.md:25, 111`), and every loop has a literal trip bound of at most 1,000,000 (`osc_parse.c:11, 910, 934`). So every OSC program terminates. Termination is not the same as cheap: two nested maximal loops are 10^12 iterations.

### The interpreter

- `osc_interp_run` (`src/compiler/osc_interp.h:25`) runs a lowered, validated `OscUnit` (the typed IR). It counts every executed instruction and traps `OSC_TRAP_RUNTIME` past `OSC_INTERP_MAX_STEPS = 2^32` (`osc_interp.h:15-17`; check at `osc_interp.c:161`). The header calls this "defensive": the static loop bounds are the real limit.
- `OSC_TRAP_RUNTIME` is documented as "runtime invariant broken ... must never happen for checked code" (`osc_ir.h:159-160`).
- The front end does not call the interpreter: the pipeline is lex, parse, check, lower, validate (`osc_front.c:27, 31, 57-58, 60`), and `osc_front.c`, `osc_check.c` and `oscc_main.c` contain no reference to it. Callers are tests (`tests/compiler/test_osc_compiler.c`, `test_osc_backend.c`, `tests/physics0/test_pd0_osc.c`) and `src/osh/host/osh_core.c`.
- It is compiled into `oscc` (every file in `src/compiler/` except `oscc_main.c` is in the library, `mk/compiler.mk:22, 51-53`) but `oscc` never calls it.
- Instructions: scalar ones are `CONST MOV BIN UN CMP CAST CALL TRAP BR CBR RET` (`osc_ir.h:74-94`). Others touch the run-time pool `OscRt`: `ALLOC RELEASE LOAD STORE`, struct, arena, pool, handle and slice operations (`osc_ir.h:83-125`; `osc_interp.c:169-175`).
- There is no I/O instruction. OSC-1 has no effects or capabilities (`OSC-1-DESIGN.md:37-38`), and the lexer reserves the words for them (`osc_lex.c:24-25`). `grep -rn "fuel\|step limit\|max_steps" src/compiler src/runtime` finds nothing in `src/compiler` and one unrelated plan field (`max_steps`, `src/runtime/rx_plan_arrange.h:41`). The interpreter cap is spelled `OSC_INTERP_MAX_STEPS`, in capitals, which that lowercase pattern does not match.

### Identities

- The OSC compiler's own identities are `ir_sha256` (`osc_ir_digest`, `osc_ir.h:246`) and `code_sha256`, printed by `oscc` (`oscc_main.c:136, 152-156`). The front end is documented as a pure function of the source bytes (`osc_front.h:3-5`), and `tests/compiler/determinism.sh` checks it across processes (`mk/compiler.mk:88, 134`).
- The Omega program layer has a separate semantic ID: `program_id = SHA256("omega.program.v2" ... body, types, pre and postcondition ids)` (`spec/program-identity.md:25-32`). Name, cost, realization bytes and machine are explicitly not part of it (`spec/program-identity.md:112-121`).
- A realization is identified by a triple id: `omega_realize_compute_triple_id` (`src/omega_realize_synth.c:24-70`) hashes the target profile, entry offset, code length, the semantic ID, the machine ID and the code bytes. A different machine ID or different code gives a different triple id; the semantic ID does not move.
- No code in `src/compiler/` refers to the Omega semantic ID (`grep -rn "SemanticId\|semantic_id" src/compiler` finds only the reference memory model, `model/osc_model.h:7, 34, 126, 143`). The IR that `omega_program_ir.h:3-5` describes is the Omega program IR, a different format from `OscUnit`. How an `OscUnit` maps to an Omega semantic ID is UNVERIFIED and may not be defined yet.

### The stores

- `omega_vcstore` is a trust store, not a cache. It holds Verified Crumb records keyed by the 32-byte semantic ID (`omega_vcstore.h:4-9`), one immutable record per ID (insert refusals, 12-15; objects sorted by semantic ID, 128), each carrying its realization ids, receipt id, evidence root, capabilities and dependencies (`OmegaVcView`, 100-117). It recomputes the record id on every read (16-17, 165-167). Records enter only through the resolver (161-163). The key has no machine ID and no compiler identity, and the header offers no way to remove a record.
- `omega_resolve` runs one chain per node and refuses on any failure, with no skip switch and no fallback (`omega_resolve.h:7-31, 53-54`). Record ids are SHA-256 (`omega_vcstore.h:123-124`); receipt ids are BLAKE3 (`omega_receipt.h:11`; the only caller of `omega_blake3_hash` in `src/` outside its own file is `omega_receipt.c:680`).
- Receipts already record the fields a cache key would need: `repo`, `commit`, `toolchain`, `machine`, `env_class`, `dirty`, input and output artifacts (`omega_receipt.h:33-40`). A receipt is found by its own id, not by those fields.
- A cache of derived results already exists elsewhere in Omega: the runtime semantic engine. Its rule is "reused if and only if" the derivation id (name, output type, implementation generation, implementation digest) and every input's id, generation and value digest match; a hit also compares the stored input record, not only the digest (`spec/omega-incremental-semantics.md:41-50`; `src/runtime/rx_semantic.c:441-452, 523-524`). That is the pattern to copy if a compile cache is ever built. It serves runtime cognition, not the compiler.

### Measured times

Measured 2026-10-13 on the DGX Spark, gcc -O2, `build/compiler/oscc` built with `make PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 oscc`. Rows marked "given" come from the orchestrator and were not repeated by this record.

| what | result |
|---|---|
| largest test program `tests/compiler/progs/oom.osc` (3194 bytes) | under 0.01 s, 8.2 MB peak RSS (given); re-measured with `/usr/bin/time`: 0.00 s wall, 8156 to 8216 KB over 3 runs; about 2 ms per run in a 10-run shell loop including process start |
| all 47 programs in `tests/compiler/progs` (40,529 bytes, 1,406 lines) | 0.089 s total (given); shell loop 112 to 121 ms over 4 runs, which includes starting 47 processes |
| synthetic near-ceiling unit: 16 functions x 200 statements (48,918 bytes, generated with a shell loop, not a real program) | 13 to 14 ms per run, 9.9 MB peak RSS; 32 functions x 200 statements is refused at the 32768-token cap (`CAPACITY`, `osc_lex.h:14`) |
| `make test-compiler-quick`, binaries already built | 28 s twice; 46 s when the seven test binaries had to be built first |
| the same quick steps timed one by one a few minutes later (other jobs on the machine, load average 1.8) | ASan backend 19.0 s, ASan legacy writer 12.9 s, ASan compiler fuzz 7.2 s: 39.1 s of 40.7 s, about 96 percent. Every plain-build step is under 0.7 s (model 0.15 s, backend 0.24 s, compiler fuzz 0.14 s, legacy writer 0.04 s, determinism 0.36 s, caller self-test 0.62 s) |
| plain full-count legs | model sweep at its default 10^6 sequences 1.4 s (`test_osc_model` default, `tests/compiler/test_osc_model.c:8`); legacy writer full differential 2.4 s; compiler tests at default counts 0.95 s |
| `make test-compiler-full` as a whole | NOT TIMED here. `docs/osc/OSC-3-DESIGN.md:33-35` records 1m32 to 2m23 single core for the suite before the quick/full split |

The quick figures vary by a third between runs because the machine is shared. The conclusions below do not depend on that spread.

### Zig facts used (fetched 2026-10-13)

- `https://ziglang.org/download/index.json`: lists `0.17.0`, dated 2026-10-01, with docs at `https://ziglang.org/documentation/0.17.0/` and notes at `https://ziglang.org/download/0.17.0/release-notes.html`.
- Language reference 0.17.0, comptime section: compile-time evaluation that runs past 1000 backwards branches is a compile error; `@setEvalBranchQuota` raises the limit; a smaller value than the default or an earlier quota is ignored. The same page admits the compiler overflows its stack instead of reporting the error for the infinite-recursion example. Section "Compile Variables": `@import("builtin")` gives compile-time constants "such as the current target, endianness, and release mode".
- Release notes 0.17.0, "Incremental Compilation": "near-instant rebuilds of projects after changing the code"; usable "for most projects targeting x86_64-linux"; enabled by passing `-fincremental --watch`; future work includes using it without `--watch`. Section "Cache System Reworked": normally only changed contents cause a cache miss, the cache can explain why a miss happened, and the on-disk format is now binary.
- Not checked: whether Zig comptime can perform I/O, and how Zig defines the identity of a build. This record makes no claim about either.

## Decision for comptime

### What we build first (named constants only)

Add integer constant items that are evaluated with the existing `fold` semantics and can appear wherever the parser today requires a literal: array length, loop bound, `for` range ends, arena bound, pool slot count, contract clauses and ordinary expressions.

- No new arithmetic. Operators, widths, wrap and overflow rules stay exactly those of `fold` (`osc_check.c:623-720`) and of OSC-1 section 5.
- A constant is defined before use, like a function (`OSC-1-DESIGN.md:109-111`). It is evaluated once, where it is declared, and its value is reused. Total work is one visit per node, bounded by the 16384-node cap, so this step needs no fuel and no new diagnostic kind.
- Every limit that applies to a literal still applies after folding, with the same refusal kind: loop bound over 1,000,000 stays `UNBOUNDED_LOOP` (`osc_parse.c:910-915`), array length over 64 stays `CAPACITY` (`osc_parse.c:125-126`), arena and pool sizes keep `ARENA_CAPACITY` and `POOL_CAPACITY`, arithmetic overflow stays `OVERFLOW_UNSAFE`.
- The identity rule is checkable: a program that uses a constant must produce the same `ir_sha256` and `code_sha256` as the same program with the value written as a literal.
- The surface syntax is not decided here. `const` is reserved today (`osc_lex.c:26`); the implementing change may un-reserve it or pick another word (Open question 2).

### What we do not build now (a compile-time function interpreter)

Running real functions at compile time through `osc_interp_run` is deferred, for three reasons found in the code:

1. Ordering. Array lengths are fixed by the parser (`osc_parse.c:112-126`) and the interpreter runs on lowered IR, which exists only after the whole unit is checked (`osc_front.c:57-58`). A function result could therefore feed values, not array lengths, loop bounds or arena and pool sizes, unless the pipeline is restructured.
2. Payoff. The IR has no optimisation pass that would use a computed value (`OSC-1-DESIGN.md:38`). I found no program in the corpus that needs one; UNVERIFIED, because I did not read all 47 programs for this.
3. Risk. A second evaluator beside `fold` and `ceval` (`osc_check.c:623, 756`) is one more place for arithmetic to disagree. The interpreter is the arithmetic oracle (`osc_interp.h:1-7`), but it is not wired into the checker.

### Contract if a function interpreter is ever added

- Deterministic. The result is a function of the source bytes only. The evaluator receives no clock, environment, file, locale or machine argument.
- Fuel. One unit per executed IR instruction, counted where the interpreter already counts (`osc_interp.c:161`), with the budget passed in by the caller. It is a parameter, not a global and not an environment variable. The existing 2^32 cap is far too large for compile time (two maximal nested loops already exceed it) and it reports `OSC_TRAP_RUNTIME`, which means a broken invariant, the wrong message for "too expensive".
- Default budget: 1,000,000 instructions per unit, which is one maximal loop of one instruction (`osc_parse.c:11`). How long that takes is UNVERIFIED. The interpreter's instructions per second must be measured before the number is fixed (Acceptance tests, B5).
- Refusal kind: a new `OSC_DIAG_COMPTIME_FUEL`, appended after `OSC_DIAG_SLICE_ALIAS` (`osc_diag.h:55`) with its name added to the table (`osc_diag.c:11`), so no existing value moves. It is not `CAPACITY`. `CAPACITY` means a fixed table or language limit was reached (`osc_lex.h:48-51`, `osc_parse.c:54-56, 125-126`, `osc_check.c:178-184`) and the remedy is to split the program. Out of fuel means a computation is too costly and the remedy is to simplify it. Negative tests identify a refusal by its kind name (`tests/compiler/neg/ar_capacity_static.osc:1`; `tests/compiler/test_osc_compiler.c:16, 686`), and the budget is a policy number we expect to tune while table sizes are structural. The kind is added in the same change as the first test that can trigger it, not earlier.
- No fallback. Out of fuel is a refusal. The computation is never moved to run time silently and no switch turns the check off, the same stance as `omega_resolve.h:53-54`.
- No effects. Only the scalar instructions `CONST MOV BIN UN CMP CAST CALL TRAP BR CBR RET` (`osc_ir.h:74-94`) are allowed. Anything that touches the run-time pool (`ALLOC`, `RELEASE`, `LOAD`, `STORE`, struct, arena, pool, handle and slice operations, `osc_ir.h:83-125`) is refused with the existing `UNSUPPORTED` kind. No I/O instruction exists, and none may be added for compile time.

### The machine-specialisation invariant

A compile-time value must never carry information about the machine into the program's meaning.

- Omega already separates the two things. Meaning is the semantic ID, which excludes realization, cost and machine (`spec/program-identity.md:112-121`). A particular machine-specific realization is identified by the triple of semantic ID, machine ID and code (`omega_realize_synth.c:24-70`). Specialising for a machine changes the machine ID and the code bytes, so the triple id changes, and the semantic ID does not.
- For OSC the same rule is: the compile-time evaluator has no machine input at all. Machine-dependent choices (register use, vector width, tile size) belong to code generation (`osc_cg`, `osc_a64`) and show up only in `code_sha256` and, at the Omega layer, in the realization triple id. If a machine-derived number reached a constant, `ir_sha256` would differ per machine, which is the OSC form of corrupting the semantic ID.
- Zig makes the opposite choice: compile-time code can read the target through `@import("builtin")` (Compile Variables, fetched). That suits Zig, whose product is one binary for one target. It does not suit Omega, where one meaning has many realizations. This is a difference to keep, not a gap to close.
- Caveat: the link between an `OscUnit` digest and an Omega semantic ID is UNVERIFIED (see Identities). The rule is stated at both levels so it still holds when that link is defined.

### Candidate uses

| # | use | what proves it |
|---|---|---|
| 1 | Named loop bound: `const N: u32 = 8;` then `for i in 0 .. N` or `while c bound N` | A1 (same digests as the literal spelling), A2 (limit still enforced) |
| 2 | Derived array length: `[u32; LANES * 4]` | A3 (same digests; 64 + 1 refused `CAPACITY` like literal 65) |
| 3 | Arena and pool sizing from one named constant | A4 (`ARENA_CAPACITY` at 65 as in `neg/ar_capacity_bound_65.osc`; `POOL_CAPACITY` over 16) |
| 4 | Contract limits: `requires n <= MAX_N` | A5 (clause elided exactly as with a literal; a constant-false clause is `CONTRACT_VIOLATION`) |
| 5 | Table value computed by a loop in a compile-time function (only if the function interpreter is built) | B1 to B4: equals the run-time interpreter result, runs out of fuel in a defined way |

## Decision for incremental compilation

Do not build an incremental or cached compile path for the OSC compiler now.

- Per program, compile time is at the floor of what a process start costs: 0.00 s and 8.2 MB for the largest test program, 0.089 s for all 47 programs (given; re-measured at 112 to 121 ms), and 13 to 14 ms for a synthetic unit near the 32768-token ceiling. Unit size is capped by design (`osc_lex.h:14`, `osc_parse.h:18`), so there is a ceiling, not a growth curve. Zig's reason for incremental compilation, large projects where a rebuild takes long, does not apply to a compiler whose largest legal unit builds in about 14 ms.
- The time that hurts is in tests. 96 percent of the quick suite's individual step time is the three ASan/UBSan test binaries; the 10^6 model sweep is 1.4 s, so it is not where the time goes. The 18 s between a cold and a warm quick run is the C compiler building the seven test binaries, and `make` already skips unchanged ones by file timestamp (`mk/compiler.mk:27-53`). Caching compiler output does not shorten any of this. The levers are fewer or smaller sanitizer legs, run in CI only on the paths that changed (already done: `docs/osc/OSC-3-DESIGN.md:50-52`, `mk/compiler.mk:108-113`), or a C object cache. None is the idea under review.
- Where a content-addressed cache could pay later is the work that is expensive and is a pure function of identities: realization synthesis, verification and receipt production for a (program, machine, compiler) combination. Whether those are expensive enough to matter is UNVERIFIED; this record did not time them.

### The key, if a cache is ever built

```
key = SHA-256( "omega.cache.v1" 0x00
               || kind          (realization | verification-receipt)
               || semantic_id   (32 bytes)
               || machine_id    (32 bytes)
               || compiler_id   (32 bytes)
               || options_id    (32 bytes) )
```

- `semantic_id` and `machine_id` are the existing 32-byte ids (`omega_realize_synth.c:24-27`). The result is the realization id or triple id, plus the receipt id.
- `compiler_id` is a digest of the compiler's own content (the `oscc` binary, or the digest of the compiler source tree), not a git commit. A commit changes on documentation-only edits (needless misses) and does not exist for a dirty tree (receipts track this with `dirty`, `omega_receipt.h:36`). The commit is recorded in the entry as provenance. Whether the crumb `source_tree` digest of `src/compiler` (`src/compiler/.crumb`) is suitable as `compiler_id` is UNVERIFIED.
- `options_id` covers anything that changes output (target profile, flags). It does not cover timing or names.
- This is the rule `rx_semantic` already uses (`spec/omega-incremental-semantics.md:41-50`): the implementation digest is part of the key, and a hit compares the stored components, not only the hash.
- Each entry stores its key components in the clear, so a miss can be explained. Zig 0.17.0 added exactly this to its cache (Cache System Reworked, fetched).

### Rules that keep a cache from weakening trust

- A cache is a separate store, not `omega_vcstore`. The vcstore is immutable, keyed by semantic ID alone, accepts records only through the resolver and re-checks them on every read (`omega_vcstore.h:12-17, 161-167`). A cache needs the opposite: entries that may be dropped and rebuilt, with a wider key.
- A cache hit never grants trust. A cached receipt still goes through the full resolver chain (`omega_resolve.h:7-31`). A cached realization is re-hashed and compared to its recorded triple id before use.
- A miss or any inconsistency means recompute. There is no flag that serves a stale entry.

### The measurement that would justify building it

Add a shadow log first, with no cache behind it. For each candidate step (realization synthesis, verification, receipt production) write one line: key as defined above, kind, wall seconds. Collect it over a representative period of CI and development runs. Build the cache only if, from that log:

1. the share of wall time spent on steps whose key was already seen is above a threshold, and
2. the absolute saving is above a second threshold that clearly exceeds the cost of hashing inputs and reading an entry.

The thresholds are a policy call for Drake. Suggested starting values to argue with: 20 percent of step time and 60 seconds per day. Until that log exists the answer to "should we cache" is no.

## What we will not do

- No general `comptime` blocks, types as values or generics. OSC-1 excludes generics (`OSC-1-DESIGN.md:37-38`) and nothing here changes that.
- No compile-time allocation, arenas, pools, handles or slices, and no I/O or capability of any kind at compile time.
- No machine information visible to the evaluator, and no source-level knob to raise the budget (Zig's `@setEvalBranchQuota` makes the limit author-controlled). The budget, if one is needed, is a compiler constant until a real program shows it is too small.
- No silent fallback from a refused compile-time computation to run time.
- No incremental compile, file watcher, daemon or partial recompilation in `oscc`.
- No compile cache or change to `omega_vcstore` semantics (immutability, one record per semantic ID).
- No unification of `ir_sha256` with the Omega semantic ID in this record.

## Acceptance tests

Tier A (named constants). New programs follow the existing layout: positive programs in `tests/compiler/progs/`, refusals in `tests/compiler/neg/` with the `// expect: KIND object=NAME line=N` header (`test_osc_compiler.c:16, 686`).

- A1. `const N` loop bound versus the literal spelling: `oscc` prints identical `ir_sha256` and `code_sha256` for both files, for both `for` and `while ... bound`.
- A2. `neg/`: a constant 1000001 used as a loop bound is refused `UNBOUNDED_LOOP`, same as the literal (`osc_parse.c:910-915`).
- A3. Array length: `[u32; 4 * 4]` equals `[u32; 16]` in both digests. `[u32; 64 + 1]` is refused `CAPACITY`, same as literal 65 (`osc_parse.c:125-126`).
- A4. `arena a bound 64 + 1` is refused `ARENA_CAPACITY`; a pool of 17 slots by constant is refused `POOL_CAPACITY`.
- A5. Contracts: `requires n <= MAX_N` with a constant gives the same IR digest as with the literal; a constant-false clause is refused `CONTRACT_VIOLATION`.
- A6. Overflow and division: `const X: i8 = 100 + 100;` and `const D: u32 = 1 / 0;` are refused `OVERFLOW_UNSAFE` (`osc_check.c:679, 703-707`).
- A7. Differential: for a seeded set of constant expressions, the folded value equals the value the interpreter returns when the same expression is compiled as `fn f() -> T { return <expr>; }`, and both refuse the same expressions. This catches a second evaluator drifting from the first. Whether an existing fuzz already covers fold against the interpreter is UNVERIFIED.
- A8. Determinism: the constant programs are added to `tests/compiler/determinism.sh`, and `ir_sha256` does not change when `LANG`, `TZ`, `HOME` or the working directory change.
- A9. Machine independence, structural: the constant-evaluation entry point has no machine, clock or environment parameter, and a grep test fails if one is added (the pattern in `omega_resolve.h:53-54`).
- A10. Identity law, existing behaviour pinned: for one fixed program and two different machine IDs, `omega_realize_compute_triple_id` returns two different ids while the semantic ID is the same. Whether such a test already exists is UNVERIFIED.

Tier B (only if a function interpreter is built).

- B1. `neg/`: nested loops of 1,000,000 x 1,000,000 inside a compile-time function are refused `COMPTIME_FUEL`.
- B2. Boundary: a function that uses exactly the budget passes and one more instruction is refused. The budget is an API parameter, so the test sets it small.
- B3. A compile-time function that allocates, opens an arena or pool, or touches a slice is refused `UNSUPPORTED`.
- B4. The compile-time result equals the run-time interpreter result for the same function over a sweep of arguments.
- B5. Measure interpreter instructions per second on the Spark and record it; the default budget is set from that number, not from this record.

Cache (only if the shadow log justifies it).

- C1. Same inputs give a hit and byte-identical output; changing any one of the five key parts gives a miss.
- C2. A corrupted entry is detected and recomputed, never served.
- C3. A cached verification receipt still fails the resolver when the underlying record is not verified.

## Open questions

1. One evaluator or two. `fold` is a static function tied to the checker context (`osc_check.c:623`), but array lengths and loop bounds are read by the parser (`osc_parse.c:118, 844, 876, 908, 931`). Either move folding to a shared file that both use, or let the parser keep a small constant table and rely on test A7 to keep the two in step. This needs a decision from whoever owns the OSC front end.
2. Surface syntax: reuse `const` (reserved, `osc_lex.c:26`) or a new word.
3. Whether Tier B is wanted at all. It needs a named program that gains from it (candidate use 5), and a pipeline change so results can feed more than values.
4. The default fuel number. 1,000,000 is a placeholder until B5 is measured.
5. The mapping from an `OscUnit` digest to an Omega semantic ID. No code defines it today.
6. A finding outside this record's scope, since confirmed and fixed: `omega_realize_compute_triple_id` used to write the code bytes only if they fit in its 4096-byte buffer after a 78-byte header, and otherwise returned an id that omitted them without any error, so for a realization over about 4018 bytes the triple id did not commit to the code. Issue #429, fixed in PR #430: the code bytes are now streamed into the hash in full and a length past capacity is refused; gate `OMEGA_REALIZATION_ID_COVERS_ALL_CODE_PASS` guards it.
7. What `compiler_id` should be (binary digest, source-tree digest, or crumb digest), and who sets the cache thresholds.
