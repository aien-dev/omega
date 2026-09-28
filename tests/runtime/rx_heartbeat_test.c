/*
 * rx_heartbeat_test.c -- qualification suite for the first heartbeat of the
 * resident reaction runtime (ADR 0016; gates R3 and R4, host reference).
 *
 * The heartbeat this suite must show, with no central function sequencing it:
 *
 *   shared object -> dependency change -> reaction becomes ready ->
 *   capability validates -> reaction executes -> atomic publication ->
 *   causal crumb
 *
 * The test code only (a) sets up objects and capabilities, (b) injects
 * external stimuli, and (c) waits for quiescence and inspects. It never calls a
 * reaction. Ordering comes from the dependency index.
 *
 * Receipt: build/qual-runs/<run>/R3/rx_heartbeat_receipt.json. A recorded
 * receipt is committed separately as evidence/R3/<sha256-of-receipt>.json in an
 * evidence-only commit. Counts are observed, never literal. The candidate
 * commit is an input (OMEGA_CANDIDATE_COMMIT), not HEAD.
 */
#include "runtime/rx_caproot.h"
#include "runtime/rx_world.h"
#include "omega_evidence.h"
#include "sha256.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---- tiny check framework ----------------------------------------------- */

#define MAX_TESTS 48
typedef struct {
    const char *name;
    const char *invariants;
    int checks;
    int failures;
} TestRec;

static TestRec g_tests[MAX_TESTS];
static int g_ntests;
static TestRec *g_cur;
static uint64_t g_crumbs_verified;
static uint64_t g_crumbs_audited;
static uint64_t g_illegal_transitions;
static uint64_t g_transitions[RX_STATE_COUNT][RX_STATE_COUNT];

#define CHECK(cond, ...) do {                                            \
        g_cur->checks++;                                                 \
        if (!(cond)) {                                                   \
            g_cur->failures++;                                           \
            fprintf(stderr, "  FAIL %s:%d [%s] ", __FILE__, __LINE__, g_cur->name); \
            fprintf(stderr, __VA_ARGS__);                                \
            fputc('\n', stderr);                                         \
        }                                                                \
    } while (0)

static void begin(const char *name, const char *invariants) {
    g_cur = &g_tests[g_ntests++];
    g_cur->name = name;
    g_cur->invariants = invariants;
    printf("[*] %s\n", name);
}

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---- principals and resources ------------------------------------------- */

enum { SUBJ_AIEN = 1, SUBJ_OMEGA = 2, SUBJ_AEGIS = 3, SUBJ_EXTERNAL = 100, SUBJ_OTHER = 7 };
enum { ISSUER_AEGIS_POLICY = 3 };
enum {
    RES_SENSOR = 0x10, RES_BELIEF = 0x20, RES_PLAN = 0x30, RES_RISK = 0x40,
    RES_FAN_SRC = 0x50, RES_FAN_OUT = 0x51, RES_PAIR = 0x60, RES_TICK = 0x61,
    RES_CHECK = 0x62, RES_SCRATCH = 0x70
};
enum { F_TEMP = 0, F_HUMIDITY = 1 };

typedef struct {
    RxCapRoot root;
    RxCapAdmin admin;
    RxWorld w;
} Env;

static int env_start(Env *e, uint32_t workers) {
    if (rx_caproot_start(&e->root, &e->admin) != RX_CAP_OK) return -1;
    if (rx_world_init(&e->w, &e->root, workers, 1u << 18) != RX_OK) {
        rx_caproot_stop(&e->root, &e->admin);
        return -1;
    }
    e->w.external_subject = SUBJ_EXTERNAL;
    return 0;
}

static RxCapRef office_of(const RxCapAdmin *admin) { return rx_capadmin_office(admin); }

static RxCapRef mint(Env *e, uint32_t subject, uint64_t resource, uint32_t rights) {
    RxCapMint m;
    memset(&m, 0, sizeof(m));
    m.issuer = ISSUER_AEGIS_POLICY;
    m.subject = subject;
    m.resource = resource;
    m.rights = rights;
    m.parent = (RxCapRef){ UINT32_MAX, 0 };
    m.authority = office_of(&e->admin);
    RxCapRef r = { UINT32_MAX, 0 };
    int rc = rx_capadmin_mint(&e->admin, &m, &r);
    if (rc != RX_CAP_OK) fprintf(stderr, "mint failed: %s\n", rx_cap_strerror(rc));
    return r;
}

static int revoke_cap(RxCapAdmin *admin, RxCapRef ref) {
    return rx_capadmin_revoke(admin, office_of(admin), ref);
}

static RxObjRef mkobj(Env *e, uint64_t resource, uint64_t v0) {
    uint64_t init[RX_MAX_FIELDS] = { v0 };
    RxObjRef r = { UINT32_MAX, 0 };
    rx_world_create(&e->w, 1, RX_PERSIST_RESIDENT, resource, init, &r);
    return r;
}

static uint64_t field(Env *e, RxObjRef r, uint32_t f) {
    RxObject o;
    if (rx_world_read(&e->w, r, &o) != RX_OK) return UINT64_MAX;
    return o.field[f];
}

static int64_t stimulus(Env *e, RxCapRef cap, RxObjRef obj, uint32_t f, uint64_t v) {
    RxMutation m = { obj, f, v };
    return rx_world_publish_external(&e->w, cap, &m, 1);
}

static void desc_init(RxReactionDesc *d, const char *name, uint32_t faculty, uint32_t subject,
                      RxFn fn, void *user) {
    memset(d, 0, sizeof(*d));
    d->name = name;
    d->faculty = faculty;
    d->subject = subject;
    d->priority = RX_PRIO_FOREGROUND;
    d->fn = fn;
    d->user = user;
}

static void add_trigger(RxReactionDesc *d, RxObjRef o, uint64_t mask) {
    d->triggers[d->n_triggers++] = (RxDep){ o, mask };
}
static void add_write(RxReactionDesc *d, RxObjRef o, uint64_t mask) {
    d->writes[d->n_writes++] = (RxDep){ o, mask };
}
static void add_cap(RxReactionDesc *d, RxCapRef c, uint64_t res, uint32_t rights) {
    d->caps[d->n_caps++] = (RxCapNeed){ c, res, rights };
}

/* Find the snapshot entry for an object. */
static const RxSnapshotDep *in_of(const RxCtx *c, RxObjRef o) {
    for (uint32_t i = 0; i < c->n_in; i++)
        if (c->in[i].obj.id == o.id) return &c->in[i];
    return NULL;
}

/* ---- causal audit: every reaction outcome must be explainable ------------ */

static bool reaches_root(const RxWorld *w, uint64_t id, int depth) {
    const RxCrumb *k = rx_world_crumb(w, id);
    if (!k || depth > 4096) return false;
    if (k->kind == RX_CRUMB_EXTERNAL || k->kind == RX_CRUMB_CREATE) return true;
    for (uint32_t i = 0; i < k->n_parents; i++)
        if (reaches_root(w, k->parents[i], depth + 1)) return true;
    return false;
}

static void audit_and_close(Env *e) {
    RxWorld *w = &e->w;
    CHECK(rx_world_wait_quiescent(w, 10000) == RX_OK, "world did not quiesce");
    uint64_t checked = 0;
    int vrc = rx_world_verify_crumbs(w, &checked);
    CHECK(vrc == 0, "crumb digest chain verification failed (%d)", vrc);
    CHECK(w->stats.crumb_overflow == 0, "crumb log overflowed");
    g_crumbs_verified += checked;
    for (uint64_t id = 1; id <= w->n_crumbs; id++) {
        const RxCrumb *k = rx_world_crumb(w, id);
        if (k->reaction == UINT32_MAX) continue;
        g_crumbs_audited++;
        const RxReactionDesc *d = &w->reactions[k->reaction].desc;
        /* trigger explainable: woken by an earlier crumb that wrote a trigger */
        const RxCrumb *cause = rx_world_crumb(w, k->wake_cause);
        bool trig = false;
        if (cause && cause->id < k->id)
            for (uint32_t o = 0; o < cause->n_outputs; o++)
                for (uint32_t t = 0; t < d->n_triggers; t++)
                    if (cause->outputs[o].obj.id == d->triggers[t].obj.id &&
                        (cause->outputs[o].mask & d->triggers[t].mask)) trig = true;
        CHECK(trig, "crumb %llu (%s): trigger not explainable",
              (unsigned long long)id, d->name);
        CHECK(k->n_inputs > 0, "crumb %llu: inputs not recorded", (unsigned long long)id);
        CHECK(k->n_caps == d->n_caps, "crumb %llu: authority not recorded", (unsigned long long)id);
        if (k->kind == RX_CRUMB_COMMIT)
            CHECK(k->n_outputs > 0, "crumb %llu: commit without outputs", (unsigned long long)id);
        CHECK(reaches_root(w, id, 0), "crumb %llu: ancestry does not reach a stimulus/creation",
              (unsigned long long)id);
    }
    g_illegal_transitions += w->stats.illegal_transitions;
    for (int a = 0; a < RX_STATE_COUNT; a++)
        for (int b = 0; b < RX_STATE_COUNT; b++) {
            g_transitions[a][b] += w->stats.transitions[a][b];
            if (w->stats.transitions[a][b])
                CHECK(rx_state_transition_legal((RxState)a, (RxState)b),
                      "illegal transition %s -> %s", rx_state_name((RxState)a),
                      rx_state_name((RxState)b));
        }
    CHECK(w->stats.illegal_transitions == 0, "illegal lifecycle transitions observed");
    rx_world_destroy(w);
    rx_caproot_stop(&e->root, &e->admin);
}

/* ---- heartbeat reactions -------------------------------------------------- */

typedef struct { RxObjRef sensor, belief, plan, risk; } HB;

static int fn_hypothesis(RxCtx *c) {            /* AIEN: sensor -> belief */
    HB *h = c->user;
    const RxSnapshotDep *s = in_of(c, h->sensor);
    c->out[c->n_out++] = (RxMutation){ h->belief, 0, s->field[F_TEMP] * 2 + 1 };
    return 0;
}

static int fn_realize(RxCtx *c) {               /* OMEGA: belief -> plan */
    HB *h = c->user;
    const RxSnapshotDep *b = in_of(c, h->belief);
    c->out[c->n_out++] = (RxMutation){ h->plan, 0, b->field[0] + 1000 };
    return 0;
}

static int fn_risk(RxCtx *c) {                  /* fan-in of two triggers */
    HB *h = c->user;
    const RxSnapshotDep *s = in_of(c, h->sensor);
    const RxSnapshotDep *b = in_of(c, h->belief);
    c->out[c->n_out++] = (RxMutation){ h->risk, 0, s->field[F_HUMIDITY] * 100000 + b->field[0] };
    return 0;
}

typedef struct { Env e; HB h; uint32_t ra, rb, rc; RxCapRef c_ext; } HBEnv;

static int hb_setup(HBEnv *x, uint32_t workers) {
    if (env_start(&x->e, workers) != 0) return -1;
    Env *e = &x->e;
    x->h.sensor = mkobj(e, RES_SENSOR, 0);
    x->h.belief = mkobj(e, RES_BELIEF, 0);
    x->h.plan = mkobj(e, RES_PLAN, 0);
    x->h.risk = mkobj(e, RES_RISK, 0);
    x->c_ext = mint(e, SUBJ_EXTERNAL, RES_SENSOR, RX_RIGHT_WRITE);
    RxCapRef a_rs = mint(e, SUBJ_AIEN, RES_SENSOR, RX_RIGHT_READ);
    RxCapRef a_wb = mint(e, SUBJ_AIEN, RES_BELIEF, RX_RIGHT_WRITE);
    RxCapRef o_rb = mint(e, SUBJ_OMEGA, RES_BELIEF, RX_RIGHT_READ);
    RxCapRef o_wp = mint(e, SUBJ_OMEGA, RES_PLAN, RX_RIGHT_WRITE);
    RxCapRef o_rs = mint(e, SUBJ_OMEGA, RES_SENSOR, RX_RIGHT_READ);
    RxCapRef o_wr = mint(e, SUBJ_OMEGA, RES_RISK, RX_RIGHT_WRITE);

    RxReactionDesc d;
    desc_init(&d, "aien.hypothesis", RX_FACULTY_AIEN, SUBJ_AIEN, fn_hypothesis, &x->h);
    add_trigger(&d, x->h.sensor, RX_FIELD(F_TEMP));
    add_write(&d, x->h.belief, RX_FIELD(0));
    add_cap(&d, a_rs, RES_SENSOR, RX_RIGHT_READ);
    add_cap(&d, a_wb, RES_BELIEF, RX_RIGHT_WRITE);
    if (rx_world_add_reaction(&e->w, &d, &x->ra) != RX_OK) return -1;

    desc_init(&d, "omega.realize", RX_FACULTY_OMEGA, SUBJ_OMEGA, fn_realize, &x->h);
    add_trigger(&d, x->h.belief, RX_FIELD(0));
    add_write(&d, x->h.plan, RX_FIELD(0));
    add_cap(&d, o_rb, RES_BELIEF, RX_RIGHT_READ);
    add_cap(&d, o_wp, RES_PLAN, RX_RIGHT_WRITE);
    if (rx_world_add_reaction(&e->w, &d, &x->rb) != RX_OK) return -1;

    desc_init(&d, "omega.risk", RX_FACULTY_OMEGA, SUBJ_OMEGA, fn_risk, &x->h);
    add_trigger(&d, x->h.sensor, RX_FIELD(F_HUMIDITY));
    add_trigger(&d, x->h.belief, RX_FIELD(0));
    add_write(&d, x->h.risk, RX_FIELD(0));
    add_cap(&d, o_rs, RES_SENSOR, RX_RIGHT_READ);
    add_cap(&d, o_rb, RES_BELIEF, RX_RIGHT_READ);
    add_cap(&d, o_wr, RES_RISK, RX_RIGHT_WRITE);
    if (rx_world_add_reaction(&e->w, &d, &x->rc) != RX_OK) return -1;
    return 0;
}

/* ---- tests ---------------------------------------------------------------- */

static char g_heartbeat_trace[2048];

static void t_heartbeat(void) {
    begin("heartbeat_chain", "I3,I4,I6,I7");
    HBEnv x;
    CHECK(hb_setup(&x, 4) == 0, "setup");
    Env *e = &x.e;
    int64_t ext = stimulus(e, x.c_ext, x.h.sensor, F_TEMP, 21);
    CHECK(ext > 0, "external stimulus refused");
    CHECK(rx_world_wait_quiescent(&e->w, 5000) == RX_OK, "quiescence");
    CHECK(field(e, x.h.belief, 0) == 43, "belief = %llu", (unsigned long long)field(e, x.h.belief, 0));
    CHECK(field(e, x.h.plan, 0) == 1043, "plan = %llu", (unsigned long long)field(e, x.h.plan, 0));

    /* Walk back from the result: plan <- omega.realize <- belief <- aien.hypothesis <- stimulus */
    uint64_t c_plan = rx_world_explain(&e->w, x.h.plan, 0);
    const RxCrumb *kp = rx_world_crumb(&e->w, c_plan);
    CHECK(kp && kp->kind == RX_CRUMB_COMMIT && kp->reaction == x.rb, "plan writer is omega.realize");
    uint64_t c_bel = rx_world_explain(&e->w, x.h.belief, 0);
    const RxCrumb *kb = rx_world_crumb(&e->w, c_bel);
    CHECK(kb && kb->kind == RX_CRUMB_COMMIT && kb->reaction == x.ra, "belief writer is aien.hypothesis");
    CHECK(kp && kp->wake_cause == c_bel, "omega.realize was woken by the belief commit");
    CHECK(kb && kb->wake_cause == (uint64_t)ext, "aien.hypothesis was woken by the stimulus");
    bool parent_ok = false;
    for (uint32_t i = 0; kp && i < kp->n_parents; i++) if (kp->parents[i] == c_bel) parent_ok = true;
    CHECK(parent_ok, "plan crumb lists the belief crumb as a causal parent");
    CHECK(kb && kb->n_caps == 2 && kp && kp->n_caps == 2, "authority used is recorded");
    CHECK(kb && kb->faculty == RX_FACULTY_AIEN && kp && kp->faculty == RX_FACULTY_OMEGA,
          "faculty contributions recorded");
    CHECK(e->w.reactions[x.ra].activations == 1 && e->w.reactions[x.rb].activations == 1,
          "each stage activated exactly once");
    if (kp && kb) {
        const RxCrumb *ke = rx_world_crumb(&e->w, (uint64_t)ext);
        char dg[3][17];
        const RxCrumb *ks[3] = { ke, kb, kp };
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 8; j++) sprintf(&dg[i][j * 2], "%02x", ks[i]->digest[j]);
        snprintf(g_heartbeat_trace, sizeof(g_heartbeat_trace),
                 "[{\"crumb\":%llu,\"kind\":\"EXTERNAL\",\"digest16\":\"%s\"},"
                 "{\"crumb\":%llu,\"kind\":\"COMMIT\",\"reaction\":\"aien.hypothesis\",\"wake_cause\":%llu,\"digest16\":\"%s\"},"
                 "{\"crumb\":%llu,\"kind\":\"COMMIT\",\"reaction\":\"omega.realize\",\"wake_cause\":%llu,\"digest16\":\"%s\"}]",
                 (unsigned long long)ke->id, dg[0],
                 (unsigned long long)kb->id, (unsigned long long)kb->wake_cause, dg[1],
                 (unsigned long long)kp->id, (unsigned long long)kp->wake_cause, dg[2]);
    }
    audit_and_close(e);
}

static void t_single_dependency_narrow(void) {
    begin("single_dependency_field_granularity", "I7");
    HBEnv x;
    CHECK(hb_setup(&x, 2) == 0, "setup");
    Env *e = &x.e;
    stimulus(e, x.c_ext, x.h.sensor, F_TEMP, 5);
    rx_world_wait_quiescent(&e->w, 5000);
    uint64_t a0 = e->w.reactions[x.ra].activations;
    /* Humidity is not in aien.hypothesis's trigger mask: it must not wake. */
    stimulus(e, x.c_ext, x.h.sensor, F_HUMIDITY, 9);
    rx_world_wait_quiescent(&e->w, 5000);
    CHECK(e->w.reactions[x.ra].activations == a0, "temperature-only reaction woke on humidity");
    CHECK(e->w.reactions[x.rc].activations >= 1, "humidity subscriber did wake");
    /* Same value again: no change, no wake (fixed point). */
    uint64_t b0 = e->w.reactions[x.rb].activations;
    stimulus(e, x.c_ext, x.h.sensor, F_TEMP, 5);
    rx_world_wait_quiescent(&e->w, 5000);
    CHECK(e->w.reactions[x.ra].activations == a0 && e->w.reactions[x.rb].activations == b0,
          "unchanged value woke reactions");
    audit_and_close(e);
}

