/*
 * rx_semantic.c -- Omega semantic variables and incremental recomputation.
 * See rx_semantic.h for the model. Simple reference: one mutex, fixed tables.
 */
#include "rx_semantic.h"

#include "sha256.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NONE UINT32_MAX

/* ---- hashing --------------------------------------------------------------- */

static void h_u32(sha256_ctx *c, uint32_t v) {
    uint8_t b[4];
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (8 * i));
    sha256_update(c, b, 4);
}

static void h_u64(sha256_ctx *c, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
    sha256_update(c, b, 8);
}

static void h_dig(sha256_ctx *c, const RxSemDigest *d) { sha256_update(c, d->b, 32); }

static void h_tag(sha256_ctx *c, const char *tag) {
    sha256_update(c, (const uint8_t *)tag, strlen(tag) + 1);
}

bool rx_sem_digest_eq(const RxSemDigest *a, const RxSemDigest *b) {
    return memcmp(a->b, b->b, 32) == 0;
}

void rx_sem_digest_value(uint32_t type, const uint64_t *words, uint32_t n, RxSemDigest *out) {
    sha256_ctx c;
    sha256_init(&c);
    h_tag(&c, "omega.sem.value.v1");
    h_u32(&c, type);
    h_u32(&c, n);
    for (uint32_t i = 0; i < n; i++) h_u64(&c, words[i]);
    sha256_final(&c, out->b);
}

void rx_sem_digest_impl(const char *text, RxSemDigest *out) {
    sha256_hash((const uint8_t *)text, strlen(text), out->b);
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- lookup ---------------------------------------------------------------- */

static uint32_t source_index(const RxSemEngine *e, uint64_t sid) {
    for (uint32_t i = 0; i < e->n_sources; i++)
        if (e->sources[i].used && e->sources[i].semantic_id == sid) return i;
    return NONE;
}

static uint32_t node_index(const RxSemEngine *e, uint64_t sid) {
    for (uint32_t i = 0; i < e->n_nodes; i++)
        if (e->nodes[i].used && e->nodes[i].semantic_id == sid) return i;
    return NONE;
}

static RxSemBranch *branch_of(RxSemEngine *e, uint32_t b) {
    if (b >= RX_SEM_MAX_BRANCHES || !e->branches[b].live) return NULL;
    return &e->branches[b];
}

/* ---- lifecycle ------------------------------------------------------------- */

int rx_sem_init(RxSemEngine *e, uint64_t event_cap) {
    if (!e || event_cap == 0) return RX_SEM_ERR_ARG;
    memset(e, 0, sizeof *e);
    if (pthread_mutex_init(&e->mu, NULL) != 0) return RX_SEM_ERR_ARG;
    e->branches = calloc(RX_SEM_MAX_BRANCHES, sizeof *e->branches);
    e->cache = calloc(RX_SEM_CACHE_SLOTS, sizeof *e->cache);
    e->events = calloc(event_cap, sizeof *e->events);
    if (!e->branches || !e->cache || !e->events) {
        rx_sem_destroy(e);
        return RX_SEM_ERR_FULL;
    }
    e->event_cap = event_cap;
    e->grain = RX_SEM_GRAIN_FIELD;
    e->cache_enabled = true;
    return RX_SEM_OK;
}

void rx_sem_destroy(RxSemEngine *e) {
    if (!e) return;
    free(e->branches);
    free(e->cache);
    free(e->events);
    e->branches = NULL;
    e->cache = NULL;
    e->events = NULL;
    pthread_mutex_destroy(&e->mu);
}

void rx_sem_set_mode(RxSemEngine *e, uint32_t grain, bool cache_enabled, bool audit) {
    pthread_mutex_lock(&e->mu);
    e->grain = grain;
    e->cache_enabled = cache_enabled;
    e->audit = audit;
    pthread_mutex_unlock(&e->mu);
}

/* ---- declarations ---------------------------------------------------------- */

int rx_sem_add_fn(RxSemEngine *e, const RxSemFnDesc *d, uint32_t *out_fn) {
    if (!e || !d || !d->fn || !d->name) return RX_SEM_ERR_ARG;
    pthread_mutex_lock(&e->mu);
    if (e->n_fns >= RX_SEM_MAX_FNS) {
        pthread_mutex_unlock(&e->mu);
        return RX_SEM_ERR_FULL;
    }
    uint32_t id = e->n_fns++;
    e->fns[id] = *d;
    sha256_ctx c;
    sha256_init(&c);
    h_tag(&c, "omega.sem.derivation.v1");
    h_tag(&c, d->name);
    h_u32(&c, d->out_type);
    h_u32(&c, d->generation);
    h_dig(&c, &d->impl_digest);
    sha256_final(&c, e->fn_derivation[id].b);
    if (out_fn) *out_fn = id;
    pthread_mutex_unlock(&e->mu);
    return RX_SEM_OK;
}

int rx_sem_add_source(RxSemEngine *e, RxObjRef obj, uint32_t field, uint32_t type,
                      bool world_bound, uint64_t *out_sid) {
    if (!e || field >= RX_MAX_FIELDS || obj.id >= RX_MAX_OBJECTS) return RX_SEM_ERR_ARG;
    uint64_t sid = RX_SEM_SOURCE_ID(obj.id, field);
    pthread_mutex_lock(&e->mu);
    int rc = RX_SEM_OK;
    if (source_index(e, sid) != NONE) rc = RX_SEM_ERR_EXISTS;
    else if (e->n_sources >= RX_SEM_MAX_SOURCES) rc = RX_SEM_ERR_FULL;
    else {
        RxSemSource *s = &e->sources[e->n_sources++];
        memset(s, 0, sizeof *s);
        s->used = true;
        s->semantic_id = sid;
        s->type = type;
        s->world_bound = world_bound;
        s->obj = obj;
        s->field = field;
        if (out_sid) *out_sid = sid;
    }
    pthread_mutex_unlock(&e->mu);
    return rc;
}

static uint64_t node_id(const RxSemDigest *deriv, const uint64_t *inputs, uint32_t n) {
    sha256_ctx c;
    uint8_t d[32];
    sha256_init(&c);
    h_tag(&c, "omega.sem.node.v1");
    h_dig(&c, deriv);
    h_u32(&c, n);
    for (uint32_t i = 0; i < n; i++) h_u64(&c, inputs[i]);
    sha256_final(&c, d);
    uint64_t v = 0;
    for (int i = 0; i < 7; i++) v |= (uint64_t)d[i] << (8 * i);
    return ((uint64_t)RX_SEM_KIND_DERIVED << 56) | v;
}

int rx_sem_add_node(RxSemEngine *e, uint32_t fn, const uint64_t *inputs, uint32_t n,
                    uint64_t *out_sid) {
    if (!e || !inputs || n == 0 || n > RX_SEM_MAX_INPUTS) return RX_SEM_ERR_ARG;
    pthread_mutex_lock(&e->mu);
    int rc = RX_SEM_OK;
    if (fn >= e->n_fns) { rc = RX_SEM_ERR_ARG; goto out; }
    /* Inputs must already exist; that makes the graph acyclic by construction. */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = RX_SEM_KIND(inputs[i]);
        if ((k == RX_SEM_KIND_SOURCE && source_index(e, inputs[i]) == NONE) ||
            (k == RX_SEM_KIND_DERIVED && node_index(e, inputs[i]) == NONE) ||
            (k != RX_SEM_KIND_SOURCE && k != RX_SEM_KIND_DERIVED)) {
            rc = RX_SEM_ERR_NOT_FOUND;
            goto out;
        }
    }
    uint64_t sid = node_id(&e->fn_derivation[fn], inputs, n);
    uint32_t have = node_index(e, sid);
    if (have != NONE) {
        RxSemNode *h = &e->nodes[have];
        bool same = h->function_id == fn && h->n_inputs == n &&
                    memcmp(h->inputs, inputs, n * sizeof *inputs) == 0;
        if (!same) rc = RX_SEM_ERR_EXISTS;   /* 56-bit id collision: refuse */
        else if (out_sid) *out_sid = sid;    /* same question: same node */
        goto out;
    }
    if (e->n_nodes >= RX_SEM_MAX_NODES) { rc = RX_SEM_ERR_FULL; goto out; }
    uint32_t idx = e->n_nodes++;
    RxSemNode *nd = &e->nodes[idx];
    memset(nd, 0, sizeof *nd);
    nd->used = true;
    nd->semantic_id = sid;
    nd->function_id = fn;
    nd->n_inputs = n;
    memcpy(nd->inputs, inputs, n * sizeof *inputs);
    for (uint32_t i = 0; i < n; i++) {
        if (RX_SEM_KIND(inputs[i]) == RX_SEM_KIND_SOURCE) {
            RxSemSource *s = &e->sources[source_index(e, inputs[i])];
            s->readers[s->n_readers++] = idx;
        } else {
            RxSemNode *p = &e->nodes[node_index(e, inputs[i])];
            p->readers[p->n_readers++] = idx;
        }
    }
    if (out_sid) *out_sid = sid;
out:
    pthread_mutex_unlock(&e->mu);
    return rc;
}

