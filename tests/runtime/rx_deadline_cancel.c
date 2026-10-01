/*
 * HD-09 resource contract v0, first enforcement cut (aien-architecture
 * docs/hardening/resource-contract-v0.md section 5): a reaction that declared
 * a deadline and whose function returns after the World's logical tick has
 * passed it is cancelled before publishing.
 *
 * Two reactions, both triggered by one outside object E, run in their own
 * functions at tick 0 and are held there by a gate:
 *   D  deadline 5, memory 64, energy 3, writes OD field 0
 *   H  no deadline, memory 32, energy 5, writes OH field 0 (the holder: it
 *      keeps its charge while D ends, so a missing or a double refund of D's
 *      charge is visible instead of being hidden by the clamp at zero)
 *
 *   late     tick moved to 6 while D is held, then D released:
 *            D has zero commits, OD field 0 and version unchanged, no COMMIT
 *            crumb of D anywhere, D's last crumb is CANCELLED with reason
 *            RX_ERR_DEADLINE, deadline_cancelled == 1; right after D ends
 *            the footprint is exactly H's charge (slots 1, memory 32,
 *            energy 5); after H ends it is back to the pre-admit zero;
 *            rx_world_verify_crumbs returns 0.
 *   control  tick moved only to 5 (the comparison is strictly greater):
 *            D commits exactly once, OD holds its value, no CANCELLED crumb,
 *            deadline_cancelled == 0, same footprint checks, chain verifies.
 *
 * Mutations that must make this test fail (none is applied by the build):
 *   M1 `>` changed to `>=` in the run_one check: control cancels -> FAIL
 *      (control: commits 0, CANCELLED crumb present).
 *   M2 the run_one check deleted: late commits -> FAIL (late: commits 1,
 *      OD changed, no CANCELLED crumb).
 *   M3 cancel, but stage/commit the proposed writes anyway: late -> FAIL
 *      (OD field/version changed, COMMIT crumb of D present).
 *   M4 cancel without end_activation's refund (or with a second uncharge):
 *      late -> FAIL (footprint after D ends is not exactly H's charge).
 *
 * Host only, CPU only. Exit 0 = PASS.
 */
#include "runtime/rx_caproot.h"
#include "runtime/rx_world.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { RD = 0, RH = 1, NR = 2 };

static const uint64_t k_mem[NR]    = { 64, 32 };
static const uint64_t k_energy[NR] = { 3, 5 };
static const uint64_t k_value[NR]  = { 0xD0D0u, 0x4040u };

/* ---- gate: each function parks until released --------------------------- */

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int in_fn[NR];
    int go[NR];
} g = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, {0}, {0} };

static RxObjRef g_out[NR];
static uint32_t g_idx[NR] = { RD, RH };

static int fn_gate(RxCtx *ctx) {
    uint32_t i = *(const uint32_t *)ctx->user;
    pthread_mutex_lock(&g.mu);
    g.in_fn[i] = 1;
    pthread_cond_broadcast(&g.cv);
    while (!g.go[i]) pthread_cond_wait(&g.cv, &g.mu);
    g.go[i] = 0;
    g.in_fn[i] = 0;
    pthread_mutex_unlock(&g.mu);
    ctx->n_out = 1;
    ctx->out[0].obj = g_out[i];
    ctx->out[0].field = 0;
    ctx->out[0].value = k_value[i];
    return 0;
}

static void release(uint32_t i) {
    pthread_mutex_lock(&g.mu);
    g.go[i] = 1;
    pthread_cond_broadcast(&g.cv);
    pthread_mutex_unlock(&g.mu);
}

static uint64_t ms_since(const struct timespec *t0) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)((now.tv_sec - t0->tv_sec) * 1000L + (now.tv_nsec - t0->tv_nsec) / 1000000L);
}

/* Bounded wait (5 s) until both functions are parked in the gate. */
static int wait_both_in_fn(void) {
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        pthread_mutex_lock(&g.mu);
        int ok = g.in_fn[RD] && g.in_fn[RH];
        pthread_mutex_unlock(&g.mu);
        if (ok) return 0;
        if (ms_since(&t0) > 5000) return -1;
        struct timespec ts = { 0, 100000 };
        nanosleep(&ts, NULL);
    }
}

/* Bounded wait (5 s) until D's activation has ended (DORMANT, H still in flight). */
static int wait_d_ended(RxWorld *w) {
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        pthread_mutex_lock(&w->mu);
        int ok = w->reactions[RD].state == RX_DORMANT && w->in_flight == 1;
        pthread_mutex_unlock(&w->mu);
        if (ok) return 0;
        if (ms_since(&t0) > 5000) return -1;
        struct timespec ts = { 0, 100000 };
        nanosleep(&ts, NULL);
    }
}

