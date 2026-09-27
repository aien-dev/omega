/*
 * rx_world.c -- resident reaction runtime, simple human reference. See rx_world.h.
 */
#include "rx_world.h"
#include "sha256.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- helpers ------------------------------------------------------------ */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void put32(sha256_ctx *c, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    sha256_update(c, b, 4);
}

static void put64(sha256_ctx *c, uint64_t v) {
    put32(c, (uint32_t)v);
    put32(c, (uint32_t)(v >> 32));
}

static void object_digest(RxObject *o) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)"AIEN_RX_OBJECT_V1", 17);
    put32(&c, o->type);
    for (uint32_t f = 0; f < RX_MAX_FIELDS; f++) put64(&c, o->field[f]);
    sha256_final(&c, o->digest);
}

/* ---- lifecycle ---------------------------------------------------------- */

bool rx_state_transition_legal(RxState from, RxState to) {
    switch (from) {
    case RX_DORMANT:
        return to == RX_READY;
    case RX_READY:
        return to == RX_RUNNING || to == RX_BLOCKED_AUTHORITY ||
               to == RX_BLOCKED_RESOURCE || to == RX_INVALIDATED;
    case RX_RUNNING:
        return to == RX_PUBLISHING || to == RX_FAILED || to == RX_CANCELLED ||
               to == RX_INVALIDATED;
    case RX_PUBLISHING:
        /* Stale input is detected while validating the publication, so the
         * explicit exit is PUBLISHING -> INVALIDATED. */
        return to == RX_COMMITTED || to == RX_CONFLICT || to == RX_REJECTED ||
               to == RX_INVALIDATED;
    case RX_COMMITTED: case RX_INVALIDATED: case RX_FAILED: case RX_CANCELLED:
    case RX_CONFLICT: case RX_REJECTED: case RX_BLOCKED_AUTHORITY: case RX_BLOCKED_RESOURCE:
        return to == RX_DORMANT;
    default:
        return false;
    }
}

const char *rx_state_name(RxState s) {
    static const char *names[RX_STATE_COUNT] = {
        "DORMANT", "READY", "BLOCKED_AUTHORITY", "BLOCKED_RESOURCE", "RUNNING",
        "PUBLISHING", "COMMITTED", "INVALIDATED", "FAILED", "CANCELLED", "CONFLICT",
        "REJECTED"
    };
    return (unsigned)s < RX_STATE_COUNT ? names[s] : "?";
}

const char *rx_crumb_kind_name(RxCrumbKind k) {
    switch (k) {
    case RX_CRUMB_CREATE: return "CREATE";
    case RX_CRUMB_EXTERNAL: return "EXTERNAL";
    case RX_CRUMB_COMMIT: return "COMMIT";
    case RX_CRUMB_INVALIDATED: return "INVALIDATED";
    case RX_CRUMB_BLOCKED_AUTHORITY: return "BLOCKED_AUTHORITY";
    case RX_CRUMB_REJECTED: return "REJECTED";
    case RX_CRUMB_FAILED: return "FAILED";
    case RX_CRUMB_NOOP: return "NOOP";
    case RX_CRUMB_RETIRE: return "RETIRE";
    default: return "?";
    }
}

static void set_state(RxWorld *w, RxReaction *r, RxState to) {
    if (!rx_state_transition_legal(r->state, to)) w->stats.illegal_transitions++;
    w->stats.transitions[r->state][to]++;
    r->state = to;
}

/* ---- causal crumbs ------------------------------------------------------ */

