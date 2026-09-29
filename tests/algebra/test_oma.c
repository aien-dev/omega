/* Parity-oracle tests for the Omega mixed algebra (src/algebra/).
 * Every op is compared against a naive integer oracle written independently
 * of the bitplane formulas. Fixed seeds: runs are reproducible. */
#include "algebra/oma_pack.h"
#include "algebra/oma_quant.h"
#include "algebra/oma_trit.h"
#include "algebra/oma_z3.h"

#include <float.h>
#include <inttypes.h>
#include <limits.h>
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

/* ---- error paths: every output untouched (spec: "On error, outputs are left
 * untouched"). Outputs are prefilled with a sentinel and compared after. ---- */
#define SENT 0x5A
#define UNTOUCHED(buf) (untouched_((const unsigned char *)(buf), sizeof(buf)))
static int untouched_(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i] != SENT) return 0;
    return 1;
}
#define FILL(buf) memset((buf), SENT, sizeof(buf))

static void test_error_untouched(void) {
    section_begin();
    /* pack_bitplane: bad trit in block 2 (reviewer M1 reproducer), bad trit at
     * random positions, short capacity. */
    {
        int8_t t[100] = {0};
        oma_block out[2];
        t[80] = 5;
        FILL(out);
        CHECK(oma_pack_bitplane(t, 100, out, 2) == OMA_E_INVALID_TRIT && UNTOUCHED(out), "pack_bitplane bad trit 80");
        t[80] = 0;
        FILL(out);
        CHECK(oma_pack_bitplane(t, 100, out, 1) == OMA_E_OVERFLOW && UNTOUCHED(out), "pack_bitplane cap");
    }
    for (int it = 0; it < 2000; it++) {
        int8_t t[600];
        oma_block out[10];
        size_t n = 1 + (size_t)(rnd() % 600u);
        for (size_t i = 0; i < n; i++) t[i] = (int8_t)rnd_trit();
        t[rnd() % n] = (int8_t)(rnd() & 1u ? 2 : -2);
        FILL(out);
        CHECK(oma_pack_bitplane(t, n, out, 10) == OMA_E_INVALID_TRIT && UNTOUCHED(out), "pack_bitplane random bad n=%zu", n);
    }
    /* unpack_bitplane: (1,1) in block 2 (M1 reproducer), stray tail lane,
     * short input; invalid block at random index. */
    {
        oma_block in[2] = {{1, 0}, {1, 1}};
        int8_t out[100];
        FILL(out);
        CHECK(oma_unpack_bitplane(in, 2, 100, out) == OMA_E_INVALID_PLANES && UNTOUCHED(out), "unpack_bitplane (1,1) blk2");
        in[1].neg = 0;
        in[1].pos = 1ull << 40; /* lane 104 >= n */
        FILL(out);
        CHECK(oma_unpack_bitplane(in, 2, 100, out) == OMA_E_ARG && UNTOUCHED(out), "unpack_bitplane tail");
        FILL(out);
        CHECK(oma_unpack_bitplane(in, 1, 100, out) == OMA_E_ARG && UNTOUCHED(out), "unpack_bitplane short");
    }
    for (int it = 0; it < 2000; it++) {
        oma_block in[10];
        int8_t out[640];
        size_t n = 1 + (size_t)(rnd() % 640u), nb = oma_bitplane_blocks(n);
        for (size_t k = 0; k < nb; k++) {
            random_valid_block(&in[k]);
            if (k == nb - 1 && n % 64) {
                uint64_t live = (1ull << (n % 64)) - 1u;
                in[k].pos &= live;
                in[k].neg &= live;
            }
        }
        size_t bad = (size_t)(rnd() % nb);
        in[bad].pos |= 1u;
        in[bad].neg |= 1u;
        FILL(out);
        CHECK(oma_unpack_bitplane(in, nb, n, out) == OMA_E_INVALID_PLANES && UNTOUCHED(out), "unpack_bitplane random n=%zu", n);
    }
    /* dense pack / unpack */
    {
        int8_t t[15] = {0};
        uint8_t out[3];
        size_t len = 777;
        t[14] = 3;
        FILL(out);
        CHECK(oma_pack_dense(t, 15, out, 3, &len) == OMA_E_INVALID_TRIT && UNTOUCHED(out) && len == 777, "pack_dense bad");
        t[14] = 0;
        FILL(out);
        CHECK(oma_pack_dense(t, 15, out, 2, &len) == OMA_E_OVERFLOW && UNTOUCHED(out) && len == 777, "pack_dense cap");
    }
    {
        const uint8_t in1[3] = {121, 121, 250}, in2[2] = {121, 0};
        int8_t out[15];
        FILL(out);
        CHECK(oma_unpack_dense(in1, 3, 15, out) == OMA_E_INVALID_BYTE && UNTOUCHED(out), "unpack_dense byte 250");
        FILL(out);
        CHECK(oma_unpack_dense(in2, 2, 6, out) == OMA_E_INVALID_BYTE && UNTOUCHED(out), "unpack_dense padding");
        FILL(out);
        CHECK(oma_unpack_dense(in1, 2, 15, out) == OMA_E_ARG && UNTOUCHED(out), "unpack_dense short");
        int8_t d5[5];
        FILL(d5);
        CHECK(oma_dense_byte_decode(243, d5) == OMA_E_INVALID_BYTE && UNTOUCHED(d5), "byte_decode 243");
    }
    for (int it = 0; it < 2000; it++) {
        uint8_t in[200];
        int8_t out[1000];
        size_t n = 1 + (size_t)(rnd() % 1000u), nb = oma_dense_bytes(n);
        for (size_t k = 0; k < nb; k++) in[k] = (uint8_t)(rnd() % 243u);
        if (n % 5) in[nb - 1] = 121; /* all-zero trits: canonical padding */
        in[rnd() % nb] = (uint8_t)(243u + rnd() % 13u);
        FILL(out);
        CHECK(oma_unpack_dense(in, nb, n, out) == OMA_E_INVALID_BYTE && UNTOUCHED(out), "unpack_dense random n=%zu", n);
    }
    /* single blocks, serialization, dots */
    {
        oma_block bad = {1u << 7, 1u << 7}, ok = {1, 2}, ob[1];
        int8_t lanes[64];
        uint8_t ser[16];
        int32_t d[1];
        FILL(lanes);
        CHECK(oma_block_decode(&bad, lanes) == OMA_E_INVALID_PLANES && UNTOUCHED(lanes), "block decode");
        int8_t enc[64] = {0};
        enc[63] = 2;
        FILL(ob);
        CHECK(oma_block_encode(enc, ob) == OMA_E_INVALID_TRIT && UNTOUCHED(ob), "block encode");
        FILL(ob);
        CHECK(oma_block_neg(&bad, ob) == OMA_E_INVALID_PLANES && UNTOUCHED(ob), "block neg");
        FILL(ob);
        CHECK(oma_block_mul(&ok, &bad, ob) == OMA_E_INVALID_PLANES && UNTOUCHED(ob), "block mul");
        oma_block sm[1], cy[1];
        FILL(sm);
        FILL(cy);
        CHECK(oma_block_add(&ok, &bad, sm, cy) == OMA_E_INVALID_PLANES && UNTOUCHED(sm) && UNTOUCHED(cy), "block add");
        FILL(d);
        CHECK(oma_block_dot(&ok, &bad, d) == OMA_E_INVALID_PLANES && UNTOUCHED(d), "block dot");
        FILL(ser);
        CHECK(oma_block_serialize(&bad, ser) == OMA_E_INVALID_PLANES && UNTOUCHED(ser), "serialize");
        uint8_t raw[16] = {0};
        raw[3] = 0x10;
        raw[11] = 0x10;
        FILL(ob);
        CHECK(oma_block_deserialize(raw, ob) == OMA_E_INVALID_PLANES && UNTOUCHED(ob), "deserialize");
        oma_block w[3] = {{1, 0}, {0, 1}, {0, 0}};
        int8_t x[192] = {0};
        w[2].pos = 1ull << 20; /* lane 148 >= n = 140 */
        FILL(d);
        CHECK(oma_dot_tw_i8(w, x, 140, d) == OMA_E_ARG && UNTOUCHED(d), "dot_tw stray lane blk3");
        w[2].pos = 1u;
        w[2].neg = 1u;
        FILL(d);
        CHECK(oma_dot_tw_i8(w, x, 140, d) == OMA_E_INVALID_PLANES && UNTOUCHED(d), "dot_tw (1,1) blk3");
        FILL(d);
        CHECK(oma_dot_tw_i8(w, x, OMA_DOT_I8_MAX_N + 1, d) == OMA_E_OVERFLOW && UNTOUCHED(d), "dot_tw n>max");
    }
    /* Z3 blocks */
    {
        oma_z3_block bad = {1u << 9, 1u << 9}, ok = {1, 2}, oz[1];
        oma_block ot[1], tbad = {5, 4};
        uint8_t zv[64];
        FILL(zv);
        CHECK(oma_z3_block_decode(&bad, zv) == OMA_E_INVALID_PLANES && UNTOUCHED(zv), "z3 decode");
        uint8_t zin[64] = {0};
        zin[63] = 3;
        FILL(oz);
        CHECK(oma_z3_block_encode(zin, oz) == OMA_E_INVALID_Z3 && UNTOUCHED(oz), "z3 encode");
        FILL(oz);
        CHECK(oma_z3_block_add(&ok, &bad, oz) == OMA_E_INVALID_PLANES && UNTOUCHED(oz), "z3 add");
        FILL(oz);
        CHECK(oma_z3_block_mul(&bad, &ok, oz) == OMA_E_INVALID_PLANES && UNTOUCHED(oz), "z3 mul");
        FILL(oz);
        CHECK(oma_z3_block_neg(&bad, oz) == OMA_E_INVALID_PLANES && UNTOUCHED(oz), "z3 neg");
        FILL(oz);
        CHECK(oma_z3_block_from_trits(&tbad, oz) == OMA_E_INVALID_PLANES && UNTOUCHED(oz), "z3 from trits");
        FILL(ot);
        CHECK(oma_z3_block_to_trits(&bad, ot) == OMA_E_INVALID_PLANES && UNTOUCHED(ot), "z3 to trits");
    }
    /* integer <-> balanced ternary */
    {
        int8_t dg[41];
        size_t nd[1];
        FILL(dg);
        FILL(nd);
        CHECK(oma_int_to_bt(INT64_MAX, dg, 40, nd) == OMA_E_OVERFLOW && UNTOUCHED(dg) && UNTOUCHED(nd), "int_to_bt cap");
        FILL(dg);
        CHECK(oma_int_to_bt_fixed(13, dg, 2) == OMA_E_OVERFLOW && UNTOUCHED(dg), "int_to_bt_fixed");
        int64_t v[1];
        int8_t digs[41] = {0};
        digs[40] = 2;
        FILL(v);
        CHECK(oma_bt_to_int(digs, 41, v) == OMA_E_INVALID_TRIT && UNTOUCHED(v), "bt_to_int digit");
        for (int i = 0; i < 41; i++) digs[i] = 1; /* (3^41-1)/2 > INT64_MAX */
        FILL(v);
        CHECK(oma_bt_to_int(digs, 41, v) == OMA_E_OVERFLOW && UNTOUCHED(v), "bt_to_int overflow");
    }
    /* quantization */
    {
        float w[8] = {1, 2, 3, 4, 5, 6, 7, NAN};
        int8_t q[8];
        float sc[1];
        double er[1];
        FILL(q);
        FILL(sc);
        CHECK(oma_quant_absmean(w, 8, q, sc) == OMA_E_ARG && UNTOUCHED(q) && UNTOUCHED(sc), "absmean nan last");
        const float den[3] = {2 * FLT_TRUE_MIN, 0, 0};
        FILL(q);
        FILL(sc);
        CHECK(oma_quant_absmean(den, 3, q, sc) == OMA_E_UNDERFLOW && UNTOUCHED(q) && UNTOUCHED(sc), "absmean underflow");
        const int8_t q1[3] = {1, 0, 0};
        FILL(er);
        CHECK(oma_quant_rel_l2(w, q1, NAN, 3, er) == OMA_E_ARG && UNTOUCHED(er), "rel_l2 nan scale");
        FILL(er);
        CHECK(oma_quant_rel_l2(w, q1, -1.0f, 3, er) == OMA_E_ARG && UNTOUCHED(er), "rel_l2 neg scale");
    }
    /* scalar ops: out-of-range wide ints rejected, outputs untouched */
    {
        static const int wide[] = {INT_MIN, -65536, -257, -129, -2, 3, 4, 127, 255, 256, 257, 258, 65536, INT_MAX};
        for (size_t i = 0; i < sizeof wide / sizeof wide[0]; i++) {
            int v = wide[i];
            uint8_t c[1], c2[1];
            oma_trit t[1], t2[1];
            oma_z3 z[1];
            int8_t d5[5];
            FILL(c); FILL(c2); FILL(t); FILL(t2); FILL(z); FILL(d5);
            if (v < 0 || v > 3) {
                CHECK(oma_code_to_trit(v, t) == OMA_E_INVALID_CODE, "code_to_trit(%d)", v);
                CHECK(oma_code_neg(v, c) == OMA_E_INVALID_CODE, "code_neg(%d)", v);
                CHECK(oma_code_add(v, 0, c, c2) == OMA_E_INVALID_CODE && oma_code_add(1, v, c, c2) == OMA_E_INVALID_CODE, "code_add(%d)", v);
                CHECK(oma_code_mul(v, 1, c) == OMA_E_INVALID_CODE && oma_code_mul(2, v, c) == OMA_E_INVALID_CODE, "code_mul(%d)", v);
            }
            CHECK(oma_trit_to_code(v, c) == OMA_E_INVALID_TRIT, "trit_to_code(%d)", v);
            CHECK(oma_trit_neg(v, t) == OMA_E_INVALID_TRIT, "trit_neg(%d)", v);
            CHECK(oma_trit_add(v, 0, t, t2) == OMA_E_INVALID_TRIT && oma_trit_add(1, v, t, t2) == OMA_E_INVALID_TRIT, "trit_add(%d)", v);
            CHECK(oma_trit_mul(v, 1, t) == OMA_E_INVALID_TRIT && oma_trit_mul(-1, v, t) == OMA_E_INVALID_TRIT, "trit_mul(%d)", v);
            CHECK(oma_trit_to_z3(v, z) == OMA_E_INVALID_TRIT, "trit_to_z3(%d)", v);
            CHECK(oma_z3_add(v, 0, z) == OMA_E_INVALID_Z3 && oma_z3_add(2, v, z) == OMA_E_INVALID_Z3, "z3_add(%d)", v);
            CHECK(oma_z3_mul(v, 1, z) == OMA_E_INVALID_Z3 && oma_z3_mul(1, v, z) == OMA_E_INVALID_Z3, "z3_mul(%d)", v);
            CHECK(oma_z3_neg(v, z) == OMA_E_INVALID_Z3, "z3_neg(%d)", v);
            CHECK(oma_z3_to_trit(v, t) == OMA_E_INVALID_Z3, "z3_to_trit(%d)", v);
            CHECK(oma_z3_make(v, z) == OMA_E_INVALID_Z3 && oma_trit_make(v, t) == OMA_E_INVALID_TRIT, "make(%d)", v);
            if (v < 0 || v >= 243) CHECK(oma_dense_byte_decode(v, d5) == OMA_E_INVALID_BYTE, "byte_decode(%d)", v);
            CHECK(UNTOUCHED(c) && UNTOUCHED(c2) && UNTOUCHED(t) && UNTOUCHED(t2) && UNTOUCHED(z) && UNTOUCHED(d5), "scalar outputs untouched (%d)", v);
        }
    }
    section_end("errors leave outputs");
}

