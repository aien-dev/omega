/* omega#440 Gate 2 tests for the in-house zlib inflate and MAT v5 reader
 * (tools/reservoir/res_inflate.c, res_mat.c), which read the authors'
 * archive. Expected values: a published test vector (Adler-32 of
 * "Wikipedia" = 0x11E60398, the worked example of the Wikipedia article on
 * Adler-32), a deflate stream made by `gzip -9n` (fixed Huffman), and the reference
 * topologies of res_fixtures.h, which were transcribed from the archive by
 * a separate tool in Gate 1; never from the code under test.
 *
 * Fixtures tests/reservoir/fixtures/Length5.mat and Length50.mat are copied
 * unmodified from Armendarez et al., Zenodo 10.5281/zenodo.17296788
 * (data/raw/Codes for Zenodo/NARMA Final Code/Connected Reservoir/),
 * licensed CC-BY-4.0; sha256 pinned below. */
#include "res_fixtures.h"
#include "res_inflate.h"
#include "res_mat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const char *FIX5 = "tests/reservoir/fixtures/Length5.mat";
static const char *FIX50 = "tests/reservoir/fixtures/Length50.mat";
static const uint8_t SHA5[32] = {0xdc, 0x6e, 0x79, 0x14, 0xd2, 0x16, 0xa2, 0x1f, 0x66, 0x4a, 0xdc, 0xd4, 0x84, 0x3e, 0x9f, 0x70,
                                 0x56, 0x16, 0xdf, 0x47, 0x51, 0x18, 0x79, 0xba, 0x0b, 0xbe, 0xce, 0xb2, 0x41, 0xb9, 0xb5, 0x0a};
static const uint8_t SHA50[32] = {0xff, 0x4c, 0x9d, 0x14, 0xab, 0xb6, 0xf6, 0x4b, 0x1a, 0xb9, 0xf3, 0x01, 0x07, 0x2d, 0xc6, 0x73,
                                  0x16, 0xb7, 0xfd, 0x59, 0xfe, 0x61, 0xd0, 0xb7, 0x63, 0x2e, 0xe4, 0x67, 0xa8, 0xc2, 0xaa, 0xe3};

static void test_inflate(void)
{
    static const uint8_t word[9] = {'W', 'i', 'k', 'i', 'p', 'e', 'd', 'i', 'a'};
    CHECK(res_adler32(word, 9) == 0x11E60398u);
    CHECK(res_adler32(word, 0) == 1u);
    /* stored block: header 78 01, BFINAL=1 BTYPE=00, LEN 9, NLEN ~9 */
    uint8_t st[] = {0x78, 0x01, 0x01, 0x09, 0x00, 0xf6, 0xff, 'W', 'i', 'k', 'i', 'p', 'e', 'd', 'i', 'a',
                    0x11, 0xe6, 0x03, 0x98};
    /* fixed Huffman block: the raw deflate of `printf Wikipedia | gzip -9n` */
    uint8_t fx[] = {0x78, 0x9c, 0x0b, 0xcf, 0xcc, 0xce, 0x2c, 0x48, 0x4d, 0xc9, 0x4c, 0x04, 0x00,
                    0x11, 0xe6, 0x03, 0x98};
    uint8_t *out = NULL;
    size_t n = 0, used = 0;
    CHECK(res_zlib_inflate(st, sizeof st, &out, &n, &used) == RES_INF_OK);
    CHECK(n == 9 && out && memcmp(out, word, 9) == 0 && used == sizeof st);
    free(out);
    out = NULL;
    CHECK(res_zlib_inflate(fx, sizeof fx, &out, &n, &used) == RES_INF_OK);
    CHECK(n == 9 && out && memcmp(out, word, 9) == 0 && used == sizeof fx);
    free(out);
    /* refusals */
    uint8_t bad[sizeof st];
    memcpy(bad, fx, sizeof fx);
    bad[sizeof fx - 1] ^= 1u; /* Adler-32 trailer */
    out = NULL;
    CHECK(res_zlib_inflate(bad, sizeof fx, &out, &n, &used) == RES_INF_ERR_ADLER);
    free(out);
    memcpy(bad, st, sizeof st);
    bad[5] ^= 1u; /* NLEN no longer the complement of LEN */
    out = NULL;
    CHECK(res_zlib_inflate(bad, sizeof st, &out, &n, &used) == RES_INF_ERR_BLOCK);
    free(out);
    uint8_t hdr_check[] = {0x78, 0x00, 0x01, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01};
    out = NULL;
    CHECK(res_zlib_inflate(hdr_check, sizeof hdr_check, &out, &n, &used) == RES_INF_ERR_HEADER);
    free(out);
    uint8_t dict[] = {0x78, 0xbb, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01};
    out = NULL;
    CHECK(res_zlib_inflate(dict, sizeof dict, &out, &n, &used) == RES_INF_ERR_HEADER);
    free(out);
    uint8_t reserved[] = {0x78, 0x01, 0x07, 0x00, 0x00, 0x00, 0x01}; /* BTYPE=11 */
    out = NULL;
    CHECK(res_zlib_inflate(reserved, sizeof reserved, &out, &n, &used) == RES_INF_ERR_BLOCK);
    free(out);
    for (size_t cut = 0; cut < sizeof fx; cut++) { /* every truncation refuses */
        out = NULL;
        CHECK(res_zlib_inflate(fx, cut, &out, &n, &used) != RES_INF_OK);
        free(out);
    }
}

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *b = malloc(1u << 16);
    *n = b ? fread(b, 1, 1u << 16, f) : 0;
    fclose(f);
    return b;
}

