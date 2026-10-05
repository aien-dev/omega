/*
 * world_diff.c -- differential harness: the lifecycle model against the real
 * rx_world (hardening lane LG). Host only; no graphics chip.
 *
 * The real World is built with the model's tree (P -> C1, C2; C1 -> G), the
 * model's budget (2 slots, 8 memory) and needs. Every reaction function is a
 * gate: it parks inside the function until the harness says "finish" (commit
 * a new, strictly increasing value) or "fail" (return an error). After every
 * operation the harness waits until the World is settled (every admitted
 * activation is parked inside its function, nothing pending) and compares
 * what the World shows with what the AS_BUILT model predicts.
 *
 * Observed through rx_world.h only: rx_world_footprint, rx_world_crumb,
 * rx_world_verify_crumbs, the RxStats block and each RxReaction's state,
 * rearm, parked, yield_left and commits fields (declared in rx_world.h, read
 * under the world lock). No src/runtime code is changed or shimmed.
 *
 * Parts:
 *  1. Exhaustive differential: every operation sequence of the AS_BUILT model
 *     up to --depth (default 6), outside publications, finishes, failures
 *     and clock ticks, replayed on a fresh World. Expect 0 mismatches.
 *  2. Harness mutants: the same comparison against the model with a leak and
 *     a double-commit fault switched on must find a mismatch (KILLED).
 *  3. Real-side checks on every settled step and at the end of each run:
 *     budget held == budget of the activations in their functions, zero at
 *     quiescence; no two commits of one reaction answer the same wake;
 *     rx_world_verify_crumbs == 0. Each checker has a shim mutation that
 *     must trip it.
 *  4. Directed replays of the model's counterexamples for the two known
 *     gaps (I2 cancel, I4 deadline) on the real World, reported as FINDINGS.
 *  --stress: concurrent publishers, clock ticks and observers against free
 *     running reactions (the ThreadSanitizer workload).
 * Output: human lines plus "RESULT key=value" lines for the receipt.
 */
#include "runtime/rx_world.h"
#include "world_explore.h"
#include "world_model.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- gate ---------------------------------------------------------------- */

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int in_fn[WM_N];
    int n_in_fn;
    int cmd[WM_N];              /* 1 finish, 2 fail */
    int drain;                  /* functions return at once (commit) */
    int free_run;               /* --stress: never park */
} Gate;

static Gate g = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, {0}, 0, {0}, 0, 0 };
static uint64_t g_value;        /* atomic; strictly increasing outputs */
static RxObjRef g_out[WM_N];
static uint32_t g_idx[WM_N] = { 0, 1, 2, 3 };

static int fn_gate(RxCtx *ctx) {
    uint32_t i = *(const uint32_t *)ctx->user;
    int c = 1;
    pthread_mutex_lock(&g.mu);
    if (g.free_run) {
        uint64_t v = __atomic_add_fetch(&g_value, 1, __ATOMIC_SEQ_CST);
        c = (v % 7u == 0) ? 2 : 1;
    } else if (!g.drain) {
        g.in_fn[i] = 1;
        g.n_in_fn++;
        while (!g.cmd[i] && !g.drain) pthread_cond_wait(&g.cv, &g.mu);
        if (g.cmd[i]) c = g.cmd[i];
        g.cmd[i] = 0;
        g.in_fn[i] = 0;
        g.n_in_fn--;
    }
    pthread_mutex_unlock(&g.mu);
    if (c == 2) return -1;
    uint64_t v = __atomic_add_fetch(&g_value, 1, __ATOMIC_SEQ_CST);
    ctx->n_out = 1;
    ctx->out[0].obj = g_out[i];
    ctx->out[0].field = 0;
    ctx->out[0].value = v;
    return 0;
}

/* ---- authority stand-in: every reference is valid ------------------------ */

static int auth_ok(const void *c, RxCapRef ref, uint32_t subject, uint64_t resource,
                   uint32_t rights, RxCapEntry *out) {
    (void)c; (void)ref; (void)subject; (void)resource; (void)rights;
    if (out) memset(out, 0, sizeof *out);
    return RX_CAP_OK;
}
static int auth_inspect(const void *c, RxCapRef ref, RxCapEntry *out) {
    (void)c; (void)ref; (void)out;
    return RX_CAP_ERR_STATE;
}
static int g_auth_ctx;

/* ---- real World ---------------------------------------------------------- */

typedef struct {
    RxWorld *w;
    RxObjRef e;
    uint8_t tick;
    uint32_t slots;
    uint64_t mem;
} Real;

