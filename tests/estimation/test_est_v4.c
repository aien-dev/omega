/* ESTIMATION v4 families (src/estimation/est_v4.c): analytic multi-step
 * propagation, ramp steady state, scale laws, missing ticks, pmf vs fast
 * score agreement, parameter checks and digests. */
#include "est_v4.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_ST(expr, want) do { est_status s_ = (expr); g_checks++; if (s_ != (want)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s = %d, want %d\n", __FILE__, __LINE__, #expr, (int)s_, (int)(want)); } } while (0)
#define CHECK_NEAR(x, y, tol) do { double x_ = (x), y_ = (y); g_checks++; if (!(fabs(x_ - y_) <= (tol))) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s = %.17g, want %.17g (tol %g)\n", __FILE__, __LINE__, #x, x_, y_, (double)(tol)); } } while (0)
#define REL(x, y, rt) CHECK_NEAR((x), (y), (rt) * fabs(y) + 1e-12)

#define QU 100.0
#define RR (QU * QU / 12.0)

static est4_params base(est4_family f)
{
    est4_params p;
    memset(&p, 0, sizeof p);
    p.family = f;
    p.dyn = f == EST4_G2_AR ? 0.6 : 4.0;
    p.q = 2500.0; p.lam = 0.7; p.nu = 1.5; p.c = 0.6;
    p.phi = 0.9; p.nu_h = 0.8; p.c_h = 0.4; p.quantum = QU;
    return p;
}

static void test_params(void)
{
    for (int f = 1; f <= 3; f++) {
        est4_params p = base((est4_family)f), b;
        CHECK_ST(est4_params_check(&p), EST_OK);
        b = p; b.family = (est4_family)4; CHECK_ST(est4_params_check(&b), EST_ERR_KIND);
        b = p; b.q = 0; CHECK(est4_params_check(&b) != EST_OK);
        b = p; b.q = NAN; CHECK_ST(est4_params_check(&b), EST_ERR_NONFINITE);
        b = p; b.lam = 1.0; CHECK(est4_params_check(&b) != EST_OK);
        b = p; b.lam = -0.1; CHECK(est4_params_check(&b) != EST_OK);
        b = p; b.phi = 0; CHECK(est4_params_check(&b) != EST_OK);
        b = p; b.phi = 1.01; CHECK(est4_params_check(&b) != EST_OK);
        b = p; b.nu = 0; CHECK(est4_params_check(&b) != EST_OK);
        b = p; b.c = -1; CHECK(est4_params_check(&b) != EST_OK);
        b = p; b.nu_h = 0; CHECK(est4_params_check(&b) != EST_OK);
        b = p; b.c_h = 0; CHECK(est4_params_check(&b) != EST_OK);
        b = p; b.c_h = INFINITY; CHECK(est4_params_check(&b) != EST_OK);
        b = p; b.quantum = 0; CHECK(est4_params_check(&b) != EST_OK);
        b = p; b.dyn = f == 2 ? 1.0 : 0.0; CHECK(est4_params_check(&b) != EST_OK);
        b = p; b.dyn = -0.1; CHECK(est4_params_check(&b) != EST_OK);
    }
    CHECK_ST(est4_params_check(NULL), EST_ERR_NULL);
    est4_state s;
    est4_params bad = base(EST4_G1_LAG); bad.nu = NAN;
    CHECK(est4_init(&bad, &s) != EST_OK);
}

static void test_digest(void)
{
    est4_params p = base(EST4_G1_LAG);
    est_digest d0, d1;
    CHECK_ST(est4_params_digest(&p, &d0), EST_OK);
    CHECK_ST(est4_params_digest(&p, &d1), EST_OK);
    CHECK(memcmp(d0.b, d1.b, 32) == 0);
    double *fields[] = { &p.dyn, &p.q, &p.lam, &p.nu, &p.c, &p.phi, &p.nu_h, &p.c_h, &p.quantum };
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        est4_params b = p;
        double *f = (double *)((char *)&b + ((char *)fields[i] - (char *)&p));
        *f = *f * 0.5;
        CHECK_ST(est4_params_digest(&b, &d1), EST_OK);
        CHECK(memcmp(d0.b, d1.b, 32) != 0);
    }
    est4_params b = p; b.family = EST4_G3_TWO;
    CHECK_ST(est4_params_digest(&b, &d1), EST_OK);
    CHECK(memcmp(d0.b, d1.b, 32) != 0);
}