/* ---- branches -------------------------------------------------------------- */

static int branch_alloc(RxSemEngine *e, uint32_t *out) {
    for (uint32_t i = 0; i < RX_SEM_MAX_BRANCHES; i++) {
        if (!e->branches[i].live) {
            *out = i;
            return RX_SEM_OK;
        }
    }
    return RX_SEM_ERR_FULL;
}

int rx_sem_branch_root(RxSemEngine *e, uint32_t agent, uint64_t task, uint32_t *out) {
    if (!e || !out) return RX_SEM_ERR_ARG;
    pthread_mutex_lock(&e->mu);
    uint32_t id;
    int rc = branch_alloc(e, &id);
    if (rc == RX_SEM_OK) {
        RxSemBranch *b = &e->branches[id];
        memset(b, 0, sizeof *b);
        b->live = true;
        b->id = id;
        b->parent = NONE;
        b->agent = agent;
        b->task = task;
        for (uint32_t i = 0; i < RX_SEM_MAX_NODES; i++) b->memo_entry[i] = NONE;
        *out = id;
    }
    pthread_mutex_unlock(&e->mu);
    return rc;
}

int rx_sem_branch_fork(RxSemEngine *e, uint32_t parent, uint32_t agent, uint64_t task,
                       uint64_t candidate, uint32_t *out) {
    if (!e || !out) return RX_SEM_ERR_ARG;
    pthread_mutex_lock(&e->mu);
    RxSemBranch *p = branch_of(e, parent);
    uint32_t id;
    int rc = p ? branch_alloc(e, &id) : RX_SEM_ERR_NOT_FOUND;
    if (rc == RX_SEM_OK) {
        RxSemBranch *b = &e->branches[id];
        memcpy(b, p, sizeof *b);
        b->id = id;
        b->parent = parent;
        b->agent = agent;
        b->task = task;
        b->candidate = candidate;
        b->invalidations = 0;
        *out = id;
    }
    pthread_mutex_unlock(&e->mu);
    return rc;
}

