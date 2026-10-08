/*
 * test_osh_caps.c -- capability enforcement of the osh Linux host adapter (aien-architecture#158, ABI section 9).
 * Authority under test: omega's rx_caproot (separate root process) through src/osh/host/osh_caps.c. Everything happens
 * in a disposable directory under $TMPDIR. The test binary is also its own helper program (`--helper echo|touch|cat`).
 * Usage: test_osh_caps [path-to-osh-binary]   (with the path, the same properties are checked through `osh --caps`)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "osh_caps.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_pass, g_fail;
#define CHECK(c, msg) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); } } while (0)

static int helper_main(int argc, char **argv)
{
    const char *op = argc > 2 ? argv[2] : "";
    if (!strcmp(op, "echo")) { for (int i = 3; i < argc; i++) printf("%s%s", i > 3 ? " " : "", argv[i]); printf("\n"); return 0; }
    if (!strcmp(op, "touch")) { int fd = open(argv[3], O_WRONLY | O_CREAT, 0666); if (fd < 0) return 1; close(fd); return 0; }
    if (!strcmp(op, "cat")) { char b[4096]; ssize_t n; while ((n = read(0, b, sizeof b)) > 0) if (write(1, b, (size_t)n) != n) return 1; return 0; }
    return 2;
}

static char T[PATH_MAX], SELF[PATH_MAX], NOHELP[PATH_MAX], DIAG[PATH_MAX];

static void P(char *out, const char *rel) { snprintf(out, PATH_MAX, "%s/%s", T, rel); }
static int exists(const char *p) { struct stat st; return lstat(p, &st) == 0; }

static char *slurp(const char *p)
{
    static char b[8192];
    b[0] = 0;
    int fd = open(p, O_RDONLY);
    if (fd < 0) return b;
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    b[n > 0 ? n : 0] = 0;
    return b;
}

static void put(const char *p, const char *s)
{
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) { if (write(fd, s, strlen(s)) < 0) {} close(fd); }
}

static void copy_self(const char *dst)
{
    int in = open(SELF, O_RDONLY), out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    char b[65536];
    ssize_t n;
    while (in >= 0 && out >= 0 && (n = read(in, b, sizeof b)) > 0) if (write(out, b, (size_t)n) < 0) break;
    if (in >= 0) close(in);
    if (out >= 0) close(out);
}

/* number of direct children of this process (the capability root is one) */
static int nchildren(void)
{
    char p[64], b[1024];
    snprintf(p, sizeof p, "/proc/self/task/%d/children", (int)getpid());
    int fd = open(p, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0) return 0;
    b[n] = 0;
    int c = 0;
    for (char *s = strtok(b, " \n"); s; s = strtok(NULL, " \n")) c++;
    return c;
}

static int run(OshSession *s, const char *const *argv, int n, const OshRedir *rd, int nrd, OshResult *r)
{
    return osh_submit_argv(s, argv, n, NULL, 0, rd, nrd, r);
}

static int run_cd(OshSession *s, const char *dir, OshResult *r)
{
    OshBuilder *b = malloc(sizeof *b);
    osh_rb_init(b, 0, OSH_CONN_NONE);
    osh_rb_cmd(b, OSH_B_CD);
    osh_rb_arg(b, "cd");
    osh_rb_arg(b, dir);
    osh_rb_seal(b);
    int st = osh_exec_record(s, b->rec, osh_rb_cells(b), b->out, b->out_used, r);
    free(b);
    return st;
}

static char *diag_text(void) { return slurp(DIAG); }
static void diag_reset(void) { put(DIAG, ""); }

static int rm_rf(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char c[PATH_MAX];
            snprintf(c, sizeof c, "%s/%s", path, e->d_name);
            rm_rf(c);
        }
        if (d) closedir(d);
        return rmdir(path);
    }
    return unlink(path);
}

static void session(OshSession *s, int diagfd)
{
    char *env[] = {"PATH=/usr/bin:/bin", NULL};
    osh_session_init(s, env);
    s->fd[2] = diagfd;
}

