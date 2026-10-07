/*
 * COMPOSITION-2 attach hygiene (rx_compose.h, attach and close in attach
 * mode), in one caller-owned World that also runs an outside reaction:
 *
 *   two per World  two compositions attach to one World as instances 0 and 1
 *                  and run goals at the same time; each keeps the winner it
 *                  verified (its own state, results, journal); a step under
 *                  one instance's subject cannot write the other's objects
 *                  with its own or the other's capabilities; a third attach,
 *                  or a second one on a directory in use, is refused
 *                  (RX_ERR_EXISTS) and changes nothing; after one closes, a
 *                  new attach takes its instance and reuses its slots.
 *   close          a candidate step is held mid-run and close is called:
 *                  close revokes first and waits for the step (no cutoff);
 *                  released, the step's write is REJECTED; close returns
 *                  only after it finished; durable state and the Cortex
 *                  record count are unchanged; the RxCompose is freed right
 *                  after close (heap), so an ASan build catches any late use.
 *   reclaim        2000 attach/close rounds (a goal on every 50th): after every round the
 *                  World's footprint (rx_world_footprint) is what it was
 *                  before the round, the reaction table does not grow after
 *                  the first round, and every attach gets the same 13
 *                  AIENOS capability slots back (close reclaimed them).
 *   full           a World with no room for four more reactions refuses the
 *                  attach (RX_ERR_FULL) before building anything.
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
static RxCompose g_c1, g_c2, g_c3;
static RxWorld g_w2;                 /* the full-table World */
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


static void footprint(RxFootprint *fp) { rx_world_footprint(&g_w, fp); }

/* Every counter of the footprint equal; `slots` 0 skips the reaction-table
 * size (the first attach of an instance grows it once, by design). */
static int fp_same(const RxFootprint *a, const RxFootprint *b, int slots) {
    return (!slots || (a->reaction_slots == b->reaction_slots &&
                       a->reactions_removed == b->reactions_removed)) &&
           a->reactions_active == b->reactions_active && a->subscriptions == b->subscriptions &&
           a->objects_live == b->objects_live && a->binders == b->binders &&
           a->bound_fields == b->bound_fields && a->in_flight == b->in_flight &&
           a->fanout_backlog == b->fanout_backlog && a->deferred == b->deferred &&
           a->used_slots == b->used_slots && a->used_memory == b->used_memory &&
           a->used_energy == b->used_energy;
}
static void fp_print(const char *tag, const RxFootprint *f) {
    fprintf(stderr, "    %s: slots %u active %u removed %u subs %llu objs %u binders %u bound %u "
            "inflight %u backlog %u deferred %u used %u/%llu/%llu\n", tag, f->reaction_slots,
            f->reactions_active, f->reactions_removed, (unsigned long long)f->subscriptions,
            f->objects_live, f->binders, f->bound_fields, f->in_flight, f->fanout_backlog,
            f->deferred, f->used_slots, (unsigned long long)f->used_memory,
            (unsigned long long)f->used_energy);
}

/* ---- two compositions per World ------------------------------------------ */

typedef struct {
    RxCompose *c;
    uint64_t base;
    uint32_t n, ok, chain_ok;
    JsBranchRef last;
} Runner;

/* n goals in a row on one composition; each must commit input*3+1 and build
 * on the branch the previous goal committed. */
static void *runner(void *arg) {
    Runner *r = arg;
    for (uint32_t i = 0; i < r->n; i++) {
        RxcResult o;
        if (fx_run(&g_fx, r->c, r->base + i, &o) == RX_OK && o.outcome == RXC_OUT_COMMITTED &&
            o.result == (r->base + i) * 3 + 1 && o.winner < RXC_K)
            r->ok++;
        if (ref_eq(o.old_ref, r->last)) r->chain_ok++;
        r->last = o.new_ref;
    }
    return NULL;
}

