/* DUAL-0b tests: binding to est_* records, unit table, evidence verification,
 * freshness classification. */
#include "dual_fixtures.h"
#include <math.h>
#include <stdio.h>

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_ST(expr, want) do { RxDualStatus s_ = (expr); g_checks++; if (s_ != (want)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s -> %d, want %d\n", __FILE__, __LINE__, #expr, (int)s_, (int)(want)); } } while (0)

static void test_units(void)
{
    RxDualUnit u;
    CHECK_ST(rx_dual_unit_from_est(EST_UNIT_BYTE, &u), RX_DUAL_OK); CHECK(u == RX_DUAL_UNIT_BYTES);
    CHECK_ST(rx_dual_unit_from_est(EST_UNIT_NANOSECOND, &u), RX_DUAL_OK); CHECK(u == RX_DUAL_UNIT_NS);
    CHECK_ST(rx_dual_unit_from_est(EST_UNIT_WATT, &u), RX_DUAL_OK); CHECK(u == RX_DUAL_UNIT_WATT);
    CHECK_ST(rx_dual_unit_from_est(EST_UNIT_LOG2_PICOSECOND, &u), RX_DUAL_OK); CHECK(u == RX_DUAL_UNIT_LOG2_PS);
    CHECK_ST(rx_dual_unit_from_est(EST_UNIT_NONE, &u), RX_DUAL_ERR_UNIT); CHECK(u == RX_DUAL_UNIT_NONE);
    CHECK_ST(rx_dual_unit_from_est(EST_UNIT_MAX_, &u), RX_DUAL_ERR_UNIT);
    CHECK_ST(rx_dual_unit_from_est((est_unit)77, &u), RX_DUAL_ERR_UNIT);
    CHECK_ST(rx_dual_unit_from_est(EST_UNIT_BYTE, NULL), RX_DUAL_ERR_NULL);
}

static void test_bind_belief_and_evidence(void)
{
    est_model m = fx_model(EST_UNIT_BYTE);
    est_observation o = fx_obs(EST_UNIT_BYTE, 5200.0, 100.0, 1);
    est_digest zero; memset(&zero, 0, sizeof zero);
    est_belief b = fx_belief(&m, &o, &zero, EST_UNIT_BYTE, 5150.0, 64.0, 7);
    RxDualInput in; RxDualDigest cal = fx_dig(0x44); est_digest bd;
    CHECK(est_check_belief(&b) == EST_OK);
    CHECK_ST(rx_dual_bind_belief(&b, 0, &cal, &in), RX_DUAL_OK);
    CHECK(in.estimate == 5150.0 && in.uncertainty == 8.0 && in.unit == RX_DUAL_UNIT_BYTES && in.generation == 7);
    CHECK(in.estimate_kind == RX_DUAL_EST_ESTIMATED && in.evidence_verified == 0 && rx_dual_digest_is_zero(&in.observation_ref));
    est_digest_belief(&b, &bd); CHECK(memcmp(bd.b, in.estimate_ref.b, 32) == 0);     /* cites the record, copies nothing else */
    CHECK(memcmp(in.evidence_root.b, b.evidence_root.b, 32) == 0);
    /* evidence verification: pass, then a broken root, then a wrong observation */
    CHECK_ST(rx_dual_verify_belief_evidence(&b, &zero, &o, &in), RX_DUAL_OK); CHECK(in.evidence_verified == 1);
    { est_belief bad = b; bad.evidence_root.b[3] ^= 1; RxDualInput in2;
      CHECK_ST(rx_dual_bind_belief(&bad, 0, &cal, &in2), RX_DUAL_OK);
      CHECK_ST(rx_dual_verify_belief_evidence(&bad, &zero, &o, &in2), RX_DUAL_ERR_EVIDENCE); CHECK(in2.evidence_verified == 0); }
    { est_observation o2 = fx_obs(EST_UNIT_BYTE, 5200.0, 100.0, 2); RxDualInput in3;
      CHECK_ST(rx_dual_bind_belief(&b, 0, &cal, &in3), RX_DUAL_OK);
      CHECK_ST(rx_dual_verify_belief_evidence(&b, &zero, &o2, &in3), RX_DUAL_ERR_EVIDENCE); }
    { est_digest other; memset(other.b, 0x99, 32); RxDualInput in4;
      CHECK_ST(rx_dual_bind_belief(&b, 0, &cal, &in4), RX_DUAL_OK);
      CHECK_ST(rx_dual_verify_belief_evidence(&b, &other, &o, &in4), RX_DUAL_ERR_EVIDENCE); }
    /* input not bound from this belief */
    { RxDualInput in5 = fx_input(1.0, 1.0, 7, 1); CHECK_ST(rx_dual_verify_belief_evidence(&b, &zero, &o, &in5), RX_DUAL_ERR_DIGEST); }
    /* refusals */
    CHECK_ST(rx_dual_bind_belief(&b, 1, &cal, &in), RX_DUAL_ERR_RANGE);                  /* component out of range */
    { est_belief prior = b; prior.generation = 0; memset(prior.parent.b, 0, 32); memset(prior.evidence_root.b, 0, 32);
      CHECK(est_check_belief(&prior) == EST_OK);
      CHECK_ST(rx_dual_bind_belief(&prior, 0, &cal, &in), RX_DUAL_ERR_EVIDENCE); }       /* a prior is not evidence */
    { est_belief nf = b; nf.x[0] = NAN; CHECK_ST(rx_dual_bind_belief(&nf, 0, &cal, &in), RX_DUAL_ERR_KIND); } /* est refuses it first */
    { est_belief nu = b; nu.unit[0] = EST_UNIT_NONE; CHECK(rx_dual_bind_belief(&nu, 0, &cal, &in) != RX_DUAL_OK); }
    CHECK_ST(rx_dual_bind_belief(NULL, 0, &cal, &in), RX_DUAL_ERR_NULL);
    /* uncalibrated binding is allowed and recorded as such */
    CHECK_ST(rx_dual_bind_belief(&b, 0, NULL, &in), RX_DUAL_OK); CHECK(rx_dual_digest_is_zero(&in.calibration_ref));
}

static void test_bind_prediction_and_observation(void)
{
    est_model m = fx_model(EST_UNIT_NANOSECOND);
    est_observation o = fx_obs(EST_UNIT_NANOSECOND, 10.0, 1.0, 1);
    est_digest zero; memset(&zero, 0, sizeof zero);
    est_belief prior = fx_belief(&m, &o, &zero, EST_UNIT_NANOSECOND, 10.0, 1.0, 3);
    est_prediction p; RxDualInput in; est_digest pd;
    memset(&p, 0, sizeof p);
    est_digest_belief(&prior, &p.prior); est_digest_model(&m, &p.model);
    p.horizon = 2; p.generation = 5; p.t_ns = 5000; p.n = 1; p.m = 1; p.x[0] = 12.0; p.P[0] = 2.25; p.y_mean[0] = 12.0; p.S[0] = 2.29;
    CHECK(est_check_prediction(&p) == EST_OK);
    CHECK_ST(rx_dual_bind_prediction(&p, &m, &prior, 0, NULL, &in), RX_DUAL_OK);
    CHECK(in.estimate == 12.0 && in.uncertainty == 1.5 && in.unit == RX_DUAL_UNIT_NS && in.generation == 5 && in.estimate_kind == RX_DUAL_EST_PREDICTED);
    est_digest_prediction(&p, &pd); CHECK(memcmp(pd.b, in.estimate_ref.b, 32) == 0);
    CHECK(memcmp(in.evidence_root.b, prior.evidence_root.b, 32) == 0);
    { est_model m2 = m; m2.Q[0] = 0.02; CHECK_ST(rx_dual_bind_prediction(&p, &m2, &prior, 0, NULL, &in), RX_DUAL_ERR_DIGEST); } /* wrong model */
    { est_belief b2 = prior; b2.x[0] = 11.0; CHECK_ST(rx_dual_bind_prediction(&p, &m, &b2, 0, NULL, &in), RX_DUAL_ERR_DIGEST); } /* wrong prior */
    CHECK_ST(rx_dual_bind_prediction(&p, &m, &prior, 1, NULL, &in), RX_DUAL_ERR_RANGE);
    /* observation: MEASURED, its own evidence */
    CHECK_ST(rx_dual_bind_observation(&o, 0, 9, NULL, &in), RX_DUAL_OK);
    CHECK(in.estimate_kind == RX_DUAL_EST_MEASURED && in.estimate == 10.0 && in.uncertainty == 1.0 && in.generation == 9);
    CHECK(rx_dual_digest_eq(&in.estimate_ref, &in.observation_ref) && in.evidence_verified == 1);
    CHECK(memcmp(in.evidence_root.b, o.evidence.b, 32) == 0);
    { est_observation bad = o; memset(bad.evidence.b, 0, 32); CHECK_ST(rx_dual_bind_observation(&bad, 0, 9, NULL, &in), RX_DUAL_ERR_KIND); }
    CHECK_ST(rx_dual_bind_observation(&o, 3, 9, NULL, &in), RX_DUAL_ERR_RANGE);
}

static void test_classify(void)
{
    RxDualController ctl = fx_ctl(1, 0.1, 0.01, 2.0, 10.0, 4);
    RxDualLambdaState st;
    RxDualInput in = fx_input(5200.0, 100.0, 10, 1);
    RxDualConstraintState prev; memset(&prev, 0, sizeof prev);
    prev.resource_id = 1; prev.unit = RX_DUAL_UNIT_BYTES; prev.cls = RX_DUAL_CLASS_SOFT; prev.budget = 5000.0;
    prev.budget_contract = fx_dig(0x22); prev.estimate_ref = fx_dig(0x33); prev.estimate_kind = RX_DUAL_EST_ESTIMATED;
    prev.estimate = 5000.0; prev.uncertainty = 100.0; prev.calibration_ref = fx_dig(0x44); prev.lambda = 0.0;
    prev.lambda_state = RX_DUAL_LAMBDA_FRESH; prev.controller_id = fx_dig(0x55); prev.generation = 9; prev.tick = 0;
    prev.evidence_root = fx_dig(0x66);
    CHECK_ST(rx_dual_classify_input(&in, &prev, &ctl, 10, &st), RX_DUAL_OK); CHECK(st == RX_DUAL_LAMBDA_FRESH);
    CHECK_ST(rx_dual_classify_input(&in, NULL, &ctl, 10, &st), RX_DUAL_OK); CHECK(st == RX_DUAL_LAMBDA_FRESH);
    in.calibration_ref = fx_dig(0);  CHECK_ST(rx_dual_classify_input(&in, &prev, &ctl, 10, &st), RX_DUAL_OK); CHECK(st == RX_DUAL_LAMBDA_UNCALIBRATED);
    in = fx_input(5200.0, 100.0, 10, 1);
    in.regime_change = 1;            CHECK_ST(rx_dual_classify_input(&in, &prev, &ctl, 10, &st), RX_DUAL_OK); CHECK(st == RX_DUAL_LAMBDA_FROZEN);
    in.regime_change = 0;
    in.evidence_verified = 0;        CHECK_ST(rx_dual_classify_input(&in, &prev, &ctl, 10, &st), RX_DUAL_OK); CHECK(st == RX_DUAL_LAMBDA_STALE);
    in.evidence_verified = 1;
    CHECK_ST(rx_dual_classify_input(&in, &prev, &ctl, 14, &st), RX_DUAL_OK); CHECK(st == RX_DUAL_LAMBDA_FRESH);  /* age 4 == max_age ok */
    CHECK_ST(rx_dual_classify_input(&in, &prev, &ctl, 15, &st), RX_DUAL_OK); CHECK(st == RX_DUAL_LAMBDA_STALE);  /* age 5 > max_age */
    in.generation = 8;               CHECK_ST(rx_dual_classify_input(&in, &prev, &ctl, 10, &st), RX_DUAL_OK); CHECK(st == RX_DUAL_LAMBDA_STALE);  /* older than prev */
    in.generation = 11;              CHECK_ST(rx_dual_classify_input(&in, &prev, &ctl, 10, &st), RX_DUAL_OK); CHECK(st == RX_DUAL_LAMBDA_REFUSED); /* future */
    in.generation = 10;
    in.estimate = NAN;               CHECK_ST(rx_dual_classify_input(&in, &prev, &ctl, 10, &st), RX_DUAL_OK); CHECK(st == RX_DUAL_LAMBDA_REFUSED);
    in.estimate = INFINITY;          CHECK_ST(rx_dual_classify_input(&in, &prev, &ctl, 10, &st), RX_DUAL_OK); CHECK(st == RX_DUAL_LAMBDA_REFUSED);
    in.estimate = 5200.0; in.uncertainty = -1.0; CHECK_ST(rx_dual_classify_input(&in, &prev, &ctl, 10, &st), RX_DUAL_OK); CHECK(st == RX_DUAL_LAMBDA_REFUSED);
    in.uncertainty = 100.0; memset(&in.evidence_root, 0, 32); CHECK_ST(rx_dual_classify_input(&in, &prev, &ctl, 10, &st), RX_DUAL_OK); CHECK(st == RX_DUAL_LAMBDA_REFUSED);
    in = fx_input(5200.0, 100.0, 10, 1);
    in.unit = RX_DUAL_UNIT_NS;       CHECK_ST(rx_dual_classify_input(&in, &prev, &ctl, 10, &st), RX_DUAL_ERR_UNIT);  /* structural */
    in.unit = RX_DUAL_UNIT_BYTES;
    prev.cls = RX_DUAL_CLASS_INVARIANT; CHECK_ST(rx_dual_classify_input(&in, &prev, &ctl, 10, &st), RX_DUAL_ERR_CLASS);
    prev.cls = RX_DUAL_CLASS_SOFT;
    ctl.max_age = 0;                 CHECK_ST(rx_dual_classify_input(&in, &prev, &ctl, 10, &st), RX_DUAL_ERR_RANGE);
    /* evidence chain */
    { RxDualDigest zero = fx_dig(0), r1, r2, r3, e = fx_dig(0x33);
      CHECK_ST(rx_dual_evidence_extend(&zero, &e, &r1), RX_DUAL_OK);
      CHECK_ST(rx_dual_evidence_extend(&r1, &e, &r2), RX_DUAL_OK);
      CHECK_ST(rx_dual_evidence_extend(&zero, &e, &r3), RX_DUAL_OK);
      CHECK(!rx_dual_digest_is_zero(&r1) && !rx_dual_digest_eq(&r1, &r2) && rx_dual_digest_eq(&r1, &r3));
      CHECK_ST(rx_dual_evidence_extend(&zero, &zero, &r1), RX_DUAL_ERR_DIGEST); }
}

int main(void)
{
    test_units();
    test_bind_belief_and_evidence();
    test_bind_prediction_and_observation();
    test_classify();
    printf("test_dual_bind: %d checks, %d failures: %s\n", g_checks, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
