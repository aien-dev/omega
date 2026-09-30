/* EST-1 reference tests.
 *
 * Part (a): the information-form reference alone against closed forms.
 * Part (b) (not compiled with -DEST_REF_STANDALONE): est_kf cross-check on
 *          random scenarios, plus NIS calibration on simulated data.
 *
 * Deterministic: xorshift64* PRNG, seeds fixed below.
 *   SEED_XCHECK_BASE 0x9E3779B97F4A7C15, scenario s uses BASE + s * STRIDE
 *   SEED_STRIDE      0xD1B54A32D192ED03
 *   SEED_NIS_BASE    0x2545F4914F6CDD1D, run for m uses BASE + m * STRIDE
 *
 * Closed forms used in part (a), all with F = H = 1 unless noted.
 *
 * 1) 1-D random walk (Q = 0), P0 = 2, R = 0.5. After k measurements
 *    posterior variance = 1 / (1/P0 + k/R).
 *      k=1: 1/(0.5 + 2)  = 0.4
 *      k=3: 1/(0.5 + 6)  = 0.153846153846...
 *      k=10: 1/(0.5+20)  = 0.048780487804...
 *    Mean with x0 = 0 and all z = 1: x_k = (k/R) / (1/P0 + k/R) = k/(0.25+k)...
 *    computed in code as P_k * (k / R).
 *
 * 2) Scalar Riccati steady state: prior P* = (Q + sqrt(Q^2 + 4QR)) / 2,
 *    posterior (P* R)/(P* + R), gain P* / (P* + R).
 *      Q=1,    R=4:    P* = 2.561552812808830, post = 1.561552812808830,
 *                      gain = 0.390388203202207
 *      Q=0.01, R=1:    P* = 0.105124921972503, post = 0.095124921972503,
 *                      gain = 0.095124921972503
 *      Q=4,    R=0.25: P* = 4.236067977499789, post = 0.236067977499789,
 *                      gain = 0.944271909999158
 *    (digits from bc, scale 15). The gain is recovered from the reference as
 *    K = P+ H R^-1 (the posterior form of the Kalman gain), i.e. post / R here.
 *
 * 3) Constant velocity, dt = 1: F = [[1,1],[0,1]], Q = q [[1/3,1/2],[1/2,1]],
 *    H = [1 0], R scalar. One hand-checked step: q = 0.6 (Q = [[0.2,0.3],
 *    [0.3,0.6]]), R = 1, P0 = I, x0 = 0, z = 1.
 *      P' = F P0 F^T + Q = [[2.2,1.3],[1.3,1.6]]     (F F^T = [[2,1],[1,1]])
 *      S  = 2.2 + 1 = 3.2,   K = [2.2, 1.3]/3.2 = [0.6875, 0.40625]
 *      P+ = P' - P' H^T H P' / S
 *         = [[0.6875, 0.40625],[0.40625, 1.6 - 1.69/3.2 = 1.071875]]
 *      x+ = K z = [0.6875, 0.40625],  nu = 1,  NIS = 1/3.2 = 0.3125
 *    Steady state: iterate the reference to convergence and compare with an
 *    independent hand-coded 2x2 covariance-form recursion; report values. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "est_ref_info.h"
#include "est_types.h"
#ifndef EST_REF_STANDALONE
#include "est_kf.h"
#endif

#define SEED_XCHECK_BASE 0x9E3779B97F4A7C15ULL
#define SEED_STRIDE 0xD1B54A32D192ED03ULL
#define SEED_NIS_BASE 0x2545F4914F6CDD1DULL

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int near_abs(double a, double b, double tol) { return fabs(a - b) <= tol; }

/* ---- PRNG ---- */
typedef struct { uint64_t s; } rng;
static uint64_t rng_u64(rng *r)
{
    uint64_t x = r->s;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    r->s = x;
    return x * 0x2545F4914F6CDD1DULL;
}
__attribute__((unused)) static void rng_seed(rng *r, uint64_t seed) { r->s = seed ? seed : 1; for (int i = 0; i < 8; i++) (void)rng_u64(r); }
static double rng_u01(rng *r) { return ((rng_u64(r) >> 11) + 0.5) * (1.0 / 9007199254740992.0); }
__attribute__((unused)) static double rng_uni(rng *r, double lo, double hi) { return lo + (hi - lo) * rng_u01(r); }
__attribute__((unused)) static double rng_gauss(rng *r)
{
    double u1 = rng_u01(r), u2 = rng_u01(r);
    return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}

