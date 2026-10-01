/*
 * M20 canonical Capability Graph and Skill Router.
 *
 * Behaviour added in M20 only: register / update / availability / withdraw
 * on keyed entries, canonical machine identity behind the graph's indexes,
 * query by required capability, authority filtering through the native
 * authority view, deterministic routing, Skill -> capabilities, no-candidate,
 * the wire form between two catalogs, and one real action-graph run whose
 * skill node was chosen by capability, not by implementation.
 */
#include "runtime/aien_machine_id.h"
#include "runtime/aienos_cap.h"
#include "runtime/rx_capq.h"
#include "runtime/rx_graph.h"
#include "runtime/rx_skillroute.h"
#include "runtime/rx_world.h"

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

#define NOW_US 1000000ull
enum { OP_SCALE = 1, OP_SCALE_FAST = 2, OP_SUMMARIZE = 3, OP_LOCAL_ONLY = 4, ALIAS_SCALE = 501 };
enum { SUBJ_EXTERNAL = 100, ISSUER = 3, SUBJ_PLAN = 61 };
#define RES_CELL 0xA000001ull
#define RES_RUN  0xA000002ull
#define RES_HELD 0xB000001ull
#define RES_ASK  0xB000002ull

static AienMachineId mid(uint8_t seed) {
    AienMachineId m;
    uint8_t root[8] = { 'n', 'o', 'd', 'e', seed, 0, 0, 0 };
    aien_mid_derive(AIEN_MID_ROOT_PROVISIONED, root, sizeof root, &m);
    return m;
}

/* The shared operation catalog both nodes load. */
static void ontology(CqCatalog *c) {
    uint32_t all = CQ_SRC(CQ_SRC_GRAPH) | CQ_SRC(CQ_SRC_SKILL) | CQ_SRC(CQ_SRC_FABRIC);
    CHECK(cq_op_define(c, OP_SCALE, 0, all) == CQ_OK, "op scale");
    CHECK(cq_op_define(c, OP_SCALE_FAST, OP_SCALE, all) == CQ_OK, "op scale.fast");
    CHECK(cq_op_define(c, OP_SUMMARIZE, 0, all) == CQ_OK, "op summarize");
    CHECK(cq_op_define(c, OP_LOCAL_ONLY, 0, CQ_SRC(CQ_SRC_SKILL)) == CQ_OK, "op local");
    CHECK(cq_alias(c, ALIAS_SCALE, OP_SCALE) == CQ_OK, "alias");
}

static CqEntry provide(uint32_t cap, uint32_t op, uint32_t real, uint64_t cost) {
    CqEntry e;
    memset(&e, 0, sizeof e);
    e.capability_id = cap;
    e.realization_id = real;
    e.op = op;
    e.effects = CQ_FX_PURE;
    e.in_types = 0x1;
    e.out_types = 0x2;
    e.confidence_ppm = 900000;
    e.reliability_ppm = 990000;
    e.evidence_level = CQ_EV_MEASURED;
    e.evidence_ref = 0xE000u + cap;
    e.cost = cost;
    e.latency_us = 100;
    e.energy_uj = 100;
    e.live = CQ_LIVE_AVAILABLE;
    e.generation = 1;
    return e;
}

static SrRequirement requirement(uint32_t op) {
    SrRequirement q;
    memset(&q, 0, sizeof q);
    q.need.semantic_operation = op;
    q.need.accepted_input_types = ~0ull;
    q.need.effect_class = 0x1FF;
    q.need.authority_ceiling.resource_hi = UINT64_MAX;
    q.need.authority_ceiling.rights = RX_RIGHT_READ | RX_RIGHT_WRITE | RX_RIGHT_EFFECT;
    q.t.n_order = 2;
    q.t.order[0] = CQ_DIM_COST;
    q.t.order[1] = CQ_DIM_LOCALITY;
    q.t.k = CQ_MAX_K;
    return q;
}