static void set_budget(Real *R) {
    RxResourceBudget b;
    memset(&b, 0, sizeof b);
    b.slots = R->slots;
    b.memory_bytes = R->mem;
    b.energy_budget = UINT64_MAX;
    b.offered_locality = UINT32_MAX;
    b.offered_accel = UINT32_MAX;
    b.compute_mask = UINT32_MAX;
    b.logical_tick = R->tick;
    rx_world_set_resources(R->w, &b);
}

static int real_start(Real *R, uint32_t workers, uint32_t slots, uint64_t mem) {
    memset(R, 0, sizeof *R);
    R->slots = slots;
    R->mem = mem;
    R->w = calloc(1, sizeof *R->w);
    if (!R->w) return -1;
    if (rx_world_init_with_auth(R->w, NULL, &g_auth_ctx, auth_ok, auth_inspect, workers,
                                1u << 14) != RX_OK) {
        free(R->w);
        return -1;
    }
    R->w->external_subject = 100;
    set_budget(R);
    if (rx_world_create(R->w, 1, RX_PERSIST_RESIDENT, 1, NULL, &R->e) != RX_OK) return -1;
    for (uint32_t i = 0; i < WM_N; i++)
        if (rx_world_create(R->w, 1, RX_PERSIST_RESIDENT, 1, NULL, &g_out[i]) != RX_OK) return -1;
    for (uint32_t i = 0; i < WM_N; i++) {
        RxReactionDesc d;
        memset(&d, 0, sizeof d);
        d.name = wm_name[i];
        d.faculty = i + 1;
        d.subject = 1;
        d.priority = RX_PRIO_FOREGROUND;
        d.need.memory_bytes = wm_mem[i];
        d.need.deadline = wm_deadline[i];
        d.n_triggers = 1;
        d.triggers[0].obj = wm_parent[i] < 0 ? R->e : g_out[wm_parent[i]];
        d.triggers[0].mask = RX_FIELD(0);
        d.n_writes = 1;
        d.writes[0].obj = g_out[i];
        d.writes[0].mask = RX_FIELD(0);
        d.n_caps = 1;
        d.caps[0].ref = (RxCapRef){ 1, 1 };
        d.caps[0].resource = 1;
        d.caps[0].rights = RX_RIGHT_READ | RX_RIGHT_WRITE;
        d.fn = fn_gate;
        d.user = &g_idx[i];
        uint32_t rid = UINT32_MAX;
        if (rx_world_add_reaction(R->w, &d, &rid) != RX_OK || rid != i) return -1;
    }
    return 0;
}

static uint64_t g_ext_value;

static int real_ext(Real *R) {
    RxMutation m = { R->e, 0, ++g_ext_value };
    return rx_world_publish_external(R->w, (RxCapRef){ 1, 1 }, &m, 1) > 0 ? 0 : -1;
}

static int real_release(uint32_t r, int how) {
    pthread_mutex_lock(&g.mu);
    int ok = g.in_fn[r];
    if (ok) g.cmd[r] = how;
    pthread_cond_broadcast(&g.cv);
    pthread_mutex_unlock(&g.mu);
    return ok ? 0 : -1;
}

/* ---- observation ----------------------------------------------------------- */

enum { CL_IDLE = 0, CL_WAITING = 1, CL_RUN = 2, CL_PARKED = 3, CL_OTHER = 9 };
typedef struct {
    uint8_t cls[WM_N], rearm[WM_N], yield[WM_N];
    uint32_t commits[WM_N];
    uint32_t used_slots, in_flight;
    uint64_t used_mem, overdue, invalidations, failed;
    int in_fn[WM_N];
} Obs;

static const char *cls_name(int c) {
    switch (c) {
    case CL_IDLE: return "IDLE"; case CL_WAITING: return "WAITING";
    case CL_RUN: return "RUN"; case CL_PARKED: return "PARKED"; default: return "OTHER";
    }
}

static void model_obs(const WmState *s, Obs *o) {
    memset(o, 0, sizeof *o);
    for (uint32_t r = 0; r < WM_N; r++) {
        o->cls[r] = s->st[r] == WM_RUN ? CL_RUN : s->st[r] == WM_WAITING ? CL_WAITING
                  : s->st[r] == WM_IDLE ? (s->parked[r] ? CL_PARKED : CL_IDLE) : CL_OTHER;
        o->rearm[r] = s->rearm[r];
        o->yield[r] = s->yield[r];
        o->commits[r] = s->commits[r];
        o->in_fn[r] = s->st[r] == WM_RUN;
    }
    o->used_slots = s->used_slots;
    o->used_mem = s->used_mem;
    o->in_flight = s->in_flight;
    o->overdue = s->overdue;
    o->invalidations = s->invalidations;
    o->failed = s->failures;
}

