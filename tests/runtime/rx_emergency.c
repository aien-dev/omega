/*
 * R16 G6 operator emergency stop (spec/r16-operator-emergency-stop.md).
 *
 * Real authority (the rx_caproot root process), real caller credentials, a
 * worker pool and a generation store. Every case states what must hold:
 *
 *   E1 refusals   no credential, a forged credential, another subject's
 *                 capability, the wrong right, the wrong resource, a revoked
 *                 capability, and a reaction's own subject holding a valid
 *                 control capability: each refused with its code; the world
 *                 digest, the crumb count and the halt state are unchanged.
 *   E2 stop       an authorized stop takes effect at once (OPERATOR_STOP crumb
 *                 naming the capability and the reason); a second stop answers
 *                 RX_HALT_ALREADY and changes nothing; outside publication is
 *                 refused (RX_ERR_HALTED) and changes nothing.
 *   E3 in flight  a stop taken while a reaction computes: its result is not
 *                 published (CANCELLED, RX_ERR_HALTED), work queued behind it is
 *                 not taken; on resume both run and each commits exactly once.
 *   E4 race       publishers and two operators racing stop/resume: in the crumb
 *                 log no EXTERNAL or COMMIT lies between a STOP and its RESUME,
 *                 each accepted stop has exactly one STOP crumb, every refused
 *                 publication returned RX_ERR_HALTED, and the chain verifies.
 *   E5 promotion  a stop during a promotion's live barrier and during its disk
 *                 writes refuses that promotion (RX_GEN_ERR_HALTED, active
 *                 generation unchanged in memory and on disk); a proposal under
 *                 a stop is refused; after resume the same promoter promotes.
 *   E6 durable    the stop writes a sealed mark; a new world over the same
 *                 directory starts stopped (restored); a mark with one byte
 *                 changed still stops it (RX_ERR_TORN); an unbound store will not
 *                 promote while the mark exists; resume keeps the stop record in
 *                 a sealed OPERATOR_HALT.resumed.* file and removes the mark; a
 *                 resume that cannot retire the mark leaves the world stopped.
 *   E7 budget     a stop while R3 (woken by another reaction's commit) computes,
 *                 with an activation budget of 1: the refused activation does not
 *                 spend the budget, R3 is not quarantined and runs on resume;
 *                 object create and retire are refused under the stop and change
 *                 nothing; a symlink mark stops a new world (TORN) and resume
 *                 removes it.
 *
 * Host only, CPU only. Exit 0 = PASS.
 */
#include "runtime/rx_caproot.h"
#include "runtime/rx_generation.h"
#include "runtime/rx_world.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define U(x) ((unsigned long long)(x))

static int g_fail;
static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "[+]" : "[-]", what);
    if (!ok) g_fail++;
}

enum { RES_E = 0x710, RES_O = 0x711, RES_Q = 0x712 };
enum { SUBJ_EXT = 9001, SUBJ_R = 9002, SUBJ_R2 = 9003, SUBJ_R3 = 9004, OP = 9100, OP2 = 9101, INTRUDER = 9102 };

/* ---- gate: R parks in its function until released ------------------------- */
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int armed, armed3, inside, go;
} g = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0, 0 };
static int g_with_r3;   /* setup adds R3, a reaction woken by R's commit (E7) */

typedef struct {
    RxCapRoot root;
    RxCapAdmin admin;
    RxWorld w;
    RxObjRef e, o, q;
    uint32_t id_r, id_r2, id_r3;
    RxCapRef c_ext, c_op, c_op2;
    RxCallerCred cr_op, cr_op2, cr_intr, cr_r, cr_r2, cr_r3;
} Env;

static int fn_r(RxCtx *c) {
    Env *e = c->user;
    pthread_mutex_lock(&g.mu);
    if (g.armed) {
        g.inside = 1;
        pthread_cond_broadcast(&g.cv);
        while (!g.go) pthread_cond_wait(&g.cv, &g.mu);
        g.go = 0;
        g.inside = 0;
        g.armed = 0;
    }
    pthread_mutex_unlock(&g.mu);
    c->n_out = 1;
    c->out[0] = (RxMutation){ e->o, 0, c->in[0].field[0] + 1 };
    return 0;
}

static int fn_r2(RxCtx *c) {
    Env *e = c->user;
    c->n_out = 1;
    c->out[0] = (RxMutation){ e->q, 0, c->in[0].field[1] * 10 };
    return 0;
}

/* R3: woken by R's commit on O (an internal cause, so its episode budget is not
 * reset by the wake), held in its function when armed3 is set. */
static int fn_r3(RxCtx *c) {
    Env *e = c->user;
    pthread_mutex_lock(&g.mu);
    if (g.armed3) {
        g.inside = 1;
        pthread_cond_broadcast(&g.cv);
        while (!g.go) pthread_cond_wait(&g.cv, &g.mu);
        g.go = 0;
        g.inside = 0;
        g.armed3 = 0;
    }
    pthread_mutex_unlock(&g.mu);
    c->n_out = 1;
    c->out[0] = (RxMutation){ e->q, 1, c->in[0].field[0] * 100 };
    return 0;
}

