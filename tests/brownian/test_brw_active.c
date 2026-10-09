/* Unit tests for the BRW-ACT-DEV0 candidate library. Synthetic data is made here from a fixed-seed
 * xorshift64* and Box-Muller; expected values are computed independently of the library. */
#include "brownian/brw_active.h"
#include "turing/ty_prd2.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint64_t xs(void) { rs ^= rs >> 12; rs ^= rs << 25; rs ^= rs >> 27; return rs * 0x2545F4914F6CDD1Dull; }
static double u01(void) { return (double)((xs() >> 11) + 1) * 0x1p-53; }
static double gauss(void) { return sqrt(-2.0 * log(u01())) * cos(6.283185307179586 * u01()); }

static brw_grid G;

static void feed_model(brw_state *s, int model, double D, double v, double th, int n)
{
    for (int j = 0; j < n; j++) {
        int ti = 2 + (j % 4);                                  /* taus 4, 8, 16, 32 */
        double tau = brw_tau[ti], mu = 0.0, var;
        if (model == 2) var = (D / th) * (1.0 - exp(-2.0 * th * tau)) + 1.0;
        else var = 2.0 * D * tau + 1.0;
        if (model == 1) mu = v * tau;
        brw_observe(s, ti, mu + sqrt(var) * gauss());
    }
}

static void test_grid(void)
{
    int c[3] = {0, 0, 0};
    for (int i = 0; i < BRW_NH; i++) c[G.h[i].model]++;
    CHECK(c[0] == 16 && c[1] == 256 && c[2] == 256);
    CHECK(fabs(G.h[0].D - 0.001) < 1e-15 && fabs(G.h[15].D - 10.0) < 1e-12);
    brw_state s;
    brw_state_init(&s, &G);
    double p[3];
    brw_model_post(&s, p);
    CHECK(fabs(p[0] - 1.0 / 3) < 1e-14 && fabs(p[1] - 1.0 / 3) < 1e-14 && fabs(p[2] - 1.0 / 3) < 1e-14);
    CHECK(fabs(brw_lse(s.logw, BRW_NH)) < 1e-13);
}

static void test_lse(void)
{
    double a[2] = {-1000.0, -1000.0};
    CHECK(fabs(brw_lse(a, 2) - (-1000.0 + log(2.0))) < 1e-12);
    double b[2] = {0.0, -1e5};
    CHECK(fabs(brw_lse(b, 2)) < 1e-15);
    double c[3] = {-1.0 / 0.0, 0.0, 0.0};
    CHECK(fabs(brw_lse(c, 3) - log(2.0)) < 1e-15);
    double d[1] = {-1.0 / 0.0};
    CHECK(brw_lse(d, 1) < -1e300);
    /* after extreme readings the posterior still sums to 1 */
    brw_state s;
    brw_state_init(&s, &G);
    brw_observe(&s, 5, 150.0);
    brw_observe(&s, 0, -40.0);
    CHECK(fabs(exp(brw_lse(s.logw, BRW_NH)) - 1.0) < 1e-12);
    double p[3];
    brw_model_post(&s, p);
    CHECK(fabs(p[0] + p[1] + p[2] - 1.0) < 1e-12);
}

static void test_posterior(void)
{
    brw_state s;
    double p[3];
    brw_state_init(&s, &G);
    feed_model(&s, 1, 0.1, 0.3, 0.0, 24);                     /* drift 0.3 over tau 4..32 is unmistakable */
    brw_model_post(&s, p);
    CHECK(p[1] > 0.99);
    brw_state_init(&s, &G);
    feed_model(&s, 2, 0.5, 0.0, 0.5, 60);                     /* saturating variance */
    brw_model_post(&s, p);
    CHECK(p[2] > p[0] && p[2] > p[1]);
    brw_state_init(&s, &G);
    feed_model(&s, 0, 0.2, 0.0, 0.0, 60);
    brw_model_post(&s, p);
    CHECK(p[0] > p[1] && p[0] > p[2]);
    CHECK(s.lik_evals == 60u * BRW_NH && s.n_obs == 60);
}

static void test_entropy(void)
{
    double w = 1.0, mu = 0.7;
    const double sds[3] = {0.5, 1.0, 12.3};
    for (int k = 0; k < 3; k++) {
        double var = sds[k] * sds[k], h = brw_mix_entropy_bits(&w, &mu, &var, 1, NULL);
        double ex = 0.5 * log2(2.0 * 3.141592653589793 * 2.718281828459045 * var);
        CHECK(fabs(h - ex) < 1e-6);
    }
    uint64_t q = 0;
    double var = 1.0;
    brw_mix_entropy_bits(&w, &mu, &var, 1, &q);
    CHECK(q == BRW_NQ);
}

