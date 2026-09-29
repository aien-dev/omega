/*
 * R11: AIEN as a continuous cognitive faculty (ADR 0016 §44).
 *
 * Part A drives AIEN alone. The test plays the rest of the world: it
 * publishes demand intervals, selection records, placements and goals from
 * outside, and reads what AIEN publishes back. Every branch of the
 * cognition is exercised here: no record, a steady record, one outlier, a
 * change of core class, drift, a goal met and unmet, memory that refuses a
 * second experiment, and authority.
 *
 * Part B is the living run on the DGX Spark with the real Omega faculty.
 * The whole process starts on the Cortex-A725 cores, where Omega's own
 * search keeps the semantic reference. The process is then moved to the
 * Cortex-X925 cores. Nothing asks anyone to re-optimize. AIEN's prediction
 * fails, AIEN explains the failure by the change of core class, publishes a
 * plan, and Omega takes it up by itself. A control world without AIEN makes
 * the same move and stays on the old record.
 *
 * Authority is the native AIENOS capability authority.
 */
#include "runtime/aienos_cap.h"
#include "runtime/rx_aien.h"
#include "runtime/rx_omega.h"
#include "runtime/rx_world.h"
#include "omega_evidence.h"
#include "sha256.h"

#include <dirent.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

enum { SUBJ_EXTERNAL = 100, ISSUER = 3 };

#define REG_M 64u
#define REG_N 256u
#define CLASS_X925 0xd85u
#define CLASS_A725 0xd87u
#define P1 0x111u
#define P2 0x222u

static int g_checks;
static int g_fail;

#define CHECK(cond, ...) do {                                            \
        g_checks++;                                                      \
        if (!(cond)) {                                                   \
            g_fail++;                                                    \
            fprintf(stderr, "  FAIL %s:%d ", __FILE__, __LINE__);        \
            fprintf(stderr, __VA_ARGS__);                                \
            fputc('\n', stderr);                                         \
        }                                                                \
    } while (0)

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_us(int us) {
    struct timespec ts = { 0, (long)us * 1000L };
    nanosleep(&ts, NULL);
}

/* ---- shared environment ---- */

typedef struct {
    AienosCapAdmin *admin;
    AienosCapView *view;
    RxWorld w;
    RxAienFaculty a;
    RxAienCaps acaps;
    int with_aien;
    /* Part A: stand-ins for Omega's demand and selection. */
    RxObjRef demand, selection;
    RxCapRef ext_demand, ext_selection;
    uint64_t calls, spent, windows;
    /* Part B: the real Omega faculty. */
    int with_omega;
    RxOmegaFaculty f;
    RxOmegaCaps ocaps;
    RxCapRef ext_request;
    uint64_t seq, served, wrong;
    /* Not correctness failures: the request never got a result. Under load
     * a publish can be refused (the crumb log is full) or the answer can miss
     * the 5 s deadline. Counted apart so they are not reported as wrong. */
    uint64_t timed_out, refused;
    RxCapRef ext_placement, ext_goal;
} Env;

static RxCapRef mint(Env *e, uint32_t subject, uint64_t resource, uint32_t rights) {
    AienosCapRef office;
    aienos_cap_office(e->admin, &office);
    AienosCapMint m = { ISSUER, subject, resource, rights, 0, { UINT32_MAX, 0 }, office };
    AienosCapRef r = { UINT32_MAX, 0 };
    if (aienos_cap_mint(e->admin, &m, &r) != 0) r = (AienosCapRef){ UINT32_MAX, 0 };
    return (RxCapRef){ r.cap_id, r.generation };
}

static int revoke_cap(Env *e, RxCapRef cap) {
    AienosCapRef office;
    aienos_cap_office(e->admin, &office);
    return aienos_cap_revoke(e->admin, office, (AienosCapRef){ cap.cap_id, cap.generation });
}

static uint64_t fld(Env *e, RxObjRef r, uint32_t i) {
    RxObject o;
    if (rx_world_read(&e->w, r, &o) != RX_OK) return UINT64_MAX;
    return o.field[i];
}

static int settle(Env *e) { return rx_world_wait_quiescent(&e->w, 20000); }

static int publish(Env *e, RxCapRef cap, RxObjRef o, const uint32_t *fields,
                   const uint64_t *vals, uint32_t n) {
    RxMutation m[RX_MAX_FIELDS];
    for (uint32_t i = 0; i < n; i++) m[i] = (RxMutation){ o, fields[i], vals[i] };
    int64_t id = rx_world_publish_external(&e->w, cap, m, n);
    return id > 0 ? 0 : -1;
}

static void place(Env *e, uint64_t cls) {
    uint64_t s = fld(e, e->a.o.placement, 0) + 1;
    uint32_t f[2] = { 0, 1 };
    uint64_t v[2] = { s, cls };
    CHECK(publish(e, e->ext_placement, e->a.o.placement, f, v, 2) == 0, "placement refused");
}

static void set_goal(Env *e, uint64_t regime, uint64_t target_ns) {
    uint64_t s = fld(e, e->a.o.goal, 0) + 1;
    uint32_t f[3] = { 0, 1, 2 };
    uint64_t v[3] = { s, regime, target_ns };
    CHECK(publish(e, e->ext_goal, e->a.o.goal, f, v, 3) == 0, "goal refused");
}

static int mint_aien(Env *e) {
    const uint32_t RW = RX_RIGHT_READ | RX_RIGHT_WRITE;
    for (uint32_t i = 0; i < RX_AIEN_RES_COUNT; i++)
        e->acaps.own[i] = mint(e, RX_AIEN_SUBJ, RX_AIEN_RES_BASE + i,
                               i == RX_AIEN_RES_PLACEMENT || i == RX_AIEN_RES_GOAL ? RX_RIGHT_READ : RW);
    /* AIEN observes demand and selection. It cannot write them. */
    e->acaps.demand = mint(e, RX_AIEN_SUBJ, e->w.objects[e->a.in.demand.id].resource, RX_RIGHT_READ);
    e->acaps.selection = mint(e, RX_AIEN_SUBJ, e->w.objects[e->a.in.selection.id].resource,
                              RX_RIGHT_READ);
    e->ext_placement = mint(e, SUBJ_EXTERNAL, RX_AIEN_RES_BASE + RX_AIEN_RES_PLACEMENT, RX_RIGHT_WRITE);
    e->ext_goal = mint(e, SUBJ_EXTERNAL, RX_AIEN_RES_BASE + RX_AIEN_RES_GOAL, RX_RIGHT_WRITE);
    return rx_aien_register(&e->a, &e->acaps);
}

static int world_start(Env *e) {
    if (aienos_cap_start(&e->admin, &e->view) != 0) return -1;
    if (rx_world_init_native(&e->w, e->view, 4, 1u << 20) != RX_OK) {
        aienos_cap_stop(e->admin, e->view);
        return -1;
    }
    e->w.external_subject = SUBJ_EXTERNAL;
    return 0;
}

static void env_stop(Env *e) {
    rx_world_wait_quiescent(&e->w, 20000);
    rx_world_destroy(&e->w);
    if (e->with_omega) rx_omega_destroy(&e->f);
    aienos_cap_stop(e->admin, e->view);
}

/* ---- Part A: AIEN against a scripted world ---- */

#define RES_DEMAND    0x7711000ull
#define RES_SELECTION 0x7711001ull