static void crumb_hash(const RxWorld *w, const RxCrumb *k, uint8_t out[32]) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)"AIEN_RX_CAUSAL_V1", 17);
    put64(&c, k->id);
    put32(&c, (uint32_t)k->kind);
    put32(&c, k->reaction);
    put32(&c, k->faculty);
    put64(&c, k->wake_cause);
    put64(&c, k->coalesced_wakes);
    put32(&c, k->n_inputs);
    for (uint32_t i = 0; i < k->n_inputs; i++) {
        put32(&c, k->inputs[i].obj.id);
        put32(&c, k->inputs[i].obj.generation);
        put64(&c, k->inputs[i].version);
        put64(&c, k->inputs[i].mask);
    }
    put32(&c, k->n_caps);
    for (uint32_t i = 0; i < k->n_caps; i++) {
        put32(&c, k->caps[i].cap_id);
        put32(&c, k->caps[i].generation);
    }
    put32(&c, k->n_outputs);
    for (uint32_t i = 0; i < k->n_outputs; i++) {
        put32(&c, k->outputs[i].obj.id);
        put32(&c, k->outputs[i].obj.generation);
        put64(&c, k->outputs[i].version);
        put64(&c, k->outputs[i].mask);
    }
    put32(&c, (uint32_t)k->reason);
    put32(&c, k->n_parents);
    for (uint32_t i = 0; i < k->n_parents; i++) {
        uint64_t p = k->parents[i];
        put64(&c, p);
        /* Parents always precede children, so their digests are final. */
        if (p >= 1 && p <= w->n_crumbs && p < k->id)
            sha256_update(&c, w->crumbs[p - 1].digest, 32);
    }
    /* Timing and worker placement are recorded but are not identity: the
     * same causal history must hash the same on any schedule. */
    sha256_final(&c, out);
}

static void add_parent(RxCrumb *k, uint64_t p) {
    if (p == 0) return;
    for (uint32_t i = 0; i < k->n_parents; i++)
        if (k->parents[i] == p) return;
    if (k->n_parents < RX_MAX_PARENTS) k->parents[k->n_parents++] = p;
}

/* Append a crumb; returns its id (0 if the log is full). Caller holds mu. */
static uint64_t crumb_append(RxWorld *w, RxCrumb *k) {
    if (w->n_crumbs >= w->crumb_cap) {
        w->stats.crumb_overflow++;
        return 0;
    }
    k->id = w->n_crumbs + 1;
    /* Canonical parent order: ascending. */
    for (uint32_t i = 1; i < k->n_parents; i++) {
        uint64_t v = k->parents[i];
        uint32_t j = i;
        while (j > 0 && k->parents[j - 1] > v) { k->parents[j] = k->parents[j - 1]; j--; }
        k->parents[j] = v;
    }
    crumb_hash(w, k, k->digest);
    w->crumbs[w->n_crumbs++] = *k;
    return k->id;
}

const RxCrumb *rx_world_crumb(const RxWorld *w, uint64_t id) {
    if (id == 0 || id > w->n_crumbs) return NULL;
    return &w->crumbs[id - 1];
}

int rx_world_verify_crumbs(RxWorld *w, uint64_t *out_checked) {
    pthread_mutex_lock(&w->mu);
    int rc = 0;
    uint64_t n = 0;
    for (uint64_t i = 0; i < w->n_crumbs; i++) {
        const RxCrumb *k = &w->crumbs[i];
        uint8_t d[32];
        if (k->id != i + 1) { rc = -1; break; }
        for (uint32_t p = 0; p < k->n_parents; p++)
            if (k->parents[p] == 0 || k->parents[p] >= k->id) { rc = -2; break; }
        if (rc) break;
        crumb_hash(w, k, d);
        if (memcmp(d, k->digest, 32) != 0) { rc = -3; break; }
        n++;
    }
    pthread_mutex_unlock(&w->mu);
    if (out_checked) *out_checked = n;
    return rc;
}

/* ---- wakeups and admission ---------------------------------------------- */

static void enqueue(RxWorld *w, uint32_t rid) {
    uint32_t p = w->reactions[rid].desc.priority;
    uint32_t tail = (w->ready_head[p] + w->ready_len[p]) % RX_MAX_REACTIONS;
    w->ready_q[p][tail] = rid;
    w->ready_len[p]++;
    w->in_flight++;
    pthread_cond_signal(&w->work_cv);
}