/* A rogue step: triggered by the outside object, writes field `field` of
 * `target`, as `subject`, holding `cap` claimed on `resource`. */
static uint64_t g_rogue_value;
static RxObjRef g_rogue_target;
static uint32_t g_rogue_field;
static int rogue_fn(RxCtx *x) {
    if (x->n_out < RX_MAX_MUTATIONS)
        x->out[x->n_out++] = (RxMutation){ g_rogue_target, g_rogue_field, g_rogue_value };
    return 0;
}

/* RX_OK if the rogue's write was refused (at registration or at run) and the
 * target field kept its value; otherwise a description in *why. */
static int rogue_refused(uint32_t subject, RxCapRef cap, uint64_t resource, RxObjRef target,
                         uint32_t field, const char **why) {
    static char buf[96];
    RxObject before, after;
    if (rx_world_read(&g_w, target, &before) != RX_OK) { *why = "target unreadable"; return -1; }
    /* The rogue's trigger read is honest (its own READ right on the outside
     * object), so the only authority in question is the write. */
    RxCapRef rd;
    if (mint(subject, RES_OUT_IN, RX_RIGHT_READ, &rd)) { *why = "mint read right"; return -1; }
    RxReactionDesc d;
    memset(&d, 0, sizeof d);
    d.name = "compose.cross.rogue";
    d.faculty = RX_FACULTY_OMEGA;
    d.subject = subject;
    d.priority = RX_PRIO_FOREGROUND;
    d.triggers[d.n_triggers++] = (RxDep){ g_oin, RX_ALL_FIELDS };
    d.writes[d.n_writes++] = (RxDep){ target, RX_FIELD(field) };
    d.caps[d.n_caps++] = (RxCapNeed){ rd, RES_OUT_IN, RX_RIGHT_READ };
    d.caps[d.n_caps++] = (RxCapNeed){ cap, resource, RX_RIGHT_WRITE };
    d.fn = rogue_fn;
    g_rogue_target = target;
    g_rogue_field = field;
    g_rogue_value = before.field[field] ^ 0x5A5Aull;
    uint32_t rid;
    int rc = rx_world_add_reaction(&g_w, &d, &rid), kind = 0, reason = 0;
    if (rc == RX_OK) {
        static uint64_t poke = 9000;         /* a new value each time: a change wakes */
        (void)outside_poke(poke++);
        pthread_mutex_lock(&g_w.mu);
        const RxCrumb *k = rx_world_crumb(&g_w, g_w.reactions[rid].last_crumb);
        kind = k ? (int)k->kind : -1;
        reason = k ? (int)k->reason : 0;
        pthread_mutex_unlock(&g_w.mu);
        while (rx_world_remove_reaction(&g_w, rid) == RX_ERR_BUSY) sleep_us(1000);
    }
    AienosCapRef office;
    if (aienos_cap_office(g_admin, &office) == 0) {
        (void)aienos_cap_revoke(g_admin, office, (AienosCapRef){ rd.cap_id, rd.generation });
        (void)aienos_cap_reclaim(g_admin, office, rd.cap_id);
    }
    if (rx_world_read(&g_w, target, &after) != RX_OK) { *why = "target gone"; return -1; }
    if (after.field[field] != before.field[field]) { *why = "target field changed"; return -1; }
    if (rc != RX_OK) {
        snprintf(buf, sizeof buf, "refused at registration (%d)", rc);
        printf("    rogue as subject %u: %s\n", subject, buf);
        *why = buf;
        return RX_OK;
    }
    snprintf(buf, sizeof buf, "crumb kind %d reason %d", kind, reason);
    printf("    rogue as subject %u: %s\n", subject, buf);
    *why = buf;
    return kind == RX_CRUMB_BLOCKED_AUTHORITY || kind == RX_CRUMB_REJECTED ? RX_OK : -1;
}

