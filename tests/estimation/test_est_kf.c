/* EST-1 linear Kalman filter tests. Expected values are worked out by hand in
 * the comments, never by calling the code under test. Every CHECK is one test
 * case; any failure makes the program exit nonzero. */
#include "est_kf.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_ST(expr, want) do { est_status s_ = (expr); g_checks++; if (s_ != (want)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s -> %d, want %d\n", __FILE__, __LINE__, #expr, (int)s_, (int)(want)); } } while (0)
#define NEAR(a, b, rel) do { double a_ = (a), b_ = (b); g_checks++; \
    if (!(fabs(a_ - b_) <= (rel) * fmax(fabs(b_), 1e-300))) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s = %.17g, want %.17g\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)
#define NEARABS(a, b, ab) do { double a_ = (a), b_ = (b); g_checks++; \
    if (!(fabs(a_ - b_) <= (ab))) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s = %.17g, want %.17g\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)

#define NS 1000000000ll

static uint64_t g_rng = 0x1234567887654321ull;
static uint64_t rnd(void)
{
    g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1Dull;
}
static double rdbl(double lo, double hi) { return lo + (hi - lo) * ((double)(rnd() >> 11) / 9007199254740992.0); }

static est_model scalar_model(double F, double Q, double H, double R)
{
    est_model m; memset(&m, 0, sizeof m);
    m.n = m.m = 1; m.estimator = EST_ESTIMATOR_LINEAR_KALMAN; m.meaning = EST_UNCERTAINTY_GAUSSIAN_COVARIANCE;
    m.state_unit[0] = EST_UNIT_MILLI_CELSIUS; m.obs_unit[0] = EST_UNIT_MILLI_CELSIUS;
    m.F[0] = F; m.Q[0] = Q; m.H[0] = H; m.R[0] = R; m.step_ns = NS;
    return m;
}

static est_observation mk_obs(const est_model *mdl, const double *z, const double *R, int64_t t, uint64_t seq)
{
    est_observation o; memset(&o, 0, sizeof o);
    o.m = mdl->m;
    for (uint32_t i = 0; i < o.m; i++) { o.unit[i] = mdl->obs_unit[i]; o.z[i] = z[i]; }
    memcpy(o.R, R, sizeof(double) * o.m * o.m);
    o.t_ns = t; o.seq = seq;
    o.source.b[0] = 0x5A; o.evidence.b[0] = (uint8_t)(seq | 1u); o.evidence.b[1] = (uint8_t)(seq >> 8);
    return o;
}

/* predict one step then update with scalar/vector z at the predicted time */
static est_status step(const est_model *mdl, est_belief *b, const double *z, const double *R,
                       uint64_t seq, est_innovation *iv)
{
    est_prediction p; est_observation o; est_belief post;
    est_status st = est_kf_predict(mdl, b, NULL, 1, &p);
    if (st != EST_OK) return st;
    o = mk_obs(mdl, z, R, p.t_ns, seq);
    st = est_kf_update(mdl, b, &p, &o, &post, iv);
    if (st != EST_OK) return st;
    *b = post;
    return EST_OK;
}
static est_status coast_step(const est_model *mdl, est_belief *b, uint32_t horizon)
{
    est_prediction p; est_belief post;
    est_status st = est_kf_predict(mdl, b, NULL, horizon, &p);
    if (st != EST_OK) return st;
    st = est_kf_coast(mdl, &p, b, &post);
    if (st != EST_OK) return st;
    *b = post;
    return EST_OK;
}
static est_belief mk_prior(const est_model *mdl, const double *x0, const double *P0)
{
    est_belief b;
    memset(&b, 0, sizeof b);
    est_status st = est_kf_prior(mdl, x0, P0, 0, &b);
    CHECK_ST(st, EST_OK);
    return b;
}

/* 1. Constant position, random walk with Q = 0.
 * Information adds: 1/P_k = 1/P0 + k/R. With P0 = 100, R = 4: P_k = 1/(0.01 + k/4).
 * With every z = 10 and x0 = 0 the mean is P_k * (x0/P0 + k*10/R) = P_k * (2.5 k). */
static void test_constant_position(void)
{
    est_model m = scalar_model(1, 0, 1, 4);
    double x0 = 0, P0 = 100, z = 10, R = 4;
    est_belief b = mk_prior(&m, &x0, &P0);
    CHECK(b.generation == 0 && est_digest_is_zero(&b.parent) && est_digest_is_zero(&b.evidence_root));
    for (int k = 1; k <= 20; k++) {
        est_innovation iv;
        CHECK_ST(step(&m, &b, &z, &R, (uint64_t)k, &iv), EST_OK);
        double Pk = 1.0 / (0.01 + k / 4.0);
        NEAR(b.P[0], Pk, 1e-12);
        NEAR(b.x[0], Pk * 2.5 * k, 1e-12);
        CHECK(b.generation == (uint64_t)(2 * k));
        CHECK(b.t_ns == (int64_t)k * NS);
    }
}

