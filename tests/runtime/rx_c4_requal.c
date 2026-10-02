/*
 * rx_c4_requal.c -- C4 local-runtime requalification, interruption and recovery.
 *
 * ONE repeatable task: the verified-scale ledger. Three goals (inputs 5, 6, 7)
 * each go through the whole COMPOSITION-2 path in one World:
 *   World -> J-Space (fork two staged branches) -> AEGIS (contract check,
 *   one candidate deliberately breaks the contract) -> World commit (seal)
 *   -> settle -> Cortex (claims, evidence, promotion), durable on disk.
 * The committed state after each goal is a signature (World state ref, J-Space
 * content digest, state result, Cortex promotion count, chain verified, exactly
 * one live branch). The baseline run records the signature after goal 0..3.
 *
 * Injections (each in a fresh directory):
 *   crash    : fault point p (one per stage boundary) x goal g: the child exits
 *              at the point (test-build hook), the parent reopens.
 *   kill     : the child runs the whole task, the parent SIGKILLs it after a
 *              delay (a sweep across the run), the parent reopens.
 *   corrupt  : the finished directory is damaged (truncate, flip a byte, zero
 *              fill, delete, append junk) in the Cortex journal, in every
 *              J-Space file, and in machine.id, then reopened; plus a foreign
 *              machine identity, and a single-byte flip sweep of the J-Space
 *              checkpoint, machine.id and (every 7th byte of) the journal.
 * Acceptance, written before the run:
 *   crash/kill : reopen succeeds and the signature equals exactly one baseline
 *                signature (crash: the one the point implies); the task then
 *                resumes from it and reaches the baseline final signature, also
 *                after one more close/reopen.
 *   corrupt    : reopen REFUSES (nonzero, repeatable); or the damage is
 *                detected when the state is read (J-Space corrupt error); or
 *                the state and every Cortex record equal the pristine reopen
 *                (UNCHANGED, benign damage); or a torn-looking tail was cut and
 *                re-completed with a visible RECOVERED admission and the same
 *                state and promotions (REPAIRED_VISIBLY). Anything else is a
 *                silent divergence and FAILS.
 *
 * Needs -DAIEN_TEST_BUILD=1 (crash points). usage: rx_c4_requal <receipt.json>
 */
#include "rx_compose_fixture.h"
#include "sha256.h"

#include <dirent.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef RXC_TEST_HOOKS
#error "rx_c4_requal needs -DAIEN_TEST_BUILD=1"
#endif

#define NGOAL 3
static const uint64_t GOALS[NGOAL] = { 5, 6, 7 };

static Fx g_fx;
static RxCompose g_c;
static char g_base[128];

typedef struct {
    int ok;                 /* chain verified, one live branch, digest readable */
    int digest_rc;          /* js_branch_content_digest result */
    JsBranchRef ref;
    uint8_t dg[32];
    uint64_t result;
    uint32_t promos;
    uint64_t nrec;
    uint32_t radm;          /* RECOVERED admissions: records re-completed after a torn tail */
    uint8_t rec[32];        /* Cortex record digest (all records, timing excluded) */
} Sig;
static Sig BASE[NGOAL + 1];
static uint8_t REOPEN_REC[32];   /* record digest after ONE reopen of the pristine finished directory */
static uint64_t REOPEN_NREC;     /* and its record count */

typedef struct {
    char name[96];
    char klass[24];         /* RECOVERED / REFUSED / DETECTED_ON_READ / UNCHANGED / DIVERGED / FAULT_NOT_FIRED */
    int pass;
    char detail[200];
} Case;
#define MAXCASE 400
static Case g_case[MAXCASE];
static int g_ncase;

static int rm_one(const char *p, const struct stat *sb, int fl, struct FTW *w) {
    (void)sb; (void)fl; (void)w;
    return remove(p);
}
static void rmtree(const char *p) { nftw(p, rm_one, 16, FTW_DEPTH | FTW_PHYS); }

static void reset_hooks(void) {
    g_c.test.fault_point = RXC_FP_NONE;
    g_c.test.fault_crash = 0;
    g_c.test.fault_k = 0;
    g_c.test.rogue_candidate = 0;
}

static int sig_eq(const Sig *a, const Sig *b) {
    /* The branch ref (slot id, generation) is an allocator artifact that differs
     * after a crash; the state is its content digest, the World result and the
     * Cortex promotion count. */
    return a->result == b->result &&
           a->promos == b->promos && memcmp(a->dg, b->dg, 32) == 0;
}

/* Which fields of a differ from b (for failure details). */
static void sig_diff(const Sig *a, const Sig *b, char *buf, size_t n) {
    snprintf(buf, n, "ref %u/%u vs %u/%u; result %llu vs %llu; promos %u vs %u; digest %s; ok=%d",
             a->ref.id, a->ref.gen, b->ref.id, b->ref.gen, (unsigned long long)a->result,
             (unsigned long long)b->result, a->promos, b->promos,
             memcmp(a->dg, b->dg, 32) ? "differs" : "same", a->ok);
}

