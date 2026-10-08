/*
 * osh_exec.c -- execution service of the Linux host adapter (OSH_PLATFORM_ABI.md sections 7.2, 8.3, 8.4, 9, 10, 11, 12).
 *
 * DECISIONS where the ABI draft is silent (all UNVERIFIED against a second implementation; flagged in the PR):
 *  - Resolution classes: a request record carries no class. The adapter asks OshClassHook (RESOLVE). Anything other
 *    than OSH_CLASS_LINUX (omega native op, AIENOS artifact/service) is REFUSED for the whole pipeline before any
 *    effect: status 126, platform error UNAVAILABLE, each command FAILED_NO_EFFECT. Never forwarded, never retried
 *    as a Linux path. Without a hook every name is a Linux executable.
 *  - Authority: spawn/chdir/open-for-write need a valid binding (domain 1 or 2; domain 1 with generation above
 *    0xFFFFFFFF is CAP_GEN_NARROW) and the PERM_CHECK hook. Failure: nothing runs, status 126, err DENIED (or the
 *    hook's reason). A Linux host has no capability authority: the embedder supplies the binding.
 *  - PATH comes from the shell's table (a prefix assignment PATH=... on the command counts, as in bash). PATH unset:
 *    bare names are not found (127); process-global environ is never consulted after osh_session_init. An empty
 *    element means the current directory (POSIX). Names containing '/' are used as given, never searched.
 *  - Statuses: not found 127; found but not executable, a directory, or execve refused 126; killed by signal 128+n.
 *    A stopped child (SIGTSTP etc.) is cancelled (SIGTERM+SIGCONT, reaped), status 128+stopsig, err INTERRUPTED:
 *    there is no job control, a pipeline either ends or is killed.
 *  - A failed redirection prints a diagnostic, the command does not run, its status is 1; its pipe ends are closed so
 *    neighbours see EOF/SIGPIPE. Redirections are applied in the CHILD for external commands (so opening a FIFO cannot
 *    deadlock the parent) and in the parent (on a private descriptor table, never on fds 0-2) for parent builtins.
 *  - Standalone assignment (empty argv): redirections first, then variables set in the session; inside a multi-command
 *    pipeline it runs in a child and has no effect on the parent.
 *  - Fork failure after some commands started: PARTIAL_LAUNCH; the started ones get SIGTERM+SIGCONT to their group
 *    and are reaped (outcome CANCELLED); not-yet-started commands are NOT_STARTED; status 1.
 *  - waitpid ECHILD for a started child (someone else reaped it): that command is OUTCOME_UNKNOWN, status 1, err
 *    OUTCOME_UNKNOWN. SIGINT/SIGQUIT delivered to the shell while it waits are caught, noted in the result
 *    (sigint_seen) and never forwarded: the terminal sends them to the foreground group; the wait is simply retried
 *    (EINTR). A pipeline whose last command died of SIGINT/SIGQUIT sets killed_by_int and osh_run_list stops there.
 *  - No durable intent/outcome record (ABI section 10) is written: outcomes live in the result only. NOT DONE.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "osh_host.h"
#include "osh_priv.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---------------- signals ---------------- */

static volatile sig_atomic_t g_sig_seen;
static void on_sig(int sig) { g_sig_seen = sig; }

static const int k_sigs[] = {SIGINT, SIGQUIT, SIGTSTP, SIGTTOU, SIGTTIN, SIGPIPE, SIGCHLD};
#define NSIGS ((int)(sizeof k_sigs / sizeof k_sigs[0]))

typedef struct {
    struct sigaction old[NSIGS];
} SigGuard;

static void guard_enter(SigGuard *g)
{
    g_sig_seen = 0;
    for (int i = 0; i < NSIGS; i++) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sigemptyset(&sa.sa_mask);
        int sig = k_sigs[i];
        if (sig == SIGINT || sig == SIGQUIT) sa.sa_handler = on_sig; /* no SA_RESTART: waitpid must see EINTR */
        else if (sig == SIGCHLD) sa.sa_handler = SIG_DFL;
        else sa.sa_handler = SIG_IGN;
        sigaction(sig, &sa, &g->old[i]);
    }
}

