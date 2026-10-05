/*
 * rx_cortex_record.c -- World execution recorded into canonical Cortex.
 * See rx_cortex_record.h.
 *
 * The World has one recorder slot. It holds a hub of Cortex links: at most
 * one whole-World link (rx_cortex_attach) and scoped links
 * (rx_cortex_attach_scoped) that record only the crumbs about their own
 * objects, under their own subjects. Each link is the single writer of its
 * own store, so no link ever writes into another's journal.
 */
#include "rx_cortex_record.h"

#include <stdlib.h>
#include <string.h>

#define RXCX_MAX_LINKS 8u

typedef struct {
    CxStore *s;
    uint64_t session;
    uint64_t token;          /* writer claim */
    uint64_t *cx_of_crumb;   /* [crumb id] -> Cortex id */
    uint64_t cap;            /* entries in cx_of_crumb (crumb_cap + 1) */
    uint64_t records, errors;
    int scoped;              /* 0: every crumb, subject = slot + 1 */
    uint32_t n_obj;          /* scoped: objects it records */
    RxObjRef obj[RXCX_SCOPE_MAX];
    uint64_t subject[RXCX_SCOPE_MAX];
} CortexLink;

typedef struct {
    uint32_t n;
    CortexLink *link[RXCX_MAX_LINKS];
} CortexHub;

static uint64_t cx_of(const CortexLink *l, uint64_t crumb) {
    return crumb && crumb < l->cap ? l->cx_of_crumb[crumb] : 0;
}

static void kind_of(RxCrumbKind k, uint32_t *cls, uint32_t *kind, uint32_t *protect) {
    *protect = 0;
    switch (k) {
    case RX_CRUMB_EXTERNAL:
        *cls = CX_OBSERVATION; *kind = CX_K_WORK_ACCEPTED; *protect = CX_PROT_AUTHORITY; break;
    case RX_CRUMB_CREATE:  *cls = CX_ENTITY; *kind = CX_K_ENTITY_CREATED; break;
    case RX_CRUMB_RETIRE:  *cls = CX_ENTITY; *kind = CX_K_ENTITY_RETIRED; break;
    case RX_CRUMB_COMMIT:
        *cls = CX_EXECUTION; *kind = CX_K_EXEC_COMMIT; *protect = CX_PROT_COMMIT_RECEIPT; break;
    case RX_CRUMB_NOOP:    *cls = CX_EXECUTION; *kind = CX_K_EXEC_NOOP; break;
    case RX_CRUMB_BLOCKED_AUTHORITY:
        *cls = CX_FAILURE; *kind = CX_K_EXEC_FAILED; *protect = CX_PROT_AUTHORITY; break;
    default:               *cls = CX_FAILURE; *kind = CX_K_EXEC_FAILED; break;
    }
}

/* Subject of `ref` in link `l`; 0 = this link does not record it. */
static uint64_t subject_in(const CortexLink *l, const RxObjRef *ref) {
    if (!l->scoped) return ref && ref->id < RX_MAX_OBJECTS ? (uint64_t)ref->id + 1u : 0;
    for (uint32_t i = 0; ref && i < l->n_obj; i++)
        if (l->obj[i].id == ref->id && l->obj[i].generation == ref->generation)
            return l->subject[i];
    return 0;
}

