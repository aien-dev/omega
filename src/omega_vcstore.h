#ifndef OMEGA_VCSTORE_H
#define OMEGA_VCSTORE_H

/* omega_vcstore: the Verified Crumb Store (VC1 stage 3, ADR 0029).
 *
 * A content-addressed, immutable store of VerifiedCrumbV1 records (aien-protocols
 * specs/verified-crumb/SPEC.md), keyed by the 32-byte semantic_id (the Omega program id).
 * The canonical encoding, the digest_kind byte and the VC id derivation are those of SPEC.md,
 * byte for byte (VC id = SHA-256 over the canonical bytes, domain tag included).
 *
 * WHAT THE STORE DOES
 *  - insert: decode (format refusals of SPEC 3.6), recompute the VC id and compare it with the
 *    caller's claimed id, refuse a different record for an existing semantic_id (immutability),
 *    accept identical bytes as a no-op, and require every dependency to be in the store already
 *    with a matching contract_id (fail closed, cycles impossible by construction and refused).
 *  - get / closure: recompute the VC id of the stored bytes on every access and refuse on mismatch.
 *  - digest: streaming SHA-256 over every object (semantic_id order, admission kind, canonical
 *    bytes). Independent of insert order. Names are never part of it.
 *  - save / load: one file, explicit big-endian, digests at the end, load recomputes everything.
 *
 * WHAT THE STORE DOES NOT DO
 *  - It does NOT verify receipts. It holds each record's receipt_id and exposes it through
 *    omega_vcstore_receipt_of(); checking the receipt (SPEC 5.1) and the verifier profile is the
 *    evidence verifier's job (VC1 stage 4/5). A record in the store is "well-formed, immutable,
 *    closed under dependencies", not "proven". The unused ADR 0029 Decision 9 codes below are
 *    reserved for that verifier and the resolver.
 *  - It does not read lockfiles and does not decide trust from a name.
 *
 * ADMISSION KIND: VERIFIED is the normal path. BOOTSTRAP is for the manually audited Genesis Set
 * VC-GENESIS-1 only (src/omega_genesis.h, docs/osc/VC-GENESIS-1.md). The two raw insert functions
 * are private (omega_vcstore_priv.h) and reachable only through omega_resolve_admit and
 * omega_resolve_admit_genesis. There is no flag, environment variable or build option that unlocks
 * BOOTSTRAP or skips any refusal: a BOOTSTRAP record whose semantic id is not on the list is
 * refused when it is inserted and when a store file holding it is loaded (GENESIS_NOT_LISTED).
 * For a bootstrap record the receipt_id field carries the hash of the audit record. The kind is
 * part of the state digest.
 *
 * NAME INDEX: a separate map name -> semantic_id. Many names may point at one id. A bound name
 * changes only through the explicit omega_vcstore_name_rebind(). Nothing in get, closure,
 * insert, receipt_of or the state digest reads it; omega_vcstore_resolve_name() is the only
 * reader and returns the id and nothing else. It has its own digest. Names are convenience,
 * digests are authority.
 */

#include <stddef.h>
#include <stdint.h>

#define OMEGA_VC_ID_BYTES 32
#define OMEGA_VC_FORMAT_VERSION 1u
#define OMEGA_VC_MAX_ENTRIES 256     /* per list, SPEC 3.3; over the limit is refused, never truncated */
#define OMEGA_VC_MAX_STRING 256
#define OMEGA_VC_DIGEST_SOURCE 0x01  /* canonical source */
#define OMEGA_VC_DIGEST_IR     0x02  /* canonical IR */

#define OMEGA_VCS_ADMISSION_VERIFIED  1u
#define OMEGA_VCS_ADMISSION_BOOTSTRAP 2u   /* genesis set only */

