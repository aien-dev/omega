/*
 * R12 on the physical chip: one resident graphics seat inside the same world.
 *
 *   processor changes A -> seat reaction ready -> admission -> native
 *   authority -> resident seat claims -> seat reads A under the same
 *   {id, generation} -> qualified 32-bit add on the chip -> seat writes B
 *   -> B is published under the same rules as processor work -> the
 *   processor reaction waiting on B wakes and writes C.
 *
 * The seat is never called as a function. Authority is the native AIENOS
 * authority (C), not the Linux stand-in. silicon_observed stays false unless
 * this run saw the chip publish into the shared image.
 */
#include "runtime/aienos_cap.h"
#include "runtime/rx_world.h"
#include "runtime/rx_resident_gpu.h"
#include "omega_blackwell_codegen.h"
#include "omega_evidence.h"
#include "sha256.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define RES_A 0x10u
#define RES_B 0x20u
#define RES_C 0x30u
#define SENTINEL 0x11111111ull

enum { SUBJ_SEAT = 3, SUBJ_DEPEND = 4, SUBJ_EXTERNAL = 100, ISSUER = 3 };

static int g_checks;
static int g_fail;
static int g_silicon;

#define CHECK(cond, ...) do {                                            \
        g_checks++;                                                      \
        if (!(cond)) {                                                   \
            g_fail++;                                                    \
            fprintf(stderr, "  FAIL %s:%d ", __FILE__, __LINE__);        \
            fprintf(stderr, __VA_ARGS__);                                \
            fputc('\n', stderr);                                         \
        }                                                                \
    } while (0)

/* Scenario outcomes for the receipt. */
typedef struct {
    const char *name;
    int checks_before;
    int fails_before;
    int ran;
    int passed;
} Scenario;

static Scenario g_sc[16];
static int g_nsc;
static int g_cur = -1;

static void begin(const char *name) {
    printf("[*] %s\n", name);
    g_cur = g_nsc++;
    g_sc[g_cur] = (Scenario){ name, g_checks, g_fail, 1, 0 };
}

static void end(void) {
    if (g_cur < 0) return;
    g_sc[g_cur].passed = g_fail == g_sc[g_cur].fails_before;
    printf("    %s (%d checks)\n", g_sc[g_cur].passed ? "PASS" : "FAIL",
           g_checks - g_sc[g_cur].checks_before);
    g_cur = -1;
}

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void barrier(void) {
    atomic_thread_fence(memory_order_seq_cst);
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint64_t load_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static void store_u64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t field_of(RxWorld *w, RxObjRef r, uint32_t f) {
    RxObject o;
    if (rx_world_read(w, r, &o) != RX_OK) return UINT64_MAX;
    return o.field[f];
}

static OmegaSharedWorldRing *ring_at(RxWorld *w, uint64_t off) {
    return (OmegaSharedWorldRing *)(w->coherent + off);
}

static uint64_t g2c_tail(RxWorld *w) {
    barrier();
    return ring_at(w, rx_world_off_g2c())->tail;
}

static uint64_t c2g_head(RxWorld *w) {
    barrier();
    return ring_at(w, rx_world_off_c2g())->head;
}

static const uint8_t *window(RxWorld *w, RxObjRef r) {
    return w->coherent + w->objects[r.id].region_offset;
}

static int g_poison;

static int fn_poison(RxCtx *c) {
    (void)c;
    g_poison++;
    return -1;
}

typedef struct {
    RxObjRef b;
    RxObjRef c;
} Chain;

/* C = B + 1, on the processor. It only runs because B changed. */
static int fn_depend(RxCtx *c) {
    Chain *ch = c->user;
    const RxSnapshotDep *s = NULL;
    for (uint32_t i = 0; i < c->n_in; i++)
        if (c->in[i].obj.id == ch->b.id) s = &c->in[i];
    if (!s) return -1;
    c->out[c->n_out++] = (RxMutation){ ch->c, 0, s->field[0] + 1 };
    return 0;
}

static AienosCapAdmin *g_admin;

static RxCapRef mint(uint32_t subject, uint64_t resource, uint32_t rights) {
    AienosCapRef office;
    aienos_cap_office(g_admin, &office);
    AienosCapMint m = { ISSUER, subject, resource, rights, 0,
                        { UINT32_MAX, 0 }, office };
    AienosCapRef r = { UINT32_MAX, 0 };
    if (aienos_cap_mint(g_admin, &m, &r) != 0) r = (AienosCapRef){ UINT32_MAX, 0 };
    return (RxCapRef){ r.cap_id, r.generation };
}

static int revoke_cap(RxCapRef cap) {
    AienosCapRef office;
    aienos_cap_office(g_admin, &office);
    return aienos_cap_revoke(g_admin, office, (AienosCapRef){ cap.cap_id, cap.generation });
}

static void offer(RxWorld *w) {
    RxResourceBudget b;
    memset(&b, 0, sizeof(b));
    b.slots = 8;
    b.memory_bytes = UINT64_MAX;
    b.energy_budget = UINT64_MAX;
    b.offered_locality = UINT32_MAX;
    b.offered_accel = RX_ACCEL_BLACKWELL;
    b.compute_mask = UINT32_MAX;
    rx_world_set_resources(w, &b);
}

static void desc_init(RxReactionDesc *d, const char *name, uint32_t subject, RxFn fn, void *user) {
    memset(d, 0, sizeof(*d));
    d->name = name;
    d->faculty = RX_FACULTY_AEGIS;
    d->subject = subject;
    d->priority = RX_PRIO_FOREGROUND;
    d->fn = fn;
    d->user = user;
}

static int add_seat(RxWorld *w, RxObjRef a, RxObjRef b, RxCapRef in_cap, RxCapRef out_cap,
                    uint32_t *id) {
    RxReactionDesc d;
    desc_init(&d, "resident.seat.add", SUBJ_SEAT, fn_poison, NULL);
    d.need.accelerator_features = RX_ACCEL_BLACKWELL;
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){ a, RX_FIELD(0) | RX_FIELD(1) };
    d.n_writes = 1;
    d.writes[0] = (RxDep){ b, RX_FIELD(0) };
    d.n_caps = 2;
    d.caps[0] = (RxCapNeed){ in_cap, RES_A, RX_RIGHT_READ };
    d.caps[1] = (RxCapNeed){ out_cap, RES_B, RX_RIGHT_WRITE };
    return rx_world_add_reaction(w, &d, id);
}