int rx_sem_branch_drop(RxSemEngine *e, uint32_t branch) {
    pthread_mutex_lock(&e->mu);
    RxSemBranch *b = branch_of(e, branch);
    if (b) b->live = false;
    pthread_mutex_unlock(&e->mu);
    return b ? RX_SEM_OK : RX_SEM_ERR_NOT_FOUND;
}

int rx_sem_branch_context(RxSemEngine *e, uint32_t branch, uint32_t agent, uint64_t task,
                          uint64_t candidate) {
    pthread_mutex_lock(&e->mu);
    RxSemBranch *b = branch_of(e, branch);
    if (b) {
        b->agent = agent;
        b->task = task;
        b->candidate = candidate;
    }
    pthread_mutex_unlock(&e->mu);
    return b ? RX_SEM_OK : RX_SEM_ERR_NOT_FOUND;
}

/* ---- invalidation ---------------------------------------------------------- */

/* Mark node `idx` and everything that reads it DIRTY in branch b. */
static void dirty_from(RxSemEngine *e, RxSemBranch *b, uint32_t idx, uint8_t *seen) {
    if (seen[idx]) return;
    seen[idx] = 1;
    if (b->state[idx] == RX_SEM_CLEAN) {
        b->state[idx] = RX_SEM_DIRTY;
        b->invalidations++;
        e->stats.invalidations++;
    }
    const RxSemNode *n = &e->nodes[idx];
    for (uint32_t i = 0; i < n->n_readers; i++) dirty_from(e, b, n->readers[i], seen);
}

static void dirty_source(RxSemEngine *e, RxSemBranch *b, uint32_t si) {
    uint8_t seen[RX_SEM_MAX_NODES];
    memset(seen, 0, sizeof seen);
    if (e->grain == RX_SEM_GRAIN_OBJECT) {
        /* Baseline: any field of the object dirties readers of every field. */
        uint32_t obj = e->sources[si].obj.id;
        for (uint32_t j = 0; j < e->n_sources; j++) {
            if (!e->sources[j].used || e->sources[j].obj.id != obj) continue;
            for (uint32_t r = 0; r < e->sources[j].n_readers; r++)
                dirty_from(e, b, e->sources[j].readers[r], seen);
        }
        return;
    }
    const RxSemSource *s = &e->sources[si];
    for (uint32_t r = 0; r < s->n_readers; r++) dirty_from(e, b, s->readers[r], seen);
}

static int set_source_locked(RxSemEngine *e, RxSemBranch *b, uint32_t si, uint32_t generation,
                             const uint64_t *words, uint32_t n, uint32_t confidence,
                             const RxSemDigest *evidence) {
    const RxSemSource *s = &e->sources[si];
    RxSemValue v;
    memset(&v, 0, sizeof v);
    v.semantic_id = s->semantic_id;
    v.type = s->type;
    v.generation = generation;
    v.confidence = confidence;
    v.n_words = n;
    memcpy(v.value, words, n * sizeof *words);
    rx_sem_digest_value(v.type, v.value, n, &v.value_digest);
    if (evidence) {
        v.n_evidence = 1;
        v.evidence_refs[0] = *evidence;
    }
    e->stats.source_writes++;
    RxSemValue *cur = &b->source[si];
    bool had = b->have_source[si];
    bool was_stale = b->source_stale[si];
    b->source_stale[si] = false;
    if (had && !was_stale && cur->generation == generation &&
        rx_sem_digest_eq(&cur->value_digest, &v.value_digest) && cur->confidence == confidence) {
        /* Same meaning. Keep the newest evidence; wake nobody. */
        if (evidence) {
            cur->n_evidence = 1;
            cur->evidence_refs[0] = *evidence;
        }
        e->stats.source_writes_unchanged++;
        return 0;
    }
    v.value_version = ++b->version_clock;
    *cur = v;
    b->have_source[si] = true;
    dirty_source(e, b, si);
    return 1;
}

int rx_sem_set_source(RxSemEngine *e, uint32_t branch, uint64_t sid, uint32_t generation,
                      const uint64_t *words, uint32_t n, uint32_t confidence,
                      const RxSemDigest *evidence) {
    if (!e || !words || n > RX_SEM_VALUE_WORDS) return RX_SEM_ERR_ARG;
    pthread_mutex_lock(&e->mu);
    RxSemBranch *b = branch_of(e, branch);
    uint32_t si = source_index(e, sid);
    int rc = !b ? RX_SEM_ERR_NOT_FOUND : si == NONE ? RX_SEM_ERR_NOT_FOUND
           : set_source_locked(e, b, si, generation, words, n, confidence, evidence);
    pthread_mutex_unlock(&e->mu);
    return rc < 0 ? rc : RX_SEM_OK;
}

