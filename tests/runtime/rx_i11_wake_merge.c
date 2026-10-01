/* I11: a dependent popped while its upstream writer is still computing must
 * not commit early and then again (omega.risk behind aien.hypothesis, found
 * by the causal replay gate, omega PR #176).
 *
 * World: sensor{temp, humidity} -> aien.hypothesis -> belief, and omega.risk
 * triggered by sensor humidity and belief (the replay world's shape).
 *
 * The bad interleaving is forced, not hoped for: with 2 workers,
 * aien.hypothesis's fn does not return until the second worker has taken
 * omega.risk off the ready ring and either held it (fixed runtime) or run and
 * committed it (defect). Each stimulus therefore takes the same path every
 * run. Expected, as with 1 worker: per stimulus exactly one omega.risk
 * crumb, a COMMIT, no INVALIDATED crumb; when both fields change its wake is
 * the hypothesis commit with one merged wake, and it saw the new belief.
 * The 1-worker and 2-worker crumb signatures must be identical.
 *
 * Mutation: built with -DRX_WORLD_MUTATE_NO_UPSTREAM_HOLD the runtime has no
 * hold and this test must FAIL (make test-i11-wake-merge checks both). */
#include "runtime/rx_caproot.h"
#include "runtime/rx_world.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { SUBJ_AIEN = 1, SUBJ_OMEGA = 2, SUBJ_EXTERNAL = 100, ISSUER_AEGIS_POLICY = 3 };
enum { RES_SENSOR = 0x10, RES_BELIEF = 0x20, RES_RISK = 0x40 };
enum { F_TEMP = 0, F_HUMIDITY = 1 };
enum { STIMULI = 24 };

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail = 1; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

typedef struct {
    RxCapRoot root;
    RxCapAdmin admin;
    RxWorld w;
    RxObjRef sensor, belief, risk;
    uint32_t id_hyp, id_risk;
    RxCapRef c_ext;
    /* gate for the forced interleaving (2-worker run, both fields changed) */
    int gate_on;
    uint64_t risk_commits0;
    int gate_timeout;
} Env;

static const RxSnapshotDep *in_of(const RxCtx *c, RxObjRef o) {
    for (uint32_t i = 0; i < c->n_in; i++)
        if (c->in[i].obj.id == o.id) return &c->in[i];
    return NULL;
}

static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

/* Runs outside the world lock. Wait until the other worker has popped
 * omega.risk: held (READY, off the ring) or already run and committed. */
static void gate(Env *e) {
    if (!e->gate_on) return;
    uint64_t limit = now_ms() + 5000;
    for (;;) {
        pthread_mutex_lock(&e->w.mu);
        const RxReaction *r = &e->w.reactions[e->id_risk];
        uint32_t p = r->desc.priority;
        int off_ring = e->w.ready_len[p] == 0;
        int held = off_ring && r->state == RX_READY;
        int ran = r->commits > e->risk_commits0 && r->state != RX_RUNNING &&
                  r->state != RX_PUBLISHING;
        pthread_mutex_unlock(&e->w.mu);
        if (held || ran) return;
        if (now_ms() > limit) { e->gate_timeout = 1; return; }
        sched_yield();
    }
}

static int fn_hypothesis(RxCtx *c) {
    Env *e = c->user;
    const RxSnapshotDep *s = in_of(c, e->sensor);
    if (!s) return -1;
    gate(e);
    c->out[c->n_out++] = (RxMutation){ e->belief, 0, s->field[F_TEMP] * 2 + 1 };
    return 0;
}

static int fn_risk(RxCtx *c) {
    Env *e = c->user;
    const RxSnapshotDep *s = in_of(c, e->sensor);
    const RxSnapshotDep *b = in_of(c, e->belief);
    if (!s || !b) return -1;
    c->out[c->n_out++] = (RxMutation){ e->risk, 0, s->field[F_HUMIDITY] * 100000 + b->field[0] };
    return 0;
}

static RxCapRef mint(Env *e, uint32_t subject, uint64_t resource, uint32_t rights) {
    RxCapMint m;
    memset(&m, 0, sizeof m);
    m.issuer = ISSUER_AEGIS_POLICY;
    m.subject = subject;
    m.resource = resource;
    m.rights = rights;
    m.parent = (RxCapRef){ UINT32_MAX, 0 };
    m.authority = rx_capadmin_office(&e->admin);
    RxCapRef r = { UINT32_MAX, 0 };
    if (rx_capadmin_mint(&e->admin, &m, &r) != RX_CAP_OK) fprintf(stderr, "mint failed\n");
    return r;
}

static RxObjRef mkobj(Env *e, uint64_t resource) {
    uint64_t init[RX_MAX_FIELDS] = { 0 };
    RxObjRef r = { UINT32_MAX, 0 };
    rx_world_create(&e->w, 1, RX_PERSIST_RESIDENT, resource, init, &r);
    return r;
}

