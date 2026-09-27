/*
 * rx_caproot.c -- host reference capability root. See rx_caproot.h.
 *
 * The writable table stays in a separate non-dumpable process. The control
 * socket is not authority. Every privileged operation must present a
 * capability this process previously handed out (the delivered set lives only
 * here, not in the readable table). R7 is not claimed: AIENOS is not this
 * process, and this Linux mint is not the permanent root.
 */
#include "rx_caproot.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef F_SEAL_FUTURE_WRITE
#define F_SEAL_FUTURE_WRITE 0x0010
#endif

typedef struct {
    int32_t status;
    uint32_t pad;
    RxCapRef ref;
} RxCapReply;

typedef struct {
    uint32_t ready;
    uint32_t office_id;
    uint32_t office_generation;
    int32_t status;
    uint8_t token[RX_CAP_TOKEN_LEN];
} RxCapHello;

/* Next table's starting generation. Taken atomically so two roots in one
 * process cannot share a generation. A later root starts higher, so a
 * reference from a dead root cannot validate against the new table. */
static _Atomic uint32_t g_next_boot_gen = 1;

static int take_boot_gen(uint32_t *out) {
    uint32_t cur = atomic_load_explicit(&g_next_boot_gen, memory_order_relaxed);
    for (;;) {
        if (cur == 0 || cur >= UINT32_MAX) return RX_CAP_ERR_EXHAUSTED;
        if (atomic_compare_exchange_weak_explicit(
                &g_next_boot_gen, &cur, cur + 1u,
                memory_order_acq_rel, memory_order_relaxed)) {
            *out = cur;
            return RX_CAP_OK;
        }
    }
}

