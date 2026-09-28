/*
 * rx_fusion.c -- Omega verified workflow fusion: observe verified runs,
 * mine repeated fragments, propose MetaSkills, verify them against the
 * original fragment, gate them through measurement, canary and the
 * generation barrier, and apply only published ones.
 *
 * Fragment shape, identity and the step program are defined in rx_graph.c
 * (rx_graph_meta_seal, rx_graph_meta_run); this file decides which fragments
 * qualify and moves a MetaSkill through its states.
 */
#include "rx_fusion.h"

#include "omega_core.h"
#include "sha256.h"

#include <string.h>

/* ---- small helpers ---- */

static void put8(sha256_ctx *c, uint8_t v) { sha256_update(c, &v, 1); }

static void put32(sha256_ctx *c, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    sha256_update(c, b, 4);
}

static void put64(sha256_ctx *c, uint64_t v) {
    put32(c, (uint32_t)(v >> 32));
    put32(c, (uint32_t)v);
}

static uint64_t word64(const uint8_t *d) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | d[i];
    return v;
}

static int bit(uint64_t set, uint32_t n) { return (int)((set >> n) & 1u); }

static int eligible_kind(AgKind k) {
    return k == AG_CONST || k == AG_PURE || k == AG_WORLD_READ || k == AG_RECALL ||
           k == AG_PHYSICAL || k == AG_SKILL || k == AG_RETRY || k == AG_EFFECT_PROPOSE ||
           k == AG_VERIFY;
}

static int step_reads(AgKind k) { return k == AG_WORLD_READ || k == AG_RECALL; }

static int boundary_kind(AgKind k) { return k == AG_WORLD_PUBLISH || k == AG_EFFECT_PERFORM; }

static int pinned(const AgGraph *g, uint32_t n) {
    for (uint32_t i = 0; i < g->n_evidence; i++)
        if (g->evidence[i] == n) return 1;
    for (uint32_t i = 0; i < g->n_success; i++)
        if (g->success[i].node == n) return 1;
    for (uint32_t i = 0; i < g->n_failure; i++)
        if (g->failure[i].node == n) return 1;
    return 0;
}

static uint32_t inputs_by_port(const AgGraph *g, uint32_t n, uint32_t out[AG_MAX_IN]) {
    uint32_t k = 0;
    for (uint32_t p = 0; p < AG_MAX_IN; p++)
        for (uint32_t e = 0; e < g->n_data; e++)
            if (g->data[e].to == n && g->data[e].port == p && k < AG_MAX_IN) out[k++] = e;
    return k;
}

const char *rx_fusion_state_name(uint32_t s) {
    static const char *n[] = { "?", "candidate", "verified", "rejected", "measured", "slower",
                               "canary_passed", "quarantined", "promoted", "published" };
    return s < sizeof n / sizeof n[0] ? n[s] : "?";
}

const char *rx_fusion_reason_name(uint32_t r) {
    static const char *n[] = { "ok", "identity", "contract", "authority", "effect", "resource",
                               "not_runnable", "semantic", "failure_behaviour", "evidence" };
    return r < sizeof n / sizeof n[0] ? n[r] : "?";
}

/* ---- fragments: extraction ---- */

enum { X_OK = 0, X_NO = -1, X_BUDGET = -2 };

typedef struct {
    const AgGraph *g;
    uint64_t set;
    uint64_t visited;
    uint32_t n;
    uint16_t order[AG_META_MAX_STEPS];
} Walk;

/* Canonical step order: post-order from the exit over data inputs by port. */
static void visit(Walk *wk, uint32_t n) {
    if (bit(wk->visited, n)) return;
    wk->visited |= 1ull << n;
    uint32_t e[AG_MAX_IN];
    uint32_t k = inputs_by_port(wk->g, n, e);
    for (uint32_t i = 0; i < k; i++) {
        uint32_t s = wk->g->data[e[i]].from;
        if (bit(wk->set, s)) visit(wk, s);
    }
    if (wk->n < AG_META_MAX_STEPS) wk->order[wk->n] = (uint16_t)n;
    wk->n++;
}