static RxCapRef mint(Env *e, uint32_t subject, uint64_t resource, uint32_t rights) {
    RxCapMint m;
    memset(&m, 0, sizeof m);
    m.issuer = 1;
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

static int setup(Env *e, uint32_t workers, uint64_t crumb_cap) {
    memset(e, 0, sizeof *e);
    if (rx_caproot_start(&e->root, &e->admin) != RX_CAP_OK) return -1;
    if (rx_world_init(&e->w, &e->root, workers, crumb_cap) != RX_OK) return -1;
    e->w.external_subject = SUBJ_EXT;
    e->e = mkobj(e, RES_E);
    e->o = mkobj(e, RES_O);
    e->q = mkobj(e, RES_Q);
    e->c_ext = mint(e, SUBJ_EXT, RES_E, RX_RIGHT_WRITE);
    e->c_op = mint(e, OP, RX_WORLD_RES_CONTROL, RX_WORLD_RIGHT_HALT);
    e->c_op2 = mint(e, OP2, RX_WORLD_RES_CONTROL, RX_WORLD_RIGHT_HALT);
    if (rx_world_enroll_caller(&e->w, OP, &e->cr_op) != RX_CALLER_OK ||
        rx_world_enroll_caller(&e->w, OP2, &e->cr_op2) != RX_CALLER_OK ||
        rx_world_enroll_caller(&e->w, INTRUDER, &e->cr_intr) != RX_CALLER_OK ||
        rx_world_enroll_caller(&e->w, SUBJ_R, &e->cr_r) != RX_CALLER_OK ||
        rx_world_enroll_caller(&e->w, SUBJ_R2, &e->cr_r2) != RX_CALLER_OK)
        return -1;
    RxReactionDesc d;
    memset(&d, 0, sizeof d);
    d.name = "estop.r";
    d.faculty = RX_FACULTY_OMEGA;
    d.subject = SUBJ_R;
    d.caller = e->cr_r;
    d.priority = RX_PRIO_FOREGROUND;
    d.fn = fn_r;
    d.user = e;
    d.triggers[d.n_triggers++] = (RxDep){ e->e, RX_FIELD(0) };
    d.writes[d.n_writes++] = (RxDep){ e->o, RX_FIELD(0) };
    d.caps[d.n_caps++] = (RxCapNeed){ mint(e, SUBJ_R, RES_E, RX_RIGHT_READ), RES_E, RX_RIGHT_READ };
    d.caps[d.n_caps++] = (RxCapNeed){ mint(e, SUBJ_R, RES_O, RX_RIGHT_WRITE), RES_O, RX_RIGHT_WRITE };
    if (rx_world_add_reaction(&e->w, &d, &e->id_r) != RX_OK) return -1;
    memset(&d, 0, sizeof d);
    d.name = "estop.r2";
    d.faculty = RX_FACULTY_AIEN;
    d.subject = SUBJ_R2;
    d.caller = e->cr_r2;
    d.priority = RX_PRIO_FOREGROUND;
    d.fn = fn_r2;
    d.user = e;
    d.triggers[d.n_triggers++] = (RxDep){ e->e, RX_FIELD(1) };
    d.writes[d.n_writes++] = (RxDep){ e->q, RX_FIELD(0) };
    d.caps[d.n_caps++] = (RxCapNeed){ mint(e, SUBJ_R2, RES_E, RX_RIGHT_READ), RES_E, RX_RIGHT_READ };
    d.caps[d.n_caps++] = (RxCapNeed){ mint(e, SUBJ_R2, RES_Q, RX_RIGHT_WRITE), RES_Q, RX_RIGHT_WRITE };
    if (rx_world_add_reaction(&e->w, &d, &e->id_r2) != RX_OK) return -1;
    if (g_with_r3) {
        if (rx_world_enroll_caller(&e->w, SUBJ_R3, &e->cr_r3) != RX_CALLER_OK) return -1;
        memset(&d, 0, sizeof d);
        d.name = "estop.r3";
        d.faculty = RX_FACULTY_AIEN;
        d.subject = SUBJ_R3;
        d.caller = e->cr_r3;
        d.priority = RX_PRIO_FOREGROUND;
        d.fn = fn_r3;
        d.user = e;
        d.triggers[d.n_triggers++] = (RxDep){ e->o, RX_FIELD(0) };
        d.writes[d.n_writes++] = (RxDep){ e->q, RX_FIELD(1) };
        d.caps[d.n_caps++] = (RxCapNeed){ mint(e, SUBJ_R3, RES_O, RX_RIGHT_READ), RES_O, RX_RIGHT_READ };
        d.caps[d.n_caps++] = (RxCapNeed){ mint(e, SUBJ_R3, RES_Q, RX_RIGHT_WRITE), RES_Q, RX_RIGHT_WRITE };
        if (rx_world_add_reaction(&e->w, &d, &e->id_r3) != RX_OK) return -1;
    }
    if (rx_world_bind_callers(&e->w) != RX_OK) return -1;
    return 0;
}

static void teardown(Env *e) {
    rx_world_destroy(&e->w);
    rx_caproot_stop(&e->root, &e->admin);
}

static uint64_t field(Env *e, RxObjRef r) {
    RxObject o;
    if (rx_world_read(&e->w, r, &o) != RX_OK) return UINT64_MAX;
    return o.field[0];
}

static int64_t publish(Env *e, uint32_t f, uint64_t v) {
    RxMutation m = { e->e, f, v };
    return rx_world_publish_external(&e->w, e->c_ext, &m, 1);
}

static uint64_t crumbs(Env *e) {
    pthread_mutex_lock(&e->w.mu);
    uint64_t n = e->w.n_crumbs;
    pthread_mutex_unlock(&e->w.mu);
    return n;
}

static void digest(Env *e, uint8_t out[32]) { rx_world_digest(&e->w, out); }

static uint64_t reaction_commits(Env *e, uint32_t rid) {
    uint64_t n = 0;
    pthread_mutex_lock(&e->w.mu);
    for (uint64_t i = 0; i < e->w.n_crumbs; i++)
        if (e->w.crumbs[i].kind == RX_CRUMB_COMMIT && e->w.crumbs[i].reaction == rid) n++;
    pthread_mutex_unlock(&e->w.mu);
    return n;
}

static int wait_inside(void) {
    for (int i = 0; i < 5000; i++) {
        pthread_mutex_lock(&g.mu);
        int in = g.inside;
        pthread_mutex_unlock(&g.mu);
        if (in) return 0;
        usleep(1000);
    }
    return -1;
}

static void gate_release(void) {
    pthread_mutex_lock(&g.mu);
    g.go = 1;
    pthread_cond_broadcast(&g.cv);
    pthread_mutex_unlock(&g.mu);
}

/* Bounded wait until reaction rid has `want` commits (5 s). */
static int wait_commits(Env *e, uint32_t rid, uint64_t want) {
    for (int i = 0; i < 5000; i++) {
        if (reaction_commits(e, rid) >= want) return 0;
        usleep(1000);
    }
    return -1;
}

static int stop(Env *e, uint32_t subj, const RxCallerCred *cr, RxCapRef cap, int32_t why) {
    return rx_world_emergency_stop(&e->w, subj, cr, cap, why, NULL);
}
static int resume(Env *e, uint32_t subj, const RxCallerCred *cr, RxCapRef cap) {
    return rx_world_emergency_resume(&e->w, subj, cr, cap, NULL);
}
static bool halted(Env *e) {
    RxHaltStatus h;
    rx_world_halt_status(&e->w, &h);
    return h.halted;
}

/* ---- E1 refusals ----------------------------------------------------------- */
static void e1_refusals(Env *e) {
    uint8_t d0[32], d1[32];
    digest(e, d0);
    uint64_t n0 = crumbs(e);
    RxCallerCred forged = e->cr_op;
    forged.secret[0] ^= 1;
    RxCapRef wrong_right = mint(e, OP, RX_WORLD_RES_CONTROL, RX_RIGHT_WRITE);
    RxCapRef wrong_res = mint(e, OP, RES_O, RX_WORLD_RIGHT_HALT);
    RxCapRef of_r = mint(e, SUBJ_R, RX_WORLD_RES_CONTROL, RX_WORLD_RIGHT_HALT);
    RxCapRef revoked = mint(e, OP, RX_WORLD_RES_CONTROL, RX_WORLD_RIGHT_HALT);
    check(rx_capadmin_revoke(&e->admin, rx_capadmin_office(&e->admin), revoked) == RX_CAP_OK,
          "E1 setup: a control capability revoked by the office");
    struct { const char *what; uint32_t subj; const RxCallerCred *cr; RxCapRef cap; int want; } t[] = {
        { "no credential", OP, NULL, e->c_op, RX_ERR_IDENTITY },
        { "forged credential", OP, &forged, e->c_op, RX_ERR_IDENTITY },
        { "enrolled intruder presenting the operator's capability", INTRUDER, &e->cr_intr, e->c_op,
          RX_ERR_AUTHORITY },
        { "operator with the wrong right on the control resource", OP, &e->cr_op, wrong_right,
          RX_ERR_AUTHORITY },
        { "operator with the halt right on another resource", OP, &e->cr_op, wrong_res,
          RX_ERR_AUTHORITY },
        { "operator with a revoked control capability", OP, &e->cr_op, revoked, RX_ERR_AUTHORITY },
        { "a reaction's subject with a valid control capability", SUBJ_R, &e->cr_r, of_r,
          RX_ERR_AUTHORITY },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        char what[200];
        int rc = stop(e, t[i].subj, t[i].cr, t[i].cap, 7);
        snprintf(what, sizeof what, "E1 stop refused: %s (rc %d, want %d)", t[i].what, rc, t[i].want);
        check(rc == t[i].want, what);
    }
    check(!halted(e), "E1 the world is still running after every refused stop");
    digest(e, d1);
    check(memcmp(d0, d1, 32) == 0 && crumbs(e) == n0,
          "E1 refused stops changed neither the world digest nor the crumb log");
    check(resume(e, OP, &forged, e->c_op) == RX_ERR_IDENTITY,
          "E1 a forged resume is refused before 'not stopped' is told");
    check(resume(e, OP, &e->cr_op, e->c_op) == RX_HALT_NOT_STOPPED,
          "E1 an authorized resume of a running world answers RX_HALT_NOT_STOPPED");
    check(crumbs(e) == n0, "E1 the not-stopped resume left no crumb");
    check(publish(e, 1, 4) > 0 && wait_commits(e, e->id_r2, 1) == 0 && field(e, e->q) == 40,
          "E1 control: the world still publishes and reacts (R2 wrote 4*10)");
}

/* ---- E2 stop and E3 in flight ------------------------------------------------ */
static void e2_e3(Env *e) {
    /* E3 first half: R is computing when the stop lands; R2's wake is queued
     * behind it on the single worker. */
    pthread_mutex_lock(&g.mu);
    g.armed = 1;
    pthread_mutex_unlock(&g.mu);
    uint64_t r_before = reaction_commits(e, e->id_r), r2_before = reaction_commits(e, e->id_r2);
    check(publish(e, 0, 41) > 0, "E3 setup: publish E.f0 = 41 (wakes R)");
    check(wait_inside() == 0, "E3 setup: R is computing, held in its function");
    check(publish(e, 1, 5) > 0, "E3 setup: publish E.f1 = 5 (wakes R2, queued: one worker)");

    uint64_t n0 = crumbs(e);
    RxHaltStatus h;
    int rc = rx_world_emergency_stop(&e->w, OP, &e->cr_op, e->c_op, 77, &h);
    check(rc == RX_OK && h.halted && h.seq == 1 && h.subject == OP && h.reason == 77,
          "E2 an authorized stop takes effect (seq 1, operator, reason 77)");
    const RxCrumb *k = rx_world_crumb(&e->w, h.crumb);
    check(k && k->kind == RX_CRUMB_OPERATOR_STOP && k->caps[0].cap_id == e->c_op.cap_id &&
              k->caps[0].generation == e->c_op.generation && k->reason == 77 &&
              k->reaction == UINT32_MAX,
          "E2 OPERATOR_STOP crumb names the control capability and the reason");
    check(h.durable == 0, "E2 no halt directory: durable = 0 (said, not hidden)");
    uint64_t n1 = crumbs(e);
    check(n1 == n0 + 1, "E2 the stop added exactly one crumb");
    check(stop(e, OP2, &e->cr_op2, e->c_op2, 78) == RX_HALT_ALREADY && crumbs(e) == n1,
          "E2 a second stop (another operator) answers RX_HALT_ALREADY, no crumb");
    rx_world_halt_status(&e->w, &h);
    check(h.seq == 1 && h.subject == OP && h.reason == 77, "E2 the second stop changed nothing");
    uint8_t d0[32], d1[32];
    digest(e, d0);
    check(publish(e, 0, 999) == RX_ERR_HALTED, "E2 outside publication refused: RX_ERR_HALTED");
    digest(e, d1);
    check(memcmp(d0, d1, 32) == 0 && crumbs(e) == n1, "E2 the refused publication changed nothing");

    /* R finishes computing under the stop. */
    uint64_t o_before = field(e, e->o);
    gate_release();
    int cancelled = 0;
    for (int i = 0; i < 5000 && !cancelled; i++) {
        pthread_mutex_lock(&e->w.mu);
        for (uint64_t j = n1; j < e->w.n_crumbs; j++)
            if (e->w.crumbs[j].kind == RX_CRUMB_CANCELLED && e->w.crumbs[j].reaction == e->id_r &&
                e->w.crumbs[j].reason == RX_ERR_HALTED)
                cancelled = 1;
        pthread_mutex_unlock(&e->w.mu);
        if (!cancelled) usleep(1000);
    }
    check(cancelled, "E3 the activation computed under the stop ended CANCELLED, RX_ERR_HALTED");
    usleep(50000);
    check(field(e, e->o) == o_before && reaction_commits(e, e->id_r) == r_before,
          "E3 nothing it computed was published (O unchanged, no COMMIT)");
    check(reaction_commits(e, e->id_r2) == r2_before,
          "E3 R2's queued wake was not taken while stopped (50 ms)");
    rx_world_halt_status(&e->w, &h);
    check(h.refused == 2, "E3 refused count = 2 (one publication, one activation)");

    check(resume(e, OP2, &e->cr_op2, e->c_op2) == RX_OK,
          "E2 another authorized operator resumes");
    rx_world_halt_status(&e->w, &h);
    const RxCrumb *rk = rx_world_crumb(&e->w, e->w.n_crumbs);
    int found = 0;
    pthread_mutex_lock(&e->w.mu);
    for (uint64_t j = n1; j < e->w.n_crumbs; j++) {
        const RxCrumb *c = &e->w.crumbs[j];
        if (c->kind != RX_CRUMB_OPERATOR_RESUME) continue;
        for (uint32_t p = 0; p < c->n_parents; p++) found |= c->parents[p] == n1;
        found &= c->caps[0].cap_id == e->c_op2.cap_id;
    }
    pthread_mutex_unlock(&e->w.mu);
    (void)rk;
    check(!h.halted && found, "E2 OPERATOR_RESUME crumb: parent = the stop, cap = the resumer's");
    check(wait_commits(e, e->id_r, r_before + 1) == 0 && wait_commits(e, e->id_r2, r2_before + 1) == 0,
          "E3 on resume the cancelled activation and the queued wake both ran");
    usleep(50000);
    check(field(e, e->o) == 42 && field(e, e->q) == 50, "E3 their results: O = 41+1, Q = 5*10");
    check(reaction_commits(e, e->id_r) == r_before + 1 && reaction_commits(e, e->id_r2) == r2_before + 1,
          "E3 each committed exactly once");
}

/* ---- E4 race ----------------------------------------------------------------- */
typedef struct {
    Env *e;
    atomic_int run;
    atomic_long ok, halted_rc, other_rc;
    atomic_long stops_ok, resumes_ok, stop_other, resume_other;
} Race;

static void *publisher(void *a) {
    Race *r = a;
    uint64_t v = 100;
    while (atomic_load(&r->run) && atomic_load(&r->ok) < 60000) {
        int64_t rc = publish(r->e, (uint32_t)(v & 1), v);
        if (rc > 0) atomic_fetch_add(&r->ok, 1);
        else if (rc == RX_ERR_HALTED) atomic_fetch_add(&r->halted_rc, 1);
        else atomic_fetch_add(&r->other_rc, 1);
        v++;
    }
    return NULL;
}

typedef struct { Race *r; uint32_t subj; RxCallerCred *cr; RxCapRef cap; int n; } Op;
static void *operator_thread(void *a) {
    Op *o = a;
    for (int i = 0; i < o->n; i++) {
        int rc = stop(o->r->e, o->subj, o->cr, o->cap, 100 + i);
        if (rc == RX_OK) atomic_fetch_add(&o->r->stops_ok, 1);
        else if (rc != RX_HALT_ALREADY) atomic_fetch_add(&o->r->stop_other, 1);
        usleep(200);
        rc = resume(o->r->e, o->subj, o->cr, o->cap);
        if (rc == RX_OK) atomic_fetch_add(&o->r->resumes_ok, 1);
        else if (rc != RX_HALT_NOT_STOPPED) atomic_fetch_add(&o->r->resume_other, 1);
        usleep(100);
    }
    return NULL;
}

static void e4_race(Env *e) {
    uint64_t n0 = crumbs(e);
    Race r;
    memset(&r, 0, sizeof r);
    r.e = e;
    atomic_store(&r.run, 1);
    pthread_t p[2], o[2];
    Op o1 = { &r, OP, &e->cr_op, e->c_op, 300 }, o2 = { &r, OP2, &e->cr_op2, e->c_op2, 300 };
    for (int i = 0; i < 2; i++) pthread_create(&p[i], NULL, publisher, &r);
    pthread_create(&o[0], NULL, operator_thread, &o1);
    pthread_create(&o[1], NULL, operator_thread, &o2);
    pthread_join(o[0], NULL);
    pthread_join(o[1], NULL);
    atomic_store(&r.run, 0);
    for (int i = 0; i < 2; i++) pthread_join(p[i], NULL);
    if (halted(e)) resume(e, OP, &e->cr_op, e->c_op);
    rx_world_wait_quiescent(&e->w, 5000);

    long stops = 0, resumes = 0, inside_bad = 0, stop_in_stop = 0, resume_outside = 0;
    int in = 0;
    pthread_mutex_lock(&e->w.mu);
    for (uint64_t j = n0; j < e->w.n_crumbs; j++) {
        const RxCrumb *c = &e->w.crumbs[j];
        if (c->kind == RX_CRUMB_OPERATOR_STOP) { stops++; stop_in_stop += in; in = 1; }
        else if (c->kind == RX_CRUMB_OPERATOR_RESUME) { resumes++; resume_outside += !in; in = 0; }
        else if (in && (c->kind == RX_CRUMB_EXTERNAL || c->kind == RX_CRUMB_COMMIT)) inside_bad++;
    }
    uint64_t overflow = e->w.stats.crumb_overflow;
    pthread_mutex_unlock(&e->w.mu);
    char what[240];
    snprintf(what, sizeof what,
             "E4 race: %ld publications ok, %ld refused HALTED, %ld other; %ld stops, %ld resumes",
             (long)atomic_load(&r.ok), (long)atomic_load(&r.halted_rc), (long)atomic_load(&r.other_rc),
             stops, resumes);
    check(atomic_load(&r.ok) > 0 && atomic_load(&r.halted_rc) > 0 && atomic_load(&r.other_rc) == 0,
          what);
    check(overflow == 0, "E4 the crumb log did not overflow (every event is in it)");
    check(inside_bad == 0, "E4 no EXTERNAL or COMMIT crumb between a STOP and its RESUME");
    check(stop_in_stop == 0 && resume_outside == 0, "E4 STOP and RESUME strictly alternate");
    check(stops == atomic_load(&r.stops_ok) && resumes == atomic_load(&r.resumes_ok) + (stops > atomic_load(&r.resumes_ok) ? 1 : 0),
          "E4 one STOP crumb per accepted stop, one RESUME per accepted resume");
    check(atomic_load(&r.stop_other) == 0 && atomic_load(&r.resume_other) == 0,
          "E4 every stop/resume answer was OK, ALREADY or NOT_STOPPED");
    uint64_t checked = 0;
    check(rx_world_verify_crumbs(&e->w, &checked) == 0, "E4 the crumb chain verifies");
}

/* ---- E5 promotion -------------------------------------------------------------- */
enum { PROP = 9200, PRO = 9201 };
static int auth_any(void *c, uint32_t id, uint64_t gen, uint32_t s, uint64_t res, uint32_t rights) {
    (void)c; (void)id; (void)gen; (void)s; (void)res; (void)rights;
    return 0;
}

typedef struct { Env *e; int rc, fired; } StopNow;
static void stop_live(void *ctx) {
    StopNow *s = ctx;
    if (s->fired++) return;
    s->rc = stop(s->e, OP, &s->e->cr_op, s->e->c_op, 501);
}
static void stop_disk(const char *dir, void *ctx) { (void)dir; stop_live(ctx); }

static void e5_promotion(void) {
    Env e;
    check(setup(&e, 1, 1u << 16) == 0, "E5 setup: second world");
    /* A fresh world: the store's proposer and promoter are its callers. The
     * world above is bound already; this one enrolls them before binding. */
    teardown(&e);
    memset(&e, 0, sizeof e);
    RxCallerCred prop, pro;
    char dir[64];
    RxGenStore *gs = NULL;
    int ok = rx_caproot_start(&e.root, &e.admin) == RX_CAP_OK &&
             rx_world_init(&e.w, &e.root, 1, 1u << 12) == RX_OK;
    if (ok) {
        e.c_op = mint(&e, OP, RX_WORLD_RES_CONTROL, RX_WORLD_RIGHT_HALT);
        ok = rx_world_enroll_caller(&e.w, OP, &e.cr_op) == RX_CALLER_OK &&
             rx_world_enroll_caller(&e.w, PROP, &prop) == RX_CALLER_OK &&
             rx_world_enroll_caller(&e.w, PRO, &pro) == RX_CALLER_OK &&
             rx_world_bind_callers(&e.w) == RX_OK &&
             mkdtemp(strcpy(dir, "/tmp/rx-estop-gen-XXXXXX")) &&
             rx_gen_open(dir, &gs) == RX_GEN_OK &&
             rx_gen_bind_authority(gs, rx_world_caller_check_fn, &e.w, auth_any, NULL) == RX_GEN_OK;
    }
    check(ok, "E5 setup: world with operator, proposer, promoter; bound generation store");
    if (!ok) return;
    static const uint8_t ev[] = "estop";
    RxGenObject obj = { 1, 1, { 0 } };
    RxGenDraft gd;
    memset(&gd, 0, sizeof gd);
    gd.proofs_ok = 1;
    gd.n_objects = 1;
    gd.objects = &obj;
    gd.evidence = ev;
    gd.evidence_len = sizeof ev - 1;
    for (int v = 0; v < 2; v++) {
        const char *where = v == 0 ? "live barrier" : "disk writes";
        uint64_t cand = 0, a0 = 0, l0 = 0, a1 = 0, l1 = 0;
        int prc = rx_gen_propose_as(gs, PROP, &prop, &gd, &cand);
        rx_gen_active(gs, &a0, &l0);
        StopNow s = { &e, -99, 0 };
        RxPromotionRequest req = { cand, PRO, 1, 1, RX_GEN_RES_PROMOTION, RX_GEN_RIGHT_PROMOTE, pro };
        if (v == 1) rx_gen_set_disk_hook(gs, stop_disk, &s);
        int rc = prc != RX_GEN_OK ? prc
               : rx_gen_promote(gs, &req, NULL, NULL, NULL, NULL, v == 0 ? stop_live : NULL,
                                v == 0 ? &s : NULL);
        if (v == 1) rx_gen_set_disk_hook(gs, NULL, NULL);
        rx_gen_active(gs, &a1, &l1);
        RxRecoveryRecord d;
        memset(&d, 0, sizeof d);
        rx_gen_recover(dir, &d);
        char what[200];
        snprintf(what, sizeof what, "E5 stop during the %s: stop rc %d, promote rc %d (want %d)",
                 where, s.rc, rc, RX_GEN_ERR_HALTED);
        check(prc == RX_GEN_OK && s.rc == RX_OK && rc == RX_GEN_ERR_HALTED, what);
        check(a1 == a0 && d.active_id == a0, "E5 active generation unchanged, in memory and on disk");
        uint64_t c2 = 0;
        check(rx_gen_propose_as(gs, PROP, &prop, &gd, &c2) == RX_GEN_ERR_HALTED,
              "E5 a proposal under the stop is refused: RX_GEN_ERR_HALTED");
        check(resume(&e, OP, &e.cr_op, e.c_op) == RX_OK, "E5 operator resumes");
    }
    uint64_t cand = 0, a1 = 0, l1 = 0;
    int prc = rx_gen_propose_as(gs, PROP, &prop, &gd, &cand);
    RxPromotionRequest req = { cand, PRO, 1, 1, RX_GEN_RES_PROMOTION, RX_GEN_RIGHT_PROMOTE, pro };
    int rc = prc == RX_GEN_OK ? rx_gen_promote(gs, &req, NULL, NULL, NULL, NULL, NULL, NULL) : prc;
    rx_gen_active(gs, &a1, &l1);
    check(rc == RX_GEN_OK && a1 == cand, "E5 control: after resume the same promoter promotes");
    rx_gen_close(gs);
    teardown(&e);
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) fprintf(stderr, "cleanup of %s failed\n", dir);
}

