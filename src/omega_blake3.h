#ifndef OMEGA_BLAKE3_H
#define OMEGA_BLAKE3_H

/* omega_blake3: BLAKE3 default (unkeyed) hash, 32-byte output, one shot. VC1 stage 4.
 *
 * Why it exists: an aien-proof EvidenceReceiptV1 id is BLAKE3 over the receipt's canonical
 * bytes (aien-protocols specs/evidence-receipt/SUBSET.md section 1), and the resolver must
 * recompute that id itself (SPEC 5.1 rule 1). Portable C, no SIMD, no allocation, no state.
 * Verified against the official BLAKE3 test vectors (tests/resolve/blake3_vectors.txt, from
 * BLAKE3-team/BLAKE3 test_vectors/test_vectors.json, input = 0,1,..,250 repeating). */

#include <stddef.h>
#include <stdint.h>

void omega_blake3_hash(const uint8_t *data, size_t len, uint8_t out[32]);

#endif /* OMEGA_BLAKE3_H */
