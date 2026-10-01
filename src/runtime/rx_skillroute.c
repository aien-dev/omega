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

/* Admissible target: a Skill; locally, one we can run, byte-identical to
 * what the graph advertises; remotely, allowed by the requirement (the lease
 * was already checked by the query). */
static int admit(void *ctx, const CqEntry *e, const CqCandidate *c) {
    AdmitCtx *a = ctx;
    if (e->skill_id == 0) { a->out->rejected.not_a_skill++; return 0; }
    if (!c->local) {
        if (a->req->local_only) { a->out->rejected.remote_refused++; return 0; }
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

int sr_route(const SrRouter *r, const SrRequirement *req, const CqHeld *held, uint64_t now_us,
             SrRoute *out) {
    if (!out) return SR_E_ARG;
    memset(out, 0, sizeof *out);
    if (!r || !r->graph || !req) return out->verdict = SR_E_ARG;
    CqPlan plan;
    out->query_verdict = cq_compile(r->graph, &req->need, &plan);
    memcpy(out->plan_digest, plan.digest, sizeof out->plan_digest);
    if (out->query_verdict == CQ_E_NO_SOURCE) return out->verdict = SR_E_NO_CANDIDATE;
    if (out->query_verdict != CQ_OK) return out->verdict = SR_E_QUERY;
    CqTradeoffs t = req->t;
    t.k = 1;                            /* the router needs the winner only */
    AdmitCtx ctx = { r, req, out };
    CqResult res;
    out->query_verdict = cq_query_admit(r->graph, &plan, &req->need, &t, held, now_us, admit, &ctx,
                                        &res, &out->stats);
    if (out->query_verdict != CQ_OK) return out->verdict = SR_E_QUERY;
    out->n_admissible = res.n_feasible;
    if (res.n == 0) return out->verdict = SR_E_NO_CANDIDATE;
    out->chosen = res.cand[0];
    out->remote = !res.cand[0].local;
    CqKey k = cq_key_of_candidate(&res.cand[0]);
    const CqEntry *e = cq_lookup(r->graph, &k);
    if (e) {
        out->skill_version = e->skill_version;
        memcpy(out->skill_digest, e->skill_digest, 32);
    }
    out->target_known = cq_machine_identity(r->graph, res.cand[0].machine_id, &out->target) == CQ_OK;
    return out->verdict = out->remote ? SR_E_REMOTE : SR_OK;
}

int sr_bind_node(const SrRouter *r, AgGraph *g, uint32_t node, const SrRequirement *req,
                 const CqHeld *held, uint64_t now_us, SrRoute *out) {
    if (!g) return SR_E_ARG;
    int rc = sr_route(r, req, held, now_us, out);
    if (rc != SR_OK) return rc;
    if (cq_bind_skill_node(g, node, &out->chosen) != CQ_OK) return out->verdict = SR_E_BIND;
    return SR_OK;
}
