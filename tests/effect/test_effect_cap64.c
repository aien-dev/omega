/*
 * test_effect_cap64.c -- hostile tests for the 64-bit effect capability
 * generation migration (spec/effect-cap64-migration.md).
 *
 * Gates (each prints "GATE <name> PASS|FAIL <passed>/<total>"):
 *   OMEGA_EFFECT_CAP64_ROUNDTRIP_PASS     full generation survives build, canonical
 *                                         encoding, OMGG serialize/decode, validate,
 *                                         Visor inspection (struct, text, JSON)
 *   OMEGA_EFFECT_CAP64_IDENTITY_PASS      the SemanticId binds all 64 generation bits
 *   OMEGA_EFFECT_CAP64_STALE_REJECT_PASS  old / truncated / malformed bytes refused
 *                                         with the specific reason
 *   OMEGA_EFFECT_CAP64_AUTHORITY_PASS     the real AIENOS authority (pinned lib,
 *                                         aienos.lock) accepts honest references
 *                                         carried through effect objects and refuses
 *                                         truncated, stale, wrong-slot, wrong-right
 *                                         and pre-restart ones
 *
 * The authority side uses the pinned AIENOS header and library directly
 * (aienos_capability.h / libaienos_capability.a); the Omega side uses only
 * the physics-free core and the Visor effect-request adapter.
 */
#include "aienos_capability.h"

#include "omega_canonical.h"
#include "omega_codec.h"
#include "omega_core.h"
#include "omega_validate.h"
#include "sha256.h"
#include "visor_effect_request.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { G_ROUNDTRIP, G_IDENTITY, G_STALE, G_AUTHORITY, G_COUNT };
static const char *const GATE_NAME[G_COUNT] = {
    "OMEGA_EFFECT_CAP64_ROUNDTRIP_PASS", "OMEGA_EFFECT_CAP64_IDENTITY_PASS",
    "OMEGA_EFFECT_CAP64_STALE_REJECT_PASS", "OMEGA_EFFECT_CAP64_AUTHORITY_PASS",
};
static int g_pass[G_COUNT], g_total[G_COUNT];

#define CHECK(gate, cond, ...)                                                  \
    do {                                                                        \
        g_total[gate]++;                                                        \
        if (cond) g_pass[gate]++;                                               \
        else {                                                                  \
            fprintf(stderr, "FAIL [%s] %s:%d ", GATE_NAME[gate], __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                                       \
            fputc('\n', stderr);                                                \
        }                                                                       \
    } while (0)

#define U32MAX 0xFFFFFFFFull

/* ---- helpers ------------------------------------------------------------ */

static OmegaObject *effect(OmegaGraph *g, uint16_t cls, uint16_t op, uint32_t slot, uint64_t gen,
                           const char *params) {
    OmegaObject *o = omega_build_effect(g, cls, op, slot, gen);
    if (!o) return NULL;
    if (params) {
        EffectPayload e;
        if (omega_effect_read(o, &e) != OMEGA_EFFECT_OK) return NULL;
        e.param_len = (uint16_t)strlen(params);
        memcpy(e.param_bytes, params, e.param_len);
        omega_effect_write(o, &e);
        omega_compute_semantic_id(o);
    }
    return o;
}

static int canon(const OmegaObject *o, uint8_t *buf, size_t cap, size_t *len) {
    return omega_canonical_encode(o, buf, cap, len);
}

/* Offset of the kind-specific payload inside a canonical encoding of an
 * object with no attributes, relations or constraints. */
#define BARE_PAYLOAD_OFF (6 + 2 + 2 + 2 + 4)

/* One-object OMGG stream from raw canonical bytes. */
static size_t omgg1(const uint8_t *c, size_t clen, uint8_t *out) {
    out[0] = 'O'; out[1] = 'M'; out[2] = 'G'; out[3] = 'G';
    out[4] = 0; out[5] = 1;
    out[6] = (uint8_t)(clen >> 24); out[7] = (uint8_t)(clen >> 16);
    out[8] = (uint8_t)(clen >> 8); out[9] = (uint8_t)clen;
    memcpy(out + 10, c, clen);
    return 10 + clen;
}

/* Append one more object to an OMGG stream built by omgg1/omggn. */
static size_t omgg_append(uint8_t *s, size_t slen, const uint8_t *c, size_t clen) {
    uint16_t n = (uint16_t)((s[4] << 8) | s[5]);
    n++;
    s[4] = (uint8_t)(n >> 8); s[5] = (uint8_t)n;
    s[slen] = (uint8_t)(clen >> 24); s[slen + 1] = (uint8_t)(clen >> 16);
    s[slen + 2] = (uint8_t)(clen >> 8); s[slen + 3] = (uint8_t)clen;
    memcpy(s + slen + 4, c, clen);
    return slen + 4 + clen;
}

static void put_be64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (56 - 8 * i));
}

static int hex_eq(const SemanticId *id, const char *hex) {
    char h[65];
    omega_hex_semantic_id(id, h);
    return strcmp(h, hex) == 0;
}

/* The old (v1, origin/main 193a7e7) canonical encoding of an effect with no
 * params and zero capability_ref: header version 0x01, 176-byte little-endian
 * host-struct payload with a 32-bit generation. Built by hand from the old
 * layout so the test does not depend on the old code. */
