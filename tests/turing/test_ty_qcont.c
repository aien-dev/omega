/* qint.v1 scorer + PRD1 reader tests. Wave 1: Gaussian family only, so there
 * are no mixture known answers; family 2 is checked as a refusal. */
#include "turing/ty_math.h"
#include "turing/ty_prd.h"
#include "turing/ty_qcont.h"
#include "qcont_kat_gen.c"

#include <fenv.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static tyq_pred G(uint32_t idx, double loc, double scale)
{
    tyq_pred p;
    memset(&p, 0, sizeof p);
    p.index = idx; p.family = TYQ_FAM_GAUSS; p.loc = loc; p.scale = scale;
    return p;
}

static void test_lnsinhc(void)
{
    /* continuity at both branch points */
    double lo = nextafter(1e-4, 0.0), hi = 1e-4;
    CHECK(fabs(tyq_lnsinhc(hi) - tyq_lnsinhc(lo)) < 1e-15);
    CHECK(fabs(log(sinh(1e-4) / 1e-4) - 1e-8 / 6.0) < 1e-15);
    lo = nextafter(1.0, 0.0); hi = 1.0;
    CHECK(fabs(tyq_lnsinhc(hi) - tyq_lnsinhc(lo)) < 1e-14);
    CHECK(fabs(log(sinh(1.0)) - (1.0 - log(2.0) + log1p(-exp(-2.0)))) < 1e-14);
    /* against long double over the whole range */
    double worst = 0;
    for (double a = 1e-9; a < 200.0; a *= 1.37) {
        long double ref = a < 1e-3 ? (long double)a * a / 6.0L - (long double)a * a * a * a / 180.0L
                                   : logl(sinhl((long double)a) / (long double)a);
        if (a > 40.0) ref = a - logl(2.0L * a) + log1pl(-expl(-2.0L * a));
        double e = fabs((double)(ref - tyq_lnsinhc(a)));
        if (e > worst) worst = e;
    }
    printf("lnsinhc worst abs err vs long double: %.3e\n", worst);
    CHECK(worst < 1e-14);
    CHECK(tyq_lnsinhc(0.0) == 0.0);
}

static void test_simpson_kats(void)
{
    const double ss[] = {0x1p-10, 1.0, 1e3};
    const double zs[] = {0, 1, 5, 30, 2048, 4096};
    const int64_t ks[] = {-12345, 777777, 0, -1};
    double worst_ratio = 0;
    int rows = 0;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 6; j++)
            for (int q = 0; q < 4; q++)
                for (int sg = 0; sg < 2; sg++) {
                    int64_t k = ks[q];
                    double zt = sg ? -zs[j] : zs[j];
                    double m = ((double)k + 0.5) * 0x1p-20 - zt * ss[i];
                    qkat_row r;
                    CHECK(qkat_ref(ss[i], m, k, &r) == 0);
                    tyq_pred p = G(0, m, ss[i]);
                    double bits; int hit = -1;
                    CHECK(ty_qcont_bits(&p, k, &bits, &hit) == TYQ_OK);
                    CHECK(hit == 0);
                    double tol = 2e-7 + 1e-12 * (double)r.bits;
                    double err = fabs(bits - (double)r.bits);
                    if (err / tol > worst_ratio) worst_ratio = err / tol;
                    CHECK(err <= tol);
                    rows++;
                }
    /* a scale below the floor is raised and reported; the KAT uses the floored scale */
    {
        int64_t k = 5;
        double m = 0.25;
        qkat_row r;
        CHECK(qkat_ref(1e-5, m, k, &r) == 0);
        tyq_pred p = G(0, m, 1e-5);
        double bits; int hit = 0;
        CHECK(ty_qcont_bits(&p, k, &bits, &hit) == TYQ_OK);
        CHECK(hit == 1);
        CHECK(fabs(bits - (double)r.bits) <= 2e-7 + 1e-12 * (double)r.bits);
    }
    printf("simpson KATs: %d rows, worst |err|/tol = %.3f\n", rows, worst_ratio);
}

