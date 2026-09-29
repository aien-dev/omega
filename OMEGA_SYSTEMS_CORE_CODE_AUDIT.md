# Omega Systems Core: code audit (OSC-0) and machine semantics freeze (OSC-0B)

Status: **PROPOSED, awaiting review.** Nothing in this document changes production code.
Decision record: [docs/adr/OMEGA-SYSTEMS-CORE-0000.md](docs/adr/OMEGA-SYSTEMS-CORE-0000.md).
Evidence appendices (worker reports, reviewed and spot-checked):
[omega-runtime](docs/osc/audit/omega-runtime.md) (cited below as **[OR]**),
[aienos-physics](docs/osc/audit/aienos-physics.md) (**[AP]**),
[language-compiler](docs/osc/audit/language-compiler.md) (**[LC]**).

Every claim carries one label:

- **FACT**: what the current code does, with a file:line reference.
- **SETTLED**: an architecture decision already made in a spec or ADR.
- **PROPOSED**: the proposed OSC-0B ruling, which becomes frozen when this document is accepted.
- **OPEN**: a question that the code and the settled architecture cannot answer yet. An OPEN item must not be decided silently by whichever implementation lands first.

Sequence (Drake, 2026-09-29):

`OSC-0 audit → OSC-0B machine semantics freeze (hard gate) → OSC-1 smallest compiler slice → OSC-2 substrate + first real C migration`

## Pins (reference truth)

| Repo | Commit | Notes |
|---|---|---|
| omega | 193a7e7 (origin/main) | audited in place |
| aienos | 8706fb8 (origin/main) | `native/capability` is byte-identical to Omega's `aienos.lock` 4c21386 (git diff empty) |
| physics | fecbedb (origin/main) | equals Omega's `physics.lock` |
| aien-architecture | local checkout, may lag origin | ADR 0013, 0014, and doctrine cited by [AP] |

FACT: the build does not enforce `aienos.lock`. The Makefile default is `AIENOS_R7_DIR ?= ../aienos-r9` (Makefile:185). On this host that checkout is at 187ecdf and has **no** `native/capability` directory, so a local R7 build needs an override. The audited authority is therefore the one pinned by the lock, not necessarily the one a local build links.

---

# Part I: Audit (OSC-0)

## I.1 Scope and size

| Code | Lines | Source |
|---|---|---|
| Omega C (src, tests, tools; 212 files) | 87,486 | [OR] §1 |
| AIENOS C + asm, non-vendored | 1,268 | [AP] §1.1 |
| AIENOS Rust (14 crates; migration input only, No-Rust rule) | 57,841 | [AP] §1 |
| PHYSICS C + asm, non-vendored | 9,399 | [AP] §1.2 |
| Vendored (not audited) | ~631k Rust crates + NVIDIA 580.173.02 headers | [AP] §1 |

## I.2 Classification (by lines, approximate)

| Class | Omega | AIENOS + PHYSICS |
|---|---|---|
| A semantic code | ~17k | ~0 |
| B safe systems code | ~9k | ~960 |
| C physical realization | ~10k (C and D together) | ~2,450 |
| D unavoidable unsafe hardware boundary | (in the row above) | ~900 |
| E temporary bootstrap: test harnesses, tools, gate code | ~37k + ~2.4k of gate code living in src | ~2,600 |
| F obsolete, to retire | fixed-output stand-ins (self_host, realize_synth) | ~1,125: the M15 software model; the Rust aienos-capability(+ffi); the M3 trap collector |

The per-file classification is in [OR] §1 and [AP] §1.

## I.3 Hazard counts

| Hazard | Omega | AIENOS + PHYSICS |
|---|---|---|
| malloc / calloc / realloc / free | 164 / 102 / 28 / 334 | 78 total, none in nvrm or m16 |
| `void*` | 312 | ~52 unsafe casts |
| Pointer arithmetic into mapped memory | 53 | see [AP] §2 |
| `goto` cleanup | 103 | 14 |
| `volatile` | 75 | see [AP] §3b |
| stdatomic / GCC atomic sites | 151 / 37 | authority: CAS plus pthread mutex |
| `dsb sy` | 18 | GPU submit path |
| Mutable globals | ~70 | 5 in nvrm.c, plus 1 function-local static |
| `_Static_assert` | 17 | **0** |
| Packed structs | 1 (SeatArgs, no asserts) | 0 |
| abort / assert / setjmp | 0 | 0 (PHYSICS asm panic-traps instead) |
| Separate generation / handle schemes | several (see I.5) | 8, in u32 and u64 widths |

## I.4 Worst hazards (reviewed)

For each hazard: where it is, what goes wrong, and what replaces it in Omega Systems Core.

1. **Untracked grants to the GPU.** omega_accelerator_world.c:257-290 and :424-451 (spot-checked).
   - `omega_world_resolve_buffer` checks epoch, generation, rights, and bounds correctly, then returns a raw CPU pointer and a GPU virtual address that nothing tracks.
   - Revoke frees memory at once when no submission is in flight (:254). `scratch_reset` (:448) rewinds while slices may still be held.
   - Replacement: a fence-scoped borrow (`slice<T>` bounded by `own<Fence>`), plus two-phase revocation (II.1).
2. **Free without device quiescence.** `nvrm_free` and `nvrm_channel_destroy` do not wait for the GPU (nvrm.c:674-706); capability revoke never unmaps or drains ([AP] §3f).
   - This contradicts the SETTLED rule in native-frame-authority.md §6.4.
   - Replacement: reclaim requires `own<Quiesced>`.
