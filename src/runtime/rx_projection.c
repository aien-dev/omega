/*
 * rx_projection.c -- Omega cognitive state projection (host reference).
 * See rx_projection.h.
 */
#include "rx_projection.h"

#include "sha256.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static const char *const g_class_names[] = {
    "?", "PINNED", "LIVE", "DERIVED", "RECALLABLE", "COMPRESSIBLE",
    "REFERENCE_ONLY", "EPHEMERAL", "BRANCH_LOCAL", "PROTECTED_EVIDENCE"
};

const char *pj_state_class_name(uint32_t c) {
    return c < PJ_STATE_CLASS_END ? g_class_names[c] : "?";
}

void pj_need_init(CognitiveNeed *n, uint32_t operation, uint64_t subject, uint64_t t_now) {
    memset(n, 0, sizeof(*n));
    n->operation_class = operation;
    n->subject = subject;
    n->required_temporal_scope.anchor = PJ_A_FIXED;
    n->required_temporal_scope.t_now = t_now;
    n->uncertainty_requirement = PJ_U_NONE;
    n->evidence_requirement = PJ_E_NONE;
    n->memory_scope = PJ_M_WORKING;
}

int pj_need_add(CognitiveNeed *n, uint32_t cls, uint32_t kind, uint32_t treatment,
                uint32_t world, uint32_t temporal) {
    if (n->n_fact_types >= PJ_MAX_FACTS) return PJ_ERR_ARG;
    n->required_fact_types[n->n_fact_types++] = (PjFactReq){ cls, kind, treatment, world, temporal };
    return PJ_OK;
}

void pj_summarize(const uint64_t *w, uint32_t n, uint64_t out[PJ_SUMMARY_WORDS]) {
    uint64_t sum = 0, sq = 0, mn = UINT64_MAX, mx = 0;
    for (uint32_t i = 0; i < n; i++) {
        sum += w[i];
        sq += w[i] * w[i];
        if (w[i] < mn) mn = w[i];
        if (w[i] > mx) mx = w[i];
    }
    out[0] = n;
    out[1] = sum;
    out[2] = sq;
    out[3] = n ? mn : 0;
    out[4] = mx;
}

/* ---- a small id set ---- */

typedef struct {
    uint64_t *slot;
    uint32_t cap, n;
} IdSet;

static int set_has(const IdSet *s, uint64_t id) {
    if (!s->cap) return 0;
    for (uint32_t i = (uint32_t)(id * 0x9E3779B97F4A7C15ull >> 40) & (s->cap - 1);; i = (i + 1) & (s->cap - 1)) {
        if (s->slot[i] == id) return 1;
        if (s->slot[i] == 0) return 0;
    }
}

static int set_add(IdSet *s, uint64_t id) {
    if ((s->n + 1) * 2 > s->cap) {
        uint32_t c = s->cap ? s->cap * 2 : 256;
        uint64_t *q = calloc(c, sizeof(uint64_t));
        if (!q) return -1;
        for (uint32_t i = 0; i < s->cap; i++) {
            uint64_t v = s->slot[i];
            if (!v) continue;
            uint32_t j = (uint32_t)(v * 0x9E3779B97F4A7C15ull >> 40) & (c - 1);
            while (q[j]) j = (j + 1) & (c - 1);
            q[j] = v;
        }
        free(s->slot);
        s->slot = q;
        s->cap = c;
    }
    uint32_t i = (uint32_t)(id * 0x9E3779B97F4A7C15ull >> 40) & (s->cap - 1);
    while (s->slot[i]) {
        if (s->slot[i] == id) return 0;
        i = (i + 1) & (s->cap - 1);
    }
    s->slot[i] = id;
    s->n++;
    return 1;
}

static void set_free(IdSet *s) { free(s->slot); memset(s, 0, sizeof(*s)); }

/* ---- compile ---- */

typedef struct {
    CxStore *s;
    const CognitiveNeed *need;
    StateProjection *p;
    IdSet in;
    uint64_t w0, now;
    uint64_t subjects[64];
    uint32_t n_subjects;
    int err;
} Ctx;