/* Wait until this seat reaction has posted its claim. */
static int wait_claimed(RxWorld *w, uint32_t id) {
    for (int i = 0; i < 3000; i++) {
        RxState s = w->reactions[id].state;
        if (s == RX_RUNNING && w->reactions[id].resident_seat) return 0;
        if (s == RX_BLOCKED_RESOURCE || s == RX_BLOCKED_AUTHORITY || s == RX_FAILED ||
            s == RX_REJECTED)
            return 1;
        sleep_ms(1);
    }
    return -1;
}

/* Wait until the chip has put one more notice on its result ring. */
static int wait_chip(RxWorld *w, uint64_t before) {
    for (int i = 0; i < 3000; i++) {
        if (g2c_tail(w) > before) return 0;
        sleep_ms(1);
    }
    return -1;
}

static int poll_accept(RxWorld *w) {
    int acc = RX_ERR_NOT_FOUND;
    for (int i = 0; i < 3000; i++) {
        acc = rx_resident_accept(w);
        if (acc != RX_ERR_NOT_FOUND) return acc;
        sleep_ms(1);
    }
    return acc;
}

static int64_t stimulate(RxWorld *w, RxCapRef ext, RxObjRef a, uint64_t x, uint64_t y) {
    RxMutation m[2] = { { a, 0, x }, { a, 1, y } };
    return rx_world_publish_external(w, ext, m, 2);
}

static uint64_t last_commit_of(RxWorld *w, uint32_t reaction) {
    uint64_t found = 0;
    for (uint64_t id = 1; id <= w->n_crumbs; id++) {
        const RxCrumb *k = rx_world_crumb(w, id);
        if (k && k->kind == RX_CRUMB_COMMIT && k->reaction == reaction) found = id;
    }
    return found;
}

static uint64_t last_crumb_of(RxWorld *w, uint32_t reaction, RxCrumbKind kind) {
    uint64_t found = 0;
    for (uint64_t id = 1; id <= w->n_crumbs; id++) {
        const RxCrumb *k = rx_world_crumb(w, id);
        if (k && k->kind == kind && k->reaction == reaction) found = id;
    }
    return found;
}

/* ---- seat loss and channel reset ----------------------------------------- */

static volatile uint32_t *hb_word(RxWorld *w, uint32_t off) {
    return (volatile uint32_t *)(w->coherent + rx_world_off_heartbeat() + off);
}

/* The chip's pass counter stops moving once its channel is gone. */
static int chip_still(RxWorld *w, int ms) {
    barrier();
    uint32_t a = *hb_word(w, RX_SEAT_HB_LIVE);
    sleep_ms(ms);
    barrier();
    return *hb_word(w, RX_SEAT_HB_LIVE) == a;
}

static uint64_t crumbs_of(RxWorld *w, uint32_t reaction, RxCrumbKind kind, int reason,
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

static int seats_holding(RxWorld *w) {
    int n = 0;
    for (uint32_t i = 0; i < w->n_reactions; i++)
        if (w->reactions[i].resident_seat) n++;
    return n;
}

/* A fresh A -> seat -> B -> processor -> C chain for the loss scenarios. */
typedef struct {
    RxObjRef a, b, c;
    RxCapRef in, out, ext;
    uint32_t seat, dep;
    Chain ch;
} Rig;

static Rig g_rig;

static int rig_up(RxWorld *w, Rig *g, uint64_t res) {
    uint64_t zero[RX_MAX_FIELDS] = { 0 };
    if (rx_world_create(w, 1, RX_PERSIST_RESIDENT, res, zero, &g->a) != RX_OK ||
        rx_world_create(w, 1, RX_PERSIST_RESIDENT, res + 1, zero, &g->b) != RX_OK ||
        rx_world_create(w, 1, RX_PERSIST_RESIDENT, res + 2, zero, &g->c) != RX_OK ||
        rx_world_attach_physical(w, g->a) != RX_OK || rx_world_attach_physical(w, g->b) != RX_OK)
        return -1;
    g->in = mint(SUBJ_SEAT, res, RX_RIGHT_READ);
    g->out = mint(SUBJ_SEAT, res + 1, RX_RIGHT_READ | RX_RIGHT_WRITE);
    g->ext = mint(SUBJ_EXTERNAL, res, RX_RIGHT_WRITE);
    RxCapRef dr = mint(SUBJ_DEPEND, res + 1, RX_RIGHT_READ);
    RxCapRef dw = mint(SUBJ_DEPEND, res + 2, RX_RIGHT_WRITE);
    if (rx_world_bind_capability(w, g->a, g->in) != RX_OK ||
        rx_world_bind_capability(w, g->b, g->out) != RX_OK)
        return -1;
    RxReactionDesc d;
    desc_init(&d, "resident.seat.add", SUBJ_SEAT, fn_poison, NULL);
    d.need.accelerator_features = RX_ACCEL_BLACKWELL;
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){ g->a, RX_FIELD(0) | RX_FIELD(1) };
    d.n_writes = 1;
    d.writes[0] = (RxDep){ g->b, RX_FIELD(0) };
    d.n_caps = 2;
    d.caps[0] = (RxCapNeed){ g->in, res, RX_RIGHT_READ };
    d.caps[1] = (RxCapNeed){ g->out, res + 1, RX_RIGHT_WRITE };
    if (rx_world_add_reaction(w, &d, &g->seat) != RX_OK) return -1;
    g->ch = (Chain){ g->b, g->c };
    desc_init(&d, "cpu.dependent", SUBJ_DEPEND, fn_depend, &g->ch);
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){ g->b, RX_FIELD(0) };
    d.n_reads = 1;
    d.reads[0] = (RxDep){ g->b, RX_FIELD(0) };
    d.n_writes = 1;
    d.writes[0] = (RxDep){ g->c, RX_FIELD(0) };
    d.n_caps = 2;
    d.caps[0] = (RxCapNeed){ dr, res + 1, RX_RIGHT_READ };
    d.caps[1] = (RxCapNeed){ dw, res + 2, RX_RIGHT_WRITE };
    return rx_world_add_reaction(w, &d, &g->dep) == RX_OK ? 0 : -1;
}

