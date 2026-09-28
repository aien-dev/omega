/*
 * Host rules for one resident seat. The graphics processor is not started.
 * A passing run is not the R12 gate. silicon_observed stays false.
 *
 * The stand-in sits in the same image as the processor: it observes a claim
 * naming input A and output B, performs the seat's one fixed operation
 * (B.field0 = A.field0 + A.field1), and publishes back. The dependent
 * reaction wakes from that publication and writes C. Authority is the native
 * AIENOS authority. Nothing here calls the graphics seat as a function.
 */
#include "runtime/aienos_cap.h"
#include "runtime/rx_world.h"
#include "omega_evidence.h"
#include "sha256.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define RES_A 0x10u
#define RES_B 0x20u
#define RES_C 0x30u
#define X 40u
#define Y 2u

enum { SUBJ_SEAT = 3, SUBJ_DEPEND = 4, SUBJ_EXTERNAL = 100, ISSUER = 3 };

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

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static uint64_t load_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static uint64_t window_field(RxWorld *w, uint32_t id, uint32_t field) {
    uint64_t off = w->objects[id].region_offset;
    return load_u64(w->coherent + off + (uint64_t)field * 8u);
}

/* The world checks capabilities through the native AIENOS authority. */
typedef struct {
    AienosCapAdmin *admin;
    AienosCapView *view;
    RxWorld w;
} Env;

typedef struct {
    RxObjRef a;
    RxObjRef b;
    RxObjRef c;
} Chain;

static int g_poison;

static int fn_poison(RxCtx *c) {
    (void)c;
    g_poison++;
    return -1;
}

/* C = B + 1 on the processor. */
static int fn_depend(RxCtx *c) {
    Chain *ch = c->user;
    const RxSnapshotDep *s = NULL;
    for (uint32_t i = 0; i < c->n_in; i++)
        if (c->in[i].obj.id == ch->b.id) s = &c->in[i];
    if (!s) return -1;
    c->out[c->n_out++] = (RxMutation){ ch->c, 0, s->field[0] + 1 };
    return 0;
}

static int env_start(Env *e) {
    memset(e, 0, sizeof(*e));
    if (aienos_cap_start(&e->admin, &e->view) != 0) return -1;
    if (rx_world_init_native(&e->w, e->view, 2, 1u << 16) != RX_OK) {
        aienos_cap_stop(e->admin, e->view);
        return -1;
    }
    e->w.external_subject = SUBJ_EXTERNAL;
    return 0;
}

static void env_stop(Env *e) {
    rx_world_destroy(&e->w);
    aienos_cap_stop(e->admin, e->view);
}

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

static RxObjRef mkobj(Env *e, uint64_t resource) {
    uint64_t init[RX_MAX_FIELDS] = { 0 };
    RxObjRef r = { UINT32_MAX, 0 };
    rx_world_create(&e->w, 1, RX_PERSIST_RESIDENT, resource, init, &r);
    return r;
}