static void t_multiple_dependency(void) {
    begin("multiple_dependency_fan_in_two_triggers", "I6,I7");
    HBEnv x;
    CHECK(hb_setup(&x, 4) == 0, "setup");
    Env *e = &x.e;
    stimulus(e, x.c_ext, x.h.sensor, F_HUMIDITY, 3);
    stimulus(e, x.c_ext, x.h.sensor, F_TEMP, 10);
    rx_world_wait_quiescent(&e->w, 5000);
    /* risk = humidity*100000 + belief, belief = 2*temp + 1 */
    CHECK(field(e, x.h.risk, 0) == 3 * 100000 + 21, "risk = %llu",
          (unsigned long long)field(e, x.h.risk, 0));
    audit_and_close(e);
}

typedef struct { RxObjRef src; RxObjRef out; uint64_t add; } FanArg;

static int fn_fan(RxCtx *c) {
    FanArg *a = c->user;
    c->out[c->n_out++] = (RxMutation){ a->out, 0, in_of(c, a->src)->field[0] + a->add };
    return 0;
}

static void t_fan_out(void) {
    begin("fan_out_64", "I6,I7");
    Env e;
    CHECK(env_start(&e, 8) == 0, "setup");
    RxObjRef src = mkobj(&e, RES_FAN_SRC, 0);
    RxCapRef ext = mint(&e, SUBJ_EXTERNAL, RES_FAN_SRC, RX_RIGHT_WRITE);
    RxCapRef r = mint(&e, SUBJ_OMEGA, RES_FAN_SRC, RX_RIGHT_READ);
    RxCapRef wr = mint(&e, SUBJ_OMEGA, RES_FAN_OUT, RX_RIGHT_WRITE);
    static FanArg args[64];
    uint32_t ids[64];
    for (int i = 0; i < 64; i++) {
        args[i] = (FanArg){ src, mkobj(&e, RES_FAN_OUT, 0), (uint64_t)i };
        RxReactionDesc d;
        desc_init(&d, "fan.out", RX_FACULTY_OMEGA, SUBJ_OMEGA, fn_fan, &args[i]);
        add_trigger(&d, src, RX_FIELD(0));
        add_write(&d, args[i].out, RX_FIELD(0));
        add_cap(&d, r, RES_FAN_SRC, RX_RIGHT_READ);
        add_cap(&d, wr, RES_FAN_OUT, RX_RIGHT_WRITE);
        CHECK(rx_world_add_reaction(&e.w, &d, &ids[i]) == RX_OK, "add fan reaction %d", i);
    }
    stimulus(&e, ext, src, 0, 1000);
    rx_world_wait_quiescent(&e.w, 5000);
    int bad = 0, multi = 0;
    for (int i = 0; i < 64; i++) {
        if (field(&e, args[i].out, 0) != 1000u + (uint64_t)i) bad++;
        if (e.w.reactions[ids[i]].activations != 1) multi++;
    }
    CHECK(bad == 0, "%d fan-out outputs wrong", bad);
    CHECK(multi == 0, "%d fan-out reactions ran more or less than once", multi);
    audit_and_close(&e);
}

typedef struct { RxObjRef src[8]; RxObjRef agg; } FanInArg;

static int fn_sum(RxCtx *c) {
    FanInArg *a = c->user;
    uint64_t s = 0;
    for (int i = 0; i < 8; i++) s += in_of(c, a->src[i])->field[0];
    c->out[c->n_out++] = (RxMutation){ a->agg, 0, s };
    return 0;
}

static void t_fan_in(void) {
    begin("fan_in_8", "I6,I7");
    Env e;
    CHECK(env_start(&e, 4) == 0, "setup");
    static FanInArg a;
    for (int i = 0; i < 8; i++) a.src[i] = mkobj(&e, RES_FAN_SRC, 0);
    a.agg = mkobj(&e, RES_FAN_OUT, 0);
    RxCapRef ext = mint(&e, SUBJ_EXTERNAL, RES_FAN_SRC, RX_RIGHT_WRITE);
    RxCapRef r = mint(&e, SUBJ_OMEGA, RES_FAN_SRC, RX_RIGHT_READ);
    RxCapRef wr = mint(&e, SUBJ_OMEGA, RES_FAN_OUT, RX_RIGHT_WRITE);
    RxReactionDesc d;
    desc_init(&d, "fan.in", RX_FACULTY_OMEGA, SUBJ_OMEGA, fn_sum, &a);
    for (int i = 0; i < 8; i++) add_trigger(&d, a.src[i], RX_FIELD(0));
    add_write(&d, a.agg, RX_FIELD(0));
    add_cap(&d, r, RES_FAN_SRC, RX_RIGHT_READ);
    add_cap(&d, wr, RES_FAN_OUT, RX_RIGHT_WRITE);
    uint32_t id;
    CHECK(rx_world_add_reaction(&e.w, &d, &id) == RX_OK, "add");
    uint64_t want = 0;
    for (int round = 0; round < 5; round++)
        for (int i = 0; i < 8; i++) {
            uint64_t v = (uint64_t)(round * 10 + i + 1);
            stimulus(&e, ext, a.src[i], 0, v);
        }
    for (int i = 0; i < 8; i++) want += (uint64_t)(40 + i + 1);
    rx_world_wait_quiescent(&e.w, 5000);
    CHECK(field(&e, a.agg, 0) == want, "agg=%llu want %llu",
          (unsigned long long)field(&e, a.agg, 0), (unsigned long long)want);
    printf("    fan-in: 40 stimuli -> %llu activations (%llu wakes coalesced)\n",
           (unsigned long long)e.w.reactions[id].activations,
           (unsigned long long)e.w.stats.coalesced_wakes);
    audit_and_close(&e);
}

typedef struct { RxObjRef x, y; int delay_ms; } SlowArg;

static int fn_slow(RxCtx *c) {
    SlowArg *a = c->user;
    uint64_t v = in_of(c, a->x)->field[0];
    sleep_ms(a->delay_ms);
    c->out[c->n_out++] = (RxMutation){ a->y, 0, v * 10 };
    return 0;
}

static void t_invalidation(void) {
    begin("invalidation_stale_snapshot", "I2,I6,I7");
    Env e;
    CHECK(env_start(&e, 2) == 0, "setup");
    static SlowArg a;
    a.x = mkobj(&e, RES_SENSOR, 0);
    a.y = mkobj(&e, RES_BELIEF, 0);
    a.delay_ms = 80;
    RxCapRef ext = mint(&e, SUBJ_EXTERNAL, RES_SENSOR, RX_RIGHT_WRITE);
    RxReactionDesc d;
    desc_init(&d, "slow.derive", RX_FACULTY_AIEN, SUBJ_AIEN, fn_slow, &a);
    add_trigger(&d, a.x, RX_FIELD(0));
    add_write(&d, a.y, RX_FIELD(0));
    add_cap(&d, mint(&e, SUBJ_AIEN, RES_SENSOR, RX_RIGHT_READ), RES_SENSOR, RX_RIGHT_READ);
    add_cap(&d, mint(&e, SUBJ_AIEN, RES_BELIEF, RX_RIGHT_WRITE), RES_BELIEF, RX_RIGHT_WRITE);
    uint32_t id;
    CHECK(rx_world_add_reaction(&e.w, &d, &id) == RX_OK, "add");
    stimulus(&e, ext, a.x, 0, 1);
    sleep_ms(20);                       /* first activation is computing on x=1 */
    stimulus(&e, ext, a.x, 0, 2);
    rx_world_wait_quiescent(&e.w, 5000);
    CHECK(field(&e, a.y, 0) == 20, "y=%llu (stale result committed?)",
          (unsigned long long)field(&e, a.y, 0));
    CHECK(e.w.stats.invalidations >= 1, "no invalidation recorded");
    CHECK(e.w.reactions[id].commits == 1, "commits=%llu; the stale candidate must not commit",
          (unsigned long long)e.w.reactions[id].commits);
    bool saw = false;
    for (uint64_t i = 1; i <= e.w.n_crumbs; i++)
        if (rx_world_crumb(&e.w, i)->kind == RX_CRUMB_INVALIDATED) saw = true;
    CHECK(saw, "INVALIDATED crumb missing");
    audit_and_close(&e);
}

/* A read-only input changes while the reaction computes. The candidate is
 * stale and must not commit, but the trigger that woke it is still owed an
 * answer: the reaction runs again and answers it. (Found by R10: a new Omega
 * selection landed while the production path was serving a request, and the
 * request was dropped.) */
typedef struct { RxObjRef x, r, y; int delay_ms; } ReadArg;

static int fn_slow_read(RxCtx *c) {
    ReadArg *a = c->user;
    uint64_t v = in_of(c, a->x)->field[0];
    uint64_t k = in_of(c, a->r)->field[0];
    sleep_ms(a->delay_ms);
    c->out[c->n_out++] = (RxMutation){ a->y, 0, v * 10 + k };
    return 0;
}

static void t_read_only_invalidation(void) {
    begin("invalidation_by_read_only_input_reruns", "I2,I6,I7");
    Env e;
    CHECK(env_start(&e, 2) == 0, "setup");
    static ReadArg a;
    a.x = mkobj(&e, RES_SENSOR, 0);
    a.r = mkobj(&e, RES_PLAN, 0);
    a.y = mkobj(&e, RES_BELIEF, 0);
    a.delay_ms = 80;
    RxCapRef ext_x = mint(&e, SUBJ_EXTERNAL, RES_SENSOR, RX_RIGHT_WRITE);
    RxCapRef ext_r = mint(&e, SUBJ_EXTERNAL, RES_PLAN, RX_RIGHT_WRITE);
    RxReactionDesc d;
    desc_init(&d, "slow.read", RX_FACULTY_AIEN, SUBJ_AIEN, fn_slow_read, &a);
    add_trigger(&d, a.x, RX_FIELD(0));
    d.reads[d.n_reads++] = (RxDep){ a.r, RX_FIELD(0) };
    add_write(&d, a.y, RX_FIELD(0));
    add_cap(&d, mint(&e, SUBJ_AIEN, RES_SENSOR, RX_RIGHT_READ), RES_SENSOR, RX_RIGHT_READ);
    add_cap(&d, mint(&e, SUBJ_AIEN, RES_PLAN, RX_RIGHT_READ), RES_PLAN, RX_RIGHT_READ);
    add_cap(&d, mint(&e, SUBJ_AIEN, RES_BELIEF, RX_RIGHT_WRITE), RES_BELIEF, RX_RIGHT_WRITE);
    uint32_t id;
    CHECK(rx_world_add_reaction(&e.w, &d, &id) == RX_OK, "add");
    stimulus(&e, ext_x, a.x, 0, 1);
    sleep_ms(20);                       /* computing on r = 0 */
    stimulus(&e, ext_r, a.r, 0, 5);     /* not a trigger: wakes nobody */
    rx_world_wait_quiescent(&e.w, 5000);
    CHECK(field(&e, a.y, 0) == 15, "y=%llu; the stimulus was dropped or answered stale",
          (unsigned long long)field(&e, a.y, 0));
    CHECK(e.w.stats.invalidations == 1, "invalidations=%llu",
          (unsigned long long)e.w.stats.invalidations);
    CHECK(e.w.reactions[id].commits == 1, "commits=%llu",
          (unsigned long long)e.w.reactions[id].commits);
    CHECK(e.w.reactions[id].activations == 2, "activations=%llu",
          (unsigned long long)e.w.reactions[id].activations);
    audit_and_close(&e);
}

/* Concurrent publication: 8 incrementers on one PAIR object, driven by 4
 * publisher threads. a and b must always move together, and no increment may
 * be lost: a == b == number of increment commits. */
typedef struct { RxObjRef pair, tick; } IncArg;
typedef struct { RxObjRef pair, check; } ChkArg;

static int fn_inc(RxCtx *c) {
    IncArg *a = c->user;
    const RxSnapshotDep *p = in_of(c, a->pair);
    c->out[c->n_out++] = (RxMutation){ a->pair, 0, p->field[0] + 1 };
    c->out[c->n_out++] = (RxMutation){ a->pair, 1, p->field[1] + 1 };
    return 0;
}

static int fn_chk(RxCtx *c) {
    ChkArg *a = c->user;
    const RxSnapshotDep *p = in_of(c, a->pair);
    const RxSnapshotDep *k = in_of(c, a->check);
    if (p->field[0] != p->field[1])
        c->out[c->n_out++] = (RxMutation){ a->check, 0, k->field[0] + 1 };
    return 0;
}

typedef struct { Env *e; RxCapRef cap; RxObjRef ticks[8]; int base; } PubArg;

static void *publisher(void *arg) {
    PubArg *p = arg;
    for (int n = 0; n < 250; n++)
        for (int i = 0; i < 2; i++)
            stimulus(p->e, p->cap, p->ticks[p->base + i], 0, (uint64_t)(n + 1));
    return NULL;
}

static void t_concurrent_publication(void) {
    begin("concurrent_publication_no_torn_no_lost", "I6,I7");
    Env e;
    CHECK(env_start(&e, 8) == 0, "setup");
    RxObjRef pair = mkobj(&e, RES_PAIR, 0);
    RxObjRef check = mkobj(&e, RES_CHECK, 0);
    RxCapRef ext = mint(&e, SUBJ_EXTERNAL, RES_TICK, RX_RIGHT_WRITE);
    RxCapRef rt = mint(&e, SUBJ_OMEGA, RES_TICK, RX_RIGHT_READ);
    RxCapRef rwp = mint(&e, SUBJ_OMEGA, RES_PAIR, RX_RIGHT_READ | RX_RIGHT_WRITE);
    RxCapRef rwc = mint(&e, SUBJ_AEGIS, RES_CHECK, RX_RIGHT_READ | RX_RIGHT_WRITE);
    RxCapRef rp = mint(&e, SUBJ_AEGIS, RES_PAIR, RX_RIGHT_READ);
    static IncArg inc[8];
    static ChkArg chk;
    uint32_t ids[8], cid;
    PubArg pa[4];
    RxObjRef ticks[8];
    for (int i = 0; i < 8; i++) {
        ticks[i] = mkobj(&e, RES_TICK, 0);
        inc[i] = (IncArg){ pair, ticks[i] };
        RxReactionDesc d;
        desc_init(&d, "pair.increment", RX_FACULTY_OMEGA, SUBJ_OMEGA, fn_inc, &inc[i]);
        add_trigger(&d, ticks[i], RX_FIELD(0));
        d.reads[d.n_reads++] = (RxDep){ pair, RX_FIELD(0) | RX_FIELD(1) };
        add_write(&d, pair, RX_FIELD(0) | RX_FIELD(1));
        add_cap(&d, rt, RES_TICK, RX_RIGHT_READ);
        add_cap(&d, rwp, RES_PAIR, RX_RIGHT_READ | RX_RIGHT_WRITE);
        CHECK(rx_world_add_reaction(&e.w, &d, &ids[i]) == RX_OK, "add inc");
    }
    chk = (ChkArg){ pair, check };
    RxReactionDesc d;
    desc_init(&d, "aegis.pair_invariant", RX_FACULTY_AEGIS, SUBJ_AEGIS, fn_chk, &chk);
    add_trigger(&d, pair, RX_FIELD(0) | RX_FIELD(1));
    d.reads[d.n_reads++] = (RxDep){ check, RX_FIELD(0) };
    add_write(&d, check, RX_FIELD(0));
    add_cap(&d, rp, RES_PAIR, RX_RIGHT_READ);
    add_cap(&d, rwc, RES_CHECK, RX_RIGHT_READ | RX_RIGHT_WRITE);
    CHECK(rx_world_add_reaction(&e.w, &d, &cid) == RX_OK, "add checker");

    pthread_t th[4];
    for (int i = 0; i < 4; i++) {
        pa[i].e = &e;
        pa[i].cap = ext;
        memcpy(pa[i].ticks, ticks, sizeof(ticks));
        pa[i].base = i * 2;
        pthread_create(&th[i], NULL, publisher, &pa[i]);
    }
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    rx_world_wait_quiescent(&e.w, 20000);
    uint64_t commits = 0;
    for (int i = 0; i < 8; i++) commits += e.w.reactions[ids[i]].commits;
    uint64_t a = field(&e, pair, 0), b = field(&e, pair, 1);
    CHECK(a == b, "torn pair a=%llu b=%llu", (unsigned long long)a, (unsigned long long)b);
    CHECK(a == commits, "lost update: a=%llu commits=%llu", (unsigned long long)a,
          (unsigned long long)commits);
    CHECK(field(&e, check, 0) == 0, "invariant checker observed %llu torn states",
          (unsigned long long)field(&e, check, 0));
    printf("    concurrent: 2000 ticks -> %llu increments committed, %llu invalidations, a=b=%llu\n",
           (unsigned long long)commits, (unsigned long long)e.w.stats.invalidations,
           (unsigned long long)a);
    audit_and_close(&e);
}

typedef struct { RxObjRef x, target; } StaleArg;

static int fn_write_target(RxCtx *c) {
    StaleArg *a = c->user;
    c->out[c->n_out++] = (RxMutation){ a->target, 0, in_of(c, a->x)->field[0] };
    return 0;
}

static int fn_read_target(RxCtx *c) { (void)c; return 0; }

