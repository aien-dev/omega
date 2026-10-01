/*
 * rx_skillroute.c -- Skill Router. See rx_skillroute.h.
 */
#include "rx_skillroute.h"

#include <string.h>

int sr_register_skill(CqCatalog *graph, const SrSkill *skill, const CqEntry *provides, uint32_t n) {
    if (!graph || !skill || (n && !provides) || skill->skill_id == 0 || skill->machine == 0)
        return SR_E_ARG;
    for (uint32_t i = 0; i < n; i++) {
        CqEntry e = provides[i];
        e.skill_id = skill->skill_id;
        e.skill_version = skill->version;
        memcpy(e.skill_digest, skill->digest, sizeof e.skill_digest);
        e.machine_id = skill->machine;
        e.source = skill->machine == graph->self_machine ? CQ_SRC_SKILL : CQ_SRC_FABRIC;
        int rc = cq_register(graph, &e, NULL, 0);
        if (rc != CQ_OK) return rc;
    }
    return SR_OK;
}

uint32_t sr_skill_capabilities(const CqCatalog *graph, uint32_t skill_id, uint32_t version,
                               CqKey *out, uint32_t max) {
    uint32_t n = 0;
    if (!graph || skill_id == 0) return 0;
    for (uint32_t i = 0; i < graph->n; i++) {
        const CqEntry *e = &graph->e[i];
        if (e->skill_id != skill_id || e->skill_version != version || e->live == CQ_LIVE_WITHDRAWN)
            continue;
        if (out && n < max) out[n] = cq_key_of(e);
        n++;
    }
    return n;
}

typedef struct {
    const SrRouter *r;
    const SrRequirement *req;
    SrRoute *out;
    uint32_t pin_index;                 /* canonical pin resolved to a graph index (0 = none) */
} AdmitCtx;

static const AgSkill *executable(const AgSkillTable *t, uint32_t id) {
    for (uint32_t i = 0; t && i < t->n; i++)
        if (t->skill[i].id == id) return &t->skill[i];
    return NULL;
}

static int digest_zero(const uint8_t *d) {
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= d[i];
    return acc == 0;
}

/* Admissible target: a Skill; matching the requirement's pins (version,
 * digest, machine); on a machine with a canonical identity when the graph is
 * canonical; locally, one we can run, byte-identical to what the graph
 * advertises; remotely, allowed by the requirement (the lease was already
 * checked by the query). */
static int admit(void *ctx, const CqEntry *e, const CqCandidate *c) {
    AdmitCtx *a = ctx;
    const SrRequirement *q = a->req;
    if (e->skill_id == 0) { a->out->rejected.not_a_skill++; return 0; }
    if ((q->pin_skill_version && e->skill_version != q->pin_skill_version) ||
        (!digest_zero(q->pin_skill_digest) && memcmp(e->skill_digest, q->pin_skill_digest, 32) != 0)) {
        a->out->rejected.pin_mismatch++;
        return 0;
    }
    if (a->pin_index && e->machine_id != a->pin_index) { a->out->rejected.machine_mismatch++; return 0; }
    if (a->r->graph->machines) {
        AienMachineId m;
        if (cq_machine_identity(a->r->graph, e->machine_id, &m) != CQ_OK) {
            a->out->rejected.machine_unknown++;
            return 0;
        }
    }
    if (!c->local) {
        if (q->local_only) { a->out->rejected.remote_refused++; return 0; }
        return 1;
    }
    const AgSkill *s = executable(a->r->skills, e->skill_id);
    if (!s) { a->out->rejected.not_executable++; return 0; }
    if (!digest_zero(e->skill_digest) && memcmp(e->skill_digest, s->identity, 32) != 0) {
        a->out->rejected.digest_mismatch++;
        return 0;
    }
    return 1;
}

