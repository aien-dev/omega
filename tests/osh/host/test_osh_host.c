/*
 * test_osh_host.c -- tests for the osh Linux host adapter execution service (aien-architecture#158).
 * Records are hand-built (no parser exists here). Everything happens in a disposable temp dir under $TMPDIR.
 * The test binary is also its own helper program: `test_osh_host --helper <op> ...` (cat, sum, gen, fdlist, env,
 * touch, kill, sleep, echo) so no external tool except yes/head/printf (used by name for the comparisons) is needed.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "osh_host.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ================= helper mode ================= */

static uint32_t fnv(uint32_t h, const unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static unsigned char gen_byte(unsigned long i) { return (unsigned char)((i * 2654435761u) >> 13); }

static int helper_main(int argc, char **argv)
{
    const char *op = argc > 2 ? argv[2] : "";
    if (!strcmp(op, "echo")) {
        for (int i = 3; i < argc; i++) printf("%s%s", i > 3 ? " " : "", argv[i]);
        printf("\n");
        return 0;
    }
    if (!strcmp(op, "cat")) {
        char b[65536];
        ssize_t n;
        while ((n = read(0, b, sizeof b)) > 0)
            if (write(1, b, (size_t)n) != n) return 1;
        return 0;
    }
    if (!strcmp(op, "sum")) {
        unsigned char b[65536];
        ssize_t n;
        uint32_t h = 2166136261u;
        unsigned long tot = 0;
        while ((n = read(0, b, sizeof b)) > 0) { h = fnv(h, b, (size_t)n); tot += (unsigned long)n; }
        printf("%lu %08x\n", tot, h);
        return 0;
    }
    if (!strcmp(op, "gen")) {
        unsigned long n = strtoul(argv[3], NULL, 10);
        unsigned char b[65536];
        for (unsigned long off = 0; off < n;) {
            size_t k = n - off < sizeof b ? (size_t)(n - off) : sizeof b;
            for (size_t i = 0; i < k; i++) b[i] = gen_byte(off + i);
            if (write(1, b, k) != (ssize_t)k) return 1;
            off += k;
        }
        return 0;
    }
    if (!strcmp(op, "fdlist")) { /* open descriptors above 2, excluding the directory handle used to list them */
        DIR *d = opendir("/proc/self/fd");
        int dfd = dirfd(d), cnt = 0;
        struct dirent *e;
        while ((e = readdir(d)))
            if (e->d_name[0] != '.' && atoi(e->d_name) > 2 && atoi(e->d_name) != dfd) {
                char l[320], t[256];
                snprintf(l, sizeof l, "/proc/self/fd/%s", e->d_name);
                ssize_t n = readlink(l, t, sizeof t - 1);
                t[n > 0 ? n : 0] = 0;
                printf("leak fd %s -> %s\n", e->d_name, t);
                cnt++;
            }
        closedir(d);
        printf("fdcount %d\n", cnt);
        return 0;
    }
    if (!strcmp(op, "env")) {
        const char *v = getenv(argv[3]);
        printf("%s\n", v ? v : "<unset>");
        return 0;
    }
    if (!strcmp(op, "envcount")) {
        extern char **environ;
        int n = 0;
        for (char **e = environ; *e; e++) n++;
        printf("%d\n", n);
        return 0;
    }
    if (!strcmp(op, "touch")) {
        int fd = open(argv[3], O_WRONLY | O_CREAT, 0666);
        return fd < 0 ? 1 : 0;
    }
    if (!strcmp(op, "kill")) { raise(atoi(argv[3])); return 0; }
    if (!strcmp(op, "sleep")) {
        struct timespec ts = {atoi(argv[3]) / 1000, (atoi(argv[3]) % 1000) * 1000000L};
        nanosleep(&ts, NULL);
        return 0;
    }
    if (!strcmp(op, "exit")) return atoi(argv[3]);
    if (!strcmp(op, "pgid")) { printf("%d\n", (int)getpgrp()); return 0; }
    if (!strcmp(op, "sigdisp")) { /* how SIGINT / SIGQUIT arrive in this process */
        struct sigaction a, q;
        sigaction(SIGINT, NULL, &a);
        sigaction(SIGQUIT, NULL, &q);
        printf("INT %s QUIT %s\n", a.sa_handler == SIG_IGN ? "ign" : a.sa_handler == SIG_DFL ? "dfl" : "handler", q.sa_handler == SIG_IGN ? "ign" : q.sa_handler == SIG_DFL ? "dfl" : "handler");
        return 0;
    }
    if (!strcmp(op, "sigblk")) { /* number of signals TERM/USR1/INT/QUIT blocked here */
        sigset_t m;
        sigprocmask(SIG_BLOCK, NULL, &m);
        printf("blocked %d\n", sigismember(&m, SIGTERM) + sigismember(&m, SIGUSR1) + sigismember(&m, SIGINT) + sigismember(&m, SIGQUIT));
        return 0;
    }
    if (!strcmp(op, "both")) { /* write to stdout and stderr */
        printf("out\n"); fflush(stdout);
        fprintf(stderr, "err\n");
        return 0;
    }
    return 99;
}

/* ================= harness ================= */

static int g_fail, g_pass;
static char g_self[4096], g_tmp[4096];

#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

static size_t g_slurp_len;

static char *slurp(const char *path)
{
    g_slurp_len = 0;
    FILE *f = fopen(path, "rb");
    char *b = calloc(1, 1 << 20);
    if (f) { size_t n = fread(b, 1, (1 << 20) - 1, f); b[n] = 0; g_slurp_len = n; fclose(f); }
    return b;
}

static void wfile(const char *path, const char *s, mode_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd >= 0) { if (write(fd, s, strlen(s)) < 0) {} close(fd); }
    chmod(path, mode);
}

static char *P(const char *rel) /* path in the temp dir (static ring) */
{
    static char ring[16][4200];
    static int k;
    char *r = ring[k++ & 15];
    snprintf(r, 4200, "%s/%s", g_tmp, rel);
    return r;
}

typedef struct {
    OshSession s;
    char op[4200], ep[4200];
    int ofd, efd;
} T;

static int count_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) if (e->d_name[0] != '.') n++;
    closedir(d);
    return n - 1; /* the DIR itself */
}

