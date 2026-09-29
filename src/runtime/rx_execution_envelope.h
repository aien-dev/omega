#ifndef RX_EXECUTION_ENVELOPE_H
#define RX_EXECUTION_ENVELOPE_H

/* SECURITY-1 derived execution view. All semantic and authority values remain
 * owned by OmegaProgram, RealizationObject, RxReactionDesc, RxWorld and the
 * capability root. This is not a world object or an authority encoding. */
#include "rx_world.h"
#include "rx_dependency_manifest.h"
#include "../omega_program.h"

#define RX_ENV_MAX_EFFECTS RX_MAX_DEPS

typedef enum {
    RX_SAFETY_VERIFIED = 0, RX_SAFETY_MEMORY_SAFE, RX_SAFETY_MANAGED_RUNTIME,
    RX_SAFETY_SANDBOXED_BYTECODE, RX_SAFETY_NATIVE_CONSTRAINED,
    RX_SAFETY_NATIVE_UNSAFE, RX_SAFETY_PHYSICAL_UNSAFE, RX_SAFETY_OPAQUE_FIRMWARE
} RxSafetyClass;

typedef enum {
    RX_ENV_ACCEPT = 0,
    RX_ENV_REFUSE_ARGUMENT,
    RX_ENV_REFUSE_PROGRAM_ID,
    RX_ENV_REFUSE_REALIZATION_ID,
    RX_ENV_REFUSE_WORLD_GENERATION,
    RX_ENV_REFUSE_SUBJECT,
    RX_ENV_REFUSE_CAPABILITY,
    RX_ENV_REFUSE_READ_SET,
    RX_ENV_REFUSE_WRITE_SET,
    RX_ENV_REFUSE_EFFECT,
    RX_ENV_REFUSE_RESOURCE,
    RX_ENV_REFUSE_VERIFICATION,
    RX_ENV_REFUSE_DEPENDENCY,
    RX_ENV_REFUSE_SAFETY
} RxEnvelopeResult;

typedef struct {
    /* References into canonical objects. */
    const OmegaProgram *program;
    const RxReactionDesc *reaction;
    const VerifyReport *verification;
    const RxDependencyManifest *dependencies;
    /* These are the existing identity fields, copied only to bind the view. */
    SemanticId program_id;
    SemanticId realization_id;
    uint32_t world_generation; /* object generation width, not cap/lineage gen */
    uint32_t required_verification_tier;
    RxSafetyClass safety_class;
    uint32_t n_effects;
    RxObjRef effects[RX_ENV_MAX_EFFECTS];
    uint8_t dependency_root[32];
    uint64_t lifetime_deadline;
    uint64_t causal_episode;
} RxExecutionEnvelope;

typedef struct {
    RxEnvelopeResult result;
    uint32_t subject;
    uint32_t world_generation;
    SemanticId program_id;
    SemanticId realization_id;
    uint32_t capability_id;
    uint64_t capability_generation;
    uint32_t invariant;
    uint32_t effect_object;
    uint64_t causal_episode;
} RxEnvelopeEvidence;

/* Checks identity and view structure; root authority remains the only source
 * of rights. current_world_generation is the caller's active semantic
 * generation, distinct from object and capability generations. */
RxEnvelopeResult rx_execution_envelope_validate(
    const RxExecutionEnvelope *env, const RxWorld *world,
    uint32_t current_world_generation, uint64_t now,
    uint64_t memory_budget, uint64_t compute_budget,
    RxEnvelopeEvidence *evidence);

#endif