static void test_info(void)
{
    /* three groups with identical components: I(Y;G) = 0 */
    double w[6], mu[6], var[6];
    int grp[6];
    for (int i = 0; i < 6; i++) {
        w[i] = 1.0 / 6;
        mu[i] = (i % 2) ? 1.5 : -0.5;
        var[i] = 1.0 + 0.5 * (i % 2);
        grp[i] = i % 3;
    }
    /* groups {0,3},{1,4},{2,5}: each holds one even and one odd index? i%3 pairs (0,3) differ in parity: same set */
    double im, ih;
    brw_info_arrays(6, w, grp, mu, var, 3, &im, &ih, NULL);
    CHECK(fabs(im) < 1e-9);
    CHECK(ih > 0.0);
    /* drift vs diffusion: I(Y;M) is larger at long tau */
    double info[2];
    const int taus[2] = {1, 32};
    for (int k = 0; k < 2; k++) {
        double ww[2] = {0.5, 0.5}, m2[2] = {0.0, 0.3 * taus[k]}, v2[2] = {1.0 + 0.2 * taus[k], 1.0 + 0.2 * taus[k]};
        int g2[2] = {0, 1};
        double a, b;
        brw_info_arrays(2, ww, g2, m2, v2, 2, &a, &b, NULL);
        info[k] = a;
    }
    CHECK(info[1] > 20.0 * info[0] && info[1] > 0.5);
    /* same on the real prior state, and bounded by log2(3) */
    brw_state s;
    brw_state_init(&s, &G);
    double i1, i32, h;
    brw_info(&s, 0, &i1, &h);
    brw_info(&s, 5, &i32, &h);
    CHECK(i32 > i1 && i32 < log2(3.0) + 1e-6 && i1 >= -1e-9);
    CHECK(s.quad_evals > 0);
}

static double wmean(const double *w, const double *mu, size_t n)
{
    double t = 0, m = 0;
    for (size_t i = 0; i < n; i++) { t += w[i]; m += w[i] * mu[i]; }
    return m / t;
}
static double wvar(const double *w, const double *mu, const double *sd, size_t n)
{
    double t = 0, v = 0, m = wmean(w, mu, n);
    for (size_t i = 0; i < n; i++) { t += w[i]; v += w[i] * (sd[i] * sd[i] + (mu[i] - m) * (mu[i] - m)); }
    return v / t;
}

