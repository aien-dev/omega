/*
 * test_osh_journal.c -- durable intent/outcome journal of the osh Linux host adapter (OSH_PLATFORM_ABI.md section 10,
 * aien-architecture#158). Records are hand-built; everything happens in a disposable temp dir under $TMPDIR.
 * The binary is its own helper program: `test_osh_journal --helper append FILE` (create + append one byte) and
 * `--helper sleep MS`. Scenarios that kill, interrupt or crash the shell run it in a forked child.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "osh_host.h"
#include "osh_journal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL line %d: %s -- ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char g_self[1024], g_dir[512];
static int g_null;

static void msleep(int ms) { struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&ts, NULL); }

static int helper_main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[2], "append")) {
        int fd = open(argv[3], O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd < 0 || write(fd, "x", 1) != 1) return 1;
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[2], "sleep")) { msleep(atoi(argv[3])); return 0; }
    if (argc >= 3 && !strcmp(argv[2], "quiet")) return 0;
    return 2;
}

/* ---------------- plumbing ---------------- */

static int count_fds(void)
{
    int n = 0;
    DIR *d = opendir("/proc/self/fd");
    int dfd = dirfd(d);
    struct dirent *e;
    while ((e = readdir(d))) if (e->d_name[0] != '.' && atoi(e->d_name) != dfd) n++;
    closedir(d);
    return n;
}

static char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    char *b = calloc(1, 1 << 16);
    size_t len = f ? fread(b, 1, (1 << 16) - 1, f) : 0;
    if (f) fclose(f);
    if (n) *n = len;
    return b;
}

static int count_sub(const char *hay, const char *needle)
{
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)); p += strlen(needle)) n++;
    return n;
}

static long fsize(const char *path) { struct stat st; return stat(path, &st) == 0 ? (long)st.st_size : -1; }

typedef struct {
    OshSession s;
    OshJournal j;
    char jpath[4400];
    char dir[4200];
} T;

static int g_tn;

static void t_begin(T *t, int with_journal)
{
    memset(t, 0, sizeof *t);
    snprintf(t->dir, sizeof t->dir, "%s/t%d", g_dir, ++g_tn);
    mkdir(t->dir, 0700);
    snprintf(t->jpath, sizeof t->jpath, "%s/journal.log", t->dir);
    char *envp[] = {"PATH=/usr/bin:/bin", "HOME=/tmp", NULL};
    osh_session_init(&t->s, envp);
    for (int i = 0; i < 3; i++) t->s.fd[i] = g_null;
    t->s.binding.valid = 1;
    t->s.binding.domain = 2;
    t->s.binding.cap_generation = 1;
    if (with_journal) {
        int rc = osh_journal_open(&t->j, t->jpath);
        CHECK(rc == 0, "journal open: %d", rc);
        t->s.journal = &t->j;
    }
}

static void t_end(T *t)
{
    if (t->s.journal) osh_journal_close(&t->j);
    osh_session_free(&t->s);
}

static void mpath(const T *t, const char *name, char *out, size_t cap) { snprintf(out, cap, "%s/%s", t->dir, name); }

static OshBuilder *nb(void)
{
    OshBuilder *b = malloc(sizeof *b);
    osh_rb_init(b, 0, 0);
    return b;
}

static void helper(OshBuilder *b, const char *op, const char *arg)
{
    osh_rb_cmd(b, 0);
    osh_rb_arg(b, g_self);
    osh_rb_arg(b, "--helper");
    osh_rb_arg(b, op);
    if (arg) osh_rb_arg(b, arg);
}

static int run(T *t, OshBuilder *b, OshResult *r)
{
    osh_rb_seal(b);
    return osh_exec_record(&t->s, b->rec, osh_rb_cells(b), b->out, b->out_used, r);
}

static char *jtext(const T *t) { return slurp(t->jpath, NULL); }

/* the Nth line (0-based) of a text blob, copied */
static int line_has(const char *text, int n, const char *needle)
{
    const char *p = text;
    for (int i = 0; i < n && p; i++) { p = strchr(p, '\n'); if (p) p++; }
    if (!p) return 0;
    const char *e = strchr(p, '\n');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    char *ln = strndup(p, len);
    int ok = strstr(ln, needle) != NULL;
    free(ln);
    return ok;
}

/* ONE ACCOUNT: for every command of the request the LAST outcome record in the journal names the same outcome and status as
 * the in-memory result. Returns the number of commands that disagree (or have no outcome record). */
static int same_account(const T *t, int ncmds, const OshResult *r)
{
    static const char *const nm[] = {"NOT_STARTED", "COMPLETED", "FAILED_NO_EFFECT", "CANCELLED", "OUTCOME_UNKNOWN"};
    char *j = jtext(t);
    int bad = 0;
    for (int i = 0; i < ncmds; i++) {
        char key[32], want[96], *hit = NULL;
        snprintf(key, sizeof key, " %d/%d ", i, ncmds);
        snprintf(want, sizeof want, "%s st=%d err=%d", nm[r->cmd[i].outcome], r->cmd[i].status, r->cmd[i].err);
        for (char *p = j; p && *p;) {
            char *e = strchr(p, '\n');
            if (strstr(p, " O ") && strstr(p, key) && (!e || strstr(p, key) < e)) hit = p;
            p = e ? e + 1 : NULL;
        }
        char *e = hit ? strchr(hit, '\n') : NULL;
        if (!hit || !(e ? (size_t)(e - hit) : strlen(hit)) || !memmem(hit, e ? (size_t)(e - hit) : strlen(hit), want, strlen(want))) {
            bad++;
            printf("  account mismatch cmd %d: want '%s' in: %.*s\n", i, want, hit ? (int)(e ? e - hit : (long)strlen(hit)) : 4, hit ? hit : "none");
        }
    }
    free(j);
    return bad;
}

static int wait_file(const char *path, long size, int ms)
{
    for (int i = 0; i < ms / 20; i++) { if (fsize(path) >= size) return 1; msleep(20); }
    return fsize(path) >= size;
}

/* run fn in a forked child (own process group when grp); returns the child's wait status */
static int in_child(void (*fn)(T *, void *), T *t, void *arg, int grp)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        if (grp) setpgid(0, 0);
        fn(t, arg);
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (grp) kill(-pid, SIGKILL);
    return st;
}

/* ---------------- tests ---------------- */

static void test_secret_names(void)
{
    CHECK(osh_secret_name("DB_PASSWORD") && osh_secret_name("api_token") && osh_secret_name("AWS_SECRET_ACCESS_KEY") &&
          osh_secret_name("Authorization"), "secret-class names are caught");
    CHECK(!osh_secret_name("PATH") && !osh_secret_name("HOME") && !osh_secret_name("LANG") && !osh_secret_name("PLAINVAR"),
          "ordinary names are not (negative control)");
}

