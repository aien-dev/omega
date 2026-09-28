/*
 * OMEGA_CAPABILITY_QUERY -- AIEN states a CapabilityNeed; Omega compiles it
 * into probes of the Capability Graph, the Skill Network, the MCP registry,
 * local physical capabilities and Fabric advertisements, and returns a short
 * list of CapabilityCandidates ranked on explicit dimensions.
 *
 * Two parts.
 *
 * Scenarios on small hand-built catalogs: aliases and specializations, each
 * hard constraint, Fabric leases and locality, MCP sessions, caller-defined
 * tradeoffs (two orders, two winners), tolerance bands, determinism and
 * registration-order independence, authority decided through the native
 * AIENOS authority (held, not held, revoked), and the chosen candidate driving
 * an action graph.
 *
 * Scaling on synthetic catalogs of 100, 1,000, 10,000 and 100,000
 * capabilities. Every query is compared with an oracle written separately in
 * this file: a full scan of every entry with its own match rule, its own
 * Pareto front and its own selection. Measured: candidate recall, selection
 * precision, query latency, bytes that cross into cognition, and how each
 * grows with the catalog.
 */
#include "runtime/aienos_cap.h"
#include "runtime/rx_capq.h"
#include "runtime/rx_graph.h"
#include "runtime/rx_world.h"
#include "omega_evidence.h"
#include "sha256.h"

#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static int g_checks;
static int g_fail;

#define CHECK(cond, ...) do {                                            \
        g_checks++;                                                      \
        if (!(cond)) {                                                   \
            g_fail++;                                                    \
            fprintf(stderr, "  FAIL %s:%d ", __FILE__, __LINE__);        \
            fprintf(stderr, __VA_ARGS__);                                \
            fputc('\n', stderr);                                         \
        }                                                                \
    } while (0)

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#define SELF     1u
#define NOW_US   1000000ull
#define RES_BASE 0xB000000ull
#define ALIAS(op) (0x80000000u | (op))

/* ---- receipt figures ---- */

#define N_SIZES 4
static const uint32_t SIZES[N_SIZES] = { 100, 1000, 10000, 100000 };

typedef struct {
    uint32_t catalog;
    uint32_t n_ops;
    uint32_t queries, with_candidates, no_op;
    uint64_t expected, hit;             /* recall: oracle's ranked list vs returned */
    uint64_t returned, returned_ok;     /* precision: returned that the oracle finds feasible */
    uint32_t top1_equal, list_mismatch, feasible_count_mismatch, front_mismatch;
    uint64_t q_median_ns, q_p99_ns, q_max_ns;
    uint64_t hot_median_ns;
    uint64_t compile_median_ns, lookup_median_ns;
    uint64_t oracle_median_ns;
    double probed_mean;
    uint64_t probed_max;
    uint64_t bytes_max, bytes_sum;
    uint64_t need_bytes;
    uint64_t desc_bytes, catalog_bytes;
    uint64_t desc_bytes_read;
    uint64_t naive_named_desc_bytes;    /* descriptions of just the named operation's providers */
    double naive_named_precision;       /* feasible share of everything named that operation */
    double build_ms;
} SizeRow;

static struct {
    char cpus[128];
    SizeRow row[N_SIZES];
    uint32_t scenario_checks;
    uint32_t tradeoff_distinct_winners;
    uint32_t det_runs, det_mismatch;
    uint32_t held_validated, revoked_seen;
    uint32_t graph_bound, graph_missing_reported;
    uint32_t seam_skill_ran;
    uint64_t mints_during_query;
} R;

/* ---- deterministic generator ---- */

static uint64_t g_rng;
static uint64_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}
static uint64_t rnd_n(uint64_t n) { return n ? rnd() % n : 0; }
static uint64_t log_uniform(uint64_t lo, uint64_t hi) {
    /* lo..hi spread over decades */
    uint32_t decades = 0;
    for (uint64_t x = lo; x < hi; x *= 10) decades++;
    uint64_t base = lo;
    for (uint32_t d = (uint32_t)rnd_n(decades ? decades : 1); d; d--) base *= 10;
    uint64_t v = base + rnd_n(base * 9);
    return v > hi ? hi : v;
}

/* ---- small hand-built catalogs ---- */

static CqEntry E(uint32_t cap, uint32_t op, uint32_t src, uint32_t machine) {
    CqEntry e;
    memset(&e, 0, sizeof e);
    e.capability_id = cap;
    e.realization_id = cap * 10;
    e.skill_id = cap;
    e.op = op;
    e.source = (uint8_t)src;
    e.machine_id = machine;
    e.effects = CQ_FX_PURE;
    e.in_types = 0x1;
    e.out_types = 0x2;
    e.confidence_ppm = 900000;
    e.reliability_ppm = 990000;
    e.evidence_level = CQ_EV_MEASURED;
    e.cost = 10;
    e.latency_us = 100;
    e.energy_uj = 100;
    e.live = 1;
    return e;
}

static const char DESC[] =
    "{\"name\":\"example\",\"description\":\"a provider description the query never reads\"}";

static void reg(CqCatalog *c, const CqEntry *e) {
    CHECK(cq_register(c, e, DESC, (uint32_t)sizeof DESC - 1) == CQ_OK, "register %u", e->capability_id);
}

static CqNeed open_need(uint32_t op) {
    CqNeed n;
    memset(&n, 0, sizeof n);
    n.semantic_operation = op;
    n.accepted_input_types = ~0ull;
    n.effect_class = 0x1FF;
    n.authority_ceiling.resource_lo = 0;
    n.authority_ceiling.resource_hi = UINT64_MAX;
    n.authority_ceiling.rights = RX_RIGHT_READ | RX_RIGHT_WRITE | RX_RIGHT_EFFECT;
    n.locality_constraints.allowed = CQ_LOC_LOCAL | CQ_LOC_FABRIC;
    return n;
}

static CqTradeoffs order1(uint32_t d) {
    CqTradeoffs t;
    memset(&t, 0, sizeof t);
    t.n_order = 1;
    t.order[0] = (uint8_t)d;
    t.k = CQ_MAX_K;
    return t;
}

static int ask(const CqCatalog *c, const CqNeed *n, const CqTradeoffs *t, const CqHeld *h,
               CqResult *r) {
    CqPlan p;
    cq_compile(c, n, &p);
    return cq_query(c, &p, n, t, h, NOW_US, r, NULL);
}

static int has(const CqResult *r, uint32_t cap) {
    for (uint32_t i = 0; i < r->n; i++)
        if (r->cand[i].capability_id == cap) return 1;
    return 0;
}

/* ops: 1 read (tool), 2 read.local (specializes 1), 3 matvec (physical),
 *      4 summarize (skill), 5 send (tool, MCP allowed), 6 read.cached (specializes 2) */
static void small_catalog(CqCatalog *c) {
    CHECK(cq_catalog_init(c, SELF, 16, 8) == CQ_OK, "init");
    uint32_t tool = CQ_SRC(CQ_SRC_GRAPH) | CQ_SRC(CQ_SRC_SKILL) | CQ_SRC(CQ_SRC_MCP) |
                    CQ_SRC(CQ_SRC_FABRIC);
    CHECK(cq_op_define(c, 1, 0, tool) == CQ_OK, "op 1");
    CHECK(cq_op_define(c, 2, 1, tool) == CQ_OK, "op 2");
    CHECK(cq_op_define(c, 3, 0, CQ_SRC(CQ_SRC_GRAPH) | CQ_SRC(CQ_SRC_PHYSICAL) |
                                    CQ_SRC(CQ_SRC_FABRIC)) == CQ_OK, "op 3");
    CHECK(cq_op_define(c, 4, 0, CQ_SRC(CQ_SRC_SKILL) | CQ_SRC(CQ_SRC_FABRIC)) == CQ_OK, "op 4");
    CHECK(cq_op_define(c, 5, 0, tool) == CQ_OK, "op 5");
    CHECK(cq_op_define(c, 6, 2, tool) == CQ_OK, "op 6");
    CHECK(cq_op_define(c, 8, 0, tool) == CQ_E_ARG, "operations are numbered densely");
    CHECK(cq_alias(c, 1001, 1) == CQ_OK && cq_alias(c, 1003, 3) == CQ_OK &&
          cq_alias(c, 1005, 5) == CQ_OK, "aliases");
    CHECK(cq_machine_advertise(c, 2, NOW_US + 5000000) == CQ_OK, "machine 2 leased");
    CHECK(cq_machine_advertise(c, 3, NOW_US - 1) == CQ_OK, "machine 3 lease expired");
    CHECK(cq_machine_advertise(c, SELF, NOW_US) == CQ_E_ARG, "self is not a Fabric machine");
}

