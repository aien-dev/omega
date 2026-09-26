#ifndef OMEGA_LIBRARY_H
#define OMEGA_LIBRARY_H

#include "omega_program.h"
#include "omega_synthesis.h"
#include <stdbool.h>
#include <stddef.h>

#define OMEGA_LIB_MAX_PROGRAMS 128
#define OMEGA_LIB_MAX_DEPS 8

typedef struct {
    OmegaProgram program;
    uint32_t version_introduced;
    uint64_t timestamp_added;
    size_t dep_count;
    SemanticId dependency_ids[OMEGA_LIB_MAX_DEPS];
    uint8_t evidence_receipt_hash[32];
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

/* Insert a verified program into the library.
 * Fails if:
 * - prog is NULL or not verified (prog->is_verified == false)
 * - duplicate SemanticId already exists
 * - capacity exceeded
 * - dependency cycle detected
 */
int omega_library_insert(OmegaLibrary *lib, const OmegaProgram *prog,
                         const SemanticId *deps, size_t dep_count,
                         const uint8_t receipt_hash[32]);

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