static int extract(const AgGraph *g, const RxWorld *w, uint64_t S, uint32_t exit, AgMetaProgram *p,
                   AgFusionSite *site) {
    memset(p, 0, sizeof *p);
    memset(site, 0, sizeof *site);
    if (exit >= g->n_nodes || !bit(S, exit)) return X_NO;
    for (uint32_t n = 0; n < g->n_nodes; n++) {
        if (!bit(S, n)) continue;
        const AgNode *x = &g->nodes[n];
        if (!x->alive || !eligible_kind(x->kind)) return X_NO;
        if (n != exit && (pinned(g, n) || x->kind == AG_VERIFY)) return X_NO;
    }
    for (uint32_t d = 0; d < g->n_deps; d++) {
        if (bit(S, g->deps[d].to)) return X_NO;                       /* control into a step */
        if (bit(S, g->deps[d].from) && g->deps[d].from != exit) return X_NO;
    }
    for (uint32_t e = 0; e < g->n_data; e++) {
        const AgDataEdge *d = &g->data[e];
        if (bit(S, d->from) && !bit(S, d->to) && d->from != exit) return X_NO;   /* second exit */
        if (bit(S, d->from) && bit(S, d->to) && d->mode != AG_EDGE_DATA) return X_NO;
    }
    Walk wk = { g, S, 0, 0, { 0 } };
    visit(&wk, exit);
    if (wk.visited != S || wk.n > AG_META_MAX_STEPS) return X_NO;   /* every step feeds the exit */
    if (wk.n < 2) return X_NO;

    int budget = 0;
    for (uint32_t j = 0; j < wk.n; j++) {
        uint32_t n = wk.order[j];
        AgMetaStep *q = &p->step[j];
        q->node = g->nodes[n];
        /* The step carries its meaning, not its place: no identity, origin,
         * parameter or bound object (those are the site's). */
        memset(q->node.id, 0, sizeof q->node.id);
        q->node.origin = 0;
        q->node.param = 0;
        q->node.obj = (RxObjRef){ 0, 0 };
        q->node.alive = 1;
        q->obj = -1;
        site->node[j] = (uint16_t)n;
        if (step_reads(g->nodes[n].kind)) {
            RxObjRef o = g->nodes[n].obj;
            int slot = -1;
            for (uint32_t i = 0; i < p->n_obj; i++)
                if (site->obj[i].id == o.id && site->obj[i].generation == o.generation) slot = (int)i;
            if (slot < 0) {
                if (p->n_obj >= AG_META_MAX_OBJ) { budget = 1; continue; }
                slot = (int)p->n_obj++;
                site->obj[slot] = o;
            }
            q->obj = slot;
            p->obj_rights[slot] |= RX_RIGHT_READ;
        }
        uint32_t e[AG_MAX_IN];
        q->n_in = inputs_by_port(g, n, e);
        for (uint32_t i = 0; i < q->n_in; i++) {
            const AgDataEdge *d = &g->data[e[i]];
            if (bit(S, d->from)) {
                uint32_t k = 0;
                while (k < j && wk.order[k] != d->from) k++;
                if (k >= j) return X_NO;
                q->in[i].step = 1;
                q->in[i].index = (uint8_t)k;
                q->in[i].mode = AG_EDGE_DATA;
                continue;
            }
            int port = -1;
            for (uint32_t k = 0; k < site->n_ext; k++)
                if (site->ext[k] == d->from) port = (int)k;
            if (port >= 0) {
                if (p->in_mode[port] != d->mode) return X_NO;   /* one source, two meanings */
            } else {
                if (site->n_ext >= AG_MAX_IN) { budget = 1; continue; }
                port = (int)site->n_ext++;
                site->ext[port] = d->from;
                p->in_type[port] = g->nodes[d->from].out_type;
                p->in_mode[port] = d->mode;
            }
            q->in[i].step = 0;
            q->in[i].index = (uint8_t)port;
            q->in[i].mode = d->mode;
        }
    }
    p->n_in = site->n_ext;
    site->n_obj = p->n_obj;
    /* One lowered reaction: run token, own cell, one per port, one per
     * object, and room for an authority slot wake. */
    if (budget || site->n_ext + 2u + p->n_obj + (p->n_obj ? 1u : 0u) > RX_MAX_DEPS) return X_BUDGET;

    /* Authority pattern: READ on the objects the steps read, nothing else. */
    for (uint32_t i = 0; i < g->n_auth; i++) {
        if (!bit(S, g->auth[i].node)) continue;
        if (g->auth[i].rights != RX_RIGHT_READ) return X_NO;
        int found = 0;
        for (uint32_t k = 0; w && k < p->n_obj; k++)
            if (site->obj[k].id < RX_MAX_OBJECTS &&
                w->objects[site->obj[k].id].resource == g->auth[i].resource)
                found = 1;
        if (w && !found) return X_NO;
    }
    /* Resource needs, merged as one reaction would carry them. */
    for (uint32_t i = 0; i < g->n_res; i++) {
        if (!bit(S, g->res[i].node)) continue;
        const RxResourceNeed *q = &g->res[i].need;
        RxResourceNeed *m = &p->need;
        if (q->compute_class && m->compute_class && q->compute_class != m->compute_class) return X_NO;
        if (q->compute_class) m->compute_class = q->compute_class;
        m->locality |= q->locality;
        m->accelerator_features |= q->accelerator_features;
        if (q->latency_class > m->latency_class) m->latency_class = q->latency_class;
        if (q->memory_bytes > m->memory_bytes) m->memory_bytes = q->memory_bytes;
        if (q->deadline && (!m->deadline || q->deadline < m->deadline)) m->deadline = q->deadline;
        m->energy_cost += q->energy_cost;
    }
    p->n_steps = wk.n;
    p->out_type = g->nodes[exit].out_type;
    rx_graph_meta_seal(p);
    site->members = S;
    site->exit = exit;
    site->n_steps = wk.n;
    return X_OK;
}

/* ---- fragments: enumeration ----
 * From each possible exit, grow backwards by adding a producer whose every
 * consumer is already inside. Every set is convex (nothing leaves and comes
 * back) and has one exit by construction; extract() checks the rest. */

typedef int (*SetFn)(void *ctx, uint64_t set, uint32_t exit);

#define SEEN_MAX 512u
typedef struct { uint32_t n; uint64_t s[SEEN_MAX]; } Seen;

static int seen_add(Seen *sn, uint64_t s) {
    for (uint32_t i = 0; i < sn->n; i++)
        if (sn->s[i] == s) return 0;
    if (sn->n >= SEEN_MAX) return 0;
    sn->s[sn->n++] = s;
    return 1;
}

static int addable(const AgGraph *g, uint64_t S, uint32_t p) {
    for (uint32_t e = 0; e < g->n_data; e++)
        if (g->data[e].from == p && !bit(S, g->data[e].to)) return 0;
    for (uint32_t d = 0; d < g->n_deps; d++)
        if (g->deps[d].from == p || g->deps[d].to == p) return 0;
    return 1;
}

static int grow(const AgGraph *g, uint64_t elig, uint64_t S, uint32_t exit, Seen *sn, SetFn fn,
                void *ctx) {
    if (!seen_add(sn, S)) return 0;
    if (fn(ctx, S, exit)) return 1;
    if ((uint32_t)__builtin_popcountll(S) >= AG_META_MAX_STEPS) return 0;
    for (uint32_t e = 0; e < g->n_data; e++) {
        const AgDataEdge *d = &g->data[e];
        if (!bit(S, d->to) || bit(S, d->from) || !bit(elig, d->from) || d->mode != AG_EDGE_DATA) continue;
        if (!addable(g, S, d->from)) continue;
        if (grow(g, elig, S | (1ull << d->from), exit, sn, fn, ctx)) return 1;
    }
    return 0;
}

static void enumerate(const AgGraph *g, SetFn fn, void *ctx) {
    uint64_t elig = 0;
    for (uint32_t n = 0; n < g->n_nodes; n++)
        if (g->nodes[n].alive && eligible_kind(g->nodes[n].kind) && !boundary_kind(g->nodes[n].kind))
            elig |= 1ull << n;
    static __thread Seen sn;
    for (uint32_t x = 0; x < g->n_nodes; x++) {
        if (!bit(elig, x)) continue;
        sn.n = 0;
        if (grow(g, elig, 1ull << x, x, &sn, fn, ctx)) return;
    }
}

/* ---- observation ---- */

void rx_fusion_observer_init(AgFusionObserver *obs) { memset(obs, 0, sizeof *obs); }

uint64_t rx_fusion_site_steps(const AgGraph *g, const AgFusionSite *site, const AgMetaProgram *p,
                              const AgResult *r) {
    uint64_t acc = 0;
    for (uint32_t j = 0; j < p->n_steps && j < site->n_steps; j++) {
        uint32_t n = site->node[j];
        uint32_t e[AG_MAX_IN];
        uint32_t k = inputs_by_port(g, n, e);
        uint64_t in[AG_MAX_IN];
        for (uint32_t i = 0; i < k; i++) in[i] = r->value[g->data[e[i]].from];
        acc = rx_graph_step_record(acc, p->step[j].local_id, r->status[n], r->value[n], in, k);
    }
    return acc;
}

static void site_inputs(const AgGraph *g, const AgFusionSite *site, const AgMetaProgram *p,
                        const AgResult *r, const AgReference *ref, AgFusionSample *s) {
    memset(s, 0, sizeof *s);
    for (uint32_t i = 0; i < site->n_ext; i++) {
        s->s[i] = r->status[site->ext[i]];
        s->v[i] = r->value[site->ext[i]];
    }
    for (uint32_t j = 0; j < p->n_steps; j++)
        if (p->step[j].obj >= 0)
            memcpy(s->world[p->step[j].obj], ref->world_seen[site->node[j]][0], sizeof s->world[0]);
    (void)g;
}

