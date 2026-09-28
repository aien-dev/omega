/*
 * OMEGA_TYPED_RESULT_CONSTRAINTS -- cognition's structured answers are
 * checked against typed contracts; invalid ones are impossible to generate
 * (constrained path) or caught before publication (free path), and nothing
 * reaches the World except through the publish gate.
 *
 * The cognitive backend here is a synthetic one: it has an intended answer
 * (valid by construction) and makes mistakes at a fixed rate from a catalogue
 * per object kind, each mistake labelled with the constraint type it breaks,
 * or "valid but wrong" when no contract could see it. Both paths see the
 * same mistakes (same seed per request and attempt). The constrained path
 * decodes under the compiled per-field domains: when its preferred value is
 * outside the domain it takes its intended value if allowed, else the
 * nearest allowed one. That is the model of masked decoding this test uses.
 * It is not a neural model.
 *
 * Every candidate the sweep produces is also judged by an independent,
 * hand-written reference predicate per kind; the contract checker must
 * agree with it on every one.
 */
#include "runtime/aienos_cap.h"
#include "runtime/rx_aien.h"
#include "runtime/rx_contract.h"
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

enum { SUBJ_COG = 71, SUBJ_GATE = 72, SUBJ_OTHER = 73, ISSUER = 3 };

#define RES_CTX       0xB000001ull
#define RES_T         0xB000100ull   /* + i: mutation targets */
#define RES_EFFECT    0xB000200ull   /* + i: effect objects */
#define RES_DRAFT     0xB000300ull   /* + kind */
#define RES_PUB       0xB000400ull   /* + kind */
#define RES_STATUS    0xB000500ull   /* + kind */
#define RES_FORBID_LO 0xBF00000ull
#define RES_FORBID_HI 0xBFFFFFFull
#define RES_OFFICE    0xBF00001ull   /* a live object in the forbidden range */

#define HYP_SEQ   41u
#define GOAL_SEQ  7u
#define PRED_SEQ  19u
#define CLASS_X925 0xd85u
#define CLASS_A725 0xd87u
#define ACTIVE_GEN 12u
#define EPOCH      3u
#define LINEAGE    0x1111u

#define N_TARGETS 6u   /* 0..3 principal W; 4 principal R only; 5 forbidden, principal W */
#define N_EFFECTS 3u   /* 0 W|E allowed; 1 W only, allowed; 2 W|E not an effect resource */

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
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

typedef struct { uint64_t s; } Rng;
static uint64_t rnd(Rng *r) { r->s = mix(r->s); return r->s; }
static uint64_t rin(Rng *r, uint64_t lo, uint64_t hi) { return lo + rnd(r) % (hi - lo + 1); }

/* ---- receipt figures ---- */

enum { PATH_CONSTRAINED = 0, PATH_FREE = 1 };

typedef struct {
    uint64_t requests, candidates, invalid, repairs, retries, draws, checks, dead_ends;
    uint64_t published, rejected, unsat, semantic_fail, invalid_published, gate_rejected;
    uint64_t gen_ns, check_ns;
    uint64_t lat[4096];
    uint32_t n_lat;
} PathStats;

static struct {
    PathStats path[2][RC_KIND_COUNT];
    uint32_t diff_candidates, diff_mismatch;
    uint32_t caught_by_kind[RC_RULE_KIND_COUNT];
    uint32_t single_fault, single_fault_misclassified;
    uint32_t sound_samples, sound_enforced_violations;
    uint32_t reach_checked, reach_missed;
    uint32_t hostile_drafts, hostile_rejected, hostile_repaired, hostile_published_invalid;
    uint32_t bypass_attempts, bypass_blocked, bypasses;
    uint32_t stale_caught;
    uint64_t budget_free_draws, budget_constrained_draws;
    uint32_t budget_free_candidates;
    uint32_t json_roundtrips, json_identity_mismatch, json_malformed, json_malformed_rejected;
    uint32_t json_candidates, json_caught;
    uint32_t rule_kinds_defined;
    uint32_t enforced_rules, post_rules;
    char cpus[64];
} R;

/* ---- environment ---- */

static uint64_t sk_dummy(const uint64_t *in, uint32_t n, uint32_t attempt, int *failed) {
    (void)attempt;
    *failed = 0;
    return n ? in[0] : 0;
}

typedef struct {
    AienosCapAdmin *admin;
    AienosCapView *view;
    RxWorld w;
    RxObjRef hyp, goal, pred, place;
    RxObjRef target[N_TARGETS], effect[N_EFFECTS];
    RxObjRef draft[RC_KIND_COUNT], pub[RC_KIND_COUNT], status[RC_KIND_COUNT];
    RxCapRef cog_draft[RC_KIND_COUNT];
    RxCapRef gate_pub[RC_KIND_COUNT];
    AgCapTable caps;
    AgSkillTable skills;
    RcContract con[RC_KIND_COUNT];
    RcCompiled comp[RC_KIND_COUNT];
    RcContext ctx;
    RcGate gate[RC_KIND_COUNT];
    /* the test's own record of what was granted, for the reference */
    uint64_t held_w[16], held_we[16];
    uint32_t n_held_w, n_held_we;
    uint64_t ev_verify_pass[64], ev_gen_pass[64];
    uint32_t n_ev_verify_pass, n_ev_gen_pass;
    uint64_t ev_verify_fail;
} Env;

static Env E;

static RxCapRef mint(uint32_t subject, uint64_t resource, uint32_t rights) {
    AienosCapRef office;
    aienos_cap_office(E.admin, &office);
    AienosCapMint m = { ISSUER, subject, resource, rights, 0, { UINT32_MAX, 0 }, office };
    AienosCapRef r = { UINT32_MAX, 0 };
    if (aienos_cap_mint(E.admin, &m, &r) != 0) r = (AienosCapRef){ UINT32_MAX, 0 };
    return (RxCapRef){ r.cap_id, r.generation };
}

static void hold(uint64_t res, uint32_t rights) {
    RxCapRef ref = mint(SUBJ_COG, res, rights);
    E.caps.cap[E.caps.n].ref = ref;
    E.caps.cap[E.caps.n].resource = res;
    E.caps.cap[E.caps.n].rights = rights;
    E.caps.n++;
    if (rights & RX_RIGHT_WRITE) E.held_w[E.n_held_w++] = res;
    if ((rights & (RX_RIGHT_WRITE | RX_RIGHT_EFFECT)) == (RX_RIGHT_WRITE | RX_RIGHT_EFFECT))
        E.held_we[E.n_held_we++] = res;
}

static int settle(void) { return rx_world_wait_quiescent(&E.w, 30000); }

static void read_obj(RxObjRef r, RxObject *o) {
    if (rx_world_read(&E.w, r, o) != RX_OK) memset(o, 0, sizeof *o);
}

static int env_start(uint32_t workers) {
    memset(&E, 0, sizeof E);
    if (aienos_cap_start(&E.admin, &E.view) != 0) return -1;
    if (rx_world_init_native(&E.w, E.view, workers, 1u << 20) != RX_OK) return -1;
    E.w.external_subject = SUBJ_COG;
    uint64_t f[RX_MAX_FIELDS] = { 0 };
    f[0] = HYP_SEQ;
    if (rx_world_create(&E.w, RX_OT_HYPOTHESIS, RX_PERSIST_RESIDENT, RES_CTX, f, &E.hyp)) return -1;
    f[0] = GOAL_SEQ;
    if (rx_world_create(&E.w, RX_OT_GOAL, RX_PERSIST_RESIDENT, RES_CTX, f, &E.goal)) return -1;
    f[0] = PRED_SEQ;
    f[6] = RX_AIEN_PRED_FAILED;
    if (rx_world_create(&E.w, RX_OT_PREDICTION, RX_PERSIST_RESIDENT, RES_CTX, f, &E.pred)) return -1;
    memset(f, 0, sizeof f);
    f[1] = CLASS_X925;
    if (rx_world_create(&E.w, RX_OT_PLACEMENT, RX_PERSIST_RESIDENT, RES_CTX, f, &E.place)) return -1;
    memset(f, 0, sizeof f);
    for (uint32_t i = 0; i < N_TARGETS; i++) {
        uint64_t res = i == 5 ? RES_OFFICE : RES_T + i;
        if (rx_world_create(&E.w, 0xB100 + i, RX_PERSIST_RESIDENT, res, f, &E.target[i])) return -1;
    }
    for (uint32_t i = 0; i < N_EFFECTS; i++)
        if (rx_world_create(&E.w, AG_OT_EFFECT, RX_PERSIST_RESIDENT, RES_EFFECT + i, f, &E.effect[i]))
            return -1;
    E.caps.subject = SUBJ_COG;
    for (uint32_t i = 0; i < 4; i++) hold(RES_T + i, RX_RIGHT_READ | RX_RIGHT_WRITE);
    hold(RES_T + 4, RX_RIGHT_READ);
    hold(RES_OFFICE, RX_RIGHT_READ | RX_RIGHT_WRITE);
    hold(RES_EFFECT + 0, RX_RIGHT_READ | RX_RIGHT_WRITE | RX_RIGHT_EFFECT);
    hold(RES_EFFECT + 1, RX_RIGHT_READ | RX_RIGHT_WRITE);
    hold(RES_EFFECT + 2, RX_RIGHT_READ | RX_RIGHT_WRITE | RX_RIGHT_EFFECT);

    E.skills.n = 3;
    for (uint32_t i = 0; i < 3; i++) E.skills.skill[i] = (AgSkill){ 11 + i, sk_dummy, { (uint8_t)(i + 1) } };

    rc_context_init(&E.ctx, &E.w, SUBJ_COG, &E.caps);
    E.ctx.next_seq = 1000;
    E.ctx.active_generation = ACTIVE_GEN;
    E.ctx.authority_epoch = EPOCH;
    E.ctx.refs[0] = E.hyp;
    E.ctx.refs[1] = E.goal;
    E.ctx.refs[2] = E.pred;
    E.ctx.refs[3] = E.place;
    E.ctx.values[0] = LINEAGE;
    E.ctx.skills = &E.skills;
    for (uint32_t i = 0; i < 3; i++) E.ctx.skill_arity[i] = i + 1;
    E.ctx.n_effect_res = 2;
    E.ctx.effect_res[0] = RES_EFFECT + 0;
    E.ctx.effect_res[1] = RES_EFFECT + 1;
    E.ctx.forbid_lo = RES_FORBID_LO;
    E.ctx.forbid_hi = RES_FORBID_HI;
    E.ctx.max_graph_nodes = 16;
    E.ctx.max_effects = UINT32_MAX;

    for (uint32_t k = 1; k < RC_KIND_COUNT; k++) {
        if (rc_contract_std((RcKind)k, &E.con[k]) != 0 || rc_compile(&E.con[k], &E.comp[k]) != 0)
            return -1;
        uint64_t z[RX_MAX_FIELDS] = { 0 };
        if (rx_world_create(&E.w, RC_OT_DRAFT + k, RX_PERSIST_EPHEMERAL, RES_DRAFT + k, z, &E.draft[k]) ||
            rx_world_create(&E.w, E.con[k].world_type, RX_PERSIST_RESIDENT, RES_PUB + k, z, &E.pub[k]) ||
            rx_world_create(&E.w, RC_OT_STATUS + k, RX_PERSIST_RESIDENT, RES_STATUS + k, z, &E.status[k]))
            return -1;
        E.cog_draft[k] = mint(SUBJ_COG, RES_DRAFT + k, RX_RIGHT_READ | RX_RIGHT_WRITE);
        mint(SUBJ_COG, RES_PUB + k, RX_RIGHT_READ);   /* cognition may read what was published */
        E.gate_pub[k] = mint(SUBJ_GATE, RES_PUB + k, RX_RIGHT_READ | RX_RIGHT_WRITE);
        if (rc_gate_register(&E.gate[k], &E.w, &E.con[k], &E.ctx, SUBJ_GATE, E.draft[k],
                             mint(SUBJ_GATE, RES_DRAFT + k, RX_RIGHT_READ), E.pub[k], E.gate_pub[k],
                             E.status[k], mint(SUBJ_GATE, RES_STATUS + k,
                                               RX_RIGHT_READ | RX_RIGHT_WRITE)) != RX_OK)
            return -1;
    }
    return 0;
}

