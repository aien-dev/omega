/* test_c2_receipt: signed realization receipts (spec/one-layer-receipts.md).
 *
 * Every refusal case starts from a receipt that this same test shows is accepted under the same
 * policy, changes one thing, and requires the exact named reason. tests/checker2/mutants.sh removes
 * each GUARD line of src/checker2/c2_receipt.c in turn and requires this test to fail (negative
 * control: the test can see each check).
 *
 * Argument 1 (optional): directory holding owner/ceremony-statement and owner/ceremony-signature
 * (default tests/checker2/fixtures). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c2_receipt.h"
#include "sha256.h"
#include "searchtrace/sig/aienos_sig.h"

static int fails, checks;
#define CHECK(cond, ...)                                                                  \
    do {                                                                                  \
        checks++;                                                                         \
        if (!(cond)) {                                                                    \
            fails++;                                                                      \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                          \
            fprintf(stderr, __VA_ARGS__);                                                 \
            fputc('\n', stderr);                                                          \
        }                                                                                 \
    } while (0)
#define EXPECT(got, want, what)                                                           \
    CHECK((got) == (want), "%s: got %s, want %s", what, c2r_reason(got), c2r_reason(want))

static const char *fixdir = "tests/checker2/fixtures";

static size_t slurp(const char *rel, uint8_t *buf, size_t cap)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", fixdir, rel);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

static void fill(uint8_t out[32], const char *label)
{
    sha256_hash((const uint8_t *)label, strlen(label), out);
}

static C2Receipt base(void)
{
    C2Receipt r;
    memset(&r, 0, sizeof r);
    r.version = C2R_FORMAT_VERSION;
    r.signer_class = C2R_CLASS_TEST;
    r.alg = C2R_ALG_ED25519;
    r.evidence_class = C2R_EV_IR_CHECKED;
    fill(r.requirements, "requirements");
    fill(r.program, "program bytes");
    fill(r.executable, "executable bytes");
    c2r_deps_digest(NULL, 0, r.dependencies);
    fill(r.checker, "checker2");
    fill(r.producer_checker, "oscc");
    c2r_target_id("aarch64-linux-host", r.target);
    /* firmware all zero: host */
    fill(r.input, "input");
    r.policy_epoch = 5;
    r.expiry_epoch = 9;
    return r;
}

static uint8_t seed[32];
static uint8_t checker_ok[32]; /* fill(.., "checker2"): the identity base() puts in a receipt */
static const uint8_t (*test_anchor)[32] = &c2r_test_pk;
static const uint8_t (*owner_anchor)[32] = &c2r_owner_root_pk;

static C2ReceiptPolicy qual(void)
{
    C2ReceiptPolicy p;
    memset(&p, 0, sizeof p);
    p.production = 0;
    p.test_anchors = test_anchor; p.n_test = 1;
    p.owner_anchors = owner_anchor; p.n_owner = 1;
    p.checkers = (const uint8_t (*)[32])checker_ok; p.n_checkers = 1;
    p.epoch_floor = 5;
    p.current_epoch = 7;
    return p;
}

static C2ReceiptExpect expect_of(const C2Receipt *r)
{
    C2ReceiptExpect e = {r->target, r->firmware, r->program, r->executable, r->producer_checker, r->dependencies};
    return e;
}

static void test_keys(void)
{
    uint8_t pk[32];
    c2r_test_seed(seed);
    CHECK(aienos_ed25519_public_key(pk, seed) == AIENOS_SIG_OK, "derive test pk");
    CHECK(memcmp(pk, c2r_test_pk, 32) == 0, "c2r_test_pk is the public key of c2r_test_seed");
    CHECK(memcmp(c2r_test_pk, c2r_owner_root_pk, 32) != 0, "test key differs from owner root");

    /* The owner root is Drake's real key: its ceremony signature (made on the YubiKey) verifies
     * under the compiled-in constant, and a one-byte change of the statement does not. */
    uint8_t msg[4096], sig[64];
    size_t n = slurp("owner/ceremony-statement", msg, sizeof msg);
    CHECK(slurp("owner/ceremony-signature", sig, sizeof sig) == 64, "ceremony signature is 64 bytes");
    CHECK(aienos_ed25519_verify(sig, msg, n, c2r_owner_root_pk) == AIENOS_SIG_OK,
          "real owner ceremony signature verifies under c2r_owner_root_pk");
    msg[n - 1] ^= 1;
    CHECK(aienos_ed25519_verify(sig, msg, n, c2r_owner_root_pk) != AIENOS_SIG_OK,
          "control: altered ceremony statement is refused");
    msg[n - 1] ^= 1;
    CHECK(aienos_ed25519_verify(sig, msg, n, c2r_test_pk) != AIENOS_SIG_OK,
          "control: ceremony signature does not verify under the TEST key");
}