/* ---- part (a) ---- */
static void part_a(void)
{
    est_ref_info r;
    double x, P, z, I2[4] = {1, 0, 0, 1};
    double one = 1.0, zero = 0.0;
    int k;

    /* 1) random walk */
    {
        double P0 = 2.0, R = 0.5, x0 = 0.0, worst = 0;
        CHECK(est_ref_init(&r, 1, &x0, &P0) == 0, "init");
        for (k = 1; k <= 10; k++) {
            double expect_P = 1.0 / (1.0 / P0 + k / R);
            double expect_x = expect_P * (k / R) * 1.0;
            z = 1.0;
            CHECK(est_ref_predict(&r, &one, &zero) == 0, "predict");
            CHECK(est_ref_update(&r, 1, &one, &R, &z) == 0, "update");
            est_ref_get(&r, &x, &P);
            worst = fmax(worst, fabs(P - expect_P) / expect_P);
            worst = fmax(worst, fabs(x - expect_x) / expect_x);
            if (k == 1 || k == 3 || k == 10)
                printf("random walk k=%d: P=%.15f expect=%.15f\n", k, P, expect_P);
        }
        CHECK(worst < 1e-14, "random walk rel error %.3g", worst);
        printf("random walk worst rel error %.3g\n", worst);
    }

    /* 2) scalar Riccati */
    {
        static const double QR[3][2] = {{1.0, 4.0}, {0.01, 1.0}, {4.0, 0.25}};
        static const double EXP_PRIOR[3] = {2.561552812808830, 0.105124921972503, 4.236067977499789};
        static const double EXP_POST[3] = {1.561552812808830, 0.095124921972503, 0.236067977499789};
        static const double EXP_GAIN[3] = {0.390388203202207, 0.095124921972503, 0.944271909999158};
        int c;
        for (c = 0; c < 3; c++) {
            double Q = QR[c][0], R = QR[c][1], P0 = 10.0, x0 = 0.0, Pp, gain;
            double Pstar = (Q + sqrt(Q * Q + 4 * Q * R)) / 2;
            double post = Pstar * R / (Pstar + R);
            double ymean, S, nu, nis;
            CHECK(near_abs(Pstar, EXP_PRIOR[c], 1e-14), "formula vs listed prior");
            CHECK(near_abs(post, EXP_POST[c], 1e-14), "formula vs listed post");
            CHECK(est_ref_init(&r, 1, &x0, &P0) == 0, "init");
            for (k = 0; k < 400; k++) {
                z = 0.0;
                est_ref_predict(&r, &one, &Q);
                if (k == 399) {
                    est_ref_get(&r, NULL, &Pp);
                    est_ref_innovation(&r, 1, &one, &R, &z, &ymean, &S, &nu, &nis);
                }
                est_ref_update(&r, 1, &one, &R, &z);
            }
            est_ref_get(&r, NULL, &P);
            gain = P / R; /* K = P+ H R^-1 */
            printf("Riccati Q=%g R=%g: prior=%.15f (exact %.15f) post=%.15f (exact %.15f) gain=%.15f (exact %.15f)\n",
                   Q, R, Pp, EXP_PRIOR[c], P, EXP_POST[c], gain, EXP_GAIN[c]);
            CHECK(near_abs(Pp, EXP_PRIOR[c], 1e-12), "prior P* Q=%g", Q);
            CHECK(near_abs(P, EXP_POST[c], 1e-12), "post Q=%g", Q);
            CHECK(near_abs(gain, EXP_GAIN[c], 1e-12), "gain Q=%g", Q);
            CHECK(near_abs(S, Pstar + R, 1e-12), "S = P*+R");
        }
    }

    /* 3) constant velocity */
    {
        double F[4] = {1, 1, 0, 1}, H[2] = {1, 0}, Rm = 1.0, q = 0.6;
        double Q[4] = {q / 3, q / 2, q / 2, q};
        double x0[2] = {0, 0}, xa[2], Pa[4], zz = 1.0;
        double ymean, S, nu, nis;
        CHECK(est_ref_init(&r, 2, x0, I2) == 0, "cv init");
        est_ref_predict(&r, F, Q);
        est_ref_innovation(&r, 1, H, &Rm, &zz, &ymean, &S, &nu, &nis);
        CHECK(near_abs(S, 3.2, 1e-14), "cv S %.17g", S);
        CHECK(near_abs(nu, 1.0, 1e-14), "cv nu");
        CHECK(near_abs(nis, 0.3125, 1e-14), "cv nis %.17g", nis);
        est_ref_update(&r, 1, H, &Rm, &zz);
        est_ref_get(&r, xa, Pa);
        printf("CV hand step: x=[%.15f %.15f] P=[[%.15f %.15f][%.15f %.15f]] S=%.15f NIS=%.15f\n",
               xa[0], xa[1], Pa[0], Pa[1], Pa[2], Pa[3], S, nis);
        CHECK(near_abs(xa[0], 0.6875, 1e-14) && near_abs(xa[1], 0.40625, 1e-14), "cv x");
        CHECK(near_abs(Pa[0], 0.6875, 1e-14) && near_abs(Pa[1], 0.40625, 1e-14) &&
              near_abs(Pa[2], 0.40625, 1e-14) && near_abs(Pa[3], 1.071875, 1e-14), "cv P");

        /* steady state: reference vs independent hand-coded covariance form */
        {
            static const double QS[2] = {0.6, 0.01};
            static const double RS[2] = {1.0, 4.0};
            int c;
            for (c = 0; c < 2; c++) {
                double qq = QS[c], RR = RS[c], P0[4] = {100, 0, 0, 100};
                double Qc[4] = {qq / 3, qq / 2, qq / 2, qq};
                double a00 = 100, a01 = 0, a11 = 100; /* covariance-form P (symmetric) */
                double prev, cur = 0;
                int it;
                est_ref_init(&r, 2, x0, P0);
                for (it = 0; it < 3000; it++) {
                    double zc = 0.0;
                    double p00, p01, p11, s, k0, k1;
                    est_ref_predict(&r, F, Qc);
                    est_ref_update(&r, 1, H, &RR, &zc);
                    /* hand: P' = F P F^T + Q with F=[[1,1],[0,1]] */
                    p00 = a00 + 2 * a01 + a11 + Qc[0];
                    p01 = a01 + a11 + Qc[1];
                    p11 = a11 + Qc[3];
                    s = p00 + RR;
                    k0 = p00 / s; k1 = p01 / s;
                    a00 = p00 - k0 * p00;
                    a01 = p01 - k0 * p01;
                    a11 = p11 - k1 * p01;
                    prev = cur;
                    est_ref_get(&r, NULL, Pa);
                    cur = Pa[0];
                    if (it > 10 && fabs(cur - prev) < 1e-15) break;
                }
                est_ref_get(&r, xa, Pa);
                printf("CV steady q=%g R=%g after %d it: P+=[[%.12f %.12f][%.12f %.12f]] hand=[[%.12f %.12f][%.12f]]\n",
                       qq, RR, it, Pa[0], Pa[1], Pa[2], Pa[3], a00, a01, a11);
                CHECK(near_abs(Pa[0], a00, 1e-9 * a00) && near_abs(Pa[1], a01, 1e-9 * fabs(a01) + 1e-12) &&
                      near_abs(Pa[3], a11, 1e-9 * a11), "cv steady vs hand q=%g", qq);
                CHECK(it < 3000, "cv steady state not converged");
            }
        }
    }
}

