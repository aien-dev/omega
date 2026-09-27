#ifndef OMEGA_NUMERIC_PROVENANCE_H
#define OMEGA_NUMERIC_PROVENANCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *mnemonic;
    uint32_t    opcode;
    const char *description;
    const char *evidence_source;
    uint32_t    fixture_w0;
    uint32_t    fixture_w1;
    uint32_t    fixture_w2;
    uint32_t    fixture_w3;
    bool        qualified_gb10;
} OmegaOpcodeProvenance;

/* Returns count of newly admitted FP32 SIMT opcodes */
size_t omega_numeric_get_opcode_count(void);

/* Retrieves opcode provenance entry by index */
const OmegaOpcodeProvenance *omega_numeric_get_opcode(size_t index);

/* Verifies all newly admitted opcodes against frozen GB10 development oracle fixtures */
int omega_numeric_verify_all_fixtures(void);

#endif /* OMEGA_NUMERIC_PROVENANCE_H */
