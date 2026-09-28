/*
 * rx_plan.c -- Omega plan IR and verified plan cache. See rx_plan.h and
 * spec/plan-reuse.md.
 */
#include "rx_plan.h"

#include "sha256.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t pl_word(const uint8_t id[32], uint32_t k) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < 8; i++) v |= (uint64_t)id[k * 8 + i] << (8 * i);
    return v;
}

/* ---- canonical bytes ---- */

typedef struct { uint8_t *b; size_t n, cap; int over; } Buf;

static void put(Buf *o, const void *p, size_t n) {
    if (o->n + n > o->cap) { o->over = 1; return; }
    memcpy(o->b + o->n, p, n);
    o->n += n;
}
static void p32(Buf *o, uint32_t v) {
    uint8_t x[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    put(o, x, 4);
}
static void p64(Buf *o, uint64_t v) { p32(o, (uint32_t)v); p32(o, (uint32_t)(v >> 32)); }

static int pred_cmp(const PlPred *a, const PlPred *b) {
    const uint32_t x[5] = { a->kind, a->slot, a->field, a->slot2, a->type };
    const uint32_t y[5] = { b->kind, b->slot, b->field, b->slot2, b->type };
    for (int i = 0; i < 5; i++)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    if (a->value != b->value) return a->value < b->value ? -1 : 1;
    return 0;
}

static void sort_list(PlPredList *l) {
    for (uint32_t i = 1; i < l->n; i++)
        for (uint32_t j = i; j > 0 && pred_cmp(&l->p[j - 1], &l->p[j]) > 0; j--) {
            PlPred t = l->p[j];
            l->p[j] = l->p[j - 1];
            l->p[j - 1] = t;
        }
}

void rx_plan_sort(PlanTemplate *t) {
    sort_list(&t->pre);
    sort_list(&t->state);
    sort_list(&t->env);
    sort_list(&t->invariants);
    sort_list(&t->success);
}

static void put_list(Buf *o, const PlPredList *l) {
    p32(o, l->n);
    for (uint32_t i = 0; i < l->n; i++) {
        const PlPred *p = &l->p[i];
        p32(o, p->kind); p32(o, p->slot); p32(o, p->field); p32(o, p->slot2); p32(o, p->type);
        p64(o, p->value);
    }
}

#define PL_MAGIC 0x4C504D4Fu   /* "OMPL" */

size_t rx_plan_encode(const PlanTemplate *t, uint8_t *buf, size_t cap) {
    Buf o = { buf, 0, cap, 0 };
    p32(&o, PL_MAGIC);
    p32(&o, 1);
    p32(&o, t->goal_kind);
    put(&o, t->goal_shape, 32);
    p32(&o, t->n_goal_slots);
    p32(&o, t->n_slots);
    for (uint32_t i = 0; i < t->n_slots; i++) {
        const PlSlot *s = &t->slots[i];
        p32(&o, s->type); p32(&o, s->role); p32(&o, s->rights);
        p32(&o, s->bind_rule); p32(&o, s->bind_from); p32(&o, s->bind_field);
    }
    put_list(&o, &t->pre);
    put_list(&o, &t->state);
    put_list(&o, &t->env);
    put_list(&o, &t->invariants);
    put_list(&o, &t->success);
    p32(&o, t->fail_flags);
    const RxResourceNeed *r = &t->step_need;
    p32(&o, r->compute_class); p32(&o, r->locality); p32(&o, r->accelerator_features);
    p32(&o, r->latency_class); p64(&o, r->memory_bytes); p64(&o, r->deadline);
    p64(&o, r->energy_cost);
    p32(&o, t->n_steps);
    p64(&o, t->energy_total);
    p32(&o, t->n_evidence);
    p32(&o, t->graph_kind);
    put(&o, t->graph_digest, 32);
    return o.over ? 0 : o.n;
}

typedef struct { const uint8_t *b; size_t n, at; int bad; } In;

static void get(In *i, void *p, size_t n) {
    if (i->at + n > i->n) { i->bad = 1; memset(p, 0, n); return; }
    memcpy(p, i->b + i->at, n);
    i->at += n;
}
static uint32_t g32(In *i) {
    uint8_t x[4];
    get(i, x, 4);
    return (uint32_t)x[0] | (uint32_t)x[1] << 8 | (uint32_t)x[2] << 16 | (uint32_t)x[3] << 24;
}
static uint64_t g64(In *i) { uint64_t lo = g32(i); return lo | (uint64_t)g32(i) << 32; }

static void get_list(In *i, PlPredList *l) {
    l->n = g32(i);
    if (l->n > PL_MAX_PREDS) { i->bad = 1; l->n = 0; return; }
    for (uint32_t k = 0; k < l->n; k++) {
        PlPred *p = &l->p[k];
        p->kind = g32(i); p->slot = g32(i); p->field = g32(i); p->slot2 = g32(i);
        p->type = g32(i); p->value = g64(i);
    }
}

int rx_plan_decode(const uint8_t *buf, size_t n, PlanTemplate *t) {
    In i = { buf, n, 0, 0 };
    memset(t, 0, sizeof *t);
    if (g32(&i) != PL_MAGIC || g32(&i) != 1) return -1;
    t->goal_kind = g32(&i);
    get(&i, t->goal_shape, 32);
    t->n_goal_slots = g32(&i);
    t->n_slots = g32(&i);
    if (t->n_slots > PL_MAX_SLOTS || t->n_goal_slots > t->n_slots) return -1;
    for (uint32_t k = 0; k < t->n_slots; k++) {
        PlSlot *s = &t->slots[k];
        s->type = g32(&i); s->role = g32(&i); s->rights = g32(&i);
        s->bind_rule = g32(&i); s->bind_from = g32(&i); s->bind_field = g32(&i);
    }
    get_list(&i, &t->pre);
    get_list(&i, &t->state);
    get_list(&i, &t->env);
    get_list(&i, &t->invariants);
    get_list(&i, &t->success);
    t->fail_flags = g32(&i);
    RxResourceNeed *r = &t->step_need;
    r->compute_class = g32(&i); r->locality = g32(&i); r->accelerator_features = g32(&i);
    r->latency_class = g32(&i); r->memory_bytes = g64(&i); r->deadline = g64(&i);
    r->energy_cost = g64(&i);
    t->n_steps = g32(&i);
    t->energy_total = g64(&i);
    t->n_evidence = g32(&i);
    t->graph_kind = g32(&i);
    get(&i, t->graph_digest, 32);
    if (i.bad || i.at != n) return -1;
    return 0;
}

void rx_plan_identify(PlanTemplate *t) {
    rx_plan_sort(t);
    rx_graph_identify(&t->graph);
    memcpy(t->graph_digest, t->graph.digest, 32);
    static uint8_t buf[1u << 16];
    size_t n = rx_plan_encode(t, buf, sizeof buf);
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)"OMEGA_PLAN_TEMPLATE_V1", 22);
    sha256_update(&c, buf, n);
    sha256_final(&c, t->semantic_id);
}

