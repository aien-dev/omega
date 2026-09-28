/*
 * OMEGA_EFFICIENCY_GOLDEN_PATH -- control leg (spec/efficiency-golden-path.md).
 *
 * This file runs the pre-registered regression-triage encounter sequence
 * with every efficiency mechanism off. It is the baseline the optimized leg
 * is compared against once the ten prerequisite gates merge. It claims no
 * gate and writes no receipt; it writes a baseline record under build/.
 *
 * Control, per encounter (spec §3):
 *   - cognition reads the full available state: every Cortex record and the
 *     whole capability catalog (no projection, no capability query);
 *   - each hypothesis branch recomputes the shared prefix itself (no branch
 *     state reuse) and nothing derived is kept between encounters (no
 *     incremental reuse, no plan cache: the plan is built and compiled anew);
 *   - the graph optimizer is off and the graph runs on one worker.
 *
 * Every run is compared with the sequential reference (status, value and
 * evidence of every node, effect chain), and the engine audit must show no
 * authority bypass. Those are the invariants of spec §5 as far as the
 * control leg can state them alone.
 */
#include "runtime/aienos_cap.h"
#include "runtime/rx_graph.h"
#include "runtime/rx_world.h"
#include "omega_types.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

enum { SUBJ_EXTERNAL = 100, ISSUER = 3, SUBJ_AIEN = 71 };

#define RES_CELL    0xB000001ull
#define RES_RUN     0xB000002ull
#define RES_TEL     0xB000010ull   /* telemetry of subsystem s: RES_TEL + s */
#define RES_CORTEX  0xB000020ull
#define RES_PLACE   0xB000030ull
#define RES_EFFECT  0xB000040ull

#define N_SUBSYS        4u
#define N_CANDIDATES    3u
#define CORTEX_RECORDS  4096u
#define CATALOG_SIZE    10000u
#define HISTORY_SAMPLES 1024u
#define PHYS_US         400u      /* real busy time of one candidate measurement */
#define GOAL_TRIAGE     1u

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

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t mix(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull;
    return x ^ (x >> 33);
}

/* ---- the encounter sequence (spec §2) ---- */

typedef struct { uint32_t phase, subsys, op; const char *note; } Encounter;

static const Encounter SEQ[] = {
    { 1, 0, 1, "E1 novel" },
    { 2, 0, 1, "E2 repeat" },          { 2, 0, 1, "E3 repeat" },
    { 3, 1, 1, "E4 family" },          { 3, 2, 1, "E5 family" },
    { 3, 1, 2, "E6 family" },          { 3, 3, 2, "E7 family" },
    { 3, 2, 2, "E8 family" },          { 3, 3, 1, "E9 family" },
    { 4, 1, 1, "E10 stable" },         { 4, 2, 1, "E11 stable" },
    { 4, 3, 1, "E12 stable" },         { 4, 1, 2, "E13 stable" },
    { 4, 2, 2, "E14 stable" },         { 4, 3, 2, "E15 stable" },
    { 4, 1, 1, "E16 stable" },
};
#define N_ENC (sizeof SEQ / sizeof SEQ[0])

/* ---- the full state the control's cognition reads ---- */

typedef struct { uint32_t subsys, op, kind; uint64_t value; } CortexRec;   /* kind 1 verified realization */
typedef struct { uint32_t op, core_class, realization; uint64_t cost_ns; } CatalogEntry;

static CortexRec g_cortex[CORTEX_RECORDS];
static CatalogEntry g_catalog[CATALOG_SIZE];
static uint64_t g_history[N_SUBSYS][HISTORY_SAMPLES];

