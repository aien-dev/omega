/*
 * osh_journal.c -- durable effect journal (OSH_PLATFORM_ABI.md section 10). Format and rules: osh_journal.h.
 * Plain POSIX: open(O_APPEND), one write() per batch, fsync(file), fsync(directory) when the file is created.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "osh_journal.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sha256.h"

static const char *const k_outcome[] = {"NOT_STARTED", "COMPLETED", "FAILED_NO_EFFECT", "CANCELLED", "OUTCOME_UNKNOWN"};

/* ---- redaction ---- */

/* The ABI draft names "a secret list" (section 13) but does not define it. ASSUMPTION: a variable NAME is secret when,
 * compared case-insensitively, it contains any of these fragments. False positives only blank a value inside a digest. */
int osh_secret_name(const char *name)
{
    static const char *const frag[] = {"PASSWORD", "PASSWD", "PASSPHRASE", "SECRET", "TOKEN", "CREDENTIAL", "APIKEY",
                                       "API_KEY", "PRIVATE", "AUTH", "COOKIE", "KEY", "SESSION", "SIGNATURE"};
    size_t n = strlen(name);
    for (size_t f = 0; f < sizeof frag / sizeof frag[0]; f++) {
        size_t fl = strlen(frag[f]);
        for (size_t i = 0; i + fl <= n; i++) {
            size_t k = 0;
            while (k < fl && toupper((unsigned char)name[i + k]) == frag[f][k]) k++;
            if (k == fl) return 1;
        }
    }
    return 0;
}

/* ---- request digest ---- */

static void h_u64(sha256_ctx *h, uint64_t v)
{
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
    sha256_update(h, b, 8);
}

static void h_str(sha256_ctx *h, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    h_u64(h, n);
    if (n) sha256_update(h, (const uint8_t *)s, n);
}

/* Canonical encoding: domain tag, ncmds, connector, flags, then per command the builtin id, argv, assignments
 * (name, value; a secret name's value is replaced by the marker "\x01REDACTED"), redirections. Lengths are prefixed. */
static void canon(sha256_ctx *hp, const OshRequest *r)
{
    sha256_ctx h = *hp;
    h_str(&h, "OSHREQ1");
    h_u64(&h, (uint64_t)r->ncmds);
    h_u64(&h, (uint64_t)r->connector_after);
    h_u64(&h, r->flags);
    for (int i = 0; i < r->ncmds; i++) {
        const OshCmd *c = &r->cmd[i];
        h_u64(&h, (uint64_t)c->builtin_id);
        h_u64(&h, (uint64_t)c->nargv);
        for (int k = 0; k < c->nargv; k++) h_str(&h, c->argv[k]);
        h_u64(&h, (uint64_t)c->nassign);
        for (int k = 0; k < c->nassign; k++) {
            h_str(&h, c->assign[k].name);
            h_str(&h, osh_secret_name(c->assign[k].name) ? "\x01REDACTED" : c->assign[k].value);
        }
        h_u64(&h, (uint64_t)c->nredir);
        for (int k = 0; k < c->nredir; k++) {
            h_u64(&h, (uint64_t)c->redir[k].kind);
            h_u64(&h, (uint64_t)c->redir[k].fd);
            h_u64(&h, (uint64_t)c->redir[k].src_fd);
            h_str(&h, c->redir[k].path);
        }
    }
    *hp = h;
}

void osh_req_digest_unkeyed(const OshRequest *r, uint8_t out[32])
{
    sha256_ctx h;
    sha256_init(&h);
    canon(&h, r);
    sha256_final(&h, out);
}

/* HMAC-SHA256 (RFC 2104) over omega's src/sha256.c, key 32 bytes. (src/fabric/fab_hmac.c is a test-only stand-in that refuses to
 * build in a production program, so it is not used.) */
static void hmac_begin(sha256_ctx *h, const uint8_t key[32])
{
    uint8_t pad[64];
    memset(pad, 0x36, sizeof pad);
    for (int i = 0; i < 32; i++) pad[i] ^= key[i];
    sha256_init(h);
    sha256_update(h, pad, sizeof pad);
}