static void test_valid_and_deterministic(void)
{
    C2Receipt r = base(), r2 = base(), got;
    uint8_t a[C2R_SIZE], b[C2R_SIZE], ida[32], idb[32];
    EXPECT(c2r_sign_test(&r, seed, a), C2R_OK, "sign");
    EXPECT(c2r_sign_test(&r2, seed, b), C2R_OK, "sign again");
    CHECK(memcmp(a, b, C2R_SIZE) == 0, "identical inputs give identical receipt bytes");
    c2r_id(a, ida);
    c2r_id(b, idb);
    CHECK(memcmp(ida, idb, 32) == 0, "identical ids");
    C2ReceiptPolicy p = qual();
    C2ReceiptExpect e = expect_of(&r);
    EXPECT(c2r_verify(a, C2R_SIZE, &p, &e, &got), C2R_OK, "valid TEST receipt, qualification policy");
    CHECK(memcmp(got.program, r.program, 32) == 0 && got.policy_epoch == 5 && got.expiry_epoch == 9,
          "decoded fields round-trip");
    /* epoch window edges are inclusive */
    p.current_epoch = 5;
    EXPECT(c2r_verify(a, C2R_SIZE, &p, &e, NULL), C2R_OK, "current == policy epoch");
    p.current_epoch = 9;
    EXPECT(c2r_verify(a, C2R_SIZE, &p, &e, NULL), C2R_OK, "current == expiry epoch");
}

/* Altered: every single-bit change anywhere in the 416 bytes is refused. */
static void test_altered(void)
{
    C2Receipt r = base();
    uint8_t a[C2R_SIZE];
    c2r_sign_test(&r, seed, a);
    C2ReceiptPolicy p = qual();
    C2ReceiptExpect e = expect_of(&r);
    unsigned accepted = 0;
    for (unsigned i = 0; i < C2R_SIZE * 8; i++) {
        uint8_t m[C2R_SIZE];
        memcpy(m, a, C2R_SIZE);
        m[i / 8] ^= (uint8_t)(1u << (i % 8));
        if (c2r_verify(m, C2R_SIZE, &p, &e, NULL) == C2R_OK) accepted++;
    }
    CHECK(accepted == 0, "every one of %u single-bit alterations refused (accepted %u)", C2R_SIZE * 8, accepted);

    /* named reasons for the structural fields */
    uint8_t m[C2R_SIZE];
    memcpy(m, a, C2R_SIZE); m[0] ^= 1;
    EXPECT(c2r_verify(m, C2R_SIZE, &p, &e, NULL), C2R_E_MAGIC, "altered magic");
    memcpy(m, a, C2R_SIZE); m[8] = 2;
    EXPECT(c2r_verify(m, C2R_SIZE, &p, &e, NULL), C2R_E_VERSION, "altered version");
    memcpy(m, a, C2R_SIZE); m[10] = 3;
    EXPECT(c2r_verify(m, C2R_SIZE, &p, &e, NULL), C2R_E_CLASS, "altered class");
    memcpy(m, a, C2R_SIZE); m[12] = 2;
    EXPECT(c2r_verify(m, C2R_SIZE, &p, &e, NULL), C2R_E_ALG, "altered algorithm");
    memcpy(m, a, C2R_SIZE); m[14] = 9;
    EXPECT(c2r_verify(m, C2R_SIZE, &p, &e, NULL), C2R_E_EVIDENCE, "altered evidence class");
    memcpy(m, a, C2R_SIZE); m[48] ^= 0x80;
    EXPECT(c2r_verify(m, C2R_SIZE, &p, NULL, NULL), C2R_E_BAD_SIGNATURE, "altered program hash");
    memcpy(m, a, C2R_SIZE); m[400] ^= 0x01;
    EXPECT(c2r_verify(m, C2R_SIZE, &p, &e, NULL), C2R_E_BAD_SIGNATURE, "altered signature");
    EXPECT(c2r_verify(a, C2R_SIZE - 1, &p, &e, NULL), C2R_E_SIZE, "truncated");
    uint8_t big[C2R_SIZE + 1];
    memcpy(big, a, C2R_SIZE); big[C2R_SIZE] = 0;
    EXPECT(c2r_verify(big, sizeof big, &p, &e, NULL), C2R_E_SIZE, "trailing byte");

    /* epoch order: an expiry before the issue epoch is refused at decode and at signing */
    C2Receipt bad = base();
    bad.expiry_epoch = 4;
    EXPECT(c2r_sign_test(&bad, seed, m), C2R_E_EPOCH_ORDER, "sign refuses expiry < policy epoch");
}

