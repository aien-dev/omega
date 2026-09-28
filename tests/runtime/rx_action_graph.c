/*
 * OMEGA_ACTION_GRAPH_IR -- the action graph compiled, optimized, lowered
 * onto resident reactions and run by readiness alone.
 *
 * Every scenario has its own world and its own native AIENOS authority. The
 * test plays the outside: it holds the admin (to give the principal the
 * authority it is born with, and to revoke), publishes World inputs and run
 * tokens, and plays the human who approves an AEGIS escalation. The graph
 * compiler is given the world and the principal's capability table only.
 *
 * For each run the lowered graph is compared with the sequential reference
 * (same node semantics, one node at a time in topological order): status,
 * value and evidence of every node, and the effect objects. Causal ancestry
 * is read back from the world's crumbs and compared with the graph.
 */
#include "runtime/aienos_cap.h"
#include "runtime/rx_aegis.h"
#include "runtime/rx_graph.h"
#include "runtime/rx_world.h"
#include "omega_evidence.h"
#include "omega_types.h"
#include "sha256.h"

#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

enum { SUBJ_EXTERNAL = 100, ISSUER = 3, SUBJ_PLAN = 61, SUBJ_OTHER = 62 };

#define RES_CELL    0xA000001ull
#define RES_RUN     0xA000002ull
#define RES_A       0xA000010ull
#define RES_B       0xA000011ull
#define RES_C       0xA000012ull
#define RES_MEM     0xA000013ull
#define RES_OUT     0xA000014ull
#define RES_EFFECT  0xA000020ull
#define RES_SECRET  0xA000030ull   /* the principal holds nothing here */

enum { GOAL_LINEAR = 1, GOAL_PARALLEL, GOAL_DIAMOND, GOAL_BRANCH, GOAL_FAILURE, GOAL_EFFECTS,
       GOAL_REDUNDANT, GOAL_RESOURCE };

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

/* ---- receipt figures ---- */
static struct {
    uint32_t runs_compared, nodes_compared, evidence_checked, evidence_missing;
    uint32_t ancestry_nodes, ancestry_mismatch;
    uint32_t det_runs, det_mismatch;
    uint32_t authority_blocked_crumbs, bypass;
    uint32_t aegis_mints_during_compile, aegis_mints_after_approval;
    uint64_t par_1w_ns, par_nw_ns, par_workers;
    uint64_t par_critical_us, par_total_us;
    uint32_t opt_before, opt_after, opt_react_before, opt_react_after;
    uint32_t opt_fold, opt_cse, opt_reads, opt_fused, opt_dead, opt_deps;
    uint64_t opt_ns_before, opt_ns_after, opt_crumbs_before, opt_crumbs_after;
    char cpus[64];
} R;

/* ---- skills (Skill Net stand-ins: deterministic procedures) ---- */

static uint64_t sk_combine(const uint64_t *in, uint32_t n, uint32_t attempt, int *failed) {
    (void)attempt;
    *failed = 0;
    uint64_t s = 7;
    for (uint32_t i = 0; i < n; i++) s += in[i] * (3 * (i + 1));
    return s;
}

static uint64_t sk_fetch(const uint64_t *in, uint32_t n, uint32_t attempt, int *failed) {
    (void)attempt;
    *failed = 0;
    return (n ? in[0] : 0) + 100;
}

static AgSkillTable g_skills;

static void skills_init(void) {
    memset(&g_skills, 0, sizeof g_skills);
    g_skills.n = 2;
    g_skills.skill[0] = (AgSkill){ 1, sk_combine, { 0x01 } };
    g_skills.skill[1] = (AgSkill){ 2, sk_fetch, { 0x02 } };
}

/* ---- environment ---- */

typedef struct {
    AienosCapAdmin *admin;
    AienosCapView *view;
    RxWorld w;
    int have_aegis;
    RxAegisFaculty a;
    RxObjRef A, B, C, mem, out, effect, secret;
    RxCapRef ext_run, ext_A, ext_B, ext_approval;
    RxCapRef cell_cap, run_cap, effect_cap;
    AgCapTable caps;
    AgLibrary lib;
    uint32_t n_lowered;
    const AgLowered *lowered[8];
} Env;

static RxCapRef mint(Env *e, uint32_t subject, uint64_t resource, uint32_t rights) {
    AienosCapRef office;
    aienos_cap_office(e->admin, &office);
    AienosCapMint m = { ISSUER, subject, resource, rights, 0, { UINT32_MAX, 0 }, office };
    AienosCapRef r = { UINT32_MAX, 0 };
    if (aienos_cap_mint(e->admin, &m, &r) != 0) r = (AienosCapRef){ UINT32_MAX, 0 };
    return (RxCapRef){ r.cap_id, r.generation };
}

static void hold(Env *e, RxCapRef ref, uint64_t res, uint32_t rights) {
    e->caps.cap[e->caps.n].ref = ref;
    e->caps.cap[e->caps.n].resource = res;
    e->caps.cap[e->caps.n].rights = rights;
    e->caps.n++;
}

static uint64_t fld(Env *e, RxObjRef r, uint32_t i) {
    RxObject o;
    if (rx_world_read(&e->w, r, &o) != RX_OK) return UINT64_MAX;
    return o.field[i];
}

static int settle(Env *e) { return rx_world_wait_quiescent(&e->w, 30000); }

/* grant_effect: whether the principal is born holding R|W|E on the effect. */
static int env_start(Env *e, uint32_t workers, int grant_effect, int with_aegis) {
    memset(e, 0, sizeof *e);
    if (aienos_cap_start(&e->admin, &e->view) != 0) return -1;
    if (rx_world_init_native(&e->w, e->view, workers, 1u << 18) != RX_OK) return -1;
    e->w.external_subject = SUBJ_EXTERNAL;
    const uint32_t R = RX_RIGHT_READ, W = RX_RIGHT_WRITE, RW = R | W;
    uint64_t z[RX_MAX_FIELDS] = { 0 };
    uint64_t a[RX_MAX_FIELDS] = { 40 }, b[RX_MAX_FIELDS] = { 7 }, c[RX_MAX_FIELDS] = { 3 };
    uint64_t m[RX_MAX_FIELDS] = { 11, 500, 12, 600, 13, 700, 0, 0 };
    if (rx_world_create(&e->w, 0xA01, RX_PERSIST_RESIDENT, RES_A, a, &e->A) != RX_OK ||
        rx_world_create(&e->w, 0xA02, RX_PERSIST_RESIDENT, RES_B, b, &e->B) != RX_OK ||
        rx_world_create(&e->w, 0xA03, RX_PERSIST_RESIDENT, RES_C, c, &e->C) != RX_OK ||
        rx_world_create(&e->w, AG_OT_MEMORY, RX_PERSIST_RESIDENT, RES_MEM, m, &e->mem) != RX_OK ||
        rx_world_create(&e->w, 0xA05, RX_PERSIST_RESIDENT, RES_OUT, z, &e->out) != RX_OK ||
        rx_world_create(&e->w, AG_OT_EFFECT, RX_PERSIST_RESIDENT, RES_EFFECT, z, &e->effect) != RX_OK ||
        rx_world_create(&e->w, 0xA07, RX_PERSIST_RESIDENT, RES_SECRET, z, &e->secret) != RX_OK)
        return -1;
    e->ext_run = mint(e, SUBJ_EXTERNAL, RES_RUN, W);
    e->ext_A = mint(e, SUBJ_EXTERNAL, RES_A, W);
    e->ext_B = mint(e, SUBJ_EXTERNAL, RES_B, W);
    e->caps.subject = SUBJ_PLAN;
    e->cell_cap = mint(e, SUBJ_PLAN, RES_CELL, RW);
    e->run_cap = mint(e, SUBJ_PLAN, RES_RUN, R);
    hold(e, mint(e, SUBJ_PLAN, RES_A, R), RES_A, R);
    hold(e, mint(e, SUBJ_PLAN, RES_B, R), RES_B, R);
    hold(e, mint(e, SUBJ_PLAN, RES_C, R), RES_C, R);
    hold(e, mint(e, SUBJ_PLAN, RES_MEM, R), RES_MEM, R);
    hold(e, mint(e, SUBJ_PLAN, RES_OUT, RW), RES_OUT, RW);
    if (grant_effect) {
        e->effect_cap = mint(e, SUBJ_PLAN, RES_EFFECT, RW | RX_RIGHT_EFFECT);
        hold(e, e->effect_cap, RES_EFFECT, RW | RX_RIGHT_EFFECT);
    }
    if (with_aegis) {
        RxAegisPolicy pol;
        memset(&pol, 0, sizeof pol);
        pol.n_rules = 1;
        pol.rules[0] = (RxAegisRule){ 1, SUBJ_PLAN, RES_EFFECT, RES_EFFECT,
                                      RW | RX_RIGHT_EFFECT, 0, 1 };  /* a human must approve */
        RxAegisClient cl = { SUBJ_PLAN, 0xA000000, 0xA0000ff, RW | RX_RIGHT_EFFECT };
        if (rx_aegis_create(&e->a, &e->w, e->admin, &pol, &cl, 1) != RX_OK) return -1;
        RxAegisCaps ac;
        ac.aegis_request = mint(e, RX_AEGIS_SUBJ, rx_aegis_res(0, RX_AEGIS_RES_REQUEST), R);
        ac.aegis_approval = mint(e, RX_AEGIS_SUBJ, rx_aegis_res(0, RX_AEGIS_RES_APPROVAL), R);
        ac.aegis_decision = mint(e, RX_AEGIS_SUBJ, rx_aegis_res(0, RX_AEGIS_RES_DECISION), RW);
        ac.root_request = mint(e, RX_AEGIS_ROOT_SUBJ, rx_aegis_res(0, RX_AEGIS_RES_REQUEST), R);
        ac.root_decision = mint(e, RX_AEGIS_ROOT_SUBJ, rx_aegis_res(0, RX_AEGIS_RES_DECISION), R);
        for (uint32_t j = 0; j < RX_AEGIS_SLOTS; j++)
            ac.root_slot[j] = mint(e, RX_AEGIS_ROOT_SUBJ, rx_aegis_res(0, RX_AEGIS_RES_SLOT0 + j), RW);
        if (rx_aegis_register(&e->a, &ac) != RX_OK) return -1;
        e->ext_approval = mint(e, SUBJ_EXTERNAL, rx_aegis_res(0, RX_AEGIS_RES_APPROVAL), W);
        e->have_aegis = 1;
    }
    return 0;
}