/* state with an anchor, chosen x, P = 0, g = gl = gv */
static void set_state(est4_state *s, const est4_params *p, const double *x, double gv)
{
    CHECK_ST(est4_init(p, s), EST_OK);
    s->has_anchor = 1;
    s->anchor = 1000.0;
    memcpy(s->x, x, sizeof(double) * s->d);
    memset(s->P, 0, sizeof s->P);
    s->g = gv; s->gl = gv;
}

static void check_law(const est4_state *s, uint32_t h, double m, double V, double gbar)
{
    est4_pred pr;
    CHECK_ST(est4_predict(s, h, &pr), EST_OK);
    REL(pr.loc, m, 1e-12);
    if (h == 1) {
        REL(pr.scale, s->p.c * sqrt(s->g * V), 1e-12);
        CHECK(pr.nu == s->p.nu);
    } else {
        REL(pr.scale, s->p.c_h * sqrt(gbar * V), 1e-12);
        CHECK(pr.nu == s->p.nu_h);
    }
}

/* G1: m_h = U + a^h (T - U);  V_h = q sum_{j=1..h} (1 - a^{h-j})^2 + r */
static void test_g1_analytic(void)
{
    est4_params p = base(EST4_G1_LAG);
    est4_state s;
    const double x[2] = { 1000.0, 5000.0 };
    set_state(&s, &p, x, 1.7);
    double a = exp(-1.0 / p.dyn);
    for (uint32_t h = 1; h <= EST_PRED_MAX_H; h++) {
        double m = x[1] + pow(a, h) * (x[0] - x[1]), V = RR;
        for (uint32_t j = 1; j <= h; j++) V += p.q * pow(1.0 - pow(a, h - j), 2);
        check_law(&s, h, m, V, 1.7);
    }
}

/* G2: m_h = Y + D sum_{k=1..h} rho^k;  V_h = q sum_{j=1..h} (sum_{i=0..h-j} rho^i)^2 + r */
static void test_g2_analytic(void)
{
    const double rhos[3] = { 0.0, 0.6, 0.95 };
    for (int k = 0; k < 3; k++) {
        est4_params p = base(EST4_G2_AR);
        p.dyn = rhos[k];
        est4_state s;
        const double x[2] = { 2000.0, 300.0 };
        set_state(&s, &p, x, 0.8);
        double r = p.dyn;
        for (uint32_t h = 1; h <= EST_PRED_MAX_H; h++) {
            double sm = 0.0, V = RR;
            for (uint32_t i = 1; i <= h; i++) sm += pow(r, i);
            for (uint32_t j = 1; j <= h; j++) {
                double w = 0.0;
                for (uint32_t i = 0; i <= h - j; i++) w += pow(r, i);
                V += p.q * w * w;
            }
            check_law(&s, h, x[0] + x[1] * sm, V, 0.8);
        }
    }
}

/* G3: Tf_h = af^h Tf + (1-af^h) w U, Ts_h = as^h Ts + (1-as^h)(1-w) U;
 * V_h = q sum_j (w (1-af^{h-j}) + (1-w)(1-as^{h-j}))^2 + r */
