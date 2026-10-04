/* DUAL-1a tests: the reference update as a pure function; analytic cases
 * against an independent closed form (dual_ref.c); property suite; hostile
 * structural cases. */
#include "dual_fixtures.h"
#include "dual_ref.h"
#include <math.h>
#include <stdio.h>

#ifndef PROP_ROUNDS
#define PROP_ROUNDS 3000
#endif
static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_ST(expr, want) do { RxDualStatus s_ = (expr); g_checks++; if (s_ != (want)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s -> %d, want %d\n", __FILE__, __LINE__, #expr, (int)s_, (int)(want)); } } while (0)
static uint64_t g_rng = 0xD1B54A32D192ED03ull;
static uint64_t rnd(void) { g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27; return g_rng * 0x2545F4914F6CDD1Dull; }
static double rdbl(double lo, double hi) { return lo + (hi - lo) * ((double)(rnd() >> 11) / 9007199254740992.0); }

static const double BUDGET = 5000.0, SCALE = 1000.0;
static RxDualDigest BC;

static RxDualStatus run(const RxDualController *ctl, const RxDualResource *res, const double *est, const double *sd,
                        uint32_t n, RxDualConstraintState *out, double *trace)
{
    RxDualConstraintState s, nx; RxDualStatus st;
    RxDualInput in = fx_input(est[0], sd[0], 1, 1);
    if ((st = rx_dual_init_state(res, RX_DUAL_CLASS_SOFT, BUDGET, &BC, &in, ctl, 1, &s)) != RX_DUAL_OK) return st;
    if (trace) trace[0] = s.lambda;
    for (uint32_t t = 1; t < n; t++) {
        in = fx_input(est[t], sd[t], t + 1, 1);
        if ((st = rx_dual_update(&s, res, &in, ctl, t + 1, &nx)) != RX_DUAL_OK) return st;
        if (trace) trace[t] = nx.lambda;
        s = nx;
    }
    *out = s;
    return RX_DUAL_OK;
}

/* ---- analytic cases vs the independent closed form ---- */
static void test_analytic(void)
{
    enum { N = 60 };
    double est[N], sd[N], tr[N];
    RxDualResource res = fx_res(1, RX_DUAL_UNIT_BYTES, SCALE);
    RxDualConstraintState fin;
    /* 1. single binding constraint: estimate 5500 (pressure 0.5), sd 100 (band 0.2) -> g = 0.3 */
    { RxDualController ctl = fx_ctl(1, 0.1, 0.05, 2.0, 10.0, 4); double g = dual_ref_gradient(5500, BUDGET, SCALE, 100, 2.0);
      for (int i = 0; i < N; i++) { est[i] = 5500; sd[i] = 100; }
      CHECK(fabs(g - 0.3) < 1e-12);
      CHECK_ST(run(&ctl, &res, est, sd, N, &fin, tr), RX_DUAL_OK);
      for (unsigned long t = 0; t < N; t++) CHECK(fabs(tr[t] - dual_ref_closed_form(0.0, g, 0.1, 0.05, t + 1)) < 1e-9);
      CHECK(tr[N - 1] > tr[0] && tr[N - 1] <= dual_ref_fixed_point(g, 0.1, 0.05) + 1e-9 && fin.lambda_state == RX_DUAL_LAMBDA_FRESH); }
    /* 2. slack constraint from a nonzero price: estimate 4000 (pressure -1), band 0.2 -> g = -0.8: decays to 0 and stays */
    { RxDualController ctl = fx_ctl(1, 0.1, 0.05, 2.0, 10.0, 4);
      for (int i = 0; i < N; i++) { est[i] = (i < 20) ? 5500 : 4000; sd[i] = 100; }
      CHECK_ST(run(&ctl, &res, est, sd, N, &fin, tr), RX_DUAL_OK);
      CHECK(tr[19] > 0.3 && fin.lambda == 0.0);
      for (int i = 20; i < N; i++) CHECK(tr[i] <= tr[i - 1] && tr[i] >= 0.0); }
    /* 3. two competing constraints: memory binding, latency slack; each priced independently */
    { RxDualController ctl; RxDualResource rm = fx_res(1, RX_DUAL_UNIT_BYTES, SCALE), rl = fx_res(2, RX_DUAL_UNIT_BYTES, SCALE);
      RxDualConstraintState fm, fl; double gm, gl;
      memset(&ctl, 0, sizeof ctl); ctl.eta = 0.2; ctl.rho = 0.1; ctl.k_sigma = 1.0; ctl.max_age = 4; ctl.cadence = 1; ctl.n = 2;
      ctl.resource_id[0] = 1; ctl.resource_id[1] = 2; ctl.lambda_max[0] = 5.0; ctl.lambda_max[1] = 5.0;
      for (int i = 0; i < N; i++) { est[i] = 6000; sd[i] = 50; }
      CHECK_ST(run(&ctl, &rm, est, sd, N, &fm, tr), RX_DUAL_OK);
      gm = dual_ref_gradient(6000, BUDGET, SCALE, 50, 1.0);
      CHECK(fabs(tr[N - 1] - dual_ref_closed_form(0.0, gm, 0.2, 0.1, N)) < 1e-9);
      for (int i = 0; i < N; i++) { est[i] = 4900; sd[i] = 50; }
      CHECK_ST(run(&ctl, &rl, est, sd, N, &fl, tr), RX_DUAL_OK);
      gl = dual_ref_gradient(4900, BUDGET, SCALE, 50, 1.0);
      CHECK(gl < 0.0 && fl.lambda == 0.0 && fm.lambda > 1.0); }
    /* 4. inside the deadband: estimate 5100, sd 100, k 2 -> band 0.2 >= pressure 0.1: no movement, ever */
    { RxDualController ctl = fx_ctl(1, 0.5, 0.0, 2.0, 10.0, 4);
      for (int i = 0; i < N; i++) { est[i] = 5100; sd[i] = 100; }
      CHECK_ST(run(&ctl, &res, est, sd, N, &fin, tr), RX_DUAL_OK);
      for (int i = 0; i < N; i++) CHECK(tr[i] == 0.0); }
    /* 5. clip: eta 1, rho 0, pressure 2 -> hits lambda_max 3 at step 2 and stays */
    { RxDualController ctl = fx_ctl(1, 1.0, 0.0, 0.0, 3.0, 4);
      for (int i = 0; i < N; i++) { est[i] = 7000; sd[i] = 0; }
      CHECK_ST(run(&ctl, &res, est, sd, N, &fin, tr), RX_DUAL_OK);
      CHECK(tr[0] == 2.0 && tr[1] == 3.0 && tr[N - 1] == 3.0); }
    /* 6. zero eta: never moves; 7. rho > 0 forgets scarcity after pressure disappears (inside band) */
    { RxDualController ctl = fx_ctl(1, 0.0, 0.1, 2.0, 10.0, 4);
      for (int i = 0; i < N; i++) { est[i] = 9000; sd[i] = 0; }
      CHECK_ST(run(&ctl, &res, est, sd, N, &fin, tr), RX_DUAL_OK);
      for (int i = 0; i < N; i++) CHECK(tr[i] == 0.0); }
    { RxDualController ctl = fx_ctl(1, 0.5, 0.1, 2.0, 10.0, 4);
      for (int i = 0; i < N; i++) { est[i] = (i < 10) ? 7000 : 5000; sd[i] = 100; }
      CHECK_ST(run(&ctl, &res, est, sd, N, &fin, tr), RX_DUAL_OK);
      CHECK(tr[9] > 1.0);
      for (int i = 10; i < N; i++) CHECK(fabs(tr[i] - tr[9] * pow(0.9, (double)(i - 9))) < 1e-9);
      CHECK(fin.lambda < 0.01 * tr[9]); }
    /* 8. determinism: same inputs -> byte-identical records */
    { RxDualController ctl = fx_ctl(1, 0.1, 0.05, 2.0, 10.0, 4); RxDualConstraintState a, b; uint8_t ea[4096], eb[4096]; size_t la, lb;
      for (int i = 0; i < N; i++) { est[i] = 5300 + 200 * sin(i * 0.3); sd[i] = 80; }
      CHECK_ST(run(&ctl, &res, est, sd, N, &a, NULL), RX_DUAL_OK);
      CHECK_ST(run(&ctl, &res, est, sd, N, &b, NULL), RX_DUAL_OK);
      CHECK_ST(rx_dual_encode_constraint(&a, ea, sizeof ea, &la), RX_DUAL_OK);
      CHECK_ST(rx_dual_encode_constraint(&b, eb, sizeof eb, &lb), RX_DUAL_OK);
      CHECK(la == lb && memcmp(ea, eb, la) == 0 && a.tick == N - 1); }
}

/* ---- held states and structural refusals ---- */
static void test_states_and_refusals(void)
{
    RxDualController ctl = fx_ctl(1, 0.1, 0.05, 2.0, 10.0, 4);
    RxDualResource res = fx_res(1, RX_DUAL_UNIT_BYTES, SCALE);
    RxDualInput in = fx_input(5500, 100, 1, 1);
    RxDualConstraintState s0, s1, s2; RxDualDigest d0;
    CHECK_ST(rx_dual_init_state(&res, RX_DUAL_CLASS_SOFT, BUDGET, &BC, &in, &ctl, 1, &s0), RX_DUAL_OK);
    CHECK(s0.tick == 0 && rx_dual_digest_is_zero(&s0.parent) && s0.lambda > 0.0 && s0.lambda_state == RX_DUAL_LAMBDA_FRESH);
    CHECK_ST(rx_dual_digest_constraint(&s0, &d0), RX_DUAL_OK);
    /* stale input: lambda, estimate, generation, evidence root held; parent/tick advance */
    in = fx_input(9000, 100, 2, 1); in.evidence_verified = 0;
    CHECK_ST(rx_dual_update(&s0, &res, &in, &ctl, 2, &s1), RX_DUAL_OK);
    CHECK(s1.lambda_state == RX_DUAL_LAMBDA_STALE && s1.lambda == s0.lambda && s1.estimate == s0.estimate && s1.generation == s0.generation);
    CHECK(rx_dual_digest_eq(&s1.parent, &d0) && s1.tick == 1 && rx_dual_digest_eq(&s1.evidence_root, &s0.evidence_root));
    /* too old */
    in = fx_input(9000, 100, 2, 1); CHECK_ST(rx_dual_update(&s0, &res, &in, &ctl, 7, &s1), RX_DUAL_OK); CHECK(s1.lambda_state == RX_DUAL_LAMBDA_STALE && s1.lambda == s0.lambda);
    /* refused values: held, REFUSED */
    in = fx_input(NAN, 100, 2, 1); CHECK_ST(rx_dual_update(&s0, &res, &in, &ctl, 2, &s1), RX_DUAL_OK); CHECK(s1.lambda_state == RX_DUAL_LAMBDA_REFUSED && s1.lambda == s0.lambda);
    in = fx_input(9000, -1, 2, 1);  CHECK_ST(rx_dual_update(&s0, &res, &in, &ctl, 2, &s1), RX_DUAL_OK); CHECK(s1.lambda_state == RX_DUAL_LAMBDA_REFUSED);
    in = fx_input(9000, 100, 5, 1); CHECK_ST(rx_dual_update(&s0, &res, &in, &ctl, 2, &s1), RX_DUAL_OK); CHECK(s1.lambda_state == RX_DUAL_LAMBDA_REFUSED); /* future generation */
    /* frozen: regime change holds the price */
    in = fx_input(9000, 100, 2, 1); in.regime_change = 1;
    CHECK_ST(rx_dual_update(&s0, &res, &in, &ctl, 2, &s1), RX_DUAL_OK); CHECK(s1.lambda_state == RX_DUAL_LAMBDA_FROZEN && s1.lambda == s0.lambda);
    /* uncalibrated: lambda moves but is marked; a later calibrated input is FRESH again */
    in = fx_input(9000, 100, 2, 0);
    CHECK_ST(rx_dual_update(&s0, &res, &in, &ctl, 2, &s1), RX_DUAL_OK); CHECK(s1.lambda_state == RX_DUAL_LAMBDA_UNCALIBRATED && s1.lambda > s0.lambda);
    in = fx_input(9000, 100, 3, 1);
    CHECK_ST(rx_dual_update(&s1, &res, &in, &ctl, 3, &s2), RX_DUAL_OK); CHECK(s2.lambda_state == RX_DUAL_LAMBDA_FRESH && s2.tick == 2);
    /* structural refusals: no record */
    in = fx_input(9000, 100, 2, 1);
    { RxDualController m = ctl; m.eta = 0.2; CHECK_ST(rx_dual_update(&s0, &res, &in, &m, 2, &s1), RX_DUAL_ERR_CONTROLLER); }   /* mutated parameters */
    { RxDualController m = ctl; m.resource_id[0] = 2; CHECK_ST(rx_dual_update(&s0, &res, &in, &m, 2, &s1), RX_DUAL_ERR_RESOURCE); } /* resource not priced */
    { RxDualResource r2 = res; r2.unit = RX_DUAL_UNIT_NS; CHECK_ST(rx_dual_update(&s0, &r2, &in, &ctl, 2, &s1), RX_DUAL_ERR_UNIT); }
    { RxDualResource r2 = res; r2.resource_id = 3; CHECK_ST(rx_dual_update(&s0, &r2, &in, &ctl, 2, &s1), RX_DUAL_ERR_RESOURCE); }
    { RxDualResource r2 = res; r2.scale = 0.0; CHECK_ST(rx_dual_update(&s0, &r2, &in, &ctl, 2, &s1), RX_DUAL_ERR_SCALE); }
    { RxDualResource r2 = res; r2.scale = -1.0; CHECK_ST(rx_dual_update(&s0, &r2, &in, &ctl, 2, &s1), RX_DUAL_ERR_SCALE); }
    { RxDualInput u = in; u.unit = RX_DUAL_UNIT_NS; CHECK_ST(rx_dual_update(&s0, &res, &u, &ctl, 2, &s1), RX_DUAL_ERR_UNIT); }         /* unit mismatch */
    { RxDualConstraintState bad = s0; bad.cls = RX_DUAL_CLASS_INVARIANT; CHECK_ST(rx_dual_update(&bad, &res, &in, &ctl, 2, &s1), RX_DUAL_ERR_CLASS); }
    { RxDualConstraintState bad = s0; bad.lambda = 11.0; CHECK_ST(rx_dual_update(&bad, &res, &in, &ctl, 2, &s1), RX_DUAL_ERR_RANGE); } /* above max */
    { RxDualConstraintState bad = s0; bad.lambda = -1.0; CHECK_ST(rx_dual_update(&bad, &res, &in, &ctl, 2, &s1), RX_DUAL_ERR_RANGE); }
    { RxDualConstraintState bad = s0; bad.parent = fx_dig(0x01); CHECK_ST(rx_dual_update(&bad, &res, &in, &ctl, 2, &s1), RX_DUAL_ERR_DIGEST); } /* malformed parent */
    { RxDualConstraintState bad = s0; memset(&bad.controller_id, 0, 32); CHECK_ST(rx_dual_update(&bad, &res, &in, &ctl, 2, &s1), RX_DUAL_ERR_DIGEST); }
    CHECK_ST(rx_dual_update(NULL, &res, &in, &ctl, 2, &s1), RX_DUAL_ERR_NULL);
    CHECK_ST(rx_dual_init_state(&res, RX_DUAL_CLASS_INVARIANT, BUDGET, &BC, &in, &ctl, 1, &s1), RX_DUAL_ERR_CLASS);
    CHECK_ST(rx_dual_init_state(&res, RX_DUAL_CLASS_UNDECLARED, BUDGET, &BC, &in, &ctl, 1, &s1), RX_DUAL_ERR_CLASS);
    CHECK_ST(rx_dual_init_state(&res, RX_DUAL_CLASS_SOFT, INFINITY, &BC, &in, &ctl, 1, &s1), RX_DUAL_ERR_NONFINITE);
    { RxDualDigest z = fx_dig(0); CHECK_ST(rx_dual_init_state(&res, RX_DUAL_CLASS_SOFT, BUDGET, &z, &in, &ctl, 1, &s1), RX_DUAL_ERR_DIGEST); }
    { RxDualInput u = in; u.estimate = NAN; CHECK_ST(rx_dual_init_state(&res, RX_DUAL_CLASS_SOFT, BUDGET, &BC, &u, &ctl, 1, &s1), RX_DUAL_ERR_NONFINITE); }
    /* CAPACITY: diagnostic price computed; class preserved */
    CHECK_ST(rx_dual_init_state(&res, RX_DUAL_CLASS_CAPACITY, BUDGET, &BC, &in, &ctl, 2, &s1), RX_DUAL_OK);
    CHECK(s1.cls == RX_DUAL_CLASS_CAPACITY && s1.lambda > 0.0);
    /* step function edge cases */
    { double o; CHECK_ST(rx_dual_step_lambda(0.5, INFINITY, 1, BUDGET, SCALE, &ctl, 10, &o), RX_DUAL_ERR_NONFINITE);
      CHECK_ST(rx_dual_step_lambda(0.5, -INFINITY, 1, BUDGET, SCALE, &ctl, 10, &o), RX_DUAL_ERR_NONFINITE);
      CHECK_ST(rx_dual_step_lambda(0.5, 1, 1, BUDGET, 0.0, &ctl, 10, &o), RX_DUAL_ERR_SCALE);
      CHECK_ST(rx_dual_step_lambda(0.5, 1, 1, BUDGET, -1.0, &ctl, 10, &o), RX_DUAL_ERR_SCALE);
      CHECK_ST(rx_dual_step_lambda(-0.5, 1, 1, BUDGET, SCALE, &ctl, 10, &o), RX_DUAL_ERR_RANGE);
      CHECK_ST(rx_dual_step_lambda(0.5, 1, -1, BUDGET, SCALE, &ctl, 10, &o), RX_DUAL_ERR_RANGE);
      CHECK_ST(rx_dual_step_lambda(11, 1, 1, BUDGET, SCALE, &ctl, 10, &o), RX_DUAL_ERR_RANGE);
      CHECK_ST(rx_dual_step_lambda(0.5, 1e308, 1, -1e308, SCALE, &ctl, 10, &o), RX_DUAL_ERR_NONFINITE); } /* overflow refused, never clipped into range */
}

/* ---- property suite ---- */
static void test_properties(void)
{
    for (int round = 0; round < PROP_ROUNDS; round++) {
        RxDualController ctl = fx_ctl(1, rdbl(0, 2), rdbl(0, 1), rdbl(0, 4), rdbl(0.01, 50), 1 + rnd() % 8);
        RxDualResource res = fx_res(1, RX_DUAL_UNIT_BYTES, rdbl(1e-3, 1e6));
        RxDualInput in = fx_input(rdbl(-1e6, 1e6), rdbl(0, 1e4), 1, 1);
        RxDualConstraintState s, nx; RxDualStatus st;
        double budget = rdbl(-1e6, 1e6), lmax = ctl.lambda_max[0];
        st = rx_dual_init_state(&res, RX_DUAL_CLASS_SOFT, budget, &BC, &in, &ctl, 1, &s);
        CHECK_ST(st, RX_DUAL_OK);
        for (uint64_t t = 2; t <= 40; t++) {
            double g, want, prev = s.lambda;
            in = fx_input(rdbl(-1e6, 1e6), rdbl(0, 1e4), t, (rnd() % 5) != 0);
            if (rnd() % 7 == 0) in.evidence_verified = 0;
            CHECK_ST(rx_dual_update(&s, &res, &in, &ctl, t, &nx), RX_DUAL_OK);
            CHECK(isfinite(nx.lambda) && nx.lambda >= 0.0 && nx.lambda <= lmax && nx.tick == t - 1);
            if (nx.lambda_state == RX_DUAL_LAMBDA_STALE) CHECK(nx.lambda == prev);
            else {
                g = dual_ref_gradient(in.estimate, budget, res.scale, in.uncertainty, ctl.k_sigma);
                want = (1.0 - ctl.rho) * prev + ctl.eta * g;
                if (want < 0) want = 0;
                if (want > lmax) want = lmax;
                CHECK(fabs(nx.lambda - want) <= 1e-9 * (1.0 + fabs(want)));
                if (g == 0.0) CHECK(nx.lambda <= prev);                 /* inside deadband: only the leak can act */
                if (ctl.eta == 0.0) CHECK(nx.lambda <= prev);
            }
            s = nx;
        }
    }
}

int main(void)
{
    BC = fx_dig(0x22);
    test_analytic();
    test_states_and_refusals();
    test_properties();
    printf("test_dual_update: %d checks, %d failures: %s\n", g_checks, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
