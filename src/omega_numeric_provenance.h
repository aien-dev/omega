#ifndef OMEGA_NUMERIC_PROVENANCE_H
#define OMEGA_NUMERIC_PROVENANCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * One entry per distinct instruction the GB10 numeric executor submits.
 * fixture_w0..w2 are the exact instruction words (register fields included);
 * the control word w3 is checked separately against the scoreboard rules.
 * An entry records what the development oracle decoded; it is not a GB10
 * qualification. GB10 parity lives only in the OMEGA-NUMERIC-0 receipt.
 */
typedef struct {
    const char *key;              /* matches OmegaNumericPatchInsn.provenance_key */
    const char *mnemonic;         /* nvdisasm text for these exact words          */
    uint32_t    opcode;           /* fixture_w0 & 0xffff                          */
    const char *description;
    const char *evidence_source;
    uint32_t    fixture_w0;
    uint32_t    fixture_w1;
    uint32_t    fixture_w2;
    bool        variable_latency; /* result arrives through a scoreboard          */
} OmegaOpcodeProvenance;

size_t omega_numeric_get_opcode_count(void);
const OmegaOpcodeProvenance *omega_numeric_get_opcode(size_t index);

/*
 * Cross-checks a provenance table against the executor:
 *  - every entry is complete, its opcode equals w0 & 0xffff, keys are unique;
 *  - every instruction every encoded op submits has an entry with identical
 *    w0/w1/w2, and every entry is used by some op (no orphan claims);
 *  - STG/EXIT words equal the vecadd baseline at 0x120/0x130 (control aside);
 *  - scoreboards: the first patched instruction waits on the load barrier
 *    (SB4); a variable-latency instruction sets a write barrier and the next
 *    STG waits on it.
 * Returns 0 when consistent; otherwise the number of problems (each printed
 * to stderr when verbose).
 */
int omega_numeric_verify_fixture_table(const OmegaOpcodeProvenance *table, size_t n, bool verbose);

/* The built-in table through omega_numeric_verify_fixture_table. */
int omega_numeric_verify_all_fixtures(void);

#endif /* OMEGA_NUMERIC_PROVENANCE_H */
