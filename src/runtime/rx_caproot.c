/*
 * rx_caproot.c -- host reference capability root. See rx_caproot.h.
 */
#include "rx_caproot.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef F_SEAL_FUTURE_WRITE
#define F_SEAL_FUTURE_WRITE 0x0010
#endif

enum {
    RX_OP_MINT = 1,
    RX_OP_REVOKE,
    RX_OP_RECLAIM,
    RX_OP_CLOCK,
    RX_OP_EPOCH,
    RX_OP_SHUTDOWN,
};

typedef struct {
    uint32_t op;
    uint32_t pad;
    RxCapMint mint;
    RxCapRef ref;
    uint64_t arg;
} RxCapRequest;

typedef struct {
    int32_t status;
    uint32_t pad;
    RxCapRef ref;
} RxCapReply;

/* ---- root process (the only writer) ------------------------------------ */

static void root_begin(RxCapTable *t) {
    atomic_fetch_add_explicit(&t->seq, 1, memory_order_acq_rel);
    atomic_thread_fence(memory_order_release);
}

static void root_end(RxCapTable *t) {
    atomic_thread_fence(memory_order_release);
    atomic_fetch_add_explicit(&t->seq, 1, memory_order_acq_rel);
}

/* Chain validation used by both sides; `t` must be a stable view. */
static int chain_ok(const RxCapTable *t, const RxCapEntry *e) {
    uint32_t depth = 0;
    while (e->parent_id != UINT32_MAX) {
        if (++depth > RX_CAP_MAX_DEPTH) return RX_CAP_ERR_CHAIN;
        if (e->parent_id >= t->capacity) return RX_CAP_ERR_CHAIN;
        const RxCapEntry *p = &t->entries[e->parent_id];
        if (p->generation != e->parent_generation) return RX_CAP_ERR_CHAIN;
        if (p->state != RX_CAP_LIVE) return RX_CAP_ERR_CHAIN;
        e = p;
    }
    return RX_CAP_OK;
}

static int root_mint(RxCapTable *t, const RxCapMint *m, RxCapRef *out) {
    uint64_t epoch = atomic_load_explicit(&t->epoch, memory_order_relaxed);
    uint64_t clock = atomic_load_explicit(&t->clock, memory_order_relaxed);
    if (m->rights == 0) return RX_CAP_ERR_RIGHTS;
    if (m->parent.cap_id != UINT32_MAX) {
        if (m->parent.cap_id >= t->capacity) return RX_CAP_ERR_BOUNDS;
        const RxCapEntry *p = &t->entries[m->parent.cap_id];
        if (p->generation != m->parent.generation) return RX_CAP_ERR_STALE_GEN;
        if (p->state != RX_CAP_LIVE) return RX_CAP_ERR_REVOKED;
        if (p->epoch != epoch) return RX_CAP_ERR_EPOCH;
        if (p->lease_expiry && clock >= p->lease_expiry) return RX_CAP_ERR_EXPIRED;
        if (chain_ok(t, p) != RX_CAP_OK) return RX_CAP_ERR_CHAIN;
        if (!(p->rights & RX_RIGHT_DELEGATE)) return RX_CAP_ERR_NOT_DELEGABLE;
        if (p->resource != m->resource) return RX_CAP_ERR_RESOURCE;
        if ((m->rights & ~p->rights) != 0) return RX_CAP_ERR_AMPLIFY;
        if (p->lease_expiry &&
            (m->lease_ticks == 0 || clock + m->lease_ticks > p->lease_expiry))
            return RX_CAP_ERR_AMPLIFY; /* a child may not outlive its parent */
    }
    for (uint32_t i = 0; i < t->capacity; i++) {
        RxCapEntry *e = &t->entries[i];
        if (e->state != RX_CAP_FREE) continue;
        root_begin(t);
        e->state = RX_CAP_LIVE;
        e->issuer = m->issuer;
        e->subject = m->subject;
        e->resource = m->resource;
        e->rights = m->rights;
        e->epoch = epoch;
        e->lease_expiry = m->lease_ticks ? clock + m->lease_ticks : 0;
        e->parent_id = m->parent.cap_id;
        e->parent_generation = m->parent.cap_id == UINT32_MAX ? 0 : m->parent.generation;
        root_end(t);
        out->cap_id = e->cap_id;
        out->generation = e->generation;
        return RX_CAP_OK;
    }
    return RX_CAP_ERR_FULL;
}