#ifndef EST_REF_STANDALONE
/* ---- part (b) ---- */
#define ND EST_MAX_DIM

typedef struct {
    unsigned n, m;
    double F[ND * ND], Q[ND * ND], H[ND * ND], R[ND * ND], x0[ND], P0[ND * ND];
} scen;

static void mk_spd(rng *g, double *M, unsigned n, double floor_, double amp)
{
    double A[ND * ND];
    unsigned i, j, k;
    for (i = 0; i < n * n; i++) A[i] = rng_uni(g, -amp, amp);
    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++) {
            double s = 0;
            for (k = 0; k < n; k++) s += A[i * n + k] * A[j * n + k];
            M[i * n + j] = s + (i == j ? floor_ : 0.0);
        }
}

static void mk_scen(rng *g, scen *s, unsigned n, unsigned m)
{
    unsigned i, j;
    memset(s, 0, sizeof *s);
    s->n = n; s->m = m;
    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++)
            s->F[i * n + j] = rng_uni(g, -0.3, 0.3) + (i == j ? 0.7 : 0.0);
    mk_spd(g, s->Q, n, 0.1, 0.5);
    mk_spd(g, s->R, m, 0.5, 0.5);
    mk_spd(g, s->P0, n, 1.0, 1.0);
    for (i = 0; i < m; i++)
        for (j = 0; j < n; j++) s->H[i * n + j] = rng_uni(g, -1, 1);
    for (i = 0; i < n; i++) s->x0[i] = rng_uni(g, -2, 2);
}