static void guard_leave(SigGuard *g)
{
    for (int i = 0; i < NSIGS; i++) sigaction(k_sigs[i], &g->old[i], NULL);
}

/* in a freshly forked child: everything back to default, nothing blocked */
static void child_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = SIG_DFL;
    for (int i = 0; i < NSIGS; i++) sigaction(k_sigs[i], &sa, NULL);
    sigset_t all;
    sigemptyset(&all);
    sigprocmask(SIG_SETMASK, &all, NULL);
}

/* ---------------- resolution ---------------- */

/* 0 executable regular file; 127 missing; 126 present but unusable (errno_out says why) */
static int check_exec(const char *path, int *why)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) { *why = ENOENT; return 127; }
        *why = errno;
        return 126;
    }
    if (S_ISDIR(st.st_mode)) { *why = EISDIR; return 126; }
    if (!S_ISREG(st.st_mode) || access(path, X_OK) != 0) { *why = EACCES; return 126; }
    return 0;
}

static int resolve_in(const char *pathvar, const char *name, char *out, size_t cap, int *why)
{
    *why = ENOENT;
    if (!*name) return 127;
    if (strchr(name, '/')) {
        if (strlen(name) >= cap) { *why = ENAMETOOLONG; return 126; }
        strcpy(out, name);
        return check_exec(out, why);
    }
    if (!pathvar) return 127;
    int denied = 0;
    const char *p = pathvar;
    for (;;) {
        const char *e = strchr(p, ':');
        size_t dl = e ? (size_t)(e - p) : strlen(p);
        const char *dir = dl ? p : ".";
        if (!dl) dl = 1;
        if (dl + 1 + strlen(name) + 1 <= cap) {
            memcpy(out, dir, dl);
            out[dl] = '/';
            strcpy(out + dl + 1, name);
            int w, rc = check_exec(out, &w);
            if (rc == 0) return 0;
            if (rc == 126 && w == EACCES) denied = 1; /* a file named so exists but cannot run; keep looking */
        }
        if (!e) break;
        p = e + 1;
    }
    *why = denied ? EACCES : ENOENT;
    return denied ? 126 : 127;
}

int osh_resolve_linux(const OshSession *s, const char *name, char *out, size_t outsz)
{
    int why;
    return resolve_in(osh_var_get(s, "PATH"), name, out, outsz, &why);
}

static const char *cmd_path_var(const OshSession *s, const OshCmd *c)
{
    const char *p = osh_var_get(s, "PATH");
    for (int i = 0; i < c->nassign; i++)
        if (strcmp(c->assign[i].name, "PATH") == 0) p = c->assign[i].value;
    return p;
}

/* ---------------- authority ---------------- */

static int authorize(const OshSession *s, const OshRequest *r)
{
    int ops = 0;
    for (int i = 0; i < r->ncmds; i++) {
        const OshCmd *c = &r->cmd[i];
        if (c->nargv > 0 && c->builtin_id == OSH_B_NONE) ops |= 1 << OSH_OP_SPAWN;
        if (c->builtin_id == OSH_B_CD) ops |= 1 << OSH_OP_CHDIR;
        for (int k = 0; k < c->nredir; k++)
            if (c->redir[k].kind == OSH_R_OUT || c->redir[k].kind == OSH_R_APPEND) ops |= 1 << OSH_OP_OPEN_WRITE;
    }
    if (!ops) return OSH_E_OK;
    const OshBinding *b = &s->binding;
    if (!b->valid) return OSH_E_DENIED;
    if (b->domain != 1 && b->domain != 2) return OSH_E_CAP_DOMAIN_MISMATCH;
    if (b->domain == 1 && b->cap_generation > 0xFFFFFFFFull) return OSH_E_CAP_GEN_NARROW;
    if (s->perm_hook)
        for (int op = 1; op <= 3; op++)
            if (ops & (1 << op)) {
                int rc = s->perm_hook(s->hook_ctx, b, op);
                if (rc != OSH_E_OK) return rc;
            }
    return OSH_E_OK;
}

/* ---------------- descriptor tables and redirections ---------------- */

typedef struct {
    int fd[3];
    int own[OSH_MAX_REDIR];
    int nown;
} FdTab;