static int envA_start(Env *e) {
    memset(e, 0, sizeof(*e));
    if (world_start(e) != 0) return -1;
    uint64_t z[RX_MAX_FIELDS] = { 0 };
    if (rx_world_create(&e->w, RX_OT_DEMAND, RX_PERSIST_RESIDENT, RES_DEMAND, z, &e->demand) != RX_OK ||
        rx_world_create(&e->w, RX_OT_SELECTION, RX_PERSIST_RESIDENT, RES_SELECTION, z,
                        &e->selection) != RX_OK)
        return -1;
    e->ext_demand = mint(e, SUBJ_EXTERNAL, RES_DEMAND, RX_RIGHT_WRITE);
    e->ext_selection = mint(e, SUBJ_EXTERNAL, RES_SELECTION, RX_RIGHT_WRITE);
    RxAienConfig cfg;
    rx_aien_default_config(&cfg);
    RxAienInputs in = { e->demand, e->selection };
    if (rx_aien_create_objects(&e->a, &e->w, &cfg, &in) != RX_OK) return -1;
    e->with_aien = 1;
    return mint_aien(e) == RX_OK ? 0 : -1;
}

/* Omega recorded a realization for the regime. */
static void record(Env *e, uint64_t epoch, uint64_t real) {
    uint32_t f[3] = { 0, 1, 6 };
    uint64_t v[3] = { epoch, real, rx_aien_regime(REG_M, REG_N) };
    CHECK(publish(e, e->ext_selection, e->selection, f, v, 3) == 0, "selection refused");
    CHECK(settle(e) == RX_OK, "world did not settle");
}

/* One demand window of 16 production calls at `ns` each, served by `real`. */
static void window(Env *e, uint64_t ns, uint64_t real) {
    e->calls += 16;
    e->spent += 16 * ns;
    e->windows++;
    uint32_t f[6] = { 0, 1, 2, 4, 5, 6 };
    uint64_t v[6] = { e->calls, e->spent, e->windows, REG_M, REG_N, real };
    CHECK(publish(e, e->ext_demand, e->demand, f, v, 6) == 0, "demand refused");
    CHECK(settle(e) == RX_OK, "world did not settle");
}

static void windows(Env *e, uint32_t n, uint64_t ns, uint64_t real) {
    for (uint32_t i = 0; i < n; i++) window(e, ns, real);
}

static uint64_t commits_of(Env *e, uint32_t rid) { return e->w.reactions[rid].commits; }

/* Does the causal past of `from` contain a crumb with this id? */
static int caused_by(Env *e, uint64_t from, uint64_t ancestor) {
    uint64_t n = e->w.n_crumbs;
    uint8_t *seen = calloc(n + 1, 1);
    uint64_t *stack = malloc((n + 1) * sizeof(uint64_t));
    uint64_t sp = 0;
    int found = 0;
    stack[sp++] = from;
    while (sp && !found) {
        uint64_t id = stack[--sp];
        if (id == 0 || id > n || seen[id]) continue;
        seen[id] = 1;
        if (id == ancestor) found = 1;
        const RxCrumb *c = rx_world_crumb(&e->w, id);
        if (!c) continue;
        for (uint32_t i = 0; i < c->n_parents && sp < n; i++) stack[sp++] = c->parents[i];
        if (c->wake_cause && sp < n) stack[sp++] = c->wake_cause;
    }
    free(seen);
    free(stack);
    return found;
}

/* Every AIEN activation was woken by a world change, never by a caller. */
static void check_only_reactive(Env *e) {
    uint64_t aien = 0, uncaused = 0;
    for (uint64_t id = 1; id <= e->w.n_crumbs; id++) {
        const RxCrumb *c = rx_world_crumb(&e->w, id);
        if (!c || c->reaction == UINT32_MAX || c->faculty != RX_FACULTY_AIEN) continue;
        aien++;
        if (c->wake_cause == 0) uncaused++;
    }
    CHECK(aien > 0 && uncaused == 0, "AIEN crumbs %llu, without a waking cause %llu",
          (unsigned long long)aien, (unsigned long long)uncaused);
}

static void t_no_record(void) {
    printf("[*] no record yet: AIEN watches and publishes nothing\n");
    Env e;
    CHECK(envA_start(&e) == 0, "setup");
    place(&e, P1);
    windows(&e, 10, 5000, 0);
    CHECK(fld(&e, e.a.o.belief, 0) == 0, "belief formed without a record");
    CHECK(fld(&e, e.a.o.prediction, 0) == 0 && fld(&e, e.a.o.plan, 0) == 0, "acted without a record");
    CHECK(e.w.reactions[e.a.r_observe].activations == 10, "observe woke %llu times for 10 windows",
          (unsigned long long)e.w.reactions[e.a.r_observe].activations);
    CHECK(e.w.reactions[e.a.r_predict].activations == 0, "predict woke with nothing to predict");
    env_stop(&e);
}

/* Warm-up, baseline, then enough hits to confirm. */
static void confirm_at(Env *e, uint64_t ns, uint64_t real) {
    windows(e, 1 + e->a.cfg.baseline_intervals + e->a.cfg.confirm_intervals, ns, real);
}

static void t_steady_and_outlier(void) {
    printf("[*] a steady record is predicted and confirmed; one outlier is not a failure\n");
    Env e;
    CHECK(envA_start(&e) == 0, "setup");
    place(&e, P1);
    window(&e, 5000, 0);          /* production runs before any record exists */
    record(&e, 1, 0xAA);
    windows(&e, 1 + e.a.cfg.baseline_intervals - 1, 5000, 0xAA);
    CHECK(fld(&e, e.a.o.prediction, 0) == 0, "predicted before the baseline was complete");
    window(&e, 5000, 0xAA);
    CHECK(fld(&e, e.a.o.prediction, 0) == 1 && fld(&e, e.a.o.prediction, 6) == RX_AIEN_PRED_HOLDING,
          "prediction after baseline: seq %llu state %llu",
          (unsigned long long)fld(&e, e.a.o.prediction, 0), (unsigned long long)fld(&e, e.a.o.prediction, 6));
    CHECK(fld(&e, e.a.o.prediction, 5) == 5000 && fld(&e, e.a.o.prediction, 4) == P1 &&
          fld(&e, e.a.o.prediction, 2) == rx_aien_regime(REG_M, REG_N), "prediction contents");
    windows(&e, e.a.cfg.confirm_intervals, 5100, 0xAA);
    CHECK(fld(&e, e.a.o.prediction, 6) == RX_AIEN_PRED_CONFIRMED, "not confirmed after %u hits",
          e.a.cfg.confirm_intervals);

    /* One interval far off, then back to normal. */
    window(&e, 9000, 0xAA);
    CHECK(fld(&e, e.a.o.belief, 7) == 1, "outlier not counted as a miss");
    CHECK(fld(&e, e.a.o.prediction, 6) == RX_AIEN_PRED_CONFIRMED, "one outlier failed the prediction");
    CHECK(fld(&e, e.a.o.prediction, 3) == 0, "hit streak survived a miss");
    windows(&e, 3, 5000, 0xAA);
    CHECK(fld(&e, e.a.o.belief, 7) == 0 && fld(&e, e.a.o.prediction, 3) == 3, "recovery after outlier");
    CHECK(fld(&e, e.a.o.hypothesis, 0) == 0 && fld(&e, e.a.o.plan, 0) == 0,
          "AIEN acted on a confirmed prediction");

    /* Intervals served before production switched to the record are not evidence. */
    uint64_t seen = fld(&e, e.a.o.belief, 5);
    record(&e, 2, 0xBB);
    windows(&e, 3, 99999, 0xAA);
    CHECK(fld(&e, e.a.o.belief, 0) == 2 && fld(&e, e.a.o.belief, 5) == 0,
          "intervals on the old realization counted for the new record (seen %llu, was %llu)",
          (unsigned long long)fld(&e, e.a.o.belief, 5), (unsigned long long)seen);
    check_only_reactive(&e);
    env_stop(&e);
}

