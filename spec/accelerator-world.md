# OMEGA Milestone 19 Architecture: Persistent Accelerator World

Milestone 19 transitions OMEGA from per-operation channel initialization to a persistent accelerator world runtime (OmegaAcceleratorWorld) on NVIDIA DGX Spark (Grace Blackwell GB10, sm_121).

## 1. Scope and Execution Boundary
- Target: NVIDIA DGX Spark (Grace Blackwell GB10, sm_121, 128 GiB unified LPDDR5x RAM).
- Substrate: M16 native libcuda-free channel via direct NVRM and UVM kernel ioctls.
- Primary Runtime Abstraction: OmegaAcceleratorWorld.
- Runtime Invariant: Resident != Immortal. The RM client, device context, VAS, and GPU channel persist across sustained heterogeneous workloads, while individual allocations, code objects, and channel generations remain bounded, explicit, mutable, and revokable.
- Foreign Dependency Invariant: Zero libcuda.so, zero libcudart.so, zero CUDA symbols, zero foreign userspace runtime mappings.

## 2. World Epoch and Two-Level Generation Model
To prevent ABA hazards without globally invalidating unrelated resident objects, the architecture enforces a two-level epoch and generation model.

### 2.1 World Epoch (WorldEpoch)
A 32-bit counter incremented only when the entire OmegaAcceleratorWorld lifecycle is initialized, reset, or reconstructed. All objects created within a world epoch inherit this epoch.

### 2.2 Object Generation (ObjectGeneration)
A 32-bit counter maintained per registry slot in the buffer and code registries. The generation counter increments whenever a slot is revoked, freed, or reused.

### 2.3 Capability Handle Structure
```text
Handle = {
    uint32_t world_epoch;
    uint32_t object_id;
    uint32_t object_generation;
    uint32_t object_type;
    uint32_t permissions;
}
```

### 2.4 Internal Address and Bounds Authority
The capability handle carries identity, generation, type, and permission flags. The external handle does not carry authoritative GPU virtual addresses or size bounds. The trusted buffer and code registries maintain the authoritative GPU VA, CPU mapping, and size. Address resolution occurs internally within OmegaAcceleratorWorld upon handle validation:

```text
handle.world_epoch == world.current_epoch
AND
handle.object_generation == registry[object_id].generation
AND
(handle.permissions & required_permissions) == required_permissions
AND
requested_offset + requested_span <= registry[object_id].size_bytes
```

Attempts to present stale handles (mismatched object generation), foreign handles (mismatched world epoch), or out-of-bounds offsets are rejected at the world boundary.

## 3. Persistent Substrate and Separation of Normal Residency from Fault Recovery

### 3.1 Normal Sustained Operation (Gate 8)
During normal sustained execution (including the 1,000-operation heterogeneous qualification schedule), the world maintains:
- Exactly 1 RM client root.
- Exactly 1 device and subdevice context.
- Exactly 1 virtual address space (VAS).
- Exactly 1 GPFIFO channel and pushbuffer ring.

No channel teardown, context recreation, or ioctl re-initialization occurs across dispatches.

### 3.2 Bounded Channel Fault Recovery (Gate 12)
If a hardware error or kernel-level channel fault occurs:
- The RM client, device context, and VAS remain resident.
- The channel is closed and reconstructed within the existing persistent VAS.
- The channel generation counter increments (channel_generation++).
- Object registrations and memory mappings in the VAS are preserved or audited.
Fault recovery is bounded to channel-scoped RM primitives. Arbitrary device-wide resets, driver unbinds, or raw MMIO tampering are strictly forbidden.

## 4. Live Queue Ring Discovery and Multi-Wrap Stress (Gate 7)
The channel GPFIFO capacity is discovered dynamically from the live channel configuration:
```text
discovered_ring_capacity = rm->entries
```
Gate 7 exercises at least 3x the discovered capacity:
```text
dispatch_count >= 3 * discovered_ring_capacity
```
Verification proves:
1. GPPut wraps modulo discovered_ring_capacity.
2. GPGet and execution progress advance monotonically.
3. Uncompleted entries are never overwritten (respecting entries - 1 flow control).
4. Sequence numbers remain strictly monotonic.
5. Completion markers correlate unambiguously to dispatch indices.
6. No stale descriptor or retired payload is replayed.

## 5. Registries and Scratch Arena

### 5.1 Buffer Registry
Maintains resident UVM buffers allocated via NVRM. Each entry tracks:
- object_id: uint32_t slot index.
- generation: uint32_t slot generation.
- gpu_va: uint64_t GPU virtual address.
- cpu_addr: void * coherent CPU virtual address.
- size_bytes: size_t allocation size.
- active: bool allocation state.
- permissions: uint32_t permitted access modes (READ, WRITE).

### 5.2 Code Registry
Maintains dynamically compiled sm_121 machine code blocks. Each entry tracks:
- code_id: uint32_t slot index.
- generation: uint32_t slot generation.
- gpu_va: uint64_t GPU virtual address.
- size_bytes: size_t code payload size.
- code_digest: uint8_t [32] SHA-256 digest of executable machine code.
- realization_id: uint8_t [32] 4-tuple realization identity binding.
- active: bool state.

### 5.3 Scratch Arena
A persistent pre-allocated UVM memory arena subdivided for transient descriptors:
- QMD launch descriptors (QMD0, QMD1).
- Driver constant bank (cbank) buffers and parameter blocks.
- Completion semaphores and status words.
Scratch slices are managed with bump or indexed slot allocations with zero OS or RM allocation overhead during steady-state dispatch.

## 6. Rolling State Digest Specification
The world state transitions deterministically across every dispatch. The rolling digest D_n is calculated using a canonical binary serialization over fixed-width fields:

