/*
 * OMEGA_SEMANTIC_COMMUNICATION -- swarms of 32, 100 and 500 cognitive
 * workers with partially overlapping tasks, served three ways from the same
 * World at the same moment:
 *
 *   B  full-state broadcast: every live object, every field, its version and
 *      its writer crumb, to every worker, every round
 *   P  semantic projection: what each worker's InformationNeed selects, fresh
 *      every round
 *   D  projection with delta delivery: the first projection, then only
 *      changed fields, new evidence, invalidations, new conclusions and
 *      changed derived values
 *
 * Each worker then performs its operation from what it received. The answer
 * is compared with a reference computed straight from the World under the
 * worker's own authority (an evaluator written apart from rx_sem_project).
 * Every required (object, field, evidence) the worker lacks or holds stale is
 * a missing-required-information failure. Every field a worker holds that its
 * principal may not read is a leak.
 *
 * The World moves between rounds: field writes, retired and replaced
 * objects, new conclusions, a revoked and later re-granted capability. The
 * test plays the outside: it holds the AIENOS admin to give principals the
 * authority they start with, to revoke and to re-grant. Projection is given
 * the world and the receiver's capability table only.
 */
#include "runtime/aienos_cap.h"
#include "runtime/rx_semcomm.h"
#include "runtime/rx_world.h"
#include "omega_evidence.h"
#include "sha256.h"

#include <dirent.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

enum { SUBJ_EXTERNAL = 100, ISSUER = 3, SUBJ_TEAM0 = 200 };

#define N_TEAMS      16u
#define N_DOMAINS    12u
#define N_INITIAL    180u
#define N_ROUNDS     24u
#define MAX_CONCL    40u
#define RES_DOM(d)   (0xB000000ull + (d))
#define RES_TRIGGER  0xB0000F0ull
#define REVOKE_ROUND 12u
#define REGRANT_ROUND 18u
#define REVOKE_TEAM  3u

enum { TY_SENSOR = 0x51, TY_TRACK, TY_ASSET, TY_PLAN, TY_RISK, TY_CONCL };
static const uint32_t ALL_TYPES[6] = { TY_SENSOR, TY_TRACK, TY_ASSET, TY_PLAN, TY_RISK, TY_CONCL };
#define UNC_FIELD 7u
#define RISK_PRIVATE (RX_FIELD(4) | RX_FIELD(5))

enum { M_B = 0, M_P, M_D, M_COUNT };
static const char *MODE_NAME[M_COUNT] = { "full_state_broadcast", "semantic_projection",
                                          "semantic_projection_delta" };

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
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