static int g_real_leak_shim;    /* checker self-test: pretend one slot leaked */

/* Wait until settled, then capture. Lock order: world, then gate (the gate
 * never takes the world lock), so the two counts are one consistent cut. */
static int settle(Real *R, Obs *o) {
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        RxWorld *w = R->w;
        pthread_mutex_lock(&w->mu);
        pthread_mutex_lock(&g.mu);
        int pending = 0;
        for (uint32_t r = 0; r < WM_N; r++) pending |= g.cmd[r];
        int ok = !pending && w->in_flight == (uint32_t)g.n_in_fn && w->deferred_len == 0;
        if (ok) {
            memset(o, 0, sizeof *o);
            for (uint32_t r = 0; r < WM_N; r++) {
                const RxReaction *x = &w->reactions[r];
                o->cls[r] = x->state == RX_DORMANT ? (x->parked ? CL_PARKED : CL_IDLE)
                          : x->state == RX_BLOCKED_RESOURCE ? CL_WAITING
                          : x->state == RX_RUNNING ? CL_RUN : CL_OTHER;
                o->rearm[r] = x->rearm;
                o->yield[r] = (uint8_t)x->yield_left;
                o->commits[r] = (uint32_t)x->commits;
                o->in_fn[r] = g.in_fn[r];
            }
            o->used_slots = w->used_slots + (g_real_leak_shim ? 1u : 0u);
            o->used_mem = w->used_memory;
            o->in_flight = w->in_flight;
            o->overdue = w->stats.deadline_overdue;
            o->invalidations = w->stats.invalidations;
            o->failed = w->stats.failed;
        }
        pthread_mutex_unlock(&g.mu);
        pthread_mutex_unlock(&w->mu);
        if (ok) return 0;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - t0.tv_sec) * 1000000000L + (now.tv_nsec - t0.tv_nsec) > 3000000000L)
            return -1;
        struct timespec ts = { 0, 20000 };
        nanosleep(&ts, NULL);
    }
}

/* Real-side I1 on one settled cut: held budget == the activations parked in
 * their functions (each holds exactly its need). */
static int real_leak_check(const Obs *o) {
    uint32_t n = 0;
    uint64_t mem = 0;
    for (uint32_t r = 0; r < WM_N; r++)
        if (o->in_fn[r]) { n++; mem += wm_mem[r]; }
    return o->used_slots == n && o->used_mem == mem && o->in_flight == n ? 0 : -1;
}

static int obs_diff(const Obs *m, const Obs *r, char *buf, size_t n) {
    for (uint32_t i = 0; i < WM_N; i++) {
        if (m->cls[i] != r->cls[i])
            return snprintf(buf, n, "%s state model=%s real=%s", wm_name[i],
                            cls_name(m->cls[i]), cls_name(r->cls[i])), 1;
        if (m->rearm[i] != r->rearm[i])
            return snprintf(buf, n, "%s rearm model=%u real=%u", wm_name[i], m->rearm[i], r->rearm[i]), 1;
        if (m->yield[i] != r->yield[i])
            return snprintf(buf, n, "%s yield model=%u real=%u", wm_name[i], m->yield[i], r->yield[i]), 1;
        if (m->commits[i] != r->commits[i])
            return snprintf(buf, n, "%s commits model=%u real=%u", wm_name[i], m->commits[i], r->commits[i]), 1;
    }
#define D(f) if (m->f != r->f) return snprintf(buf, n, #f " model=%llu real=%llu", \
                 (unsigned long long)m->f, (unsigned long long)r->f), 1;
    D(used_slots) D(used_mem) D(in_flight) D(overdue) D(invalidations) D(failed)
#undef D
    return 0;
}

/* Real-side I3 over the crumb log: no two COMMIT crumbs of one reaction share
 * a wake cause. `dup_shim` appends a copy of the first commit (self-test). */
