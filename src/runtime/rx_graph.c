/*
 * rx_graph.c -- Omega action graph IR: build, check, compile, optimize,
 * lower onto resident reactions, and the sequential reference.
 *
 * One function gives a node its meaning (semantics()). The lowered reaction
 * and the reference both call it, so the two executions differ only in who
 * decides when a node runs: the world's readiness, or a fixed topological
 * order.
 */
#include "rx_graph.h"

#include "omega_core.h"
#include "sha256.h"

#include <string.h>
#include <time.h>

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

static uint64_t low64(const uint8_t d[32]) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | d[i];
    return v;
}

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Real work for cost_us: the node holds its worker for that long. */
static void busy_us(uint32_t us) {
    if (!us) return;
    uint64_t end = mono_ns() + (uint64_t)us * 1000ull;
    volatile uint64_t x = 0;
    while (mono_ns() < end) x++;
}

const char *rx_graph_kind_name(AgKind k) {
    static const char *n[AG_KIND_COUNT] = {
        "?", "ag.const", "ag.pure", "ag.world_read", "ag.world_publish", "ag.recall",
        "ag.cap_resolve", "ag.physical", "ag.skill", "ag.effect_propose", "ag.effect_perform",
        "ag.verify", "ag.branch", "ag.join", "ag.retry"
    };
    return (unsigned)k < AG_KIND_COUNT ? n[k] : "?";
}

static int is_boundary(AgKind k) { return k == AG_WORLD_PUBLISH || k == AG_EFFECT_PERFORM; }
static int reads_world(AgKind k) {
    return k == AG_WORLD_READ || k == AG_RECALL || k == AG_EFFECT_PERFORM;
}
static int has_target(AgKind k) { return reads_world(k) || k == AG_WORLD_PUBLISH; }

uint64_t rx_graph_chain(uint64_t chain, uint64_t run, uint64_t value) {
    sha256_ctx c;
    uint8_t d[32];
    sha256_init(&c);
    put64(&c, chain);
    put64(&c, run);
    put64(&c, value);
    sha256_final(&c, d);
    return low64(d);
}

uint64_t rx_graph_evidence_word(const AgNode *n, uint64_t run, uint32_t status, uint64_t value,
                                const uint64_t *in, uint32_t n_in) {
    sha256_ctx c;
    uint8_t d[32];
    sha256_init(&c);
    sha256_update(&c, n->id, 32);
    put64(&c, run);
    put32(&c, status);
    put64(&c, value);
    put32(&c, n_in);
    for (uint32_t i = 0; i < n_in; i++) put64(&c, in[i]);
    sha256_final(&c, d);
    return low64(d) | 1u;
}

/* ---- building ---- */

void rx_graph_init(AgGraph *g, uint32_t subject) {
    memset(g, 0, sizeof *g);
    g->subject = subject;
}

int rx_graph_node(AgGraph *g, AgKind kind, AgType out) {
    if (g->n_nodes >= AG_MAX_NODES) return AG_E_FULL;
    uint32_t i = g->n_nodes++;
    AgNode *n = &g->nodes[i];
    memset(n, 0, sizeof *n);
    n->kind = kind;
    n->out_type = out;
    n->alive = 1;
    n->origin = i;
    return (int)i;
}

int rx_graph_data(AgGraph *g, uint32_t from, uint32_t to, uint32_t port, uint32_t mode) {
    if (from >= g->n_nodes || to >= g->n_nodes || port >= AG_MAX_IN || g->n_data >= AG_MAX_EDGES)
        return AG_E_ARG;
    g->data[g->n_data++] = (AgDataEdge){ (uint16_t)from, (uint16_t)to, (uint8_t)port,
                                         (uint8_t)mode, g->nodes[from].out_type };
    return 0;
}

static int add_dep(AgGraph *g, uint32_t from, uint32_t to, uint8_t kind, uint8_t pol) {
    if (from >= g->n_nodes || to >= g->n_nodes || g->n_deps >= AG_MAX_DEPS) return AG_E_ARG;
    g->deps[g->n_deps++] = (AgDep){ (uint16_t)from, (uint16_t)to, kind, pol };
    return 0;
}

int rx_graph_guard(AgGraph *g, uint32_t branch, uint32_t node, uint32_t polarity) {
    return add_dep(g, branch, node, AG_DEP_GUARD, polarity ? 1 : 0);
}

int rx_graph_order(AgGraph *g, uint32_t before, uint32_t after) {
    return add_dep(g, before, after, AG_DEP_ORDER, 0);
}

int rx_graph_need_authority(AgGraph *g, uint32_t node, uint64_t resource, uint32_t rights) {
    if (node >= g->n_nodes) return AG_E_ARG;
    for (uint32_t i = 0; i < g->n_auth; i++)
        if (g->auth[i].node == node && g->auth[i].resource == resource) {
            g->auth[i].rights |= rights;
            return 0;
        }
    if (g->n_auth >= AG_MAX_REQS) return AG_E_FULL;
    g->auth[g->n_auth++] = (AgAuthReq){ (uint16_t)node, resource, rights };
    return 0;
}

int rx_graph_need_resource(AgGraph *g, uint32_t node, const RxResourceNeed *need) {
    if (node >= g->n_nodes || g->n_res >= AG_MAX_REQS) return AG_E_ARG;
    g->res[g->n_res++] = (AgResReq){ (uint16_t)node, *need };
    return 0;
}

static int add_cond(AgCond *c, uint32_t *n, uint32_t node, uint32_t status) {
    if (*n >= AG_MAX_CONDS) return AG_E_FULL;
    c[(*n)++] = (AgCond){ (uint16_t)node, (uint8_t)status };
    return 0;
}

int rx_graph_success(AgGraph *g, uint32_t node, uint32_t status) {
    return node < g->n_nodes ? add_cond(g->success, &g->n_success, node, status) : AG_E_ARG;
}

int rx_graph_failure(AgGraph *g, uint32_t node, uint32_t status) {
    return node < g->n_nodes ? add_cond(g->failure, &g->n_failure, node, status) : AG_E_ARG;
}

int rx_graph_evidence(AgGraph *g, uint32_t node) {
    if (node >= g->n_nodes) return AG_E_ARG;
    for (uint32_t i = 0; i < g->n_evidence; i++)
        if (g->evidence[i] == node) return 0;
    g->evidence[g->n_evidence++] = (uint16_t)node;
    return 0;
}

static int is_evidence(const AgGraph *g, uint32_t n) {
    for (uint32_t i = 0; i < g->n_evidence; i++)
        if (g->evidence[i] == n) return 1;
    return 0;
}

static int in_conds(const AgGraph *g, uint32_t n) {
    for (uint32_t i = 0; i < g->n_success; i++)
        if (g->success[i].node == n) return 1;
    for (uint32_t i = 0; i < g->n_failure; i++)
        if (g->failure[i].node == n) return 1;
    return 0;
}

/* Nodes that no pass may merge, fuse into another, or remove. */
static int pinned(const AgGraph *g, uint32_t n) {
    return is_boundary(g->nodes[n].kind) || is_evidence(g, n) || in_conds(g, n);
}

/* ---- structure ---- */

/* Deterministic topological order over live nodes (smallest index first).
 * Returns the count, or AG_E_CYCLE. */
static int topo(const AgGraph *g, uint16_t order[AG_MAX_NODES]) {
    uint32_t indeg[AG_MAX_NODES] = { 0 };
    uint32_t live = 0;
    for (uint32_t i = 0; i < g->n_nodes; i++) live += g->nodes[i].alive;
    for (uint32_t e = 0; e < g->n_data; e++) indeg[g->data[e].to]++;
    for (uint32_t e = 0; e < g->n_deps; e++) indeg[g->deps[e].to]++;
    uint8_t done[AG_MAX_NODES] = { 0 };
    uint32_t k = 0;
    while (k < live) {
        int pick = -1;
        for (uint32_t i = 0; i < g->n_nodes; i++)
            if (g->nodes[i].alive && !done[i] && indeg[i] == 0) { pick = (int)i; break; }
        if (pick < 0) return AG_E_CYCLE;
        done[pick] = 1;
        order[k++] = (uint16_t)pick;
        for (uint32_t e = 0; e < g->n_data; e++)
            if (g->data[e].from == pick) indeg[g->data[e].to]--;
        for (uint32_t e = 0; e < g->n_deps; e++)
            if (g->deps[e].from == pick) indeg[g->deps[e].to]--;
    }
    return (int)k;
}

