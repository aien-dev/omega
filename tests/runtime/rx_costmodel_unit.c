/*
 * rx_costmodel_unit -- the empirical cost model on synthetic measurements.
 *
 * Any host. Checks fit, calibration of the predictive intervals on held-out
 * points, the decision rules (select, measure when unsure, frozen, budget,
 * eligibility, failures, fallback, latency/energy budgets) and the canonical
 * blob (round trip, tamper, truncation, version).
 */
#include "runtime/rx_costmodel.h"
#include "sha256.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_fail;

#define CHECK(cond, ...) do {                                            \
        g_checks++;                                                      \
        if (!(cond)) {                                                   \
            g_fail++;                                                    \
            fprintf(stderr, "  FAIL %s:%d ", __FILE__, __LINE__);        \
            fprintf(stderr, __VA_ARGS__);                                \
            fputc('\n', stderr);                                         \
        }                                                                \
    } while (0)

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t next(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}
static double unif(void) { return (double)(next() >> 11) / 9007199254740992.0; }
static double gauss(void) {
    double u = unif(), v = unif();
    if (u < 1e-300) u = 1e-300;
    return sqrt(-2.0 * log(u)) * cos(6.283185307179586 * v);
}

/* Synthetic truth: log2 ps for three arms, shaped like the Spark probe
 * (a fixed cost, a per-element cost, a thin-matrix penalty for arm 2). */
static int g_exact;   /* 1: truth lies in the model's basis */
static double truth(uint32_t arm, uint32_t M, uint32_t N, uint32_t core) {
    double w = g_exact ? log2((double)M) + log2((double)N) : log2((double)M * N + 1.0);
    double base = 10.0 + 0.95 * w + (core ? 0.4 : 0.0);
    if (arm == 1) base += 0.15;
    if (arm == 2) base += (N <= 3 ? 0.8 : -0.7);
    return base;
}

static RxCmFeatures feat(uint32_t M, uint32_t N, uint32_t core, uint32_t pressure) {
    RxCmFeatures f;
    memset(&f, 0, sizeof f);
    f.op = RX_CM_OP_MATVEC;
    f.M = M;
    f.N = N;
    f.state_bytes = ((uint64_t)M * N + M + N) * 8u;
    f.cache_bytes = 2u << 20;
    f.core = core;
    f.pressure = pressure;
    f.eligible = 0x7;
    f.need_evidence = 1;
    return f;
}

static const uint32_t TRAIN_M[] = { 1, 4, 16, 64, 256 };
/* Odd and even sizes, so "N <= 3" and "N % 4 != 0" are not confounded. */
static const uint32_t TRAIN_N[] = { 1, 2, 3, 5, 8, 12, 33, 64, 100, 256, 1024 };
#define N_TRAIN_N (sizeof TRAIN_N / sizeof TRAIN_N[0])

static void train(RxCostModel *m, double noise, uint32_t core, uint32_t pressure, int reps) {
    for (int r = 0; r < reps; r++)
        for (unsigned i = 0; i < 5; i++)
            for (unsigned j = 0; j < N_TRAIN_N; j++)
                for (uint32_t a = 0; a < 3; a++) {
                    RxCmFeatures f = feat(TRAIN_M[i], TRAIN_N[j], core, pressure);
                    double y = truth(a, f.M, f.N, core) + noise * gauss();
                    RxCmObservation o = { (uint64_t)exp2(y), 0 };
                    rx_cm_observe(m, &f, a, &o);
                }
}

static void t_basis(void) {
    RxCmFeatures f = feat(8, 3, RX_CM_CORE_A725, 1);
    f.state_bytes = 8u << 20;
    double phi[RX_CM_D];
    rx_cm_basis(&f, phi);
    CHECK(phi[0] == 1.0 && phi[1] == 3.0 && fabs(phi[2] - log2(3.0)) < 1e-12, "basis logs");
    CHECK(phi[5] == 1.0 && phi[6] == 1.0 && fabs(phi[7] - 2.0) < 1e-12, "basis indicators");
    CHECK(rx_cm_cell(&f) == 1u * RX_CM_PRESSURE + 1u, "cell index");
    f.core = 99;
    f.pressure = 99;
    CHECK(rx_cm_cell(&f) < RX_CM_CELLS, "out-of-range features clamp into a cell");
}

