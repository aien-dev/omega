/*
 * R8: AEGIS resident authority (ADR 0016 §41, scope note 2026-09-28).
 *
 * A client principal ("worker", subject 51) holds its authority in
 * capability slots in the world. Its work reaction reads its reference from a
 * slot. The test only publishes outside stimuli and reads the world:
 *
 *   intent (outside) -> worker.ask -> request -> aegis.decide -> decision
 *     -> root.install (AIENOS mint) -> slot -> worker.tick wakes and runs
 *
 * and afterwards, with the slot live, every input runs worker.tick with no
 * AEGIS activation at all. Attacks on the slow path must be refused by the
 * root whatever AEGIS or a hostile writer publishes.
 *
 * Authority is the native AIENOS capability authority.
 */
#include "runtime/aienos_cap.h"
#include "runtime/rx_aegis.h"
#include "runtime/rx_world.h"
#include "omega_evidence.h"
#include "sha256.h"

#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

enum { SUBJ_EXTERNAL = 100, ISSUER = 3, SUBJ_WORKER = 51, SUBJ_OTHER = 52, SUBJ_ROGUE = 60 };

#define RES_WORK    0x9000001ull   /* rule 1: R|W, no lease, no approval */
#define RES_INPUT   0x9000002ull   /* the worker's input, static read */
#define RES_INTENT  0x9000003ull   /* outside stimulus: what the worker should now ask for */
#define RES_LOG     0x9000005ull   /* the worker's own log, static read and write */
#define RES_EFFECT  0x9000011ull   /* rule 2: R|W|E, needs a human */
#define RES_LEASED  0x9000021ull   /* rule 3: R, lease 100 */
#define RES_OUTSIDE 0x9100000ull   /* rule 4: a policy mistake outside the worker's domain */
#define RES_NORULE  0x9000040ull   /* in the domain, no rule */
#define RES_ROGUE   0x9800000ull

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

typedef struct {
    AienosCapAdmin *admin;
    AienosCapView *view;
    RxWorld w;
    RxAegisFaculty a;
    RxObjRef input, intent, work, leased_out, lease_log;
    RxCapRef ext_input, ext_intent;
    uint32_t r_tick, r_ask, r_lease;
    uint64_t inputs;
} Env;

static RxCapRef mint(Env *e, uint32_t subject, uint64_t resource, uint32_t rights) {
    AienosCapRef office;
    aienos_cap_office(e->admin, &office);
    AienosCapMint m = { ISSUER, subject, resource, rights, 0, { UINT32_MAX, 0 }, office };
    AienosCapRef r = { UINT32_MAX, 0 };
    if (aienos_cap_mint(e->admin, &m, &r) != 0) r = (AienosCapRef){ UINT32_MAX, 0 };
    return (RxCapRef){ r.cap_id, r.generation };
}

static uint64_t fld(Env *e, RxObjRef r, uint32_t i) {
    RxObject o;
    if (rx_world_read(&e->w, r, &o) != RX_OK) return UINT64_MAX;
    return o.field[i];
}

static int settle(Env *e) { return rx_world_wait_quiescent(&e->w, 20000); }

static RxObjRef slot(Env *e, uint32_t j) { return e->a.o[0].slot[j]; }

/* ---- the worker's reactions ---- */

static const RxSnapshotDep *in_of(const RxCtx *c, RxObjRef r) {
    for (uint32_t i = 0; i < c->n_in; i++)
        if (c->in[i].obj.id == r.id) return &c->in[i];
    return NULL;
}

/* worker.tick: count each input into `work`. Needs R|W on RES_WORK from slot 0. */
static int fn_tick(RxCtx *c) {
    Env *e = c->user;
    const RxSnapshotDep *in = in_of(c, e->input), *wk = in_of(c, e->work);
    if (!in || !wk) return -1;
    if (in->field[0] == wk->field[1]) return 0;     /* this input already counted */
    c->out[c->n_out++] = (RxMutation){ e->work, 0, wk->field[0] + 1 };
    c->out[c->n_out++] = (RxMutation){ e->work, 1, in->field[0] };
    return 0;
}

/* worker.lease: read the leased object and log the input it saw. Needs READ on
 * RES_LEASED from slot 2; the log is the worker's own. */
static int fn_lease(RxCtx *c) {
    Env *e = c->user;
    const RxSnapshotDep *in = in_of(c, e->input), *lo = in_of(c, e->leased_out);
    if (!in || !lo) return -1;
    c->out[c->n_out++] = (RxMutation){ e->lease_log, 0, in->field[0] };
    return 0;
}

/* worker.ask: the worker turns an intent into its own authority request.
 * intent: 0 seq, 1 resource, 2 rights, 3 lease, 4 slot, 5 op */
static int fn_ask(RxCtx *c) {
    Env *e = c->user;
    const RxSnapshotDep *it = in_of(c, e->intent), *rq = in_of(c, e->a.o[0].request);
    if (!it || !rq) return -1;
    if (it->field[0] == 0 || it->field[0] == rq->field[0]) return 0;
    for (uint32_t i = 0; i < 6; i++)
        c->out[c->n_out++] = (RxMutation){ e->a.o[0].request, i, it->field[i] };
    return 0;
}

