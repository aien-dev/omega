#ifndef FORGE_REALIZATION_H
#define FORGE_REALIZATION_H

#include "forge_descriptor.h"
#include "forge_types.h"
#include "nvrm.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Omega-side typed realization request wrapper.
 */
typedef struct {
    uint8_t                program_digest[32];
    uint32_t               opcode_count;
    const uint8_t         *ir_payload;
    size_t                 ir_size;
    ForgeMachineDescriptor machine_desc;
    uint8_t                descriptor_digest[32];
} OmegaRealizationRequest;

/*
 * Constructs an OmegaRealizationRequest bound to an observed ForgeMachineDescriptor.
 */
int omega_realization_request_init(OmegaRealizationRequest       *req,
                                  const uint8_t                 *program_digest,
                                  uint32_t                       opcode_count,
                                  const uint8_t                 *ir_payload,
                                  size_t                         ir_size,
                                  const ForgeMachineDescriptor  *desc);

/*
 * Dispatches realization lowering to FORGE.
 * Produces ForgeRealizationResult (unverified).
 */
int omega_forge_realize(const OmegaRealizationRequest *req, ForgeRealizationResult *out_res);

/*
 * Submits a verified realization to hardware through the typed FORGE boundary.
 * Strictly requires ForgeVerifiedRealization.
 */
int omega_forge_submit(Nvrm                           *rm,
                       const ForgeVerifiedRealization *verified_real,
                       const ForgeMachineDescriptor   *live_desc,
                       ForgeExecutionEvidence         *out_evidence);

#endif /* FORGE_REALIZATION_H */