/* ---- authority stand-in: every reference is valid ------------------------ */

static int g_auth_ctx;
static int auth_ok(const void *c, RxCapRef ref, uint32_t subject, uint64_t resource,
                   uint32_t rights, RxCapEntry *out) {
    (void)c; (void)ref; (void)subject; (void)resource; (void)rights;
    if (out) memset(out, 0, sizeof *out);
    return RX_CAP_OK;
}
static int auth_inspect(const void *c, RxCapRef ref, RxCapEntry *out) {
    (void)c; (void)ref; (void)out;
    return RX_CAP_ERR_STATE;
}

/* ---- checks ----------------------------------------------------------------- */

static int g_fail;
#define CHECK(cond, ...) do { \
        if (cond) { printf("  ok    "); printf(__VA_ARGS__); printf("\n"); } \
        else { printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); g_fail++; } \
    } while (0)

static void budget(RxResourceBudget *b, uint64_t tick) {
    memset(b, 0, sizeof *b);
    b->slots = 4;
    b->memory_bytes = 1024;
    b->energy_budget = 1024;
    b->offered_locality = UINT32_MAX;
    b->offered_accel = UINT32_MAX;
    b->compute_mask = UINT32_MAX;
    b->logical_tick = tick;
}

static void footprint_is(RxWorld *w, uint32_t slots, uint64_t mem, uint64_t energy,
                         const char *when) {
    RxFootprint fp;
    rx_world_footprint(w, &fp);
    CHECK(fp.used_slots == slots && fp.used_memory == mem && fp.used_energy == energy,
          "%s: used_slots=%u used_memory=%llu used_energy=%llu (want %u/%llu/%llu)", when,
          fp.used_slots, (unsigned long long)fp.used_memory, (unsigned long long)fp.used_energy,
          slots, (unsigned long long)mem, (unsigned long long)energy);
}

