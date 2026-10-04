/* ANS (Inertial Alignment v1) tests 1-10 of docs/ans/INERTIAL_ALIGNMENT_SPEC_V1.md.
 * Numbers chosen (reference below), per drift component:
 *   q: OBSERVE .001 EFFECT .01 DELEGATE .02 INGEST .03 SELF_MODIFY .05
 *   r: FIX .01 < REFERENCE .1 < IMU 1.0; prior P = r[FIX] = .01
 *   tier_sigma .25 / .5 / 1.0 on sigma = sqrt(trace P) = 2 sqrt(p)
 * Steady state of alternating EFFECT + IMU: p = (-q + sqrt(q^2 + 4 q r)) / 2
 * = .0951, sigma = .617, which lies in [.5, 1.0): IMU alone holds SIMULATE_ONLY
 * and can never restore FULL (test 5). */
#include "ans.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_ST(expr, want) do { int s_ = (int)(expr); g_checks++; if (s_ != (int)(want)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s = %d, want %d\n", __FILE__, __LINE__, #expr, s_, (int)(want)); } } while (0)

static ans_digest dg(uint8_t v) { ans_digest d; memset(d.b, v, sizeof d.b); return d; }
static int deq(const ans_digest *a, const ans_digest *b) { return memcmp(a->b, b->b, EST_DIGEST_SIZE) == 0; }
static void hex(const ans_digest *d, char *o) { for (int i = 0; i < 32; i++) sprintf(o + 2 * i, "%02x", d->b[i]); }

static ans_reference mkref(void)
{
    ans_reference r;
    memset(&r, 0, sizeof r);
    r.version = 1;
    for (int i = 0; i < (int)ANS_ATOMS; i++) r.limit[i] = 1.0;
    r.limit[ANS_ATOM_SELF_MODIFICATION_LIMITS] = 0.5;
    r.q[ANS_ACT_OBSERVE] = 0.001; r.q[ANS_ACT_EFFECT] = 0.01; r.q[ANS_ACT_DELEGATE] = 0.02;
    r.q[ANS_ACT_INGEST_UNTRUSTED] = 0.03; r.q[ANS_ACT_SELF_MODIFY] = 0.05;
    r.r[ANS_SENSOR_IMU] = 1.0; r.r[ANS_SENSOR_REFERENCE] = 0.1; r.r[ANS_SENSOR_FIX] = 0.01;
    r.tier_sigma[0] = 0.25; r.tier_sigma[1] = 0.5; r.tier_sigma[2] = 1.0;
    r.nis_limit = 13.28;
    r.promote_limit = 0.6;
    return r;
}

typedef struct { ans_reference ref; ans_digest rd; ans_state s; } rig;
static void mkrig(rig *g)
{
    g->ref = mkref();
    CHECK_ST(ans_reference_freeze(&g->ref, &g->rd), ANS_OK);
    ans_digest goal = dg(0x11), intent = dg(0x22);
    CHECK_ST(ans_state_prior(&g->ref, &g->rd, &goal, &intent, &g->s, NULL), ANS_OK);
}
static ans_action act(ans_act_kind k, uint8_t id, double risk)
{
    ans_action a;
    memset(&a, 0, sizeof a);
    a.kind = k; a.id = dg(id); a.reversibility_risk = risk;
    return a;
}
static ans_measurement meas(ans_sensor_class c, const rig *g, const double *z, uint8_t ev)
{
    ans_measurement m;
    memset(&m, 0, sizeof m);
    m.cls = c;
    for (int i = 0; i < (int)ANS_DIM; i++) m.z[i] = z ? z[i] : g->s.drift.x[i];
    m.source = dg(0x33); m.evidence = dg(ev);
    if (c != ANS_SENSOR_IMU) m.reference = g->rd;
    m.t_ns = 100; m.seq = ev;
    return m;
}
static void step(rig *g, ans_act_kind k, uint8_t id, double risk)
{
    ans_action a = act(k, id, risk);
    CHECK_ST(ans_step(&g->ref, &g->rd, &g->s, &a, &g->s, NULL), ANS_OK);
}
static void measure(rig *g, ans_sensor_class c, const double *z, uint8_t ev)
{
    ans_measurement m = meas(c, g, z, ev);
    CHECK_ST(ans_measure(&g->ref, &g->rd, &g->s, &m, &g->s, NULL, NULL), ANS_OK);
}
static ans_verdict verdict(const rig *g)
{
    ans_verdict v;
    memset(&v, 0, sizeof v);
    CHECK_ST(ans_autonomy(&g->ref, &g->rd, &g->s, &v), ANS_OK);
    return v;
}
static ans_promo promo(const rig *g, const ans_state *cand, const ans_digest *ref_birth,
                       const ans_digest *t_birth, const ans_digest *t_now)
{
    ans_promotion_request q;
    ans_promotion_record out;
    memset(&q, 0, sizeof q);
    q.candidate = dg(0x44);
    q.reference_at_birth = *ref_birth;
    q.tests_at_birth = *t_birth;
    q.tests_now = *t_now;
    q.candidate_state = *cand;
    memset(&out, 0xAA, sizeof out);
    CHECK_ST(ans_promotion_check(&g->ref, &g->rd, &q, &out), ANS_OK);
    ans_digest d;
    CHECK_ST(ans_digest_promotion_record(&out, &d), ANS_OK);
    CHECK(deq(&d, &out.digest));
    return out.result;
}

