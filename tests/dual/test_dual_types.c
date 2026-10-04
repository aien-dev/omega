/* DUAL-0a contract tests: validation, refusal, canonical encoding, digests,
 * properties (ADR 0031 sections 3, 4, 8.1). Plain C, assert-style: every
 * CHECK counts as one case; any failure exits nonzero. */
#include "rx_dual.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PROP_ROUNDS
#define PROP_ROUNDS 2000
#endif

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_ST(expr, want) do { RxDualStatus s_ = (expr); g_checks++; if (s_ != (want)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s -> %d, want %d\n", __FILE__, __LINE__, #expr, (int)s_, (int)(want)); } } while (0)

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void)
{
    g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1Dull;
}
static double rdbl(double lo, double hi) { return lo + (hi - lo) * ((double)(rnd() >> 11) / 9007199254740992.0); }
static RxDualUnit runit(void) { return (RxDualUnit)(1u + (uint32_t)(rnd() % (RX_DUAL_UNIT_MAX_ - 1u))); }
static void rdig(RxDualDigest *d) { for (int i = 0; i < 32; i++) d->b[i] = (uint8_t)rnd(); }
static RxDualDigest fixed(uint8_t v) { RxDualDigest d; memset(d.b, v, 32); return d; }

/* ---- fixtures ---- */
static RxDualResource mk_resource(uint32_t id, RxDualUnit u)
{
    RxDualResource r;
    memset(&r, 0, sizeof r);
    r.resource_id = id; r.unit = u; r.scale = 1000.0; r.contract = fixed(0x11);
    return r;
}
static RxDualConstraintState mk_constraint(uint32_t id, RxDualUnit u, RxDualClass cls)
{
    RxDualConstraintState s;
    memset(&s, 0, sizeof s);
    s.resource_id = id; s.unit = u; s.cls = cls;
    s.budget = 5000.0; s.budget_contract = fixed(0x22);
    s.estimate_ref = fixed(0x33); s.estimate_kind = RX_DUAL_EST_ESTIMATED;
    s.estimate = 5200.0; s.uncertainty = 100.0;
    s.calibration_ref = fixed(0x44);
    s.lambda = 0.25; s.lambda_state = RX_DUAL_LAMBDA_FRESH;
    s.controller_id = fixed(0x55); s.generation = 7; s.tick = 3;
    s.evidence_root = fixed(0x66); s.parent = fixed(0x77);
    return s;
}
static RxDualController mk_controller(void)
{
    RxDualController c;
    memset(&c, 0, sizeof c);
    c.eta = 0.1; c.rho = 0.01; c.k_sigma = 2.0; c.max_age = 4; c.cadence = 1;
    c.n = 3;
    c.resource_id[0] = 1; c.resource_id[1] = 2; c.resource_id[2] = 9;
    c.lambda_max[0] = 10.0; c.lambda_max[1] = 10.0; c.lambda_max[2] = 1.0;
    return c;
}
static RxDualPriceVector mk_pv(void)
{
    RxDualPriceVector v;
    memset(&v, 0, sizeof v);
    v.generation = 7; v.context = fixed(0x88); v.n = 2;
    v.resource_id[0] = 1; v.resource_id[1] = 2;
    v.state[0] = fixed(0x91); v.state[1] = fixed(0x92);
    return v;
}
static RxDualSite mk_site(void)
{
    RxDualSite s;
    memset(&s, 0, sizeof s);
    s.site_id = 1; memcpy(s.name, "jspace.residency", 16); s.owner = fixed(0xA1); s.max_alternatives = 5;
    return s;
}
static RxDualRecommendation mk_rec(void)
{
    RxDualRecommendation r;
    memset(&r, 0, sizeof r);
    r.decision_site = fixed(0xB1); r.price_vector = fixed(0xB2); r.generation = 7;
    r.n_candidates = 3; r.candidates[0] = fixed(0xC1); r.candidates[1] = fixed(0xC2); r.candidates[2] = fixed(0xC3);
    r.actual = 0; r.recommended = 2; r.n_consequences = 2;
    rx_dual_consequence_set(&r.c[0], 1, RX_DUAL_UNIT_PS, 100.0, 10.0, 1, 112.0);
    rx_dual_consequence_set(&r.c[1], 2, RX_DUAL_UNIT_PJ, 50.0, 5.0, 0, 0.0);
    r.authority = RX_DUAL_AUTHORITY_NONE;
    return r;
}
static RxDualOutcome mk_outcome(void)
{
    RxDualOutcome o;
    memset(&o, 0, sizeof o);
    o.recommendation = fixed(0xD1); o.before_vector = fixed(0xD2); o.after_vector = fixed(0xD3);
    o.target_resource_id = 1; o.generation_before = 7; o.generation_after = 9;
    o.lambda_before = 0.5; o.lambda_after = 0.3;
    return o;
}

/* ---- round trip + digest determinism, per record kind ---- */
#define ROUNDTRIP(T, enc, dec, dig, obj, kindbyte)                                              \
    do {                                                                                        \
        uint8_t b1[RX_DUAL_ENCODED_MAX], b2[RX_DUAL_ENCODED_MAX];                               \
        size_t l1 = 0, l2 = 0; T back; RxDualDigest d1, d2, d3;                                 \
        CHECK_ST(enc(&(obj), b1, sizeof b1, &l1), RX_DUAL_OK);                                  \
        CHECK(l1 > 2 && b1[0] == (kindbyte) && b1[1] == RX_DUAL_FORMAT_VERSION);                \
        CHECK_ST(dec(b1, l1, &back), RX_DUAL_OK);                                               \
        CHECK_ST(enc(&back, b2, sizeof b2, &l2), RX_DUAL_OK);                                   \
        CHECK(l1 == l2 && memcmp(b1, b2, l1) == 0);                                             \
        CHECK_ST(dig(&(obj), &d1), RX_DUAL_OK);                                                 \
        CHECK_ST(dig(&back, &d2), RX_DUAL_OK);                                                  \
        CHECK_ST(dig(&(obj), &d3), RX_DUAL_OK);                                                 \
        CHECK(rx_dual_digest_eq(&d1, &d2) && rx_dual_digest_eq(&d1, &d3));                     \
        CHECK_ST(dec(b1, l1 - 1, &back), RX_DUAL_ERR_ENCODING);        /* truncated */          \
        CHECK_ST(dec(b1, l1 + 1, &back), RX_DUAL_ERR_ENCODING);        /* trailing byte */      \
        CHECK_ST(dec(b1, 1, &back), RX_DUAL_ERR_ENCODING);                                      \
        b1[1] = 2; CHECK_ST(dec(b1, l1, &back), RX_DUAL_ERR_ENCODING); b1[1] = 1; /* version */ \
        b1[0] = 99; CHECK_ST(dec(b1, l1, &back), RX_DUAL_ERR_KIND); b1[0] = (kindbyte);         \
        CHECK_ST(enc(&(obj), b1, 4, &l1), RX_DUAL_ERR_ENCODING);        /* capacity */          \
        CHECK_ST(enc(&(obj), NULL, 0, &l1), RX_DUAL_ERR_NULL);                                  \
        CHECK_ST(dec(NULL, l1, &back), RX_DUAL_ERR_NULL);                                       \
    } while (0)

static void test_roundtrips(void)
{
    RxDualResource r = mk_resource(1, RX_DUAL_UNIT_PS);
    RxDualConstraintState s = mk_constraint(1, RX_DUAL_UNIT_PS, RX_DUAL_CLASS_SOFT);
    RxDualController c = mk_controller();
    RxDualPriceVector v = mk_pv();
    RxDualSite site = mk_site();
    RxDualRecommendation rec = mk_rec();
    RxDualOutcome o = mk_outcome();
    ROUNDTRIP(RxDualResource, rx_dual_encode_resource, rx_dual_decode_resource, rx_dual_digest_resource, r, RX_DUAL_KIND_RESOURCE);
    ROUNDTRIP(RxDualConstraintState, rx_dual_encode_constraint, rx_dual_decode_constraint, rx_dual_digest_constraint, s, RX_DUAL_KIND_CONSTRAINT);
    ROUNDTRIP(RxDualController, rx_dual_encode_controller, rx_dual_decode_controller, rx_dual_digest_controller, c, RX_DUAL_KIND_CONTROLLER);
    ROUNDTRIP(RxDualPriceVector, rx_dual_encode_price_vector, rx_dual_decode_price_vector, rx_dual_digest_price_vector, v, RX_DUAL_KIND_PRICE_VECTOR);
    ROUNDTRIP(RxDualSite, rx_dual_encode_site, rx_dual_decode_site, rx_dual_digest_site, site, RX_DUAL_KIND_SITE);
    ROUNDTRIP(RxDualRecommendation, rx_dual_encode_recommendation, rx_dual_decode_recommendation, rx_dual_digest_recommendation, rec, RX_DUAL_KIND_RECOMMENDATION);
    ROUNDTRIP(RxDualOutcome, rx_dual_encode_outcome, rx_dual_decode_outcome, rx_dual_digest_outcome, o, RX_DUAL_KIND_OUTCOME);
}

/* ---- kind confusion and digest-domain separation ---- */
static void test_kind_confusion(void)
{
    uint8_t buf[RX_DUAL_ENCODED_MAX]; size_t len = 0;
    RxDualResource r = mk_resource(1, RX_DUAL_UNIT_PS);
    RxDualConstraintState s = mk_constraint(1, RX_DUAL_UNIT_PS, RX_DUAL_CLASS_SOFT);
    RxDualConstraintState sback; RxDualResource rback; RxDualOutcome oback;
    RxDualDigest d_res, d_con, d_pv, d_site;
    RxDualPriceVector v = mk_pv();
    RxDualSite site = mk_site();

    CHECK_ST(rx_dual_encode_resource(&r, buf, sizeof buf, &len), RX_DUAL_OK);
    CHECK_ST(rx_dual_decode_constraint(buf, len, &sback), RX_DUAL_ERR_KIND);
    CHECK_ST(rx_dual_decode_outcome(buf, len, &oback), RX_DUAL_ERR_KIND);
    CHECK_ST(rx_dual_encode_constraint(&s, buf, sizeof buf, &len), RX_DUAL_OK);
    CHECK_ST(rx_dual_decode_resource(buf, len, &rback), RX_DUAL_ERR_KIND);
    /* Same payload bytes under another kind byte must not decode as that kind
     * (length and validation differ), and digests of distinct kinds differ. */
    buf[0] = (uint8_t)RX_DUAL_KIND_RESOURCE;
    CHECK(rx_dual_decode_resource(buf, len, &rback) != RX_DUAL_OK);
    CHECK_ST(rx_dual_digest_resource(&r, &d_res), RX_DUAL_OK);
    CHECK_ST(rx_dual_digest_constraint(&s, &d_con), RX_DUAL_OK);
    CHECK_ST(rx_dual_digest_price_vector(&v, &d_pv), RX_DUAL_OK);
    CHECK_ST(rx_dual_digest_site(&site, &d_site), RX_DUAL_OK);
    CHECK(!rx_dual_digest_eq(&d_res, &d_con) && !rx_dual_digest_eq(&d_pv, &d_site) && !rx_dual_digest_eq(&d_res, &d_pv));
}

/* ---- hostile resource / registry ---- */
static void test_resource_registry(void)
{
    RxDualRegistry reg; RxDualResource r = mk_resource(5, RX_DUAL_UNIT_BYTES);
    RxDualDigest d;
    rx_dual_registry_init(&reg);
    CHECK_ST(rx_dual_registry_add(&reg, &r), RX_DUAL_OK);
    CHECK_ST(rx_dual_registry_add(&reg, &r), RX_DUAL_ERR_RESOURCE);        /* duplicate id */
    r.resource_id = 2; CHECK_ST(rx_dual_registry_add(&reg, &r), RX_DUAL_OK);
    r.resource_id = 9; CHECK_ST(rx_dual_registry_add(&reg, &r), RX_DUAL_OK);
    CHECK(reg.n == 3 && reg.r[0].resource_id == 2 && reg.r[1].resource_id == 5 && reg.r[2].resource_id == 9);
    CHECK(rx_dual_registry_find(&reg, 5) != NULL && rx_dual_registry_find(&reg, 7) == NULL);
    r.resource_id = 3; r.scale = -1.0;  CHECK_ST(rx_dual_registry_add(&reg, &r), RX_DUAL_ERR_SCALE);     /* negative scale */
    r.scale = 0.0;                      CHECK_ST(rx_dual_registry_add(&reg, &r), RX_DUAL_ERR_SCALE);     /* zero scale */
    r.scale = NAN;                      CHECK_ST(rx_dual_check_resource(&r), RX_DUAL_ERR_NONFINITE);
    r.scale = INFINITY;                 CHECK_ST(rx_dual_check_resource(&r), RX_DUAL_ERR_NONFINITE);
    r.scale = -INFINITY;                CHECK_ST(rx_dual_check_resource(&r), RX_DUAL_ERR_NONFINITE);
    r.scale = 1.0; r.unit = RX_DUAL_UNIT_NONE; CHECK_ST(rx_dual_check_resource(&r), RX_DUAL_ERR_UNIT);
    r.unit = (RxDualUnit)RX_DUAL_UNIT_MAX_;    CHECK_ST(rx_dual_check_resource(&r), RX_DUAL_ERR_UNIT);
    r.unit = RX_DUAL_UNIT_KV_BLOCKS; memset(&r.contract, 0, 32); CHECK_ST(rx_dual_digest_resource(&r, &d), RX_DUAL_ERR_DIGEST);
    CHECK_ST(rx_dual_check_resource(NULL), RX_DUAL_ERR_NULL);
    /* capacity */
    rx_dual_registry_init(&reg);
    for (uint32_t i = 0; i < RX_DUAL_MAX_RESOURCES; i++) { RxDualResource x = mk_resource(100 + i, RX_DUAL_UNIT_PS); CHECK_ST(rx_dual_registry_add(&reg, &x), RX_DUAL_OK); }
    { RxDualResource x = mk_resource(999, RX_DUAL_UNIT_PS); CHECK_ST(rx_dual_registry_add(&reg, &x), RX_DUAL_ERR_FULL); }
}

/* ---- hostile constraint ---- */
static void test_constraint_rules(void)
{
    RxDualConstraintState s = mk_constraint(1, RX_DUAL_UNIT_PS, RX_DUAL_CLASS_SOFT);
    RxDualResource r = mk_resource(1, RX_DUAL_UNIT_PS);
    RxDualDigest d;
    CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_OK);
    CHECK_ST(rx_dual_check_constraint_against(&s, &r), RX_DUAL_OK);
    s.cls = RX_DUAL_CLASS_INVARIANT;  CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_ERR_CLASS); /* INVARIANT unconstructible */
    CHECK_ST(rx_dual_digest_constraint(&s, &d), RX_DUAL_ERR_CLASS);
    s.cls = RX_DUAL_CLASS_UNDECLARED; CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_ERR_CLASS); /* never defaulted */
    s.cls = (RxDualClass)RX_DUAL_CLASS_MAX_; CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_ERR_CLASS);
    s.cls = RX_DUAL_CLASS_CAPACITY;   CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_OK);       /* diagnostic price allowed */
    s.cls = RX_DUAL_CLASS_SOFT;
    s.lambda = -0.1;      CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_ERR_RANGE);
    s.lambda = NAN;       CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_ERR_NONFINITE);
    s.lambda = INFINITY;  CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_ERR_NONFINITE);
    s.lambda = 0.25;
    s.uncertainty = -1.0; CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_ERR_RANGE);
    s.uncertainty = -INFINITY; CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_ERR_NONFINITE);
    s.uncertainty = 100.0;
    s.estimate = NAN;     CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_ERR_NONFINITE); s.estimate = 5200.0;
    s.budget = INFINITY;  CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_ERR_NONFINITE); s.budget = 5000.0;
    s.unit = RX_DUAL_UNIT_NONE; CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_ERR_UNIT); s.unit = RX_DUAL_UNIT_PS;
    /* unit mismatch against the registry: refused, never converted */
    r.unit = RX_DUAL_UNIT_NS; CHECK_ST(rx_dual_check_constraint_against(&s, &r), RX_DUAL_ERR_UNIT); r.unit = RX_DUAL_UNIT_PS;
    r.resource_id = 2;        CHECK_ST(rx_dual_check_constraint_against(&s, &r), RX_DUAL_ERR_RESOURCE); r.resource_id = 1;
    /* kinds */
    s.estimate_kind = (RxDualEstimateKind)0; CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_ERR_KIND);
    s.estimate_kind = RX_DUAL_EST_MEASURED;  CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_ERR_DIGEST); /* measured needs observation_ref */
    s.observation_ref = fixed(0x12);         CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_OK);
    s.estimate_kind = RX_DUAL_EST_ESTIMATED;
    /* lambda state */
    s.lambda_state = (RxDualLambdaState)0;   CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_ERR_RANGE);
    s.lambda_state = (RxDualLambdaState)RX_DUAL_LAMBDA_MAX_; CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_ERR_RANGE);
    s.lambda_state = RX_DUAL_LAMBDA_FRESH;
    /* FRESH requires calibration; UNCALIBRATED may be recorded */
    memset(&s.calibration_ref, 0, 32);       CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_ERR_CALIBRATION);
    s.lambda_state = RX_DUAL_LAMBDA_UNCALIBRATED; CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_OK);
    s.lambda_state = RX_DUAL_LAMBDA_STALE;   CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_OK);
    s.calibration_ref = fixed(0x44); s.lambda_state = RX_DUAL_LAMBDA_FRESH;
    /* required digests */
    memset(&s.estimate_ref, 0, 32);    CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_ERR_DIGEST); s.estimate_ref = fixed(0x33);
    memset(&s.controller_id, 0, 32);   CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_ERR_DIGEST); s.controller_id = fixed(0x55);
    memset(&s.evidence_root, 0, 32);   CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_ERR_DIGEST); s.evidence_root = fixed(0x66);
    memset(&s.budget_contract, 0, 32); CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_ERR_DIGEST); s.budget_contract = fixed(0x22);
    /* parent / tick linkage: broken parent refused both ways */
    memset(&s.parent, 0, 32);          CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_ERR_DIGEST);
    s.tick = 0;                        CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_OK);
    s.parent = fixed(0x77);            CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_ERR_DIGEST);
    s.tick = 3;                        CHECK_ST(rx_dual_check_constraint(&s), RX_DUAL_OK);
    /* a decoder refuses what the checker refuses: craft bytes with class INVARIANT */
    {
        uint8_t buf[RX_DUAL_ENCODED_MAX]; size_t len = 0; RxDualConstraintState back;
        CHECK_ST(rx_dual_encode_constraint(&s, buf, sizeof buf, &len), RX_DUAL_OK);
        buf[2 + 4 + 4] = (uint8_t)RX_DUAL_CLASS_INVARIANT;   /* class field follows id and unit */
        CHECK_ST(rx_dual_decode_constraint(buf, len, &back), RX_DUAL_ERR_CLASS);
        buf[2 + 4 + 4] = (uint8_t)RX_DUAL_CLASS_UNDECLARED;
        CHECK_ST(rx_dual_decode_constraint(buf, len, &back), RX_DUAL_ERR_CLASS);
    }
    /* -0.0 canonicalizes to +0.0: same digest */
    {
        RxDualConstraintState a = mk_constraint(1, RX_DUAL_UNIT_PS, RX_DUAL_CLASS_SOFT), b = a; RxDualDigest da, db;
        a.lambda = 0.0; b.lambda = -0.0;
        CHECK_ST(rx_dual_digest_constraint(&a, &da), RX_DUAL_OK);
        CHECK_ST(rx_dual_digest_constraint(&b, &db), RX_DUAL_OK);
        CHECK(rx_dual_digest_eq(&da, &db));
    }
}

