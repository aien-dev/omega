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
 *  - Statuses: not found 127; found but not executable, a directory, or execve refused (including ENOEXEC: a script
 *    without a shebang is NOT run through sh) 126; killed by signal 128+n.
 *  - STOPPED MEMBERS (no job control in R1): the wait loop notices a stop of ANY member (SIGSTOP, SIGTSTP, ...), sends
 *    SIGKILL to the pipeline (killpg when it has its own group, else to each unreaped member), reaps all, writes a
 *    diagnostic, and the pipeline status is 128+stopsig of the first member seen stopped. The stopped member is
 *    CANCELLED with err INTERRUPTED; members killed by that SIGKILL are CANCELLED too (termsig 9). Same for
 *    interactive and non-interactive sessions. Job control (fg/bg, resuming) is R1.2+.
 *  - PROCESS GROUPS AND SIGNALS. Non-interactive (osh -c, scripts, embedder default): no new process group; children
 *    stay in the shell's group, so a terminal ^C or a pgroup SIGINT reaches them and the shell together. While
 *    waiting, the shell does not die of SIGINT/SIGQUIT: it notes them (result sigint_seen) and acts on the child's
 *    status. If the last command died of SIGINT/SIGQUIT the session sets exit_requested with exit_status 128+sig
 *    (130 for SIGINT) after the pipeline, as bash does for scripts; osh_run_list stops there. A SIGINT sent only to
 *    the shell pid is not forwarded (nothing is in a separate group). If SIGINT/SIGQUIT were ignored when osh_exec
 *    was entered, they stay ignored and the children inherit that (POSIX). Interactive (s->interactive): every
 *    pipeline is its own process group; with tty_fd the shell hands it the terminal (tcsetpgrp) and takes the terminal
 *    back afterwards; the shell ignores SIGINT/SIGQUIT/SIGTSTP/SIGTTOU/SIGTTIN itself (noted, never fatal); a ^C goes
 *    to the foreground group = the pipeline; a SIGINT/SIGQUIT sent to the shell pid while it waits is forwarded once to
 *    the pipeline's group (killpg). Interactive children always start with default dispositions; the interactive
 *    session does not exit when a child dies of SIGINT (osh_run_list only stops the list). SIGINT/SIGQUIT stay blocked
 *    across fork so a signal cannot run the shell's handler inside a new child.
 *  - A failed redirection prints a diagnostic, the command does not run, its status is 1; its pipe ends are closed so
 *    neighbours see EOF/SIGPIPE. Redirections are applied in the CHILD for external commands (so opening a FIFO cannot
 *    deadlock the parent) and in the parent (on a private descriptor table, never on fds 0-2) for parent builtins and
 *    standalone assignments. A parent-side redirection of a FIFO with no peer therefore BLOCKS the shell in open(),
 *    exactly as bash does (`cd < fifo`, `A=1 > fifo`); only a signal ends it. Intentionally not worked around.
 *  - Standalone assignment (empty argv): redirections first, then variables set in the session; inside a multi-command
 *    pipeline it runs in a child and has no effect on the parent.
 *  - Fork failure after some commands started: PARTIAL_LAUNCH; the started ones get SIGTERM+SIGCONT (to their group
 *    when interactive, else to each pid) and are reaped (outcome CANCELLED); not-yet-started commands are NOT_STARTED; status 1.
 *  - waitpid ECHILD for a started child (someone else reaped it): that command is OUTCOME_UNKNOWN, status 1, err
 *    OUTCOME_UNKNOWN. A pipeline whose last command died of SIGINT/SIGQUIT sets killed_by_int and osh_run_list stops there.
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

static volatile sig_atomic_t g_sig_seen, g_sig_count;
static void on_sig(int sig) { g_sig_seen = sig; g_sig_count++; }
static void on_chld(int sig) { (void)sig; } /* only here so that sigsuspend wakes on a child state change */

static const int k_sigs[] = {SIGINT, SIGQUIT, SIGTSTP, SIGTTOU, SIGTTIN, SIGPIPE, SIGCHLD};
#define NSIGS ((int)(sizeof k_sigs / sizeof k_sigs[0]))