static void sig_take(RxCompose *c, Sig *s) {
    memset(s, 0, sizeof *s);
    s->ref = rx_compose_state(c);
    s->digest_rc = js_branch_content_digest(&c->js, s->ref.id, s->dg);
    RxObject so;
    memset(&so, 0, sizeof so);
    rx_world_read(&c->w, c->state, &so);
    s->result = so.field[RXC_S_RESULT];
    s->promos = fx_count(&c->cx, CX_K_PROMOTION, UINT64_MAX);
    rx_compose_record_digest(&c->cx, s->rec);
    s->nrec = c->cx.n;
    s->radm = fx_count(&c->cx, CX_K_ADMISSION, RXC_ADMIT_RECOVERED);
    s->ok = cx_verify_chain(&c->cx) == CX_OK && fx_live_branches(&c->js) == 1 && s->digest_rc == JS_OK;
}

static int open_dir(const char *d) {
    memset(&g_c, 0, sizeof g_c);
    return fx_open(&g_fx, &g_c, d, 1);
}

/* Index j of the baseline signature equal to s, or -1. */
static int match_base(const Sig *s) {
    for (int j = 0; j <= NGOAL; j++)
        if (s->ok && sig_eq(s, &BASE[j])) return j;
    return -1;
}

static void add_case(const char *name, const char *klass, int pass, const char *fmt, ...) {
    if (g_ncase >= MAXCASE) { fprintf(stderr, "too many cases\n"); exit(2); }
    Case *c = &g_case[g_ncase++];
    snprintf(c->name, sizeof c->name, "%s", name);
    snprintf(c->klass, sizeof c->klass, "%s", klass);
    c->pass = pass;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->detail, sizeof c->detail, fmt, ap);
    va_end(ap);
}

/* Baseline: run the whole task once in `d`, filling sig[]. */
static int run_task(const char *d, Sig *sig) {
    if (open_dir(d) != RX_OK) return -1;
    sig_take(&g_c, &sig[0]);
    for (int i = 0; i < NGOAL; i++) {
        RxcResult o;
        fx_a_bad = 1;
        int rc = fx_run(&g_fx, &g_c, GOALS[i], &o);
        fx_a_bad = 0;
        if (rc != RX_OK || o.outcome != RXC_OUT_COMMITTED || o.result != GOALS[i] * 3 + 1) {
            fx_close(&g_fx, &g_c);
            return -2;
        }
        sig_take(&g_c, &sig[i + 1]);
    }
    fx_close(&g_fx, &g_c);
    return 0;
}

/* After a recovery landed on baseline index j: finish the task from goal j,
 * check the final signature, close, reopen, check again. Returns 1 on success. */
static int resume_and_check(const char *d, int j, char *why, size_t wn) {
    for (int i = j; i < NGOAL; i++) {
        RxcResult o;
        fx_a_bad = 1;
        int rc = fx_run(&g_fx, &g_c, GOALS[i], &o);
        fx_a_bad = 0;
        if (rc != RX_OK || o.outcome != RXC_OUT_COMMITTED) {
            snprintf(why, wn, "resume goal %d rc=%d outcome=%d", i, rc, o.outcome);
            fx_close(&g_fx, &g_c);
            return 0;
        }
    }
    Sig f;
    sig_take(&g_c, &f);
    fx_close(&g_fx, &g_c);
    if (!f.ok || !sig_eq(&f, &BASE[NGOAL])) {
        { char dd[160]; sig_diff(&f, &BASE[NGOAL], dd, sizeof dd); snprintf(why, wn, "final differs: %.150s", dd); }
        return 0;
    }
    if (open_dir(d) != RX_OK) { snprintf(why, wn, "second reopen failed"); return 0; }
    sig_take(&g_c, &f);
    fx_close(&g_fx, &g_c);
    if (!f.ok || !sig_eq(&f, &BASE[NGOAL])) { snprintf(why, wn, "second reopen differs"); return 0; }
    return 1;
}

/* Reopen after an interruption; acceptance: recovered signature is baseline
 * index `expect` (-1 = any index), then resume reaches the baseline final. */
static void recover_case(const char *name, const char *d, int expect) {
    char why[200] = "";
    int rc = open_dir(d);
    if (rc != RX_OK) {
        add_case(name, "REFUSED", 0, "interruption must be recoverable, open rc=%d", rc);
        return;
    }
    Sig s;
    sig_take(&g_c, &s);
    int j = match_base(&s);
    if (j < 0) {
        fx_close(&g_fx, &g_c);
        { char dd[160]; sig_diff(&s, &BASE[0], dd, sizeof dd); add_case(name, "DIVERGED", 0, "matches no baseline; vs genesis: %s", dd); }
        return;
    }
    if (expect >= 0 && j != expect) {
        fx_close(&g_fx, &g_c);
        add_case(name, "DIVERGED", 0, "recovered to baseline %d, implied %d", j, expect);
        return;
    }
    if (!resume_and_check(d, j, why, sizeof why)) {
        add_case(name, "DIVERGED", 0, "%s", why);
        return;
    }
    add_case(name, "RECOVERED", 1, "recovered to baseline %d, resumed to final, 2nd reopen equal", j);
}

static const char *fp_name[RXC_FP_END] = { "none", "before_fork", "candidate", "before_commit",
                                           "after_validation", "seal", "reclaim", "cortex" };

static void mkcasedir(char *out, size_t n, const char *leaf) {
    snprintf(out, n, "%s/%s", g_base, leaf);
    rmtree(out);
}