static void test1_freeze(void)
{
    ans_reference a = mkref(), b = mkref();
    ans_digest da, db, dc;
    memset(&da, 0, sizeof da);
    CHECK_ST(ans_reference_check(&a, &da), ANS_ERR_REFERENCE); /* unfrozen refused */
    CHECK_ST(ans_reference_freeze(&a, &da), ANS_OK);
    CHECK_ST(ans_reference_freeze(&b, &db), ANS_OK);
    CHECK(deq(&da, &db));
    CHECK(a.frozen == 1);
    CHECK_ST(ans_reference_check(&a, &da), ANS_OK);
    /* every field change gives a different digest */
    ans_reference m;
#define MUT(EXPR) do { m = mkref(); EXPR; CHECK_ST(ans_digest_reference(&m, &dc), ANS_OK); CHECK(!deq(&dc, &da)); } while (0)
    MUT(m.version = 2);
    for (int i = 0; i < (int)ANS_ATOMS; i++) MUT(m.limit[i] += 0.125);
    for (int i = 0; i < (int)ANS_ACT_KINDS; i++) MUT(m.q[i] *= 1.5);
    for (int i = 0; i < (int)ANS_SENSOR_KINDS; i++) MUT(m.r[i] *= 0.9);
    for (int i = 0; i < 3; i++) MUT(m.tier_sigma[i] *= 1.01);
    MUT(m.nis_limit += 1.0);
    MUT(m.promote_limit += 0.01);
#undef MUT
    /* invalid references cannot be frozen */
    m = mkref(); m.r[ANS_SENSOR_IMU] = 0.05; /* breaks FIX < REFERENCE < IMU */
    CHECK_ST(ans_reference_freeze(&m, &dc), ANS_ERR_REFERENCE);
    CHECK(m.frozen == 0);
    m = mkref(); m.limit[3] = 0.0;
    CHECK_ST(ans_reference_freeze(&m, &dc), ANS_ERR_REFERENCE);
    m = mkref(); m.q[1] = NAN;
    CHECK_ST(ans_reference_freeze(&m, &dc), ANS_ERR_REFERENCE);
    m = mkref(); m.tier_sigma[1] = m.tier_sigma[0];
    CHECK_ST(ans_reference_freeze(&m, &dc), ANS_ERR_REFERENCE);
    /* an unfrozen reference is refused by every API even with the right digest */
    rig g; mkrig(&g);
    ans_reference u = mkref();
    ans_action ac = act(ANS_ACT_EFFECT, 1, 0.1);
    ans_state o;
    ans_digest goal = dg(1);
    CHECK_ST(ans_step(&u, &g.rd, &g.s, &ac, &o, NULL), ANS_ERR_REFERENCE);
    CHECK_ST(ans_state_prior(&u, &g.rd, &goal, &goal, &o, NULL), ANS_ERR_REFERENCE);
}