static void env_stop(void) {
    settle();
    rx_world_destroy(&E.w);
    rc_context_destroy(&E.ctx);
    aienos_cap_stop(E.admin, E.view);
}

/* ---- the gate path ---- */

/* Publish a draft; wait; return the gate's verdict (0 when it did not run:
 * the draft equals the one already there). */
static uint64_t through_gate(RcKind k, const RcObject *o) {
    RxObject st0, d0;
    read_obj(E.status[k], &st0);
    read_obj(E.draft[k], &d0);
    if (memcmp(d0.field, o->f, sizeof o->f) == 0) return 0;
    E.gate[k].graph = o->graph;
    RxMutation m[RX_MAX_FIELDS];
    for (uint32_t i = 0; i < RX_MAX_FIELDS; i++) m[i] = (RxMutation){ E.draft[k], i, o->f[i] };
    int64_t rc = rx_world_publish_external(&E.w, E.cog_draft[k], m, RX_MAX_FIELDS);
    CHECK(rc > 0, "draft publish for %s: %lld", rc_kind_name(k), (long long)rc);
    settle();
    RxObject st;
    read_obj(E.status[k], &st);
    CHECK(st.field[0] == st0.field[0] + 1, "gate %s ran once per draft", rc_kind_name(k));
    return st.field[1];
}

static void published(RcKind k, RcObject *o) {
    RxObject p;
    read_obj(E.pub[k], &p);
    memset(o, 0, sizeof *o);
    o->kind = k;
    memcpy(o->f, p.field, sizeof o->f);
    o->graph = E.gate[k].graph;
}

/* What the outside learns from a publication. */
static void observe(RcKind k, const RcObject *o) {
    if (k == RC_EVIDENCE) {
        /* The registry is bounded; what it could not keep is not evidence anyone can cite. */
        if (rc_context_add_evidence(&E.ctx, (uint32_t)o->f[0], o->f[1], (uint32_t)o->f[5]) != 0)
            return;
        if (o->f[5] == RC_VERDICT_PASS && o->f[0] == RC_EV_VERIFY && E.n_ev_verify_pass < 64)
            E.ev_verify_pass[E.n_ev_verify_pass++] = o->f[1];
        if (o->f[5] == RC_VERDICT_PASS && o->f[0] == RC_EV_GENERATION && E.n_ev_gen_pass < 64)
            E.ev_gen_pass[E.n_ev_gen_pass++] = o->f[1];
    }
    if (k == RC_PLAN || k == RC_HYPOTHESIS) E.ctx.next_seq++;
    if (k == RC_EFFECT_PROPOSAL) {
        pthread_mutex_lock(&E.ctx.mu);
        E.ctx.effects_used++;
        pthread_mutex_unlock(&E.ctx.mu);
    }
}

/* ---- the reference: hand-written predicates, one per kind ---- */

static int in_list(const uint64_t *l, uint32_t n, uint64_t v) {
    for (uint32_t i = 0; i < n; i++)
        if (l[i] == v) return 1;
    return 0;
}

static int is_forbidden(uint64_t r) { return r >= RES_FORBID_LO && r <= RES_FORBID_HI; }

static int live_at(uint64_t id, uint64_t gen, uint64_t *res) {
    if (id >= RX_MAX_OBJECTS) return 0;
    pthread_mutex_lock(&E.w.mu);
    RxObject o = E.w.objects[id];
    pthread_mutex_unlock(&E.w.mu);
    if (res) *res = o.resource;
    return o.live && (gen == UINT64_MAX || o.generation == gen);
}

static int resource_is_live(uint64_t r) {
    for (uint64_t i = 0; i < RX_MAX_OBJECTS; i++) {
        uint64_t res;
        if (live_at(i, UINT64_MAX, &res) && res == r) return 1;
    }
    return 0;
}

static uint64_t ctx_left(void) {
    return E.ctx.effects_used >= E.ctx.max_effects ? 0 : E.ctx.max_effects - E.ctx.effects_used;
}

static int graph_ok(const AgGraph *g, uint64_t f[RC_MAX_FIELDS], int *fields_match) {
    *fields_match = 0;
    if (!g) return 0;
    static AgGraph copy;
    copy = *g;
    if (rx_graph_validate(&copy, &E.w) != AG_OK_READY) return 0;
    uint32_t live = 0;
    for (uint32_t i = 0; i < copy.n_nodes; i++) live += copy.nodes[i].alive;
    uint64_t d0 = 0, d1 = 0;
    for (int i = 0; i < 8; i++) {
        d0 |= (uint64_t)copy.digest[i] << (8 * i);
        d1 |= (uint64_t)copy.digest[8 + i] << (8 * i);
    }
    *fields_match = f[0] == d0 && f[1] == d1 && f[3] == live && f[4] == copy.n_effects &&
                    f[6] == copy.n_evidence;
    int ok = copy.subject == f[2] && live <= 16 && copy.n_evidence >= copy.n_effects;
    for (uint32_t i = 0; i < copy.n_auth; i++)
        ok = ok && !(copy.auth[i].rights & RX_RIGHT_PRIVILEGED) &&
             !is_forbidden(copy.auth[i].resource);
    return ok;
}

static int ref_ok(RcKind k, const RcObject *o) {
    const uint64_t *f = o->f;
    uint32_t n = E.con[k].n_fields;
    for (uint32_t i = n; i < RC_MAX_FIELDS; i++)
        if (f[i]) return 0;
    switch (k) {
    case RC_PLAN:
        return f[0] == E.ctx.next_seq && f[1] == RX_AIEN_ACT_RESEARCH &&
               (f[2] >> 32) >= 1 && f[2] >= ((1ull << 32) | 1u) && f[2] <= ((0xfffffull << 32) | 0xfffffu) &&
               f[3] >= 1 && f[3] <= 0xfff && (f[4] == 0 || f[4] == HYP_SEQ) &&
               f[5] >= 1 && f[5] <= 3 && (f[6] == 0 || f[6] == GOAL_SEQ) &&
               (f[5] == 3 || f[4] != 0) && (f[5] != 3 || f[6] != 0) && f[7] == 0;
    case RC_HYPOTHESIS:
        return f[0] == E.ctx.next_seq && (f[1] == 1 || f[1] == 2) && f[2] == PRED_SEQ &&
               f[3] >= 1 && f[3] <= 0xfff && f[4] == CLASS_X925 &&
               (f[1] != 1 || f[4] != f[3]) && (f[1] != 2 || f[4] == f[3]) &&
               f[5] >= 1 && f[5] <= 1000000000000ull && f[6] != 0 && f[6] != f[5] &&
               f[7] >= 1 && f[7] <= 5;
    case RC_CAPABILITY_NEED:
        return f[0] == SUBJ_COG && f[1] >= 1 && f[1] <= (1u << 20) && f[2] != 0 &&
               resource_is_live(f[2]) && !is_forbidden(f[2]) && f[3] != 0 && f[3] <= 0xf &&
               f[4] <= RC_EFFECT_CLASSES && ((f[3] & RX_RIGHT_EFFECT) || f[4] == 0) &&
               f[5] != 0 && f[5] <= RC_LOCALITIES && f[6] >= 1 && f[6] <= 60000000 &&
               f[7] <= 1 && (!(f[3] & RX_RIGHT_EFFECT) || f[7] == 1);
    case RC_ACTION_GRAPH: {
        int match;
        uint64_t ff[RC_MAX_FIELDS];
        memcpy(ff, f, sizeof ff);
        if (f[2] == 0 || f[2] > UINT32_MAX || f[3] > UINT32_MAX ||
            f[4] > UINT32_MAX || f[5] > UINT32_MAX || f[6] > UINT32_MAX)
            return 0;
        return graph_ok(o->graph, ff, &match) && match && f[2] == SUBJ_COG && f[5] >= 1 &&
               f[5] <= 16 && f[4] <= ctx_left() && f[7] == 0;
    }
    case RC_SKILL_INVOCATION: {
        if (f[6] != SUBJ_COG || f[0] < 11 || f[0] > 13 || f[1] != f[0] - 10) return 0;
        for (uint32_t i = 0; i < 3; i++) {
            if (f[2 + i] > (1ull << 40)) return 0;
            if (i >= f[1] && f[2 + i] != 0) return 0;
        }
        return f[5] >= 1 && f[5] <= 8;
    }
    case RC_WORLD_MUTATION: {
        uint64_t res;
        return f[4] == SUBJ_COG && f[0] < RX_MAX_OBJECTS && f[1] <= UINT32_MAX &&
               live_at(f[0], f[1], &res) && res == f[5] && f[5] != 0 &&
               in_list(E.held_w, E.n_held_w, f[5]) && !is_forbidden(f[5]) && f[2] <= 7;
    }
    case RC_EFFECT_PROPOSAL:
        return f[5] == SUBJ_COG && ctx_left() > 0 && f[0] != 0 &&
               (f[0] == RES_EFFECT + 0 || f[0] == RES_EFFECT + 1) &&
               in_list(E.held_we, E.n_held_we, f[0]) && f[6] >= 1 && f[6] <= 3 && f[3] <= 1 &&
               (f[6] != 3 || f[3] == 1) && (f[3] != 1 || f[4] != 0) &&
               in_list(E.ev_verify_pass, E.n_ev_verify_pass, f[2]);
    case RC_GENERATION_CANDIDATE:
        return f[6] == SUBJ_COG && f[1] == ACTIVE_GEN && f[0] > ACTIVE_GEN && f[2] == EPOCH &&
               f[7] == LINEAGE && in_list(E.ev_gen_pass, E.n_ev_gen_pass, f[3]) && f[4] == 1 &&
               f[5] >= 1 && f[5] <= 32;
    case RC_EVIDENCE:
        return f[7] == SUBJ_COG && f[0] >= 1 && f[0] <= 3 && f[1] != 0 && f[2] != 0 &&
               f[3] >= 1 && f[3] <= (1ull << 32) && (f[5] == 1 || f[5] == 2) && f[4] <= f[3] &&
               (f[5] != 1 || f[4] == 0) && (f[5] != 2 || f[4] > 0) && f[6] != 0;
    default:
        return 0;
    }
}

