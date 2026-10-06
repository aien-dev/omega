/* R16 G6 operator entry point (docs/r16-operator-control.md). */
#include "rx_operator.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

struct RxOperator {
    RxOperatorConfig cfg;
    char sock_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int dir_fd;                  /* the validated control directory, held open */
    int lock_fd;                 /* flock on RX_OPERATOR_LOCK in it, held until close */
    struct stat sock_st, cred_st;/* what this instance created (device, inode) */
    int sock_made, cred_made;
    int listen_fd;
    int wake[2];                 /* a byte on wake[1] ends the listener */
    pthread_t thread;
    int thread_live;
    pthread_mutex_t gate;        /* held by the program while it sets the world up */
};

/* Validate an open directory descriptor: a directory owned by this uid with no
 * group or other bits. Checks the descriptor itself, not a path. */
static int dir_fd_ok(int fd, const char *dir, char *why, size_t n) {
    struct stat st;
    if (fstat(fd, &st) != 0) { snprintf(why, n, "%s: %s", dir, strerror(errno)); return -1; }
    if (!S_ISDIR(st.st_mode)) { snprintf(why, n, "%s is not a directory", dir); return -1; }
    if (st.st_uid != geteuid()) {
        snprintf(why, n, "%s is owned by uid %u, not by this program's uid %u", dir,
                 (unsigned)st.st_uid, (unsigned)geteuid());
        return -1;
    }
    if (st.st_mode & 077) {
        snprintf(why, n, "%s has mode %03o: group or other may reach it (need no bits in 077)",
                 dir, (unsigned)(st.st_mode & 0777));
        return -1;
    }
    return 0;
}

int rx_operator_dir_open(const char *dir, int create, int *fd_out, char *why, size_t n) {
    *fd_out = -1;
    if (!dir || !dir[0]) { snprintf(why, n, "no directory"); return -1; }
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT && create) {
        if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
            snprintf(why, n, "%s: cannot create: %s", dir, strerror(errno));
            return -1;
        }
        fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    }
    if (fd < 0) {
        int e = errno;
        struct stat st;   /* only to name the refusal; the decision is the failed open */
        int l = lstat(dir, &st) == 0;
        if (l && S_ISLNK(st.st_mode)) snprintf(why, n, "%s is a symlink", dir);
        else if (l && st.st_uid != geteuid())
            snprintf(why, n, "%s is owned by uid %u, not by this program's uid %u (%s)", dir,
                     (unsigned)st.st_uid, (unsigned)geteuid(), strerror(e));
        else if (e == ENOTDIR) snprintf(why, n, "%s is not a directory", dir);
        else snprintf(why, n, "%s: %s", dir, strerror(e));
        return -1;
    }
    if (dir_fd_ok(fd, dir, why, n) != 0) { close(fd); return -1; }
    *fd_out = fd;
    return 0;
}

int rx_operator_dir_check(const char *dir, int create, char *why, size_t n) {
    int fd;
    if (rx_operator_dir_open(dir, create, &fd, why, n) != 0) return -1;
    close(fd);
    return 0;
}

/* flock on RX_OPERATOR_LOCK inside the held directory `dfd`. */
static int lock_at(int dfd, const char *dir, int *lock_fd, char *why, size_t n) {
    *lock_fd = -1;
    int fd = openat(dfd, RX_OPERATOR_LOCK, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) { snprintf(why, n, "%s/%s: %s", dir, RX_OPERATOR_LOCK, strerror(errno)); return -1; }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid()) {
        snprintf(why, n, "%s/%s is not a regular file of this uid", dir, RX_OPERATOR_LOCK);
        close(fd);
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int e = errno;
        char held[32] = "";
        ssize_t r = pread(fd, held, sizeof held - 1, 0);
        held[r > 0 ? r : 0] = 0;
        held[strcspn(held, "\n")] = 0;
        if (e == EWOULDBLOCK)
            snprintf(why, n, "%s is in use by another program (pid %s)", dir, held[0] ? held : "?");
        else
            snprintf(why, n, "%s/%s: lock: %s", dir, RX_OPERATOR_LOCK, strerror(e));
        close(fd);
        return -1;
    }
    char pid[32];
    int len = snprintf(pid, sizeof pid, "%ld\n", (long)getpid());
    if (ftruncate(fd, 0) != 0 || pwrite(fd, pid, (size_t)len, 0) != len) {
        snprintf(why, n, "%s/%s: %s", dir, RX_OPERATOR_LOCK, strerror(errno));
        close(fd);
        return -1;
    }
    *lock_fd = fd;
    return 0;
}