3. **Completion read without acquire.** m16_native.c:97-108 (spot-checked). The poll uses a plain volatile read, so later reads of GPU results can be reordered before the marker read on AArch64.
   - Replacement: `observe()` produces `own<Fence>` (II.2).
4. **Shared function-local static in `uvm_map`.** nvrm.c:440 (spot-checked). Every `Nvrm` context shares it, so concurrent contexts race.
   - Replacement: opaque `own<DeviceSession>`. Trivial C fix listed in I.8.
5. **Image moved under cached pointers.** rx_coherent.c:921-931 (spot-checked). `rx_world_bind_coherent` copies the coherent image and frees the old one; lock-free seat readers survive only because of call ordering ([OR] §3a).
   - Replacement: the image is `pin<T>`; moving it requires proof of no views and bumps the region generation (II.7).
6. **Live handles stored as durable identity.** rx_generation.c:560-565 (spot-checked) writes (slot id, generation, digest) rows to disk; crumb digests hash slot ids and generations (rx_world.c:139-156); `EffectPayload` embeds a live capability slot and generation inside a SemanticId payload (omega_types.h:160-167).
   - Replacement: see II.6.
7. **Identity hashes depend on layout.**
   - Whole-struct hashing: omega_program.c:276 (spot-checked; it also drops the last 32 bytes of the struct), omega_accelerator.c:83, omega_vector.c:23, omega_synthesis.c:98, and the M15 physics_accel.c:103,119.
   - Canonical payloads memcpy host structs (omega_core.c:84 ff).
   - Replacement: see II.3.
8. **Four memory-ordering idioms on one ABI** ([OR] §3a):
   - `volatile` cast to `_Atomic`, which is formally undefined behaviour;
   - GCC `__atomic` builtins;
   - `volatile` plus `dsb sy`;
   - a seqlock over a memfd.

   The seat flags (abort, marker_ok, sem_ok, lease_run) are `volatile`, not atomic.
9. **Unchecked construction and bounds.**
   - pthread_create and mutex_init results are ignored (rx_world.c:1397-1405).
   - `uint32_t pb[1024]` pushbuffers are filled without bounds checks (omega_blackwell_submit.c:131 ff).
   - Silent truncation (rx_world.c:171-176; omega_accelerator_world.c:550).
   - grow() doubles with no overflow check (rx_projection.c:143-152).
10. **Encoder silently masks immediates.** aarch64_encoder.c:114,122 (spot-checked). A misaligned or out-of-range offset is truncated instead of rejected, and SP and XZR share a register number in the API.

## I.5 Existing mechanisms to preserve (FACT)

- The pointer-free, offset-based shared world with epoch, generation, sequence, and checksum, and a hostile-input reader (omega_shared_world_abi.h:21-52; rx_coherent.c:287-398, 709-719).
- Generation checks that retire a saturated slot instead of wrapping (omega_accelerator_world.c:197-199, 248; aienos_capability.c:262-266).
- A handle epoch that is never reused (omega_accelerator_world.c:17-27); a capability restart floor above all earlier generations (aienos_capability.c:45-107).
- The `retiring` deferred free (omega_accelerator_world.c:84-93). CPU `mprotect(PROT_READ)` while the GPU owns a buffer, which is a hardware-enforced borrow (:659-665).
- Seat and producer generation fencing of a lost GPU engine (rx_world.c:1673-1680, 1814-1864).
- Snapshot-by-value reaction contexts (rx_world.h:185-222).
- Visor holds no runtime pointers (visor_world.h:60,73).
- Explicit-endian field encoders:
  - big-endian in the graph and plan code (rx_graph.c:20-30);
  - declared little-endian in the crumb and generation store (rx_world.c:44-66, 127-168; rx_generation.c:122-141).
- The capability rights model:
  - 10 rights; privileged rights never delegable; delegation only narrows.
  - Revoke cascades.
  - Stale, revoked, epoch, expired, and chain errors all fail closed (aienos_capability.c:143-303).
- The M2 stackless trap collector (vector_table.s:161-268) as the reference for capture-and-halt.
- W^X JIT pages plus a sandbox with canaries (rx_omega.c:137-152, 400-460).
- Verification tiers V0/V1/V2 and the rx_contract publish gate ([LC] §5).

## I.6 C ABI boundary that cannot disappear yet

- The 19 `aienos_cap_*` functions (17 called by Omega), through Omega's hand-copied `aienos_cap.h`. Only size and two offsets are checked (rx_native_bind.c:10-16).
- `nvrm.c` and `m16_native.c` compiled into Omega. `Nvrm` field layout is a de facto ABI because Omega reads its fields.
- NVIDIA RM/UVM ioctl headers (vendored).
- libc on the semantic path: `memcpy memset memcmp strcmp strlen strstr snprintf calloc` ([LC] §7). Executing generated code needs `mmap mprotect munmap sysconf __builtin___clear_cache`.
- The two lock files. `physics.lock` is honoured; `aienos.lock` is not (see Pins).

## I.7 Contradictions and rulings

