/*
 * aien_machine_id_test.c -- canonical machine identity (M20 identity convergence).
 * Covers: serialization round-trip, equality/hash determinism, conversion from
 * the identities the stack carries today, rejection of malformed persisted
 * identity. Known answers were computed independently with sha256sum.
 */
#include "runtime/aien_machine_id.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static void unhex(const char *s, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned v; sscanf(s + 2 * i, "%2x", &v); out[i] = (uint8_t)v;
    }
}

/* printf 'AIENOS-MACHINE-ID-V1\0aien-m20-kat-provisioned-root' | sha256sum */
static const char KAT_PROV[] = "b3d2c32ee1a3ef5803e1cc604943dc8189fda2f5ea90bb5158eecc0acf5ff366";
/* domain || 32 x 0xA5 (owner-key digest stand-in) */
static const char KAT_HW[]   = "f7492de96e80c47f2e160f34ca2137b2ea2e5baa75a6e2c8924a8ae479ec9db1";
/* physics FORGE V2 KAT machine_identity: sha256("FORGE-V2-KAT/machine/first-qualified-machine") */
static const char KAT_FORGE[] = "063af610c67991c10e693c5e48dd28662b8b057ae4b54a9e344949f6f4a2c2b7";

static void test_derive_and_aienos_compat(void) {
    AienMachineId m, again, hw;
    const char *root = "aien-m20-kat-provisioned-root";
    uint8_t want[32];
    CHECK(aien_mid_derive(AIEN_MID_ROOT_PROVISIONED, (const uint8_t *)root, strlen(root), &m) == AIEN_MID_OK);
    unhex(KAT_PROV, want, 32);
    /* id == aienos ADR 0014 machine_id_digest of the same provisioned bytes */
    CHECK(memcmp(m.id, want, 32) == 0);
    CHECK(m.root == AIEN_MID_ROOT_PROVISIONED);
    CHECK(aien_mid_derive(AIEN_MID_ROOT_PROVISIONED, (const uint8_t *)root, strlen(root), &again) == AIEN_MID_OK);
    CHECK(aien_mid_equal(&m, &again) && aien_mid_hash(&m) == aien_mid_hash(&again));

    uint8_t key[32]; memset(key, 0xA5, sizeof key);
    CHECK(aien_mid_derive(AIEN_MID_ROOT_HARDWARE, key, sizeof key, &hw) == AIEN_MID_OK);
    unhex(KAT_HW, want, 32);
    CHECK(memcmp(hw.id, want, 32) == 0 && hw.root == AIEN_MID_ROOT_HARDWARE);
    CHECK(!aien_mid_equal(&m, &hw));

    /* bad roots */
    CHECK(aien_mid_derive(AIEN_MID_ROOT_HARDWARE, key, 31, &hw) == AIEN_MID_E_ARG);
    CHECK(aien_mid_derive(AIEN_MID_ROOT_PROVISIONED, key, 0, &hw) == AIEN_MID_E_ARG);
    CHECK(aien_mid_derive(AIEN_MID_ROOT_IMPORTED, key, 32, &hw) == AIEN_MID_E_ARG);
    CHECK(aien_mid_derive(0, key, 32, &hw) == AIEN_MID_E_ARG);
}

static void test_round_trip(void) {
    AienMachineId m, d;
    const char *root = "aien-m20-kat-provisioned-root";
    aien_mid_derive(AIEN_MID_ROOT_PROVISIONED, (const uint8_t *)root, strlen(root), &m);

    uint8_t rec[AIEN_MID_RECORD_BYTES], rec2[AIEN_MID_RECORD_BYTES];
    CHECK(aien_mid_encode(&m, rec) == AIEN_MID_OK);
    CHECK(memcmp(rec, "AMID\x01\x01\x00\x00", 8) == 0);
    CHECK(aien_mid_decode(rec, sizeof rec, &d) == AIEN_MID_OK);
    CHECK(aien_mid_equal(&m, &d) && d.root == m.root);
    CHECK(aien_mid_encode(&d, rec2) == AIEN_MID_OK && memcmp(rec, rec2, sizeof rec) == 0);

    char text[AIEN_MID_TEXT_CHARS + 1];
    CHECK(aien_mid_to_text(&m, text) == AIEN_MID_OK && strlen(text) == AIEN_MID_TEXT_CHARS);
    CHECK(aien_mid_from_text(text, &d) == AIEN_MID_OK && aien_mid_equal(&m, &d) && d.root == m.root);

    char path[] = "/tmp/aien_mid_test_XXXXXX";
    int fd = mkstemp(path); CHECK(fd >= 0); close(fd);
    CHECK(aien_mid_store(path, &m) == AIEN_MID_OK);
    memset(&d, 0, sizeof d);
    CHECK(aien_mid_load(path, &d) == AIEN_MID_OK && aien_mid_equal(&m, &d) && d.root == m.root);
    unlink(path);
}