static uint64_t sk_scale(const uint64_t *in, uint32_t n, uint32_t attempt, int *failed) {
    (void)attempt;
    *failed = 0;
    return (n ? in[0] : 0) * 3 + 1;
}
static uint64_t sk_other(const uint64_t *in, uint32_t n, uint32_t attempt, int *failed) {
    (void)attempt;
    *failed = 0;
    return (n ? in[0] : 0) + 1000;
}

static AgSkillTable g_skills;

static void skills_init(void) {
    memset(&g_skills, 0, sizeof g_skills);
    g_skills.n = 2;
    g_skills.skill[0] = (AgSkill){ 7, sk_scale, { 0x07 } };
    g_skills.skill[1] = (AgSkill){ 9, sk_other, { 0x09 } };
}

static SrSkill skill(uint32_t id, uint32_t version, uint8_t digest0, uint32_t machine) {
    SrSkill s;
    memset(&s, 0, sizeof s);
    s.skill_id = id;
    s.version = version;
    s.digest[0] = digest0;
    s.machine = machine;
    return s;
}

/* ---- register / update / availability / withdraw, machine identity ---- */

static void t_lifecycle(void) {
    printf("[*] register, update in place, availability, withdraw, canonical machines\n");
    AienMachineId slots[8], a = mid(1), b = mid(2);
    AienMachineIndex ix;
    aien_mid_index_init(&ix, slots, 8);
    CqCatalog c;
    CHECK(cq_catalog_init_canonical(&c, &ix, &a, 8, 8) == CQ_OK, "canonical catalog");
    AienMachineId got;
    CHECK(cq_machine_identity(&c, c.self_machine, &got) == CQ_OK && aien_mid_equal(&got, &a),
          "self index resolves to the canonical identity");
    uint32_t bi = 0;
    CHECK(cq_machine_advertise_id(&c, &b, NOW_US + 1000, &bi) == CQ_OK && bi != c.self_machine &&
          cq_machine_index(&c, &b) == bi, "remote machine bound by identity");
    ontology(&c);

    CqEntry e = provide(10, OP_SCALE, 100, 50);
    e.source = CQ_SRC_GRAPH;
    e.machine_id = c.self_machine;
    CHECK(cq_register(&c, &e, "x", 1) == CQ_OK && c.n == 1, "register");
    CHECK(cq_catalog_build(&c) == CQ_OK, "build");
    SrRouter r = { &c, &g_skills };
    SrRequirement q = requirement(ALIAS_SCALE);
    CqPlan p;
    CqResult res;
    cq_compile(&c, &q.need, &p);
    CHECK(cq_query(&c, &p, &q.need, &q.t, NULL, NOW_US, &res, NULL) == CQ_OK && res.n == 1 &&
          res.cand[0].expected_cost == 50, "query by alias finds the capability");

    e.cost = 20;
    e.generation = 2;
    CHECK(cq_register(&c, &e, NULL, 0) == CQ_OK && c.n == 1 && c.built, "update in place keeps the index");
    CqKey k = cq_key_of(&e);
    CHECK(cq_lookup(&c, &k)->desc_len == 1, "update without text keeps the description");
    cq_query(&c, &p, &q.need, &q.t, NULL, NOW_US, &res, NULL);
    CHECK(res.n == 1 && res.cand[0].expected_cost == 20, "the update is what the query sees");

    CqStats st;
    CHECK(cq_set_availability(&c, &k, CQ_LIVE_UNAVAILABLE) == CQ_OK && c.built, "unavailable");
    cq_query(&c, &p, &q.need, &q.t, NULL, NOW_US, &res, &st);
    CHECK(res.n == 0 && st.rejected_unavailable == 1, "an unavailable provider is not returned");
    CHECK(cq_set_availability(&c, &k, CQ_LIVE_AVAILABLE) == CQ_OK, "available again");
    cq_query(&c, &p, &q.need, &q.t, NULL, NOW_US, &res, NULL);
    CHECK(res.n == 1, "back");
    CHECK(cq_withdraw(&c, &k) == CQ_OK, "withdraw");
    cq_query(&c, &p, &q.need, &q.t, NULL, NOW_US, &res, &st);
    CHECK(res.n == 0 && st.rejected_dead == 1, "a withdrawn provider is not returned");
    CHECK(cq_set_availability(&c, &k, CQ_LIVE_AVAILABLE) == CQ_E_WITHDRAWN,
          "availability cannot revive a withdrawal");
    CqKey nk = { 99, 1, c.self_machine, 0 };
    CHECK(cq_withdraw(&c, &nk) == CQ_E_NOT_FOUND, "unknown key");
    e.generation = 3;
    CHECK(cq_register(&c, &e, NULL, 0) == CQ_OK && c.n == 1, "registering again revives it");
    cq_query(&c, &p, &q.need, &q.t, NULL, NOW_US, &res, NULL);
    CHECK(res.n == 1, "revived");
    e.op = OP_SUMMARIZE;
    CHECK(cq_register(&c, &e, NULL, 0) == CQ_OK && !c.built, "moving operations needs a rebuild");
    CHECK(cq_query(&c, &p, &q.need, &q.t, NULL, NOW_US, &res, NULL) == CQ_E_UNBUILT, "unbuilt refused");

    SrRoute out;
    CHECK(sr_route(&r, &q, NULL, NOW_US, &out) == SR_E_QUERY, "router reports the unbuilt graph");
    cq_catalog_free(&c);

    CqCatalog plain;
    CHECK(cq_catalog_init(&plain, 1, 4, 4) == CQ_OK, "index-only catalog");
    CHECK(cq_machine_identity(&plain, 1, &got) == CQ_E_NO_IDENTITY &&
          cq_machine_advertise_id(&plain, &b, 1, NULL) == CQ_E_NO_IDENTITY,
          "no identity table: no canonical identity");
    cq_catalog_free(&plain);
}