static int root_revoke(RxCapTable *t, RxCapRef r) {
    if (r.cap_id >= t->capacity) return RX_CAP_ERR_BOUNDS;
    RxCapEntry *e = &t->entries[r.cap_id];
    if (e->generation != r.generation) return RX_CAP_ERR_STALE_GEN;
    if (e->state != RX_CAP_LIVE) return RX_CAP_ERR_STATE;
    root_begin(t);
    e->state = RX_CAP_REVOKED;
    root_end(t);
    return RX_CAP_OK;
}

static int root_reclaim(RxCapTable *t, uint32_t id) {
    if (id >= t->capacity) return RX_CAP_ERR_BOUNDS;
    RxCapEntry *e = &t->entries[id];
    if (e->state != RX_CAP_REVOKED) return RX_CAP_ERR_STATE;
    root_begin(t);
    e->generation++;
    e->state = RX_CAP_FREE;
    root_end(t);
    return RX_CAP_OK;
}

static int read_full(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static void root_main(int memfd, int sock) {
    /* Non-dumpable: the parent (same uid) may not ptrace this process or open
     * its /proc fds, so the writable mapping stays private to the root. */
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
    RxCapTable *t = mmap(NULL, sizeof(RxCapTable), PROT_READ | PROT_WRITE,
                         MAP_SHARED, memfd, 0);
    close(memfd);
    if (t == MAP_FAILED) _exit(2);
    memset(t, 0, sizeof(*t));
    t->magic = RX_CAP_TABLE_MAGIC;
    t->capacity = RX_CAP_MAX;
    atomic_store(&t->epoch, 1);
    for (uint32_t i = 0; i < RX_CAP_MAX; i++) {
        t->entries[i].cap_id = i;
        t->entries[i].generation = 1;
        t->entries[i].state = RX_CAP_FREE;
        t->entries[i].parent_id = UINT32_MAX;
    }
    uint8_t ready = 1;
    if (write_full(sock, &ready, 1) != 0) _exit(3);

    RxCapRequest q;
    while (read_full(sock, &q, sizeof(q)) == 0) {
        RxCapReply r;
        memset(&r, 0, sizeof(r));
        switch (q.op) {
        case RX_OP_MINT:    r.status = root_mint(t, &q.mint, &r.ref); break;
        case RX_OP_REVOKE:  r.status = root_revoke(t, q.ref); break;
        case RX_OP_RECLAIM: r.status = root_reclaim(t, q.ref.cap_id); break;
        case RX_OP_CLOCK:
            root_begin(t);
            atomic_fetch_add(&t->clock, q.arg);
            root_end(t);
            break;
        case RX_OP_EPOCH:
            root_begin(t);
            atomic_fetch_add(&t->epoch, 1);
            root_end(t);
            break;
        case RX_OP_SHUTDOWN:
            write_full(sock, &r, sizeof(r));
            _exit(0);
        default: r.status = RX_CAP_ERR_STATE; break;
        }
        if (write_full(sock, &r, sizeof(r)) != 0) break;
    }
    _exit(0);
}

/* ---- runtime side ------------------------------------------------------- */

int rx_caproot_start(RxCapRoot *root) {
    memset(root, 0, sizeof(*root));
    root->ctl_fd = root->ro_fd = -1;
    int memfd = memfd_create("aien-capability-root", MFD_ALLOW_SEALING | MFD_CLOEXEC);
    if (memfd < 0) return RX_CAP_ERR_IO;
    if (ftruncate(memfd, sizeof(RxCapTable)) != 0) { close(memfd); return RX_CAP_ERR_IO; }
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
        close(memfd);
        return RX_CAP_ERR_IO;
    }
    pid_t pid = fork();
    if (pid < 0) { close(memfd); close(sv[0]); close(sv[1]); return RX_CAP_ERR_IO; }
    if (pid == 0) {
        close(sv[0]);
        root_main(memfd, sv[1]);
    }
    close(sv[1]);
    uint8_t ready = 0;
    if (read_full(sv[0], &ready, 1) != 0 || ready != 1) goto fail;
    /* The root holds the only writable mapping. Seal the inode so nothing else
     * can ever obtain one, then map it read-only for this process. */
    if (fcntl(memfd, F_ADD_SEALS,
              F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_FUTURE_WRITE | F_SEAL_SEAL) != 0)
        goto fail;
    const RxCapTable *t = mmap(NULL, sizeof(RxCapTable), PROT_READ, MAP_SHARED, memfd, 0);
    if (t == MAP_FAILED) goto fail;
    if (t->magic != RX_CAP_TABLE_MAGIC) { munmap((void *)t, sizeof(RxCapTable)); goto fail; }
    root->table = t;
    root->ctl_fd = sv[0];
    root->ro_fd = memfd;
    root->root_pid = pid;
    root->running = true;
    return RX_CAP_OK;
fail:
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    close(memfd);
    close(sv[0]);
    return RX_CAP_ERR_IO;
}