static void state_init(void) {
    for (uint32_t i = 0; i < CORTEX_RECORDS; i++) {
        uint64_t h = mix(i + 1);
        g_cortex[i] = (CortexRec){ (uint32_t)(16 + h % 48), (uint32_t)((h >> 8) % 16), (uint32_t)((h >> 16) % 4),
                                   (h >> 24) % 100000 };
    }
    /* the records the task needs: last verified realization per (s, op) */
    for (uint32_t s = 0; s < N_SUBSYS; s++)
        for (uint32_t op = 1; op <= 2; op++)
            g_cortex[(s * 97 + op * 31) % CORTEX_RECORDS] = (CortexRec){ s, op, 1, 1000 + s * 10 + op };
    for (uint32_t i = 0; i < CATALOG_SIZE; i++) {
        uint64_t h = mix(0xCA7 + i);
        g_catalog[i] = (CatalogEntry){ (uint32_t)(3 + h % 200), (uint32_t)((h >> 8) % 8), i,
                                       200 + (h >> 16) % 5000 };
    }
    for (uint32_t op = 1; op <= 2; op++)
        for (uint32_t k = 0; k < N_CANDIDATES; k++)
            g_catalog[(op * 1009 + k * 17) % CATALOG_SIZE] =
                (CatalogEntry){ op, k, (op * 1009 + k * 17) % CATALOG_SIZE, 300 + 100 * k + op };
    for (uint32_t s = 0; s < N_SUBSYS; s++)
        for (uint32_t i = 0; i < HISTORY_SAMPLES; i++)
            g_history[s][i] = 800 + (mix(s * 7919 + i) % 200);
}

/* What cognition concludes for one encounter, and what it cost. */
typedef struct {
    uint64_t last_verified;         /* Cortex: last verified realization of (s, op) */
    uint32_t n_candidates;
    uint32_t candidate[N_CANDIDATES];
    uint64_t hyp[3];                /* core class changed, shape drifted, realization regressed */
    uint64_t ops;                   /* state items examined */
    uint64_t bytes_in;              /* bytes that crossed into cognition */
    uint32_t activations;           /* general-cognition activations */
    uint64_t ns;
} Cognition;

/* Shared prefix every hypothesis needs: the subsystem's baseline. In the
 * control each branch derives it again from the full history. */
static uint64_t prefix_baseline(uint32_t s, uint64_t *ops) {
    uint64_t sum = 0;
    for (uint32_t i = 0; i < HISTORY_SAMPLES; i++) sum += g_history[s][i];
    *ops += HISTORY_SAMPLES;
    return sum / HISTORY_SAMPLES;
}

static void cognition_control(const Encounter *e, uint64_t observed, Cognition *c) {
    memset(c, 0, sizeof *c);
    uint64_t t0 = now_ns();
    /* memory: scan every Cortex record */
    for (uint32_t i = 0; i < CORTEX_RECORDS; i++)
        if (g_cortex[i].subsys == e->subsys && g_cortex[i].op == e->op && g_cortex[i].kind == 1)
            c->last_verified = g_cortex[i].value;
    c->ops += CORTEX_RECORDS;
    c->bytes_in += sizeof g_cortex;
    c->activations++;
    /* capability discovery: read the whole catalog */
    for (uint32_t i = 0; i < CATALOG_SIZE; i++)
        if (g_catalog[i].op == e->op && c->n_candidates < N_CANDIDATES)
            c->candidate[c->n_candidates++] = g_catalog[i].realization;
    c->ops += CATALOG_SIZE;
    c->bytes_in += sizeof g_catalog;
    c->activations++;
    /* J-Space: three hypothesis branches, each recomputing the prefix */
    for (uint32_t h = 0; h < 3; h++) {
        uint64_t base = prefix_baseline(e->subsys, &c->ops);
        c->bytes_in += sizeof g_history[0];
        c->hyp[h] = observed > base ? (observed - base) * (h + 1) : 0;
        c->activations++;
    }
    c->activations++;   /* planning: the triage plan is built from scratch */
    c->ns = now_ns() - t0;
}

/* ---- skills ---- */

static uint64_t sk_measure(const uint64_t *in, uint32_t n, uint32_t attempt, int *failed) {
    (void)attempt; (void)n;
    *failed = 0;
    return 100 + in[0] % 900;       /* measured ns of the candidate */
}

static uint64_t sk_decide(const uint64_t *in, uint32_t n, uint32_t attempt, int *failed) {
    (void)attempt;
    *failed = 0;
    uint64_t best = UINT64_MAX, arg = 0;
    for (uint32_t i = 0; i < n && i < N_CANDIDATES; i++)
        if (in[i] < best) { best = in[i]; arg = i; }
    return (arg << 32) | best;      /* placement: candidate index, its cost */
}