/* ---- E6 durable ------------------------------------------------------------------ */
static int read_file(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    return (int)n;
}

static int count_resumed(const char *dir, char *name, size_t cap) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)))
        if (strncmp(de->d_name, RX_GEN_HALT_MARK ".resumed.", strlen(RX_GEN_HALT_MARK) + 9) == 0) {
            n++;
            snprintf(name, cap, "%s", de->d_name);
        }
    closedir(d);
    return n;
}

static void e6_durable(void) {
    char dir[64], mark[160], raw[600];
    check(mkdtemp(strcpy(dir, "/tmp/rx-estop-mark-XXXXXX")) != NULL, "E6 setup: halt directory");
    snprintf(mark, sizeof mark, "%s/%s", dir, RX_GEN_HALT_MARK);
    Env e;
    check(setup(&e, 1, 1u << 16) == 0, "E6 setup: world");
    check(rx_world_set_halt_dir(&e.w, dir) == 0, "E6 no mark: set_halt_dir answers 0, world runs");
    RxHaltStatus h;
    check(rx_world_emergency_stop(&e.w, OP, &e.cr_op, e.c_op, 606, &h) == RX_OK && h.durable == 1,
          "E6 the stop wrote its mark durably (durable = 1)");
    int n = read_file(mark, raw, sizeof raw);
    char want_head[200];
    snprintf(want_head, sizeof want_head, "aien-operator-halt v1\nseq 1\nsubject %u\ncap %u %llu\nreason 606\n",
             OP, e.c_op.cap_id, U(e.c_op.generation));
    check(n > 0 && strncmp(raw, want_head, strlen(want_head)) == 0 && strstr(raw, "\nsha256 "),
          "E6 the mark names the operator, the capability and the reason, and is sealed");
    teardown(&e);   /* the process "restarts": a new world over the same directory */

    check(setup(&e, 1, 1u << 16) == 0, "E6 restart: new world");
    int rr = rx_world_set_halt_dir(&e.w, dir);
    rx_world_halt_status(&e.w, &h);
    check(rr == 1 && h.halted && h.restored && h.seq == 1 && h.subject == OP && h.reason == 606,
          "E6 the new world starts stopped, restored from the mark (seq, operator, reason)");
    check(publish(&e, 0, 1) == RX_ERR_HALTED, "E6 the restored stop refuses publication");
    teardown(&e);

    /* A changed byte: still stopped, reported torn. */
    char torn[600];
    memcpy(torn, raw, (size_t)n + 1);
    torn[30] = torn[30] == '1' ? '2' : '1';
    char tpath[200];
    snprintf(tpath, sizeof tpath, "%s/torn", dir);
    check(mkdir(tpath, 0700) == 0, "E6 setup: directory with a damaged mark");
    char tmark[240];
    snprintf(tmark, sizeof tmark, "%s/%s", tpath, RX_GEN_HALT_MARK);
    FILE *f = fopen(tmark, "w");
    if (f) { fwrite(torn, 1, (size_t)n, f); fclose(f); }
    check(setup(&e, 1, 1u << 16) == 0, "E6 torn: new world");
    rr = rx_world_set_halt_dir(&e.w, tpath);
    rx_world_halt_status(&e.w, &h);
    check(rr == RX_ERR_TORN && h.halted && h.reason == RX_ERR_TORN,
          "E6 a mark with one byte changed still stops the world (RX_ERR_TORN)");
    check(resume(&e, OP, &e.cr_op, e.c_op) == RX_OK && access(tmark, F_OK) != 0,
          "E6 an authorized resume lifts the torn stop and retires its mark");
    char rname[260], rpath[520], rbuf[1400];
    check(count_resumed(tpath, rname, sizeof rname) == 1, "E6 the torn record is kept, not deleted");
    snprintf(rpath, sizeof rpath, "%s/%s", tpath, rname);
    check(read_file(rpath, rbuf, sizeof rbuf) > n && memcmp(rbuf, torn, (size_t)n) == 0,
          "E6 the kept record starts with the damaged bytes, unchanged");
    teardown(&e);

    /* An unbound store over the directory will not promote while the mark exists. */
    RxGenStore *gs = NULL;
    check(rx_gen_open(dir, &gs) == RX_GEN_OK, "E6 unbound store opened over the halt directory");
    static const uint8_t ev[] = "estop";
    RxGenObject obj = { 1, 1, { 0 } };
    RxGenDraft gd;
    memset(&gd, 0, sizeof gd);
    gd.proofs_ok = 1;
    gd.n_objects = 1;
    gd.objects = &obj;
    gd.evidence = ev;
    gd.evidence_len = sizeof ev - 1;
    uint64_t cand = 0, a0 = 0, l0 = 0, a1 = 0, l1 = 0;
    int prc = rx_gen_propose(gs, PROP, &gd, &cand);
    rx_gen_active(gs, &a0, &l0);
    RxPromotionRequest req = { cand, PRO, 1, 1, RX_GEN_RES_PROMOTION, RX_GEN_RIGHT_PROMOTE, { 0, { 0 } } };
    int rc = prc == RX_GEN_OK ? rx_gen_promote(gs, &req, auth_any, NULL, NULL, NULL, NULL, NULL) : prc;
    rx_gen_active(gs, &a1, &l1);
    check(rc == RX_GEN_ERR_HALTED && a1 == a0, "E6 unbound store: promotion refused by the mark");

    /* Resume through a world: record kept, mark gone, the store promotes. */
    check(setup(&e, 1, 1u << 16) == 0 && rx_world_set_halt_dir(&e.w, dir) == 1, "E6 world restored stopped");
    if (geteuid() == 0) {
        printf("[~] E6 read-only directory case skipped: running as root ignores the permission\n");
    } else {
        check(chmod(dir, 0500) == 0, "E6 setup: halt directory made read-only");
        check(resume(&e, OP, &e.cr_op, e.c_op) == RX_ERR_IO && halted(&e) && access(mark, F_OK) == 0,
              "E6 a resume that cannot retire the mark is refused: world stays stopped, mark stays");
        check(chmod(dir, 0700) == 0, "E6 setup: halt directory writable again");
    }
    check(resume(&e, OP, &e.cr_op, e.c_op) == RX_OK && !halted(&e) && access(mark, F_OK) != 0,
          "E6 resume: world runs, mark removed");
    check(count_resumed(dir, rname, sizeof rname) == 1, "E6 exactly one kept stop record");
    snprintf(rpath, sizeof rpath, "%s/%s", dir, rname);
    int rn = read_file(rpath, rbuf, sizeof rbuf);
    char who[120];
    snprintf(who, sizeof who, "resumed_by %u\nresume_cap %u %llu\n", OP, e.c_op.cap_id,
             U(e.c_op.generation));
    check(rn > n && memcmp(rbuf, raw, (size_t)n) == 0 && strstr(rbuf + n, who) &&
              strstr(rbuf + n, "\nsha256 "),
          "E6 kept record = the stop's bytes + who resumed, sealed");
    rc = rx_gen_promote(gs, &req, auth_any, NULL, NULL, NULL, NULL, NULL);
    rx_gen_active(gs, &a1, &l1);
    check(rc == RX_GEN_OK && a1 == cand, "E6 control: with the mark gone the store promotes");
    rx_gen_close(gs);
    teardown(&e);
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) fprintf(stderr, "cleanup of %s failed\n", dir);
}

