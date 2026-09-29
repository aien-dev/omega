# POLYGLOT-0 independent adversarial review (lane G)

Reviewer: worker G (Opus 5.5), did not author any reviewed code.
Reviewed: branch `feat/polyglot-0` at 17d2854 (worktree `~/workspace/omega-polyglot-review`, branch `polyglot/review`).
Scope: `spec/polyglot-0.md`, `docs/polyglot/POLYGLOT_AGENT_OWNERSHIP.md`, `mk/polyglot*.mk`, `src/polyglot/**`,
`polyglot/mojo/omx_mojo.mojo`, `tests/polyglot/**`.

What was run (correctness only, plus one smoke bench; no full timed bench):

- `build/polyglot/verify_polyglot` (plain, 2,000 and 20,000 random) and `_asan`: PASS, 18 candidates.
- `make test-polyglot-asm`, `test-polyglot-encoder`, `test-polyglot-mojo`: PASS (encoder byte compare 356/356 identical).
- `make bench-polyglot-smoke` (`.spark-quiet` absent, load 1.0): 72 receipts in `build/polyglot/smoke`.
- `polyglot_explain` on the smoke receipts, twice (byte-identical), plus tampered copies (below).
- 10 mutants (5 assembly edits in this worktree, restored after each run; 5 C mutant candidates in a scratch lane
  file linked in place of the Mojo lane) and one callee-saved-register sentinel harness (appendix).

Headline: the shipped kernels are correct on everything I threw at them, and they preserve callee-saved registers.
The weak spots are the gate machinery: the bench breaks its own 1 ms rule on S1, two receipt fields are wrong
for Mojo and the encoder, and the shared verifier misses five of my ten mutants.

## Mutant results

| id | defect injected | verify plain | verify ASan | lane test | bench would catch |
|---|---|---|---|---|---|
| A | `asm_crumb` reads 16 B past the end of the packed weights (`ldr q5,[x5]` at `.Lcr_ok`) | **missed** | **missed** | caught (test-polyglot-asm segfault) | no |
| B | `asm_sdot` tail mask off by one lane | caught (41,376 fails) | caught | n/a | yes |
| C | `asm_sdot` zeroes d8 and d15 (callee-saved) | **missed** | **missed** | **missed** (test-polyglot-asm PASS) | no |
| D | `asm_sdot` zeroes x27, x28 | caught (crash) | caught (crash) | caught (crash) | n/a |
| E | `asm_sdot` reads x[n..n+15] | caught (guard fault) | caught | n/a | n/a |
| M1 | `-1 * x` computed as int8 negation (wrong only at x = -128) | caught (32,760 fails) | | | |
| M2 | weight column offset kept in 16 bits (wraps at n > 65536) | **missed** | | | **no** (n <= 4096) |
| M3 | rows with index >= 301 off by one | **missed** | | | yes (S3, m = 4096) |
| M4 | memoises the last two results keyed on (plan, x pointer, m, n, first 16 bytes of x) | **missed** | | | **no**, and would win S1-S3 |
| M5 | vectorised validator: only the last (m*n) % 16 weights really validated | **missed** | | | no |

Commands: `/tmp/pgrev/mut.sh NAME FILE 'perl-subst' [asm]` (applies the edit, builds into
`build-mut/NAME`, runs both verifiers and optionally `test-polyglot-asm`, then `git checkout` the file).
C mutants: `/tmp/pgrev/mut_lane.c` defines a strong `omx_lane_mojo[]`; built with
`cc -std=gnu11 -O2 -march=armv8.6-a+dotprod+i8mm+sve -Isrc -o verify_mut tests/polyglot/verify_polyglot.c
src/polyglot/omx_lang.c src/polyglot/omx_bench.c src/sha256.c mut_lane.c build/polyglot/o2/*.o -lm`.
The mutant bodies are quoted in the findings below so they can be recreated without `/tmp`.

## BLOCKING

### G-B1. Timed samples on S1 are shorter than the 1 ms the spec requires
`tests/polyglot/bench_polyglot.c:647-655` (calibration) and `:670-686` (sampling). Spec section 7, gate 10.5.

Calibration times ONE call with two `clock_gettime` reads around it, so for S1 (about 35 ns per call) the timer
overhead dominates: `one` came out as 64 ns for R1_sdot, giving reps = ceil(1.25e6 / 64) = 19,532 and a real sample
of 19,532 x 36.3 ns = 0.71 ms. Nothing checks the sample length afterwards.
Smoke run evidence (`calls_per_sample * latency median` from the receipts): 13 of 18 S1 cells are below 1 ms,
from 0.709 ms (R1_sdot, R1_sdot@O3) to 0.991 ms (R1_smmla). S2-S4 are fine.
Fix: calibrate with a doubling loop of `sample()` until one sample is >= 1 ms, and reject or re-take any sample with
`t < 1e6` (count it in the receipt).