typedef struct { uint64_t s; } Rng;
static uint64_t rnd(Rng *r) {
    uint64_t z = (r->s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static uint64_t mix(uint32_t id, uint32_t f, uint64_t v) {
    return (v * 0x9E3779B97F4A7C15ull) ^ (((uint64_t)id << 8) | f);
}

static RxSemSchema g_schema;

static void schema_init(void) {
    memset(&g_schema, 0, sizeof g_schema);
    g_schema.n = 6;
    g_schema.t[0] = (RxSemType){ TY_SENSOR, 0, (int32_t)UNC_FIELD, false };
    g_schema.t[1] = (RxSemType){ TY_TRACK, 0, (int32_t)UNC_FIELD, false };
    g_schema.t[2] = (RxSemType){ TY_ASSET, 0, -1, false };
    g_schema.t[3] = (RxSemType){ TY_PLAN, 0, -1, false };
    g_schema.t[4] = (RxSemType){ TY_RISK, RISK_PRIVATE, -1, false };
    g_schema.t[5] = (RxSemType){ TY_CONCL, 0, (int32_t)UNC_FIELD, true };
}

static const RxSemType *stype(uint32_t type) {
    for (uint32_t i = 0; i < g_schema.n; i++)
        if (g_schema.t[i].type == type) return &g_schema.t[i];
    return NULL;
}

/* ---- metrics ---- */

typedef struct {
    uint64_t bytes, messages, records;
    uint64_t sender_cpu_ns, receiver_cpu_ns;
    uint64_t round_wall_ns[N_ROUNDS];
    uint64_t receiver_mem_peak, sender_mem_peak;
    uint64_t task_ok, task_total, missing, leaks, validations;
} ModeM;

typedef struct {
    uint32_t agents;
    ModeM m[M_COUNT];
    uint64_t denied_by_authority, excluded_uncertain;
    uint64_t equiv_checked, equiv_mismatch;
    uint64_t det_checked, det_mismatch;
    uint64_t quiet_checked, quiet_nonempty;
    uint64_t inval_reason[5];
    uint64_t delta_changed_fields, delta_new_evidence, delta_new_conclusions, delta_derived;
    uint64_t conclusions_received;
    uint64_t revocation_invalidations;
    uint64_t revoke_expected, revoke_delivered;
    /* energy (µJ per scenario run; 0 = sensor unavailable) */
    int energy_ok;
    uint64_t e_runs[3], e_pkg[3], e_cpu[3], e_ns[3];   /* none, B, D */
} ScaleM;

static struct {
    ScaleM scale[3];
    uint64_t control_missing, control_agents;
    uint32_t write_attempts, write_denied, write_bypass, write_positive;
    char cpus[64];
    char energy_source[160];
    uint64_t idle_pkg_uw, idle_cpu_uw;
} R;

/* ---- environment ---- */

typedef struct {
    AienosCapAdmin *admin;
    AienosCapView *view;
    RxWorld w;
    RxCapRef ext[N_DOMAINS];
    RxCapRef ext_trigger;
    RxSemCaps team[N_TEAMS];
    RxCapRef revoked;
    uint32_t n_live;
    RxObjRef live[RX_MAX_OBJECTS];
    uint32_t n_concl;
    uint64_t base_crumb;
} Env;

static RxCapRef mint(Env *e, uint32_t subject, uint64_t resource, uint32_t rights) {
    AienosCapRef office;
    aienos_cap_office(e->admin, &office);
    AienosCapMint m = { ISSUER, subject, resource, rights, 0, { UINT32_MAX, 0 }, office };
    AienosCapRef r = { UINT32_MAX, 0 };
    if (aienos_cap_mint(e->admin, &m, &r) != 0) r = (AienosCapRef){ UINT32_MAX, 0 };
    return (RxCapRef){ r.cap_id, r.generation };
}

static void hold(RxSemCaps *c, RxCapRef ref, uint64_t res, uint32_t rights) {
    if (c->n >= SC_MAX_CAPS) return;
    c->cap[c->n].ref = ref;
    c->cap[c->n].resource = res;
    c->cap[c->n].rights = rights;
    c->n++;
}

static uint32_t team_domain(uint32_t t, uint32_t k) {
    static const uint32_t off[4] = { 0, 1, 3, 7 };
    return (t + off[k]) % N_DOMAINS;
}

static void fields_for(Rng *r, uint64_t f[RX_MAX_FIELDS]) {
    for (uint32_t i = 0; i < RX_MAX_FIELDS; i++) f[i] = rnd(r) % 1000000u;
}

static int env_start(Env *e) {
    memset(e, 0, sizeof *e);
    if (aienos_cap_start(&e->admin, &e->view) != 0) return -1;
    if (rx_world_init_native(&e->w, e->view, 1, 1u << 16) != RX_OK) return -1;
    e->w.external_subject = SUBJ_EXTERNAL;
    Rng r = { 0x5EEDu };
    for (uint32_t i = 0; i < N_INITIAL; i++) {
        uint64_t f[RX_MAX_FIELDS];
        fields_for(&r, f);
        uint32_t dom = (i / 15u) % N_DOMAINS;
        if (rx_world_create(&e->w, ALL_TYPES[i % 5u], RX_PERSIST_RESIDENT, RES_DOM(dom), f,
                            &e->live[e->n_live]) != RX_OK)
            return -1;
        e->n_live++;
    }
    for (uint32_t d = 0; d < N_DOMAINS; d++) e->ext[d] = mint(e, SUBJ_EXTERNAL, RES_DOM(d), RX_RIGHT_WRITE);
    for (uint32_t t = 0; t < N_TEAMS; t++) {
        e->team[t].subject = SUBJ_TEAM0 + t;
        for (uint32_t k = 0; k < 4; k++) {
            uint64_t res = RES_DOM(team_domain(t, k));
            RxCapRef c = mint(e, SUBJ_TEAM0 + t, res, RX_RIGHT_READ);
            hold(&e->team[t], c, res, RX_RIGHT_READ);
            if (t == REVOKE_TEAM && k == 0) e->revoked = c;
        }
        if (t % 2 == 0) {
            uint64_t pres = rx_sem_private_resource(RES_DOM(team_domain(t, 0)));
            hold(&e->team[t], mint(e, SUBJ_TEAM0 + t, pres, RX_RIGHT_READ), pres, RX_RIGHT_READ);
        }
    }
    e->base_crumb = e->w.n_crumbs;
    return 0;
}

static void env_stop(Env *e) {
    rx_world_wait_quiescent(&e->w, 30000);
    rx_world_destroy(&e->w);
    aienos_cap_stop(e->admin, e->view);
}

static uint32_t domain_of(Env *e, RxObjRef ref) {
    return (uint32_t)(e->w.objects[ref.id].resource - RES_DOM(0));
}

/* The World moves. The same sequence for every swarm size. */
static void mutate(Env *e, uint32_t round) {
    Rng r = { 0xC0FFEEull + round * 0x1000193ull };
    for (uint32_t k = 0; k < 12; k++) {
        RxObjRef ref = e->live[rnd(&r) % e->n_live];
        uint32_t nf = 1 + (uint32_t)(rnd(&r) % 3u);
        RxMutation m[3];
        for (uint32_t j = 0; j < nf; j++)
            m[j] = (RxMutation){ ref, (uint32_t)(rnd(&r) % RX_MAX_FIELDS), rnd(&r) % 1000000u };
        int64_t rc = rx_world_publish_external(&e->w, e->ext[domain_of(e, ref)], m, nf);
        CHECK(rc > 0, "outside publication accepted (%lld)", (long long)rc);
    }
    if (round % 4 == 3) {
        uint32_t idx = (uint32_t)(rnd(&r) % e->n_live);
        RxObject o;
        if (rx_world_read(&e->w, e->live[idx], &o) == RX_OK && o.type != TY_CONCL) {
            uint64_t f[RX_MAX_FIELDS];
            fields_for(&r, f);
            CHECK(rx_world_retire(&e->w, e->live[idx]) == RX_OK, "retire");
            CHECK(rx_world_create(&e->w, o.type, RX_PERSIST_RESIDENT, o.resource, f,
                                  &e->live[idx]) == RX_OK, "replace");
        }
    }
    if (round % 3 == 2 && e->n_concl < MAX_CONCL) {
        uint64_t f[RX_MAX_FIELDS];
        fields_for(&r, f);
        uint32_t dom = (uint32_t)(rnd(&r) % N_DOMAINS);
        if (rx_world_create(&e->w, TY_CONCL, RX_PERSIST_RESIDENT, RES_DOM(dom), f,
                            &e->live[e->n_live]) == RX_OK) {
            e->n_live++;
            e->n_concl++;
        }
    }
    if (round == REVOKE_ROUND) {
        AienosCapRef office;
        aienos_cap_office(e->admin, &office);
        CHECK(aienos_cap_revoke(e->admin, office,
                                (AienosCapRef){ e->revoked.cap_id, e->revoked.generation }) == 0,
              "revoke team %u READ on its home domain", REVOKE_TEAM);
    }
    if (round == REGRANT_ROUND) {
        uint64_t res = RES_DOM(team_domain(REVOKE_TEAM, 0));
        hold(&e->team[REVOKE_TEAM], mint(e, SUBJ_TEAM0 + REVOKE_TEAM, res, RX_RIGHT_READ), res,
             RX_RIGHT_READ);
    }
}

/* ---- workers ---- */

typedef struct {
    uint32_t team;
    RxInformationNeed need;
    RxSemView v[M_COUNT];
    RxSemCursor cur;
    RxSemBuf msg;
    uint64_t result;
    RxObjRef arg;
    uint32_t n_inputs;
} Agent;

static void make_need(Env *e, Agent *a, uint32_t i) {
    Rng r = { 0xA6E47ull ^ ((uint64_t)i * 0x9E3779B97F4A7C15ull) };
    a->team = i % N_TEAMS;
    RxInformationNeed *n = &a->need;
    memset(n, 0, sizeof *n);
    n->receiver = SUBJ_TEAM0 + a->team;
    if (i % 10u == 9u) {
        /* A coordinator: every type, every object, every data field, with
         * writer evidence. Broad needs keep the comparison honest. */
        n->operation.kind = SC_OP_INSPECT;
        n->operation.fields = 0x7Fu;
        n->evidence_requirement = SC_EV_WRITER;
        return;
    }
    uint32_t x = (uint32_t)(rnd(&r) % 10u);
    if (x < 5) {
        n->operation.kind = SC_OP_INSPECT;
        uint32_t k = 2 + (uint32_t)(rnd(&r) % 3u);
        for (uint32_t j = 0; j < k; j++) n->operation.fields |= RX_FIELD(rnd(&r) % 7u);
    } else {
        n->operation.kind = x < 8 ? SC_OP_SUM : SC_OP_MAX;
        n->operation.field = (uint32_t)(rnd(&r) % 7u);
    }
    n->n_types = 1 + (uint32_t)(rnd(&r) % 2u);
    n->required_types[0] = ALL_TYPES[rnd(&r) % 6u];
    if (n->n_types == 2) {
        do n->required_types[1] = ALL_TYPES[rnd(&r) % 6u];
        while (n->required_types[1] == n->required_types[0]);
    }
    if (rnd(&r) % 10u < 7) {
        /* Task groups: workers in a group look at neighbouring windows. */
        uint32_t g = (uint32_t)(rnd(&r) % 24u);
        uint32_t start = (g * 29u + (uint32_t)(rnd(&r) % 8u)) % N_INITIAL;
        uint32_t width = 12 + (uint32_t)(rnd(&r) % 37u);
        for (uint32_t j = 0; j < width && n->n_objects < SC_MAX_RELEVANT; j++)
            n->relevant_objects[n->n_objects++] = e->live[(start + j) % N_INITIAL];
    }
    if (rnd(&r) % 10u < 2) n->relevant_time_range.from_crumb = e->base_crumb + 40 + rnd(&r) % 240u;
    uint32_t ev = (uint32_t)(rnd(&r) % 20u);
    n->evidence_requirement = ev < 10 ? SC_EV_NONE : ev < 17 ? SC_EV_WRITER : SC_EV_DIGEST;
    if (rnd(&r) % 10u < 4) n->uncertainty_requirement.max_ppm = 300000 + (uint32_t)(rnd(&r) % 600000u);
    n->uncertainty_requirement.deliver = rnd(&r) % 10u < 3;
}

/* ---- reference evaluation ----
 * Applies a need to a view-shaped source under the principal's authority.
 * The truth runs it on a copy taken straight from the World; the broadcast
 * worker runs it on what it decoded. It shares no code with rx_sem_project. */

typedef struct {
    uint16_t id;
    uint8_t f;
    bool value, ev;
} Req;

typedef struct {
    uint64_t result;
    RxObjRef arg;
    uint32_t n_inputs;
    uint32_t n_req;
    Req req[RX_MAX_OBJECTS * RX_MAX_FIELDS];
    uint32_t denied, uncertain;
} Eval;

static bool authorized(Env *e, const RxSemCaps *c, uint64_t res, uint64_t *validations) {
    for (uint32_t i = 0; i < c->n; i++) {
        if (c->cap[i].resource != res) continue;
        (*validations)++;
        if (rx_world_validate_cap(&e->w, c->cap[i].ref, c->subject, res, RX_RIGHT_READ, NULL) ==
            RX_CAP_OK)
            return true;
    }
    return false;
}

static void evaluate(Env *e, const RxSemView *v, const Agent *a, Eval *out, uint64_t *validations) {
    const RxInformationNeed *n = &a->need;
    const RxSemCaps *caps = &e->team[a->team];
    memset(out, 0, offsetof(Eval, req));
    out->denied = out->uncertain = 0;
    const bool derived = n->operation.kind != SC_OP_INSPECT;
    bool first = true;
    for (uint32_t id = 0; id < RX_MAX_OBJECTS; id++) {
        if (!v->present[id]) continue;
        bool tw = n->n_types == 0;
        for (uint32_t i = 0; i < n->n_types; i++) tw |= n->required_types[i] == v->type[id];
        if (!tw) continue;
        bool ow = n->n_objects == 0;
        for (uint32_t i = 0; i < n->n_objects; i++)
            ow |= n->relevant_objects[i].id == id && n->relevant_objects[i].generation == v->gen[id];
        if (!ow) continue;
        uint64_t want = derived ? RX_FIELD(n->operation.field) : n->operation.fields;
        uint64_t sel = 0;
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++) {
            uint64_t c = v->crumb[id][f];
            bool t = c >= n->relevant_time_range.from_crumb &&
                     (n->relevant_time_range.to_crumb == 0 || c <= n->relevant_time_range.to_crumb);
            if ((want & RX_FIELD(f)) && t) sel |= RX_FIELD(f);
        }
        if (!sel) continue;
        if (!authorized(e, caps, v->resource[id], validations)) { out->denied++; continue; }
        const RxSemType *T = stype(v->type[id]);
        if (T && T->uncertainty_field >= 0 && n->uncertainty_requirement.max_ppm &&
            v->value[id][T->uncertainty_field] > n->uncertainty_requirement.max_ppm) {
            out->uncertain++;
            continue;
        }
        uint64_t unc = (T && T->uncertainty_field >= 0 && n->uncertainty_requirement.deliver)
                           ? RX_FIELD((uint32_t)T->uncertainty_field) : 0;
        uint64_t priv = T ? T->private_mask : 0;
        if (((sel | unc) & priv) &&
            !authorized(e, caps, rx_sem_private_resource(v->resource[id]), validations)) {
            sel &= ~priv;
            unc &= ~priv;
            if (!sel) continue;
        }
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++) {
            uint64_t b = RX_FIELD(f);
            if (!((sel | unc) & b)) continue;
            Req *q = &out->req[out->n_req++];
            q->id = (uint16_t)id;
            q->f = (uint8_t)f;
            q->value = derived ? (unc & b) != 0 : true;
            q->ev = n->evidence_requirement != SC_EV_NONE;
            if (!derived && (sel & b)) out->result += mix(id, f, v->value[id][f]);
        }
        if (derived) {
            uint64_t x = v->value[id][n->operation.field];
            if (n->operation.kind == SC_OP_SUM) out->result += x;
            else if (first || x > out->result) { out->result = x; out->arg = (RxObjRef){ id, v->gen[id] }; }
            first = false;
            out->n_inputs++;
        }
    }
}