static void t_stale_input(void) {
    begin("stale_generation_rejected", "I2,I7");
    Env e;
    CHECK(env_start(&e, 2) == 0, "setup");
    static StaleArg a;
    a.x = mkobj(&e, RES_SENSOR, 0);
    a.target = mkobj(&e, RES_SCRATCH, 7);
    RxObjRef reader_dep = a.target;
    RxCapRef ext = mint(&e, SUBJ_EXTERNAL, RES_SENSOR, RX_RIGHT_WRITE);
    RxCapRef ext_s = mint(&e, SUBJ_EXTERNAL, RES_SCRATCH, RX_RIGHT_WRITE);
    RxCapRef rs = mint(&e, SUBJ_AIEN, RES_SENSOR, RX_RIGHT_READ);
    RxCapRef ws = mint(&e, SUBJ_AIEN, RES_SCRATCH, RX_RIGHT_READ | RX_RIGHT_WRITE);
    RxReactionDesc d;
    desc_init(&d, "stale.writer", RX_FACULTY_AIEN, SUBJ_AIEN, fn_write_target, &a);
    add_trigger(&d, a.x, RX_FIELD(0));
    add_write(&d, a.target, RX_FIELD(0));
    add_cap(&d, rs, RES_SENSOR, RX_RIGHT_READ);
    add_cap(&d, ws, RES_SCRATCH, RX_RIGHT_READ | RX_RIGHT_WRITE);
    uint32_t wid, rid;
    CHECK(rx_world_add_reaction(&e.w, &d, &wid) == RX_OK, "add writer");
    desc_init(&d, "stale.reader", RX_FACULTY_AIEN, SUBJ_AIEN, fn_read_target, NULL);
    add_trigger(&d, a.x, RX_FIELD(0));
    d.reads[d.n_reads++] = (RxDep){ reader_dep, RX_FIELD(0) };
    add_cap(&d, rs, RES_SENSOR, RX_RIGHT_READ);
    add_cap(&d, ws, RES_SCRATCH, RX_RIGHT_READ | RX_RIGHT_WRITE);
    CHECK(rx_world_add_reaction(&e.w, &d, &rid) == RX_OK, "add reader");

    /* Retire the target and reuse its slot: same id, next generation. */
    CHECK(rx_world_retire(&e.w, a.target) == RX_OK, "retire");
    RxObjRef reborn = mkobj(&e, RES_SCRATCH, 99);
    CHECK(reborn.id == a.target.id && reborn.generation == a.target.generation + 1,
          "slot reuse did not advance generation");
    CHECK(stimulus(&e, ext_s, a.target, 0, 5) == RX_ERR_STALE_GEN,
          "external write through a stale reference was accepted");
    stimulus(&e, ext, a.x, 0, 1234);
    rx_world_wait_quiescent(&e.w, 5000);
    CHECK(field(&e, reborn, 0) == 99, "stale writer mutated the reborn object");
    CHECK(e.w.reactions[wid].commits == 0, "stale writer committed");
    CHECK(e.w.stats.rejected >= 1, "stale write not rejected");
    CHECK(e.w.stats.invalidations >= 1, "reader of retired object not invalidated");
    audit_and_close(&e);
}

typedef struct { RxObjRef x, declared, undeclared; int mode; } WsArg;

static int fn_outside(RxCtx *c) {
    WsArg *a = c->user;
    if (a->mode == 0)   /* an object it holds WRITE authority for, but never declared */
        c->out[c->n_out++] = (RxMutation){ a->undeclared, 0, 555 };
    else                /* the declared object, but a field outside its write mask */
        c->out[c->n_out++] = (RxMutation){ a->declared, 3, 555 };
    c->out[c->n_out++] = (RxMutation){ a->declared, 0, 1 };
    return 0;
}

static void t_write_set_enforced(void) {
    begin("write_outside_declared_set_rejected", "I6,I9");
    Env e;
    CHECK(env_start(&e, 2) == 0, "setup");
    static WsArg a[2];
    RxObjRef x = mkobj(&e, RES_SENSOR, 0);
    RxCapRef ext = mint(&e, SUBJ_EXTERNAL, RES_SENSOR, RX_RIGHT_WRITE);
    RxCapRef rs = mint(&e, SUBJ_AIEN, RES_SENSOR, RX_RIGHT_READ);
    RxCapRef wb = mint(&e, SUBJ_AIEN, RES_BELIEF, RX_RIGHT_WRITE);
    uint32_t ids[2];
    for (int m = 0; m < 2; m++) {
        a[m] = (WsArg){ x, mkobj(&e, RES_BELIEF, 0), mkobj(&e, RES_BELIEF, 0), m };
        RxReactionDesc d;
        desc_init(&d, m ? "outside.field_mask" : "outside.object", RX_FACULTY_AIEN, SUBJ_AIEN,
                  fn_outside, &a[m]);
        add_trigger(&d, x, RX_FIELD(0));
        add_write(&d, a[m].declared, RX_FIELD(0));
        add_cap(&d, rs, RES_SENSOR, RX_RIGHT_READ);
        add_cap(&d, wb, RES_BELIEF, RX_RIGHT_WRITE);
        CHECK(rx_world_add_reaction(&e.w, &d, &ids[m]) == RX_OK, "add");
    }
    stimulus(&e, ext, x, 0, 1);
    rx_world_wait_quiescent(&e.w, 5000);
    for (int m = 0; m < 2; m++) {
        RxObject o;
        rx_world_read(&e.w, a[m].declared, &o);
        CHECK(o.field[0] == 0 && o.field[3] == 0, "mode %d: partial publication leaked", m);
        CHECK(field(&e, a[m].undeclared, 0) == 0, "mode %d: undeclared object written", m);
        CHECK(e.w.reactions[ids[m]].commits == 0, "mode %d: committed", m);
    }
    CHECK(e.w.stats.rejected == 2, "rejected=%llu", (unsigned long long)e.w.stats.rejected);
    audit_and_close(&e);
}

typedef struct { RxObjRef d, out; } DupArg;

static int fn_dup(RxCtx *c) {
    DupArg *a = c->user;
    uint64_t v = in_of(c, a->d)->field[0];
    sleep_ms(2);
    c->out[c->n_out++] = (RxMutation){ a->out, 0, v };
    return 0;
}

static void t_duplicate_wake(void) {
    begin("duplicate_wake_coalesced", "I7,I13");
    Env e;
    CHECK(env_start(&e, 4) == 0, "setup");
    static DupArg a;
    a.d = mkobj(&e, RES_SENSOR, 0);
    a.out = mkobj(&e, RES_BELIEF, 0);
    RxCapRef ext = mint(&e, SUBJ_EXTERNAL, RES_SENSOR, RX_RIGHT_WRITE);
    RxReactionDesc d;
    desc_init(&d, "dup.derive", RX_FACULTY_AIEN, SUBJ_AIEN, fn_dup, &a);
    add_trigger(&d, a.d, RX_FIELD(0));
    add_write(&d, a.out, RX_FIELD(0));
    add_cap(&d, mint(&e, SUBJ_AIEN, RES_SENSOR, RX_RIGHT_READ), RES_SENSOR, RX_RIGHT_READ);
    add_cap(&d, mint(&e, SUBJ_AIEN, RES_BELIEF, RX_RIGHT_WRITE), RES_BELIEF, RX_RIGHT_WRITE);
    uint32_t id;
    CHECK(rx_world_add_reaction(&e.w, &d, &id) == RX_OK, "add");
    for (uint64_t v = 1; v <= 1000; v++) stimulus(&e, ext, a.d, 0, v);
    rx_world_wait_quiescent(&e.w, 10000);
    uint64_t acts = e.w.reactions[id].activations;
    CHECK(field(&e, a.out, 0) == 1000, "final output %llu != last input",
          (unsigned long long)field(&e, a.out, 0));
    CHECK(acts < 1000, "no coalescing: %llu activations for 1000 wakes", (unsigned long long)acts);
    CHECK(e.w.stats.coalesced_wakes > 0, "coalesced counter is zero");
    printf("    duplicate wake: 1000 wakes -> %llu activations, %llu commits\n",
           (unsigned long long)acts, (unsigned long long)e.w.reactions[id].commits);
    audit_and_close(&e);
}

/* ---- authority ------------------------------------------------------------ */

static int fn_forbidden(RxCtx *c) {
    HB *h = c->user;
    c->out[c->n_out++] = (RxMutation){ h->belief, 0, 666 };
    return 0;
}

static uint64_t blocked_with(Env *e, RxObjRef sensor, RxObjRef belief, RxCapRef ext,
                             uint32_t subject, RxCapRef rcap, RxCapRef wcap, uint64_t trigger_v,
                             int *out_reason) {
    static HB h;
    h.sensor = sensor;
    h.belief = belief;
    RxReactionDesc d;
    desc_init(&d, "attack.writer", RX_FACULTY_AIEN, subject, fn_forbidden, &h);
    add_trigger(&d, sensor, RX_FIELD(F_TEMP));
    add_write(&d, belief, RX_FIELD(0));
    add_cap(&d, rcap, RES_SENSOR, RX_RIGHT_READ);
    add_cap(&d, wcap, RES_BELIEF, RX_RIGHT_WRITE);
    uint32_t id;
    if (rx_world_add_reaction(&e->w, &d, &id) != RX_OK) return UINT64_MAX;
    uint64_t first = e->w.n_crumbs + 1;
    stimulus(e, ext, sensor, F_TEMP, trigger_v);
    rx_world_wait_quiescent(&e->w, 5000);
    *out_reason = 0;
    uint64_t blocked = 0;
    for (uint64_t i = first; i <= e->w.n_crumbs; i++) {
        const RxCrumb *k = rx_world_crumb(&e->w, i);
        if (k->reaction != id) continue;
        if (k->kind == RX_CRUMB_BLOCKED_AUTHORITY) { *out_reason = k->reason; blocked++; }
        else blocked += 1000;           /* any other outcome is a failure */
    }
    return blocked;
}

static void t_capability_attacks(void) {
    begin("capability_attacks_all_fail", "I3,I4,I9");
    Env e;
    CHECK(env_start(&e, 2) == 0, "setup");
    RxObjRef sensor = mkobj(&e, RES_SENSOR, 0);
    RxObjRef belief = mkobj(&e, RES_BELIEF, 0);
    RxCapRef ext = mint(&e, SUBJ_EXTERNAL, RES_SENSOR, RX_RIGHT_WRITE);
    RxCapRef rs = mint(&e, SUBJ_AIEN, RES_SENSOR, RX_RIGHT_READ);
    RxCapRef wb = mint(&e, SUBJ_AIEN, RES_BELIEF, RX_RIGHT_WRITE);
    int reason = 0;
    uint64_t v = 1;

    struct { const char *name; RxCapRef w; uint32_t subject; int expect; } cases[8];
    int nc = 0;
    cases[nc++] = (typeof(cases[0])){ "forged_reference", (RxCapRef){ 200, 7 }, SUBJ_AIEN, RX_CAP_ERR_STALE_GEN };
    cases[nc++] = (typeof(cases[0])){ "wrong_subject", wb, SUBJ_OTHER, RX_CAP_ERR_SUBJECT };
    cases[nc++] = (typeof(cases[0])){ "wrong_resource", mint(&e, SUBJ_AIEN, RES_PLAN, RX_RIGHT_WRITE), SUBJ_AIEN, RX_CAP_ERR_RESOURCE };
    cases[nc++] = (typeof(cases[0])){ "insufficient_rights", mint(&e, SUBJ_AIEN, RES_BELIEF, RX_RIGHT_READ), SUBJ_AIEN, RX_CAP_ERR_RIGHTS };
    RxCapRef rev = mint(&e, SUBJ_AIEN, RES_BELIEF, RX_RIGHT_WRITE);
    revoke_cap(&e.admin, rev);
    cases[nc++] = (typeof(cases[0])){ "revoked", rev, SUBJ_AIEN, RX_CAP_ERR_REVOKED };
    /* Generation replay: revoke, reclaim, the slot is re-minted to someone else. */
    RxCapRef old = mint(&e, SUBJ_AIEN, RES_BELIEF, RX_RIGHT_WRITE);
    revoke_cap(&e.admin, old);
    rx_capadmin_reclaim(&e.admin, office_of(&e.admin), old.cap_id);
    RxCapRef reused = mint(&e, SUBJ_OTHER, RES_BELIEF, RX_RIGHT_WRITE);
    CHECK(reused.cap_id == old.cap_id && reused.generation == old.generation + 1,
          "reclaimed slot was not reused with a new generation");
    cases[nc++] = (typeof(cases[0])){ "generation_replay", old, SUBJ_AIEN, RX_CAP_ERR_STALE_GEN };
    RxCapMint lm;
    memset(&lm, 0, sizeof(lm));
    lm.issuer = ISSUER_AEGIS_POLICY;
    lm.subject = SUBJ_AIEN;
    lm.resource = RES_BELIEF;
    lm.rights = RX_RIGHT_WRITE;
    lm.lease_ticks = 5;
    lm.parent = (RxCapRef){ UINT32_MAX, 0 };
    lm.authority = office_of(&e.admin);
    RxCapRef leased;
    rx_capadmin_mint(&e.admin, &lm, &leased);
    rx_capadmin_advance_clock(&e.admin, office_of(&e.admin), 10);
    cases[nc++] = (typeof(cases[0])){ "expired_lease", leased, SUBJ_AIEN, RX_CAP_ERR_EXPIRED };

    for (int i = 0; i < nc; i++) {
        uint64_t n = blocked_with(&e, sensor, belief, ext, cases[i].subject,
                                 cases[i].subject == SUBJ_AIEN ? rs : mint(&e, cases[i].subject, RES_SENSOR, RX_RIGHT_READ),
                                 cases[i].w, v++, &reason);
        CHECK(n == 1 && reason == cases[i].expect, "%s: blocked=%llu reason=%s (want %s)",
              cases[i].name, (unsigned long long)n, rx_cap_strerror(reason),
              rx_cap_strerror(cases[i].expect));
        CHECK(field(&e, belief, 0) == 0, "%s: forbidden write reached the world", cases[i].name);
    }

    /* Delegation may only attenuate. */
    RxCapRef parent = mint(&e, SUBJ_AIEN, RES_BELIEF, RX_RIGHT_READ | RX_RIGHT_DELEGATE);
    RxCapMint amp;
    memset(&amp, 0, sizeof(amp));
    amp.issuer = SUBJ_AIEN;
    amp.subject = SUBJ_OMEGA;
    amp.resource = RES_BELIEF;
    amp.rights = RX_RIGHT_READ | RX_RIGHT_WRITE;
    amp.parent = parent;
    amp.authority = parent;
    CHECK(rx_capadmin_mint(&e.admin, &amp, NULL) == RX_CAP_ERR_AMPLIFY, "rights amplification accepted");
    RxCapMint other = amp;
    other.resource = RES_PLAN;
    other.rights = RX_RIGHT_READ;
    CHECK(rx_capadmin_mint(&e.admin, &other, NULL) == RX_CAP_ERR_RESOURCE, "resource widening accepted");
    RxCapMint nd = amp;
    nd.resource = RES_SENSOR;
    nd.rights = RX_RIGHT_READ;
    nd.parent = rs;
    nd.authority = rs;
    CHECK(rx_capadmin_mint(&e.admin, &nd, NULL) == RX_CAP_ERR_NOT_DELEGABLE, "non-delegable parent delegated");
    RxCapMint ok = amp;
    ok.rights = RX_RIGHT_READ;
    RxCapRef child;
    CHECK(rx_capadmin_mint(&e.admin, &ok, &child) == RX_CAP_OK, "valid attenuation refused");
    CHECK(rx_caproot_validate(&e.root, child, SUBJ_OMEGA, RES_BELIEF, RX_RIGHT_READ, NULL) == RX_CAP_OK,
          "delegated child invalid");
    revoke_cap(&e.admin, parent);
    int child_after = rx_caproot_validate(&e.root, child, SUBJ_OMEGA, RES_BELIEF, RX_RIGHT_READ, NULL);
    CHECK(child_after == RX_CAP_ERR_REVOKED || child_after == RX_CAP_ERR_CHAIN,
          "child survived parent revocation (%s)", rx_cap_strerror(child_after));

    /* External stimulus with a capability for the wrong subject. */
    CHECK(stimulus(&e, rs, sensor, F_TEMP, 77) == RX_ERR_AUTHORITY, "external write with wrong cap accepted");

    /* Epoch change invalidates everything minted before it. */
    rx_capadmin_bump_epoch(&e.admin, office_of(&e.admin));
    CHECK(rx_caproot_validate(&e.root, rs, SUBJ_AIEN, RES_SENSOR, RX_RIGHT_READ, NULL) == RX_CAP_ERR_EPOCH,
          "pre-epoch capability still valid");
    audit_and_close(&e);
}

typedef struct { RxObjRef x, y; } RevArg;

static int fn_rev(RxCtx *c) {
    RevArg *a = c->user;
    sleep_ms(80);
    c->out[c->n_out++] = (RxMutation){ a->y, 0, in_of(c, a->x)->field[0] + 1 };
    return 0;
}

static void t_revoke_during_run(void) {
    begin("revocation_during_run_rejected", "I4,I7");
    Env e;
    CHECK(env_start(&e, 2) == 0, "setup");
    static RevArg a;
    a.x = mkobj(&e, RES_SENSOR, 0);
    a.y = mkobj(&e, RES_BELIEF, 0);
    RxCapRef ext = mint(&e, SUBJ_EXTERNAL, RES_SENSOR, RX_RIGHT_WRITE);
    RxCapRef w = mint(&e, SUBJ_AIEN, RES_BELIEF, RX_RIGHT_WRITE);
    RxReactionDesc d;
    desc_init(&d, "revoked.midflight", RX_FACULTY_AIEN, SUBJ_AIEN, fn_rev, &a);
    add_trigger(&d, a.x, RX_FIELD(0));
    add_write(&d, a.y, RX_FIELD(0));
    add_cap(&d, mint(&e, SUBJ_AIEN, RES_SENSOR, RX_RIGHT_READ), RES_SENSOR, RX_RIGHT_READ);
    add_cap(&d, w, RES_BELIEF, RX_RIGHT_WRITE);
    uint32_t id;
    CHECK(rx_world_add_reaction(&e.w, &d, &id) == RX_OK, "add");
    stimulus(&e, ext, a.x, 0, 41);
    sleep_ms(20);
    CHECK(revoke_cap(&e.admin, w) == RX_CAP_OK, "revoke");
    rx_world_wait_quiescent(&e.w, 5000);
    CHECK(field(&e, a.y, 0) == 0, "revoked authority executed");
    CHECK(e.w.stats.rejected == 1, "publication not rejected");
    bool saw = false;
    for (uint64_t i = 1; i <= e.w.n_crumbs; i++) {
        const RxCrumb *k = rx_world_crumb(&e.w, i);
        if (k->kind == RX_CRUMB_REJECTED && k->reason == RX_CAP_ERR_REVOKED) saw = true;
    }
    CHECK(saw, "REJECTED(revoked) crumb missing");
    audit_and_close(&e);
}

