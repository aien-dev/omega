/* c2_receipt.c: signed Omega realization receipts v1. Normative layout: spec/one-layer-receipts.md.
 *
 * Signature code: the vendored in-house Ed25519 (src/searchtrace/sig, aienos native/sig @ 913b962,
 * PROVENANCE.txt there). SHA-256: src/sha256.c. No other dependency. */
#include "c2_receipt.h"

#include <string.h>

#include "sha256.h"
#include "searchtrace/sig/aienos_sig.h"

static const uint8_t MAGIC[8] = {'O', 'M', 'R', 'C', 'P', 'T', '1', 0};

/* 578ada3c...9e76: aienos-owner-ceremony/20261014T114310Z/slot-9c-public.pem and
 * owner-pubkeys.txt (aienos#380). Public data; tests/checker2/fixtures/owner holds the ceremony
 * statement and its real signature, which test_c2_receipt verifies against this key. */
const uint8_t c2r_owner_root_pk[32] = {
    0x57, 0x8a, 0xda, 0x3c, 0xb0, 0xc2, 0xd1, 0x54, 0x62, 0x3a, 0xc0, 0x89, 0x19, 0x10, 0x23, 0xc8,
    0xad, 0x4b, 0xc5, 0xe6, 0x3b, 0x8c, 0xcd, 0x1a, 0x38, 0xdb, 0x2d, 0x67, 0x9b, 0x8c, 0x9e, 0x76};

/* Derived from c2r_test_seed by aienos_ed25519_public_key; test_c2_receipt re-derives and compares. */
const uint8_t c2r_test_pk[32] = {
    0x60, 0x28, 0x10, 0xd4, 0x3c, 0x01, 0x15, 0xdf, 0x87, 0x7b, 0x0c, 0xf8, 0xc3, 0x27, 0x01, 0x0e,
    0x5a, 0x7e, 0x57, 0x04, 0x90, 0xc8, 0x5e, 0x31, 0x46, 0x6c, 0x0c, 0xff, 0x6c, 0xa3, 0x74, 0xc5};

void c2r_test_seed(uint8_t seed[32])
{
    static const char s[] = "OMEGA-CHECKER2-TEST-KEY-v1 NOT FOR PRODUCTION";
    sha256_hash((const uint8_t *)s, sizeof s - 1, seed);
}

const char *c2r_reason(int code)
{
    switch (code) {
    case C2R_OK: return "OK";
    case C2R_E_SIZE: return "SIZE";
    case C2R_E_MAGIC: return "MAGIC";
    case C2R_E_VERSION: return "VERSION";
    case C2R_E_ALG: return "SIGNATURE_ALGORITHM";
    case C2R_E_CLASS: return "SIGNER_CLASS";
    case C2R_E_EVIDENCE: return "EVIDENCE_CLASS";
    case C2R_E_EPOCH_ORDER: return "EPOCH_ORDER";
    case C2R_E_TEST_IN_PRODUCTION: return "TEST_SIGNER_IN_PRODUCTION";
    case C2R_E_UNTRUSTED_SIGNER: return "UNTRUSTED_SIGNER";
    case C2R_E_BAD_SIGNATURE: return "BAD_SIGNATURE";
    case C2R_E_REVOKED: return "REVOKED";
    case C2R_E_STALE: return "STALE_EPOCH";
    case C2R_E_EXPIRED: return "EXPIRED";
    case C2R_E_NOT_YET_VALID: return "NOT_YET_VALID";
    case C2R_E_WRONG_TARGET: return "WRONG_TARGET";
    case C2R_E_WRONG_FIRMWARE: return "WRONG_FIRMWARE";
    case C2R_E_PROGRAM_MISMATCH: return "PROGRAM_MISMATCH";
    case C2R_E_EXECUTABLE_MISMATCH: return "EXECUTABLE_MISMATCH";
    case C2R_E_ARG: return "ARGUMENT";
    case C2R_E_KEY_CLASS: return "KEY_CLASS";
    default: return "UNKNOWN";
    }
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint64_t get64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

void c2r_target_id(const char *name, uint8_t out[32])
{
    static const char tag[] = "OMEGA-TARGET-V1"; /* sizeof includes the NUL */
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)tag, sizeof tag);
    sha256_update(&c, (const uint8_t *)name, strlen(name));
    sha256_final(&c, out);
}

