> Appendix to OMEGA_SYSTEMS_CORE_CODE_AUDIT.md. Worker report (Opus, read-only), aienos 8706fb8 (native/capability byte-identical to Omega lock 4c21386), physics fecbedb. Reviewed: spot-checked nvrm.c:440 function-static UVM params, m16_native.c:97-108 poll without acquire, aienos_capability.h:7-10,77 u64 generation (all confirmed). Correction to its OPEN item 14: the Makefile default AIENOS_R7_DIR=../aienos-r9 on this host has no native/capability directory (checked 2026-09-29), so local R7 builds need an override; the build does not enforce aienos.lock. Not authoritative on its own; Part II of the main document rules.

# OSC-0 / OSC-0B audit: AIENOS + PHYSICS C and assembly (read-only)

Sources: AIENOS snapshot @ 8706fb8 (`aienos-snap/`), PHYSICS snapshot @ fecbedb (`physics-snap/`),
Omega link points in `omega/.claude/worktrees/osc-0` (Makefile, `.github/workflows/rx-host.yml`,
`src/runtime/aienos_cap.h`, `src/runtime/rx_native_bind.c`, a few `src/` call sites),
aien-architecture @ 873025c (local HEAD, 2026-09-27; may lag origin).
Nothing was built, run or modified.

Labels: **[FACT]** code fact (file:line) · **[SETTLED]** decision already written down (doc cited) ·
**[PROPOSED]** recommended OSC-0B decision · **[OPEN]** unresolved.

Paths below are relative to the snapshot roots (`aienos-snap/…` = A:, `physics-snap/…` = P:, Omega worktree = O:).

---

## 1. Inventory

### 1.1 AIENOS C and assembly (non-vendored) — 1,268 lines total

| Path | Lines | Purpose | Class |
|---|---|---|---|
| A:native/capability/aienos_capability.c | 580 | Native capability authority (slot table, 64-bit generations, rights, delegation, cascade revoke, epoch, lease clock, restart). **Omega links it** as `libaienos_capability.a`. | Table logic (`state_*`, lines 20-356) = **B**; hosted shell (pthread, calloc, `/dev/urandom`, `CLOCK_BOOTTIME`, lines 358-580) = **E** |
| A:native/capability/aienos_capability.h | 133 | Public C ABI of the authority (constants, `AienosCapRef/Entry/Mint`, 19 functions) | **B** (C ABI boundary) |
| A:native/capability/tests/capability_test.c | 323 | Rule tests | B (test) |
| A:native/capability/Makefile | — | `-std=gnu11 -O2 -Wall -Wextra -Werror -fstack-protector-strong` | — |
| A:crates/aienos-artifact-tool/fixtures/p2_6/p26seed/probe.S | 131 | EL0 test probe: requests one capability, checks allow/deny via `svc` | **A/B** test fixture (keep as asm; it is a test payload) |
| A:…/p2_5/p25exec/probe.S | 83 | EL0 execution probe (handle array, forged handle denial) | test fixture |
| A:…/p2_5/p25wx/probe.S | 12 | W^X probe: store to own code page must fault | test fixture |
| A:…/p2_5/p25spin/probe.S | 6 | Runaway loop; only timer IRQ budget can end it | test fixture |

AIENOS Rust (to be rewritten under the no-Rust rule; line counts only), 57,841 lines in 14 crates:

| Crate | Lines | Purpose |
|---|---|---|
| aienos-kernel | 37,976 | Bare-metal no_std AArch64 kernel: vectors, GIC, timer, page tables, SMMUv3, DMA gate, NVMe, store, caps, scheduler |
| aienos-boot | 4,580 | UEFI entry / handoff |
| aienos-artifact | 2,505 | Binary Artifact v0 parser + identity |
| aienos-accel | 2,218 | GB10 PCI/BAR0 discovery |
| aienos-aegis | 1,757 | Capability graph / effect intent pipeline |
| aienos-evidence | 1,728 | Host evidence capture/verify |
| aienos-agent-state | 1,440 | Persistent agent state ABI |
| aienos-artifact-tool | 1,319 | Host artifact tooling |
| aienos-capability | 1,001 | Rust capability authority (u32 generations). Currently the declared differential oracle (its Cargo.toml description). **F [PROPOSED]** under the no-Rust rule once the C authority is the only authority |
| aienos-crypto | 922 | no_std crypto primitives |
| aienos-cortex | 728 | Cortex provenance store |
| aienos-c1-tree | 721 | COW prefix tree |
| aienos-store-tool | 477 | Store v1 image tool |
| aienos-capability-ffi | 469 | C binding for the Rust authority (**F**, replaced by native C) |

Vendored (list only): A:vendor/ — 58 Rust crates (~630k lines: syn, libc, uefi, uefi-raw, serde*, curve25519-dalek, ed25519-dalek, sha2, der, …). No vendored C.
P:third_party/nvidia-open-580.173.02 — NVIDIA open kernel module headers; Omega compiles against four of its include dirs (O:Makefile:10-13).

### 1.2 PHYSICS C and assembly (non-vendored) — 9,399 lines total