/* One crumb into one link. Called with the world mutex held. */
static void record_link(CortexLink *l, const RxWorld *w, const RxCrumb *k) {
    /* An operator stop or resume (R16 G6) is about no World object and has no
     * Cortex kind; its evidence is the crumb log and the durable halt mark. */
    if (k->kind == RX_CRUMB_OPERATOR_STOP || k->kind == RX_CRUMB_OPERATOR_RESUME) return;
    const RxObjRef *ref = k->n_outputs ? &k->outputs[0].obj
                        : k->n_inputs ? &k->inputs[0].obj : NULL;
    uint64_t subject = subject_in(l, ref);
    if (l->scoped && !subject) return;   /* not about this link's objects */
    CxHeader h;
    memset(&h, 0, sizeof h);
    kind_of(k->kind, &h.cls, &h.kind, &h.protect);
    uint64_t version = k->n_outputs ? k->outputs[0].version
                     : k->n_inputs ? k->inputs[0].version : 0;
    h.subject = subject;
    h.t = l->s->n + 1;
    h.generation = ref ? ref->generation : 0;
    h.tag = (uint64_t)k->kind;
    uint32_t nl = 0;
    uint64_t cause = cx_of(l, k->wake_cause);
    if (cause) h.links[nl++] = cause;
    for (uint32_t i = 0; i < k->n_parents && nl < CX_LINKS; i++) {
        uint64_t p = cx_of(l, k->parents[i]);
        int dup = p == 0;
        for (uint32_t j = 0; j < nl && !dup; j++) dup = h.links[j] == p;
        if (!dup) h.links[nl++] = p;
    }

    uint64_t p[CX_WREC_WORDS];
    memset(p, 0, sizeof p);
    p[CX_WREC_SESSION] = l->session;
    p[CX_WREC_CRUMB] = k->id;
    p[CX_WREC_CRUMB_KIND] = (uint64_t)k->kind;
    p[CX_WREC_REACTION] = k->reaction;
    p[CX_WREC_FACULTY] = k->faculty;
    p[CX_WREC_EPISODE] = k->episode;
    p[CX_WREC_REASON] = (uint64_t)(int64_t)k->reason;
    p[CX_WREC_OBJ] = ref ? ref->id : UINT64_MAX;
    p[CX_WREC_OBJ_GEN] = ref ? ref->generation : 0;
    p[CX_WREC_OBJ_VERSION] = version;
    if (ref && ref->id < RX_MAX_OBJECTS)
        for (uint32_t f = 0; f < RX_MAX_FIELDS && f < 8; f++)
            p[CX_WREC_FIELD0 + f] = w->objects[ref->id].field[f];
    for (int i = 0; i < 4; i++) {
        uint64_t v = 0;
        for (int b = 0; b < 8; b++) v |= (uint64_t)k->digest[8 * i + b] << (8 * b);
        p[CX_WREC_DIGEST0 + i] = v;
    }
    p[CX_WREC_N_INPUTS] = k->n_inputs;
    p[CX_WREC_N_OUTPUTS] = k->n_outputs;
    p[CX_WREC_T_START_NS] = k->t_start_ns;
    p[CX_WREC_T_END_NS] = k->t_end_ns;

    uint64_t id = 0;
    if (cx_append_as(l->s, l->token, &h, p, CX_WREC_WORDS, &id) != CX_OK) {
        l->errors++;
        return;
    }
    if (k->id < l->cap) l->cx_of_crumb[k->id] = id;
    l->records++;
}

/* The World's recorder: every crumb to every link. World mutex held. */
static void record(void *ctx, const RxWorld *w, const RxCrumb *k) {
    CortexHub *hub = ctx;
    for (uint32_t i = 0; i < hub->n; i++) record_link(hub->link[i], w, k);
}

static void link_free(CortexLink *l) {
    cx_release_writer(l->s, l->token);
    free(l->cx_of_crumb);
    free(l);
}

static void release(void *ctx) {
    CortexHub *hub = ctx;
    for (uint32_t i = 0; i < hub->n; i++) link_free(hub->link[i]);
    free(hub);
}

static CortexHub *hub_of(RxWorld *w) {
    return w->recorder == record ? w->recorder_ctx : NULL;
}

/* The link writing `s`, or (s == NULL) the default link: the whole-World
 * link, else the only link. World mutex held. */
static CortexLink *link_of(RxWorld *w, const CxStore *s) {
    CortexHub *hub = hub_of(w);
    if (!hub) return NULL;
    for (uint32_t i = 0; i < hub->n; i++)
        if (s ? hub->link[i]->s == s : !hub->link[i]->scoped) return hub->link[i];
    return !s && hub->n == 1 ? hub->link[0] : NULL;
}

static CortexLink *link_new(RxWorld *w, CxStore *s, uint64_t session, int *rc) {
    CortexLink *l = calloc(1, sizeof(*l));
    if (!l) { *rc = RX_ERR_FULL; return NULL; }
    l->s = s;
    l->session = session;
    l->token = (uint64_t)(uintptr_t)l;
    l->cap = w->crumb_cap + 1;
    l->cx_of_crumb = calloc(l->cap, sizeof(uint64_t));
    if (!l->cx_of_crumb) { free(l); *rc = RX_ERR_FULL; return NULL; }
    if (cx_claim_writer(s, l->token) != CX_OK) {
        free(l->cx_of_crumb);
        free(l);
        *rc = RX_ERR_IDENTITY;
        return NULL;
    }
    *rc = RX_OK;
    return l;
}