static void wake(RxWorld *w, uint32_t rid, uint64_t cause) {
    RxReaction *r = &w->reactions[rid];
    w->stats.wakes++;
    switch (r->state) {
    case RX_DORMANT:
        set_state(w, r, RX_READY);
        r->wake_cause = cause;
        r->coalesced = 0;
        enqueue(w, rid);
        break;
    case RX_READY:
        /* Already admitted-pending: fold this wake into the same activation. */
        r->wake_cause = cause;
        r->coalesced++;
        w->stats.coalesced_wakes++;
        break;
    default:
        /* Running or publishing: its snapshot may be stale. Run again after. */
        r->wake_cause = cause;
        r->rearm = true;
        w->stats.coalesced_wakes++;
        break;
    }
}

/* Only the subscribers of this object, filtered by field mask. */
static void propagate(RxWorld *w, uint32_t obj, uint64_t changed, uint64_t cause) {
    uint32_t gen = w->objects[obj].generation;
    for (uint32_t i = 0; i < w->n_subs[obj]; i++) {
        const RxSub *s = &w->subs[obj][i];
        w->stats.subscriptions_checked++;
        if (s->generation != gen) continue;
        if (s->mask & changed) wake(w, s->reaction, cause);
    }
}

static bool pop_ready(RxWorld *w, uint32_t *rid) {
    for (uint32_t p = 0; p < RX_PRIORITY_CLASSES; p++) {
        if (w->ready_len[p] == 0) continue;
        *rid = w->ready_q[p][w->ready_head[p]];
        w->ready_head[p] = (w->ready_head[p] + 1) % RX_MAX_REACTIONS;
        w->ready_len[p]--;
        return true;
    }
    return false;
}

/* ---- validation --------------------------------------------------------- */

static bool ref_live(const RxWorld *w, RxObjRef ref) {
    return ref.id < RX_MAX_OBJECTS && w->objects[ref.id].live &&
           w->objects[ref.id].generation == ref.generation;
}

static int validate_caps(const RxWorld *w, const RxReactionDesc *d, int *first_err) {
    for (uint32_t i = 0; i < d->n_caps; i++) {
        int rc = rx_caproot_validate(w->root, d->caps[i].ref, d->subject,
                                     d->caps[i].resource, d->caps[i].rights, NULL);
        if (rc != RX_CAP_OK) {
            if (first_err) *first_err = rc;
            return -1;
        }
    }
    return 0;
}

/* Static coverage: every object read needs a READ capability need on its
 * resource and every object written a WRITE one. */
static bool covered(const RxWorld *w, const RxReactionDesc *d, RxObjRef o, uint32_t right) {
    uint64_t res = w->objects[o.id].resource;
    for (uint32_t i = 0; i < d->n_caps; i++)
        if (d->caps[i].resource == res && (d->caps[i].rights & right) == right) return true;
    return false;
}

/* Collect triggers + reads into one deduplicated dependency list. */
static uint32_t gather_deps(const RxReactionDesc *d, RxDep out[RX_MAX_DEPS]) {
    uint32_t n = 0;
    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t cnt = pass ? d->n_reads : d->n_triggers;
        const RxDep *src = pass ? d->reads : d->triggers;
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t j;
            for (j = 0; j < n; j++)
                if (out[j].obj.id == src[i].obj.id && out[j].obj.generation == src[i].obj.generation) {
                    out[j].mask |= src[i].mask;
                    break;
                }
            if (j == n && n < RX_MAX_DEPS) out[n++] = src[i];
        }
    }
    return n;
}

/* ---- publication -------------------------------------------------------- */

typedef struct {
    RxObjRef obj;
    uint64_t changed;
    uint64_t value[RX_MAX_FIELDS];
} PendingWrite;

/* Apply mutations atomically (caller holds mu). Returns the number of objects
 * written into pw/n_pw, or a negative error with nothing applied. */