static void offer(RxWorld *w, uint32_t accel) {
    RxResourceBudget b;
    memset(&b, 0, sizeof(b));
    b.slots = 8;
    b.memory_bytes = UINT64_MAX;
    b.energy_budget = UINT64_MAX;
    b.offered_locality = UINT32_MAX;
    b.offered_accel = accel;
    b.compute_mask = UINT32_MAX;
    rx_world_set_resources(w, &b);
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

static void seat_desc(RxReactionDesc *d, RxObjRef a, RxObjRef b, RxCapRef in, RxCapRef out) {
    desc_init(d, "resident.seat.add", RX_FACULTY_AEGIS, SUBJ_SEAT, fn_poison, NULL);
    d->need.accelerator_features = RX_ACCEL_BLACKWELL;
    d->n_triggers = 1;
    d->triggers[0] = (RxDep){ a, RX_FIELD(0) | RX_FIELD(1) };
    d->n_writes = 1;
    d->writes[0] = (RxDep){ b, RX_FIELD(0) };
    d->n_caps = 2;
    d->caps[0] = (RxCapNeed){ in, RES_A, RX_RIGHT_READ };
    d->caps[1] = (RxCapNeed){ out, RES_B, RX_RIGHT_WRITE };
}

static int wait_seat(RxWorld *w, uint32_t id) {
    for (int i = 0; i < 2000; i++) {
        if (w->reactions[id].state == RX_RUNNING && w->reactions[id].resident_seat) return 0;
        RxState s = w->reactions[id].state;
        if (s == RX_BLOCKED_RESOURCE || s == RX_BLOCKED_AUTHORITY || s == RX_FAILED ||
            s == RX_REJECTED || s == RX_DORMANT)
            return 1;
        sleep_ms(1);
    }
    return -1;
}

static uint64_t field_of(RxWorld *w, RxObjRef r, uint32_t f) {
    RxObject o;
    if (rx_world_read(w, r, &o) != RX_OK) return UINT64_MAX;
    return o.field[f];
}

static void t_shape(void) {
    printf("[*] registration shape\n");
    Env e;
    CHECK(env_start(&e) == 0, "setup");
    RxObjRef a = mkobj(&e, RES_A);
    RxObjRef b = mkobj(&e, RES_B);
    RxCapRef in = mint(&e, SUBJ_SEAT, RES_A, RX_RIGHT_READ);
    RxCapRef out = mint(&e, SUBJ_SEAT, RES_B, RX_RIGHT_READ | RX_RIGHT_WRITE);
    RxReactionDesc d;
    seat_desc(&d, a, b, in, out);
    d.need.accelerator_features = 0;
    d.fn = NULL;
    CHECK(rx_world_add_reaction(&e.w, &d, NULL) == RX_ERR_ARG, "a processor reaction needs a body");

    seat_desc(&d, a, a, in, out);
    d.writes[0] = (RxDep){ a, RX_FIELD(0) };
    CHECK(rx_world_add_reaction(&e.w, &d, NULL) == RX_ERR_ARG,
          "the seat must not write the object it reads");

    seat_desc(&d, a, b, in, out);
    d.writes[0] = (RxDep){ b, RX_FIELD(1) };
    CHECK(rx_world_add_reaction(&e.w, &d, NULL) == RX_ERR_ARG, "the seat writes field 0 only");

    seat_desc(&d, a, b, in, out);
    d.triggers[0] = (RxDep){ a, RX_FIELD(2) };
    CHECK(rx_world_add_reaction(&e.w, &d, NULL) == RX_ERR_ARG, "the seat reads fields 0 and 1");

    seat_desc(&d, a, b, in, out);
    CHECK(rx_world_add_reaction(&e.w, &d, NULL) == RX_OK, "seat registration");
    env_stop(&e);
}

typedef struct {
    Env e;
    Chain ch;
    uint32_t seat;
    uint32_t dep;
    RxCapRef ext;
    RxCapRef seat_in;
    RxCapRef seat_out;
} Rig;

static int rig_start(Rig *r, int place) {
    memset(r, 0, sizeof(*r));
    g_poison = 0;
    if (env_start(&r->e) != 0) return -1;
    r->ch.a = mkobj(&r->e, RES_A);
    r->ch.b = mkobj(&r->e, RES_B);
    r->ch.c = mkobj(&r->e, RES_C);
    if (place && (rx_world_attach_physical(&r->e.w, r->ch.a) != RX_OK ||
                  rx_world_attach_physical(&r->e.w, r->ch.b) != RX_OK))
        return -1;
    r->seat_in = mint(&r->e, SUBJ_SEAT, RES_A, RX_RIGHT_READ);
    r->seat_out = mint(&r->e, SUBJ_SEAT, RES_B, RX_RIGHT_READ | RX_RIGHT_WRITE);
    if (rx_world_bind_capability(&r->e.w, r->ch.a, r->seat_in) != RX_OK) return -1;
    if (rx_world_bind_capability(&r->e.w, r->ch.b, r->seat_out) != RX_OK) return -1;
    r->ext = mint(&r->e, SUBJ_EXTERNAL, RES_A, RX_RIGHT_WRITE);
    RxCapRef dep_r = mint(&r->e, SUBJ_DEPEND, RES_B, RX_RIGHT_READ);
    RxCapRef dep_w = mint(&r->e, SUBJ_DEPEND, RES_C, RX_RIGHT_WRITE);
    if (rx_world_enable_resident(&r->e.w) != RX_OK) return -1;

    RxReactionDesc d;
    seat_desc(&d, r->ch.a, r->ch.b, r->seat_in, r->seat_out);
    if (rx_world_add_reaction(&r->e.w, &d, &r->seat) != RX_OK) return -1;

    desc_init(&d, "cpu.dependent", RX_FACULTY_AEGIS, SUBJ_DEPEND, fn_depend, &r->ch);
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){ r->ch.b, RX_FIELD(0) };
    d.n_writes = 1;
    d.writes[0] = (RxDep){ r->ch.c, RX_FIELD(0) };
    d.n_caps = 2;
    d.caps[0] = (RxCapNeed){ dep_r, RES_B, RX_RIGHT_READ };
    d.caps[1] = (RxCapNeed){ dep_w, RES_C, RX_RIGHT_WRITE };
    if (rx_world_add_reaction(&r->e.w, &d, &r->dep) != RX_OK) return -1;
    return 0;
}

