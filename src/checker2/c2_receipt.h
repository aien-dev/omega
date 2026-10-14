/* c2_receipt: signed Omega realization receipts, format v1 (spec/one-layer-receipts.md, normative).
 *
 * A receipt is 416 bytes, fixed layout, little-endian, no text, no timestamps. It binds one
 * checker2 verdict to the exact bytes it judged (requirements, canonical program bytes, executable
 * bytes, dependencies, checker identities, evidence class, target, firmware, input identity) and a
 * policy-epoch window, and carries a pure Ed25519 signature (RFC 8032 section 5.1) made by a key of
 * one signer class: TEST (class 1, automated tests only) or OWNER (class 2, Drake's owner root, held
 * on a YubiKey; this code never sees that private key). The class numbers equal aienos
 * native/kernel/artifact/osc_unit.h OSC_SIGNER_TEST / OSC_SIGNER_OWNER.
 *
 * A signature establishes provenance (who vouched), never correctness. A receipt grants no
 * capability. Receipt identity = SHA-256 of all 416 bytes; a receipt is never rewritten.
 *
 * No heap, no clock, no I/O in this file (the store and the tool live in c2_receipt_tool.c). */
#ifndef C2_RECEIPT_H
#define C2_RECEIPT_H

#include <stddef.h>
#include <stdint.h>

#define C2R_SIZE 416u
#define C2R_SIGNED_LEN 352u   /* bytes [0, 352) are covered by the signature */
#define C2R_FORMAT_VERSION 1u
#define C2R_ALG_ED25519 1u

enum { C2R_CLASS_TEST = 1, C2R_CLASS_OWNER = 2 };

/* Evidence class: what checker2 actually did before the receipt was signed. */
enum {
    C2R_EV_IR_CHECKED = 1,     /* checker2 decoded the canonical bytes and accepted them;
                                  the executable is bound by hash, NOT translation-validated */
    C2R_EV_IR_CHECKED_LRAT = 2 /* as 1, plus an LRAT-checked proof of the declared property */
};

typedef struct {
    uint16_t version;              /* C2R_FORMAT_VERSION */
    uint16_t signer_class;         /* C2R_CLASS_* */
    uint16_t alg;                  /* C2R_ALG_ED25519 */
    uint16_t evidence_class;       /* C2R_EV_* */
    uint8_t requirements[32];      /* SHA-256 of the requirements object bytes */
    uint8_t program[32];           /* SHA-256 of the canonical program bytes (OSC IR) */
    uint8_t executable[32];        /* SHA-256 of the realized executable bytes */
    uint8_t dependencies[32];      /* c2r_deps_digest over the dependency receipt ids */
    uint8_t checker[32];           /* identity of the checker2 build that judged */
    uint8_t producer_checker[32];  /* identity of the producer's checker (oscc binary hash) */
    uint8_t target[32];            /* c2r_target_id(target name) */
    uint8_t firmware[32];          /* SHA-256 of the firmware image, all zero = none (host) */
    uint8_t input[32];             /* SHA-256 of the input / dataset identity, zero = none */
    uint64_t policy_epoch;         /* epoch the receipt was issued under */
    uint64_t expiry_epoch;         /* last epoch at which it is fresh (>= policy_epoch) */
    uint8_t key_fp[32];            /* SHA-256 of the signer's 32-byte Ed25519 public key */
    uint8_t sig[64];
} C2Receipt;

/* Named verdicts. 0 is the only acceptance. Values are part of the spec; never renumbered. */
enum {
    C2R_OK = 0,
    C2R_E_SIZE = 1,             /* not exactly 416 bytes */
    C2R_E_MAGIC = 2,
    C2R_E_VERSION = 3,
    C2R_E_ALG = 4,
    C2R_E_CLASS = 5,            /* signer class not TEST or OWNER */
    C2R_E_EVIDENCE = 6,         /* unknown evidence class */
    C2R_E_EPOCH_ORDER = 7,      /* expiry_epoch < policy_epoch */
    /* 8 is unassigned (format v1 has no reserved bytes) */
    C2R_E_TEST_IN_PRODUCTION = 9,
    C2R_E_UNTRUSTED_SIGNER = 10, /* key fingerprint not in the anchor set of its class */
    C2R_E_BAD_SIGNATURE = 11,
    C2R_E_REVOKED = 12,         /* receipt id or signer key fingerprint is revoked */
    C2R_E_STALE = 13,           /* policy_epoch below the verifier's floor: replay of a superseded receipt */
    C2R_E_EXPIRED = 14,         /* current epoch past expiry_epoch */
    C2R_E_NOT_YET_VALID = 15,   /* current epoch before policy_epoch */
    C2R_E_WRONG_TARGET = 16,
    C2R_E_WRONG_FIRMWARE = 17,
    C2R_E_PROGRAM_MISMATCH = 18,
    C2R_E_EXECUTABLE_MISMATCH = 19,
    C2R_E_ARG = 20,
    C2R_E_KEY_CLASS = 21,       /* signing: key not allowed for the requested class */
    C2R_E_UNTRUSTED_CHECKER = 22, /* checker identity not in the verifier's checker list */
    C2R_E_WRONG_PRODUCER = 23,  /* producer checker identity differs from the expected one */
    C2R_E_DEPS_MISMATCH = 24    /* dependency digest differs from the expected one */
};
const char *c2r_reason(int code);

