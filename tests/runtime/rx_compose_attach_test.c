/*
 * COMPOSITION-2 attach hygiene (rx_compose.h, attach and close in attach
 * mode), in one caller-owned World that also runs an outside reaction:
 *
 *   one per World  a second attach while one runs is refused (RX_ERR_EXISTS)
 *                  and changes nothing; the first keeps committing goals;
 *                  after close another RxCompose reattaches and recovers.
 *   close          a candidate step is held mid-run and close is called:
 *                  close revokes first and waits for the step (no cutoff);
 *                  released, the step's write is REJECTED; close returns
 *                  only after it finished; durable state and the Cortex
 *                  record count are unchanged; the RxCompose is freed right
 *                  after close (heap), so an ASan build catches any late use.
 *   inert + bound  attach/close until the World's reaction table is full:
 *                  every closed attach's reactions stay DORMANT with
 *                  unchanged activation counts while later attaches reuse
 *                  the same object slots and run goals; the attach past the
 *                  bound is refused with RX_ERR_FULL before it builds
 *                  anything and the World keeps working. The cycle count
 *                  (far above AIENOS_CAP_MAX / 13) shows close reclaims its
 *                  capability slots.
 */
#include "rx_compose_fixture.h"

#include <ftw.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int g_checks, g_fail;

#define CHECK(cond, ...) do {                                            \
        g_checks++;                                                      \
        if (!(cond)) {                                                   \
            g_fail++;                                                    \
            fprintf(stderr, "  FAIL %s:%d ", __FILE__, __LINE__);        \
            fprintf(stderr, __VA_ARGS__);                                \
            fputc('\n', stderr);                                         \
        }                                                                \
    } while (0)

#define EXT_SUBJ 100u
#define OUT_SUBJ 300u
#define RES_OUT_IN  0xD1000001ull
#define RES_OUT_OUT 0xD1000002ull

static Fx g_fx;
static RxWorld g_w;                  /* large: static */
static RxCompose g_c1, g_c2;
static AienosCapAdmin *g_admin;
static AienosCapView *g_view;
static char g_base[128];
static RxObjRef g_oin, g_oout;
static RxCapRef g_cap_ext, g_cap_rin, g_cap_wout;
static uint32_t g_rx_out;

static int rm_one(const char *p, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb; (void)flag; (void)ftw;
    return remove(p);
}
static void rmtree(const char *p) { nftw(p, rm_one, 16, FTW_DEPTH | FTW_PHYS); }
static void dir_for(char *out, size_t n, const char *name) {
    snprintf(out, n, "%s/%s", g_base, name);
    rmtree(out);
}
static int ref_eq(JsBranchRef a, JsBranchRef b) { return a.id == b.id && a.gen == b.gen; }
static void sleep_us(long us) {
    struct timespec ts = { us / 1000000, (us % 1000000) * 1000 };
    nanosleep(&ts, NULL);
}

static int mint(uint32_t subject, uint64_t resource, uint32_t rights, RxCapRef *out) {
    AienosCapRef office, ref = { UINT32_MAX, 0 };
    if (aienos_cap_office(g_admin, &office) != 0) return -1;
    AienosCapMint m = { 3u, subject, resource, rights, 0, { UINT32_MAX, 0 }, office };
    if (aienos_cap_mint(g_admin, &m, &ref) != 0) return -1;
    *out = (RxCapRef){ ref.cap_id, ref.generation };
    return 0;
}

/* The outside reaction: out.field0 = in.field0 + 1. */
static int outside_fn(RxCtx *x) {
    for (uint32_t i = 0; i < x->n_in; i++)
        if (x->in[i].obj.id == g_oin.id && x->n_out < RX_MAX_MUTATIONS)
            x->out[x->n_out++] = (RxMutation){ g_oout, 0, x->in[i].field[0] + 1 };
    return 0;
}