/* ---- controller ---- */
static void test_controller(void)
{
    RxDualController c = mk_controller(); RxDualDigest d1, d2; double lm = 0;
    CHECK_ST(rx_dual_check_controller(&c), RX_DUAL_OK);
    CHECK_ST(rx_dual_digest_controller(&c, &d1), RX_DUAL_OK);
    CHECK_ST(rx_dual_controller_lambda_max(&c, 9, &lm), RX_DUAL_OK); CHECK(lm == 1.0);
    CHECK_ST(rx_dual_controller_lambda_max(&c, 4, &lm), RX_DUAL_ERR_RESOURCE); /* unknown resource */
    c.eta = 0.1000001; CHECK_ST(rx_dual_digest_controller(&c, &d2), RX_DUAL_OK);
    CHECK(!rx_dual_digest_eq(&d1, &d2));                      /* parameter mutation = new controller */
    c.eta = 0.1;
    c.eta = -0.1;      CHECK_ST(rx_dual_check_controller(&c), RX_DUAL_ERR_RANGE); c.eta = 0.1;
    c.eta = NAN;       CHECK_ST(rx_dual_check_controller(&c), RX_DUAL_ERR_NONFINITE); c.eta = 0.1;
    c.rho = 1.5;       CHECK_ST(rx_dual_check_controller(&c), RX_DUAL_ERR_RANGE); c.rho = 0.01;
    c.k_sigma = -1.0;  CHECK_ST(rx_dual_check_controller(&c), RX_DUAL_ERR_RANGE); c.k_sigma = 2.0;
    c.max_age = 0;     CHECK_ST(rx_dual_check_controller(&c), RX_DUAL_ERR_RANGE); c.max_age = 4;
    c.cadence = 0;     CHECK_ST(rx_dual_check_controller(&c), RX_DUAL_ERR_RANGE); c.cadence = 1;
    c.lambda_max[1] = 0.0; CHECK_ST(rx_dual_check_controller(&c), RX_DUAL_ERR_RANGE);
    c.lambda_max[1] = INFINITY; CHECK_ST(rx_dual_check_controller(&c), RX_DUAL_ERR_NONFINITE); c.lambda_max[1] = 10.0;
    c.resource_id[1] = 1; CHECK_ST(rx_dual_check_controller(&c), RX_DUAL_ERR_RESOURCE); /* duplicate */
    c.resource_id[1] = 0; CHECK_ST(rx_dual_check_controller(&c), RX_DUAL_ERR_RESOURCE); /* unordered */
    c.resource_id[1] = 2;
    c.n = 0; CHECK_ST(rx_dual_check_controller(&c), RX_DUAL_ERR_RANGE);
    c.n = RX_DUAL_MAX_RESOURCES + 1; CHECK_ST(rx_dual_check_controller(&c), RX_DUAL_ERR_RANGE);
    c.n = 3;
    /* malformed controller bytes: n above capacity is refused before any read */
    {
        uint8_t buf[RX_DUAL_ENCODED_MAX]; size_t len = 0; RxDualController back;
        CHECK_ST(rx_dual_encode_controller(&c, buf, sizeof buf, &len), RX_DUAL_OK);
        buf[2 + 8 * 5] = 0xFF; buf[2 + 8 * 5 + 1] = 0xFF;
        CHECK_ST(rx_dual_decode_controller(buf, len, &back), RX_DUAL_ERR_RANGE);
    }
}