uint64_t rx_graph_ancestors(const AgGraph *g, uint32_t node) {
    uint64_t anc = 0, frontier = 1ull << node;
    while (frontier) {
        uint64_t next = 0;
        for (uint32_t e = 0; e < g->n_data; e++)
            if (frontier & (1ull << g->data[e].to)) next |= 1ull << g->data[e].from;
        for (uint32_t e = 0; e < g->n_deps; e++)
            if (frontier & (1ull << g->deps[e].to)) next |= 1ull << g->deps[e].from;
        next &= ~anc;
        anc |= next;
        frontier = next;
    }
    return anc;
}

static uint32_t n_inputs(const AgGraph *g, uint32_t n, uint32_t *ports) {
    uint32_t k = 0;
    for (uint32_t e = 0; e < g->n_data; e++)
        if (g->data[e].to == n) {
            if (ports) ports[k] = e;
            k++;
        }
    return k;
}

/* Data edges into n sorted by port (edge indices). */
static uint32_t inputs_by_port(const AgGraph *g, uint32_t n, uint32_t out[AG_MAX_IN]) {
    uint32_t k = 0;
    for (uint32_t p = 0; p < AG_MAX_IN; p++)
        for (uint32_t e = 0; e < g->n_data; e++)
            if (g->data[e].to == n && g->data[e].port == p && k < AG_MAX_IN) out[k++] = e;
    return k;
}

static int dep_of(const AgGraph *g, uint32_t n, uint8_t kind) {
    for (uint32_t e = 0; e < g->n_deps; e++)
        if (g->deps[e].to == n && g->deps[e].kind == kind) return (int)e;
    return -1;
}

static int type_ok(const AgGraph *g, uint32_t n) {
    const AgNode *x = &g->nodes[n];
    uint32_t e[AG_MAX_IN];
    uint32_t k = inputs_by_port(g, n, e);
    if (k != n_inputs(g, n, NULL)) return 0;   /* duplicate or out-of-range ports */
    for (uint32_t i = 0; i < k; i++)
        if (g->data[e[i]].port != i) return 0;  /* ports are dense from 0 */
    int numeric_in = 1;
    for (uint32_t i = 0; i < k; i++) {
        const AgDataEdge *d = &g->data[e[i]];
        if (d->mode == AG_EDGE_ON_FAIL) continue;
        if (d->type != AG_T_U64 && d->type != AG_T_BOOL) numeric_in = 0;
    }
    int numeric_out = x->out_type == AG_T_U64 || x->out_type == AG_T_BOOL;
    switch (x->kind) {
    case AG_CONST: case AG_CAP_RESOLVE: case AG_WORLD_READ:
        return k == 0 && numeric_out;
    case AG_RECALL:
        return k <= 1 && numeric_in && x->out_type == AG_T_U64;
    case AG_PURE:
        return k >= 1 && k <= 2 && numeric_in && numeric_out;
    case AG_PHYSICAL: case AG_SKILL: case AG_RETRY:
        return k >= 1 && numeric_in && x->out_type == AG_T_U64;
    case AG_WORLD_PUBLISH:
        return k == 1 && numeric_in && x->out_type == AG_T_RECEIPT;
    case AG_EFFECT_PROPOSE:
        return k == 1 && numeric_in && x->out_type == AG_T_PROPOSAL;
    case AG_EFFECT_PERFORM:
        return k == 2 && g->data[e[0]].type == AG_T_PROPOSAL &&
               g->data[e[1]].type == AG_T_VERDICT && x->out_type == AG_T_RECEIPT &&
               g->data[e[0]].mode == AG_EDGE_DATA && g->data[e[1]].mode == AG_EDGE_DATA;
    case AG_VERIFY:
        return k == 1 && numeric_in && x->out_type == AG_T_VERDICT;
    case AG_BRANCH:
        return k == 1 && numeric_in && x->out_type == AG_T_BOOL;
    case AG_JOIN:
        if (k < 2) return 0;
        for (uint32_t i = 0; i < k; i++)
            if (g->data[e[i]].type != x->out_type || g->data[e[i]].mode != AG_EDGE_DATA) return 0;
        return 1;
    default:
        return 0;
    }
}

/* What must hold for node n to end OK: nodes that must be OK, nodes that
 * must have FAILED, branches that must be true / false. */
typedef struct { uint64_t ok, fail, gt, gf; } Req;

static void requirements(const AgGraph *g, const uint16_t *ord, uint32_t cnt, Req *r) {
    memset(r, 0, sizeof(Req) * AG_MAX_NODES);
    for (uint32_t t = 0; t < cnt; t++) {
        uint32_t n = ord[t];
        Req q = { 0, 0, 0, 0 };
        if (g->nodes[n].kind == AG_JOIN) {
            int first = 1;
            for (uint32_t e = 0; e < g->n_data; e++) {
                if (g->data[e].to != n) continue;
                uint32_t s = g->data[e].from;
                Req a = r[s];
                a.ok |= 1ull << s;
                if (first) { q = a; first = 0; }
                else { q.ok &= a.ok; q.fail &= a.fail; q.gt &= a.gt; q.gf &= a.gf; }
            }
        } else {
            for (uint32_t e = 0; e < g->n_data; e++) {
                if (g->data[e].to != n) continue;
                uint32_t s = g->data[e].from;
                if (g->data[e].mode == AG_EDGE_ON_FAIL) { q.fail |= 1ull << s; continue; }
                q.ok |= r[s].ok | (1ull << s);
                q.fail |= r[s].fail;
                q.gt |= r[s].gt;
                q.gf |= r[s].gf;
            }
        }
        for (uint32_t e = 0; e < g->n_deps; e++) {
            if (g->deps[e].to != n || g->deps[e].kind != AG_DEP_GUARD) continue;
            uint32_t b = g->deps[e].from;
            q.ok |= r[b].ok | (1ull << b);
            q.fail |= r[b].fail;
            q.gt |= r[b].gt;
            q.gf |= r[b].gf;
            if (g->deps[e].polarity) q.gt |= 1ull << b;
            else q.gf |= 1ull << b;
        }
        r[n] = q;
    }
}

static int exclusive(const Req *a, const Req *b) {
    return ((a->gt & b->gf) | (a->gf & b->gt) | (a->ok & b->fail) | (a->fail & b->ok)) != 0;
}

/* Reaction dependencies a lowered node uses (rx_world RX_MAX_DEPS). */
static uint32_t dep_budget(const AgGraph *g, uint32_t n) {
    uint64_t srcs = 0;
    for (uint32_t e = 0; e < g->n_data; e++)
        if (g->data[e].to == n) srcs |= 1ull << g->data[e].from;
    for (uint32_t e = 0; e < g->n_deps; e++)
        if (g->deps[e].to == n) srcs |= 1ull << g->deps[e].from;
    uint32_t k = (uint32_t)__builtin_popcountll(srcs);
    k += 2;                                         /* run token + own cell */
    if (reads_world(g->nodes[n].kind)) k++;         /* the World object */
    if (has_target(g->nodes[n].kind)) k++;          /* room for an authority slot wake */
    return k;
}

static void derive_requirements(AgGraph *g, const RxWorld *w) {
    for (uint32_t n = 0; n < g->n_nodes; n++) {
        AgNode *x = &g->nodes[n];
        if (!x->alive) continue;
        if (x->kind == AG_VERIFY || is_boundary(x->kind)) rx_graph_evidence(g, n);
        if (!w || !has_target(x->kind) || x->obj.id >= RX_MAX_OBJECTS) continue;
        uint64_t res = w->objects[x->obj.id].resource;
        uint32_t rights = RX_RIGHT_READ;
        if (x->kind == AG_WORLD_PUBLISH) rights = RX_RIGHT_WRITE;
        if (x->kind == AG_EFFECT_PERFORM) rights = RX_RIGHT_READ | RX_RIGHT_WRITE | RX_RIGHT_EFFECT;
        rx_graph_need_authority(g, n, res, rights);
    }
}

