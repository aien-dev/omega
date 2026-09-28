/*
 * rx_capq.c -- Omega capability query IR. See rx_capq.h.
 *
 * The catalog is indexed by (operation, source). A query compiles to the
 * buckets of its operation and specializations, in the sources that may
 * realize it; it never walks the rest of the catalog and never reads a
 * description. What it returns is a bounded array of fixed-size records.
 */
#include "rx_capq.h"

#include "sha256.h"

#include <stdlib.h>
#include <string.h>

/* ---- small open-addressing maps (key 0 = empty) ---- */

static uint32_t pow2_at_least(uint32_t n) {
    uint32_t p = 16;
    while (p < n) p <<= 1;
    return p;
}

static uint32_t hslot(uint32_t key, uint32_t mask) { return (key * 0x9E3779B1u) & mask; }

static int map_put(uint32_t *keys, uint32_t mask, uint32_t *count, uint32_t key, uint32_t *slot) {
    if (key == 0) return CQ_E_ARG;
    uint32_t i = hslot(key, mask);
    for (uint32_t probe = 0; probe <= mask; probe++, i = (i + 1) & mask) {
        if (keys[i] == key) { *slot = i; return CQ_OK; }
        if (keys[i] == 0) {
            if ((*count + 1) * 2 > mask + 1) return CQ_E_NOMEM;   /* keep load <= 1/2 */
            keys[i] = key;
            (*count)++;
            *slot = i;
            return CQ_OK;
        }
    }
    return CQ_E_NOMEM;
}

static int map_get(const uint32_t *keys, uint32_t mask, uint32_t key, uint32_t *slot) {
    if (key == 0 || !keys) return 0;
    uint32_t i = hslot(key, mask);
    for (uint32_t probe = 0; probe <= mask; probe++, i = (i + 1) & mask) {
        if (keys[i] == key) { *slot = i; return 1; }
        if (keys[i] == 0) return 0;
    }
    return 0;
}

/* ---- catalog ---- */

int cq_catalog_init(CqCatalog *c, uint32_t self_machine, uint32_t alias_capacity,
                    uint32_t machine_capacity) {
    if (!c || self_machine == 0) return CQ_E_ARG;
    memset(c, 0, sizeof *c);
    c->self_machine = self_machine;
    uint32_t an = pow2_at_least(alias_capacity * 2 + 2), mn = pow2_at_least(machine_capacity * 2 + 2);
    c->alias_mask = an - 1;
    c->mach_mask = mn - 1;
    c->alias_key = calloc(an, sizeof *c->alias_key);
    c->alias_op = calloc(an, sizeof *c->alias_op);
    c->mach_key = calloc(mn, sizeof *c->mach_key);
    c->mach_lease = calloc(mn, sizeof *c->mach_lease);
    c->cap_ops = 64;
    c->ops = calloc(c->cap_ops + 1, sizeof *c->ops);
    if (!c->alias_key || !c->alias_op || !c->mach_key || !c->mach_lease || !c->ops) {
        cq_catalog_free(c);
        return CQ_E_NOMEM;
    }
    return CQ_OK;
}

void cq_catalog_free(CqCatalog *c) {
    if (!c) return;
    free(c->alias_key);
    free(c->alias_op);
    free(c->mach_key);
    free(c->mach_lease);
    free(c->ops);
    free(c->e);
    free(c->desc);
    free(c->bucket_start);
    free(c->bucket);
    memset(c, 0, sizeof *c);
}

int cq_op_define(CqCatalog *c, uint32_t op, uint32_t parent, uint32_t domains) {
    if (!c || op != c->n_ops + 1 || parent >= op || (domains & ~CQ_SRC_ALL)) return CQ_E_ARG;
    if (op > c->cap_ops) {
        uint32_t nc = c->cap_ops * 2;
        CqOp *n = realloc(c->ops, (size_t)(nc + 1) * sizeof *n);
        if (!n) return CQ_E_NOMEM;
        memset(n + c->cap_ops + 1, 0, (size_t)(nc - c->cap_ops) * sizeof *n);
        c->ops = n;
        c->cap_ops = nc;
    }
    c->ops[op] = (CqOp){ parent, domains, 0, 0 };
    if (parent) {
        c->ops[op].next_sibling = c->ops[parent].first_child;
        c->ops[parent].first_child = op;
    }
    c->n_ops = op;
    c->built = 0;
    return CQ_OK;
}