static int64_t poke(Rig *r, uint64_t x, uint64_t y) {
    RxMutation m[2] = { { r->ch.a, 0, x }, { r->ch.a, 1, y } };
    return rx_world_publish_external(&r->e.w, r->ext, m, 2);
}

static void t_admission_and_wake(void) {
    printf("[*] admission, then A -> seat add -> B -> processor -> C\n");
    Rig r;
    CHECK(rig_start(&r, 1) == 0, "setup");
    if (g_fail && r.e.w.n_workers == 0) return;
    offer(&r.e.w, 0);
    int64_t cause = poke(&r, X, Y);
    CHECK(cause > 0, "stimulus");
    CHECK(rx_world_wait_quiescent(&r.e.w, 1000) == RX_OK, "blocked seat did not settle");
    CHECK(r.e.w.reactions[r.seat].state == RX_BLOCKED_RESOURCE, "seat ran without the graphics budget");
    CHECK(r.e.w.reactions[r.seat].activations == 0, "blocked seat counted an activation");
    CHECK(r.e.w.reactions[r.seat].resident_seat == false, "a claim was posted without admission");
    CHECK(field_of(&r.e.w, r.ch.b, 0) == 0, "B changed before the seat ran");
    CHECK(g_poison == 0, "the seat body was called as a function");

    offer(&r.e.w, RX_ACCEL_BLACKWELL);
    CHECK(wait_seat(&r.e.w, r.seat) == 0, "admitted seat did not reach the claim");
    int step = rx_resident_seat_step(&r.e.w);
    CHECK(step == 1, "stand-in step %d", step);
    CHECK(g_poison == 0, "stand-in called the seat body");
    CHECK(window_field(&r.e.w, r.ch.b.id, 0) == X + Y, "B's window does not hold the sum");
    CHECK(field_of(&r.e.w, r.ch.b, 0) == 0, "canonical B changed before accept");
    CHECK(rx_resident_accept(&r.e.w) == RX_OK, "accept");
    CHECK(rx_world_wait_quiescent(&r.e.w, 3000) == RX_OK, "dependent did not finish");

    CHECK(field_of(&r.e.w, r.ch.a, 0) == X && field_of(&r.e.w, r.ch.a, 1) == Y, "A was rewritten");
    CHECK(field_of(&r.e.w, r.ch.b, 0) == X + Y, "canonical B");
    CHECK(field_of(&r.e.w, r.ch.c, 0) == X + Y + 1, "dependent did not observe B");
    CHECK(r.e.w.objects[r.ch.b.id].coherency == RX_COHERENCY_SEAT, "coherency");
    CHECK(g_poison == 0, "seat body ran during accept");
    CHECK(r.e.w.reactions[r.seat].commits == 1, "seat commits %llu",
          (unsigned long long)r.e.w.reactions[r.seat].commits);
    CHECK(r.e.w.reactions[r.dep].commits == 1, "dependent commits");

    uint64_t seat_id = 0, dep_id = 0;
    for (uint64_t id = 1; id <= r.e.w.n_crumbs; id++) {
        const RxCrumb *k = rx_world_crumb(&r.e.w, id);
        if (!k || k->kind != RX_CRUMB_COMMIT) continue;
        if (k->worker == RX_SEAT_BLACKWELL) seat_id = id;
        if (k->reaction == r.dep) dep_id = id;
    }
    CHECK(seat_id != 0 && dep_id != 0, "missing commit crumbs");
    if (seat_id && dep_id) {
        const RxCrumb *seat = rx_world_crumb(&r.e.w, seat_id);
        const RxCrumb *dep = rx_world_crumb(&r.e.w, dep_id);
        CHECK(seat->wake_cause == (uint64_t)cause, "seat cause is not the stimulus");
        CHECK(dep->wake_cause == seat_id, "dependent was not woken by the seat");
        CHECK(rx_world_explain(&r.e.w, r.ch.b, 0) == seat_id, "B writer");
        CHECK(rx_world_explain(&r.e.w, r.ch.c, 0) == dep_id, "C writer");
    }
    uint64_t checked = 0;
    CHECK(rx_world_verify_crumbs(&r.e.w, &checked) == 0, "crumb chain");
    CHECK(checked > 0, "no crumbs verified");
    CHECK(r.e.w.stats.illegal_transitions == 0, "illegal lifecycle");

    CHECK(rx_resident_shutdown(&r.e.w) == RX_OK, "shutdown");
    CHECK(rx_resident_seat_step(&r.e.w) == 2, "shutdown was not observed");
    CHECK(r.e.w.resident_stopped, "seat did not stop");
    uint64_t tail = rx_world_publication_tail(&r.e.w);
    CHECK(rx_resident_reset(&r.e.w) == RX_OK, "reset");
    CHECK(!r.e.w.resident_stopped, "reset left the seat stopped");
    CHECK(rx_world_publication_tail(&r.e.w) == tail, "reset moved the ring");
    CHECK(rx_resident_seat_step(&r.e.w) == 0, "reset replayed a finished claim");
    CHECK(r.e.w.reactions[r.seat].commits == 1, "replay committed again");
    env_stop(&r.e);
}

