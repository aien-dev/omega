/*
 * aien_machine_id.h -- the one canonical AIEN machine identity (M20).
 *
 * A MachineId names one physical machine for its whole life (aienos ADR 0010:
 * created once at provisioning, resumed across boot, reboot, restart and model
 * reload; a part swap keeps it; a new box of the same brand gets a new one).
 * It is the identity carried by every 32-byte machine slot in the stack:
 *
 *   aienos   receipt `machine_id_digest` (ADR 0014 offset 352): the slot holds
 *            SHA-256("AIENOS-MACHINE-ID-V1\0" || provisioned MachineId bytes),
 *            which is exactly AienMachineId.id below. Zero there = absent.
 *   aienos   ARGUS ABI `machine_id[32]` (native/argus/argus_abi.h).
 *   physics  FORGE V2 `machine_identity` d32 (tags 0x0101, 0x0602): the stable
 *            name, distinct from the descriptor digest.
 *   omega    rx_argus event slot; the Capability Graph's machine column through
 *            AienMachineIndex (below).
 *
 * What it is NOT (demoted forms, kept for what they are):
 *   - OmegaMachineGraph.machine_id / omega_blackwell_get_machine_id: a digest
 *     of a hardware PROFILE (pipeline, caches, registers). Every DGX Spark has
 *     the same one. It names a machine model inside realization triple ids;
 *     it is never a machine identity.
 *   - CqEntry/CqCandidate/CqNeed `machine_id` (uint32_t, rx_capq.h): a local
 *     runtime INDEX, valid in one process only. Never persisted, never sent.
 *     Resolve it through AienMachineIndex.
 *   - rx_argus provisional id SHA-256("ARGUS-PROVISIONAL-MACHINE-v1"||run_id):
 *     a per-run handle. Used only while no canonical record is configured.
 *
 * Derivation. id = SHA-256("AIENOS-MACHINE-ID-V1\0" || root), where root is
 *   AIEN_MID_ROOT_PROVISIONED   the opaque bytes generated once at provisioning
 *   AIEN_MID_ROOT_HARDWARE      the 32-byte digest of the owner-enrolled key
 *                               (TRUST-1), when the machine has one
 * AIEN_MID_ROOT_IMPORTED marks an id taken from a bare 32-byte slot whose root
 * is not known here. The root kind is provenance only: equality and hashing use
 * the 32 id bytes, so the same machine compares equal whichever slot it came
 * from.
 *
 * Persisted / transported form (44 bytes, every field a byte string, so no
 * endianness):
 *   0  4  magic   "AMID"
 *   4  1  version 0x01
 *   5  1  root    AIEN_MID_ROOT_*
 *   6  2  reserved, zero
 *   8 32  id      non-zero
 *  40  4  check   first 4 bytes of SHA-256(bytes 0..39)
 * Text form: the 88 lowercase hex digits of those 44 bytes. A decoder rejects
 * any other length, magic, version, root, nonzero reserved, zero id or bad
 * check. Nothing transient (pid, run id, thread, pointer, index) is in it.
 *
 * Ownership of runtime state (one writer each; nothing here moves them):
 *   World state        rx_world.h. Shared logical state changes only by a
 *                      reaction's atomic publish at a World commit.
 *   Cortex             rx_cortex.h. Durable, append-only memory/evidence;
 *                      records name machines by AienMachineId, never by index.
 *   J-Space            rx_jspace.h. Runtime state-space material; placement
 *                      names a machine by AienMachineId; mutation of shared
 *                      logical state goes through the World commit.
 *   Capability Graph   rx_capq.h. Holds uint32 machine indexes from
 *                      AienMachineIndex; anything persisted or sent converts
 *                      the index back to AienMachineId first.
 *
 * Header-only codec (needs only sha256.c) so every producer links it as-is;
 * persistence and the index table live in aien_machine_id.c.
 */
#ifndef AIEN_MACHINE_ID_H
#define AIEN_MACHINE_ID_H

#include "sha256.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AIEN_MID_ID_BYTES      32u
#define AIEN_MID_RECORD_BYTES  44u
#define AIEN_MID_TEXT_CHARS    (2u * AIEN_MID_RECORD_BYTES)
#define AIEN_MID_VERSION       0x01u
#define AIEN_MID_ROOT_MAX      4096u   /* longest provisioned root accepted */

static const char AIEN_MID_DOMAIN[] = "AIENOS-MACHINE-ID-V1"; /* + its NUL: 21 bytes */
#define AIEN_MID_DOMAIN_BYTES  21u

