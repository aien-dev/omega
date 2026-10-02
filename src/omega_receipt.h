#ifndef OMEGA_RECEIPT_H
#define OMEGA_RECEIPT_H

/* omega_receipt: reader for the aien-proof EvidenceReceiptV1 wire contract (VC1 stage 4).
 *
 * Authority: aien-protocols specs/evidence-receipt/SUBSET.md (pinned source aien-sovereign-core
 * 40dd373, crates/aien-proof/src/evidence.rs). This file is a second, independent reader of that
 * contract in C: it parses the receipt JSON the way serde_json does for `Receipt`
 * (deny_unknown_fields, the same required and defaulted fields), applies every validation that
 * `canonical_bytes` applies, builds the canonical byte encoding (domain tag, big-endian integers,
 * length-prefixed strings, sorted unique lists) and recomputes the id as BLAKE3 over those bytes.
 * tests/resolve/receipts/ holds three receipts written by the Rust implementation; the C id must
 * equal the Rust id for each.
 *
 * NOT replicated (stated, not hidden): the ledger and lease bindings of verify_with_store
 * (ledger_bind.rs, lease.rs need the proof board store) and chain walking (the resolver walks the
 * Verified Crumb closure itself and ties receipt dependencies to it, SPEC 5.1 rule 6). A receipt
 * whose hash matches is only as trustworthy as the store it came from: receipts are not signed.
 *
 * Every function fails closed: any parse error, unknown field, bad enum, bad hex or oversize input
 * is a nonzero return with a plain-text reason. */

#include <stddef.h>
#include <stdint.h>

#define OMEGA_RECEIPT_MAX_BYTES (4u * 1024u * 1024u)

typedef struct { char *id, *expected, *observed, *source, *note; int pass; } OmegaReceiptAssertion;

typedef struct {
    uint32_t version;
    uint8_t id[32];                 /* the "id" field, decoded (must equal the recomputed id) */
    char *kind, *tier, *repo, *commit, *toolchain, *procedure, *machine, *env_class, *authority;
    uint8_t result;                 /* 0 PASS 1 FAIL 2 BLOCKED 3 INCOMPLETE 4 SKIPPED */
    int tier_rank;                  /* 0 TEST_ONLY_TRUST .. 7 PRODUCTION */
    int dirty;
    uint64_t timestamp;
    char **input_artifacts;  size_t n_input;
    char **output_artifacts; size_t n_output;
    OmegaReceiptAssertion *assertions; size_t n_assertions;
    char **dependencies;     size_t n_dependencies;
    uint8_t declared_mutation, observed_mutation;
    uint8_t output_digest[32];
    char **external_refs;    size_t n_external;
    int has_ledger; uint64_t ledger_index; uint8_t ledger_hash[32];
    int has_lease;  uint8_t lease_hold[32]; char *lease_resource;
    char **required_features; size_t n_features;
} OmegaReceipt;

#define OMEGA_RECEIPT_PASS 0

/* Parse and validate receipt JSON (everything canonical_bytes validates, plus serde's shape
 * rules). 0 ok; -1 with err filled. On failure *r holds nothing to free. */
int  omega_receipt_parse(const uint8_t *json, size_t len, OmegaReceipt *r, char *err, size_t errcap);
void omega_receipt_free(OmegaReceipt *r);

/* The canonical bytes (malloc'd, caller frees) and id = BLAKE3(canonical bytes). */
int omega_receipt_canonical(const OmegaReceipt *r, uint8_t **out, size_t *len);
int omega_receipt_compute_id(const OmegaReceipt *r, uint8_t out[32]);

/* Parse, recompute, and require the "id" field to equal the recomputed id (case-insensitive hex,
 * as aien-proof verify_bytes). 0 ok with *r filled and out_id set; -1 with err. */
int omega_receipt_verify_bytes(const uint8_t *json, size_t len, OmegaReceipt *r, uint8_t out_id[32],
                               char *err, size_t errcap);

/* Tier rank by name (-1 unknown) and the aien-proof tier_satisfies rule (chain.rs:259). */
int omega_tier_rank(const char *tier);
int omega_tier_satisfies(int need_rank, int have_rank);

#endif /* OMEGA_RECEIPT_H */