/* 2. Constant velocity, dt = 1 s. F = [[1,1],[0,1]], Q = 0, H = [1,0], R = 1,
 * x0 = (0,1), P0 = I.
 * predict: x' = (1,1); P' = F P F^T = [[1+1,1],[1,1]] = [[2,1],[1,1]].
 * S = 2 + 1 = 3. K = P' H^T / S = (2/3, 1/3). z = 1.5, nu = 0.5.
 * x = (1 + 1/3, 1 + 1/6) = (4/3, 7/6).
 * P = P' - K H P' = [[2 - 4/3, 1 - 2/3],[1 - 2/3, 1 - 1/3]] = [[2/3,1/3],[1/3,2/3]].
 * nis = 0.25 / 3. */
static void test_constant_velocity(void)
{
    est_model m; memset(&m, 0, sizeof m);
    m.n = 2; m.m = 1; m.estimator = EST_ESTIMATOR_LINEAR_KALMAN; m.meaning = EST_UNCERTAINTY_GAUSSIAN_COVARIANCE;
    m.state_unit[0] = EST_UNIT_MILLI_CELSIUS; m.state_unit[1] = EST_UNIT_MILLI_CELSIUS_PER_S; m.obs_unit[0] = EST_UNIT_MILLI_CELSIUS;
    m.F[0] = 1; m.F[1] = 1; m.F[3] = 1; m.H[0] = 1; m.R[0] = 1; m.step_ns = NS;
    double x0[2] = { 0, 1 }, P0[4] = { 1, 0, 0, 1 }, z = 1.5, R = 1;
    est_belief b = mk_prior(&m, x0, P0);
    est_prediction p;
    CHECK_ST(est_kf_predict(&m, &b, NULL, 1, &p), EST_OK);
    NEAR(p.x[0], 1.0, 1e-14); NEAR(p.x[1], 1.0, 1e-14);
    NEAR(p.P[0], 2.0, 1e-14); NEAR(p.P[1], 1.0, 1e-14); NEAR(p.P[2], 1.0, 1e-14); NEAR(p.P[3], 1.0, 1e-14);
    NEAR(p.y_mean[0], 1.0, 1e-14); NEAR(p.S[0], 2.0 + 1.0, 1e-14);   /* H P' H^T + R_model */
    est_observation o = mk_obs(&m, &z, &R, p.t_ns, 1);
    est_belief post; est_innovation iv;
    CHECK_ST(est_kf_update(&m, &b, &p, &o, &post, &iv), EST_OK);
    NEAR(post.x[0], 4.0 / 3.0, 1e-13); NEAR(post.x[1], 7.0 / 6.0, 1e-13);
    NEAR(post.P[0], 2.0 / 3.0, 1e-13); NEAR(post.P[1], 1.0 / 3.0, 1e-13);
    NEAR(post.P[2], 1.0 / 3.0, 1e-13); NEAR(post.P[3], 2.0 / 3.0, 1e-13);
    NEAR(iv.nu[0], 0.5, 1e-14); NEAR(iv.S[0], 3.0, 1e-14); NEAR(iv.nis, 0.25 / 3.0, 1e-13);
    /* A second step with x' = F x etc. keeps a valid symmetric covariance. */
    est_belief b2 = post;
    CHECK_ST(step(&m, &b2, &z, &R, 2, NULL), EST_OK);
    CHECK_ST(est_check_covariance(b2.P, 2), EST_OK);
}

/* 3. Noisy position: x0 = 0, P0 = 1, R = 1, Q = 0. z1 = 2: K = 1/2, x = 1, P = 1/2.
 * z2 = 4: S = 1.5, K = 1/3, x = 1 + 3/3 = 2, P = 1/3. (mean of {0,2,4} = 2). */
static void test_noisy_position(void)
{
    est_model m = scalar_model(1, 0, 1, 1);
    double x0 = 0, P0 = 1, R = 1, z1 = 2, z2 = 4;
    est_belief b = mk_prior(&m, &x0, &P0);
    est_innovation iv;
    CHECK_ST(step(&m, &b, &z1, &R, 1, &iv), EST_OK);
    NEAR(b.x[0], 1.0, 1e-14); NEAR(b.P[0], 0.5, 1e-14);
    NEAR(iv.nu[0], 2.0, 1e-14); NEAR(iv.S[0], 2.0, 1e-14); NEAR(iv.nis, 2.0, 1e-14);
    CHECK_ST(step(&m, &b, &z2, &R, 2, &iv), EST_OK);
    NEAR(b.x[0], 2.0, 1e-14); NEAR(b.P[0], 1.0 / 3.0, 1e-14);
    NEAR(iv.nu[0], 3.0, 1e-14); NEAR(iv.S[0], 1.5, 1e-14); NEAR(iv.nis, 6.0, 1e-14);
}