static int env_start(Env *e) {
    memset(e, 0, sizeof(*e));
    if (aienos_cap_start(&e->admin, &e->view) != 0) return -1;
    if (rx_world_init_native(&e->w, e->view, 4, 1u << 18) != RX_OK) return -1;
    e->w.external_subject = SUBJ_EXTERNAL;
    const uint32_t R = RX_RIGHT_READ, RW = RX_RIGHT_READ | RX_RIGHT_WRITE;

    RxAegisPolicy pol;
    memset(&pol, 0, sizeof pol);
    pol.n_rules = 4;
    pol.rules[0] = (RxAegisRule){ 1, SUBJ_WORKER, 0x9000000, 0x900000f, RW, 0, 0 };
    pol.rules[1] = (RxAegisRule){ 2, SUBJ_WORKER, 0x9000010, 0x900001f, RW | RX_RIGHT_EFFECT, 0, 1 };
    pol.rules[2] = (RxAegisRule){ 3, SUBJ_WORKER, 0x9000020, 0x900002f, R, 100, 0 };
    pol.rules[3] = (RxAegisRule){ 4, SUBJ_WORKER, RES_OUTSIDE, RES_OUTSIDE, RW, 0, 0 };
    RxAegisClient cl = { SUBJ_WORKER, 0x9000000, 0x90000ff, RW | RX_RIGHT_EFFECT };
    if (rx_aegis_create(&e->a, &e->w, e->admin, &pol, &cl, 1) != RX_OK) return -1;

    RxAegisCaps caps;
    caps.aegis_request = mint(e, RX_AEGIS_SUBJ, rx_aegis_res(0, RX_AEGIS_RES_REQUEST), R);
    caps.aegis_approval = mint(e, RX_AEGIS_SUBJ, rx_aegis_res(0, RX_AEGIS_RES_APPROVAL), R);
    caps.aegis_decision = mint(e, RX_AEGIS_SUBJ, rx_aegis_res(0, RX_AEGIS_RES_DECISION), RW);
    caps.root_request = mint(e, RX_AEGIS_ROOT_SUBJ, rx_aegis_res(0, RX_AEGIS_RES_REQUEST), R);
    caps.root_decision = mint(e, RX_AEGIS_ROOT_SUBJ, rx_aegis_res(0, RX_AEGIS_RES_DECISION), R);
    for (uint32_t j = 0; j < RX_AEGIS_SLOTS; j++)
        caps.root_slot[j] = mint(e, RX_AEGIS_ROOT_SUBJ, rx_aegis_res(0, RX_AEGIS_RES_SLOT0 + j), RW);
    if (rx_aegis_register(&e->a, &caps) != RX_OK) return -1;

    uint64_t z[RX_MAX_FIELDS] = { 0 };
    if (rx_world_create(&e->w, 0x9001, RX_PERSIST_RESIDENT, RES_INPUT, z, &e->input) != RX_OK ||
        rx_world_create(&e->w, 0x9002, RX_PERSIST_RESIDENT, RES_INTENT, z, &e->intent) != RX_OK ||
        rx_world_create(&e->w, 0x9003, RX_PERSIST_RESIDENT, RES_WORK, z, &e->work) != RX_OK ||
        rx_world_create(&e->w, 0x9004, RX_PERSIST_RESIDENT, RES_LEASED, z, &e->leased_out) != RX_OK ||
        rx_world_create(&e->w, 0x9005, RX_PERSIST_RESIDENT, RES_LOG, z, &e->lease_log) != RX_OK)
        return -1;
    e->ext_input = mint(e, SUBJ_EXTERNAL, RES_INPUT, RX_RIGHT_WRITE);
    e->ext_intent = mint(e, SUBJ_EXTERNAL, RES_INTENT, RX_RIGHT_WRITE);

    /* Static authority the worker is born with: read its input and its intent,
     * read its own slots, write its own request. Nothing on RES_WORK. */
    RxCapRef w_input = mint(e, SUBJ_WORKER, RES_INPUT, R);
    RxCapRef w_intent = mint(e, SUBJ_WORKER, RES_INTENT, R);
    RxCapRef w_request = mint(e, SUBJ_WORKER, rx_aegis_res(0, RX_AEGIS_RES_REQUEST), RW);
    RxCapRef w_slot[RX_AEGIS_SLOTS];
    for (uint32_t j = 0; j < RX_AEGIS_SLOTS; j++)
        w_slot[j] = mint(e, SUBJ_WORKER, rx_aegis_res(0, RX_AEGIS_RES_SLOT0 + j), R);

    RxReactionDesc d;
    memset(&d, 0, sizeof d);
    d.name = "worker.tick";
    d.faculty = RX_FACULTY_EXTERNAL;
    d.subject = SUBJ_WORKER;
    d.priority = RX_PRIO_INTERACTIVE;
    d.fn = fn_tick;
    d.user = e;
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){ e->input, RX_ALL_FIELDS };
    d.n_reads = 1;
    d.reads[0] = (RxDep){ e->work, RX_ALL_FIELDS };
    d.n_writes = 1;
    d.writes[0] = (RxDep){ e->work, RX_ALL_FIELDS };
    d.n_caps = 3;
    d.caps[0] = (RxCapNeed){ w_input, RES_INPUT, R };
    d.caps[1] = (RxCapNeed){ { UINT32_MAX, 0 }, RES_WORK, RW };
    d.caps[2] = (RxCapNeed){ w_slot[0], rx_aegis_res(0, RX_AEGIS_RES_SLOT0), R };
    rx_aegis_use_slot(&d, 1, slot(e, 0));
    if (rx_world_add_reaction(&e->w, &d, &e->r_tick) != RX_OK) return -1;

    memset(&d, 0, sizeof d);
    d.name = "worker.lease";
    d.faculty = RX_FACULTY_EXTERNAL;
    d.subject = SUBJ_WORKER;
    d.priority = RX_PRIO_INTERACTIVE;
    d.fn = fn_lease;
    d.user = e;
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){ e->input, RX_ALL_FIELDS };
    d.n_reads = 1;
    d.reads[0] = (RxDep){ e->leased_out, RX_ALL_FIELDS };
    d.n_writes = 1;
    d.writes[0] = (RxDep){ e->lease_log, RX_ALL_FIELDS };
    d.n_caps = 4;
    d.caps[0] = (RxCapNeed){ w_input, RES_INPUT, R };
    d.caps[1] = (RxCapNeed){ { UINT32_MAX, 0 }, RES_LEASED, R };
    d.caps[2] = (RxCapNeed){ w_slot[2], rx_aegis_res(0, RX_AEGIS_RES_SLOT0 + 2), R };
    d.caps[3] = (RxCapNeed){ mint(e, SUBJ_WORKER, RES_LOG, RW), RES_LOG, RW };
    rx_aegis_use_slot(&d, 1, slot(e, 2));
    if (rx_world_add_reaction(&e->w, &d, &e->r_lease) != RX_OK) return -1;

    memset(&d, 0, sizeof d);
    d.name = "worker.ask";
    d.faculty = RX_FACULTY_EXTERNAL;
    d.subject = SUBJ_WORKER;
    d.priority = RX_PRIO_INTERACTIVE;
    d.fn = fn_ask;
    d.user = e;
    d.stamp_proposed = true;
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){ e->intent, RX_ALL_FIELDS };
    d.n_reads = 1;
    d.reads[0] = (RxDep){ e->a.o[0].request, RX_ALL_FIELDS };
    d.n_writes = 1;
    d.writes[0] = (RxDep){ e->a.o[0].request, RX_ALL_FIELDS };
    d.n_caps = 2;
    d.caps[0] = (RxCapNeed){ w_intent, RES_INTENT, R };
    d.caps[1] = (RxCapNeed){ w_request, rx_aegis_res(0, RX_AEGIS_RES_REQUEST), RW };
    if (rx_world_add_reaction(&e->w, &d, &e->r_ask) != RX_OK) return -1;
    return 0;
}