/* ---- views and predicates ---- */

void rx_plan_view(RxWorld *w, PlView *v) {
    pthread_mutex_lock(&w->mu);
    v->n = RX_MAX_OBJECTS;
    for (uint32_t i = 0; i < RX_MAX_OBJECTS; i++) {
        const RxObject *o = &w->objects[i];
        v->o[i].type = o->type;
        v->o[i].generation = o->generation;
        v->o[i].live = o->live;
        v->o[i].resource = o->resource;
        memcpy(v->o[i].field, o->field, sizeof o->field);
    }
    pthread_mutex_unlock(&w->mu);
}

static const uint64_t *fields_of(const PlView *v, RxObjRef r) {
    if (r.id >= v->n || !v->o[r.id].live || v->o[r.id].generation != r.generation) return NULL;
    return v->o[r.id].field;
}

static int is_slot(RxObjRef r, const RxObjRef *slots, uint32_t n) {
    for (uint32_t s = 0; s < n; s++)
        if (slots[s].id == r.id && slots[s].generation == r.generation) return 1;
    return 0;
}

int rx_plan_pred(const PlPred *p, const PlView *v, const PlEnv *env, const RxObjRef *slots,
                 uint32_t n_slots) {
    uint32_t f = p->field & (RX_MAX_FIELDS - 1);
    if (p->kind == PL_P_ENV_EQ || p->kind == PL_P_ENV_GE) {
        if (!env || p->slot >= PL_ENV_COUNT) return 0;
        const uint64_t *x = fields_of(v, env->obj[p->slot]);
        if (!x) return 0;
        return p->kind == PL_P_ENV_EQ ? x[f] == p->value : x[f] >= p->value;
    }
    if (!slots || p->slot >= n_slots) return 0;
    RxObjRef r = slots[p->slot];
    const uint64_t *x = fields_of(v, r);
    if (!x) return 0;
    switch (p->kind) {
    case PL_P_EQ_CONST:
        return x[f] == p->value;
    case PL_P_EQ_SLOT:
        return p->slot2 < n_slots && x[f] == pl_ref_pack(slots[p->slot2]);
    case PL_P_NOT_SLOT:
        if (x[f] == 0) return 0;
        for (uint32_t s = 0; s < n_slots; s++)
            if (x[f] == pl_ref_pack(slots[s])) return 0;
        return 1;
    case PL_P_SELF_REF:
        return x[f] == pl_ref_pack(r);
    case PL_P_LIVE:
        return v->o[r.id].type == p->type;
    case PL_P_UNREFERENCED: {
        uint64_t me = pl_ref_pack(r);
        for (uint32_t i = 0; i < v->n; i++) {
            if (!v->o[i].live || v->o[i].type != p->type) continue;
            if (v->o[i].field[f] != me) continue;
            if (!is_slot((RxObjRef){ i, v->o[i].generation }, slots, n_slots)) return 0;
        }
        return 1;
    }
    default:
        return 0;
    }
}

