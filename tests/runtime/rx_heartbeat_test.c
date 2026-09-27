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
 * Receipt: build/qual-runs/<run>/R3/rx_heartbeat_receipt.json (or
 * evidence/runs/<run>/... with OMEGA_QUAL_RECORD=1). Counts are observed, never
 * literal. The candidate commit is an input (OMEGA_CANDIDATE_COMMIT), not HEAD.
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

#define MAX_TESTS 32
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
    RxWorld w;
} Env;

static int env_start(Env *e, uint32_t workers) {
    if (rx_caproot_start(&e->root) != RX_CAP_OK) return -1;
    if (rx_world_init(&e->w, &e->root, workers, 1u << 18) != RX_OK) {
        rx_caproot_stop(&e->root);
        return -1;
    }
    e->w.external_subject = SUBJ_EXTERNAL;
    return 0;
}

static RxCapRef mint(Env *e, uint32_t subject, uint64_t resource, uint32_t rights) {
    RxCapMint m = { ISSUER_AEGIS_POLICY, subject, resource, rights, 0, { UINT32_MAX, 0 } };
    RxCapRef r = { UINT32_MAX, 0 };
    int rc = rx_caproot_mint(&e->root, &m, &r);
    if (rc != RX_CAP_OK) fprintf(stderr, "mint failed: %s\n", rx_cap_strerror(rc));
    return r;
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
    rx_caproot_stop(&e->root);
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
    rx_caproot_revoke(&e.root, rev);
    cases[nc++] = (typeof(cases[0])){ "revoked", rev, SUBJ_AIEN, RX_CAP_ERR_REVOKED };
    /* Generation replay: revoke, reclaim, the slot is re-minted to someone else. */
    RxCapRef old = mint(&e, SUBJ_AIEN, RES_BELIEF, RX_RIGHT_WRITE);
    rx_caproot_revoke(&e.root, old);
    rx_caproot_reclaim(&e.root, old.cap_id);
    RxCapRef reused = mint(&e, SUBJ_OTHER, RES_BELIEF, RX_RIGHT_WRITE);
    CHECK(reused.cap_id == old.cap_id && reused.generation == old.generation + 1,
          "reclaimed slot was not reused with a new generation");
    cases[nc++] = (typeof(cases[0])){ "generation_replay", old, SUBJ_AIEN, RX_CAP_ERR_STALE_GEN };
    RxCapMint lm = { ISSUER_AEGIS_POLICY, SUBJ_AIEN, RES_BELIEF, RX_RIGHT_WRITE, 5, { UINT32_MAX, 0 } };
    RxCapRef leased;
    rx_caproot_mint(&e.root, &lm, &leased);
    rx_caproot_advance_clock(&e.root, 10);
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
    RxCapMint amp = { SUBJ_AIEN, SUBJ_OMEGA, RES_BELIEF, RX_RIGHT_READ | RX_RIGHT_WRITE, 0, parent };
    CHECK(rx_caproot_mint(&e.root, &amp, NULL) == RX_CAP_ERR_AMPLIFY, "rights amplification accepted");
    RxCapMint other = { SUBJ_AIEN, SUBJ_OMEGA, RES_PLAN, RX_RIGHT_READ, 0, parent };
    CHECK(rx_caproot_mint(&e.root, &other, NULL) == RX_CAP_ERR_RESOURCE, "resource widening accepted");
    RxCapMint nd = { SUBJ_AIEN, SUBJ_OMEGA, RES_SENSOR, RX_RIGHT_READ, 0, rs };
    CHECK(rx_caproot_mint(&e.root, &nd, NULL) == RX_CAP_ERR_NOT_DELEGABLE, "non-delegable parent delegated");
    RxCapMint ok = { SUBJ_AIEN, SUBJ_OMEGA, RES_BELIEF, RX_RIGHT_READ, 0, parent };
    RxCapRef child;
    CHECK(rx_caproot_mint(&e.root, &ok, &child) == RX_CAP_OK, "valid attenuation refused");
    CHECK(rx_caproot_validate(&e.root, child, SUBJ_OMEGA, RES_BELIEF, RX_RIGHT_READ, NULL) == RX_CAP_OK,
          "delegated child invalid");
    rx_caproot_revoke(&e.root, parent);
    CHECK(rx_caproot_validate(&e.root, child, SUBJ_OMEGA, RES_BELIEF, RX_RIGHT_READ, NULL) == RX_CAP_ERR_CHAIN,
          "child survived parent revocation");

    /* External stimulus with a capability for the wrong subject. */
    CHECK(stimulus(&e, rs, sensor, F_TEMP, 77) == RX_ERR_AUTHORITY, "external write with wrong cap accepted");

    /* Epoch change invalidates everything minted before it. */
    rx_caproot_bump_epoch(&e.root);
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
    CHECK(rx_caproot_revoke(&e.root, w) == RX_CAP_OK, "revoke");
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
    CHECK(rx_caproot_start(&root) == RX_CAP_OK, "root");
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
    rx_caproot_stop(&root);
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
    fprintf(f, "  \"hardware_scope\": \"host CPU only; no accelerator involved\",\n");
    fprintf(f, "  \"gates\": {\n");
    fprintf(f, "    \"R3_REACTION_CORE\": \"%s\",\n", all ? "PASS (host reference)" : "FAIL");
    fprintf(f, "    \"R4_CAUSAL_TRACE\": \"%s\",\n", all ? "PASS (host reference; every reaction crumb in every test audited)" : "FAIL");
    fprintf(f, "    \"not_claimed\": [\"R1 canonical shared pool (unify with OMEGA_SHARED_WORLD_V1)\", \"R2\", \"R5\", \"R6\", \"R7 native capability root\", \"R8\", \"R9\", \"R10-R16\"]\n  },\n");
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
    t_concurrent_publication();
    t_stale_input();
    t_write_set_enforced();
    t_duplicate_wake();
    t_capability_attacks();
    t_revoke_during_run();
    t_root_table_unwritable();
    t_crumb_tamper_detected();
    t_schedule_independence();

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
    printf("%s\n", fails == 0 ? "R3_REACTION_CORE: PASS (host)  R4_CAUSAL_TRACE: PASS (host)"
                              : "R3/R4: FAIL");
    return fails == 0 ? 0 : 1;
}