### G-B2. `code_size_bytes` is wrong for Mojo and the own encoder; encoder compile time measures the wrong thing
`tests/polyglot/bench_polyglot.c:342-343, 403-406`; `src/polyglot/omx_mojo.c:48-80`; `src/polyglot/omx_encoder.c:671-679`.
Spec section 7 ("code size of the realization's text section").

The rule "run + pack function symbol sizes" resolves `im->run` to the C adapter, not the realization:
MJ1_sdot/MJ2c_crumb report `run_i8`/`run_crumb` = 72 B (the Mojo kernels `omx_mj_i8_run` etc. are not counted);
enc_sdot/enc_crumb report `enc_*_run` = 112 B (a thunk; the 156 / 200 emitted instructions = 624 / 800 B are not
counted). asm_sdot correctly reports 624 B. A ledger built from these receipts would say Mojo's run code is 9x
smaller than hand assembly. Encoder `compile_time_s_min_of_5` (0.189 s) is the gcc compile of
`omx_encoder.c`, not the time Omega's encoder takes to emit the kernel at run time.
Fix: let each lane name its realization symbols (or byte count) in `omx_candidate`; for the encoder use
`omx_encoder_code(k, &bytes)` and time `omx_encoder_build`.

## SHOULD-FIX

### G-S1. Shared verifier never guards the packed weights; non-C kernels are not ASan-instrumented
`tests/polyglot/verify_polyglot.c:187-201`. Mutant A.
Guard pages surround the canonical W, but run kernels read `p->mem` (heap, from `oma_rz_alloc`, rounded to 64 B).
ASan does not see assembly, JIT code or the Mojo object. Mutant A (16-byte over-read past the packed buffer) PASSES
both verifier builds. B1 and B2's own tests move `p->mem` against a guard page and catch it; the Mojo lane test has no
guard pages at all, so a Mojo packed-weight over-read is caught by nothing. Fix: in `run_case`, copy `p.mem` into a
guarded buffer (flush against the trailing guard, and once flush against the leading one) for every candidate, as
`tests/polyglot/test_asm.c:172-190` does.

### G-S2. Random cases never reach n > 65536 with random data, or m > 300
`tests/polyglot/verify_polyglot.c:341-357`. Mutants M2, M3.
Random n <= 8200 and m <= 300; the only n > 65536 cases (`:345, :347, :348`) use all -1 or all +1 weights with
x = -128, so a 16-bit column wrap reads a different column with the same value and passes (M2:
`a += W[i*n + (uint16_t)j] * x[j]`, 0 failures). M3 (`y[i] += 1` for i >= 301) passes the verifier; only the bench's
S3 check would catch it, without guard pages. Fix: add random-data cases at the four bench shapes (S3 is
m = 4096, n = 4096) and at n in {65537, 131073, 1,000,003} with m in {1, 5}, plus a few with m in 1000-4100.

### G-S3. Error contract tested only on a 2x3 matrix
`tests/polyglot/verify_polyglot.c:232-260`. Mutant M5.
Six weights never reach a 16-byte vector body, so a validator that only checks the scalar tail passes
(M5: loop `for (i = (m*n) & ~15; i < m*n; i++)`). MJ1_sdot's pack is exactly this shape
(`polyglot/mojo/omx_mojo.mojo:67-72`); it is correct today and the Mojo lane test covers it (shapes up to 7x129),
but the shared gate does not. Fix: bad weight at first, last and a random position for shapes like 4x65, 7x129,
3x1000.

### G-S4. No test checks callee-saved registers
Mutant C (d8 and d15 zeroed in `asm_sdot`) passes verify plain, verify ASan and `test-polyglot-asm`. x19-x28
clobbers happen to crash the caller (mutant D), d8-d15 clobbers are silent. Current code is clean: the sentinel
harness in the appendix ran all 18 candidates on 7 shapes (126 calls), 0 clobbers of x19-x28 / d8-d15; the same
harness flags mutant C on all 7 shapes. Fix: add the trampoline to `test_asm.c` and `test_encoder.c`
(or the shared verifier).

### G-S5. The bench cannot detect a memoising candidate
`tests/polyglot/bench_polyglot.c:67-69, 176-190`. Mutant M4.
S1-S3 use one x (`nx = 1`), so every call in a sample has identical inputs and the checksum is the same whether or
not the kernel ran. M4 (returns a cached y when plan, x pointer, m, n and the first 16 bytes of x match) passes the
verifier (the verifier never calls run twice in a row with the same x) and would report near-zero latency on S1-S3.
Our own lanes do not do this; the harness should still make it impossible. Fix: rotate at least 2 x vectors per
workload (S4 already rotates 256), or flip one byte of x between calls and fold it into the expected checksum.