static void t_two_per_world(void) {
    printf("[*] two compositions per World: isolated, concurrent, third refused\n");
    char d1[256], d2[256], d3[256];
    dir_for(d1, sizeof d1, "one");
    dir_for(d2, sizeof d2, "two");
    dir_for(d3, sizeof d3, "three");
    RxFootprint fp0, fp1, fp2;
    footprint(&fp0);
    RxcResult o;
    CHECK(attach(&g_c1, d1) == RX_OK, "first attach");
    CHECK(attach(&g_c2, d2) == RX_OK, "second attach (other dir) runs alongside");
    CHECK(g_c1.inst == 0 && g_c2.inst == 1, "instances 0 and 1 (%u, %u)", g_c1.inst, g_c2.inst);
    CHECK(g_c1.subj_commit == RXC_SUBJ_COMMIT && g_c2.subj_commit == RXC_SUBJ_COMMIT + 8u &&
          g_c2.res[4] == RXC_RES_STATE + 0x100u, "instance 1 has its own subjects and resources");
    footprint(&fp1);
    CHECK(fp1.binders == 2 && fp1.bound_fields == 2 && fp1.reactions_active == fp0.reactions_active + 8 &&
          fp1.objects_live == fp0.objects_live + 10, "two binders, 8 reactions, 10 objects");
    CHECK(attach(&g_c3, d3) == RX_ERR_EXISTS, "third attach refused");
    CHECK(attach(&g_c3, d1) == RX_ERR_EXISTS, "attach on a directory in use refused");
    CHECK(g_c3.world == NULL, "refused attach left its RxCompose unattached");
    footprint(&fp2);
    CHECK(fp_same(&fp1, &fp2, 1), "refused attaches changed nothing");

    /* Goals at the same time on both; the outside reaction runs too. */
    Runner ra = { &g_c1, 1000, 40, 0, 0, rx_compose_state(&g_c1) };
    Runner rb = { &g_c2, 5000, 40, 0, 0, rx_compose_state(&g_c2) };
    pthread_t ta, tb;
    CHECK(pthread_create(&ta, NULL, runner, &ra) == 0 && pthread_create(&tb, NULL, runner, &rb) == 0,
          "runner threads");
    uint32_t pokes = 0;
    for (uint32_t i = 0; i < 20; i++) pokes += (uint32_t)outside_poke(600 + i);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    CHECK(pokes == 20, "outside reaction answered during both runs (%u/20)", pokes);
    CHECK(ra.ok == ra.n && rb.ok == rb.n, "every concurrent goal committed its own result (%u, %u)",
          ra.ok, rb.ok);
    CHECK(ra.chain_ok == ra.n && rb.chain_ok == rb.n,
          "each goal built on its own composition's last commit (%u, %u)", ra.chain_ok, rb.chain_ok);
    CHECK(ref_eq(rx_compose_state(&g_c1), ra.last) && ref_eq(rx_compose_state(&g_c2), rb.last),
          "each composition keeps the winner it verified");
    RxObject s1, s2;
    CHECK(rx_world_read(&g_w, g_c1.state, &s1) == RX_OK && rx_world_read(&g_w, g_c2.state, &s2) == RX_OK &&
          s1.field[RXC_S_REF] == fx_pack(ra.last) && s2.field[RXC_S_REF] == fx_pack(rb.last),
          "state objects name their own branch");
    CHECK(fx_count(&g_c1.cx, CX_K_PROMOTION, UINT64_MAX) == ra.n && fx_count(&g_c2.cx, CX_K_PROMOTION, UINT64_MAX) == rb.n,
          "each journal holds only its own promotions (%u, %u)",
          fx_count(&g_c1.cx, CX_K_PROMOTION, UINT64_MAX), fx_count(&g_c2.cx, CX_K_PROMOTION, UINT64_MAX));

    /* Cross-talk: instance 1's steps against instance 0's objects. */
    const char *why = "";
    CHECK(rogue_refused(g_c2.subj_cand[0], g_c2.cap_cand[0][2], g_c2.res[1], g_c1.cand[0], RXC_C_REF, &why) == RX_OK,
          "B candidate with its own right on its own resource cannot write A's candidate: %s", why);
    CHECK(rogue_refused(g_c2.subj_cand[0], g_c2.cap_cand[0][2], g_c1.res[1], g_c1.cand[0], RXC_C_REF, &why) == RX_OK,
          "B candidate claiming A's resource with its own right refused: %s", why);
    CHECK(rogue_refused(g_c2.subj_cand[0], g_c1.cap_cand[0][2], g_c1.res[1], g_c1.cand[0], RXC_C_REF, &why) == RX_OK,
          "B candidate presenting A's capability refused: %s", why);
    CHECK(rogue_refused(g_c2.subj_commit, g_c1.cap_commit[1], g_c1.res[4], g_c1.state, RXC_S_REF, &why) == RX_OK,
          "B commit presenting A's commit capability cannot move A's state: %s", why);
    CHECK(rogue_refused(g_c1.subj_commit, g_c1.cap_commit[1], g_c2.res[4], g_c2.state, RXC_S_REF, &why) == RX_OK,
          "A commit cannot move B's state: %s", why);
    RxFootprint fpr;                /* rogues removed: their slots wait for reuse */
    footprint(&fpr);
    CHECK(fp_same(&fp1, &fpr, 0), "removed rogues left nothing active behind");
    CHECK(fx_run(&g_fx, &g_c1, 7, &o) == RX_OK && o.outcome == RXC_OUT_COMMITTED && o.result == 22 &&
          ref_eq(o.old_ref, ra.last), "A still commits after the rogues (%d)", o.outcome);
    JsBranchRef a_last = o.new_ref;
    CHECK(fx_run(&g_fx, &g_c2, 8, &o) == RX_OK && o.outcome == RXC_OUT_COMMITTED && o.result == 25 &&
          ref_eq(o.old_ref, rb.last), "B still commits after the rogues (%d)", o.outcome);

    /* Close A while B keeps running; a new attach takes instance 0 and its
     * removed reaction slots, and recovers A's last commit. */
    rx_compose_close(&g_c1);
    footprint(&fp2);
    CHECK(fp2.binders == 1 && fp2.reactions_active == fp0.reactions_active + 4 &&
          fp2.objects_live == fp0.objects_live + 5 && fp2.reactions_removed == fpr.reactions_removed + 4,
          "close of A took back its binder, reactions and objects");
    CHECK(fx_run(&g_fx, &g_c2, 9, &o) == RX_OK && o.outcome == RXC_OUT_COMMITTED && o.result == 28,
          "B commits after A closed (%d)", o.outcome);
    CHECK(attach(&g_c3, d1) == RX_OK && g_c3.inst == 0, "reattach on A's directory gets instance 0");
    footprint(&fp2);
    CHECK(fp2.reaction_slots == fpr.reaction_slots, "reattach reused the removed slots (%u vs %u)",
          fp2.reaction_slots, fpr.reaction_slots);
    CHECK(ref_eq(rx_compose_state(&g_c3), a_last), "reattach recovers A's last commit");
    CHECK(fx_run(&g_fx, &g_c3, 10, &o) == RX_OK && o.outcome == RXC_OUT_COMMITTED && o.result == 31 &&
          ref_eq(o.old_ref, a_last), "goal after reattach commits (%d)", o.outcome);
    rx_compose_close(&g_c3);
    rx_compose_close(&g_c2);
    footprint(&fp2);
    CHECK(fp_same(&fp0, &fp2, 0), "after both close the footprint is what it was before");
    if (!fp_same(&fp0, &fp2, 0)) { fp_print("before", &fp0); fp_print("after", &fp2); }
    CHECK(outside_poke(42), "World still works after close");
    rmtree(d1);
    rmtree(d2);
    rmtree(d3);
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

/* Admitted activations parked behind a running writer of their inputs (I11:
 * that writer's wake merges into them instead of a second commit). They count
 * in in_flight but nothing of theirs runs. */
static uint32_t upstream_held(void) {
    pthread_mutex_lock(&g_w.mu);
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_w.n_reactions; i++)
        if (g_w.reactions[i].upstream_held) n++;
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
    /* Everything else of this goal settles: the held step is the only one
     * running. The step that reads its proposal is woken by the other
     * candidate and (I11) waits parked behind the held writer instead of
     * committing on the stale value; it is admitted, so in_flight counts it. */
    for (waited = 0; in_flight() != 1 + upstream_held() && waited < 10000; waited++) sleep_us(1000);
    CHECK(in_flight() == 1 + upstream_held(), "only the held step running (in_flight %u, held behind it %u)",
          in_flight(), upstream_held());
    CHECK(upstream_held() == 1, "exactly one step parked behind the held writer (%u)", upstream_held());

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
    bool removed = g_w.reactions[rid].removed;
    CHECK(st == RX_DORMANT, "held step DORMANT after close (%s)", rx_state_name(st));
    CHECK(removed, "close removed the held step from the World");
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

/* ---- close reclaims everything: 2000 rounds -------------------------------- */

#define ROUNDS 2000u

static void cap_ids(const RxCompose *c, uint32_t out[13]) {
    uint32_t n = 0;
    out[n++] = c->cap_ext.cap_id;
    for (uint32_t k = 0; k < RXC_K; k++)
        for (uint32_t j = 0; j < 3; j++) out[n++] = c->cap_cand[k][j].cap_id;
    for (uint32_t j = 0; j < 4; j++) out[n++] = c->cap_verify[j].cap_id;
    for (uint32_t j = 0; j < 2; j++) out[n++] = c->cap_commit[j].cap_id;
    for (uint32_t i = 1; i < 13; i++)        /* sorted: the set, not the order */
        for (uint32_t j = i; j > 0 && out[j - 1] > out[j]; j--) {
            uint32_t t = out[j]; out[j] = out[j - 1]; out[j - 1] = t;
        }
}

static void t_reclaim_rounds(void) {
    printf("[*] reclaim: %u attach/close rounds, footprint identical after each\n", ROUNDS);
    char d[256];
    dir_for(d, sizeof d, "rounds");
    RxFootprint before, after, first_after, during;
    footprint(&before);
    uint32_t caps0[13], caps[13];
    uint32_t bad_fp = 0, bad_slots = 0, bad_caps = 0, bad_goal = 0, bad_attach = 0, bad_during = 0;
    uint32_t bad_removed = 0, reused = 0, pokes = 0, poked = 0, goals = 0;
    RxObjRef first_goal = { UINT32_MAX, 0 };
    for (uint32_t r = 0; r < ROUNDS; r++) {
        RxFootprint pre;
        footprint(&pre);
        if (attach(&g_c1, d) != RX_OK) { bad_attach++; break; }
        if (r == 0) first_goal = g_c1.goal;
        else if (g_c1.goal.id == first_goal.id && g_c1.goal.generation != first_goal.generation)
            reused++;
        footprint(&during);
        if (during.reactions_active != pre.reactions_active + 4 || during.binders != pre.binders + 1 ||
            during.objects_live != pre.objects_live + 5 || during.bound_fields != pre.bound_fields + 1)
            bad_during++;
        cap_ids(&g_c1, caps);
        if (r == 0) memcpy(caps0, caps, sizeof caps0);
        else if (memcmp(caps0, caps, sizeof caps)) bad_caps++;
        /* A goal on the first rounds, every 50th and the last two: the
         * leak check is about attach/close; goals show the World still works. */
        if (r < 3 || r % 50 == 0 || r + 2 >= ROUNDS) {
            RxcResult o;
            goals++;
            if (fx_run(&g_fx, &g_c1, 100 + r, &o) != RX_OK || o.outcome != RXC_OUT_COMMITTED ||
                o.result != (100 + r) * 3 + 1)
                bad_goal++;
        }
        uint32_t ids[4] = { g_c1.rx_cand[0], g_c1.rx_cand[1], g_c1.rx_verify, g_c1.rx_commit };
        rx_compose_close(&g_c1);
        pthread_mutex_lock(&g_w.mu);
        for (uint32_t i = 0; i < 4; i++)
            if (!g_w.reactions[ids[i]].removed || g_w.reactions[ids[i]].desc.fn) bad_removed++;
        pthread_mutex_unlock(&g_w.mu);
        footprint(&after);
        if (!fp_same(&pre, &after, r > 0)) {
            if (!bad_fp) { fp_print("pre", &pre); fp_print("post", &after); }
            bad_fp++;
        }
        if (r == 0) first_after = after;
        else if (after.reaction_slots != first_after.reaction_slots) bad_slots++;
        if (r % 100 == 0) { poked++; pokes += (uint32_t)outside_poke(20000 + r); }
    }
    CHECK(bad_attach == 0, "every attach succeeded");
    CHECK(bad_goal == 0, "every goal committed (%u of %u failed)", bad_goal, goals);
    CHECK(bad_during == 0, "each attach held exactly 4 reactions, 5 objects, 1 binder (%u off)",
          bad_during);
    CHECK(bad_fp == 0, "footprint after close identical to before the round (%u rounds differ)",
          bad_fp);
    CHECK(fp_same(&before, &first_after, 0) &&
          first_after.reaction_slots <= before.reaction_slots + 4u &&
          first_after.reactions_removed <= before.reactions_removed + 4u,
          "first round: only the reaction table kept its (at most 4) removed slots for reuse");
    CHECK(bad_slots == 0, "reaction table did not grow after the first round (%u rounds grew)",
          bad_slots);
    CHECK(bad_caps == 0, "every attach got the same 13 capability slots back (%u differ)", bad_caps);
    CHECK(bad_removed == 0, "every closed reaction removed and unreachable (%u not)", bad_removed);
    CHECK(reused > 0, "later attaches reused the retired object slots (%u)", reused);
    CHECK(pokes == poked, "outside reaction kept working (%u/%u)", pokes, poked);
    footprint(&after);
    printf("    %u rounds, %u goals: reaction slots %u (before %u), active %u, objects %u, binders %u\n",
           ROUNDS, goals, after.reaction_slots, before.reaction_slots, after.reactions_active,
           after.objects_live, after.binders);
    rmtree(d);
}

/* ---- a full reaction table refuses the attach ------------------------------- */

static int filler_fn(RxCtx *x) { (void)x; return 0; }

static void t_full_refused(void) {
    printf("[*] full: no room for four reactions -> RX_ERR_FULL, nothing built\n");
    char d[256];
    dir_for(d, sizeof d, "full");
    if (rx_world_init_native(&g_w2, g_view, 1, 1u << 16) != RX_OK) { CHECK(0, "second World"); return; }
    g_w2.external_subject = EXT_SUBJ;
    uint64_t z[RX_MAX_FIELDS] = { 0 };
    RxObjRef o;
    CHECK(rx_world_create(&g_w2, 1, RX_PERSIST_RESIDENT, RES_OUT_IN, z, &o) == RX_OK, "object");
    RxReactionDesc fd;
    memset(&fd, 0, sizeof fd);
    fd.name = "filler";
    fd.faculty = RX_FACULTY_OMEGA;
    fd.subject = OUT_SUBJ;
    fd.priority = RX_PRIO_BACKGROUND;
    fd.triggers[fd.n_triggers++] = (RxDep){ o, RX_ALL_FIELDS };
    fd.caps[fd.n_caps++] = (RxCapNeed){ g_cap_rin, RES_OUT_IN, RX_RIGHT_READ };
    fd.fn = filler_fn;
    uint32_t rid, n = 0;
    while (g_w2.n_reactions < RX_MAX_REACTIONS - 3u && rx_world_add_reaction(&g_w2, &fd, &rid) == RX_OK)
        n++;
    CHECK(g_w2.n_reactions == RX_MAX_REACTIONS - 3u, "table filled to 3 free (%u)", g_w2.n_reactions);
    RxFootprint a, b;
    rx_world_footprint(&g_w2, &a);
    int rc = rx_compose_attach(&g_c1, &g_w2, NULL, d, &g_fx.self, FX_SESSION, &g_fx.router,
                               fx_contract, g_admin);
    rx_world_footprint(&g_w2, &b);
    CHECK(rc == RX_ERR_FULL, "attach refused with RX_ERR_FULL (%d)", rc);
    CHECK(g_c1.world == NULL && fp_same(&a, &b, 1), "refused attach built nothing");
    /* Removing other subjects' reactions does not make room: a removed slot
     * is reused only by the same subject and faculty. */
    CHECK(rx_world_remove_reaction(&g_w2, rid) == RX_OK, "filler removed");
    CHECK(rx_world_remove_reaction(&g_w2, rid) == RX_ERR_NOT_FOUND, "second remove refused");
    CHECK(rx_world_add_reaction(&g_w2, &fd, &n) == RX_OK && n == rid, "same subject reuses the slot");
    rx_world_destroy(&g_w2);
    rmtree(d);
}

/* ---- the settle wait is configurable and reset by attach ------------------- */

static void t_wait_ms(void) {
    printf("[*] wait_ms: default 30000, set 1..600000, reset by attach\n");
    char d[256];
    dir_for(d, sizeof d, "waitms");
    CHECK(attach(&g_c1, d) == RX_OK, "attach");
    CHECK(g_c1.wait_ms == RXC_WAIT_MS_DEFAULT && RXC_WAIT_MS_DEFAULT == 30000u, "default 30000 (%u)",
          g_c1.wait_ms);
    CHECK(rx_compose_set_wait_ms(&g_c1, 0) == RX_ERR_ARG, "0 refused");
    CHECK(rx_compose_set_wait_ms(&g_c1, RXC_WAIT_MS_MAX + 1u) == RX_ERR_ARG, "600001 refused");
    CHECK(rx_compose_set_wait_ms(NULL, 1000) == RX_ERR_ARG, "NULL refused");
    CHECK(g_c1.wait_ms == RXC_WAIT_MS_DEFAULT, "refused calls change nothing");
    CHECK(rx_compose_set_wait_ms(&g_c1, 45000) == RX_OK && g_c1.wait_ms == 45000u, "45000 set");
    RxcResult o;
    CHECK(fx_run(&g_fx, &g_c1, 7, &o) == RX_OK && o.outcome == RXC_OUT_COMMITTED && o.result == 22,
          "a run still commits with the longer wait");
    rx_compose_close(&g_c1);
    CHECK(attach(&g_c1, d) == RX_OK, "reattach");
    CHECK(g_c1.wait_ms == RXC_WAIT_MS_DEFAULT, "reset to 30000 by attach (%u)", g_c1.wait_ms);
    rx_compose_close(&g_c1);
    rmtree(d);
}

int main(void) {
    snprintf(g_base, sizeof g_base, "/tmp/rx_compose_attach_test.XXXXXX");
    if (!mkdtemp(g_base)) { perror("mkdtemp"); return 1; }
    if (fx_init(&g_fx) != 0) { fprintf(stderr, "fixture init failed\n"); return 1; }
    int rc = world_start();
    if (rc != 0) { fprintf(stderr, "world start failed (%d)\n", rc); return 1; }
    t_two_per_world();
    t_close_waits();
    t_reclaim_rounds();
    t_full_refused();
    t_wait_ms();
    rx_world_destroy(&g_w);
    aienos_cap_stop(g_admin, g_view);
    fx_free(&g_fx);
    rmtree(g_base);
    printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