static void test_refusals(void)
{
    double bits; int hit; int64_t ub;
    double nan = NAN, inf = INFINITY;
    tyq_pred p;
    p = G(0, nan, 1.0);   CHECK(ty_qcont_bits(&p, 0, &bits, &hit) == TYQ_FAIL_PROTOCOL);
    p = G(0, inf, 1.0);   CHECK(ty_qcont_bits(&p, 0, &bits, &hit) == TYQ_FAIL_PROTOCOL);
    p = G(0, -inf, 1.0);  CHECK(ty_qcont_bits(&p, 0, &bits, &hit) == TYQ_FAIL_PROTOCOL);
    p = G(0, 0.0, nan);   CHECK(ty_qcont_bits(&p, 0, &bits, &hit) == TYQ_FAIL_PROTOCOL);
    p = G(0, 0.0, inf);   CHECK(ty_qcont_bits(&p, 0, &bits, &hit) == TYQ_FAIL_PROTOCOL);
    p = G(0, 0.0, 0.0);   CHECK(ty_qcont_bits(&p, 0, &bits, &hit) == TYQ_FAIL_PROTOCOL);
    p = G(0, 0.0, -1.0);  CHECK(ty_qcont_bits(&p, 0, &bits, &hit) == TYQ_FAIL_PROTOCOL);
    p = G(0, 0.0, -0.0);  CHECK(ty_qcont_bits(&p, 0, &bits, &hit) == TYQ_FAIL_PROTOCOL);
    p = G(0, 0.0, 1.0);
    CHECK(ty_qcont_bits(&p, INT64_C(1) << 51, &bits, &hit) == TYQ_FAIL_PROTOCOL);
    CHECK(ty_qcont_bits(&p, -(INT64_C(1) << 51), &bits, &hit) == TYQ_FAIL_PROTOCOL);
    CHECK(ty_qcont_bits(&p, (INT64_C(1) << 51) - 1, &bits, &hit) == TYQ_OK);
    CHECK(ty_qcont_bits(NULL, 0, &bits, &hit) == TYQ_E_ARG);
    CHECK(ty_qcont_bits(&p, 0, NULL, &hit) == TYQ_E_ARG);
    p.family = TYQ_FAM_STUDENT; CHECK(ty_qcont_bits(&p, 0, &bits, &hit) == TYQ_FAIL_PROTOCOL);
    p.family = TYQ_FAM_MIX;     CHECK(ty_qcont_bits(&p, 0, &bits, &hit) == TYQ_FAIL_PROTOCOL);
    p.family = 77;              CHECK(ty_qcont_bits(&p, 0, &bits, &hit) == TYQ_FAIL_PROTOCOL);

    /* huge standardized distance: finite bits scored at 1e6, refused at 1e150 */
    p = G(0, 0.0, 1.0);
    double m6 = 0.5 * 0x1p-20 - 1e6;             /* k = 0, zc = 1e6 */
    p.loc = m6;
    CHECK(ty_qcont_bits(&p, 0, &bits, &hit) == TYQ_OK && isfinite(bits) && bits > 7e11);
    CHECK(ty_qcont_point_ub(&p, 0, &ub, &hit) == TYQ_OK && ub > 0);
    p.loc = 0.5 * 0x1p-20 - 1e150;
    CHECK(ty_qcont_bits(&p, 0, &bits, &hit) == TYQ_OK && isfinite(bits));  /* finite, but over the ub guard */
    CHECK(ty_qcont_point_ub(&p, 0, &ub, &hit) == TYQ_FAIL_PROTOCOL);
    p.loc = 0.5 * 0x1p-20 - 1e160;                /* zc^2 overflows: non-finite bits */
    CHECK(ty_qcont_bits(&p, 0, &bits, &hit) == TYQ_FAIL_PROTOCOL);
    /* finite bits whose 1e6*bits passes 9e18: zc = 1e7 gives ~7e13 bits, 7e19 ub */
    p.loc = 0.5 * 0x1p-20 - 1e7;
    CHECK(ty_qcont_bits(&p, 0, &bits, &hit) == TYQ_OK && isfinite(bits));
    CHECK(ty_qcont_point_ub(&p, 0, &ub, &hit) == TYQ_FAIL_PROTOCOL);

    /* rounding mode must be to-nearest */
    p = G(0, 0.0, 1.0);
    if (fesetround(FE_UPWARD) == 0) {
        CHECK(ty_qcont_bits(&p, 0, &bits, &hit) == TYQ_FAIL_PROTOCOL);
        fesetround(FE_TONEAREST);
    }
    CHECK(ty_qcont_bits(&p, 0, &bits, &hit) == TYQ_OK);
}