static int add_subject(uint64_t *list, uint32_t *n, uint32_t cap, uint64_t s) {
    for (uint32_t i = 0; i < *n; i++)
        if (list[i] == s) return 0;
    if (*n >= cap) return -1;
    list[(*n)++] = s;
    return 1;
}

static int is_protected(const CxObject *o) { return o->protect != 0 || o->cls == CX_EVIDENCE; }

static uint32_t section_of(const CxObject *o, uint32_t treatment, uint32_t temporal) {
    if (treatment == PJ_LIVE && temporal == PJ_T_LATEST) return PJ_SEC_WORLD;
    if (is_protected(o)) return PJ_SEC_EVIDENCE;
    if (o->cls == CX_CLAIM) return PJ_SEC_HYPOTHESIS;
    return PJ_SEC_CORTEX;
}

static uint32_t form_of(uint32_t treatment) {
    switch (treatment) {
    case PJ_COMPRESSIBLE: return PJ_FORM_SUMMARY;
    case PJ_RECALLABLE:
    case PJ_REFERENCE_ONLY: return PJ_FORM_REF;
    default: return PJ_FORM_FULL;
    }
}

static int grow(void **p, uint64_t *cap, uint64_t need, size_t elem) {
    if (need <= *cap) return 0;
    uint64_t c = *cap ? *cap : 64;
    while (c < need) c *= 2;
    void *q = realloc(*p, c * elem);
    if (!q) return -1;
    *p = q;
    *cap = c;
    return 0;
}

static int add_entry(Ctx *c, const CxObject *o, uint32_t section, uint32_t treatment, uint32_t form) {
    StateProjection *p = c->p;
    if (set_has(&c->in, o->id)) return PJ_OK;
    if (is_protected(o) && (form == PJ_FORM_SUMMARY || treatment == PJ_EPHEMERAL))
        return PJ_ERR_PROTECTED;
    uint64_t cap = p->cap;
    if (grow((void **)&p->e, &cap, (uint64_t)p->n + 1, sizeof(PjEntry)) || set_add(&c->in, o->id) < 0)
        return PJ_ERR_NOMEM;
    p->cap = (uint32_t)cap;
    PjEntry *e = &p->e[p->n++];
    memset(e, 0, sizeof(*e));
    e->id = o->id;
    e->section = section;
    e->treatment = treatment;
    e->form = form;
    memcpy(e->digest, o->digest, 32);
    p->bytes += PJ_HEADER_BYTES;
    if (form == PJ_FORM_FULL) {
        p->bytes += (uint64_t)o->n * PJ_WORD_BYTES;
    } else if (form == PJ_FORM_SUMMARY) {
        if (grow((void **)&p->arena, &p->arena_cap, p->arena_n + PJ_SUMMARY_WORDS, sizeof(uint64_t)))
            return PJ_ERR_NOMEM;
        e->off = p->arena_n;
        e->n = PJ_SUMMARY_WORDS;
        pj_summarize(cx_payload(c->s, o), o->n, p->arena + p->arena_n);
        p->arena_n += PJ_SUMMARY_WORDS;
        p->bytes += PJ_SUMMARY_WORDS * PJ_WORD_BYTES;
    } else {
        p->bytes += PJ_DIGEST_BYTES;
    }
    if (form != PJ_FORM_FULL && treatment != PJ_EPHEMERAL) {
        uint64_t rcap = p->rcap;
        if (grow((void **)&p->r, &rcap, (uint64_t)p->nr + 1, sizeof(PjRecover))) return PJ_ERR_NOMEM;
        p->rcap = (uint32_t)rcap;
        PjRecover *r = &p->r[p->nr++];
        r->id = o->id;
        r->form = form;
        memcpy(r->digest, o->digest, 32);
    }
    add_subject(c->subjects, &c->n_subjects, 64, o->subject);
    return PJ_OK;
}

