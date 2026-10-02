# Omega GPU Engine: design (phase A2)

Status: DESIGN ONLY. Nothing here launches a GPU kernel yet. Every test and
chip label for this work is NOT_RUN (host NOT_RUN, chip NOT_RUN). No receipt
exists today.

Sources. Facts below come from two read-only scout reports, cited as A1 and
A1b: `~/handoffs/2026-10-02-DEEP2-A1-scout.md` and
`~/handoffs/2026-10-02-DEEP2-A1b-scout.md`. A1 section names are quoted as
"A1 File N" or "A1 (a)..(f)". Line numbers are those the scouts recorded
(MAIN f633e343 for A1, 2db71da for A1b; PR198 lines are marked "PR198" and
may have drifted because that PR has since moved, A1b section 6). Anything
neither scout verified is labeled UNVERIFIED with a confidence.

## 1. What it is, and why

The Omega GPU Engine is one small module that takes "here is my already-built
GPU kernel, its inputs, and the buffers I expect it to fill" and returns
either SUCCESS or exactly one named failure that says which step broke.

Why: five launcher files repeat the same open, allocate, copy-in, pushbuffer,
submit, wait, readback, close sequence (A1 Files 1 to 5, A1b section 4:
"What NUM duplicates from SUB"). They report errors in three different styles
(the GB10_DEVFAIL macros, the LDST_DEVERR line, and a bare `-1`, A1 macro
section and File 4). The divsqrt second-marker wait fails silently with no
step name (A1 headline 2: divsqrt 1160-1162). `omega_blackwell_submit.c` has
no semaphore wait and no second-marker protection, "C3" (A1 headline 3 and
A1b section 1: instant compare at line 207, no L2 flush, no marker2).

## 2. Motivating case

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
step name "wait" (A1 File 5), so a timeout there is ambiguous.

## 3. What the module owns, and what it does not

Kept small on purpose, so it does not become a new giant layer.

OWNS:
1. Submission of one already-built kernel image.
2. Buffer allocation and copy-in.
3. The completion rule, in one fixed order: wait for the first marker, then
   the second marker after the L2 flush (C3), then the release semaphore,
   then a barrier before readback. Host order follows divsqrt (A1 File 1:
   marker 1156, marker2 1160, semaphore 1168, barrier 1171), which PR198
   copies into reduce (A1 File 2: 470, 473-477, 482-485).
4. Per-step timeouts, recorded in the result.
5. Copy-out.
6. Cleanup on every exit, success or failure.
7. Poison-fill before and unwritten scan after (the "unwritten-output trap"),
   reusing `omega_unwritten_scan` from `src/omega_unwritten_trap.h` (A1 (a):
   the caller passes the poison; callers use 0x55555555, matching the 0x55
   fills in the launchers). The engine default is 0x55555555.
8. One error vocabulary, with the failing step named.

DOES NOT OWN:
1. Kernel generation or codegen.
2. Launch-geometry choice.
3. Numeric contracts, or what a gate measures.
4. Evidence directories and receipts.
5. Physics pin, lock and quiet-flag policy. These stay in the scripts and the
   chip_run wrapper of another program (A1b section 5: `check-physics-lock`).
6. Retries.
7. Scheduling.
8. Running more than one kernel per call.

Check against the scout facts. The split holds. Three adjustments or
clarifications, each with evidence:

- A. Pre-device refusals stay with the caller. Argument checks, kernel
  building and QMD preflight happen before any device is opened and return
  BAD_ARGS or OPERANDS (A1 File 1: 1023-1046; File 2: 318; File 5: 234-255).
  These are the caller's contract checks, so the engine does not absorb them.
- B. The Blackwell backend, not the engine core and not the caller, turns the
  opaque launch descriptor into a QMD (the hardware work descriptor) and a
  pushbuffer. Evidence: today the QMD is built after allocation because it
  needs the GPU addresses of the buffers (A1 File 2: allocs 333-336 then
  "qmd" at 370; File 3: allocs 113-123 then "qmd" at 180). The engine core
  passes the descriptor through untouched, which keeps "launch-geometry
  choice" with the caller.