static int world_start(void) {
    if (aienos_cap_start(&g_admin, &g_view) != 0) return -1;
    if (rx_world_init_native(&g_w, g_view, 2, 1u << 20) != RX_OK) return -2;
    g_w.external_subject = EXT_SUBJ;
    uint64_t z[RX_MAX_FIELDS] = { 0 };
    if (rx_world_create(&g_w, 1, RX_PERSIST_RESIDENT, RES_OUT_IN, z, &g_oin) != RX_OK ||
        rx_world_create(&g_w, 1, RX_PERSIST_RESIDENT, RES_OUT_OUT, z, &g_oout) != RX_OK)
        return -3;
    if (mint(EXT_SUBJ, RES_OUT_IN, RX_RIGHT_WRITE, &g_cap_ext) ||
        mint(OUT_SUBJ, RES_OUT_IN, RX_RIGHT_READ, &g_cap_rin) ||
        mint(OUT_SUBJ, RES_OUT_OUT, RX_RIGHT_WRITE, &g_cap_wout))
        return -4;
    RxReactionDesc d;
    memset(&d, 0, sizeof d);
    d.name = "outside.copy";
    d.faculty = RX_FACULTY_OMEGA;
    d.subject = OUT_SUBJ;
    d.priority = RX_PRIO_FOREGROUND;
    d.triggers[d.n_triggers++] = (RxDep){ g_oin, RX_ALL_FIELDS };
    d.writes[d.n_writes++] = (RxDep){ g_oout, RX_ALL_FIELDS };
    d.caps[d.n_caps++] = (RxCapNeed){ g_cap_rin, RES_OUT_IN, RX_RIGHT_READ };
    d.caps[d.n_caps++] = (RxCapNeed){ g_cap_wout, RES_OUT_OUT, RX_RIGHT_WRITE };
    d.fn = outside_fn;
    return rx_world_add_reaction(&g_w, &d, &g_rx_out) == RX_OK ? 0 : -5;
}

/* Outside activity: publish v, the outside reaction answers v + 1. */
static int outside_poke(uint64_t v) {
    RxMutation m = { g_oin, 0, v };
    if (rx_world_publish_external(&g_w, g_cap_ext, &m, 1) < 0) return 0;
    if (rx_world_wait_quiescent(&g_w, 10000) != RX_OK) return 0;
    RxObject o;
    return rx_world_read(&g_w, g_oout, &o) == RX_OK && o.field[0] == v + 1;
}

static int attach(RxCompose *c, const char *dir) {
    return rx_compose_attach(c, &g_w, NULL, dir, &g_fx.self, FX_SESSION, &g_fx.router,
                             fx_contract, g_admin);
}

static uint32_t n_reactions(void) {
    pthread_mutex_lock(&g_w.mu);
    uint32_t n = g_w.n_reactions;
    pthread_mutex_unlock(&g_w.mu);
    return n;
}

/* ---- one composition per World ------------------------------------------- */

static void t_one_per_world(void) {
    printf("[*] one composition per World: second attach refused, first unharmed, reattach\n");
    char d1[256], d2[256];
    dir_for(d1, sizeof d1, "one");
    dir_for(d2, sizeof d2, "two");
    RxcResult o;
    CHECK(attach(&g_c1, d1) == RX_OK, "first attach");
    CHECK(fx_run(&g_fx, &g_c1, 5, &o) == RX_OK && o.outcome == RXC_OUT_COMMITTED &&
          o.result == 16, "first goal commits (%d, %llu)", o.outcome, (unsigned long long)o.result);
    JsBranchRef first = o.new_ref;
    uint32_t nr = n_reactions();
    CHECK(attach(&g_c2, d2) == RX_ERR_EXISTS, "second attach (other dir) refused");
    CHECK(attach(&g_c2, d1) == RX_ERR_EXISTS, "second attach (same dir) refused");
    CHECK(g_c2.world == NULL, "refused attach left its RxCompose unattached");
    CHECK(n_reactions() == nr, "refused attach registered nothing (%u vs %u)", n_reactions(), nr);
    CHECK(g_w.bind_ctx == &g_c1, "binder still the first composition's");
    CHECK(fx_run(&g_fx, &g_c1, 6, &o) == RX_OK && o.outcome == RXC_OUT_COMMITTED &&
          o.result == 19 && ref_eq(o.old_ref, first),
          "first composition still commits after the refusal (%d, %llu)", o.outcome,
          (unsigned long long)o.result);
    JsBranchRef second = o.new_ref;
    CHECK(outside_poke(41), "outside reaction runs alongside");
    rx_compose_close(&g_c1);
    CHECK(g_w.bind_check == NULL, "close removed the binder");
    CHECK(attach(&g_c2, d1) == RX_OK, "reattach after close (another RxCompose)");
    CHECK(ref_eq(rx_compose_state(&g_c2), second), "reattach recovers the last commit");
    CHECK(fx_run(&g_fx, &g_c2, 7, &o) == RX_OK && o.outcome == RXC_OUT_COMMITTED &&
          o.result == 22 && ref_eq(o.old_ref, second), "goal after reattach commits (%d)",
          o.outcome);
    rx_compose_close(&g_c2);
    CHECK(outside_poke(42), "World still works after close");
    rmtree(d1);
    rmtree(d2);
}