static void t_begin(T *t, const char *path)
{
    extern char **environ;
    char *env[] = {"HOME=/nonexistent-home", "LANG=C", NULL};
    (void)environ;
    osh_session_init(&t->s, env);
    osh_var_export(&t->s, "PATH", path);
    snprintf(t->op, sizeof t->op, "%s/cap.out", g_tmp);
    snprintf(t->ep, sizeof t->ep, "%s/cap.err", g_tmp);
    t->ofd = open(t->op, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    t->efd = open(t->ep, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    t->s.fd[1] = t->ofd;
    t->s.fd[2] = t->efd;
    t->s.binding.valid = 1;
    t->s.binding.domain = 2;
    t->s.binding.cap_generation = 1;
}

static void t_end(T *t)
{
    close(t->ofd);
    close(t->efd);
    osh_session_free(&t->s);
}

static char *t_out(T *t) { return slurp(t->op); }
static char *t_err(T *t) { return slurp(t->ep); }

static void t_clear(T *t)
{
    if (ftruncate(t->ofd, 0)) {}
    if (ftruncate(t->efd, 0)) {}
    lseek(t->ofd, 0, SEEK_SET);
    lseek(t->efd, 0, SEEK_SET);
}

/* build a command: cmd(b, builtin_id, "a", "b", NULL) */
static void cmd(OshBuilder *b, int bi, ...)
{
    osh_rb_cmd(b, bi);
    va_list ap;
    va_start(ap, bi);
    for (const char *a; (a = va_arg(ap, const char *));) osh_rb_arg(b, a);
    va_end(ap);
}

static void helper(OshBuilder *b, ...) /* a command that runs this binary as a helper */
{
    osh_rb_cmd(b, 0);
    osh_rb_arg(b, g_self);
    osh_rb_arg(b, "--helper");
    va_list ap;
    va_start(ap, b);
    for (const char *a; (a = va_arg(ap, const char *));) osh_rb_arg(b, a);
    va_end(ap);
}

static int go(T *t, OshBuilder *b, OshResult *res)
{
    osh_rb_seal(b);
    CHECK(b->err == 0, "builder error %d", b->err);
    return osh_exec_record(&t->s, b->rec, osh_rb_cells(b), b->out, b->out_used, res);
}

static OshBuilder *nb(unsigned flags, int conn)
{
    static OshBuilder b;
    osh_rb_init(&b, flags, conn);
    return &b;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int rm_cb(const char *p, const struct stat *sb, int tf, struct FTW *w)
{
    (void)sb; (void)tf; (void)w;
    return remove(p);
}

/* ================= tests ================= */

static void test_simple_and_path(void)
{
    T t;
    t_begin(&t, "/nonexistent-dir:/usr/bin:/bin");
    OshResult r;
    OshBuilder *b = nb(0, 0);
    cmd(b, 0, "printf", "%s\n", "via-path", NULL);
    CHECK(go(&t, b, &r) == 0 && r.cmd[0].outcome == OSH_OUT_COMPLETED, "status %d", r.status);
    char *o = t_out(&t);
    CHECK(strcmp(o, "via-path\n") == 0, "got '%s'", o);
    free(o);
    CHECK(t.s.last_status == 0, "last_status");

    t_clear(&t);
    b = nb(0, 0);
    cmd(b, 0, "definitely-not-a-command-xyz", NULL);
    CHECK(go(&t, b, &r) == 127 && r.cmd[0].outcome == OSH_OUT_FAILED_NO_EFFECT && r.cmd[0].pid == 0, "PATH miss -> 127");
    char *e = t_err(&t);
    CHECK(strstr(e, "not found") != NULL, "diag '%s'", e);
    free(e);

    /* non-executable file: 126, found before a later dir that has an executable of that name? (first usable wins) */
    mkdir(P("bin1"), 0700);
    mkdir(P("bin2"), 0700);
    wfile(P("bin1/tool"), "#!/bin/true\n", 0644);
    CHECK(osh_var_set(&t.s, "PATH", P("bin1")) == 0, "set PATH");
    b = nb(0, 0);
    cmd(b, 0, "tool", NULL);
    CHECK(go(&t, b, &r) == 126, "non-exec -> 126, got %d", r.status);
    b = nb(0, 0);
    cmd(b, 0, P("bin1/tool"), NULL);
    CHECK(go(&t, b, &r) == 126, "path with slash non-exec -> 126");
    b = nb(0, 0);
    cmd(b, 0, P("bin1"), NULL);
    CHECK(go(&t, b, &r) == 126, "directory -> 126");
    b = nb(0, 0);
    cmd(b, 0, P("bin1/absent"), NULL);
    CHECK(go(&t, b, &r) == 127, "absent path with slash -> 127");

    /* a non-exec file in an earlier dir does not hide an executable in a later one (bash/dash behaviour) */
    {
        int in = open("/bin/true", O_RDONLY), out = open(P("bin2/tool"), O_WRONLY | O_CREAT | O_TRUNC, 0755);
        char buf[65536]; ssize_t n;
        while (in >= 0 && out >= 0 && (n = read(in, buf, sizeof buf)) > 0) if (write(out, buf, (size_t)n) < 0) break;
        if (in >= 0) close(in);
        if (out >= 0) close(out);
        chmod(P("bin2/tool"), 0755);
    }
    char pp[9000];
    snprintf(pp, sizeof pp, "%s:%s", P("bin1"), P("bin2"));
    osh_var_set(&t.s, "PATH", pp);
    b = nb(0, 0);
    cmd(b, 0, "tool", NULL);
    CHECK(go(&t, b, &r) == 0, "later executable found past a non-exec, got %d", r.status);

    /* a name containing '/' is never searched: bin2 is on PATH but "bin2/tool" relative to cwd is not found there */
    osh_var_set(&t.s, "PATH", g_tmp);
    b = nb(0, 0);
    cmd(b, 0, "bin2/tool", NULL);
    char *cwd0 = getcwd(NULL, 0);
    CHECK(chdir("/") == 0, "chdir /");
    CHECK(go(&t, b, &r) == 127, "slash names are not PATH-searched, got %d", r.status);

    /* empty PATH element = cwd (POSIX); also leading, trailing and "::" */
    CHECK(chdir(P("bin2")) == 0, "chdir bin2");
    const char *empties[] = {":/nonexistent", "/nonexistent:", "/nonexistent::/nonexistent2", ""};
    for (int i = 0; i < 4; i++) {
        osh_var_set(&t.s, "PATH", empties[i]);
        b = nb(0, 0);
        cmd(b, 0, "tool", NULL);
        CHECK(go(&t, b, &r) == 0, "empty PATH element means cwd: '%s' -> %d", empties[i], r.status);
    }
    /* PATH unset: bare names not found, no fallback to the process environment */
    osh_var_unset(&t.s, "PATH");
    b = nb(0, 0);
    cmd(b, 0, "tool", NULL);
    CHECK(go(&t, b, &r) == 127, "PATH unset -> 127, got %d", r.status);
    /* a prefix assignment PATH=... applies to the lookup of that command */
    b = nb(0, 0);
    cmd(b, 0, "tool", NULL);
    osh_rb_assign(b, "PATH", ".");
    CHECK(go(&t, b, &r) == 0, "PATH= prefix assignment used for lookup, got %d", r.status);
    CHECK(chdir(cwd0) == 0, "chdir back");
    free(cwd0);
    t_end(&t);
}

static void test_builtins(void)
{
    T t;
    t_begin(&t, "/usr/bin:/bin");
    mkdir(P("d1"), 0700);
    mkdir(P("d1/sub"), 0700);
    OshResult r;
    char *cwd0 = getcwd(NULL, 0);
    osh_var_set(&t.s, "PWD", cwd0);

    /* cd with explicit dir, PWD/OLDPWD, pwd logical */
    OshBuilder *b = nb(0, 0);
    cmd(b, OSH_B_CD, "cd", P("d1"), NULL);
    CHECK(go(&t, b, &r) == 0, "cd ok");
    CHECK(osh_var_get(&t.s, "PWD") && strcmp(osh_var_get(&t.s, "PWD"), P("d1")) == 0, "PWD=%s", osh_var_get(&t.s, "PWD"));
    CHECK(osh_var_get(&t.s, "OLDPWD") && strcmp(osh_var_get(&t.s, "OLDPWD"), cwd0) == 0, "OLDPWD");
    b = nb(0, 0);
    cmd(b, OSH_B_CD, "cd", "sub/../sub/.", NULL);
    CHECK(go(&t, b, &r) == 0, "relative cd");
    CHECK(strcmp(osh_var_get(&t.s, "PWD"), P("d1/sub")) == 0, "lexical PWD got %s", osh_var_get(&t.s, "PWD"));
    b = nb(0, 0);
    cmd(b, OSH_B_PWD, "pwd", NULL);
    CHECK(go(&t, b, &r) == 0, "pwd");
    char *o = t_out(&t);
    char want[4300];
    snprintf(want, sizeof want, "%s\n", P("d1/sub"));
    CHECK(strcmp(o, want) == 0, "pwd got '%s'", o);
    free(o);
    char *cw = getcwd(NULL, 0);
    CHECK(strcmp(cw, P("d1/sub")) == 0 || strstr(cw, "d1/sub"), "process cwd moved: %s", cw);
    free(cw);

    /* logical pwd through a symlink */
    if (symlink(P("d1"), P("link"))) g_fail++;
    b = nb(0, 0);
    cmd(b, OSH_B_CD, "cd", P("link"), NULL);
    go(&t, b, &r);
    t_clear(&t);
    b = nb(0, 0);
    cmd(b, OSH_B_PWD, "pwd", NULL);
    go(&t, b, &r);
    o = t_out(&t);
    snprintf(want, sizeof want, "%s\n", P("link"));
    CHECK(strcmp(o, want) == 0, "pwd is logical through symlink, got '%s'", o);
    free(o);
    t_clear(&t);
    b = nb(0, 0);
    cmd(b, OSH_B_PWD, "pwd", "-P", NULL);
    go(&t, b, &r);
    o = t_out(&t);
    CHECK(strstr(o, "d1\n") != NULL && strstr(o, "link") == NULL, "pwd -P physical, got '%s'", o);
    free(o);

    /* cd with HOME default, cd -, errors */
    osh_var_set(&t.s, "HOME", P("d1/sub"));
    b = nb(0, 0);
    cmd(b, OSH_B_CD, "cd", P("d1"), NULL);
    go(&t, b, &r);
    b = nb(0, 0);
    cmd(b, OSH_B_CD, "cd", NULL);
    CHECK(go(&t, b, &r) == 0 && strcmp(osh_var_get(&t.s, "PWD"), P("d1/sub")) == 0, "cd with no arg goes to HOME");
    t_clear(&t);
    b = nb(0, 0);
    cmd(b, OSH_B_CD, "cd", "-", NULL);
    CHECK(go(&t, b, &r) == 0 && strcmp(osh_var_get(&t.s, "PWD"), P("d1")) == 0, "cd - returns");
    o = t_out(&t);
    snprintf(want, sizeof want, "%s\n", P("d1"));
    CHECK(strcmp(o, want) == 0, "cd - prints the directory, got '%s'", o);
    free(o);
    osh_var_unset(&t.s, "HOME");
    b = nb(0, 0);
    cmd(b, OSH_B_CD, "cd", NULL);
    CHECK(go(&t, b, &r) == 1, "cd without HOME fails");
    b = nb(0, 0);
    cmd(b, OSH_B_CD, "cd", P("absent-dir"), NULL);
    CHECK(go(&t, b, &r) == 1 && strcmp(osh_var_get(&t.s, "PWD"), P("d1")) == 0, "failed cd keeps PWD");

    /* export / unset in parent; exported reaches children, unexported does not */
    b = nb(0, 0);
    cmd(b, OSH_B_EXPORT, "export", "XA=1", "XB", NULL);
    CHECK(go(&t, b, &r) == 0, "export");
    osh_var_set(&t.s, "XC", "3"); /* shell variable, not exported */
    t_clear(&t);
    b = nb(0, 0);
    helper(b, "env", "XA", NULL);
    go(&t, b, &r);
    b = nb(0, 0);
    helper(b, "env", "XC", NULL);
    go(&t, b, &r);
    b = nb(0, 0);
    helper(b, "env", "XB", NULL);
    go(&t, b, &r);
    o = t_out(&t);
    CHECK(strcmp(o, "1\n<unset>\n<unset>\n") == 0, "export visibility got '%s'", o);
    free(o);
    b = nb(0, 0);
    cmd(b, OSH_B_EXPORT, "export", "XC", NULL); /* now export an existing shell var */
    go(&t, b, &r);
    t_clear(&t);
    b = nb(0, 0);
    helper(b, "env", "XC", NULL);
    go(&t, b, &r);
    o = t_out(&t);
    CHECK(strcmp(o, "3\n") == 0, "exported later, got '%s'", o);
    free(o);
    b = nb(0, 0);
    cmd(b, OSH_B_EXPORT, "export", "1bad", NULL);
    CHECK(go(&t, b, &r) == 1, "export invalid identifier");
    b = nb(0, 0);
    cmd(b, OSH_B_UNSET, "unset", "XA", NULL);
    CHECK(go(&t, b, &r) == 0 && osh_var_get(&t.s, "XA") == NULL, "unset");
    t_clear(&t);
    b = nb(0, 0);
    helper(b, "env", "XA", NULL);
    go(&t, b, &r);
    o = t_out(&t);
    CHECK(strcmp(o, "<unset>\n") == 0, "unset reaches children, got '%s'", o);
    free(o);

    /* exit: parent only when alone */
    b = nb(0, 0);
    cmd(b, OSH_B_EXIT, "exit", "7", NULL);
    CHECK(go(&t, b, &r) == 7 && t.s.exit_requested && t.s.exit_status == 7 && r.exit_requested, "exit 7 in parent");
    t.s.exit_requested = 0;
    t.s.last_status = 5;
    b = nb(0, 0);
    cmd(b, OSH_B_EXIT, "exit", NULL);
    CHECK(go(&t, b, &r) == 5 && t.s.exit_status == 5, "exit with no arg uses $?");
    t.s.exit_requested = 0;
    b = nb(0, 0);
    cmd(b, OSH_B_EXIT, "exit", "abc", NULL);
    CHECK(go(&t, b, &r) == 2, "exit non-numeric -> 2");
    t.s.exit_requested = 0;

    /* inside a pipeline builtins run in a child: parent state is unchanged */
    const char *before_pwd = strdup(osh_var_get(&t.s, "PWD"));
    char *cw0 = getcwd(NULL, 0);
    t_clear(&t);
    b = nb(0, 0);
    cmd(b, OSH_B_CD, "cd", P("d1/sub"), NULL);
    helper(b, "cat", NULL);
    t.s.fd[0] = open("/dev/null", O_RDONLY | O_CLOEXEC);
    CHECK(go(&t, b, &r) == 0, "cd | cat");
    char *cw1 = getcwd(NULL, 0);
    CHECK(strcmp(cw0, cw1) == 0 && strcmp(osh_var_get(&t.s, "PWD"), before_pwd) == 0, "cd in pipeline does not move the parent");
    free(cw0); free(cw1);
    b = nb(0, 0);
    cmd(b, OSH_B_EXPORT, "export", "PIPEVAR=1", NULL);
    helper(b, "cat", NULL);
    go(&t, b, &r);
    CHECK(osh_var_get(&t.s, "PIPEVAR") == NULL, "export in pipeline does not touch parent");
    b = nb(0, 0);
    cmd(b, OSH_B_UNSET, "unset", "XC", NULL);
    helper(b, "cat", NULL);
    go(&t, b, &r);
    CHECK(osh_var_get(&t.s, "XC") != NULL, "unset in pipeline does not touch parent");
    b = nb(0, 0);
    cmd(b, OSH_B_EXIT, "exit", "9", NULL);
    helper(b, "cat", NULL);
    int st = go(&t, b, &r);
    CHECK(!t.s.exit_requested && st == 0 && r.cmd[0].status == 9, "exit in pipeline only ends its child (cmd0 status %d)", r.cmd[0].status);
    /* last command a builtin in a pipeline: its status is the pipeline status, output goes through the pipe path */
    t_clear(&t);
    b = nb(0, 0);
    helper(b, "echo", "x", NULL);
    cmd(b, OSH_B_PWD, "pwd", NULL);
    CHECK(go(&t, b, &r) == 0, "echo | pwd");
    o = t_out(&t);
    CHECK(o[0] == '/', "pwd inside pipeline wrote to stdout '%s'", o);
    free(o);
    close(t.s.fd[0]);
    t.s.fd[0] = 0;

    /* standalone assignment: shell variable, not exported; with argv: per command environment only */
    b = nb(0, 0);
    osh_rb_cmd(b, 0);
    osh_rb_assign(b, "SA", "one");
    CHECK(go(&t, b, &r) == 0 && strcmp(osh_var_get(&t.s, "SA"), "one") == 0, "standalone assignment sets shell var");
    t_clear(&t);
    b = nb(0, 0);
    helper(b, "env", "SA", NULL);
    go(&t, b, &r);
    o = t_out(&t);
    CHECK(strcmp(o, "<unset>\n") == 0, "standalone assignment is not exported, got '%s'", o);
    free(o);
    t_clear(&t);
    b = nb(0, 0);
    helper(b, "env", "PA", NULL);
    osh_rb_assign(b, "PA", "temp");
    go(&t, b, &r);
    o = t_out(&t);
    CHECK(strcmp(o, "temp\n") == 0 && osh_var_get(&t.s, "PA") == NULL, "prefix assignment is per-command, got '%s'", o);
    free(o);
    t_clear(&t);
    b = nb(0, 0);
    helper(b, "env", "SA", NULL);
    osh_rb_assign(b, "SA", "over"); /* overrides, and does not change the shell variable */
    go(&t, b, &r);
    o = t_out(&t);
    CHECK(strcmp(o, "over\n") == 0 && strcmp(osh_var_get(&t.s, "SA"), "one") == 0, "prefix over shell var '%s'", o);
    free(o);
    /* env is built fresh: exactly the exported table plus the override, nothing from the test process environ */
    t_clear(&t);
    int nexp = 0;
    for (size_t i = 0; i < t.s.nvars; i++) nexp += t.s.vars[i].exported && t.s.vars[i].value;
    setenv("LEAK_FROM_PROCESS_ENV", "1", 1);
    b = nb(0, 0);
    helper(b, "envcount", NULL);
    go(&t, b, &r);
    o = t_out(&t);
    CHECK(atoi(o) == nexp, "child env has %d entries, expected %d", atoi(o), nexp);
    free(o);
    unsetenv("LEAK_FROM_PROCESS_ENV");

    CHECK(chdir(cwd0) == 0, "restore cwd");
    free(cwd0);
    free((void *)before_pwd);
    t_end(&t);
}

static void printf_case(T *t, const char *fmt, const char *a1, const char *a2, const char *a3, const char *a4, const char *a5, const char *a6)
{
    OshResult r;
    const char *args[6] = {a1, a2, a3, a4, a5, a6};
    char *res[2];
    int st[2];
    for (int k = 0; k < 2; k++) {
        t_clear(t);
        OshBuilder *b = nb(0, 0);
        if (k == 0) cmd(b, OSH_B_PRINTF, "printf", fmt, NULL);
        else cmd(b, 0, "/usr/bin/printf", fmt, NULL);
        for (int i = 0; i < 6 && args[i]; i++) osh_rb_arg(b, args[i]);
        st[k] = go(t, b, &r);
        res[k] = t_out(t);
    }
    CHECK(strcmp(res[0], res[1]) == 0 && st[0] == st[1], "printf '%s': builtin '%s' (%d) vs /usr/bin/printf '%s' (%d)", fmt, res[0], st[0], res[1], st[1]);
    free(res[0]);
    free(res[1]);
}

static void test_printf(void)
{
    T t;
    t_begin(&t, "/usr/bin:/bin");
    printf_case(&t, "%s\n", "hello", NULL, NULL, NULL, NULL, NULL);
    printf_case(&t, "%d %i %u %x %o %c|%%\n", "42", "-7", "7", "255", "8", "xyz");
    printf_case(&t, "%s %s\n", "a", "b", "c", "d", NULL, NULL);
    printf_case(&t, "%s %s\n", "a", "b", "c", NULL, NULL, NULL);
    printf_case(&t, "\\t|\\101|\\\\|\\n|\\a\\b\\f\\r\\v|\\\"|\n", NULL, NULL, NULL, NULL, NULL, NULL);
    printf_case(&t, "no conversion\n", "ignored", NULL, NULL, NULL, NULL, NULL);
    printf_case(&t, "%s\n", NULL, NULL, NULL, NULL, NULL, NULL);
    printf_case(&t, "%d|%u\n", NULL, NULL, NULL, NULL, NULL, NULL);
    printf_case(&t, "%d %d %d\n", "0x1f", "010", "99", NULL, NULL, NULL);
    printf_case(&t, "%d\n", "'A", NULL, NULL, NULL, NULL, NULL);
    printf_case(&t, "%s\n", "a\\nb", NULL, NULL, NULL, NULL, NULL);
    printf_case(&t, "%x %o\n", "3735928559", "511", NULL, NULL, NULL, NULL);
    printf_case(&t, "%d\n", "abc", NULL, NULL, NULL, NULL, NULL);
    printf_case(&t, "%s", "x", "y", "z", NULL, NULL, NULL);
    printf_case(&t, "100%%\n", NULL, NULL, NULL, NULL, NULL, NULL);
    printf_case(&t, "\\0101\n", NULL, NULL, NULL, NULL, NULL, NULL);
    /* outside the documented subset: refused, nothing written, status 1 */
    OshResult r;
    t_clear(&t);
    OshBuilder *b = nb(0, 0);
    cmd(b, OSH_B_PRINTF, "printf", "%5d\n", "3", NULL);
    CHECK(go(&t, b, &r) == 1, "width refused");
    char *o = t_out(&t);
    CHECK(o[0] == 0, "nothing printed on refusal, got '%s'", o);
    free(o);
    t_clear(&t);
    b = nb(0, 0);
    cmd(b, OSH_B_PRINTF, "printf", "a\\qb\n", NULL);
    go(&t, b, &r);
    o = t_out(&t);
    CHECK(strcmp(o, "a\\qb\n") == 0, "unknown escape kept verbatim, got '%s'", o);
    free(o);
    b = nb(0, 0);
    cmd(b, OSH_B_PRINTF, "printf", NULL);
    CHECK(go(&t, b, &r) == 2, "printf with no format (bash: 2)");
    t_end(&t);
}

static void test_redirs(void)
{
    T t;
    t_begin(&t, "/usr/bin:/bin");
    OshResult r;
    OshBuilder *b;
    char *o;

    /* > and < */
    b = nb(0, 0);
    helper(b, "echo", "hello", NULL);
    osh_rb_redir(b, OSH_R_OUT, 1, P("o1"), 0);
    CHECK(go(&t, b, &r) == 0, "> file");
    o = slurp(P("o1"));
    CHECK(strcmp(o, "hello\n") == 0, "> content '%s'", o);
    free(o);
    b = nb(0, 0);
    helper(b, "cat", NULL);
    osh_rb_redir(b, OSH_R_IN, 0, P("o1"), 0);
    osh_rb_redir(b, OSH_R_OUT, 1, P("o2"), 0);
    CHECK(go(&t, b, &r) == 0, "< file > file");
    o = slurp(P("o2"));
    CHECK(strcmp(o, "hello\n") == 0, "< content '%s'", o);
    free(o);
    /* >> appends, > truncates */
    b = nb(0, 0);
    helper(b, "echo", "more", NULL);
    osh_rb_redir(b, OSH_R_APPEND, 1, P("o1"), 0);
    go(&t, b, &r);
    o = slurp(P("o1"));
    CHECK(strcmp(o, "hello\nmore\n") == 0, ">> content '%s'", o);
    free(o);
    b = nb(0, 0);
    helper(b, "echo", "new", NULL);
    osh_rb_redir(b, OSH_R_OUT, 1, P("o1"), 0);
    go(&t, b, &r);
    o = slurp(P("o1"));
    CHECK(strcmp(o, "new\n") == 0, "> truncates '%s'", o);
    free(o);
    /* N> and N>> on stderr */
    b = nb(0, 0);
    helper(b, "both", NULL);
    osh_rb_redir(b, OSH_R_OUT, 2, P("e1"), 0);
    go(&t, b, &r);
    o = slurp(P("e1"));
    CHECK(strcmp(o, "err\n") == 0, "2> content '%s'", o);
    free(o);
    b = nb(0, 0);
    helper(b, "both", NULL);
    osh_rb_redir(b, OSH_R_APPEND, 2, P("e1"), 0);
    go(&t, b, &r);
    o = slurp(P("e1"));
    CHECK(strcmp(o, "err\nerr\n") == 0, "2>> content '%s'", o);
    free(o);

    /* order: `>o 2>&1` sends both to the file; `2>&1 >o` sends stderr to the OLD stdout (the capture), stdout to file */
    t_clear(&t);
    b = nb(0, 0);
    helper(b, "both", NULL);
    osh_rb_redir(b, OSH_R_OUT, 1, P("o3"), 0);
    osh_rb_redir(b, OSH_R_DUP, 2, NULL, 1);
    CHECK(go(&t, b, &r) == 0, ">o 2>&1");
    o = slurp(P("o3"));
    CHECK(strcmp(o, "out\nerr\n") == 0, ">o 2>&1: file '%s'", o);
    free(o);
    o = t_out(&t);
    CHECK(o[0] == 0, ">o 2>&1: capture empty, got '%s'", o);
    free(o);
    t_clear(&t);
    b = nb(0, 0);
    helper(b, "both", NULL);
    osh_rb_redir(b, OSH_R_DUP, 2, NULL, 1);
    osh_rb_redir(b, OSH_R_OUT, 1, P("o4"), 0);
    CHECK(go(&t, b, &r) == 0, "2>&1 >o");
    o = slurp(P("o4"));
    CHECK(strcmp(o, "out\n") == 0, "2>&1 >o: file has only stdout '%s'", o);
    free(o);
    o = t_out(&t);
    CHECK(strcmp(o, "err\n") == 0, "2>&1 >o: stderr went to the old stdout '%s'", o);
    free(o);
    /* N>&M with N != 1: 0<&... and 1>&2 */
    t_clear(&t);
    b = nb(0, 0);
    helper(b, "echo", "to-stderr", NULL);
    osh_rb_redir(b, OSH_R_DUP, 1, NULL, 2);
    go(&t, b, &r);
    o = t_err(&t);
    CHECK(strcmp(o, "to-stderr\n") == 0, "1>&2 '%s'", o);
    free(o);

    /* failed redirection: diagnostic, status 1, command not run */
    unlink(P("marker"));
    t_clear(&t);
    b = nb(0, 0);
    helper(b, "touch", P("marker"), NULL);
    osh_rb_redir(b, OSH_R_IN, 0, P("no-such-input"), 0);
    CHECK(go(&t, b, &r) == 1, "failed < gives 1, got %d", r.status);
    CHECK(access(P("marker"), F_OK) != 0, "command did not run after failed redirection");
    o = t_err(&t);
    CHECK(strstr(o, "no-such-input") != NULL, "diagnostic names target: '%s'", o);
    free(o);
    b = nb(0, 0);
    helper(b, "touch", P("marker"), NULL);
    osh_rb_redir(b, OSH_R_OUT, 1, P("no-dir/x"), 0);
    CHECK(go(&t, b, &r) == 1 && access(P("marker"), F_OK) != 0, "failed > into missing dir");
    /* later redirection fails after an earlier one succeeded: earlier file created, no leak, command not run */
    int fds0 = count_fds();
    b = nb(0, 0);
    helper(b, "touch", P("marker"), NULL);
    osh_rb_redir(b, OSH_R_OUT, 2, P("partial"), 0);
    osh_rb_redir(b, OSH_R_IN, 0, P("no-such-input"), 0);
    CHECK(go(&t, b, &r) == 1 && access(P("marker"), F_OK) != 0, "second redirection fails");
    CHECK(count_fds() == fds0, "no descriptors leaked in the parent after failed redirections (%d vs %d)", count_fds(), fds0);
    /* failure inside a pipeline: the failed command's status is 1; neighbours still finish (reader sees EOF) */
    t_clear(&t);
    b = nb(0, 0);
    helper(b, "echo", "data", NULL);
    helper(b, "cat", NULL);
    osh_rb_redir(b, OSH_R_IN, 0, P("no-such-input"), 0);
    CHECK(go(&t, b, &r) == 1 && r.cmd[1].status == 1, "pipeline with failed redirect on last command");
    /* builtin with redirection: applied then restored in the parent; the shell's own fds are untouched */
    t_clear(&t);
    b = nb(0, 0);
    cmd(b, OSH_B_PWD, "pwd", NULL);
    osh_rb_redir(b, OSH_R_OUT, 1, P("pwdout"), 0);
    CHECK(go(&t, b, &r) == 0, "pwd > file");
    o = slurp(P("pwdout"));
    CHECK(o[0] == '/', "builtin output went to file '%s'", o);
    free(o);
    o = t_out(&t);
    CHECK(o[0] == 0, "and not to the shell's stdout, got '%s'", o);
    free(o);
    b = nb(0, 0);
    cmd(b, OSH_B_PWD, "pwd", NULL);
    CHECK(go(&t, b, &r) == 0, "pwd again");
    o = t_out(&t);
    CHECK(o[0] == '/', "stdout restored for next builtin '%s'", o);
    free(o);
    b = nb(0, 0);
    cmd(b, OSH_B_PWD, "pwd", NULL);
    osh_rb_redir(b, OSH_R_IN, 0, P("no-such-input"), 0);
    CHECK(go(&t, b, &r) == 1, "builtin with failed redirection -> 1");
    /* redirection-only command */
    b = nb(0, 0);
    osh_rb_cmd(b, 0);
    osh_rb_redir(b, OSH_R_OUT, 1, P("created-by-redir-only"), 0);
    CHECK(go(&t, b, &r) == 0 && access(P("created-by-redir-only"), F_OK) == 0, "> f alone creates the file");
    t_end(&t);
}

static void test_pipelines(void)
{
    T t;
    t_begin(&t, "/usr/bin:/bin");
    OshResult r;
    OshBuilder *b;
    char *o;

    /* pipeline of 3, status = last */
    b = nb(0, 0);
    helper(b, "gen", "100000", NULL);
    helper(b, "cat", NULL);
    helper(b, "sum", NULL);
    CHECK(go(&t, b, &r) == 0 && r.ncmds == 3, "gen | cat | sum");
    unsigned char *buf = malloc(100000);
    for (unsigned long i = 0; i < 100000; i++) buf[i] = gen_byte(i);
    char want[64];
    snprintf(want, sizeof want, "100000 %08x\n", fnv(2166136261u, buf, 100000));
    free(buf);
    o = t_out(&t);
    CHECK(strcmp(o, want) == 0, "3-stage checksum '%s' want '%s'", o, want);
    free(o);
    /* status is the last command's, not the first's */
    b = nb(0, 0);
    helper(b, "exit", "5", NULL);
    helper(b, "exit", "0", NULL);
    CHECK(go(&t, b, &r) == 0 && r.cmd[0].status == 5, "status is last command");
    b = nb(0, 0);
    helper(b, "exit", "0", NULL);
    helper(b, "exit", "3", NULL);
    CHECK(go(&t, b, &r) == 3, "last failing");

    /* yes | head -1 terminates within 2 s (SIGPIPE default in children) */
    t_clear(&t);
    b = nb(0, 0);
    cmd(b, 0, "yes", NULL);
    cmd(b, 0, "head", "-1", NULL);
    double t0 = now();
    int st = go(&t, b, &r);
    double dt = now() - t0;
    o = t_out(&t);
    CHECK(st == 0 && strcmp(o, "y\n") == 0 && dt < 2.0, "yes | head -1: status %d out '%s' %.3fs", st, o, dt);
    CHECK(r.cmd[0].termsig == SIGPIPE && r.cmd[0].status == 128 + SIGPIPE, "writer died of SIGPIPE (sig %d)", r.cmd[0].termsig);
    free(o);

    /* 10 MB with backpressure */
    t_clear(&t);
    b = nb(0, 0);
    helper(b, "gen", "10485760", NULL);
    helper(b, "cat", NULL);
    helper(b, "cat", NULL);
    helper(b, "sum", NULL);
    t0 = now();
    st = go(&t, b, &r);
    dt = now() - t0;
    buf = malloc(10485760);
    for (unsigned long i = 0; i < 10485760; i++) buf[i] = gen_byte(i);
    snprintf(want, sizeof want, "10485760 %08x\n", fnv(2166136261u, buf, 10485760));
    free(buf);
    o = t_out(&t);
    CHECK(st == 0 && strcmp(o, want) == 0, "10 MB through 4 stages: '%s' want '%s' (%.2fs)", o, want, dt);
    free(o);

    /* signal status 128+n and abort */
    b = nb(0, 0);
    helper(b, "kill", "15", NULL);
    CHECK(go(&t, b, &r) == 128 + 15 && r.cmd[0].termsig == 15, "SIGTERM -> 143, got %d", r.status);
    b = nb(0, 0);
    helper(b, "kill", "9", NULL);
    CHECK(go(&t, b, &r) == 137, "SIGKILL -> 137");
    b = nb(0, 0);
    helper(b, "kill", "6", NULL);
    CHECK(go(&t, b, &r) == 134, "SIGABRT -> 134");
    /* SIGINT death: killed_by_int stops list execution */
    b = nb(0, 0);
    helper(b, "kill", "2", NULL);
    CHECK(go(&t, b, &r) == 130 && r.killed_by_int, "SIGINT -> 130 and killed_by_int");
    /* a stopped child (SIGTSTP) is ended, not parked: no job control */
    b = nb(0, 0);
    helper(b, "kill", "20", NULL);
    st = go(&t, b, &r);
    CHECK(st == 128 + 20 && r.cmd[0].outcome == OSH_OUT_CANCELLED && r.err == OSH_E_INTERRUPTED && waitpid(r.cmd[0].pid, NULL, WNOHANG) < 0,
          "SIGTSTP child cancelled and reaped: status %d outcome %d", st, r.cmd[0].outcome);

    t_end(&t);
}

static void raise_int(void *ctx) { (void)ctx; kill(getpid(), SIGINT); }

static void test_partial_launch(void)
{
    T t;
    t_begin(&t, "/usr/bin:/bin");
    OshResult r;
    OshBuilder *b;
    char *o;
    t.s.fd[0] = open("/dev/null", O_RDONLY | O_CLOEXEC);

    /* 2nd command not found: others still run and are reaped, 127 for it, status = last */
    unlink(P("m1"));
    b = nb(0, 0);
    helper(b, "touch", P("m1"), NULL);
    cmd(b, 0, "no-such-command-xyz", NULL);
    int st = go(&t, b, &r);
    CHECK(st == 127 && r.cmd[1].status == 127 && r.cmd[0].status == 0 && access(P("m1"), F_OK) == 0, "partial: status %d", st);
    CHECK(waitpid(r.cmd[0].pid, NULL, WNOHANG) < 0 && errno == ECHILD, "first command reaped");
    /* not-found in the middle: the writer gets EOF/SIGPIPE, the reader gets EOF */
    t_clear(&t);
    b = nb(0, 0);
    helper(b, "gen", "1000000", NULL);
    cmd(b, 0, "no-such-command-xyz", NULL);
    helper(b, "sum", NULL);
    st = go(&t, b, &r);
    o = t_out(&t);
    CHECK(st == 0 && strcmp(o, "0 811c9dc5\n") == 0 && r.cmd[1].status == 127 && r.cmd[0].termsig == SIGPIPE, "middle not found: '%s' st %d", o, st);
    free(o);
    /* not found as the first command, last still runs with EOF input */
    t_clear(&t);
    b = nb(0, 0);
    cmd(b, 0, "no-such-command-xyz", NULL);
    helper(b, "sum", NULL);
    st = go(&t, b, &r);
    o = t_out(&t);
    CHECK(st == 0 && strcmp(o, "0 811c9dc5\n") == 0, "first not found '%s'", o);
    free(o);

    /* real launch failure (fork fails at the 2nd command): PARTIAL_LAUNCH, started ones cancelled and reaped */
    int fds0 = count_fds();
    b = nb(0, 0);
    helper(b, "sleep", "30000", NULL);
    helper(b, "sleep", "30000", NULL);
    helper(b, "sleep", "30000", NULL);
    t.s.fail_fork_at = 2;
    double t0 = now();
    st = go(&t, b, &r);
    double dt = now() - t0;
    t.s.fail_fork_at = 0;
    CHECK(r.err == OSH_E_PARTIAL_LAUNCH && st == 1 && dt < 5.0, "partial launch err %d status %d in %.2fs", r.err, st, dt);
    CHECK(r.cmd[0].outcome == OSH_OUT_CANCELLED && r.cmd[1].outcome == OSH_OUT_NOT_STARTED && r.cmd[2].outcome == OSH_OUT_NOT_STARTED,
          "outcomes %d %d %d", r.cmd[0].outcome, r.cmd[1].outcome, r.cmd[2].outcome);
    CHECK(r.cmd[0].pid > 0 && kill(r.cmd[0].pid, 0) < 0 && errno == ESRCH, "started command is gone (reaped, no zombie)");
    CHECK(count_fds() == fds0, "no descriptors left in the parent (%d vs %d)", count_fds(), fds0);
    close(t.s.fd[0]);
    t_end(&t);
}

static void test_fd_leaks(void)
{
    T t;
    t_begin(&t, "/usr/bin:/bin");
    OshResult r;
    OshBuilder *b;
    char *o;
    t.s.fd[0] = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int fds0 = count_fds();

    /* 4-stage pipeline where every stage lists its descriptors: only 0,1,2 may exist */
    b = nb(0, 0);
    helper(b, "echo", "x", NULL);
    helper(b, "fdlist", NULL);
    osh_rb_redir(b, OSH_R_OUT, 1, P("fd1"), 0);
    helper(b, "fdlist", NULL);
    osh_rb_redir(b, OSH_R_OUT, 1, P("fd2"), 0);
    helper(b, "fdlist", NULL);
    CHECK(go(&t, b, &r) == 0, "fdlist pipeline");
    o = t_out(&t);
    CHECK(strcmp(o, "fdcount 0\n") == 0, "last stage fd list: '%s'", o);
    free(o);
    o = slurp(P("fd1"));
    CHECK(strcmp(o, "fdcount 0\n") == 0, "stage 2 (with > redirect) fd list: '%s'", o);
    free(o);
    o = slurp(P("fd2"));
    CHECK(strcmp(o, "fdcount 0\n") == 0, "stage 3 fd list: '%s'", o);
    free(o);
    /* single command with every redirection kind */
    t_clear(&t);
    b = nb(0, 0);
    helper(b, "fdlist", NULL);
    osh_rb_redir(b, OSH_R_IN, 0, P("fd1"), 0);
    osh_rb_redir(b, OSH_R_APPEND, 2, P("fd3"), 0);
    osh_rb_redir(b, OSH_R_DUP, 2, NULL, 1);
    CHECK(go(&t, b, &r) == 0, "fdlist with redirs");
    o = t_out(&t);
    CHECK(strcmp(o, "fdcount 0\n") == 0, "redirected single command fd list: '%s'", o);
    free(o);
    /* builtin in a pipeline child also sheds descriptors: builtin cannot show them, but exec'd neighbour proves pipes */
    CHECK(count_fds() == fds0, "parent descriptor count unchanged after all runs (%d vs %d)", count_fds(), fds0);
    /* parent builtin with redirects leaves nothing behind either */
    b = nb(0, 0);
    cmd(b, OSH_B_PWD, "pwd", NULL);
    osh_rb_redir(b, OSH_R_OUT, 1, P("fd4"), 0);
    osh_rb_redir(b, OSH_R_DUP, 2, NULL, 1);
    go(&t, b, &r);
    CHECK(count_fds() == fds0, "parent builtin redirection closes its descriptors (%d vs %d)", count_fds(), fds0);
    close(t.s.fd[0]);
    t_end(&t);
}

static void test_connectors(void)
{
    T t;
    t_begin(&t, "/usr/bin:/bin");
    OshRequest *rq[4];
    static OshBuilder bs[4];
    OshResult res[4];
    const int conns[4] = {OSH_CONN_AND, OSH_CONN_OR, OSH_CONN_SEMI, OSH_CONN_NONE};
    const char *marks[4] = {"c0", "c1", "c2", "c3"};

    /* false && X ; ... X is skipped; `|| Y` then runs because $? is still 1; ; Z always runs */
    for (int i = 0; i < 4; i++) {
        osh_rb_init(&bs[i], 0, conns[i]);
        if (i == 0) helper(&bs[i], "exit", "1", NULL);
        else helper(&bs[i], "touch", P(marks[i]), NULL);
        osh_rb_seal(&bs[i]);
        rq[i] = malloc(sizeof(OshRequest));
        CHECK(osh_req_decode(bs[i].rec, osh_rb_cells(&bs[i]), bs[i].out, bs[i].out_used, rq[i]) == 0, "decode");
        unlink(P(marks[i]));
    }
    /* list: false && c1 || c2 ; c3 */
    int last = osh_run_list(&t.s, (const OshRequest *const *)rq, 4, res);
    CHECK(access(P("c1"), F_OK) != 0, "&& after failure skipped");
    CHECK(access(P("c2"), F_OK) == 0, "|| after skipped && still sees $?=1 and runs");
    CHECK(access(P("c3"), F_OK) == 0, "; always runs");
    CHECK(last == 0, "final $? = %d", last);
    for (int i = 0; i < 4; i++) free(rq[i]);

    /* $? tracking: true || X skipped; true && X runs */
    int cs[3] = {OSH_CONN_OR, OSH_CONN_AND, OSH_CONN_NONE};
    OshRequest *q[3];
    static OshBuilder b3[3];
    for (int i = 0; i < 3; i++) {
        osh_rb_init(&b3[i], 0, cs[i]);
        if (i == 0) helper(&b3[i], "exit", "0", NULL);
        else helper(&b3[i], "touch", P(i == 1 ? "k1" : "k2"), NULL);
        osh_rb_seal(&b3[i]);
        q[i] = malloc(sizeof(OshRequest));
        osh_req_decode(b3[i].rec, osh_rb_cells(&b3[i]), b3[i].out, b3[i].out_used, q[i]);
    }
    unlink(P("k1")); unlink(P("k2"));
    osh_run_list(&t.s, (const OshRequest *const *)q, 3, res);
    CHECK(access(P("k1"), F_OK) != 0 && access(P("k2"), F_OK) == 0, "true || X skipped, then && runs");
    for (int i = 0; i < 3; i++) free(q[i]);
    CHECK(osh_conn_should_run(OSH_CONN_AND, 0) && !osh_conn_should_run(OSH_CONN_AND, 1) && !osh_conn_should_run(OSH_CONN_OR, 0) &&
              osh_conn_should_run(OSH_CONN_OR, 3) && osh_conn_should_run(OSH_CONN_SEMI, 9) && osh_conn_should_run(OSH_CONN_NONE, 9),
          "connector truth table");

    /* exit in a list stops it */
    OshRequest *e[2];
    static OshBuilder be[2];
    for (int i = 0; i < 2; i++) {
        osh_rb_init(&be[i], 0, i == 0 ? OSH_CONN_SEMI : 0);
        if (i == 0) cmd(&be[i], OSH_B_EXIT, "exit", "4", NULL);
        else helper(&be[i], "touch", P("after-exit"), NULL);
        osh_rb_seal(&be[i]);
        e[i] = malloc(sizeof(OshRequest));
        osh_req_decode(be[i].rec, osh_rb_cells(&be[i]), be[i].out, be[i].out_used, e[i]);
    }
    unlink(P("after-exit"));
    int st = osh_run_list(&t.s, (const OshRequest *const *)e, 2, res);
    CHECK(st == 4 && access(P("after-exit"), F_OK) != 0, "exit 4 ends the list (st %d)", st);
    free(e[0]); free(e[1]);
    t_end(&t);
}

static void test_refusals(void)
{
    T t;
    t_begin(&t, "/usr/bin:/bin");
    OshBuilder *b;
    OshRequest *rq = malloc(sizeof *rq);

    b = nb(0, 0);
    helper(b, "echo", "x", NULL);
    osh_rb_seal(b);
    size_t nc = osh_rb_cells(b);
    CHECK(osh_req_decode(b->rec, nc, b->out, b->out_used, rq) == 0, "baseline decodes");
    uint64_t save;
#define MUT(idx, val, want, what) do { save = b->rec[idx]; b->rec[idx] = (val); \
    int rc_ = osh_req_decode(b->rec, nc, b->out, b->out_used, rq); CHECK(rc_ == (want), what ": got %d want %d", rc_, (want)); \
    OshResult rr_; int st_ = osh_exec_record(&t.s, b->rec, nc, b->out, b->out_used, &rr_); CHECK(st_ == 2 && rr_.refusal == (want), what " not run"); \
    b->rec[idx] = save; } while (0)
    MUT(0, 0x4F524552, OSH_ABI_MAGIC, "bad magic");
    MUT(5, 2, OSH_ABI_VERSION, "version 2");
    MUT(5, 0, OSH_ABI_VERSION, "version 0");
    MUT(5, 0x100000001ull, OSH_ABI_VERSION, "version high bits");
    MUT(6, 1, OSH_ABI_RESERVED, "reserved cell 6");
    MUT(7, 1, OSH_ABI_RESERVED, "reserved cell 7");
    MUT(3, 2, OSH_ABI_RESERVED, "unknown flag bit");
    MUT(1, 0, OSH_REQ_FIELD, "ncmds 0");
    MUT(1, 9, OSH_REQ_FIELD, "ncmds 9");
    MUT(1, 2, OSH_ABI_LENGTH, "ncmds beyond supplied cells");
    MUT(2, 4, OSH_REQ_FIELD, "bad connector");
    MUT(4, 9000, OSH_REQ_FIELD, "out_used over cap");
    MUT(4, 1, OSH_REQ_FIELD, "out_used smaller than the offsets used");
#undef MUT
    CHECK(osh_req_decode(b->rec, 3, b->out, b->out_used, rq) == OSH_ABI_LENGTH, "header too short");
    CHECK(osh_req_decode(NULL, 0, NULL, 0, rq) == OSH_ABI_LENGTH, "NULL record");
    /* field-level malformations */
    uint64_t c0 = OSH_REQ_HDR_CELLS;
    save = b->rec[c0 + 0]; b->rec[c0 + 0] = 33;
    CHECK(osh_req_decode(b->rec, nc, b->out, b->out_used, rq) == OSH_REQ_FIELD, "nargv 33");
    b->rec[c0 + 0] = save;
    save = b->rec[c0 + 3]; b->rec[c0 + 3] = 7;
    CHECK(osh_req_decode(b->rec, nc, b->out, b->out_used, rq) == OSH_REQ_FIELD, "unknown builtin id");
    b->rec[c0 + 3] = save;
    save = b->rec[c0 + 4]; b->rec[c0 + 4] = 1000000;
    CHECK(osh_req_decode(b->rec, nc, b->out, b->out_used, rq) == OSH_REQ_FIELD, "argv offset out of range");
    b->rec[c0 + 4] = save;
    save = b->rec[c0 + 5]; b->rec[c0 + 5] = 1000000;
    CHECK(osh_req_decode(b->rec, nc, b->out, b->out_used, rq) == OSH_REQ_FIELD, "argv length out of range");
    b->rec[c0 + 5] = save;
    save = b->rec[c0 + 4 + 2 * 5]; b->rec[c0 + 4 + 2 * 5] = 1; /* entry beyond nargv must be zero */
    CHECK(osh_req_decode(b->rec, nc, b->out, b->out_used, rq) == OSH_ABI_RESERVED, "stray entry beyond nargv");
    b->rec[c0 + 4 + 2 * 5] = save;
    uint64_t sv = b->out[0]; b->out[0] = 0; /* NUL inside an argument */
    CHECK(osh_req_decode(b->rec, nc, b->out, b->out_used, rq) == OSH_REQ_FIELD, "NUL byte in argument");
    b->out[0] = sv;
    sv = b->out[0]; b->out[0] = 256; /* a cell is a byte */
    CHECK(osh_req_decode(b->rec, nc, b->out, b->out_used, rq) == OSH_REQ_FIELD, "cell above 255");
    b->out[0] = sv;
    CHECK(osh_req_decode(b->rec, nc, b->out, b->out_used, rq) == 0, "restored record decodes");
    /* redirection fields */
    b = nb(0, 0);
    helper(b, "echo", "x", NULL);
    osh_rb_redir(b, OSH_R_DUP, 2, NULL, 1);
    osh_rb_seal(b);
    nc = osh_rb_cells(b);
    uint64_t rbase = c0 + 4 + 64 + 64;
    CHECK(osh_req_decode(b->rec, nc, b->out, b->out_used, rq) == 0, "dup redir decodes");
    b->rec[rbase + 2] = 3;
    CHECK(osh_req_decode(b->rec, nc, b->out, b->out_used, rq) == OSH_REQ_FIELD, "N>&3 refused (M in 0..2)");
    b->rec[rbase + 2] = 1; b->rec[rbase + 1] = 3;
    CHECK(osh_req_decode(b->rec, nc, b->out, b->out_used, rq) == OSH_REQ_FIELD, "fd 3 refused");
    b->rec[rbase + 1] = 2; b->rec[rbase + 0] = 9;
    CHECK(osh_req_decode(b->rec, nc, b->out, b->out_used, rq) == OSH_REQ_FIELD, "redir kind 9");
    b->rec[rbase + 0] = 4; b->rec[rbase + 3] = 1;
    CHECK(osh_req_decode(b->rec, nc, b->out, b->out_used, rq) == OSH_REQ_FIELD, "dup with nonzero length");
    /* direct records may not carry builtins */
    b = nb(OSH_REQ_FLAG_DIRECT, 0);
    cmd(b, OSH_B_PWD, "pwd", NULL);
    osh_rb_seal(b);
    CHECK(osh_req_decode(b->rec, osh_rb_cells(b), b->out, b->out_used, rq) == OSH_REQ_FIELD, "DIRECT with builtin_id refused");
    /* the wire-form check on a request with a runnable command produced nothing */
    free(rq);
    t_end(&t);
}

static int cls_hook(void *ctx, const char *name)
{
    (void)ctx;
    if (!strcmp(name, "native-op")) return OSH_CLASS_OMEGA_NATIVE_OP;
    if (!strcmp(name, "artifact")) return OSH_CLASS_AIENOS_ARTIFACT;
    if (!strcmp(name, "service")) return OSH_CLASS_AIENOS_SERVICE;
    return OSH_CLASS_LINUX;
}

static int perm_hook(void *ctx, const OshBinding *b, int op)
{
    (void)b;
    return *(int *)ctx == op ? OSH_E_REVOKED : OSH_E_OK;
}

static void test_destinations_and_authority(void)
{
    T t;
    t_begin(&t, "/usr/bin:/bin");
    OshResult r;
    OshBuilder *b;
    t.s.class_hook = cls_hook;
    const char *names[3] = {"native-op", "artifact", "service"};
    for (int i = 0; i < 3; i++) {
        unlink(P("m2"));
        b = nb(0, 0);
        helper(b, "touch", P("m2"), NULL); /* the first command would run if the pipeline were not refused as a whole */
        cmd(b, 0, names[i], NULL);
        CHECK(go(&t, b, &r) == 126 && r.err == OSH_E_UNAVAILABLE && r.cmd[0].outcome == OSH_OUT_FAILED_NO_EFFECT, "%s refused (st %d err %d)", names[i], r.status, r.err);
        CHECK(access(P("m2"), F_OK) != 0, "%s: nothing in the pipeline ran, never forwarded", names[i]);
        CHECK(r.cmd[0].pid == 0 && r.cmd[1].pid == 0, "no process started");
    }
    /* even if a Linux file of that name exists on PATH, the class hook wins: no fallback between classes */
    wfile(P("artifact"), "#!/bin/true\n", 0755);
    osh_var_set(&t.s, "PATH", g_tmp);
    b = nb(0, 0);
    cmd(b, 0, "artifact", NULL);
    CHECK(go(&t, b, &r) == 126 && r.err == OSH_E_UNAVAILABLE, "no fallback to a same-named Linux executable");
    t.s.class_hook = NULL;

    /* authority: no binding, bad domain, narrowing, hook denial */
    osh_var_set(&t.s, "PATH", "/usr/bin:/bin");
    t.s.binding.valid = 0;
    unlink(P("m3"));
    b = nb(0, 0);
    helper(b, "touch", P("m3"), NULL);
    CHECK(go(&t, b, &r) == 126 && r.err == OSH_E_DENIED && access(P("m3"), F_OK) != 0, "no binding -> DENIED, nothing runs");
    t.s.binding.valid = 1;
    t.s.binding.domain = 3;
    b = nb(0, 0);
    helper(b, "touch", P("m3"), NULL);
    CHECK(go(&t, b, &r) == 126 && r.err == OSH_E_CAP_DOMAIN_MISMATCH, "domain mismatch");
    t.s.binding.domain = 1;
    t.s.binding.cap_generation = 0x100000000ull;
    b = nb(0, 0);
    helper(b, "touch", P("m3"), NULL);
    CHECK(go(&t, b, &r) == 126 && r.err == OSH_E_CAP_GEN_NARROW, "domain-1 generation above 32 bits");
    t.s.binding.domain = 2;
    int deny = OSH_OP_OPEN_WRITE;
    t.s.perm_hook = perm_hook;
    t.s.hook_ctx = &deny;
    b = nb(0, 0);
    helper(b, "touch", P("m3"), NULL);
    CHECK(go(&t, b, &r) == 0 && access(P("m3"), F_OK) == 0, "spawn allowed when only writes are revoked");
    b = nb(0, 0);
    helper(b, "echo", "x", NULL);
    osh_rb_redir(b, OSH_R_OUT, 1, P("denied-out"), 0);
    CHECK(go(&t, b, &r) == 126 && r.err == OSH_E_REVOKED && access(P("denied-out"), F_OK) != 0, "revoked write: effect does not happen");
    deny = OSH_OP_SPAWN;
    b = nb(0, 0);
    helper(b, "touch", P("m4"), NULL);
    CHECK(go(&t, b, &r) == 126 && r.err == OSH_E_REVOKED && access(P("m4"), F_OK) != 0, "revoked spawn");
    t_end(&t);
}

static void test_direct_equals_record(void)
{
    T t;
    t_begin(&t, "/usr/bin:/bin");
    OshResult r1, r2;

    /* direct-argv submit builds the same record: same decoded content, same behaviour */
    const char *argv[] = {g_self, "--helper", "both"};
    OshRedir rd[2] = {{.kind = OSH_R_OUT, .fd = 1, .path = P("dout")}, {.kind = OSH_R_DUP, .fd = 2, .src_fd = 1}};
    OshAssign ov[1] = {{"DIRECT_VAR", "v"}};
    int s1 = osh_submit_argv(&t.s, argv, 3, ov, 1, rd, 2, &r1);
    char *o1 = slurp(P("dout"));

    OshBuilder *b = nb(0, 0);
    helper(b, "both", NULL);
    osh_rb_assign(b, "DIRECT_VAR", "v");
    osh_rb_redir(b, OSH_R_OUT, 1, P("rout"), 0);
    osh_rb_redir(b, OSH_R_DUP, 2, NULL, 1);
    int s2 = go(&t, b, &r2);
    char *o2 = slurp(P("rout"));
    CHECK(s1 == s2 && s1 == 0 && strcmp(o1, o2) == 0 && strcmp(o1, "out\nerr\n") == 0, "direct vs record: %d/%d '%s' '%s'", s1, s2, o1, o2);
    free(o1); free(o2);

    /* identical decoded structs (apart from the flag) */
    OshBuilder bd, br;
    osh_rb_init(&bd, OSH_REQ_FLAG_DIRECT, 0);
    osh_rb_cmd(&bd, 0);
    osh_rb_arg(&bd, "a"); osh_rb_arg(&bd, ""); osh_rb_arg(&bd, "c d");
    osh_rb_assign(&bd, "K", "v");
    osh_rb_redir(&bd, OSH_R_APPEND, 1, "f", 0);
    osh_rb_seal(&bd);
    osh_rb_init(&br, 0, 0);
    osh_rb_cmd(&br, 0);
    osh_rb_arg(&br, "a"); osh_rb_arg(&br, ""); osh_rb_arg(&br, "c d");
    osh_rb_assign(&br, "K", "v");
    osh_rb_redir(&br, OSH_R_APPEND, 1, "f", 0);
    osh_rb_seal(&br);
    OshRequest *a = malloc(sizeof *a), *c = malloc(sizeof *c);
    CHECK(osh_req_decode(bd.rec, osh_rb_cells(&bd), bd.out, bd.out_used, a) == 0 && osh_req_decode(br.rec, osh_rb_cells(&br), br.out, br.out_used, c) == 0, "decode both");
    CHECK(a->cmd[0].nargv == 3 && c->cmd[0].nargv == 3 && strcmp(a->cmd[0].argv[2], c->cmd[0].argv[2]) == 0 && a->cmd[0].argv[1][0] == 0 &&
              a->cmd[0].nassign == 1 && a->cmd[0].redir[0].kind == c->cmd[0].redir[0].kind && a->flags == 1 && c->flags == 0,
          "decoded content equal (empty argument preserved)");
    free(a); free(c);

    /* the direct path obeys the same resolution, refusals and authority checks */
    const char *bad[] = {"no-such-command-xyz"};
    CHECK(osh_submit_argv(&t.s, bad, 1, NULL, 0, NULL, 0, &r1) == 127, "direct: PATH miss 127");
    const char *pwdv[] = {"pwd"};
    CHECK(osh_submit_argv(&t.s, pwdv, 1, NULL, 0, NULL, 0, &r1) == 0, "direct: 'pwd' is an external /usr/bin/pwd, not the builtin");
    CHECK(r1.cmd[0].pid > 0, "direct: a process was started for pwd (builtin_id must be 0)");
    t.s.binding.valid = 0;
    CHECK(osh_submit_argv(&t.s, argv, 3, NULL, 0, NULL, 0, &r1) == 126 && r1.err == OSH_E_DENIED, "direct path honours the binding check");
    t.s.binding.valid = 1;
    CHECK(osh_submit_argv(&t.s, argv, 0, NULL, 0, NULL, 0, &r1) == 0, "direct: empty argv with no assignments is a no-op");
    OshAssign badname[1] = {{"1x", "v"}};
    CHECK(osh_submit_argv(&t.s, argv, 3, badname, 1, NULL, 0, &r1) == 2 && r1.refusal == OSH_REQ_FIELD, "direct: bad assignment name refused as REQ_FIELD");
    const char *many[33];
    for (int i = 0; i < 33; i++) many[i] = "x";
    CHECK(osh_submit_argv(&t.s, many, 33, NULL, 0, NULL, 0, &r1) == 2 && r1.refusal == OSH_REQ_FIELD, "direct: 33 args refused");
    OshRedir badr[1] = {{.kind = OSH_R_DUP, .fd = 2, .src_fd = 5}};
    CHECK(osh_submit_argv(&t.s, argv, 3, NULL, 0, badr, 1, &r1) == 2 && r1.refusal == OSH_REQ_FIELD, "direct: N>&5 refused");
    t_end(&t);
}

static void test_session_env(void)
{
    OshSession s;
    char *env[] = {"A=1", "B=two=2", "=bad", "NOEQ", "9x=1", "C=", NULL};
    CHECK(osh_session_init(&s, env) == 0, "init");
    CHECK(strcmp(osh_var_get(&s, "A"), "1") == 0 && strcmp(osh_var_get(&s, "B"), "two=2") == 0 && osh_var_get(&s, "C") && !*osh_var_get(&s, "C"), "imported");
    CHECK(osh_var_get(&s, "NOEQ") == NULL && osh_var_get(&s, "9x") == NULL, "malformed entries ignored");
    CHECK(osh_var_get(&s, "PWD") != NULL, "PWD set");
    OshAssign ov[2] = {{"A", "9"}, {"Z", "z"}};
    char **e = osh_build_envp(&s, ov, 2);
    int a = 0, z = 0, n = 0;
    for (char **p = e; *p; p++) { n++; a += !strcmp(*p, "A=9"); z += !strcmp(*p, "Z=z"); }
    CHECK(a == 1 && z == 1 && n == 5, "fresh envp with override (n=%d)", n);
    osh_envp_free(e);
    CHECK(osh_var_set(&s, "bad name", "x") < 0 && osh_var_export(&s, "", NULL) < 0, "invalid names refused");
    osh_var_set(&s, "U", "u");
    e = osh_build_envp(&s, NULL, 0);
    int found = 0;
    for (char **p = e; *p; p++) found += !strncmp(*p, "U=", 2);
    CHECK(found == 0, "unexported variable not in envp");
    osh_envp_free(e);
    osh_session_free(&s);
}

/* ================= review fixes (PR 337): isolation helper ================= */

/* Results of a test that ran in its own process (so a hang or a signal storm cannot take the suite with it). */
typedef struct {
    int status, err, outcome0, outcome1, outcome2, termsig0, termsig1, sigint_seen, killed_by_int, exit_requested, exit_status;
    int extra[6];
    double dt;
    char text[256];
} Rep;

static int g_only_set;
static const char *g_only;
static int want(const char *name) { return !g_only_set || strstr(name, g_only) != NULL; }

/* Run fn in a forked process (own process group if new_group, with a SIGALRM watchdog). Returns 0 when it reported,
 * -1 when it hung or crashed (then any members of its group are killed). */
static int run_isolated(void (*fn)(Rep *), Rep *out, int new_group, unsigned secs)
{
    int pf[2];
    memset(out, 0, sizeof *out);
    if (pipe(pf)) return -1;
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (new_group) setpgid(0, 0);
        close(pf[0]);
        alarm(secs);
        Rep r;
        memset(&r, 0, sizeof r);
        fn(&r);
        if (write(pf[1], &r, sizeof r) < 0) {}
        _exit(0);
    }
    close(pf[1]);
    if (new_group) setpgid(pid, pid);
    size_t got = 0;
    while (got < sizeof *out) {
        ssize_t n = read(pf[0], (char *)out + got, sizeof *out - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(pf[0]);
    int st;
    waitpid(pid, &st, 0);
    if (new_group) kill(-pid, SIGKILL); /* leftovers of a failed run */
    if (got != sizeof *out) { memset(out, 0, sizeof *out); return -1; }
    return 0;
}

static void rep_fill(Rep *rp, const OshResult *r, int st, double dt)
{
    rp->status = st;
    rp->err = r->err;
    rp->outcome0 = r->cmd[0].outcome;
    rp->outcome1 = r->ncmds > 1 ? r->cmd[1].outcome : 0;
    rp->outcome2 = r->ncmds > 2 ? r->cmd[2].outcome : 0;
    rp->termsig0 = r->cmd[0].termsig;
    rp->termsig1 = r->ncmds > 1 ? r->cmd[1].termsig : 0;
    rp->sigint_seen = r->sigint_seen;
    rp->killed_by_int = r->killed_by_int;
    rp->exit_requested = r->exit_requested;
    rp->dt = dt;
}

static void raise_int_group(void *ctx) { (void)ctx; kill(0, SIGINT); } /* what the terminal does on ^C: the whole group */

/* ---- BUG1: non-interactive session ---- */

static void iso_sigint_group(Rep *rp)
{
    T t;
    t_begin(&t, "/usr/bin:/bin");
    OshResult r;
    OshBuilder *b = nb(0, 0);
    helper(b, "pgid", NULL);
    helper(b, "sleep", "20000", NULL);
    helper(b, "cat", NULL);
    t.s.after_launch_hook = raise_int_group;
    double t0 = now();
    int st = go(&t, b, &r);
    rep_fill(rp, &r, st, now() - t0);
    rp->exit_status = t.s.exit_status;
    rp->exit_requested = t.s.exit_requested;
    rp->termsig1 = r.cmd[1].termsig;
    t.s.after_launch_hook = NULL;
    t_clear(&t);
    b = nb(0, 0);
    helper(b, "pgid", NULL);
    t.s.exit_requested = 0;
    go(&t, b, &r);
    char *o = t_out(&t);
    rp->extra[0] = atoi(o) == (int)getpgrp(); /* child stays in the shell's own process group */
    free(o);
    t_end(&t);
}

static void test_sigint_noninteractive(void)
{
    Rep rp;
    int rc = run_isolated(iso_sigint_group, &rp, 1, 15);
    CHECK(rc == 0, "group SIGINT test hung or crashed");
    CHECK(rc == 0 && rp.dt < 3.0, "pipeline with a terminal-style SIGINT to the group ended in %.2fs (hung until the child finished?)", rp.dt);
    CHECK(rc == 0 && rp.termsig1 == SIGINT && rp.killed_by_int == 1 && rp.sigint_seen == 1,
          "sleeping child died of SIGINT (termsig %d) and the shell saw it (seen %d, killed_by_int %d)", rp.termsig1, rp.sigint_seen, rp.killed_by_int);
    CHECK(rc == 0 && rp.exit_requested == 1 && rp.exit_status == 130,
          "non-interactive shell exits 128+2 after the pipeline whose foreground child died of SIGINT (requested %d status %d)", rp.exit_requested, rp.exit_status);
    CHECK(rc == 0 && rp.extra[0] == 1, "non-interactive children stay in the shell's process group");
}

static void iso_sigint_direct(Rep *rp)
{
    T t;
    t_begin(&t, "/usr/bin:/bin");
    OshResult r;
    OshBuilder *b = nb(0, 0);
    helper(b, "sleep", "400", NULL);
    t.s.after_launch_hook = raise_int;
    double t0 = now();
    int st = go(&t, b, &r);
    rep_fill(rp, &r, st, now() - t0);
    rp->exit_requested = t.s.exit_requested;
    t_end(&t);
}

static void test_sigint_direct_noninteractive(void)
{
    Rep rp;
    int rc = run_isolated(iso_sigint_direct, &rp, 1, 15);
    CHECK(rc == 0 && rp.sigint_seen == 1 && rp.status == 0 && rp.termsig0 == 0 && rp.dt >= 0.3 && !rp.killed_by_int && !rp.exit_requested,
          "kill -INT <shell pid> while waiting (non-interactive): nothing forwarded, child finished (status %d termsig %d dt %.2f exit %d)",
          rp.status, rp.termsig0, rp.dt, rp.exit_requested);
}

static void iso_ign_inherited(Rep *rp)
{
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    T t;
    t_begin(&t, "/usr/bin:/bin");
    OshResult r;
    OshBuilder *b = nb(0, 0);
    helper(b, "sigdisp", NULL);
    int st = go(&t, b, &r);
    char *o = t_out(&t);
    rp->extra[0] = strcmp(o, "INT ign QUIT ign\n") == 0;
    snprintf(rp->text, sizeof rp->text, "%s", o);
    free(o);
    struct sigaction sa;
    sigaction(SIGINT, NULL, &sa);
    rp->extra[1] = sa.sa_handler == SIG_IGN; /* the embedder's own disposition is restored */
    rp->status = st;
    t_end(&t);
}

static void iso_ign_not_inherited(Rep *rp)
{
    T t;
    t_begin(&t, "/usr/bin:/bin");
    OshResult r;
    OshBuilder *b = nb(0, 0);
    helper(b, "sigdisp", NULL);
    go(&t, b, &r);
    char *o = t_out(&t);
    rp->extra[0] = strcmp(o, "INT dfl QUIT dfl\n") == 0;
    snprintf(rp->text, sizeof rp->text, "%s", o);
    free(o);
    t_end(&t);
}

static void test_inherited_ignore(void)
{
    Rep rp;
    int rc = run_isolated(iso_ign_inherited, &rp, 0, 15);
    CHECK(rc == 0 && rp.extra[0] == 1, "SIGINT/SIGQUIT ignored at entry stay ignored in children (POSIX); child saw: %s", rp.text);
    CHECK(rc == 0 && rp.extra[1] == 1, "embedder's ignore disposition restored after the run");
    rc = run_isolated(iso_ign_not_inherited, &rp, 0, 15);
    CHECK(rc == 0 && rp.extra[0] == 1, "children get default SIGINT/SIGQUIT when the shell did not have them ignored; child saw: %s", rp.text);
}

/* ---- BUG3: a stopped member anywhere ends the pipeline ---- */

static void iso_stop_tail(Rep *rp)
{
    T t;
    t_begin(&t, "/usr/bin:/bin");
    OshResult r;
    OshBuilder *b = nb(0, 0);
    cmd(b, 0, "yes", NULL);
    cmd(b, 0, "sh", "-c", "kill -STOP $$", NULL);
    double t0 = now();
    int st = go(&t, b, &r);
    rep_fill(rp, &r, st, now() - t0);
    rp->extra[0] = waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD; /* nothing left unreaped */
    char *e = t_err(&t);
    rp->extra[1] = strstr(e, "stopped") != NULL;
    free(e);
    t_end(&t);
}

static void iso_stop_head(Rep *rp)
{
    T t;
    t_begin(&t, "/usr/bin:/bin");
    OshResult r;
    OshBuilder *b = nb(0, 0);
    helper(b, "kill", "19", NULL);
    helper(b, "sleep", "20000", NULL);
    helper(b, "cat", NULL);
    double t0 = now();
    int st = go(&t, b, &r);
    rep_fill(rp, &r, st, now() - t0);
    rp->extra[0] = waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD;
    t_end(&t);
}

static void iso_stop_middle(Rep *rp)
{
    T t;
    t_begin(&t, "/usr/bin:/bin");
    OshResult r;
    OshBuilder *b = nb(0, 0);
    helper(b, "sleep", "20000", NULL);
    helper(b, "kill", "20", NULL);
    helper(b, "cat", NULL);
    double t0 = now();
    int st = go(&t, b, &r);
    rep_fill(rp, &r, st, now() - t0);
    rp->extra[0] = waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD;
    t_end(&t);
}

static void test_stopped_member(void)
{
    Rep rp;
    int rc = run_isolated(iso_stop_tail, &rp, 1, 10);
    CHECK(rc == 0 && rp.dt < 2.0, "yes | sh -c 'kill -STOP $$' finishes within 2 s (took %.2f, rc %d)", rp.dt, rc);
    CHECK(rc == 0 && rp.status == 128 + SIGSTOP && rp.outcome1 == OSH_OUT_CANCELLED && rp.outcome0 == OSH_OUT_CANCELLED && rp.err == OSH_E_INTERRUPTED,
          "stopped last member: status %d (want 147), outcomes %d/%d, err %d", rp.status, rp.outcome0, rp.outcome1, rp.err);
    CHECK(rc == 0 && rp.extra[0] == 1 && rp.extra[1] == 1, "no child left unreaped (%d) and a diagnostic mentions 'stopped' (%d)", rp.extra[0], rp.extra[1]);
    rc = run_isolated(iso_stop_head, &rp, 1, 10);
    CHECK(rc == 0 && rp.dt < 2.0 && rp.status == 128 + SIGSTOP && rp.extra[0] == 1,
          "stopped FIRST member of a 3-stage pipeline ends it: dt %.2f status %d reaped %d", rp.dt, rp.status, rp.extra[0]);
    rc = run_isolated(iso_stop_middle, &rp, 1, 10);
    CHECK(rc == 0 && rp.dt < 2.0 && rp.status == 128 + SIGTSTP && rp.extra[0] == 1,
          "stopped MIDDLE member (SIGTSTP) ends it: dt %.2f status %d reaped %d", rp.dt, rp.status, rp.extra[0]);
}

/* ---- BUG1: interactive session on a pseudo-terminal ---- */

static void iso_pty_common(Rep *rp, int mode)
{
    /* mode 0: ^C typed on the terminal; 1: kill -INT <shell pid>; 2: stopped member */
    setsid();
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) || unlockpt(master)) { rp->extra[5] = -1; return; }
    char *sn = ptsname(master);
    int slave = sn ? open(sn, O_RDWR) : -1; /* the session leader without a terminal acquires it */
    if (slave < 0) { rp->extra[5] = -1; return; }
    rp->extra[2] = tcgetpgrp(slave) == getpgrp(); /* shell owns the terminal before */
    T t;
    t_begin(&t, "/usr/bin:/bin");
    t.s.interactive = 1;
    t.s.tty_fd = slave;
    OshResult r;
    OshBuilder *b = nb(0, 0);
    if (mode == 2) {
        cmd(b, 0, "yes", NULL);
        cmd(b, 0, "sh", "-c", "kill -STOP $$", NULL);
    } else {
        helper(b, "sleep", "8000", NULL);
        helper(b, "cat", NULL);
    }
    t.s.after_launch_hook = mode == 1 ? raise_int : NULL;
    if (mode == 0) {
        /* the terminal driver sends ^C to the FOREGROUND group: type it once the pipeline owns the terminal */
        pid_t kid = fork();
        if (kid == 0) {
            usleep(400000);
            if (write(master, "\003", 1) < 0) {}
            _exit(0);
        }
        double t0 = now();
        int st = go(&t, b, &r);
        rep_fill(rp, &r, st, now() - t0);
        waitpid(kid, NULL, 0);
    } else {
        double t0 = now();
        int st = go(&t, b, &r);
        rep_fill(rp, &r, st, now() - t0);
    }
    rp->extra[3] = tcgetpgrp(slave) == getpgrp(); /* the shell took the terminal back */
    rp->exit_requested = t.s.exit_requested;
    rp->extra[4] = waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD;
    t_end(&t);
}

static void iso_pty_ctrl_c(Rep *rp) { iso_pty_common(rp, 0); }
static void iso_pty_kill_shell(Rep *rp) { iso_pty_common(rp, 1); }
static void iso_pty_stop(Rep *rp) { iso_pty_common(rp, 2); }

static void iso_pty_pgid(Rep *rp)
{
    setsid();
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) || unlockpt(master)) { rp->extra[5] = -1; return; }
    char *sn = ptsname(master);
    int slave = sn ? open(sn, O_RDWR) : -1;
    if (slave < 0) { rp->extra[5] = -1; return; }
    T t;
    t_begin(&t, "/usr/bin:/bin");
    t.s.interactive = 1;
    t.s.tty_fd = slave;
    OshResult r;
    OshBuilder *b = nb(0, 0);
    helper(b, "pgid", NULL);
    helper(b, "cat", NULL);
    go(&t, b, &r);
    char *o = t_out(&t);
    int pg = atoi(o);
    free(o);
    rp->extra[0] = pg > 0 && pg != (int)getpgrp(); /* its own group, not the shell's */
    /* SIGINT delivered to a child in an interactive session is back to default: even if ignored at entry */
    signal(SIGINT, SIG_IGN);
    t_clear(&t);
    b = nb(0, 0);
    helper(b, "sigdisp", NULL);
    go(&t, b, &r);
    o = t_out(&t);
    rp->extra[1] = strcmp(o, "INT dfl QUIT dfl\n") == 0;
    free(o);
    t_end(&t);
}