static void fill_model(const scen *s, est_model *mdl)
{
    unsigned i;
    memset(mdl, 0, sizeof *mdl);
    mdl->n = s->n; mdl->m = s->m;
    mdl->estimator = EST_ESTIMATOR_LINEAR_KALMAN;
    mdl->meaning = EST_UNCERTAINTY_GAUSSIAN_COVARIANCE;
    for (i = 0; i < ND; i++) { mdl->state_unit[i] = i < s->n ? EST_UNIT_DIMENSIONLESS : EST_UNIT_NONE;
                               mdl->obs_unit[i] = i < s->m ? EST_UNIT_DIMENSIONLESS : EST_UNIT_NONE; }
    memcpy(mdl->F, s->F, sizeof mdl->F);
    memcpy(mdl->Q, s->Q, sizeof mdl->Q);
    memcpy(mdl->H, s->H, sizeof mdl->H);
    memcpy(mdl->R, s->R, sizeof mdl->R);
    mdl->step_ns = 1000;
}

static void fill_obs(const scen *s, est_observation *o, const double *z, int64_t t_ns, uint64_t seq)
{
    unsigned i;
    memset(o, 0, sizeof *o);
    o->m = s->m;
    for (i = 0; i < s->m; i++) { o->unit[i] = EST_UNIT_DIMENSIONLESS; o->z[i] = z[i]; }
    memcpy(o->R, s->R, sizeof o->R);
    o->t_ns = t_ns; o->seq = seq;
    for (i = 0; i < EST_DIGEST_SIZE; i++) { o->source.b[i] = (uint8_t)(0x10 + i); o->evidence.b[i] = (uint8_t)(0xA0 ^ i); }
}

/* max |a-b| / max(|ref| entries, floor) */
static double rel_diff(const double *a, const double *ref, unsigned cnt)
{
    double mx = 0, d = 0;
    unsigned i;
    for (i = 0; i < cnt; i++) { if (fabs(ref[i]) > mx) mx = fabs(ref[i]); }
    if (mx < 1e-12) mx = 1e-12;
    for (i = 0; i < cnt; i++) { double e = fabs(a[i] - ref[i]); if (e > d) d = e; }
    return d / mx;
}

static void pack_P(const double *Pk, unsigned n, double *out) /* est records use stride EST_MAX_DIM? no: n*n */
{ memcpy(out, Pk, n * n * sizeof(double)); }

/* The ASan/UBSan build passes smaller values (mk/estimation.mk): same checks, fewer scenarios/steps. */
#ifndef NIS_STEPS
#define NIS_STEPS 100000u
#endif
#ifndef XCHECK_SCENARIOS
#define XCHECK_SCENARIOS 600u
#endif