- C. One kernel per call is confirmed. The reduce level loop calls the chunk
  launcher repeatedly (A1 File 2: `omega_reduce_gb10` 467-517) and the loop
  stays in the caller. Persistent device contexts (`omega_accelerator_world.c`
  and the resident seat in `src/runtime/rx_resident_gpu.c`) keep one device
  open across many submits (A1b sections 2 and 3). They are mapped here but
  not absorbed in v1: the engine opens and closes per call, like the five
  launchers do today. `src/runtime` is M20-owned: reuse ideas only, never edit.

## 4. The contract

Callers give: the kernel image; the launch descriptor (opaque, passed
through); input buffers; output buffers with sizes; per-step timeouts
(marker, semaphore, marker2, in milliseconds).

The engine returns SUCCESS or exactly one of:

| name | meaning |
|---|---|
| DEVICE_OPEN | device or channel could not be opened |
| DEVICE_ALLOC | a device buffer could not be allocated |
| DEVICE_SUBMIT | building or submitting the work failed |
| GPU_COMPLETION_TIMEOUT | a completion wait ran out; the result names the wait (MARKER, SEMAPHORE or MARKER2) and `waited_ms` |
| OUTPUT_NOT_VISIBLE | the GPU finished but the outputs could not be made visible and read back |
| OUTPUT_NOT_WRITTEN | after readback, some output still holds the poison word; the result gives the unwritten byte count per output |
| DEVICE_ERROR | any other device or driver fault, step named (also used for a bad request and for cleanup failure) |

State machine: NONE -> PREPARED -> SUBMITTED -> GPU_COMPLETE ->
OUTPUT_VISIBLE -> OUTPUT_PRODUCED -> COMMITTED. NONE is "nothing done yet"
and is the state reported by a run that fails before PREPARED. Success is
reported only at COMMITTED. The result always records the last state reached,
so a failure says how far the run got.

Every output is poison-filled before submit (device copy and host copy) and
must contain non-poison words before OUTPUT_PRODUCED. Poison can be switched
off per request (flag NO_POISON); the default is ON. The C3 protection (L2
flush and second marker) is also ON by default; flag NO_C3 exists only for
the A/B control arm that PR198 keeps under `OMEGA_C3_PROTECT_OFF` (A1
headline 1).

What the engine promises: "faithfully launched, the GPU completed, and these
bytes are genuinely its output". It never judges whether the math is correct.

| # | Transition | Guard (check) | Failure name | Launcher step today (A1 / A1b) |
|---|---|---|---|---|
| 0 | start -> NONE | request is valid: kernel present, 1 to 8 outputs, backend table complete | DEVICE_ERROR, step "bad_request" | arg checks: divsqrt 1023, reduce 318 (BAD_ARGS) |
| 1 | NONE -> PREPARED | device and channel open; all buffers allocated; inputs copied in; outputs poison-filled | DEVICE_OPEN (steps "open", "channel"); DEVICE_ALLOC (steps "alloc_pb", "alloc_buffers"); DEVICE_ERROR (steps "copy_in", "poison") | open 1059, channel 1060, alloc_pb 1062, alloc_buffers 1073, fills 1075-1078 (divsqrt); open 324, channel 325, alloc 327-337, fills 340-341 (reduce); simt 103-129; ldst 268-286 |
| 2 | PREPARED -> SUBMITTED | backend builds QMD and pushbuffer and submits | DEVICE_SUBMIT (steps "build", "submit") | "submit": divsqrt 1154, reduce 448, simt 263, ldst 360, submit.c 200; "qmd"/"build_kernel": reduce 346, 370, simt 134, 180 |
| 3 | SUBMITTED -> GPU_COMPLETE | first marker seen; then marker2 (C3); then release semaphore; each within its own timeout | GPU_COMPLETION_TIMEOUT with wait = MARKER, MARKER2 or SEMAPHORE, `wait_timeout_ms`, `waited_ms`; a non-timeout driver fault gives DEVICE_ERROR | marker: divsqrt 1156 (600 s), reduce 449 (5 s), simt 267 (5 s), ldst 362 (600 s), submit.c 203 (5 s); marker2: divsqrt 1160-1162 (SILENT), reduce/ldst PR198 only; semaphore: divsqrt 1168, reduce 455, ldst 367; none in simt, instant compare only in submit.c 207 |
| 4 | GPU_COMPLETE -> OUTPUT_VISIBLE | barrier (dsb sy) succeeds and every output is copied back | OUTPUT_NOT_VISIBLE | dsb + memcpy: divsqrt 1171-1172, reduce 458-459; simt and submit.c have no dsb before readback (271, 210). No launcher can fail here today |
| 5 | OUTPUT_VISIBLE -> OUTPUT_PRODUCED | `omega_unwritten_scan` finds zero poison words in every output | OUTPUT_NOT_WRITTEN, with unwritten bytes per output | no launcher does this; only `tests/test_omega_unwritten_trap_gb10.c:158` scans after the call (A1 (b)) |
| 6 | OUTPUT_PRODUCED -> COMMITTED | every buffer freed and device closed without error | DEVICE_ERROR, step "cleanup" (state stays OUTPUT_PRODUCED) | close: divsqrt 1173, reduce 460, simt 273, ldst 370, submit.c 225; none frees buffers |