static void test_interactive_pty(void)
{
    Rep rp;
    int rc = run_isolated(iso_pty_pgid, &rp, 0, 15);
    if (rc == 0 && rp.extra[5] == -1) { printf("note: no pseudo-terminal available, interactive tests skipped\n"); g_pass++; return; }
    CHECK(rc == 0 && rp.extra[0] == 1, "interactive: pipeline runs in its own process group");
    CHECK(rc == 0 && rp.extra[1] == 1, "interactive: children get default SIGINT/SIGQUIT");
    rc = run_isolated(iso_pty_ctrl_c, &rp, 0, 20);
    CHECK(rc == 0 && rp.extra[2] == 1, "interactive: shell owns the terminal before the pipeline");
    CHECK(rc == 0 && rp.dt < 4.0 && rp.termsig0 == SIGINT && rp.termsig1 == SIGINT && rp.killed_by_int == 1,
          "^C on the terminal reaches the pipeline's group (dt %.2f termsig %d/%d)", rp.dt, rp.termsig0, rp.termsig1);
    CHECK(rc == 0 && rp.sigint_seen == 0 && rp.exit_requested == 0, "^C does not reach the interactive shell (seen %d) and the shell does not exit", rp.sigint_seen);
    CHECK(rc == 0 && rp.extra[3] == 1 && rp.extra[4] == 1, "shell takes the terminal back after the pipeline (%d) and nothing is left unreaped (%d)", rp.extra[3], rp.extra[4]);
    rc = run_isolated(iso_pty_kill_shell, &rp, 0, 20);
    CHECK(rc == 0 && rp.dt < 4.0 && rp.termsig0 == SIGINT && rp.sigint_seen == 1,
          "kill -INT <shell pid> while waiting (interactive): forwarded to the pipeline's group (dt %.2f termsig %d seen %d)", rp.dt, rp.termsig0, rp.sigint_seen);
    CHECK(rc == 0 && rp.extra[3] == 1 && rp.exit_requested == 0, "terminal returned, interactive shell does not exit");
    rc = run_isolated(iso_pty_stop, &rp, 0, 20);
    CHECK(rc == 0 && rp.dt < 2.0 && rp.status == 128 + SIGSTOP && rp.outcome1 == OSH_OUT_CANCELLED && rp.extra[3] == 1 && rp.extra[4] == 1,
          "interactive stopped member: cancelled within 2 s (dt %.2f status %d), terminal returned %d, reaped %d", rp.dt, rp.status, rp.extra[3], rp.extra[4]);
}