static void env_stop(Env *e) {
    rx_world_wait_quiescent(&e->w, 20000);
    rx_world_destroy(&e->w);
    rx_aegis_destroy(&e->a);
    aienos_cap_stop(e->admin, e->view);
}

static int64_t input(Env *e) {
    RxMutation m = { e->input, 0, ++e->inputs };
    int64_t id = rx_world_publish_external(&e->w, e->ext_input, &m, 1);
    CHECK(id > 0, "input refused");
    CHECK(settle(e) == RX_OK, "settle");
    return id;
}

static uint64_t intent(Env *e, uint64_t res, uint64_t rights, uint64_t lease, uint64_t sl,
                       uint64_t op) {
    /* Past anything the request has carried, including a forged one. */
    uint64_t s = fld(e, e->intent, 0), r = fld(e, e->a.o[0].request, 0);
    s = (s > r ? s : r) + 1;
    RxMutation m[6] = { { e->intent, 0, s }, { e->intent, 1, res }, { e->intent, 2, rights },
                        { e->intent, 3, lease }, { e->intent, 4, sl }, { e->intent, 5, op } };
    CHECK(rx_world_publish_external(&e->w, e->ext_intent, m, 6) > 0, "intent refused");
    CHECK(settle(e) == RX_OK, "settle");
    return s;
}

static void approve(Env *e, uint64_t seq, uint64_t verdict) {
    RxCapRef c = mint(e, SUBJ_EXTERNAL, rx_aegis_res(0, RX_AEGIS_RES_APPROVAL), RX_RIGHT_WRITE);
    RxMutation m[2] = { { e->a.o[0].approval, 0, seq }, { e->a.o[0].approval, 1, verdict } };
    CHECK(rx_world_publish_external(&e->w, c, m, 2) > 0, "approval refused");
    CHECK(settle(e) == RX_OK, "settle");
}

static uint64_t acts(Env *e, uint32_t rid) { return e->w.reactions[rid].activations; }

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

/* Receipt data. */
static uint64_t g_fast_n, g_fast_decide, g_fast_install, g_slow_ns, g_check_ns, g_eval_ns;
static uint64_t g_spawn_ns, g_mints, g_revokes;

/* ---- tests ---- */

