/*
 * rx_r15_g7.c -- production keeps serving through a generation promotion
 * (R15 G7; spec/r15-performance-proof.md §6.7 and §19).
 *
 * The R13 body is built by the R15 rig. A resident world hands the R9 store's
 * physical work (the proposal's and the promotion's fsyncs) to the store's
 * durable executor, so the one semantic worker of RES-1 is free while the
 * disk works. These checks prove it on the host stand-in seat:
 *
 *  1. RES-1: while the promotion is between its durable candidate write and
 *     its flip, a production request is READY and executes (the disk hook
 *     waits, on the executor thread, for production to commit).
 *  2. RES-1: production commits inside [barrier begin, receipt].
 *  3. The in-force record is published once, by generation.promote, after
 *     the durable flip, and names the generation now on disk.
 *  4. A stop before the flip recovers OLD.
 *  5. A stop after the flip recovers NEW.
 *  6. A stop during the flip recovers OLD or NEW; never TORN.
 *  7. Only generation.promote may write the promotion and in-force records,
 *     and every such write came from it (the executor holds no authority).
 *  8. No orchestrator: every crumb was made by a world worker or the seat;
 *     the executor runs no reaction; RES has no orchestrator thread.
 *  9. RES-4 promotes, reaches goal MET and serves during the barrier.
 * 10. SEQ is unchanged: no executor, no deferral, and it still pauses
 *     production for the whole barrier (the sequential reference).
 *
 * Episodes can miss the TARGET_PCT goal on a noisy host (a measured margin,
 * unrelated to promotion). Each check takes the first of up to
 * R15_G7_ATTEMPTS (default 4) episodes that promoted and reached goal MET.
 */
#include "rx_r15_rig.h"

#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_checks, g_fail;

#define CHECK(c, ...) do {                                        \
        g_checks++;                                               \
        if (!(c)) {                                               \
            g_fail++;                                             \
            fprintf(stderr, "  FAIL: " __VA_ARGS__);              \
            fputc('\n', stderr);                                  \
        }                                                         \
    } while (0)

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int attempts(void) {
    const char *e = getenv("R15_G7_ATTEMPTS");
    int n = e ? atoi(e) : 4;
    return n > 0 ? n : 4;
}

/* ---- 1: the disk hook runs on the executor between candidate write and flip */

typedef struct {
    R15Rig *r;
    int called;
    uint64_t served_at_hook, served_after, waited_ns;
    int ready_seen;             /* serve READY or RUNNING at some poll */
} HookCtx;

static void disk_hook(const char *dir, void *ctx) {
    (void)dir;
    HookCtx *h = ctx;
    R15Rig *r = h->r;
    h->called++;
    uint32_t rs = r->omega.r_serve;
    uint64_t s0 = __atomic_load_n(&r->served, __ATOMIC_RELAXED);
    uint64_t t0 = now_ns();
    h->served_at_hook = s0;
    /* Up to 2 s: with a held worker production could never advance here. */
    while (now_ns() - t0 < 2000000000ull) {
        pthread_mutex_lock(&r->w.mu);
        RxState st = r->w.reactions[rs].state;
        pthread_mutex_unlock(&r->w.mu);
        if (st == RX_READY || st == RX_RUNNING || st == RX_PUBLISHING) h->ready_seen = 1;
        if (__atomic_load_n(&r->served, __ATOMIC_RELAXED) >= s0 + 3 && h->ready_seen) break;
        struct timespec ts = {0, 50000};
        nanosleep(&ts, NULL);
    }
    h->served_after = __atomic_load_n(&r->served, __ATOMIC_RELAXED);
    h->waited_ns = now_ns() - t0;
}

/* ---- crumb scans --------------------------------------------------------- */

