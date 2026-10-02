#ifndef OMEGA_GENESIS_H
#define OMEGA_GENESIS_H

/* VC-GENESIS-1: the Genesis Set of ADR 0029 Decision 8 (VC1 stage 6).
 *
 * The Genesis Set is the enumerated list of semantic ids (Omega program ids) that may exist in
 * the Verified Crumb Store as BOOTSTRAP records, that is, without a receipt. It is the only start
 * of trust that does not come from a receipt, so it is part of the trusted computing base and is
 * kept as small as the audit allows. The audit record, the members and the reason for each are
 * in docs/osc/VC-GENESIS-1.md.
 *
 * HOW IT IS USED (and the only ways):
 *  - omega_vcstore.c refuses to insert or to load a BOOTSTRAP record whose semantic id is not
 *    listed here (OMEGA_VCS_GENESIS_NOT_LISTED);
 *  - omega_resolve.c refuses omega_resolve_admit_genesis for an unlisted id and refuses to let an
 *    unlisted BOOTSTRAP record satisfy an import. The resolver has no genesis switch of any kind: whether
 *    a BOOTSTRAP record counts is decided by membership in this table and by nothing else.
 *
 * NO WAY AROUND IT: the table is a compile-time constant. There is no flag, no environment
 * variable, no preprocessor switch, no file and no store content that adds a member. Changing
 * the set means editing this file, and every edit is an ADR-level change reviewed as one (the
 * source test tests/test_omega_genesis.c pins the count and the table below, and the audit record).
 *
 * No wildcard and no prefix match: an id is a member only if all 32 bytes equal a row. The
 * all-zero id is never a member, whatever the table holds. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define OMEGA_GENESIS_SET_NAME "VC-GENESIS-1"

/* Number of members. The audit (docs/osc/VC-GENESIS-1.md, section "Audit") found that neither
 * bootstrap caller of the library (src/crumbline/cl_program.c and src/omega_discovery.c) needs a
 * record without a receipt, so the set is empty. */
#define OMEGA_GENESIS_1_COUNT 0u

/* One row per member, strictly ascending by memcmp. The row after the last member is an all-zero
 * terminator that is never a member (so the array is never zero-sized). */
static const uint8_t OMEGA_GENESIS_1[OMEGA_GENESIS_1_COUNT + 1u][32] = {
    { 0 }
};

static inline size_t omega_genesis_count(void)
{
    return OMEGA_GENESIS_1_COUNT;
}

/* The i-th member (0 <= i < count), or NULL. */
static inline const uint8_t *omega_genesis_member(size_t i)
{
    const size_t n = OMEGA_GENESIS_1_COUNT;
    return i < n ? OMEGA_GENESIS_1[i] : NULL;
}

/* 1 when id (32 bytes) is a member of VC-GENESIS-1, else 0. NULL and the zero id are not members. */
static inline int omega_genesis_contains(const uint8_t *id)
{
    static const uint8_t zero[32] = { 0 };
    const size_t n = OMEGA_GENESIS_1_COUNT;
    if (!id || memcmp(id, zero, 32) == 0) return 0;
    for (size_t i = 0; i < n; i++)
        if (memcmp(OMEGA_GENESIS_1[i], id, 32) == 0) return 1;
    return 0;
}

#endif /* OMEGA_GENESIS_H */
