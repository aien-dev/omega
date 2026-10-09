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
void osh_req_digest(const OshRequest *r, uint8_t out[32])
{
    sha256_ctx h;
    sha256_init(&h);
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
    sha256_final(&h, out);
}

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

/* append one batch and make it durable */
static int commit(OshJournal *j, const char *buf, size_t n)
{
    if (j->fd < 0) return -1;
    if (write_all(j->fd, buf, n) != 0) return -1;
    return sync_fd(j->fd);
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

int osh_journal_open(OshJournal *j, const char *path)
{
    memset(j, 0, sizeof *j);
    j->fd = -1;
    int existed = access(path, F_OK) == 0;
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return -errno;
    if (!existed) {
        int rc = sync_fd(fd) == 0 ? fsync_parent(path) : -errno;
        if (rc) { close(fd); return rc < 0 ? rc : -EIO; }
    }
    uint64_t mx = 0;
    int nl = 1;
    if (existed) {
        int bad = 0;
        int rc = scan(path, max_cb, &mx, &bad, &nl);
        if (rc && rc != -ENOENT) { close(fd); return rc; }
    }
    j->fd = fd;
    j->next_rec = mx + 1;
    if (!nl && commit(j, "\n", 1) != 0) { int e = errno; close(fd); j->fd = -1; return -(e ? e : EIO); }
    return 0;
}

void osh_journal_close(OshJournal *j)
{
    if (j->fd >= 0) close(j->fd);
    j->fd = -1;
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
    if (commit(j, line, (size_t)n) != 0) return 0;
    j->next_rec++;
    if (j->die_after_intent && ++j->n_intent == j->die_after_intent) _exit(77);
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
    int rc = commit(j, buf, len);
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
        if (commit(j, line, (size_t)n) != 0) { found = -EIO; break; }
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
            if (mkdir(d, 0700) != 0 && errno != EEXIST) return -errno;
            *p = c;
            if (!c) break;
        }
    }
    return 0;
}
