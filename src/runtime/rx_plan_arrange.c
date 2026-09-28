/*
 * rx_plan_arrange.c -- arrangement domain, goal shape, binding, A* planner
 * and generaliser. See rx_plan_arrange.h.
 */
#include "rx_plan_arrange.h"

#include "omega_types.h"
#include "sha256.h"

#include <stdlib.h>
#include <string.h>

#define FLOOR 15u
#define NONE  0xFFu

uint64_t pla_fact(RxObjRef x, const RxObjRef *t) {
    uint64_t v = (1ull << 63) | (x.id & 0xffu) | ((uint64_t)(x.generation & 0xffffffu) << 8);
    if (!t) return v | (1ull << 62);
    return v | ((uint64_t)(t->id & 0xffu) << 32) | ((uint64_t)(t->generation & 0x3fffffu) << 40);
}

static void fact_unpack(uint64_t f, RxObjRef *x, RxObjRef *t, uint8_t *floor) {
    x->id = (uint32_t)(f & 0xffu);
    x->generation = (uint32_t)((f >> 8) & 0xffffffu);
    *floor = (uint8_t)((f >> 62) & 1u);
    t->id = (uint32_t)((f >> 32) & 0xffu);
    t->generation = (uint32_t)((f >> 40) & 0x3fffffu);
}

static int same(RxObjRef a, RxObjRef b) { return a.id == b.id && a.generation == b.generation; }

static const uint64_t *fields(const PlView *v, RxObjRef r) {
    if (r.id >= v->n || !v->o[r.id].live || v->o[r.id].generation != r.generation) return NULL;
    return v->o[r.id].field;
}

/* ---- goal ---- */

int pla_goal_read(const PlView *v, const PlEnv *env, PlaGoal *g, PlCost *cost) {
    memset(g, 0, sizeof *g);
    const uint64_t *gf = fields(v, env->obj[PL_ENV_GOAL]);
    if (!gf || v->o[env->obj[PL_ENV_GOAL].id].type != PL_OT_GOAL) return -1;
    g->seq = gf[0];
    g->kind = gf[1];
    g->max_steps = gf[2];
    g->energy = gf[3];
    if (!gf[4]) return -1;
    RxObjRef fr = pl_ref_unpack(gf[4]);
    const uint64_t *ff = fields(v, fr);
    if (!ff || v->o[fr.id].type != PL_OT_GOALFACTS) return -1;
    for (uint32_t i = 0; i < PLA_MAX_FACTS; i++) {
        if (cost) cost->shape_ops++;
        if (!(ff[i] >> 63)) continue;
        uint32_t k = g->n_facts++;
        fact_unpack(ff[i], &g->x[k], &g->t[k], &g->floor[k]);
    }
    if (!g->n_facts) return -1;

    /* Nodes of the goal forest. */
    RxObjRef node[2 * PLA_MAX_FACTS];
    uint32_t nn = 0;
    int parent[2 * PLA_MAX_FACTS], child[2 * PLA_MAX_FACTS];
    for (uint32_t i = 0; i < g->n_facts; i++) {
        RxObjRef c[2] = { g->x[i], g->t[i] };
        for (uint32_t q = 0; q < (g->floor[i] ? 1u : 2u); q++) {
            uint32_t k = 0;
            while (k < nn && !same(node[k], c[q])) k++;
            if (k == nn) node[nn++] = c[q];
        }
    }
    for (uint32_t k = 0; k < nn; k++) parent[k] = -2, child[k] = -1;   /* -2 no fact, -1 floor */
    for (uint32_t i = 0; i < g->n_facts; i++) {
        uint32_t a = 0, b = 0;
        while (!same(node[a], g->x[i])) a++;
        if (parent[a] != -2) return -2;                 /* two facts about one unit */
        if (g->floor[i]) { parent[a] = -1; continue; }
        while (!same(node[b], g->t[i])) b++;
        if (a == b || child[b] >= 0) return -2;         /* self, or two units on one */
        parent[a] = (int)b;
        child[b] = (int)a;
    }
    /* Chains from each bottom, then a canonical order: grounded first, longer first. */
    uint32_t seen = 0;
    struct { uint32_t len, grounded; RxObjRef o[2 * PLA_MAX_FACTS]; } ch[2 * PLA_MAX_FACTS];
    uint32_t nc = 0;
    for (uint32_t k = 0; k < nn; k++) {
        if (parent[k] >= 0) continue;
        ch[nc].len = 0;
        ch[nc].grounded = parent[k] == -1;
        for (int u = (int)k; u >= 0; u = child[u]) {
            ch[nc].o[ch[nc].len++] = node[u];
            seen++;
            if (cost) cost->shape_ops++;
        }
        nc++;
    }
    if (seen != nn) return -3;                          /* a cycle */
    for (uint32_t i = 1; i < nc; i++)
        for (uint32_t j = i; j > 0; j--) {
            int swap = ch[j].grounded > ch[j - 1].grounded ||
                       (ch[j].grounded == ch[j - 1].grounded && ch[j].len > ch[j - 1].len);
            if (!swap) break;
            __typeof__(ch[0]) tmp = ch[j];
            ch[j] = ch[j - 1];
            ch[j - 1] = tmp;
            if (cost) cost->shape_ops++;
        }
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)"OMEGA_PLAN_SHAPE_ARRANGE_V1", 27);
    uint8_t b[8] = { (uint8_t)g->kind, (uint8_t)nc };
    sha256_update(&c, b, 2);
    for (uint32_t i = 0; i < nc; i++) {
        g->chain[i].start = g->n_obj;
        g->chain[i].len = ch[i].len;
        g->chain[i].grounded = ch[i].grounded;
        for (uint32_t j = 0; j < ch[i].len; j++) g->obj[g->n_obj++] = ch[i].o[j];
        b[0] = (uint8_t)ch[i].grounded;
        b[1] = (uint8_t)ch[i].len;
        sha256_update(&c, b, 2);
    }
    g->n_chains = nc;
    sha256_final(&c, g->shape);
    return 0;
}