static void t_fit_and_calibration(int exact, double *cov80, double *cov95, double *rmse) {
    g_exact = exact;
    RxCostModel m;
    rx_cm_init(&m, 3);
    const double noise = 0.08;
    train(&m, noise, RX_CM_CORE_X925, 0, 2);
    uint32_t in80 = 0, in95 = 0, n = 0;
    double se = 0.0;
    double q80 = 0, q95 = 0;
    for (int k = 0; k < 4000; k++) {
        /* held out: shapes not on the training grid */
        uint32_t M = 1 + (uint32_t)(next() % 300);
        uint32_t N = 1 + (uint32_t)(next() % 1000);
        uint32_t a = (uint32_t)(next() % 3);
        RxCmFeatures f = feat(M, N, RX_CM_CORE_X925, 0);
        RxCmPrediction p;
        rx_cm_predict(&m, &f, a, &p);
        double y = truth(a, M, N, 0) + noise * gauss();
        double z = fabs(y - p.mean_log2_ps);
        q80 = rx_cm_t_quantile(0.80, p.dof) * p.sd_log2;
        q95 = rx_cm_t_quantile(0.95, p.dof) * p.sd_log2;
        in80 += z <= q80;
        in95 += z <= q95;
        se += (y - p.mean_log2_ps) * (y - p.mean_log2_ps);
        n++;
    }
    *cov80 = (double)in80 / n;
    *cov95 = (double)in95 / n;
    *rmse = sqrt(se / n);
    printf("    held-out synthetic (%s): rmse %.3f log2 (noise %.2f), coverage 80%%: %.3f, 95%%: %.3f\n",
           exact ? "truth in basis" : "truth outside basis", *rmse, noise, *cov80, *cov95);
    g_exact = 0;
    CHECK(*rmse < 0.2, "held-out error %.3f", *rmse);
    if (exact) {
        CHECK(*cov80 > 0.74 && *cov80 < 0.86, "80%% interval covers %.3f", *cov80);
        CHECK(*cov95 > 0.92 && *cov95 < 0.98, "95%% interval covers %.3f", *cov95);
    } else {   /* misspecified: may be wide, must not be overconfident */
        CHECK(*cov80 > 0.74, "80%% interval covers %.3f", *cov80);
        CHECK(*cov95 > 0.92, "95%% interval covers %.3f", *cov95);
    }
}