static void t_core_class(void) {
    printf("[*] a failed prediction after a core-class change becomes a hypothesis and a plan\n");
    Env e;
    CHECK(envA_start(&e) == 0, "setup");
    RxAienFaculty *a = &e.a;
    uint64_t R = rx_aien_regime(REG_M, REG_N);
    place(&e, P1);
    window(&e, 5000, 0);          /* production runs before any record exists */
    record(&e, 1, 0xAA);
    confirm_at(&e, 5000, 0xAA);
    CHECK(fld(&e, a->o.prediction, 6) == RX_AIEN_PRED_CONFIRMED, "setup prediction");

    /* The environment moves the work. AIEN notes nothing yet: moving is not evidence. */
    place(&e, P2);
    CHECK(settle(&e) == RX_OK, "settle");
    uint64_t place_crumb = rx_world_explain(&e.w, a->o.placement, 1);
    CHECK(fld(&e, a->o.hypothesis, 0) == 0, "a placement alone produced a hypothesis");
    window(&e, 2000, 0xAA);
    CHECK(fld(&e, a->o.prediction, 6) == RX_AIEN_PRED_CONFIRMED, "one miss failed the prediction");
    window(&e, 2100, 0xAA);
    CHECK(fld(&e, a->o.prediction, 6) == RX_AIEN_PRED_FAILED && fld(&e, a->o.prediction, 7) == 2100,
          "prediction state %llu observed %llu", (unsigned long long)fld(&e, a->o.prediction, 6),
          (unsigned long long)fld(&e, a->o.prediction, 7));
    CHECK(fld(&e, a->o.hypothesis, 0) == 1 && fld(&e, a->o.hypothesis, 1) == RX_AIEN_HYP_CORE_CLASS,
          "hypothesis kind %llu", (unsigned long long)fld(&e, a->o.hypothesis, 1));
    CHECK(fld(&e, a->o.hypothesis, 3) == P1 && fld(&e, a->o.hypothesis, 4) == P2 &&
          fld(&e, a->o.hypothesis, 5) == 5000 && fld(&e, a->o.hypothesis, 6) == 2100,
          "hypothesis contents");
    CHECK(fld(&e, a->o.hypothesis, 7) == RX_AIEN_HYP_TESTING, "hypothesis state %llu",
          (unsigned long long)fld(&e, a->o.hypothesis, 7));
    CHECK(fld(&e, a->o.plan, 0) == 1 && fld(&e, a->o.plan, 1) == RX_AIEN_ACT_RESEARCH &&
          fld(&e, a->o.plan, 2) == R && fld(&e, a->o.plan, 3) == P2 && fld(&e, a->o.plan, 4) == 1 &&
          fld(&e, a->o.plan, 5) == RX_AIEN_WHY_CORE_CLASS, "plan contents");
    CHECK(fld(&e, a->o.memory, 0) == 1 && fld(&e, a->o.memory, 1) == rx_aien_key(P2, R), "memory");

    /* The plan's causal past holds both the evidence and the observation that explains it. */
    uint64_t plan_crumb = rx_world_explain(&e.w, a->o.plan, 0);
    uint64_t demand_crumb = rx_world_explain(&e.w, e.demand, 1);
    CHECK(caused_by(&e, plan_crumb, place_crumb), "plan does not trace to the placement");
    CHECK(caused_by(&e, plan_crumb, demand_crumb), "plan does not trace to the failing evidence");

    /* More failing windows do not repeat the plan. */
    windows(&e, 4, 2000, 0xAA);
    CHECK(fld(&e, a->o.plan, 0) == 1 && commits_of(&e, a->r_plan) >= 1, "plan repeated");

    /* Some faculty answers with a new record measured where the work runs now. */
    record(&e, 2, 0xBB);
    confirm_at(&e, 2000, 0xBB);
    CHECK(fld(&e, a->o.prediction, 0) == 2 && fld(&e, a->o.prediction, 6) == RX_AIEN_PRED_CONFIRMED &&
          fld(&e, a->o.prediction, 4) == P2, "prediction under the new condition");
    CHECK(fld(&e, a->o.hypothesis, 7) == RX_AIEN_HYP_SUPPORTED, "hypothesis state %llu",
          (unsigned long long)fld(&e, a->o.hypothesis, 7));

    /* Back to P1: that condition was never explored by AIEN, so one plan for it. */
    place(&e, P1);
    windows(&e, 2, 5000, 0xBB);
    CHECK(fld(&e, a->o.hypothesis, 0) == 2 && fld(&e, a->o.hypothesis, 4) == P1 &&
          fld(&e, a->o.hypothesis, 7) == RX_AIEN_HYP_TESTING, "second hypothesis");
    CHECK(fld(&e, a->o.plan, 0) == 2 && fld(&e, a->o.plan, 3) == P1, "second plan");
    record(&e, 3, 0xAA);
    confirm_at(&e, 5000, 0xAA);
    CHECK(fld(&e, a->o.hypothesis, 7) == RX_AIEN_HYP_SUPPORTED, "second hypothesis not supported");

    /* P2 again: explored already. AIEN says so and plans nothing. */
    place(&e, P2);
    windows(&e, 2, 2000, 0xAA);
    CHECK(fld(&e, a->o.hypothesis, 0) == 3 && fld(&e, a->o.hypothesis, 7) == RX_AIEN_HYP_EXHAUSTED,
          "third hypothesis seq %llu state %llu", (unsigned long long)fld(&e, a->o.hypothesis, 0),
          (unsigned long long)fld(&e, a->o.hypothesis, 7));
    CHECK(fld(&e, a->o.plan, 0) == 2, "planned an explored condition");
    check_only_reactive(&e);
    env_stop(&e);
}

static void t_drift(void) {
    printf("[*] a failure with nothing else changed is drift; a second failure refutes it\n");
    Env e;
    CHECK(envA_start(&e) == 0, "setup");
    RxAienFaculty *a = &e.a;
    place(&e, P1);
    window(&e, 5000, 0);          /* production runs before any record exists */
    record(&e, 1, 0xAA);
    confirm_at(&e, 5000, 0xAA);
    windows(&e, 2, 9000, 0xAA);
    CHECK(fld(&e, a->o.hypothesis, 1) == RX_AIEN_HYP_DRIFT && fld(&e, a->o.hypothesis, 3) == P1 &&
          fld(&e, a->o.hypothesis, 4) == P1, "drift hypothesis");
    CHECK(fld(&e, a->o.plan, 0) == 1 && fld(&e, a->o.plan, 5) == RX_AIEN_WHY_DRIFT, "drift plan");
    record(&e, 2, 0xBB);
    windows(&e, 1 + a->cfg.baseline_intervals, 9000, 0xBB);
    windows(&e, 2, 15000, 0xBB);
    CHECK(fld(&e, a->o.prediction, 0) == 2 && fld(&e, a->o.prediction, 6) == RX_AIEN_PRED_FAILED,
          "second prediction");
    CHECK(fld(&e, a->o.hypothesis, 7) == RX_AIEN_HYP_UNSUPPORTED, "hypothesis state %llu",
          (unsigned long long)fld(&e, a->o.hypothesis, 7));
    CHECK(fld(&e, a->o.plan, 0) == 1, "planned again");
    env_stop(&e);
}