/* 4. Position and velocity both measured: H = I, R = I, F = I, Q = 0, P0 = 4 I.
 * S = 4 + 1 = 5 per axis, K = 0.8 I, P = (1-0.8)*4 + 0.64*1 = 0.8 + ... Joseph:
 * (0.2)^2*4 + 0.8^2*1 = 0.16 + 0.64 = 0.8. z = (5,-5), x0 = 0: x = (4,-4).
 * nis = (25 + 25) / 5 = 10. Two-dimensional Cholesky path. */
static void test_position_and_velocity(void)
{
    est_model m; memset(&m, 0, sizeof m);
    m.n = m.m = 2; m.estimator = EST_ESTIMATOR_LINEAR_KALMAN; m.meaning = EST_UNCERTAINTY_GAUSSIAN_COVARIANCE;
    m.state_unit[0] = m.obs_unit[0] = EST_UNIT_MILLI_CELSIUS;
    m.state_unit[1] = m.obs_unit[1] = EST_UNIT_MILLI_CELSIUS_PER_S;
    m.F[0] = m.F[3] = 1; m.H[0] = m.H[3] = 1; m.R[0] = m.R[3] = 1; m.step_ns = NS;
    double x0[2] = { 0, 0 }, P0[4] = { 4, 0, 0, 4 }, z[2] = { 5, -5 }, R[4] = { 1, 0, 0, 1 };
    est_belief b = mk_prior(&m, x0, P0);
    est_innovation iv;
    CHECK_ST(step(&m, &b, z, R, 1, &iv), EST_OK);
    NEAR(b.x[0], 4.0, 1e-13); NEAR(b.x[1], -4.0, 1e-13);
    NEAR(b.P[0], 0.8, 1e-13); NEAR(b.P[3], 0.8, 1e-13);
    NEARABS(b.P[1], 0.0, 1e-15); NEARABS(b.P[2], 0.0, 1e-15);
    NEAR(iv.nis, 10.0, 1e-13);
    CHECK(iv.m == 2);
}

/* Riccati: F = H = 1. P_prior' = P_post + Q, P_post = P_prior R / (P_prior + R).
 * Fixed point: P^2 - Q P - Q R = 0, so P_prior* = (Q + sqrt(Q^2 + 4 Q R)) / 2.
 * Innovation covariance S = P_prior* + R, gain K = P_prior* / S = 1 - R / S. */
static double steady_gain(double Q, double R, double *Pprior)
{
    est_model m = scalar_model(1, Q, 1, R);
    double x0 = 0, P0 = 1, z = 0;
    est_belief b = mk_prior(&m, &x0, &P0);
    est_innovation iv;
    for (int k = 1; k <= 4000; k++)
        if (step(&m, &b, &z, &R, (uint64_t)k, &iv) != EST_OK) { g_fail++; fprintf(stderr, "FAIL steady step\n"); return 0; }
    *Pprior = iv.S[0] - R;
    return (iv.S[0] - R) / iv.S[0];
}
static void test_riccati_and_regimes(void)
{
    double Qs[5] = { 1e-3, 1e-2, 1e-1, 1.0, 10.0 };
    double Ps[5], prevK = -1;
    for (int i = 0; i < 5; i++) {
        double R = 1.0, P;
        double K = steady_gain(Qs[i], R, &P);
        Ps[i] = P;
        double Pstar = (Qs[i] + sqrt(Qs[i] * Qs[i] + 4.0 * Qs[i] * R)) / 2.0;
        NEAR(P, Pstar, 1e-9);
        NEAR(K, Pstar / (Pstar + R), 1e-9);
        CHECK(K > prevK);             /* gain grows with Q/R */
        prevK = K;
    }
    CHECK(Ps[0] < Ps[1] && Ps[1] < Ps[2] && Ps[2] < Ps[3] && Ps[3] < Ps[4]);
    /* Observation noise regimes: Q = 1, R in {1e-2, 1, 1e2}; gain falls as R grows. */
    double Rs[3] = { 1e-2, 1.0, 1e2 };
    double prev = 2;
    for (int i = 0; i < 3; i++) {
        double P, Q = 1.0;
        double K = steady_gain(Q, Rs[i], &P);
        double Pstar = (Q + sqrt(Q * Q + 4.0 * Q * Rs[i])) / 2.0;
        NEAR(P, Pstar, 1e-9);
        NEAR(K, Pstar / (Pstar + Rs[i]), 1e-9);
        CHECK(K < prev);
        prev = K;
    }
}