/* ---- absmean oracle: independent of the implementation's round/clamp.
 * Inputs are dyadic, w_i = k_i * 2^-e with |k_i| < 2^21, so the exact mean is
 * the rational S / (n * 2^e), S = sum |k_i| (exact int64). All comparisons
 * below are exact in double (<= 35 significant bits). ---- */
static int cmp_x_mean(double x, int64_t S, size_t n, int e) { /* sign(x - S/(n 2^e)) */
    double l = ldexp(x * (double)n, e), r = (double)S;
    return l < r ? -1 : (l > r ? 1 : 0);
}
/* rc/q/scale for dyadic input vs the exact rules:
 *  S == 0          -> OK, scale 0, q 0
 *  mean < FLT_MIN  -> OMA_E_UNDERFLOW
 *  else scale = mean rounded to nearest float (checked via both half-ulp
 *  midpoints) and q_i = sign(k_i) iff 2|k_i| >= scale * 2^e. */
static int quant_oracle_ok(const int32_t *k, size_t n, int e, int rc, const int8_t *q, float s) {
    int64_t S = 0;
    for (size_t i = 0; i < n; i++) S += k[i] < 0 ? -(int64_t)k[i] : k[i];
    if (S == 0) {
        if (rc != OMA_OK || s != 0.0f) return 0;
        for (size_t i = 0; i < n; i++)
            if (q[i]) return 0;
        return 1;
    }
    if (cmp_x_mean((double)FLT_MIN, S, n, e) > 0) return rc == OMA_E_UNDERFLOW;
    if (rc != OMA_OK || !(s >= FLT_MIN) || !isfinite(s)) return 0;
    double lo = ((double)s + (double)nextafterf(s, 0.0f)) / 2.0, hi = ((double)s + (double)nextafterf(s, INFINITY)) / 2.0;
    if (cmp_x_mean(lo, S, n, e) > 0 || cmp_x_mean(hi, S, n, e) < 0) return 0;
    for (size_t i = 0; i < n; i++) {
        double ak = 2.0 * fabs((double)k[i]), th = ldexp((double)s, e);
        int exp = ak >= th ? (k[i] > 0 ? 1 : -1) : 0;
        if (q[i] != exp) return 0;
    }
    return 1;
}

