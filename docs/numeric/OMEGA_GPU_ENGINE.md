# Omega GPU Engine: design (phase A2, reconciled to Drake's first-cut plan)

Status: DRAFT, chip-unproven. The engine core (section 13) and, since cut A3b1,
the Blackwell backend (section 14) exist, and the vector executor
`omega_blackwell_execute_vector` now routes through `omega_gpu_execute`. No
other launcher is converted. Every test and chip label for this work is
NOT_RUN (host NOT_RUN, chip NOT_RUN). No receipt exists today. Editing
`src/omega_blackwell_submit.c` changes the sha256 that the M17 and M18 gates
compute at run time (`tests/run_m17_gates.sh:66-67,129-130`,
`src/omega_blackwell_gates.c:1166-1167`), so receipts for the vector gates can
only be verified on the chip in Phase B, and any PR carrying this change stays
DRAFT until then.

Plan authority: Drake's pasted first-cut plan, items 1 to 11, 2026-10-02
(`~/handoffs/2026-10-02-DEEP2-first-cut-drake.md`), plus the queen rulings in
that file. This document, the header and the stub were reconciled to it in the
second commit of omega#218. Where the first draft differed, the change is
named in section 12.

Sources. Facts below come from two read-only scout reports, cited as A1 and
A1b: `~/handoffs/2026-10-02-DEEP2-A1-scout.md` and
`~/handoffs/2026-10-02-DEEP2-A1b-scout.md`. A1 section names are quoted as
"A1 File N" or "A1 (a)..(f)". Line numbers are those the scouts recorded
(MAIN f633e343 for A1, 2db71da for A1b; PR198 lines are marked "PR198" and
may have drifted because that PR has since moved, A1b section 6). Source
lines quoted as "now" were read directly on this branch (base 2db71da).
Anything neither scout nor this cut verified is labeled UNVERIFIED with a
confidence.

## 1. What it is, and why

The Omega GPU Engine is one small module that takes "here is my already-built
GPU program, its two inputs, and the buffer I expect it to fill" and returns
either SUCCESS or exactly one named failure that says which step broke.

Why: several launcher files repeat the same open, allocate, copy-in,
pushbuffer, submit, wait, readback, close sequence (A1 Files 1 to 5, A1b
section 4: "What NUM duplicates from SUB"). They report errors in three
different styles (the GB10_DEVFAIL macros, the LDST_DEVERR line, and a bare
`-1`, A1 macro section and File 4). The divsqrt second-marker wait fails
silently with no step name (A1 headline 2: divsqrt 1160-1162).
`omega_blackwell_submit.c` has no semaphore wait and no second-marker
protection, "C3" (A1 headline 3 and A1b section 1: instant compare at line
207, no L2 flush, no marker2).

Execution paths found (plan item 1, from the scouts): `omega_blackwell_submit.c`
runs vector addition, integer matrix multiplication and tensor matrix
multiplication on its own (A1 File 4). Separate executors handle ordinary
numeric operations, reduction chunks and DIV/SQRT/transcendentals (A1 Files
1 to 3). `omega_accelerator_world.c` has persistent vector, matrix and queued
submission paths, `rx_resident_gpu.c` has a resident-kernel lifecycle, and
`omega_world_gates.c` submits directly for qualification (A1b sections 2 and
3). The differences that matter (plan item 3): ordinary numeric execution
checks only the host marker; reduction also waits for the release semaphore
and uses a CPU barrier; DIV/SQRT also flushes GPU cache writes and waits for a
second marker; persistent world execution tracks in-flight ownership; the
resident kernel has its own cooperative shutdown protocol.

## 2. Motivating case (E1)

REPORTED, RECEIPT NOT READ (source: Drake/queen message 2026-10-02, relayed
from the E1 owner; no receipt path available to this cut): the live rc=-4
failures on the reduce chip run are marker2 wait timeouts. The second marker,
written after the L2 flush, was not seen within 5000 ms. The reduce launcher
waits 5 s while ldst and divsqrt wait 600 s (A1 File 2: reduce 5000 ms at
449-450 and 473-477 PR198; A1 File 1: divsqrt 600000 ms at 1156-1169; A1
File 5: ldst 600000 ms at 362-367 PR198).

So the result must say WHICH wait timed out and AFTER HOW LONG, and each
timeout is a per-step input from the caller, never hard-coded in the engine.
For contrast, today ldst gives its marker wait and its semaphore wait the same
step name "wait" (A1 File 5), so a timeout there is ambiguous. E1 is not
touched by this cut.

## 3. The engine seed, and what the module owns

Seed (plan item 4 and item 8). The engine is extracted from the vector
lifecycle that already lives inside `src/omega_blackwell_submit.c`:
`omega_blackwell_execute_vector` (A1 File 4, lines 24 to 226 recorded by the
scout). That path is the FIRST MIGRATED PATH of the engine. It already does
open (45), channel (46), pushbuffer and buffer allocation (50, 58-64), copy-in,
a poison pre-fill of the output, QMD build, `dsb sy` (128), submit (200), a
5000 ms marker wait (203), an equality check of the semaphore word without a
wait (207), readback (210) and close (225) (A1 File 4). Reading the source
now, the poison pre-fill is at lines 74 to 80 and uses
`OMEGA_VECTOR_POISON_VALUE`, which is `0xdeadbeefU` (`src/omega_vector.h:10`).
This differs from the scout line "no poison fill" for that file (A1 File 4
common row), so the source wins and the scout line is recorded as a
discrepancy. The engine strengthens the path with: the second marker after an
L2 flush (C3), a real release semaphore wait, a visibility barrier before
readback, a post-readback check that no output word is still its poison word,
named failure steps and the uncertain-completion rule below.

The seed reuses the existing launch-descriptor builders (the QMD and
pushbuffer code in `omega_blackwell_submit.c`, which moves to the GB10 backend
`src/omega_blackwell_engine.c/.h`) and the unwritten-output idea of
`src/omega_unwritten_trap.h` (A1 (a)). The trap module takes a single poison
word, while the engine job carries one poison word per output word, so the
engine core (A3) does its own word comparison against the poison array and
does not call `omega_unwritten_scan`. This is a design choice of this cut, not
something a scout verified.

Poison responsibility (plan item 8). The wrapper (the vector operation that
calls the engine) supplies poison values that are guaranteed to differ from the
expected outputs. The engine writes those words to the device output before
submit and afterwards only checks whether each output word changed. The
engine never chooses poison. Math verification (the oracle that compares the
output against the expected arithmetic result, today
`omega_blackwell_gates.c:191` for the poison test and the parity code in the
vector executor) stays in the wrapper.

OWNS:
1. Submission of one already-built program.
2. Buffer allocation and copy-in.
3. The completion rule, in one fixed order: wait for the first marker, then
   the second marker after the L2 flush (C3), then the release semaphore, then
   a visibility barrier before readback. Host order follows divsqrt (A1 File
   1: marker 1156, marker2 1160, semaphore 1168, barrier 1171), which PR198
   copies into reduce (A1 File 2: 470, 473-477, 482-485).
4. Per-step timeouts, recorded in the result.
5. Copy-out.
6. Cleanup on every exit where completion is known, success or failure.
7. The poison write before and the unchanged-word check after.
8. One error vocabulary, with the failing step named.
9. The uncertain-completion block (section 5).

DOES NOT OWN:
1. Kernel generation or codegen.
2. Launch-geometry choice.
3. Numeric contracts, math verification, or what a gate measures.
4. Choosing poison values.
5. Evidence directories and receipts.
6. Physics pin, lock and quiet-flag policy. These stay in the scripts and the
   chip_run wrapper (A1b section 5: `check-physics-lock`).
7. Retries.
8. Scheduling.
9. Running more than one program per call.

Three clarifications, each with evidence:

- A. Pre-device refusals stay with the caller where they are about numeric
  contracts. Argument checks, kernel building and QMD preflight happen before
  any device is opened and return BAD_ARGS or OPERANDS (A1 File 1: 1023-1046;
  File 2: 318; File 5: 234-255). The engine's own INVALID_ARGS covers only a
  malformed job (null pointers, wrong lengths, zero timeouts), checked before
  any device is touched.
