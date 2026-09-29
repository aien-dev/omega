/* TURING Field V0: records that link one contract to its realization specs,
 * the evidence measured for them, and the selection decisions that cite that
 * evidence. docs/turing/TURING_W0_PROPOSAL.md sections F and K.
 *
 * Data only, not a subsystem. Built post hoc from existing receipts
 * (evidence/MIXED_ALGEBRA) and the oma_rz_impl registry, read-only. Nothing
 * here touches src/runtime, src/algebra or src/polyglot.
 *
 * Canonical bytes: every record is laid out as an OMG0 object
 * (omega_canonical_encode: attributes sorted by key, relations sorted by kind
 * then target) and digested as SHA-256(domain || 0x00 || OMG0 bytes). The
 * domain prefix keeps every Field digest distinct from an Omega semantic id
 * (which is SHA-256(OMG0 bytes) with no prefix); Field digests are NOT Omega
 * semantic ids and must never be used as one (OSC-0B, omega PR #76).
 *
 * contract_digest is PROVISIONAL (V0): no existing digest names the Omega-X
 * contract, so V0 digests the canonical contract bytes defined in field.c.
 */
#ifndef TURING_FIELD_H
#define TURING_FIELD_H

#include <stddef.h>
#include <stdint.h>

#define TURING_DIGEST_BYTES 32
#define TURING_TEXT 96
#define TURING_MAX_SPECS 32
#define TURING_MAX_EVIDENCE 1024
#define TURING_MAX_RECEIPTS 8
#define TURING_MAX_CAND 16
#define TURING_MAX_CITE 128 /* v1 cites a whole footprint: specs x sparsities x receipts */

#define TURING_DOMAIN_CONTRACT "turing.contract.v0.provisional"
#define TURING_DOMAIN_SPEC "turing.spec.v0"
#define TURING_DOMAIN_EVIDENCE "turing.evidence.v0"
#define TURING_DOMAIN_DECISION "turing.decision.v1" /* v1: cites committed as one cite-set digest */
#define TURING_DOMAIN_CITESET "turing.citeset.v0"

/* ADR 0019 tiers are PROPOSED: tier is stored as text with the revision it
 * came from, never as an enum bound to the schema. */
#define TURING_TIER_SOURCE "ADR 0019 PROPOSED, aien-architecture PR #58 head ed4474a"

typedef struct {
    uint8_t b[TURING_DIGEST_BYTES];
} turing_digest;

/* The operation's contract (canonical bytes -> PROVISIONAL contract_digest). */
typedef struct {
    char name[TURING_TEXT];
    char statement[256];
    char exactness[TURING_TEXT];
    char overflow[TURING_TEXT];
    char oracle[TURING_TEXT];
    char source[TURING_TEXT];
    uint64_t max_n;
} turing_contract;

/* A realization choice. No build fields: spec_id must be stable across
 * rebuilds (Fable item 2). */
typedef struct {
    turing_digest contract_digest;
    char rz_id[32];           /* oma_rz_impl.id, e.g. "R2c_crumb" */
    char algorithm[TURING_TEXT];
    char algebra[TURING_TEXT];
    char representation[TURING_TEXT];
    char precision[TURING_TEXT];
    char language[TURING_TEXT];
    char backend[TURING_TEXT];
    char machine_class[TURING_TEXT];
    uint64_t max_n;
    int exact;
    int weak_baseline;
} turing_rz_spec;

enum { TURING_PACK_ONCE = 0, TURING_PACK_PER_CALL = 1 };

/* One measured cell of one receipt for one spec. Build identity lives here. */
typedef struct {
    turing_digest spec_id;
    uint64_t n, m;
    uint32_t sparsity_milli;      /* sparsity x 1000 */
    turing_digest receipt_digest; /* SHA-256 of the receipt file bytes */
    char receipt_path[256];       /* where to find it; NOT part of the digest */
    char run_id[TURING_TEXT];
    char tier[8];                 /* text, e.g. "E2" */
    char tier_source[TURING_TEXT];
    int verified;                 /* receipt "verified" (oracle-checked) */
    uint32_t forced, retries, samples;
    uint64_t median_ps, q25_ps, q75_ps, min_ps, pack_ps;
    uint32_t noise_ppm, pack_noise_ppm;
    uint64_t weight_bytes, working_set_bytes;
    char build_digest[72];        /* bench binary SHA-256 (hex) */
    char git_commit[48];
    int tree_dirty;
    char toolchain[TURING_TEXT];
    char cpu_freq_state[TURING_TEXT];
    char quiet_flag[TURING_TEXT];
    char thermal[TURING_TEXT];
} turing_evidence;