Cleanup runs on every exit. If cleanup itself fails after an earlier failure,
the first failure is kept in the result; the engine never overwrites it.

Mapping to the existing numeric codes (`src/omega_numeric.h:271-276`, A1 (d):
OK 0, BAD_ARGS -1, NOT_ENCODED -2, OPERANDS -3, DEVICE -4, FPENV -5; no
"unwritten" code exists; the next free value is -6, which is an inference):

| engine result | old code a migrating caller reports |
|---|---|
| OK | OK (0) |
| DEVICE_OPEN, DEVICE_ALLOC, DEVICE_SUBMIT, GPU_COMPLETION_TIMEOUT, OUTPUT_NOT_VISIBLE, DEVICE_ERROR (from the device) | ERR_DEVICE (-4) |
| DEVICE_ERROR with step "bad_request" | ERR_BAD_ARGS (-1) |
| OUTPUT_NOT_WRITTEN | ERR_DEVICE (-4) during migration; PROPOSED new `OMEGA_NUMERIC_ERR_UNWRITTEN` = -6 in a later PR |

This PR does not edit `omega_numeric.h`. Callers keep reporting -4 during
migration, but the engine result carries the precise name, wait and time, so
the old code no longer hides the cause.

## 5. Backend seam

The engine core talks to the device only through a small table of function
pointers (`OmegaGpuEngineBackend` in `src/omega_gpu_engine.h`): open, alloc,
copy_in, poison_output, build_and_submit, wait_marker, wait_marker2,
wait_semaphore, flush_barrier, copy_out, free_buf, close. This lets a fake
backend drive every transition and every failure name in host tests. The one
addition to the list the program brief gave is `poison_output`, because the
poison fill is a device-side write today (A1 File 1: divsqrt 1077).

A wait callback returns 0 when satisfied, a defined TIMEOUT value on timeout,
and any other nonzero value for a driver fault. The engine measures
`waited_ms` itself and captures `errno` right after a failing call.

The real Blackwell GB10 backend wraps the same `m16_native_*` calls the
launchers use today: open, create_channel, alloc_memory, free_memory,
submit_methods, wait_marker, close (declared in `physics/m16/m16_native.h`,
A1 (f)). No fake of these exists anywhere today (A1 (f), A1b section 5), so
this seam is the first.

CPU_ONLY seam. The device backend is compiled out under
`OMEGA_NUMERIC_CPU_ONLY`, exactly like the reduce launcher: include guards at
`src/omega_numeric_reduce_gb10.c:38-41`, the executor guarded at 258, and the
early return at 479-480 (A1 (c)). The engine core and the fake backend need no
physics headers and build under that flag. The phase A2 header includes no
physics header at all.