/* Result codes. 0 is success. Names are what matters (omega_vcstore_code_name). */
enum {
    OMEGA_VCS_OK = 0,
    /* ARCH-0029 Decision 9, in the ADR's order */
    OMEGA_VCS_UNVERIFIED_DEPENDENCY = 1,   /* absent id, absent dependency, contract mismatch on an edge */
    OMEGA_VCS_MISSING_RECEIPT,             /* all-zero receipt_id */
    OMEGA_VCS_RECEIPT_HASH_MISMATCH,       /* reserved: evidence verifier */
    OMEGA_VCS_DEPENDENCY_NOT_PINNED,       /* reserved: resolver / lockfile */
    OMEGA_VCS_DEPENDENCY_CYCLE,            /* self dependency, or a cycle in a loaded or tampered graph */
    OMEGA_VCS_STALE_RECEIPT,               /* reserved: evidence verifier */
    OMEGA_VCS_UNDECLARED_IMPORT,           /* reserved: compiler */
    OMEGA_VCS_TAINTED_ARTIFACT,            /* reserved: resolver */
    OMEGA_VCS_UNKNOWN_VERIFIER_PROFILE,    /* reserved: evidence verifier */
    /* SPEC 3.6 format refusals (same numbers as vc1-check) */
    OMEGA_VCS_BAD_DOMAIN_TAG = 20,
    OMEGA_VCS_UNKNOWN_FORMAT_VERSION,
    OMEGA_VCS_TRUNCATED,
    OMEGA_VCS_TRAILING_BYTES,
    OMEGA_VCS_DUPLICATE_DEPENDENCY,
    OMEGA_VCS_UNSORTED_DEPENDENCIES,
    OMEGA_VCS_NONCANONICAL_SET,
    OMEGA_VCS_BAD_STRING,
    OMEGA_VCS_ZERO_ID,
    OMEGA_VCS_TOO_MANY_ENTRIES,
    OMEGA_VCS_BAD_DIGEST_KIND,
    /* store-local */
    OMEGA_VCS_VCSTORE_ID_MISMATCH = 40,    /* recomputed VC id or loaded digest differs */
    OMEGA_VCS_VCSTORE_IMMUTABLE_CONFLICT,  /* same semantic_id, different bytes or admission kind */
    OMEGA_VCS_VCSTORE_MALFORMED,           /* bad argument, bad name, bad store file */
    OMEGA_VCS_VCSTORE_NOT_FOUND,           /* name not bound */
    OMEGA_VCS_VCSTORE_NAME_EXISTS,         /* name already bound to another id; use name_rebind */
    OMEGA_VCS_VCSTORE_CAPACITY,            /* closure output array too small (needed count returned) */
    OMEGA_VCS_VCSTORE_NOMEM,
    OMEGA_VCS_VCSTORE_IO,
    OMEGA_VCS_GENESIS_NOT_LISTED           /* BOOTSTRAP record whose semantic id is not in VC-GENESIS-1 */
};

const char *omega_vcstore_code_name(int code);

/* ---- decoded view (pointers into the canonical bytes; no copies) ---- */
typedef struct { const uint8_t *p; size_t len; } OmegaVcStr;

typedef struct {
    uint32_t format_version;
    const uint8_t *semantic_id;           /* 32 */
    const uint8_t *contract_id;           /* 32 */
    uint8_t digest_kind;
    const uint8_t *source_or_ir_digest;   /* 32 */
    uint32_t n_realizations;
    const uint8_t *realization_ids;       /* n x 32 */
    uint32_t n_dependencies;
    const uint8_t *dependencies;          /* n x 64: semantic_id (32) then required_contract (32) */
    const uint8_t *receipt_id;            /* 32 */
    OmegaVcStr verifier_profile, verifier_version;
    const uint8_t *evidence_root;         /* 32 */
    uint32_t n_exports;
    OmegaVcStr exports[OMEGA_VC_MAX_ENTRIES];
    uint32_t n_capabilities;
    OmegaVcStr capabilities[OMEGA_VC_MAX_ENTRIES];
} OmegaVcView;

/* Format-only check of canonical bytes (SPEC 3, refusal order SPEC 3.5). Returns 0 or the first
 * format refusal code. Never sorts: unsorted input is refused. On success fills *view. */
int omega_vc_decode(const uint8_t *bytes, size_t len, OmegaVcView *view);

/* VC id = SHA-256(canonical bytes), tag included (SPEC 3.4). Does not validate. */
void omega_vc_compute_id(const uint8_t *bytes, size_t len, uint8_t out[OMEGA_VC_ID_BYTES]);

/* ---- store ----
 * The fields are public so tests can inspect and corrupt them; production code must treat them
 * as read-only and use the functions. objs is sorted ascending by semantic_id (memcmp). */
typedef struct {
    uint8_t semantic_id[32];
    uint8_t vc_id[32];
    uint8_t admission_kind;     /* OMEGA_VCS_ADMISSION_* */
    uint8_t *bytes;             /* owned canonical bytes, stable until destroy */
    size_t len;
} OmegaVcObject;