/* ---- graphs ---- */

enum { GR_OK = 0, GR_BROKEN, GR_FORBIDDEN, GR_TWO_EFFECTS, GR_COUNT };

static void build_graph(AgGraph *g, int variant, uint64_t c0, uint64_t imm) {
    rx_graph_init(g, SUBJ_COG);
    int k = rx_graph_node(g, AG_CONST, AG_T_U64);
    g->nodes[k].imm = c0;
    int p = rx_graph_node(g, AG_PURE, AG_T_U64);
    g->nodes[p].op = OP_ADD;
    g->nodes[p].imm = imm;
    rx_graph_data(g, (uint32_t)k, (uint32_t)p, 0, AG_EDGE_DATA);
    int v = rx_graph_node(g, AG_VERIFY, AG_T_VERDICT);
    g->nodes[v].imm = 0;
    g->nodes[v].imm2 = 1ull << 40;
    rx_graph_data(g, (uint32_t)p, (uint32_t)v, 0, AG_EDGE_DATA);
    int effects = variant == GR_TWO_EFFECTS ? 2 : 1;
    int last = -1;
    for (int e = 0; e < effects; e++) {
        int pr = rx_graph_node(g, AG_EFFECT_PROPOSE, AG_T_PROPOSAL);
        rx_graph_data(g, (uint32_t)p, (uint32_t)pr, 0, AG_EDGE_DATA);
        int pf = rx_graph_node(g, AG_EFFECT_PERFORM, AG_T_RECEIPT);
        g->nodes[pf].obj = variant == GR_FORBIDDEN ? E.target[5] : E.effect[0];
        /* broken: a raw number where a proposal must go */
        rx_graph_data(g, (uint32_t)(variant == GR_BROKEN ? k : pr), (uint32_t)pf, 0, AG_EDGE_DATA);
        rx_graph_data(g, (uint32_t)v, (uint32_t)pf, 1, AG_EDGE_DATA);
        if (last >= 0) rx_graph_order(g, (uint32_t)last, (uint32_t)pf);
        last = pf;
    }
    rx_graph_success(g, (uint32_t)last, AG_OK);
    rx_graph_failure(g, (uint32_t)v, AG_FAILED);
}

static void graph_fields_of(const AgGraph *g, uint64_t *f) {
    static AgGraph copy;
    copy = *g;
    rx_graph_validate(&copy, &E.w);
    uint32_t live = 0;
    for (uint32_t i = 0; i < copy.n_nodes; i++) live += copy.nodes[i].alive;
    f[0] = f[1] = 0;
    for (int i = 0; i < 8; i++) {
        f[0] |= (uint64_t)copy.digest[i] << (8 * i);
        f[1] |= (uint64_t)copy.digest[8 + i] << (8 * i);
    }
    f[3] = live;
    f[4] = copy.n_effects;
    f[6] = copy.n_evidence;
}

/* ---- intents: what a correct cognition would answer ---- */

typedef struct {
    RcObject o;
    AgGraph graph[GR_COUNT];
} Intent;

static void make_intent(RcKind k, Rng *r, Intent *in) {
    RcObject *o = &in->o;
    memset(o, 0, sizeof *o);
    o->kind = k;
    uint64_t *f = o->f;
    switch (k) {
    case RC_PLAN:
        f[0] = E.ctx.next_seq;
        f[1] = RX_AIEN_ACT_RESEARCH;
        f[5] = rin(r, 1, 3);
        f[2] = (rin(r, 1, 4096) << 32) | rin(r, 1, 4096);
        f[3] = rin(r, 0, 1) ? CLASS_X925 : CLASS_A725;
        f[4] = f[5] == 3 ? (rin(r, 0, 1) ? HYP_SEQ : 0) : HYP_SEQ;
        f[6] = f[5] == 3 ? GOAL_SEQ : (rin(r, 0, 1) ? GOAL_SEQ : 0);
        break;
    case RC_HYPOTHESIS:
        f[0] = E.ctx.next_seq;
        f[1] = rin(r, 1, 2);
        f[7] = rin(r, 1, 5);
        f[2] = PRED_SEQ;
        f[4] = CLASS_X925;
        f[3] = f[1] == RX_AIEN_HYP_CORE_CLASS ? CLASS_A725 : CLASS_X925;
        f[5] = rin(r, 1000, 1000000000);
        f[6] = f[5] + rin(r, 1, 100000);
        break;
    case RC_CAPABILITY_NEED: {
        static const uint64_t live_res[] = { RES_T + 0, RES_T + 1, RES_T + 2, RES_EFFECT + 0, RES_CTX };
        f[0] = SUBJ_COG;
        f[1] = rin(r, 1, 1u << 20);
        f[2] = live_res[rin(r, 0, 4)];
        f[3] = rin(r, 1, 15);
        f[4] = (f[3] & RX_RIGHT_EFFECT) ? rin(r, 0, RC_EFFECT_CLASSES) : 0;
        f[5] = rin(r, 1, RC_LOCALITIES);
        f[6] = rin(r, 1, 60000000);
        f[7] = (f[3] & RX_RIGHT_EFFECT) ? 1 : rin(r, 0, 1);
        break;
    }
    case RC_ACTION_GRAPH: {
        uint64_t c0 = rin(r, 1, 1000), imm = rin(r, 1, 1000);
        for (int v = 0; v < GR_COUNT; v++) build_graph(&in->graph[v], v, c0, imm);
        o->graph = &in->graph[GR_OK];
        graph_fields_of(o->graph, f);
        f[2] = SUBJ_COG;
        f[5] = rin(r, 1, 16);
        break;
    }
    case RC_SKILL_INVOCATION:
        f[6] = SUBJ_COG;
        f[0] = rin(r, 11, 13);
        f[1] = f[0] - 10;
        for (uint32_t i = 0; i < f[1]; i++) f[2 + i] = rin(r, 0, 1ull << 40);
        f[5] = rin(r, 1, 8);
        break;
    case RC_WORLD_MUTATION: {
        uint32_t t = (uint32_t)rin(r, 0, 3);
        f[4] = SUBJ_COG;
        f[0] = E.target[t].id;
        f[1] = E.target[t].generation;
        f[2] = rin(r, 0, 7);
        f[3] = rnd(r);
        f[5] = RES_T + t;
        break;
    }
    case RC_EFFECT_PROPOSAL:
        f[5] = SUBJ_COG;
        f[0] = RES_EFFECT + 0;
        f[6] = rin(r, 1, 3);
        f[3] = f[6] == 3 ? 1 : rin(r, 0, 1);
        f[4] = f[3] ? rin(r, 1, 1u << 30) : 0;
        f[1] = rnd(r);
        f[2] = E.ev_verify_pass[rin(r, 0, E.n_ev_verify_pass - 1)];
        break;
    case RC_GENERATION_CANDIDATE:
        f[6] = SUBJ_COG;
        f[1] = ACTIVE_GEN;
        f[0] = ACTIVE_GEN + rin(r, 1, 3);
        f[2] = EPOCH;
        f[7] = LINEAGE;
        f[3] = E.ev_gen_pass[rin(r, 0, E.n_ev_gen_pass - 1)];
        f[4] = 1;
        f[5] = rin(r, 1, 32);
        break;
    case RC_EVIDENCE:
        f[7] = SUBJ_COG;
        f[0] = rin(r, 1, 3);
        f[1] = rin(r, 1, 1u << 30);
        f[2] = rin(r, 1, 1u << 30);
        f[3] = rin(r, 1, 1000000);
        f[5] = rin(r, 1, 2);
        f[4] = f[5] == RC_VERDICT_PASS ? 0 : rin(r, 1, f[3]);
        f[6] = rnd(r) | 1u;
        break;
    default:
        break;
    }
}

/* ---- mistakes ---- */

#define VALID_WRONG 0xffu   /* no contract can see this one */
#define N_FAULTS 10

/* Apply mistake `id` to o. Returns the constraint type it breaks (a
 * RcRuleKind, 0 for a type, VALID_WRONG), or -1 when it does not apply to
 * this intent. *graph_variant receives a graph swap, if any. */