enum {
    AIEN_MID_ROOT_PROVISIONED = 1,
    AIEN_MID_ROOT_HARDWARE    = 2,
    AIEN_MID_ROOT_IMPORTED    = 3
};

enum {
    AIEN_MID_OK = 0,
    AIEN_MID_E_ARG = -1,      /* null, bad length, unknown root */
    AIEN_MID_E_ZERO = -2,     /* all-zero id: "absent", never an identity */
    AIEN_MID_E_FORMAT = -3,   /* bad magic, version, reserved or hex */
    AIEN_MID_E_CHECK = -4,    /* check bytes do not match */
    AIEN_MID_E_IO = -5,
    AIEN_MID_E_FULL = -6      /* index table full */
};

typedef struct {
    uint8_t root;                     /* AIEN_MID_ROOT_*: provenance, not identity */
    uint8_t id[AIEN_MID_ID_BYTES];    /* the identity */
} AienMachineId;

static inline int aien_mid_is_zero_(const uint8_t *b) {
    uint8_t acc = 0;
    for (unsigned i = 0; i < AIEN_MID_ID_BYTES; i++) acc |= b[i];
    return acc == 0;
}

static inline int aien_mid_root_valid_(uint8_t r) {
    return r == AIEN_MID_ROOT_PROVISIONED || r == AIEN_MID_ROOT_HARDWARE ||
           r == AIEN_MID_ROOT_IMPORTED;
}

/* Derive from a root. HARDWARE takes exactly the 32-byte owner-key digest;
 * PROVISIONED takes 1..AIEN_MID_ROOT_MAX opaque bytes. */
static inline int aien_mid_derive(uint8_t root_kind, const uint8_t *root, size_t len,
                                  AienMachineId *out) {
    if (!root || !out) return AIEN_MID_E_ARG;
    if (root_kind == AIEN_MID_ROOT_HARDWARE) {
        if (len != AIEN_MID_ID_BYTES) return AIEN_MID_E_ARG;
    } else if (root_kind == AIEN_MID_ROOT_PROVISIONED) {
        if (len == 0 || len > AIEN_MID_ROOT_MAX) return AIEN_MID_E_ARG;
    } else {
        return AIEN_MID_E_ARG;
    }
    sha256_ctx h;
    sha256_init(&h);
    sha256_update(&h, (const uint8_t *)AIEN_MID_DOMAIN, AIEN_MID_DOMAIN_BYTES);
    sha256_update(&h, root, len);
    sha256_final(&h, out->id);
    out->root = root_kind;
    return aien_mid_is_zero_(out->id) ? AIEN_MID_E_ZERO : AIEN_MID_OK;
}

/* From a bare 32-byte slot (FORGE machine_identity, aienos receipt
 * machine_id_digest, ARGUS machine_id). All-zero means "absent": rejected. */
static inline int aien_mid_from_slot(const uint8_t slot[AIEN_MID_ID_BYTES], AienMachineId *out) {
    if (!slot || !out) return AIEN_MID_E_ARG;
    if (aien_mid_is_zero_(slot)) return AIEN_MID_E_ZERO;
    memcpy(out->id, slot, AIEN_MID_ID_BYTES);
    out->root = AIEN_MID_ROOT_IMPORTED;
    return AIEN_MID_OK;
}

static inline void aien_mid_to_slot(const AienMachineId *m, uint8_t slot[AIEN_MID_ID_BYTES]) {
    memcpy(slot, m->id, AIEN_MID_ID_BYTES);
}

/* Equality and hash: the 32 id bytes only. Hash is the first 8 id bytes read
 * big-endian: the same on every host, no seed. */
static inline int aien_mid_equal(const AienMachineId *a, const AienMachineId *b) {
    return memcmp(a->id, b->id, AIEN_MID_ID_BYTES) == 0;
}

static inline int aien_mid_compare(const AienMachineId *a, const AienMachineId *b) {
    return memcmp(a->id, b->id, AIEN_MID_ID_BYTES);
}

static inline uint64_t aien_mid_hash(const AienMachineId *m) {
    uint64_t h = 0;
    for (unsigned i = 0; i < 8; i++) h = (h << 8) | m->id[i];
    return h;
}

static inline void aien_mid_check_(const uint8_t rec[AIEN_MID_RECORD_BYTES], uint8_t out[4]) {
    uint8_t d[SHA256_DIGEST_SIZE];
    sha256_hash(rec, 40, d);
    memcpy(out, d, 4);
}