/* ---- Skill -> capabilities, routing, authority, no-candidate, determinism ---- */

static AienosCapAdmin *g_admin;

static RxCapRef mint(uint32_t subject, uint64_t resource, uint32_t rights) {
    AienosCapRef office;
    aienos_cap_office(g_admin, &office);
    AienosCapMint m = { ISSUER, subject, resource, rights, 0, { UINT32_MAX, 0 }, office };
    AienosCapRef ref = { UINT32_MAX, 0 };
    if (aienos_cap_mint(g_admin, &m, &ref) != 0) ref = (AienosCapRef){ UINT32_MAX, 0 };
    return (RxCapRef){ ref.cap_id, ref.generation };
}

/* Node A's graph: skill 7 (scale, digest 0x07, matches the table), skill 8
 * (scale, cheaper, but this machine has no procedure 8), skill 9 (scale,
 * cheapest, advertised digest differs from the executable one), a native
 * (non-Skill) provider, and machine B's skill 11 (scale, remote). */
static void graph_a(CqCatalog *c, AienMachineIndex *ix, AienMachineId *slots, int reverse) {
    AienMachineId a = mid(1), b = mid(2);
    aien_mid_index_init(ix, slots, 8);
    CHECK(cq_catalog_init_canonical(c, ix, &a, 8, 8) == CQ_OK, "catalog A");
    uint32_t bi = 0;
    CHECK(cq_machine_advertise_id(c, &b, NOW_US + 1000000, &bi) == CQ_OK, "B leased");
    ontology(c);
    SrSkill s7 = skill(7, 1, 0x07, c->self_machine), s8 = skill(8, 1, 0x08, c->self_machine),
            s9 = skill(9, 1, 0x99, c->self_machine), s11 = skill(11, 4, 0x11, bi);
    CqEntry p7[2] = { provide(1, OP_SCALE, 70, 40), provide(2, OP_SUMMARIZE, 71, 40) };
    CqEntry p7h = provide(3, OP_LOCAL_ONLY, 72, 40);
    p7h.auth_resource = RES_HELD;
    p7h.auth_rights = RX_RIGHT_READ;
    CqEntry p8 = provide(4, OP_SCALE, 80, 30);
    CqEntry p9 = provide(5, OP_SCALE, 90, 10);
    CqEntry p9a = provide(6, OP_LOCAL_ONLY, 91, 10);
    p9a.auth_resource = RES_ASK;
    p9a.auth_rights = RX_RIGHT_READ;
    CqEntry p11 = provide(7, OP_SCALE_FAST, 110, 5);
    CqEntry nat = provide(8, OP_SCALE, 120, 1);
    nat.source = CQ_SRC_GRAPH;
    nat.machine_id = c->self_machine;
    for (int pass = 0; pass < 2; pass++) {
        if ((pass == 0) != (reverse == 0)) {
            CHECK(sr_register_skill(c, &s7, p7, 2) == SR_OK && sr_register_skill(c, &s7, &p7h, 1) == SR_OK,
                  "skill 7");
            CHECK(sr_register_skill(c, &s8, &p8, 1) == SR_OK, "skill 8");
        } else {
            CHECK(cq_register(c, &nat, NULL, 0) == CQ_OK, "native provider");
            CHECK(sr_register_skill(c, &s11, &p11, 1) == SR_OK, "remote skill 11");
            CHECK(sr_register_skill(c, &s9, &p9, 1) == SR_OK && sr_register_skill(c, &s9, &p9a, 1) == SR_OK,
                  "skill 9");
        }
    }
    CHECK(cq_catalog_build(c) == CQ_OK, "build A");
}