static void test2_hostile(void)
{
    rig g; mkrig(&g);
    ans_reference evil = g.ref;
    evil.limit[ANS_ATOM_SELF_MODIFICATION_LIMITS] = 100.0; /* candidate loosens its own compass */
    CHECK(evil.frozen == 1);
    ans_state o;
    ans_verdict v;
    ans_promotion_record pr;
    ans_promotion_request q;
    ans_action ac = act(ANS_ACT_EFFECT, 1, 0.1);
    ans_measurement m = meas(ANS_SENSOR_FIX, &g, NULL, 5);
    ans_digest goal = dg(1);
    memset(&q, 0, sizeof q);
    q.candidate_state = g.s; q.reference_at_birth = g.rd;
    CHECK_ST(ans_state_prior(&evil, &g.rd, &goal, &goal, &o, NULL), ANS_ERR_REFERENCE);
    CHECK_ST(ans_step(&evil, &g.rd, &g.s, &ac, &o, NULL), ANS_ERR_REFERENCE);
    CHECK_ST(ans_measure(&evil, &g.rd, &g.s, &m, &o, NULL, NULL), ANS_ERR_REFERENCE);
    CHECK_ST(ans_autonomy(&evil, &g.rd, &g.s, &v), ANS_ERR_REFERENCE);
    CHECK_ST(ans_promotion_check(&evil, &g.rd, &q, &pr), ANS_ERR_REFERENCE);
    /* a copy whose frozen flag was cleared, and the genuine reference with a forged digest */
    ans_reference thawed = g.ref; thawed.frozen = 0;
    CHECK_ST(ans_step(&thawed, &g.rd, &g.s, &ac, &o, NULL), ANS_ERR_REFERENCE);
    ans_digest forged = g.rd; forged.b[0] ^= 1;
    CHECK_ST(ans_step(&g.ref, &forged, &g.s, &ac, &o, NULL), ANS_ERR_REFERENCE);
    CHECK_ST(ans_autonomy(&g.ref, &forged, &g.s, &v), ANS_ERR_REFERENCE);
    /* the evil copy self-consistently re-frozen has a new digest: a state bound to the old one refuses it */
    ans_reference evil2 = evil; ans_digest ed;
    evil2.frozen = 0;
    CHECK_ST(ans_reference_freeze(&evil2, &ed), ANS_OK);
    CHECK(!deq(&ed, &g.rd));
    CHECK_ST(ans_step(&evil2, &ed, &g.s, &ac, &o, NULL), ANS_ERR_REFERENCE);
}

static void test3_drift_grows(void)
{
    rig g; mkrig(&g);
    ans_verdict v = verdict(&g);
    double prev = v.sigma;
    ans_tier pt = v.tier;
    CHECK(v.tier == ANS_TIER_FULL);
    for (int i = 0; i < 50; i++) {
        step(&g, ANS_ACT_EFFECT, (uint8_t)(0x40 + i), 0.1);
        v = verdict(&g);
        CHECK(v.sigma > prev);
        CHECK(v.tier >= pt);
        prev = v.sigma; pt = v.tier;
    }
    CHECK(v.tier == ANS_TIER_HALT_REQUEST_FIX);
    CHECK(g.s.since_fix == 50);
    CHECK(g.s.generation == 50);
    CHECK(v.D >= v.sigma);
}

static void test4_fix_restores(void)
{
    rig g; mkrig(&g);
    for (int i = 0; i < 50; i++) step(&g, ANS_ACT_EFFECT, (uint8_t)(0x40 + i), 0.1);
    CHECK(verdict(&g).tier == ANS_TIER_HALT_REQUEST_FIX);
    measure(&g, ANS_SENSOR_FIX, NULL, 0x90);
    ans_verdict v = verdict(&g);
    CHECK(v.sigma < g.ref.tier_sigma[0]);
    CHECK(g.s.since_fix == 0);
    CHECK(v.tier == ANS_TIER_FULL);
    CHECK(g.s.disagreement == 0);
}

static void test5_imu_cannot_restore(void)
{
    rig g; mkrig(&g);
    int guard = 0;
    while (verdict(&g).tier != ANS_TIER_SIMULATE_ONLY && guard++ < 100) step(&g, ANS_ACT_EFFECT, (uint8_t)(0x40 + guard), 0.1);
    CHECK(verdict(&g).tier == ANS_TIER_SIMULATE_ONLY);
    for (int i = 0; i < 200; i++) {
        step(&g, ANS_ACT_EFFECT, (uint8_t)(i + 1), 0.1);
        measure(&g, ANS_SENSOR_IMU, NULL, (uint8_t)(0x80 + (i & 0x3f)));
        CHECK(verdict(&g).tier != ANS_TIER_FULL);
    }
    ans_verdict v = verdict(&g);
    CHECK(v.tier == ANS_TIER_SIMULATE_ONLY);
    CHECK(fabs(v.sigma - 0.617) < 0.01);
    CHECK(g.s.since_fix > 200);
}