/* Seen on GB10 in R15 (2026-09-28): production jitter on A725 failed the
 * prediction twice with nothing changed; the drift research found nothing and
 * the failure was settled. The prediction then stayed FAILED, so the later
 * move to X925 was never noticed and no plan followed. A settled failure must
 * give way to a fresh prediction learned from fresh intervals. */
static void t_settled_failure_relearns(void) {
    printf("[*] a settled failure is relearned, so a later core move is still noticed\n");
    Env e;
    CHECK(envA_start(&e) == 0, "setup");
    RxAienFaculty *a = &e.a;
    uint64_t R = rx_aien_regime(REG_M, REG_N);
    place(&e, P1);
    window(&e, 5000, 0);          /* production runs before any record exists */
    record(&e, 1, 0xAA);
    confirm_at(&e, 5000, 0xAA);
    windows(&e, 2, 9000, 0xAA);   /* jitter: drift hypothesis, drift plan */
    CHECK(fld(&e, a->o.plan, 0) == 1 && fld(&e, a->o.plan, 5) == RX_AIEN_WHY_DRIFT, "drift plan");
    record(&e, 2, 0xAA);          /* the research kept the same realization */
    windows(&e, 1 + a->cfg.baseline_intervals, 9000, 0xAA);
    windows(&e, 2, 15000, 0xAA);  /* fails again: the drift hypothesis is refuted */
    CHECK(settle(&e) == RX_OK, "settle");
    uint64_t failed_seq = fld(&e, a->o.prediction, 0);
    CHECK(fld(&e, a->o.prediction, 6) == RX_AIEN_PRED_FAILED, "second failure");
    CHECK(fld(&e, a->o.hypothesis, 7) == RX_AIEN_HYP_UNSUPPORTED ||
          fld(&e, a->o.hypothesis, 7) == RX_AIEN_HYP_EXHAUSTED, "failure not settled: hypothesis state %llu",
          (unsigned long long)fld(&e, a->o.hypothesis, 7));

    /* Nothing is left to try here: AIEN relearns the record where it runs. */
    windows(&e, 2 + a->cfg.baseline_intervals + a->cfg.confirm_intervals, 15000, 0xAA);
    CHECK(fld(&e, a->o.prediction, 0) == failed_seq + 1 &&
          fld(&e, a->o.prediction, 6) == RX_AIEN_PRED_CONFIRMED &&
          fld(&e, a->o.prediction, 5) == 15000 && fld(&e, a->o.prediction, 4) == P1,
          "no fresh prediction after a settled failure: seq %llu (failed %llu) state %llu cost %llu",
          (unsigned long long)fld(&e, a->o.prediction, 0), (unsigned long long)failed_seq,
          (unsigned long long)fld(&e, a->o.prediction, 6), (unsigned long long)fld(&e, a->o.prediction, 5));
    CHECK(fld(&e, a->o.plan, 0) == 1, "relearning planned");

    /* The real change: the work moves. It is noticed, explained, planned. */
    uint64_t hyps = fld(&e, a->o.hypothesis, 0);
    place(&e, P2);
    windows(&e, 2, 4000, 0xAA);
    CHECK(fld(&e, a->o.prediction, 6) == RX_AIEN_PRED_FAILED, "the move was not noticed");
    CHECK(fld(&e, a->o.hypothesis, 0) == hyps + 1 &&
          fld(&e, a->o.hypothesis, 1) == RX_AIEN_HYP_CORE_CLASS &&
          fld(&e, a->o.hypothesis, 3) == P1 && fld(&e, a->o.hypothesis, 4) == P2 &&
          fld(&e, a->o.hypothesis, 7) == RX_AIEN_HYP_TESTING,
          "core-class hypothesis: seq %llu kind %llu state %llu", (unsigned long long)fld(&e, a->o.hypothesis, 0),
          (unsigned long long)fld(&e, a->o.hypothesis, 1), (unsigned long long)fld(&e, a->o.hypothesis, 7));
    CHECK(fld(&e, a->o.plan, 0) == 2 && fld(&e, a->o.plan, 2) == R && fld(&e, a->o.plan, 3) == P2 &&
          fld(&e, a->o.plan, 5) == RX_AIEN_WHY_CORE_CLASS, "no plan for the move");
    check_only_reactive(&e);
    env_stop(&e);
}

static void t_goal(void) {
    printf("[*] a human goal: unknown, met, unmet -> one plan, then unmet and explored\n");
    Env e;
    CHECK(envA_start(&e) == 0, "setup");
    RxAienFaculty *a = &e.a;
    uint64_t R = rx_aien_regime(REG_M, REG_N);
    place(&e, P1);
    set_goal(&e, R, 6000);
    CHECK(settle(&e) == RX_OK, "settle");
    CHECK(fld(&e, a->o.assessment, 4) == RX_AIEN_GOAL_UNKNOWN, "goal without evidence: %llu",
          (unsigned long long)fld(&e, a->o.assessment, 4));
    window(&e, 5000, 0);          /* production runs before any record exists */
    record(&e, 1, 0xAA);
    confirm_at(&e, 5000, 0xAA);
    CHECK(fld(&e, a->o.assessment, 4) == RX_AIEN_GOAL_MET && fld(&e, a->o.assessment, 3) == 5000,
          "goal met: status %llu", (unsigned long long)fld(&e, a->o.assessment, 4));
    CHECK(fld(&e, a->o.plan, 0) == 0, "planned for a met goal");

    set_goal(&e, R, 3000);
    CHECK(settle(&e) == RX_OK, "settle");
    CHECK(fld(&e, a->o.plan, 0) == 1 && fld(&e, a->o.plan, 5) == RX_AIEN_WHY_GOAL &&
          fld(&e, a->o.plan, 6) == 2 && fld(&e, a->o.plan, 3) == P1, "goal plan");
    record(&e, 2, 0xBB);
    confirm_at(&e, 4000, 0xBB);
    CHECK(fld(&e, a->o.assessment, 4) == RX_AIEN_GOAL_UNMET_EXPLORED && fld(&e, a->o.assessment, 3) == 4000,
          "after the experiment: status %llu", (unsigned long long)fld(&e, a->o.assessment, 4));
    set_goal(&e, R, 2500);
    CHECK(settle(&e) == RX_OK, "settle");
    CHECK(fld(&e, a->o.plan, 0) == 1, "planned an explored condition for a new goal");
    set_goal(&e, R, 4500);
    CHECK(settle(&e) == RX_OK, "settle");
    CHECK(fld(&e, a->o.assessment, 4) == RX_AIEN_GOAL_MET, "relaxed goal not met");
    env_stop(&e);
}

