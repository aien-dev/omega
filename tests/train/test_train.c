/* test_train.c -- M22 substrate exit tests (Lane 21). Host only, one core.
 *
 *   test_train <scratch-dir>
 *
 * Sections: basic commit + history; SGD training on a fixed quadratic;
 * crash matrix (fork + _exit at every injectable point, recovery yields
 * exactly OLD or NEW, digest verified; plus torn-file simulation); lock-free
 * reader racing a writer; negatives (corrupted shadow, truncated generation,
 * digest mismatch, pointer mismatch, validation reject, double switch, stale
 * shadow, dispatch tamper, truncated dispatch log, second writer, re-create);
 * provenance cost (tier a constant per record, tier b only at commit);
 * replay (same update sequence from G0 -> byte-identical G_n, and each
 * committed step re-derived from its dispatch record). Prints
 * "replay_digest <hex>" and the verdict line M22_SUBSTRATE_TESTS_PASS. */
#define _XOPEN_SOURCE 700
#include "train/tg_sgd.h"
#include "train/tg_store.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_checks, g_fail;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static char g_base[1024];
static int g_dirn;

static int rm_cb(const char *p, const struct stat *sb, int f, struct FTW *fw)
{ (void)sb; (void)f; (void)fw; return remove(p); }
static void rmtree(const char *d) { nftw(d, rm_cb, 16, FTW_DEPTH | FTW_PHYS); }

static void newdir(char *out, size_t n, const char *tag)
{
    snprintf(out, n, "%s/%02d-%s", g_base, g_dirn++, tag);
    rmtree(out);
}

/* ---- deterministic fixture: quadratic L = 0.5 * sum (p - t)^2 ---------- */
static uint32_t lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s; }
static float urand(uint32_t *s) { return (float)(lcg(s) >> 8) / 16777216.0f - 0.5f; }

static void init_params(float *p, size_t n, uint32_t seed) { for (size_t i = 0; i < n; i++) p[i] = 4.0f * urand(&seed); }
static void target(float *t, size_t n) { uint32_t s = 0xC0FFEEu; for (size_t i = 0; i < n; i++) t[i] = urand(&s); }

static void grad_of(const float *p, const float *t, float *g, size_t n)
{ for (size_t i = 0; i < n; i++) g[i] = p[i] - t[i]; }

static double loss_of(const float *p, const float *t, size_t n)
{ double l = 0; for (size_t i = 0; i < n; i++) { double d = (double)p[i] - t[i]; l += 0.5 * d * d; } return l; }

static int v_finite(const tg_shadow *sh, const tg_snapshot *c, void *ctx)
{
    (void)c; (void)ctx;
    const float *p = (const float *)(const void *)sh->params;
    for (size_t i = 0; i < sh->param_bytes / 4; i++) if (!isfinite(p[i])) return 1;
    return 0;
}
static int v_reject(const tg_shadow *sh, const tg_snapshot *c, void *ctx) { (void)sh; (void)c; (void)ctx; return 1; }

#define N_SMALL 1024
static const float LR = 0.05f, MOM = 0.9f;

static int make_store(const char *dir, size_t n, int momentum)
{
    float *p = calloc(n, 4), *v = calloc(n, 4);
    init_params(p, n, 12345u);
    int r = tg_create(dir, p, n * 4, v, momentum ? n * 4 : 0);
    free(p); free(v);
    return r;
}

/* One SGD step on an open store; returns commit result. */
static int sgd_commit(tg_store *st, const float *t, size_t n, float mom, uint64_t ref)
{
    tg_shadow sh;
    int r = tg_shadow_begin(st, &sh);
    if (r) return r;
    float *g = malloc(n * 4);
    grad_of((const float *)(const void *)tg_committed(st)->params, t, g, n);
    r = tg_sgd_step(st, &sh, g, n, LR, mom, ref);
    free(g);
    if (r) { tg_shadow_discard(&sh); return r; }
    return tg_commit(st, &sh, v_finite, NULL);
}

static int count_refusals(const char *dir, const char *code)
{
    char p[1200], line[512], key[64];
    snprintf(p, sizeof p, "%s/refusals.log", dir);
    snprintf(key, sizeof key, "REFUSE %s ", code);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    int c = 0;
    while (fgets(line, sizeof line, f)) if (!strncmp(line, key, strlen(key))) c++;
    fclose(f);
    return c;
}