static void t_slow_then_fast(void) {
    printf("[*] no authority: blocked, AEGIS untouched; the worker asks; then the fast path\n");
    Env e;
    CHECK(env_start(&e) == 0, "setup");
    RxAegisFaculty *a = &e.a;

    input(&e);
    CHECK(e.w.reactions[e.r_tick].commits == 0 && fld(&e, e.work, 0) == 0, "ran without authority");
    CHECK(e.w.stats.blocked_authority > 0, "no block recorded");
    CHECK(acts(&e, a->r_decide[0]) == 0 && acts(&e, a->r_install[0]) == 0,
          "a block by itself woke AEGIS or the root");

    /* The worker is told what it will need. It asks; nobody waits. */
    uint64_t t0 = now_ns();
    uint64_t seq = intent(&e, RES_WORK, RX_RIGHT_READ | RX_RIGHT_WRITE, 0, 0, RX_AEGIS_OP_ACQUIRE);
    g_slow_ns = now_ns() - t0;
    CHECK(fld(&e, a->o[0].request, 0) == seq, "request not published");
    CHECK(fld(&e, a->o[0].decision, 0) == seq && fld(&e, a->o[0].decision, 1) == RX_AEGIS_GRANT &&
          fld(&e, a->o[0].decision, 6) == 1, "decision %llu rule %llu",
          (unsigned long long)fld(&e, a->o[0].decision, 1), (unsigned long long)fld(&e, a->o[0].decision, 6));
    CHECK(fld(&e, slot(&e, 0), 2) == RX_AEGIS_SLOT_LIVE && fld(&e, slot(&e, 0), 5) == seq &&
          fld(&e, slot(&e, 0), 3) == RES_WORK, "slot 0 state %llu why %llu",
          (unsigned long long)fld(&e, slot(&e, 0), 2), (unsigned long long)fld(&e, slot(&e, 0), 7));
    /* The pending input ran when the slot filled. */
    CHECK(fld(&e, e.work, 0) == 1 && fld(&e, e.work, 1) == 1, "blocked input did not run after the grant");
    CHECK(a->mints == 1, "mints %llu", (unsigned long long)a->mints);

    /* The minted reference is the worker's, from AEGIS, bounded as decided. */
    RxCapEntry ce;
    RxCapRef ref = { (uint32_t)fld(&e, slot(&e, 0), 0), fld(&e, slot(&e, 0), 1) };
    CHECK(rx_world_inspect_cap(&e.w, ref, &ce) == RX_CAP_OK && ce.subject == SUBJ_WORKER &&
          ce.resource == RES_WORK && ce.rights == (RX_RIGHT_READ | RX_RIGHT_WRITE) &&
          ce.issuer == RX_AEGIS_SUBJ, "minted entry");

    /* Causal chain: work <- slot <- decision <- request <- intent. */
    uint64_t c_work = rx_world_explain(&e.w, e.work, 0);
    uint64_t c_slot = rx_world_explain(&e.w, slot(&e, 0), 0);
    uint64_t c_dec = rx_world_explain(&e.w, a->o[0].decision, 1);
    uint64_t c_req = rx_world_explain(&e.w, a->o[0].request, 0);
    uint64_t c_int = rx_world_explain(&e.w, e.intent, 0);
    CHECK(caused_by(&e, c_work, c_slot) && caused_by(&e, c_slot, c_dec) &&
          caused_by(&e, c_dec, c_req) && caused_by(&e, c_req, c_int), "authority chain broken");

    /* Fast path: many inputs, no AEGIS or root activation. */
    uint64_t d0 = acts(&e, a->r_decide[0]), i0 = acts(&e, a->r_install[0]);
    uint64_t w0 = fld(&e, e.work, 0);
    g_fast_n = 2000;
    for (uint64_t i = 0; i < g_fast_n; i++) {
        RxMutation m = { e.input, 0, ++e.inputs };
        rx_world_publish_external(&e.w, e.ext_input, &m, 1);
        if ((i & 63) == 63) settle(&e);
    }
    CHECK(settle(&e) == RX_OK, "settle");
    CHECK(fld(&e, e.work, 1) == e.inputs && fld(&e, e.work, 0) > w0, "fast path lost the last input");
    g_fast_decide = acts(&e, a->r_decide[0]) - d0;
    g_fast_install = acts(&e, a->r_install[0]) - i0;
    CHECK(g_fast_decide == 0 && g_fast_install == 0, "fast path woke AEGIS %llu / root %llu times",
          (unsigned long long)g_fast_decide, (unsigned long long)g_fast_install);

    /* Cost of one fast-path authority check against one synchronous policy decision. */
    const int N = 200000;
    uint64_t t = now_ns();
    int bad = 0;
    for (int i = 0; i < N; i++)
        bad |= rx_world_validate_cap(&e.w, ref, SUBJ_WORKER, RES_WORK, RX_RIGHT_WRITE, NULL);
    g_check_ns = (now_ns() - t) / N;
    CHECK(bad == 0, "fast check failed");
    uint64_t l;
    uint32_t why, na;
    volatile uint32_t sink = 0;
    t = now_ns();
    for (int i = 0; i < N; i++)
        sink += rx_aegis_evaluate(&a->policy, SUBJ_WORKER, RES_WORK, RX_RIGHT_WRITE, 0, &l, &why, &na);
    g_eval_ns = (now_ns() - t) / N;
    (void)sink;

    /* Release: AEGIS revokes through the root; the worker stops; the world does not. */
    intent(&e, RES_WORK, 0, 0, 0, RX_AEGIS_OP_RELEASE);
    CHECK(fld(&e, a->o[0].decision, 1) == RX_AEGIS_REVOKE, "release decision");
    CHECK(fld(&e, slot(&e, 0), 2) == RX_AEGIS_SLOT_REVOKED, "slot after release");
    CHECK(rx_world_validate_cap(&e.w, ref, SUBJ_WORKER, RES_WORK, RX_RIGHT_WRITE, NULL) != RX_CAP_OK,
          "released reference still valid");
    uint64_t before = fld(&e, e.work, 0);
    input(&e);
    CHECK(fld(&e, e.work, 0) == before, "worked after release");
    CHECK(fld(&e, e.input, 0) == e.inputs, "the world stopped with the worker");
    g_mints = a->mints;
    g_revokes = a->revokes;
    env_stop(&e);
}

