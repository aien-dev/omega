#ifndef OMEGA_V3_H
#define OMEGA_V3_H
#include "omega_verify.h"
#include "runtime/rx_execution_envelope.h"

typedef enum { OMEGA_V3_AUTHORITY, OMEGA_V3_WORLD, OMEGA_V3_EFFECT,
               OMEGA_V3_SUPPLY_CHAIN, OMEGA_V3_SECRET, OMEGA_V3_FOREIGN } OmegaV3AttackClass;
typedef enum { OMEGA_V3_CONTAINED, OMEGA_V3_ESCAPED, OMEGA_V3_UNAVAILABLE } OmegaV3Observed;
typedef struct {
    SemanticId program_id, realization_id;
    uint8_t envelope_identity[32];
    uint64_t world_generation;
    OmegaV3AttackClass attack_class;
    uint8_t attack_input_digest[32];
    uint32_t expected_invariant;
    OmegaV3Observed observed;
    uint64_t authority_attempted;
    uint64_t effect_attempted;
    uint32_t containment_boundary;
    uint32_t verdict; /* 0 pass, 1 fail, 2 required coverage unavailable */
    uint8_t evidence_root[32];
} OmegaV3Evidence;

typedef struct {
    uint32_t required, executed, contained, failed, required_skips;
    uint8_t evidence_root[32];
    bool passed;
} OmegaV3Report;

/* Qualification is passable only when all required cases actually ran and
 * every case was contained. This reducer does not claim an attack is safe. */
int omega_v3_reduce(const OmegaV3Evidence *cases, uint32_t count,
                    uint32_t required, OmegaV3Report *out);

#endif