| # | Contradiction | Ruling |
|---|---|---|
| C1 | Capability generation width. The C authority and Omega runtime use u64 that never wraps. AIENOS ADR 0013, the Rust kernel, spec/type-system.md:35, EffectPayload, shared-world descriptor :130/132, and PHYSICS M2/M3/M15 use u32. rx_world.h:146-152 smuggles the high halves; rx_world.c:147 hashes only the low 32 bits. | **SETTLED for Omega effects after the pin:** omega#71 (main 8e7a445, spec/effect-cap64-migration.md, 5 OMEGA_EFFECT_CAP64 gates PASS) made `EffectPayload.capability_generation` u64 with an explicit 178-byte big-endian v2 encoder (canonical version byte 0x02 for EFFECT). v1 effect bytes are refused, never reinterpreted. The crumb_hash low-32 truncation and type-system.md:35 are being fixed on that session's branch fix/crumb-cap-gen64. **DECIDED (Drake, 2026-09-29) for the rest: u64 end to end**, slot retired at max. This amends AIENOS ADR 0013 (amendment note in docs/adr/OMEGA-SYSTEMS-CORE-0000.md; the AIENOS-side edit is follow-up work OSC-2) and covers the Rust kernel, the shared-world descriptor :130/132, src/omega_accelerator.h, and PHYSICS M2/M3/M15. Note that #71 still puts the live slot and generation inside the EFFECT canonical payload, so II.6 stays PROPOSED. |
| C2 | canonical-encoding.md says the payload is always big-endian; omega_core.c copies host little-endian structs. | Both SETTLED documents stand, but for different layers (II.3). Canonical SemanticId payloads must move to explicit big-endian encoders. This is a deliberate identity break with a version bump (OSC-2 follow-up). |
| C3 | Two digest byte orders coexist (big-endian rx_graph and plan; little-endian crumb and generation store). | Not a contradiction once declared: they are different **WireLayouts**. The rule is that every wire format declares its byte order and is written by an encoder. |
| C4 | Who owns authority: the PHYSICS README ("PHYSICS AUTHORIZES", SMMUv3) versus ADR 0014 (AIENOS owns interrupts, capabilities, and DMA confinement; FORGE/PHYSICS is realization only). | **SETTLED by ADR 0014.** The PHYSICS M3 ledger and effect broker are class F legacy evidence. The README claim is stale. |
| C5 | Release requires DMA quiescence (native-frame-authority.md §6.4) versus release paths that do not wait. | SETTLED rule; the code violates it. PROPOSED II.1 encodes it in types. |
| C6 | TRUST.md:260 "OOM panic, core" versus error-code OOM everywhere in the code. | Resolved by context (II.4). An OOM with no possible recovery (core init, trap or IRQ) traps; runtime allocation paths return a typed error. |
| C7 | The shared-world ABI says 64 objects (omega_shared_world_abi.h:254); rx_coherent.c writes 256. The ABI header claims to live in PHYSICS but lives in Omega. | **OPEN** (ABI v2 question, not language semantics). |
| C8 | README "OMEGA is not a programming language/compiler" versus spec/omega-language-v0.md and self-host.md. | **DECIDED (Drake, 2026-09-29).** The README doctrine is retired: Omega is the reaction runtime and the compiler for Omega Systems Core (README.md updated; recorded in OMEGA-SYSTEMS-CORE-0000). The M6 "C1==C2==C3" result is a self-copy quine and is never evidence of compilation ([LC] §3). |
| C9 | semantic-object.md:52 "fully relocatable" versus the pinned crumb log and the JsReal pointer graph. | The statement holds for **semantic** objects, not runtime realizations. II.7 makes the difference explicit. |
| C10 | The ABI comments say head/tail are atomics, but they are declared `volatile uint64_t`. | PROPOSED II.2: a `device<atomic<u64>>` type replaces the declaration. |
| C11 | Omega pins aienos 4c21386, but the default local build directory lacks the code. | FACT recorded. Enforcing the lock is a follow-up (I.8). |

The full lists are in [OR] §8, [AP] §8, and [LC] "Contradictions".

## I.8 Recommended follow-ups (not done here; production edits are out of scope for this stage)

- nvrm.c:440: remove the function-local static.
- m16_native.c:103: make the successful marker read an acquire.
- omega_program.c:276: fix the SynthesisTask hash range.
- rx_world.c:1397-1405: check pthread results.
- Bound every `pb[n++]`.
- Remove `aienos_cap_force_generation` from the production ABI.
- `restart` must not revive a killed writer.
- Makefile: enforce `aienos.lock`.
- Put the shared-world ABI header in a single location.

Each of these is a separate, small, reviewable change, to be scheduled after this freeze.

## I.9 Candidate first real migration (OSC-2)

1. **crumbline**, [OR] §6 (recommended first):
   - `src/crumbline`, 1,216 lines, plus the learner tool.
   - CPU-only, deterministic, pointer-plus-length buffers, existing tests, no PHYSICS link.
   - Exercises `slice<T>`, `own<T>`, and `Result`.
2. **rx_cortex + rx_projection** (~1,300 lines). The main hazard is interior pointers invalidated by realloc, which is exactly what arenas and offset handles fix.
3. **visor_parse_command** (260 lines). The smallest first C ABI adapter.

Do not migrate first: rx_world, rx_coherent, rx_resident_gpu, the accelerator world, and canonical/sha256.

## I.10 Performance baseline

Host-only, CPU-side. Captured by a Sonnet 5.5 worker and sample-verified by the orchestrator. Full data and method: [baseline](docs/osc/audit/baseline.md).

Setup:
- gcc 13.3.0 at -O2 on aarch64, 20 cores.
- The machine was not quiet (load average 1–2.5).
- Build time is the median of 5 clean builds.
- Allocations are summed over every process a test starts.

| Binary | Build (s) | Size text+data+bss | Peak RSS (kB) | Allocs | Frees | Live at exit |
|---|---|---|---|---|---|---|
| omegatool | 1.20 | 367,386 | not run | not run | not run | not run |
| rx_heartbeat_test (R3) | 1.85 | 198,215 | 5,568 | 396 | 386 | 13 |
| rx_action_graph_test | 2.08 | ~504k | 4,212 | 445 | 435 | 13 |
| rx_state_projection_test | 2.26 | 169,543 | 120,480 | 83,026 | 105,057* | 13 |
| rx_plan_reuse_test (informational) | 2.06 | ~627k | 5,584 | 199,695 | 199,683 | 15 |
| rx_capability_query_test | 2.13 | ~555k | 119,628 | 23,687 | 23,699* | 11 |
| rx_typed_results_test | 2.16 | 3,453,513 | 21,284 | 19,932 | 19,922 | 13 |
| crumbline decode | 0.36 | ~1.13M | 1,328 | 4 | 3 | 1 |

