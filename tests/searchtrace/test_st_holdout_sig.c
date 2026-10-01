/* test_st_holdout_sig.c - owner signature over the G3 commitment record.
 * Built by `make test-searchtrace` (plain and ASan/UBSan) with
 * src/searchtrace/st_holdout.c, src/sha256.c and the vendored
 * src/searchtrace/sig/{ed25519,sha512,ct}.c.
 *
 * Keys here are TEST keys only: two fixed seeds derived from labelled
 * strings ("OMEGA TEST-ONLY G3 signing key N (not an owner key)"). No owner
 * key material exists in this repository; the real record is signed in the
 * offline owner key ceremony with `searchtrace holdout sign`. */
#include "searchtrace/st_holdout.h"
#include "searchtrace/sig/aienos_sig.h"
#include "sha256.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_run, g_pass;

static void check(const char *name, int ok)
{
    g_run++;
    if (ok)
        g_pass++;
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}

/* Same known-answer record as test_st_holdout.c (spec section
 * "Known-answer vector"); signing must not change it. */
#define KAT_TASKSET "task alpha\ntask beta\n"
#define KAT_CM "7008574e68b49d88b0debe5101eec7823a0f11a99833cb5e6aedc9262822b20d"
#define KAT_RD "20ba763127e9ffd0b563365d1db13f7d3b813f39c9d3b215c1447a48e9f39a28"

/* RFC 8032 section 7.1, TEST 1 and TEST 2 (copied from aienos
 * native/sig/tests/rfc8032_vectors.h, itself verbatim from the RFC). */
#define RFC1_SK "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"
#define RFC1_PK "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"
#define RFC1_SIG "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"
#define RFC2_SK "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb"
#define RFC2_PK "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c"
#define RFC2_SIG "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"

static void unhex(const char *h, uint8_t *o, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(h + 2 * i, "%2x", &v);
        o[i] = (uint8_t)v;
    }
}

static void hexs(const uint8_t *d, size_t n, char *o)
{
    static const char x[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        o[2 * i] = x[d[i] >> 4];
        o[2 * i + 1] = x[d[i] & 15];
    }
    o[2 * n] = '\0';
}

static void test_key(int which, uint8_t sk[32], uint8_t pk[32])
{
    char label[80];
    snprintf(label, sizeof label,
             "OMEGA TEST-ONLY G3 signing key %d (not an owner key)", which);
    sha256_hash((const uint8_t *)label, strlen(label), sk);
    aienos_ed25519_public_key(pk, sk);
}

/* Recompute the trailing "end <hex>\n" line of a record in place. */
static void reseal(char *b, size_t len)
{
    uint8_t d[32];
    char h[65];
    size_t body = len - 69;
    sha256_hash((const uint8_t *)b, body, d);
    hexs(d, 32, h);
    memcpy(b + body + 4, h, 64);
}

/* Replace the value of field `key` (first line starting "key ") with
 * `val`, then reseal. Returns new length. */
static size_t set_field(char *b, size_t len, size_t cap, const char *key,
                        const char *val)
{
    char tmp[2048];
    size_t kl = strlen(key);
    char *p = b;
    while (p < b + len) {
        char *nl = memchr(p, '\n', (size_t)(b + len - p));
        if ((size_t)(nl - p) > kl && !memcmp(p, key, kl) && p[kl] == ' ') {
            size_t pre = (size_t)(p - b) + kl + 1;
            size_t post = len - (size_t)(nl - b);
            size_t vl = strlen(val);
            if (pre + vl + post > cap)
                return 0;
            memcpy(tmp, b, pre);
            memcpy(tmp + pre, val, vl);
            memcpy(tmp + pre + vl, nl, post);
            memcpy(b, tmp, pre + vl + post);
            reseal(b, pre + vl + post);
            return pre + vl + post;
        }
        p = nl + 1;
    }
    return 0;
}

