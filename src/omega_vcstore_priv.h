#ifndef OMEGA_VCSTORE_PRIV_H
#define OMEGA_VCSTORE_PRIV_H

/* PRIVATE to the Verified Crumb Store and its one admission door (VC1 stage 6, ADR 0029
 * Decisions 5, 7 and 8).
 *
 * The two raw insert functions are not in omega_vcstore.h on purpose. A record may enter a store
 * only through omega_resolve_admit (receipt checked) or omega_resolve_admit_genesis (member of
 * VC-GENESIS-1 only), both in omega_resolve.c. The only files that may include this header are
 *   src/omega_vcstore.c   (defines them)
 *   src/omega_resolve.c   (the admission door)
 * and test code under tests/. tests/test_omega_resolve.c scans src/ and tools/ and fails if any
 * other file includes this header or calls either function.
 *
 * The raw functions themselves enforce what the store can see: format, immutability,
 * dependencies present, and (BOOTSTRAP only) membership of the semantic id in VC-GENESIS-1. They
 * do NOT check receipts: that is the admission door's job. */

#include "omega_vcstore.h"

/* Insert a Verified Crumb (admission kind VERIFIED). claimed_vc_id is required (NULL is
 * MALFORMED, never "skip the check"). The store is unchanged on every refusal. Refusals:
 *  - NULL argument, zero length: VCSTORE_MALFORMED
 *  - any SPEC 3.6 format refusal (includes MISSING_RECEIPT for a zero receipt_id and
 *    DEPENDENCY_CYCLE for a self dependency)
 *  - SHA-256(bytes) != claimed_vc_id: VCSTORE_ID_MISMATCH
 *  - semantic_id already present with different bytes or another admission kind:
 *    VCSTORE_IMMUTABLE_CONFLICT (identical bytes and kind: success, no change)
 *  - a dependency semantic_id not in the store, or whose contract_id differs from the entry's
 *    required_contract: UNVERIFIED_DEPENDENCY
 *  - out of memory: VCSTORE_NOMEM
 * There is no capacity limit: the store grows. */
int omega_vcstore_insert(OmegaVcStore *s, const uint8_t *canonical, size_t len,
                         const uint8_t claimed_vc_id[32]);

/* Genesis loading only. Same refusals, plus GENESIS_NOT_LISTED when the record's semantic id is
 * not a member of VC-GENESIS-1 (src/omega_genesis.h). Admission kind BOOTSTRAP. For a bootstrap
 * record the receipt_id field carries the hash of the audit record. */
int omega_vcstore_insert_bootstrap(OmegaVcStore *s, const uint8_t *canonical, size_t len,
                                   const uint8_t claimed_vc_id[32]);

#endif /* OMEGA_VCSTORE_PRIV_H */
