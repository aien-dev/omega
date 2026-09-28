/*
 * Host rules for one resident seat. The graphics processor is not started.
 * A passing run is not the R12 gate. silicon_observed stays false.
 *
 * The stand-in sits in the same image as the processor: it observes a claim,
 * applies the rule that was fixed before launch, and publishes back. The
 * dependent reaction wakes from that publication. Nothing here calls the
 * graphics seat as a function.
 */
#include "runtime/rx_caproot.h"
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

#define RULE_K 0xA5A5u
#define STIMULUS 42u
#define RES_SRC 0x10u
#define RES_OUT 0x20u

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

typedef struct {
    RxCapRoot root;
    RxCapAdmin admin;
    RxWorld w;
} Env;

typedef struct {
    RxObjRef src;
    RxObjRef out;
} Chain;

static int g_poison;

static int fn_poison(RxCtx *c) {
    (void)c;
    g_poison++;
    return -1;
}

static int fn_depend(RxCtx *c) {
    Chain *ch = c->user;
    const RxSnapshotDep *s = NULL;
    for (uint32_t i = 0; i < c->n_in; i++)
        if (c->in[i].obj.id == ch->src.id) s = &c->in[i];
    if (!s) return -1;
    c->out[c->n_out++] = (RxMutation){ ch->out, 0, s->field[1] + 1 };
    return 0;
}

static int env_start(Env *e) {
    memset(e, 0, sizeof(*e));
    if (rx_caproot_start(&e->root, &e->admin) != RX_CAP_OK) return -1;
    if (rx_world_init(&e->w, &e->root, 2, 1u << 16) != RX_OK) {
        rx_caproot_stop(&e->root, &e->admin);
        return -1;
    }
    e->w.external_subject = SUBJ_EXTERNAL;
    return 0;
}

static void env_stop(Env *e) {
    rx_world_destroy(&e->w);
    rx_caproot_stop(&e->root, &e->admin);
}

static RxCapRef mint(Env *e, uint32_t subject, uint64_t resource, uint32_t rights) {
    RxCapMint m;
    memset(&m, 0, sizeof(m));
    m.issuer = ISSUER;
    m.subject = subject;
    m.resource = resource;
    m.rights = rights;
    m.parent = (RxCapRef){ UINT32_MAX, 0 };
    m.authority = rx_capadmin_office(&e->admin);
    RxCapRef r = { UINT32_MAX, 0 };
    if (rx_capadmin_mint(&e->admin, &m, &r) != RX_CAP_OK) r = (RxCapRef){ UINT32_MAX, 0 };
    return r;
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
    RxObjRef src = mkobj(&e, RES_SRC);
    RxCapRef cap = mint(&e, SUBJ_SEAT, RES_SRC, RX_RIGHT_READ | RX_RIGHT_WRITE);
    RxReactionDesc d;
    desc_init(&d, "no.fn", RX_FACULTY_AEGIS, SUBJ_SEAT, NULL, NULL);
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){ src, RX_FIELD(0) };
    d.n_writes = 1;
    d.writes[0] = (RxDep){ src, RX_FIELD(0) };
    d.n_caps = 1;
    d.caps[0] = (RxCapNeed){ cap, RES_SRC, RX_RIGHT_READ | RX_RIGHT_WRITE };
    CHECK(rx_world_add_reaction(&e.w, &d, NULL) == RX_ERR_ARG, "a processor reaction needs a body");

    desc_init(&d, "bad.mask", RX_FACULTY_AEGIS, SUBJ_SEAT, NULL, NULL);
    d.need.accelerator_features = RX_ACCEL_BLACKWELL;
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){ src, RX_FIELD(0) | RX_FIELD(1) };
    d.n_writes = 1;
    d.writes[0] = (RxDep){ src, RX_FIELD(1) };
    d.n_caps = 1;
    d.caps[0] = (RxCapNeed){ cap, RES_SRC, RX_RIGHT_READ | RX_RIGHT_WRITE };
    CHECK(rx_world_add_reaction(&e.w, &d, NULL) == RX_ERR_ARG,
          "the seat must not wake on the field it writes");

    desc_init(&d, "seat", RX_FACULTY_AEGIS, SUBJ_SEAT, fn_poison, NULL);
    d.need.accelerator_features = RX_ACCEL_BLACKWELL;
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){ src, RX_FIELD(0) };
    d.n_writes = 1;
    d.writes[0] = (RxDep){ src, RX_FIELD(1) };
    d.n_caps = 1;
    d.caps[0] = (RxCapNeed){ cap, RES_SRC, RX_RIGHT_READ | RX_RIGHT_WRITE };
    CHECK(rx_world_add_reaction(&e.w, &d, NULL) == RX_OK, "seat registration");
    env_stop(&e);
}