static size_t v1_effect_bytes(uint16_t cls, uint16_t op, uint32_t slot, uint32_t gen32, uint8_t *out) {
    size_t p = 0;
    out[p++] = 'O'; out[p++] = 'M'; out[p++] = 'G'; out[p++] = '0';
    out[p++] = 0x01; out[p++] = KIND_EFFECT;
    for (int i = 0; i < 6; i++) out[p++] = 0;      /* attr, rel, const counts */
    out[p++] = 0; out[p++] = 0; out[p++] = 0; out[p++] = 176;
    uint8_t s[176];
    memset(s, 0, sizeof s);
    s[0] = (uint8_t)cls; s[1] = (uint8_t)(cls >> 8);
    s[2] = (uint8_t)op; s[3] = (uint8_t)(op >> 8);
    for (int i = 0; i < 4; i++) s[4 + i] = (uint8_t)(slot >> (8 * i));
    for (int i = 0; i < 4; i++) s[8 + i] = (uint8_t)(gen32 >> (8 * i));
    memcpy(out + p, s, sizeof s);
    return p + sizeof s;
}

static const uint64_t GENS[] = {
    0ull, 1ull, U32MAX, U32MAX + 1ull, 0x0123456789ABCDEFull, 1ull << 63,
    0xFFFFFFFFFFFFFFFEull, UINT64_MAX,
};
#define NGENS (sizeof GENS / sizeof GENS[0])

/* ---- ROUNDTRIP -------------------------------------------------------------- */

static void roundtrip_one(uint64_t gen, uint32_t slot, const char *params) {
    OmegaGraph *g = omega_graph_create();
    OmegaGraph *d = calloc(1, sizeof *d);
    static uint8_t buf[1 << 20];
    OmegaObject *o = effect(g, 7, 2, slot, gen, params);
    CHECK(G_ROUNDTRIP, o && o->payload_len == OMEGA_EFFECT_PAYLOAD_LEN, "build gen=%" PRIu64, gen);
    if (!o) goto out;
    SemanticId id = o->id;

    /* canonical header version and the big-endian generation bytes */
    uint8_t c[4096];
    size_t clen = 0;
    CHECK(G_ROUNDTRIP, canon(o, c, sizeof c, &clen) == 0 && c[4] == OMEGA_EFFECT_VERSION &&
          c[5] == KIND_EFFECT, "canonical header v2 gen=%" PRIu64, gen);
    uint8_t be[8];
    put_be64(be, gen);
    CHECK(G_ROUNDTRIP, clen == BARE_PAYLOAD_OFF + OMEGA_EFFECT_PAYLOAD_LEN &&
          memcmp(c + BARE_PAYLOAD_OFF + 8, be, 8) == 0,
          "wire carries all 8 generation bytes big-endian gen=%" PRIu64, gen);

    /* in-memory decode */
    EffectPayload e;
    CHECK(G_ROUNDTRIP, omega_effect_read(o, &e) == OMEGA_EFFECT_OK && e.capability_generation == gen &&
          e.capability_slot == slot && e.resource_class == 7 && e.operation_code == 2,
          "decode gen=%" PRIu64, gen);

    /* OMGG graph serialize -> deserialize */
    size_t n = 0;
    CHECK(G_ROUNDTRIP, omega_graph_serialize_binary(g, buf, sizeof buf, &n) == 0, "serialize");
    CHECK(G_ROUNDTRIP, omega_graph_deserialize_binary(buf, n, d) == 0 && d->object_count == 1,
          "deserialize gen=%" PRIu64, gen);
    const OmegaObject *o2 = omega_graph_find_object_const(d, &id);
    EffectPayload e2;
    CHECK(G_ROUNDTRIP, o2 && omega_effect_read(o2, &e2) == OMEGA_EFFECT_OK &&
          e2.capability_generation == gen && e2.capability_slot == slot &&
          memcmp(&e2, &e, sizeof e) == 0, "decoded object equal gen=%" PRIu64, gen);
    char err[200];
    CHECK(G_ROUNDTRIP, omega_validate_graph(d, err, sizeof err) == 0, "validate: %s", err);

    /* Visor inspection */
    VisorEffectRequest r;
    CHECK(G_ROUNDTRIP, visor_effect_request_build(d, &id, &r) == 0 && r.capability_generation == gen &&
          r.capability_slot == slot && !r.authorized, "visor request gen=%" PRIu64, gen);
    char text[2048], json[2048], want[96];
    CHECK(G_ROUNDTRIP, visor_effect_request_format_text(&r, text, sizeof text) == 0, "text");
    snprintf(want, sizeof want, "generation %" PRIu64 " ref", gen);
    CHECK(G_ROUNDTRIP, strstr(text, want) != NULL, "text prints exact generation %s", want);
    CHECK(G_ROUNDTRIP, visor_effect_request_format_json(&r, json, sizeof json) == 0, "json");
    snprintf(want, sizeof want, "\"capability_generation\":%" PRIu64 ",", gen);
    CHECK(G_ROUNDTRIP, strstr(json, want) != NULL, "json prints exact generation %s", want);
out:
    free(d);
    omega_graph_destroy(g);
}