static void crash_case(int point, int g) {
    char d[200], name[96], leaf[64];
    snprintf(name, sizeof name, "crash/%s/goal%d", fp_name[point], g);
    snprintf(leaf, sizeof leaf, "crash_%d_%d", point, g);
    mkcasedir(d, sizeof d, leaf);
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        if (open_dir(d) != RX_OK) _exit(2);
        for (int i = 0; i < g; i++) {
            RxcResult o;
            fx_a_bad = 1;
            int rc = fx_run(&g_fx, &g_c, GOALS[i], &o);
            fx_a_bad = 0;
            if (rc != RX_OK || o.outcome != RXC_OUT_COMMITTED) _exit(4);
        }
        reset_hooks();
        g_c.test.fault_point = point;
        g_c.test.fault_crash = 1;
        g_c.test.fault_k = 0;
        RxcResult o;
        fx_a_bad = 1;
        fx_run(&g_fx, &g_c, GOALS[g], &o);
        _exit(3);   /* fault did not fire */
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (!(WIFEXITED(st) && WEXITSTATUS(st) == RXC_CRASH_EXIT)) {
        add_case(name, "FAULT_NOT_FIRED", 0, "child status %d", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        rmtree(d);
        return;
    }
    recover_case(name, d, point == RXC_FP_CORTEX ? g + 1 : g);
    rmtree(d);
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static void child_full_run(const char *d) {
    Sig tmp[NGOAL + 1];
    _exit(run_task(d, tmp) == 0 ? 0 : 5);
}

static void kill_sweep(int n) {
    char d[200];
    mkcasedir(d, sizeof d, "kill_t");
    fflush(NULL);
    double t0 = now_s();
    pid_t pid = fork();
    if (pid == 0) child_full_run(d);
    int st;
    waitpid(pid, &st, 0);
    double T = now_s() - t0;
    rmtree(d);
    for (int i = 0; i < n; i++) {
        char name[96], leaf[64];
        snprintf(name, sizeof name, "kill/sigkill@%d_of_%d", i + 1, n);
        snprintf(leaf, sizeof leaf, "kill_%d", i);
        mkcasedir(d, sizeof d, leaf);
        fflush(NULL);
        pid = fork();
        if (pid == 0) child_full_run(d);
        double delay = T * 1.1 * (i + 1) / n;
        struct timespec ts = { (time_t)delay, (long)((delay - (time_t)delay) * 1e9) };
        nanosleep(&ts, NULL);
        kill(pid, SIGKILL);
        waitpid(pid, &st, 0);
        /* A child that already finished is a valid (complete) case. */
        recover_case(name, d, -1);
        rmtree(d);
    }
}

/* ---- corruption of a finished directory ---------------------------------- */

typedef enum { OP_TRUNC_BY, OP_TRUNC_TO, OP_TRUNC_HALF, OP_FLIP_MID, OP_FLIP_FIRST, OP_FLIP_LAST,
               OP_ZERO_TAIL, OP_DELETE, OP_APPEND_JUNK, OP_FLIP_PM, OP_FLIP_ABS } Op;
static const char *op_name[] = { "truncate_by", "truncate_to", "truncate_half", "flip_mid",
                                 "flip_first", "flip_last", "zero_tail", "delete", "append_junk", "flip_permille", "flip_byte" };

static int apply_op(const char *path, Op op, long arg) {
    struct stat sb;
    if (stat(path, &sb) != 0) return -1;
    off_t n = sb.st_size;
    if (op == OP_DELETE) return unlink(path);
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    int rc = 0;
    uint8_t b;
    switch (op) {
    case OP_TRUNC_BY: rc = ftruncate(fd, n > arg ? n - arg : 0); break;
    case OP_TRUNC_TO: rc = ftruncate(fd, arg); break;
    case OP_TRUNC_HALF: rc = ftruncate(fd, n / 2); break;
    case OP_FLIP_MID: case OP_FLIP_FIRST: case OP_FLIP_LAST: case OP_FLIP_PM: case OP_FLIP_ABS: {
        off_t at = op == OP_FLIP_MID ? n / 2 : op == OP_FLIP_FIRST ? 0 : op == OP_FLIP_PM ? n * arg / 1000 : op == OP_FLIP_ABS ? arg : n - 1;
        if (n == 0 || pread(fd, &b, 1, at) != 1) { rc = -1; break; }
        b ^= 0x40;
        rc = pwrite(fd, &b, 1, at) == 1 ? 0 : -1;
        break;
    }
    case OP_ZERO_TAIL: {
        uint8_t z[64] = { 0 };
        off_t at = n > 64 ? n - 64 : 0;
        rc = pwrite(fd, z, n > 64 ? 64 : (size_t)n, at) >= 0 ? 0 : -1;
        break;
    }
    case OP_APPEND_JUNK: {
        uint8_t j[64];
        for (int i = 0; i < 64; i++) j[i] = (uint8_t)(0xA5 ^ i);
        rc = pwrite(fd, j, sizeof j, n) == (ssize_t)sizeof j ? 0 : -1;
        break;
    }
    default: rc = -1;
    }
    if (rc == 0) fsync(fd);
    close(fd);
    return rc;
}

/* Open a damaged finished directory and classify what happened. Returns 1 if
 * acceptable (refused stably, detected on read, or nothing committed changed). */
static int classify_dir(const char *d, const char **klass, char *detail, size_t dn) {
    int rc = open_dir(d);
    if (rc != RX_OK) {
        int rc2 = open_dir(d);   /* a refusal must be stable */
        if (rc2 == RX_OK) fx_close(&g_fx, &g_c);
        *klass = "REFUSED";
        snprintf(detail, dn, "open rc=%d, again rc=%d", rc, rc2);
        return rc2 == rc;
    }
    Sig s;
    sig_take(&g_c, &s);
    fx_close(&g_fx, &g_c);
    if (s.digest_rc != JS_OK) {
        *klass = "DETECTED_ON_READ";
        snprintf(detail, dn, "open ok, state read refused (js rc=%d)", s.digest_rc);
        return 1;
    }
    if (s.ok && sig_eq(&s, &BASE[NGOAL]) && memcmp(s.rec, REOPEN_REC, 32) == 0) {
        *klass = "UNCHANGED";
        snprintf(detail, dn, "open ok, state and every Cortex record equal the committed final state");
        return 1;
    }
    /* A flip that makes a record look torn gets the log cut there; recovery repairs
     * it by re-completing the composition records and leaves a RECOVERED admission
     * as visible evidence. Same state and promotions, bounded record count. */
    if (s.ok && sig_eq(&s, &BASE[NGOAL]) && s.radm >= 1 && s.nrec >= REOPEN_NREC && s.nrec <= REOPEN_NREC + 2) {
        *klass = "REPAIRED_VISIBLY";
        snprintf(detail, dn, "torn-looking tail cut and re-completed: state and promotions equal, %u RECOVERED admission(s), records %llu vs %llu",
                 s.radm, (unsigned long long)s.nrec, (unsigned long long)REOPEN_NREC);
        return 1;
    }
    *klass = "DIVERGED";
    snprintf(detail, dn, "open ok but state or Cortex records differ from the committed final state (j=%d result=%llu promos=%u ok=%d records=%llu vs pristine-reopen %llu)",
             match_base(&s), (unsigned long long)s.result, s.promos, s.ok, (unsigned long long)s.nrec,
             (unsigned long long)REOPEN_NREC);
    return 0;
}

static void corrupt_case(const char *rel, Op op, long arg) {
    char d[200], name[128], path[400], detail[220];
    const char *klass;
    if (op == OP_TRUNC_BY || op == OP_TRUNC_TO || op == OP_FLIP_PM || op == OP_FLIP_ABS) snprintf(name, sizeof name, "corrupt/%s/%s_%ld", rel, op_name[op], arg);
    else snprintf(name, sizeof name, "corrupt/%s/%s", rel, op_name[op]);
    mkcasedir(d, sizeof d, "corrupt");
    Sig tmp[NGOAL + 1];
    if (run_task(d, tmp) != 0) { add_case(name, "DIVERGED", 0, "could not build the finished directory"); return; }
    snprintf(path, sizeof path, "%s/%s", d, rel);
    /* A torn Cortex tail is what a crash leaves; classify_dir accepts it only as
     * REPAIRED_VISIBLY. Every other damage must leave every record byte-identical
     * or be refused. */
    if (apply_op(path, op, arg) != 0) {
        add_case(name, "NOT_APPLICABLE", 1, "operation not applicable (file is empty or absent in this task)");
        rmtree(d);
        return;
    }
    int ok = classify_dir(d, &klass, detail, sizeof detail);
    add_case(name, klass, ok, "%s", detail);
    rmtree(d);
}

/* Exhaustive single-byte sweep of one file: flip every `stride`-th byte (each
 * from a pristine copy of the directory), reopen, classify. One summary case. */
typedef struct { char rel[300]; uint8_t *b; size_t n; } Snap;
static int snap_dir(const char *d, Snap *s, int max) {
    static const char *fixed[] = { "machine.id", "cortex.cx" };
    char names[16][300];
    int k = 0;
    for (size_t i = 0; i < 2; i++) snprintf(names[k++], sizeof names[0], "%s", fixed[i]);
    char jd[260];
    snprintf(jd, sizeof jd, "%s/jspace", d);
    DIR *dir = opendir(jd);
    struct dirent *e;
    while (dir && (e = readdir(dir)) && k < 14)
        if (e->d_name[0] != '.') snprintf(names[k++], sizeof names[0], "jspace/%.200s", e->d_name);
    if (dir) closedir(dir);
    int ns = 0;
    for (int i = 0; i < k && ns < max; i++) {
        char p[600];
        snprintf(p, sizeof p, "%.200s/%.290s", d, names[i]);
        struct stat sb;
        if (stat(p, &sb) != 0) continue;
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        s[ns].b = malloc(sb.st_size ? (size_t)sb.st_size : 1);
        s[ns].n = fread(s[ns].b, 1, sb.st_size, f);
        fclose(f);
        snprintf(s[ns].rel, sizeof s[ns].rel, "%.290s", names[i]);
        ns++;
    }
    return ns;
}
static void restore_dir(const char *d, const Snap *s, int ns) {
    for (int i = 0; i < ns; i++) {
        char p[600];
        snprintf(p, sizeof p, "%.200s/%.290s", d, s[i].rel);
        FILE *f = fopen(p, "wb");
        if (!f) continue;
        fwrite(s[i].b, 1, s[i].n, f);
        fclose(f);
    }
}
static void byte_sweep(const char *rel, size_t stride) {
    char d[200], name[128], path[600], detail[220];
    snprintf(name, sizeof name, "sweep/%s/flip_every_%zu_bytes", rel, stride);
    mkcasedir(d, sizeof d, "sweep");
    Sig tmp[NGOAL + 1];
    if (run_task(d, tmp) != 0) { add_case(name, "DIVERGED", 0, "could not build the finished directory"); return; }
    Snap snap[16];
    int ns = snap_dir(d, snap, 16);
    snprintf(path, sizeof path, "%s/%s", d, rel);
    struct stat sb;
    if (stat(path, &sb) != 0 || sb.st_size == 0) {
        add_case(name, "NOT_APPLICABLE", 1, "file is empty or absent in this task");
    } else {
        size_t n = (size_t)sb.st_size;
        unsigned refused = 0, detected = 0, unchanged = 0, diverged = 0, tried = 0;
        char first_bad[120] = "";
        for (size_t i = 0; i < n; i += stride) {
            restore_dir(d, snap, ns);
            if (apply_op(path, OP_FLIP_ABS, (long)i) != 0) continue;
            const char *klass;
            int ok = classify_dir(d, &klass, detail, sizeof detail);
            tried++;
            if (!ok) { diverged++; if (getenv("C4_VERBOSE")) fprintf(stderr, "sweep divergence at %zu: %s\n", i, detail); if (!first_bad[0]) snprintf(first_bad, sizeof first_bad, "first at offset %zu", i); }
            else if (!strcmp(klass, "REFUSED")) refused++;
            else if (!strcmp(klass, "DETECTED_ON_READ")) detected++;
            else unchanged++;
        }
        add_case(name, diverged ? "DIVERGED" : "SWEEP", diverged == 0,
                 "%zu-byte file, %u flips: refused %u, detected on read %u, unchanged (benign) %u, silent divergence %u %s",
                 n, tried, refused, detected, unchanged, diverged, first_bad);
    }
    for (int i = 0; i < ns; i++) free(snap[i].b);
    rmtree(d);
}

static void corrupt_all(void) {
    static const struct { Op op; long arg; } cx_ops[] = {
        { OP_TRUNC_BY, 1 }, { OP_TRUNC_BY, 40 }, { OP_TRUNC_BY, 400 }, { OP_TRUNC_HALF, 0 },
        { OP_TRUNC_TO, 0 }, { OP_TRUNC_TO, 24 }, { OP_FLIP_FIRST, 0 }, { OP_FLIP_MID, 0 },
        { OP_FLIP_LAST, 0 }, { OP_ZERO_TAIL, 0 }, { OP_APPEND_JUNK, 0 }, { OP_DELETE, 0 },
        { OP_FLIP_PM, 125 }, { OP_FLIP_PM, 250 }, { OP_FLIP_PM, 375 }, { OP_FLIP_PM, 625 },
        { OP_FLIP_PM, 750 }, { OP_FLIP_PM, 875 } };
    for (size_t i = 0; i < sizeof cx_ops / sizeof cx_ops[0]; i++)
        corrupt_case("cortex.cx", cx_ops[i].op, cx_ops[i].arg);
    static const struct { Op op; long arg; } mid_ops[] = { { OP_FLIP_FIRST, 0 }, { OP_DELETE, 0 } };
    for (size_t i = 0; i < 2; i++) corrupt_case("machine.id", mid_ops[i].op, mid_ops[i].arg);
    /* every J-Space file in a finished directory */
    char d[200], jd[260];
    mkcasedir(d, sizeof d, "corrupt_list");
    Sig tmp[NGOAL + 1];
    if (run_task(d, tmp) != 0) { add_case("corrupt/jspace/list", "DIVERGED", 0, "no directory"); return; }
    snprintf(jd, sizeof jd, "%s/jspace", d);
    char names[16][256];
    int nn = 0;
    DIR *dir = opendir(jd);
    struct dirent *e;
    while (dir && (e = readdir(dir)) && nn < 16)
        if (e->d_name[0] != '.') snprintf(names[nn++], sizeof names[0], "%.200s", e->d_name);
    if (dir) closedir(dir);
    rmtree(d);
    static const struct { Op op; long arg; } js_ops[] = {
        { OP_TRUNC_HALF, 0 }, { OP_TRUNC_BY, 1 }, { OP_TRUNC_TO, 0 }, { OP_FLIP_FIRST, 0 },
        { OP_FLIP_MID, 0 }, { OP_FLIP_LAST, 0 }, { OP_ZERO_TAIL, 0 }, { OP_DELETE, 0 },
        { OP_FLIP_ABS, 20 }, { OP_FLIP_ABS, 70 }, { OP_FLIP_ABS, 100 }, { OP_FLIP_ABS, 130 },
        { OP_FLIP_PM, 125 }, { OP_FLIP_PM, 250 }, { OP_FLIP_PM, 375 }, { OP_FLIP_PM, 625 },
        { OP_FLIP_PM, 750 }, { OP_FLIP_PM, 875 } };
    for (int f = 0; f < nn; f++)
        for (size_t i = 0; i < sizeof js_ops / sizeof js_ops[0]; i++) {
            char rel[300];
            snprintf(rel, sizeof rel, "jspace/%.200s", names[f]);
            corrupt_case(rel, js_ops[i].op, js_ops[i].arg);
        }
}

/* A directory made on one machine is refused by another identity, and the
 * refusal changes nothing (the owner still opens it and sees the final state). */
static void identity_case(void) {
    char d[200];
    mkcasedir(d, sizeof d, "identity");
    Sig tmp[NGOAL + 1];
    if (run_task(d, tmp) != 0) { add_case("identity/foreign_machine", "DIVERGED", 0, "no directory"); return; }
    AienMachineId mine = g_fx.self;
    g_fx.self = fx_mid(2);
    int rc = open_dir(d);
    g_fx.self = mine;
    if (rc == RX_OK) fx_close(&g_fx, &g_c);
    Sig s;
    int rc2 = open_dir(d);
    int same = 0;
    if (rc2 == RX_OK) { sig_take(&g_c, &s); fx_close(&g_fx, &g_c); same = s.ok && sig_eq(&s, &BASE[NGOAL]); }
    add_case("identity/foreign_machine", rc == RX_ERR_IDENTITY ? "REFUSED" : "DIVERGED",
             rc == RX_ERR_IDENTITY && same, "foreign open rc=%d (want %d), owner reopen same=%d", rc,
             RX_ERR_IDENTITY, same);
    rmtree(d);
}

/* Stale J-Space: put back the J-Space checkpoint from after goal `k` while the
 * Cortex journal (and machine.id) are from after the last goal. Cortex has
 * already promoted the later goals, which only happens after their state is
 * durable, so a checkpoint that lost them is not a crash window: it must be
 * refused, never silently rolled back. */
static void stale_case(int k) {
    char d[200], name[96];
    snprintf(name, sizeof name, "stale/jspace_checkpoint_from_after_goal%d", k);
    mkcasedir(d, sizeof d, "stale");
    Snap snap[16];
    int ns = 0;
    if (open_dir(d) != RX_OK) { add_case(name, "DIVERGED", 0, "open failed"); return; }
    for (int pass = 0; pass < 2; pass++) {
        for (int i = pass ? k : 0; i < (pass ? NGOAL : k); i++) {
            RxcResult o;
            fx_a_bad = 1;
            int rc = fx_run(&g_fx, &g_c, GOALS[i], &o);
            fx_a_bad = 0;
            if (rc != RX_OK || o.outcome != RXC_OUT_COMMITTED) { fx_close(&g_fx, &g_c); add_case(name, "DIVERGED", 0, "setup goal %d failed", i); rmtree(d); return; }
        }
        fx_close(&g_fx, &g_c);
        if (pass == 0) {
            ns = snap_dir(d, snap, 16);
            if (open_dir(d) != RX_OK) { add_case(name, "DIVERGED", 0, "reopen failed"); return; }
        }
    }
    int nj = 0;
    for (int i = 0; i < ns; i++)
        if (!strncmp(snap[i].rel, "jspace/", 7)) snap[nj++] = snap[i];
    restore_dir(d, snap, nj);
    int rc = open_dir(d);
    if (rc != RX_OK) {
        int rc2 = open_dir(d);
        if (rc2 == RX_OK) fx_close(&g_fx, &g_c);
        add_case(name, "REFUSED", rc2 == rc, "open rc=%d, again rc=%d", rc, rc2);
    } else {
        Sig s;
        sig_take(&g_c, &s);
        fx_close(&g_fx, &g_c);
        char dd[160];
        sig_diff(&s, &BASE[NGOAL], dd, sizeof dd);
        add_case(name, "DIVERGED", 0, "promoted goals silently rolled back (state matches baseline %d of %d); vs final: %.100s",
                 match_base(&s), NGOAL, dd);
    }
    rmtree(d);
}

/* ---- Cortex journal cut at a record boundary ------------------------------
 * cx_open accepts a journal cut exactly at a record boundary (no count or head
 * in its header). rx_compose cross-checks the J-Space checkpoint anchor
 * (count + head digest): a cut behind the anchored count must be refused at the
 * compose seam; a cut that only drops records written after the last commit
 * (count == anchor count) must still open. The test parses the journal and the
 * anchor (last 48 bytes of jspace.meta, version 3) itself. */
static int read_file_all(const char *path, uint8_t **out, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(sz > 0 ? (size_t)sz : 1);
    if (!b || (sz > 0 && fread(b, 1, (size_t)sz, f) != (size_t)sz)) { free(b); fclose(f); return -1; }
    fclose(f);
    *out = b; *n = (size_t)sz;
    return 0;
}
static uint64_t le64_at(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

/* Fill off[0..nrec] with record boundaries (off[0] = journal header end). */
static int journal_bounds(const uint8_t *j, size_t n, size_t *off, int max, int *nrec) {
    size_t pos = 4u * 8u;   /* CX_J_HDR_WORDS * 8 */
    int k = 0;
    if (n < pos) return -1;
    off[0] = pos;
    while (pos + 13u * 8u <= n && k + 1 < max) {
        uint64_t pw = le64_at(j + pos + 12u * 8u);   /* word index 12: payload words */
        size_t len = (size_t)(13u + pw + 4u) * 8u;
        if (pos + len > n) return -1;
        pos += len;
        off[++k] = pos;
    }
    if (pos != n) return -1;
    *nrec = k;
    return 0;
}

static int cx_alone_opens(const char *path) {
    CxStore cx;
    int rc = cx_open(&cx, path, RX_CORTEX_SUBJECTS, CX_OPEN_SYNC | CX_OPEN_REPAIR_TAIL);
    if (rc == CX_OK) cx_close(&cx);
    return rc;
}

static void boundary_cut_cases(void) {
    const char *nA = "cortex_cut/boundary_behind_checkpoint_anchor";
    const char *nB = "cortex_cut/boundary_after_last_commit_only";
    char d[200], jp[260], mp[260];
    Sig tmp[NGOAL + 1];
    mkcasedir(d, sizeof d, "cutb");
    if (run_task(d, tmp) != 0) {
        add_case(nA, "DIVERGED", 0, "no directory");
        add_case(nB, "DIVERGED", 0, "no directory");
        return;
    }
    snprintf(jp, sizeof jp, "%s/cortex.cx", d);
    snprintf(mp, sizeof mp, "%s/jspace/jspace.meta", d);
    uint8_t *j = NULL, *m = NULL;
    size_t jn = 0, mn = 0;
    static size_t off[4096];
    int nrec = 0;
    if (read_file_all(jp, &j, &jn) || read_file_all(mp, &m, &mn) || mn < 176u ||
        journal_bounds(j, jn, off, 4096, &nrec)) {
        add_case(nA, "DIVERGED", 0, "cannot parse journal or checkpoint");
        add_case(nB, "DIVERGED", 0, "cannot parse journal or checkpoint");
        free(j); free(m); rmtree(d);
        return;
    }
    uint64_t cnt = le64_at(m + mn - 48);   /* anchor: u64 count first */
    free(m);
    if (cnt < 1 || cnt > (uint64_t)nrec) {
        add_case(nA, "DIVERGED", 0, "anchor count %llu vs %d records", (unsigned long long)cnt, nrec);
        add_case(nB, "DIVERGED", 0, "anchor count %llu vs %d records", (unsigned long long)cnt, nrec);
        free(j); rmtree(d);
        return;
    }
    /* (a) cut to cnt-1 records: at least the newest checkpointed record is gone. */
    char da[200], db[200], cmd[640];
    mkcasedir(da, sizeof da, "cutb_a");
    snprintf(cmd, sizeof cmd, "cp -a '%s' '%s'", d, da);
    int cp = system(cmd);
    char ja[260];
    snprintf(ja, sizeof ja, "%s/cortex.cx", da);
    int tr = truncate(ja, (off_t)off[cnt - 1]);
    int rc = open_dir(da);
    if (rc == RX_OK) fx_close(&g_fx, &g_c);
    int rc2 = open_dir(da);
    if (rc2 == RX_OK) fx_close(&g_fx, &g_c);
    int cxrc = cx_alone_opens(ja);
    add_case(nA, rc == RX_ERR_REPLAY ? "REFUSED" : "DIVERGED",
             cp == 0 && tr == 0 && rc == RX_ERR_REPLAY && rc2 == RX_ERR_REPLAY && cxrc == CX_OK,
             "cut to %llu of %d records (anchor count %llu): compose open rc=%d again rc=%d (want %d), cx_open alone rc=%d (want %d)",
             (unsigned long long)(cnt - 1), nrec, (unsigned long long)cnt, rc, rc2, RX_ERR_REPLAY, cxrc, CX_OK);
    rmtree(da);
    /* (b) cut to exactly the anchored count: only post-commit records dropped. */
    if ((uint64_t)nrec == cnt) {
        add_case(nB, "NOT_APPLICABLE", 1, "no records written after the final commit (%d records, anchor count %llu); lower bound not exercised", nrec, (unsigned long long)cnt);
    } else {
        mkcasedir(db, sizeof db, "cutb_b");
        snprintf(cmd, sizeof cmd, "cp -a '%s' '%s'", d, db);
        cp = system(cmd);
        char jb[260];
        snprintf(jb, sizeof jb, "%s/cortex.cx", db);
        tr = truncate(jb, (off_t)off[cnt]);
        rc = open_dir(db);
        if (rc == RX_OK) fx_close(&g_fx, &g_c);
        add_case(nB, rc == RX_OK ? "RECOVERED" : "DIVERGED", cp == 0 && tr == 0 && rc == RX_OK,
                 "cut to anchor count %llu of %d records: open rc=%d (want 0)", (unsigned long long)cnt, nrec, rc);
        rmtree(db);
    }
    free(j);
    rmtree(d);
}

/* C4: a version-2 checkpoint (no anchor) cannot detect a lost journal through
 * the anchor, so recover() must still refuse durable J-Space history that has no
 * state record in the Cortex. The v2 file is made from the v3 one the way
 * rx_jspace_prod does: drop the 48-byte anchor section, patch the version and
 * recompute the body and header digests. A control reopen with the journal intact
 * proves the conversion is valid; the emptied-journal reopen must be refused. */
static void v2_lost_journal_case(void) {
    const char *nm = "cortex_cut/v2_checkpoint_journal_emptied";
    char d[200], mp[260], jp[260];
    Sig tmp[NGOAL + 1];
    mkcasedir(d, sizeof d, "v2lost");
    if (run_task(d, tmp) != 0) { add_case(nm, "DIVERGED", 0, "no directory"); return; }
    snprintf(mp, sizeof mp, "%s/jspace/jspace.meta", d);
    snprintf(jp, sizeof jp, "%s/cortex.cx", d);
    uint8_t *m = NULL;
    size_t mn = 0;
    if (read_file_all(mp, &m, &mn) || mn < 176u || m[8] != 3) {
        add_case(nm, "DIVERGED", 0, "cannot read a version-3 checkpoint");
        free(m); rmtree(d);
        return;
    }
    size_t nn = mn - 48;
    uint64_t bl = (uint64_t)nn - 128;
    for (int i = 0; i < 8; i++) m[24 + i] = (uint8_t)(bl >> (8 * i));
    m[8] = 2;
    sha256_hash(m + 128, bl, m + 64);
    sha256_hash(m, 96, m + 96);
    int fd = open(mp, O_WRONLY | O_TRUNC);
    int wr = fd >= 0 && write(fd, m, nn) == (ssize_t)nn;
    if (fd >= 0) close(fd);
    free(m);
    /* Control: v2 checkpoint, journal intact. */
    int rc0 = wr ? open_dir(d) : -1;
    if (rc0 == RX_OK) fx_close(&g_fx, &g_c);
    /* Journal emptied. */
    int tr = truncate(jp, 0);
    int rc = wr ? open_dir(d) : -1;
    if (rc == RX_OK) fx_close(&g_fx, &g_c);
    add_case(nm, rc == RX_ERR_REPLAY ? "REFUSED" : "DIVERGED",
             wr && tr == 0 && rc0 == RX_OK && rc == RX_ERR_REPLAY,
             "version-2 checkpoint: intact journal open rc=%d (want 0); journal emptied open rc=%d (want %d)",
             rc0, rc, RX_ERR_REPLAY);
    rmtree(d);
}

static void jstr(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <receipt.json>\n", argv[0]); return 2; }
    snprintf(g_base, sizeof g_base, "/tmp/rx_c4_requal.XXXXXX");
    if (!mkdtemp(g_base)) { perror("mkdtemp"); return 2; }
    if (fx_init(&g_fx) != 0) { fprintf(stderr, "fixture init failed\n"); return 2; }

    /* Baseline, twice: must be deterministic and fully committed. */
    char d0[200], d1[200];
    mkcasedir(d0, sizeof d0, "base0");
    mkcasedir(d1, sizeof d1, "base1");
    Sig B1[NGOAL + 1];
    int b0 = run_task(d0, BASE), b1 = run_task(d1, B1);
    int base_ok = b0 == 0 && b1 == 0;
    for (int i = 0; base_ok && i <= NGOAL; i++)
        base_ok = BASE[i].ok && sig_eq(&BASE[i], &B1[i]) && (i == 0 || !sig_eq(&BASE[i], &BASE[i - 1]));
    add_case("baseline/two_runs_identical_signatures", "BASELINE", base_ok, "rc=%d,%d", b0, b1);
    /* Baseline reopen after clean close names the final state. */
    Sig rb;
    int rr = open_dir(d0);
    int reopen_ok = 0;
    if (rr == RX_OK) { sig_take(&g_c, &rb); fx_close(&g_fx, &g_c); reopen_ok = rb.ok && sig_eq(&rb, &BASE[NGOAL]); memcpy(REOPEN_REC, rb.rec, 32); REOPEN_NREC = rb.nrec; }
    add_case("baseline/close_reopen_same_state", "BASELINE", reopen_ok, "rc=%d", rr);
    rmtree(d0);
    rmtree(d1);

    if (base_ok) {
        for (int g = 0; g < NGOAL; g++)
            for (int p = RXC_FP_BEFORE_FORK; p < RXC_FP_END; p++) crash_case(p, g);
        kill_sweep(24);
        corrupt_all();
        identity_case();
        for (int k = 1; k < NGOAL; k++) stale_case(k);
        boundary_cut_cases();
        v2_lost_journal_case();
        byte_sweep("jspace/jspace.meta", 1);
        byte_sweep("cortex.cx", 7);
        byte_sweep("machine.id", 1);
    }

    int npass = 0, nfail = 0, nna = 0;
    for (int i = 0; i < g_ncase; i++) nna += strcmp(g_case[i].klass, "NOT_APPLICABLE") == 0;
    for (int i = 0; i < g_ncase; i++) g_case[i].pass ? npass++ : nfail++;
    FILE *f = fopen(argv[1], "w");
    if (!f) { perror(argv[1]); return 2; }
    fprintf(f, "{\n  \"task\": \"verified-scale ledger: goals 5,6,7, result = 3*input+1, Skill A breaks the contract\",\n");
    fprintf(f, "  \"stages\": \"World -> J-Space -> AEGIS verify -> World commit/seal -> settle -> Cortex\",\n");
    fprintf(f, "  \"baseline_final\": {\"result\": %llu, \"promotions\": %u},\n",
            (unsigned long long)BASE[NGOAL].result, BASE[NGOAL].promos);
    fprintf(f, "  \"cases\": %d, \"pass\": %d, \"fail\": %d, \"not_applicable\": %d,\n  \"verdict\": \"%s\",\n  \"results\": [\n", g_ncase, npass,
            nfail, nna, nfail == 0 && base_ok ? "PASS" : "FAIL");
    for (int i = 0; i < g_ncase; i++) {
        fprintf(f, "    {\"name\": ");
        jstr(f, g_case[i].name);
        fprintf(f, ", \"class\": \"%s\", \"result\": \"%s\", \"detail\": ", g_case[i].klass,
                g_case[i].pass ? "PASS" : "FAIL");
        jstr(f, g_case[i].detail);
        fprintf(f, "}%s\n", i + 1 < g_ncase ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    for (int i = 0; i < g_ncase; i++)
        if (!g_case[i].pass || getenv("C4_VERBOSE"))
            printf("%s %-52s %-17s %s\n", g_case[i].pass ? "ok  " : "FAIL", g_case[i].name,
                   g_case[i].klass, g_case[i].detail);
    printf("C4 requalification (interruption+recovery): %d cases, %d pass, %d fail: %s\n", g_ncase, npass, nfail,
           nfail == 0 && base_ok ? "PASS" : "FAIL");
    fx_free(&g_fx);
    rmtree(g_base);
    return nfail == 0 && base_ok ? 0 : 1;
}
