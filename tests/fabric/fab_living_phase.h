/*
 * fab_living_phase.h -- Fabric F5-0 in a living composition (Lane 13).
 *
 * One scenario, shared by the R13 living host test (the composition attached
 * to the living World) and the stand-alone test-fabric-living (a composition
 * that owns its World, plain and under ASan/UBSan, run twice for
 * determinism). The caller supplies attach/close; everything else is here.
 *
 *   machine A  this machine (the living system's identity); its Capability
 *              Graph is the composition router's graph; one local Skill
 *              (FL_SKILL_LOCAL, cost 10) provides FX_OP_SCALE.
 *   machine B  a second simulated machine on the loopback transport: joins,
 *              advertises FL_SKILL_REMOTE (cost 5, so ranked first) for
 *              FX_OP_SCALE and FL_SKILL_ONLY for FL_OP_REMOTE_ONLY, which
 *              nobody else provides. Its procedures run through the Fabric
 *              dispatcher (fab_dispatch.h, in-process stand-in: F5-0 has no
 *              work message).
 *   machine X  enrolled nowhere (forger, wrong machine).
 *
 * Steps (clock in microseconds, simulated, so every run is identical):
 *   1  B joins and advertises; A holds both entries as CQ_SRC_FABRIC, not held.
 *   2  goal 1: route = [B remote, A local]; the Fabric candidate runs on B,
 *      passes the contract, wins and is committed; its claim says HOME_FABRIC
 *      with B's advertised digest.
 *   3  forged advertisement (B's identity, wrong key; and a genuine one with
 *      one byte altered): FAB_E_AUTH, graph unchanged.
 *   4  wrong AienMachineId: B relays X's record (FAB_E_MISMATCH), a message
 *      for X delivered to A (FAB_E_MISMATCH), a requirement pinned to X
 *      (SR_E_MACHINE, World unchanged), a route retargeted to X refused by
 *      the dispatcher.
 *   5  B's procedure differs from what it advertised: dispatcher refuses
 *      (FAB_DX_DIGEST), the local candidate wins. B returns a wrong result
 *      under the right digest: the AEGIS contract rejects it, local wins.
 *   6  mid-run loss: B's LEAVE waits in A's mailbox when the goal is routed;
 *      the dispatcher applies it first, the route is withdrawn
 *      (FAB_DX_ROUTE), the local candidate wins.
 *   7  B rejoins (generation 2); a replay of its generation-1 traffic is
 *      FAB_E_STALE_GEN; goal runs on B again.
 *   8  stale lease: the clock passes B's lease before A has applied it; the
 *      router already excludes B (lease check), the goal falls back to A;
 *      B's late ADVERTISE is FAB_E_LEASE_EXPIRED; after the tick B is LOST
 *      and its entries are withdrawn; the remote-only operation is refused
 *      (SR_E_NO_CANDIDATE), World unchanged.
 */
#ifndef FAB_LIVING_PHASE_H
#define FAB_LIVING_PHASE_H

/* Lane 32: test-build only. The production program (docs/r16-production-entry-point.md)
 * never compiles this; the build refuses it without -DAIEN_TEST_BUILD=1. */
#ifndef AIEN_TEST_BUILD
#error "fab_living_phase.h (Fabric living test phase with fixed test keys) is test and simulation only: build with -DAIEN_TEST_BUILD=1, never in the production program"
#endif

#include "fabric/fab_dispatch.h"
#include "fabric/fab_hmac.h"
#include "fabric/fab_loopback.h"
#include "../runtime/rx_compose_fixture.h"

#include <stdio.h>
#include <string.h>

/* Link-map marker (test build only): tools/r16_prod_hygiene.sh refuses any
 * production binary that carries an aien_test_build_* symbol. */
__attribute__((used)) static const char aien_test_build_fab_living_phase[] =
    "AIEN_TEST_BUILD piece: Fabric living test phase (fixed test keys fl-key-*)";