/* ================= review fixes (PR 337): printf table and wired adversarial checks ================= */

typedef struct {
    const char *fmt, *a1, *a2, *a3;
} PfRow;

/* SAME: stdout and status equal /usr/bin/printf (its stderr is not compared). */
static const PfRow k_pf_same[] = {
    {"\\%q|\\n", "5", NULL, NULL},          /* review B2: backslash hides % from the old validator */
    {"\\%", "x", NULL, NULL},               /* review B2: % at the very end after a backslash, extra arg follows */
    {"\\%d\\n", "7", NULL, NULL},
    {"\\\\%d\\n", "7", NULL, NULL},
    {"a\\", NULL, NULL, NULL},              /* lone trailing backslash */
    {"\\", "x", NULL, NULL},
    {"\\%s|\\n", "a", NULL, NULL},
    {"%%%%\\n", NULL, NULL, NULL},
    {"%%d\\n", "5", NULL, NULL},
    {"%s%%%s\\n", "a", "b", NULL},
    {"%s\\n", "", NULL, NULL},
    {"%d\\n", "-5", NULL, NULL},
    {"%d\\n", "9223372036854775807", NULL, NULL},
    {"%d\\n", "9223372036854775808", NULL, NULL},
    {"%u\\n", "-1", NULL, NULL},
    {"%x\\n", "-1", NULL, NULL},
    {"%o\\n", "0777", NULL, NULL},
    {"%c|\\n", "", NULL, NULL},
    {"%c|\\n", "hello", NULL, NULL},
    {"%s %s %s\\n", "a", "b", NULL},
    {"%s\\n", "a", "b", "c"},
    {"%d\\n", "1", "2", "3"},
    {"%i\\n", "0x10", NULL, NULL},
    {"%d\\n", "'x", NULL, NULL},
    {"%d\\n", "\"ab", NULL, NULL},
    {"\\101\\102\\n", NULL, NULL, NULL},
    {"\\1012\\n", NULL, NULL, NULL},
    {"\\8\\n", NULL, NULL, NULL},
    {"\\\"q\\\"\\n", NULL, NULL, NULL},
    {"x\\ty\\n", NULL, NULL, NULL},
    {"%s\\\\n", "a", NULL, NULL},
    {"%d%d\\n", "1", "2", NULL},
    {"%s", "-", NULL, NULL},
    {"\\a\\b\\f\\r\\v", NULL, NULL, NULL},
    {"\\q\\z\\n", NULL, NULL, NULL},
};