static int fault(RcKind k, int id, RcObject *o, const Intent *in, Rng *r, int *graph_variant) {
    uint64_t *f = o->f;
    *graph_variant = -1;
    switch (k) {
    case RC_PLAN:
        switch (id) {
        case 0: f[5] = 200; return 0;                                    /* type */
        case 1: f[5] = 7; return RC_RULE_ENUM;
        case 2: f[2] = 5; return RC_RULE_RANGE;
        case 3: f[3] = 0x1000; return RC_RULE_RANGE;
        case 4: f[4] = HYP_SEQ + 1; return RC_RULE_REFERENCE;
        case 5: if (f[5] == 3) return -1; f[4] = 0; return RC_RULE_CROSS;
        case 6: f[0] += 5; return RC_RULE_REFERENCE;                      /* derived: repairable */
        case 7: f[7] = 3; return RC_RULE_RANGE;
        case 8: f[3] = f[3] == CLASS_X925 ? CLASS_A725 : CLASS_X925; return VALID_WRONG;
        case 9: f[1] = 2; return RC_RULE_ENUM;
        }
        break;
    case RC_HYPOTHESIS:
        switch (id) {
        case 0: f[1] = 90; return 0;
        case 1: f[1] = 3; return RC_RULE_ENUM;
        case 2: f[7] = 9; return RC_RULE_ENUM;
        case 3: f[2] = PRED_SEQ + 7; return RC_RULE_REFERENCE;
        case 4: f[4] = CLASS_A725; f[3] = f[1] == 1 ? CLASS_X925 : CLASS_A725; return RC_RULE_REFERENCE;
        case 5: if (f[1] != 2) return -1; f[3] = CLASS_A725; return RC_RULE_CROSS;
        case 6: f[6] = f[5]; return RC_RULE_CROSS;
        case 7: f[0] = 1; return RC_RULE_REFERENCE;                       /* derived */
        case 8: f[5] = 0; f[6] = rin(r, 1, 100); return RC_RULE_RANGE;
        case 9: f[5] += 1; if (f[5] == f[6]) f[5] += 1; return VALID_WRONG;
        }
        break;
    case RC_CAPABILITY_NEED:
        switch (id) {
        case 0: f[0] = SUBJ_OTHER; return RC_RULE_AUTHORITY;
        case 1: f[2] = RES_OFFICE; return RC_RULE_EFFECT;
        case 2: f[2] = 0xB0DEAD0; return RC_RULE_REFERENCE;
        case 3: f[3] |= RX_RIGHT_MINT; return RC_RULE_AUTHORITY;          /* narrowable */
        case 4: f[3] |= 0x40000000u; return RC_RULE_STRUCT;               /* narrowable */
        case 5: f[3] = RX_RIGHT_REVOKE; return RC_RULE_STRUCT;            /* narrows to nothing */
        case 6: f[6] = 0; return RC_RULE_RANGE;
        case 7: if (!(f[3] & RX_RIGHT_EFFECT)) return -1; f[7] = 0; return RC_RULE_CROSS;
        case 8: if (f[3] & RX_RIGHT_EFFECT) return -1; f[4] = 1; return RC_RULE_CROSS;
        case 9: f[6] = f[6] > 1 ? f[6] - 1 : 2; return VALID_WRONG;
        }
        break;
    case RC_ACTION_GRAPH:
        switch (id) {
        case 0: *graph_variant = GR_BROKEN; return RC_RULE_STRUCT;
        case 1: *graph_variant = GR_FORBIDDEN; graph_fields_of(&in->graph[GR_FORBIDDEN], f);
                return RC_RULE_EFFECT;
        case 2: f[0] ^= 1; return RC_RULE_STRUCT;                         /* derived */
        case 3: f[2] = SUBJ_OTHER; return RC_RULE_AUTHORITY;
        case 4: f[5] = 99; return RC_RULE_RANGE;
        case 5: f[7] = 1; return RC_RULE_RANGE;
        case 6: f[5] = f[5] < 16 ? f[5] + 1 : 1; return VALID_WRONG;
        case 7: f[3] = 1ull << 40; return 0;
        default: return -1;
        }
    case RC_SKILL_INVOCATION:
        switch (id) {
        case 0: f[0] = 99; return RC_RULE_REFERENCE;
        case 1: f[1] = f[1] == 3 ? 1 : f[1] + 1; return RC_RULE_REFERENCE;
        case 2: if (f[1] > 2 || f[1] != f[0] - 10) return -1; f[2 + f[1]] = 5; return RC_RULE_CROSS;
        case 3: f[5] = 0; return RC_RULE_RANGE;
        case 4: f[2] = 1ull << 50; return RC_RULE_RANGE;
        case 5: f[6] = SUBJ_OTHER; return RC_RULE_AUTHORITY;
        case 6: f[1] = 1ull << 40; return 0;
        case 7: f[2] = f[2] + 1; return VALID_WRONG;
        default: return -1;
        }
    case RC_WORLD_MUTATION:
        switch (id) {
        case 0: f[0] = E.target[4].id; f[1] = E.target[4].generation; f[5] = RES_T + 4;
                return RC_RULE_AUTHORITY;
        case 1: f[1] += 1; return RC_RULE_GENERATION;
        case 2: f[2] = 9; return RC_RULE_RANGE;
        case 3: f[5] = RES_T + (f[5] == RES_T + 0 ? 1 : 0); return RC_RULE_REFERENCE;  /* derived */
        case 4: f[0] = E.target[5].id; f[1] = E.target[5].generation; f[5] = RES_OFFICE;
                return RC_RULE_EFFECT;
        case 5: f[4] = SUBJ_OTHER; return RC_RULE_AUTHORITY;
        case 6: f[0] = 300; return 0;
        case 7: f[3] ^= 0x10; return VALID_WRONG;
        default: return -1;
        }
    case RC_EFFECT_PROPOSAL:
        switch (id) {
        case 0: f[0] = RES_EFFECT + 2; return RC_RULE_EFFECT;
        case 1: f[0] = RES_EFFECT + 1; return RC_RULE_AUTHORITY;
        case 2: f[2] = 0xDEAD; return RC_RULE_EVIDENCE;
        case 3: f[2] = E.ev_verify_fail; return RC_RULE_EVIDENCE;
        case 4: if (f[6] != 3) return -1; f[3] = 0; return RC_RULE_CROSS;
        case 5: if (f[3] != 1) return -1; f[4] = 0; return RC_RULE_EFFECT;
        case 6: f[6] = 5; return RC_RULE_ENUM;
        case 7: f[5] = SUBJ_OTHER; return RC_RULE_AUTHORITY;
        case 8: f[3] = 2; return 0;
        case 9: f[1] ^= 0x100; return VALID_WRONG;
        }
        break;
    case RC_GENERATION_CANDIDATE:
        switch (id) {
        case 0: f[1] = ACTIVE_GEN - 1; return RC_RULE_GENERATION;
        case 1: f[0] = ACTIVE_GEN; return RC_RULE_GENERATION;
        case 2: f[2] = EPOCH - 1; return RC_RULE_GENERATION;
        case 3: f[3] = 0xBEEF; return RC_RULE_EVIDENCE;
        case 4: f[4] = 0; return RC_RULE_RANGE;
        case 5: f[5] = 40; return RC_RULE_RANGE;
        case 6: f[7] = LINEAGE + 1; return RC_RULE_REFERENCE;
        case 7: f[6] = SUBJ_OTHER; return RC_RULE_AUTHORITY;
        case 8: f[4] = 2; return 0;
        case 9: f[5] = f[5] < 32 ? f[5] + 1 : 1; return VALID_WRONG;
        }
        break;
    case RC_EVIDENCE:
        switch (id) {
        case 0: if (f[5] != 1) return -1; f[4] = 1; return RC_RULE_CROSS;
        case 1: if (f[5] != 2) return -1; f[4] = f[3] + 1; return RC_RULE_CROSS;
        case 2: f[3] = 0; f[4] = 0; return RC_RULE_RANGE;
        case 3: f[0] = 9; return RC_RULE_ENUM;
        case 4: f[6] = 0; return RC_RULE_STRUCT;
        case 5: f[7] = SUBJ_OTHER; return RC_RULE_AUTHORITY;
        case 6: f[1] = 0; return RC_RULE_STRUCT;
        case 7: f[0] = 70; return 0;
        case 8: f[1] ^= 0x4; if (!f[1]) f[1] = 1; return VALID_WRONG;
        case 9: if (f[5] != 1) return -1; f[5] = 2; return RC_RULE_CROSS;
        }
        break;
    default:
        break;
    }
    return -1;
}

/* ---- the synthetic backend ---- */

#define FAULT_RATE_PCT 35u
#define SECOND_FAULT_PCT 25u

typedef struct {
    RcKind k;
    Intent *in;
    uint64_t seed;
    RcObject cur;           /* this attempt's answer, mistakes included */
    int graph_variant;
    int n_faults;
    int fault_class[2];
} Synth;

static void synth_begin(void *self, uint32_t attempt) {
    Synth *s = self;
    Rng r = { mix(s->seed ^ ((uint64_t)attempt << 48)) };
    s->cur = s->in->o;
    s->graph_variant = GR_OK;
    s->n_faults = 0;
    if (rin(&r, 0, 99) >= FAULT_RATE_PCT) return;
    int want = rin(&r, 0, 99) < SECOND_FAULT_PCT ? 2 : 1;
    for (int tries = 0; tries < 20 && s->n_faults < want; tries++) {
        int gv;
        RcObject t = s->cur;
        int c = fault(s->k, (int)rin(&r, 0, N_FAULTS - 1), &t, s->in, &r, &gv);
        if (c < 0) continue;
        s->cur = t;
        if (gv >= 0) s->graph_variant = gv;
        s->fault_class[s->n_faults++] = c;
    }
}

static const AgGraph *synth_graph(void *self, uint32_t attempt) {
    Synth *s = self;
    (void)attempt;
    return &s->in->graph[s->graph_variant];
}

static uint64_t synth_draw(void *self, const RcContract *c, uint32_t attempt, uint32_t fld,
                           const RcObject *partial, const RcDomain *dom) {
    Synth *s = self;
    (void)c; (void)attempt; (void)partial;
    uint64_t want = s->cur.f[fld];
    if (!dom || rc_domain_contains(dom, want)) return want;
    /* Masked: the preferred token is not allowed; its next choice is what it meant. */
    return s->in->o.f[fld];
}

static RcBackend backend(Synth *s, int constrained) {
    RcBackend b = { constrained ? "synthetic.constrained" : "synthetic.free", constrained,
                    synth_draw, synth_graph, synth_begin, s };
    return b;
}

/* ---- scenarios ---- */