static void t_root_table_unwritable(void) {
    begin("capability_table_unforgeable_by_runtime", "I3");
    RxCapRoot root;
    RxCapAdmin admin;
    CHECK(rx_caproot_start(&root, &admin) == RX_CAP_OK, "root");
    void *page = (void *)root.table;
    errno = 0;
    CHECK(mprotect(page, sizeof(RxCapTable), PROT_READ | PROT_WRITE) != 0,
          "runtime made the capability table writable");
    void *m = mmap(NULL, sizeof(RxCapTable), PROT_READ | PROT_WRITE, MAP_SHARED, root.ro_fd, 0);
    CHECK(m == MAP_FAILED, "runtime obtained a writable mapping");
    if (m != MAP_FAILED) munmap(m, sizeof(RxCapTable));
    uint32_t junk = 0xffffffffu;
    CHECK(pwrite(root.ro_fd, &junk, sizeof(junk), offsetof(RxCapTable, entries)) < 0,
          "runtime wrote the capability table through its fd");
    CHECK(ftruncate(root.ro_fd, 0) != 0, "runtime resized the capability table");
    /* Reopen through /proc: the seal is on the inode, so every path is sealed. */
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", root.ro_fd);
    int fd2 = open(path, O_RDWR);
    if (fd2 >= 0) {
        CHECK(pwrite(fd2, &junk, sizeof(junk), offsetof(RxCapTable, entries)) < 0,
              "reopened fd could write the table");
        close(fd2);
    }
    /* The root process is non-dumpable: its writable mapping is out of reach. */
    snprintf(path, sizeof(path), "/proc/%d/mem", (int)root.root_pid);
    int memfd = open(path, O_RDWR);
    CHECK(memfd < 0, "runtime opened the root's memory");
    if (memfd >= 0) close(memfd);
    /* A direct store faults. Prove it in a throwaway child. */
    pid_t pid = fork();
    if (pid == 0) {
        volatile uint32_t *rights = (volatile uint32_t *)&((RxCapTable *)page)->entries[0].rights;
        *rights = 0xffffffffu;
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFSIGNALED(st) && (WTERMSIG(st) == SIGSEGV || WTERMSIG(st) == SIGBUS),
          "direct store into the capability table did not fault");
    rx_caproot_stop(&root, &admin);
}

static void t_crumb_tamper_detected(void) {
    begin("causal_record_tamper_detected", "I7,I15");
    HBEnv x;
    CHECK(hb_setup(&x, 2) == 0, "setup");
    Env *e = &x.e;
    stimulus(e, x.c_ext, x.h.sensor, F_TEMP, 3);
    rx_world_wait_quiescent(&e->w, 5000);
    uint64_t id = rx_world_explain(&e->w, x.h.belief, 0);
    RxCrumb *k = &e->w.crumbs[id - 1];
    k->caps[0].cap_id ^= 1;             /* rewrite "which authority allowed it" */
    CHECK(rx_world_verify_crumbs(&e->w, NULL) != 0, "rewritten authority went undetected");
    k->caps[0].cap_id ^= 1;
    CHECK(rx_world_verify_crumbs(&e->w, NULL) == 0, "restored record fails verification");
    k->cap_issuer[0] ^= 1;              /* rewrite who issued that authority */
    CHECK(rx_world_verify_crumbs(&e->w, NULL) != 0, "rewritten issuer went undetected");
    k->cap_issuer[0] ^= 1;
    CHECK(rx_world_verify_crumbs(&e->w, NULL) == 0, "restored issuer fails verification");
    audit_and_close(e);
}

/* No orchestrator, and the order does not matter: for a pure DAG the committed
 * world is identical whatever the worker count or thread interleaving. */
static void t_schedule_independence(void) {
    begin("schedule_independent_result", "I16");
    uint8_t ref[32];
    int differ = 0, runs = 0;
    for (uint32_t workers = 1; workers <= 8; workers++)
        for (int rep = 0; rep < 6; rep++) {
            HBEnv x;
            if (hb_setup(&x, workers) != 0) { differ++; continue; }
            Env *e = &x.e;
            for (uint64_t v = 1; v <= 20; v++) {
                stimulus(e, x.c_ext, x.h.sensor, F_TEMP, v * 3);
                stimulus(e, x.c_ext, x.h.sensor, F_HUMIDITY, v);
            }
            rx_world_wait_quiescent(&e->w, 5000);
            uint8_t dg[32];
            rx_world_digest(&e->w, dg);
            if (runs == 0) memcpy(ref, dg, 32);
            else if (memcmp(ref, dg, 32) != 0) differ++;
            runs++;
            audit_and_close(e);
        }
    CHECK(differ == 0, "%d of %d runs committed a different world", differ, runs);
    printf("    %d runs across 1..8 workers, identical committed world\n", runs);
}

/* ---- authority hardening (host). Not R7. -------------------------------- */

static RxCapMint delegated(RxCapRef parent, uint32_t subject, uint64_t resource, uint32_t rights) {
    RxCapMint m;
    memset(&m, 0, sizeof(m));
    m.issuer = ISSUER_AEGIS_POLICY;
    m.subject = subject;
    m.resource = resource;
    m.rights = rights;
    m.parent = parent;
    m.authority = parent;
    return m;
}

static void t_authority_office(void) {
    begin("authority_office_required", "I3");
    RxCapRoot root;
    RxCapAdmin admin;
    CHECK(rx_caproot_start(&root, &admin) == RX_CAP_OK, "root");
    RxCapRef office = office_of(&admin);
    CHECK(office.cap_id != UINT32_MAX, "office was not delivered");

    RxCapMint bare;
    memset(&bare, 0, sizeof(bare));
    bare.issuer = ISSUER_AEGIS_POLICY;
    bare.subject = SUBJ_AIEN;
    bare.resource = RES_SENSOR;
    bare.rights = RX_RIGHT_READ;
    bare.parent = (RxCapRef){ UINT32_MAX, 0 };
    bare.authority = (RxCapRef){ UINT32_MAX, 0 };
    CHECK(rx_capadmin_mint(&admin, &bare, NULL) == RX_CAP_ERR_UNAUTHORIZED,
          "socket holder minted without a delivered authority");

    RxCapMint ok = bare;
    ok.authority = office;
    RxCapRef world;
    CHECK(rx_capadmin_mint(&admin, &ok, &world) == RX_CAP_OK, "office mint refused");
    CHECK(rx_capadmin_revoke(&admin, world, world) == RX_CAP_ERR_UNAUTHORIZED,
          "ordinary capability revoked");
    CHECK(rx_capadmin_advance_clock(&admin, world, 1) == RX_CAP_ERR_UNAUTHORIZED,
          "ordinary capability advanced the clock");
    CHECK(rx_capadmin_bump_epoch(&admin, world) == RX_CAP_ERR_UNAUTHORIZED,
          "ordinary capability changed the epoch");
    CHECK(rx_capadmin_reclaim(&admin, world, world.cap_id) == RX_CAP_ERR_UNAUTHORIZED,
          "ordinary capability reclaimed a slot");

    RxCapMint both = ok;
    both.rights = RX_RIGHT_REVOKE | RX_RIGHT_DELEGATE;
    CHECK(rx_capadmin_mint(&admin, &both, NULL) == RX_CAP_ERR_NOT_DELEGABLE,
          "privileged right combined with delegate");
    RxCapMint revoker = ok;
    revoker.rights = RX_RIGHT_REVOKE;
    revoker.resource = RX_CAP_RES_AUTHORITY;
    RxCapRef revcap;
    CHECK(rx_capadmin_mint(&admin, &revoker, &revcap) == RX_CAP_OK, "limited revoker refused");
    RxCapMint pass = delegated(revcap, SUBJ_OMEGA, RX_CAP_RES_AUTHORITY, RX_RIGHT_REVOKE);
    CHECK(rx_capadmin_mint(&admin, &pass, NULL) == RX_CAP_ERR_NOT_DELEGABLE,
          "privileged right was delegated");
    CHECK(rx_capadmin_revoke(&admin, revcap, world) == RX_CAP_OK, "limited revoker could not revoke");

    /* Office MINT must not attenuate an unrelated capability. */
    RxCapRef parent = office;
    (void)parent;
    RxCapMint sneak = bare;
    sneak.authority = office;
    sneak.parent = world; /* revoked, and not the office */
    sneak.rights = RX_RIGHT_READ;
    CHECK(rx_capadmin_mint(&admin, &sneak, NULL) != RX_CAP_OK,
          "office minted a delegation of a capability it does not hold");

    RxCapRef cur;
    RxCapMint base = ok;
    base.rights = RX_RIGHT_READ | RX_RIGHT_DELEGATE;
    base.resource = RES_BELIEF;
    CHECK(rx_capadmin_mint(&admin, &base, &cur) == RX_CAP_OK, "delegable parent refused");
    int depth_ok = 0;
    for (int i = 0; i < 8; i++) {
        RxCapMint step = delegated(cur, SUBJ_OMEGA, RES_BELIEF, RX_RIGHT_READ | RX_RIGHT_DELEGATE);
        RxCapRef child;
        if (rx_capadmin_mint(&admin, &step, &child) != RX_CAP_OK) break;
        cur = child;
        depth_ok++;
    }
    CHECK(depth_ok == 8, "delegation depth stopped early (%d)", depth_ok);
    RxCapMint too_deep = delegated(cur, SUBJ_OMEGA, RES_BELIEF, RX_RIGHT_READ | RX_RIGHT_DELEGATE);
    CHECK(rx_capadmin_mint(&admin, &too_deep, NULL) == RX_CAP_ERR_CHAIN,
          "delegation deeper than the limit was accepted");

    /* A narrower revoker cannot strike the office, and revocation cascades. */
    RxCapMint chain = ok;
    chain.rights = RX_RIGHT_READ | RX_RIGHT_DELEGATE;
    chain.resource = RES_SENSOR;
    RxCapRef link;
    CHECK(rx_capadmin_mint(&admin, &chain, &link) == RX_CAP_OK, "chain parent");
    RxCapMint step1 = delegated(link, SUBJ_OMEGA, RES_SENSOR, RX_RIGHT_READ | RX_RIGHT_DELEGATE);
    RxCapRef mid;
    CHECK(rx_capadmin_mint(&admin, &step1, &mid) == RX_CAP_OK, "chain child");
    RxCapMint step2 = delegated(mid, SUBJ_OMEGA, RES_SENSOR, RX_RIGHT_READ);
    RxCapRef grand;
    CHECK(rx_capadmin_mint(&admin, &step2, &grand) == RX_CAP_OK, "chain grandchild");
    CHECK(rx_capadmin_revoke(&admin, revcap, office) == RX_CAP_ERR_UNAUTHORIZED,
          "narrow revoker struck the office");
    CHECK(rx_caproot_validate(&root, office, 0, RX_CAP_RES_AUTHORITY, RX_RIGHT_MINT, NULL) == RX_CAP_OK,
          "office died after a refused revoke");
    CHECK(rx_capadmin_revoke(&admin, office, link) == RX_CAP_OK, "parent revoke");
    RxCapEntry seen;
    CHECK(rx_caproot_inspect(&root, grand, &seen) == RX_CAP_OK, "grandchild disappeared");
    CHECK(seen.state == RX_CAP_REVOKED, "grandchild stayed live after its ancestor was revoked");
    CHECK(rx_caproot_validate(&root, grand, SUBJ_OMEGA, RES_SENSOR, RX_RIGHT_READ, NULL) != RX_CAP_OK,
          "grandchild of a revoked capability still validated");
    CHECK(seen.minted_by_id == mid.cap_id, "grandchild did not record who minted it");

    /* The office id is visible in the readable table. Without the token the
     * socket must still refuse. */
    RxCapRequest forged;
    memset(&forged, 0, sizeof(forged));
    forged.op = RX_OP_MINT;
    forged.mint = bare;
    forged.mint.authority = office;
    forged.authority = office;
    CHECK(write(admin.ctl_fd, &forged, sizeof(forged)) == (ssize_t)sizeof(forged),
          "could not send a forged mint");
    struct { int32_t status; uint32_t pad; RxCapRef ref; } reply;
    memset(&reply, 0, sizeof(reply));
    CHECK(read(admin.ctl_fd, &reply, sizeof(reply)) == (ssize_t)sizeof(reply),
          "mint did not answer the forged request");
    CHECK(reply.status == RX_CAP_ERR_UNAUTHORIZED,
          "table-visible office minted without the office token (%d)", reply.status);
    rx_caproot_stop(&root, &admin);
}

static void t_counter_fail_closed(void) {
    begin("counters_fail_closed", "I3");
    uint32_t gen = 0;
    CHECK(rx_cap_generation_advance(UINT32_MAX, &gen) == RX_CAP_ERR_EXHAUSTED,
          "generation wrap was accepted");
    CHECK(gen == 0, "exhausted generation was written");
    CHECK(rx_cap_generation_advance(UINT32_MAX - 1, &gen) == RX_CAP_OK, "last generation refused");
    CHECK(gen == UINT32_MAX, "last generation value");
    uint64_t sum = 1;
    CHECK(rx_cap_add_u64(UINT64_MAX, 1, &sum) == RX_CAP_ERR_OVERFLOW, "u64 wrap was accepted");
    CHECK(sum == 1, "overflow wrote a sum");
    CHECK(rx_cap_add_u64(UINT64_MAX - 5, 5, &sum) == RX_CAP_OK, "exact top of range refused");
    CHECK(sum == UINT64_MAX, "exact sum");
    CHECK(rx_cap_add_u64(UINT64_MAX - 5, 6, &sum) == RX_CAP_ERR_OVERFLOW, "one past the top was accepted");

    RxCapRoot root;
    RxCapAdmin admin;
    CHECK(rx_caproot_start(&root, &admin) == RX_CAP_OK, "root");
    RxCapRef office = office_of(&admin);
    RxCapMint huge;
    memset(&huge, 0, sizeof(huge));
    huge.issuer = ISSUER_AEGIS_POLICY;
    huge.subject = SUBJ_AIEN;
    huge.resource = RES_SCRATCH;
    huge.rights = RX_RIGHT_READ;
    huge.parent = (RxCapRef){ UINT32_MAX, 0 };
    huge.authority = office;
    int filled = 0;
    for (uint32_t i = 0; i < RX_CAP_MAX + 2; i++) {
        int rc = rx_capadmin_mint(&admin, &huge, NULL);
        if (rc == RX_CAP_OK) filled++;
        else {
            CHECK(rc == RX_CAP_ERR_FULL, "table exhaustion returned %s", rx_cap_strerror(rc));
            break;
        }
    }
    CHECK(filled == (int)RX_CAP_MAX - 1, "filled %d slots, want %u", filled, RX_CAP_MAX - 1);
    rx_caproot_stop(&root, &admin);

    CHECK(rx_caproot_start(&root, &admin) == RX_CAP_OK, "clock root");
    office = office_of(&admin);
    huge.authority = office;
    CHECK(rx_capadmin_advance_clock(&admin, office, UINT64_MAX - 8) == RX_CAP_OK, "clock near the top");
    CHECK(rx_capadmin_advance_clock(&admin, office, 100) == RX_CAP_ERR_OVERFLOW, "clock wrap was accepted");
    huge.lease_ticks = 100;
    huge.resource = RES_SENSOR;
    CHECK(rx_capadmin_mint(&admin, &huge, NULL) == RX_CAP_ERR_OVERFLOW, "lease wrap was accepted");
    huge.lease_ticks = 1;
    RxCapRef leased;
    CHECK(rx_capadmin_mint(&admin, &huge, &leased) == RX_CAP_OK, "in-range lease refused");
    CHECK(rx_capadmin_bump_epoch(&admin, office) == RX_CAP_OK, "epoch step refused");
    CHECK(rx_caproot_validate(&root, leased, SUBJ_AIEN, RES_SENSOR, RX_RIGHT_READ, NULL) == RX_CAP_ERR_EPOCH,
          "pre-epoch lease survived");
    CHECK(rx_capadmin_bump_epoch(&admin, office) == RX_CAP_ERR_EPOCH,
          "stale office advanced the epoch again");
    rx_caproot_stop(&root, &admin);
}

static void t_root_death_restart(void) {
    begin("root_death_and_restart", "I3");
    RxCapRoot root;
    RxCapAdmin admin;
    CHECK(rx_caproot_start(&root, &admin) == RX_CAP_OK, "root");
    RxCapRef old_office = office_of(&admin);
    RxCapMint m;
    memset(&m, 0, sizeof(m));
    m.issuer = ISSUER_AEGIS_POLICY;
    m.subject = SUBJ_AIEN;
    m.resource = RES_SENSOR;
    m.rights = RX_RIGHT_READ;
    m.parent = (RxCapRef){ UINT32_MAX, 0 };
    m.authority = old_office;
    RxCapRef old;
    CHECK(rx_capadmin_mint(&admin, &m, &old) == RX_CAP_OK, "mint");
    pid_t pid = root.root_pid;
    CHECK(kill(pid, SIGKILL) == 0, "could not stop the mint process");
    int st = 0;
    waitpid(pid, &st, 0);
    alarm(3);
    int still = rx_caproot_validate(&root, old, SUBJ_AIEN, RES_SENSOR, RX_RIGHT_READ, NULL);
    alarm(0);
    CHECK(still == RX_CAP_OK || still == RX_CAP_ERR_IO || still == RX_CAP_ERR_STALE_GEN,
          "dead root validate returned %s", rx_cap_strerror(still));
    CHECK(rx_capadmin_mint(&admin, &m, NULL) == RX_CAP_ERR_IO, "dead root still minted");
    rx_caproot_stop(&root, &admin);
    CHECK(rx_caproot_start(&root, &admin) == RX_CAP_OK, "restart");
    CHECK(rx_caproot_validate(&root, old, SUBJ_AIEN, RES_SENSOR, RX_RIGHT_READ, NULL) == RX_CAP_ERR_STALE_GEN,
          "capability from the dead root validated on the new one");
    CHECK(rx_caproot_validate(&root, old_office, 0, RX_CAP_RES_AUTHORITY, RX_RIGHT_MINT, NULL) != RX_CAP_OK,
          "old office still authorizes the new root");
    void *writable = mmap(NULL, sizeof(RxCapTable), PROT_READ | PROT_WRITE, MAP_SHARED, root.ro_fd, 0);
    CHECK(writable == MAP_FAILED, "restarted root handed out a writable table");
    if (writable != MAP_FAILED) munmap(writable, sizeof(RxCapTable));
    rx_caproot_stop(&root, &admin);
}

/* ---- R5 resource admission (host) --------------------------------------- */

static volatile int g_hold = 0;

static int fn_hold(RxCtx *c) {
    (void)c;
    while (g_hold) sleep_ms(1);
    return 0;
}

static int fn_noop(RxCtx *c) {
    (void)c;
    return 0;
}