/* Coast: P_{k+1} = F P_k F^T + Q exactly. F = [[1,1],[0,1]], Q = diag(.1,.2), P0 = diag(1,2), x0 = (0,1).
 * P1 = [[3,2],[2,2]] + Q = [[3.1,2],[2,2.2]]
 * P2: F P1 = [[5.1,4.2],[2,2.2]]; F P1 F^T = [[9.3,4.2],[4.2,2.2]]; + Q = [[9.4,4.2],[4.2,2.4]]
 * P3: F P2 = [[13.6,6.6],[4.2,2.4]]; F P2 F^T = [[20.2,6.6],[6.6,2.4]]; + Q = [[20.3,6.6],[6.6,2.6]]
 * x_k = (k, 1). */
static void test_coast(void)
{
    est_model m; memset(&m, 0, sizeof m);
    m.n = 2; m.m = 1; m.estimator = EST_ESTIMATOR_LINEAR_KALMAN; m.meaning = EST_UNCERTAINTY_GAUSSIAN_COVARIANCE;
    m.state_unit[0] = EST_UNIT_MILLI_CELSIUS; m.state_unit[1] = EST_UNIT_MILLI_CELSIUS_PER_S; m.obs_unit[0] = EST_UNIT_MILLI_CELSIUS;
    m.F[0] = 1; m.F[1] = 1; m.F[3] = 1; m.H[0] = 1; m.Q[0] = 0.1; m.Q[3] = 0.2; m.R[0] = 1; m.step_ns = NS;
    double x0[2] = { 0, 1 }, P0[4] = { 1, 0, 0, 2 };
    double want[3][4] = { { 3.1, 2, 2, 2.2 }, { 9.4, 4.2, 4.2, 2.4 }, { 20.3, 6.6, 6.6, 2.6 } };
    est_belief b = mk_prior(&m, x0, P0);
    est_digest root0 = b.evidence_root;
    for (int k = 1; k <= 3; k++) {
        CHECK_ST(coast_step(&m, &b, 1), EST_OK);
        for (int i = 0; i < 4; i++) NEAR(b.P[i], want[k - 1][i], 1e-13);
        NEAR(b.x[0], (double)k, 1e-13); NEAR(b.x[1], 1.0, 1e-13);
        CHECK(b.generation == (uint64_t)(2 * k));
        CHECK(memcmp(b.evidence_root.b, root0.b, 32) == 0);   /* no observation, no new evidence */
        CHECK(b.t_ns == (int64_t)k * NS);
    }
    /* One predict with horizon 3 equals three single steps. */
    est_belief b0 = mk_prior(&m, x0, P0);
    est_prediction p;
    CHECK_ST(est_kf_predict(&m, &b0, NULL, 3, &p), EST_OK);
    for (int i = 0; i < 4; i++) NEAR(p.P[i], want[2][i], 1e-13);
    CHECK(p.horizon == 3 && p.generation == 1 && p.t_ns == 3 * NS);
    /* prediction of the observation: y = H x = 3, S = P[0] + R = 21.3 */
    NEAR(p.y_mean[0], 3.0, 1e-13); NEAR(p.S[0], 21.3, 1e-13);
    /* known input Bu = (0, 0.5) added each step: x_k = (k + 0.5 * k(k-1)/2 ... ) worked for k = 1:
     * x1 = F (0,1) + (0,0.5) = (1, 1.5). */
    double Bu[2] = { 0, 0.5 };
    CHECK_ST(est_kf_predict(&m, &b0, Bu, 1, &p), EST_OK);
    NEAR(p.x[0], 1.0, 1e-14); NEAR(p.x[1], 1.5, 1e-14);
}

/* Repeated identical observation z = 5, P0 = 1e6, R = 1, Q = 0: 1/P_k = 1e-6 + k. */
static void test_repeated_identical(void)
{
    est_model m = scalar_model(1, 0, 1, 1);
    double x0 = 0, P0 = 1e6, z = 5, R = 1;
    est_belief b = mk_prior(&m, &x0, &P0);
    for (int k = 1; k <= 100; k++) {
        CHECK_ST(step(&m, &b, &z, &R, (uint64_t)k, NULL), EST_OK);
        double Pk = 1.0 / (1e-6 + k);
        NEAR(b.P[0], Pk, 1e-10);
        NEAR(b.x[0], Pk * (0.0 / 1e6 + 5.0 * k), 1e-10);
    }
    NEARABS(b.x[0], 5.0, 1e-3);
}