static void t_resolution(void) {
    printf("[*] naming: aliases, specializations, unknown operations, sources that apply\n");
    CqCatalog c;
    small_catalog(&c);
    CqEntry e = E(10, 1, CQ_SRC_GRAPH, SELF);
    reg(&c, &e);
    e = E(20, 2, CQ_SRC_SKILL, SELF);
    reg(&c, &e);
    e = E(30, 6, CQ_SRC_MCP, SELF);
    reg(&c, &e);
    e = E(40, 3, CQ_SRC_PHYSICAL, SELF);
    reg(&c, &e);
    e = E(50, 3, CQ_SRC_MCP, SELF);
    CHECK(cq_register(&c, &e, NULL, 0) == CQ_E_ARG, "MCP cannot realize a physical operation");
    e = E(51, 1, CQ_SRC_FABRIC, SELF);
    CHECK(cq_register(&c, &e, NULL, 0) == CQ_E_ARG, "the Fabric speaks only for other machines");
    e = E(52, 1, CQ_SRC_GRAPH, 2);
    CHECK(cq_register(&c, &e, NULL, 0) == CQ_E_ARG, "another machine arrives only through the Fabric");
    CqResult r;
    CqNeed n = open_need(1);
    CqTradeoffs t = order1(CQ_DIM_COST);
    t.include_dominated = 1;
    CHECK(ask(&c, &n, &t, NULL, &r) == CQ_E_UNBUILT, "a catalog is indexed before use");
    CHECK(cq_catalog_build(&c) == CQ_OK, "build");

    CHECK(ask(&c, &n, &t, NULL, &r) == CQ_OK && r.n == 3 && has(&r, 10) && has(&r, 20) && has(&r, 30),
          "a need for 'read' reaches its specializations (%u)", r.n);
    n.semantic_operation = 1001;
    CqResult r2;
    CHECK(ask(&c, &n, &t, NULL, &r2) == CQ_OK && r2.n == 3 &&
          memcmp(&r, &r2, cq_result_bytes(&r)) == 0,
          "asked by an alias, the provider registered under the canonical name is found");
    CqPlan p1, p2;
    CqNeed a = open_need(1), b = open_need(1001);
    cq_compile(&c, &a, &p1);
    cq_compile(&c, &b, &p2);
    CHECK(memcmp(p1.digest, p2.digest, 32) == 0 && p1.op == 1 && p1.n_ops == 3,
          "the plan's identity does not depend on the name used");
    n = open_need(2);
    CHECK(ask(&c, &n, &t, NULL, &r) == CQ_OK && r.n == 2 && !has(&r, 10),
          "a specialized need does not reach the general provider");
    n = open_need(0x7FFFFFF0u);
    CHECK(ask(&c, &n, &t, NULL, &r) == CQ_E_NO_OP && r.n == 0, "an unknown operation");
    n = open_need(3);
    cq_compile(&c, &n, &p1);
    CHECK(p1.verdict == CQ_OK && !(p1.sources & CQ_SRC(CQ_SRC_MCP)) &&
          (p1.sources & CQ_SRC(CQ_SRC_PHYSICAL)), "MCP is not consulted for a physical operation");
    n.sources = CQ_SRC(CQ_SRC_MCP);
    cq_compile(&c, &n, &p1);
    CHECK(p1.verdict == CQ_E_NO_SOURCE, "no applicable source");
    n = open_need(1);
    cq_compile(&c, &n, &p1);
    CHECK(!(p1.sources & CQ_SRC(CQ_SRC_PHYSICAL)), "local physical is not consulted for a tool operation");
    cq_catalog_free(&c);
}

static void t_constraints(void) {
    printf("[*] each hard constraint excludes exactly what it should\n");
    CqCatalog c;
    small_catalog(&c);
    CqEntry e;
    e = E(1, 5, CQ_SRC_GRAPH, SELF);                                    reg(&c, &e); /* baseline */
    e = E(2, 5, CQ_SRC_GRAPH, SELF); e.in_types = 0x5;                  reg(&c, &e); /* needs type 2 */
    e = E(3, 5, CQ_SRC_GRAPH, SELF); e.out_types = 0x8;                 reg(&c, &e); /* wrong output */
    e = E(4, 5, CQ_SRC_MCP, SELF); e.effects = CQ_FX_EXTERNAL_WRITE;    reg(&c, &e);
    e = E(5, 5, CQ_SRC_MCP, SELF); e.effects = CQ_FX_EXTERNAL_WRITE | CQ_FX_EXTERNAL_IRREVERSIBLE;
    reg(&c, &e);
    e = E(6, 5, CQ_SRC_GRAPH, SELF); e.auth_resource = RES_BASE + 1; e.auth_rights = RX_RIGHT_READ;
    reg(&c, &e);
    e = E(7, 5, CQ_SRC_GRAPH, SELF); e.auth_resource = RES_BASE + 1;
    e.auth_rights = RX_RIGHT_READ | RX_RIGHT_WRITE | RX_RIGHT_EFFECT;   reg(&c, &e);
    e = E(8, 5, CQ_SRC_GRAPH, SELF); e.auth_resource = RES_BASE + 99; e.auth_rights = RX_RIGHT_READ;
    reg(&c, &e);
    e = E(9, 5, CQ_SRC_GRAPH, SELF); e.latency_us = 50000;              reg(&c, &e);
    e = E(10, 5, CQ_SRC_GRAPH, SELF); e.energy_uj = 90000;              reg(&c, &e);
    e = E(11, 5, CQ_SRC_GRAPH, SELF); e.reliability_ppm = 800000;       reg(&c, &e);
    e = E(12, 5, CQ_SRC_MCP, SELF); e.evidence_level = CQ_EV_DECLARED;  reg(&c, &e);
    e = E(13, 5, CQ_SRC_GRAPH, SELF); e.live = 0;                       reg(&c, &e);
    e = E(14, 5, CQ_SRC_MCP, SELF); e.live = 0;                         reg(&c, &e); /* session gone */
    e = E(15, 5, CQ_SRC_FABRIC, 2);                                     reg(&c, &e); /* leased */
    e = E(16, 5, CQ_SRC_FABRIC, 3);                                     reg(&c, &e); /* lease expired */
    e = E(17, 5, CQ_SRC_FABRIC, 4);                                     reg(&c, &e); /* never advertised */
    e = E(18, 5, CQ_SRC_GRAPH, SELF); e.cost = 5000;                    reg(&c, &e);
    e = E(19, 5, CQ_SRC_GRAPH, SELF); e.confidence_ppm = 300000;        reg(&c, &e);
    CHECK(cq_catalog_build(&c) == CQ_OK, "build");

    CqTradeoffs t = order1(CQ_DIM_COST);
    t.include_dominated = 1;
    CqResult r;
    CqNeed n = open_need(5);
    CqStats st;
    CqPlan p;
    cq_compile(&c, &n, &p);
    CHECK(cq_query(&c, &p, &n, &t, NULL, NOW_US, &r, &st) == CQ_OK && r.n_feasible == 15 &&
          st.rejected_dead == 4 && st.desc_bytes_read == 0,
          "open need: all but the withdrawn, the dead session and the two unleased machines (%u, dead %llu)",
          r.n_feasible, (unsigned long long)st.rejected_dead);
    CHECK(!has(&r, 13) && !has(&r, 14) && !has(&r, 16) && !has(&r, 17) && has(&r, 15),
          "Fabric capabilities live exactly as long as their machine's lease");

    n = open_need(5); n.accepted_input_types = 0x1;
    CHECK(ask(&c, &n, &t, NULL, &r) == CQ_OK && !has(&r, 2) && has(&r, 1), "inputs: only types the caller can supply");
    n = open_need(5); n.required_output_types = 0x2;
    CHECK(ask(&c, &n, &t, NULL, &r) == CQ_OK && !has(&r, 3) && has(&r, 1), "outputs: must include what is required");
    n = open_need(5); n.effect_class = CQ_FX_PURE | CQ_FX_EXTERNAL_WRITE;
    CHECK(ask(&c, &n, &t, NULL, &r) == CQ_OK && has(&r, 4) && !has(&r, 5),
          "effects: an irreversible external write is excluded when not accepted");
    n = open_need(5); n.effect_class = CQ_FX_PURE;
    CHECK(ask(&c, &n, &t, NULL, &r) == CQ_OK && !has(&r, 4) && !has(&r, 5), "effects: pure only");
    n = open_need(5); n.authority_ceiling.rights = RX_RIGHT_READ;
    CHECK(ask(&c, &n, &t, NULL, &r) == CQ_OK && has(&r, 6) && !has(&r, 7),
          "authority: rights above the ceiling are excluded");
    n = open_need(5); n.authority_ceiling.resource_lo = RES_BASE; n.authority_ceiling.resource_hi = RES_BASE + 10;
    CHECK(ask(&c, &n, &t, NULL, &r) == CQ_OK && has(&r, 6) && !has(&r, 8) && has(&r, 1),
          "authority: resources outside the ceiling are excluded; needing none is fine");
    n = open_need(5); n.latency_budget = 1000;
    CHECK(ask(&c, &n, &t, NULL, &r) == CQ_OK && !has(&r, 9) && has(&r, 1), "latency budget");
    n = open_need(5); n.energy_budget = 1000;
    CHECK(ask(&c, &n, &t, NULL, &r) == CQ_OK && !has(&r, 10) && has(&r, 1), "energy budget");
    n = open_need(5); n.reliability_requirement = 950000;
    CHECK(ask(&c, &n, &t, NULL, &r) == CQ_OK && !has(&r, 11) && has(&r, 1), "reliability requirement");
    n = open_need(5); n.evidence_requirement = CQ_EV_MEASURED;
    CHECK(ask(&c, &n, &t, NULL, &r) == CQ_OK && !has(&r, 12) && has(&r, 1), "evidence requirement");
    n = open_need(5); n.locality_constraints.allowed = CQ_LOC_LOCAL;
    CHECK(ask(&c, &n, &t, NULL, &r) == CQ_OK && !has(&r, 15) && has(&r, 1), "local only");
    n = open_need(5); n.locality_constraints.allowed = CQ_LOC_FABRIC;
    CHECK(ask(&c, &n, &t, NULL, &r) == CQ_OK && r.n == 1 && has(&r, 15), "Fabric only");
    n = open_need(5); n.locality_constraints.machine = 2;
    CHECK(ask(&c, &n, &t, NULL, &r) == CQ_OK && r.n == 1 && has(&r, 15), "pinned to machine 2");
    n = open_need(5); n.locality_constraints.machine = SELF;
    CHECK(ask(&c, &n, &t, NULL, &r) == CQ_OK && !has(&r, 15) && has(&r, 1), "pinned to this machine");
    n = open_need(5); n.sources = CQ_SRC(CQ_SRC_MCP);
    CHECK(ask(&c, &n, &t, NULL, &r) == CQ_OK && r.n == 3 && has(&r, 4) && has(&r, 5) && has(&r, 12),
          "restricted to the MCP registry");
    n = open_need(5);
    CqTradeoffs t2 = t;
    t2.max_cost = 1000;
    t2.min_confidence = 500000;
    CHECK(ask(&c, &n, &t2, NULL, &r) == CQ_OK && !has(&r, 18) && !has(&r, 19) && has(&r, 1),
          "the caller's hard maxima on cost and minimum confidence");
    cq_catalog_free(&c);
}