static void test6_disagreement(void)
{
    rig g; mkrig(&g);
    ans_verdict v = verdict(&g);
    CHECK(v.tier == ANS_TIER_FULL);
    double far[ANS_DIM] = { 3.0, 0, 0, 0 };
    est_innovation iv;
    ans_measurement m = meas(ANS_SENSOR_REFERENCE, &g, far, 0x61);
    CHECK_ST(ans_measure(&g.ref, &g.rd, &g.s, &m, &g.s, &iv, NULL), ANS_OK);
    CHECK(iv.nis > g.ref.nis_limit);
    CHECK(g.s.last_nis == iv.nis);
    CHECK(g.s.disagreement == 1);
    v = verdict(&g);
    CHECK(v.sigma < g.ref.tier_sigma[0]); /* sigma small, yet */
    CHECK(v.tier == ANS_TIER_HALT_REQUEST_FIX);
    CHECK(v.mean_norm > 0.2); /* fused anyway (gain .09 on a 3.0 reading), never hidden */
    /* an agreeing REFERENCE does not clear it; only a FIX within limit does */
    measure(&g, ANS_SENSOR_REFERENCE, NULL, 0x62);
    CHECK(g.s.disagreement == 1);
    measure(&g, ANS_SENSOR_FIX, NULL, 0x63);
    CHECK(g.s.disagreement == 0);
    /* an agreeing reading never raises it */
    rig h; mkrig(&h);
    measure(&h, ANS_SENSOR_REFERENCE, NULL, 0x64);
    CHECK(h.s.disagreement == 0);
}

static void test7_refusals(void)
{
    rig g; mkrig(&g);
    ans_state o;
    ans_measurement m;
    m = meas(ANS_SENSOR_FIX, &g, NULL, 1); memset(&m.evidence, 0, sizeof m.evidence);
    CHECK_ST(ans_measure(&g.ref, &g.rd, &g.s, &m, &o, NULL, NULL), ANS_ERR_EVIDENCE);
    m = meas(ANS_SENSOR_FIX, &g, NULL, 1); memset(&m.source, 0, sizeof m.source);
    CHECK_ST(ans_measure(&g.ref, &g.rd, &g.s, &m, &o, NULL, NULL), ANS_ERR_EVIDENCE);
    m = meas(ANS_SENSOR_IMU, &g, NULL, 1); m.reference = g.rd;
    CHECK_ST(ans_measure(&g.ref, &g.rd, &g.s, &m, &o, NULL, NULL), ANS_ERR_SENSOR);
    m = meas(ANS_SENSOR_FIX, &g, NULL, 1); m.reference = dg(0x77);
    CHECK_ST(ans_measure(&g.ref, &g.rd, &g.s, &m, &o, NULL, NULL), ANS_ERR_SENSOR);
    m = meas(ANS_SENSOR_REFERENCE, &g, NULL, 1); memset(&m.reference, 0, sizeof m.reference);
    CHECK_ST(ans_measure(&g.ref, &g.rd, &g.s, &m, &o, NULL, NULL), ANS_ERR_SENSOR);
    m = meas(ANS_SENSOR_FIX, &g, NULL, 1); m.z[2] = NAN;
    CHECK_ST(ans_measure(&g.ref, &g.rd, &g.s, &m, &o, NULL, NULL), ANS_ERR_NONFINITE);
    m = meas(ANS_SENSOR_IMU, &g, NULL, 1); m.z[0] = INFINITY;
    CHECK_ST(ans_measure(&g.ref, &g.rd, &g.s, &m, &o, NULL, NULL), ANS_ERR_NONFINITE);
    m = meas(ANS_SENSOR_IMU, &g, NULL, 1); m.cls = (ans_sensor_class)9;
    CHECK_ST(ans_measure(&g.ref, &g.rd, &g.s, &m, &o, NULL, NULL), ANS_ERR_SENSOR);
    /* a belief digest passed off as evidence (EST refuses) surfaces as ERR_EST */
    est_status es = EST_OK;
    measure(&g, ANS_SENSOR_FIX, NULL, 0x30); /* now the belief has an evidence root */
    m = meas(ANS_SENSOR_IMU, &g, NULL, 1); m.evidence = g.s.evidence_root;
    CHECK_ST(ans_measure(&g.ref, &g.rd, &g.s, &m, &o, NULL, &es), ANS_ERR_EST);
    CHECK(es != EST_OK);
    /* actions */
    ans_action a = act(ANS_ACT_EFFECT, 1, 0.1);
    CHECK_ST(ans_step(&g.ref, &g.rd, &g.s, &a, &o, NULL), ANS_OK);
    a = act(ANS_ACT_EFFECT, 1, NAN);
    CHECK_ST(ans_step(&g.ref, &g.rd, &g.s, &a, &o, NULL), ANS_ERR_NONFINITE);
    a = act(ANS_ACT_EFFECT, 1, 1.5);
    CHECK_ST(ans_step(&g.ref, &g.rd, &g.s, &a, &o, NULL), ANS_ERR_RANGE);
    a = act(ANS_ACT_EFFECT, 0, 0.1);
    CHECK_ST(ans_step(&g.ref, &g.rd, &g.s, &a, &o, NULL), ANS_ERR_EVIDENCE);
    a = act((ans_act_kind)7, 1, 0.1);
    CHECK_ST(ans_step(&g.ref, &g.rd, &g.s, &a, &o, NULL), ANS_ERR_RANGE);
    a = act(ANS_ACT_EFFECT, 1, 0.1); a.touches[3] = 2;
    CHECK_ST(ans_step(&g.ref, &g.rd, &g.s, &a, &o, NULL), ANS_ERR_RANGE);
    a = act(ANS_ACT_SELF_MODIFY, 2, 0.9); a.touches[ANS_ATOM_SELF_MODIFICATION_LIMITS] = 1;
    CHECK_ST(ans_step(&g.ref, &g.rd, &g.s, &a, &o, NULL), ANS_ERR_ATOM);
    /* the refusal is recorded, drift is not repaired */
    CHECK(o.generation == g.s.generation + 1);
    CHECK(!deq(&o.provenance, &g.s.provenance));
    CHECK(memcmp(o.drift.P, g.s.drift.P, sizeof o.drift.P) == 0);
    CHECK(o.since_fix == g.s.since_fix);
    CHECK(o.self_mod_steps == 0);
    ans_digest zg; memset(&zg, 0, sizeof zg);
    CHECK_ST(ans_state_prior(&g.ref, &g.rd, &zg, &g.rd, &o, NULL), ANS_ERR_RANGE);
    CHECK_ST(ans_step(NULL, &g.rd, &g.s, &a, &o, NULL), ANS_ERR_NULL);
}