/* Add `l` to the World's hub (creating it if the World has no recorder) and
 * hand it every crumb already in the log, all under the world mutex. */
static int link_add(RxWorld *w, CortexLink *l) {
    pthread_mutex_lock(&w->mu);
    CortexHub *hub = hub_of(w);
    int rc = RX_OK;
    if (!hub && w->recorder) rc = RX_ERR_EXISTS;      /* some other recorder */
    else if (hub && hub->n >= RXCX_MAX_LINKS) rc = RX_ERR_FULL;
    else if (hub && !l->scoped && link_of(w, NULL) && !link_of(w, NULL)->scoped) rc = RX_ERR_EXISTS;
    for (uint32_t i = 0; rc == RX_OK && hub && i < hub->n; i++) {
        const CortexLink *o = hub->link[i];
        if (!o->scoped || !l->scoped) continue;
        for (uint32_t a = 0; a < o->n_obj; a++)            /* scopes are disjoint */
            for (uint32_t b = 0; b < l->n_obj; b++)
                if (o->obj[a].id == l->obj[b].id && o->obj[a].generation == l->obj[b].generation)
                    rc = RX_ERR_EXISTS;
    }
    if (rc == RX_OK && !hub) {
        hub = calloc(1, sizeof(*hub));
        if (!hub) rc = RX_ERR_FULL;
        else {
            w->recorder = record;
            w->recorder_release = release;
            w->recorder_ctx = hub;
        }
    }
    if (rc == RX_OK) {
        for (uint64_t i = 0; i < w->n_crumbs; i++) record_link(l, w, &w->crumbs[i]);
        hub->link[hub->n++] = l;
    }
    pthread_mutex_unlock(&w->mu);
    return rc;
}

int rx_cortex_attach(RxWorld *w, CxStore *s, uint64_t session) {
    if (!w || !s || s->n_subjects < RX_CORTEX_SUBJECTS) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    CortexLink *d = link_of(w, NULL);
    int has = (w->recorder && !hub_of(w)) || (d && !d->scoped);
    pthread_mutex_unlock(&w->mu);
    if (has) return RX_ERR_EXISTS;   /* link_add re-checks under the lock */
    int rc;
    CortexLink *l = link_new(w, s, session, &rc);
    if (!l) return rc;
    if ((rc = link_add(w, l)) != RX_OK) link_free(l);
    return rc;
}

int rx_cortex_attach_scoped(RxWorld *w, CxStore *s, uint64_t session, const RxObjRef *objs,
                            const uint64_t *subjects, uint32_t n) {
    if (!w || !s || !objs || !subjects || n == 0 || n > RXCX_SCOPE_MAX) return RX_ERR_ARG;
    for (uint32_t i = 0; i < n; i++)
        if (subjects[i] == 0 || subjects[i] >= s->n_subjects) return RX_ERR_ARG;
    int rc;
    CortexLink *l = link_new(w, s, session, &rc);
    if (!l) return rc;
    l->scoped = 1;
    l->n_obj = n;
    memcpy(l->obj, objs, n * sizeof *objs);
    memcpy(l->subject, subjects, n * sizeof *subjects);
    if ((rc = link_add(w, l)) != RX_OK) link_free(l);
    return rc;
}

/* Remove link `l` (found under the lock); the hub goes when it is empty. */
static int link_remove(RxWorld *w, const CxStore *s) {
    pthread_mutex_lock(&w->mu);
    CortexHub *hub = hub_of(w);
    CortexLink *l = link_of(w, s);
    int empty = 0;
    if (l) {
        uint32_t j = 0;
        for (uint32_t i = 0; i < hub->n; i++)
            if (hub->link[i] != l) hub->link[j++] = hub->link[i];
        hub->n = j;
        if (j == 0) {
            w->recorder = NULL;
            w->recorder_release = NULL;
            w->recorder_ctx = NULL;
            empty = 1;
        }
    }
    pthread_mutex_unlock(&w->mu);
    if (!l) return RX_ERR_NOT_FOUND;
    link_free(l);
    if (empty) free(hub);
    return RX_OK;
}

