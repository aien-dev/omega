#ifndef OMEGA_VERIFY_H
#define OMEGA_VERIFY_H

#include "omega_types.h"
#include "omega_realize.h"
#include <stdbool.h>

typedef enum {
    VERIFY_TIER_V0 = 0, /* Structural */
    VERIFY_TIER_V1 = 1, /* Differential */
    VERIFY_TIER_V2 = 2, /* Property / Invariant */
    VERIFY_TIER_V3 = 3, /* Adversarial qualification; see omega_v3.h */
    VERIFY_TIER_V4 = 4, /* Symbolic (stub) */
    VERIFY_TIER_V5 = 5  /* Proof-Carrying (stub) */
} VerifyTier;

typedef struct {
    bool passed;
    VerifyTier tier;
    uint32_t check_count;
    uint32_t fail_count;
    char error_detail[512];
} VerifyReport;

/* V0: Structural verification
 * Validates graph typing & DAG integrity (if graph != NULL)
 * Validates realization profile, size bounds, instruction stream, terminal RET (if real != NULL)
 */
int omega_verify_v0_structural(const OmegaGraph *graph, const RealizationObject *real, VerifyReport *report);

/* V1: Differential verification
 * For each 3-tuple (a, b, c) in test_inputs:
 * Computes reference semantic result and native realization result.
 * Checks exact equality.
 */
int omega_verify_v1_differential(const OmegaGraph *graph, const RealizationObject *real,
                                 const uint64_t *test_inputs, size_t input_triplet_count,
                                 VerifyReport *report);

/* V2: Property / Invariant verification
 * Evaluates declared properties:
 * - Commutativity: op(a, b) == op(b, a) for all declared commutative operations
 * - Identity element: op(a, identity) == a
 * - Overflow wrapping: validates 2^64-1 + 1 == 0 when OVERFLOW_WRAP declared
 * - Range preservation: output fits width
 */
int omega_verify_v2_properties(const OmegaGraph *graph, const RealizationObject *real, VerifyReport *report);

/* Master verification pipeline running V0 up to max_tier */
int omega_verify_pipeline(const OmegaGraph *graph, const RealizationObject *real,
                          VerifyTier max_tier, VerifyReport *report);

#endif /* OMEGA_VERIFY_H */
