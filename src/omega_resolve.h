#ifndef OMEGA_RESOLVE_H
#define OMEGA_RESOLVE_H

/* omega_resolve: the Verified Crumb resolver (VC1 stage 4, ADR 0029 Decisions 3 to 5,
 * aien-protocols specs/verified-crumb/SPEC.md sections 5 to 8).
 *
 * ONE CHAIN, every link must hold, any failure REFUSES and no partial closure is ever returned:
 *
 *   import NAME -> omega.lock line -> semantic_id -> Verified Crumb Store -> receipt check
 *               -> transitive closure (every dependency repeats the chain, minus the lock)
 *
 * Per node, in this order (SPEC 6; the first failure wins):
 *   1 lock      name has a lock line, else DEPENDENCY_NOT_PINNED
 *   2 store     omega_vcstore_get(semantic_id); absent or integrity failure: UNVERIFIED_DEPENDENCY
 *   3 identity  key == record id; source or IR recheck (below); lock receipt == record
 *               receipt_id, else RECEIPT_HASH_MISMATCH
 *   - kind      a BOOTSTRAP record satisfies an import only when its id is a member of the
 *               pinned genesis set VC-GENESIS-1 (omega_genesis.h), else UNVERIFIED_DEPENDENCY
 *               (store code GENESIS_NOT_LISTED). Membership is the ONLY input: no flag, no
 *               environment variable, nothing read from the store.
 *   4 taint     capability "omega-dev.taint": TAINTED_ARTIFACT (both domains)
 *   5 profile   not in the table: UNKNOWN_VERIFIER_PROFILE; version below the minimum (or not a
 *               dotted number): VERIFIER_TOO_OLD
 *   6 receipt   absent: MISSING_RECEIPT; SPEC 5.1 rules 1,3,4,5,6 fail: RECEIPT_HASH_MISMATCH
 *               (rule 4 naming the semantic_id but another source digest: STALE_RECEIPT);
 *               then rule 2 (PASS, assertions pass, tier at least the profile minimum, never
 *               TEST_ONLY_TRUST): UNVERIFIED_DEPENDENCY
 *   7 edges     each dependency repeats 2 to 7; its contract_id must equal required_contract;
 *               a dependency already on the path: DEPENDENCY_CYCLE
 *   9 closure   the union, sorted ascending by semantic_id, with its digest
 * Declaration check (before any store access): every import needs a lock line
 * (DEPENDENCY_NOT_PINNED) and every lock line needs an import (UNDECLARED_IMPORT).
 *
 * DECISIONS (also in the PR body):
 *  - BOOTSTRAP records (the audited genesis set) carry an audit hash in receipt_id, not a
 *    receipt. A listed one skips steps 5 and 6 (the lock must still pin the audit hash). A
 *    BOOTSTRAP record may NOT depend on a VERIFIED record: the audited base must be closed over
 *    itself, so the resolver refuses it (UNVERIFIED_DEPENDENCY) and omega_resolve_admit_genesis
 *    refuses to insert it. A VERIFIED record may depend on BOOTSTRAP (the genesis set is where
 *    trust starts). The set is pinned in the source (omega_genesis.h, docs/osc/VC-GENESIS-1.md);
 *    changing it is a reviewed source change, never a runtime switch.
 *  - SOURCE/IR RECHECK (SPEC 6 step 3). In the build domain the source or IR store is
 *    MANDATORY: with no fetch_blob every node is refused (UNVERIFIED_DEPENDENCY). The blob must
 *    hash to source_or_ir_digest, and when the digest kind is IR the program id is recomputed
 *    with omega_program_compute_id (omega_program_ir.h) and must equal the record's semantic id.
 *    A record whose digest kind is OSC source cannot be recomputed to a program id here, so the
 *    build domain refuses it. The dev domain may omit the blob store (its output is tainted);
 *    when it supplies one the same checks run.
 *  - omega-dev and omega-build run the SAME checks. The domain never loosens a check (ADR 0029
 *    Decision 4: flags cannot change what is legal, only the closure can). It labels the output:
 *    an omega-dev artifact is TAINTED, its identity (build id) commits to that, and the resolver
 *    refuses to insert it into the store or to treat it as a dependency.
 *  - There is no skip switch, no environment variable and no fallback in this file or in the
 *    compiler. A test greps for them.
 *
 * LIMITS (stated, not hidden): receipts are hashes, not signatures (see omega_receipt.h); the
 * receipt binding (rule 4) ties semantic_id and source digest; contract_id is not recomputed
 * from the IR (the edge check compares the stored contract ids only). */

#include <stddef.h>
#include <stdint.h>

