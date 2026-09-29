/* Parity-oracle tests for the Omega mixed algebra (src/algebra/).
 * Every op is compared against a naive integer oracle written independently
 * of the bitplane formulas. Fixed seeds: runs are reproducible. */
#include "algebra/oma_pack.h"
#include "algebra/oma_quant.h"
#include "algebra/oma_trit.h"
#include "algebra/oma_z3.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned long long g_checks, g_fail, s_checks, s_fail;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        s_checks++;                                                             \
        if (!(cond)) {                                                          \
            if (s_fail++ < 20) {                                                \
                fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__);          \
                fprintf(stderr, __VA_ARGS__);                                   \
                fputc('\n', stderr);                                            \
            }                                                                   \
        }                                                                       \
    } while (0)

static void section_begin(void) { s_checks = s_fail = 0; }
static void section_end(const char *name) {
    printf("%s %-28s %12llu checks, %llu failures\n", s_fail ? "FAIL" : "PASS", name, s_checks, s_fail);
    g_checks += s_checks;
    g_fail += s_fail;
}

/* splitmix64 */
static uint64_t g_rng = 0x0A1E0B1E5EEDull;
static uint64_t rnd(void) {
    uint64_t z = (g_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static int rnd_trit(void) { return (int)(rnd() % 3u) - 1; }

/* ---- naive oracles ---- */
static int code_val(unsigned c) { return (int)(c & 1u) - (int)((c >> 1) & 1u); }
static void naive_add(int a, int b, int *s, int *c) {
    int v = a + b;
    *c = v > 1 ? 1 : (v < -1 ? -1 : 0);
    *s = v - 3 * *c;
}
static void random_valid_block(oma_block *b) {
    uint64_t p = rnd(), n = rnd();
    switch (rnd() % 4u) { /* vary density */
    case 0: p &= rnd(); n &= rnd(); break;
    case 1: p = 0; break;
    case 2: n = 0; break;
    default: break;
    }
    b->pos = p & ~n;
    b->neg = n;
}
static int lane(const oma_block *b, int i) { return (int)((b->pos >> i) & 1u) - (int)((b->neg >> i) & 1u); }

/* ---------------------------------------------------------------- */
static void test_constructors(void) {
    section_begin();
    for (int v = -300; v <= 300; v++) {
        oma_trit t = 7;
        int rc = oma_trit_make(v, &t);
        CHECK((v >= -1 && v <= 1) ? (rc == OMA_OK && t == v) : (rc == OMA_E_INVALID_TRIT && t == 7), "make %d", v);
        oma_z3 z = 9;
        rc = oma_z3_make(v, &z);
        CHECK((v >= 0 && v <= 2) ? (rc == OMA_OK && z == v) : (rc == OMA_E_INVALID_Z3), "z3 make %d", v);
    }
    for (unsigned c = 0; c < 256; c++) {
        oma_trit t;
        int rc = oma_code_to_trit((uint8_t)c, &t);
        CHECK(c <= 2 ? (rc == OMA_OK && t == code_val(c)) : rc == OMA_E_INVALID_CODE, "code %u", c);
    }
    for (int v = -128; v <= 127; v++) {
        uint8_t c = 99;
        int rc = oma_trit_to_code((oma_trit)v, &c);
        if (v >= -1 && v <= 1) CHECK(rc == OMA_OK && code_val(c) == v && c != OMA_CODE_INVALID, "to_code %d", v);
        else CHECK(rc == OMA_E_INVALID_TRIT, "to_code %d", v);
    }
    /* Encoding table (H1) pinned literally. */
    uint8_t c;
    CHECK(oma_trit_to_code(-1, &c) == OMA_OK && c == 2u, "-1 -> (pos0,neg1)");
    CHECK(oma_trit_to_code(0, &c) == OMA_OK && c == 0u, "0 -> (0,0)");
    CHECK(oma_trit_to_code(1, &c) == OMA_OK && c == 1u, "+1 -> (1,0)");
    CHECK(oma_trit_make(0, NULL) == OMA_E_ARG, "null out");
    section_end("constructors+codes");
}

static void test_trit_truth_tables(void) {
    section_begin();
    /* literal balanced add table: a,b -> sum,carry */
    static const int add_tab[3][3][2] = {
        /* a=-1 */ {{1, -1}, {-1, 0}, {0, 0}},
        /* a= 0 */ {{-1, 0}, {0, 0}, {1, 0}},
        /* a=+1 */ {{0, 0}, {1, 0}, {-1, 1}},
    };
    static const int mul_tab[3][3] = {{1, 0, -1}, {0, 0, 0}, {-1, 0, 1}};
    const unsigned codes[] = {0, 1, 2, 3, 4, 255};
    for (size_t i = 0; i < 6; i++) {
        unsigned a = codes[i];
        uint8_t r = 77;
        int rc = oma_code_neg((uint8_t)a, &r);
        if (a <= 2) CHECK(rc == OMA_OK && code_val(r) == -code_val(a) && r != 3, "neg %u", a);
        else CHECK(rc == OMA_E_INVALID_CODE && r == 77, "neg invalid %u", a);
        for (size_t j = 0; j < 6; j++) {
            unsigned b = codes[j];
            uint8_t s = 77, cy = 77, m = 77;
            int rca = oma_code_add((uint8_t)a, (uint8_t)b, &s, &cy);
            int rcm = oma_code_mul((uint8_t)a, (uint8_t)b, &m);
            if (a <= 2 && b <= 2) {
                int va = code_val(a), vb = code_val(b), ns, nc;
                naive_add(va, vb, &ns, &nc);
                CHECK(rca == OMA_OK && s <= 2 && cy <= 2 && code_val(s) == ns && code_val(cy) == nc, "add %u %u", a, b);
                CHECK(code_val(s) == add_tab[va + 1][vb + 1][0] && code_val(cy) == add_tab[va + 1][vb + 1][1], "add table %d %d", va, vb);
                CHECK(va + vb == code_val(s) + 3 * code_val(cy), "add identity");
                CHECK(rcm == OMA_OK && m <= 2 && code_val(m) == va * vb && va * vb == mul_tab[va + 1][vb + 1], "mul %u %u", a, b);
            } else {
                CHECK(rca == OMA_E_INVALID_CODE && s == 77 && cy == 77, "add invalid %u %u", a, b);
                CHECK(rcm == OMA_E_INVALID_CODE && m == 77, "mul invalid %u %u", a, b);
            }
        }
    }
    /* value-level ops, including invalid int8 values */
    for (int a = -3; a <= 3; a++) {
        oma_trit r;
        int rc = oma_trit_neg((oma_trit)a, &r);
        CHECK((a >= -1 && a <= 1) ? (rc == OMA_OK && r == -a) : rc == OMA_E_INVALID_TRIT, "trit neg %d", a);
        for (int b = -3; b <= 3; b++) {
            oma_trit s, cy, m;
            int ok = a >= -1 && a <= 1 && b >= -1 && b <= 1;
            int rca = oma_trit_add((oma_trit)a, (oma_trit)b, &s, &cy);
            int rcm = oma_trit_mul((oma_trit)a, (oma_trit)b, &m);
            if (ok) {
                int ns, nc;
                naive_add(a, b, &ns, &nc);
                CHECK(rca == OMA_OK && s == ns && cy == nc, "trit add %d %d", a, b);
                CHECK(rcm == OMA_OK && m == a * b, "trit mul %d %d", a, b);
            } else {
                CHECK(rca == OMA_E_INVALID_TRIT && rcm == OMA_E_INVALID_TRIT, "trit invalid %d %d", a, b);
            }
        }
    }
    section_end("per-trit truth tables");
}

/* Any op given a block with an overlapping lane must return an error. */
static void test_invalid_blocks(void) {
    section_begin();
    oma_block good = {0, 0};
    for (int i = 0; i < 20000; i++) {
        oma_block bad, out, out2;
        random_valid_block(&bad);
        int bit = (int)(rnd() % 64u);
        bad.pos |= 1ull << bit;
        bad.neg |= 1ull << bit; /* lane `bit` is now (1,1) */
        int8_t v[64];
        int32_t d;
        uint8_t ser[16];
        oma_z3_block zb = {bad.pos, bad.neg}, zo = {0, 0};
        CHECK(oma_block_validate(&bad) == OMA_E_INVALID_PLANES, "validate");
        CHECK(oma_block_decode(&bad, v) == OMA_E_INVALID_PLANES, "decode");
        CHECK(oma_block_neg(&bad, &out) == OMA_E_INVALID_PLANES, "neg");
        CHECK(oma_block_add(&bad, &good, &out, &out2) == OMA_E_INVALID_PLANES, "add a");
        CHECK(oma_block_add(&good, &bad, &out, &out2) == OMA_E_INVALID_PLANES, "add b");
        CHECK(oma_block_mul(&bad, &good, &out) == OMA_E_INVALID_PLANES, "mul a");
        CHECK(oma_block_mul(&good, &bad, &out) == OMA_E_INVALID_PLANES, "mul b");
        CHECK(oma_block_dot(&bad, &good, &d) == OMA_E_INVALID_PLANES, "dot a");
        CHECK(oma_block_dot(&good, &bad, &d) == OMA_E_INVALID_PLANES, "dot b");
        memset(v, 0, sizeof v);
        CHECK(oma_dot_tw_i8(&bad, v, 64, &d) == OMA_E_INVALID_PLANES, "tw_i8");
        CHECK(oma_block_serialize(&bad, ser) == OMA_E_INVALID_PLANES, "serialize");
        CHECK(oma_unpack_bitplane(&bad, 1, 64, v) == OMA_E_INVALID_PLANES, "unpack bitplane");
        for (int k = 0; k < 8; k++) {
            ser[k] = (uint8_t)(bad.pos >> (8 * k));
            ser[8 + k] = (uint8_t)(bad.neg >> (8 * k));
        }
        CHECK(oma_block_deserialize(ser, &out) == OMA_E_INVALID_PLANES, "deserialize");
        CHECK(oma_z3_block_validate(&zb) == OMA_E_INVALID_PLANES, "z3 validate");
        CHECK(oma_z3_block_add(&zb, &zo, &zo) == OMA_E_INVALID_PLANES, "z3 add");
        CHECK(oma_z3_block_mul(&zb, &zb, &zo) == OMA_E_INVALID_PLANES, "z3 mul");
        CHECK(oma_z3_block_neg(&zb, &zo) == OMA_E_INVALID_PLANES, "z3 neg");
        CHECK(oma_z3_block_to_trits(&zb, &out) == OMA_E_INVALID_PLANES, "z3 to trits");
        CHECK(oma_z3_block_from_trits(&bad, &zo) == OMA_E_INVALID_PLANES, "z3 from trits");
    }
    int8_t in[64] = {0};
    oma_block b;
    for (int v = -128; v <= 127; v++) {
        in[17] = (int8_t)v;
        int rc = oma_block_encode(in, &b);
        CHECK((v >= -1 && v <= 1) ? rc == OMA_OK : rc == OMA_E_INVALID_TRIT, "encode %d", v);
    }
    section_end("invalid (1,1) rejection");
}

/* Exhaustive: all 3^k x 3^k vector pairs for k = 1..6. */
static void vec_from_index(unsigned idx, int k, int8_t v[64]) {
    memset(v, 0, 64);
    for (int i = 0; i < k; i++) {
        v[i] = (int8_t)((int)(idx % 3u) - 1);
        idx /= 3u;
    }
}

static void test_exhaustive_small(void) {
    section_begin();
    static int8_t vecs[729][64];
    static oma_block blks[729];
    static int8_t acts[4][64];
    for (int i = 0; i < 64; i++) {
        acts[0][i] = -128;
        acts[1][i] = 127;
        acts[2][i] = (int8_t)(rnd() & 0xFF);
        acts[3][i] = (int8_t)((i & 1) ? -128 : 127);
    }
    for (int k = 1; k <= 6; k++) {
        unsigned nv = 1;
        for (int i = 0; i < k; i++) nv *= 3u;
        for (unsigned i = 0; i < nv; i++) {
            vec_from_index(i, k, vecs[i]);
            CHECK(oma_block_encode(vecs[i], &blks[i]) == OMA_OK, "encode");
        }
        for (unsigned ia = 0; ia < nv; ia++) {
            const int8_t *a = vecs[ia];
            const oma_block *A = &blks[ia];
            int8_t dec[64];
            oma_block N;
            CHECK(oma_block_decode(A, dec) == OMA_OK && memcmp(dec, a, 64) == 0, "decode k=%d", k);
            CHECK(oma_block_neg(A, &N) == OMA_OK, "neg");
            for (int i = 0; i < 64; i++) CHECK(lane(&N, i) == -a[i], "neg lane");

            /* dense + bitplane pack round trip */
            uint8_t dense[2];
            size_t dl;
            int8_t back[64];
            CHECK(oma_pack_dense(a, (size_t)k, dense, sizeof dense, &dl) == OMA_OK && dl == oma_dense_bytes((size_t)k), "dense pack");
            CHECK(oma_unpack_dense(dense, dl, (size_t)k, back) == OMA_OK && memcmp(back, a, (size_t)k) == 0, "dense rt");
            {
                unsigned exp = 0, p = 1; /* naive: (t_i+1)*3^i */
                for (int i = 0; i < 5 && i < k; i++, p *= 3u) exp += (unsigned)(a[i] + 1) * p;
                for (int i = k; i < 5; i++, p *= 3u) exp += p;
                CHECK(dense[0] == exp && dense[0] < 243, "dense byte value");
            }
            oma_block pb;
            CHECK(oma_pack_bitplane(a, (size_t)k, &pb, 1) == OMA_OK && pb.pos == A->pos && pb.neg == A->neg, "bitplane pack");
            CHECK(oma_unpack_bitplane(&pb, 1, (size_t)k, back) == OMA_OK && memcmp(back, a, (size_t)k) == 0, "bitplane rt");

            /* digits -> int -> canonical digits */
            int64_t iv, nav = 0;
            for (int i = k - 1; i >= 0; i--) nav = nav * 3 + a[i];
            CHECK(oma_bt_to_int(a, (size_t)k, &iv) == OMA_OK && iv == nav, "bt_to_int");
            int8_t dg[OMA_INT64_MAX_DIGITS];
            size_t nd;
            CHECK(oma_int_to_bt(iv, dg, sizeof dg, &nd) == OMA_OK, "int_to_bt");
            int top = k - 1;
            while (top >= 0 && a[top] == 0) top--;
            CHECK(nd == (size_t)(top + 1) && memcmp(dg, a, nd) == 0, "canonical rt");

            /* ternary-weight x int8 activation dot */
            for (int s = 0; s < 4; s++) {
                int32_t d = 0, nd2 = 0;
                for (int i = 0; i < k; i++) nd2 += a[i] * acts[s][i];
                CHECK(oma_dot_tw_i8(A, acts[s], (size_t)k, &d) == OMA_OK && d == nd2, "tw_i8 k=%d", k);
            }

            for (unsigned ib = 0; ib < nv; ib++) {
                const int8_t *b = vecs[ib];
                const oma_block *B = &blks[ib];
                oma_block S, C, M;
                int32_t d;
                CHECK(oma_block_add(A, B, &S, &C) == OMA_OK && oma_block_validate(&S) == OMA_OK && oma_block_validate(&C) == OMA_OK, "add");
                CHECK(oma_block_mul(A, B, &M) == OMA_OK && oma_block_validate(&M) == OMA_OK, "mul");
                int nd3 = 0;
                for (int i = 0; i < k; i++) {
                    int ns, nc;
                    naive_add(a[i], b[i], &ns, &nc);
                    CHECK(lane(&S, i) == ns && lane(&C, i) == nc, "add lane");
                    CHECK(lane(&M, i) == a[i] * b[i], "mul lane");
                    nd3 += a[i] * b[i];
                }
                CHECK(((S.pos | S.neg | C.pos | C.neg | M.pos | M.neg) >> k) == 0, "padding lanes stay zero");
                CHECK(oma_block_dot(A, B, &d) == OMA_OK && d == nd3, "dot");

                /* Z3 on the same vectors via the 2 <-> -1 map */
                oma_z3_block za, zbb, zs, zm;
                CHECK(oma_z3_block_from_trits(A, &za) == OMA_OK && oma_z3_block_from_trits(B, &zbb) == OMA_OK, "z3 map");
                CHECK(oma_z3_block_add(&za, &zbb, &zs) == OMA_OK && oma_z3_block_mul(&za, &zbb, &zm) == OMA_OK, "z3 ops");
                uint8_t zsv[64], zmv[64];
                oma_z3_block_decode(&zs, zsv);
                oma_z3_block_decode(&zm, zmv);
                for (int i = 0; i < k; i++) {
                    unsigned ua = (unsigned)(a[i] < 0 ? 2 : a[i]), ub = (unsigned)(b[i] < 0 ? 2 : b[i]);
                    CHECK(zsv[i] == (ua + ub) % 3u && zmv[i] == (ua * ub) % 3u, "z3 lane");
                }
            }
        }
    }
    section_end("exhaustive 3^k (k<=6)");
}

static void test_dense_bytes(void) {
    section_begin();
    for (unsigned byte = 0; byte < 256; byte++) {
        int8_t t[5] = {9, 9, 9, 9, 9}, big[5];
        int rc = oma_dense_byte_decode((uint8_t)byte, t);
        if (byte < 243) {
            unsigned v = byte;
            int ok = rc == OMA_OK;
            for (int j = 0; j < 5; j++, v /= 3u) ok = ok && t[j] == (int)(v % 3u) - 1;
            CHECK(ok, "decode byte %u", byte);
            uint8_t re;
            size_t len;
            CHECK(oma_pack_dense(t, 5, &re, 1, &len) == OMA_OK && len == 1 && re == byte, "repack %u", byte);
            uint8_t b8 = (uint8_t)byte;
            CHECK(oma_unpack_dense(&b8, 1, 5, big) == OMA_OK && memcmp(big, t, 5) == 0, "unpack5 %u", byte);
            /* shorter n: padding trits must be 0 (digit value 1) */
            for (size_t n = 1; n < 5; n++) {
                int pad_ok = 1;
                for (size_t j = n; j < 5; j++) pad_ok = pad_ok && t[j] == 0;
                int rc2 = oma_unpack_dense(&b8, 1, n, big);
                CHECK(pad_ok ? rc2 == OMA_OK : rc2 == OMA_E_INVALID_BYTE, "padding %u n=%zu", byte, n);
            }
        } else {
            CHECK(rc == OMA_E_INVALID_BYTE, "reject byte %u", byte);
            uint8_t buf[3] = {121, (uint8_t)byte, 121};
            int8_t out[15];
            CHECK(oma_unpack_dense(buf, 3, 15, out) == OMA_E_INVALID_BYTE, "reject in stream %u", byte);
        }
    }
    section_end("dense bytes (all 256)");
}

static void test_int_conversion(void) {
    section_begin();
    /* all int16 vs an independent offset-base-3 oracle, n = 11 digits */
    const int64_t H = (177147 - 1) / 2; /* (3^11 - 1)/2 */
    for (int32_t v = INT16_MIN; v <= INT16_MAX; v++) {
        int8_t dg[OMA_INT64_MAX_DIGITS], fx[11], orc[11];
        size_t nd;
        int64_t u = v + H, back;
        for (int i = 0; i < 11; i++, u /= 3) orc[i] = (int8_t)(u % 3 - 1);
        CHECK(oma_int_to_bt(v, dg, sizeof dg, &nd) == OMA_OK, "to_bt %d", v);
        CHECK(nd == 0 ? v == 0 : dg[nd - 1] != 0, "canonical %d", v);
        CHECK(memcmp(dg, orc, nd) == 0, "oracle digits %d", v);
        for (size_t i = nd; i < 11; i++) CHECK(orc[i] == 0, "oracle high zero %d", v);
        CHECK(oma_bt_to_int(dg, nd, &back) == OMA_OK && back == v, "rt %d", v);
        CHECK(oma_int_to_bt_fixed(v, fx, 11) == OMA_OK && memcmp(fx, orc, 11) == 0, "fixed %d", v);
        if (nd > 0) CHECK(oma_int_to_bt_fixed(v, fx, nd - 1) == OMA_E_OVERFLOW, "fixed overflow %d", v);
    }
    /* boundaries: (3^k-1)/2 needs k digits, +1 needs k+1 */
    int64_t m = 0; /* m_k = (3^k - 1)/2 = 3*m_{k-1} + 1, fits int64 for k <= 40 */
    for (int k = 1; k <= 40; k++) {
        m = 3 * m + 1;
        int8_t dg[OMA_INT64_MAX_DIGITS];
        size_t nd;
        CHECK(oma_int_to_bt(m, dg, sizeof dg, &nd) == OMA_OK && nd == (size_t)k, "max k=%d", k);
        for (size_t i = 0; i < nd; i++) CHECK(dg[i] == 1, "all ones k=%d", k);
        CHECK(oma_int_to_bt(-m, dg, sizeof dg, &nd) == OMA_OK && nd == (size_t)k, "min k=%d", k);
        CHECK(oma_int_to_bt(m + 1, dg, sizeof dg, &nd) == OMA_OK && nd == (size_t)k + 1, "max+1 k=%d", k);
        CHECK(oma_int_to_bt(m + 1, dg, (size_t)k, &nd) == OMA_E_OVERFLOW, "cap overflow k=%d", k);
    }
    /* int64 extremes and random */
    const int64_t fixed_vals[] = {INT64_MIN, INT64_MIN + 1, INT64_MAX, INT64_MAX - 1, -1, 0, 1};
    for (size_t i = 0; i < sizeof fixed_vals / sizeof fixed_vals[0] + 1000000; i++) {
        int64_t v = i < 7 ? fixed_vals[i] : (int64_t)rnd();
        if (i >= 7 && (i & 3) == 0) v >>= (rnd() % 63); /* spread magnitudes */
        int8_t dg[OMA_INT64_MAX_DIGITS];
        size_t nd;
        int64_t back;
        CHECK(oma_int_to_bt(v, dg, sizeof dg, &nd) == OMA_OK && nd <= 41, "i64 to_bt %" PRId64, v);
        CHECK(nd == 0 ? v == 0 : dg[nd - 1] != 0, "i64 canonical");
        CHECK(oma_bt_to_int(dg, nd, &back) == OMA_OK && back == v, "i64 rt %" PRId64, v);
    }
    /* overflow and invalid digits on decode */
    int8_t ones[41], dg2[3] = {1, 2, 0};
    int64_t out;
    memset(ones, 1, sizeof ones);
    CHECK(oma_bt_to_int(ones, 41, &out) == OMA_E_OVERFLOW, "41 ones overflow");
    memset(ones, -1, sizeof ones);
    CHECK(oma_bt_to_int(ones, 41, &out) == OMA_E_OVERFLOW, "41 minus ones overflow");
    CHECK(oma_bt_to_int(dg2, 3, &out) == OMA_E_INVALID_TRIT, "digit 2 rejected");
    int8_t lead0[5] = {1, 0, 0, 0, 0};
    CHECK(oma_bt_to_int(lead0, 5, &out) == OMA_OK && out == 1, "leading zeros accepted");
    section_end("integer <-> balanced ternary");
}

static void test_random_blocks(void) {
    section_begin();
    for (int it = 0; it < 1000000; it++) {
        oma_block A, B, S, C, M, N;
        int32_t d;
        random_valid_block(&A);
        random_valid_block(&B);
        CHECK(oma_block_add(&A, &B, &S, &C) == OMA_OK, "add");
        CHECK(oma_block_mul(&A, &B, &M) == OMA_OK, "mul");
        CHECK(oma_block_neg(&A, &N) == OMA_OK, "neg");
        CHECK(oma_block_dot(&A, &B, &d) == OMA_OK, "dot");
        int nd = 0, bad = 0;
        for (int i = 0; i < 64; i++) {
            int a = lane(&A, i), b = lane(&B, i), ns, nc;
            naive_add(a, b, &ns, &nc);
            bad |= lane(&S, i) != ns || lane(&C, i) != nc || lane(&M, i) != a * b || lane(&N, i) != -a;
            nd += a * b;
        }
        CHECK(!bad && d == nd, "random block lanes it=%d", it);
        CHECK(!(S.pos & S.neg) && !(C.pos & C.neg) && !(M.pos & M.neg), "outputs valid");
    }
    /* multi-block ternary x int8 dot, random length */
    static oma_block W[8];
    static int8_t T[512], X[512];
    for (int it = 0; it < 100000; it++) {
        size_t n = (size_t)(rnd() % 513u);
        for (size_t i = 0; i < n; i++) {
            T[i] = (int8_t)rnd_trit();
            X[i] = (int8_t)(rnd() & 0xFF);
        }
        CHECK(oma_pack_bitplane(T, n, W, 8) == OMA_OK, "pack");
        int32_t d, nd = 0;
        for (size_t i = 0; i < n; i++) nd += T[i] * X[i];
        CHECK(oma_dot_tw_i8(W, X, n, &d) == OMA_OK && d == nd, "tw_i8 n=%zu", n);
        if (n % 64) { /* stray lane beyond n is rejected */
            oma_block keep = W[n / 64];
            W[n / 64].pos |= 1ull << 63;
            W[n / 64].neg &= ~(1ull << 63);
            CHECK(oma_dot_tw_i8(W, X, n, &d) == OMA_E_ARG, "stray lane");
            W[n / 64] = keep;
        }
    }
    int32_t d;
    CHECK(oma_dot_tw_i8(W, X, OMA_DOT_I8_MAX_N + 1, &d) == OMA_E_OVERFLOW, "n bound");
    CHECK(oma_dot_tw_i8(NULL, NULL, 0, &d) == OMA_OK && d == 0, "empty dot");
    section_end("random block properties");
}

static void test_pack_random(void) {
    section_begin();
    static int8_t t[1000], back[1000];
    static uint8_t dense[200];
    static oma_block blk[16];
    for (int it = 0; it < 20000; it++) {
        size_t n = (size_t)(rnd() % 1001u), dl;
        for (size_t i = 0; i < n; i++) t[i] = (int8_t)rnd_trit();
        CHECK(oma_pack_dense(t, n, dense, sizeof dense, &dl) == OMA_OK && dl == (n + 4) / 5, "dense pack");
        CHECK(oma_unpack_dense(dense, dl, n, back) == OMA_OK && memcmp(back, t, n) == 0, "dense rt");
        size_t nb = oma_bitplane_blocks(n);
        CHECK(oma_pack_bitplane(t, n, blk, 16) == OMA_OK, "bp pack");
        for (size_t k = 0; k < nb; k++) {
            uint8_t ser[16];
            oma_block rt;
            CHECK(oma_block_serialize(&blk[k], ser) == OMA_OK && oma_block_deserialize(ser, &rt) == OMA_OK &&
                      rt.pos == blk[k].pos && rt.neg == blk[k].neg, "serialize rt");
        }
        CHECK(oma_unpack_bitplane(blk, nb, n, back) == OMA_OK && memcmp(back, t, n) == 0, "bp rt");
        if (n) {
            size_t j = (size_t)(rnd() % n);
            int8_t keep = t[j];
            t[j] = (int8_t)(rnd() & 1 ? 2 : -2);
            CHECK(oma_pack_dense(t, n, dense, sizeof dense, &dl) == OMA_E_INVALID_TRIT, "dense rejects bad trit");
            CHECK(oma_pack_bitplane(t, n, blk, 16) == OMA_E_INVALID_TRIT, "bp rejects bad trit");
            t[j] = keep;
            if ((n + 4) / 5 > 1) CHECK(oma_pack_dense(t, n, dense, (n + 4) / 5 - 1, &dl) == OMA_E_OVERFLOW, "cap");
        }
    }
    section_end("pack/unpack random");
}

static void test_z3(void) {
    section_begin();
    for (int a = 0; a < 256; a++) {
        oma_z3 r;
        oma_trit t;
        if (a <= 2) {
            CHECK(oma_z3_neg((oma_z3)a, &r) == OMA_OK && r == (3 - a) % 3, "neg");
            CHECK(oma_z3_to_trit((oma_z3)a, &t) == OMA_OK && t == (a == 2 ? -1 : a), "to_trit");
            CHECK(oma_trit_to_z3(t, &r) == OMA_OK && r == a, "map rt");
        } else {
            CHECK(oma_z3_neg((oma_z3)a, &r) == OMA_E_INVALID_Z3, "neg invalid");
            CHECK(oma_z3_to_trit((oma_z3)a, &t) == OMA_E_INVALID_Z3, "to_trit invalid");
        }
        for (int b = 0; b < 256; b++) {
            oma_z3 s, m;
            int rca = oma_z3_add((oma_z3)a, (oma_z3)b, &s), rcm = oma_z3_mul((oma_z3)a, (oma_z3)b, &m);
            if (a <= 2 && b <= 2) {
                CHECK(rca == OMA_OK && s == (a + b) % 3 && rcm == OMA_OK && m == (a * b) % 3, "z3 %d %d", a, b);
                /* isomorphism: map(a+b) = balanced sum trit; map(a*b) = trit product */
                oma_trit ta, tb, ts, tc, tm, ms, mm;
                oma_z3_to_trit((oma_z3)a, &ta);
                oma_z3_to_trit((oma_z3)b, &tb);
                oma_trit_add(ta, tb, &ts, &tc);
                oma_trit_mul(ta, tb, &tm);
                oma_z3_to_trit(s, &ms);
                oma_z3_to_trit(m, &mm);
                CHECK(ms == ts && mm == tm, "isomorphism %d %d", a, b);
            } else {
                CHECK(rca == OMA_E_INVALID_Z3 && rcm == OMA_E_INVALID_Z3, "z3 invalid %d %d", a, b);
            }
        }
    }
    for (int t = -128; t <= 127; t++) {
        oma_z3 r;
        if (t < -1 || t > 1) CHECK(oma_trit_to_z3((oma_trit)t, &r) == OMA_E_INVALID_TRIT, "trit_to_z3 %d", t);
    }
    /* random Z3 blocks vs mod-3 oracle */
    for (int it = 0; it < 200000; it++) {
        uint8_t va[64], vb[64], vs[64], vm[64], vn[64], rt[64];
        for (int i = 0; i < 64; i++) {
            va[i] = (uint8_t)(rnd() % 3u);
            vb[i] = (uint8_t)(rnd() % 3u);
        }
        oma_z3_block A, B, S, M, N;
        CHECK(oma_z3_block_encode(va, &A) == OMA_OK && oma_z3_block_encode(vb, &B) == OMA_OK, "enc");
        CHECK(oma_z3_block_decode(&A, rt) == OMA_OK && memcmp(rt, va, 64) == 0, "dec rt");
        CHECK(oma_z3_block_add(&A, &B, &S) == OMA_OK && oma_z3_block_mul(&A, &B, &M) == OMA_OK &&
                  oma_z3_block_neg(&A, &N) == OMA_OK, "ops");
        oma_z3_block_decode(&S, vs);
        oma_z3_block_decode(&M, vm);
        oma_z3_block_decode(&N, vn);
        int bad = 0;
        for (int i = 0; i < 64; i++)
            bad |= vs[i] != (va[i] + vb[i]) % 3 || vm[i] != (va[i] * vb[i]) % 3 || vn[i] != (3 - va[i]) % 3;
        CHECK(!bad, "z3 block lanes");
    }
    uint8_t badv[64] = {0};
    oma_z3_block zb;
    badv[5] = 3;
    CHECK(oma_z3_block_encode(badv, &zb) == OMA_E_INVALID_Z3, "z3 encode rejects 3");
    section_end("Z3");
}

static void test_quant(void) {
    section_begin();
    int8_t q[1024], q2[1024];
    float s, s2;
    double e;
    {
        const float w[4] = {0.5f, -1.5f, 0.0f, 2.0f};
        const int8_t exp[4] = {1, -1, 0, 1}; /* 0.5 rounds half away from zero */
        CHECK(oma_quant_absmean(w, 4, q, &s) == OMA_OK && s == 1.0f && memcmp(q, exp, 4) == 0, "known vec 1");
        CHECK(oma_quant_rel_l2(w, q, s, 4, &e) == OMA_OK && fabs(e - sqrt(1.5 / 6.5)) < 1e-12, "known err 1 %.17g", e);
    }
    {
        const float w[4] = {0.1f, 0.2f, 0.3f, -0.4f};
        const int8_t exp[4] = {0, 1, 1, -1};
        double m = (fabs((double)0.1f) + fabs((double)0.2f) + fabs((double)0.3f) + fabs((double)-0.4f)) / 4.0;
        CHECK(oma_quant_absmean(w, 4, q, &s) == OMA_OK && s == (float)m && memcmp(q, exp, 4) == 0, "known vec 2");
    }
    {
        const float z[3] = {0, 0, 0};
        CHECK(oma_quant_absmean(z, 3, q, &s) == OMA_OK && s == 0.0f && !q[0] && !q[1] && !q[2], "zero input");
        CHECK(oma_quant_rel_l2(z, q, s, 3, &e) == OMA_OK && e == 0.0, "zero err");
        float bad[2] = {1.0f, NAN};
        CHECK(oma_quant_absmean(bad, 2, q, &s) == OMA_E_ARG, "nan rejected");
        bad[1] = INFINITY;
        CHECK(oma_quant_absmean(bad, 2, q, &s) == OMA_E_ARG, "inf rejected");
        int8_t badq[1] = {2};
        CHECK(oma_quant_rel_l2(z, badq, 1.0f, 1, &e) == OMA_E_INVALID_TRIT, "err rejects bad trit");
    }
    static float w[1024], w2[1024];
    for (int it = 0; it < 20000; it++) {
        size_t n = 1 + (size_t)(rnd() % 1024u);
        /* (a) already-ternary input: q == input, scale == fraction non-zero */
        size_t nnz = 0;
        for (size_t i = 0; i < n; i++) {
            int t = rnd_trit();
            w[i] = (float)t;
            nnz += t != 0;
        }
        CHECK(oma_quant_absmean(w, n, q, &s) == OMA_OK, "q");
        int same = 1;
        for (size_t i = 0; i < n; i++) same &= q[i] == (int8_t)w[i];
        CHECK(same, "idempotent on ternary it=%d", it);
        CHECK(s == (float)((double)nnz / (double)n), "ternary scale");
        if (nnz == n) CHECK(oma_quant_rel_l2(w, q, s, n, &e) == OMA_OK && e == 0.0, "exact when dense");
        /* (b) random floats: scale == mean|w|; re-quantizing q*scale gives q */
        float amp = (float)((rnd() % 1000u) + 1u) / 100.0f;
        double sum = 0.0;
        for (size_t i = 0; i < n; i++) {
            w[i] = amp * ((float)(int64_t)(rnd() >> 40) / (float)(1 << 23) - 1.0f);
            sum += fabs((double)w[i]);
        }
        CHECK(oma_quant_absmean(w, n, q, &s) == OMA_OK, "q2");
        CHECK(s == (float)(sum / (double)n), "scale = mean|w|");
        int ok = 1;
        for (size_t i = 0; i < n && s > 0; i++) {
            double r = round((double)w[i] / (double)s);
            int eq = r > 1 ? 1 : (r < -1 ? -1 : (int)r);
            ok &= q[i] == eq;
        }
        CHECK(ok, "q formula");
        for (size_t i = 0; i < n; i++) w2[i] = (float)q[i] * s;
        CHECK(oma_quant_absmean(w2, n, q2, &s2) == OMA_OK && memcmp(q, q2, n) == 0, "requant idempotent");
        CHECK(oma_quant_rel_l2(w, q, s, n, &e) == OMA_OK && isfinite(e) && e >= 0.0, "err finite");
    }
    section_end("absmean quantization");
}

int main(void) {
    test_constructors();
    test_trit_truth_tables();
    test_invalid_blocks();
    test_exhaustive_small();
    test_dense_bytes();
    test_int_conversion();
    test_random_blocks();
    test_pack_random();
    test_z3();
    test_quant();
    printf("OMA ALGEBRA: %llu checks, %llu failures -> %s\n", g_checks, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