/* Linked evidence and protected state, as the need's evidence requirement says. */
static int follow_evidence(Ctx *c, const CxObject *o) {
    uint32_t req = c->need->evidence_requirement;
    if (req != PJ_E_REFS && req != PJ_E_FULL) return PJ_OK;
    for (uint32_t i = 0; i < CX_LINKS; i++) {
        const CxObject *l = cx_get(c->s, o->links[i]);
        if (!l || !is_protected(l)) continue;
        if (l->branch != 0 && l->branch != c->need->branch) continue;
        c->s->examined++;
        int rc = add_entry(c, l, PJ_SEC_EVIDENCE, PJ_PROTECTED_EVIDENCE,
                           req == PJ_E_FULL ? PJ_FORM_FULL : PJ_FORM_REF);
        if (rc) return rc;
    }
    return PJ_OK;
}

static int include(Ctx *c, const CxObject *o, const PjFactReq *f, uint32_t temporal) {
    if (is_protected(o) && (f->treatment == PJ_COMPRESSIBLE || f->treatment == PJ_DERIVED ||
                            f->treatment == PJ_EPHEMERAL))
        return PJ_ERR_PROTECTED;
    int rc = add_entry(c, o, section_of(o, f->treatment, temporal), f->treatment, form_of(f->treatment));
    if (rc) return rc;
    return follow_evidence(c, o);
}

typedef struct {
    Ctx *c;
    const PjFactReq *f;
    uint32_t temporal;
} Visit;

static int visit_include(void *user, const CxObject *o) {
    Visit *v = user;
    int rc = include(v->c, o, v->f, v->temporal);
    if (rc) {
        v->c->err = rc;
        return 1;
    }
    return 0;
}

int pj_feature_compute(CxStore *s, uint64_t subject, uint32_t cls, uint32_t kind, uint32_t branch,
                       uint64_t t0, uint64_t t1, PjFeature *out) {
    memset(out, 0, sizeof(*out));
    out->cls = cls;
    out->kind = kind;
    out->branch = branch;
    out->subject = subject;
    out->t0 = t0;
    out->t1 = t1;
    out->min = UINT64_MAX;
    if (subject >= s->n_subjects) return PJ_ERR_ARG;
    const CxIdList *l = &s->by_subject[subject];
    sha256_ctx h;
    sha256_init(&h);
    for (uint32_t i = 0; i < l->n; i++) {
        const CxObject *o = &s->obj[l->ids[i] - 1];
        if (o->t < t0) continue;
        if (o->t > t1) break;
        s->examined++;
        if (o->cls != cls || o->kind != kind || o->branch != branch) continue;
        if (is_protected(o)) return PJ_ERR_PROTECTED;
        const uint64_t *w = cx_payload(s, o);
        for (uint32_t k = 0; k < o->n; k++) {
            out->sum += w[k];
            out->sumsq += w[k] * w[k];
            if (w[k] < out->min) out->min = w[k];
            if (w[k] > out->max) out->max = w[k];
        }
        out->count += o->n;
        out->n_src++;
        sha256_update(&h, o->digest, 32);
    }
    if (!out->count) out->min = 0;
    sha256_final(&h, out->src_digest);
    return PJ_OK;
}

/* The subject's newest realization whose linked evidence verifies. */
static const CxObject *verified_anchor(Ctx *c) {
    const CognitiveNeed *n = c->need;
    const CxIdList *l = &c->s->by_subject[n->subject];
    for (uint32_t i = l->n; i > 0; i--) {
        const CxObject *o = &c->s->obj[l->ids[i - 1] - 1];
        c->s->examined++;
        if (o->t > c->now || o->branch != 0 || o->cls != CX_REALIZATION ||
            o->kind != n->required_temporal_scope.realization_kind)
            continue;
        const CxObject *ev = cx_get(c->s, o->links[0]);
        if (ev && (ev->protect & CX_PROT_VERIFY_EVIDENCE) && ev->tag == PJ_VERIFIED &&
            cx_verify(c->s, ev->id) == CX_OK)
            return o;
    }
    return NULL;
}