int cq_alias(CqCatalog *c, uint32_t alias, uint32_t op) {
    if (!c || op == 0 || op > c->n_ops || alias == op) return CQ_E_ARG;
    uint32_t s;
    int rc = map_put(c->alias_key, c->alias_mask, &c->n_alias, alias, &s);
    if (rc != CQ_OK) return rc;
    c->alias_op[s] = op;
    return CQ_OK;
}

int cq_machine_advertise(CqCatalog *c, uint32_t machine, uint64_t lease_until_us) {
    if (!c || machine == 0 || machine == c->self_machine) return CQ_E_ARG;
    uint32_t s;
    int rc = map_put(c->mach_key, c->mach_mask, &c->n_mach, machine, &s);
    if (rc != CQ_OK) return rc;
    c->mach_lease[s] = lease_until_us;
    return CQ_OK;
}

int cq_register(CqCatalog *c, const CqEntry *e, const char *desc, uint32_t desc_len) {
    if (!c || !e || e->op == 0 || e->op > c->n_ops || e->source >= CQ_SOURCES ||
        e->machine_id == 0 || (desc_len && !desc))
        return CQ_E_ARG;
    if (!(c->ops[e->op].domains & CQ_SRC(e->source))) return CQ_E_ARG;
    /* Only the Fabric speaks for another machine; the Fabric only for another machine. */
    if ((e->source == CQ_SRC_FABRIC) != (e->machine_id != c->self_machine)) return CQ_E_ARG;
    if (c->n == c->cap) {
        uint32_t nc = c->cap ? c->cap * 2 : 256;
        CqEntry *n = realloc(c->e, (size_t)nc * sizeof *n);
        if (!n) return CQ_E_NOMEM;
        c->e = n;
        c->cap = nc;
    }
    if (c->desc_bytes + desc_len > c->desc_cap) {
        uint64_t nc = c->desc_cap ? c->desc_cap : 65536;
        while (nc < c->desc_bytes + desc_len) nc *= 2;
        char *n = realloc(c->desc, nc);
        if (!n) return CQ_E_NOMEM;
        c->desc = n;
        c->desc_cap = nc;
    }
    CqEntry x = *e;
    x.desc_off = c->desc_bytes;
    x.desc_len = desc_len;
    if (desc_len) memcpy(c->desc + c->desc_bytes, desc, desc_len);
    c->desc_bytes += desc_len;
    c->e[c->n++] = x;
    c->built = 0;
    return CQ_OK;
}

static uint32_t bucket_key(uint32_t op, uint32_t source) { return op * CQ_SOURCES + source; }

int cq_catalog_build(CqCatalog *c) {
    if (!c) return CQ_E_ARG;
    free(c->bucket_start);
    free(c->bucket);
    uint32_t nb = (c->n_ops + 1) * CQ_SOURCES;
    c->bucket_start = calloc((size_t)nb + 1, sizeof *c->bucket_start);
    c->bucket = malloc((size_t)(c->n ? c->n : 1) * sizeof *c->bucket);
    if (!c->bucket_start || !c->bucket) return CQ_E_NOMEM;
    for (uint32_t i = 0; i < c->n; i++) c->bucket_start[bucket_key(c->e[i].op, c->e[i].source) + 1]++;
    for (uint32_t b = 0; b < nb; b++) c->bucket_start[b + 1] += c->bucket_start[b];
    uint32_t *fill = malloc((size_t)nb * sizeof *fill);
    if (!fill) return CQ_E_NOMEM;
    memcpy(fill, c->bucket_start, (size_t)nb * sizeof *fill);
    for (uint32_t i = 0; i < c->n; i++) c->bucket[fill[bucket_key(c->e[i].op, c->e[i].source)]++] = i;
    free(fill);
    c->built = 1;
    return CQ_OK;
}

uint32_t cq_resolve(const CqCatalog *c, uint32_t name) {
    if (!c || name == 0) return 0;
    uint32_t s;
    if (map_get(c->alias_key, c->alias_mask, name, &s)) return c->alias_op[s];
    return name <= c->n_ops ? name : 0;
}