enum { FL_OP_REMOTE_ONLY = 2 };
enum { FL_SKILL_LOCAL = 7, FL_SKILL_REMOTE = 9, FL_SKILL_ONLY = 10 };
#define FL_MS 1000ull
#define FL_T0 (1000 * FL_MS)               /* start of the scenario clock */
#define FL_LEASE (500 * FL_MS)

typedef int (*FlAttachFn)(void *ctx, RxCompose *c, const char *dir, const AienMachineId *self,
                          const SrRouter *router);
typedef void (*FlCloseFn)(void *ctx, RxCompose *c);

typedef struct {
    int ran, ok;
    unsigned checks, failures;
    uint64_t result[6];                 /* committed results of the six goals */
    uint32_t winner[6];                 /* winning candidate (0 = first route) */
    uint32_t remote_wins;               /* goals won by the Fabric candidate */
    uint64_t dispatched, dispatch_refused[FAB_DX_N];
    int forged_wrong_key, forged_altered, relayed_record, misdelivered, stale_gen,
        lease_expired;                  /* FabVerdict codes */
    int pinned_wrong_machine, remote_only_after_loss;   /* rx_compose_run returns */
    int fabric_held;                    /* Fabric candidates ever seen held (must stay 0) */
    int fabric_auth_lost;               /* Fabric candidates without B's carried authority (must stay 0) */
    uint8_t record_digest[32], fabric_digest[32];
} FlReceipt;

typedef struct {
    AienMachineId a, b, x;
    uint8_t key_a[32], key_b[32], key_x[32];
    AienMachineId roster_ids[2];
    uint8_t roster_keys[2][32];
    FabRoster roster;
    FabLoop loop;
    AienMachineId slots[3][8];
    AienMachineIndex ix[3];
    CqCatalog cat_a, cat_b, cat_x;
    FabHmacAuth auth_a, auth_b;
    FabNode node_a, node_b;
    uint8_t gen1_msg[FAB_MSG_MAX];
    size_t gen1_len;
    AgSkillTable skills_a, skills_b;
    SrRouter router;
    FabDispatch disp;
    CqKey own_remote, own_only, own_x;
    RxCompose c;
    FlReceipt *g;
} FlRig;

static int fl_b_bad;                    /* B's procedure breaks the contract */

static uint64_t fl_sk_remote(const uint64_t *in, uint32_t n, uint32_t attempt, int *failed) {
    (void)attempt;
    *failed = 0;
    return (n ? in[0] : 0) * 3 + 1 + (fl_b_bad ? 2 : 0);
}

#define FL_CHECK(cond, ...) do { r->g->checks++; if (!(cond)) { r->g->failures++; \
    fprintf(stderr, "FABRIC-LIVING FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static int fl_catalog(CqCatalog *c, AienMachineIndex *ix, AienMachineId *slots,
                      const AienMachineId *self) {
    aien_mid_index_init(ix, slots, 8);
    if (cq_catalog_init_canonical(c, ix, self, 8, 16) != CQ_OK) return -1;
    uint32_t all = CQ_SRC(CQ_SRC_GRAPH) | CQ_SRC(CQ_SRC_SKILL) | CQ_SRC(CQ_SRC_FABRIC);
    /* The same operation catalog on every machine (the JOIN compares it). */
    if (cq_op_define(c, FX_OP_SCALE, 0, all) != CQ_OK) return -2;
    if (cq_op_define(c, FL_OP_REMOTE_ONLY, 0, all) != CQ_OK) return -3;
    return 0;
}

/* Register one Skill of `self` in its own catalog; returns its key. */
static int fl_own_skill(CqCatalog *cat, const AgSkill *s, uint32_t op, uint32_t cap, uint32_t rights,
                        uint64_t cost, uint64_t generation, CqKey *key) {
    SrSkill k;
    memset(&k, 0, sizeof k);
    k.skill_id = s->id;
    k.version = 1;
    memcpy(k.digest, s->identity, 32);
    k.machine = cat->self_machine;
    CqEntry e = fx_provide(cap, op, 80 + cap, cost);
    e.generation = generation;
    if (rights) {                        /* needs authority on its own machine */
        e.auth_resource = 0x5000u + cap;
        e.auth_rights = rights;
    }
    if (sr_register_skill(cat, &k, &e, 1) != SR_OK) return -1;
    if (cq_catalog_build(cat) != CQ_OK) return -2;
    e.skill_id = s->id;
    e.skill_version = 1;
    memcpy(e.skill_digest, s->identity, 32);
    e.machine_id = cat->self_machine;
    e.source = CQ_SRC_SKILL;
    *key = cq_key_of(&e);
    return cq_lookup(cat, key) ? 0 : -3;
}

