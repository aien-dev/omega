/*
 * rx_r15_instr.c -- R15 instrumentation checks (spec/r15-performance-proof.md
 * §6 and clarification C1). Host only. Every counter the R15 harness reduces
 * is checked here against a case whose true value is known in advance:
 *
 *   propagation   fanout n: inspected, matched, wake attempts, wakes accepted
 *                 and ready insertions are exact, and a subscriber on another
 *                 field is inspected but not matched;
 *   scheduler     wall and thread-CPU time are both recorded, separately;
 *   timing        a buffer that overflows says so;
 *   bytes         each ring path counts one 128-byte descriptor per copy;
 *   R9 I/O        a store counts only its own writes and syncs.
 *
 * Exit status is the number of failed checks (0 = pass).
 */
#include "runtime/rx_caproot.h"
#include "runtime/rx_generation.h"
#include "runtime/rx_world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_checks, g_failures;

#define CHECK(cond, ...) do {                                            \
        g_checks++;                                                      \
        if (!(cond)) {                                                   \
            g_failures++;                                                \
            fprintf(stderr, "  FAIL %s:%d ", __FILE__, __LINE__);        \
            fprintf(stderr, __VA_ARGS__);                                \
            fputc('\n', stderr);                                         \
        }                                                                \
    } while (0)

enum { SUBJ_WORK = 2, SUBJ_EXTERNAL = 100, ISSUER = 3 };
enum { RES_SRC = 0x50, RES_OUT = 0x51 };

typedef struct {
    RxCapRoot root;
    RxCapAdmin admin;
    RxWorld w;
} Env;