static void test_quant(void) {
    section_begin();
    int8_t q[1024], q2[1024];
    float s, s2;
    double e;
    /* hand-computed vectors */
    {
        const float w[4] = {0.5f, -1.5f, 0.0f, 2.0f}; /* sum 4, scale 1 */
        const int8_t exp[4] = {1, -1, 0, 1};           /* 0.5 rounds half away from zero */
        CHECK(oma_quant_absmean(w, 4, q, &s) == OMA_OK && s == 1.0f && memcmp(q, exp, 4) == 0, "known vec 1");
        /* diff {-0.5,-0.5,0,1}: num 1.5, den 6.5 */
        CHECK(oma_quant_rel_l2(w, q, s, 4, &e) == OMA_OK && fabs(e - sqrt(1.5 / 6.5)) < 1e-15, "known err 1 %.17g", e);
    }
    {
        const float w[4] = {0.25f, -0.25f, 1.0f, 0.5f}; /* sum 2, scale 0.5; +-0.25 are exact ties */
        const int8_t exp[4] = {1, -1, 1, 1};
        CHECK(oma_quant_absmean(w, 4, q, &s) == OMA_OK && s == 0.5f && memcmp(q, exp, 4) == 0, "known vec ties");
        /* recon {.5,-.5,.5,.5}; num = 1/16+1/16+1/4 = 3/8, den = 1/16+1/16+1+1/4 = 11/8 */
        CHECK(oma_quant_rel_l2(w, q, s, 4, &e) == OMA_OK && fabs(e - sqrt(3.0 / 11.0)) < 1e-15, "known err ties %.17g", e);
    }
    {
        const float b = 0.5f - 0x1p-25f; /* largest float below the tie at scale 1 */
        const float w[4] = {2.5f, -1.0f, b, 0x1p-25f}; /* sum exactly 4 -> scale 1 */
        const int8_t exp[4] = {1, -1, 0, 0};
        CHECK(oma_quant_absmean(w, 4, q, &s) == OMA_OK && s == 1.0f && memcmp(q, exp, 4) == 0, "just below tie");
        const float wn[4] = {-2.5f, 1.0f, -b, -0x1p-25f};
        const int8_t expn[4] = {-1, 1, 0, 0};
        CHECK(oma_quant_absmean(wn, 4, q, &s) == OMA_OK && s == 1.0f && memcmp(q, expn, 4) == 0, "just below tie neg");
    }
    {
        const float w[4] = {0.1f, 0.2f, 0.3f, -0.4f};
        const int8_t exp[4] = {0, 1, 1, -1}; /* scale ~0.25: 0.1/0.25 = 0.4 -> 0, 0.2/0.25 = 0.8 -> 1 */
        CHECK(oma_quant_absmean(w, 4, q, &s) == OMA_OK && fabsf(s - 0.25f) < 1e-7f && memcmp(q, exp, 4) == 0, "known vec 2");
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
    /* denormal and extreme inputs */
    {
        const float d1[3] = {2 * FLT_TRUE_MIN, 0, 0}, d2[2] = {FLT_TRUE_MIN, 0};
        CHECK(oma_quant_absmean(d1, 3, q, &s) == OMA_E_UNDERFLOW, "2*denorm_min underflow");
        CHECK(oma_quant_absmean(d2, 2, q, &s) == OMA_E_UNDERFLOW, "denorm_min underflow");
        const float m3[3] = {3 * FLT_MIN, 0, 0};
        CHECK(oma_quant_absmean(m3, 3, q, &s) == OMA_OK && s == FLT_MIN && q[0] == 1 && !q[1] && !q[2], "3*FLT_MIN");
        const float r3[3] = {FLT_MIN, 0, 0}; /* re-quantization of the above: mean FLT_MIN/3 */
        CHECK(oma_quant_absmean(r3, 3, q2, &s2) == OMA_E_UNDERFLOW, "requant below FLT_MIN is explicit");
        const float m2[2] = {FLT_MIN, -FLT_MIN};
        CHECK(oma_quant_absmean(m2, 2, q, &s) == OMA_OK && s == FLT_MIN && q[0] == 1 && q[1] == -1, "FLT_MIN pair");
        const float big[4] = {FLT_MAX, -FLT_MAX, FLT_MAX, 0};
        CHECK(oma_quant_absmean(big, 4, q, &s) == OMA_OK && isfinite(s) && q[0] == 1 && q[1] == -1 && q[2] == 1 && !q[3], "FLT_MAX");
        CHECK(oma_quant_rel_l2(big, q, s, 4, &e) == OMA_OK && isfinite(e), "FLT_MAX err");
    }
    /* rel_l2 argument checks */
    {
        const float one[1] = {1}, zero[1] = {0}, nw[1] = {NAN}, iw[1] = {INFINITY};
        const int8_t q1[1] = {1}, q0[1] = {0};
        CHECK(oma_quant_rel_l2(one, q1, NAN, 1, &e) == OMA_E_ARG, "scale nan");
        CHECK(oma_quant_rel_l2(one, q1, INFINITY, 1, &e) == OMA_E_ARG, "scale inf");
        CHECK(oma_quant_rel_l2(one, q1, -1.0f, 1, &e) == OMA_E_ARG, "scale negative");
        CHECK(oma_quant_rel_l2(nw, q1, 1.0f, 1, &e) == OMA_E_ARG, "w nan");
        CHECK(oma_quant_rel_l2(iw, q1, 1.0f, 1, &e) == OMA_E_ARG, "w inf");
        CHECK(oma_quant_rel_l2(zero, q1, 1.0f, 1, &e) == OMA_E_ARG, "w=0, recon!=0 undefined");
        CHECK(oma_quant_rel_l2(zero, q1, 0.0f, 1, &e) == OMA_OK && e == 0.0, "w=0, scale 0");
        CHECK(oma_quant_rel_l2(one, q0, 0.0f, 1, &e) == OMA_OK && e == 1.0, "recon 0 -> err 1");
    }
    static float w[1024], w2[1024];
    static int32_t kk[1024];
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
        /* (b) dyadic floats vs the exact oracle. Half the runs sit near and
         * below FLT_MIN (subnormal inputs, underflow branch). */
        int e2 = (it & 1) ? 110 + (int)(rnd() % 40u) : (int)(rnd() % 40u);
        int sparse = (int)(rnd() % 4u);
        for (size_t i = 0; i < n; i++) {
            int32_t k = (int32_t)(rnd() % (1u << 21)) - (1 << 20);
            if (sparse == 0 && rnd() % 8u) k = 0;
            if (sparse == 1) k >>= (int)(rnd() % 21u); /* spread magnitudes */
            kk[i] = k;
            w[i] = ldexpf((float)k, -e2);
        }
        int rc = oma_quant_absmean(w, n, q, &s);
        CHECK(quant_oracle_ok(kk, n, e2, rc, q, s), "oracle it=%d n=%zu e=%d rc=%d", it, n, e2, rc);
        if (rc != OMA_OK) continue;
        /* re-quantizing q*scale: same q, or OMA_E_UNDERFLOW exactly when
         * nnz(q)*scale/n < FLT_MIN (spec). */
        size_t kq = 0;
        for (size_t i = 0; i < n; i++) {
            w2[i] = (float)q[i] * s;
            kq += q[i] != 0;
        }
        int rc2 = oma_quant_absmean(w2, n, q2, &s2);
        if (kq && (double)kq * (double)s < (double)FLT_MIN * (double)n)
            CHECK(rc2 == OMA_E_UNDERFLOW, "requant underflow it=%d", it);
        else
            CHECK(rc2 == OMA_OK && memcmp(q, q2, n) == 0, "requant idempotent it=%d", it);
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
    test_error_untouched();
    test_quant();
    printf("OMA ALGEBRA: %llu checks, %llu failures -> %s\n", g_checks, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