static int same_key(const AgFusionSample *a, const AgFusionSample *b, uint32_t n_in) {
    for (uint32_t i = 0; i < n_in; i++)
        if (a->s[i] != b->s[i] || a->v[i] != b->v[i]) return 0;
    return memcmp(a->world, b->world, sizeof a->world) == 0;
}

typedef struct {
    AgFusionObserver *obs;
    const RxWorld *w;
    const AgGraph *g;
    const AgResult *got;
    const AgReference *ref;
} ObsCtx;

static int observe_set(void *vc, uint64_t S, uint32_t exit) {
    ObsCtx *c = vc;
    AgFusionObserver *obs = c->obs;
    static __thread AgMetaProgram p;
    AgFusionSite site;
    int rc = extract(c->g, c->w, S, exit, &p, &site);
    if (rc == X_BUDGET) { obs->fragments_over_budget++; return 0; }
    if (rc != X_OK) return 0;
    obs->fragments_seen++;
    for (uint32_t j = 0; j < site.n_steps; j++)
        if (c->got->status[site.node[j]] == AG_PENDING) return 0;     /* did not run */
    AgFusionPattern *pat = NULL;
    for (uint32_t i = 0; i < obs->n; i++)
        if (memcmp(obs->p[i].prog.signature, p.signature, 32) == 0) pat = &obs->p[i];
    if (!pat) {
        if (obs->n >= AG_FUSION_MAX_PATTERNS) { obs->patterns_dropped++; return 0; }
        pat = &obs->p[obs->n++];
        memset(pat, 0, sizeof *pat);
        pat->prog = p;
    }
    pat->occurrences++;
    int known = 0;
    for (uint32_t i = 0; i < pat->n_graphs; i++) known |= memcmp(pat->graphs[i], c->g->digest, 32) == 0;
    if (!known && pat->n_graphs < AG_FUSION_MAX_GRAPHS) memcpy(pat->graphs[pat->n_graphs++], c->g->digest, 32);
    if (pat->n_ancestry < AG_FUSION_MAX_ANCESTRY) {
        AgAncestry *a = &pat->ancestry[pat->n_ancestry++];
        memcpy(a->graph, c->g->digest, 32);
        a->run = c->got->run;
        a->exit_origin = c->g->nodes[exit].origin;
    }
    uint8_t st = c->got->status[exit];
    if (st == AG_OK) pat->ok++;
    else if (st == AG_FAILED) pat->failed++;
    else if (st == AG_SKIPPED) pat->skipped++;

    AgFusionSample s;
    site_inputs(c->g, &site, &p, c->got, c->ref, &s);
    s.status = st;
    s.value = c->got->value[exit];
    s.attempts = c->got->attempts[exit];
    s.steps = rx_fusion_site_steps(c->g, &site, &p, c->got);
    /* The resident run and the reference must tell the same step story. */
    if (rx_fusion_site_steps(c->g, &site, &p, &c->ref->r) != s.steps) {
        pat->evidence_unstable++;
        return 0;
    }
    for (uint32_t i = 0; i < pat->n_samples; i++) {
        const AgFusionSample *q = &pat->sample[i];
        if (!same_key(q, &s, p.n_in)) continue;
        if (q->status == s.status && q->value == s.value && q->steps == s.steps &&
            q->attempts == s.attempts)
            pat->evidence_stable++;
        else
            pat->evidence_unstable++;
        return 0;
    }
    if (pat->n_samples < AG_FUSION_MAX_SAMPLES) pat->sample[pat->n_samples++] = s;
    return 0;
}

int rx_fusion_observe(AgFusionObserver *obs, const RxWorld *w, const AgGraph *g,
                      const AgResult *got, const AgReference *ref) {
    int ok = got->outcome != AG_RUN_INCOMPLETE && got->outcome == ref->r.outcome;
    for (uint32_t n = 0; ok && n < g->n_nodes; n++)
        if (got->status[n] != ref->r.status[n] || got->value[n] != ref->r.value[n] ||
            (got->status[n] && got->evidence[n] != ref->r.evidence[n]) ||
            got->steps[n] != ref->r.steps[n])
            ok = 0;
    if (ok && got->outcome == AG_RUN_SUCCESS)
        for (uint32_t i = 0; i < g->n_evidence; i++)
            if (got->status[g->evidence[i]] == AG_PENDING || got->evidence[g->evidence[i]] == 0) ok = 0;
    if (!ok) {
        obs->runs_refused++;
        return 0;
    }
    obs->runs_observed++;
    ObsCtx c = { obs, w, g, got, ref };
    enumerate(g, observe_set, &c);
    return 1;
}

int rx_fusion_judge(const AgFusionObserver *obs, uint32_t i, const AgFusionPolicy *pol) {
    const AgFusionPattern *p = &obs->p[i];
    uint32_t min_steps = pol->min_steps > 2 ? pol->min_steps : 2;
    if (p->prog.n_steps < min_steps) return AG_FUSE_TOO_SMALL;
    /* What the fragment is comes before how often it was seen. */
    if (p->evidence_unstable) return AG_FUSE_UNSTABLE;
    uint64_t decided = (uint64_t)p->ok + p->failed;
    if (!decided || (uint64_t)p->failed * 1000u > (uint64_t)pol->max_fail_permille * decided)
        return AG_FUSE_FAILURE_RATE;
    if (p->occurrences < pol->min_occurrences) return AG_FUSE_TOO_FEW;
    if (p->n_graphs < pol->min_graphs) return AG_FUSE_TOO_FEW_GRAPHS;
    return AG_FUSE_ELIGIBLE;
}

uint32_t rx_fusion_candidates(const AgFusionObserver *obs, const AgFusionPolicy *pol,
                              uint32_t *idx, uint32_t max) {
    uint32_t k = 0;
    for (uint32_t i = 0; i < obs->n && k < max; i++)
        if (rx_fusion_judge(obs, i, pol) == AG_FUSE_ELIGIBLE) idx[k++] = i;
    for (uint32_t a = 1; a < k; a++)
        for (uint32_t b = a; b > 0; b--) {
            const AgFusionPattern *x = &obs->p[idx[b - 1]], *y = &obs->p[idx[b]];
            int cmp = x->prog.n_steps != y->prog.n_steps ? (int)y->prog.n_steps - (int)x->prog.n_steps
                    : x->occurrences != y->occurrences ? (int)y->occurrences - (int)x->occurrences
                    : memcmp(x->prog.signature, y->prog.signature, 32);
            if (cmp <= 0) break;
            uint32_t t = idx[b]; idx[b] = idx[b - 1]; idx[b - 1] = t;
        }
    return k;
}

/* ---- candidate ---- */

static uint32_t count_kind(const AgMetaProgram *p, AgKind k) {
    uint32_t n = 0;
    for (uint32_t j = 0; j < p->n_steps && j < AG_META_MAX_STEPS; j++) n += p->step[j].node.kind == k;
    return n;
}