static uint64_t serve_commits_in(RxWorld *w, uint32_t r_serve, uint64_t lo, uint64_t hi) {
    uint64_t n = 0;
    for (uint64_t id = 1; id <= w->n_crumbs; id++) {
        const RxCrumb *k = rx_world_crumb(w, id);
        if (k && k->reaction == r_serve && k->kind == RX_CRUMB_COMMIT &&
            k->t_end_ns >= lo && k->t_end_ns <= hi)
            n++;
    }
    return n;
}

static int writes_obj(const RxCrumb *k, RxObjRef o) {
    for (uint32_t i = 0; i < k->n_outputs; i++)
        if (k->outputs[i].obj.id == o.id) return 1;
    return 0;
}

/* 3, 7, 8 on a finished episode. */
static void audit(R15Rig *r, const RxGenPhases *ph, const char *cfg) {
    RxWorld *w = &r->w;
    const RxLivingPromoter *p = &r->promoter;
    uint64_t inforce_writes = 0, promo_writes = 0, foreign = 0, bad_worker = 0;
    const RxCrumb *inforce = NULL;
    for (uint64_t id = 1; id <= w->n_crumbs; id++) {
        const RxCrumb *k = rx_world_crumb(w, id);
        if (!k) continue;
        if (k->kind == RX_CRUMB_COMMIT || k->kind == RX_CRUMB_NOOP) {
            int ok_worker = k->worker < w->n_workers || k->worker == RX_SEAT_BLACKWELL ||
                            (w->sequential && k->worker == RX_SEQ_WORKER);
            if (!ok_worker) bad_worker++;
        }
        if (k->kind != RX_CRUMB_COMMIT) continue;
        int wi = writes_obj(k, p->inforce), wp = writes_obj(k, p->promotion);
        if (!wi && !wp) continue;
        if (k->reaction != p->reaction) foreign++;
        if (wi) { inforce_writes++; inforce = k; }
        if (wp) promo_writes++;
    }
    CHECK(foreign == 0, "%s: %" PRIu64 " promotion/in-force writes not by generation.promote",
          cfg, foreign);
    CHECK(inforce_writes == 1 && promo_writes == 1,
          "%s: in-force written %" PRIu64 "x, promotion %" PRIu64 "x (want once each)", cfg,
          inforce_writes, promo_writes);
    CHECK(inforce && ph->flip_ns && ph->receipt_ns && inforce->t_end_ns >= ph->receipt_ns,
          "%s: in-force published before the durable flip and receipt", cfg);
    uint32_t writers = 0;
    for (uint32_t i = 0; i < w->n_reactions; i++) {
        const RxReactionDesc *d = &w->reactions[i].desc;
        for (uint32_t j = 0; j < d->n_writes; j++)
            if (d->writes[j].obj.id == p->inforce.id) writers++;
    }
    /* generation.promote only: the R15 body registers no restore. */
    CHECK(writers == 1, "%s: %u reactions declare in-force writes (want 1)", cfg, writers);
    CHECK(bad_worker == 0, "%s: %" PRIu64 " crumbs from no world worker and no seat", cfg,
          bad_worker);
    RxObject rec;
    uint64_t active = 0, lineage = 0;
    rx_gen_active(r->gen, &active, &lineage);
    CHECK(rx_world_read(w, p->inforce, &rec) == RX_OK && rec.field[7] == active &&
          active == ph->candidate_id,
          "%s: in-force names generation %" PRIu64 ", store active %" PRIu64 ", promoted %" PRIu64,
          cfg, rec.field[7], active, ph->candidate_id);
    RxRecoveryRecord rr;
    CHECK(rx_gen_recover(r->generation_dir, &rr) == RX_GEN_OK && rr.coherent &&
          rr.active_id == active && rr.receipt_present,
          "%s: recovery of the live store does not name the promoted generation", cfg);
}

/* One episode that promoted and met its goal, or 0 after the attempts. The
 * rig stays started on success; the caller stops it. */