static void hmac_end(sha256_ctx *h, const uint8_t key[32], uint8_t out[32])
{
    uint8_t pad[64], inner[32];
    sha256_final(h, inner);
    memset(pad, 0x5c, sizeof pad);
    for (int i = 0; i < 32; i++) pad[i] ^= key[i];
    sha256_init(h);
    sha256_update(h, pad, sizeof pad);
    sha256_update(h, inner, sizeof inner);
    sha256_final(h, out);
}

void osh_req_digest(const OshJournal *j, const OshRequest *r, uint8_t out[32])
{
    sha256_ctx h;
    hmac_begin(&h, j->key);
    canon(&h, r);
    hmac_end(&h, j->key, out);
}

#ifdef OSH_JOURNAL_TEST_HOOKS
/* the same HMAC, over a byte string, so the test can check RFC 4231 vectors (key given zero-extended to 32 bytes, which HMAC treats as the shorter key) */
void osh_hmac_for_test(const uint8_t key[32], const uint8_t *msg, size_t n, uint8_t out[32])
{
    sha256_ctx h;
    hmac_begin(&h, key);
    sha256_update(&h, msg, n);
    hmac_end(&h, key, out);
}
#endif

static void hex(const uint8_t d[32], char out[65])
{
    static const char x[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i] = x[d[i] >> 4];
        out[2 * i + 1] = x[d[i] & 15];
    }
    out[64] = 0;
}

/* ---- low-level io ---- */

static int write_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int sync_fd(int fd)
{
    for (;;) {
        if (fsync(fd) == 0) return 0;
        if (errno != EINTR) return -1;
    }
}