static void test8_promotion(void)
{
    rig g; mkrig(&g);
    ans_digest t0 = dg(0x55), t1 = dg(0x56);
    ans_state good;
    { rig h = g; measure(&h, ANS_SENSOR_FIX, NULL, 0x71); good = h.s; }
    CHECK(promo(&g, &good, &g.rd, &t0, &t0) == ANS_PROMOTE_OK);
    ans_digest other = dg(0x99);
    CHECK(promo(&g, &good, &other, &t0, &t0) == ANS_PROMOTE_REFUSED_REFERENCE);
    CHECK(promo(&g, &good, &g.rd, &t0, &t1) == ANS_PROMOTE_REFUSED_TESTS);
    ans_state dis;
    { rig h = g; double far[ANS_DIM] = { 3.0, 0, 0, 0 }; measure(&h, ANS_SENSOR_REFERENCE, far, 0x72); dis = h.s; }
    CHECK(dis.disagreement == 1);
    CHECK(promo(&g, &dis, &g.rd, &t0, &t0) == ANS_PROMOTE_REFUSED_DISAGREEMENT);
    ans_state drift;
    { rig h = g; for (int i = 0; i < 12; i++) step(&h, ANS_ACT_EFFECT, (uint8_t)(0x50 + i), 0.1); drift = h.s; }
    CHECK(drift.disagreement == 0 && drift.self_mod_unfixed == 0);
    CHECK(promo(&g, &drift, &g.rd, &t0, &t0) == ANS_PROMOTE_REFUSED_DRIFT);
    ans_state sm;
    { rig h = g; ans_action a = act(ANS_ACT_SELF_MODIFY, 0x58, 0.2); a.touches[ANS_ATOM_SELF_MODIFICATION_LIMITS] = 1;
      CHECK_ST(ans_step(&h.ref, &h.rd, &h.s, &a, &h.s, NULL), ANS_OK); sm = h.s; }
    CHECK(promo(&g, &sm, &g.rd, &t0, &t0) == ANS_PROMOTE_REFUSED_SELF_MOD);
    /* precedence: reference change is reported before everything else */
    CHECK(promo(&g, &sm, &other, &t0, &t1) == ANS_PROMOTE_REFUSED_REFERENCE);
}

