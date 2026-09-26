#ifndef OMEGA_MACHINE_H
#define OMEGA_MACHINE_H

#include "omega_types.h"
#include "omega_realize.h"
#include <stdbool.h>
#include <stddef.h>

#define OMEGA_MACHINE_MAX_UNITS 8
#define OMEGA_MACHINE_MAX_CACHES 8

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
    MachineComputeUnit units[OMEGA_MACHINE_MAX_UNITS];
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
    MachineCacheLevel caches[OMEGA_MACHINE_MAX_CACHES];
    uint64_t dram_base;
    uint64_t dram_size;
    bool is_physics_authorized;
    uint8_t physics_receipt_seal[32];
} OmegaMachineGraph;

/* Raw physical machine descriptor packet ingested from Physics */
typedef struct {
    uint32_t magic;             /* 0x4D414348 = 'MACH' */
    uint16_t version;
    uint16_t target_profile;
    char cpu_name[32];
    uint32_t issue_width;
    uint32_t gpr_count;
    uint32_t vec_count;
    uint64_t l1d_size;
    uint64_t l2_size;
    uint64_t l3_size;
    uint64_t dram_base;
    uint64_t dram_size;
    uint8_t physics_seal[32];
} PhysicsDescriptor;

/* Initialize empty machine graph */
void omega_machine_init(OmegaMachineGraph *mg, const char *name, uint8_t target_profile);

/* Compute canonical MACHINE_ID via SHA-256 over canonical hardware encoding */
int omega_machine_compute_id(OmegaMachineGraph *mg);

/* Construct canonical NVIDIA DGX Spark Grace profile (Neoverse V2, 4-wide dispatch) */
int omega_machine_build_dgx_spark(OmegaMachineGraph *mg);

/* Construct canonical QEMU virt AArch64 baseline profile (2-wide dispatch) */
int omega_machine_build_qemu_virt(OmegaMachineGraph *mg);

/* Ingest Physics hardware descriptor and bind Physics authority seal */
int omega_machine_ingest_physics_descriptor(OmegaMachineGraph *mg, const PhysicsDescriptor *desc);

/* Validate topological consistency (cache ordering, alignment, non-zero capacities) */
int omega_machine_validate_topology(const OmegaMachineGraph *mg, char *err_msg, size_t err_msg_len);

/* Estimate execution latency in machine cycles for a given realization on target hardware */
uint32_t omega_machine_estimate_latency(const OmegaMachineGraph *mg, const RealizationObject *real);

#endif /* OMEGA_MACHINE_H */