static void test_symbol_and_ub(void)
{
    /* symbol KAT: x = -0.5*delta floors to k = -1; bin(-1) is centred at -delta/2 */
    int64_t k = (int64_t)floor(-0.5 * TYQ_DELTA / TYQ_DELTA);
    CHECK(k == -1);
    tyq_pred p = G(0, 0.0, 1.0);
    double bits; int hit; int64_t ub;
    CHECK(ty_qcont_bits(&p, 0, &bits, &hit) == TYQ_OK);
    /* at the centre of a unit normal: -log2(delta/sqrt(2pi)) = 20 + log2(sqrt(2pi)) */
    double expect = 20.0 + 0.5 * log(2.0 * QKAT_PI) / log(2.0);
    CHECK(fabs(bits - expect) < 1e-6);
    CHECK(ty_qcont_point_ub(&p, 0, &ub, &hit) == TYQ_OK);
    CHECK(ub == (int64_t)llrint(1e6 * bits));

    /* sum: three points, one floored */
    tyq_pred ps[3] = { G(0, 0.0, 1.0), G(1, 0.5, 1e-6), G(2, -2.0, 3.0) };
    int64_t ks[3] = { 0, 524288, -100 }, sum, e0, e1, e2;
    uint64_t hits;
    CHECK(ty_qcont_sum_ub(ps, ks, 3, &sum, &hits) == TYQ_OK);
    CHECK(ty_qcont_point_ub(&ps[0], ks[0], &e0, &hit) == TYQ_OK);
    CHECK(ty_qcont_point_ub(&ps[1], ks[1], &e1, &hit) == TYQ_OK && hit == 1);
    CHECK(ty_qcont_point_ub(&ps[2], ks[2], &e2, &hit) == TYQ_OK);
    CHECK(sum == e0 + e1 + e2 && hits == 1);
    ps[1].scale = -1.0;
    CHECK(ty_qcont_sum_ub(ps, ks, 3, &sum, &hits) == TYQ_FAIL_PROTOCOL);
    CHECK(ty_qcont_sum_ub(NULL, NULL, 0, &sum, &hits) == TYQ_OK && sum == 0 && hits == 0);

    /* overflow of the running sum is a refusal: each point ~4.5e12 ub at zc = 1e6 -> use many big ones */
    tyq_pred big = G(0, 0.5 * 0x1p-20 - 2.0e6, 1.0);   /* ~2.9e12 bits = 2.9e18 ub */
    tyq_pred bp[4]; int64_t bk[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; i++) bp[i] = big;
    CHECK(ty_qcont_point_ub(&big, 0, &ub, &hit) == TYQ_OK && ub > 2e18);
    CHECK(ty_qcont_sum_ub(bp, bk, 4, &sum, &hits) == TYQ_FAIL_PROTOCOL);

    /* L(M) pieces */
    CHECK(ty_qcont_bytes_ub(0, &ub) == TYQ_OK && ub == 0);
    CHECK(ty_qcont_bytes_ub(1000, &ub) == TYQ_OK && ub == 8000000000LL);
    CHECK(ty_qcont_bytes_ub(UINT32_MAX, &ub) == TYQ_OK && ub == 8000000LL * 4294967295LL);
    CHECK(ty_qcont_rider_ub(0, 128, &ub) == TYQ_OK && ub == 0);
    CHECK(ty_qcont_rider_ub(0, 0, &ub) == TYQ_OK && ub == 0);
    CHECK(ty_qcont_rider_ub(2, 128, &ub) == TYQ_OK && ub == 7000000LL);  /* 2*0.5*7 bits */
    CHECK(ty_qcont_rider_ub(3, 1, &ub) == TYQ_OK && ub == 0);
    CHECK(ty_qcont_rider_ub(1, 0, &ub) == TYQ_FAIL_PROTOCOL);
    CHECK(ty_qcont_rider_ub(1, 3, &ub) == TYQ_OK && ub == (int64_t)llrint(1e6 * 0.5 * log2(3.0)));

    /* floor accounting: 25,600 points allows 25 hits, not 26; 800 points allows none */
    CHECK(ty_qcont_floor_check(25, 25600) == TYQ_OK);
    CHECK(ty_qcont_floor_check(26, 25600) == TYQ_FAIL_FLOOR);
    CHECK(ty_qcont_floor_check(0, 800) == TYQ_OK);
    CHECK(ty_qcont_floor_check(1, 800) == TYQ_FAIL_FLOOR);
    CHECK(ty_qcont_floor_check(UINT64_MAX, 1) == TYQ_FAIL_FLOOR);

    CHECK(strcmp(tyq_status_name(TYQ_FAIL_FLOOR), "FAIL_FLOOR") == 0);
    CHECK(strcmp(tyq_status_name(-1), "UNKNOWN") == 0);
}

