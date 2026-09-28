/*
 * OMEGA_WORKFLOW_FUSION -- repeated verified workflow fragments become
 * MetaSkills, and only a verified, measured, canaried, promoted and published
 * MetaSkill ever replaces what the steps did.
 *
 * The test plays the outside: it holds the authority admin (to give the
 * principal what it is born with, to revoke, and to mint the promotion right),
 * publishes World inputs and run tokens, and drives the runs. Omega's side
 * (rx_graph, rx_fusion) gets the world, the principal's capability table and
 * the generation store; it never holds an admin handle.
 *
 * Procedures (three share the workflow W = read A -> fetch(A, base) ->
 * (*3 + 1) -> score, where `base` is a recall that also feeds a check):
 *
 *   EFFECT    W -> verify -> propose -> perform           (effect boundary)
 *   PUBLISH   W -> verify, publish                        (World boundary)
 *   COMPARE   W + read B -> add -> verify, publish
 *   UNSTABLE  read C -> drift -> +9 -> ...                (drift: hidden input)
 *   FLAKY     read B -> flaky -> *2 -> ...                (fails half the time)
 *   BUDGET    four outside sources + two objects          (too big for one reaction)
 *   PARALLEL, PARALLEL2  two independent heavy steps joined (fusion would serialize)
 */
#include "runtime/aienos_cap.h"
#include "runtime/rx_fusion.h"
#include "runtime/rx_generation.h"
#include "runtime/rx_graph.h"
#include "runtime/rx_world.h"
#include "omega_evidence.h"
#include "omega_types.h"
#include "sha256.h"

#include <dirent.h>
#include <math.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

enum { SUBJ_EXTERNAL = 100, ISSUER = 3, SUBJ_PLAN = 61, SUBJ_PROPOSER = 71, SUBJ_PROMOTER = 72 };

#define RES_CELL    0xB000001ull
#define RES_RUN     0xB000002ull
#define RES_A       0xB000010ull
#define RES_B       0xB000011ull
#define RES_C       0xB000012ull
#define RES_MEM     0xB000013ull
#define RES_OUT     0xB000014ull
#define RES_D       0xB000015ull
#define RES_EFFECT  0xB000020ull

enum { GOAL_EFFECT = 1, GOAL_PUBLISH, GOAL_COMPARE, GOAL_UNSTABLE, GOAL_FLAKY, GOAL_BUDGET,
       GOAL_PARALLEL, GOAL_PARALLEL2, N_GOALS = GOAL_PARALLEL2 };

enum { SK_FETCH = 1, SK_SCORE, SK_DRIFT, SK_FLAKY, SK_COMBINE4, SK_HEAVY };

enum { MS_COMPILED = 101, MS_HYBRID, MS_LEARNED, MS_HARDWARE, MS_POISONED, MS_PARALLEL };

#define FETCH_US  100u
#define SCORE_US  100u
#define HEAVY_US  1500u
#define OBSERVE_RUNS 32u

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

static uint64_t cpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- receipt figures ---- */
static struct {
    char cpus[64];
    /* observation */
    uint32_t obs_runs, obs_refused, obs_patterns, obs_fragments, obs_over_budget, obs_dropped;
    uint32_t w_steps, w_occ, w_graphs, w_ok, w_failed, w_stable, w_samples;
    uint32_t n_candidates, judged_unstable, judged_failure_rate, judged_few_graphs;
    char w_signature[65];
    /* verification */
    struct { uint32_t state, reason, vectors, observed, generated, combos, hits; } ver[4];
    uint32_t tamper_cases, tamper_caught;
    char tamper_names[512];
    uint32_t coherent_rewrite_sites;
    /* never silently */
    uint32_t unpublished_refusals, unpublished_digest_equal;
    /* measurement */
    AgFusionMeasure m_compiled, m_hybrid, m_parallel;
    uint32_t parallel_state, hybrid_state;
    /* canary */
    uint32_t canary_runs, canary_divergences, poisoned_state, poisoned_verified, poisoned_promote_refused;
    uint32_t poisoned_canary_runs;
    /* promotion */
    uint32_t promote_refused_no_right, promote_refused_same_subject, publish_refused_changed;
    uint64_t generation;
    /* use */
    uint32_t fused_graphs, fused_nodes_before, fused_nodes_after;
    uint32_t eq_runs, eq_nodes, eq_mismatch, cross_runs, cross_mismatch, steps_checked, steps_mismatch;
    uint32_t evidence_checked, evidence_missing, ancestry_nodes, ancestry_mismatch;
    uint32_t det_runs, det_mismatch, bypass;
    uint32_t n3_blocked_original, n3_blocked_fused, n3_meta_auth_exact;
    uint32_t applied_after_publish, refused_after_publish;
} R;

/* ---- skills ---- */

static uint64_t g_drift;

static uint64_t sk_fetch(const uint64_t *in, uint32_t n, uint32_t attempt, int *failed) {
    (void)attempt;
    *failed = 0;
    return (n > 0 ? in[0] : 0) + (n > 1 ? in[1] : 0) + 100;
}

static uint64_t sk_score(const uint64_t *in, uint32_t n, uint32_t attempt, int *failed) {
    (void)attempt;
    uint64_t v = n ? in[0] : 0;
    *failed = (v % 16u) == 0;
    return *failed ? 0 : v * 7 + 3;
}

static uint64_t sk_drift(const uint64_t *in, uint32_t n, uint32_t attempt, int *failed) {
    (void)attempt;
    *failed = 0;
    return (n ? in[0] : 0) + g_drift;       /* depends on something the graph does not name */
}

static uint64_t sk_flaky(const uint64_t *in, uint32_t n, uint32_t attempt, int *failed) {
    (void)attempt;
    uint64_t v = n ? in[0] : 0;
    *failed = (int)(v & 1u);
    return *failed ? 0 : v + 1;
}

static uint64_t sk_combine4(const uint64_t *in, uint32_t n, uint32_t attempt, int *failed) {
    (void)attempt;
    *failed = 0;
    uint64_t s = 5;
    for (uint32_t i = 0; i < n; i++) s += in[i] * (i + 2);
    return s;
}

static uint64_t sk_heavy(const uint64_t *in, uint32_t n, uint32_t attempt, int *failed) {
    (void)attempt;
    *failed = 0;
    return (n ? in[0] : 0) * 5 + 1;
}

static AgSkillTable g_skills;

static void skills_init(void) {
    memset(&g_skills, 0, sizeof g_skills);
    g_skills.skill[g_skills.n++] = (AgSkill){ SK_FETCH, sk_fetch, { 0x11 } };
    g_skills.skill[g_skills.n++] = (AgSkill){ SK_SCORE, sk_score, { 0x12 } };
    g_skills.skill[g_skills.n++] = (AgSkill){ SK_DRIFT, sk_drift, { 0x13 } };
    g_skills.skill[g_skills.n++] = (AgSkill){ SK_FLAKY, sk_flaky, { 0x14 } };
    g_skills.skill[g_skills.n++] = (AgSkill){ SK_COMBINE4, sk_combine4, { 0x15 } };
    g_skills.skill[g_skills.n++] = (AgSkill){ SK_HEAVY, sk_heavy, { 0x16 } };
}

/* ---- environment ---- */