int rx_sem_sync_world(RxSemEngine *e, uint32_t branch, RxWorld *w) {
    if (!e || !w) return RX_SEM_ERR_ARG;
    pthread_mutex_lock(&e->mu);
    RxSemBranch *b = branch_of(e, branch);
    if (!b) {
        pthread_mutex_unlock(&e->mu);
        return RX_SEM_ERR_NOT_FOUND;
    }
    int changed = 0;
    for (uint32_t si = 0; si < e->n_sources; si++) {
        RxSemSource *s = &e->sources[si];
        if (!s->used || !s->world_bound) continue;
        RxObject o;
        if (rx_world_read(w, s->obj, &o) != RX_OK) {
            /* Retired: nothing derived from the old object may be served. */
            if (!b->source_stale[si]) {
                b->source_stale[si] = true;
                dirty_source(e, b, si);
                changed++;
            }
            continue;
        }
        RxSemValue *cur = &b->source[si];
        if (b->have_source[si] && !b->source_stale[si] && cur->generation == o.generation &&
            b->world_version[si] == o.field_version[s->field])
            continue;   /* world version unchanged: not even read further */
        b->world_version[si] = o.field_version[s->field];
        /* Evidence: the causal crumb that last wrote this field. */
        RxSemDigest ev;
        memset(&ev, 0, sizeof ev);
        const RxCrumb *cr = rx_world_crumb(w, o.field_writer[s->field]);
        if (cr) memcpy(ev.b, cr->digest, 32);
        uint64_t word = o.field[s->field];
        int r = set_source_locked(e, b, si, o.generation, &word, 1, RX_SEM_CONFIDENCE_ONE, &ev);
        if (r > 0) changed++;
    }
    pthread_mutex_unlock(&e->mu);
    return changed;
}

int rx_sem_rebind(RxSemEngine *e, uint64_t sid, RxObjRef obj) {
    pthread_mutex_lock(&e->mu);
    uint32_t si = source_index(e, sid);
    if (si != NONE) e->sources[si].obj = obj;
    pthread_mutex_unlock(&e->mu);
    return si == NONE ? RX_SEM_ERR_NOT_FOUND : RX_SEM_OK;
}

/* ---- cache ----------------------------------------------------------------- */

static void compute_key(const RxSemDigest *deriv, const RxSemInputVersion *in, uint32_t n,
                        RxSemDigest *out) {
    sha256_ctx c;
    sha256_init(&c);
    h_tag(&c, "omega.sem.key.v1");
    h_dig(&c, deriv);
    h_u32(&c, n);
    for (uint32_t i = 0; i < n; i++) {
        h_u64(&c, in[i].semantic_id);
        h_u32(&c, in[i].generation);
        h_dig(&c, &in[i].value_digest);
    }
    sha256_final(&c, out->b);
}

static void digest_result(sha256_ctx *c, const RxSemValue *v) {
    h_u64(c, v->semantic_id);
    h_u32(c, v->type);
    h_u32(c, v->generation);
    h_dig(c, &v->derivation_id);
    h_u32(c, v->confidence);
    h_u32(c, v->n_deps);
    for (uint32_t i = 0; i < v->n_deps; i++) h_u64(c, v->dependencies[i]);
    h_u32(c, v->n_words);
    for (uint32_t i = 0; i < v->n_words; i++) h_u64(c, v->value[i]);
    h_dig(c, &v->value_digest);
}

static void entry_digest(const RxSemDerived *d, RxSemDigest *out) {
    sha256_ctx c;
    sha256_init(&c);
    h_tag(&c, "omega.sem.entry.v1");
    h_u32(&c, d->function_id);
    h_dig(&c, &d->derivation_id);
    h_dig(&c, &d->key);
    h_u32(&c, d->n_inputs);
    for (uint32_t i = 0; i < d->n_inputs; i++) {
        h_u64(&c, d->input_versions[i].semantic_id);
        h_u32(&c, d->input_versions[i].generation);
        h_dig(&c, &d->input_versions[i].value_digest);
        h_u64(&c, d->input_versions[i].value_version);
        h_dig(&c, &d->input_versions[i].evidence);
    }
    digest_result(&c, &d->result);
    h_u32(&c, d->producer_branch);
    h_u32(&c, d->producer_agent);
    h_u64(&c, d->producer_task);
    h_u64(&c, d->producer_candidate);
    h_u64(&c, d->compute_ns);
    h_u64(&c, d->seq);
    sha256_final(&c, out->b);
}

static bool same_inputs(const RxSemDerived *d, const RxSemDigest *deriv,
                        const RxSemInputVersion *in, uint32_t n) {
    if (!rx_sem_digest_eq(&d->derivation_id, deriv) || d->n_inputs != n) return false;
    for (uint32_t i = 0; i < n; i++) {
        if (d->input_versions[i].semantic_id != in[i].semantic_id ||
            d->input_versions[i].generation != in[i].generation ||
            !rx_sem_digest_eq(&d->input_versions[i].value_digest, &in[i].value_digest))
            return false;
    }
    return true;
}

static uint32_t slot_of(const RxSemDigest *key) {
    uint32_t h = (uint32_t)key->b[0] | (uint32_t)key->b[1] << 8 | (uint32_t)key->b[2] << 16 |
                 (uint32_t)key->b[3] << 24;
    return h & (RX_SEM_CACHE_SLOTS - 1u);
}