/* Sensor confidence extremes and initial collapse. */
static void test_sensor_extremes(void)
{
    est_model m = scalar_model(1, 0, 1, 1e-12);
    double x0 = 0, P0 = 1, z = 7, R = 1e-12;
    est_belief b = mk_prior(&m, &x0, &P0);
    CHECK_ST(step(&m, &b, &z, &R, 1, NULL), EST_OK);
    NEARABS(b.x[0], 7.0, 1e-9);                  /* K = 1/(1+1e-12) */
    NEAR(b.P[0], 1.0 / (1.0 + 1e12), 1e-9);      /* 1/P = 1/P0 + 1/R */

    R = 1e12; m = scalar_model(1, 0, 1, R);
    b = mk_prior(&m, &x0, &P0);
    CHECK_ST(step(&m, &b, &z, &R, 1, NULL), EST_OK);
    NEARABS(b.x[0], 7.0 * 1.0 / (1.0 + 1e12), 1e-15);   /* ~ prior 0 */
    NEAR(b.P[0], 1.0 / (1.0 + 1e-12), 1e-12);           /* ~ prior 1 */

    /* initial uncertainty collapse: P0 = 1e8, R = 1 -> P = 1/(1e-8 + 1) */
    R = 1; m = scalar_model(1, 0, 1, R); P0 = 1e8;
    b = mk_prior(&m, &x0, &P0);
    CHECK_ST(step(&m, &b, &z, &R, 1, NULL), EST_OK);
    NEAR(b.P[0], 1.0 / (1e-8 + 1.0), 1e-9);
    CHECK(b.P[0] < 1.0 && b.P[0] > 0.0);
}

/* Growth without observations. F = 1, Q = 0.5, P0 = 1: P_k = 1 + 0.5 k.
 * F = 1.1: P_k = 1.21^k P0 + Q (1.21^k - 1)/0.21. */
static void test_growth(void)
{
    est_model m = scalar_model(1, 0.5, 1, 1);
    double x0 = 0, P0 = 1;
    est_belief b = mk_prior(&m, &x0, &P0);
    double prev = 1;
    for (int k = 1; k <= 50; k++) {
        CHECK_ST(coast_step(&m, &b, 1), EST_OK);
        NEAR(b.P[0], 1.0 + 0.5 * k, 1e-12);
        CHECK(b.P[0] > prev);
        prev = b.P[0];
    }
    m = scalar_model(1.1, 0.5, 1, 1);
    b = mk_prior(&m, &x0, &P0);
    for (int k = 1; k <= 10; k++) CHECK_ST(coast_step(&m, &b, 1), EST_OK);
    double g = pow(1.21, 10.0);
    NEAR(b.P[0], g * 1.0 + 0.5 * (g - 1.0) / 0.21, 1e-11);
}

static void test_linkage(void)
{
    est_model m = scalar_model(1, 0.1, 1, 2);
    double x0 = 1, P0 = 3, z = 4, R = 2;
    est_belief b = mk_prior(&m, &x0, &P0);
    est_digest md; CHECK_ST(est_digest_model(&m, &md), EST_OK);
    CHECK(memcmp(b.model.b, md.b, 32) == 0);
    est_prediction p; est_observation o; est_belief post; est_innovation iv;
    CHECK_ST(est_kf_predict(&m, &b, NULL, 2, &p), EST_OK);
    est_digest bd; CHECK_ST(est_digest_belief(&b, &bd), EST_OK);
    CHECK(memcmp(p.prior.b, bd.b, 32) == 0 && memcmp(p.model.b, md.b, 32) == 0);
    CHECK(p.generation == b.generation + 1 && p.t_ns == b.t_ns + 2 * NS);
    NEAR(p.P[0], 3.2, 1e-14);              /* 3 + 2*0.1 */
    NEAR(p.S[0], 5.2, 1e-14);              /* + R_model 2 */
    o = mk_obs(&m, &z, &R, p.t_ns, 9);
    CHECK_ST(est_kf_update(&m, &b, &p, &o, &post, &iv), EST_OK);
    est_digest pd, od, root;
    CHECK_ST(est_digest_prediction(&p, &pd), EST_OK);
    CHECK_ST(est_digest_observation(&o, &od), EST_OK);
    CHECK_ST(est_evidence_root_extend(&b.evidence_root, &od, &root), EST_OK);
    CHECK(memcmp(post.parent.b, pd.b, 32) == 0);
    CHECK(memcmp(post.evidence_root.b, root.b, 32) == 0);
    CHECK(!est_digest_is_zero(&post.evidence_root));
    CHECK(post.generation == p.generation + 1 && post.t_ns == p.t_ns);
    CHECK(memcmp(post.model.b, md.b, 32) == 0);
    CHECK(memcmp(iv.observation.b, od.b, 32) == 0 && memcmp(iv.prediction.b, pd.b, 32) == 0);
    CHECK(memcmp(iv.model.b, md.b, 32) == 0 && iv.t_ns == p.t_ns);
    /* nu = 4 - 1 = 3, S = 3.2 + 2 = 5.2, nis = 9/5.2, K = 3.2/5.2 */
    NEAR(iv.nu[0], 3.0, 1e-14); NEAR(iv.S[0], 5.2, 1e-14); NEAR(iv.nis, 9.0 / 5.2, 1e-13);
    NEAR(post.x[0], 1.0 + 3.2 / 5.2 * 3.0, 1e-13);
    NEAR(post.P[0], 3.2 * 2.0 / 5.2, 1e-13);
    CHECK(post.unit[0] == EST_UNIT_MILLI_CELSIUS);
    /* the posterior digest depends on the evidence: a different z gives a different root */
    double z2 = 4.5; est_observation o2 = mk_obs(&m, &z2, &R, p.t_ns, 9); est_belief post2;
    CHECK_ST(est_kf_update(&m, &b, &p, &o2, &post2, NULL), EST_OK);
    CHECK(memcmp(post2.evidence_root.b, post.evidence_root.b, 32) != 0);
    /* coast keeps root, links parent to the prediction */
    est_belief c;
    CHECK_ST(est_kf_coast(&m, &p, &b, &c), EST_OK);
    CHECK(memcmp(c.parent.b, pd.b, 32) == 0 && memcmp(c.evidence_root.b, b.evidence_root.b, 32) == 0);
    CHECK(c.generation == p.generation + 1);
}

