> Appendix to OMEGA_SYSTEMS_CORE_CODE_AUDIT.md. Worker report (Opus, read-only), omega 193a7e7. Reviewed by the OSC orchestrator: spot-checked omega_accelerator_world.c:257-290, rx_generation.c:560-565, rx_coherent.c:921-931 (all confirmed). Not authoritative on its own; Part II of the main document rules.

# Omega C memory / ownership hazard map (OSC-0 / OSC-0B input)

Repo: aien-dev/omega at 193a7e7 (worktree osc-0). Read-only audit; nothing built or run.
Scope: all C in src/**, tests/** (harness), tools/*.c = 87,486 lines in 212 files.
Labels: [FACT] code fact with file:line; [SETTLED] decided in a spec/doc (cited); [PROPOSED] recommended OSC-0B decision; [OPEN] unresolved.
Non-overlap: src/language, aarch64 encoder/decoder/emitters, canonical/SemanticId (omega_canonical, omega_codec, sha256), self-host and compiler determinism belong to another worker and are only inventoried here. PHYSICS (nvrm.c, m16_native.c, the NVIDIA-open headers) and AIENOS (libaienos_capability.a) belong to a third worker; only their boundary with Omega is noted.

Counts are grep counts over src+tests+tools (*.c, *.h). They are "sites", not proven bugs.

---

## 1. Inventory

All rows [FACT] unless marked otherwise.

Class key: A semantic / B safe systems / C physical realization / D unavoidable unsafe hardware boundary / E temporary bootstrap dependency / F obsolete, retire.

| Subsystem (files) | Lines | Purpose | Class |
|---|---:|---|---|
| src/runtime rx-core: rx_world.c/.h, rx_coherent.c, rx_seq_reference.c/.h | 3,848 | Resident reaction runtime: object slots, reactions, scheduler/workers, causal crumb log, CPU image of the shared world, resident-seat ring protocol | B (rx_coherent ring part D) |
| src/runtime/omega_shared_world_abi.h, aienos_cap.h, rx_native_bind.c | 414 | Frozen OMEGA_SHARED_WORLD_V1 wire ABI; C view of AIENOS capability authority; layout asserts binding the two | D (ABI) |
| src/runtime/rx_caproot.c/.h | 891 | Linux host-reference capability root: separate process, sealed memfd table, seqlock readers | B, E (Linux oracle; to be replaced by AIENOS native root) |
| src/runtime/rx_resident_gpu.c/.h | 1,146 | Persistent Blackwell "seat": builds SASS program, launches via PHYSICS, heartbeat/lease threads, kill/relaunch | D |
| src/runtime/rx_generation.c/.h | 1,630 | R9 generation store on disk (candidate/active, rename flip, journal, receipts) | B |
| src/runtime rx-state: rx_jspace, rx_cortex, rx_projection, rx_semantic, rx_semcomm (.c/.h) | 4,684 | Branch-shared cognitive state (refcounted realizations, spill), Cortex append store, state projection, semantic reuse cache, semantic communication | A/B |
| src/runtime rx-planning: rx_graph, rx_contract, rx_capq, rx_route, rx_plan, rx_plan_arrange, rx_fusion | 8,982 | Action-graph IR, typed result contracts, capability query, cognitive routing, plan IR/cache, workflow fusion | A |
| src/runtime faculties: rx_aien, rx_omega, rx_aegis, rx_living | 3,181 | Reaction bodies for AIEN/OMEGA/AEGIS faculties; rx_omega JIT store + fork sandbox; living-system glue | A (rx_omega exec pages C) |
| src/omega_accelerator.c/.h, omega_accelerator_world.c/.h | 1,789 | M19 accelerator world: buffer/code registries, handles, scratch bump arena, in-flight submissions, channel-fault recovery, rolling digest | C/D |
| src/omega_blackwell_* (codegen, encoder, qmd, submit, matmul, realize, gates) | 4,485 | Blackwell IR->SASS codegen, QMD build, pushbuffer submission, M17/M18 gate suites | C/D (gates: E) |
| src/omega_world_gates.c/.h | 2,401 | M19 gate suite compiled into omegatool (global gate state) | E/F (test code living in src) |
| src/ semantic core: omega_types.h, omega_validate, omega_core, omega_program, omega_verify, omega_synthesis, omega_library, omega_discovery, omega_machine, omega_evidence, omega_vector | 3,523 | Semantic object graph, validation, interpreter, synthesis/discovery, library, machine graph, evidence receipts | A |
| src/ CPU realization: aarch64_encoder/decoder/target, omega_realize, omega_realize_synth, omega_exec, omega_self_host, omega_matvec(_quad) | 2,182 | AArch64 emit/decode, realization objects, in-process native exec, self-host quine, matvec | C (other worker owns encoder/self-host) |
| src/omega_canonical, omega_codec, sha256 | 659 | Canonical encoding, codec, SHA-256 | A/B (other worker) |
| src/language (lex/parse/lower) | 1,328 | Omega surface language V0 | A (other worker) |
| src/visor (+ rx/visor_world_rx.c) | 5,415 | Visor V1 console: parse, semantic/realization/machine/world/evidence/verify views, effect-request lane | A/B |
| src/crumbline (cl_common, cl_crumb, cl_program, cl_search) | 1,216 | Crumbline learner: crumb decode, program model, bounded search | A |
| tools/omegatool.c | 3,965 | M-gate driver (gates 1..19), demos, forged-receipt tests | E |
| tools/omega.c | 795 | Visor REPL binary (static view buffers) | B |
| tools/r15_reduce.c | 1,289 | R15 evidence reducer (file globals) | E |
| tools/crumbline_learner.c | 294 | Crumbline learner entry point | A |
| tests/runtime R7..R14 + heartbeat (10 files) | 12,241 | Host + silicon qualification of authority, barrier, resident seat, living, recovery; fork/threads/hostile attacks | E (harness) |
| tests/runtime Omega IR gates (14 files) | 14,281 | Action graph ... workflow fusion gates | E (harness) |
| tests/runtime R15 rig (r15_measure, r15_gpu_load, rx_r15_*) | 3,926 | Perf/PMU (perf_event ioctl), NVML via dlopen, load rig | E (NVML = outside dep, retire) |
| tests/visor, tests/language | 2,921 | Visor + language tests | E |
| research/m15/{spbm,pmu}/*.c,*.h (listed only) | n/a | aien_spbm_readonly.c, contract_test.c, contract.h, reduce.c, sample.c, load.c, pmu_probe.c | F/E (research) |

[FACT] External deps (Makefile:3-25, 186-197): libc, pthread, libm; PHYSICS sources compiled in (`$(PHYSICS_DIR)/m16/m16_native.c`, `nvrm/nvrm.c`) plus NVIDIA open-kernel 580.173.02 headers (-I paths Makefile:9-13); AIENOS `libaienos_capability.a` (Makefile:186); NVML `libnvidia-ml.so.1` via dlopen in tests/runtime/r15_measure.c:195-203; perf_event ioctl (r15_measure.c:104-112, rx_plan_reuse.c:88). No libcuda. GPU entry points from Omega: nvrm_* / m16_native_* only in omega_blackwell_submit.c, rx_resident_gpu.c, omega_world_gates.c, omega_accelerator_world.c.

---

## 2. Hazard counts and representative sites

All rows [FACT] (grep counts at 193a7e7) unless marked otherwise.

| Category | Count (sites / files) | Representative file:line |
|---|---|---|
| malloc / calloc / realloc / free | 164 / 102 / 28 / 334 (49 files free) | rx_jspace.c:174 calloc JsReal, :297 realloc chain; rx_world.c:535 realloc deferred ring, :1574 calloc subs; rx_resident_gpu.c:74,1007,1020; heaviest: omega_blackwell_gates.c (36 allocs), rx_jspace.c (24), rx_capq.c (16) |
| posix_memalign / aligned | 1 | rx_coherent.c:198 (coherent image) |
| raw pointer struct fields | ~200 decl lines (heaviest: visor_semantic.c 14, rx_jspace.h 11, rx_contract.h 11, visor_console.h 11) | rx_world.h:491-558 (root, auth_ctx, subs[], crumbs, coherent, timing); rx_jspace.h:96-111 (parent_realization, bytes, packed, index_next); omega_accelerator_world.h:63,75 (void *cpu_addr); rx_resident_gpu.c:48-64 (world, job, hb_watch) |
| pointer arithmetic (base+offset into mapped/shared memory) | 53 (src/tools) | rx_coherent.c:124-135 `w->coherent + off_*()`; omega_accelerator_world.c:283 `(uint8_t *)entry->cpu_addr + req_offset`; :440 scratch `mem.cpu + off`; rx_resident_gpu.c:775,881; rx_cortex.c:113 `s->arena + o->off` |
| NULL sentinels | `== / != NULL` 86 in 32 files; `return NULL` 127 in 39 files | rx_world.c:22-25 crumb_log_map returns NULL on overflow/MAP_FAILED; rx_jspace.c:140-143 index chain terminated by NULL; rx_world.c:231-233 rx_world_crumb NULL = not found |
| fixed MAX arrays (`#define *MAX* n`) | 140 defines / 51 files | rx_world.h:33-45 (256 objects, 1024 reactions, 16 workers, 1024 subs/object); omega_accelerator_world.h (128 buffers, 64 code, 256 in-flight); omega_shared_world_abi.h (1024 ring, 64 objects) |
| ptr+len conventions | pervasive; `uint8_t *buf; size_t len` | cl_common.h:24,40; rx_semcomm.h:176; rx_jspace.h:105-106 (packed/packed_len) |
| flexible array members | 0 | none |
| void* | 312 / 71 files | rx_world.h:206,290 `void *user`; rx_world.h:494 `const void *auth_ctx`; visor_console.h:57,98; cl_search.h:41,66 |
| data<->function pointer punning | 7+ | union {void*; Fn} rx_omega.c:177,456; omega_exec.c:28-33; omega_matvec.c:291-296; dlsym casts r15_measure.c:197-203 |
| volatile<->_Atomic punning | 4 helpers | rx_coherent.c:75-85 casts `volatile uint64_t*` to `_Atomic uint64_t*` |
| struct-typed pointer casts on raw memory | many | rx_coherent.c:124-135 `(OmegaSharedWorldRing *)(w->coherent + off)`; rx_world.c:1809; grow((void **)&p->arena ...) rx_projection.c:174 (T** -> void**) |
| uintptr_t | 0 | none (addresses kept as `void*` + `uint64_t va`) |
| goto cleanup | 103 / 16 files | rx_generation.c (20; e.g. :974-982 `goto done`), rx_coherent.c (14; :661-700 `goto out` with mutex unlock), rx_world.c (9) |
| manual multi-free cleanup ladders | several | rx_resident_gpu.c:1020-1085 (each failure path repeats m16_native_close/free(code)/free(s)); omega_matvec.c:331-334 |
| global mutable state (file-scope non-const statics) | 202 lines / 46 files (≈70 in src+tools) | rx_coherent.c:88-89 g_crc/g_crc_ready; rx_resident_gpu.c:137 emit_overflow; rx_fusion.c:675 g_bench, :899 g_blob; rx_capq.c:302 `__thread t_sort`; rx_generation.c:149 g_io_bytes/g_io_syncs; omega_evidence.c:36-37 g_run_id; visor_evidence.c:29-32,445-447 parser buffers; tools/omega.c:42-53 view buffers; omega_world_gates.c:19-34, 2169-2170 |
| arenas / bump / pools | 5 | OmegaScratchArena bump (omega_accelerator_world.c:424-451); CxStore arena (rx_cortex.h:79, grow rx_cortex.c:58); projection arena (rx_projection.c:143-179); crumb log (rx_world.c:21-26, MAP_NORESERVE reservation); fixed slot pools RxWorld.objects/reactions, accel buffers/code_entries |
| accelerator object registries | 2 | OmegaAcceleratorWorld.buffers[128]/code_entries[64]/in_flight[256] (omega_accelerator_world.h); RxWorld.objects[256] with physical windows (rx_world.h:293-316) |
| CPU addresses / GPU VAs as fields | gpu_va/.va 458 hits in 10 files (omega_blackwell_submit.c 42, omega_world_gates.c 28, omega_accelerator_world.c 19, rx_resident_gpu.c 14) | OmegaBufferEntry/CodeEntry `void *cpu_addr; uint64_t gpu_va; NvrmMem mem` (omega_accelerator_world.h:57-77); OmegaCompletionTracker `volatile uint32_t *cpu_marker; uint64_t gpu_va`; SeatArgs.region = image.va (rx_resident_gpu.c:35,1065) |
| generations / epochs / object IDs | "generation" 1010 / 85 files; epoch 234 / 37 | RxObjRef{id,generation} rx_world.h:172; RxCapRef{cap_id, u64 generation} rx_caproot.h:118; OmegaHandle{world_epoch,id,gen,type,perms} omega_accelerator_world.h:48; seat_generation rx_world.h:549; channel_generation; ABI world_epoch/object_generation/sequence |
| permission / rights fields | rights 319 / 52; authority 573 / 83 | RxCapEntry.rights (rx_caproot.h:127); OmegaHandle.permissions + entry.permissions; ABI OmegaSharedWorldObject.permissions (written 0, never authority: rx_coherent.c:6-7) |
| stale-handle validation | "stale" 200 / 30 | omega_accelerator_world.c:397-422, 257-290; rx_caproot.c:569-618; rx_coherent.c notice_intact :709-719, check_locked :287; rx_world.c RX_ERR_STALE_GEN |
| serialization / deserialization | canonical 179 hits / 79 files; OMGG 1 | Explicit LE encoders: rx_world.c:44-52 put32/put64, rx_generation.c:122-141, 251-290; rx_capq.c:202; frozen wire: omega_shared_world_abi.h; library "LIB1" (omega_library.c), "OMG0"/"OMG_R0" headers (omega_machine.c, omega_realize.c) [other worker] |
| mmap / munmap / mprotect | 18 / 24 / ~20 | rx_world.c:23 crumb log; rx_caproot.c:341,456 memfd table; rx_jspace.c:846; rx_omega.c:137 JIT page; omega_exec.c:15; omega_accelerator_world.c:659-664 output write-protect |
| ioctl | 4 (tests only, perf_event) | r15_measure.c:104-112 |
| MMIO / doorbell / semaphore | doorbell 9, semaphore 25 (in PHYSICS calls) | nvrm_ring / m16_native_submit_methods (omega_accelerator_world.c:688-694, rx_resident_gpu.c:844); host semaphores `volatile uint32_t *hsem` omega_blackwell_submit.c:125-126, omega_accelerator_world.c:899,1119 |
| inline asm | 23 (dsb sy ×18, yield ×4) | rx_resident_gpu.c:710-984; omega_blackwell_submit.c:128,340,571; omega_world_gates.c:673-753 |
| threads | pthread_create 22 (src: rx_world.c:1404, rx_resident_gpu.c ×3, rx_generation.c ×1) | fork 23 (src: rx_omega.c sandbox, rx_caproot.c root process, rx_jspace/rx_semantic "fork" = branch fork, not process) |
| atomics | stdatomic 151 hits/15 files; __atomic/__sync 37/9 | see §3a |
| packed / static_assert / offsetof | packed attr 1 (rx_resident_gpu.c:34); _Static_assert 17; offsetof 17 | omega_shared_world_abi.h:286-296; rx_coherent.c:30-35; rx_native_bind.c:10-15 |
| structs hashed by raw bytes | ≥6 in src | omega_accelerator.c:83; omega_program.c:276; omega_vector.c:23; omega_synthesis.c:98; rx_semcomm.c:616-631; omega_accelerator_world.c:530-563 (memcpy of native ints) |
| abort / assert / setjmp | 0 / 0 / 0 | failure is return codes everywhere |
| exit / _exit | 10 (src only rx_caproot.c ×8 in root child) | rx_caproot.c:338-339 |
| errno | 37 / 13 | rx_caproot.c:81 EINTR loop; rx_generation.c |
| time sources | 57 / 41 | rx_world.c:33-43; rx_caproot.c:52-58 CLOCK_BOOTTIME; omega_evidence.c:41-51 wall clock |
| qsort | 34 / 17 | comparators are total orders with id tiebreak rx_capq.c:290-299 |
| dlopen/dlsym | 6 (tests) | r15_measure.c:195-203 NVML |
| refcounts / parent links | refcount 4; parent 326 / 48 | JsReal.refs + parent_realization rx_jspace.h:96-99; RxCapEntry.parent_id chain rx_caproot.h:129; crumb parents[] (ids) rx_world.h:340 |

---

## 3. Machine-semantics evidence (OSC-0B items 1-7)

### 3a. Concurrency, atomics, publication (items 1, 2)

- [FACT] Threads: RxWorld owns up to 16 worker pthreads created in rx_world_init (rx_world.c:1402-1405); every RxWorld mutation is under one `pthread_mutex_t mu` (rx_world.h:487; "Simple reference: one mutex" rx_semantic.c header). RxGpuSeat adds a launch thread and a lease thread (rx_resident_gpu.c:985, 950-964, 1071). rx_generation.c has one worker thread. rx_jspace is single-threaded by contract (rx_jspace.c:4).
- [FACT] Four distinct memory-model idioms coexist:
  1. C11 acquire/release + release fences on SPSC ring head/tail (rx_coherent.c:75-85, 238-244, 721-733, 444-468, 885-897), done by casting the ABI's `volatile uint64_t tail/head` (omega_shared_world_abi.h ring struct) to `_Atomic uint64_t*` — type punning, formally UB.
  2. GCC `__atomic_load_n/__atomic_store_n` ACQUIRE/RELEASE on the same fields (rx_world.c:1808-1811 ring_discard).
  3. `volatile` plain loads/stores + `__asm__ volatile("dsb sy")` for CPU<->GPU heartbeat, lease, hold, completion marker and semaphores (rx_resident_gpu.c:707-712, 860-864, 897-928, 945, 984; omega_blackwell_submit.c:125-128; omega_accelerator_world.c:135, 899, 1119; omega_world_gates.c:673-753).
  4. Seqlock across processes over a MAP_SHARED memfd: `_Atomic uint64_t seq` odd while writing, entries copied with plain memcpy between acquire load and acquire fence (rx_caproot.c:96-104, 569-618; rx_caproot.h:136-143). Entry reads are formally racy (seqlock pattern); readers spin unbounded while `seq` odd unless the writer is dead (rx_caproot.c:574-576).
  Relaxed-only counters: rx_generation.c:151-203 (`__atomic_add_fetch RELAXED`), omega_accelerator_world.c:17-27 (handle epoch CAS relaxed), rx_caproot.c:50-73 (boot generation CAS acq_rel).
- [FACT] Cross-thread flags that are `volatile`, not atomic: RxGpuSeat.abort, marker_ok, sem_ok, lease_run (rx_resident_gpu.c:53-63). Data races by the C11 model; work in practice on AArch64 GCC.
- [FACT] Unsynchronized lazy global init: g_crc/g_crc_ready (rx_coherent.c:88-106) can be initialised concurrently by two workers (benign-in-practice race). `emit_overflow` static (rx_resident_gpu.c:137) makes seat program build non-reentrant. `static Bench g_bench` "verification is not re-entrant" (rx_fusion.c:675). `__thread RankCtx t_sort` for qsort context (rx_capq.c:302).
- [FACT] CPU->GPU publication today: CPU writes 128-byte descriptor into ring slot by memcpy, seals with CRC32C over bytes [0,0x3C)+payload (rx_coherent.c:109-122), release fence, release-store tail (rx_coherent.c:721-733). The ring memory is a GPU-uncached NvrmMem mapping when the seat is live (rx_resident_gpu.c:1036-1043). GPU->CPU: seat writes g2c ring; CPU acquire-loads tail, memcpys slot, release-stores head, then verifies magic/epoch/checksum (torn-write detection) (rx_coherent.c:709-719, 885-897). Liveness: heartbeat/lease words written with volatile+dsb (rx_resident_gpu.c:897-928). Non-resident dispatch: pushbuffer enqueue + nvrm_ring doorbell (omega_accelerator_world.c:666, 688-694), completion by polling a host-visible marker/semaphore (m16_native_wait_marker, omega_blackwell_submit.c:203; payload serial compare omega_accelerator_world.c:38-40).
- [SETTLED] The ring discipline (SPSC, free-running 64-bit counters, release-published, checksum as defence-in-depth, fail-closed reader) is frozen in omega_shared_world_abi.h:21-52, 159-167 (M20 Stage 1 design laws; ring head/tail at :163,:166) and r12-resident-seat.md.
- [FACT] Generation change while another engine holds a view:
  - Object generation moves while chip holds a claim -> invalidated and recorded; stale claim refused by chip fault; seat loss moves `seat_generation`, not object generations; old-seat results refused by `producer_generation != seat_generation` (rx_world.c:1673-1680, 1814-1864; rx_resident_gpu.c:453). [SETTLED] r12-resident-seat.md:34-70.
  - Accelerator world: revoke marks entry inactive and bumps generation immediately; memory freed only after no in-flight submission binds it (`retiring`), omega_accelerator_world.c:234-255, 84-93. Output buffer is mprotect(PROT_READ) on the CPU while the GPU owns it (omega_accelerator_world.c:659-665) and restored at completion/abandon (:500, 756, 810).
  - [FACT] Raw pointers already handed out by omega_world_resolve_buffer (:257-290) and scratch slices (:424-446) are not tracked: revoke of a not-in-flight buffer frees memory immediately (:254) and `omega_world_scratch_reset` (:448-451) rewinds the bump pointer with no in-flight or outstanding-slice check. A caller's cached `cpu`/`gpu_va` then dangles or aliases.
  - [OPEN] rx_world_relocate_physical (rx_coherent.c:661-700) moves an object's window without generation change and without checking for an outstanding resident claim on that object; r12 table covers "detach while chip holds claim" but not relocate. Whether the seat re-reads the table per claim is not verified here.
  - [FACT] rx_world_bind_coherent (rx_coherent.c:921-931) memcpys the whole image into new memory and frees the old one under `mu`; rx_resident_take_result (:885-897) reads `w->coherent` without `mu`, but its only src caller holds `mu` (rx_world.c:1669-1673). Lock-free readers of `w->coherent` exist in the seat (hb_word_at rx_resident_gpu.c:880-882, lease thread :897-907); ordering of lease_stop before seat_return_image (:1085-1090) is what keeps them safe.

### 3b. Layout (item 3: runtime vs wire layout)

- [FACT] Wire/hardware-format structs with size/offset asserts: OmegaSharedWorldDesc 128 B, object record 32 B, fault mailbox 64 B, checksum at 0x3C, payload at 0x40 (omega_shared_world_abi.h:286-296; rx_coherent.c:30-35). Cache-line padding is by explicit `_pad` arrays, not `aligned` attributes (0 uses of aligned/_Alignas).
- [FACT] One `__attribute__((packed))` struct: SeatArgs (rx_resident_gpu.c:34-43), uploaded word-by-word into a GPU constant bank (:801) — a CPU/GPU-kernel ABI with no static_assert on size/offsets.
- [FACT] Cross-repo native layout coupling: AienosCapEntry must equal RxCapEntry in size and selected offsets (rx_native_bind.c:10-15). RxCapEntry has implicit padding (u32 before u64 at cap_id/generation, parent_id/parent_generation, minted_by_id/minted_by_generation — rx_caproot.h:120-133) and is shared across processes via memfd (rx_caproot.c:341, 456).
- [FACT] Digests depending on native layout or endianness:
  - Whole-struct hash: omega_accelerator.c:83 (OmegaEffectIntent), omega_program.c:276 (SynthesisTask minus id; any padding bytes are hashed), omega_vector.c:23, omega_synthesis.c:98 (other worker's area for SemanticId; flagged only).
  - Native-endian integer arrays hashed: rx_semcomm.c:616-631; rolling world digest memcpy of u32/u64 (omega_accelerator_world.c:530-563); `sha256_hash(semantic_words)` (:676-678).
  - Contrast (good): explicit little-endian field encoders for RxObject digest and crumb digest (rx_world.c:44-66, 127-168), generation store (rx_generation.c:122-141, 251-290), capq (rx_capq.c:202-205).
- [SETTLED] The shared world is "Zero Serialization": native LE fixed-width, distinct from big-endian canonical semantic encoding (omega_shared_world_abi.h:30-38 ("Zero Serialization" :32); canonical-encoding.md).
- [FACT] Width mismatch: RxCapRef.generation is u64 but the frozen descriptor carries u32; the high halves are smuggled into payload bytes 40/44 (rx_world.h:146-152), and crumb_hash hashes only the low 32 bits of cap generations (`put32(&c, k->caps[i].generation)`, rx_world.c:147).

### 3c. Failure handling (item 4)

- [FACT] No abort, assert or setjmp anywhere (0 hits). All failures are negative return codes: RX_ERR_* (rx_world.h:107-121), RX_CAP_ERR_* (rx_caproot.h:96-114), OMEGA_WORLD_ERR_* (omega_accelerator_world.h), JS_ERR_NOMEM, plain -1.
- OOM: [FACT] mapped to RX_ERR_FULL (rx_world.c:1375-1389, 1574-1575), OMEGA_WORLD_ERR_NO_MEM, JS_ERR_NOMEM (rx_jspace.c:298, 306, 374), or -1/NULL. Some allocations unchecked for arithmetic overflow: grow() doubles `c` with no overflow check and `c * elem` unchecked (rx_projection.c:143-152, rx_cortex.c:58).
- Bounds: [FACT] fail-closed checks return *_ERR_BOUNDS (omega_accelerator_world.c:283-285; rx_coherent.c:548-558). Silent truncation exists: add_parent drops parents past RX_MAX_PARENTS (rx_world.c:171-176); rolling digest caps id/gen pairs at 64 bytes while hashing the full count (omega_accelerator_world.c:550-553); pushbuffer `uint32_t pb[1024]` stack arrays filled by `pb[n++]` with no bound check (omega_blackwell_submit.c:131, 343, 573; omega_accelerator_world.c:906, 1126) or a check only after writing (rx_resident_gpu.c:786, 839).
- Stale handle: [FACT] *_ERR_STALE_GEN / STALE_EPOCH everywhere; generation saturation retires the slot rather than wrapping (omega_accelerator_world.c:197-199, 248).
- Vanished device: [FACT] marker timeout -> m16_native_close and -1 (omega_blackwell_submit.c:203); accelerator channel fault -> destroy/rebuild channel, bump channel_generation, abandon uncommitted in-flight, restore output protection (omega_accelerator_world.c:461-511); sticky `faulted` flag refuses further work (:620). Seat loss -> rx_resident_seat_lost (rx_world.c:1814-1864). [SETTLED] r12-resident-seat.md:52-70.
- Failed construction: [FACT] partial cleanup ladders (rx_resident_gpu.c:1020-1085). rx_world_init ignores pthread_create / pthread_mutex_init return values (rx_world.c:1397-1405); a failed create leaves an uninitialised pthread_t that rx_world_destroy joins (:1435).
- Cleanup failure: [FACT] nvrm_free failure sets world->faulted and keeps the slot retiring (omega_accelerator_world.c:59-81); `(void)release_*` ignores the result in bulk release (:87-92); omega_matvec.c:338 ignores mprotect failure then executes.
- Invariant violation: [FACT] reported as return code plus crumb/fault mailbox (raise_fault rx_coherent.c:170-178; RX_CRUMB_FAILED); crumb log full refuses the publication (rx_world.c:179-183, "no action without evidence").
- [PROPOSED] see §8.

### 3d. Cycles and weak references (item 5)

- [FACT] Refcounted DAG with raw back-links: JsReal.refs/holders, parent_realization pointer, cascading real_unref (rx_jspace.h:96-111; rx_jspace.c:158-168). `uint32_t refs` has no overflow check. Intrusive index chain `index_next` (rx_jspace.c:140-155). Manual weak ref: `s->cache_r` cleared when its target is freed (rx_jspace.c:161).
- [FACT] Capability parent chain by id, depth-bounded by RX_CAP_MAX_DEPTH (rx_caproot.c:106-110, 599-606) — no pointers.
- [FACT] Crumb causal graph by dense id (`parents[]`, `wake_cause`), parents always precede children (rx_world.c:160-165) — acyclic by construction.
- [FACT] Back-pointers without lifetime tie: RxGpuSeat.world (rx_resident_gpu.c:59) — seat_return_image writes into the world (:70-81), so rx_world_destroy before rx_gpu_seat_finish is a use-after-free; nothing enforces order. rx_living.h:124-137, rx_aegis.h:127, rx_aien.h:132, rx_omega.h:138, rx_capq.h:245 all hold `RxWorld *`. Cortex `links[CX_LINKS]` are ids (rx_cortex.h:65).
- [FACT] No ownership cycles found among heap objects other than JsReal (which is a DAG).

### 3e. Relocation, compaction, pinning (item 7)

- [FACT] Pinned by design: crumb log records "never move, so a crumb pointer stays valid for the life of the world" (rx_world.c:15-19), and rx_world_crumb returns a raw pointer without the lock (:231-233).
- [FACT] Pinned implicitly: RxWorld embeds pthread mutex/conds and worker threads hold `w` (rx_world.h:487-489, 533) — the struct must never be copied or moved after init. RxGpuSeat likewise.
- [FACT] Relocatable by offset: the shared world stores region-relative offsets, not VAs (omega_shared_world_abi.h:207-208, 272); Cortex and projection store arena offsets (rx_cortex.h:67, rx_projection.c:176).
- [FACT] Relocation that invalidates interior pointers: grow() realloc of arenas while projection items hold `it->w = p->arena + e->off` (rx_projection.c:526); JsBranch.units realloc (rx_jspace.c:443); coherent image moved by rx_world_bind_coherent (rx_coherent.c:921-931) — any cached `hb_watch` (rx_resident_gpu.c:861) or ring pointer becomes stale.
- [FACT] Window move: rx_world_relocate_physical copies 64 B to a spare window, keeping id/generation/version/digest (rx_coherent.c:661-700).
- [FACT] GPU-shared memory is pinned by PHYSICS allocation (NvrmMem cpu+va pairs); JIT pages are mmap'd, mprotect RX, never moved (rx_omega.c:137-152).

### 3f. Determinism (and item 6: live handle vs persistent identity)

- [FACT] Deliberately excluded from identity: timing and worker placement in crumb digests (rx_world.c:166-167); host addresses and time in the rolling world trace (omega_accelerator_world.c comment before :572).
- [FACT] Nondeterministic inputs: evidence run id from wall clock + git sha (omega_evidence.c:41-51); capability slot generations seeded from CLOCK_BOOTTIME<<8 (rx_caproot.c:52-73, 351, 430); getrandom tokens (rx_caproot.c:77-85). Because crumb_hash includes cap generations (rx_world.c:145-149), host-reference crumb digests differ between runs whenever caps are stamped. qsort comparators are total orders (rx_capq.c:290-299) so ordering is deterministic; no pointer values printed (%p: 0 hits).
- [FACT] Item 6 — live handles serialized as identity: runtime slot handles (object slot index + generation, cap_id + cap generation) are written into durable/hashed records: generation-store object rows `id, generation, digest` (rx_generation.c:560-565); receipt carries cap_id (rx_generation.c:515-531); crumb digests hash slot ids/generations (rx_world.c:139-156); world digest hashes (id, generation, content digest) (rx_world.h:627). No CPU address or GPU VA was found entering a durable digest or wire record; addresses appear only in live launch args (SeatArgs.region, rx_resident_gpu.c:1065), pushbuffer words, and registry entries.
- [FACT] Build-path leakage: Makefile:14 bakes `-DOMEGA_PHYSICS_DIR="$(PHYSICS_DIR)"` into the binary (compiler determinism otherwise owned by the other worker).
- [OPEN] Whether slot-index+generation is meant to be durable identity or must be replaced by SemanticId/content digest in persisted artifacts.

---

## 4. Per-site hazard table (top 40)

Hazard, ownership and lifetime columns are [FACT]; the "proposed replacement" column is [PROPOSED].

Format: repository/path | function/type | hazard | current ownership model | current lifetime model | proposed Omega replacement | migration difficulty | required compatibility boundary

1. omega/src/omega_accelerator_world.c:257-290 | omega_world_resolve_buffer | returns raw `void*` + GPU VA with no borrow tracking; revoke (:254) frees immediately if not in flight | registry owns NvrmMem; caller gets untracked alias | until revoke/destroy; caller cannot know | `&T`/`&mut T` borrow scoped to registry + `device<T>` view; revoke requires no live borrows | high | PHYSICS NvrmMem ABI (C ABI adapter)
2. omega/src/omega_accelerator_world.c:424-451 | omega_world_scratch_acquire / _reset | bump arena reset with no check of outstanding slices or in-flight GPU use | world owns arena | "until reset", unchecked | `arena/region` with region token; reset consumes token only when no in-flight fence | med | GPU VA handed to pushbuffer
3. omega/src/runtime/rx_coherent.c:75-85 | load_acquire_u64 etc. | casts `volatile uint64_t*` to `_Atomic uint64_t*` | ring memory owned by world or borrowed GPU mapping | lifetime of image | `device<T>` atomic cell type in the ABI with declared ordering | med | frozen ABI header layout (omega_shared_world_abi.h)
4. omega/src/runtime/rx_resident_gpu.c:53-63 | RxGpuSeat.abort/marker_ok/sem_ok/lease_run | volatile cross-thread flags (data race) | seat struct | seat lifetime | atomic<bool> with acquire/release | low | none
5. omega/src/runtime/rx_resident_gpu.c:707-712, 897-928 | wait_marker_or_abort, lease_main | volatile + `dsb sy` CPU<->GPU publication, no formal model | image owned by PHYSICS mapping | seat lifetime | `unsafe physical block` with named publication primitive (e.g. publish_release/observe_acquire on device memory) | med | GPU SASS program side (membar semantics)
6. omega/src/runtime/rx_resident_gpu.c:59, 70-81 | RxGpuSeat.world | raw back-pointer; seat_return_image writes into world; no ordering with rx_world_destroy | seat borrows world | assumed world outlives seat | `&mut RxWorld` borrow held by seat / seat owned by world (`own<Seat>` field) | med | none
7. omega/src/runtime/rx_coherent.c:921-931 | rx_world_bind_coherent | relocates image by memcpy + free; cached pointers (hb_watch :861) and lock-free readers dangle | world owns or borrows (`coherent_borrowed`) | until next bind/free | `pin<Image>` + ownership enum {Owned(own<T>), Borrowed(device<T>)}; rebind requires no outstanding views | high | PHYSICS NvrmMem, seat launch args
8. omega/src/runtime/rx_coherent.c:248-252 | rx_coherent_free | bool flag decides whether to free | flag-based conditional ownership | world lifetime | `own<T>` vs `&T` in the type, not a flag | low | none
9. omega/src/runtime/rx_coherent.c:661-700 | rx_world_relocate_physical | window moved without generation bump or resident-claim check | world | object lifetime | window as `handle<Window>` with its own generation; move invalidates outstanding claims | med | frozen 32-byte object record
10. omega/src/runtime/rx_resident_gpu.c:34-43 | SeatArgs (packed) | hardware-format struct with no size/offset asserts; carries live GPU VA | stack, copied to cbank | one launch | `device<T>` ABI struct with compiler-checked layout; VA as `device ptr` type, never serializable | low | SASS kernel argument layout
11. omega/src/omega_blackwell_submit.c:131,343,573; omega_accelerator_world.c:906,1126 | execute_* / dispatch_* | `uint32_t pb[1024]` stack array filled via `pb[n++]` without bound check | stack | call | `slice<u32>` builder with checked push | low | pushbuffer word format (PHYSICS)
12. omega/src/runtime/rx_resident_gpu.c:786-839 | launch_main | pushbuffer bound checked only after writes (`if (n > 1024)`) | stack | call | checked builder | low | same
13. omega/src/runtime/rx_caproot.c:569-618 | rx_caproot_inspect/validate | seqlock with plain memcpy of entries; unbounded spin while writer alive | root process owns table; readers map RO memfd | root process lifetime | `unsafe physical block` seqlock primitive with bounded retry; entries as `device<T>`-like shared view | med | AIENOS cap entry layout (rx_native_bind.c)
14. omega/src/runtime/rx_caproot.h:120-133 | RxCapEntry | implicit padding in a cross-process, cross-repo layout | memfd | root lifetime | explicit fixed layout with reserved fields; layout asserted on both sides | low | AIENOS AienosCapEntry (C ABI adapter)
15. omega/src/runtime/rx_world.c:139-156 | crumb_hash | hashes slot ids/generations; cap generation truncated to 32 bits (:147) | world | world lifetime | hash `SemanticId<T>` / full 64-bit generation via canonical encoder | low | existing crumb digests in evidence
16. omega/src/runtime/rx_world.h:146-152 | claim payload bytes 40/44 | u64 cap generation split across frozen u32 field + payload | ring | per message | ABI v2 with u64 generation field | med | frozen OMEGA_SHARED_WORLD_V1
17. omega/src/runtime/rx_world.c:15-26, 231-233 | crumb_log_map / rx_world_crumb | raw pointer into MAP_NORESERVE log returned unlocked; relies on pinning | world | world lifetime | `handle<Crumb>` (dense id) + `&T` scoped borrow; log as `pin<arena>` | low | crumb ids in evidence
18. omega/src/runtime/rx_world.c:1397-1405 | rx_world_init | pthread_create / mutex_init results ignored; destroy joins garbage | world | world lifetime | constructor returns Result; partially built world not observable | low | none
19. omega/src/runtime/rx_world.h:486-567 | RxWorld | large value type (estimated ~1 MB from field counts; not built) with embedded mutex; stack-allocated in tests (e.g. tests/runtime/rx_r13_living.c:30) | caller | caller scope | `pin<own<World>>` heap-only | low | public C API (rx_world_init takes RxWorld*)
20. omega/src/runtime/rx_world.c:525-545 | defer_wake | memmove + realloc ring; index arithmetic overflow guarded only by `next < cap` | world | world | `Vec`-like `own<[T]>` with checked growth | low | none
21. omega/src/runtime/rx_jspace.c:158-168 | real_unref | manual refcount cascade, u32 refs unchecked, raw parent pointers | JsSpace + refcount holders | until refs==0 | `arena/region` for JsReal with `handle<JsReal>` + counted edges, or `own<T>` tree + `weak<T>` cache | med | none (single-threaded)
22. omega/src/runtime/rx_jspace.h:179 / rx_jspace.c:161 | JsSpace.cache_r | hand-made weak ref cleared on free | space | ad hoc | `weak<JsReal>` | low | none
23. omega/src/runtime/rx_jspace.c:140-155 | index_insert/remove | intrusive singly-linked chain in owned objects | space | object lifetime | index keyed `SemanticId<T>` -> `handle<T>` map | low | none
24. omega/src/runtime/rx_projection.c:143-179, 526 | grow / pj items | realloc arena while items hold `arena + off` pointers; `c*=2` / `c*elem` unchecked | projection owns arena | until grow | `arena/region` with offset handles; `slice<T>` borrows invalidated by type system | low | none
25. omega/src/runtime/rx_cortex.c:58, 113 | grow / cx_payload | same pattern (pointer into growable arena) | store | until grow | same | low | none
26. omega/src/runtime/rx_omega.c:137-152, 177, 456 | store_put / sandbox_run | data->function pointer punning on JIT page; page lifetime = faculty | faculty owns pages | faculty lifetime | `unsafe physical block` exec-page type (W^X enforced), callable only via typed thunk | med | AArch64 ABI of generated code
27. omega/src/omega_exec.c:8-40 | omega_exec_native_f3 | executes unverified generated code in-process (no fork sandbox) | function-local page | call | same as 26, sandbox or verifier gate | med | AArch64 calling convention
28. omega/src/omega_matvec.c:331-338 | benchmark path | mprotect result ignored; memcpy of code_len into a fixed 4096 page without len check | local | call | checked `slice<u8>` copy + Result | low | same
29. omega/src/omega_accelerator_world.c:234-255 | omega_world_revoke_buffer | generation bump + deferred free via `retiring` flag | registry | revoke or drain | `handle<T>` + fence-tracked `device<T>` deferred drop | med | PHYSICS nvrm_free
30. omega/src/omega_accelerator_world.c:612-685 | omega_world_submit | input digest computed after enqueue from raw cpu_addr; inputs not write-protected | registry | in-flight | `&T` shared borrow of inputs held until fence | med | pushbuffer
31. omega/src/omega_accelerator_world.c:461-511 | omega_world_recover_channel_fault | device-loss path mutates many slots; `(void)release_*` drops errors | world | world | typed fault state machine; errors aggregated | med | PHYSICS channel API
32. omega/src/omega_accelerator_world.h:57-77 | OmegaBufferEntry/CodeEntry | cpu_addr + gpu_va + NvrmMem duplicated address state | registry | slot | `device<T>` pairing CPU view and GPU VA in one opaque value | med | NvrmMem
33. omega/src/omega_accelerator_world.c:398-421 | omega_world_validate_handle | permissions carried in caller-held handle and re-checked vs entry (good), but handle is plain struct (forgeable fields) | caller | value | `handle<T>` opaque + rights as type/capability | low | handle wire in gates/evidence
34. omega/src/omega_accelerator.c:83; omega_program.c:276; omega_vector.c:23 | digest of whole native structs | padding/endianness enter digests | value | value | canonical encoder (other worker) | low | existing digests/receipts
35. omega/src/runtime/rx_semcomm.c:616-631 | rx_sem_view_digest | native-endian u64 arrays hashed | value | value | explicit LE encoder | low | view digests in evidence
36. omega/src/runtime/rx_coherent.c:88-106 | g_crc lazy init | racy global init | global | process | const table computed at compile time | low | none
37. omega/src/runtime/rx_resident_gpu.c:137 | emit_overflow | global error flag in codegen | global | process | Result from builder | low | none
38. omega/src/runtime/rx_fusion.c:675, 899 | g_bench, g_blob | non-reentrant globals | global | process | per-call `own<T>` context | low | none
39. omega/src/runtime/rx_generation.c:560-565, 515-531 | generation blobs/receipt | persists runtime slot id+generation and cap_id as durable identity | store (disk) | across restarts | `SemanticId<T>` / durable object id; slot handles stay runtime-only | high | on-disk R9 format v2 (ROOT_AUTH_GEN_HI etc.)
40. omega/src/visor/visor_evidence.c:29-32, 445-447; tools/omega.c:42-53 | JSON parser + REPL view buffers | large static mutable buffers, non-reentrant | global | process | `own<T>` session context | low | Visor console API

---

## 5. Existing good patterns worth keeping

- [FACT] Pointer-free, offset-based, generation+epoch+sequence protected shared world with hostile-input reader: omega_shared_world_abi.h:21-52; notice_intact rx_coherent.c:709-719; check_locked :287-398; fault mailbox codes.
- [FACT] Generation checks with slot retirement on saturation (no wrap): omega_accelerator_world.c:197-199, 248; handle epoch never reused (:17-27).
- [FACT] Deferred free while GPU holds a reference (`retiring`): omega_accelerator_world.c:84-93, 250-253.
- [FACT] CPU write-protection of a buffer while the GPU owns it (mprotect PROT_READ): omega_accelerator_world.c:659-665 — a hardware-enforced borrow.
- [FACT] Seat generation / producer_generation fencing of a lost GPU engine: rx_world.c:1814-1864, 1673-1680.
- [FACT] Snapshots by value to reaction bodies (RxCtx carries values, not pointers): rx_world.h:185-222; rx_world_read copies out (rx_world.h:614).
- [FACT] Explicit little-endian field encoders for digests and disk formats: rx_world.c:44-66, 127-168; rx_generation.c:122-141, 251-290; rx_capq.c:202.
- [FACT] Crumb log bounded; full log refuses action (rx_world.c:179-183).
- [FACT] Authority split: runtime handle has no mint power (rx_caproot.h:145-151); separate non-dumpable root process (rx_caproot.c:1-9); constant-time token compare (:88-92).
- [FACT] W^X for JIT pages + fork sandbox with canaries for generated code (rx_omega.c:137-152, 400-460).
- [FACT] Visor views never hold pointers into the runtime (visor_world.h:60, 73-74; visor_world_rx.c header).
- [FACT] qsort comparators are total orders (rx_capq.c:290-299).
- [FACT] Hostile attack tests on mappings (writable remap of the RO cap table, tests/runtime/rx_heartbeat_test.c:987, 1292).

---

## 6. Candidate first subsystems to migrate

All three [PROPOSED].

1. src/crumbline (cl_common, cl_crumb, cl_program, cl_search; 1,216 lines) + tools/crumbline_learner.c. Linked without physics or gates (Makefile:31-40); CPU-only; ptr+len buffers and bounded search; deterministic by design. Tests exist (test-crumbline). Exercises slice<T>, own<T>, Result.
2. src/runtime/rx_cortex + rx_projection (≈1,300 lines incl. headers). Pure CPU, append-only store with id links and offset arenas; the only real hazard is realloc-invalidated interior pointers (§4 rows 24-25), exactly what arena/region + offset handles fix. Has gate tests (rx_state_projection).
3. src/visor/visor_parse_command.c/.h (260 lines, "no semantics", no authority). Smallest self-contained parser; good first C ABI adapter test against tools/omega.c.
Avoid first: rx_world/rx_coherent/rx_resident_gpu/accelerator_world (hardware coupled), sha256/canonical (other worker).

---

## 7. Per-object lifetime / authority / placement (item 12, as the code does it today)

All rows [FACT].

| Object kind | Who keeps it alive | Who may touch it | Where it physically lives |
|---|---|---|---|
| RxWorld | caller-allocated storage (stack/static/heap), rx_world_init (rx_world.c:1320, 1329, 1338) / rx_world_destroy (:1427-1445) | any thread holding `mu`; worker threads hold raw `w` | host heap/stack; must not move (embedded mutex) |
| RxObject (slot) | slot in RxWorld.objects[256]; retire bumps generation (rx_world.h:596) | writes only via reaction publication under `mu` after capability validation (auth_validate or caproot) | host struct + optional 64-B window in coherent image |
| RxReaction | slot in reactions[1024]; never removed | scheduler under `mu`; body gets value snapshot | host struct |
| RxCrumb | append-only log, world lifetime | appended under `mu`; readers get raw pointer | MAP_NORESERVE anonymous mapping, pinned |
| Coherent image (OMEGA_SHARED_WORLD_V1) | RxWorld if owned; PHYSICS mapping if `coherent_borrowed` (rx_resident_gpu.c:1036-1043) | CPU under `mu` for rings/table; seat GPU program; lease thread lock-free | posix_memalign host heap or GPU-uncached NvrmMem; moved by bind_coherent |
| RxGpuSeat | calloc in rx_gpu_seat_begin, freed in rx_gpu_seat_finish | owner thread + launch thread + lease thread | host heap; owns M16 context/channel and launch memory |
| RxCapTable / RxCapRoot | separate root process owns writable memfd | root writes under seqlock; runtime maps read-only | sealed memfd MAP_SHARED |
| OmegaBufferEntry / OmegaCodeEntry | registry slot + NvrmMem, freed on revoke or after drain | holder of OmegaHandle with matching epoch/gen/perms; GPU via VA while in flight | PHYSICS-allocated GPU-visible memory, CPU-mapped |
| Scratch arena slices | world; reset rewinds | anyone with the returned pointers | 2 MiB NvrmMem |
| JsReal / JsBranch | refcounts (branches, child recipes, index) | single thread | host heap; optional spill file / compressed buffer |
| CxStore objects | store (grow-realloc) | store owner | host heap arena |
| RxOmegaRealization exec pages | faculty store, faculty lifetime | faculty under its mutex; forked sandbox child executes | mmap RX pages |
| RxGenStore generation | on-disk files; active pointer file + rename flip | promotion only with native promotion right (r9 doc) | filesystem |

---

## 8. Contradictions (code vs doc)

1. omega_shared_world_abi.h:14-16 (text at :15) says the header "is placed in the PHYSICS repository so both compile against a single definition"; it lives at omega src/runtime/omega_shared_world_abi.h while Makefile:9 also puts `$(PHYSICS_DIR)` on the include path. Two copies can drift. [FACT]
2. Frozen ABI object table is OMEGA_SW_MAX_OBJECTS = 64 (omega_shared_world_abi.h:254; table offset field :231) but rx_coherent.c:23-28, 215-217 writes `count = RX_MAX_OBJECTS` (256) into a 256-entry RxProjectedTable at the frozen `off_object_table`. Acknowledged in rx_coherent.c:10-12. A consumer honouring the frozen ABI reads a different table size. [FACT]
3. omega_shared_world_abi.h describes head/tail as release-published atomics but declares them `volatile uint64_t` (omega_shared_world_abi.h:163, 166); code casts to `_Atomic` (rx_coherent.c:75-85) and elsewhere uses GCC builtins (rx_world.c:1810). [FACT]
4. spec/semantic-object.md:52 "fully relocatable ... without pointer patching or swizzling" vs pinned crumb log with raw pointers (rx_world.c:15-19) and JsReal pointer graph (rx_jspace.h:96-111). (Runtime realization vs semantic object; still a stated-property gap.) [FACT]
5. Frozen descriptor generations are u32 (omega_shared_world_abi.h:130 producer_generation, :132 object_generation) while capability generations are u64 (rx_caproot.h:118); workaround in payload bytes (rx_world.h:146-152) and 32-bit truncation in crumb hash (rx_world.c:147). Git log at 8a7d95e records "32-bit EffectPayload finding". [FACT]
6. ABI design law 1 (omega_shared_world_abi.h:21-28) "no field ever holds a ... GPU virtual address" is kept inside the shared world, but the packed SeatArgs launch record (rx_resident_gpu.c:34-43, 1065) carries the region's GPU VA; this is launch args, not the shared world, so not a violation — noted so the freeze wording scopes the law precisely. [FACT]

---

## 9. Recommended OSC-0B decisions

1. [PROPOSED] One memory model: Omega Systems Core adopts the C11/C++ model (relaxed/acquire/release/seq_cst) for CPU threads; `volatile` is banned for synchronization. Shared CPU/GPU cells are a distinct type (`device<atomic<T>>`) whose ops lower to acquire/release + the required DSB/membar pair; `dsb sy` appears only inside one unsafe physical block primitive. Replaces idioms 1-3 in §3a.
2. [PROPOSED] Publication contract freeze: SPSC ring with free-running u64 counters, producer release-stores tail after full slot write, consumer acquire-loads tail, then validates magic/epoch/generation/sequence/checksum; checksum stays defence-in-depth. Heartbeat/lease/marker words get the same named primitive. [SETTLED basis: omega_shared_world_abi.h laws 3-5; r12-resident-seat.md]
3. [PROPOSED] Seqlock is a library primitive with bounded retry and a dead-writer check; entries read through it are values, not references.
4. [PROPOSED] Two layouts, never mixed: (a) runtime layout = compiler's choice, unobservable; (b) wire/hardware layout = explicit `repr(wire)` with fixed width, LE, explicit reserved fields, compiler-checked size/offset. No digest may hash a runtime-layout struct; all digests go through the canonical encoder. Fixes §3b whole-struct hashes and RxCapEntry padding.
5. [PROPOSED] Failure semantics: Result-typed errors everywhere (matches today's return codes); OOM is a recoverable error in runtime paths; bounds violation in safe code is a trap (not silent truncation); stale generation/epoch is a typed error; device loss is a typed fault that moves an engine generation (seat/channel) and abandons uncommitted work; failed construction yields no object; cleanup failure marks the owner faulted (sticky) and is reported, never ignored; invariant violation = fail-closed error + evidence record, never continue.
6. [PROPOSED] Cycles: no owning cycles; parent/back links are `handle<T>` or `weak<T>`; refcounts only inside an arena/region with checked counters.
7. [PROPOSED] Identity: live handles (slot index+generation, cap slot+generation, VA, CPU address) are runtime-only and not serializable by type; durable records and digests use SemanticId<T> or an explicit durable id. [OPEN] migration of the R9 on-disk format and crumb digests that already embed slot handles.
8. [PROPOSED] Relocation: objects are movable by default; `pin<T>` is required for anything shared with another engine or holding OS sync primitives; moving a pinned region (bind_coherent) requires proof of no outstanding views and bumps a region generation. Arena growth invalidates borrows by type.
9. [PROPOSED] Generations are u64 end to end; ABI v2 widens descriptor generation fields (ends the payload split).
10. [PROPOSED] Every raw-pointer grant to the GPU (resolve_buffer, scratch_acquire) becomes a fence-scoped borrow: the region cannot be revoked/reset until the fence recorded for its last submission has passed.
11. [OPEN] Whether relocate_physical must refuse or invalidate while a resident claim is outstanding (§3a).
12. [OPEN] Whether the frozen ABI's 64-object table or the runtime's 256-slot projection is authoritative (Contradiction 2), and where the single copy of the ABI header lives (Contradiction 1).