static void t_resource_admission(void) {
    begin("resource_admission_1000", "R5");
    Env *e = calloc(1, sizeof(*e));
    CHECK(e != NULL, "allocation");
    if (!e) return;
    CHECK(env_start(e, 2) == 0, "setup");
    RxResourceBudget b;
    memset(&b, 0, sizeof(b));
    b.slots = 4;
    b.memory_bytes = 40;
    b.offered_accel = 0x1u;
    rx_world_set_resources(&e->w, &b);

    RxObjRef sensor = mkobj(e, RES_SENSOR, 0);
    RxCapRef ext = mint(e, SUBJ_EXTERNAL, RES_SENSOR, RX_RIGHT_WRITE);
    RxCapRef rd = mint(e, SUBJ_AIEN, RES_SENSOR, RX_RIGHT_READ);
    g_hold = 1;
    for (int i = 0; i < 4; i++) {
        RxReactionDesc d;
        desc_init(&d, "hold.bg", RX_FACULTY_AIEN, SUBJ_AIEN, fn_hold, NULL);
        d.priority = RX_PRIO_BACKGROUND;
        add_trigger(&d, sensor, RX_FIELD(0));
        add_cap(&d, rd, RES_SENSOR, RX_RIGHT_READ);
        CHECK(rx_world_add_reaction(&e->w, &d, NULL) == RX_OK, "bg hold");
    }
    for (int i = 0; i < 4; i++) {
        RxReactionDesc d;
        desc_init(&d, "hold.crit", RX_FACULTY_AIEN, SUBJ_AIEN, fn_hold, NULL);
        d.priority = RX_PRIO_CRITICAL;
        add_trigger(&d, sensor, RX_FIELD(0));
        add_cap(&d, rd, RES_SENSOR, RX_RIGHT_READ);
        CHECK(rx_world_add_reaction(&e->w, &d, NULL) == RX_OK, "crit hold");
    }
    stimulus(e, ext, sensor, 0, 1);
    int saw = 0;
    for (int n = 0; n < 200; n++) {
        int crit = 0, bg = 0;
        for (uint32_t i = 0; i < e->w.n_reactions; i++) {
            RxState s = e->w.reactions[i].state;
            if (e->w.reactions[i].desc.priority == RX_PRIO_CRITICAL &&
                (s == RX_RUNNING || s == RX_READY || s == RX_PUBLISHING))
                crit++;
            if (e->w.reactions[i].desc.priority == RX_PRIO_BACKGROUND &&
                s == RX_BLOCKED_RESOURCE)
                bg++;
        }
        if (crit == 4 && bg == 4) { saw = 1; break; }
        sleep_ms(5);
    }
    g_hold = 0;
    CHECK(saw, "critical work did not take the scarce slots ahead of background");
    CHECK(rx_world_wait_quiescent(&e->w, 5000) == RX_OK, "holders did not finish");

    uint32_t base = e->w.n_reactions;
    for (uint32_t i = 0; i < 1000; i++) {
        RxReactionDesc d;
        desc_init(&d, "crowd", RX_FACULTY_AIEN, SUBJ_AIEN, fn_noop, NULL);
        d.priority = i % RX_PRIORITY_CLASSES;
        d.need.memory_bytes = 10;
        add_trigger(&d, sensor, RX_FIELD(0));
        add_cap(&d, rd, RES_SENSOR, RX_RIGHT_READ);
        CHECK(rx_world_add_reaction(&e->w, &d, NULL) == RX_OK, "crowd %u", i);
    }
    RxReactionDesc bad;
    desc_init(&bad, "impossible", RX_FACULTY_AIEN, SUBJ_AIEN, fn_noop, NULL);
    bad.priority = RX_PRIO_CRITICAL;
    bad.need.memory_bytes = 10;
    bad.need.accelerator_features = 0x80000000u;
    add_trigger(&bad, sensor, RX_FIELD(0));
    add_cap(&bad, rd, RES_SENSOR, RX_RIGHT_READ);
    CHECK(rx_world_add_reaction(&e->w, &bad, NULL) == RX_OK, "impossible");
    e->w.peak_slots = 0;
    e->w.peak_blocked = 0;
    stimulus(e, ext, sensor, 0, 2);
    CHECK(rx_world_wait_quiescent(&e->w, 10000) == RX_OK, "crowd did not finish");
    CHECK(e->w.peak_slots <= 4, "admitted %u reactions into 4 slots", e->w.peak_slots);
    CHECK(e->w.peak_blocked >= 996, "only %u waited for resources", e->w.peak_blocked);
    uint32_t ran = 0;
    for (uint32_t i = base; i < base + 1000; i++) {
        CHECK(e->w.reactions[i].activations == 1, "crowd reaction %u ran %llu",
              i, (unsigned long long)e->w.reactions[i].activations);
        ran += (uint32_t)e->w.reactions[i].activations;
    }
    CHECK(ran == 1000, "ran %u of 1000", ran);
    CHECK(e->w.reactions[base + 1000].activations == 0, "unsupported accelerator ran");
    CHECK(e->w.reactions[base + 1000].state == RX_BLOCKED_RESOURCE, "impossible reaction was not blocked");
    g_illegal_transitions += e->w.stats.illegal_transitions;
    rx_world_destroy(&e->w);
    rx_caproot_stop(&e->root, &e->admin);
    free(e);
}

typedef struct { RxObjRef obj; int left; uint32_t prio; } Spin;

static uint32_t g_log[64];
static uint32_t g_nlog;

static int fn_spin(RxCtx *c) {
    Spin *s = c->user;
    if (g_nlog < 64) g_log[g_nlog++] = s->prio;
    if (s->left > 0) {
        s->left--;
        uint64_t cur = in_of(c, s->obj)->field[0];
        c->out[c->n_out++] = (RxMutation){ s->obj, 0, cur + 1 };
    }
    return 0;
}

static int first_log(uint32_t prio) {
    for (uint32_t i = 0; i < g_nlog; i++)
        if (g_log[i] == prio) return (int)i;
    return -1;
}

static void t_zero_resource_budget(void) {
    begin("zero_resource_budget_is_zero", "R5");
    Env e;
    CHECK(env_start(&e, 1) == 0, "setup");
    RxObjRef sensor = mkobj(&e, RES_SENSOR, 0);
    RxCapRef ext = mint(&e, SUBJ_EXTERNAL, RES_SENSOR, RX_RIGHT_WRITE);
    RxCapRef rd = mint(&e, SUBJ_AIEN, RES_SENSOR, RX_RIGHT_READ);

    RxReactionDesc d;
    desc_init(&d, "zero.budget", RX_FACULTY_AIEN, SUBJ_AIEN, fn_noop, NULL);
    d.need.memory_bytes = 1;
    add_trigger(&d, sensor, RX_FIELD(0));
    add_cap(&d, rd, RES_SENSOR, RX_RIGHT_READ);
    uint32_t id;
    CHECK(rx_world_add_reaction(&e.w, &d, &id) == RX_OK, "reaction");

    RxResourceBudget b;
    memset(&b, 0, sizeof(b));
    rx_world_set_resources(&e.w, &b);
    stimulus(&e, ext, sensor, 0, 1);
    CHECK(rx_world_wait_quiescent(&e.w, 100) == RX_OK, "zero-budget world did not settle");
    CHECK(e.w.reactions[id].activations == 0, "zero slots/memory admitted work");
    CHECK(e.w.reactions[id].state == RX_BLOCKED_RESOURCE, "reaction not blocked at zero budget");

    b.slots = 1;
    b.memory_bytes = 1;
    rx_world_set_resources(&e.w, &b);
    CHECK(rx_world_wait_quiescent(&e.w, 5000) == RX_OK, "restored budget did not run work");
    CHECK(e.w.reactions[id].activations == 1, "restored budget ran %llu activations",
          (unsigned long long)e.w.reactions[id].activations);
    audit_and_close(&e);
}

static void t_background_not_starved(void) {
    begin("background_not_starved", "R5");
    Env e;
    CHECK(env_start(&e, 1) == 0, "setup");
    RxResourceBudget b;
    memset(&b, 0, sizeof(b));
    b.slots = 1;
    b.starvation_bound = 4;
    rx_world_set_resources(&e.w, &b);
    static Spin crit, bg;
    crit.obj = mkobj(&e, RES_TICK, 0);
    bg.obj = crit.obj;
    crit.left = 12;
    bg.left = 0;
    crit.prio = RX_PRIO_CRITICAL;
    bg.prio = RX_PRIO_BACKGROUND;
    RxCapRef ext = mint(&e, SUBJ_EXTERNAL, RES_TICK, RX_RIGHT_WRITE);
    RxCapRef rd = mint(&e, SUBJ_AIEN, RES_TICK, RX_RIGHT_READ);
    RxCapRef wr = mint(&e, SUBJ_AIEN, RES_TICK, RX_RIGHT_WRITE);
    RxReactionDesc d;
    desc_init(&d, "spin.bg", RX_FACULTY_AIEN, SUBJ_AIEN, fn_spin, &bg);
    d.priority = RX_PRIO_BACKGROUND;
    add_trigger(&d, crit.obj, RX_FIELD(0));
    add_cap(&d, rd, RES_TICK, RX_RIGHT_READ);
    add_cap(&d, wr, RES_TICK, RX_RIGHT_WRITE);
    CHECK(rx_world_add_reaction(&e.w, &d, NULL) == RX_OK, "bg");
    desc_init(&d, "spin.crit", RX_FACULTY_AIEN, SUBJ_AIEN, fn_spin, &crit);
    d.priority = RX_PRIO_CRITICAL;
    add_trigger(&d, crit.obj, RX_FIELD(0));
    add_write(&d, crit.obj, RX_FIELD(0));
    add_cap(&d, rd, RES_TICK, RX_RIGHT_READ);
    add_cap(&d, wr, RES_TICK, RX_RIGHT_WRITE);
    CHECK(rx_world_add_reaction(&e.w, &d, NULL) == RX_OK, "crit");
    g_nlog = 0;
    stimulus(&e, ext, crit.obj, 0, 1);
    CHECK(rx_world_wait_quiescent(&e.w, 5000) == RX_OK, "spin did not finish");
    int at = first_log(RX_PRIO_BACKGROUND);
    CHECK(at >= 0, "background never ran");
    CHECK(at <= 5, "background waited %d higher admissions (bound 4)", at);
    CHECK(at > 0, "background jumped the critical work");
    audit_and_close(&e);
}

static void t_priority_ladder(void) {
    begin("priority_ladder", "R5");
    Env e;
    CHECK(env_start(&e, 1) == 0, "setup");
    RxResourceBudget b;
    memset(&b, 0, sizeof(b));
    b.slots = 1;
    b.starvation_bound = 3;
    rx_world_set_resources(&e.w, &b);
    g_hold = 1;
    RxObjRef gate = mkobj(&e, RES_SCRATCH, 0);
    RxObjRef tick = mkobj(&e, RES_TICK, 0);
    RxCapRef ext_g = mint(&e, SUBJ_EXTERNAL, RES_SCRATCH, RX_RIGHT_WRITE);
    RxCapRef rd_g = mint(&e, SUBJ_AIEN, RES_SCRATCH, RX_RIGHT_READ);
    RxCapRef ext = mint(&e, SUBJ_EXTERNAL, RES_TICK, RX_RIGHT_WRITE);
    RxCapRef rd = mint(&e, SUBJ_AIEN, RES_TICK, RX_RIGHT_READ);
    RxCapRef wr = mint(&e, SUBJ_AIEN, RES_TICK, RX_RIGHT_WRITE);
    RxReactionDesc d;
    desc_init(&d, "ladder.hold", RX_FACULTY_AIEN, SUBJ_AIEN, fn_hold, NULL);
    d.priority = RX_PRIO_CRITICAL;
    add_trigger(&d, gate, RX_FIELD(0));
    add_cap(&d, rd_g, RES_SCRATCH, RX_RIGHT_READ);
    CHECK(rx_world_add_reaction(&e.w, &d, NULL) == RX_OK, "hold");
    static Spin spins[RX_PRIORITY_CLASSES];
    for (uint32_t p = 0; p < RX_PRIORITY_CLASSES; p++) {
        spins[p].obj = tick;
        spins[p].left = (p == RX_PRIO_CRITICAL) ? 8 : 0;
        spins[p].prio = p;
        desc_init(&d, "ladder", RX_FACULTY_AIEN, SUBJ_AIEN, fn_spin, &spins[p]);
        d.priority = p;
        add_trigger(&d, tick, RX_FIELD(0));
        add_cap(&d, rd, RES_TICK, RX_RIGHT_READ);
        if (p == RX_PRIO_CRITICAL) {
            add_write(&d, tick, RX_FIELD(0));
            add_cap(&d, wr, RES_TICK, RX_RIGHT_WRITE);
        }
        CHECK(rx_world_add_reaction(&e.w, &d, NULL) == RX_OK, "class %u", p);
    }
    g_nlog = 0;
    stimulus(&e, ext_g, gate, 0, 1);
    sleep_ms(20);
    stimulus(&e, ext, tick, 0, 1);
    sleep_ms(20);
    g_hold = 0;
    CHECK(rx_world_wait_quiescent(&e.w, 5000) == RX_OK, "ladder did not finish");
    int prev = -1;
    for (uint32_t p = 0; p < RX_PRIORITY_CLASSES; p++) {
        int at = first_log(p);
        CHECK(at >= 0, "class %u never ran", p);
        if (at >= 0 && prev >= 0)
            CHECK(at > prev, "class %u ran before a more urgent class (%d <= %d)", p, at, prev);
        if (at >= 0) prev = at;
    }
    CHECK(first_log(RX_PRIO_BACKGROUND) <= 12, "background waited %d admissions",
          first_log(RX_PRIO_BACKGROUND));
    g_illegal_transitions += e.w.stats.illegal_transitions;
    rx_world_destroy(&e.w);
    rx_caproot_stop(&e.root, &e.admin);
}

/* ---- R6 stability (host) ------------------------------------------------ */

static int fn_flip(RxCtx *c) {
    RxObjRef *o = c->user;
    uint64_t v = in_of(c, *o)->field[0];
    c->out[c->n_out++] = (RxMutation){ *o, 0, v ^ 1ull };
    return 0;
}

static int fn_down(RxCtx *c) {
    RxObjRef *o = c->user;
    uint64_t v = in_of(c, *o)->field[0];
    if (v > 0) c->out[c->n_out++] = (RxMutation){ *o, 0, v - 1 };
    return 0;
}

static int fn_up_to(RxCtx *c) {
    RxObjRef *o = c->user;
    uint64_t v = in_of(c, *o)->field[0];
    if (v < 5) c->out[c->n_out++] = (RxMutation){ *o, 0, v + 1 };
    return 0;
}

static int fn_const(RxCtx *c) {
    RxObjRef *o = c->user;
    c->out[c->n_out++] = (RxMutation){ *o, 0, 1 };
    return 0;
}