int rx_plan_preds(const PlPredList *l, const PlView *v, const PlEnv *env, const RxObjRef *slots,
                  uint32_t n_slots, PlCost *cost) {
    for (uint32_t i = 0; i < l->n; i++) {
        if (cost) cost->preds++;
        if (!rx_plan_pred(&l->p[i], v, env, slots, n_slots)) return (int)i;
    }
    return -1;
}

static int cap_find(RxWorld *w, const AgCapTable *caps, uint64_t resource, uint32_t rights,
                    RxCapRef *out) {
    for (uint32_t i = 0; caps && i < caps->n; i++) {
        if (caps->cap[i].resource != resource || (caps->cap[i].rights & rights) != rights) continue;
        if (rx_world_validate_cap(w, caps->cap[i].ref, caps->subject, resource, rights, NULL) !=
            RX_CAP_OK)
            continue;
        if (out) *out = caps->cap[i].ref;
        return 1;
    }
    return 0;
}

static int need_fits(const RxResourceNeed *n, const RxResourceBudget *b) {
    if (n->memory_bytes > b->memory_bytes) return 0;
    if (n->accelerator_features & ~b->offered_accel) return 0;
    if (n->locality & ~b->offered_locality) return 0;
    if (n->compute_class && !(b->compute_mask & (1u << (n->compute_class & 31u)))) return 0;
    if (n->energy_cost > b->energy_budget) return 0;
    return 1;
}