static void seed_evidence(void) {
    printf("[*] evidence published through the evidence gate\n");
    Rng r = { 77 };
    for (int i = 0; i < 12; i++) {
        RcObject o = { .kind = RC_EVIDENCE };
        o.f[7] = SUBJ_COG;
        o.f[0] = i < 6 ? RC_EV_VERIFY : (i < 10 ? RC_EV_GENERATION : RC_EV_MEASURE);
        o.f[1] = 500 + (uint64_t)i;
        o.f[2] = 1 + (uint64_t)i;
        o.f[3] = rin(&r, 10, 1000);
        o.f[5] = i == 5 ? RC_VERDICT_FAIL : RC_VERDICT_PASS;
        o.f[4] = o.f[5] == RC_VERDICT_PASS ? 0 : 1;
        o.f[6] = rnd(&r) | 1u;
        uint64_t v = through_gate(RC_EVIDENCE, &o);
        CHECK(v == RC_GATE_PUBLISHED, "seed evidence %d published (%llu)", i, (unsigned long long)v);
        RcObject p;
        published(RC_EVIDENCE, &p);
        CHECK(memcmp(p.f, o.f, sizeof o.f) == 0, "seed evidence %d read back", i);
        observe(RC_EVIDENCE, &p);
        if (i == 5) E.ev_verify_fail = o.f[1];
    }
    CHECK(E.n_ev_verify_pass == 5 && E.n_ev_gen_pass == 4, "evidence registry %u verify %u gen",
          E.n_ev_verify_pass, E.n_ev_gen_pass);
}

static void t_contracts(void) {
    printf("[*] contracts are typed state with a stable identity\n");
    uint32_t kinds = 0;
    for (uint32_t k = 1; k < RC_KIND_COUNT; k++) {
        RcContract a, b;
        rc_contract_std((RcKind)k, &a);
        rc_contract_std((RcKind)k, &b);
        CHECK(memcmp(a.identity, b.identity, 32) == 0, "%s identity stable", a.name);
        b.field[0].name = "renamed";
        rc_contract_identify(&b);
        CHECK(memcmp(a.identity, b.identity, 32) == 0, "%s identity ignores names", a.name);
        b.rule[0].k ^= 1;
        rc_contract_identify(&b);
        CHECK(memcmp(a.identity, b.identity, 32) != 0, "%s identity follows its rules", a.name);
        for (uint32_t j = 1; j < k; j++)
            CHECK(memcmp(a.identity, E.con[j].identity, 32) != 0, "%s differs from %s", a.name,
                  E.con[j].name);
        for (uint32_t i = 0; i < a.n_rules; i++) kinds |= 1u << a.rule[i].kind;
        R.enforced_rules += E.comp[k].n_enforced;
        R.post_rules += E.comp[k].n_post;
        printf("    %-22s %u fields %2u rules: %2u compiled into generation, %u checked after\n",
               a.name, a.n_fields, a.n_rules, E.comp[k].n_enforced, E.comp[k].n_post);
    }
    for (uint32_t t = RC_RULE_RANGE; t < RC_RULE_KIND_COUNT; t++) {
        CHECK(kinds & (1u << t), "constraint type %s is used by some contract",
              rc_rule_kind_name((RcRuleKind)t));
        if (kinds & (1u << t)) R.rule_kinds_defined++;
    }
    RcContract bad = E.con[RC_PLAN];
    bad.order[1] = bad.order[0];
    RcCompiled pc;
    CHECK(rc_compile(&bad, &pc) != 0, "a generation order that repeats a field is refused");
}

/* Every candidate, valid or not, judged by the contract and by the reference. */
static void t_differential(uint32_t per_kind) {
    printf("[*] contract checker against the independent reference\n");
    for (uint32_t k = 1; k < RC_KIND_COUNT; k++) {
        static Intent in;
        for (uint32_t i = 0; i < per_kind; i++) {
            Rng r = { mix(0xD1FFull * k + i) };
            make_intent((RcKind)k, &r, &in);
            RcVerdict v;
            rc_check(&E.con[k], &E.ctx, &in.o, &v);
            CHECK(v.ok && ref_ok((RcKind)k, &in.o), "%s intent %u is valid (rule %u)",
                  rc_kind_name((RcKind)k), i, v.n ? v.v[0].rule : 0);
            int nf = (int)rin(&r, 0, 2);
            RcObject o = in.o;
            int cls[2] = { -1, -1 }, applied = 0;
            for (int j = 0; j < nf; j++) {
                int gv;
                RcObject t = o;
                int c = fault((RcKind)k, (int)rin(&r, 0, N_FAULTS - 1), &t, &in, &r, &gv);
                if (c < 0) continue;
                o = t;
                if (gv >= 0) o.graph = &in.graph[gv];
                cls[applied++] = c;
            }
            rc_check(&E.con[k], &E.ctx, &o, &v);
            int ref = ref_ok((RcKind)k, &o);
            R.diff_candidates++;
            if (v.ok != ref) {
                R.diff_mismatch++;
                CHECK(0, "%s candidate %u: contract %d reference %d (rule %u)", rc_kind_name((RcKind)k),
                      i, v.ok, ref, v.n ? v.v[0].rule : 0);
            }
            if (!v.ok)
                for (uint32_t t = 0; t < RC_RULE_KIND_COUNT; t++)
                    if (v.kinds_violated & (1u << t)) R.caught_by_kind[t]++;
            if (applied == 1 && cls[0] != VALID_WRONG) {
                R.single_fault++;
                int seen = cls[0] == 0 ? (v.kinds_violated & 1u) != 0
                                       : (v.kinds_violated & (1u << cls[0])) != 0;
                if (!seen) {
                    R.single_fault_misclassified++;
                    CHECK(0, "%s: a %s mistake was not reported as one (violated 0x%x)",
                          rc_kind_name((RcKind)k), rc_rule_kind_name((RcRuleKind)cls[0]),
                          v.kinds_violated);
                }
            }
            if (applied == 1 && cls[0] == VALID_WRONG)
                CHECK(v.ok, "%s: a valid-but-wrong answer passes (no contract can see it)",
                      rc_kind_name((RcKind)k));
        }
    }
    for (uint32_t t = 0; t < RC_RULE_KIND_COUNT; t++)
        CHECK(R.caught_by_kind[t] > 0, "a %s violation was caught",
              rc_rule_kind_name((RcRuleKind)t));
    printf("    %u candidates, %u disagreements; %u single mistakes, %u reported under another type\n",
           R.diff_candidates, R.diff_mismatch, R.single_fault, R.single_fault_misclassified);
}

/* Draws uniformly inside every compiled domain: the rules folded into the
 * domains must then always hold (soundness), and every intent must lie inside
 * the domains given its own prefix (nothing valid is masked away). */
static uint64_t uniform_in(const RcDomain *d, Rng *r) {
    uint64_t v = 0;
    if (d->has_set && d->n_set) v = d->set[rin(r, 0, d->n_set - 1)];
    else if (d->has_enum) v = rin(r, 0, 63);
    else if (d->is_bits) v = rnd(r) & d->bits;
    else if (d->hi - d->lo < UINT64_MAX) v = d->lo + rnd(r) % (d->hi - d->lo + 1);
    else v = rnd(r);
    if (!rc_domain_contains(d, v)) rc_domain_pick(d, v, &v);
    return v;
}