static void t_decide(void) {
    RxCmPolicy pol;
    rx_cm_default_policy(&pol);
    RxCostModel m;
    rx_cm_init(&m, 3);
    RxCmDecision d;
    RxCmFeatures f = feat(64, 256, RX_CM_CORE_X925, 0);

    rx_cm_decide(&m, &pol, &f, 1, &d);
    CHECK(d.action == RX_CM_MEASURE && d.why == RX_CM_WHY_UNKNOWN_ARM, "empty model measures");
    rx_cm_decide(&m, &pol, &f, 0, &d);
    CHECK(d.action == RX_CM_FALLBACK && d.arm == 0, "empty frozen model falls back");

    train(&m, 0.03, RX_CM_CORE_X925, 0, 2);
    rx_cm_decide(&m, &pol, &f, 1, &d);
    CHECK(d.action == RX_CM_SELECT && d.arm == 2 && d.why == RX_CM_WHY_CONFIDENT,
          "wide matrix: arm 2 selected confidently (action %d arm %u why %u)", d.action, d.arm,
          d.why);
    f = feat(256, 1, RX_CM_CORE_X925, 0);
    rx_cm_decide(&m, &pol, &f, 1, &d);
    CHECK(d.action == RX_CM_SELECT && d.arm == 0, "thin matrix: arm 0 selected (arm %u)", d.arm);

    /* two arms predicted equal: worth a run only if the model is unsure */
    RxCostModel c, noisy;
    rx_cm_init(&c, 2);
    rx_cm_init(&noisy, 2);
    RxCmFeatures g = feat(16, 16, RX_CM_CORE_X925, 0);
    g.eligible = 0x3;
    for (int r = 0; r < 30; r++) {
        RxCmObservation o0 = { (uint64_t)exp2(20.0 + 0.02 * gauss()), 0 };
        RxCmObservation o1 = { (uint64_t)exp2(20.01 + 0.02 * gauss()), 0 };
        rx_cm_observe(&c, &g, 0, &o0);
        rx_cm_observe(&c, &g, 1, &o1);
        RxCmObservation n0 = { (uint64_t)exp2(20.0 + 0.4 * gauss()), 0 };
        RxCmObservation n1 = { (uint64_t)exp2(20.0 + 0.4 * gauss()), 0 };
        rx_cm_observe(&noisy, &g, 0, &n0);
        rx_cm_observe(&noisy, &g, 1, &n1);
    }
    rx_cm_decide(&c, &pol, &g, 1, &d);
    CHECK(d.action == RX_CM_SELECT && d.why == RX_CM_WHY_CONFIDENT,
          "equal and tight: not worth a run (action %d, ei %.3f)", d.action, d.expected_improvement);
    rx_cm_decide(&noisy, &pol, &g, 1, &d);
    CHECK(d.action == RX_CM_MEASURE && d.why == RX_CM_WHY_WORTH_MEASURING,
          "equal and unsure: measured (action %d, ei %.3f)", d.action, d.expected_improvement);
    rx_cm_decide(&noisy, &pol, &g, 0, &d);
    CHECK(d.action == RX_CM_SELECT, "frozen: never measures");
    rx_cm_charge(&noisy, 0, pol.explore_allow_ps + 1);
    rx_cm_decide(&noisy, &pol, &g, 1, &d);
    CHECK(d.action == RX_CM_SELECT && d.why == RX_CM_WHY_BUDGET_SPENT,
          "spent budget: select instead of measure");
    rx_cm_charge(&noisy, (uint64_t)((double)(pol.explore_allow_ps + 1) / pol.explore_frac) * 2, 0);
    rx_cm_decide(&noisy, &pol, &g, 1, &d);
    CHECK(d.action == RX_CM_MEASURE, "budget refills as chosen work accrues");

    /* unseen pressure: borrowed and widened */
    f = feat(64, 256, RX_CM_CORE_X925, 2);
    RxCmPrediction p0, p2;
    RxCmFeatures f0 = feat(64, 256, RX_CM_CORE_X925, 0);
    rx_cm_predict(&m, &f0, 2, &p0);
    rx_cm_predict(&m, &f, 2, &p2);
    CHECK(p2.borrowed && !p0.borrowed && p2.mean_log2_ps == p0.mean_log2_ps &&
              fabs(p2.sd_log2 - sqrt(p0.sd_log2 * p0.sd_log2 + RX_CM_UNSEEN_SHIFT * RX_CM_UNSEEN_SHIFT)) < 1e-9,
          "unseen pressure: quiet prediction, widened by the unseen-shift prior");

    /* Pressure learned as a correction: arm 2 slows by 1.5 doublings, arm 0
     * by 0.2. A handful of observations flips the choice. */
    RxCostModel pm = m;
    RxCmFeatures pf[6];
    for (int i = 0; i < 6; i++) {
        pf[i] = feat(TRAIN_M[i % 5], TRAIN_N[(i * 3) % N_TRAIN_N], RX_CM_CORE_X925, 1);
        for (uint32_t a = 0; a < 3; a++) {
            double shift = a == 2 ? 1.5 : 0.2;
            RxCmObservation o = { (uint64_t)exp2(truth(a, pf[i].M, pf[i].N, 0) + shift + 0.03 * gauss()), 0 };
            CHECK(rx_cm_observe(&pm, &pf[i], a, &o) == RX_CM_OK, "pressure observation");
        }
    }
    RxCmFeatures q1 = feat(64, 256, RX_CM_CORE_X925, 1);
    RxCmPrediction pq;
    rx_cm_predict(&pm, &q1, 2, &pq);
    CHECK(pq.known && !pq.borrowed && fabs(pq.mean_log2_ps - (p0.mean_log2_ps + 1.5)) < 0.15,
          "pressure correction learned from 6 points (%.2f vs %.2f)", pq.mean_log2_ps,
          p0.mean_log2_ps + 1.5);
    rx_cm_decide(&pm, &pol, &q1, 0, &d);
    CHECK(d.arm == 0, "under pressure the choice flips to arm 0 (got %u)", d.arm);
    RxCostModel bare;
    rx_cm_init(&bare, 3);
    RxCmObservation ob = { 1000, 0 };
    CHECK(rx_cm_observe(&bare, &q1, 0, &ob) == RX_CM_SKIPPED, "no quiet model: pressure data skipped");
    rx_cm_decide(&m, &pol, &f, 0, &d);
    CHECK(d.action == RX_CM_SELECT && d.arm == 2, "frozen uses the borrowed prediction");
    RxCostModel mid;   /* close, moderately sure: not worth a run here, worth one when borrowed */
    rx_cm_init(&mid, 2);
    for (int r = 0; r < 40; r++) {
        RxCmObservation o0 = { (uint64_t)exp2(20.0 + 0.1 * gauss()), 0 };
        RxCmObservation o1 = { (uint64_t)exp2(20.0 + 0.1 * gauss()), 0 };
        rx_cm_observe(&mid, &g, 0, &o0);
        rx_cm_observe(&mid, &g, 1, &o1);
    }
    rx_cm_decide(&mid, &pol, &g, 1, &d);
    CHECK(d.action == RX_CM_SELECT, "close and moderately sure: select (ei %.3f)", d.expected_improvement);
    RxCmFeatures gp = g;
    gp.pressure = 1;
    rx_cm_decide(&mid, &pol, &gp, 1, &d);
    CHECK(d.action == RX_CM_MEASURE && d.why == RX_CM_WHY_WORTH_MEASURING,
          "same arms under unseen pressure: widened spread makes it worth a run (ei %.3f)",
          d.expected_improvement);

    /* unseen core class with no data anywhere: fallback or measure */
    f = feat(64, 256, RX_CM_CORE_A725, 0);
    rx_cm_decide(&m, &pol, &f, 0, &d);
    CHECK(d.action == RX_CM_FALLBACK && d.arm == 0, "no data for this core class: fallback");
}