#include "omega_vcstore.h"

/* Resolve refusals. 1..9 are ARCH-0029 Decision 9 in the ADR's order (same numbers as the
 * reserved OMEGA_VCS_* codes), 10 is the VC1 addition of SPEC 6.1. */
enum {
    OMEGA_RES_OK = 0,
    OMEGA_RES_UNVERIFIED_DEPENDENCY = 1,
    OMEGA_RES_MISSING_RECEIPT,
    OMEGA_RES_RECEIPT_HASH_MISMATCH,
    OMEGA_RES_DEPENDENCY_NOT_PINNED,
    OMEGA_RES_DEPENDENCY_CYCLE,
    OMEGA_RES_STALE_RECEIPT,
    OMEGA_RES_UNDECLARED_IMPORT,
    OMEGA_RES_TAINTED_ARTIFACT,
    OMEGA_RES_UNKNOWN_VERIFIER_PROFILE,
    OMEGA_RES_VERIFIER_TOO_OLD,
    /* not trust decisions */
    OMEGA_RES_BAD_ARGUMENT = 20,
    OMEGA_RES_NOMEM
};
const char *omega_resolve_code_name(int code);

typedef struct {
    int code;                /* OMEGA_RES_* */
    int store_code;          /* OMEGA_VCS_* when the store said no, else 0 */
    char subject[140];       /* import name, or hex semantic id */
    char message[240];       /* plain text */
} OmegaResolveError;

typedef enum { OMEGA_DOMAIN_BUILD = 1, OMEGA_DOMAIN_DEV = 2 } OmegaDomain;
const char *omega_domain_name(OmegaDomain d);

/* ---- verifier profiles: profile name -> minimum verifier version and minimum receipt tier ---- */
typedef struct { const char *name, *min_version, *min_tier; } OmegaVerifierProfile;
/* The static table the compiler uses (names and tiers match aien-closure profile_min_tier):
 *   host-v1 HOST_TEST, qemu-v1 QEMU, production-v1 PRODUCTION, each minimum version 1.0.0.
 * There is no "accept any profile" entry. */
const OmegaVerifierProfile *omega_resolve_default_profiles(size_t *n);
int omega_resolve_check_profile(const OmegaVerifierProfile *table, size_t n, const char *profile,
                                const char *version, OmegaResolveError *err);

/* ---- omega.lock (SPEC 8) ---- */
#define OMEGA_LOCK_MAX_ENTRIES 4096
typedef struct { char name[129]; uint8_t semantic[32], receipt[32]; } OmegaLockEntry;
typedef struct { OmegaLockEntry *e; size_t n; } OmegaLock;
/* Fail-closed parser: any malformed lock is DEPENDENCY_NOT_PINNED. */
int omega_lock_parse(const char *text, size_t len, OmegaLock *out, OmegaResolveError *err);
void omega_lock_free(OmegaLock *l);
const OmegaLockEntry *omega_lock_find(const OmegaLock *l, const char *name);

/* ---- receipt and blob sources ---- */
/* 0 = found (*bytes malloc'd, caller frees), 1 = absent, other = error (refused as missing) */
typedef int (*OmegaReceiptFetch)(void *ctx, const uint8_t id[32], uint8_t **bytes, size_t *len);
typedef int (*OmegaBlobFetch)(void *ctx, const uint8_t digest[32], uint8_t **bytes, size_t *len);
/* ctx = const char *directory; reads <dir>/<64 lowercase hex>.json (receipts) or .blob (blobs) */
int omega_receipt_dir_fetch(void *dir, const uint8_t id[32], uint8_t **bytes, size_t *len);
int omega_blob_dir_fetch(void *dir, const uint8_t digest[32], uint8_t **bytes, size_t *len);

/* ---- the resolver ---- */
typedef struct {
    const OmegaVcStore *store;
    OmegaDomain domain;
    const OmegaVerifierProfile *profiles;  /* NULL (with n_profiles 0): the default table */
    size_t n_profiles;
    OmegaReceiptFetch fetch_receipt;       /* required for any VERIFIED record */
    void *fetch_ctx;
    OmegaBlobFetch fetch_blob;             /* REQUIRED in the build domain (without it every node
                                              is refused); optional in dev. SHA-256 of the blob named
                                              by source_or_ir_digest must equal that digest, and an
                                              IR blob must recompute to the semantic id */
    void *blob_ctx;
} OmegaResolver;

typedef struct { uint8_t semantic_id[32], receipt_id[32], admission_kind; } OmegaClosureEntry;
typedef struct {
    OmegaClosureEntry *entries;            /* sorted ascending by semantic_id */
    size_t n;
    uint8_t digest[32];                    /* omega_closure_digest of entries */
} OmegaClosure;
void omega_closure_free(OmegaClosure *c);