static int start_caps(OshCaps *c, const char *policy)
{
    char err[256];
    if (osh_caps_start(c) != 0) return -1;
    if (osh_caps_load(c, policy, err, sizeof err) != 0) { printf("policy error: %s\n", err); return -1; }
    return 0;
}

static int spawn_status(pid_t pid)
{
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

/* run the osh program with --caps; returns its exit status; stdout/stderr go to /dev/null */
static int run_osh(const char *osh, const char *policy, const char *cmd, const char *extra_env)
{
    pid_t pid = fork();
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        dup2(dn, 1);
        dup2(dn, 2);
        if (extra_env) putenv((char *)extra_env);
        if (policy) execl(osh, osh, "--caps", policy, "-c", cmd, (char *)NULL);
        else execl(osh, osh, "-c", cmd, (char *)NULL);
        _exit(99);
    }
    return spawn_status(pid);
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "--helper")) return helper_main(argc, argv);
    const char *osh_bin = argc > 1 ? argv[1] : NULL;
    ssize_t sl = readlink("/proc/self/exe", SELF, sizeof SELF - 1);
    if (sl <= 0) return 70;
    SELF[sl] = 0;
    const char *tmp = getenv("TMPDIR");
    char tpl[PATH_MAX];
    snprintf(tpl, sizeof tpl, "%s/osh-caps-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(tpl)) return 70;
    if (!realpath(tpl, T)) return 70;
    char ok[PATH_MAX], no[PATH_MAX], tmpf[PATH_MAX];
    P(ok, "ok"); P(no, "no"); P(DIAG, "diag"); P(NOHELP, "no/helper");
    mkdir(ok, 0777); mkdir(no, 0777);
    copy_self(NOHELP);
    char old[PATH_MAX], keep[PATH_MAX];
    P(old, "no/old"); put(old, "KEEP");
    P(keep, "ok/in"); put(keep, "READABLE\n");
    char selfdir[PATH_MAX];
    strcpy(selfdir, SELF);
    *strrchr(selfdir, '/') = 0;

    char policy[4 * PATH_MAX];
    snprintf(policy, sizeof policy, "# test policy\nprincipal 77\nallow spawn %s\nallow read %s\nallow write %s\nallow chdir %s\n",
             selfdir, ok, ok, ok);

    int dfd = open(DIAG, O_WRONLY | O_CREAT | O_APPEND, 0666);
    OshCaps caps;
    OshSession s;
    OshResult r;
    CHECK(start_caps(&caps, policy) == 0, "root starts and policy mints");
    session(&s, dfd);
    osh_caps_attach(&caps, &s);
    CHECK(s.binding.cap_generation > 0xFFFFFFFFull, "real generation exceeds 2^32");
    CHECK(s.binding.cap_generation == caps.principal_ref.generation && s.binding.domain == 2, "binding carries the full 64-bit generation, domain 2");
    int base_children = nchildren();
    CHECK(base_children == 1, "only the capability root is a child before the tests");

    /* ---- allowed ---- */
    char o1[PATH_MAX], a1[PATH_MAX];
    P(o1, "ok/out"); P(a1, "ok/app");
    const char *echo[] = {SELF, "--helper", "echo", "hi"};
    OshRedir w1[] = {{OSH_R_OUT, 1, 0, o1}};
    CHECK(run(&s, echo, 4, w1, 1, &r) == 0 && r.err == OSH_E_OK, "allowed spawn + allowed write succeeds");
    CHECK(!strcmp(slurp(o1), "hi\n"), "allowed write produced the bytes");
    OshRedir ap[] = {{OSH_R_APPEND, 1, 0, o1}};
    CHECK(run(&s, echo, 4, ap, 1, &r) == 0 && !strcmp(slurp(o1), "hi\nhi\n"), "allowed append works");
    const char *cat[] = {SELF, "--helper", "cat"};
    OshRedir rin[] = {{OSH_R_IN, 0, 0, keep}, {OSH_R_OUT, 1, 0, a1}};
    CHECK(run(&s, cat, 3, rin, 2, &r) == 0 && !strcmp(slurp(a1), "READABLE\n"), "allowed read redirection works");
    char cwd0[PATH_MAX], cwd1[PATH_MAX];
    if (!getcwd(cwd0, sizeof cwd0)) cwd0[0] = 0;
    CHECK(run_cd(&s, ok, &r) == 0 && getcwd(cwd1, sizeof cwd1) && !strcmp(cwd1, ok), "allowed cd works");
    if (chdir(cwd0) != 0) {}

    /* ---- denied: no effect ---- */
    diag_reset();
    char mk[PATH_MAX];
    P(mk, "no/marker");
    const char *tch[] = {NOHELP, "--helper", "touch", mk};
    CHECK(run(&s, tch, 4, NULL, 0, &r) == 126 && r.err == OSH_E_DENIED && r.cmd[0].outcome == OSH_OUT_FAILED_NO_EFFECT, "denied spawn: 126, DENIED, FAILED_NO_EFFECT");
    CHECK(r.cmd[0].pid == 0 && !exists(mk) && nchildren() == base_children, "denied spawn: no pid, marker absent, process table unchanged");
    CHECK(strstr(diag_text(), "spawn") && strstr(diag_text(), "denied (DENIED)"), "denied spawn names the error");

    char nw[PATH_MAX];
    P(nw, "no/new");
    OshRedir wn[] = {{OSH_R_OUT, 1, 0, nw}};
    CHECK(run(&s, echo, 4, wn, 1, &r) == 1 && r.err == OSH_E_DENIED && r.cmd[0].pid == 0, "denied write: status 1, DENIED, no child");
    CHECK(!exists(nw), "denied write: file was not created");
    OshRedir wt[] = {{OSH_R_OUT, 1, 0, old}};
    CHECK(run(&s, echo, 4, wt, 1, &r) == 1 && !strcmp(slurp(old), "KEEP"), "denied truncate: existing file untouched");
    OshRedir wa[] = {{OSH_R_APPEND, 1, 0, old}};
    CHECK(run(&s, echo, 4, wa, 1, &r) == 1 && !strcmp(slurp(old), "KEEP"), "denied append: existing file untouched");
    OshRedir rn[] = {{OSH_R_IN, 0, 0, old}};
    CHECK(run(&s, cat, 3, rn, 1, &r) == 1 && r.err == OSH_E_DENIED && r.cmd[0].pid == 0, "denied read: status 1, no child");
    CHECK(run_cd(&s, no, &r) == 1 && getcwd(cwd1, sizeof cwd1) && !strcmp(cwd1, cwd0), "denied cd: status 1, directory unchanged");
    CHECK(strstr(diag_text(), "cd ") != NULL, "denied cd named");
    /* a read-only grant never authorizes a write and the other way round */
    char readonly[PATH_MAX];
    P(readonly, "ok");
    CHECK(osh_caps_effect(&caps, &s.binding, OSH_OP_OPEN_WRITE, "/etc/passwd") == OSH_E_DENIED, "no grant covers /etc/passwd");
    CHECK(osh_caps_effect(&caps, &s.binding, OSH_OP_SPAWN, keep) == OSH_E_DENIED, "write/read grants do not authorize spawn");

    /* symlink out of an allowed prefix */
    char lnk[PATH_MAX], viaf[PATH_MAX], dang[PATH_MAX], dtarget[PATH_MAX];
    P(lnk, "ok/lnk"); P(viaf, "ok/lnk/f"); P(dang, "ok/dang"); P(dtarget, "no/dtarget");
    if (symlink(no, lnk) != 0) {}
    OshRedir wl[] = {{OSH_R_OUT, 1, 0, viaf}};
    CHECK(run(&s, echo, 4, wl, 1, &r) == 1 && !exists(viaf) && !exists("/dev/null/x"), "symlink escape denied");
    char nof[PATH_MAX];
    P(nof, "no/f");
    CHECK(!exists(nof), "symlink escape: nothing created in the target");
    if (symlink(dtarget, dang) != 0) {}
    OshRedir wd[] = {{OSH_R_OUT, 1, 0, dang}};
    CHECK(run(&s, echo, 4, wd, 1, &r) == 1 && !exists(dtarget), "dangling symlink out of the prefix: target not created");

    /* ---- the documented non-claim: an allowed child is not contained ---- */
    char bychild[PATH_MAX];
    P(bychild, "no/by_child");
    const char *tch2[] = {SELF, "--helper", "touch", bychild};
    CHECK(run(&s, tch2, 4, NULL, 0, &r) == 0 && exists(bychild), "NON-CLAIM: an allowed child can write outside the write grants (no containment)");

    /* ---- forged principal ---- */
    osh_var_set(&s, "OSH_PRINCIPAL", "77");
    osh_var_set(&s, "PRINCIPAL", "77");
    osh_var_export(&s, "CAP_GEN", "1");
    CHECK(run(&s, tch, 4, NULL, 0, &r) == 126 && !exists(mk), "variables named like a principal grant nothing");
    OshBinding save = s.binding, f;
    f = save; f.principal_id[0] = 78;
    s.binding = f;
    CHECK(run(&s, echo, 4, NULL, 0, &r) == 126 && r.err == OSH_E_DENIED, "forged principal id (wrong subject) denied");
    f = save; f.principal_id[9] = 1;
    s.binding = f;
    CHECK(run(&s, echo, 4, NULL, 0, &r) == 126, "principal id with nonzero upper bytes denied");
    f = save; f.cap_index = 200;
    s.binding = f;
    CHECK(run(&s, echo, 4, NULL, 0, &r) == 126, "forged capability index denied");
    f = save; f.cap_generation ^= (1ull << 40); /* same low 32 bits */
    s.binding = f;
    CHECK((uint32_t)f.cap_generation == (uint32_t)save.cap_generation, "forgery keeps the low 32 bits");
    CHECK(run(&s, echo, 4, NULL, 0, &r) == 126 && r.err == OSH_E_STALE, "generation differing only above bit 32 is STALE (never narrowed)");
    f = save; f.domain = 1;
    s.binding = f;
    CHECK(run(&s, echo, 4, NULL, 0, &r) == 126 && r.err == OSH_E_CAP_GEN_NARROW, "domain 1 + 64-bit generation: CAP_GEN_NARROW");
    f.cap_generation = (uint32_t)save.cap_generation;
    s.binding = f;
    CHECK(run(&s, echo, 4, NULL, 0, &r) == 126 && r.err == OSH_E_CAP_DOMAIN_MISMATCH, "domain 1 binding against hosted resources: CAP_DOMAIN_MISMATCH");
    f = save; f.domain = 3;
    s.binding = f;
    CHECK(run(&s, echo, 4, NULL, 0, &r) == 126 && r.err == OSH_E_CAP_DOMAIN_MISMATCH, "unknown domain refused");
    s.binding = save;
    CHECK(run(&s, echo, 4, w1, 1, &r) == 0, "restored binding works again");

    /* ---- revocation and stale handles ---- */
    RxCapRef office = rx_capadmin_office(&caps.admin);
    int wi = 2; /* grant index of 'allow write' */
    CHECK(caps.grants[wi].op == OSH_OP_OPEN_WRITE, "grant table order as written");
    CHECK(rx_capadmin_revoke(&caps.admin, office, caps.grants[wi].ref) == RX_CAP_OK, "operator revokes the write grant");
    diag_reset();
    CHECK(run(&s, echo, 4, w1, 1, &r) == 1 && r.err == OSH_E_REVOKED, "same write after revoke: REVOKED");
    CHECK(strstr(diag_text(), "denied (REVOKED)") != NULL, "revocation named in the diagnostic");
    char o2[PATH_MAX];
    P(o2, "ok/out2");
    CHECK(!exists(o2), "no stray file");
    OshRedir w2[] = {{OSH_R_OUT, 1, 0, o2}};
    CHECK(run(&s, echo, 4, w2, 1, &r) == 1 && !exists(o2), "revoked grant: new file not created");
    CHECK(run(&s, echo, 4, NULL, 0, &r) == 0, "other grants unaffected by the revoke (spawn still works)");
    /* revoke the principal: everything stops */
    CHECK(rx_capadmin_revoke(&caps.admin, office, caps.principal_ref) == RX_CAP_OK, "operator revokes the session principal");
    CHECK(run(&s, echo, 4, NULL, 0, &r) == 126 && r.err == OSH_E_REVOKED && r.cmd[0].pid == 0, "spawn after principal revoke: REVOKED, not started");
    CHECK(run_cd(&s, ok, &r) == 1, "cd after principal revoke denied");
    /* reclaim bumps the generation: the old handle is stale, and stays so after the slot is reused */
    uint64_t g0 = caps.principal_ref.generation;
    CHECK(rx_capadmin_reclaim(&caps.admin, office, caps.principal_ref.cap_id) == RX_CAP_OK, "slot reclaimed");
    CHECK(run(&s, echo, 4, NULL, 0, &r) == 126 && r.err == OSH_E_STALE, "old handle after reclaim: STALE");
    RxCapMint m = {.issuer = 0, .subject = 77, .resource = 0, .rights = RX_RIGHT_READ, .parent = {UINT32_MAX, 0}, .authority = office};
    RxCapRef again;
    CHECK(rx_capadmin_mint(&caps.admin, &m, &again) == RX_CAP_OK && again.cap_id == caps.principal_ref.cap_id && again.generation > g0, "slot reused with a larger 64-bit generation");
    CHECK(run(&s, echo, 4, NULL, 0, &r) == 126 && r.err == OSH_E_STALE, "old handle still STALE after reuse");

    /* ---- seal: nothing in this process can mint or revoke ---- */
    OshCaps c2;
    OshSession s2;
    CHECK(start_caps(&c2, policy) == 0, "second root starts (above the first root's generations)");
    CHECK(c2.principal_ref.generation > caps.principal_ref.generation || c2.principal_ref.generation != caps.principal_ref.generation, "second root has its own generations");
    session(&s2, dfd);
    osh_caps_attach(&c2, &s2);
    osh_caps_seal(&c2);
    CHECK(run(&s2, echo, 4, w1, 1, &r) == 0, "sealed session: allowed effects still work");
    CHECK(rx_capadmin_mint(&c2.admin, &m, &again) != RX_CAP_OK, "sealed: mint impossible");
    CHECK(rx_capadmin_revoke(&c2.admin, rx_capadmin_office(&c2.admin), c2.principal_ref) != RX_CAP_OK, "sealed: revoke impossible");
    CHECK(run(&s2, tch, 4, NULL, 0, &r) == 126 && !exists(mk), "sealed: denied stays denied");
    osh_caps_stop(&c2);
    osh_session_free(&s2);

    /* ---- policy errors, empty policy ---- */
    OshCaps c3;
    char err[256];
    CHECK(osh_caps_start(&c3) == 0 && osh_caps_load(&c3, "allow spawn /\n", err, sizeof err) != 0, "grant before principal refused");
    osh_caps_stop(&c3);
    CHECK(osh_caps_start(&c3) == 0 && osh_caps_load(&c3, "principal 5\nallow spawn relative/path\n", err, sizeof err) != 0, "relative prefix refused");
    osh_caps_stop(&c3);
    CHECK(osh_caps_start(&c3) == 0 && osh_caps_load(&c3, "principal 5\nallow exec /bin\n", err, sizeof err) != 0, "unknown verb refused");
    osh_caps_stop(&c3);
    CHECK(osh_caps_start(&c3) == 0 && osh_caps_load(&c3, "", err, sizeof err) != 0, "policy without a principal refused");
    osh_caps_stop(&c3);
    OshSession s3;
    session(&s3, dfd);
    CHECK(osh_caps_start(&c3) == 0 && osh_caps_load(&c3, "principal 5\n", err, sizeof err) == 0, "principal-only policy loads");
    osh_caps_attach(&c3, &s3);
    CHECK(run(&s3, echo, 4, NULL, 0, &r) == 126 && r.err == OSH_E_DENIED, "no grants: default deny");
    osh_caps_stop(&c3);
    osh_session_free(&s3);

    /* ---- the osh program itself ---- */
    if (osh_bin) {
        char pol[PATH_MAX], cmd[4 * PATH_MAX], e1[PATH_MAX];
        P(pol, "policy");
        put(pol, policy);
        P(e1, "ok/cli");
        snprintf(cmd, sizeof cmd, "%s --helper echo viaosh > %s", SELF, e1);
        CHECK(run_osh(osh_bin, pol, cmd, NULL) == 0 && !strcmp(slurp(e1), "viaosh\n"), "osh --caps: allowed command runs");
        char nfile[PATH_MAX];
        P(nfile, "no/cli");
        snprintf(cmd, sizeof cmd, "%s --helper echo x > %s", SELF, nfile);
        CHECK(run_osh(osh_bin, pol, cmd, NULL) == 1 && !exists(nfile), "osh --caps: denied redirection status 1, file not created");
        snprintf(cmd, sizeof cmd, "%s --helper touch %s", NOHELP, mk);
        CHECK(run_osh(osh_bin, pol, cmd, NULL) == 126 && !exists(mk), "osh --caps: denied spawn status 126, no marker");
        snprintf(cmd, sizeof cmd, "cd %s", no);
        CHECK(run_osh(osh_bin, pol, cmd, NULL) == 1, "osh --caps: denied cd status 1");
        snprintf(cmd, sizeof cmd, "cd %s", ok);
        CHECK(run_osh(osh_bin, pol, cmd, NULL) == 0, "osh --caps: allowed cd status 0");
        snprintf(cmd, sizeof cmd, "OSH_PRINCIPAL=77 PRINCIPAL=77 %s --helper touch %s", NOHELP, mk);
        CHECK(run_osh(osh_bin, pol, cmd, "OSH_PRINCIPAL=77") == 126 && !exists(mk), "osh --caps: principal-looking variables grant nothing");
        snprintf(cmd, sizeof cmd, "/bin/true");
        CHECK(run_osh(osh_bin, pol, cmd, NULL) == 126, "osh --caps: /bin/true is outside the spawn grant");
        P(tmpf, "missing-policy");
        CHECK(run_osh(osh_bin, tmpf, "true", NULL) == 70, "osh --caps with an unreadable policy refuses to start");
        char bad[PATH_MAX];
        P(bad, "bad-policy");
        put(bad, "principal 1\nallow bogus /\n");
        CHECK(run_osh(osh_bin, bad, "true", NULL) == 70, "osh --caps with a bad policy refuses to start");
        char pol0[PATH_MAX];
        P(pol0, "empty-policy");
        put(pol0, "principal 1\n");
        CHECK(run_osh(osh_bin, pol0, "/bin/true", NULL) == 126, "osh --caps with no grants: everything denied");
        /* fail closed: no --caps at all is the same as a policy with no grants */
        char dflt[PATH_MAX];
        P(dflt, "default-denied");
        CHECK(run_osh(osh_bin, NULL, "/bin/true", NULL) == 126, "osh without --caps: spawn denied (fail closed)");
        snprintf(cmd, sizeof cmd, "printf x > %s", dflt);
        CHECK(run_osh(osh_bin, NULL, cmd, NULL) == 1 && !exists(dflt), "osh without --caps: redirection denied, file not created");
        CHECK(run_osh(osh_bin, NULL, "cd /", NULL) == 1, "osh without --caps: cd denied");
        CHECK(run_osh(osh_bin, NULL, "printf ''", NULL) == 0, "osh without --caps: a builtin with no effect still runs");
        CHECK(run_osh(osh_bin, NULL, "OSH_CAPS=/ PATH=/bin true", "OSH_CAPS=/") == 126, "osh without --caps: variables grant nothing");
    }

    osh_caps_stop(&caps);
    osh_session_free(&s);
    close(dfd);
    if (g_fail) printf("DIAG: %s\n", slurp(DIAG));
    rm_rf(T);
    printf("checks: %d passed, %d failed\n", g_pass, g_fail);
    printf(g_fail ? "OSH_CAPS_FAIL\n" : "OSH_CAPS_PASS\n");
    return g_fail ? 1 : 0;
}