/* Returns the slot holding `key`, or NONE; *free_slot gets the first empty. */
static uint32_t cache_find(const RxSemEngine *e, const RxSemDigest *key, const RxSemDigest *deriv,
                           const RxSemInputVersion *in, uint32_t n, uint32_t *free_slot) {
    uint32_t s = slot_of(key);
    *free_slot = NONE;
    for (uint32_t probe = 0; probe < RX_SEM_CACHE_SLOTS; probe++) {
        const RxSemDerived *d = &e->cache[(s + probe) & (RX_SEM_CACHE_SLOTS - 1u)];
        if (!d->used) {
            *free_slot = (s + probe) & (RX_SEM_CACHE_SLOTS - 1u);
            return NONE;
        }
        /* The key is a digest; the full input record is compared as well. */
        if (rx_sem_digest_eq(&d->key, key) && same_inputs(d, deriv, in, n))
            return (s + probe) & (RX_SEM_CACHE_SLOTS - 1u);
    }
    return NONE;
}

static void event_append(RxSemEngine *e, uint32_t kind, uint32_t slot, const RxSemBranch *b,
                         const RxSemDigest *inputs_evidence) {
    if (e->n_events >= e->event_cap) {
        e->event_overflow++;
        return;
    }
    RxSemEvent *ev = &e->events[e->n_events];
    memset(ev, 0, sizeof *ev);
    ev->id = e->n_events + 1;
    ev->kind = kind;
    ev->entry = slot;
    ev->entry_digest = e->cache[slot].entry_digest;
    ev->branch = b->id;
    ev->agent = b->agent;
    ev->task = b->task;
    ev->candidate = b->candidate;
    ev->inputs_evidence = *inputs_evidence;
    if (e->n_events > 0) ev->prev = e->events[e->n_events - 1].digest;
    sha256_ctx c;
    sha256_init(&c);
    h_tag(&c, "omega.sem.event.v1");
    h_dig(&c, &ev->prev);
    h_u64(&c, ev->id);
    h_u32(&c, ev->kind);
    h_u32(&c, ev->entry);
    h_dig(&c, &ev->entry_digest);
    h_u32(&c, ev->branch);
    h_u32(&c, ev->agent);
    h_u64(&c, ev->task);
    h_u64(&c, ev->candidate);
    h_dig(&c, &ev->inputs_evidence);
    sha256_final(&c, ev->digest.b);
    e->n_events++;
}

static bool event_ok(const RxSemEvent *ev, const RxSemDigest *prev) {
    if (!rx_sem_digest_eq(&ev->prev, prev)) return false;
    RxSemDigest d;
    sha256_ctx c;
    sha256_init(&c);
    h_tag(&c, "omega.sem.event.v1");
    h_dig(&c, &ev->prev);
    h_u64(&c, ev->id);
    h_u32(&c, ev->kind);
    h_u32(&c, ev->entry);
    h_dig(&c, &ev->entry_digest);
    h_u32(&c, ev->branch);
    h_u32(&c, ev->agent);
    h_u64(&c, ev->task);
    h_u64(&c, ev->candidate);
    h_dig(&c, &ev->inputs_evidence);
    sha256_final(&c, d.b);
    return rx_sem_digest_eq(&d, &ev->digest);
}

/* ---- evaluation ------------------------------------------------------------ */

static int run_fn(RxSemEngine *e, const RxSemNode *nd, const RxSemValue *const *in,
                  RxSemValue *out, uint64_t *ns) {
    const RxSemFnDesc *f = &e->fns[nd->function_id];
    memset(out, 0, sizeof *out);
    uint32_t conf = RX_SEM_CONFIDENCE_ONE;
    for (uint32_t i = 0; i < nd->n_inputs; i++)
        if (in[i]->confidence < conf) conf = in[i]->confidence;
    out->confidence = conf;
    uint64_t t0 = now_ns();
    int rc = f->fn(in, nd->n_inputs, out, f->user);
    uint64_t t1 = now_ns();
    if (ns) *ns = t1 - t0;
    if (rc != 0 || out->n_words > RX_SEM_VALUE_WORDS) return RX_SEM_ERR_FN;
    if (out->confidence > RX_SEM_CONFIDENCE_ONE) out->confidence = RX_SEM_CONFIDENCE_ONE;
    out->semantic_id = nd->semantic_id;
    out->type = f->out_type;
    out->generation = f->generation;
    out->derivation_id = e->fn_derivation[nd->function_id];
    out->n_deps = nd->n_inputs;
    memcpy(out->dependencies, nd->inputs, nd->n_inputs * sizeof nd->inputs[0]);
    out->n_evidence = 0;
    out->value_version = 0;
    rx_sem_digest_value(out->type, out->value, out->n_words, &out->value_digest);
    return RX_SEM_OK;
}

static bool same_content(const RxSemValue *a, const RxSemValue *b) {
    return a->type == b->type && a->n_words == b->n_words && a->confidence == b->confidence &&
           memcmp(a->value, b->value, a->n_words * sizeof a->value[0]) == 0;
}

static int get_locked(RxSemEngine *e, RxSemBranch *b, uint32_t idx, RxSemServed *served);

