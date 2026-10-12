# Omega GPU native boundary: dependency map and freestanding link target

Native v1 P07 cut A. Tracks aienos#286 (cut A) and aien-architecture#190.
Companion of `docs/numeric/OMEGA_GPU_ENGINE.md` (the engine contract) and
`src/omega_gpu_platform.h` (the platform contract this cut adds).

Status words used here: DONE (host evidence in this repo's CI), NOT_RUN (no
evidence; needs the physical Spark in an attended window), PLANNED (a later
cut). NOT_RUN is never PASS.

## 1. What this cut claims, and what it does not

Claimed (host evidence, `make test-gpu-engine-freestanding`):

- Six files of the first-compute path build with `-ffreestanding -fno-builtin`
  and `OMEGA_GPU_NO_POSIX`, and their partial link leaves only `memcpy`,
  `memset`, `strcmp` and `strlen` undefined: `src/omega_gpu_engine.c`,
  `src/omega_gpu_platform.c`, `src/omega_blackwell_encoder.c`,
  `src/omega_blackwell_qmd.c`, `src/omega_blackwell_realize.c`, `src/sha256.c`.
  No Linux header, no `errno`, no `clock_gettime`, no `printf`, no `malloc`,
  no `pthread_*`, no `nvrm_*`, no `m16_*`. The make target fails on any other
  undefined symbol.
- The engine contract test (`tests/test_omega_gpu_engine.c`, 50 cases) passes
  linked against the freestanding engine object, with one added case: with no
  platform clock installed, every job is refused at `STEP_CLOCK_CHECK` before
  any backend call.
- The platform contract (`src/omega_gpu_platform.h`) has a test
  (`tests/test_omega_gpu_platform.c`, 6 cases) reaching every refusal name.
- The engine mutation sweep still kills all 49 mutants (`make test-gpu-engine-mutants`).

Not claimed:

- No whole-project independence from Linux. Section 3 lists every file that
  still calls the Linux RM/UVM driver through `physics/nvrm` and `physics/m16`,
  and which cut migrates it.
- No device ownership. Nothing in this cut opens, discovers, maps or resets a
  GPU. Device discovery, bounded MMIO/DMA/IRQ ownership and the SMMU path are
  cut B, and their evidence is NOT_RUN until an attended hardware window
  (native v1 gate N4; the B7b one-page SMMU probe is not GPU compute).
- No production use of the test-only hooks. `omega_gpu_engine_test_set_clock`
  remains test-only; the production time service is
  `omega_gpu_engine_set_clock`, which the native platform calls once at init.

## 2. Driver-provided responsibilities: who owns each today and later

Today every row is served by the Linux RM/UVM driver (nvidia-open 580.173.02)
through `physics/nvrm/nvrm.c` and `physics/m16/m16_native.c`, called from the
files in section 3.

| Responsibility | Linux path today | Native owner | Cut | Status |
|---|---|---|---|---|
| Device discovery, bounded ownership (MMIO/DMA/IRQ capabilities) | RM device open (`nvrm_open`, `m16_native_open`) | AIENOS PCI/SMMU layer + `OmegaGpuPlatform` MMIO/DMA services | B | NOT_RUN |
| Device and firmware initialization (GSP, falcons) | RM boot inside the kernel module | AIENOS GB10 firmware lifecycle (P08) | B/P08 | NOT_RUN |
| Context / address space creation | RM client + VA space objects (`nvrm_mthd`) | native context object behind `OmegaGpuDeviceHandle` | B | NOT_RUN |
| Memory allocation with cache attributes | `nvrm_alloc`, `nvrm_alloc_gpu_uncached` | `OmegaGpuPlatform.dma_alloc` with `OmegaGpuBufferDesc` (kind, rights, cache policy) | B | contract DONE, device NOT_RUN |
| GPU page tables / mapping | UVM + RM map | AIENOS page tables, GPU VA through `OmegaGpuVa` | B | NOT_RUN |
| Bus addresses / SMMU translation | kernel DMA API inside RM | AIENOS SMMU, `OmegaGpuBusAddr` (no raw physical address type exists) | B | NOT_RUN |
| Channel / queue creation (GPFIFO, USERD) | `m16_native_create_channel`, `nvrm_channel` | native queue behind `OmegaGpuQueueHandle`, ring is a `BUF_QUEUE` buffer | B/C | NOT_RUN |
| Submit token / doorbell | `m16_native_submit_methods`, `nvrm_enqueue`, `nvrm_ring` | `OmegaGpuPlatform.mmio_write32` on the doorbell region | C | NOT_RUN |
| Completion (marker words, semaphores) | `m16_native_wait_marker*`, host polling | `OmegaGpuPlatform.wait_word` on a HOST_POLLED, GPU-uncached buffer | C | contract DONE, device NOT_RUN |
| Interrupts | RM interrupt handling (not used by our launchers: they poll) | `OMEGA_GPU_SVC_IRQ` only when a real interrupt wait exists; polling never claims it | B | NOT_RUN |
| Visibility / cache maintenance | L2 flush mem-op + uncached marker (C3 block) | `OmegaGpuPlatform.sync_*` plus the same mem-op sequence in the encoder | C | NOT_RUN |
| Time, waiting, locking | `clock_gettime`, `usleep`, pthread | `OmegaGpuPlatform.time_now_ms`, `wait_ms`, `lock`/`unlock` | A (contract) | DONE |
| Reset / recovery of a hung device | RM channel teardown, process exit | native quiesce path (P03 lifecycle), no `ck_reset`-style shortcut | P08 | NOT_RUN |
| Resource numbering across incarnations | per-process RM handles | aienos#285 resource incarnations, before any domain-1 number is persisted | P05/#285 | PLANNED |