int c2r_deps_digest(const uint8_t (*ids)[32], unsigned n, uint8_t out[32])
{
    static const char tag[] = "OMEGA-RECEIPT-DEPS-V1";
    for (unsigned i = 1; i < n; i++)
        if (memcmp(ids[i - 1], ids[i], 32) >= 0) return -1;
    uint8_t cnt[4] = {(uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24)};
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)tag, sizeof tag);
    sha256_update(&c, cnt, 4);
    for (unsigned i = 0; i < n; i++) sha256_update(&c, ids[i], 32);
    sha256_final(&c, out);
    return 0;
}

/* Offsets (spec/one-layer-receipts.md section 2). */
enum {
    O_MAGIC = 0, O_VER = 8, O_CLASS = 10, O_ALG = 12, O_EV = 14,
    O_REQ = 16, O_PROG = 48, O_EXE = 80, O_DEPS = 112, O_CHK = 144, O_PCHK = 176,
    O_TGT = 208, O_FW = 240, O_IN = 272, O_EPOCH = 304, O_EXP = 312, O_KFP = 320, O_SIG = 352
};

void c2r_encode(const C2Receipt *r, uint8_t out[C2R_SIZE])
{
    memcpy(out + O_MAGIC, MAGIC, 8);
    put16(out + O_VER, r->version);
    put16(out + O_CLASS, r->signer_class);
    put16(out + O_ALG, r->alg);
    put16(out + O_EV, r->evidence_class);
    memcpy(out + O_REQ, r->requirements, 32);
    memcpy(out + O_PROG, r->program, 32);
    memcpy(out + O_EXE, r->executable, 32);
    memcpy(out + O_DEPS, r->dependencies, 32);
    memcpy(out + O_CHK, r->checker, 32);
    memcpy(out + O_PCHK, r->producer_checker, 32);
    memcpy(out + O_TGT, r->target, 32);
    memcpy(out + O_FW, r->firmware, 32);
    memcpy(out + O_IN, r->input, 32);
    put64(out + O_EPOCH, r->policy_epoch);
    put64(out + O_EXP, r->expiry_epoch);
    memcpy(out + O_KFP, r->key_fp, 32);
    memcpy(out + O_SIG, r->sig, 64);
}

int c2r_decode(const uint8_t *b, size_t len, C2Receipt *r)
{
    if (!b || !r) return C2R_E_ARG;
    if (len != C2R_SIZE) return C2R_E_SIZE;
    if (memcmp(b, MAGIC, 8) != 0) return C2R_E_MAGIC; /* GUARD:magic */
    r->version = get16(b + O_VER);
    r->signer_class = get16(b + O_CLASS);
    r->alg = get16(b + O_ALG);
    r->evidence_class = get16(b + O_EV);
    if (r->version != C2R_FORMAT_VERSION) return C2R_E_VERSION;
    if (r->alg != C2R_ALG_ED25519) return C2R_E_ALG;
    if (r->signer_class != C2R_CLASS_TEST && r->signer_class != C2R_CLASS_OWNER) return C2R_E_CLASS;
    if (r->evidence_class != C2R_EV_IR_CHECKED && r->evidence_class != C2R_EV_IR_CHECKED_LRAT)
        return C2R_E_EVIDENCE;
    memcpy(r->requirements, b + O_REQ, 32);
    memcpy(r->program, b + O_PROG, 32);
    memcpy(r->executable, b + O_EXE, 32);
    memcpy(r->dependencies, b + O_DEPS, 32);
    memcpy(r->checker, b + O_CHK, 32);
    memcpy(r->producer_checker, b + O_PCHK, 32);
    memcpy(r->target, b + O_TGT, 32);
    memcpy(r->firmware, b + O_FW, 32);
    memcpy(r->input, b + O_IN, 32);
    r->policy_epoch = get64(b + O_EPOCH);
    r->expiry_epoch = get64(b + O_EXP);
    if (r->expiry_epoch < r->policy_epoch) return C2R_E_EPOCH_ORDER; /* GUARD:epoch-order */
    memcpy(r->key_fp, b + O_KFP, 32);
    memcpy(r->sig, b + O_SIG, 64);
    return C2R_OK;
}