/* SIGINT / SIGQUIT were ignored when osh_exec was entered (nohup, `cmd &` in a parent shell, an embedder choice).
 * POSIX: a non-interactive shell leaves them ignored and its children inherit that. Set by guard_enter. */
static int g_entry_ign[2];

typedef struct {
    struct sigaction old[NSIGS];
} SigGuard;

static void guard_enter(SigGuard *g, int interactive)
{
    g_sig_seen = 0;
    g_sig_count = 0;
    g_entry_ign[0] = g_entry_ign[1] = 0;
    for (int i = 0; i < NSIGS; i++) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sigemptyset(&sa.sa_mask);
        int sig = k_sigs[i];
        sigaction(sig, NULL, &g->old[i]);
        if (sig == SIGINT || sig == SIGQUIT) {
            int ign = g->old[i].sa_handler == SIG_IGN;
            g_entry_ign[sig == SIGQUIT] = ign && !interactive;
            /* The shell never dies of these while it waits: it notes them (no SA_RESTART: waitpid/sigsuspend must see
             * EINTR) and acts on the child's status. An inherited ignore stays an ignore (non-interactive only). */
            sa.sa_handler = ign && !interactive ? SIG_IGN : on_sig;
        } else if (sig == SIGCHLD) {
            sa.sa_handler = on_chld;
        } else {
            sa.sa_handler = SIG_IGN; /* TSTP, TTOU, TTIN, PIPE */
        }
        sigaction(sig, &sa, NULL);
    }
}

static void guard_leave(SigGuard *g)
{
    for (int i = 0; i < NSIGS; i++) sigaction(k_sigs[i], &g->old[i], NULL);
}

/* in a freshly forked child: everything back to default, nothing blocked. The one exception is a non-interactive
 * shell that inherited SIGINT / SIGQUIT as ignored: its children keep them ignored (POSIX). */
static void child_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = SIG_DFL;
    for (int i = 0; i < NSIGS; i++) sigaction(k_sigs[i], &sa, NULL);
    sa.sa_handler = SIG_IGN;
    if (g_entry_ign[0]) sigaction(SIGINT, &sa, NULL);
    if (g_entry_ign[1]) sigaction(SIGQUIT, &sa, NULL);
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

/* ---------------- per-effect authority (ABI section 9.2) ---------------- */

static const char *e_name(int e)
{
    switch (e) {
    case OSH_E_DENIED: return "DENIED";
    case OSH_E_REVOKED: return "REVOKED";
    case OSH_E_STALE: return "STALE";
    case OSH_E_CAP_DOMAIN_MISMATCH: return "CAP_DOMAIN_MISMATCH";
    case OSH_E_CAP_GEN_NARROW: return "CAP_GEN_NARROW";
    default: return "REFUSED";
    }
}