static void test10_self_mod(void)
{
    rig g; mkrig(&g);
    ans_digest t0 = dg(0x55);
    ans_action a = act(ANS_ACT_SELF_MODIFY, 0x58, 0.2);
    CHECK_ST(ans_step(&g.ref, &g.rd, &g.s, &a, &g.s, NULL), ANS_OK); /* SELF_MODIFY by kind alone */
    CHECK(g.s.self_mod_steps == 1 && g.s.self_mod_unfixed == 1);
    CHECK(verdict(&g).D <= g.ref.promote_limit); /* drift alone would not refuse */
    CHECK(promo(&g, &g.s, &g.rd, &t0, &t0) == ANS_PROMOTE_REFUSED_SELF_MOD);
    measure(&g, ANS_SENSOR_FIX, NULL, 0x73);
    CHECK(g.s.self_mod_steps == 1 && g.s.self_mod_unfixed == 0);
    CHECK(promo(&g, &g.s, &g.rd, &t0, &t0) == ANS_PROMOTE_OK);
}

/* Golden values: the digests below are the contract of encoding v1. */
static const char *GOLD_REF   = "3be9ab21e6c989c6793e5eb69aa9d87493d4dfd42ce4aad04a3e89d3c4d0a110";
static const char *GOLD_STATE = "20cfa169d802561f5c5a03c82edf8162711737aee68dbe5e17c3f436ff4caf04";
static const char *GOLD_VERD  = "e82707eb13485dbca4325a2214c60072444f6ed9c0ff0838bea0131208deec25";
static const char *GOLD_PROMO = "5541443d59abf153e5595d16f05e1d645aecbacc9f85c619af4ac6cd7e990530";
static const char *GOLD_ACT   = "f9ac8b7cd8612affbf1a3f3917953bbb9fa40d5cec380892ef2a35750b7c4c49";
static const char *GOLD_MEAS  = "db1f5305061835bd0b338aa387dd1564a285c756bfe420e4b17f8aa8fb55e00f";

static void gold(const char *name, const ans_digest *d, const char *want)
{
    char h[65];
    hex(d, h);
    printf("golden %-12s %s\n", name, h);
    CHECK(strcmp(h, want) == 0);
}

static void rt_check(int ok_a, int ok_b) { CHECK(ok_a); CHECK(ok_b); }

