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

static void lib_digest_u32(sha256_ctx *ctx, uint32_t v) {
    uint8_t b[4];
    b[0] = (uint8_t)((v >> 24) & 0xFF);
    b[1] = (uint8_t)((v >> 16) & 0xFF);
    b[2] = (uint8_t)((v >> 8) & 0xFF);
    b[3] = (uint8_t)(v & 0xFF);
    sha256_update(ctx, b, sizeof b);
}

/* Streaming digest over the whole library: no fixed buffer, so no entry can ever fall
 * outside the digest however many entries or dependencies the library holds. */
int omega_library_compute_digest(OmegaLibrary *lib) {
    if (!lib) return -1;

    sha256_ctx ctx;
    sha256_init(&ctx);

    /* Magic "LIB1" */
    sha256_update(&ctx, (const uint8_t *)"LIB1", 4);

    lib_digest_u32(&ctx, lib->version);
    lib_digest_u32(&ctx, (uint32_t)lib->count);

    for (size_t i = 0; i < lib->count; ++i) {
        const OmegaLibraryEntry *e = &lib->entries[i];

        sha256_update(&ctx, e->program.program_id.bytes, OMEGA_ID_BYTES);
        sha256_update(&ctx, e->program.realization.realization_id.bytes, OMEGA_ID_BYTES);
        lib_digest_u32(&ctx, e->version_introduced);
        {
            uint8_t kind = e->admission_kind;   /* VC1:digest-kind */
            sha256_update(&ctx, &kind, 1);
        }
        lib_digest_u32(&ctx, (uint32_t)e->dep_count);
        for (size_t d = 0; d < e->dep_count && d < OMEGA_LIB_MAX_DEPS; ++d) {
            sha256_update(&ctx, e->dependency_ids[d].bytes, OMEGA_ID_BYTES);
        }
        sha256_update(&ctx, e->evidence_receipt_hash, 32);
    }

    sha256_final(&ctx, lib->state_digest);
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

static bool lib_hash_is_zero(const uint8_t h[32]) {
    uint8_t acc = 0;
    for (size_t i = 0; i < 32; ++i) acc |= h[i];
    return acc == 0;
}

/* Shared admission path. Every refusal returns -1 before the library is touched.
 * Each guard sits on one tagged line: the VC1-LIB mutation test (Makefile test-library)
 * deletes or weakens exactly that line in a copy of this file and expects the unit test
 * to fail. Keep one guard per line when editing. */
static int lib_admit(OmegaLibrary *lib, const OmegaProgram *prog,
                     const SemanticId *deps, size_t dep_count,
                     const uint8_t hash[32], uint8_t kind) {
    if (!lib || !prog) return -1;

    /* Fail-closed verification gating: only verified programs admitted */
    if (!prog->is_verified || !prog->is_realized) return -1; /* VC1:verified-gate */

    /* Capacity check */
    if (lib->count >= OMEGA_LIB_MAX_PROGRAMS) return -1; /* VC1:capacity */

    /* No identity (no semantic body): refuse (spec/program-identity.md 2.3) */
    {
        static const uint8_t zero[OMEGA_ID_BYTES];
        if (memcmp(prog->program_id.bytes, zero, OMEGA_ID_BYTES) == 0) return -1; /* VC1:zero-id */
    }

    /* Duplicate check */
    if (omega_library_find_by_id(lib, &prog->program_id) != NULL) return -1; /* VC1:duplicate */

    /* Dependencies: never truncated, every one must already be in the library */
    if (dep_count > OMEGA_LIB_MAX_DEPS) return -1; /* VC1:dep-overflow */
    if (dep_count > 0 && !deps) return -1; /* VC1:dep-null */
    for (size_t i = 0; i < dep_count; ++i) {
        if (omega_library_find_by_id(lib, &deps[i]) == NULL) return -1; /* VC1:dep-unknown */
    }

    /* Cycle check (kept: defence in depth; unreachable while deps must pre-exist) */
    if (dep_count > 0) {
        if (omega_library_has_cycle(lib, &prog->program_id, deps, dep_count)) return -1; /* VC1:cycle */
    }

    OmegaLibraryEntry *e = &lib->entries[lib->count];
    memset(e, 0, sizeof(OmegaLibraryEntry));

    e->program = *prog;
    e->version_introduced = lib->version;
    e->timestamp_added = (uint64_t)time(NULL);
    e->admission_kind = kind;
    e->dep_count = dep_count;
    for (size_t i = 0; i < dep_count; ++i) {
        e->dependency_ids[i] = deps[i];
    }
    memcpy(e->evidence_receipt_hash, hash, 32);

    lib->count++;
    return omega_library_compute_digest(lib);
}

int omega_library_insert(OmegaLibrary *lib, const OmegaProgram *prog,
                         const SemanticId *deps, size_t dep_count,
                         const uint8_t receipt_hash[32]) {
    /* Evidence: a receipt hash is mandatory */
    if (!receipt_hash) return -1; /* VC1:null-receipt */
    if (lib_hash_is_zero(receipt_hash)) return -1; /* VC1:zero-receipt */
    return lib_admit(lib, prog, deps, dep_count, receipt_hash, OMEGA_LIB_ADMISSION_VERIFIED);
}

/* Genesis loading only. See the contract in omega_library.h. No flag unlocks this. */
int omega_library_insert_bootstrap(OmegaLibrary *lib, const OmegaProgram *prog,
                                   const SemanticId *deps, size_t dep_count,
                                   const uint8_t audit_hash[32]) {
    /* Evidence: the audit hash of the manual genesis audit is mandatory */
    if (!audit_hash) return -1; /* VC1:bootstrap-null-audit */
    if (lib_hash_is_zero(audit_hash)) return -1; /* VC1:bootstrap-zero-audit */
    return lib_admit(lib, prog, deps, dep_count, audit_hash, OMEGA_LIB_ADMISSION_BOOTSTRAP); /* VC1:kind-bootstrap */
}

int omega_library_export_primitives(const OmegaLibrary *lib, SynthPrimitiveBank *bank) {
    if (!lib || !bank) return -1;
    memset(bank, 0, sizeof(SynthPrimitiveBank));

    for (size_t i = 0; i < lib->count && bank->count < SYNTH_MAX_PRIMITIVES; ++i) {
        bank->programs[bank->count++] = lib->entries[i].program;
    }
    return 0;
}