static SrRequirement fl_req(uint32_t op) {
    SrRequirement q = fx_requirement();
    q.need.semantic_operation = op;
    q.local_only = 0;
    return q;
}

/* Deliver what waits for B (A's traffic) without the lock: no run is active. */
static void fl_pump_b(FlRig *r, uint64_t now) {
    FabVerdict v;
    while (fab_poll(&r->node_b, now, &v) == 1) {}
}

/* Inject one message into A's mailbox and apply it; returns its verdict. */
static int fl_deliver_a(FlRig *r, const uint8_t *msg, size_t len, uint64_t now) {
    FabVerdict v;
    memset(&v, 0, sizeof v);
    if (fab_loop_inject(&r->loop, &r->a, msg, len) != FAB_OK) return 999;
    if (fab_poll(&r->node_a, now, &v) != 1) return 998;
    return v.code;
}

/* First waiting message for `to` from `from` of `kind`, copied out. */
static size_t fl_peek(FlRig *r, const AienMachineId *to, const AienMachineId *from, uint32_t kind,
                      uint8_t *out) {
    uint8_t rec[AIEN_MID_RECORD_BYTES];
    aien_mid_encode(from, rec);
    for (uint32_t i = 0; i < r->loop.n; i++) {
        const FabLoopBox *b = &r->loop.box[i];
        if (!aien_mid_equal(&b->id, to)) continue;
        for (uint32_t k = 0; k < b->count; k++) {
            const FabLoopMsg *m = &b->q[(b->head + k) % FAB_LOOP_DEPTH];
            if (m->b[5] == kind && memcmp(m->b + 8, rec, sizeof rec) == 0) {
                memcpy(out, m->b, m->len);
                return m->len;
            }
        }
    }
    return 0;
}

static uint32_t fl_fabric_entries(FlRig *r, uint64_t now, uint32_t op) {
    SrRequirement q = fl_req(op);
    SrRoute rt[RXC_K];
    int n = sr_route_alternatives(&r->router, &q, NULL, now, rt, RXC_K);
    uint32_t f = 0;
    for (int i = 0; i < n; i++) {
        if (rt[i].chosen.source == CQ_SRC_FABRIC) f++;
        if (rt[i].chosen.source != CQ_SRC_FABRIC) continue;
        if (rt[i].chosen.required_authority.held) r->g->fabric_held++;
        /* The entry carries the authority B needs on its own machine (the
         * routing never mints it here): resource 0x5000+cap, READ. */
        uint64_t want = 0x5000u + (op == FX_OP_SCALE ? 2u : 3u);
        if (rt[i].chosen.required_authority.resource != want ||
            rt[i].chosen.required_authority.rights != RX_RIGHT_READ)
            r->g->fabric_auth_lost++;
    }
    return f;
}

static void fl_word_digest(const uint64_t *w, uint8_t d[32]) {
    for (int i = 0; i < 4; i++)
        for (int b = 0; b < 8; b++) d[8 * i + b] = (uint8_t)(w[i] >> (8 * b));
}

/* One goal; checks the outcome and records the winner. Returns the result
 * struct's outcome (0 on a run error). */