static void tab_init(FdTab *t, const OshSession *s)
{
    for (int i = 0; i < 3; i++) t->fd[i] = s->fd[i];
    t->nown = 0;
}

static void tab_close_owned(FdTab *t)
{
    while (t->nown > 0) close(t->own[--t->nown]);
}

/* move fd to a CLOEXEC descriptor >= 10; always consumes fd */
static int to_high(int fd)
{
    int h = fcntl(fd, F_DUPFD_CLOEXEC, 10);
    int saved = errno;
    close(fd);
    errno = saved;
    return h;
}

/* Apply redirections in record order on the table. 0 ok; -1 failed (diagnostic written to diagfd, owned fds stay
 * recorded in the table so the caller can close them). Every descriptor opened here is O_CLOEXEC. */
static int apply_redirs(FdTab *t, const OshCmd *c, int diagfd)
{
    for (int i = 0; i < c->nredir; i++) {
        const OshRedir *d = &c->redir[i];
        int nfd;
        if (d->kind == OSH_R_DUP) {
            nfd = fcntl(t->fd[d->src_fd], F_DUPFD_CLOEXEC, 10);
            if (nfd < 0) { osh_diag(diagfd, "%d>&%d: %s", d->fd, d->src_fd, strerror(errno)); return -1; }
        } else {
            int fl = d->kind == OSH_R_IN ? O_RDONLY : O_WRONLY | O_CREAT | (d->kind == OSH_R_APPEND ? O_APPEND : O_TRUNC);
            nfd = open(d->path, fl | O_CLOEXEC, 0666);
            if (nfd >= 0 && nfd < 10) nfd = to_high(nfd);
            if (nfd < 0) { osh_diag(diagfd, "%s: %s", d->path, strerror(errno)); return -1; }
        }
        t->own[t->nown++] = nfd;
        t->fd[d->fd] = nfd;
    }
    return 0;
}

/* child only: make table entries the real descriptors 0..2 */
static int child_install(FdTab *t)
{
    for (int i = 0; i < 3; i++)
        if (t->fd[i] < 3 && t->fd[i] != i) {
            int h = fcntl(t->fd[i], F_DUPFD_CLOEXEC, 10);
            if (h < 0) return -1;
            t->fd[i] = h;
        }
    for (int i = 0; i < 3; i++) {
        if (t->fd[i] == i) { if (fcntl(i, F_SETFD, 0) < 0) return -1; }
        else if (dup2(t->fd[i], i) < 0) return -1;
    }
    return 0;
}

/* ---------------- running one command ---------------- */

typedef struct {
    int pp[OSH_MAX_CMDS][2]; /* pipe i connects command i (write end) to command i+1 (read end) */
    int n;
} Pipes;

static void close_pipes(Pipes *p)
{
    for (int i = 0; i < p->n; i++)
        for (int e = 0; e < 2; e++)
            if (p->pp[i][e] >= 0) { close(p->pp[i][e]); p->pp[i][e] = -1; }
}

static void set_cmd(OshCmdResult *r, int status, int outcome, int err)
{
    r->status = status;
    r->outcome = outcome;
    r->err = err;
}

static void fail_all(OshResult *res, int n, int status, int err)
{
    for (int i = 0; i < n; i++) set_cmd(&res->cmd[i], status, OSH_OUT_FAILED_NO_EFFECT, err);
    res->status = status;
    res->err = err;
}

/* single parent-side command: builtin, or standalone assignment / redirection-only */
static void run_in_parent(OshSession *s, const OshCmd *c, OshResult *res)
{
    FdTab t;
    tab_init(&t, s);
    int st;
    if (apply_redirs(&t, c, s->fd[2]) != 0) {
        st = 1;
        set_cmd(&res->cmd[0], st, OSH_OUT_FAILED_NO_EFFECT, OSH_E_IO);
    } else {
        if (c->builtin_id) st = osh_builtin_run(s, c, t.fd, 1);
        else {
            st = 0;
            for (int i = 0; i < c->nassign; i++)
                if (osh_var_set(s, c->assign[i].name, c->assign[i].value)) { osh_diag(s->fd[2], "%s: cannot set", c->assign[i].name); st = 1; }
        }
        set_cmd(&res->cmd[0], st, OSH_OUT_COMPLETED, OSH_E_OK);
    }
    tab_close_owned(&t);
    res->status = st;
}