static void t_eligibility_property(void) {
    RxCmPolicy pol;
    rx_cm_default_policy(&pol);
    RxCostModel m;
    rx_cm_init(&m, 4);
    /* arm 3 would be the fastest by far, but is never eligible */
    for (int r = 0; r < 3; r++)
        for (unsigned i = 0; i < 5; i++)
            for (unsigned j = 0; j < N_TRAIN_N; j++) {
                RxCmFeatures f = feat(TRAIN_M[i], TRAIN_N[j], RX_CM_CORE_X925, 0);
                for (uint32_t a = 0; a < 3; a++) {
                    RxCmObservation o = { (uint64_t)exp2(truth(a, f.M, f.N, 0) + 0.05 * gauss()), 0 };
                    rx_cm_observe(&m, &f, a, &o);
                }
                RxCmObservation o3 = { (uint64_t)exp2(truth(0, f.M, f.N, 0) - 3.0), 0 };
                rx_cm_observe(&m, &f, 3, &o3);
            }
    uint32_t touched = 0, decisions = 0;
    for (int k = 0; k < 20000; k++) {
        RxCmFeatures f = feat(1 + (uint32_t)(next() % 2000), 1 + (uint32_t)(next() % 4000),
                              (uint32_t)(next() % 3), (uint32_t)(next() % 3));
        f.eligible = 0x7;
        f.latency_budget_ps = (next() & 1) ? (uint64_t)exp2(10 + (double)(next() % 20)) : 0;
        RxCmDecision d;
        rx_cm_decide(&m, &pol, &f, (int)(next() & 1), &d);
        decisions++;
        if (d.arm == 3 || (d.action == RX_CM_MEASURE && (d.measure[0] == 3 || d.measure[1] == 3)))
            touched++;
    }
    CHECK(touched == 0, "an ineligible arm was chosen or measured %u times in %u decisions",
          touched, decisions);

    /* failures exclude an arm; the fallback is never excluded */
    RxCmFeatures f = feat(64, 256, RX_CM_CORE_X925, 0);
    RxCmDecision d;
    rx_cm_decide(&m, &pol, &f, 0, &d);
    uint32_t fast = d.arm;
    CHECK(fast == 2, "before failures, arm 2 wins");
    RxCmObservation bad = { 0, 1 };
    for (int i = 0; i < 3; i++) rx_cm_observe(&m, &f, 2, &bad);
    rx_cm_decide(&m, &pol, &f, 0, &d);
    CHECK(d.arm != 2 && d.pred[2].fail_p > pol.max_fail_p, "a failing arm is no longer selected");
    for (int i = 0; i < 400; i++) rx_cm_observe(&m, &f, 0, &bad);
    f.eligible = 0x1;
    rx_cm_decide(&m, &pol, &f, 1, &d);
    CHECK(d.action == RX_CM_FALLBACK && d.arm == 0, "the fallback stays available");
}