uint64_t cq_catalog_bytes(const CqCatalog *c) {
    uint64_t b = (uint64_t)c->cap * sizeof(CqEntry) + (uint64_t)(c->cap_ops + 1) * sizeof(CqOp) +
                 (uint64_t)(c->alias_mask + 1) * 8u + (uint64_t)(c->mach_mask + 1) * 12u +
                 c->desc_cap;
    if (c->built) b += (uint64_t)((c->n_ops + 1) * CQ_SOURCES + 1) * 4u + (uint64_t)c->n * 4u;
    return b;
}

/* ---- compile ---- */

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void put_u32(sha256_ctx *h, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    sha256_update(h, b, 4);
}

static void put_u64(sha256_ctx *h, uint64_t v) {
    put_u32(h, (uint32_t)v);
    put_u32(h, (uint32_t)(v >> 32));
}

int cq_compile(const CqCatalog *c, const CqNeed *need, CqPlan *plan) {
    if (!plan) return CQ_E_ARG;
    memset(plan, 0, sizeof *plan);
    if (!c || !need) return plan->verdict = CQ_E_ARG;
    uint32_t op = cq_resolve(c, need->semantic_operation);
    if (op == 0) return plan->verdict = CQ_E_NO_OP;
    plan->op = op;

    /* The operation and every specialization of it (breadth first). */
    plan->ops[plan->n_ops++] = op;
    uint32_t domains = 0;
    for (uint32_t i = 0; i < plan->n_ops; i++) {
        domains |= c->ops[plan->ops[i]].domains;
        for (uint32_t ch = c->ops[plan->ops[i]].first_child; ch; ch = c->ops[ch].next_sibling) {
            if (plan->n_ops == CQ_MAX_PLAN_OPS) return plan->verdict = CQ_E_PLAN_FULL;
            plan->ops[plan->n_ops++] = ch;
        }
    }
    qsort(plan->ops, plan->n_ops, sizeof plan->ops[0], cmp_u32);

    /* Sources: those that can realize the operation, that the caller allows,
     * and that the locality allows. */
    uint32_t s = domains & (need->sources ? need->sources : CQ_SRC_ALL);
    uint32_t loc = need->locality_constraints.allowed ? need->locality_constraints.allowed
                                                       : (CQ_LOC_LOCAL | CQ_LOC_FABRIC);
    if (!(loc & CQ_LOC_LOCAL)) s &= CQ_SRC(CQ_SRC_FABRIC);
    if (!(loc & CQ_LOC_FABRIC)) s &= ~CQ_SRC(CQ_SRC_FABRIC);
    uint32_t pin = need->locality_constraints.machine;
    if (pin) s &= pin == c->self_machine ? ~CQ_SRC(CQ_SRC_FABRIC) : CQ_SRC(CQ_SRC_FABRIC);
    plan->sources = s;
    plan->n_probes = plan->n_ops * (uint32_t)__builtin_popcount(s);

    /* Identity: the resolved operation, not the name it was asked by. */
    sha256_ctx h;
    sha256_init(&h);
    sha256_update(&h, (const uint8_t *)"OMEGA-CAPQ-PLAN-1", 17);
    put_u32(&h, plan->op);
    put_u32(&h, plan->n_ops);
    for (uint32_t i = 0; i < plan->n_ops; i++) put_u32(&h, plan->ops[i]);
    put_u32(&h, plan->sources);
    put_u64(&h, need->accepted_input_types);
    put_u64(&h, need->required_output_types);
    put_u32(&h, need->effect_class);
    put_u64(&h, need->authority_ceiling.resource_lo);
    put_u64(&h, need->authority_ceiling.resource_hi);
    put_u32(&h, need->authority_ceiling.rights);
    put_u32(&h, loc);
    put_u32(&h, pin);
    put_u64(&h, need->latency_budget);
    put_u64(&h, need->energy_budget);
    put_u32(&h, need->reliability_requirement);
    put_u32(&h, need->evidence_requirement);
    sha256_final(&h, plan->digest);
    return plan->verdict = s ? CQ_OK : CQ_E_NO_SOURCE;
}

/* ---- ranking ---- */