static void test_equality_hash(void) {
    AienMachineId a, b, c;
    uint8_t s[32];
    unhex(KAT_PROV, s, 32);
    aien_mid_from_slot(s, &a);
    aien_mid_derive(AIEN_MID_ROOT_PROVISIONED, (const uint8_t *)"aien-m20-kat-provisioned-root", 29, &b);
    /* root kind is provenance: the same machine compares equal from any source */
    CHECK(a.root == AIEN_MID_ROOT_IMPORTED && b.root == AIEN_MID_ROOT_PROVISIONED);
    CHECK(aien_mid_equal(&a, &b) && aien_mid_compare(&a, &b) == 0);
    /* hash is a fixed function of the bytes (first 8, big-endian): no seed */
    CHECK(aien_mid_hash(&a) == 0xb3d2c32ee1a3ef58ull);
    CHECK(aien_mid_hash(&a) == aien_mid_hash(&b));
    c = a; c.id[31] ^= 1;
    CHECK(!aien_mid_equal(&a, &c) && aien_mid_compare(&a, &c) != 0);
    c = a; c.id[0] ^= 1;
    CHECK(aien_mid_hash(&a) != aien_mid_hash(&c));
}

static void test_conversions(void) {
    AienMachineId m;
    uint8_t slot[32], back[32];
    /* FORGE V2 machine_identity d32 */
    unhex(KAT_FORGE, slot, 32);
    CHECK(aien_mid_from_slot(slot, &m) == AIEN_MID_OK && m.root == AIEN_MID_ROOT_IMPORTED);
    aien_mid_to_slot(&m, back);
    CHECK(memcmp(slot, back, 32) == 0);
    /* aienos receipt slot zero = absent: never an identity */
    memset(slot, 0, sizeof slot);
    CHECK(aien_mid_from_slot(slot, &m) == AIEN_MID_E_ZERO);

    /* capq's uint32 machine is an index; it resolves through the table */
    AienMachineId slots[2], x, y, z, got;
    AienMachineIndex t;
    aien_mid_index_init(&t, slots, 2);
    aien_mid_derive(AIEN_MID_ROOT_PROVISIONED, (const uint8_t *)"x", 1, &x);
    aien_mid_derive(AIEN_MID_ROOT_PROVISIONED, (const uint8_t *)"y", 1, &y);
    aien_mid_derive(AIEN_MID_ROOT_PROVISIONED, (const uint8_t *)"z", 1, &z);
    CHECK(aien_mid_index_bind(&t, &x) == 1);
    CHECK(aien_mid_index_bind(&t, &y) == 2);
    CHECK(aien_mid_index_bind(&t, &x) == 1);              /* idempotent */
    CHECK(aien_mid_index_bind(&t, &z) == 0);              /* bounded */
    CHECK(aien_mid_index_find(&t, &y) == 2 && aien_mid_index_find(&t, &z) == 0);
    CHECK(aien_mid_index_get(&t, 2, &got) == AIEN_MID_OK && aien_mid_equal(&got, &y));
    CHECK(aien_mid_index_get(&t, 0, &got) == AIEN_MID_E_ARG);
    CHECK(aien_mid_index_get(&t, 3, &got) == AIEN_MID_E_ARG);
    memset(&z, 0, sizeof z); z.root = AIEN_MID_ROOT_IMPORTED;
    CHECK(aien_mid_index_bind(&t, &z) == 0);              /* zero id never bound */
}