static void t_escalate_deny(void) {
    printf("[*] a human approves or rejects; privileged and unruled requests are denied\n");
    Env e;
    CHECK(env_start(&e) == 0, "setup");
    RxAegisFaculty *a = &e.a;
    const uint64_t RWE = RX_RIGHT_READ | RX_RIGHT_WRITE | RX_RIGHT_EFFECT;

    uint64_t s1 = intent(&e, RES_EFFECT, RWE, 0, 1, RX_AEGIS_OP_ACQUIRE);
    CHECK(fld(&e, a->o[0].decision, 1) == RX_AEGIS_ESCALATE, "not escalated");
    CHECK(fld(&e, slot(&e, 1), 2) == RX_AEGIS_SLOT_EMPTY && a->mints == 0, "minted before approval");
    approve(&e, s1, RX_AEGIS_APPROVE);
    CHECK(fld(&e, a->o[0].decision, 1) == RX_AEGIS_GRANT && fld(&e, slot(&e, 1), 2) == RX_AEGIS_SLOT_LIVE,
          "approved request not installed");

    uint64_t s2 = intent(&e, RES_EFFECT + 1, RWE, 0, 3, RX_AEGIS_OP_ACQUIRE);
    CHECK(fld(&e, a->o[0].decision, 1) == RX_AEGIS_ESCALATE, "second not escalated");
    approve(&e, s2, RX_AEGIS_REJECT);
    CHECK(fld(&e, a->o[0].decision, 1) == RX_AEGIS_DENY && fld(&e, a->o[0].decision, 6) == RX_AEGIS_WHY_HUMAN,
          "rejection");
    CHECK(fld(&e, slot(&e, 3), 2) == RX_AEGIS_SLOT_EMPTY, "rejected request installed");

    intent(&e, RES_WORK, RX_RIGHT_READ | RX_RIGHT_MINT, 0, 3, RX_AEGIS_OP_ACQUIRE);
    CHECK(fld(&e, a->o[0].decision, 1) == RX_AEGIS_DENY &&
          fld(&e, a->o[0].decision, 6) == RX_AEGIS_WHY_PRIVILEGED, "privileged request");
    intent(&e, RES_NORULE, RX_RIGHT_READ, 0, 3, RX_AEGIS_OP_ACQUIRE);
    CHECK(fld(&e, a->o[0].decision, 1) == RX_AEGIS_DENY &&
          fld(&e, a->o[0].decision, 6) == RX_AEGIS_WHY_NO_RULE, "unruled request");
    intent(&e, RES_WORK, RWE, 0, 3, RX_AEGIS_OP_ACQUIRE);
    CHECK(fld(&e, a->o[0].decision, 1) == RX_AEGIS_DENY, "rights beyond the rule");
    intent(&e, RES_WORK, RX_RIGHT_READ, 0, 9, RX_AEGIS_OP_ACQUIRE);
    CHECK(fld(&e, a->o[0].decision, 6) == RX_AEGIS_WHY_BAD_REQUEST, "slot out of range");
    CHECK(fld(&e, slot(&e, 3), 2) == RX_AEGIS_SLOT_EMPTY && a->mints == 1, "a denial minted");
    env_stop(&e);
}

