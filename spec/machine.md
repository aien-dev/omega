# Specification: Milestone 13 — Omega Machine Graph (`OMEGA_MACHINE_GRAPH`)

```text
Document ID:     SPEC-OMEGA-M13
Milestone:       Milestone 13 (OMEGA_MACHINE_GRAPH)
Classification:  Sovereign Machine Canonical Specification
Target Substrate: Formal Machine Hardware Graph ($G_M$) Describing Execution Pipelines and Memory Hierarchies
Status:          SPECIFIED / IN PROGRESS (aien-dev/omega#18, aien-dev/aien-architecture#24)
Lineage:         SILICON -> ATLAS (M1) -> PHYSICS (M2/M3) -> OMEGA (M4-M13) -> AIEN
```

---

## 1. Executive Summary & Doctrine

Milestone 13 establishes the formal machine hardware graph substrate (`OMEGA_MACHINE_GRAPH`):

> **PHYSICS GOVERNS THE MACHINE; OMEGA MODELS ITS CAPACITIES. THE MACHINE GRAPH $G_M$ EXPOSES PHYSICAL REALITY AS AN AUTHORITATIVE, IMMUTABLE REASONING SURFACE WITHOUT DEFILING PURE SEMANTICS.**

In sovereign architecture:
- $G_S$ defines pure mathematical/computational meaning without machine commitments.
- $G_M$ defines the physical target machine: pipelines, functional execution units, register files, and cache/memory topologies, authorized by Physics.
- $G_R$ (M14) is synthesized from $G_S \times G_M \to G_R$, matching execution structures to physical hardware properties.

---

## 2. Core Invariants

1. **Physics Authority Ingress**: $G_M$ is initialized from capability-bounded physical descriptors verified by Physics.
2. **Canonical Machine Identity (`MACHINE_ID`)**: Every distinct hardware configuration possesses a deterministic, canonical SHA-256 identity.
3. **Execution Pipeline Modeling**: Explicit representation of issue width, dispatch ports, execution latency, and throughput per unit type.
4. **Memory Hierarchy Modeling**: Multi-level cache topologies (L1I, L1D, L2, L3) with capacity, line size, associativity, and cycle latency.
5. **Topology Disambiguation**: Heterogeneous microarchitectures (e.g., NVIDIA DGX Spark Neoverse V2 vs QEMU Cortex-A57) yield distinct `MACHINE_ID`s.
6. **Realization Cost Grounding**: $G_M$ provides empirical cost functions (cycle latency, register pressure limits) to guide optimal realization synthesis.

---

## 3. Data Structures & Declarations

```c
typedef enum {
    UNIT_ALU = 0,
    UNIT_BRANCH = 1,
    UNIT_MULTIPLIER = 2,
    UNIT_DIVIDER = 3,
    UNIT_LOAD_STORE = 4,
    UNIT_VECTOR = 5,
    UNIT_ACCELERATOR_PORT = 6
} ComputeUnitType;

typedef struct {
    ComputeUnitType type;
    uint32_t count;
    uint32_t latency_cycles;
    uint32_t throughput_per_cycle;
} MachineComputeUnit;

typedef struct {
    uint32_t issue_width;
    uint32_t max_in_flight;
    bool out_of_order;
    size_t unit_count;
    MachineComputeUnit units[8];
} MachinePipeline;

typedef struct {
    uint32_t gpr_count;         /* 31 for AArch64 X0-X30 */
    uint32_t gpr_width_bits;    /* 64-bit */
    uint32_t vector_count;     /* 32 for V0-V31 */
    uint32_t vector_width_bits; /* 128-bit for NEON / 256+ for SVE */
} MachineRegisterFile;

typedef struct {
    uint32_t level;             /* 1 = L1, 2 = L2, 3 = L3 */
    bool is_instruction;
    uint64_t size_bytes;
    uint32_t line_size_bytes;
    uint32_t associativity;
    uint32_t latency_cycles;
} MachineCacheLevel;

typedef struct {
    SemanticId machine_id;
    char name[64];
    uint8_t target_profile;
    MachinePipeline pipeline;
    MachineRegisterFile registers;
    size_t cache_count;
    MachineCacheLevel caches[8];
    uint64_t dram_base;
    uint64_t dram_size;
    bool is_physics_authorized;
    uint8_t physics_receipt_seal[32];
} OmegaMachineGraph;
```

---

## 4. Qualification Gates

1. `OMEGA_MACHINE_INIT_PASS`: Machine graph initialized with valid profile and bounds.
2. `OMEGA_MACHINE_PIPELINE_PASS`: Multi-unit execution pipeline modeled with issue width and latencies.
3. `OMEGA_MACHINE_REGISTER_FILE_PASS`: Register file capacities and widths validated.
4. `OMEGA_MACHINE_MEMORY_HIERARCHY_PASS`: Multi-tier cache hierarchy (L1I, L1D, L2, L3) modeled.
5. `OMEGA_MACHINE_PHYSICS_INGRESS_PASS`: Physical descriptor ingested and verified against Physics receipt.
6. `OMEGA_MACHINE_CANONICAL_ID_PASS`: Deterministic bit-for-bit `MACHINE_ID` generation.
7. `OMEGA_MACHINE_TOPOLOGY_DIFFERENCE_PASS`: Distinct hardware topologies yield distinct `MACHINE_ID`s.
8. `OMEGA_MACHINE_CYCLE_PREVENTION_PASS`: Topological validation ensures acyclic dependency across memory hierarchy.
9. `OMEGA_MACHINE_COST_EVALUATION_PASS`: Execution cost modeled accurately according to hardware parameters.
10. `OMEGA_MACHINE_RECEIPT_PASS`: Master qualification receipt generated with full provenance.