static void test_malformed(void) {
    AienMachineId m, d;
    aien_mid_derive(AIEN_MID_ROOT_HARDWARE, (const uint8_t *)"0123456789abcdef0123456789abcdef", 32, &m);
    uint8_t good[AIEN_MID_RECORD_BYTES], bad[AIEN_MID_RECORD_BYTES + 1];
    aien_mid_encode(&m, good);

    CHECK(aien_mid_decode(good, sizeof good - 1, &d) == AIEN_MID_E_FORMAT);
    memcpy(bad, good, sizeof good); bad[44] = 0;
    CHECK(aien_mid_decode(bad, sizeof bad, &d) == AIEN_MID_E_FORMAT);
    memcpy(bad, good, sizeof good); bad[0] = 'X';
    CHECK(aien_mid_decode(bad, 44, &d) == AIEN_MID_E_FORMAT);
    memcpy(bad, good, sizeof good); bad[4] = 2;
    CHECK(aien_mid_decode(bad, 44, &d) == AIEN_MID_E_FORMAT);
    memcpy(bad, good, sizeof good); bad[5] = 9;
    CHECK(aien_mid_decode(bad, 44, &d) == AIEN_MID_E_FORMAT);
    memcpy(bad, good, sizeof good); bad[7] = 1;
    CHECK(aien_mid_decode(bad, 44, &d) == AIEN_MID_E_FORMAT);
    for (unsigned i = 8; i < 44; i++) {   /* any flipped id or check bit */
        memcpy(bad, good, sizeof good); bad[i] ^= 0x10;
        CHECK(aien_mid_decode(bad, 44, &d) == AIEN_MID_E_CHECK);
    }
    /* well-framed record of a zero id, with a correct check */
    memcpy(bad, good, sizeof good); memset(bad + 8, 0, 32);
    { uint8_t dg[32]; sha256_hash(bad, 40, dg); memcpy(bad + 40, dg, 4); }
    CHECK(aien_mid_decode(bad, 44, &d) == AIEN_MID_E_ZERO);

    char text[AIEN_MID_TEXT_CHARS + 1];
    aien_mid_to_text(&m, text);
    text[10] = 'G';
    CHECK(aien_mid_from_text(text, &d) == AIEN_MID_E_FORMAT);
    aien_mid_to_text(&m, text);
    for (char *p = text; *p; p++) if (*p >= 'a' && *p <= 'f') { *p = (char)(*p - 32); break; }
    CHECK(aien_mid_from_text(text, &d) == AIEN_MID_E_FORMAT);   /* uppercase not canonical */
    text[AIEN_MID_TEXT_CHARS - 1] = 0;
    CHECK(aien_mid_from_text(text, &d) == AIEN_MID_E_FORMAT);

    /* persisted files: truncated, extended, garbage, missing */
    char path[] = "/tmp/aien_mid_bad_XXXXXX";
    int fd = mkstemp(path); CHECK(fd >= 0);
    CHECK(write(fd, good, 20) == 20); close(fd);
    CHECK(aien_mid_load(path, &d) == AIEN_MID_E_FORMAT);
    fd = open(path, O_WRONLY | O_TRUNC); CHECK(write(fd, good, 44) == 44 && write(fd, "x", 1) == 1); close(fd);
    CHECK(aien_mid_load(path, &d) == AIEN_MID_E_FORMAT);
    fd = open(path, O_WRONLY | O_TRUNC); memcpy(bad, good, 44); bad[20] ^= 1;
    CHECK(write(fd, bad, 44) == 44); close(fd);
    CHECK(aien_mid_load(path, &d) == AIEN_MID_E_CHECK);
    unlink(path);
    CHECK(aien_mid_load(path, &d) == AIEN_MID_E_IO);

    /* encode refuses what decode would refuse */
    d = m; d.root = 0;
    CHECK(aien_mid_encode(&d, good) == AIEN_MID_E_ARG);
    d = m; memset(d.id, 0, 32);
    CHECK(aien_mid_encode(&d, good) == AIEN_MID_E_ZERO);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--kat-text") == 0) {   /* for the ARGUS adapter check */
        AienMachineId m;
        char text[AIEN_MID_TEXT_CHARS + 1];
        aien_mid_derive(AIEN_MID_ROOT_PROVISIONED, (const uint8_t *)"aien-m20-kat-provisioned-root", 29, &m);
        aien_mid_to_text(&m, text);
        printf("%s\n", text);
        return 0;
    }
    test_derive_and_aienos_compat();
    test_round_trip();
    test_equality_hash();
    test_conversions();
    test_malformed();
    printf("aien_machine_id: %d/%d checks passed\n", checks - fails, checks);
    if (fails) return 1;
    printf("M20_MACHINE_IDENTITY_PASS\n");
    return 0;
}