uint64_t cq_dim_value(const CqCandidate *c, uint32_t dim) {
    switch (dim) {
    case CQ_DIM_COST:        return c->expected_cost;
    case CQ_DIM_LATENCY:     return c->expected_latency;
    case CQ_DIM_ENERGY:      return c->expected_energy;
    case CQ_DIM_CONFIDENCE:  return c->confidence >= CQ_PPM ? 0 : CQ_PPM - c->confidence;
    case CQ_DIM_RELIABILITY: return c->reliability >= CQ_PPM ? 0 : CQ_PPM - c->reliability;
    case CQ_DIM_AUTHORITY:   return c->required_authority.held ? 0 : 1;
    case CQ_DIM_LOCALITY:    return c->local ? 0 : 1;
    default:                 return 0;
    }
}

typedef struct {
    const CqCandidate *c;
    const uint8_t *dims;
    uint32_t n_dims;
} RankCtx;

/* Strict order: the named dimensions, then the ids. */
static int cmp_full(const CqCandidate *a, const CqCandidate *b, const uint8_t *dims, uint32_t nd) {
    for (uint32_t i = 0; i < nd; i++) {
        uint64_t x = cq_dim_value(a, dims[i]), y = cq_dim_value(b, dims[i]);
        if (x != y) return x < y ? -1 : 1;
    }
    uint32_t ka[4] = { a->capability_id, a->realization_id, a->machine_id, a->skill_id };
    uint32_t kb[4] = { b->capability_id, b->realization_id, b->machine_id, b->skill_id };
    for (int i = 0; i < 4; i++)
        if (ka[i] != kb[i]) return ka[i] < kb[i] ? -1 : 1;
    return 0;
}

static __thread RankCtx t_sort;   /* qsort has no context argument */

static int cmp_idx(const void *a, const void *b) {
    const RankCtx *r = &t_sort;
    return cmp_full(&r->c[*(const uint32_t *)a], &r->c[*(const uint32_t *)b], r->dims, r->n_dims);
}

static int dominates(const CqCandidate *a, const CqCandidate *b, const uint8_t *dims, uint32_t nd) {
    int strict = 0;
    for (uint32_t i = 0; i < nd; i++) {
        uint64_t x = cq_dim_value(a, dims[i]), y = cq_dim_value(b, dims[i]);
        if (x > y) return 0;
        if (x < y) strict = 1;
    }
    return strict;
}

static int within(uint64_t v, uint64_t best, uint32_t tol_ppm) {
    unsigned __int128 lim = (unsigned __int128)best * (CQ_PPM + (uint64_t)tol_ppm);
    return (unsigned __int128)v * CQ_PPM <= lim;
}