static AgSkillTable g_skills;

/* ---- plan: the triage graph (built anew every encounter in the control) ----
 * Parameters: 1 telemetry(s), 2 Cortex, 3 placement, 4 effect. */

static int N(AgGraph *g, AgKind k, AgType t) { return rx_graph_node(g, k, t); }

static void build_triage(AgGraph *g, const Cognition *c, uint32_t op) {
    rx_graph_init(g, 0);
    int obs = N(g, AG_WORLD_READ, AG_T_U64);
    g->nodes[obs].param = 1; g->nodes[obs].field = 0;
    int rc = N(g, AG_RECALL, AG_T_U64);
    g->nodes[rc].param = 2; g->nodes[rc].imm = op;
    int hyp = N(g, AG_CONST, AG_T_U64);
    g->nodes[hyp].imm = c->hyp[0] ^ (c->hyp[1] << 1) ^ (c->hyp[2] << 2);
    int meas[N_CANDIDATES];
    for (uint32_t k = 0; k < c->n_candidates; k++) {
        /* the repeated subworkflow: run candidate -> measure -> verify */
        int cand = N(g, AG_CONST, AG_T_U64);
        g->nodes[cand].imm = c->candidate[k];
        int ph = N(g, AG_PHYSICAL, AG_T_U64);
        g->nodes[ph].imm = k + 1;
        g->nodes[ph].cost_us = PHYS_US;
        rx_graph_data(g, (uint32_t)cand, (uint32_t)ph, 0, AG_EDGE_DATA);
        rx_graph_data(g, (uint32_t)rc, (uint32_t)ph, 1, AG_EDGE_DATA);
        rx_graph_data(g, (uint32_t)obs, (uint32_t)ph, 2, AG_EDGE_DATA);
        int m = N(g, AG_SKILL, AG_T_U64);
        g->nodes[m].op = 1;
        rx_graph_data(g, (uint32_t)ph, (uint32_t)m, 0, AG_EDGE_DATA);
        int v = N(g, AG_VERIFY, AG_T_VERDICT);
        g->nodes[v].imm = 100; g->nodes[v].imm2 = 999;
        rx_graph_data(g, (uint32_t)m, (uint32_t)v, 0, AG_EDGE_DATA);
        rx_graph_evidence(g, (uint32_t)v);
        meas[k] = m;
    }
    int d = N(g, AG_SKILL, AG_T_U64);
    g->nodes[d].op = 2;
    for (uint32_t k = 0; k < c->n_candidates; k++)
        rx_graph_data(g, (uint32_t)meas[k], (uint32_t)d, k, AG_EDGE_DATA);
    rx_graph_data(g, (uint32_t)hyp, (uint32_t)d, c->n_candidates, AG_EDGE_DATA);
    int cost = N(g, AG_PURE, AG_T_U64);
    g->nodes[cost].op = OP_AND; g->nodes[cost].imm = 0xffffffffull;
    rx_graph_data(g, (uint32_t)d, (uint32_t)cost, 0, AG_EDGE_DATA);
    int vf = N(g, AG_VERIFY, AG_T_VERDICT);
    g->nodes[vf].imm = 100; g->nodes[vf].imm2 = 999;
    rx_graph_data(g, (uint32_t)cost, (uint32_t)vf, 0, AG_EDGE_DATA);
    int p = N(g, AG_EFFECT_PROPOSE, AG_T_PROPOSAL);
    rx_graph_data(g, (uint32_t)d, (uint32_t)p, 0, AG_EDGE_DATA);
    int f = N(g, AG_EFFECT_PERFORM, AG_T_RECEIPT);
    g->nodes[f].param = 4;
    rx_graph_data(g, (uint32_t)p, (uint32_t)f, 0, AG_EDGE_DATA);
    rx_graph_data(g, (uint32_t)vf, (uint32_t)f, 1, AG_EDGE_DATA);
    int pub = N(g, AG_WORLD_PUBLISH, AG_T_RECEIPT);
    g->nodes[pub].param = 3; g->nodes[pub].field = 0;
    rx_graph_data(g, (uint32_t)d, (uint32_t)pub, 0, AG_EDGE_DATA);
    rx_graph_order(g, (uint32_t)f, (uint32_t)pub);
    rx_graph_success(g, (uint32_t)f, AG_OK);
    rx_graph_success(g, (uint32_t)pub, AG_OK);
    rx_graph_failure(g, (uint32_t)vf, AG_FAILED);
}