static void test_roundtrip(void) {
    printf("-- roundtrip\n");
    for (size_t i = 0; i < NGENS; i++) roundtrip_one(GENS[i], 3, NULL);
    roundtrip_one(U32MAX + 1ull, UINT32_MAX, "set=42");
    roundtrip_one(1ull << 63, 255, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
                                   "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");

    /* Exact v2 layout of one fixture (spec/effect-cap64-migration.md 2.2). */
    OmegaGraph *g = omega_graph_create();
    OmegaObject *o = effect(g, 0x0102, 0x0304, 0x05060708u, 0x090A0B0C0D0E0F10ull, "ab");
    uint8_t want[OMEGA_EFFECT_PAYLOAD_LEN];
    memset(want, 0, sizeof want);
    const uint8_t head[] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                             0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10 };
    memcpy(want, head, sizeof head);
    want[0x30] = 0x00; want[0x31] = 0x02; want[0x32] = 'a'; want[0x33] = 'b';
    CHECK(G_ROUNDTRIP, o && o->payload_len == sizeof want && memcmp(o->payload, want, sizeof want) == 0,
          "fixture payload bytes match the spec layout exactly");
    omega_graph_destroy(g);
}

/* ---- IDENTITY --------------------------------------------------------------- */

/* New ids (spec/effect-cap64-migration.md section 3.1) and the old ids they replace. */
#define OLD_ID_OMEGATOOL "ff7aa9701f39e226ac7a8babaafd74c052b169e986f03bac8555d4d6bf4fc46e"
#define OLD_ID_VERIFY    "94d5b66158ff57354be51e44b3867b810c7a2afde8749a95722877ed01195bfb"
#define NEW_ID_OMEGATOOL "13f2c858be4dd0dcde1f458cf6e87d468a4d2bac2f8355254c32a977bc4692eb"
#define NEW_ID_VERIFY    "d3873a85a31eaa1b33018edebbf7e6dfce8a36a49703217505555a884fde7988"

static SemanticId id_of(uint64_t gen) {
    OmegaGraph *g = omega_graph_create();
    OmegaObject *o = effect(g, 1, 2, 9, gen, "p");
    SemanticId id = o->id;
    omega_graph_destroy(g);
    return id;
}

static void test_identity(void) {
    printf("-- identity\n");
    for (size_t i = 0; i < NGENS; i++) {
        uint64_t x = GENS[i];
        SemanticId a = id_of(x), b = id_of(x);
        CHECK(G_IDENTITY, omega_compare_semantic_id(&a, &b) == 0, "deterministic gen=%" PRIu64, x);
        SemanticId hi = id_of(x ^ (1ull << 32)), top = id_of(x ^ (1ull << 63)), lo = id_of(x ^ 1ull);
        CHECK(G_IDENTITY, omega_compare_semantic_id(&a, &hi) != 0, "bit 32 changes the id");
        CHECK(G_IDENTITY, omega_compare_semantic_id(&a, &top) != 0, "bit 63 changes the id");
        CHECK(G_IDENTITY, omega_compare_semantic_id(&a, &lo) != 0, "bit 0 changes the id");
        if (x > U32MAX) {
            SemanticId cut = id_of(x & U32MAX);
            CHECK(G_IDENTITY, omega_compare_semantic_id(&a, &cut) != 0,
                  "truncated generation has a different id gen=%" PRIu64, x);
        }
    }
    SemanticId m = id_of(U32MAX), m1 = id_of(U32MAX + 1ull), z = id_of(0);
    CHECK(G_IDENTITY, omega_compare_semantic_id(&m, &m1) != 0 && omega_compare_semantic_id(&m1, &z) != 0,
          "UINT32_MAX, UINT32_MAX+1 and 0 are three identities");

    /* id == sha256(canonical encoding) */
    OmegaGraph *g = omega_graph_create();
    OmegaObject *o = effect(g, 1, 2, 9, 1ull << 63, "set=42");
    uint8_t c[4096], h[32];
    size_t clen = 0;
    canon(o, c, sizeof c, &clen);
    sha256_hash(c, clen, h);
    CHECK(G_IDENTITY, memcmp(h, o->id.bytes, 32) == 0, "id is sha256 of the canonical bytes");

    /* Altering the generation after the id was computed. */
    SemanticId named = o->id;
    EffectPayload e;
    omega_effect_read(o, &e);
    const uint64_t variants[] = { e.capability_generation ^ (1ull << 40), e.capability_generation & U32MAX,
                                  e.capability_generation + 1, e.capability_generation - 1 };
    for (size_t i = 0; i < sizeof variants / sizeof variants[0]; i++) {
        EffectPayload t = e;
        t.capability_generation = variants[i];
        omega_effect_write(o, &t);
        VisorEffectRequest r;
        CHECK(G_IDENTITY, visor_effect_request_build(g, &named, &r) == -1 && !r.authorized,
              "edited generation without re-id refused (variant %zu)", i);
        omega_compute_semantic_id(o);
        CHECK(G_IDENTITY, omega_compare_semantic_id(&o->id, &named) != 0, "re-id gives a new id (variant %zu)", i);
        SemanticId nid = o->id;
        CHECK(G_IDENTITY, visor_effect_request_build(g, &nid, &r) == 0 &&
              r.capability_generation == variants[i] && memcmp(r.request_digest, named.bytes, 32) != 0,
              "re-identified object is a different request (variant %zu)", i);
        omega_effect_write(o, &e);
        omega_compute_semantic_id(o);
        CHECK(G_IDENTITY, omega_compare_semantic_id(&o->id, &named) == 0, "restore gives the old id back");
    }
    omega_graph_destroy(g);

    /* Golden ids of the two in-tree effects (old -> new, spec section 3). */
    g = omega_graph_create();
    OmegaObject *t1 = omega_build_effect(g, 1, 1, 0, 1);
    OmegaObject *t2 = omega_build_effect(g, 1, 1, 3, 1);
    char hx[65];
    omega_hex_semantic_id(&t1->id, hx);
    printf("   id (1,1,0,1) = %s\n", hx);
    omega_hex_semantic_id(&t2->id, hx);
    printf("   id (1,1,3,1) = %s\n", hx);
    CHECK(G_IDENTITY, hex_eq(&t1->id, NEW_ID_OMEGATOOL), "golden new id (1,1,0,1)");
    CHECK(G_IDENTITY, hex_eq(&t2->id, NEW_ID_VERIFY), "golden new id (1,1,3,1)");
    CHECK(G_IDENTITY, !hex_eq(&t1->id, OLD_ID_OMEGATOOL) && !hex_eq(&t2->id, OLD_ID_VERIFY),
          "new ids differ from the old ids");
    /* The old ids are exactly the hash of the hand-built v1 bytes (so the
     * refusal tests below refuse the real old format, not a guess). */
    uint8_t v1[256];
    SemanticId old;
    size_t v1n = v1_effect_bytes(1, 1, 0, 1, v1);
    sha256_hash(v1, v1n, old.bytes);
    CHECK(G_IDENTITY, hex_eq(&old, OLD_ID_OMEGATOOL), "hand-built v1 bytes hash to the old id (1,1,0,1)");
    v1n = v1_effect_bytes(1, 1, 3, 1, v1);
    sha256_hash(v1, v1n, old.bytes);
    CHECK(G_IDENTITY, hex_eq(&old, OLD_ID_VERIFY), "hand-built v1 bytes hash to the old id (1,1,3,1)");
    omega_graph_destroy(g);
}