### G-S6. Explainer selects from smoke, N < 20, and leftover receipts of other runs
`tests/polyglot/polyglot_explain.c:225, 316-323, 378-389`.
`gate_eligible` and `smoke` only print a NOTE. Run on the smoke receipts it prints SELECT/CHOSEN lines from N = 3
data. Tamper test: copy the smoke receipts, add `ghost_sdot__S1.json` (asm_sdot with id renamed, run_id
`...-OLDRUN`, median 1.0): the explainer lists both runs and SELECTS `ghost_sdot` for S1 (median 1.0 with min 35.2
is not questioned). This becomes blocking if `evidence/POLYGLOT/` ever holds more than one run. Fix: eligibility
requires `gate_eligible`, `tree_dirty_files == 0`, one `run_id` (or one per workload, refused otherwise), and
`min <= median`.

### G-S7. Noise band too narrow to support "CHOSEN"
`tests/polyglot/polyglot_explain.c:405`.
Band = 3 x max(MAD/median) from within-run samples. On S1 the smoke run gives asm_sdot MAD 0.0 and R1_sdot@O3 a 0.2%
band, so a 3.2% gap is called CHOSEN. Within-run MAD does not include run-to-run, core-to-core or boot-state spread
(see the X925 clock-cap episode). Fix: a floor (for example 2%) or a second full run on a different X925 core that
must agree before CHOSEN is printed; report TIE otherwise.

### G-S8. Own-encoder candidate breaks section 1's "effects: none" and run's error contract
`src/polyglot/omx_encoder.c:620-676`.
The first `run()` call does `mmap` + `mprotect` (global state, a syscall) through `pthread_once`, and if that fails
every later `run()` returns `OMA_RZ_E_NOMEM`, which section 1 lists only for pack. W^X and cache maintenance are
right (RW, write, mprotect RX, then `__builtin___clear_cache` on the code range; DC CVAU on a read-only page is
allowed), and `test_encoder.c` checks the mapping permissions. Fix: build the code in `pack` (or an explicit init
step recorded in the plan), or add the exception to section 1.

### G-S9. Compile time comparability
`tests/polyglot/bench_polyglot.c:212-252`, `mk/polyglot.mk:141-153`.
Compile commands run as children of the pinned bench process, so they inherit one-core affinity: `mojo build`
(multithreaded) is handicapped relative to gcc. C timings compile the whole MA-3 file (4 realizations per object,
noted in the receipt). Record the affinity in `compile_note`, or unpin before timing compiles.

## NOTE

- N1. Contract digest covers section 1 only (`src/polyglot/omx_bench.c:72-95`), as section 8 says. Changing section 7
  (tested: "N >= 20" to "N >= 2") leaves every receipt "not stale". Gate 10.1 says "this contract, unchanged";
  consider hashing sections 1-7.
- N2. `spec/polyglot-0.md:16` says overlap of y with W/x is "checked by the verifier". The verifier never builds an
  overlapping case; it only never creates one. Reword ("never produced by the verifier").
- N3. y is always 4-byte aligned (`verify_polyglot.c:159, 226`); all 16-byte residues are covered. Fine for `int32_t *`,
  but gate 10.3 says "unaligned x and y".
- N4. The size-overflow case accepts `E_NOMEM` as well as `E_OVERFLOW` (`verify_polyglot.c:278-280`); all candidates
  actually return `E_OVERFLOW` via `oma_rz_check_shape`.
- N5. The verifier memsets the plan before pack, so a pack that returns early without clearing `*p` is invisible
  (same as MA-3; the Mojo adapter returns shape errors before its memset, `omx_mojo.c:27-31`).
- N6. Undeclared stack scratch: `asm_crumb` uses 64 B of stack (`omx_crumb.S:73`), Mojo `load_x64` a 64-byte
  temporary; `scratch_bytes` is 0 for both.
- N7. S4 cannot be compute-bound by construction: the GEMV ABI re-reads all of W for every x, so the 256 x only
  rotate. In practice asm_sdot does 64 Ki MAC in 413 ns (about 2.6 SDOT per cycle), close to SDOT throughput, so
  the label holds empirically, not by design.
- N8. Round-robin order is fixed (candidate 0 always first in each round); rotate the start per round.
- N9. The schedstat run-delay test does not see IRQ/softirq time on the pinned core.
- N10. The checksum loop (m adds per call) is inside the timed region for every candidate; at most 4,096 adds per
  about 0.5 ms S3 call, negligible.
- N11. Flags: the -O3 flavor keeps `-march=...` and adds `-mcpu=native` (generic tune on gcc 13.3), recorded.
  Mojo builds for the host CPU (X925 tuning) while C uses generic tuning; the Mojo receipt's `flags` does not
  say so, and its toolchain string "mojo 1.0.0" is hard-coded (`omx_mojo.c:97-98`), not read from `mojo --version`.