static void t_routing(void) {
    printf("[*] Skill -> capabilities; requirement -> admissible Skill target; authority; determinism\n");
    AienosCapView *view;
    CHECK(aienos_cap_start(&g_admin, &view) == 0, "authority");
    RxWorld w;
    CHECK(rx_world_init_native(&w, view, 2, 1u << 14) == RX_OK, "world");
    w.external_subject = SUBJ_EXTERNAL;
    AgCapTable caps;
    memset(&caps, 0, sizeof caps);
    caps.subject = SUBJ_PLAN;
    caps.cap[0].ref = mint(SUBJ_PLAN, RES_HELD, RX_RIGHT_READ);
    caps.cap[0].resource = RES_HELD;
    caps.cap[0].rights = RX_RIGHT_READ;
    caps.cap[1].ref = (RxCapRef){ 4000, 1 };          /* listed, never minted */
    caps.cap[1].resource = RES_ASK;
    caps.cap[1].rights = RX_RIGHT_READ;
    caps.n = 2;
    CqHeld held = { &w, &caps };

    AienMachineId slots[8], slots2[8], b = mid(2);
    AienMachineIndex ix, ix2;
    CqCatalog c, c2;
    graph_a(&c, &ix, slots, 0);
    graph_a(&c2, &ix2, slots2, 1);
    SrRouter r = { &c, &g_skills }, r2 = { &c2, &g_skills };

    CqKey keys[8];
    CHECK(sr_skill_capabilities(&c, 7, 1, keys, 8) == 3, "skill 7 provides three capabilities");
    CHECK(sr_skill_capabilities(&c, 7, 2, keys, 8) == 0, "no such version");
    CHECK(sr_skill_capabilities(&c, 11, 4, keys, 8) == 1 && keys[0].machine_id == cq_machine_index(&c, &b),
          "the remote Skill's capability is on machine B");

    /* Fabric allowed: the cheapest admissible provider is B's skill 11
     * (scale.fast specializes scale). Skill 9 is cheaper locally but its
     * advertised digest is not the procedure this machine runs; skill 8 has
     * no procedure here; the native provider is not a Skill. */
    SrRequirement q = requirement(OP_SCALE);
    SrRoute out, out2;
    CHECK(sr_route(&r, &q, &held, NOW_US, &out) == SR_E_REMOTE && out.chosen.capability_id == 7 &&
          out.target_known && aien_mid_equal(&out.target, &b) && out.skill_version == 4 &&
          out.skill_digest[0] == 0x11, "remote winner returned with B's canonical identity");
    CHECK(out.rejected.not_a_skill == 1 && out.rejected.not_executable == 1 &&
          out.rejected.digest_mismatch == 1 && out.n_admissible == 2,
          "rejections counted: native %llu, missing %llu, digest %llu, admissible %u",
          (unsigned long long)out.rejected.not_a_skill, (unsigned long long)out.rejected.not_executable,
          (unsigned long long)out.rejected.digest_mismatch, out.n_admissible);

    /* Local only: skill 7. Same answer from the other registration order. */
    q.local_only = 1;
    CHECK(sr_route(&r, &q, &held, NOW_US, &out) == SR_OK && out.chosen.skill_id == 7 &&
          out.chosen.capability_id == 1 && !out.remote, "local only: skill 7");
    CHECK(sr_route(&r2, &q, &held, NOW_US, &out2) == SR_OK && out2.chosen.skill_id == 7 &&
          !memcmp(&out.chosen, &out2.chosen, sizeof out.chosen) &&
          !memcmp(out.plan_digest, out2.plan_digest, 32) && !memcmp(&out.target, &out2.target, sizeof out.target),
          "registration order does not change the route");
    for (int i = 0; i < 50; i++) {
        sr_route(&r, &q, &held, NOW_US, &out2);
        if (memcmp(&out.chosen, &out2.chosen, sizeof out.chosen)) { CHECK(0, "route changed on run %d", i); break; }
    }

    /* Authority. OP_LOCAL_ONLY: skill 9's provider (cheaper) needs RES_ASK,
     * whose table entry is forged; skill 7's needs RES_HELD, which is held. */
    SrRoute oa;
    g_skills.skill[1].identity[0] = 0x99;              /* make skill 9 executable as advertised */
    SrRequirement qa = requirement(OP_LOCAL_ONLY);
    CHECK(sr_route(&r, &qa, &held, NOW_US, &oa) == SR_OK && oa.chosen.skill_id == 9 &&
          oa.chosen.required_authority.held == 0, "without the filter: the cheaper, unheld one");
    qa.t.require_held = 1;
    CHECK(sr_route(&r, &qa, &held, NOW_US, &oa) == SR_OK && oa.chosen.skill_id == 7 &&
          oa.chosen.required_authority.held == 1 && oa.stats.rejected_authority == 1,
          "require_held: the forged authority is refused by the view, skill 7 wins");
    qa.need.authority_ceiling.rights = 0;
    CHECK(sr_route(&r, &qa, &held, NOW_US, &oa) == SR_E_NO_CANDIDATE,
          "ceiling without rights: no candidate");
    g_skills.skill[1].identity[0] = 0x09;

    /* No candidate. */
    SrRequirement qn = requirement(OP_SUMMARIZE);
    qn.need.required_output_types = 0x4;
    CHECK(sr_route(&r, &qn, &held, NOW_US, &out) == SR_E_NO_CANDIDATE && out.n_admissible == 0,
          "nothing produces that type");
    SrRequirement qu = requirement(777);
    CHECK(sr_route(&r, &qu, &held, NOW_US, &out) == SR_E_QUERY && out.query_verdict == CQ_E_NO_OP,
          "unknown capability");
    SrRequirement ql = requirement(OP_SCALE);
    CqKey k7 = { 1, 70, c.self_machine, 7 };
    CHECK(cq_withdraw(&c, &k7) == CQ_OK, "withdraw skill 7's scale");
    ql.local_only = 1;
    CHECK(sr_route(&r, &ql, &held, NOW_US, &out) == SR_E_NO_CANDIDATE &&
          out.rejected.digest_mismatch == 1 && out.rejected.not_executable == 1,
          "only inadmissible local providers left: no candidate");
    CHECK(sr_skill_capabilities(&c, 7, 1, keys, 8) == 2, "skill 7 now provides two");
    ql.local_only = 0;
    CHECK(sr_route(&r, &ql, &held, NOW_US, &out) == SR_E_REMOTE && out.chosen.skill_id == 11,
          "B still serves it while its lease holds");
    CHECK(sr_route(&r, &ql, &held, NOW_US + 2000000, &out) == SR_E_NO_CANDIDATE &&
          out.stats.rejected_dead >= 1, "after B's lease ends nothing remote either");

    cq_catalog_free(&c);
    cq_catalog_free(&c2);
    rx_world_wait_quiescent(&w, 30000);
    rx_world_destroy(&w);
    aienos_cap_stop(g_admin, view);
}