/* ---- close waits for the composition's own steps -------------------------- */

static int g_closed;
static void *closer(void *arg) {
    rx_compose_close(arg);
    __atomic_store_n(&g_closed, 1, __ATOMIC_RELEASE);
    return NULL;
}

static uint32_t in_flight(void) {
    pthread_mutex_lock(&g_w.mu);
    uint32_t n = g_w.in_flight;
    pthread_mutex_unlock(&g_w.mu);
    return n;
}

static void t_close_waits(void) {
    printf("[*] close: revoke first, wait for the held step, its late write refused\n");
    char d[256];
    dir_for(d, sizeof d, "hold");
    RxCompose *c = calloc(1, sizeof *c);   /* heap: freed right after close */
    if (!c) { CHECK(0, "calloc"); return; }
    RxcResult o;
    CHECK(attach(c, d) == RX_OK, "attach");
    CHECK(fx_run(&g_fx, c, 5, &o) == RX_OK && o.outcome == RXC_OUT_COMMITTED, "goal commits");
    JsBranchRef S1 = o.new_ref;

    /* Next goal straight into the World (as rx_compose_run publishes it),
     * with candidate 0 held after its Skill ran, before it proposes. */
    c->test.hold_k1 = 1;
    uint64_t sk[RXC_K] = { 0, 0 };
    for (uint32_t k = 0; k < c->n_routes && k < RXC_K; k++)
        sk[k] = ((uint64_t)c->run_route[k].skill_version << 32) | c->run_route[k].chosen.skill_id;
    uint64_t seq = ++c->seq;
    RxMutation g[5] = { { c->goal, RXC_G_INPUT, 6 }, { c->goal, RXC_G_OP, FX_OP_SCALE },
                        { c->goal, RXC_G_SKILL0, sk[0] }, { c->goal, RXC_G_SKILL1, sk[1] },
                        { c->goal, RXC_G_SEQ, seq } };
    CHECK(rx_world_publish_external(&g_w, c->cap_ext, g, 5) >= 0, "goal published");
    int waited = 0;
    while (!__atomic_load_n(&c->test.held, __ATOMIC_ACQUIRE) && waited++ < 10000) sleep_us(1000);
    CHECK(c->test.held, "candidate 0 is held mid-run");
    /* Everything else of this goal settles; only the held step is in flight. */
    for (waited = 0; in_flight() != 1 && waited < 10000; waited++) sleep_us(1000);
    CHECK(in_flight() == 1, "only the held step in flight (%u)", in_flight());

    uint32_t rid = c->rx_cand[0];
    uint64_t cx_before = c->cx.n;
    RxCapRef cand_w = c->cap_cand[0][2];
    RxObjRef cand_obj = c->cand[0];
    RxObject before_obj;
    CHECK(rx_world_read(&g_w, cand_obj, &before_obj) == RX_OK, "candidate object reads");

    pthread_t th;
    g_closed = 0;
    CHECK(pthread_create(&th, NULL, closer, c) == 0, "closer thread");
    /* close revokes first ... */
    for (waited = 0; waited < 5000 &&
         rx_world_validate_cap(&g_w, cand_w, RXC_SUBJ_CAND0, RXC_RES_CAND0, RX_RIGHT_WRITE,
                               NULL) == RX_CAP_OK; waited++)
        sleep_us(1000);
    CHECK(rx_world_validate_cap(&g_w, cand_w, RXC_SUBJ_CAND0, RXC_RES_CAND0, RX_RIGHT_WRITE,
                                NULL) != RX_CAP_OK, "close revoked the step's write right");
    /* ... and does not return while the step runs (no cutoff). */
    sleep_us(200000);
    CHECK(!__atomic_load_n(&g_closed, __ATOMIC_ACQUIRE), "close still waiting for the held step");
    CHECK(!__atomic_load_n(&c->test.hold_done, __ATOMIC_ACQUIRE), "step still held");

    __atomic_store_n(&c->test.release, 1, __ATOMIC_RELEASE);
    pthread_join(th, NULL);
    CHECK(g_closed, "close returned");
    CHECK(__atomic_load_n(&c->test.hold_done, __ATOMIC_ACQUIRE), "step finished before close returned");
    pthread_mutex_lock(&g_w.mu);
    const RxReaction *r = &g_w.reactions[rid];
    RxState st = r->state;
    uint64_t acts = r->activations;
    const RxCrumb *k = rx_world_crumb(&g_w, r->last_crumb);
    int kind = k ? (int)k->kind : -1;
    pthread_mutex_unlock(&g_w.mu);
    CHECK(st == RX_DORMANT, "held step DORMANT after close (%s)", rx_state_name(st));
    CHECK(kind == RX_CRUMB_REJECTED, "its late write was REJECTED (crumb kind %d)", kind);
    CHECK(rx_world_read(&g_w, cand_obj, &before_obj) != RX_OK, "candidate object retired");
    free(c);                                /* any later use of c is a use-after-free */

    CHECK(outside_poke(77), "World works after close (outside reaction)");
    CHECK(rx_world_wait_quiescent(&g_w, 10000) == RX_OK, "World quiet");
    pthread_mutex_lock(&g_w.mu);
    uint64_t acts_after = g_w.reactions[rid].activations;
    pthread_mutex_unlock(&g_w.mu);
    CHECK(acts_after == acts, "closed step never ran again (%llu vs %llu)",
          (unsigned long long)acts_after, (unsigned long long)acts);

    /* Durable state and the record are what they were before close. */
    CHECK(attach(&g_c1, d) == RX_OK, "reattach");
    CHECK(ref_eq(rx_compose_state(&g_c1), S1), "durable state unchanged (names S1)");
    /* Attach records the creation of its five fresh objects (the scoped
     * link replays their CREATE crumbs); nothing else may have been added:
     * the refused late write never entered the journal. */
    uint32_t created = 0;
    for (uint64_t id = cx_before + 1; id <= g_c1.cx.n; id++) {
        const CxObject *x = cx_get(&g_c1.cx, id);
        if (x && x->kind == CX_K_ENTITY_CREATED) created++;
    }
    CHECK(g_c1.cx.n == cx_before + 5 && created == 5,
          "Cortex: only the reattach's 5 creation records were added (%llu -> %llu, %u created)",
          (unsigned long long)cx_before, (unsigned long long)g_c1.cx.n, created);
    CHECK(g_c1.rolled_back == 0, "nothing to roll back");
    CHECK(fx_live_branches(&g_c1.js) == 1, "one durable branch");
    CHECK(fx_run(&g_fx, &g_c1, 7, &o) == RX_OK && o.outcome == RXC_OUT_COMMITTED &&
          ref_eq(o.old_ref, S1) && o.result == 22, "goal after reattach commits (%d)", o.outcome);
    rx_compose_close(&g_c1);
    rmtree(d);
}