static void test_g3_analytic(void)
{
    est4_params p = base(EST4_G3_TWO);
    est4_state s;
    const double x[3] = { 700.0, 300.0, 4000.0 };
    set_state(&s, &p, x, 2.5);
    double af = exp(-1.0 / p.dyn), as = exp(-1.0 / (p.dyn * EST4_TAU_RATIO)), w = EST4_G3_W;
    for (uint32_t h = 1; h <= EST_PRED_MAX_H; h++) {
        double m = pow(af, h) * x[0] + (1.0 - pow(af, h)) * w * x[2] +
                   pow(as, h) * x[1] + (1.0 - pow(as, h)) * (1.0 - w) * x[2];
        double V = RR;
        for (uint32_t j = 1; j <= h; j++) {
            double cj = w * (1.0 - pow(af, h - j)) + (1.0 - w) * (1.0 - pow(as, h - j));
            V += p.q * cj * cj;
        }
        check_law(&s, h, m, V, 2.5);
    }
}

/* horizon scale reversion: gbar_h = (1/h) sum_{j=1..h} (gl + (g - gl) phi^{j-1}) */
static void test_scale_reversion(void)
{
    est4_params p = base(EST4_G1_LAG);
    est4_state s;
    const double x[2] = { 1000.0, 1000.0 };
    set_state(&s, &p, x, 1.0);
    s.g = 9.0; s.gl = 1.0;
    est4_pred one, ten;
    CHECK_ST(est4_predict(&s, 1, &one), EST_OK);
    CHECK_ST(est4_predict(&s, 10, &ten), EST_OK);
    double gb = 0.0;
    for (int j = 1; j <= 10; j++) gb += 1.0 + 8.0 * pow(p.phi, j - 1);
    gb /= 10.0;
    double a = exp(-1.0 / p.dyn), V = RR;
    for (int j = 1; j <= 10; j++) V += p.q * pow(1.0 - pow(a, 10 - j), 2);
    REL(ten.scale, p.c_h * sqrt(gb * V), 1e-12);
    REL(one.scale, p.c * sqrt(9.0 * (RR + p.q * 0.0)), 1e-12); /* h=1: (1 - a^0)^2 = 0 */
    /* phi = 1: no reversion */
    s.p.phi = 1.0;
    CHECK_ST(est4_predict(&s, 10, &ten), EST_OK);
    REL(ten.scale, p.c_h * sqrt(9.0 * V), 1e-12);
}

/* G2 on an exact ramp y_t = s t: converges to the analytic steady state
 * e = (1-rho) s / (rho k2 + (1-rho) k1), D = (s - k1 e)/rho, with the steady
 * gain K = P- H / S from the converged covariance; the ten-step forecast is
 * then Y + D sum rho^k. Also a constant input gives m_h = y for every h. */
static void test_ramp(void)
{
    est4_params p = base(EST4_G2_AR);
    p.dyn = 0.9;
    est4_state s;
    CHECK_ST(est4_init(&p, &s), EST_OK);
    const double slope = 100.0;
    double y = 0.0;
    for (int t = 0; t < 3000; t++) { y = slope * t; CHECK_ST(est4_update(&s, 1, y), EST_OK); }
    double r = p.dyn;
    /* prior covariance P- = A P A' + Q, A = [[1, r],[0, r]] */
    double P00 = s.P[0], P01 = s.P[1], P11 = s.P[3];
    double m00 = P00 + 2 * r * P01 + r * r * P11 + p.q, m01 = r * P01 + r * r * P11 + p.q;
    double S = m00 + RR, k1 = m00 / S, k2 = m01 / S;
    double e = (1 - r) * slope / (r * k2 + (1 - r) * k1);
    double D = (slope - k1 * e) / r;
    est4_pred one, ten;
    CHECK_ST(est4_predict(&s, 1, &one), EST_OK);
    REL(slope * 3000 - one.loc, e, 1e-6);
    REL(s.x[1], D, 1e-6);
    double sm = 0.0;
    for (int k = 1; k <= 10; k++) sm += pow(r, k);
    CHECK_ST(est4_predict(&s, 10, &ten), EST_OK);
    REL(ten.loc, s.x[0] + s.x[1] * sm, 1e-12);
    /* the AR(1) change model under-forecasts a persistent ramp by a known amount */
    REL(slope * 3009 - ten.loc, slope * 3009 - (s.x[0] + D * sm), 1e-6);

    for (int f = 1; f <= 3; f++) {
        est4_params pc = base((est4_family)f);
        CHECK_ST(est4_init(&pc, &s), EST_OK);
        for (int t = 0; t < 500; t++) CHECK_ST(est4_update(&s, 1, 4200.0), EST_OK);
        for (uint32_t h = 1; h <= 64; h++) {
            CHECK_ST(est4_predict(&s, h, &ten), EST_OK);
            CHECK_NEAR(ten.loc, 4200.0, 1e-6);
        }
    }
}