/* Truth source: the World now, including crumb digests. */
static RxSemView *g_truth;

static void snapshot_truth(Env *e) {
    if (!g_truth->digest) g_truth->digest = calloc(RX_MAX_OBJECTS, sizeof *g_truth->digest);
    pthread_mutex_lock(&e->w.mu);
    for (uint32_t id = 0; id < RX_MAX_OBJECTS; id++) {
        const RxObject *o = &e->w.objects[id];
        g_truth->present[id] = o->live;
        if (!o->live) continue;
        g_truth->gen[id] = o->generation;
        g_truth->type[id] = o->type;
        g_truth->resource[id] = o->resource;
        g_truth->mask[id] = g_truth->ev_mask[id] = RX_ALL_FIELDS;
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++) {
            g_truth->value[id][f] = o->field[f];
            g_truth->version[id][f] = o->field_version[f];
            g_truth->crumb[id][f] = o->field_writer[f];
            const RxCrumb *c = rx_world_crumb(&e->w, o->field_writer[f]);
            if (c) memcpy(g_truth->digest[id][f], c->digest, 32);
            else memset(g_truth->digest[id][f], 0, 32);
        }
    }
    pthread_mutex_unlock(&e->w.mu);
}

/* Required information the worker lacks or holds stale. */
static uint32_t missing_in(const RxSemView *v, const Eval *t, uint32_t ev_mode, bool check_digest) {
    uint32_t miss = 0;
    for (uint32_t i = 0; i < t->n_req; i++) {
        const Req *q = &t->req[i];
        uint32_t id = q->id, f = q->f;
        if (!v->present[id] || v->gen[id] != g_truth->gen[id]) { miss++; continue; }
        if (q->value && (!(v->mask[id] & RX_FIELD(f)) || v->value[id][f] != g_truth->value[id][f] ||
                         v->version[id][f] != g_truth->version[id][f])) { miss++; continue; }
        if (q->ev) {
            if (!(v->ev_mask[id] & RX_FIELD(f)) || v->crumb[id][f] != g_truth->crumb[id][f]) { miss++; continue; }
            if (check_digest && ev_mode == SC_EV_DIGEST &&
                (!v->digest || memcmp(v->digest[id][f], g_truth->digest[id][f], 32) != 0)) { miss++; continue; }
        }
    }
    return miss;
}

/* Fields a worker holds that its principal may not read. */
static uint32_t leaks_in(Env *e, const RxSemView *v, const Agent *a) {
    uint32_t leaks = 0;
    uint64_t scratch = 0;
    const RxSemCaps *caps = &e->team[a->team];
    for (uint32_t id = 0; id < RX_MAX_OBJECTS; id++) {
        if (!v->present[id]) continue;
        uint64_t held = v->mask[id];
        if (!authorized(e, caps, v->resource[id], &scratch)) {
            leaks += (uint32_t)__builtin_popcountll(held);
            continue;
        }
        const RxSemType *T = stype(v->type[id]);
        if (T && (held & T->private_mask) &&
            !authorized(e, caps, rx_sem_private_resource(v->resource[id]), &scratch))
            leaks += (uint32_t)__builtin_popcountll(held & T->private_mask);
    }
    return leaks;
}