static void t_revoked_input(void) {
    printf("[*] input capability revoked before the seat reads\n");
    Rig r;
    CHECK(rig_start(&r, 1) == 0, "setup");
    offer(&r.e.w, RX_ACCEL_BLACKWELL);
    CHECK(poke(&r, X, Y) > 0, "stimulus");
    CHECK(wait_seat(&r.e.w, r.seat) == 0, "claim was not posted");
    CHECK(revoke_cap(&r.e, r.seat_in) == 0, "revoke");
    int step = rx_resident_seat_step(&r.e.w);
    CHECK(step == RX_ERR_AUTHORITY, "revoked claim step %d", step);
    CHECK(window_field(&r.e.w, r.ch.b.id, 0) == 0, "revoked claim wrote B's window");
    CHECK(rx_resident_accept(&r.e.w) == RX_ERR_STALE_GEN, "fault was published as success");
    CHECK(rx_world_wait_quiescent(&r.e.w, 1000) == RX_OK, "revoked seat did not settle");
    CHECK(field_of(&r.e.w, r.ch.b, 0) == 0, "canonical B changed");
    CHECK(field_of(&r.e.w, r.ch.c, 0) == 0, "dependent ran after a refused seat");
    CHECK(g_poison == 0, "poison body");
    env_stop(&r.e);
}

static void t_revoked_output(void) {
    printf("[*] output capability revoked after the seat wrote, before publication\n");
    Rig r;
    CHECK(rig_start(&r, 1) == 0, "setup");
    offer(&r.e.w, RX_ACCEL_BLACKWELL);
    CHECK(poke(&r, X, Y) > 0, "stimulus");
    CHECK(wait_seat(&r.e.w, r.seat) == 0, "claim was not posted");
    CHECK(rx_resident_seat_step(&r.e.w) == 1, "stand-in step");
    CHECK(window_field(&r.e.w, r.ch.b.id, 0) == X + Y, "seat did not write");
    CHECK(revoke_cap(&r.e, r.seat_out) == 0, "revoke");
    CHECK(rx_resident_accept(&r.e.w) == RX_ERR_AUTHORITY, "revoked output published");
    CHECK(window_field(&r.e.w, r.ch.b.id, 0) == 0, "B's window was not restored");
    CHECK(rx_world_wait_quiescent(&r.e.w, 1000) == RX_OK, "did not settle");
    CHECK(field_of(&r.e.w, r.ch.b, 0) == 0, "canonical B changed");
    CHECK(field_of(&r.e.w, r.ch.c, 0) == 0, "dependent ran");
    env_stop(&r.e);
}