static inline int aien_mid_encode(const AienMachineId *m, uint8_t out[AIEN_MID_RECORD_BYTES]) {
    if (!m || !out || !aien_mid_root_valid_(m->root)) return AIEN_MID_E_ARG;
    if (aien_mid_is_zero_(m->id)) return AIEN_MID_E_ZERO;
    out[0] = 'A'; out[1] = 'M'; out[2] = 'I'; out[3] = 'D';
    out[4] = AIEN_MID_VERSION;
    out[5] = m->root;
    out[6] = 0; out[7] = 0;
    memcpy(out + 8, m->id, AIEN_MID_ID_BYTES);
    aien_mid_check_(out, out + 40);
    return AIEN_MID_OK;
}

static inline int aien_mid_decode(const uint8_t *in, size_t len, AienMachineId *out) {
    if (!in || !out) return AIEN_MID_E_ARG;
    if (len != AIEN_MID_RECORD_BYTES) return AIEN_MID_E_FORMAT;
    if (in[0] != 'A' || in[1] != 'M' || in[2] != 'I' || in[3] != 'D' ||
        in[4] != AIEN_MID_VERSION || in[6] != 0 || in[7] != 0)
        return AIEN_MID_E_FORMAT;
    if (!aien_mid_root_valid_(in[5])) return AIEN_MID_E_FORMAT;
    uint8_t chk[4];
    aien_mid_check_(in, chk);
    if (memcmp(chk, in + 40, 4) != 0) return AIEN_MID_E_CHECK;
    if (aien_mid_is_zero_(in + 8)) return AIEN_MID_E_ZERO;
    out->root = in[5];
    memcpy(out->id, in + 8, AIEN_MID_ID_BYTES);
    return AIEN_MID_OK;
}

/* Text form: out must hold AIEN_MID_TEXT_CHARS + 1 bytes (NUL-terminated). */
static inline int aien_mid_to_text(const AienMachineId *m, char out[AIEN_MID_TEXT_CHARS + 1]) {
    static const char hx[] = "0123456789abcdef";
    uint8_t rec[AIEN_MID_RECORD_BYTES];
    int rc = aien_mid_encode(m, rec);
    if (rc != AIEN_MID_OK) return rc;
    for (unsigned i = 0; i < AIEN_MID_RECORD_BYTES; i++) {
        out[2 * i] = hx[rec[i] >> 4];
        out[2 * i + 1] = hx[rec[i] & 15];
    }
    out[AIEN_MID_TEXT_CHARS] = 0;
    return AIEN_MID_OK;
}

static inline int aien_mid_hexval_(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;   /* uppercase is not canonical */
}

static inline int aien_mid_from_text(const char *s, AienMachineId *out) {
    if (!s || !out) return AIEN_MID_E_ARG;
    if (strlen(s) != AIEN_MID_TEXT_CHARS) return AIEN_MID_E_FORMAT;
    uint8_t rec[AIEN_MID_RECORD_BYTES];
    for (unsigned i = 0; i < AIEN_MID_RECORD_BYTES; i++) {
        int hi = aien_mid_hexval_(s[2 * i]), lo = aien_mid_hexval_(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return AIEN_MID_E_FORMAT;
        rec[i] = (uint8_t)(hi << 4 | lo);
    }
    return aien_mid_decode(rec, sizeof rec, out);
}

/* ---- aien_machine_id.c -------------------------------------------------- */

/* Persist atomically (temp file, fsync, rename, fsync directory). */
int aien_mid_store(const char *path, const AienMachineId *m);
/* Load; any file that is not exactly one valid record is rejected. */
int aien_mid_load(const char *path, AienMachineId *out);

/* Local runtime index <-> canonical identity. Index 0 means "none" (as in
 * rx_capq); indexes are dense from 1 in bind order and live only in this
 * process. The table is caller-owned and bounded. */
typedef struct {
    uint32_t capacity, count;
    AienMachineId *slots;              /* slots[i] is index i + 1 */
} AienMachineIndex;

void     aien_mid_index_init(AienMachineIndex *t, AienMachineId *slots, uint32_t capacity);
/* Index for m, binding a new one if m is not known yet. 0 on full or bad id. */
uint32_t aien_mid_index_bind(AienMachineIndex *t, const AienMachineId *m);
/* Index for m if bound, else 0. */
uint32_t aien_mid_index_find(const AienMachineIndex *t, const AienMachineId *m);
/* Identity behind an index; AIEN_MID_E_ARG for 0 or an unbound index. */
int      aien_mid_index_get(const AienMachineIndex *t, uint32_t index, AienMachineId *out);

#endif