/* The worker's operation, from what it holds. */
static void act_projection(Agent *a, const RxSemView *v) {
    a->result = 0;
    a->arg = (RxObjRef){ 0, 0 };
    a->n_inputs = 0;
    if (a->need.operation.kind != SC_OP_INSPECT) {
        if (v->n_derived) {
            a->result = v->derived[0].value;
            a->arg = v->derived[0].arg;
            a->n_inputs = v->derived[0].n_inputs;
        }
        return;
    }
    for (uint32_t id = 0; id < RX_MAX_OBJECTS; id++) {
        if (!v->present[id]) continue;
        uint64_t m = v->mask[id] & a->need.operation.fields;
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++)
            if (m & RX_FIELD(f)) a->result += mix(id, f, v->value[id][f]);
    }
}

static bool answer_ok(const Agent *a, const Eval *t) {
    if (a->result != t->result) return false;
    if (a->need.operation.kind == SC_OP_INSPECT) return true;
    if (a->n_inputs != t->n_inputs) return false;
    if (a->need.operation.kind == SC_OP_MAX && t->n_inputs &&
        (a->arg.id != t->arg.id || a->arg.generation != t->arg.generation))
        return false;
    return true;
}

typedef struct { Env *e; Agent *a; ScaleM *S; } ReasonCtx;

static uint32_t reason_why(ReasonCtx *c, RxObjRef gone) {
    Env *e = c->e;
    const RxObject *o = &e->w.objects[gone.id];
    if (!o->live || o->generation != gone.generation) return SC_INV_RETIRED;
    uint64_t scratch = 0;
    if (!authorized(e, &e->team[c->a->team], o->resource, &scratch)) return SC_INV_AUTHORITY;
    const RxSemType *T = stype(o->type);
    uint32_t max = c->a->need.uncertainty_requirement.max_ppm;
    if (T && T->uncertainty_field >= 0 && max && o->field[T->uncertainty_field] > max)
        return SC_INV_UNCERTAIN;
    return SC_INV_SCOPE;
}

static uint32_t reason_of(void *ctx, RxObjRef gone) {
    ReasonCtx *c = ctx;
    uint32_t why = reason_why(c, gone);
    if (c->S && why < 5) c->S->inval_reason[why]++;
    return why;
}

/* ---- one scenario ---- */

static Eval *g_eval;
static RxSemanticProjection *g_proj;

static void run_scale(uint32_t n_agents, uint32_t mode_mask, int verify, ScaleM *S) {
    Env *e = calloc(1, sizeof *e);
    Agent *ag = calloc(n_agents, sizeof *ag);
    RxSemBuf bcast = { 0 };
    CHECK(e && ag && env_start(e) == 0, "environment for %u workers", n_agents);
    if (!e || !ag) exit(2);
    for (uint32_t i = 0; i < n_agents; i++) {
        make_need(e, &ag[i], i);
        for (uint32_t m = 0; m < M_COUNT; m++) rx_sem_view_init(&ag[i].v[m]);
    }
    uint64_t seq = 0;
    uint32_t *inv_before = calloc(n_agents, sizeof *inv_before);
    bool *held_revoked = calloc(n_agents, sizeof *held_revoked);
    const uint64_t revoked_res = RES_DOM(team_domain(REVOKE_TEAM, 0));
    for (uint32_t round = 0; round < N_ROUNDS; round++) {
        if (verify && round == REVOKE_ROUND) {
            /* Who holds something the revocation takes away. */
            for (uint32_t i = 0; i < n_agents; i++) {
                const RxSemView *v = &ag[i].v[M_D];
                inv_before[i] = v->invalidations_seen;
                held_revoked[i] = false;
                if (ag[i].team != REVOKE_TEAM) continue;
                for (uint32_t id = 0; id < RX_MAX_OBJECTS; id++)
                    held_revoked[i] |= v->present[id] && v->resource[id] == revoked_res;
            }
        }
        mutate(e, round);
        seq++;
        if (verify) snapshot_truth(e);
        for (uint32_t m = 0; m < M_COUNT; m++) {
            if (!(mode_mask & (1u << m))) continue;
            ModeM *M = &S->m[m];
            uint64_t w0 = now_ns(), c0 = cpu_ns();
            /* Sender. */
            uint64_t sent_mem = 0;
            if (m == M_B) {
                rx_sem_encode_full_state(&e->w, 0, seq, &bcast);
                M->bytes += bcast.len * n_agents;
                M->messages += n_agents;
                sent_mem = bcast.len;
            } else {
                for (uint32_t i = 0; i < n_agents; i++) {
                    Agent *a = &ag[i];
                    rx_sem_project(&e->w, &g_schema, &a->need, &e->team[a->team], g_proj);
                    M->validations += g_proj->validations;
                    if (m == M_P) {
                        rx_sem_encode_projection(g_proj, a->need.receiver, seq, &a->msg);
                    } else {
                        ReasonCtx rc = { e, a, S };
                        RxSemDeltaStats st;
                        rx_sem_delta(&a->cur, g_proj, a->need.receiver, seq, reason_of, &rc, &a->msg, &st);
                        if (round > 0) {
                            S->delta_changed_fields += st.changed_fields;
                            S->delta_new_evidence += st.new_evidence;
                            S->delta_new_conclusions += st.new_conclusions;
                            S->delta_derived += st.derived_changed;
                        }
                        sent_mem += rx_sem_cursor_bytes(&a->cur);
                    }
                    if (verify && m == M_P && round == 0) {
                        S->denied_by_authority += g_proj->withheld_authority;
                        S->excluded_uncertain += g_proj->excluded_uncertain;
                    }
                    M->bytes += a->msg.len;
                    M->messages++;
                    if (m == M_P && a->msg.len > sent_mem) sent_mem = a->msg.len;
                }
            }
            uint64_t c1 = cpu_ns();
            /* Receivers. */
            uint64_t held = 0;
            for (uint32_t i = 0; i < n_agents; i++) {
                Agent *a = &ag[i];
                const RxSemBuf *msg = m == M_B ? &bcast : &a->msg;
                int rc = rx_sem_view_apply(&a->v[m], msg->buf, msg->len);
                if (verify) CHECK(rc == SC_OK, "decode %s worker %u round %u", MODE_NAME[m], i, round);
                if (m == M_B) {
                    evaluate(e, &a->v[m], a, g_eval, &M->validations);
                    a->result = g_eval->result;
                    a->arg = g_eval->arg;
                    a->n_inputs = g_eval->n_inputs;
                } else {
                    act_projection(a, &a->v[m]);
                }
                held += rx_sem_view_bytes(&a->v[m]);
            }
            uint64_t c2 = cpu_ns(), w1 = now_ns();
            M->sender_cpu_ns += c1 - c0;
            M->receiver_cpu_ns += c2 - c1;
            M->round_wall_ns[round] = w1 - w0;
            if (held > M->receiver_mem_peak) M->receiver_mem_peak = held;
            if (sent_mem > M->sender_mem_peak) M->sender_mem_peak = sent_mem;

            if (!verify) continue;
            /* Judge every worker against the World. */
            for (uint32_t i = 0; i < n_agents; i++) {
                Agent *a = &ag[i];
                uint64_t scratch = 0;
                Agent answer = *a;   /* keep the worker's answer; evaluate() below is the judge */
                evaluate(e, g_truth, a, g_eval, &scratch);
                uint32_t miss = missing_in(&a->v[m], g_eval, a->need.evidence_requirement, m != M_B);
                bool derived_ok = true;
                if (m != M_B && a->need.operation.kind != SC_OP_INSPECT) derived_ok = answer_ok(&answer, g_eval);
                if (!derived_ok) miss++;
                M->missing += miss;
                M->leaks += leaks_in(e, &a->v[m], a);
                M->task_total++;
                if (miss == 0 && answer_ok(&answer, g_eval)) M->task_ok++;
            }
        }
        if (!verify) continue;
        /* A view rebuilt from deltas holds exactly the fresh projection. */
        if ((mode_mask & (1u << M_P)) && (mode_mask & (1u << M_D))) {
            for (uint32_t i = 0; i < n_agents; i++) {
                uint8_t dp[32], dd[32];
                rx_sem_view_digest(&ag[i].v[M_P], dp);
                rx_sem_view_digest(&ag[i].v[M_D], dd);
                S->equiv_checked++;
                if (memcmp(dp, dd, 32) != 0) S->equiv_mismatch++;
            }
        }
        if (round == REVOKE_ROUND && (mode_mask & (1u << M_D))) {
            for (uint32_t i = 0; i < n_agents; i++) {
                if (ag[i].team == REVOKE_TEAM)
                    S->revocation_invalidations += ag[i].v[M_D].invalidations_seen - inv_before[i];
                if (!held_revoked[i]) continue;
                S->revoke_expected++;
                bool still = false;
                for (uint32_t id = 0; id < RX_MAX_OBJECTS; id++)
                    still |= ag[i].v[M_D].present[id] && ag[i].v[M_D].resource[id] == revoked_res;
                if (!still && ag[i].v[M_D].invalidations_seen > inv_before[i]) S->revoke_delivered++;
            }
        }
        /* Same need, same World: identical bytes, whatever order the capabilities are held in. */
        if (round == 0 || round == N_ROUNDS / 2) {
            RxSemBuf b1 = { 0 }, b2 = { 0 };
            for (uint32_t i = 0; i < n_agents; i++) {
                Agent *a = &ag[i];
                RxSemCaps rev = e->team[a->team];
                for (uint32_t k = 0; k < rev.n / 2; k++) {
                    __typeof__(rev.cap[0]) t = rev.cap[k];
                    rev.cap[k] = rev.cap[rev.n - 1 - k];
                    rev.cap[rev.n - 1 - k] = t;
                }
                rx_sem_project(&e->w, &g_schema, &a->need, &e->team[a->team], g_proj);
                rx_sem_encode_projection(g_proj, a->need.receiver, seq, &b1);
                rx_sem_project(&e->w, &g_schema, &a->need, &rev, g_proj);
                rx_sem_encode_projection(g_proj, a->need.receiver, seq, &b2);
                S->det_checked++;
                if (b1.len != b2.len || memcmp(b1.buf, b2.buf, b1.len) != 0) S->det_mismatch++;
            }
            rx_sem_buf_free(&b1);
            rx_sem_buf_free(&b2);
        }
    }
    if (verify) {
        /* Nothing moved: every delta is an empty header. */
        for (uint32_t i = 0; i < n_agents; i++) {
            Agent *a = &ag[i];
            rx_sem_project(&e->w, &g_schema, &a->need, &e->team[a->team], g_proj);
            RxSemDeltaStats st;
            rx_sem_delta(&a->cur, g_proj, a->need.receiver, seq + 1, NULL, NULL, &a->msg, &st);
            S->quiet_checked++;
            if (a->msg.len != 21) S->quiet_nonempty++;
            S->conclusions_received += a->v[M_D].conclusions_seen;
        }
    }
    free(inv_before);
    free(held_revoked);
    for (uint32_t i = 0; i < n_agents; i++) {
        for (uint32_t m = 0; m < M_COUNT; m++) rx_sem_view_free(&ag[i].v[m]);
        rx_sem_buf_free(&ag[i].msg);
    }
    rx_sem_buf_free(&bcast);
    env_stop(e);
    free(ag);
    free(e);
}