static void xcheck(void)
{
    const unsigned NSCEN = XCHECK_SCENARIOS, STEPS = 50;
    double wx = 0, wP = 0, wN = 0;
    unsigned sc, tested_steps = 0, coasts = 0, updates = 0;
    for (sc = 0; sc < NSCEN; sc++) {
        rng g;
        scen s;
        est_model mdl;
        est_belief b, nb;
        est_prediction pred;
        est_observation obs;
        est_innovation inn;
        est_ref_info ref;
        unsigned n, m, t;
        est_status st;
        rng_seed(&g, SEED_XCHECK_BASE + (uint64_t)sc * SEED_STRIDE);
        n = 1 + (unsigned)(rng_u64(&g) % 4);
        m = 1 + (unsigned)(rng_u64(&g) % n);
        mk_scen(&g, &s, n, m);
        fill_model(&s, &mdl);
        st = est_kf_prior(&mdl, s.x0, s.P0, 0, &b);
        CHECK(st == EST_OK, "scen %u prior status %d", sc, (int)st);
        if (st != EST_OK) continue;
        CHECK(est_ref_init(&ref, n, s.x0, s.P0) == 0, "ref init");
        for (t = 1; t <= STEPS; t++) {
            double z[ND], xr[ND], Pr[ND * ND], Pk[ND * ND], nis_ref = 0, ymr[ND], Sr[ND * ND], nur[ND];
            int have = rng_u01(&g) > 0.3;
            unsigned i;
            for (i = 0; i < m; i++) z[i] = rng_uni(&g, -3, 3);
            st = est_kf_predict(&mdl, &b, NULL, 1, &pred);
            CHECK(st == EST_OK, "scen %u step %u predict %d", sc, t, (int)st);
            if (st != EST_OK) break;
            CHECK(est_ref_predict(&ref, s.F, s.Q) == 0, "ref predict");
            if (have) {
                fill_obs(&s, &obs, z, pred.t_ns, t);
                CHECK(est_ref_innovation(&ref, m, s.H, s.R, z, ymr, Sr, nur, &nis_ref) == 0, "ref innov");
                st = est_kf_update(&mdl, &b, &pred, &obs, &nb, &inn);
                CHECK(st == EST_OK, "scen %u step %u update %d", sc, t, (int)st);
                if (st != EST_OK) break;
                CHECK(est_ref_update(&ref, m, s.H, s.R, z) == 0, "ref update");
                {
                    double e = fabs(inn.nis - nis_ref) / fmax(fabs(nis_ref), 1e-12);
                    if (e > wN) wN = e;
                    CHECK(e <= 1e-8, "scen %u step %u NIS kf=%.17g ref=%.17g", sc, t, inn.nis, nis_ref);
                    e = rel_diff(inn.S, Sr, m * m);
                    CHECK(e <= 1e-9, "scen %u step %u S rel %.3g", sc, t, e);
                }
                updates++;
            } else {
                st = est_kf_coast(&mdl, &pred, &b, &nb);
                CHECK(st == EST_OK, "scen %u step %u coast %d", sc, t, (int)st);
                if (st != EST_OK) break;
                coasts++;
            }
            est_ref_get(&ref, xr, Pr);
            pack_P(nb.P, n, Pk);
            {
                double ex = rel_diff(nb.x, xr, n), eP = rel_diff(Pk, Pr, n * n);
                if (ex > wx) wx = ex;
                if (eP > wP) wP = eP;
                CHECK(ex <= 1e-9, "scen %u (n=%u m=%u) step %u x rel %.3g", sc, n, m, t, ex);
                CHECK(eP <= 1e-9, "scen %u (n=%u m=%u) step %u P rel %.3g", sc, n, m, t, eP);
            }
            b = nb;
            tested_steps++;
        }
    }
    printf("xcheck: %u scenarios, %u steps (%u updates, %u coasts); worst rel x %.3g, P %.3g, NIS %.3g\n",
           NSCEN, tested_steps, updates, coasts, wx, wP, wN);
}

/* Lower-triangular Cholesky for sampling. */
static void chol(const double *A, unsigned n, double *L)
{
    unsigned i, j, k;
    memset(L, 0, ND * ND * sizeof(double));
    for (i = 0; i < n; i++)
        for (j = 0; j <= i; j++) {
            double s = A[i * n + j];
            for (k = 0; k < j; k++) s -= L[i * n + k] * L[j * n + k];
            L[i * n + j] = (i == j) ? sqrt(s) : s / L[j * n + j];
        }
}
static void samp(rng *g, const double *L, unsigned n, double *out)
{
    double w[ND];
    unsigned i, j;
    for (i = 0; i < n; i++) w[i] = rng_gauss(g);
    for (i = 0; i < n; i++) { double s = 0; for (j = 0; j <= i; j++) s += L[i * n + j] * w[j]; out[i] = s; }
}