| Path | Lines | Purpose | Class |
|---|---|---|---|
| P:nvrm/nvrm.c | 810 | Native NVIDIA RM/UVM client: ioctls on `/dev/nvidiactl`, `/dev/nvidia0`, `/dev/nvidia-uvm`; memory alloc/free with GPU VA == CPU VA; channel group, GPFIFO, USERD, doorbell. **Omega compiles it into omegatool and R12/R-silicon tests.** | **D** (ioctl/mmap/doorbell) wrapped in **B** bookkeeping (live table, VA free list) |
| P:nvrm/nvrm.h | 119 | `Nvrm`, `NvrmMem`, `NvrmLiveAlloc`; 15 functions; `nvrm_mthd` inline | **D/B** C ABI |
| P:nvrm/lifecycle_gates.c | 350 | M19R hardware gates for nvrm_free / wait_marker_ge | test (E) |
| P:m16/m16_native.c | 112 | Thin wrapper: open, 64-slot legacy alloc table, channel + 4 KiB pushbuffer, semaphore-release method stream, marker poll | **C/D** |
| P:m16/m16_native.h | 54 | `M16NativeContext` (embeds whole `Nvrm`) | C ABI |
| P:m16/m16_concurrent.c | 135 | Two-channel causality test | test (E) |
| P:m16/m16_requalify.c | 132 | Single-channel causality test | test (E) |
| P:m15/physics_accel.c | 457 | **Software model** of SMMUv3 windows / queues / receipts (no MMIO, no ioctl) | **F** (or keep as spec only) |
| P:m15/physics_accel.h | 255 | Model structs: `EffectIntent` (64 B), `EffectReceipt` (192 B), `AcceleratorCapability` (u32 generation) | F / spec |
| P:m15/physics_accel_native.c/.h | 286 / 126 | sysfs probe of SMMU + GB10 (read-only files) + DMA "model" | F (sysfs probe = E) |
| P:m15/tools/m15tool.c | 701 | M15 host tool | E |
| P:tools/m2tool.c | 1,120 | M2 host qualification tool (sha256, hexfield, audit-verify, golden) | **E** |
| P:m3/tools/m3tool.c | 260 | M3 host tool | E |
| P:sha256_clean.c | 107 | Freestanding SHA-256 used by Atlas bootstrap image and host tools | **B** (keep) |
| P:physics.s | 336 | M2 PHYSICS entry: ingress validation, EL check, VBAR install, frame authority init, panic traps, polled PL011 UART | **C/D** |
| P:vector_table.s | 305 | M2 16-slot vector table + stackless trap capture → print → `wfe` halt | **D** |
| P:memory_alloc.s | 189 | M2 physical frame bitmap authority (grant only) | **C** |
| P:capability.s | 137 | M2 CAP_ROOT genesis + `validate_capability(ptr, op, addr, size)` | **C** |
| P:atlas_m2.s | 174 | Atlas M2 bootstrap seed: DAIF mask, descriptor, SHA-256 verify, `br` to PHYSICS | **E** (bootstrap) / D |
| P:m3/physics.s | 277 | M3 PHYSICS entry | C/D |
| P:m3/vector_table.s | 205 | M3 vectors (regressed vs M2, see §3a) | D (**F** once M2 collector is reused) |
| P:m3/capability.s | 533 | M3 32-slot capability ledger, u32 generation, derive/lookup/revoke, provenance SHA-256 | **C** |
| P:m3/memory_alloc.s | 222 | M3 frame bitmap: grant, grant-in-range, release | **C** |
| P:m3/effect_broker.s | 411 | M3 `submit_effect(intent_ptr)` 10-stage admission + receipt | **C** (semantics = A, should move up) |
| P:m3/receipt_ledger.s | 176 | Append-only 32-receipt hash chain | C |
| P:m3/replay_cache.s | 157 | 32-entry idempotency cache | C |
| P:m3/sha256_be.s | 85 | Big-endian SHA-256 in asm | C |
| P:m3/atlas_m3.s | 155 | Atlas M3 seed | E |
| P:m3/tests/test_m3_authority.s, sha256_kat.s, kat_stub.s | 843 / 163 / 7 | In-guest tests (sha256_kat enables SCTLR_EL1.A) | test |

Class totals (C + asm lines, excluding tests/fixtures, approximate):
- **A semantic**: ~0 standalone (effect_broker admission rules are semantic logic living in C-class asm).
- **B safe systems**: ~960 (capability table logic ~360 + header 133; sha256_clean 107; nvrm bookkeeping ~350).
- **C physical realization**: ~2,450 (M2/M3 capability, frame, broker, ledger, replay, sha256_be, entry; m16_native).
- **D unavoidable hardware boundary**: ~900 (nvrm ioctl/mmap/doorbell ~460, vector tables 510, UART/entry parts).
- **E temporary bootstrap / host tooling**: ~2,600 (atlas_m2/m3, m2tool, m3tool, m15tool, cap shell ~220, tests).
- **F obsolete / retire**: ~1,125 C (M15 model) + Rust aienos-capability(+ffi) 1,470.

---

## 2. Hazard counts (C files, non-vendored; asm listed separately)

| Hazard | Count | Representative sites |
|---|---|---|
| malloc/calloc/realloc/free | 78 (m2tool 42, m3tool 20, aienos_capability.c 15, physics_accel_native 1); **0 in nvrm/m16** | A:native/capability/aienos_capability.c:423-425 (3× calloc), :363 free in `release`, :514 malloc in restart; P:m3/tools/m3tool.c:48,53 |
| Raw pointer fields in structs | 8 | P:nvrm/nvrm.h:27 `void *cpu`, :44 `void *cpu`, :59 `void *usermode_cpu`, :60 `volatile uint32_t *doorbell`; A:…/aienos_capability.c:37,41 `CapShared *shared` (admin and view share one) |
| Pointer arithmetic | ~90 | P:nvrm/nvrm.c:285 doorbell = mmio + offset; :717 `fifo.cpu + userd_off + offsetof`; P:m15/physics_accel.c:382 ring slot; all PHYSICS asm uses base+shift addressing (m3/capability.s:147-149) |
| NULL / 0 sentinels | ~35 C + asm | NvrmMem all-zero == "freed" (nvrm.c:547); `MAP_FAILED` vs NULL mix (nvrm.c:162,284); asm `allocate_frame` returns 0 = exhausted (memory_alloc.s:112); `lookup_capability` returns 0 ptr (m3/capability.s:217); `AIENOS_CAP_PARENT_NONE = UINT32_MAX` (aienos_capability.h:69) |
| ptr+len conventions | 12 APIs | Good: `nvrm_enqueue(pb, off, nwords)` (nvrm.c:720) checks span; `sha256_compute(data, len, scratch[128], out[8])`. **Missing length**: `aienos_cap_authorize(admin, presented)` reads 32 B (aienos_capability.c:572-580); `aienos_cap_cognition_mint(view, req, token)`; `m16_native_build_release(words, …)` writes 8 words (m16_native.c:52-66); asm `validate_capability(cap_ptr, …)` (capability.s:92); `submit_effect(intent_ptr)` (m3/effect_broker.s:82) |
| Flexible array members | 0 | — |
| Unsafe casts | 52 (m2tool 28, nvrm.c 8, m3tool 5, others) | nvrm.c:285 `(volatile uint32_t *)((uint8_t *)mmio + …)`; nvrm.c:485 `mmap((void *)va…)` integer→pointer; nvrm.c:729 `(volatile uint64_t *)rm->fifo.cpu`; `NV_PTR_TO_NvP64(params)` pointer→u64 into kernel (nvrm.c:73, 270, 293); O:src/runtime/rx_native_bind.c:21 `(AienosCapEntry *)out` type pun |
| goto cleanup | 14 (nvrm.c 7, m2tool 7) | nvrm.c:688-706 `goto failed` in channel_destroy |
| Global mutable state | nvrm.c: 5 (`g_va_counter`, `g_va_slots_lock`, `g_va_recycled[1024]`, `g_va_recycled_count` :183-186, **function-local `static UVM_MAP_EXTERNAL_ALLOCATION_PARAMS ma`** :442); aienos_capability.c: 1 (`last_boot_gen` :52); PHYSICS asm: all state is at fixed physical addresses (0x40205800.., 0x40206000, 0x40207000, M3 0x40209000/0x4020A000/0x4020C000) |
| Arenas / pools | 5 | Fixed tables: `Nvrm.live[4096]`, `Nvrm.free_list[4096]` (nvrm.h:64-67), `M16NativeContext.legacy_allocs[64]` (m16_native.h:18), `CapState.entries[256]` (aienos_capability.c:24), M3 cap table 32×128 B / receipts 32×192 B / replay 32×64 B |
| GPU VAs / CPU addresses in structs | 9 | `NvrmMem.va` + `.cpu` (identical by construction, nvrm.h:25-28); `NvrmVaRange`; `Nvrm.va_next/va_slot_base/channel_va`; M15 `DmaWindowDescriptor.iova_base/phys_base`; M2/M3 cap records `bound_base/bound_size` (physical) |
| Generation counters / handle IDs | 8 schemes | C authority u64 (aienos_capability.h:73-76); M2 cap u32 never checked (capability.s:37-41); M3 u32 (m3/capability.s:403-408); M15 u32 (physics_accel.h:88); RM handles `next_handle++` u32 from 0xcf000001, refuse at UINT32_MAX (nvrm.c:65-72); GPFIFO `put/retired` u32 wrap (nvrm.h:63-64); marker serial compare (m16_native.c:103); Rust caps u32 (kernel caps.rs:76, abi.rs:141) |
| Rights fields | 4 | `AIENOS_CAP_RIGHT_*` 10 bits (aienos_capability.h:43-60); M2/M3 `allowed_ops` u32 (capability.s:20); M15 `ACCEL_OP_*`; Omega `RX_RIGHT_*` re-declared (O:src/runtime/rx_caproot.h:67-80) |
| Stale-handle validation | 5 sites | aienos_capability.c:135-138, 143-167, 204; m3/capability.s:153-157, 183-195; nvrm_free live-table match (nvrm.c:553-563) |
| Serialization of native structs | 4 | M15 hashes raw `EffectIntent`/`EffectReceipt` bytes (physics_accel.c:103, 119-121); M3 broker builds receipt at fixed offsets (effect_broker.s:66-79); M3 derive hashes 80 B built at fixed offsets (m3/capability.s:412-436); `AienosCapEntry` copied across C ABI by value/pointer |
| MMIO | 3 | Usermode doorbell `*rm->doorbell = token` (nvrm.c:742-744); USERD GPPut store (nvrm.c:733); PL011 UART at 0x09000000 (physics.s:249-268) |
| ioctl / mmap | 19 in nvrm.c | nvrm.c:59 (all RM escapes), 145 (UVM), 177 (mmap RM memory), 485 (PROT_NONE reservation), 596 munmap |
| Page-table / SMMU code | 0 in C/asm | Only in Rust kernel (smmu.rs 1,392 lines, pagetable.rs 658) and M15 model. PHYSICS M2/M3 run with MMU off. |
| DMA code | GPU only | Coherent sysmem via RM `NV01_MEMORY_SYSTEM` + `RM_MAP_MEMORY_DMA` with `CACHE_SNOOP_ENABLE` (nvrm.c:434) + UVM external range (nvrm.c:417-450) |
| `_Static_assert` | **0 in AIENOS/PHYSICS**; 4 in Omega's shim (O:rx_native_bind.c:10-16) | — |
| packed / `__attribute__` layout | 0 packed; 3 `aligned(16)` scratch buffers | — |