void c2r_id(const uint8_t buf[C2R_SIZE], uint8_t out[32]) { sha256_hash(buf, C2R_SIZE, out); }

void c2r_signed_message(const uint8_t buf[C2R_SIZE], uint8_t msg[C2R_SIGNED_MSG_LEN])
{
    static const char body_tag[] = "OMEGA-REALIZATION-RECEIPT-V1";
    static const char sig_tag[] = "OMEGA-REALIZATION-RECEIPT-SIG-V1";
    _Static_assert(sizeof sig_tag + 32 == C2R_SIGNED_MSG_LEN, "signed message length");
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)body_tag, sizeof body_tag);
    sha256_update(&c, buf, C2R_SIGNED_LEN);
    memcpy(msg, sig_tag, sizeof sig_tag);
    sha256_final(&c, msg + sizeof sig_tag);
}

static int fields_ok(const C2Receipt *r)
{
    uint8_t tmp[C2R_SIZE];
    C2Receipt chk;
    C2Receipt copy = *r;
    memset(copy.key_fp, 0, 32);
    memset(copy.sig, 0, 64);
    c2r_encode(&copy, tmp);
    return c2r_decode(tmp, C2R_SIZE, &chk);
}

int c2r_sign_test(C2Receipt *r, const uint8_t seed[32], uint8_t out[C2R_SIZE])
{
    if (!r || !seed || !out) return C2R_E_ARG;
    if (r->signer_class != C2R_CLASS_TEST) return C2R_E_KEY_CLASS; /* GUARD:sign-test-class-only */
    uint8_t pk[32];
    if (aienos_ed25519_public_key(pk, seed) != AIENOS_SIG_OK) return C2R_E_ARG;
    /* Only the TEST key may sign TEST class here; any other secret is refused, so a stray key can
     * never mint a receipt that a TEST-anchored verifier would also take. */
    if (memcmp(pk, c2r_test_pk, 32) != 0) return C2R_E_KEY_CLASS; /* GUARD:sign-test-key-only */
    int e = fields_ok(r);
    if (e) return e;
    sha256_hash(pk, 32, r->key_fp);
    memset(r->sig, 0, 64);
    c2r_encode(r, out);
    uint8_t msg[C2R_SIGNED_MSG_LEN];
    c2r_signed_message(out, msg);
    if (aienos_ed25519_sign(r->sig, msg, sizeof msg, seed) != AIENOS_SIG_OK) return C2R_E_ARG;
    memcpy(out + O_SIG, r->sig, 64);
    return C2R_OK;
}

int c2r_attach(C2Receipt *r, const uint8_t pk[32], const uint8_t sig[64], uint8_t out[C2R_SIZE])
{
    if (!r || !pk || !sig || !out) return C2R_E_ARG;
    int e = fields_ok(r);
    if (e) return e;
    sha256_hash(pk, 32, r->key_fp);
    memcpy(r->sig, sig, 64);
    c2r_encode(r, out);
    uint8_t msg[C2R_SIGNED_MSG_LEN];
    c2r_signed_message(out, msg);
    if (aienos_ed25519_verify(sig, msg, sizeof msg, pk) != AIENOS_SIG_OK) return C2R_E_BAD_SIGNATURE;
    return C2R_OK;
}

/* The anchor of the receipt's class whose fingerprint matches, or NULL. */
static const uint8_t *anchor_for(const uint8_t (*set)[32], unsigned n, const uint8_t fp[32])
{
    for (unsigned i = 0; set && i < n; i++) {
        uint8_t h[32];
        sha256_hash(set[i], 32, h);
        if (memcmp(h, fp, 32) == 0) return set[i];
    }
    return NULL;
}

