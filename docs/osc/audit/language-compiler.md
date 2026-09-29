> Appendix to OMEGA_SYSTEMS_CORE_CODE_AUDIT.md. Worker report (Opus, read-only), omega 193a7e7. Reviewed: spot-checked omega_program.c:276 raw-struct hash, aarch64_encoder.c:114,122 silent immediate truncation, absence of ADDS/BL/STP (confirmed). Correction: a SUBS-immediate emitter exists (aarch64_encoder.c:158); the report says there is no ADD/SUB immediate. Not authoritative on its own; Part II of the main document rules.

# Omega language / IR / encoder / identity audit (OSC-0 / OSC-0B input)

Repo: omega worktree `osc-0` at 193a7e7. Read-only audit plus one throwaway encoder probe
(`/home/drakestapleton/.claude/jobs/9d0b86f6/tmp/probe.c`). Other docs: aienos ADR 0012 read live
from GitHub, aienos main `d39dd5b`; local aien-architecture at `873025c` on branch
`feat/resident-semantic-store-boundary` (may lag origin). The Seed/Forge ADR 0012 lives in
**aienos**, not aien-architecture (aien-architecture's 0012 is "shell-dispatch-then-landing").

Labels: [FACT] code fact, [SETTLED] decided in a doc, [PROPOSED] recommended OSC-0B decision, [OPEN] unresolved.

---

## 1. The Omega language as implemented (V0)

[FACT] Code: `src/language/omega_lex.[ch]` (197 lines), `omega_parse.[ch]` (429), `omega_lower.[ch]` (487).
Test: `tests/language/test_language_v0.c` + `tests/language/golden/v0.txt` (27 golden rows).
[FACT] The spec `spec/omega-language-v0.md` matches the code closely (the EBNF there is accurate).

### Grammar the parser actually accepts (one statement per LINE)
```
line      = [ stmt ]
stmt      = "let" NAME ":" type "=" expr
          | "fn" NAME "(" NAME ":" type ")" "->" type ["requires" clause] ["ensures" clause] "{" expr "}"
          | expr [ ":" type ]
expr      = or [cmp_op or]         -- comparisons PARSE but lowering rejects them (-2)
or        = and { "|" and } ; and = add { "&" add } ; add = mul { ("+"|"-") mul } ; mul = prim { ("*"|"/") prim }
prim      = INT | "true" | "false" | NAME | "(" expr [":" type] ")"
type      = u8 | u16 | u32 | u64 | bool          (i8..i64 recognised, rejected -2)
INT       = decimal | 0x.. | 0b..  (<= 64 bits, decoded in lexer)
comments  = // ... | /* ... */
```
- [FACT] Keywords (`omega_lex.c:96-102`): `let fn requires ensures true false`. Tokens: `( ) { } : , -> = + - * / & | == != < <= > >= !`. Strings, chars, `[ ]` rejected in the lexer.
- [FACT] Reserved (rejected as names/values, -2), `omega_parse.c:44-56`: types `u8..u64 bool i8..i64`, `result`, `effect effects cap capability mint revoke grant publish promote authority admin syscall io print import extern unsafe asm`, `if else while for loop return match struct enum mut var const self rec`. This is a pre-planned keyword set for growth.
- [FACT] `fn`: exactly one param, must be `u64 -> u64` after lowering; body must be a constant-step chain `((x op c1) op c2)...` with op in `+ - * & |`, constants <= 0xFFFFFFFF; no calls, no recursion. Contract clauses are canonicalised TEXT only (never checked).
- [FACT] Arithmetic lowers with `OVERFLOW_WRAP` only.

Real example that parses and lowers (per spec + golden file):
```
let x: u64 = 7
x * 2 + 1
fn f(x: u64) -> u64 requires x < 100 ensures result > x { x * 2 + 1 }
```

### Spec/wished features NOT implemented
[FACT] No evaluator in the language; no comparisons/bool ops in lowering; no signed ints; no unary minus / `!`; no calls; no multi-param or non-u64 functions; no blocks, statements sequences, control flow, loops, mutation, structs/enums, slices, references, pointers, arrays, strings; no effects/authority; contracts not verified; programs not verified (`is_verified=false`). Multi-line source is not supported at all.

### Data structures and memory
- [FACT] Lexer: `OmegaToken{kind,col,value,text[64]}`, caller-provided array, max 256 tokens, line <= 1024 bytes (`omega_lex.h`).
- [FACT] Parser: `OmegaAst` holds a fixed in-struct arena `nodes[128]` (index links `lhs/rhs`, no pointers), depth <= 32, precedence-climbing loops. **No heap allocation anywhere in `src/language/`** (libc used: `snprintf memcpy memset strcmp strlen strstr memcmp` only).
- [FACT] AST is declared DISPOSABLE: never hashed or stored (`omega_parse.h` header, spec "Purpose").
- [FACT] Lowering goes into the existing `OmegaGraph` (fixed 256 objects) via `omega_build_*`, dedupes by SemanticId, and restores `object_count` on any error (transactional).
- [FACT] Error model: return codes -1 syntax / -2 unsupported / -3 capacity, message "column N: ...".

## 2. Other IR / compiler machinery

| Thing | File | What it really does | Reusable as typed AST / verified IR target? |
|---|---|---|---|
| Semantic object graph | `src/omega_core.c`, `omega_types.h`, `omega_canonical.c` | Content-addressed DAG of VALUE/TYPE/OPERATION/APPLY objects; ops ADD..COMPILE incl. SELECT, SLICE, EQUAL, LESS_THAN; overflow policy WRAP/SATURATE/FAIL_CLOSED in op payload | Good pure-expression IR; has type tags for SIGNED_INT, SEQUENCE, TUPLE, ADDRESS, RESOURCE, CAPABILITY_REF already. No statements, locals, control flow, memory. Fixed 256 objects. |
| Program model | `src/omega_program.c/.h` | `OmegaProgram` = name + contract text + cost + one `RealizationObject`; `build_unary_op` emits MOVZ/MOVK + one ALU op + RET; `compose` concatenates (drops RET). u64->u64 only | Not a general IR. `program_id` hashes name/contract/cost bytes, NOT code (spec admits). |
| Realize (M5) | `src/omega_realize.c` | Emits fixed patterns for `f_add_sub` and single pure binary ops | Pattern emitter only. |
| realize_synth | `src/omega_realize_synth.c:34-50` | **Ignores the program**: always emits `MOVZ x1,3; MUL; MOVZ 2; SUB` (x*3-2), two schedules by issue width | Fixed-output stand-in (same class as self-host). |
| Self-host (M6) | `src/omega_self_host.c` | **Confirmed fixed-output stand-in**: emitted "compiler" checks magic "OMGG", dispatches on object COUNT byte (8 -> writes 3 hard-coded words ADD/SUB/RET; 5 -> ADR self + copy-loop of its own instructions = quine). | Not a compiler. Its two-pass label scheme (`SET_LABEL`, `REL_IMM19`) is the only fixup machinery in repo. |
| Action graph IR | `src/runtime/rx_graph.[ch]`, spec `action-graph-ir.md` | Typed goal->node graph (nodes, data/guard/order edges, auth[], res[], effects[] in total order), validated, optimised, lowered onto resident reactions | Runtime orchestration IR, not a language IR. Its identity/effect-order discipline is a good model. |
| Plan IR | `src/runtime/rx_plan.[ch]` | Plan templates + verified plan cache; semantic_id over sorted predicate lists | Same: orchestration, not codegen. |
| State projection | `rx_cortex`, `rx_projection` | need -> minimal projection of Cortex store | Not relevant to codegen. |
| Visor | `src/visor/*` | `./omega` REPL session: graph, bindings, programs table (16), realization/verify/evidence commands; hosts language V0 | The front door the new language should plug into. |
| Flow IR | none | [FACT] Nothing named Flow IR. "flow" appears only as prose ("control flow", "flow control", `plan-reuse.md` §4 "Flow"). | Name is free. |

## 3. Identity and determinism (OSC-0B items 3, 6)

### How identity is computed
- [FACT] SemanticId = SHA-256(canonical encoding) (`omega_canonical.c`), header "OMG0", version, kind, attrs sorted by key, relations sorted, constraints sorted, then `payload_len` + payload.
- [FACT] **Payload is `memcpy` of host C structs** (`omega_core.c:84,96,109,122,135,148,162,174,188,212,232,247,262`), each `memset` to 0 first, so padding is zero and output is deterministic ON ONE ABI, but NOT layout/endianness independent: enums are native 4-byte, `uint16_t byte_len` etc. little-endian. Golden row for `7: u64` shows `...0800 00..07` = byte_len 8 stored LE, value bytes BE. [SETTLED] `omega-language-v0.md` "Golden vectors": "golden bytes are pinned to this ABI (aarch64/x86-64 LP64, 4-byte enums)".
- [FACT] `omega_program_compute_id` (`omega_program.c:41-67`) builds an explicit byte buffer (layout-independent) but omits code.
- [FACT] `omega_task_init` hashes the raw `SynthesisTask` struct: `sha256_hash(task, sizeof(SynthesisTask) - 32, ...)` (`omega_program.c:276`). `task_id` is the FIRST field, so this hashes the zeroed id and **drops the last 32 bytes (expected_outputs[12..15])** - a latent bug, plus ABI-bound.
- [FACT] Runtime IRs do it right: `rx_graph.c:20-30` `put32/put64` explicit big-endian into SHA; `rx_plan` uses `rx_plan_encode` + sort. Layout-independent.
- [FACT] Realization id: `omega_realize_compute_triple_id` = SHA(OMG0 | kind | profile | semantic id | machine id | code bytes).

### Nondeterminism
- [FACT] No `__DATE__/__TIME__` in src. Receipts embed wall time + `git rev-parse` (`omega_evidence.c:41-45`) by design. `omega_library.c:193` stores `time(NULL)` in `timestamp_added` but the library digest (`:60-82`) excludes it. `clock_gettime` only in benchmarks/submit timing.
- [FACT] Build flags (`Makefile:1,8`, `mk/00-visor-core.mk:8`): `gcc -std=gnu11 -O2 -Wall -Wextra -Werror -D_GNU_SOURCE`; no `-ffile-prefix-map`, no `SOURCE_DATE_EPOCH`, no reproducible-build check of Omega's own binaries.
- [FACT] "C1 == C2 == C3" (`tests/run_m6_gates.sh:78-83`, `tools/omegatool.c:813-823`, `omega_self_host.c` bootstrap): the memcmp is real, but C2 is produced by C1's self-copy loop, so equality holds **by construction**; it proves the copy loop and determinism of C0's emitter, not compilation. The receipt also hard-codes `COMPILER_REALIZATION_ID` as a literal (`run_m6_gates.sh` ~line 57) and writes `"fixed_point_verified": true` as text (guarded only by `set -e` on earlier steps).

### Live handle vs persistent identity (item 6)
- [FACT] Distinct types exist, not unified:
  - `SemanticId{uint8_t[32]}` - content identity (`omega_types.h:25`).
  - `OmegaHandle{world_epoch, object_id, object_generation(u32), object_type, permissions}` - live accelerator handle (`omega_accelerator_world.h:44-51`).
  - `RxCapRef{cap_id, generation(u64)}` (`rx_caproot.h:117`), `AienosCapRef{cap_id, generation u64}` (`aienos_cap.h:21`) - live capability refs.
  - `EffectPayload{capability_slot u32, capability_generation u32, capability_ref SemanticId}` (`omega_types.h:160-167`) - a live slot/generation **embedded in a hashed payload**, i.e. a live handle does enter a SemanticId today.
- [FACT] No type-level rule stops a live handle being serialised as durable identity.

## 4. Machine-code emitters

[FACT] `src/aarch64_encoder.c` (168 lines) is the only AArch64 encoder. Exact emittable instructions:
ADD/SUB/AND/ORR/EOR (shifted register, no shift), MUL (MADD with XZR), MOV (ORR XZR), MOVZ, MOVK (any hw), RET, B, B.cond, CBZ, CBNZ, ADR, LDR/STR unsigned-offset (W/X), LDRB/STRB unsigned-offset, LDR/STR post-index (W and X), SUBS immediate, SUBS register (so CMP via rd=XZR).
Users: `omega_matvec.c`, `omega_matvec_quad.c` (quad4 / M12 kinds), `omega_realize*.c`, `omega_program.c`, `omega_self_host.c`. (The Blackwell files emit GPU code, not AArch64.)

Probe (compiled against the real encoder, all matched the ARM reference words):
```
ADD X0,X0,X1 8B010000 OK | SUB X0,X0,X2 CB020000 OK | MUL X0,X0,X1 9B017C00 OK
MOVZ X0,#1 D2800020 OK | MOVK X0,#1,LSL16 F2A00020 OK | MOVZ LSL48 D2E00020 OK
LDR X0,[X1,#8] F9400420 OK | STR X0,[X1,#8] F9000420 OK
CMP X0,#1 F100041F OK | CMP X0,X1 EB01001F OK | B.EQ +2 54000040 OK | B.VS +2 54000046 OK
CBZ X0,+2 B4000040 OK | B -1 17FFFFFF OK | RET D65F03C0 OK | buffer-full -> rc=-1 OK
ADD rd=31 -> 8B0003FF (XZR, not SP)
LDR X0,[X1,#12] -> F9400420 == LDR [X1,#8]   <-- SILENT MISENCODING (no alignment check)
```

| Capability | Yes/No | Evidence |
|---|---|---|
| LDR/STR immediate offset | YES (unsigned, scaled) | `aarch64_encoder.c:112-126`; misaligned offsets silently truncated; no range check (masked to 12 bits) |
| LDP/STP | NO | not in encoder or decoder |
| Register ALU ops | YES: ADD SUB AND ORR EOR MUL | `:13-61` |
| MOVZ/MOVK | YES | `:67-102` |
| CMP + B.cond / CBZ | YES (CMP = SUBS to XZR) | `:83-96,158-168` |
| ADDS + B.VS | NO ADDS (B.VS encodes; SUBS exists, so sub-overflow check is possible) | no adds emitter |
| Mul overflow (SMULH/UMULH) | NO | - |
| SDIV/UDIV | NO | - |
| BL / BLR | NO (RET only) | - |
| Stack frame (SUB SP / STP FP,LR) | NO: `REG_SP == REG_XZR == 31` (`aarch64_target.h:23-24`); shifted-register ADD/SUB cannot address SP; no ADD/SUB immediate; LDR/STR with rn=31 would address SP but SP can't be adjusted | probe |
| Branch range checks | NO (imm masked) | `:78-96` |
| Labels / fixups | only ad hoc two-pass in `omega_self_host.c` | - |

Execution: [FACT] `omega_exec_native_f3` (`omega_exec.c:8-40`) mmap RW, memcpy, `__builtin___clear_cache`, mprotect RX, call as `uint64_t f(uint64_t,uint64_t,uint64_t)`, munmap. Same pattern inline in `omega_realize_synth.c` (~`:150`) and matvec. No ELF writer anywhere. Signature fixed to 3 u64 args.
[FACT] Verification gate: `omega_verify_v0_structural` calls `aarch64_validate_code_buffer` (`omega_verify.c:62`), which rejects any opcode not in the decoder allowlist (`aarch64_decoder.c`, "Undefined/unrecognized AArch64 opcode") and requires a final RET. **Every new emitter must be mirrored in the decoder**, and "last instruction must be RET" breaks with multi-function code or trap stubs after RET.

Effort estimate for a tiny language backend (functions, stack locals, checked i64 arithmetic, bounds checks, calls to runtime alloc/free):
- Encoder + decoder additions (~15 forms: ADD/SUB immediate incl. SP form, ADDS/SUBS reg, SMULH, SDIV, STP/LDP pre/post/offset, BL, BLR, LDR/STR register-offset with LSL, CSET, BRK/UDF trap, plus alignment/range checks) with probe tests against known words: ~1-2 worker-days.
- Label/fixup assembler, frame layout, simple stack-slot codegen (no register allocator), trap-on-overflow/bounds via B.VS/B.HS to a trap block, BLR to C runtime alloc/free via MOVZ/MOVK absolute address, multi-function blob + entry table, exec harness generalised beyond f3: ~3-5 worker-days.
- Decoder "final insn must be RET" rule needs relaxing to a function table.

## 5. Verification machinery to reuse / stay consistent with
- [FACT] `omega_validate_graph/object` (`omega_validate.h`): typing + DAG + reference resolution.
- [FACT] `omega_verify` tiers: V0 structural (graph typing + decoder allowlist + RET), V1 differential (native result vs reference `omega_eval` on input triples) - the existing oracle model, V2 fixed properties (commutativity, identity, WRAP overflow, width range).
- [FACT] Typed result constraints `rx_contract.[ch]`: `rc_check/rc_compile/rc_domain/rc_gate_register` - publish gate that refuses non-conforming results.
- [FACT] Capability checks: rx_world `validate_caps` twice per activation, generations + `RX_ERR_STALE_GEN`; action graph `auth[]`; `rx_graph.o` link check forbids `aienos_cap_*` admin symbols (Omega never mints) [SETTLED `action-graph-ir.md` Boundaries].
- [FACT] Tests: shell gate runners `tests/run_m*_gates.sh`, canonicalization/identity/malformed shell suites, language golden vectors, hostile tests (`tests/visor/test_visor_authority.c`), runtime tests. **No fuzzer or property generator found** (no PRNG-driven test harness in language/core).

## 6. Lifetime vs authority vs placement today (item 12)
- [FACT] Surface language: none of the three is expressible; the words are reserved.
- [FACT] IR: authority = `EffectPayload` cap slot/generation + `CAPABILITY_REF` type tag + action-graph `auth[]`; lifetime = object/capability generations and stale-generation refusal (runtime only), no static lifetime; placement = `RealizationObject.target_profile` + machine graph + triple realization id (semantic x machine x code).

## 7. libc the substrate must replace (item 10)
- [FACT] Core semantic path (core, canonical, sha256, program, language): `memcpy memset memcmp strcmp strlen strstr snprintf` + one `calloc` (`omega_core.c:8`, graph allocation). SHA-256 is in-house (`src/sha256.c`).
- [FACT] Encoder/exec: `mmap mprotect munmap sysconf __builtin___clear_cache`.
- [FACT] Everything else (printf/fprintf/fopen/popen/getenv/time/strftime/qsort(1, `visor_evidence.c:477`)/malloc) is in receipts, gates, console, Blackwell/GPU code (36 mallocs in `omega_blackwell_gates.c`). `<math.h>`/`<pthread.h>` only in Blackwell and world gates, not the language path.

## Contradictions (doc vs code)
1. `README.md:19` "OMEGA is not a programming language, compiler..." vs `spec/omega-language-v0.md` (a language exists) and `spec/self-host.md` ("self-hosting compiler").
2. `spec/canonical-encoding.md` §2 rule 4 "payload always big-endian" vs `omega_core.c` memcpy of host LE structs; `omega-language-v0.md` admits the ABI pin.
3. `spec/type-system.md:35` CAPABILITY_REF `generation: u32` / `EffectPayload.capability_generation u32` vs runtime `RxCapRef/AienosCapRef generation u64` (also recorded in commit 8a7d95e).
4. `spec/self-host.md` "compiles itself" vs `omega_self_host.c` count-dispatch + self-copy quine.
5. `spec/realize_synth.md` machine-aware synthesis from a program vs `omega_realize_synth.c:34-50` fixed x*3-2 code.
6. `spec/program.md:21` "verified native code" vs language lowering leaving `is_verified=false` and `program_id` not covering code.

## Recommended OSC-0B decisions (all [PROPOSED])
1. **Syntax: extend V0, don't replace it.** Keep its lexer rules, literal typing rule (no guessed widths), precedence, `fn ... requires/ensures`, and reserved list; activate reserved words (`i8..i64`, `mut`, `struct`, `if/else/while/return`, `unsafe`) rather than inventing new ones. Add multi-line source with a per-function arena (lift the 1-line/256-token/128-node limits).
2. **Typed AST stays disposable** (as V0 settled); identity attaches to the lowered IR (call it Flow IR; name is free), never to syntax.
3. **Wire layout rule:** every hashed/serialised byte comes from an explicit big-endian writer (the `rx_graph put32/put64` pattern). No `memcpy` of structs into identity. Migrating the core payloads is a deliberate identity break (re-generate golden vectors once, with a version bump in the "OMG0" header), and fix the `SynthesisTask` hash range bug.
4. **Handle rule:** separate types for durable identity (`SemanticId`) vs live handles (`{id, generation}` refs); live handles are forbidden in any canonical encoder (checker rejects the type in hashed payloads). Migrate `EffectPayload` to reference capabilities by SemanticId only, and settle generation width at u64 in the spec.
5. **Checked arithmetic default:** new signed ints lower with `OVERFLOW_FAIL_CLOSED` (already an enum value) - trap, not wrap; V0 unsigned WRAP stays for back-compat.
6. **Backend for the slice: both, with roles.** (a) Direct AArch64 via the extended in-repo encoder is the real backend (matches founding principle + no-outside-deps; the encoder is correct where it exists, per probe). (b) Emit C compiled by system `cc` only as a **differential oracle** in the V1 style (same program, two backends, compare results + traps), never shipped. Pros (a): sovereign, reuses exec/verify; cons: 4-7 worker-days, needs decoder mirror + fixups + SP support. Pros (b): fast, catches codegen bugs; cons: depends on gcc semantics (UB) - acceptable only as an oracle since gcc already builds Omega.
7. **Encoder hardening first:** reject misaligned/out-of-range offsets and branch displacements (currently silently masked), distinguish SP from XZR in the API.
8. **Reproducible build check:** add `-ffile-prefix-map` and a build-twice-compare gate for the new compiler; do not reuse the M6 C1==C2==C3 claim as evidence of compilation.
9. [OPEN] Whether contracts (`requires/ensures`) become checked in the slice or stay text.
10. [OPEN] Whether the new IR lives inside `OmegaGraph` (256-object fixed cap) or a new store.
