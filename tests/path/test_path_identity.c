/* TEST HARNESS: PATH-1 native identity and construction
 * (spec/path-semantic-object.md section 15, PATH-1 pass criteria).
 * C only, CPU only, no heap. Prints one FAIL line per failed check. */
#include "path/rx_path.h"
#include "sha256.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #c); } } while (0)

/* Frozen known answers. They pin the big-endian layout and both tags across
 * hosts: any change to the encoding or identity law changes them. */
#define KAT_SEMANTIC_HEX "90b28b4367cc0f6cc7bd5ec45930629a25d0303a64da13a57e9dfc4a728b7938"
#define KAT_REALIZATION_HEX "32dfd9644010a192e879186c3c0b7824fdc59f7061368dac2c9b15a22b13e6ce"
#define RANDOM_FOLD_HEX "30fa471ba8284437c2a38fbb12c021fff3d0418db2a616ebc7c390c18193134c"

static RxPathStepArena arena;
static RxPath pa, pb, pc, pd, pe, pf;
static uint8_t buf[RX_PATH_MAX_TOTAL_SERIALIZATION + 64];
static uint8_t buf2[RX_PATH_MAX_TOTAL_SERIALIZATION + 64];

static SemanticId mkid(unsigned v) {
    SemanticId r;
    for (unsigned i = 0; i < 32; i++) r.bytes[i] = (uint8_t)(v * 7u + i);
    return r;
}
static int same(const SemanticId *x, const SemanticId *y) { return !memcmp(x->bytes, y->bytes, 32); }
static void hex(const SemanticId *id, char out[65]) {
    static const char d[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; i++) { out[2*i] = d[id->bytes[i] >> 4]; out[2*i+1] = d[id->bytes[i] & 15]; }
    out[64] = 0;
}
/* Independent identity: hash written out here, not through rx_path. */
static void tagged_sha(const char *tag, const uint8_t *b, size_t n, SemanticId *out) {
    sha256_ctx h; uint8_t zero = 0;
    sha256_init(&h);
    sha256_update(&h, (const uint8_t *)tag, strlen(tag));
    sha256_update(&h, &zero, 1);
    sha256_update(&h, b, n);
    sha256_final(&h, out->bytes);
}

/* xorshift64: platform-stable pseudo-random stream. */
static uint64_t rng;
static uint32_t rnd(uint32_t bound) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)(rng % bound);
}
static SemanticId rid(void) {
    SemanticId r;
    for (unsigned i = 0; i < 32; i++) r.bytes[i] = (uint8_t)rnd(256);
    return r;
}

static RxPathStep simple_step(uint16_t role, unsigned seed) {
    RxPathStep s;
    memset(&s, 0, sizeof(s));
    s.step_family = (uint16_t)(role >> 8);
    s.step_role = role;
    s.operator_id = mkid(seed);
    return s;
}

/* Hand-written expected bytes for the KAT path. */
static uint8_t kat[512];
static size_t katn;
static size_t off_attr_count, off_attr0, off_attr1, off_cons_count, off_steps_len, off_s0, off_s1;
static void lit(const uint8_t *b, size_t n) { memcpy(kat + katn, b, n); katn += n; }
static void lit_id(unsigned v) { SemanticId x = mkid(v); lit(x.bytes, 32); }
#define LIT(...) do { static const uint8_t t_[] = {__VA_ARGS__}; lit(t_, sizeof(t_)); } while (0)