static void t_stale(void) {
    printf("[*] stale generation\n");
    Rig r;
    CHECK(rig_start(&r, 1) == 0, "setup");
    offer(&r.e.w, RX_ACCEL_BLACKWELL);
    CHECK(poke(&r, X, Y) > 0, "stimulus");
    CHECK(wait_seat(&r.e.w, r.seat) == 0, "claim was not posted");
    CHECK(rx_world_retire(&r.e.w, r.ch.a) == RX_OK, "retire");
    int step = rx_resident_seat_step(&r.e.w);
    CHECK(step == RX_ERR_STALE_GEN, "stale step %d", step);
    CHECK(window_field(&r.e.w, r.ch.b.id, 0) == 0, "stale claim wrote B");
    CHECK(rx_resident_accept(&r.e.w) == RX_ERR_STALE_GEN, "stale accept");
    CHECK(rx_world_wait_quiescent(&r.e.w, 1000) == RX_OK, "stale seat did not settle");
    CHECK(field_of(&r.e.w, r.ch.c, 0) == 0, "dependent ran on a stale seat");
    CHECK(g_poison == 0, "poison body");
    env_stop(&r.e);
}

static void t_output_moved(void) {
    printf("[*] output generation moved after the claim\n");
    Rig r;
    CHECK(rig_start(&r, 1) == 0, "setup");
    offer(&r.e.w, RX_ACCEL_BLACKWELL);
    CHECK(poke(&r, X, Y) > 0, "stimulus");
    CHECK(wait_seat(&r.e.w, r.seat) == 0, "claim was not posted");
    CHECK(rx_world_retire(&r.e.w, r.ch.b) == RX_OK, "retire B");
    int step = rx_resident_seat_step(&r.e.w);
    CHECK(step == RX_ERR_STALE_GEN, "moved output step %d", step);
    CHECK(rx_resident_accept(&r.e.w) == RX_ERR_STALE_GEN, "moved output accept");
    CHECK(rx_world_wait_quiescent(&r.e.w, 1000) == RX_OK, "did not settle");
    CHECK(field_of(&r.e.w, r.ch.c, 0) == 0, "dependent ran");
    env_stop(&r.e);
}

static void t_hostile_window(void) {
    printf("[*] hostile input window\n");
    Rig r;
    CHECK(rig_start(&r, 1) == 0, "setup");
    offer(&r.e.w, RX_ACCEL_BLACKWELL);
    CHECK(poke(&r, X, Y) > 0, "stimulus");
    CHECK(wait_seat(&r.e.w, r.seat) == 0, "claim was not posted");
    uint8_t *win = r.e.w.coherent + r.e.w.objects[r.ch.a.id].region_offset;
    win[0] ^= 0xff;
    int step = rx_resident_seat_step(&r.e.w);
    CHECK(step == RX_ERR_TORN, "torn step %d", step);
    CHECK(rx_resident_accept(&r.e.w) == RX_ERR_STALE_GEN, "torn accept");
    CHECK(rx_world_wait_quiescent(&r.e.w, 1000) == RX_OK, "torn seat did not settle");
    CHECK(window_field(&r.e.w, r.ch.b.id, 0) == 0, "torn claim wrote B");
    CHECK(field_of(&r.e.w, r.ch.c, 0) == 0, "dependent ran on a torn window");
    env_stop(&r.e);
}

static void t_forged_result(void) {
    printf("[*] result window that is not the sum\n");
    Rig r;
    CHECK(rig_start(&r, 1) == 0, "setup");
    offer(&r.e.w, RX_ACCEL_BLACKWELL);
    CHECK(poke(&r, X, Y) > 0, "stimulus");
    CHECK(wait_seat(&r.e.w, r.seat) == 0, "claim was not posted");
    CHECK(rx_resident_seat_step(&r.e.w) == 1, "stand-in step");
    uint8_t *win = r.e.w.coherent + r.e.w.objects[r.ch.b.id].region_offset;
    win[0] ^= 0x01;
    CHECK(rx_resident_accept(&r.e.w) == RX_ERR_TORN, "a wrong sum was published");
    CHECK(window_field(&r.e.w, r.ch.b.id, 0) == 0, "B's window was not restored");
    CHECK(rx_world_wait_quiescent(&r.e.w, 1000) == RX_OK, "did not settle");
    CHECK(field_of(&r.e.w, r.ch.b, 0) == 0 && field_of(&r.e.w, r.ch.c, 0) == 0, "B or C moved");
    env_stop(&r.e);
}