/* ---- binding ---- */

static uint32_t fact_n(uint32_t n) { uint32_t f = 1; while (n > 1) f *= n--; return f; }

int pla_bind(const PlanTemplate *t, const PlView *v, const PlaGoal *g, uint32_t attempt,
             RxObjRef slots[PL_MAX_SLOTS], PlCost *cost) {
    if (t->n_goal_slots != g->n_obj || t->n_slots > PL_MAX_SLOTS) return 0;
    /* Groups of interchangeable chains; attempt = mixed-radix permutation index. */
    uint32_t order[2 * PLA_MAX_FACTS];
    uint32_t rest = attempt;
    uint64_t total = 1;
    for (uint32_t i = 0; i < g->n_chains;) {
        uint32_t j = i;
        while (j < g->n_chains && g->chain[j].len == g->chain[i].len &&
               g->chain[j].grounded == g->chain[i].grounded) j++;
        uint32_t m = j - i, radix = fact_n(m);
        total *= radix;
        uint32_t code = rest % radix;
        rest /= radix;
        uint32_t pool[2 * PLA_MAX_FACTS], np = m;
        for (uint32_t k = 0; k < m; k++) pool[k] = i + k;
        for (uint32_t k = 0; k < m; k++) {
            uint32_t f = fact_n(m - 1 - k), pick = code / f;
            code %= f;
            order[i + k] = pool[pick];
            for (uint32_t q = pick; q + 1 < np; q++) pool[q] = pool[q + 1];
            np--;
        }
        i = j;
    }
    if ((uint64_t)attempt >= total || total > 720) return 0;
    if (cost) cost->bind_attempts++;
    uint32_t s = 0;
    for (uint32_t i = 0; i < g->n_chains; i++) {
        uint32_t c = order[i];
        for (uint32_t k = 0; k < g->chain[c].len; k++) slots[s++] = g->obj[g->chain[c].start + k];
    }
    for (; s < t->n_slots; s++) {
        const PlSlot *sl = &t->slots[s];
        if (sl->bind_rule != PL_BIND_RESTS_ON || sl->bind_from >= s) return -1;
        uint64_t want = pl_ref_pack(slots[sl->bind_from]);
        int found = -1;
        for (uint32_t i = 0; i < v->n; i++) {
            if (!v->o[i].live || v->o[i].type != sl->type) continue;
            if (v->o[i].field[sl->bind_field & (RX_MAX_FIELDS - 1)] != want) continue;
            if (found >= 0) return -1;                  /* not unique */
            found = (int)i;
        }
        if (cost) cost->preds++;
        if (found < 0) return -1;
        slots[s] = (RxObjRef){ (uint32_t)found, v->o[found].generation };
    }
    return 1;
}