static int request(RxCapRoot *root, RxCapRequest *q, RxCapReply *r) {
    if (!root->running) return RX_CAP_ERR_IO;
    if (write_full(root->ctl_fd, q, sizeof(*q)) != 0) return RX_CAP_ERR_IO;
    if (read_full(root->ctl_fd, r, sizeof(*r)) != 0) return RX_CAP_ERR_IO;
    return r->status;
}

void rx_caproot_stop(RxCapRoot *root) {
    if (!root->running) return;
    RxCapRequest q = { .op = RX_OP_SHUTDOWN };
    RxCapReply r;
    request(root, &q, &r);
    waitpid(root->root_pid, NULL, 0);
    munmap((void *)root->table, sizeof(RxCapTable));
    close(root->ctl_fd);
    close(root->ro_fd);
    root->running = false;
}

int rx_caproot_mint(RxCapRoot *root, const RxCapMint *req, RxCapRef *out) {
    RxCapRequest q;
    memset(&q, 0, sizeof(q));
    q.op = RX_OP_MINT;
    q.mint = *req;
    RxCapReply r;
    int rc = request(root, &q, &r);
    if (rc == RX_CAP_OK && out) *out = r.ref;
    return rc;
}

int rx_caproot_revoke(RxCapRoot *root, RxCapRef ref) {
    RxCapRequest q;
    memset(&q, 0, sizeof(q));
    q.op = RX_OP_REVOKE;
    q.ref = ref;
    RxCapReply r;
    return request(root, &q, &r);
}

int rx_caproot_reclaim(RxCapRoot *root, uint32_t cap_id) {
    RxCapRequest q;
    memset(&q, 0, sizeof(q));
    q.op = RX_OP_RECLAIM;
    q.ref.cap_id = cap_id;
    RxCapReply r;
    return request(root, &q, &r);
}

int rx_caproot_advance_clock(RxCapRoot *root, uint64_t ticks) {
    RxCapRequest q;
    memset(&q, 0, sizeof(q));
    q.op = RX_OP_CLOCK;
    q.arg = ticks;
    RxCapReply r;
    return request(root, &q, &r);
}

int rx_caproot_bump_epoch(RxCapRoot *root) {
    RxCapRequest q;
    memset(&q, 0, sizeof(q));
    q.op = RX_OP_EPOCH;
    RxCapReply r;
    return request(root, &q, &r);
}