/* Every claim ever posted has ended exactly once and nothing is held. */
static void check_settled(RxWorld *w, const char *when) {
    CHECK(w->stats.resident_claims == w->stats.resident_closed,
          "%s: claims %llu ended %llu", when, (unsigned long long)w->stats.resident_claims,
          (unsigned long long)w->stats.resident_closed);
    CHECK(seats_holding(w) == 0, "%s: a claim is still held", when);
    CHECK(w->used_slots == 0 && w->used_memory == 0 && w->used_energy == 0,
          "%s: a resource charge was not released (%u slots)", when, w->used_slots);
}

/* Takes every result as it lands. A result computed from an A that has
 * since moved is refused as torn; the rearmed activation runs again. */
typedef struct {
    RxWorld *w;
    atomic_int stop;
    uint64_t ok, stale, input_moved, other;
    int other_rc;
} Acceptor;

static void *acceptor_main(void *arg) {
    Acceptor *a = arg;
    while (!atomic_load(&a->stop)) {
        int rc = rx_resident_accept(a->w);
        if (rc == RX_OK) a->ok++;
        else if (rc == RX_ERR_STALE_GEN) a->stale++;
        else if (rc == RX_ERR_TORN) a->input_moved++;
        else if (rc == RX_ERR_NOT_FOUND) {
            struct timespec ts = { 0, 100000L };
            nanosleep(&ts, NULL);
        } else {
            a->other++;
            a->other_rc = rc;
        }
    }
    return NULL;
}

/* Keeps changing A for the whole scenario, including while a channel is
 * being torn down, so resets land on work in flight. */
typedef struct {
    RxWorld *w;
    RxCapRef ext;
    RxObjRef a;
    atomic_int stop;
    atomic_ullong want;
    uint64_t sent;
} Loader;

