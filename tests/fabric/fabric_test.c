/*
 * fabric_test.c -- F5-0 exit gate (fabric.h) over the loopback transport.
 *
 * Three enrolled machines A, B, C join, advertise one capability each (B two),
 * renew leases; C goes silent, its lease ends, A and B mark it LOST and its
 * Capability Graph entries are withdrawn. Negatives: expired lease, stale
 * generation, forged identity (wrong key, altered bytes, unenrolled sender),
 * replayed advertisement, machine mismatch (relayed record, misdelivery),
 * after-leave. Routing never mints authority: every Fabric candidate comes
 * back not held, and require_held filters all of them. The whole scenario
 * runs twice from scratch; transcripts and state digests must match.
 *
 * Prints one line per check on failure, then the verdict and the digests.
 */
#include "fabric.h"
#include "fab_hmac.h"
#include "fab_loopback.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); } } while (0)

static void hex(const uint8_t *b, size_t n, char *out) {
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = h[b[i] >> 4]; out[2 * i + 1] = h[b[i] & 15]; }
    out[2 * n] = 0;
}

static int hex_eq(const uint8_t *b, const char *want) {
    char s[65];
    hex(b, 32, s);
    return strcmp(s, want) == 0;
}

/* ---- HMAC-SHA256 against RFC 4231 ---- */
static void test_hmac(void) {
    uint8_t k1[20], t[32];
    memset(k1, 0x0b, sizeof k1);
    fab_hmac_sha256(k1, sizeof k1, (const uint8_t *)"Hi There", 8, t);
    CHECK(hex_eq(t, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"), "RFC 4231 case 1");
    fab_hmac_sha256((const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28, t);
    CHECK(hex_eq(t, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"), "RFC 4231 case 2");
    uint8_t k6[131];
    memset(k6, 0xaa, sizeof k6);
    const char *m6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    fab_hmac_sha256(k6, sizeof k6, (const uint8_t *)m6, strlen(m6), t);
    CHECK(hex_eq(t, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"), "RFC 4231 case 6");
}

/* ---- the simulated house ---- */

#define NM 4                         /* A, B, C enrolled; D is not */
enum { A = 0, B = 1, C = 2, D = 3 };
#define MS 1000ull
#define LEASE (1000 * MS)

typedef struct {
    AienMachineId id[NM];
    uint8_t key[NM][32];
    AienMachineId roster_ids[3];
    FabRoster roster;
    FabLoop loop;
    AienMachineId slots[NM][8];
    AienMachineIndex index[NM];
    CqCatalog cat[3];
    FabHmacAuth auth[3];
    FabNode node[3];
    CqKey own[3][2];
} House;

static void house_init(House *h) {
    memset(h, 0, sizeof *h);
    for (int i = 0; i < NM; i++) {
        char root[16];
        int n = snprintf(root, sizeof root, "machine-%c", 'A' + i);
        CHECK(aien_mid_derive(AIEN_MID_ROOT_PROVISIONED, (const uint8_t *)root, (size_t)n, &h->id[i]) == AIEN_MID_OK, "derive");
        char kr[16];
        n = snprintf(kr, sizeof kr, "fab-key-%c", 'A' + i);
        sha256_hash((const uint8_t *)kr, (size_t)n, h->key[i]);
    }
    for (int i = 0; i < 3; i++) h->roster_ids[i] = h->id[i];
    h->roster.n = 3;
    h->roster.ids = h->roster_ids;
    fab_loop_init(&h->loop);
    for (int i = 0; i < NM; i++) CHECK(fab_loop_add(&h->loop, &h->id[i]) == FAB_OK, "loop add");
    for (int i = 0; i < 3; i++) {
        aien_mid_index_init(&h->index[i], h->slots[i], 8);
        CqCatalog *c = &h->cat[i];
        CHECK(cq_catalog_init_canonical(c, &h->index[i], &h->id[i], 8, 16) == CQ_OK, "catalog");
        uint32_t dom = CQ_SRC(CQ_SRC_GRAPH) | CQ_SRC(CQ_SRC_FABRIC);
        CHECK(cq_op_define(c, 1, 0, dom) == CQ_OK && cq_op_define(c, 2, 1, dom) == CQ_OK, "ops");
        /* Verifiers hold the roster's keys (HMAC stand-in, see fab_loopback.h). */
        fab_hmac_auth_init(&h->auth[i], &h->id[i], h->key[i], 3, h->roster_ids,
                           (const uint8_t (*)[32])h->key);
        FabConfig cfg = { h->id[i], &h->roster, c, &h->loop.transport, &h->auth[i].auth, LEASE, 1 };
        CHECK(fab_node_init(&h->node[i], &cfg) == FAB_OK, "node init");
    }
}

static void house_free(House *h) {
    for (int i = 0; i < 3; i++) cq_catalog_free(&h->cat[i]);
}

/* Deliver everything waiting, nodes in fixed order. Returns refusals seen. */
static int pump(House *h, uint64_t now) {
    int refused = 0;
    for (int round = 0; round < 64 && fab_loop_pending(&h->loop); round++)
        for (int i = 0; i < 3; i++) {
            FabVerdict v;
            while (fab_poll(&h->node[i], now, &v) == 1)
                if (v.code != FAB_OK) refused++;
        }
    return refused;
}

/* Deliver one injected message to node i and return its verdict. */
static FabVerdict deliver(House *h, int i, const uint8_t *msg, size_t len, uint64_t now) {
    FabVerdict v;
    memset(&v, 0, sizeof v);
    CHECK(fab_loop_inject(&h->loop, &h->id[i], msg, len) == FAB_OK, "inject");
    CHECK(fab_poll(&h->node[i], now, &v) == 1, "poll injected");
    return v;
}

static void register_own(House *h, int i, int slot, uint32_t cap, uint32_t rev) {
    CqEntry e;
    memset(&e, 0, sizeof e);
    e.capability_id = cap;
    e.realization_id = 1;
    e.machine_id = h->cat[i].self_machine;
    e.op = 2;
    e.source = CQ_SRC_GRAPH;
    e.evidence_level = CQ_EV_DECLARED;
    e.effects = CQ_FX_PURE;
    e.auth_resource = 0x5000u + cap;     /* needs authority on its own machine */
    e.auth_rights = 1;
    e.confidence_ppm = e.reliability_ppm = 900000;
    e.cost = 10 + cap;
    e.latency_us = 100;
    e.energy_uj = 50;
    e.live = CQ_LIVE_AVAILABLE;
    e.generation = fab_entry_generation(&h->node[i], rev);
    CHECK(cq_register(&h->cat[i], &e, NULL, 0) == CQ_OK, "register own %u", cap);
    CHECK(cq_catalog_build(&h->cat[i]) == CQ_OK, "build");
    h->own[i][slot] = cq_key_of(&e);
}

typedef struct { uint32_t n, local, fabric, held, from[3]; } QView;

static QView query(House *h, int i, uint64_t now, int require_held) {
    QView q;
    memset(&q, 0, sizeof q);
    CqNeed need;
    memset(&need, 0, sizeof need);
    need.semantic_operation = 2;
    need.accepted_input_types = ~0ull;
    need.effect_class = CQ_FX_PURE;
    need.authority_ceiling.resource_lo = 0;
    need.authority_ceiling.resource_hi = ~0ull;
    need.authority_ceiling.rights = ~0u;
    need.locality_constraints.allowed = CQ_LOC_LOCAL | CQ_LOC_FABRIC;
    CqTradeoffs t;
    memset(&t, 0, sizeof t);
    t.n_order = 1;
    t.order[0] = CQ_DIM_COST;
    t.k = CQ_MAX_K;
    t.include_dominated = 1;
    t.require_held = (uint32_t)require_held;
    CqPlan p;
    CqResult r;
    CHECK(cq_compile(&h->cat[i], &need, &p) == CQ_OK, "compile");
    CHECK(cq_query(&h->cat[i], &p, &need, &t, NULL, now, &r, NULL) == CQ_OK, "query");
    q.n = r.n;
    for (uint32_t k = 0; k < r.n; k++) {
        const CqCandidate *c = &r.cand[k];
        if (c->local) q.local++;
        if (c->source == CQ_SRC_FABRIC) q.fabric++;
        if (c->required_authority.held) q.held++;
        AienMachineId m;
        if (cq_machine_identity(&h->cat[i], c->machine_id, &m) == CQ_OK)
            for (int j = 0; j < 3; j++) if (aien_mid_equal(&m, &h->id[j])) q.from[j]++;
    }
    return q;
}

/* Copy the first waiting message in node i's mailbox from `from` of `kind`. */
static size_t peek(House *h, int i, int from, uint32_t kind, uint8_t *out) {
    const FabLoopBox *b = &h->loop.box[i];
    uint8_t rec[AIEN_MID_RECORD_BYTES];
    aien_mid_encode(&h->id[from], rec);
    for (uint32_t k = 0; k < b->count; k++) {
        const FabLoopMsg *m = &b->q[(b->head + k) % FAB_LOOP_DEPTH];
        if (m->b[5] == kind && memcmp(m->b + 8, rec, sizeof rec) == 0) {
            memcpy(out, m->b, m->len);
            return m->len;
        }
    }
    return 0;
}

static void reseal(uint8_t *msg, size_t len, const uint8_t key[32]) {
    fab_hmac_sha256(key, 32, msg, len - FAB_TAG_BYTES, msg + len - FAB_TAG_BYTES);
}

static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/* The scenario. out = transcript digest || A, B, C state digests. */
static void scenario(uint8_t out[4][32]) {
    static House hh;
    House *h = &hh;
    house_init(h);
    uint8_t join_c1[FAB_MSG_MAX], adv_b[FAB_MSG_MAX];
    size_t join_c1_len, adv_b_len;

    /* t=0: everyone joins. Keep C's generation-1 JOIN to A for later. */
    for (int i = 0; i < 3; i++) CHECK(fab_join(&h->node[i], LEASE, 0) == FAB_OK, "join %d", i);
    join_c1_len = peek(h, A, C, FAB_MSG_JOIN, join_c1);
    CHECK(join_c1_len == FAB_HDR_BYTES + FAB_JOIN_BODY + FAB_TAG_BYTES, "captured C join");
    CHECK(pump(h, 0) == 0, "joins accepted");
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            if (i != j) CHECK(fab_member_live(&h->node[i], &h->id[j], 0), "%d sees %d", i, j);

    /* t=10ms: advertise. B has two capabilities. Keep B's ADVERTISE to A. */
    register_own(h, A, 0, 101, 1);
    register_own(h, B, 0, 201, 1);
    register_own(h, B, 1, 202, 1);
    register_own(h, C, 0, 301, 1);
    CHECK(fab_advertise(&h->node[A], &h->own[A][0], CQ_WIRE_ADVERTISE, 10 * MS) == FAB_OK, "adv A");
    CHECK(fab_advertise(&h->node[B], &h->own[B][0], CQ_WIRE_ADVERTISE, 10 * MS) == FAB_OK, "adv B");
    adv_b_len = peek(h, A, B, FAB_MSG_ADVERTISE, adv_b);
    CHECK(adv_b_len == FAB_MSG_MAX, "captured B advertise");
    CHECK(fab_advertise(&h->node[B], &h->own[B][1], CQ_WIRE_ADVERTISE, 10 * MS) == FAB_OK, "adv B2");
    CHECK(fab_advertise(&h->node[C], &h->own[C][0], CQ_WIRE_ADVERTISE, 10 * MS) == FAB_OK, "adv C");
    CHECK(pump(h, 10 * MS) == 0, "advertisements accepted");
    QView q = query(h, A, 10 * MS, 0);
    CHECK(q.n == 4 && q.local == 1 && q.fabric == 3 && q.from[B] == 2 && q.from[C] == 1,
          "A sees 1 local + 3 Fabric (n=%u local=%u fabric=%u)", q.n, q.local, q.fabric);
    /* Routing never mints authority. */
    CHECK(q.held == 0, "no candidate held without the World's authority view");
    CHECK(query(h, A, 10 * MS, 1).n == 0, "require_held keeps none: Fabric granted nothing");
    q = query(h, C, 10 * MS, 0);
    CHECK(q.n == 4 && q.from[A] == 1 && q.from[B] == 2, "C sees A and B");

    /* t=500ms: A and B renew; C has gone silent (a lost machine). */
    fab_loop_silence(&h->loop, &h->id[C], 1);
    CHECK(fab_renew(&h->node[A], LEASE, 500 * MS) == FAB_OK, "renew A");
    CHECK(fab_renew(&h->node[B], LEASE, 500 * MS) == FAB_OK, "renew B");
    CHECK(fab_renew(&h->node[C], LEASE, 500 * MS) == FAB_OK, "renew C (dropped)");
    CHECK(pump(h, 500 * MS) == 0, "renewals accepted");

    /* t=1.2s: C's lease (granted at 0 for 1s) has ended. */
    uint64_t t1 = 1200 * MS;
    CHECK(fab_tick(&h->node[A], t1) == 1 && fab_tick(&h->node[B], t1) == 1, "A and B detect one loss");
    CHECK(fab_member(&h->node[A], &h->id[C])->state == FAB_ST_LOST, "C lost in A's view");
    CHECK(fab_member_live(&h->node[A], &h->id[B], t1), "B still live");
    CqKey kc = h->own[C][0];
    kc.machine_id = cq_machine_index(&h->cat[A], &h->id[C]);
    CHECK(cq_lookup(&h->cat[A], &kc) && cq_lookup(&h->cat[A], &kc)->live == CQ_LIVE_WITHDRAWN,
          "C's entry withdrawn in A's graph");
    q = query(h, A, t1, 0);
    CHECK(q.n == 3 && q.from[C] == 0 && q.from[B] == 2, "A's query no longer returns C");
    JsHome home;
    CHECK(fab_home(&h->node[A], &h->id[C], t1, &home) == FAB_E_NOT_MEMBER, "no J-Space home on lost C");
    CHECK(fab_home(&h->node[A], &h->id[B], t1, &home) == FAB_OK && home.locality == JS_HOME_REMOTE_OWNED &&
          memcmp(home.machine, h->id[B].id, 32) == 0, "B home REMOTE_OWNED by AienMachineId");
    CHECK(fab_home(&h->node[A], &h->id[A], t1, &home) == FAB_OK && home.locality == JS_HOME_LOCAL, "A home LOCAL");
    /* Partition sovereignty (aienos ADR 0010 inv. 5): loss is the receivers' view;
     * C's own graph still offers its own capability locally. */
    CHECK(query(h, C, t1, 0).local == 1, "C keeps its own capability while cut off");

    /* Negative: expired lease. C comes back on the same generation. */
    fab_loop_silence(&h->loop, &h->id[C], 0);
    CHECK(fab_renew(&h->node[C], LEASE, t1) == FAB_OK, "C renew sent");
    uint8_t m[FAB_MSG_MAX];
    size_t ml = peek(h, A, C, FAB_MSG_RENEW, m);
    FabVerdict v;
    CHECK(ml > 0, "C renew queued");
    pump(h, t1);
    CHECK(h->node[A].counts[-FAB_E_LEASE_EXPIRED] == 1 && h->node[B].counts[-FAB_E_LEASE_EXPIRED] == 1,
          "expired-lease renew refused by A and B");
    CHECK(fab_advertise(&h->node[C], &h->own[C][0], CQ_WIRE_ADVERTISE, t1) == FAB_OK, "C adv sent");
    pump(h, t1);
    CHECK(h->node[A].counts[-FAB_E_LEASE_EXPIRED] == 2, "expired-lease advertise refused");
    CHECK(query(h, A, t1, 0).from[C] == 0, "still nothing from C");

    /* Negative: stale generation. C restarts with generation 2 and rejoins;
     * its old generation-1 JOIN is then refused. */
    CHECK(fab_node_set_generation(&h->node[C], 1) == FAB_E_STALE_GEN, "generation cannot go back");
    CHECK(fab_node_set_generation(&h->node[C], 2) == FAB_OK, "C generation 2");
    CHECK(fab_join(&h->node[C], LEASE, t1) == FAB_OK, "C rejoin");
    CHECK(pump(h, t1) == 0, "rejoin accepted");
    CHECK(fab_member_live(&h->node[A], &h->id[C], t1), "C live again");
    v = deliver(h, A, join_c1, join_c1_len, t1);
    CHECK(v.code == FAB_E_STALE_GEN, "stale-generation JOIN refused (%s)", fab_strerror(v.code));
    CHECK(fab_member(&h->node[A], &h->id[C])->generation == 2, "membership did not roll back");
    /* Re-advertise under generation 2: the withdrawn entry comes back. */
    register_own(h, C, 0, 301, 1);
    CHECK(fab_advertise(&h->node[C], &h->own[C][0], CQ_WIRE_ADVERTISE, t1) == FAB_OK, "C re-adv");
    CHECK(pump(h, t1) == 0, "C re-advertise accepted");
    CHECK(query(h, A, t1, 0).from[C] == 1, "C's capability back after rejoin");

    /* Negative: replayed advertisement (B's first ADVERTISE to A, again). */
    v = deliver(h, A, adv_b, adv_b_len, t1);
    CHECK(v.code == FAB_E_REPLAY, "replayed advertisement refused (%s)", fab_strerror(v.code));

    /* Negative: forged identity. C claims to be B (B's message, fresh seq,
     * tag made with C's key); a flipped byte; an unenrolled machine D. */
    memcpy(m, adv_b, adv_b_len);
    put64(m + 104, 1000000);
    reseal(m, adv_b_len, h->key[C]);
    v = deliver(h, A, m, adv_b_len, t1);
    CHECK(v.code == FAB_E_AUTH, "C forging B refused (%s)", fab_strerror(v.code));
    memcpy(m, adv_b, adv_b_len);
    put64(m + 104, 1000000);
    reseal(m, adv_b_len, h->key[B]);
    m[FAB_HDR_BYTES + 140] ^= 1;          /* reliability_ppm inside the record */
    v = deliver(h, A, m, adv_b_len, t1);
    CHECK(v.code == FAB_E_AUTH, "altered bytes refused (%s)", fab_strerror(v.code));
    memcpy(m, adv_b, adv_b_len);
    aien_mid_encode(&h->id[D], m + 8);
    reseal(m, adv_b_len, h->key[D]);
    v = deliver(h, A, m, adv_b_len, t1);
    CHECK(v.code == FAB_E_NOT_ENROLLED, "unenrolled D refused (%s)", fab_strerror(v.code));

    /* Negative: machine mismatch. B relays C's record under its own name;
     * a message for A delivered to C. */
    uint8_t crec[CQ_WIRE_BYTES];
    CHECK(cq_wire_encode(&h->cat[C], cq_lookup(&h->cat[C], &h->own[C][0]), CQ_WIRE_ADVERTISE, crec) == CQ_OK,
          "C record");
    CHECK(fab_seal(&h->node[B], &h->id[A], FAB_MSG_ADVERTISE, crec, sizeof crec, t1, m, &ml) == FAB_OK, "seal");
    v = deliver(h, A, m, ml, t1);
    CHECK(v.code == FAB_E_MISMATCH, "relayed record refused (%s)", fab_strerror(v.code));
    uint8_t rb[FAB_RENEW_BODY];
    put64(rb, LEASE);
    CHECK(fab_seal(&h->node[B], &h->id[A], FAB_MSG_RENEW, rb, sizeof rb, t1, m, &ml) == FAB_OK, "seal");
    v = deliver(h, C, m, ml, t1);
    CHECK(v.code == FAB_E_MISMATCH, "misdelivered message refused (%s)", fab_strerror(v.code));
    /* The same message is still good where it was addressed. */
    v = deliver(h, A, m, ml, t1);
    CHECK(v.code == FAB_OK, "addressed copy accepted (%s)", fab_strerror(v.code));

    /* Refusals changed nothing: A's view is still B x2, C x1, A x1. */
    q = query(h, A, t1, 0);
    CHECK(q.n == 4 && q.from[B] == 2 && q.from[C] == 1 && q.held == 0, "graph unchanged by refusals");

    /* An owner withdraws one of its own entries over ADVERTISE (capq kind WITHDRAW). */
    register_own(h, B, 1, 202, 2);
    CHECK(fab_advertise(&h->node[B], &h->own[B][1], CQ_WIRE_WITHDRAW, t1) == FAB_OK, "B withdraw sent");
    CHECK(pump(h, t1) == 0, "owner withdraw accepted");
    CHECK(query(h, A, t1, 0).from[B] == 1, "B's withdrawn entry gone from A");

    /* B leaves: its entries go; a later message is not a member's. */
    CHECK(fab_leave(&h->node[B], t1 + MS) == FAB_OK, "B leave");
    CHECK(pump(h, t1 + MS) == 0, "leave accepted");
    CHECK(fab_member(&h->node[A], &h->id[B])->state == FAB_ST_LEFT, "B left");
    CHECK(query(h, A, t1 + MS, 0).from[B] == 0, "B's capabilities withdrawn");
    CHECK(fab_renew(&h->node[B], LEASE, t1 + 2 * MS) == FAB_OK, "B renew sent");
    pump(h, t1 + 2 * MS);
    CHECK(h->node[A].counts[-FAB_E_NOT_MEMBER] == 1, "after-leave renew refused");

    /* A lease whose end would pass UINT64_MAX is refused before anything changes. */
    uint64_t tmax = UINT64_MAX - 10;
    CHECK(fab_node_set_generation(&h->node[C], 3) == FAB_OK, "C generation 3");
    CHECK(fab_seal(&h->node[C], &h->id[A], FAB_MSG_JOIN, NULL, 0, tmax, m, &ml) == FAB_E_ARG, "join needs a body");
    {
        uint8_t jb[FAB_JOIN_BODY];
        cq_ontology_digest(&h->cat[C], jb);
        put64(jb + 32, LEASE);
        CHECK(fab_seal(&h->node[C], &h->id[A], FAB_MSG_JOIN, jb, sizeof jb, tmax, m, &ml) == FAB_OK, "seal join");
    }
    v = deliver(h, A, m, ml, tmax);
    CHECK(v.code == FAB_E_FORMAT && fab_member(&h->node[A], &h->id[C])->generation == 2,
          "overflowing lease refused, membership unchanged (%s)", fab_strerror(v.code));

    fab_loop_transcript(&h->loop, out[0]);
    for (int i = 0; i < 3; i++) fab_state_digest(&h->node[i], out[1 + i]);
    house_free(h);
}

int main(void) {
    test_hmac();
    uint8_t r1[4][32], r2[4][32];
    scenario(r1);
    unsigned first_checks = checks;
    scenario(r2);
    CHECK(memcmp(r1, r2, sizeof r1) == 0, "deterministic replay: transcripts and state digests match");
    uint8_t all[32];
    sha256_hash(&r1[0][0], sizeof r1, all);
    char s[65];
    printf("checks %u (scenario %u per run), failures %u\n", checks, first_checks, failures);
    hex(r1[0], 32, s); printf("transcript %s\n", s);
    for (int i = 0; i < 3; i++) { hex(r1[1 + i], 32, s); printf("state_%c %s\n", 'A' + i, s); }
    hex(all, 32, s); printf("replay_digest %s\n", s);
    puts(failures ? "F5_0_FABRIC_LOOPBACK_FAIL" : "F5_0_FABRIC_LOOPBACK_PASS");
    return failures ? 1 : 0;
}
