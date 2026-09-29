/*
 * One launch of the resident graphics seat on the physical chip.
 * The processor never calls the seat as a function. Both sides use the
 * same image. A host-only stand-in is not started. silicon_observed stays
 * false unless this run saw the chip publish into that image.
 */
#include "runtime/rx_caproot.h"
#include "runtime/rx_world.h"
#include "runtime/rx_resident_gpu.h"
#include "omega_blackwell_codegen.h"
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

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
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

static uint64_t c2g_head(RxWorld *w) {
    uint64_t v = 0;
    memcpy(&v, w->coherent + rx_world_off_c2g() + 64, sizeof v);
    return v;
}

static uint32_t heartbeat(RxWorld *w) {
    uint32_t v = 0;
    memcpy(&v, w->coherent + rx_world_off_heartbeat(), sizeof v);
    return v;
}

static uint32_t hb_word(RxWorld *w, uint32_t byte_off) {
    uint32_t v = 0;
    memcpy(&v, w->coherent + rx_world_off_heartbeat() + byte_off, sizeof v);
    return v;
}

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

static RxCapRef mint(RxCapAdmin *admin, uint32_t subject, uint64_t resource, uint32_t rights) {
    RxCapMint m;
    memset(&m, 0, sizeof(m));
    m.issuer = ISSUER;
    m.subject = subject;
    m.resource = resource;
    m.rights = rights;
    m.parent = (RxCapRef){ UINT32_MAX, 0 };
    m.authority = rx_capadmin_office(admin);
    RxCapRef r = { UINT32_MAX, 0 };
    if (rx_capadmin_mint(admin, &m, &r) != RX_CAP_OK) r = (RxCapRef){ UINT32_MAX, 0 };
    return r;
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

static int wait_seat(RxWorld *w, uint32_t id) {
    for (int i = 0; i < 3000; i++) {
        if (w->reactions[id].state == RX_RUNNING && w->reactions[id].resident_seat) return 0;
        RxState s = w->reactions[id].state;
        if (s == RX_BLOCKED_RESOURCE || s == RX_BLOCKED_AUTHORITY || s == RX_FAILED ||
            s == RX_REJECTED || s == RX_DORMANT)
            return 1;
        sleep_ms(1);
    }
    return -1;
}

static int poll_accept(RxWorld *w) {
    int acc = RX_ERR_NOT_FOUND;
    for (int i = 0; i < 4000; i++) {
        acc = rx_resident_accept(w);
        if (acc != RX_ERR_NOT_FOUND) return acc;
        sleep_ms(1);
    }
    return acc;
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
            "  \"schema\": \"AIEN_RX_R12_RESIDENT_SILICON_V1\",\n"
            "  \"run_id\": \"%s\",\n"
            "  \"candidate_commit\": %s%s%s,\n"
            "  \"candidate_bound\": %s,\n"
            "  \"run_commit\": \"%s\",\n"
            "  \"tree_dirty\": %s,\n"
            "  \"checks\": %d,\n"
            "  \"failures\": %d,\n"
            "  \"silicon_observed\": %s,\n"
            "  \"claimed\": %s,\n"
            "  \"test_binary_sha256\": \"%s\",\n"
            "  \"host\": {\"sysname\": \"%s\", \"release\": \"%s\", \"machine\": \"%s\"},\n"
            "  \"hardware_scope\": \"one persistent graphics seat on the machine's own chip, same image as the processor\",\n"
            "  \"gates\": {\n"
            "    \"R12_RESIDENT_SEAT\": \"%s\",\n"
            "    \"not_claimed\": [\"R8\", \"R10\", \"R11\", \"R13\"%s]\n"
            "  }\n"
            "}\n",
            omega_evidence_run_id(), candidate ? "\"" : "", candidate ? candidate : "null",
            candidate ? "\"" : "", bound ? "true" : "false", commit,
            omega_evidence_tree_dirty() ? "true" : "false", g_checks, g_fail,
            g_silicon ? "true" : "false", claimed ? "true" : "false", digest, u.sysname, u.release,
            u.machine, claimed ? "PASS" : (g_silicon ? "SILICON_PASS_UNBOUND" : "NOT_CLAIMED"),
            claimed ? "" : ", \"R12\"");
    fclose(f);
    printf("receipt: %s\n", path);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    printf("[*] graphics seat on this machine\n");
    int fixtures = omega_blackwell_verify_codegen_fixtures();
    CHECK(fixtures == 0, "instruction checks %d", fixtures);

    RxCapRoot root;
    RxCapAdmin admin;
    RxWorld w;
    memset(&root, 0, sizeof root);
    memset(&admin, 0, sizeof admin);
    memset(&w, 0, sizeof w);
    int root_up = 0;
    int world_up = 0;
    RxGpuSeat *seat = NULL;
    CHECK(rx_caproot_start(&root, &admin) == RX_CAP_OK, "this project's authority did not start");
    if (g_fail) goto done;
    root_up = 1;
    CHECK(rx_world_init(&w, &root, 2, 1u << 16) == RX_OK, "world did not start");
    if (g_fail) goto done;
    world_up = 1;
    w.external_subject = SUBJ_EXTERNAL;

    uint64_t init[RX_MAX_FIELDS] = { 0 };
    RxObjRef src = { UINT32_MAX, 0 };
    RxObjRef out = { UINT32_MAX, 0 };
    Chain ch;
    CHECK(rx_world_create(&w, 1, RX_PERSIST_RESIDENT, RES_SRC, init, &src) == RX_OK, "source object");
    CHECK(rx_world_create(&w, 1, RX_PERSIST_RESIDENT, RES_OUT, init, &out) == RX_OK, "result object");
    ch.src = src;
    ch.out = out;
    RxCapRef forged = { 200, 7 };
    RxMutation bad = { src, 0, 5 };
    CHECK(rx_world_publish_external(&w, forged, &bad, 1) == RX_ERR_AUTHORITY,
          "a forged capability published");
    CHECK(field_of(&w, src, 0) == 0, "a forged publish changed the object");

    CHECK(rx_world_attach_physical(&w, src) == RX_OK, "the object has no window");
    RxCapRef seat_cap = mint(&admin, SUBJ_SEAT, RES_SRC, RX_RIGHT_READ | RX_RIGHT_WRITE);
    RxCapRef ext = mint(&admin, SUBJ_EXTERNAL, RES_SRC, RX_RIGHT_WRITE);
    RxCapRef dep_r = mint(&admin, SUBJ_DEPEND, RES_SRC, RX_RIGHT_READ);
    RxCapRef dep_w = mint(&admin, SUBJ_DEPEND, RES_OUT, RX_RIGHT_WRITE);
    CHECK(rx_world_bind_capability(&w, src, seat_cap) == RX_OK, "capability was not bound");
    CHECK(rx_world_set_resident_rule(&w, RULE_K) == RX_OK, "rule was not fixed");
    offer(&w);

    RxReactionDesc d;
    uint32_t seat_id = 0, dep_id = 0;
    desc_init(&d, "resident.seat", SUBJ_SEAT, fn_poison, &ch);
    d.need.accelerator_features = RX_ACCEL_BLACKWELL;
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){ src, RX_FIELD(0) };
    d.n_writes = 1;
    d.writes[0] = (RxDep){ src, RX_FIELD(1) };
    d.n_caps = 1;
    d.caps[0] = (RxCapNeed){ seat_cap, RES_SRC, RX_RIGHT_READ | RX_RIGHT_WRITE };
    CHECK(rx_world_add_reaction(&w, &d, &seat_id) == RX_OK, "seat was not registered");
    desc_init(&d, "cpu.dependent", SUBJ_DEPEND, fn_depend, &ch);
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){ src, RX_FIELD(1) };
    d.n_writes = 1;
    d.writes[0] = (RxDep){ out, RX_FIELD(0) };
    d.n_caps = 2;
    d.caps[0] = (RxCapNeed){ dep_r, RES_SRC, RX_RIGHT_READ };
    d.caps[1] = (RxCapNeed){ dep_w, RES_OUT, RX_RIGHT_WRITE };
    CHECK(rx_world_add_reaction(&w, &d, &dep_id) == RX_OK, "dependent was not registered");
    if (g_fail) goto done;

    int began = rx_gpu_seat_begin(&w, &seat);
    CHECK(began == 0 && seat != NULL, "the graphics seat did not show a heartbeat");
    if (began != 0 || !seat) goto done;
    printf("  heartbeat at launch %u\n", heartbeat(&w));
    sleep_ms(200);
    printf("  heartbeat after 200ms %u head %llu\n", heartbeat(&w),
           (unsigned long long)c2g_head(&w));
    printf("  [DEBUG-r12a] stage %u tail %u head %u give %u flag %u\n",
           hb_word(&w, 8), hb_word(&w, 12), hb_word(&w, 16), hb_word(&w, 20),
           hb_word(&w, 4));

    RxMutation m = { src, 0, STIMULUS };
    int64_t cause = rx_world_publish_external(&w, ext, &m, 1);
    CHECK(cause > 0, "stimulus was not published");
    CHECK(wait_seat(&w, seat_id) == 0, "the admitted seat did not post a claim");
    CHECK(g_poison == 0, "the seat was called as a function");
    int acc = poll_accept(&w);
    {
        uint64_t win_off = w.objects[src.id].region_offset;
        uint64_t gt = 0, ct = 0;
        memcpy(&ct, w.coherent + rx_world_off_c2g(), sizeof ct);
        memcpy(&gt, w.coherent + rx_world_off_g2c(), sizeof gt);
        printf("  [DEBUG-r12c] accept %d c2g head %llu tail %llu g2c tail %llu window0 %llu window1 %llu flag %u hb %u\n",
               acc, (unsigned long long)c2g_head(&w), (unsigned long long)ct,
               (unsigned long long)gt, (unsigned long long)load_u64(w.coherent + win_off),
               (unsigned long long)load_u64(w.coherent + win_off + 8),
               hb_word(&w, 4), heartbeat(&w));
        printf("  [DEBUG-r12c] gpu saw tail %u head %u\n", hb_word(&w, 8), hb_word(&w, 12));
    }
    CHECK(acc == RX_OK, "the graphics publication was not accepted (%d)", acc);
    CHECK(g_poison == 0, "accept called the seat as a function");
    CHECK(rx_world_wait_quiescent(&w, 3000) == RX_OK, "the processor dependent did not finish");

    uint64_t expect = (uint64_t)STIMULUS ^ RULE_K;
    CHECK(field_of(&w, src, 0) == STIMULUS, "field 0 changed");
    CHECK(field_of(&w, src, 1) == expect, "the graphics seat did not publish field 1");
    CHECK(field_of(&w, out, 0) == expect + 1, "the processor dependent did not observe the seat");
    CHECK(w.objects[src.id].coherency == RX_COHERENCY_SEAT, "the publication was not marked from the seat");
    CHECK(w.reactions[seat_id].commits == 1, "seat commits");
    CHECK(w.reactions[dep_id].commits == 1, "dependent commits");

    uint64_t seat_crumb = 0, dep_crumb = 0;
    for (uint64_t id = 1; id <= w.n_crumbs; id++) {
        const RxCrumb *k = rx_world_crumb(&w, id);
        if (!k || k->kind != RX_CRUMB_COMMIT) continue;
        if (k->worker == RX_SEAT_BLACKWELL) seat_crumb = id;
        if (k->reaction == dep_id) dep_crumb = id;
    }
    CHECK(seat_crumb != 0 && dep_crumb != 0, "the causal record is missing a commit");
    if (seat_crumb && dep_crumb) {
        const RxCrumb *sk = rx_world_crumb(&w, seat_crumb);
        const RxCrumb *dk = rx_world_crumb(&w, dep_crumb);
        CHECK(sk->wake_cause == (uint64_t)cause, "the seat cause is not the stimulus");
        CHECK(dk->wake_cause == seat_crumb, "the dependent was not woken by the seat");
        CHECK(rx_world_explain(&w, src, 1) == seat_crumb, "field 1 has no seat writer");
        CHECK(rx_world_explain(&w, out, 0) == dep_crumb, "the result has no dependent writer");
    }
    uint64_t checked = 0;
    CHECK(rx_world_verify_crumbs(&w, &checked) == 0, "the causal chain did not verify");
    CHECK(w.stats.illegal_transitions == 0, "a reaction took an illegal step");

    uint32_t old_gen = src.generation;
    uint64_t win_off = w.objects[src.id].region_offset;
    CHECK(rx_world_retire(&w, src) == RX_OK, "the object was not retired");
    uint8_t *win = w.coherent + win_off;
    store_u64(win + 8, SENTINEL);
    __asm__ volatile("dsb sy" ::: "memory");
    uint64_t head_before = c2g_head(&w);
    OmegaSharedWorldDesc stale;
    memset(&stale, 0, sizeof stale);
    stale.msg_type = RX_RING_CLAIM;
    stale.object_id = src.id;
    stale.object_generation = old_gen;
    stale.object_length = 64;
    stale.payload_len = 24;
    rx_world_seal_descriptor(&stale);
    CHECK(rx_world_inject_descriptor(&w, &stale) == RX_OK, "the old claim was not posted");
    int moved = 0;
    for (int i = 0; i < 3000; i++) {
        __asm__ volatile("dsb sy" ::: "memory");
        if (c2g_head(&w) > head_before) { moved = 1; break; }
        sleep_ms(1);
    }
    CHECK(moved, "the graphics seat did not consume the old claim");
    CHECK(load_u64(win + 8) == SENTINEL, "an old generation changed the window");
    printf("  heartbeat before shutdown %u\n", heartbeat(&w));

    CHECK(rx_resident_shutdown(&w) == RX_OK, "shutdown was not posted");
    int finished = rx_gpu_seat_finish(seat);
    seat = NULL;
    CHECK(finished == 0, "the graphics seat did not leave cleanly");
    if (g_fail == 0) g_silicon = 1;

done:
    if (world_up && w.coherent)
        printf("  heartbeat at end %u flag %u\n", heartbeat(&w), hb_word(&w, 4));
    if (seat) rx_gpu_seat_finish(seat);
    if (world_up) rx_world_destroy(&w);
    if (root_up) rx_caproot_stop(&root, &admin);
    printf("checks %d failures %d silicon %d\n", g_checks, g_fail, g_silicon);
    write_receipt();
    return g_fail ? 1 : 0;
}