static void t_unplaced(void) {
    printf("[*] unplaced objects\n");
    Rig r;
    CHECK(rig_start(&r, 0) == 0, "setup");
    offer(&r.e.w, RX_ACCEL_BLACKWELL);
    CHECK(poke(&r, X, Y) > 0, "stimulus");
    CHECK(rx_world_wait_quiescent(&r.e.w, 2000) == RX_OK, "unplaced seat did not settle");
    CHECK(r.e.w.reactions[r.seat].commits == 0, "unplaced seat committed");
    CHECK(r.e.w.reactions[r.seat].resident_seat == false, "unplaced seat left a claim");
    CHECK(field_of(&r.e.w, r.ch.b, 0) == 0, "B");
    CHECK(field_of(&r.e.w, r.ch.c, 0) == 0, "dependent");
    CHECK(r.e.w.stats.invalidations > 0, "no invalidation");
    CHECK(g_poison == 0, "poison body");
    env_stop(&r.e);
}

static void t_forged_publish(void) {
    printf("[*] forged capability against the native authority\n");
    Env e;
    CHECK(env_start(&e) == 0, "setup");
    RxObjRef a = mkobj(&e, RES_A);
    RxCapRef forged = { 200, 7 };
    RxMutation bad = { a, 0, 5 };
    CHECK(rx_world_publish_external(&e.w, forged, &bad, 1) == RX_ERR_AUTHORITY, "forged publish");
    CHECK(field_of(&e.w, a, 0) == 0, "forged publish wrote the object");
    env_stop(&e);
}

static uint64_t crumb_count(RxWorld *w, uint32_t reaction, RxCrumbKind kind, int reason,
                            uint64_t *last) {
    uint64_t n = 0;
    for (uint64_t id = 1; id <= w->n_crumbs; id++) {
        const RxCrumb *k = rx_world_crumb(w, id);
        if (!k || k->reaction != reaction || k->kind != kind) continue;
        if (reason && k->reason != reason) continue;
        n++;
        if (last) *last = id;
    }
    return n;
}

static OmegaSharedWorldRing *ring(RxWorld *w, uint64_t off) {
    return (OmegaSharedWorldRing *)(w->coherent + off);
}