typedef struct {
    Env e;
    Chain ch;
    uint32_t seat;
    uint32_t dep;
    RxCapRef ext;
    RxCapRef seat_cap;
} Rig;

static int rig_start(Rig *r, int place) {
    memset(r, 0, sizeof(*r));
    g_poison = 0;
    if (env_start(&r->e) != 0) return -1;
    r->ch.src = mkobj(&r->e, RES_SRC);
    r->ch.out = mkobj(&r->e, RES_OUT);
    if (place && rx_world_attach_physical(&r->e.w, r->ch.src) != RX_OK) return -1;
    r->seat_cap = mint(&r->e, SUBJ_SEAT, RES_SRC, RX_RIGHT_READ | RX_RIGHT_WRITE);
    if (rx_world_bind_capability(&r->e.w, r->ch.src, r->seat_cap) != RX_OK) return -1;
    r->ext = mint(&r->e, SUBJ_EXTERNAL, RES_SRC, RX_RIGHT_WRITE);
    RxCapRef dep_r = mint(&r->e, SUBJ_DEPEND, RES_SRC, RX_RIGHT_READ);
    RxCapRef dep_w = mint(&r->e, SUBJ_DEPEND, RES_OUT, RX_RIGHT_WRITE);
    if (rx_world_set_resident_rule(&r->e.w, RULE_K) != RX_OK) return -1;

    RxReactionDesc d;
    desc_init(&d, "resident.seat", RX_FACULTY_AEGIS, SUBJ_SEAT, fn_poison, &r->ch);
    d.need.accelerator_features = RX_ACCEL_BLACKWELL;
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){ r->ch.src, RX_FIELD(0) };
    d.n_writes = 1;
    d.writes[0] = (RxDep){ r->ch.src, RX_FIELD(1) };
    d.n_caps = 1;
    d.caps[0] = (RxCapNeed){ r->seat_cap, RES_SRC, RX_RIGHT_READ | RX_RIGHT_WRITE };
    if (rx_world_add_reaction(&r->e.w, &d, &r->seat) != RX_OK) return -1;

    desc_init(&d, "cpu.dependent", RX_FACULTY_AEGIS, SUBJ_DEPEND, fn_depend, &r->ch);
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){ r->ch.src, RX_FIELD(1) };
    d.n_writes = 1;
    d.writes[0] = (RxDep){ r->ch.out, RX_FIELD(0) };
    d.n_caps = 2;
    d.caps[0] = (RxCapNeed){ dep_r, RES_SRC, RX_RIGHT_READ };
    d.caps[1] = (RxCapNeed){ dep_w, RES_OUT, RX_RIGHT_WRITE };
    if (rx_world_add_reaction(&r->e.w, &d, &r->dep) != RX_OK) return -1;
    return 0;
}

static int64_t poke(Rig *r, uint64_t value) {
    RxMutation m = { r->ch.src, 0, value };
    return rx_world_publish_external(&r->e.w, r->ext, &m, 1);
}