static void t_stability(void) {
    begin("stability_containment", "R6");
    Env e;
    CHECK(env_start(&e, 1) == 0, "setup");
    RxStabilityBudget s;
    memset(&s, 0, sizeof(s));
    s.fanout_limit = 5;
    s.oscillation_limit = 4;
    s.livelock_limit = 3;
    rx_world_set_stability(&e.w, &s);

    RxObjRef fan = mkobj(&e, RES_FAN_SRC, 0);
    RxCapRef ext = mint(&e, SUBJ_EXTERNAL, RES_FAN_SRC, RX_RIGHT_WRITE);
    RxCapRef rd = mint(&e, SUBJ_AIEN, RES_FAN_SRC, RX_RIGHT_READ);
    uint32_t fan_ids[40];
    for (int i = 0; i < 40; i++) {
        RxReactionDesc d;
        desc_init(&d, "fan.bomb", RX_FACULTY_AIEN, SUBJ_AIEN, fn_noop, NULL);
        d.priority = (uint32_t)(i % RX_PRIORITY_CLASSES);
        add_trigger(&d, fan, RX_FIELD(0));
        add_cap(&d, rd, RES_FAN_SRC, RX_RIGHT_READ);
        CHECK(rx_world_add_reaction(&e.w, &d, &fan_ids[i]) == RX_OK, "fan");
    }
    stimulus(&e, ext, fan, 0, 1);
    CHECK(rx_world_wait_quiescent(&e.w, 5000) == RX_OK, "fanout did not settle");
    uint32_t fan_ran = 0, fan_crit = 0;
    for (int i = 0; i < 40; i++) {
        fan_ran += (uint32_t)e.w.reactions[fan_ids[i]].activations;
        if (e.w.reactions[fan_ids[i]].desc.priority == RX_PRIO_CRITICAL)
            fan_crit += (uint32_t)e.w.reactions[fan_ids[i]].activations;
    }
    CHECK(fan_ran == 40, "fanout delivered only %u of 40 dependent reactions", fan_ran);
    CHECK(fan_crit >= 5, "critical dependents did not execute (%u)", fan_crit);
    CHECK(e.w.stats.deferred_wakes >= 35, "fanout deferred only %llu wakes",
          (unsigned long long)e.w.stats.deferred_wakes);
    CHECK(e.w.stats.deferred_peak >= 35, "deferred peak %llu",
          (unsigned long long)e.w.stats.deferred_peak);

    RxObjRef flip = mkobj(&e, RES_PAIR, 0);
    RxCapRef ext_f = mint(&e, SUBJ_EXTERNAL, RES_PAIR, RX_RIGHT_WRITE);
    RxCapRef rd_f = mint(&e, SUBJ_AIEN, RES_PAIR, RX_RIGHT_READ);
    RxCapRef wr_f = mint(&e, SUBJ_AIEN, RES_PAIR, RX_RIGHT_WRITE);
    static RxObjRef flip_o, down_o, up_o;
    flip_o = flip;
    RxReactionDesc d;
    desc_init(&d, "osc.flip", RX_FACULTY_AIEN, SUBJ_AIEN, fn_flip, &flip_o);
    add_trigger(&d, flip, RX_FIELD(0));
    add_write(&d, flip, RX_FIELD(0));
    add_cap(&d, rd_f, RES_PAIR, RX_RIGHT_READ);
    add_cap(&d, wr_f, RES_PAIR, RX_RIGHT_WRITE);
    uint32_t flip_id;
    CHECK(rx_world_add_reaction(&e.w, &d, &flip_id) == RX_OK, "flip");
    stimulus(&e, ext_f, flip, 0, 1);
    CHECK(rx_world_wait_quiescent(&e.w, 5000) == RX_OK, "oscillation did not settle");
    uint64_t flip_acts = e.w.reactions[flip_id].activations;
    CHECK(e.w.reactions[flip_id].quarantined, "oscillation was not quarantined");
    CHECK(flip_acts > 0 && flip_acts < 30, "oscillation ran %llu times",
          (unsigned long long)flip_acts);
    stimulus(&e, ext_f, flip, 0, field(&e, flip, 0) ^ 1ull);
    CHECK(rx_world_wait_quiescent(&e.w, 5000) == RX_OK, "lift did not settle");
    CHECK(e.w.reactions[flip_id].activations > flip_acts, "external cause did not resume the reaction");

    down_o = mkobj(&e, RES_CHECK, 0);
    RxCapRef ext_d = mint(&e, SUBJ_EXTERNAL, RES_CHECK, RX_RIGHT_WRITE);
    RxCapRef rd_d = mint(&e, SUBJ_AIEN, RES_CHECK, RX_RIGHT_READ);
    RxCapRef wr_d = mint(&e, SUBJ_AIEN, RES_CHECK, RX_RIGHT_WRITE);
    desc_init(&d, "converge", RX_FACULTY_AIEN, SUBJ_AIEN, fn_down, &down_o);
    add_trigger(&d, down_o, RX_FIELD(0));
    add_write(&d, down_o, RX_FIELD(0));
    add_cap(&d, rd_d, RES_CHECK, RX_RIGHT_READ);
    add_cap(&d, wr_d, RES_CHECK, RX_RIGHT_WRITE);
    uint32_t down_id;
    CHECK(rx_world_add_reaction(&e.w, &d, &down_id) == RX_OK, "down");
    stimulus(&e, ext_d, down_o, 0, 8);
    CHECK(rx_world_wait_quiescent(&e.w, 5000) == RX_OK, "convergent loop did not settle");
    CHECK(field(&e, down_o, 0) == 0, "convergent loop did not reach the fixed point");
    CHECK(!e.w.reactions[down_id].quarantined, "convergent loop was quarantined");

    up_o = mkobj(&e, RES_SCRATCH, 0);
    RxCapRef ext_u = mint(&e, SUBJ_EXTERNAL, RES_SCRATCH, RX_RIGHT_WRITE);
    RxCapRef rd_u = mint(&e, SUBJ_AIEN, RES_SCRATCH, RX_RIGHT_READ);
    RxCapRef wr_u = mint(&e, SUBJ_AIEN, RES_SCRATCH, RX_RIGHT_WRITE);
    desc_init(&d, "periodic", RX_FACULTY_AIEN, SUBJ_AIEN, fn_up_to, &up_o);
    add_trigger(&d, up_o, RX_FIELD(0));
    add_write(&d, up_o, RX_FIELD(0));
    add_cap(&d, rd_u, RES_SCRATCH, RX_RIGHT_READ);
    add_cap(&d, wr_u, RES_SCRATCH, RX_RIGHT_WRITE);
    uint32_t up_id;
    CHECK(rx_world_add_reaction(&e.w, &d, &up_id) == RX_OK, "up");
    /* activation budget is 6; counting to 5 must still finish */
    stimulus(&e, ext_u, up_o, 0, 1);
    CHECK(rx_world_wait_quiescent(&e.w, 5000) == RX_OK, "periodic loop did not settle");
    CHECK(field(&e, up_o, 0) == 5, "periodic loop stopped early at %llu",
          (unsigned long long)field(&e, up_o, 0));
    CHECK(!e.w.reactions[up_id].quarantined, "periodic loop was quarantined");

    RxObjRef bell = mkobj(&e, RES_RISK, 0);
    RxObjRef sink = mkobj(&e, RES_PLAN, 0);
    RxCapRef ext_b = mint(&e, SUBJ_EXTERNAL, RES_RISK, RX_RIGHT_WRITE);
    RxCapRef rd_b = mint(&e, SUBJ_AIEN, RES_RISK, RX_RIGHT_READ);
    RxCapRef wr_b = mint(&e, SUBJ_AIEN, RES_RISK, RX_RIGHT_WRITE);
    RxCapRef rd_p = mint(&e, SUBJ_AIEN, RES_PLAN, RX_RIGHT_READ);
    RxCapRef wr_p = mint(&e, SUBJ_AIEN, RES_PLAN, RX_RIGHT_WRITE);
    static RxObjRef sink_o;
    sink_o = sink;
    desc_init(&d, "livelock.spin", RX_FACULTY_AIEN, SUBJ_AIEN, fn_const, &sink_o);
    add_trigger(&d, bell, RX_FIELD(0));
    add_write(&d, sink, RX_FIELD(0));
    add_cap(&d, rd_b, RES_RISK, RX_RIGHT_READ);
    add_cap(&d, wr_p, RES_PLAN, RX_RIGHT_WRITE);
    uint32_t spin_id;
    CHECK(rx_world_add_reaction(&e.w, &d, &spin_id) == RX_OK, "spin");
    static Spin metro;
    metro.obj = bell;
    metro.left = 10;
    metro.prio = RX_PRIO_FOREGROUND;
    desc_init(&d, "livelock.metro", RX_FACULTY_OMEGA, SUBJ_AIEN, fn_spin, &metro);
    d.priority = RX_PRIO_FOREGROUND;
    add_trigger(&d, bell, RX_FIELD(0));
    add_write(&d, bell, RX_FIELD(0));
    add_cap(&d, rd_b, RES_RISK, RX_RIGHT_READ);
    add_cap(&d, wr_b, RES_RISK, RX_RIGHT_WRITE);
    CHECK(rx_world_add_reaction(&e.w, &d, NULL) == RX_OK, "metro");
    (void)rd_p;
    stimulus(&e, ext_b, bell, 0, 1);
    CHECK(rx_world_wait_quiescent(&e.w, 5000) == RX_OK, "livelock did not settle");
    CHECK(e.w.reactions[spin_id].quarantined, "noop loop was not quarantined");
    uint64_t spin_acts = e.w.reactions[spin_id].activations;
    CHECK(spin_acts > 0 && spin_acts <= 4, "noop loop ran %llu times",
          (unsigned long long)spin_acts);
    CHECK(e.w.stats.livelock_trips >= 1, "livelock was not counted");
    CHECK(e.w.stats.useful_commits > 0, "useful work was not counted separately from churn");

    s.activation_budget = 4;
    rx_world_set_stability(&e.w, &s);
    static Spin bounded;
    bounded.obj = mkobj(&e, RES_SENSOR, 0);
    bounded.left = 30;
    bounded.prio = RX_PRIO_FOREGROUND;
    RxCapRef ext_s = mint(&e, SUBJ_EXTERNAL, RES_SENSOR, RX_RIGHT_WRITE);
    RxCapRef rd_s = mint(&e, SUBJ_AIEN, RES_SENSOR, RX_RIGHT_READ);
    RxCapRef wr_s = mint(&e, SUBJ_AIEN, RES_SENSOR, RX_RIGHT_WRITE);
    desc_init(&d, "budget", RX_FACULTY_AIEN, SUBJ_AIEN, fn_spin, &bounded);
    add_trigger(&d, bounded.obj, RX_FIELD(0));
    add_write(&d, bounded.obj, RX_FIELD(0));
    add_cap(&d, rd_s, RES_SENSOR, RX_RIGHT_READ);
    add_cap(&d, wr_s, RES_SENSOR, RX_RIGHT_WRITE);
    uint32_t budget_id;
    CHECK(rx_world_add_reaction(&e.w, &d, &budget_id) == RX_OK, "budget");
    stimulus(&e, ext_s, bounded.obj, 0, 1);
    CHECK(rx_world_wait_quiescent(&e.w, 5000) == RX_OK, "budgeted reaction did not settle");
    CHECK(e.w.reactions[budget_id].activations == 4, "activation budget ran %llu",
          (unsigned long long)e.w.reactions[budget_id].activations);
    CHECK(e.w.reactions[budget_id].quarantined, "activation budget did not quarantine");
    uint64_t before_episode = e.w.reactions[budget_id].activations;
    stimulus(&e, ext_s, bounded.obj, 0, field(&e, bounded.obj, 0) + 1);
    CHECK(rx_world_wait_quiescent(&e.w, 5000) == RX_OK, "fresh causal episode did not settle");
    CHECK(e.w.reactions[budget_id].activations == before_episode + 4,
          "fresh external episode did not receive a fresh activation budget (%llu -> %llu)",
          (unsigned long long)before_episode,
          (unsigned long long)e.w.reactions[budget_id].activations);
    CHECK(e.w.reactions[budget_id].quarantined,
          "second activation episode did not enforce its own bound");
    audit_and_close(&e);
}

typedef struct { RxObjRef obj; int left; } FlipN;

static int fn_flip_n(RxCtx *c) {
    FlipN *s = c->user;
    if (s->left <= 0) return 0;
    s->left--;
    uint64_t v = in_of(c, s->obj)->field[0];
    c->out[c->n_out++] = (RxMutation){ s->obj, 0, v ^ 1ull };
    return 0;
}

static void t_periodic_and_backoff(void) {
    begin("periodic_versus_oscillation", "R6");
    Env e;
    CHECK(env_start(&e, 1) == 0, "setup");
    RxStabilityBudget s;
    memset(&s, 0, sizeof(s));
    s.oscillation_limit = 4;
    s.livelock_limit = 6;
    rx_world_set_stability(&e.w, &s);
    static FlipN period;
    period.obj = mkobj(&e, RES_TICK, 0);
    period.left = 12;
    RxCapRef ext = mint(&e, SUBJ_EXTERNAL, RES_TICK, RX_RIGHT_WRITE);
    RxCapRef rd = mint(&e, SUBJ_AIEN, RES_TICK, RX_RIGHT_READ);
    RxCapRef wr = mint(&e, SUBJ_AIEN, RES_TICK, RX_RIGHT_WRITE);
    RxReactionDesc d;
    desc_init(&d, "periodic.heartbeat", RX_FACULTY_AIEN, SUBJ_AIEN, fn_flip_n, &period);
    d.loop_kind = RX_LOOP_PERIODIC;
    add_trigger(&d, period.obj, RX_FIELD(0));
    add_write(&d, period.obj, RX_FIELD(0));
    add_cap(&d, rd, RES_TICK, RX_RIGHT_READ);
    add_cap(&d, wr, RES_TICK, RX_RIGHT_WRITE);
    uint32_t id;
    CHECK(rx_world_add_reaction(&e.w, &d, &id) == RX_OK, "periodic");
    stimulus(&e, ext, period.obj, 0, 1);
    CHECK(rx_world_wait_quiescent(&e.w, 5000) == RX_OK, "periodic loop did not settle");
    CHECK(!e.w.reactions[id].quarantined, "a declared periodic loop was quarantined");
    CHECK(e.w.reactions[id].activations > 4, "periodic loop ran %llu times",
          (unsigned long long)e.w.reactions[id].activations);
    CHECK(e.w.stats.periodic_commits >= 4, "periodic commits %llu",
          (unsigned long long)e.w.stats.periodic_commits);
    CHECK(e.w.stats.oscillation_trips == 0, "periodic loop was counted as oscillation");

    RxResourceBudget b;
    memset(&b, 0, sizeof(b));
    b.slots = 1;
    b.starvation_bound = 1;
    rx_world_set_resources(&e.w, &b);
    s.oscillation_limit = 0;
    rx_world_set_stability(&e.w, &s);
    g_hold = 1;
    RxObjRef gate = mkobj(&e, RES_SCRATCH, 0);
    RxCapRef ext_g = mint(&e, SUBJ_EXTERNAL, RES_SCRATCH, RX_RIGHT_WRITE);
    RxCapRef rd_g = mint(&e, SUBJ_AIEN, RES_SCRATCH, RX_RIGHT_READ);
    desc_init(&d, "backoff.hold", RX_FACULTY_AIEN, SUBJ_AIEN, fn_hold, NULL);
    d.priority = RX_PRIO_CRITICAL;
    add_trigger(&d, gate, RX_FIELD(0));
    add_cap(&d, rd_g, RES_SCRATCH, RX_RIGHT_READ);
    CHECK(rx_world_add_reaction(&e.w, &d, NULL) == RX_OK, "hold");
    static FlipN hot;
    static Spin cool;
    hot.obj = mkobj(&e, RES_RISK, 0);
    hot.left = 6;
    cool.obj = mkobj(&e, RES_PLAN, 0);
    cool.left = 4;
    cool.prio = RX_PRIO_BACKGROUND;
    RxCapRef ext_h = mint(&e, SUBJ_EXTERNAL, RES_RISK, RX_RIGHT_WRITE);
    RxCapRef rd_h = mint(&e, SUBJ_AIEN, RES_RISK, RX_RIGHT_READ);
    RxCapRef wr_h = mint(&e, SUBJ_AIEN, RES_RISK, RX_RIGHT_WRITE);
    RxCapRef ext_c = mint(&e, SUBJ_EXTERNAL, RES_PLAN, RX_RIGHT_WRITE);
    RxCapRef rd_c = mint(&e, SUBJ_AIEN, RES_PLAN, RX_RIGHT_READ);
    RxCapRef wr_c = mint(&e, SUBJ_AIEN, RES_PLAN, RX_RIGHT_WRITE);
    desc_init(&d, "backoff.hot", RX_FACULTY_AIEN, SUBJ_AIEN, fn_flip_n, &hot);
    d.priority = RX_PRIO_CRITICAL;
    add_trigger(&d, hot.obj, RX_FIELD(0));
    add_write(&d, hot.obj, RX_FIELD(0));
    add_cap(&d, rd_h, RES_RISK, RX_RIGHT_READ);
    add_cap(&d, wr_h, RES_RISK, RX_RIGHT_WRITE);
    uint32_t hot_id;
    CHECK(rx_world_add_reaction(&e.w, &d, &hot_id) == RX_OK, "hot");
    desc_init(&d, "backoff.cool", RX_FACULTY_OMEGA, SUBJ_AIEN, fn_spin, &cool);
    d.priority = RX_PRIO_BACKGROUND;
    add_trigger(&d, cool.obj, RX_FIELD(0));
    add_write(&d, cool.obj, RX_FIELD(0));
    add_cap(&d, rd_c, RES_PLAN, RX_RIGHT_READ);
    add_cap(&d, wr_c, RES_PLAN, RX_RIGHT_WRITE);
    CHECK(rx_world_add_reaction(&e.w, &d, NULL) == RX_OK, "cool");
    stimulus(&e, ext_g, gate, 0, 1);
    sleep_ms(20);
    stimulus(&e, ext_h, hot.obj, 0, 1);
    stimulus(&e, ext_c, cool.obj, 0, 1);
    g_hold = 0;
    CHECK(rx_world_wait_quiescent(&e.w, 5000) == RX_OK, "backoff case did not settle");
    CHECK(field(&e, cool.obj, 0) == 5, "background work did not finish while the hot loop yielded");
    CHECK(e.w.reactions[hot_id].activations > 0, "hot loop never ran");
    CHECK(e.w.stats.backoffs > 0, "hot loop never yielded");
    audit_and_close(&e);
}

/* ---- one identity, then the CPU cross-engine descriptor ------------------- */

static uint32_t ld32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t ld64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static void st32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void st64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static RxObjRef g_note_out;

static int fn_note(RxCtx *c) {
    c->out[c->n_out++] = (RxMutation){ g_note_out, 0, c->in[0].field[0] + 1 };
    return 0;
}

static OmegaSharedWorldDesc make_pub(RxWorld *w, const RxObject *o, RxCapRef cap, uint64_t crumb) {
    OmegaSharedWorldDesc d;
    memset(&d, 0, sizeof(d));
    d.msg_type = RX_RING_PUBLISH;
    d.sequence = rx_world_publication_tail(w);
    d.world_epoch = 1;
    d.object_id = o->id;
    d.object_generation = o->generation;
    d.object_offset = 0;
    d.object_length = (uint32_t)o->size_bytes;
    d.payload_len = 24;
    st32(d.payload, cap.cap_id);
    st32(d.payload + 4, cap.generation);
    st64(d.payload + 8, o->version);
    st64(d.payload + 16, crumb);
    rx_world_seal_descriptor(&d);
    return d;
}

static int inject_take(RxWorld *w, const OmegaSharedWorldDesc *d, uint32_t *fault) {
    if (rx_world_inject_descriptor(w, d) != RX_OK) return RX_ERR_FULL;
    return rx_world_take_publication(w, NULL, fault);
}