static int resolve_window(Ctx *c) {
    const CognitiveNeed *n = c->need;
    StateProjection *p = c->p;
    PjWindow *w = &p->w[p->nw++];
    memset(w, 0, sizeof(*w));
    w->subject = n->subject;
    w->anchor = n->required_temporal_scope.anchor;
    w->treatment = PJ_EPHEMERAL;
    w->t1 = c->now;
    if (w->anchor == PJ_A_FIXED) {
        c->w0 = n->required_temporal_scope.t0;
    } else if (w->anchor == PJ_A_SINCE_VERIFIED) {
        const CxObject *a = verified_anchor(c);
        if (!a) return PJ_ERR_NO_ANCHOR;
        c->w0 = a->t;
        w->anchor_id = a->id;
        int rc = add_entry(c, a, PJ_SEC_CORTEX, PJ_PINNED, PJ_FORM_FULL);
        if (rc) return rc;
        const CxObject *ev = cx_get(c->s, a->links[0]);
        rc = add_entry(c, ev, PJ_SEC_EVIDENCE, PJ_PROTECTED_EVIDENCE,
                       n->evidence_requirement == PJ_E_FULL ? PJ_FORM_FULL : PJ_FORM_REF);
        if (rc) return rc;
    } else if (w->anchor == PJ_A_GENERATION) {
        CxFilter f = { CX_ENTITY, n->required_temporal_scope.generation_kind, 0, 0 };
        uint64_t id = cx_latest(c->s, 0, c->now, &f);
        const CxObject *g = cx_get(c->s, id);
        if (!g) return PJ_ERR_NO_ANCHOR;
        c->w0 = g->t;
        w->anchor_id = g->id;
        int rc = add_entry(c, g, PJ_SEC_WORLD, PJ_PINNED, PJ_FORM_FULL);
        if (rc) return rc;
    } else {
        return PJ_ERR_ARG;
    }
    w->t0 = c->w0;
    return PJ_OK;
}

/* Subjects a fact type draws from. */
static uint32_t fact_subjects(Ctx *c, const PjFactReq *f, uint64_t *out, uint64_t *deps, uint32_t nd) {
    if (f->world == PJ_W_MACHINE) {
        out[0] = 0;
        return 1;
    }
    out[0] = c->need->subject;
    uint32_t n = 1;
    if (f->world == PJ_W_DEPENDENCIES)
        for (uint32_t i = 0; i < nd && n < 64; i++) out[n++] = deps[i];
    return n;
}

static int run_fact(Ctx *c, const PjFactReq *f, uint64_t *deps, uint32_t nd) {
    const CognitiveNeed *n = c->need;
    uint32_t temporal = f->temporal;
    if (temporal == PJ_T_MEMORY) temporal = n->memory_scope == PJ_M_EPISODIC ? PJ_T_ALL : PJ_T_WINDOW;
    uint32_t branch = 0;
    if (f->treatment == PJ_BRANCH_LOCAL) {
        if (n->branch == 0) return PJ_ERR_BRANCH;
        branch = n->branch;
    }
    CxFilter filt = { f->cls, f->kind, branch, 0 };
    uint64_t subj[64];
    uint32_t ns = fact_subjects(c, f, subj, deps, nd);
    for (uint32_t i = 0; i < ns; i++) {
        uint64_t t0 = temporal == PJ_T_ALL ? 0 : c->w0;
        if (f->treatment == PJ_DERIVED) {
            if (temporal != PJ_T_WINDOW && temporal != PJ_T_ALL) return PJ_ERR_ARG;
            StateProjection *p = c->p;
            if (p->nf >= sizeof p->f / sizeof p->f[0]) return PJ_ERR_ARG;
            PjFeature *ft = &p->f[p->nf];
            int rc = pj_feature_compute(c->s, subj[i], f->cls, f->kind, branch, t0, c->now, ft);
            if (rc) return rc;
            ft->section = n->uncertainty_requirement == PJ_U_INTERVAL ? PJ_FEAT_UNCERTAINTY : PJ_FEAT_DERIVED;
            p->nf++;
            p->bytes += PJ_FEATURE_BYTES;
            add_subject(c->subjects, &c->n_subjects, 64, subj[i]);
            continue;
        }
        Visit v = { c, f, temporal };
        if (temporal == PJ_T_LATEST) {
            const CxObject *o = cx_get(c->s, cx_latest(c->s, subj[i], c->now, &filt));
            if (o) {
                int rc = include(c, o, f, temporal);
                if (rc) return rc;
            }
            continue;
        }
        if (temporal == PJ_T_CARRY_IN && c->w0 > 0) {
            const CxObject *o = cx_get(c->s, cx_latest(c->s, subj[i], c->w0 - 1, &filt));
            if (o) {
                int rc = include(c, o, f, temporal);
                if (rc) return rc;
            }
        }
        cx_range(c->s, subj[i], t0, c->now, &filt, visit_include, &v);
        if (c->err) return c->err;
    }
    return PJ_OK;
}