/* One run. final_tick 6 = late case, 5 = control case. */
static int run_case(uint64_t final_tick) {
    const int late = final_tick > 5;
    printf("%s: deadline 5, tick 0 -> %llu while D is held\n", late ? "late" : "control",
           (unsigned long long)final_tick);
    memset(g.in_fn, 0, sizeof g.in_fn);
    memset(g.go, 0, sizeof g.go);

    RxWorld *w = calloc(1, sizeof *w);
    if (!w) return -1;
    if (rx_world_init_with_auth(w, NULL, &g_auth_ctx, auth_ok, auth_inspect, 2, 1u << 12) != RX_OK) {
        free(w);
        return -1;
    }
    w->external_subject = 100;
    RxResourceBudget b;
    budget(&b, 0);
    rx_world_set_resources(w, &b);

    RxObjRef e;
    int rc = rx_world_create(w, 1, RX_PERSIST_RESIDENT, 1, NULL, &e);
    for (uint32_t i = 0; i < NR && rc == RX_OK; i++)
        rc = rx_world_create(w, 1, RX_PERSIST_RESIDENT, 1, NULL, &g_out[i]);
    for (uint32_t i = 0; i < NR && rc == RX_OK; i++) {
        RxReactionDesc d;
        memset(&d, 0, sizeof d);
        d.name = i == RD ? "D" : "H";
        d.faculty = i + 1;
        d.subject = 1;
        d.priority = RX_PRIO_FOREGROUND;
        d.need.memory_bytes = k_mem[i];
        d.need.energy_cost = k_energy[i];
        d.need.deadline = i == RD ? 5 : 0;
        d.n_triggers = 1;
        d.triggers[0].obj = e;
        d.triggers[0].mask = RX_FIELD(0);
        d.n_writes = 1;
        d.writes[0].obj = g_out[i];
        d.writes[0].mask = RX_FIELD(0);
        d.n_caps = 1;
        d.caps[0].ref = (RxCapRef){ 1, 1 };
        d.caps[0].resource = 1;
        d.caps[0].rights = RX_RIGHT_READ | RX_RIGHT_WRITE;
        d.fn = fn_gate;
        d.user = &g_idx[i];
        uint32_t rid = UINT32_MAX;
        rc = rx_world_add_reaction(w, &d, &rid);
        if (rc == RX_OK && rid != i) rc = RX_ERR_ARG;
    }
    if (rc != RX_OK) {
        printf("  FAIL  setup rc=%d\n", rc);
        g_fail++;
        rx_world_destroy(w);
        free(w);
        return -1;
    }

    RxObject od0;
    memset(&od0, 0, sizeof od0);
    rx_world_read(w, g_out[RD], &od0);
    footprint_is(w, 0, 0, 0, "before admission");

    RxMutation m = { e, 0, 1 };
    int64_t ext = rx_world_publish_external(w, (RxCapRef){ 1, 1 }, &m, 1);
    CHECK(ext > 0, "outside publication at tick 0 (crumb %lld)", (long long)ext);
    int parked = wait_both_in_fn();
    CHECK(parked == 0, "D and H are both inside their functions");
    if (parked != 0) {
        release(RD);
        release(RH);
        rx_world_wait_quiescent(w, 5000);
        rx_world_destroy(w);
        free(w);
        return -1;
    }
    footprint_is(w, 2, k_mem[RD] + k_mem[RH], k_energy[RD] + k_energy[RH], "both running");

    budget(&b, final_tick);
    rx_world_set_resources(w, &b);
    release(RD);
    CHECK(wait_d_ended(w) == 0, "D's activation ended while H is still held");
    /* Exactly H's charge: D refunded once, not zero times, not twice. */
    footprint_is(w, 1, k_mem[RH], k_energy[RH], "after D ended, H still held");

    release(RH);
    CHECK(rx_world_wait_quiescent(w, 5000) == 0, "World quiescent after H released");
    footprint_is(w, 0, 0, 0, "after quiescence (pre-admission values)");

    RxObject od;
    memset(&od, 0, sizeof od);
    rx_world_read(w, g_out[RD], &od);
    RxObject oh;
    memset(&oh, 0, sizeof oh);
    rx_world_read(w, g_out[RH], &oh);
    uint64_t d_commits, h_commits, cancelled, d_commit_crumbs = 0, cancel_crumbs = 0;
    const RxCrumb *d_last = NULL;
    pthread_mutex_lock(&w->mu);
    d_commits = w->reactions[RD].commits;
    h_commits = w->reactions[RH].commits;
    cancelled = w->stats.deadline_cancelled;
    for (uint64_t i = 0; i < w->n_crumbs; i++) {
        const RxCrumb *c = &w->crumbs[i];
        if (c->reaction != RD) continue;
        d_last = c;
        if (c->kind == RX_CRUMB_COMMIT) d_commit_crumbs++;
        if (c->kind == RX_CRUMB_CANCELLED) cancel_crumbs++;
    }
    int d_last_kind = d_last ? (int)d_last->kind : -1;
    int d_last_reason = d_last ? d_last->reason : 0;
    pthread_mutex_unlock(&w->mu);

    CHECK(h_commits == 1 && oh.field[0] == k_value[RH], "H (no deadline) committed once");
    if (late) {
        CHECK(d_commits == 0, "D commits == 0 (got %llu)", (unsigned long long)d_commits);
        CHECK(d_commit_crumbs == 0, "no COMMIT crumb of D (got %llu)",
              (unsigned long long)d_commit_crumbs);
        CHECK(od.field[0] == od0.field[0] && od.version == od0.version &&
              od.field_version[0] == od0.field_version[0],
              "OD unchanged: field0 %llu -> %llu, version %llu -> %llu",
              (unsigned long long)od0.field[0], (unsigned long long)od.field[0],
              (unsigned long long)od0.version, (unsigned long long)od.version);
        CHECK(d_last_kind == RX_CRUMB_CANCELLED && d_last_reason == RX_ERR_DEADLINE,
              "D's last crumb is CANCELLED/RX_ERR_DEADLINE (got kind %d reason %d)",
              d_last_kind, d_last_reason);
        CHECK(cancel_crumbs == 1, "exactly one CANCELLED crumb of D (got %llu)",
              (unsigned long long)cancel_crumbs);
        CHECK(cancelled == 1, "stats.deadline_cancelled == 1 (got %llu)",
              (unsigned long long)cancelled);
    } else {
        CHECK(d_commits == 1, "D commits == 1 (got %llu)", (unsigned long long)d_commits);
        CHECK(od.field[0] == k_value[RD], "OD holds D's value");
        CHECK(cancel_crumbs == 0 && d_last_kind == RX_CRUMB_COMMIT,
              "no CANCELLED crumb of D; last is COMMIT (kind %d)", d_last_kind);
        CHECK(cancelled == 0, "stats.deadline_cancelled == 0 (got %llu)",
              (unsigned long long)cancelled);
    }
    uint64_t checked = 0;
    int vr = rx_world_verify_crumbs(w, &checked);
    CHECK(vr == 0, "crumb chain verifies (rc %d, %llu crumbs)", vr, (unsigned long long)checked);

    rx_world_destroy(w);
    free(w);
    return 0;
}

int main(void) {
    run_case(6);
    run_case(5);
    if (g_fail) {
        printf("RX_DEADLINE_CANCEL: FAIL (%d checks)\n", g_fail);
        return 1;
    }
    printf("RX_DEADLINE_CANCEL: PASS\n");
    return 0;
}