static int fn_forge_selection(RxCtx *c) {
    Env *e = c->user;
    c->out[c->n_out++] = (RxMutation){ e->selection, 1, 0xDEAD };
    return 0;
}

static void t_authority(void) {
    printf("[*] AIEN cannot write what it observes; revoked, AIEN stops and the world does not\n");
    Env e;
    CHECK(envA_start(&e) == 0, "setup");
    /* An AIEN reaction that tries to publish into Omega's record with AIEN's read-only reference. */
    RxReactionDesc d;
    memset(&d, 0, sizeof d);
    d.name = "aien.forge";
    d.faculty = RX_FACULTY_AIEN;
    d.subject = RX_AIEN_SUBJ;
    d.priority = RX_PRIO_LEARNING;
    d.fn = fn_forge_selection;
    d.user = &e;
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){ e.demand, RX_FIELD(2) };
    d.n_writes = 1;
    d.writes[0] = (RxDep){ e.selection, RX_ALL_FIELDS };
    d.n_caps = 2;
    d.caps[0] = (RxCapNeed){ e.acaps.demand, RES_DEMAND, RX_RIGHT_READ };
    d.caps[1] = (RxCapNeed){ e.acaps.selection, RES_SELECTION, RX_RIGHT_READ | RX_RIGHT_WRITE };
    uint32_t rid = UINT32_MAX;
    CHECK(rx_world_add_reaction(&e.w, &d, &rid) == RX_OK, "register");
    place(&e, P1);
    window(&e, 5000, 0);          /* production runs before any record exists */
    record(&e, 1, 0xAA);
    windows(&e, 3, 5000, 0xAA);
    CHECK(fld(&e, e.selection, 1) == 0xAA, "AIEN wrote Omega's selection");
    CHECK(e.w.reactions[rid].commits == 0, "forging reaction committed");
    CHECK(e.w.stats.blocked_authority > 0, "no authority block recorded");

    /* Revoke AIEN's belief reference. Observation stops; demand keeps flowing. */
    uint64_t seen = fld(&e, e.a.o.belief, 5);
    CHECK(revoke_cap(&e, e.acaps.own[RX_AIEN_RES_BELIEF]) == 0, "revoke");
    uint64_t before = e.windows;
    windows(&e, 4, 5000, 0xAA);
    CHECK(e.windows == before + 4 && fld(&e, e.demand, 2) == e.windows, "world stopped with AIEN");
    CHECK(fld(&e, e.a.o.belief, 5) == seen, "belief advanced without authority");
    CHECK(e.w.reactions[e.a.r_observe].state == RX_BLOCKED_AUTHORITY ||
          e.w.reactions[e.a.r_observe].state == RX_DORMANT, "observe state %s",
          rx_state_name(e.w.reactions[e.a.r_observe].state));
    env_stop(&e);
}

/* ---- Part B: the living run on the DGX Spark ---- */

static cpu_set_t g_x925, g_a725;
static int g_nx, g_na;
static char g_x_cpus[128], g_a_cpus[128];

static void find_core_classes(void) {
    CPU_ZERO(&g_x925);
    CPU_ZERO(&g_a725);
    long n = sysconf(_SC_NPROCESSORS_CONF);
    size_t ux = 0, ua = 0;
    for (long c = 0; c < n && c < CPU_SETSIZE; c++) {
        char path[128];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%ld/regs/identification/midr_el1", c);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        unsigned long long midr = 0;
        int ok = fscanf(fp, "%llx", &midr) == 1;
        fclose(fp);
        unsigned part = (unsigned)((midr >> 4) & 0xfffu);
        if (!ok) continue;
        if (part == CLASS_X925 && ux < sizeof g_x_cpus - 8) {
            CPU_SET((int)c, &g_x925);
            g_nx++;
            ux += (size_t)snprintf(g_x_cpus + ux, sizeof g_x_cpus - ux, "%s%ld", ux ? "," : "", c);
        } else if (part == CLASS_A725 && ua < sizeof g_a_cpus - 8) {
            CPU_SET((int)c, &g_a725);
            g_na++;
            ua += (size_t)snprintf(g_a_cpus + ua, sizeof g_a_cpus - ua, "%s%ld", ua ? "," : "", c);
        }
    }
}

/* Move every thread of this process, the world's workers included. */
static int move_all_threads(const cpu_set_t *set) {
    DIR *d = opendir("/proc/self/task");
    if (!d) return -1;
    struct dirent *de;
    int moved = 0, bad = 0;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        pid_t tid = (pid_t)atoi(de->d_name);
        if (sched_setaffinity(tid, sizeof *set, set) == 0) moved++;
        else bad++;
    }
    closedir(d);
    return bad ? -1 : moved;
}

static int envB_start(Env *e, int with_aien) {
    memset(e, 0, sizeof(*e));
    if (world_start(e) != 0) return -1;
    RxOmegaConfig oc;
    rx_omega_default_config(&oc);
    oc.hot_calls = 64;
    oc.hot_ns = 200000;
    /* A realization must be clearly better to replace the reference. */
    oc.margin_pct = 10;
    if (rx_omega_create_objects(&e->f, &e->w, &oc) != RX_OK) return -1;
    e->with_omega = 1;
    const uint32_t RW = RX_RIGHT_READ | RX_RIGHT_WRITE;
    for (uint32_t i = 0; i < RX_OMEGA_RES_COUNT; i++) {
        e->ocaps.serve[i] = (RxCapRef){ UINT32_MAX, 0 };
        e->ocaps.omega[i] = (RxCapRef){ UINT32_MAX, 0 };
    }
    uint64_t B = RX_OMEGA_RES_BASE;
    e->ocaps.serve[RX_OMEGA_RES_REQUEST] = mint(e, RX_OMEGA_SUBJ_SERVE, B + RX_OMEGA_RES_REQUEST, RX_RIGHT_READ);
    e->ocaps.serve[RX_OMEGA_RES_SELECTION] = mint(e, RX_OMEGA_SUBJ_SERVE, B + RX_OMEGA_RES_SELECTION, RX_RIGHT_READ);
    e->ocaps.serve[RX_OMEGA_RES_DEMAND] = mint(e, RX_OMEGA_SUBJ_SERVE, B + RX_OMEGA_RES_DEMAND, RW);
    e->ocaps.serve[RX_OMEGA_RES_RESULT] = mint(e, RX_OMEGA_SUBJ_SERVE, B + RX_OMEGA_RES_RESULT, RW);
    e->ocaps.omega[RX_OMEGA_RES_DEMAND] = mint(e, RX_OMEGA_SUBJ_OMEGA, B + RX_OMEGA_RES_DEMAND, RX_RIGHT_READ);
    e->ocaps.omega[RX_OMEGA_RES_SEARCH] = mint(e, RX_OMEGA_SUBJ_OMEGA, B + RX_OMEGA_RES_SEARCH, RW);
    e->ocaps.omega[RX_OMEGA_RES_SELECTION] = mint(e, RX_OMEGA_SUBJ_OMEGA, B + RX_OMEGA_RES_SELECTION, RW);
    for (uint32_t k = 0; k < e->f.cfg.n_slots; k++) {
        uint32_t c = RX_OMEGA_RES_CANDIDATE0 + k, v = RX_OMEGA_RES_VERDICT0 + k,
                 m = RX_OMEGA_RES_MEASURE0 + k;
        e->ocaps.omega[c] = mint(e, RX_OMEGA_SUBJ_OMEGA, B + c, RW);
        e->ocaps.omega[v] = mint(e, RX_OMEGA_SUBJ_OMEGA, B + v, RW);
        e->ocaps.omega[m] = mint(e, RX_OMEGA_SUBJ_OMEGA, B + m, RW);
    }
    e->ext_request = mint(e, SUBJ_EXTERNAL, B + RX_OMEGA_RES_REQUEST, RX_RIGHT_WRITE);
    if (rx_omega_register(&e->f, &e->ocaps) != RX_OK) return -1;

    RxAienConfig ac;
    rx_aien_default_config(&ac);
    RxAienInputs in = { e->f.o.demand, e->f.o.selection };
    if (rx_aien_create_objects(&e->a, &e->w, &ac, &in) != RX_OK) return -1;
    e->ext_placement = mint(e, SUBJ_EXTERNAL, RX_AIEN_RES_BASE + RX_AIEN_RES_PLACEMENT, RX_RIGHT_WRITE);
    e->ext_goal = mint(e, SUBJ_EXTERNAL, RX_AIEN_RES_BASE + RX_AIEN_RES_GOAL, RX_RIGHT_WRITE);
    if (!with_aien) return 0;
    e->with_aien = 1;
    if (mint_aien(e) != RX_OK) return -1;
    /* Omega may read AIEN's plan. It gets no other right on AIEN's objects. */
    RxCapRef plan_read = mint(e, RX_OMEGA_SUBJ_OMEGA, RX_AIEN_RES_BASE + RX_AIEN_RES_PLAN, RX_RIGHT_READ);
    return rx_omega_register_reconsider(&e->f, e->a.o.plan, plan_read,
                                        e->ocaps.omega[RX_OMEGA_RES_SEARCH],
                                        e->ocaps.omega[RX_OMEGA_RES_SELECTION]) == RX_OK ? 0 : -1;
}