/* ---- STALE_REJECT (hand-crafted bytes) ------------------------------------ */

static int decode_stream(const uint8_t *s, size_t n) {
    OmegaGraph *d = calloc(1, sizeof *d);
    int rc = omega_graph_deserialize_binary(s, n, d);
    free(d);
    return rc;
}

static void test_stale_reject(void) {
    printf("-- stale / truncated / legacy bytes\n");
    static uint8_t s[16384];
    uint8_t c[4096], v1[256];
    size_t clen = 0, n;

    /* header version rule */
    CHECK(G_STALE, omega_canonical_check_header(0x01, KIND_EFFECT) == OMEGA_CANON_ERR_EFFECT_V1, "v1 effect header");
    CHECK(G_STALE, omega_canonical_check_header(0x02, KIND_EFFECT) == OMEGA_CANON_OK, "v2 effect header");
    CHECK(G_STALE, omega_canonical_check_header(0x03, KIND_EFFECT) == OMEGA_CANON_ERR_VERSION, "v3 effect header");
    CHECK(G_STALE, omega_canonical_check_header(0x00, KIND_EFFECT) == OMEGA_CANON_ERR_VERSION, "v0 effect header");
    CHECK(G_STALE, omega_canonical_check_header(0x01, KIND_TYPE) == OMEGA_CANON_OK, "v1 type header (control)");
    CHECK(G_STALE, omega_canonical_check_header(0x02, KIND_TYPE) == OMEGA_CANON_ERR_VERSION, "v2 type header");
    CHECK(G_STALE, omega_canonical_check_header(0x02, KIND_VALUE) == OMEGA_CANON_ERR_VERSION, "v2 value header");

    /* A real old (v1) effect, carrying a generation that was truncated on the way in. */
    const uint64_t live = (1ull << 40) | 0x1234u;
    n = omgg1(v1, v1_effect_bytes(1, 2, 3, (uint32_t)(live & U32MAX), v1), s);
    CHECK(G_STALE, decode_stream(s, n) == -1, "old v1 effect with a truncated generation: refused");
    n = omgg1(v1, v1_effect_bytes(1, 1, 0, 1, v1), s);
    CHECK(G_STALE, decode_stream(s, n) == -1, "old v1 effect (omegatool fixture): refused");

    /* control: an unchanged v1 non-effect object still decodes; mixed stream is refused whole */
    OmegaGraph *g = omega_graph_create();
    OmegaObject *t = omega_build_type_uint(g, 64);
    canon(t, c, sizeof c, &clen);
    CHECK(G_STALE, c[4] == 0x01, "non-effect objects keep version 0x01");
    n = omgg1(c, clen, s);
    CHECK(G_STALE, decode_stream(s, n) == 0, "control: v1 type object decodes");
    size_t v1n = v1_effect_bytes(1, 1, 0, 1, v1);
    n = omgg_append(s, n, v1, v1n);
    CHECK(G_STALE, decode_stream(s, n) == -1, "stream with one v1 effect is refused as a whole");

    /* v2 header written in front of an unchanged non-effect object */
    c[4] = 0x02;
    n = omgg1(c, clen, s);
    CHECK(G_STALE, decode_stream(s, n) == -1, "v2 header on a type object: refused (no second identity)");

    /* v2 header in front of the old 176-byte struct payload */
    v1[4] = 0x02;
    n = omgg1(v1, v1n, s);
    CHECK(G_STALE, decode_stream(s, n) == -1, "v2 header + old 176-byte payload: refused");
    CHECK(G_STALE, omega_effect_payload_decode(v1 + BARE_PAYLOAD_OFF, 176, &(EffectPayload){0}) ==
          OMEGA_EFFECT_ERR_LEGACY_V1, "176-byte payload reported as legacy v1");

    /* v2 bytes with the high 32 bits of the generation cut out (174-byte payload) */
    OmegaObject *e = effect(g, 1, 2, 3, live, "x");
    canon(e, c, sizeof c, &clen);
    {
        uint8_t cut[4096];
        size_t po = BARE_PAYLOAD_OFF, k = 0;
        memcpy(cut, c, po + 8);                        /* header + first 8 payload bytes */
        k = po + 8;
        memcpy(cut + k, c + po + 12, clen - (po + 12)); /* drop generation bytes 0..3 (high word) */
        k += clen - (po + 12);
        uint32_t pl = OMEGA_EFFECT_PAYLOAD_LEN - 4;
        cut[po - 4] = (uint8_t)(pl >> 24); cut[po - 3] = (uint8_t)(pl >> 16);
        cut[po - 2] = (uint8_t)(pl >> 8); cut[po - 1] = (uint8_t)pl;
        n = omgg1(cut, k, s);
        CHECK(G_STALE, decode_stream(s, n) == -1, "generation with the high 32 bits cut out (174 bytes): refused");
        CHECK(G_STALE, omega_effect_payload_decode(cut + po, pl, &(EffectPayload){0}) == OMEGA_EFFECT_ERR_LENGTH,
              "174-byte payload: LENGTH");
    }
    /* stream cut in the middle of the generation field */
    n = omgg1(c, clen, s);
    CHECK(G_STALE, decode_stream(s, n) == 0, "control: intact v2 effect decodes");
    CHECK(G_STALE, decode_stream(s, 10 + BARE_PAYLOAD_OFF + 12) == -1, "stream truncated inside the generation: refused");
    CHECK(G_STALE, decode_stream(s, n - 1) == -1, "stream missing its last byte: refused");

    /* malformed v2 payloads, in memory and on the wire */
    struct { const char *what; size_t off; uint8_t val; int want; } bad[] = {
        { "resource_class 0", 0x00, 0x00, OMEGA_EFFECT_ERR_RESOURCE },
        { "param_len 129", 0x30, 0x00, OMEGA_EFFECT_ERR_PARAM_LEN },
        { "non-zero padding after params", 0x32 + 5, 0x7F, OMEGA_EFFECT_ERR_PADDING },
        { "non-zero last padding byte", OMEGA_EFFECT_PAYLOAD_LEN - 1, 0x01, OMEGA_EFFECT_ERR_PADDING },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        uint8_t p[OMEGA_EFFECT_PAYLOAD_LEN];
        memcpy(p, e->payload, sizeof p);
        if (bad[i].off == 0x00) { p[0] = 0; p[1] = 0; }
        else if (bad[i].off == 0x30) { p[0x30] = 0; p[0x31] = 129; }
        else p[bad[i].off] = bad[i].val;
        EffectPayload out;
        CHECK(G_STALE, omega_effect_payload_decode(p, sizeof p, &out) == bad[i].want, "decode: %s", bad[i].what);
        uint8_t cc[4096];
        memcpy(cc, c, clen);
        memcpy(cc + BARE_PAYLOAD_OFF, p, sizeof p);
        n = omgg1(cc, clen, s);
        CHECK(G_STALE, decode_stream(s, n) == -1, "wire: %s refused", bad[i].what);
        OmegaObject *m = omega_graph_add_object(g, KIND_EFFECT);
        memcpy(m->payload, p, sizeof p);
        m->payload_len = sizeof p;
        omega_compute_semantic_id(m);
        char err[200];
        SemanticId mid = m->id;
        VisorEffectRequest r;
        CHECK(G_STALE, omega_validate_object(g, m, err, sizeof err) != 0, "validate: %s refused", bad[i].what);
        CHECK(G_STALE, visor_effect_request_build(g, &mid, &r) == -1 && !r.authorized, "visor: %s refused", bad[i].what);
    }
    /* wrong lengths, and a raw memcpy of the (new) in-memory struct */
    const size_t lens[] = { 0, 8, 175, 176, 177, 179, 184, 256 };
    for (size_t i = 0; i < sizeof lens / sizeof lens[0]; i++) {
        OmegaObject *m = omega_graph_add_object(g, KIND_EFFECT);
        memset(m->payload, 0, lens[i] ? lens[i] : 1);
        memcpy(m->payload, e->payload, lens[i] < OMEGA_EFFECT_PAYLOAD_LEN ? lens[i] : OMEGA_EFFECT_PAYLOAD_LEN);
        m->payload_len = (uint32_t)lens[i];
        omega_compute_semantic_id(m);
        EffectPayload out;
        int want = lens[i] == 176 ? OMEGA_EFFECT_ERR_LEGACY_V1 : OMEGA_EFFECT_ERR_LENGTH;
        char err[200];
        CHECK(G_STALE, omega_effect_read(m, &out) == want && omega_validate_object(g, m, err, sizeof err) != 0,
              "payload length %zu refused", lens[i]);
    }
    {
        EffectPayload host;
        omega_effect_read(e, &host);
        OmegaObject *m = omega_graph_add_object(g, KIND_EFFECT);
        memcpy(m->payload, &host, sizeof host);        /* host-struct image, not the wire layout */
        m->payload_len = sizeof host;
        omega_compute_semantic_id(m);
        EffectPayload out;
        CHECK(G_STALE, sizeof host != OMEGA_EFFECT_PAYLOAD_LEN && omega_effect_read(m, &out) != OMEGA_EFFECT_OK,
              "raw host-struct image (%zu bytes) refused", sizeof host);
    }
    omega_graph_destroy(g);
}