/* ---- wire form between two catalogs ---- */

static void t_wire(void) {
    printf("[*] wire form: B advertises to A by canonical identity\n");
    AienMachineId sa[8], sb[8], a = mid(1), b = mid(2);
    AienMachineIndex ia, ib;
    aien_mid_index_init(&ia, sa, 8);
    aien_mid_index_init(&ib, sb, 8);
    /* Bind something first on A so B's index differs between the two catalogs. */
    AienMachineId other = mid(9);
    aien_mid_index_bind(&ia, &other);
    CqCatalog ca, cb;
    CHECK(cq_catalog_init_canonical(&ca, &ia, &a, 8, 8) == CQ_OK, "A");
    CHECK(cq_catalog_init_canonical(&cb, &ib, &b, 8, 8) == CQ_OK, "B");
    ontology(&ca);
    ontology(&cb);
    uint8_t da[32], db[32];
    cq_ontology_digest(&ca, da);
    cq_ontology_digest(&cb, db);
    CHECK(!memcmp(da, db, 32), "same operation catalog, same digest");

    SrSkill s = skill(11, 4, 0x11, cb.self_machine);
    CqEntry p = provide(7, OP_SCALE_FAST, 110, 5);
    p.auth_resource = 0x77;
    p.auth_rights = RX_RIGHT_READ;
    p.generation = 5;
    CHECK(sr_register_skill(&cb, &s, &p, 1) == SR_OK, "B registers its skill");
    CqKey kb = { 7, 110, cb.self_machine, 11 };
    const CqEntry *eb = cq_lookup(&cb, &kb);
    uint8_t rec[CQ_WIRE_BYTES];
    CHECK(eb && cq_wire_encode(&cb, eb, CQ_WIRE_ADVERTISE, rec) == CQ_OK, "B encodes");

    CqKey ka;
    CHECK(cq_wire_apply(&ca, rec, sizeof rec, &ka) == CQ_OK, "A ingests");
    const CqEntry *ea = cq_lookup(&ca, &ka);
    AienMachineId got;
    CHECK(ea && ea->source == CQ_SRC_FABRIC && ka.machine_id != cb.self_machine &&
          cq_machine_identity(&ca, ka.machine_id, &got) == CQ_OK && aien_mid_equal(&got, &b),
          "on A the entry is Fabric, on A's own index for B's identity");
    CHECK(ea && ea->skill_id == 11 && ea->skill_version == 4 && ea->skill_digest[0] == 0x11 &&
          ea->cost == 5 && ea->auth_resource == 0x77 && ea->auth_rights == RX_RIGHT_READ &&
          ea->evidence_ref == eb->evidence_ref && ea->generation == 5 && ea->op == OP_SCALE_FAST,
          "fields survive the trip");
    uint8_t rec2[CQ_WIRE_BYTES];
    CHECK(ea && cq_wire_encode(&ca, ea, CQ_WIRE_ADVERTISE, rec2) == CQ_E_WIRE,
          "A will not re-advertise B's capability");
    CHECK(cq_wire_apply(&cb, rec, sizeof rec, NULL) == CQ_E_WIRE, "B refuses a record naming itself");
    CHECK(cq_wire_apply(&ca, rec, sizeof rec, NULL) == CQ_E_STALE, "replayed generation is stale");

    uint8_t bad[CQ_WIRE_BYTES + 1];
    memcpy(bad, rec, sizeof rec);
    CHECK(cq_wire_apply(&ca, bad, CQ_WIRE_BYTES - 1, NULL) == CQ_E_WIRE, "short record");
    CHECK(cq_wire_apply(&ca, bad, CQ_WIRE_BYTES + 1, NULL) == CQ_E_WIRE, "long record");
    bad[130] ^= 1;
    CHECK(cq_wire_apply(&ca, bad, CQ_WIRE_BYTES, NULL) == CQ_E_WIRE, "flipped bit fails the check");
    memcpy(bad, rec, sizeof rec);
    bad[0] = 'X';
    CHECK(cq_wire_apply(&ca, bad, CQ_WIRE_BYTES, NULL) == CQ_E_WIRE, "bad magic");

    /* Unknown operation on the receiver. */
    CqCatalog cc;
    AienMachineId sc[4], cself = mid(3);
    AienMachineIndex ic;
    aien_mid_index_init(&ic, sc, 4);
    CHECK(cq_catalog_init_canonical(&cc, &ic, &cself, 4, 4) == CQ_OK, "C");
    CHECK(cq_op_define(&cc, 1, 0, CQ_SRC(CQ_SRC_FABRIC)) == CQ_OK, "C knows one op");
    uint8_t dc[32];
    cq_ontology_digest(&cc, dc);
    CHECK(memcmp(dc, db, 32) != 0, "different catalog, different digest");
    CHECK(cq_wire_apply(&cc, rec, sizeof rec, NULL) == CQ_E_NO_OP, "C does not know scale.fast");
    cq_catalog_free(&cc);

    /* Availability and withdrawal travel the same way. */
    CHECK(cq_catalog_build(&ca) == CQ_OK, "A build");
    CqEntry x = *eb;
    x.generation = 6;
    x.live = CQ_LIVE_UNAVAILABLE;
    CHECK(cq_wire_encode(&cb, &x, CQ_WIRE_AVAILABILITY, rec) == CQ_OK &&
          cq_wire_apply(&ca, rec, sizeof rec, NULL) == CQ_OK && cq_lookup(&ca, &ka)->live == CQ_LIVE_UNAVAILABLE &&
          ca.built, "availability update, index kept");
    x.generation = 7;
    CHECK(cq_wire_encode(&cb, &x, CQ_WIRE_WITHDRAW, rec) == CQ_OK &&
          cq_wire_apply(&ca, rec, sizeof rec, NULL) == CQ_OK && cq_lookup(&ca, &ka)->live == CQ_LIVE_WITHDRAWN,
          "withdrawal");
    x.generation = 8;
    x.live = CQ_LIVE_AVAILABLE;
    CHECK(cq_wire_encode(&cb, &x, CQ_WIRE_AVAILABILITY, rec) == CQ_OK &&
          cq_wire_apply(&ca, rec, sizeof rec, NULL) == CQ_E_WITHDRAWN, "availability cannot revive");
    CHECK(cq_wire_encode(&cb, &x, CQ_WIRE_ADVERTISE, rec) == CQ_OK &&
          cq_wire_apply(&ca, rec, sizeof rec, NULL) == CQ_OK && cq_lookup(&ca, &ka)->live == CQ_LIVE_AVAILABLE,
          "a new advertisement does");

    CqCatalog plain;
    CHECK(cq_catalog_init(&plain, 1, 4, 4) == CQ_OK, "index-only");
    CHECK(cq_wire_apply(&plain, rec, sizeof rec, NULL) == CQ_E_NO_IDENTITY, "index-only catalog cannot ingest");
    cq_catalog_free(&plain);
    cq_catalog_free(&ca);
    cq_catalog_free(&cb);
}