static void desc(RxReactionDesc *d, const char *name, uint32_t fac, uint32_t subj, RxFn fn, void *u) {
    memset(d, 0, sizeof *d);
    d->name = name;
    d->faculty = fac;
    d->subject = subj;
    d->priority = RX_PRIO_FOREGROUND;
    d->fn = fn;
    d->user = u;
}
static void trig(RxReactionDesc *d, RxObjRef o, uint64_t m) { d->triggers[d->n_triggers++] = (RxDep){ o, m }; }
static void wr(RxReactionDesc *d, RxObjRef o, uint64_t m) { d->writes[d->n_writes++] = (RxDep){ o, m }; }
static void cap(RxReactionDesc *d, RxCapRef c, uint64_t res, uint32_t rights) {
    d->caps[d->n_caps++] = (RxCapNeed){ c, res, rights };
}

static int setup(Env *e, uint32_t workers) {
    memset(e, 0, sizeof *e);
    if (rx_caproot_start(&e->root, &e->admin) != RX_CAP_OK) return -1;
    if (rx_world_init(&e->w, &e->root, workers, 1u << 16) != RX_OK) return -1;
    e->w.external_subject = SUBJ_EXTERNAL;
    e->sensor = mkobj(e, RES_SENSOR);
    e->belief = mkobj(e, RES_BELIEF);
    e->risk = mkobj(e, RES_RISK);
    e->c_ext = mint(e, SUBJ_EXTERNAL, RES_SENSOR, RX_RIGHT_WRITE);
    RxReactionDesc d;
    desc(&d, "aien.hypothesis", RX_FACULTY_AIEN, SUBJ_AIEN, fn_hypothesis, e);
    trig(&d, e->sensor, RX_FIELD(F_TEMP));
    wr(&d, e->belief, RX_FIELD(0));
    cap(&d, mint(e, SUBJ_AIEN, RES_SENSOR, RX_RIGHT_READ), RES_SENSOR, RX_RIGHT_READ);
    cap(&d, mint(e, SUBJ_AIEN, RES_BELIEF, RX_RIGHT_WRITE), RES_BELIEF, RX_RIGHT_WRITE);
    if (rx_world_add_reaction(&e->w, &d, &e->id_hyp) != RX_OK) return -1;
    desc(&d, "omega.risk", RX_FACULTY_OMEGA, SUBJ_OMEGA, fn_risk, e);
    trig(&d, e->sensor, RX_FIELD(F_HUMIDITY));
    trig(&d, e->belief, RX_FIELD(0));
    wr(&d, e->risk, RX_FIELD(0));
    cap(&d, mint(e, SUBJ_OMEGA, RES_SENSOR, RX_RIGHT_READ), RES_SENSOR, RX_RIGHT_READ);
    cap(&d, mint(e, SUBJ_OMEGA, RES_BELIEF, RX_RIGHT_READ), RES_BELIEF, RX_RIGHT_READ);
    cap(&d, mint(e, SUBJ_OMEGA, RES_RISK, RX_RIGHT_WRITE), RES_RISK, RX_RIGHT_WRITE);
    if (rx_world_add_reaction(&e->w, &d, &e->id_risk) != RX_OK) return -1;
    return 0;
}

static uint64_t field(Env *e, RxObjRef r, uint32_t f) {
    RxObject o;
    if (rx_world_read(&e->w, r, &o) != RX_OK) return UINT64_MAX;
    return o.field[f];
}

/* One crumb's schedule-independent signature, relative to the stimulus's
 * first crumb so the two runs compare. */
typedef struct { uint32_t who, kind; uint64_t cause_rel, coalesced, value; } Sig;

static Sig g_sig[2][STIMULI * 8];
static uint32_t g_nsig[2];

/* kinds: 0 temp only, 1 humidity only, 2 both. Fixed sequence. */
static uint32_t pick(uint32_t k) { return (k * 7u + 2u) % 3u; }