/* ---- negative control: an under-declared need must show up as missing ---- */

static void t_control(void) {
    Env *e = calloc(1, sizeof *e);
    CHECK(e && env_start(e) == 0, "control environment");
    mutate(e, 0);
    snapshot_truth(e);
    RxSemView *v = calloc(1, sizeof *v);
    RxSemBuf b = { 0 };
    for (uint32_t i = 0; i < 64; i++) {
        Agent a;
        memset(&a, 0, sizeof a);
        make_need(e, &a, i);
        if (a.need.n_types != 2) continue;
        uint64_t scratch = 0;
        evaluate(e, g_truth, &a, g_eval, &scratch);
        if (g_eval->n_req == 0) continue;
        uint32_t second = a.need.required_types[1];
        bool second_used = false;
        for (uint32_t k = 0; k < g_eval->n_req; k++)
            second_used |= g_truth->type[g_eval->req[k].id] == second;
        if (!second_used) continue;
        RxInformationNeed under = a.need;
        under.n_types = 1;                       /* forgot one type it needs */
        rx_sem_project(&e->w, &g_schema, &under, &e->team[a.team], g_proj);
        rx_sem_encode_projection(g_proj, under.receiver, 1, &b);
        rx_sem_view_init(v);
        rx_sem_view_apply(v, b.buf, b.len);
        R.control_missing += missing_in(v, g_eval, a.need.evidence_requirement, true);
        R.control_agents++;
        rx_sem_view_free(v);
    }
    CHECK(R.control_agents > 0 && R.control_missing > 0,
          "an under-declared need is caught as missing information (%llu over %llu workers)",
          (unsigned long long)R.control_missing, (unsigned long long)R.control_agents);
    rx_sem_buf_free(&b);
    free(v);
    env_stop(e);
    free(e);
}

/* ---- receiving a projection gives no write authority ---- */

static int rx_write_fn(RxCtx *ctx) {
    const RxObjRef *target = ctx->user;
    ctx->out[0] = (RxMutation){ *target, 0, 424242 };
    ctx->n_out = 1;
    return 0;
}

