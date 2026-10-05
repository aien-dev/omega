/* PRD2 reader + mixture scorer tests: EXP-002D row 28 under rule R28-v2 and
 * negative controls N1-N4 of docs/turing/PRD2_PREDICTION_FORMAT.md. The fixture
 * path is synthetic (fixed-seed xorshift64*, Box-Muller); no Brownian profile data. */
#include "turing/ty_prd.h"
#include "turing/ty_prd2.h"
#include "turing/ty_qcont.h"

#include <fenv.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define R28(label, cond) do { CHECK(cond); printf("R28-v2 %s: %s\n", label, (cond) ? "ok" : "VIOLATED"); } while (0)

static tyq_pred G(uint32_t idx, double loc, double scale)
{
    tyq_pred p;
    memset(&p, 0, sizeof p);
    p.index = idx; p.family = TYQ_FAM_GAUSS; p.K = 1;
    p.loc = loc; p.scale = scale;
    p.comp[0].pi = 1.0; p.comp[0].loc = loc; p.comp[0].scale = scale;
    return p;
}
static tyq_pred M2(uint32_t idx, double pi0, double l0, double s0, double pi1, double l1, double s1)
{
    tyq_pred p;
    memset(&p, 0, sizeof p);
    p.index = idx; p.family = TYQ_FAM_MIX; p.K = 2;
    p.comp[0].pi = pi0; p.comp[0].loc = l0; p.comp[0].scale = s0;
    p.comp[1].pi = pi1; p.comp[1].loc = l1; p.comp[1].scale = s1;
    return p;
}
static int64_t kof(double y) { return (int64_t)floor(y / TYQ_DELTA); }

static void test_row28(void)
{
    int r;
    double bits;
    tyq_pred p = M2(0, 0.5, 0.0, 1.0, 0.4, 0.0, 1.0);                 /* sum 0.9 */
    R28("1 sum 0.9 -> WEIGHT_SUM", ty_prd2_validate(&p, &r) == TYQ_FAIL_PROTOCOL && r == PRD2_R_WEIGHT_SUM);
    R28("4b no renormalization: scorer refuses, no number", ty_qcont2_bits(&p, 0, &bits, NULL) == TYQ_FAIL_PROTOCOL);
    p.comp[1].pi = 0.6;                                                /* sum 1.1 */
    R28("2 sum 1.1 -> WEIGHT_SUM", ty_prd2_validate(&p, &r) == TYQ_FAIL_PROTOCOL && r == PRD2_R_WEIGHT_SUM);
    p.comp[0].pi = 1.5; p.comp[1].pi = -0.5;                           /* sum 1, negative */
    R28("3 negative weight -> WEIGHT_RANGE", ty_prd2_validate(&p, &r) == TYQ_FAIL_PROTOCOL && r == PRD2_R_WEIGHT_RANGE);
    p.comp[0].pi = 0.5; p.comp[1].pi = 0.5;
    R28("4a well-formed mixture accepted and scored",
        ty_prd2_validate(&p, &r) == TYQ_OK && r == PRD2_R_NONE && ty_qcont2_bits(&p, 123, &bits, NULL) == TYQ_OK && isfinite(bits));
    /* the reason is the weight rule, not the family: family itself is accepted */
    CHECK(r != PRD2_R_FAMILY);

    /* 5: K = 1, pi = 1 mixture == Gaussian, bit for bit, on a KAT-like grid */
    int exact = 1;
    for (int i = -40; i <= 40; i++) {
        double loc = 0.37 * i, scale = 0.001 + 0.05 * (double)(i & 7);
        int64_t k = kof(loc + 0.013 * i);
        tyq_pred g = G(0, loc, scale);
        tyq_pred m;
        memset(&m, 0, sizeof m);
        m.family = TYQ_FAM_MIX; m.K = 1; m.comp[0].pi = 1.0; m.comp[0].loc = loc; m.comp[0].scale = scale;
        double bg, bm;
        int hg = 0, hm = 0;
        if (ty_qcont_bits(&g, k, &bg, &hg) != TYQ_OK || ty_qcont2_bits(&m, k, &bm, &hm) != TYQ_OK) { exact = 0; break; }
        if (memcmp(&bg, &bm, sizeof bg) != 0 || hg != hm) exact = 0;
    }
    R28("5 K=1 pi=1 mixture bit-identical to Gaussian", exact);

    /* 6: eight identical components earn nothing */
    double worst = 0.0;
    for (int i = -20; i <= 20; i++) {
        double loc = 0.1 * i, scale = 0.2;
        int64_t k = kof(loc + 0.05);
        tyq_pred g = G(0, loc, scale), m;
        memset(&m, 0, sizeof m);
        m.family = TYQ_FAM_MIX; m.K = 8;
        for (int j = 0; j < 8; j++) { m.comp[j].pi = 0.125; m.comp[j].loc = loc; m.comp[j].scale = scale; }
        double bg, bm;
        CHECK(ty_qcont_bits(&g, k, &bg, NULL) == TYQ_OK && ty_qcont2_bits(&m, k, &bm, NULL) == TYQ_OK);
        if (fabs(bg - bm) > worst) worst = fabs(bg - bm);
    }
    R28("6 eight identical components within 1e-12 of one", worst <= 1e-12);
}