static int good_episode(R15Rig *r, R15Config cfg, HookCtx *hook, R15Outcome *out) {
    for (int a = 1; a <= attempts(); a++) {
        if (r15_start(r, cfg) != 0) {
            fprintf(stderr, "  %s attempt %d: build failed at stage %d (%s)\n",
                    r15_config_name(cfg), a, r->stage, r->stage_why ? r->stage_why : "");
            r15_stop(r);
            continue;
        }
        if (hook) {
            memset(hook, 0, sizeof *hook);
            hook->r = r;
            rx_gen_set_disk_hook(r->gen, disk_hook, hook);
        }
        int rc = r15_episode(r, 0, out);
        r15_producer_stop(r);
        rx_world_wait_quiescent(&r->w, 10000);
        if (rc == 0 && out->ok && out->promote_result == RX_GEN_OK) {
            printf("  %s: episode %d reached goal MET (generation %llu -> %llu)\n",
                   r15_config_name(cfg), a, (unsigned long long)out->gen_before,
                   (unsigned long long)out->gen_after);
            return 1;
        }
        printf("  %s: episode %d did not reach goal MET (%s); another\n",
               r15_config_name(cfg), a, out->why);
        r15_stop(r);
    }
    return 0;
}

static R15Rig g_rig;

static void res_case(R15Config cfg) {
    R15Rig *r = &g_rig;
    R15Outcome out;
    HookCtx hook;
    const char *name = r15_config_name(cfg);
    printf("[*] %s: promotion while production runs\n", name);
    if (!good_episode(r, cfg, &hook, &out)) {
        CHECK(0, "%s: no episode reached goal MET in %d attempts", name, attempts());
        return;
    }
    RxGenPhases ph;
    memset(&ph, 0, sizeof ph);
    rx_gen_last_phases(r->gen, &ph);
    uint64_t during = serve_commits_in(&r->w, r->omega.r_serve, ph.barrier_ns, ph.receipt_ns);
    uint64_t to_flip = serve_commits_in(&r->w, r->omega.r_serve, ph.barrier_ns, ph.flip_ns);
    printf("    barrier %.1f ms (flip at %.1f ms); production commits during it %" PRIu64
           " (%" PRIu64 " before the flip); hook saw %" PRIu64 " -> %" PRIu64 " served in %.2f ms\n",
           (double)(ph.receipt_ns - ph.barrier_ns) / 1e6, (double)(ph.flip_ns - ph.barrier_ns) / 1e6,
           during, to_flip, hook.served_at_hook, hook.served_after, (double)hook.waited_ns / 1e6);
    CHECK(rx_gen_exec_running(r->gen), "%s: store has no durable executor", name);
    CHECK(hook.called == 1, "%s: disk hook ran %d times (want 1)", name, hook.called);
    CHECK(hook.ready_seen, "%s: no production request READY during the promotion", name);
    CHECK(hook.served_after >= hook.served_at_hook + 3,
          "%s: production did not commit while the promotion's disk work waited", name);
    CHECK(during > 0, "%s: no production commit inside the barrier", name);
    CHECK(to_flip > 0, "%s: no production commit between barrier begin and flip", name);
    CHECK(r->w.stats.deferrals >= 2, "%s: %" PRIu64 " deferrals (want >= 2: proposal, promotion)",
          name, r->w.stats.deferrals);
    CHECK(!r->orch_live, "%s: an orchestrator thread runs in a resident world", name);
    CHECK(out.wrong == 0 && out.unpromoted_use == 0 && out.illegal == 0 && out.crumbs_verified &&
          out.lost_triggers == 0,
          "%s: integrity (wrong %" PRIu64 ", unpromoted %" PRIu64 ", illegal %" PRIu64
          ", verified %d, lost %" PRIu64 ")", name, out.wrong, out.unpromoted_use, out.illegal,
          out.crumbs_verified, out.lost_triggers);
    audit(r, &ph, name);
    r15_stop(r);
}