typedef struct {
    pid_t pid;
    int stopped; /* stop signal seen */
} Child;

static void wait_all(OshSession *s, OshResult *res, Child *ch, int n, pid_t pgid)
{
    for (int i = 0; i < n; i++) {
        if (ch[i].pid <= 0) continue;
        int st = 0;
        pid_t w;
        for (;;) {
            w = waitpid(ch[i].pid, &st, WUNTRACED);
            if (w < 0 && errno == EINTR) continue; /* a signal arrived; the effect is not re-run, just wait again */
            if (w == ch[i].pid && WIFSTOPPED(st)) {
                /* no job control: a stopped pipeline is ended, not parked */
                ch[i].stopped = WSTOPSIG(st);
                killpg(pgid, SIGTERM);
                killpg(pgid, SIGCONT);
                continue;
            }
            break;
        }
        OshCmdResult *r = &res->cmd[i];
        if (w < 0) {
            set_cmd(r, 1, OSH_OUT_UNKNOWN, OSH_E_OUTCOME_UNKNOWN); /* fate cannot be determined */
        } else if (ch[i].stopped) {
            r->termsig = WIFSIGNALED(st) ? WTERMSIG(st) : 0;
            set_cmd(r, 128 + ch[i].stopped, OSH_OUT_CANCELLED, OSH_E_INTERRUPTED);
        } else if (WIFSIGNALED(st)) {
            r->termsig = WTERMSIG(st);
            set_cmd(r, 128 + r->termsig, OSH_OUT_COMPLETED, OSH_E_OK);
        } else {
            set_cmd(r, WEXITSTATUS(st), OSH_OUT_COMPLETED, OSH_E_OK);
        }
    }
    (void)s;
}