static int fill_token(uint8_t token[RX_CAP_TOKEN_LEN]) {
    size_t n = 0;
    while (n < RX_CAP_TOKEN_LEN) {
        ssize_t r = getrandom(token + n, RX_CAP_TOKEN_LEN - n, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        n += (size_t)r;
    }
    return 0;
}

static int token_ok(const uint8_t *a, const uint8_t *b) {
    uint8_t diff = 0;
    for (uint32_t i = 0; i < RX_CAP_TOKEN_LEN; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

/* ---- root process (the only writer) ------------------------------------ */

static void root_begin(RxCapTable *t) {
    atomic_fetch_add_explicit(&t->seq, 1, memory_order_acq_rel);
    atomic_thread_fence(memory_order_release);
}

static void root_end(RxCapTable *t) {
    atomic_thread_fence(memory_order_release);
    atomic_fetch_add_explicit(&t->seq, 1, memory_order_acq_rel);
}

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

static uint32_t ancestor_hops(const RxCapTable *t, const RxCapEntry *e) {
    uint32_t hops = 0;
    while (e->parent_id != UINT32_MAX && hops <= RX_CAP_MAX_DEPTH) {
        if (e->parent_id >= t->capacity) return RX_CAP_MAX_DEPTH + 1;
        hops++;
        e = &t->entries[e->parent_id];
    }
    return hops;
}

/* Possession is the delivered bit, not a copy of an id read from the table. */
static int auth_use(RxCapTable *t, const uint8_t *delivered, RxCapRef a,
                    uint32_t need, const RxCapEntry **out) {
    if (a.cap_id >= t->capacity) return RX_CAP_ERR_UNAUTHORIZED;
    if (!delivered[a.cap_id]) return RX_CAP_ERR_UNAUTHORIZED;
    const RxCapEntry *e = &t->entries[a.cap_id];
    if (e->generation != a.generation) return RX_CAP_ERR_STALE_GEN;
    if (e->state != RX_CAP_LIVE) return RX_CAP_ERR_REVOKED;
    uint64_t epoch = atomic_load_explicit(&t->epoch, memory_order_relaxed);
    uint64_t clock = atomic_load_explicit(&t->clock, memory_order_relaxed);
    if (e->epoch != epoch) return RX_CAP_ERR_EPOCH;
    if (e->lease_expiry && clock >= e->lease_expiry) return RX_CAP_ERR_EXPIRED;
    if (chain_ok(t, e) != RX_CAP_OK) return RX_CAP_ERR_CHAIN;
    if ((e->rights & need) != need) return RX_CAP_ERR_UNAUTHORIZED;
    if (out) *out = e;
    return RX_CAP_OK;
}

static int lease_expiry(uint64_t clock, uint64_t ticks, uint64_t *out) {
    if (ticks == 0) {
        *out = 0;
        return RX_CAP_OK;
    }
    return rx_cap_add_u64(clock, ticks, out);
}

static int root_mint(RxCapTable *t, const uint8_t *delivered, uint8_t *delivered_mut,
                     const RxCapMint *m, RxCapRef *out) {
    uint64_t epoch = atomic_load_explicit(&t->epoch, memory_order_relaxed);
    uint64_t clock = atomic_load_explicit(&t->clock, memory_order_relaxed);
    if (m->rights == 0 || (m->rights & ~RX_RIGHT_KNOWN) != 0) return RX_CAP_ERR_RIGHTS;
    if ((m->rights & RX_RIGHT_PRIVILEGED) && (m->rights & RX_RIGHT_DELEGATE))
        return RX_CAP_ERR_NOT_DELEGABLE;

    const RxCapEntry *auth = NULL;
    if (m->parent.cap_id == UINT32_MAX) {
        int arc = auth_use(t, delivered, m->authority, RX_RIGHT_MINT, &auth);
        if (arc != RX_CAP_OK) return arc;
        if ((m->rights & RX_RIGHT_PRIVILEGED) & ~auth->rights)
            return RX_CAP_ERR_UNAUTHORIZED;
    } else {
        /* Delegation presents the parent. The office's MINT right does not
         * reach into someone else's capability. */
        if (m->authority.cap_id != m->parent.cap_id ||
            m->authority.generation != m->parent.generation)
            return RX_CAP_ERR_UNAUTHORIZED;
        int arc = auth_use(t, delivered, m->authority, 0, &auth);
        if (arc != RX_CAP_OK) return arc;
        if (!(auth->rights & RX_RIGHT_DELEGATE)) return RX_CAP_ERR_NOT_DELEGABLE;
        if (m->parent.cap_id >= t->capacity) return RX_CAP_ERR_BOUNDS;
        const RxCapEntry *p = &t->entries[m->parent.cap_id];
        if (p != auth) return RX_CAP_ERR_UNAUTHORIZED;
        if (p->resource != m->resource) return RX_CAP_ERR_RESOURCE;
        if ((m->rights & ~p->rights) != 0) return RX_CAP_ERR_AMPLIFY;
        if (m->rights & RX_RIGHT_PRIVILEGED) return RX_CAP_ERR_NOT_DELEGABLE;
        if (ancestor_hops(t, p) >= RX_CAP_MAX_DEPTH) return RX_CAP_ERR_CHAIN;
        if (p->lease_expiry) {
            uint64_t child = 0;
            int lrc = lease_expiry(clock, m->lease_ticks, &child);
            if (lrc != RX_CAP_OK) return lrc;
            if (child == 0 || child > p->lease_expiry) return RX_CAP_ERR_AMPLIFY;
        }
    }

    uint64_t expiry = 0;
    int lrc = lease_expiry(clock, m->lease_ticks, &expiry);
    if (lrc != RX_CAP_OK) return lrc;

    int saw_exhausted = 0;
    for (uint32_t i = 0; i < t->capacity; i++) {
        RxCapEntry *e = &t->entries[i];
        if (e->state != RX_CAP_FREE) continue;
        if (e->generation == UINT32_MAX) { saw_exhausted = 1; continue; }
        root_begin(t);
        e->state = RX_CAP_LIVE;
        e->issuer = m->issuer;
        e->subject = m->subject;
        e->resource = m->resource;
        e->rights = m->rights;
        e->epoch = epoch;
        e->lease_expiry = expiry;
        e->parent_id = m->parent.cap_id;
        e->parent_generation = m->parent.cap_id == UINT32_MAX ? 0 : m->parent.generation;
        e->minted_by_id = auth->cap_id;
        e->minted_by_generation = auth->generation;
        delivered_mut[i] = 1;
        root_end(t);
        out->cap_id = e->cap_id;
        out->generation = e->generation;
        return RX_CAP_OK;
    }
    return saw_exhausted ? RX_CAP_ERR_EXHAUSTED : RX_CAP_ERR_FULL;
}

/* A revoked parent makes every descendant unusable. Mark them revoked too,
 * the way AIENOS walks a derivation tree, so inspection does not still show
 * a live child of a dead parent. */
static void cascade_revoke(RxCapTable *t) {
    int guard = 0;
    int changed = 1;
    while (changed && guard++ < (int)RX_CAP_MAX) {
        changed = 0;
        for (uint32_t i = 0; i < t->capacity; i++) {
            RxCapEntry *e = &t->entries[i];
            if (e->state != RX_CAP_LIVE || e->parent_id == UINT32_MAX) continue;
            if (e->parent_id >= t->capacity) continue;
            const RxCapEntry *p = &t->entries[e->parent_id];
            if (p->generation == e->parent_generation && p->state == RX_CAP_REVOKED) {
                e->state = RX_CAP_REVOKED;
                changed = 1;
            }
        }
    }
}

static int root_revoke(RxCapTable *t, const uint8_t *delivered, RxCapRef authority, RxCapRef r) {
    const RxCapEntry *auth = NULL;
    int arc = auth_use(t, delivered, authority, RX_RIGHT_REVOKE, &auth);
    if (arc != RX_CAP_OK) return arc;
    if (r.cap_id >= t->capacity) return RX_CAP_ERR_BOUNDS;
    RxCapEntry *e = &t->entries[r.cap_id];
    if (e->generation != r.generation) return RX_CAP_ERR_STALE_GEN;
    if (e->state != RX_CAP_LIVE) return RX_CAP_ERR_STATE;
    /* A narrower revoker cannot strike a capability that carries privileged
     * rights the revoker does not itself hold. The office stays put. */
    if ((e->rights & RX_RIGHT_PRIVILEGED) & ~auth->rights) return RX_CAP_ERR_UNAUTHORIZED;
    root_begin(t);
    e->state = RX_CAP_REVOKED;
    cascade_revoke(t);
    root_end(t);
    return RX_CAP_OK;
}

static int root_reclaim(RxCapTable *t, const uint8_t *delivered, uint8_t *delivered_mut,
                        RxCapRef authority, uint32_t id) {
    int arc = auth_use(t, delivered, authority, RX_RIGHT_RECLAIM, NULL);
    if (arc != RX_CAP_OK) return arc;
    if (id >= t->capacity) return RX_CAP_ERR_BOUNDS;
    RxCapEntry *e = &t->entries[id];
    if (e->state != RX_CAP_REVOKED) return RX_CAP_ERR_STATE;
    uint32_t next = 0;
    int grc = rx_cap_generation_advance(e->generation, &next);
    if (grc != RX_CAP_OK) return grc;
    root_begin(t);
    e->generation = next;
    e->state = RX_CAP_FREE;
    e->rights = 0;
    e->lease_expiry = 0;
    e->parent_id = UINT32_MAX;
    e->parent_generation = 0;
    delivered_mut[id] = 0;
    root_end(t);
    return RX_CAP_OK;
}

static int root_clock(RxCapTable *t, const uint8_t *delivered, RxCapRef authority, uint64_t ticks) {
    int arc = auth_use(t, delivered, authority, RX_RIGHT_CLOCK, NULL);
    if (arc != RX_CAP_OK) return arc;
    uint64_t now = atomic_load_explicit(&t->clock, memory_order_relaxed);
    uint64_t next = 0;
    int rc = rx_cap_add_u64(now, ticks, &next);
    if (rc != RX_CAP_OK) return rc;
    root_begin(t);
    atomic_store_explicit(&t->clock, next, memory_order_relaxed);
    root_end(t);
    return RX_CAP_OK;
}

static int root_epoch(RxCapTable *t, const uint8_t *delivered, RxCapRef authority) {
    int arc = auth_use(t, delivered, authority, RX_RIGHT_EPOCH, NULL);
    if (arc != RX_CAP_OK) return arc;
    uint64_t now = atomic_load_explicit(&t->epoch, memory_order_relaxed);
    uint64_t next = 0;
    int rc = rx_cap_add_u64(now, 1, &next);
    if (rc != RX_CAP_OK) return rc;
    root_begin(t);
    atomic_store_explicit(&t->epoch, next, memory_order_relaxed);
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
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
    uint32_t boot_gen = 0;
    if (read_full(sock, &boot_gen, sizeof(boot_gen)) != 0) _exit(2);
    if (boot_gen == 0 || boot_gen == UINT32_MAX) _exit(2);

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
        t->entries[i].generation = boot_gen;
        t->entries[i].state = RX_CAP_FREE;
        t->entries[i].parent_id = UINT32_MAX;
    }

    /* Bootstrap office. Not requested over the socket. Privileged rights only,
     * no DELEGATE, so they cannot be handed on by attenuation. */
    uint8_t delivered[RX_CAP_MAX];
    memset(delivered, 0, sizeof(delivered));
    RxCapEntry *office = &t->entries[0];
    office->state = RX_CAP_LIVE;
    office->issuer = 0;
    office->subject = 0;
    office->resource = RX_CAP_RES_AUTHORITY;
    office->rights = RX_RIGHT_PRIVILEGED;
    office->epoch = 1;
    office->parent_id = UINT32_MAX;
    delivered[0] = 1;

    RxCapHello hello;
    memset(&hello, 0, sizeof(hello));
    hello.ready = 1;
    hello.office_id = office->cap_id;
    hello.office_generation = office->generation;
    hello.status = RX_CAP_OK;
    if (fill_token(hello.token) != 0) _exit(2);
    if (write_full(sock, &hello, sizeof(hello)) != 0) _exit(3);
    uint8_t token[RX_CAP_TOKEN_LEN];
    memcpy(token, hello.token, sizeof(token));

    RxCapRequest q;
    while (read_full(sock, &q, sizeof(q)) == 0) {
        RxCapReply r;
        memset(&r, 0, sizeof(r));
        if (!token_ok(q.token, token)) {
            r.status = RX_CAP_ERR_UNAUTHORIZED;
            if (write_full(sock, &r, sizeof(r)) != 0) break;
            continue;
        }
        switch (q.op) {
        case RX_OP_MINT:
            r.status = root_mint(t, delivered, delivered, &q.mint, &r.ref);
            break;
        case RX_OP_REVOKE:
            r.status = root_revoke(t, delivered, q.authority, q.ref);
            break;
        case RX_OP_RECLAIM:
            r.status = root_reclaim(t, delivered, delivered, q.authority, q.ref.cap_id);
            break;
        case RX_OP_CLOCK:
            r.status = root_clock(t, delivered, q.authority, q.arg);
            break;
        case RX_OP_EPOCH:
            r.status = root_epoch(t, delivered, q.authority);
            break;
        case RX_OP_SHUTDOWN:
            r.status = auth_use(t, delivered, q.authority, RX_RIGHT_MINT, NULL);
            if (write_full(sock, &r, sizeof(r)) != 0) _exit(4);
            if (r.status == RX_CAP_OK) _exit(0);
            continue;
        default:
            r.status = RX_CAP_ERR_STATE;
            break;
        }
        if (write_full(sock, &r, sizeof(r)) != 0) break;
    }
    _exit(0);
}

/* ---- runtime side ------------------------------------------------------- */

int rx_caproot_start(RxCapRoot *root, RxCapAdmin *admin) {
    if (!root || !admin) return RX_CAP_ERR_STATE;
    memset(root, 0, sizeof(*root));
    memset(admin, 0, sizeof(*admin));
    root->ro_fd = -1;
    admin->ctl_fd = -1;
    admin->office.cap_id = UINT32_MAX;
    uint32_t boot = 0;
    int brc = take_boot_gen(&boot);
    if (brc != RX_CAP_OK) return brc;
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
    if (write_full(sv[0], &boot, sizeof(boot)) != 0) goto fail;
    RxCapHello hello;
    memset(&hello, 0, sizeof(hello));
    if (read_full(sv[0], &hello, sizeof(hello)) != 0 || hello.ready != 1 ||
        hello.status != RX_CAP_OK)
        goto fail;
    if (fcntl(memfd, F_ADD_SEALS,
              F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_FUTURE_WRITE | F_SEAL_SEAL) != 0)
        goto fail;
    const RxCapTable *t = mmap(NULL, sizeof(RxCapTable), PROT_READ, MAP_SHARED, memfd, 0);
    if (t == MAP_FAILED) goto fail;
    if (t->magic != RX_CAP_TABLE_MAGIC) { munmap((void *)t, sizeof(RxCapTable)); goto fail; }
    root->table = t;
    root->ro_fd = memfd;
    root->root_pid = pid;
    root->running = true;
    admin->ctl_fd = sv[0];
    admin->root_pid = pid;
    admin->running = true;
    admin->office.cap_id = hello.office_id;
    admin->office.generation = hello.office_generation;
    memcpy(admin->token, hello.token, RX_CAP_TOKEN_LEN);
    return RX_CAP_OK;
fail:
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    close(memfd);
    close(sv[0]);
    return RX_CAP_ERR_IO;
}

static int request(RxCapAdmin *admin, RxCapRequest *q, RxCapReply *r) {
    if (!admin || !admin->running) return RX_CAP_ERR_IO;
    memcpy(q->token, admin->token, RX_CAP_TOKEN_LEN);
    if (write_full(admin->ctl_fd, q, sizeof(*q)) != 0) return RX_CAP_ERR_IO;
    if (read_full(admin->ctl_fd, r, sizeof(*r)) != 0) return RX_CAP_ERR_IO;
    return r->status;
}

void rx_caproot_stop(RxCapRoot *root, RxCapAdmin *admin) {
    if (!root || !admin) return;
    if (admin->running) {
        RxCapRequest q;
        memset(&q, 0, sizeof(q));
        q.op = RX_OP_SHUTDOWN;
        q.authority = admin->office;
        RxCapReply r;
        if (request(admin, &q, &r) != RX_CAP_OK && admin->root_pid > 0)
            kill(admin->root_pid, SIGKILL);
    }
    if (admin->root_pid > 0) waitpid(admin->root_pid, NULL, 0);
    if (root->table) munmap((void *)root->table, sizeof(RxCapTable));
    if (admin->ctl_fd >= 0) close(admin->ctl_fd);
    if (root->ro_fd >= 0) close(root->ro_fd);
    memset(admin->token, 0, sizeof(admin->token));
    root->table = NULL;
    root->ro_fd = -1;
    root->running = false;
    admin->ctl_fd = -1;
    admin->running = false;
}

RxCapRef rx_capadmin_office(const RxCapAdmin *admin) {
    return admin ? admin->office : (RxCapRef){ UINT32_MAX, 0 };
}

int rx_capadmin_mint(RxCapAdmin *admin, const RxCapMint *req, RxCapRef *out) {
    RxCapRequest q;
    memset(&q, 0, sizeof(q));
    q.op = RX_OP_MINT;
    q.mint = *req;
    RxCapReply r;
    int rc = request(admin, &q, &r);
    if (rc == RX_CAP_OK && out) *out = r.ref;
    return rc;
}

int rx_capadmin_revoke(RxCapAdmin *admin, RxCapRef authority, RxCapRef ref) {
    RxCapRequest q;
    memset(&q, 0, sizeof(q));
    q.op = RX_OP_REVOKE;
    q.authority = authority;
    q.ref = ref;
    RxCapReply r;
    return request(admin, &q, &r);
}

int rx_capadmin_reclaim(RxCapAdmin *admin, RxCapRef authority, uint32_t cap_id) {
    RxCapRequest q;
    memset(&q, 0, sizeof(q));
    q.op = RX_OP_RECLAIM;
    q.authority = authority;
    q.ref.cap_id = cap_id;
    RxCapReply r;
    return request(admin, &q, &r);
}

int rx_capadmin_advance_clock(RxCapAdmin *admin, RxCapRef authority, uint64_t ticks) {
    RxCapRequest q;
    memset(&q, 0, sizeof(q));
    q.op = RX_OP_CLOCK;
    q.authority = authority;
    q.arg = ticks;
    RxCapReply r;
    return request(admin, &q, &r);
}

int rx_capadmin_bump_epoch(RxCapAdmin *admin, RxCapRef authority) {
    RxCapRequest q;
    memset(&q, 0, sizeof(q));
    q.op = RX_OP_EPOCH;
    q.authority = authority;
    RxCapReply r;
    return request(admin, &q, &r);
}

static int writer_dead(const RxCapRoot *root) {
    if (!root->running || root->root_pid <= 0) return 1;
    if (kill(root->root_pid, 0) == 0) return 0;
    return errno == ESRCH;
}

int rx_caproot_inspect(const RxCapRoot *root, RxCapRef ref, RxCapEntry *out) {
    const RxCapTable *t = root->table;
    if (!t || ref.cap_id >= RX_CAP_MAX) return RX_CAP_ERR_BOUNDS;
    for (;;) {
        uint64_t s0 = atomic_load_explicit(&t->seq, memory_order_acquire);
        if (s0 & 1u) {
            if (writer_dead(root)) return RX_CAP_ERR_IO;
            continue;
        }
        RxCapEntry e;
        memcpy(&e, (const void *)&t->entries[ref.cap_id], sizeof(e));
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&t->seq, memory_order_relaxed) != s0) continue;
        if (e.generation != ref.generation) return RX_CAP_ERR_STALE_GEN;
        if (e.state == RX_CAP_FREE) return RX_CAP_ERR_STATE;
        if (out) *out = e;
        return RX_CAP_OK;
    }
}

int rx_caproot_validate(const RxCapRoot *root, RxCapRef ref,
                        uint32_t subject, uint64_t resource, uint32_t rights,
                        RxCapEntry *out_entry) {
    const RxCapTable *t = root->table;
    if (!t || ref.cap_id >= RX_CAP_MAX) return RX_CAP_ERR_BOUNDS;
    for (;;) {
        uint64_t s0 = atomic_load_explicit(&t->seq, memory_order_acquire);
        if (s0 & 1u) {
            if (writer_dead(root)) return RX_CAP_ERR_IO;
            continue;
        }
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
    case RX_CAP_ERR_UNAUTHORIZED: return "unauthorized";
    case RX_CAP_ERR_OVERFLOW: return "overflow";
    case RX_CAP_ERR_EXHAUSTED: return "generation-exhausted";
    default: return "unknown";
    }
}