/* SHA-256("OMEGA.CLOSURE.V1" 0x00 || u32be n || n x (semantic_id || receipt_id || kind byte)). */
void omega_closure_digest(const OmegaClosureEntry *e, size_t n, uint8_t out[32]);

/* Resolve the imports of one compilation. names may be empty (n_names 0): the declaration check
 * still runs, so a lock with entries and no imports is UNDECLARED_IMPORT. 0 or a refusal code
 * with *err filled; on refusal *out is empty (never partial). */
int omega_resolve_imports(const OmegaResolver *r, const OmegaLock *lock, const char *const *names,
                          size_t n_names, OmegaClosure *out, OmegaResolveError *err);

/* SPEC 5.1 for one record: the receipt named by vc->receipt_id against profile, semantic id,
 * source digest, evidence root and the receipt ids of the record's dependencies (read from the
 * store). Steps 5 and 6 of the chain; exported for the admit path and tests. */
int omega_resolve_check_record_receipt(const OmegaResolver *r, const OmegaVcView *vc, OmegaResolveError *err);

/* ---- the one door into the store ---- */
/* Build metadata of a compile: the domain, the taint mark and the identities. */
typedef struct {
    OmegaDomain domain;
    uint8_t tainted;                       /* 1 exactly when domain is omega-dev */
    uint8_t ir_digest[32], closure_digest[32], build_id[32];
} OmegaArtifactMeta;

/* build_id = SHA-256("OSC1.BUILD.V1" 0x00 || ir_digest || domain byte || closure_digest).
 * THE place the closure digest enters a build's identity: two builds of the same IR with
 * different closures, or in different domains, have different build ids. */
void omega_build_id(const uint8_t ir_digest[32], OmegaDomain d, const uint8_t closure_digest[32], uint8_t out[32]);
void omega_artifact_meta_make(OmegaArtifactMeta *m, OmegaDomain d, const uint8_t ir_digest[32], const uint8_t closure_digest[32]);
/* Text header, "OMEGA-ARTIFACT v1\ndomain dev\ntainted 1\nir_sha256 <hex>\nclosure_sha256 <hex>\nbuild_id <hex>\n". */
int omega_artifact_meta_text(const OmegaArtifactMeta *m, char *buf, size_t cap);
/* Fail-closed parse: unknown line, wrong order, tainted mark inconsistent with the domain, or a
 * build_id that does not recompute: refused (TAINTED_ARTIFACT for a mark/identity lie,
 * BAD_ARGUMENT for malformed text). */
int omega_artifact_meta_parse(const char *text, size_t len, OmegaArtifactMeta *m, OmegaResolveError *err);

/* Insert a Verified Crumb into the store, only together with the receipt that qualifies it
 * (THE only way a VERIFIED record enters a store; the raw insert is private to this file, see
 * omega_vcstore_priv.h):
 * origin (may be NULL when the record did not come from a compile) tainted: TAINTED_ARTIFACT;
 * the record lists omega-dev.taint: TAINTED_ARTIFACT; then steps 5 and 6 above; then
 * the private raw insert (VERIFIED). The store is unchanged on every refusal. */
int omega_resolve_admit(const OmegaResolver *r, OmegaVcStore *s, const uint8_t *canonical, size_t len,
                        const uint8_t claimed_vc_id[32], const OmegaArtifactMeta *origin, OmegaResolveError *err);
/* Genesis loading: same taint refusals, no receipt (audit hash), refuses any VERIFIED
 * dependency, and refuses any record whose semantic id is not a member of VC-GENESIS-1
 * (UNVERIFIED_DEPENDENCY with store code GENESIS_NOT_LISTED). THE only way a BOOTSTRAP record
 * enters a store. */
int omega_resolve_admit_genesis(OmegaVcStore *s, const uint8_t *canonical, size_t len,
                                const uint8_t claimed_vc_id[32], const OmegaArtifactMeta *origin, OmegaResolveError *err);

#define OMEGA_TAINT_CAPABILITY "omega-dev.taint"
/* A record minted by omega_vc_bridge (no independent qualification run) lists this capability. The
 * build-domain resolver refuses it, so a bridge record can sit in a store but can never satisfy a
 * build import (VC1 stage 6 fix). The dev domain accepts it (the artifact is tainted anyway). */
#define OMEGA_BRIDGE_SELFMINTED_CAPABILITY "omega-bridge.selfminted"

#endif /* OMEGA_RESOLVE_H */