static void test_normal_and_redaction(void)
{
    T t;
    t_begin(&t, 1);
    char marker[4400];
    mpath(&t, "m1", marker, sizeof marker);
    int fds0 = count_fds();
    OshBuilder *b = nb();
    helper(b, "append", marker);
    osh_rb_arg(b, "--password=hunter2XYZ");
    osh_rb_assign(b, "API_TOKEN", "hunter2XYZ");
    osh_rb_assign(b, "PLAINVAR", "visibleval");
    char outf[4400];
    mpath(&t, "out-hunter2XYZ.txt", outf, sizeof outf);
    osh_rb_redir(b, OSH_R_OUT, 1, outf, 0);
    OshResult r;
    int st = run(&t, b, &r);
    free(b);
    CHECK(st == 0 && r.cmd[0].outcome == OSH_OUT_COMPLETED && fsize(marker) == 1, "ran once (st %d)", st);
    CHECK(same_account(&t, 1, &r) == 0, "normal: record == result");
    char *j = jtext(&t);
    CHECK(count_sub(j, " I ") == 1 && count_sub(j, " O ") == 1, "one intent and one outcome:\n%s", j);
    CHECK(line_has(j, 0, " I ") && line_has(j, 1, " O 1 ") && line_has(j, 1, "COMPLETED st=0 err=0"), "order and ref:\n%s", j);
    CHECK(line_has(j, 0, "argc=") && line_has(j, 0, "wr=1"), "write-class open counted:\n%s", j);
    CHECK(!strstr(j, "hunter2XYZ"), "secret value absent from record bytes");
    CHECK(!strstr(j, "visibleval") && !strstr(j, "PLAINVAR") && !strstr(j, "API_TOKEN") && !strstr(j, "password"),
          "no assignment, argument or target text at all");
    CHECK(strstr(j, "a0=") != NULL, "argv[0] is recorded");
    free(j);
    struct stat sb;
    CHECK(stat(t.jpath, &sb) == 0 && (sb.st_mode & 0777) == 0600, "journal mode 0600");
    CHECK(count_fds() == fds0, "no descriptor leaked by a run (%d vs %d)", count_fds(), fds0);
    t_end(&t);

    /* digest: a secret-named assignment's VALUE does not change it; an ordinary value and an argument do (control) */
    char kpa[700];
    snprintf(kpa, sizeof kpa, "%s/digest-a.log", g_dir);
    OshJournal ja;
    CHECK(osh_journal_open(&ja, kpa) == 0, "open journal A: %s", ja.why);
    static OshRequest ra, rb, rc, rd;
    OshBuilder *x[4];
    OshRequest *rq[4] = {&ra, &rb, &rc, &rd};
    const char *tok[4] = {"aaa", "bbb", "aaa", "aaa"}, *pv[4] = {"v", "v", "w", "v"}, *arg[4] = {"p", "p", "p", "q"};
    uint8_t dg[4][32];
    for (int i = 0; i < 4; i++) {
        x[i] = nb();
        osh_rb_cmd(x[i], 0);
        osh_rb_arg(x[i], "prog");
        osh_rb_arg(x[i], arg[i]);
        osh_rb_assign(x[i], "MY_TOKEN", tok[i]);
        osh_rb_assign(x[i], "PLAINVAR", pv[i]);
        osh_rb_seal(x[i]);
        CHECK(osh_req_decode(x[i]->rec, osh_rb_cells(x[i]), x[i]->out, x[i]->out_used, rq[i]) == 0, "decode");
        osh_req_digest(&ja, rq[i], dg[i]);
        free(x[i]);
    }
    CHECK(!memcmp(dg[0], dg[1], 32), "secret value masked in the digest");
    CHECK(memcmp(dg[0], dg[2], 32) && memcmp(dg[0], dg[3], 32), "ordinary value and argument change the digest");
    osh_journal_close(&ja);
}

static void hexs(const uint8_t d[32], char o[65])
{
    for (int i = 0; i < 32; i++) snprintf(o + 2 * i, 3, "%02x", d[i]);
}

/* The digest is keyed: two journals give two digests for one request, and a low-entropy secret passed as a plain argument
 * cannot be confirmed by hashing guesses (the plain SHA-256 of every PIN does not match the recorded digest). */
static void test_keyed_digest(void)
{
    char pa[700], pb[700];
    snprintf(pa, sizeof pa, "%s/keyed-a.log", g_dir);
    snprintf(pb, sizeof pb, "%s/keyed-b.log", g_dir);
    OshJournal a, b2, a2;
    CHECK(osh_journal_open(&a, pa) == 0 && osh_journal_open(&b2, pb) == 0, "two journals, two keys");
    CHECK(memcmp(a.key, b2.key, 32) != 0, "keys differ between journals");
    OshBuilder *x = nb();
    osh_rb_cmd(x, 0);
    osh_rb_arg(x, "login");
    osh_rb_arg(x, "--pin");
    osh_rb_arg(x, "1234");
    osh_rb_seal(x);
    static OshRequest rq;
    CHECK(osh_req_decode(x->rec, osh_rb_cells(x), x->out, x->out_used, &rq) == 0, "decode");
    free(x);
    uint8_t da[32], db[32], da2[32], pl[32];
    osh_req_digest(&a, &rq, da);
    osh_req_digest(&b2, &rq, db);
    CHECK(memcmp(da, db, 32) != 0, "same request under two journal keys: different digests");
    osh_journal_close(&a);
    CHECK(osh_journal_open(&a2, pa) == 0, "reopen: %s", a2.why);
    osh_req_digest(&a2, &rq, da2);
    CHECK(!memcmp(da, da2, 32), "the key is reused on reopen: same digest");
    struct stat sb;
    char kp[720];
    snprintf(kp, sizeof kp, "%s.key", pa);
    CHECK(stat(kp, &sb) == 0 && sb.st_size == 32 && (sb.st_mode & 0777) == 0600, "key file: 32 bytes, mode 0600");
    /* an attacker who knows the encoding and the argument shape but not the key */
    osh_req_digest_unkeyed(&rq, pl);
    CHECK(memcmp(da, pl, 32) != 0, "recorded digest is not the plain SHA-256 of the request");
    char want[65], plain[65], got[65];
    hexs(da, want);
    hexs(pl, plain);
    int hit = 0, ctl = 0;
    for (int pin = 0; pin < 10000; pin++) {
        char s[16];
        snprintf(s, sizeof s, "%04d", pin);
        OshBuilder *y = nb();
        osh_rb_cmd(y, 0);
        osh_rb_arg(y, "login");
        osh_rb_arg(y, "--pin");
        osh_rb_arg(y, s);
        osh_rb_seal(y);
        static OshRequest rg;
        osh_req_decode(y->rec, osh_rb_cells(y), y->out, y->out_used, &rg);
        free(y);
        uint8_t g[32];
        osh_req_digest_unkeyed(&rg, g);
        hexs(g, got);
        hit += !strcmp(got, want);
        ctl += !strcmp(got, plain);
    }
    CHECK(hit == 0, "none of the 10000 PIN guesses hashed with plain SHA-256 matches the recorded digest (control: see next check)");
    CHECK(ctl == 1, "control: the same loop does find the PIN against the PLAIN digest of the request (%d)", ctl);
    osh_journal_close(&a2);
    osh_journal_close(&b2);
}