static int needs_deps(const CognitiveNeed *n) {
    for (uint32_t i = 0; i < n->n_fact_types; i++)
        if (n->required_fact_types[i].world == PJ_W_DEPENDENCIES) return 1;
    return 0;
}

typedef struct {
    Ctx *c;
    uint64_t *deps;
    uint32_t nd;
} DepVisit;

static int visit_dep(void *user, const CxObject *o) {
    DepVisit *v = user;
    int rc = add_entry(v->c, o, PJ_SEC_CORTEX, PJ_REFERENCE_ONLY, PJ_FORM_REF);
    if (rc) {
        v->c->err = rc;
        return 1;
    }
    if (o->tag < v->c->s->n_subjects && o->tag != v->c->need->subject)
        add_subject(v->deps, &v->nd, 63, o->tag);
    return 0;
}

static int cmp_entry(const void *a, const void *b) {
    uint64_t x = ((const PjEntry *)a)->id, y = ((const PjEntry *)b)->id;
    return x < y ? -1 : x > y;
}

int pj_compile(CxStore *s, const CognitiveNeed *need, StateProjection *out) {
    memset(out, 0, sizeof(*out));
    if (!s || !need || need->subject >= s->n_subjects || need->n_fact_types > PJ_MAX_FACTS)
        return PJ_ERR_ARG;
    uint64_t t_start = now_ns(), ex0 = s->examined;
    Ctx c;
    memset(&c, 0, sizeof c);
    c.s = s;
    c.need = need;
    c.p = out;
    c.now = need->required_temporal_scope.t_now;
    out->store_count = s->n;
    memcpy(out->store_chain, s->chain, 32);

    int rc = resolve_window(&c);
    uint64_t deps[64];
    uint32_t nd = 0;
    if (!rc && needs_deps(need)) {
        CxFilter f = { CX_RELATIONSHIP, need->required_world_scope.dependency_kind, 0, 0 };
        DepVisit dv = { &c, deps, 0 };
        cx_range(s, need->subject, 0, c.now, &f, visit_dep, &dv);
        rc = c.err;
        nd = dv.nd;
    }
    if (!rc && need->focus) {
        const CxObject *fo = cx_get(s, need->focus);
        if (!fo) rc = PJ_ERR_ARG;
        else {
            rc = add_entry(&c, fo, section_of(fo, PJ_PINNED, 0), PJ_PINNED, PJ_FORM_FULL);
            if (!rc) rc = follow_evidence(&c, fo);
        }
    }
    for (uint32_t i = 0; !rc && i < need->n_fact_types; i++)
        rc = run_fact(&c, &need->required_fact_types[i], deps, nd);

    if (!rc) {
        out->objects = out->n + out->nf;
        const uint64_t mb = need->resource_budget.max_bytes, mo = need->resource_budget.max_objects;
        if ((mb && out->bytes > mb) || (mo && out->objects > mo)) rc = PJ_ERR_BUDGET;
    }
    if (!rc) {
        qsort(out->e, out->n, sizeof(PjEntry), cmp_entry);
        uint64_t in_scope = 0;
        for (uint32_t i = 0; i < c.n_subjects && out->nx < 71; i++) {
            uint64_t sj = c.subjects[i], mine = 0;
            for (uint32_t k = 0; k < out->n; k++)
                if (s->obj[out->e[k].id - 1].subject == sj) mine++;
            in_scope += cx_count_subject(s, sj);
            out->x[out->nx++] = (PjExclusion){ PJ_X_IN_SCOPE_SUBJECT, sj, cx_count_subject(s, sj) - mine };
        }
        out->x[out->nx++] = (PjExclusion){ PJ_X_OTHER_SUBJECTS, UINT64_MAX, s->n - in_scope };
    }
    set_free(&c.in);
    out->examined = s->examined - ex0;
    out->compile_ns = now_ns() - t_start;
    if (rc) {
        pj_free(out);
        return rc;
    }
    return PJ_OK;
}