static void t_write_authority(void) {
    Env *e = calloc(1, sizeof *e);
    CHECK(e && env_start(e) == 0, "authority environment");
    const uint32_t team = 1, writer_team = 2;
    const uint32_t dom = team_domain(team, 0);
    uint64_t zero[RX_MAX_FIELDS] = { 0 };
    RxObjRef trig;
    CHECK(rx_world_create(&e->w, 0xF0, RX_PERSIST_RESIDENT, RES_TRIGGER, zero, &trig) == RX_OK, "trigger");
    RxCapRef ext_trig = mint(e, SUBJ_EXTERNAL, RES_TRIGGER, RX_RIGHT_WRITE);
    RxCapRef team_trig = mint(e, SUBJ_TEAM0 + team, RES_TRIGGER, RX_RIGHT_READ);
    RxCapRef writer_trig = mint(e, SUBJ_TEAM0 + writer_team, RES_TRIGGER, RX_RIGHT_READ);

    /* The worker receives a projection of its home domain. */
    Agent a;
    memset(&a, 0, sizeof a);
    a.team = team;
    a.need.receiver = SUBJ_TEAM0 + team;
    a.need.operation.kind = SC_OP_INSPECT;
    a.need.operation.fields = RX_FIELD(0);
    rx_sem_project(&e->w, &g_schema, &a.need, &e->team[team], g_proj);
    RxObjRef target = { UINT32_MAX, 0 };
    for (uint32_t k = 0; k < g_proj->n_objects; k++)
        if (g_proj->object_refs[k].resource == RES_DOM(dom)) { target = g_proj->object_refs[k].ref; break; }
    CHECK(target.id != UINT32_MAX, "projection names an object in the worker's domain");
    RxObject before;
    rx_world_read(&e->w, target, &before);
    RxCapRef read_cap = e->team[team].cap[0].ref;

    /* 1. Publish with what it holds. */
    R.write_attempts++;
    RxMutation m = { target, 0, 999 };
    int64_t rc = rx_world_publish_external(&e->w, read_cap, &m, 1);
    if (rc < 0) R.write_denied++;

    /* 2. A reaction that claims WRITE on the READ capability it holds. */
    RxReactionDesc d;
    memset(&d, 0, sizeof d);
    d.name = "projection-holder-writes";
    d.faculty = RX_FACULTY_AIEN;
    d.subject = SUBJ_TEAM0 + team;
    d.priority = RX_PRIO_FOREGROUND;
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){ trig, RX_FIELD(0) };
    d.n_writes = 1;
    d.writes[0] = (RxDep){ target, RX_FIELD(0) };
    d.n_caps = 2;
    d.caps[0] = (RxCapNeed){ team_trig, RES_TRIGGER, RX_RIGHT_READ };
    d.caps[1] = (RxCapNeed){ read_cap, RES_DOM(dom), RX_RIGHT_WRITE };
    d.fn = rx_write_fn;
    d.user = &target;
    uint32_t rid;
    R.write_attempts++;
    CHECK(rx_world_add_reaction(&e->w, &d, &rid) == RX_OK, "reaction registered (authority is checked when it runs)");
    uint64_t blocked0 = e->w.stats.blocked_authority;
    RxMutation tm = { trig, 0, 1 };
    CHECK(rx_world_publish_external(&e->w, ext_trig, &tm, 1) > 0, "trigger");
    rx_world_wait_quiescent(&e->w, 30000);
    if (e->w.stats.blocked_authority > blocked0) R.write_denied++;

    /* 3. The same reaction declared honestly, with READ only, is refused outright. */
    RxReactionDesc d3 = d;
    d3.caps[1].rights = RX_RIGHT_READ;
    uint32_t rid3;
    R.write_attempts++;
    if (rx_world_add_reaction(&e->w, &d3, &rid3) != RX_OK) R.write_denied++;

    RxObject after;
    rx_world_read(&e->w, target, &after);
    if (after.field[0] != before.field[0]) R.write_bypass++;
    CHECK(R.write_denied == R.write_attempts && R.write_bypass == 0,
          "holding a projection gives no write (%u of %u denied, %u bypasses)", R.write_denied,
          R.write_attempts, R.write_bypass);

    /* Positive control: a principal that really holds WRITE does write. */
    RxCapRef wcap = mint(e, SUBJ_TEAM0 + writer_team, RES_DOM(dom), RX_RIGHT_WRITE);
    RxReactionDesc d4 = d;
    d4.name = "real-writer";
    d4.subject = SUBJ_TEAM0 + writer_team;
    d4.caps[0] = (RxCapNeed){ writer_trig, RES_TRIGGER, RX_RIGHT_READ };
    d4.caps[1] = (RxCapNeed){ wcap, RES_DOM(dom), RX_RIGHT_WRITE };
    uint32_t rid4;
    CHECK(rx_world_add_reaction(&e->w, &d4, &rid4) == RX_OK, "writer registered");
    tm.value = 2;
    CHECK(rx_world_publish_external(&e->w, ext_trig, &tm, 1) > 0, "trigger again");
    rx_world_wait_quiescent(&e->w, 30000);
    rx_world_read(&e->w, target, &after);
    R.write_positive = after.field[0] == 424242;
    CHECK(R.write_positive, "a principal holding WRITE commits (control)");
    env_stop(e);
    free(e);
}

/* ---- energy ---- */

static char g_hwmon[96];

static int find_energy(void) {
    DIR *dir = opendir("/sys/class/hwmon");
    if (!dir) return 0;
    struct dirent *de;
    int ok = 0;
    while ((de = readdir(dir))) {
        if (de->d_name[0] == '.') continue;
        char path[512], name[64] = { 0 };
        snprintf(path, sizeof path, "/sys/class/hwmon/%.64s/name", de->d_name);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        if (fgets(name, sizeof name, fp) && strncmp(name, "aien_spbm", 9) == 0) {
            snprintf(g_hwmon, sizeof g_hwmon, "/sys/class/hwmon/%.64s", de->d_name);
            ok = 1;
        }
        fclose(fp);
        if (ok) break;
    }
    closedir(dir);
    return ok;
}