/* REFUSE: outside the documented subset or malformed. The builtin refuses BEFORE printing anything and returns 1;
 * /usr/bin/printf prints what precedes the bad conversion, then fails (also 1). Intentional difference. */
static const PfRow k_pf_refuse[] = {
    {"%q\\n", "5", NULL, NULL},
    {"ab%q", "5", NULL, NULL},
    {"%s%q\\n", "x", "y", NULL},
    {"%", NULL, NULL, NULL},
    {"a%", "x", NULL, NULL},
    {"%\\n", NULL, NULL, NULL},
    {"\\%%\\n", NULL, NULL, NULL},
    {"%5d\\n", "3", NULL, NULL},
    {"%-3s|\\n", "a", NULL, NULL},
    {"%.2s\\n", "abc", NULL, NULL},
    {"%ld\\n", "5", NULL, NULL},
    {"%X\\n", "255", NULL, NULL},
    {"%b\\n", "a", NULL, NULL},
    {"%e\\n", "1", NULL, NULL},
    {"%s %z\\n", "a", NULL, NULL},
};

/* DIFF: accepted by the builtin but intentionally not like coreutils (documented in OSH_PLATFORM_ABI / PR body). */
static const struct { const char *fmt, *want; } k_pf_diff[] = {
    {"\\x41\\n", "\\x41\n"},
    {"\\u0041\\n", "\\u0041\n"},
    {"a\\cb\\n", "a\\cb\n"},
};