static void test_wrong_key(void)
{
    C2Receipt r = base();
    uint8_t a[C2R_SIZE], other_seed[32], other_pk[32];
    fill(other_seed, "some other key");
    aienos_ed25519_public_key(other_pk, other_seed);
    EXPECT(c2r_sign_test(&r, other_seed, a), C2R_E_KEY_CLASS, "signing refuses a key that is not the TEST key");

    /* a receipt signed by an unknown key (attached signature) is refused: not an anchor */
    C2Receipt r2 = base();
    uint8_t tbs[C2R_SIZE], msg[C2R_SIGNED_MSG_LEN], sig[64];
    sha256_hash(other_pk, 32, r2.key_fp);
    c2r_encode(&r2, tbs);
    c2r_signed_message(tbs, msg);
    aienos_ed25519_sign(sig, msg, sizeof msg, other_seed);
    EXPECT(c2r_attach(&r2, other_pk, sig, a), C2R_OK, "attach a valid foreign signature");
    C2ReceiptPolicy p = qual();
    C2ReceiptExpect e = expect_of(&r2);
    EXPECT(c2r_verify(a, C2R_SIZE, &p, &e, NULL), C2R_E_UNTRUSTED_SIGNER, "wrong key");

    /* the right key fingerprint with a signature made by another key */
    C2Receipt r3 = base();
    sha256_hash(c2r_test_pk, 32, r3.key_fp);
    c2r_encode(&r3, tbs);
    c2r_signed_message(tbs, msg);
    aienos_ed25519_sign(sig, msg, sizeof msg, other_seed);
    EXPECT(c2r_attach(&r3, c2r_test_pk, sig, a), C2R_E_BAD_SIGNATURE, "attach refuses a signature by the wrong key");
    memcpy(a + 352, sig, 64);
    EXPECT(c2r_verify(a, C2R_SIZE, &p, &e, NULL), C2R_E_BAD_SIGNATURE, "TEST fingerprint, foreign signature");

    /* swap two equally valid receipts' signatures: refused */
    C2Receipt x = base(), y = base();
    y.policy_epoch = 6;
    uint8_t bx[C2R_SIZE], by[C2R_SIZE];
    c2r_sign_test(&x, seed, bx);
    c2r_sign_test(&y, seed, by);
    memcpy(bx + 352, by + 352, 64);
    EXPECT(c2r_verify(bx, C2R_SIZE, &p, &e, NULL), C2R_E_BAD_SIGNATURE, "signature of another receipt");
}

static void test_production(void)
{
    C2Receipt r = base();
    uint8_t a[C2R_SIZE];
    c2r_sign_test(&r, seed, a);
    C2ReceiptPolicy p = qual();
    C2ReceiptExpect e = expect_of(&r);
    EXPECT(c2r_verify(a, C2R_SIZE, &p, &e, NULL), C2R_OK, "control: accepted in qualification");
    p.production = 1;
    EXPECT(c2r_verify(a, C2R_SIZE, &p, &e, NULL), C2R_E_TEST_IN_PRODUCTION, "TEST receipt in production");

    /* the TEST key relabelled OWNER class: production refuses it even if a careless policy lists
     * the TEST key as an owner anchor */
    C2Receipt o = base();
    o.signer_class = C2R_CLASS_OWNER;
    EXPECT(c2r_sign_test(&o, seed, a), C2R_E_KEY_CLASS, "TEST key cannot sign OWNER class");
    uint8_t tbs[C2R_SIZE], msg[C2R_SIGNED_MSG_LEN], sig[64];
    sha256_hash(c2r_test_pk, 32, o.key_fp);
    c2r_encode(&o, tbs);
    c2r_signed_message(tbs, msg);
    aienos_ed25519_sign(sig, msg, sizeof msg, seed);
    EXPECT(c2r_attach(&o, c2r_test_pk, sig, a), C2R_OK, "attach (OWNER class, TEST key)");
    const uint8_t sloppy[2][32] = {{0}, {0}};
    uint8_t (*sl)[32] = (uint8_t (*)[32])sloppy;
    memcpy(sl[0], c2r_owner_root_pk, 32);
    memcpy(sl[1], c2r_test_pk, 32);
    C2ReceiptPolicy q = qual();
    q.production = 1;
    q.owner_anchors = (const uint8_t (*)[32])sl;
    q.n_owner = 2;
    q.test_anchors = NULL;
    q.n_test = 0;
    EXPECT(c2r_verify(a, C2R_SIZE, &q, &e, NULL), C2R_E_UNTRUSTED_SIGNER,
           "production pins the owner root: TEST key listed as owner anchor is refused");
    /* control for the pin: the same sloppy policy outside production takes it */
    q.production = 0;
    EXPECT(c2r_verify(a, C2R_SIZE, &q, &e, NULL), C2R_OK, "control: non-production takes listed owner anchor");
    /* cross-class: the same key may not be anchored in both classes */
    q.test_anchors = test_anchor;
    q.n_test = 1;
    EXPECT(c2r_verify(a, C2R_SIZE, &q, &e, NULL), C2R_E_UNTRUSTED_SIGNER, "key anchored in both classes");

    /* an OWNER-class receipt claiming the owner fingerprint but signed by the TEST key */
    C2Receipt f = base();
    f.signer_class = C2R_CLASS_OWNER;
    sha256_hash(c2r_owner_root_pk, 32, f.key_fp);
    c2r_encode(&f, tbs);
    c2r_signed_message(tbs, msg);
    aienos_ed25519_sign(sig, msg, sizeof msg, seed);
    memcpy(tbs + 352, sig, 64);
    EXPECT(c2r_attach(&f, c2r_owner_root_pk, sig, a), C2R_E_BAD_SIGNATURE, "attach refuses forged owner signature");
    p = qual();
    p.production = 1;
    EXPECT(c2r_verify(tbs, C2R_SIZE, &p, &e, NULL), C2R_E_BAD_SIGNATURE, "forged owner receipt in production");
}