/* ---- targets ---- */

void pla_target_from_goal(const PlaGoal *g, PlaTarget *tg) {
    memset(tg, 0, sizeof *tg);
    for (uint32_t i = 0; i < g->n_facts; i++) {
        tg->x[tg->n_facts] = g->x[i];
        tg->t[tg->n_facts] = g->t[i];
        tg->floor[tg->n_facts++] = g->floor[i];
    }
    for (uint32_t i = 0; i < g->n_obj; i++) tg->insider[tg->n_insiders++] = g->obj[i];
}

int pla_target_from_template(const PlanTemplate *t, const RxObjRef *slots, PlaTarget *tg) {
    memset(tg, 0, sizeof *tg);
    for (uint32_t i = 0; i < t->state.n; i++) {
        const PlPred *p = &t->state.p[i];
        if (p->field != PLA_F_SUPPORT) continue;
        if (p->kind == PL_P_NOT_SLOT) return -1;
        if (p->kind == PL_P_EQ_CONST && p->value == 0) {
            tg->x[tg->n_facts] = slots[p->slot];
            tg->floor[tg->n_facts++] = 1;
        } else if (p->kind == PL_P_EQ_SLOT) {
            tg->x[tg->n_facts] = slots[p->slot];
            tg->t[tg->n_facts++] = slots[p->slot2];
        }
    }
    for (uint32_t i = 0; i < t->invariants.n; i++)
        if (t->invariants.p[i].kind == PL_P_UNREFERENCED)
            tg->protect[tg->n_protect++] = slots[t->invariants.p[i].slot];
    for (uint32_t s = 0; s < t->n_slots; s++) tg->insider[tg->n_insiders++] = slots[s];
    return 0;
}

/* ---- A* ---- */

typedef struct { uint64_t state; uint32_t g, parent; uint8_t mover, dest; } SNode;
typedef struct { uint32_t f, h, node; uint64_t seq; } HeapE;

typedef struct {
    uint32_t n;
    uint8_t want[PLA_MAX_UNITS];        /* target index, FLOOR, or NONE */
    uint16_t insider, protect;
} Model;

static inline uint32_t sup(uint64_t s, uint32_t u) { return (uint32_t)((s >> (4 * u)) & 0xfu); }
static inline uint64_t set_sup(uint64_t s, uint32_t u, uint32_t v) {
    return (s & ~(0xfull << (4 * u))) | ((uint64_t)v << (4 * u));
}

static uint32_t heur(const Model *m, uint64_t s) {
    uint8_t wp[PLA_MAX_UNITS];
    memset(wp, 2, sizeof wp);                   /* 2 = unknown */
    uint32_t h = 0;
    for (uint32_t u = 0; u < m->n; u++) {
        if (m->want[u] == NONE) continue;
        /* Walk down while the fact holds; well placed if it holds all the way. */
        uint32_t path[PLA_MAX_UNITS], np = 0, x = u;
        int ok = 1;
        for (;;) {
            if (wp[x] != 2) { ok = wp[x]; break; }
            path[np++] = x;
            if (sup(s, x) != m->want[x]) { ok = 0; break; }
            if (m->want[x] == FLOOR) break;
            x = m->want[x];
            if (m->want[x] == NONE) break;
            if (np > m->n) { ok = 0; break; }
        }
        for (uint32_t i = 0; i < np; i++) wp[path[i]] = (uint8_t)ok;
        h += !wp[u];
    }
    for (uint32_t u = 0; u < m->n; u++) {
        if (m->insider & (1u << u)) continue;
        uint32_t d = sup(s, u);
        if (d != FLOOR && (m->protect & (1u << d))) h++;
    }
    return h;
}