static void t_budgets(void) {
    RxCmPolicy pol;
    rx_cm_default_policy(&pol);
    RxCostModel m;
    rx_cm_init(&m, 2);
    RxCmFeatures f = feat(32, 32, RX_CM_CORE_X925, 0);
    /* arm 0: 2^20 ps, tight. arm 1: 2^19.8 ps, noisy. */
    for (int r = 0; r < 40; r++) {
        RxCmObservation o0 = { (uint64_t)exp2(20.0 + 0.02 * gauss()), 0 };
        RxCmObservation o1 = { (uint64_t)exp2(19.8 + 0.6 * gauss()), 0 };
        rx_cm_observe(&m, &f, 0, &o0);
        rx_cm_observe(&m, &f, 1, &o1);
    }
    RxCmDecision d;
    f.eligible = 0x3;
    rx_cm_decide(&m, &pol, &f, 0, &d);
    uint32_t free_pick = d.arm;
    f.latency_budget_ps = (uint64_t)exp2(20.2);
    rx_cm_decide(&m, &pol, &f, 0, &d);
    CHECK(d.arm == 0 && d.why == RX_CM_WHY_CONFIDENT,
          "latency budget picks the arm whose 90%% bound fits (free pick %u, got %u)", free_pick,
          d.arm);
    f.latency_budget_ps = 1000;
    rx_cm_decide(&m, &pol, &f, 0, &d);
    CHECK(d.action == RX_CM_SELECT && d.why == RX_CM_WHY_OVER_BUDGET, "no arm fits: reported");
    f.latency_budget_ps = 0;
    m.power_mw[RX_CM_CORE_X925][0] = 1000.0;
    m.power_mw[RX_CM_CORE_X925][1] = 5000.0;
    RxCmPrediction p0, p1;
    rx_cm_predict(&m, &f, 0, &p0);
    rx_cm_predict(&m, &f, 1, &p1);
    f.energy_budget_pj = (uint64_t)((p0.energy_pj + p1.energy_pj) / 2.0);
    rx_cm_decide(&m, &pol, &f, 0, &d);
    CHECK(d.arm == 0, "energy budget excludes the hungrier arm");
    CHECK(fabs(p0.energy_pj - exp2(p0.mean_log2_ps) * 1.0) < 1e-6 * p0.energy_pj,
          "energy = predicted ps x measured mW");
}