/* ---- one real runtime route: an action graph that names a capability ---- */

static void t_runtime_route(void) {
    printf("[*] runtime: an action graph's skill node is routed by capability and runs on the World\n");
    AienosCapView *view;
    CHECK(aienos_cap_start(&g_admin, &view) == 0, "authority");
    RxWorld w;
    CHECK(rx_world_init_native(&w, view, 2, 1u << 14) == RX_OK, "world");
    w.external_subject = SUBJ_EXTERNAL;
    AgCapTable caps;
    memset(&caps, 0, sizeof caps);
    caps.subject = SUBJ_PLAN;
    caps.cap[0].ref = mint(SUBJ_PLAN, RES_HELD, RX_RIGHT_READ);
    caps.cap[0].resource = RES_HELD;
    caps.cap[0].rights = RX_RIGHT_READ;
    caps.n = 1;
    CqHeld held = { &w, &caps };

    AienMachineId slots[8];
    AienMachineIndex ix;
    CqCatalog c;
    graph_a(&c, &ix, slots, 0);
    SrRouter r = { &c, &g_skills };

    /* The template states no implementation: node s is AG_SKILL op 0. */
    static AgGraph tmpl;
    rx_graph_init(&tmpl, 0);
    int k = rx_graph_node(&tmpl, AG_CONST, AG_T_U64);
    tmpl.nodes[k].imm = 14;
    int s = rx_graph_node(&tmpl, AG_SKILL, AG_T_U64);
    int v = rx_graph_node(&tmpl, AG_VERIFY, AG_T_VERDICT);
    tmpl.nodes[v].imm = 0;
    tmpl.nodes[v].imm2 = 1000;
    rx_graph_data(&tmpl, (uint32_t)k, (uint32_t)s, 0, AG_EDGE_DATA);
    rx_graph_data(&tmpl, (uint32_t)s, (uint32_t)v, 0, AG_EDGE_DATA);
    rx_graph_success(&tmpl, (uint32_t)v, AG_OK);
    CHECK(tmpl.nodes[s].op == 0, "unbound skill node");

    SrRequirement q = requirement(ALIAS_SCALE);       /* "I need scale", by alias */
    q.local_only = 1;
    q.t.require_held = 1;
    SrRoute route;
    CHECK(sr_bind_node(&r, &tmpl, (uint32_t)s, &q, &held, NOW_US, &route) == SR_OK &&
          tmpl.nodes[s].op == 7, "the router bound the node to skill 7 (route %d)", route.verdict);

    AgLibrary lib = { 1, { { 1, &tmpl } } };
    AgGoal goal = { 1, 0, { { 0, 0 } } };
    AgConstraints cons;
    memset(&cons, 0, sizeof cons);
    cons.max_effects = UINT32_MAX;
    cons.budget.slots = RX_MAX_REACTIONS;
    cons.budget.memory_bytes = UINT64_MAX;
    cons.budget.energy_budget = UINT64_MAX;
    cons.budget.offered_locality = UINT32_MAX;
    cons.budget.offered_accel = UINT32_MAX;
    cons.budget.compute_mask = UINT32_MAX;
    static AgGraph g;
    AgReport rep;
    int rc = rx_graph_compile(&goal, &w, &caps, &cons, &lib, 1, &g, &rep);
    CHECK(rc == AG_OK_READY && rep.n_missing == 0, "compile %d, missing %u", rc, rep.n_missing);
    RxCapRef cell = mint(SUBJ_PLAN, RES_CELL, RX_RIGHT_READ | RX_RIGHT_WRITE);
    RxCapRef runc = mint(SUBJ_PLAN, RES_RUN, RX_RIGHT_READ);
    RxCapRef ext = mint(SUBJ_EXTERNAL, RES_RUN, RX_RIGHT_WRITE);
    static AgLowered L;
    CHECK(rx_graph_lower(&L, &w, &g, &g_skills, &caps, RES_CELL, cell, RES_RUN, runc, NULL) == 0, "lower");
    CHECK(rx_graph_start(&L, ext, 1) > 0, "start");
    CHECK(rx_world_wait_quiescent(&w, 30000) == RX_OK, "settle");
    AgResult res;
    rx_graph_collect(&L, 1, &res);
    int sn = -1;
    for (uint32_t i = 0; i < g.n_nodes; i++)
        if (g.nodes[i].alive && g.nodes[i].kind == AG_SKILL) sn = (int)i;
    CHECK(res.outcome == AG_RUN_SUCCESS && sn >= 0 && res.value[sn] == 14 * 3 + 1,
          "the World ran the routed Skill (outcome %d, value %llu)", res.outcome,
          sn >= 0 ? (unsigned long long)res.value[sn] : 0ull);

    cq_catalog_free(&c);
    rx_world_wait_quiescent(&w, 30000);
    rx_world_destroy(&w);
    aienos_cap_stop(g_admin, view);
}

int main(void) {
    skills_init();
    t_lifecycle();
    t_routing();
    t_wire();
    t_runtime_route();
    printf("checks %d failures %d\n", g_checks, g_fail);
    if (g_fail == 0) printf("M20_CAPABILITY_GRAPH_PASS\n");
    return g_fail ? 1 : 0;
}