/* ---- environment ---- */

typedef struct {
    AienosCapAdmin *admin;
    AienosCapView *view;
    RxWorld w;
    RxObjRef tel[N_SUBSYS], cortex, place, effect;
    RxCapRef ext_run, ext_tel[N_SUBSYS], cell_cap, run_cap;
    AgCapTable caps;
    AgGraph tmpl;
    AgLibrary lib;
} Env;

static RxCapRef mint(Env *e, uint32_t subject, uint64_t resource, uint32_t rights) {
    AienosCapRef office;
    aienos_cap_office(e->admin, &office);
    AienosCapMint m = { ISSUER, subject, resource, rights, 0, { UINT32_MAX, 0 }, office };
    AienosCapRef r = { UINT32_MAX, 0 };
    if (aienos_cap_mint(e->admin, &m, &r) != 0) r = (AienosCapRef){ UINT32_MAX, 0 };
    return (RxCapRef){ r.cap_id, r.generation };
}

static void hold(Env *e, uint64_t res, uint32_t rights) {
    e->caps.cap[e->caps.n].ref = mint(e, SUBJ_AIEN, res, rights);
    e->caps.cap[e->caps.n].resource = res;
    e->caps.cap[e->caps.n].rights = rights;
    e->caps.n++;
}

/* State that outlives one encounter's world (spec §3: the control builds
 * everything else anew). The world holds RX_MAX_OBJECTS objects, so each
 * encounter gets its own world seeded from this. */
static struct { uint64_t tel[N_SUBSYS]; uint64_t place[RX_MAX_FIELDS]; uint64_t effect[RX_MAX_FIELDS]; } g_carry;

static void carry_out(Env *e) {
    RxObject o;
    for (uint32_t s = 0; s < N_SUBSYS; s++)
        if (rx_world_read(&e->w, e->tel[s], &o) == RX_OK) g_carry.tel[s] = o.field[0];
    if (rx_world_read(&e->w, e->place, &o) == RX_OK) memcpy(g_carry.place, o.field, sizeof g_carry.place);
    if (rx_world_read(&e->w, e->effect, &o) == RX_OK) memcpy(g_carry.effect, o.field, sizeof g_carry.effect);
}

static int env_start(Env *e, uint32_t workers) {
    memset(e, 0, sizeof *e);
    if (aienos_cap_start(&e->admin, &e->view) != 0) return -1;
    if (rx_world_init_native(&e->w, e->view, workers, 1u << 18) != RX_OK) return -1;
    e->w.external_subject = SUBJ_EXTERNAL;
    const uint32_t R = RX_RIGHT_READ, W = RX_RIGHT_WRITE, RW = R | W;
    for (uint32_t s = 0; s < N_SUBSYS; s++) {
        uint64_t t[RX_MAX_FIELDS] = { g_carry.tel[s] };
        if (rx_world_create(&e->w, 0xB01, RX_PERSIST_RESIDENT, RES_TEL + s, t, &e->tel[s]) != RX_OK)
            return -1;
        e->ext_tel[s] = mint(e, SUBJ_EXTERNAL, RES_TEL + s, W);
        hold(e, RES_TEL + s, R);
    }
    /* the World's Cortex view: last verified realization per op (key = op) */
    uint64_t m[RX_MAX_FIELDS] = { 1, 1001, 2, 1002, 0, 0, 0, 0 };
    if (rx_world_create(&e->w, AG_OT_MEMORY, RX_PERSIST_RESIDENT, RES_CORTEX, m, &e->cortex) != RX_OK ||
        rx_world_create(&e->w, 0xB03, RX_PERSIST_RESIDENT, RES_PLACE, g_carry.place, &e->place) != RX_OK ||
        rx_world_create(&e->w, AG_OT_EFFECT, RX_PERSIST_RESIDENT, RES_EFFECT, g_carry.effect, &e->effect) != RX_OK)
        return -1;
    e->ext_run = mint(e, SUBJ_EXTERNAL, RES_RUN, W);
    e->caps.subject = SUBJ_AIEN;
    e->cell_cap = mint(e, SUBJ_AIEN, RES_CELL, RW);
    e->run_cap = mint(e, SUBJ_AIEN, RES_RUN, R);
    hold(e, RES_CORTEX, R);
    hold(e, RES_PLACE, RW);
    hold(e, RES_EFFECT, RW | RX_RIGHT_EFFECT);
    return 0;
}