static void test_printf_review(void)
{
    T t;
    t_begin(&t, "/usr/bin:/bin");
    OshResult r;
    int nsame = (int)(sizeof k_pf_same / sizeof k_pf_same[0]);
    for (int i = 0; i < nsame; i++) {
        const PfRow *w = &k_pf_same[i];
        t_clear(&t);
        char *res[2];
        size_t len[2] = {0, 0};
        int st[2];
        for (int k = 0; k < 2; k++) {
            t_clear(&t);
            OshBuilder *b = nb(0, 0);
            if (k == 0) cmd(b, OSH_B_PRINTF, "printf", w->fmt, NULL);
            else cmd(b, 0, "/usr/bin/printf", w->fmt, NULL);
            if (w->a1) osh_rb_arg(b, w->a1);
            if (w->a2) osh_rb_arg(b, w->a2);
            if (w->a3) osh_rb_arg(b, w->a3);
            st[k] = go(&t, b, &r);
            res[k] = t_out(&t);
            len[k] = g_slurp_len;
        }
        CHECK(len[0] == len[1] && memcmp(res[0], res[1], len[0]) == 0 && st[0] == st[1], "printf table #%d fmt '%s': builtin '%s' (%d) vs /usr/bin/printf '%s' (%d)", i, w->fmt, res[0], st[0], res[1], st[1]);
        free(res[0]);
        free(res[1]);
    }
    int nref = (int)(sizeof k_pf_refuse / sizeof k_pf_refuse[0]);
    for (int i = 0; i < nref; i++) {
        const PfRow *w = &k_pf_refuse[i];
        t_clear(&t);
        OshBuilder *b = nb(0, 0);
        cmd(b, OSH_B_PRINTF, "printf", w->fmt, NULL);
        if (w->a1) osh_rb_arg(b, w->a1);
        if (w->a2) osh_rb_arg(b, w->a2);
        int st = go(&t, b, &r);
        char *o = t_out(&t), *e = t_err(&t);
        CHECK(st == 1 && o[0] == 0 && e[0] != 0, "printf refuse #%d fmt '%s': status %d, stdout '%s', stderr '%s'", i, w->fmt, st, o, e);
        free(o);
        free(e);
    }
    int ndiff = (int)(sizeof k_pf_diff / sizeof k_pf_diff[0]);
    for (int i = 0; i < ndiff; i++) {
        t_clear(&t);
        OshBuilder *b = nb(0, 0);
        cmd(b, OSH_B_PRINTF, "printf", k_pf_diff[i].fmt, NULL);
        int st = go(&t, b, &r);
        char *o = t_out(&t);
        CHECK(st == 0 && strcmp(o, k_pf_diff[i].want) == 0, "printf documented difference '%s': got '%s'", k_pf_diff[i].fmt, o);
        free(o);
    }
    /* never read past the NUL: the argument after a trailing % or \ must never show up as format text */
    t_clear(&t);
    OshBuilder *b = nb(0, 0);
    cmd(b, OSH_B_PRINTF, "printf", "\\%", "SECRET", NULL);
    go(&t, b, &r);
    char *o = t_out(&t);
    CHECK(strstr(o, "SECRET") == NULL, "format scan stopped at the terminator, got '%s'", o);
    free(o);
    t_end(&t);
}