static void test_reduce(void)
{
    enum { N = 100 };
    double w[N], mu[N], sd[N], t = 0;
    for (int i = 0; i < N; i++) {
        w[i] = u01() * u01();
        mu[i] = 20.0 * (u01() - 0.5);
        sd[i] = 1.0 + 5.0 * u01();
        t += w[i];
    }
    for (int i = 0; i < N; i++) w[i] /= t;
    tyq_pred p;
    CHECK(brw_reduce(w, mu, sd, N, 7, &p) == TYQ_OK);
    CHECK(p.K == 8 && p.family == TYQ_FAM_MIX && p.index == 7);
    double rw[8], rm[8], rsd[8];
    for (int k = 0; k < 8; k++) { rw[k] = p.comp[k].pi; rm[k] = p.comp[k].loc; rsd[k] = p.comp[k].scale; }
    CHECK(fabs(wmean(rw, rm, 8) - wmean(w, mu, N)) < 1e-9);
    CHECK(fabs(wvar(rw, rm, rsd, 8) - wvar(w, mu, sd, N)) < 1e-9);
    double sum = 0;
    for (int k = 0; k < 8; k++) sum += rw[k];
    CHECK(fabs(sum - 1.0) < 1e-12);
    CHECK(ty_prd2_validate(&p, NULL) == TYQ_OK);
    for (int k = 0; k < 7; k++) CHECK(rw[k] >= rw[k + 1] || k == 6);   /* heaviest first */
    /* the seven kept are the seven heaviest */
    int heavier = 0;
    for (int i = 0; i < N; i++) if (w[i] > rw[6] + 1e-15) heavier++;
    CHECK(heavier <= 6);

    /* <= 8 components: exact copy */
    for (int n = 1; n <= 8; n++) {
        double w8[8], m8[8], s8[8], t8 = 0;
        for (int i = 0; i < n; i++) { w8[i] = 1.0 + i; m8[i] = 3.0 * i - 4; s8[i] = 1.0 + 0.25 * i; t8 += w8[i]; }
        for (int i = 0; i < n; i++) w8[i] /= t8;
        CHECK(brw_reduce(w8, m8, s8, (size_t)n, 0, &p) == TYQ_OK && p.K == (uint32_t)n);
        int ex = 1;
        for (int i = 0; i < n; i++) {
            int found = 0;
            for (int k = 0; k < n; k++)
                if (p.comp[k].loc == m8[i] && p.comp[k].scale == s8[i] && fabs(p.comp[k].pi - w8[i]) < 1e-15) found = 1;
            ex &= found;
        }
        CHECK(ex);
    }
    /* a real posterior predictive */
    brw_state s;
    brw_state_init(&s, &G);
    feed_model(&s, 1, 0.3, 0.2, 0.0, 10);
    for (int ti = 0; ti < BRW_NT; ti++) {
        CHECK(brw_predict_prd2(&s, ti, 3, &p) == TYQ_OK);
        double fw[BRW_NH], fsd[BRW_NH];
        for (int i = 0; i < BRW_NH; i++) { fw[i] = exp(s.logw[i]); fsd[i] = sqrt(G.var[ti][i]); }
        double rw2[8], rm2[8], rs2[8];
        for (uint32_t k = 0; k < p.K; k++) { rw2[k] = p.comp[k].pi; rm2[k] = p.comp[k].loc; rs2[k] = p.comp[k].scale; }
        CHECK(fabs(wmean(rw2, rm2, p.K) - wmean(fw, G.mu[ti], BRW_NH)) < 1e-9);
        CHECK(fabs(wvar(rw2, rm2, rs2, p.K) - wvar(fw, G.mu[ti], fsd, BRW_NH)) < 1e-9);
    }
}

/* independent: exact binomial tail with Pascal-triangle coefficients */
static unsigned crit_ref(unsigned n)
{
    static long double C[5001];
    C[0] = 1;
    for (unsigned r = 1; r <= n; r++)
        for (unsigned k = r; k >= 1; k--) C[k] += C[k - 1];
    for (unsigned c = 0; c <= n + 1; c++) {
        long double t = 0;
        for (unsigned k = c; k <= n; k++) t += C[k] * powl(0.01L, k) * powl(0.99L, n - k);
        if (t <= 0.001L) return c;
    }
    return n + 1;
}

static void test_binom(void)
{
    CHECK(brw_binom_crit(10) == 3);          /* P(X>=2)=0.0043, P(X>=3)=0.00011 */
    CHECK(brw_binom_crit(1) == 2);           /* P(X>=1)=0.01 > 0.001, P(X>=2)=0 */
    CHECK(brw_binom_crit(100) == 6);
    int same = 1;
    for (unsigned n = 1; n <= 150; n++) {
        static long double C[5001];
        (void)C;
        unsigned a = brw_binom_crit(n), b;
        long double Cn[152];
        memset(Cn, 0, sizeof Cn);
        Cn[0] = 1;
        for (unsigned r = 1; r <= n; r++)
            for (unsigned k = r; k >= 1; k--) Cn[k] += Cn[k - 1];
        b = n + 1;
        for (unsigned c = 0; c <= n + 1; c++) {
            long double t = 0;
            for (unsigned k = c; k <= n; k++) t += Cn[k] * powl(0.01L, k) * powl(0.99L, n - k);
            if (t <= 0.001L) { b = c; break; }
        }
        if (a != b) { same = 0; printf("binom mismatch n=%u lib=%u ref=%u\n", n, a, b); }
    }
    CHECK(same);
    (void)crit_ref;
}

static void set_post(brw_state *s, int model, double pm)
{
    /* put pm on model `model` (spread evenly inside it) and the rest evenly elsewhere */
    int cnt[3] = {0, 0, 0};
    for (int i = 0; i < BRW_NH; i++) cnt[G.h[i].model]++;
    for (int i = 0; i < BRW_NH; i++) {
        int m = G.h[i].model;
        double mass = (m == model) ? pm : (1.0 - pm) / 2.0;
        s->logw[i] = log(mass / cnt[m]);
    }
}