Rule carried over from the 2026-10-02 E1 stall: anything the CPU polls for a
device write is `OMEGA_GPU_RIGHT_HOST_POLLED` and must be
`OMEGA_GPU_CACHE_GPU_UNCACHED`. Source: nvidia-open 580.173.02
`src/common/sdk/nvidia/inc/nvos.h`, `NVOS32_ATTR2_GPU_CACHEABLE_YES`: "For
system memory this will not be coherent with direct CPU mappings." The
platform contract refuses a cached host-polled buffer
(`OMEGA_GPU_PLAT_REFUSED_CACHE_POLICY`).

## 3. File inventory: Linux driver and libc dependencies, migrating cut

Counts are `grep -c` of `nvrm_` and `m16_` on 2026-10-12 at omega main
536870f1. "Linux headers" lists hosted headers the file includes. "Cut" is
where the file stops depending on them. "Host tooling" means the file is
development-host measurement or gating and is not part of the native product
(the plan separates development hosts from the delivered runtime).

| File | nvrm_ | m16_ | Linux headers | Cut | Note |
|---|---|---|---|---|---|
| `src/omega_gpu_engine.c` | 0 | 0 | none (errno/time only on the host build) | A | DONE: `OMEGA_GPU_NO_POSIX` build, clock refusal |
| `src/omega_gpu_platform.c` | 0 | 0 | none | A | DONE: new, contract checks and handle table |
| `src/omega_blackwell_encoder.c` | 0 | 0 | none (stdio removed in A) | A | DONE |
| `src/omega_blackwell_qmd.c` | 0 | 0 | none | A | DONE |
| `src/omega_blackwell_realize.c` | 0 | 0 | none | A | DONE |
| `src/sha256.c` | 0 | 0 | none | A | DONE |
| `src/omega_blackwell_codegen.c` | 0 | 0 | stdio, stdlib (malloc/free for kernel code, printf in the variation self-test) | A.2 | allocator seam: caller-provided code buffer, self-test moved to the test |
| `src/omega_blackwell_matmul.c` | 0 | 0 | stdlib (free) | A.2 | same allocator seam |
| `src/omega_gpu_wait.c` | 0 | 0 | time.h | C | clock from the platform time service |
| `src/omega_gpu_serving.c` | 0 | 0 | none | C | moves with the session |
| `src/omega_gpu_matmul_api.c` | 0 | 0 | stdlib, stdio, time | C | moves with the session |
| `src/omega_gpu_attention_api.c` | 0 | 0 | stdio, stdlib, time | C | moves with the session |
| `src/omega_gpu_session.c` (+ `.h` leaks `NvrmMem`) | 19 | 7 | pthread, stdio, time | C | `NvrmMem` in the session header becomes `OmegaGpuBufferHandle`; lock from the platform |
| `src/omega_blackwell_engine.c` | 19 | 13 | errno, stdio, time | stays Linux-private | the Linux backend of `OmegaGpuBackend`; a sibling native backend over `OmegaGpuPlatform` arrives in B/C |
| `src/omega_numeric_gb10.c` | 21 | 2 | stdio, stdlib, errno, time | C/D | numeric launchers follow the engine Phase B order |
| `src/omega_numeric_divsqrt_gb10.c` | 20 | 2 | stdio, stdlib, errno, time | C/D | same |
| `src/omega_numeric_reduce_gb10.c` | 17 | 2 | stdio, stdlib, errno, time | C/D | same |
| `src/omega_numeric_ldst_gb10.c` | 16 | 2 | stdio, stdlib | C/D | same |
| `src/omega_blackwell_submit.c` | 41 | 34 | stdio, stdlib, time, unistd | D | vector executor; sha256-pinned by the M17/M18 gates, re-sealed on chip |
| `src/runtime/rx_resident_gpu.c` | 18 | 9 | pthread, stdio, stdlib, unistd | D | resident runtime seat |
| `src/omega_world_gates.c` | 60 | 2 | stdio, stdlib, time, unistd | host tooling | gates and measurement, stays on Linux |
| `src/omega_accelerator_world.c` | 38 | 9 | stdio, stdlib, time, unistd, sys/mman | host tooling | stays on Linux |
| `src/omega_blackwell_gates.c` | 0 | 1 | stdio, stdlib, time | host tooling | stays on Linux |

Headers that still expose a Linux driver type and must lose it before the cut
that migrates their users: `src/omega_gpu_session.h` (`NvrmMem`, cut C),
`src/omega_numeric_native.h` (C/D), `src/forge_realization.h`,
`src/omega_evidence.h`, `src/omega_accelerator_world.h`,
`src/omega_physics_dir.h` (host tooling; they keep it).