static int may_fail(const AgMetaProgram *p) {
    if (p->n_in) return 1;
    for (uint32_t j = 0; j < p->n_steps && j < AG_META_MAX_STEPS; j++) {
        AgKind k = p->step[j].node.kind;
        if (k == AG_VERIFY || k == AG_RECALL || k == AG_RETRY || k == AG_SKILL || k == AG_PURE) return 1;
    }
    return 0;
}

/* Contracts are derived from the fragment, never chosen. */
static void derive_contracts(const AgMetaProgram *q, AgMetaSkill *m) {
    m->input_contract.n = q->n_in;
    for (uint32_t i = 0; i < AG_MAX_IN; i++) {
        m->input_contract.type[i] = q->in_type[i];
        m->input_contract.mode[i] = q->in_mode[i];
    }
    m->output_contract.type = q->out_type;
    m->output_contract.may_fail = (uint8_t)may_fail(q);
    m->authority_contract.n_obj = q->n_obj;
    for (uint32_t i = 0; i < AG_META_MAX_OBJ; i++) m->authority_contract.rights[i] = q->obj_rights[i];
    m->resource_contract = q->need;
    m->effect_contract.performs = count_kind(q, AG_EFFECT_PERFORM);
    m->effect_contract.publishes = count_kind(q, AG_WORLD_PUBLISH);
    m->effect_contract.proposals = count_kind(q, AG_EFFECT_PROPOSE);
}

void rx_fusion_identity(const AgMetaSkill *m, uint8_t out[32]) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)"OMEGA_METASKILL_V1", 18);
    put32(&c, m->id);
    put32(&c, m->input_contract.n);
    for (uint32_t i = 0; i < AG_MAX_IN; i++) {
        put32(&c, m->input_contract.type[i]);
        put8(&c, m->input_contract.mode[i]);
    }
    put32(&c, m->output_contract.type);
    put8(&c, m->output_contract.may_fail);
    put32(&c, m->authority_contract.n_obj);
    for (uint32_t i = 0; i < AG_META_MAX_OBJ; i++) put32(&c, m->authority_contract.rights[i]);
    const RxResourceNeed *r = &m->resource_contract;
    put32(&c, r->compute_class);
    put32(&c, r->locality);
    put32(&c, r->accelerator_features);
    put32(&c, r->latency_class);
    put64(&c, r->memory_bytes);
    put64(&c, r->deadline);
    put64(&c, r->energy_cost);
    put32(&c, m->effect_contract.performs);
    put32(&c, m->effect_contract.publishes);
    put32(&c, m->effect_contract.proposals);
    sha256_update(&c, m->realization.identity, 32);
    sha256_update(&c, m->reference.signature, 32);
    put32(&c, m->n_ancestry);
    for (uint32_t i = 0; i < m->n_ancestry && i < AG_FUSION_MAX_ANCESTRY; i++) {
        sha256_update(&c, m->ancestry[i].graph, 32);
        put64(&c, m->ancestry[i].run);
        put32(&c, m->ancestry[i].exit_origin);
    }
    sha256_final(&c, out);
}

int rx_fusion_build(const AgFusionPattern *p, AgRealKind kind, uint32_t skill_id, AgMetaSkill *out) {
    if (!p || !out || p->prog.n_steps < 2 || kind < AG_REAL_COMPILED || kind > AG_REAL_HARDWARE)
        return AG_E_ARG;
    memset(out, 0, sizeof *out);
    out->id = skill_id;
    out->state = AG_MS_CANDIDATE;
    out->n_ancestry = p->n_ancestry;
    memcpy(out->ancestry, p->ancestry, sizeof out->ancestry);
    derive_contracts(&p->prog, out);
    out->reference = p->prog;
    out->n_samples = p->n_samples;
    memcpy(out->samples, p->sample, sizeof out->samples);
    out->realization.kind = kind;
    out->realization.prog = p->prog;
    if (kind == AG_REAL_LEARNED || kind == AG_REAL_HYBRID)
        for (uint32_t i = 0; i < p->n_samples && i < AG_META_TABLE; i++) {
            const AgFusionSample *s = &p->sample[i];
            AgMetaEntry *t = &out->realization.table[out->realization.n_table++];
            memcpy(t->s, s->s, sizeof t->s);
            memcpy(t->v, s->v, sizeof t->v);
            t->world = rx_graph_meta_world_key(&p->prog, s->world);
            t->status = s->status;
            t->value = s->value;
            t->steps = s->steps;
            t->attempts = s->attempts;
        }
    rx_graph_realization_identity(&out->realization);
    rx_fusion_identity(out, out->identity);
    return 0;
}

/* ---- verification ---- */

#define FAIL_SKILL 0xFFFFFF01u

static uint64_t always_fails(const uint64_t *in, uint32_t n, uint32_t attempt, int *failed) {
    (void)in; (void)n; (void)attempt;
    *failed = 1;
    return 0;
}

static uint64_t rng(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *s = x;
}

/* Port drivers: each port gets a node of its contract type that ends OK
 * with value v, FAILED, or SKIPPED. */
static int drivers(AgGraph *g, const AgMetaProgram *p, const uint8_t *s, const uint64_t *v,
                   uint16_t *port_node) {
    int br = -1;
    for (uint32_t i = 0; i < p->n_in; i++) {
        int base = rx_graph_node(g, AG_CONST, AG_T_U64);
        if (base < 0) return -1;
        g->nodes[base].imm = v[i];
        int src = base;
        if (s[i] == AG_FAILED) {
            int k = rx_graph_node(g, AG_SKILL, AG_T_U64);
            if (k < 0) return -1;
            g->nodes[k].op = FAIL_SKILL;
            rx_graph_data(g, (uint32_t)base, (uint32_t)k, 0, AG_EDGE_DATA);
            src = k;
        }
        int c;
        switch (p->in_type[i]) {
        case AG_T_U64: case AG_T_BOOL:
            c = rx_graph_node(g, AG_PURE, p->in_type[i]);
            if (c >= 0) g->nodes[c].op = OP_IDENTITY;
            break;
        case AG_T_PROPOSAL:
            c = rx_graph_node(g, AG_EFFECT_PROPOSE, AG_T_PROPOSAL);
            break;
        case AG_T_VERDICT:
            c = rx_graph_node(g, AG_VERIFY, AG_T_VERDICT);
            if (c >= 0) g->nodes[c].imm2 = UINT64_MAX;
            break;
        default:
            return -1;
        }
        if (c < 0) return -1;
        rx_graph_data(g, (uint32_t)src, (uint32_t)c, 0, AG_EDGE_DATA);
        if (s[i] == AG_SKIPPED) {
            if (br < 0) {
                int zero = rx_graph_node(g, AG_CONST, AG_T_U64);
                br = rx_graph_node(g, AG_BRANCH, AG_T_BOOL);
                if (zero < 0 || br < 0) return -1;
                rx_graph_data(g, (uint32_t)zero, (uint32_t)br, 0, AG_EDGE_DATA);
            }
            rx_graph_guard(g, (uint32_t)br, (uint32_t)c, 1);
        }
        port_node[i] = (uint16_t)c;
    }
    return 0;
}

