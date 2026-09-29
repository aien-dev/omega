#ifndef RX_EFFECT_IDENTITY_H
#define RX_EFFECT_IDENTITY_H

#include <stdint.h>
#include "../omega_types.h"

/* Canonical id for one authorized semantic effect. The authorization context
 * is included so a request cannot be moved between subjects, grants, Worlds,
 * or policies while retaining the same id. */
typedef struct {
    uint32_t subject;
    uint32_t capability_slot;
    uint64_t capability_generation;
    uint16_t resource_class;
    uint16_t operation_code;
    SemanticId target;
    uint8_t input_digest[32];
    uint64_t world_generation;
    SemanticId authorization_context;
    uint8_t idempotency_key[32];
} RxEffectIdentityInput;

int rx_effect_identity_compute(const RxEffectIdentityInput *in, uint8_t out[32]);

#endif