static int fsync_parent(const char *path)
{
    char d[4096];
    if (strlen(path) >= sizeof d) return -ENAMETOOLONG;
    strcpy(d, path);
    char *sl = strrchr(d, '/');
    if (!sl) strcpy(d, ".");
    else if (sl == d) d[1] = 0;
    else *sl = 0;
    int fd = open(d, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -errno;
    int rc = sync_fd(fd) == 0 ? 0 : -errno;
    close(fd);
    return rc;
}

/* the journal's own fsync; the test build can make one fail (the tests also count real fsync(2) calls) */
static int jsync(OshJournal *j)
{
#ifdef OSH_JOURNAL_TEST_HOOKS
    if (j->fail_fsync_at && j->n_commit == j->fail_fsync_at) { errno = EIO; return -1; }
#endif
    return sync_fd(j->fd);
}

/* Append one batch and make it durable. STICKY: the first failed write or fsync poisons the journal for the rest of the
 * session, and after that no new intent is written (every later effect is refused instead). Reason: a short write can
 * leave a fragment with no newline, and a record appended after it would be glued onto it and be unreadable, so an effect
 * could run with no readable intent. On a failed write the fragment is cut back to the previous end (tail_clean); only
 * then may an OUTCOME for an effect that already started still be appended (is_outcome), because it cannot be glued. After
 * a failed fsync the tail is unknown and nothing more is written. Reopening also closes a torn tail with a newline. */
static int commit(OshJournal *j, const char *buf, size_t n, int is_outcome)
{
    if (j->fd < 0) return -1;
    if (j->poisoned && !(is_outcome && j->tail_clean)) { errno = EIO; return -1; }
    off_t pre = lseek(j->fd, 0, SEEK_END);
    int rc = 0, e = 0, wrote_failed = 0;
#ifdef OSH_JOURNAL_TEST_HOOKS
    j->n_commit++;
    if (j->fail_write_at && j->n_commit == j->fail_write_at) {
        if (n > 1) (void)write_all(j->fd, buf, n / 2);
        rc = -1;
        e = ENOSPC;
        wrote_failed = 1;
    }
#endif
    if (!rc && write_all(j->fd, buf, n) != 0) { rc = -1; e = errno; wrote_failed = 1; }
    if (wrote_failed) {
        j->tail_clean = pre >= 0 && ftruncate(j->fd, pre) == 0;
    } else if (!rc && jsync(j) != 0) {
        rc = -1;
        e = errno;
        j->tail_clean = 0;
    }
    if (rc) {
        j->poisoned = 1;
        errno = e ? e : EIO;
        return -1;
    }
    return 0;
}

/* ---- parsing (shared by open and recover) ---- */

typedef struct {
    int kind; /* 'I', 'O', 0 = unparsable */
    uint64_t rec, ref;
    char digest[65];
    int idx, ncmds;
    char a0[200];
} Line;

static void parse_line(char *s, Line *o)
{
    memset(o, 0, sizeof *o);
    char *sv = NULL, *tok = strtok_r(s, " ", &sv);
    if (!tok || strcmp(tok, "OSHJ1")) return;
    if (!(tok = strtok_r(NULL, " ", &sv))) return;
    o->rec = strtoull(tok, NULL, 10);
    if (!(tok = strtok_r(NULL, " ", &sv)) || (tok[0] != 'I' && tok[0] != 'O') || tok[1]) return;
    char kind = tok[0];
    if (kind == 'O') {
        if (!(tok = strtok_r(NULL, " ", &sv))) return;
        o->ref = strtoull(tok, NULL, 10);
    }
    if (!(tok = strtok_r(NULL, " ", &sv)) || strlen(tok) != 64) return;
    memcpy(o->digest, tok, 65);
    if (!(tok = strtok_r(NULL, " ", &sv)) || sscanf(tok, "%d/%d", &o->idx, &o->ncmds) != 2) return;
    if (kind == 'I') {
        for (int k = 0; k < 3; k++) /* b= argc= wr= */
            if (!strtok_r(NULL, " ", &sv)) return;
        if (!(tok = strtok_r(NULL, " ", &sv)) || strncmp(tok, "a0=", 3)) return;
        snprintf(o->a0, sizeof o->a0, "%s", tok + 3);
    }
    o->kind = kind;
}

/* Visit every line of the file. cb returns nothing; torn/garbled lines are counted in *bad. */
static int scan(const char *path, void (*cb)(void *, const Line *), void *ctx, int *bad, int *ends_nl)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    size_t cap = 1 << 16, len = 0;
    char *buf = malloc(cap);
    if (!buf) { close(fd); return -ENOMEM; }
    for (;;) {
        if (len == cap) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); close(fd); return -ENOMEM; }
            buf = nb;
            cap *= 2;
        }
        ssize_t n = read(fd, buf + len, cap - len);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { int e = errno; free(buf); close(fd); return -e; }
        if (n == 0) break;
        len += (size_t)n;
    }
    close(fd);
    if (ends_nl) *ends_nl = len == 0 || buf[len - 1] == '\n';
    size_t i = 0;
    while (i < len) {
        size_t e = i;
        while (e < len && buf[e] != '\n') e++;
        int terminated = e < len;
        char *line = malloc(e - i + 1);
        if (!line) { free(buf); return -ENOMEM; }
        memcpy(line, buf + i, e - i);
        line[e - i] = 0;
        Line l;
        parse_line(line, &l);
        if (!terminated || !l.kind || l.rec == 0) { if (bad && (e > i)) (*bad)++; }
        else cb(ctx, &l);
        free(line);
        i = e + 1;
    }
    free(buf);
    return 0;
}

static void max_cb(void *ctx, const Line *l) { uint64_t *m = ctx; if (l->rec > *m) *m = l->rec; }

static int jfail(OshJournal *j, int e, const char *fmt, const char *a, unsigned m)
{
    snprintf(j->why, sizeof j->why, fmt, a, m);
    return -e;
}