PlAxis rx_plan_applicable(const PlanTemplate *t, RxWorld *w, const PlView *v, const PlEnv *env,
                          const RxObjRef *slots, const AgCapTable *caps, PlCost *cost,
                          int *pred_out) {
    int dummy;
    if (!pred_out) pred_out = &dummy;
    *pred_out = -1;
    if (t->status == PL_STATUS_STALE) return PL_AX_STALE;
    /* Binding: every slot live, at its generation, of its type. */
    for (uint32_t s = 0; s < t->n_slots; s++) {
        PlPred p = { PL_P_LIVE, s, 0, 0, t->slots[s].type, 0 };
        if (cost) cost->preds++;
        if (!rx_plan_pred(&p, v, env, slots, t->n_slots)) { *pred_out = (int)s; return PL_AX_BINDING; }
    }
    int k;
    if ((k = rx_plan_preds(&t->pre, v, env, slots, t->n_slots, cost)) >= 0) {
        *pred_out = k;
        return PL_AX_GOAL;
    }
    PlPred wg = { PL_P_ENV_EQ, PL_ENV_WORLDGEN, 0, 0, 0, t->verified_world_gen };
    PlPred cg = { PL_P_ENV_EQ, PL_ENV_COGNITION, 0, 0, 0, t->verified_cog_gen };
    if (cost) cost->preds += 2;
    if (!rx_plan_pred(&wg, v, env, NULL, 0)) return PL_AX_WORLD_GEN;
    if (!rx_plan_pred(&cg, v, env, NULL, 0)) return PL_AX_COG_GEN;
    if ((k = rx_plan_preds(&t->env, v, env, slots, t->n_slots, cost)) >= 0) {
        *pred_out = k;
        return PL_AX_ENVIRONMENT;
    }
    if ((k = rx_plan_preds(&t->state, v, env, slots, t->n_slots, cost)) >= 0) {
        *pred_out = k;
        return PL_AX_STATE;
    }
    if ((k = rx_plan_preds(&t->invariants, v, env, slots, t->n_slots, cost)) >= 0) {
        *pred_out = 1000 + k;
        return PL_AX_STATE;
    }
    for (uint32_t s = 0; s < t->n_slots; s++) {
        if (!t->slots[s].rights) continue;
        if (cost) cost->preds++;
        if (!cap_find(w, caps, v->o[slots[s].id].resource, t->slots[s].rights, NULL)) {
            *pred_out = (int)s;
            return PL_AX_AUTHORITY;
        }
    }
    RxResourceBudget b;
    pthread_mutex_lock(&w->mu);
    b = w->budget;
    pthread_mutex_unlock(&w->mu);
    if (cost) cost->preds++;
    if (!need_fits(&t->step_need, &b)) return PL_AX_RESOURCES;
    return PL_AX_OK;
}

/* ---- store ---- */

void rx_plan_cache_init(PlCache *c) { memset(c, 0, sizeof *c); }

void rx_plan_cache_free(PlCache *c) {
    for (uint32_t i = 0; i < c->n; i++) free(c->t[i]);
    memset(c, 0, sizeof *c);
}

PlanTemplate *rx_plan_cache_find(PlCache *c, const uint8_t semantic_id[32]) {
    for (uint32_t i = 0; i < c->n; i++)
        if (memcmp(c->t[i]->semantic_id, semantic_id, 32) == 0) return c->t[i];
    return NULL;
}

PlanTemplate *rx_plan_cache_store(PlCache *c, const PlanTemplate *t, uint64_t world_gen,
                                  uint64_t cog_gen) {
    PlanTemplate *have = rx_plan_cache_find(c, t->semantic_id);
    if (have) {
        /* Same meaning found again: renew where it was verified. It has one
         * verified execution under these generations, as a new candidate has. */
        have->verified_world_gen = world_gen;
        have->verified_cog_gen = cog_gen;
        have->status = PL_STATUS_CANDIDATE;
        have->rederivations++;
        c->deduplicated++;
        return have;
    }
    if (c->n >= PL_MAX_TEMPLATES) return NULL;
    PlanTemplate *n = malloc(sizeof *n);
    if (!n) return NULL;
    *n = *t;
    n->status = PL_STATUS_CANDIDATE;
    n->verified_world_gen = world_gen;
    n->verified_cog_gen = cog_gen;
    n->uses = n->successes = n->failures = n->promotions = n->rederivations = 0;
    memset(n->refusals, 0, sizeof n->refusals);
    c->t[c->n++] = n;
    c->stored++;
    return n;
}

uint32_t rx_plan_cache_retrieve(PlCache *c, uint32_t goal_kind, const uint8_t shape[32],
                                PlanTemplate **out, uint32_t max, PlCost *cost) {
    uint32_t k = 0;
    for (uint32_t i = 0; i < c->n && k < max; i++) {
        if (c->t[i]->goal_kind != goal_kind || memcmp(c->t[i]->goal_shape, shape, 32) != 0) continue;
        out[k++] = c->t[i];
        if (cost) cost->candidates++;
    }
    return k;
}

void rx_plan_record_use(PlanTemplate *t, int success) {
    t->uses++;
    if (success) {
        t->successes++;
        if (t->status == PL_STATUS_CANDIDATE) {
            t->status = PL_STATUS_VERIFIED;
            t->promotions++;
        }
    } else {
        t->failures++;
        t->status = PL_STATUS_STALE;
    }
}

