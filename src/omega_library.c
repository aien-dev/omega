#include "omega_library.h"
#include "sha256.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int omega_library_init(OmegaLibrary *lib) {
    if (!lib) return -1;
    memset(lib, 0, sizeof(OmegaLibrary));
    lib->version = 1;
    return omega_library_compute_digest(lib);
}

void omega_library_destroy(OmegaLibrary *lib) {
    if (!lib) return;
    for (size_t i = 0; i < lib->count; ++i) {
        omega_program_destroy(&lib->entries[i].program);
    }
    memset(lib, 0, sizeof(OmegaLibrary));
}

int omega_library_compute_digest(OmegaLibrary *lib) {
    if (!lib) return -1;

    uint8_t buffer[65536];
    size_t pos = 0;

    /* Magic "LIB1" */
    buffer[pos++] = 'L'; buffer[pos++] = 'I'; buffer[pos++] = 'B'; buffer[pos++] = '1';

    /* Version (4 bytes BE) */
    buffer[pos++] = (uint8_t)((lib->version >> 24) & 0xFF);
    buffer[pos++] = (uint8_t)((lib->version >> 16) & 0xFF);
    buffer[pos++] = (uint8_t)((lib->version >> 8) & 0xFF);
    buffer[pos++] = (uint8_t)(lib->version & 0xFF);

    /* Count (4 bytes BE) */
    uint32_t cnt = (uint32_t)lib->count;
    buffer[pos++] = (uint8_t)((cnt >> 24) & 0xFF);
    buffer[pos++] = (uint8_t)((cnt >> 16) & 0xFF);
    buffer[pos++] = (uint8_t)((cnt >> 8) & 0xFF);
    buffer[pos++] = (uint8_t)(cnt & 0xFF);

    for (size_t i = 0; i < lib->count; ++i) {
        const OmegaLibraryEntry *e = &lib->entries[i];

        /* Program ID */
        memcpy(&buffer[pos], e->program.program_id.bytes, OMEGA_ID_BYTES);
        pos += OMEGA_ID_BYTES;

        /* Realization ID */
        memcpy(&buffer[pos], e->program.realization.realization_id.bytes, OMEGA_ID_BYTES);
        pos += OMEGA_ID_BYTES;

        /* Version introduced */
        buffer[pos++] = (uint8_t)((e->version_introduced >> 24) & 0xFF);
        buffer[pos++] = (uint8_t)((e->version_introduced >> 16) & 0xFF);
        buffer[pos++] = (uint8_t)((e->version_introduced >> 8) & 0xFF);
        buffer[pos++] = (uint8_t)(e->version_introduced & 0xFF);

        /* Dependency count */
        uint32_t dc = (uint32_t)e->dep_count;
        buffer[pos++] = (uint8_t)((dc >> 24) & 0xFF);
        buffer[pos++] = (uint8_t)((dc >> 16) & 0xFF);
        buffer[pos++] = (uint8_t)((dc >> 8) & 0xFF);
        buffer[pos++] = (uint8_t)(dc & 0xFF);

        /* Dependency IDs */
        for (size_t d = 0; d < e->dep_count && d < OMEGA_LIB_MAX_DEPS; ++d) {
            memcpy(&buffer[pos], e->dependency_ids[d].bytes, OMEGA_ID_BYTES);
            pos += OMEGA_ID_BYTES;
        }

        /* Evidence receipt hash */
        memcpy(&buffer[pos], e->evidence_receipt_hash, 32);
        pos += 32;

        if (pos + 256 > sizeof(buffer)) break;
    }

    sha256_hash(buffer, pos, lib->state_digest);
    return 0;
}

int omega_library_advance_version(OmegaLibrary *lib) {
    if (!lib) return -1;
    lib->version++;
    return omega_library_compute_digest(lib);
}

const OmegaLibraryEntry* omega_library_find_by_id(const OmegaLibrary *lib, const SemanticId *prog_id) {
    if (!lib || !prog_id) return NULL;
    for (size_t i = 0; i < lib->count; ++i) {
        if (memcmp(lib->entries[i].program.program_id.bytes, prog_id->bytes, OMEGA_ID_BYTES) == 0) {
            return &lib->entries[i];
        }
    }
    return NULL;
}