/* Verifier policy. Anchors are raw 32-byte Ed25519 public keys; the key used is always taken from
 * the anchor set of the receipt's class, never from the receipt. production = 1 refuses TEST class
 * outright and accepts only the anchors in owner_anchors. */
typedef struct {
    int production;
    const uint8_t (*test_anchors)[32];  unsigned n_test;
    const uint8_t (*owner_anchors)[32]; unsigned n_owner;
    const uint8_t (*revoked_ids)[32];   unsigned n_revoked_ids;   /* receipt ids */
    const uint8_t (*revoked_keys)[32];  unsigned n_revoked_keys;  /* key fingerprints */
    /* checker2 identities (section 5) whose verdicts this verifier accepts. Required: an empty or
     * NULL list refuses every receipt (C2R_E_UNTRUSTED_CHECKER), so a policy cannot forget it. */
    const uint8_t (*checkers)[32];      unsigned n_checkers;
    uint64_t epoch_floor;     /* receipts issued under an older epoch are refused (C2R_E_STALE) */
    uint64_t current_epoch;
} C2ReceiptPolicy;

/* What the admitting side expects the receipt to be about. NULL fields are not checked. */
typedef struct {
    const uint8_t *target;      /* 32 bytes: c2r_target_id of the launching target */
    const uint8_t *firmware;    /* 32 bytes */
    const uint8_t *program;     /* 32 bytes: SHA-256 of the program bytes being admitted */
    const uint8_t *executable;  /* 32 bytes: SHA-256 of the executable bytes being admitted */
    const uint8_t *producer_checker; /* 32 bytes: identity of the producer's checker (oscc hash) */
    const uint8_t *dependencies; /* 32 bytes: c2r_deps_digest of the expected dependency receipt ids.
                                    Format v1 binds the ids only: the dependency receipts themselves
                                    are NOT fetched, resolved or verified here. */
} C2ReceiptExpect;

/* Drake's owner root public key (aienos#380 ceremony, YubiKey PIV slot 9c). */
extern const uint8_t c2r_owner_root_pk[32];
/* The TEST-tier key: secret seed = SHA-256("OMEGA-CHECKER2-TEST-KEY-v1 NOT FOR PRODUCTION").
 * Public on purpose: anything it signs is TEST class and refused in production. */
void c2r_test_seed(uint8_t seed[32]);
extern const uint8_t c2r_test_pk[32];

void c2r_target_id(const char *target_name, uint8_t out[32]);
/* SHA-256("OMEGA-RECEIPT-DEPS-V1\0" || u32 count || ids in ascending byte order). ids must be
 * strictly ascending (sorted, unique); returns -1 otherwise. count 0 is allowed. */
int c2r_deps_digest(const uint8_t (*ids)[32], unsigned n, uint8_t out[32]);

void c2r_encode(const C2Receipt *r, uint8_t out[C2R_SIZE]);
/* Structural decode only (size, magic, version, alg, class, evidence, epoch order). */
int c2r_decode(const uint8_t *buf, size_t len, C2Receipt *r);
void c2r_id(const uint8_t buf[C2R_SIZE], uint8_t out[32]);
/* The exact bytes the Ed25519 signature covers (65 bytes):
 * "OMEGA-REALIZATION-RECEIPT-SIG-V1\0" || SHA-256("OMEGA-REALIZATION-RECEIPT-V1\0" || buf[0,352)). */
#define C2R_SIGNED_MSG_LEN 65u
void c2r_signed_message(const uint8_t buf[C2R_SIZE], uint8_t msg[C2R_SIGNED_MSG_LEN]);

/* Sign r (fills key_fp and sig, then encodes into out). TEST class only takes the TEST key;
 * OWNER class is refused here (the owner key never leaves the YubiKey: use c2r_attach). */
int c2r_sign_test(C2Receipt *r, const uint8_t seed[32], uint8_t out[C2R_SIZE]);
/* Attach a signature made elsewhere (YubiKey) over c2r_signed_message for public key pk.
 * Verifies it before returning 0. */
int c2r_attach(C2Receipt *r, const uint8_t pk[32], const uint8_t sig[64], uint8_t out[C2R_SIZE]);

/* Full admission check, in spec order. Returns C2R_OK or the first failing named reason. */
int c2r_verify(const uint8_t *buf, size_t len, const C2ReceiptPolicy *pol, const C2ReceiptExpect *ex,
               C2Receipt *out);

#endif /* C2_RECEIPT_H */