static int fl_goal(FlRig *r, int i, uint64_t input, uint64_t now, int want_remote_first,
                   uint32_t want_winner) {
    RxcResult o;
    SrRequirement q = fl_req(FX_OP_SCALE);
    int rc = rx_compose_run(&r->c, input, &q, NULL, now, &o);
    FL_CHECK(rc == RX_OK, "goal %d run %d", i, rc);
    if (rc != RX_OK) return 0;
    FL_CHECK(o.outcome == RXC_OUT_COMMITTED, "goal %d outcome %d", i, o.outcome);
    FL_CHECK(o.result == input * 3 + 1, "goal %d result %llu", i, (unsigned long long)o.result);
    FL_CHECK(o.winner == want_winner, "goal %d winner %u want %u", i, o.winner, want_winner);
    if (want_remote_first) {
        FL_CHECK(o.n_alternatives == 2 && o.route[0].verdict == SR_E_REMOTE &&
                 o.route[0].chosen.source == CQ_SRC_FABRIC &&
                 o.route[0].chosen.skill_id == FL_SKILL_REMOTE && o.route[0].target_known &&
                 aien_mid_equal(&o.route[0].target, &r->b) && o.route[1].verdict == SR_OK &&
                 o.route[1].chosen.skill_id == FL_SKILL_LOCAL,
                 "goal %d route: n=%u v0=%d src0=%u", i, o.n_alternatives, o.route[0].verdict,
                 o.route[0].chosen.source);
        FL_CHECK(!o.route[0].chosen.required_authority.held, "goal %d Fabric candidate held", i);
    } else {
        FL_CHECK(o.n_alternatives == 1 && o.route[0].verdict == SR_OK &&
                 o.route[0].chosen.skill_id == FL_SKILL_LOCAL,
                 "goal %d fallback route: n=%u v0=%d", i, o.n_alternatives, o.route[0].verdict);
    }
    /* The claim of the winner says where its Skill ran. */
    uint32_t w = o.winner < RXC_K ? o.winner : 0;
    const CxObject *cl = o.cx_candidate[w] ? cx_get(&r->c.cx, o.cx_candidate[w]) : NULL;
    FL_CHECK(cl && cl->n >= RXC_CP_WORDS, "goal %d winner claim", i);
    if (cl && cl->n >= RXC_CP_WORDS) {
        const uint64_t *p = cx_payload(&r->c.cx, cl);
        int remote = want_remote_first && want_winner == 0;
        FL_CHECK(p[RXC_CP_HOME] == (remote ? RXC_HOME_FABRIC : RXC_HOME_LOCAL),
                 "goal %d claim home %llu", i, (unsigned long long)p[RXC_CP_HOME]);
        uint8_t d[32];
        fl_word_digest(p + RXC_CP_SKILLDIG0, d);
        const uint8_t *want = remote ? r->skills_b.skill[0].identity : r->skills_a.skill[0].identity;
        FL_CHECK(memcmp(d, want, 32) == 0, "goal %d claim digest", i);
        if (remote) r->g->remote_wins++;
    }
    if (i < 6) {
        r->g->result[i] = o.result;
        r->g->winner[i] = o.winner;
    }
    return o.outcome;
}