/* one-step scale update g' = lam g + (1-lam) max(e^2, r) / V_1 */
static void test_scale_update(void)
{
    est4_params p = base(EST4_G1_LAG);
    est4_state s;
    CHECK_ST(est4_init(&p, &s), EST_OK);
    est4_pred pr;
    CHECK_ST(est4_predict(&s, 1, &pr), EST_ERR_STALE);
    CHECK_ST(est4_update(&s, 1, 3000.0), EST_OK);
    CHECK(s.has_anchor && s.anchor == 3000.0 && s.gap == 0);
    for (int t = 0; t < 50; t++) CHECK_ST(est4_update(&s, 1, 3000.0 + 100.0 * (t % 3)), EST_OK);
    double g0 = s.g, gl0 = s.gl;
    CHECK_ST(est4_predict(&s, 1, &pr), EST_OK);
    double V1 = pow(pr.scale / p.c, 2) / g0;
    double yv = pr.loc + 900.0;
    CHECK_ST(est4_update(&s, 1, yv), EST_OK);
    double gn = p.lam * g0 + (1 - p.lam) * 810000.0 / V1;
    REL(s.g, gn, 1e-12);
    REL(s.gl, EST4_GL_LAMBDA * gl0 + (1 - EST4_GL_LAMBDA) * gn, 1e-12);
    /* small error: floored at r */
    g0 = s.g;
    CHECK_ST(est4_predict(&s, 1, &pr), EST_OK);
    V1 = pow(pr.scale / p.c, 2) / g0;
    CHECK_ST(est4_update(&s, 1, pr.loc), EST_OK);
    REL(s.g, p.lam * g0 + (1 - p.lam) * RR / V1, 1e-12);
    CHECK_ST(est4_predict(&s, 0, &pr), EST_ERR_TIME);
    CHECK_ST(est4_predict(&s, EST_PRED_MAX_H + 1, &pr), EST_ERR_TIME);
}

/* missing ticks: time update only; one-step location after k misses equals the
 * (k+1)-step location before them; dpred horizon = gap + 1; g unchanged. */
static void test_missing(void)
{
    for (int f = 1; f <= 3; f++) {
        est4_params p = base((est4_family)f);
        est4_state s;
        CHECK_ST(est4_init(&p, &s), EST_OK);
        for (int t = 0; t < 40; t++) CHECK_ST(est4_update(&s, 1, 2000.0 + 100.0 * t), EST_OK);
        est4_pred before, after;
        CHECK_ST(est4_predict(&s, 6, &before), EST_OK);
        double g0 = s.g;
        uint64_t gen = s.generation;
        for (int k = 0; k < 5; k++) CHECK_ST(est4_update(&s, (k & 1), NAN), EST_OK);
        CHECK(s.gap == 5 && s.generation == gen + 5 && s.g == g0 && s.anchor == 2000.0 + 3900.0);
        CHECK_ST(est4_predict(&s, 1, &after), EST_OK);
        REL(after.loc, before.loc, 1e-12);
        est_dpred dp;
        CHECK_ST(est4_dpred(&s, &after, &dp), EST_OK);
        CHECK(dp.horizon == 6);
        CHECK_ST(est4_predict(&s, 59, &after), EST_OK);
        CHECK_ST(est4_dpred(&s, &after, &dp), EST_OK);
        CHECK_ST(est4_predict(&s, 60, &after), EST_OK);
        CHECK_ST(est4_dpred(&s, &after, &dp), EST_ERR_TIME);
    }
}

