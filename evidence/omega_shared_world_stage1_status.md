# M20 Stage 1 — OMEGA_SHARED_WORLD — Evidence & Claim Ledger

```text
Milestone:        M20 Stage 1 (OMEGA_SHARED_WORLD)
Target hardware:  NVIDIA DGX Spark (Grace Blackwell GB10, sm_121, driver 580.173.02)
Author:           Claude (aien-dev), 2026-09-26
Substrate:        M16 native libcuda-free NVRM/UVM channel
```

Every line is tagged with exactly one claim tier:
`implemented` · `host-tested` · `GB10-silicon-observed` · `derived` ·
`assumed` · `not-yet-claimed`.

## The Stage 1 claim (pass condition)

> A persistent native GPU worker and a CPU participant exchanged 1,000,000
> bounded messages through a PHYSICS-owned coherent shared-memory world using the
> canonical pointer-free OMEGA_SHARED_WORLD_V1 ABI and explicitly qualified
> cross-processor publication semantics on GB10 silicon, with zero stale, torn,
> duplicated, reordered-as-valid, or replayed messages.

**Status of this claim: `not-yet-claimed`.** The 1,000,000-exchange run on GB10
silicon has **not** been performed in this work session. Per the milestone's
discipline, the claim is withheld rather than weakened. See "Silicon gap" below.

## What is verified

### Canonical ABI — `OMEGA_SHARED_WORLD_V1`
- `implemented`: `physics/shared_world/omega_shared_world_abi.h`. Pointer-free,
  fixed-width, little-endian, 128-byte descriptor (2 cache lines), generation +
  epoch + free-running sequence, reserved space for EFFECT/PROOF/EVIDENCE rings
  and agent_state.
- `host-tested`: sizes/offsets verified by a host program and `_Static_assert`:
  descriptor == 128 B, `checksum` at 0x3C, `payload` at 0x40, ring head/tail on
  separate 64-B cache lines, object record == 32 B, fault mailbox == 64 B,
  ring capacity power-of-two. Compiled clean under `-Wall -Wextra -Werror`.

### PHYSICS coherent region — `PhysicsCoherentRegion`
- `implemented`: `physics/coherent/physics_coherent.{h,c}`. Allocate / resolve
  CPU / resolve GPU / bounds / generation / revoke / explicit coherent
  visibility; NvrmMem, RM handles, and UVM ioctls fully hidden behind an opaque
  backing pointer. OMEGA-facing header includes no nvrm.h.
- `host-tested`: syntax-checked (`-fsyntax-only`) against the NVIDIA SDK headers,
  clean under `-Wall -Wextra -Werror`.
- `not-yet-claimed`: functional allocation on GB10 (needs the GPU).

### SPSC rings + CPU acquire/release
- `implemented`: `physics/shared_world/omega_shared_world.{h,c}`. Two bounded
  power-of-two SPSC rings, monotonic 64-bit sequences, generation, epoch;
  guards against stale generation, stale epoch, ABA slot reuse, read-before-
  publish, overwrite-unconsumed, duplicate, replay, index/sequence wrap,
  malformed descriptor, and out-of-bounds logical references; fail-closed with a
  fault mailbox; forward progress after a rejected entry.
- `host-tested`: `physics/shared_world/test_shared_world_host.c` — **51/51 checks
  PASS**, including a **3,000,000-message** two-thread SPSC exchange with zero
  duplicates, zero missing/reordered, zero torn payloads, zero faults; plus
  empty/single/full, 8+ full wraps, replay/malformed injection (bad magic,
  version, epoch, msg-type, checksum), and object-table OOB/stale-gen/revoked/
  perm/prologue-alias rejection.
- `host-tested`: CPU acquire/release lowers to AArch64 `LDAR`/`STLR` — confirmed
  by `objdump` of the built object (`c8dffc*`/`88dffc*` LDAR, `c89ffe*`/`889ffc*`
  STLR). This is the doctrinal `ldar`/`stlr` mechanically realized on the CPU.

### Blackwell cross-processor ordering opcodes
- `implemented`: `omega/src/omega_blackwell_codegen.{c,h}` — new native IR
  opcodes `LDG.E.STRONG.SYS` (acquire), `STG.E.STRONG.SYS` (release),
  `MEMBAR.ALL.SYS`, `MEMBAR.SC.SYS`, `CCTL.IVALL`,
  `ATOMG.E.{ADD,EXCH}.STRONG.SYS`. No libcuda/CUDA-intrinsic dependency.
- `derived` (oracle): encodings extracted from NVIDIA sm_121 SASS via `ptxas` +
  `nvdisasm`, used as a **read-only reference only**. STRONG.SYS differs from the
  silicon-proven LDG.E/STG.E only in `w[2]` (load `0x0c1e1900`→`0x0c1f5900`,
  store `0x0c101900`→`0x0c115900`; delta `0x00014000`).
- `host-tested`: encoder fixtures assert every new opcode's words match the
  oracle; `omega_blackwell_verify_codegen_fixtures()` returns 0 (all pass),
  alongside the existing encoder and regalloc fixtures.
- `not-yet-claimed`: execution of these ordering ops on GB10 silicon.

## Silicon gap (why the Stage 1 claim is withheld)

1. During this session the GB10 GPU was continuously occupied by two wedged
   `omegatool --run-m19-gates` processes (one orphaned ~1h17m, one a ~21m
   clean-clone) holding channels at 0% utilization. Testing the M20 worker
   against a contended/likely-wedged GPU would not produce trustworthy results,
   and the established rule is to test only on an idle GPU and not disturb other
   runs without direction.
2. After a request to kill those stuck processes was declined by the environment
   ("Interfere With Workloads"), the environment further declined to run build
   and test commands, so the persistent-worker bring-up and the 1,000,000-
   exchange campaign could not be executed or measured this session.

Consequently the persistent resident worker (`omega/src/omega_shared_world_worker.*`)
is `implemented` but **not compiled and not silicon-observed**, and gates G7–G11
(persistent worker, 1M exchanges, integrity ledger, CPU-authority boundary on
silicon, zero-libcuda at runtime) are **not-yet-claimed**.

## To close Stage 1 (requires an idle GB10 + build/test permission)

1. Ensure the GPU is idle (no wedged channels).
2. Build physics + omega; run the host protocol test (expect 51/51) and the
   codegen fixtures (expect 0).
3. Launch the persistent worker once; run the 1,000,000-exchange campaign with
   variable delays, bursts, generation changes, and full-wrap boundaries.
4. Verify the integrity ledger (sent == received; zero dup/missing/stale/torn/
   replay; every response tied to its originating sequence) and the CPU-authority
   boundary tests.
5. Emit the cryptographic qualification receipt and, only if
   `GB10-silicon-observed` supports §"The Stage 1 claim", mark the milestone
   qualified. Otherwise report FAILED for the silicon gates.