- B. The Blackwell backend, not the engine core and not the caller, turns the
  job into a QMD (the hardware work descriptor) and a pushbuffer. Evidence:
  today the QMD is built after allocation because it needs the GPU addresses
  of the buffers (A1 File 2: allocs 333-336 then "qmd" at 370; File 3: allocs
  113-123 then "qmd" at 180; File 4: QMD invariant at 118).
- C. One program per call. The reduce level loop calls the chunk launcher
  repeatedly (A1 File 2: `omega_reduce_gb10` 467-517) and the loop stays in
  the caller. Persistent device contexts (`omega_accelerator_world.c` and the
  resident seat in `src/runtime/rx_resident_gpu.c`) keep one device open across
  many submits (A1b sections 2 and 3). They are mapped here but not absorbed in
  v1: the engine opens and closes per call, like the launchers do today.
  `src/runtime` is M20-owned: reuse ideas only, never edit.

## 4. The contract

Public entry point, in `src/omega_gpu_engine.h`:

    int omega_gpu_execute(const OmegaGpuJob *job, OmegaGpuResult *result);

It returns 0 (`OMEGA_GPU_ENGINE_OK`) on success, otherwise the failure code,
which is also stored in `result->failure`.

### 4.1 The job (plan item 5)

| field | meaning |
|---|---|
| program, program_len | encoded program bytes, already built by the caller |
| layout | `OMEGA_GPU_LAYOUT_VECTOR_1D` only; later layouts are separate qualified cuts |
| element_count | number of 32-bit elements, greater than 0 |
| input_a, input_a_len | first input, `element_count * 4` bytes |
| input_b, input_b_len | second input, `element_count * 4` bytes |
| output, output_len | host output buffer, 4-byte aligned, `element_count * 4` bytes |
| poison, poison_count | wrapper-supplied poison words, one per output word, `poison_count == element_count` |
| timeouts | `marker_ms`, `release_semaphore_ms`, `marker2_ms`, `visibility_ms`, each greater than 0 (`marker2_ms` is ignored with the A/B flag below) |
| flags | zero means every protection ON. `OMEGA_GPU_ENGINE_FLAG_NO_C3` exists only for the A/B control arm that PR198 keeps under `OMEGA_C3_PROTECT_OFF` (A1 headline 1) |

The 4-byte element size follows the vector executor's buffer sizing
`(n*4+0xfff)&~0xfff` (A1 File 4: 55-56). Anything else is INVALID_ARGS.

### 4.2 The result (plan items 6 and 7)

The result records: the failure name; the last COMPLETED state; the precise
failed step (an enum, `OMEGA_GPU_ENGINE_STEP_*`, with names); which wait
failed (`MARKER`, `RELEASE_SEMAPHORE` or `MARKER2`), its configured limit and
the milliseconds actually waited; the driver return code, `errno` captured
right after the failing call, and the driver text; the observed marker,
marker2 and semaphore words, with a mask saying which are meaningful; whether
context and allocations were retained; whether cleanup failed; and the number
of still-poison words per output (`output_unchanged_words`, one output today).
The vector path uses one semaphore word (A1 File 4: `hsem` at lines 124-126
now; A1b section 1), so the result carries one.

### 4.3 States (plan item 6)

INITIAL -> PREPARED -> SUBMITTED -> GPU_COMPLETE -> OUTPUT_VISIBLE ->
OUTPUT_PRODUCED -> SUCCESS.

INITIAL is "nothing completed yet" and is the last state of a run that fails
before PREPARED (the device may or may not be open). Success is reported only
at SUCCESS. The result always records the last state completed, so a failure
says how far the run got. The earlier draft of this document called the first
state NONE and the last state COMMITTED.

### 4.4 Failures (plan item 7)

| name | meaning |
|---|---|
| DEVICE_OPEN | the device could not be opened |
| CHANNEL_CREATE | the channel could not be created |
| ALLOC | a device buffer could not be allocated |
| PREPARE | copy-in, the device-side poison fill, or the descriptor build failed |
| SUBMIT | submitting the work failed |
| COMPLETION_WAIT | the marker wait or the marker2 wait failed or timed out; `wait` says MARKER or MARKER2, with `wait_timeout_ms` and `waited_ms` |
| RELEASE_WAIT | the release semaphore wait failed or timed out; `wait` is RELEASE_SEMAPHORE, with `wait_timeout_ms` and `waited_ms` |
| VISIBILITY_WAIT | the visibility barrier failed or timed out, or the readback failed |
| OUTPUT_UNCHANGED | after readback, some output words still equal their poison words; the result gives the unchanged-word count per output |
| INVALID_ARGS | the job or the result pointer is malformed |
| INTERNAL_INVARIANT | the engine or its backend broke its own contract (incomplete backend table, a stub that is not implemented yet, an impossible state) |
| CLEANUP | free or close failed after an otherwise good run |
| UNCERTAIN_COMPLETION_BLOCKED | an earlier job in this process left completion uncertain; this job was refused without touching the device |

Reconciliation note. Plan item 7 lists "completion wait" and "release wait"
as two names. The lieutenant brief for this cut said the completion wait names
which wait it was (MARKER, RELEASE_SEMAPHORE or MARKER2). This document keeps
both names distinct and splits the waits so no name overlaps: COMPLETION_WAIT
covers MARKER and MARKER2, and RELEASE_WAIT covers RELEASE_SEMAPHORE. The
`wait` field names the wait in both cases. If the queen wants all three waits
under COMPLETION_WAIT, RELEASE_WAIT would then have no meaning, so that is
left as a decision for the queen.

Preserved on every failure: driver return code, `errno`, driver text, and the
observed marker, marker2 and semaphore words.

Cleanup runs on every exit where completion is known. If cleanup itself fails
after an earlier failure, the first failure is kept in the result and
`cleanup_failed` is set; the engine never overwrites the first failure. If
cleanup fails after an otherwise good run, the failure is CLEANUP, the last
state stays OUTPUT_PRODUCED, and the run is not SUCCESS.

What the engine promises: "faithfully launched, the GPU completed, and these
bytes are not their poison words". It never judges whether the math is correct.

### 4.5 Transitions

| # | Transition | Guard (check) | Failure name, step | Launcher step today (A1 / A1b) |
|---|---|---|---|---|
| 0 | start -> INITIAL | the engine is not blocked; the job and result are well formed; the backend table is complete | UNCERTAIN_COMPLETION_BLOCKED, step BLOCKED; INVALID_ARGS, step VALIDATE; INTERNAL_INVARIANT, step BACKEND_CHECK | arg checks: divsqrt 1023, reduce 318 (BAD_ARGS) |
| 1 | INITIAL -> PREPARED | device and channel open; all buffers allocated; inputs copied in; output poison written; descriptors built | DEVICE_OPEN (OPEN_DEVICE); CHANNEL_CREATE (CREATE_CHANNEL); ALLOC (ALLOC); PREPARE (COPY_IN, FILL_POISON, BUILD) | vector: open 45, channel 46, alloc 50 and 58-64, qmd 118; divsqrt: open 1059, channel 1060, alloc 1062 and 1073, fills 1075-1078; reduce 324-341; ldst 268-286 |
| 2 | PREPARED -> SUBMITTED | the backend submits the work | SUBMIT (SUBMIT) | "submit": vector 200, divsqrt 1154, reduce 448, simt 263, ldst 360 |
| 3 | SUBMITTED -> GPU_COMPLETE | first marker seen; then marker2 (C3); then the release semaphore; each within its own timeout | COMPLETION_WAIT (WAIT_MARKER or WAIT_MARKER2); RELEASE_WAIT (WAIT_RELEASE_SEMAPHORE). Any non-zero wait result is an uncertain completion (section 5) | marker: vector 203 (5 s), divsqrt 1156 (600 s), reduce 449 (5 s), simt 267 (5 s), ldst 362 (600 s); marker2: divsqrt 1160-1162 (SILENT), reduce and ldst PR198 only; semaphore: divsqrt 1168, reduce 455, ldst 367; vector has an instant compare only at 207, simt has none |
| 4 | GPU_COMPLETE -> OUTPUT_VISIBLE | the barrier (`dsb sy`) succeeds within `visibility_ms` and the output is copied back | VISIBILITY_WAIT (BARRIER or COPY_OUT) | dsb + memcpy: divsqrt 1171-1172, reduce 458-459; vector 128 is before submit and the readback at 210 has no barrier; simt has none (271) |
| 5 | OUTPUT_VISIBLE -> OUTPUT_PRODUCED | no output word still equals its poison word | OUTPUT_UNCHANGED (SCAN), with the unchanged-word count per output | the vector executor poisons at lines 74-80 but the check lives in `omega_blackwell_gates.c:191`; `tests/test_omega_unwritten_trap_gb10.c:158` scans after the call (A1 (b)) |
| 6 | OUTPUT_PRODUCED -> SUCCESS | every buffer freed and the device closed without error | CLEANUP (FREE or CLOSE); last state stays OUTPUT_PRODUCED | close: vector 225, divsqrt 1173, reduce 460, simt 273, ldst 370; none frees buffers |