#define VOBJ_ID 250u

/* The original fragment as graph nodes after its drivers. */
static int harness_original(AgGraph *g, const AgMetaProgram *p, const uint8_t *s, const uint64_t *v,
                            AgFusionSite *site) {
    rx_graph_init(g, 0);
    memset(site, 0, sizeof *site);
    uint16_t port[AG_MAX_IN];
    if (drivers(g, p, s, v, port) != 0) return -1;
    for (uint32_t j = 0; j < p->n_steps; j++) {
        const AgMetaStep *q = &p->step[j];
        int n = rx_graph_node(g, q->node.kind, q->node.out_type);
        if (n < 0) return -1;
        AgNode *x = &g->nodes[n];
        x->op = q->node.op;
        x->imm = q->node.imm;
        x->imm2 = q->node.imm2;
        x->field = q->node.field;
        x->cost_us = 0;             /* verification checks meaning, not time */
        x->n_fused = q->node.n_fused;
        memcpy(x->fused, q->node.fused, sizeof x->fused);
        if (q->obj >= 0) x->obj = (RxObjRef){ VOBJ_ID + (uint32_t)q->obj, 1 };
        for (uint32_t i = 0; i < q->n_in; i++) {
            uint32_t from = q->in[i].step ? site->node[q->in[i].index] : port[q->in[i].index];
            rx_graph_data(g, from, (uint32_t)n, i, q->in[i].step ? AG_EDGE_DATA : q->in[i].mode);
        }
        site->node[j] = (uint16_t)n;
    }
    site->n_steps = p->n_steps;
    site->exit = site->node[p->n_steps - 1];
    return rx_graph_validate(g, NULL);
}

/* The same drivers and one AG_META node running the candidate. */
static int harness_fused(AgGraph *g, const AgMetaSkill *m, const uint8_t *s, const uint64_t *v,
                         uint32_t *meta) {
    const AgMetaProgram *p = &m->realization.prog;
    rx_graph_init(g, 0);
    uint16_t port[AG_MAX_IN];
    if (drivers(g, p, s, v, port) != 0) return -1;
    int n = rx_graph_node(g, AG_META, p->out_type);
    if (n < 0) return -1;
    AgNode *x = &g->nodes[n];
    x->op = m->id;
    x->imm = word64(m->realization.identity);
    x->imm2 = word64(m->realization.identity + 8);
    x->n_mobj = p->n_obj;
    for (uint32_t i = 0; i < p->n_obj; i++) x->mobj[i] = (RxObjRef){ VOBJ_ID + i, 1 };
    for (uint32_t i = 0; i < p->n_in; i++) {
        x->in_type[i] = p->in_type[i];
        rx_graph_data(g, port[i], (uint32_t)n, i, p->in_mode[i]);
    }
    *meta = (uint32_t)n;
    return rx_graph_validate(g, NULL);
}

static int prog_equal_ids(const AgMetaProgram *a, const AgMetaProgram *b) {
    if (a->n_steps != b->n_steps || memcmp(a->signature, b->signature, 32) != 0) return 0;
    for (uint32_t j = 0; j < a->n_steps && j < AG_META_MAX_STEPS; j++)
        if (memcmp(a->step[j].local_id, b->step[j].local_id, 32) != 0) return 0;
    return 1;
}

typedef struct {
    AgGraph h0, h1;
    AgReference r0, r1;
    AgSkillTable t0, t1;
} Bench;

static Bench g_bench;   /* verification is not re-entrant */

static void run_vector(AgMetaSkill *m, Bench *b, const uint8_t *s, const uint64_t *v,
                       const uint64_t world[AG_META_MAX_OBJ][RX_MAX_FIELDS]) {
    AgFusionVerdict *vd = &m->verdict;
    vd->vectors++;
    AgFusionSite site;
    uint32_t meta = 0;
    if (harness_original(&b->h0, &m->reference, s, v, &site) != 0 ||
        harness_fused(&b->h1, m, s, v, &meta) != 0) {
        vd->semantic_mismatch++;
        return;
    }
    AgObjContents objs[AG_META_MAX_OBJ];
    for (uint32_t i = 0; i < AG_META_MAX_OBJ; i++) {
        objs[i].obj = (RxObjRef){ VOBJ_ID + i, 1 };
        memcpy(objs[i].field, world[i], sizeof objs[i].field);
    }
    rx_graph_reference_on(&b->h0, &b->t0, objs, AG_META_MAX_OBJ, 1, &b->r0);
    rx_graph_reference_on(&b->h1, &b->t1, objs, AG_META_MAX_OBJ, 1, &b->r1);
    /* What the ports actually carried (a VERDICT is 1 when OK). */
    uint8_t ps[AG_MAX_IN];
    uint64_t pv[AG_MAX_IN];
    int any_bad = 0;
    for (uint32_t i = 0; i < m->reference.n_in; i++) {
        uint32_t e[AG_MAX_IN];
        (void)e;
        uint32_t pn = 0;
        for (uint32_t d = 0; d < b->h1.n_data; d++)
            if (b->h1.data[d].to == meta && b->h1.data[d].port == i) pn = b->h1.data[d].from;
        ps[i] = b->r1.r.status[pn];
        pv[i] = b->r1.r.value[pn];
        any_bad |= ps[i] != AG_OK;
    }
    AgMetaOut mo;
    if (rx_graph_meta_run(&m->realization, &b->t1, ps, pv, m->reference.n_in, world, &mo) != 0)
        vd->not_runnable++;
    vd->table_hits += (uint32_t)mo.table_hit;
    uint32_t x = site.exit;
    uint8_t es = b->r0.r.status[x], gs = b->r1.r.status[meta];
    if (es != gs || b->r0.r.value[x] != b->r1.r.value[meta] ||
        b->r0.r.attempts[x] != b->r1.r.attempts[meta]) {
        if (any_bad || es != AG_OK) vd->failure_mismatch++;
        else vd->semantic_mismatch++;
        return;
    }
    if (es != AG_PENDING &&
        rx_fusion_site_steps(&b->h0, &site, &m->reference, &b->r0.r) != b->r1.r.steps[meta])
        vd->evidence_mismatch++;
}