static void run_pipeline(OshSession *s, const OshRequest *r, OshResult *res)
{
    int n = r->ncmds;
    Pipes p;
    p.n = n - 1;
    for (int i = 0; i < OSH_MAX_CMDS; i++) p.pp[i][0] = p.pp[i][1] = -1;
    for (int i = 0; i < n - 1; i++)
        if (pipe2(p.pp[i], O_CLOEXEC) != 0) {
            osh_diag(s->fd[2], "pipe: %s", strerror(errno));
            close_pipes(&p);
            fail_all(res, n, 1, OSH_E_LIMIT);
            return;
        }
    Child ch[OSH_MAX_CMDS];
    memset(ch, 0, sizeof ch);
    pid_t pgid = 0;
    int started = 0, launch_failed = 0;
    int tty = s->interactive && s->tty_fd >= 0;

    for (int i = 0; i < n; i++) {
        const OshCmd *c = &r->cmd[i];
        FdTab t;
        tab_init(&t, s);
        int rd = i > 0 ? p.pp[i - 1][0] : -1, wr = i < n - 1 ? p.pp[i][1] : -1;
        if (rd >= 0) t.fd[0] = rd;
        if (wr >= 0) t.fd[1] = wr;
        char path[PATH_MAX];
        char **envp = NULL;
        int external = c->nargv > 0 && c->builtin_id == OSH_B_NONE;

        if (external) {
            int why, rc = resolve_in(cmd_path_var(s, c), c->argv[0], path, sizeof path, &why);
            if (rc != 0) {
                if (rc == 127 && !strchr(c->argv[0], '/')) osh_diag(s->fd[2], "%s: command not found", c->argv[0]);
                else osh_diag(s->fd[2], "%s: %s", c->argv[0], strerror(why));
                set_cmd(&res->cmd[i], rc, OSH_OUT_FAILED_NO_EFFECT, OSH_E_NOT_FOUND);
                if (rd >= 0) { close(rd); p.pp[i - 1][0] = -1; }
                if (wr >= 0) { close(wr); p.pp[i][1] = -1; }
                continue;
            }
            envp = osh_build_envp(s, c->assign, c->nassign);
            if (!envp) { launch_failed = 1; set_cmd(&res->cmd[i], 1, OSH_OUT_NOT_STARTED, OSH_E_LIMIT); break; }
        }

        pid_t pid;
        if (s->fail_fork_at == i + 1) { pid = -1; errno = EAGAIN; }
        else pid = fork();
        if (pid < 0) {
            osh_diag(s->fd[2], "fork: %s", strerror(errno));
            osh_envp_free(envp);
            set_cmd(&res->cmd[i], 1, OSH_OUT_NOT_STARTED, OSH_E_LIMIT);
            launch_failed = 1;
            break;
        }
        if (pid == 0) {
            /* ---- child ---- */
            setpgid(0, pgid); /* pgid 0 on the first one: become a leader */
            if (tty && pgid == 0) tcsetpgrp(s->tty_fd, getpid()); /* SIGTTOU is still ignored here */
            child_signals();
            int st = 1;
            if (apply_redirs(&t, c, s->fd[2]) == 0 && child_install(&t) == 0) {
                for (int k = 0; k < p.n; k++)
                    for (int e = 0; e < 2; e++) if (p.pp[k][e] >= 0) close(p.pp[k][e]);
                tab_close_owned(&t);
                for (int k = 0; k < 3; k++) {
                    if (t.fd[k] >= 3) close(t.fd[k]);
                    if (s->fd[k] >= 3) close(s->fd[k]);
                }
                if (tty && s->tty_fd >= 3) close(s->tty_fd);
                if (external) {
                    execve(path, (char *const *)c->argv, envp);
                    osh_diag(2, "%s: %s", c->argv[0], strerror(errno));
                    st = (errno == ENOENT || errno == ENOTDIR) ? 127 : 126;
                } else if (c->builtin_id) {
                    static const int io[3] = {0, 1, 2};
                    st = osh_builtin_run(s, c, io, 0);
                } else {
                    st = 0; /* standalone assignment or redirections only: no parent effect in a pipeline */
                }
            }
            _exit(st);
        }
        /* ---- parent ---- */
        osh_envp_free(envp);
        if (pgid == 0) pgid = pid;
        setpgid(pid, pgid);
        if (tty && started == 0) tcsetpgrp(s->tty_fd, pgid);
        ch[i].pid = pid;
        res->cmd[i].pid = pid;
        started++;
        if (rd >= 0) { close(rd); p.pp[i - 1][0] = -1; }
        if (wr >= 0) { close(wr); p.pp[i][1] = -1; }
    }
    close_pipes(&p);

    if (launch_failed) {
        if (started > 0) {
            killpg(pgid, SIGTERM);
            killpg(pgid, SIGCONT);
        }
        wait_all(s, res, ch, n, pgid);
        for (int i = 0; i < n; i++)
            if (ch[i].pid > 0 && res->cmd[i].outcome == OSH_OUT_COMPLETED) res->cmd[i].outcome = OSH_OUT_CANCELLED;
        if (tty) tcsetpgrp(s->tty_fd, getpgrp());
        res->status = 1;
        res->err = started > 0 ? OSH_E_PARTIAL_LAUNCH : OSH_E_LIMIT;
        return;
    }
    if (s->after_launch_hook) s->after_launch_hook(s->after_launch_ctx);
    wait_all(s, res, ch, n, pgid);
    if (tty) tcsetpgrp(s->tty_fd, getpgrp());
    res->status = res->cmd[n - 1].status;
    for (int i = 0; i < n; i++)
        if (res->cmd[i].err != OSH_E_OK && res->cmd[i].err != OSH_E_NOT_FOUND && res->err == OSH_E_OK) res->err = res->cmd[i].err;
}

/* ---------------- public entry points ---------------- */