static uint32_t why_of(const SrRoute *o) {
    uint32_t w = 0;
    if (o->stats.rejected_dead) w |= SR_WHY_GONE;
    if (o->stats.rejected_unavailable) w |= SR_WHY_UNAVAILABLE;
    if (o->stats.rejected_authority) w |= SR_WHY_AUTHORITY;
    if (o->rejected.not_a_skill) w |= SR_WHY_NOT_SKILL;
    if (o->rejected.not_executable) w |= SR_WHY_NOT_EXECUTABLE;
    if (o->rejected.digest_mismatch) w |= SR_WHY_DIGEST;
    if (o->rejected.remote_refused) w |= SR_WHY_REMOTE_REFUSED;
    if (o->rejected.pin_mismatch) w |= SR_WHY_PIN;
    if (o->rejected.machine_mismatch) w |= SR_WHY_MACHINE;
    if (o->rejected.machine_unknown) w |= SR_WHY_MACHINE_UNKNOWN;
    return w;
}

static void fill_target(const SrRouter *r, const CqCandidate *c, SrRoute *x) {
    x->chosen = *c;
    x->remote = !c->local;
    x->key = cq_key_of_candidate(c);
    const CqEntry *e = cq_lookup(r->graph, &x->key);
    if (e) {
        x->skill_version = e->skill_version;
        memcpy(x->skill_digest, e->skill_digest, 32);
        x->generation = e->generation;
    }
    x->target_known = cq_machine_identity(r->graph, c->machine_id, &x->target) == CQ_OK;
    x->verdict = x->remote ? SR_E_REMOTE : SR_OK;
}

/* Shared by sr_route (k = 1) and sr_route_alternatives (dominated included).
 * Fills out[0..n-1]; returns n (>= 1) or the SR_E_* left in out[0].verdict. */
static int route_k(const SrRouter *r, const SrRequirement *req, const CqHeld *held,
                   uint64_t now_us, SrRoute *out, uint32_t max, int alternatives) {
    SrRoute *o = &out[0];
    memset(o, 0, sizeof *o);
    if (!r || !r->graph || !req) return o->verdict = SR_E_ARG;
    AdmitCtx ctx = { r, req, o, 0 };
    if (req->pin_machine_set) {
        ctx.pin_index = cq_machine_index(r->graph, &req->pin_machine);
        if (ctx.pin_index == 0) return o->verdict = SR_E_MACHINE;
    }
    CqPlan plan;
    o->query_verdict = cq_compile(r->graph, &req->need, &plan);
    memcpy(o->plan_digest, plan.digest, sizeof o->plan_digest);
    if (o->query_verdict == CQ_E_NO_SOURCE) return o->verdict = SR_E_NO_CANDIDATE;
    if (o->query_verdict != CQ_OK) return o->verdict = SR_E_QUERY;
    CqTradeoffs t = req->t;
    if (alternatives) {
        t.k = max < CQ_MAX_K ? max : CQ_MAX_K;
        t.include_dominated = 1;
    } else {
        t.k = 1;                        /* the router needs the winner only */
    }
    CqResult res;
    o->query_verdict = cq_query_admit(r->graph, &plan, &req->need, &t, held, now_us, admit, &ctx,
                                      &res, &o->stats);
    if (o->query_verdict != CQ_OK) return o->verdict = SR_E_QUERY;
    o->n_admissible = res.n_feasible;
    if (res.n == 0) {
        o->why = why_of(o);
        return o->verdict = SR_E_NO_CANDIDATE;
    }
    uint32_t n = res.n < t.k ? res.n : t.k;
    for (uint32_t i = 0; i < n; i++) {
        SrRoute *x = &out[i];
        if (i) {
            memset(x, 0, sizeof *x);
            x->query_verdict = o->query_verdict;
            memcpy(x->plan_digest, o->plan_digest, sizeof x->plan_digest);
            x->n_admissible = o->n_admissible;
        }
        fill_target(r, &res.cand[i], x);
    }
    return (int)n;
}

int sr_route(const SrRouter *r, const SrRequirement *req, const CqHeld *held, uint64_t now_us,
             SrRoute *out) {
    if (!out) return SR_E_ARG;
    int n = route_k(r, req, held, now_us, out, 1, 0);
    return n < 0 ? n : out->verdict;
}