static void test_wire(void)
{
    /* round trip, refusals by reason */
    tyq_pred rec[3] = { G(128, 0.5, 0.25), M2(129, 0.25, -1.0, 0.5, 0.75, 2.0, 0.125), G(130, 0.0, 1.0) };
    ty_prd2 p = { 3, 128, rec }, d;
    uint8_t *b; size_t len; int r;
    CHECK(ty_prd2_encode(&p, &b, &len) == TYQ_OK && len == 16u + 3 * 16u + 4 * 24u);
    CHECK(ty_prd2_parse(b, len, 128, 3, &d, &r) == TYQ_OK && d.n == 3 && d.rec[1].K == 2 && d.rec[1].comp[1].loc == 2.0 && d.rec[2].loc == 0.0);
    uint8_t *e; size_t elen;
    CHECK(ty_prd2_encode(&d, &e, &elen) == TYQ_OK && elen == len && memcmp(e, b, len) == 0);
    free(e); ty_prd2_free(&d);
    CHECK(ty_prd2_parse(b, len, 128, 4, &d, &r) == TYQ_FAIL_PROTOCOL && r == PRD2_R_COUNT);
    CHECK(ty_prd2_parse(b, len, 129, 3, &d, &r) == TYQ_FAIL_PROTOCOL && r == PRD2_R_COUNT);
    CHECK(ty_prd2_parse(b, len - 1, 128, 3, &d, &r) == TYQ_FAIL_PROTOCOL && r == PRD2_R_SIZE);
    { uint8_t *m = malloc(len + 1); memcpy(m, b, len); m[len] = 0;
      CHECK(ty_prd2_parse(m, len + 1, 128, 3, &d, &r) == TYQ_FAIL_PROTOCOL && r == PRD2_R_SIZE); free(m); }
    { uint8_t *m = malloc(len); memcpy(m, b, len); m[5] = 3;
      CHECK(ty_prd2_parse(m, len, 128, 3, &d, &r) == TYQ_FAIL_PROTOCOL && r == PRD2_R_VERSION); free(m); }
    { uint8_t *m = malloc(len); memcpy(m, b, len); m[16 + 12] = 1;               /* reserved of record 0 */
      CHECK(ty_prd2_parse(m, len, 128, 3, &d, &r) == TYQ_FAIL_PROTOCOL && r == PRD2_R_RESERVED); free(m); }
    { uint8_t *m = malloc(len); memcpy(m, b, len); m[16 + 4] = 1;                /* family 1 Student-t */
      CHECK(ty_prd2_parse(m, len, 128, 3, &d, &r) == TYQ_FAIL_PROTOCOL && r == PRD2_R_FAMILY); free(m); }
    { uint8_t *m = malloc(len); memcpy(m, b, len); m[16 + 8] = 9;                /* K = 9 */
      CHECK(ty_prd2_parse(m, len, 128, 3, &d, &r) == TYQ_FAIL_PROTOCOL && r == PRD2_R_K); free(m); }
    /* N1: v1 bytes into v2 reader, v2 bytes into the unchanged v1 reader */
    ty_prd v1 = { 1, 7, rec }; uint8_t *vb; size_t vlen;
    tyq_pred g7 = G(7, 1.0, 1.0); v1.rec = &g7;
    CHECK(ty_prd_encode(&v1, &vb, &vlen) == TYQ_OK && vlen == 32u);
    CHECK(ty_prd2_parse(vb, vlen, 7, 1, &d, &r) == TYQ_FAIL_PROTOCOL && r == PRD2_R_MAGIC);
    printf("N1 PRD1 bytes to PRD2 reader: refused (MAGIC)\n");
    ty_prd dv;
    CHECK(ty_prd_parse(b, len, 128, 3, &dv) == TYQ_FAIL_PROTOCOL);
    printf("N1 PRD2 bytes to PRD1 reader: refused\n");
    free(vb); free(b);
    /* encode refuses an invalid record */
    tyq_pred bad = M2(0, 0.5, 0.0, 1.0, 0.4, 0.0, 1.0); ty_prd2 pb = { 1, 0, &bad };
    CHECK(ty_prd2_encode(&pb, &b, &len) == TYQ_FAIL_PROTOCOL);
    CHECK(ty_prd2_validate(NULL, &r) == TYQ_E_ARG);
    CHECK(ty_prd2_parse(NULL, 0, 0, 1, &d, &r) == TYQ_E_ARG);
}