static void env_stop(Env *e) {
    rx_world_wait_quiescent(&e->w, 30000);
    carry_out(e);
    rx_world_destroy(&e->w);
    aienos_cap_stop(e->admin, e->view);
}

/* Commits to the effect or placement must come from the lowered boundary
 * reactions of the encounter's own graph. Counts anything else. */
static uint32_t bypasses(Env *e, const AgLowered *L, uint64_t from) {
    uint32_t bad = 0;
    for (uint64_t id = from + 1; id <= e->w.n_crumbs; id++) {
        const RxCrumb *k = rx_world_crumb(&e->w, id);
        if (k->kind != RX_CRUMB_COMMIT) continue;
        for (uint32_t o = 0; o < k->n_outputs; o++) {
            uint32_t oid = k->outputs[o].obj.id;
            if (oid != e->effect.id && oid != e->place.id) continue;
            int legit = 0;
            for (uint32_t n = 0; n < L->g->n_nodes; n++)
                if (L->reaction[n] == k->reaction &&
                    (L->g->nodes[n].kind == AG_EFFECT_PERFORM ||
                     L->g->nodes[n].kind == AG_WORLD_PUBLISH) &&
                    L->g->nodes[n].obj.id == oid)
                    legit = 1;
            if (!legit) bad++;
        }
    }
    return bad;
}

/* ---- one encounter, control leg ---- */

typedef struct {
    uint64_t time_ns, cog_ns, run_ns, cog_ops, bytes_in, phys_us, crumbs, mem_bytes;
    uint32_t activations, nodes, reactions, success, verify_fail;
    uint64_t critical_us, total_us;
    uint8_t digest[32];
} Row;

static Row g_rows[N_ENC];

static void semantic_digest(const AgGraph *g, const AgResult *r, uint64_t chain, uint8_t out[32]) {
    sha256_ctx s;
    sha256_init(&s);
    for (uint32_t i = 0; i < g->n_effects; i++) {
        uint16_t n = g->effects[i];
        sha256_update(&s, &r->status[n], 1);
        sha256_update(&s, (const uint8_t *)&r->value[n], 8);
    }
    sha256_update(&s, (const uint8_t *)&chain, 8);
    sha256_final(&s, out);
}