static int fl_setup(FlRig *r, const AienMachineId *self) {
    r->a = *self;
    const char *rb = "fabric-living-B", *rx = "fabric-living-X";
    if (aien_mid_derive(AIEN_MID_ROOT_PROVISIONED, (const uint8_t *)rb, strlen(rb), &r->b) != AIEN_MID_OK ||
        aien_mid_derive(AIEN_MID_ROOT_PROVISIONED, (const uint8_t *)rx, strlen(rx), &r->x) != AIEN_MID_OK)
        return -1;
    sha256_hash((const uint8_t *)"fl-key-A", 8, r->key_a);
    sha256_hash((const uint8_t *)"fl-key-B", 8, r->key_b);
    sha256_hash((const uint8_t *)"fl-key-X", 8, r->key_x);
    r->roster_ids[0] = r->a;
    r->roster_ids[1] = r->b;
    memcpy(r->roster_keys[0], r->key_a, 32);
    memcpy(r->roster_keys[1], r->key_b, 32);
    r->roster = (FabRoster){ 2, r->roster_ids };
    fab_loop_init(&r->loop);
    if (fab_loop_add(&r->loop, &r->a) || fab_loop_add(&r->loop, &r->b) ||
        fab_loop_add(&r->loop, &r->x))
        return -2;
    if (fl_catalog(&r->cat_a, &r->ix[0], r->slots[0], &r->a) ||
        fl_catalog(&r->cat_b, &r->ix[1], r->slots[1], &r->b) ||
        fl_catalog(&r->cat_x, &r->ix[2], r->slots[2], &r->x))
        return -3;
    /* A: one local Skill for the scaled operation. */
    r->skills_a.n = 1;
    r->skills_a.skill[0] = (AgSkill){ FL_SKILL_LOCAL, fx_sk_a, { 0xA7, 0x01 } };
    CqKey ka;
    if (fl_own_skill(&r->cat_a, &r->skills_a.skill[0], FX_OP_SCALE, 1, 0, 10, 1, &ka)) return -4;
    /* B: the cheaper provider of the same operation and the only provider of
     * FL_OP_REMOTE_ONLY. */
    r->skills_b.n = 2;
    r->skills_b.skill[0] = (AgSkill){ FL_SKILL_REMOTE, fl_sk_remote, { 0xF9, 0xB0, 0x01 } };
    r->skills_b.skill[1] = (AgSkill){ FL_SKILL_ONLY, fl_sk_remote, { 0xFA, 0xB0, 0x02 } };
    if (fab_hmac_auth_init(&r->auth_a, &r->a, r->key_a, 2, r->roster_ids,
                           (const uint8_t (*)[32])r->roster_keys) != FAB_OK ||
        fab_hmac_auth_init(&r->auth_b, &r->b, r->key_b, 2, r->roster_ids,
                           (const uint8_t (*)[32])r->roster_keys) != FAB_OK)
        return -5;
    FabConfig ca = { r->a, &r->roster, &r->cat_a, &r->loop.transport, &r->auth_a.auth, FL_LEASE, 1, NULL, 0 };
    FabConfig cb = { r->b, &r->roster, &r->cat_b, &r->loop.transport, &r->auth_b.auth, FL_LEASE, 1, NULL, 0 };
    if (fab_node_init(&r->node_a, &ca) != FAB_OK || fab_node_init(&r->node_b, &cb) != FAB_OK)
        return -6;
    if (fl_own_skill(&r->cat_b, &r->skills_b.skill[0], FX_OP_SCALE, 2, RX_RIGHT_READ, 5,
                     fab_entry_generation(&r->node_b, 1), &r->own_remote) ||
        fl_own_skill(&r->cat_b, &r->skills_b.skill[1], FL_OP_REMOTE_ONLY, 3, RX_RIGHT_READ, 5,
                     fab_entry_generation(&r->node_b, 1), &r->own_only))
        return -7;
    r->router = (SrRouter){ &r->cat_a, &r->skills_a };
    if (fab_dispatch_init(&r->disp, &r->node_a) != FAB_OK ||
        fab_dispatch_add(&r->disp, &r->b, &r->skills_b) != FAB_OK)
        return -8;
    return 0;
}

static void fl_free(FlRig *r) {
    fab_dispatch_destroy(&r->disp);
    cq_catalog_free(&r->cat_a);
    cq_catalog_free(&r->cat_b);
    cq_catalog_free(&r->cat_x);
}

/* B joins at `now` and advertises both entries; A applies it. */
static void fl_b_online(FlRig *r, uint64_t now, int a_joins) {
    if (a_joins) FL_CHECK(fab_join(&r->node_a, FL_LEASE, now) == FAB_OK, "A join");
    FL_CHECK(fab_join(&r->node_b, FL_LEASE, now) == FAB_OK, "B join");
    fl_pump_b(r, now);
    FL_CHECK(fab_dispatch_pump(&r->disp, now) >= 1, "A applies B's join");
    FL_CHECK(fab_advertise(&r->node_b, &r->own_remote, CQ_WIRE_ADVERTISE, now) == FAB_OK, "B adv");
    FL_CHECK(fab_advertise(&r->node_b, &r->own_only, CQ_WIRE_ADVERTISE, now) == FAB_OK, "B adv 2");
    FL_CHECK(fab_dispatch_pump(&r->disp, now) == 2 && r->disp.refused_msgs == 0,
             "A applies B's advertisements (refused %llu)", (unsigned long long)r->disp.refused_msgs);
    FL_CHECK(fab_member_live(&r->node_a, &r->b, now), "B live at A");
}