static uint64_t read_energy(const char *label) {
    for (int i = 1; i <= 8; i++) {
        char path[512], buf[64] = { 0 };
        snprintf(path, sizeof path, "%s/energy%d_label", g_hwmon, i);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        int match = fgets(buf, sizeof buf, fp) && strncmp(buf, label, strlen(label)) == 0 &&
                    (buf[strlen(label)] == '\n' || buf[strlen(label)] == 0);
        fclose(fp);
        if (!match) continue;
        snprintf(path, sizeof path, "%s/energy%d_input", g_hwmon, i);
        fp = fopen(path, "r");
        unsigned long long v = 0;
        if (fp) { if (fscanf(fp, "%llu", &v) != 1) v = 0; fclose(fp); }
        return v;
    }
    return 0;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* Scenario runs per block until at least `min_ns` passed. Modes interleave
 * (none, B, D, then D, B, none, ...) so drift does not favour one. */
static void measure_energy(uint32_t n_agents, ScaleM *S) {
    const int reps = 4;
    const uint64_t min_ns = 1500000000ull;
    uint64_t pkg[3][8], cpu[3][8], ns[3][8], runs[3][8];
    static ScaleM scratch;
    for (int rep = 0; rep < reps; rep++) {
        for (int j = 0; j < 3; j++) {
            int k = (rep % 2 == 0) ? j : 2 - j;         /* 0 none, 1 B, 2 D */
            uint32_t mask = k == 0 ? 0 : k == 1 ? (1u << M_B) : (1u << M_D);
            uint64_t p0 = read_energy("pkg"), c0 = read_energy("cpu_p"), t0 = now_ns();
            uint64_t n = 0;
            do {
                memset(&scratch, 0, sizeof scratch);
                run_scale(n_agents, mask, 0, &scratch);
                n++;
            } while (now_ns() - t0 < min_ns);
            uint64_t t1 = now_ns(), p1 = read_energy("pkg"), c1 = read_energy("cpu_p");
            pkg[k][rep] = (p1 - p0) / n;
            cpu[k][rep] = (c1 - c0) / n;
            ns[k][rep] = (t1 - t0) / n;
            runs[k][rep] = n;
        }
    }
    for (int k = 0; k < 3; k++) {
        qsort(pkg[k], reps, sizeof(uint64_t), cmp_u64);
        qsort(cpu[k], reps, sizeof(uint64_t), cmp_u64);
        qsort(ns[k], reps, sizeof(uint64_t), cmp_u64);
        S->e_pkg[k] = (pkg[k][1] + pkg[k][2]) / 2;
        S->e_cpu[k] = (cpu[k][1] + cpu[k][2]) / 2;
        S->e_ns[k] = (ns[k][1] + ns[k][2]) / 2;
        S->e_runs[k] = 0;
        for (int r = 0; r < reps; r++) S->e_runs[k] += runs[k][r];
    }
    S->energy_ok = 1;
}

static void idle_power(void) {
    uint64_t p0 = read_energy("pkg"), c0 = read_energy("cpu_p"), t0 = now_ns();
    struct timespec ts = { 1, 0 };
    nanosleep(&ts, NULL);
    uint64_t t1 = now_ns(), p1 = read_energy("pkg"), c1 = read_energy("cpu_p");
    double s = (double)(t1 - t0) / 1e9;
    R.idle_pkg_uw = (uint64_t)((double)(p1 - p0) / s);
    R.idle_cpu_uw = (uint64_t)((double)(c1 - c0) / s);
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

static uint64_t median_wall(const ModeM *M) {
    uint64_t v[N_ROUNDS];
    memcpy(v, M->round_wall_ns, sizeof v);
    qsort(v, N_ROUNDS, sizeof(uint64_t), cmp_u64);
    return v[N_ROUNDS / 2];
}

static uint64_t max_wall(const ModeM *M) {
    uint64_t x = 0;
    for (uint32_t i = 0; i < N_ROUNDS; i++) if (M->round_wall_ns[i] > x) x = M->round_wall_ns[i];
    return x;
}

static int scale_pass(const ScaleM *S) {
    const ModeM *B = &S->m[M_B], *P = &S->m[M_P], *D = &S->m[M_D];
    return P->bytes < B->bytes && D->bytes < P->bytes && P->missing == 0 && D->missing == 0 &&
           B->missing == 0 && P->leaks == 0 && D->leaks == 0 && D->task_ok == D->task_total &&
           P->task_ok == P->task_total && B->task_ok == B->task_total &&
           S->equiv_mismatch == 0 && S->det_mismatch == 0 && S->quiet_nonempty == 0;
}

static void write_receipt(int gate) {
    char path[512];
    if (omega_evidence_path("SEMANTIC_COMM/rx_semantic_comm_receipt.json", path, sizeof path) != 0) return;
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
    fprintf(fp, "{\n  \"schema\": \"OMEGA_SEMANTIC_COMMUNICATION_V1\",\n");
    fprintf(fp, "  \"run_id\": \"%s\",\n", omega_evidence_run_id());
    fprintf(fp, "  \"candidate_commit\": %s%s%s,\n", candidate ? "\"" : "", candidate ? candidate : "null",
            candidate ? "\"" : "");
    fprintf(fp, "  \"candidate_bound\": %s,\n", bound ? "true" : "false");
    fprintf(fp, "  \"run_commit\": \"%s\",\n  \"tree_dirty\": %s,\n", commit,
            omega_evidence_tree_dirty() ? "true" : "false");
    fprintf(fp, "  \"aienos_commit\": %s%s%s,\n", aienos ? "\"" : "", aienos ? aienos : "null",
            aienos ? "\"" : "");
    fprintf(fp, "  \"checks\": %d,\n  \"failures\": %d,\n", g_checks, g_fail);
    fprintf(fp, "  \"test_binary_sha256\": \"%s\",\n", digest);
    fprintf(fp, "  \"host\": {\"sysname\": \"%s\", \"release\": \"%s\", \"machine\": \"%s\", \"cpus\": \"%s\"},\n",
            u.sysname, u.release, u.machine, R.cpus);
    fprintf(fp, "  \"scope\": \"host processor; native AIENOS authority library; resident reaction world; "
                "one sender thread; delivery is in-process decode of real encoded bytes\",\n");
    fprintf(fp, "  \"baseline\": \"full_state_broadcast = every live object, every field, its version and its "
                "writer crumb id, to every worker every round (no crumb digests, which favours the baseline)\",\n");
    fprintf(fp, "  \"world\": {\"initial_objects\": %u, \"types\": 6, \"domains\": %u, \"teams\": %u, "
                "\"rounds\": %u, \"writes_per_round\": 12, \"retire_replace_every\": 4, "
                "\"new_conclusion_every\": 3, \"revoke_round\": %u, \"regrant_round\": %u},\n",
            N_INITIAL, N_DOMAINS, N_TEAMS, N_ROUNDS, REVOKE_ROUND, REGRANT_ROUND);
    fprintf(fp, "  \"scales\": [\n");
    for (int s = 0; s < 3; s++) {
        const ScaleM *S = &R.scale[s];
        fprintf(fp, "    {\"agents\": %u, \"pass\": %s,\n", S->agents, scale_pass(S) ? "true" : "false");
        for (int m = 0; m < M_COUNT; m++) {
            const ModeM *M = &S->m[m];
            fprintf(fp,
                    "     \"%s\": {\"bytes\": %llu, \"messages\": %llu, \"sender_cpu_ns\": %llu, "
                    "\"receiver_cpu_ns\": %llu, \"round_latency_ns_median\": %llu, \"round_latency_ns_max\": %llu, "
                    "\"receiver_memory_peak_bytes\": %llu, \"sender_memory_peak_bytes\": %llu, "
                    "\"authority_validations\": %llu, \"task_success\": %llu, \"tasks\": %llu, "
                    "\"missing_required_information\": %llu, \"fields_delivered_without_authority\": %llu},\n",
                    MODE_NAME[m], (unsigned long long)M->bytes, (unsigned long long)M->messages,
                    (unsigned long long)M->sender_cpu_ns, (unsigned long long)M->receiver_cpu_ns,
                    (unsigned long long)median_wall(M), (unsigned long long)max_wall(M),
                    (unsigned long long)M->receiver_mem_peak, (unsigned long long)M->sender_mem_peak,
                    (unsigned long long)M->validations, (unsigned long long)M->task_ok,
                    (unsigned long long)M->task_total, (unsigned long long)M->missing,
                    (unsigned long long)M->leaks);
        }
        fprintf(fp,
                "     \"bytes_ratio_broadcast_over_delta\": %.2f, \"bytes_ratio_broadcast_over_projection\": %.2f,\n"
                "     \"withheld_by_authority_round0\": %llu, \"excluded_by_uncertainty_round0\": %llu,\n"
                "     \"delta\": {\"changed_fields\": %llu, \"new_evidence\": %llu, \"new_conclusions\": %llu, "
                "\"derived_changed\": %llu, \"conclusions_received\": %llu, \"revoked_team_invalidations\": %llu, "
                "\"revocation_expected_workers\": %llu, \"revocation_told_workers\": %llu},\n"
                "     \"invalidation_reasons\": {\"retired\": %llu, \"authority\": %llu, \"uncertain\": %llu, \"scope\": %llu},\n"
                "     \"equivalence\": {\"checked\": %llu, \"mismatches\": %llu}, "
                "\"determinism\": {\"checked\": %llu, \"mismatches\": %llu}, "
                "\"quiet_round\": {\"checked\": %llu, \"nonempty\": %llu},\n",
                (double)S->m[M_B].bytes / (double)(S->m[M_D].bytes ? S->m[M_D].bytes : 1),
                (double)S->m[M_B].bytes / (double)(S->m[M_P].bytes ? S->m[M_P].bytes : 1),
                (unsigned long long)S->denied_by_authority, (unsigned long long)S->excluded_uncertain,
                (unsigned long long)S->delta_changed_fields, (unsigned long long)S->delta_new_evidence,
                (unsigned long long)S->delta_new_conclusions, (unsigned long long)S->delta_derived,
                (unsigned long long)S->conclusions_received, (unsigned long long)S->revocation_invalidations,
                (unsigned long long)S->revoke_expected, (unsigned long long)S->revoke_delivered,
                (unsigned long long)S->inval_reason[SC_INV_RETIRED], (unsigned long long)S->inval_reason[SC_INV_AUTHORITY],
                (unsigned long long)S->inval_reason[SC_INV_UNCERTAIN], (unsigned long long)S->inval_reason[SC_INV_SCOPE],
                (unsigned long long)S->equiv_checked, (unsigned long long)S->equiv_mismatch,
                (unsigned long long)S->det_checked, (unsigned long long)S->det_mismatch,
                (unsigned long long)S->quiet_checked, (unsigned long long)S->quiet_nonempty);
        if (S->energy_ok)
            fprintf(fp,
                    "     \"energy_uJ_per_scenario_run\": {\"source\": \"%s\", \"method\": \"median of 4 interleaved "
                    "blocks, each >=1.5 s; whole scenario incl. world setup; raw, nothing subtracted\",\n"
                    "       \"world_only_no_messages\": {\"pkg\": %llu, \"cpu_p\": %llu, \"wall_ns\": %llu, \"runs\": %llu},\n"
                    "       \"full_state_broadcast\": {\"pkg\": %llu, \"cpu_p\": %llu, \"wall_ns\": %llu, \"runs\": %llu},\n"
                    "       \"semantic_projection_delta\": {\"pkg\": %llu, \"cpu_p\": %llu, \"wall_ns\": %llu, \"runs\": %llu}}}%s\n",
                    R.energy_source, (unsigned long long)S->e_pkg[0], (unsigned long long)S->e_cpu[0],
                    (unsigned long long)S->e_ns[0], (unsigned long long)S->e_runs[0],
                    (unsigned long long)S->e_pkg[1], (unsigned long long)S->e_cpu[1],
                    (unsigned long long)S->e_ns[1], (unsigned long long)S->e_runs[1],
                    (unsigned long long)S->e_pkg[2], (unsigned long long)S->e_cpu[2],
                    (unsigned long long)S->e_ns[2], (unsigned long long)S->e_runs[2], s < 2 ? "," : "");
        else
            fprintf(fp, "     \"energy_uJ_per_scenario_run\": \"unavailable: no aien_spbm energy sensor on this host\"}%s\n",
                    s < 2 ? "," : "");
    }
    fprintf(fp, "  ],\n");
    fprintf(fp, "  \"idle_power_uW\": {\"pkg\": %llu, \"cpu_p\": %llu},\n", (unsigned long long)R.idle_pkg_uw,
            (unsigned long long)R.idle_cpu_uw);
    fprintf(fp, "  \"negative_control\": {\"under_declared_workers\": %llu, \"missing_detected\": %llu},\n",
            (unsigned long long)R.control_agents, (unsigned long long)R.control_missing);
    fprintf(fp, "  \"write_authority\": {\"attempts_with_projection_only\": %u, \"denied\": %u, \"bypasses\": %u, "
                "\"real_writer_control_committed\": %s},\n",
            R.write_attempts, R.write_denied, R.write_bypass, R.write_positive ? "true" : "false");
    fprintf(fp, "  \"gates\": {\n    \"OMEGA_SEMANTIC_COMMUNICATION_PASS\": \"%s\",\n", gate ? "PASS" : "FAIL");
    fprintf(fp, "    \"not_claimed\": [\"transport over a network or between processes (delivery is in-process decode)\", "
                "\"graphics-processor senders or receivers\", \"AIENOS kernel (the authority runs as a host library)\", "
                "\"learned or inferred needs (needs are declared)\", \"uncertainty estimation (Omega carries the producer's declared value)\", "
                "\"worlds above 256 objects\", \"energy as a gate condition (it is measured and reported only)\"]\n");
    fprintf(fp, "  }\n}\n");
    fclose(fp);
    printf("receipt: %s\n", path);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    place();
    schema_init();
    g_truth = calloc(1, sizeof *g_truth);
    g_eval = calloc(1, sizeof *g_eval);
    g_proj = calloc(1, sizeof *g_proj);
    if (!g_truth || !g_eval || !g_proj) return 2;

    t_write_authority();
    t_control();

    static const uint32_t scales[3] = { 32, 100, 500 };
    for (int s = 0; s < 3; s++) {
        ScaleM *S = &R.scale[s];
        S->agents = scales[s];
        run_scale(scales[s], (1u << M_B) | (1u << M_P) | (1u << M_D), 1, S);
        const ModeM *B = &S->m[M_B], *P = &S->m[M_P], *D = &S->m[M_D];
        printf("[%3u workers] bytes B %llu  P %llu  D %llu  (B/D %.1fx)  success B %llu/%llu P %llu/%llu D %llu/%llu  "
               "missing P %llu D %llu  leaks P %llu D %llu (B %llu)\n",
               scales[s], (unsigned long long)B->bytes, (unsigned long long)P->bytes,
               (unsigned long long)D->bytes, (double)B->bytes / (double)(D->bytes ? D->bytes : 1),
               (unsigned long long)B->task_ok, (unsigned long long)B->task_total,
               (unsigned long long)P->task_ok, (unsigned long long)P->task_total,
               (unsigned long long)D->task_ok, (unsigned long long)D->task_total,
               (unsigned long long)P->missing, (unsigned long long)D->missing,
               (unsigned long long)P->leaks, (unsigned long long)D->leaks, (unsigned long long)B->leaks);
        printf("              cpu send/recv ms B %.1f/%.1f  P %.1f/%.1f  D %.1f/%.1f  round median us B %.0f P %.0f D %.0f  "
               "recv mem KiB B %llu P %llu D %llu\n",
               B->sender_cpu_ns / 1e6, B->receiver_cpu_ns / 1e6, P->sender_cpu_ns / 1e6,
               P->receiver_cpu_ns / 1e6, D->sender_cpu_ns / 1e6, D->receiver_cpu_ns / 1e6,
               median_wall(B) / 1e3, median_wall(P) / 1e3, median_wall(D) / 1e3,
               (unsigned long long)B->receiver_mem_peak / 1024, (unsigned long long)P->receiver_mem_peak / 1024,
               (unsigned long long)D->receiver_mem_peak / 1024);
        CHECK(scale_pass(S), "%u workers: pruning keeps every required fact, leaks nothing, sends less", scales[s]);
        CHECK(S->revoke_delivered == S->revoke_expected,
              "%u workers: every worker holding revoked state is told (%llu of %llu)", scales[s],
              (unsigned long long)S->revoke_delivered, (unsigned long long)S->revoke_expected);
    }

    if (!getenv("SEMCOMM_NO_ENERGY") && find_energy()) {
        snprintf(R.energy_source, sizeof R.energy_source, "%s (pkg, cpu_p; microjoules)", g_hwmon);
        idle_power();
        for (int s = 0; s < 3; s++) {
            measure_energy(R.scale[s].agents, &R.scale[s]);
            const ScaleM *S = &R.scale[s];
            printf("[%3u workers] energy per run uJ pkg/cpu_p: world-only %llu/%llu  B %llu/%llu  D %llu/%llu\n",
                   S->agents, (unsigned long long)S->e_pkg[0], (unsigned long long)S->e_cpu[0],
                   (unsigned long long)S->e_pkg[1], (unsigned long long)S->e_cpu[1],
                   (unsigned long long)S->e_pkg[2], (unsigned long long)S->e_cpu[2]);
        }
    } else {
        printf("[*] energy sensor unavailable; energy not measured\n");
    }

    int gate = g_fail == 0 && R.write_bypass == 0 && R.control_missing > 0;
    uint64_t revoke_seen = 0;
    for (int s = 0; s < 3; s++) {
        gate = gate && scale_pass(&R.scale[s]);
        revoke_seen += R.scale[s].revoke_expected;
    }
    CHECK(revoke_seen > 0, "revocation of held state was exercised (%llu workers)", (unsigned long long)revoke_seen);
    gate = gate && revoke_seen > 0;
    printf("checks %d failures %d  OMEGA_SEMANTIC_COMMUNICATION_PASS: %s\n", g_checks, g_fail,
           gate ? "PASS" : "FAIL");
    write_receipt(gate);
    return gate ? 0 : 1;
}