static int in_set(const uint8_t (*set)[32], unsigned n, const uint8_t v[32])
{
    for (unsigned i = 0; set && i < n; i++)
        if (memcmp(set[i], v, 32) == 0) return 1;
    return 0;
}

int c2r_verify(const uint8_t *buf, size_t len, const C2ReceiptPolicy *pol, const C2ReceiptExpect *ex,
               C2Receipt *out)
{
    C2Receipt r;
    if (!pol) return C2R_E_ARG;
    /* 1. structure */
    int e = c2r_decode(buf, len, &r);
    if (e) return e;
    /* 2. class policy: production takes OWNER only, and only Drake's root */
    if (pol->production && r.signer_class != C2R_CLASS_OWNER) return C2R_E_TEST_IN_PRODUCTION; /* GUARD:production-class */
    const uint8_t *key;
    if (r.signer_class == C2R_CLASS_TEST) {
        key = anchor_for(pol->test_anchors, pol->n_test, r.key_fp);
    } else {
        key = anchor_for(pol->owner_anchors, pol->n_owner, r.key_fp);
        /* production: an owner anchor that is not the compiled-in root is not an owner */
        if (key && pol->production && memcmp(key, c2r_owner_root_pk, 32) != 0) key = NULL; /* GUARD:production-root */
    }
    if (!key) return C2R_E_UNTRUSTED_SIGNER; /* GUARD:anchor */
    /* a TEST-class receipt signed by an owner anchor key (or the reverse) is refused: the key
     * must not be an anchor of the other class either */
    int other = r.signer_class == C2R_CLASS_TEST ? anchor_for(pol->owner_anchors, pol->n_owner, r.key_fp) != NULL
                                                 : anchor_for(pol->test_anchors, pol->n_test, r.key_fp) != NULL;
    if (other) return C2R_E_UNTRUSTED_SIGNER; /* GUARD:cross-class */
    /* 3. signature over the domain-separated digest */
    uint8_t msg[C2R_SIGNED_MSG_LEN];
    c2r_signed_message(buf, msg);
    if (aienos_ed25519_verify(r.sig, msg, sizeof msg, key) != AIENOS_SIG_OK) return C2R_E_BAD_SIGNATURE; /* GUARD:signature */
    /* 4. revocation (receipt id, then signer key) */
    uint8_t id[32];
    c2r_id(buf, id);
    if (in_set(pol->revoked_ids, pol->n_revoked_ids, id)) return C2R_E_REVOKED; /* GUARD:revoked-id */
    if (in_set(pol->revoked_keys, pol->n_revoked_keys, r.key_fp)) return C2R_E_REVOKED; /* GUARD:revoked-key */
    /* 5. freshness */
    if (r.policy_epoch < pol->epoch_floor) return C2R_E_STALE; /* GUARD:stale */
    if (pol->current_epoch < r.policy_epoch) return C2R_E_NOT_YET_VALID; /* GUARD:not-yet-valid */
    if (pol->current_epoch > r.expiry_epoch) return C2R_E_EXPIRED; /* GUARD:expired */
    /* 6. binding to what is being admitted */
    if (ex) {
        if (ex->target && memcmp(ex->target, r.target, 32) != 0) return C2R_E_WRONG_TARGET; /* GUARD:target */
        if (ex->firmware && memcmp(ex->firmware, r.firmware, 32) != 0) return C2R_E_WRONG_FIRMWARE; /* GUARD:firmware */
        if (ex->program && memcmp(ex->program, r.program, 32) != 0) return C2R_E_PROGRAM_MISMATCH; /* GUARD:program */
        if (ex->executable && memcmp(ex->executable, r.executable, 32) != 0) return C2R_E_EXECUTABLE_MISMATCH; /* GUARD:executable */
    }
    if (out) *out = r;
    return C2R_OK;
}