static int stage_mutations(RxWorld *w, const RxMutation *m, uint32_t n,
                           const RxDep *write_set, uint32_t n_ws,
                           PendingWrite pw[RX_MAX_WRITES], uint32_t *n_pw) {
    *n_pw = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (m[i].field >= RX_MAX_FIELDS) return RX_ERR_WRITE_SET;
        if (!ref_live(w, m[i].obj)) return RX_ERR_STALE_GEN;
        if (write_set) {
            bool ok = false;
            for (uint32_t k = 0; k < n_ws; k++)
                if (write_set[k].obj.id == m[i].obj.id &&
                    write_set[k].obj.generation == m[i].obj.generation &&
                    (write_set[k].mask & RX_FIELD(m[i].field))) ok = true;
            if (!ok) return RX_ERR_WRITE_SET;
        }
        uint32_t j;
        for (j = 0; j < *n_pw; j++)
            if (pw[j].obj.id == m[i].obj.id) break;
        if (j == *n_pw) {
            if (*n_pw >= RX_MAX_WRITES) return RX_ERR_FULL;
            pw[j].obj = m[i].obj;
            pw[j].changed = 0;
            memcpy(pw[j].value, w->objects[m[i].obj.id].field, sizeof(pw[j].value));
            (*n_pw)++;
        }
        pw[j].value[m[i].field] = m[i].value;
    }
    for (uint32_t j = 0; j < *n_pw; j++) {
        const RxObject *o = &w->objects[pw[j].obj.id];
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++)
            if (pw[j].value[f] != o->field[f]) pw[j].changed |= RX_FIELD(f);
    }
    return RX_OK;
}

static void commit_writes(RxWorld *w, PendingWrite *pw, uint32_t n_pw, RxCrumb *k) {
    k->n_outputs = 0;
    for (uint32_t j = 0; j < n_pw; j++) {
        if (!pw[j].changed) continue;
        RxObject *o = &w->objects[pw[j].obj.id];
        o->version++;
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++) {
            if (!(pw[j].changed & RX_FIELD(f))) continue;
            o->field[f] = pw[j].value[f];
            o->field_version[f] = o->version;
        }
        object_digest(o);
        k->outputs[k->n_outputs].obj = pw[j].obj;
        k->outputs[k->n_outputs].version = o->version;
        k->outputs[k->n_outputs].mask = pw[j].changed;
        k->n_outputs++;
    }
}

/* After the crumb id is known: stamp writers, then wake subscribers. */
static void finish_writes(RxWorld *w, PendingWrite *pw, uint32_t n_pw, uint64_t cid) {
    for (uint32_t j = 0; j < n_pw; j++) {
        if (!pw[j].changed) continue;
        RxObject *o = &w->objects[pw[j].obj.id];
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++)
            if (pw[j].changed & RX_FIELD(f)) o->field_writer[f] = cid;
    }
    for (uint32_t j = 0; j < n_pw; j++)
        if (pw[j].changed) propagate(w, pw[j].obj.id, pw[j].changed, cid);
}

/* ---- worker ------------------------------------------------------------- */

static void end_activation(RxWorld *w, uint32_t rid) {
    RxReaction *r = &w->reactions[rid];
    set_state(w, r, RX_DORMANT);
    w->in_flight--;
    if (r->rearm) {
        r->rearm = false;
        set_state(w, r, RX_READY);
        r->coalesced = 0;
        enqueue(w, rid);
    }
    if (w->in_flight == 0) pthread_cond_broadcast(&w->idle_cv);
}

static void fill_inputs(RxCrumb *k, const RxCtx *ctx, const RxDep *deps, uint32_t n) {
    k->n_inputs = n;
    for (uint32_t i = 0; i < n; i++) {
        k->inputs[i].obj = deps[i].obj;
        k->inputs[i].version = ctx ? ctx->in[i].version : 0;
        k->inputs[i].mask = deps[i].mask;
    }
}