## 6. What is NOT changed

This PR changes no gate measurement, no kernel, no launcher and no evidence
file. It adds only: this document, `src/omega_gpu_engine.h`,
`src/omega_gpu_engine.c` (a stub), `mk/omega-gpu-engine.mk` and
`tests/test_omega_gpu_engine_compile.c`. The top-level Makefile is not edited
(omega#198 edits it). None of the files in the omega#198 list is touched
(A1b section 6). In particular `src/omega_blackwell_submit.c` is not edited in
Phase A: its sha256 is recomputed by `tests/run_m17_gates.sh:66-67,129-130`
and `src/omega_blackwell_gates.c:1166-1167` (A1b section 1), so any edit
invalidates chip receipts and can only be verified on the chip.

## 7. Phase plan

Receipts that exist today: NONE. All labels are NOT_RUN.

1. A2 (this PR, draft): design doc, header, compiling stub, and
   `make test-gpu-engine-compile`, which checks the stub's exact return values
   and that every enum name is distinct. Host NOT_RUN until the forge runs it.
2. A3 (stacked on A2): engine core, a fake backend, a host test that drives
   every transition and every failure name, and mutants that remove each
   transition check (each mutant must make the test fail).
3. Phase B, held until the queen says the GPU is released. The Blackwell GB10
   backend is evolved out of `src/omega_blackwell_submit.c` (it is the
   file that already owns the 18-word setup, the pushbuffer, the 0x44444444
   marker and the semaphore constants, A1b section 1). Conversion order, with
   real GB10 tests before the next step starts:
   1. Reduce launcher first.
   2. Load/store launcher.
   3. Base executor (`omega_numeric_gb10.c`), then divsqrt, then
      `omega_blackwell_submit.c`.
   4. A final PR deletes the old duplicated launch code.
   Moving `omega_blackwell_submit.c` re-seals the M17 and M18 digests, which
   needs a chip rerun (A1b "Implications"). Any PR that touches it stays DRAFT
   until its chip gate passes. Converting `omega_numeric_gb10.c` and
   `omega_blackwell_submit.c` also adds the semaphore wait and the C3 protection
   they lack today (A1 headline 3 and 4): a behavior change, tested on the chip.
   The numeric launchers do not link `omega_blackwell_submit.c` today (A1b
   section 1), so their chip build recipes (`tests/run_numeric_gates.sh:156-163`,
   `tests/run_reduce_chip.sh:64`, `tests/run_tensor_chip.sh:67`) must gain the
   engine sources at the step that converts them.

## 8. Open questions and UNVERIFIED items

Carried from A1 and A1b, plus new ones from this cut:
1. The E1 marker2 timeout case in section 2 is reported, receipt not read.
2. How `src/omega_numeric_gb10.c` is compiled in the main binary: not read, 70%
   a glob or a tool compiles it (A1 open questions).
3. PR198 line numbers in this document may have drifted; the head moved from
   617971aa to 44741c1b (A1b section 6).
4. No GPU run was done by the scouts; chip behavior is code-reading only (A1).
5. The poison value used by `omega_blackwell_submit.c` was not read; 75% it is
   0x55 (A1b open question 1).
6. Whether the NUM files carry their own 18 setup words: 70% yes (A1b open
   question 3). Three implementations of those words likely exist, which the
   backend should unify.
7. `omega_world_dispatch_matmul` was read only by grep (A1b open question 5).
8. How `--run-m18-gates` is invoked from the Makefile or tools: 70% a tools
   script (A1b open question 2).
9. New: a legitimate output equal to the poison word 0x55555555 (a valid float)
   would be reported as unwritten. The poison is a per-output field so a caller
   can choose another word. Likelihood unmeasured.
10. New: there is no BAD_REQUEST failure name, because the brief fixes the
    vocabulary. The stub reports a bad request as DEVICE_ERROR with step
    "bad_request". A3 or the queen may add a name.
11. New: the numeric code -6 for "unwritten" is a proposal only.
12. New: the choice of `waited_ms` measurement clock is left to A3.