/* Filter / outcome reason codes carried per candidate in a decision. */
typedef enum {
    TURING_R_CHOSEN = 0,
    TURING_R_RANKED,            /* eligible, measured, not chosen */
    TURING_R_TIED,              /* eligible, inside the tied set, not chosen */
    TURING_R_CONTRACT_MISMATCH,
    TURING_R_NOT_EXACT,
    TURING_R_MAX_N,
    TURING_R_NO_EVIDENCE,
    TURING_R_RECEIPT_UNVERIFIED,
    TURING_R_UNVERIFIED_RUN,
    TURING_R_CONTENTION_FORCED,
    TURING_R_TIER,
    TURING_R_EXPLORE,           /* control arm: cold-start pick */
    TURING_R__COUNT
} turing_reason;

const char *turing_reason_name(turing_reason r);

typedef struct {
    turing_digest contract_digest;
    char selector[32];          /* "turing.field.v1", "turing.history.v0", or retired "turing.field.v0" */
    char constraints[TURING_TEXT];
    uint64_t n, m;
    uint32_t sparsity_milli;
    int pack;                   /* TURING_PACK_* */
    uint64_t cell_n, cell_m;    /* the measured cell (v0) or footprint (v1) the costs came from */
    uint32_t cell_sparsity_milli; /* TURING_SPARSITY_ANY when the footprint spans every sparsity */
    int exact_cell;
    size_t ncand;
    turing_digest cand[TURING_MAX_CAND];
    char cand_rz[TURING_MAX_CAND][32];
    turing_reason reason[TURING_MAX_CAND];
    uint64_t cost_ps[TURING_MAX_CAND]; /* 0 when unmeasured */
    int chosen;                 /* index into cand, -1 none */
    char verdict[16];           /* CHOSEN, TIE, NONE, EXPLORE */
    char tie_resolution[16];    /* none, incumbent, reference, cheapest */
    uint32_t margin_ppm, band_ppm;
    size_t ncite;
    turing_digest cite[TURING_MAX_CITE]; /* evidence digests */
    int has_supersedes;
    turing_digest supersedes;
} turing_decision;

typedef struct {
    turing_digest digest;
    char path[256];
    char run_id[TURING_TEXT];
} turing_receipt_ref;

typedef struct {
    turing_contract contract;
    turing_digest contract_digest;
    size_t nspec;
    turing_rz_spec spec[TURING_MAX_SPECS];
    turing_digest spec_id[TURING_MAX_SPECS];
    size_t nev;
    turing_evidence ev[TURING_MAX_EVIDENCE];
    turing_digest ev_id[TURING_MAX_EVIDENCE];
    size_t nreceipt;
    turing_receipt_ref receipt[TURING_MAX_RECEIPTS];
} turing_store;

/* Digests. Every function returns 0 on success, -1 on error. */
int turing_contract_digest(const turing_contract *c, turing_digest *out);
int turing_spec_digest(const turing_rz_spec *s, turing_digest *out);
int turing_evidence_digest(const turing_evidence *e, turing_digest *out);
int turing_decision_digest(const turing_decision *d, turing_digest *out);
/* Canonical bytes (OMG0 layout) of a record; *len set. For tests. */
int turing_spec_bytes(const turing_rz_spec *s, uint8_t *buf, size_t cap, size_t *len);

void turing_hex(const turing_digest *d, char out[65]);
void turing_hex_short(const turing_digest *d, char out[13]);
int turing_digest_eq(const turing_digest *a, const turing_digest *b);
int turing_file_digest(const char *path, turing_digest *out);

/* The Omega-X contract (spec/mixed-algebra-ma2.md, realize_common.h). */
void turing_contract_omega_x(turing_contract *c);

/* Store. The store is large: allocate with turing_store_new. */
turing_store *turing_store_new(void);
void turing_store_free(turing_store *st);
/* Adapter 1: oma_rz_impl registry (read-only) -> spec records. */
int turing_ingest_registry(turing_store *st);
/* Adapter 2: MA-3 bench receipt (post hoc, read-only) -> spec (must already
 * exist or be identical) + evidence records. Returns number of evidence
 * records added, or -1. */
int turing_ingest_receipt(turing_store *st, const char *path);
/* Spec derived from a receipt's realizations row (for the rebuild cross-check). */
int turing_spec_from_fields(const turing_digest *contract, const char *rz_id, const char *family,
                            int exact, int weak_baseline, uint64_t max_n, turing_rz_spec *out);
int turing_find_spec(const turing_store *st, const char *rz_id);
int turing_find_evidence(const turing_store *st, const turing_digest *ev_id);

/* Decision verification: recompute the decision's cited evidence digests from
 * the store and re-hash each cited receipt file. Returns 0 when every cited
 * receipt verifies; otherwise a negative code and a reason in why. */
int turing_decision_verify(const turing_store *st, const turing_decision *d, char *why, size_t whylen);

/* Per-call cost of one evidence record for a pack mode. */
uint64_t turing_ev_cost(const turing_evidence *e, int pack);

#endif /* TURING_FIELD_H */