---

## 3. Machine-semantics evidence

### (a) Interrupts, vectors, interrupt context
- [FACT] PHYSICS M2 has **no interrupt handling**: Atlas masks DAIF (`msr daifset,#0xf`, P:atlas_m2.s:56) and nothing unmasks it. All 16 vectors (P:vector_table.s:35-130) store the slot in x0 (after stashing x0 in TPIDR_EL1) and branch to `common_trap_entry`, which saves all GPRs + SP + ESR/ELR/SPSR/FAR to a fixed frame at 0x40205880 **without touching the stack**, writes magic `TRAPFRM1` last, `dsb sy`, switches to a known stack, prints, then `wfe` forever (:161-268). Every exception is terminal. Nothing in exception context can allocate (there is no allocator call) or block (it halts by design).
- [FACT] PHYSICS M3 vectors (P:m3/vector_table.s:32-35, 116-140) put the slot in x0 **before** saving x0 and load the frame base into x1 **before** saving x1 → original x0/x1 are lost; FAR is marked valid for EC 0x26 (SP alignment, FAR not architecturally valid) and FnV is not checked. M3 is a regression from M2.
- [FACT] The collector overwrites TPIDR_EL0/EL1 (vector_table.s:37,162); a nested fault overwrites the single frame. Acceptable because it never returns.
- [FACT] AIENOS C has no interrupt code. The AIENOS kernel (Rust) takes IRQs: `irq_dispatcher_inner` acknowledges GIC, handles only PPI 30 (timer) by reprogramming compare and calling a hook transmuted from an `AtomicUsize` (A:crates/aienos-kernel/src/thread.rs:229-248, fatal.rs:169-183). No allocation in that path.
- [SETTLED] aien-architecture ADR 0014 (873025c): AIENOS owns "boot continuation, address spaces, tasks, interrupts, capabilities, DMA confinement … native device ownership"; FORGE (ex-PHYSICS) is realization only.

### (b) MMIO / volatile / barriers / cache / DMA / page tables / SMMU
- [FACT] GPU submission ordering: ring entry written via `volatile uint64_t*`, `dsb sy`, GPPut store, `dsb sy` (P:nvrm/nvrm.c:729-734); pushbuffer copied then `dsb sy` (m16_native.c:72-73). Doorbell store `*rm->doorbell = token` has **no barrier of its own and no NULL check** (nvrm.c:742-744); it relies on the trailing `dsb` in enqueue. `dsb sy` is stronger than needed (`dmb oshst`/`dsb st` would do) but correct.
- [FACT] Completion observation: `m16_native_wait_marker_ge` polls a `volatile uint32_t*` with `usleep(50)` + `yield` and **no acquire barrier** after success (m16_native.c:97-108). Subsequent reads of GPU-written results are not ordered after the marker read on AArch64.
- [FACT] No `dc`/`ic` cache maintenance anywhere in PHYSICS or AIENOS C. GPU memory is coherent system memory with `NVOS46_FLAGS_CACHE_SNOOP_ENABLE` (nvrm.c:434) and optionally GPU-uncached (`nvrm_alloc_gpu_uncached`, nvrm.c:473). PHYSICS M2/M3 run MMU-off (no MMU/TTBR/SCTLR.M code; only the KAT test sets SCTLR_EL1.A, m3/tests/sha256_kat.s:52-54), so all data accesses are Device-nGnRnE.
- [FACT] Atlas hands off with `br x19` and no `ic iallu`/`dsb`/`isb` (P:atlas_m2.s:130-134). Fine with caches off under QEMU; not fine natively once I-cache is on.
- [FACT] No page table, SMMU or IOMMU programming in any C/asm. SMMU on GB10 is owned by Linux (P:evidence/physics_accelerator_link_qualification_receipt.json:38 "takeover deferred"). The Rust kernel has host-testable SMMUv3 bring-up (GBPA.ABORT first, abort STEs by default, A:crates/aienos-kernel/src/smmu.rs:1-12) and a BME gate (dma_gate.rs:1-20), plus `dc cvac`/`dc civac` + `dsb` helpers (arch/aarch64.rs:262-337).
- [FACT] GPU VA == CPU VA by construction: PROT_NONE `MAP_FIXED_NOREPLACE` reservation at the chosen VA (nvrm.c:485-489), then `MAP_FIXED` RM mapping over it (nvrm.c:177), then `RM_MAP_MEMORY_DMA` with `DMA_OFFSET_FIXED` at the same VA (nvrm.c:433-437) and UVM external range (nvrm.c:419-450).
- [OPEN] That RM keeps the physical pages pinned for the life of the RM memory object is assumed (NVIDIA RM contract), not verified in the snapshot.