int rx_fusion_verify(AgMetaSkill *m, const AgSkillTable *skills, uint64_t seed) {
    if (!m || m->state != AG_MS_CANDIDATE) return AG_FUSION_E_STATE;
    AgFusionVerdict *vd = &m->verdict;
    memset(vd, 0, sizeof *vd);
    const AgMetaProgram *ref = &m->reference;
    const AgRealization *rz = &m->realization;

    /* 1. Identity: what it carries is what it is. */
    static __thread AgMetaProgram pr;
    static __thread AgRealization rr;
    pr = *ref;
    rx_graph_meta_seal(&pr);
    rr = *rz;
    rx_graph_meta_seal(&rr.prog);
    rx_graph_realization_identity(&rr);
    uint8_t id[32];
    rx_fusion_identity(m, id);
    vd->identity_ok = prog_equal_ids(&pr, ref) && prog_equal_ids(&rr.prog, &rz->prog) &&
                      memcmp(rr.identity, rz->identity, 32) == 0 && memcmp(id, m->identity, 32) == 0;

    /* 2. Contracts: derived from the reference fragment, and the realization
     * serves that fragment. */
    static __thread AgMetaSkill d;
    memset(&d, 0, sizeof d);
    derive_contracts(ref, &d);
    vd->contract_ok = ref->n_steps >= 2 && ref->n_steps <= AG_META_MAX_STEPS &&
                      memcmp(&d.input_contract, &m->input_contract, sizeof d.input_contract) == 0 &&
                      d.output_contract.type == m->output_contract.type &&
                      d.output_contract.may_fail == m->output_contract.may_fail &&
                      memcmp(rz->prog.signature, ref->signature, 32) == 0;
    vd->authority_equal = m->authority_contract.n_obj == ref->n_obj &&
                          memcmp(m->authority_contract.rights, ref->obj_rights,
                                 sizeof m->authority_contract.rights) == 0 &&
                          memcmp(rz->prog.obj_rights, ref->obj_rights, sizeof ref->obj_rights) == 0;
    for (uint32_t i = 0; i < ref->n_obj && i < AG_META_MAX_OBJ; i++)
        if (ref->obj_rights[i] != RX_RIGHT_READ) vd->authority_equal = 0;
    vd->effect_equal = d.effect_contract.performs == 0 && d.effect_contract.publishes == 0 &&
                       count_kind(&rz->prog, AG_EFFECT_PERFORM) == 0 &&
                       count_kind(&rz->prog, AG_WORLD_PUBLISH) == 0 &&
                       memcmp(&d.effect_contract, &m->effect_contract, sizeof d.effect_contract) == 0;
    vd->resource_equal = memcmp(&d.resource_contract, &m->resource_contract, sizeof d.resource_contract) == 0 &&
                         memcmp(&rz->prog.need, &ref->need, sizeof ref->need) == 0;
    if (!vd->identity_ok) vd->reason = AG_VR_IDENTITY;
    else if (!vd->contract_ok) vd->reason = AG_VR_CONTRACT;
    else if (!vd->authority_equal) vd->reason = AG_VR_AUTHORITY;
    else if (!vd->effect_equal) vd->reason = AG_VR_EFFECT;
    else if (!vd->resource_equal) vd->reason = AG_VR_RESOURCE;
    if (vd->reason) {
        m->state = AG_MS_REJECTED;
        return 0;
    }

    /* 3. Differential: the fragment step by step against the candidate. */
    Bench *b = &g_bench;
    b->t0 = skills ? *skills : (AgSkillTable){ 0 };
    b->t0.n_meta = 0;
    if (b->t0.n < AG_MAX_SKILLS) b->t0.skill[b->t0.n++] = (AgSkill){ FAIL_SKILL, always_fails, { 0 } };
    b->t1 = b->t0;
    b->t1.meta[b->t1.n_meta++] = (typeof(b->t1.meta[0])){ m->id, &m->realization };

    uint32_t n_in = ref->n_in;
    /* Observed inputs, as they were: the fragment must reproduce what was
     * recorded, and the candidate must agree with the fragment. */
    for (uint32_t i = 0; i < m->n_samples && i < AG_FUSION_MAX_SAMPLES; i++) {
        const AgFusionSample *q = &m->samples[i];
        run_vector(m, b, q->s, q->v, (const uint64_t (*)[RX_MAX_FIELDS])q->world);
        vd->observed++;
        AgFusionSite site;
        if (harness_original(&b->h0, ref, q->s, q->v, &site) == 0) {
            AgObjContents objs[AG_META_MAX_OBJ];
            for (uint32_t o = 0; o < AG_META_MAX_OBJ; o++) {
                objs[o].obj = (RxObjRef){ VOBJ_ID + o, 1 };
                memcpy(objs[o].field, q->world[o], sizeof objs[o].field);
            }
            rx_graph_reference_on(&b->h0, &b->t0, objs, AG_META_MAX_OBJ, 1, &b->r0);
            if (b->r0.r.status[site.exit] != q->status || b->r0.r.value[site.exit] != q->value ||
                rx_fusion_site_steps(&b->h0, &site, ref, &b->r0.r) != q->steps)
                vd->semantic_mismatch++;
        }
    }
    /* Generated: every status combination over the ports, several value and
     * World sets each (zeros, ones, all-ones, random, VERIFY/imm bounds). */
    uint64_t bounds[16];
    uint32_t nb = 0;
    for (uint32_t j = 0; j < ref->n_steps && nb + 4 <= 16; j++) {
        const AgNode *x = &ref->step[j].node;
        if (x->kind == AG_VERIFY || x->kind == AG_RECALL || x->kind == AG_PURE) {
            bounds[nb++] = x->imm;
            bounds[nb++] = x->imm - 1;
            bounds[nb++] = x->imm2;
            bounds[nb++] = x->imm2 + 1;
        }
    }
    uint32_t combos = 1;
    for (uint32_t i = 0; i < n_in; i++) combos *= 3;
    vd->status_combos = combos;
    uint64_t rs = seed ? seed : 0x9E3779B97F4A7C15ull;
    const uint32_t sets = 6 + nb + 24;   /* then 24 sets over the full range */
    for (uint32_t c = 0; c < combos; c++) {
        uint8_t s[AG_MAX_IN] = { 0 };
        uint32_t cc = c;
        for (uint32_t i = 0; i < n_in; i++) {
            s[i] = (uint8_t)(cc % 3 == 0 ? AG_OK : cc % 3 == 1 ? AG_FAILED : AG_SKIPPED);
            cc /= 3;
        }
        for (uint32_t k = 0; k < sets; k++) {
            uint64_t v[AG_MAX_IN] = { 0 };
            uint64_t world[AG_META_MAX_OBJ][RX_MAX_FIELDS];
            memset(world, 0, sizeof world);
            for (uint32_t i = 0; i < n_in; i++) {
                v[i] = k == 0 ? 0 : k == 1 ? 1 : k == 2 ? UINT64_MAX
                     : k < 6 ? rng(&rs) % (k == 3 ? 1000u : k == 4 ? 1000000u : UINT32_MAX)
                     : k < 6 + nb ? bounds[k - 6] : rng(&rs);
                if (s[i] != AG_OK) v[i] = 0;
            }
            for (uint32_t o = 0; o < ref->n_obj; o++)
                for (uint32_t f = 0; f < RX_MAX_FIELDS; f++)
                    world[o][f] = k == 0 ? 0 : k == 1 ? 1 : k == 2 ? UINT64_MAX
                                : k < 6 ? rng(&rs) % (k == 3 ? 1000u : k == 4 ? 1000000u : UINT32_MAX)
                                : k < 6 + nb ? bounds[k - 6] : rng(&rs);
            /* Recall keys the steps look for, present in half the sets. */
            for (uint32_t j = 0; j < ref->n_steps; j++) {
                const AgMetaStep *q = &ref->step[j];
                if (q->node.kind != AG_RECALL || q->obj < 0 || (k & 1u)) continue;
                uint64_t key = q->n_in ? v[q->in[0].index] : q->node.imm;
                if (q->n_in && q->in[0].step) key = world[q->obj][0];
                world[q->obj][2] = key;
                world[q->obj][3] = rng(&rs) % 100000u;
            }
            run_vector(m, b, s, v, (const uint64_t (*)[RX_MAX_FIELDS])world);
            vd->generated++;
        }
    }
    vd->pass = vd->semantic_mismatch == 0 && vd->failure_mismatch == 0 && vd->evidence_mismatch == 0 &&
               vd->not_runnable == 0;
    if (!vd->pass)
        vd->reason = vd->not_runnable ? AG_VR_NOT_RUNNABLE
                   : vd->semantic_mismatch ? AG_VR_SEMANTIC
                   : vd->failure_mismatch ? AG_VR_FAILURE : AG_VR_EVIDENCE;
    m->state = vd->pass ? AG_MS_VERIFIED : AG_MS_REJECTED;
    return 0;
}