static void t_lease(void) {
    printf("[*] a leased grant expires on the AIENOS clock; renewal replaces it\n");
    Env e;
    CHECK(env_start(&e) == 0, "setup");
    RxAegisFaculty *a = &e.a;
    intent(&e, RES_LEASED, RX_RIGHT_READ, 5000, 2, RX_AEGIS_OP_ACQUIRE);
    CHECK(fld(&e, a->o[0].decision, 4) == 100, "policy did not cap the lease: %llu",
          (unsigned long long)fld(&e, a->o[0].decision, 4));
    CHECK(fld(&e, slot(&e, 2), 2) == RX_AEGIS_SLOT_LIVE && fld(&e, slot(&e, 2), 6) != 0, "leased slot");
    RxCapRef old = { (uint32_t)fld(&e, slot(&e, 2), 0), fld(&e, slot(&e, 2), 1) };
    input(&e);
    CHECK(fld(&e, e.lease_log, 0) == e.inputs, "leased reader did not run inside its lease");

    AienosCapRef office;
    aienos_cap_office(e.admin, &office);
    CHECK(rx_world_validate_cap(&e.w, old, SUBJ_WORKER, RES_LEASED, RX_RIGHT_READ, NULL) == RX_CAP_OK,
          "lease valid before expiry");
    CHECK(aienos_cap_advance_clock(e.admin, office, 101) == 0, "clock");
    CHECK(rx_world_validate_cap(&e.w, old, SUBJ_WORKER, RES_LEASED, RX_RIGHT_READ, NULL) ==
          RX_CAP_ERR_EXPIRED, "lease did not expire");
    uint64_t seen = fld(&e, e.lease_log, 0);
    input(&e);
    CHECK(fld(&e, e.lease_log, 0) == seen, "leased reader ran after expiry");
    intent(&e, RES_LEASED, RX_RIGHT_READ, 0, 2, RX_AEGIS_OP_ACQUIRE);
    input(&e);
    CHECK(fld(&e, e.lease_log, 0) == e.inputs, "leased reader did not resume after renewal");
    RxCapRef neu = { (uint32_t)fld(&e, slot(&e, 2), 0), fld(&e, slot(&e, 2), 1) };
    CHECK(neu.cap_id != old.cap_id || neu.generation != old.generation, "renewal reused the old grant");
    CHECK(rx_world_validate_cap(&e.w, neu, SUBJ_WORKER, RES_LEASED, RX_RIGHT_READ, NULL) == RX_CAP_OK,
          "renewed grant invalid");
    CHECK(a->mints == 2 && a->revokes == 1, "mints %llu revokes %llu", (unsigned long long)a->mints,
          (unsigned long long)a->revokes);
    env_stop(&e);
}

/* rogue.forge: a non-AEGIS reaction that was wrongly given WRITE on the decision. */
static struct { RxObjRef decision, request; uint64_t seq; } g_forge;
static int fn_forge(RxCtx *c) {
    const RxSnapshotDep *rq = NULL;
    for (uint32_t i = 0; i < c->n_in; i++)
        if (c->in[i].obj.id == g_forge.request.id) rq = &c->in[i];
    if (!rq || rq->field[0] == 0) return 0;
    uint64_t v[8] = { rq->field[0], RX_AEGIS_GRANT, rq->field[1], rq->field[2], 0, rq->field[4], 1,
                      rq->field[5] };
    for (uint32_t i = 0; i < 8; i++) c->out[c->n_out++] = (RxMutation){ g_forge.decision, i, v[i] };
    return 0;
}

