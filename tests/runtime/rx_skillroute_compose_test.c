/*
 * COMPOSITION-2 WP-D: the Skill Router end to end and failing closed.
 *
 * One real requirement is routed from a CqNeed through cq_compile and the
 * canonical Capability Graph to a Skill, bound into an action-graph node
 * (skill id, version, digest, generation proven) and run on the World. Then
 * each failure mode must fail closed with its specific verdict and leave the
 * node unbound:
 *   (a) provider withdrawn            (b) stale generation (held route, upsert)
 *   (c) provider unavailable          (d) machine mismatch / unknown machine
 *   (e) skill digest / version pin    (f) authority not held
 * plus sr_route_alternatives (>= 2 ranked, out[0] == sr_route, dominated
 * included). The no-mint symbol check on rx_skillroute.o runs in the build.
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
enum { OP_SCALE = 1, OP_SCALE_FAST = 2, OP_SUMMARIZE = 3, ALIAS_SCALE = 501 };
enum { SUBJ_EXTERNAL = 100, ISSUER = 3, SUBJ_PLAN = 61 };
enum { UNBOUND_MACHINE = 1000 };        /* a graph index with no canonical identity */
#define RES_CELL 0xA000001ull
#define RES_RUN  0xA000002ull
#define RES_HELD 0xB000001ull