static void test_adversarial(void)
{
    T t;
    t_begin(&t, "/usr/bin:/bin");
    t.s.fd[0] = open("/dev/null", O_RDONLY | O_CLOEXEC);
    OshResult r;
    OshBuilder *b;
    /* fd and zombie hygiene on six error-path shapes, repeated (review R1) */
    int base = count_fds(), zomb = 0;
    for (int it = 0; it < 150; it++) {
        b = nb(0, 0); cmd(b, 0, "true", NULL); cmd(b, 0, "true", NULL); cmd(b, 0, "true", NULL);
        t.s.fail_fork_at = 3; go(&t, b, &r); t.s.fail_fork_at = 0;
        b = nb(0, 0); cmd(b, 0, "true", NULL); cmd(b, 0, "cat", NULL); osh_rb_redir(b, OSH_R_IN, 0, "/nonexistent/x", 0); go(&t, b, &r);
        b = nb(0, 0); cmd(b, 0, "true", NULL); cmd(b, 0, "nope-zz", NULL); cmd(b, 0, "cat", NULL); go(&t, b, &r);
        b = nb(0, 0); cmd(b, OSH_B_PWD, "pwd", NULL); osh_rb_redir(b, OSH_R_OUT, 1, "/dev/null", 0); osh_rb_redir(b, OSH_R_IN, 0, "/nonexistent/x", 0); go(&t, b, &r);
        b = nb(0, 0); cmd(b, OSH_B_PWD, "pwd", NULL); osh_rb_redir(b, OSH_R_OUT, 1, "/dev/null", 0); osh_rb_redir(b, OSH_R_DUP, 2, NULL, 1); go(&t, b, &r);
        b = nb(0, 0); cmd(b, OSH_B_PWD, "pwd", NULL); cmd(b, 0, "cat", NULL); go(&t, b, &r);
    }
    if (!(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD)) zomb = 1;
    CHECK(!zomb, "no unreaped child after 900 error-path runs");
    CHECK(count_fds() == base, "no descriptor leak after 900 error-path runs (%d vs %d)", count_fds(), base);
    /* exit / cd / export inside a pipeline do not touch the parent */
    b = nb(0, 0); cmd(b, OSH_B_EXIT, "exit", "3", NULL); cmd(b, 0, "cat", NULL);
    go(&t, b, &r);
    CHECK(!r.exit_requested && !t.s.exit_requested, "exit in a pipeline does not exit the shell");
    char *cwd0 = getcwd(NULL, 0);
    b = nb(0, 0); cmd(b, OSH_B_CD, "cd", "/", NULL); cmd(b, OSH_B_EXPORT, "export", "QQ=1", NULL); go(&t, b, &r);
    char *cwd1 = getcwd(NULL, 0);
    CHECK(strcmp(cwd0, cwd1) == 0 && osh_var_get(&t.s, "QQ") == NULL, "cd/export in a pipeline do not touch the shell");
    free(cwd0);
    free(cwd1);
    /* PATH edge cases */
    char longn[5000], longp[5000], out[PATH_MAX];
    memset(longn, 'a', 4999); longn[4999] = 0;
    b = nb(0, 0); cmd(b, 0, longn, NULL);
    CHECK(go(&t, b, &r) == 127, "over-long command name is not found");
    longp[0] = '/'; memset(longp + 1, 'a', 4990); longp[4991] = 0;
    b = nb(0, 0); cmd(b, 0, longp, NULL);
    int st = go(&t, b, &r);
    CHECK(st == 127 || st == 126, "over-long path status %d", st);
    osh_var_unset(&t.s, "PATH");
    CHECK(osh_resolve_linux(&t.s, "true", out, sizeof out) == 127, "PATH unset: not found");
    osh_var_export(&t.s, "PATH", "/usr/bin:/bin");
    CHECK(osh_resolve_linux(&t.s, "true", out, sizeof out) == 0 && osh_resolve_linux(&t.s, "", out, sizeof out) == 127, "PATH resolution and empty name");
    /* duplicate prefix assignments keep the last one, nothing leaks into the session, unexported stays private */
    t_clear(&t);
    b = nb(0, 0); cmd(b, 0, "printenv", "ZZV", NULL); osh_rb_assign(b, "ZZV", "1"); osh_rb_assign(b, "ZZV", "2");
    go(&t, b, &r);
    char *o = t_out(&t);
    CHECK(strcmp(o, "2\n") == 0 && osh_var_get(&t.s, "ZZV") == NULL, "duplicate prefix assignment: '%s'", o);
    free(o);
    osh_var_set(&t.s, "UNEXP", "v");
    b = nb(0, 0); cmd(b, 0, "printenv", "UNEXP", NULL);
    CHECK(go(&t, b, &r) == 1, "unexported variable is not passed to children");
    /* printf to a full device reports a write error */
    b = nb(0, 0); cmd(b, OSH_B_PRINTF, "printf", "%s\n", "x", NULL); osh_rb_redir(b, OSH_R_OUT, 1, "/dev/full", 0);
    CHECK(go(&t, b, &r) == 1, "printf to /dev/full fails with status 1");
    /* a script without a shebang: DECISION (review N1): the adapter does not fall back to sh; execve's ENOEXEC is
     * reported as 126, like a file that is found but cannot be executed. */
    wfile(P("noshebang.sh"), "echo hi\n", 0755);
    b = nb(0, 0); cmd(b, 0, P("noshebang.sh"), NULL);
    CHECK(go(&t, b, &r) == 126, "no-shebang script is 126 (documented: no sh fallback)");
    close(t.s.fd[0]);
    t_end(&t);
}