static uint32_t bypasses(Env *e);

static void env_stop(Env *e) {
    rx_world_wait_quiescent(&e->w, 30000);
    R.bypass += bypasses(e);
    rx_world_destroy(&e->w);
    if (e->have_aegis) rx_aegis_destroy(&e->a);
    aienos_cap_stop(e->admin, e->view);
}

static int lower(Env *e, AgLowered *L, const AgGraph *g, const AgAegisBinding *ab) {
    if (e->n_lowered < 8) e->lowered[e->n_lowered++] = L;
    return rx_graph_lower(L, &e->w, g, &g_skills, &e->caps, RES_CELL, e->cell_cap, RES_RUN,
                          e->run_cap, ab);
}

/* ---- procedures (graph templates; World objects are parameters) ----
 * Parameters: 1 A, 2 B, 3 C, 4 memory, 5 out, 6 effect, 7 secret. */

static AgGraph T_linear, T_parallel, T_diamond, T_branch, T_failure, T_effects, T_redundant,
               T_resource;

static int N(AgGraph *g, AgKind k, AgType t) { return rx_graph_node(g, k, t); }

static int node_read(AgGraph *g, uint32_t param, uint32_t field, uint32_t cost) {
    int n = N(g, AG_WORLD_READ, AG_T_U64);
    g->nodes[n].param = param;
    g->nodes[n].field = field;
    g->nodes[n].cost_us = cost;
    return n;
}

static int node_pure(AgGraph *g, uint32_t op, int a, int b, uint64_t imm) {
    int n = N(g, AG_PURE, (op == OP_LESS_THAN || op == OP_EQUAL) ? AG_T_BOOL : AG_T_U64);
    g->nodes[n].op = op;
    g->nodes[n].imm = imm;
    if (a >= 0) rx_graph_data(g, (uint32_t)a, (uint32_t)n, 0, AG_EDGE_DATA);
    if (b >= 0) rx_graph_data(g, (uint32_t)b, (uint32_t)n, 1, AG_EDGE_DATA);
    return n;
}

static int node_verify(AgGraph *g, int in, uint64_t lo, uint64_t hi) {
    int n = N(g, AG_VERIFY, AG_T_VERDICT);
    g->nodes[n].imm = lo;
    g->nodes[n].imm2 = hi;
    rx_graph_data(g, (uint32_t)in, (uint32_t)n, 0, AG_EDGE_DATA);
    return n;
}

static int node_perform(AgGraph *g, int proposal, int verdict) {
    int n = N(g, AG_EFFECT_PERFORM, AG_T_RECEIPT);
    g->nodes[n].param = 6;
    rx_graph_data(g, (uint32_t)proposal, (uint32_t)n, 0, AG_EDGE_DATA);
    rx_graph_data(g, (uint32_t)verdict, (uint32_t)n, 1, AG_EDGE_DATA);
    return n;
}

static int node_propose(AgGraph *g, int in) {
    int n = N(g, AG_EFFECT_PROPOSE, AG_T_PROPOSAL);
    rx_graph_data(g, (uint32_t)in, (uint32_t)n, 0, AG_EDGE_DATA);
    return n;
}

static int node_publish(AgGraph *g, int in, uint32_t param, uint32_t field) {
    int n = N(g, AG_WORLD_PUBLISH, AG_T_RECEIPT);
    g->nodes[n].param = param;
    g->nodes[n].field = field;
    rx_graph_data(g, (uint32_t)in, (uint32_t)n, 0, AG_EDGE_DATA);
    return n;
}