/* The scenario. attach must leave r->c attached/open on `dir` with r->router. */
static int fl_run(const AienMachineId *self, const char *dir, FlAttachFn attach, FlCloseFn close,
                  void *ctx, FlReceipt *g) {
    static FlRig rig;
    FlRig *r = &rig;
    memset(r, 0, sizeof *r);
    memset(g, 0, sizeof *g);
    r->g = g;
    g->ran = 1;
    fl_b_bad = 0;
    int setup = fl_setup(r, self);
    FL_CHECK(setup == 0, "setup %d", setup);
    if (setup) { fl_free(r); return -1; }
    uint64_t t = FL_T0;

    /* 1: B joins and advertises; A sees Fabric candidates, never held. */
    fl_b_online(r, t, 1);
    FL_CHECK(fl_fabric_entries(r, t, FX_OP_SCALE) == 1, "Fabric candidate for the scaled op");
    FL_CHECK(fl_fabric_entries(r, t, FL_OP_REMOTE_ONLY) == 1, "Fabric candidate for the remote-only op");

    int rc = attach(ctx, &r->c, dir, self, &r->router);
    FL_CHECK(rc == RX_OK, "attach %d", rc);
    if (rc != RX_OK) { fl_free(r); return -1; }
    FL_CHECK(rx_compose_set_remote(&r->c, fab_dispatch_run, &r->disp) == RX_OK, "set remote");

    /* 2: the Fabric candidate runs on B and wins. */
    t += 10 * FL_MS;
    fl_goal(r, 0, 5, t, 1, 0);
    FL_CHECK(r->disp.dispatched == 1 && r->c.remote.ran == 1, "one dispatch to B");

    /* 3: forged advertisements. */
    {
        uint8_t msg[FAB_MSG_MAX];
        size_t len;
        /* B's identity and a cheaper offer, tagged with a key that is not
         * B's (an impostor sealing as B). */
        uint8_t rec[CQ_WIRE_BYTES];
        CqEntry e = *cq_lookup(&r->cat_b, &r->own_remote);
        e.cost = 1;                      /* would outrank everything */
        FL_CHECK(cq_wire_encode(&r->cat_b, &e, CQ_WIRE_ADVERTISE, rec) == CQ_OK, "encode");
        FL_CHECK(fab_seal(&r->node_b, &r->a, FAB_MSG_ADVERTISE, rec, sizeof rec, t, msg, &len) == FAB_OK, "seal");
        fab_hmac_sig64(r->key_x, msg, len - FAB_SIG_BYTES, msg + len - FAB_SIG_BYTES);
        g->forged_wrong_key = fl_deliver_a(r, msg, len, t);
        FL_CHECK(g->forged_wrong_key == FAB_E_AUTH, "forged (wrong key) %d", g->forged_wrong_key);
        /* A genuine advertisement with one byte altered in flight. The
         * unaltered original is kept back for the replay in step 7. */
        FL_CHECK(fab_advertise(&r->node_b, &r->own_remote, CQ_WIRE_ADVERTISE, t) == FAB_OK, "B re-adv");
        len = fl_peek(r, &r->a, &r->b, FAB_MSG_ADVERTISE, msg);
        FL_CHECK(len == FAB_MSG_MAX, "captured B advertisement");
        memcpy(r->gen1_msg, msg, len);
        r->gen1_len = len;
        msg[FAB_HDR_BYTES + 144] ^= 0x7F;     /* one byte of the record */
        r->loop.box[0].count = 0;             /* drop the original from A's mailbox */
        g->forged_altered = fl_deliver_a(r, msg, len, t);
        FL_CHECK(g->forged_altered == FAB_E_AUTH, "forged (altered) %d", g->forged_altered);
        const CqEntry *got = NULL;
        SrRequirement q = fl_req(FX_OP_SCALE);
        SrRoute rt;
        FL_CHECK(sr_route(&r->router, &q, NULL, t, &rt) == SR_E_REMOTE &&
                 (got = cq_lookup(&r->cat_a, &rt.key)) && got->cost == 5,
                 "forgeries changed the graph");
    }

    /* 4: wrong AienMachineId. */
    {
        uint8_t msg[FAB_MSG_MAX], rec[CQ_WIRE_BYTES];
        size_t len;
        AgSkill xs = { 11, fl_sk_remote, { 0xEE } };
        FL_CHECK(fl_own_skill(&r->cat_x, &xs, FX_OP_SCALE, 4, RX_RIGHT_READ, 1, 1, &r->own_x) == 0, "X skill");
        FL_CHECK(cq_wire_encode(&r->cat_x, cq_lookup(&r->cat_x, &r->own_x), CQ_WIRE_ADVERTISE, rec) == CQ_OK, "X rec");
        /* B relays X's record: nobody speaks for another machine. */
        FL_CHECK(fab_seal(&r->node_b, &r->a, FAB_MSG_ADVERTISE, rec, sizeof rec, t, msg, &len) == FAB_OK, "seal relay");
        g->relayed_record = fl_deliver_a(r, msg, len, t);
        FL_CHECK(g->relayed_record == FAB_E_MISMATCH, "relayed record %d", g->relayed_record);
        /* B's message for X, delivered to A. */
        uint8_t body[8] = { 0 };
        body[2] = 0x07;
        FL_CHECK(fab_seal(&r->node_b, &r->x, FAB_MSG_RENEW, body, sizeof body, t, msg, &len) == FAB_OK, "seal renew");
        g->misdelivered = fl_deliver_a(r, msg, len, t);
        FL_CHECK(g->misdelivered == FAB_E_MISMATCH, "misdelivered %d", g->misdelivered);
        /* A requirement pinned to X: refused before anything is published. */
        JsBranchRef before = rx_compose_state(&r->c);
        SrRequirement q = fl_req(FX_OP_SCALE);
        q.pin_machine_set = 1;
        q.pin_machine = r->x;
        RxcResult o;
        g->pinned_wrong_machine = rx_compose_run(&r->c, 6, &q, NULL, t, &o);
        JsBranchRef after = rx_compose_state(&r->c);
        FL_CHECK(g->pinned_wrong_machine == SR_E_MACHINE && before.id == after.id &&
                 before.gen == after.gen, "pinned to X: %d", g->pinned_wrong_machine);
        /* A route retargeted to X is refused by the dispatcher. */
        SrRoute rt;
        q.pin_machine_set = 0;
        FL_CHECK(sr_route(&r->router, &q, NULL, t, &rt) == SR_E_REMOTE, "route to B");
        rt.target = r->x;
        uint64_t res = 0;
        uint64_t d0 = r->disp.dispatched;
        FL_CHECK(fab_dispatch_run(&r->disp, &r->router, &rt, 6, t, &res) == FAB_DX_ROUTE &&
                 r->disp.dispatched == d0, "retargeted route dispatched");
    }

    /* 5: B runs something other than what it advertised; then a wrong
     * result under the right digest. The local candidate wins both. */
    t += 10 * FL_MS;
    r->skills_b.skill[0].identity[31] ^= 0x01;
    fl_goal(r, 1, 7, t, 1, 1);
    FL_CHECK(r->disp.last_refusal == FAB_DX_DIGEST, "digest refusal %d", r->disp.last_refusal);
    r->skills_b.skill[0].identity[31] ^= 0x01;
    fl_b_bad = 1;
    uint64_t before_bad = r->disp.dispatched;
    fl_goal(r, 2, 8, t, 1, 1);
    FL_CHECK(r->disp.dispatched == before_bad + 1, "wrong result was dispatched, then rejected");
    fl_b_bad = 0;

    /* 6: B leaves while the goal is being routed. */
    t += 10 * FL_MS;
    FL_CHECK(fab_leave(&r->node_b, t) == FAB_OK, "B leave");
    uint64_t refused0 = r->disp.refused[FAB_DX_ROUTE];
    fl_goal(r, 3, 9, t, 1, 1);
    FL_CHECK(r->disp.refused[FAB_DX_ROUTE] == refused0 + 1, "mid-run loss refused");
    FL_CHECK(!fab_member_live(&r->node_a, &r->b, t) && fl_fabric_entries(r, t, FX_OP_SCALE) == 0,
             "B's entries withdrawn after LEAVE");

    /* 7: B rejoins with a new generation; its old traffic is stale. */
    t += 10 * FL_MS;
    {
        FL_CHECK(fab_node_set_generation(&r->node_b, 2) == FAB_OK, "B gen 2");
        /* Re-number B's entries under the new generation. */
        CqEntry e1 = *cq_lookup(&r->cat_b, &r->own_remote), e2 = *cq_lookup(&r->cat_b, &r->own_only);
        e1.generation = e2.generation = fab_entry_generation(&r->node_b, 1);
        FL_CHECK(cq_register(&r->cat_b, &e1, NULL, 0) == CQ_OK &&
                 cq_register(&r->cat_b, &e2, NULL, 0) == CQ_OK && cq_catalog_build(&r->cat_b) == CQ_OK,
                 "B re-registers");
        fl_b_online(r, t, 0);
        g->stale_gen = fl_deliver_a(r, r->gen1_msg, r->gen1_len, t);
        FL_CHECK(g->stale_gen == FAB_E_STALE_GEN, "gen-1 replay %d", g->stale_gen);
    }
    fl_goal(r, 4, 10, t, 1, 0);

    /* 8: B's lease ends. The router excludes it before A has applied the
     * clock; B's late advertisement is refused; the remote-only op is
     * refused once B is gone. */
    t += FL_LEASE + 1;
    fl_goal(r, 5, 11, t, 0, 0);
    {
        FL_CHECK(fab_advertise(&r->node_b, &r->own_remote, CQ_WIRE_ADVERTISE, t) == FAB_OK, "late adv");
        uint8_t msg[FAB_MSG_MAX];
        size_t len = fl_peek(r, &r->a, &r->b, FAB_MSG_ADVERTISE, msg);
        r->loop.box[0].count = 0;
        g->lease_expired = fl_deliver_a(r, msg, len, t);
        FL_CHECK(g->lease_expired == FAB_E_LEASE_EXPIRED, "late advertisement %d", g->lease_expired);
        const FabMember *m = fab_member(&r->node_a, &r->b);
        FL_CHECK(m && m->state == FAB_ST_LOST, "B lost");
        FL_CHECK(fl_fabric_entries(r, t, FX_OP_SCALE) == 0 &&
                 fl_fabric_entries(r, t, FL_OP_REMOTE_ONLY) == 0, "B's entries withdrawn");
        JsBranchRef before = rx_compose_state(&r->c);
        SrRequirement q = fl_req(FL_OP_REMOTE_ONLY);
        RxcResult o;
        g->remote_only_after_loss = rx_compose_run(&r->c, 12, &q, NULL, t, &o);
        JsBranchRef after = rx_compose_state(&r->c);
        FL_CHECK(g->remote_only_after_loss == SR_E_NO_CANDIDATE && before.id == after.id &&
                 before.gen == after.gen, "remote-only op after loss: %d", g->remote_only_after_loss);
    }

    FL_CHECK(g->fabric_held == 0, "a Fabric candidate came back held");
    FL_CHECK(g->fabric_auth_lost == 0, "a Fabric candidate lost its carried authority");
    FL_CHECK(g->remote_wins == 2, "remote wins %u", g->remote_wins);
    g->dispatched = r->disp.dispatched;
    memcpy(g->dispatch_refused, r->disp.refused, sizeof g->dispatch_refused);
    rx_compose_record_digest(&r->c.cx, g->record_digest);
    fab_state_digest(&r->node_a, g->fabric_digest);
    FL_CHECK(rx_compose_set_remote(&r->c, NULL, NULL) == RX_OK, "remove remote");
    close(ctx, &r->c);
    fl_free(r);
    g->ok = g->failures == 0;
    return g->ok ? 0 : -1;
}

#endif /* FAB_LIVING_PHASE_H */