static int file_exists(const char *dir, const char *name)
{ char p[1200]; snprintf(p, sizeof p, "%s/%s", dir, name); return access(p, F_OK) == 0; }

static off_t file_size(const char *dir, const char *name)
{ char p[1200]; struct stat sb; snprintf(p, sizeof p, "%s/%s", dir, name); return stat(p, &sb) ? -1 : sb.st_size; }

static void gen_file(char *out, size_t n, const char *dir, uint64_t g)
{ snprintf(out, n, "%s/gen-%020llu.bin", dir, (unsigned long long)g); }

static void flip_byte(const char *path, off_t at)
{
    int fd = open(path, O_RDWR);
    uint8_t b = 0;
    if (fd < 0) return;
    if (pread(fd, &b, 1, at) == 1) { b ^= 0x01; (void)!pwrite(fd, &b, 1, at); }
    close(fd);
}

/* ---- 1. basic --------------------------------------------------------- */
static void t_basic(const float *t)
{
    char d[1100];
    newdir(d, sizeof d, "basic");
    CHECK(make_store(d, N_SMALL, 1) == TG_OK, "create");
    tg_store *st; tg_recovery rec;
    CHECK(tg_open(d, &st, &rec) == TG_OK, "open");
    if (!st) return;
    CHECK(tg_committed(st)->gen == 0 && tg_committed(st)->step == 0, "gen0");
    uint8_t d0[TG_DIGEST]; memcpy(d0, tg_committed(st)->digest, TG_DIGEST);
    CHECK(sgd_commit(st, t, N_SMALL, MOM, 1) == TG_OK, "commit 1");
    CHECK(tg_committed(st)->gen == 1 && tg_committed(st)->step == 1, "gen1");
    CHECK(!memcmp(tg_committed(st)->prev_digest, d0, TG_DIGEST), "prev digest links to gen0");
    tg_snapshot s;
    CHECK(tg_read_current(d, &s) == TG_OK && s.gen == 1 && !memcmp(s.digest, tg_committed(st)->digest, TG_DIGEST),
          "reader sees gen1");
    tg_snapshot_free(&s);
    tg_close(st);
    uint64_t nv;
    CHECK(tg_verify_history(d, &nv) == TG_OK && nv == 2, "history 2 gens");
    CHECK(!file_exists(d, "CURRENT.tmp"), "no pointer tmp left");
}

/* ---- 2. SGD training ---------------------------------------------------- */
static void t_training(const float *t)
{
    char d[1100];
    for (int m = 0; m < 2; m++) {
        newdir(d, sizeof d, m ? "sgd-momentum" : "sgd-plain");
        float mom = m ? MOM : 0.0f;
        CHECK(make_store(d, N_SMALL, m) == TG_OK, "create");
        tg_store *st;
        CHECK(tg_open(d, &st, NULL) == TG_OK, "open");
        if (!st) return;
        double l0 = loss_of((const float *)(const void *)tg_committed(st)->params, t, N_SMALL);
        for (int k = 0; k < 40; k++) CHECK(sgd_commit(st, t, N_SMALL, mom, 100 + (uint64_t)k) == TG_OK, "step %d", k);
        double l1 = loss_of((const float *)(const void *)tg_committed(st)->params, t, N_SMALL);
        printf("sgd_%s loss_g0 %.6f loss_g40 %.6f\n", m ? "momentum" : "plain", l0, l1);
        CHECK(l1 < l0 * 0.1, "loss fell by 10x (%f -> %f)", l0, l1);
        CHECK(tg_committed(st)->gen == 40, "40 generations");
        tg_close(st);
        uint64_t nv;
        CHECK(tg_verify_history(d, &nv) == TG_OK && nv == 41, "history 41");
    }
}