static void build_templates(uint32_t cost) {
    AgGraph *g;
    int a, b, c, d, v, p, f;

    /* linear: A -> +5 -> *3 -> verify -> propose -> perform */
    g = &T_linear;
    rx_graph_init(g, 0);
    a = node_read(g, 1, 0, 0);
    b = node_pure(g, OP_ADD, a, -1, 5);
    c = node_pure(g, OP_MUL, b, -1, 3);
    v = node_verify(g, c, 0, 1000);
    p = node_propose(g, c);
    f = node_perform(g, p, v);
    rx_graph_success(g, (uint32_t)f, AG_OK);
    rx_graph_failure(g, (uint32_t)v, AG_FAILED);

    /* parallel: recall + three World reads, each `cost` of real work, combine */
    g = &T_parallel;
    rx_graph_init(g, 0);
    int r0 = N(g, AG_RECALL, AG_T_U64);
    g->nodes[r0].param = 4;
    g->nodes[r0].imm = 11;
    g->nodes[r0].cost_us = cost;
    int r1 = node_read(g, 1, 0, cost), r2 = node_read(g, 2, 0, cost), r3 = node_read(g, 3, 0, cost);
    int sk = N(g, AG_SKILL, AG_T_U64);
    g->nodes[sk].op = 1;
    rx_graph_data(g, (uint32_t)r0, (uint32_t)sk, 0, AG_EDGE_DATA);
    rx_graph_data(g, (uint32_t)r1, (uint32_t)sk, 1, AG_EDGE_DATA);
    rx_graph_data(g, (uint32_t)r2, (uint32_t)sk, 2, AG_EDGE_DATA);
    rx_graph_data(g, (uint32_t)r3, (uint32_t)sk, 3, AG_EDGE_DATA);
    v = node_verify(g, sk, 0, 1ull << 40);
    p = node_propose(g, sk);
    f = node_perform(g, p, v);
    rx_graph_success(g, (uint32_t)f, AG_OK);

    /* diamond: A -> (+1, *2) -> add -> verify, publish after verify */
    g = &T_diamond;
    rx_graph_init(g, 0);
    a = node_read(g, 1, 0, 0);
    b = node_pure(g, OP_ADD, a, -1, 1);
    c = node_pure(g, OP_MUL, a, -1, 2);
    d = node_pure(g, OP_ADD, b, c, 0);
    v = node_verify(g, d, 0, 10000);
    f = node_publish(g, d, 5, 0);
    rx_graph_order(g, (uint32_t)v, (uint32_t)f);
    rx_graph_success(g, (uint32_t)f, AG_OK);

    /* branch/join: A < 50 ? A*2 : A+100 */
    g = &T_branch;
    rx_graph_init(g, 0);
    a = node_read(g, 1, 0, 0);
    b = node_pure(g, OP_LESS_THAN, a, -1, 50);
    int br = N(g, AG_BRANCH, AG_T_BOOL);
    rx_graph_data(g, (uint32_t)b, (uint32_t)br, 0, AG_EDGE_DATA);
    int t = node_pure(g, OP_MUL, a, -1, 2), e = node_pure(g, OP_ADD, a, -1, 100);
    rx_graph_guard(g, (uint32_t)br, (uint32_t)t, 1);
    rx_graph_guard(g, (uint32_t)br, (uint32_t)e, 0);
    int j = N(g, AG_JOIN, AG_T_U64);
    rx_graph_data(g, (uint32_t)t, (uint32_t)j, 0, AG_EDGE_DATA);
    rx_graph_data(g, (uint32_t)e, (uint32_t)j, 1, AG_EDGE_DATA);
    v = node_verify(g, j, 0, 1000);
    p = node_propose(g, j);
    f = node_perform(g, p, v);
    rx_graph_success(g, (uint32_t)f, AG_OK);

    /* failure branch: recall a key memory does not hold; the failure arm
     * supplies a fallback; a bounded retry follows */
    g = &T_failure;
    rx_graph_init(g, 0);
    int rc = N(g, AG_RECALL, AG_T_U64);
    g->nodes[rc].param = 4;
    g->nodes[rc].imm = 99;
    b = node_pure(g, OP_ADD, rc, -1, 1);
    int fb = N(g, AG_PURE, AG_T_U64);
    g->nodes[fb].op = OP_CONSTANT;
    g->nodes[fb].imm = 777;
    rx_graph_data(g, (uint32_t)rc, (uint32_t)fb, 0, AG_EDGE_ON_FAIL);
    j = N(g, AG_JOIN, AG_T_U64);
    rx_graph_data(g, (uint32_t)b, (uint32_t)j, 0, AG_EDGE_DATA);
    rx_graph_data(g, (uint32_t)fb, (uint32_t)j, 1, AG_EDGE_DATA);
    int rt = N(g, AG_RETRY, AG_T_U64);
    g->nodes[rt].op = 2;
    g->nodes[rt].imm = 3;       /* at most 3 attempts */
    g->nodes[rt].imm2 = 2;      /* the stand-in fails the first 2 */
    rx_graph_data(g, (uint32_t)j, (uint32_t)rt, 0, AG_EDGE_DATA);
    v = node_verify(g, rt, 0, 1000);
    f = node_publish(g, rt, 5, 1);
    rx_graph_order(g, (uint32_t)v, (uint32_t)f);
    rx_graph_success(g, (uint32_t)f, AG_OK);

    /* effect boundary: two identical performs, a publication, and reads of
     * the published object before and after it */
    g = &T_effects;
    rx_graph_init(g, 0);
    a = node_read(g, 1, 0, 0);
    p = node_propose(g, a);
    v = node_verify(g, a, 0, 1000);
    int f1 = node_perform(g, p, v), f2 = node_perform(g, p, v);
    rx_graph_order(g, (uint32_t)f1, (uint32_t)f2);
    int pub = node_publish(g, a, 5, 2);
    rx_graph_order(g, (uint32_t)f2, (uint32_t)pub);
    int after = node_read(g, 5, 2, 0), before = node_read(g, 5, 2, 0);
    rx_graph_order(g, (uint32_t)pub, (uint32_t)after);
    rx_graph_order(g, (uint32_t)before, (uint32_t)pub);
    d = node_pure(g, OP_SUB, after, before, 0);
    v = node_verify(g, d, 0, 1ull << 62);
    rx_graph_success(g, (uint32_t)v, AG_OK);
    rx_graph_success(g, (uint32_t)pub, AG_OK);

    /* redundant plan: what a planner emits before Omega cleans it up */
    g = &T_redundant;
    rx_graph_init(g, 0);
    int n0 = node_read(g, 1, 0, 0), n1 = node_read(g, 1, 0, 0);
    int n2 = N(g, AG_CONST, AG_T_U64), n3 = N(g, AG_CONST, AG_T_U64);
    g->nodes[n2].imm = 6;
    g->nodes[n3].imm = 7;
    int n4 = node_pure(g, OP_MUL, n2, n3, 0);
    int n5 = node_pure(g, OP_ADD, n0, n4, 0), n6 = node_pure(g, OP_ADD, n1, n4, 0);
    int n7 = node_pure(g, OP_MUL, n5, -1, 2), n8 = node_pure(g, OP_ADD, n7, -1, 1);
    int n9 = node_pure(g, OP_ADD, n8, n6, 0), n10 = node_pure(g, OP_SUB, n9, -1, 3);
    int n11 = node_verify(g, n10, 0, 100000);
    int n12 = node_propose(g, n10);
    int n13 = node_perform(g, n12, n11);
    int n14 = node_read(g, 2, 0, 0);
    node_pure(g, OP_ADD, n14, -1, 1);                      /* unused diagnostic */
    rx_graph_order(g, (uint32_t)n14, (uint32_t)n5);        /* false order */
    rx_graph_order(g, (uint32_t)n5, (uint32_t)n9);         /* implied order */
    rx_graph_success(g, (uint32_t)n13, AG_OK);

    /* resource: a physical node needs more memory than the body offers */
    g = &T_resource;
    rx_graph_init(g, 0);
    a = node_read(g, 1, 0, 0);
    b = node_read(g, 2, 0, 0);
    int ph = N(g, AG_PHYSICAL, AG_T_U64);
    g->nodes[ph].imm = 5;
    rx_graph_data(g, (uint32_t)b, (uint32_t)ph, 0, AG_EDGE_DATA);
    RxResourceNeed need;
    memset(&need, 0, sizeof need);
    need.memory_bytes = 1ull << 40;
    rx_graph_need_resource(g, (uint32_t)ph, &need);
    c = node_pure(g, OP_ADD, a, ph, 0);
    v = node_verify(g, c, 0, UINT64_MAX);
    f = node_publish(g, c, 5, 3);
    rx_graph_order(g, (uint32_t)v, (uint32_t)f);
    int ind = node_pure(g, OP_ADD, a, -1, 1);
    int v2 = node_verify(g, ind, 0, 1000);
    rx_graph_success(g, (uint32_t)f, AG_OK);
    rx_graph_success(g, (uint32_t)v2, AG_OK);
}

static void library(Env *e) {
    AgLibrary *l = &e->lib;
    memset(l, 0, sizeof *l);
    const AgGraph *t[] = { &T_linear, &T_parallel, &T_diamond, &T_branch, &T_failure,
                           &T_effects, &T_redundant, &T_resource };
    for (uint32_t i = 0; i < 8; i++) l->proc[l->n++] = (AgProcedure){ i + 1, t[i] };
}

static AgGoal goal_for(Env *e, uint32_t kind) {
    AgGoal g = { kind, 7, { e->A, e->B, e->C, e->mem, e->out, e->effect, e->secret } };
    return g;
}

static AgConstraints open_constraints(void) {
    AgConstraints c;
    memset(&c, 0, sizeof c);
    c.max_effects = UINT32_MAX;
    c.budget.slots = RX_MAX_REACTIONS;
    c.budget.memory_bytes = UINT64_MAX;
    c.budget.energy_budget = UINT64_MAX;
    c.budget.offered_locality = UINT32_MAX;
    c.budget.offered_accel = UINT32_MAX;
    c.budget.compute_mask = UINT32_MAX;
    return c;
}

static int compile(Env *e, uint32_t kind, int optimize, AgGraph *g, AgReport *rep) {
    AgGoal goal = goal_for(e, kind);
    AgConstraints c = open_constraints();
    return rx_graph_compile(&goal, &e->w, &e->caps, &c, &e->lib, optimize, g, rep);
}

/* ---- running, comparing, ancestry ---- */

static int64_t run(Env *e, AgLowered *L, uint64_t id, AgResult *res, uint64_t *ns) {
    uint64_t t0 = now_ns();
    int64_t ext = rx_graph_start(L, e->ext_run, id);
    CHECK(ext > 0, "run token refused");
    CHECK(settle(e) == RX_OK, "settle");
    if (ns) *ns = now_ns() - t0;
    rx_graph_collect(L, id, res);
    return ext;
}

/* Every node's status/value/evidence, the outcome, and the effect object. */
static void compare(Env *e, const AgGraph *g, const AgResult *got, const AgReference *ref,
                    const char *what) {
    R.runs_compared++;
    int bad = 0;
    for (uint32_t n = 0; n < g->n_nodes; n++) {
        R.nodes_compared++;
        if (got->status[n] != ref->r.status[n] || got->value[n] != ref->r.value[n] ||
            (got->status[n] && got->evidence[n] != ref->r.evidence[n])) {
            bad = 1;
            fprintf(stderr, "    %s node %u (%s): got s%u v%llu, reference s%u v%llu\n", what, n,
                    rx_graph_kind_name(g->nodes[n].kind), got->status[n],
                    (unsigned long long)got->value[n], ref->r.status[n],
                    (unsigned long long)ref->r.value[n]);
        }
    }
    CHECK(!bad, "%s: lowered run differs from the sequential reference", what);
    CHECK(got->outcome == ref->r.outcome, "%s: outcome %d vs reference %d", what, got->outcome,
          ref->r.outcome);
    uint32_t last_perform = UINT32_MAX;
    for (uint32_t i = 0; i < g->n_effects; i++)
        if (g->nodes[g->effects[i]].kind == AG_EFFECT_PERFORM) last_perform = i;
    if (last_perform != UINT32_MAX)
        CHECK(fld(e, e->effect, 2) == ref->effect_chain[last_perform],
              "%s: effect order chain differs from the reference", what);
    /* Evidence: every evidence node of a completed run has its word. */
    if (got->outcome == AG_RUN_SUCCESS)
        for (uint32_t i = 0; i < g->n_evidence; i++) {
            uint32_t n = g->evidence[i];
            R.evidence_checked++;
            if (got->status[n] == AG_PENDING || got->evidence[n] == 0) R.evidence_missing++;
        }
}