int rx_graph_validate(AgGraph *g, const RxWorld *w) {
    uint16_t ord[AG_MAX_NODES];
    int cnt = topo(g, ord);
    if (cnt < 0) return cnt;
    for (uint32_t e = 0; e < g->n_data; e++) {
        if (!g->nodes[g->data[e].from].alive || !g->nodes[g->data[e].to].alive) return AG_E_ARG;
        g->data[e].type = g->nodes[g->data[e].from].out_type;
    }
    for (uint32_t n = 0; n < g->n_nodes; n++) {
        if (!g->nodes[n].alive) continue;
        if (!type_ok(g, n)) return AG_E_TYPE;
        uint32_t guards = 0, orders = 0;
        for (uint32_t e = 0; e < g->n_deps; e++) {
            if (g->deps[e].to != n) continue;
            if (g->deps[e].kind == AG_DEP_GUARD) {
                guards++;
                if (g->nodes[g->deps[e].from].out_type != AG_T_BOOL) return AG_E_TYPE;
            } else {
                orders++;
            }
        }
        (void)orders;
        if (guards > 1) return AG_E_DEP_BUDGET;
        if (dep_budget(g, n) > RX_MAX_DEPS) return AG_E_DEP_BUDGET;
    }
    derive_requirements(g, w);

    /* Effect boundaries: one total order, recorded in that order. */
    g->n_effects = 0;
    for (int t = 0; t < cnt; t++)
        if (is_boundary(g->nodes[ord[t]].kind)) g->effects[g->n_effects++] = ord[t];
    for (uint32_t i = 1; i < g->n_effects; i++)
        if (!(rx_graph_ancestors(g, g->effects[i]) & (1ull << g->effects[i - 1])))
            return AG_E_EFFECT_ORDER;
    /* A node that reads an object a boundary writes must be ordered against
     * that boundary; otherwise what it sees depends on worker timing. */
    for (uint32_t i = 0; i < g->n_effects; i++) {
        uint32_t b = g->effects[i];
        for (uint32_t n = 0; n < g->n_nodes; n++) {
            if (n == b || !g->nodes[n].alive || !has_target(g->nodes[n].kind) ||
                g->nodes[n].obj.id != g->nodes[b].obj.id)
                continue;
            if (!(rx_graph_ancestors(g, n) & (1ull << b)) && !(rx_graph_ancestors(g, b) & (1ull << n)))
                return AG_E_EFFECT_ORDER;
        }
    }

    /* Joins: every pair of arms mutually exclusive. */
    Req r[AG_MAX_NODES];
    requirements(g, ord, (uint32_t)cnt, r);
    for (uint32_t n = 0; n < g->n_nodes; n++) {
        if (!g->nodes[n].alive || g->nodes[n].kind != AG_JOIN) continue;
        uint32_t e[AG_MAX_IN];
        uint32_t k = inputs_by_port(g, n, e);
        for (uint32_t i = 0; i < k; i++)
            for (uint32_t j = i + 1; j < k; j++) {
                Req a = r[g->data[e[i]].from], b = r[g->data[e[j]].from];
                a.ok |= 1ull << g->data[e[i]].from;
                b.ok |= 1ull << g->data[e[j]].from;
                if (!exclusive(&a, &b)) return AG_E_JOIN;
            }
    }
    rx_graph_identify(g);
    return 0;
}

/* ---- identity ---- */

void rx_graph_identify(AgGraph *g) {
    uint16_t ord[AG_MAX_NODES];
    int cnt = topo(g, ord);
    if (cnt < 0) return;
    for (int t = 0; t < cnt; t++) {
        uint32_t n = ord[t];
        AgNode *x = &g->nodes[n];
        sha256_ctx c;
        sha256_init(&c);
        sha256_update(&c, (const uint8_t *)"OMEGA_ACTION_NODE_V1", 20);
        put32(&c, x->kind);
        put32(&c, x->out_type);
        put32(&c, x->op);
        put64(&c, x->imm);
        put64(&c, x->imm2);
        put32(&c, x->param);
        put32(&c, has_target(x->kind) ? x->obj.id : 0);
        put32(&c, has_target(x->kind) ? x->obj.generation : 0);
        put32(&c, x->field);
        put32(&c, x->cost_us);
        put32(&c, x->n_fused);
        for (uint32_t i = 0; i < x->n_fused; i++) {
            put32(&c, x->fused[i].op);
            put64(&c, x->fused[i].imm);
        }
        uint32_t e[AG_MAX_IN];
        uint32_t k = inputs_by_port(g, n, e);
        put32(&c, k);
        for (uint32_t i = 0; i < k; i++) {
            put8(&c, g->data[e[i]].mode);
            sha256_update(&c, g->nodes[g->data[e[i]].from].id, 32);
        }
        /* Control dependencies, sorted by (kind, polarity, source identity). */
        uint32_t d[AG_MAX_DEPS], nd = 0;
        for (uint32_t j = 0; j < g->n_deps; j++)
            if (g->deps[j].to == n) d[nd++] = j;
        for (uint32_t a = 1; a < nd; a++)
            for (uint32_t b = a; b > 0; b--) {
                const AgDep *p = &g->deps[d[b - 1]], *q = &g->deps[d[b]];
                int cmp = p->kind != q->kind ? p->kind - q->kind
                        : p->polarity != q->polarity ? p->polarity - q->polarity
                        : memcmp(g->nodes[p->from].id, g->nodes[q->from].id, 32);
                if (cmp <= 0) break;
                uint32_t tmp = d[b]; d[b] = d[b - 1]; d[b - 1] = tmp;
            }
        put32(&c, nd);
        for (uint32_t j = 0; j < nd; j++) {
            put8(&c, g->deps[d[j]].kind);
            put8(&c, g->deps[d[j]].polarity);
            sha256_update(&c, g->nodes[g->deps[d[j]].from].id, 32);
        }
        /* A World read is the same read only between the same publications
         * of that object: fold in every ancestor publication to it. */
        if (x->kind == AG_WORLD_READ || x->kind == AG_RECALL) {
            uint64_t anc = rx_graph_ancestors(g, n);
            for (int u = 0; u < cnt; u++) {
                uint32_t p = ord[u];
                if ((anc & (1ull << p)) && g->nodes[p].kind == AG_WORLD_PUBLISH &&
                    g->nodes[p].obj.id == x->obj.id)
                    sha256_update(&c, g->nodes[p].id, 32);
            }
        }
        /* A boundary is also its place in the effect chain. */
        if (is_boundary(x->kind)) {
            uint32_t pos = 0;
            uint64_t anc = rx_graph_ancestors(g, n);
            for (uint32_t p = 0; p < g->n_nodes; p++)
                if ((anc & (1ull << p)) && g->nodes[p].alive && is_boundary(g->nodes[p].kind)) pos++;
            put32(&c, pos);
        }
        sha256_final(&c, x->id);
    }
    /* Graph digest: sorted node identities, then conditions and evidence by
     * identity, then the principal. Independent of builder order. */
    uint8_t ids[AG_MAX_NODES][32];
    uint32_t m = 0;
    for (uint32_t n = 0; n < g->n_nodes; n++)
        if (g->nodes[n].alive) memcpy(ids[m++], g->nodes[n].id, 32);
    for (uint32_t a = 1; a < m; a++)
        for (uint32_t b = a; b > 0 && memcmp(ids[b - 1], ids[b], 32) > 0; b--) {
            uint8_t t[32];
            memcpy(t, ids[b], 32); memcpy(ids[b], ids[b - 1], 32); memcpy(ids[b - 1], t, 32);
        }
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)"OMEGA_ACTION_GRAPH_V1", 21);
    put32(&c, g->subject);
    put32(&c, m);
    for (uint32_t i = 0; i < m; i++) sha256_update(&c, ids[i], 32);
    /* Conditions and evidence as sorted (identity, tag) records. */
    uint8_t rec[3 * AG_MAX_CONDS + AG_MAX_NODES][33];
    uint32_t nr = 0;
    for (uint32_t i = 0; i < g->n_success; i++) {
        memcpy(rec[nr], g->nodes[g->success[i].node].id, 32);
        rec[nr++][32] = (uint8_t)(0x10 | g->success[i].status);
    }
    for (uint32_t i = 0; i < g->n_failure; i++) {
        memcpy(rec[nr], g->nodes[g->failure[i].node].id, 32);
        rec[nr++][32] = (uint8_t)(0x20 | g->failure[i].status);
    }
    for (uint32_t i = 0; i < g->n_evidence; i++) {
        memcpy(rec[nr], g->nodes[g->evidence[i]].id, 32);
        rec[nr++][32] = 0x30;
    }
    for (uint32_t a = 1; a < nr; a++)
        for (uint32_t b = a; b > 0 && memcmp(rec[b - 1], rec[b], 33) > 0; b--) {
            uint8_t t[33];
            memcpy(t, rec[b], 33); memcpy(rec[b], rec[b - 1], 33); memcpy(rec[b - 1], t, 33);
        }
    put32(&c, nr);
    for (uint32_t i = 0; i < nr; i++) sha256_update(&c, rec[i], 33);
    sha256_final(&c, g->digest);
}

/* ---- semantics (shared by the lowered reaction and the reference) ---- */

typedef struct {
    uint32_t n_in;
    uint64_t v[AG_MAX_IN];
    uint8_t s[AG_MAX_IN];
    uint8_t mode[AG_MAX_IN];
    int has_guard;
    uint8_t guard_s, guard_pol;
    uint64_t guard_v;
    int has_order;
    uint8_t order_s;
    uint64_t world[RX_MAX_FIELDS];
} AgIn;

typedef struct {
    uint8_t status;
    uint64_t value;
    uint32_t attempts;
    int write;          /* a boundary that performs its write */
} AgOut;