int osh_exec(OshSession *s, const OshRequest *r, OshResult *res)
{
    memset(res, 0, sizeof *res);
    res->ncmds = r->ncmds;
    int a = authorize(s, r);
    if (a != OSH_E_OK) {
        osh_diag(s->fd[2], "%s: not permitted (platform error %d); nothing was run", r->cmd[0].nargv ? r->cmd[0].argv[0] : "request", a);
        fail_all(res, r->ncmds, 126, a);
        s->last_status = res->status;
        return res->status;
    }
    for (int i = 0; i < r->ncmds; i++) {
        const OshCmd *c = &r->cmd[i];
        if (c->nargv == 0 || c->builtin_id != OSH_B_NONE || !s->class_hook) continue;
        int cls = s->class_hook(s->hook_ctx, c->argv[0]);
        if (cls != OSH_CLASS_LINUX) {
            osh_diag(s->fd[2], "%s: destination class %d is not a Linux executable and is unavailable on this host; refused", c->argv[0], cls);
            fail_all(res, r->ncmds, 126, OSH_E_UNAVAILABLE);
            s->last_status = res->status;
            return res->status;
        }
    }
    SigGuard g;
    guard_enter(&g);
    if (r->ncmds == 1 && (r->cmd[0].builtin_id != OSH_B_NONE || r->cmd[0].nargv == 0)) run_in_parent(s, &r->cmd[0], res);
    else run_pipeline(s, r, res);
    res->sigint_seen = g_sig_seen == SIGINT;
    const OshCmdResult *last = &res->cmd[r->ncmds - 1];
    res->killed_by_int = last->termsig == SIGINT || last->termsig == SIGQUIT;
    guard_leave(&g);
    res->exit_requested = s->exit_requested;
    s->last_status = res->status;
    return res->status;
}

int osh_exec_record(OshSession *s, const uint64_t *rec, size_t nrec, const uint64_t *out, size_t nout, OshResult *res)
{
    OshRequest *r = malloc(sizeof *r);
    memset(res, 0, sizeof *res);
    if (!r) { res->status = 1; res->err = OSH_E_LIMIT; return 1; }
    int rc = osh_req_decode(rec, nrec, out, nout, r);
    if (rc) {
        free(r);
        osh_diag(s->fd[2], "request refused (code %d); nothing was run", rc);
        res->refusal = rc;
        res->status = 2; /* ABI 5.4: a refused list reports status 2 */
        res->err = OSH_E_INVALID_ARG;
        s->last_status = 2;
        return 2;
    }
    int st = osh_exec(s, r, res);
    free(r);
    return st;
}

int osh_conn_should_run(int connector_after, int last_status)
{
    if (connector_after == OSH_CONN_AND) return last_status == 0;
    if (connector_after == OSH_CONN_OR) return last_status != 0;
    return 1;
}

int osh_run_list(OshSession *s, const OshRequest *const *reqs, int n, OshResult *res_out)
{
    for (int i = 0; i < n; i++) {
        OshResult local;
        OshResult *res = res_out ? &res_out[i] : &local;
        if (i > 0 && !osh_conn_should_run(reqs[i - 1]->connector_after, s->last_status)) {
            memset(res, 0, sizeof *res); /* skipped: $? is unchanged, the next connector sees the same status */
            continue;
        }
        osh_exec(s, reqs[i], res);
        if (s->exit_requested) return s->exit_status;
        if (res->killed_by_int) break;
    }
    return s->last_status;
}

int osh_submit_argv(OshSession *s, const char *const *argv, int nargv, const OshAssign *ov, int nov,
                    const OshRedir *redirs, int nredir, OshResult *res)
{
    OshBuilder *b = malloc(sizeof *b);
    if (!b) { memset(res, 0, sizeof *res); res->status = 1; res->err = OSH_E_LIMIT; return 1; }
    osh_rb_init(b, OSH_REQ_FLAG_DIRECT, OSH_CONN_NONE);
    osh_rb_cmd(b, OSH_B_NONE);
    for (int i = 0; i < nargv; i++) osh_rb_arg(b, argv[i]);
    for (int i = 0; i < nov; i++) osh_rb_assign(b, ov[i].name, ov[i].value);
    for (int i = 0; i < nredir; i++) osh_rb_redir(b, redirs[i].kind, redirs[i].fd, redirs[i].path, redirs[i].src_fd);
    osh_rb_seal(b);
    int st;
    if (b->err) { /* overflow while building: same refusal the decoder would give an oversized record */
        memset(res, 0, sizeof *res);
        osh_diag(s->fd[2], "direct request refused (code %d); nothing was run", b->err);
        res->refusal = b->err;
        res->status = 2;
        res->err = OSH_E_INVALID_ARG;
        s->last_status = 2;
        st = 2;
    } else {
        st = osh_exec_record(s, b->rec, osh_rb_cells(b), b->out, b->out_used, res);
    }
    free(b);
    return st;
}