int rx_operator_lock_dir(const char *dir, int create, int *lock_fd, char *why, size_t n) {
    int dfd;
    *lock_fd = -1;
    if (rx_operator_dir_open(dir, create, &dfd, why, n) != 0) return -1;
    int rc = lock_at(dfd, dir, lock_fd, why, n);
    close(dfd);
    return rc;
}

static void hex32(const uint8_t *b, char out[65]) {
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", b[i]);
}

static uint64_t realtime_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* The operator's copy of its credential: 0600, written whole or not at all,
 * every name relative to the held control directory. */
static int write_cred(RxOperator *op, uint32_t subject, const RxCallerCred *cred, RxCapRef cap) {
    static const char tmp[] = RX_OPERATOR_CRED ".partial";
    char secret[65], buf[1024];
    hex32(cred->secret, secret);
    int len = snprintf(buf, sizeof buf,
        "aien-operator-credential v1\nworld %s %ld %llu\nsocket %s\nsubject %u\n"
        "cred %llu %s\ncap %u %llu\n",
        op->cfg.world_key ? op->cfg.world_key : "-", (long)getpid(),
        (unsigned long long)realtime_ns(), op->sock_path, subject,
        (unsigned long long)cred->generation, secret, cap.cap_id,
        (unsigned long long)cap.generation);
    for (size_t i = 0; i < sizeof secret; i++) ((volatile char *)secret)[i] = 0;
    if (len <= 0 || (size_t)len >= sizeof buf) return -1;
    int fd = openat(op->dir_fd, tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    int rc = fd < 0 ? -1 : 0;
    if (rc == 0 && fchmod(fd, 0600) != 0) rc = -1;
    for (int off = 0; rc == 0 && off < len;) {
        ssize_t w = write(fd, buf + off, (size_t)(len - off));
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) rc = -1; else off += (int)w;
    }
    if (rc == 0 && fsync(fd) != 0) rc = -1;
    if (rc == 0 && fstat(fd, &op->cred_st) != 0) rc = -1;
    if (fd >= 0) close(fd);
    for (size_t i = 0; i < sizeof buf; i++) ((volatile char *)buf)[i] = 0;
    if (rc == 0 && renameat(op->dir_fd, tmp, op->dir_fd, RX_OPERATOR_CRED) != 0) rc = -1;
    if (rc == 0 && fsync(op->dir_fd) != 0) rc = -1;
    if (rc != 0) unlinkat(op->dir_fd, tmp, 0);
    else op->cred_made = 1;
    return rc;
}

/* Remove `name` from the held directory only if it is still the file this
 * instance made (same device and inode). */
static void unlink_own(RxOperator *op, const char *name, const struct stat *made, int *flag) {
    struct stat st;
    if (!*flag || op->dir_fd < 0) return;
    if (fstatat(op->dir_fd, name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
        st.st_dev == made->st_dev && st.st_ino == made->st_ino)
        unlinkat(op->dir_fd, name, 0);
    *flag = 0;
}

/* ---- one request ---------------------------------------------------------- */

typedef struct {
    char cmd[16];
    uint32_t subject;
    RxCallerCred cred;
    RxCapRef cap;
    int32_t reason;
    uint64_t deadline_ns;        /* CLOCK_MONOTONIC: the client has given up after this */
    int have_subject, have_gen, have_secret, have_cap, have_reason, have_deadline;
} Req;

/* A request deadline further ahead than this is refused (the client waits 60 s). */
#define RX_OPERATOR_DEADLINE_MAX_NS 120000000000ull

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Strict unsigned decimal into *out, at most `max`. */
static int udec(const char *s, uint64_t max, uint64_t *out) {
    if (!*s) return -1;
    uint64_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        uint64_t d = (uint64_t)(*s - '0');
        if (v > (max - d) / 10) return -1;
        v = v * 10 + d;
    }
    *out = v;
    return 0;
}