static void nis_calibration(void)
{
    static const double CHI95[5] = {0, 3.841459, 5.991465, 7.814728, 9.487729};
    const unsigned N = NIS_STEPS;
    unsigned m;
    for (m = 1; m <= 4; m++) {
        rng g;
        scen s;
        est_model mdl;
        est_belief b, nb;
        est_prediction pred;
        est_observation obs;
        est_innovation inn;
        double Lq[ND * ND], Lr[ND * ND], Lp[ND * ND], xt[ND], w[ND], sum = 0;
        unsigned n = 4, t, i, j, above = 0;
        rng_seed(&g, SEED_NIS_BASE + (uint64_t)m * SEED_STRIDE);
        mk_scen(&g, &s, n, m);
        { /* stable truth: scale F so its infinity norm is <= 0.9 (spectral radius < 1); an unstable random F made the truth overflow (protocol note: test-setup fix, not a threshold change) */
          double mx = 0; unsigned r_, c_; for (r_ = 0; r_ < n; r_++) { double rs = 0; for (c_ = 0; c_ < n; c_++) rs += fabs(s.F[r_ * n + c_]); if (rs > mx) mx = rs; }
          if (mx > 0.9) for (r_ = 0; r_ < n * n; r_++) s.F[r_] *= 0.9 / mx; }
        fill_model(&s, &mdl);
        chol(s.Q, n, Lq); chol(s.R, m, Lr); chol(s.P0, n, Lp);
        samp(&g, Lp, n, w);
        for (i = 0; i < n; i++) xt[i] = s.x0[i] + w[i]; /* truth ~ N(x0,P0) */
        CHECK(est_kf_prior(&mdl, s.x0, s.P0, 0, &b) == EST_OK, "nis prior");
        for (t = 1; t <= N; t++) {
            double z[ND], pn[ND], rn[ND], xn[ND];
            { int rc_ = est_kf_predict(&mdl, &b, NULL, 1, &pred); if (rc_ != EST_OK) fprintf(stderr, "DBG predict rc=%d m=%u t=%u\n", rc_, m, t); CHECK(rc_ == EST_OK, "nis predict"); }
            samp(&g, Lq, n, pn);
            for (i = 0; i < n; i++) { double a = 0; for (j = 0; j < n; j++) a += s.F[i * n + j] * xt[j]; xn[i] = a + pn[i]; }
            memcpy(xt, xn, sizeof xn);
            samp(&g, Lr, m, rn);
            for (i = 0; i < m; i++) { double a = 0; for (j = 0; j < n; j++) a += s.H[i * n + j] * xt[j]; z[i] = a + rn[i]; }
            fill_obs(&s, &obs, z, pred.t_ns, t);
            if (est_kf_update(&mdl, &b, &pred, &obs, &nb, &inn) != EST_OK) { CHECK(0, "nis update"); return; }
            sum += inn.nis;
            if (inn.nis > CHI95[m]) above++;
            b = nb;
        }
        {
            double mean = sum / N, se = sqrt(2.0 * m / N);
            double frac = (double)above / N, sef = sqrt(0.05 * 0.95 / N);
            printf("NIS m=%u N=%u: mean=%.5f (expect %u, SE %.5f, z=%.2f); frac>chi2_95=%.5f (expect 0.05, SE %.5f, z=%.2f)\n",
                   m, N, mean, m, se, (mean - m) / se, frac, sef, (frac - 0.05) / sef);
            CHECK(fabs(mean - m) <= 3 * se, "NIS mean m=%u", m);
            CHECK(fabs(frac - 0.05) <= 3 * sef, "NIS tail fraction m=%u", m);
        }
    }
}
#endif

int main(void)
{
    part_a();
#ifndef EST_REF_STANDALONE
    xcheck();
    nis_calibration();
#endif
    if (g_fail) { printf("est_ref: FAIL (%d checks)\n", g_fail); return 1; }
    printf("est_ref: PASS\n");
    return 0;
}