static void heap_push(HeapE *h, uint32_t *n, HeapE e) {
    uint32_t i = (*n)++;
    h[i] = e;
    while (i) {
        uint32_t p = (i - 1) / 2;
        int less = h[i].f < h[p].f || (h[i].f == h[p].f && (h[i].h < h[p].h ||
                   (h[i].h == h[p].h && h[i].seq < h[p].seq)));
        if (!less) break;
        HeapE t = h[i]; h[i] = h[p]; h[p] = t;
        i = p;
    }
}

static HeapE heap_pop(HeapE *h, uint32_t *n) {
    HeapE top = h[0];
    h[0] = h[--(*n)];
    uint32_t i = 0;
    for (;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        #define LESS(a, b) (h[a].f < h[b].f || (h[a].f == h[b].f && (h[a].h < h[b].h || \
                           (h[a].h == h[b].h && h[a].seq < h[b].seq))))
        if (l < *n && LESS(l, m)) m = l;
        if (r < *n && LESS(r, m)) m = r;
        #undef LESS
        if (m == i) break;
        HeapE t = h[i]; h[i] = h[m]; h[m] = t;
        i = m;
    }
    return top;
}

static inline uint64_t hmix(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull;
    return x ^ (x >> 33);
}

int pla_plan(const PlView *v, const PlaGoal *g, const PlaTarget *tg, uint64_t max_expand,
             PlaPlan *out, PlCost *cost) {
    (void)g;
    memset(out, 0, sizeof *out);
    /* Units: insiders first in the target's order, then the rest by id. */
    RxObjRef unit[PLA_MAX_UNITS];
    uint32_t n = 0;
    for (uint32_t i = 0; i < tg->n_insiders; i++) {
        if (!fields(v, tg->insider[i]) || v->o[tg->insider[i].id].type != PL_OT_UNIT) return -1;
        int dup = 0;
        for (uint32_t k = 0; k < n; k++) dup |= same(unit[k], tg->insider[i]);
        if (dup) continue;
        if (n >= PLA_MAX_UNITS) return -2;
        unit[n++] = tg->insider[i];
    }
    uint32_t n_in = n;
    for (uint32_t i = 0; i < v->n; i++) {
        if (!v->o[i].live || v->o[i].type != PL_OT_UNIT) continue;
        RxObjRef r = { i, v->o[i].generation };
        int dup = 0;
        for (uint32_t k = 0; k < n_in; k++) dup |= same(unit[k], r);
        if (dup) continue;
        if (n >= PLA_MAX_UNITS) return -2;
        unit[n++] = r;
    }
    Model m;
    memset(&m, 0, sizeof m);
    m.n = n;
    memset(m.want, NONE, sizeof m.want);
    for (uint32_t k = 0; k < n_in; k++) m.insider |= (uint16_t)(1u << k);
    #define IDX(r, out_) do { out_ = NONE; for (uint32_t q_ = 0; q_ < n; q_++) \
                              if (same(unit[q_], (r))) { out_ = q_; break; } } while (0)
    for (uint32_t i = 0; i < tg->n_facts; i++) {
        uint32_t a, b = FLOOR;
        IDX(tg->x[i], a);
        if (a == NONE) return -1;
        if (!tg->floor[i]) { IDX(tg->t[i], b); if (b == NONE) return -1; }
        m.want[a] = (uint8_t)b;
    }
    for (uint32_t i = 0; i < tg->n_protect; i++) {
        uint32_t a;
        IDX(tg->protect[i], a);
        if (a == NONE) return -1;
        m.protect |= (uint16_t)(1u << a);
    }
    uint64_t s0 = 0;
    for (uint32_t u = 0; u < n; u++) {
        uint64_t f = v->o[unit[u].id].field[PLA_F_SUPPORT];
        uint32_t d = FLOOR;
        if (f) { IDX(pl_ref_unpack(f), d); if (d == NONE) return -3; }
        s0 = set_sup(s0, u, d);
    }
    for (uint32_t u = n; u < 16; u++) s0 = set_sup(s0, u, 0);
    #undef IDX

    /* Tables start small and double as the search grows. */
    uint64_t cap_nodes = 1024, cap_heap = 1024, hcap = 2048;
    SNode *nodes = malloc(cap_nodes * sizeof *nodes);
    HeapE *heap = malloc(cap_heap * sizeof *heap);
    uint32_t *table = malloc(hcap * sizeof *table);
    if (!nodes || !heap || !table) { free(nodes); free(heap); free(table); return -4; }
    memset(table, 0xff, hcap * sizeof *table);
    uint32_t nn = 0, nh = 0;
    uint64_t seq = 0;
    int rc = -5;

    nodes[nn] = (SNode){ s0, 0, UINT32_MAX, 0, 0 };
    table[hmix(s0) & (hcap - 1)] = nn;
    uint32_t h0 = heur(&m, s0);
    if (cost) cost->heuristic++;
    heap_push(heap, &nh, (HeapE){ h0, h0, nn, seq++ });
    nn++;
    uint64_t expanded = 0;
    while (nh) {
        HeapE e = heap_pop(heap, &nh);
        SNode cur = nodes[e.node];
        if (e.f != cur.g + e.h) continue;               /* superseded entry */
        if (e.h == 0) {
            /* Goal test (h == 0 is exact here: every fact well placed, no outsider on a protected unit). */
            uint32_t len = cur.g, at = e.node;
            if (len > PLA_MAX_MOVES) { rc = -6; break; }
            out->n = len;
            for (uint32_t k = len; k > 0; k--) {
                const SNode *x = &nodes[at];
                out->mover[k - 1] = unit[x->mover];
                out->to_floor[k - 1] = x->dest == FLOOR;
                if (x->dest != FLOOR) out->dest[k - 1] = unit[x->dest];
                at = x->parent;
            }
            rc = 0;
            break;
        }
        if (++expanded > max_expand) { rc = -5; break; }
        if (cost) cost->expanded++;
        uint16_t covered = 0;
        for (uint32_t u = 0; u < n; u++) {
            uint32_t d = sup(cur.state, u);
            if (d != FLOOR) covered |= (uint16_t)(1u << d);
        }
        for (uint32_t u = 0; u < n; u++) {
            if (covered & (1u << u)) continue;
            uint32_t from = sup(cur.state, u);
            for (uint32_t dd = 0; dd <= n; dd++) {
                uint32_t d = dd == n ? FLOOR : dd;
                if (d == from || d == u) continue;
                if (d != FLOOR) {
                    if (!(m.insider & (1u << d)) || (covered & (1u << d))) continue;
                    if (!(m.insider & (1u << u))) continue;  /* outsiders only go to the floor */
                }
                uint64_t ns = set_sup(cur.state, u, d);
                if (cost) cost->generated++;
                uint64_t hi = hmix(ns) & (hcap - 1);
                uint32_t found = UINT32_MAX;
                while (table[hi] != UINT32_MAX) {
                    if (nodes[table[hi]].state == ns) { found = table[hi]; break; }
                    hi = (hi + 1) & (hcap - 1);
                }
                uint32_t ng = cur.g + 1;
                if (found != UINT32_MAX && nodes[found].g <= ng) continue;
                if (nn + 1 >= cap_nodes) {
                    SNode *g2 = realloc(nodes, 2 * cap_nodes * sizeof *nodes);
                    if (!g2) { rc = -4; goto done; }
                    nodes = g2;
                    cap_nodes *= 2;
                }
                if (nh + 1 >= cap_heap) {
                    HeapE *h2 = realloc(heap, 2 * cap_heap * sizeof *heap);
                    if (!h2) { rc = -4; goto done; }
                    heap = h2;
                    cap_heap *= 2;
                }
                if (2 * (nn + 1) > hcap) {
                    /* Rehash at half load. */
                    uint32_t *t2 = malloc(2 * hcap * sizeof *t2);
                    if (!t2) { rc = -4; goto done; }
                    memset(t2, 0xff, 2 * hcap * sizeof *t2);
                    for (uint32_t q = 0; q < nn; q++) {
                        uint64_t p = hmix(nodes[q].state) & (2 * hcap - 1);
                        while (t2[p] != UINT32_MAX) p = (p + 1) & (2 * hcap - 1);
                        t2[p] = q;
                    }
                    free(table);
                    table = t2;
                    hcap *= 2;
                    if (found == UINT32_MAX) {
                        hi = hmix(ns) & (hcap - 1);
                        while (table[hi] != UINT32_MAX) hi = (hi + 1) & (hcap - 1);
                    }
                }
                uint32_t id = found;
                if (found == UINT32_MAX) {
                    id = nn++;
                    table[hi] = id;
                }
                nodes[id] = (SNode){ ns, ng, e.node, (uint8_t)u, (uint8_t)d };
                uint32_t h = heur(&m, ns);
                if (cost) cost->heuristic++;
                heap_push(heap, &nh, (HeapE){ ng + h, h, id, seq++ });
            }
        }
    }