/* ---- AUTHORITY (real AIENOS, pinned lib) ----------------------------------- */

#define SUBJ 7u
#define OTHER 9u
#define RES 0x7101ull

typedef struct {
    AienosCapAdmin *admin;
    AienosCapView *view;
} Auth;

static AienosCapRef office(Auth *a) {
    AienosCapRef o = { AIENOS_CAP_PARENT_NONE, 0 };
    aienos_cap_office(a->admin, &o);
    return o;
}

/* Put the next free slot on generation `gen` (0 = leave it) and mint there. */
static int mint_at(Auth *a, uint32_t next_free, uint64_t gen, uint32_t subject, uint32_t rights,
                   AienosCapRef *out) {
    if (gen) {
        int rc = aienos_cap_force_generation(a->admin, next_free, gen);
        if (rc != AIENOS_CAP_OK) return rc;
    }
    AienosCapMint m;
    memset(&m, 0, sizeof m);
    m.issuer = 3;
    m.subject = subject;
    m.resource = RES;
    m.rights = rights;
    m.parent = (AienosCapRef){ AIENOS_CAP_PARENT_NONE, 0 };
    m.authority = office(a);
    return aienos_cap_mint(a->admin, &m, out);
}

/* Carry a reference through an effect object: build, serialize, decode,
 * Visor request; return the reference the authority is shown. */