int rx_cortex_detach(RxWorld *w) {
    if (!w) return RX_ERR_ARG;
    return link_remove(w, NULL);
}

int rx_cortex_detach_store(RxWorld *w, const CxStore *s) {
    if (!w || !s) return RX_ERR_ARG;
    return link_remove(w, s);
}

int rx_cortex_status(RxWorld *w, uint64_t *records, uint64_t *errors) {
    if (!w) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    const CortexLink *l = link_of(w, NULL);
    if (l) {
        if (records) *records = l->records;
        if (errors) *errors = l->errors;
    }
    pthread_mutex_unlock(&w->mu);
    return l ? RX_OK : RX_ERR_NOT_FOUND;
}

uint64_t rx_cortex_record_in(RxWorld *w, const CxStore *s, uint64_t crumb) {
    if (!w) return 0;
    pthread_mutex_lock(&w->mu);
    const CortexLink *l = link_of(w, s);
    uint64_t id = l ? cx_of(l, crumb) : 0;
    pthread_mutex_unlock(&w->mu);
    return id;
}

uint64_t rx_cortex_record_of(RxWorld *w, uint64_t crumb) {
    return rx_cortex_record_in(w, NULL, crumb);
}

uint64_t rx_cortex_recall_result(CxStore *s, uint32_t obj_slot, CxWorldRecord *out) {
    if (!s || obj_slot >= RX_MAX_OBJECTS) return 0;
    CxFilter f = { CX_EXECUTION, CX_K_EXEC_COMMIT, 0, 1 };
    uint64_t id = cx_latest(s, (uint64_t)obj_slot + 1u, UINT64_MAX - 1, &f);
    if (!id || !out) return id;
    CxRecord r;
    if (cx_recall_id(s, id, &r) != CX_OK || cx_world_decode(&r, out) != CX_OK) return 0;
    return id;
}

/* COMPOSITION-2: records the composition layer adds about World work
 * (candidates, verification evidence, admissions, promotions) go through the
 * writer of their store, under the world mutex so they interleave with crumb
 * records in one order. */
int rx_cortex_append_in(RxWorld *w, CxStore *s, const CxHeader *h, const uint64_t *payload,
                        uint32_t n, uint64_t *out_id) {
    if (!w || !h) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    CortexLink *l = link_of(w, s);
    int rc = !l ? RX_ERR_NOT_FOUND
           : cx_append_as(l->s, l->token, h, payload, n, out_id) == CX_OK ? RX_OK : RX_ERR_FULL;
    if (l && rc != RX_OK) l->errors++;
    pthread_mutex_unlock(&w->mu);
    return rc;
}

int rx_cortex_append(RxWorld *w, const CxHeader *h, const uint64_t *payload, uint32_t n,
                     uint64_t *out_id) {
    return rx_cortex_append_in(w, NULL, h, payload, n, out_id);
}

int rx_cortex_promote_in(RxWorld *w, CxStore *s, uint64_t candidate, uint64_t evidence,
                         uint64_t *out_id) {
    if (!w) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    CortexLink *l = link_of(w, s);
    int rc = !l ? RX_ERR_NOT_FOUND
           : cx_promote(l->s, l->token, candidate, evidence, l->s->n + 1, out_id) == CX_OK
           ? RX_OK : RX_ERR_ARG;
    pthread_mutex_unlock(&w->mu);
    return rc;
}

int rx_cortex_promote(RxWorld *w, uint64_t candidate, uint64_t evidence, uint64_t *out_id) {
    return rx_cortex_promote_in(w, NULL, candidate, evidence, out_id);
}

uint64_t rx_cortex_next_t_in(RxWorld *w, const CxStore *s) {
    if (!w) return 0;
    pthread_mutex_lock(&w->mu);
    CortexLink *l = link_of(w, s);
    uint64_t t = l ? l->s->n + 1 : 0;
    pthread_mutex_unlock(&w->mu);
    return t;
}

uint64_t rx_cortex_next_t(RxWorld *w) { return rx_cortex_next_t_in(w, NULL); }