int cq_rank(const CqCandidate *in, uint32_t n, const CqTradeoffs *t, CqCandidate *out, uint32_t k,
            uint32_t *front) {
    static const uint8_t all_dims[CQ_DIMS] = { 0, 1, 2, 3, 4, 5, 6 };
    const uint8_t *dims = t && t->n_order ? t->order : all_dims;
    uint32_t nd = t && t->n_order ? t->n_order : CQ_DIMS;
    if (nd > CQ_DIMS) return CQ_E_ARG;
    for (uint32_t i = 0; i < nd; i++)
        if (dims[i] >= CQ_DIMS) return CQ_E_ARG;
    if (front) *front = 0;
    if (n == 0 || k == 0) return 0;

    uint32_t *idx = malloc((size_t)n * sizeof *idx);
    uint8_t *mark = calloc(n, 1);   /* 1 in pool, 2 taken */
    if (!idx || !mark) { free(idx); free(mark); return CQ_E_NOMEM; }
    for (uint32_t i = 0; i < n; i++) idx[i] = i;
    t_sort = (RankCtx){ in, dims, nd };
    qsort(idx, n, sizeof *idx, cmp_idx);

    /* Pareto front: a dominator sorts earlier, and dominance is transitive,
     * so checking against the front found so far is enough. */
    uint32_t nf = 0;
    uint32_t *fr = malloc((size_t)n * sizeof *fr);
    if (!fr) { free(idx); free(mark); return CQ_E_NOMEM; }
    for (uint32_t i = 0; i < n; i++) {
        const CqCandidate *x = &in[idx[i]];
        int dom = 0;
        for (uint32_t j = 0; j < nf && !dom; j++) dom = dominates(&in[fr[j]], x, dims, nd);
        if (!dom) fr[nf++] = idx[i];
    }
    if (front) *front = nf;
    int all = t && t->include_dominated;
    if (all) for (uint32_t i = 0; i < n; i++) mark[i] = 1;
    else for (uint32_t j = 0; j < nf; j++) mark[fr[j]] = 1;
    uint32_t pool = all ? n : nf;
    if (k > pool) k = pool;

    /* Caller's priorities: keep what is within the band of the best on the
     * first dimension, let the next decide among those, and so on; the strict
     * order (dimensions, then ids) breaks what remains. Pool members are
     * visited in the strict order, so the first survivor wins. */
    uint8_t *live = malloc(n);
    if (!live) { free(idx); free(mark); free(fr); return CQ_E_NOMEM; }
    uint32_t got = 0;
    while (got < k) {
        for (uint32_t i = 0; i < n; i++) live[i] = mark[i] == 1;
        for (uint32_t d = 0; d < nd; d++) {
            uint64_t best = UINT64_MAX;
            for (uint32_t i = 0; i < n; i++)
                if (live[i]) {
                    uint64_t v = cq_dim_value(&in[i], dims[d]);
                    if (v < best) best = v;
                }
            uint32_t tol = t ? t->tolerance_ppm[dims[d]] : 0;
            for (uint32_t i = 0; i < n; i++)
                if (live[i] && !within(cq_dim_value(&in[i], dims[d]), best, tol)) live[i] = 0;
        }
        uint32_t pick = UINT32_MAX;
        for (uint32_t i = 0; i < n && pick == UINT32_MAX; i++)
            if (live[idx[i]]) pick = idx[i];
        if (pick == UINT32_MAX) break;
        out[got++] = in[pick];
        mark[pick] = 2;
    }
    free(live);
    free(fr);
    free(mark);
    free(idx);
    return (int)got;
}

/* ---- run ---- */

size_t cq_result_bytes(const CqResult *r) {
    return offsetof(CqResult, cand) + (size_t)r->n * sizeof(CqCandidate);
}

static int machine_live(const CqCatalog *c, uint32_t machine, uint64_t now_us) {
    if (machine == c->self_machine) return 1;
    uint32_t s;
    return map_get(c->mach_key, c->mach_mask, machine, &s) && c->mach_lease[s] > now_us;
}

static uint32_t held_now(const CqHeld *h, uint64_t resource, uint32_t rights) {
    if (rights == 0) return 1;
    if (!h || !h->w || !h->caps) return 0;
    for (uint32_t i = 0; i < h->caps->n; i++) {
        if (h->caps->cap[i].resource != resource || (rights & ~h->caps->cap[i].rights)) continue;
        RxCapEntry ent;
        if (rx_world_validate_cap(h->w, h->caps->cap[i].ref, h->caps->subject, resource, rights,
                                  &ent) == RX_OK)
            return 1;
    }
    return 0;
}