/* ---- price vector ---- */
static void test_price_vector(void)
{
    RxDualPriceVector v = mk_pv(); RxDualDigest d;
    RxDualConstraintState a = mk_constraint(1, RX_DUAL_UNIT_PS, RX_DUAL_CLASS_SOFT);
    RxDualConstraintState b = mk_constraint(2, RX_DUAL_UNIT_PJ, RX_DUAL_CLASS_CAPACITY);
    RxDualConstraintState c = mk_constraint(1, RX_DUAL_UNIT_PJ, RX_DUAL_CLASS_SOFT);
    const RxDualConstraintState *ok[2] = { &a, &b }, *dup[2] = { &a, &c }, *rev[2] = { &b, &a };
    RxDualPriceVector built; RxDualDigest ctx = fixed(0x88);
    CHECK_ST(rx_dual_check_price_vector(&v), RX_DUAL_OK);
    v.resource_id[1] = 1; CHECK_ST(rx_dual_check_price_vector(&v), RX_DUAL_ERR_RESOURCE);   /* duplicate */
    v.resource_id[1] = 0; CHECK_ST(rx_dual_check_price_vector(&v), RX_DUAL_ERR_RESOURCE);   /* unordered */
    v.resource_id[1] = 2;
    memset(&v.state[1], 0, 32); CHECK_ST(rx_dual_check_price_vector(&v), RX_DUAL_ERR_DIGEST); v.state[1] = fixed(0x92);
    memset(&v.context, 0, 32);  CHECK_ST(rx_dual_check_price_vector(&v), RX_DUAL_ERR_DIGEST); v.context = fixed(0x88);
    v.n = 0; CHECK_ST(rx_dual_check_price_vector(&v), RX_DUAL_ERR_RANGE); v.n = 2;
    CHECK_ST(rx_dual_price_vector_build(ok, 2, &ctx, &built), RX_DUAL_OK);
    CHECK(built.n == 2 && built.generation == 7 && built.resource_id[1] == 2);
    CHECK_ST(rx_dual_digest_constraint(&b, &d), RX_DUAL_OK); CHECK(rx_dual_digest_eq(&d, &built.state[1]));
    CHECK_ST(rx_dual_price_vector_build(dup, 2, &ctx, &built), RX_DUAL_ERR_RESOURCE);      /* duplicate resource */
    CHECK_ST(rx_dual_price_vector_build(rev, 2, &ctx, &built), RX_DUAL_ERR_RESOURCE);      /* noncanonical order */
    b.generation = 8; CHECK_ST(rx_dual_price_vector_build(ok, 2, &ctx, &built), RX_DUAL_ERR_GENERATION); b.generation = 7;
    b.cls = RX_DUAL_CLASS_INVARIANT; CHECK_ST(rx_dual_price_vector_build(ok, 2, &ctx, &built), RX_DUAL_ERR_CLASS); b.cls = RX_DUAL_CLASS_CAPACITY;
    memset(&ctx, 0, 32); CHECK_ST(rx_dual_price_vector_build(ok, 2, &ctx, &built), RX_DUAL_ERR_DIGEST);
}