static void test_revoked_replayed_fresh(void)
{
    C2Receipt r = base();
    uint8_t a[C2R_SIZE], id[32], fp[32], other[32];
    c2r_sign_test(&r, seed, a);
    c2r_id(a, id);
    sha256_hash(c2r_test_pk, 32, fp);
    fill(other, "unrelated id");
    C2ReceiptExpect e = expect_of(&r);
    C2ReceiptPolicy p = qual();
    const uint8_t (*ids)[32] = (const uint8_t (*)[32])id;
    const uint8_t (*oth)[32] = (const uint8_t (*)[32])other;
    const uint8_t (*fps)[32] = (const uint8_t (*)[32])fp;
    p.revoked_ids = oth; p.n_revoked_ids = 1;
    EXPECT(c2r_verify(a, C2R_SIZE, &p, &e, NULL), C2R_OK, "control: unrelated revocation");
    p.revoked_ids = ids;
    EXPECT(c2r_verify(a, C2R_SIZE, &p, &e, NULL), C2R_E_REVOKED, "revoked receipt id");
    p = qual();
    p.revoked_keys = fps; p.n_revoked_keys = 1;
    EXPECT(c2r_verify(a, C2R_SIZE, &p, &e, NULL), C2R_E_REVOKED, "revoked signer key");

    /* replay of a superseded receipt: policy moved its floor past the receipt's epoch */
    p = qual();
    p.epoch_floor = 6;
    EXPECT(c2r_verify(a, C2R_SIZE, &p, &e, NULL), C2R_E_STALE, "replayed after epoch floor moved");
    p = qual();
    p.current_epoch = 10;
    EXPECT(c2r_verify(a, C2R_SIZE, &p, &e, NULL), C2R_E_EXPIRED, "expired");
    p = qual();
    p.epoch_floor = 0;
    p.current_epoch = 4;
    EXPECT(c2r_verify(a, C2R_SIZE, &p, &e, NULL), C2R_E_NOT_YET_VALID, "not yet valid");
}

static void test_binding(void)
{
    C2Receipt r = base();
    uint8_t a[C2R_SIZE], x[32];
    c2r_sign_test(&r, seed, a);
    C2ReceiptPolicy p = qual();
    C2ReceiptExpect e = expect_of(&r);
    EXPECT(c2r_verify(a, C2R_SIZE, &p, &e, NULL), C2R_OK, "control: all bindings match");

    c2r_target_id("gb10-aienos", x);
    e = expect_of(&r); e.target = x;
    EXPECT(c2r_verify(a, C2R_SIZE, &p, &e, NULL), C2R_E_WRONG_TARGET, "wrong target");
    fill(x, "firmware v2");
    e = expect_of(&r); e.firmware = x;
    EXPECT(c2r_verify(a, C2R_SIZE, &p, &e, NULL), C2R_E_WRONG_FIRMWARE, "wrong firmware");
    fill(x, "other program");
    e = expect_of(&r); e.program = x;
    EXPECT(c2r_verify(a, C2R_SIZE, &p, &e, NULL), C2R_E_PROGRAM_MISMATCH, "program mismatch");
    fill(x, "other executable");
    e = expect_of(&r); e.executable = x;
    EXPECT(c2r_verify(a, C2R_SIZE, &p, &e, NULL), C2R_E_EXECUTABLE_MISMATCH, "executable mismatch");
}