typedef struct {
    char *name;                 /* owned, NUL-terminated */
    uint8_t id[32];
} OmegaVcName;

typedef struct {
    OmegaVcObject *objs;
    size_t count, cap;
    OmegaVcName *names;         /* sorted ascending by strcmp */
    size_t n_names, cap_names;
} OmegaVcStore;

typedef struct {
    const uint8_t *canonical;   /* points into the store, valid until destroy */
    size_t canonical_len;
    uint8_t vc_id[32];
    uint8_t admission_kind;
    OmegaVcView vc;
} OmegaVcRecord;

int  omega_vcstore_init(OmegaVcStore *s);
void omega_vcstore_destroy(OmegaVcStore *s);
size_t omega_vcstore_count(const OmegaVcStore *s);

/* There is no public insert. Records enter only through omega_resolve_admit and
 * omega_resolve_admit_genesis (src/omega_resolve.h); the raw functions are declared in the private
 * header omega_vcstore_priv.h (VC1 stage 6). */

/* Fetch by semantic_id. Recomputes the VC id of the stored bytes and re-decodes them; any
 * mismatch is VCSTORE_ID_MISMATCH. Absent: UNVERIFIED_DEPENDENCY. */
int omega_vcstore_get(const OmegaVcStore *s, const uint8_t semantic_id[32], OmegaVcRecord *out);

/* The receipt_id the record carries. NOT verified here (see header). */
int omega_vcstore_receipt_of(const OmegaVcStore *s, const uint8_t semantic_id[32], uint8_t out_receipt[32]);

int omega_vcstore_admission_kind(const OmegaVcStore *s, const uint8_t semantic_id[32], uint8_t *out_kind);

/* Transitive dependency closure of id, root included, in deterministic post-order: every
 * dependency appears before whoever needs it; dependencies are visited in ascending semantic_id
 * order; each id appears once. out_ids receives n x 32 bytes; *out_n the count. Each record is
 * fetched with the get checks, each edge's contract is checked. Errors: UNVERIFIED_DEPENDENCY
 * (absent id/dependency, contract mismatch), VCSTORE_ID_MISMATCH, DEPENDENCY_CYCLE. If cap is
 * smaller than the closure: VCSTORE_CAPACITY with *out_n = the needed count (cap 0 and
 * out_ids NULL may be used to ask for the size). On any other error *out_n = 0. Never a
 * partial success. Does not read the name index. */
int omega_vcstore_closure(const OmegaVcStore *s, const uint8_t id[32],
                          uint8_t *out_ids, size_t cap, size_t *out_n);

/* State digest over the objects only (never the names). Streaming; no size limit. */
int omega_vcstore_digest(const OmegaVcStore *s, uint8_t out[32]);

/* ---- name index ---- names match [A-Za-z_][A-Za-z0-9_.-]{0,127} (the omega.lock grammar) */
int omega_vcstore_name_bind(OmegaVcStore *s, const char *name, const uint8_t id[32]);
int omega_vcstore_name_rebind(OmegaVcStore *s, const char *name, const uint8_t id[32]);
int omega_vcstore_resolve_name(const OmegaVcStore *s, const char *name, uint8_t out_id[32]);
int omega_vcstore_name_index_digest(const OmegaVcStore *s, uint8_t out[32]);

/* ---- persistence ----
 * File layout (all integers big-endian, no host structs):
 *   "AIEN_VCSTORE_V1" (15 bytes)
 *   u64 object count, then per object: u8 admission_kind, u64 length, canonical bytes
 *     (objects strictly ascending by semantic_id)
 *   u64 name count, then per name: u64 length, name bytes, 32-byte id (names strictly ascending)
 *   32 bytes object state digest, 32 bytes name-index digest
 *   (end of file: no trailing byte)
 * load builds a new store, re-decodes and re-hashes every object, re-checks dependencies and
 * acyclicity, recomputes both digests and refuses on any mismatch. The digests are integrity
 * checks, not signatures: anyone can recompute them. On failure *s is unchanged; on success its old contents are replaced (s must be initialised). */
int omega_vcstore_save(const OmegaVcStore *s, const char *path);
int omega_vcstore_load(OmegaVcStore *s, const char *path);

#endif /* OMEGA_VCSTORE_H */