/* ---- site, recommendation, outcome ---- */
static void test_site_rec_outcome(void)
{
    RxDualSite s = mk_site(); RxDualRecommendation r = mk_rec(); RxDualOutcome o = mk_outcome();
    RxDualConsequence c;
    CHECK_ST(rx_dual_check_site(&s), RX_DUAL_OK);
    s.name[16] = 'x';           CHECK_ST(rx_dual_check_site(&s), RX_DUAL_OK);   /* longer name still canonical */
    s.name[20] = 'y';           CHECK_ST(rx_dual_check_site(&s), RX_DUAL_ERR_SITE); /* embedded NUL then data */
    s = mk_site(); s.name[3] = ' '; CHECK_ST(rx_dual_check_site(&s), RX_DUAL_ERR_SITE); /* non-printable */
    s = mk_site(); s.name[0] = 0;   CHECK_ST(rx_dual_check_site(&s), RX_DUAL_ERR_SITE);
    s = mk_site(); memset(s.name, 'a', 32); CHECK_ST(rx_dual_check_site(&s), RX_DUAL_ERR_SITE); /* unterminated */
    s = mk_site(); s.max_alternatives = 0; CHECK_ST(rx_dual_check_site(&s), RX_DUAL_ERR_RANGE);
    s = mk_site(); memset(&s.owner, 0, 32); CHECK_ST(rx_dual_check_site(&s), RX_DUAL_ERR_DIGEST);

    CHECK_ST(rx_dual_check_recommendation(&r), RX_DUAL_OK);
    CHECK(r.c[0].error == 1.2);
    r.authority = 1;            CHECK_ST(rx_dual_check_recommendation(&r), RX_DUAL_ERR_AUTHORITY); r.authority = 0;
    r.actual = 3;               CHECK_ST(rx_dual_check_recommendation(&r), RX_DUAL_ERR_RANGE); r.actual = 0;
    r.recommended = 99;         CHECK_ST(rx_dual_check_recommendation(&r), RX_DUAL_ERR_RANGE); r.recommended = 2;
    r.n_candidates = 0;         CHECK_ST(rx_dual_check_recommendation(&r), RX_DUAL_ERR_RANGE); r.n_candidates = 3;
    memset(&r.candidates[1], 0, 32); CHECK_ST(rx_dual_check_recommendation(&r), RX_DUAL_ERR_DIGEST); r.candidates[1] = fixed(0xC2);
    memset(&r.price_vector, 0, 32);  CHECK_ST(rx_dual_check_recommendation(&r), RX_DUAL_ERR_DIGEST); r.price_vector = fixed(0xB2);
    memset(&r.decision_site, 0, 32); CHECK_ST(rx_dual_check_recommendation(&r), RX_DUAL_ERR_DIGEST); r.decision_site = fixed(0xB1);
    r.c[1].resource_id = 1;     CHECK_ST(rx_dual_check_recommendation(&r), RX_DUAL_ERR_RESOURCE); r.c[1].resource_id = 2;
    r.c[0].error = 1.3;         CHECK_ST(rx_dual_check_recommendation(&r), RX_DUAL_ERR_RANGE);   /* lying error field */
    r.c[0].error = 1.2;
    r.c[1].measured = 1.0;      CHECK_ST(rx_dual_check_recommendation(&r), RX_DUAL_ERR_RANGE);   /* unmeasured with a value */
    r.c[1].measured = 0.0;
    r.c[0].predicted_sd = 0.0;  CHECK_ST(rx_dual_check_recommendation(&r), RX_DUAL_ERR_RANGE);   /* measured needs sd > 0 */
    r.c[0].predicted_sd = 10.0;
    r.c[0].unit = RX_DUAL_UNIT_NONE; CHECK_ST(rx_dual_check_recommendation(&r), RX_DUAL_ERR_UNIT); r.c[0].unit = RX_DUAL_UNIT_PS;
    r.c[0].predicted = NAN;     CHECK_ST(rx_dual_check_recommendation(&r), RX_DUAL_ERR_NONFINITE); r.c[0].predicted = 100.0;
    /* authority byte in the encoding is refused by the decoder too */
    {
        uint8_t buf[RX_DUAL_ENCODED_MAX]; size_t len = 0; RxDualRecommendation back;
        CHECK_ST(rx_dual_encode_recommendation(&r, buf, sizeof buf, &len), RX_DUAL_OK);
        buf[len - 4] = 1;
        CHECK_ST(rx_dual_decode_recommendation(buf, len, &back), RX_DUAL_ERR_AUTHORITY);
    }
    CHECK_ST(rx_dual_consequence_set(&c, 1, RX_DUAL_UNIT_PS, 1.0, 0.0, 1, 2.0), RX_DUAL_ERR_RANGE);
    CHECK_ST(rx_dual_consequence_set(&c, 1, RX_DUAL_UNIT_PS, 1.0, NAN, 0, 0.0), RX_DUAL_ERR_NONFINITE);
    CHECK_ST(rx_dual_consequence_set(&c, 1, RX_DUAL_UNIT_PS, 1.0, 2.0, 1, INFINITY), RX_DUAL_ERR_NONFINITE);
    CHECK_ST(rx_dual_consequence_set(&c, 1, RX_DUAL_UNIT_PS, 1.0, 2.0, 0, 0.0), RX_DUAL_OK);

    CHECK_ST(rx_dual_check_outcome(&o), RX_DUAL_OK);
    o.generation_after = 6;     CHECK_ST(rx_dual_check_outcome(&o), RX_DUAL_ERR_GENERATION); o.generation_after = 9;
    o.lambda_after = -1.0;      CHECK_ST(rx_dual_check_outcome(&o), RX_DUAL_ERR_RANGE); o.lambda_after = 0.3;
    o.lambda_before = NAN;      CHECK_ST(rx_dual_check_outcome(&o), RX_DUAL_ERR_NONFINITE); o.lambda_before = 0.5;
    memset(&o.after_vector, 0, 32); CHECK_ST(rx_dual_check_outcome(&o), RX_DUAL_ERR_DIGEST);
}