static void t_soundness(uint32_t per_kind) {
    printf("[*] compiled domains are sound and keep every valid answer\n");
    static Intent in;
    for (uint32_t k = 1; k < RC_KIND_COUNT; k++) {
        const RcCompiled *p = &E.comp[k];
        for (uint32_t i = 0; i < per_kind; i++) {
            Rng r = { mix(0x50F7ull * k + i) };
            make_intent((RcKind)k, &r, &in);
            RcObject o = { .kind = (RcKind)k, .graph = in.o.graph };
            int dead = 0;
            for (uint32_t j = 0; j < E.con[k].n_fields && !dead; j++) {
                uint32_t fl = E.con[k].order[j];
                RcDomain d;
                rc_domain(p, &E.ctx, &o, fl, &d);
                if (d.empty) { dead = 1; break; }
                o.f[fl] = uniform_in(&d, &r);
                CHECK(rc_domain_contains(&d, o.f[fl]), "%s field %u drawn inside its domain",
                      E.con[k].name, fl);
            }
            if (!dead) {
                RcVerdict v;
                rc_check(&E.con[k], &E.ctx, &o, &v);
                R.sound_samples++;
                for (uint32_t x = 0; x < v.n; x++)
                    if (v.v[x].rule == RC_TYPE_RULE || p->enforced[v.v[x].rule]) {
                        R.sound_enforced_violations++;
                        CHECK(0, "%s: compiled rule %u violated inside the domains", E.con[k].name,
                              v.v[x].rule);
                    }
            }
            /* reachability of the intent */
            RcObject q = { .kind = (RcKind)k, .graph = in.o.graph };
            for (uint32_t j = 0; j < E.con[k].n_fields; j++) {
                uint32_t fl = E.con[k].order[j];
                RcDomain d;
                rc_domain(p, &E.ctx, &q, fl, &d);
                R.reach_checked++;
                if (!rc_domain_contains(&d, in.o.f[fl])) {
                    R.reach_missed++;
                    CHECK(0, "%s: valid value of field %u masked away", E.con[k].name, fl);
                }
                q.f[fl] = in.o.f[fl];
            }
        }
    }
    printf("    %u samples, %u compiled-rule violations; %u intent fields, %u masked away\n",
           R.sound_samples, R.sound_enforced_violations, R.reach_checked, R.reach_missed);
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static int meaning_equal(RcKind k, const RcObject *a, const RcObject *b) {
    for (uint32_t i = 0; i < E.con[k].n_fields; i++)
        if (!E.con[k].field[i].derived && a->f[i] != b->f[i]) return 0;
    return 1;
}

/* The workload: each request produced, sent through the gate, read back. */
static void run_path(int path, uint32_t per_kind) {
    printf("[*] %s path: %u requests per kind\n", path == PATH_CONSTRAINED ? "constrained" : "free",
           per_kind);
    static Intent in;
    for (uint32_t k = 1; k < RC_KIND_COUNT; k++) {
        PathStats *ps = &R.path[path][k];
        for (uint32_t i = 0; i < per_kind; i++) {
            Rng r = { mix(0xC0DEull * k + i) };
            make_intent((RcKind)k, &r, &in);
            Synth s = { (RcKind)k, &in, mix(0xFA17ull * k + i), { 0 }, GR_OK, 0, { 0, 0 } };
            RcBackend b = backend(&s, path == PATH_CONSTRAINED);
            RcStats st;
            memset(&st, 0, sizeof st);
            RcObject out;
            uint64_t t0 = now_ns();
            int res = rc_produce(&E.comp[k], &E.ctx, &b, 3, &out, &st);
            ps->requests++;
            ps->candidates += st.candidates;
            ps->invalid += st.invalid;
            ps->repairs += st.repairs;
            ps->retries += st.retries;
            ps->dead_ends += st.dead_ends;
            ps->draws += st.draws;
            ps->checks += st.checks;
            ps->gen_ns += st.gen_ns;
            ps->check_ns += st.check_ns;
            if (res == RC_OUT_UNSATISFIABLE) { ps->unsat++; continue; }
            if (res == RC_OUT_REJECTED) { ps->rejected++; continue; }
            uint64_t verdict = through_gate((RcKind)k, &out);
            uint64_t t1 = now_ns();
            if (verdict == 0) continue;     /* identical to the draft already there */
            if (ps->n_lat < 4096) ps->lat[ps->n_lat++] = t1 - t0;
            if (verdict == RC_GATE_REJECTED) { ps->gate_rejected++; continue; }
            RcObject p;
            published((RcKind)k, &p);
            RcVerdict v;
            rc_check(&E.con[k], &E.ctx, &p, &v);
            if (!v.ok || !ref_ok((RcKind)k, &p)) {
                ps->invalid_published++;
                CHECK(0, "%s published an object the contract refuses", rc_kind_name((RcKind)k));
            }
            ps->published++;
            if (!meaning_equal((RcKind)k, &p, &in.o) ||
                (k == RC_ACTION_GRAPH && p.graph != in.o.graph))
                ps->semantic_fail++;
            observe((RcKind)k, &p);
        }
        CHECK(ps->invalid_published == 0, "%s: no invalid object published", rc_kind_name((RcKind)k));
        if (path == PATH_CONSTRAINED && E.comp[k].n_post == 0)
            CHECK(ps->invalid == 0 && ps->repairs == 0,
                  "%s: every rule compiled, so no invalid candidate was generated (%llu)",
                  rc_kind_name((RcKind)k), (unsigned long long)ps->invalid);
        CHECK(ps->gate_rejected == 0, "%s: the gate refused nothing produce accepted",
              rc_kind_name((RcKind)k));
    }
}

/* Drafts written straight into the draft object, skipping produce. */
static void t_hostile_drafts(void) {
    printf("[*] hostile drafts: the gate checks everything itself\n");
    static Intent in;
    for (uint32_t k = 1; k < RC_KIND_COUNT; k++) {
        for (int id = 0; id < N_FAULTS; id++) {
            Rng r = { mix(0x4057ull * k + (uint64_t)id) };
            make_intent((RcKind)k, &r, &in);
            RcObject o = in.o;
            int gv;
            int c = fault((RcKind)k, id, &o, &in, &r, &gv);
            if (c < 0 || c == VALID_WRONG) continue;
            if (gv >= 0) o.graph = &in.graph[gv];
            RcObject before;
            published((RcKind)k, &before);
            RcVerdict v;
            rc_check(&E.con[k], &E.ctx, &o, &v);
            RcObject rep = o;
            RcVerdict v2;
            int repairable = rc_repair(&E.con[k], &E.ctx, &rep, &v) >= 0 &&
                             rc_check(&E.con[k], &E.ctx, &rep, &v2) == 0;
            uint64_t verdict = through_gate((RcKind)k, &o);
            if (verdict == 0) continue;
            R.hostile_drafts++;
            RcObject after;
            published((RcKind)k, &after);
            if (verdict == RC_GATE_REJECTED) {
                R.hostile_rejected++;
                CHECK(!repairable, "%s mistake %d: rejected only when not repairable",
                      rc_kind_name((RcKind)k), id);
                CHECK(memcmp(before.f, after.f, sizeof before.f) == 0,
                      "%s mistake %d: a rejection leaves the published object as it was",
                      rc_kind_name((RcKind)k), id);
            } else {
                CHECK(verdict == RC_GATE_REPAIRED && repairable,
                      "%s mistake %d: an invalid draft is never published as is (verdict %llu)",
                      rc_kind_name((RcKind)k), id, (unsigned long long)verdict);
                R.hostile_repaired++;
                RcVerdict pv;
                rc_check(&E.con[k], &E.ctx, &after, &pv);
                if (!pv.ok || !ref_ok((RcKind)k, &after)) R.hostile_published_invalid++;
                CHECK(pv.ok, "%s mistake %d: the repaired object passes", rc_kind_name((RcKind)k), id);
                CHECK(meaning_equal((RcKind)k, &after, &in.o),
                      "%s mistake %d: repair restored the intended meaning", rc_kind_name((RcKind)k), id);
                observe((RcKind)k, &after);
            }
        }
    }
    CHECK(R.hostile_published_invalid == 0, "no hostile draft published invalid");
    printf("    %u hostile drafts: %u rejected, %u repaired (derived fields or narrowing only)\n",
           R.hostile_drafts, R.hostile_rejected, R.hostile_repaired);
}

static int bypass_fn(RxCtx *x) {
    const RxObjRef *target = x->user;
    x->out[x->n_out++] = (RxMutation){ *target, 0, 0xBAD };
    return 0;
}

static void t_bypass(void) {
    printf("[*] nothing but the gate can publish\n");
    for (uint32_t k = 1; k < RC_KIND_COUNT; k++) {
        RxObject before;
        read_obj(E.pub[k], &before);
        /* cognition writes the published object with its draft capability */
        RxMutation m = { E.pub[k], 0, 0xBAD };
        R.bypass_attempts++;
        int64_t rc = rx_world_publish_external(&E.w, E.cog_draft[k], &m, 1);
        if (rc == RX_ERR_AUTHORITY) R.bypass_blocked++;
        CHECK(rc == RX_ERR_AUTHORITY, "%s: cognition cannot publish around the gate (%lld)",
              rc_kind_name((RcKind)k), (long long)rc);
        RxObject after;
        read_obj(E.pub[k], &after);
        CHECK(after.version == before.version, "%s: published object untouched", rc_kind_name((RcKind)k));
    }
    /* a cognition reaction presenting the gate's own capability */
    static RxObjRef tgt;
    tgt = E.pub[RC_PLAN];
    RxObject before;
    read_obj(tgt, &before);
    RxReactionDesc d;
    memset(&d, 0, sizeof d);
    d.name = "cognition.bypass";
    d.faculty = RX_FACULTY_AIEN;
    d.subject = SUBJ_COG;
    d.priority = RX_PRIO_FOREGROUND;
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){ E.draft[RC_SKILL_INVOCATION], RX_ALL_FIELDS };
    d.n_writes = 1;
    d.writes[0] = (RxDep){ tgt, RX_ALL_FIELDS };
    d.n_caps = 2;
    d.caps[0] = (RxCapNeed){ E.gate_pub[RC_PLAN], RES_PUB + RC_PLAN, RX_RIGHT_WRITE };
    d.caps[1] = (RxCapNeed){ E.cog_draft[RC_SKILL_INVOCATION], RES_DRAFT + RC_SKILL_INVOCATION,
                             RX_RIGHT_READ };
    d.fn = bypass_fn;
    d.user = &tgt;
    uint32_t id;
    CHECK(rx_world_add_reaction(&E.w, &d, &id) == RX_OK, "bypass reaction registered");
    uint64_t blocked0 = E.w.stats.blocked_authority;
    RcObject o;
    Rng r = { 5 };
    static Intent in;
    make_intent(RC_SKILL_INVOCATION, &r, &in);
    o = in.o;
    o.f[5] = 8;
    through_gate(RC_SKILL_INVOCATION, &o);
    R.bypass_attempts++;
    RxObject after;
    read_obj(tgt, &after);
    int blocked = E.w.stats.blocked_authority > blocked0 && after.version == before.version;
    if (blocked) R.bypass_blocked++;
    CHECK(blocked, "a stolen gate capability does not let cognition publish");
    /* every commit on every published field came from its gate */
    for (uint32_t k = 1; k < RC_KIND_COUNT; k++)
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++) {
            uint64_t c = rx_world_explain(&E.w, E.pub[k], f);
            uint32_t react, subj, fac;
            int o2 = rx_world_crumb_origin(&E.w, c, &react, &subj, &fac);
            if (o2 == 1) continue;   /* never written: creation value */
            if (o2 != RX_OK || react != E.gate[k].reaction || subj != SUBJ_GATE) R.bypasses++;
        }
    CHECK(R.bypasses == 0, "every published field was written by its gate (%u not)", R.bypasses);
}

/* The world changes between generation and publication. */
static void t_stale(void) {
    printf("[*] a reference that goes stale before publication is caught\n");
    static Intent in;
    Rng r = { 99 };
    make_intent(RC_WORLD_MUTATION, &r, &in);
    in.o.f[0] = E.target[3].id;
    in.o.f[1] = E.target[3].generation;
    in.o.f[5] = RES_T + 3;
    Synth s = { RC_WORLD_MUTATION, &in, 1234, { 0 }, GR_OK, 0, { 0, 0 } };
    s.seed = 1;
    RcBackend b = backend(&s, 1);
    RcStats st;
    memset(&st, 0, sizeof st);
    RcObject out;
    int res = rc_produce(&E.comp[RC_WORLD_MUTATION], &E.ctx, &b, 3, &out, &st);
    CHECK(res != RC_OUT_REJECTED && res != RC_OUT_UNSATISFIABLE, "mutation produced");
    CHECK(rx_world_retire(&E.w, E.target[3]) == RX_OK, "target retired");
    RcObject before;
    published(RC_WORLD_MUTATION, &before);
    uint64_t v = through_gate(RC_WORLD_MUTATION, &out);
    CHECK(v == RC_GATE_REJECTED, "the gate rejects the stale mutation (%llu)", (unsigned long long)v);
    RxObject st2;
    read_obj(E.status[RC_WORLD_MUTATION], &st2);
    CHECK(st2.field[3] & (1u << RC_RULE_GENERATION), "rejected as a generation violation");
    if (v == RC_GATE_REJECTED) R.stale_caught++;
    RcObject after;
    published(RC_WORLD_MUTATION, &after);
    CHECK(memcmp(before.f, after.f, sizeof before.f) == 0, "nothing published for it");
    /* and a fresh request can no longer even name it */
    RcDomain d;
    RcObject part = { .kind = RC_WORLD_MUTATION };
    part.f[4] = SUBJ_COG;
    part.f[5] = RES_T + 3;
    rc_domain(&E.comp[RC_WORLD_MUTATION], &E.ctx, &part, 0, &d);
    CHECK(!rc_domain_contains(&d, E.target[3].id) || d.empty,
          "the constrained path cannot choose the retired object");
}