/* ---- 3. crash matrix ---------------------------------------------------- */
static void t_crash(const float *t)
{
    /* Reference: OLD = gen1, NEW = gen2 with the same step applied cleanly. */
    char ref[1100];
    newdir(ref, sizeof ref, "crash-ref");
    tg_store *st;
    uint8_t dold[TG_DIGEST], dnew[TG_DIGEST];
    make_store(ref, N_SMALL, 1);
    CHECK(tg_open(ref, &st, NULL) == TG_OK, "ref open");
    if (!st) return;
    sgd_commit(st, t, N_SMALL, MOM, 1);
    memcpy(dold, tg_committed(st)->digest, TG_DIGEST);
    CHECK(sgd_commit(st, t, N_SMALL, MOM, 2) == TG_OK, "ref new");
    memcpy(dnew, tg_committed(st)->digest, TG_DIGEST);
    tg_close(st);

    for (int torn = 0; torn < 2; torn++)
    for (int fp = 1; fp < TG_FP_COUNT; fp++) {
        if (torn && fp != TG_FP_AFTER_SHADOW_WRITE && fp != TG_FP_AFTER_GEN_RENAME && fp != TG_FP_BEFORE_SWITCH_RENAME) continue;
        char d[1100];
        newdir(d, sizeof d, "crash");
        make_store(d, N_SMALL, 1);
        CHECK(tg_open(d, &st, NULL) == TG_OK, "open");
        if (!st) return;
        sgd_commit(st, t, N_SMALL, MOM, 1);
        CHECK(!memcmp(tg_committed(st)->digest, dold, TG_DIGEST), "old digest reproducible");
        tg_close(st);

        fflush(NULL);
        pid_t pid = fork();
        if (pid == 0) {
            tg_store *w;
            if (tg_open(d, &w, NULL)) _exit(2);
            tg_failpoint_arm((tg_failpoint)fp, TG_FPMODE_EXIT);
            sgd_commit(w, t, N_SMALL, MOM, 2);
            _exit(3); /* fail point not reached */
        }
        int status = 0;
        waitpid(pid, &status, 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == TG_FP_EXIT_CODE, "fp %s: child exit %d",
              tg_failpoint_name((tg_failpoint)fp), WIFEXITED(status) ? WEXITSTATUS(status) : -1);

        if (torn) { /* simulate partial persistence of the not-yet-current files */
            char p[1200];
            if (fp == TG_FP_AFTER_SHADOW_WRITE) snprintf(p, sizeof p, "%s/shadow-%020llu.tmp", d, 2ull);
            else if (fp == TG_FP_AFTER_GEN_RENAME) gen_file(p, sizeof p, d, 2);
            else snprintf(p, sizeof p, "%s/CURRENT.tmp", d);
            struct stat sb;
            CHECK(stat(p, &sb) == 0 && truncate(p, sb.st_size / 2) == 0, "torn %s", p);
        }

        int expect_new = fp >= TG_FP_AFTER_SWITCH_BEFORE_FSYNC;
        const uint8_t *want = expect_new ? dnew : dold;
        tg_snapshot s;
        int rr = tg_read_current(d, &s); /* reader before recovery */
        CHECK(rr == TG_OK && s.gen == (expect_new ? 2u : 1u) && !memcmp(s.digest, want, TG_DIGEST),
              "fp %s torn=%d: reader before recovery sees %s (rc %s)", tg_failpoint_name((tg_failpoint)fp), torn,
              expect_new ? "NEW" : "OLD", tg_err_name(rr));
        tg_snapshot_free(&s);

        tg_recovery rec = {0, 0, 0};
        CHECK(tg_open(d, &st, &rec) == TG_OK, "fp %s: recovery open", tg_failpoint_name((tg_failpoint)fp));
        if (!st) continue;
        CHECK(tg_committed(st)->gen == (expect_new ? 2u : 1u) && !memcmp(tg_committed(st)->digest, want, TG_DIGEST),
              "fp %s torn=%d: recovered state is exactly %s", tg_failpoint_name((tg_failpoint)fp), torn, expect_new ? "NEW" : "OLD");
        CHECK(!file_exists(d, "CURRENT.tmp"), "pointer tmp swept");
        CHECK(expect_new || !file_exists(d, "gen-00000000000000000002.bin"), "orphan NEW generation swept");
        if (!expect_new && fp >= TG_FP_BEFORE_SHADOW_WRITE)
            CHECK(rec.dispatch_truncated == 192, "fp %s: uncommitted dispatch record dropped (%llu)",
                  tg_failpoint_name((tg_failpoint)fp), (unsigned long long)rec.dispatch_truncated);
        printf("crash fp=%s torn=%d recovered=%s tmp=%u orphan=%u dispatch_dropped=%llu\n",
               tg_failpoint_name((tg_failpoint)fp), torn, expect_new ? "NEW" : "OLD", rec.removed_tmp,
               rec.removed_orphan_gen, (unsigned long long)rec.dispatch_truncated);
        /* Store remains usable; redoing the step from OLD lands on the same NEW digest. */
        if (!expect_new) {
            CHECK(sgd_commit(st, t, N_SMALL, MOM, 2) == TG_OK && !memcmp(tg_committed(st)->digest, dnew, TG_DIGEST),
                  "fp %s: redo from OLD reproduces NEW", tg_failpoint_name((tg_failpoint)fp));
        }
        tg_close(st);
        uint64_t nv;
        CHECK(tg_verify_history(d, &nv) == TG_OK && nv == 3, "history after crash");
    }
}

