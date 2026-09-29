# Evidence: capability authority, Rust crate vs C port (historical, black box)

Repo: aien-dev/aienos, clone at /home/drakestapleton/.claude/jobs/cd904311/tmp/aienos-h, main = 603c91d8ec2e6b1d7e2db25591275662eb5c92f0 (`git rev-parse HEAD`).
Host: this Spark (aarch64), cargo/rustc 1.98.1, cc = gcc. Nothing was written, edited or added to the repo; only existing entry points were run.
Compared units: Rust = `crates/aienos-capability` (core, no_std) + `crates/aienos-capability-ffi` (C binding). C = `native/capability/aienos_capability.{c,h}` (authority + binding in one file). The C `aienos_contain.*` gate (ARGUS-1 lane G, added later) has no Rust twin and is excluded.

## 1. Build time (clean, wall clock, min of 3)
| Language | Command (per run, after clean) | Runs (s) | Min |
|---|---|---|---|
| C | `make -C native/capability clean; /usr/bin/time -f %e make -C native/capability out/libaienos_capability.a` (-O2 -Wall -Wextra -Werror) | 0.12, 0.12, 0.13 | 0.12 s |
| Rust | `rm -rf target; /usr/bin/time -f %e cargo build --offline --release -p aienos-capability-ffi` | 0.30, 0.27, 0.24 | 0.24 s |

Rust builds offline (vendor/ + .cargo/config.toml); nothing downloaded. Release profile only; debug builds not measured.

## 2. Artifact size
| Item | Command | Result |
|---|---|---|
| C object, text/data/bss | `size native/capability/out/aienos_capability.o` | 9145 / 0 / 8 |
| C archive | `ls -l native/capability/out/libaienos_capability.a` | 17616 bytes |
| Rust object files of the two crates, text | `size target/release/libaienos_capability_ffi.a` (rows for aienos_capability_ffi cgu.0 = 8821, aienos_capability cgu.0 = 4392, ffi other = 140) | 13353 text total |
| Rust whole staticlib (includes std) | `ls -l target/release/libaienos_capability_ffi.a`; `size -t` | 23637394 bytes; text 985814 |

The staticlib size is dominated by the bundled Rust std, not the capability code.

## 3. Source size and unsafe/pointer sites
| Metric | Command | Result |
|---|---|---|
| C implementation lines | `wc -l native/capability/aienos_capability.c native/capability/aienos_capability.h` | 676 + 188 header |
| Rust core lines | `wc -l crates/aienos-capability/src/lib.rs` (unit tests start at line 690, `grep -n cfg(test)`) | 1001 total, 689 before test module |
| Rust FFI lines | `wc -l crates/aienos-capability-ffi/src/lib.rs` | 469 |
| Rust `unsafe` | `grep -cw unsafe` on core / ffi | core 0; ffi 37 (`unsafe {` 20; `unsafe fn/impl/extern` 17) |
| C explicit pointer casts | `grep -cE '\([A-Za-z_0-9 ]+ ?\*+\)'` and `grep -cE 'uintptr_t\|intptr_t'` | 0 and 0 |
| C raw pointer arithmetic | `grep -nE '\*\s*\(\s*\w+\s*[+-]\|\b\w+\s*\+\s*\w+\s*\)\s*(->\|\[)\|ptr\s*[+-]'` | 0 matches |
| C heap use | `grep -nE 'calloc\|malloc\|free\('` | 3 calloc in start, matching frees on failure/stop paths |

Grep patterns are heuristics; a manual audit was not done.

## 4. Existing tests
| Language | Command | Result |
|---|---|---|
| Rust core | `cargo test --offline -p aienos-capability` | 7 tests, 7 passed, 0 failed |
| Rust ffi | `cargo test --offline -p aienos-capability-ffi` | 0 tests |
| C | `make -C native/capability clean; make -C native/capability test` | capability_test: 571 checks, 0 failures (seed 24301); contain_test (gate, not comparable): 664 checks, 0 failures |

The PR #156 text reported 336 checks at merge; the file has grown since (later commits). Count units differ (Rust #[test] functions vs C individual checks); they are not comparable as coverage.

## 5. Runtime
No existing benchmark or test binary exercises both at similar work (`grep -rln bench native crates/aienos-capability*`: only `native/argus/tests/bench_argus_*.c`, which are C-only ARGUS benches). Not measured; no benchmark was built.

## 6. Defect history (git log on native/capability, crates/aienos-capability*; PR #156, #157)
| Commit | Date | What |
|---|---|---|
| 29125de | 2026-09-27/28 | PR #156, C port. Message: 336 checks; 200 seeds x 5000 ops differential vs Rust, identical on every step (= 1,000,000 ops) |
| b270dad | 2026-09-27 | (PR #157) build output native/capability/out/ (aarch64 object) committed by mistake in #156; x86 consumers linked the stale archive. Packaging bug, not a logic bug |
| b1fa747 | 2026-09-28 | logic bug in the C library: after a restart a reference minted before a reclaim could validate again (generation was old+1 after both restart and reclaim); fixed with 64-bit generations, restart start above all old generations. Only C files changed; the Rust crate got no matching commit |
| 187ecdf, 12add16 | 2026-09-27/28 | feature commits (promotion right; observer callback), not defects. 12add16 fixes an ARGUS integration gap (mint/revoke by non-AEGIS callers unseen), which is a missing feature |

Did the differential test find bugs? The PR states no divergence in the final run. The harness is not in the repo (`git grep -n differential -- native/capability` finds nothing but a comment in aienos_capability.c; no Rust or C differential file exists), so the 1M-op claim could not be re-run and is recorded as reported, not measured. The b1fa747 bug survived that differential run, and whether the Rust crate has the same flaw was not tested.

## 7. Dependency burden
| Language | Command | Result |
|---|---|---|
| Rust core | `cargo tree --offline -p aienos-capability` | 0 dependencies (no_std, uses core::sync::atomic) |
| Rust ffi | `cargo tree --offline -p aienos-capability-ffi` | 1 (aienos-capability); uses std (fs, io, sync) |
| C | `grep -n '#include'` | pthread.h stdatomic.h stdbool.h stdio.h stdlib.h string.h time.h + own header (which includes stdint.h); hosted libc + pthreads |

## Task family
Task family: capability authority (ownership-heavy systems state). C: 676 lines (+188 header), 9145 bytes text, 0.12 s clean build, 0 pointer casts and 0 pointer-arithmetic sites by grep, 571 checks pass, 3 calloc sites, one post-port logic bug (b1fa747) and one packaging bug (b270dad). Rust: 689 lines core + 469 ffi, 13353 bytes text for the two crates, 0.24 s clean release build, 0 unsafe in core and 37 in the ffi binding, 7 unit tests pass, 0 dependencies. Result: at this size both builds are sub-second, the C object is smaller in text, unsafe is confined to the Rust binding layer; the reported 1M-op differential equivalence could not be re-verified from the repo, and the C port had a real restart/reclaim bug found after the differential run.

## Limits
- One small module, one author pair (AI-assisted); not a general language comparison.
- Rust and C compared through different boundaries: Rust needs core + ffi crates for what C does in one file; line counts include comments/blank lines.
- Release Rust only; no debug build, no LTO/size tuning; C at the Makefile's -O2.
- No runtime benchmark exists; none measured.
- Differential harness absent from repo; 1M-op figure is from the PR text.
- Grep counts are heuristics. Build times are warm-cache (vendored deps, no download).