static void t_one_identity(void) {
    begin("canonical_object_one_identity", "one id, one generation, one lineage");
    Env e;
    CHECK(env_start(&e, 2) == 0, "setup");
    CHECK(sizeof(OmegaSharedWorldObject) == 32, "physical record changed size");
    CHECK(sizeof(OmegaSharedWorldDesc) == 128, "descriptor changed size");
    RxObjRef obj = mkobj(&e, RES_SENSOR, 0);
    g_note_out = mkobj(&e, RES_BELIEF, 0);
    RxObject sem;
    CHECK(rx_world_read(&e.w, obj, &sem) == RX_OK, "read");
    OmegaSharedWorldObject phys;
    CHECK(rx_world_physical(&e.w, obj, &phys) == RX_ERR_UNPLACED, "a new object already had a physical copy");
    CHECK(!sem.placed && sem.region_offset == 0 && sem.size_bytes == 0, "semantic object carried a placement");
    CHECK(rx_world_attach_physical(&e.w, obj) == RX_OK, "attach");
    CHECK(rx_world_read(&e.w, obj, &sem) == RX_OK, "read after attach");
    CHECK(sem.generation == obj.generation, "attach advanced the generation");
    CHECK(rx_world_physical(&e.w, obj, &phys) == RX_OK, "physical");
    CHECK(phys.object_id == sem.id && phys.object_id == obj.id, "two ids for one object");
    CHECK(phys.generation == sem.generation && sem.generation == 1, "create opened a second generation");
    CHECK(phys.permissions == 0, "shared record carried a permission bit");
    CHECK(phys.region_offset == sem.region_offset && phys.size_bytes == sem.size_bytes, "placement diverged");
    CHECK(sem.size_bytes == RX_OBJECT_WINDOW, "window length");
    CHECK(sem.region_offset > sem.id && sem.region_offset < e.w.coherent_bytes, "placement is not inside the shared image");
    CHECK(sem.placement == RX_PLACE_COHERENT && sem.locality == RX_LOCALITY_MACHINE, "locality");
    CHECK(sem.coherency == RX_COHERENCY_HOST, "coherency");
    CHECK(sem.cap.cap_id == 0 && sem.cap.generation == 0, "unbound object named a capability");

    RxCapRef cap = mint(&e, SUBJ_EXTERNAL, RES_SENSOR, RX_RIGHT_READ | RX_RIGHT_WRITE);
    RxCapRef rd = mint(&e, SUBJ_AIEN, RES_SENSOR, RX_RIGHT_READ);
    RxCapRef wr = mint(&e, SUBJ_AIEN, RES_BELIEF, RX_RIGHT_WRITE);
    CHECK(rx_world_bind_capability(&e.w, obj, cap) == RX_OK, "bind");
    RxReactionDesc d;
    desc_init(&d, "note", RX_FACULTY_AIEN, SUBJ_AIEN, fn_note, NULL);
    add_trigger(&d, obj, RX_FIELD(0));
    add_write(&d, g_note_out, RX_FIELD(0));
    add_cap(&d, rd, RES_SENSOR, RX_RIGHT_READ);
    add_cap(&d, wr, RES_BELIEF, RX_RIGHT_WRITE);
    uint32_t rid = 0;
    CHECK(rx_world_add_reaction(&e.w, &d, &rid) == RX_OK, "add note");

    int64_t cid = stimulus(&e, cap, obj, 0, 5);
    CHECK(cid > 0, "stimulus");
    CHECK(rx_world_wait_quiescent(&e.w, 3000) == RX_OK, "quiet");
    CHECK(field(&e, g_note_out, 0) == 6, "host reaction did not wake");
    CHECK(e.w.reactions[rid].commits == 1, "host reaction did not publish");

    OmegaSharedWorldDesc got;
    uint32_t fault = 99;
    CHECK(rx_world_take_publication(&e.w, &got, &fault) == RX_OK, "publication was not accepted");
    CHECK(fault == 0, "fresh publication raised a fault");
    CHECK(got.msg_type == RX_RING_PUBLISH, "ring was not a publication");
    CHECK(got.object_id == obj.id && got.object_generation == obj.generation, "descriptor named a different object");
    RxObject after;
    CHECK(rx_world_read(&e.w, obj, &after) == RX_OK, "reread");
    CHECK(after.generation == obj.generation, "publication bumped the generation");
    CHECK(after.version > sem.version, "publication did not advance the canonical version");
    CHECK(ld64(got.payload + 8) == after.version, "descriptor version differs from the object");
    CHECK(ld64(got.payload + 16) == (uint64_t)cid, "descriptor lost the causal record");
    CHECK(ld32(got.payload) == cap.cap_id && ld32(got.payload + 4) == cap.generation, "capability reference differs");
    CHECK(rx_world_explain(&e.w, obj, 0) == (uint64_t)cid, "field history points elsewhere");
    const RxCrumb *k = rx_world_crumb(&e.w, (uint64_t)cid);
    CHECK(k && k->kind == RX_CRUMB_EXTERNAL, "stimulus crumb");
    CHECK(k->n_outputs == 1 && k->outputs[0].obj.id == got.object_id &&
          k->outputs[0].obj.generation == got.object_generation, "lineage broke");
    CHECK(rx_world_take_publication(&e.w, NULL, &fault) == RX_ERR_NOT_FOUND, "a second object was published");

    uint32_t old_gen = obj.generation;
    CHECK(rx_world_retire(&e.w, obj) == RX_OK, "retire");
    RxObjRef neu = { obj.id, e.w.objects[obj.id].generation };
    CHECK(neu.generation == old_gen + 1, "one generation counter did not advance");
    OmegaSharedWorldObject retired;
    CHECK(rx_world_physical(&e.w, neu, &retired) == RX_OK, "retired projection missing");
    CHECK(retired.generation == neu.generation && retired.object_id == obj.id, "projection has its own generation");
    CHECK(retired.state == OMEGA_SW_OBJ_REVOKED && retired.permissions == 0, "retired record");
    OmegaSharedWorldDesc stale = got;
    stale.sequence = rx_world_publication_tail(&e.w);
    stale.object_generation = old_gen;
    rx_world_seal_descriptor(&stale);
    CHECK(inject_take(&e.w, &stale, &fault) == RX_ERR_STALE_GEN, "stale generation was accepted");
    CHECK(rx_world_read(&e.w, obj, &after) == RX_ERR_STALE_GEN, "host still honored the retired generation");
    RxObjRef again = mkobj(&e, RES_SENSOR, 0);
    CHECK(again.id == obj.id && again.generation == neu.generation, "reuse opened a second identity");
    CHECK(rx_world_physical(&e.w, obj, &retired) == RX_ERR_STALE_GEN, "old physical name survived reuse");
    CHECK(rx_world_physical(&e.w, again, &retired) == RX_ERR_UNPLACED, "reused object inherited the old placement");
    CHECK(rx_world_read(&e.w, again, &after) == RX_OK && after.generation == again.generation, "reused object");
    audit_and_close(&e);
}

static void t_identity_closed(void) {
    begin("identity_space_is_closed", "one identity space; placement is not required");
    Env e;
    CHECK(env_start(&e, 1) == 0, "setup");
    RxObjRef refs[RX_MAX_OBJECTS];
    uint32_t n = 0;
    for (; n < RX_MAX_OBJECTS; n++) {
        refs[n] = mkobj(&e, RES_SCRATCH, n);
        if (refs[n].id == UINT32_MAX) break;
    }
    CHECK(n == RX_MAX_OBJECTS, "identity space is smaller than the host world");
    RxObjRef extra = mkobj(&e, RES_SCRATCH, 0);
    CHECK(extra.id == UINT32_MAX, "an object was created with nowhere to stand");
    uint32_t bad = 0;
    for (uint32_t i = 0; i < n; i++) {
        RxObject sem;
        OmegaSharedWorldObject phys;
        if (rx_world_read(&e.w, refs[i], &sem) != RX_OK || sem.placed ||
            sem.generation != refs[i].generation ||
            rx_world_physical(&e.w, refs[i], &phys) != RX_ERR_UNPLACED)
            bad++;
    }
    CHECK(bad == 0, "a semantic object was born with a physical copy");
    CHECK(rx_world_attach_physical(&e.w, refs[0]) == RX_OK, "attach one");
    RxObject sem;
    OmegaSharedWorldObject phys;
    CHECK(rx_world_read(&e.w, refs[0], &sem) == RX_OK, "read attached");
    CHECK(rx_world_physical(&e.w, refs[0], &phys) == RX_OK, "physical of one");
    CHECK(phys.object_id == sem.id && phys.generation == sem.generation && phys.permissions == 0,
          "attached record used a different name");
    audit_and_close(&e);
}

static void t_semantic_without_transport(void) {
    begin("semantic_commit_without_transport", "a reaction publishes with no physical copy");
    Env e;
    CHECK(env_start(&e, 2) == 0, "setup");
    RxObjRef obj = mkobj(&e, RES_SENSOR, 0);
    g_note_out = mkobj(&e, RES_BELIEF, 0);
    RxCapRef cap = mint(&e, SUBJ_EXTERNAL, RES_SENSOR, RX_RIGHT_READ | RX_RIGHT_WRITE);
    RxCapRef rd = mint(&e, SUBJ_AIEN, RES_SENSOR, RX_RIGHT_READ);
    RxCapRef wr = mint(&e, SUBJ_AIEN, RES_BELIEF, RX_RIGHT_WRITE);
    CHECK(rx_world_bind_capability(&e.w, obj, cap) == RX_OK, "bind");
    RxReactionDesc d;
    desc_init(&d, "note-semantic", RX_FACULTY_AIEN, SUBJ_AIEN, fn_note, NULL);
    add_trigger(&d, obj, RX_FIELD(0));
    add_write(&d, g_note_out, RX_FIELD(0));
    add_cap(&d, rd, RES_SENSOR, RX_RIGHT_READ);
    add_cap(&d, wr, RES_BELIEF, RX_RIGHT_WRITE);
    uint32_t rid = 0;
    CHECK(rx_world_add_reaction(&e.w, &d, &rid) == RX_OK, "add");
    RxObject before;
    CHECK(rx_world_read(&e.w, obj, &before) == RX_OK, "before");
    int64_t cid = stimulus(&e, cap, obj, 0, 5);
    CHECK(cid > 0, "stimulus");
    CHECK(rx_world_wait_quiescent(&e.w, 3000) == RX_OK, "quiet");
    CHECK(field(&e, g_note_out, 0) == 6, "host reaction did not see the semantic change");
    CHECK(e.w.reactions[rid].commits == 1, "host reaction did not publish");
    uint32_t fault = 0;
    CHECK(rx_world_take_publication(&e.w, NULL, &fault) == RX_ERR_NOT_FOUND,
          "a ring slot appeared for an object with no physical copy");
    RxObject after;
    CHECK(rx_world_read(&e.w, obj, &after) == RX_OK, "after");
    CHECK(after.id == before.id && after.generation == before.generation && after.version > before.version,
          "semantic publication changed the object's name");
    CHECK(!after.placed, "publication invented a placement");
    audit_and_close(&e);
}

static int same_bytes(const uint8_t *a, const uint8_t *b, size_t n) {
    return memcmp(a, b, n) == 0;
}

static void t_physical_realization(void) {
    begin("physical_realization", "attach, detach, relocate; identity stays");
    Env e;
    CHECK(env_start(&e, 1) == 0, "setup");
    RxObjRef obj = mkobj(&e, RES_SENSOR, 4);
    RxObject sem;
    CHECK(rx_world_read(&e.w, obj, &sem) == RX_OK, "read");
    uint8_t digest[32];
    memcpy(digest, sem.digest, 32);
    uint8_t world0[32], world1[32];
    rx_world_digest(&e.w, world0);
    RxObjRef stale = obj;
    stale.generation = 0;
    CHECK(rx_world_attach_physical(&e.w, stale) == RX_ERR_STALE_GEN, "generation 0 was attached");
    stale.generation = obj.generation + 9;
    CHECK(rx_world_attach_physical(&e.w, stale) == RX_ERR_STALE_GEN, "future generation was attached");
    CHECK(rx_world_read(&e.w, obj, &sem) == RX_OK && !sem.placed, "a refused attach placed the object");
    CHECK(same_bytes(sem.digest, digest, 32), "refused attach changed the content");

    CHECK(rx_world_attach_physical(&e.w, obj) == RX_OK, "attach");
    CHECK(rx_world_attach_physical(&e.w, obj) == RX_ERR_EXISTS, "second physical copy was allowed");
    CHECK(rx_world_read(&e.w, obj, &sem) == RX_OK, "read placed");
    CHECK(sem.generation == obj.generation && same_bytes(sem.digest, digest, 32),
          "attach changed identity or content");
    OmegaSharedWorldObject phys;
    CHECK(rx_world_physical(&e.w, obj, &phys) == RX_OK, "physical");
    CHECK(phys.object_id == obj.id && phys.generation == obj.generation && phys.permissions == 0,
          "projection name");
    uint64_t off0 = phys.region_offset;

    CHECK(rx_world_relocate_physical(&e.w, obj) == RX_OK, "relocate");
    CHECK(rx_world_read(&e.w, obj, &sem) == RX_OK, "read moved");
    CHECK(rx_world_physical(&e.w, obj, &phys) == RX_OK, "physical moved");
    CHECK(phys.region_offset != off0 && phys.region_offset == sem.region_offset, "bytes did not move");
    CHECK(phys.object_id == obj.id && phys.generation == obj.generation, "move renamed the object");
    CHECK(sem.version == 1 && same_bytes(sem.digest, digest, 32), "move changed the content");
    rx_world_digest(&e.w, world1);
    CHECK(same_bytes(world0, world1, 32), "move changed the world content");
    CHECK(field(&e, obj, 0) == 4, "move changed a field");

    CHECK(rx_world_place_physical(&e.w, obj, UINT64_MAX, RX_OBJECT_WINDOW) == RX_ERR_BOUNDS,
          "offset at the top of the range was accepted");
    CHECK(rx_world_place_physical(&e.w, obj, sem.region_offset, UINT64_MAX) == RX_ERR_BOUNDS,
          "length at the top of the range was accepted");
    CHECK(rx_world_place_physical(&e.w, obj, UINT64_MAX - 8u, RX_OBJECT_WINDOW) == RX_ERR_BOUNDS,
          "wrapping offset plus length was accepted");
    CHECK(rx_world_read(&e.w, obj, &sem) == RX_OK && sem.region_offset == phys.region_offset &&
              same_bytes(sem.digest, digest, 32),
          "a refused move changed the object");

    OmegaSharedWorldObject lie;
    CHECK(rx_world_physical(&e.w, obj, &lie) == RX_OK, "lie source");
    lie.state = OMEGA_SW_OBJ_REVOKED;
    CHECK(rx_world_overwrite_physical(&e.w, obj.id, &lie) == RX_OK, "poke liveness");
    RxCapRef cap = mint(&e, SUBJ_EXTERNAL, RES_SENSOR, RX_RIGHT_READ | RX_RIGHT_WRITE);
    CHECK(rx_world_bind_capability(&e.w, obj, cap) == RX_OK, "bind");
    int64_t cid = stimulus(&e, cap, obj, 0, 4);
    CHECK(cid > 0, "stimulus");
    CHECK(rx_world_wait_quiescent(&e.w, 2000) == RX_OK, "quiet");
    CHECK(rx_world_read(&e.w, obj, &sem) == RX_OK, "read after poke");
    OmegaSharedWorldDesc bad = make_pub(&e.w, &sem, cap, (uint64_t)cid);
    uint32_t fault = 0;
    CHECK(inject_take(&e.w, &bad, &fault) == RX_ERR_STALE_GEN, "liveness disagreement was accepted");
    CHECK(fault == RX_FAULT_DIVERGED, "disagreement was not reported");
    RxObject still;
    CHECK(rx_world_read(&e.w, obj, &still) == RX_OK, "read after disagreement");
    CHECK(still.generation == sem.generation && still.version == sem.version &&
              still.field[0] == sem.field[0],
          "disagreement changed the object");
    lie.state = OMEGA_SW_OBJ_ACTIVE;
    lie.generation = sem.generation;
    lie.permissions = 0;
    CHECK(rx_world_overwrite_physical(&e.w, obj.id, &lie) == RX_OK, "restore liveness");

    CHECK(rx_world_detach_physical(&e.w, stale) == RX_ERR_STALE_GEN, "stale detach");
    CHECK(rx_world_detach_physical(&e.w, obj) == RX_OK, "detach");
    CHECK(rx_world_detach_physical(&e.w, obj) == RX_ERR_UNPLACED, "second detach");
    CHECK(rx_world_physical(&e.w, obj, &phys) == RX_ERR_UNPLACED, "detached object still resolved");
    CHECK(rx_world_read(&e.w, obj, &still) == RX_OK && still.generation == obj.generation &&
              still.field[0] == 4,
          "detach changed the semantic object");
    OmegaSharedWorldDesc gone = make_pub(&e.w, &still, cap, (uint64_t)cid);
    CHECK(inject_take(&e.w, &gone, &fault) == RX_ERR_UNPLACED, "detached realization was accepted");
    CHECK(rx_world_read(&e.w, obj, &still) == RX_OK && still.field[0] == 4 && still.version == sem.version,
          "rejected descriptor changed the object");
    CHECK(rx_world_attach_physical(&e.w, obj) == RX_OK, "reattach");
    CHECK(rx_world_physical(&e.w, obj, &phys) == RX_OK && phys.object_id == obj.id &&
              phys.generation == obj.generation,
          "reattach used a new name");
    audit_and_close(&e);
}