static void *loader_main(void *arg) {
    Loader *l = arg;
    for (uint64_t n = 0; !atomic_load(&l->stop); n++) {
        uint64_t x = 1000u + n, y = n % 7u;
        RxMutation m[2] = { { l->a, 0, x }, { l->a, 1, y } };
        if (rx_world_publish_external(l->w, l->ext, m, 2) > 0) {
            atomic_store(&l->want, (uint64_t)(uint32_t)(x + y));
            l->sent++;
        }
        /* Paced so the causal record holds the whole scenario. */
        struct timespec ts = { 0, 4000000L };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static uint64_t g_resets, g_resets_in_flight, g_claims_lost, g_kill_lost, g_kill_ms_max;

static uint32_t xs32(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
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
    if (omega_evidence_path("R12/rx_resident_silicon_receipt.json", path, sizeof path) != 0) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    char commit[41];
    memset(commit, 0, sizeof commit);
    if (!omega_evidence_run_commit(commit)) memcpy(commit, "unknown", 8);
    const char *candidate = getenv("OMEGA_CANDIDATE_COMMIT");
    const char *aienos = getenv("AIENOS_COMMIT");
    const char *physics = getenv("PHYSICS_COMMIT");
    int bound = candidate && candidate[0] && strcmp(candidate, commit) == 0 &&
                !omega_evidence_tree_dirty();
    int claimed = g_silicon && g_fail == 0 && bound;
    char digest[65];
    binary_digest(digest);
    struct utsname u;
    memset(&u, 0, sizeof u);
    uname(&u);
    fprintf(f,
            "{\n"
            "  \"schema\": \"AIEN_RX_R12_RESIDENT_SILICON_V3\",\n"
            "  \"run_id\": \"%s\",\n"
            "  \"candidate_commit\": %s%s%s,\n"
            "  \"candidate_bound\": %s,\n"
            "  \"run_commit\": \"%s\",\n"
            "  \"tree_dirty\": %s,\n"
            "  \"aienos_commit\": \"%s\",\n"
            "  \"physics_commit\": \"%s\",\n"
            "  \"authority\": \"native AIENOS capability authority (C); Linux stand-in not started\",\n"
            "  \"seat_body\": \"qualified 32-bit integer add (IADD3), A.field0 + A.field1 -> B.field0\",\n"
            "  \"checks\": %d,\n"
            "  \"failures\": %d,\n"
            "  \"silicon_observed\": %s,\n"
            "  \"claimed\": %s,\n"
            "  \"test_binary_sha256\": \"%s\",\n"
            "  \"host\": {\"sysname\": \"%s\", \"release\": \"%s\", \"machine\": \"%s\"},\n"
            "  \"hardware_scope\": \"one persistent graphics seat on the machine's own chip, same image as the processor\",\n"
            "  \"scenarios\": [\n",
            omega_evidence_run_id(), candidate ? "\"" : "", candidate ? candidate : "null",
            candidate ? "\"" : "", bound ? "true" : "false", commit,
            omega_evidence_tree_dirty() ? "true" : "false", aienos ? aienos : "unknown",
            physics ? physics : "unknown", g_checks, g_fail, g_silicon ? "true" : "false",
            claimed ? "true" : "false", digest, u.sysname, u.release, u.machine);
    for (int i = 0; i < g_nsc; i++)
        fprintf(f, "    {\"name\": \"%s\", \"result\": \"%s\"}%s\n", g_sc[i].name,
                g_sc[i].passed ? "PASS" : "FAIL", i + 1 < g_nsc ? "," : "");
    fprintf(f,
            "  ],\n"
            "  \"seat_loss\": {\"killed_holding_claims_lost\": %llu, \"channel_resets\": %llu, "
            "\"resets_with_work_in_flight\": %llu, \"claims_lost_and_retried\": %llu, "
            "\"longest_channel_teardown_ms\": %llu, "
            "\"kill_method\": \"host destroys the running channel group; not a chip-raised fault\"},\n"
            "  \"not_exercised\": [\"recovery from a chip-raised fault (MMU fault, Xid) rather than a "
            "host-initiated channel teardown\", \"seat lost before its first heartbeat\"],\n"
            "  \"gates\": {\n",
            (unsigned long long)g_kill_lost, (unsigned long long)g_resets,
            (unsigned long long)g_resets_in_flight, (unsigned long long)g_claims_lost,
            (unsigned long long)g_kill_ms_max);
    fprintf(f,
            "    \"R12_RESIDENT_SEAT\": \"%s\",\n"
            "    \"not_claimed\": [\"R8\", \"R10\", \"R11\", \"R13\"%s]\n"
            "  }\n"
            "}\n",
            claimed ? "PASS" : (g_silicon ? "SILICON_PASS_UNBOUND" : "NOT_CLAIMED"),
            claimed ? "" : ", \"R12\"");
    fclose(f);
    printf("receipt: %s\n", path);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[*] R12 resident seat on this machine's graphics chip\n");
    int fixtures = omega_blackwell_verify_codegen_fixtures();
    CHECK(fixtures == 0, "instruction checks %d", fixtures);

    AienosCapView *view = NULL;
    RxWorld w;
    memset(&w, 0, sizeof w);
    int auth_up = 0, world_up = 0, observed = 0;
    RxGpuSeat *seat = NULL;
    CHECK(aienos_cap_start(&g_admin, &view) == 0, "the native authority did not start");
    if (g_fail) goto done;
    auth_up = 1;
    CHECK(rx_world_init_native(&w, view, 2, 1u << 16) == RX_OK, "world did not start");
    if (g_fail) goto done;
    world_up = 1;
    w.external_subject = SUBJ_EXTERNAL;

    uint64_t zero[RX_MAX_FIELDS] = { 0 };
    RxObjRef A = { UINT32_MAX, 0 }, B = { UINT32_MAX, 0 }, C = { UINT32_MAX, 0 };
    CHECK(rx_world_create(&w, 1, RX_PERSIST_RESIDENT, RES_A, zero, &A) == RX_OK, "object A");
    CHECK(rx_world_create(&w, 1, RX_PERSIST_RESIDENT, RES_B, zero, &B) == RX_OK, "object B");
    CHECK(rx_world_create(&w, 1, RX_PERSIST_RESIDENT, RES_C, zero, &C) == RX_OK, "object C");
    CHECK(rx_world_attach_physical(&w, A) == RX_OK, "A has no window");
    CHECK(rx_world_attach_physical(&w, B) == RX_OK, "B has no window");
    Chain ch = { B, C };

    RxCapRef seat_in = mint(SUBJ_SEAT, RES_A, RX_RIGHT_READ);
    RxCapRef seat_out = mint(SUBJ_SEAT, RES_B, RX_RIGHT_READ | RX_RIGHT_WRITE);
    RxCapRef ext = mint(SUBJ_EXTERNAL, RES_A, RX_RIGHT_WRITE);
    RxCapRef dep_r = mint(SUBJ_DEPEND, RES_B, RX_RIGHT_READ);
    RxCapRef dep_w = mint(SUBJ_DEPEND, RES_C, RX_RIGHT_WRITE);
    CHECK(seat_in.cap_id != UINT32_MAX && seat_out.cap_id != UINT32_MAX &&
              ext.cap_id != UINT32_MAX && dep_r.cap_id != UINT32_MAX &&
              dep_w.cap_id != UINT32_MAX,
          "the native authority did not mint");
    CHECK(rx_world_bind_capability(&w, A, seat_in) == RX_OK, "A capability not bound");
    CHECK(rx_world_bind_capability(&w, B, seat_out) == RX_OK, "B capability not bound");
    CHECK(rx_world_enable_resident(&w) == RX_OK, "resident seat not enabled");
    offer(&w);

    RxCapRef forged = { 200, 7 };
    RxMutation bad = { A, 0, 5 };
    CHECK(rx_world_publish_external(&w, forged, &bad, 1) == RX_ERR_AUTHORITY,
          "a forged capability published");

    uint32_t seat_id = 0, dep_id = 0;
    CHECK(add_seat(&w, A, B, seat_in, seat_out, &seat_id) == RX_OK, "seat not registered");
    RxReactionDesc d;
    desc_init(&d, "cpu.dependent", SUBJ_DEPEND, fn_depend, &ch);
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){ B, RX_FIELD(0) };
    d.n_reads = 1;
    d.reads[0] = (RxDep){ B, RX_FIELD(0) };
    d.n_writes = 1;
    d.writes[0] = (RxDep){ C, RX_FIELD(0) };
    d.n_caps = 2;
    d.caps[0] = (RxCapNeed){ dep_r, RES_B, RX_RIGHT_READ };
    d.caps[1] = (RxCapNeed){ dep_w, RES_C, RX_RIGHT_WRITE };
    CHECK(rx_world_add_reaction(&w, &d, &dep_id) == RX_OK, "dependent not registered");
    if (g_fail) goto done;

    int began = rx_gpu_seat_begin(&w, &seat);
    CHECK(began == 0 && seat != NULL, "the graphics seat did not show a heartbeat");
    if (began != 0 || !seat) goto done;

    /* 1. The chain. */
    begin("chain A -> chip add -> B -> processor -> C");
    {
        int64_t cause = stimulate(&w, ext, A, 40, 2);
        CHECK(cause > 0, "stimulus was not published");
        uint64_t before = g2c_tail(&w);
        CHECK(wait_claimed(&w, seat_id) == 0, "the admitted seat did not post a claim");
        CHECK(wait_chip(&w, before) == 0, "the chip did not answer");
        barrier();
        uint32_t pick = *hb_word(&w, RX_SEAT_HB_T_PICK);
        uint32_t done_ns = *hb_word(&w, RX_SEAT_HB_T_DONE);
        uint32_t chip_ns = done_ns - pick; /* low 32-bit timer wraps naturally */
        CHECK(pick != 0 && chip_ns > 0 && chip_ns < 1000000000u,
              "chip timing stamps are invalid (pickup %u, duration %u ns)",
              pick, chip_ns);
        CHECK(g_poison == 0, "the seat was called as a function");
        CHECK(load_u64(window(&w, B)) == 42, "the chip did not write the sum into B's window");
        CHECK(field_of(&w, B, 0) == 0, "B changed before publication");
        int acc = poll_accept(&w);
        CHECK(acc == RX_OK, "the chip publication was not accepted (%d)", acc);
        observed = acc == RX_OK;
        CHECK(rx_world_wait_quiescent(&w, 3000) == RX_OK, "the processor dependent did not finish");
        CHECK(field_of(&w, A, 0) == 40 && field_of(&w, A, 1) == 2, "A changed");
        CHECK(field_of(&w, B, 0) == 42, "B is not A.field0 + A.field1");
        CHECK(field_of(&w, C, 0) == 43, "C did not observe B");
        CHECK(w.objects[B.id].coherency == RX_COHERENCY_SEAT, "B not marked from the seat");
        CHECK(w.objects[A.id].generation == A.generation &&
                  w.objects[B.id].generation == B.generation,
              "an identity moved");
        uint64_t sk_id = last_commit_of(&w, seat_id), dk_id = last_commit_of(&w, dep_id);
        CHECK(sk_id && dk_id, "the causal record is missing a commit");
        if (sk_id && dk_id) {
            const RxCrumb *sk = rx_world_crumb(&w, sk_id);
            const RxCrumb *dk = rx_world_crumb(&w, dk_id);
            CHECK(sk->worker == RX_SEAT_BLACKWELL, "the seat commit is not marked as the chip");
            CHECK(sk->wake_cause == (uint64_t)cause, "the seat cause is not the stimulus");
            CHECK(dk->wake_cause == sk_id, "C was not woken by the chip's publication");
            CHECK(rx_world_explain(&w, B, 0) == sk_id, "B has no chip writer");
            CHECK(rx_world_explain(&w, C, 0) == dk_id, "C has no dependent writer");
        }
        uint64_t checked = 0;
        CHECK(rx_world_verify_crumbs(&w, &checked) == 0, "the causal chain did not verify");
    }
    end();

    /* 2. The same completion delivered twice. */
    begin("duplicate completion is not published twice");
    {
        OmegaSharedWorldRing *g = ring_at(&w, rx_world_off_g2c());
        barrier();
        uint64_t t = g->tail;
        memcpy(&g->slots[t & g->mask], &g->slots[(t - 1) & g->mask], sizeof(OmegaSharedWorldDesc));
        barrier();
        g->tail = t + 1;
        barrier();
        uint64_t commits = w.reactions[seat_id].commits;
        uint64_t head_before = g->head;
        int acc = rx_resident_accept(&w);
        CHECK(acc != RX_OK, "a repeated completion was accepted");
        barrier();
        CHECK(g->head == head_before + 1, "the repeated completion was not consumed");
        CHECK(w.reactions[seat_id].commits == commits, "a repeated completion committed");
        CHECK(field_of(&w, B, 0) == 42 && field_of(&w, C, 0) == 43, "a repeat changed B or C");
    }
    end();

    /* 3. A torn result notice. */
    begin("torn result notice is refused and B is restored");
    {
        uint64_t before = g2c_tail(&w);
        CHECK(stimulate(&w, ext, A, 1000, 1) > 0, "stimulus");
        CHECK(wait_claimed(&w, seat_id) == 0, "no claim");
        CHECK(wait_chip(&w, before) == 0, "the chip did not answer");
        OmegaSharedWorldRing *g = ring_at(&w, rx_world_off_g2c());
        g->slots[before & g->mask].payload[12] ^= 0x40;
        barrier();
        int acc = poll_accept(&w);
        CHECK(acc == RX_ERR_TORN, "a torn notice was not refused (%d)", acc);
        CHECK(field_of(&w, B, 0) == 42, "a torn notice changed B");
        CHECK(load_u64(window(&w, B)) == 42, "B's window was not restored");
        CHECK(last_crumb_of(&w, seat_id, RX_CRUMB_REJECTED) != 0, "no rejection in the record");
        CHECK(rx_world_wait_quiescent(&w, 3000) == RX_OK, "world did not settle");
        CHECK(field_of(&w, C, 0) == 43, "C moved without a publication");
    }
    end();

    /* 4. The seat leaves and a new one starts; identity stays with the world. */
    begin("seat restart keeps identity and history");
    {
        uint64_t n_crumbs = w.n_crumbs;
        CHECK(rx_resident_shutdown(&w) == RX_OK, "shutdown was not posted");
        int finished = rx_gpu_seat_finish(seat);
        seat = NULL;
        CHECK(finished == 0, "the seat did not leave cleanly");
        CHECK(field_of(&w, A, 0) == 1000 && field_of(&w, B, 0) == 42 && field_of(&w, C, 0) == 43,
              "objects changed when the seat left");
        CHECK(w.objects[A.id].generation == A.generation &&
                  w.objects[B.id].generation == B.generation &&
                  w.objects[C.id].generation == C.generation,
              "an identity moved when the seat left");
        CHECK(w.n_crumbs >= n_crumbs, "history shrank");
        began = rx_gpu_seat_begin(&w, &seat);
        CHECK(began == 0 && seat, "the second seat did not start");
        if (began != 0 || !seat) goto done;
        uint64_t before = g2c_tail(&w);
        int64_t cause = stimulate(&w, ext, A, 7, 8);
        CHECK(cause > 0, "stimulus");
        CHECK(wait_claimed(&w, seat_id) == 0, "no claim");
        CHECK(wait_chip(&w, before) == 0, "the second seat did not answer");
        CHECK(poll_accept(&w) == RX_OK, "the second seat's publication was refused");
        CHECK(rx_world_wait_quiescent(&w, 3000) == RX_OK, "world did not settle");
        CHECK(field_of(&w, B, 0) == 15 && field_of(&w, C, 0) == 16,
              "the chain did not run again after restart");
        uint64_t checked = 0;
        CHECK(rx_world_verify_crumbs(&w, &checked) == 0, "history did not verify across restart");
    }
    end();

    /* 5. B's physical window goes away while the chip holds the claim. */
    begin("detached window refuses the publication");
    {
        uint64_t before = g2c_tail(&w);
        CHECK(stimulate(&w, ext, A, 20, 22) > 0, "stimulus");
        CHECK(wait_claimed(&w, seat_id) == 0, "no claim");
        CHECK(wait_chip(&w, before) == 0, "the chip did not answer");
        CHECK(rx_world_detach_physical(&w, B) == RX_OK, "B could not be detached");
        int acc = poll_accept(&w);
        CHECK(acc != RX_OK, "a publication into a detached window was accepted");
        CHECK(field_of(&w, B, 0) == 15, "B changed through a detached window");
        CHECK(rx_world_attach_physical(&w, B) == RX_OK, "B could not be attached again");
        CHECK(rx_world_bind_capability(&w, B, seat_out) == RX_OK, "B capability not rebound");
        CHECK(load_u64(window(&w, B)) == 15, "the new window does not show canonical B");
        CHECK(rx_world_wait_quiescent(&w, 3000) == RX_OK, "world did not settle");
        CHECK(field_of(&w, C, 0) == 16, "C moved without a publication");
    }
    end();

    /* 6. Authority is taken back after the claim, before publication. */
    begin("capability revoked after the claim is refused at publication");
    {
        uint64_t before = g2c_tail(&w);
        CHECK(stimulate(&w, ext, A, 100, 5) > 0, "stimulus");
        CHECK(wait_claimed(&w, seat_id) == 0, "no claim");
        CHECK(wait_chip(&w, before) == 0, "the chip did not answer");
        CHECK(load_u64(window(&w, B)) == 105, "the chip did not compute");
        CHECK(revoke_cap(seat_out) == 0, "the native authority did not revoke");
        int acc = poll_accept(&w);
        CHECK(acc == RX_ERR_AUTHORITY, "a revoked capability published (%d)", acc);
        CHECK(field_of(&w, B, 0) == 15, "B changed without authority");
        CHECK(load_u64(window(&w, B)) == 15, "B's window was not restored");
        CHECK(rx_world_wait_quiescent(&w, 3000) == RX_OK, "world did not settle");
        CHECK(field_of(&w, C, 0) == 16, "C moved without a publication");
        /* A fresh grant, a fresh seat registration. The revoked one stays blocked. */
        seat_out = mint(SUBJ_SEAT, RES_B, RX_RIGHT_READ | RX_RIGHT_WRITE);
        CHECK(rx_world_bind_capability(&w, B, seat_out) == RX_OK, "fresh capability not bound");
        CHECK(add_seat(&w, A, B, seat_in, seat_out, &seat_id) == RX_OK, "fresh seat not registered");
    }
    end();

    /* 7. A's generation moves while the chip holds the claim. */
    begin("generation moved in flight invalidates the chip's work");
    {
        uint64_t before = g2c_tail(&w);
        CHECK(stimulate(&w, ext, A, 3, 4) > 0, "stimulus");
        CHECK(wait_claimed(&w, seat_id) == 0, "no claim");
        CHECK(wait_chip(&w, before) == 0, "the chip did not answer");
        uint32_t old_gen = A.generation;
        CHECK(rx_world_retire(&w, A) == RX_OK, "A was not retired");
        CHECK(w.objects[A.id].generation != old_gen, "A's generation did not move");
        int acc = poll_accept(&w);
        CHECK(acc == RX_ERR_STALE_GEN, "work on an old generation published (%d)", acc);
        CHECK(field_of(&w, B, 0) == 15, "old-generation work changed B");
        CHECK(last_crumb_of(&w, seat_id, RX_CRUMB_INVALIDATED) != 0, "no invalidation in the record");
        CHECK(rx_world_wait_quiescent(&w, 3000) == RX_OK, "world did not settle");
    }
    end();

    /* 8. A claim naming the old generation, sent straight to the chip. */
    begin("stale generation claim is refused on the chip");
    {
        uint8_t *bw = w.coherent + w.objects[B.id].region_offset;
        store_u64(bw, SENTINEL);
        barrier();
        uint64_t head_before = c2g_head(&w);
        uint64_t before = g2c_tail(&w);
        OmegaSharedWorldDesc stale;
        memset(&stale, 0, sizeof stale);
        stale.msg_type = RX_RING_CLAIM;
        stale.object_id = A.id;
        stale.object_generation = A.generation;   /* the retired one */
        stale.object_length = 64;
        stale.payload_len = 40;
        /* The current seat generation: only the object generation is stale. */
        stale.producer_generation = w.seat_generation;
        for (int i = 0; i < 4; i++) {
            stale.payload[24 + i] = (uint8_t)(B.id >> (8 * i));
            stale.payload[28 + i] = (uint8_t)(w.objects[B.id].generation >> (8 * i));
        }
        rx_world_seal_descriptor(&stale);
        CHECK(rx_world_inject_descriptor(&w, &stale) == RX_OK, "the old claim was not posted");
        int moved = 0;
        for (int i = 0; i < 3000 && !moved; i++) {
            if (c2g_head(&w) > head_before) moved = 1;
            else sleep_ms(1);
        }
        CHECK(moved, "the chip did not consume the old claim");
        CHECK(wait_chip(&w, before) == 0, "the chip did not answer the old claim");
        OmegaSharedWorldRing *g = ring_at(&w, rx_world_off_g2c());
        uint16_t msg = g->slots[before & g->mask].msg_type;
        CHECK(msg == RX_RING_FAULT, "the chip did not answer with a fault (%u)", msg);
        CHECK(load_u64(bw) == SENTINEL, "an old generation changed B's window");
        barrier();
        g->head = g->tail;   /* drain the fault notice; there is no reaction for it */
        barrier();
    }
    end();

    /* 9. The seat is killed while it holds a claim. */
    begin("seat killed holding a claim: recorded, restored, released once, retried");
    {
        Rig *g = &g_rig;
        CHECK(rig_up(&w, g, 0x40) == 0, "the second chain was not built");
        if (g_fail) goto done;
        uint32_t gen0 = w.seat_generation;
        uint32_t ga = g->a.generation, gb = g->b.generation, gc = g->c.generation;
        *hb_word(&w, RX_SEAT_HB_HOLD) = 1;
        barrier();
        uint64_t before = g2c_tail(&w);
        int64_t cause = stimulate(&w, g->ext, g->a, 30, 12);
        CHECK(cause > 0, "stimulus");
        CHECK(wait_claimed(&w, g->seat) == 0, "no claim");
        uint64_t seq = w.reactions[g->seat].resident_seq;
        int held = 0;
        for (int i = 0; i < 3000 && !held; i++) {
            barrier();
            if (*hb_word(&w, RX_SEAT_HB_HELD) == (uint32_t)(seq + 1)) held = 1;
            else sleep_ms(1);
        }
        CHECK(held, "the chip did not reach the hold with this claim");
        CHECK(load_u64(window(&w, g->b)) == 42, "the chip did not write B's window before the hold");
        CHECK(g2c_tail(&w) == before, "the chip posted a result while holding");
        CHECK(!chip_still(&w, 20), "the holding seat is not alive");
        OmegaSharedWorldRing *c2g = ring_at(&w, rx_world_off_c2g());
        OmegaSharedWorldDesc old = c2g->slots[seq & c2g->mask];
        uint64_t fails0 = w.stats.transitions[RX_RUNNING][RX_FAILED];

        CHECK(rx_gpu_seat_kill(seat) == 0, "the channel was not destroyed");
        CHECK(chip_still(&w, 50), "the chip still runs after its channel was destroyed");
        int lost = rx_resident_seat_lost(&w, 1);
        g_kill_lost = lost > 0 ? (uint64_t)lost : 0;
        CHECK(lost == 1, "the held claim was not declared lost (%d)", lost);
        CHECK(w.seat_generation == gen0 + 1, "the seat generation did not move");
        CHECK(w.stats.transitions[RX_RUNNING][RX_FAILED] == fails0 + 1, "a claim failed twice");
        CHECK(load_u64(window(&w, g->b)) == 0, "B's window still holds the dead seat's sum");
        CHECK(field_of(&w, g->b, 0) == 0 && field_of(&w, g->c, 0) == 0, "B or C moved");
        uint64_t fail_id = 0;
        CHECK(crumbs_of(&w, g->seat, RX_CRUMB_FAILED, RX_ERR_SEAT_LOST, &fail_id) == 1,
              "no single seat-lost crumb");
        if (fail_id) {
            const RxCrumb *k = rx_world_crumb(&w, fail_id);
            CHECK(k->wake_cause == (uint64_t)cause && k->worker == RX_SEAT_BLACKWELL,
                  "the loss does not name the stimulus and the chip");
        }
        int finished = rx_gpu_seat_finish(seat);
        seat = NULL;
        CHECK(finished == 0, "the killed seat did not return the image");

        began = rx_gpu_seat_begin(&w, &seat);
        CHECK(began == 0 && seat, "a new seat did not start after the loss");
        if (began != 0 || !seat) goto done;
        CHECK(wait_claimed(&w, g->seat) == 0, "the lost work was not retried");
        CHECK(w.reactions[g->seat].resident_parent == fail_id, "the retry is not caused by the loss");
        int acc = poll_accept(&w);
        CHECK(acc == RX_OK, "the retried result was refused (%d)", acc);
        CHECK(rx_world_wait_quiescent(&w, 3000) == RX_OK, "world did not settle");
        CHECK(field_of(&w, g->b, 0) == 42 && field_of(&w, g->c, 0) == 43,
              "the retried work did not reach B and C");
        uint64_t commit_id = 0;
        CHECK(crumbs_of(&w, g->seat, RX_CRUMB_COMMIT, 0, &commit_id) == 1 &&
                  w.reactions[g->seat].commits == 1,
              "the work was not published exactly once");
        if (commit_id)
            CHECK(rx_world_crumb(&w, commit_id)->wake_cause == fail_id,
                  "the publication is not caused by the loss");
        CHECK(rx_world_validate_cap(&w, g->in, SUBJ_SEAT, 0x40, RX_RIGHT_READ, NULL) == RX_CAP_OK &&
                  rx_world_validate_cap(&w, g->out, SUBJ_SEAT, 0x41, RX_RIGHT_WRITE, NULL) ==
                      RX_CAP_OK,
              "the authority lost a grant across the loss");
        CHECK(w.objects[g->a.id].generation == ga && w.objects[g->b.id].generation == gb &&
                  w.objects[g->c.id].generation == gc,
              "an object generation moved");

        /* The dead seat's claim, posted again: the new seat refuses it. */
        uint64_t before2 = g2c_tail(&w);
        CHECK(rx_world_inject_descriptor(&w, &old) == RX_OK, "the old claim was not posted");
        CHECK(wait_chip(&w, before2) == 0, "the new seat did not answer the old claim");
        OmegaSharedWorldRing *g2 = ring_at(&w, rx_world_off_g2c());
        CHECK(g2->slots[before2 & g2->mask].msg_type == RX_RING_FAULT,
              "the new seat took the old claim");
        CHECK(load_u64(window(&w, g->b)) == 42, "the old claim changed B's window");
        CHECK(poll_accept(&w) == RX_ERR_STALE_GEN, "the old seat's generation was accepted");
        CHECK(w.reactions[g->seat].commits == 1 && field_of(&w, g->c, 0) == 43,
              "the old claim published");
        check_settled(&w, "after the kill");
        uint64_t checked = 0;
        CHECK(rx_world_verify_crumbs(&w, &checked) == 0, "history did not verify across the kill");
    }
    end();

    /* 10. The channel is destroyed and rebuilt again and again while claims
     * flow. The image stays in place; the seat is launched again on it. */
    begin("channel reset under load");
    {
        Rig *g = &g_rig;
        uint32_t gen0 = w.seat_generation;
        uint32_t ga = g->a.generation, gb = g->b.generation, gc = g->c.generation;
        uint64_t commits0 = w.reactions[g->seat].commits;
        Acceptor ac;
        memset(&ac, 0, sizeof ac);
        ac.w = &w;
        Loader ld;
        memset(&ld, 0, sizeof ld);
        ld.w = &w;
        ld.ext = g->ext;
        ld.a = g->a;
        pthread_t at, lt;
        CHECK(pthread_create(&at, NULL, acceptor_main, &ac) == 0, "acceptor did not start");
        CHECK(pthread_create(&lt, NULL, loader_main, &ld) == 0, "load did not start");
        uint32_t rng = 0x5eed1234u;
        const int rounds = 8;
        uint64_t held_rounds = 0, held_in_flight = 0;
        for (int i = 0; i < rounds && !g_fail; i++) {
            struct timespec d = { 0, (long)(20 + xs32(&rng) % 180u) * 1000000L };
            nanosleep(&d, NULL);
            /* Even rounds: the chip holds whatever claim it takes next, so
             * the reset lands on it. Odd rounds: nothing is arranged. */
            int held = (i % 2) == 0;
            if (held) {
                *hb_word(&w, RX_SEAT_HB_HOLD) = 1;
                barrier();
                held_rounds++;
                sleep_ms(20);
            }
            struct timespec k0, k1;
            clock_gettime(CLOCK_MONOTONIC, &k0);
            CHECK(rx_gpu_seat_kill(seat) == 0, "round %d: the channel was not destroyed", i);
            clock_gettime(CLOCK_MONOTONIC, &k1);
            uint64_t ms = (uint64_t)(k1.tv_sec - k0.tv_sec) * 1000u +
                          (uint64_t)((k1.tv_nsec - k0.tv_nsec) / 1000000L);
            if (ms > g_kill_ms_max) g_kill_ms_max = ms;
            CHECK(chip_still(&w, 5), "round %d: the chip still runs after the reset", i);
            int lost = rx_resident_seat_lost(&w, 1);
            CHECK(lost >= 0, "round %d: recovery failed", i);
            g_resets++;
            if (lost > 0) {
                g_resets_in_flight++;
                g_claims_lost += (uint64_t)lost;
                if (held) held_in_flight++;
            }
            CHECK(rx_gpu_seat_relaunch(seat) == 0, "round %d: the seat did not come back", i);
        }
        atomic_store(&ld.stop, 1);
        pthread_join(lt, NULL);
        CHECK(rx_world_wait_quiescent(&w, 5000) == RX_OK, "world did not settle after the load");
        atomic_store(&ac.stop, 1);
        pthread_join(at, NULL);
        uint64_t want = atomic_load(&ld.want);
        printf("    resets %llu (held %llu), with work in flight %llu, claims lost and retried %llu,\n"
               "    changes %llu, published %llu, refused as input moved %llu, stale refused %llu,\n"
               "    longest channel teardown %llu ms\n",
               (unsigned long long)g_resets, (unsigned long long)held_rounds,
               (unsigned long long)g_resets_in_flight, (unsigned long long)g_claims_lost,
               (unsigned long long)ld.sent, (unsigned long long)ac.ok,
               (unsigned long long)ac.input_moved, (unsigned long long)ac.stale,
               (unsigned long long)g_kill_ms_max);
        CHECK(g_resets == (uint64_t)rounds, "not every round reset the channel");
        CHECK(held_in_flight == held_rounds, "a held round found no claim in flight");
        CHECK(ac.other == 0, "the acceptor saw %llu unexpected refusals (last %d)",
              (unsigned long long)ac.other, ac.other_rc);
        CHECK(field_of(&w, g->b, 0) == want && field_of(&w, g->c, 0) == want + 1,
              "after the load B=%llu C=%llu want %llu", (unsigned long long)field_of(&w, g->b, 0),
              (unsigned long long)field_of(&w, g->c, 0), (unsigned long long)want);
        check_settled(&w, "after the resets");
        CHECK(w.seat_generation == gen0 + (uint32_t)rounds,
              "the seat generation did not follow the resets");
        CHECK(w.objects[g->a.id].generation == ga && w.objects[g->b.id].generation == gb &&
                  w.objects[g->c.id].generation == gc,
              "an object generation moved under resets");
        /* Each loss is published at most once. */
        for (uint64_t id = 1; id <= w.n_crumbs; id++) {
            const RxCrumb *k = rx_world_crumb(&w, id);
            if (!k || k->reaction != g->seat || k->kind != RX_CRUMB_FAILED ||
                k->reason != RX_ERR_SEAT_LOST)
                continue;
            uint64_t n = 0;
            for (uint64_t j = id + 1; j <= w.n_crumbs; j++) {
                const RxCrumb *c = rx_world_crumb(&w, j);
                if (c && c->reaction == g->seat && c->kind == RX_CRUMB_COMMIT && c->wake_cause == id)
                    n++;
            }
            CHECK(n <= 1, "a lost claim was published %llu times", (unsigned long long)n);
        }
        CHECK(w.reactions[g->seat].commits - commits0 == ac.ok,
              "commits and accepted results differ");
        CHECK(w.stats.crumb_overflow == 0, "the causal record overflowed");
        CHECK(rx_world_explain(&w, g->c, 0) == last_commit_of(&w, g->dep), "C's writer");
        uint64_t checked = 0;
        CHECK(rx_world_verify_crumbs(&w, &checked) == 0, "history did not verify under resets");
    }
    end();

    /* Regression (R14). The seat reads the world's projected table, which has
     * RX_MAX_OBJECTS entries. It refused every claim on an object id of 64 or
     * more (the frozen ABI's OMEGA_SW_MAX_OBJECTS), while the processor
     * stand-in accepted them; R14's GPU lanes found it. */
    begin("objects past id 64 are claimed on the chip");
    {
        static Rig hi;
        uint64_t zero[RX_MAX_FIELDS] = { 0 };
        RxObjRef filler;
        uint32_t made = 0;
        while (w.n_reactions && made < RX_MAX_OBJECTS &&
               rx_world_create(&w, 1, RX_PERSIST_RESIDENT, 0x70, zero, &filler) == RX_OK &&
               filler.id < 200)
            made++;
        CHECK(rig_up(&w, &hi, 0x80) == 0, "the high chain was not built");
        CHECK(hi.a.id >= 64 && hi.b.id >= 64, "the high chain has ids %u and %u", hi.a.id, hi.b.id);
        if (g_fail) goto done;
        int64_t cause = stimulate(&w, hi.ext, hi.a, 1000, 24);
        CHECK(cause > 0, "stimulus");
        uint64_t before = g2c_tail(&w);
        CHECK(wait_claimed(&w, hi.seat) == 0, "no claim on the high chain");
        CHECK(wait_chip(&w, before) == 0, "the chip did not answer");
        OmegaSharedWorldRing *g = ring_at(&w, rx_world_off_g2c());
        CHECK(g->slots[before & g->mask].msg_type == RX_RING_PUBLISH,
              "the chip refused a claim on object %u (answer 0x%x)", hi.a.id,
              g->slots[before & g->mask].msg_type);
        int acc = poll_accept(&w);
        CHECK(acc == RX_OK, "the high publication was refused (%d)", acc);
        CHECK(rx_world_wait_quiescent(&w, 3000) == RX_OK, "settle");
        CHECK(field_of(&w, hi.b, 0) == 1024 && field_of(&w, hi.c, 0) == 1025,
              "B=%llu C=%llu", (unsigned long long)field_of(&w, hi.b, 0),
              (unsigned long long)field_of(&w, hi.c, 0));
        check_settled(&w, "after the high chain");
    }
    end();

    begin("seat leaves on request");
    {
        CHECK(rx_resident_shutdown(&w) == RX_OK, "shutdown was not posted");
        int finished = rx_gpu_seat_finish(seat);
        seat = NULL;
        CHECK(finished == 0, "the seat did not leave cleanly");
        CHECK(g_poison == 0, "the seat was called as a function");
        CHECK(w.stats.illegal_transitions == 0, "a reaction took an illegal step");
    }
    end();
    if (g_fail == 0 && observed) g_silicon = 1;

done:
    if (seat) rx_gpu_seat_finish(seat);
    if (world_up) rx_world_destroy(&w);
    if (auth_up) aienos_cap_stop(g_admin, view);
    printf("checks %d failures %d silicon %d\n", g_checks, g_fail, g_silicon);
    write_receipt();
    return g_fail ? 1 : 0;
}
