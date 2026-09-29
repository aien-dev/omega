/* MIXED_ALGEBRA_DIGITAL_V1 bottom (⊥) coverage at the realization layer.
 * spec/mixed-algebra-digital-v1.md section 3 item 4.
 *
 * Two distinct 2-bit trit encodings are in use:
 *  - H1 (pos, neg) bitplanes: (0,0)=0, (1,0)=+1, (0,1)=-1, (1,1)=⊥.
 *    Reference library (oma_trit) and R2_bitplane.
 *  - crumb (R2c_crumb): two's complement 00=0, 01=+1, 11=-1; 10 = ⊥.
 * ⊥ is never a value: library ops return an error and leave outputs
 * untouched; realizations refuse any int8 weight outside {-1,0,+1}; no
 * realization's packed form may contain its own ⊥ pattern (R2_bitplane
 * and R2b_lut use H1 per lane; R2c_crumb uses the crumb code).
 *
 * Reads realization-private packed layouts (as test_realize.c does for
 * R5_dense5); no library or realization source is modified. */
#include "algebra/oma_pack.h"
#include "algebra/oma_trit.h"
#include "algebra/realize_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long g_checks, g_fail;
static uint64_t g_rng = 0x4d412d4456312d42ULL; /* "MA-DV1-B" */

