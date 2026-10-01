/* ESTIMATION-2 protocol v3: hostile suite for est_pred (numerics, contract,
 * adversarial calibration scenarios). Deterministic (splitmix64, fixed seeds),
 * reads no files. Every CHECK counts; any failure exits nonzero. */
#include "est_pred.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_ST(expr, want) do { est_status s_ = (expr); g_checks++; if (s_ != (want)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s -> %d, want %d\n", __FILE__, __LINE__, #expr, (int)s_, (int)(want)); } } while (0)
#define CHECK_NEAR(x, y, tol) do { double x_ = (x), y_ = (y); g_checks++; if (!(fabs(x_ - y_) <= (tol))) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s = %.17g vs %.17g (tol %g)\n", __FILE__, __LINE__, #x, x_, y_, (double)(tol)); } } while (0)

#define Q 100.0
#define PI 3.14159265358979323846

/* ------------------------------------------------------------ PRNG */
typedef struct { uint64_t s; } rng;
static uint64_t rnext(rng *r)
{
    uint64_t z = (r->s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static double runif(rng *r) { return ((double)(rnext(r) >> 11) + 0.5) / 9007199254740992.0; }
static double rnorm(rng *r) { return sqrt(-2.0 * log(runif(r))) * cos(2.0 * PI * runif(r)); }
static double rt(rng *r, int nu)
{
    double c = 0.0;
    for (int i = 0; i < nu; i++) { double g = rnorm(r); c += g * g; }
    return rnorm(r) / sqrt(c / nu);
}
static double grid(double v) { return Q * nearbyint(v / Q); }

/* ---------------------------------------------------------- helpers */
static est_assumption mk(est_family f, uint32_t np, const double *p)
{
    est_assumption a;
    memset(&a, 0, sizeof a);
    a.family = f;
    a.unit = EST_UNIT_MILLI_CELSIUS;
    a.quantum = Q;
    a.lo = -1e8;
    a.hi = 1e8;
    a.nparam = np;
    for (uint32_t i = 0; i < np; i++) a.param[i] = p[i];
    return a;
}
static est_assumption f1(double qp, double r, double p0) { double p[3] = { qp, r, p0 }; return mk(EST_FAM_GAUSS_KF, 3, p); }
static est_assumption f2(double qp, double r, double p0, double c) { double p[4] = { qp, r, p0, c }; return mk(EST_FAM_HUBER_KF, 4, p); }
static est_assumption f3(double nu, double s) { double p[2] = { nu, s }; return mk(EST_FAM_STUDENT_T, 2, p); }
static est_assumption f4(double k, double w0, double w1, double w2, double v0, double v1, double v2)
{ double p[7] = { k, w0, w1, w2, v0, v1, v2 }; return mk(EST_FAM_SCALE_MIX, 7, p); }
static est_assumption f5(double lam, double nu, double c, double fl, double s0)
{ double p[5] = { lam, nu, c, fl, s0 }; return mk(EST_FAM_ADAPTIVE_T, 5, p); }

static est_digest evd(uint64_t t)
{
    est_digest d;
    for (int i = 0; i < 32; i++) d.b[i] = (uint8_t)((t + 1u) * 0x9Du >> (i % 8) ^ (uint64_t)i ^ 0x5Au);
    d.b[0] = 0xEE; /* never all zero */
    return d;
}
static est_pobs pob(const est_assumption *a, double v, int present, uint64_t t)
{
    est_pobs o;
    memset(&o, 0, sizeof o);
    est_digest d = evd(t);
    CHECK_ST(est_pobs_classify(a, v, present, &d, &o), EST_OK);
    return o;
}
static int pmf_ok(const est_dpred *p)
{
    double s = 0.0;
    for (int i = 0; i < EST_PRED_N; i++) {
        if (!(p->pmf[i] > 0.0) || !isfinite(p->pmf[i])) return 0;
        s += p->pmf[i];
    }
    return fabs(s - 1.0) <= 1e-12;
}

/* -------------------------------------------------------- stream runner */
#define NMAX 10000
static double g_y[NMAX];
static int g_pres[NMAX];
static double g_lvl[NMAX];
static double g_xh[NMAX];

typedef struct {
    est_calib c1, c3;
    uint64_t pmf_bad;
    est_bstate b;
} run_res;

/* One-step scoring of ticks [lo, hi) via est_pred_innov; if h3, non-overlapping
 * 3-ahead forecasts every third tick scored via est_dpred_score. */
static void run(const est_assumption *a, int n, int lo, int hi, int h3, run_res *rr)
{
    static est_dpred p1, p3;
    memset(rr, 0, sizeof *rr);
    est_calib_init(&rr->c1);
    est_calib_init(&rr->c3);
    CHECK_ST(est_pred_init(a, &rr->b), EST_OK);
    est_bstate *b = &rr->b;
    int have3 = 0, target3 = -1;
    for (int t = 0; t < n; t++) {
        int have1 = 0;
        if (h3 && t % 3 == 0 && b->has_anchor && !have3) {
            if (est_pred_predict(a, b, 3, &p3) == EST_OK) { have3 = 1; target3 = t + 2; }
            else g_fail++;
        }
        if (b->has_anchor) {
            est_status st = est_pred_predict(a, b, 1, &p1);
            if (st != EST_OK) { g_fail++; fprintf(stderr, "predict t=%d st=%d\n", t, (int)st); }
            else { have1 = 1; if (!pmf_ok(&p1)) rr->pmf_bad++; }
        }
        est_pobs o = pob(a, g_y[t], g_pres[t], (uint64_t)t);
        if (have1) {
            est_pinnov iv;
            est_status st = est_pred_innov(b, &p1, &o, &iv);
            if (st != EST_OK) { g_fail++; fprintf(stderr, "innov t=%d st=%d\n", t, (int)st); }
            else if (iv.valid && t >= lo && t < hi) {
                if (est_calib_add(&rr->c1, &p1, &iv) != EST_OK) g_fail++;
            }
        }
        if (have3 && t == target3) {
            est_pinnov iv;
            if (est_dpred_score(&p3, &o, &iv) != EST_OK) g_fail++;
            else if (iv.valid && est_calib_add(&rr->c3, &p3, &iv) != EST_OK) g_fail++;
            have3 = 0;
        }
        if (est_pred_update(a, b, &o) != EST_OK) { g_fail++; fprintf(stderr, "update t=%d\n", t); }
        g_xh[t] = b->xhat;
    }
}

static est_verdict verdict(const est_calib *c, char *why)
{
    est_bands bd;
    est_bands_default(&bd);
    return est_calib_verdict(c, &bd, why, 128);
}

#define EXPECT(c, want, name) do { char w_[128]; est_verdict v_ = verdict((c), w_); g_checks++; \
    printf("  %-44s n=%-5llu %-16s %s\n", (name), (unsigned long long)(c)->n, \
           v_ == EST_CALIBRATED ? "CALIBRATED" : "NOT_CALIBRATED", w_); \
    if (v_ != (want)) { g_fail++; fprintf(stderr, "FAIL scenario %s: %s\n", (name), w_); } } while (0)

/* ---------------------------------------------------------- numerics */
static void test_numerics(void)
{
    /* est_betai known values */
    for (double x = 0.0; x <= 1.0; x += 0.125) {
        CHECK_NEAR(est_betai(1, 1, x), x, 1e-14);
        CHECK_NEAR(est_betai(3.5, 1, x), pow(x, 3.5), 1e-13);
        CHECK_NEAR(est_betai(1, 2.5, x), 1.0 - pow(1.0 - x, 2.5), 1e-13);
    }
    CHECK_NEAR(est_betai(2, 3, 0.4), 0.5248, 1e-14);
    CHECK_NEAR(est_betai(7.3, 7.3, 0.5), 0.5, 1e-13);
    CHECK_NEAR(est_betai(0.5, 0.5, 0.25), 2.0 / PI * asin(0.5), 1e-13);
    CHECK_NEAR(est_betai(4, 6, 0.3) + est_betai(6, 4, 0.7), 1.0, 1e-13);
    CHECK(isnan(est_betai(-1, 2, 0.5)));
    CHECK(isnan(est_betai(1, 0, 0.5)));
    CHECK(isnan(est_betai(1, 1, 1.5)));
    CHECK(isnan(est_betai(1, 1, NAN)));

    /* Student-t vs closed forms */
    double ts[] = { -1e6, -300, -12, -3, -1, -0.25, 0, 0.25, 1, 3, 12, 300, 1e6 };
    for (size_t i = 0; i < sizeof ts / sizeof ts[0]; i++) {
        double t = ts[i];
        double c1 = t < 0 ? atan(-1.0 / t) / PI : 0.5 + atan(t) / PI; /* nu = 1 */
        if (t < 0) CHECK(fabs(est_t_cdf(t, 1) - c1) <= 1e-12 * c1);
        else CHECK_NEAR(est_t_cdf(t, 1), c1, 1e-14);
        double r2 = sqrt(2.0 + t * t);
        double sf2 = t >= 0 ? 1.0 / (r2 * (r2 + t)) : 1.0 - 1.0 / (r2 * (r2 - t)); /* nu = 2 */
        if (t >= 0) CHECK(fabs(est_t_sf(t, 2) - sf2) <= 1e-12 * sf2);
        else CHECK_NEAR(est_t_sf(t, 2), sf2, 1e-14);
        CHECK_NEAR(est_t_cdf(t, 3.7) + est_t_sf(t, 3.7), 1.0, 1e-14);
        CHECK_NEAR(est_t_cdf(t, 3.7), est_t_sf(-t, 3.7), 0.0);
    }
    CHECK(fabs(est_t_sf(1e3, 1) - atan(1e-3) / PI) <= 1e-12 * est_t_sf(1e3, 1));
    CHECK(isnan(est_t_cdf(1, 0)) && isnan(est_t_cdf(NAN, 3)) && isnan(est_t_sf(1, -2)));
    CHECK(est_t_sf(INFINITY, 3) == 0.0 && est_t_cdf(INFINITY, 3) == 1.0);
    for (double x = -6; x <= 6; x += 0.5)
        CHECK_NEAR(est_t_cdf(x, 1e5), est_norm_cdf(x), 1e-5);

    /* norm ppf */
    CHECK(est_norm_ppf(0.0) == -8.0 && est_norm_ppf(1.0) == 8.0);
    CHECK(est_norm_ppf(-1.0) == -8.0 && est_norm_ppf(2.0) == 8.0);
    CHECK(fabs(est_norm_ppf(0.5)) <= 1e-15);
    CHECK_NEAR(est_norm_ppf(0.975), 1.959963984540054, 1e-12);
    for (double x = -7.5; x <= 3.0; x += 0.25) CHECK_NEAR(est_norm_ppf(est_norm_cdf(x)), x, 1e-9);
    for (double p = 1e-12; p < 1.0; p = p < 0.5 ? p * 3.7 : 1.0 - (1.0 - p) / 3.7) {
        double z = est_norm_ppf(p);
        CHECK(fabs(est_norm_cdf(z) - p) <= 1e-12 * (p < 0.5 ? p : 1.0) + 1e-16);
        if (1.0 - p < 1e-12) break;
    }
    CHECK_NEAR(est_norm_ppf(1e-20), -8.0, 0.0);

    /* fractional coverage */
    CHECK_NEAR(est_frac_cover(0.0, 1.0, 0.5), 0.5, 1e-15);
    CHECK_NEAR(est_frac_cover(0.0, 1.0, 0.95), 0.95, 1e-15);
    CHECK_NEAR(est_frac_cover(0.3, 0.6, 0.5), 1.0, 0.0);
    CHECK_NEAR(est_frac_cover(0.0, 0.2, 0.5), 0.0, 0.0);
    CHECK_NEAR(est_frac_cover(0.8, 1.0, 0.5), 0.0, 0.0);
    CHECK_NEAR(est_frac_cover(0.2, 0.3, 0.5), 0.5, 1e-15);
    CHECK_NEAR(est_frac_cover(0.7, 0.9, 0.5), 0.25, 1e-15);
    CHECK_NEAR(est_frac_cover(0.5, 0.5, 0.5), 1.0, 0.0);   /* point inside */
    CHECK_NEAR(est_frac_cover(0.25, 0.25, 0.5), 1.0, 0.0); /* point on the edge */
    CHECK_NEAR(est_frac_cover(0.1, 0.1, 0.5), 0.0, 0.0);   /* point outside */
    CHECK_NEAR(est_frac_cover(0.0, 0.0, 0.95), 0.0, 0.0);
    CHECK_NEAR(est_frac_cover(1.0, 1.0, 0.95), 0.0, 0.0);
}

/* ---------------------------------------------------------- contract */
static void test_assumption(void)
{
    est_assumption ok[5] = { f1(1e4, 1e3, 1e5), f2(1e4, 1e3, 1e5, 1.345), f3(3, 150),
                             f4(3, 0.5, 0.3, 0.2, 1e3, 1e4, 1e5), f5(0.9, 3, 1, 833.3, 28.9) };
    for (int i = 0; i < 5; i++) CHECK_ST(est_assumption_check(&ok[i]), EST_OK);
    est_assumption a2 = f4(2, 0.5, 0.5, 0, 1e3, 1e4, 0);
    CHECK_ST(est_assumption_check(&a2), EST_OK);
    CHECK_ST(est_assumption_check(NULL), EST_ERR_NULL);

    est_assumption a = ok[2];
    a.nparam = 3; CHECK_ST(est_assumption_check(&a), EST_ERR_DIM);
    a = ok[2]; a.nparam = 1; CHECK_ST(est_assumption_check(&a), EST_ERR_DIM);
    a = ok[2]; a.param[2] = 1.0; CHECK_ST(est_assumption_check(&a), EST_ERR_DIM);
    a = ok[0]; a.param[7] = -0.5; CHECK_ST(est_assumption_check(&a), EST_ERR_DIM);
    a = ok[2]; a.quantum = 0; CHECK_ST(est_assumption_check(&a), EST_ERR_NOT_PD);
    a = ok[2]; a.quantum = -100; CHECK_ST(est_assumption_check(&a), EST_ERR_NOT_PD);
    a = ok[2]; a.quantum = NAN; CHECK_ST(est_assumption_check(&a), EST_ERR_NONFINITE);
    a = ok[2]; a.quantum = INFINITY; CHECK_ST(est_assumption_check(&a), EST_ERR_NONFINITE);
    a = ok[2]; a.lo = a.hi; CHECK_ST(est_assumption_check(&a), EST_ERR_ENCODING);
    a = ok[2]; a.lo = 5; a.hi = 4; CHECK_ST(est_assumption_check(&a), EST_ERR_ENCODING);
    a = ok[2]; a.hi = NAN; CHECK_ST(est_assumption_check(&a), EST_ERR_NONFINITE);
    a = ok[2]; a.param[1] = NAN; CHECK_ST(est_assumption_check(&a), EST_ERR_NONFINITE);
    a = ok[2]; a.param[5] = NAN; CHECK_ST(est_assumption_check(&a), EST_ERR_NONFINITE);
    a = ok[2]; a.param[1] = 0; CHECK_ST(est_assumption_check(&a), EST_ERR_NOT_PD);
    a = ok[2]; a.param[0] = -3; CHECK_ST(est_assumption_check(&a), EST_ERR_NOT_PD);
    a = ok[2]; a.family = (est_family)0; CHECK_ST(est_assumption_check(&a), EST_ERR_KIND);
    a = ok[2]; a.family = (est_family)6; CHECK_ST(est_assumption_check(&a), EST_ERR_KIND);
    a = ok[2]; a.unit = EST_UNIT_NONE; CHECK_ST(est_assumption_check(&a), EST_ERR_UNIT);
    a = ok[0]; a.param[0] = 0; a.param[1] = 0; CHECK_ST(est_assumption_check(&a), EST_ERR_ENCODING);
    a = ok[0]; a.param[1] = -1; CHECK_ST(est_assumption_check(&a), EST_ERR_NOT_PD);
    a = ok[1]; a.param[3] = 0; CHECK_ST(est_assumption_check(&a), EST_ERR_NOT_PD);
    a = ok[3]; a.param[0] = 4; CHECK_ST(est_assumption_check(&a), EST_ERR_DIM);
    a = ok[3]; a.param[0] = 2.5; CHECK_ST(est_assumption_check(&a), EST_ERR_DIM);
    a = ok[3]; a.param[1] = 0.6; CHECK(est_assumption_check(&a) != EST_OK);
    a = ok[3]; a.param[5] = 0; CHECK(est_assumption_check(&a) != EST_OK);
    a = a2; a.param[6] = 5; CHECK_ST(est_assumption_check(&a), EST_ERR_DIM);
    a = ok[4]; a.param[0] = 1.5; CHECK_ST(est_assumption_check(&a), EST_ERR_ENCODING);
    a = ok[4]; a.param[3] = 0; CHECK_ST(est_assumption_check(&a), EST_ERR_NOT_PD);
    est_digest d;
    a = ok[2]; a.nparam = 5; CHECK(est_assumption_digest(&a, &d) != EST_OK);

    /* digest: stable, and sensitive to every field */
    for (int i = 0; i < 5; i++) {
        est_digest d0, d1, dx;
        CHECK_ST(est_assumption_digest(&ok[i], &d0), EST_OK);
        CHECK_ST(est_assumption_digest(&ok[i], &d1), EST_OK);
        CHECK(memcmp(d0.b, d1.b, 32) == 0);
        a = ok[i]; a.unit = EST_UNIT_WATT;
        CHECK(est_assumption_digest(&a, &dx) == EST_OK && memcmp(d0.b, dx.b, 32) != 0);
        a = ok[i]; a.quantum = 50;
        CHECK(est_assumption_digest(&a, &dx) == EST_OK && memcmp(d0.b, dx.b, 32) != 0);
        a = ok[i]; a.lo = -2e8;
        CHECK(est_assumption_digest(&a, &dx) == EST_OK && memcmp(d0.b, dx.b, 32) != 0);
        a = ok[i]; a.hi = 2e8;
        CHECK(est_assumption_digest(&a, &dx) == EST_OK && memcmp(d0.b, dx.b, 32) != 0);
        for (uint32_t j = 0; j < ok[i].nparam; j++) {
            a = ok[i];
            if (i == 3) { if (j == 0) continue; /* k change needs a matching vector, below */
                if (j >= 1 && j <= 3) { a.param[1] -= 0.25; a.param[2] += 0.25; }
                else a.param[j] *= 1.5;
            } else if (i == 4 && j == 0) a.param[0] = 0.8;
            else a.param[j] *= 1.5;
            CHECK(est_assumption_digest(&a, &dx) == EST_OK && memcmp(d0.b, dx.b, 32) != 0);
        }
    }
    est_digest da, db;
    est_assumption x = f3(3, 150), y = f3(3, 150);
    x.lo = 0.0; x.hi = 1e8; y.lo = -0.0; y.hi = 1e8;
    CHECK(est_assumption_digest(&x, &da) == EST_OK && est_assumption_digest(&y, &db) == EST_OK &&
          memcmp(da.b, db.b, 32) == 0);
    /* family is part of identity */
    est_assumption g1 = f1(1, 2, 3), g2 = f2(1, 2, 3, 4);
    CHECK(est_assumption_digest(&g1, &da) == EST_OK && est_assumption_digest(&g2, &db) == EST_OK &&
          memcmp(da.b, db.b, 32) != 0);
    g2.nparam = 3; g2.param[3] = 0; /* F2 with F1's vector is refused */
    CHECK_ST(est_assumption_digest(&g2, &db), EST_ERR_DIM);
}

static void test_classify(void)
{
    est_assumption a = f3(3, 150);
    a.lo = 0; a.hi = 100000;
    est_digest ev = evd(1), zero;
    memset(&zero, 0, sizeof zero);
    est_pobs o;
    CHECK_ST(est_pobs_classify(&a, 500, 1, &zero, &o), EST_ERR_KIND);
    CHECK_ST(est_pobs_classify(&a, 500, 1, NULL, &o), EST_ERR_NULL);
    est_assumption bad = a; bad.quantum = -1;
    CHECK(est_pobs_classify(&bad, 500, 1, &ev, &o) != EST_OK);
    /* order: MISSING, NONFINITE, OUT_OF_RANGE, OFF_GRID, OK */
    CHECK(est_pobs_classify(&a, NAN, 0, &ev, &o) == EST_OK && o.cls == EST_OBS_MISSING && isnan(o.value));
    CHECK(est_pobs_classify(&a, 1e9 + 37, 0, &ev, &o) == EST_OK && o.cls == EST_OBS_MISSING);
    CHECK(est_pobs_classify(&a, NAN, 1, &ev, &o) == EST_OK && o.cls == EST_OBS_NONFINITE && isnan(o.value));
    CHECK(est_pobs_classify(&a, -INFINITY, 1, &ev, &o) == EST_OK && o.cls == EST_OBS_NONFINITE);
    CHECK(est_pobs_classify(&a, 1e9 + 37, 1, &ev, &o) == EST_OK && o.cls == EST_OBS_OUT_OF_RANGE &&
          o.value == 1e9 + 37);
    CHECK(est_pobs_classify(&a, -100, 1, &ev, &o) == EST_OK && o.cls == EST_OBS_OUT_OF_RANGE);
    CHECK(est_pobs_classify(&a, 100100, 1, &ev, &o) == EST_OK && o.cls == EST_OBS_OUT_OF_RANGE);
    CHECK(est_pobs_classify(&a, 537, 1, &ev, &o) == EST_OK && o.cls == EST_OBS_OFF_GRID && o.value == 537);
    CHECK(est_pobs_classify(&a, 500.001, 1, &ev, &o) == EST_OK && o.cls == EST_OBS_OFF_GRID);
    CHECK(est_pobs_classify(&a, 500 + 1e-8, 1, &ev, &o) == EST_OK && o.cls == EST_OBS_OK);
    CHECK(est_pobs_classify(&a, 0, 1, &ev, &o) == EST_OK && o.cls == EST_OBS_OK);
    CHECK(est_pobs_classify(&a, 100000, 1, &ev, &o) == EST_OK && o.cls == EST_OBS_OK);
    CHECK(memcmp(o.evidence.b, ev.b, 32) == 0);
}

static void test_state_machine(void)
{
    est_assumption a = f3(3, 150);
    est_bstate b;
    est_dpred p, p2;
    est_pinnov iv;
    CHECK_ST(est_pred_init(&a, &b), EST_OK);
    CHECK(b.generation == 0 && !b.has_anchor);
    CHECK_ST(est_pred_predict(&a, &b, 1, &p), EST_ERR_STALE);
    CHECK(b.generation == 0);

    /* evidence root chains over every class; counters; bad readings never anchor */
    est_digest root;
    memset(&root, 0, sizeof root);
    uint64_t t = 0;
    double vals[] = { NAN, 37, 1e9, 0 };
    int pres[] = { 1, 1, 1, 0 };
    for (int i = 0; i < 4; i++) {
        est_pobs o = pob(&a, vals[i], pres[i], t);
        est_digest r2, before = b.evidence_root;
        CHECK_ST(est_pred_update(&a, &b, &o), EST_OK);
        CHECK(est_evidence_root_extend(&root, &o.evidence, &r2) == EST_OK);
        root = r2;
        CHECK(memcmp(b.evidence_root.b, root.b, 32) == 0 && memcmp(before.b, root.b, 32) != 0);
        CHECK(b.generation == ++t);
        CHECK(!b.has_anchor);
    }
    CHECK(b.count[EST_OBS_NONFINITE] == 1 && b.count[EST_OBS_OFF_GRID] == 1 &&
          b.count[EST_OBS_OUT_OF_RANGE] == 1 && b.count[EST_OBS_MISSING] == 1 && b.count[EST_OBS_OK] == 0);
    CHECK_ST(est_pred_predict(&a, &b, 1, &p), EST_ERR_STALE);

    est_pobs o = pob(&a, 25000, 1, t);
    CHECK_ST(est_pred_update(&a, &b, &o), EST_OK);
    t++;
    CHECK(b.has_anchor && b.anchor == 25000 && b.gap == 0 && b.generation == t && b.count[EST_OBS_OK] == 1);

    /* mismatched class records are refused and change nothing */
    est_bstate keep = b;
    est_pobs bad = o; bad.value = NAN;                 /* says OK, value NaN */
    CHECK_ST(est_pred_update(&a, &b, &bad), EST_ERR_KIND);
    bad = o; bad.value = 25037;                         /* says OK, off grid */
    CHECK_ST(est_pred_update(&a, &b, &bad), EST_ERR_KIND);
    bad = o; bad.cls = EST_OBS_OFF_GRID;                /* says off grid, on grid */
    CHECK_ST(est_pred_update(&a, &b, &bad), EST_ERR_KIND);
    bad = o; bad.cls = EST_OBS_NONFINITE;
    CHECK_ST(est_pred_update(&a, &b, &bad), EST_ERR_KIND);
    bad = o; bad.cls = (est_obs_class)9;
    CHECK_ST(est_pred_update(&a, &b, &bad), EST_ERR_KIND);
    bad = o; memset(&bad.evidence, 0, sizeof bad.evidence);
    CHECK_ST(est_pred_update(&a, &b, &bad), EST_ERR_KIND);
    est_assumption other = f3(3, 151);
    CHECK_ST(est_pred_update(&other, &b, &o), EST_ERR_MODEL);
    CHECK_ST(est_pred_predict(&other, &b, 1, &p), EST_ERR_MODEL);
    CHECK(memcmp(&keep, &b, sizeof b) == 0);

    /* generation +1 per predict; innov staleness */
    CHECK_ST(est_pred_predict(&a, &b, 1, &p), EST_OK);
    CHECK(b.generation == ++t && p.generation == t && p.horizon == 1 && p.anchor == 25000 && pmf_ok(&p));
    est_pobs o2 = pob(&a, 25100, 1, 99);
    CHECK_ST(est_pred_innov(&b, &p, &o2, &iv), EST_OK);
    CHECK(iv.valid == 1 && iv.k == 1 && iv.F_lo > 0.5 && iv.F_hi > iv.F_lo);
    CHECK_ST(est_pred_predict(&a, &b, 1, &p2), EST_OK);
    t++;
    CHECK_ST(est_pred_innov(&b, &p, &o2, &iv), EST_ERR_STALE);   /* older generation */
    CHECK_ST(est_pred_innov(&b, &p2, &o2, &iv), EST_OK);
    CHECK_ST(est_pred_predict(&a, &b, 2, &p), EST_OK);
    t++;
    CHECK(p.horizon == 2);
    CHECK_ST(est_pred_innov(&b, &p, &o2, &iv), EST_ERR_STALE);   /* horizon != gap + 1 */
    CHECK_ST(est_dpred_score(&p, &o2, &iv), EST_OK);              /* any horizon is scorable */
    CHECK(iv.valid == 1);
    CHECK_ST(est_pred_predict(&a, &b, 1, &p), EST_OK);
    t++;
    est_bstate b2;
    CHECK_ST(est_pred_init(&other, &b2), EST_OK);
    b2.has_anchor = 1; b2.generation = b.generation; b2.gap = b.gap;
    CHECK_ST(est_pred_innov(&b2, &p, &o2, &iv), EST_ERR_STALE);  /* other assumption */
    est_pobs miss = pob(&a, 0, 0, 7);
    CHECK_ST(est_pred_innov(&b, &p, &miss, &iv), EST_OK);
    CHECK(iv.valid == 0 && iv.cls == EST_OBS_MISSING);
    est_pinnov dummy;
    est_calib cc;
    est_calib_init(&cc);
    CHECK_ST(est_calib_add(&cc, &p, &iv), EST_ERR_KIND);
    CHECK(cc.n == 0);
    est_pobs lie = o2; lie.value = NAN;
    CHECK_ST(est_pred_innov(&b, &p, &lie, &dummy), EST_ERR_KIND);
    CHECK_ST(est_pred_update(&a, &b, &o2), EST_OK);
    t++;
    CHECK_ST(est_pred_innov(&b, &p, &o2, &iv), EST_ERR_STALE);   /* after update */
    CHECK(b.anchor == 25100 && b.generation == t);

    /* horizon limits and coasting */
    CHECK_ST(est_pred_predict(&a, &b, 0, &p), EST_ERR_TIME);
    CHECK_ST(est_pred_predict(&a, &b, EST_PRED_MAX_H + 1, &p), EST_ERR_TIME);
    CHECK(b.generation == t);
    CHECK_ST(est_pred_predict(&a, &b, EST_PRED_MAX_H, &p), EST_OK);
    t++;
    CHECK(p.horizon == EST_PRED_MAX_H && pmf_ok(&p));
    for (int i = 0; i < 60; i++) {
        est_pobs m = pob(&a, 0, 0, 1000u + (uint64_t)i);
        CHECK_ST(est_pred_update(&a, &b, &m), EST_OK);
        t++;
    }
    CHECK(b.gap == 60 && b.anchor == 25100);
    CHECK_ST(est_pred_predict(&a, &b, 5, &p), EST_ERR_TIME);
    CHECK_ST(est_pred_predict(&a, &b, 4, &p), EST_OK);
    t++;
    CHECK(p.horizon == 64 && pmf_ok(&p));
    CHECK_ST(est_pred_predict(&a, &b, 1, &p), EST_OK);
    t++;
    CHECK(p.horizon == 61);
    est_pobs o3 = pob(&a, 26000, 1, 5000);
    CHECK_ST(est_pred_innov(&b, &p, &o3, &iv), EST_OK);
    for (int i = 0; i < 70; i++) {
        est_pobs m = pob(&a, NAN, 1, 2000u + (uint64_t)i);
        CHECK_ST(est_pred_update(&a, &b, &m), EST_OK);
        t++;
    }
    CHECK_ST(est_pred_predict(&a, &b, 1, &p), EST_ERR_TIME);       /* beyond MAX_H: no forecast */
    CHECK_ST(est_pred_update(&a, &b, &o3), EST_OK);                /* but re-anchors */
    t++;
    CHECK(b.gap == 0 && b.anchor == 26000);
    CHECK_ST(est_pred_predict(&a, &b, 1, &p), EST_OK);
    t++;
    CHECK(b.generation == t && b.count[EST_OBS_NONFINITE] == 71);
}

static void naive_conv(const double *a, const double *b, double *out)
{
    for (int i = 0; i < EST_PRED_N; i++) out[i] = 0.0;
    for (int i = 0; i < EST_PRED_N; i++)
        for (int j = 0; j < EST_PRED_N; j++) {
            if (a[i] == 0.0 || b[j] == 0.0) continue;
            int t = i + j - EST_PRED_K;
            if (t < 0) t = 0;
            if (t > EST_PRED_N - 1) t = EST_PRED_N - 1;
            out[t] += a[i] * b[j];
        }
}

static double g_one[EST_PRED_N], g_out[EST_PRED_N], g_ref[EST_PRED_N], g_tmp[EST_PRED_N];

static void test_pmfs(void)
{
    rng r = { 0xC0FFEEull };
    /* conv_pow vs naive repeated convolution (support stays inside +-K) */
    for (int trial = 0; trial < 4; trial++) {
        memset(g_one, 0, sizeof g_one);
        double s = 0;
        for (int k = -5; k <= 5; k++) { g_one[EST_PRED_K + k] = runif(&r) * (k == 0 ? 4 : 1); s += g_one[EST_PRED_K + k]; }
        if (trial == 3) { s -= g_one[EST_PRED_K - 5] + g_one[EST_PRED_K + 2]; g_one[EST_PRED_K - 5] = 0; g_one[EST_PRED_K + 2] = 0; }
        for (int i = 0; i < EST_PRED_N; i++) g_one[i] /= s;
        memcpy(g_ref, g_one, sizeof g_ref);
        for (uint32_t h = 1; h <= EST_PRED_MAX_H; h++) {
            if (h > 1) { naive_conv(g_ref, g_one, g_tmp); memcpy(g_ref, g_tmp, sizeof g_ref); }
            if (h == 1 || h == 2 || h == 3 || h == 7 || h == 16 || h == 33 || h == 64) {
                CHECK_ST(est_pmf_conv_pow(g_one, h, g_out), EST_OK);
                double md = 0, tot = 0;
                for (int i = 0; i < EST_PRED_N; i++) { double d = fabs(g_out[i] - g_ref[i]); if (d > md) md = d; tot += g_out[i]; }
                CHECK(md <= 1e-14);
                CHECK_NEAR(tot, 1.0, 1e-12);
            }
        }
    }
    /* folding case: mass conserved, edges hold the overflow */
    for (int i = 0; i < EST_PRED_N; i++) g_one[i] = 1.0 / EST_PRED_N;
    CHECK_ST(est_pmf_conv_pow(g_one, 64, g_out), EST_OK);
    double tot = 0;
    for (int i = 0; i < EST_PRED_N; i++) tot += g_out[i];
    CHECK_NEAR(tot, 1.0, 1e-12);
    CHECK(g_out[0] > 0.1 && g_out[EST_PRED_N - 1] > 0.1 && fabs(g_out[0] - g_out[EST_PRED_N - 1]) < 1e-12);
    CHECK_ST(est_pmf_conv_pow(g_one, 0, g_out), EST_ERR_TIME);
    CHECK_ST(est_pmf_conv_pow(g_one, EST_PRED_MAX_H + 1, g_out), EST_ERR_TIME);
    CHECK_ST(est_pmf_conv_pow(NULL, 2, g_out), EST_ERR_NULL);
    g_one[3] = -1e-3;
    CHECK(est_pmf_conv_pow(g_one, 2, g_out) != EST_OK);

    /* every family's pmf: sums to 1, no zero bin, every horizon */
    est_assumption fams[6] = { f1(9e4, 4e4, 1e6), f2(9e4, 4e4, 1e6, 1.345), f3(3, 150),
                               f4(3, 0.6, 0.3, 0.1, 833.3, 1e4, 1e6), f5(0.9, 3, 0.6, 833.3, 28.9),
                               f3(1.5, 10) };
    static est_dpred p;
    est_bstate b;
    for (int f = 0; f < 6; f++) {
        CHECK_ST(est_pred_init(&fams[f], &b), EST_OK);
        double y = 30000;
        for (int t = 0; t < 40; t++) {
            y += grid(300 * rnorm(&r));
            est_pobs o = pob(&fams[f], (t % 7 == 3) ? NAN : y, t % 5 != 4, (uint64_t)t);
            CHECK_ST(est_pred_update(&fams[f], &b, &o), EST_OK);
            uint32_t hs[] = { 1, 2, 3, 7, 64 - b.gap };
            for (int k = 0; k < 5; k++) {
                if (t % 9 != 0 && k > 1) continue;
                CHECK_ST(est_pred_predict(&fams[f], &b, hs[k], &p), EST_OK);
                CHECK(pmf_ok(&p));
                CHECK(p.family == fams[f].family && p.quantum == Q && p.anchor == b.anchor);
            }
        }
    }
    /* est_pmf_floor on a degenerate pmf */
    memset(g_one, 0, sizeof g_one);
    g_one[EST_PRED_K] = 1.0;
    est_pmf_floor(g_one);
    tot = 0;
    int nz = 1;
    for (int i = 0; i < EST_PRED_N; i++) { tot += g_one[i]; if (!(g_one[i] > 0)) nz = 0; }
    CHECK(nz && fabs(tot - 1.0) <= 1e-12 && g_one[EST_PRED_K] > 1.0 - 2e-12);

    /* F1 closed form: N(xhat - anchor, P + H q + r) interval masses, floored */
    est_assumption a = f1(9e4, 4e4, 1e6);
    CHECK_ST(est_pred_init(&a, &b), EST_OK);
    double y = 31000;
    for (int t = 0; t < 12; t++) {
        y += grid(300 * rnorm(&r) + 120);
        est_pobs o = pob(&a, y, t != 8 && t != 9, (uint64_t)t);
        CHECK_ST(est_pred_update(&a, &b, &o), EST_OK);
    }
    CHECK(b.gap == 0 && b.xhat != b.anchor);
    uint32_t Hs[] = { 1, 5, 64 };
    for (int k = 0; k < 3; k++) {
        CHECK_ST(est_pred_predict(&a, &b, Hs[k], &p), EST_OK);
        double mu = b.xhat - b.anchor, sd = sqrt(b.P + Hs[k] * 9e4 + 4e4);
        for (int i = 0; i < EST_PRED_N; i++) {
            double lo = ((i - EST_PRED_K) - 0.5) * Q, hi = ((i - EST_PRED_K) + 0.5) * Q;
            double cl = i == 0 ? 0.0 : est_norm_cdf((lo - mu) / sd);
            double ch = i == EST_PRED_N - 1 ? 1.0 : est_norm_cdf((hi - mu) / sd);
            g_ref[i] = ch - cl;
        }
        est_pmf_floor(g_ref);
        double md = 0;
        for (int i = 0; i < EST_PRED_N; i++) { double d = fabs(p.pmf[i] - g_ref[i]); if (d > md) md = d; }
        CHECK(md <= 1e-12);
    }

    /* F3 one-step = discretized t; F5 with lambda = 1 equals F3 */
    est_assumption t3 = f3(3, 150), t5 = f5(1.0, 3, 1.0, 833.3, 150);
    est_bstate bt, b5;
    static est_dpred pt, p5;
    CHECK(est_pred_init(&t3, &bt) == EST_OK && est_pred_init(&t5, &b5) == EST_OK);
    y = 30000;
    for (int t = 0; t < 10; t++) {
        y += grid(150 * rt(&r, 3));
        est_pobs o = pob(&t3, y, t != 6, (uint64_t)t);
        CHECK(est_pred_update(&t3, &bt, &o) == EST_OK && est_pred_update(&t5, &b5, &o) == EST_OK);
    }
    CHECK(b5.s2 == 150.0 * 150.0);
    for (uint32_t h = 1; h <= 3; h++) {
        CHECK(est_pred_predict(&t3, &bt, h, &pt) == EST_OK && est_pred_predict(&t5, &b5, h, &p5) == EST_OK);
        double md = 0;
        for (int i = 0; i < EST_PRED_N; i++) { double d = fabs(pt.pmf[i] - p5.pmf[i]); if (d > md) md = d; }
        CHECK(md <= 1e-15);
    }
    CHECK_ST(est_pred_predict(&t3, &bt, 1, &pt), EST_OK);
    for (int k = -4; k <= 4; k++) {
        double ref = est_t_cdf((k + 0.5) * Q / 150, 3) - est_t_cdf((k - 0.5) * Q / 150, 3);
        CHECK_NEAR(pt.pmf[EST_PRED_K + k], ref, 1e-12);
    }
    CHECK_NEAR(pt.pmf[EST_PRED_N - 1], est_t_sf((EST_PRED_K - 0.5) * Q / 150, 3), 1e-14);
    CHECK_NEAR(pt.pmf[EST_PRED_K + 300] / (est_t_sf(299.5 * Q / 150, 3) - est_t_sf(300.5 * Q / 150, 3)), 1.0, 1e-5);

    /* F4 one-step vs est_mix_cdf_abs */
    est_assumption m4 = f4(2, 0.7, 0.3, 0, 2500, 90000, 0);
    est_mix mx = { 2, { 0.7, 0.3, 0 }, { 2500, 90000, 0 } };
    CHECK_ST(est_pred_init(&m4, &b), EST_OK);
    est_pobs o = pob(&m4, 30000, 1, 1);
    CHECK_ST(est_pred_update(&m4, &b, &o), EST_OK);
    CHECK_ST(est_pred_predict(&m4, &b, 1, &p), EST_OK);
    for (int k = 0; k <= 6; k++) {
        double c_hi, c_lo = 0;
        CHECK(est_mix_cdf_abs(&mx, 1, (k + 0.5) * Q, &c_hi) == EST_OK);
        if (k > 0) CHECK(est_mix_cdf_abs(&mx, 1, (k - 0.5) * Q, &c_lo) == EST_OK);
        double ref = k == 0 ? c_hi : 0.5 * (c_hi - c_lo);
        CHECK_NEAR(p.pmf[EST_PRED_K + k], ref, 1e-11);
        CHECK_NEAR(p.pmf[EST_PRED_K - k], ref, 1e-11);
    }

    /* width80 */
    est_assumption g = f1(1e6, 0, 0);
    CHECK_ST(est_pred_init(&g, &b), EST_OK);
    CHECK_ST(est_pred_update(&g, &b, &o), EST_OK);
    CHECK_ST(est_pred_predict(&g, &b, 1, &p), EST_OK);
    double w;
    CHECK_ST(est_dpred_width80(&p, &w), EST_OK);
    CHECK(fabs(w - 2 * 1.2816 * 1000) <= 2 * Q);
    /* scoring an outcome beyond +-K goes to the edge bin */
    est_pinnov iv;
    est_pobs far = pob(&g, 30000 + 1e6, 1, 3);
    CHECK_ST(est_dpred_score(&p, &far, &iv), EST_OK);
    CHECK(iv.valid && iv.k == EST_PRED_K && iv.F_hi == 1.0 && iv.z > 3 && isfinite(iv.logp));
    far = pob(&g, 30000 - 1e6, 1, 4);
    CHECK_ST(est_dpred_score(&p, &far, &iv), EST_OK);
    CHECK(iv.valid && iv.k == -EST_PRED_K && iv.F_lo == 0.0 && iv.z < -3);
    est_dpred bp = p;
    bp.pmf[5] = NAN;
    CHECK(est_dpred_score(&bp, &far, &iv) != EST_OK);
    bp = p; bp.horizon = 0;
    CHECK_ST(est_dpred_score(&bp, &far, &iv), EST_ERR_TIME);
}

static void test_verdict_order(void)
{
    est_bands bd;
    est_bands_default(&bd);
    CHECK(bd.cov50[0] == 0.46 && bd.cov50[1] == 0.54 && bd.cov80[0] == 0.76 && bd.cov80[1] == 0.84 &&
          bd.cov95[0] == 0.93 && bd.cov95[1] == 0.97 && bd.pit[0] == 0.07 && bd.pit[1] == 0.13 &&
          bd.bias_max == 0.10 && bd.lag1_max == 0.20 && bd.min_n == 2000);
    est_calib c;
    est_calib_init(&c);
    char why[128];
    const char *want[] = { "n ", "cov50 ", "cov80 ", "cov95 ", "pit0 ", "pit7 ", "bias ", "lag1 ", "calibrated" };
    for (int stage = 0; stage < 9; stage++) {
        est_calib_init(&c);
        double n = 3000;
        c.n = stage == 0 ? 1999 : 3000;
        c.cov[0] = (stage == 1 ? 0.60 : 0.50) * n;
        c.cov[1] = (stage <= 2 ? 0.70 : 0.80) * n;   /* bad from stage <= 2: cov50 must win at 1 */
        c.cov[2] = (stage <= 3 ? 0.99 : 0.95) * n;
        for (int j = 0; j < 10; j++) c.pit[j] = 0.1 * n;
        if (stage <= 4) { c.pit[0] = 0.05 * n; c.pit[7] = 0.15 * n; }
        if (stage == 5) { c.pit[7] = 0.14 * n; c.pit[8] = 0.06 * n; }
        c.z_sum = (stage <= 6 ? 0.2 : 0.0) * n;
        c.z_sq = n;
        c.z_lag = (stage <= 7 ? 0.5 : 0.0) * (n - 1);
        est_verdict v = est_calib_verdict(&c, &bd, why, sizeof why);
        CHECK(strncmp(why, want[stage], strlen(want[stage])) == 0);
        CHECK(v == (stage == 8 ? EST_CALIBRATED : EST_NOT_CALIBRATED));
        if (strncmp(why, want[stage], strlen(want[stage])) != 0) fprintf(stderr, "  stage %d: %s\n", stage, why);
    }
    /* lag1 from streaming sums matches a direct computation */
    rng r = { 77 };
    est_calib_init(&c);
    double z[500], prev = 0;
    for (int i = 0; i < 500; i++) { z[i] = 0.7 * prev + rnorm(&r); prev = z[i]; }
    double m = 0, sxx = 0, sxy = 0;
    for (int i = 0; i < 500; i++) {
        c.n++; c.z_sum += z[i]; c.z_sq += z[i] * z[i];
        if (i) c.z_lag += z[i] * z[i - 1];
        m += z[i];
    }
    m /= 500;
    for (int i = 0; i < 500; i++) { sxx += z[i] * z[i]; if (i) sxy += z[i] * z[i - 1]; }
    CHECK_NEAR(est_calib_lag1(&c), (sxy / 499 - m * m) / (sxx / 500 - m * m), 1e-12);
    CHECK(est_calib_lag1(&c) > 0.5);
    /* NULL reason buffer is tolerated */
    CHECK(est_calib_verdict(&c, &bd, NULL, 0) == EST_NOT_CALIBRATED);
}

/* ---------------------------------------------------------- scenarios */
static void gen_steps(rng *r, int n, double start, double (*step)(rng *, int, void *), void *ctx)
{
    double y = start;
    for (int t = 0; t < n; t++) {
        if (t) y += grid(step(r, t, ctx));
        g_lvl[t] = y;
        g_y[t] = y;
        g_pres[t] = 1;
    }
}
static double st_gauss(rng *r, int t, void *c) { (void)t; return *(double *)c * rnorm(r); }
static double st_t3(rng *r, int t, void *c) { (void)t; return *(double *)c * rt(r, 3); }
static double st_t5(rng *r, int t, void *c) { (void)t; return *(double *)c * rt(r, 5); }
static double st_drift(rng *r, int t, void *c) { (void)t; (void)c; return 150 * rnorm(r) + 60; }
static double st_regime(rng *r, int t, void *c) { return (t < *(int *)c ? 100 : 400) * rt(r, 3); }
static double st_mix(rng *r, int t, void *c) { (void)t; (void)c; return runif(r) < 0.9 ? 30 * rnorm(r) : 300 * rnorm(r); }
static double g_ar;
static double st_ar(rng *r, int t, void *c) { (void)t; (void)c; g_ar = 0.6 * g_ar + 400 * rnorm(r); return g_ar; }

static void scenarios(void)
{
    static run_res rr;
    char why[128];
    (void)why;
    rng r;

    printf("scenarios (bands fixed by protocol v3):\n");
    /* 1a. Gaussian random walk on the grid; F1 with r = 0 is exactly right */
    r.s = 101; { double s = 300; gen_steps(&r, 4000, 30000, st_gauss, &s); }
    est_assumption a = f1(9e4, 0, 0);
    run(&a, 4000, 0, 4000, 0, &rr);
    CHECK(rr.pmf_bad == 0);
    EXPECT(&rr.c1, EST_CALIBRATED, "gauss RW, F1 true (r=0)");
    a = f1(9e4 * 4, 0, 0); run(&a, 4000, 0, 4000, 0, &rr);
    EXPECT(&rr.c1, EST_NOT_CALIBRATED, "gauss RW, F1 variance x4 (too wide)");
    a = f1(9e4 / 4, 0, 0); run(&a, 4000, 0, 4000, 0, &rr);
    EXPECT(&rr.c1, EST_NOT_CALIBRATED, "gauss RW, F1 variance /4 (too narrow)");

    /* 1b. Gaussian level random walk + Gaussian noise, quantized obs */
    r.s = 102;
    { double x = 30000.37; for (int t = 0; t < 4000; t++) { x += 300 * rnorm(&r); g_lvl[t] = x;
        g_y[t] = grid(x + 200 * rnorm(&r)); g_pres[t] = 1; } }
    a = f1(9e4, 4e4 + Q * Q / 12, 1e6); run(&a, 4000, 0, 4000, 0, &rr);
    EXPECT(&rr.c1, EST_CALIBRATED, "noisy gauss RW, F1 true (r + q^2/12)");

    /* 2. heavy-tailed t3 changes */
    r.s = 103; { double s = 150; gen_steps(&r, 4000, 30000, st_t3, &s); }
    a = f3(3, 150); run(&a, 4000, 0, 4000, 0, &rr);
    EXPECT(&rr.c1, EST_CALIBRATED, "t3 changes, F3 true");
    double lp_true = rr.c1.logp_sum;
    a = f3(3, 150.0 / 4); run(&a, 4000, 0, 4000, 0, &rr);
    EXPECT(&rr.c1, EST_NOT_CALIBRATED, "t3 changes, F3 scale/4");
    CHECK(rr.c1.logp_sum < lp_true);
    a = f1(150.0 * 150.0, 0, 0); run(&a, 4000, 0, 4000, 0, &rr);
    EXPECT(&rr.c1, EST_NOT_CALIBRATED, "t3 changes, F1 small variance");
    CHECK(rr.c1.logp_sum < lp_true);

    /* 3. sparse huge spikes: Huber clips, plain Kalman is dragged */
    r.s = 104; { double s = 200; gen_steps(&r, 4000, 30000, st_gauss, &s); }
    int spikes = 0;
    for (int t = 50; t < 4000; t++) if (runif(&r) < 0.005) { g_y[t] = g_lvl[t] + 20000; spikes++; }
    CHECK(spikes > 5);
    double e1 = 0, e2 = 0;
    a = f1(4e4, 2500, 1e6); run(&a, 4000, 0, 4000, 0, &rr);
    for (int t = 50; t < 4000; t++) { double d = fabs(g_xh[t] - g_lvl[t]); if (d > e1) e1 = d; }
    CHECK(rr.pmf_bad == 0 && isfinite(rr.c1.logp_sum));
    a = f2(4e4, 2500, 1e6, 2.0); run(&a, 4000, 0, 4000, 0, &rr);
    for (int t = 50; t < 4000; t++) { double d = fabs(g_xh[t] - g_lvl[t]); if (d > e2) e2 = d; }
    CHECK(rr.pmf_bad == 0 && isfinite(rr.c1.logp_sum) && isfinite(est_calib_lag1(&rr.c1)));
    printf("  spikes: %d, max |xhat-level| F1 %.0f mC, F2 Huber %.0f mC\n", spikes, e1, e2);
    CHECK(e1 > 15000 && e2 < 1500);

    /* 4. slow drift not in the model */
    r.s = 105; gen_steps(&r, 4000, 30000, st_drift, NULL);
    a = f1(150.0 * 150.0, 0, 0); run(&a, 4000, 0, 4000, 0, &rr);
    EXPECT(&rr.c1, EST_NOT_CALIBRATED, "unmodelled drift, F1 zero-mean");

    /* 5. regime change: scale x4 halfway */
    r.s = 106; { int half = 2000; gen_steps(&r, 4000, 30000, st_regime, &half); }
    a = f3(3, 100); run(&a, 4000, 0, 4000, 0, &rr);
    EXPECT(&rr.c1, EST_NOT_CALIBRATED, "regime change, F3 fixed small scale");
    double lp_fix = rr.c1.logp_sum;
    a = f5(0.9, 3, 0.6, Q * Q / 12, 100); run(&a, 4000, 0, 4000, 0, &rr);
    printf("  regime change: F5 adaptive logp %.1f vs F3 fixed %.1f\n", rr.c1.logp_sum, lp_fix);
    CHECK(rr.c1.logp_sum > lp_fix && rr.pmf_bad == 0);

    /* 6. 20 % missing: coasting one-step and non-overlapping 3-step forecasts */
    r.s = 107; { double s = 200; gen_steps(&r, NMAX, 30000, st_t5, &s); }
    int nmiss = 0;
    for (int t = 1; t < NMAX; t++) if (runif(&r) < 0.2) { g_pres[t] = 0; g_y[t] = NAN; nmiss++; }
    a = f3(5, 200); run(&a, NMAX, 0, NMAX, 1, &rr);
    CHECK(rr.b.count[EST_OBS_MISSING] == (uint64_t)nmiss);
    EXPECT(&rr.c1, EST_CALIBRATED, "20% missing, F3 true, coasted 1-step");
    EXPECT(&rr.c3, EST_CALIBRATED, "20% missing, F3 true, 3-step dpred_score");

    /* 7. bad sensor values mixed in: counted, never anchored */
    r.s = 108; { double s = 150; gen_steps(&r, 4000, 30000, st_t3, &s); }
    uint64_t cnt[EST_OBS_CLASSES_] = { 0 };
    for (int t = 1; t < 4000; t++) {
        double u = runif(&r);
        if (u < 0.03) g_y[t] = NAN;
        else if (u < 0.06) g_y[t] = 1e9;
        else if (u < 0.09) g_y[t] = g_lvl[t] + 37;
        else if (u < 0.12) g_pres[t] = 0;
    }
    for (int t = 0; t < 4000; t++) {
        est_pobs o = pob(&a, g_y[t], g_pres[t], (uint64_t)t);
        cnt[o.cls]++;
    }
    a = f3(3, 150); run(&a, 4000, 0, 4000, 0, &rr);
    for (int k = 0; k < EST_OBS_CLASSES_; k++) CHECK(rr.b.count[k] == cnt[k]);
    CHECK(cnt[EST_OBS_NONFINITE] > 50 && cnt[EST_OBS_OUT_OF_RANGE] > 50 && cnt[EST_OBS_OFF_GRID] > 50);
    CHECK(rr.c1.n == cnt[EST_OBS_OK] - 1);
    { int last = 0; for (int t = 0; t < 4000; t++) if (g_pres[t] && isfinite(g_y[t]) && g_y[t] == g_lvl[t]) last = t;
      CHECK(rr.b.anchor == g_lvl[last]); }
    EXPECT(&rr.c1, EST_CALIBRATED, "bad readings mixed in, F3 true");

    /* 8. correlated (AR(1)) changes under an iid model with the right marginal */
    r.s = 109; g_ar = 0; gen_steps(&r, 6000, 30000, st_ar, NULL);
    a = f1(250000, 0, 0); run(&a, 6000, 0, 6000, 0, &rr);
    {
        est_verdict v = verdict(&rr.c1, why);
        printf("  %-44s n=%-5llu %-16s %s\n", "AR(1) changes, F1 iid (right marginal)",
               (unsigned long long)rr.c1.n, v == EST_CALIBRATED ? "CALIBRATED" : "NOT_CALIBRATED", why);
        CHECK(v == EST_NOT_CALIBRATED && strncmp(why, "lag1 ", 5) == 0);
    }

    /* 9. long stable run: mostly exact-zero grid changes */
    r.s = 110; { double s = 20; gen_steps(&r, 4000, 30000, st_t3, &s); }
    { int z = 0; for (int t = 1; t < 4000; t++) z += g_y[t] == g_y[t - 1]; CHECK(z > 3400);
      printf("  stable run: %.1f%% zero changes\n", 100.0 * z / 3999); }
    a = f3(3, 20); run(&a, 4000, 0, 4000, 0, &rr);
    EXPECT(&rr.c1, EST_CALIBRATED, "stable (t3 s=20), F3 matching pmf");
    a = f3(3, 2); run(&a, 4000, 0, 4000, 0, &rr);
    EXPECT(&rr.c1, EST_NOT_CALIBRATED, "stable (t3 s=20), F3 s=2 over-narrow");
    r.s = 111; gen_steps(&r, 4000, 30000, st_mix, NULL);
    a = f4(2, 0.9, 0.1, 0, 900, 90000, 0); run(&a, 4000, 0, 4000, 0, &rr);
    EXPECT(&rr.c1, EST_CALIBRATED, "stable (mixture), F4 matching pmf");

    /* 10. abrupt level jump under a near-constant-level model */
    r.s = 112;
    for (int t = 0; t < 6000; t++) { g_lvl[t] = 25030.0 + (t >= 3000 ? 5000.0 : 0.0);
        g_y[t] = grid(g_lvl[t] + 300 * rnorm(&r)); g_pres[t] = 1; }
    a = f1(1, 9e4 + Q * Q / 12, 1e6);
    run(&a, 6000, 0, 3000, 0, &rr);
    EXPECT(&rr.c1, EST_CALIBRATED, "before the jump, F1 slow level");
    run(&a, 6000, 0, 6000, 0, &rr);
    EXPECT(&rr.c1, EST_NOT_CALIBRATED, "abrupt jump, F1 slow level over-narrow");

    /* 11. quantized observations of a slowly moving continuous level */
    for (int t = 0; t < 4000; t++) { g_lvl[t] = 30000 + 2000 * sin(2 * PI * t / 2000.0);
        g_y[t] = grid(g_lvl[t]); g_pres[t] = 1; }
    a = f3(3, 1); run(&a, 4000, 0, 4000, 0, &rr);
    EXPECT(&rr.c1, EST_NOT_CALIBRATED, "slow sine quantized, F3 s=1 over-narrow");
    a = f5(0.9, 3, 0.6, Q * Q / 12, 28.9); run(&a, 4000, 0, 4000, 0, &rr);
    {
        est_verdict v = verdict(&rr.c1, why);
        printf("  %-44s n=%-5llu %-16s %s (recorded, no theory claim)\n", "slow sine quantized, F5 adaptive",
               (unsigned long long)rr.c1.n, v == EST_CALIBRATED ? "CALIBRATED" : "NOT_CALIBRATED", why);
        CHECK(rr.pmf_bad == 0 && isfinite(rr.c1.logp_sum));
    }
}

int main(void)
{
    test_numerics();
    test_assumption();
    test_classify();
    test_state_machine();
    test_pmfs();
    test_verdict_order();
    scenarios();
    printf("test_est_pred: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