static AgReference g_ref;

static void reference(Env *e, const AgGraph *g, uint64_t id) {
    CHECK(rx_graph_reference(g, &e->w, &g_skills, &e->caps, id, &g_ref) == 0, "reference");
}

/* Crumbs of this run (episode = the run token's EXTERNAL crumb): for each
 * node, walk its commit's parents inside the episode and compare the nodes
 * reached with the graph's own ancestors. */
static void ancestry(Env *e, const AgLowered *L, int64_t ext, const char *what) {
    const AgGraph *g = L->g;
    RxWorld *w = &e->w;
    int32_t node_of[RX_MAX_REACTIONS];
    for (uint32_t i = 0; i < RX_MAX_REACTIONS; i++) node_of[i] = -1;
    for (uint32_t n = 0; n < g->n_nodes; n++) node_of[L->reaction[n]] = (int32_t)n;
    uint64_t *stack = calloc(w->n_crumbs + 1, sizeof(uint64_t));
    uint8_t *seen = calloc(w->n_crumbs + 1, 1);
    int bad = 0;
    for (uint32_t n = 0; n < g->n_nodes; n++) {
        uint64_t mine = 0;
        for (uint64_t id = 1; id <= w->n_crumbs; id++) {
            const RxCrumb *k = rx_world_crumb(w, id);
            if (k->episode == (uint64_t)ext && k->kind == RX_CRUMB_COMMIT && k->reaction == L->reaction[n])
                mine = id;
        }
        if (!mine) { bad = 1; continue; }
        memset(seen, 0, w->n_crumbs + 1);
        uint64_t sp = 0, got = 0;
        int reached_run = 0;
        stack[sp++] = mine;
        seen[mine] = 1;
        while (sp) {
            const RxCrumb *k = rx_world_crumb(w, stack[--sp]);
            for (uint32_t i = 0; i < k->n_parents; i++) {
                uint64_t p = k->parents[i];
                if (!p || seen[p]) continue;
                const RxCrumb *q = rx_world_crumb(w, p);
                if (!q || q->episode != (uint64_t)ext) continue;
                seen[p] = 1;
                if (p == (uint64_t)ext) reached_run = 1;
                if (q->reaction < RX_MAX_REACTIONS && node_of[q->reaction] >= 0)
                    got |= 1ull << node_of[q->reaction];
                stack[sp++] = p;
            }
        }
        R.ancestry_nodes++;
        if (got != rx_graph_ancestors(g, n) || !reached_run) {
            bad = 1;
            R.ancestry_mismatch++;
            fprintf(stderr, "    %s node %u: crumb ancestry %llx, graph %llx, run %d\n", what, n,
                    (unsigned long long)got, (unsigned long long)rx_graph_ancestors(g, n),
                    reached_run);
        }
    }
    free(stack);
    free(seen);
    CHECK(!bad, "%s: causal ancestry differs from the graph", what);
}

/* Semantic result of a run: every node's status and value, and the effect
 * object. Not timing, not crumb ids. */
static void semantic_digest(Env *e, const AgGraph *g, const AgResult *r, uint8_t out[32]) {
    sha256_ctx c;
    sha256_init(&c);
    for (uint32_t n = 0; n < g->n_nodes; n++) {
        sha256_update(&c, &r->status[n], 1);
        sha256_update(&c, (const uint8_t *)&r->value[n], 8);
        sha256_update(&c, (const uint8_t *)&r->evidence[n], 8);
    }
    for (uint32_t f = 0; f < 4; f++) {
        uint64_t v = fld(e, e->effect, f);
        sha256_update(&c, (const uint8_t *)&v, 8);
    }
    sha256_final(&c, out);
}

/* Commits to boundary targets must come from lowered boundary reactions
 * holding valid authority. Counts anything else. */
static uint32_t bypasses(Env *e) {
    uint32_t bad = 0;
    for (uint64_t id = 1; id <= e->w.n_crumbs; id++) {
        const RxCrumb *k = rx_world_crumb(&e->w, id);
        if (k->kind != RX_CRUMB_COMMIT) continue;
        for (uint32_t o = 0; o < k->n_outputs; o++) {
            uint32_t oid = k->outputs[o].obj.id;
            if (oid != e->effect.id && oid != e->out.id && oid != e->secret.id) continue;
            int legit = 0;
            for (uint32_t l = 0; l < e->n_lowered; l++) {
                const AgLowered *L = e->lowered[l];
                for (uint32_t n = 0; n < L->g->n_nodes; n++)
                    if (L->reaction[n] == k->reaction &&
                        (L->g->nodes[n].kind == AG_EFFECT_PERFORM ||
                         L->g->nodes[n].kind == AG_WORLD_PUBLISH) &&
                        L->g->nodes[n].obj.id == oid)
                        legit = 1;
            }
            if (!legit) bad++;
        }
    }
    return bad;
}

static uint32_t count_crumbs(Env *e, uint32_t reaction, RxCrumbKind kind) {
    uint32_t n = 0;
    for (uint64_t id = 1; id <= e->w.n_crumbs; id++) {
        const RxCrumb *k = rx_world_crumb(&e->w, id);
        if (k->reaction == reaction && k->kind == kind) n++;
    }
    return n;
}

static int node_of_kind(const AgGraph *g, AgKind k) {
    for (uint32_t n = 0; n < g->n_nodes; n++)
        if (g->nodes[n].kind == k) return (int)n;
    return -1;
}

/* Compile, lower, run with reference and ancestry. */
static void full_run(Env *e, AgLowered *L, uint64_t id, const char *what, AgResult *res) {
    reference(e, L->g, id);
    int64_t ext = run(e, L, id, res, NULL);
    compare(e, L->g, res, &g_ref, what);
    ancestry(e, L, ext, what);
}

/* ---- scenarios ---- */

static void t_linear(void) {
    printf("[*] linear graph\n");
    Env e;
    CHECK(env_start(&e, 4, 1, 0) == 0, "env");
    library(&e);
    AgGraph g;
    AgReport rep;
    CHECK(compile(&e, GOAL_LINEAR, 1, &g, &rep) == AG_OK_READY, "compile %d", rep.verdict);
    CHECK(rep.n_missing == 0 && rep.nodes_before == 6 && rep.nodes_after == 5 && rep.passes.fused == 1,
          "linear report missing %u nodes %u->%u fused %u", rep.n_missing, rep.nodes_before,
          rep.nodes_after, rep.passes.fused);
    AgLowered L;
    CHECK(lower(&e, &L, &g, NULL) == 0, "lower");
    AgResult res;
    for (uint64_t r = 1; r <= 3; r++) full_run(&e, &L, r, "linear", &res);
    CHECK(res.outcome == AG_RUN_SUCCESS && res.value[node_of_kind(&g, AG_PURE)] == 135,
          "linear: (40+5)*3 = 135");
    CHECK(fld(&e, e.effect, 0) == 3 && fld(&e, e.effect, 1) == 135, "three performed effects");
    env_stop(&e);
}

static void t_diamond(void) {
    printf("[*] diamond graph\n");
    Env e;
    CHECK(env_start(&e, 4, 1, 0) == 0, "env");
    library(&e);
    AgGraph g;
    AgReport rep;
    CHECK(compile(&e, GOAL_DIAMOND, 1, &g, &rep) == AG_OK_READY, "compile %d", rep.verdict);
    AgLowered L;
    CHECK(lower(&e, &L, &g, NULL) == 0, "lower");
    AgResult res;
    full_run(&e, &L, 1, "diamond", &res);
    CHECK(res.outcome == AG_RUN_SUCCESS && fld(&e, e.out, 0) == 41 + 80, "diamond publishes 121");
    RxMutation m = { e.A, 0, 5 };
    CHECK(rx_world_publish_external(&e.w, e.ext_A, &m, 1) > 0, "A");
    settle(&e);
    full_run(&e, &L, 2, "diamond", &res);
    CHECK(fld(&e, e.out, 0) == 6 + 10, "diamond follows A: 16");
    env_stop(&e);
}