Failures at steps 1, 2 and 4 and the late failures at 5 and 6 run cleanup
(free every allocated buffer, then close) before returning. A failure at step 3
does not: see section 5.

Mapping to the existing numeric codes (`src/omega_numeric.h:271-276`, A1 (d):
OK 0, BAD_ARGS -1, NOT_ENCODED -2, OPERANDS -3, DEVICE -4, FPENV -5; no
"unwritten" code exists; the next free value is -6, which is an inference):

| engine result | old code a migrating caller reports |
|---|---|
| OK | OK (0) |
| DEVICE_OPEN, CHANNEL_CREATE, ALLOC, PREPARE, SUBMIT, COMPLETION_WAIT, RELEASE_WAIT, VISIBILITY_WAIT, CLEANUP, UNCERTAIN_COMPLETION_BLOCKED, INTERNAL_INVARIANT | ERR_DEVICE (-4) |
| INVALID_ARGS | ERR_BAD_ARGS (-1) |
| OUTPUT_UNCHANGED | ERR_DEVICE (-4) during migration; PROPOSED new `OMEGA_NUMERIC_ERR_UNWRITTEN` = -6 in a later PR |

This cut does not edit `omega_numeric.h`. Callers keep reporting -4 during
migration, but the engine result carries the precise name, step, wait and
time, so the old code no longer hides the cause. The vector executor returns
a bare -1 for every failure today (A1 File 4), so its wrapper decides its own
mapping when it migrates.

## 5. Uncertain completion rule (plan item 7)

If the engine cannot tell whether the GPU finished, it RETAINS the device
context and every allocation: no free, no close. "Cannot tell" means any
non-zero result from a completion wait (marker, marker2 or release
semaphore): a timeout (`OMEGA_GPU_BACKEND_TIMEOUT`), an unknown device state
(`OMEGA_GPU_BACKEND_UNKNOWN_STATE`), or a driver fault, because in each case the GPU may
still be running and may still write the buffers. The reason to retain: freeing
memory or closing the context under a running GPU could let it write into
memory that has been reused. (UNVERIFIED, 70%: the exact driver behavior on
freeing under a running channel was not measured by anyone in this program.)

The engine then:
1. records in the result that context and allocations were retained
   (`retained = 1`), the failure (COMPLETION_WAIT or RELEASE_WAIT), the wait,
   `waited_ms`, driver return code, `errno`, driver text and the observed
   marker, marker2 and semaphore words;
2. sets a process-wide block;
3. refuses every later `omega_gpu_execute` call in that process, in order
   before argument checks, with UNCERTAIN_COMPLETION_BLOCKED at step BLOCKED
   and last state INITIAL.

Stub-level API for the block, in `src/omega_gpu_engine.h`:
`omega_gpu_engine_is_blocked()` returns 1 once blocked. A test-only
`omega_gpu_engine_test_reset_block()` clears it. A second test-only function,
`omega_gpu_engine_test_force_block()`, was added so the compile test can
exercise the blocked result before the real core exists. Production code
must never call either test function. There is no production way to clear the
block: recovery is a new process, and whether that is safe is unproven (section
10).

## 6. Backend seam

The engine core talks to the device only through the function table
`OmegaGpuBackend` in `src/omega_gpu_engine.h`, which carries its own context
pointer. The table: open_device, create_channel, alloc, copy_in, fill_poison,
build, submit, wait_marker, wait_marker2, wait_release_semaphore, barrier,
copy_out, diagnostics, free_buf, close. This lets a fake backend drive every
transition and every failure name in host tests. Compared with the first
draft: open is split into open_device and create_channel so DEVICE_OPEN and
CHANNEL_CREATE are told apart; `build_and_submit` is split into `build` and
`submit` so PREPARE and SUBMIT are told apart; the `diagnostics` callback
returns the raw driver code, driver text and observed sync words; and the
roles are the vector layout's (program, input A, input B, output, backend
scratch).

A wait callback returns 0 when satisfied, `OMEGA_GPU_BACKEND_TIMEOUT` on
timeout, `OMEGA_GPU_BACKEND_UNKNOWN_STATE` when the device reports a state the
backend cannot classify, and any other non-zero value for a driver fault. The
engine measures `waited_ms` itself and captures `errno` right after a failing
call. The barrier callback uses the same codes, but a barrier failure is a
VISIBILITY_WAIT, which is not an uncertain completion because completion is
already known at that point.

Default backend. `omega_gpu_execute` uses a process-wide default backend
pointer. Real code sets it with `omega_gpu_engine_set_backend`; host tests set
a fake. With none set, `omega_gpu_execute` returns the defined stub result
(INTERNAL_INVARIANT, step NOT_IMPLEMENTED) until the A3 core exists.

Replaced call. The first draft had `omega_gpu_engine_run(backend, ctx,
request, result)`. It is replaced by `omega_gpu_execute(job, result)` plus the
default-backend setter, because plan item 5 fixes the public call. Nothing
needed the explicit-backend call: a test that wants a fake sets it as the
default. The first draft's `OmegaGpuEngineRequest` and its multi-buffer, flags
and opaque-launch-descriptor fields are replaced by `OmegaGpuJob`
(VECTOR_1D only).

The real Blackwell GB10 backend wraps the same `m16_native_*` calls the
launchers use today: open, create_channel, alloc_memory, free_memory,
submit_methods, wait_marker, close (declared in `physics/m16/m16_native.h`,
A1 (f)). No fake of these exists anywhere today (A1 (f), A1b section 5), so
this seam is the first.

CPU_ONLY seam. The device backend is compiled out under
`OMEGA_NUMERIC_CPU_ONLY`, exactly like the reduce launcher: include guards at
`src/omega_numeric_reduce_gb10.c:38-41`, the executor guarded at 258, and the
early return at 479-480 (A1 (c)). The engine core and the fake backend need no
physics headers and build under that flag. The header includes no physics
header at all.

## 7. What this cut changes