static void put_window(RxWorld *w, uint32_t id, uint64_t v) {
    uint8_t *p = w->coherent + w->objects[id].region_offset;
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* The seat dies holding a claim, after writing B's window and before its
 * result. The claim fails once with a recorded cause, B's window returns to
 * the canonical object, the charge is released once, and the retried claim
 * runs under the next seat generation. Nothing from the old seat publishes. */
static void t_seat_lost_retry(void) {
    printf("[*] seat lost holding a claim: recorded, restored, released once, retried\n");
    Rig r;
    CHECK(rig_start(&r, 1) == 0, "setup");
    RxWorld *w = &r.e.w;
    offer(w, RX_ACCEL_BLACKWELL);
    uint32_t gen0 = w->seat_generation;
    uint32_t ga = w->objects[r.ch.a.id].generation, gb = w->objects[r.ch.b.id].generation;
    int64_t cause = poke(&r, X, Y);
    CHECK(cause > 0, "stimulus");
    CHECK(wait_seat(w, r.seat) == 0, "no claim");
    OmegaSharedWorldRing *c2g = ring(w, rx_world_off_c2g());
    OmegaSharedWorldDesc old = c2g->slots[w->reactions[r.seat].resident_seq & c2g->mask];
    CHECK(old.producer_generation == gen0, "the claim does not carry the seat generation");
    put_window(w, r.ch.b.id, X + Y);   /* the dead seat got this far */
    CHECK(w->used_slots == 1, "the claim is not charged");
    uint64_t lost_before = w->stats.transitions[RX_RUNNING][RX_FAILED];

    CHECK(rx_resident_seat_lost(w, 1) == 1, "the held claim was not declared lost");
    CHECK(w->seat_generation == gen0 + 1, "the seat generation did not move");
    CHECK(w->stats.transitions[RX_RUNNING][RX_FAILED] == lost_before + 1, "lost more than once");
    CHECK(window_field(w, r.ch.b.id, 0) == 0, "B's window was not restored");
    CHECK(field_of(w, r.ch.b, 0) == 0, "canonical B changed");
    uint64_t fail_id = 0;
    CHECK(crumb_count(w, r.seat, RX_CRUMB_FAILED, RX_ERR_SEAT_LOST, &fail_id) == 1,
          "no single seat-lost crumb");
    if (fail_id)
        CHECK(rx_world_crumb(w, fail_id)->wake_cause == (uint64_t)cause,
              "the loss does not name the stimulus");

    CHECK(wait_seat(w, r.seat) == 0, "the lost work was not retried");
    uint64_t seq = w->reactions[r.seat].resident_seq;
    OmegaSharedWorldDesc fresh = c2g->slots[seq & c2g->mask];
    CHECK(fresh.producer_generation == gen0 + 1, "the retry carries the old generation");
    CHECK(w->reactions[r.seat].resident_parent == fail_id, "the retry is not caused by the loss");
    CHECK(w->used_slots == 1, "the retry is charged %u times", w->used_slots);

    /* A result under the old generation that names the retried claim, with
     * the right sum in B's window. It must not publish. */
    OmegaSharedWorldDesc forged = fresh;
    forged.msg_type = RX_RING_PUBLISH;
    forged.producer_generation = gen0;
    rx_world_seal_descriptor(&forged);
    OmegaSharedWorldRing *g2c = ring(w, rx_world_off_g2c());
    uint64_t t = g2c->tail;
    g2c->slots[t & g2c->mask] = forged;
    __atomic_store_n(&g2c->tail, t + 1, __ATOMIC_RELEASE);
    put_window(w, r.ch.b.id, X + Y);
    CHECK(rx_resident_accept(w) == RX_ERR_STALE_GEN, "an old-generation result was accepted");
    CHECK(field_of(w, r.ch.b, 0) == 0 && w->reactions[r.seat].commits == 0,
          "an old-generation result published");
    put_window(w, r.ch.b.id, 0);

    /* The old claim, posted again: the seat refuses it. */
    CHECK(rx_world_inject_descriptor(w, &old) == RX_OK, "replay not posted");
    CHECK(rx_resident_seat_step(w) == 1, "the retried claim did not run");
    CHECK(rx_resident_accept(w) == RX_OK, "the retried result was refused");
    CHECK(rx_resident_seat_step(w) < 0, "the replayed claim was taken");
    CHECK(rx_resident_accept(w) == RX_ERR_STALE_GEN, "the replay's fault notice was accepted");
    CHECK(rx_world_wait_quiescent(w, 3000) == RX_OK, "world did not settle");

    CHECK(field_of(w, r.ch.b, 0) == X + Y && field_of(w, r.ch.c, 0) == X + Y + 1,
          "the retried work did not reach B and C");
    CHECK(w->reactions[r.seat].commits == 1, "seat commits %llu",
          (unsigned long long)w->reactions[r.seat].commits);
    uint64_t commit_id = 0;
    CHECK(crumb_count(w, r.seat, RX_CRUMB_COMMIT, 0, &commit_id) == 1, "not exactly one commit");
    if (commit_id)
        CHECK(rx_world_crumb(w, commit_id)->wake_cause == fail_id,
              "the commit is not caused by the loss");
    CHECK(w->used_slots == 0 && w->in_flight == 0, "a charge was not released");
    CHECK(w->stats.resident_claims == w->stats.resident_closed, "claims %llu closed %llu",
          (unsigned long long)w->stats.resident_claims,
          (unsigned long long)w->stats.resident_closed);
    CHECK(w->objects[r.ch.a.id].generation == ga && w->objects[r.ch.b.id].generation == gb,
          "an object generation moved");
    CHECK(rx_resident_seat_lost(w, 1) == 0, "an idle seat lost a claim");
    CHECK(w->used_slots == 0 && w->reactions[r.seat].commits == 1, "a second loss changed state");
    uint64_t checked = 0;
    CHECK(rx_world_verify_crumbs(w, &checked) == 0, "crumb chain");
    CHECK(w->stats.illegal_transitions == 0, "illegal lifecycle");
    env_stop(&r.e);
}

/* Without retry the loss is final for that activation. The next change of A
 * runs normally under the new generation. */
static void t_seat_lost_final(void) {
    printf("[*] seat lost without retry: the activation fails, the next one runs\n");
    Rig r;
    CHECK(rig_start(&r, 1) == 0, "setup");
    RxWorld *w = &r.e.w;
    offer(w, RX_ACCEL_BLACKWELL);
    CHECK(poke(&r, X, Y) > 0, "stimulus");
    CHECK(wait_seat(w, r.seat) == 0, "no claim");
    CHECK(rx_resident_seat_lost(w, 0) == 1, "the held claim was not declared lost");
    CHECK(rx_world_wait_quiescent(w, 3000) == RX_OK, "world did not settle");
    CHECK(w->reactions[r.seat].state == RX_DORMANT, "the lost activation ran again");
    CHECK(rx_resident_seat_step(w) == 0, "a discarded claim was still on the ring");
    CHECK(w->used_slots == 0, "the charge was not released");
    CHECK(field_of(w, r.ch.b, 0) == 0 && field_of(w, r.ch.c, 0) == 0, "B or C moved");
    CHECK(poke(&r, 5, 6) > 0, "stimulus");
    CHECK(wait_seat(w, r.seat) == 0, "no claim after the loss");
    CHECK(rx_resident_seat_step(w) == 1, "stand-in step");
    CHECK(rx_resident_accept(w) == RX_OK, "accept");
    CHECK(rx_world_wait_quiescent(w, 3000) == RX_OK, "world did not settle");
    CHECK(field_of(w, r.ch.b, 0) == 11 && field_of(w, r.ch.c, 0) == 12, "the next run");
    CHECK(w->stats.resident_claims == w->stats.resident_closed, "a claim did not end once");
    CHECK(w->stats.illegal_transitions == 0, "illegal lifecycle");
    env_stop(&r.e);
}

static void binary_digest(char out[65]) {
    strcpy(out, "unavailable");
    FILE *f = fopen("/proc/self/exe", "rb");
    if (!f) return;
    sha256_ctx c;
    sha256_init(&c);
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) sha256_update(&c, buf, n);
    fclose(f);
    uint8_t d[32];
    sha256_final(&c, d);
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", d[i]);
}