static void run_one(RxWorld *w, uint32_t rid, uint32_t worker) {
    RxReaction *r = &w->reactions[rid];
    const RxReactionDesc *d = &r->desc;
    RxCrumb k;
    memset(&k, 0, sizeof(k));
    k.reaction = rid;
    k.faculty = d->faculty;
    k.worker = worker;
    k.wake_cause = r->wake_cause;
    k.coalesced_wakes = r->coalesced;
    k.t_start_ns = now_ns();
    k.n_caps = d->n_caps;
    for (uint32_t i = 0; i < d->n_caps; i++) k.caps[i] = d->caps[i].ref;
    add_parent(&k, r->wake_cause);
    r->activations++;

    RxDep deps[RX_MAX_DEPS];
    uint32_t n_deps = gather_deps(d, deps);

    /* Semantic readiness: required inputs exist. */
    for (uint32_t i = 0; i < n_deps; i++) {
        if (!ref_live(w, deps[i].obj)) {
            set_state(w, r, RX_INVALIDATED);
            fill_inputs(&k, NULL, deps, n_deps);
            k.kind = RX_CRUMB_INVALIDATED;
            k.reason = RX_ERR_STALE_GEN;
            k.t_end_ns = now_ns();
            crumb_append(w, &k);
            w->stats.invalidations++;
            end_activation(w, rid);
            return;
        }
    }
    /* Required authority exists. */
    int cap_err = 0;
    if (validate_caps(w, d, &cap_err) != 0) {
        set_state(w, r, RX_BLOCKED_AUTHORITY);
        fill_inputs(&k, NULL, deps, n_deps);
        k.kind = RX_CRUMB_BLOCKED_AUTHORITY;
        k.reason = cap_err;
        k.t_end_ns = now_ns();
        crumb_append(w, &k);
        w->stats.blocked_authority++;
        end_activation(w, rid);
        return;
    }

    set_state(w, r, RX_RUNNING);
    RxCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.user = d->user;
    ctx.worker = worker;
    ctx.n_in = n_deps;
    for (uint32_t i = 0; i < n_deps; i++) {
        const RxObject *o = &w->objects[deps[i].obj.id];
        ctx.in[i].obj = deps[i].obj;
        ctx.in[i].mask = deps[i].mask;
        ctx.in[i].version = o->version;
        memcpy(ctx.in[i].field, o->field, sizeof(o->field));
        memcpy(ctx.in[i].field_version, o->field_version, sizeof(o->field_version));
        memcpy(ctx.in[i].field_writer, o->field_writer, sizeof(o->field_writer));
    }

    /* Compute against the snapshot, outside the world lock. */
    pthread_mutex_unlock(&w->mu);
    int frc = d->fn(&ctx);
    pthread_mutex_lock(&w->mu);

    fill_inputs(&k, &ctx, deps, n_deps);
    /* Data ancestry: whoever last wrote each field this reaction read. */
    for (uint32_t i = 0; i < n_deps; i++)
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++)
            if (deps[i].mask & RX_FIELD(f)) add_parent(&k, ctx.in[i].field_writer[f]);

    if (frc < 0) {
        set_state(w, r, RX_FAILED);
        k.kind = RX_CRUMB_FAILED;
        k.reason = frc;
        k.t_end_ns = now_ns();
        crumb_append(w, &k);
        w->stats.failed++;
        end_activation(w, rid);
        return;
    }

    set_state(w, r, RX_PUBLISHING);
    /* 1. Versioned reads still valid? */
    bool stale = false;
    for (uint32_t i = 0; i < n_deps && !stale; i++) {
        const RxObject *o = &w->objects[deps[i].obj.id];
        if (!ref_live(w, deps[i].obj)) { stale = true; break; }
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++)
            if ((deps[i].mask & RX_FIELD(f)) && o->field_version[f] != ctx.in[i].field_version[f]) {
                stale = true;
                break;
            }
    }
    if (stale) {
        set_state(w, r, RX_INVALIDATED);
        k.kind = RX_CRUMB_INVALIDATED;
        k.reason = RX_ERR_STALE_GEN;
        k.t_end_ns = now_ns();
        crumb_append(w, &k);
        w->stats.invalidations++;
        /* The change that made this stale has already re-armed it. */
        end_activation(w, rid);
        return;
    }
    /* 2. Authority still valid (revocation during the run must not execute). */
    if (validate_caps(w, d, &cap_err) != 0) {
        set_state(w, r, RX_REJECTED);
        k.kind = RX_CRUMB_REJECTED;
        k.reason = cap_err;
        k.t_end_ns = now_ns();
        crumb_append(w, &k);
        w->stats.rejected++;
        end_activation(w, rid);
        return;
    }
    /* 3. Mutations inside the declared write set, targets not stale. */
    PendingWrite pw[RX_MAX_WRITES];
    uint32_t n_pw = 0;
    int src = stage_mutations(w, ctx.out, ctx.n_out, d->writes, d->n_writes, pw, &n_pw);
    if (src != RX_OK) {
        set_state(w, r, RX_REJECTED);
        k.kind = RX_CRUMB_REJECTED;
        k.reason = src;
        k.t_end_ns = now_ns();
        crumb_append(w, &k);
        w->stats.rejected++;
        end_activation(w, rid);
        return;
    }
    /* 4. Atomic publish. */
    commit_writes(w, pw, n_pw, &k);
    k.kind = k.n_outputs ? RX_CRUMB_COMMIT : RX_CRUMB_NOOP;
    k.t_end_ns = now_ns();
    uint64_t cid = crumb_append(w, &k);
    set_state(w, r, RX_COMMITTED);
    if (k.n_outputs) {
        w->stats.commits++;
        r->commits++;
    } else {
        w->stats.noops++;
    }
    finish_writes(w, pw, n_pw, cid);
    end_activation(w, rid);
}