/* ---- execution ---- */

int rx_plan_execute(const PlanTemplate *t, RxWorld *w, const PlEnv *env, const RxObjRef *slots,
                    const AgCapTable *caps, const PlExecCtx *x, uint64_t run, PlRunStore *rs,
                    PlLegalFn legal, PlExecResult *out) {
    memset(out, 0, sizeof *out);
    if (t->graph_kind != PL_GRAPH_RX_GRAPH || t->n_slots > 8) return -1;
    uint64_t t0 = now_ns();
    AgLibrary lib;
    memset(&lib, 0, sizeof lib);
    lib.n = 1;
    lib.proc[0] = (AgProcedure){ t->goal_kind, &t->graph };
    AgGoal goal;
    memset(&goal, 0, sizeof goal);
    goal.kind = t->goal_kind;
    goal.n_args = t->n_slots;
    for (uint32_t s = 0; s < t->n_slots; s++) goal.args[s] = slots[s];
    AgConstraints cons;
    memset(&cons, 0, sizeof cons);
    cons.max_effects = UINT32_MAX;
    pthread_mutex_lock(&w->mu);
    cons.budget = w->budget;
    pthread_mutex_unlock(&w->mu);
    AgReport rep;
    out->compile_verdict = rx_graph_compile(&goal, w, caps, &cons, &lib, 1, &rs->g, &rep);
    out->n_missing = rep.n_missing;
    out->n_resource_blocked = rep.n_resource_blocked;
    out->compile_ns = now_ns() - t0;
    if (out->compile_verdict != AG_OK_READY) {
        out->fail = PL_FAIL_GRAPH;
        return 0;
    }
    memcpy(out->compiled_digest, rs->g.digest, 32);
    out->nodes = rs->g.n_nodes;

    /* Realization: this binding, the capabilities it uses, the compiled graph,
     * the World generation and the core class. */
    PlView *v = malloc(sizeof *v);
    if (!v) return -1;
    rx_plan_view(w, v);
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)"OMEGA_PLAN_REALIZATION_V1", 25);
    sha256_update(&c, t->semantic_id, 32);
    uint8_t b8[8];
    for (uint32_t s = 0; s < t->n_slots; s++) {
        RxCapRef cr = { 0, 0 };
        if (t->slots[s].rights)
            cap_find(w, caps, v->o[slots[s].id].resource, t->slots[s].rights, &cr);
        uint64_t words[2] = { pl_ref_pack(slots[s]), ((uint64_t)cr.generation << 32) | cr.cap_id };
        for (int q = 0; q < 2; q++) {
            for (int i = 0; i < 8; i++) b8[i] = (uint8_t)(words[q] >> (8 * i));
            sha256_update(&c, b8, 8);
        }
    }
    sha256_update(&c, rs->g.digest, 32);
    const uint64_t *wg = fields_of(v, env->obj[PL_ENV_WORLDGEN]);
    uint64_t tail[2] = { wg ? wg[0] : 0, x->core_class };
    for (int q = 0; q < 2; q++) {
        for (int i = 0; i < 8; i++) b8[i] = (uint8_t)(tail[q] >> (8 * i));
        sha256_update(&c, b8, 8);
    }
    sha256_final(&c, out->realization_id);

    if (x->reference_first) {
        uint64_t r0 = now_ns();
        AgReference *ref = malloc(sizeof *ref);
        if (!ref) { free(v); return -1; }
        rx_graph_reference(&rs->g, w, x->skills, caps, run, ref);
        out->reference_outcome = ref->r.outcome;
        free(ref);
        out->reference_ns = now_ns() - r0;
        if (out->reference_outcome != AG_RUN_SUCCESS) {
            out->fail = PL_FAIL_GRAPH;
            free(v);
            return 0;
        }
    }

    uint64_t t1 = now_ns();
    uint64_t checked = 0;
    pthread_mutex_lock(&w->mu);
    out->crumbs_before = w->n_crumbs;
    pthread_mutex_unlock(&w->mu);
    if (rx_graph_lower(&rs->L, w, &rs->g, x->skills, caps, x->cell_res, x->cell_cap, x->run_res,
                       x->run_cap, NULL) != 0) {
        out->fail = PL_FAIL_GRAPH;
        free(v);
        return 0;
    }
    if (rx_graph_start(&rs->L, x->ext_run_cap, run) < 0) {
        out->fail = PL_FAIL_GRAPH;
        free(v);
        return 0;
    }
    rx_world_wait_quiescent(w, 30000);
    AgResult res;
    rx_graph_collect(&rs->L, run, &res);
    out->outcome = res.outcome;
    out->run_ns = now_ns() - t1;

    uint64_t t2 = now_ns();
    if (res.outcome != AG_RUN_SUCCESS) out->fail |= PL_FAIL_GRAPH;
    out->evidence_required = rs->g.n_evidence;
    for (uint32_t i = 0; i < rs->g.n_evidence; i++)
        out->evidence_present += res.evidence[rs->g.evidence[i]] != 0 &&
                                 res.status[rs->g.evidence[i]] == AG_OK;
    if (out->evidence_present != out->evidence_required) out->fail |= PL_FAIL_EVIDENCE;
    rx_plan_view(w, v);
    if (rx_plan_preds(&t->invariants, v, env, slots, t->n_slots, NULL) >= 0 || (legal && !legal(v)))
        out->fail |= PL_FAIL_INVARIANT;
    int ok = rx_plan_preds(&t->success, v, env, slots, t->n_slots, NULL) < 0;
    if (rx_world_verify_crumbs(w, &checked) != 0) out->fail |= PL_FAIL_CRUMBS;
    out->crumbs_checked = checked;
    pthread_mutex_lock(&w->mu);
    out->crumbs_after = w->n_crumbs;
    pthread_mutex_unlock(&w->mu);
    out->success = ok && out->fail == 0;
    out->verify_ns = now_ns() - t2;
    free(v);
    return 0;
}