/* 1-based edge list of a chain conmat (column-major L x L), checked symmetric. */
static int chain_edges(const double *cm, uint32_t L, uint32_t (*e)[2], uint32_t max)
{
    int m = 0;
    for (uint32_t j = 0; j < L; j++)
        for (uint32_t i = 0; i < L; i++) {
            if (cm[i + L * j] != cm[j + L * i]) return -1;
            if (i < j && cm[i + L * j] != 0.0) {
                if (cm[i + L * j] != 1.0 || (uint32_t)m >= max) return -1;
                e[m][0] = i + 1u;
                e[m][1] = j + 1u;
                m++;
            }
        }
    return m;
}

/* The ASan build (-DRES_QUICK) samples the flip and truncation sweeps:
 * each parse is slow under ASan's stack-use-after-return detection (a fake
 * frame per call). The plain build flips three bits at every byte and cuts
 * at every length. */
#ifdef RES_QUICK
#define FLIP_STRIDE 11u
#else
#define FLIP_STRIDE 1u
#endif

static ResMatFile g_f, g_g;

static int same_values(const ResMatFile *a, const ResMatFile *b, const char *name)
{
    const ResMatArray *x = res_mat_find(a, name), *y = res_mat_find(b, name);
    if (!x || !y) return x == y;
    if (x->numel != y->numel || x->cls != y->cls) return 0;
    return !x->re || memcmp(x->re, y->re, x->numel * sizeof(double)) == 0;
}

static void test_mat(void)
{
    const double *cm, *gr, *ix;
    uint32_t e[64][2];
    CHECK(res_mat_load(FIX5, SHA5, &g_f) == RES_MAT_OK);
    CHECK(res_mat_values(res_mat_find(&g_f, "conmat"), 25, &cm) == RES_MAT_OK);
    CHECK(res_mat_values(res_mat_find(&g_f, "ground"), 1, &gr) == RES_MAT_OK && gr[0] == 5.0);
    CHECK(res_mat_values(res_mat_find(&g_f, "inex"), 25, &ix) == RES_MAT_OK);
    int all1 = 1;
    for (int k = 0; k < 25; k++) all1 &= ix[k] == 1.0;
    CHECK(all1);
    CHECK(chain_edges(cm, 5, e, 64) == 4);
    for (uint32_t k = 0; k < 4; k++) CHECK(e[k][0] == k + 1u && e[k][1] == k + 2u);
    CHECK(res_mat_find(&g_f, "no_such_variable") == NULL);
    res_mat_free(&g_f);

    CHECK(res_mat_load(FIX50, SHA50, &g_f) == RES_MAT_OK);
    CHECK(res_mat_values(res_mat_find(&g_f, "conmat"), 2500, &cm) == RES_MAT_OK);
    CHECK(res_mat_values(res_mat_find(&g_f, "ground"), 1, &gr) == RES_MAT_OK && gr[0] == 50.0);
    int m = chain_edges(cm, 50, e, 64);
    CHECK(m == 49);
    int same = m == 49;
    for (int k = 0; same && k < 49; k++) /* fixture edges may be listed either way round */
        same = (e[k][0] == RES_FIX_CHAIN50_EDGES[k][0] && e[k][1] == RES_FIX_CHAIN50_EDGES[k][1]) ||
               (e[k][0] == RES_FIX_CHAIN50_EDGES[k][1] && e[k][1] == RES_FIX_CHAIN50_EDGES[k][0]);
    CHECK(same);
    res_mat_free(&g_f);

    CHECK(res_mat_load(FIX5, SHA50, &g_f) == RES_MAT_ERR_DIGEST);
    CHECK(res_mat_load("tests/reservoir/fixtures/absent.mat", NULL, &g_f) == RES_MAT_ERR_IO);
}

/* A corrupted or truncated file must refuse, or decode to exactly the same
 * values: never silently different data. */
static void test_corruption(void)
{
    size_t n = 0;
    uint8_t *b = slurp(FIX5, &n);
    CHECK(b && n == 2130);
    if (!b || n != 2130) return;
    CHECK(res_mat_parse(b, n, &g_f) == RES_MAT_OK);
    uint32_t full = g_f.var_count;
    int silent = 0, refused = 0;
    for (size_t off = 0; off < n; off += FLIP_STRIDE)
        for (int bit = 0; bit < 8; bit += 3) {
            b[off] ^= (uint8_t)(1u << bit);
            ResMatStatus s = res_mat_parse(b, n, &g_g);
            if (s != RES_MAT_OK) refused++;
            else if (g_g.var_count != full || !same_values(&g_f, &g_g, "conmat") || !same_values(&g_f, &g_g, "ground") ||
                     !same_values(&g_f, &g_g, "inex"))
                silent++;
            res_mat_free(&g_g);
            b[off] ^= (uint8_t)(1u << bit);
        }
    CHECK(silent == 0);
    CHECK(refused > 0);
    for (size_t cut = 0; cut < n; cut += FLIP_STRIDE) {
        ResMatStatus s = res_mat_parse(b, cut, &g_g);
        if (s == RES_MAT_OK) /* a cut at an element boundary is a valid shorter file */
            CHECK(g_g.var_count < full && (!res_mat_find(&g_g, "conmat") || same_values(&g_f, &g_g, "conmat")));
        res_mat_free(&g_g);
    }
    res_mat_free(&g_f);
    free(b);
}

int main(void)
{
    res_mat_verbose = 0; /* thousands of expected refusals below */
    test_inflate();
    test_mat();
    test_corruption();
    printf("test_res_mat: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