static void seq_case(void) {
    R15Rig *r = &g_rig;
    R15Outcome out;
    printf("[*] SEQ: the sequential reference is unchanged\n");
    if (!good_episode(r, R15_SEQ, NULL, &out)) {
        CHECK(0, "SEQ: no episode reached goal MET in %d attempts", attempts());
        return;
    }
    RxGenPhases ph;
    memset(&ph, 0, sizeof ph);
    rx_gen_last_phases(r->gen, &ph);
    uint64_t during = serve_commits_in(&r->w, r->omega.r_serve, ph.barrier_ns, ph.receipt_ns);
    printf("    barrier %.1f ms; production commits during it %" PRIu64 "\n",
           (double)(ph.receipt_ns - ph.barrier_ns) / 1e6, during);
    CHECK(!rx_gen_exec_running(r->gen), "SEQ: a durable executor was started");
    CHECK(r->w.stats.deferrals == 0, "SEQ: %" PRIu64 " deferrals", r->w.stats.deferrals);
    CHECK(during == 0, "SEQ: production ran during a stage (the reference is sequential)");
    audit(r, &ph, "SEQ");
    r15_stop(r);
}

/* ---- 4, 5, 6: stop the process at an R9 step, recover from disk ---------- */

static void crash_case(int step, const char *label, int want) {
    printf("[*] RES-1: stop %s\n", label);
    int fd[2];
    if (pipe(fd) != 0) { CHECK(0, "pipe"); return; }
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        close(fd[0]);
        static R15Rig c;
        if (r15_start(&c, R15_RES1) != 0) _exit(3);
        if (write(fd[1], c.generation_dir, sizeof c.generation_dir) != sizeof c.generation_dir)
            _exit(4);
        close(fd[1]);
        rx_gen_set_crash(c.gen, step);
        for (int a = 0; a < attempts(); a++) {
            R15Outcome o;
            (void)r15_episode(&c, 0, &o);
            /* the store stops the process at `step` if a promotion began */
        }
        _exit(5);
    }
    close(fd[1]);
    char dir[128] = {0};
    ssize_t got = read(fd[0], dir, sizeof dir);
    close(fd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    CHECK(got == (ssize_t)sizeof dir && dir[0], "%s: child did not report its store", label);
    CHECK(code == 86, "%s: child exit %d (want 86, the injected stop)", label, code);
    if (got != (ssize_t)sizeof dir || !dir[0]) return;
    RxRecoveryRecord rr;
    int rc = rx_gen_recover(dir, &rr);
    printf("    recovered generation %" PRIu64 " (lineage %" PRIu64 "), rc %d, coherent %d\n",
           rr.active_id, rr.lineage, rc, rr.coherent);
    CHECK(rc == RX_GEN_OK && rr.coherent, "%s: recovery rc %d coherent %d (TORN)", label, rc,
          rr.coherent);
    if (want == 1) CHECK(rr.active_id == 1, "%s: recovered %" PRIu64 ", want OLD (1)", label,
                         rr.active_id);
    else if (want == 2) CHECK(rr.active_id == 2, "%s: recovered %" PRIu64 ", want NEW (2)", label,
                              rr.active_id);
    else CHECK(rr.active_id == 1 || rr.active_id == 2, "%s: recovered %" PRIu64 ", want OLD or NEW",
               label, rr.active_id);
    /* A second recovery agrees (recovery is idempotent). */
    RxRecoveryRecord again;
    CHECK(rx_gen_recover(dir, &again) == RX_GEN_OK && again.active_id == rr.active_id,
          "%s: second recovery disagrees", label);
    char cmd[200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) fprintf(stderr, "note: could not remove %s\n", dir);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    res_case(R15_RES1);
    res_case(R15_RES4);
    seq_case();
    crash_case(RX_CRASH_BEFORE_ROOT_FLIP, "before the flip", 1);
    crash_case(RX_CRASH_DURING_ROOT_FLIP, "during the flip", 0);
    crash_case(RX_CRASH_AFTER_ROOT_FLIP, "after the flip", 2);
    printf("R15 G7 worker split: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