/* Checker identity is a policy anchor list: a receipt judged by any other checker build is refused,
 * an empty list refuses everything (fail closed). Producer and dependency digests are bound like
 * the other fields. */
static void test_checker_identity(void)
{
    C2Receipt r = base(), o = base();
    uint8_t a[C2R_SIZE], b[C2R_SIZE], x[32], two[2][32];
    fill(o.checker, "checker2 built from other sources");
    c2r_sign_test(&r, seed, a);
    c2r_sign_test(&o, seed, b);
    C2ReceiptPolicy p = qual();
    C2ReceiptExpect e = expect_of(&r);
    EXPECT(c2r_verify(a, C2R_SIZE, &p, &e, NULL), C2R_OK, "control: receipt from the anchored checker");
    EXPECT(c2r_verify(b, C2R_SIZE, &p, &e, NULL), C2R_E_UNTRUSTED_CHECKER, "receipt from a different checker identity");
    EXPECT(c2r_verify(b, C2R_SIZE, &p, NULL, NULL), C2R_E_UNTRUSTED_CHECKER, "checker identity checked without expectations");
    memcpy(two[0], checker_ok, 32);
    memcpy(two[1], o.checker, 32);
    p.checkers = (const uint8_t (*)[32])two;
    p.n_checkers = 2;
    EXPECT(c2r_verify(b, C2R_SIZE, &p, &e, NULL), C2R_OK, "control: second identity anchored");
    p.n_checkers = 0;
    EXPECT(c2r_verify(a, C2R_SIZE, &p, &e, NULL), C2R_E_UNTRUSTED_CHECKER, "empty checker list refuses everything");
    p.checkers = NULL;
    p.n_checkers = 1;
    EXPECT(c2r_verify(a, C2R_SIZE, &p, &e, NULL), C2R_E_UNTRUSTED_CHECKER, "NULL checker list refuses everything");

    p = qual();
    fill(x, "another oscc binary");
    e = expect_of(&r); e.producer_checker = x;
    EXPECT(c2r_verify(a, C2R_SIZE, &p, &e, NULL), C2R_E_WRONG_PRODUCER, "wrong producer checker");
    uint8_t ids[1][32];
    fill(ids[0], "some dependency receipt");
    c2r_deps_digest((const uint8_t (*)[32])ids, 1, x);
    e = expect_of(&r); e.dependencies = x;
    EXPECT(c2r_verify(a, C2R_SIZE, &p, &e, NULL), C2R_E_DEPS_MISMATCH, "dependency digest mismatch");
}

static void test_deps(void)
{
    uint8_t ids[3][32], d1[32], d2[32];
    fill(ids[0], "a"); fill(ids[1], "b"); fill(ids[2], "c");
    /* sort ascending */
    for (int i = 0; i < 3; i++)
        for (int j = i + 1; j < 3; j++)
            if (memcmp(ids[i], ids[j], 32) > 0) {
                uint8_t t[32];
                memcpy(t, ids[i], 32); memcpy(ids[i], ids[j], 32); memcpy(ids[j], t, 32);
            }
    CHECK(c2r_deps_digest((const uint8_t (*)[32])ids, 3, d1) == 0, "sorted deps accepted");
    CHECK(c2r_deps_digest((const uint8_t (*)[32])ids, 2, d2) == 0 && memcmp(d1, d2, 32) != 0,
          "dependency count is bound");
    uint8_t swapped[2][32];
    memcpy(swapped[0], ids[1], 32); memcpy(swapped[1], ids[0], 32);
    CHECK(c2r_deps_digest((const uint8_t (*)[32])swapped, 2, d2) == -1, "unsorted deps refused");
    memcpy(swapped[1], ids[1], 32);
    CHECK(c2r_deps_digest((const uint8_t (*)[32])swapped, 2, d2) == -1, "duplicate deps refused");
}

int main(int argc, char **argv)
{
    if (argc > 1) fixdir = argv[1];
    fill(checker_ok, "checker2");
    test_keys();
    test_valid_and_deterministic();
    test_altered();
    test_wrong_key();
    test_production();
    test_revoked_replayed_fresh();
    test_binding();
    test_checker_identity();
    test_deps();
    printf("test_c2_receipt: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