/* Bind without leaving a half-bound node behind on failure. */
static int bind_closed(AgGraph *g, uint32_t node, const CqCandidate *c) {
    if (node >= g->n_nodes) return SR_E_BIND;
    uint32_t op = g->nodes[node].op, n_auth = g->n_auth;
    if (cq_bind_skill_node(g, node, c) != CQ_OK) {
        g->nodes[node].op = op;
        g->n_auth = n_auth;
        return SR_E_BIND;
    }
    return SR_OK;
}

int sr_bind_node(const SrRouter *r, AgGraph *g, uint32_t node, const SrRequirement *req,
                 const CqHeld *held, uint64_t now_us, SrRoute *out) {
    if (!g) return SR_E_ARG;
    int rc = sr_route(r, req, held, now_us, out);
    if (rc != SR_OK) return rc;
    return out->verdict = bind_closed(g, node, &out->chosen);
}

/* COMPOSITION-2: up to `max` admissible local-or-remote alternatives, ranked
 * like sr_route (dominated providers included, so a costlier fallback is
 * still offered). out[0] is what sr_route would choose: the banded order
 * keeps a dominator wherever it keeps what it dominates, and the dominator
 * sorts first. Returns the number filled (>= 1), or the sr_route error.
 * Discovers only; mints nothing. */
int sr_route_alternatives(const SrRouter *r, const SrRequirement *req, const CqHeld *held,
                          uint64_t now_us, SrRoute *out, uint32_t max) {
    if (!out || max == 0) return SR_E_ARG;
    return route_k(r, req, held, now_us, out, max, 1);
}

/* A machine is live when it is this one or its Fabric lease has not ended.
 * Scans the catalog's lease table (keys are unique; no hash assumption). */
static int lease_live(const CqCatalog *c, uint32_t machine, uint64_t now_us) {
    if (machine == c->self_machine) return 1;
    if (machine == 0 || !c->mach_key || !c->mach_lease) return 0;
    for (uint32_t i = 0; i <= c->mach_mask; i++)
        if (c->mach_key[i] == machine) return c->mach_lease[i] > now_us;
    return 0;
}

int sr_route_check(const SrRouter *r, const SrRoute *route, uint64_t now_us) {
    if (!r || !r->graph || !route || (route->verdict != SR_OK && route->verdict != SR_E_REMOTE))
        return SR_E_ARG;
    const CqEntry *e = cq_lookup(r->graph, &route->key);
    if (!e || e->live == CQ_LIVE_WITHDRAWN) return SR_E_WITHDRAWN;
    if (e->live != CQ_LIVE_AVAILABLE) return SR_E_UNAVAILABLE;
    if (e->generation != route->generation || e->skill_version != route->skill_version ||
        memcmp(e->skill_digest, route->skill_digest, 32) != 0 ||
        e->skill_id != route->chosen.skill_id ||
        e->auth_resource != route->chosen.required_authority.resource ||
        e->auth_rights != route->chosen.required_authority.rights)
        return SR_E_STALE;
    if (r->graph->machines) {
        AienMachineId m;
        if (cq_machine_identity(r->graph, e->machine_id, &m) != CQ_OK ||
            (route->target_known && !aien_mid_equal(&m, &route->target)))
            return SR_E_MACHINE;
    }
    if (!lease_live(r->graph, e->machine_id, now_us)) return SR_E_MACHINE;   /* lease ended */
    if (route->remote) return SR_E_REMOTE;
    if (e->machine_id != r->graph->self_machine) return SR_E_MACHINE;
    const AgSkill *s = executable(r->skills, e->skill_id);
    if (!s || (!digest_zero(e->skill_digest) && memcmp(e->skill_digest, s->identity, 32) != 0))
        return SR_E_STALE;
    return SR_OK;
}

int sr_bind_route(const SrRouter *r, AgGraph *g, uint32_t node, const SrRoute *route,
                  uint64_t now_us) {
    if (!g) return SR_E_ARG;
    int rc = sr_route_check(r, route, now_us);
    if (rc != SR_OK) return rc;
    return bind_closed(g, node, &route->chosen);
}