/* ---- World records ---- */

int64_t rx_plan_decide(RxWorld *w, RxCapRef cap, RxObjRef decision, uint64_t goal_seq,
                       PlAxis axis, const PlanTemplate *t, int pred, uint64_t seq,
                       const uint8_t *realization_id) {
    uint64_t f[RX_MAX_FIELDS] = {
        goal_seq, (uint64_t)axis, t ? pl_word(t->semantic_id, 0) : 0,
        t ? pl_word(t->semantic_id, 1) : 0, pred >= 0 ? (uint64_t)pred + 1 : 0, seq,
        realization_id ? pl_word(realization_id, 0) : 0, t ? t->status : 0
    };
    RxMutation m[RX_MAX_FIELDS];
    for (uint32_t i = 0; i < RX_MAX_FIELDS; i++) m[i] = (RxMutation){ decision, i, f[i] };
    return rx_world_publish_external(w, cap, m, RX_MAX_FIELDS);
}

int64_t rx_plan_publish_index(RxWorld *w, RxCapRef cap, RxObjRef index, const PlCache *c,
                              const PlanTemplate *t) {
    uint64_t f[RX_MAX_FIELDS] = {
        c->n, pl_word(t->semantic_id, 0), pl_word(t->semantic_id, 1), t->status,
        t->verified_world_gen, t->uses, t->successes, t->failures
    };
    RxMutation m[RX_MAX_FIELDS];
    for (uint32_t i = 0; i < RX_MAX_FIELDS; i++) m[i] = (RxMutation){ index, i, f[i] };
    return rx_world_publish_external(w, cap, m, RX_MAX_FIELDS);
}

const char *rx_plan_axis_name(PlAxis a) {
    static const char *n[PL_AX_COUNT] = { "accepted", "shape", "binding", "goal_constraints",
                                          "world_generation", "cognitive_generation",
                                          "environment", "required_state", "authority",
                                          "resources", "stale" };
    return a < PL_AX_COUNT ? n[a] : "?";
}

const char *rx_plan_status_name(uint32_t s) {
    return s == PL_STATUS_CANDIDATE ? "CANDIDATE" : s == PL_STATUS_VERIFIED ? "VERIFIED"
         : s == PL_STATUS_STALE ? "STALE" : "?";
}