static void write_receipt(void) {
    char path[512];
    if (omega_evidence_path("R12/rx_resident_host_receipt.json", path, sizeof path) != 0) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    char commit[41];
    memset(commit, 0, sizeof commit);
    if (!omega_evidence_run_commit(commit)) memcpy(commit, "unknown", 8);
    const char *candidate = getenv("OMEGA_CANDIDATE_COMMIT");
    int bound = candidate && candidate[0] && strcmp(candidate, commit) == 0 &&
                !omega_evidence_tree_dirty();
    char digest[65];
    binary_digest(digest);
    struct utsname u;
    memset(&u, 0, sizeof u);
    uname(&u);
    fprintf(f,
            "{\n"
            "  \"schema\": \"AIEN_RX_R12_RESIDENT_HOST_V1\",\n"
            "  \"run_id\": \"%s\",\n"
            "  \"candidate_commit\": %s%s%s,\n"
            "  \"candidate_bound\": %s,\n"
            "  \"run_commit\": \"%s\",\n"
            "  \"tree_dirty\": %s,\n"
            "  \"checks\": %d,\n"
            "  \"failures\": %d,\n"
            "  \"silicon_observed\": false,\n"
            "  \"claimed\": false,\n"
            "  \"test_binary_sha256\": \"%s\",\n"
            "  \"host\": {\"sysname\": \"%s\", \"release\": \"%s\", \"machine\": \"%s\"},\n"
            "  \"hardware_scope\": \"host CPU only; graphics seat is an in-process stand-in; R12 is not claimed\",\n"
            "  \"gates\": {\n"
            "    \"R12_RESIDENT_SEAT\": \"HOST_RULES_PASS\",\n"
            "    \"not_claimed\": [\"R8\", \"R10\", \"R11\", \"R12\", \"R13\"]\n"
            "  }\n"
            "}\n",
            omega_evidence_run_id(), candidate ? "\"" : "", candidate ? candidate : "null",
            candidate ? "\"" : "", bound ? "true" : "false", commit,
            omega_evidence_tree_dirty() ? "true" : "false", g_checks, g_fail, digest, u.sysname,
            u.release, u.machine);
    fclose(f);
    printf("receipt: %s\n", path);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    t_shape();
    t_admission_and_wake();
    t_revoked_input();
    t_revoked_output();
    t_stale();
    t_output_moved();
    t_hostile_window();
    t_forged_result();
    t_unplaced();
    t_forged_publish();
    t_seat_lost_retry();
    t_seat_lost_final();
    printf("checks %d failures %d\n", g_checks, g_fail);
    write_receipt();
    return g_fail ? 1 : 0;
}