static void t_tradeoffs(void) {
    printf("[*] ranking: Pareto front, caller-defined order, tolerance bands, no single score\n");
    CqCatalog c;
    small_catalog(&c);
    CqEntry e;
    /* fast: low latency, high energy. frugal: the reverse. middling: dominated by nothing
     * on (latency, energy) but beaten by `better` on both. */
    e = E(100, 3, CQ_SRC_PHYSICAL, SELF); e.latency_us = 100;  e.energy_uj = 9000; e.cost = 50; reg(&c, &e);
    e = E(200, 3, CQ_SRC_GRAPH, SELF);    e.latency_us = 900;  e.energy_uj = 1000; e.cost = 40; reg(&c, &e);
    e = E(300, 3, CQ_SRC_GRAPH, SELF);    e.latency_us = 500;  e.energy_uj = 5000; e.cost = 30; reg(&c, &e);
    e = E(400, 3, CQ_SRC_GRAPH, SELF);    e.latency_us = 600;  e.energy_uj = 6000; e.cost = 30; reg(&c, &e);
    e = E(500, 3, CQ_SRC_FABRIC, 2);      e.latency_us = 105;  e.energy_uj = 2000; e.cost = 60; reg(&c, &e);
    CHECK(cq_catalog_build(&c) == CQ_OK, "build");
    CqNeed n = open_need(1003);
    CqResult r;

    CqTradeoffs lat = order1(CQ_DIM_LATENCY);
    lat.n_order = 2;
    lat.order[1] = CQ_DIM_ENERGY;
    CHECK(ask(&c, &n, &lat, NULL, &r) == CQ_OK && r.n >= 1 && r.cand[0].capability_id == 100,
          "latency first: the fast one");
    uint32_t w_lat = r.cand[0].capability_id;
    CHECK(r.n_front == 3 && !has(&r, 300) && !has(&r, 400),
          "front over (latency, energy): 300 and 400 are beaten by 500 on both");
    CqTradeoffs en = order1(CQ_DIM_ENERGY);
    en.n_order = 2;
    en.order[1] = CQ_DIM_LATENCY;
    CHECK(ask(&c, &n, &en, NULL, &r) == CQ_OK && r.n >= 1 && r.cand[0].capability_id == 200,
          "energy first: the frugal one");
    uint32_t w_en = r.cand[0].capability_id;
    if (w_lat != w_en) R.tradeoff_distinct_winners++;
    CHECK(w_lat != w_en, "same candidates, two caller tradeoffs, two winners");

    /* Tolerance: latency within 10% counts as tied, so energy decides 100 vs 500. */
    lat.tolerance_ppm[CQ_DIM_LATENCY] = 100000;
    CHECK(ask(&c, &n, &lat, NULL, &r) == CQ_OK && r.cand[0].capability_id == 500 &&
          r.cand[1].capability_id == 100,
          "latency within 10%% is a tie; energy then prefers the Fabric provider");
    CqTradeoffs loc = order1(CQ_DIM_LOCALITY);
    loc.n_order = 2;
    loc.order[1] = CQ_DIM_LATENCY;
    CHECK(ask(&c, &n, &loc, NULL, &r) == CQ_OK && r.cand[0].capability_id == 100 && r.cand[0].local,
          "locality first keeps the work on this machine");
    CqTradeoffs cost = order1(CQ_DIM_COST);
    CHECK(ask(&c, &n, &cost, NULL, &r) == CQ_OK && r.n == 2 && r.n_front == 2 &&
          r.cand[0].capability_id == 300 && r.cand[1].capability_id == 400,
          "cost only: the tie at 30 is broken by id, deterministically");
    CqTradeoffs k1 = lat;
    k1.k = 1;
    CHECK(ask(&c, &n, &k1, NULL, &r) == CQ_OK && r.n == 1 && r.n_feasible == 5 &&
          cq_result_bytes(&r) == offsetof(CqResult, cand) + sizeof(CqCandidate),
          "k = 1 returns one record; the others stay counts");
    /* Every candidate carries its dimensions; none carries a combined score. */
    CHECK(sizeof(CqCandidate) == 88, "candidate record is %zu bytes", sizeof(CqCandidate));
    R.scenario_checks += 8;
    cq_catalog_free(&c);
}

static void t_determinism(void) {
    printf("[*] determinism: repeated queries and registration order\n");
    CqCatalog a, b;
    CHECK(cq_catalog_init(&a, SELF, 8, 8) == CQ_OK && cq_catalog_init(&b, SELF, 8, 8) == CQ_OK, "init");
    for (CqCatalog *c = &a; c; c = c == &a ? &b : NULL) {
        CHECK(cq_op_define(c, 1, 0, CQ_SRC_ALL) == CQ_OK, "op");
        CHECK(cq_machine_advertise(c, 2, NOW_US * 10) == CQ_OK, "machine");
    }
    CqEntry es[200];
    g_rng = 0x5eed;
    for (uint32_t i = 0; i < 200; i++) {
        uint32_t src = (uint32_t)rnd_n(CQ_SOURCES);
        es[i] = E(i + 1, 1, src, src == CQ_SRC_FABRIC ? 2 : SELF);
        es[i].latency_us = 100 + rnd_n(50);   /* many near-ties */
        es[i].energy_uj = 100 + rnd_n(50);
        es[i].cost = 1 + rnd_n(5);
    }
    for (uint32_t i = 0; i < 200; i++) reg(&a, &es[i]);
    for (uint32_t i = 200; i-- > 0;) reg(&b, &es[i]);
    CHECK(cq_catalog_build(&a) == CQ_OK && cq_catalog_build(&b) == CQ_OK, "build");
    CqNeed n = open_need(1);
    CqTradeoffs t = order1(CQ_DIM_COST);
    t.n_order = 3;
    t.order[1] = CQ_DIM_LATENCY;
    t.order[2] = CQ_DIM_ENERGY;
    t.tolerance_ppm[CQ_DIM_LATENCY] = 50000;
    t.include_dominated = 1;
    CqResult r0, r;
    CHECK(ask(&a, &n, &t, NULL, &r0) == CQ_OK && r0.n == CQ_MAX_K, "reference answer");
    for (uint32_t i = 0; i < 8; i++) {
        R.det_runs += 2;
        CHECK(ask(&a, &n, &t, NULL, &r) == CQ_OK, "query");
        if (memcmp(&r, &r0, sizeof r)) R.det_mismatch++;
        CHECK(ask(&b, &n, &t, NULL, &r) == CQ_OK, "query");
        if (memcmp(&r, &r0, sizeof r)) R.det_mismatch++;
    }
    CHECK(R.det_mismatch == 0, "identical answers across runs and registration orders (%u)", R.det_mismatch);
    cq_catalog_free(&a);
    cq_catalog_free(&b);
}

/* ---- authority through the native AIENOS authority, and the action graph ---- */

enum { SUBJ_EXTERNAL = 100, ISSUER = 3, SUBJ_PLAN = 61 };
#define RES_CELL  0xA000001ull
#define RES_RUN   0xA000002ull
#define RES_HELD  (RES_BASE + 1)
#define RES_ASK   (RES_BASE + 2)

static AienosCapAdmin *g_admin;
static uint64_t g_mints;