static void put32(uint8_t *b, uint32_t v) { for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (8 * i)); }
static void putf(uint8_t *b, double d) { uint64_t u; memcpy(&u, &d, 8); for (int i = 0; i < 8; i++) b[i] = (uint8_t)(u >> (8 * i)); }

/* Build a valid n-record file starting at first; caller may then corrupt it. */
static size_t build(uint8_t *b, uint32_t first, uint32_t n)
{
    memcpy(b, "PRD1", 4); put32(b + 4, 1);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *q = b + 8 + 24 * i;
        put32(q, first + i); put32(q + 4, 0);
        putf(q + 8, 0.25 * i); putf(q + 16, 1.0 + i);
    }
    return 8 + 24 * (size_t)n;
}

static void test_prd(void)
{
    uint8_t b[8 + 24 * 8 + 64];
    ty_prd d;
    size_t len = build(b, 128, 4);
    CHECK(ty_prd_parse(b, len, 128, 4, &d) == TYQ_OK && d.n == 4 && d.rec[3].index == 131
          && d.rec[2].loc == 0.5 && d.rec[2].scale == 3.0 && d.rec[2].family == 0);
    /* encode round trip is byte identical */
    uint8_t *e; size_t elen;
    CHECK(ty_prd_encode(&d, &e, &elen) == TYQ_OK && elen == len && memcmp(e, b, len) == 0);
    free(e);
    ty_prd_free(&d);
    CHECK(d.rec == NULL && d.n == 0);

    /* file read */
    char path[] = "/tmp/tyqcont_prdXXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(write(fd, b, len) == (ssize_t)len);
        close(fd);
        CHECK(ty_prd_read(path, 128, 4, &d) == TYQ_OK && d.n == 4);
        ty_prd_free(&d);
        CHECK(ty_prd_read(path, 128, 5, &d) == TYQ_FAIL_PROTOCOL);
        CHECK(ty_prd_read(path, 129, 4, &d) == TYQ_FAIL_PROTOCOL);
        unlink(path);
    }
    CHECK(ty_prd_read("/nonexistent/prd", 0, 1, &d) == TYQ_E_IO);
    CHECK(ty_prd_parse(NULL, 0, 0, 1, &d) == TYQ_E_ARG);
    CHECK(ty_prd_parse(b, len, 128, 0, &d) == TYQ_E_ARG);

    /* every malformed case is FAIL_PROTOCOL */
    uint8_t m[8 + 24 * 8 + 64];