```text
D_n = SHA256(
    domain_tag                    /* 32 bytes: "OMEGA_WORLD_ROLLING_STATE_V1\0..." */
 || D_(n-1)                       /* 32 bytes: previous rolling digest */
 || uint32_t world_epoch          /* 4 bytes */
 || uint64_t sequence_number      /* 8 bytes */
 || uint8_t  semantic_id[32]      /* 32 bytes */
 || uint8_t  realization_id[32]   /* 32 bytes */
 || uint8_t  code_digest[32]      /* 32 bytes */
 || uint8_t  input_bindings[32]   /* 32 bytes: SHA-256 of input buffer digests */
 || uint32_t obj_count            /* 4 bytes */
 || uint32_t obj_id_gen_pairs[K]  /* 8*K bytes: object_id + generation */
 || uint32_t capability_scope     /* 4 bytes */
 || uint32_t completion_value     /* 4 bytes */
 || uint8_t  result_digest[32]    /* 32 bytes: SHA-256 of output buffers */
 || uint32_t outcome_code         /* 4 bytes: 0 = SUCCESS */
)
```

Nondeterministic parameters (timestamps, host pointer addresses, process IDs) are strictly forbidden from the digest serialization.

## 7. Deterministic 1,000-Operation Sustained Qualification Schedule
The 1,000-operation sustained qualification workload executes 250 repetitions of a deterministic four-operation cycle:

1. VecAdd (vector integer addition, 1024 elements).
2. INT32 MatMul (integer matrix multiplication, M=16, N=16, K=16).
3. FP16 MatMul (tensor-core HMMA.16816.F32, M=16, N=16, K=16).
4. BF16 MatMul (tensor-core HMMA.16816.F32.BF16, M=16, N=16, K=16).

Total dispatches: 250 * 4 = 1,000 operations on a single persistent channel and VAS.

## 8. Milestone 19 Gate Definitions
- Gate 1: `OMEGA_ACCEL_RESIDENT_WORLD_CREATE_PASS`: Creation and deterministic initialization of persistent `OmegaAcceleratorWorld` substrate in coherent memory.
- Gate 2: `OMEGA_ACCEL_RESIDENT_CONTEXT_REUSE_PASS`: Successful dispatch of successive distinct compute tasks reusing the same persistent RM client, GPU device, and VAS aperture without re-initialization.
- Gate 3: `OMEGA_ACCEL_RESIDENT_CHANNEL_REUSE_PASS`: Successful reuse of GPFIFO channel, USERD, and doorbell mapping across successive pushbuffer submissions.
- Gate 4: `OMEGA_ACCEL_RESIDENT_CODE_REGISTRY_PASS`: Resident code registry registration, lookup, deduplication, and execution of distinct sm_121 kernels (VecAdd, MatMul INT32, MatMul FP16, MatMul BF16).
- Gate 5: `OMEGA_ACCEL_RESIDENT_BUFFER_REGISTRY_PASS`: Resident buffer registry managing persistent coherent memory buffers with handle tracking and bounds checking.
- Gate 6: `OMEGA_ACCEL_RESIDENT_MIXED_WORKLOAD_PASS`: Alternating execution of mixed workloads (Vector Add, INT32 MatMul, FP16 MatMul, BF16 MatMul) on the shared resident channel.
- Gate 7: `OMEGA_ACCEL_RESIDENT_QUEUE_WRAP_PASS`: GPFIFO pushbuffer ring buffer wraparound verified under continuous dispatch without channel stalls or pushbuffer corruption.
- Gate 8: `OMEGA_ACCEL_RESIDENT_1000_OP_PASS`: Sustained continuous execution of >= 1,000 heterogeneous operations on physical GB10 silicon without context teardown or re-initialization.
- Gate 9: `OMEGA_ACCEL_RESIDENT_GENERATION_PASS`: Monotonic generation counters on resident objects correctly incrementing upon lifecycle transitions and updates.
- Gate 10: `OMEGA_ACCEL_RESIDENT_STALE_HANDLE_REFUSAL_PASS`: Fail-closed refusal and error reporting when attempting dispatch with expired or stale capability handles from prior generations.
- Gate 11: `OMEGA_ACCEL_RESIDENT_REVOCATION_PASS`: Deterministic capability revocation immediately invalidating registered buffers or code handles, with subsequent access rejected.
- Gate 12: `OMEGA_ACCEL_RESIDENT_FAULT_RECOVERY_PASS`: Isolated channel reset and deterministic recovery upon fault injection without leaking system resources or corrupting device state.
- Gate 13: `OMEGA_ACCEL_RESIDENT_MEMORY_BOUND_PASS`: Flat, bounded resident memory consumption with zero heap or coherent memory leaks measured across 1,000 operations.
- Gate 14: `OMEGA_ACCEL_RESIDENT_STATE_DIGEST_PASS`: Deterministic rolling SHA-256 state digest verifying cumulative execution integrity across all completed operations.
- Gate 15: `OMEGA_ACCEL_RESIDENT_ZERO_LIBCUDA_PASS`: Zero foreign userspace runtime verification (ldd, nm -u, /proc/self/maps) throughout resident substrate lifecycle.
- Gate 16: `OMEGA_ACCEL_RESIDENT_CLEAN_CLONE_PASS`: Clean-clone isolated reproduction on DGX Spark silicon from scratch.
- Gate 17: `OMEGA_ACCEL_RESIDENT_REGRESSION_PASS`: Cumulative regression parity: 157 / 157 prior milestone gates passing (M4 through M18).
- Gate 18: `OMEGA_ACCEL_RESIDENT_RECEIPT_PASS`: Milestone 19 cryptographic qualification receipt generation with hardware trace evidence.