static RxCapRef mint(uint32_t subject, uint64_t resource, uint32_t rights) {
    AienosCapRef office;
    aienos_cap_office(g_admin, &office);
    AienosCapMint m = { ISSUER, subject, resource, rights, 0, { UINT32_MAX, 0 }, office };
    AienosCapRef r = { UINT32_MAX, 0 };
    if (aienos_cap_mint(g_admin, &m, &r) != 0) r = (AienosCapRef){ UINT32_MAX, 0 };
    g_mints++;
    return (RxCapRef){ r.cap_id, r.generation };
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

static void t_authority_and_graph(void) {
    printf("[*] authority decided by the native AIENOS authority; the winner drives an action graph\n");
    AienosCapView *view;
    CHECK(aienos_cap_start(&g_admin, &view) == 0, "authority");
    RxWorld w;
    CHECK(rx_world_init_native(&w, view, 2, 1u << 14) == RX_OK, "world");
    w.external_subject = SUBJ_EXTERNAL;
    AgCapTable caps;
    memset(&caps, 0, sizeof caps);
    caps.subject = SUBJ_PLAN;
    RxCapRef held_cap = mint(SUBJ_PLAN, RES_HELD, RX_RIGHT_READ);
    caps.cap[caps.n].ref = held_cap;
    caps.cap[caps.n].resource = RES_HELD;
    caps.cap[caps.n].rights = RX_RIGHT_READ;
    caps.n++;
    /* A forged entry: listed in the table, never minted. Validation must refuse it. */
    caps.cap[caps.n].ref = (RxCapRef){ 4000, 1 };
    caps.cap[caps.n].resource = RES_ASK;
    caps.cap[caps.n].rights = RX_RIGHT_READ;
    caps.n++;

    CqCatalog c;
    small_catalog(&c);
    CqEntry e;
    e = E(7, 4, CQ_SRC_SKILL, SELF); e.skill_id = 7; e.latency_us = 800;
    e.auth_resource = RES_HELD; e.auth_rights = RX_RIGHT_READ; reg(&c, &e);
    e = E(8, 4, CQ_SRC_SKILL, SELF); e.skill_id = 8; e.latency_us = 200;
    e.auth_resource = RES_ASK; e.auth_rights = RX_RIGHT_READ; reg(&c, &e);
    CHECK(cq_catalog_build(&c) == CQ_OK, "build");
    CqHeld h = { &w, &caps };
    CqNeed n = open_need(4);
    CqResult r;
    uint64_t mints0 = g_mints;

    CqTradeoffs auth = order1(CQ_DIM_AUTHORITY);
    auth.n_order = 2;
    auth.order[1] = CQ_DIM_LATENCY;
    CHECK(ask(&c, &n, &auth, &h, &r) == CQ_OK && r.n == 2 && r.cand[0].capability_id == 7 &&
          r.cand[0].required_authority.held == 1,
          "authority first: the capability the principal already holds (validated by the view)");
    R.held_validated++;
    CqCandidate held_winner = r.cand[0];
    CqTradeoffs fast = order1(CQ_DIM_LATENCY);
    CHECK(ask(&c, &n, &fast, &h, &r) == CQ_OK && r.n == 1 && r.cand[0].capability_id == 8 &&
          r.cand[0].required_authority.held == 0,
          "latency first: the faster one, whose authority is not held (the forged table entry is refused)");
    CqCandidate ask_winner = r.cand[0];
    R.mints_during_query = g_mints - mints0;
    CHECK(R.mints_during_query == 0, "queries minted nothing");

    /* The chosen candidate becomes an action graph's skill node. */
    AgSkillTable skills;
    memset(&skills, 0, sizeof skills);
    skills.n = 2;
    skills.skill[0] = (AgSkill){ 7, sk_scale, { 0x07 } };
    skills.skill[1] = (AgSkill){ 8, sk_other, { 0x08 } };
    for (int pass = 0; pass < 2; pass++) {
        const CqCandidate *cand = pass == 0 ? &held_winner : &ask_winner;
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
        CHECK(cq_bind_skill_node(&tmpl, (uint32_t)s, cand) == CQ_OK, "bind the candidate");
        CHECK(tmpl.nodes[s].op == cand->skill_id, "the node runs the candidate's procedure");
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
        CHECK(rc == AG_OK_READY, "compile %d", rc);
        if (pass == 0) {
            CHECK(rep.n_missing == 0, "held authority: nothing missing");
            R.graph_bound++;
            RxCapRef cell = mint(SUBJ_PLAN, RES_CELL, RX_RIGHT_READ | RX_RIGHT_WRITE);
            RxCapRef runc = mint(SUBJ_PLAN, RES_RUN, RX_RIGHT_READ);
            RxCapRef ext = mint(SUBJ_EXTERNAL, RES_RUN, RX_RIGHT_WRITE);
            static AgLowered L;
            CHECK(rx_graph_lower(&L, &w, &g, &skills, &caps, RES_CELL, cell, RES_RUN, runc, NULL) == 0,
                  "lower");
            CHECK(rx_graph_start(&L, ext, 1) > 0, "start");
            CHECK(rx_world_wait_quiescent(&w, 30000) == RX_OK, "settle");
            AgResult res;
            rx_graph_collect(&L, 1, &res);
            int sn = -1;
            for (uint32_t i = 0; i < g.n_nodes; i++)
                if (g.nodes[i].alive && g.nodes[i].kind == AG_SKILL) sn = (int)i;
            CHECK(res.outcome == AG_RUN_SUCCESS && sn >= 0 && res.value[sn] == 14 * 3 + 1,
                  "the graph ran the chosen capability's procedure (outcome %d)", res.outcome);
            if (res.outcome == AG_RUN_SUCCESS) R.seam_skill_ran++;
        } else {
            CHECK(rep.n_missing == 1 && rep.missing[0].resource == RES_ASK &&
                  rep.missing[0].rights == RX_RIGHT_READ,
                  "authority not held: compile reports exactly the candidate's requirement");
            if (rep.n_missing == 1) R.graph_missing_reported++;
        }
    }

    /* Revoke the held capability; the next query sees it. */
    AienosCapRef office;
    aienos_cap_office(g_admin, &office);
    CHECK(aienos_cap_revoke(g_admin, office, (AienosCapRef){ held_cap.cap_id, held_cap.generation }) == 0,
          "revoke");
    auth.include_dominated = 1;
    CHECK(ask(&c, &n, &auth, &h, &r) == CQ_OK && r.n == 2 && r.cand[0].capability_id == 8 &&
          has(&r, 7) && r.cand[0].required_authority.held == 0,
          "after revocation nothing is held; latency decides");
    for (uint32_t i = 0; i < r.n; i++)
        if (r.cand[i].capability_id == 7 && r.cand[i].required_authority.held == 0) R.revoked_seen++;
    CHECK(R.revoked_seen == 1, "the revoked capability is reported as not held");
    cq_catalog_free(&c);
    rx_world_wait_quiescent(&w, 30000);
    rx_world_destroy(&w);
    aienos_cap_stop(g_admin, view);
}

/* ---- synthetic catalogs ---- */

typedef struct {
    CqCatalog c;
    uint32_t n_ops, hot;
    uint32_t *parent;               /* the generator's own copy of the tree */
    uint32_t n_hot;
} Synth;

static const uint16_t FX_PRESET[] = {
    CQ_FX_PURE, CQ_FX_PURE, CQ_FX_PURE, CQ_FX_READ_FILESYSTEM, CQ_FX_READ_NETWORK,
    CQ_FX_SPAWN_PROCESS | CQ_FX_LOCAL_EPHEMERAL, CQ_FX_WORLD_MUTATION, CQ_FX_EXTERNAL_WRITE,
    CQ_FX_EXTERNAL_WRITE | CQ_FX_EXTERNAL_IRREVERSIBLE, CQ_FX_SECRET_BEARING | CQ_FX_READ_NETWORK,
};
#define N_TYPES 12
#define N_MACH  16

static uint32_t op_domains(uint32_t op) {
    switch (op % 3) {
    case 0:  return CQ_SRC(CQ_SRC_GRAPH) | CQ_SRC(CQ_SRC_SKILL) | CQ_SRC(CQ_SRC_MCP) | CQ_SRC(CQ_SRC_FABRIC);
    case 1:  return CQ_SRC(CQ_SRC_GRAPH) | CQ_SRC(CQ_SRC_PHYSICAL) | CQ_SRC(CQ_SRC_FABRIC);
    default: return CQ_SRC(CQ_SRC_GRAPH) | CQ_SRC(CQ_SRC_SKILL) | CQ_SRC(CQ_SRC_FABRIC);
    }
}

static uint32_t pick_bit(uint32_t mask) {
    uint32_t n = (uint32_t)__builtin_popcount(mask), k = (uint32_t)rnd_n(n);
    for (uint32_t b = 0; b < 32; b++)
        if (mask & (1u << b)) {
            if (k == 0) return b;
            k--;
        }
    return 0;
}

static uint32_t make_desc(char *buf, uint32_t cap, uint32_t op, uint32_t len) {
    static const char *words[] = { "returns", "the", "value", "of", "a", "semantic", "object",
                                   "given", "input", "schema", "type", "string", "required",
                                   "optional", "provider", "tool", "resource", "timeout" };
    int n = snprintf(buf, len, "{\"name\":\"op%u.impl%u\",\"description\":\"", op, cap);
    uint32_t p = (uint32_t)n;
    while (p + 16 < len) {
        const char *w = words[rnd_n(sizeof words / sizeof words[0])];
        size_t l = strlen(w);
        memcpy(buf + p, w, l);
        p += (uint32_t)l;
        buf[p++] = ' ';
    }
    memcpy(buf + p, "\",\"inputSchema\":{}}", 19);
    return p + 19 <= len ? p + 19 : len;
}

static int synth_build(Synth *s, uint32_t n_entries, double *build_ms) {
    memset(s, 0, sizeof *s);
    uint64_t t0 = now_ns();
    g_rng = 0x9e3779b97f4a7c15ull ^ n_entries;
    s->n_ops = n_entries / 6 > 4 ? n_entries / 6 : 4;
    s->hot = 1;
    s->n_hot = n_entries / 10 < 1000 ? n_entries / 10 : 1000;
    if (cq_catalog_init(&s->c, SELF, s->n_ops, N_MACH) != CQ_OK) return -1;
    s->parent = calloc(s->n_ops + 1, sizeof *s->parent);
    if (!s->parent) return -1;
    for (uint32_t op = 1; op <= s->n_ops; op++) {
        uint32_t parent = 0;
        if (op > 4 && op % 4 == 0) {
            parent = op - 1 - (uint32_t)rnd_n(3);
            /* a specialization lives where its general operation can */
            if (op_domains(op) & ~op_domains(parent)) parent = 0;
        }
        s->parent[op] = parent;
        if (cq_op_define(&s->c, op, parent, op_domains(op)) != CQ_OK) return -1;
        if (cq_alias(&s->c, ALIAS(op), op) != CQ_OK) return -1;
    }
    for (uint32_t m = 2; m < 2 + N_MACH; m++)
        cq_machine_advertise(&s->c, m, m < 2 + N_MACH - 4 ? NOW_US + 1000000000ull : NOW_US - 1);
    char desc[2048];
    for (uint32_t i = 0; i < n_entries; i++) {
        CqEntry e;
        memset(&e, 0, sizeof e);
        e.capability_id = i + 1;
        e.realization_id = (uint32_t)(0x100000u + i);
        e.op = i < s->n_hot ? s->hot : 2 + (i - s->n_hot) % (s->n_ops - 1);
        e.source = (uint8_t)pick_bit(op_domains(e.op));
        e.machine_id = e.source == CQ_SRC_FABRIC ? 2 + (uint32_t)rnd_n(N_MACH) : SELF;
        e.skill_id = e.source == CQ_SRC_SKILL ? 1 + (uint32_t)rnd_n(4096) : 0;
        e.in_types = 1ull << rnd_n(N_TYPES);
        if (rnd_n(10) < 3) e.in_types |= 1ull << rnd_n(N_TYPES);
        e.out_types = 1ull << rnd_n(N_TYPES);
        if (rnd_n(10) < 4) e.out_types |= 1ull << rnd_n(N_TYPES);
        e.effects = e.source == CQ_SRC_MCP ? FX_PRESET[3 + rnd_n(7)]
                                           : FX_PRESET[rnd_n(sizeof FX_PRESET / sizeof FX_PRESET[0])];
        uint32_t rr = (uint32_t)rnd_n(10);
        e.auth_rights = rr < 3 ? 0 : rr < 7 ? RX_RIGHT_READ : rr < 9 ? RX_RIGHT_READ | RX_RIGHT_WRITE
                                                            : RX_RIGHT_READ | RX_RIGHT_WRITE | RX_RIGHT_EFFECT;
        e.auth_resource = e.auth_rights ? RES_BASE + rnd_n(64) : 0;
        e.cost = log_uniform(1, 1000000);
        e.latency_us = log_uniform(1, 10000000);
        e.energy_uj = log_uniform(1, 10000000);
        e.confidence_ppm = 500000 + (uint32_t)rnd_n(500001);
        e.reliability_ppm = 900000 + (uint32_t)rnd_n(100001);
        e.evidence_level = (uint8_t)(e.source == CQ_SRC_MCP ? rnd_n(2) : rnd_n(4));
        e.evidence_ref = rnd();
        e.live = rnd_n(100) < (e.source == CQ_SRC_MCP ? 90u : 97u);
        uint32_t len = make_desc(desc, e.capability_id, e.op, 256 + (uint32_t)rnd_n(1280));
        if (cq_register(&s->c, &e, desc, len) != CQ_OK) return -1;
    }
    int rc = cq_catalog_build(&s->c);
    *build_ms = (double)(now_ns() - t0) / 1e6;
    return rc;
}

static void synth_free(Synth *s) {
    cq_catalog_free(&s->c);
    free(s->parent);
}

/* Loosen a random need toward one real provider, as AIEN usually asks for
 * something that exists. Each field still has its own chance to be too strict. */
static void seed_need(CqNeed *n, const CqEntry *e) {
    n->accepted_input_types |= e->in_types;
    if (n->required_output_types && rnd_n(10) < 7) n->required_output_types = e->out_types & -e->out_types;
    if (rnd_n(10) < 8) n->effect_class |= e->effects;
    if (rnd_n(10) < 8) {
        n->authority_ceiling.rights |= e->auth_rights;
        if (e->auth_rights && e->auth_resource > n->authority_ceiling.resource_hi)
            n->authority_ceiling.resource_hi = e->auth_resource;
    }
    if (rnd_n(10) < 8) {
        n->locality_constraints.allowed |= e->machine_id == SELF ? CQ_LOC_LOCAL : CQ_LOC_FABRIC;
        n->locality_constraints.machine = 0;
    }
    if (n->latency_budget && n->latency_budget < e->latency_us) n->latency_budget = e->latency_us * (1 + rnd_n(10));
    if (n->energy_budget && n->energy_budget < e->energy_uj) n->energy_budget = e->energy_uj * (1 + rnd_n(10));
    if (n->reliability_requirement > e->reliability_ppm && rnd_n(2)) n->reliability_requirement = 0;
    if (n->evidence_requirement > e->evidence_level && rnd_n(10) < 8) n->evidence_requirement = e->evidence_level;
    if (n->sources) n->sources |= 1u << e->source;
}

static CqNeed random_need(const Synth *s, int hot) {
    CqNeed n;
    memset(&n, 0, sizeof n);
    const CqEntry *seed = &s->c.e[hot ? rnd_n(s->n_hot) : rnd_n(s->c.n)];
    int seeded = rnd_n(10) < 8;
    uint32_t op = seeded ? seed->op : hot ? s->hot : 1 + (uint32_t)rnd_n(s->n_ops);
    n.semantic_operation = rnd_n(2) ? ALIAS(op) : op;
    int unknown = !hot && rnd_n(100) < 4;
    if (unknown) n.semantic_operation = 0x7FFFFF00u + (uint32_t)rnd_n(64);
    for (uint32_t k = 4 + (uint32_t)rnd_n(6); k; k--) n.accepted_input_types |= 1ull << rnd_n(N_TYPES);
    if (rnd_n(10) < 7) n.required_output_types = 1ull << rnd_n(N_TYPES);
    static const uint32_t FX_NEED[] = { CQ_FX_PURE, CQ_FX_PURE | CQ_FX_READ_FILESYSTEM | CQ_FX_READ_NETWORK,
                                        0x1FF & ~(CQ_FX_EXTERNAL_IRREVERSIBLE | CQ_FX_SECRET_BEARING), 0x1FF };
    n.effect_class = FX_NEED[rnd_n(4)];
    n.authority_ceiling.resource_lo = RES_BASE;
    n.authority_ceiling.resource_hi = RES_BASE + (rnd_n(2) ? 31 : 63);
    static const uint32_t RIGHTS[] = { RX_RIGHT_READ, RX_RIGHT_READ | RX_RIGHT_WRITE,
                                       RX_RIGHT_READ | RX_RIGHT_WRITE | RX_RIGHT_EFFECT };
    n.authority_ceiling.rights = RIGHTS[rnd_n(3)];
    uint32_t l = (uint32_t)rnd_n(20);
    n.locality_constraints.allowed = l < 12 ? CQ_LOC_LOCAL | CQ_LOC_FABRIC : l < 16 ? CQ_LOC_LOCAL
                                   : l < 19 ? CQ_LOC_FABRIC : CQ_LOC_LOCAL | CQ_LOC_FABRIC;
    if (l == 19) n.locality_constraints.machine = 1 + (uint32_t)rnd_n(N_MACH + 1);
    if (rnd_n(2)) n.latency_budget = log_uniform(100, 10000000);
    if (rnd_n(2)) n.energy_budget = log_uniform(100, 10000000);
    static const uint32_t REL[] = { 0, 0, 950000, 990000 };
    n.reliability_requirement = REL[rnd_n(4)];
    static const uint32_t EV[] = { CQ_EV_NONE, CQ_EV_NONE, CQ_EV_DECLARED, CQ_EV_MEASURED, CQ_EV_RECEIPT };
    n.evidence_requirement = EV[rnd_n(5)];
    if (rnd_n(10) == 0) n.sources = 1u + (uint32_t)rnd_n(CQ_SRC_ALL);
    if (seeded && !unknown) seed_need(&n, seed);
    return n;
}

static CqTradeoffs random_tradeoffs(uint32_t i) {
    CqTradeoffs t;
    memset(&t, 0, sizeof t);
    t.k = i % 2 ? 8 : CQ_MAX_K;
    switch (i % 5) {
    case 0: t.n_order = 3; t.order[0] = CQ_DIM_LATENCY; t.order[1] = CQ_DIM_ENERGY; t.order[2] = CQ_DIM_COST;
            t.tolerance_ppm[CQ_DIM_LATENCY] = 100000; break;
    case 1: t.n_order = 2; t.order[0] = CQ_DIM_ENERGY; t.order[1] = CQ_DIM_LATENCY; break;
    case 2: t.n_order = 3; t.order[0] = CQ_DIM_COST; t.order[1] = CQ_DIM_RELIABILITY;
            t.order[2] = CQ_DIM_LATENCY; t.tolerance_ppm[CQ_DIM_COST] = 250000;
            t.max_cost = 500000; break;
    case 3: t.n_order = 3; t.order[0] = CQ_DIM_RELIABILITY; t.order[1] = CQ_DIM_CONFIDENCE;
            t.order[2] = CQ_DIM_COST; t.min_confidence = 600000; break;
    default: t.n_order = 0; t.include_dominated = 1; break;   /* every dimension, everything feasible */
    }
    return t;
}

/* ---- the oracle: written separately, reads every entry ---- */

static int oracle_is_under(const Synth *s, uint32_t op, uint32_t target) {
    for (uint32_t x = op; x; x = s->parent[x])
        if (x == target) return 1;
    return 0;
}

static int oracle_match(const Synth *s, const CqNeed *n, const CqTradeoffs *t, const CqEntry *e,
                        uint32_t target) {
    if (!oracle_is_under(s, e->op, target)) return 0;
    if (n->sources && !(n->sources & (1u << e->source))) return 0;
    if (!e->live) return 0;
    int local = e->machine_id == SELF;
    if (!local) {
        /* machines 2..13 hold a lease past NOW; 14..17 expired */
        if (e->machine_id < 2 || e->machine_id >= 2 + N_MACH - 4) return 0;
        if (!(n->locality_constraints.allowed & CQ_LOC_FABRIC)) return 0;
    } else if (!(n->locality_constraints.allowed & CQ_LOC_LOCAL)) {
        return 0;
    }
    if (n->locality_constraints.machine && n->locality_constraints.machine != e->machine_id) return 0;
    for (uint32_t b = 0; b < 64; b++) {
        uint64_t bit = 1ull << b;
        if ((e->in_types & bit) && !(n->accepted_input_types & bit)) return 0;   /* can't supply */
        if ((n->required_output_types & bit) && !(e->out_types & bit)) return 0; /* not produced */
    }
    for (uint32_t b = 0; b < 16; b++)
        if ((e->effects >> b & 1) && !(n->effect_class >> b & 1)) return 0;
    for (uint32_t b = 0; b < 32; b++)
        if ((e->auth_rights >> b & 1) && !(n->authority_ceiling.rights >> b & 1)) return 0;
    if (e->auth_rights && (e->auth_resource < n->authority_ceiling.resource_lo ||
                           e->auth_resource > n->authority_ceiling.resource_hi))
        return 0;
    if (n->latency_budget != 0 && e->latency_us > n->latency_budget) return 0;
    if (n->energy_budget != 0 && e->energy_uj > n->energy_budget) return 0;
    if (e->reliability_ppm < n->reliability_requirement) return 0;
    if (e->evidence_level < n->evidence_requirement) return 0;
    if (t->max_cost != 0 && e->cost > t->max_cost) return 0;
    if (e->confidence_ppm < t->min_confidence) return 0;
    return 1;
}

static void oracle_vec(const CqEntry *e, const CqTradeoffs *t, uint64_t *v, uint32_t *nd) {
    uint64_t all[CQ_DIMS] = { e->cost, e->latency_us, e->energy_uj, 1000000 - e->confidence_ppm,
                              1000000 - e->reliability_ppm, e->auth_rights ? 1 : 0,
                              e->machine_id == SELF ? 0 : 1 };
    if (t->n_order == 0) {
        for (uint32_t d = 0; d < CQ_DIMS; d++) v[d] = all[d];
        *nd = CQ_DIMS;
    } else {
        for (uint32_t d = 0; d < t->n_order; d++) v[d] = all[t->order[d]];
        *nd = t->n_order;
    }
}

typedef struct { const CqEntry *e; uint64_t v[CQ_DIMS]; uint32_t nd; int front, taken; } OItem;

static int oracle_less(const OItem *a, const OItem *b) {   /* strict: dimensions then ids */
    for (uint32_t d = 0; d < a->nd; d++)
        if (a->v[d] != b->v[d]) return a->v[d] < b->v[d];
    if (a->e->capability_id != b->e->capability_id) return a->e->capability_id < b->e->capability_id;
    if (a->e->realization_id != b->e->realization_id) return a->e->realization_id < b->e->realization_id;
    if (a->e->machine_id != b->e->machine_id) return a->e->machine_id < b->e->machine_id;
    return a->e->skill_id < b->e->skill_id;
}

static uint32_t tol_of(const CqTradeoffs *t, uint32_t d) {
    return t->n_order ? t->tolerance_ppm[t->order[d]] : t->tolerance_ppm[d];
}

/* Returns the feasible count; fills the ranked list (capability ids) and front size. */
static uint32_t oracle(const Synth *s, const CqNeed *n, const CqTradeoffs *t, uint32_t *list,
                       uint32_t *n_list, uint32_t *n_front, OItem *items) {
    *n_list = 0;
    *n_front = 0;
    uint32_t target = 0;
    uint32_t name = n->semantic_operation;
    if (name & 0x80000000u) target = name & 0x7FFFFFFFu;
    else if (name >= 1 && name <= s->n_ops) target = name;
    if (target == 0 || target > s->n_ops) return 0;
    uint32_t nf = 0;
    for (uint32_t i = 0; i < s->c.n; i++) {
        const CqEntry *e = &s->c.e[i];
        if (!oracle_match(s, n, t, e, target)) continue;
        items[nf].e = e;
        oracle_vec(e, t, items[nf].v, &items[nf].nd);
        items[nf].front = 1;
        items[nf].taken = 0;
        nf++;
    }
    for (uint32_t i = 0; i < nf; i++)
        for (uint32_t j = 0; j < nf && items[i].front; j++) {
            if (i == j) continue;
            int le = 1, lt = 0;
            for (uint32_t d = 0; d < items[i].nd; d++) {
                if (items[j].v[d] > items[i].v[d]) le = 0;
                if (items[j].v[d] < items[i].v[d]) lt = 1;
            }
            if (le && lt) items[i].front = 0;
        }
    uint32_t pool = 0;
    for (uint32_t i = 0; i < nf; i++) {
        if (items[i].front) (*n_front)++;
        if (!t->include_dominated && !items[i].front) items[i].taken = 1;
        else pool++;
    }
    uint32_t k = t->k ? t->k : CQ_MAX_K;
    if (k > CQ_MAX_K) k = CQ_MAX_K;
    while (*n_list < k && *n_list < pool) {
        /* band filter per dimension, in priority order */
        static uint8_t in[200000];
        for (uint32_t i = 0; i < nf; i++) in[i] = !items[i].taken;
        uint32_t nd = nf ? items[0].nd : 0;
        for (uint32_t d = 0; d < nd; d++) {
            uint64_t best = UINT64_MAX;
            for (uint32_t i = 0; i < nf; i++)
                if (in[i] && items[i].v[d] < best) best = items[i].v[d];
            long double lim = (long double)best * (1.0L + (long double)tol_of(t, d) / 1e6L);
            for (uint32_t i = 0; i < nf; i++)
                if (in[i] && (long double)items[i].v[d] > lim) in[i] = 0;
        }
        int pick = -1;
        for (uint32_t i = 0; i < nf; i++)
            if (in[i] && (pick < 0 || oracle_less(&items[i], &items[pick]))) pick = (int)i;
        if (pick < 0) break;
        items[pick].taken = 1;
        list[(*n_list)++] = items[pick].e->capability_id;
    }
    return nf;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

#define QUERIES     2000
#define HOT_QUERIES 200

static void t_scale(uint32_t si) {
    uint32_t size = SIZES[si];
    SizeRow *row = &R.row[si];
    memset(row, 0, sizeof *row);
    Synth s;
    CHECK(synth_build(&s, size, &row->build_ms) == 0, "build synthetic catalog of %u", size);
    row->catalog = size;
    row->n_ops = s.n_ops;
    row->desc_bytes = s.c.desc_bytes;
    row->catalog_bytes = cq_catalog_bytes(&s.c);
    row->need_bytes = sizeof(CqNeed) + sizeof(CqTradeoffs);
    OItem *items = malloc((size_t)size * sizeof *items);
    uint64_t *lat = malloc(QUERIES * sizeof *lat), *orc = malloc(QUERIES * sizeof *orc);
    uint64_t *hot = malloc(HOT_QUERIES * sizeof *hot);
    uint64_t *cmp = malloc(QUERIES * sizeof *cmp), *look = malloc(QUERIES * sizeof *look);
    CHECK(items && lat && orc && hot && cmp && look, "memory");
    uint64_t probed_sum = 0, named_total = 0, named_feasible = 0;
    g_rng = 0xC0FFEEull + size;
    uint32_t nq = 0;
    for (uint32_t q = 0; q < QUERIES + HOT_QUERIES; q++) {
        int is_hot = q >= QUERIES;
        CqNeed n = random_need(&s, is_hot);
        CqTradeoffs t = random_tradeoffs(q);
        CqPlan p;
        CqResult r;
        CqStats st;
        uint64_t t0 = now_ns();
        cq_compile(&s.c, &n, &p);
        uint64_t t1 = now_ns();
        int rc = cq_query(&s.c, &p, &n, &t, NULL, NOW_US, &r, &st);
        uint64_t t2 = now_ns(), dt = t2 - t0;
        if (is_hot) hot[q - QUERIES] = dt;
        else { lat[nq] = dt; cmp[nq] = t1 - t0; look[nq] = t2 - t1; }
        uint32_t list[CQ_MAX_K], n_list, n_front;
        uint64_t o0 = now_ns();
        uint32_t nf = oracle(&s, &n, &t, list, &n_list, &n_front, items);
        if (!is_hot) orc[nq++] = now_ns() - o0;

        row->queries++;
        if (rc == CQ_E_NO_OP) { row->no_op++; CHECK(nf == 0, "unknown operation, but the oracle found some"); continue; }
        if (rc != CQ_OK && rc != CQ_E_NO_SOURCE) { CHECK(0, "query failed %d", rc); continue; }
        if (rc == CQ_E_NO_SOURCE) r.n = r.n_feasible = r.n_front = 0;
        probed_sum += st.probed;
        if (st.probed > row->probed_max) row->probed_max = st.probed;
        row->desc_bytes_read += st.desc_bytes_read;
        size_t bytes = cq_result_bytes(&r);
        row->bytes_sum += bytes;
        if (bytes > row->bytes_max) row->bytes_max = bytes;
        if (r.n_feasible != nf) row->feasible_count_mismatch++;
        if (!t.include_dominated && r.n_front != n_front) row->front_mismatch++;
        row->expected += n_list;
        row->returned += r.n;
        int same = r.n == n_list;
        for (uint32_t i = 0; i < r.n; i++) {
            int in_list = 0;
            for (uint32_t j = 0; j < n_list; j++) in_list |= list[j] == r.cand[i].capability_id;
            row->hit += (uint64_t)in_list;
            const CqEntry *e = &s.c.e[r.cand[i].capability_id - 1];
            uint32_t target = cq_resolve(&s.c, n.semantic_operation);
            row->returned_ok += (uint64_t)oracle_match(&s, &n, &t, e, target);
            if (i >= n_list || list[i] != r.cand[i].capability_id) same = 0;
        }
        if (!same) row->list_mismatch++;
        if (n_list > 0) {
            row->with_candidates++;
            if (r.n > 0 && r.cand[0].capability_id == list[0]) row->top1_equal++;
        }
        /* What handing AIEN everything named this operation would carry. */
        for (uint32_t i = 0; i < p.n_ops; i++)
            for (uint32_t src = 0; src < CQ_SOURCES; src++) {
                uint32_t b = p.ops[i] * CQ_SOURCES + src;
                for (uint32_t j = s.c.bucket_start[b]; j < s.c.bucket_start[b + 1]; j++) {
                    row->naive_named_desc_bytes += s.c.e[s.c.bucket[j]].desc_len;
                    named_total++;
                }
            }
        named_feasible += nf;
    }
    qsort(lat, nq, sizeof *lat, cmp_u64);
    qsort(orc, nq, sizeof *orc, cmp_u64);
    qsort(hot, HOT_QUERIES, sizeof *hot, cmp_u64);
    qsort(cmp, nq, sizeof *cmp, cmp_u64);
    qsort(look, nq, sizeof *look, cmp_u64);
    row->compile_median_ns = cmp[nq / 2];
    row->lookup_median_ns = look[nq / 2];
    row->q_median_ns = lat[nq / 2];
    row->q_p99_ns = lat[(nq * 99) / 100];
    row->q_max_ns = lat[nq - 1];
    row->oracle_median_ns = orc[nq / 2];
    row->hot_median_ns = hot[HOT_QUERIES / 2];
    uint32_t answered = row->queries - row->no_op;
    row->probed_mean = answered ? (double)probed_sum / answered : 0;
    row->naive_named_desc_bytes = answered ? row->naive_named_desc_bytes / answered : 0;
    row->naive_named_precision = named_total ? (double)named_feasible / (double)named_total : 0;

    printf("    %6u capabilities (%5u operations): %u queries, %u with candidates; recall %llu/%llu, "
           "precision %llu/%llu, top-1 %u/%u\n",
           size, s.n_ops, row->queries, row->with_candidates, (unsigned long long)row->hit,
           (unsigned long long)row->expected, (unsigned long long)row->returned_ok,
           (unsigned long long)row->returned, row->top1_equal, row->with_candidates);
    printf("           query median %llu ns (compile %llu + lookup %llu) p99 %llu ns (hot operation, %u providers: %llu ns); "
           "full scan median %llu ns; probed %.1f/query (max %llu)\n",
           (unsigned long long)row->q_median_ns, (unsigned long long)row->compile_median_ns,
           (unsigned long long)row->lookup_median_ns, (unsigned long long)row->q_p99_ns, s.n_hot,
           (unsigned long long)row->hot_median_ns, (unsigned long long)row->oracle_median_ns,
           row->probed_mean, (unsigned long long)row->probed_max);
    printf("           into cognition: max %llu bytes/query; all descriptions %llu bytes; "
           "named-operation descriptions %llu bytes/query\n",
           (unsigned long long)row->bytes_max, (unsigned long long)row->desc_bytes,
           (unsigned long long)row->naive_named_desc_bytes);
    CHECK(row->list_mismatch == 0 && row->feasible_count_mismatch == 0 && row->front_mismatch == 0,
          "size %u: ranked lists %u, feasible counts %u, fronts %u differ from the full scan", size,
          row->list_mismatch, row->feasible_count_mismatch, row->front_mismatch);
    CHECK(row->hit == row->expected && row->returned_ok == row->returned &&
          row->top1_equal == row->with_candidates, "size %u: recall, precision, selection", size);
    CHECK(row->desc_bytes_read == 0, "size %u: the query read no description", size);
    CHECK(row->bytes_max <= offsetof(CqResult, cand) + CQ_MAX_K * sizeof(CqCandidate),
          "size %u: bytes into cognition bounded by K", size);
    CHECK(row->with_candidates >= 500, "size %u: at least 500 queries with candidates to compare (%u)",
          size, row->with_candidates);
    free(items);
    free(lat);
    free(orc);
    free(hot);
    free(cmp);
    free(look);
    synth_free(&s);
}

/* ---- placement and receipt ---- */

static void place(void) {
    cpu_set_t set;
    CPU_ZERO(&set);
    int count = 0;
    char *p = R.cpus;
    long n = sysconf(_SC_NPROCESSORS_CONF);
    for (long c = 0; c < n; c++) {
        char path[128];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%ld/regs/identification/midr_el1", c);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        unsigned long long midr = 0;
        int ok = fscanf(fp, "%llx", &midr) == 1;
        fclose(fp);
        if (ok && ((midr >> 4) & 0xfffu) == 0xd85u) {
            CPU_SET(c, &set);
            count++;
            if (p < R.cpus + sizeof R.cpus - 5) p += sprintf(p, "%s%ld", count > 1 ? "," : "", c);
        }
    }
    if (count == 0 || sched_setaffinity(0, sizeof set, &set) != 0) {
        snprintf(R.cpus, sizeof R.cpus, "unpinned");
        return;
    }
    printf("[*] placed on %d Cortex-X925 cores: %s\n", count, R.cpus);
}

static void binary_digest(char out[65]) {
    strcpy(out, "unavailable");
    FILE *fp = fopen("/proc/self/exe", "rb");
    if (!fp) return;
    sha256_ctx c;
    sha256_init(&c);
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, fp)) > 0) sha256_update(&c, buf, n);
    fclose(fp);
    uint8_t d[32];
    sha256_final(&c, d);
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", d[i]);
}