static void encounter(Env *e, uint32_t idx) {
    const Encounter *en = &SEQ[idx];
    Row *row = &g_rows[idx];
    memset(row, 0, sizeof *row);
    uint64_t observed = 1000 + 37 * idx + 50 * en->subsys;
    uint64_t t0 = now_ns();
    RxMutation mu = { e->tel[en->subsys], 0, observed };
    CHECK(rx_world_publish_external(&e->w, e->ext_tel[en->subsys], &mu, 1) > 0, "publish telemetry");
    CHECK(rx_world_wait_quiescent(&e->w, 30000) == RX_OK, "settle telemetry");

    Cognition c;
    cognition_control(en, observed, &c);
    CHECK(c.n_candidates == N_CANDIDATES, "%s: candidates %u", en->note, c.n_candidates);
    CHECK(c.last_verified == 1000 + en->subsys * 10 + en->op, "%s: recall", en->note);

    build_triage(&e->tmpl, &c, en->op);
    e->lib.n = 1;
    e->lib.proc[0] = (AgProcedure){ GOAL_TRIAGE, &e->tmpl };
    AgGoal goal = { GOAL_TRIAGE, 4, { e->tel[en->subsys], e->cortex, e->place, e->effect } };
    AgConstraints k;
    memset(&k, 0, sizeof k);
    k.max_effects = UINT32_MAX;
    k.budget.slots = RX_MAX_REACTIONS;
    k.budget.memory_bytes = UINT64_MAX;
    k.budget.energy_budget = UINT64_MAX;
    k.budget.offered_locality = UINT32_MAX;
    k.budget.offered_accel = UINT32_MAX;
    k.budget.compute_mask = UINT32_MAX;
    static AgGraph g;
    AgReport rep;
    int rc = rx_graph_compile(&goal, &e->w, &e->caps, &k, &e->lib, 0 /* optimizer off */, &g, &rep);
    CHECK(rc == AG_OK_READY && rep.n_missing == 0, "%s: compile %d missing %u", en->note, rc, rep.n_missing);

    static AgLowered L;
    CHECK(rx_graph_lower(&L, &e->w, &g, &g_skills, &e->caps, RES_CELL, e->cell_cap, RES_RUN,
                         e->run_cap, NULL) == 0, "%s: lower", en->note);
    uint64_t crumbs0 = e->w.n_crumbs;
    uint64_t r0 = now_ns();
    uint64_t run_id = idx + 1;
    static AgReference ref;
    RxObject eff_before;
    rx_world_read(&e->w, e->effect, &eff_before);
    rx_graph_reference(&g, &e->w, &g_skills, &e->caps, run_id, &ref);
    CHECK(rx_graph_start(&L, e->ext_run, run_id) > 0, "%s: start", en->note);
    CHECK(rx_world_wait_quiescent(&e->w, 30000) == RX_OK, "%s: settle", en->note);
    row->run_ns = now_ns() - r0;
    AgResult res;
    rx_graph_collect(&L, run_id, &res);
    row->time_ns = now_ns() - t0;

    /* invariants the control states alone: lowered == reference, success */
    uint32_t mism = 0;
    for (uint32_t n = 0; n < g.n_nodes; n++) {
        if (!g.nodes[n].alive) continue;
        if (res.status[n] != ref.r.status[n] || res.value[n] != ref.r.value[n] ||
            res.evidence[n] != ref.r.evidence[n])
            mism++;
        if (g.nodes[n].kind == AG_VERIFY && res.status[n] == AG_FAILED) row->verify_fail++;
        if (g.nodes[n].kind == AG_PHYSICAL && res.status[n] == AG_OK) row->phys_us += g.nodes[n].cost_us;
    }
    CHECK(mism == 0, "%s: %u node(s) differ from the reference", en->note, mism);
    CHECK(res.outcome == AG_RUN_SUCCESS, "%s: outcome %d", en->note, res.outcome);
    RxObject eff;
    rx_world_read(&e->w, e->effect, &eff);
    CHECK(eff.field[2] == ref.effect_chain[0], "%s: effect chain", en->note);
    CHECK(eff.field[0] == eff_before.field[0] + 1, "%s: one effect", en->note);

    row->cog_ns = c.ns;
    row->cog_ops = c.ops;
    row->bytes_in = c.bytes_in;
    row->activations = c.activations;
    row->mem_bytes = sizeof g_cortex + sizeof g_catalog + 3 * sizeof g_history[0] +
                     (uint64_t)rep.nodes_after * sizeof(RxObject);
    row->nodes = rep.nodes_after;
    row->reactions = rep.reactions_after;
    row->crumbs = e->w.n_crumbs - crumbs0;
    uint32_t byp = bypasses(e, &L, crumbs0);
    CHECK(byp == 0, "%s: %u authority bypass(es)", en->note, byp);
    row->success = res.outcome == AG_RUN_SUCCESS;
    rx_graph_costs(&g, &row->critical_us, &row->total_us);
    semantic_digest(&g, &res, eff.field[2], row->digest);
}