done:
    free(nodes);
    free(heap);
    free(table);
    return rc;
}

int pla_apply(PlView *v, const PlaPlan *p) {
    for (uint32_t k = 0; k < p->n; k++) {
        uint64_t *x = (uint64_t *)fields(v, p->mover[k]);
        if (!x) return -1;
        uint64_t me = pl_ref_pack(p->mover[k]);
        uint64_t to = p->to_floor[k] ? 0 : pl_ref_pack(p->dest[k]);
        if (!p->to_floor[k] && !fields(v, p->dest[k])) return -1;
        for (uint32_t i = 0; i < v->n; i++) {
            if (!v->o[i].live || v->o[i].type != PL_OT_UNIT) continue;
            if (v->o[i].field[PLA_F_SUPPORT] == me) return -2;           /* mover not clear */
            if (to && v->o[i].field[PLA_F_SUPPORT] == to) return -3;     /* destination not clear */
        }
        x[PLA_F_SUPPORT] = to;
    }
    return 0;
}

int pla_legal(const PlView *v) {
    for (uint32_t i = 0; i < v->n; i++) {
        if (!v->o[i].live || v->o[i].type != PL_OT_UNIT) continue;
        RxObjRef me = { i, v->o[i].generation };
        if (v->o[i].field[PLA_F_SELF] != pl_ref_pack(me)) return 0;
        uint64_t s = v->o[i].field[PLA_F_SUPPORT];
        uint32_t steps = 0;
        while (s) {
            const uint64_t *f = fields(v, pl_ref_unpack(s));
            if (!f || v->o[pl_ref_unpack(s).id].type != PL_OT_UNIT) return 0;
            if (++steps > v->n) return 0;               /* cycle */
            s = f[PLA_F_SUPPORT];
        }
        for (uint32_t j = i + 1; j < v->n; j++)
            if (v->o[j].live && v->o[j].type == PL_OT_UNIT && v->o[i].field[PLA_F_SUPPORT] &&
                v->o[j].field[PLA_F_SUPPORT] == v->o[i].field[PLA_F_SUPPORT])
                return 0;                               /* one unit carries two */
    }
    return 1;
}