static void run(uint32_t workers, int slot) {
    Env *e = calloc(1, sizeof *e);
    if (!e || setup(e, workers)) { CHECK(0, "setup (%u workers)", workers); free(e); return; }
    uint32_t n_both = 0;
    for (uint32_t k = 0; k < STIMULI; k++) {
        uint32_t kind = pick(k);
        RxMutation m[2];
        uint32_t n = 0;
        uint64_t t = field(e, e->sensor, F_TEMP), h = field(e, e->sensor, F_HUMIDITY);
        if (kind != 1) m[n++] = (RxMutation){ e->sensor, F_TEMP, (t + 1 + k % 7) % 8 };
        if (kind != 0) m[n++] = (RxMutation){ e->sensor, F_HUMIDITY, (h + 1 + k % 5) % 8 };
        if (kind == 2) n_both++;
        pthread_mutex_lock(&e->w.mu);
        e->risk_commits0 = e->w.reactions[e->id_risk].commits;
        pthread_mutex_unlock(&e->w.mu);
        e->gate_on = workers > 1 && kind == 2;
        uint64_t first = e->w.n_crumbs + 1;
        int64_t rc = rx_world_publish_external(&e->w, e->c_ext, m, n);
        CHECK(rc > 0, "publish refused %lld", (long long)rc);
        CHECK(rx_world_wait_quiescent(&e->w, 10000) == RX_OK, "no quiescence");
        e->gate_on = 0;
        CHECK(!e->gate_timeout, "%u workers: forced interleaving never reached (stimulus %u)", workers, k);
        uint32_t risk_crumbs = 0;
        uint64_t hyp_commit = 0;
        for (uint64_t i = first; i <= e->w.n_crumbs; i++) {
            const RxCrumb *c = rx_world_crumb(&e->w, i);
            CHECK(c->kind != RX_CRUMB_INVALIDATED, "%u workers: INVALIDATED crumb %llu (stimulus %u)",
                  workers, (unsigned long long)i, k);
            if (c->reaction == e->id_hyp && c->kind == RX_CRUMB_COMMIT) hyp_commit = i;
            if (c->reaction == e->id_risk) {
                risk_crumbs++;
                CHECK(c->kind == RX_CRUMB_COMMIT, "%u workers: risk crumb %llu kind %u", workers,
                      (unsigned long long)i, (unsigned)c->kind);
                uint64_t want_coal = kind == 2 ? 1 : 0;
                CHECK(c->coalesced_wakes == want_coal,
                      "%u workers: stimulus %u (kind %u) risk merged %llu wakes, want %llu", workers, k,
                      kind, (unsigned long long)c->coalesced_wakes, (unsigned long long)want_coal);
                if (kind != 1)
                    CHECK(hyp_commit && c->wake_cause == hyp_commit,
                          "%u workers: stimulus %u risk woken by %llu, want hypothesis commit %llu",
                          workers, k, (unsigned long long)c->wake_cause, (unsigned long long)hyp_commit);
            }
            if (g_nsig[slot] < STIMULI * 8) {
                Sig *s = &g_sig[slot][g_nsig[slot]++];
                s->who = c->reaction == e->id_hyp ? 1 : c->reaction == e->id_risk ? 2 : 0;
                s->kind = (uint32_t)c->kind;
                s->cause_rel = c->wake_cause >= first ? c->wake_cause - first + 1 : 0;
                s->coalesced = c->coalesced_wakes;
                s->value = c->n_outputs ? c->outputs[0].version : 0;
            }
        }
        CHECK(risk_crumbs == 1, "%u workers: stimulus %u (kind %u): %u omega.risk crumbs, want 1",
              workers, k, kind, risk_crumbs);
        uint64_t want = field(e, e->sensor, F_HUMIDITY) * 100000 + field(e, e->belief, 0);
        CHECK(field(e, e->risk, 0) == want, "%u workers: stimulus %u risk %llu want %llu", workers, k,
              (unsigned long long)field(e, e->risk, 0), (unsigned long long)want);
    }
    printf("  %u worker(s): %u stimuli (%u both-field), omega.risk commits %llu, crumbs %llu\n", workers,
           STIMULI, n_both, (unsigned long long)e->w.reactions[e->id_risk].commits,
           (unsigned long long)e->w.n_crumbs);
    CHECK(e->w.reactions[e->id_risk].commits == STIMULI, "%u workers: omega.risk commits %llu, want %u",
          workers, (unsigned long long)e->w.reactions[e->id_risk].commits, STIMULI);
    uint64_t checked = 0;
    CHECK(rx_world_verify_crumbs(&e->w, &checked) == 0, "crumb chain verification failed");
    rx_world_destroy(&e->w);
    rx_caproot_stop(&e->root, &e->admin);
    free(e);
}

int main(void) {
#ifdef RX_WORLD_MUTATE_NO_UPSTREAM_HOLD
    printf("rx_i11_wake_merge: MUTANT build (no upstream hold)\n");
#endif
    run(1, 0);
    run(2, 1);
    CHECK(g_nsig[0] == g_nsig[1], "crumb count 1 worker %u vs 2 workers %u", g_nsig[0], g_nsig[1]);
    for (uint32_t i = 0; i < g_nsig[0] && i < g_nsig[1]; i++)
        if (memcmp(&g_sig[0][i], &g_sig[1][i], sizeof(Sig)) != 0) {
            CHECK(0, "crumb %u differs between 1 and 2 workers (who %u/%u kind %u/%u coalesced %llu/%llu)", i,
                  g_sig[0][i].who, g_sig[1][i].who, g_sig[0][i].kind, g_sig[1][i].kind,
                  (unsigned long long)g_sig[0][i].coalesced, (unsigned long long)g_sig[1][i].coalesced);
            break;
        }
    printf("rx_i11_wake_merge: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