static void test_determinism_and_const(void)
{
    est_model m = scalar_model(1, 0.1, 1, 2);
    double x0 = 1, P0 = 3, z = 4, R = 2;
    est_belief b = mk_prior(&m, &x0, &P0);
    est_prediction p1, p2; est_observation o; est_belief a1, a2; est_innovation i1, i2;
    est_model msnap; est_belief bsnap; est_prediction psnap; est_observation osnap;
    CHECK_ST(est_kf_predict(&m, &b, NULL, 1, &p1), EST_OK);
    CHECK_ST(est_kf_predict(&m, &b, NULL, 1, &p2), EST_OK);
    CHECK(memcmp(&p1.x, &p2.x, sizeof p1.x) == 0 && memcmp(&p1.P, &p2.P, sizeof p1.P) == 0);
    est_digest d1, d2;
    CHECK_ST(est_digest_prediction(&p1, &d1), EST_OK); CHECK_ST(est_digest_prediction(&p2, &d2), EST_OK);
    CHECK(memcmp(d1.b, d2.b, 32) == 0);
    o = mk_obs(&m, &z, &R, p1.t_ns, 1);
    memcpy(&msnap, &m, sizeof m); memcpy(&bsnap, &b, sizeof b); memcpy(&psnap, &p1, sizeof p1); memcpy(&osnap, &o, sizeof o);
    CHECK_ST(est_kf_update(&m, &b, &p1, &o, &a1, &i1), EST_OK);
    CHECK_ST(est_kf_update(&m, &b, &p1, &o, &a2, &i2), EST_OK);
    CHECK(memcmp(&msnap, &m, sizeof m) == 0 && memcmp(&bsnap, &b, sizeof b) == 0);
    CHECK(memcmp(&psnap, &p1, sizeof p1) == 0 && memcmp(&osnap, &o, sizeof o) == 0);
    CHECK_ST(est_digest_belief(&a1, &d1), EST_OK); CHECK_ST(est_digest_belief(&a2, &d2), EST_OK);
    CHECK(memcmp(d1.b, d2.b, 32) == 0);
    CHECK_ST(est_digest_innovation(&i1, &d1), EST_OK); CHECK_ST(est_digest_innovation(&i2, &d2), EST_OK);
    CHECK(memcmp(d1.b, d2.b, 32) == 0);
    /* coast and predict leave their inputs alone too */
    est_belief c;
    CHECK_ST(est_kf_coast(&m, &p1, &b, &c), EST_OK);
    CHECK(memcmp(&psnap, &p1, sizeof p1) == 0 && memcmp(&bsnap, &b, sizeof b) == 0);
    /* update without an innovation out-parameter is allowed */
    CHECK_ST(est_kf_update(&m, &b, &p1, &o, &a2, NULL), EST_OK);
}