static const AgSkill *find_skill(const AgSkillTable *t, uint32_t id) {
    for (uint32_t i = 0; t && i < t->n; i++)
        if (t->skill[i].id == id) return &t->skill[i];
    return NULL;
}

static int pure_op(uint32_t op, uint64_t a, uint64_t b, uint64_t *out) {
    if (op == OP_IDENTITY) { *out = a; return 0; }
    if (op == OP_CONSTANT) { *out = b; return 0; }
    return omega_eval_pure_binary_uint((OpCode)op, OVERFLOW_WRAP, 64, a, b, out);
}

static void semantics(const AgNode *x, const AgSkillTable *sk, const AgIn *in, AgOut *o) {
    memset(o, 0, sizeof *o);
    if (in->has_guard) {
        if (in->guard_s == AG_FAILED) { o->status = AG_FAILED; return; }
        if (in->guard_s == AG_SKIPPED || (in->guard_v != 0) != (in->guard_pol != 0)) {
            o->status = AG_SKIPPED;
            return;
        }
    }
    if (in->has_order && in->order_s == AG_FAILED) { o->status = AG_FAILED; return; }

    if (x->kind == AG_JOIN) {
        int ok = -1, oks = 0, failed = 0;
        for (uint32_t i = 0; i < in->n_in; i++) {
            if (in->s[i] == AG_OK) { ok = (int)i; oks++; }
            if (in->s[i] == AG_FAILED) failed = 1;
        }
        if (oks == 1) { o->status = AG_OK; o->value = in->v[ok]; }
        else if (oks > 1) o->status = AG_FAILED;
        else o->status = failed ? AG_FAILED : AG_SKIPPED;
        return;
    }
    int skipped = 0, failed = 0;
    for (uint32_t i = 0; i < in->n_in; i++) {
        if (in->mode[i] == AG_EDGE_ON_FAIL) {
            if (in->s[i] != AG_FAILED) skipped = 1;
            continue;
        }
        if (in->s[i] == AG_SKIPPED) skipped = 1;
        if (in->s[i] == AG_FAILED) failed = 1;
    }
    if (skipped) { o->status = AG_SKIPPED; return; }
    if (failed) { o->status = AG_FAILED; return; }

    uint64_t a = in->n_in > 0 && in->mode[0] == AG_EDGE_DATA ? in->v[0] : 0;
    uint64_t v = 0;
    o->status = AG_OK;
    switch (x->kind) {
    case AG_CONST: case AG_CAP_RESOLVE:
        v = x->imm;
        break;
    case AG_PURE: {
        uint64_t b = in->n_in > 1 ? in->v[1] : x->imm;
        if (pure_op(x->op, a, b, &v) != 0) { o->status = AG_FAILED; return; }
        for (uint32_t i = 0; i < x->n_fused; i++)
            if (pure_op(x->fused[i].op, v, x->fused[i].imm, &v) != 0) {
                o->status = AG_FAILED;
                return;
            }
        break;
    }
    case AG_WORLD_READ:
        v = in->world[x->field & (RX_MAX_FIELDS - 1)];
        break;
    case AG_RECALL: {
        uint64_t key = in->n_in ? a : x->imm;
        o->status = AG_FAILED;
        for (uint32_t i = 0; i + 1 < RX_MAX_FIELDS; i += 2)
            if (in->world[i] == key && key != 0) { v = in->world[i + 1]; o->status = AG_OK; break; }
        break;
    }
    case AG_PHYSICAL: {
        /* Deterministic mixing of the inputs: stands for work the body does. */
        uint64_t h = 0x9E3779B97F4A7C15ull ^ x->imm;
        for (uint32_t i = 0; i < in->n_in; i++) {
            h ^= in->v[i] + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
            h *= 0xBF58476D1CE4E5B9ull;
        }
        v = h ^ (h >> 31);
        break;
    }
    case AG_SKILL: case AG_RETRY: {
        const AgSkill *s = find_skill(sk, x->op);
        if (!s) { o->status = AG_FAILED; return; }
        uint32_t max = x->kind == AG_RETRY ? (uint32_t)(x->imm ? x->imm : 1) : 1;
        int fail = 1;
        for (uint32_t t = 0; t < max && fail; t++) {
            fail = 0;
            o->attempts = t + 1;
            v = s->fn(in->v, in->n_in, t, &fail);
            if (x->kind == AG_RETRY && t < x->imm2) fail = 1;   /* the stand-in's leading failures */
        }
        if (fail) { o->status = AG_FAILED; v = 0; }
        break;
    }
    case AG_EFFECT_PROPOSE:
        v = a;
        break;
    case AG_VERIFY:
        if (a < x->imm || a > x->imm2) { o->status = AG_FAILED; return; }
        v = 1;
        break;
    case AG_BRANCH:
        v = a != 0;
        break;
    case AG_WORLD_PUBLISH:
        v = a;
        o->write = 1;
        break;
    case AG_EFFECT_PERFORM:
        if (in->v[1] != 1) { o->status = AG_FAILED; return; }
        v = in->world[0] + 1;       /* receipt: the effect's sequence number */
        o->write = 1;
        break;
    default:
        o->status = AG_FAILED;
        return;
    }
    o->value = v;
}

/* ---- optimization ---- */

static void redirect(AgGraph *g, uint32_t from, uint32_t to) {
    for (uint32_t e = 0; e < g->n_data; e++)
        if (g->data[e].from == from) g->data[e].from = (uint16_t)to;
    for (uint32_t e = 0; e < g->n_deps; e++)
        if (g->deps[e].from == from) g->deps[e].from = (uint16_t)to;
}

static void drop_edges_to(AgGraph *g, uint32_t n) {
    uint32_t k = 0;
    for (uint32_t e = 0; e < g->n_data; e++)
        if (g->data[e].to != n) g->data[k++] = g->data[e];
    g->n_data = k;
}

static void drop_edges_of(AgGraph *g, uint32_t n) {
    uint32_t k = 0;
    for (uint32_t e = 0; e < g->n_data; e++)
        if (g->data[e].to != n && g->data[e].from != n) g->data[k++] = g->data[e];
    g->n_data = k;
    k = 0;
    for (uint32_t e = 0; e < g->n_deps; e++)
        if (g->deps[e].to != n && g->deps[e].from != n) g->deps[k++] = g->deps[e];
    g->n_deps = k;
}

static uint32_t consumers(const AgGraph *g, uint32_t n) {
    uint32_t k = 0;
    for (uint32_t e = 0; e < g->n_data; e++) k += g->data[e].from == n;
    for (uint32_t e = 0; e < g->n_deps; e++) k += g->deps[e].from == n;
    return k;
}

/* Remove dead nodes and renumber everything that names a node. */
static void compact(AgGraph *g) {
    int16_t map[AG_MAX_NODES];
    uint32_t k = 0;
    for (uint32_t n = 0; n < g->n_nodes; n++) {
        map[n] = g->nodes[n].alive ? (int16_t)k : -1;
        if (g->nodes[n].alive) g->nodes[k++] = g->nodes[n];
    }
    g->n_nodes = k;
    uint32_t j = 0;
    for (uint32_t e = 0; e < g->n_data; e++)
        if (map[g->data[e].from] >= 0 && map[g->data[e].to] >= 0) {
            g->data[j] = g->data[e];
            g->data[j].from = (uint16_t)map[g->data[e].from];
            g->data[j].to = (uint16_t)map[g->data[e].to];
            j++;
        }
    g->n_data = j;
    j = 0;
    for (uint32_t e = 0; e < g->n_deps; e++)
        if (map[g->deps[e].from] >= 0 && map[g->deps[e].to] >= 0) {
            g->deps[j] = g->deps[e];
            g->deps[j].from = (uint16_t)map[g->deps[e].from];
            g->deps[j].to = (uint16_t)map[g->deps[e].to];
            j++;
        }
    g->n_deps = j;
#define REMAP(arr, cnt, field) do {                                  \
        uint32_t q = 0;                                              \
        for (uint32_t i = 0; i < (cnt); i++)                         \
            if (map[(arr)[i] field] >= 0) {                          \
                (arr)[q] = (arr)[i];                                 \
                (arr)[q] field = (uint16_t)map[(arr)[i] field];      \
                q++;                                                 \
            }                                                        \
        (cnt) = q;                                                   \
    } while (0)
    REMAP(g->auth, g->n_auth, .node);
    REMAP(g->res, g->n_res, .node);
    REMAP(g->success, g->n_success, .node);
    REMAP(g->failure, g->n_failure, .node);
    REMAP(g->evidence, g->n_evidence, );
    REMAP(g->effects, g->n_effects, );
#undef REMAP
}