typedef struct {
    AienosCapAdmin *admin;
    AienosCapView *view;
    RxWorld w;
    RxObjRef A, B, C, D, mem, out, effect;
    RxCapRef ext_run, ext_A, ext_B;
    RxCapRef cell_cap, run_cap, read_A;
    AgCapTable caps;
    AgLibrary lib;
    uint32_t n_lowered;
    const AgLowered *lowered[16];
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

static AgGraph T[N_GOALS + 1];

static void library(Env *e) {
    memset(&e->lib, 0, sizeof e->lib);
    for (uint32_t k = 1; k <= N_GOALS && e->lib.n < AG_MAX_PROCS; k++)
        e->lib.proc[e->lib.n++] = (AgProcedure){ k, &T[k] };
}

static int env_start(Env *e, uint32_t workers) {
    memset(e, 0, sizeof *e);
    if (aienos_cap_start(&e->admin, &e->view) != 0) return -1;
    if (rx_world_init_native(&e->w, e->view, workers, 1u << 18) != RX_OK) return -1;
    e->w.external_subject = SUBJ_EXTERNAL;
    const uint32_t R_ = RX_RIGHT_READ, W_ = RX_RIGHT_WRITE, RW = R_ | W_;
    uint64_t z[RX_MAX_FIELDS] = { 0 };
    uint64_t a[RX_MAX_FIELDS] = { 40 }, b[RX_MAX_FIELDS] = { 7 }, c[RX_MAX_FIELDS] = { 3 };
    uint64_t d[RX_MAX_FIELDS] = { 9 };
    uint64_t m[RX_MAX_FIELDS] = { 11, 500, 12, 600, 13, 700, 0, 0 };
    if (rx_world_create(&e->w, 0xB01, RX_PERSIST_RESIDENT, RES_A, a, &e->A) != RX_OK ||
        rx_world_create(&e->w, 0xB02, RX_PERSIST_RESIDENT, RES_B, b, &e->B) != RX_OK ||
        rx_world_create(&e->w, 0xB03, RX_PERSIST_RESIDENT, RES_C, c, &e->C) != RX_OK ||
        rx_world_create(&e->w, 0xB04, RX_PERSIST_RESIDENT, RES_D, d, &e->D) != RX_OK ||
        rx_world_create(&e->w, AG_OT_MEMORY, RX_PERSIST_RESIDENT, RES_MEM, m, &e->mem) != RX_OK ||
        rx_world_create(&e->w, 0xB05, RX_PERSIST_RESIDENT, RES_OUT, z, &e->out) != RX_OK ||
        rx_world_create(&e->w, AG_OT_EFFECT, RX_PERSIST_RESIDENT, RES_EFFECT, z, &e->effect) != RX_OK)
        return -1;
    e->ext_run = mint(e, SUBJ_EXTERNAL, RES_RUN, W_);
    e->ext_A = mint(e, SUBJ_EXTERNAL, RES_A, W_);
    e->ext_B = mint(e, SUBJ_EXTERNAL, RES_B, W_);
    e->caps.subject = SUBJ_PLAN;
    e->cell_cap = mint(e, SUBJ_PLAN, RES_CELL, RW);
    e->run_cap = mint(e, SUBJ_PLAN, RES_RUN, R_);
    e->read_A = mint(e, SUBJ_PLAN, RES_A, R_);
    hold(e, e->read_A, RES_A, R_);
    hold(e, mint(e, SUBJ_PLAN, RES_B, R_), RES_B, R_);
    hold(e, mint(e, SUBJ_PLAN, RES_C, R_), RES_C, R_);
    hold(e, mint(e, SUBJ_PLAN, RES_D, R_), RES_D, R_);
    hold(e, mint(e, SUBJ_PLAN, RES_MEM, R_), RES_MEM, R_);
    hold(e, mint(e, SUBJ_PLAN, RES_OUT, RW), RES_OUT, RW);
    hold(e, mint(e, SUBJ_PLAN, RES_EFFECT, RW | RX_RIGHT_EFFECT), RES_EFFECT, RW | RX_RIGHT_EFFECT);
    library(e);
    return 0;
}

static uint32_t bypasses(Env *e);

static void env_stop(Env *e) {
    rx_world_wait_quiescent(&e->w, 30000);
    R.bypass += bypasses(e);
    rx_world_destroy(&e->w);
    aienos_cap_stop(e->admin, e->view);
}

static int lower(Env *e, AgLowered *L, const AgGraph *g, const AgSkillTable *sk) {
    if (e->n_lowered < 16) e->lowered[e->n_lowered++] = L;
    return rx_graph_lower(L, &e->w, g, sk, &e->caps, RES_CELL, e->cell_cap, RES_RUN, e->run_cap, NULL);
}

/* Commits to boundary targets must come from lowered boundary reactions. */
static uint32_t bypasses(Env *e) {
    uint32_t bad = 0;
    for (uint64_t id = 1; id <= e->w.n_crumbs; id++) {
        const RxCrumb *k = rx_world_crumb(&e->w, id);
        if (k->kind != RX_CRUMB_COMMIT) continue;
        for (uint32_t o = 0; o < k->n_outputs; o++) {
            uint32_t oid = k->outputs[o].obj.id;
            if (oid != e->effect.id && oid != e->out.id) continue;
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

static uint32_t commits_between(Env *e, uint64_t from, uint64_t to) {
    uint32_t n = 0;
    for (uint64_t id = from + 1; id <= to; id++) n += rx_world_crumb(&e->w, id)->kind == RX_CRUMB_COMMIT;
    return n;
}

/* ---- procedures ----
 * Parameters: 1 A, 2 B, 3 C, 4 memory, 5 out, 6 effect, 7 D. */

static int N(AgGraph *g, AgKind k, AgType t) { return rx_graph_node(g, k, t); }

static int node_read(AgGraph *g, uint32_t param, uint32_t field, uint32_t cost) {
    int n = N(g, AG_WORLD_READ, AG_T_U64);
    g->nodes[n].param = param;
    g->nodes[n].field = field;
    g->nodes[n].cost_us = cost;
    return n;
}

static int node_recall(AgGraph *g, uint64_t key) {
    int n = N(g, AG_RECALL, AG_T_U64);
    g->nodes[n].param = 4;
    g->nodes[n].imm = key;
    return n;
}

static int node_pure(AgGraph *g, uint32_t op, int a, int b, uint64_t imm) {
    int n = N(g, AG_PURE, AG_T_U64);
    g->nodes[n].op = op;
    g->nodes[n].imm = imm;
    if (a >= 0) rx_graph_data(g, (uint32_t)a, (uint32_t)n, 0, AG_EDGE_DATA);
    if (b >= 0) rx_graph_data(g, (uint32_t)b, (uint32_t)n, 1, AG_EDGE_DATA);
    return n;
}

static int node_skill(AgGraph *g, uint32_t id, int a, int b, uint32_t cost) {
    int n = N(g, AG_SKILL, AG_T_U64);
    g->nodes[n].op = id;
    g->nodes[n].cost_us = cost;
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

static int node_publish(AgGraph *g, int in, uint32_t field) {
    int n = N(g, AG_WORLD_PUBLISH, AG_T_RECEIPT);
    g->nodes[n].param = 5;
    g->nodes[n].field = field;
    rx_graph_data(g, (uint32_t)in, (uint32_t)n, 0, AG_EDGE_DATA);
    return n;
}

/* The shared workflow. Returns its result; `base` is also checked outside. */
static int workflow(AgGraph *g) {
    int base = node_recall(g, 11);
    int vb = node_verify(g, base, 1, 1000000);
    rx_graph_success(g, (uint32_t)vb, AG_OK);
    int a = node_read(g, 1, 0, 0);
    int f = node_skill(g, SK_FETCH, a, base, FETCH_US);
    int p = node_pure(g, OP_MUL, f, -1, 3);
    int q = node_pure(g, OP_ADD, p, -1, 1);
    return node_skill(g, SK_SCORE, q, -1, SCORE_US);
}

/* verify(x) then publish(x) to out.field; failure when the check fails. */
static void check_publish(AgGraph *g, int x, uint32_t field) {
    int v = node_verify(g, x, 0, 1ull << 40);
    int pub = node_publish(g, x, field);
    rx_graph_order(g, (uint32_t)v, (uint32_t)pub);
    rx_graph_success(g, (uint32_t)pub, AG_OK);
    rx_graph_failure(g, (uint32_t)v, AG_FAILED);
}

static void build_templates(void) {
    AgGraph *g;
    int x;

    g = &T[GOAL_EFFECT];
    rx_graph_init(g, 0);
    x = workflow(g);
    int v = node_verify(g, x, 0, 1ull << 40);
    int pr = N(g, AG_EFFECT_PROPOSE, AG_T_PROPOSAL);
    rx_graph_data(g, (uint32_t)x, (uint32_t)pr, 0, AG_EDGE_DATA);
    int pf = N(g, AG_EFFECT_PERFORM, AG_T_RECEIPT);
    g->nodes[pf].param = 6;
    rx_graph_data(g, (uint32_t)pr, (uint32_t)pf, 0, AG_EDGE_DATA);
    rx_graph_data(g, (uint32_t)v, (uint32_t)pf, 1, AG_EDGE_DATA);
    rx_graph_success(g, (uint32_t)pf, AG_OK);
    rx_graph_failure(g, (uint32_t)v, AG_FAILED);

    g = &T[GOAL_PUBLISH];
    rx_graph_init(g, 0);
    check_publish(g, workflow(g), 0);

    g = &T[GOAL_COMPARE];
    rx_graph_init(g, 0);
    x = workflow(g);
    int b = node_read(g, 2, 0, 0);
    check_publish(g, node_pure(g, OP_ADD, x, b, 0), 1);

    g = &T[GOAL_UNSTABLE];
    rx_graph_init(g, 0);
    int c = node_read(g, 3, 0, 0);
    check_publish(g, node_pure(g, OP_ADD, node_skill(g, SK_DRIFT, c, -1, 0), -1, 9), 2);

    g = &T[GOAL_FLAKY];
    rx_graph_init(g, 0);
    b = node_read(g, 2, 0, 0);
    check_publish(g, node_pure(g, OP_MUL, node_skill(g, SK_FLAKY, b, -1, 0), -1, 2), 3);

    /* Four sources the fragment cannot absorb (each is also checked), and
     * two World objects read inside it. */
    g = &T[GOAL_BUDGET];
    rx_graph_init(g, 0);
    int e1 = node_recall(g, 11), e2 = node_recall(g, 12), e3 = node_recall(g, 13);
    int e4 = node_read(g, 2, 0, 0);
    int srcs[4] = { e1, e2, e3, e4 };
    for (int i = 0; i < 4; i++) rx_graph_success(g, (uint32_t)node_verify(g, srcs[i], 0, UINT64_MAX), AG_OK);
    int k = N(g, AG_SKILL, AG_T_U64);
    g->nodes[k].op = SK_COMBINE4;
    for (int i = 0; i < 4; i++) rx_graph_data(g, (uint32_t)srcs[i], (uint32_t)k, (uint32_t)i, AG_EDGE_DATA);
    int p1 = node_pure(g, OP_ADD, k, node_read(g, 1, 0, 0), 0);
    int p2 = node_pure(g, OP_ADD, p1, node_read(g, 7, 0, 0), 0);
    check_publish(g, p2, 4);

    /* Two heavy steps that run side by side, joined. */
    for (uint32_t goal = GOAL_PARALLEL; goal <= GOAL_PARALLEL2; goal++) {
        g = &T[goal];
        rx_graph_init(g, 0);
        int ha = node_skill(g, SK_HEAVY, node_read(g, 1, 0, 0), -1, HEAVY_US);
        int hc = node_skill(g, SK_HEAVY, node_read(g, 3, 0, 0), -1, HEAVY_US);
        check_publish(g, node_skill(g, SK_FETCH, ha, hc, 0), goal == GOAL_PARALLEL ? 5 : 6);
    }
}

static AgGoal goal_for(Env *e, uint32_t kind) {
    AgGoal g = { kind, 7, { e->A, e->B, e->C, e->mem, e->out, e->effect, e->D } };
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

static int compile(Env *e, uint32_t kind, AgGraph *g) {
    AgGoal goal = goal_for(e, kind);
    AgConstraints c = open_constraints();
    AgReport rep;
    return rx_graph_compile(&goal, &e->w, &e->caps, &c, &e->lib, 1, g, &rep);
}

static int compile_fused(Env *e, uint32_t kind, const AgFusionLibrary *lib, const AgSkillTable *sk,
                         AgGraph *g, AgReport *rep, AgFusionReport *frep) {
    AgGoal goal = goal_for(e, kind);
    AgConstraints c = open_constraints();
    return rx_fusion_compile(&goal, &e->w, &e->caps, &c, &e->lib, lib, sk, g, rep, frep);
}

/* ---- running and comparing ---- */

static uint64_t a_value(uint64_t run) { return 2 + 3 * run; }   /* run = 5 (mod 16): score fails */

static void set_inputs(Env *e, uint64_t a, uint64_t b) {
    RxMutation m1 = { e->A, 0, a }, m2 = { e->B, 0, b };
    CHECK(rx_world_publish_external(&e->w, e->ext_A, &m1, 1) > 0, "publish A");
    CHECK(rx_world_publish_external(&e->w, e->ext_B, &m2, 1) > 0, "publish B");
    CHECK(settle(e) == RX_OK, "settle inputs");
}

static int64_t run(Env *e, AgLowered *L, uint64_t id, AgResult *res, uint64_t *ns) {
    uint64_t t0 = now_ns();
    int64_t ext = rx_graph_start(L, e->ext_run, id);
    CHECK(ext > 0, "run token refused");
    CHECK(settle(e) == RX_OK, "settle");
    if (ns) *ns = now_ns() - t0;
    rx_graph_collect(L, id, res);
    return ext;
}

static AgReference g_ref;

/* Lowered run against the sequential reference: every node's status,
 * value, evidence and step evidence; the outcome; the evidence owed. */
static int compare(const AgGraph *g, const AgResult *got, const AgReference *ref, const char *what) {
    R.eq_runs++;
    int bad = 0;
    for (uint32_t n = 0; n < g->n_nodes; n++) {
        R.eq_nodes++;
        if (got->status[n] != ref->r.status[n] || got->value[n] != ref->r.value[n] ||
            (got->status[n] && got->evidence[n] != ref->r.evidence[n]) || got->steps[n] != ref->r.steps[n]) {
            bad = 1;
            fprintf(stderr, "    %s node %u (%s): got s%u v%llu, reference s%u v%llu\n", what, n,
                    rx_graph_kind_name(g->nodes[n].kind), got->status[n],
                    (unsigned long long)got->value[n], ref->r.status[n],
                    (unsigned long long)ref->r.value[n]);
        }
    }
    if (got->outcome != ref->r.outcome) bad = 1;
    if (bad) R.eq_mismatch++;
    CHECK(!bad, "%s: lowered run differs from the sequential reference", what);
    if (got->outcome == AG_RUN_SUCCESS)
        for (uint32_t i = 0; i < g->n_evidence; i++) {
            uint32_t n = g->evidence[i];
            R.evidence_checked++;
            if (got->status[n] == AG_PENDING || got->evidence[n] == 0 ||
                (g->nodes[n].kind == AG_META && got->steps[n] == 0))
                R.evidence_missing++;
        }
    return !bad;
}

/* Crumb ancestry of every node inside the run's episode equals the graph's. */
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
        }
    }
    free(stack);
    free(seen);
    CHECK(!bad, "%s: causal ancestry differs from the graph", what);
}

static int meta_node(const AgGraph *g, uint32_t skill) {
    for (uint32_t n = 0; n < g->n_nodes; n++)
        if (g->nodes[n].kind == AG_META && g->nodes[n].op == skill) return (int)n;
    return -1;
}

static void hex(const uint8_t *d, char out[65]) {
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", d[i]);
}

/* ---- scenario 1: observation ---- */

static AgFusionObserver g_obs;
static AgFusionPolicy g_pol = { 24, 2, 100, 2 };
static int g_w = -1, g_par = -1;     /* pattern indices: the workflow, the parallel fragment */

static void t_observe(void) {
    printf("[*] observation: verified runs of eight procedures\n");
    static Env e;
    CHECK(env_start(&e, 4) == 0, "env");
    static AgGraph g[N_GOALS + 1];
    static AgLowered L[N_GOALS + 1];
    for (uint32_t k = 1; k <= N_GOALS; k++) {
        CHECK(compile(&e, k, &g[k]) == AG_OK_READY, "compile goal %u", k);
        CHECK(lower(&e, &L[k], &g[k], &g_skills) == 0, "lower goal %u", k);
    }
    rx_fusion_observer_init(&g_obs);
    AgResult res;
    for (uint64_t r = 1; r <= OBSERVE_RUNS; r++) {
        set_inputs(&e, a_value(r), r);
        for (uint32_t k = 1; k <= N_GOALS; k++) {
            g_drift = r;
            CHECK(rx_graph_reference(&g[k], &e.w, &g_skills, &e.caps, r, &g_ref) == 0, "reference");
            run(&e, &L[k], r, &res, NULL);
            compare(&g[k], &res, &g_ref, "observe");
            CHECK(rx_fusion_observe(&g_obs, &e.w, &g[k], &res, &g_ref) == 1, "verified run taken in");
        }
    }
    /* A run that does not match its reference is not evidence of anything. */
    AgResult bad = res;
    bad.value[0] ^= 1;
    CHECK(rx_fusion_observe(&g_obs, &e.w, &g[N_GOALS], &bad, &g_ref) == 0, "unverified run refused");
    R.obs_runs = g_obs.runs_observed;
    R.obs_refused = g_obs.runs_refused;
    R.obs_patterns = g_obs.n;
    R.obs_fragments = g_obs.fragments_seen;
    R.obs_over_budget = g_obs.fragments_over_budget;
    R.obs_dropped = g_obs.patterns_dropped;
    CHECK(R.obs_runs == OBSERVE_RUNS * N_GOALS && R.obs_refused == 1, "runs %u refused %u",
          R.obs_runs, R.obs_refused);
    CHECK(R.obs_dropped == 0, "no pattern dropped");
    CHECK(R.obs_over_budget > 0, "the budget procedure's full fragment does not fit one reaction");

    /* The workflow: four steps, one port (base), one object (A), in the
     * three procedures that share it. */
    for (uint32_t i = 0; i < g_obs.n; i++) {
        const AgFusionPattern *p = &g_obs.p[i];
        if (p->prog.n_steps == 4 && p->prog.n_in == 1 && p->prog.n_obj == 1 &&
            p->prog.step[3].node.kind == AG_SKILL && p->prog.step[3].node.op == SK_SCORE)
            g_w = (int)i;
        if (p->prog.n_steps == 5 && p->prog.n_obj == 2 && p->prog.step[4].node.op == SK_FETCH) g_par = (int)i;
        CHECK(!(p->prog.n_in == 4 && p->prog.n_obj == 2), "no pattern beyond the reaction budget");
    }
    CHECK(g_w >= 0 && g_par >= 0, "workflow and parallel patterns found");
    if (g_w < 0 || g_par < 0) { env_stop(&e); return; }
    const AgFusionPattern *w = &g_obs.p[g_w];
    R.w_steps = w->prog.n_steps;
    R.w_occ = w->occurrences;
    R.w_graphs = w->n_graphs;
    R.w_ok = w->ok;
    R.w_failed = w->failed;
    R.w_stable = w->evidence_stable;
    R.w_samples = w->n_samples;
    hex(w->prog.signature, R.w_signature);
    CHECK(w->occurrences == 3 * OBSERVE_RUNS && w->n_graphs == 3, "workflow: %u occurrences in %u graphs",
          w->occurrences, w->n_graphs);
    CHECK(w->failed == 3 * 2 && w->ok == 3 * (OBSERVE_RUNS - 2), "workflow fails where score fails");
    CHECK(w->evidence_unstable == 0 && w->evidence_stable > 0, "workflow evidence stable");
    CHECK(w->prog.obj_rights[0] == RX_RIGHT_READ && w->prog.in_type[0] == AG_T_U64 &&
          w->prog.out_type == AG_T_U64, "workflow contracts");

    /* Judging: unstable, flaky and single-graph fragments do not qualify. */
    uint32_t idx[AG_FUSION_MAX_PATTERNS];
    R.n_candidates = rx_fusion_candidates(&g_obs, &g_pol, idx, AG_FUSION_MAX_PATTERNS);
    int w_in = 0, par_in = 0;
    for (uint32_t i = 0; i < R.n_candidates; i++) {
        w_in |= (int)idx[i] == g_w;
        par_in |= (int)idx[i] == g_par;
    }
    CHECK(w_in && par_in, "workflow and parallel fragments are candidates");
    CHECK(R.n_candidates > 0 && (int)idx[0] == g_par, "largest candidate first");
    for (uint32_t i = 0; i < g_obs.n; i++) {
        const AgFusionPattern *p = &g_obs.p[i];
        int j = rx_fusion_judge(&g_obs, i, &g_pol);
        int drift = 0, flaky = 0;
        for (uint32_t s = 0; s < p->prog.n_steps; s++) {
            drift |= p->prog.step[s].node.kind == AG_SKILL && p->prog.step[s].node.op == SK_DRIFT;
            flaky |= p->prog.step[s].node.kind == AG_SKILL && p->prog.step[s].node.op == SK_FLAKY;
        }
        if (drift) { R.judged_unstable += j == AG_FUSE_UNSTABLE; CHECK(j == AG_FUSE_UNSTABLE, "drift: unstable (%d)", j); }
        if (flaky) { R.judged_failure_rate += j == AG_FUSE_FAILURE_RATE; CHECK(j == AG_FUSE_FAILURE_RATE, "flaky: failure rate (%d)", j); }
        if (j == AG_FUSE_TOO_FEW_GRAPHS) R.judged_few_graphs++;
    }
    CHECK(R.judged_unstable > 0 && R.judged_failure_rate > 0, "unstable and flaky patterns seen");
    CHECK(R.judged_few_graphs > 0, "budget-procedure fragments appear in one graph only");
    printf("    %u verified runs (%u refused); %u patterns; workflow %u steps, %u occurrences in %u graphs, "
           "%u failed; %u candidates\n", R.obs_runs, R.obs_refused, R.obs_patterns, R.w_steps, R.w_occ,
           R.w_graphs, R.w_failed, R.n_candidates);
    env_stop(&e);
}

/* ---- scenario 2: candidates and verification ---- */

static AgMetaSkill MC, MH, ML, MW, MQ, MP;

static void verdict_note(uint32_t i, const AgMetaSkill *m) {
    R.ver[i].state = m->state;
    R.ver[i].reason = m->verdict.reason;
    R.ver[i].vectors = m->verdict.vectors;
    R.ver[i].observed = m->verdict.observed;
    R.ver[i].generated = m->verdict.generated;
    R.ver[i].combos = m->verdict.status_combos;
    R.ver[i].hits = m->verdict.table_hits;
}

static void retag(AgMetaSkill *m) {
    rx_graph_realization_identity(&m->realization);
    rx_fusion_identity(m, m->identity);
}

static void tamper(const char *name, AgMetaSkill *t, uint32_t want) {
    t->state = AG_MS_CANDIDATE;
    rx_fusion_verify(t, &g_skills, 7);
    R.tamper_cases++;
    int caught = t->state == AG_MS_REJECTED && t->verdict.reason == want;
    R.tamper_caught += caught;
    CHECK(caught, "tamper %s: state %s reason %s, want %s", name, rx_fusion_state_name(t->state),
          rx_fusion_reason_name(t->verdict.reason), rx_fusion_reason_name(want));
    size_t l = strlen(R.tamper_names);
    snprintf(R.tamper_names + l, sizeof R.tamper_names - l, "%s\"%s: %s\"", l ? ", " : "", name,
             rx_fusion_reason_name(t->verdict.reason));
}

static void t_verify(void) {
    printf("[*] candidates and verification against the original fragment\n");
    if (g_w < 0 || g_par < 0) return;
    const AgFusionPattern *w = &g_obs.p[g_w];
    CHECK(rx_fusion_build(w, AG_REAL_COMPILED, MS_COMPILED, &MC) == 0, "build compiled");
    CHECK(rx_fusion_build(w, AG_REAL_HYBRID, MS_HYBRID, &MH) == 0, "build hybrid");
    CHECK(rx_fusion_build(w, AG_REAL_LEARNED, MS_LEARNED, &ML) == 0, "build learned");
    CHECK(rx_fusion_build(w, AG_REAL_HARDWARE, MS_HARDWARE, &MW) == 0, "build hardware");
    CHECK(rx_fusion_build(&g_obs.p[g_par], AG_REAL_COMPILED, MS_PARALLEL, &MP) == 0, "build parallel");
    CHECK(MC.n_ancestry > 0 && MC.state == AG_MS_CANDIDATE && MC.effect_contract.performs == 0 &&
          MC.authority_contract.n_obj == 1 && MC.authority_contract.rights[0] == RX_RIGHT_READ,
          "compiled candidate: ancestry, READ on one object, no effects");
    CHECK(rx_fusion_find(&T[GOAL_EFFECT], NULL, MC.reference.signature, NULL, 0) == 0,
          "find with no room finds nothing");

    AgMetaSkill *ms[4] = { &MC, &MH, &ML, &MW };
    for (uint32_t i = 0; i < 4; i++) {
        CHECK(rx_fusion_verify(ms[i], &g_skills, 1) == 0, "verify ran");
        verdict_note(i, ms[i]);
    }
    CHECK(rx_fusion_verify(&MP, &g_skills, 1) == 0 && MP.state == AG_MS_VERIFIED, "parallel verified");
    CHECK(MC.state == AG_MS_VERIFIED && MC.verdict.pass && MC.verdict.observed == MC.n_samples &&
          MC.verdict.status_combos == 3 && MC.verdict.semantic_mismatch == 0 &&
          MC.verdict.failure_mismatch == 0 && MC.verdict.evidence_mismatch == 0,
          "compiled: verified over observed inputs and every port status (%s)",
          rx_fusion_reason_name(MC.verdict.reason));
    CHECK(MH.state == AG_MS_VERIFIED && MH.verdict.table_hits > 0, "hybrid: verified, learned entries used");
    CHECK(ML.state == AG_MS_REJECTED && ML.verdict.reason == AG_VR_NOT_RUNNABLE,
          "learned alone: rejected outside what it saw (%s)", rx_fusion_reason_name(ML.verdict.reason));
    CHECK(MW.state == AG_MS_REJECTED && MW.verdict.reason == AG_VR_NOT_RUNNABLE,
          "hardware: no realization on this host");

    /* Tampering. Each copy starts from a good candidate. */
    static AgMetaSkill t;
    AgMetaSkill base_c = MC, base_h = MH;
    base_c.state = base_h.state = AG_MS_CANDIDATE;

    t = base_c;
    t.authority_contract.rights[0] = 0;
    retag(&t);
    tamper("authority narrowed", &t, AG_VR_AUTHORITY);

    t = base_c;
    t.authority_contract.rights[0] |= RX_RIGHT_WRITE;
    retag(&t);
    tamper("authority widened", &t, AG_VR_AUTHORITY);

    t = base_c;
    t.effect_contract.performs = 1;
    retag(&t);
    tamper("claims an effect", &t, AG_VR_EFFECT);

    t = base_c;
    t.output_contract.may_fail = 0;
    tamper("changed without new identity", &t, AG_VR_IDENTITY);

    t = base_c;
    t.realization.prog.step[2].node.imm += 1;       /* the *3 step */
    rx_graph_meta_seal(&t.realization.prog);
    retag(&t);
    tamper("program step changed", &t, AG_VR_CONTRACT);

    t = base_h;
    uint32_t ok_i = UINT32_MAX, fail_i = UINT32_MAX;
    for (uint32_t i = 0; i < t.realization.n_table; i++) {
        if (t.realization.table[i].status == AG_OK && ok_i == UINT32_MAX) ok_i = i;
        if (t.realization.table[i].status == AG_FAILED && fail_i == UINT32_MAX) fail_i = i;
    }
    CHECK(ok_i != UINT32_MAX && fail_i != UINT32_MAX, "hybrid table holds OK and FAILED entries");
    if (ok_i != UINT32_MAX && fail_i != UINT32_MAX) {
        t.realization.table[ok_i].value ^= 1;
        retag(&t);
        tamper("learned wrong value", &t, AG_VR_SEMANTIC);

        t = base_h;
        t.realization.table[ok_i].steps ^= 2;
        retag(&t);
        tamper("learned right value, wrong evidence", &t, AG_VR_EVIDENCE);

        t = base_h;
        t.realization.table[fail_i].status = AG_OK;
        t.realization.table[fail_i].value = 12345;
        retag(&t);
        tamper("learned failure turned into success", &t, AG_VR_FAILURE);
    }

    /* A coherent rewrite (reference and realization changed together) no
     * longer reproduces what was observed, and matches no real workflow. */
    t = base_c;
    t.reference.step[2].node.imm += 1;
    rx_graph_meta_seal(&t.reference);
    t.realization.prog = t.reference;
    retag(&t);
    tamper("reference and realization rewritten together", &t, AG_VR_SEMANTIC);
    AgGraph g;
    static Env e;
    CHECK(env_start(&e, 1) == 0, "env");
    CHECK(compile(&e, GOAL_EFFECT, &g) == AG_OK_READY, "compile");
    AgFusionSite sites[4];
    R.coherent_rewrite_sites = rx_fusion_find(&g, &e.w, t.reference.signature, sites, 4);
    CHECK(R.coherent_rewrite_sites == 0, "the rewritten fragment matches no real workflow");
    CHECK(rx_fusion_find(&g, &e.w, MC.reference.signature, sites, 4) == 1, "the real workflow is found once");
    env_stop(&e);
    printf("    compiled %s (%u vectors: %u observed, %u generated over %u port-status combinations); "
           "hybrid %s (%u learned hits); learned %s (%s); hardware %s; tampering caught %u/%u\n",
           rx_fusion_state_name(MC.state), MC.verdict.vectors, MC.verdict.observed, MC.verdict.generated,
           MC.verdict.status_combos, rx_fusion_state_name(MH.state), MH.verdict.table_hits,
           rx_fusion_state_name(ML.state), rx_fusion_reason_name(ML.verdict.reason),
           rx_fusion_state_name(MW.state), R.tamper_caught, R.tamper_cases);
}

/* ---- never silently: an unpublished MetaSkill is not applied ---- */

static void unpublished_refused(const AgMetaSkill *m, const char *when) {
    static Env e;
    CHECK(env_start(&e, 1) == 0, "env");
    AgGraph plain, fused;
    AgReport rep;
    AgFusionReport frep;
    CHECK(compile(&e, GOAL_EFFECT, &plain) == AG_OK_READY, "compile");
    AgFusionLibrary lib = { 1, { (AgMetaSkill *)m } };
    AgSkillTable sk = g_skills;
    sk.meta[sk.n_meta++] = (typeof(sk.meta[0])){ m->id, &m->realization };   /* even if present */
    CHECK(compile_fused(&e, GOAL_EFFECT, &lib, &sk, &fused, &rep, &frep) == AG_OK_READY, "compile fused");
    int same = memcmp(plain.digest, fused.digest, 32) == 0 && meta_node(&fused, m->id) < 0;
    R.unpublished_refusals += frep.applied == 0 && frep.refused_unpublished == 1;
    R.unpublished_digest_equal += same;
    CHECK(frep.applied == 0 && frep.refused_unpublished == 1 && same,
          "%s MetaSkill not applied (applied %u refused %u)", when, frep.applied, frep.refused_unpublished);
    env_stop(&e);
}

/* ---- measurement: original and fused, side by side ---- */

typedef struct {
    Env o, f;
    AgGraph go, gf;
    AgLowered lo, lf;
    AgSkillTable trial;
    AgFusionSite site;
    int meta;
    const AgMetaSkill *m;
} Pair;

static Pair g_pair;

static int pair_start(Pair *p, uint32_t goal, const AgMetaSkill *m, uint32_t workers) {
    memset(p, 0, sizeof *p);
    p->m = m;
    if (env_start(&p->o, workers) != 0 || env_start(&p->f, workers) != 0) return -1;
    if (compile(&p->o, goal, &p->go) != AG_OK_READY || compile(&p->f, goal, &p->gf) != AG_OK_READY) return -1;
    AgGraph g = p->gf;
    uint32_t applied = 0;
    if (rx_fusion_trial(&g, &p->f.w, m, &g_skills, &p->trial, &p->gf, &applied) != 0 || applied != 1) return -1;
    p->meta = meta_node(&p->gf, m->id);
    if (rx_fusion_find(&p->go, &p->o.w, m->reference.signature, &p->site, 1) != 1 || p->meta < 0) return -1;
    if (lower(&p->o, &p->lo, &p->go, &g_skills) != 0 || lower(&p->f, &p->lf, &p->gf, &p->trial) != 0) return -1;
    return 0;
}

static void pair_stop(Pair *p) {
    env_stop(&p->o);
    env_stop(&p->f);
}

/* Same inputs into both; returns 1 when the fused run diverged from the
 * original on anything the original produced. */
static int pair_run(Pair *p, uint64_t id, uint64_t a, AgResult *ro, AgResult *rf, uint64_t *ns_o,
                    uint64_t *ns_f, int check_reference) {
    set_inputs(&p->o, a, id);
    set_inputs(&p->f, a, id);
    run(&p->o, &p->lo, id, ro, ns_o);
    if (check_reference)
        CHECK(rx_graph_reference(&p->gf, &p->f.w, &p->trial, &p->f.caps, id, &g_ref) == 0, "reference");
    run(&p->f, &p->lf, id, rf, ns_f);
    if (check_reference) compare(&p->gf, rf, &g_ref, "fused");
    uint32_t x = p->site.exit, m = (uint32_t)p->meta;
    R.cross_runs++;
    int div = ro->status[x] != rf->status[m] || ro->value[x] != rf->value[m] ||
              ro->attempts[x] != rf->attempts[m] || ro->outcome != rf->outcome;
    for (uint32_t f = 0; f < RX_MAX_FIELDS; f++)
        div |= fld(&p->o, p->o.out, f) != fld(&p->f, p->f.out, f);
    for (uint32_t f = 0; f < 4; f++)
        div |= fld(&p->o, p->o.effect, f) != fld(&p->f, p->f.effect, f);
    if (ro->status[x] != AG_PENDING) {
        R.steps_checked++;
        if (rx_fusion_site_steps(&p->go, &p->site, &p->m->reference, ro) != rf->steps[m]) {
            R.steps_mismatch++;
            div = 1;
        }
    }
    R.cross_mismatch += (uint32_t)div;
    return div;
}

/* ---- energy (package telemetry, where the machine has it) ---- */

static char g_epkg[128], g_ecpu[128];

static int energy_open(void) {
    for (int h = 0; h < 32; h++) {
        char path[128], name[64] = { 0 };
        snprintf(path, sizeof path, "/sys/class/hwmon/hwmon%d/name", h);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        int ok = fgets(name, sizeof name, fp) != NULL;
        fclose(fp);
        if (!ok || strncmp(name, "aien_spbm", 9) != 0) continue;
        snprintf(g_epkg, sizeof g_epkg, "/sys/class/hwmon/hwmon%d/energy1_input", h);
        snprintf(g_ecpu, sizeof g_ecpu, "/sys/class/hwmon/hwmon%d/energy3_input", h);
        return 1;
    }
    return 0;
}

static uint64_t energy_uj(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    unsigned long long v = 0;
    if (fscanf(fp, "%llu", &v) != 1) v = 0;
    fclose(fp);
    return v;
}

typedef struct { double pkg_uj, cpu_uj, seconds; uint64_t runs; } EnergyLeg;

static void energy_leg(Pair *p, int fused, double seconds, uint64_t *next_id, EnergyLeg *out) {
    Env *e = fused ? &p->f : &p->o;
    AgLowered *L = fused ? &p->lf : &p->lo;
    AgResult res;
    uint64_t e0 = energy_uj(g_epkg), c0 = energy_uj(g_ecpu), t0 = now_ns(), n = 0;
    while ((double)(now_ns() - t0) < seconds * 1e9) {
        uint64_t id = (*next_id)++;
        run(e, L, id, &res, NULL);
        n++;
    }
    out->seconds = (double)(now_ns() - t0) / 1e9;
    out->pkg_uj = (double)(energy_uj(g_epkg) - e0);
    out->cpu_uj = (double)(energy_uj(g_ecpu) - c0);
    out->runs = n;
}

static void energy_idle(double seconds, EnergyLeg *out) {
    uint64_t e0 = energy_uj(g_epkg), c0 = energy_uj(g_ecpu), t0 = now_ns();
    struct timespec ts = { (time_t)seconds, (long)((seconds - (double)(time_t)seconds) * 1e9) };
    nanosleep(&ts, NULL);
    out->seconds = (double)(now_ns() - t0) / 1e9;
    out->pkg_uj = (double)(energy_uj(g_epkg) - e0);
    out->cpu_uj = (double)(energy_uj(g_ecpu) - c0);
    out->runs = 0;
}

/* Energy per run of one leg with the idle power around it subtracted. */
static double per_run(const EnergyLeg *leg, const EnergyLeg *i0, const EnergyLeg *i1, int cpu) {
    double idle = ((cpu ? i0->cpu_uj : i0->pkg_uj) + (cpu ? i1->cpu_uj : i1->pkg_uj)) /
                  (i0->seconds + i1->seconds);
    return ((cpu ? leg->cpu_uj : leg->pkg_uj) - idle * leg->seconds) / (double)leg->runs;
}

static struct {
    int available;
    double pkg[2][2], cpu[2][2];     /* [round][before, after] */
} g_energy;

/* Two rounds of idle, original, idle, fused, idle (1 s each). The spread
 * between rounds is the noise; a difference inside it is not claimed. */
static void measure_energy(Pair *p, AgFusionMeasure *ms) {
    if (!energy_open()) return;
    uint64_t id = 100000;
    set_inputs(&p->o, a_value(3), 3);
    set_inputs(&p->f, a_value(3), 3);
    for (int r = 0; r < 2; r++) {
        EnergyLeg i0, lo, i1, lf, i2;
        energy_idle(1.0, &i0);
        energy_leg(p, 0, 1.0, &id, &lo);
        energy_idle(1.0, &i1);
        energy_leg(p, 1, 1.0, &id, &lf);
        energy_idle(1.0, &i2);
        if (!lo.runs || !lf.runs) return;
        g_energy.pkg[r][0] = per_run(&lo, &i0, &i1, 0);
        g_energy.pkg[r][1] = per_run(&lf, &i1, &i2, 0);
        g_energy.cpu[r][0] = per_run(&lo, &i0, &i1, 1);
        g_energy.cpu[r][1] = per_run(&lf, &i1, &i2, 1);
    }
    g_energy.available = 1;
    ms->energy_available = 1;
    ms->energy_uj_before = (g_energy.pkg[0][0] + g_energy.pkg[1][0]) / 2;
    ms->energy_uj_after = (g_energy.pkg[0][1] + g_energy.pkg[1][1]) / 2;
    ms->energy_noise_uj = fmax(fabs(g_energy.pkg[0][0] - g_energy.pkg[1][0]),
                               fabs(g_energy.pkg[0][1] - g_energy.pkg[1][1])) / 2;
}

static double cpu_mean(int leg) { return (g_energy.cpu[0][leg] + g_energy.cpu[1][leg]) / 2; }
static double cpu_noise(void) {
    return fmax(fabs(g_energy.cpu[0][0] - g_energy.cpu[1][0]), fabs(g_energy.cpu[0][1] - g_energy.cpu[1][1])) / 2;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* 41 interleaved runs per leg at 4 workers: reactions that computed per
 * run, crumbs per run, median latency, CPU time, outcomes. */
static void measure(AgMetaSkill *m, uint32_t goal, AgFusionMeasure *ms, int with_energy, const char *what) {
    Pair *p = &g_pair;
    CHECK(pair_start(p, goal, m, 4) == 0, "%s: pair", what);
    memset(ms, 0, sizeof *ms);
    AgResult ro, rf;
    for (uint64_t r = 1; r <= 3; r++) pair_run(p, r, a_value(r), &ro, &rf, NULL, NULL, 0);
    enum { RUNS = 41 };
    uint64_t ns_o[RUNS], ns_f[RUNS], cpu_o = 0, cpu_f = 0, cr_o = 0, cr_f = 0, cm_o = 0, cm_f = 0;
    for (uint64_t i = 0; i < RUNS; i++) {
        uint64_t id = 10 + i, a = a_value(id);
        set_inputs(&p->o, a, id);
        set_inputs(&p->f, a, id);
        uint64_t c0 = p->o.w.n_crumbs, t0 = cpu_ns();
        run(&p->o, &p->lo, id, &ro, &ns_o[i]);
        cpu_o += cpu_ns() - t0;
        cr_o += p->o.w.n_crumbs - c0;
        cm_o += commits_between(&p->o, c0, p->o.w.n_crumbs);
        c0 = p->f.w.n_crumbs;
        t0 = cpu_ns();
        run(&p->f, &p->lf, id, &rf, &ns_f[i]);
        cpu_f += cpu_ns() - t0;
        cr_f += p->f.w.n_crumbs - c0;
        cm_f += commits_between(&p->f, c0, p->f.w.n_crumbs);
        ms->ok_before += ro.outcome == AG_RUN_SUCCESS;
        ms->failed_before += ro.outcome == AG_RUN_FAILURE;
        ms->ok_after += rf.outcome == AG_RUN_SUCCESS;
        ms->failed_after += rf.outcome == AG_RUN_FAILURE;
    }
    qsort(ns_o, RUNS, sizeof ns_o[0], cmp_u64);
    qsort(ns_f, RUNS, sizeof ns_f[0], cmp_u64);
    ms->runs = RUNS;
    ms->median_ns_before = ns_o[RUNS / 2];
    ms->median_ns_after = ns_f[RUNS / 2];
    ms->cpu_ns_before = cpu_o / RUNS;
    ms->cpu_ns_after = cpu_f / RUNS;
    ms->crumbs_before = (double)cr_o / RUNS;
    ms->crumbs_after = (double)cr_f / RUNS;
    ms->commits_before = (double)cm_o / RUNS;
    ms->commits_after = (double)cm_f / RUNS;
    ms->reactions_before = p->go.n_nodes;
    ms->reactions_after = p->gf.n_nodes;
    ms->objects_before = p->go.n_nodes + 1;
    ms->objects_after = p->gf.n_nodes + 1;
    if (with_energy) measure_energy(p, ms);
    pair_stop(p);
    printf("    %s: reactions computing per run %.1f -> %.1f, crumbs %.1f -> %.1f, median %.1f -> %.1f us, "
           "CPU %.1f -> %.1f us/run", what, ms->commits_before, ms->commits_after, ms->crumbs_before,
           ms->crumbs_after, ms->median_ns_before / 1e3, ms->median_ns_after / 1e3,
           ms->cpu_ns_before / 1e3, ms->cpu_ns_after / 1e3);
    if (ms->energy_available)
        printf("; energy per run (idle subtracted, spread between two rounds): package %.1f -> %.1f "
               "(+/- %.1f) uJ, performance cores %.1f -> %.1f (+/- %.1f) uJ", ms->energy_uj_before,
               ms->energy_uj_after, ms->energy_noise_uj, cpu_mean(0), cpu_mean(1), cpu_noise());
    printf("\n");
}

static void t_measure(void) {
    printf("[*] measurement: original and fused side by side\n");
    if (MC.state != AG_MS_VERIFIED) return;
    unpublished_refused(&MC, "verified");
    measure(&MC, GOAL_EFFECT, &R.m_compiled, 1, "compiled");
    CHECK(rx_fusion_accept_measure(&MC, &R.m_compiled, 50) == 0 && MC.state == AG_MS_MEASURED,
          "compiled accepted: fewer reactions, not slower, same outcomes");
    measure(&MH, GOAL_EFFECT, &R.m_hybrid, 0, "hybrid");
    rx_fusion_accept_measure(&MH, &R.m_hybrid, 50);
    R.hybrid_state = MH.state;
    CHECK(MH.state == AG_MS_MEASURED, "hybrid accepted");
    measure(&MP, GOAL_PARALLEL, &R.m_parallel, 0, "parallel");
    CHECK(rx_fusion_accept_measure(&MP, &R.m_parallel, 50) != 0 && MP.state == AG_MS_SLOWER,
          "parallel fragment refused: fusing it serializes independent work (%.0f -> %.0f us)",
          R.m_parallel.median_ns_before / 1e3, R.m_parallel.median_ns_after / 1e3);
    R.parallel_state = MP.state;
    CHECK(rx_fusion_canary(&MP, 0, 1) == AG_FUSION_E_STATE, "a slower MetaSkill cannot enter canary");
    unpublished_refused(&MC, "measured");
}

/* ---- canary ---- */

static void t_canary(void) {
    printf("[*] canary: fused graph in shadow beside production\n");
    if (MC.state != AG_MS_MEASURED) return;
    Pair *p = &g_pair;
    CHECK(pair_start(p, GOAL_EFFECT, &MC, 4) == 0, "pair");
    AgResult ro, rf;
    for (uint64_t r = 1; r <= 16 && MC.state == AG_MS_MEASURED; r++) {
        uint64_t a = r == 10 ? 4242 : 1000 + 7 * r;       /* traffic it never saw */
        int div = pair_run(p, 200 + r, a, &ro, &rf, NULL, NULL, 1);
        rx_fusion_canary(&MC, div, 16);
    }
    pair_stop(p);
    R.canary_runs = MC.canary_runs;
    R.canary_divergences = MC.canary_divergences;
    CHECK(MC.state == AG_MS_CANARY_PASSED && MC.canary_divergences == 0, "compiled passed canary (%s)",
          rx_fusion_state_name(MC.state));

    /* A hybrid with one poisoned learned entry, for an input verification
     * never generates. It verifies; the canary catches it. */
    MQ = MH;
    MQ.id = MS_POISONED;
    MQ.state = AG_MS_CANDIDATE;
    AgMetaEntry *q = &MQ.realization.table[MQ.realization.n_table++];
    memset(q, 0, sizeof *q);
    q->s[0] = AG_OK;
    q->v[0] = 500;
    uint64_t world[AG_META_MAX_OBJ][RX_MAX_FIELDS];
    memset(world, 0, sizeof world);
    world[0][0] = 4242;
    q->world = rx_graph_meta_world_key(&MQ.realization.prog, (const uint64_t (*)[RX_MAX_FIELDS])world);
    q->status = AG_OK;
    q->value = 1;
    q->steps = 1;
    retag(&MQ);
    rx_fusion_verify(&MQ, &g_skills, 1);
    R.poisoned_verified = MQ.state == AG_MS_VERIFIED;
    CHECK(MQ.state == AG_MS_VERIFIED, "poisoned hybrid passes verification (the entry is never generated)");
    AgFusionMeasure mq;
    measure(&MQ, GOAL_EFFECT, &mq, 0, "poisoned hybrid");
    rx_fusion_accept_measure(&MQ, &mq, 50);
    CHECK(pair_start(p, GOAL_EFFECT, &MQ, 4) == 0, "pair");
    /* The divergence here is the point; it is kept out of the fusion totals. */
    uint32_t saved_runs = R.cross_runs, saved_mis = R.cross_mismatch, saved_sc = R.steps_checked,
             saved_sm = R.steps_mismatch;
    for (uint64_t r = 1; r <= 16 && MQ.state == AG_MS_MEASURED; r++) {
        uint64_t a = r == 10 ? 4242 : 1000 + 7 * r;
        int div = pair_run(p, 300 + r, a, &ro, &rf, NULL, NULL, 0);
        rx_fusion_canary(&MQ, div, 16);
    }
    R.poisoned_canary_runs = MQ.canary_runs;
    R.cross_runs = saved_runs;
    R.cross_mismatch = saved_mis;
    R.steps_checked = saved_sc;
    R.steps_mismatch = saved_sm;
    pair_stop(p);
    R.poisoned_state = MQ.state;
    CHECK(MQ.state == AG_MS_QUARANTINED && MQ.canary_divergences == 1, "poisoned hybrid quarantined (%s)",
          rx_fusion_state_name(MQ.state));
    unpublished_refused(&MC, "canary-passed");
}

/* ---- promotion and publication ---- */

typedef struct { AienosCapView *view; } AuthCtx;

static int native_auth(void *ctx, uint32_t cap_id, uint32_t cap_generation, uint32_t subject,
                       uint64_t resource, uint32_t rights) {
    AuthCtx *a = ctx;
    AienosCapEntry entry;
    return aienos_cap_validate(a->view, (AienosCapRef){ cap_id, cap_generation }, subject, resource,
                               rights, &entry);
}

static AgSkillTable g_pub;
static char g_store_dir[64];

static void t_promote(void) {
    printf("[*] promotion through the generation barrier, then publication\n");
    static Env e;
    CHECK(env_start(&e, 1) == 0, "env");
    strcpy(g_store_dir, "/tmp/rx-fusion-XXXXXX");
    CHECK(mkdtemp(g_store_dir) != NULL, "store dir");
    RxGenStore *store = NULL;
    CHECK(rx_gen_open(g_store_dir, &store) == RX_GEN_OK, "open store");
    AuthCtx ac = { e.view };

    RxCapRef weak = mint(&e, SUBJ_PROMOTER, RX_GEN_RES_PROMOTION, RX_RIGHT_READ);
    RxCapRef right = mint(&e, SUBJ_PROMOTER, RX_GEN_RES_PROMOTION, RX_GEN_RIGHT_PROMOTE);
    RxCapRef self = mint(&e, SUBJ_PROPOSER, RX_GEN_RES_PROMOTION, RX_GEN_RIGHT_PROMOTE);
    RxPromotionRequest req = { 0, SUBJ_PROMOTER, weak.cap_id, weak.generation, RX_GEN_RES_PROMOTION,
                               RX_GEN_RIGHT_PROMOTE };
    uint64_t active0 = 0, lineage0 = 0;
    CHECK(rx_gen_active(store, &active0, &lineage0) == RX_GEN_OK, "store has an active generation");

    CHECK(rx_fusion_promote(&MQ, store, SUBJ_PROPOSER, &req, native_auth, &ac) == AG_FUSION_E_STATE,
          "quarantined MetaSkill cannot be promoted");
    R.poisoned_promote_refused = 1;
    int rc = rx_fusion_promote(&MC, store, SUBJ_PROPOSER, &req, native_auth, &ac);
    R.promote_refused_no_right = rc == RX_GEN_ERR_AUTHORITY && MC.state == AG_MS_CANARY_PASSED;
    CHECK(R.promote_refused_no_right, "promotion without the promote right refused (%d)", rc);
    req = (RxPromotionRequest){ 0, SUBJ_PROPOSER, self.cap_id, self.generation, RX_GEN_RES_PROMOTION,
                                RX_GEN_RIGHT_PROMOTE };
    rc = rx_fusion_promote(&MC, store, SUBJ_PROPOSER, &req, native_auth, &ac);
    R.promote_refused_same_subject = rc == RX_GEN_ERR_AUTHORITY && MC.state == AG_MS_CANARY_PASSED;
    CHECK(R.promote_refused_same_subject, "the proposer cannot promote its own candidate (%d)", rc);
    uint64_t active = 0, lineage = 0;
    CHECK(rx_gen_active(store, &active, &lineage) == RX_GEN_OK && active == active0,
          "refusals left the active generation as it was");

    req = (RxPromotionRequest){ 0, SUBJ_PROMOTER, right.cap_id, right.generation, RX_GEN_RES_PROMOTION,
                                RX_GEN_RIGHT_PROMOTE };
    rc = rx_fusion_promote(&MC, store, SUBJ_PROPOSER, &req, native_auth, &ac);
    CHECK(rc == RX_GEN_OK && MC.state == AG_MS_PROMOTED && MC.generation != active0, "promoted (%d)", rc);
    R.generation = MC.generation;
    CHECK(rx_gen_active(store, &active, &lineage) == RX_GEN_OK && active == MC.generation,
          "the promoted generation is active");
    unpublished_refused(&MC, "promoted");

    /* The MetaSkill changed after promotion: publication refused. */
    static AgMetaSkill changed;
    changed = MC;
    changed.realization.prog.step[1].node.cost_us += 1;
    rx_graph_meta_seal(&changed.realization.prog);
    retag(&changed);
    rc = rx_fusion_publish(&changed, store, &g_pub);
    R.publish_refused_changed = rc == AG_FUSION_E_IDENTITY;
    CHECK(R.publish_refused_changed && g_pub.n_meta == 0, "changed after promotion: not published (%d)", rc);

    g_pub = g_skills;
    CHECK(rx_fusion_publish(&MC, store, &g_pub) == 0 && MC.state == AG_MS_PUBLISHED && g_pub.n_meta == 1,
          "published into the Skill Net table");
    rx_gen_close(store);
    env_stop(&e);
    printf("    refused without the right and for the proposer itself; generation %llu active; "
           "published as skill %u\n", (unsigned long long)MC.generation, MC.id);
}

/* ---- use after publication ---- */

static void semantic_digest(const AgGraph *g, const AgResult *r, Env *e, uint8_t out[32]) {
    sha256_ctx c;
    sha256_init(&c);
    for (uint32_t n = 0; n < g->n_nodes; n++) {
        sha256_update(&c, &r->status[n], 1);
        sha256_update(&c, (const uint8_t *)&r->value[n], 8);
        sha256_update(&c, (const uint8_t *)&r->evidence[n], 8);
        sha256_update(&c, (const uint8_t *)&r->steps[n], 8);
    }
    for (uint32_t f = 0; f < 4; f++) {
        uint64_t v = fld(e, e->effect, f);
        sha256_update(&c, (const uint8_t *)&v, 8);
    }
    sha256_final(&c, out);
}

static void t_use(void) {
    printf("[*] use: compile applies the published MetaSkill, and only it\n");
    if (MC.state != AG_MS_PUBLISHED) return;
    AgFusionLibrary lib = { 4, { &MC, &MQ, &MP, &MH } };
    static Env o, f;
    for (uint32_t goal = GOAL_EFFECT; goal <= GOAL_COMPARE; goal++) {
        CHECK(env_start(&o, 4) == 0 && env_start(&f, 4) == 0, "envs");
        static AgGraph go, gf;
        AgReport rep;
        AgFusionReport frep;
        CHECK(compile(&o, goal, &go) == AG_OK_READY, "compile original");
        CHECK(compile_fused(&f, goal, &lib, &g_pub, &gf, &rep, &frep) == AG_OK_READY, "compile fused %d",
              rep.verdict);
        R.applied_after_publish += frep.applied;
        R.refused_after_publish += frep.refused_unpublished;
        CHECK(frep.applied == 1 && frep.fused[0].skill == MS_COMPILED && frep.fused[0].steps == 4 &&
              frep.refused_unpublished == 3, "goal %u: one fusion reported, three unpublished refused", goal);
        CHECK(memcmp(go.digest, gf.digest, 32) != 0 && gf.n_nodes == go.n_nodes - 3 && rep.n_missing == 0,
              "the fused graph is a different, smaller graph with nothing missing");
        R.fused_graphs++;
        R.fused_nodes_before += go.n_nodes;
        R.fused_nodes_after += gf.n_nodes;
        int m = meta_node(&gf, MS_COMPILED);
        /* Authority of the fused node: READ on A, exactly. */
        uint32_t na = 0, exact = 1;
        for (uint32_t i = 0; i < gf.n_auth; i++)
            if (gf.auth[i].node == (uint32_t)m) {
                na++;
                exact &= gf.auth[i].resource == RES_A && gf.auth[i].rights == RX_RIGHT_READ;
            }
        CHECK(m >= 0 && na == 1 && exact, "fused node needs READ on A and nothing else");
        R.n3_meta_auth_exact += m >= 0 && na == 1 && exact;
        static AgLowered lo, lf;
        CHECK(lower(&o, &lo, &go, &g_skills) == 0 && lower(&f, &lf, &gf, &g_pub) == 0, "lower");
        AgFusionSite site;
        CHECK(rx_fusion_find(&go, &o.w, MC.reference.signature, &site, 1) == 1, "site");
        AgResult ro, rf;
        for (uint64_t r = 1; r <= 24; r++) {
            uint64_t a = a_value(r + 40);
            set_inputs(&o, a, r);
            set_inputs(&f, a, r);
            run(&o, &lo, r, &ro, NULL);
            CHECK(rx_graph_reference(&gf, &f.w, &g_pub, &f.caps, r, &g_ref) == 0, "reference");
            int64_t ext = run(&f, &lf, r, &rf, NULL);
            compare(&gf, &rf, &g_ref, "published");
            ancestry(&f, &lf, ext, "published");
            R.cross_runs++;
            int div = ro.status[site.exit] != rf.status[m] || ro.value[site.exit] != rf.value[m] ||
                      ro.outcome != rf.outcome;
            for (uint32_t k = 0; k < RX_MAX_FIELDS; k++) div |= fld(&o, o.out, k) != fld(&f, f.out, k);
            for (uint32_t k = 0; k < 4; k++) div |= fld(&o, o.effect, k) != fld(&f, f.effect, k);
            R.steps_checked++;
            if (rx_fusion_site_steps(&go, &site, &MC.reference, &ro) != rf.steps[m]) {
                R.steps_mismatch++;
                div = 1;
            }
            R.cross_mismatch += (uint32_t)div;
            CHECK(!div, "goal %u run %llu: fused differs from original", goal, (unsigned long long)r);
        }
        env_stop(&o);
        env_stop(&f);
    }
    /* Procedures without the workflow are left as they were. */
    CHECK(env_start(&o, 1) == 0, "env");
    for (uint32_t goal = GOAL_UNSTABLE; goal <= N_GOALS; goal++) {
        static AgGraph go, gf;
        AgReport rep;
        AgFusionReport frep;
        CHECK(compile(&o, goal, &go) == AG_OK_READY &&
              compile_fused(&o, goal, &lib, &g_pub, &gf, &rep, &frep) == AG_OK_READY, "compile");
        CHECK(frep.applied == 0 && memcmp(go.digest, gf.digest, 32) == 0, "goal %u untouched", goal);
    }
    env_stop(&o);

    /* Deterministic across worker counts. */
    uint8_t first[32];
    int have = 0;
    for (uint32_t wc = 1; wc <= 8; wc *= 2) {
        CHECK(env_start(&f, wc) == 0, "env");
        static AgGraph gf;
        AgReport rep;
        AgFusionReport frep;
        CHECK(compile_fused(&f, GOAL_EFFECT, &lib, &g_pub, &gf, &rep, &frep) == AG_OK_READY, "compile");
        static AgLowered lf;
        CHECK(lower(&f, &lf, &gf, &g_pub) == 0, "lower");
        AgResult rf;
        for (uint64_t r = 1; r <= 4; r++) {
            set_inputs(&f, a_value(r), r);
            run(&f, &lf, r, &rf, NULL);
        }
        uint8_t d[32];
        semantic_digest(&gf, &rf, &f, d);
        R.det_runs++;
        if (!have) { memcpy(first, d, 32); have = 1; }
        else if (memcmp(first, d, 32) != 0) R.det_mismatch++;
        env_stop(&f);
    }
    CHECK(R.det_mismatch == 0, "fused result identical at 1, 2, 4 and 8 workers");
}

/* ---- authority removed (N3) ---- */

static void t_revoked(void) {
    printf("[*] authority removed: original and fused both block, and record it\n");
    if (MC.state != AG_MS_PUBLISHED) return;
    AgFusionLibrary lib = { 1, { &MC } };
    static Env o, f;
    CHECK(env_start(&o, 4) == 0 && env_start(&f, 4) == 0, "envs");
    static AgGraph go, gf;
    AgReport rep;
    AgFusionReport frep;
    CHECK(compile(&o, GOAL_EFFECT, &go) == AG_OK_READY, "compile");
    CHECK(compile_fused(&f, GOAL_EFFECT, &lib, &g_pub, &gf, &rep, &frep) == AG_OK_READY && frep.applied == 1,
          "compile fused");
    static AgLowered lo, lf;
    CHECK(lower(&o, &lo, &go, &g_skills) == 0 && lower(&f, &lf, &gf, &g_pub) == 0, "lower");
    AienosCapRef office;
    aienos_cap_office(o.admin, &office);
    CHECK(aienos_cap_revoke(o.admin, office, (AienosCapRef){ o.read_A.cap_id, o.read_A.generation }) == 0, "revoke");
    aienos_cap_office(f.admin, &office);
    CHECK(aienos_cap_revoke(f.admin, office, (AienosCapRef){ f.read_A.cap_id, f.read_A.generation }) == 0, "revoke");
    AgResult ro, rf;
    run(&o, &lo, 1, &ro, NULL);
    run(&f, &lf, 1, &rf, NULL);
    int rd = -1;
    for (uint32_t n = 0; n < go.n_nodes; n++)
        if (go.nodes[n].kind == AG_WORLD_READ && go.nodes[n].obj.id == o.A.id) rd = (int)n;
    int m = meta_node(&gf, MS_COMPILED);
    R.n3_blocked_original = rd >= 0 ? count_crumbs(&o, lo.reaction[rd], RX_CRUMB_BLOCKED_AUTHORITY) : 0;
    R.n3_blocked_fused = m >= 0 ? count_crumbs(&f, lf.reaction[m], RX_CRUMB_BLOCKED_AUTHORITY) : 0;
    CHECK(R.n3_blocked_original >= 1 && R.n3_blocked_fused >= 1, "both blocked for authority (%u, %u)",
          R.n3_blocked_original, R.n3_blocked_fused);
    CHECK(ro.outcome == AG_RUN_INCOMPLETE && rf.outcome == AG_RUN_INCOMPLETE &&
          fld(&o, o.effect, 0) == 0 && fld(&f, f.effect, 0) == 0, "no effect on either side");
    env_stop(&o);
    env_stop(&f);

    /* Compiling for a principal without READ on A reports it missing on
     * the fused node, as it would on the read. */
    CHECK(env_start(&f, 1) == 0, "env");
    for (uint32_t i = 0; i < f.caps.n; i++)
        if (f.caps.cap[i].resource == RES_A) f.caps.cap[i].resource = RES_A + 0x1000;
    CHECK(compile_fused(&f, GOAL_EFFECT, &lib, &g_pub, &gf, &rep, &frep) == AG_OK_READY && frep.applied == 1,
          "compile fused without READ on A");
    m = meta_node(&gf, MS_COMPILED);
    int missing = 0;
    for (uint32_t i = 0; i < rep.n_missing; i++)
        missing |= rep.missing[i].node == (uint32_t)m && rep.missing[i].resource == RES_A;
    CHECK(missing && rep.n_missing == 1, "missing authority reported on the fused node");
    env_stop(&f);
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
    hex(d, out);
}

static void measure_json(FILE *fp, const char *name, const AgFusionMeasure *m, int last) {
    fprintf(fp,
            "    \"%s\": {\"runs_per_leg\": %u, \"reactions_computing_per_run_before\": %.2f, "
            "\"reactions_computing_per_run_after\": %.2f,\n"
            "      \"cognitive_operations_avoided_per_run\": %.2f, \"crumbs_per_run_before\": %.2f, "
            "\"crumbs_per_run_after\": %.2f,\n"
            "      \"median_run_ns_before\": %llu, \"median_run_ns_after\": %llu, "
            "\"cpu_ns_per_run_before\": %llu, \"cpu_ns_per_run_after\": %llu,\n"
            "      \"reactions_registered_before\": %u, \"reactions_registered_after\": %u, "
            "\"world_objects_before\": %u, \"world_objects_after\": %u,\n"
            "      \"success_before\": %u, \"failure_before\": %u, \"success_after\": %u, \"failure_after\": %u",
            name, m->runs, m->commits_before, m->commits_after, m->commits_before - m->commits_after,
            m->crumbs_before, m->crumbs_after, (unsigned long long)m->median_ns_before,
            (unsigned long long)m->median_ns_after, (unsigned long long)m->cpu_ns_before,
            (unsigned long long)m->cpu_ns_after, m->reactions_before, m->reactions_after,
            m->objects_before, m->objects_after, m->ok_before, m->failed_before, m->ok_after,
            m->failed_after);
    if (m->energy_available) {
        double dp = fabs(m->energy_uj_before - m->energy_uj_after), dc = fabs(cpu_mean(0) - cpu_mean(1));
        fprintf(fp,
                ",\n      \"energy\": {\"source\": \"aien_spbm accumulators (whole package / whole performance-core cluster, "
                "not per process); idle power around each leg subtracted; two rounds\",\n"
                "        \"package_uj_per_run_before\": %.2f, \"package_uj_per_run_after\": %.2f, "
                "\"package_round_spread_uj\": %.2f, \"package_difference_resolved\": %s,\n"
                "        \"performance_cores_uj_per_run_before\": %.2f, \"performance_cores_uj_per_run_after\": %.2f, "
                "\"performance_cores_round_spread_uj\": %.2f, \"performance_cores_difference_resolved\": %s}",
                m->energy_uj_before, m->energy_uj_after, m->energy_noise_uj,
                dp > 2 * m->energy_noise_uj ? "true" : "false", cpu_mean(0), cpu_mean(1), cpu_noise(),
                dc > 2 * cpu_noise() ? "true" : "false");
    }
    else
        fprintf(fp, ",\n      \"energy\": \"unavailable\"");
    fprintf(fp, "}%s\n", last ? "" : ",");
}

static void write_receipt(void) {
    char path[512];
    if (omega_evidence_path("WORKFLOW_FUSION/rx_workflow_fusion_receipt.json", path, sizeof path) != 0) return;
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
    int pass = g_fail == 0 && R.bypass == 0 && R.evidence_missing == 0 && R.ancestry_mismatch == 0 &&
               R.eq_mismatch == 0 && R.cross_mismatch == 0 && R.steps_mismatch == 0 && R.det_mismatch == 0 &&
               R.tamper_caught == R.tamper_cases && MC.state == AG_MS_PUBLISHED &&
               R.parallel_state == AG_MS_SLOWER && R.poisoned_state == AG_MS_QUARANTINED;
    static const char *kinds[4] = { "compiled", "hybrid", "learned", "hardware" };
    fprintf(fp,
            "{\n"
            "  \"schema\": \"OMEGA_WORKFLOW_FUSION_V1\",\n"
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
            "  \"scope\": \"host processor; native AIENOS authority library; resident reaction world; R9 generation store\",\n"
            "  \"observation\": {\"verified_runs\": %u, \"refused_runs\": %u, \"patterns\": %u, \"fragments\": %u,\n"
            "                  \"fragments_over_reaction_budget\": %u, \"patterns_dropped\": %u,\n"
            "                  \"workflow\": {\"signature\": \"%s\", \"steps\": %u, \"occurrences\": %u, \"graphs\": %u,\n"
            "                               \"ok\": %u, \"failed\": %u, \"repeat_inputs_stable\": %u, \"samples\": %u},\n"
            "                  \"candidates\": %u, \"refused_unstable\": %u, \"refused_failure_rate\": %u,\n"
            "                  \"refused_one_graph\": %u},\n"
            "  \"verification\": {\n",
            omega_evidence_run_id(), candidate ? "\"" : "", candidate ? candidate : "null",
            candidate ? "\"" : "", bound ? "true" : "false", commit,
            omega_evidence_tree_dirty() ? "true" : "false", aienos ? "\"" : "", aienos ? aienos : "null",
            aienos ? "\"" : "", g_checks, g_fail, digest, u.sysname, u.release, u.machine, R.cpus,
            R.obs_runs, R.obs_refused, R.obs_patterns, R.obs_fragments, R.obs_over_budget, R.obs_dropped,
            R.w_signature, R.w_steps, R.w_occ, R.w_graphs, R.w_ok, R.w_failed, R.w_stable, R.w_samples,
            R.n_candidates, R.judged_unstable, R.judged_failure_rate, R.judged_few_graphs);
    for (int i = 0; i < 4; i++)
        fprintf(fp,
                "    \"%s\": {\"state\": \"%s\", \"reason\": \"%s\", \"vectors\": %u, \"observed\": %u, "
                "\"generated\": %u, \"port_status_combinations\": %u, \"learned_hits\": %u},\n",
                kinds[i], rx_fusion_state_name(R.ver[i].state), rx_fusion_reason_name(R.ver[i].reason),
                R.ver[i].vectors, R.ver[i].observed, R.ver[i].generated, R.ver[i].combos, R.ver[i].hits);
    fprintf(fp,
            "    \"tampering\": {\"cases\": %u, \"caught\": %u, \"detail\": [%s]},\n"
            "    \"coherent_rewrite_sites_in_real_workflow\": %u\n"
            "  },\n"
            "  \"never_silent\": {\"unpublished_states_refused\": %u, \"graph_unchanged\": %u,\n"
            "                   \"refused_in_library_after_publication\": %u},\n"
            "  \"measurement\": {\n",
            R.tamper_cases, R.tamper_caught, R.tamper_names, R.coherent_rewrite_sites, R.unpublished_refusals,
            R.unpublished_digest_equal, R.refused_after_publish);
    measure_json(fp, "compiled", &R.m_compiled, 0);
    measure_json(fp, "hybrid", &R.m_hybrid, 0);
    measure_json(fp, "parallel_fragment", &R.m_parallel, 1);
    fprintf(fp,
            "  },\n"
            "  \"promotion\": {\"parallel_fragment\": \"%s\", \"hybrid\": \"%s\",\n"
            "                \"canary_runs\": %u, \"canary_divergences\": %u,\n"
            "                \"poisoned_hybrid_verified\": %s, \"poisoned_hybrid\": \"%s\", \"poisoned_canary_runs_until_quarantine\": %u,\n"
            "                \"poisoned_promotion_refused\": %s,\n"
            "                \"refused_without_right\": %s, \"refused_for_proposer\": %s,\n"
            "                \"publication_refused_after_change\": %s, \"generation\": %llu, \"compiled\": \"%s\"},\n"
            "  \"use\": {\"graphs_fused\": %u, \"nodes_before\": %u, \"nodes_after\": %u,\n"
            "          \"runs_compared_with_reference\": %u, \"node_results_compared\": %u, \"reference_mismatches\": %u,\n"
            "          \"runs_compared_with_original\": %u, \"original_mismatches\": %u,\n"
            "          \"step_evidence_checked\": %u, \"step_evidence_mismatches\": %u,\n"
            "          \"evidence_checked\": %u, \"evidence_missing\": %u,\n"
            "          \"ancestry_nodes\": %u, \"ancestry_mismatches\": %u,\n"
            "          \"determinism_worker_counts\": [1, 2, 4, 8], \"determinism_mismatches\": %u,\n"
            "          \"bypasses\": %u},\n"
            "  \"authority_removed\": {\"original_blocked_crumbs\": %u, \"fused_blocked_crumbs\": %u,\n"
            "                        \"fused_authority_exactly_steps\": %u},\n"
            "  \"gates\": {\n"
            "    \"OMEGA_WORKFLOW_FUSION_PASS\": \"%s\",\n"
            "    \"not_claimed\": [\"a hardware realization (the kind is in the contract; none is built)\", "
            "\"a neural learned module (the learned realization is a table of verified observations)\", "
            "\"machine code (the compiled realization is a typed step program run in one reaction)\", "
            "\"fusing effect boundaries, branches or joins\", "
            "\"observation as resident reactions (observe and propose are host calls)\", "
            "\"energy saved per run beyond the meter's noise\", "
            "\"AIENOS kernel (the authority runs as a host library)\"]\n"
            "  }\n"
            "}\n",
            rx_fusion_state_name(R.parallel_state), rx_fusion_state_name(R.hybrid_state), R.canary_runs,
            R.canary_divergences, R.poisoned_verified ? "true" : "false", rx_fusion_state_name(R.poisoned_state),
            R.poisoned_canary_runs, R.poisoned_promote_refused ? "true" : "false", R.promote_refused_no_right ? "true" : "false",
            R.promote_refused_same_subject ? "true" : "false", R.publish_refused_changed ? "true" : "false",
            (unsigned long long)R.generation, rx_fusion_state_name(MC.state), R.fused_graphs,
            R.fused_nodes_before, R.fused_nodes_after, R.eq_runs, R.eq_nodes, R.eq_mismatch, R.cross_runs,
            R.cross_mismatch, R.steps_checked, R.steps_mismatch, R.evidence_checked, R.evidence_missing,
            R.ancestry_nodes, R.ancestry_mismatch, R.det_mismatch, R.bypass, R.n3_blocked_original,
            R.n3_blocked_fused, R.n3_meta_auth_exact, pass ? "PASS" : "FAIL");
    fclose(fp);
    printf("receipt: %s\n", path);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    place();
    skills_init();
    build_templates();
    t_observe();
    t_verify();
    t_measure();
    t_canary();
    t_promote();
    t_use();
    t_revoked();
    printf("    compared %u runs (%u node results) with the reference, %u with the original "
           "(%u mismatches); step evidence %u checked, %u mismatches; ancestry %u nodes, %u mismatches; "
           "bypasses %u\n", R.eq_runs, R.eq_nodes, R.cross_runs, R.cross_mismatch, R.steps_checked,
           R.steps_mismatch, R.ancestry_nodes, R.ancestry_mismatch, R.bypass);
    printf("checks %d failures %d\n", g_checks, g_fail);
    write_receipt();
    return g_fail ? 1 : 0;
}