static int scaling_ok(void) {
    const SizeRow *a = &R.row[0], *z = &R.row[N_SIZES - 1];
    /* catalog grows 1000x; the query may grow at most 10x, the bytes not at all */
    const uint64_t bound = offsetof(CqResult, cand) + CQ_MAX_K * sizeof(CqCandidate);
    for (uint32_t i = 0; i < N_SIZES; i++)
        if (R.row[i].bytes_max > bound) return 0;
    return z->q_median_ns <= 10 * (a->q_median_ns ? a->q_median_ns : 1) &&
           z->oracle_median_ns > 100 * a->oracle_median_ns;
}

static void write_receipt(void) {
    char path[512];
    if (omega_evidence_path("CAPABILITY_QUERY/rx_capability_query_receipt.json", path, sizeof path) != 0)
        return;
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    char commit[41];
    memset(commit, 0, sizeof commit);
    if (!omega_evidence_run_commit(commit)) memcpy(commit, "unknown", 8);
    const char *candidate = getenv("OMEGA_CANDIDATE_COMMIT");
    int bound = candidate && candidate[0] && strcmp(candidate, commit) == 0 && !omega_evidence_tree_dirty();
    const char *aienos = getenv("AIENOS_COMMIT");
    char digest[65];
    binary_digest(digest);
    struct utsname u;
    memset(&u, 0, sizeof u);
    uname(&u);
    int scale = scaling_ok();
    int pass = g_fail == 0 && scale && R.mints_during_query == 0 && R.det_mismatch == 0;
    fprintf(fp,
            "{\n"
            "  \"schema\": \"OMEGA_CAPABILITY_QUERY_V1\",\n"
            "  \"run_id\": \"%s\",\n"
            "  \"candidate_commit\": %s%s%s,\n"
            "  \"candidate_bound\": %s,\n"
            "  \"run_commit\": \"%s\",\n"
            "  \"tree_dirty\": %s,\n"
            "  \"aienos_commit\": %s%s%s,\n"
            "  \"checks\": %d,\n"
            "  \"failures\": %d,\n"
            "  \"test_binary_sha256\": \"%s\",\n"
            "  \"host\": {\"sysname\": \"%s\", \"release\": \"%s\", \"machine\": \"%s\", \"cpus\": \"%s\"},\n"
            "  \"scope\": \"host processor; in-process synthetic catalogs; native AIENOS authority library\",\n"
            "  \"record_bytes\": {\"need\": %zu, \"tradeoffs\": %zu, \"candidate\": %zu, \"result_header\": %zu, "
            "\"max_candidates\": %u},\n"
            "  \"oracle\": \"full scan of every entry with an independently written match rule, Pareto front and selection\",\n"
            "  \"sizes\": [\n",
            omega_evidence_run_id(), candidate ? "\"" : "", candidate ? candidate : "null",
            candidate ? "\"" : "", bound ? "true" : "false", commit,
            omega_evidence_tree_dirty() ? "true" : "false", aienos ? "\"" : "", aienos ? aienos : "null",
            aienos ? "\"" : "", g_checks, g_fail, digest, u.sysname, u.release, u.machine, R.cpus,
            sizeof(CqNeed), sizeof(CqTradeoffs), sizeof(CqCandidate), offsetof(CqResult, cand), CQ_MAX_K);
    for (uint32_t i = 0; i < N_SIZES; i++) {
        const SizeRow *r = &R.row[i];
        fprintf(fp,
                "    {\"capabilities\": %u, \"operations\": %u, \"queries\": %u, \"with_candidates\": %u, "
                "\"unknown_operation\": %u,\n"
                "     \"candidate_recall\": {\"returned_in_oracle_ranking\": %llu, \"oracle_ranking\": %llu, \"value\": %.6f},\n"
                "     \"selection_precision\": {\"returned_feasible\": %llu, \"returned\": %llu, \"value\": %.6f,\n"
                "                             \"top1_equal\": %u, \"of\": %u, \"top1_value\": %.6f},\n"
                "     \"ranked_list_mismatches\": %u, \"feasible_count_mismatches\": %u, \"front_mismatches\": %u,\n"
                "     \"query_latency_ns\": {\"median\": %llu, \"p99\": %llu, \"max\": %llu, \"hot_operation_median\": %llu,\n\"compile_median\": %llu, \"lookup_and_rank_median\": %llu},\n"
                "     \"full_scan_median_ns\": %llu, \"entries_probed_mean\": %.2f, \"entries_probed_max\": %llu,\n"
                "     \"bytes_into_cognition\": {\"max_per_query\": %llu, \"mean_per_query\": %.1f, "
                "\"need_and_tradeoffs_out\": %llu},\n"
                "     \"description_bytes_read_by_queries\": %llu,\n"
                "     \"catalog_description_bytes\": %llu, \"catalog_bytes\": %llu, \"build_ms\": %.1f,\n"
                "     \"baseline_named_operation\": {\"description_bytes_per_query\": %llu, \"feasible_share\": %.4f}}%s\n",
                r->catalog, r->n_ops, r->queries, r->with_candidates, r->no_op,
                (unsigned long long)r->hit, (unsigned long long)r->expected,
                r->expected ? (double)r->hit / (double)r->expected : 1.0,
                (unsigned long long)r->returned_ok, (unsigned long long)r->returned,
                r->returned ? (double)r->returned_ok / (double)r->returned : 1.0, r->top1_equal,
                r->with_candidates, r->with_candidates ? (double)r->top1_equal / r->with_candidates : 1.0,
                r->list_mismatch, r->feasible_count_mismatch, r->front_mismatch,
                (unsigned long long)r->q_median_ns, (unsigned long long)r->q_p99_ns,
                (unsigned long long)r->q_max_ns, (unsigned long long)r->hot_median_ns,
                (unsigned long long)r->compile_median_ns, (unsigned long long)r->lookup_median_ns,
                (unsigned long long)r->oracle_median_ns, r->probed_mean, (unsigned long long)r->probed_max,
                (unsigned long long)r->bytes_max,
                (r->queries - r->no_op) ? (double)r->bytes_sum / (r->queries - r->no_op) : 0.0,
                (unsigned long long)r->need_bytes, (unsigned long long)r->desc_bytes_read,
                (unsigned long long)r->desc_bytes, (unsigned long long)r->catalog_bytes, r->build_ms,
                (unsigned long long)r->naive_named_desc_bytes, r->naive_named_precision,
                i + 1 < N_SIZES ? "," : "");
    }
    const SizeRow *a = &R.row[0], *z = &R.row[N_SIZES - 1];
    fprintf(fp,
            "  ],\n"
            "  \"scaling\": {\"catalog_growth\": %.0f, \"query_median_growth\": %.3f, "
            "\"full_scan_median_growth\": %.1f,\n"
            "              \"bytes_into_cognition_bound\": %zu, \"bytes_into_cognition_max_by_size\": [%llu, %llu, %llu, %llu],\n"
            "              \"criterion\": \"catalog x1000: query median at most x10, full scan more than x100, "
            "bytes into cognition under the same bound (header + K records) at every size\", \"met\": %s},\n"
            "  \"tradeoffs\": {\"distinct_winners_from_two_orders\": %u, \"single_relevance_score\": false},\n"
            "  \"determinism\": {\"runs\": %u, \"mismatches\": %u, \"registration_orders\": 2},\n"
            "  \"authority\": {\"held_validated_by_native_view\": %u, \"revoked_reported_not_held\": %u,\n"
            "                \"mints_during_query\": %llu, \"query_has_admin_handle\": false},\n"
            "  \"action_graph\": {\"winner_bound_and_ran\": %u, \"missing_authority_reported\": %u},\n"
            "  \"gates\": {\n"
            "    \"OMEGA_CAPABILITY_QUERY_PASS\": \"%s\",\n"
            "    \"not_claimed\": [\"live MCP sessions (the MCP registry is an in-process catalog)\", "
            "\"Fabric over a network (advertisements and leases are in-process)\", "
            "\"lexical, semantic or model-based retrieval (operations are matched by id, alias and specialization)\", "
            "\"the query as a resident reaction (it is a host function call)\", "
            "\"learned confidence (confidence and reliability are catalog values)\", "
            "\"graphics-processor execution\"]\n"
            "  }\n"
            "}\n",
            (double)z->catalog / a->catalog, (double)z->q_median_ns / (double)(a->q_median_ns ? a->q_median_ns : 1),
            (double)z->oracle_median_ns / (double)(a->oracle_median_ns ? a->oracle_median_ns : 1),
            offsetof(CqResult, cand) + CQ_MAX_K * sizeof(CqCandidate),
            (unsigned long long)R.row[0].bytes_max, (unsigned long long)R.row[1].bytes_max,
            (unsigned long long)R.row[2].bytes_max, (unsigned long long)R.row[3].bytes_max, scale ? "true" : "false",
            R.tradeoff_distinct_winners, R.det_runs, R.det_mismatch, R.held_validated, R.revoked_seen,
            (unsigned long long)R.mints_during_query, R.seam_skill_ran, R.graph_missing_reported,
            pass ? "PASS" : "FAIL");
    fclose(fp);
    printf("receipt: %s\n", path);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    place();
    t_resolution();
    t_constraints();
    t_tradeoffs();
    t_determinism();
    t_authority_and_graph();
    printf("[*] scaling against the full-scan oracle\n");
    for (uint32_t i = 0; i < N_SIZES; i++) t_scale(i);
    CHECK(scaling_ok(), "scaling criterion");
    printf("checks %d failures %d\n", g_checks, g_fail);
    write_receipt();
    return g_fail ? 1 : 0;
}