static AienMachineId mid(uint8_t seed) {
    AienMachineId m;
    uint8_t root[8] = { 'n', 'o', 'd', 'e', seed, 0, 0, 0 };
    aien_mid_derive(AIEN_MID_ROOT_PROVISIONED, root, sizeof root, &m);
    return m;
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

static CqEntry provide(uint32_t cap, uint32_t op, uint32_t real, uint64_t cost, uint64_t lat) {
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
    e.latency_us = lat;
    e.energy_uj = 100;
    e.auth_resource = RES_HELD;
    e.auth_rights = RX_RIGHT_READ;
    e.live = CQ_LIVE_AVAILABLE;
    e.generation = 1;
    return e;
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

/* The graph: A (self) has skill 7 (scale, cost 10, latency 100, digest 0x07
 * = the executable one) and skill 9 (scale, cost 20, latency 200, digest
 * 0x09: dominated by 7 on both named dimensions). Machine B (leased) has
 * skill 11 (scale.fast, cost 30). Machine C is leased and bound but provides
 * nothing. Index UNBOUND_MACHINE is leased by index only (no identity) and
 * provides skill 12 (summarize). */
typedef struct {
    AienMachineId slots[8];
    AienMachineIndex ix;
    CqCatalog c;
    SrRouter r;
    uint32_t b, cm;
} Fx;

static CqEntry p7(void) { return provide(1, OP_SCALE, 70, 10, 100); }

static void fx_init(Fx *f) {
    AienMachineId a = mid(1), b = mid(2), cm = mid(3);
    aien_mid_index_init(&f->ix, f->slots, 8);
    CHECK(cq_catalog_init_canonical(&f->c, &f->ix, &a, 8, 8) == CQ_OK, "catalog");
    CHECK(cq_machine_advertise_id(&f->c, &b, NOW_US + 1000000, &f->b) == CQ_OK, "B leased");
    CHECK(cq_machine_advertise_id(&f->c, &cm, NOW_US + 1000000, &f->cm) == CQ_OK, "C leased");
    CHECK(cq_machine_advertise(&f->c, UNBOUND_MACHINE, NOW_US + 1000000) == CQ_OK, "index-only lease");
    uint32_t all = CQ_SRC(CQ_SRC_GRAPH) | CQ_SRC(CQ_SRC_SKILL) | CQ_SRC(CQ_SRC_FABRIC);
    CHECK(cq_op_define(&f->c, OP_SCALE, 0, all) == CQ_OK && cq_op_define(&f->c, OP_SCALE_FAST, OP_SCALE, all) == CQ_OK &&
          cq_op_define(&f->c, OP_SUMMARIZE, 0, all) == CQ_OK && cq_alias(&f->c, ALIAS_SCALE, OP_SCALE) == CQ_OK,
          "ontology");
    SrSkill s7 = skill(7, 1, 0x07, f->c.self_machine), s9 = skill(9, 1, 0x09, f->c.self_machine),
            s11 = skill(11, 4, 0x11, f->b), s12 = skill(12, 1, 0x12, UNBOUND_MACHINE);
    CqEntry e7 = p7(), e9 = provide(2, OP_SCALE, 90, 20, 200), e11 = provide(3, OP_SCALE_FAST, 110, 30, 300),
            e12 = provide(4, OP_SUMMARIZE, 120, 5, 50);
    CHECK(sr_register_skill(&f->c, &s7, &e7, 1) == SR_OK && sr_register_skill(&f->c, &s9, &e9, 1) == SR_OK &&
          sr_register_skill(&f->c, &s11, &e11, 1) == SR_OK && sr_register_skill(&f->c, &s12, &e12, 1) == SR_OK,
          "skills registered");
    CHECK(cq_catalog_build(&f->c) == CQ_OK, "build");
    f->r = (SrRouter){ &f->c, &g_skills };
}

static SrRequirement requirement(uint32_t op) {
    SrRequirement q;
    memset(&q, 0, sizeof q);
    q.need.semantic_operation = op;
    q.need.accepted_input_types = ~0ull;
    q.need.required_output_types = 0x2;
    q.need.effect_class = CQ_FX_PURE;
    q.need.authority_ceiling.resource_hi = UINT64_MAX;
    q.need.authority_ceiling.rights = RX_RIGHT_READ;
    q.t.n_order = 2;
    q.t.order[0] = CQ_DIM_COST;
    q.t.order[1] = CQ_DIM_LATENCY;
    q.t.k = CQ_MAX_K;
    q.t.require_held = 1;
    q.local_only = 1;
    q.need.locality_constraints.allowed = CQ_LOC_LOCAL;   /* B's skill never enters the query */
    return q;
}

/* An action-graph template with one unbound skill node. */
static AgGraph g_tmpl;
static int tmpl(void) {
    rx_graph_init(&g_tmpl, 0);
    int k = rx_graph_node(&g_tmpl, AG_CONST, AG_T_U64);
    g_tmpl.nodes[k].imm = 14;
    int s = rx_graph_node(&g_tmpl, AG_SKILL, AG_T_U64);
    int v = rx_graph_node(&g_tmpl, AG_VERIFY, AG_T_VERDICT);
    g_tmpl.nodes[v].imm = 0;
    g_tmpl.nodes[v].imm2 = 1000;
    rx_graph_data(&g_tmpl, (uint32_t)k, (uint32_t)s, 0, AG_EDGE_DATA);
    rx_graph_data(&g_tmpl, (uint32_t)s, (uint32_t)v, 0, AG_EDGE_DATA);
    rx_graph_success(&g_tmpl, (uint32_t)v, AG_OK);
    return s;
}
static int unbound(int s) { return g_tmpl.nodes[s].op == 0 && g_tmpl.n_auth == 0; }

static AienosCapAdmin *g_admin;
static RxWorld g_w;
static AgCapTable g_caps, g_caps_none;

static RxCapRef mint(uint32_t subject, uint64_t resource, uint32_t rights) {
    AienosCapRef office;
    aienos_cap_office(g_admin, &office);
    AienosCapMint m = { ISSUER, subject, resource, rights, 0, { UINT32_MAX, 0 }, office };
    AienosCapRef ref = { UINT32_MAX, 0 };
    if (aienos_cap_mint(g_admin, &m, &ref) != 0) ref = (AienosCapRef){ UINT32_MAX, 0 };
    return (RxCapRef){ ref.cap_id, ref.generation };
}

/* ---- end to end ---- */

static void t_end_to_end(void) {
    printf("[*] requirement -> cq_compile -> providers -> Skill -> bound node -> runs on the World\n");
    Fx f;
    fx_init(&f);
    CqHeld held = { &g_w, &g_caps };
    SrRequirement q = requirement(ALIAS_SCALE);

    CqPlan plan;
    CHECK(cq_compile(&f.c, &q.need, &plan) == CQ_OK && plan.op == OP_SCALE && plan.n_ops == 2,
          "need compiles to scale + scale.fast (op %u, n_ops %u)", plan.op, plan.n_ops);
    CqResult res;
    CqTradeoffs t = q.t;
    CHECK(cq_query(&f.c, &plan, &q.need, &t, &held, NOW_US, &res, NULL) == CQ_OK && res.n_feasible == 2,
          "graph has two local providers of the capability (%u)", res.n_feasible);

    int s = tmpl();
    SrRoute route;
    CHECK(sr_bind_node(&f.r, &g_tmpl, (uint32_t)s, &q, &held, NOW_US, &route) == SR_OK, "bind (%d)",
          route.verdict);
    CHECK(g_tmpl.nodes[s].op == 7 && route.chosen.skill_id == 7 && route.skill_version == 1 &&
          route.skill_digest[0] == 0x07 && memcmp(route.skill_digest, g_skills.skill[0].identity, 32) == 0 &&
          route.generation == 1 && route.key.skill_id == 7 && route.key.capability_id == 1 &&
          route.key.machine_id == f.c.self_machine && !route.remote && route.target_known,
          "node bound to skill 7 v1, digest = executable identity, generation 1");
    CHECK(g_tmpl.n_auth == 1 && g_tmpl.auth[0].node == (uint16_t)s && g_tmpl.auth[0].resource == RES_HELD &&
          g_tmpl.auth[0].rights == RX_RIGHT_READ, "bound node declares the provider's authority");
    CHECK(memcmp(route.plan_digest, plan.digest, 32) == 0, "route carries the compiled plan identity");
    CHECK(sr_route_check(&f.r, &route, NOW_US) == SR_OK, "held route still valid");

    AgLibrary lib = { 1, { { 1, &g_tmpl } } };
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
    int rc = rx_graph_compile(&goal, &g_w, &g_caps, &cons, &lib, 1, &g, &rep);
    CHECK(rc == AG_OK_READY && rep.n_missing == 0, "compile %d, missing %u", rc, rep.n_missing);
    RxCapRef cell = mint(SUBJ_PLAN, RES_CELL, RX_RIGHT_READ | RX_RIGHT_WRITE);
    RxCapRef runc = mint(SUBJ_PLAN, RES_RUN, RX_RIGHT_READ);
    RxCapRef ext = mint(SUBJ_EXTERNAL, RES_RUN, RX_RIGHT_WRITE);
    static AgLowered L;
    CHECK(rx_graph_lower(&L, &g_w, &g, &g_skills, &g_caps, RES_CELL, cell, RES_RUN, runc, NULL) == 0, "lower");
    CHECK(rx_graph_start(&L, ext, 1) > 0, "start");
    CHECK(rx_world_wait_quiescent(&g_w, 30000) == RX_OK, "settle");
    AgResult ar;
    rx_graph_collect(&L, 1, &ar);
    int sn = -1;
    for (uint32_t i = 0; i < g.n_nodes; i++)
        if (g.nodes[i].alive && g.nodes[i].kind == AG_SKILL) sn = (int)i;
    CHECK(ar.outcome == AG_RUN_SUCCESS && sn >= 0 && ar.value[sn] == 14 * 3 + 1,
          "the World ran skill 7 (outcome %d, value %llu)", ar.outcome,
          sn >= 0 ? (unsigned long long)ar.value[sn] : 0ull);
    cq_catalog_free(&f.c);
}

/* ---- failure modes ---- */

static void t_withdrawn(void) {
    printf("[*] (a) provider withdrawal\n");
    Fx f;
    fx_init(&f);
    CqHeld held = { &g_w, &g_caps };
    SrRequirement q = requirement(OP_SCALE);
    SrRoute held_route, route;
    CHECK(sr_route(&f.r, &q, &held, NOW_US, &held_route) == SR_OK && held_route.chosen.skill_id == 7, "route");
    CqKey k7 = held_route.key;
    CHECK(cq_withdraw(&f.c, &k7) == CQ_OK, "withdraw 7");
    int s = tmpl();
    CHECK(sr_bind_route(&f.r, &g_tmpl, (uint32_t)s, &held_route, NOW_US) == SR_E_WITHDRAWN && unbound(s),
          "held route to a withdrawn provider: SR_E_WITHDRAWN, node unbound");
    CHECK(sr_bind_node(&f.r, &g_tmpl, (uint32_t)s, &q, &held, NOW_US, &route) == SR_OK &&
          g_tmpl.nodes[s].op == 9, "fresh route falls back to skill 9");
    SrRoute r9 = route;
    CHECK(cq_withdraw(&f.c, &r9.key) == CQ_OK, "withdraw 9");
    s = tmpl();
    CHECK(sr_bind_node(&f.r, &g_tmpl, (uint32_t)s, &q, &held, NOW_US, &route) == SR_E_NO_CANDIDATE &&
          route.why == SR_WHY_GONE && route.stats.rejected_dead == 2 && unbound(s),
          "all local providers withdrawn: SR_E_NO_CANDIDATE why GONE (why 0x%x), node unbound", route.why);
    cq_catalog_free(&f.c);
}

static void t_stale(void) {
    printf("[*] (b) stale generation: a route held from before an upsert\n");
    Fx f;
    fx_init(&f);
    CqHeld held = { &g_w, &g_caps };
    SrRequirement q = requirement(OP_SCALE);
    SrRoute held_route, route;
    CHECK(sr_route(&f.r, &q, &held, NOW_US, &held_route) == SR_OK && held_route.generation == 1, "route gen 1");
    CqEntry up = p7();
    up.skill_id = 7;
    up.skill_version = 1;
    up.skill_digest[0] = 0x07;
    up.machine_id = f.c.self_machine;
    up.source = CQ_SRC_SKILL;
    up.generation = 2;
    CHECK(cq_register(&f.c, &up, NULL, 0) == CQ_OK && cq_lookup(&f.c, &held_route.key)->generation == 2,
          "upsert in place to generation 2");
    int s = tmpl();
    CHECK(sr_route_check(&f.r, &held_route, NOW_US) == SR_E_STALE &&
          sr_bind_route(&f.r, &g_tmpl, (uint32_t)s, &held_route, NOW_US) == SR_E_STALE && unbound(s),
          "held generation-1 route: SR_E_STALE, node unbound");
    /* An upsert that changes the procedure (new version and digest). */
    SrRoute held2;
    CHECK(sr_route(&f.r, &q, &held, NOW_US, &held2) == SR_OK && held2.generation == 2, "route gen 2");
    up.skill_version = 2;
    up.skill_digest[0] = 0x77;
    up.generation = 3;
    CHECK(cq_register(&f.c, &up, NULL, 0) == CQ_OK, "upsert v2");
    CHECK(sr_bind_route(&f.r, &g_tmpl, (uint32_t)s, &held2, NOW_US) == SR_E_STALE && unbound(s),
          "held route to v1 after the v2 upsert: SR_E_STALE, node unbound");
    CHECK(sr_route(&f.r, &q, &held, NOW_US, &route) == SR_OK && route.chosen.skill_id == 9 &&
          route.rejected.digest_mismatch == 1, "fresh route refuses 7 (graph digest no longer the executable one)");
    cq_catalog_free(&f.c);
}

static void t_unavailable(void) {
    printf("[*] (c) unavailable provider\n");
    Fx f;
    fx_init(&f);
    CqHeld held = { &g_w, &g_caps };
    SrRequirement q = requirement(OP_SCALE);
    SrRoute held_route, r9, route;
    CHECK(sr_route(&f.r, &q, &held, NOW_US, &held_route) == SR_OK, "route");
    CHECK(cq_set_availability(&f.c, &held_route.key, CQ_LIVE_UNAVAILABLE) == CQ_OK, "7 unavailable");
    int s = tmpl();
    CHECK(sr_bind_route(&f.r, &g_tmpl, (uint32_t)s, &held_route, NOW_US) == SR_E_UNAVAILABLE && unbound(s),
          "held route: SR_E_UNAVAILABLE, node unbound");
    CHECK(sr_route(&f.r, &q, &held, NOW_US, &r9) == SR_OK && r9.chosen.skill_id == 9, "9 still available");
    CHECK(cq_set_availability(&f.c, &r9.key, CQ_LIVE_UNAVAILABLE) == CQ_OK, "9 unavailable");
    CHECK(sr_bind_node(&f.r, &g_tmpl, (uint32_t)s, &q, &held, NOW_US, &route) == SR_E_NO_CANDIDATE &&
          route.why == SR_WHY_UNAVAILABLE && route.stats.rejected_unavailable == 2 && unbound(s),
          "SR_E_NO_CANDIDATE why UNAVAILABLE (why 0x%x), node unbound", route.why);
    cq_catalog_free(&f.c);
}

static void t_machine(void) {
    printf("[*] (d) machine mismatch and unknown machine\n");
    Fx f;
    fx_init(&f);
    CqHeld held = { &g_w, &g_caps };
    SrRoute route;
    int s = tmpl();
    /* Pinned to a machine the graph does not know. */
    SrRequirement q = requirement(OP_SCALE);
    q.local_only = 0;
    q.need.locality_constraints.allowed = 0;
    q.pin_machine_set = 1;
    q.pin_machine = mid(9);
    CHECK(sr_bind_node(&f.r, &g_tmpl, (uint32_t)s, &q, &held, NOW_US, &route) == SR_E_MACHINE && unbound(s),
          "pinned machine unknown: SR_E_MACHINE, node unbound (%d)", route.verdict);
    /* Pinned to C: known, leased, provides nothing; A's and B's providers are on other machines. */
    q.pin_machine = mid(3);
    CHECK(sr_bind_node(&f.r, &g_tmpl, (uint32_t)s, &q, &held, NOW_US, &route) == SR_E_NO_CANDIDATE &&
          route.why == SR_WHY_MACHINE && route.rejected.machine_mismatch == 3 && unbound(s),
          "providers on other machines: SR_E_NO_CANDIDATE why MACHINE (why 0x%x, %llu)", route.why,
          (unsigned long long)route.rejected.machine_mismatch);
    /* Pinned to B: the winner is B's skill 11, remote, so the node stays unbound. */
    q.pin_machine = mid(2);
    AienMachineId b = mid(2);
    CHECK(sr_bind_node(&f.r, &g_tmpl, (uint32_t)s, &q, &held, NOW_US, &route) == SR_E_REMOTE &&
          route.chosen.skill_id == 11 && aien_mid_equal(&route.target, &b) && unbound(s),
          "pinned to B: SR_E_REMOTE to B's skill 11, node unbound");
    /* A held remote route is checked against the lease: valid before it ends, SR_E_MACHINE after. */
    CHECK(sr_route_check(&f.r, &route, NOW_US) == SR_E_REMOTE, "held route to B before lease end: SR_E_REMOTE");
    CHECK(sr_route_check(&f.r, &route, NOW_US + 999999) == SR_E_REMOTE, "one microsecond before lease end");
    CHECK(sr_route_check(&f.r, &route, NOW_US + 1000000) == SR_E_MACHINE &&
          sr_bind_route(&f.r, &g_tmpl, (uint32_t)s, &route, NOW_US + 2000000) == SR_E_MACHINE && unbound(s),
          "held route to B after lease end: SR_E_MACHINE, node unbound");
    CHECK(cq_machine_advertise_id(&f.c, &b, NOW_US + 5000000, NULL) == CQ_OK &&
          sr_route_check(&f.r, &route, NOW_US + 2000000) == SR_E_REMOTE, "lease renewed: route valid again");
    /* A provider on a leased index with no canonical identity: never a target. */
    SrRequirement qs = requirement(OP_SUMMARIZE);
    qs.local_only = 0;
    qs.need.locality_constraints.allowed = 0;
    CHECK(sr_bind_node(&f.r, &g_tmpl, (uint32_t)s, &qs, &held, NOW_US, &route) == SR_E_NO_CANDIDATE &&
          route.why == SR_WHY_MACHINE_UNKNOWN && unbound(s),
          "provider machine without identity: SR_E_NO_CANDIDATE why MACHINE_UNKNOWN (verdict %d why 0x%x)",
          route.verdict, route.why);
    cq_catalog_free(&f.c);
}

static void t_digest(void) {
    printf("[*] (e) skill digest / version pin\n");
    Fx f;
    fx_init(&f);
    CqHeld held = { &g_w, &g_caps };
    SrRoute route;
    int s = tmpl();
    SrRequirement q = requirement(OP_SCALE);
    q.pin_skill_digest[0] = 0x07;
    q.pin_skill_digest[1] = 0xAA;                  /* X: the graph advertises 0x07 00.. (Y) */
    CHECK(sr_bind_node(&f.r, &g_tmpl, (uint32_t)s, &q, &held, NOW_US, &route) == SR_E_NO_CANDIDATE &&
          route.why == SR_WHY_PIN && route.rejected.pin_mismatch == 2 && unbound(s),
          "pinned digest X, graph has Y: SR_E_NO_CANDIDATE why PIN (why 0x%x), node unbound", route.why);
    memset(q.pin_skill_digest, 0, 32);
    q.pin_skill_version = 2;
    CHECK(sr_bind_node(&f.r, &g_tmpl, (uint32_t)s, &q, &held, NOW_US, &route) == SR_E_NO_CANDIDATE &&
          route.why == SR_WHY_PIN && unbound(s), "pinned version 2, graph has 1: why PIN");
    q.pin_skill_version = 1;
    q.pin_skill_digest[0] = 0x09;                  /* the costlier, matching provider */
    CHECK(sr_bind_node(&f.r, &g_tmpl, (uint32_t)s, &q, &held, NOW_US, &route) == SR_OK &&
          g_tmpl.nodes[s].op == 9 && route.skill_digest[0] == 0x09, "matching pin binds skill 9");
    cq_catalog_free(&f.c);
}

static void t_authority(void) {
    printf("[*] (f) authority insufficiency\n");
    Fx f;
    fx_init(&f);
    CqHeld none = { &g_w, &g_caps_none };
    SrRoute route;
    int s = tmpl();
    SrRequirement q = requirement(OP_SCALE);
    CHECK(sr_bind_node(&f.r, &g_tmpl, (uint32_t)s, &q, &none, NOW_US, &route) == SR_E_NO_CANDIDATE &&
          route.why == SR_WHY_AUTHORITY && route.stats.rejected_authority == 2 && unbound(s),
          "authority listed but not held: SR_E_NO_CANDIDATE why AUTHORITY (why 0x%x), node unbound", route.why);
    CHECK(sr_bind_node(&f.r, &g_tmpl, (uint32_t)s, &q, NULL, NOW_US, &route) == SR_E_NO_CANDIDATE &&
          route.why == SR_WHY_AUTHORITY && unbound(s), "no authority view at all: why AUTHORITY");
    cq_catalog_free(&f.c);
}

/* ---- alternatives ---- */

static void t_alternatives(void) {
    printf("[*] sr_route_alternatives: ranked, out[0] = sr_route, dominated included\n");
    Fx f;
    fx_init(&f);
    CqHeld held = { &g_w, &g_caps };
    for (int remote = 0; remote < 2; remote++) {
        SrRequirement q = requirement(OP_SCALE);
        q.local_only = (uint32_t)!remote;
        if (remote) q.need.locality_constraints.allowed = 0;
        SrRoute one, alt[8];
        int rc1 = sr_route(&f.r, &q, &held, NOW_US, &one);
        int n = sr_route_alternatives(&f.r, &q, &held, NOW_US, alt, 8);
        CHECK(rc1 == SR_OK && n == 2 + remote, "count %d (remote %d)", n, remote);
        if (n < 2) continue;
        CHECK(memcmp(&alt[0].chosen, &one.chosen, sizeof one.chosen) == 0 && alt[0].verdict == one.verdict &&
              alt[0].generation == one.generation && memcmp(&alt[0].key, &one.key, sizeof one.key) == 0,
              "out[0] is sr_route's choice");
        CHECK(alt[0].chosen.skill_id == 7 && alt[1].chosen.skill_id == 9 &&
              alt[1].chosen.expected_cost > alt[0].chosen.expected_cost &&
              alt[1].chosen.expected_latency > alt[0].chosen.expected_latency,
              "skill 9, dominated by 7 on cost and latency, is offered second");
        for (int i = 1; i < n; i++)
            CHECK(cq_dim_value(&alt[i - 1].chosen, CQ_DIM_COST) <= cq_dim_value(&alt[i].chosen, CQ_DIM_COST),
                  "ranked by cost");
        if (remote)
            CHECK(alt[2].chosen.skill_id == 11 && alt[2].verdict == SR_E_REMOTE && alt[2].target_known,
                  "remote alternative carries its canonical target");
        CHECK(sr_route_check(&f.r, &alt[1], NOW_US) == SR_OK, "an alternative is a valid held route");
        int s = tmpl();
        CHECK(sr_bind_route(&f.r, &g_tmpl, (uint32_t)s, &alt[1], NOW_US) == SR_OK && g_tmpl.nodes[s].op == 9,
              "the second alternative binds");
    }
    SrRoute alt[2];
    SrRequirement q = requirement(OP_SCALE);
    CHECK(sr_route_alternatives(&f.r, &q, &held, NOW_US, alt, 1) == 1 && alt[0].chosen.skill_id == 7,
          "max 1 gives the winner");
    CHECK(sr_route_alternatives(&f.r, &q, &held, NOW_US, alt, 0) == SR_E_ARG, "max 0 refused");
    cq_catalog_free(&f.c);
}

int main(void) {
    memset(&g_skills, 0, sizeof g_skills);
    g_skills.n = 2;
    g_skills.skill[0] = (AgSkill){ 7, sk_scale, { 0x07 } };
    g_skills.skill[1] = (AgSkill){ 9, sk_other, { 0x09 } };

    AienosCapView *view;
    CHECK(aienos_cap_start(&g_admin, &view) == 0, "authority");
    CHECK(rx_world_init_native(&g_w, view, 2, 1u << 14) == RX_OK, "world");
    g_w.external_subject = SUBJ_EXTERNAL;
    memset(&g_caps, 0, sizeof g_caps);
    g_caps.subject = SUBJ_PLAN;
    g_caps.cap[0].ref = mint(SUBJ_PLAN, RES_HELD, RX_RIGHT_READ);
    g_caps.cap[0].resource = RES_HELD;
    g_caps.cap[0].rights = RX_RIGHT_READ;
    g_caps.n = 1;
    memset(&g_caps_none, 0, sizeof g_caps_none);
    g_caps_none.subject = SUBJ_PLAN;
    g_caps_none.cap[0].ref = (RxCapRef){ 4000, 1 };     /* listed, never minted */
    g_caps_none.cap[0].resource = RES_HELD;
    g_caps_none.cap[0].rights = RX_RIGHT_READ;
    g_caps_none.n = 1;

    t_end_to_end();
    t_withdrawn();
    t_stale();
    t_unavailable();
    t_machine();
    t_digest();
    t_authority();
    t_alternatives();

    rx_world_wait_quiescent(&g_w, 30000);
    rx_world_destroy(&g_w);
    aienos_cap_stop(g_admin, view);
    printf("checks %d failures %d\n", g_checks, g_fail);
    if (g_fail == 0) printf("COMPOSITION2_SKILLROUTE_PASS\n");
    return g_fail ? 1 : 0;
}
