/*
 * rx_cortex_record.c -- World execution recorded into canonical Cortex.
 * See rx_cortex_record.h.
 */
#include "rx_cortex_record.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    CxStore *s;
    uint64_t session;
    uint64_t token;          /* writer claim */
    uint64_t *cx_of_crumb;   /* [crumb id] -> Cortex id */
    uint64_t cap;            /* entries in cx_of_crumb (crumb_cap + 1) */
    uint64_t records, errors;
} CortexLink;

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

/* Called by the World with its mutex held. */
static void record(void *ctx, const RxWorld *w, const RxCrumb *k) {
    CortexLink *l = ctx;
    CxHeader h;
    memset(&h, 0, sizeof h);
    kind_of(k->kind, &h.cls, &h.kind, &h.protect);

    const RxObjRef *ref = k->n_outputs ? &k->outputs[0].obj
                        : k->n_inputs ? &k->inputs[0].obj : NULL;
    uint64_t version = k->n_outputs ? k->outputs[0].version
                     : k->n_inputs ? k->inputs[0].version : 0;
    h.subject = ref && ref->id < RX_MAX_OBJECTS ? (uint64_t)ref->id + 1u : 0;
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

static void release(void *ctx) {
    CortexLink *l = ctx;
    cx_release_writer(l->s, l->token);
    free(l->cx_of_crumb);
    free(l);
}

int rx_cortex_attach(RxWorld *w, CxStore *s, uint64_t session) {
    if (!w || !s || s->n_subjects < RX_CORTEX_SUBJECTS) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    int has = w->recorder != NULL;
    pthread_mutex_unlock(&w->mu);
    if (has) return RX_ERR_EXISTS;   /* rx_world_set_recorder re-checks under the lock */
    CortexLink *l = calloc(1, sizeof(*l));
    if (!l) return RX_ERR_FULL;
    l->s = s;
    l->session = session;
    l->token = (uint64_t)(uintptr_t)l;
    l->cap = w->crumb_cap + 1;
    l->cx_of_crumb = calloc(l->cap, sizeof(uint64_t));
    if (!l->cx_of_crumb) { free(l); return RX_ERR_FULL; }
    if (cx_claim_writer(s, l->token) != CX_OK) {
        free(l->cx_of_crumb);
        free(l);
        return RX_ERR_IDENTITY;
    }
    int rc = rx_world_set_recorder(w, record, release, l, true);
    if (rc != RX_OK) release(l);
    return rc;
}

int rx_cortex_detach(RxWorld *w) {
    if (!w) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    void *ctx = w->recorder == record ? w->recorder_ctx : NULL;
    pthread_mutex_unlock(&w->mu);
    if (!ctx || rx_world_clear_recorder(w, ctx) != RX_OK) return RX_ERR_NOT_FOUND;
    release(ctx);
    return RX_OK;
}

int rx_cortex_status(RxWorld *w, uint64_t *records, uint64_t *errors) {
    if (!w) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    const CortexLink *l = w->recorder == record ? w->recorder_ctx : NULL;
    if (l) {
        if (records) *records = l->records;
        if (errors) *errors = l->errors;
    }
    pthread_mutex_unlock(&w->mu);
    return l ? RX_OK : RX_ERR_NOT_FOUND;
}

uint64_t rx_cortex_record_of(RxWorld *w, uint64_t crumb) {
    if (!w) return 0;
    pthread_mutex_lock(&w->mu);
    const CortexLink *l = w->recorder == record ? w->recorder_ctx : NULL;
    uint64_t id = l ? cx_of(l, crumb) : 0;
    pthread_mutex_unlock(&w->mu);
    return id;
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
 * same single writer, under the world mutex so they interleave with crumb
 * records in one order. */
int rx_cortex_append(RxWorld *w, const CxHeader *h, const uint64_t *payload, uint32_t n,
                     uint64_t *out_id) {
    if (!w || !h) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    CortexLink *l = w->recorder == record ? w->recorder_ctx : NULL;
    int rc = !l ? RX_ERR_NOT_FOUND
           : cx_append_as(l->s, l->token, h, payload, n, out_id) == CX_OK ? RX_OK : RX_ERR_FULL;
    if (l && rc != RX_OK) l->errors++;
    pthread_mutex_unlock(&w->mu);
    return rc;
}

int rx_cortex_promote(RxWorld *w, uint64_t candidate, uint64_t evidence, uint64_t *out_id) {
    if (!w) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    CortexLink *l = w->recorder == record ? w->recorder_ctx : NULL;
    int rc = !l ? RX_ERR_NOT_FOUND
           : cx_promote(l->s, l->token, candidate, evidence, l->s->n + 1, out_id) == CX_OK
           ? RX_OK : RX_ERR_ARG;
    pthread_mutex_unlock(&w->mu);
    return rc;
}

uint64_t rx_cortex_next_t(RxWorld *w) {
    if (!w) return 0;
    pthread_mutex_lock(&w->mu);
    CortexLink *l = w->recorder == record ? w->recorder_ctx : NULL;
    uint64_t t = l ? l->s->n + 1 : 0;
    pthread_mutex_unlock(&w->mu);
    return t;
}