/* A timed phase measures this host. When an R15 timed measurement is
 * running (its quiet flag exists or an R15 program is alive) or the machine
 * is already loaded, the numbers would describe the other work, so the
 * living run is not started. Same test as rx_plan_reuse.c. */
static int other_load(char *why, size_t n) {
    const char *home = getenv("HOME");
    char flag[512];
    snprintf(flag, sizeof flag, "%s/workspace/.spark-quiet", home ? home : "");
    if (access(flag, F_OK) == 0) {
        snprintf(why, n, "an R15 timed measurement holds ~/workspace/.spark-quiet");
        return 1;
    }
    DIR *d = opendir("/proc");
    if (d) {
        struct dirent *de;
        int busy = 0;
        while (!busy && (de = readdir(d)) != NULL) {
            if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
            char p[300], comm[64] = { 0 };
            snprintf(p, sizeof p, "/proc/%s/comm", de->d_name);
            FILE *f = fopen(p, "r");
            if (!f) continue;
            if (fgets(comm, sizeof comm, f))
                busy = strncmp(comm, "rx_r15", 6) == 0 || strncmp(comm, "r15_", 4) == 0;
            fclose(f);
        }
        closedir(d);
        if (busy) {
            snprintf(why, n, "an R15 program is running");
            return 1;
        }
    }
    double load = 0;
    FILE *f = fopen("/proc/loadavg", "r");
    if (f) {
        if (fscanf(f, "%lf", &load) != 1) load = 0;
        fclose(f);
    }
    if (load > 2.0) {
        snprintf(why, n, "1-minute load average %.2f is above 2", load);
        return 1;
    }
    return 0;
}

static uint64_t expected_digest(uint64_t seed, uint32_t M, uint32_t N) {
    uint64_t *A = malloc((size_t)M * N * sizeof(uint64_t));
    uint64_t *x = malloc((size_t)N * sizeof(uint64_t));
    uint64_t *y = malloc((size_t)M * sizeof(uint64_t));
    rx_omega_fill(seed, A, x, M, N);
    omega_matvec_reference(A, x, y, M, N);
    uint64_t d = rx_omega_digest(y, M);
    free(A); free(x); free(y);
    return d;
}

/* One outside request; waits for its result the way a client would. */
static uint64_t request(Env *e) {
    uint64_t seq = ++e->seq;
    uint64_t seed = seq * 0x2545F4914F6CDD1Dull;
    RxMutation m[4] = {
        { e->f.o.request, 0, seq }, { e->f.o.request, 1, REG_M },
        { e->f.o.request, 2, REG_N }, { e->f.o.request, 3, seed } };
    if (rx_world_publish_external(&e->w, e->ext_request, m, 4) <= 0) { e->refused++; return UINT64_MAX; }
    uint64_t t0 = now_ns();
    while (fld(e, e->f.o.result, 0) != seq) {
        if (now_ns() - t0 > 5000000000ull) { e->timed_out++; return UINT64_MAX; }
        sleep_us(20);
    }
    RxObject r;
    rx_world_read(&e->w, e->f.o.result, &r);
    e->served++;
    if (r.field[1] != expected_digest(seed, REG_M, REG_N)) e->wrong++;
    return r.field[3];
}

typedef int (*Until)(Env *e);

static uint64_t serve_until(Env *e, Until done, uint64_t limit) {
    uint64_t n = 0;
    while (!done(e) && n < limit) { request(e); n++; }
    return n;
}

/* Mean production ns over n requests. */
static uint64_t serve_mean(Env *e, uint32_t n) {
    uint64_t s = 0, k = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t ns = request(e);
        if (ns != UINT64_MAX) { s += ns; k++; }
    }
    return k ? s / k : 0;
}

static int u_selected1(Env *e) { return fld(e, e->f.o.selection, 0) >= 1; }
static int u_confirmed(Env *e) {
    return fld(e, e->a.o.prediction, 6) == RX_AIEN_PRED_CONFIRMED;
}
static int u_supported(Env *e) {
    return fld(e, e->a.o.hypothesis, 7) == RX_AIEN_HYP_SUPPORTED;
}

/* Receipt data. */
static int g_b_run;
static const char *g_b_skip = "";
static char g_b_skip_buf[160];
static uint64_t g_sel1_real, g_a725_ns, g_x925_old_ns, g_x925_new_ns, g_sel2_real;
static uint64_t g_ctrl_a725_ns, g_ctrl_x925_ns, g_ctrl_search, g_after_move, g_during;
static uint64_t g_hyp_kind, g_hyp_state, g_plan_seq, g_search_epoch, g_pred_seq;
static uint64_t g_improve_x100;
static int g_improved;

static uint32_t named(Env *e, const char *nm) {
    for (uint32_t i = 0; i < e->w.n_reactions; i++)
        if (!strcmp(e->w.reactions[i].desc.name, nm)) return i;
    return UINT32_MAX;
}

static uint64_t first_commit_after(Env *e, uint32_t rid, uint64_t after) {
    for (uint64_t id = after + 1; id <= e->w.n_crumbs; id++) {
        const RxCrumb *c = rx_world_crumb(&e->w, id);
        if (c && c->kind == RX_CRUMB_COMMIT && c->reaction == rid) return id;
    }
    return 0;
}

