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
| `src/compiler/osc_rt.h:69-70` `slot_serial[]`, `next_serial` (`++next_serial`, no overflow check) | u32 | OSC (this lane) | item 2: handle generations are u64 with retire-at-max; the region serial is widened or traps on wrap (stated in the item 2 section) |
| AIENOS ADR 0013 (`index u32; generation u32`, retire at u32::MAX) | u32 | aienos lane | request only, see item 5 (`docs/osc/OSC-3-ADR0013-REQUEST.md`); not edited from this repository |

Only the `osc_rt` row is inside this lane's files. The others are listed so the
owning lanes can schedule them; each is a versioned change with old records
refused, never reinterpreted. None is applied by OSC-3.

## Slice order after item 4

Per the audit (III.7 and the 2026-10-01 review), the first real migration is
crumbline (`src/crumbline/`) once effects and capabilities exist:
item 4 (effects/capabilities), then item 5 (identity break + ADR 0013
request), then item 6 (crumbline as the first production C module under
contracts; proposal in docs/osc first).