/* ---- 4. reader racing a writer --------------------------------------- */
static void t_reader_race(const float *t)
{
    char d[1100];
    newdir(d, sizeof d, "race");
    make_store(d, N_SMALL, 1);
    int stop[2], res[2];
    if (pipe(stop) || pipe(res)) { CHECK(0, "pipe"); return; }
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        close(stop[1]); close(res[0]);
        fcntl(stop[0], F_SETFL, O_NONBLOCK);
        long reads = 0, bad = 0, backwards = 0, last = -1;
        for (;;) {
            char c;
            ssize_t r = read(stop[0], &c, 1);
            tg_snapshot s;
            int rc = tg_read_current(d, &s);
            if (rc) bad++;
            else { if ((long)s.gen < last) backwards++; last = (long)s.gen; tg_snapshot_free(&s); }
            reads++;
            if (r == 0) break;
        }
        long out[4] = {reads, bad, backwards, last};
        (void)!write(res[1], out, sizeof out);
        _exit(0);
    }
    close(stop[0]); close(res[1]);
    tg_store *st;
    CHECK(tg_open(d, &st, NULL) == TG_OK, "open");
    for (int k = 0; k < 150 && st; k++) {
        sgd_commit(st, t, N_SMALL, MOM, (uint64_t)k);
        if (k % 8 == 0) usleep(200);
    }
    tg_close(st);
    close(stop[1]);
    long out[4] = {0, -1, -1, -1};
    CHECK(read(res[0], out, sizeof out) == (ssize_t)sizeof out, "reader result");
    waitpid(pid, NULL, 0);
    printf("reader_race reads %ld bad %ld backwards %ld last_gen %ld\n", out[0], out[1], out[2], out[3]);
    CHECK(out[0] > 0 && out[1] == 0 && out[2] == 0 && out[3] == 150, "reader saw only whole, verified, monotonic generations");
}