static void t_living(void) {
    printf("[*] living run: moved from Cortex-A725 to Cortex-X925, AIEN notices and Omega re-realizes\n");
    Env e;
    CHECK(envB_start(&e, 1) == 0, "setup");
    RxAienFaculty *a = &e.a;
    RxOmegaFaculty *f = &e.f;
    CHECK(move_all_threads(&g_a725) > 0, "move to A725");
    place(&e, CLASS_A725);

    serve_until(&e, u_selected1, 200000);
    CHECK(fld(&e, f->o.selection, 0) == 1, "Omega's own first search never finished");
    g_sel1_real = fld(&e, f->o.selection, 1);
    serve_until(&e, u_confirmed, 20000);
    CHECK(u_confirmed(&e), "AIEN never confirmed a prediction on A725");
    CHECK(fld(&e, a->o.prediction, 4) == CLASS_A725, "prediction class");
    g_a725_ns = fld(&e, a->o.prediction, 5);
    CHECK(fld(&e, a->o.plan, 0) == 0 && fld(&e, f->o.search, 0) == 1, "acted before anything changed");

    /* The environment moves the work. Nothing else is published. */
    CHECK(move_all_threads(&g_x925) > 0, "move to X925");
    place(&e, CLASS_X925);
    uint64_t c_move = e.w.n_crumbs;
    uint64_t served_at_move = e.served;
    g_after_move = serve_until(&e, u_supported, 200000);
    CHECK(u_supported(&e), "hypothesis never supported (state %llu, plan %llu, search %llu)",
          (unsigned long long)fld(&e, a->o.hypothesis, 7), (unsigned long long)fld(&e, a->o.plan, 0),
          (unsigned long long)fld(&e, f->o.search, 0));
    g_hyp_kind = fld(&e, a->o.hypothesis, 1);
    g_hyp_state = fld(&e, a->o.hypothesis, 7);
    g_plan_seq = fld(&e, a->o.plan, 0);
    g_search_epoch = fld(&e, f->o.search, 0);
    g_pred_seq = fld(&e, a->o.prediction, 0);
    CHECK(g_hyp_kind == RX_AIEN_HYP_CORE_CLASS, "hypothesis kind %llu", (unsigned long long)g_hyp_kind);
    CHECK(fld(&e, a->o.hypothesis, 3) == CLASS_A725 && fld(&e, a->o.hypothesis, 4) == CLASS_X925,
          "hypothesis classes");
    CHECK(g_plan_seq == 1 && fld(&e, a->o.plan, 5) == RX_AIEN_WHY_CORE_CLASS, "one plan, for the class");
    CHECK(g_search_epoch == 2 && fld(&e, f->o.search, 6) == 1, "Omega took the plan up: search %llu",
          (unsigned long long)g_search_epoch);
    CHECK(fld(&e, f->o.selection, 0) == 2, "no new selection");
    g_x925_old_ns = fld(&e, a->o.hypothesis, 6);
    g_x925_new_ns = fld(&e, a->o.prediction, 5);
    g_sel2_real = fld(&e, f->o.selection, 1);
    CHECK(fld(&e, a->o.prediction, 4) == CLASS_X925, "new prediction class");

    /* The causal chain: selection 2 <- measure <- ... <- search 2 <- omega.reconsider
     * <- plan <- hypothesis <- failed prediction <- demand + placement. */
    uint32_t r_recon = named(&e, "omega.reconsider");
    uint64_t c_plan = first_commit_after(&e, a->r_plan, c_move);
    uint64_t c_recon = first_commit_after(&e, r_recon, c_move);
    uint64_t c_sel2 = rx_world_explain(&e.w, f->o.selection, 0);
    uint64_t c_place = rx_world_explain(&e.w, a->o.placement, 1);
    CHECK(c_plan && c_recon && c_plan < c_recon && c_recon < c_sel2, "order plan %llu reconsider %llu sel %llu",
          (unsigned long long)c_plan, (unsigned long long)c_recon, (unsigned long long)c_sel2);
    CHECK(caused_by(&e, c_sel2, c_recon), "selection does not trace to omega.reconsider");
    CHECK(caused_by(&e, c_recon, c_plan), "reconsider does not trace to AIEN's plan");
    CHECK(caused_by(&e, c_plan, c_place), "plan does not trace to the placement");
    g_during = 0;
    for (uint64_t id = c_plan + 1; id < c_sel2; id++) {
        const RxCrumb *c = rx_world_crumb(&e.w, id);
        if (c && c->kind == RX_CRUMB_COMMIT && c->reaction == f->r_serve) g_during++;
    }
    CHECK(g_during > 0, "production stopped while AIEN and Omega worked");
    CHECK(e.w.reactions[f->r_watch].commits == 1, "omega.watch searched again on its own");
    printf("    requests: served %llu, wrong %llu, timed out %llu, refused %llu, crumb overflow %llu\n",
           (unsigned long long)e.served, (unsigned long long)e.wrong,
           (unsigned long long)e.timed_out, (unsigned long long)e.refused,
           (unsigned long long)e.w.stats.crumb_overflow);
    CHECK(e.wrong == 0, "%llu wrong results", (unsigned long long)e.wrong);
    CHECK(e.timed_out == 0 && e.refused == 0 && e.w.stats.crumb_overflow == 0,
          "harness/load: %llu timed out, %llu refused, %llu crumb overflow (not wrong results; "
          "rerun on a quiet machine)",
          (unsigned long long)e.timed_out, (unsigned long long)e.refused,
          (unsigned long long)e.w.stats.crumb_overflow);
    CHECK(e.served > served_at_move, "served");
    check_only_reactive(&e);

    /* The improvement is AIEN's doing only if Omega now runs something faster here. */
    g_improve_x100 = g_x925_new_ns ? g_x925_old_ns * 100u / g_x925_new_ns : 0;
    g_improved = g_sel1_real == 0 && g_sel2_real != 0 && g_improve_x100 >= 120;
    printf("    A725 record %llx at %llu ns; X925 under it %llu ns; X925 new record %llx at %llu ns (%.2fx)\n",
           (unsigned long long)g_sel1_real, (unsigned long long)g_a725_ns,
           (unsigned long long)g_x925_old_ns, (unsigned long long)g_sel2_real,
           (unsigned long long)g_x925_new_ns, g_improve_x100 / 100.0);
    CHECK(g_sel1_real == 0, "Omega kept a realization on A725 at a 10%% margin; the move cannot "
                            "show an improvement (record %llx)", (unsigned long long)g_sel1_real);
    CHECK(g_improved, "no improvement on X925: %llu -> %llu ns",
          (unsigned long long)g_x925_old_ns, (unsigned long long)g_x925_new_ns);
    env_stop(&e);

    /* Control: the same world and move, without AIEN. */
    printf("[*] control: the same move without AIEN leaves the body on the old record\n");
    Env c;
    CHECK(envB_start(&c, 0) == 0, "control setup");
    CHECK(move_all_threads(&g_a725) > 0, "control move to A725");
    serve_until(&c, u_selected1, 200000);
    g_ctrl_a725_ns = serve_mean(&c, 128);
    CHECK(move_all_threads(&g_x925) > 0, "control move to X925");
    for (uint64_t i = 0; i < g_after_move; i++) request(&c);
    g_ctrl_x925_ns = serve_mean(&c, 128);
    CHECK(settle(&c) == RX_OK, "control settle");
    g_ctrl_search = fld(&c, c.f.o.search, 0);
    CHECK(g_ctrl_search == 1 && fld(&c, c.f.o.selection, 0) == 1,
          "control re-searched without AIEN (search %llu)", (unsigned long long)g_ctrl_search);
    CHECK(fld(&c, c.f.o.selection, 1) == g_sel1_real, "control changed its record");
    CHECK(c.wrong == 0, "control: %llu wrong results", (unsigned long long)c.wrong);
    CHECK(c.timed_out == 0 && c.refused == 0 && c.w.stats.crumb_overflow == 0,
          "control harness/load: %llu timed out, %llu refused, %llu crumb overflow (not wrong "
          "results; rerun on a quiet machine)",
          (unsigned long long)c.timed_out, (unsigned long long)c.refused,
          (unsigned long long)c.w.stats.crumb_overflow);
    printf("    control: A725 %llu ns, X925 after %llu more requests %llu ns\n",
           (unsigned long long)g_ctrl_a725_ns, (unsigned long long)g_after_move,
           (unsigned long long)g_ctrl_x925_ns);
    env_stop(&c);
    g_b_run = 1;
}