/* Collect the current inputs of node idx in branch b (bringing nodes up to date). */
static int gather(RxSemEngine *e, RxSemBranch *b, const RxSemNode *nd,
                  const RxSemValue **in, RxSemInputVersion *iv, RxSemDigest *inputs_evidence) {
    sha256_ctx c;
    sha256_init(&c);
    h_tag(&c, "omega.sem.inputs-evidence.v1");
    for (uint32_t i = 0; i < nd->n_inputs; i++) {
        uint64_t sid = nd->inputs[i];
        const RxSemValue *v;
        if (RX_SEM_KIND(sid) == RX_SEM_KIND_SOURCE) {
            uint32_t si = source_index(e, sid);
            if (!b->have_source[si]) return RX_SEM_ERR_NOT_FOUND;
            if (b->source_stale[si]) return RX_SEM_ERR_STALE;
            v = &b->source[si];
            iv[i].evidence = v->n_evidence ? v->evidence_refs[0] : (RxSemDigest){ { 0 } };
        } else {
            uint32_t ci = node_index(e, sid);
            int rc = get_locked(e, b, ci, NULL);
            if (rc != RX_SEM_OK) return rc;
            v = &b->memo[ci];
            iv[i].evidence = b->memo_entry[ci] != NONE ? e->cache[b->memo_entry[ci]].entry_digest
                                                        : (RxSemDigest){ { 0 } };
        }
        in[i] = v;
        iv[i].semantic_id = sid;
        iv[i].generation = v->generation;
        iv[i].value_digest = v->value_digest;
        iv[i].value_version = v->value_version;
        h_dig(&c, &iv[i].evidence);
    }
    sha256_final(&c, inputs_evidence->b);
    return RX_SEM_OK;
}

static void install_memo(RxSemBranch *b, uint32_t idx, const RxSemValue *v, const RxSemDigest *key,
                         uint32_t slot, const RxSemDigest *entry_dig,
                         const RxSemDigest *inputs_evidence) {
    bool changed = b->state[idx] == RX_SEM_ABSENT ||
                   !rx_sem_digest_eq(&b->memo[idx].value_digest, &v->value_digest);
    uint64_t ver = changed ? ++b->version_clock : b->memo[idx].value_version;
    b->memo[idx] = *v;
    b->memo[idx].value_version = ver;
    b->memo[idx].n_evidence = 0;
    if (entry_dig) b->memo[idx].evidence_refs[b->memo[idx].n_evidence++] = *entry_dig;
    b->memo[idx].evidence_refs[b->memo[idx].n_evidence++] = *inputs_evidence;
    b->memo_key[idx] = *key;
    b->memo_entry[idx] = slot;
    b->state[idx] = RX_SEM_CLEAN;
}

/* In audit mode, recompute a served value and compare. */
static void audit_hit(RxSemEngine *e, const RxSemNode *nd, const RxSemValue *const *in,
                      const RxSemValue *served) {
    if (!e->audit) return;
    RxSemValue fresh;
    e->stats.audits++;
    if (run_fn(e, nd, in, &fresh, NULL) != RX_SEM_OK || !same_content(&fresh, served))
        e->stats.false_hits++;
}

static int get_locked(RxSemEngine *e, RxSemBranch *b, uint32_t idx, RxSemServed *served) {
    const RxSemNode *nd = &e->nodes[idx];
    e->stats.gets++;
    const RxSemValue *in[RX_SEM_MAX_INPUTS];
    RxSemInputVersion iv[RX_SEM_MAX_INPUTS];
    RxSemDigest ievid;

    if (b->state[idx] == RX_SEM_CLEAN) {
        e->stats.memo_hits++;
        if (b->memo_entry[idx] != NONE) e->stats.saved_ns += e->cache[b->memo_entry[idx]].compute_ns;
        if (served) *served = RX_SEM_SERVED_MEMO;
        if (e->audit) {
            /* Clean inputs are already current: gathering does no work. The
             * audit's own reads are not counted as service. */
            RxSemStats keep = e->stats;
            int grc = gather(e, b, nd, in, iv, &ievid);
            uint64_t audits = e->stats.audits, false_hits = e->stats.false_hits;
            e->stats = keep;
            e->stats.audits = audits;
            e->stats.false_hits = false_hits;
            if (grc == RX_SEM_OK) audit_hit(e, nd, in, &b->memo[idx]);
            else e->stats.false_hits++;
        }
        return RX_SEM_OK;
    }

    int rc = gather(e, b, nd, in, iv, &ievid);
    if (rc != RX_SEM_OK) return rc;
    const RxSemDigest *deriv = &e->fn_derivation[nd->function_id];
    RxSemDigest key;
    compute_key(deriv, iv, nd->n_inputs, &key);

    if (b->state[idx] == RX_SEM_DIRTY && rx_sem_digest_eq(&b->memo_key[idx], &key)) {
        /* Marked, but every input came back with the same content. */
        b->state[idx] = RX_SEM_CLEAN;
        e->stats.cutoffs++;
        if (b->memo_entry[idx] != NONE) e->stats.saved_ns += e->cache[b->memo_entry[idx]].compute_ns;
        if (served) *served = RX_SEM_SERVED_CUTOFF;
        audit_hit(e, nd, in, &b->memo[idx]);
        return RX_SEM_OK;
    }

    uint32_t free_slot = NONE;
    uint32_t slot = cache_find(e, &key, deriv, iv, nd->n_inputs, &free_slot);
    if (slot != NONE && e->cache_enabled) {
        RxSemDerived *d = &e->cache[slot];
        d->reuses++;
        e->stats.cache_hits++;
        e->stats.saved_ns += d->compute_ns;
        if (d->producer_branch != b->id) e->stats.cache_hits_cross_branch++;
        if (d->producer_agent != b->agent) e->stats.cache_hits_cross_agent++;
        if (d->producer_task != b->task) e->stats.cache_hits_cross_task++;
        if (d->producer_candidate != b->candidate) e->stats.cache_hits_cross_candidate++;
        install_memo(b, idx, &d->result, &key, slot, &d->entry_digest, &ievid);
        event_append(e, RX_SEM_EVENT_REUSE, slot, b, &ievid);
        if (served) *served = RX_SEM_SERVED_CACHE;
        audit_hit(e, nd, in, &d->result);
        return RX_SEM_OK;
    }

    RxSemValue out;
    uint64_t ns = 0;
    rc = run_fn(e, nd, in, &out, &ns);
    if (rc != RX_SEM_OK) return rc;
    e->stats.computes++;
    e->stats.compute_ns += ns;
    if (served) *served = RX_SEM_SERVED_COMPUTED;

    if (slot == NONE && free_slot != NONE &&
        e->cache_used < RX_SEM_CACHE_SLOTS - RX_SEM_CACHE_SLOTS / 8u) {
        RxSemDerived *d = &e->cache[free_slot];
        memset(d, 0, sizeof *d);
        d->used = true;
        d->function_id = nd->function_id;
        d->derivation_id = *deriv;
        d->key = key;
        d->n_inputs = nd->n_inputs;
        memcpy(d->input_versions, iv, nd->n_inputs * sizeof iv[0]);
        d->result = out;
        d->producer_branch = b->id;
        d->producer_agent = b->agent;
        d->producer_task = b->task;
        d->producer_candidate = b->candidate;
        d->compute_ns = ns;
        d->seq = ++e->cache_seq;
        entry_digest(d, &d->entry_digest);
        e->cache_used++;
        install_memo(b, idx, &out, &key, free_slot, &d->entry_digest, &ievid);
        event_append(e, RX_SEM_EVENT_PRODUCE, free_slot, b, &ievid);
    } else if (slot != NONE) {
        /* Cache lookups are off (baseline) and this key is already filed. */
        install_memo(b, idx, &out, &key, slot, &e->cache[slot].entry_digest, &ievid);
    } else {
        e->stats.cache_full++;
        install_memo(b, idx, &out, &key, NONE, NULL, &ievid);
    }
    return RX_SEM_OK;
}