/* ---- measurement, canary ---- */

int rx_fusion_accept_measure(AgMetaSkill *m, const AgFusionMeasure *ms, uint32_t tol) {
    if (!m || m->state != AG_MS_VERIFIED) return AG_FUSION_E_STATE;
    m->measure = *ms;
    int fewer = ms->commits_after < ms->commits_before && ms->crumbs_after < ms->crumbs_before;
    int in_time = (uint64_t)ms->median_ns_after * 1000u <= (uint64_t)ms->median_ns_before * (1000u + tol);
    int same = ms->ok_before == ms->ok_after && ms->failed_before == ms->failed_after;
    m->state = fewer && in_time && same ? AG_MS_MEASURED : AG_MS_SLOWER;
    return m->state == AG_MS_MEASURED ? 0 : -1;
}

int rx_fusion_canary(AgMetaSkill *m, int diverged, uint32_t required) {
    if (!m || m->state != AG_MS_MEASURED) return AG_FUSION_E_STATE;
    m->canary_runs++;
    if (diverged) {
        m->canary_divergences++;
        m->state = AG_MS_QUARANTINED;
        return -1;
    }
    if (m->canary_runs >= required && m->canary_divergences == 0) m->state = AG_MS_CANARY_PASSED;
    return 0;
}

/* ---- promotion and publication ---- */

static uint8_t g_blob[AG_REALIZATION_MAX_BYTES];

/* "model" blob: realization identity, MetaSkill identity. */
static void model_blob(const AgMetaSkill *m, uint8_t out[64]) {
    memcpy(out, m->realization.identity, 32);
    memcpy(out + 32, m->identity, 32);
}

int rx_fusion_promote(AgMetaSkill *m, RxGenStore *store, uint32_t proposer,
                      const RxPromotionRequest *req, RxGenAuthFn auth, void *auth_ctx) {
    if (!m || !store || !req || !auth) return RX_GEN_ERR_ARG;
    if (m->state != AG_MS_CANARY_PASSED) return AG_FUSION_E_STATE;
    size_t n = rx_graph_realization_encode(&m->realization, g_blob, sizeof g_blob);
    if (n > sizeof g_blob) return RX_GEN_ERR_ARG;
    uint8_t model[64];
    model_blob(m, model);
    /* Evidence: verification, measurement and canary figures as recorded. */
    uint8_t ev[sizeof(AgFusionVerdict) + sizeof(AgFusionMeasure) + 8];
    memcpy(ev, &m->verdict, sizeof m->verdict);
    memcpy(ev + sizeof m->verdict, &m->measure, sizeof m->measure);
    memcpy(ev + sizeof m->verdict + sizeof m->measure, &m->canary_runs, 4);
    memcpy(ev + sizeof m->verdict + sizeof m->measure + 4, &m->canary_divergences, 4);
    uint8_t prov[AG_FUSION_MAX_ANCESTRY * 44];
    size_t np = 0;
    for (uint32_t i = 0; i < m->n_ancestry && i < AG_FUSION_MAX_ANCESTRY; i++) {
        memcpy(prov + np, m->ancestry[i].graph, 32);
        memcpy(prov + np + 32, &m->ancestry[i].run, 8);
        memcpy(prov + np + 40, &m->ancestry[i].exit_origin, 4);
        np += 44;
    }
    RxGenObject obj;
    memset(&obj, 0, sizeof obj);
    obj.id = m->id;
    obj.generation = 1;
    memcpy(obj.digest, m->identity, 32);
    RxGenDraft d;
    memset(&d, 0, sizeof d);
    d.authority_epoch = 1;
    d.authority_generation = 1;
    d.proofs_ok = m->verdict.pass;
    d.objects = &obj;
    d.n_objects = 1;
    d.evidence = ev;
    d.evidence_len = sizeof ev;
    d.model = model;
    d.model_len = sizeof model;
    d.realization = g_blob;
    d.realization_len = n;
    d.config = (const uint8_t *)&m->authority_contract;
    d.config_len = sizeof m->authority_contract;
    d.provenance = prov;
    d.provenance_len = np;
    uint64_t cand = 0;
    int rc = rx_gen_propose(store, proposer, &d, &cand);
    if (rc != RX_GEN_OK) return rc;
    RxPromotionRequest r = *req;
    r.candidate_id = cand;
    rc = rx_gen_promote(store, &r, auth, auth_ctx, NULL, NULL, NULL, NULL);
    if (rc != RX_GEN_OK) return rc;
    m->generation = cand;
    m->state = AG_MS_PROMOTED;
    return RX_GEN_OK;
}