/* ---- generalisation ---- */

static int slot_of(const RxObjRef *slots, uint32_t n, RxObjRef r) {
    for (uint32_t s = 0; s < n; s++)
        if (same(slots[s], r)) return (int)s;
    return -1;
}

static void add(PlPredList *l, PlPred p) { if (l->n < PL_MAX_PREDS) l->p[l->n++] = p; }

int pla_generalize(const PlView *v0, const PlaGoal *g, const PlaPlan *p, uint32_t origin,
                   const uint8_t *parent, uint64_t cog_gen, PlanTemplate *t,
                   RxObjRef slots[PL_MAX_SLOTS]) {
    memset(t, 0, sizeof *t);
    t->goal_kind = PLA_GOAL_ARRANGE;
    memcpy(t->goal_shape, g->shape, 32);
    if (g->n_obj > PL_MAX_SLOTS) return -1;
    uint32_t n = 0;
    for (uint32_t i = 0; i < g->n_obj; i++) {
        slots[n] = g->obj[i];
        t->slots[n++] = (PlSlot){ PL_OT_UNIT, PL_ROLE_GOAL, 0, PL_BIND_FROM_GOAL, 0, 0 };
    }
    t->n_goal_slots = n;
    /* Closure: units resting on a slot at the start. */
    for (uint32_t s = 0; s < n; s++) {
        uint64_t me = pl_ref_pack(slots[s]);
        for (uint32_t i = 0; i < v0->n; i++) {
            if (!v0->o[i].live || v0->o[i].type != PL_OT_UNIT) continue;
            if (v0->o[i].field[PLA_F_SUPPORT] != me) continue;
            RxObjRef r = { i, v0->o[i].generation };
            if (slot_of(slots, n, r) >= 0) continue;
            if (n >= PL_MAX_SLOTS) return -1;
            slots[n] = r;
            t->slots[n++] = (PlSlot){ PL_OT_UNIT, PL_ROLE_INVOLVED, 0, PL_BIND_RESTS_ON, s,
                                      PLA_F_SUPPORT };
        }
    }
    t->n_slots = n;
    for (uint32_t k = 0; k < p->n; k++)
        if (slot_of(slots, n, p->mover[k]) < 0 ||
            (!p->to_floor[k] && slot_of(slots, n, p->dest[k]) < 0))
            return -2;

    /* Required state and invariants. */
    for (uint32_t s = 0; s < n; s++) {
        uint64_t f = v0->o[slots[s].id].field[PLA_F_SUPPORT];
        int j = f ? slot_of(slots, n, pl_ref_unpack(f)) : -1;
        if (!f) add(&t->state, (PlPred){ PL_P_EQ_CONST, s, PLA_F_SUPPORT, 0, 0, 0 });
        else if (j >= 0) add(&t->state, (PlPred){ PL_P_EQ_SLOT, s, PLA_F_SUPPORT, (uint32_t)j, 0, 0 });
        else add(&t->state, (PlPred){ PL_P_NOT_SLOT, s, PLA_F_SUPPORT, 0, 0, 0 });
        add(&t->invariants, (PlPred){ PL_P_UNREFERENCED, s, PLA_F_SUPPORT, 0, PL_OT_UNIT, 0 });
        add(&t->invariants, (PlPred){ PL_P_SELF_REF, s, PLA_F_SELF, 0, 0, 0 });
    }
    t->n_steps = p->n;
    t->step_need.energy_cost = PLA_MOVE_ENERGY;
    t->energy_total = (uint64_t)p->n * PLA_MOVE_ENERGY;
    add(&t->pre, (PlPred){ PL_P_ENV_EQ, PL_ENV_GOAL, 1, 0, 0, PLA_GOAL_ARRANGE });
    add(&t->pre, (PlPred){ PL_P_ENV_GE, PL_ENV_GOAL, 2, 0, 0, p->n });
    add(&t->pre, (PlPred){ PL_P_ENV_GE, PL_ENV_GOAL, 3, 0, 0, t->energy_total });
    add(&t->env, (PlPred){ PL_P_ENV_EQ, PL_ENV_STATION, 0, 0, 0, 1 });
    for (uint32_t i = 0; i < g->n_facts; i++) {
        int a = slot_of(slots, n, g->x[i]);
        int b = g->floor[i] ? -1 : slot_of(slots, n, g->t[i]);
        if (a < 0 || (!g->floor[i] && b < 0)) return -3;
        if (g->floor[i]) add(&t->success, (PlPred){ PL_P_EQ_CONST, (uint32_t)a, PLA_F_SUPPORT, 0, 0, 0 });
        else add(&t->success, (PlPred){ PL_P_EQ_SLOT, (uint32_t)a, PLA_F_SUPPORT, (uint32_t)b, 0, 0 });
    }
    t->fail_flags = PL_FAIL_GRAPH | PL_FAIL_INVARIANT | PL_FAIL_EVIDENCE | PL_FAIL_CRUMBS;

    /* The action graph. Parameters are slot + 1. */
    AgGraph *G = &t->graph;
    rx_graph_init(G, 0);
    int prev = -1;
    #define NODE(k, ty) ({ int n_ = rx_graph_node(G, (k), (ty)); if (n_ < 0) return -4; n_; })
    for (uint32_t k = 0; k < p->n; k++) {
        uint32_t xs = (uint32_t)slot_of(slots, n, p->mover[k]);
        int src;
        if (p->to_floor[k]) {
            src = NODE(AG_CONST, AG_T_U64);
            G->nodes[src].imm = 0;
        } else {
            src = NODE(AG_WORLD_READ, AG_T_U64);
            G->nodes[src].param = (uint32_t)slot_of(slots, n, p->dest[k]) + 1;
            G->nodes[src].field = PLA_F_SELF;
            t->slots[G->nodes[src].param - 1].rights |= RX_RIGHT_READ;
            if (prev >= 0) rx_graph_order(G, (uint32_t)prev, (uint32_t)src);
        }
        int pub = NODE(AG_WORLD_PUBLISH, AG_T_RECEIPT);
        G->nodes[pub].param = xs + 1;
        G->nodes[pub].field = PLA_F_SUPPORT;
        t->slots[xs].rights |= RX_RIGHT_WRITE;
        rx_graph_data(G, (uint32_t)src, (uint32_t)pub, 0, AG_EDGE_DATA);
        if (prev >= 0) rx_graph_order(G, (uint32_t)prev, (uint32_t)pub);
        rx_graph_need_resource(G, (uint32_t)pub, &t->step_need);
        prev = pub;
    }
    for (uint32_t i = 0; i < g->n_facts; i++) {
        uint32_t a = (uint32_t)slot_of(slots, n, g->x[i]);
        int rx = NODE(AG_WORLD_READ, AG_T_U64);
        G->nodes[rx].param = a + 1;
        G->nodes[rx].field = PLA_F_SUPPORT;
        t->slots[a].rights |= RX_RIGHT_READ;
        if (prev >= 0) rx_graph_order(G, (uint32_t)prev, (uint32_t)rx);
        int ver = NODE(AG_VERIFY, AG_T_VERDICT);
        if (g->floor[i]) {
            rx_graph_data(G, (uint32_t)rx, (uint32_t)ver, 0, AG_EDGE_DATA);
            G->nodes[ver].imm = 0;
            G->nodes[ver].imm2 = 0;
        } else {
            uint32_t b = (uint32_t)slot_of(slots, n, g->t[i]);
            int rt = NODE(AG_WORLD_READ, AG_T_U64);
            G->nodes[rt].param = b + 1;
            G->nodes[rt].field = PLA_F_SELF;
            t->slots[b].rights |= RX_RIGHT_READ;
            if (prev >= 0) rx_graph_order(G, (uint32_t)prev, (uint32_t)rt);
            int eq = NODE(AG_PURE, AG_T_U64);
            G->nodes[eq].op = OP_EQUAL;
            rx_graph_data(G, (uint32_t)rx, (uint32_t)eq, 0, AG_EDGE_DATA);
            rx_graph_data(G, (uint32_t)rt, (uint32_t)eq, 1, AG_EDGE_DATA);
            rx_graph_data(G, (uint32_t)eq, (uint32_t)ver, 0, AG_EDGE_DATA);
            G->nodes[ver].imm = 1;
            G->nodes[ver].imm2 = 1;
        }
        rx_graph_success(G, (uint32_t)ver, AG_OK);
    }
    #undef NODE
    t->n_evidence = p->n + g->n_facts;
    t->graph_kind = PL_GRAPH_RX_GRAPH;
    t->ancestry.origin = origin;
    t->ancestry.has_parent = parent != NULL;
    if (parent) memcpy(t->ancestry.parent, parent, 32);
    t->ancestry.goal_seq = g->seq;
    t->ancestry.cog_gen = cog_gen;
    rx_plan_identify(t);
    return 0;
}

int pla_template_moves(const PlanTemplate *t, const RxObjRef *slots, PlaPlan *out) {
    memset(out, 0, sizeof *out);
    const AgGraph *G = &t->graph;
    for (uint32_t n = 0; n < G->n_nodes; n++) {
        const AgNode *x = &G->nodes[n];
        if (!x->alive || x->kind != AG_WORLD_PUBLISH || !x->param) continue;
        int src = -1;
        for (uint32_t e = 0; e < G->n_data; e++)
            if (G->data[e].to == n) src = G->data[e].from;
        if (src < 0 || out->n >= PLA_MAX_MOVES) return -1;
        const AgNode *s = &G->nodes[src];
        out->mover[out->n] = slots[x->param - 1];
        if (s->kind == AG_CONST && s->imm == 0) out->to_floor[out->n] = 1;
        else if (s->kind == AG_WORLD_READ && s->param) out->dest[out->n] = slots[s->param - 1];
        else return -1;
        out->n++;
    }
    return 0;
}
