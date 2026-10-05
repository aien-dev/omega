#ifndef OMEGA_LIBRARY_H
#define OMEGA_LIBRARY_H

#include "omega_program.h"
#include "omega_synthesis.h"
#include <stdbool.h>
#include <stddef.h>

#define OMEGA_LIB_MAX_PROGRAMS 128
#define OMEGA_LIB_MAX_DEPS 32

/* How an entry was admitted. Part of the state digest, so a bootstrap entry and a verified
 * entry with identical bytes never share a digest. 0 is never a valid kind. */
#define OMEGA_LIB_ADMISSION_VERIFIED  1u   /* omega_library_insert: verified + receipt */
#define OMEGA_LIB_ADMISSION_BOOTSTRAP 2u   /* omega_library_insert_bootstrap: genesis set only */

typedef struct {
    OmegaProgram program;
    uint32_t version_introduced;
    uint64_t timestamp_added;
    size_t dep_count;
    SemanticId dependency_ids[OMEGA_LIB_MAX_DEPS];
    uint8_t evidence_receipt_hash[32];  /* verified: receipt hash; bootstrap: audit hash */
    uint8_t admission_kind;             /* OMEGA_LIB_ADMISSION_* */
} OmegaLibraryEntry;

typedef struct {
    uint32_t version;
    size_t count;
    OmegaLibraryEntry entries[OMEGA_LIB_MAX_PROGRAMS];
    uint8_t state_digest[32];
} OmegaLibrary;

/* Initialize empty library at version 1 */
int omega_library_init(OmegaLibrary *lib);

/* Free all resources owned by the library */
void omega_library_destroy(OmegaLibrary *lib);

/* Insert a verified program into the library (admission kind VERIFIED).
 * Returns -1 and leaves the library unchanged if:
 * - lib or prog is NULL, or prog is not verified and realized
 * - receipt_hash is NULL or all zero (a verified admission needs evidence)
 * - prog has no identity (all-zero program_id)
 * - duplicate SemanticId already exists
 * - capacity exceeded
 * - dep_count > OMEGA_LIB_MAX_DEPS (never truncated), or deps is NULL with dep_count > 0
 * - any dependency id is not already in the library
 * - dependency cycle detected
 */
int omega_library_insert(OmegaLibrary *lib, const OmegaProgram *prog,
                         const SemanticId *deps, size_t dep_count,
                         const uint8_t receipt_hash[32]);

/* Insert one program of the manually audited GENESIS set (admission kind BOOTSTRAP).
 * Same refusals as omega_library_insert, with audit_hash (NULL or all zero refused) in
 * place of the receipt hash.
 *
 * MUST NEVER be called outside genesis loading. There is no runtime flag, environment
 * variable or build option that unlocks it: reviewers must reject any caller that is not
 * the genesis loader (VC1 stage 6). The admission kind is recorded in the entry and in the
 * state digest, so bootstrap entries are always distinguishable from verified ones. */
int omega_library_insert_bootstrap(OmegaLibrary *lib, const OmegaProgram *prog,
                                   const SemanticId *deps, size_t dep_count,
                                   const uint8_t audit_hash[32]);

/* Content-addressed lookup by program SemanticId */
const OmegaLibraryEntry* omega_library_find_by_id(const OmegaLibrary *lib, const SemanticId *prog_id);

/* Content-addressed lookup by realization SemanticId */
const OmegaLibraryEntry* omega_library_find_by_realization_id(const OmegaLibrary *lib, const SemanticId *real_id);

/* Lookup by human-readable name */
const OmegaLibraryEntry* omega_library_find_by_name(const OmegaLibrary *lib, const char *name);

/* Semantic query: find all programs matching input/output types and widths */
size_t omega_library_query_by_type(const OmegaLibrary *lib,
                                   TypeTag in_type, uint16_t in_width,
                                   TypeTag out_type, uint16_t out_width,
                                   const OmegaLibraryEntry **out_results, size_t max_results);

/* Compute cryptographic checksum over entire library state */
int omega_library_compute_digest(OmegaLibrary *lib);

/* Advance version and seal state digest */
int omega_library_advance_version(OmegaLibrary *lib);

/* Cycle detection across library dependencies */
bool omega_library_has_cycle(const OmegaLibrary *lib, const SemanticId *target_id,
                             const SemanticId *new_deps, size_t new_dep_count);

/* Export library programs into a synthesis primitive bank for abstraction reuse */
int omega_library_export_primitives(const OmegaLibrary *lib, SynthPrimitiveBank *bank);

#endif /* OMEGA_LIBRARY_H */