/* Could node n end SKIPPED / FAILED in some run? Conservative. */
static void may_flags(const AgGraph *g, const uint16_t *ord, uint32_t cnt,
                      uint8_t *may_skip, uint8_t *may_fail) {
    for (uint32_t t = 0; t < cnt; t++) {
        uint32_t n = ord[t];
        AgKind k = g->nodes[n].kind;
        uint8_t sk = 0, fl = 0;
        fl = k == AG_VERIFY || k == AG_RECALL || k == AG_RETRY || k == AG_SKILL ||
             k == AG_JOIN || is_boundary(k) || k == AG_PURE;   /* PURE: division by zero */
        for (uint32_t e = 0; e < g->n_data; e++) {
            if (g->data[e].to != n) continue;
            if (g->data[e].mode == AG_EDGE_ON_FAIL) sk = 1;
            sk |= may_skip[g->data[e].from];
            fl |= may_fail[g->data[e].from];
        }
        for (uint32_t e = 0; e < g->n_deps; e++) {
            if (g->deps[e].to != n) continue;
            if (g->deps[e].kind == AG_DEP_GUARD) sk = 1;
            sk |= may_skip[g->deps[e].from];
            fl |= may_fail[g->deps[e].from];
        }
        may_skip[n] = sk;
        may_fail[n] = fl;
    }
}

/* Is `a` an ancestor of `b` along strict paths only: DATA edges into
 * non-JOIN nodes, and order edges, through nodes that can never be SKIPPED?
 * Along such a path OK stays OK-or-FAILED and FAILED arrives as FAILED, so
 * an order edge a -> b says nothing the path does not. A node that can be
 * skipped would turn an arriving FAILED into SKIPPED (guards and skipped
 * inputs are looked at first), so the path stops there. */
static int strict_ancestor(const AgGraph *g, uint32_t a, uint32_t b, int skip_dep,
                           const uint8_t *may_skip) {
    uint64_t seen = 1ull << a, frontier = 1ull << a;
    while (frontier) {
        uint64_t next = 0;
        for (uint32_t e = 0; e < g->n_data; e++)
            if ((frontier & (1ull << g->data[e].from)) && g->data[e].mode == AG_EDGE_DATA &&
                g->nodes[g->data[e].to].kind != AG_JOIN && !may_skip[g->data[e].to])
                next |= 1ull << g->data[e].to;
        for (uint32_t e = 0; e < g->n_deps; e++)
            if ((int)e != skip_dep && (frontier & (1ull << g->deps[e].from)) &&
                g->deps[e].kind == AG_DEP_ORDER && !may_skip[g->deps[e].to])
                next |= 1ull << g->deps[e].to;
        next &= ~seen;
        seen |= next;
        frontier = next;
    }
    return (seen >> b) & 1u;
}

int rx_graph_optimize(AgGraph *g, AgReport *rep) {
    AgReport dummy;
    if (!rep) { memset(&dummy, 0, sizeof dummy); rep = &dummy; }
    uint16_t ord[AG_MAX_NODES];
    int cnt;

    /* Passes 1-4 can open work for each other (dropping an order edge can
     * make two nodes identical; merging can make a producer single-use), so
     * they repeat until a round changes nothing. Each round only removes
     * nodes or edges, so this ends. */
    for (;;) {
        uint32_t before_round = rep->passes.const_folded + rep->passes.cse + rep->passes.world_reads +
                                rep->passes.fused + rep->passes.deps_dropped;
        /* 1. Constant propagation: a PURE node whose inputs are all CONST, with
         * no control dependency, becomes a CONST. Guarded nodes keep their guard. */
        for (int changed = 1; changed;) {
            changed = 0;
            if ((cnt = topo(g, ord)) < 0) return cnt;
            for (int t = 0; t < cnt; t++) {
                uint32_t n = ord[t];
                AgNode *x = &g->nodes[n];
                if (x->kind != AG_PURE || pinned(g, n)) continue;
                uint32_t e[AG_MAX_IN];
                uint32_t k = inputs_by_port(g, n, e);
                int all = k > 0;
                AgIn in;
                memset(&in, 0, sizeof in);
                in.n_in = k;
                for (uint32_t i = 0; i < k && all; i++) {
                    const AgNode *s = &g->nodes[g->data[e[i]].from];
                    if (s->kind != AG_CONST || g->data[e[i]].mode != AG_EDGE_DATA ||
                        dep_of(g, g->data[e[i]].from, AG_DEP_GUARD) >= 0)
                        all = 0;
                    in.v[i] = s->imm;
                    in.s[i] = AG_OK;
                }
                if (!all) continue;
                AgOut o;
                semantics(x, NULL, &in, &o);
                if (o.status != AG_OK) continue;          /* keep a failing fold as it is */
                x->kind = AG_CONST;
                x->imm = o.value;
                x->op = 0;
                x->n_fused = 0;
                drop_edges_to(g, n);
                rep->passes.const_folded++;
                changed = 1;
            }
        }

        /* 2. Common subexpressions and redundant World reads. */
        rx_graph_identify(g);
        if ((cnt = topo(g, ord)) < 0) return cnt;
        for (int t = 0; t < cnt; t++) {
            uint32_t n = ord[t];
            AgNode *x = &g->nodes[n];
            if (!x->alive || pinned(g, n) || x->kind == AG_BRANCH || x->kind == AG_JOIN) continue;
            for (int u = 0; u < t; u++) {
                uint32_t m = ord[u];
                if (!g->nodes[m].alive || pinned(g, m)) continue;
                if (memcmp(g->nodes[m].id, x->id, 32) != 0) continue;
                redirect(g, n, m);
                drop_edges_of(g, n);
                x->alive = 0;
                if (x->kind == AG_WORLD_READ || x->kind == AG_RECALL) rep->passes.world_reads++;
                else rep->passes.cse++;
                break;
            }
            rx_graph_identify(g);
        }

        /* 3. Pure fusion: a unary PURE step whose only producer is a PURE node
         * with no other consumer folds into that producer. */
        for (int changed = 1; changed;) {
            changed = 0;
            if ((cnt = topo(g, ord)) < 0) return cnt;
            for (int t = 0; t < cnt && !changed; t++) {
                uint32_t c = ord[t];
                AgNode *y = &g->nodes[c];
                if (y->kind != AG_PURE || pinned(g, c) || y->op == OP_IDENTITY ||
                    y->op == OP_CONSTANT)
                    continue;
                uint32_t e[AG_MAX_IN];
                if (inputs_by_port(g, c, e) != 1 || g->data[e[0]].mode != AG_EDGE_DATA) continue;
                int has_ctl = 0;
                for (uint32_t d = 0; d < g->n_deps; d++) has_ctl |= g->deps[d].to == c;
                if (has_ctl) continue;
                uint32_t p = g->data[e[0]].from;
                AgNode *x = &g->nodes[p];
                if (x->kind != AG_PURE || pinned(g, p) || consumers(g, p) != 1) continue;
                if (x->n_fused + 1 + y->n_fused > AG_MAX_FUSED) continue;
                x->fused[x->n_fused++] = (AgFusedStep){ y->op, y->imm };
                for (uint32_t i = 0; i < y->n_fused; i++) x->fused[x->n_fused++] = y->fused[i];
                x->out_type = y->out_type;
                x->cost_us += y->cost_us;
                drop_edges_to(g, c);
                redirect(g, c, p);
                y->alive = 0;
                rep->passes.fused++;
                changed = 1;
            }
        }

        /* 4. Dependency simplification. An order edge goes when the order it
         * carries is already implied by a strict path, or when it only delays an
         * effect-free node behind a source that can neither fail nor skip. An
         * order edge that touches an effect boundary stays unless implied. */
        if ((cnt = topo(g, ord)) < 0) return cnt;
        uint8_t may_skip[AG_MAX_NODES] = { 0 }, may_fail[AG_MAX_NODES] = { 0 };
        may_flags(g, ord, (uint32_t)cnt, may_skip, may_fail);
        for (uint32_t e = 0; e < g->n_deps;) {
            const AgDep *d = &g->deps[e];
            int drop = 0;
            if (d->kind == AG_DEP_ORDER) {
                int touches = is_boundary(g->nodes[d->from].kind) || is_boundary(g->nodes[d->to].kind);
                if (!may_skip[d->from] && strict_ancestor(g, d->from, d->to, (int)e, may_skip)) drop = 1;
                else if (!touches && !may_skip[d->from] && !may_fail[d->from]) drop = 1;
            }
            if (drop) {
                g->deps[e] = g->deps[--g->n_deps];
                rep->passes.deps_dropped++;
            } else {
                e++;
            }
        }

        if (rep->passes.const_folded + rep->passes.cse + rep->passes.world_reads +
            rep->passes.fused + rep->passes.deps_dropped == before_round)
            break;
    }

    /* 5. Dead nodes: keep what reaches a boundary, evidence or condition.
     * Runs after 4 so a node held only by a dropped order edge goes too. */
    uint64_t keep = 0;
    for (uint32_t n = 0; n < g->n_nodes; n++)
        if (g->nodes[n].alive && pinned(g, n)) keep |= (1ull << n) | rx_graph_ancestors(g, n);
    for (uint32_t n = 0; n < g->n_nodes; n++)
        if (g->nodes[n].alive && !(keep & (1ull << n))) {
            drop_edges_of(g, n);
            g->nodes[n].alive = 0;
            rep->passes.dead++;
        }

    compact(g);
    return 0;
}