/* No effect left: the constrained path stops before generating; the free
 * path spends every retry on candidates that cannot pass. */
static void t_budget(void) {
    printf("[*] exhausted effect budget\n");
    uint32_t saved = E.ctx.max_effects;
    pthread_mutex_lock(&E.ctx.mu);
    E.ctx.max_effects = E.ctx.effects_used;
    pthread_mutex_unlock(&E.ctx.mu);
    static Intent in;
    for (int path = 0; path < 2; path++) {
        Rng r = { 31 };
        make_intent(RC_EFFECT_PROPOSAL, &r, &in);
        Synth s = { RC_EFFECT_PROPOSAL, &in, 4321, { 0 }, GR_OK, 0, { 0, 0 } };
        RcBackend b = backend(&s, path == PATH_CONSTRAINED);
        RcStats st;
        memset(&st, 0, sizeof st);
        RcObject out;
        int res = rc_produce(&E.comp[RC_EFFECT_PROPOSAL], &E.ctx, &b, 3, &out, &st);
        if (path == PATH_CONSTRAINED) {
            CHECK(res == RC_OUT_UNSATISFIABLE && st.draws == 0,
                  "constrained: unsatisfiable, nothing generated (%d, %u draws)", res, st.draws);
            R.budget_constrained_draws = st.draws;
        } else {
            CHECK(res == RC_OUT_REJECTED && st.candidates == 4,
                  "free: every candidate rejected (%d, %u candidates)", res, st.candidates);
            R.budget_free_draws = st.draws;
            R.budget_free_candidates = st.candidates;
        }
    }
    /* the graph contract sees the same budget */
    Rng r = { 32 };
    make_intent(RC_ACTION_GRAPH, &r, &in);
    RcVerdict v;
    rc_check(&E.con[RC_ACTION_GRAPH], &E.ctx, &in.o, &v);
    CHECK(!v.ok && (v.kinds_violated & (1u << RC_RULE_EFFECT)),
          "a graph with an effect is refused when no effect is left");
    pthread_mutex_lock(&E.ctx.mu);
    E.ctx.max_effects = saved;
    pthread_mutex_unlock(&E.ctx.mu);
}