static void t_cross_engine(void) {
    begin("cross_engine_descriptor_cpu", "pointer-free descriptor, hostile input rejected");
    Env e;
    CHECK(env_start(&e, 1) == 0, "setup");
    RxObjRef obj = mkobj(&e, RES_SENSOR, 1);
    CHECK(rx_world_attach_physical(&e.w, obj) == RX_OK, "attach");
    RxCapRef cap = mint(&e, SUBJ_EXTERNAL, RES_SENSOR, RX_RIGHT_READ | RX_RIGHT_WRITE);
    RxCapRef other = mint(&e, SUBJ_EXTERNAL, RES_BELIEF, RX_RIGHT_READ | RX_RIGHT_WRITE);
    CHECK(rx_world_bind_capability(&e.w, obj, cap) == RX_OK, "bind");
    int64_t cid = stimulus(&e, cap, obj, 0, 9);
    CHECK(cid > 0, "stimulus");
    CHECK(rx_world_wait_quiescent(&e.w, 2000) == RX_OK, "quiet");
    OmegaSharedWorldDesc good;
    uint32_t fault = 99;
    CHECK(rx_world_take_publication(&e.w, &good, &fault) == RX_OK, "good descriptor rejected");
    CHECK(good.msg_type == RX_RING_PUBLISH, "message kind");
    CHECK(good.object_offset == 0 && good.object_length == RX_OBJECT_WINDOW, "window");
    CHECK((good.flags & OMEGA_SW_FLAG_CHECKSUM) != 0, "checksum missing");

    RxObject sem;
    CHECK(rx_world_read(&e.w, obj, &sem) == RX_OK, "read");
    OmegaSharedWorldObject poked;
    CHECK(rx_world_physical(&e.w, obj, &poked) == RX_OK, "physical");
    poked.permissions = OMEGA_SW_PERM_READ | OMEGA_SW_PERM_WRITE;
    CHECK(rx_world_overwrite_physical(&e.w, obj.id, &poked) == RX_OK, "poke permissions");
    OmegaSharedWorldDesc again = make_pub(&e.w, &sem, cap, (uint64_t)cid);
    CHECK(inject_take(&e.w, &again, &fault) == RX_OK, "permission bits were treated as authority");

    poked.generation = sem.generation + 9;
    CHECK(rx_world_overwrite_physical(&e.w, obj.id, &poked) == RX_OK, "poke generation");
    OmegaSharedWorldDesc lied = make_pub(&e.w, &sem, cap, (uint64_t)cid);
    CHECK(inject_take(&e.w, &lied, &fault) == RX_ERR_STALE_GEN, "edited generation became a second object");
    poked.generation = sem.generation;
    poked.permissions = 0;
    CHECK(rx_world_overwrite_physical(&e.w, obj.id, &poked) == RX_OK, "restore");

    OmegaSharedWorldDesc torn = make_pub(&e.w, &sem, cap, (uint64_t)cid);
    torn.payload[30] ^= 0x5a;
    CHECK(inject_take(&e.w, &torn, &fault) == RX_ERR_TORN, "torn descriptor accepted");
    OmegaSharedWorldDesc oldver = make_pub(&e.w, &sem, cap, (uint64_t)cid);
    st64(oldver.payload + 8, 0);
    rx_world_seal_descriptor(&oldver);
    CHECK(inject_take(&e.w, &oldver, &fault) == RX_ERR_TORN, "stale publication version accepted");

    OmegaSharedWorldDesc replay = make_pub(&e.w, &sem, cap, (uint64_t)cid);
    replay.sequence = rx_world_publication_tail(&e.w) - 1;
    rx_world_seal_descriptor(&replay);
    CHECK(inject_take(&e.w, &replay, &fault) == RX_ERR_REPLAY, "replayed sequence accepted");
    OmegaSharedWorldDesc skip = make_pub(&e.w, &sem, cap, (uint64_t)cid);
    skip.sequence += 3;
    rx_world_seal_descriptor(&skip);
    CHECK(inject_take(&e.w, &skip, &fault) == RX_ERR_REPLAY, "skipped sequence accepted");

    OmegaSharedWorldDesc wide = make_pub(&e.w, &sem, cap, (uint64_t)cid);
    wide.object_length = (uint32_t)sem.size_bytes + 1u;
    rx_world_seal_descriptor(&wide);
    CHECK(inject_take(&e.w, &wide, &fault) == RX_ERR_BOUNDS, "length past the object accepted");
    OmegaSharedWorldDesc call = make_pub(&e.w, &sem, cap, (uint64_t)cid);
    call.msg_type = OMEGA_SW_MSG_XFORM_REQ;
    rx_world_seal_descriptor(&call);
    CHECK(inject_take(&e.w, &call, &fault) == RX_ERR_BAD_DESC, "transform call accepted on the reaction ring");
    OmegaSharedWorldDesc magic = make_pub(&e.w, &sem, cap, (uint64_t)cid);
    magic.magic = 0;
    CHECK(inject_take(&e.w, &magic, &fault) == RX_ERR_BAD_DESC, "bad magic accepted");

    OmegaSharedWorldDesc wrong = make_pub(&e.w, &sem, other, (uint64_t)cid);
    CHECK(inject_take(&e.w, &wrong, &fault) == RX_ERR_AUTHORITY, "a different capability was accepted");
    CHECK(revoke_cap(&e.admin, cap) == RX_CAP_OK, "revoke");
    OmegaSharedWorldDesc dead = make_pub(&e.w, &sem, cap, (uint64_t)cid);
    CHECK(inject_take(&e.w, &dead, &fault) == RX_ERR_AUTHORITY, "revoked capability was accepted");

    /* Re-bind a live capability so the remaining refusals are about the
     * descriptor, and confirm each refusal leaves the object untouched. */
    RxCapRef fresh = mint(&e, SUBJ_EXTERNAL, RES_SENSOR, RX_RIGHT_READ | RX_RIGHT_WRITE);
    CHECK(rx_world_bind_capability(&e.w, obj, fresh) == RX_OK, "rebind");
    CHECK(rx_world_read(&e.w, obj, &sem) == RX_OK, "reread for hostile set");
    cid = stimulus(&e, fresh, obj, 0, 11);
    CHECK(cid > 0, "second stimulus");
    CHECK(rx_world_wait_quiescent(&e.w, 2000) == RX_OK, "quiet after second stimulus");
    while (rx_world_take_publication(&e.w, NULL, &fault) == RX_OK) {}
    CHECK(rx_world_read(&e.w, obj, &sem) == RX_OK, "baseline");

    OmegaSharedWorldDesc huge = make_pub(&e.w, &sem, fresh, (uint64_t)cid);
    huge.object_id = UINT32_MAX;
    rx_world_seal_descriptor(&huge);
    CHECK(inject_take(&e.w, &huge, &fault) == RX_ERR_BOUNDS, "object id at the top of the range was accepted");

    OmegaSharedWorldDesc zero = make_pub(&e.w, &sem, fresh, (uint64_t)cid);
    zero.object_generation = 0;
    rx_world_seal_descriptor(&zero);
    CHECK(inject_take(&e.w, &zero, &fault) == RX_ERR_STALE_GEN, "generation zero was accepted");

    OmegaSharedWorldDesc edge = make_pub(&e.w, &sem, fresh, (uint64_t)cid);
    edge.object_offset = (uint32_t)sem.size_bytes - 1u;
    edge.object_length = 1;
    rx_world_seal_descriptor(&edge);
    CHECK(inject_take(&e.w, &edge, &fault) == RX_OK, "last byte of the object was refused");

    OmegaSharedWorldDesc past = make_pub(&e.w, &sem, fresh, (uint64_t)cid);
    past.object_offset = (uint32_t)sem.size_bytes;
    past.object_length = 1;
    rx_world_seal_descriptor(&past);
    CHECK(inject_take(&e.w, &past, &fault) == RX_ERR_BOUNDS, "one byte past the object was accepted");

    OmegaSharedWorldDesc offmax = make_pub(&e.w, &sem, fresh, (uint64_t)cid);
    offmax.object_offset = UINT32_MAX;
    offmax.object_length = 1;
    rx_world_seal_descriptor(&offmax);
    CHECK(inject_take(&e.w, &offmax, &fault) == RX_ERR_BOUNDS, "maximum offset was accepted");

    OmegaSharedWorldDesc lenmax = make_pub(&e.w, &sem, fresh, (uint64_t)cid);
    lenmax.object_offset = 0;
    lenmax.object_length = UINT32_MAX;
    rx_world_seal_descriptor(&lenmax);
    CHECK(inject_take(&e.w, &lenmax, &fault) == RX_ERR_BOUNDS, "maximum length was accepted");

    OmegaSharedWorldDesc wrap = make_pub(&e.w, &sem, fresh, (uint64_t)cid);
    wrap.object_offset = 0xFFFFFFF0u;
    wrap.object_length = 0x20u;
    rx_world_seal_descriptor(&wrap);
    CHECK(inject_take(&e.w, &wrap, &fault) == RX_ERR_BOUNDS, "wrapping offset plus length was accepted");

    OmegaSharedWorldDesc kind = make_pub(&e.w, &sem, fresh, (uint64_t)cid);
    kind.msg_type = 0x00FFu;
    rx_world_seal_descriptor(&kind);
    CHECK(inject_take(&e.w, &kind, &fault) == RX_ERR_BAD_DESC, "unknown ring kind was accepted");

    OmegaSharedWorldDesc keep = make_pub(&e.w, &sem, fresh, (uint64_t)cid);
    keep.msg_type = RX_RING_KEEPALIVE;
    rx_world_seal_descriptor(&keep);
    uint64_t ver = sem.version;
    CHECK(inject_take(&e.w, &keep, &fault) == RX_OK, "keepalive was treated as a call");
    CHECK(rx_world_read(&e.w, obj, &sem) == RX_OK && sem.version == ver, "keepalive changed the object");

    OmegaSharedWorldDesc down = make_pub(&e.w, &sem, fresh, (uint64_t)cid);
    down.msg_type = RX_RING_SHUTDOWN;
    rx_world_seal_descriptor(&down);
    CHECK(inject_take(&e.w, &down, &fault) == RX_OK, "shutdown notice was refused");
    CHECK(rx_world_read(&e.w, obj, &sem) == RX_OK && sem.version == ver && sem.field[0] == 11,
          "shutdown notice changed the object");

    uint32_t old = obj.generation;
    uint64_t held = sem.field[0];
    CHECK(rx_world_retire(&e.w, obj) == RX_OK, "retire for revoked descriptor");
    OmegaSharedWorldDesc revoked = make_pub(&e.w, &sem, fresh, (uint64_t)cid);
    revoked.object_generation = old;
    revoked.sequence = rx_world_publication_tail(&e.w);
    rx_world_seal_descriptor(&revoked);
    CHECK(inject_take(&e.w, &revoked, &fault) == RX_ERR_STALE_GEN, "revoked generation was accepted");
    OmegaSharedWorldDesc deadobj = revoked;
    deadobj.object_generation = e.w.objects[obj.id].generation;
    deadobj.sequence = rx_world_publication_tail(&e.w);
    rx_world_seal_descriptor(&deadobj);
    CHECK(inject_take(&e.w, &deadobj, &fault) == RX_ERR_BOUNDS, "revoked object was accepted");
    CHECK(e.w.objects[obj.id].field[0] == held && e.w.objects[obj.id].generation == old + 1,
          "revoked descriptor changed the object");

    RxObject tail;
    CHECK(rx_world_read(&e.w, (RxObjRef){ obj.id, old }, &tail) == RX_ERR_STALE_GEN,
          "host accepted the retired name after the hostile set");
    audit_and_close(&e);
}

/* ---- receipt -------------------------------------------------------------- */

static void write_receipt(int total_checks, int total_fail, const char *binary_digest) {
    char path[512];
    if (omega_evidence_path("R3/rx_heartbeat_receipt.json", path, sizeof(path)) != 0) {
        fprintf(stderr, "receipt: refused (path exists in record mode?)\n");
        return;
    }
    FILE *f = fopen(path, "w");
    if (!f) { perror("receipt"); return; }
    char commit[41] = "unknown";
    omega_evidence_run_commit(commit);
    const char *cand = getenv("OMEGA_CANDIDATE_COMMIT");
    struct utsname u;
    uname(&u);
    int tests_passed = 0;
    for (int i = 0; i < g_ntests; i++) if (g_tests[i].failures == 0) tests_passed++;
    bool all = total_fail == 0 && tests_passed == g_ntests;
    time_t now = time(NULL);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));

    fprintf(f, "{\n  \"schema\": \"AIEN_RX_HEARTBEAT_RECEIPT_V1\",\n");
    fprintf(f, "  \"adr\": \"aien-architecture ADR 0016\",\n");
    fprintf(f, "  \"run_id\": \"%s\",\n  \"timestamp_utc\": \"%s\",\n", omega_evidence_run_id(), ts);
    fprintf(f, "  \"candidate_commit\": %s%s%s,\n  \"candidate_bound\": %s,\n",
            cand ? "\"" : "", cand ? cand : "null", cand ? "\"" : "", cand ? "true" : "false");
    fprintf(f, "  \"run_commit\": \"%s\",\n  \"tree_dirty\": %s,\n", commit,
            omega_evidence_tree_dirty() ? "true" : "false");
    fprintf(f, "  \"test_binary_sha256\": \"%s\",\n", binary_digest);
    fprintf(f, "  \"host\": {\"sysname\": \"%s\", \"release\": \"%s\", \"machine\": \"%s\"},\n",
            u.sysname, u.release, u.machine);
    const char *physics = getenv("PHYSICS_COMMIT");
    fprintf(f, "  \"physics_commit\": %s%s%s,\n",
            physics ? "\"" : "", physics ? physics : "null", physics ? "\"" : "");
    fprintf(f, "  \"hardware_scope\": \"host/reference; no silicon claim\",\n");
    fprintf(f, "  \"gates\": {\n");
    int r5_ok = all;
    int r6_ok = all;
    for (int i = 0; i < g_ntests; i++) {
        if (strcmp(g_tests[i].name, "resource_admission_1000") == 0 ||
            strcmp(g_tests[i].name, "zero_resource_budget_is_zero") == 0 ||
            strcmp(g_tests[i].name, "background_not_starved") == 0 ||
            strcmp(g_tests[i].name, "priority_ladder") == 0)
            if (g_tests[i].failures) r5_ok = 0;
        if ((strcmp(g_tests[i].name, "stability_containment") == 0 ||
             strcmp(g_tests[i].name, "periodic_versus_oscillation") == 0) && g_tests[i].failures)
            r6_ok = 0;
    }
    fprintf(f, "    \"R3_REACTION_CORE\": \"%s\",\n", all ? "PASS (host reference)" : "FAIL");
    fprintf(f, "    \"R4_CAUSAL_TRACE\": \"%s\",\n", all ? "PASS (host reference; every reaction crumb in every test audited)" : "FAIL");
    fprintf(f, "    \"R5_RESOURCE_ARBITRATION\": \"%s\",\n",
            r5_ok ? "PASS (host reference; not silicon)" : "FAIL");
    fprintf(f, "    \"R6_REACTION_STABILITY\": \"%s\",\n",
            r6_ok ? "PASS (host reference; not silicon)" : "FAIL");
    int r1_ok = all, r2_ok = all;
    for (int i = 0; i < g_ntests; i++) {
        if ((strcmp(g_tests[i].name, "canonical_object_one_identity") == 0 ||
             strcmp(g_tests[i].name, "identity_space_is_closed") == 0 ||
             strcmp(g_tests[i].name, "semantic_commit_without_transport") == 0 ||
             strcmp(g_tests[i].name, "physical_realization") == 0) && g_tests[i].failures)
            r1_ok = 0;
        if (strcmp(g_tests[i].name, "cross_engine_descriptor_cpu") == 0 && g_tests[i].failures)
            r2_ok = 0;
    }
    fprintf(f, "    \"R1_CANONICAL_WORLD\": \"%s\",\n",
            r1_ok ? "PASS (host/reference; no silicon claim)" : "FAIL");
    fprintf(f, "    \"R1_CANONICAL_SHARED_WORLD_PASS\": \"%s\",\n",
            r1_ok ? "PASS (host/reference; no silicon claim)" : "FAIL");
    fprintf(f, "    \"R2_CROSS_ENGINE_ABI\": \"%s\",\n",
            r2_ok ? "PASS (CPU/reference; no GB10 claim; no resident GPU claim)" : "FAIL");
    fprintf(f, "    \"R2_CROSS_ENGINE_ABI_PASS\": \"%s\",\n",
            r2_ok ? "PASS (CPU/reference; no GB10 claim; no resident GPU claim)" : "FAIL");
    fprintf(f, "    \"authority_host_reference\": \"%s\",\n",
            all ? "hardened; R7 native root NOT claimed" : "FAIL");
    fprintf(f, "    \"not_claimed\": [\"R7 native\", \"R8\", \"R9\", \"R10\", \"R11\", \"R12\", \"R13+\"]\n  },\n");
    fprintf(f, "  \"tests\": [\n");
    for (int i = 0; i < g_ntests; i++)
        fprintf(f, "    {\"name\": \"%s\", \"invariants\": \"%s\", \"checks\": %d, \"failures\": %d}%s\n",
                g_tests[i].name, g_tests[i].invariants, g_tests[i].checks, g_tests[i].failures,
                i + 1 < g_ntests ? "," : "");
    fprintf(f, "  ],\n");
    fprintf(f, "  \"totals\": {\"tests\": %d, \"tests_passed\": %d, \"checks\": %d, \"failures\": %d},\n",
            g_ntests, tests_passed, total_checks, total_fail);
    fprintf(f, "  \"causal\": {\"crumbs_digest_verified\": %llu, \"reaction_crumbs_audited\": %llu},\n",
            (unsigned long long)g_crumbs_verified, (unsigned long long)g_crumbs_audited);
    fprintf(f, "  \"lifecycle\": {\"illegal_transitions\": %llu, \"observed\": [",
            (unsigned long long)g_illegal_transitions);
    bool first = true;
    for (int a = 0; a < RX_STATE_COUNT; a++)
        for (int b = 0; b < RX_STATE_COUNT; b++)
            if (g_transitions[a][b]) {
                fprintf(f, "%s\n    {\"from\": \"%s\", \"to\": \"%s\", \"count\": %llu}", first ? "" : ",",
                        rx_state_name((RxState)a), rx_state_name((RxState)b),
                        (unsigned long long)g_transitions[a][b]);
                first = false;
            }
    fprintf(f, "\n  ]},\n");
    fprintf(f, "  \"heartbeat_trace\": %s\n}\n", g_heartbeat_trace[0] ? g_heartbeat_trace : "null");
    fclose(f);
    printf("[*] receipt: %s\n", path);
}

static void binary_digest(const char *argv0, char out[65]) {
    strcpy(out, "unavailable");
    FILE *f = fopen("/proc/self/exe", "rb");
    if (!f) f = fopen(argv0, "rb");
    if (!f) return;
    sha256_ctx c;
    sha256_init(&c);
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) sha256_update(&c, buf, n);
    fclose(f);
    uint8_t d[32];
    sha256_final(&c, d);
    for (int i = 0; i < 32; i++) sprintf(&out[i * 2], "%02x", d[i]);
}

int main(int argc, char **argv) {
    (void)argc;
    signal(SIGPIPE, SIG_IGN);
    printf("AIEN resident reaction runtime -- heartbeat qualification (ADR 0016, R3/R4 host)\n");
    t_heartbeat();
    t_single_dependency_narrow();
    t_multiple_dependency();
    t_fan_out();
    t_fan_in();
    t_invalidation();
    t_read_only_invalidation();
    t_concurrent_publication();
    t_stale_input();
    t_write_set_enforced();
    t_duplicate_wake();
    t_capability_attacks();
    t_revoke_during_run();
    t_root_table_unwritable();
    t_crumb_tamper_detected();
    t_schedule_independence();
    t_authority_office();
    t_counter_fail_closed();
    t_root_death_restart();
    t_resource_admission();
    t_zero_resource_budget();
    t_background_not_starved();
    t_priority_ladder();
    t_stability();
    t_periodic_and_backoff();
    t_semantic_without_transport();
    t_one_identity();
    t_identity_closed();
    t_physical_realization();
    t_cross_engine();

    int checks = 0, fails = 0;
    for (int i = 0; i < g_ntests; i++) {
        checks += g_tests[i].checks;
        fails += g_tests[i].failures;
        printf("  %-45s %s (%d checks)\n", g_tests[i].name,
               g_tests[i].failures ? "FAIL" : "PASS", g_tests[i].checks);
    }
    char dg[65];
    binary_digest(argv[0], dg);
    printf("TOTAL: %d tests, %d checks, %d failures; %llu crumbs verified, %llu reaction crumbs audited, %llu illegal transitions\n",
           g_ntests, checks, fails, (unsigned long long)g_crumbs_verified,
           (unsigned long long)g_crumbs_audited, (unsigned long long)g_illegal_transitions);
    write_receipt(checks, fails, dg);
    printf("%s\n", fails == 0
                       ? "R1_CANONICAL_WORLD: PASS (host CPU, not silicon)  "
                         "R2_CROSS_ENGINE_ABI: PASS (host CPU, not silicon)  "
                         "R3_REACTION_CORE: PASS (host)  R4_CAUSAL_TRACE: PASS (host)  "
                         "R5_RESOURCE_ARBITRATION: PASS (host)  R6_REACTION_STABILITY: PASS (host)  "
                         "R7: NOT CLAIMED"
                       : "R1/R2/R3/R4/R5/R6: FAIL");
    return fails == 0 ? 0 : 1;
}