static int parse(char *line, Req *q) {
    memset(q, 0, sizeof *q);
    char *save = NULL;
    char *t = strtok_r(line, " ", &save);
    if (!t || strcmp(t, "aien-operator") != 0) return -1;
    if (!(t = strtok_r(NULL, " ", &save)) || strcmp(t, "v1") != 0) return -1;
    if (!(t = strtok_r(NULL, " ", &save)) || strlen(t) >= sizeof q->cmd) return -1;
    snprintf(q->cmd, sizeof q->cmd, "%s", t);
    while ((t = strtok_r(NULL, " ", &save))) {
        char *eq = strchr(t, '=');
        if (!eq) return -1;
        *eq = 0;
        const char *k = t, *v = eq + 1;
        uint64_t x, y;
        if (strcmp(k, "subject") == 0 && !q->have_subject) {
            if (udec(v, UINT32_MAX, &x) != 0) return -1;
            q->subject = (uint32_t)x; q->have_subject = 1;
        } else if (strcmp(k, "gen") == 0 && !q->have_gen) {
            if (udec(v, UINT64_MAX, &x) != 0) return -1;
            q->cred.generation = x; q->have_gen = 1;
        } else if (strcmp(k, "secret") == 0 && !q->have_secret) {
            if (strlen(v) != 2 * RX_CALLER_SECRET_LEN) return -1;
            for (unsigned i = 0; i < RX_CALLER_SECRET_LEN; i++) {
                int hi = hexval(v[2 * i]), lo = hexval(v[2 * i + 1]);
                if (hi < 0 || lo < 0) return -1;
                q->cred.secret[i] = (uint8_t)(hi << 4 | lo);
            }
            q->have_secret = 1;
        } else if (strcmp(k, "cap") == 0 && !q->have_cap) {
            char a[24];
            const char *c = strchr(v, ':');
            if (!c || (size_t)(c - v) >= sizeof a) return -1;
            memcpy(a, v, (size_t)(c - v));
            a[c - v] = 0;
            if (udec(a, UINT32_MAX, &x) != 0 || udec(c + 1, UINT64_MAX, &y) != 0) return -1;
            q->cap = (RxCapRef){(uint32_t)x, y}; q->have_cap = 1;
        } else if (strcmp(k, "reason") == 0 && !q->have_reason) {
            if (udec(v, INT32_MAX, &x) != 0) return -1;
            q->reason = (int32_t)x; q->have_reason = 1;
        } else if (strcmp(k, "deadline") == 0 && !q->have_deadline) {
            if (udec(v, UINT64_MAX, &x) != 0) return -1;
            q->deadline_ns = x; q->have_deadline = 1;
        } else {
            return -1;
        }
    }
    if (!q->have_subject || !q->have_gen || !q->have_secret || !q->have_cap || !q->have_deadline)
        return -1;
    if (q->deadline_ns > monotonic_ns() + RX_OPERATOR_DEADLINE_MAX_NS) return -1;
    if (q->have_reason && strcmp(q->cmd, "stop") != 0) return -1;
    return 0;
}

static const char *refusal(int rc) {
    return rc == RX_ERR_IDENTITY ? "identity" : rc == RX_ERR_AUTHORITY ? "authority" : NULL;
}