/* ---- 5. negatives ------------------------------------------------------- */
static void t_negatives(const float *t)
{
    char d[1100], p[1200];
    tg_store *st, *st2;
    tg_shadow a, b;
    float *g = malloc(N_SMALL * 4);

    /* corrupted shadow */
    newdir(d, sizeof d, "neg-shadow");
    make_store(d, N_SMALL, 1);
    tg_open(d, &st, NULL);
    off_t log0 = file_size(d, "dispatch.log");
    tg_failpoint_arm(TG_FP_AFTER_SHADOW_WRITE, TG_FPMODE_CORRUPT);
    int r = sgd_commit(st, t, N_SMALL, MOM, 1);
    tg_failpoint_disarm();
    CHECK(r == TG_E_SHADOW, "corrupted shadow refused (%s)", tg_err_name(r));
    CHECK(tg_committed(st)->gen == 0 && file_size(d, "dispatch.log") == log0, "state and dispatch log unchanged");
    CHECK(!file_exists(d, "shadow-00000000000000000001.tmp") && !file_exists(d, "gen-00000000000000000001.bin"), "no shadow left");
    CHECK(sgd_commit(st, t, N_SMALL, MOM, 1) == TG_OK, "clean retry commits");
    CHECK(count_refusals(d, "E_SHADOW") == 1, "E_SHADOW recorded");

    /* validation reject */
    tg_shadow_begin(st, &a);
    grad_of((const float *)(const void *)tg_committed(st)->params, t, g, N_SMALL);
    tg_sgd_step(st, &a, g, N_SMALL, LR, MOM, 9);
    r = tg_commit(st, &a, v_reject, NULL);
    CHECK(r == TG_E_REJECT && tg_committed(st)->gen == 1, "validation reject refused");
    CHECK(count_refusals(d, "E_REJECT") == 1, "E_REJECT recorded");
    CHECK(tg_commit(st, &a, NULL, NULL) == TG_E_STATE, "rejected shadow cannot be committed later");

    /* double switch */
    tg_shadow_begin(st, &a);
    tg_sgd_step(st, &a, g, N_SMALL, LR, MOM, 10);
    CHECK(tg_commit(st, &a, NULL, NULL) == TG_OK && tg_committed(st)->gen == 2, "first switch");
    CHECK(tg_commit(st, &a, NULL, NULL) == TG_E_STATE && tg_committed(st)->gen == 2, "double switch refused");
    CHECK(count_refusals(d, "E_STATE") == 2, "E_STATE recorded");

    /* stale shadow */
    tg_shadow_begin(st, &a);
    tg_shadow_begin(st, &b);
    CHECK(tg_commit(st, &a, NULL, NULL) == TG_OK, "A commits");
    CHECK(tg_commit(st, &b, NULL, NULL) == TG_E_STALE && tg_committed(st)->gen == 3, "stale B refused");
    CHECK(count_refusals(d, "E_STALE") == 1, "E_STALE recorded");

    /* second writer, re-create */
    CHECK(tg_open(d, &st2, NULL) == TG_E_LOCKED, "second writer refused");
    tg_close(st);
    CHECK(make_store(d, N_SMALL, 1) == TG_E_EXISTS, "re-create refused");
    uint64_t nv;
    CHECK(tg_verify_history(d, &nv) == TG_OK && nv == 4, "history 4 after refusals");

    /* truncated generation file */
    newdir(d, sizeof d, "neg-trunc");
    make_store(d, N_SMALL, 1);
    tg_open(d, &st, NULL); sgd_commit(st, t, N_SMALL, MOM, 1); tg_close(st);
    gen_file(p, sizeof p, d, 1);
    struct stat sb; stat(p, &sb);
    CHECK(truncate(p, sb.st_size - 10) == 0, "truncate");
    tg_snapshot s;
    CHECK(tg_read_current(d, &s) == TG_E_TRUNC, "reader refuses truncated");
    CHECK(tg_open(d, &st, NULL) == TG_E_TRUNC, "open refuses truncated");
    CHECK(count_refusals(d, "E_TRUNC") == 1, "E_TRUNC recorded");
    CHECK(truncate(p, 100) == 0 && tg_open(d, &st, NULL) == TG_E_TRUNC, "header-truncated refused");

    /* digest mismatch: payload byte flipped; then pointer names another digest */
    newdir(d, sizeof d, "neg-digest");
    make_store(d, N_SMALL, 1);
    tg_open(d, &st, NULL); sgd_commit(st, t, N_SMALL, MOM, 1); tg_close(st);
    gen_file(p, sizeof p, d, 1);
    flip_byte(p, 152 + 777);
    CHECK(tg_read_current(d, &s) == TG_E_DIGEST, "reader refuses flipped payload");
    CHECK(tg_open(d, &st, NULL) == TG_E_DIGEST, "open refuses flipped payload");
    flip_byte(p, 152 + 777);
    CHECK(tg_open(d, &st, NULL) == TG_OK, "restored file opens");
    tg_close(st);
    snprintf(p, sizeof p, "%s/CURRENT", d);
    flip_byte(p, 40); /* inside the hex digest: '0'..'9'/'a'..'f' xor 1 stays hex */
    CHECK(tg_open(d, &st, NULL) == TG_E_DIGEST, "pointer digest mismatch refused");
    CHECK(count_refusals(d, "E_DIGEST") == 2, "E_DIGEST recorded");

    /* dispatch log tamper and truncation */
    newdir(d, sizeof d, "neg-chain");
    make_store(d, N_SMALL, 1);
    tg_open(d, &st, NULL); sgd_commit(st, t, N_SMALL, MOM, 1); sgd_commit(st, t, N_SMALL, MOM, 2); tg_close(st);
    snprintf(p, sizeof p, "%s/dispatch.log", d);
    off_t committed_len = file_size(d, "dispatch.log");
    {   /* uncommitted tail present: a refused open must not modify the store */
        int fd = open(p, O_WRONLY | O_APPEND);
        uint8_t junk[192]; memset(junk, 0xab, sizeof junk);
        CHECK(fd >= 0 && write(fd, junk, sizeof junk) == (ssize_t)sizeof junk, "append tail");
        if (fd >= 0) close(fd);
    }
    flip_byte(p, 192 + 33);
    CHECK(tg_open(d, &st, NULL) == TG_E_CHAIN, "tampered dispatch record refused");
    CHECK(file_size(d, "dispatch.log") == committed_len + 192, "refused open left the dispatch log untouched");
    flip_byte(p, 192 + 33);
    CHECK(tg_open(d, &st, NULL) == TG_OK && file_size(d, "dispatch.log") == committed_len, "valid open drops the tail");
    tg_close(st);
    CHECK(truncate(p, 192) == 0 && tg_open(d, &st, NULL) == TG_E_CHAIN, "truncated dispatch log refused");
    CHECK(count_refusals(d, "E_CHAIN") == 2, "E_CHAIN recorded");

    /* bad updater arguments */
    newdir(d, sizeof d, "neg-args");
    make_store(d, N_SMALL, 0);
    tg_open(d, &st, NULL);
    tg_shadow_begin(st, &a);
    CHECK(tg_sgd_step(st, &a, g, N_SMALL, LR, MOM, 1) == TG_E_ARG, "momentum without optimizer state refused");
    CHECK(tg_sgd_step(st, &a, g, N_SMALL - 1, LR, 0, 1) == TG_E_ARG, "size mismatch refused");
    CHECK(tg_sgd_step(st, &a, g, N_SMALL, NAN, 0, 1) == TG_E_ARG, "NaN lr refused");
    tg_shadow_discard(&a);
    CHECK(tg_commit(st, &a, NULL, NULL) == TG_E_STATE, "discarded shadow refused");
    tg_close(st);
    free(g);
}