int rx_graph_check_preserved(const AgGraph *before, const AgGraph *after) {
    int16_t where[AG_MAX_NODES];
    for (uint32_t i = 0; i < AG_MAX_NODES; i++) where[i] = -1;
    for (uint32_t n = 0; n < after->n_nodes; n++) where[after->nodes[n].origin] = (int16_t)n;
    if (before->n_effects != after->n_effects) return -1;
    for (uint32_t i = 0; i < before->n_effects; i++) {
        const AgNode *b = &before->nodes[before->effects[i]];
        const AgNode *a = &after->nodes[after->effects[i]];
        if (a->origin != b->origin || a->kind != b->kind || a->obj.id != b->obj.id ||
            a->obj.generation != b->obj.generation || a->field != b->field)
            return -2;
    }
    for (uint32_t i = 0; i < before->n_evidence; i++) {
        int16_t n = where[before->nodes[before->evidence[i]].origin];
        if (n < 0 || after->nodes[n].kind != before->nodes[before->evidence[i]].kind) return -3;
        int found = 0;
        for (uint32_t j = 0; j < after->n_evidence; j++) found |= after->evidence[j] == n;
        if (!found) return -3;
    }
    for (uint32_t i = 0; i < before->n_success; i++)
        if (where[before->nodes[before->success[i].node].origin] < 0) return -4;
    for (uint32_t i = 0; i < before->n_failure; i++)
        if (where[before->nodes[before->failure[i].node].origin] < 0) return -4;
    if (before->n_success != after->n_success || before->n_failure != after->n_failure) return -4;
    return 0;
}

void rx_graph_costs(const AgGraph *g, uint64_t *critical_us, uint64_t *total_us) {
    uint16_t ord[AG_MAX_NODES];
    int cnt = topo(g, ord);
    uint64_t fin[AG_MAX_NODES] = { 0 }, best = 0, total = 0;
    for (int t = 0; t < cnt; t++) {
        uint32_t n = ord[t];
        uint64_t start = 0;
        for (uint32_t e = 0; e < g->n_data; e++)
            if (g->data[e].to == n && fin[g->data[e].from] > start) start = fin[g->data[e].from];
        for (uint32_t e = 0; e < g->n_deps; e++)
            if (g->deps[e].to == n && fin[g->deps[e].from] > start) start = fin[g->deps[e].from];
        fin[n] = start + g->nodes[n].cost_us;
        total += g->nodes[n].cost_us;
        if (fin[n] > best) best = fin[n];
    }
    if (critical_us) *critical_us = best;
    if (total_us) *total_us = total;
}

/* ---- compile ---- */

static int cap_lookup(const RxWorld *w, const AgCapTable *caps, uint32_t subject,
                      uint64_t resource, uint32_t rights, RxCapRef *out) {
    for (uint32_t i = 0; caps && i < caps->n; i++) {
        if (caps->cap[i].resource != resource || (caps->cap[i].rights & rights) != rights) continue;
        if (rx_world_validate_cap(w, caps->cap[i].ref, subject, resource, rights, NULL) != RX_CAP_OK)
            continue;
        if (out) *out = caps->cap[i].ref;
        return 1;
    }
    return 0;
}

static int fits(const RxResourceNeed *n, const RxResourceBudget *b) {
    if (n->memory_bytes > b->memory_bytes) return 0;
    if (n->accelerator_features & ~b->offered_accel) return 0;
    if (n->locality & ~b->offered_locality) return 0;
    if (n->compute_class && !(b->compute_mask & (1u << (n->compute_class & 31u)))) return 0;
    if (n->energy_cost > b->energy_budget) return 0;
    return 1;
}

int rx_graph_compile(const AgGoal *goal, RxWorld *w, const AgCapTable *caps,
                     const AgConstraints *cons, const AgLibrary *lib, int optimize,
                     AgGraph *out, AgReport *rep) {
    if (!goal || !w || !caps || !cons || !lib || !out || !rep) return AG_E_ARG;
    memset(rep, 0, sizeof *rep);
    const AgGraph *tmpl = NULL;
    for (uint32_t i = 0; i < lib->n; i++)
        if (lib->proc[i].goal_kind == goal->kind) tmpl = lib->proc[i].tmpl;
    if (!tmpl) return rep->verdict = AG_E_NO_PROCEDURE;
    *out = *tmpl;
    out->subject = caps->subject;

    /* Bind World objects and check they are current. */
    for (uint32_t n = 0; n < out->n_nodes; n++) {
        AgNode *x = &out->nodes[n];
        if (!x->alive || !x->param) continue;
        if (x->param > goal->n_args) return rep->verdict = AG_E_ARG;
        x->obj = goal->args[x->param - 1];
    }
    for (uint32_t n = 0; n < out->n_nodes; n++) {
        AgNode *x = &out->nodes[n];
        if (!x->alive || !has_target(x->kind)) continue;
        RxObject o;
        if (rx_world_read(w, x->obj, &o) != RX_OK) rep->stale[rep->n_stale++] = (uint16_t)n;
    }
    if (rep->n_stale) return rep->verdict = AG_E_STALE;

    /* Capability resolution happens here, against what the principal holds. */
    for (uint32_t n = 0; n < out->n_nodes; n++) {
        AgNode *x = &out->nodes[n];
        if (!x->alive || x->kind != AG_CAP_RESOLVE) continue;
        x->imm = (uint64_t)cap_lookup(w, caps, caps->subject, x->imm, (uint32_t)x->imm2, NULL);
        x->imm2 = 0;
        x->kind = AG_CONST;
    }
    int rc = rx_graph_validate(out, w);
    if (rc != 0) return rep->verdict = rc;

    /* Constraints. */
    if (cons->max_effects != UINT32_MAX) {
        uint32_t performs = 0;
        for (uint32_t i = 0; i < out->n_effects; i++)
            performs += out->nodes[out->effects[i]].kind == AG_EFFECT_PERFORM;
        if (performs > cons->max_effects) return rep->verdict = AG_E_CONSTRAINT;
    }
    if (cons->forbid_hi)
        for (uint32_t i = 0; i < out->n_auth; i++)
            if (out->auth[i].resource >= cons->forbid_lo && out->auth[i].resource <= cons->forbid_hi)
                return rep->verdict = AG_E_CONSTRAINT;

    rep->nodes_before = 0;
    for (uint32_t n = 0; n < out->n_nodes; n++) rep->nodes_before += out->nodes[n].alive;
    AgGraph before = *out;
    if (optimize) {
        if ((rc = rx_graph_optimize(out, rep)) != 0) return rep->verdict = rc;
        if ((rc = rx_graph_validate(out, w)) != 0) return rep->verdict = rc;
        if (rx_graph_check_preserved(&before, out) != 0) return rep->verdict = AG_E_CONSTRAINT;
    }
    rep->nodes_after = out->n_nodes;
    rx_graph_costs(out, &rep->critical_path_us, &rep->total_work_us);
    if (cons->critical_path_us && rep->critical_path_us > cons->critical_path_us)
        return rep->verdict = AG_E_CONSTRAINT;

    /* Authority: found, or reported missing. Never created here. */
    for (uint32_t i = 0; i < out->n_auth; i++)
        if (!cap_lookup(w, caps, caps->subject, out->auth[i].resource, out->auth[i].rights, NULL))
            rep->missing[rep->n_missing++] = out->auth[i];
    /* Resources: what the body offers now. */
    for (uint32_t i = 0; i < out->n_res; i++)
        if (!fits(&out->res[i].need, &cons->budget))
            rep->resource_blocked[rep->n_resource_blocked++] = out->res[i].node;
    return rep->verdict = AG_OK_READY;
}

/* ---- lowering ---- */

#define CELL_MASK (RX_FIELD(0) | RX_FIELD(1) | RX_FIELD(2) | RX_FIELD(3) | RX_FIELD(4))

static const RxSnapshotDep *snap(const RxCtx *c, RxObjRef r) {
    for (uint32_t i = 0; i < c->n_in; i++)
        if (c->in[i].obj.id == r.id && c->in[i].obj.generation == r.generation) return &c->in[i];
    return NULL;
}