static int commit_unique_check(RxWorld *w, int dup_shim, uint64_t *n_commits) {
    uint64_t n = w->n_crumbs;
    uint64_t cap = n + 1;
    uint32_t *rx = malloc(sizeof(uint32_t) * cap);
    uint64_t *cause = malloc(sizeof(uint64_t) * cap);
    if (!rx || !cause) { free(rx); free(cause); return -1; }
    uint64_t k = 0;
    for (uint64_t id = 1; id <= n; id++) {
        const RxCrumb *c = rx_world_crumb(w, id);
        if (c && c->kind == RX_CRUMB_COMMIT) { rx[k] = c->reaction; cause[k] = c->wake_cause; k++; }
    }
    if (dup_shim && k) { rx[k] = rx[0]; cause[k] = cause[0]; k++; }
    int bad = 0;
    for (uint64_t i = 0; i < k && !bad; i++)
        for (uint64_t j = i + 1; j < k; j++)
            if (rx[i] == rx[j] && cause[i] == cause[j]) { bad = 1; break; }
    if (n_commits) *n_commits = k;
    free(rx);
    free(cause);
    return bad ? -1 : 0;
}

static void gate_reset(void) {
    pthread_mutex_lock(&g.mu);
    memset(g.in_fn, 0, sizeof g.in_fn);
    memset(g.cmd, 0, sizeof g.cmd);
    g.n_in_fn = 0;
    g.drain = 0;
    pthread_mutex_unlock(&g.mu);
}

/* Drain, wait quiescent, run end-of-run checks, destroy. */
typedef struct { int quiesce_fail, leak_fail, dup_fail, verify_fail; } EndCheck;
static void real_finish(Real *R, EndCheck *ec) {
    pthread_mutex_lock(&g.mu);
    g.drain = 1;
    pthread_cond_broadcast(&g.cv);
    pthread_mutex_unlock(&g.mu);
    if (rx_world_wait_quiescent(R->w, 5000) != RX_OK) ec->quiesce_fail++;
    RxFootprint f;
    rx_world_footprint(R->w, &f);
    if (f.used_slots || f.used_memory || f.used_energy || f.in_flight) ec->leak_fail++;
    if (commit_unique_check(R->w, 0, NULL) != 0) ec->dup_fail++;
    uint64_t checked = 0;
    if (rx_world_verify_crumbs(R->w, &checked) != 0) ec->verify_fail++;
    rx_world_destroy(R->w);
    free(R->w);
    R->w = NULL;
    gate_reset();
}

/* ---- one replay ------------------------------------------------------------ */

typedef struct {
    uint64_t runs, steps, mismatches, real_leak, harness_err;
    EndCheck end;
    char first[512];
} DiffStats;

static void trace_str(const WmOp *t, int n, char *buf, size_t cap) {
    size_t at = 0;
    buf[0] = 0;
    for (int i = 0; i < n && at + 24 < cap; i++) {
        char b[32];
        wm_op_str(t[i], b, sizeof b);
        at += (size_t)snprintf(buf + at, cap - at, "%s%s", i ? " -> " : "", b);
    }
}

/* Returns 1 on a model/real mismatch. */
static int replay(const WmProfile *p, const WmOp *t, int n, DiffStats *ds) {
    Real R;
    gate_reset();
    if (real_start(&R, WM_N, WM_SLOTS, WM_MEM) != 0) { ds->harness_err++; return 0; }
    WmState s;
    wm_init(&s);
    int mismatch = 0;
    Obs mo, ro;
    char why[200] = "";
    for (int i = 0; i < n && !mismatch; i++) {
        wm_step(p, &s, t[i]);
        int rc = 0;
        switch (t[i].kind) {
        case OP_EXT:  rc = real_ext(&R); break;
        case OP_TICK: R.tick++; set_budget(&R); break;
        case OP_FIN:  rc = real_release(t[i].r, 1); break;
        case OP_FAIL: rc = real_release(t[i].r, 2); break;
        default: break;
        }
        if (rc != 0 || settle(&R, &ro) != 0) {
            mismatch = 1;
            snprintf(why, sizeof why, "step %d: real World did not accept/settle (rc=%d)", i + 1, rc);
            break;
        }
        ds->steps++;
        if (real_leak_check(&ro) != 0) ds->real_leak++;
        model_obs(&s, &mo);
        if (obs_diff(&mo, &ro, why, sizeof why)) {
            mismatch = 1;
            char w2[200];
            snprintf(w2, sizeof w2, "after step %d: %s", i + 1, why);
            memcpy(why, w2, sizeof why);
        }
    }
    if (mismatch && !ds->first[0]) {
        char tb[300];
        trace_str(t, n, tb, sizeof tb);
        snprintf(ds->first, sizeof ds->first, "[%s] %s", tb, why);
    }
    if (mismatch) ds->mismatches++;
    ds->runs++;
    real_finish(&R, &ds->end);
    return mismatch;
}