static void t_branch(void) {
    printf("[*] branch/join graph: both arms\n");
    Env e;
    CHECK(env_start(&e, 4, 1, 0) == 0, "env");
    library(&e);
    AgGraph g;
    AgReport rep;
    CHECK(compile(&e, GOAL_BRANCH, 1, &g, &rep) == AG_OK_READY, "compile %d", rep.verdict);
    AgLowered L;
    CHECK(lower(&e, &L, &g, NULL) == 0, "lower");
    AgResult res;
    full_run(&e, &L, 1, "branch A=40", &res);
    CHECK(res.value[node_of_kind(&g, AG_JOIN)] == 80, "A=40 takes the *2 arm");
    RxMutation m = { e.A, 0, 70 };
    CHECK(rx_world_publish_external(&e.w, e.ext_A, &m, 1) > 0, "A");
    settle(&e);
    full_run(&e, &L, 2, "branch A=70", &res);
    CHECK(res.value[node_of_kind(&g, AG_JOIN)] == 170, "A=70 takes the +100 arm");
    uint32_t skipped = 0;
    for (uint32_t n = 0; n < g.n_nodes; n++) skipped += res.status[n] == AG_SKIPPED;
    CHECK(skipped == 1, "exactly the untaken arm is skipped (%u)", skipped);
    env_stop(&e);
}

static void t_failure(void) {
    printf("[*] failure branch and bounded retry\n");
    Env e;
    CHECK(env_start(&e, 4, 1, 0) == 0, "env");
    library(&e);
    AgGraph g;
    AgReport rep;
    CHECK(compile(&e, GOAL_FAILURE, 1, &g, &rep) == AG_OK_READY, "compile %d", rep.verdict);
    AgLowered L;
    CHECK(lower(&e, &L, &g, NULL) == 0, "lower");
    AgResult res;
    full_run(&e, &L, 1, "failure", &res);
    int rc = node_of_kind(&g, AG_RECALL), rt = node_of_kind(&g, AG_RETRY);
    CHECK(res.status[rc] == AG_FAILED, "recall of an unknown key fails");
    CHECK(res.outcome == AG_RUN_SUCCESS && fld(&e, e.out, 1) == 877, "fallback 777 + fetch 100");
    CHECK(res.attempts[rt] == 3, "retry took 3 attempts (%u)", res.attempts[rt]);

    /* Exhausted retry: a declared failure condition makes the run FAILURE. */
    AgGraph x;
    rx_graph_init(&x, SUBJ_PLAN);
    int c = N(&x, AG_CONST, AG_T_U64);
    x.nodes[c].imm = 5;
    int r = N(&x, AG_RETRY, AG_T_U64);
    x.nodes[r].op = 2;
    x.nodes[r].imm = 2;
    x.nodes[r].imm2 = 5;
    rx_graph_data(&x, (uint32_t)c, (uint32_t)r, 0, AG_EDGE_DATA);
    int v = node_verify(&x, r, 0, 1000);
    rx_graph_success(&x, (uint32_t)v, AG_OK);
    rx_graph_failure(&x, (uint32_t)r, AG_FAILED);
    CHECK(rx_graph_validate(&x, &e.w) == 0, "retry graph valid");
    AgLowered L2;
    CHECK(lower(&e, &L2, &x, NULL) == 0, "lower");
    full_run(&e, &L2, 1, "retry exhausted", &res);
    CHECK(res.outcome == AG_RUN_FAILURE && res.attempts[r] == 2 && res.status[v] == AG_FAILED,
          "exhausted retry fails the run after 2 attempts");
    env_stop(&e);
}

static void t_effects(void) {
    printf("[*] effect boundary\n");
    Env e;
    CHECK(env_start(&e, 4, 1, 0) == 0, "env");
    library(&e);
    AgGraph g;
    AgReport rep;
    CHECK(compile(&e, GOAL_EFFECTS, 1, &g, &rep) == AG_OK_READY, "compile %d", rep.verdict);
    uint32_t performs = 0, reads_out = 0;
    for (uint32_t n = 0; n < g.n_nodes; n++) {
        performs += g.nodes[n].kind == AG_EFFECT_PERFORM;
        reads_out += g.nodes[n].kind == AG_WORLD_READ && g.nodes[n].obj.id == e.out.id;
    }
    CHECK(performs == 2, "identical performs are never merged (%u)", performs);
    CHECK(reads_out == 2, "reads across a publication of their object are not merged (%u)", reads_out);
    CHECK(g.n_effects == 3, "three boundaries in the chain");

    AgLowered L;
    CHECK(lower(&e, &L, &g, NULL) == 0, "lower");
    AgResult res;
    full_run(&e, &L, 1, "effects", &res);
    CHECK(res.outcome == AG_RUN_SUCCESS && fld(&e, e.effect, 0) == 2 && fld(&e, e.out, 2) == 40,
          "both effects performed in order, then the publication");
    CHECK(res.value[node_of_kind(&g, AG_PURE)] == 40, "read after publish sees it, read before does not");

    /* Thinking about an effect performs nothing. */
    AgGraph t;
    rx_graph_init(&t, SUBJ_PLAN);
    int a = N(&t, AG_WORLD_READ, AG_T_U64);
    t.nodes[a].obj = e.A;
    int p = node_propose(&t, a);
    int v = node_verify(&t, a, 0, 1000);
    rx_graph_success(&t, (uint32_t)p, AG_OK);
    rx_graph_success(&t, (uint32_t)v, AG_OK);
    CHECK(rx_graph_validate(&t, &e.w) == 0 && t.n_effects == 0, "proposal graph has no boundary");
    AgLowered L2;
    CHECK(lower(&e, &L2, &t, NULL) == 0, "lower");
    uint64_t before[4];
    for (uint32_t f = 0; f < 4; f++) before[f] = fld(&e, e.effect, f);
    full_run(&e, &L2, 1, "propose only", &res);
    int same = 1;
    for (uint32_t f = 0; f < 4; f++) same &= before[f] == fld(&e, e.effect, f);
    CHECK(res.outcome == AG_RUN_SUCCESS && res.value[p] == 40 && same,
          "a proposal is a value; the effect object is untouched");

    /* Constraints: the goal may forbid effects or resources outright. */
    AgGoal goal = goal_for(&e, GOAL_EFFECTS);
    AgConstraints c = open_constraints();
    c.max_effects = 1;
    CHECK(rx_graph_compile(&goal, &e.w, &e.caps, &c, &e.lib, 1, &t, &rep) == AG_E_CONSTRAINT,
          "two performs exceed max_effects 1");
    c = open_constraints();
    c.forbid_lo = RES_EFFECT;
    c.forbid_hi = RES_EFFECT;
    CHECK(rx_graph_compile(&goal, &e.w, &e.caps, &c, &e.lib, 1, &t, &rep) == AG_E_CONSTRAINT,
          "a forbidden resource refuses the graph");
    env_stop(&e);
}

static void t_parallel(void) {
    printf("[*] parallel graph: critical path\n");
    const uint32_t reps = 15;
    uint64_t times[2][32];
    uint32_t workers[2] = { 1, 4 };
    AgReport rep;
    for (uint32_t k = 0; k < 2; k++) {
        Env e;
        CHECK(env_start(&e, workers[k], 1, 0) == 0, "env");
        library(&e);
        AgGraph g;
        CHECK(compile(&e, GOAL_PARALLEL, 1, &g, &rep) == AG_OK_READY, "compile %d", rep.verdict);
        AgLowered L;
        CHECK(lower(&e, &L, &g, NULL) == 0, "lower");
        AgResult res;
        full_run(&e, &L, 1, "parallel warm", &res);
        CHECK(res.outcome == AG_RUN_SUCCESS &&
              res.value[node_of_kind(&g, AG_SKILL)] == 7 + 500 * 3 + 40 * 6 + 7 * 9 + 3 * 12,
              "combine = 7 + 3*500 + 6*40 + 9*7 + 12*3");
        for (uint32_t i = 0; i < reps; i++) {
            reference(&e, &g, 2 + i);
            int64_t ext = run(&e, &L, 2 + i, &res, &times[k][i]);
            compare(&e, &g, &res, &g_ref, "parallel");
            if (i == 0) ancestry(&e, &L, ext, "parallel");
        }
        env_stop(&e);
    }
    for (uint32_t k = 0; k < 2; k++)
        for (uint32_t i = 1; i < reps; i++)
            for (uint32_t j = i; j > 0 && times[k][j - 1] > times[k][j]; j--) {
                uint64_t t = times[k][j]; times[k][j] = times[k][j - 1]; times[k][j - 1] = t;
            }
    R.par_1w_ns = times[0][reps / 2];
    R.par_nw_ns = times[1][reps / 2];
    R.par_workers = workers[1];
    R.par_critical_us = rep.critical_path_us;
    R.par_total_us = rep.total_work_us;
    double s = (double)R.par_1w_ns / (double)R.par_nw_ns;
    printf("    median run: 1 worker %.2f ms, %u workers %.2f ms, speedup %.2fx "
           "(critical path %llu us of %llu us work)\n", R.par_1w_ns / 1e6, workers[1],
           R.par_nw_ns / 1e6, s, (unsigned long long)rep.critical_path_us,
           (unsigned long long)rep.total_work_us);
    CHECK(s >= 2.0, "independent nodes run concurrently: speedup %.2f < 2", s);
    CHECK(R.par_nw_ns < (uint64_t)rep.total_work_us * 1000ull,
          "parallel run beats the sequential sum of work");
}