\* Frees can exceed allocs because reallocations are counted separately.

A floor of about 10–13 live blocks (~7 KB) at exit appears in almost every test and has not been attributed to a cause, so it is **not** evidence of leaks.

**Not measured:**
- the GPU/accelerator workloads (silicon, R12–R15, M17);
- the R7–R11 targets, branch reuse, and the Visor targets;
- the M5–M19 gates;
- the crumbline conformance sweep;
- fragmentation beyond mallinfo2 at exit;
- throughput and latency (no host test prints them).

These gaps must be filled on a quiet machine before any OSC-15 comparison.

---

# Part II: Machine semantics freeze (OSC-0B)

The core principle is that **the replacement for the pointer is an object relationship.** Every object has three properties, tracked independently:

- **LIFETIME**: who keeps it alive;
- **AUTHORITY**: who may operate on it;
- **PLACEMENT**: where it is physically realized right now.

C folds all three into one address. Item II.12 is the rule that keeps them apart.

## II.1 Concurrency and publication (CPU, GPU, threads, interrupts)

**FACT.**
- RxWorld mutates under one mutex with up to 16 workers, plus seat launch and lease threads (rx_world.c:1402; rx_resident_gpu.c:950-1071).
- CPU→GPU goes through SPSC rings: the CPU writes a slot, seals it with a CRC, issues a release fence, and release-stores the tail (rx_coherent.c:721-733). The reverse direction uses an acquire-load of the tail and then validates magic, epoch, and checksum (:885-897).
- The non-resident path uses a pushbuffer, a doorbell, and a polled marker.
- PHYSICS has **no interrupts**; every exception captures and halts ([AP] §3a). AIENOS's (Rust) kernel handles only the timer IRQ.

**SETTLED.**
- The ring discipline: free-running u64 counters, release publish, fail-closed reader (omega_shared_world_abi.h:21-52; r12-resident-seat.md).
- Stale claims are refused after a seat or object generation moves (r12-resident-seat.md:34-70).
- Release requires DMA quiescence (native-frame-authority.md §6.4).
- AIENOS owns interrupts (ADR 0014).
- Cooperative "quit" instead of killing a channel (03d2820).

**PROPOSED.**
1. **No data races in safe code, by construction.** A value is either shared and immutable, or held by exactly one mutable owner or borrow. Cross-participant sharing is only through types: `atomic<T>`, `mutex<T>`, `seqlock<T>` (values out, bounded retry, dead-writer check), `ring<T>` (SPSC), and `device<T>` cells. Two engines can never both hold mutable access through addresses.
2. **Publication is a named pair.** A write becomes visible to another participant only through a *publish* (release semantics) matched by an *observe* (acquire semantics) on the same cell or ring. There is no other visibility guarantee, even on coherent memory.
3. **Completion is a value.** Device completion produces `own<Fence>` only from an acquire observation of a marker or semaphore. Retiring ring entries, reusing pushbuffers, and reclaiming memory all consume a Fence. Polling without acquire is not expressible in safe code.
4. **Revocation is two-phase.**
   - (a) *Revoke*: no new uses. Immediate; table state only.
   - (b) *Reclaim* of memory, virtual addresses, frames, or channels: requires `own<Quiesced>`, a proof that the device has drained (fence observed or channel torn down). On native hardware this also requires SMMU unmap, TLBI, and CMD_SYNC.

   Reclaim without the token is a type error.
5. **Generation change while a view is live.** Every borrow handed to another engine is scoped by a Fence. An object generation bump invalidates outstanding claims, records the event, and refuses late results (the r12 rule). A view does not survive a generation change.
6. **Interrupt context is an effect:** `ctx(irq)` and `ctx(trap)`. Code in these contexts cannot allocate, take a blocking lock, or call the authority's mutating API; the checker enforces this through the effect system. IRQ↔thread sharing only through atomics or IRQ-masked critical sections.
7. **Cancellation is cooperative.** It is a request observed at defined points, and it never tears memory away from a live view (generalizes 03d2820).

**OPEN.**
- Whether `rx_world_relocate_physical` must refuse or invalidate while a resident claim is outstanding (rx_coherent.c:661-700).
- How finely the runtime is divided into fault domains (see II.4).

## II.2 Atomics and memory ordering