### (c) Atomics and memory orderings
- [FACT] aienos_capability.c: `last_boot_gen` CAS acq_rel/relaxed (:66-78); `holders` refcount `fetch_sub` acq_rel (:361); all table state under `pthread_mutex` (LOCKED macro, :369-375, a GNU statement expression).
- [FACT] nvrm.c: `atomic_flag` spinlock acquire/release around `g_va_recycled` (:189-209); `g_va_counter` CAS relaxed (:193-200). `Nvrm` itself has **no lock**; each context is single-threaded by convention. The function-local `static UVM_MAP_EXTERNAL_ALLOCATION_PARAMS ma` (:442) is shared by every `Nvrm` → **data race** when two contexts allocate concurrently (m16_concurrent.c and Omega's threaded GPU seat O:src/runtime/rx_resident_gpu.c:47-60 do run multiple contexts).
- [FACT] M15 `atomic_thread_fence(seq_cst)` (physics_accel.c:386) orders ordinary struct memory in a software model; no device visibility effect.
- [FACT] PHYSICS asm: zero exclusives/atomics (no ldxr/stxr/ldar/stlr/cas); single boot CPU. [SETTLED] P:docs/native-frame-authority.md §6.5: boot CPU alone owns the frame authority in v1; one lock later, no per-CPU cache.
- [FACT] Rust kernel uses Release 50×, Acquire 31×, SeqCst 14×, Relaxed 13×, AcqRel 1×.

### (d) Layout, hardware formats, endianness, hashing
- [FACT] No `packed` structs and **zero `_Static_assert`** in AIENOS/PHYSICS C. `AienosCapRef` {u32, u64} has 4 bytes of interior padding (size 16); `AienosCapEntry` has padding after `cap_id`, `parent_id`, `minted_by_id`. Omega re-declares both structs by hand (O:src/runtime/aienos_cap.h:18-47) and checks only size + two offsets (O:rx_native_bind.c:10-16).
- [FACT] M15 hashes raw struct bytes (`sizeof(EffectIntent)`, first 128 B of `EffectReceipt`, physics_accel.c:103, 119-121). By hand both are padding-free (64 B and 192 B) on LP64, but nothing enforces it. Receipts copy `out_receipt` including `length = sizeof(EffectReceipt)`.
- [FACT] Hardware formats done with shifts, not bitfields: GP entry (nvrm.c:712-714), method header `nvrm_mthd` (nvrm.h:111-113, `count` not masked to 13 bits), semaphore release stream (m16_native.c:56-65). Stores are native little-endian; correct only because host and GPU are both LE.
- [FACT] PHYSICS asm records and hash inputs are built at explicit byte offsets, little-endian (m3/capability.s:412-429; m3/effect_broker.s:66-79). `sha256_clean.c` writes the length big-endian explicitly (:91-95), reads input bytewise; output is native u32 words (callers convert). Contract declares `"endianness": "little"` (P:machine_contract.json).
- [SETTLED] AIENOS ADR 0013 (ABI v1) and ADR 0015 (Store v1) define explicit LE wire layouts with offsets (e.g. adr/0015:92-93, adr/0013:57).

### (e) Failure handling
- [FACT] PHYSICS M2/M3 ingress and invariant failures are **fail-closed panic traps**: distinct string then `wfe` loop (P:physics.s:193-241). Frame exhaustion returns 0 (not a panic, memory_alloc.s:112, 158). Invalid capability returns 0/decision code (capability.s:125; m3/capability.s DEC_* codes). Any CPU exception = capture + halt.
- [FACT] AIENOS C authority: every failure is a negative error code (17 codes, aienos_capability.h:27-43); no abort/assert/exit anywhere in aienos_capability.c, nvrm.c or m16_native.c. OOM in `aienos_cap_start/restart` → `AIENOS_CAP_ERR_IO` (:426-430, :515).
- [FACT] nvrm: error code −1 + message in `rm->err[256]` + **sticky `rm->faulted`**; after any irreversible partial failure the allocation is quarantined (VA never reused) (nvrm.c:453-460, 575-600). Device loss has no dedicated path: the channel error notifier is allocated and registered (`hObjectError`, nvrm.c:629, 635) but **never read** by PHYSICS or Omega; a lost/faulted GPU shows up only as a marker-poll timeout (m16_native.c:102-108).
- [SETTLED] aien-architecture doctrine/TRUST.md:260 lists "OOM panic, core" under catastrophic failure; ADR 0006 (AIENOS) requires deterministic crash records.

### (f) Capability authority generation / rights model
- [FACT] C authority (the one Omega links): reference = `{u32 cap_id, u64 generation}`; 256 slots; max delegation depth 8. Reclaim advances generation by 1 (`aienos_cap_generation_advance`, :80-84); a slot at `UINT64_MAX` is skipped as exhausted, never wrapped (:262-266). Restart starts every slot above `max(last start + 1, table_floor, CLOCK_BOOTTIME_ns << 8)` so references from an earlier table or process fail (:45-79, 97-107, 507-527).
- [FACT] Stale references fail closed: wrong generation → `ERR_STALE_GEN`; non-live → `ERR_REVOKED`; epoch mismatch → `ERR_EPOCH`; lease expired → `ERR_EXPIRED`; any ancestor stale/revoked/expired/wrong-epoch → `ERR_CHAIN` (:143-167). Revoke cascades to live descendants (:286-303). Admin ops additionally require `delivered[i]` (:201-215).
- [FACT] Rights: 10 bits (READ, WRITE, EFFECT, DELEGATE, MINT, REVOKE, RECLAIM, EPOCH, CLOCK, PROMOTE); privileged rights can never be delegated (:227-229, 249); delegation can only narrow rights, same resource, shorter-or-equal lease (:240-254).
- [FACT] Gaps: `restart` resets `writer_alive = true`, undoing `kill` (bootstrap :113); `aienos_cap_force_generation` is a test seam exported in the production ABI (:343-351); the office token is never checked by any operation (:572-580, header comment); `cognition_*` are hard-coded refusals (:540-557).
- [FACT] PHYSICS M3: `{u32 slot, u32 generation}`, 32 slots; on wrap to 0 the slot is skipped (fail closed, m3/capability.s:403-408); revoke does **not** cascade but lookup walks ancestors (≤8) on every use (:167-195); `lookup_capability` returns a **raw pointer** into the table (:211).
- [FACT] PHYSICS M2: `validate_capability` takes a record pointer, checks state/ops/bounds with overflow check, **never checks generation** (capability.s:92-127).
- **No coupling between capability revocation and device views** [FACT]: revoking a capability in the C authority does not unmap any GPU VA, drain any channel, or invalidate any SMMU/TLB entry. `nvrm_free` does not check that the GPU has finished with the buffer, and `nvrm_channel_destroy` does not check `put == retired` before unregistering and freeing (nvrm.c:674-706). The only fence-like guard is `nvrm_enqueue` refusing to overrun unretired ring entries, and `nvrm_retire` trusts the caller (nvrm.c:726-740). Omega calls `nvrm_retire(&rm, rm.put)` directly (O:src/omega_accelerator_world.c:984, 1204).

---

## 4. Per-site hazard table (top 30)

repository/path | function/type | hazard | current ownership model | current lifetime model | proposed Omega replacement | migration difficulty | required compatibility boundary
---|---|---|---|---|---|---|---
physics/nvrm/nvrm.c:543-612 | `nvrm_free` | Frees RM memory + unmaps while GPU work may still reference it; other `NvrmMem` copies dangle | Caller holds `NvrmMem` by value; `Nvrm.live[]` is the real owner | Manual; "freed" = all-zero struct; no GPU fence | `handle<DeviceMem>` owned by an `arena/region` per channel; free requires a completion token (`own<Fence>`) | High | RM/UVM ioctl ABI (C ABI adapter)
physics/nvrm/nvrm.c:674-706 | `nvrm_channel_destroy` | Tears down channel with in-flight GP entries (no `put==retired` check) | `Nvrm` owns channel handles | Manual, ordered frees with `goto failed` | `own<Channel>` whose drop requires drained state; typestate | Medium | RM free order (C ABI adapter)
physics/nvrm/nvrm.c:442 | `uvm_map` `static … ma` | Hidden global shared by all contexts → data race | Function-local static | Process lifetime | Stack `T inline` (it is ~ a few KB) or per-context field | Low | none
physics/nvrm/nvrm.c:726-740 | `nvrm_retire` | Trusts caller that entries completed; no GPGet | Caller asserts completion | Monotonic u32 counters, wrap by subtraction | `own<Fence>` produced only by observed marker; retire consumes it | Medium | Omega calls (omega_accelerator_world.c:984)
physics/m16/m16_native.c:97-108 | `m16_native_wait_marker_ge` | Volatile poll with no acquire barrier; data read after may be stale | Borrowed `volatile u32*` | Caller-guaranteed | `device<u32>` with `load_acquire`; returns `own<Fence>` | Low | none
physics/nvrm/nvrm.c:742-744 | `nvrm_ring` | MMIO store with no NULL check, no own barrier | `Nvrm.doorbell` raw ptr into RM mmap | Lives until `munmap(usermode_cpu)` in close | `device<Doorbell>` (MMIO typed handle), `unsafe physical block` for the store | Low | usermode MMIO mapping
physics/m16/m16_native.c:69-75 | `m16_native_enqueue_methods` | Rewrites single 4 KiB pushbuffer at offset 0 while earlier entries may be unconsumed | `M16NativeContext.pb_mem` | Reused every submit | `arena/region` ring of pushbuffer slices tied to GP entries; `slice<u32>` | Medium | GPFIFO entry format
physics/nvrm/nvrm.h:24-29 | `NvrmMem` | Copyable struct with raw `cpu` pointer + VA; aliasing and dangling copies | Value type, no owner | None | `handle<DeviceMem>` (gen-checked) + `&[u8]`/`slice<T>` view borrowed from it | Medium | Omega struct field access
physics/nvrm/nvrm.h:40-84 | `Nvrm` (~225 KiB) | Huge, stack-allocated by Omega (omega_blackwell_submit.c:44); fields read directly by Omega | Caller-owned storage | Scope of caller | `own<Nvrm>` heap/region; opaque type with accessors | Medium | Omega reads `.put/.retired/.faulted/.entries/.root/.device/.vaspace`
physics/nvrm/nvrm.c:161-181, 485 | `map_to_cpu`, `alloc_with_cacheability` | int→pointer casts, `MAP_FIXED` over reservation | Kernel mapping owned by `Nvrm.live[]` | Until `munmap` | `pin<DeviceMem>` (pinned, identity VA) produced by an `unsafe physical block` | High | Linux mmap + RM ABI
physics/nvrm/nvrm.c:64-80 | `rm_alloc` | `NV_PTR_TO_NvP64(params)` passes user pointer to kernel as u64 | Stack param blocks | Duration of ioctl | C ABI adapter taking `&mut T` | Low | NVIDIA ioctl structs (third_party headers)
physics/nvrm/nvrm.c:183-209 | VA slot globals | Process-global mutable state + spinlock; recycled list silently drops when >1024 | Process | Process | `arena/region` VA allocator object owned by a device-session root | Low | none
physics/nvrm/nvrm.c:629-635 | channel error notifier | Allocated, never read → device fault/loss invisible | `Nvrm.notifier` | Channel | `device<Notifier>` checked on every wait; explicit `DeviceLost` error | Medium | RM notifier layout
physics/nvrm/nvrm.h:111-113 | `nvrm_mthd` | `count` not masked; overflows into opcode bits | pure fn | — | `T inline` bitfield type with checked constructor | Low | method header format
physics/m16/m16_native.c:52-66 | `m16_native_build_release` | Writes 8 words to buffer of unknown length | Caller buffer | — | returns `[u32; 8]` `T inline` or takes `&mut slice<u32>` | Low | none
physics/m16/m16_native.c:22-42 | legacy alloc table | Lookup by CPU pointer; 64-slot linear | Context-owned | Manual | `handle<DeviceMem>` | Low | Omega does not use it directly
aienos/native/capability/aienos_capability.c:420-449, 452-462 | `aienos_cap_start/stop` | Two heap handles share a refcounted `CapShared`; use after `stop` is UAF; double stop double-frees | Refcount (`holders`) | Manual `stop` | `own<Admin>` + `own<View>` sharing `Rc`-like region; or `SemanticId`-keyed singleton in a region | Medium | Omega calls `aienos_cap_stop` 27×
aienos/native/capability/aienos_capability.c:572-580 | `aienos_cap_authorize` | Reads 32 bytes from pointer with no length | Borrowed | Call | `&[u8; 32]` | Low | C ABI (unused by Omega)
aienos/native/capability/aienos_capability.c:343-351, 558-561 | `aienos_cap_force_generation` | Test seam exported in production ABI | Admin | — | Remove from production; test-only build | Low | 2 call sites in Omega src+tests
aienos/native/capability/aienos_capability.c:111-128 | `bootstrap` (via restart) | `restart` revives a killed writer (`writer_alive=true`) | Admin | — | Typestate: killed authority cannot restart without explicit revival right | Low | behaviour change visible to R-gates
aienos/native/capability/aienos_capability.c:369-375 | `LOCKED` macro | GNU statement expression; lock not held across multi-call sequences (validate then use = TOCTOU) | pthread mutex | Per call | `&T` view under a region lock; validated ref returns `handle<Cap>` checked again at use | Medium | Omega rx_world validate→act pattern
omega/src/runtime/aienos_cap.h:18-47 + rx_native_bind.c:21 | header copy + `(AienosCapEntry *)out` | Hand-copied structs, no constants, padding-dependent, type pun between `RxCapEntry` and `AienosCapEntry` | Omega copy | Build | Single generated header; `SemanticId<Cap>` = `{u32 idx, u64 gen}` with explicit layout + asserts | Low | the C ABI itself
physics/m3/capability.s:139-211 | `lookup_capability` | Returns raw pointer into cap table; caller may keep it past revoke | Table owns records | Until slot reused | `handle<Cap>` returned; record accessed via `&T` scoped borrow | Medium (asm) | M3 gate receipts
physics/m3/capability.s:403-408 | derive generation | u32 generation; slot retired on wrap | Table | Fixed | u64 generation `SemanticId<Cap>` | Medium | M3 receipts/hashes include u32 gen
physics/capability.s:92-127 | `validate_capability` (M2) | Takes record pointer, no generation check | Caller pointer | — | `handle<Cap>` lookup | Low (M2 frozen) | M2 byte-identical image
physics/m3/memory_alloc.s:185-222 | `release_frame` | Releases any allocated frame without capability or DMA-quiescence proof (contradicts doc §6.4) | Bitmap | Manual | `own<Frame>` consumed by release together with `own<DmaQuiesced>` receipt | Medium | frame-authority gates
physics/m3/effect_broker.s:82-135 | `submit_effect(intent_ptr)` | Pointer only, alignment checked, no range check against caller memory | Caller | Call | `&EffectIntent` (64 B `T inline`, explicit LE layout) | Medium | receipt format
physics/m3/vector_table.s:32-35, 116-140 | `common_trap` | Clobbers x0/x1 before save; FAR validity wrong for EC 0x26 | Fixed frame | Terminal | Reuse M2 collector (`unsafe physical block`) | Low | M3 gate expectations
physics/m15/physics_accel.c:85-127, 343-392 | receipt seal, `submit_command` | Hash over native struct bytes; ring tail overwrites without head check | Link struct | Manual | Explicit serializer to `[u8; N]`; model code retired | Low (retire) | none (not linked)
physics/atlas_m2.s:130-134 | handoff `br x19` | No `ic iallu; dsb; isb` before jumping to loaded image | Atlas | One-shot | `unsafe physical block` with mandatory I-cache sync | Low | Atlas byte-identical image (M2 frozen)

---

## 5. C ABI dependencies that cannot immediately disappear

**Omega → AIENOS capability authority** [FACT] O:Makefile:184-197 (and 210, 224, 242, 256, 282, 310, 329, 351): links `$(AIENOS_R7_DIR)/native/capability/out/libaienos_capability.a` (default dir `../aienos-r9`; CI checks out the commit in `aienos.lock` = 4c21386, O:.github/workflows/rx-host.yml:38-58). Header: Omega's own copy `src/runtime/aienos_cap.h` (no constants). Calls (count in src+tests): office 49, mint 30, stop 27, start 23, revoke 22, validate 15, inspect 7, clock 5, advance_clock 5, reclaim 4, cognition_mint 4, restart 3, cognition_admin 3, bump_epoch 3, kill 2, generation_advance 2, force_generation 2.
Pointer passing and implied ownership:
- `start(Admin **, View **)`: library allocates both; caller owns them and must call `stop` exactly once each (no double stop, no use after).
- `mint(Admin *, const Mint *, Ref *out)`, `validate/inspect(View *, Ref, …, Entry *out)`: borrowed in/out pointers for the call only; entries are copied out by value (no pointer into the table escapes — good).
- `View *` is handed to the reaction world as an opaque `void *ctx` (O:rx_native_bind.c:28-33); the world must not outlive the view.
- `authorize/cognition_mint(…, const uint8_t *token)`: 32-byte implied length.
- Layout contract: `AienosCapRef` (16 B with padding), `AienosCapEntry`, `AienosCapMint` passed by value/pointer; rights and error numbers are duplicated as literals on the Omega side (`RX_RIGHT_*`, O:src/runtime/rx_caproot.h:67-80).

**Omega → PHYSICS NVRM/M16** [FACT] O:Makefile:3-24, 71-75, 316-331, 357: compiles `$(PHYSICS_DIR)/m16/m16_native.c` and `nvrm/nvrm.c` into omegatool and GPU tests; include paths `-I physics/m16 -I physics/nvrm` + four NVIDIA open-580.173.02 header dirs; pinned by `physics.lock` = fecbedb and a Makefile check (O:Makefile:47-60). Calls: nvrm_mthd 114, m16_native_close 56, nvrm_alloc 35, m16_native_submit_methods 6, wait_marker_ge 5, open 5, create_channel 5, nvrm_retire 4, wait_marker 3, nvrm_free 2, nvrm_enqueue 2, nvrm_channel/destroy 2 each, nvrm_ring 1, nvrm_alloc_gpu_uncached 1.
Pointer passing and implied ownership:
- `M16NativeContext *`/`Nvrm *`: caller-provided storage (stack or struct field); library fills it; caller must `close` exactly once.
- `nvrm_alloc(Nvrm *, size, NvrmMem *out)`: out is a *copy* of an identity; the `Nvrm` owns the memory; `cpu` pointer valid until `nvrm_free`/`nvrm_close`.
- `nvrm_enqueue(Nvrm *, const NvrmMem *pb, off, nwords)`: GPU reads `pb` asynchronously after the call returns → caller must keep `pb` alive and unmodified until a completion is observed (not enforced).
- `m16_native_wait_marker_ge(volatile uint32_t *marker, …)`: marker must point into a live coherent allocation.
- **De facto ABI beyond functions**: Omega reads `Nvrm` internals directly (`.entries`, `.faulted`, `.put`, `.retired`, `.root`, `.device`, `.vaspace`; O:src/omega_accelerator_world.c:112,159,670,732,984; omega_world_gates.c:122-124). The struct layout is part of the boundary.
- Below that, the NVIDIA RM/UVM ioctl ABI (driver 580.173.02 headers) is an unavoidable class-D boundary until AIENOS owns the GPU natively.

**Not linked by Omega** [FACT]: PHYSICS M2/M3 asm images (QEMU-only, separate binaries), M15 model, tools.

---

## 6. Existing good patterns worth keeping

- [FACT] M2 stackless trap collector: saves every GPR before reuse, magic written last, `dsb sy`, then switches to a known stack (P:vector_table.s:161-237).
- [FACT] Fail-closed ingress with distinct panic strings, no state written before validation (P:physics.s:21-124, 193-241).
- [FACT] Frame allocator overflow guard and "0 is never a frame" sentinel documented (P:memory_alloc.s:40-50, 107-113).
- [FACT] M2 `validate_capability` address+size wrap check (P:capability.s:108-112); M3 generation wrap retires the slot (m3/capability.s:403-408).
- [FACT] C authority: 64-bit never-wrap generations, restart floor above every previously used generation + boot-time seed (A:…/aienos_capability.c:45-79, 97-107); ancestor chain checked on every use (:143-167); privileged rights non-delegable (:227-229); `delivered[]` gating (:204); checked add for clock/epoch/lease (:86-90); results copied out, no interior pointers escape (:529-547); constant-time token compare (:572-580).
- [FACT] nvrm_free staged release with per-stage flags; quarantine VA on any partial failure; stale/duplicate `NvrmMem` detection without touching the driver (P:nvrm/nvrm.c:543-612, 453-460).
- [FACT] `MAP_FIXED_NOREPLACE` PROT_NONE reservation before mapping device memory at a fixed VA (nvrm.c:485-489).
- [FACT] Sticky `faulted` bit that blocks further alloc/enqueue after unknown driver state (nvrm.c:478, 721).
- [FACT] `nvrm_enqueue` span + overflow checks and "never overrun unretired entries" (nvrm.c:720-728); RFC 1982 serial compare for markers (m16_native.c:103).
- [FACT] RM handles never reused within a client; refuse at `UINT32_MAX` (nvrm.c:65-72).
- [FACT] Explicit big-endian length encoding and caller-provided scratch in `sha256_clean.c:88-103` (no hidden static state; freestanding).
- [FACT] Rust kernel design rules worth carrying into the rewrite: SMMU GBPA.ABORT first and never cleared, abort STE default (smmu.rs:8-12); BME only after `dma_grant` (dma_gate.rs:1-20).

---

## 7. OSC-0B items (hardware/kernel side)

**1. Interrupt + device concurrency and publication**
- [FACT] PHYSICS: no interrupts; single CPU; all exceptions terminal (§3a).
- [FACT] GPU publication = ring write → `dsb sy` → GPPut → `dsb sy` → doorbell; completion = volatile poll without acquire (§3b).
- [FACT] Revocation while a device holds a view: nothing enforces it. Capability revoke is purely table state; `nvrm_free`/`nvrm_channel_destroy` do not wait for the GPU; there is no SMMU/TLBI step in C (§3f).
- [SETTLED] P:docs/native-frame-authority.md §6.4: release requires the revoked capability plus a receipt showing all SMMU mappings removed and `CMD_TLBI_*`+`CMD_SYNC` completed. Not implemented anywhere.
- [PROPOSED] Revocation is two-phase: (1) revoke = no new use (table state, immediate); (2) reclaim of any memory/VA/frame requires an `own<Quiesced>` token proving the device drained (fence observed or channel torn down) and, natively, SMMU unmap + TLBI + sync. Reclaim without the token is a type error, not a runtime check.
**2. Atomics / barriers / ordering** — [PROPOSED] Omega exposes only: `load_acquire/store_release` on `device<T>`, `dmb ish/oshst/oshld`, `dsb`, `isb`, and seq-cst RMW; no plain volatile in safe code; every device-completion read is acquire.
**3. Device layout / wire formats** — [PROPOSED] Every struct that crosses a device, a hash, a receipt or the C ABI is declared with explicit size, offsets and LE encoding and compiled with generated `_Static_assert`s; hashes are over a serializer output, never over native struct bytes.
**4. Failure semantics** — [FACT] PHYSICS: panic-trap for ingress/invariant, 0 for exhaustion; C authority and nvrm: error codes, no aborts; nvrm sticky `faulted`; device loss undetected except by timeout. [PROPOSED] Kernel/interrupt context: invariant violation → trap to a stackless capture + halt (M2 pattern); OOM/exhaustion → error value, never a panic; invalid/stale capability → error value (fail closed); device loss → explicit `DeviceLost` error, sticky per device session, sourced from the channel error notifier.
**7. Pinning / memory that must not move** — [FACT] Must not move: PHYSICS fixed regions (vector table 2 KiB-aligned 0x40201000, boot state, trap frame 0x40205880, cap table, bitmap; P:machine_contract.json disjoint_regions); GPU allocations at identity VA (nvrm.c:485); GPFIFO + USERD in one allocation at `entries*8` (nvrm.c:627-640); markers polled by resident GPU programs. [PROPOSED] These are `pin<T>`/`device<T>` objects created only in `unsafe physical block`s; safe code gets `slice<T>` borrows whose lifetime is bounded by the pin.
**8. MMIO / DMA / volatile / cache / SMMU / no-block contexts** — [FACT] MMIO: PL011 UART (polled busy-wait on TXFF, physics.s:252-257 — blocks in trap path by design), GPU usermode doorbell. DMA: GPU coherent snooped sysmem only. Cache ops: none in C/asm. SMMU: Linux-owned for now. No-block contexts: PHYSICS trap path (halts), Rust IRQ path. [OPEN] whether native GB10 sysmem is coherent for all GPU engines (P:docs/native-frame-authority.md §9 says "assumed").
**12. Authority model** — [FACT] see §3f. Main objects:

| Object | Who keeps it alive | Who may touch | Where it lives |
|---|---|---|---|
| C capability table (`CapShared`) | refcount of Admin+View handles | Admin: mutate; View: validate/inspect; cognition: nothing | heap (calloc) in the Omega process |
| `Nvrm` / `M16NativeContext` | the Omega caller (stack or struct) | single owning thread by convention (no lock) | Omega stack/struct (~225 KiB) |
| GPU memory (`NvrmMem`) | `Nvrm.live[]` + RM object | anyone holding a copy of the pointer | Linux-RM sysmem at identity VA (pinning assumed, [OPEN]) |
| GPFIFO / USERD / doorbell | `Nvrm` | `nvrm_enqueue`/`nvrm_ring`, Omega reads counters directly | RM sysmem / usermode MMIO |
| PHYSICS cap table, bitmap, ledgers | the PHYSICS image forever | PHYSICS code only (MMU off, EL1, single CPU) | fixed physical addresses per contract |

---

## 8. Contradictions (docs vs code, code vs code)

1. **Generation width.** C authority: u64, never wraps, EXHAUSTED at max (A:native/capability/aienos_capability.h:8-10, .c:80-84). Omega header: u64 (O:src/runtime/aienos_cap.h:7-8). AIENOS ADR 0013 + Rust kernel: u32, retire at `u32::MAX` (A:docs/adr/0013-aienos-abi-v1.md:57, 83-94; A:crates/aienos-kernel/src/caps.rs:76, abi.rs:141). Rust aienos-capability crate: u32 (src lib :86-87). PHYSICS M3: u32 retire on wrap (m3/capability.s:403-408). M2: u32, unchecked. M15: u32 (physics_accel.h:88).
2. **Who owns authority.** PHYSICS README "Trusted Machine Authority … PHYSICS AUTHORIZES", governor of SMMUv3/devices (P:README.md:1-40). aien-architecture ADR 0013/0014 (873025c): PHYSICS→FORGE is realization only, "not a gatekeeper"; AIENOS owns interrupts, capabilities, DMA confinement (docs/adr/0014:5-10; doctrine/SOVEREIGNTY.md:383). The M3 capability ledger and effect broker in PHYSICS asm are authority code under the old model.
3. **Release requires DMA quiescence** (P:docs/native-frame-authority.md §6.4) vs M3 `release_frame` checks only the bitmap bit (P:m3/memory_alloc.s:185-222) and nvrm_free has no GPU completion check.
4. **README claims SMMUv3 DMA protection** (P:README.md:38) vs no SMMU code in PHYSICS; evidence says Linux programs the SMMU, "takeover deferred" (P:evidence/physics_accelerator_link_qualification_receipt.json:38-39).
5. **M2 cap comment vs code**: header says `resource_type = 0xFFFFFFFF` (P:capability.s:24) but code stores 1 (:47-49); contract says 1.
6. **Decision codes**: `DEC_REJECTED` and `DEC_REJECTED_STRUCTURAL` are both 2 (P:m15/physics_accel.h:33-35).
7. **"No Rust"** in A:native/capability/Makefile:1 vs aienos-capability + aienos-capability-ffi still workspace members (A:Cargo.toml:4-5).
8. **Omega pin vs snapshot**: Omega `aienos.lock` = 4c21386 and Makefile default dir `../aienos-r9`; this audit's snapshot is 8706fb8. [OPEN] which authority commit is actually linked in local builds.
9. **Header drift**: Omega's `aienos_cap.h` omits every constant; Omega re-declares rights (`RX_RIGHT_*`) with no compile-time tie to `AIENOS_CAP_RIGHT_*`.
10. **M3 vs M2 trap collector**: M2 preserves x0/x1 and checks FnV; M3 does neither (§3a).
11. **Name vs behaviour**: `m16_native_wait_marker` (equals) calls `_ge` (m16_native.c:93-95).
12. `nvrm.h` declares `nvrm_alloc_gpu_uncached` after `#endif` (P:nvrm/nvrm.h:117-119).

---

## 9. Recommended OSC-0B decisions

1. [PROPOSED] **One capability identity**: `SemanticId<Cap> = { u32 index, u64 generation }`, explicit 16-byte LE layout, generation never wraps, slot retired at max, restart floor as in the C authority. Supersede ADR 0013's u32 generation and M3/M15 u32 (needs ADR amendment; Drake-visible).
2. [PROPOSED] **Two-phase revocation**: revoke is immediate for new uses; reclaim of memory, VA, frames or channels requires an `own<Quiesced>` proof (observed fence or drained channel, and natively SMMU unmap + TLBI + CMD_SYNC). Encoded in types, matching native-frame-authority.md §6.4.
3. [PROPOSED] **Completion is a value**: GPU completion produces `own<Fence>` only from an acquire-load of a marker; `nvrm_retire`, pushbuffer reuse and `nvrm_free` consume fences. Plain volatile polling without acquire is forbidden.
4. [PROPOSED] **Failure classes**: invariant violation in kernel/trap context → stackless capture + halt (M2 collector is the reference); exhaustion/OOM, invalid/stale capability, device loss → error values, fail closed, never silent. Device sessions carry a sticky `faulted/DeviceLost` state sourced from the RM error notifier.
5. [PROPOSED] **No-allocate/no-block contexts**: trap/IRQ code may not allocate, take a blocking lock, or call the capability authority's mutating API; the type system marks these contexts.
6. [PROPOSED] **Explicit layouts**: any struct crossing a device, hash, receipt, or C ABI is declared with offsets and LE encoding and emits `_Static_assert`s in generated C headers; hashes are computed over serializer output. Omega's hand-copied `aienos_cap.h` is replaced by one generated header.
7. [PROPOSED] **Opaque device sessions**: `Nvrm` becomes an opaque `own<DeviceSession>` (heap/region, not stack); Omega stops reading its fields and uses accessors. Remove the function-local static in `uvm_map` now (trivial C fix, independent of OSC).
8. [PROPOSED] **Pinned memory**: fixed-address PHYSICS regions and identity-VA GPU memory are `pin<T>`/`device<T>` created only in `unsafe physical block`s; everything above borrows `slice<T>`.
9. [PROPOSED] **Minimal ordering vocabulary**: acquire/release on `device<T>`, `dmb ish/oshld/oshst`, `dsb`, `isb`, cache clean/invalidate by range; nothing else in safe code.
10. [PROPOSED] **Retire/merge**: M15 model (F), Rust aienos-capability(+ffi) (F), M3 trap collector (replace with M2's). Keep sha256_clean.c pattern. Remove `aienos_cap_force_generation` from the production ABI; make `restart` unable to revive a killed writer without an explicit right.
11. [OPEN] Where authority lives long-term: C authority in AIENOS (current Omega link) vs PHYSICS M3 ledger (old doctrine). ADR 0014 points to AIENOS; PHYSICS M3 ledger is then legacy evidence.
12. [OPEN] Native GB10 coherence for all engines, and whether GPU-uncached allocations need CPU-side cache maintenance natively.
13. [OPEN] Atlas handoff I-cache sync requirement once caches are on natively (atlas_m2.s:130-134).
14. [OPEN] Which AIENOS commit Omega actually links (lock 4c21386 vs snapshot 8706fb8).