static void t_determinism(void) {
    printf("[*] deterministic semantic result across worker counts\n");
    uint8_t first[8][32];
    uint32_t wk[4] = { 1, 2, 4, 8 };
    for (uint32_t k = 0; k < 4; k++) {
        Env e;
        CHECK(env_start(&e, wk[k], 1, 0) == 0, "env");
        library(&e);
        AgGraph g;
        AgReport rep;
        CHECK(compile(&e, GOAL_BRANCH, 1, &g, &rep) == AG_OK_READY, "compile");
        AgLowered L;
        CHECK(lower(&e, &L, &g, NULL) == 0, "lower");
        for (uint64_t r = 1; r <= 8; r++) {
            RxMutation m = { e.A, 0, r * 11 };  /* both arms across the runs */
            rx_world_publish_external(&e.w, e.ext_A, &m, 1);
            settle(&e);
            AgResult res;
            full_run(&e, &L, r, "determinism", &res);
            uint8_t d[32];
            semantic_digest(&e, &g, &res, d);
            R.det_runs++;
            if (k == 0) memcpy(first[r - 1], d, 32);
            else if (memcmp(first[r - 1], d, 32) != 0) R.det_mismatch++;
        }
        env_stop(&e);
    }
    CHECK(R.det_mismatch == 0, "semantic result differs across worker counts (%u)", R.det_mismatch);
}

static void t_optimize(void) {
    printf("[*] optimization passes preserve the result\n");
    AgResult res[2];
    uint64_t chain[2] = { 0, 0 };
    AgGraph gs[2];
    for (int opt = 0; opt < 2; opt++) {
        Env e;
        CHECK(env_start(&e, 4, 1, 0) == 0, "env");
        library(&e);
        AgReport rep;
        CHECK(compile(&e, GOAL_REDUNDANT, opt, &gs[opt], &rep) == AG_OK_READY, "compile %d", rep.verdict);
        AgLowered L;
        CHECK(lower(&e, &L, &gs[opt], NULL) == 0, "lower");
        uint64_t c0 = e.w.n_crumbs, ns = 0, best = UINT64_MAX;
        full_run(&e, &L, 1, opt ? "optimized" : "unoptimized", &res[opt]);
        uint64_t per_run = e.w.n_crumbs - c0;
        for (uint64_t r = 2; r <= 9; r++) {
            run(&e, &L, r, &res[opt], &ns);
            if (ns < best) best = ns;
        }
        chain[opt] = fld(&e, e.effect, 2);
        if (opt) {
            R.opt_before = rep.nodes_before;
            R.opt_after = rep.nodes_after;
            R.opt_fold = rep.passes.const_folded;
            R.opt_cse = rep.passes.cse;
            R.opt_reads = rep.passes.world_reads;
            R.opt_fused = rep.passes.fused;
            R.opt_dead = rep.passes.dead;
            R.opt_deps = rep.passes.deps_dropped;
            R.opt_ns_after = best;
            R.opt_crumbs_after = per_run;
        } else {
            R.opt_ns_before = best;
            R.opt_crumbs_before = per_run;
        }
        env_stop(&e);
    }
    R.opt_react_before = gs[0].n_nodes;
    R.opt_react_after = gs[1].n_nodes;
    printf("    nodes %u -> %u (folded %u, cse %u, world reads %u, fused %u, dead %u, "
           "order edges dropped %u); crumbs per run %llu -> %llu\n", R.opt_before, R.opt_after,
           R.opt_fold, R.opt_cse, R.opt_reads, R.opt_fused, R.opt_dead, R.opt_deps,
           (unsigned long long)R.opt_crumbs_before, (unsigned long long)R.opt_crumbs_after);
    CHECK(R.opt_before == 16 && R.opt_after == 8, "16 nodes optimize to 8 (%u)", R.opt_after);
    CHECK(R.opt_fold >= 1 && R.opt_cse >= 1 && R.opt_reads >= 1 && R.opt_fused >= 2 &&
          R.opt_dead >= 2 && R.opt_deps >= 2, "every pass engaged");
    CHECK(rx_graph_check_preserved(&gs[0], &gs[1]) == 0, "effects, evidence and conditions kept");
    /* Same semantic result: the success node, the verified value, the effect chain. */
    int p0 = node_of_kind(&gs[0], AG_EFFECT_PERFORM), p1 = node_of_kind(&gs[1], AG_EFFECT_PERFORM);
    int v0 = node_of_kind(&gs[0], AG_VERIFY), v1 = node_of_kind(&gs[1], AG_VERIFY);
    CHECK(res[0].outcome == AG_RUN_SUCCESS && res[1].outcome == AG_RUN_SUCCESS, "both succeed");
    CHECK(res[0].value[p0] == res[1].value[p1] && res[0].status[v0] == res[1].status[v1],
          "same receipts and verdicts");
    CHECK(chain[0] == chain[1] && chain[0] != 0, "same effect chain before and after optimization");
    /* (40 + 42) * 2 + 1 + (40 + 42) - 3 = 244 */
    CHECK(res[1].value[node_of_kind(&gs[1], AG_EFFECT_PROPOSE)] == 244, "optimized value 244");

    /* Graph identity does not depend on builder order. */
    AgGraph x, y;
    rx_graph_init(&x, SUBJ_PLAN);
    rx_graph_init(&y, SUBJ_PLAN);
    int a1 = N(&x, AG_CONST, AG_T_U64), b1 = N(&x, AG_CONST, AG_T_U64);
    x.nodes[a1].imm = 1; x.nodes[b1].imm = 2;
    int s1 = node_pure(&x, OP_ADD, a1, b1, 0);
    rx_graph_success(&x, (uint32_t)s1, AG_OK);
    int b2 = N(&y, AG_CONST, AG_T_U64), a2 = N(&y, AG_CONST, AG_T_U64);
    y.nodes[a2].imm = 1; y.nodes[b2].imm = 2;
    int s2 = node_pure(&y, OP_ADD, a2, b2, 0);
    rx_graph_success(&y, (uint32_t)s2, AG_OK);
    CHECK(rx_graph_validate(&x, NULL) == 0 && rx_graph_validate(&y, NULL) == 0 &&
          memcmp(x.digest, y.digest, 32) == 0, "same graph, other builder order, same identity");
    y.nodes[a2].imm = 3;
    rx_graph_identify(&y);
    CHECK(memcmp(x.digest, y.digest, 32) != 0, "a semantic change changes the identity");
}

static void t_structure(void) {
    printf("[*] structural refusals\n");
    AgGraph g;
    /* cycle */
    rx_graph_init(&g, SUBJ_PLAN);
    int a = N(&g, AG_PURE, AG_T_U64), b = N(&g, AG_PURE, AG_T_U64);
    rx_graph_data(&g, (uint32_t)a, (uint32_t)b, 0, AG_EDGE_DATA);
    rx_graph_data(&g, (uint32_t)b, (uint32_t)a, 0, AG_EDGE_DATA);
    CHECK(rx_graph_validate(&g, NULL) == AG_E_CYCLE, "cycle refused");
    /* type: perform fed a raw number instead of a proposal */
    rx_graph_init(&g, SUBJ_PLAN);
    a = N(&g, AG_CONST, AG_T_U64);
    int v = node_verify(&g, a, 0, 9);
    node_perform(&g, a, v);
    CHECK(rx_graph_validate(&g, NULL) == AG_E_TYPE, "perform without a proposal refused");
    /* effects with no order between them */
    rx_graph_init(&g, SUBJ_PLAN);
    a = N(&g, AG_CONST, AG_T_U64);
    v = node_verify(&g, a, 0, 9);
    int p = node_propose(&g, a);
    node_perform(&g, p, v);
    node_perform(&g, p, v);
    CHECK(rx_graph_validate(&g, NULL) == AG_E_EFFECT_ORDER, "unordered effects refused");
    /* a join whose arms can both be OK */
    rx_graph_init(&g, SUBJ_PLAN);
    a = N(&g, AG_CONST, AG_T_U64);
    b = N(&g, AG_CONST, AG_T_U64);
    int j = N(&g, AG_JOIN, AG_T_U64);
    rx_graph_data(&g, (uint32_t)a, (uint32_t)j, 0, AG_EDGE_DATA);
    rx_graph_data(&g, (uint32_t)b, (uint32_t)j, 1, AG_EDGE_DATA);
    CHECK(rx_graph_validate(&g, NULL) == AG_E_JOIN, "non-exclusive join refused");
}