static int random_bytes(uint8_t *b, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = getrandom(b + got, n - got, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    if (got == n) return 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    while (got < n) {
        ssize_t r = read(fd, b + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { close(fd); return -1; }
        got += (size_t)r;
    }
    close(fd);
    return 0;
}

/* The per-journal HMAC key lives beside the journal as <journal>.key (32 random bytes, 0600, owned by the caller).
 * Created atomically (temporary file with O_EXCL, fsync, link(2) which never overwrites, fsync of the directory) and
 * reused when present. FAIL CLOSED: an existing key with another owner, a mode other than 0600, a size other than 32 or
 * that cannot be read refuses the journal; so does a missing key beside a journal that already holds records (a new
 * key would silently make every earlier digest meaningless). */
static int load_key(OshJournal *j, const char *path, int have_records)
{
    char kp[4200];
    if (snprintf(kp, sizeof kp, "%s.key", path) >= (int)sizeof kp) return jfail(j, ENAMETOOLONG, "journal path too long%s", "", 0);
    int fd = open(kp, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT) {
        if (have_records) return jfail(j, ENOENT, "key file %s is missing but the journal already has records; refusing to start a new key", kp, 0);
        char tmp[4300];
        snprintf(tmp, sizeof tmp, "%s.tmp.%ld", kp, (long)getpid());
        int tfd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (tfd < 0) return jfail(j, errno, "cannot create key file %s", kp, 0);
        uint8_t k[32];
        int ok = random_bytes(k, sizeof k) == 0 && write_all(tfd, (const char *)k, sizeof k) == 0 && sync_fd(tfd) == 0;
        int e = ok ? 0 : (errno ? errno : EIO);
        close(tfd);
        memset(k, 0, sizeof k);
        if (ok && link(tmp, kp) != 0 && errno != EEXIST) { ok = 0; e = errno; }
        unlink(tmp);
        if (!ok) return jfail(j, e, "cannot create key file %s", kp, 0);
        int dr = fsync_parent(kp);
        if (dr) return jfail(j, -dr, "cannot sync the directory of key file %s", kp, 0);
        fd = open(kp, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    }
    if (fd < 0) return jfail(j, errno, "cannot open key file %s", kp, 0);
    struct stat st;
    if (fstat(fd, &st) != 0) { int e = errno; close(fd); return jfail(j, e, "cannot stat key file %s", kp, 0); }
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0777) != 0600 || st.st_size != 32) {
        close(fd);
        return jfail(j, EACCES, "key file %s must be a regular file of 32 bytes, owned by you, mode 0600 (found mode %04o); refusing it", kp, (unsigned)(st.st_mode & 07777));
    }
    size_t got = 0;
    while (got < 32) {
        ssize_t r = read(fd, j->key + got, 32 - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { close(fd); return jfail(j, EIO, "cannot read key file %s", kp, 0); }
        got += (size_t)r;
    }
    close(fd);
    return 0;
}

int osh_journal_open(OshJournal *j, const char *path)
{
    memset(j, 0, sizeof *j);
    j->fd = -1;
    struct stat st;
    int existed = lstat(path, &st) == 0;
    long size = 0;
    if (existed) {
        if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 07777) != 0600)
            return jfail(j, EACCES, "journal %s must be a regular file owned by you with mode 0600 (found mode %04o); refusing it, fix or remove it", path, (unsigned)(st.st_mode & 07777));
        size = (long)st.st_size;
    }
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return jfail(j, errno, "cannot open journal %s", path, 0);
    /* ONE SESSION PER JOURNAL: take the lock first, before the key is read or created and before recovery runs, so a live
     * session's open intents are never reported UNKNOWN by another. The kernel drops the lock when this process exits or dies. */
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int e = errno;
        close(fd);
        if (e == EWOULDBLOCK) return jfail(j, EBUSY, "journal %s is in use by another osh session (one session per journal); start this one with --journal FILE to use a separate journal", path, 0);
        return jfail(j, e, "cannot lock journal %s", path, 0);
    }
    /* everything below is decided from the locked descriptor, not from the earlier lstat (another session may have created or filled the file meanwhile) */
    if (fstat(fd, &st) != 0) { int e = errno; close(fd); return jfail(j, e, "cannot stat journal %s", path, 0); }
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 07777) != 0600) {
        close(fd);
        return jfail(j, EACCES, "journal %s must be a regular file owned by you with mode 0600 (found mode %04o); refusing it, fix or remove it", path, (unsigned)(st.st_mode & 07777));
    }
    size = (long)st.st_size;
    int krc = load_key(j, path, size > 0);
    if (krc) { close(fd); return krc; }
    if (size == 0) { /* empty: new, so make it and its directory entry durable */
        int rc = sync_fd(fd) == 0 ? fsync_parent(path) : -errno;
        if (rc) { close(fd); return jfail(j, rc < 0 ? -rc : EIO, "cannot sync new journal %s", path, 0); }
    }
    uint64_t mx = 0;
    int nl = 1;
    if (size > 0) {
        int bad = 0;
        int rc = scan(path, max_cb, &mx, &bad, &nl);
        if (rc && rc != -ENOENT) { close(fd); return jfail(j, -rc, "cannot read journal %s", path, 0); }
    }
    j->fd = fd;
    j->next_rec = mx + 1;
    if (!nl && commit(j, "\n", 1, 0) != 0) { int e = errno; close(fd); j->fd = -1; return jfail(j, e ? e : EIO, "cannot repair journal tail %s", path, 0); }
    return 0;
}