const OmegaLibraryEntry* omega_library_find_by_realization_id(const OmegaLibrary *lib, const SemanticId *real_id) {
    if (!lib || !real_id) return NULL;
    for (size_t i = 0; i < lib->count; ++i) {
        if (memcmp(lib->entries[i].program.realization.realization_id.bytes, real_id->bytes, OMEGA_ID_BYTES) == 0) {
            return &lib->entries[i];
        }
    }
    return NULL;
}

const OmegaLibraryEntry* omega_library_find_by_name(const OmegaLibrary *lib, const char *name) {
    if (!lib || !name) return NULL;
    for (size_t i = 0; i < lib->count; ++i) {
        if (strcmp(lib->entries[i].program.name, name) == 0) {
            return &lib->entries[i];
        }
    }
    return NULL;
}

size_t omega_library_query_by_type(const OmegaLibrary *lib,
                                   TypeTag in_type, uint16_t in_width,
                                   TypeTag out_type, uint16_t out_width,
                                   const OmegaLibraryEntry **out_results, size_t max_results) {
    if (!lib || !out_results || max_results == 0) return 0;
    size_t found = 0;

    for (size_t i = 0; i < lib->count && found < max_results; ++i) {
        const OmegaProgram *p = &lib->entries[i].program;
        if (p->contract.input_type == in_type &&
            p->contract.input_width == in_width &&
            p->contract.output_type == out_type &&
            p->contract.output_width == out_width) {
            out_results[found++] = &lib->entries[i];
        }
    }
    return found;
}

bool omega_library_has_cycle(const OmegaLibrary *lib, const SemanticId *target_id,
                             const SemanticId *new_deps, size_t new_dep_count) {
    if (!lib || !target_id || !new_deps || new_dep_count == 0) return false;

    /* Check direct containment */
    for (size_t i = 0; i < new_dep_count; ++i) {
        if (memcmp(target_id->bytes, new_deps[i].bytes, OMEGA_ID_BYTES) == 0) {
            return true;
        }
        /* Recursive transitive lookup through library entries */
        const OmegaLibraryEntry *dep_entry = omega_library_find_by_id(lib, &new_deps[i]);
        if (dep_entry && dep_entry->dep_count > 0) {
            if (omega_library_has_cycle(lib, target_id, dep_entry->dependency_ids, dep_entry->dep_count)) {
                return true;
            }
        }
    }
    return false;
}

int omega_library_insert(OmegaLibrary *lib, const OmegaProgram *prog,
                         const SemanticId *deps, size_t dep_count,
                         const uint8_t receipt_hash[32]) {
    if (!lib || !prog) return -1;

    /* Fail-closed verification gating: only verified programs admitted */
    if (!prog->is_verified || !prog->is_realized) {
        return -1;
    }

    /* Capacity check */
    if (lib->count >= OMEGA_LIB_MAX_PROGRAMS) {
        return -1;
    }

    /* Duplicate check */
    if (omega_library_find_by_id(lib, &prog->program_id) != NULL) {
        return -1;
    }

    /* Cycle check */
    if (deps && dep_count > 0) {
        if (omega_library_has_cycle(lib, &prog->program_id, deps, dep_count)) {
            return -1;
        }
    }

    OmegaLibraryEntry *e = &lib->entries[lib->count];
    memset(e, 0, sizeof(OmegaLibraryEntry));

    e->program = *prog;
    e->version_introduced = lib->version;
    e->timestamp_added = (uint64_t)time(NULL);

    if (deps && dep_count > 0) {
        size_t to_copy = dep_count < OMEGA_LIB_MAX_DEPS ? dep_count : OMEGA_LIB_MAX_DEPS;
        e->dep_count = to_copy;
        for (size_t i = 0; i < to_copy; ++i) {
            e->dependency_ids[i] = deps[i];
        }
    }

    if (receipt_hash) {
        memcpy(e->evidence_receipt_hash, receipt_hash, 32);
    }

    lib->count++;
    return omega_library_compute_digest(lib);
}

int omega_library_export_primitives(const OmegaLibrary *lib, SynthPrimitiveBank *bank) {
    if (!lib || !bank) return -1;
    memset(bank, 0, sizeof(SynthPrimitiveBank));

    for (size_t i = 0; i < lib->count && bank->count < SYNTH_MAX_PRIMITIVES; ++i) {
        bank->programs[bank->count++] = lib->entries[i].program;
    }
    return 0;
}