/* ---- inert reactions and the reaction-table bound --------------------------- */

#define MAX_CLOSED RX_MAX_REACTIONS
static uint32_t g_closed_rid[MAX_CLOSED];
static uint64_t g_closed_acts[MAX_CLOSED];

static void t_inert_and_bound(void) {
    printf("[*] inert reactions: attach/close until the reaction table is full\n");
    char d[256];
    dir_for(d, sizeof d, "cycle");
    uint32_t base = n_reactions();
    uint32_t want = (RX_MAX_REACTIONS - base) / 4u;
    uint32_t cycles = 0, n_closed = 0, goals = 0, reused = 0, bad_goal = 0;
    RxObjRef first_goal = { UINT32_MAX, 0 };
    int rc;
    while ((rc = attach(&g_c1, d)) == RX_OK) {
        if (cycles == 0) first_goal = g_c1.goal;
        else if (g_c1.goal.id == first_goal.id && g_c1.goal.generation != first_goal.generation)
            reused++;
        /* A goal on the first cycles, then every 32nd and the last few. */
        if (cycles < 3 || cycles % 32 == 0 || cycles + 2 >= want) {
            RxcResult o;
            if (fx_run(&g_fx, &g_c1, 100 + cycles, &o) != RX_OK || o.outcome != RXC_OUT_COMMITTED)
                bad_goal++;
            goals++;
        }
        uint32_t ids[4] = { g_c1.rx_cand[0], g_c1.rx_cand[1], g_c1.rx_verify, g_c1.rx_commit };
        rx_compose_close(&g_c1);
        pthread_mutex_lock(&g_w.mu);
        for (uint32_t i = 0; i < 4 && n_closed < MAX_CLOSED; i++) {
            g_closed_rid[n_closed] = ids[i];
            g_closed_acts[n_closed++] = g_w.reactions[ids[i]].activations;
        }
        pthread_mutex_unlock(&g_w.mu);
        cycles++;
        if (cycles > RX_MAX_REACTIONS) break;   /* never: the bound must stop it */
    }
    CHECK(rc == RX_ERR_FULL, "attach past the bound refused with RX_ERR_FULL (%d)", rc);
    CHECK(cycles == want, "cycles %u = floor((%u - %u) / 4) = %u", cycles, RX_MAX_REACTIONS, base,
          want);
    CHECK(cycles > 256u / 13u, "more cycles than AIENOS slots without reclaim (%u)", cycles);
    CHECK(bad_goal == 0, "every goal committed (%u of %u failed)", bad_goal, goals);
    CHECK(reused > 0, "later attaches reused the retired object slots (%u)", reused);
    CHECK(g_c1.world == NULL && g_w.bind_check == NULL, "refused attach built nothing");
    CHECK(n_reactions() == base + 4u * cycles, "refused attach registered nothing (%u)",
          n_reactions());
    CHECK(outside_poke(1234), "World works after the refusal");
    CHECK(outside_poke(1235), "World works after the refusal (again)");
    uint32_t ran = 0, awake = 0;
    pthread_mutex_lock(&g_w.mu);
    for (uint32_t i = 0; i < n_closed; i++) {
        const RxReaction *r = &g_w.reactions[g_closed_rid[i]];
        if (r->activations != g_closed_acts[i]) ran++;
        if (r->state != RX_DORMANT || r->rearm || r->parked || r->deferred) awake++;
    }
    pthread_mutex_unlock(&g_w.mu);
    CHECK(ran == 0, "no closed reaction ran again (%u of %u did)", ran, n_closed);
    CHECK(awake == 0, "every closed reaction DORMANT (%u not)", awake);
    printf("    %u attach/close cycles (base %u reactions), %u goals, %u inert reactions\n",
           cycles, base, goals, n_closed);
    rmtree(d);
}

int main(void) {
    snprintf(g_base, sizeof g_base, "/tmp/rx_compose_attach_test.XXXXXX");
    if (!mkdtemp(g_base)) { perror("mkdtemp"); return 1; }
    if (fx_init(&g_fx) != 0) { fprintf(stderr, "fixture init failed\n"); return 1; }
    int rc = world_start();
    if (rc != 0) { fprintf(stderr, "world start failed (%d)\n", rc); return 1; }
    t_one_per_world();
    t_close_waits();
    t_inert_and_bound();
    rx_world_destroy(&g_w);
    aienos_cap_stop(g_admin, g_view);
    fx_free(&g_fx);
    rmtree(g_base);
    printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