static int deny_all(void *c, const OshBinding *bd, int op, const char *p) { (void)c; (void)bd; (void)op; (void)p; return OSH_E_DENIED; }

static void test_denial(void)
{
    T t;
    t_begin(&t, 1);
    char marker[4400];
    mpath(&t, "m3", marker, sizeof marker);
    t.s.binding.valid = 0; /* no authority */
    OshBuilder *b = nb();
    helper(b, "append", marker);
    OshResult r;
    int st = run(&t, b, &r);
    free(b);
    msleep(150);
    CHECK(st == 126 && r.err == OSH_E_DENIED && r.cmd[0].pid == 0, "denied (st %d err %d)", st, r.err);
    CHECK(fsize(marker) < 0, "negative control: no child ran, marker absent");
    char *j = jtext(&t);
    CHECK(count_sub(j, " I ") == 0 && count_sub(j, " O 0 ") == 1 && strstr(j, "FAILED_NO_EFFECT st=126 err=1"), "denial record, no intent:\n%s", j);
    free(j);

    /* control: the same request with authority runs */
    t.s.binding.valid = 1;
    b = nb();
    helper(b, "append", marker);
    st = run(&t, b, &r);
    free(b);
    CHECK(st == 0 && fsize(marker) == 1, "with authority the marker appears");
    t_end(&t);

    /* per-effect denial (ABI 9.2) at the moment of the spawn */
    T u;
    t_begin(&u, 1);
    mpath(&u, "m3b", marker, sizeof marker);
    u.s.effect_hook = deny_all;
    b = nb();
    helper(b, "append", marker);
    st = run(&u, b, &r);
    free(b);
    msleep(150);
    char *j2 = jtext(&u);
    CHECK(st == 126 && fsize(marker) < 0 && count_sub(j2, " I ") == 0 && strstr(j2, "FAILED_NO_EFFECT"), "effect-hook denial:\n%s", j2);
    free(j2);
    t_end(&u);
}

static void raise_int_group(void *ctx) { (void)ctx; kill(0, SIGINT); }

static void child_sigint(T *t, void *arg)
{
    (void)arg;
    OshBuilder *b = nb();
    helper(b, "sleep", "20000");
    helper(b, "sleep", "20000");
    t->s.after_launch_hook = raise_int_group;
    OshResult r;
    run(t, b, &r);
    _exit(same_account(t, 2, &r) == 0 && r.cmd[0].outcome == OSH_OUT_COMPLETED && r.cmd[1].outcome == OSH_OUT_COMPLETED ? 0 : 3);
}

static void test_interrupt(void)
{
    T t;
    t_begin(&t, 1);
    int st = in_child(child_sigint, &t, NULL, 1);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "SIGINT: record == result for both commands, both COMPLETED (child status %d)", st);
    char *j = jtext(&t);
    CHECK(count_sub(j, " I ") == 2 && count_sub(j, " O ") == 2 && count_sub(j, "COMPLETED st=130") == 2, "both ended by the signal: COMPLETED st=130 (ABI 8.4 convention):\n%s", j);
    CHECK(line_has(j, 2, " O 1 ") && line_has(j, 3, " O 2 "), "outcomes close their intents:\n%s", j);
    free(j);
    osh_journal_close(&t.j);
    OshJournal j2;
    CHECK(osh_journal_open(&j2, t.jpath) == 0, "reopen");
    int bad = 0, n = osh_journal_recover(&j2, t.jpath, NULL, 0, &bad);
    CHECK(n == 0 && bad == 0, "nothing unresolved after an interrupt (%d)", n);
    osh_journal_close(&j2);
    t.s.journal = NULL;
    t_end(&t);
}

static void test_partial_launch(void)
{
    T t;
    t_begin(&t, 1);
    OshBuilder *b = nb();
    helper(b, "sleep", "30000");
    helper(b, "sleep", "30000");
    helper(b, "sleep", "30000");
    t.s.fail_fork_at = 2;
    OshResult r;
    int st = run(&t, b, &r);
    free(b);
    CHECK(st == 1 && r.err == OSH_E_PARTIAL_LAUNCH, "partial launch (st %d err %d)", st, r.err);
    CHECK(same_account(&t, 3, &r) == 0, "partial launch: record == result");
    char *j = jtext(&t);
    CHECK(count_sub(j, " I ") == 2, "first and second have intents, third never reached:\n%s", j);
    CHECK(count_sub(j, " O ") == 3, "three outcomes");
    CHECK(strstr(j, " 0/3 CANCELLED") && strstr(j, " 1/3 NOT_STARTED") && strstr(j, " 2/3 NOT_STARTED"), "first CANCELLED, rest NOT_STARTED:\n%s", j);
    CHECK(strstr(j, " O 0 ") != NULL, "the never-reached command has an outcome with no intent");
    free(j);
    t_end(&t);
}

static void steal_child(void *ctx) { (void)ctx; msleep(300); waitpid(-1, NULL, 0); }

static void test_echild_unknown(void)
{
    T t;
    t_begin(&t, 1);
    OshBuilder *b = nb();
    helper(b, "quiet", NULL);
    t.s.after_launch_hook = steal_child; /* somebody else reaps our child: its fate is unknowable */
    OshResult r;
    run(&t, b, &r);
    free(b);
    CHECK(r.cmd[0].outcome == OSH_OUT_UNKNOWN && r.err == OSH_E_OUTCOME_UNKNOWN, "result says unknown");
    CHECK(same_account(&t, 1, &r) == 0, "ECHILD: record == result");
    char *j = jtext(&t);
    CHECK(strstr(j, " O 1 ") && strstr(j, "OUTCOME_UNKNOWN st=1 err=11"), "durably mirrored:\n%s", j);
    free(j);
    t_end(&t);
}

static void child_kill_after_launch_hook(void *ctx) { (void)ctx; kill(getpid(), SIGKILL); }
static void child_kill_between(T *t, void *arg)
{
    OshBuilder *b = nb();
    helper(b, "append", (const char *)arg);
    t->s.after_launch_hook = child_kill_after_launch_hook; /* the shell dies after the spawn, before any outcome */
    OshResult r;
    run(t, b, &r);
}