int rx_sem_get(RxSemEngine *e, uint32_t branch, uint64_t sid, RxSemValue *out,
               RxSemServed *served) {
    if (!e) return RX_SEM_ERR_ARG;
    pthread_mutex_lock(&e->mu);
    RxSemBranch *b = branch_of(e, branch);
    int rc = RX_SEM_ERR_NOT_FOUND;
    if (b) {
        uint32_t idx = node_index(e, sid);
        if (idx != NONE) {
            rc = get_locked(e, b, idx, served);
            if (rc == RX_SEM_OK && out) *out = b->memo[idx];
        } else {
            uint32_t si = source_index(e, sid);
            if (si != NONE && b->have_source[si] && !b->source_stale[si]) {
                rc = RX_SEM_OK;
                if (out) *out = b->source[si];
            } else if (si != NONE && b->source_stale[si]) rc = RX_SEM_ERR_STALE;
        }
    }
    pthread_mutex_unlock(&e->mu);
    return rc;
}

int rx_sem_read_source(RxSemEngine *e, uint32_t branch, uint64_t sid, RxSemValue *out) {
    if (RX_SEM_KIND(sid) != RX_SEM_KIND_SOURCE) return RX_SEM_ERR_ARG;
    return rx_sem_get(e, branch, sid, out, NULL);
}

int rx_sem_state(RxSemEngine *e, uint32_t branch, uint64_t sid) {
    pthread_mutex_lock(&e->mu);
    RxSemBranch *b = branch_of(e, branch);
    uint32_t idx = node_index(e, sid);
    int st = (!b || idx == NONE) ? RX_SEM_ERR_NOT_FOUND : b->state[idx];
    pthread_mutex_unlock(&e->mu);
    return st;
}

int rx_sem_explain(RxSemEngine *e, uint32_t branch, uint64_t sid, RxSemDerived *out) {
    pthread_mutex_lock(&e->mu);
    RxSemBranch *b = branch_of(e, branch);
    uint32_t idx = node_index(e, sid);
    int rc = RX_SEM_ERR_NOT_FOUND;
    if (b && idx != NONE && b->state[idx] == RX_SEM_CLEAN && b->memo_entry[idx] != NONE) {
        *out = e->cache[b->memo_entry[idx]];
        rc = RX_SEM_OK;
    }
    pthread_mutex_unlock(&e->mu);
    return rc;
}

/* ---- verification ---------------------------------------------------------- */