static void test_refusals(void)
{
    est_model m = scalar_model(1, 0.1, 1, 2);
    double x0 = 1, P0 = 3, z = 4, R = 2;
    est_belief b = mk_prior(&m, &x0, &P0);
    est_prediction p; est_observation o; est_belief post, sent_b; est_innovation iv, sent_i;
    CHECK_ST(est_kf_predict(&m, &b, NULL, 1, &p), EST_OK);
    o = mk_obs(&m, &z, &R, p.t_ns, 1);
    memset(&sent_b, 0xAB, sizeof sent_b); memset(&sent_i, 0xCD, sizeof sent_i);
    post = sent_b; iv = sent_i;
    est_belief post_ok; CHECK_ST(est_kf_update(&m, &b, &p, &o, &post_ok, NULL), EST_OK);

    /* stale: prior digest does not match the belief presented */
    est_belief other = b; other.x[0] = 1.5;
    CHECK_ST(est_kf_update(&m, &other, &p, &o, &post, &iv), EST_ERR_STALE);
    CHECK_ST(est_kf_coast(&m, &p, &other, &post), EST_ERR_STALE);
    /* stale: an old prediction against a newer belief */
    CHECK_ST(est_kf_update(&m, &post_ok, &p, &o, &post, &iv), EST_ERR_STALE);
    /* stale generation inside a matching digest chain */
    est_prediction pg = p; pg.generation += 1;
    CHECK_ST(est_kf_update(&m, &b, &pg, &o, &post, &iv), EST_ERR_STALE);
    /* wrong model */
    est_model m2 = scalar_model(1, 0.1, 1, 3);
    CHECK_ST(est_kf_update(&m2, &b, &p, &o, &post, &iv), EST_ERR_MODEL);
    CHECK_ST(est_kf_predict(&m2, &b, NULL, 1, &p), EST_ERR_MODEL);
    CHECK_ST(est_kf_coast(&m2, &p, &b, &post), EST_ERR_MODEL);
    /* wrong time */
    est_observation ot = o; ot.t_ns += 1;
    CHECK_ST(est_kf_update(&m, &b, &p, &ot, &post, &iv), EST_ERR_TIME);
    ot = o; ot.t_ns -= 1;
    CHECK_ST(est_kf_update(&m, &b, &p, &ot, &post, &iv), EST_ERR_TIME);
    CHECK_ST(est_kf_predict(&m, &b, NULL, 0, &p), EST_ERR_TIME);
    /* wrong unit, wrong dimension */
    est_observation ou = o; ou.unit[0] = EST_UNIT_WATT;
    CHECK_ST(est_kf_update(&m, &b, &p, &ou, &post, &iv), EST_ERR_UNIT);
    est_observation od = o; od.m = 2; od.unit[1] = EST_UNIT_WATT; od.R[3] = 1;
    CHECK_ST(est_kf_update(&m, &b, &p, &od, &post, &iv), EST_ERR_DIM);
    /* NaN / Inf observation */
    est_observation on = o; on.z[0] = NAN;
    CHECK_ST(est_kf_update(&m, &b, &p, &on, &post, &iv), EST_ERR_NONFINITE);
    on = o; on.z[0] = INFINITY;
    CHECK_ST(est_kf_update(&m, &b, &p, &on, &post, &iv), EST_ERR_NONFINITE);
    on = o; on.R[0] = NAN;
    CHECK_ST(est_kf_update(&m, &b, &p, &on, &post, &iv), EST_ERR_NONFINITE);
    /* observation with no evidence bound is refused */
    est_observation oe = o; memset(&oe.evidence, 0, 32);
    CHECK_ST(est_kf_update(&m, &b, &p, &oe, &post, &iv), EST_ERR_KIND);
    /* NaN known input */
    double Bu = NAN;
    CHECK_ST(est_kf_predict(&m, &b, &Bu, 1, &p), EST_ERR_NONFINITE);
    /* nulls */
    CHECK_ST(est_kf_update(&m, &b, &p, NULL, &post, &iv), EST_ERR_NULL);
    CHECK_ST(est_kf_predict(&m, NULL, NULL, 1, &p), EST_ERR_NULL);
    /* Every refusal left the caller's outputs untouched. */
    CHECK(memcmp(&post, &sent_b, sizeof post) == 0 && memcmp(&iv, &sent_i, sizeof iv) == 0);

    /* singular S: P = 0 and R = 0 */
    est_model ms = scalar_model(1, 0, 1, 0);
    double zero = 0, z1 = 1, R0 = 0;
    est_belief bs = mk_prior(&ms, &zero, &zero);
    est_prediction ps;
    CHECK_ST(est_kf_predict(&ms, &bs, NULL, 1, &ps), EST_OK);
    est_observation os = mk_obs(&ms, &z1, &R0, ps.t_ns, 1);
    memset(&post, 0xAB, sizeof post);
    CHECK_ST(est_kf_update(&ms, &bs, &ps, &os, &post, &iv), EST_ERR_NOT_PD);
    CHECK(memcmp(&post, &sent_b, sizeof post) == 0);
    /* prior refusals */
    est_belief pr;
    double negP = -1, nanx = NAN;
    CHECK_ST(est_kf_prior(&m, &x0, &negP, 0, &pr), EST_ERR_NOT_PSD);
    CHECK_ST(est_kf_prior(&m, &nanx, &P0, 0, &pr), EST_ERR_NONFINITE);
    est_model bad = m; bad.n = 0;
    CHECK_ST(est_kf_prior(&bad, &x0, &P0, 0, &pr), EST_ERR_DIM);
    /* belief for a different-dimension model */
    est_model m2d = m; m2d.n = 2; m2d.state_unit[1] = EST_UNIT_WATT; m2d.F[0] = m2d.F[3] = 1; m2d.H[0] = 1;
    CHECK_ST(est_kf_predict(&m2d, &b, NULL, 1, &p), EST_ERR_DIM);
}