static void t_authority(void) {
    printf("[*] authority-blocked node: AEGIS slow path, human approval, no mint in compile\n");
    Env e;
    CHECK(env_start(&e, 4, 0, 1) == 0, "env");
    library(&e);
    AgGraph g;
    AgReport rep;
    uint64_t mints0 = e.a.mints;
    CHECK(compile(&e, GOAL_LINEAR, 1, &g, &rep) == AG_OK_READY, "compile %d", rep.verdict);
    R.aegis_mints_during_compile = (uint32_t)(e.a.mints - mints0);
    CHECK(rep.n_missing == 1 && rep.missing[0].resource == RES_EFFECT &&
          rep.missing[0].rights == (RX_RIGHT_READ | RX_RIGHT_WRITE | RX_RIGHT_EFFECT),
          "compile reports the missing effect authority");
    CHECK(e.a.mints == mints0, "compile minted nothing");

    AgAegisBinding ab;
    memset(&ab, 0, sizeof ab);
    ab.client = &e.a.o[0];
    ab.request_res = rx_aegis_res(0, RX_AEGIS_RES_REQUEST);
    ab.request_cap = mint(&e, SUBJ_PLAN, ab.request_res, RX_RIGHT_READ | RX_RIGHT_WRITE);
    for (uint32_t j = 0; j < RX_AEGIS_SLOTS; j++) {
        ab.slot_res[j] = rx_aegis_res(0, RX_AEGIS_RES_SLOT0 + j);
        ab.slot_cap[j] = mint(&e, SUBJ_PLAN, ab.slot_res[j], RX_RIGHT_READ);
    }
    AgLowered L;
    CHECK(lower(&e, &L, &g, &ab) == 0 && L.n_ask == 1, "lower with the AEGIS binding");
    int pf = node_of_kind(&g, AG_EFFECT_PERFORM);
    AgResult res;
    run(&e, &L, 1, &res, NULL);
    uint32_t blocked = count_crumbs(&e, L.reaction[pf], RX_CRUMB_BLOCKED_AUTHORITY);
    R.authority_blocked_crumbs += blocked;
    CHECK(res.outcome == AG_RUN_INCOMPLETE && res.status[pf] == AG_PENDING && blocked >= 1,
          "perform is blocked for authority (%u crumbs)", blocked);
    CHECK(fld(&e, e.effect, 0) == 0, "no effect without authority");
    CHECK(fld(&e, e.a.o[0].decision, 1) == RX_AEGIS_ESCALATE, "AEGIS escalated to a human");
    CHECK(count_crumbs(&e, L.reaction[pf], RX_CRUMB_COMMIT) == 0, "perform never committed");

    /* The human approves. Only the root mints; the slot wakes the perform. */
    uint64_t seq = fld(&e, e.a.o[0].request, 0);
    RxMutation m[2] = { { e.a.o[0].approval, 0, seq }, { e.a.o[0].approval, 1, RX_AEGIS_APPROVE } };
    CHECK(rx_world_publish_external(&e.w, e.ext_approval, m, 2) > 0, "approval");
    CHECK(settle(&e) == RX_OK, "settle");
    R.aegis_mints_after_approval = (uint32_t)(e.a.mints - mints0);
    rx_graph_collect(&L, 1, &res);
    CHECK(e.a.mints == mints0 + 1, "the root minted once, after approval");
    CHECK(res.outcome == AG_RUN_SUCCESS && fld(&e, e.effect, 0) == 1 && fld(&e, e.effect, 1) == 135,
          "the waiting perform ran once its slot held authority");
    /* Next run: the fast path. No AEGIS decision, no new mint. */
    uint64_t decides = count_crumbs(&e, e.a.r_decide[0], RX_CRUMB_COMMIT);
    run(&e, &L, 2, &res, NULL);
    CHECK(res.outcome == AG_RUN_SUCCESS && e.a.mints == mints0 + 1 &&
          count_crumbs(&e, e.a.r_decide[0], RX_CRUMB_COMMIT) == decides,
          "existing authority: resident fast path, no AEGIS round trip");
    env_stop(&e);
}

static void t_adversarial_authority(void) {
    printf("[*] adversarial authority: stolen, revoked, forged, tampered\n");
    Env e;
    CHECK(env_start(&e, 4, 0, 0) == 0, "env");
    library(&e);
    AgGraph g;
    AgReport rep;

    /* A capability minted for someone else, placed in the principal's table. */
    RxCapRef stolen = mint(&e, SUBJ_OTHER, RES_EFFECT, RX_RIGHT_READ | RX_RIGHT_WRITE | RX_RIGHT_EFFECT);
    hold(&e, stolen, RES_EFFECT, RX_RIGHT_READ | RX_RIGHT_WRITE | RX_RIGHT_EFFECT);
    CHECK(compile(&e, GOAL_LINEAR, 1, &g, &rep) == AG_OK_READY && rep.n_missing == 1,
          "a stolen capability does not count as held");
    AgLowered L;
    CHECK(lower(&e, &L, &g, NULL) == 0, "lower");
    int pf = node_of_kind(&g, AG_EFFECT_PERFORM);
    AgResult res;
    run(&e, &L, 1, &res, NULL);
    uint32_t blocked = count_crumbs(&e, L.reaction[pf], RX_CRUMB_BLOCKED_AUTHORITY);
    R.authority_blocked_crumbs += blocked;
    CHECK(blocked >= 1 && fld(&e, e.effect, 0) == 0, "stolen capability: blocked, no effect");
    env_stop(&e);

    /* Revoked between compile and run. */
    CHECK(env_start(&e, 4, 1, 0) == 0, "env");
    library(&e);
    CHECK(compile(&e, GOAL_LINEAR, 1, &g, &rep) == AG_OK_READY && rep.n_missing == 0, "compile");
    CHECK(lower(&e, &L, &g, NULL) == 0, "lower");
    AienosCapRef office;
    aienos_cap_office(e.admin, &office);
    CHECK(aienos_cap_revoke(e.admin, office, (AienosCapRef){ e.effect_cap.cap_id, e.effect_cap.generation }) == 0,
          "revoke");
    pf = node_of_kind(&g, AG_EFFECT_PERFORM);
    run(&e, &L, 1, &res, NULL);
    blocked = count_crumbs(&e, L.reaction[pf], RX_CRUMB_BLOCKED_AUTHORITY);
    R.authority_blocked_crumbs += blocked;
    CHECK(blocked >= 1 && fld(&e, e.effect, 0) == 0 && res.outcome == AG_RUN_INCOMPLETE,
          "revoked after compile: blocked at run time, no effect");

    /* A plan that publishes where the principal holds nothing. */
    AgGraph f;
    rx_graph_init(&f, SUBJ_PLAN);
    int c = N(&f, AG_CONST, AG_T_U64);
    f.nodes[c].imm = 666;
    int pub = node_publish(&f, c, 0, 0);
    f.nodes[pub].obj = e.secret;
    rx_graph_success(&f, (uint32_t)pub, AG_OK);
    CHECK(rx_graph_validate(&f, &e.w) == 0 && f.n_auth == 1, "forged publish declares its need");
    AgLowered L2;
    CHECK(lower(&e, &L2, &f, NULL) == 0, "lower");
    run(&e, &L2, 1, &res, NULL);
    blocked = count_crumbs(&e, L2.reaction[pub], RX_CRUMB_BLOCKED_AUTHORITY);
    R.authority_blocked_crumbs += blocked;
    CHECK(blocked >= 1 && fld(&e, e.secret, 0) == 0, "publish without authority: blocked, untouched");

    /* The outside cannot write a graph cell, and a graph cell cannot be
     * written with the run token's capability. */
    RxMutation m = { L.cell[0], 2, 12345 };
    CHECK(rx_world_publish_external(&e.w, e.ext_run, &m, 1) < 0, "outside write to a cell refused");
    env_stop(&e);
}

static void t_resource(void) {
    printf("[*] resource-blocked node\n");
    Env e;
    CHECK(env_start(&e, 4, 1, 0) == 0, "env");
    library(&e);
    RxResourceBudget b = e.w.budget;
    b.memory_bytes = 1ull << 30;
    rx_world_set_resources(&e.w, &b);
    AgGoal goal = goal_for(&e, GOAL_RESOURCE);
    AgConstraints c = open_constraints();
    c.budget = b;
    AgGraph g;
    AgReport rep;
    CHECK(rx_graph_compile(&goal, &e.w, &e.caps, &c, &e.lib, 1, &g, &rep) == AG_OK_READY,
          "compile %d", rep.verdict);
    int ph = node_of_kind(&g, AG_PHYSICAL);
    CHECK(rep.n_resource_blocked == 1 && rep.resource_blocked[0] == ph,
          "compile reports the node the body cannot hold");
    AgLowered L;
    CHECK(lower(&e, &L, &g, NULL) == 0, "lower");
    AgResult res;
    reference(&e, &g, 1);
    run(&e, &L, 1, &res, NULL);
    int pub = node_of_kind(&g, AG_WORLD_PUBLISH);
    int ind = -1;
    for (uint32_t n = 0; n < g.n_nodes; n++)
        if (g.nodes[n].kind == AG_VERIFY && res.status[n] == AG_OK) ind = (int)n;
    CHECK(res.status[ph] == AG_PENDING && res.status[pub] == AG_PENDING && ind >= 0,
          "blocked node and its dependants wait; the independent branch completes");
    CHECK(fld(&e, e.out, 3) == 0 && res.outcome == AG_RUN_INCOMPLETE, "nothing published yet");
    b.memory_bytes = 1ull << 41;
    rx_world_set_resources(&e.w, &b);
    CHECK(settle(&e) == RX_OK, "settle");
    rx_graph_collect(&L, 1, &res);
    compare(&e, &g, &res, &g_ref, "resource resumed");
    CHECK(res.outcome == AG_RUN_SUCCESS && fld(&e, e.out, 3) == res.value[pub],
          "admitted when the budget allows; the run completes");
    env_stop(&e);
}