/* Every sequence of the AS_BUILT model up to `depth` (no CANCEL: rx_world has
 * no cancel entry point; see the directed replay). Replays leaves only;
 * every prefix is compared along the way. */
static int g_stop_on_first;
static void enumerate(const WmProfile *p, WmState *s, WmOp *t, int at, int depth, DiffStats *ds) {
    if (g_stop_on_first && ds->mismatches) return;
    WmOp ops[16];
    int nops = wm_all_ops(ops);
    int any = 0;
    if (at < depth) {
        for (int o = 0; o < nops; o++) {
            if (ops[o].kind == OP_CANCEL || !wm_enabled(p, s, ops[o])) continue;
            any = 1;
            WmState c = *s;
            wm_step(p, &c, ops[o]);
            t[at] = ops[o];
            enumerate(p, &c, t, at + 1, depth, ds);
        }
    }
    if (!any && at > 0) replay(p, t, at, ds);
}

/* ---- directed replays of the model's counterexamples ---------------------- */

static int directed_cancel(void) {
    WxResult xr;
    wx_explore(&wm_as_built, &xr);
    int b = wx_bit_index(V_CANCEL_CHILD);
    if (xr.trace_len[b] <= 0) { printf("RESULT real.I2.directed=NOT_RUN\n"); return -1; }
    WmOp *t = xr.trace[b];
    int n = xr.trace_len[b];
    char tb[300];
    trace_str(t, n, tb, sizeof tb);
    Real R;
    gate_reset();
    if (real_start(&R, WM_N, WM_SLOTS, WM_MEM) != 0) return -1;
    WmState spec;
    wm_init(&spec);
    Obs ro;
    int remove_rc = 0, removed_busy = 0, still = 0;
    uint32_t target = 0;
    char who[64] = "";
    for (int i = 0; i < n; i++) {
        wm_step(&wm_spec, &spec, t[i]);
        switch (t[i].kind) {
        case OP_EXT:  real_ext(&R); break;
        case OP_TICK: R.tick++; set_budget(&R); break;
        case OP_FIN:  real_release(t[i].r, 1); break;
        case OP_FAIL: real_release(t[i].r, 2); break;
        case OP_CANCEL:
            /* No cancel entry point. The nearest public call, removal, is
             * tried on every busy member of the subtree. */
            target = t[i].r;
            settle(&R, &ro);
            for (uint32_t x = 0; x < WM_N; x++)
                if ((x == target || wm_is_descendant(x, target)) && ro.cls[x] != CL_IDLE) {
                    remove_rc = rx_world_remove_reaction(R.w, x);
                    if (remove_rc == RX_ERR_BUSY) removed_busy++;
                }
            break;
        }
        settle(&R, &ro);
    }
    uint32_t child = WM_N;
    for (uint32_t x = 0; x < WM_N; x++)
        if (wm_is_descendant(x, target) && ro.cls[x] != CL_IDLE) {
            still++;
            size_t l = strlen(who);
            snprintf(who + l, sizeof who - l, "%s%s=%s", l ? "," : "", wm_name[x], cls_name(ro.cls[x]));
            if (child == WM_N && ro.cls[x] == CL_RUN) child = x;
        }
    uint32_t commits_before = child < WM_N ? ro.commits[child] : 0, commits_after = 0;
    if (child < WM_N) {
        real_release(child, 1);
        settle(&R, &ro);
        commits_after = ro.commits[child];
    }
    int spec_idle = 1;
    for (uint32_t x = 0; x < WM_N; x++)
        if ((x == target || wm_is_descendant(x, target)) && spec.st[x] != WM_IDLE) spec_idle = 0;
    printf("FINDING I2 cancel does not reach children (KNOWN_FAIL)\n");
    printf("  trace: %s\n", tb);
    printf("  SPEC model after cancel(%s): whole subtree idle=%s, budget slots held=%u\n",
           wm_name[target], spec_idle ? "yes" : "no", spec.used_slots);
    printf("  real rx_world: no cancel entry point (RX_CANCELLED is never entered);"
           " rx_world_remove_reaction on busy members returned %d (%d x RX_ERR_BUSY)\n",
           remove_rc, removed_busy);
    printf("  real rx_world after the cancel: children still in flight: %s\n", who);
    if (child < WM_N)
        printf("  continuation FIN(%s): real commits %u -> %u (child published after its parent was cancelled)\n",
               wm_name[child], commits_before, commits_after);
    EndCheck ec = { 0, 0, 0, 0 };
    real_finish(&R, &ec);
    int reproduced = still > 0 && spec_idle;
    printf("RESULT real.I2.directed=%s\n", reproduced ? "KNOWN_FAIL" : "NOT_REPRODUCED");
    printf("RESULT real.I2.counterexample=%s\n", tb);
    printf("RESULT real.I2.children_in_flight_after_cancel=%s\n", who[0] ? who : "none");
    printf("RESULT real.I2.child_commit_after_cancel=%s\n",
           child < WM_N && commits_after > commits_before ? "YES" : "NO");
    return reproduced ? 0 : -1;
}