/* a state directory made by an earlier run must be ours and private; otherwise refuse (0 ok, -EACCES with why) */
int osh_journal_check_dir(OshJournal *j, const char *dir)
{
    struct stat st;
    if (stat(dir, &st) != 0) return jfail(j, errno, "cannot stat state directory %s", dir, 0);
    if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077) != 0)
        return jfail(j, EACCES, "state directory %s must be owned by you with mode 0700 (found mode %04o); refusing it, fix it with chmod 700", dir, (unsigned)(st.st_mode & 07777));
    return 0;
}

void osh_journal_close(OshJournal *j)
{
    if (j->fd >= 0) close(j->fd);
    j->fd = -1;
    memset(j->key, 0, sizeof j->key);
}

/* ---- records ---- */

static void esc(const char *s, char *out, size_t cap)
{
    size_t o = 0, n = strlen(s);
    if (!n) { snprintf(out, cap, "%%"); return; } /* an empty argv[0] */
    size_t from = n > 64 ? n - 64 : 0; /* keep the LAST 64 bytes: the program name matters, not its directory */
    if (from && o + 1 < cap) out[o++] = '~';
    for (size_t i = from; i < n && o + 4 < cap; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || c == '.' || c == '_' || c == '/' || c == '-' || c == '+') out[o++] = (char)c;
        else o += (size_t)snprintf(out + o, cap - o, "%%%02X", c);
    }
    out[o] = 0;
}

uint64_t osh_journal_intent(OshJournal *j, const uint8_t dig[32], const OshRequest *r, int idx)
{
    const OshCmd *c = &r->cmd[idx];
    char dh[65], a0[200], lens[OSH_MAX_ARGV * 6 + 4], line[1024];
    hex(dig, dh);
    esc(c->nargv > 0 ? c->argv[0] : "", a0, sizeof a0);
    size_t lo = 0;
    for (int k = 0; k < c->nargv; k++) lo += (size_t)snprintf(lens + lo, sizeof lens - lo, "%s%zu", k ? "," : "", strlen(c->argv[k]));
    if (!lo) strcpy(lens, "-");
    int wr = 0;
    for (int k = 0; k < c->nredir; k++) wr += c->redir[k].kind == OSH_R_OUT || c->redir[k].kind == OSH_R_APPEND;
    uint64_t rec = j->next_rec;
    int n = snprintf(line, sizeof line, "OSHJ1 %llu I %s %d/%d b=%d argc=%d wr=%d a0=%s lens=%s\n", (unsigned long long)rec, dh, idx,
                     r->ncmds, c->builtin_id, c->nargv, wr, a0, lens);
    if (n <= 0 || (size_t)n >= sizeof line) return 0;
    if (commit(j, line, (size_t)n, 0) != 0) return 0;
    j->next_rec++;
#ifdef OSH_JOURNAL_TEST_HOOKS
    if (j->die_after_intent && ++j->n_intent == j->die_after_intent) _exit(77);
#endif
    return rec;
}

int osh_journal_outcomes(OshJournal *j, const uint8_t dig[32], int ncmds, const OshJournalOutcome *o, int n)
{
    if (n <= 0) return 0;
    char dh[65];
    hex(dig, dh);
    char *buf = malloc((size_t)n * 160 + 1);
    if (!buf) return -1;
    size_t len = 0;
    uint64_t rec = j->next_rec;
    for (int i = 0; i < n; i++) {
        int oc = o[i].outcome;
        if (oc < 0 || oc > 4) oc = 4;
        len += (size_t)snprintf(buf + len, 160, "OSHJ1 %llu O %llu %s %d/%d %s st=%d err=%d\n", (unsigned long long)rec++,
                                (unsigned long long)o[i].ref, dh, o[i].idx, ncmds, k_outcome[oc], o[i].status, o[i].err);
    }
    int rc = commit(j, buf, len, 1);
    free(buf);
    if (rc != 0) return -1;
    j->next_rec = rec;
    return 0;
}