void pj_free(StateProjection *p) {
    free(p->e);
    free(p->r);
    free(p->arena);
    p->e = NULL;
    p->r = NULL;
    p->arena = NULL;
    p->n = p->cap = p->nr = p->rcap = 0;
    p->arena_n = p->arena_cap = 0;
}

/* ---- views ---- */

static void item_from(PjItem *it, const CxStore *s, const CxObject *o) {
    it->id = o->id;
    it->cls = o->cls;
    it->kind = o->kind;
    it->subject = o->subject;
    it->t = o->t;
    it->generation = o->generation;
    it->tag = o->tag;
    memcpy(it->links, o->links, sizeof it->links);
    it->branch = o->branch;
    it->protect = o->protect;
    it->n = o->n;
    it->w = cx_payload(s, o);
}

int pj_view_projection(const CxStore *s, const StateProjection *p, PjView *v) {
    uint64_t t0 = now_ns();
    memset(v, 0, sizeof(*v));
    v->items = calloc(p->n ? p->n : 1, sizeof(PjItem));
    if (!v->items) return PJ_ERR_NOMEM;
    for (uint32_t i = 0; i < p->n; i++) {
        const PjEntry *e = &p->e[i];
        PjItem *it = &v->items[i];
        item_from(it, s, cx_get(s, e->id));
        it->section = e->section;
        it->treatment = e->treatment;
        it->form = e->form;
        if (e->form == PJ_FORM_SUMMARY) {
            it->n = e->n;
            it->w = p->arena + e->off;
        } else if (e->form == PJ_FORM_REF) {
            it->n = 0;
            it->w = NULL;
        }
    }
    v->n = p->n;
    v->feat = p->f;
    v->nf = p->nf;
    v->bytes = p->bytes;
    v->build_ns = now_ns() - t0;
    return PJ_OK;
}

uint64_t pj_full_bytes(const CxStore *s) {
    return s->n * (uint64_t)PJ_HEADER_BYTES + s->arena_n * (uint64_t)PJ_WORD_BYTES;
}

int pj_view_full(const CxStore *s, PjView *v) {
    uint64_t t0 = now_ns();
    memset(v, 0, sizeof(*v));
    v->items = calloc(s->n ? s->n : 1, sizeof(PjItem));
    if (!v->items) return PJ_ERR_NOMEM;
    for (uint64_t i = 0; i < s->n; i++) {
        item_from(&v->items[i], s, &s->obj[i]);
        v->items[i].section = PJ_SEC_CORTEX;
        v->items[i].treatment = PJ_PINNED;
        v->items[i].form = PJ_FORM_FULL;
    }
    v->n = (uint32_t)s->n;
    v->dense = 1;
    v->bytes = pj_full_bytes(s);
    v->build_ns = now_ns() - t0;
    return PJ_OK;
}

static int cmp_item(const void *a, const void *b) {
    uint64_t x = ((const PjItem *)a)->id, y = ((const PjItem *)b)->id;
    return x < y ? -1 : x > y;
}