Distinct driver entry points in use (count of call sites, same snapshot):
`nvrm_mthd` 188, `m16_native_close` 46, `nvrm_alloc` 33,
`nvrm_alloc_gpu_uncached` 30, `m16_native_submit_methods` 11,
`m16_native_create_channel` 10, `nvrm_free` 6, `m16_native_open` 6,
`nvrm_retire` 5, `m16_native_wait_marker` 4, `nvrm_enqueue` 2,
`nvrm_channel_destroy` 2, `nvrm_channel` 2, `m16_native_wait_marker_ge` 2,
`nvrm_ring`, `nvrm_open`, `nvrm_close` 1 each. The 188 `nvrm_mthd` sites are
RM control calls (class allocation, VA space, channel setup); each maps to a
row of section 2, and none of them exists in the native path by design: the
native backend talks to the device through the platform services, not through
an RM method table.

## 4. The freestanding link target

`mk/omega-gpu-engine.mk`, targets `gpu-engine-freestanding` and
`test-gpu-engine-freestanding`, run by the `gpu-engine` job of
`.github/workflows/host-suites-2.yml`.

- Flags: `-std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin
  -fno-stack-protector -DOMEGA_GPU_NO_POSIX -DOMEGA_NUMERIC_CPU_ONLY`.
- The six objects are partially linked (`gcc -r -nostdlib`); `nm -u` on the
  result may show only `memcpy|memmove|memset|memcmp|strlen|strcmp` (and the
  GOT symbol). Anything else fails the build with the symbol named.
- `OMEGA_GPU_NO_POSIX` in `src/omega_gpu_engine.c`: `err_no` is recorded as 0
  (there is no errno), there is no default clock, and `omega_gpu_execute`
  refuses with `INTERNAL_INVARIANT` at `STEP_CLOCK_CHECK` until
  `omega_gpu_engine_set_clock` installed one. The host build is unchanged
  (errno captured, `CLOCK_MONOTONIC` default), and the 15 errno pass-through
  expectations of the contract test become 0 under the freestanding build.
- The same 49-mutant sweep runs on the host build; the mutation that removes
  the poison fill now matches the `ERRNO_CLEAR()` spelling.

## 5. The platform contract in one page

`src/omega_gpu_platform.h`, ABI 1.0.

- Handles: `OmegaGpuDeviceHandle`, `OmegaGpuBufferHandle`, `OmegaGpuQueueHandle`,
  each `index + generation`; generation 0 is never issued; a closed handle is
  stale and refused; a handle of the wrong kind is refused.
- Addresses: `OmegaCpuView` (pointer + length), `OmegaGpuVa`, `OmegaGpuBusAddr`
  are three distinct struct types. No raw physical address type.
- Buffers: `OmegaGpuBufferDesc` carries kind (DATA, CODE, QUEUE), explicit
  `bytes`, `rights`, cache policy and alignment. Code is CPU-written and
  GPU-executed, never GPU-written; a queue is CPU-written and device-read; a
  host-polled word is CPU-read, GPU-written and GPU-uncached.
- Services: TIME, WAIT, LOCK, MMIO, DMA, SYNC, COMPLETION are required for
  v1; IRQ is optional and claimed only by a real interrupt wait.
  `omega_gpu_platform_check` refuses a claimed service without a function and
  refuses a function that no claimed service declares (a hidden path is a
  silent fallback).
- Limits and concurrency: `OmegaGpuPlatformCaps` carries device, buffer,
  queue, in-flight, per-buffer and total byte limits;
  `omega_gpu_platform_negotiate` refuses a required set the platform does not
  meet; `OmegaGpuHandleTable` enforces per-kind slot limits and byte totals.
- Refusals: thirteen named codes, no silent fallback to Linux, CPU or a
  remote device anywhere in the contract.

## 6. Next cuts

- A.2: allocator seam for `omega_blackwell_codegen.c` and
  `omega_blackwell_matmul.c` (caller-provided code buffers, self-test printf
  moved into the test), adding them to the freestanding target.
- B (needs an attended window, native v1 gate order N1 then N4): AIENOS-side
  discovery and bounded device ownership through the platform services; a
  discovery-only run makes zero configuration writes; SMMU/DMA resolved;
  polling does not claim IRQ. Implement aienos#285 before any domain-1
  resource number is reused or persisted. The arch#163 native-order conflict
  is resolved explicitly in that cut, not implied here.
- C: native backend of `OmegaGpuBackend` over `OmegaGpuPlatform`; the
  session, serving and API files drop `NvrmMem` and libc time/lock calls.
- D: vector executor (sha256 pin re-sealed on chip), numeric launchers,
  resident runtime.

Pre-existing, unrelated to this cut: `make test-gpu-serving-alloc` fails its
`bounds` mode on omega main 536870f1 ("in-bounds paged calls touched the
driver (1 allocs)"), reproduced on an untouched checkout on 2026-10-12. It
belongs to the matmul serving lane (cut I, omega#414), and is reported there.
