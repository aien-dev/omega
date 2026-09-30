/* EST-0 contract tests: validation, refusal, encoding, digests, properties.
 * Plain C, assert-style: every CHECK counts as one test case; any failure
 * makes the program exit nonzero. */
#include "est_types.h"
#include "sha256.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_ST(expr, want) do { est_status s_ = (expr); g_checks++; if (s_ != (want)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s -> %d, want %d\n", __FILE__, __LINE__, #expr, (int)s_, (int)(want)); } } while (0)

#define BUF 2400u

/* ---- deterministic xorshift64* ---- */
static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void)
{
    g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1Dull;
}
static double rdbl(double lo, double hi) { return lo + (hi - lo) * ((double)(rnd() >> 11) / 9007199254740992.0); }
static uint32_t rdim(void) { return 1u + (uint32_t)(rnd() % EST_MAX_DIM); }
static est_unit runit(void) { return (est_unit)(1u + (uint32_t)(rnd() % 8u)); }
static void rdig(est_digest *d) { for (int i = 0; i < 32; i++) d->b[i] = (uint8_t)rnd(); }

/* Diagonally dominant symmetric matrix: valid, and stays valid under small
 * symmetric perturbations of one off-diagonal pair. */
static void gen_cov(double *A, uint32_t n)
{
    memset(A, 0, sizeof(double) * EST_MAX_DIM * EST_MAX_DIM);
    for (uint32_t i = 0; i < n; i++) {
        A[i * n + i] = rdbl(1.0, 2.0);
        for (uint32_t j = 0; j < i; j++) { double v = rdbl(-0.05, 0.05); A[i * n + j] = v; A[j * n + i] = v; }
    }
}
static void gen_model(est_model *o)
{
    memset(o, 0, sizeof *o);
    o->n = rdim(); o->m = rdim();
    o->estimator = EST_ESTIMATOR_LINEAR_KALMAN; o->meaning = EST_UNCERTAINTY_GAUSSIAN_COVARIANCE;
    for (uint32_t i = 0; i < o->n; i++) o->state_unit[i] = runit();
    for (uint32_t i = 0; i < o->m; i++) o->obs_unit[i] = runit();
    for (uint32_t i = 0; i < o->n * o->n; i++) o->F[i] = rdbl(-2, 2);
    for (uint32_t i = 0; i < o->m * o->n; i++) o->H[i] = rdbl(-2, 2);
    gen_cov(o->Q, o->n); gen_cov(o->R, o->m);
    o->step_ns = 1 + (int64_t)(rnd() % 1000000000u);
}
static void gen_obs(est_observation *o)
{
    memset(o, 0, sizeof *o);
    o->m = rdim();
    for (uint32_t i = 0; i < o->m; i++) { o->unit[i] = runit(); o->z[i] = rdbl(-100, 100); }
    gen_cov(o->R, o->m);
    o->t_ns = (int64_t)(rnd() >> 2); o->seq = rnd();
    rdig(&o->source); rdig(&o->evidence); o->evidence.b[0] |= 1;
}
static void gen_belief(est_belief *o)
{
    memset(o, 0, sizeof *o);
    o->n = rdim();
    for (uint32_t i = 0; i < o->n; i++) { o->unit[i] = runit(); o->x[i] = rdbl(-100, 100); }
    gen_cov(o->P, o->n);
    o->generation = rnd(); o->t_ns = (int64_t)(rnd() >> 2);
    rdig(&o->model); rdig(&o->parent); rdig(&o->evidence_root);
}
static void gen_pred(est_prediction *o)
{
    memset(o, 0, sizeof *o);
    o->n = rdim(); o->m = rdim();
    rdig(&o->prior); rdig(&o->model);
    o->horizon = 1u + (uint32_t)(rnd() % 1000u); o->generation = rnd(); o->t_ns = (int64_t)(rnd() >> 2);
    for (uint32_t i = 0; i < o->n; i++) o->x[i] = rdbl(-100, 100);
    for (uint32_t i = 0; i < o->m; i++) o->y_mean[i] = rdbl(-100, 100);
    gen_cov(o->P, o->n); gen_cov(o->S, o->m);
}
static void gen_innov(est_innovation *o)
{
    memset(o, 0, sizeof *o);
    o->m = rdim();
    rdig(&o->prediction); rdig(&o->observation); rdig(&o->model);
    for (uint32_t i = 0; i < o->m; i++) o->nu[i] = rdbl(-100, 100);
    gen_cov(o->S, o->m);
    o->nis = rdbl(0, 50); o->t_ns = (int64_t)(rnd() >> 2);
}