int cq_query(const CqCatalog *c, const CqPlan *plan, const CqNeed *need, const CqTradeoffs *t,
             const CqHeld *held, uint64_t now_us, CqResult *out, CqStats *stats) {
    if (!out) return CQ_E_ARG;
    memset(out, 0, offsetof(CqResult, cand));
    CqStats st;
    memset(&st, 0, sizeof st);
    if (!c || !plan || !need || !t) return out->verdict = CQ_E_ARG;
    if (!c->built) return out->verdict = CQ_E_UNBUILT;
    if (plan->verdict != CQ_OK) {
        if (stats) *stats = st;
        return out->verdict = plan->verdict;
    }

    uint64_t upper = 0;
    for (uint32_t i = 0; i < plan->n_ops; i++)
        for (uint32_t s = 0; s < CQ_SOURCES; s++)
            if (plan->sources & CQ_SRC(s)) {
                uint32_t b = bucket_key(plan->ops[i], s);
                upper += c->bucket_start[b + 1] - c->bucket_start[b];
            }
    CqCandidate *feas = malloc((size_t)(upper ? upper : 1) * sizeof *feas);
    if (!feas) return out->verdict = CQ_E_NOMEM;

    const uint32_t loc = need->locality_constraints.allowed ? need->locality_constraints.allowed
                                                             : (CQ_LOC_LOCAL | CQ_LOC_FABRIC);
    const uint32_t pin = need->locality_constraints.machine;
    uint32_t nf = 0;
    for (uint32_t i = 0; i < plan->n_ops; i++)
        for (uint32_t s = 0; s < CQ_SOURCES; s++) {
            if (!(plan->sources & CQ_SRC(s))) continue;
            uint32_t b = bucket_key(plan->ops[i], s);
            for (uint32_t j = c->bucket_start[b]; j < c->bucket_start[b + 1]; j++) {
                const CqEntry *e = &c->e[c->bucket[j]];
                st.probed++;
                if (!e->live || !machine_live(c, e->machine_id, now_us)) { st.rejected_dead++; continue; }
                int local = e->machine_id == c->self_machine;
                if (!(loc & (local ? CQ_LOC_LOCAL : CQ_LOC_FABRIC))) continue;
                if (pin && e->machine_id != pin) continue;
                if (e->in_types & ~need->accepted_input_types) continue;
                if (need->required_output_types & ~e->out_types) continue;
                if (e->effects & ~need->effect_class) continue;
                if (e->auth_rights & ~need->authority_ceiling.rights) continue;
                if (e->auth_rights && (e->auth_resource < need->authority_ceiling.resource_lo ||
                                       e->auth_resource > need->authority_ceiling.resource_hi))
                    continue;
                if (need->latency_budget && e->latency_us > need->latency_budget) continue;
                if (need->energy_budget && e->energy_uj > need->energy_budget) continue;
                if (e->reliability_ppm < need->reliability_requirement) continue;
                if (e->evidence_level < need->evidence_requirement) continue;
                if (t->max_cost && e->cost > t->max_cost) continue;
                if (e->confidence_ppm < t->min_confidence) continue;
                CqCandidate *x = &feas[nf++];
                memset(x, 0, sizeof *x);
                x->capability_id = e->capability_id;
                x->skill_id = e->skill_id;
                x->machine_id = e->machine_id;
                x->realization_id = e->realization_id;
                x->required_authority.resource = e->auth_resource;
                x->required_authority.rights = e->auth_rights;
                x->required_authority.held = held_now(held, e->auth_resource, e->auth_rights);
                x->expected_cost = e->cost;
                x->expected_latency = e->latency_us;
                x->expected_energy = e->energy_uj;
                x->confidence = e->confidence_ppm;
                x->reliability = e->reliability_ppm;
                x->evidence.ref = e->evidence_ref;
                x->evidence.level = e->evidence_level;
                x->source = e->source;
                x->local = (uint8_t)local;
                x->effects = e->effects;
            }
        }
    st.feasible = nf;
    uint32_t k = t->k ? t->k : CQ_MAX_K;
    if (k > CQ_MAX_K) k = CQ_MAX_K;
    uint32_t front = 0;
    int n = cq_rank(feas, nf, t, out->cand, k, &front);
    free(feas);
    if (n < 0) return out->verdict = n;
    out->n = (uint32_t)n;
    out->n_feasible = nf;
    out->n_front = front;
    if (stats) *stats = st;
    return out->verdict = CQ_OK;
}

/* ---- consumption ---- */

int cq_bind_skill_node(AgGraph *g, uint32_t node, const CqCandidate *c) {
    if (!g || !c || node >= g->n_nodes || c->skill_id == 0) return CQ_E_ARG;
    if (g->nodes[node].kind != AG_SKILL && g->nodes[node].kind != AG_RETRY) return CQ_E_ARG;
    g->nodes[node].op = c->skill_id;
    if (c->required_authority.rights &&
        rx_graph_need_authority(g, node, c->required_authority.resource,
                                c->required_authority.rights) < 0)
        return CQ_E_ARG;
    return CQ_OK;
}

const char *cq_source_name(uint32_t s) {
    static const char *n[CQ_SOURCES] = { "capability_graph", "skill_network", "mcp_registry",
                                         "local_physical", "fabric" };
    return s < CQ_SOURCES ? n[s] : "?";
}