This commit changes no gate measurement, no kernel, no launcher and no
evidence file. It changes only: this document, `src/omega_gpu_engine.h`,
`src/omega_gpu_engine.c` (still a stub; superseded by section 13) and
`tests/test_omega_gpu_engine_compile.c`. `mk/omega-gpu-engine.mk` is unchanged (superseded by section 13).
The top-level Makefile is not edited (omega#198 edits it). No launcher file and
not `src/omega_blackwell_submit.c` is edited in this cut. None of the files in
the omega#198 list is touched (A1b section 6).

## 8. Phase plan (corrected)

Receipts that exist today: NONE. All labels are NOT_RUN.

Correction to the first draft: Phase A may edit `src/omega_blackwell_submit.c`
and `.h`. Drake asked for that twice (queen ruling). The condition is that any
PR touching `omega_blackwell_submit.c` stays DRAFT and cannot merge until the
M17 and M18 gates and the vector chip test have run in Phase B. The reason is
the file's sha256 pin: `tests/run_m17_gates.sh:66-67,129-130` and
`src/omega_blackwell_gates.c:1166-1167` (A1b section 1). Existing blackwell
chip gates must keep measuring the same things: no gate changes.

Phase A (light lane only, no GPU):
1. A2 (omega#218, draft): this document, the header, the compiling stub and
   `make test-gpu-engine-compile`, which checks the stub's exact return values,
   that every enum name is distinct, and that the block functions behave as
   specified.
2. A3 (stacked): the engine core in `src/omega_gpu_engine.c`.
3. `src/omega_blackwell_engine.c/.h`: the GB10 backend extracted from
   `omega_blackwell_execute_vector`.
4. Edit `src/omega_blackwell_submit.c/.h` so the vector executor becomes a
   wrapper over the engine (it supplies poison guaranteed to differ from its
   expected outputs, and keeps the arithmetic oracle). DRAFT only.
5. `tests/test_omega_gpu_engine.c`: host test of the REAL engine core against a
   simulated driver, driving every transition and every failure name, the
   uncertain-completion block, and cleanup-failure precedence.
6. A mutation sweep on the shared runner `tools/mutation_runner.sh`
   (merged in omega#204, no Python; edit kinds sed, marker-sed and replace).
   Each mutant removes one protection (poison write, unchanged check, marker2
   wait, release wait, barrier, retain-on-uncertain, block, cleanup) and must be
   KILLED by the host test.
7. `tests/test_omega_gpu_engine_gb10.c` and
   `tools/manifests/gpu_engine.chiprun`, written now in the format of
   `tools/manifests/unwritten_trap.chiprun` over `tools/chip_run.sh`
   (omega#205, both on origin/main). Written in Phase A, run only in Phase B.
8. Before any chip test (plan item 10): the host test above, the mutant sweep,
   the existing numeric, reduction and DIV/SQRT host regressions, and a
   production build at the pinned PHYSICS version.

Phase B, held until the queen says the GPU is released. Conversion order is
unchanged, with real GB10 tests before the next step starts:
1. The vector chip test, and the M17 and M18 gates for the edited
   `omega_blackwell_submit.c` (the sha256 pin is re-sealed by a chip rerun).
2. Reduce launcher: the FIRST NUMERIC migration.
3. Load/store launcher.
4. Base executor (`omega_numeric_gb10.c`), then divsqrt, then the remaining
   executors in `omega_blackwell_submit.c`.
5. A final PR deletes the old duplicated launch code.

Converting `omega_numeric_gb10.c` and the remaining `omega_blackwell_submit.c`
executors also adds the semaphore wait and the C3 protection they lack today
(A1 headline 3 and 4): a behavior change, tested on the chip. The numeric
launchers do not link `omega_blackwell_submit.c` today (A1b section 1), so
their chip build recipes (`tests/run_numeric_gates.sh:156-163`,
`tests/run_reduce_chip.sh:64`, `tests/run_tensor_chip.sh:67`) must gain the
engine sources at the step that converts them.

## 9. Order note

Vector addition is the engine SEED: it proves the engine using the path
already inside `omega_blackwell_submit.c`. Reduce stays the first numeric
migration in Phase B.

## 10. Unproven until chip

Unproven assumptions: sufficiency of this synchronization sequence for the vector path; cache visibility under faults; safe recovery after uncertain completion; suitability for persistent kernels. Host tests cannot prove those. E1 untouched; this cut is not chip-qualified.

## 11. Open questions and UNVERIFIED items

Carried from A1 and A1b, plus new ones from this cut:
1. The E1 marker2 timeout case in section 2 is reported, receipt not read.
2. How `src/omega_numeric_gb10.c` is compiled in the main binary: not read, 70%
   a glob or a tool compiles it (A1 open questions).
3. PR198 line numbers in this document may have drifted; the head moved from
   617971aa to 44741c1b (A1b section 6).
4. No GPU run was done by the scouts or by this cut; chip behavior is code
   reading only.
5. The A1 scout said `omega_blackwell_submit.c` has no poison fill; the source
   on this branch fills the vector output with `0xdeadbeef` (lines 74-80, and
   at 286 for the matrix path). Poison is uniform there, so the wrapper will
   supply an array filled with that word.
6. Whether the NUM files carry their own 18 setup words: 70% yes (A1b open
   question 3). Three implementations of those words likely exist, which the
   backend should unify.
7. `omega_world_dispatch_matmul` was read only by grep (A1b open question 5).
8. How `--run-m18-gates` is invoked from the Makefile or tools: 70% a tools
   script (A1b open question 2).
9. A legitimate output equal to its poison word would be reported as unchanged.
   The wrapper must pick poison guaranteed to differ from the expected output;
   the engine cannot check that. Likelihood unmeasured.
10. The numeric code -6 for "unwritten" is a proposal only.
11. The clock used for `waited_ms` is left to A3 (superseded by section 13: an injectable test clock was added).
12. Whether a failed SUBMIT can leave work queued on the GPU is unknown (no
    evidence read). This cut treats SUBMIT as a known failure and runs cleanup;
    the queen may want SUBMIT to count as uncertain too. UNVERIFIED, 50%.
13. Whether the COMPLETION_WAIT / RELEASE_WAIT split in section 4.4 matches
    the queen's intent.
14. (A3b1) Whether `src/omega_gpu_engine.h` and `.c` join the sha256 pinned lists next to
    `src/omega_blackwell_engine.*` (the vector executor now depends on them). They are not
    added in A3b1. Also whether free-then-close is safe on the chip for the vector launch
    (section 14, UNVERIFIED, 70%).

## 12. Changes from the first draft (omega#218 commit 6e077f1)

| first draft | now |
|---|---|
| `omega_gpu_engine_run(backend, ctx, request, result)`, `OmegaGpuEngineRequest/Result` | `omega_gpu_execute(job, result)`, `OmegaGpuJob/Result`, default backend setter |
| states NONE .. COMMITTED | states INITIAL .. SUCCESS |
| failures DEVICE_OPEN, DEVICE_ALLOC, DEVICE_SUBMIT, GPU_COMPLETION_TIMEOUT, OUTPUT_NOT_VISIBLE, OUTPUT_NOT_WRITTEN, DEVICE_ERROR | DEVICE_OPEN, CHANNEL_CREATE, ALLOC, PREPARE, SUBMIT, COMPLETION_WAIT, RELEASE_WAIT, VISIBILITY_WAIT, OUTPUT_UNCHANGED, INVALID_ARGS, INTERNAL_INVARIANT, CLEANUP, UNCERTAIN_COMPLETION_BLOCKED |
| wait name SEMAPHORE | wait name RELEASE_SEMAPHORE |
| step as a string | step as an enum with names |
| poison per output buffer, default 0x55555555, engine default | poison array supplied by the wrapper, engine never chooses |
| flag NO_POISON | removed: poison is always on |
| Phase A may not edit `omega_blackwell_submit.c` | Phase A may edit it as a DRAFT that cannot merge before the M17/M18 gates and the vector chip test |
| mutation tool: unspecified | shared shell runner `tools/mutation_runner.sh` |

## 13. Host tests and mutants (A3a engine core)

Status: host NOT_RUN, chip NOT_RUN. Nothing in this section has been compiled
or executed by its author. The queue lines `DEEP2-A3a-host`, `DEEP2-A3a-mut`
and `DEEP2-A3a-compile` carry the first runs.

What A3a adds: the real `omega_gpu_execute` in `src/omega_gpu_engine.c`
(replacing the stub when a backend is set), a fake-backend host test, and a
mutation sweep. Build targets in `mk/omega-gpu-engine.mk`: `test-gpu-engine`,
`test-gpu-engine-mutants`, `test-gpu-engine-compile`. No physics header, no
device. The Blackwell backend and `omega_blackwell_submit.c` are untouched
(A3b).

Core details the doc did not fix before (choices of this cut):
- Order of backend calls: open, channel, alloc of SCRATCH, PROGRAM, INPUT_A,
  INPUT_B, OUTPUT (in that order; SCRATCH gets `bytes == 0`, the backend sizes
  it, UNVERIFIED: the A3b backend may need a different rule), copy-in of
  PROGRAM, INPUT_A, INPUT_B, poison fill, build, submit, marker wait, marker2
  wait (skipped with NO_C3), release semaphore wait, barrier, copy-out, then
  free in reverse allocation order and close.
- Diagnostics (driver code, text, observed marker, marker2 and semaphore words)
  are captured with one `diagnostics` call at the FIRST failure of a run,
  including OUTPUT_UNCHANGED, and are not captured on success. `errno` is read
  immediately after the failing callback. A later cleanup fault never
  overwrites them.
- `waited_ms` is measured by the engine around each wait callback using
  CLOCK_MONOTONIC. A small additive test hook,
  `omega_gpu_engine_test_set_clock`, lets the host test supply a fake clock so
  `waited_ms` is asserted exactly.
- The header has one output only (`OMEGA_GPU_ENGINE_JOB_OUTPUTS` is 1, no
  buffer counts), so the "two outputs" and "buffer count out of range" cases
  of the A3a brief do not exist yet. The unchanged-word count is per output
  and the test checks output 0.
- With no backend set, the defined stub result (INTERNAL_INVARIANT at step
  NOT_IMPLEMENTED) is kept, as section 6 says. A NULL job or result is
  INVALID_ARGS.
- SUBMIT failure is a known failure (section 11, item 12): cleanup runs, no
  block. A barrier or copy-out failure also runs cleanup (completion is known).

Test cases in `tests/test_omega_gpu_engine.c` (id: what it guards):
- SUCCESS_FULL_RUN: a good run is SUCCESS with every result field defined,
  output bytes copied back, balanced open/close.
- CALL_ORDER_FULL_RUN: the exact backend call order of the whole run.
- STATE_ADVANCES_STEP_BY_STEP: `last_state` at each backend call (INITIAL until
  build, PREPARED at submit, SUBMITTED during the waits, GPU_COMPLETE at barrier
  and copy-out, OUTPUT_PRODUCED during cleanup).
- ROLES_BYTES_AND_REVERSE_FREE_ORDER: alloc and copy-in roles and byte counts,
  free in reverse order.
- PER_STEP_TIMEOUTS_FROM_JOB: each wait gets its own timeout from the job.
- POISON_WRITTEN_BEFORE_SUBMIT: the fake device output holds the wrapper poison
  words at submit.
- NO_C3_SKIPS_MARKER2_ONLY: the NO_C3 flag drops only the marker2 wait.
- FAIL_DEVICE_OPEN, FAIL_CHANNEL_CREATE, FAIL_ALLOC (third buffer, partial
  cleanup), FAIL_PREPARE_COPY_IN, FAIL_PREPARE_FILL_POISON, FAIL_PREPARE_BUILD:
  failure name, failing step, last state INITIAL, driver code, errno and text
  passthrough, cleanup, host output untouched.
- FAIL_SUBMIT_IS_NOT_UNCERTAIN: SUBMIT cleans up and does not block.
- FAIL_VISIBILITY_BARRIER, FAIL_VISIBILITY_COPY_OUT: VISIBILITY_WAIT at the
  right step, cleanup, no block.
- UNCERTAIN_MARKER_TIMEOUT, UNCERTAIN_MARKER2_UNKNOWN_STATE,
  UNCERTAIN_RELEASE_DRIVER_FAULT: COMPLETION_WAIT or RELEASE_WAIT, which wait,
  configured limit, exact `waited_ms`, observed sync words, retained = 1,
  nothing freed or closed, process blocked.
- UNCERTAIN_BLOCKS_NEXT_JOB_NO_BACKEND_CALL, UNCERTAIN_TEST_RESET_CLEARS_BLOCK.
- UNCHANGED_NOTHING_WRITTEN, UNCHANGED_PARTIAL_EXACT_COUNT,
  UNCHANGED_LAST_WORD_ONLY, UNCHANGED_FIRST_WORD_ONLY,
  UNCHANGED_ALL_WRITTEN_IS_SUCCESS, UNCHANGED_NEEDS_POISON_FILL:
  OUTPUT_UNCHANGED with the exact count, SUCCESS when every word changed.
- CLEANUP_FREE_FAILS_AFTER_GOOD_RUN, CLEANUP_CLOSE_FAILS_AFTER_GOOD_RUN:
  CLEANUP failure, last state stays OUTPUT_PRODUCED, remaining frees and close
  still run. CLEANUP_FIRST_CLEANUP_FAULT_KEPT,
  CLEANUP_FAULT_NEVER_OVERWRITES_FIRST_FAILURE,
  CLEANUP_FAULT_AFTER_UNCHANGED_KEEPS_UNCHANGED: first failure is kept,
  `cleanup_failed` set.
- INVALID_ARGS_PROGRAM, _LAYOUT, _ZERO_ELEMENTS, _BUFFERS, _OUTPUT_ALIGNMENT,
  _POISON, _UNKNOWN_FLAG, _ZERO_MARKER_TIMEOUT, _ZERO_RELEASE_TIMEOUT,
  _ZERO_VISIBILITY_TIMEOUT, _ZERO_MARKER2_TIMEOUT_WITH_C3, _NULL_JOB,
  _NULL_RESULT: INVALID_ARGS at VALIDATE with no backend call.
- BLOCKED_REFUSES_BEFORE_ANY_CHECK_OR_CALL, BLOCK_RESET_ALLOWS_JOBS_AGAIN,
  NO_BACKEND_NOT_IMPLEMENTED, BACKEND_TABLE_INCOMPLETE_REFUSED (each of the 15
  callbacks missing in turn): the refusals that come before any backend call.

Mutants in `tools/gpu_engine_mutations.sh` (each is a `MUT:<name>` marker in
`src/omega_gpu_engine.c`; each must be KILLED by the host test; none is
currently marked redundant):
- VALIDATE_SKIPPED, VALID_PROGRAM, VALID_LAYOUT, VALID_COUNT_ZERO, VALID_ALIGN,
  VALID_POISON_COUNT, VALID_FLAGS, VALID_TIMEOUT_MARKER1,
  VALID_TIMEOUT_MARKER2: argument validation, whole and one check at a time.
- BLOCK_CHECK_SKIPPED: the blocked refusal. BACKEND_CHECK_SKIPPED: the table
  completeness check.
- OPEN_NOT_TRACKED, ALLOC_NOT_TRACKED: cleanup bookkeeping (close, free).
- CLEANUP_SKIPPED_CHANNEL, _ALLOC, _COPY_IN, _SUBMIT: cleanup on one early
  failure path each. FREE_UNALLOCATED, FREE_SKIPPED, CLOSE_SKIPPED: what
  cleanup frees and closes.
- CLEANUP_OVERWRITES_FIRST, CLEANUP_FLAG_NOT_SET, SUCCESS_AFTER_CLEANUP_FAIL:
  cleanup fault precedence and the SUCCESS rule.
- POISON_FILL_SKIPPED, POISON_INDEX_FIXED, UNCHANGED_COUNT_SATURATES,
  UNCHANGED_CHECK_SKIPPED: the poison write and the unchanged-word check.
- STATE_PREPARED_SKIPPED, STATE_SUBMITTED_SKIPPED, STATE_GPU_COMPLETE_SKIPPED,
  STATE_OUTPUT_VISIBLE_SKIPPED, STATE_OUTPUT_PRODUCED_SKIPPED,
  STATE_SUCCESS_SKIPPED: each transition's `last_state` update.
- MARKER1_WAIT_SKIPPED, MARKER1_TIMEOUT_WRONG, MARKER1_RESULT_IGNORED,
  MARKER2_WAIT_SKIPPED, C3_FLAG_IGNORED, MARKER2_RESULT_IGNORED,
  RELEASE_WAIT_SKIPPED, RELEASE_RESULT_IGNORED, BARRIER_SKIPPED,
  BARRIER_RESULT_IGNORED: the completion rule, per wait: skipping the call,
  ignoring its result, the NO_C3 flag, the timeout wiring.
- UNCERTAIN_FREES_MARKER1, UNCERTAIN_FREES_MARKER2, UNCERTAIN_FREES_RELEASE
  (free and close on uncertain completion), RETAINED_FLAG_NOT_SET, NO_BLOCK
  (do not block afterwards): the uncertain-completion rule.
- WAITED_NOT_A_DIFFERENCE: `waited_ms` is end minus start.

Not covered by any host test (needs the chip, see section 10): whether the
sequence is sufficient on real hardware, cache visibility under faults,
recovery after an uncertain completion, persistent kernels.

## 14. Blackwell backend and the vector executor (A3b1)

Status: DRAFT, host NOT_RUN, chip NOT_RUN. Nothing in this section has been
compiled or executed by its author. The queue lines `DEEP2-A3b1-backend`,
`DEEP2-A3b1-prod`, `DEEP2-A3b1-host` and `DEEP2-A3b1-mut` carry the first runs.
The simulated-driver host test, the backend mutants, the chip test and the chip
manifest are section 14.1 (cut A3b2).

What this cut adds:
- `src/omega_blackwell_engine.c` and `.h`: the backend table
  (`omega_blackwell_engine_backend()`, type `OmegaGpuBackend`) that the core
  drives. It defines no job, result or execute type. The device sequence is the
  one in `omega_blackwell_execute_vector` as it stood at commit 676f16f, split
  into the fifteen callbacks.
- `omega_blackwell_execute_vector` in `src/omega_blackwell_submit.c` keeps its
  public signature and result fields. Inside, it encodes the program, builds the
  poison words, calls `omega_gpu_execute`, then runs the vector oracle
  (`omega_vector_verify_oracle`) on the host as before.
- Makefile: `src/omega_blackwell_engine.c` and `src/omega_gpu_engine.c` join the
  two source lists that already hold `src/omega_blackwell_submit.c` (`SRCS` and
  `RX_COMPOSE_GPU_SRCS`), nothing else.
- `mk/omega-gpu-engine.mk`: `test-gpu-engine-backend-compile` (syntax check of
  the backend, `-c` only, no link, no device) and `test-gpu-engine-prod-build`
  (`make build/omegatool` at the physics pin: links only, opens no device).
- Digest lists: `src/omega_blackwell_engine.h` and `.c` are added to the pinned
  list in `src/omega_blackwell_gates.c` (manifest at 1166-1169) and in
  `tests/run_m17_gates.sh`. `src/omega_gpu_engine.h` and `.c` are NOT added (the
  vector executor depends on them now; whether to pin them is open, section 11).

Choices of this cut:
- Poison. The wrapper supplies it. Each poison word is the bit complement of the
  expected output word (`~(a[i] + b[i])`), computed on the host before launch.
  A word and its complement always differ, so a poison word never equals the
  expected word at its position. This replaces the constant
  `OMEGA_VECTOR_POISON_VALUE` for this executor.
- L2 flush. The core has no callback for it, so `build` emits the L2 flush and
  the second release marker (0x46464646 at marker page + 0x10) inside the
  pushbuffer unless the job sets NO_C3, as divsqrt does today
  (`src/omega_numeric_divsqrt_gb10.c:1145-1152`). The vector wrapper passes flags
  0, so the vector launch now has C3, a second marker wait and a semaphore wait
  that it did not have at 676f16f (A1 finding 1 and 3). The timeouts are 5000 ms
  each, the value the old marker wait used.
- Completion rule per wait: the native wait is a ">=" serial compare
  (`physics/m16/m16_native.c:93-108`), so each wait callback also re-checks that
  the word is exactly the expected value and reports a mismatch as
  `UNKNOWN_STATE` (an uncertain completion). The check is from GPT-6 Astra's
  first cut.
- Values on success. The core copies driver diagnostics only on a failure, and
  frees and closes the device (unmapping the marker and semaphore memory) before
  it returns. So the backend snapshots the marker, second marker and semaphore
  words inside its wait callbacks, plus the submit and marker-done timestamps,
  and `omega_blackwell_engine_run_info` returns them. The wrapper fills
  `completion_marker`, `intermediate_semaphore`, `launch_timestamp_ns`,
  `completion_timestamp_ns` and `elapsed_ns` from that. The engine result itself
  cannot supply these on success.
- Cleanup. The backend frees every buffer with `nvrm_free` before `close`,
  which the old launcher never did (it relied on `nvrm_close`, `nvrm.c:757-766`).
  `nvrm_free` before close is used by `src/omega_accelerator_world.c`
  (A1b section 4). Chip behaviour of this order for the vector launch is
  UNVERIFIED (confidence 70%).
- Allocation order differs from 676f16f: the four SCRATCH buffers come first,
  then program, input A, input B, output. Sizes are unchanged. Chip effect
  UNVERIFIED (confidence 90% harmless, no address is hard coded).
- One run at a time: the backend keeps one static context and refuses a second
  open while a run is active.

### 14.1 Simulated-driver host test, backend mutants, chip test (A3b2)

Status: host PASS at DEEP2-A3b3 (333 sim assertions, 120 mutants: 107 killed, 13
redundant as listed, 0 test gaps), chip NOT_RUN. The A3b2 run found no bug in the
engine or backend sources: the first run failed because the test re-used the hang
or overshoot armed in the simulated driver for its "backend works again after
drain" check (test bug, fixed in the test), and one mutant row did not build
(unused variable under -Werror, fixed in the row). An address and undefined-behaviour
sanitizer run of the sim test is clean except one old finding in src/omega_blackwell_qmd.c
line 139 (`0x1000 << 19` overflows a signed int; gcc wraps it to the intended value). The
file is gate-hashed (M17/M18) and was left unchanged.

Files:
- `tests/test_omega_blackwell_engine.c` and `tests/fake_m16_native.c/.h`
  (`make test-gpu-engine-sim`). The REAL engine core, the REAL Blackwell backend
  and the REAL `omega_blackwell_execute_vector` run on the host. Only seven
  driver symbols are replaced by the fake: `m16_native_open`,
  `m16_native_create_channel`, `m16_native_submit_methods`, `m16_native_wait_marker`,
  `m16_native_close`, `nvrm_alloc`, `nvrm_free`. No physics C file is linked.
  Output is `[PASS] id` or `[FAIL] id` per check and a last line
  `SUMMARY passed=N failed=M`.
- `tools/gpu_engine_backend_mutations.sh` (`make test-gpu-engine-backend-mutants`),
  a table adapter over `tools/mutation_runner.sh`. One mutant per protection in
  `src/omega_blackwell_engine.c` and in the vector wrapper. The real sources carry
  no marker comments; the rows match their text exactly once or by line range.
- `tests/test_omega_gpu_engine_gb10.c` (compile only here,
  `make test-gpu-engine-gb10-compile`) and `tools/manifests/gpu_engine.chiprun`
  (omega#205 chip_run format). Phase B only: needs the GB10, the queen's go and
  the quiet flag via chip_run. Never run in Phase A. The chip test is a fresh
  rewrite against the real API; GPT-6 Astra's first-cut test (commit 21392ee,
  older API) was the outline.

Test case groups (each id starts with `sim_`; the full list is the `check` calls in the test):
`success` (24-entry call order, allocation sizes, pushbuffer packet order, wait
order, zeroed sync words at the doorbell, kernel arguments and addresses),
`noc3`, `twice` (back to back runs), `open`, `channel`, `alloc` (fault on each of
the 8 allocations, and a buffer that fails its own checks), `submit`, `unc_*`
(8 uncertain-completion scenarios: marker, second marker and semaphore each
hanging and overshooting, plus no-C3), `cleanup` (free and close faults),
`unchanged` (unwritten output, wrong program bytes), `guard` (direct table calls
with bad roles, lengths, NULLs, page rounding), `close` (close unmaps the sync
words), `vec` (the wrapper: result, oracle, poison complement, timeouts, flags,
timestamps).

Failure names that no driver fault can reach through the real backend, covered
only by the fake-backend core test (`tests/test_omega_gpu_engine.c`): `PREPARE`
(copy-in and build refuse only on arguments the core already validated),
`VISIBILITY_WAIT` (the barrier always returns 0), `INTERNAL_INVARIANT` (the
backend table is complete).

Modelling assumptions of the fake. Every one is a MODEL, UNVERIFIED on the chip,
confidence about 70 percent that it matches the real driver and device:
device memory is host memory with GPU address equal to CPU address and a dirty
0x5a fill; the second marker release lands only if an L2 flush came between the
two releases; a successful real `nvrm_free` clobbers `errno` (to EBADF); the
channel's own 4 KiB pushbuffer buffer is orphaned when the backend installs the
large ring; the pushbuffer is parsed and executed in order. The link set (the
Makefile `RX_COMPOSE_GPU_SRCS` list plus `omega_core.c`, `omega_canonical.c`,
`sha256.c`) links and runs on the host (checked at A3b3); whether it also matches the chip build is not tested here.

Backend and wrapper mutants. Each must be KILLED by the sim test, or carries class
`redundant` with a reason (a surviving redundant mutant is accepted by the runner).

- `BE_ALLOC_CHECK_CPU`: a buffer without a CPU mapping is refused (ALLOC)
- `BE_ALLOC_CHECK_SIZE`: a buffer smaller than asked is refused (ALLOC)
- `BE_ALLOC_CHECK_ALIGN`: a GPU address that is not 256-byte aligned is refused (ALLOC)
- `BE_ALLOC_OWN_CHECK_NOTE`: the backend's own refusal text reaches the result
- `BE_ALLOC_OWN_CHECK_FREE`: a buffer that fails its own checks is freed, not leaked
- `BE_ROLE_FREE_SKIPPED`: free_buf really frees a program, input or output buffer
- `BE_SCRATCH_FREE_SKIPPED`: release_scratch really frees all four SCRATCH parts
- `BE_SCRATCH_FREE_ERROR_DROPPED`: a failed SCRATCH part free is reported
- `BE_SCRATCH_FREE_STOPS_AT_ERROR`: one failed SCRATCH part free does not leave the other parts allocated
- `BE_SCRATCH_ALLOC_FAIL_NO_RELEASE`: a failed SCRATCH allocation frees the parts that did succeed (the core does not)
- `BE_ERRNO_NOT_RESTORED`: the frees after a failed SCRATCH allocation do not clobber the errno the core reads
- `BE_ALLOC_FAIL_TEXT_RESTORE` (redundant): the failure text survives the cleanup frees. Reason: release_scratch writes neither c->text nor the driver text on a successful free, in the simulated driver and in nvrm_free (physics nvrm.h: free only zeroes the NvrmMem); the restore is a safety net for a driver that writes text on success, which no model here does
- `BE_SCRATCH_PB_NOT_INSTALLED`: the large pushbuffer ring replaces the channel's 4 KiB buffer
- `BE_PB_REPLACED_RESET` (redundant): the flag that records the replaced ring is cleared after SCRATCH is freed. Reason: the zeroing of m.pb_mem stays (it is a separate line); a stale flag only repeats that harmless zeroing, and open memsets the whole context each run. physics m16_native_close is only nvrm_close(&ctx->rm) and never reads pb_mem
- `BE_BUSY_CHECK_DROPPED`: a second open while a run is retained is refused
- `BE_OPEN_FAIL_KEEPS_BUSY`: a failed open releases the busy flag
- `BE_CLOSE_BUSY_CLEAR_DROPPED`: close releases the busy flag so the next run can open
- `BE_OPEN_MEMSET_DROPPED`: open resets the whole context, including the previous run's snapshot
- `BE_OPEN_NATIVE_FLAG_DROPPED`: a failed open still reports the driver's own text
- `BE_OPEN_ERROR_IGNORED`: a failed driver open is reported (DEVICE_OPEN)
- `BE_OPEN_LAST_RC_DROPPED`: the failed open's driver code reaches the result
- `BE_CHANNEL_ERROR_IGNORED`: a failed channel create is reported (CHANNEL_CREATE)
- `BE_CHANNEL_LAST_RC_DROPPED`: the failed channel create's driver code reaches the result
- `BE_ALLOC_LAST_RC_DROPPED`: the failed allocation's driver code reaches the result
- `BE_ALLOC_UNKNOWN_ROLE_ACCEPTED`: alloc refuses an unknown role
- `BE_ROLE_MEM_UPPER_BOUND`: a role above the last one has no buffer slot
- `BE_ROLE_MEM_LOWER_BOUND`: a negative role has no buffer slot
- `BE_ROUND_PAGES_ZERO_ACCEPTED`: a zero-byte allocation is refused
- `BE_ROUND_PAGES_NO_ROUNDING`: allocation sizes round up to whole pages
- `BE_ROUND_PAGES_MINIMUM` (redundant): one-page minimum. Reason: after the zero check, rounding any nonzero byte count up to a page multiple already gives at least one page, so the minimum can never apply
- `BE_COPY_IN_NO_MEMCPY`: copy_in really copies the program and the inputs
- `BE_COPY_IN_BOUNDS`: copy_in refuses a length beyond the buffer
- `BE_COPY_IN_NULL_SOURCE`: copy_in refuses a NULL source
- `BE_FILL_POISON_DROPPED`: the device output is pre-filled with the poison words
- `BE_FILL_POISON_SHORT`: every output word is pre-filled, including the last
- `BE_FILL_POISON_BOUNDS`: fill_poison refuses a length beyond the output buffer
- `BE_COPY_OUT_NO_MEMCPY`: copy_out really reads the device output back
- `BE_COPY_OUT_BOUNDS`: copy_out refuses a length beyond the output buffer
- `BE_BUILD_LAYOUT_CHECK`: build refuses a missing job and an unsupported layout
- `BE_BUILD_COUNT_OVERFLOW` (redundant): grid arithmetic overflow guard. Reason: an element count above UINT32_MAX - 63 needs at least 16 GiB of input, output and input buffers, so the buffer-size check in the same condition chain refuses it first; only the grid rounding (n + 63) could overflow, and it is never reached
- `BE_BUILD_BUFFERS_SIZE`: build refuses buffers smaller than the element count needs
- `BE_C3_FLAG_IGNORED`: the NO_C3 flag removes the L2 flush and the second marker
- `BE_C3_NEVER`: with flags 0 the L2 flush and the second marker are in the pushbuffer
- `BE_THREADS_WRONG`: the launch covers every element
- `BE_GRID_ROUNDS_DOWN`: the grid rounds up so a partial last block still runs
- `BE_ARGS_C_SWAPPED`: the kernel writes the OUTPUT buffer
- `BE_ARGS_A_B_SWAPPED`: the kernel gets input A and input B in the right argument slots
- `BE_PROGRAM_VA`: the QMD points at the program buffer
- `BE_CBANK_VA`: the QMD points at the constant bank buffer
- `BE_SEM_VA`: the QMD release semaphore address is the word the host waits on
- `BE_MARKER_VA`: the first release lands on the word the host waits on
- `BE_SEM_ZERO_DROPPED`: the semaphore word is zeroed before the doorbell (no stale DONE)
- `BE_MARKER_ZERO_DROPPED`: the first marker word is zeroed before the doorbell (no stale payload)
- `BE_MARKER2_ZERO_DROPPED`: the second marker word is zeroed before the doorbell
- `BE_NOC3_MARKER2_REPORTED`: with NO_C3 the second marker is neither waited for nor reported
- `BE_MARKER2_OFFSET_CHANGED`: the second marker is 0x10 bytes after the first
- `BE_MARKER2_PAYLOAD_CHANGED`: the second marker payload is 0x46464646
- `BE_RELEASE1_PAYLOAD_ZERO`: the first release writes the completion payload
- `BE_RELEASE2_PAYLOAD_ZERO`: the second release writes its payload
- `BE_FLUSH_OP_DROPPED`: the L2 flush operation word is L2_FLUSH_DIRTY
- `BE_FLUSH_METHOD_CHANGED`: the flush uses the memory-operation method
- `BE_WFI_FLAG_RELEASE1`: the first release waits for idle (WFI)
- `BE_WFI_FLAG_RELEASE2`: the second release waits for idle (WFI)
- `BE_BARRIER_IN_BUILD` (redundant): dsb sy after zeroing the sync words. Reason: a CPU barrier instruction has no effect a host test can observe (the fake driver runs on the same core); it is verified on the chip only (Phase B)
- `BE_QMD_RELEASE_CHECK` (redundant): extra QMD1 release-word checks. Reason: the real descriptor builder always produces these words (src/omega_blackwell_qmd.c), so no input makes the check fire; it guards a future change of the builder, which tests/test_omega_blackwell_qmd.c style checks cover
- `BE_PB_OVERFLOW` (redundant): pushbuffer word-capacity guard. Reason: the vector pushbuffer is a fixed 492 words against a 1024-word capacity, so it cannot overflow for any job
- `BE_SUBMIT_NOTHING_BUILT` (redundant): submit before build is refused. Reason: the driver refuses an empty submission too (physics m16_native.c m16_native_enqueue_methods: count == 0 returns -1), so the result is the same refusal
- `BE_LAUNCH_NS_DROPPED`: the launch time is recorded before the doorbell
- `BE_SUBMIT_ERROR_IGNORED`: a failed submit is reported (SUBMIT), not treated as launched
- `BE_WAIT_UNMAPPED_ACCEPTED`: a wait before build or after free is refused without a driver call
- `BE_WAIT_DRIVER_RC_DROPPED`: the driver's wait code reaches the result
- `BE_WAIT_RC_MAPPING` (redundant): timeout versus driver fault classification. Reason: the engine core treats every nonzero wait result the same (uncertain completion), and the real m16_native_wait_marker returns only 0 or -1 (m16_native.c)
- `BE_WAIT_EQUALITY_RECHECK`: an overshoot of the ">=" compare is refused (UNKNOWN_STATE)
- `BE_WAIT_SNAPSHOT_DROPPED`: the observed word is recorded after every wait
- `BE_MARKER_DONE_NS_ALWAYS`: the completion time is recorded only when the first marker wait succeeded
- `BE_WAIT_MARKER_WRONG_WORD`: wait_marker waits on the first marker word
- `BE_WAIT_SEMAPHORE_WRONG_VALUE`: the release semaphore wait is for DONE (6), not the initial value (5)
- `BE_DIAG_DRV_RC_DROPPED`: diagnostics report the raw driver code
- `BE_DIAG_TEXT_OWN_DROPPED`: diagnostics report the backend's own text
- `BE_DIAG_TEXT_DRIVER_DROPPED`: diagnostics fall back to the driver's text
- `BE_DIAG_MARKER_DROPPED`: diagnostics report the first marker word
- `BE_DIAG_MARKER2_DROPPED`: diagnostics report the second marker word
- `BE_DIAG_SEMAPHORE_DROPPED`: diagnostics report the semaphore word
- `BE_DIAG_VALID_MARKER_DROPPED`: the first marker is flagged valid only when mapped
- `BE_DIAG_VALID_MARKER2_DROPPED`: the second marker is flagged valid only when mapped
- `BE_DIAG_VALID_SEMAPHORE_DROPPED`: the semaphore is flagged valid only when mapped
- `BE_FREE_SCRATCH_ROLE_DROPPED`: free_buf of SCRATCH frees the four SCRATCH parts
- `BE_FREE_UNKNOWN_ROLE_ACCEPTED`: free_buf refuses an unknown role
- `BE_FREE_ERROR_DROPPED`: a failed free is reported (CLEANUP)
- `BE_RELEASE_KEEPS_MARKER_POINTER`: the first marker word is unmapped before SCRATCH is freed
- `BE_RELEASE_KEEPS_MARKER2_POINTER`: the second marker word is unmapped before SCRATCH is freed
- `BE_RELEASE_KEEPS_SEM_POINTER`: the semaphore word is unmapped before SCRATCH is freed
- `BE_CLOSE_KEEPS_MARKER_POINTER`: close unmaps the first marker word
- `BE_CLOSE_KEEPS_MARKER2_POINTER`: close unmaps the second marker word
- `BE_CLOSE_KEEPS_SEM_POINTER`: close unmaps the semaphore word
- `BE_CLOSE_NOT_CALLED`: close really closes the driver
- `BE_CLOSE_ERROR_IGNORED`: a failed close is reported (CLEANUP)
- `WRAP_POISON_NOT_COMPLEMENT`: the poison word is the complement of the expected sum
- `WRAP_POISON_CONSTANT`: the poison word never collides with a legitimate sum (0xdeadbeef is one)
- `WRAP_ENGINE_FAILURE_IGNORED`: an engine failure (here a cleanup fault) fails the wrapper
- `WRAP_ORACLE_IGNORED`: the host oracle decides success, not the engine
- `WRAP_PARITY_FLAG_FORCED`: parity_verified follows the oracle
- `WRAP_MARKER_NOT_REPORTED`: the completion marker is reported
- `WRAP_SEMAPHORE_NOT_REPORTED`: the semaphore value is reported
- `WRAP_LAUNCH_TIME_NOT_REPORTED`: the launch time is reported
- `WRAP_DONE_TIME_NOT_REPORTED`: the completion time is reported
- `WRAP_ELAPSED_NOT_REPORTED`: the elapsed time is reported
- `WRAP_SM_ARCH_NOT_REPORTED`: the architecture is reported
- `WRAP_ELEMENT_COUNT_NOT_REPORTED`: the element count is reported
- `WRAP_NULL_GUARD_DROPPED`: a NULL argument is refused before any use
- `WRAP_ZERO_COUNT_GUARD` (redundant): a zero element count is refused by the wrapper. Reason: the engine refuses element_count 0 as INVALID_ARGS (src/omega_gpu_engine.c job_valid) and the wrapper then returns -1, so the result is the same refusal
- `WRAP_MARKER_TIMEOUT`: the first marker wait limit is 5000 ms
- `WRAP_SEMAPHORE_TIMEOUT`: the semaphore wait limit is 5000 ms
- `WRAP_MARKER2_TIMEOUT`: the second marker wait limit is 5000 ms
- `WRAP_VISIBILITY_TIMEOUT` (redundant): the visibility limit is 5000 ms. Reason: the Blackwell barrier callback ignores its timeout argument (it is a CPU dsb sy that cannot wait), so no host observation depends on the value
- `WRAP_FLAGS_ALL_PROTECTIONS`: the wrapper runs with every protection on (flags 0)
- `WRAP_BACKEND_NOT_INSTALLED`: the wrapper installs the Blackwell backend itself
- `WRAP_PROGRAM_LENGTH`: the whole encoded program is handed to the engine
- `WRAP_MARKER_CHECK` (redundant): second check of the marker payload. Reason: the backend wait_word already refuses any first-marker value other than 0x44444444 (UNKNOWN_STATE), so the engine fails before this line is reached
- `WRAP_SEMAPHORE_CHECK` (redundant): second check of the semaphore value. Reason: the backend wait_word already refuses any semaphore value other than DONE (6), so the engine fails before this line is reached

The `dsb sy` barrier mutants are chip only: a CPU barrier instruction has no
effect a host test can observe, so they are redundant on the host and belong to
Phase B. Mutants of the core itself stay in `tools/gpu_engine_mutations.sh`.

## 15. Origin

The engine core and the Blackwell backend were seeded from the first cut written
by GPT-6 Astra (branch `fix/omega-gpu-engine-first-cut`, commit 21392ee, dated
2026-10-02). Claude workers did the host test, the mutation sweep, the
reconciliation of Astra's single-file engine to the omega#218 public API
(`OmegaGpuJob`, `OmegaGpuResult`, `omega_gpu_execute`, the backend table), and the
vector wrapper. What was kept from Astra: the device call order, the pushbuffer
and QMD sequence, the allocation checks, the QMD1 word checks, the exact-value
re-check after each wait, the dsb barrier and the L2 flush plus second marker.
What was not copied: Astra's own job and result types, her state machine, the
poison comparison and the quarantine logic, because the A3a core already does
those.