static int node_fn(RxCtx *c) {
    const struct AgNodeCtx *nc = c->user;
    const AgLowered *L = nc->L;
    const AgGraph *g = L->g;
    uint32_t n = nc->node;
    const AgNode *x = &g->nodes[n];
    const RxSnapshotDep *run = snap(c, L->run), *own = snap(c, L->cell[n]);
    if (!run || !own) return -1;
    uint64_t r = run->field[0];
    if (r == 0 || own->field[0] == r) return 0;     /* no run, or done for this run */

    AgIn in;
    memset(&in, 0, sizeof in);
    uint32_t e[AG_MAX_IN];
    in.n_in = inputs_by_port(g, n, e);
    for (uint32_t i = 0; i < in.n_in; i++) {
        const RxSnapshotDep *s = snap(c, L->cell[g->data[e[i]].from]);
        if (!s) return -1;
        if (s->field[0] != r) return 0;             /* an input is not in for this run */
        in.s[i] = (uint8_t)s->field[1];
        in.v[i] = s->field[2];
        in.mode[i] = g->data[e[i]].mode;
    }
    int gd = dep_of(g, n, AG_DEP_GUARD);
    if (gd >= 0) {
        const RxSnapshotDep *s = snap(c, L->cell[g->deps[gd].from]);
        if (!s) return -1;
        if (s->field[0] != r) return 0;
        in.has_guard = 1;
        in.guard_s = (uint8_t)s->field[1];
        in.guard_v = s->field[2];
        in.guard_pol = g->deps[gd].polarity;
    }
    for (uint32_t od = 0; od < g->n_deps; od++) {
        if (g->deps[od].to != n || g->deps[od].kind != AG_DEP_ORDER) continue;
        const RxSnapshotDep *s = snap(c, L->cell[g->deps[od].from]);
        if (!s) return -1;
        if (s->field[0] != r) return 0;
        in.has_order = 1;
        if (s->field[1] == AG_FAILED) in.order_s = AG_FAILED;
    }
    const RxSnapshotDep *wo = NULL;
    if (reads_world(x->kind)) {
        wo = snap(c, x->obj);
        if (!wo) return -1;
        memcpy(in.world, wo->field, sizeof in.world);
    }
    AgOut o;
    semantics(x, L->skills, &in, &o);
    if (o.status == AG_OK) busy_us(x->cost_us);
    uint64_t ev = rx_graph_evidence_word(x, r, o.status, o.value, in.v, in.n_in);
    c->out[c->n_out++] = (RxMutation){ L->cell[n], 0, r };
    c->out[c->n_out++] = (RxMutation){ L->cell[n], 1, o.status };
    c->out[c->n_out++] = (RxMutation){ L->cell[n], 2, o.value };
    c->out[c->n_out++] = (RxMutation){ L->cell[n], 3, ev };
    c->out[c->n_out++] = (RxMutation){ L->cell[n], 4, o.attempts };
    if (o.status == AG_OK && o.write) {
        if (x->kind == AG_WORLD_PUBLISH) {
            c->out[c->n_out++] = (RxMutation){ x->obj, x->field, o.value };
        } else {
            c->out[c->n_out++] = (RxMutation){ x->obj, 0, wo->field[0] + 1 };
            c->out[c->n_out++] = (RxMutation){ x->obj, 1, in.v[0] };
            c->out[c->n_out++] = (RxMutation){ x->obj, 2, rx_graph_chain(wo->field[2], r, in.v[0]) };
            c->out[c->n_out++] = (RxMutation){ x->obj, 3, r };
        }
    }
    return 0;
}

/* The principal asks AEGIS for authority it lacks, once per run while the
 * slot is empty. request: 0 seq, 1 resource, 2 rights, 3 lease, 4 slot, 5 op. */
static int ask_fn(RxCtx *c) {
    const struct AgAskCtx *ac = c->user;
    const AgLowered *L = ac->L;
    uint32_t k = ac->k;
    const RxAegisClientObjects *cl = L->aegis_client;
    const RxSnapshotDep *run = snap(c, L->run), *rq = snap(c, cl->request),
                        *sl = snap(c, cl->slot[L->asked[k].slot]);
    if (!run || !rq || !sl) return -1;
    uint64_t r = run->field[0];
    if (r == 0 || sl->field[2] == RX_AEGIS_SLOT_LIVE || rq->field[0] >= r) return 0;
    const AgAuthReq *a = &L->ask_req[k];
    c->out[c->n_out++] = (RxMutation){ cl->request, 0, r };
    c->out[c->n_out++] = (RxMutation){ cl->request, 1, a->resource };
    c->out[c->n_out++] = (RxMutation){ cl->request, 2, a->rights };
    c->out[c->n_out++] = (RxMutation){ cl->request, 3, 0 };
    c->out[c->n_out++] = (RxMutation){ cl->request, 4, L->asked[k].slot };
    c->out[c->n_out++] = (RxMutation){ cl->request, 5, RX_AEGIS_OP_ACQUIRE };
    return 0;
}

static void add_trigger(RxReactionDesc *d, RxObjRef o, uint64_t mask) {
    for (uint32_t i = 0; i < d->n_triggers; i++)
        if (d->triggers[i].obj.id == o.id) { d->triggers[i].mask |= mask; return; }
    d->triggers[d->n_triggers++] = (RxDep){ o, mask };
}

static void add_read(RxReactionDesc *d, RxObjRef o, uint64_t mask) {
    for (uint32_t i = 0; i < d->n_triggers; i++)
        if (d->triggers[i].obj.id == o.id) return;
    for (uint32_t i = 0; i < d->n_reads; i++)
        if (d->reads[i].obj.id == o.id) { d->reads[i].mask |= mask; return; }
    d->reads[d->n_reads++] = (RxDep){ o, mask };
}

int rx_graph_lower(AgLowered *L, RxWorld *w, const AgGraph *g, const AgSkillTable *skills,
                   const AgCapTable *caps, uint64_t cell_res, RxCapRef cell_cap,
                   uint64_t run_res, RxCapRef run_cap, const AgAegisBinding *aegis) {
    memset(L, 0, sizeof *L);
    L->w = w;
    L->g = g;
    L->skills = skills;
    L->cell_res = cell_res;
    L->run_res = run_res;
    L->aegis_client = aegis ? aegis->client : NULL;
    uint64_t z[RX_MAX_FIELDS] = { 0 };
    if (rx_world_create(w, AG_OT_RUN, RX_PERSIST_RESIDENT, run_res, z, &L->run) != RX_OK)
        return AG_E_LOWER;
    for (uint32_t n = 0; n < g->n_nodes; n++)
        if (rx_world_create(w, AG_OT_CELL, RX_PERSIST_RESIDENT, cell_res, z, &L->cell[n]) != RX_OK)
            return AG_E_LOWER;
    uint16_t ord[AG_MAX_NODES];
    int cnt = topo(g, ord);
    if (cnt < 0) return cnt;
    for (int t = 0; t < cnt; t++) {
        uint32_t n = ord[t];
        const AgNode *x = &g->nodes[n];
        RxReactionDesc d;
        memset(&d, 0, sizeof d);
        d.name = rx_graph_kind_name(x->kind);
        d.faculty = RX_FACULTY_OMEGA;
        d.subject = g->subject;
        d.priority = RX_PRIO_FOREGROUND;
        d.stamp_proposed = true;
        d.fn = node_fn;
        L->nctx[n] = (struct AgNodeCtx){ L, n };
        d.user = &L->nctx[n];
        for (uint32_t i = 0; i < g->n_res; i++)
            if (g->res[i].node == n) d.need = g->res[i].need;
        for (uint32_t e = 0; e < g->n_data; e++)
            if (g->data[e].to == n) add_trigger(&d, L->cell[g->data[e].from], CELL_MASK);
        for (uint32_t e = 0; e < g->n_deps; e++)
            if (g->deps[e].to == n) add_trigger(&d, L->cell[g->deps[e].from], CELL_MASK);
        if (d.n_triggers == 0) add_trigger(&d, L->run, RX_FIELD(0));
        else add_read(&d, L->run, RX_FIELD(0));
        add_read(&d, L->cell[n], RX_FIELD(0));
        if (reads_world(x->kind)) add_read(&d, x->obj, RX_ALL_FIELDS);
        d.writes[d.n_writes++] = (RxDep){ L->cell[n], CELL_MASK };
        if (x->kind == AG_WORLD_PUBLISH) d.writes[d.n_writes++] = (RxDep){ x->obj, RX_FIELD(x->field) };
        if (x->kind == AG_EFFECT_PERFORM)
            d.writes[d.n_writes++] = (RxDep){ x->obj, RX_FIELD(0) | RX_FIELD(1) | RX_FIELD(2) | RX_FIELD(3) };
        d.caps[d.n_caps++] = (RxCapNeed){ cell_cap, cell_res, RX_RIGHT_READ | RX_RIGHT_WRITE };
        d.caps[d.n_caps++] = (RxCapNeed){ run_cap, run_res, RX_RIGHT_READ };
        for (uint32_t i = 0; i < g->n_auth; i++) {
            if (g->auth[i].node != n) continue;
            RxCapRef ref = { UINT32_MAX, 0 };   /* missing: the engine blocks and records it */
            int have = cap_lookup(w, caps, g->subject, g->auth[i].resource, g->auth[i].rights, &ref);
            uint32_t ci = d.n_caps;
            d.caps[d.n_caps++] = (RxCapNeed){ ref, g->auth[i].resource, g->auth[i].rights };
            if (!have && aegis && aegis->client) {
                if (L->n_ask >= RX_AEGIS_SLOTS || L->n_ask >= 1) return AG_E_LOWER;
                uint32_t k = L->n_ask++;
                L->asked[k].node = n;
                L->asked[k].slot = k;
                L->ask_req[k] = g->auth[i];
                d.caps[d.n_caps++] = (RxCapNeed){ aegis->slot_cap[k], aegis->slot_res[k], RX_RIGHT_READ };
                rx_aegis_use_slot(&d, ci, aegis->client->slot[k]);
            }
        }
        if (d.n_triggers + d.n_reads > RX_MAX_DEPS) return AG_E_DEP_BUDGET;
        if (rx_world_add_reaction(w, &d, &L->reaction[n]) != RX_OK) return AG_E_LOWER;
    }
    for (uint32_t k = 0; k < L->n_ask; k++) {
        RxReactionDesc d;
        memset(&d, 0, sizeof d);
        d.name = "ag.ask";
        d.faculty = RX_FACULTY_OMEGA;
        d.subject = g->subject;
        d.priority = RX_PRIO_FOREGROUND;
        d.stamp_proposed = true;
        d.fn = ask_fn;
        L->actx[k] = (struct AgAskCtx){ L, k };
        d.user = &L->actx[k];
        d.n_triggers = 1;
        d.triggers[0] = (RxDep){ L->run, RX_FIELD(0) };
        d.n_reads = 2;
        d.reads[0] = (RxDep){ aegis->client->request, RX_ALL_FIELDS };
        d.reads[1] = (RxDep){ aegis->client->slot[L->asked[k].slot], RX_ALL_FIELDS };
        d.n_writes = 1;
        d.writes[0] = (RxDep){ aegis->client->request, RX_ALL_FIELDS };
        d.n_caps = 3;
        d.caps[0] = (RxCapNeed){ run_cap, run_res, RX_RIGHT_READ };
        d.caps[1] = (RxCapNeed){ aegis->request_cap, aegis->request_res,
                                 RX_RIGHT_READ | RX_RIGHT_WRITE };
        d.caps[2] = (RxCapNeed){ aegis->slot_cap[L->asked[k].slot],
                                 aegis->slot_res[L->asked[k].slot], RX_RIGHT_READ };
        if (rx_world_add_reaction(w, &d, &L->ask_reaction[k]) != RX_OK) return AG_E_LOWER;
    }
    return 0;
}