static void handle(RxOperator *op, Req *q, char *out, size_t n) {
    RxWorld *w = op->cfg.world;
    RxHaltStatus h;
    memset(&h, 0, sizeof h);
    int stop = strcmp(q->cmd, "stop") == 0, resume = strcmp(q->cmd, "resume") == 0;
    int rc;
    if (stop) {
        rc = rx_world_emergency_stop(w, q->subject, &q->cred, q->cap, q->reason, &h);
        if (rc == RX_OK) {
            if (op->cfg.on_halt) op->cfg.on_halt(op->cfg.ctx, 1);
            if (h.durable == 1)
                snprintf(out, n, "OK state=stopped seq=%llu crumb=%llu durable=1",
                         (unsigned long long)h.seq, (unsigned long long)h.crumb);
            else   /* in force in memory; a restart would NOT come up stopped */
                snprintf(out, n, "STOPPED_NOT_DURABLE state=stopped seq=%llu crumb=%llu durable=%d "
                         "errno=%d (%s)", (unsigned long long)h.seq, (unsigned long long)h.crumb,
                         h.durable, h.durable < 0 ? -h.durable : 0,
                         h.durable < 0 ? strerror(-h.durable) : "no durable halt directory");
        } else if (rc == RX_HALT_ALREADY) {
            snprintf(out, n, "ALREADY state=stopped seq=%llu", (unsigned long long)h.seq);
        } else if (refusal(rc)) {
            snprintf(out, n, "REFUSED reason=%s", refusal(rc));
        } else {
            snprintf(out, n, "ERROR rc=%d", rc);
        }
        return;
    }
    if (resume) {
        rc = rx_world_emergency_resume(w, q->subject, &q->cred, q->cap, &h);
        if (rc == RX_OK) {
            if (op->cfg.on_halt) op->cfg.on_halt(op->cfg.ctx, 0);
            snprintf(out, n, "OK state=running seq=%llu", (unsigned long long)h.seq);
        } else if (rc == RX_HALT_NOT_STOPPED) {
            snprintf(out, n, "NOT_STOPPED state=running");
        } else if (refusal(rc)) {
            snprintf(out, n, "REFUSED reason=%s", refusal(rc));
        } else {
            snprintf(out, n, "ERROR rc=%d", rc);
        }
        return;
    }
    int status = strcmp(q->cmd, "status") == 0, shutdown = strcmp(q->cmd, "shutdown") == 0;
    int revoke = strcmp(q->cmd, "revoke") == 0, revoke_cap = strcmp(q->cmd, "revoke-cap") == 0;
    if (!status && !shutdown && !revoke && !revoke_cap) { snprintf(out, n, "BAD_REQUEST"); return; }
    rc = rx_world_operator_authorize(w, q->subject, &q->cred, q->cap);
    if (rc != RX_OK) {
        if (refusal(rc)) snprintf(out, n, "REFUSED reason=%s", refusal(rc));
        else snprintf(out, n, "ERROR rc=%d", rc);
        return;
    }
    rx_world_halt_status(w, &h);
    if (status) {
        int used = snprintf(out, n, "OK state=%s restored=%d seq=%llu durable=%d refused=%llu "
                            "cancelled=%llu crumbs=%llu reactions=%u",
                            h.halted ? "stopped" : "running", h.restored ? 1 : 0,
                            (unsigned long long)h.seq, h.durable, (unsigned long long)h.refused,
                            (unsigned long long)h.cancelled,
                            (unsigned long long)__atomic_load_n(&w->n_crumbs, __ATOMIC_RELAXED),
                            (unsigned)__atomic_load_n(&w->n_reactions, __ATOMIC_RELAXED));
        if (op->cfg.describe && used > 0 && (size_t)used + 2 < n) {
            out[used++] = ' ';
            op->cfg.describe(op->cfg.ctx, out + used, n - (size_t)used);
        }
        return;
    }
    if (shutdown) {
        if (!h.halted) { snprintf(out, n, "NOT_STOPPED state=running"); return; }
        if (op->cfg.on_shutdown) op->cfg.on_shutdown(op->cfg.ctx);
        snprintf(out, n, "OK state=stopped shutdown=1");
        return;
    }
    /* revoke-cap / revoke: the authority's office voids the control
     * capability; revoke also retires the caller enrollment (it needs the
     * current credential, so only its holder can). */
    AienosCapRef office;
    if (aienos_cap_office(op->cfg.admin, &office) != 0 ||
        aienos_cap_revoke(op->cfg.admin, office,
                          (AienosCapRef){q->cap.cap_id, q->cap.generation}) != 0) {
        snprintf(out, n, "ERROR rc=revoke-capability");
        return;
    }
    if (revoke) {
        rc = rx_world_revoke_caller(w, q->subject, &q->cred);
        if (rc != RX_CALLER_OK) { snprintf(out, n, "ERROR rc=revoke-credential %d", rc); return; }
        unlink_own(op, RX_OPERATOR_CRED, &op->cred_st, &op->cred_made);
        snprintf(out, n, "OK revoked=capability,credential");
        return;
    }
    snprintf(out, n, "OK revoked=capability");
}