static void *worker_main(void *arg) {
    RxWorld *w = arg;
    pthread_mutex_lock(&w->mu);
    uint32_t me = UINT32_MAX;
    for (uint32_t i = 0; i < w->n_workers; i++)
        if (pthread_equal(w->workers[i], pthread_self())) me = i;
    for (;;) {
        uint32_t rid;
        while (!w->stopping && !pop_ready(w, &rid))
            pthread_cond_wait(&w->work_cv, &w->mu);
        if (w->stopping) break;
        run_one(w, rid, me);
    }
    pthread_mutex_unlock(&w->mu);
    return NULL;
}

/* ---- public API ---------------------------------------------------------- */

int rx_world_init(RxWorld *w, RxCapRoot *root, uint32_t n_workers, uint64_t crumb_cap) {
    if (!w || !root || n_workers == 0 || n_workers > RX_MAX_WORKERS || crumb_cap == 0)
        return RX_ERR_ARG;
    memset(w, 0, sizeof(*w));
    w->root = root;
    w->crumbs = calloc(crumb_cap, sizeof(RxCrumb));
    if (!w->crumbs) return RX_ERR_FULL;
    w->crumb_cap = crumb_cap;
    for (uint32_t i = 0; i < RX_MAX_OBJECTS; i++) {
        w->objects[i].id = i;
        w->objects[i].generation = 1;
    }
    pthread_mutex_init(&w->mu, NULL);
    pthread_cond_init(&w->work_cv, NULL);
    pthread_cond_init(&w->idle_cv, NULL);
    pthread_mutex_lock(&w->mu);
    w->n_workers = n_workers;
    for (uint32_t i = 0; i < n_workers; i++)
        pthread_create(&w->workers[i], NULL, worker_main, w);
    pthread_mutex_unlock(&w->mu);
    return RX_OK;
}

void rx_world_destroy(RxWorld *w) {
    pthread_mutex_lock(&w->mu);
    w->stopping = true;
    pthread_cond_broadcast(&w->work_cv);
    pthread_mutex_unlock(&w->mu);
    for (uint32_t i = 0; i < w->n_workers; i++) pthread_join(w->workers[i], NULL);
    for (uint32_t i = 0; i < RX_MAX_OBJECTS; i++) free(w->subs[i]);
    free(w->crumbs);
    pthread_cond_destroy(&w->work_cv);
    pthread_cond_destroy(&w->idle_cv);
    pthread_mutex_destroy(&w->mu);
}