static int directed_deadline(void) {
    WxResult xr;
    wx_explore(&wm_as_built, &xr);
    int b = wx_bit_index(V_DEADLINE);
    if (xr.trace_len[b] <= 0) { printf("RESULT real.I4.directed=NOT_RUN\n"); return -1; }
    WmOp *t = xr.trace[b];
    int n = xr.trace_len[b];
    char tb[300];
    trace_str(t, n, tb, sizeof tb);
    Real R;
    gate_reset();
    if (real_start(&R, WM_N, WM_SLOTS, WM_MEM) != 0) return -1;
    WmState spec;
    wm_init(&spec);
    Obs ro;
    for (int i = 0; i < n; i++) {
        wm_step(&wm_spec, &spec, t[i]);
        switch (t[i].kind) {
        case OP_EXT:  real_ext(&R); break;
        case OP_TICK: R.tick++; set_budget(&R); break;
        case OP_FIN:  real_release(t[i].r, 1); break;
        case OP_FAIL: real_release(t[i].r, 2); break;
        default: break;
        }
        settle(&R, &ro);
    }
    uint32_t late = WM_N;
    for (uint32_t x = 0; x < WM_N; x++)
        if (ro.cls[x] == CL_RUN && wm_deadline[x] && R.tick > wm_deadline[x]) { late = x; break; }
    uint64_t real_overdue = ro.overdue;
    uint32_t c0 = late < WM_N ? ro.commits[late] : 0, c1 = 0;
    if (late < WM_N) {
        real_release(late, 1);
        settle(&R, &ro);
        c1 = ro.commits[late];
    }
    printf("FINDING I4 deadline overrun is not surfaced while running, and never enforced (KNOWN_FAIL)\n");
    printf("  trace: %s\n", tb);
    if (late < WM_N)
        printf("  %s (deadline %u) still running at tick %u: SPEC surfaced overruns=%u, real deadline_overdue=%llu\n",
               wm_name[late], wm_deadline[late], R.tick, spec.overdue, (unsigned long long)real_overdue);
    printf("  continuation FIN: real commits %u -> %u (late work still publishes)\n", c0, c1);
    EndCheck ec = { 0, 0, 0, 0 };
    real_finish(&R, &ec);
    int reproduced = late < WM_N && spec.overdue > real_overdue;
    printf("RESULT real.I4.directed=%s\n", reproduced ? "KNOWN_FAIL" : "NOT_REPRODUCED");
    printf("RESULT real.I4.counterexample=%s\n", tb);
    printf("RESULT real.I4.spec_overdue=%u\n", spec.overdue);
    printf("RESULT real.I4.real_deadline_overdue=%llu\n", (unsigned long long)real_overdue);
    printf("RESULT real.I4.late_commit_published=%s\n", c1 > c0 ? "YES" : "NO");
    return reproduced ? 0 : -1;
}

/* ---- checker self-tests (shim mutations) ---------------------------------- */

static int self_tests(void) {
    int bad = 0;
    /* Leak checker: one settled run with the observation shim on must trip. */
    WmOp t[2] = { { OP_EXT, 0 }, { OP_FIN, WM_P } };
    DiffStats ds;
    memset(&ds, 0, sizeof ds);
    g_real_leak_shim = 1;
    replay(&wm_as_built, t, 2, &ds);
    g_real_leak_shim = 0;
    int leak_killed = ds.real_leak > 0;
    printf("RESULT shim.real_leak_check=%s\n", leak_killed ? "KILLED" : "SURVIVED");
    bad += !leak_killed;
    /* Duplicate-commit checker: a copied COMMIT crumb must trip it. */
    Real R;
    gate_reset();
    int dup_killed = 0, clean_ok = 0;
    if (real_start(&R, WM_N, WM_SLOTS, WM_MEM) == 0) {
        Obs ro;
        real_ext(&R);
        settle(&R, &ro);
        real_release(WM_P, 1);
        settle(&R, &ro);
        uint64_t k = 0;
        clean_ok = commit_unique_check(R.w, 0, &k) == 0 && k > 0;
        dup_killed = commit_unique_check(R.w, 1, NULL) != 0;
        EndCheck ec = { 0, 0, 0, 0 };
        real_finish(&R, &ec);
    }
    printf("RESULT shim.commit_unique_check=%s\n", dup_killed && clean_ok ? "KILLED" : "SURVIVED");
    bad += !(dup_killed && clean_ok);
    return bad;
}