#define MAL(setup) do { memcpy(m, b, len); size_t ml = len; setup; \
        CHECK(ty_prd_parse(m, ml, 128, 4, &d) == TYQ_FAIL_PROTOCOL && d.rec == NULL); } while (0)
    MAL(m[0] = 'X');                                   /* wrong magic */
    MAL(put32(m + 4, 2));                              /* wrong version */
    MAL(put32(m + 4, 0));
    MAL(ml = 5);                                       /* header truncated */
    MAL(ml = 0);
    MAL(ml = len - 1);                                 /* truncated record */
    MAL(ml = len - 24);                                /* missing prediction */
    MAL(m[len] = 0; ml = len + 1);                     /* trailing byte */
    MAL(memset(m + len, 0, 24); ml = len + 24);        /* extra record */
    MAL(put32(m + 8 + 24 * 1, 129 + 5));               /* index gap */
    MAL(put32(m + 8, 127));                            /* index below the held-out set */
    MAL(put32(m + 8 + 24, 128));                       /* duplicate / not increasing */
    MAL(put32(m + 8 + 24 * 3, 200));
    MAL(put32(m + 8 + 4, 1));                          /* family 1 (Student-t) */
    MAL(put32(m + 8 + 4, 2));                          /* family 2 (mixture): refused in wave 1 */
    MAL(put32(m + 8 + 4, 3));                          /* unknown family */
    MAL(put32(m + 8 + 4, 0xFFFFFFFFu));
    MAL(putf(m + 8 + 8, NAN));                         /* NaN loc */
    MAL(putf(m + 8 + 8, INFINITY));
    MAL(putf(m + 8 + 16, NAN));                        /* NaN scale */
    MAL(putf(m + 8 + 16, INFINITY));
    MAL(putf(m + 8 + 16, 0.0));                        /* scale <= 0 */
    MAL(putf(m + 8 + 16, -0.0));
    MAL(putf(m + 8 + 16, -2.0));
#undef MAL
    /* a valid family-2 style record (K and triples) is refused too */
    {
        uint8_t f2[8 + 24 + 4 + 24];
        memcpy(f2, "PRD1", 4); put32(f2 + 4, 1);
        put32(f2 + 8, 0); put32(f2 + 12, 2); putf(f2 + 16, 0.0); putf(f2 + 24, 1.0);
        put32(f2 + 32, 1); putf(f2 + 36, 1.0); putf(f2 + 44, 0.0); putf(f2 + 52, 1.0);
        CHECK(ty_prd_parse(f2, sizeof f2, 0, 1, &d) == TYQ_FAIL_PROTOCOL);
    }
    /* a tiny scale is legal input (floored at scoring, not refused) */
    len = build(b, 0, 1); putf(b + 8 + 16, 1e-9);
    CHECK(ty_prd_parse(b, len, 0, 1, &d) == TYQ_OK);
    ty_prd_free(&d);

    /* validate and encode refusals */
    tyq_pred p = G(0, 0.0, 1.0);
    CHECK(ty_prd_validate(&p) == TYQ_OK);
    p.family = TYQ_FAM_MIX; CHECK(ty_prd_validate(&p) == TYQ_FAIL_PROTOCOL);
    p = G(0, 0.0, 0.0); CHECK(ty_prd_validate(&p) == TYQ_FAIL_PROTOCOL);
    CHECK(ty_prd_validate(NULL) == TYQ_E_ARG);
    tyq_pred rec = G(7, NAN, 1.0);
    ty_prd bad = { 1, 7, &rec };
    CHECK(ty_prd_encode(&bad, &e, &elen) == TYQ_FAIL_PROTOCOL);
    rec = G(8, 0.0, 1.0);
    CHECK(ty_prd_encode(&bad, &e, &elen) == TYQ_FAIL_PROTOCOL);  /* index != first_index */
}

int main(void)
{
    test_lnsinhc();
    test_simpson_kats();
    test_refusals();
    test_symbol_and_ub();
    test_prd();
    printf("test_ty_qcont: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