static void build_kat_bytes(void) {
    katn = 0;
    LIT(0x4F, 0x4D, 0x47, 0x30, 0x01, 0x0C, 0x00, 0x02);
    lit_id(0x11); lit_id(0x22); lit_id(0x33);
    off_attr_count = katn;
    LIT(0x00, 0x02);
    off_attr0 = katn;
    LIT(0x05, 'a', 'l', 'p', 'h', 'a', 0x00, 0x02, 0x02, 0x03);
    off_attr1 = katn;
    LIT(0x04, 'z', 'e', 't', 'a', 0x00, 0x01, 0x01);
    off_cons_count = katn;
    LIT(0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x01, 0xAA);
    off_steps_len = katn;
    LIT(0x00, 0x00, 0x00, 0x9E); /* 112 + 46 */
    off_s0 = katn;
    LIT(0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00, 0x01);
    lit_id(0x40); lit_id(0x50);
    LIT(0x00, 0x01);
    lit_id(0x60);
    LIT(0x00, 0x00, 0x00, 0x02, 0x01, 0x02);
    off_s1 = katn;
    LIT(0x00, 0x01, 0x00, 0x02, 0x02, 0x04, 0x00, 0x00);
    lit_id(0x51);
    LIT(0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
}

/* Builds the KAT path with attributes and constraints in reverse order. */
static void build_kat_path(RxPath *p) {
    SemanticId s = mkid(0x11), e = mkid(0x22), c = mkid(0x33);
    static const uint8_t v1[] = {0x01}, v2[] = {0x02, 0x03}, cp[] = {0xAA};
    CHECK(rx_path_init(p, &arena, &s, &e, &c) == RX_PATH_OK);
    CHECK(rx_path_add_attribute(p, "zeta", v1, 1) == RX_PATH_OK);
    CHECK(rx_path_add_attribute(p, "alpha", v2, 2) == RX_PATH_OK);
    CHECK(rx_path_add_constraint(p, 3, cp, 1) == RX_PATH_OK);
    CHECK(rx_path_add_constraint(p, 1, NULL, 0) == RX_PATH_OK);
    RxPathStep a = simple_step(ROLE_REL_ENTITY, 0x50);
    a.input_count = 1; a.input_ids[0] = mkid(0x40);
    a.output_count = 1; a.output_ids[0] = mkid(0x60);
    a.param_len = 2; a.param_bytes[0] = 1; a.param_bytes[1] = 2;
    CHECK(rx_path_append(p, &a) == RX_PATH_OK);
    RxPathStep b = simple_step(ROLE_EXEC_RESULT, 0x51);
    CHECK(rx_path_append(p, &b) == RX_PATH_OK);
}

static RxPathRealization base_realization(void) {
    RxPathRealization r;
    memset(&r, 0, sizeof(r));
    r.machine_id = mkid(0x70); r.engine_profile_id = mkid(0x71);
    r.code_digest = mkid(0x72); r.schedule_kind = 1; r.argus_observation_digest = mkid(0x73);
    return r;
}

static void known_answer(void) {
    rx_path_arena_init(&arena);
    build_kat_bytes();
    build_kat_path(&pa);
    size_t n = 0;
    CHECK(rx_path_encode(&pa, buf, sizeof(buf), &n) == RX_PATH_OK);
    CHECK(n == katn && memcmp(buf, kat, katn) == 0);
    CHECK(buf[6] == 0x00 && buf[7] == 0x02 && buf[0x68] == 0x00 && buf[0x69] == 0x02);
    CHECK(off_attr_count == 0x68);
    SemanticId sem, want, real;
    CHECK(rx_path_semantic_id(&pa, &sem) == RX_PATH_OK);
    tagged_sha("omega.path.v1", kat, katn, &want);
    CHECK(same(&sem, &want));
    RxPathRealization r = base_realization();
    CHECK(rx_path_realization_id(&pa, &r, &real) == RX_PATH_OK);
    uint8_t pre[32 * 5 + 2];
    memcpy(pre, sem.bytes, 32); memcpy(pre + 32, r.machine_id.bytes, 32);
    memcpy(pre + 64, r.engine_profile_id.bytes, 32); memcpy(pre + 96, r.code_digest.bytes, 32);
    pre[128] = 0x00; pre[129] = 0x01; memcpy(pre + 130, r.argus_observation_digest.bytes, 32);
    tagged_sha("omega.path.realization.v1", pre, sizeof(pre), &want);
    CHECK(same(&real, &want));
    char h1[65], h2[65];
    hex(&sem, h1); hex(&real, h2);
    printf("KAT semantic    %s\nKAT realization %s\n", h1, h2);
    CHECK(strcmp(h1, KAT_SEMANTIC_HEX) == 0);
    CHECK(strcmp(h2, KAT_REALIZATION_HEX) == 0);
}

/* Random content for one path, applied in forward or reverse builder order. */
typedef struct {
    unsigned nsteps, nattr, ncons;
    RxPathStep steps[24];
    char keys[6][8]; uint8_t vals[6][4]; uint16_t vlen[6];
    uint16_t ckind[4]; uint8_t cpay[4][3]; uint16_t clen[4];
    SemanticId start, end, ctx;
} Spec;
static Spec spec;
static const uint8_t fam_roles[6] = {0, 4, 4, 7, 7, 6};

static void random_spec(void) {
    memset(&spec, 0, sizeof(spec));
    spec.start = rid(); spec.end = rid(); spec.ctx = rid();
    spec.nsteps = rnd(25);
    for (unsigned i = 0; i < spec.nsteps; i++) {
        RxPathStep *s = &spec.steps[i];
        uint16_t f = (uint16_t)(1 + rnd(5));
        s->step_family = f;
        s->step_role = (uint16_t)((f << 8) | (1 + rnd(fam_roles[f])));
        s->input_count = (uint16_t)rnd(9);
        for (unsigned k = 0; k < s->input_count; k++) s->input_ids[k] = rid();
        s->operator_id = rid();
        s->output_count = (uint16_t)rnd(9);
        for (unsigned k = 0; k < s->output_count; k++) s->output_ids[k] = rid();
        s->param_len = rnd(65);
        for (unsigned k = 0; k < s->param_len; k++) s->param_bytes[k] = (uint8_t)rnd(256);
    }
    spec.nattr = rnd(7);
    unsigned base = rnd(50);
    for (unsigned i = 0; i < spec.nattr; i++) {
        snprintf(spec.keys[i], sizeof(spec.keys[i]), "k%02u", (base + i * 7u) % 100u);
        spec.vlen[i] = (uint16_t)rnd(5);
        for (unsigned k = 0; k < spec.vlen[i]; k++) spec.vals[i][k] = (uint8_t)rnd(256);
    }
    spec.ncons = rnd(5);
    for (unsigned i = 0; i < spec.ncons; i++) {
        spec.ckind[i] = (uint16_t)(1 + rnd(9));
        spec.clen[i] = (uint16_t)(1 + rnd(3));
        spec.cpay[i][0] = (uint8_t)i; /* unique payload head: no duplicates */
        for (unsigned k = 1; k < spec.clen[i]; k++) spec.cpay[i][k] = (uint8_t)rnd(256);
    }
}

static int build_spec(RxPath *p, int reverse, int swap_a, int swap_b) {
    int ok = rx_path_init(p, &arena, &spec.start, &spec.end, &spec.ctx) == RX_PATH_OK;
    for (unsigned j = 0; j < spec.nattr; j++) {
        unsigned i = reverse ? spec.nattr - 1 - j : j;
        ok &= rx_path_add_attribute(p, spec.keys[i], spec.vals[i], spec.vlen[i]) == RX_PATH_OK;
    }
    for (unsigned j = 0; j < spec.ncons; j++) {
        unsigned i = reverse ? spec.ncons - 1 - j : j;
        ok &= rx_path_add_constraint(p, spec.ckind[i], spec.cpay[i], spec.clen[i]) == RX_PATH_OK;
    }
    for (unsigned j = 0; j < spec.nsteps; j++) {
        unsigned i = j;
        if ((int)j == swap_a) i = (unsigned)swap_b;
        else if ((int)j == swap_b) i = (unsigned)swap_a;
        ok &= rx_path_append(p, &spec.steps[i]) == RX_PATH_OK;
    }
    return ok;
}

static void randomized(void) {
    sha256_ctx fold;
    sha256_init(&fold);
    unsigned roundtrips = 0, swaps = 0;
    for (unsigned t = 0; t < 1000; t++) {
        rng = 0x9E3779B97F4A7C15ull ^ ((uint64_t)(t + 1) * 0xD1B54A32D192ED03ull);
        rx_path_arena_init(&arena);
        random_spec();
        SemanticId a, a2, b, d;
        CHECK(build_spec(&pa, 0, -1, -1));
        CHECK(build_spec(&pb, 1, -1, -1));
        CHECK(rx_path_semantic_id(&pa, &a) == RX_PATH_OK);
        CHECK(rx_path_semantic_id(&pa, &a2) == RX_PATH_OK && same(&a, &a2));
        CHECK(rx_path_semantic_id(&pb, &b) == RX_PATH_OK && same(&a, &b));
        size_t n = 0, n2 = 0;
        CHECK(rx_path_encode(&pa, buf, sizeof(buf), &n) == RX_PATH_OK);
        int eq = 0;
        if (rx_path_decode(&pc, &arena, buf, n, &a) == RX_PATH_OK &&
            rx_path_equal(&pa, &pc, &eq) == RX_PATH_OK && eq &&
            rx_path_semantic_id(&pc, &d) == RX_PATH_OK && same(&a, &d) &&
            rx_path_encode(&pc, buf2, sizeof(buf2), &n2) == RX_PATH_OK &&
            n2 == n && memcmp(buf, buf2, n) == 0)
            roundtrips++;
        if (spec.nsteps >= 2) {
            unsigned x = rnd(spec.nsteps), y = rnd(spec.nsteps);
            RxPathStep sx, sy;
            if (x != y && rx_path_get_step(&pa, x, &sx) == 0 && rx_path_get_step(&pa, y, &sy) == 0 &&
                memcmp(&spec.steps[x], &spec.steps[y], sizeof(RxPathStep)) != 0) {
                SemanticId s;
                CHECK(build_spec(&pd, 0, (int)x, (int)y));
                CHECK(rx_path_semantic_id(&pd, &s) == RX_PATH_OK && !same(&a, &s));
                swaps++;
            }
        }
        sha256_update(&fold, a.bytes, 32);
    }
    CHECK(roundtrips == 1000);
    CHECK(swaps > 800);
    SemanticId f;
    char h[65];
    sha256_final(&fold, f.bytes);
    hex(&f, h);
    printf("random fold     %s (1000 paths, %u round trips, %u swaps)\n", h, roundtrips, swaps);
    CHECK(strcmp(h, RANDOM_FOLD_HEX) == 0);
}

static void order_sensitivity(void) {
    rx_path_arena_init(&arena);
    SemanticId s = mkid(1), e = mkid(2), c = mkid(3), x, y;
    RxPathStep a = simple_step(ROLE_EXEC_OPERATION, 10), b = simple_step(ROLE_EXEC_VERIFICATION, 11);
    CHECK(rx_path_init(&pa, &arena, &s, &e, &c) == 0);
    CHECK(rx_path_append(&pa, &a) == 0 && rx_path_append(&pa, &b) == 0);
    CHECK(rx_path_init(&pb, &arena, &s, &e, &c) == 0);
    CHECK(rx_path_append(&pb, &b) == 0 && rx_path_append(&pb, &a) == 0);
    CHECK(rx_path_semantic_id(&pa, &x) == 0 && rx_path_semantic_id(&pb, &y) == 0);
    CHECK(!same(&x, &y));
}

static void realization_separation(void) {
    rx_path_arena_init(&arena);
    build_kat_path(&pa);
    RxPathRealization r = base_realization(), m;
    SemanticId sem0, real0, sem, real;
    CHECK(rx_path_semantic_id(&pa, &sem0) == 0 && rx_path_realization_id(&pa, &r, &real0) == 0);
    for (unsigned field = 0; field < 5; field++) {
        m = r;
        if (field == 0) m.machine_id = mkid(0x90);
        if (field == 1) m.engine_profile_id = mkid(0x91);
        if (field == 2) m.code_digest = mkid(0x92);
        if (field == 3) m.schedule_kind = 2;
        if (field == 4) m.argus_observation_digest = mkid(0x93); /* evidence */
        CHECK(rx_path_realization_id(&pa, &m, &real) == 0 && !same(&real, &real0));
        CHECK(rx_path_semantic_id(&pa, &sem) == 0 && same(&sem, &sem0));
    }
    CHECK(rx_path_realization_id(&pa, &r, &real) == 0 && same(&real, &real0));
    /* Semantic change moves both identities. */
    SemanticId e2 = mkid(0x23);
    CHECK(rx_path_set_end_anchor(&pa, &e2) == 0);
    CHECK(rx_path_semantic_id(&pa, &sem) == 0 && !same(&sem, &sem0));
    CHECK(rx_path_realization_id(&pa, &r, &real) == 0 && !same(&real, &real0));
}

static int decode_rc(const uint8_t *b, size_t n, const SemanticId *want) {
    uint32_t before = arena.used;
    int rc = rx_path_decode(&pc, &arena, b, n, want);
    if (rc != RX_PATH_OK) CHECK(arena.used == before && rx_path_step_count(&pc) == 0);
    return rc;
}

/* Patch kat into buf2, rehash it so only the canonical check can refuse. */
static int patched_rc(size_t at, const uint8_t *b, size_t n) {
    SemanticId want;
    memcpy(buf2, kat, katn);
    memcpy(buf2 + at, b, n);
    tagged_sha("omega.path.v1", buf2, katn, &want);
    return decode_rc(buf2, katn, &want);
}

static void tamper_and_canonical(void) {
    rx_path_arena_init(&arena);
    build_kat_bytes();
    build_kat_path(&pa);
    SemanticId id;
    CHECK(rx_path_semantic_id(&pa, &id) == 0);
    CHECK(decode_rc(kat, katn, &id) == RX_PATH_OK);
    unsigned refused = 0;
    for (size_t bit = 0; bit < katn * 8; bit++) {
        memcpy(buf2, kat, katn);
        buf2[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        if (decode_rc(buf2, katn, &id) != RX_PATH_OK) refused++;
    }
    printf("bit flips       %u of %u refused\n", refused, (unsigned)(katn * 8));
    CHECK(refused == katn * 8);
    unsigned truncated = 0;
    for (size_t n = 0; n < katn; n++)
        if (decode_rc(kat, n, &id) != RX_PATH_OK) truncated++;
    CHECK(truncated == katn);
    memcpy(buf2, kat, katn); buf2[katn] = 0;
    CHECK(decode_rc(buf2, katn + 1, &id) != RX_PATH_OK);
    SemanticId other = mkid(5);
    CHECK(decode_rc(kat, katn, &other) == RX_PATH_ERR_ID_MISMATCH);

    /* Non-canonical inputs refused even when the expected id matches them. */
    uint8_t swapped[18];
    memcpy(swapped, kat + off_attr1, 8);
    memcpy(swapped + 8, kat + off_attr0, 10);
    CHECK(patched_rc(off_attr0, swapped, 18) == RX_PATH_ERR_MALFORMED);
    uint8_t dup[18];
    memcpy(dup, kat + off_attr1, 8); memcpy(dup + 8, kat + off_attr1, 8);
    dup[16] = 0x78; dup[17] = 0x78; /* keep length; content no longer canonical */
    CHECK(patched_rc(off_attr0, dup, 18) == RX_PATH_ERR_MALFORMED);
    static const uint8_t cons_swapped[9] = {0x00, 0x03, 0x00, 0x01, 0xAA, 0x00, 0x01, 0x00, 0x00};
    CHECK(patched_rc(off_cons_count + 2, cons_swapped, 9) == RX_PATH_ERR_MALFORMED);
    static const uint8_t idx5[2] = {0x00, 0x05};
    CHECK(patched_rc(off_s1, idx5, 2) == RX_PATH_ERR_MALFORMED);
    static const uint8_t badrole[2] = {0x02, 0x05};
    CHECK(patched_rc(off_s1 + 4, badrole, 2) == RX_PATH_ERR_MALFORMED);
    static const uint8_t crossrole[2] = {0x03, 0x01};
    CHECK(patched_rc(off_s1 + 4, crossrole, 2) == RX_PATH_ERR_MALFORMED);
    static const uint8_t badlen[4] = {0x00, 0x00, 0x00, 0x9D};
    CHECK(patched_rc(off_steps_len, badlen, 4) == RX_PATH_ERR_MALFORMED);
    /* One trailing byte with steps_payload_len raised to cover it. */
    {
        SemanticId want;
        memcpy(buf2, kat, katn);
        buf2[off_steps_len + 3] = 0x9F;
        buf2[katn] = 0x00;
        tagged_sha("omega.path.v1", buf2, katn + 1, &want);
        CHECK(decode_rc(buf2, katn + 1, &want) == RX_PATH_ERR_MALFORMED);
    }
    static const uint8_t badmagic[1] = {0x31};
    CHECK(patched_rc(3, badmagic, 1) == RX_PATH_ERR_MALFORMED);
    static const uint8_t badver[1] = {0x02};
    CHECK(patched_rc(4, badver, 1) == RX_PATH_ERR_MALFORMED);
    static const uint8_t badkind[1] = {0x0B};
    CHECK(patched_rc(5, badkind, 1) == RX_PATH_ERR_MALFORMED);

    /* Decoder limits (spec 6.3). */
    static const uint8_t steps513[2] = {0x02, 0x01}, attr33[2] = {0x00, 0x21}, cons17[2] = {0x00, 0x11};
    static const uint8_t nine[2] = {0x00, 0x09}, p1025[4] = {0x00, 0x00, 0x04, 0x01};
    CHECK(patched_rc(6, steps513, 2) == RX_PATH_ERR_STEP_LIMIT);
    CHECK(patched_rc(off_attr_count, attr33, 2) == RX_PATH_ERR_ATTR_LIMIT);
    CHECK(patched_rc(off_cons_count, cons17, 2) == RX_PATH_ERR_CONST_LIMIT);
    CHECK(patched_rc(off_s1 + 6, nine, 2) == RX_PATH_ERR_INPUT_LIMIT);
    CHECK(patched_rc(off_s1 + 8 + 32, nine, 2) == RX_PATH_ERR_OUTPUT_LIMIT);
    CHECK(patched_rc(off_s1 + 8 + 32 + 2, p1025, 4) == RX_PATH_ERR_PARAM_LIMIT);
    CHECK(decode_rc(buf, RX_PATH_MAX_TOTAL_SERIALIZATION + 1, &id) == RX_PATH_ERR_BUFFER_OVERFLOW);
}

static void limits(void) {
    rx_path_arena_init(&arena);
    SemanticId s = mkid(1), e = mkid(2), c = mkid(3), before, after;
    CHECK(rx_path_init(&pa, &arena, &s, &e, &c) == 0);
    RxPathStep st = simple_step(ROLE_JS_ACTION, 9);
    unsigned ok = 0;
    for (unsigned i = 0; i < 512; i++) ok += rx_path_append(&pa, &st) == RX_PATH_OK;
    CHECK(ok == 512 && rx_path_step_count(&pa) == 512);
    CHECK(rx_path_semantic_id(&pa, &before) == 0);
    uint32_t used = arena.used;
    CHECK(rx_path_append(&pa, &st) == RX_PATH_ERR_STEP_LIMIT);
    CHECK(rx_path_step_count(&pa) == 512 && arena.used == used);
    CHECK(rx_path_semantic_id(&pa, &after) == 0 && same(&before, &after));
    size_t n;
    CHECK(rx_path_encode(&pa, buf, sizeof(buf), &n) == 0);
    CHECK(decode_rc(buf, n, &before) == RX_PATH_OK && rx_path_step_count(&pc) == 512);

    rx_path_arena_init(&arena);
    CHECK(rx_path_init(&pa, &arena, &s, &e, &c) == 0);
    RxPathStep bad = simple_step(ROLE_EXEC_GOAL, 1);
    bad.input_count = 9;
    CHECK(rx_path_append(&pa, &bad) == RX_PATH_ERR_INPUT_LIMIT);
    bad.input_count = 8; bad.output_count = 9;
    CHECK(rx_path_append(&pa, &bad) == RX_PATH_ERR_OUTPUT_LIMIT);
    bad.output_count = 8; bad.param_len = 1025;
    CHECK(rx_path_append(&pa, &bad) == RX_PATH_ERR_PARAM_LIMIT);
    bad.param_len = 1024; bad.step_role = 0x0205;
    CHECK(rx_path_append(&pa, &bad) == RX_PATH_ERR_MALFORMED);
    bad.step_family = 6; bad.step_role = 0x0601;
    CHECK(rx_path_append(&pa, &bad) == RX_PATH_ERR_MALFORMED);
    bad.step_family = STEP_FAM_EXECUTION; bad.step_role = ROLE_EXEC_GOAL;
    CHECK(rx_path_step_count(&pa) == 0 && arena.used == 0);
    unsigned big = 0;
    int rc;
    while ((rc = rx_path_append(&pa, &bad)) == RX_PATH_OK) big++;
    CHECK(rc == RX_PATH_ERR_BUFFER_OVERFLOW && big > 0 && big < 512);
    CHECK(rx_path_encoded_size(&pa, &n) == 0 && n <= RX_PATH_MAX_TOTAL_SERIALIZATION);
    CHECK(rx_path_encode(&pa, buf, 16, &n) == RX_PATH_ERR_BUFFER_OVERFLOW);

    rx_path_arena_init(&arena);
    CHECK(rx_path_init(&pa, &arena, &s, &e, &c) == 0);
    char key[8];
    uint8_t v = 1;
    for (unsigned i = 0; i < 32; i++) {
        snprintf(key, sizeof(key), "a%02u", i);
        CHECK(rx_path_add_attribute(&pa, key, &v, 1) == RX_PATH_OK);
    }
    CHECK(rx_path_add_attribute(&pa, "zz", &v, 1) == RX_PATH_ERR_ATTR_LIMIT);
    CHECK(rx_path_init(&pa, &arena, &s, &e, &c) == 0);
    CHECK(rx_path_add_attribute(&pa, "k", &v, 1) == RX_PATH_OK);
    CHECK(rx_path_add_attribute(&pa, "k", &v, 1) == RX_PATH_ERR_MALFORMED);
    for (unsigned i = 0; i < 16; i++) {
        uint8_t pay = (uint8_t)i;
        CHECK(rx_path_add_constraint(&pa, 1, &pay, 1) == RX_PATH_OK);
    }
    CHECK(rx_path_add_constraint(&pa, 2, &v, 1) == RX_PATH_ERR_CONST_LIMIT);
}

static RxPathStep numbered(unsigned i) {
    RxPathStep s = simple_step(ROLE_JS_ACTION, 200 + i);
    s.input_count = 1; s.input_ids[0] = mkid(300 + i);
    return s;
}

static void fork_prefix(void) {
    rx_path_arena_init(&arena);
    SemanticId s = mkid(1), e = mkid(2), c = mkid(3), pid0, pid, cid, fid, did, eid;
    CHECK(rx_path_init(&pa, &arena, &s, &e, &c) == 0);
    for (unsigned i = 0; i < 10; i++) { RxPathStep t = numbered(i); CHECK(rx_path_append(&pa, &t) == 0); }
    CHECK(rx_path_semantic_id(&pa, &pid0) == 0);
    CHECK(rx_path_fork(&pa, &pa, 3) == RX_PATH_ERR_ARG);
    CHECK(rx_path_fork(&pb, &pa, 11) == RX_PATH_ERR_ARG);
    CHECK(pa.frozen == 0);

    /* Child at K=6 before any tail equals the flat 6-step prefix. */
    CHECK(rx_path_fork(&pb, &pa, 6) == RX_PATH_OK);
    CHECK(rx_path_init(&pf, &arena, &s, &e, &c) == 0);
    for (unsigned i = 0; i < 6; i++) { RxPathStep t = numbered(i); CHECK(rx_path_append(&pf, &t) == 0); }
    CHECK(rx_path_semantic_id(&pb, &cid) == 0 && rx_path_semantic_id(&pf, &fid) == 0 && same(&cid, &fid));

    uint32_t used = arena.used;
    for (unsigned i = 50; i < 53; i++) { RxPathStep t = numbered(i); CHECK(rx_path_append(&pb, &t) == 0); }
    CHECK(arena.used == used + 3); /* prefix shared, never copied */
    CHECK(pb.tail_count == 3 && rx_path_step_count(&pb) == 9);
    for (unsigned i = 50; i < 53; i++) { RxPathStep t = numbered(i); CHECK(rx_path_append(&pf, &t) == 0); }
    CHECK(rx_path_semantic_id(&pb, &cid) == 0 && rx_path_semantic_id(&pf, &fid) == 0 && same(&cid, &fid));
    size_t n1, n2;
    CHECK(rx_path_encode(&pb, buf, sizeof(buf), &n1) == 0 && rx_path_encode(&pf, buf2, sizeof(buf2), &n2) == 0);
    CHECK(n1 == n2 && memcmp(buf, buf2, n1) == 0);
    int eq = 0;
    CHECK(rx_path_equal(&pb, &pf, &eq) == 0 && eq);

    /* Parent frozen after fork (spec 7.1 rule 3). */
    RxPathStep t = numbered(99);
    uint8_t v = 1;
    SemanticId e9 = mkid(9);
    CHECK(pa.frozen == 1);
    CHECK(rx_path_append(&pa, &t) == RX_PATH_ERR_FROZEN);
    CHECK(rx_path_add_attribute(&pa, "x", &v, 1) == RX_PATH_ERR_FROZEN);
    CHECK(rx_path_add_constraint(&pa, 1, &v, 1) == RX_PATH_ERR_FROZEN);
    CHECK(rx_path_set_end_anchor(&pa, &e9) == RX_PATH_ERR_FROZEN);
    CHECK(rx_path_semantic_id(&pa, &pid) == 0 && same(&pid, &pid0) && rx_path_step_count(&pa) == 10);

    /* Sibling isolation. */
    CHECK(rx_path_fork(&pd, &pa, 6) == RX_PATH_OK);
    RxPathStep u = numbered(77);
    CHECK(rx_path_append(&pd, &u) == 0);
    CHECK(rx_path_semantic_id(&pd, &did) == 0 && !same(&did, &cid));
    CHECK(rx_path_semantic_id(&pb, &fid) == 0 && same(&fid, &cid));

    /* Nested fork of the child, then compare with the flat equivalent. */
    CHECK(rx_path_fork(&pe, &pb, 8) == RX_PATH_OK);
    CHECK(rx_path_append(&pb, &u) == RX_PATH_ERR_FROZEN);
    CHECK(rx_path_append(&pe, &u) == 0);
    CHECK(rx_path_init(&pf, &arena, &s, &e, &c) == 0);
    for (unsigned i = 0; i < 6; i++) { RxPathStep w = numbered(i); CHECK(rx_path_append(&pf, &w) == 0); }
    for (unsigned i = 50; i < 52; i++) { RxPathStep w = numbered(i); CHECK(rx_path_append(&pf, &w) == 0); }
    CHECK(rx_path_append(&pf, &u) == 0);
    CHECK(rx_path_semantic_id(&pe, &eid) == 0 && rx_path_semantic_id(&pf, &fid) == 0 && same(&eid, &fid));

    /* A corrupted frozen ancestor is detected, not silently re-identified. */
    uint32_t keep = pa.tail[0];
    pa.tail[0] = pa.tail[1];
    CHECK(rx_path_semantic_id(&pb, &cid) == RX_PATH_ERR_ID_MISMATCH);
    CHECK(rx_path_semantic_id(&pe, &eid) == RX_PATH_ERR_ID_MISMATCH);
    pa.tail[0] = keep;
    CHECK(rx_path_semantic_id(&pe, &eid) == 0 && same(&eid, &fid));
}

/* I1: a capability-family path is recorded evidence. Building, encoding and
 * naming it produce only bytes and ids; the boundary script checks that the
 * module contains no dispatch, authorization or capability-validation code. */
static void no_authority(void) {
    rx_path_arena_init(&arena);
    SemanticId s = mkid(1), e = mkid(2), c = mkid(3), id;
    CHECK(rx_path_init(&pa, &arena, &s, &e, &c) == 0);
    for (uint16_t r = ROLE_CAP_INTENT; r <= ROLE_CAP_EFFECT; r++) {
        RxPathStep t = simple_step(r, r);
        CHECK(rx_path_append(&pa, &t) == 0);
    }
    RxPathStep promo = simple_step(ROLE_JS_CANDIDATE, 1);
    CHECK(rx_path_append(&pa, &promo) == 0);
    CHECK(rx_path_semantic_id(&pa, &id) == 0);
    size_t n;
    CHECK(rx_path_encode(&pa, buf, sizeof(buf), &n) == 0);
    CHECK(pa.frozen == 0 && rx_path_step_count(&pa) == 8);
}

/* Outside-review regressions (2026-09-30): tampered public records, fork into
 * an ancestor, encode over a changed prefix, and equal constraints. */
static void review_regressions(void) {
    rx_path_arena_init(&arena);
    SemanticId s = mkid(1), e = mkid(2), c = mkid(3), id, id2;
    size_t n;
    int eq;
    CHECK(rx_path_init(&pa, &arena, &s, &e, &c) == 0);
    RxPathStep t = simple_step(ROLE_JS_ACTION, 5);
    CHECK(rx_path_append(&pa, &t) == 0);

    /* A stored step changed after insertion is refused by every reader. */
    arena.steps[pa.tail[0]].input_count = 9;
    CHECK(rx_path_encoded_size(&pa, &n) == RX_PATH_ERR_INPUT_LIMIT);
    CHECK(rx_path_encode(&pa, buf, sizeof(buf), &n) == RX_PATH_ERR_INPUT_LIMIT);
    CHECK(rx_path_semantic_id(&pa, &id) == RX_PATH_ERR_INPUT_LIMIT);
    CHECK(rx_path_equal(&pa, &pa, &eq) == RX_PATH_ERR_INPUT_LIMIT);
    arena.steps[pa.tail[0]].input_count = t.input_count;
    arena.steps[pa.tail[0]].param_len = 0xFFFFFFF0u;
    CHECK(rx_path_encode(&pa, buf, sizeof(buf), &n) == RX_PATH_ERR_PARAM_LIMIT);
    arena.steps[pa.tail[0]].param_len = t.param_len;
    pa.attr_count = RX_PATH_MAX_ATTR_COUNT + 1;
    CHECK(rx_path_encode(&pa, buf, sizeof(buf), &n) == RX_PATH_ERR_ATTR_LIMIT);
    pa.attr_count = 0;
    pa.constraint_count = RX_PATH_MAX_CONSTRAINT_COUNT + 1;
    CHECK(rx_path_encode(&pa, buf, sizeof(buf), &n) == RX_PATH_ERR_CONST_LIMIT);
    pa.constraint_count = 0;
    CHECK(rx_path_semantic_id(&pa, &id) == 0);

    /* Forking into the storage of an ancestor is refused; nothing changes. */
    CHECK(rx_path_fork(&pb, &pa, 1) == RX_PATH_OK);
    CHECK(rx_path_fork(&pa, &pb, 1) == RX_PATH_ERR_ARG);
    CHECK(pa.frozen == 1 && pa.parent == NULL && rx_path_step_count(&pa) == 1);
    CHECK(rx_path_semantic_id(&pa, &id2) == 0 && same(&id, &id2));
    CHECK(rx_path_fork(&pc, &pb, 1) == RX_PATH_OK);
    CHECK(rx_path_fork(&pa, &pc, 0) == RX_PATH_ERR_ARG);
    CHECK(rx_path_semantic_id(&pa, &id2) == 0 && same(&id, &id2));

    /* Encode refuses a child whose frozen ancestor changed after the fork. */
    CHECK(rx_path_encode(&pb, buf, sizeof(buf), &n) == RX_PATH_OK);
    pa.start_anchor_id = mkid(77);
    CHECK(rx_path_encode(&pb, buf, sizeof(buf), &n) == RX_PATH_ERR_ID_MISMATCH);
    CHECK(rx_path_encode(&pc, buf, sizeof(buf), &n) == RX_PATH_ERR_ID_MISMATCH);
    pa.start_anchor_id = s;
    CHECK(rx_path_encode(&pc, buf, sizeof(buf), &n) == RX_PATH_OK);

    /* Equal constraints are kept (canonical-encoding rule 3), sorted, and
     * survive a round trip; builder order still does not matter. */
    uint8_t p1 = 1, p2 = 2;
    CHECK(rx_path_init(&pd, &arena, &s, &e, &c) == 0);
    CHECK(rx_path_add_constraint(&pd, 4, &p2, 1) == 0);
    CHECK(rx_path_add_constraint(&pd, 4, &p1, 1) == 0);
    CHECK(rx_path_add_constraint(&pd, 4, &p2, 1) == 0);
    CHECK(pd.constraint_count == 3 && pd.constraints[0].payload[0] == 1 &&
          pd.constraints[1].payload[0] == 2 && pd.constraints[2].payload[0] == 2);
    CHECK(rx_path_init(&pe, &arena, &s, &e, &c) == 0);
    CHECK(rx_path_add_constraint(&pe, 4, &p2, 1) == 0);
    CHECK(rx_path_add_constraint(&pe, 4, &p2, 1) == 0);
    CHECK(rx_path_add_constraint(&pe, 4, &p1, 1) == 0);
    CHECK(rx_path_semantic_id(&pd, &id) == 0 && rx_path_semantic_id(&pe, &id2) == 0 && same(&id, &id2));
    CHECK(rx_path_encode(&pd, buf, sizeof(buf), &n) == 0);
    CHECK(decode_rc(buf, n, &id) == RX_PATH_OK && pc.constraint_count == 3);
    eq = 0;
    CHECK(rx_path_equal(&pc, &pd, &eq) == 0 && eq);
    /* Two equal constraints differ in identity from one. */
    CHECK(rx_path_init(&pf, &arena, &s, &e, &c) == 0);
    CHECK(rx_path_add_constraint(&pf, 4, &p1, 1) == 0 && rx_path_add_constraint(&pf, 4, &p2, 1) == 0);
    CHECK(rx_path_semantic_id(&pf, &id2) == 0 && !same(&id, &id2));
}

int main(void) {
    known_answer();
    randomized();
    order_sensitivity();
    realization_separation();
    tamper_and_canonical();
    limits();
    fork_prefix();
    no_authority();
    review_regressions();
    if (failures) {
        printf("PATH-1: %u of %u checks FAILED\n", failures, checks);
        return 1;
    }
    printf("PATH-1: PASS, %u checks\n", checks);
    return 0;
}