/* fixed-seed generator for the synthetic path */
static uint64_t st = 0x9E3779B97F4A7C15ull;
static double u01(void)
{
    st ^= st >> 12; st ^= st << 25; st ^= st >> 27;
    uint64_t v = st * 2685821657736338717ull;
    return ((double)(v >> 11) + 0.5) / 9007199254740992.0;
}
static double gauss(void)
{
    double a = u01(), b = u01();
    return sqrt(-2.0 * log(a)) * cos(6.283185307179586 * b);
}

#define NPTS 2000
static void test_controls(void)
{
    static double y[NPTS + 1];
    static int64_t k[NPTS];
    static tyq_pred H[NPTS], C[NPTS], S[NPTS], P[NPTS];
    const double sd = 0.05;
    y[0] = 0.0;
    for (int i = 0; i < NPTS; i++) {
        y[i + 1] = y[i] + sd * gauss();
        k[i] = kof(y[i + 1]);
        H[i] = G((uint32_t)i, y[i], sd);                               /* honest: previous value, true sd */
        C[i] = M2((uint32_t)i, 0.5, 0.0, sd, 0.5, 0.0, sd);             /* N2 constant predictor as legal mixture */
        P[i] = M2((uint32_t)i, 0.5, y[i], sd, 0.5, y[i], 1e6);          /* N4 padded far component */
    }
    for (int i = 0; i < NPTS; i++)                                      /* N3 fixed permutation (stride) */
        S[i] = G((uint32_t)i, y[(i * 7919) % NPTS], sd);
    int64_t h, c, s, p; uint64_t fh;
    CHECK(ty_qcont2_sum_ub(H, k, NPTS, &h, &fh) == TYQ_OK && fh == 0);
    CHECK(ty_qcont2_sum_ub(C, k, NPTS, &c, &fh) == TYQ_OK);
    CHECK(ty_qcont2_sum_ub(S, k, NPTS, &s, &fh) == TYQ_OK);
    CHECK(ty_qcont2_sum_ub(P, k, NPTS, &p, &fh) == TYQ_OK);
    int64_t hv; CHECK(ty_qcont_sum_ub(H, k, NPTS, &hv, &fh) == TYQ_OK && hv == h);   /* v2 == v1 on Gaussians */
    double per_h = (double)h / 1e6 / NPTS, per_c = (double)c / 1e6 / NPTS, per_s = (double)s / 1e6 / NPTS, per_p = (double)p / 1e6 / NPTS;
    printf("bits/point honest %.4f constant %.4f shuffled %.4f padded %.4f\n", per_h, per_c, per_s, per_p);
    CHECK(c > h); printf("N2 constant predictor loses to honest: %s\n", c > h ? "yes" : "NO");
    CHECK(s > h); printf("N3 shuffled predictions lose to honest: %s\n", s > h ? "yes" : "NO");
    CHECK(per_p - per_h >= 0.9); printf("N4 padded mixture costs %.4f bits/point (>= 0.9 required): %s\n", per_p - per_h, per_p - per_h >= 0.9 ? "yes" : "NO");
}

int main(void)
{
    fesetround(FE_TONEAREST);
    test_row28();
    test_wire();
    test_controls();
    printf("test_ty_prd2: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