static void serve_one(RxOperator *op, int fd) {
    char line[RX_OPERATOR_LINE], body[RX_OPERATOR_LINE + 512];
    struct ucred pc;
    socklen_t pl = sizeof pc;
    struct timeval tv = {2, 0};
    /* Without both timeouts one client could hold the listener: refused, no reply. */
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) != 0)
        return;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &pc, &pl) != 0 || pl != sizeof pc ||
        pc.uid != geteuid()) {
        snprintf(body, sizeof body, "REFUSED reason=peer");
        goto reply;
    }
    size_t got = 0;
    int complete = 0;
    while (got < sizeof line - 1) {
        ssize_t r = read(fd, line + got, sizeof line - 1 - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
        char *nl = memchr(line, '\n', got);
        if (nl) {
            size_t len = (size_t)(nl - line);
            *nl = 0;
            /* nothing after the newline, no NUL byte inside the line */
            complete = len == got - 1 && strlen(line) == len;
            break;
        }
    }
    Req q;
    memset(&q, 0, sizeof q);
    if (!complete || parse(line, &q) != 0) {
        snprintf(body, sizeof body, "BAD_REQUEST");
    } else {
        pthread_mutex_lock(&op->gate);
        /* A request waits here while the program sets a world up. Once its
         * client has given up (deadline passed) it is dropped, never run late:
         * a late resume would restart a world nobody is watching. A late stop
         * is fail-safe and still runs. */
        if (strcmp(q.cmd, "stop") != 0 && monotonic_ns() > q.deadline_ns)
            snprintf(body, sizeof body, "EXPIRED (the client deadline passed; nothing done)");
        else
            handle(op, &q, body, sizeof body);
        pthread_mutex_unlock(&op->gate);
    }
    for (size_t i = 0; i < sizeof q; i++) ((volatile uint8_t *)&q)[i] = 0;
    for (size_t i = 0; i < sizeof line; i++) ((volatile char *)line)[i] = 0;
reply:;
    char out[sizeof body + 32];
    int len = snprintf(out, sizeof out, "aien-operator v1 %s\n", body);
    if (len > 0) {
        size_t sent = 0;
        while (sent < (size_t)len) {
            ssize_t s = send(fd, out + sent, (size_t)len - sent, MSG_NOSIGNAL);
            if (s < 0 && errno == EINTR) continue;
            if (s <= 0) break;
            sent += (size_t)s;
        }
    }
    /* Read what the client still sends (a refusal is answered before its request is
     * read) until it closes its side, so closing here never resets a client that is
     * still writing. Bounded: a few reads, each under the 2 s receive timeout. */
    shutdown(fd, SHUT_WR);
    char sink[RX_OPERATOR_LINE];
    for (int i = 0; i < 4;) {
        ssize_t r = read(fd, sink, sizeof sink);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        i++;
    }
}

/* The listener: one connection at a time until the wake pipe fires. It
 * sequences no faculty; it carries operator requests to the world. */
static void *listener_main(void *arg) {
    RxOperator *op = arg;
    for (;;) {
        struct pollfd p[2] = {{op->listen_fd, POLLIN, 0}, {op->wake[0], POLLIN, 0}};
        int r = poll(p, 2, -1);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 || (p[1].revents & (POLLIN | POLLHUP | POLLERR))) break;
        if (!(p[0].revents & POLLIN)) continue;
        int fd = accept4(op->listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0) continue;
        serve_one(op, fd);
        close(fd);
    }
    return NULL;
}

