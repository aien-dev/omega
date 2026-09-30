#ifndef AEGIS_VERIFICATION_H
#define AEGIS_VERIFICATION_H

#include "forge_realization.h"
#include <stdbool.h>

/*
 * Omega Aegis Verification API:
 * Verifies that a lowered ForgeRealizationResult satisfies all contracts
 * and invariants before granting a ForgeVerifiedRealization token.
 */
int omega_aegis_verify(const OmegaRealizationRequest  *req,
                       const ForgeRealizationResult   *res,
                       const ForgeMachineDescriptor   *desc,
                       ForgeVerifiedRealization       *out_verified);

#endif /* AEGIS_VERIFICATION_H */