static void t_stale(void) {
    printf("[*] stale-generation node\n");
    Env e;
    CHECK(env_start(&e, 4, 1, 0) == 0, "env");
    library(&e);
    AgGraph g;
    AgReport rep;
    /* At compile time: a retired object refuses the graph. */
    CHECK(rx_world_retire(&e.w, e.C) == RX_OK, "retire C");
    CHECK(compile(&e, GOAL_PARALLEL, 1, &g, &rep) == AG_E_STALE && rep.n_stale == 1,
          "compile refuses a stale World reference");
    /* After lowering: B is retired and its slot reused by a new object. */
    CHECK(compile(&e, GOAL_RESOURCE, 1, &g, &rep) == AG_OK_READY, "compile");
    AgLowered L;
    CHECK(lower(&e, &L, &g, NULL) == 0, "lower");
    CHECK(rx_world_retire(&e.w, e.B) == RX_OK, "retire B");
    uint64_t imposter[RX_MAX_FIELDS] = { 999 };
    RxObjRef nb;
    CHECK(rx_world_create(&e.w, 0xA02, RX_PERSIST_RESIDENT, RES_B, imposter, &nb) == RX_OK, "new B");
    AgResult res;
    reference(&e, &g, 1);
    int64_t ext = run(&e, &L, 1, &res, NULL);
    (void)ext;
    int rb = -1;
    for (uint32_t n = 0; n < g.n_nodes; n++)
        if (g.nodes[n].kind == AG_WORLD_READ && g.nodes[n].obj.id == e.B.id) rb = (int)n;
    uint32_t inval = 0;
    for (uint64_t id = 1; id <= e.w.n_crumbs; id++) {
        const RxCrumb *k = rx_world_crumb(&e.w, id);
        if (k->reaction == L.reaction[rb] && k->kind == RX_CRUMB_INVALIDATED && k->reason == RX_ERR_STALE_GEN)
            inval++;
    }
    CHECK(rb >= 0 && inval >= 1 && res.status[rb] == AG_PENDING,
          "the old-generation read is invalidated, not re-bound (%u)", inval);
    CHECK(res.outcome == AG_RUN_INCOMPLETE && fld(&e, e.out, 3) == 0,
          "nothing downstream of a stale read happens");
    compare(&e, &g, &res, &g_ref, "stale");
    env_stop(&e);
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

static void write_receipt(void) {
    char path[512];
    if (omega_evidence_path("ACTION_GRAPH/rx_action_graph_receipt.json", path, sizeof path) != 0) return;
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    char commit[41];
    memset(commit, 0, sizeof commit);
    if (!omega_evidence_run_commit(commit)) memcpy(commit, "unknown", 8);
    const char *candidate = getenv("OMEGA_CANDIDATE_COMMIT");
    int bound = candidate && candidate[0] && strcmp(candidate, commit) == 0 &&
                !omega_evidence_tree_dirty();
    const char *aienos = getenv("AIENOS_COMMIT");
    char digest[65];
    binary_digest(digest);
    struct utsname u;
    memset(&u, 0, sizeof u);
    uname(&u);
    int pass = g_fail == 0 && R.bypass == 0 && R.evidence_missing == 0 && R.ancestry_mismatch == 0 &&
               R.det_mismatch == 0 && R.aegis_mints_during_compile == 0;
    fprintf(fp,
            "{\n"
            "  \"schema\": \"OMEGA_ACTION_GRAPH_IR_V1\",\n"
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
            "  \"scope\": \"host processor; native AIENOS authority library; resident reaction world\",\n"
            "  \"equivalence\": {\"runs_compared_with_sequential_reference\": %u, \"node_results_compared\": %u},\n"
            "  \"evidence\": {\"required_checked\": %u, \"missing\": %u},\n"
            "  \"causal_ancestry\": {\"nodes_checked\": %u, \"mismatches\": %u},\n"
            "  \"determinism\": {\"runs\": %u, \"worker_counts\": [1, 2, 4, 8], \"mismatches\": %u},\n"
            "  \"parallel\": {\"median_run_ns_1_worker\": %llu, \"median_run_ns_%llu_workers\": %llu,\n"
            "               \"speedup\": %.3f, \"critical_path_us\": %llu, \"total_work_us\": %llu},\n"
            "  \"optimization\": {\"nodes_before\": %u, \"nodes_after\": %u, \"constant_folded\": %u,\n"
            "                   \"cse\": %u, \"world_reads_removed\": %u, \"fused\": %u, \"dead\": %u,\n"
            "                   \"order_edges_dropped\": %u, \"crumbs_per_run_before\": %llu,\n"
            "                   \"crumbs_per_run_after\": %llu, \"best_run_ns_before\": %llu,\n"
            "                   \"best_run_ns_after\": %llu},\n"
            "  \"authority\": {\"bypasses\": %u, \"blocked_authority_crumbs\": %u,\n"
            "                \"mints_during_compile\": %u, \"mints_after_human_approval\": %u,\n"
            "                \"compile_has_admin_handle\": false},\n"
            "  \"gates\": {\n"
            "    \"OMEGA_ACTION_GRAPH_IR_PASS\": \"%s\",\n"
            "    \"not_claimed\": [\"graphics-processor nodes\", \"AIENOS kernel (the authority runs as a host library)\", "
            "\"a learned planner (procedures are fixed templates)\", \"more than one AEGIS request per graph\", "
            "\"retry that re-enters the world between attempts\", \"graphs above 64 nodes\"]\n"
            "  }\n"
            "}\n",
            omega_evidence_run_id(), candidate ? "\"" : "", candidate ? candidate : "null",
            candidate ? "\"" : "", bound ? "true" : "false", commit,
            omega_evidence_tree_dirty() ? "true" : "false", aienos ? "\"" : "",
            aienos ? aienos : "null", aienos ? "\"" : "", g_checks, g_fail, digest, u.sysname,
            u.release, u.machine, R.cpus, R.runs_compared, R.nodes_compared, R.evidence_checked,
            R.evidence_missing, R.ancestry_nodes, R.ancestry_mismatch, R.det_runs, R.det_mismatch,
            (unsigned long long)R.par_1w_ns, (unsigned long long)R.par_workers,
            (unsigned long long)R.par_nw_ns, (double)R.par_1w_ns / (double)(R.par_nw_ns ? R.par_nw_ns : 1),
            (unsigned long long)R.par_critical_us, (unsigned long long)R.par_total_us, R.opt_before,
            R.opt_after, R.opt_fold, R.opt_cse, R.opt_reads, R.opt_fused, R.opt_dead, R.opt_deps,
            (unsigned long long)R.opt_crumbs_before, (unsigned long long)R.opt_crumbs_after,
            (unsigned long long)R.opt_ns_before, (unsigned long long)R.opt_ns_after, R.bypass,
            R.authority_blocked_crumbs, R.aegis_mints_during_compile, R.aegis_mints_after_approval,
            pass ? "PASS" : "FAIL");
    fclose(fp);
    printf("receipt: %s\n", path);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    place();
    skills_init();
    build_templates(3000);
    t_structure();
    t_linear();
    t_diamond();
    t_branch();
    t_failure();
    t_effects();
    t_parallel();
    t_determinism();
    t_optimize();
    t_authority();
    t_adversarial_authority();
    t_resource();
    t_stale();
    printf("    compared %u runs (%u node results) with the sequential reference; "
           "ancestry %u nodes, %u mismatches; evidence %u required, %u missing; bypasses %u\n",
           R.runs_compared, R.nodes_compared, R.ancestry_nodes, R.ancestry_mismatch,
           R.evidence_checked, R.evidence_missing, R.bypass);
    printf("checks %d failures %d\n", g_checks, g_fail);
    write_receipt();
    return g_fail ? 1 : 0;
}