int rx_world_create(RxWorld *w, uint32_t type, RxPersist persist, uint64_t resource,
                    const uint64_t init[RX_MAX_FIELDS], RxObjRef *out) {
    pthread_mutex_lock(&w->mu);
    for (uint32_t i = 0; i < RX_MAX_OBJECTS; i++) {
        RxObject *o = &w->objects[i];
        if (o->live) continue;
        o->live = true;
        o->type = type;
        o->persist = persist;
        o->resource = resource;
        o->version = 1;
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++) {
            o->field[f] = init ? init[f] : 0;
            o->field_version[f] = 1;
        }
        object_digest(o);
        RxCrumb k;
        memset(&k, 0, sizeof(k));
        k.kind = RX_CRUMB_CREATE;
        k.reaction = UINT32_MAX;
        k.faculty = RX_FACULTY_EXTERNAL;
        k.n_outputs = 1;
        k.outputs[0].obj = (RxObjRef){ i, o->generation };
        k.outputs[0].version = 1;
        k.outputs[0].mask = RX_ALL_FIELDS;
        k.t_start_ns = k.t_end_ns = now_ns();
        uint64_t cid = crumb_append(w, &k);
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++) o->field_writer[f] = cid;
        out->id = i;
        out->generation = o->generation;
        pthread_mutex_unlock(&w->mu);
        return RX_OK;
    }
    pthread_mutex_unlock(&w->mu);
    return RX_ERR_FULL;
}

int rx_world_retire(RxWorld *w, RxObjRef ref) {
    pthread_mutex_lock(&w->mu);
    if (!ref_live(w, ref)) {
        pthread_mutex_unlock(&w->mu);
        return RX_ERR_STALE_GEN;
    }
    RxObject *o = &w->objects[ref.id];
    o->live = false;
    o->generation++;
    RxCrumb k;
    memset(&k, 0, sizeof(k));
    k.kind = RX_CRUMB_RETIRE;
    k.reaction = UINT32_MAX;
    k.n_inputs = 1;
    k.inputs[0].obj = ref;
    k.inputs[0].version = o->version;
    k.inputs[0].mask = RX_ALL_FIELDS;
    k.t_start_ns = k.t_end_ns = now_ns();
    crumb_append(w, &k);
    pthread_mutex_unlock(&w->mu);
    return RX_OK;
}

int rx_world_add_reaction(RxWorld *w, const RxReactionDesc *d, uint32_t *out_id) {
    if (!d || !d->fn || d->priority >= RX_PRIORITY_CLASSES || d->n_triggers == 0 ||
        d->n_triggers > RX_MAX_DEPS || d->n_reads > RX_MAX_DEPS ||
        d->n_writes > RX_MAX_WRITES || d->n_caps > RX_MAX_CAPS)
        return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    int rc = RX_OK;
    if (w->n_reactions >= RX_MAX_REACTIONS) { rc = RX_ERR_FULL; goto out; }
    /* Declared dependencies must exist now, and every read and write must be
     * covered by a declared capability need on that object's resource. The
     * needs themselves are only validated against the root at run time. */
    for (uint32_t i = 0; i < d->n_triggers; i++)
        if (!ref_live(w, d->triggers[i].obj)) { rc = RX_ERR_STALE_GEN; goto out; }
    for (uint32_t i = 0; i < d->n_reads; i++)
        if (!ref_live(w, d->reads[i].obj)) { rc = RX_ERR_STALE_GEN; goto out; }
    for (uint32_t i = 0; i < d->n_writes; i++)
        if (!ref_live(w, d->writes[i].obj)) { rc = RX_ERR_STALE_GEN; goto out; }
    for (uint32_t i = 0; i < d->n_triggers; i++)
        if (!covered(w, d, d->triggers[i].obj, RX_RIGHT_READ)) { rc = RX_ERR_AUTHORITY; goto out; }
    for (uint32_t i = 0; i < d->n_reads; i++)
        if (!covered(w, d, d->reads[i].obj, RX_RIGHT_READ)) { rc = RX_ERR_AUTHORITY; goto out; }
    for (uint32_t i = 0; i < d->n_writes; i++)
        if (!covered(w, d, d->writes[i].obj, RX_RIGHT_WRITE)) { rc = RX_ERR_AUTHORITY; goto out; }
    /* Index triggers only: a read-only dependency is observed, not a wake. */
    for (uint32_t i = 0; i < d->n_triggers; i++) {
        uint32_t o = d->triggers[i].obj.id;
        if (!w->subs[o]) {
            w->subs[o] = calloc(RX_MAX_SUBS, sizeof(RxSub));
            if (!w->subs[o]) { rc = RX_ERR_FULL; goto out; }
        }
        if (w->n_subs[o] >= RX_MAX_SUBS) { rc = RX_ERR_FULL; goto out; }
    }
    uint32_t rid = w->n_reactions++;
    RxReaction *r = &w->reactions[rid];
    memset(r, 0, sizeof(*r));
    r->desc = *d;
    r->state = RX_DORMANT;
    for (uint32_t i = 0; i < d->n_triggers; i++) {
        uint32_t o = d->triggers[i].obj.id;
        w->subs[o][w->n_subs[o]++] = (RxSub){ rid, d->triggers[i].obj.generation, d->triggers[i].mask };
    }
    if (out_id) *out_id = rid;
out:
    pthread_mutex_unlock(&w->mu);
    return rc;
}