static void t_admission_and_wake(void) {
    printf("[*] admission then one world wake\n");
    Rig r;
    CHECK(rig_start(&r, 1) == 0, "setup");
    if (g_fail && r.e.w.n_workers == 0) return;
    offer(&r.e.w, 0);
    int64_t cause = poke(&r, STIMULUS);
    CHECK(cause > 0, "stimulus");
    CHECK(rx_world_wait_quiescent(&r.e.w, 1000) == RX_OK, "blocked seat did not settle");
    CHECK(r.e.w.reactions[r.seat].state == RX_BLOCKED_RESOURCE, "seat ran without the graphics budget");
    CHECK(r.e.w.reactions[r.seat].activations == 0, "blocked seat counted an activation");
    CHECK(r.e.w.reactions[r.seat].resident_seat == false, "a claim was posted without admission");
    CHECK(field_of(&r.e.w, r.ch.src, 1) == 0, "field 1 changed before the seat ran");
    CHECK(g_poison == 0, "the seat body was called as a function");

    offer(&r.e.w, RX_ACCEL_BLACKWELL);
    CHECK(wait_seat(&r.e.w, r.seat) == 0, "admitted seat did not reach the claim");
    int step = rx_resident_seat_step(&r.e.w);
    CHECK(step == 1, "stand-in step %d", step);
    CHECK(g_poison == 0, "stand-in called the seat body");
    CHECK(window_field(&r.e.w, r.ch.src.id, 1) == (STIMULUS ^ RULE_K),
          "window was not updated in place");
    CHECK(field_of(&r.e.w, r.ch.src, 1) == 0, "canonical field changed before accept");
    CHECK(rx_resident_accept(&r.e.w) == RX_OK, "accept");
    CHECK(rx_world_wait_quiescent(&r.e.w, 3000) == RX_OK, "dependent did not finish");

    uint64_t expect = (uint64_t)STIMULUS ^ RULE_K;
    CHECK(field_of(&r.e.w, r.ch.src, 0) == STIMULUS, "field 0 was rewritten");
    CHECK(field_of(&r.e.w, r.ch.src, 1) == expect, "canonical field 1");
    CHECK(field_of(&r.e.w, r.ch.out, 0) == expect + 1, "dependent did not observe the seat");
    CHECK(r.e.w.objects[r.ch.src.id].coherency == RX_COHERENCY_SEAT, "coherency");
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
        CHECK(rx_world_explain(&r.e.w, r.ch.src, 1) == seat_id, "field 1 writer");
        CHECK(rx_world_explain(&r.e.w, r.ch.out, 0) == dep_id, "result writer");
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

static void t_revoked(void) {
    printf("[*] revoked capability\n");
    Rig r;
    CHECK(rig_start(&r, 1) == 0, "setup");
    offer(&r.e.w, RX_ACCEL_BLACKWELL);
    CHECK(poke(&r, STIMULUS) > 0, "stimulus");
    CHECK(wait_seat(&r.e.w, r.seat) == 0, "claim was not posted");
    CHECK(rx_capadmin_revoke(&r.e.admin, rx_capadmin_office(&r.e.admin), r.seat_cap) == RX_CAP_OK,
          "revoke");
    int step = rx_resident_seat_step(&r.e.w);
    CHECK(step == RX_ERR_AUTHORITY, "revoked claim step %d", step);
    CHECK(window_field(&r.e.w, r.ch.src.id, 1) == 0, "revoked claim wrote the window");
    CHECK(rx_resident_accept(&r.e.w) == RX_ERR_STALE_GEN, "fault was published as success");
    CHECK(rx_world_wait_quiescent(&r.e.w, 1000) == RX_OK, "revoked seat did not settle");
    CHECK(field_of(&r.e.w, r.ch.src, 1) == 0, "canonical field changed");
    CHECK(field_of(&r.e.w, r.ch.out, 0) == 0, "dependent ran after a refused seat");
    CHECK(g_poison == 0, "poison body");
    env_stop(&r.e);
}

static void t_stale(void) {
    printf("[*] stale generation\n");
    Rig r;
    CHECK(rig_start(&r, 1) == 0, "setup");
    offer(&r.e.w, RX_ACCEL_BLACKWELL);
    CHECK(poke(&r, STIMULUS) > 0, "stimulus");
    CHECK(wait_seat(&r.e.w, r.seat) == 0, "claim was not posted");
    uint64_t off = r.e.w.objects[r.ch.src.id].region_offset;
    CHECK(rx_world_retire(&r.e.w, r.ch.src) == RX_OK, "retire");
    int step = rx_resident_seat_step(&r.e.w);
    CHECK(step == RX_ERR_STALE_GEN, "stale step %d", step);
    CHECK(load_u64(r.e.w.coherent + off + 8) == 0, "stale claim wrote field 1");
    CHECK(rx_resident_accept(&r.e.w) == RX_ERR_STALE_GEN, "stale accept");
    CHECK(rx_world_wait_quiescent(&r.e.w, 1000) == RX_OK, "stale seat did not settle");
    CHECK(field_of(&r.e.w, r.ch.out, 0) == 0, "dependent ran on a stale seat");
    CHECK(g_poison == 0, "poison body");
    env_stop(&r.e);
}

static void t_hostile_window(void) {
    printf("[*] hostile window\n");
    Rig r;
    CHECK(rig_start(&r, 1) == 0, "setup");
    offer(&r.e.w, RX_ACCEL_BLACKWELL);
    CHECK(poke(&r, STIMULUS) > 0, "stimulus");
    CHECK(wait_seat(&r.e.w, r.seat) == 0, "claim was not posted");
    uint8_t *win = r.e.w.coherent + r.e.w.objects[r.ch.src.id].region_offset;
    win[0] ^= 0xff;
    int step = rx_resident_seat_step(&r.e.w);
    CHECK(step == RX_ERR_TORN, "torn step %d", step);
    CHECK(rx_resident_accept(&r.e.w) == RX_ERR_STALE_GEN, "torn accept");
    CHECK(rx_world_wait_quiescent(&r.e.w, 1000) == RX_OK, "torn seat did not settle");
    CHECK(window_field(&r.e.w, r.ch.src.id, 0) == STIMULUS, "window was not restored");
    CHECK(window_field(&r.e.w, r.ch.src.id, 1) == 0, "torn claim published field 1");
    CHECK(field_of(&r.e.w, r.ch.out, 0) == 0, "dependent ran on a torn window");
    env_stop(&r.e);
}

static void t_unplaced(void) {
    printf("[*] unplaced object\n");
    Rig r;
    CHECK(rig_start(&r, 0) == 0, "setup");
    offer(&r.e.w, RX_ACCEL_BLACKWELL);
    CHECK(poke(&r, STIMULUS) > 0, "stimulus");
    CHECK(rx_world_wait_quiescent(&r.e.w, 2000) == RX_OK, "unplaced seat did not settle");
    CHECK(r.e.w.reactions[r.seat].commits == 0, "unplaced seat committed");
    CHECK(r.e.w.reactions[r.seat].resident_seat == false, "unplaced seat left a claim");
    CHECK(field_of(&r.e.w, r.ch.src, 1) == 0, "field 1");
    CHECK(field_of(&r.e.w, r.ch.out, 0) == 0, "dependent");
    CHECK(r.e.w.stats.invalidations > 0, "no invalidation");
    CHECK(g_poison == 0, "poison body");
    env_stop(&r.e);
}

static void t_own_authority(void) {
    printf("[*] this project's own authority on the same seat\n");
    Env e;
    if (env_start(&e) != 0) {
        CHECK(0, "world");
        return;
    }
    g_poison = 0;
    RxObjRef src = mkobj(&e, RES_SRC);
    RxObjRef out = mkobj(&e, RES_OUT);
    Chain ch = { src, out };
    RxCapRef forged = { 200, 7 };
    RxMutation bad = { src, 0, 5 };
    CHECK(rx_world_publish_external(&e.w, forged, &bad, 1) == RX_ERR_AUTHORITY, "forged publish");
    CHECK(field_of(&e.w, src, 0) == 0, "forged publish wrote the object");

    CHECK(rx_world_attach_physical(&e.w, src) == RX_OK, "attach");
    RxCapRef seat_cap = mint(&e, SUBJ_SEAT, RES_SRC, RX_RIGHT_READ | RX_RIGHT_WRITE);
    RxCapRef ext = mint(&e, SUBJ_EXTERNAL, RES_SRC, RX_RIGHT_WRITE);
    RxCapRef dep_r = mint(&e, SUBJ_DEPEND, RES_SRC, RX_RIGHT_READ);
    RxCapRef dep_w = mint(&e, SUBJ_DEPEND, RES_OUT, RX_RIGHT_WRITE);
    CHECK(rx_world_bind_capability(&e.w, src, seat_cap) == RX_OK, "bind");
    CHECK(rx_world_set_resident_rule(&e.w, RULE_K) == RX_OK, "rule");
    offer(&e.w, RX_ACCEL_BLACKWELL);

    RxReactionDesc d;
    uint32_t seat = 0, dep = 0;
    desc_init(&d, "resident.seat", RX_FACULTY_AEGIS, SUBJ_SEAT, fn_poison, &ch);
    d.need.accelerator_features = RX_ACCEL_BLACKWELL;
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){ src, RX_FIELD(0) };
    d.n_writes = 1;
    d.writes[0] = (RxDep){ src, RX_FIELD(1) };
    d.n_caps = 1;
    d.caps[0] = (RxCapNeed){ seat_cap, RES_SRC, RX_RIGHT_READ | RX_RIGHT_WRITE };
    CHECK(rx_world_add_reaction(&e.w, &d, &seat) == RX_OK, "seat");
    desc_init(&d, "cpu.dependent", RX_FACULTY_AEGIS, SUBJ_DEPEND, fn_depend, &ch);
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){ src, RX_FIELD(1) };
    d.n_writes = 1;
    d.writes[0] = (RxDep){ out, RX_FIELD(0) };
    d.n_caps = 2;
    d.caps[0] = (RxCapNeed){ dep_r, RES_SRC, RX_RIGHT_READ };
    d.caps[1] = (RxCapNeed){ dep_w, RES_OUT, RX_RIGHT_WRITE };
    CHECK(rx_world_add_reaction(&e.w, &d, &dep) == RX_OK, "dependent");

    RxMutation m = { src, 0, STIMULUS };
    CHECK(rx_world_publish_external(&e.w, ext, &m, 1) > 0, "stimulus");
    CHECK(wait_seat(&e.w, seat) == 0, "claim");
    CHECK(rx_resident_seat_step(&e.w) == 1, "step");
    CHECK(rx_resident_accept(&e.w) == RX_OK, "accept");
    CHECK(rx_world_wait_quiescent(&e.w, 3000) == RX_OK, "dependent");
    uint64_t expect = (uint64_t)STIMULUS ^ RULE_K;
    CHECK(field_of(&e.w, src, 1) == expect, "field 1");
    CHECK(field_of(&e.w, out, 0) == expect + 1, "dependent value");
    CHECK(g_poison == 0, "the seat was called as a function");
    env_stop(&e);
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
    t_revoked();
    t_stale();
    t_hostile_window();
    t_unplaced();
    t_own_authority();
    printf("checks %d failures %d\n", g_checks, g_fail);
    write_receipt();
    return g_fail ? 1 : 0;
}