int pj_view_ids(const CxStore *s, const uint64_t *ids, uint32_t n, PjView *v) {
    uint64_t t0 = now_ns();
    memset(v, 0, sizeof(*v));
    v->items = calloc(n ? n : 1, sizeof(PjItem));
    if (!v->items) return PJ_ERR_NOMEM;
    for (uint32_t i = 0; i < n; i++) {
        const CxObject *o = cx_get(s, ids[i]);
        if (!o) return PJ_ERR_ARG;
        item_from(&v->items[i], s, o);
        v->items[i].section = PJ_SEC_CORTEX;
        v->items[i].treatment = PJ_PINNED;
        v->items[i].form = PJ_FORM_FULL;
        v->bytes += PJ_HEADER_BYTES + (uint64_t)o->n * PJ_WORD_BYTES;
    }
    qsort(v->items, n, sizeof(PjItem), cmp_item);
    v->n = n;
    v->build_ns = now_ns() - t0;
    return PJ_OK;
}

void pj_view_free(PjView *v) {
    free(v->items);
    memset(v, 0, sizeof(*v));
}

const PjItem *pj_view_find(const PjView *v, uint64_t id) {
    if (v->dense) return id >= 1 && id <= v->n ? &v->items[id - 1] : NULL;
    uint32_t lo = 0, hi = v->n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (v->items[mid].id < id) lo = mid + 1;
        else hi = mid;
    }
    return lo < v->n && v->items[lo].id == id ? &v->items[lo] : NULL;
}

const uint64_t *pj_recall(const CxStore *s, const StateProjection *p, uint64_t id, uint32_t *n_out) {
    for (uint32_t i = 0; i < p->nr; i++) {
        if (p->r[i].id != id) continue;
        const CxObject *o = cx_get(s, id);
        if (!o || memcmp(o->digest, p->r[i].digest, 32) != 0 || cx_verify(s, id) != CX_OK) return NULL;
        if (n_out) *n_out = o->n;
        return cx_payload(s, o);
    }
    return NULL;
}

/* ---- recoverability ---- */

int pj_recover_all(CxStore *s, const StateProjection *p, uint64_t *checked) {
    uint64_t k = 0;
    int bad = 0;
    for (uint32_t i = 0; i < p->n; i++) {
        const CxObject *o = cx_get(s, p->e[i].id);
        k++;
        if (!o || memcmp(o->digest, p->e[i].digest, 32) != 0 || cx_verify(s, o->id) != CX_OK) bad++;
    }
    for (uint32_t i = 0; i < p->nr; i++) {
        const CxObject *o = cx_get(s, p->r[i].id);
        k++;
        if (!o || memcmp(o->digest, p->r[i].digest, 32) != 0 || cx_verify(s, o->id) != CX_OK) {
            bad++;
            continue;
        }
        if (p->r[i].form == PJ_FORM_SUMMARY) {
            /* The summary must be what the verified source summarizes to. */
            const PjEntry *e = NULL;
            for (uint32_t j = 0; j < p->n && !e; j++)
                if (p->e[j].id == o->id) e = &p->e[j];
            uint64_t sm[PJ_SUMMARY_WORDS];
            pj_summarize(cx_payload(s, o), o->n, sm);
            if (!e || memcmp(sm, p->arena + e->off, sizeof sm) != 0) bad++;
        }
    }
    for (uint32_t i = 0; i < p->nf; i++) {
        const PjFeature *f = &p->f[i];
        PjFeature g;
        k++;
        if (pj_feature_compute(s, f->subject, f->cls, f->kind, f->branch, f->t0, f->t1, &g) != PJ_OK ||
            g.n_src != f->n_src || g.count != f->count || g.sum != f->sum || g.sumsq != f->sumsq ||
            g.min != f->min || g.max != f->max || memcmp(g.src_digest, f->src_digest, 32) != 0)
            bad++;
        /* each source object must verify too */
        const CxIdList *l = &s->by_subject[f->subject];
        for (uint32_t j = 0; j < l->n; j++) {
            const CxObject *o = &s->obj[l->ids[j] - 1];
            if (o->t < f->t0 || o->t > f->t1 || o->cls != f->cls || o->kind != f->kind || o->branch != f->branch)
                continue;
            k++;
            if (cx_verify(s, o->id) != CX_OK) bad++;
        }
    }
    if (checked) *checked = k;
    return bad ? PJ_ERR_RECOVER : PJ_OK;
}