/* Covariance collapse: 1e5 random-walk steps, R alternating between tiny and
 * huge at random. Q = 0 makes the posterior exactly the information sum
 * 1/P_k = 1/P0 + sum 1/R_i, so it is checked against that; a run with Q > 0
 * checks symmetry / PSD / finiteness at every step. */
static void test_long_run(void)
{
    est_model m = scalar_model(1, 0, 1, 1);
    double x0 = 0, P0 = 10;
    est_belief b = mk_prior(&m, &x0, &P0);
    double info = 1.0 / P0;
    int ok = 1;
    for (int k = 1; k <= 100000; k++) {
        double R = (rnd() & 1) ? rdbl(1e-9, 1e-8) : rdbl(1e8, 1e9);
        double z = rdbl(-1, 1);
        info += 1.0 / R;
        if (step(&m, &b, &z, &R, (uint64_t)k, NULL) != EST_OK) { ok = 0; break; }
        if (!(b.P[0] > 0.0) || !isfinite(b.P[0]) || !isfinite(b.x[0])) { ok = 0; break; }
    }
    CHECK(ok);
    NEAR(b.P[0], 1.0 / info, 1e-7);
    CHECK(b.generation == 200000u);

    /* with process noise, 2-D constant velocity, mixed R, checks every step */
    est_model m2; memset(&m2, 0, sizeof m2);
    m2.n = 2; m2.m = 1; m2.estimator = EST_ESTIMATOR_LINEAR_KALMAN; m2.meaning = EST_UNCERTAINTY_GAUSSIAN_COVARIANCE;
    m2.state_unit[0] = EST_UNIT_MILLI_CELSIUS; m2.state_unit[1] = EST_UNIT_MILLI_CELSIUS_PER_S; m2.obs_unit[0] = EST_UNIT_MILLI_CELSIUS;
    m2.F[0] = 1; m2.F[1] = 1; m2.F[3] = 1; m2.H[0] = 1; m2.Q[0] = 1e-6; m2.Q[3] = 1e-6; m2.R[0] = 1; m2.step_ns = NS;
    double xa[2] = { 0, 0 }, Pa[4] = { 1, 0, 0, 1 };
    b = mk_prior(&m2, xa, Pa);
    ok = 1;
    for (int k = 1; k <= 30000; k++) {
        double R = (rnd() & 1) ? rdbl(1e-9, 1e-8) : rdbl(1e8, 1e9);
        double z = rdbl(-1, 1);
        if (step(&m2, &b, &z, &R, (uint64_t)k, NULL) != EST_OK) { ok = 0; break; }
        if (est_check_covariance(b.P, 2) != EST_OK || b.P[0] < 0.0 || b.P[3] < 0.0 || b.P[1] != b.P[2]) { ok = 0; break; }
    }
    CHECK(ok);
    CHECK(b.P[0] < 1e-6 || b.P[0] < 1.0);   /* did not blow up */
}

int main(void)
{
    test_constant_position();
    test_constant_velocity();
    test_noisy_position();
    test_position_and_velocity();
    test_riccati_and_regimes();
    test_coast();
    test_repeated_identical();
    test_sensor_extremes();
    test_growth();
    test_linkage();
    test_determinism_and_const();
    test_refusals();
    test_long_run();
    printf("test_est_kf: %d checks, %d failed\n", g_checks, g_fail);
    if (g_fail) { printf("test_est_kf: FAIL\n"); return 1; }
    printf("test_est_kf: PASS\n");
    return 0;
}