static int carry(AienosCapRef ref, uint16_t op, uint8_t *stream, size_t *stream_len, AienosCapRef *shown) {
    OmegaGraph *g = omega_graph_create();
    OmegaGraph *d = calloc(1, sizeof *d);
    int ok = -1;
    OmegaObject *o = effect(g, 1, op, ref.cap_id, ref.generation, "set=1");
    size_t n = 0;
    if (o && omega_graph_serialize_binary(g, stream, 8192, &n) == 0 &&
        omega_graph_deserialize_binary(stream, n, d) == 0) {
        VisorEffectRequest r;
        if (visor_effect_request_build(d, &o->id, &r) == 0) {
            shown->cap_id = r.capability_slot;
            shown->generation = r.capability_generation;
            ok = 0;
        }
    }
    if (stream_len) *stream_len = n;
    free(d);
    omega_graph_destroy(g);
    return ok;
}

/* Re-read a stored stream (e.g. saved before an authority restart). */
static int reread(const uint8_t *stream, size_t n, AienosCapRef *shown) {
    OmegaGraph *d = calloc(1, sizeof *d);
    int ok = -1;
    if (omega_graph_deserialize_binary(stream, n, d) == 0 && d->object_count == 1) {
        VisorEffectRequest r;
        if (visor_effect_request_build(d, &d->objects[0].id, &r) == 0) {
            shown->cap_id = r.capability_slot;
            shown->generation = r.capability_generation;
            ok = 0;
        }
    }
    free(d);
    return ok;
}

static int val(Auth *a, AienosCapRef r, uint32_t subject, uint64_t res, uint32_t rights) {
    return aienos_cap_validate(a->view, r, subject, res, rights, NULL);
}