**FACT.** There are four coexisting idioms (I.4 #8). The authority uses a mutex plus acquire-release CAS; nvrm has no lock. `dsb sy` is stronger than needed but correct ([AP] §3b).

**PROPOSED.** Omega owns its ordering vocabulary. Each operation is defined below, with its C11 equivalent and AArch64 lowering given for the bootstrap and for review. `volatile` is banned from safe code.

| Omega operation | Guarantee | C11 equivalent | AArch64 lowering |
|---|---|---|---|
| `load_relaxed` / `store_relaxed` | atomic, no ordering | relaxed | LDR / STR |
| `load_acquire` | later accesses stay after it | acquire | LDAR |
| `store_release` | earlier accesses stay before it | release | STLR |
| `rmw_acq_rel` (CAS, fetch_add) | both | acq_rel | CASAL / LDAXR+STLXR |
| `seq_cst` operations | single total order | seq_cst | LDAR/STLR (+ DMB ISH where needed) |
| `fence_acquire` / `fence_release` | standalone fences | atomic_thread_fence | DMB ISHLD / DMB ISH |
| `publish_device(cell)` | CPU writes visible to the device before the cell | (none) | DMB OSHST, then store; DSB ST before a doorbell MMIO store |
| `observe_device(cell) -> Fence` | device writes visible after the observation | (none) | LDAR (or LDR + DMB OSHLD) |
| `mmio_read` / `mmio_write` | exactly once, in order, never merged | (volatile) | LDR/STR to Device memory, DSB where the device requires it |
| `isb` | instruction stream resynchronized | (none) | ISB |
| `cache_clean(range)` / `cache_invalidate(range)` | non-coherent DMA visibility | (none) | DC CVAC / DC CIVAC + DSB |

The bootstrap may keep the stronger `dsb sy` inside these primitives until measurement justifies weakening it. The four current idioms each map onto rows of this table.

## II.3 Semantic, runtime, wire, and device layout

**FACT.**
- Canonical payloads are memcpy'd host structs (omega_core.c:84 ff; [LC] §3).
- Several digests hash whole native structs (I.4 #7).
- `RxCapEntry` has implicit padding and is shared across processes (rx_caproot.h:120-133).
- The only packed struct, SeatArgs, has no asserts.
- AIENOS and PHYSICS have 0 `_Static_assert`.
- Hardware formats are built with shifts, in little-endian.

**SETTLED.**
- canonical-encoding.md: big-endian canonical encoding for SemanticId.
- The shared world is little-endian "Zero Serialization" with fixed offsets (omega_shared_world_abi.h:30-38).
- AIENOS ADR 0013 and 0015 define explicit little-endian wire layouts.
- The V0 AST is disposable; identity attaches to the lowered form, never to syntax.

**PROPOSED.** Four layouts, never mixed:

- **SemanticLayout.** Abstract meaning; never observable. Identity comes only from its canonical encoding.
- **RuntimeLayout.** Chosen by the compiler; unobservable in safe code. No hashing, serialization, or foreign exposure of runtime-layout values.
- **WireLayout.** Every byte that is hashed, persisted, or put in a receipt comes from a generated encoder with a declared byte order. The SemanticId encoding is big-endian (SETTLED); evidence and store formats may be little-endian if declared. There is no implicit padding: reserved bytes are explicit and zero. Raw struct hashing is forbidden.
- **DeviceLayout.** Declared with explicit width, offsets, byte order, and alignment. Used for hardware formats, shared-world records, and C ABI structs. The compiler checks size and offsets, and generates C headers with `_Static_assert`. This replaces hand-copied headers such as `aienos_cap.h`.

Enums and tagged unions have an explicit tag width wherever they appear in wire or device layouts.

## II.4 Failure semantics

**FACT.**
- There are no aborts or asserts; everything returns negative error codes ([OR] §3c, [AP] §3e).
- PHYSICS panic-traps on ingress and invariant failures, and returns 0 when frames run out.
- nvrm has a sticky `faulted` flag and quarantines virtual addresses.
- Device loss is seen only as a timeout, because the RM error notifier is never read (nvrm.c:629,635).

**SETTLED.**
- Fail closed.
- "No action without evidence": a full crumb log refuses action (rx_world.c:179-183).
- Deterministic crash records (AIENOS ADR 0006).
- TRUST.md: OOM in the core is catastrophic.

**PROPOSED.** No stack-unwinding exceptions. Expected failures are typed `Result` errors that the caller must handle. Invariant violations are deterministic **traps**.

| Failure | Ordinary code | `ctx(trap)` / `ctx(irq)` / core init |
|---|---|---|
| Out of memory / exhaustion | `Err(NoMem)` / `Err(Exhausted)` | error value where a caller exists; trap only where no recovery path exists (core init) |
| Bounds, dynamic | `a[i]` traps; `a.get(i)` returns `Option` | trap |
| Integer overflow (default ops) | trap | trap |
| Stale generation / epoch | `Err(Stale{object, held_gen, current_gen})` | error value, fail closed |
| Device vanished | `Err(DeviceLost)`, sticky per device session, sourced from the error notifier *and* timeouts | trap if no session owner can record it |
| Failed construction | no object exists; fields already built are destroyed in reverse order | same |
| Cleanup failure | device release is an explicit must-use `Result`. A device object dropped without release goes to quarantine (retiring) and its owner is marked faulted. Never ignored | same |
| Invariant violation | trap plus an evidence record (crumb or fault mailbox) | stackless capture and halt (the M2 collector is the reference) |

A **trap** is a deterministic fail-stop of the enclosing fault domain, recorded as evidence. In the bootstrap, that domain is the process.

**OPEN.** Finer-grained fault domains: per reaction, per task, or per world.

## II.5 Cyclic object graphs and weak references

**FACT.**
- The only owning pointer graph is the refcounted JsReal DAG; its `refs` counter has no overflow check (rx_jspace.h:96-111).
- Capability chains, crumb parents, and cortex links are ids, and acyclic by construction.
- Back-pointers such as `RxGpuSeat.world` (rx_resident_gpu.c:59) are not tied to a lifetime: destroying the world before the seat is a use-after-free.

**SETTLED.** The content-addressed graph model: relations by SemanticId (semantic-object.md).

**PROPOSED.**
- **Ownership forms a forest.** No `own<T>` cycle is expressible.
- Graph-shaped data is owned by a pool, arena, or graph store. Edges are `handle<T>`, `weak<T>`, or `SemanticId<T>`, never owning.
- Reference counting exists only as an explicit `rc<T>` inside a region, with checked counters and an acyclicity rule. It is never the default.
- A back-reference to an owner is a borrow with a lifetime tie. For example, the seat borrows the world, so destroying the world while the seat lives is a compile error.
- `weak<T>` is fallible: resolving it returns `Option`, checked by generation.

## II.6 Live handles versus persistent identity

**FACT.** The distinct types already exist ([LC] §3):

- `SemanticId`: content identity.
- `OmegaHandle{epoch, object_id, u32 generation, type, rights}`: live accelerator handle.
- `RxCapRef` / `AienosCapRef{id, u64 generation}`: live capability references.

However, live handles *do* enter durable identity: in EffectPayload, in generation-store rows, and in crumb digests (I.4 #6). No CPU address or GPU virtual address was found in any durable record.

**PROPOSED.** These are distinct, non-interchangeable types:

| Type | Meaning | Durable? |
|---|---|---|
| `SemanticId<T>` | identity across time, storage, and machines (hash of the canonical encoding) | yes |
| `PersistentRef<T>` | resolvable after relocation or reboot (SemanticId or declared durable id, plus store generation) | yes |
| `Handle<T>` | live identity inside a resident generation: {slot, u64 generation, world epoch, type, rights} | **no** |
| `&T` / `&mut T` | temporary access permission | no |
| `slice<T>` | bounded temporary view | no |
| `pin<T>` | intentionally fixed physical realization | no |
| `DeviceRef<T>` | capability-bound physical resource | no |

Rules:
- Non-durable types (handles, borrows, slices, pins, device references, addresses) are **statically rejected** by the canonical encoder and by every durable-store or receipt writer.
- The only conversion is an explicit `persist(h) -> SemanticId/PersistentRef`, which resolves the live object.
- A handle carries its world epoch, so a handle from an earlier boot fails closed (the existing epoch pattern).
- Generation width: see C1 (u64 end to end, DECIDED 2026-09-29).

**DECIDED (Drake, 2026-09-29).** Migrating the existing on-disk R9 generation store, crumb digests, and EffectPayload is an identity break. It is scheduled for OSC-2, right after the compiler slice, as a versioned transition: a new canonical version byte, and old records are refused, never reinterpreted (the omega#71 effect-cap64 pattern). Constraint: TURING records hash omega_canonical_encode OMG0 bytes, so any OMG0 encoder change (including the C2 big-endian move) ships with a TURING record version bump and a verification path for existing golden digests. See OMEGA-SYSTEMS-CORE-0000, "Decisions 2026-09-29", item 4.

## II.7 Relocation, compaction, and pinning

**FACT.**
- The shared world uses offsets and is relocatable (omega_shared_world_abi.h:207).
- Pinned by design: the crumb log, RxWorld (embedded mutex), GPU memory at identity virtual addresses, JIT pages, and PHYSICS fixed regions.
- Relocations that break interior pointers: arena realloc (rx_projection.c:526), JsBranch units, and bind_coherent.

**PROPOSED.**
- **Movable by default.** Long-lived objects are reached through handles, so the runtime may compact pools by rebinding slot→placement without changing the handles.
- **`pin<T>` is rare, and created only in `unsafe physical`.** It is required for:
  - memory shared with another engine;
  - OS or hardware synchronization objects;
  - fixed physical regions;
  - executable pages;
  - logs handed out by address.
- **Moving a pinned region** requires a proof of no outstanding views (all Fences observed, all borrows ended), and it bumps the region generation.
- **Arena growth** is a mutation of the arena, so it is a compile error while any borrow into the arena is live. Long-lived references into arenas are offsets or handles.

## II.8 MMIO, DMA, volatile, interrupts, cache, and no-block contexts

**FACT.**
- PHYSICS runs with the MMU off (Device memory), has no cache maintenance, and has no SMMU code; Linux owns the SMMU for now.
- GPU memory is coherent and snooped.
- GPU virtual address equals CPU virtual address by construction (nvrm.c:485-489, 433-437).
- Atlas hands off without an I-cache synchronization.
- The PL011 UART trap path busy-waits by design.

**SETTLED.**
- AIENOS owns DMA confinement and devices (ADR 0014).
- Reclaim requires SMMU unmap, TLBI, and SYNC (native-frame-authority.md §6.4).
- On v1, the boot CPU alone owns the frame authority (§6.5).

**PROPOSED.**
- **Physical code lives in `unsafe physical` blocks.** Every such block must declare:
  - `requires capability<...>`, alignment, and range;
  - `ensures ...`;
  - an audit record, negative tests, and bounds, ownership, and capability documentation.
- **Registers** are `mmio<Reg>`, accessed only through `mmio_read` / `mmio_write` (II.2).
- **DMA buffers** are `device<T>` over `pin<T>`, with placement recorded. Non-coherent placements require `cache_clean` before the device reads and `cache_invalidate` after the device writes.
- **No-block / no-allocate contexts** are the `ctx(irq)` / `ctx(trap)` effects (II.1).

**OPEN.**
- Whether native GB10 system memory is coherent for every GPU engine (native-frame-authority.md §9 says "assumed").
- Whether GPU-uncached allocations need CPU-side cache maintenance natively.
- The Atlas I-cache synchronization once caches are on natively (atlas_m2.s:130-134).
- That RM keeps pages pinned for the life of the memory object is an assumed NVIDIA contract, not verified.

## II.9 Deterministic, reproducible compilation

**FACT.**
- Build flags: `-std=gnu11 -O2 -Wall -Wextra -Werror`, with no `-ffile-prefix-map`, no SOURCE_DATE_EPOCH, and no build-twice check.
- Makefile:14 bakes in `OMEGA_PHYSICS_DIR`.
- Receipts embed wall time and the git sha by design.
- Capability generations seeded from boot time flow into crumb digests (rx_caproot.c:52-73; rx_world.c:145-149).
- M6 "C1==C2==C3" holds by self-copy, not by compilation (tests/run_m6_gates.sh:78-83).

**PROPOSED.**
- Compiler output is a pure function of (source bytes, compiler identity digest, target profile, declared flags):
  - no timestamps, paths, or environment in the output;
  - no hash-iteration-order dependence (ordered containers only).
- The verified IR ("Flow IR", which does not exist yet; the name is free) has a canonical encoding and a digest.
- Every build writes a receipt with the compiler digest, input digests, and output digest.
- Required gates:
  - **build twice, compare bytes**;
  - a differential oracle (the C backend) whose output is never identity;
  - **bootstrap equivalence**, defined as `compiler_n` built by `compiler_{n-1}` producing byte-identical outputs on a fixed test corpus. Self-copy is never evidence.
- Bootstrap C builds add `-ffile-prefix-map`.
- Nondeterministic inputs (boot-time generations, run ids) are excluded from semantic digests.

## II.10 Minimal standard substrate

**FACT.** On the semantic path, Omega leans only on `memcpy memset memcmp strcmp strlen strstr snprintf calloc`. SHA-256 is in-house. printf, fopen, qsort, and malloc live in gates and tools ([LC] §7).

**PROPOSED.** The v0 substrate (so that production code has no reason to fall back to C):

- `Option`, `Result`;
- checked, `wrap_`, `sat_`, and `overflowing_` integers;
- `slice<T>`, `bytes`, `str` (validated UTF-8, bounded);
- `vec<T>` (owned, growable, checked), fixed arrays, `ring<T>`;
- `arena` / `region`, `pool<T>` with `Handle<T>`;
- an ordered map and a deterministic hash map (iteration order independent of hashing);
- total-order sort;
- SHA-256 and the canonical encoder/decoder;
- the II.2 atomics vocabulary, `mutex<T>`, `seqlock<T>`, `Fence`;
- formatted output for receipts;
- file I/O through a capability.

It is built in C first as the bootstrap runtime, then ported.

## II.11 Executable model of the memory rules

**FACT.** No fuzzer or property generator exists in the language or core ([LC] §5). V1 differential testing (native versus `omega_eval`) is the existing oracle pattern.

**PROPOSED.** A small executable reference model, in C, is the **OSC-0B exit gate** (it is not compiler work). It is a state machine over objects, slots, generations, regions, borrows, and publication.

Object states: `unallocated → owned → {borrowed_shared(n) | borrowed_mut} → moved | released`. A slot moves `free(g) → live(g) → free(g+1)`, and at the maximum generation it becomes `retired`. Regions go `open → destroyed`. Cells go `written → published(release) → observed(acquire)`.

Valid transitions are the ones this document allows. Named **invalid** transitions, each of which must be rejected deterministically:

- use after move
- use after release
- double release
- stale-generation use
- generation wrap
- mutable alias (a second `&mut` or mixed `&`/`&mut`)
- borrow outlives owner
- arena escape (reference outlives its region)
- reclaim without `Quiesced`
- read of device data before `observe`
- publish without release
- forged rights (rights wider than the parent)
- live handle written to a durable encoder

A seeded, deterministic random-sequence generator drives the model; later, OSC-1/2 compiler output is compared against it (V1 style). Launched only after this document is reviewed.

## II.12 Lifetime, authority, and placement per object

**FACT.** Today the three are tangled: a raw pointer grants all three. The current per-object tables are in [OR] §7 and [AP] §7.

**PROPOSED.** The compiler and runtime track them independently:

- **Lifetime** is in the type: inline, `own<T>`, borrow, region, `Handle<T>` pool, static, device, or persistent.
- **Authority** is in rights: `Handle<T>` rights and capabilities. Delegation narrows only (SETTLED, aienos_capability.c:240-254). Mutability is authority.
- **Placement** is realization metadata, not type, unless the object is `pin<T>` or `device<T>`. PHYSICS may change placement without changing meaning.

Example:

```
Tensor T:
  identity:  SemanticId 84A7...
  lifetime:  World generation 9182 (pool handle)
  authority: CPU read; GPU read+mutate (capability 17 gen 5)
  placement: unified physical region 31 (pinned while a GPU claim is live)
  mutation epoch: GPU / generation 217
```

---

# Part III: The OSC-0 contract

## III.1 Reference and type taxonomy

| Form | Meaning |
|---|---|
| `T` | value owned inline |
| `own<T>` | unique dynamic ownership |
| `&T` / `&mut T` | shared or exclusive bounded borrow |
| `slice<T>` | bounded contiguous view |
| `arena` / `region` | objects sharing one explicit lifetime epoch |
| `Handle<T>` | live, generation-checked logical reference |
| `weak<T>` | non-owning fallible reference |
| `SemanticId<T>`, `PersistentRef<T>` | durable identity (II.6) |
| `pin<T>` | non-relocatable physical realization |
| `device<T>`, `DeviceRef<T>`, `mmio<R>` | capability-controlled physical objects |
| `Fence`, `Quiesced` | linear proofs of completion and drain (II.1) |

Safe code has no `T*`, `void*`, address arithmetic, arbitrary casts, manual `free`, array decay, null dereference, uninitialized reads, unbounded strings, silent overflow, implicit ownership transfer, or unrestricted mutable aliasing.

## III.2 Ownership and lifetime rule

Every live object has exactly one mechanically known lifetime strategy: stack, unique, region, generation pool, static, device, or persistent. Destruction is deterministic with the owner unless the object is explicitly moved into another domain. There is no default tracing GC. Preferred order: stack → own → arena → generation pool → content-addressed → persistent store → capability-controlled device.

## III.3 Integers and aliasing

- Default arithmetic is checked (trap on overflow). Explicit `wrap_add`, `sat_add`, and `overflowing_add` are available. There are no implicit promotions.
- The existing V0 unsigned `WRAP` stays for backward compatibility; the new signed types use `OVERFLOW_FAIL_CLOSED` (an existing enum value).
- Aliasing: many readers **or** one mutable. Mutability is authority.

## III.4 Unsafe physical boundary

The only place raw addresses exist is inside `unsafe physical { ... }` with the declarations required by II.8. Unsafe is the implementation surface for physical primitives (`pin`, `device`, `mmio`, DMA, fences), not a switch to turn the language off.

## III.5 C ABI boundary

Foreign declarations must state, per parameter:

- borrowed or owned;
- nullable;
- length source;
- mutability;
- lifetime;
- thread safety.

Unknown means unsafe. No unannotated C pointer enters safe code as a trusted reference. Layouts crossing the boundary are DeviceLayout, and the compiler generates the C headers with asserts. The first adapters are `aienos_cap_*` and `visor_parse_command`.

## III.6 Compiler pipeline and backend

1. lexer/parser, **extending V0's grammar**: V0 already reserves `i64 mut struct if while return unsafe cap`, so no competing grammar is invented ([LC] §1).
2. typed AST (disposable)
3. name/type resolution
4. ownership graph
5. borrow/lifetime analysis
6. effect/capability analysis (`ctx(...)`, `requires`, `effects`)
7. verified typed IR ("Flow IR", canonical encoding plus digest)
8. realization

The backend is **direct AArch64 through the in-repo encoder**, after hardening: reject masked immediates, separate SP from XZR, and add ADDS/SUBS, LDP/STP, BL/BLR, SMULH, and stack frames, with every instruction mirrored in the verifier's decoder. Estimate: 4–7 worker-days. A **C-emitting backend compiled by system cc** is allowed only as a differential oracle, never as shipped output or semantics.

Diagnostics must name the object, lifetime, borrow, conflicting access, generation, capability, origin, and the attempted transition.

**DECIDED (Drake, 2026-09-29).** In OSC-1, `requires` / `ensures` are text only: recorded, not checked. Enforcement is OSC-2 and is MANDATORY. No production C migrates into Omega Systems Core until enforcement is on.

## III.7 Migration order

- **OSC-0** (this document)
- **OSC-0B** (Part II, plus the II.11 model as the exit gate)
- **OSC-1:** the smallest vertical slice. Source → parser → typed value → unique allocation → borrow → bounds check → deterministic destruction → native execution. It covers the scope of the original OSC-1 through OSC-3.
- **OSC-2:** the substrate (II.10), arenas, and generational handles. The first real migration is crumbline (I.9). Covers the original OSC-4, OSC-5, and OSC-9/10.
- **Later**, in their original meaning:
  - unsafe-physical and capability effects (OSC-6/7);
  - Flow IR lowering (OSC-8);
  - accelerator registry / object world (OSC-11);
  - remaining safe C (OSC-12);
  - residual unsafe (OSC-13);
  - self-host (OSC-14);
  - qualification (OSC-15);
  - canonical (OSC-16).

A milestone never passes on source code existing alone.

## III.8 Acceptance tests

**OSC-1 slice** (in `tests/osc/`):
- A valid program runs natively.
- These are **compile** failures:
  - use after move;
  - mutable alias;
  - borrow outlives owner;
  - statically known out-of-bounds.
- Dynamic out-of-bounds is a **deterministic runtime trap**.
- Forgotten free is inexpressible in safe syntax.

**Breaker lane:** additionally stale generation, arena escape, double release, integer overflow, forged rights, bad C ownership annotation, null FFI result, generation wrap, and cross-engine stale reference.

Each negative test asserts the exact diagnostic fields from III.6.

**OSC-0B exit:** the II.11 model rejects every named invalid transition across at least 10^6 seeded random sequences, with zero false accepts. The seed and counts are recorded in a receipt.

---

## Checklist

**§22 items (OSC-0 contract):**
1. live audit: Part I
2. hazard inventory: I.3, I.4, and the appendices
3. ownership model: III.2
4. taxonomy: III.1, II.6
5. unsafe boundary: III.4, II.8
6. C ABI boundary: I.6, III.5
7. pipeline: III.6
8. migration order: III.7
9. benchmark baseline: I.10 (host-only; GPU gaps listed)
10. acceptance tests: III.8

**OSC-0B items (Drake's 12):**
1. concurrency: II.1
2. atomics: II.2
3. layouts: II.3
4. failures: II.4
5. cycles: II.5
6. live versus persistent: II.6
7. relocation and pinning: II.7
8. MMIO, DMA, and contexts: II.8
9. determinism: II.9
10. substrate: II.10
11. executable model: II.11
12. lifetime, authority, and placement: II.12

**Decisions taken by Drake, 2026-09-29 (freeze accepted; see OMEGA-SYSTEMS-CORE-0000):**
- C1: DECIDED. u64 generations end to end, slot retired at max; amends AIENOS ADR 0013 (AIENOS edit is OSC-2).
- C8: DECIDED. README non-compiler doctrine retired; Omega is the reaction runtime and the compiler for Omega Systems Core.
- III.6: DECIDED. Contracts are text only in OSC-1; enforcement in OSC-2 is mandatory before any production C migrates.
- II.6: DECIDED. Identity break in OSC-2 right after the compiler slice; versioned, old records refused; TURING version bump and golden-digest verification path required.