static void test_kill_between_and_recovery(void)
{
    T t;
    t_begin(&t, 1);
    char marker[4400];
    mpath(&t, "m7", marker, sizeof marker);
    int st = in_child(child_kill_between, &t, marker, 0);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "shell died by SIGKILL between intent and outcome");
    CHECK(wait_file(marker, 1, 3000), "the already launched child still ran once");
    char *j = jtext(&t);
    CHECK(count_sub(j, " I ") == 1 && count_sub(j, " O ") == 0, "intent only:\n%s", j);
    free(j);
    osh_journal_close(&t.j);

    OshJournal j2;
    CHECK(osh_journal_open(&j2, t.jpath) == 0, "reopen after the crash");
    OshJournalUnknown u[4];
    int bad = -1, n = osh_journal_recover(&j2, t.jpath, u, 4, &bad);
    CHECK(n == 1 && bad == 0 && u[0].idx == 0 && u[0].ncmds == 1 && strlen(u[0].digest) == 64, "one UNKNOWN reported (n %d)", n);
    CHECK(strstr(u[0].argv0, "test_osh_journal") != NULL, "with argv0 (%s)", u[0].argv0);
    {
        OshBuilder *kb = nb();
        helper(kb, "append", marker);
        osh_rb_seal(kb);
        static OshRequest kr;
        uint8_t kd[32];
        char kh[65];
        CHECK(osh_req_decode(kb->rec, osh_rb_cells(kb), kb->out, kb->out_used, &kr) == 0, "decode");
        free(kb);
        osh_req_digest(&j2, &kr, kd);
        hexs(kd, kh);
        CHECK(!strcmp(kh, u[0].digest), "recovery reports the KEYED digest of the request (recomputed with the journal key)");
        osh_req_digest_unkeyed(&kr, kd);
        hexs(kd, kh);
        CHECK(strcmp(kh, u[0].digest) != 0, "and it is not the plain SHA-256");
    }
    msleep(700);
    CHECK(fsize(marker) == 1, "recovery did not replay: marker still 1 byte (%ld)", fsize(marker));
    j = jtext(&t);
    CHECK(strstr(j, "OUTCOME_UNKNOWN") && strstr(j, "recovered=1"), "recovery closes it durably as UNKNOWN:\n%s", j);
    free(j);
    n = osh_journal_recover(&j2, t.jpath, u, 4, &bad);
    CHECK(n == 0, "second recovery reports nothing new (%d)", n);
    osh_journal_close(&j2);
    t.s.journal = NULL;
    t_end(&t);
}

static void child_die_after_intent(T *t, void *arg)
{
    t->j.die_after_intent = 1; /* crash right after the intent is durable, before fork */
    OshBuilder *b = nb();
    helper(b, "append", (const char *)arg);
    OshResult r;
    run(t, b, &r);
}

static void test_crash_before_launch(void)
{
    T t;
    t_begin(&t, 1);
    char marker[4400];
    mpath(&t, "m8", marker, sizeof marker);
    int st = in_child(child_die_after_intent, &t, marker, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 77, "crashed after the intent (status %d)", st);
    msleep(300);
    CHECK(fsize(marker) < 0, "nothing was launched");
    osh_journal_close(&t.j);
    OshJournal j2;
    CHECK(osh_journal_open(&j2, t.jpath) == 0, "reopen");
    OshJournalUnknown u[2];
    int bad = 0, n = osh_journal_recover(&j2, t.jpath, u, 2, &bad);
    CHECK(n == 1, "intent without outcome is UNKNOWN (%d)", n);
    msleep(300);
    CHECK(fsize(marker) < 0, "recovery launched nothing");
    osh_journal_close(&j2);
    t.s.journal = NULL;
    t_end(&t);
}

static void test_fail_closed(void)
{
    T t;
    t_begin(&t, 1);
    char marker[4400];
    mpath(&t, "m9", marker, sizeof marker);
    int full = open("/dev/full", O_WRONLY);
    dup2(full, t.j.fd); /* the journal's descriptor now fails every write with ENOSPC */
    close(full);
    OshBuilder *b = nb();
    helper(b, "append", marker);
    OshResult r;
    int st = run(&t, b, &r);
    free(b);
    msleep(150);
    CHECK(st == 1 && r.err == OSH_E_IO && r.cmd[0].outcome == OSH_OUT_FAILED_NO_EFFECT && fsize(marker) < 0,
          "no durable intent -> command not launched (st %d err %d)", st, r.err);
    t_end(&t);
}

static void test_non_effects_and_direct(void)
{
    T t;
    t_begin(&t, 1);
    long before = fsize(t.jpath);
    OshBuilder *b = nb();
    osh_rb_cmd(b, OSH_B_PWD);
    osh_rb_arg(b, "pwd"); /* builtin, no write-class open: not an effect */
    OshResult r;
    run(&t, b, &r);
    free(b);
    CHECK(fsize(t.jpath) == before, "a builtin with no write open writes no record");
    char outf[4400];
    mpath(&t, "o10", outf, sizeof outf);
    b = nb();
    osh_rb_cmd(b, OSH_B_PWD);
    osh_rb_arg(b, "pwd");
    osh_rb_redir(b, OSH_R_OUT, 1, outf, 0); /* write-class open IS an effect, even for a builtin */
    run(&t, b, &r);
    free(b);
    char *j = jtext(&t);
    CHECK(count_sub(j, " I ") == 1 && count_sub(j, "COMPLETED") == 1 && line_has(j, 0, "b=2"), "builtin with output redirection recorded:\n%s", j);
    free(j);

    char marker[4400];
    mpath(&t, "m10", marker, sizeof marker);
    const char *argv[] = {g_self, "--helper", "append", marker};
    int st = osh_submit_argv(&t.s, argv, 4, NULL, 0, NULL, 0, &r); /* ABI section 11: same path, same records */
    j = jtext(&t);
    CHECK(st == 0 && count_sub(j, " I ") == 2 && count_sub(j, " O ") == 2, "direct-argv submission is journaled:\n%s", j);
    free(j);
    t_end(&t);

    /* control: a session without a journal behaves as before and writes nothing */
    T u;
    t_begin(&u, 0);
    b = nb();
    helper(b, "append", marker);
    st = run(&u, b, &r);
    free(b);
    CHECK(st == 0 && fsize(u.jpath) < 0, "no journal -> no file");
    t_end(&u);
}

static void test_torn_tail_and_cleanup(void)
{
    T t;
    t_begin(&t, 1);
    OshBuilder *b = nb();
    helper(b, "quiet", NULL);
    OshResult r;
    run(&t, b, &r);
    free(b);
    osh_journal_close(&t.j);
    t.s.journal = NULL;
    FILE *f = fopen(t.jpath, "ab");
    fputs("OSHJ1 99 I 00ff", f); /* a write cut short by a crash: no newline */
    fclose(f);
    int fds0 = count_fds();
    OshJournal j2;
    CHECK(osh_journal_open(&j2, t.jpath) == 0, "reopen over a torn tail");
    CHECK(j2.next_rec == 3, "record counter continues (%llu)", (unsigned long long)j2.next_rec);
    int bad = 0, n = osh_journal_recover(&j2, t.jpath, NULL, 0, &bad);
    CHECK(n == 0 && bad == 1, "torn line is ignored, not an intent (n %d bad %d)", n, bad);
    osh_journal_close(&j2);
    CHECK(count_fds() == fds0, "open/recover/close leaks no descriptor");
    int files = 0;
    DIR *d = opendir(t.dir);
    struct dirent *e;
    while ((e = readdir(d))) if (e->d_name[0] != '.' && strcmp(e->d_name, "journal.log") && strcmp(e->d_name, "journal.log.key")) files++;
    closedir(d);
    CHECK(files == 0, "only the journal and its key exist, no temp files (%d stray)", files);
    t_end(&t);
}