int rx_sem_verify(RxSemEngine *e, uint64_t *entries_checked, uint64_t *events_checked,
                  uint64_t *memos_checked) {
    pthread_mutex_lock(&e->mu);
    int rc = RX_SEM_OK;
    uint64_t ne = 0, nv = 0, nm = 0;
    for (uint32_t s = 0; s < RX_SEM_CACHE_SLOTS; s++) {
        const RxSemDerived *d = &e->cache[s];
        if (!d->used) continue;
        ne++;
        RxSemDigest k, dd, vd;
        compute_key(&d->derivation_id, d->input_versions, d->n_inputs, &k);
        entry_digest(d, &dd);
        rx_sem_digest_value(d->result.type, d->result.value, d->result.n_words, &vd);
        if (!rx_sem_digest_eq(&k, &d->key) || !rx_sem_digest_eq(&dd, &d->entry_digest) ||
            !rx_sem_digest_eq(&vd, &d->result.value_digest) ||
            d->function_id >= e->n_fns ||
            !rx_sem_digest_eq(&d->derivation_id, &e->fn_derivation[d->function_id]))
            rc = RX_SEM_ERR_VERIFY;
    }
    RxSemDigest prev;
    memset(&prev, 0, sizeof prev);
    for (uint64_t i = 0; i < e->n_events; i++) {
        const RxSemEvent *ev = &e->events[i];
        nv++;
        if (ev->id != i + 1 || !event_ok(ev, &prev) || ev->entry >= RX_SEM_CACHE_SLOTS ||
            !e->cache[ev->entry].used ||
            !rx_sem_digest_eq(&ev->entry_digest, &e->cache[ev->entry].entry_digest))
            rc = RX_SEM_ERR_VERIFY;
        prev = ev->digest;
    }
    /* Every clean memo is exactly its entry's result, and that entry was
     * derived from what the branch holds now. */
    for (uint32_t bi = 0; bi < RX_SEM_MAX_BRANCHES; bi++) {
        const RxSemBranch *b = &e->branches[bi];
        if (!b->live) continue;
        for (uint32_t n = 0; n < e->n_nodes; n++) {
            if (b->state[n] != RX_SEM_CLEAN) continue;
            nm++;
            const RxSemNode *nd = &e->nodes[n];
            if (b->memo_entry[n] == NONE) continue;
            const RxSemDerived *d = &e->cache[b->memo_entry[n]];
            if (!same_content(&d->result, &b->memo[n]) || d->n_inputs != nd->n_inputs) {
                rc = RX_SEM_ERR_VERIFY;
                continue;
            }
            for (uint32_t i = 0; i < nd->n_inputs; i++) {
                uint64_t sid = nd->inputs[i];
                const RxSemValue *v;
                if (RX_SEM_KIND(sid) == RX_SEM_KIND_SOURCE) {
                    uint32_t si = source_index(e, sid);
                    if (b->source_stale[si] || !b->have_source[si]) { rc = RX_SEM_ERR_VERIFY; break; }
                    v = &b->source[si];
                } else {
                    uint32_t ci = node_index(e, sid);
                    if (b->state[ci] != RX_SEM_CLEAN) { rc = RX_SEM_ERR_VERIFY; break; }
                    v = &b->memo[ci];
                }
                if (d->input_versions[i].semantic_id != sid ||
                    d->input_versions[i].generation != v->generation ||
                    !rx_sem_digest_eq(&d->input_versions[i].value_digest, &v->value_digest)) {
                    rc = RX_SEM_ERR_VERIFY;
                    break;
                }
            }
        }
    }
    if (entries_checked) *entries_checked = ne;
    if (events_checked) *events_checked = nv;
    if (memos_checked) *memos_checked = nm;
    pthread_mutex_unlock(&e->mu);
    return rc;
}

/* ---- structure ------------------------------------------------------------- */

int rx_sem_node_inputs(RxSemEngine *e, uint64_t sid, uint64_t *inputs, uint32_t *n) {
    pthread_mutex_lock(&e->mu);
    uint32_t idx = node_index(e, sid);
    if (idx != NONE) {
        *n = e->nodes[idx].n_inputs;
        memcpy(inputs, e->nodes[idx].inputs, *n * sizeof *inputs);
    }
    pthread_mutex_unlock(&e->mu);
    return idx == NONE ? RX_SEM_ERR_NOT_FOUND : RX_SEM_OK;
}

static void collect(const RxSemEngine *e, uint32_t idx, uint8_t *seen) {
    if (seen[idx]) return;
    seen[idx] = 1;
    for (uint32_t i = 0; i < e->nodes[idx].n_readers; i++) collect(e, e->nodes[idx].readers[i], seen);
}

int rx_sem_downstream(RxSemEngine *e, uint64_t sid, uint64_t *out, uint32_t cap, uint32_t *n) {
    pthread_mutex_lock(&e->mu);
    uint8_t seen[RX_SEM_MAX_NODES];
    memset(seen, 0, sizeof seen);
    int rc = RX_SEM_OK;
    uint32_t si = source_index(e, sid);
    uint32_t ni = node_index(e, sid);
    if (si != NONE) {
        for (uint32_t r = 0; r < e->sources[si].n_readers; r++)
            collect(e, e->sources[si].readers[r], seen);
    } else if (ni != NONE) {
        for (uint32_t r = 0; r < e->nodes[ni].n_readers; r++)
            collect(e, e->nodes[ni].readers[r], seen);
    } else rc = RX_SEM_ERR_NOT_FOUND;
    uint32_t k = 0;
    for (uint32_t i = 0; i < e->n_nodes && rc == RX_SEM_OK; i++) {
        if (!seen[i]) continue;
        if (k >= cap) { rc = RX_SEM_ERR_FULL; break; }
        out[k++] = e->nodes[i].semantic_id;
    }
    *n = k;
    pthread_mutex_unlock(&e->mu);
    return rc;
}