int main(void) {
    state_init();
    memset(&g_skills, 0, sizeof g_skills);
    g_skills.n = 2;
    g_skills.skill[0] = (AgSkill){ 1, sk_measure, { 0x11 } };
    g_skills.skill[1] = (AgSkill){ 2, sk_decide, { 0x12 } };

    printf("[*] golden path, CONTROL leg (all efficiency mechanisms off, 1 worker)\n");
    for (uint32_t s = 0; s < N_SUBSYS; s++) g_carry.tel[s] = 900 + 50 * s;
    static Env e;
    uint64_t prev_count = 0, prev_chain = 0;
    for (uint32_t i = 0; i < N_ENC; i++) {
        if (env_start(&e, 1) != 0) { fprintf(stderr, "env_start failed\n"); return 1; }
        encounter(&e, i);
        env_stop(&e);
        /* the effect history is one chain across encounters */
        CHECK(g_carry.effect[0] == prev_count + 1, "%s: effect count carried", SEQ[i].note);
        CHECK(g_carry.effect[2] != prev_chain, "%s: effect chain advanced", SEQ[i].note);
        prev_count = g_carry.effect[0];
        prev_chain = g_carry.effect[2];
    }

    /* the control does not learn: cognition per result is flat across phases */
    uint64_t lo = UINT64_MAX, hi = 0;
    for (uint32_t i = 0; i < N_ENC; i++) {
        if (g_rows[i].cog_ops < lo) lo = g_rows[i].cog_ops;
        if (g_rows[i].cog_ops > hi) hi = g_rows[i].cog_ops;
    }
    CHECK(lo == hi, "control cognition should be flat (%llu..%llu)", (unsigned long long)lo,
          (unsigned long long)hi);

    printf("    %-11s %5s %9s %9s %8s %9s %6s %6s %6s %s\n", "encounter", "phase", "time_us",
           "cog_us", "cog_ops", "bytes_in", "activ", "nodes", "crumbs", "par(total/crit)");
    for (uint32_t i = 0; i < N_ENC; i++) {
        const Row *r = &g_rows[i];
        printf("    %-11s %5u %9llu %9llu %8llu %9llu %6u %6u %6llu %.2f\n", SEQ[i].note, SEQ[i].phase,
               (unsigned long long)(r->time_ns / 1000), (unsigned long long)(r->cog_ns / 1000),
               (unsigned long long)r->cog_ops, (unsigned long long)r->bytes_in, r->activations,
               r->nodes, (unsigned long long)r->crumbs,
               r->critical_us ? (double)r->total_us / (double)r->critical_us : 0.0);
    }

    mkdir("build", 0755);
    FILE *f = fopen("build/golden_path_control_baseline.json", "w");
    if (f) {
        fprintf(f, "{\n  \"kind\": \"golden_path_control_baseline\",\n  \"gate_claimed\": null,\n"
                   "  \"note\": \"control leg only; not a qualification receipt\",\n"
                   "  \"energy\": \"not measured in this leg\",\n  \"encounters\": [\n");
        for (uint32_t i = 0; i < N_ENC; i++) {
            const Row *r = &g_rows[i];
            char hex[65];
            for (int b = 0; b < 32; b++) sprintf(hex + 2 * b, "%02x", r->digest[b]);
            fprintf(f, "    {\"id\": \"%s\", \"phase\": %u, \"time_ns\": %llu, \"cognition_ns\": %llu, "
                       "\"cognition_ops\": %llu, \"bytes_into_cognition\": %llu, \"activations\": %u, "
                       "\"physical_us\": %llu, \"memory_bytes\": %llu, \"nodes\": %u, \"reactions\": %u, "
                       "\"crumbs\": %llu, \"critical_us\": %llu, \"total_work_us\": %llu, "
                       "\"success\": %u, \"verify_failures\": %u, \"semantic_digest\": \"%s\"}%s\n",
                    SEQ[i].note, SEQ[i].phase, (unsigned long long)r->time_ns,
                    (unsigned long long)r->cog_ns, (unsigned long long)r->cog_ops,
                    (unsigned long long)r->bytes_in, r->activations, (unsigned long long)r->phys_us,
                    (unsigned long long)r->mem_bytes, r->nodes, r->reactions,
                    (unsigned long long)r->crumbs, (unsigned long long)r->critical_us,
                    (unsigned long long)r->total_us, r->success, r->verify_fail, hex,
                    i + 1 < N_ENC ? "," : "");
        }
        fprintf(f, "  ],\n  \"checks\": %d,\n  \"failures\": %d\n}\n", g_checks, g_fail);
        fclose(f);
    }
    printf("checks %d failures %d\n", g_checks, g_fail);
    printf("baseline: build/golden_path_control_baseline.json\n");
    return g_fail ? 1 : 0;
}
