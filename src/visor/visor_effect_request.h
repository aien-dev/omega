/*
 * visor_effect_request.h -- Omega Visor V1, lane 7: effect requests.
 *
 * Invariant: THE VISOR CAN ASK. THE VISOR CANNOT GRANT.
 *
 * A VisorEffectRequest is a read-only description of an existing canonical
 * KIND_EFFECT object, in the shape of an unauthorized EffectIntent. It is a
 * question, never an answer:
 *
 *   Visor -> EffectIntent -> AEGIS/PHYSICS authority -> bounded execution -> receipt
 *
 * Rules (spec/visor-authority.md):
 *  - `authorized` is always false. No function in the Visor sets it, and this
 *    header exports no setter. A consumer must never read `authorized == true`
 *    as permission: the only thing that may act on a request is the existing
 *    authority path OUTSIDE the Visor (rx_aegis_evaluate policy, the capability
 *    root validating the presented reference, the generation barrier's replay
 *    record, the World's publish checks). The Visor links none of them.
 *  - Building a request never mutates the graph. It recomputes the canonical
 *    digest and refuses (-1) an object whose stored SemanticId no longer
 *    matches its contents (edited after it was named/authorized).
 *  - The capability slot/generation/ref fields are copied as data. The Visor
 *    does not validate, mint, inspect or hold capabilities.
 *
 * This file is physics-free and runtime-free: it depends only on omega_types,
 * omega_canonical, omega_validate, omega_core (lookup) and sha256.
 */
#ifndef OMEGA_VISOR_EFFECT_REQUEST_H
#define OMEGA_VISOR_EFFECT_REQUEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "omega_types.h"

#define VISOR_EFFECT_PARAM_MAX 128
#define VISOR_EFFECT_STATUS_UNAUTHORIZED "UNAUTHORIZED_REQUEST"
#define VISOR_EFFECT_ROUTE \
    "Visor -> EffectIntent -> AEGIS/PHYSICS authority -> bounded execution -> receipt"

typedef struct {
    SemanticId effect_id;              /* id of the KIND_EFFECT object asked about */
    uint16_t resource_class;
    uint16_t operation_code;
    uint32_t capability_slot;          /* copied, never validated here */
    uint32_t capability_generation;    /* copied, never validated here */
    SemanticId capability_ref;         /* copied, never resolved here */
    uint16_t param_len;
    uint8_t param_bytes[VISOR_EFFECT_PARAM_MAX];
    uint8_t request_digest[32];        /* sha256 over the canonical effect object encoding */
    bool authorized;                   /* ALWAYS false; there is no setter */
    char status[64];                   /* "UNAUTHORIZED_REQUEST" */
    char route[128];                   /* VISOR_EFFECT_ROUTE */
} VisorEffectRequest;

/* Build a request from an existing KIND_EFFECT object in `g`.
 * Returns 0 on success; -1 (and *out zeroed, authorized=false) when the id is
 * missing, the object is not KIND_EFFECT, omega_validate_object rejects it,
 * payload_len != sizeof(EffectPayload), param_len > 128, the object has no id,
 * or the recomputed canonical digest differs from its stored SemanticId.
 * Never mutates `g`. */
int visor_effect_request_build(const OmegaGraph *g, const SemanticId *effect_object_id,
                               VisorEffectRequest *out);

/* *requires_authority = true when the graph reachable from `id` (relations and
 * payload references) contains a KIND_EFFECT object or a TYPE object tagged
 * TYPE_CAPABILITY_REF / TYPE_EFFECT_INTENT_REF / TYPE_EFFECT_RECEIPT_REF.
 * Returns 0, or -1 when `id` or any referenced non-zero id is missing from the
 * graph (fail closed: callers must refuse to run on -1 as well as on true). */
int visor_effect_request_classify(const OmegaGraph *g, const SemanticId *id,
                                  bool *requires_authority);

/* Deterministic rendering. Return 0, or -1 on bad args / truncation. */
int visor_effect_request_format_text(const VisorEffectRequest *r, char *out, size_t n);
int visor_effect_request_format_json(const VisorEffectRequest *r, char *out, size_t n);

#endif /* OMEGA_VISOR_EFFECT_REQUEST_H */