/* ---- E7 budget, create/retire, a mark that is not a file -------------------- */
static void e7_budget_objects(void) {
    Env e;
    g_with_r3 = 1;
    int su = setup(&e, 1, 1u << 16);
    g_with_r3 = 0;
    check(su == 0, "E7 setup: world with R3 (woken by R's commit)");
    if (su != 0) return;
    RxStabilityBudget sb;
    memset(&sb, 0, sizeof sb);
    sb.activation_budget = 1;   /* one activation per reaction per episode */
    rx_world_set_stability(&e.w, &sb);
    pthread_mutex_lock(&g.mu);
    g.armed3 = 1;
    pthread_mutex_unlock(&g.mu);
    check(publish(&e, 0, 6) > 0, "E7 setup: publish E.f0 = 6 (R runs, its commit wakes R3)");
    check(wait_inside() == 0, "E7 setup: R3 is computing, held in its function");
    check(stop(&e, OP, &e.cr_op, e.c_op, 707) == RX_OK, "E7 operator stop while R3 computes");
    gate_release();
    usleep(50 * 1000);
    pthread_mutex_lock(&e.w.mu);
    bool quarantined = e.w.reactions[e.id_r3].quarantined;
    pthread_mutex_unlock(&e.w.mu);
    check(!quarantined, "E7 the refused activation did not spend R3's episode budget (not quarantined)");

    /* Create and retire are commits too: refused under the stop, nothing changes. */
    uint8_t d0[32], d1[32];
    digest(&e, d0);
    uint64_t n0 = crumbs(&e);
    uint64_t init[RX_MAX_FIELDS] = { 0 };
    RxObjRef nr = { UINT32_MAX, 0 };
    check(rx_world_create(&e.w, 1, RX_PERSIST_RESIDENT, 0x7ff, init, &nr) == RX_ERR_HALTED &&
              nr.id == UINT32_MAX,
          "E7 object create under the stop: RX_ERR_HALTED, no object");
    check(rx_world_retire(&e.w, e.q) == RX_ERR_HALTED, "E7 object retire under the stop: RX_ERR_HALTED");
    digest(&e, d1);
    check(memcmp(d0, d1, 32) == 0 && crumbs(&e) == n0 && field(&e, e.q) != UINT64_MAX,
          "E7 refused create/retire changed nothing (digest, crumbs, Q still live)");

    check(resume(&e, OP, &e.cr_op, e.c_op) == RX_OK, "E7 operator resume");
    check(wait_commits(&e, e.id_r3, 1) == 0, "E7 R3 ran again after resume and committed");
    RxObject q;
    check(rx_world_read(&e.w, e.q, &q) == RX_OK && q.field[1] == 700, "E7 its result: Q.f1 = (6+1)*100");
    teardown(&e);

    /* A mark that is a symlink (here dangling) is not a mark the world can read:
     * the world starts stopped (TORN, never "absent, go"), and resume keeps a
     * record and removes the link, so the store is not blocked for good. */
    char dir[64], mark[160], rname[260];
    check(mkdtemp(strcpy(dir, "/tmp/rx-estop-link-XXXXXX")) != NULL, "E7 setup: halt directory");
    snprintf(mark, sizeof mark, "%s/%s", dir, RX_GEN_HALT_MARK);
    check(symlink("/nonexistent/rx-estop-target", mark) == 0, "E7 setup: OPERATOR_HALT is a dangling symlink");
    check(setup(&e, 1, 1u << 16) == 0, "E7 link: new world");
    int rr = rx_world_set_halt_dir(&e.w, dir);
    check(rr == RX_ERR_TORN && halted(&e), "E7 a symlink mark stops the world (RX_ERR_TORN)");
    check(resume(&e, OP, &e.cr_op, e.c_op) == RX_OK && !halted(&e), "E7 resume lifts it");
    struct stat st;
    check(lstat(mark, &st) != 0 && count_resumed(dir, rname, sizeof rname) == 1,
          "E7 the link is removed and a resumed record kept");
    teardown(&e);
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) fprintf(stderr, "cleanup of %s failed\n", dir);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    Env e;
    if (setup(&e, 1, 1u << 16) != 0) {
        printf("[-] setup failed\nRX_EMERGENCY_STOP: FAIL\n");
        return 1;
    }
    e1_refusals(&e);
    e2_e3(&e);
    teardown(&e);
    if (setup(&e, 4, 1u << 21) != 0) {
        printf("[-] setup (4 workers) failed\nRX_EMERGENCY_STOP: FAIL\n");
        return 1;
    }
    e4_race(&e);
    teardown(&e);
    e5_promotion();
    e6_durable();
    e7_budget_objects();
    printf("RX_EMERGENCY_STOP: %s (%d failures; host CPU, real caproot authority)\n",
           g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