int rx_operator_open(RxOperator **out, const RxOperatorConfig *cfg, uint32_t subject,
                     RxCallerCred *cred, RxCapRef cap) {
    if (!out || !cfg || !cfg->world || !cfg->admin || !cfg->control_dir || !cred) return -1;
    *out = NULL;
    char why[320];
    RxOperator *op = calloc(1, sizeof *op);
    if (!op) return -1;
    op->cfg = *cfg;
    op->dir_fd = op->lock_fd = op->listen_fd = -1;
    op->wake[0] = op->wake[1] = -1;
    pthread_mutex_init(&op->gate, NULL);
    pthread_mutex_lock(&op->gate);          /* held until rx_operator_release */
    /* The control directory is held open and locked: from here every name in
     * it is resolved against this descriptor, and no other program can be
     * serving or replacing the socket and the credential. */
    if (rx_operator_dir_open(cfg->control_dir, 0, &op->dir_fd, why, sizeof why) != 0 ||
        lock_at(op->dir_fd, cfg->control_dir, &op->lock_fd, why, sizeof why) != 0) {
        fprintf(stderr, "R13 operator: control directory refused: %s\n", why);
        goto fail;
    }
    char bind_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    if ((size_t)snprintf(op->sock_path, sizeof op->sock_path, "%s/%s", cfg->control_dir,
                         RX_OPERATOR_SOCK) >= sizeof op->sock_path ||
        (size_t)snprintf(bind_path, sizeof bind_path, "/proc/self/fd/%d/%s", op->dir_fd,
                         RX_OPERATOR_SOCK) >= sizeof bind_path) {
        fprintf(stderr, "R13 operator: control path too long for a socket address\n");
        goto fail;
    }
    /* A socket left by an earlier start of this program (the lock is ours). */
    if (unlinkat(op->dir_fd, RX_OPERATOR_SOCK, 0) != 0 && errno != ENOENT) {
        fprintf(stderr, "R13 operator: stale socket: %s\n", strerror(errno));
        goto fail;
    }
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    memcpy(a.sun_path, bind_path, strlen(bind_path) + 1);   /* lands in the held directory */
    op->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    mode_t old = umask(077);
    int ok = op->listen_fd >= 0 && bind(op->listen_fd, (struct sockaddr *)&a, sizeof a) == 0;
    umask(old);
    if (ok && fstatat(op->dir_fd, RX_OPERATOR_SOCK, &op->sock_st, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISSOCK(op->sock_st.st_mode))
        op->sock_made = 1;
    else
        ok = 0;
    ok = ok && fchmodat(op->dir_fd, RX_OPERATOR_SOCK, 0600, 0) == 0 &&
         listen(op->listen_fd, 8) == 0 && pipe2(op->wake, O_CLOEXEC) == 0 &&
         write_cred(op, subject, cred, cap) == 0;
    for (size_t i = 0; i < sizeof *cred; i++) ((volatile uint8_t *)cred)[i] = 0;
    if (ok && pthread_create(&op->thread, NULL, listener_main, op) == 0) {
        op->thread_live = 1;
        *out = op;
        return 0;
    }
    fprintf(stderr, "R13 operator: entry point could not start: %s\n", strerror(errno));
fail:
    for (size_t i = 0; i < sizeof *cred; i++) ((volatile uint8_t *)cred)[i] = 0;
    pthread_mutex_unlock(&op->gate);
    rx_operator_close(op);
    return -1;
}

void rx_operator_hold(RxOperator *op) { if (op) pthread_mutex_lock(&op->gate); }
void rx_operator_release(RxOperator *op) { if (op) pthread_mutex_unlock(&op->gate); }

void rx_operator_close(RxOperator *op) {
    if (!op) return;
    if (op->thread_live) {
        char b = 1;
        while (write(op->wake[1], &b, 1) < 0 && errno == EINTR) {}
        pthread_join(op->thread, NULL);
    }
    if (op->listen_fd >= 0) close(op->listen_fd);
    if (op->wake[0] >= 0) close(op->wake[0]);
    if (op->wake[1] >= 0) close(op->wake[1]);
    /* Only what this instance created, while its lock is still held. */
    unlink_own(op, RX_OPERATOR_SOCK, &op->sock_st, &op->sock_made);
    unlink_own(op, RX_OPERATOR_CRED, &op->cred_st, &op->cred_made);
    if (op->lock_fd >= 0) close(op->lock_fd);   /* releases the flock */
    if (op->dir_fd >= 0) close(op->dir_fd);
    pthread_mutex_destroy(&op->gate);
    free(op);
}