/* ---- 6. provenance cost ------------------------------------------------- */
static void t_cost(void)
{
    size_t sizes[2] = {1024, 262144};
    double per[2];
    for (int k = 0; k < 2; k++) {
        char d[1100];
        newdir(d, sizeof d, "cost");
        size_t n = sizes[k];
        float *tt = malloc(n * 4); target(tt, n);
        make_store(d, n, 1);
        tg_store *st; tg_open(d, &st, NULL);
        for (int i = 0; i < 3; i++) sgd_commit(st, tt, n, MOM, (uint64_t)i);
        const tg_stats *s = tg_get_stats(st);
        per[k] = (double)s->dispatch_hashed_bytes / (double)s->dispatch_records;
        printf("cost n=%zu dispatch_records %llu dispatch_hashed_per_record %.0f commit_hashed_bytes %llu\n", n,
               (unsigned long long)s->dispatch_records, per[k], (unsigned long long)s->commit_hashed_bytes);
        CHECK(s->commit_hashed_bytes == 3 * (120 + 2 * n * 4), "tier b hashes the full state once per commit");
        tg_close(st);
        free(tt);
    }
    CHECK(per[0] == 192 && per[1] == 192, "tier a per-record hashing is constant (independent of model size)");
}

/* ---- 7. replay ---------------------------------------------------------- */
static void run_seq(const char *d, const float *t, int steps, uint8_t out[TG_DIGEST], uint8_t head[TG_DIGEST])
{
    make_store(d, N_SMALL, 1);
    tg_store *st;
    if (tg_open(d, &st, NULL)) { memset(out, 0, TG_DIGEST); return; }
    for (int k = 0; k < steps; k++) sgd_commit(st, t, N_SMALL, k < steps / 2 ? MOM : 0.5f, 1000 + (uint64_t)k);
    memcpy(out, tg_committed(st)->digest, TG_DIGEST);
    memcpy(head, tg_committed(st)->dispatch_head, TG_DIGEST);
    tg_close(st);
}