static void test9_encodings(void)
{
    rig g; mkrig(&g);
    ans_action a = act(ANS_ACT_DELEGATE, 0x21, 0.25); a.touches[ANS_ATOM_DELEGATION_LIMITS] = 1;
    step(&g, ANS_ACT_EFFECT, 0x20, 0.1);
    CHECK_ST(ans_step(&g.ref, &g.rd, &g.s, &a, &g.s, NULL), ANS_OK);
    double z[ANS_DIM] = { 0.05, -0.02, 0.0, 0.01 };
    ans_measurement m = meas(ANS_SENSOR_REFERENCE, &g, z, 0x23);
    CHECK_ST(ans_measure(&g.ref, &g.rd, &g.s, &m, &g.s, NULL, NULL), ANS_OK);
    ans_verdict v = verdict(&g);
    ans_digest t0 = dg(0x55);
    ans_promotion_request q;
    ans_promotion_record pr;
    memset(&q, 0, sizeof q);
    q.candidate = dg(0x44); q.reference_at_birth = g.rd; q.tests_at_birth = t0; q.tests_now = t0; q.candidate_state = g.s;
    CHECK_ST(ans_promotion_check(&g.ref, &g.rd, &q, &pr), ANS_OK);

    uint8_t b1[ANS_ENCODED_MAX], b2[ANS_ENCODED_MAX];
    size_t l1, l2;
    ans_digest d;
#define RT(NAME, TYPE, OBJ)                                                                    \
    do {                                                                                       \
        TYPE back;                                                                             \
        CHECK_ST(ans_encode_##NAME(&(OBJ), b1, sizeof b1, &l1), ANS_OK);                       \
        CHECK_ST(ans_decode_##NAME(b1, l1, &back), ANS_OK);                                    \
        CHECK_ST(ans_encode_##NAME(&back, b2, sizeof b2, &l2), ANS_OK);                        \
        rt_check(l1 == l2, memcmp(b1, b2, l1) == 0);                                           \
        CHECK_ST(ans_decode_##NAME(b1, l1 - 1, &back), ANS_ERR_ENCODING);                      \
        CHECK_ST(ans_encode_##NAME(&(OBJ), b2, l1 - 1, &l2), ANS_ERR_ENCODING);                \
        uint8_t bad[ANS_ENCODED_MAX]; memcpy(bad, b1, l1); bad[4] ^= 0x7f;                     \
        CHECK_ST(ans_decode_##NAME(bad, l1, &back), ANS_ERR_ENCODING);                         \
        memcpy(bad, b1, l1); bad[5] = 9;                                                       \
        CHECK_ST(ans_decode_##NAME(bad, l1, &back), ANS_ERR_ENCODING);                         \
        memcpy(bad, b1, l1); bad[l1] = 0;                                                      \
        CHECK_ST(ans_decode_##NAME(bad, l1 + 1, &back), ANS_ERR_ENCODING);                     \
    } while (0)
    RT(reference, ans_reference, g.ref);
    RT(action, ans_action, a);
    RT(measurement, ans_measurement, m);
    RT(state, ans_state, g.s);
    RT(verdict, ans_verdict, v);
    RT(promotion_record, ans_promotion_record, pr);
#undef RT
    /* cross-kind decode refused */
    { ans_action back; CHECK_ST(ans_encode_state(&g.s, b1, sizeof b1, &l1), ANS_OK);
      CHECK_ST(ans_decode_action(b1, l1, &back), ANS_ERR_ENCODING); }
    /* a decoded reference keeps its digest and its seal */
    { ans_reference back; ans_digest d2;
      CHECK_ST(ans_encode_reference(&g.ref, b1, sizeof b1, &l1), ANS_OK);
      CHECK_ST(ans_decode_reference(b1, l1, &back), ANS_OK);
      CHECK_ST(ans_digest_reference(&back, &d2), ANS_OK);
      CHECK(deq(&d2, &g.rd)); CHECK(back.frozen == 1); CHECK_ST(ans_reference_check(&back, &g.rd), ANS_OK); }
    /* digest stability and domain separation */
    ans_digest da, db;
    CHECK_ST(ans_digest_state(&g.s, &da), ANS_OK);
    CHECK_ST(ans_digest_state(&g.s, &db), ANS_OK);
    CHECK(deq(&da, &db));
    { ans_state t = g.s; t.last_nis += 1e-9; CHECK_ST(ans_digest_state(&t, &db), ANS_OK); CHECK(!deq(&da, &db)); }
    CHECK(!deq(&g.rd, &da));
    /* determinism: the same sequence yields the same final state digest */
    { rig h; mkrig(&h); step(&h, ANS_ACT_EFFECT, 0x20, 0.1);
      CHECK_ST(ans_step(&h.ref, &h.rd, &h.s, &a, &h.s, NULL), ANS_OK);
      CHECK_ST(ans_measure(&h.ref, &h.rd, &h.s, &m, &h.s, NULL, NULL), ANS_OK);
      CHECK_ST(ans_digest_state(&h.s, &db), ANS_OK); CHECK(deq(&da, &db)); }
    /* goldens */
    gold("reference", &g.rd, GOLD_REF);
    CHECK_ST(ans_digest_action(&a, &d), ANS_OK); gold("action", &d, GOLD_ACT);
    CHECK_ST(ans_digest_measurement(&m, &d), ANS_OK); gold("measurement", &d, GOLD_MEAS);
    gold("state", &da, GOLD_STATE);
    gold("verdict", &v.digest, GOLD_VERD);
    gold("promotion", &pr.digest, GOLD_PROMO);
}

int main(void)
{
    test1_freeze();
    test2_hostile();
    test3_drift_grows();
    test4_fix_restores();
    test5_imu_cannot_restore();
    test6_disagreement();
    test7_refusals();
    test8_promotion();
    test9_encodings();
    test10_self_mod();
    printf("test_ans: %d checks, %d failures: %s\n", g_checks, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