/* ---- single-field mutators: each returns 1 while idx names a field ---- */
static est_unit bump_unit(est_unit u) { return u == EST_UNIT_WATT ? EST_UNIT_BYTE : EST_UNIT_WATT; }
static void bump_cov(double *A, uint32_t n, uint32_t i, uint32_t j)
{
    if (i == j) A[i * n + i] += 1.0;
    else { A[i * n + j] += 0.01; A[j * n + i] += 0.01; }
}
#define FIELD(stmt) do { if (k++ == idx) { *o = *b; stmt; return 1; } } while (0)
#define DIGF(d) do { FIELD(o->d.b[0] ^= 1); FIELD(o->d.b[31] ^= 0x80); } while (0)

static int mut_model(const est_model *b, est_model *o, int idx)
{
    int k = 0; uint32_t n = b->n, m = b->m;
    for (uint32_t i = 0; i < n; i++) FIELD(o->state_unit[i] = bump_unit(o->state_unit[i]));
    for (uint32_t i = 0; i < m; i++) FIELD(o->obs_unit[i] = bump_unit(o->obs_unit[i]));
    for (uint32_t i = 0; i < n * n; i++) FIELD(o->F[i] += 1.0);
    for (uint32_t i = 0; i < n; i++) for (uint32_t j = i; j < n; j++) FIELD(bump_cov(o->Q, n, i, j));
    for (uint32_t i = 0; i < m * n; i++) FIELD(o->H[i] += 1.0);
    for (uint32_t i = 0; i < m; i++) for (uint32_t j = i; j < m; j++) FIELD(bump_cov(o->R, m, i, j));
    FIELD(o->step_ns += 1);
    return 0;
}
static int mut_obs(const est_observation *b, est_observation *o, int idx)
{
    int k = 0; uint32_t m = b->m;
    for (uint32_t i = 0; i < m; i++) FIELD(o->unit[i] = bump_unit(o->unit[i]));
    for (uint32_t i = 0; i < m; i++) FIELD(o->z[i] += 1.0);
    for (uint32_t i = 0; i < m; i++) for (uint32_t j = i; j < m; j++) FIELD(bump_cov(o->R, m, i, j));
    FIELD(o->t_ns += 1); FIELD(o->seq += 1);
    DIGF(source); DIGF(evidence);
    return 0;
}
static int mut_belief(const est_belief *b, est_belief *o, int idx)
{
    int k = 0; uint32_t n = b->n;
    for (uint32_t i = 0; i < n; i++) FIELD(o->unit[i] = bump_unit(o->unit[i]));
    for (uint32_t i = 0; i < n; i++) FIELD(o->x[i] += 1.0);
    for (uint32_t i = 0; i < n; i++) for (uint32_t j = i; j < n; j++) FIELD(bump_cov(o->P, n, i, j));
    FIELD(o->generation += 1); FIELD(o->t_ns += 1);
    DIGF(model); DIGF(parent); DIGF(evidence_root);
    return 0;
}
static int mut_pred(const est_prediction *b, est_prediction *o, int idx)
{
    int k = 0; uint32_t n = b->n, m = b->m;
    DIGF(prior); DIGF(model);
    FIELD(o->horizon += 1); FIELD(o->generation += 1); FIELD(o->t_ns += 1);
    for (uint32_t i = 0; i < n; i++) FIELD(o->x[i] += 1.0);
    for (uint32_t i = 0; i < n; i++) for (uint32_t j = i; j < n; j++) FIELD(bump_cov(o->P, n, i, j));
    for (uint32_t i = 0; i < m; i++) FIELD(o->y_mean[i] += 1.0);
    for (uint32_t i = 0; i < m; i++) for (uint32_t j = i; j < m; j++) FIELD(bump_cov(o->S, m, i, j));
    return 0;
}
static int mut_innov(const est_innovation *b, est_innovation *o, int idx)
{
    int k = 0; uint32_t m = b->m;
    DIGF(prediction); DIGF(observation); DIGF(model);
    for (uint32_t i = 0; i < m; i++) FIELD(o->nu[i] += 1.0);
    for (uint32_t i = 0; i < m; i++) for (uint32_t j = i; j < m; j++) FIELD(bump_cov(o->S, m, i, j));
    FIELD(o->nis += 1.0); FIELD(o->t_ns += 1);
    return 0;
}

static int dig_eq(const est_digest *a, const est_digest *b) { return memcmp(a->b, b->b, 32) == 0; }