/* ---- recovery ---- */

typedef struct {
    Line *open;
    int n, cap;
    uint64_t *closed;
    int nclosed, capclosed;
} Rec;

static void rec_cb(void *ctx, const Line *l)
{
    Rec *r = ctx;
    if (l->kind == 'I') {
        if (r->n == r->cap) {
            int nc = r->cap ? r->cap * 2 : 64;
            Line *p = realloc(r->open, (size_t)nc * sizeof *p);
            if (!p) return;
            r->open = p;
            r->cap = nc;
        }
        r->open[r->n++] = *l;
    } else if (l->ref) {
        if (r->nclosed == r->capclosed) {
            int nc = r->capclosed ? r->capclosed * 2 : 64;
            uint64_t *p = realloc(r->closed, (size_t)nc * sizeof *p);
            if (!p) return;
            r->closed = p;
            r->capclosed = nc;
        }
        r->closed[r->nclosed++] = l->ref;
    }
}

int osh_journal_recover(OshJournal *j, const char *path, OshJournalUnknown *out, int cap, int *bad)
{
    Rec r;
    memset(&r, 0, sizeof r);
    int nb = 0;
    int rc = scan(path, rec_cb, &r, &nb, NULL);
    if (bad) *bad = nb;
    if (rc) { free(r.open); free(r.closed); return rc; }
    int found = 0;
    for (int i = 0; i < r.n; i++) {
        int closed = 0;
        for (int k = 0; k < r.nclosed && !closed; k++) closed = r.closed[k] == r.open[i].rec;
        if (closed) continue;
        if (out && found < cap) {
            OshJournalUnknown *u = &out[found];
            u->intent_rec = r.open[i].rec;
            memcpy(u->digest, r.open[i].digest, 65);
            u->idx = r.open[i].idx;
            u->ncmds = r.open[i].ncmds;
            snprintf(u->argv0, sizeof u->argv0, "%s", r.open[i].a0);
        }
        found++;
        char line[256];
        int n = snprintf(line, sizeof line, "OSHJ1 %llu O %llu %s %d/%d OUTCOME_UNKNOWN st=-1 err=%d recovered=1\n",
                         (unsigned long long)j->next_rec, (unsigned long long)r.open[i].rec, r.open[i].digest, r.open[i].idx,
                         r.open[i].ncmds, OSH_E_OUTCOME_UNKNOWN);
        if (commit(j, line, (size_t)n, 1) != 0) { found = -EIO; break; }
        j->next_rec++;
    }
    free(r.open);
    free(r.closed);
    return found;
}

/* ---- default location ---- */

int osh_journal_default_path(const char *xdg, const char *home, char *out, size_t cap)
{
    int n;
    if (xdg && xdg[0] == '/') n = snprintf(out, cap, "%s/osh/effects.journal", xdg);
    else if (home && home[0] == '/') n = snprintf(out, cap, "%s/.local/state/osh/effects.journal", home);
    else return -1;
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

int osh_journal_mkparents(const char *path)
{
    char d[4096];
    if (strlen(path) >= sizeof d) return -ENAMETOOLONG;
    strcpy(d, path);
    char *last = strrchr(d, '/');
    if (!last) return 0;
    *last = 0;
    for (char *p = d + 1; ; p++) {
        if (*p == '/' || *p == 0) {
            char c = *p;
            *p = 0;
            if (mkdir(d, 0700) == 0) { /* newly created: make it and its place in the parent durable */
                int dfd = open(d, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                if (dfd < 0) return -errno;
                int sr = sync_fd(dfd) == 0 ? 0 : -errno;
                close(dfd);
                if (sr) return sr;
                int pr = fsync_parent(d);
                if (pr) return pr;
            } else if (errno != EEXIST) return -errno;
            *p = c;
            if (!c) break;
        }
    }
    return 0;
}

#ifdef OSH_JOURNAL_TEST_HOOKS
/* marker symbol: the build checks the shipped osh does NOT contain it (so no test hook is compiled into production) */
int osh_journal_test_hooks_compiled(void) { return 1; }
#endif
