#ifndef OMEGA_VC_BRIDGE_H
#define OMEGA_VC_BRIDGE_H

/* omega_vc_bridge: the one in-process way a program enters an OmegaLibrary together with the
 * Verified Crumb Store (VC1 stage 6, ADR 0029 Decisions 8 and 11).
 *
 * Before this existed Crumbline and discovery called omega_library_insert directly with a bare
 * 32-byte hash. Now every program goes through omega_resolve_admit (the only door into the
 * store): the bridge builds the canonical Verified Crumb of the program (semantic id =
 * omega_program_compute_id, contract id = omega_program_contract_id, digest kind IR, the IR of
 * omega_program_ir.h), writes an evidence receipt for it, and lets the resolver run SPEC 5.1 on
 * that receipt. Only then is the library entry inserted, with the RECEIPT ID as its evidence hash.
 *
 * WHAT THE RECEIPT IS (stated, not hidden): the bridge itself runs omega_program_verify on a private
 * copy of the program and recomputes the program id from the IR it stores, and refuses (-1) when
 * either fails. It does NOT trust the caller's is_verified flag or program_id. It then records that
 * as an EvidenceReceiptV1 of kind host-v1, tier HOST_TEST, with the evidence digest the caller
 * supplies as its output_digest (the evidence_root). It is still a SELF-MINTED HOST_TEST receipt:
 * the same code that asks for admission writes it, and no independent verifier re-ran anything.
 * So the record it makes lists the capability OMEGA_BRIDGE_SELFMINTED_CAPABILITY, which the
 * build-domain resolver refuses: a bridge record can sit in a store, but it can never satisfy a
 * build import (oscv --domain build). The dev domain accepts it. Real aien-test receipts for
 * Crumbline and discovery programs are owed (ADR 0029 Decision 5 and 11) and are not here.
 *
 * NOT A GENESIS PATH: the bridge never calls omega_resolve_admit_genesis. Nothing here can create
 * a BOOTSTRAP record. */

#include <stddef.h>
#include <stdint.h>

#include "omega_discovery.h"
#include "omega_library.h"
#include "omega_program.h"
#include "omega_resolve.h"
#include "omega_vcstore.h"

typedef struct { uint8_t id[32]; char *json; size_t len; } OmegaVcBridgeReceipt;
typedef struct { uint8_t digest[32]; uint8_t *bytes; size_t len; } OmegaVcBridgeBlob;

typedef struct {
    OmegaVcStore store;
    OmegaVcBridgeReceipt *receipts; size_t n_receipts, cap_receipts;
    OmegaVcBridgeBlob *blobs;       size_t n_blobs, cap_blobs;
} OmegaVcBridge;

int  omega_vc_bridge_init(OmegaVcBridge *b);
void omega_vc_bridge_destroy(OmegaVcBridge *b);

/* Receipt and IR sources over the bridge's own tables, in the resolver's fetch signatures
 * (ctx = the OmegaVcBridge *), so a resolver can be pointed at the same store. */
int omega_vc_bridge_fetch_receipt(void *ctx, const uint8_t id[32], uint8_t **bytes, size_t *len);
int omega_vc_bridge_fetch_blob(void *ctx, const uint8_t digest[32], uint8_t **bytes, size_t *len);

/* Admit prog (realized by the caller; verified and id-recomputed here) into the store through omega_resolve_admit,
 * then into lib with the receipt id as its evidence hash. deps/dep_count are library
 * dependencies; each must already be admitted through this bridge. evidence is the caller's
 * non-zero evidence digest (it becomes the record's evidence_root). 0 on success; -1 and nothing
 * inserted into lib on any refusal (a record that reached the store stays there: records are
 * immutable and identical bytes are a no-op). */
int omega_vc_bridge_admit(OmegaVcBridge *b, OmegaLibrary *lib, const OmegaProgram *prog,
                          const SemanticId *deps, size_t dep_count, const uint8_t evidence[32]);

/* Discovery: the abstraction of cand, with the same guards omega_discovery_admit_to_library had
 * (verified, non-trivial, positive compression). */
int omega_vc_bridge_admit_abstraction(OmegaVcBridge *b, OmegaLibrary *lib, const OmegaAbstractionCandidate *cand,
                                      const uint8_t evidence[32]);

#endif /* OMEGA_VC_BRIDGE_H */