/* pmf: sums to 1, symmetric when centred on the anchor; the fast score
 * agrees with est_dpred_score on the floored pmf. */
static void test_pmf_vs_fast(void)
{
    unsigned long long st = 0x5EED1234ULL;
    int compared = 0;
    for (int f = 1; f <= 3; f++) {
        est4_params p = base((est4_family)f);
        const double nus[4] = { 0.6, 1.0, 1.5, 5.0 };
        for (int k = 0; k < 4; k++) {
            p.nu = nus[k]; p.nu_h = nus[3 - k];
            est4_state s;
            CHECK_ST(est4_init(&p, &s), EST_OK);
            for (int t = 0; t < 120; t++) {
                st = st * 6364136223846793005ULL + 1442695040888963407ULL;
                double v = 3000.0 + 100.0 * (double)((st >> 33) % 7) + (t > 60 ? 2000.0 : 0.0);
                CHECK_ST(est4_update(&s, 1, v), EST_OK);
            }
            for (uint32_t h = 1; h <= 10; h += 9) {
                est4_pred pr; est_dpred dp;
                CHECK_ST(est4_predict(&s, h, &pr), EST_OK);
                CHECK_ST(est4_dpred(&s, &pr, &dp), EST_OK);
                double sum = 0.0;
                for (int i = 0; i < EST_PRED_N; i++) { sum += dp.pmf[i]; CHECK(dp.pmf[i] > 0.0); }
                CHECK_NEAR(sum, 1.0, 1e-12);
                static const int far[10] = { -1000, -450, -401, -400, -399, 399, 400, 401, 450, 1000 };
                for (int ki = 0; ki < 131; ki++) {
                    int kk = ki < 121 ? ki - 60 : far[ki - 121];
                    double yv = s.anchor + kk * QU, flo, fhi;
                    double lf = est4_fast_logp(&pr, QU, s.anchor, yv, &flo, &fhi);
                    est_pobs ob; est_pinnov iv;
                    memset(&ob, 0, sizeof ob);
                    ob.value = yv; ob.cls = EST_OBS_OK; ob.evidence.b[0] = 1;
                    CHECK_ST(est_dpred_score(&dp, &ob, &iv), EST_OK);
                    {
                        /* same pmf up to the renormalisation rounding, floor and edges included */
                        CHECK_NEAR(exp(iv.logp), exp(lf), 1e-15 + 1e-11 * exp(lf));
                        CHECK_NEAR(iv.logp, lf, 1e-6);
                        CHECK_NEAR(iv.F_lo, flo, 1e-9);
                        CHECK_NEAR(iv.F_hi, fhi, 1e-9);
                        compared++;
                    }
                }
            }
        }
    }
    CHECK(compared > 1000);
    /* centred law: symmetric pmf */
    est4_params p = base(EST4_G1_LAG);
    est4_state s;
    const double x[2] = { 1000.0, 1000.0 };
    set_state(&s, &p, x, 1.0);
    est4_pred pr; est_dpred dp;
    CHECK_ST(est4_predict(&s, 10, &pr), EST_OK);
    CHECK_ST(est4_dpred(&s, &pr, &dp), EST_OK);
    double asym = 0.0;
    for (int i = 0; i < EST_PRED_N; i++) asym = fmax(asym, fabs(dp.pmf[i] - dp.pmf[EST_PRED_N - 1 - i]));
    CHECK(asym < 1e-15);
    CHECK(dp.family == (est_family)0 && dp.horizon == 10 && dp.anchor == 1000.0 && dp.quantum == QU);
}

int main(void)
{
    test_params();
    test_digest();
    test_g1_analytic();
    test_g2_analytic();
    test_g3_analytic();
    test_scale_reversion();
    test_ramp();
    test_scale_update();
    test_missing();
    test_pmf_vs_fast();
    printf("test_est_v4: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