/* ---- receipt ---- */

static void binary_digest(char out[65]) {
    strcpy(out, "unavailable");
    FILE *fp = fopen("/proc/self/exe", "rb");
    if (!fp) return;
    sha256_ctx c;
    sha256_init(&c);
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, fp)) > 0) sha256_update(&c, buf, n);
    fclose(fp);
    uint8_t d[32];
    sha256_final(&c, d);
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", d[i]);
}

static void write_receipt(void) {
    char path[512];
    if (omega_evidence_path("R11/rx_aien_faculty_receipt.json", path, sizeof path) != 0) return;
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    char commit[41];
    memset(commit, 0, sizeof commit);
    if (!omega_evidence_run_commit(commit)) memcpy(commit, "unknown", 8);
    const char *candidate = getenv("OMEGA_CANDIDATE_COMMIT");
    int bound = candidate && candidate[0] && strcmp(candidate, commit) == 0 &&
                !omega_evidence_tree_dirty();
    const char *aienos = getenv("AIENOS_COMMIT");
    char digest[65];
    binary_digest(digest);
    struct utsname u;
    memset(&u, 0, sizeof u);
    uname(&u);
    const char *gate = g_fail ? "FAIL" : g_b_run ? "PASS" : "HOST_PASS_LIVING_RUN_NOT_EXERCISED";
    fprintf(fp,
            "{\n"
            "  \"schema\": \"AIEN_RX_R11_AIEN_FACULTY_V1\",\n"
            "  \"run_id\": \"%s\",\n"
            "  \"candidate_commit\": %s%s%s,\n"
            "  \"candidate_bound\": %s,\n"
            "  \"run_commit\": \"%s\",\n"
            "  \"tree_dirty\": %s,\n"
            "  \"aienos_commit\": %s%s%s,\n"
            "  \"checks\": %d,\n"
            "  \"failures\": %d,\n"
            "  \"test_binary_sha256\": \"%s\",\n"
            "  \"host\": {\"sysname\": \"%s\", \"release\": \"%s\", \"machine\": \"%s\"},\n"
            "  \"cpus\": {\"cortex_x925\": \"%s\", \"cortex_a725\": \"%s\"},\n"
            "  \"hardware_scope\": \"host processor only; no graphics processor is used\",\n"
            "  \"cognition_model\": \"explicit statistical belief model (interval means, tolerance "
            "bands, streaks); not a neural model\",\n"
            "  \"living_run\": {\n"
            "    \"exercised\": %s,\n"
            "    \"skip_reason\": \"%s\",\n"
            "    \"operation\": \"omega_matvec (M12), regime %ux%u\",\n"
            "    \"a725_record_realization_word0\": \"0x%llx\",\n"
            "    \"a725_predicted_ns_per_call\": %llu,\n"
            "    \"x925_observed_ns_under_old_record\": %llu,\n"
            "    \"x925_record_realization_word0\": \"0x%llx\",\n"
            "    \"x925_predicted_ns_under_new_record\": %llu,\n"
            "    \"improvement_x100\": %llu,\n"
            "    \"requests_after_move_until_supported\": %llu,\n"
            "    \"production_commits_between_plan_and_new_selection\": %llu,\n"
            "    \"hypothesis_kind\": %llu, \"hypothesis_state\": %llu,\n"
            "    \"plans\": %llu, \"search_epochs\": %llu, \"predictions\": %llu,\n"
            "    \"control_without_aien\": {\"a725_ns\": %llu, \"x925_ns\": %llu, \"search_epochs\": %llu}\n"
            "  },\n"
            "  \"gates\": {\n"
            "    \"R11_CONTINUOUS_COGNITION\": \"%s\",\n"
            "    \"aien_caused_improvement\": %s,\n"
            "    \"not_claimed\": [\"R8\", \"R13\", \"neural cognition\", \"durable beliefs (R9)\", "
            "\"new memory as a trigger\"]\n"
            "  }\n"
            "}\n",
            omega_evidence_run_id(), candidate ? "\"" : "", candidate ? candidate : "null",
            candidate ? "\"" : "", bound ? "true" : "false", commit,
            omega_evidence_tree_dirty() ? "true" : "false", aienos ? "\"" : "",
            aienos ? aienos : "null", aienos ? "\"" : "", g_checks, g_fail, digest, u.sysname,
            u.release, u.machine, g_x_cpus, g_a_cpus, g_b_run ? "true" : "false", g_b_skip, REG_M,
            REG_N, (unsigned long long)g_sel1_real, (unsigned long long)g_a725_ns,
            (unsigned long long)g_x925_old_ns, (unsigned long long)g_sel2_real,
            (unsigned long long)g_x925_new_ns, (unsigned long long)g_improve_x100,
            (unsigned long long)g_after_move, (unsigned long long)g_during,
            (unsigned long long)g_hyp_kind, (unsigned long long)g_hyp_state,
            (unsigned long long)g_plan_seq, (unsigned long long)g_search_epoch,
            (unsigned long long)g_pred_seq, (unsigned long long)g_ctrl_a725_ns,
            (unsigned long long)g_ctrl_x925_ns, (unsigned long long)g_ctrl_search, gate,
            g_improved ? "true" : "false");
    fclose(fp);
    printf("receipt: %s\n", path);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    t_no_record();
    t_steady_and_outlier();
    t_core_class();
    t_drift();
    t_settled_failure_relearns();
    t_goal();
    t_authority();
#if defined(__aarch64__)
    find_core_classes();
    char why[128];
    if (g_nx > 0 && g_na > 0 && other_load(why, sizeof why)) {
        snprintf(g_b_skip_buf, sizeof g_b_skip_buf, "SKIPPED-LOADED: %s", why);
        g_b_skip = g_b_skip_buf;
        printf("[-] SKIPPED-LOADED: living run not started: %s\n", why);
    } else if (g_nx > 0 && g_na > 0) t_living();
    else g_b_skip = "this host does not have both Cortex-X925 and Cortex-A725 cores";
#else
    g_b_skip = "Omega's realizations are AArch64";
#endif
    if (!g_b_run) printf("[-] living run not exercised: %s\n", g_b_skip);
    printf("checks %d failures %d\n", g_checks, g_fail);
    write_receipt();
    return g_fail ? 1 : 0;
}