static void t_json(void) {
    printf("[*] JSON is one serialization of the typed object\n");
    static Intent in;
    char js[1024];
    uint8_t bin[256];
    for (uint32_t k = 1; k < RC_KIND_COUNT; k++) {
        for (uint32_t i = 0; i < 200; i++) {
            Rng r = { mix(0x7A50ull * k + i) };
            make_intent((RcKind)k, &r, &in);
            RcObject a = in.o, b, c;
            a.graph = NULL;
            uint8_t ia[32], ib[32], ic[32];
            rc_identity(&E.con[k], &a, ia);
            size_t n = rc_encode(&E.con[k], &a, bin, sizeof bin);
            int ok = n > 0 && rc_decode(&E.con[k], bin, n, &b) == 0;
            ok = ok && rc_to_json(&E.con[k], &a, js, sizeof js) > 0 &&
                 rc_from_json(&E.con[k], js, &c) == 0;
            rc_identity(&E.con[k], &b, ib);
            rc_identity(&E.con[k], &c, ic);
            R.json_roundtrips++;
            if (!ok || memcmp(ia, ib, 32) || memcmp(ia, ic, 32)) {
                R.json_identity_mismatch++;
                CHECK(0, "%s: binary and JSON round trips keep the identity", E.con[k].name);
            }
            /* a JSON-speaking backend: its text is parsed, then checked like any other */
            int gv;
            RcObject f = in.o;
            int cls = fault((RcKind)k, (int)(i % N_FAULTS), &f, &in, &r, &gv);
            if (cls < 0 || cls == VALID_WRONG || gv >= 0 || k == RC_ACTION_GRAPH) continue;
            RcObject parsed;
            if (rc_to_json(&E.con[k], &f, js, sizeof js) <= 0) continue;
            R.json_candidates++;
            RcVerdict v;
            if (rc_from_json(&E.con[k], js, &parsed) != 0 ||
                rc_check(&E.con[k], &E.ctx, &parsed, &v) != 0)
                R.json_caught++;
        }
    }
    CHECK(R.json_caught == R.json_candidates, "every faulty JSON candidate is caught (%u of %u)",
          R.json_caught, R.json_candidates);
    RcObject o;
    const RcContract *c = &E.con[RC_SKILL_INVOCATION];
    const char *good = "{\"kind\":\"skill_invocation\",\"skill\":11,\"arity\":1,\"arg0\":5,\"arg1\":0,"
                       "\"arg2\":0,\"max_attempts\":2,\"principal\":71}";
    CHECK(rc_from_json(c, good, &o) == 0 && o.f[0] == 11 && o.f[6] == SUBJ_COG, "well-formed parses");
    static const char *bad[] = {
        "{\"kind\":\"skill_invocation\",\"skill\":11,\"skill\":12,\"arity\":1,\"arg0\":5,\"arg1\":0,\"arg2\":0,\"max_attempts\":2,\"principal\":71}",
        "{\"kind\":\"skill_invocation\",\"skill\":11,\"arity\":1,\"arg0\":5,\"arg1\":0,\"arg2\":0,\"max_attempts\":2}",
        "{\"kind\":\"skill_invocation\",\"skill\":11,\"arity\":1,\"arg0\":5,\"arg1\":0,\"arg2\":0,\"max_attempts\":2,\"principal\":71,\"extra\":1}",
        "{\"kind\":\"skill_invocation\",\"skill\":-11,\"arity\":1,\"arg0\":5,\"arg1\":0,\"arg2\":0,\"max_attempts\":2,\"principal\":71}",
        "{\"kind\":\"skill_invocation\",\"skill\":11.0,\"arity\":1,\"arg0\":5,\"arg1\":0,\"arg2\":0,\"max_attempts\":2,\"principal\":71}",
        "{\"kind\":\"skill_invocation\",\"skill\":011,\"arity\":1,\"arg0\":5,\"arg1\":0,\"arg2\":0,\"max_attempts\":2,\"principal\":71}",
        "{\"kind\":\"skill_invocation\",\"skill\":99999999999999999999,\"arity\":1,\"arg0\":5,\"arg1\":0,\"arg2\":0,\"max_attempts\":2,\"principal\":71}",
        "{\"kind\":\"plan\",\"skill\":11,\"arity\":1,\"arg0\":5,\"arg1\":0,\"arg2\":0,\"max_attempts\":2,\"principal\":71}",
        "{\"kind\":\"skill_invocation\",\"skill\":11,\"arity\":1,\"arg0\":5,\"arg1\":0,\"arg2\":0,\"max_attempts\":2,\"principal\":71} x",
        "{\"kind\":\"skill_invocation\",\"skill\":\"11\",\"arity\":1,\"arg0\":5,\"arg1\":0,\"arg2\":0,\"max_attempts\":2,\"principal\":71}",
        "[1,2,3]",
        "",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        R.json_malformed++;
        int rc = rc_from_json(c, bad[i], &o);
        if (rc != 0) R.json_malformed_rejected++;
        CHECK(rc != 0, "malformed JSON %zu refused", i);
    }
    printf("    %u round trips, %u identity mismatches; %u malformed texts, %u refused; "
           "%u faulty JSON candidates, %u caught\n",
           R.json_roundtrips, R.json_identity_mismatch, R.json_malformed, R.json_malformed_rejected,
           R.json_candidates, R.json_caught);
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

typedef struct {
    uint64_t requests, candidates, invalid, repairs, retries, dead_ends, draws, checks, published, rejected;
    uint64_t unsat, semantic_fail, invalid_published, gen_ns, check_ns, p50, p99;
} Sum;

static Sum summarize(int path, int kind) {
    Sum s;
    memset(&s, 0, sizeof s);
    static uint64_t lat[4096 * RC_KIND_COUNT];
    uint32_t n = 0;
    for (uint32_t k = 1; k < RC_KIND_COUNT; k++) {
        if (kind && (int)k != kind) continue;
        const PathStats *p = &R.path[path][k];
        s.requests += p->requests; s.candidates += p->candidates; s.invalid += p->invalid;
        s.repairs += p->repairs; s.retries += p->retries; s.dead_ends += p->dead_ends; s.draws += p->draws;
        s.checks += p->checks; s.published += p->published;
        s.rejected += p->rejected + p->gate_rejected; s.unsat += p->unsat;
        s.semantic_fail += p->semantic_fail; s.invalid_published += p->invalid_published;
        s.gen_ns += p->gen_ns; s.check_ns += p->check_ns;
        memcpy(lat + n, p->lat, p->n_lat * sizeof lat[0]);
        n += p->n_lat;
    }
    qsort(lat, n, sizeof lat[0], cmp_u64);
    s.p50 = n ? lat[n / 2] : 0;
    s.p99 = n ? lat[(n * 99) / 100] : 0;
    return s;
}

static double ratio(uint64_t a, uint64_t b) { return b ? (double)a / (double)b : 0.0; }

static void print_sum(FILE *fp, const char *name, const Sum *s) {
    fprintf(fp,
            "\"%s\": {\"requests\": %llu, \"candidates\": %llu, \"invalid_candidates\": %llu, "
            "\"invalid_candidate_rate\": %.4f, \"repairs\": %llu, \"repair_rate\": %.4f, "
            "\"retries\": %llu, \"retries_per_request\": %.4f, \"dead_ends\": %llu, \"draws\": %llu, \"checks\": %llu, "
            "\"published\": %llu, \"rejected_after_retries\": %llu, \"unsatisfiable\": %llu, "
            "\"semantic_failures\": %llu, \"semantic_failure_rate\": %.4f, "
            "\"invalid_published\": %llu, \"generate_ns\": %llu, \"check_ns\": %llu, "
            "\"produce_mean_ns\": %llu, "
            "\"end_to_end_p50_ns\": %llu, \"end_to_end_p99_ns\": %llu}",
            name, (unsigned long long)s->requests, (unsigned long long)s->candidates,
            (unsigned long long)s->invalid, ratio(s->invalid, s->candidates),
            (unsigned long long)s->repairs, ratio(s->repairs, s->published),
            (unsigned long long)s->retries, ratio(s->retries, s->requests), (unsigned long long)s->dead_ends,
            (unsigned long long)s->draws, (unsigned long long)s->checks,
            (unsigned long long)s->published, (unsigned long long)s->rejected,
            (unsigned long long)s->unsat, (unsigned long long)s->semantic_fail,
            ratio(s->semantic_fail, s->published), (unsigned long long)s->invalid_published,
            (unsigned long long)s->gen_ns, (unsigned long long)s->check_ns,
            (unsigned long long)(s->requests ? (s->gen_ns + s->check_ns) / s->requests : 0),
            (unsigned long long)s->p50, (unsigned long long)s->p99);
}

static int gate_pass(const Sum *c, const Sum *f) {
    return g_fail == 0 && c->invalid_published == 0 && f->invalid_published == 0 &&
           R.diff_mismatch == 0 && R.sound_enforced_violations == 0 && R.reach_missed == 0 &&
           R.hostile_published_invalid == 0 && R.bypasses == 0 &&
           R.bypass_blocked == R.bypass_attempts && R.stale_caught == 1 &&
           R.json_identity_mismatch == 0 && R.json_malformed_rejected == R.json_malformed &&
           R.json_caught == R.json_candidates && R.rule_kinds_defined == RC_RULE_KIND_COUNT - 1 &&
           R.single_fault_misclassified == 0 && f->invalid > 0 && c->invalid < f->invalid &&
           c->draws < f->draws;
}

static void write_receipt(void) {
    Sum c = summarize(PATH_CONSTRAINED, 0), f = summarize(PATH_FREE, 0);
    char path[512];
    if (omega_evidence_path("TYPED_RESULTS/rx_typed_results_receipt.json", path, sizeof path) != 0)
        return;
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
    fprintf(fp, "{\n  \"schema\": \"OMEGA_TYPED_RESULT_CONSTRAINTS_V1\",\n");
    fprintf(fp, "  \"run_id\": \"%s\",\n", omega_evidence_run_id());
    fprintf(fp, "  \"candidate_commit\": %s%s%s,\n", candidate ? "\"" : "",
            candidate ? candidate : "null", candidate ? "\"" : "");
    fprintf(fp, "  \"candidate_bound\": %s,\n  \"run_commit\": \"%s\",\n  \"tree_dirty\": %s,\n",
            bound ? "true" : "false", commit, omega_evidence_tree_dirty() ? "true" : "false");
    fprintf(fp, "  \"aienos_commit\": %s%s%s,\n", aienos ? "\"" : "", aienos ? aienos : "null",
            aienos ? "\"" : "");
    fprintf(fp, "  \"checks\": %d,\n  \"failures\": %d,\n  \"test_binary_sha256\": \"%s\",\n",
            g_checks, g_fail, digest);
    fprintf(fp, "  \"host\": {\"sysname\": \"%s\", \"release\": \"%s\", \"machine\": \"%s\", "
                "\"cpus\": \"%s\"},\n", u.sysname, u.release, u.machine, R.cpus);
    fprintf(fp, "  \"scope\": \"host processor; native AIENOS authority library; resident reaction "
                "world; synthetic cognitive backend with a fixed mistake catalogue\",\n");
    fprintf(fp, "  \"backend_model\": {\"mistake_rate_pct\": %u, \"second_mistake_pct\": %u, "
                "\"max_retries\": 3, \"masked_fallback\": \"intended value if allowed, else nearest "
                "allowed\"},\n", FAULT_RATE_PCT, SECOND_FAULT_PCT);
    fprintf(fp, "  \"contracts\": {\"kinds\": %u, \"constraint_types_used\": %u, "
                "\"rules_compiled_into_generation\": %u, \"rules_checked_after\": %u},\n",
            RC_KIND_COUNT - 1, R.rule_kinds_defined, R.enforced_rules, R.post_rules);
    fprintf(fp, "  \"measures\": {\n    ");
    print_sum(fp, "constrained", &c);
    fprintf(fp, ",\n    ");
    print_sum(fp, "free", &f);
    fprintf(fp, ",\n    \"compute_avoided\": {\"draws_fraction\": %.4f, \"candidates_fraction\": %.4f, "
                "\"checks_fraction\": %.4f, \"exhausted_budget_draws_free\": %llu, "
                "\"exhausted_budget_draws_constrained\": %llu},\n",
            1.0 - ratio(c.draws, f.draws), 1.0 - ratio(c.candidates, f.candidates),
            1.0 - ratio(c.checks, f.checks), (unsigned long long)R.budget_free_draws,
            (unsigned long long)R.budget_constrained_draws);
    fprintf(fp, "    \"per_kind\": {");
    for (uint32_t k = 1; k < RC_KIND_COUNT; k++) {
        Sum kc = summarize(PATH_CONSTRAINED, (int)k), kf = summarize(PATH_FREE, (int)k);
        fprintf(fp, "%s\n      \"%s\": {\"constrained_invalid_rate\": %.4f, \"free_invalid_rate\": %.4f, "
                    "\"constrained_repair_rate\": %.4f, \"free_repair_rate\": %.4f, "
                    "\"constrained_retries\": %llu, \"free_retries\": %llu, "
                    "\"constrained_draws\": %llu, \"free_draws\": %llu, "
                    "\"constrained_semantic_failure_rate\": %.4f, \"free_semantic_failure_rate\": %.4f, "
                    "\"invalid_published\": %llu}",
                k > 1 ? "," : "", rc_kind_name((RcKind)k), ratio(kc.invalid, kc.candidates),
                ratio(kf.invalid, kf.candidates), ratio(kc.repairs, kc.published),
                ratio(kf.repairs, kf.published), (unsigned long long)kc.retries,
                (unsigned long long)kf.retries, (unsigned long long)kc.draws,
                (unsigned long long)kf.draws, ratio(kc.semantic_fail, kc.published),
                ratio(kf.semantic_fail, kf.published),
                (unsigned long long)(kc.invalid_published + kf.invalid_published));
    }
    fprintf(fp, "\n    }\n  },\n");
    fprintf(fp, "  \"differential\": {\"candidates\": %u, \"contract_vs_reference_mismatches\": %u, "
                "\"single_mistakes\": %u, \"reported_under_another_type\": %u},\n",
            R.diff_candidates, R.diff_mismatch, R.single_fault, R.single_fault_misclassified);
    fprintf(fp, "  \"caught_by_constraint_type\": {");
    for (uint32_t t = 0; t < RC_RULE_KIND_COUNT; t++)
        fprintf(fp, "%s\"%s\": %u", t ? ", " : "", rc_rule_kind_name((RcRuleKind)t), R.caught_by_kind[t]);
    fprintf(fp, "},\n");
    fprintf(fp, "  \"compiled_domains\": {\"samples\": %u, \"compiled_rule_violations\": %u, "
                "\"intent_fields_checked\": %u, \"valid_values_masked_away\": %u},\n",
            R.sound_samples, R.sound_enforced_violations, R.reach_checked, R.reach_missed);
    fprintf(fp, "  \"gate\": {\"hostile_drafts\": %u, \"rejected\": %u, \"repaired\": %u, "
                "\"published_invalid\": %u, \"bypass_attempts\": %u, \"bypass_blocked\": %u, "
                "\"fields_not_written_by_gate\": %u, \"stale_between_generation_and_publication_caught\": %u},\n",
            R.hostile_drafts, R.hostile_rejected, R.hostile_repaired, R.hostile_published_invalid,
            R.bypass_attempts, R.bypass_blocked, R.bypasses, R.stale_caught);
    fprintf(fp, "  \"serialization\": {\"round_trips\": %u, \"identity_mismatches\": %u, "
                "\"malformed_texts\": %u, \"malformed_refused\": %u, \"faulty_json_candidates\": %u, "
                "\"faulty_json_caught\": %u},\n",
            R.json_roundtrips, R.json_identity_mismatch, R.json_malformed, R.json_malformed_rejected,
            R.json_candidates, R.json_caught);
    fprintf(fp, "  \"gates\": {\n    \"OMEGA_TYPED_RESULT_CONSTRAINTS_PASS\": \"%s\",\n",
            gate_pass(&c, &f) ? "PASS" : "FAIL");
    fprintf(fp, "    \"not_claimed\": [\"a neural or LLM backend (the backend is synthetic, with a "
                "fixed mistake catalogue)\", \"AIEN's live R11 reactions routed through the gate\", "
                "\"valid-but-wrong answers (no contract can see them; they are measured as semantic "
                "failures)\", \"lookahead: a compiled domain can lead to a dead end that costs a retry\", "
                "\"graphics-processor execution\", \"AIENOS kernel (the authority runs as a host "
                "library)\"]\n  }\n}\n");
    fclose(fp);
    printf("receipt: %s\n", path);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    uint32_t n = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 400;
    place();
    if (env_start(4) != 0) {
        fprintf(stderr, "environment did not start\n");
        return 2;
    }
    t_contracts();
    seed_evidence();
    t_differential(n * 2);
    t_soundness(n);
    run_path(PATH_CONSTRAINED, n);
    run_path(PATH_FREE, n);
    t_hostile_drafts();
    t_bypass();
    t_budget();
    t_json();
    t_stale();
    Sum c = summarize(PATH_CONSTRAINED, 0), f = summarize(PATH_FREE, 0);
    printf("    constrained: invalid %.3f repair %.3f retries %llu draws %llu semantic %.4f p50 %llu ns\n",
           ratio(c.invalid, c.candidates), ratio(c.repairs, c.published), (unsigned long long)c.retries,
           (unsigned long long)c.draws, ratio(c.semantic_fail, c.published), (unsigned long long)c.p50);
    printf("    free:        invalid %.3f repair %.3f retries %llu draws %llu semantic %.4f p50 %llu ns\n",
           ratio(f.invalid, f.candidates), ratio(f.repairs, f.published), (unsigned long long)f.retries,
           (unsigned long long)f.draws, ratio(f.semantic_fail, f.published), (unsigned long long)f.p50);
    CHECK(c.invalid < f.invalid, "the constrained path generates fewer invalid candidates");
    CHECK(c.draws < f.draws, "the constrained path draws less");
    CHECK(c.invalid_published == 0 && f.invalid_published == 0, "no invalid object published");
    env_stop();
    printf("checks %d failures %d\n", g_checks, g_fail);
    write_receipt();
    return g_fail ? 1 : 0;
}
