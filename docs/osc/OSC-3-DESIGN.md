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
  10^5 sequences, compiler fuzz 120 per entry plain and 24 under ASan/UBSan,
  legacy writer differential in its sampled mode). Target under 60 s single
  core. CI runs it on every change to the compiler paths. It is a smoke run
  and is never receipt evidence.

The quick wall time is measured inside every OSC-3 receipt
(`test_compiler_quick.wall_seconds`, binaries already built).