static void test_verdict(void)
{
    brw_state s;
    brw_state_init(&s, &G);
    CHECK(brw_verdict(&s) == BRW_V_UNDETERMINED);
    set_post(&s, 1, 0.995);
    CHECK(brw_verdict(&s) == BRW_V_M1);
    set_post(&s, 2, 0.99);
    CHECK(brw_verdict(&s) == BRW_V_M2 || brw_verdict(&s) == BRW_V_UNDETERMINED);   /* 0.99 within rounding */
    set_post(&s, 2, 0.992);
    CHECK(brw_verdict(&s) == BRW_V_M2);
    set_post(&s, 0, 0.98);
    CHECK(brw_verdict(&s) == BRW_V_UNDETERMINED);
    set_post(&s, 0, 0.999);
    CHECK(brw_verdict(&s) == BRW_V_M0);
    s.n_pred = 10;
    s.n_out99 = 3;                                        /* c(10) = 3: exceeds means > 3 */
    CHECK(brw_verdict(&s) == BRW_V_M0);
    s.n_out99 = 4;
    CHECK(brw_verdict(&s) == BRW_V_INADEQUATE);           /* wins over a confident model */
    set_post(&s, 1, 0.3);
    CHECK(brw_verdict(&s) == BRW_V_INADEQUATE);
    CHECK(strcmp(brw_verdict_name(BRW_V_INADEQUATE), "INADEQUATE") == 0);
}

static void test_choice(void)
{
    brw_state s;
    brw_state_init(&s, &G);
    int ph = 0;
    CHECK(brw_choose(&s, 2, &ph) == -1);                  /* below every cost */
    CHECK(brw_choose(&s, 0, &ph) == -1);
    CHECK(brw_choose(&s, 3, &ph) == 0);                   /* only tau = 1 costs 3 */
    int t = brw_choose(&s, 5, &ph);
    CHECK(t == 0 || t == 1);
    t = brw_choose(&s, 400, &ph);
    CHECK(t >= 0 && ph == 1);
    CHECK(brw_choose(&s, 400, NULL) == t);                /* deterministic */
    set_post(&s, 1, 0.995);
    brw_choose(&s, 400, &ph);
    CHECK(ph == 2);
    /* ties go to the smaller tau; non-ok entries are skipped */
    double sc[6] = {1, 5, 5, 2, 5, 0};
    int ok[6] = {1, 1, 1, 1, 1, 1}, ok2[6] = {1, 0, 1, 1, 1, 1}, none[6] = {0};
    CHECK(brw_pick(sc, ok, 6) == 1);
    CHECK(brw_pick(sc, ok2, 6) == 2);
    CHECK(brw_pick(sc, none, 6) == -1);
    /* choice over an affordable subset never picks an unaffordable entry */
    brw_state_init(&s, &G);
    for (int rem = 3; rem < 40; rem += 3) {
        int c = brw_choose(&s, rem, NULL);
        CHECK(c >= 0 && BRW_COST(c) <= rem);
    }
}

static void test_cdf_prequential(void)
{
    brw_state s;
    brw_state_init(&s, &G);
    for (int i = 0; i < BRW_NH; i++) s.logw[i] = -1.0 / 0.0;
    s.logw[3] = 0.0;                                      /* all mass on one hypothesis: M0, D = G.h[3].D */
    double mu = G.mu[2][3], sd = sqrt(G.var[2][3]);
    CHECK(fabs(brw_cdf(&s, 2, mu) - 0.5) < 1e-15);
    CHECK(fabs(brw_cdf(&s, 2, mu + sd) - 0.8413447460685429) < 1e-12);
    CHECK(brw_cdf(&s, 2, mu - 50 * sd) < 1e-300 && brw_cdf(&s, 2, mu + 50 * sd) > 1.0 - 1e-15);
    brw_state_init(&s, &G);
    CHECK(brw_observe(&s, 3, 0.1) == 0);
    CHECK(brw_observe(&s, 3, 5000.0) == 1);
    CHECK(s.n_pred == 2 && s.n_out99 == 1 && s.n_obs == 2);
    CHECK(brw_observe(&s, 3, 1.0 / 0.0) == -1 && s.n_obs == 2);
    CHECK(s.cdf_evals == 2u * BRW_NH);
}

int main(void)
{
    brw_grid_init(&G);
    test_grid();
    test_lse();
    test_posterior();
    test_entropy();
    test_info();
    test_reduce();
    test_binom();
    test_verdict();
    test_choice();
    test_cdf_prequential();
    printf("test_brw_active: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