static int write_file(const char *path, const void *d, size_t n, int mode)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    int ok = fwrite(d, 1, n, f) == n;
    ok = (fclose(f) == 0) && ok;
    return (ok && chmod(path, (mode_t)mode) == 0) ? 0 : -1;
}

static size_t read_all(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t got = fread(buf, 1, cap, f);
    fclose(f);
    return got;
}

static int cli(int argc, ...)
{
    char *argv[8];
    va_list ap;
    va_start(ap, argc);
    for (int i = 0; i < argc; i++)
        argv[i] = va_arg(ap, char *);
    va_end(ap);
    argv[argc] = NULL;
    fflush(stdout);
    return st_holdout_cli(argc, argv);
}

int main(void)
{
    uint8_t salt[32], sk1[32], pk1[32], sk2[32], pk2[32];
    char rec[1024], rec2[1024], sig[1024], sig2[1024], m[2048], why[128];
    size_t rl = 0, rl2 = 0, sl = 0, sl2 = 0;
    StHoldoutCommitment c;
    char h[129];
    int r;

    for (int i = 0; i < 32; i++)
        salt[i] = (uint8_t)i;
    test_key(1, sk1, pk1);
    test_key(2, sk2, pk2);

    /* --- vendored Ed25519 still matches RFC 8032 --- */
    {
        uint8_t sk[32], pk[32], want_pk[32], s[64], want_s[64], msg[1] = {0x72};
        unhex(RFC1_SK, sk, 32);
        unhex(RFC1_PK, want_pk, 32);
        unhex(RFC1_SIG, want_s, 64);
        aienos_ed25519_public_key(pk, sk);
        aienos_ed25519_sign(s, msg, 0, sk);
        check("vendored ed25519 RFC 8032 TEST 1 (pk, sig, verify)",
              !memcmp(pk, want_pk, 32) && !memcmp(s, want_s, 64) &&
                  aienos_ed25519_verify(s, msg, 0, pk) == AIENOS_SIG_OK);
        unhex(RFC2_SK, sk, 32);
        unhex(RFC2_PK, want_pk, 32);
        unhex(RFC2_SIG, want_s, 64);
        aienos_ed25519_public_key(pk, sk);
        aienos_ed25519_sign(s, msg, 1, sk);
        check("vendored ed25519 RFC 8032 TEST 2 (pk, sig, verify)",
              !memcmp(pk, want_pk, 32) && !memcmp(s, want_s, 64) &&
                  aienos_ed25519_verify(s, msg, 1, pk) == AIENOS_SIG_OK);
    }

    /* --- the unsigned record is unchanged --- */
    r = st_holdout_commit("g3-kat-1", (const uint8_t *)KAT_TASKSET,
                          sizeof(KAT_TASKSET) - 1, salt, rec, sizeof rec, &rl);
    st_holdout_parse(rec, rl, &c, why, sizeof why);
    hexs(c.record_digest, 32, h);
    check("KAT record digest unchanged (20ba7631...)",
          r == 0 && !strcmp(h, KAT_RD));
    hexs(c.commitment, 32, h);
    check("KAT commitment unchanged (7008574e...)", !strcmp(h, KAT_CM));

    /* --- sign and verify --- */
    r = st_holdout_sign(rec, rl, sk1, sig, sizeof sig, &sl, why, sizeof why);
    check("sign KAT record with TEST key 1", r == 0 && sl > 0);
    memset(&c, 0, sizeof c);
    r = st_holdout_verify_signed(rec, rl, sig, sl, pk1, &c, why, sizeof why);
    hexs(c.record_digest, 32, h);
    check("strict verify accepts it and fills the record",
          r == 0 && !strcmp(h, KAT_RD) && !strcmp(c.holdout_id, "g3-kat-1") &&
              c.task_count == 2);
    r = st_holdout_sign(rec, rl, sk1, sig2, sizeof sig2, &sl2, why, sizeof why);
    check("signing is deterministic (same bytes twice)",
          r == 0 && sl2 == sl && !memcmp(sig, sig2, sl));
    {
        char want[1024];
        uint8_t kid[32], s[64], msg[1100];
        char kh[65], sh[129];
        sha256_hash(pk1, 32, kid);
        hexs(kid, 32, kh);
        memcpy(msg, ST_HOLDOUT_SIG_DOMAIN, sizeof(ST_HOLDOUT_SIG_DOMAIN));
        memcpy(msg + sizeof(ST_HOLDOUT_SIG_DOMAIN), rec, rl);
        aienos_ed25519_sign(s, msg, sizeof(ST_HOLDOUT_SIG_DOMAIN) + rl, sk1);
        hexs(s, 64, sh);
        int n = snprintf(want, sizeof want,
                         "OMEGA-G3-HOLDOUT-SIG v1\n"
                         "domain omega.g3.holdout.sig.v1\n"
                         "holdout_id g3-kat-1\n"
                         "record_digest " KAT_RD "\n"
                         "key_id %s\nsignature %s\nend %064d\n",
                         kh, sh, 0);
        reseal(want, (size_t)n);
        check("signature record layout is the documented one",
              (size_t)n == sl && !memcmp(want, sig, sl));
    }
    r = st_holdout_sign(rec, rl, sk1, sig2, sl - 1, &sl2, why, sizeof why);
    check("sign refuses a small output buffer", r == ST_HOLDOUT_ECAP);

    /* --- refusals --- */
    r = st_holdout_verify_signed(rec, rl, sig, sl, pk2, &c, why, sizeof why);
    check("wrong public key refused (EKEY)", r == ST_HOLDOUT_EKEY);
    r = st_holdout_verify_signed(rec, rl, NULL, 0, pk1, &c, why, sizeof why);
    check("unsigned record refused in strict mode (NULL)",
          r == ST_HOLDOUT_EUNSIGNED);
    r = st_holdout_verify_signed(rec, rl, sig, 0, pk1, &c, why, sizeof why);
    check("unsigned record refused in strict mode (empty)",
          r == ST_HOLDOUT_EUNSIGNED);
    {
        /* key 2 signs, then the key_id is relabelled as key 1. */
        uint8_t kid[32];
        char kh[65];
        st_holdout_sign(rec, rl, sk2, sig2, sizeof sig2, &sl2, why, sizeof why);
        sha256_hash(pk1, 32, kid);
        hexs(kid, 32, kh);
        sl2 = set_field(sig2, sl2, sizeof sig2, "key_id", kh);
        r = st_holdout_verify_signed(rec, rl, sig2, sl2, pk1, &c, why,
                                     sizeof why);
        check("key 2 signature relabelled as key 1 refused (ESIG)",
              sl2 && r == ST_HOLDOUT_ESIG);
    }
    {
        /* small-order public key (identity point) named as the signer */
        uint8_t id_pk[32] = {1}, kid[32];
        char kh[65];
        memcpy(sig2, sig, sl);
        sha256_hash(id_pk, 32, kid);
        hexs(kid, 32, kh);
        sl2 = set_field(sig2, sl, sizeof sig2, "key_id", kh);
        r = st_holdout_verify_signed(rec, rl, sig2, sl2, id_pk, &c, why,
                                     sizeof why);
        check("small-order public key refused (ESIG)",
              sl2 && r == ST_HOLDOUT_ESIG);
    }
    {
        char zero[129];
        memset(zero, '0', 128);
        zero[128] = '\0';
        memcpy(sig2, sig, sl);
        sl2 = set_field(sig2, sl, sizeof sig2, "signature", zero);
        r = st_holdout_verify_signed(rec, rl, sig2, sl2, pk1, &c, why,
                                     sizeof why);
        check("all-zero signature refused (ESIG)", r == ST_HOLDOUT_ESIG);
    }
    {
        /* a different, validly signed record must not borrow this signature */
        r = st_holdout_commit("g3-kat-2", (const uint8_t *)KAT_TASKSET,
                              sizeof(KAT_TASKSET) - 1, salt, rec2, sizeof rec2,
                              &rl2);
        int r1 = st_holdout_verify_signed(rec2, rl2, sig, sl, pk1, &c, why,
                                          sizeof why);
        memcpy(sig2, sig, sl);
        sl2 = set_field(sig2, sl, sizeof sig2, "holdout_id", "g3-kat-2");
        int r2 = st_holdout_verify_signed(rec2, rl2, sig2, sl2, pk1, &c, why,
                                          sizeof why);
        uint8_t d[32];
        sha256_hash((const uint8_t *)rec2, rl2, d);
        hexs(d, 32, h);
        sl2 = set_field(sig2, sl2, sizeof sig2, "record_digest", h);
        int r3 = st_holdout_verify_signed(rec2, rl2, sig2, sl2, pk1, &c, why,
                                          sizeof why);
        check("signature of record A refused for record B (EBIND x2, ESIG)",
              r == 0 && r1 == ST_HOLDOUT_EBIND && r2 == ST_HOLDOUT_EBIND &&
                  r3 == ST_HOLDOUT_ESIG);
    }
    {
        /* commitment record edited and resealed (end digest recomputed) */
        memcpy(rec2, rec, rl);
        rl2 = set_field(rec2, rl, sizeof rec2, "task_count", "3");
        int ok = st_holdout_parse(rec2, rl2, &c, why, sizeof why) == 0;
        r = st_holdout_verify_signed(rec2, rl2, sig, sl, pk1, &c, why,
                                     sizeof why);
        check("resealed tampered record refused (EBIND)",
              ok && r == ST_HOLDOUT_EBIND);
    }
    {
        int all = 1, n = 0;
        for (size_t i = 0; i < rl; i++)
            for (int b = 0; b < 2; b++) {
                memcpy(rec2, rec, rl);
                rec2[i] = (char)(rec2[i] ^ (b ? 0x80 : 0x01));
                if (st_holdout_verify_signed(rec2, rl, sig, sl, pk1, &c, why,
                                             sizeof why) == 0)
                    all = 0;
                n++;
            }
        check("every commitment-record byte flip refused", all && n > 0);
        all = 1;
        for (size_t i = 0; i < sl; i++)
            for (int b = 0; b < 2; b++) {
                memcpy(sig2, sig, sl);
                sig2[i] = (char)(sig2[i] ^ (b ? 0x80 : 0x01));
                if (st_holdout_verify_signed(rec, rl, sig2, sl, pk1, &c, why,
                                             sizeof why) == 0)
                    all = 0;
            }
        check("every signature-record byte flip refused", all);
    }
    {
        /* signature-only edits that keep the record canonical: each 128-hex
         * digit changed to another valid lowercase hex digit, resealed */
        int all = 1;
        for (int i = 0; i < 128; i++) {
            char sh[129];
            const char *p = strstr(sig, "\nsignature ") + 11;
            memcpy(sh, p, 128);
            sh[128] = '\0';
            sh[i] = sh[i] == '0' ? '1' : '0';
            memcpy(sig2, sig, sl);
            sl2 = set_field(sig2, sl, sizeof sig2, "signature", sh);
            r = st_holdout_verify_signed(rec, rl, sig2, sl2, pk1, &c, why,
                                         sizeof why);
            if (r != ST_HOLDOUT_ESIG)
                all = 0;
        }
        check("each resealed signature-hex change refused (ESIG)", all);
    }
    {
        int all = 1;
        for (size_t L = 1; L < sl; L++) {
            char *t = malloc(L);
            memcpy(t, sig, L);
            if (st_holdout_verify_signed(rec, rl, t, L, pk1, &c, why,
                                         sizeof why) != ST_HOLDOUT_EFORMAT)
                all = 0;
            free(t);
        }
        check("every truncated signature record refused (EFORMAT)", all);
        memcpy(sig2, sig, sl);
        sig2[sl] = '\n';
        r = st_holdout_verify_signed(rec, rl, sig2, sl + 1, pk1, &c, why,
                                     sizeof why);
        check("extended signature record refused", r == ST_HOLDOUT_EFORMAT);
    }
    {
        struct {
            const char *key, *val, *name;
        } bad[] = {
            {"domain", "omega.g3.holdout.commit.v1", "wrong domain"},
            {"holdout_id", "bad id!", "bad label"},
            {"key_id", "ABCDEF", "short key_id"},
            {"signature", "00", "short signature"},
        };
        int all = 1;
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            memcpy(sig2, sig, sl);
            sl2 = set_field(sig2, sl, sizeof sig2, bad[i].key, bad[i].val);
            if (!sl2 || st_holdout_verify_signed(rec, rl, sig2, sl2, pk1, &c,
                                                 why, sizeof why) !=
                            ST_HOLDOUT_EFORMAT)
                all = 0;
        }
        /* uppercase hex in the signature */
        const char *p = strstr(sig, "\nsignature ") + 11;
        char sh[129];
        memcpy(sh, p, 128);
        sh[128] = '\0';
        for (int i = 0; i < 128; i++)
            if (sh[i] >= 'a' && sh[i] <= 'f') {
                sh[i] = (char)(sh[i] - 'a' + 'A');
                break;
            }
        memcpy(sig2, sig, sl);
        sl2 = set_field(sig2, sl, sizeof sig2, "signature", sh);
        if (st_holdout_verify_signed(rec, rl, sig2, sl2, pk1, &c, why,
                                     sizeof why) != ST_HOLDOUT_EFORMAT)
            all = 0;
        /* wrong magic, CR, reordered fields */
        memcpy(sig2, sig, sl);
        sig2[21] = '2';
        if (st_holdout_verify_signed(rec, rl, sig2, sl, pk1, &c, why,
                                     sizeof why) != ST_HOLDOUT_EFORMAT)
            all = 0;
        memcpy(sig2, sig, sl);
        sig2[23] = '\r';
        if (st_holdout_verify_signed(rec, rl, sig2, sl, pk1, &c, why,
                                     sizeof why) != ST_HOLDOUT_EFORMAT)
            all = 0;
        {
            /* swap the record_digest and key_id lines */
            char *a = strstr(sig2, "record_digest ");
            memcpy(sig2, sig, sl);
            a = strstr(sig2, "record_digest ");
            char *k = strstr(sig2, "key_id ");
            char la[128], lk[128];
            size_t na = (size_t)(k - a), nk = (size_t)(strchr(k, '\n') + 1 - k);
            memcpy(la, a, na);
            memcpy(lk, k, nk);
            memcpy(a, lk, nk);
            memcpy(a + nk, la, na);
            reseal(sig2, sl);
            if (st_holdout_verify_signed(rec, rl, sig2, sl, pk1, &c, why,
                                         sizeof why) != ST_HOLDOUT_EFORMAT)
                all = 0;
        }
        check("non-canonical signature records refused (EFORMAT)", all);
        memcpy(sig2, sig, sl);
        sig2[sl - 2] = sig2[sl - 2] == '0' ? '1' : '0';
        r = st_holdout_verify_signed(rec, rl, sig2, sl, pk1, &c, why,
                                     sizeof why);
        check("signature record end digest mismatch refused (EDIGEST)",
              r == ST_HOLDOUT_EDIGEST);
    }
    {
        memcpy(rec2, rec, rl);
        rec2[rl] = 'x';
        r = st_holdout_sign(rec2, rl + 1, sk1, sig2, sizeof sig2, &sl2, why,
                            sizeof why);
        int r2 = st_holdout_verify_signed(rec2, rl + 1, sig, sl, pk1, &c, why,
                                          sizeof why);
        check("non-canonical commitment record: sign and verify refuse",
              r == ST_HOLDOUT_EFORMAT && r2 == ST_HOLDOUT_EFORMAT);
        check("NULL arguments refused (EARG)",
              st_holdout_sign(NULL, rl, sk1, sig2, sizeof sig2, &sl2, why,
                              sizeof why) == ST_HOLDOUT_EARG &&
                  st_holdout_sign(rec, rl, NULL, sig2, sizeof sig2, &sl2, why,
                                  sizeof why) == ST_HOLDOUT_EARG &&
                  st_holdout_verify_signed(rec, rl, sig, sl, NULL, &c, why,
                                           sizeof why) == ST_HOLDOUT_EARG &&
                  st_holdout_verify_signed(rec, rl, sig, sl, pk1, NULL, why,
                                           sizeof why) == ST_HOLDOUT_EARG);
    }

    /* --- CLI --- */
    char dir[] = "/tmp/st_holdout_sig_XXXXXX";
    if (!mkdtemp(dir)) {
        check("mkdtemp", 0);
    } else {
        char tsp[256], sp[256], recp[512], sigp[512], k1[256], k1h[256],
            k1d[256], k2[256], kpub[256], kpubh[256], kpubd[256], kpub2[256],
            kopen[256], sub[256], recs[512], buf[2048];
        static const uint8_t PKCS8[16] = {0x30, 0x2e, 0x02, 0x01, 0x00, 0x30,
                                          0x05, 0x06, 0x03, 0x2b, 0x65, 0x70,
                                          0x04, 0x22, 0x04, 0x20};
        static const uint8_t SPKI[12] = {0x30, 0x2a, 0x30, 0x05, 0x06, 0x03,
                                         0x2b, 0x65, 0x70, 0x03, 0x21, 0x00};
        uint8_t der[48];
        snprintf(tsp, sizeof tsp, "%s/taskset.txt", dir);
        snprintf(sp, sizeof sp, "%s/salt.bin", dir);
        snprintf(k1, sizeof k1, "%s/TEST-ONLY-key-1.sk", dir);
        snprintf(k1h, sizeof k1h, "%s/TEST-ONLY-key-1.hex.sk", dir);
        snprintf(k1d, sizeof k1d, "%s/TEST-ONLY-key-1.der.sk", dir);
        snprintf(k2, sizeof k2, "%s/TEST-ONLY-key-2.sk", dir);
        snprintf(kopen, sizeof kopen, "%s/TEST-ONLY-key-1.world.sk", dir);
        snprintf(kpub, sizeof kpub, "%s/TEST-ONLY-key-1.pk", dir);
        snprintf(kpubh, sizeof kpubh, "%s/TEST-ONLY-key-1.hex.pk", dir);
        snprintf(kpubd, sizeof kpubd, "%s/TEST-ONLY-key-1.der.pk", dir);
        snprintf(kpub2, sizeof kpub2, "%s/TEST-ONLY-key-2.pk", dir);
        snprintf(sub, sizeof sub, "%s/k2", dir);
        snprintf(recp, sizeof recp, "%s/g3-commit-%s.txt", dir, KAT_RD);
        snprintf(sigp, sizeof sigp, "%s/g3-sig-%s.txt", dir, KAT_RD);
        snprintf(recs, sizeof recs, "%s/g3-sig-%s.txt", sub, KAT_RD);
        mkdir(sub, 0700);
        write_file(tsp, KAT_TASKSET, sizeof(KAT_TASKSET) - 1, 0600);
        write_file(sp, salt, 32, 0600);
        write_file(k1, sk1, 32, 0600);
        hexs(sk1, 32, h);
        h[64] = '\n';
        write_file(k1h, h, 65, 0600);
        memcpy(der, PKCS8, 16);
        memcpy(der + 16, sk1, 32);
        write_file(k1d, der, 48, 0600);
        write_file(k2, sk2, 32, 0600);
        write_file(kopen, sk1, 32, 0644);
        write_file(kpub, pk1, 32, 0644);
        hexs(pk1, 32, h);
        write_file(kpubh, h, 64, 0644);
        memcpy(der, SPKI, 12);
        memcpy(der + 12, pk1, 32);
        write_file(kpubd, der, 44, 0644);
        write_file(kpub2, pk2, 32, 0644);

        check("cli commit (unchanged)",
              cli(5, "commit", "g3-kat-1", tsp, sp, dir) == 0);
        check("cli strict verify of an unsigned record refused",
              cli(4, "verify", "--strict", kpub, recp) == 2);
        check("cli plain verify of the unsigned record still OK",
              cli(2, "verify", recp) == 0);
        check("cli sign refuses a group/world-readable key file",
              cli(4, "sign", recp, kopen, dir) == 2 && access(sigp, F_OK) != 0);
        check("cli sign with TEST key 1", cli(4, "sign", recp, k1, dir) == 0);
        sl2 = read_all(sigp, buf, sizeof buf);
        check("cli signature file is content-named and equals the library's",
              sl2 == sl && !memcmp(buf, sig, sl));
        check("cli sign again is idempotent", cli(4, "sign", recp, k1, dir) == 0);
        check("cli sign with hex and PKCS#8 key files gives the same file",
              cli(4, "sign", recp, k1h, dir) == 0 &&
                  cli(4, "sign", recp, k1d, dir) == 0);
        check("cli sign with another key refuses to replace the file",
              cli(4, "sign", recp, k2, dir) == 2);
        check("cli strict verify (raw, hex, SPKI public key)",
              cli(4, "verify", "--strict", kpub, recp) == 0 &&
                  cli(4, "verify", "--strict", kpubh, recp) == 0 &&
                  cli(4, "verify", "--strict", kpubd, recp) == 0);
        check("cli strict verify with explicit signature path",
              cli(5, "verify", "--strict", kpub, recp, sigp) == 0);
        check("cli strict verify with the wrong public key refused",
              cli(4, "verify", "--strict", kpub2, recp) == 2);
        check("cli sign with key 2 into another folder",
              cli(4, "sign", recp, k2, sub) == 0);
        check("cli strict verify refuses key 2's signature under key 1",
              cli(5, "verify", "--strict", kpub, recp, recs) == 2);
        check("cli strict verify refuses a secret key given as public key",
              cli(4, "verify", "--strict", k1h, recp) == 2);
        write_file(sigp, sig, sl - 1, 0644);
        check("cli strict verify refuses a truncated signature file",
              cli(4, "verify", "--strict", kpub, recp) == 2);
        unlink(sigp);
        check("cli strict verify refuses once the signature is removed",
              cli(4, "verify", "--strict", kpub, recp) == 2);
        check("cli usage errors exit 1",
              cli(3, "verify", "--strict", kpub) == 1 &&
                  cli(3, "sign", recp, k1) == 1 &&
                  cli(3, "verify", "--lax", kpub) == 1);

        DIR *dd = opendir(sub);
        struct dirent *e;
        while (dd && (e = readdir(dd))) {
            char p[600];
            if (e->d_name[0] == '.')
                continue;
            snprintf(p, sizeof p, "%s/%s", sub, e->d_name);
            unlink(p);
        }
        if (dd)
            closedir(dd);
        rmdir(sub);
        dd = opendir(dir);
        while (dd && (e = readdir(dd))) {
            char p[600];
            if (e->d_name[0] == '.')
                continue;
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            unlink(p);
        }
        if (dd)
            closedir(dd);
        rmdir(dir);
    }
    memset(sk1, 0, sizeof sk1);
    memset(sk2, 0, sizeof sk2);
    (void)m;

    printf("st_holdout_sig: %d/%d PASS\n", g_pass, g_run);
    return g_pass == g_run ? 0 : 1;
}