/* the real program. jpath NULL = no --journal (default location from xdg/home); xdg/home NULL = variable unset */
static int run_osh(const char *osh, const char *caps, const char *jpath, const char *script, const char *errf, const char *xdg,
                   const char *home)
{
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open(errf, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        dup2(fd, 2);
        dup2(g_null, 1);
        if (xdg) setenv("XDG_STATE_HOME", xdg, 1); else unsetenv("XDG_STATE_HOME");
        if (home) setenv("HOME", home, 1); else unsetenv("HOME");
        const char *av[12];
        int n = 0;
        av[n++] = osh;
        if (caps) { av[n++] = "--caps"; av[n++] = caps; }
        if (jpath) { av[n++] = "--journal"; av[n++] = jpath; }
        av[n++] = "-c";
        av[n++] = script;
        av[n] = NULL;
        execv(osh, (char *const *)av);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void test_cli(const char *osh)
{
    char jp[700], ef[700], pol[700], xdg[700], home[700], jd[1400], mk[700], sc[1500];
    snprintf(jp, sizeof jp, "%s/cli.log", g_dir);
    snprintf(ef, sizeof ef, "%s/cli.err", g_dir);
    snprintf(pol, sizeof pol, "%s/cli.policy", g_dir);
    snprintf(xdg, sizeof xdg, "%s/xdg", g_dir);
    snprintf(home, sizeof home, "%s/home", g_dir);
    snprintf(mk, sizeof mk, "%s/cli.marker", g_dir);
    FILE *pf = fopen(pol, "w");
    fputs("principal 77\nallow spawn /bin\nallow spawn /usr/bin\nallow spawn /home\nallow write /home\n", pf);
    fclose(pf);
    /* default policy (no grants): the spawns are denied; denials are recorded, no intents, nothing ran */
    CHECK(run_osh(osh, NULL, jp, "/bin/true", ef, xdg, home) == 126, "denied by the default policy");
    char *d0 = slurp(jp, NULL);
    CHECK(count_sub(d0, " I ") == 0 && count_sub(d0, "FAILED_NO_EFFECT st=126 err=1") == 1, "program records the denial:\n%s", d0);
    free(d0);
    unlink(jp);
    CHECK(run_osh(osh, pol, jp, "/bin/true; /bin/false", ef, xdg, home) == 1, "osh --journal runs the script");
    char *j = slurp(jp, NULL);
    CHECK(count_sub(j, " I ") == 2 && count_sub(j, " O ") == 2 && strstr(j, "COMPLETED st=0") && strstr(j, "COMPLETED st=1"), "program writes intents and outcomes:\n%s", j);
    free(j);
    FILE *f = fopen(jp, "ab"); /* a previous run died after its intent */
    fprintf(f, "OSHJ1 50 I %064d 0/1 b=0 argc=2 wr=0 a0=/bin/touch lens=10,3\n", 7);
    fclose(f);
    CHECK(run_osh(osh, pol, jp, "/bin/echo second-run", ef, xdg, home) == 0, "second start");
    char *e = slurp(ef, NULL);
    CHECK(strstr(e, "has outcome UNKNOWN; it is not re-run") && strstr(e, "/bin/touch"), "UNKNOWN reported at start:\n%s", e);
    free(e);
    CHECK(run_osh(osh, pol, jp, "/bin/true", ef, xdg, home) == 0, "third start");
    e = slurp(ef, NULL);
    CHECK(!strstr(e, "UNKNOWN"), "reported once only");
    free(e);

    /* MANDATORY: no --journal -> the default location is used ($XDG_STATE_HOME/osh/effects.journal), 0700 dirs, 0600 file */
    snprintf(jd, sizeof jd, "%s/osh/effects.journal", xdg);
    CHECK(run_osh(osh, pol, NULL, "/bin/true", ef, xdg, home) == 0, "no --journal still runs");
    struct stat sb;
    char xo[1000];
    snprintf(xo, sizeof xo, "%s/osh", xdg);
    CHECK(stat(jd, &sb) == 0 && (sb.st_mode & 0777) == 0600, "default journal exists with mode 0600");
    CHECK(stat(xo, &sb) == 0 && (sb.st_mode & 0777) == 0700, "its directory is 0700");
    j = slurp(jd, NULL);
    CHECK(count_sub(j, " I ") == 1 && count_sub(j, " O ") == 1, "the effect was recorded there:\n%s", j);
    free(j);
    /* fallback: no XDG_STATE_HOME -> $HOME/.local/state/osh/effects.journal */
    snprintf(jd, sizeof jd, "%s/.local/state/osh/effects.journal", home);
    mkdir(home, 0700);
    CHECK(run_osh(osh, pol, NULL, "/bin/true", ef, NULL, home) == 0 && stat(jd, &sb) == 0 && (sb.st_mode & 0777) == 0600,
          "HOME fallback location used");
    char dd[1200];
    snprintf(dd, sizeof dd, "%s/.local/state/osh", home);
    CHECK(stat(dd, &sb) == 0 && (sb.st_mode & 0777) == 0700, "fallback directory is 0700");

    /* unwritable journal: effects refused by name, nothing launched, non-effect builtins still run */
    snprintf(jd, sizeof jd, "%s/not-a-dir", g_dir);
    FILE *nf = fopen(jd, "w"); /* a regular file where the state directory must be */
    fclose(nf);
    unlink(mk);
    snprintf(sc, sizeof sc, "/bin/touch %s", mk);
    CHECK(run_osh(osh, pol, NULL, sc, ef, jd, home) == 1, "effect refused when the journal cannot be opened");
    e = slurp(ef, NULL);
    CHECK(strstr(e, "effect journal unavailable") && strstr(e, "effect refused, nothing was run"), "named error:\n%s", e);
    free(e);
    msleep(150);
    CHECK(fsize(mk) < 0, "no child was launched (marker absent)");
    CHECK(run_osh(osh, pol, NULL, "printf x", ef, jd, home) == 0, "a builtin without a write redirection is unaffected");
    snprintf(sc, sizeof sc, "printf x > %s", mk);
    CHECK(run_osh(osh, pol, NULL, sc, ef, jd, home) == 1 && fsize(mk) < 0, "a write-class redirection is an effect: refused, no file");
    /* an explicit --journal that cannot be opened is the same refusal (no silent fallback) */
    snprintf(sc, sizeof sc, "/bin/touch %s", mk);
    CHECK(run_osh(osh, pol, "/nonexistent-dir/j.log", sc, ef, xdg, home) == 1 && fsize(mk) < 0, "explicit unopenable --journal: effect refused");
    /* the key file sits beside the default journal: 32 bytes, 0600 */
    snprintf(jd, sizeof jd, "%s/.local/state/osh/effects.journal.key", home);
    CHECK(stat(jd, &sb) == 0 && sb.st_size == 32 && (sb.st_mode & 0777) == 0600, "default key file: 32 bytes, 0600");
    /* wrong-mode key, wrong-mode journal, wrong-mode state dir: refused by name, nothing launched; fixing them restores service */
    char kd[3000], jf[2900], sd[1500];
    snprintf(sd, sizeof sd, "%s/.local/state/osh", home);
    snprintf(jf, sizeof jf, "%s/effects.journal", sd);
    snprintf(kd, sizeof kd, "%s.key", jf);
    snprintf(sc, sizeof sc, "/bin/touch %s", mk);
    unlink(mk);
    chmod(kd, 0644);
    CHECK(run_osh(osh, pol, NULL, sc, ef, NULL, home) == 1 && fsize(mk) < 0, "key with mode 0644: effect refused, nothing launched");
    e = slurp(ef, NULL);
    CHECK(strstr(e, "effect journal unavailable") && strstr(e, "key file"), "named: %s", e);
    free(e);
    chmod(kd, 0600);
    chmod(jf, 0644);
    CHECK(run_osh(osh, pol, NULL, sc, ef, NULL, home) == 1 && fsize(mk) < 0, "journal with mode 0644: effect refused");
    chmod(jf, 0600);
    chmod(sd, 0755);
    CHECK(run_osh(osh, pol, NULL, sc, ef, NULL, home) == 1 && fsize(mk) < 0, "state directory with mode 0755: effect refused");
    e = slurp(ef, NULL);
    CHECK(strstr(e, "state directory") && strstr(e, "0700"), "named: %s", e);
    free(e);
    chmod(sd, 0700);
    unlink(kd);
    CHECK(run_osh(osh, pol, NULL, sc, ef, NULL, home) == 1 && fsize(mk) < 0, "key deleted beside a journal with records: effect refused");
    snprintf(sc, sizeof sc, "/bin/touch %s", mk);
    /* control: the same command with a working journal does run */
    CHECK(run_osh(osh, pol, jp, sc, ef, xdg, home) == 0 && fsize(mk) == 0, "control: with a working journal the marker is created");
}

static void test_key_and_modes(void)
{
    char p[1400], k[1500], dir[700];
    snprintf(dir, sizeof dir, "%s/km", g_dir);
    mkdir(dir, 0700);
    snprintf(p, sizeof p, "%s/j.log", dir);
    snprintf(k, sizeof k, "%s.key", p);
    OshJournal j;
    CHECK(osh_journal_open(&j, p) == 0, "fresh journal and key: %s", j.why);
    osh_journal_close(&j);
    chmod(k, 0644);
    CHECK(osh_journal_open(&j, p) != 0 && strstr(j.why, "mode 0600"), "key with mode 0644 refused: %s", j.why);
    chmod(k, 0600);
    CHECK(truncate(k, 16) == 0 && osh_journal_open(&j, p) != 0, "key of the wrong size refused");
    unlink(k);
    mkdir(k, 0700);
    CHECK(osh_journal_open(&j, p) != 0, "unreadable key (a directory) refused");
    rmdir(k);
    /* a missing key beside a journal with records: refused, no silent new key */
    FILE *f = fopen(p, "ab");
    fputs("OSHJ1 1 O 0 00 0/1 NOT_STARTED st=0 err=0\n", f);
    fclose(f);
    CHECK(osh_journal_open(&j, p) != 0 && strstr(j.why, "missing") && access(k, F_OK) != 0, "missing key beside a journal with records refused, no key invented: %s", j.why);
    /* a pre-existing journal with a wrong mode is REFUSED (not silently fixed), and nothing is changed */
    unlink(p);
    f = fopen(p, "w");
    fclose(f);
    chmod(p, 0644);
    CHECK(osh_journal_open(&j, p) != 0 && strstr(j.why, "mode 0600"), "journal with mode 0644 refused: %s", j.why);
    struct stat sb;
    CHECK(stat(p, &sb) == 0 && (sb.st_mode & 0777) == 0644, "and its mode was left alone");
    CHECK(osh_journal_check_dir(&j, dir) == 0, "private directory accepted");
    chmod(dir, 0755);
    CHECK(osh_journal_check_dir(&j, dir) != 0 && strstr(j.why, "0700"), "directory with mode 0755 refused: %s", j.why);
    chmod(dir, 0700);
}

/* library level: journal_required with no journal open refuses every effect and launches nothing */
static void test_required_without_journal(void)
{
    T t;
    t_begin(&t, 0);
    t.s.journal_required = 1;
    t.s.journal_error = "test: unwritable";
    char marker[4400];
    mpath(&t, "mreq", marker, sizeof marker);
    OshBuilder *b = nb();
    helper(b, "append", marker);
    OshResult r;
    int st = run(&t, b, &r);
    free(b);
    msleep(150);
    CHECK(st == 1 && r.err == OSH_E_IO && r.cmd[0].outcome == OSH_OUT_FAILED_NO_EFFECT && r.cmd[0].pid == 0 && fsize(marker) < 0,
          "required journal missing -> effect refused (st %d)", st);
    b = nb();
    osh_rb_cmd(b, OSH_B_PWD);
    osh_rb_arg(b, "pwd");
    st = run(&t, b, &r);
    free(b);
    CHECK(st == 0 && r.cmd[0].outcome == OSH_OUT_COMPLETED, "non-effect builtin unaffected");
    t_end(&t);
}

/* ---------------- review round: partial launch on a failed later intent, sticky failure, fsync, modes, hooks ---------------- */

void osh_hmac_for_test(const uint8_t key[32], const uint8_t *msg, size_t n, uint8_t out[32]);

static void test_hmac_vectors(void)
{
    /* RFC 4231 test cases 1 and 2 (keys shorter than 32 bytes are zero-extended, which HMAC treats as the same key) */
    uint8_t key[32] = {0}, out[32];
    char h[65];
    memset(key, 0x0b, 20);
    osh_hmac_for_test(key, (const uint8_t *)"Hi There", 8, out);
    hexs(out, h);
    CHECK(!strcmp(h, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"), "RFC 4231 case 1: %s", h);
    memset(key, 0, sizeof key);
    memcpy(key, "Jefe", 4);
    osh_hmac_for_test(key, (const uint8_t *)"what do ya want for nothing?", 28, out);
    hexs(out, h);
    CHECK(!strcmp(h, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"), "RFC 4231 case 2: %s", h);
}

/* The test program supplies its own fsync(2); calls on the journal's descriptor are counted, then really performed. If the
 * journal stopped calling fsync, the count below would fall short and this test fails. */
static int g_fsync_fd = -1, g_fsync_n;
int fsync(int fd)
{
    if (fd == g_fsync_fd) g_fsync_n++;
    return (int)syscall(SYS_fsync, fd);
}

static void test_fsync_counted(void)
{
    T t;
    t_begin(&t, 1);
    g_fsync_fd = t.j.fd;
    g_fsync_n = 0;
    OshBuilder *b = nb();
    helper(b, "quiet", NULL);
    OshResult r;
    run(&t, b, &r);
    free(b);
    CHECK(t.j.n_commit == 2 && g_fsync_n == 2, "one intent + one outcome = 2 commits, each followed by a real fsync on the journal (commits %d, fsyncs %d)", t.j.n_commit, g_fsync_n);
    g_fsync_fd = -1;
    t_end(&t);
}

static int zombies(void)
{
    int n = 0;
    while (waitpid(-1, NULL, WNOHANG) > 0) n++;
    return n;
}

static void test_pipeline_later_intent_fails(void)
{
    T t;
    t_begin(&t, 1);
    char m1[4400], m2[4400], m3[4400];
    mpath(&t, "pl1", m1, sizeof m1);
    mpath(&t, "pl2", m2, sizeof m2);
    mpath(&t, "pl3", m3, sizeof m3);
    OshBuilder *b = nb();
    helper(b, "sleep", "30000");   /* a: starts */
    helper(b, "append", m2);       /* b: its intent write fails */
    helper(b, "append", m3);       /* c: never reached */
    t.j.fail_write_at = 2;         /* commit 1 = a's intent, commit 2 = b's intent */
    OshResult r;
    int st = run(&t, b, &r);
    free(b);
    msleep(200);
    CHECK(st == 1 && r.err == OSH_E_PARTIAL_LAUNCH, "journal failure on member 2 of 3: PARTIAL_LAUNCH (st %d err %d)", st, r.err);
    CHECK(r.cmd[0].outcome == OSH_OUT_CANCELLED && r.cmd[0].pid > 0, "member 1 was started and is CANCELLED (%d)", r.cmd[0].outcome);
    CHECK(kill(r.cmd[0].pid, 0) != 0 && errno == ESRCH, "member 1 is gone, not running");
    CHECK(zombies() == 0, "and reaped: no zombie left");
    CHECK(r.cmd[1].outcome == OSH_OUT_FAILED_NO_EFFECT && r.cmd[1].pid == 0 && r.cmd[2].outcome == OSH_OUT_NOT_STARTED && r.cmd[2].pid == 0,
          "members 2 and 3 never launched (%d, %d)", r.cmd[1].outcome, r.cmd[2].outcome);
    CHECK(fsize(m2) < 0 && fsize(m3) < 0, "their marker files do not exist");
    CHECK(same_account(&t, 3, &r) == 0, "records match the result");
    char *j = jtext(&t);
    CHECK(count_sub(j, " I ") == 1 && strstr(j, " 0/3 CANCELLED") && strstr(j, " 1/3 FAILED_NO_EFFECT"), "journal: one intent, member 1 cancelled:\n%s", j);
    free(j);
    t_end(&t);
}

static int rec_numbers_unique(const char *text)
{
    unsigned long long seen[512];
    int n = 0, bad = 0;
    for (const char *p = text; p && *p;) {
        unsigned long long v;
        if (!strncmp(p, "OSHJ1 ", 6) && sscanf(p + 6, "%llu", &v) == 1) {
            for (int i = 0; i < n; i++) bad += seen[i] == v;
            if (n < 512) seen[n++] = v;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return bad;
}

static void sticky_case(int write_fail)
{
    T t;
    t_begin(&t, 1);
    char m1[4400], m2[4400];
    mpath(&t, write_fail ? "sk1" : "sk3", m1, sizeof m1);
    mpath(&t, write_fail ? "sk2" : "sk4", m2, sizeof m2);
    if (write_fail) t.j.fail_write_at = 1; else t.j.fail_fsync_at = 1;
    OshBuilder *b = nb();
    helper(b, "append", m1);
    OshResult r;
    int st = run(&t, b, &r);
    free(b);
    msleep(150);
    CHECK(st == 1 && fsize(m1) < 0, "%s: first command refused (st %d), nothing launched", write_fail ? "short write" : "fsync failure", st);
    CHECK(t.j.poisoned, "journal is poisoned");
    long size1 = fsize(t.jpath);
    char *before = slurp(t.jpath, NULL);
    int intents_before = count_sub(before, " I ");
    free(before);
    int commits = t.j.n_commit;
    /* the injected fault is one-shot; a second command must STILL be refused, by name, and write nothing */
    int err = dup(2), ef = open("/dev/null", O_WRONLY);
    char errf[4400];
    mpath(&t, "sk.err", errf, sizeof errf);
    int efd = open(errf, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    t.s.fd[2] = efd;
    b = nb();
    helper(b, "append", m2);
    st = run(&t, b, &r);
    free(b);
    close(efd); close(ef); close(err);
    msleep(150);
    char *e = slurp(errf, NULL);
    CHECK(st == 1 && r.err == OSH_E_IO && r.cmd[0].outcome == OSH_OUT_FAILED_NO_EFFECT && fsize(m2) < 0, "second command refused, nothing launched (st %d)", st);
    CHECK(e && strstr(e, "effect journal failed") && strstr(e, "nothing was run"), "refusal is named:\n%s", e ? e : "(none)");
    free(e);
    char *after = slurp(t.jpath, NULL);
    CHECK(count_sub(after, " I ") == intents_before, "no new intent was written after the poison");
    CHECK(write_fail || fsize(t.jpath) == size1, "after a failed fsync nothing at all is written (%ld vs %ld)", fsize(t.jpath), size1);
    CHECK(t.j.n_commit <= commits + 1, "at most an intent-less outcome line was attempted (%d vs %d)", t.j.n_commit, commits);
    free(after);
    /* recovery reads the file sanely: reopen, then run a command; record numbers never repeat and every line parses */
    osh_journal_close(&t.j);
    OshJournal j2;
    CHECK(osh_journal_open(&j2, t.jpath) == 0, "reopen after the failure: %s", j2.why);
    OshJournalUnknown u[8];
    int bad = 0;
    int nu = osh_journal_recover(&j2, t.jpath, u, 8, &bad);
    CHECK(nu >= 0 && bad == 0, "recovery reads the file sanely (unknown %d, unreadable lines %d)", nu, bad);
    t.s.journal = &j2;
    t.s.fd[2] = g_null;
    b = nb();
    helper(b, "quiet", NULL);
    st = run(&t, b, &r);
    free(b);
    char *txt = slurp(t.jpath, NULL);
    CHECK(st == 0 && rec_numbers_unique(txt) == 0, "after reopening, a new command runs and no record number repeats:\n%s", txt);
    free(txt);
    osh_journal_close(&j2);
    t.s.journal = NULL;
    osh_session_free(&t.s);
}

static void test_sticky_failure(void)
{
    sticky_case(1);
    sticky_case(0);
}

static void test_explicit_journal_modes(void)
{
    char dir[700], p[1400], k[1500];
    snprintf(dir, sizeof dir, "%s/em", g_dir);
    mkdir(dir, 0700);
    snprintf(p, sizeof p, "%s/j.log", dir);
    snprintf(k, sizeof k, "%s.key", p);
    OshJournal j;
    CHECK(osh_journal_open(&j, p) == 0, "fresh");
    osh_journal_close(&j);
    chmod(p, 0400);
    CHECK(osh_journal_open(&j, p) != 0 && strstr(j.why, "0600"), "journal mode 0400 refused: %s", j.why);
    chmod(p, 0700);
    CHECK(osh_journal_open(&j, p) != 0 && strstr(j.why, "0600"), "journal mode 0700 refused: %s", j.why);
    chmod(p, 0600);
    CHECK(osh_journal_open(&j, p) == 0, "exactly 0600 accepted");
    osh_journal_close(&j);
    /* mkparents creates every missing level at 0700 */
    char deep[1800];
    snprintf(deep, sizeof deep, "%s/mp/a/b/effects.journal", g_dir);
    CHECK(osh_journal_mkparents(deep) == 0, "mkparents");
    struct stat sb;
    char d1[1800];
    snprintf(d1, sizeof d1, "%s/mp/a/b", g_dir);
    CHECK(stat(d1, &sb) == 0 && (sb.st_mode & 0777) == 0700, "created directory is 0700");
    CHECK(osh_journal_mkparents(deep) == 0, "and is idempotent");
}

static void test_cli_explicit_dir(const char *osh)
{
    char pol[700], ef[700], sd[700], jp[900], mk[700], sc[1500];
    snprintf(pol, sizeof pol, "%s/cli.policy", g_dir);
    snprintf(ef, sizeof ef, "%s/cli2.err", g_dir);
    snprintf(sd, sizeof sd, "%s/ed", g_dir);
    snprintf(jp, sizeof jp, "%s/j.log", sd);
    snprintf(mk, sizeof mk, "%s/cli2.marker", g_dir);
    snprintf(sc, sizeof sc, "/bin/touch %s", mk);
    mkdir(sd, 0755);
    CHECK(run_osh(osh, pol, jp, sc, ef, NULL, "/tmp") == 1 && fsize(mk) < 0, "explicit --journal in a 0755 directory: effect refused, nothing launched");
    char *e = slurp(ef, NULL);
    CHECK(strstr(e, "state directory") && strstr(e, "0700"), "named: %s", e);
    free(e);
    chmod(sd, 0700);
    CHECK(run_osh(osh, pol, jp, sc, ef, NULL, "/tmp") == 0 && fsize(mk) == 0, "control: with a private directory it runs");
}

/* ONE SESSION PER JOURNAL (flock). Start osh in its own process group without waiting. */
static pid_t start_osh(const char *osh, const char *caps, const char *jpath, const char *script, const char *errf)
{
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        int fd = open(errf, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        dup2(fd, 2);
        dup2(g_null, 1);
        const char *av[8] = {osh, "--caps", caps, "--journal", jpath, "-c", script, NULL};
        execv(osh, (char *const *)av);
        _exit(127);
    }
    setpgid(pid, pid);
    return pid;
}

static void test_cli_one_session(const char *osh)
{
    char pol[700], jp[900], ef1[700], ef2[700], ef3[700], mk[700], sc[1500], key[1000];
    snprintf(pol, sizeof pol, "%s/cli.policy", g_dir);
    snprintf(jp, sizeof jp, "%s/lock.log", g_dir);
    snprintf(key, sizeof key, "%s.key", jp);
    snprintf(ef1, sizeof ef1, "%s/lk1.err", g_dir);
    snprintf(ef2, sizeof ef2, "%s/lk2.err", g_dir);
    snprintf(ef3, sizeof ef3, "%s/lk3.err", g_dir);
    snprintf(mk, sizeof mk, "%s/lk.marker", g_dir);
    pid_t a = start_osh(osh, pol, jp, "/bin/sleep 2; /bin/true", ef1);
    for (int i = 0; i < 200 && !(fsize(jp) > 0); i++) msleep(25);
    CHECK(fsize(jp) > 0, "first session holds the journal and has written its intent");
    /* second session on the SAME journal: effects refused by name, nothing launched, builtins still work */
    snprintf(sc, sizeof sc, "/bin/touch %s", mk);
    CHECK(run_osh(osh, pol, jp, sc, ef2, NULL, "/tmp") == 1 && fsize(mk) < 0, "second session: effect refused, nothing launched");
    char *e = slurp(ef2, NULL);
    CHECK(strstr(e, "in use by another osh session") && strstr(e, "--journal"), "named, and says to use --journal:\n%s", e);
    free(e);
    CHECK(run_osh(osh, pol, jp, "printf x", ef2, NULL, "/tmp") == 0, "second session: a builtin without a write still runs");
    int st = 0;
    waitpid(a, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "first session unaffected (exit %d)", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    char *j = slurp(jp, NULL);
    CHECK(count_sub(j, " I ") == 2 && count_sub(j, " O ") == 2 && rec_numbers_unique(j) == 0, "no duplicate record numbers, nothing from the refused session:\n%s", j);
    free(j);
    /* the lock dies with the process: SIGKILL a session mid-command, a new session takes the lock and recovery reports the open intent once */
    unlink(jp);
    unlink(key);
    pid_t b = start_osh(osh, pol, jp, "/bin/sleep 30", ef1);
    for (int i = 0; i < 200 && !(fsize(jp) > 0); i++) msleep(25);
    CHECK(fsize(jp) > 0, "second run: intent written");
    kill(-b, SIGKILL);
    waitpid(b, NULL, 0);
    msleep(100);
    snprintf(sc, sizeof sc, "/bin/touch %s", mk);
    CHECK(run_osh(osh, pol, jp, sc, ef3, NULL, "/tmp") == 0 && fsize(mk) == 0, "after the holder was killed, a new session gets the lock and runs");
    e = slurp(ef3, NULL);
    CHECK(count_sub(e, "has outcome UNKNOWN") == 1, "recovery marks the killed session's open intent UNKNOWN exactly once:\n%s", e);
    free(e);
    CHECK(run_osh(osh, pol, jp, "/bin/true", ef3, NULL, "/tmp") == 0, "next start");
    e = slurp(ef3, NULL);
    CHECK(count_sub(e, "UNKNOWN") == 0, "and not again");
    free(e);
    j = slurp(jp, NULL);
    CHECK(rec_numbers_unique(j) == 0, "record numbers unique across sessions:\n%s", j);
    free(j);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "--helper")) return helper_main(argc, argv);
    const char *osh_bin = argc >= 2 ? argv[1] : NULL;
    ssize_t n = readlink("/proc/self/exe", g_self, sizeof g_self - 1);
    if (n <= 0) return 1;
    g_self[n] = 0;
    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/osh-journal-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(g_dir)) return 1;
    g_null = open("/dev/null", O_RDWR);
    test_secret_names();
    test_keyed_digest();
    test_normal_and_redaction();
    test_denial();
    test_interrupt();
    test_partial_launch();
    test_echild_unknown();
    test_kill_between_and_recovery();
    test_crash_before_launch();
    test_fail_closed();
    test_non_effects_and_direct();
    test_torn_tail_and_cleanup();
    test_required_without_journal();
    test_key_and_modes();
    test_hmac_vectors();
    test_fsync_counted();
    test_pipeline_later_intent_fails();
    test_sticky_failure();
    test_explicit_journal_modes();
    if (osh_bin) { test_cli(osh_bin); test_cli_explicit_dir(osh_bin); test_cli_one_session(osh_bin); }
    close(g_null);
    char cmd[4400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    if (system(cmd)) {}
    printf("%d checks, %d failed\n", g_checks, g_fail);
    printf(g_fail ? "OSH_JOURNAL_FAIL\n" : "OSH_JOURNAL_PASS\n");
    return g_fail ? 1 : 0;
}