static int env_start(Env *e, uint32_t workers) {
    if (rx_caproot_start(&e->root, &e->admin) != RX_CAP_OK) return -1;
    if (rx_world_init(&e->w, &e->root, workers, 1u << 16) != RX_OK) {
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
    rx_capadmin_mint(&e->admin, &m, &r);
    return r;
}

static RxObjRef mkobj(Env *e, uint64_t resource) {
    uint64_t init[RX_MAX_FIELDS] = { 0 };
    RxObjRef r = { UINT32_MAX, 0 };
    rx_world_create(&e->w, 1, RX_PERSIST_RESIDENT, resource, init, &r);
    return r;
}

/* Publishes nothing: the only propagation wave is the stimulus's. */
static int fn_quiet(RxCtx *c) { (void)c; return 0; }

static uint32_t add_listener(Env *e, RxObjRef src, uint64_t mask, RxCapRef rd) {
    RxReactionDesc d;
    memset(&d, 0, sizeof(d));
    d.name = "r15.listener";
    d.faculty = RX_FACULTY_OMEGA;
    d.subject = SUBJ_WORK;
    d.priority = RX_PRIO_FOREGROUND;
    d.fn = fn_quiet;
    d.triggers[d.n_triggers++] = (RxDep){ src, mask };
    d.caps[d.n_caps++] = (RxCapNeed){ rd, RES_SRC, RX_RIGHT_READ };
    uint32_t id = UINT32_MAX;
    CHECK(rx_world_add_reaction(&e->w, &d, &id) == RX_OK, "add listener");
    return id;
}

static void t_propagation_and_scheduler(uint32_t fanout) {
    Env e;
    CHECK(env_start(&e, 1) == 0, "setup fanout %u", fanout);
    RxObjRef src = mkobj(&e, RES_SRC);
    RxCapRef ext = mint(&e, SUBJ_EXTERNAL, RES_SRC, RX_RIGHT_WRITE);
    RxCapRef rd = mint(&e, SUBJ_WORK, RES_SRC, RX_RIGHT_READ);
    for (uint32_t i = 0; i < fanout; i++) add_listener(&e, src, RX_FIELD(0), rd);
    add_listener(&e, src, RX_FIELD(1), rd);        /* inspected, never matched */
    RxTiming buf[512];
    rx_world_set_timing(&e.w, buf, 512);
    RxMutation m = { src, 0, 7 };
    CHECK(rx_world_publish_external(&e.w, ext, &m, 1) > 0, "stimulus");
    CHECK(rx_world_wait_quiescent(&e.w, 5000) == RX_OK, "quiet");
    pthread_mutex_lock(&e.w.mu);
    RxPropWave p = e.w.last_prop;
    RxStats s = e.w.stats;
    pthread_mutex_unlock(&e.w.mu);
    CHECK(p.inspected == fanout + 1u, "fanout %u: inspected %llu", fanout,
          (unsigned long long)p.inspected);
    CHECK(p.matched == fanout, "fanout %u: matched %llu", fanout, (unsigned long long)p.matched);
    CHECK(p.wake_attempts == fanout, "fanout %u: wake attempts %llu", fanout,
          (unsigned long long)p.wake_attempts);
    CHECK(p.wakes_accepted == fanout, "fanout %u: accepted %llu", fanout,
          (unsigned long long)p.wakes_accepted);
    CHECK(p.ready_inserts == fanout, "fanout %u: ready inserts %llu", fanout,
          (unsigned long long)p.ready_inserts);
    CHECK(p.coalesced == 0 && p.deferred == 0 && p.suppressed == 0,
          "fanout %u: unexpected coalesced/deferred/suppressed", fanout);
    CHECK(p.wall_ns > 0 && p.cpu_ns > 0, "fanout %u: wave time not recorded", fanout);
    uint64_t stored = 0, attempted = 0;
    CHECK(rx_world_timing_status(&e.w, &stored, &attempted) == RX_OK && stored == fanout &&
          attempted == fanout, "fanout %u: timing stored %llu attempted %llu", fanout,
          (unsigned long long)stored, (unsigned long long)attempted);
    uint64_t cpu = 0, wall = 0;
    int missing = 0;
    for (uint64_t i = 0; i < stored; i++) {
        if (!buf[i].t_demand || !buf[i].t_ready || !buf[i].t_run || !buf[i].sched_ns ||
            !buf[i].sched_cpu_ns) missing++;
        cpu += buf[i].sched_cpu_ns;
        wall += buf[i].sched_ns;
    }
    CHECK(missing == 0, "fanout %u: %d samples lack timestamps or scheduler time", fanout, missing);
    CHECK(s.sched_wall_ns > 0 && s.sched_cpu_ns > 0, "world scheduler totals missing");
    /* Both clocks count nanoseconds on the same thread; CPU time cannot run
     * meaningfully ahead of wall time. 1 us per sample allows for the two
     * clocks' separate reads. */
    CHECK(cpu <= wall + stored * 1000u, "fanout %u: scheduler CPU %llu > wall %llu", fanout,
          (unsigned long long)cpu, (unsigned long long)wall);
    rx_world_set_timing(&e.w, NULL, 0);
    env_stop(&e);
}

/* One slot: every matched dependent is woken, but only one is placed on the
 * ready ring in the wave. Wake attempts are not ready insertions. */
static void t_blocked_wave(void) {
    Env e;
    CHECK(env_start(&e, 1) == 0, "setup");
    RxResourceBudget b = e.w.budget;
    b.slots = 1;
    rx_world_set_resources(&e.w, &b);
    RxObjRef src = mkobj(&e, RES_SRC);
    RxCapRef ext = mint(&e, SUBJ_EXTERNAL, RES_SRC, RX_RIGHT_WRITE);
    RxCapRef rd = mint(&e, SUBJ_WORK, RES_SRC, RX_RIGHT_READ);
    for (int i = 0; i < 8; i++) add_listener(&e, src, RX_FIELD(0), rd);
    RxTiming buf[64];
    rx_world_set_timing(&e.w, buf, 64);
    RxMutation m = { src, 0, 3 };
    CHECK(rx_world_publish_external(&e.w, ext, &m, 1) > 0, "stimulus");
    pthread_mutex_lock(&e.w.mu);
    RxPropWave p = e.w.last_prop;
    pthread_mutex_unlock(&e.w.mu);
    CHECK(p.matched == 8 && p.wake_attempts == 8 && p.wakes_accepted == 8,
          "blocked wave: matched %llu attempts %llu accepted %llu",
          (unsigned long long)p.matched, (unsigned long long)p.wake_attempts,
          (unsigned long long)p.wakes_accepted);
    CHECK(p.ready_inserts == 1, "blocked wave: %llu ready insertions with one slot",
          (unsigned long long)p.ready_inserts);
    CHECK(rx_world_wait_quiescent(&e.w, 5000) == RX_OK, "quiet");
    CHECK(e.w.stats.ready_inserts == 8, "all eight eventually ran (%llu)",
          (unsigned long long)e.w.stats.ready_inserts);
    rx_world_set_timing(&e.w, NULL, 0);
    env_stop(&e);
}

static void t_timing_overflow(void) {
    Env e;
    CHECK(env_start(&e, 1) == 0, "setup");
    RxObjRef src = mkobj(&e, RES_SRC);
    RxCapRef ext = mint(&e, SUBJ_EXTERNAL, RES_SRC, RX_RIGHT_WRITE);
    RxCapRef rd = mint(&e, SUBJ_WORK, RES_SRC, RX_RIGHT_READ);
    for (int i = 0; i < 10; i++) add_listener(&e, src, RX_FIELD(0), rd);
    RxTiming buf[4];
    rx_world_set_timing(&e.w, buf, 4);
    RxMutation m = { src, 0, 1 };
    rx_world_publish_external(&e.w, ext, &m, 1);
    rx_world_wait_quiescent(&e.w, 5000);
    uint64_t stored = 0, attempted = 0;
    CHECK(rx_world_timing_status(&e.w, &stored, &attempted) == RX_ERR_FULL,
          "an overflowing timing buffer was reported complete");
    CHECK(stored == 4 && attempted == 10, "stored %llu attempted %llu",
          (unsigned long long)stored, (unsigned long long)attempted);
    rx_world_set_timing(&e.w, NULL, 0);
    env_stop(&e);
}

static void t_ring_bytes(void) {
    Env e;
    CHECK(env_start(&e, 1) == 0, "setup");
    RxObjRef obj = mkobj(&e, RES_SRC);
    CHECK(rx_world_attach_physical(&e.w, obj) == RX_OK, "attach");
    RxCapRef cap = mint(&e, SUBJ_EXTERNAL, RES_SRC, RX_RIGHT_READ | RX_RIGHT_WRITE);
    CHECK(rx_world_bind_capability(&e.w, obj, cap) == RX_OK, "bind");
    RxStats s0 = e.w.stats;
    RxMutation m = { obj, 0, 5 };
    CHECK(rx_world_publish_external(&e.w, cap, &m, 1) > 0, "stimulus");
    CHECK(rx_world_wait_quiescent(&e.w, 3000) == RX_OK, "quiet");
    RxStats s1 = e.w.stats;
    CHECK(s1.c2g_write_bytes - s0.c2g_write_bytes == sizeof(OmegaSharedWorldDesc),
          "publication descriptor write not counted once (%llu)",
          (unsigned long long)(s1.c2g_write_bytes - s0.c2g_write_bytes));
    CHECK(s1.proj_bytes > s0.proj_bytes, "projection write not counted");
    CHECK(s1.crumb_bytes - s0.crumb_bytes == sizeof(RxCrumb), "stimulus crumb not counted once");
    uint32_t fault = 0;
    CHECK(rx_world_take_publication(&e.w, NULL, &fault) == RX_OK, "take publication");
    CHECK(e.w.stats.c2g_read_bytes - s1.c2g_read_bytes == sizeof(OmegaSharedWorldDesc),
          "publication descriptor read not counted once");
    RxStats s2 = e.w.stats;
    CHECK(rx_world_relocate_physical(&e.w, obj) == RX_OK, "relocate");
    CHECK(e.w.stats.window_move_bytes - s2.window_move_bytes == 2u * RX_OBJECT_WINDOW,
          "window relocation not counted");
    env_stop(&e);
}

/* A long episode must not die of a full causal log: the long-episode
 * capacity takes more records than the old 2^18 R14 log, records never
 * move, and they verify. A deliberately small log still refuses a
 * publication it cannot record (no action without evidence). */
static void t_long_log(void) {
    RxCapRoot root;
    RxCapAdmin admin;
    static RxWorld w;
    CHECK(rx_caproot_start(&root, &admin) == RX_CAP_OK, "authority");
    CHECK(rx_world_init(&w, &root, 1, RX_CRUMBS_LONG_EPISODE) == RX_OK, "long-episode world");
    w.external_subject = SUBJ_EXTERNAL;
    uint64_t init[RX_MAX_FIELDS] = {0};
    RxObjRef src = {UINT32_MAX, 0};
    rx_world_create(&w, 1, RX_PERSIST_RESIDENT, RES_SRC, init, &src);
    RxCapMint mm;
    memset(&mm, 0, sizeof mm);
    mm.issuer = ISSUER; mm.subject = SUBJ_EXTERNAL; mm.resource = RES_SRC;
    mm.rights = RX_RIGHT_WRITE; mm.parent = (RxCapRef){UINT32_MAX, 0};
    mm.authority = rx_capadmin_office(&admin);
    RxCapRef ext = {UINT32_MAX, 0};
    rx_capadmin_mint(&admin, &mm, &ext);
    RxMutation m = {src, 0, 1};
    CHECK(rx_world_publish_external(&w, ext, &m, 1) > 0, "first record");
    const RxCrumb *first = rx_world_crumb(&w, 1);
    const uint64_t n = 600000;
    int refused = 0;
    for (uint64_t i = 2; i <= n; i++) {
        m.value = i;
        if (rx_world_publish_external(&w, ext, &m, 1) <= 0) { refused = 1; break; }
    }
    CHECK(!refused && w.stats.crumb_overflow == 0 && w.n_crumbs >= n,
          "long log refused after %llu records", (unsigned long long)w.n_crumbs);
    CHECK(rx_world_crumb(&w, 1) == first, "a record moved while the log grew");
    uint64_t checked = 0;
    CHECK(rx_world_verify_crumbs(&w, &checked) == 0 && checked == w.n_crumbs,
          "long log does not verify");
    rx_world_destroy(&w);

    static RxWorld small;
    CHECK(rx_world_init(&small, &root, 1, 16) == RX_OK, "small world");
    small.external_subject = SUBJ_EXTERNAL;
    RxObjRef s2 = {UINT32_MAX, 0};
    rx_world_create(&small, 1, RX_PERSIST_RESIDENT, RES_SRC, init, &s2);
    RxMutation m2 = {s2, 0, 1};
    int64_t last = 1;
    for (int i = 0; i < 40 && last > 0; i++) { m2.value = (uint64_t)i + 2; last = rx_world_publish_external(&small, ext, &m2, 1); }
    CHECK(last <= 0 && small.stats.crumb_overflow > 0, "a full log did not refuse the publication");
    rx_world_destroy(&small);
    rx_caproot_stop(&root, &admin);
}

static int mkstore(char *dir, size_t n, const char *tag) {
    snprintf(dir, n, "/tmp/rx_r15_instr_%ld_%s", (long)getpid(), tag);
    return mkdir(dir, 0700);
}

static void t_store_io_scoped(void) {
    char a_dir[128], b_dir[128];
    CHECK(mkstore(a_dir, sizeof a_dir, "a") == 0 && mkstore(b_dir, sizeof b_dir, "b") == 0,
          "store dirs");
    uint64_t pb0 = 0, ps0 = 0;
    rx_gen_io_counters(&pb0, &ps0);
    RxGenStore *a = NULL, *b = NULL;
    CHECK(rx_gen_open(a_dir, &a) == RX_GEN_OK, "open a");
    uint64_t ab = 0, as = 0;
    CHECK(rx_gen_store_io(a, &ab, &as) == RX_GEN_OK, "store a io");
    CHECK(ab > 0 && as > 0, "opening a store wrote nothing (%llu bytes, %llu syncs)",
          (unsigned long long)ab, (unsigned long long)as);
    CHECK(rx_gen_open(b_dir, &b) == RX_GEN_OK, "open b");
    uint64_t ab2 = 0, as2 = 0, bb = 0, bs = 0, pb1 = 0, ps1 = 0;
    rx_gen_store_io(a, &ab2, &as2);
    rx_gen_store_io(b, &bb, &bs);
    rx_gen_io_counters(&pb1, &ps1);
    CHECK(ab2 == ab && as2 == as, "store b's writes were charged to store a");
    CHECK(pb1 - pb0 == ab + bb && ps1 - ps0 == as + bs,
          "process total differs from the sum of the two stores");
    rx_gen_close(a);
    rx_gen_close(b);
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' '%s'", a_dir, b_dir);
    if (system(cmd) != 0) fprintf(stderr, "note: could not remove %s\n", a_dir);
}

int main(void) {
    static const uint32_t fanouts[] = { 1, 2, 4, 8, 16, 32, 64, 128, 256 };
    for (size_t i = 0; i < sizeof fanouts / sizeof fanouts[0]; i++)
        t_propagation_and_scheduler(fanouts[i]);
    t_blocked_wave();
    t_timing_overflow();
    t_ring_bytes();
    t_store_io_scoped();
    t_long_log();
    printf("R15 instrumentation: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