static void t_attacks(void) {
    printf("[*] forged decisions, requests written for the worker, slot stuffing, and a policy "
           "mistake are refused by the root\n");
    Env e;
    CHECK(env_start(&e) == 0, "setup");
    RxAegisFaculty *a = &e.a;
    const uint32_t R = RX_RIGHT_READ, RW = RX_RIGHT_READ | RX_RIGHT_WRITE;

    /* 1. Policy mistake: rule 4 grants outside the worker's domain. */
    intent(&e, RES_OUTSIDE, RW, 0, 3, RX_AEGIS_OP_ACQUIRE);
    CHECK(fld(&e, a->o[0].decision, 1) == RX_AEGIS_GRANT, "rule 4 should grant (the mistake)");
    CHECK(fld(&e, slot(&e, 3), 2) == RX_AEGIS_SLOT_EMPTY && fld(&e, slot(&e, 3), 7) == RX_AEGIS_WHY_DOMAIN,
          "root installed outside the domain: state %llu why %llu",
          (unsigned long long)fld(&e, slot(&e, 3), 2), (unsigned long long)fld(&e, slot(&e, 3), 7));
    CHECK(a->mints == 0, "minted outside the domain");

    /* 2. A request written for the worker by someone else (an outside writer
     * that was wrongly given WRITE on the worker's request). AEGIS judges the
     * worker's policy and grants; the root sees who wrote it. */
    RxCapRef ext_req = mint(&e, SUBJ_EXTERNAL, rx_aegis_res(0, RX_AEGIS_RES_REQUEST), RX_RIGHT_WRITE);
    uint64_t s = fld(&e, a->o[0].request, 0) + 1;
    RxMutation m[6] = { { a->o[0].request, 0, s }, { a->o[0].request, 1, RES_WORK },
                        { a->o[0].request, 2, RW }, { a->o[0].request, 3, 0 },
                        { a->o[0].request, 4, 2 }, { a->o[0].request, 5, RX_AEGIS_OP_ACQUIRE } };
    CHECK(rx_world_publish_external(&e.w, ext_req, m, 6) > 0, "outside request");
    CHECK(settle(&e) == RX_OK, "settle");
    CHECK(fld(&e, a->o[0].decision, 0) == s && fld(&e, a->o[0].decision, 1) == RX_AEGIS_GRANT,
          "AEGIS decided the forged request");
    CHECK(fld(&e, slot(&e, 2), 2) == RX_AEGIS_SLOT_EMPTY &&
          fld(&e, slot(&e, 2), 7) == RX_AEGIS_WHY_REQUEST_ORIGIN, "root installed a forged request: why %llu",
          (unsigned long long)fld(&e, slot(&e, 2), 7));
    CHECK(a->mints == 0, "minted for a forged request");

    /* 3. A forged decision from a non-AEGIS reaction, and from outside. */
    g_forge.decision = a->o[0].decision;
    g_forge.request = a->o[0].request;
    RxReactionDesc d;
    memset(&d, 0, sizeof d);
    d.name = "rogue.forge";
    d.faculty = RX_FACULTY_EXTERNAL;
    d.subject = SUBJ_ROGUE;
    d.priority = RX_PRIO_FOREGROUND;
    d.fn = fn_forge;
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){ a->o[0].request, RX_ALL_FIELDS };
    d.n_writes = 1;
    d.writes[0] = (RxDep){ a->o[0].decision, RX_ALL_FIELDS };
    d.n_caps = 2;
    d.caps[0] = (RxCapNeed){ mint(&e, SUBJ_ROGUE, rx_aegis_res(0, RX_AEGIS_RES_REQUEST), R),
                             rx_aegis_res(0, RX_AEGIS_RES_REQUEST), R };
    RxCapRef rogue_dec = mint(&e, SUBJ_ROGUE, rx_aegis_res(0, RX_AEGIS_RES_DECISION), RW);
    d.caps[1] = (RxCapNeed){ rogue_dec, rx_aegis_res(0, RX_AEGIS_RES_DECISION), RW };
    uint32_t rid;
    CHECK(rx_world_add_reaction(&e.w, &d, &rid) == RX_OK, "rogue register");
    /* The worker asks for something AEGIS denies; the rogue overwrites it with a grant. */
    intent(&e, RES_NORULE, R, 0, 1, RX_AEGIS_OP_ACQUIRE);
    CHECK(e.w.reactions[rid].commits > 0, "rogue never wrote (activations %llu, state %s)",
          (unsigned long long)e.w.reactions[rid].activations, rx_state_name(e.w.reactions[rid].state));
    for (uint64_t id = 1; id <= e.w.n_crumbs && e.w.reactions[rid].commits == 0; id++) {
        const RxCrumb *c = rx_world_crumb(&e.w, id);
        if (c && c->reaction == rid)
            fprintf(stderr, "    rogue crumb %s reason %d\n", rx_crumb_kind_name(c->kind), c->reason);
    }
    CHECK(fld(&e, slot(&e, 1), 2) == RX_AEGIS_SLOT_EMPTY, "root installed a forged decision");
    CHECK(fld(&e, slot(&e, 1), 7) == RX_AEGIS_WHY_DECISION_ORIGIN || fld(&e, slot(&e, 1), 5) == 0,
          "refusal reason %llu", (unsigned long long)fld(&e, slot(&e, 1), 7));
    CHECK(a->mints == 0, "minted for a forged decision");

    RxCapRef ext_dec = mint(&e, SUBJ_EXTERNAL, rx_aegis_res(0, RX_AEGIS_RES_DECISION), RX_RIGHT_WRITE);
    uint64_t rs = fld(&e, a->o[0].request, 0);
    RxMutation fd[8] = { { a->o[0].decision, 0, rs }, { a->o[0].decision, 1, RX_AEGIS_GRANT },
                         { a->o[0].decision, 2, RES_NORULE }, { a->o[0].decision, 3, R },
                         { a->o[0].decision, 4, 0 }, { a->o[0].decision, 5, 0 },
                         { a->o[0].decision, 6, 1 }, { a->o[0].decision, 7, RX_AEGIS_OP_ACQUIRE } };
    CHECK(rx_world_publish_external(&e.w, ext_dec, fd, 8) > 0, "outside decision");
    CHECK(settle(&e) == RX_OK, "settle");
    CHECK(fld(&e, slot(&e, 0), 2) == RX_AEGIS_SLOT_EMPTY && a->mints == 0,
          "root installed an outside decision");

    /* 4. Slot stuffing: a hostile writer puts another subject's valid reference
     * in the worker's slot. The authority still says no. */
    RxCapRef other = mint(&e, SUBJ_OTHER, RES_WORK, RW);
    RxCapRef ext_slot = mint(&e, SUBJ_EXTERNAL, rx_aegis_res(0, RX_AEGIS_RES_SLOT0), RX_RIGHT_WRITE);
    RxMutation st[3] = { { slot(&e, 0), 0, other.cap_id }, { slot(&e, 0), 1, other.generation },
                         { slot(&e, 0), 2, RX_AEGIS_SLOT_LIVE } };
    CHECK(rx_world_publish_external(&e.w, ext_slot, st, 3) > 0, "stuffed slot");
    CHECK(settle(&e) == RX_OK, "settle");
    uint64_t w0 = fld(&e, e.work, 0);
    input(&e);
    CHECK(fld(&e, e.work, 0) == w0, "ran on another subject's reference");
    CHECK(e.w.reactions[e.r_tick].state != RX_RUNNING, "state");

    /* A principal wrongly holding WRITE on the decision can jam it (it cannot
     * get anything minted). The remedy is to revoke that grant. Then the
     * honest path works again. */
    AienosCapRef office;
    aienos_cap_office(e.admin, &office);
    CHECK(aienos_cap_revoke(e.admin, office, (AienosCapRef){ rogue_dec.cap_id, rogue_dec.generation }) == 0,
          "revoke rogue");
    CHECK(a->mints == 0, "the rogue got something minted");
    intent(&e, RES_WORK, RW, 0, 0, RX_AEGIS_OP_ACQUIRE);
    CHECK(fld(&e, slot(&e, 0), 2) == RX_AEGIS_SLOT_LIVE && a->mints == 1, "honest grant after attacks");
    CHECK(fld(&e, e.work, 0) == w0 + 1, "worker did not resume");
    CHECK(rx_world_validate_cap(&e.w, other, SUBJ_OTHER, RES_WORK, RX_RIGHT_WRITE, NULL) == RX_CAP_OK,
          "the root revoked a reference it never minted because it sat in a slot");
    CHECK(a->revokes == 0, "revokes %llu", (unsigned long long)a->revokes);

    /* No AEGIS or root activation happened without a waking cause. */
    uint64_t uncaused = 0, n = 0;
    for (uint64_t id = 1; id <= e.w.n_crumbs; id++) {
        const RxCrumb *c = rx_world_crumb(&e.w, id);
        if (!c || c->reaction == UINT32_MAX) continue;
        if (c->faculty != RX_FACULTY_AEGIS && c->faculty != RX_FACULTY_ROOT) continue;
        n++;
        if (!c->wake_cause) uncaused++;
    }
    CHECK(n > 0 && uncaused == 0, "authority crumbs %llu uncaused %llu", (unsigned long long)n,
          (unsigned long long)uncaused);
    uint64_t checked = 0;
    CHECK(rx_world_verify_crumbs(&e.w, &checked) == 0 && checked > 0, "crumb chain");
    env_stop(&e);
}