/* Generic property runner, one instantiation per kind. */
#define PROP(NAME, TYPE, GEN, MUT, ROUNDS)                                                   \
static void prop_##NAME(void)                                                                \
{                                                                                            \
    for (int r = 0; r < (ROUNDS); r++) {                                                     \
        TYPE a, o, dec; est_digest d0, d1; uint8_t buf[BUF], buf2[BUF]; size_t l = 0, l2 = 0;\
        GEN(&a);                                                                             \
        CHECK_ST(est_check_##NAME(&a), EST_OK);                                              \
        CHECK_ST(est_digest_##NAME(&a, &d0), EST_OK);                                        \
        CHECK_ST(est_digest_##NAME(&a, &d1), EST_OK);                                        \
        CHECK(dig_eq(&d0, &d1));                                                             \
        CHECK_ST(est_encode_##NAME(&a, buf, sizeof buf, &l), EST_OK);                        \
        CHECK_ST(est_decode_##NAME(buf, l, &dec), EST_OK);                                   \
        CHECK_ST(est_encode_##NAME(&dec, buf2, sizeof buf2, &l2), EST_OK);                   \
        CHECK(l == l2 && memcmp(buf, buf2, l) == 0);                                         \
        CHECK_ST(est_digest_##NAME(&dec, &d1), EST_OK);                                      \
        CHECK(dig_eq(&d0, &d1));                                                             \
        for (int idx = 0; MUT(&a, &o, idx); idx++) {                                         \
            CHECK_ST(est_check_##NAME(&o), EST_OK);                                          \
            CHECK_ST(est_digest_##NAME(&o, &d1), EST_OK);                                    \
            if (dig_eq(&d0, &d1)) { g_fail++; fprintf(stderr, "FAIL digest unchanged: " #NAME " field %d\n", idx); } \
            g_checks++;                                                                      \
        }                                                                                    \
    }                                                                                        \
}
PROP(model, est_model, gen_model, mut_model, 2000)
PROP(observation, est_observation, gen_obs, mut_obs, 2000)
PROP(belief, est_belief, gen_belief, mut_belief, 2000)
PROP(prediction, est_prediction, gen_pred, mut_pred, 2000)
PROP(innovation, est_innovation, gen_innov, mut_innov, 2000)

/* ---- kind dispatch helpers ---- */
typedef union { est_model m; est_observation o; est_belief b; est_prediction p; est_innovation i; } anyrec;
static est_status enc_kind(int kind, uint8_t *buf, size_t *len)
{
    anyrec a;
    switch (kind) {
    case 1: gen_model(&a.m); return est_encode_model(&a.m, buf, BUF, len);
    case 2: gen_obs(&a.o); return est_encode_observation(&a.o, buf, BUF, len);
    case 3: gen_belief(&a.b); return est_encode_belief(&a.b, buf, BUF, len);
    case 4: gen_pred(&a.p); return est_encode_prediction(&a.p, buf, BUF, len);
    default: gen_innov(&a.i); return est_encode_innovation(&a.i, buf, BUF, len);
    }
}
static est_status dec_kind(int kind, const uint8_t *buf, size_t len)
{
    anyrec a;
    switch (kind) {
    case 1: return est_decode_model(buf, len, &a.m);
    case 2: return est_decode_observation(buf, len, &a.o);
    case 3: return est_decode_belief(buf, len, &a.b);
    case 4: return est_decode_prediction(buf, len, &a.p);
    default: return est_decode_innovation(buf, len, &a.i);
    }
}

static void test_kind_separation(void)
{
    for (int round = 0; round < 50; round++)
        for (int ek = 1; ek <= 5; ek++) {
            uint8_t buf[BUF]; size_t l = 0;
            CHECK_ST(enc_kind(ek, buf, &l), EST_OK);
            for (int dk = 1; dk <= 5; dk++) {
                est_status want = (dk == ek) ? EST_OK : EST_ERR_KIND;
                CHECK_ST(dec_kind(dk, buf, l), want);
            }
        }
}

static void test_truncation_and_extension(void)
{
    for (int ek = 1; ek <= 5; ek++)
        for (int round = 0; round < 20; round++) {
            uint8_t buf[BUF + 16]; size_t l = 0;
            CHECK_ST(enc_kind(ek, buf, &l), EST_OK);
            int all_refused = 1;
            for (size_t cut = 0; cut < l; cut++) if (dec_kind(ek, buf, cut) == EST_OK) all_refused = 0;
            CHECK(all_refused);
            memset(buf + l, 0, 16);
            CHECK_ST(dec_kind(ek, buf, l + 1), EST_ERR_ENCODING);
            CHECK_ST(dec_kind(ek, buf, l + 8), EST_ERR_ENCODING);
            /* bad magic, bad version, nonzero reserved */
            uint8_t c[BUF];
            memcpy(c, buf, l); c[0] ^= 1; CHECK_ST(dec_kind(ek, c, l), EST_ERR_ENCODING);
            memcpy(c, buf, l); c[5] = (uint8_t)(EST_FORMAT_VERSION + 1u); CHECK_ST(dec_kind(ek, c, l), EST_ERR_ENCODING);
            memcpy(c, buf, l); c[6] = 1; CHECK_ST(dec_kind(ek, c, l), EST_ERR_ENCODING);
            /* unknown kind byte is a kind mismatch, never a decode */
            memcpy(c, buf, l); c[4] = 99; CHECK_ST(dec_kind(ek, c, l), EST_ERR_KIND);
            /* encoder refuses a buffer that is too small */
            size_t l2;
            CHECK_ST(est_encode_belief(&(est_belief){ .n = 1, .unit = { EST_UNIT_WATT }, .P = { 1.0 } }, c, 10, &l2), EST_ERR_ENCODING);
        }
    CHECK_ST(dec_kind(3, NULL, 0), EST_ERR_NULL);
}

/* ---- exact byte layout known-answer test ---- */
static void test_layout_kat(void)
{
    est_observation o;
    memset(&o, 0, sizeof o);
    o.m = 1; o.unit[0] = EST_UNIT_WATT; o.z[0] = 1.0; o.R[0] = -0.0 + 0.5;
    o.t_ns = 0x0102030405060708ll; o.seq = 0x1112131415161718ull;
    for (int i = 0; i < 32; i++) { o.source.b[i] = (uint8_t)i; o.evidence.b[i] = (uint8_t)(0x80 + i); }
    uint8_t buf[BUF]; size_t l = 0;
    CHECK_ST(est_encode_observation(&o, buf, sizeof buf, &l), EST_OK);
    CHECK(l == 8 + 4 + 4 + 8 + 8 + 8 + 8 + 32 + 32);
    CHECK(memcmp(buf, "OEST", 4) == 0 && buf[4] == 2 && buf[5] == 1 && buf[6] == 0 && buf[7] == 0);
    CHECK(buf[8] == 1 && buf[9] == 0 && buf[10] == 0 && buf[11] == 0);       /* m */
    CHECK(buf[12] == 3 && buf[13] == 0 && buf[14] == 0 && buf[15] == 0);     /* unit WATT = 3 */
    static const uint8_t one[8] = { 0, 0, 0, 0, 0, 0, 0xF0, 0x3F };          /* 1.0 bits, LE */
    CHECK(memcmp(buf + 16, one, 8) == 0);
    static const uint8_t half[8] = { 0, 0, 0, 0, 0, 0, 0xE0, 0x3F };         /* 0.5 */
    CHECK(memcmp(buf + 24, half, 8) == 0);
    static const uint8_t t[8] = { 8, 7, 6, 5, 4, 3, 2, 1 };
    CHECK(memcmp(buf + 32, t, 8) == 0);
    static const uint8_t sq[8] = { 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11 };
    CHECK(memcmp(buf + 40, sq, 8) == 0);
    CHECK(buf[48] == 0 && buf[79] == 31 && buf[80] == 0x80 && buf[111] == 0x9F);
    /* digest = SHA-256("omega.est.observation.v1" || 0 || encoding) */
    uint8_t cat[BUF + 64]; size_t dl = strlen(EST_DOMAIN_OBSERVATION);
    memcpy(cat, EST_DOMAIN_OBSERVATION, dl); cat[dl] = 0; memcpy(cat + dl + 1, buf, l);
    uint8_t want[32]; sha256_hash(cat, dl + 1 + l, want);
    est_digest d;
    CHECK_ST(est_digest_observation(&o, &d), EST_OK);
    CHECK(memcmp(d.b, want, 32) == 0);
}

static void test_negative_zero_and_unused(void)
{
    est_belief a, b; est_digest da, db;
    gen_belief(&a); b = a;
    a.x[0] = 0.0; b.x[0] = -0.0;
    CHECK_ST(est_digest_belief(&a, &da), EST_OK);
    CHECK_ST(est_digest_belief(&b, &db), EST_OK);
    CHECK(dig_eq(&da, &db));
    /* entries beyond n are not identity */
    b = a;
    for (uint32_t i = a.n; i < EST_MAX_DIM; i++) b.x[i] = 12345.0;
    for (uint32_t i = a.n * a.n; i < EST_MAX_DIM * EST_MAX_DIM; i++) b.P[i] = -7.0;
    CHECK_ST(est_digest_belief(&b, &db), EST_OK);
    CHECK(dig_eq(&da, &db));
    /* different dimension is a different record */
    if (a.n > 1) { b = a; b.n = a.n - 1; gen_cov(b.P, b.n); CHECK_ST(est_digest_belief(&b, &db), EST_OK); CHECK(!dig_eq(&da, &db)); }
}

static void test_digest_domains(void)
{
    /* same numbers in the shared shape, five kinds: five different digests */
    est_model mm; est_observation oo; est_belief bb; est_prediction pp; est_innovation ii;
    memset(&mm, 0, sizeof mm); memset(&oo, 0, sizeof oo); memset(&bb, 0, sizeof bb);
    memset(&pp, 0, sizeof pp); memset(&ii, 0, sizeof ii);
    mm.n = mm.m = 1; mm.estimator = EST_ESTIMATOR_LINEAR_KALMAN; mm.meaning = EST_UNCERTAINTY_GAUSSIAN_COVARIANCE;
    mm.state_unit[0] = mm.obs_unit[0] = EST_UNIT_WATT; mm.F[0] = mm.H[0] = mm.Q[0] = mm.R[0] = 1.0; mm.step_ns = 1;
    oo.m = 1; oo.unit[0] = EST_UNIT_WATT; oo.z[0] = 1.0; oo.R[0] = 1.0; oo.evidence.b[0] = 1;
    bb.n = 1; bb.unit[0] = EST_UNIT_WATT; bb.x[0] = 1.0; bb.P[0] = 1.0;
    pp.n = pp.m = 1; pp.horizon = 1; pp.x[0] = pp.y_mean[0] = pp.P[0] = pp.S[0] = 1.0;
    ii.m = 1; ii.nu[0] = 1.0; ii.S[0] = 1.0; ii.nis = 1.0;
    est_digest d[5];
    CHECK_ST(est_digest_model(&mm, &d[0]), EST_OK);
    CHECK_ST(est_digest_observation(&oo, &d[1]), EST_OK);
    CHECK_ST(est_digest_belief(&bb, &d[2]), EST_OK);
    CHECK_ST(est_digest_prediction(&pp, &d[3]), EST_OK);
    CHECK_ST(est_digest_innovation(&ii, &d[4]), EST_OK);
    for (int i = 0; i < 5; i++) for (int j = i + 1; j < 5; j++) CHECK(!dig_eq(&d[i], &d[j]));
    /* belief and prediction with all-zero digests and the same n: still different */
    CHECK(est_digest_is_zero(&bb.model) == 1);
    est_digest z; memset(&z, 0, sizeof z); CHECK(est_digest_is_zero(&z)); CHECK(!est_digest_is_zero(&d[0]));
}

static void test_evidence_root(void)
{
    est_digest r, a, b, ra, rb, rab, rba, rab2;
    memset(&r, 0, sizeof r); rdig(&a); rdig(&b);
    CHECK_ST(est_evidence_root_extend(&r, &a, &ra), EST_OK);
    CHECK_ST(est_evidence_root_extend(&r, &b, &rb), EST_OK);
    CHECK(!dig_eq(&ra, &rb) && !dig_eq(&ra, &r));
    CHECK_ST(est_evidence_root_extend(&ra, &b, &rab), EST_OK);
    CHECK_ST(est_evidence_root_extend(&rb, &a, &rba), EST_OK);
    CHECK(!dig_eq(&rab, &rba));                       /* order-sensitive */
    CHECK_ST(est_evidence_root_extend(&ra, &b, &rab2), EST_OK);
    CHECK(dig_eq(&rab, &rab2));                       /* deterministic */
    CHECK_ST(est_evidence_root_extend(&a, &b, &ra), EST_OK);
    CHECK_ST(est_evidence_root_extend(&b, &a, &rb), EST_OK);
    CHECK(!dig_eq(&ra, &rb));                         /* root and obs are not interchangeable */
    /* KAT: SHA-256("omega.est.belief.v1-root" || 0 || root || obs) */
    uint8_t cat[128]; size_t dl = strlen("omega.est.belief.v1-root");
    memcpy(cat, "omega.est.belief.v1-root", dl); cat[dl] = 0;
    memcpy(cat + dl + 1, a.b, 32); memcpy(cat + dl + 33, b.b, 32);
    uint8_t want[32]; sha256_hash(cat, dl + 65, want);
    CHECK_ST(est_evidence_root_extend(&a, &b, &ra), EST_OK);
    CHECK(memcmp(ra.b, want, 32) == 0);
    CHECK_ST(est_evidence_root_extend(NULL, &a, &ra), EST_ERR_NULL);
    /* out may alias an input */
    est_digest x = a;
    CHECK_ST(est_evidence_root_extend(&x, &b, &x), EST_OK);
    CHECK(memcmp(x.b, want, 32) == 0);
}

/* ---- invalid inputs ---- */
static void test_covariance_rules(void)
{
    double I2[4] = { 1, 0, 0, 1 };
    CHECK_ST(est_check_covariance(I2, 2), EST_OK);
    double Z[9] = { 0 };
    CHECK_ST(est_check_covariance(Z, 3), EST_OK);                 /* all-zero is PSD */
    double rank1[4] = { 1, 1, 1, 1 };
    CHECK_ST(est_check_covariance(rank1, 2), EST_OK);             /* singular PSD */
    double asym[4] = { 1, 0.5, 0, 1 };
    CHECK_ST(est_check_covariance(asym, 2), EST_ERR_ASYMMETRIC);
    double asym_tiny[4] = { 1, 1e-6, 0, 1 };                      /* relative 1e-6 > 1e-9 */
    CHECK_ST(est_check_covariance(asym_tiny, 2), EST_ERR_ASYMMETRIC);
    double sym_ok[4] = { 1, 1e-12, 0, 1 };                        /* within 1e-9 relative */
    CHECK_ST(est_check_covariance(sym_ok, 2), EST_OK);
    double negdef[4] = { -1, 0, 0, -1 };
    CHECK_ST(est_check_covariance(negdef, 2), EST_ERR_NOT_PSD);
    double neg1[1] = { -1e-3 };
    CHECK_ST(est_check_covariance(neg1, 1), EST_ERR_NOT_PSD);
    double indef[4] = { 1, 2, 2, 1 };
    CHECK_ST(est_check_covariance(indef, 2), EST_ERR_NOT_PSD);
    double indef0[4] = { 0, 1, 1, 0 };
    CHECK_ST(est_check_covariance(indef0, 2), EST_ERR_NOT_PSD);
    double indef3[9] = { 1, 0, 0, 0, 1, 0, 0, 0, -0.5 };
    CHECK_ST(est_check_covariance(indef3, 3), EST_ERR_NOT_PSD);
    double bad[4];
    double poison[3] = { NAN, INFINITY, -INFINITY };
    for (int p = 0; p < 3; p++) for (int pos = 0; pos < 4; pos++) {
        memcpy(bad, I2, sizeof bad); bad[pos] = poison[p];
        CHECK_ST(est_check_covariance(bad, 2), EST_ERR_NONFINITE);
    }
    CHECK_ST(est_check_covariance(I2, 0), EST_ERR_DIM);
    CHECK_ST(est_check_covariance(I2, 9), EST_ERR_DIM);
    CHECK_ST(est_check_covariance(NULL, 2), EST_ERR_NULL);
    double big[64]; memset(big, 0, sizeof big);
    for (int i = 0; i < 8; i++) big[i * 8 + i] = 1e300;
    CHECK_ST(est_check_covariance(big, 8), EST_OK);
    for (int i = 0; i < 8; i++) big[i * 8 + i] = 1e-300;
    CHECK_ST(est_check_covariance(big, 8), EST_OK);
}

static est_model base_model(void)
{
    est_model m; memset(&m, 0, sizeof m);
    m.n = 2; m.m = 1; m.estimator = EST_ESTIMATOR_LINEAR_KALMAN; m.meaning = EST_UNCERTAINTY_GAUSSIAN_COVARIANCE;
    m.state_unit[0] = EST_UNIT_MILLI_CELSIUS; m.state_unit[1] = EST_UNIT_MILLI_CELSIUS_PER_S; m.obs_unit[0] = EST_UNIT_MILLI_CELSIUS;
    m.F[0] = 1; m.F[1] = 0.1; m.F[3] = 1; m.H[0] = 1; m.Q[0] = 0.01; m.Q[3] = 0.01; m.R[0] = 4; m.step_ns = 100;
    return m;
}

static void test_invalid_records(void)
{
    double poison[3] = { NAN, INFINITY, -INFINITY };
    est_model m0 = base_model(); est_model m;
    CHECK_ST(est_check_model(&m0), EST_OK);
    for (int p = 0; p < 3; p++) {
        m = m0; m.F[1] = poison[p]; CHECK_ST(est_check_model(&m), EST_ERR_NONFINITE);
        m = m0; m.H[0] = poison[p]; CHECK_ST(est_check_model(&m), EST_ERR_NONFINITE);
        m = m0; m.Q[0] = poison[p]; CHECK_ST(est_check_model(&m), EST_ERR_NONFINITE);
        m = m0; m.R[0] = poison[p]; CHECK_ST(est_check_model(&m), EST_ERR_NONFINITE);
    }
    m = m0; m.n = 0; CHECK_ST(est_check_model(&m), EST_ERR_DIM);
    m = m0; m.n = 9; CHECK_ST(est_check_model(&m), EST_ERR_DIM);
    m = m0; m.m = 0; CHECK_ST(est_check_model(&m), EST_ERR_DIM);
    m = m0; m.m = 9; CHECK_ST(est_check_model(&m), EST_ERR_DIM);
    m = m0; m.state_unit[1] = EST_UNIT_NONE; CHECK_ST(est_check_model(&m), EST_ERR_UNIT);
    m = m0; m.obs_unit[0] = EST_UNIT_NONE; CHECK_ST(est_check_model(&m), EST_ERR_UNIT);
    m = m0; m.obs_unit[0] = EST_UNIT_MAX_; CHECK_ST(est_check_model(&m), EST_ERR_UNIT);
    m = m0; m.meaning = (est_uncertainty_meaning)0; CHECK_ST(est_check_model(&m), EST_ERR_UNIT);
    m = m0; m.estimator = (est_estimator_kind)2; CHECK_ST(est_check_model(&m), EST_ERR_UNIT);
    m = m0; m.step_ns = 0; CHECK_ST(est_check_model(&m), EST_ERR_TIME);
    m = m0; m.step_ns = -5; CHECK_ST(est_check_model(&m), EST_ERR_TIME);
    m = m0; m.Q[1] = 0.5; CHECK_ST(est_check_model(&m), EST_ERR_ASYMMETRIC);
    m = m0; m.Q[0] = -1; CHECK_ST(est_check_model(&m), EST_ERR_NOT_PSD);
    m = m0; m.R[0] = -1; CHECK_ST(est_check_model(&m), EST_ERR_NOT_PSD);
    CHECK_ST(est_check_model(NULL), EST_ERR_NULL);
    /* unused entries may hold anything */
    m = m0; m.F[63] = NAN; m.H[40] = INFINITY; CHECK_ST(est_check_model(&m), EST_OK);
    uint8_t buf[BUF]; size_t l = 7;
    m = m0; m.H[0] = NAN; CHECK_ST(est_encode_model(&m, buf, sizeof buf, &l), EST_ERR_NONFINITE);
    est_digest d;
    CHECK_ST(est_digest_model(&m, &d), EST_ERR_NONFINITE);

    est_observation o0; memset(&o0, 0, sizeof o0);
    o0.m = 2; o0.unit[0] = EST_UNIT_WATT; o0.unit[1] = EST_UNIT_WATT; o0.z[0] = 1; o0.z[1] = 2; o0.R[0] = 1; o0.R[3] = 1; o0.evidence.b[3] = 9;
    est_observation o;
    CHECK_ST(est_check_observation(&o0), EST_OK);
    for (int p = 0; p < 3; p++) {
        o = o0; o.z[1] = poison[p]; CHECK_ST(est_check_observation(&o), EST_ERR_NONFINITE);
        o = o0; o.R[3] = poison[p]; CHECK_ST(est_check_observation(&o), EST_ERR_NONFINITE);
    }
    o = o0; o.m = 0; CHECK_ST(est_check_observation(&o), EST_ERR_DIM);
    o = o0; o.m = 9; CHECK_ST(est_check_observation(&o), EST_ERR_DIM);
    o = o0; o.unit[1] = EST_UNIT_NONE; CHECK_ST(est_check_observation(&o), EST_ERR_UNIT);
    o = o0; memset(&o.evidence, 0, sizeof o.evidence); CHECK_ST(est_check_observation(&o), EST_ERR_KIND);
    o = o0; o.R[1] = 3; CHECK_ST(est_check_observation(&o), EST_ERR_ASYMMETRIC);
    o = o0; o.R[0] = 1; o.R[1] = o.R[2] = 2; CHECK_ST(est_check_observation(&o), EST_ERR_NOT_PSD);
    CHECK_ST(est_check_observation(NULL), EST_ERR_NULL);
    CHECK_ST(est_encode_observation(&o, buf, sizeof buf, &l), EST_ERR_NOT_PSD);
    CHECK_ST(est_encode_observation(&o0, NULL, 10, &l), EST_ERR_NULL);
    CHECK_ST(est_encode_observation(&o0, buf, sizeof buf, NULL), EST_ERR_NULL);

    est_belief b0; memset(&b0, 0, sizeof b0);
    b0.n = 2; b0.unit[0] = b0.unit[1] = EST_UNIT_BYTE; b0.x[0] = 1; b0.x[1] = -1; b0.P[0] = 2; b0.P[3] = 3;
    est_belief b;
    CHECK_ST(est_check_belief(&b0), EST_OK);
    for (int p = 0; p < 3; p++) {
        b = b0; b.x[0] = poison[p]; CHECK_ST(est_check_belief(&b), EST_ERR_NONFINITE);
        b = b0; b.P[2] = poison[p]; CHECK_ST(est_check_belief(&b), EST_ERR_NONFINITE);
    }
    b = b0; b.n = 0; CHECK_ST(est_check_belief(&b), EST_ERR_DIM);
    b = b0; b.n = 9; CHECK_ST(est_check_belief(&b), EST_ERR_DIM);
    b = b0; b.unit[0] = EST_UNIT_NONE; CHECK_ST(est_check_belief(&b), EST_ERR_UNIT);
    b = b0; b.unit[1] = (est_unit)77; CHECK_ST(est_check_belief(&b), EST_ERR_UNIT);
    b = b0; b.P[1] = 1; CHECK_ST(est_check_belief(&b), EST_ERR_ASYMMETRIC);
    b = b0; b.P[1] = b.P[2] = 5; CHECK_ST(est_check_belief(&b), EST_ERR_NOT_PSD);
    b = b0; b.P[0] = b.P[3] = -1; CHECK_ST(est_check_belief(&b), EST_ERR_NOT_PSD);
    CHECK_ST(est_check_belief(NULL), EST_ERR_NULL);

    est_prediction p0; memset(&p0, 0, sizeof p0);
    p0.n = 2; p0.m = 1; p0.horizon = 1; p0.x[0] = 1; p0.P[0] = 1; p0.P[3] = 1; p0.S[0] = 1;
    est_prediction p;
    CHECK_ST(est_check_prediction(&p0), EST_OK);
    for (int q = 0; q < 3; q++) {
        p = p0; p.x[1] = poison[q]; CHECK_ST(est_check_prediction(&p), EST_ERR_NONFINITE);
        p = p0; p.y_mean[0] = poison[q]; CHECK_ST(est_check_prediction(&p), EST_ERR_NONFINITE);
        p = p0; p.P[3] = poison[q]; CHECK_ST(est_check_prediction(&p), EST_ERR_NONFINITE);
        p = p0; p.S[0] = poison[q]; CHECK_ST(est_check_prediction(&p), EST_ERR_NONFINITE);
    }
    p = p0; p.horizon = 0; CHECK_ST(est_check_prediction(&p), EST_ERR_TIME);
    p = p0; p.n = 0; CHECK_ST(est_check_prediction(&p), EST_ERR_DIM);
    p = p0; p.n = 9; CHECK_ST(est_check_prediction(&p), EST_ERR_DIM);
    p = p0; p.m = 0; CHECK_ST(est_check_prediction(&p), EST_ERR_DIM);
    p = p0; p.m = 9; CHECK_ST(est_check_prediction(&p), EST_ERR_DIM);
    p = p0; p.P[1] = 1; CHECK_ST(est_check_prediction(&p), EST_ERR_ASYMMETRIC);
    p = p0; p.S[0] = -1; CHECK_ST(est_check_prediction(&p), EST_ERR_NOT_PSD);
    CHECK_ST(est_check_prediction(NULL), EST_ERR_NULL);

    est_innovation i0; memset(&i0, 0, sizeof i0);
    i0.m = 1; i0.nu[0] = 1; i0.S[0] = 2; i0.nis = 0.5;
    est_innovation iv;
    CHECK_ST(est_check_innovation(&i0), EST_OK);
    for (int q = 0; q < 3; q++) {
        iv = i0; iv.nu[0] = poison[q]; CHECK_ST(est_check_innovation(&iv), EST_ERR_NONFINITE);
        iv = i0; iv.S[0] = poison[q]; CHECK_ST(est_check_innovation(&iv), EST_ERR_NONFINITE);
        iv = i0; iv.nis = poison[q]; CHECK_ST(est_check_innovation(&iv), EST_ERR_NONFINITE);
    }
    iv = i0; iv.nis = -0.1; CHECK_ST(est_check_innovation(&iv), EST_ERR_NOT_PSD);
    iv = i0; iv.m = 0; CHECK_ST(est_check_innovation(&iv), EST_ERR_DIM);
    iv = i0; iv.m = 9; CHECK_ST(est_check_innovation(&iv), EST_ERR_DIM);
    iv = i0; iv.S[0] = -2; CHECK_ST(est_check_innovation(&iv), EST_ERR_NOT_PSD);
    CHECK_ST(est_check_innovation(NULL), EST_ERR_NULL);

    /* bytes that decode structurally but hold an invalid record are refused */
    l = 0;
    CHECK_ST(est_encode_belief(&b0, buf, sizeof buf, &l), EST_OK);
    uint8_t c[BUF]; memcpy(c, buf, l);
    static const uint8_t nanbits[8] = { 0, 0, 0, 0, 0, 0, 0xF8, 0x7F };
    memcpy(c + 8 + 4 + 8, nanbits, 8);                    /* x[0] */
    CHECK_ST(est_decode_belief(c, l, &b), EST_ERR_NONFINITE);
    memcpy(c, buf, l); c[8] = 0;                          /* n = 0 */
    CHECK_ST(est_decode_belief(c, l, &b), EST_ERR_DIM);
    memcpy(c, buf, l); c[8] = 9;                          /* n = 9 */
    CHECK_ST(est_decode_belief(c, l, &b), EST_ERR_DIM);
    memcpy(c, buf, l); c[12] = 0;                         /* unit[0] = NONE */
    CHECK_ST(est_decode_belief(c, l, &b), EST_ERR_UNIT);
    memcpy(c, buf, l); c[12] = 200;                       /* unit out of range */
    CHECK_ST(est_decode_belief(c, l, &b), EST_ERR_UNIT);
}

int main(void)
{
    test_covariance_rules();
    test_invalid_records();
    test_layout_kat();
    test_negative_zero_and_unused();
    test_digest_domains();
    test_evidence_root();
    test_kind_separation();
    test_truncation_and_extension();
    prop_model(); prop_observation(); prop_belief(); prop_prediction(); prop_innovation();
    printf("test_est_types: %d checks, %d failed\n", g_checks, g_fail);
    if (g_fail) { printf("test_est_types: FAIL\n"); return 1; }
    printf("test_est_types: PASS\n");
    return 0;
}