/* ---- stress (ThreadSanitizer workload) ------------------------------------ */

typedef struct { Real *R; int iters; int stop; } StressCtx;

static void *stress_pub(void *a) {
    StressCtx *c = a;
    for (int i = 0; i < c->iters; i++) {
        RxMutation m = { c->R->e, 0, __atomic_add_fetch(&g_ext_value, 1, __ATOMIC_SEQ_CST) };
        rx_world_publish_external(c->R->w, (RxCapRef){ 1, 1 }, &m, 1);
    }
    return NULL;
}
static void *stress_tick(void *a) {
    StressCtx *c = a;
    for (int i = 0; i < 200; i++) {
        RxResourceBudget b;
        memset(&b, 0, sizeof b);
        b.slots = 3; b.memory_bytes = 16; b.energy_budget = UINT64_MAX;
        b.offered_locality = b.offered_accel = b.compute_mask = UINT32_MAX;
        b.logical_tick = (uint64_t)i;
        rx_world_set_resources(c->R->w, &b);
    }
    return NULL;
}
static void *stress_observe(void *a) {
    StressCtx *c = a;
    while (!__atomic_load_n(&c->stop, __ATOMIC_SEQ_CST)) {
        RxFootprint f;
        RxObject o;
        uint8_t d[32];
        rx_world_footprint(c->R->w, &f);
        rx_world_read(c->R->w, c->R->e, &o);
        rx_world_digest(c->R->w, d);
    }
    return NULL;
}

static int stress(int iters) {
    Real R;
    gate_reset();
    pthread_mutex_lock(&g.mu);
    g.free_run = 1;
    pthread_mutex_unlock(&g.mu);
    if (real_start(&R, 8, 3, 16) != 0) { printf("RESULT stress=ERROR\n"); return 1; }
    StressCtx c = { &R, iters, 0 };
    pthread_t pub[3], tick, obs;
    for (int i = 0; i < 3; i++) pthread_create(&pub[i], NULL, stress_pub, &c);
    pthread_create(&tick, NULL, stress_tick, &c);
    pthread_create(&obs, NULL, stress_observe, &c);
    for (int i = 0; i < 3; i++) pthread_join(pub[i], NULL);
    pthread_join(tick, NULL);
    int q = rx_world_wait_quiescent(R.w, 20000);
    __atomic_store_n(&c.stop, 1, __ATOMIC_SEQ_CST);
    pthread_join(obs, NULL);
    RxFootprint f;
    rx_world_footprint(R.w, &f);
    uint64_t commits = 0, checked = 0;
    int dup = commit_unique_check(R.w, 0, &commits);
    int ver = rx_world_verify_crumbs(R.w, &checked);
    pthread_mutex_lock(&R.w->mu);
    RxStats st = R.w->stats;
    pthread_mutex_unlock(&R.w->mu);
    int ok = q == RX_OK && !f.used_slots && !f.used_memory && !f.in_flight && dup == 0 && ver == 0;
    printf("[*] stress: %d x 3 publications, 200 clock ticks, observer thread: quiescent=%s "
           "commits=%llu failed=%llu invalidations=%llu crumbs_verified=%llu held_at_rest=%u/%llu\n",
           iters, q == RX_OK ? "yes" : "NO", (unsigned long long)commits,
           (unsigned long long)st.failed, (unsigned long long)st.invalidations,
           (unsigned long long)checked, f.used_slots, (unsigned long long)f.used_memory);
    printf("RESULT stress.publications=%d\n", iters * 3);
    printf("RESULT stress.commits=%llu\n", (unsigned long long)commits);
    printf("RESULT stress.no_leak_at_rest=%s\n", !f.used_slots && !f.used_memory && !f.in_flight ? "PASS" : "FAIL");
    printf("RESULT stress.commit_unique=%s\n", dup == 0 ? "PASS" : "FAIL");
    printf("RESULT stress.crumbs_verify=%s\n", ver == 0 ? "PASS" : "FAIL");
    printf("RESULT stress=%s\n", ok ? "PASS" : "FAIL");
    EndCheck ec = { 0, 0, 0, 0 };
    real_finish(&R, &ec);
    pthread_mutex_lock(&g.mu);
    g.free_run = 0;
    pthread_mutex_unlock(&g.mu);
    return ok ? 0 : 1;
}