static void iso_low_nofile(Rep *rp)
{
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    rl.rlim_cur = 12;
    setrlimit(RLIMIT_NOFILE, &rl);
    T t;
    t_begin(&t, "/usr/bin:/bin");
    t.s.fd[0] = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int base = count_fds();
    OshResult r;
    OshBuilder *b = nb(0, 0);
    for (int i = 0; i < 5; i++) cmd(b, 0, "cat", NULL);
    int st = go(&t, b, &r);
    rp->status = st;
    rp->extra[0] = waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD;
    rp->extra[1] = count_fds() == base;
    for (int i = 0; i < 100; i++) {
        b = nb(0, 0); cmd(b, 0, "true", NULL); osh_rb_redir(b, OSH_R_OUT, 1, "/dev/null", 0); go(&t, b, &r);
        b = nb(0, 0); cmd(b, 0, "true", NULL); cmd(b, 0, "cat", NULL); cmd(b, 0, "cat", NULL); cmd(b, 0, "cat", NULL); cmd(b, 0, "cat", NULL); go(&t, b, &r);
    }
    rp->extra[2] = waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD && count_fds() == base;
    close(t.s.fd[0]);
    t_end(&t);
}

static void iso_child_mask(Rep *rp)
{
    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, SIGTERM);
    sigaddset(&m, SIGUSR1);
    sigprocmask(SIG_BLOCK, &m, NULL);
    T t;
    t_begin(&t, "/usr/bin:/bin");
    OshResult r;
    OshBuilder *b = nb(0, 0);
    helper(b, "sigblk", NULL);
    go(&t, b, &r);
    char *o = t_out(&t);
    rp->extra[0] = strcmp(o, "blocked 0\n") == 0;
    free(o);
    sigset_t cur;
    sigprocmask(SIG_BLOCK, NULL, &cur);
    rp->extra[1] = sigismember(&cur, SIGTERM) && sigismember(&cur, SIGUSR1); /* embedder's mask restored */
    t_end(&t);
}

static void test_adversarial_isolated(void)
{
    Rep rp;
    int rc = run_isolated(iso_low_nofile, &rp, 1, 60);
    CHECK(rc == 0 && rp.extra[0] && rp.extra[1] && rp.extra[2], "RLIMIT_NOFILE=12: 5-stage pipeline status %d, no zombie %d, no leak %d/%d", rp.status, rp.extra[0], rp.extra[1], rp.extra[2]);
    rc = run_isolated(iso_child_mask, &rp, 0, 15);
    CHECK(rc == 0 && rp.extra[0] == 1 && rp.extra[1] == 1, "children start with an empty signal mask; the embedder's mask is restored");
    /* decoder fuzz: mutated records are accepted or refused, never crash, accepted pointers stay inside the arena */
    OshBuilder g;
    osh_rb_init(&g, 0, 0);
    osh_rb_cmd(&g, 0); osh_rb_arg(&g, "true");
    osh_rb_cmd(&g, 0); osh_rb_arg(&g, "cat"); osh_rb_assign(&g, "A", "b"); osh_rb_redir(&g, OSH_R_OUT, 1, "/dev/null", 0);
    osh_rb_seal(&g);
    static OshRequest rq;
    static uint64_t rec[OSH_REQ_HDR_CELLS + OSH_MAX_CMDS * OSH_CMD_CELLS], out[OSH_OUT_CAP];
    unsigned seed = 1;
    int accepted = 0, refused = 0, badptr = 0;
    for (int i = 0; i < 100000; i++) {
        memcpy(rec, g.rec, sizeof rec);
        memcpy(out, g.out, sizeof out);
        int k = 1 + rand_r(&seed) % 3;
        for (int j = 0; j < k; j++) {
            size_t idx = rand_r(&seed) % (OSH_REQ_HDR_CELLS + 2 * OSH_CMD_CELLS + 4);
            uint64_t v;
            switch (rand_r(&seed) % 5) {
            case 0: v = ~0ull; break;
            case 1: v = (uint64_t)rand_r(&seed); break;
            case 2: v = 1ull << 63; break;
            case 3: v = 0xFFFFFFFFull; break;
            default: v = (uint64_t)(rand_r(&seed) % 9000);
            }
            rec[idx] = v;
        }
        size_t nrec = OSH_REQ_HDR_CELLS + 2 * OSH_CMD_CELLS;
        if (rand_r(&seed) % 8 == 0) nrec = (size_t)rand_r(&seed) % (nrec + 1);
        int dc = osh_req_decode(rec, nrec, out, rand_r(&seed) % 4 ? OSH_OUT_CAP : (size_t)(rand_r(&seed) % 100), &rq);
        if (dc) { refused++; continue; }
        accepted++;
        for (int c = 0; c < rq.ncmds; c++)
            for (int a = 0; a < rq.cmd[c].nargv; a++)
                if (!rq.cmd[c].argv[a] || rq.cmd[c].argv[a] < rq.arena || rq.cmd[c].argv[a] > rq.arena + sizeof rq.arena) badptr++;
    }
    CHECK(badptr == 0 && accepted > 0 && refused > 0, "decoder fuzz: %d accepted, %d refused, %d pointers outside the arena", accepted, refused, badptr);
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "--helper")) return helper_main(argc, argv);
    ssize_t n = readlink("/proc/self/exe", g_self, sizeof g_self - 1);
    if (n <= 0) { printf("cannot find own path\n"); return 2; }
    g_self[n] = 0;
    const char *base = getenv("TMPDIR");
    snprintf(g_tmp, sizeof g_tmp, "%s/osh-host-XXXXXX", base && *base ? base : "/tmp");
    if (!mkdtemp(g_tmp)) { printf("mkdtemp failed\n"); return 2; }
    char *cwd0 = getcwd(NULL, 0);
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc > 2 && !strcmp(argv[1], "--only")) { g_only_set = 1; g_only = argv[2]; }
    alarm(300); /* watchdog: a hang must fail the run, not stall it */
    if (want("env")) test_session_env();
    if (want("simple")) test_simple_and_path();
    if (want("builtins")) test_builtins();
    if (want("printf")) { test_printf(); test_printf_review(); }
    if (want("redirs")) test_redirs();
    if (want("pipelines")) test_pipelines();
    if (want("sigint")) { test_sigint_noninteractive(); test_sigint_direct_noninteractive(); test_inherited_ignore(); }
    if (want("stop")) test_stopped_member();
    if (want("pty")) test_interactive_pty();
    if (want("adversarial")) { test_adversarial(); test_adversarial_isolated(); }
    if (want("partial")) test_partial_launch();
    if (want("fdleaks")) test_fd_leaks();
    if (want("connectors")) test_connectors();
    if (want("refusals")) test_refusals();
    if (want("destinations")) test_destinations_and_authority();
    if (want("direct")) test_direct_equals_record();

    if (chdir(cwd0)) {}
    free(cwd0);
    nftw(g_tmp, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    printf("checks passed %d failed %d\n", g_pass, g_fail);
    printf(g_fail ? "OSH_HOST_FAIL\n" : "OSH_HOST_PASS\n");
    return g_fail != 0;
}