static void t_replay(const float *t)
{
    char a[1100], b[1100], dk[1200], hx[65], hh[65];
    uint8_t da[TG_DIGEST], db[TG_DIGEST], ha[TG_DIGEST], hb[TG_DIGEST];
    const int STEPS = 25;
    newdir(a, sizeof a, "replay-a");
    newdir(b, sizeof b, "replay-b");
    run_seq(a, t, STEPS, da, ha);
    run_seq(b, t, STEPS, db, hb);
    CHECK(!memcmp(da, db, TG_DIGEST) && !memcmp(ha, hb, TG_DIGEST),
          "same sequence from G0 gives byte-identical G_n and dispatch chain");
    tg_hex(da, hx); tg_hex(ha, hh);
    printf("replay_digest %s\nreplay_dispatch_head %s\n", hx, hh);

    /* Each committed step k re-derived from G_{k-1} (digest-verified, and equal
     * to the base digest its dispatch record references) plus the recorded
     * scalar arguments: the new parameters + optimizer state must be
     * byte-identical to the committed G_k payload. */
    int ok = 0;
    snprintf(dk, sizeof dk, "%s-stepcheck", a);
    for (uint64_t k = 1; k <= (uint64_t)STEPS; k++) {
        tg_snapshot gp, gk;
        tg_dispatch_rec rec;
        if (tg_read_gen(a, k, NULL, &gk)) break;
        int good = tg_read_gen(a, k - 1, gk.prev_digest, &gp) == TG_OK
                && tg_dispatch_read(a, k - 1, &rec) == TG_OK
                && rec.op_id == TG_OP_SGD && rec.step == k && rec.base_gen == k - 1
                && rec.arg1 == 1000 + k - 1 && !memcmp(rec.base_digest, gp.digest, TG_DIGEST);
        if (good) {
            float lr, mom;
            uint32_t u = (uint32_t)rec.arg0; memcpy(&lr, &u, 4);
            u = (uint32_t)(rec.arg0 >> 32); memcpy(&mom, &u, 4);
            rmtree(dk);
            tg_store *st;
            good = tg_create(dk, gp.params, gp.param_bytes, gp.opt, gp.opt_bytes) == TG_OK
                && tg_open(dk, &st, NULL) == TG_OK;
            if (good) {
                tg_shadow sh;
                float *g = malloc(N_SMALL * 4);
                tg_shadow_begin(st, &sh);
                grad_of((const float *)(const void *)gp.params, t, g, N_SMALL);
                good = tg_sgd_step(st, &sh, g, N_SMALL, lr, mom, rec.arg1) == TG_OK
                    && tg_commit(st, &sh, NULL, NULL) == TG_OK
                    && tg_committed(st)->param_bytes == gk.param_bytes && tg_committed(st)->opt_bytes == gk.opt_bytes
                    && !memcmp(tg_committed(st)->params, gk.params, gk.param_bytes)
                    && !memcmp(tg_committed(st)->opt, gk.opt, gk.opt_bytes);
                free(g);
                tg_close(st);
            }
            tg_snapshot_free(&gp);
        }
        tg_snapshot_free(&gk);
        if (good) ok++;
    }
    rmtree(dk);
    printf("replay_steps_reverified %d/%d\n", ok, STEPS);
    CHECK(ok == STEPS, "each committed step re-derived from its base generation + dispatch record (%d/%d)", ok, STEPS);
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: test_train <scratch-dir>\n"); return 2; }
    snprintf(g_base, sizeof g_base, "%s", argv[1]);
    rmtree(g_base);
    if (mkdir(g_base, 0755) && errno != EEXIST) { perror("mkdir"); return 2; }
    float *t = malloc(N_SMALL * 4);
    target(t, N_SMALL);
    t_basic(t);
    t_training(t);
    t_crash(t);
    t_reader_race(t);
    t_negatives(t);
    t_cost();
    t_replay(t);
    free(t);
    printf("checks %d failed %d\n", g_checks, g_fail);
    if (g_fail) { printf("M22_SUBSTRATE_TESTS_FAIL\n"); return 1; }
    rmtree(g_base);
    printf("M22_SUBSTRATE_TESTS_PASS\n");
    return 0;
}