static uint64_t rnd(void) {
    uint64_t z = (g_rng += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

#define CHECK(c, ...)                                         \
    do {                                                      \
        g_checks++;                                           \
        if (!(c)) {                                           \
            if (g_fail++ < 20) {                              \
                fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                fprintf(stderr, __VA_ARGS__);                 \
                fprintf(stderr, "\n");                        \
            }                                                 \
        }                                                     \
    } while (0)

enum { W_RANDOM, W_POS, W_NEG, W_ZERO };

static void fill_w(int8_t *w, size_t cnt, int kind) {
    for (size_t i = 0; i < cnt; i++) {
        if (kind == W_POS) w[i] = 1;
        else if (kind == W_NEG) w[i] = -1;
        else if (kind == W_ZERO) w[i] = 0;
        else w[i] = (int8_t)((int)(rnd() % 3) - 1);
    }
}

/* ---- 1. H1 ⊥ at the library level (re-asserted for this gate) ---- */
static void test_h1_bottom(void) {
    unsigned long long c0 = g_checks, f0 = g_fail;
    /* encode: every int8 outside {-1,0,1} refused; round trip of valid values */
    for (int v = -128; v <= 127; v++) {
        uint8_t code = 77;
        int rc = oma_trit_to_code(v, &code);
        if (v >= -1 && v <= 1) {
            oma_trit t = 9;
            CHECK(rc == OMA_OK && code != OMA_CODE_INVALID && oma_code_to_trit(code, &t) == OMA_OK && t == v,
                  "round trip %d", v);
        } else {
            CHECK(rc == OMA_E_INVALID_TRIT, "encode %d must be refused", v);
        }
    }
    oma_trit t = 9;
    CHECK(oma_code_to_trit(OMA_CODE_INVALID, &t) == OMA_E_INVALID_CODE && t == 9, "decode ⊥ code");
    /* add / mul with ⊥ on either side: error, outputs untouched */
    for (int a = 0; a <= 3; a++)
        for (int b = 0; b <= 3; b++) {
            if (a != 3 && b != 3) continue;
            uint8_t s = 77, cy = 77, m = 77;
            CHECK(oma_code_add(a, b, &s, &cy) == OMA_E_INVALID_CODE && s == 77 && cy == 77, "add %d %d", a, b);
            CHECK(oma_code_mul(a, b, &m) == OMA_E_INVALID_CODE && m == 77, "mul %d %d", a, b);
        }
    /* block with a (1,1) lane at every position: decode / unpack / add / mul / dot refused */
    oma_block good = {0, 0};
    for (unsigned k = 0; k < 64; k++) {
        oma_block bad = {(uint64_t)1 << k, (uint64_t)1 << k}, o1 = {5, 5}, o2 = {6, 6};
        int8_t lanes[64], unp[64];
        memset(lanes, 42, sizeof lanes);
        memset(unp, 42, sizeof unp);
        int32_t d = 42;
        CHECK(oma_block_decode(&bad, lanes) == OMA_E_INVALID_PLANES && lanes[0] == 42 && lanes[63] == 42,
              "decode ⊥ lane %u", k);
        CHECK(oma_unpack_bitplane(&bad, 1, 64, unp) == OMA_E_INVALID_PLANES && unp[k] == 42, "unpack ⊥ lane %u", k);
        CHECK(oma_block_add(&bad, &good, &o1, &o2) == OMA_E_INVALID_PLANES && o1.pos == 5 && o2.pos == 6,
              "add ⊥ lane %u", k);
        CHECK(oma_block_add(&good, &bad, &o1, &o2) == OMA_E_INVALID_PLANES && o1.neg == 5 && o2.neg == 6,
              "add ⊥ b lane %u", k);
        CHECK(oma_block_mul(&bad, &good, &o1) == OMA_E_INVALID_PLANES && o1.pos == 5, "mul ⊥ lane %u", k);
        CHECK(oma_block_mul(&good, &bad, &o1) == OMA_E_INVALID_PLANES && o1.neg == 5, "mul ⊥ b lane %u", k);
        CHECK(oma_block_dot(&good, &bad, &d) == OMA_E_INVALID_PLANES && d == 42, "dot ⊥ lane %u", k);
    }
    printf("%s H1 ⊥ refused (encode/decode/add/mul/dot)  %8llu checks, %llu failures\n",
           g_fail == f0 ? "PASS" : "FAIL", g_checks - c0, g_fail - f0);
}

/* ---- 2. every realization refuses every non-trit int8 weight ---- */
static void test_rz_refuse_non_trits(void) {
    unsigned long long c0 = g_checks, f0 = g_fail;
    const size_t m = 3, n = 131;
    int8_t *w = malloc(m * n);
    if (!w) { CHECK(0, "oom"); return; }
    for (int v = -128; v <= 127; v++) {
        if (v >= -1 && v <= 1) continue;
        fill_w(w, m * n, W_RANDOM);
        size_t pos = (size_t)(rnd() % (m * n));
        w[pos] = (int8_t)v;
        for (size_t r = 0; r < oma_rz_count(); r++) {
            const oma_rz_impl *im = oma_rz_get(r);
            oma_rz_plan p;
            memset(&p, 0, sizeof p);
            int rc = im->pack(&p, w, m, n);
            CHECK(rc == OMA_RZ_E_TRIT, "%s accepted weight %d at %zu (rc %d)", im->id, v, pos, rc);
            if (rc == OMA_RZ_OK) oma_rz_free(&p);
        }
    }
    free(w);
    printf("%s every non-trit int8 refused by every pack   %8llu checks, %llu failures\n",
           g_fail == f0 ? "PASS" : "FAIL", g_checks - c0, g_fail - f0);
}

/* ---- 3/4. packed forms never hold their own ⊥ and decode back to W ---- */
static void check_bitplane(const int8_t *w, size_t m, size_t n) {
    const oma_rz_impl *im = oma_rz_find("R2_bitplane");
    oma_rz_plan p;
    memset(&p, 0, sizeof p);
    CHECK(im && im->pack(&p, w, m, n) == OMA_RZ_OK, "R2_bitplane pack m=%zu n=%zu", m, n);
    if (!im || !p.mem) return;
    const uint8_t *b = p.mem;
    size_t chunks = (n + 127) / 128, rb = chunks * 32;
    size_t both = 0, wrong = 0;
    for (size_t i = 0; i < m; i++)
        for (size_t c = 0; c < chunks; c++) {
            const uint8_t *P = b + i * rb + c * 32, *N = P + 16;
            for (unsigned j = 0; j < 16; j++) {
                if (P[j] & N[j]) both++;
                for (unsigned k = 0; k < 8; k++) {
                    size_t col = c * 128 + 16u * k + j;
                    int v = ((P[j] >> k) & 1) - ((N[j] >> k) & 1);
                    int want = col < n ? w[i * n + col] : 0;
                    if (v != want) wrong++;
                }
            }
        }
    CHECK(both == 0, "R2_bitplane m=%zu n=%zu: %zu bytes hold a (1,1) ⊥ lane", m, n, both);
    CHECK(wrong == 0, "R2_bitplane m=%zu n=%zu: %zu lanes decode wrong", m, n, wrong);
    oma_rz_free(&p);
}

static void check_crumb(const int8_t *w, size_t m, size_t n) {
    const oma_rz_impl *im = oma_rz_find("R2c_crumb");
    oma_rz_plan p;
    memset(&p, 0, sizeof p);
    CHECK(im && im->pack(&p, w, m, n) == OMA_RZ_OK, "R2c_crumb pack m=%zu n=%zu", m, n);
    if (!im || !p.mem) return;
    const uint8_t *b = p.mem;
    size_t chunks = (n + 63) / 64, rb = chunks * 16;
    size_t bottom = 0, wrong = 0;
    for (size_t i = 0; i < m; i++)
        for (size_t c = 0; c < chunks; c++)
            for (unsigned j = 0; j < 16; j++) {
                unsigned byte = b[i * rb + c * 16 + j];
                for (unsigned k = 0; k < 4; k++) {
                    unsigned cr = (byte >> (2 * k)) & 3u;
                    if (cr == 2u) bottom++;
                    int v = cr == 0 ? 0 : cr == 1 ? 1 : cr == 3 ? -1 : 99;
                    size_t col = c * 64 + 16u * k + j;
                    int want = col < n ? w[i * n + col] : 0;
                    if (v != want) wrong++;
                }
            }
    CHECK(bottom == 0, "R2c_crumb m=%zu n=%zu: %zu crumbs are the ⊥ code 10", m, n, bottom);
    CHECK(wrong == 0, "R2c_crumb m=%zu n=%zu: %zu crumbs decode wrong", m, n, wrong);
    oma_rz_free(&p);
}

/* R2b_lut (realize_bitplane.c pack_lut): 16-row blocks x 16-column
 * super-groups, 64 bytes each at (b*sg + G)*64; byte (r/4)*16 + (r%4)*4 + j
 * holds row b*16+r, columns G*16+4j+q (q = 0..3): pos nibble bit q (low) and
 * neg nibble bit 4+q (high). H1 per lane: a lane with both bits set is ⊥. */
static void check_lut(const int8_t *w, size_t m, size_t n) {
    const oma_rz_impl *im = oma_rz_find("R2b_lut");
    oma_rz_plan p;
    memset(&p, 0, sizeof p);
    CHECK(im && im->pack(&p, w, m, n) == OMA_RZ_OK, "R2b_lut pack m=%zu n=%zu", m, n);
    if (!im || !p.mem) return;
    const uint8_t *b = p.mem;
    size_t sg = (n + 15) / 16, mb = (m + 15) / 16;
    CHECK(p.weight_bytes == mb * sg * 64, "R2b_lut m=%zu n=%zu: weight_bytes %zu", m, n, (size_t)p.weight_bytes);
    size_t both = 0, wrong = 0;
    for (size_t bl = 0; bl < mb; bl++)
        for (size_t G = 0; G < sg; G++) {
            const uint8_t *blk = b + (bl * sg + G) * 64;
            for (unsigned r = 0; r < 16; r++)
                for (unsigned j = 0; j < 4; j++) {
                    unsigned byte = blk[(r / 4) * 16 + (r % 4) * 4 + j];
                    if ((byte & 0xFu) & (byte >> 4)) both++;
                    for (unsigned q = 0; q < 4; q++) {
                        size_t row = bl * 16 + r, col = G * 16 + 4u * j + q;
                        int v = (int)((byte >> q) & 1u) - (int)((byte >> (4 + q)) & 1u);
                        int want = (row < m && col < n) ? w[row * n + col] : 0;
                        if (v != want) wrong++;
                    }
                }
        }
    CHECK(both == 0, "R2b_lut m=%zu n=%zu: %zu bytes set the same bit in pos and neg nibble (⊥)", m, n, both);
    CHECK(wrong == 0, "R2b_lut m=%zu n=%zu: %zu lanes decode wrong", m, n, wrong);
    oma_rz_free(&p);
}

static void test_packed_no_bottom(void) {
    unsigned long long c0 = g_checks, f0 = g_fail;
    static const size_t ns[] = {1, 15, 16, 17, 63, 64, 65, 127, 128, 129, 255, 256, 257, 1000, 4099};
    static const size_t ms[] = {1, 2, 3, 5, 17};
    for (size_t a = 0; a < sizeof ns / sizeof ns[0]; a++)
        for (size_t bi = 0; bi < sizeof ms / sizeof ms[0]; bi++)
            for (int kind = W_RANDOM; kind <= W_ZERO; kind++) {
                size_t m = ms[bi], n = ns[a];
                int8_t *w = malloc(m * n);
                if (!w) { CHECK(0, "oom"); return; }
                fill_w(w, m * n, kind);
                check_bitplane(w, m, n);
                check_crumb(w, m, n);
                check_lut(w, m, n);
                free(w);
            }
    for (int it = 0; it < 200; it++) {
        size_t n = 1 + (size_t)(rnd() % 3000), m = 1 + (size_t)(rnd() % 40);
        int8_t *w = malloc(m * n);
        if (!w) { CHECK(0, "oom"); return; }
        fill_w(w, m * n, W_RANDOM);
        check_bitplane(w, m, n);
        check_crumb(w, m, n);
        check_lut(w, m, n);
        free(w);
    }
    /* large: bench-size row */
    {
        size_t m = 2, n = 16384;
        int8_t *w = malloc(m * n);
        if (!w) { CHECK(0, "oom"); return; }
        fill_w(w, m * n, W_RANDOM);
        check_bitplane(w, m, n);
        check_crumb(w, m, n);
        check_lut(w, m, n);
        free(w);
    }
    printf("%s packed H1 planes/LUT nibbles/crumbs no ⊥  %8llu checks, %llu failures\n",
           g_fail == f0 ? "PASS" : "FAIL", g_checks - c0, g_fail - f0);
}

int main(void) {
    test_h1_bottom();
    test_rz_refuse_non_trits();
    test_packed_no_bottom();
    printf("MA_DIGITAL_V1 bottom %s: %llu checks, %llu failures, %zu realizations\n", g_fail ? "FAIL" : "PASS",
           g_checks, g_fail, oma_rz_count());
    return g_fail ? 1 : 0;
}