- N12. Break-even: the bench uses the cheapest pack over all measured candidates, including the weak baseline and
  toolchain-only ones (`bench_polyglot.c:708`); the explainer uses eligible ones only. The receipt and the
  explanation can disagree. MJ1_sdot packs about 8x cheaper (0.08 ns/weight, vector validation), so it is
  the pack reference in most cells.
- N13. `peak_rss_kib` is the whole bench process with every candidate's plans alive (230 MB in smoke), the same in all
  receipts.
- N14. `rx_costmodel.state_bytes = (m*n + n + m) * 8` (`bench_polyglot.c:462-466`): the factor 8 is unexplained; check
  against `rx_costmodel` before POLYGLOT-1 maps onto it.
- N15. `--workloads` uses `strstr` (`bench_polyglot.c:497`); harmless with S1-S4.

## Rules check (item 5)

- 28 files changed against 4b217aa: no `.py`, no `.rs`, no Cargo files. No GPU code, no `/dev/nvidia*`, no CUDA.
- Runtime reads outside the repo: `/proc` and `/sys` (machine facts, schedstat), `$HOME/workspace/.spark-quiet`
  (by design), `/tmp/polyglot-ctime-*` (compile timing scratch). The bench also spawns `mojo build` from
  `$HOME/.pixi` for compile timing; that toolkit bundles Python 3.14 (already on Drake's flag list). The run
  kernels themselves read nothing outside their arguments.

## Explainer checks passed

- Byte-identical output on two runs over the same receipts.
- A one-character edit to section 1 marks all 72 receipts stale and selects nothing.
- No hard-coded candidate ids or winners in `polyglot_explain.c`; it names the runner-up, margins, pack and
  break-even, per-language best, and a reason for every rejected candidate.
- Exact-median ties break by id; TIE is printed when inside the band.

## Appendix: callee-saved sentinel harness

`cs_call(fn, p, x, y, out)` sets x19-x28 and d8-d15 to known values, calls `fn(p, x, y)`, and stores the registers to
`out[0..17]`. The driver packs each candidate at shapes (1,1) (3,15) (4,16) (5,17) (7,100) (9,1025) (33,4099), calls
through `cs_call`, and compares the registers and y against the oracle.

```asm
	.text
	.globl cs_call
	.type cs_call, %function
cs_call:                        // x0 fn, x1 p, x2 x, x3 y, x4 out
	stp x29, x30, [sp, #-176]!
	mov x29, sp
	stp x19, x20, [sp, #16]
	stp x21, x22, [sp, #32]
	stp x23, x24, [sp, #48]
	stp x25, x26, [sp, #64]
	stp x27, x28, [sp, #80]
	stp d8, d9, [sp, #96]
	stp d10, d11, [sp, #112]
	stp d12, d13, [sp, #128]
	stp d14, d15, [sp, #144]
	str x4, [sp, #160]
	mov x16, x0
	mov x0, x1
	mov x1, x2
	mov x2, x3
	movz x19, #0x1919
	movz x20, #0x2020
	movz x21, #0x2121
	movz x22, #0x2222
	movz x23, #0x2323
	movz x24, #0x2424
	movz x25, #0x2525
	movz x26, #0x2626
	movz x27, #0x2727
	movz x28, #0x2828
	fmov d8, x19
	fmov d9, x20
	fmov d10, x21
	fmov d11, x22
	fmov d12, x23
	fmov d13, x24
	fmov d14, x25
	fmov d15, x26
	blr x16
	ldr x4, [sp, #160]
	stp x19, x20, [x4]
	stp x21, x22, [x4, #16]
	stp x23, x24, [x4, #32]
	stp x25, x26, [x4, #48]
	stp x27, x28, [x4, #64]
	stp d8, d9, [x4, #80]
	stp d10, d11, [x4, #96]
	stp d12, d13, [x4, #112]
	stp d14, d15, [x4, #128]
	ldp x19, x20, [sp, #16]
	ldp x21, x22, [sp, #32]
	ldp x23, x24, [sp, #48]
	ldp x25, x26, [sp, #64]
	ldp x27, x28, [sp, #80]
	ldp d8, d9, [sp, #96]
	ldp d10, d11, [sp, #112]
	ldp d12, d13, [sp, #128]
	ldp d14, d15, [sp, #144]
	ldp x29, x30, [sp], #176
	ret
	.size cs_call, .-cs_call
	.section .note.GNU-stack,"",%progbits
```

Result on 17d2854: `cs_check: 18 candidates, 126 calls, 0 problems (x19-x28, d8-d15)`. With mutant C linked in place of
`omx_sdot.S.o`: 7 problems, all `asm_sdot reg#10` (d8).