int64_t rx_graph_start(AgLowered *L, RxCapRef external_run_cap, uint64_t run) {
    RxMutation m = { L->run, 0, run };
    return rx_world_publish_external(L->w, external_run_cap, &m, 1);
}

static int outcome(const AgGraph *g, const uint8_t *status) {
    for (uint32_t i = 0; i < g->n_failure; i++)
        if (status[g->failure[i].node] == g->failure[i].status) return AG_RUN_FAILURE;
    for (uint32_t i = 0; i < g->n_success; i++)
        if (status[g->success[i].node] != g->success[i].status) return AG_RUN_INCOMPLETE;
    return g->n_success ? AG_RUN_SUCCESS : AG_RUN_INCOMPLETE;
}

int rx_graph_collect(AgLowered *L, uint64_t run, AgResult *out) {
    memset(out, 0, sizeof *out);
    out->run = run;
    for (uint32_t n = 0; n < L->g->n_nodes; n++) {
        RxObject o;
        if (rx_world_read(L->w, L->cell[n], &o) != RX_OK) return AG_E_STALE;
        if (o.field[0] != run) continue;
        out->status[n] = (uint8_t)o.field[1];
        out->value[n] = o.field[2];
        out->evidence[n] = o.field[3];
        out->attempts[n] = (uint32_t)o.field[4];
    }
    out->outcome = outcome(L->g, out->status);
    return 0;
}

/* ---- reference ---- */

int rx_graph_reference(const AgGraph *g, RxWorld *w, const AgSkillTable *skills,
                       const AgCapTable *caps, uint64_t run, AgReference *ref) {
    memset(ref, 0, sizeof *ref);
    ref->r.run = run;
    uint16_t ord[AG_MAX_NODES];
    int cnt = topo(g, ord);
    if (cnt < 0) return cnt;
    /* Shadow of the World objects the graph touches. */
    RxObject shadow[AG_MAX_NODES];
    uint8_t live[AG_MAX_NODES] = { 0 };
    for (uint32_t n = 0; n < g->n_nodes; n++)
        if (has_target(g->nodes[n].kind)) live[n] = rx_world_read(w, g->nodes[n].obj, &shadow[n]) == RX_OK;
    uint8_t resolved[AG_MAX_NODES] = { 0 };
    uint32_t eff = 0;
    for (int t = 0; t < cnt; t++) {
        uint32_t n = ord[t];
        const AgNode *x = &g->nodes[n];
        if (has_target(x->kind) && !live[n]) continue;          /* stale: never runs */
        int allowed = 1;
        for (uint32_t i = 0; i < g->n_auth; i++)
            if (g->auth[i].node == n &&
                !cap_lookup(w, caps, g->subject, g->auth[i].resource, g->auth[i].rights, NULL))
                allowed = 0;
        if (!allowed) continue;                                 /* blocked: never runs */
        AgIn in;
        memset(&in, 0, sizeof in);
        uint32_t e[AG_MAX_IN];
        in.n_in = inputs_by_port(g, n, e);
        int ready = 1;
        for (uint32_t i = 0; i < in.n_in; i++) {
            uint32_t s = g->data[e[i]].from;
            if (!resolved[s]) ready = 0;
            in.s[i] = ref->r.status[s];
            in.v[i] = ref->r.value[s];
            in.mode[i] = g->data[e[i]].mode;
        }
        int gd = dep_of(g, n, AG_DEP_GUARD);
        if (gd >= 0) {
            uint32_t s = g->deps[gd].from;
            if (!resolved[s]) ready = 0;
            in.has_guard = 1;
            in.guard_s = ref->r.status[s];
            in.guard_v = ref->r.value[s];
            in.guard_pol = g->deps[gd].polarity;
        }
        for (uint32_t od = 0; od < g->n_deps; od++) {
            if (g->deps[od].to != n || g->deps[od].kind != AG_DEP_ORDER) continue;
            uint32_t s = g->deps[od].from;
            if (!resolved[s]) ready = 0;
            in.has_order = 1;
            if (ref->r.status[s] == AG_FAILED) in.order_s = AG_FAILED;
        }
        if (!ready) continue;
        /* The latest shadow of this object: the last boundary that wrote it. */
        if (reads_world(x->kind))
            for (uint32_t m = 0; m < g->n_nodes; m++)
                if (has_target(g->nodes[m].kind) && g->nodes[m].obj.id == x->obj.id && live[m])
                    memcpy(in.world, shadow[m].field, sizeof in.world);
        AgOut o;
        semantics(x, skills, &in, &o);
        resolved[n] = 1;
        ref->r.status[n] = o.status;
        ref->r.value[n] = o.value;
        ref->r.attempts[n] = o.attempts;
        ref->r.evidence[n] = rx_graph_evidence_word(x, run, o.status, o.value, in.v, in.n_in);
        if (is_boundary(x->kind)) {
            if (o.status == AG_OK && o.write) {
                RxObject *so = &shadow[n];
                if (x->kind == AG_WORLD_PUBLISH) {
                    so->field[x->field] = o.value;
                    ref->published[eff] = o.value;
                } else {
                    so->field[2] = rx_graph_chain(so->field[2], run, in.v[0]);
                    so->field[0] += 1;
                    so->field[1] = in.v[0];
                    so->field[3] = run;
                    ref->effect_count++;
                }
                for (uint32_t m = 0; m < g->n_nodes; m++)
                    if (m != n && has_target(g->nodes[m].kind) && g->nodes[m].obj.id == x->obj.id)
                        shadow[m] = *so;
            }
            ref->effect_chain[eff] = shadow[n].field[2];
            eff++;
        }
    }
    ref->r.outcome = outcome(g, ref->r.status);
    return 0;
}