/* ---- properties: random valid records round-trip byte-identically; any
 * single-byte mutation of the encoding either fails to decode or yields a
 * different digest (no two distinct encodings share an identity). ---- */
static void test_properties(void)
{
    for (int round = 0; round < PROP_ROUNDS; round++) {
        RxDualConstraintState s = mk_constraint(1u + (uint32_t)(rnd() % 1000u), runit(),
                                                (rnd() & 1) ? RX_DUAL_CLASS_SOFT : RX_DUAL_CLASS_CAPACITY);
        uint8_t b1[RX_DUAL_ENCODED_MAX], b2[RX_DUAL_ENCODED_MAX]; size_t l1 = 0, l2 = 0;
        RxDualConstraintState back; RxDualDigest d1, d2;
        s.budget = rdbl(-1e6, 1e6); s.estimate = rdbl(-1e6, 1e6); s.uncertainty = rdbl(0, 1e3);
        s.lambda = rdbl(0, 10); s.generation = rnd(); s.tick = 1 + (rnd() % 1000);
        rdig(&s.estimate_ref); rdig(&s.parent); rdig(&s.evidence_root); rdig(&s.controller_id); rdig(&s.budget_contract);
        s.estimate_kind = (RxDualEstimateKind)(1u + (uint32_t)(rnd() % 3u));
        if (s.estimate_kind == RX_DUAL_EST_MEASURED) rdig(&s.observation_ref);
        s.lambda_state = (RxDualLambdaState)(1u + (uint32_t)(rnd() % 5u));
        if (rnd() % 4 == 0) { memset(&s.calibration_ref, 0, 32); if (s.lambda_state == RX_DUAL_LAMBDA_FRESH) s.lambda_state = RX_DUAL_LAMBDA_UNCALIBRATED; }
        else rdig(&s.calibration_ref);
        CHECK_ST(rx_dual_encode_constraint(&s, b1, sizeof b1, &l1), RX_DUAL_OK);
        CHECK_ST(rx_dual_decode_constraint(b1, l1, &back), RX_DUAL_OK);
        CHECK_ST(rx_dual_encode_constraint(&back, b2, sizeof b2, &l2), RX_DUAL_OK);
        CHECK(l1 == l2 && memcmp(b1, b2, l1) == 0);
        CHECK_ST(rx_dual_digest_constraint(&s, &d1), RX_DUAL_OK);
        {
            size_t pos = 2 + (size_t)(rnd() % (l1 - 2));
            uint8_t saved = b1[pos];
            b1[pos] ^= (uint8_t)(1u << (rnd() % 8));
            if (rx_dual_decode_constraint(b1, l1, &back) == RX_DUAL_OK) {
                CHECK_ST(rx_dual_digest_constraint(&back, &d2), RX_DUAL_OK);
                CHECK(!rx_dual_digest_eq(&d1, &d2));
            }
            b1[pos] = saved;
        }
    }
}

int main(void)
{
    test_roundtrips();
    test_kind_confusion();
    test_resource_registry();
    test_constraint_rules();
    test_controller();
    test_price_vector();
    test_site_rec_outcome();
    test_properties();
    printf("test_dual_types: %d checks, %d failures: %s\n", g_checks, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