int rx_caproot_validate(const RxCapRoot *root, RxCapRef ref,
                        uint32_t subject, uint64_t resource, uint32_t rights,
                        RxCapEntry *out_entry) {
    const RxCapTable *t = root->table;
    if (!t || ref.cap_id >= RX_CAP_MAX) return RX_CAP_ERR_BOUNDS;
    for (;;) {
        uint64_t s0 = atomic_load_explicit(&t->seq, memory_order_acquire);
        if (s0 & 1u) continue;
        /* Copy the chain we need into a private view, then check the seqlock. */
        RxCapEntry chain[RX_CAP_MAX_DEPTH + 1];
        uint32_t n = 0;
        uint32_t id = ref.cap_id;
        int rc = RX_CAP_OK;
        while (id != UINT32_MAX) {
            if (n > RX_CAP_MAX_DEPTH || id >= RX_CAP_MAX) { rc = RX_CAP_ERR_CHAIN; break; }
            memcpy(&chain[n], (const void *)&t->entries[id], sizeof(RxCapEntry));
            id = chain[n].parent_id;
            n++;
        }
        uint64_t epoch = atomic_load_explicit(&t->epoch, memory_order_relaxed);
        uint64_t clock = atomic_load_explicit(&t->clock, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&t->seq, memory_order_relaxed) != s0) continue;
        if (rc != RX_CAP_OK) return rc;

        const RxCapEntry *e = &chain[0];
        if (e->generation != ref.generation) return RX_CAP_ERR_STALE_GEN;
        if (e->state != RX_CAP_LIVE) return RX_CAP_ERR_REVOKED;
        if (e->epoch != epoch) return RX_CAP_ERR_EPOCH;
        if (e->lease_expiry && clock >= e->lease_expiry) return RX_CAP_ERR_EXPIRED;
        for (uint32_t i = 1; i < n; i++) {
            if (chain[i].generation != chain[i - 1].parent_generation) return RX_CAP_ERR_CHAIN;
            if (chain[i].state != RX_CAP_LIVE) return RX_CAP_ERR_CHAIN;
            if (chain[i].epoch != epoch) return RX_CAP_ERR_CHAIN;
            if (chain[i].lease_expiry && clock >= chain[i].lease_expiry) return RX_CAP_ERR_CHAIN;
        }
        if (e->subject != subject) return RX_CAP_ERR_SUBJECT;
        if (e->resource != resource) return RX_CAP_ERR_RESOURCE;
        if ((e->rights & rights) != rights) return RX_CAP_ERR_RIGHTS;
        if (out_entry) *out_entry = *e;
        return RX_CAP_OK;
    }
}

const char *rx_cap_strerror(int code) {
    switch (code) {
    case RX_CAP_OK: return "ok";
    case RX_CAP_ERR_BOUNDS: return "bounds";
    case RX_CAP_ERR_STALE_GEN: return "stale-generation";
    case RX_CAP_ERR_REVOKED: return "revoked";
    case RX_CAP_ERR_EPOCH: return "epoch";
    case RX_CAP_ERR_SUBJECT: return "wrong-subject";
    case RX_CAP_ERR_RESOURCE: return "wrong-resource";
    case RX_CAP_ERR_RIGHTS: return "insufficient-rights";
    case RX_CAP_ERR_EXPIRED: return "lease-expired";
    case RX_CAP_ERR_CHAIN: return "delegation-chain-invalid";
    case RX_CAP_ERR_AMPLIFY: return "rights-amplification";
    case RX_CAP_ERR_NOT_DELEGABLE: return "not-delegable";
    case RX_CAP_ERR_FULL: return "table-full";
    case RX_CAP_ERR_IO: return "root-io";
    case RX_CAP_ERR_STATE: return "bad-state";
    default: return "unknown";
    }
}