static void test_authority(void) {
    printf("-- authority (AIENOS native capability library)\n");
    Auth a = { 0 };
    int rc = aienos_cap_start(&a.admin, &a.view);
    CHECK(G_AUTHORITY, rc == AIENOS_CAP_OK, "aienos_cap_start %d", rc);
    if (rc != AIENOS_CAP_OK) return;
    const uint32_t RW = AIENOS_CAP_RIGHT_READ | AIENOS_CAP_RIGHT_WRITE;
    static uint8_t st[8][8192];
    size_t stn[8];
    AienosCapRef shown;

    /* 1. natural generation (boot-time seeded by aienos#158) */
    AienosCapRef nat;
    CHECK(G_AUTHORITY, mint_at(&a, 1, 0, SUBJ, RW, &nat) == AIENOS_CAP_OK && nat.cap_id == 1, "natural mint");
    printf("   natural generation %" PRIu64 " (%s 2^32)\n", nat.generation,
           nat.generation > U32MAX ? "above" : "not above");
    CHECK(G_AUTHORITY, carry(nat, 2, st[0], &stn[0], &shown) == 0 && shown.generation == nat.generation &&
          shown.cap_id == nat.cap_id, "natural reference carried intact");
    CHECK(G_AUTHORITY, val(&a, shown, SUBJ, RES, AIENOS_CAP_RIGHT_WRITE) == AIENOS_CAP_OK,
          "natural reference validates after the round trip");

    /* 2. forced generations at the 32-bit boundary and at bit 63 */
    const uint64_t forced[] = { U32MAX, U32MAX + 1ull, 1ull << 63 };
    AienosCapRef ref[3];
    for (int i = 0; i < 3; i++) {
        uint32_t slot = 2u + (uint32_t)i;
        rc = mint_at(&a, slot, forced[i], SUBJ, RW, &ref[i]);
        CHECK(G_AUTHORITY, rc == AIENOS_CAP_OK && ref[i].cap_id == slot && ref[i].generation == forced[i],
              "mint at forced generation %" PRIu64 " (rc %d)", forced[i], rc);
        CHECK(G_AUTHORITY, carry(ref[i], 2, st[1 + i], &stn[1 + i], &shown) == 0 &&
              shown.generation == forced[i] && shown.cap_id == slot, "carried %" PRIu64, forced[i]);
        CHECK(G_AUTHORITY, val(&a, shown, SUBJ, RES, AIENOS_CAP_RIGHT_WRITE) == AIENOS_CAP_OK,
              "honest reference at %" PRIu64 " validates", forced[i]);
        AienosCapRef t = shown;
        t.generation = shown.generation & U32MAX;       /* what the old format kept */
        if (t.generation != shown.generation)
            CHECK(G_AUTHORITY, val(&a, t, SUBJ, RES, AIENOS_CAP_RIGHT_WRITE) == AIENOS_CAP_ERR_STALE_GEN,
                  "high 32 bits cut: STALE_GEN at %" PRIu64, forced[i]);
        t.generation = shown.generation + 1;
        CHECK(G_AUTHORITY, val(&a, t, SUBJ, RES, AIENOS_CAP_RIGHT_WRITE) == AIENOS_CAP_ERR_STALE_GEN,
              "generation+1: STALE_GEN");
        t.generation = shown.generation - 1;
        CHECK(G_AUTHORITY, val(&a, t, SUBJ, RES, AIENOS_CAP_RIGHT_WRITE) == AIENOS_CAP_ERR_STALE_GEN,
              "generation-1: STALE_GEN");
        /* wrong right / wrong resource / wrong subject on the carried reference */
        CHECK(G_AUTHORITY, val(&a, shown, SUBJ, RES, AIENOS_CAP_RIGHT_EFFECT) == AIENOS_CAP_ERR_RIGHTS,
              "wrong right: RIGHTS");
        CHECK(G_AUTHORITY, val(&a, shown, SUBJ, RES + 1, AIENOS_CAP_RIGHT_WRITE) == AIENOS_CAP_ERR_RESOURCE,
              "wrong resource: RESOURCE");
        CHECK(G_AUTHORITY, val(&a, shown, OTHER, RES, AIENOS_CAP_RIGHT_WRITE) == AIENOS_CAP_ERR_SUBJECT,
              "wrong subject: SUBJECT");
    }

    /* 3. hand-crafted wire: the v2 object with its high generation word zeroed
     *    decodes as a different (valid-looking) object and the authority refuses it */
    {
        OmegaGraph *d = calloc(1, sizeof *d);
        uint8_t w[8192];
        memcpy(w, st[3], stn[3]);                      /* the 1<<63 reference */
        size_t goff = 10 + BARE_PAYLOAD_OFF + 8;
        CHECK(G_AUTHORITY, w[goff] == 0x80, "located the generation bytes");
        memset(w + goff, 0, 4);
        CHECK(G_AUTHORITY, omega_graph_deserialize_binary(w, stn[3], d) == 0, "zeroed-high-word object decodes");
        EffectPayload e;
        CHECK(G_AUTHORITY, omega_effect_read(&d->objects[0], &e) == OMEGA_EFFECT_OK &&
              e.capability_generation == ((1ull << 63) & U32MAX), "it carries the truncated generation");
        OmegaGraph *d0 = calloc(1, sizeof *d0);
        omega_graph_deserialize_binary(st[3], stn[3], d0);
        CHECK(G_AUTHORITY, omega_compare_semantic_id(&d->objects[0].id, &d0->objects[0].id) != 0,
              "and a different SemanticId");
        AienosCapRef t = { e.capability_slot, e.capability_generation };
        CHECK(G_AUTHORITY, val(&a, t, SUBJ, RES, AIENOS_CAP_RIGHT_WRITE) == AIENOS_CAP_ERR_STALE_GEN,
              "authority refuses it: STALE_GEN");
        free(d0);
        free(d);
    }

    /* 4. wrong slot */
    AienosCapRef oth;
    CHECK(G_AUTHORITY, mint_at(&a, 5, (1ull << 62) + 7, OTHER, RW, &oth) == AIENOS_CAP_OK, "mint other subject");
    CHECK(G_AUTHORITY, carry(oth, 2, st[4], &stn[4], &shown) == 0 &&
          val(&a, shown, SUBJ, RES, AIENOS_CAP_RIGHT_WRITE) == AIENOS_CAP_ERR_SUBJECT,
          "someone else's live slot: SUBJECT");
    AienosCapRef ws = { ref[2].cap_id + 1000u, ref[2].generation };
    CHECK(G_AUTHORITY, carry(ws, 2, st[5], &stn[5], &shown) == 0 &&
          val(&a, shown, SUBJ, RES, AIENOS_CAP_RIGHT_WRITE) == AIENOS_CAP_ERR_BOUNDS, "slot out of range: BOUNDS");
    AienosCapRef sw = { ref[1].cap_id, ref[2].generation };   /* right generation, wrong slot */
    CHECK(G_AUTHORITY, carry(sw, 2, st[5], &stn[5], &shown) == 0 &&
          val(&a, shown, SUBJ, RES, AIENOS_CAP_RIGHT_WRITE) == AIENOS_CAP_ERR_STALE_GEN,
          "generation of slot 4 presented on slot 3: STALE_GEN");

    /* 5. stale: revoke, reclaim, re-mint in the same slot */
    AienosCapRef of = office(&a);
    CHECK(G_AUTHORITY, aienos_cap_revoke(a.admin, of, ref[0]) == AIENOS_CAP_OK, "revoke UINT32_MAX ref");
    CHECK(G_AUTHORITY, reread(st[1], stn[1], &shown) == 0 &&
          val(&a, shown, SUBJ, RES, AIENOS_CAP_RIGHT_WRITE) == AIENOS_CAP_ERR_REVOKED, "revoked: REVOKED");
    CHECK(G_AUTHORITY, aienos_cap_reclaim(a.admin, of, ref[0].cap_id) == AIENOS_CAP_OK, "reclaim (generation advances)");
    CHECK(G_AUTHORITY, reread(st[1], stn[1], &shown) == 0 &&
          val(&a, shown, SUBJ, RES, AIENOS_CAP_RIGHT_WRITE) == AIENOS_CAP_ERR_STALE_GEN, "reclaimed: STALE_GEN");
    AienosCapRef again;
    CHECK(G_AUTHORITY, mint_at(&a, ref[0].cap_id, 0, SUBJ, RW, &again) == AIENOS_CAP_OK &&
          again.cap_id == ref[0].cap_id && again.generation == U32MAX + 1ull,
          "re-mint in the slot crosses 2^32 (gen %" PRIu64 ")", again.generation);
    CHECK(G_AUTHORITY, reread(st[1], stn[1], &shown) == 0 &&
          val(&a, shown, SUBJ, RES, AIENOS_CAP_RIGHT_WRITE) == AIENOS_CAP_ERR_STALE_GEN,
          "old effect replayed against the re-grant: STALE_GEN");
    CHECK(G_AUTHORITY, carry(again, 2, st[6], &stn[6], &shown) == 0 &&
          val(&a, shown, SUBJ, RES, AIENOS_CAP_RIGHT_WRITE) == AIENOS_CAP_OK, "control: the re-grant validates");

    /* 6. replay after authority restart (restart-safe table, aienos#158) */
    CHECK(G_AUTHORITY, aienos_cap_kill(a.admin) == AIENOS_CAP_OK, "kill");
    rc = aienos_cap_restart(a.admin);
    CHECK(G_AUTHORITY, rc == AIENOS_CAP_OK, "restart (%d)", rc);
    for (int i = 0; i < 7; i++) {
        if (i == 5) continue;                          /* slot-out-of-range probe */
        CHECK(G_AUTHORITY, reread(st[i], stn[i], &shown) == 0, "pre-restart effect %d re-reads", i);
        int v = val(&a, shown, i == 4 ? OTHER : SUBJ, RES, AIENOS_CAP_RIGHT_WRITE);
        CHECK(G_AUTHORITY, v == AIENOS_CAP_ERR_STALE_GEN,
              "pre-restart reference (gen %" PRIu64 ") refused by the new table: %d", shown.generation, v);
    }
    AienosCapRef fresh;
    CHECK(G_AUTHORITY, mint_at(&a, 1, 0, SUBJ, RW, &fresh) == AIENOS_CAP_OK, "mint on the new table");
    printf("   post-restart generation %" PRIu64 "\n", fresh.generation);
    CHECK(G_AUTHORITY, fresh.generation > (1ull << 63), "new table starts above every old generation");
    CHECK(G_AUTHORITY, carry(fresh, 2, st[7], &stn[7], &shown) == 0 && shown.generation == fresh.generation &&
          val(&a, shown, SUBJ, RES, AIENOS_CAP_RIGHT_WRITE) == AIENOS_CAP_OK,
          "restart-safe generation round-trips and validates");

    /* 7. near the top: 0xFFFFFFFFFFFFFFFE validates; a further restart refuses (EXHAUSTED) */
    AienosCapRef top;
    rc = mint_at(&a, 2, 0xFFFFFFFFFFFFFFFEull, SUBJ, RW, &top);
    CHECK(G_AUTHORITY, rc == AIENOS_CAP_OK && top.generation == 0xFFFFFFFFFFFFFFFEull, "mint at 2^64-2 (%d)", rc);
    CHECK(G_AUTHORITY, carry(top, 2, st[0], &stn[0], &shown) == 0 && shown.generation == top.generation &&
          val(&a, shown, SUBJ, RES, AIENOS_CAP_RIGHT_WRITE) == AIENOS_CAP_OK, "2^64-2 round-trips and validates");
    AienosCapRef t = shown;
    t.generation = U32MAX - 1;                         /* 0xFFFFFFFE: the old format's view */
    CHECK(G_AUTHORITY, val(&a, t, SUBJ, RES, AIENOS_CAP_RIGHT_WRITE) == AIENOS_CAP_ERR_STALE_GEN,
          "2^64-2 cut to 32 bits: STALE_GEN");
    rc = aienos_cap_restart(a.admin);
    CHECK(G_AUTHORITY, rc == AIENOS_CAP_ERR_EXHAUSTED,
          "restart above 2^64-2 is refused (EXHAUSTED by design, never wraps): %d", rc);
    CHECK(G_AUTHORITY, val(&a, shown, SUBJ, RES, AIENOS_CAP_RIGHT_WRITE) == AIENOS_CAP_OK,
          "the table was not replaced by the refused restart");
    aienos_cap_stop(a.admin, a.view);
}

int main(void) {
    test_roundtrip();
    test_identity();
    test_stale_reject();
    test_authority();
    int all_p = 0, all_t = 0, ok = 1;
    for (int i = 0; i < G_COUNT; i++) {
        int pass = g_total[i] > 0 && g_pass[i] == g_total[i];
        printf("GATE %s %s %d/%d\n", GATE_NAME[i], pass ? "PASS" : "FAIL", g_pass[i], g_total[i]);
        all_p += g_pass[i];
        all_t += g_total[i];
        ok &= pass;
    }
    printf("%s %d/%d\n", ok ? "PASS" : "FAIL", all_p, all_t);
    if (ok) printf("OMEGA_EFFECT_CAP64_PASS\n");
    return ok ? 0 : 1;
}
