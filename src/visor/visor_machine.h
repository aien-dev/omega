/*
 * visor_machine.h -- Omega Visor V1, lane 5: machine view (read-only).
 *
 * Shows the canonical OmegaMachineGraph the Visor realizes against. In V1 no
 * physics descriptor is ingested, so every view is an ASSUMED canonical
 * profile (observed=false); provenance records what the host reported
 * verbatim and where it does not match the profile's own labels.
 */
#ifndef OMEGA_VISOR_MACHINE_H
#define OMEGA_VISOR_MACHINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "omega_machine.h"

#define VISOR_MACHINE_MAX_TARGETS 4
#define VISOR_TARGET_AARCH64      "aarch64-v8a"
#define VISOR_TARGET_DGX_SPARK    "dgx-spark"
#define VISOR_TARGET_QEMU_VIRT    "qemu-virt"
#define VISOR_TARGET_BLACKWELL    "blackwell-gb10-sm121"

typedef struct {
    const char *type_name;
    uint32_t count, latency, throughput;
} VisorMachineUnit;

typedef struct {
    uint32_t level;
    bool is_instruction;
    uint64_t size;
    uint32_t line, assoc, latency;
} VisorMachineCache;

typedef struct {
    char name[32];
    char machine_id[72];
    bool linked;          /* code for this target is linked into this binary */
    bool runnable;        /* Visor `run` may execute realizations for it */
    const char *note;
} VisorMachineTarget;

typedef struct {
    char name[64];
    char id_text[72];
    uint8_t target_profile;
    bool observed;        /* false = assumed canonical profile (always in V1) */
    char provenance[384];
    uint32_t issue_width, max_in_flight;
    bool out_of_order;
    size_t unit_count;
    VisorMachineUnit units[OMEGA_MACHINE_MAX_UNITS];
    uint32_t gpr_count, gpr_width, vec_count, vec_width;
    size_t cache_count;
    VisorMachineCache caches[OMEGA_MACHINE_MAX_CACHES];
    uint64_t dram_base, dram_size;
    bool physics_authorized;          /* the graph's flag, as stored */
    bool physics_seal_is_placeholder; /* canonical builder constant seal, not a receipt */
    bool topology_valid;
    char topology_error[96];
    size_t target_count;
    VisorMachineTarget targets[VISOR_MACHINE_MAX_TARGETS];
} VisorMachineView;

/* Host facts used to pick a canonical profile (uname / DMI sysfs / cpuinfo). */
typedef struct {
    char uname_machine[32];
    char dmi_product[64];
    char cpu_parts[64];   /* distinct "CPU part" values, sorted, comma separated */
} VisorHostFacts;

int visor_machine_host_facts(VisorHostFacts *out);

/* Pick a canonical profile from host facts (pure; testable):
 * dgx_spark when uname==aarch64 AND (DMI product mentions DGX_Spark / DGX Spark
 * OR a CPU part is 0xd4f = Neoverse-V2), else qemu_virt. observed is ALWAYS false. */
int visor_machine_from_facts(const VisorHostFacts *f, OmegaMachineGraph *out_mg,
                             VisorMachineView *out_view);

/* visor_machine_from_facts(visor_machine_host_facts()). */
int visor_machine_current(OmegaMachineGraph *out_mg, VisorMachineView *out_view);

/* Describe any machine graph (observed=false, generic provenance). */
int visor_machine_view(const OmegaMachineGraph *mg, VisorMachineView *out);

/* Deterministic text / JSON. Return bytes written (excluding NUL) or -1 when truncated. */
int visor_machine_format_text(const VisorMachineView *v, char *out, size_t n);
int visor_machine_format_json(const VisorMachineView *v, char *out, size_t n);

/* Blackwell GB10 target id via omega_blackwell_get_machine_id when that object
 * is linked (weak reference). Returns 0 and fills id, or -1 when not linked. */
int visor_machine_blackwell_id(SemanticId *out_id);

const char *visor_machine_unit_name(ComputeUnitType t);

#endif /* OMEGA_VISOR_MACHINE_H */