int osh_effect_check(const OshSession *s, int op, const char *path, int diagfd)
{
    if (!s->effect_hook) return OSH_E_OK;
    int rc = s->effect_hook(s->effect_ctx, &s->binding, op, path);
    if (rc == OSH_E_OK) return OSH_E_OK;
    const char *what = op == OSH_OP_SPAWN ? "spawn" : op == OSH_OP_CHDIR ? "cd" : op == OSH_OP_OPEN_WRITE ? "write" : "read";
    osh_diag(diagfd, "%s %s: denied (%s); nothing was done", what, path, e_name(rc));
    return rc;
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
static int apply_redirs(const OshSession *s, FdTab *t, const OshCmd *c, int diagfd, int *err)
{
    for (int i = 0; i < c->nredir; i++) {
        const OshRedir *d = &c->redir[i];
        int nfd;
        if (d->kind == OSH_R_DUP) {
            nfd = fcntl(t->fd[d->src_fd], F_DUPFD_CLOEXEC, 10);
            if (nfd < 0) { osh_diag(diagfd, "%d>&%d: %s", d->fd, d->src_fd, strerror(errno)); return -1; }
        } else {
            int ge = osh_effect_check(s, d->kind == OSH_R_IN ? OSH_OP_OPEN_READ : OSH_OP_OPEN_WRITE, d->path, diagfd);
            if (ge != OSH_E_OK) { if (err) *err = ge; return -1; }
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
    int rerr = OSH_E_IO;
    if (apply_redirs(s, &t, c, s->fd[2], &rerr) != 0) {
        st = 1;
        set_cmd(&res->cmd[0], st, OSH_OUT_FAILED_NO_EFFECT, rerr);
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
    int done;      /* reaped (or lost) */
    int lost;      /* waitpid said ECHILD: somebody else reaped it, the fate is unknown */
    int stopped;   /* stop signal seen */
    int cancelled; /* we sent it SIGKILL because a member stopped */
    int st;        /* wait status when done */
} Child;

/* Send sig to every member that is still running. Interactive sessions put the pipeline in its own process group, so
 * one killpg does it. Non-interactive sessions share the shell's group and must NEVER killpg: signal each pid, which
 * is safe because an unreaped child's pid cannot be reused. */
static void signal_kids(const Child *ch, int n, pid_t pgid, int own_pg, int sig)
{
    if (own_pg) {
        if (pgid > 0) killpg(pgid, sig);
        return;
    }
    for (int i = 0; i < n; i++)
        if (ch[i].pid > 0 && !ch[i].done) kill(ch[i].pid, sig);
}

/* Wait for every member, noticing a stop of ANY member (not just the one being waited on). R1 has no job control: the
 * first stop cancels the pipeline with SIGKILL (to the group when there is one, else to each member), then all are
 * reaped. Returns the first stop signal seen, or 0. While waiting, SIGINT/SIGQUIT delivered to the shell itself are
 * forwarded to the pipeline's group once per delivery when it has its own group (interactive); a non-interactive
 * pipeline shares the shell's group and gets the terminal's signal directly, so nothing extra is sent. */
static int wait_all(OshSession *s, OshResult *res, Child *ch, int n, pid_t pgid, int own_pg)
{
    sigset_t blk, old, susp;
    sigemptyset(&blk);
    sigaddset(&blk, SIGCHLD);
    sigaddset(&blk, SIGINT);
    sigaddset(&blk, SIGQUIT);
    sigprocmask(SIG_BLOCK, &blk, &old); /* blocked while scanning: sigsuspend then wakes for anything that arrived */
    susp = old;
    sigdelset(&susp, SIGCHLD);
    int pending = 0, first_stop = 0, cancel_sent = 0;
    for (int i = 0; i < n; i++) if (ch[i].pid > 0) pending++;
    int seen = 0; /* g_sig_count restarts at 0 for every osh_exec: a signal that landed during launch is forwarded too */
    while (pending > 0) {
        for (int i = 0; i < n; i++) {
            if (ch[i].pid <= 0 || ch[i].done) continue;
            int st = 0;
            pid_t w = waitpid(ch[i].pid, &st, WNOHANG | WUNTRACED);
            if (w == 0) continue;
            if (w < 0) {
                if (errno == EINTR) { i--; continue; }
                ch[i].done = ch[i].lost = 1;
                pending--;
                continue;
            }
            if (WIFSTOPPED(st)) {
                if (!ch[i].stopped) {
                    ch[i].stopped = WSTOPSIG(st);
                    if (!first_stop) first_stop = ch[i].stopped;
                }
                continue; /* it is killed below; the next pass reaps it */
            }
            ch[i].st = st;
            ch[i].done = 1;
            pending--;
        }
        if (pending == 0) break;
        if (first_stop && !cancel_sent) {
            cancel_sent = 1;
            osh_diag(s->fd[2], "pipeline stopped (signal %d); there is no job control yet, so it is cancelled", first_stop);
            for (int i = 0; i < n; i++) if (ch[i].pid > 0 && !ch[i].done) ch[i].cancelled = 1;
            signal_kids(ch, n, pgid, own_pg, SIGKILL);
        }
        if (own_pg && !cancel_sent && g_sig_count != seen && pgid > 0) {
            seen = g_sig_count;
            killpg(pgid, g_sig_seen == SIGQUIT ? SIGQUIT : SIGINT);
        }
        sigsuspend(&susp); /* returns after a handled signal: SIGCHLD, or SIGINT/SIGQUIT noted for the next pass */
    }
    sigprocmask(SIG_SETMASK, &old, NULL);
    for (int i = 0; i < n; i++) {
        if (ch[i].pid <= 0) continue;
        OshCmdResult *r = &res->cmd[i];
        int st = ch[i].st;
        if (ch[i].lost) {
            set_cmd(r, 1, OSH_OUT_UNKNOWN, OSH_E_OUTCOME_UNKNOWN); /* fate cannot be determined */
        } else if (ch[i].stopped) {
            r->termsig = WIFSIGNALED(st) ? WTERMSIG(st) : 0;
            set_cmd(r, 128 + ch[i].stopped, OSH_OUT_CANCELLED, OSH_E_INTERRUPTED);
        } else if (WIFSIGNALED(st)) {
            r->termsig = WTERMSIG(st);
            if (ch[i].cancelled && r->termsig == SIGKILL) set_cmd(r, 128 + r->termsig, OSH_OUT_CANCELLED, OSH_E_INTERRUPTED);
            else set_cmd(r, 128 + r->termsig, OSH_OUT_COMPLETED, OSH_E_OK);
        } else {
            set_cmd(r, WEXITSTATUS(st), OSH_OUT_COMPLETED, OSH_E_OK);
        }
    }
    return first_stop;
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
    /* Interactive: each pipeline is its own process group and (with a terminal) owns the terminal while it runs.
     * Non-interactive: no new group at all; the children stay in the shell's group like bash does for scripts. */
    int own_pg = s->interactive;
    int tty = own_pg && s->tty_fd >= 0;
    sigset_t fork_blk, fork_old;
    sigemptyset(&fork_blk);
    sigaddset(&fork_blk, SIGINT);
    sigaddset(&fork_blk, SIGQUIT);

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
        int pre = OSH_E_OK;
        for (int k = 0; k < c->nredir && pre == OSH_E_OK; k++)
            if (c->redir[k].kind != OSH_R_DUP)
                pre = osh_effect_check(s, c->redir[k].kind == OSH_R_IN ? OSH_OP_OPEN_READ : OSH_OP_OPEN_WRITE, c->redir[k].path, s->fd[2]);
        if (pre != OSH_E_OK) { /* denied before any process exists; the child re-checks right before its own open() */
            set_cmd(&res->cmd[i], 1, OSH_OUT_FAILED_NO_EFFECT, pre);
            if (rd >= 0) { close(rd); p.pp[i - 1][0] = -1; }
            if (wr >= 0) { close(wr); p.pp[i][1] = -1; }
            continue;
        }

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
            int ge = osh_effect_check(s, OSH_OP_SPAWN, path, s->fd[2]);
            if (ge != OSH_E_OK) {
                set_cmd(&res->cmd[i], 126, OSH_OUT_FAILED_NO_EFFECT, ge);
                if (rd >= 0) { close(rd); p.pp[i - 1][0] = -1; }
                if (wr >= 0) { close(wr); p.pp[i][1] = -1; }
                continue;
            }
            envp = osh_build_envp(s, c->assign, c->nassign);
            if (!envp) { launch_failed = 1; set_cmd(&res->cmd[i], 1, OSH_OUT_NOT_STARTED, OSH_E_LIMIT); break; }
        }

        pid_t pid;
        /* SIGINT/SIGQUIT stay blocked across fork: a signal that lands before the child has reset its handlers would
         * otherwise run the shell's handler inside the child. They are delivered once the child's signals are default. */
        sigprocmask(SIG_BLOCK, &fork_blk, &fork_old);
        if (s->fail_fork_at == i + 1) { pid = -1; errno = EAGAIN; }
        else pid = fork();
        if (pid < 0) {
            int saved = errno;
            sigprocmask(SIG_SETMASK, &fork_old, NULL);
            errno = saved;
            osh_diag(s->fd[2], "fork: %s", strerror(errno));
            osh_envp_free(envp);
            set_cmd(&res->cmd[i], 1, OSH_OUT_NOT_STARTED, OSH_E_LIMIT);
            launch_failed = 1;
            break;
        }
        if (pid == 0) {
            /* ---- child ---- */
            if (own_pg) setpgid(0, pgid); /* pgid 0 on the first one: become a leader */
            if (tty && pgid == 0) tcsetpgrp(s->tty_fd, getpid()); /* SIGTTOU is still ignored here */
            child_signals();
            int st = 1;
            if (apply_redirs(s, &t, c, s->fd[2], NULL) == 0 && child_install(&t) == 0) {
                /* close every descriptor that is not 0..2, each exactly once (no double close) */
                int cl[2 * OSH_MAX_CMDS + OSH_MAX_REDIR + 8], ncl = 0;
                for (int k = 0; k < p.n; k++)
                    for (int e = 0; e < 2; e++) if (p.pp[k][e] >= 0) cl[ncl++] = p.pp[k][e];
                for (int k = 0; k < t.nown; k++) cl[ncl++] = t.own[k];
                for (int k = 0; k < 3; k++) {
                    cl[ncl++] = t.fd[k];
                    cl[ncl++] = s->fd[k];
                }
                if (tty) cl[ncl++] = s->tty_fd;
                for (int k = 0; k < ncl; k++) {
                    int dup = cl[k] < 3;
                    for (int j = 0; j < k && !dup; j++) dup = cl[j] == cl[k];
                    if (!dup) close(cl[k]);
                }
                if (external) {
                    if (osh_effect_check(s, OSH_OP_SPAWN, path, 2) != OSH_E_OK) _exit(126); /* defensive: revoked between fork and exec; the osh program seals its policy before the first command, so it cannot trigger there (no test reaches it) */
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
        sigprocmask(SIG_SETMASK, &fork_old, NULL);
        osh_envp_free(envp);
        if (pgid == 0) pgid = pid;
        if (own_pg) setpgid(pid, pgid);
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
            signal_kids(ch, n, pgid, own_pg, SIGTERM);
            signal_kids(ch, n, pgid, own_pg, SIGCONT);
        }
        wait_all(s, res, ch, n, pgid, own_pg);
        for (int i = 0; i < n; i++)
            if (ch[i].pid > 0 && res->cmd[i].outcome == OSH_OUT_COMPLETED) res->cmd[i].outcome = OSH_OUT_CANCELLED;
        if (tty) tcsetpgrp(s->tty_fd, getpgrp());
        res->status = 1;
        res->err = started > 0 ? OSH_E_PARTIAL_LAUNCH : OSH_E_LIMIT;
        return;
    }
    if (s->after_launch_hook) s->after_launch_hook(s->after_launch_ctx);
    int stopsig = wait_all(s, res, ch, n, pgid, own_pg);
    if (tty) tcsetpgrp(s->tty_fd, getpgrp());
    res->status = stopsig ? 128 + stopsig : res->cmd[n - 1].status;
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
    guard_enter(&g, s->interactive);
    if (r->ncmds == 1 && (r->cmd[0].builtin_id != OSH_B_NONE || r->cmd[0].nargv == 0)) run_in_parent(s, &r->cmd[0], res);
    else run_pipeline(s, r, res);
    res->sigint_seen = g_sig_seen == SIGINT;
    const OshCmdResult *last = &res->cmd[r->ncmds - 1];
    res->killed_by_int = last->termsig == SIGINT || last->termsig == SIGQUIT;
    if (res->killed_by_int && !s->interactive) { /* bash, non-interactive: the foreground child died of SIGINT/SIGQUIT, so the shell ends too */
        s->exit_requested = 1;
        s->exit_status = 128 + last->termsig;
    }
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