/* ---- main ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    int depth = 6, do_stress = 0, stress_iters = 3000, only_stress = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--depth") && i + 1 < argc) depth = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stress")) do_stress = 1;
        else if (!strcmp(argv[i], "--stress-only")) do_stress = only_stress = 1;
        else if (!strcmp(argv[i], "--stress-iters") && i + 1 < argc) stress_iters = atoi(argv[++i]);
        else { fprintf(stderr, "usage: %s [--depth N] [--stress|--stress-only] [--stress-iters N]\n", argv[0]); return 2; }
    }
    int bad = 0;
    if (!only_stress) {
        WmOp t[64];
        WmState s;
        DiffStats ds;
        memset(&ds, 0, sizeof ds);
        wm_init(&s);
        enumerate(&wm_as_built, &s, t, 0, depth, &ds);
        printf("[*] differential AS_BUILT vs rx_world, depth %d: %llu runs, %llu settled steps, "
               "%llu mismatches\n", depth, (unsigned long long)ds.runs,
               (unsigned long long)ds.steps, (unsigned long long)ds.mismatches);
        if (ds.first[0]) printf("  first mismatch: %s\n", ds.first);
        printf("RESULT diff.depth=%d\n", depth);
        printf("RESULT diff.runs=%llu\n", (unsigned long long)ds.runs);
        printf("RESULT diff.steps=%llu\n", (unsigned long long)ds.steps);
        printf("RESULT diff.mismatches=%llu\n", (unsigned long long)ds.mismatches);
        printf("RESULT real.I1.settled_leak_violations=%llu\n", (unsigned long long)ds.real_leak);
        printf("RESULT real.I1.quiescent_leak_violations=%d\n", ds.end.leak_fail);
        printf("RESULT real.I3.duplicate_commit_runs=%d\n", ds.end.dup_fail);
        printf("RESULT real.crumbs_verify_failures=%d\n", ds.end.verify_fail);
        printf("RESULT real.quiesce_failures=%d\n", ds.end.quiesce_fail);
        printf("RESULT diff.harness_errors=%llu\n", (unsigned long long)ds.harness_err);
        int diff_ok = ds.runs > 0 && ds.mismatches == 0 && ds.real_leak == 0 && ds.end.leak_fail == 0 &&
                      ds.end.dup_fail == 0 && ds.end.verify_fail == 0 && ds.end.quiesce_fail == 0 &&
                      ds.harness_err == 0;
        printf("RESULT diff.verdict=%s\n", diff_ok ? "PASS" : "FAIL");
        bad += !diff_ok;

        /* Harness mutants: AS_BUILT plus one fault; the comparison must notice. */
        const WmProfile mut_leak = { "DIFF_MUT_LEAK_ON_FAIL", CANCEL_NONE, 0, 1, 0 };
        const WmProfile mut_double = { "DIFF_MUT_COMMIT_KEEPS_ACTIVATION", CANCEL_NONE, 0, 0, 1 };
        const WmProfile *muts[2] = { &mut_leak, &mut_double };
        g_stop_on_first = 1;
        for (int m = 0; m < 2; m++) {
            DiffStats dm;
            memset(&dm, 0, sizeof dm);
            wm_init(&s);
            enumerate(muts[m], &s, t, 0, depth < 4 ? depth : 4, &dm);
            int killed = dm.mismatches > 0;
            printf("RESULT diff.%s=%s\n", muts[m]->name, killed ? "KILLED" : "SURVIVED");
            if (killed) printf("  %s killed: %s\n", muts[m]->name, dm.first);
            bad += !killed;
        }
        g_stop_on_first = 0;
        bad += self_tests();
        if (directed_cancel() != 0) bad++;
        if (directed_deadline() != 0) bad++;
    }
    if (do_stress) bad += stress(stress_iters);
    printf("RESULT world_diff.verdict=%s\n", bad ? "FAIL" : "PASS");
    printf("WORLD DIFF: %s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