int64_t rx_world_publish_external(RxWorld *w, RxCapRef cap, const RxMutation *muts, uint32_t n) {
    if (!muts || n == 0 || n > RX_MAX_MUTATIONS) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    for (uint32_t i = 0; i < n; i++) {
        if (!ref_live(w, muts[i].obj)) { pthread_mutex_unlock(&w->mu); return RX_ERR_STALE_GEN; }
        int rc = rx_caproot_validate(w->root, cap, w->external_subject,
                                     w->objects[muts[i].obj.id].resource, RX_RIGHT_WRITE, NULL);
        if (rc != RX_CAP_OK) { pthread_mutex_unlock(&w->mu); return RX_ERR_AUTHORITY; }
    }
    PendingWrite pw[RX_MAX_WRITES];
    uint32_t n_pw = 0;
    int rc = stage_mutations(w, muts, n, NULL, 0, pw, &n_pw);
    if (rc != RX_OK) { pthread_mutex_unlock(&w->mu); return rc; }
    RxCrumb k;
    memset(&k, 0, sizeof(k));
    k.kind = RX_CRUMB_EXTERNAL;
    k.reaction = UINT32_MAX;
    k.faculty = RX_FACULTY_EXTERNAL;
    k.n_caps = 1;
    k.caps[0] = cap;
    k.t_start_ns = now_ns();
    commit_writes(w, pw, n_pw, &k);
    k.t_end_ns = now_ns();
    uint64_t cid = crumb_append(w, &k);
    finish_writes(w, pw, n_pw, cid);
    pthread_mutex_unlock(&w->mu);
    return (int64_t)cid;
}

int rx_world_wait_quiescent(RxWorld *w, int timeout_ms) {
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += timeout_ms / 1000;
    dl.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&w->mu);
    int rc = RX_OK;
    while (w->in_flight != 0) {
        if (pthread_cond_timedwait(&w->idle_cv, &w->mu, &dl) == ETIMEDOUT) {
            rc = RX_ERR_TIMEOUT;
            break;
        }
    }
    pthread_mutex_unlock(&w->mu);
    return rc;
}

int rx_world_read(RxWorld *w, RxObjRef ref, RxObject *out) {
    pthread_mutex_lock(&w->mu);
    int rc = ref_live(w, ref) ? RX_OK : RX_ERR_STALE_GEN;
    if (rc == RX_OK) *out = w->objects[ref.id];
    pthread_mutex_unlock(&w->mu);
    return rc;
}

uint64_t rx_world_explain(RxWorld *w, RxObjRef ref, uint32_t field) {
    pthread_mutex_lock(&w->mu);
    uint64_t id = (ref_live(w, ref) && field < RX_MAX_FIELDS)
                      ? w->objects[ref.id].field_writer[field] : 0;
    pthread_mutex_unlock(&w->mu);
    return id;
}

void rx_world_digest(RxWorld *w, uint8_t out[32]) {
    pthread_mutex_lock(&w->mu);
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)"AIEN_RX_WORLD_V1", 16);
    for (uint32_t i = 0; i < RX_MAX_OBJECTS; i++) {
        const RxObject *o = &w->objects[i];
        if (!o->live) continue;
        put32(&c, o->id);
        put32(&c, o->generation);
        sha256_update(&c, o->digest, 32);
    }
    sha256_final(&c, out);
    pthread_mutex_unlock(&w->mu);
}