/* The floor of any shell-out policy path (the current spark-aegis route):
 * spawn and reap one process that does nothing. */
static void t_spawn_floor(void) {
    char *argv[] = { "/bin/true", NULL };
    const int N = 50;
    uint64_t t = now_ns();
    int ok = 0;
    for (int i = 0; i < N; i++) {
        pid_t pid;
        if (posix_spawn(&pid, "/bin/true", NULL, NULL, argv, environ) != 0) continue;
        int st;
        if (waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0) ok++;
    }
    g_spawn_ns = ok ? (now_ns() - t) / (uint64_t)ok : 0;
    CHECK(ok == N, "spawn floor %d/%d", ok, N);
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
    if (omega_evidence_path("R8/rx_aegis_resident_receipt.json", path, sizeof path) != 0) return;
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
    fprintf(fp,
            "{\n"
            "  \"schema\": \"AIEN_RX_R8_AEGIS_RESIDENT_V1\",\n"
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
            "  \"scope\": \"host processor; the native AIENOS authority library is the root\",\n"
            "  \"fast_path\": {\"actions\": %llu, \"aegis_activations\": %llu, \"root_activations\": %llu,\n"
            "                \"synchronous_policy_round_trips_saved\": %llu},\n"
            "  \"cost_ns\": {\"fast_path_authority_check\": %llu, \"in_process_policy_evaluation\": %llu,\n"
            "              \"process_spawn_floor_for_shell_out\": %llu,\n"
            "              \"slow_path_intent_to_work\": %llu},\n"
            "  \"root\": {\"mints\": %llu, \"revokes\": %llu},\n"
            "  \"gates\": {\n"
            "    \"R8_CONSTITUTIONAL_AEGIS\": \"%s\",\n"
            "    \"not_claimed\": [\"AIENOS kernel (the authority runs as a host library)\", "
            "\"policy learned or changed at run time\", \"R13\"]\n"
            "  }\n"
            "}\n",
            omega_evidence_run_id(), candidate ? "\"" : "", candidate ? candidate : "null",
            candidate ? "\"" : "", bound ? "true" : "false", commit,
            omega_evidence_tree_dirty() ? "true" : "false", aienos ? "\"" : "",
            aienos ? aienos : "null", aienos ? "\"" : "", g_checks, g_fail, digest, u.sysname,
            u.release, u.machine, (unsigned long long)g_fast_n, (unsigned long long)g_fast_decide,
            (unsigned long long)g_fast_install, (unsigned long long)g_fast_n,
            (unsigned long long)g_check_ns, (unsigned long long)g_eval_ns,
            (unsigned long long)g_spawn_ns, (unsigned long long)g_slow_ns,
            (unsigned long long)g_mints, (unsigned long long)g_revokes, g_fail ? "FAIL" : "PASS");
    fclose(fp);
    printf("receipt: %s\n", path);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    t_slow_then_fast();
    t_escalate_deny();
    t_lease();
    t_attacks();
    t_spawn_floor();
    printf("    fast check %llu ns, policy evaluation %llu ns, process spawn %llu ns, "
           "slow path %llu ns\n", (unsigned long long)g_check_ns, (unsigned long long)g_eval_ns,
           (unsigned long long)g_spawn_ns, (unsigned long long)g_slow_ns);
    printf("checks %d failures %d\n", g_checks, g_fail);
    write_receipt();
    return g_fail ? 1 : 0;
}