/* ---- audit ---- */

static int in_list(const uint64_t *l, uint32_t n, uint64_t v) {
    for (uint32_t i = 0; i < n; i++)
        if (l[i] == v) return 1;
    return 0;
}

int pj_audit(CxStore *s, const CognitiveNeed *need, const StateProjection *p, PjAudit *a) {
    memset(a, 0, sizeof(*a));
    IdSet in = { 0 };
    uint64_t scope[64];
    uint32_t nscope = 0;
    for (uint32_t i = 0; i < p->n; i++) {
        if (set_add(&in, p->e[i].id) < 0) return PJ_ERR_NOMEM;
        const CxObject *o = cx_get(s, p->e[i].id);
        add_subject(scope, &nscope, 64, o->subject);
        if (o->branch != 0 && o->branch != need->branch) a->other_branch_included++;
    }
    for (uint32_t i = 0; i < p->nf; i++) add_subject(scope, &nscope, 64, p->f[i].subject);
    const uint64_t w0 = p->nw ? p->w[0].t0 : 0, now = need->required_temporal_scope.t_now;
    uint64_t per_scope[64] = { 0 }, other = 0;

    for (uint64_t id = 1; id <= s->n; id++) {
        const CxObject *o = &s->obj[id - 1];
        if (set_has(&in, id)) {
            a->included++;
            a->by_why[PJ_WHY_INCLUDED]++;
            continue;
        }
        a->excluded++;
        if (cx_verify(s, id) == CX_OK) a->excluded_intact++;
        uint32_t why;
        if (!in_list(scope, nscope, o->subject)) {
            why = PJ_WHY_SUBJECT;
            other++;
        } else {
            for (uint32_t k = 0; k < nscope; k++)
                if (scope[k] == o->subject) per_scope[k]++;
            why = PJ_WHY_TYPE;
            for (uint32_t k = 0; k < need->n_fact_types; k++) {
                const PjFactReq *f = &need->required_fact_types[k];
                if (f->cls != o->cls || (f->kind && f->kind != o->kind)) continue;
                uint32_t br = f->treatment == PJ_BRANCH_LOCAL ? need->branch : 0;
                if (o->branch != br) {
                    if (why == PJ_WHY_TYPE) why = PJ_WHY_BRANCH;
                    continue;
                }
                uint32_t tp = f->temporal;
                if (tp == PJ_T_MEMORY) tp = need->memory_scope == PJ_M_EPISODIC ? PJ_T_ALL : PJ_T_WINDOW;
                uint64_t t0 = tp == PJ_T_ALL ? 0 : w0;
                if (f->treatment == PJ_DERIVED && o->t >= t0 && o->t <= now) {
                    why = PJ_WHY_DERIVED_SOURCE;
                    break;
                }
                if ((tp == PJ_T_LATEST || tp == PJ_T_CARRY_IN) && o->t <= now) {
                    why = PJ_WHY_SUPERSEDED;
                    continue;
                }
                if (why == PJ_WHY_TYPE || why == PJ_WHY_BRANCH) why = PJ_WHY_TIME;
            }
        }
        a->by_why[why]++;
    }
    /* The projection's own counts must agree with the object-by-object audit. */
    int match = 1;
    for (uint32_t i = 0; i < p->nx; i++) {
        const PjExclusion *x = &p->x[i];
        if (x->reason == PJ_X_OTHER_SUBJECTS) {
            if (x->count != other) match = 0;
        } else {
            uint64_t got = 0;
            for (uint32_t k = 0; k < nscope; k++)
                if (scope[k] == x->subject) got = per_scope[k];
            if (x->count != got) match = 0;
        }
    }
    a->counts_match = match;
    set_free(&in);
    return PJ_OK;
}