int rx_fusion_publish(AgMetaSkill *m, const RxGenStore *store, AgSkillTable *skills) {
    if (!m || !store || !skills) return AG_E_ARG;
    if (m->state != AG_MS_PROMOTED) return AG_FUSION_E_STATE;
    uint64_t active = 0, lineage = 0;
    if (rx_gen_active(store, &active, &lineage) != RX_GEN_OK || active != m->generation)
        return AG_FUSION_E_GENERATION;
    /* The MetaSkill as it is now must be the one that was promoted. */
    static __thread AgRealization rz;
    rz = m->realization;
    rx_graph_realization_identity(&rz);
    uint8_t id[32];
    rx_fusion_identity(m, id);
    if (memcmp(rz.identity, m->realization.identity, 32) != 0 || memcmp(id, m->identity, 32) != 0)
        return AG_FUSION_E_IDENTITY;
    uint8_t model[64], want[64];
    size_t len = 0;
    if (rx_gen_read_blob(store, m->generation, "model", model, sizeof model, &len) != RX_GEN_OK ||
        len != sizeof model)
        return AG_FUSION_E_IDENTITY;
    model_blob(m, want);
    if (memcmp(model, want, sizeof want) != 0) return AG_FUSION_E_IDENTITY;
    static uint8_t got[AG_REALIZATION_MAX_BYTES];
    size_t n = rx_graph_realization_encode(&m->realization, g_blob, sizeof g_blob);
    if (rx_gen_read_blob(store, m->generation, "realization", got, sizeof got, &len) != RX_GEN_OK ||
        len != n || memcmp(got, g_blob, n) != 0)
        return AG_FUSION_E_IDENTITY;
    uint32_t k = 0;
    while (k < skills->n_meta && skills->meta[k].id != m->id) k++;
    if (k >= AG_MAX_SKILLS) return AG_E_FULL;
    skills->meta[k].id = m->id;
    skills->meta[k].r = &m->realization;
    if (k == skills->n_meta) skills->n_meta++;
    m->state = AG_MS_PUBLISHED;
    return 0;
}

/* ---- use ---- */

typedef struct {
    const RxWorld *w;
    const uint8_t *sig;
    AgFusionSite *sites;
    uint32_t max, n;
    uint64_t taken;
} FindCtx;

static __thread const AgGraph *find_graph;

static int find_cb(void *vc, uint64_t S, uint32_t exit) {
    FindCtx *c = vc;
    if (c->n >= c->max) return 1;
    if (S & c->taken) return 0;             /* sites do not overlap */
    static __thread AgMetaProgram p;
    AgFusionSite site;
    if (extract(find_graph, c->w, S, exit, &p, &site) != X_OK) return 0;
    if (memcmp(p.signature, c->sig, 32) != 0) return 0;
    c->sites[c->n++] = site;
    c->taken |= S;
    return 0;
}

uint32_t rx_fusion_find(const AgGraph *g, const RxWorld *w, const uint8_t signature[32],
                        AgFusionSite *sites, uint32_t max) {
    FindCtx c = { w, signature, sites, max, 0, 0 };
    find_graph = g;
    enumerate(g, find_cb, &c);
    return c.n;
}

/* Fuse every site of one MetaSkill, checking authority stays exactly the
 * union of the steps'. */
static int fuse_all(AgGraph *g, RxWorld *w, const AgMetaSkill *m, AgFusionReport *rep) {
    for (uint32_t round = 0; round < AG_FUSION_MAX_APPLIED; round++) {
        AgFusionSite site;
        if (rx_fusion_find(g, w, m->reference.signature, &site, 1) == 0) return 0;
        /* Authority the steps needed. */
        AgAuthReq want[AG_MAX_REQS];
        uint32_t nw = 0;
        for (uint32_t i = 0; i < g->n_auth; i++)
            if (bit(site.members, g->auth[i].node)) {
                uint32_t k = 0;
                while (k < nw && want[k].resource != g->auth[i].resource) k++;
                if (k == nw) want[nw++] = (AgAuthReq){ 0, g->auth[i].resource, 0 };
                want[k].rights |= g->auth[i].rights;
            }
        int mi = rx_graph_fuse(g, site.members, site.exit, m->id, &m->realization, site.ext, site.n_ext,
                               site.obj, site.n_obj);
        if (mi < 0) return mi;
        int rc = rx_graph_validate(g, w);
        if (rc != 0) return rc;
        /* ... and what the fused node needs now: the same, no more, no less. */
        uint32_t have = 0;
        for (uint32_t i = 0; i < g->n_auth; i++) {
            if (g->auth[i].node != (uint32_t)mi) continue;
            int match = 0;
            for (uint32_t k = 0; k < nw; k++)
                match |= want[k].resource == g->auth[i].resource && want[k].rights == g->auth[i].rights;
            if (!match) return AG_FUSION_E_AUTHORITY;
            have++;
        }
        if (have != nw) return AG_FUSION_E_AUTHORITY;
        if (rep && rep->applied < AG_FUSION_MAX_APPLIED) {
            rep->fused[rep->applied].skill = m->id;
            rep->fused[rep->applied].steps = site.n_steps;
            rep->fused[rep->applied].node = (uint32_t)mi;
            rep->applied++;
        }
    }
    return 0;
}

static int published(const AgMetaSkill *m, const AgSkillTable *sk) {
    if (m->state != AG_MS_PUBLISHED) return 0;
    for (uint32_t i = 0; i < sk->n_meta && i < AG_MAX_SKILLS; i++)
        if (sk->meta[i].id == m->id && sk->meta[i].r == &m->realization) return 1;
    return 0;
}

int rx_fusion_apply(AgGraph *g, RxWorld *w, const AgFusionLibrary *lib, const AgSkillTable *sk,
                    AgFusionReport *rep) {
    AgFusionReport dummy;
    if (!rep) rep = &dummy;
    memset(rep, 0, sizeof *rep);
    for (uint32_t i = 0; lib && i < lib->n && i < AG_FUSION_MAX_LIBRARY; i++) {
        const AgMetaSkill *m = lib->m[i];
        if (!m) continue;
        if (!sk || !published(m, sk)) {
            rep->refused_unpublished++;         /* never silently: not published, not used */
            continue;
        }
        int rc = fuse_all(g, w, m, rep);
        if (rc != 0) return rep->verdict = rc;
    }
    return rep->verdict = 0;
}

int rx_fusion_trial(const AgGraph *g, RxWorld *w, const AgMetaSkill *m, const AgSkillTable *skills,
                    AgSkillTable *trial, AgGraph *out, uint32_t *applied) {
    if (!m || (m->state != AG_MS_VERIFIED && m->state != AG_MS_MEASURED)) return AG_FUSION_E_STATE;
    *trial = skills ? *skills : (AgSkillTable){ 0 };
    trial->n_meta = 0;
    trial->meta[trial->n_meta++] = (typeof(trial->meta[0])){ m->id, &m->realization };
    *out = *g;
    AgFusionReport rep;
    memset(&rep, 0, sizeof rep);
    int rc = fuse_all(out, w, m, &rep);
    if (applied) *applied = rep.applied;
    return rc;
}

int rx_fusion_compile(const AgGoal *goal, RxWorld *w, const AgCapTable *caps,
                      const AgConstraints *cons, const AgLibrary *procedures,
                      const AgFusionLibrary *lib, const AgSkillTable *skills, AgGraph *out,
                      AgReport *rep, AgFusionReport *frep) {
    int rc = rx_graph_compile(goal, w, caps, cons, procedures, 1, out, rep);
    if (rc != AG_OK_READY) return rc;
    rc = rx_fusion_apply(out, w, lib, skills, frep);
    if (rc != 0) return rep->verdict = rc;
    return rep->verdict = rx_graph_report_needs(out, w, caps, cons, rep);
}