static void t_calibrate(void) {
    /* Trained on quiet data; later the machine drifts (extra 0.15 log2). */
    RxCostModel m;
    rx_cm_init(&m, 3);
    g_exact = 1;
    train(&m, 0.03, RX_CM_CORE_X925, 0, 2);
    RxCmFeatures cf[600];
    uint32_t ca[600];
    uint64_t cps[600];
    for (int i = 0; i < 600; i++) {
        cf[i] = feat(1 + (uint32_t)(next() % 300), 1 + (uint32_t)(next() % 1000), RX_CM_CORE_X925, 0);
        ca[i] = (uint32_t)(next() % 3);
        cps[i] = (uint64_t)exp2(truth(ca[i], cf[i].M, cf[i].N, 0) + 0.03 * gauss() + 0.15 * gauss());
    }
    uint32_t before = 0, after = 0, n = 0;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            double s = rx_cm_calibrate(&m, cf, ca, cps, 300, 0.80);
            printf("    calibration scale after drift: %.2f\n", s);
            CHECK(s > 1.5, "drift widens the spread (%.2f)", s);
        }
        for (int i = 300; i < 600; i++) {
            RxCmPrediction p;
            rx_cm_predict(&m, &cf[i], ca[i], &p);
            double z = fabs(log2((double)cps[i]) - p.mean_log2_ps);
            int in = z <= rx_cm_t_quantile(0.80, p.dof) * p.sd_log2;
            if (pass == 0) before += in;
            else after += in;
            n += pass == 0;
        }
    }
    g_exact = 0;
    printf("    80%% coverage on later, drifted data: %.3f before calibration, %.3f after\n",
           (double)before / n, (double)after / n);
    CHECK((double)before / n < 0.6, "uncalibrated model is overconfident under drift");
    CHECK((double)after / n > 0.72 && (double)after / n < 0.88, "calibrated coverage %.3f",
          (double)after / n);
    RxCostModel few;
    rx_cm_init(&few, 3);
    CHECK(rx_cm_calibrate(&few, cf, ca, cps, 300, 0.8) == 0.0 && few.sd_scale == 1.0,
          "no usable predictions: scale unchanged");
}

static void t_blob(void) {
    RxCostModel m, r;
    rx_cm_init(&m, 5);
    train(&m, 0.05, RX_CM_CORE_A725, 1, 1);
    m.power_mw[1][2] = 1234.5;
    m.verify_ns[2] = 77;
    m.synth_ns[2] = 88;
    m.code_bytes[2] = 99;
    m.explore_ps = 5;
    m.sd_scale = 1.7;
    size_t n = rx_cm_blob_size(), got = 0;
    printf("    model blob: %zu bytes\n", n);
    CHECK(n <= 65536u, "fits the generation store blob limit");
    uint8_t *a = malloc(n), *b = malloc(n);
    CHECK(rx_cm_serialize(&m, a, n, &got) == RX_CM_OK && got == n, "serialize");
    CHECK(rx_cm_deserialize(&r, a, n) == RX_CM_OK, "deserialize");
    CHECK(r.explore_ps == 0 && r.work_ps == 0, "runtime counters are not durable");
    CHECK(rx_cm_serialize(&r, b, n, &got) == RX_CM_OK && memcmp(a, b, n) == 0,
          "round trip is byte-identical");
    uint8_t d1[32], d2[32];
    rx_cm_digest(&m, d1);
    rx_cm_digest(&r, d2);
    CHECK(memcmp(d1, d2, 32) == 0, "digest survives the round trip");
    RxCmFeatures f = feat(100, 30, RX_CM_CORE_A725, 1);
    RxCmPrediction p, q;
    rx_cm_predict(&m, &f, 1, &p);
    rx_cm_predict(&r, &f, 1, &q);
    CHECK(p.mean_log2_ps == q.mean_log2_ps && p.sd_log2 == q.sd_log2, "same predictions");
    CHECK(r.sd_scale == 1.7, "calibration scale is durable");
    memcpy(b, a, n);
    b[100] ^= 1;
    CHECK(rx_cm_deserialize(&r, b, n) == RX_CM_ERR_DIGEST, "tampered body refused");
    CHECK(rx_cm_deserialize(&r, a, n - 1) == RX_CM_ERR_FORMAT, "truncated blob refused");
    memcpy(b, a, n);
    b[4] = 9;   /* version */
    sha256_hash(b, n - 32, b + n - 32);
    CHECK(rx_cm_deserialize(&r, b, n) == RX_CM_ERR_FORMAT, "unknown version refused");
    free(a);
    free(b);
}

int main(void) {
    double c80, c95, rmse;
    t_basis();
    t_fit_and_calibration(1, &c80, &c95, &rmse);
    t_fit_and_calibration(0, &c80, &c95, &rmse);
    t_decide();
    t_eligibility_property();
    t_budgets();
    t_calibrate();
    t_blob();
    printf("checks %d failures %d\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
