/*
 * genret.c -- file-backed retrieval provider of the GEN experiment (aien-spark/omega#479, #475).
 * Contract: tools/gen/retrieval/CONTRACT.md (genret-cli-v3). Arms C and D run the same algorithm with the
 * same k, byte cap and rendering; only --kind differs. The query is built from the public requirement (entry,
 * params, ret values) and the public prompt only; no other task file is opened.
 *
 *   genret query --corpus <MANIFEST> --manifest-sha256 <hex> --recheck <recheck binary> --recheck-sha256 <hex>
 *                --kind raw|abstraction --k <n> --cap-bytes <n>
 *                --requirement <file> --prompt <file> --out <new dir> [--exclude-family <f>]...
 *                [--mode normal|irrelevant|empty|length-matched] [--target-bytes <n>]
 *   genret version
 *
 * Exit 0 on success (an empty selection included); exit 2 on any fatal condition, with one stderr line
 * "genret: FATAL <reason> <detail>" and no --out. All scoring is integer arithmetic.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sha256.h"

/* sha256 of the recheck.c the build verified and compiled next to genret (mk/gen-retrieval.mk) */
#ifndef GENRET_RECHECK_SOURCE_SHA256
#error "build with -DGENRET_RECHECK_SOURCE_SHA256='\"<64 hex>\"' (see mk/gen-retrieval.mk)"
#endif

extern char **environ;

#define CONTRACT "genret-cli-v3"
#define MAX_RECORD 65536
#define MAX_RECORDS 65536
#define MAX_TASKFILE 4096
#define MAX_MANIFEST (MAX_RECORDS * 4200)
#define MAX_RECHECK_BIN (64 << 20)
#define MAX_EXCL 256

typedef unsigned __int128 u128;
typedef struct { const char *p; size_t n; } span;
typedef struct { char *p; size_t n, cap; } buf;

/* the record fields genret reads (RECORD_SCHEMA.md v1 section 2); every other key line is skipped */
enum { K_ID, K_KIND, K_SPLIT, K_FAMILY, K_VALIDITY, K_ENTRY, K_PARAMS, K_RET, K_DESC, K_N };
static const char *KEYS[K_N] = { "id", "kind", "split", "family", "validity", "entry", "params", "ret", "desc" };

typedef struct {
    char digest[65];
    const char *relpath;
    unsigned char *raw;   /* record bytes as read */
    char *low;            /* same bytes, A-Z lowered: token spans point here */
    size_t len;
    span f[K_N];          /* field values, offsets into raw */
    int has[K_N];
    span body;
    char reason[160];     /* exclusion reason; empty while selected or undecided */
    int indexed, selected, pad;
    span *tok; size_t ntok;
    uint64_t score;
    unsigned tf;          /* scratch */
} rec;

/* ---- small helpers ------------------------------------------------------------------------------- */
static void fatal(const char *reason, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "genret: FATAL %s ", reason);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
    exit(2);
}
static void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) fatal("internal", "out of memory"); return p; }
static void *xrealloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) fatal("internal", "out of memory"); return p; }

static void bput(buf *b, const void *p, size_t n)
{
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->p = xrealloc(b->p, b->cap); }
    memcpy(b->p + b->n, p, n); b->n += n; b->p[b->n] = 0;
}
static void bputs(buf *b, const char *s) { bput(b, s, strlen(s)); }
static void bspan(buf *b, span s) { bput(b, s.p, s.n); }
static void bprintf(buf *b, const char *fmt, ...)
{
    char tmp[512]; va_list ap; int n;
    va_start(ap, fmt); n = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (n < 0 || (size_t)n >= sizeof tmp) fatal("internal", "format overflow");
    bput(b, tmp, (size_t)n);
}

/* unsigned byte compare; a proper prefix sorts first */
static int cmpb(const char *a, size_t na, const char *b, size_t nb)
{
    size_t m = na < nb ? na : nb;
    int c = m ? memcmp(a, b, m) : 0;
    if (c) return c;
    return na < nb ? -1 : na > nb ? 1 : 0;
}
static int spaneq(span s, const char *lit) { return s.n == strlen(lit) && memcmp(s.p, lit, s.n) == 0; }

/* reads an open file from its current offset to end; returns NULL on a read error; *over = 1 when longer
 * than max (reading stops one byte past max) */
static unsigned char *slurp_fd(int fd, size_t max, size_t *len, int *over)
{
    size_t cap = 1 << 16, n = 0;
    unsigned char *p = xmalloc(cap + 1);
    for (;;) {
        size_t want = (max + 1 - n) < (cap - n) ? (max + 1 - n) : (cap - n);
        ssize_t got = read(fd, p + n, want);
        if (got < 0) { if (errno == EINTR) continue; free(p); return NULL; }
        if (got == 0) break;
        n += (size_t)got;
        if (n > max) break;
        if (n == cap) { cap *= 2; p = xrealloc(p, cap + 1); }
    }
    *over = n > max; *len = n; p[n] = 0;
    return p;
}
/* reads a whole file; returns NULL when unreadable */
static unsigned char *slurp(const char *path, size_t max, size_t *len, int *over)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    unsigned char *p = slurp_fd(fd, max, len, over);
    close(fd);
    return p;
}
static void hexdigest(const void *p, size_t n, char out[65])
{
    uint8_t d[SHA256_DIGEST_SIZE];
    sha256_hash((const uint8_t *)p, n, d);
    for (int i = 0; i < SHA256_DIGEST_SIZE; i++) sprintf(out + 2 * i, "%02x", d[i]);
}
static int ishex64(const char *p, size_t n)
{
    if (n != 64) return 0;
    for (size_t i = 0; i < n; i++) if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) return 0;
    return 1;
}

/* ---- value patterns ------------------------------------------------------------------------------- */
static int c_name(int c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; }
static int c_word(int c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }
static int tokpat(const char *p, size_t n, int (*ok)(int), size_t max)
{
    if (n < 1 || n > max) return 0;
    for (size_t i = 0; i < n; i++) if (!ok((unsigned char)p[i])) return 0;
    return 1;
}
/* space-separated list of tokens (single spaces); min = minimum count */
static int listpat(span s, int (*ok)(int), size_t max, int min)
{
    if (s.n == 0) return min == 0;
    size_t i = 0, cnt = 0;
    while (i <= s.n) {
        size_t j = i;
        while (j < s.n && s.p[j] != ' ') j++;
        if (!tokpat(s.p + i, j - i, ok, max)) return 0;
        cnt++; i = j + 1;
    }
    return (int)cnt >= min;
}
static int lower_az(int c) { return c >= 'a' && c <= 'z'; }
static int valid_value(int k, span v)
{
    switch (k) {
    case K_ID: case K_SPLIT: return tokpat(v.p, v.n, c_name, 64);
    case K_KIND: return spaneq(v, "raw") || spaneq(v, "abstraction");
    case K_FAMILY: return listpat(v, c_name, 64, 1);
    case K_VALIDITY: return tokpat(v.p, v.n, lower_az, 16);
    case K_ENTRY: {
        if (v.n < 1 || v.n > 64) return 0;
        for (size_t i = 0; i < v.n; i++) {
            int c = (unsigned char)v.p[i];
            int a = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
            if (!(a || (i > 0 && c >= '0' && c <= '9'))) return 0;
        }
        return 1;
    }
    case K_PARAMS: return listpat(v, c_word, 16, 0);
    case K_RET: return tokpat(v.p, v.n, c_word, 16);
    case K_DESC: return v.n >= 1 && v.n <= 512;
    }
    return 0;
}

/* ---- record parsing (RECORD_SCHEMA.md v1; the record already passed recheck, CONTRACT.md 2) ---------- */
static void parse_record(rec *r)
{
    const char *p = (const char *)r->raw, *end = p + r->len;
    static const char MAGIC[] = "gen-record 1\n";
    if (r->len < sizeof MAGIC - 1 || memcmp(p, MAGIC, sizeof MAGIC - 1) != 0)
        fatal("invalid_record", "%s: first line is not 'gen-record 1'", r->relpath);
    p += sizeof MAGIC - 1;
    for (;;) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) fatal("invalid_record", "%s: no 'body' line", r->relpath);
        size_t n = (size_t)(nl - p);
        for (size_t i = 0; i < n; i++)
            if ((unsigned char)p[i] < 0x20 || (unsigned char)p[i] > 0x7e)
                fatal("invalid_record", "%s: header byte outside 0x20..0x7E", r->relpath);
        if (n == 4 && memcmp(p, "body", 4) == 0) { p = nl + 1; break; }
        size_t kl = 0;
        while (kl < n && p[kl] != ' ') kl++;
        int k = -1;
        for (int i = 0; i < K_N; i++) if (kl == strlen(KEYS[i]) && memcmp(p, KEYS[i], kl) == 0) k = i;
        if (k < 0) { p = nl + 1; continue; }   /* signature, receipt, ancestry, source, pre, post, ...: not used */
        if (r->has[k]) fatal("invalid_record", "%s: key %s repeated", r->relpath, KEYS[k]);
        span v = { p + kl + (kl < n), kl < n ? n - kl - 1 : 0 };
        if (kl == n && k != K_PARAMS) fatal("invalid_record", "%s: key %s has no value", r->relpath, KEYS[k]);
        if (!valid_value(k, v)) fatal("invalid_record", "%s: bad %s value", r->relpath, KEYS[k]);
        r->has[k] = 1; r->f[k] = v;
        p = nl + 1;
    }
    for (int i = 0; i < K_N; i++)
        if (!r->has[i]) fatal("invalid_record", "%s: missing key %s", r->relpath, KEYS[i]);
    r->body.p = p; r->body.n = (size_t)(end - p);
    if (r->body.n < 1 || p[r->body.n - 1] != '\n') fatal("invalid_record", "%s: body empty or not ending in a newline", r->relpath);
    for (size_t i = 0; i < r->body.n; i++)
        if (p[i] != '\n' && ((unsigned char)p[i] < 0x20 || (unsigned char)p[i] > 0x7e))
            fatal("invalid_record", "%s: body byte outside \\n and 0x20..0x7E", r->relpath);
}

/* ---- tokenization (CONTRACT.md 4) ----------------------------------------------------------------- */
static void tokenize(const char *low, size_t n, span **tok, size_t *ntok, size_t *cap)
{
    size_t i = 0;
    while (i < n) {
        while (i < n && !c_word((unsigned char)low[i])) i++;
        size_t j = i;
        while (j < n && c_word((unsigned char)low[j])) j++;
        if (j > i) {
            if (*ntok == *cap) { *cap = *cap ? *cap * 2 : 64; *tok = xrealloc(*tok, *cap * sizeof **tok); }
            (*tok)[(*ntok)++] = (span){ low + i, j - i };
        }
        i = j;
    }
}
static char *lowered(const unsigned char *p, size_t n)
{
    char *l = xmalloc(n + 1);
    for (size_t i = 0; i < n; i++) l[i] = (char)((p[i] >= 'A' && p[i] <= 'Z') ? p[i] + 32 : p[i]);
    l[n] = 0;
    return l;
}
static int cmpspan(const void *a, const void *b)
{
    const span *x = a, *y = b;
    return cmpb(x->p, x->n, y->p, y->n);
}
/* occurrences of t in a sorted token array */
static unsigned count_tok(const span *tok, size_t n, span t)
{
    size_t lo = 0, hi = n;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (cmpb(tok[m].p, tok[m].n, t.p, t.n) < 0) lo = m + 1; else hi = m; }
    unsigned c = 0;
    while (lo < n && cmpb(tok[lo].p, tok[lo].n, t.p, t.n) == 0) { c++; lo++; }
    return c;
}

/* ---- scoring (CONTRACT.md 6) ---------------------------------------------------------------------- */
static uint64_t log2q16(uint64_t a, uint64_t b)
{
    uint64_t k = 0, y, r;
    while (a >= 2 * b) { b *= 2; k++; }
    y = (a << 30) / b;
    r = k << 16;
    for (int i = 15; i >= 0; i--) {
        y = (y * y) >> 30;
        if (y >= (UINT64_C(1) << 31)) { y >>= 1; r += UINT64_C(1) << i; }
    }
    return r;
}

/* ---- rendering ------------------------------------------------------------------------------------ */
/* one heading for both kinds: C and D suffixes differ only in record content */
#define HEADING "\nVerified reference code from earlier tasks, most relevant first.\n"
static void render_head(buf *b) { bputs(b, HEADING); }
static void render_block(buf *b, size_t i, const rec *r)
{
    bprintf(b, "\nRecord %zu.\n\nentry ", i); bspan(b, r->f[K_ENTRY]);
    bputs(b, "\nparams"); if (r->f[K_PARAMS].n) { bputs(b, " "); bspan(b, r->f[K_PARAMS]); }
    bputs(b, "\nret "); bspan(b, r->f[K_RET]);
    bputs(b, "\n\n"); bspan(b, r->f[K_DESC]);
    bputs(b, "\n\n```\n"); bspan(b, r->body); bputs(b, "```\n");
}
static size_t block_len(size_t i, const rec *r)
{
    buf b = { 0 }; render_block(&b, i, r); size_t n = b.n; free(b.p); return n;
}

/* ---- ordering ------------------------------------------------------------------------------------- */
static int cmp_id(const rec *x, const rec *y) { return cmpb(x->f[K_ID].p, x->f[K_ID].n, y->f[K_ID].p, y->f[K_ID].n); }
static int cmp_desc(const void *a, const void *b)
{
    const rec *x = *(rec *const *)a, *y = *(rec *const *)b;
    if (x->score != y->score) return x->score > y->score ? -1 : 1;
    return cmp_id(x, y);
}
static int cmp_asc(const void *a, const void *b)
{
    const rec *x = *(rec *const *)a, *y = *(rec *const *)b;
    if (x->score != y->score) return x->score < y->score ? -1 : 1;
    return cmp_id(x, y);
}
static int cmp_byid(const void *a, const void *b) { return cmp_id(*(rec *const *)a, *(rec *const *)b); }
static int cmpstr(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

/* ---- task files (CONTRACT.md 3) ------------------------------------------------------------------- */
/* opens a task file once, checks the resolved path of THAT open file (no component named "hidden",
 * byte-exact) and that it is a regular file, then reads it from the same descriptor. A symlink swapped in
 * after the check cannot change what is read. Hard links into a hidden directory are not detectable. */
static unsigned char *read_task(const char *path, const char *what, size_t *len, int *over)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);   /* a FIFO must not block before fstat */
    if (fd < 0) fatal(what, "cannot read %s", path);
    char link[64], rp[PATH_MAX];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, rp, sizeof rp - 1);
    if (n <= 0) fatal(what, "cannot resolve %s", path);
    rp[n] = 0;
    for (char *s = rp; *s; ) {
        while (*s == '/') s++;
        char *e = s; while (*e && *e != '/') e++;
        if (e - s == 6 && memcmp(s, "hidden", 6) == 0) fatal("hidden_path", "%s is under a 'hidden' directory", path);
        s = e;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) fatal(what, "%s is not a regular file", path);
    unsigned char *p = slurp_fd(fd, MAX_TASKFILE, len, over);
    close(fd);
    if (!p) fatal(what, "cannot read %s", path);
    return p;
}
static int parse_num(const char *s, long lo, long hi, long *out)
{
    if (!*s || strlen(s) > 9) return 0;
    for (const char *p = s; *p; p++) if (*p < '0' || *p > '9') return 0;
    if (s[0] == '0' && s[1]) return 0;
    long v = strtol(s, NULL, 10);
    if (v < lo || v > hi) return 0;
    *out = v; return 1;
}

/* ---- record validation by Agent D's recheck (CONTRACT.md 2): genret and recheck cannot disagree -------
 * recheck --receipts <dir> <record>... prints "OK <id>" per accepted record or "REFUSED <file>: ..." and
 * exits 0 (all accepted), 1 (any refused) or 2 (usage). Run in batches; every OK line must name the id
 * genret parsed from the same file, in order. */
#define RECHECK_BATCH 128
/* opens the recheck binary once and hashes it; refuses it unless the sha256 equals the pin. The returned
 * descriptor is what run_recheck executes (fexecve), so the binary that runs is the one that was hashed.
 * It is not close-on-exec so that a script interpreter can open it through /proc/self/fd. */
static int open_recheck(const char *bin, const char *pin, char sha[65])
{
    int fd = open(bin, O_RDONLY);
    if (fd < 0) fatal("recheck_failed", "cannot open %s", bin);
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) fatal("recheck_failed", "%s is not a regular file", bin);
    size_t n; int over;
    unsigned char *p = slurp_fd(fd, MAX_RECHECK_BIN, &n, &over);
    if (!p || over) fatal("recheck_failed", "cannot read %s", bin);
    hexdigest(p, n, sha);
    free(p);
    if (strcmp(sha, pin) != 0) fatal("recheck_untrusted", "%s has sha256 %s, pinned %s", bin, sha, pin);
    return fd;
}
static void run_recheck(int binfd, const char *bin, const char *rdir, const char *mdir, rec *R, size_t nr)
{
    for (size_t b0 = 0; b0 < nr; b0 += RECHECK_BATCH) {
        size_t bn = nr - b0 < RECHECK_BATCH ? nr - b0 : RECHECK_BATCH;
        char **av = xmalloc((bn + 4) * sizeof *av);
        av[0] = (char *)bin; av[1] = "--receipts"; av[2] = (char *)rdir;
        for (size_t i = 0; i < bn; i++) {
            av[3 + i] = xmalloc(strlen(mdir) + strlen(R[b0 + i].relpath) + 1);
            strcpy(av[3 + i], mdir); strcat(av[3 + i], R[b0 + i].relpath);
        }
        av[3 + bn] = NULL;
        int pfd[2];
        if (pipe(pfd) != 0) fatal("recheck_failed", "pipe");
        fflush(NULL);
        pid_t pid = fork();
        if (pid < 0) fatal("recheck_failed", "fork");
        if (pid == 0) {
            close(pfd[0]);
            if (dup2(pfd[1], 1) < 0) _exit(127);
            close(pfd[1]);
            fexecve(binfd, av, environ);
            _exit(127);
        }
        close(pfd[1]);
        buf o = { 0 };
        char tmp[4096]; ssize_t n;
        while ((n = read(pfd[0], tmp, sizeof tmp)) != 0) {
            if (n < 0) { if (errno == EINTR) continue; fatal("recheck_failed", "read"); }
            bput(&o, tmp, (size_t)n);
        }
        close(pfd[0]);
        int st;
        while (waitpid(pid, &st, 0) < 0) if (errno != EINTR) fatal("recheck_failed", "waitpid");
        if (!WIFEXITED(st) || (WEXITSTATUS(st) != 0 && WEXITSTATUS(st) != 1))
            fatal("recheck_failed", "%s did not run cleanly (status %d)", bin, st);
        if (WEXITSTATUS(st) == 1) {
            char *l = o.p ? strstr(o.p, "REFUSED ") : NULL;
            if (!l || (l != o.p && l[-1] != '\n')) fatal("recheck_failed", "%s exit 1 without a REFUSED line", bin);
            char *e = strchr(l, '\n'); if (e) *e = 0;
            fatal("invalid_record", "recheck: %s", l + 8);
        }
        const char *s = o.p ? o.p : "";
        for (size_t i = 0; i < bn; i++) {
            const rec *r = &R[b0 + i];
            const char *e = strchr(s, '\n');
            if (!e || strncmp(s, "OK ", 3) || cmpb(s + 3, (size_t)(e - s - 3), r->f[K_ID].p, r->f[K_ID].n) != 0)
                fatal("recheck_failed", "%s: no 'OK %.*s' line for %s", bin, (int)r->f[K_ID].n, r->f[K_ID].p, r->relpath);
            s = e + 1;
        }
        if (*s) fatal("recheck_failed", "%s printed extra output", bin);
        for (size_t i = 0; i < bn; i++) free(av[3 + i]);
        free(av); free(o.p);
    }
}

/* ---- output --------------------------------------------------------------------------------------- */
typedef struct { const char *name; buf *b; } outfile;
/* writes every file into a fresh sibling directory <out>.tmp.<pid>, then renames it to <out> without
 * replacing anything (RENAME_NOREPLACE): --out either does not exist or holds all six files. */
static void tmp_clean(const char *tmp, outfile *fs, int nf)
{
    char path[PATH_MAX + 32];
    for (int j = 0; j < nf; j++) { snprintf(path, sizeof path, "%s/%s", tmp, fs[j].name); unlink(path); }
    rmdir(tmp);
}
static void write_out(const char *dir, outfile *fs, int nf)
{
    char tmp[PATH_MAX];
    size_t dl = strlen(dir);
    while (dl > 1 && dir[dl - 1] == '/') dl--;
    if (dl + 32 >= sizeof tmp) fatal("write_failed", "--out path too long");
    snprintf(tmp, sizeof tmp, "%.*s.tmp.%ld", (int)dl, dir, (long)getpid());
    if (mkdir(tmp, 0777) != 0) fatal("write_failed", "cannot create %s", tmp);
    for (int i = 0; i < nf; i++) {
        char path[PATH_MAX + 32];
        snprintf(path, sizeof path, "%s/%s", tmp, fs[i].name);
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        size_t n = fs[i].b->n, w = 0;
        const char *p = fs[i].b->p ? fs[i].b->p : "";
        while (fd >= 0 && w < n) {
            ssize_t r = write(fd, p + w, n - w);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            w += (size_t)r;
        }
        int ok = fd >= 0 && w == n && fsync(fd) == 0;
        if (fd >= 0 && close(fd) != 0) ok = 0;
        if (!ok) { tmp_clean(tmp, fs, nf); fatal("write_failed", "cannot write %s/%s", tmp, fs[i].name); }
    }
    if (renameat2(AT_FDCWD, tmp, AT_FDCWD, dir, RENAME_NOREPLACE) != 0) {
        int e = errno;
        tmp_clean(tmp, fs, nf);
        if (e == EEXIST) fatal("out_exists", "%s exists", dir);
        fatal("write_failed", "cannot rename %s to %s", tmp, dir);
    }
}

static void usage(const char *why)
{
    fatal("usage", "%s (genret query --corpus MANIFEST --manifest-sha256 HEX --recheck BIN --recheck-sha256 HEX "
          "--kind raw|abstraction --k N "
          "--cap-bytes N --requirement F --prompt F --out DIR [--exclude-family F]... "
          "[--mode normal|irrelevant|empty|length-matched] [--target-bytes N]; genret version)", why);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "version") == 0) { puts(CONTRACT); return 0; }
    if (argc < 2 || strcmp(argv[1], "query") != 0) usage("first argument must be query or version");

    const char *corpus = NULL, *msha = NULL, *kind = NULL, *reqf = NULL, *prf = NULL, *out = NULL, *mode = NULL, *recheck = NULL, *rpin = NULL;
    const char *excl[MAX_EXCL]; int nexcl = 0;
    long k = -1, cap = -1, target = -1;
    for (int i = 2; i < argc; i++) {
        const char *o = argv[i];
        if (i + 1 >= argc) usage("option without value");
        const char *v = argv[++i];
        const char **slot = NULL; long *num = NULL; long lo = 0, hi = 0;
        if (!strcmp(o, "--corpus")) slot = &corpus;
        else if (!strcmp(o, "--manifest-sha256")) slot = &msha;
        else if (!strcmp(o, "--kind")) slot = &kind;
        else if (!strcmp(o, "--requirement")) slot = &reqf;
        else if (!strcmp(o, "--prompt")) slot = &prf;
        else if (!strcmp(o, "--out")) slot = &out;
        else if (!strcmp(o, "--recheck")) slot = &recheck;
        else if (!strcmp(o, "--recheck-sha256")) slot = &rpin;
        else if (!strcmp(o, "--mode")) slot = &mode;
        else if (!strcmp(o, "--k")) { num = &k; lo = 1; hi = 64; }
        else if (!strcmp(o, "--cap-bytes")) { num = &cap; lo = 1; hi = 1048576; }
        else if (!strcmp(o, "--target-bytes")) { num = &target; lo = 1; hi = 1048576; }
        else if (!strcmp(o, "--exclude-family")) {
            if (!tokpat(v, strlen(v), c_name, 64)) usage("--exclude-family must match [a-z0-9_-]{1,64}");
            if (nexcl == MAX_EXCL) usage("too many --exclude-family");
            excl[nexcl++] = v; continue;
        } else usage("unknown option");
        if (slot) { if (*slot) usage("option repeated"); *slot = v; }
        else { if (*num != -1) usage("option repeated"); if (!parse_num(v, lo, hi, num)) usage("number out of range"); }
    }
    if (!corpus || !msha || !kind || !reqf || !prf || !out || !recheck || !rpin || k < 0 || cap < 0) usage("missing required option");
    if (!ishex64(msha, strlen(msha))) usage("--manifest-sha256 must be 64 lowercase hex");
    if (!ishex64(rpin, strlen(rpin))) usage("--recheck-sha256 must be 64 lowercase hex");
    if (strcmp(kind, "raw") && strcmp(kind, "abstraction")) usage("--kind must be raw or abstraction");
    if (!mode) mode = "normal";
    int m_normal = !strcmp(mode, "normal"), m_irr = !strcmp(mode, "irrelevant"), m_empty = !strcmp(mode, "empty"),
        m_len = !strcmp(mode, "length-matched");
    if (!(m_normal || m_irr || m_empty || m_len)) usage("unknown --mode");
    if (m_len != (target != -1)) usage("--target-bytes is required with, and only with, --mode length-matched");
    if (m_len && target > cap) usage("--target-bytes must not exceed --cap-bytes");
    struct stat st;
    if (lstat(out, &st) == 0) fatal("out_exists", "%s exists", out);
    char rsha[65];
    int rfd = open_recheck(recheck, rpin, rsha);

    /* ---- task files: the only query input ---- */
    size_t rn, pn; int over;
    unsigned char *req = read_task(reqf, "requirement_invalid", &rn, &over);
    if (over || memchr(req, 0, rn)) fatal("requirement_invalid", "%s: over %d bytes or holds NUL", reqf, MAX_TASKFILE);
    unsigned char *pr = read_task(prf, "prompt_invalid", &pn, &over);
    if (over || pn < 1) fatal("prompt_invalid", "%s: must be 1..%d bytes", prf, MAX_TASKFILE);
    for (size_t i = 0; i < pn; i++)
        if (pr[i] != '\n' && (pr[i] < 0x20 || pr[i] > 0x7e)) fatal("prompt_invalid", "%s: byte outside \\n and 0x20..0x7E", prf);
    char *reql = lowered(req, rn), *prl = lowered(pr, pn);
    span *q = NULL; size_t nq = 0, qcap = 0;
    {
        static const char *QK[3] = { "entry", "params", "ret" };
        int seen[3] = { 0, 0, 0 };
        size_t i = 0;
        while (i < rn) {
            const unsigned char *nl = memchr(req + i, '\n', rn - i);
            size_t e = nl ? (size_t)(nl - req) : rn;
            for (int j = 0; j < 3; j++) {
                size_t kl = strlen(QK[j]);
                if (e - i >= kl && memcmp(req + i, QK[j], kl) == 0 && (e - i == kl || req[i + kl] == ' ')) {
                    seen[j]++;
                    size_t vs = i + kl + (e - i > kl);
                    tokenize(reql + vs, e - vs, &q, &nq, &qcap);
                }
            }
            i = e + 1;
        }
        for (int j = 0; j < 3; j++)
            if (seen[j] != 1) fatal("requirement_invalid", "%s: needs exactly one '%s' line", reqf, QK[j]);
    }
    tokenize(prl, pn, &q, &nq, &qcap);
    qsort(q, nq, sizeof *q, cmpspan);
    size_t nu = 0;
    for (size_t i = 0; i < nq; i++) if (nu == 0 || cmpspan(&q[nu - 1], &q[i]) != 0) q[nu++] = q[i];
    nq = nu;

    /* ---- manifest ---- */
    size_t mn;
    unsigned char *man = slurp(corpus, MAX_MANIFEST, &mn, &over);
    if (!man || over) fatal("manifest_unreadable", "cannot read %s", corpus);
    char mdig[65]; hexdigest(man, mn, mdig);
    if (strcmp(mdig, msha) != 0) fatal("manifest_digest_mismatch", "%s has sha256 %s, pinned %s", corpus, mdig, msha);
    if (mn == 0 || man[mn - 1] != '\n') fatal("manifest_malformed", "empty or no final newline");
    /* record paths resolve against the manifest's directory, made absolute (recheck takes them as arguments) */
    char *mdir = xmalloc(PATH_MAX + 2);
    {
        char *d = xmalloc(strlen(corpus) + 3);
        strcpy(d, corpus);
        char *s = strrchr(d, '/'); if (s) s[1] = 0; else strcpy(d, "./");
        if (!realpath(d, mdir)) fatal("manifest_unreadable", "cannot resolve the directory of %s", corpus);
        strcat(mdir, "/");
        free(d);
    }
    size_t nr = 0;
    for (size_t i = 0; i < mn; i++) if (man[i] == '\n') nr++;
    if (nr > MAX_RECORDS) fatal("manifest_malformed", "more than %d lines", MAX_RECORDS);
    rec *R = calloc(nr, sizeof *R);
    if (!R) fatal("internal", "out of memory");
    {
        size_t i = 0, li = 0;
        while (i < mn) {
            char *line = (char *)man + i, *nl = memchr(line, '\n', mn - i);
            size_t n = (size_t)(nl - line);
            *nl = 0;
            if (n < 67 || !ishex64(line, 64) || line[64] != ' ' || line[65] != ' ')
                fatal("manifest_malformed", "line %zu is not '<sha256>  <path>'", li + 1);
            if (memchr(line, 0, n)) fatal("manifest_malformed", "line %zu holds NUL", li + 1);
            char *path = line + 66;
            if (path[0] == '/' || strlen(path) > 4096) fatal("manifest_malformed", "line %zu: bad path", li + 1);
            for (char *s = path; ; ) {
                char *e = strchr(s, '/'); size_t cl = e ? (size_t)(e - s) : strlen(s);
                if (cl == 0 || (cl == 1 && s[0] == '.') || (cl == 2 && s[0] == '.' && s[1] == '.'))
                    fatal("manifest_malformed", "line %zu: bad path component", li + 1);
                for (size_t c = 0; c < cl; c++)
                    if (!(c_name((unsigned char)s[c]) || (s[c] >= 'A' && s[c] <= 'Z') || s[c] == '.'))
                        fatal("manifest_malformed", "line %zu: bad path byte", li + 1);
                if (!e) break;
                s = e + 1;
            }
            rec *r = &R[li];
            memcpy(r->digest, line, 64); r->digest[64] = 0;
            r->relpath = path;
            li++; i += n + 1;
        }
        /* duplicate paths: sort the path pointers (sizeof(char *) on purpose) and compare neighbours */
        const char **paths = xmalloc(nr * sizeof *paths);
        for (size_t j = 0; j < nr; j++) paths[j] = R[j].relpath;
        qsort(paths, nr, sizeof *paths, cmpstr);
        for (size_t j = 1; j < nr; j++)
            if (!strcmp(paths[j - 1], paths[j])) fatal("manifest_malformed", "duplicate path %s", paths[j]);
        free(paths);
    }

    /* ---- records: integrity is fatal ---- */
    for (size_t i = 0; i < nr; i++) {
        rec *r = &R[i];
        char *full = xmalloc(strlen(mdir) + strlen(r->relpath) + 1);
        strcpy(full, mdir); strcat(full, r->relpath);
        r->raw = slurp(full, MAX_RECORD, &r->len, &over);
        free(full);
        if (!r->raw) fatal("record_missing", "%s", r->relpath);
        if (over) fatal("invalid_record", "%s: over %d bytes", r->relpath, MAX_RECORD);
        char d[65]; hexdigest(r->raw, r->len, d);
        if (strcmp(d, r->digest)) fatal("stale_digest", "%s has sha256 %s, manifest says %s", r->relpath, d, r->digest);
        parse_record(r);
        r->low = lowered(r->raw, r->len);
    }
    {
        char *rdir = xmalloc(strlen(mdir) + 16);
        strcpy(rdir, mdir); strcat(rdir, "receipts");
        run_recheck(rfd, recheck, rdir, mdir, R, nr);
        free(rdir);
    }
    /* the files recheck read must still be the bytes genret parsed */
    for (size_t i = 0; i < nr; i++) {
        char *full = xmalloc(strlen(mdir) + strlen(R[i].relpath) + 1), d[65];
        strcpy(full, mdir); strcat(full, R[i].relpath);
        size_t l2; unsigned char *again = slurp(full, MAX_RECORD, &l2, &over);
        free(full);
        if (!again) fatal("record_missing", "%s", R[i].relpath);
        hexdigest(again, l2, d);
        if (strcmp(d, R[i].digest)) fatal("stale_digest", "%s changed while it was checked", R[i].relpath);
        free(again);
    }
    /* byid, ord and sel are arrays of rec pointers: sizeof *x is sizeof(rec *) on purpose (sorted by pointer) */
    rec **byid = xmalloc(nr * sizeof *byid);
    for (size_t i = 0; i < nr; i++) byid[i] = &R[i];
    qsort(byid, nr, sizeof *byid, cmp_byid);
    for (size_t i = 1; i < nr; i++)
        if (cmp_id(byid[i - 1], byid[i]) == 0) fatal("duplicate_id", "%.*s", (int)byid[i]->f[K_ID].n, byid[i]->f[K_ID].p);

    /* ---- eligibility (CONTRACT.md 5) ---- */
    size_t N = 0; uint64_t T = 0;
    for (size_t i = 0; i < nr; i++) {
        rec *r = &R[i];
        span *f = r->f;
        if (!spaneq(f[K_KIND], kind)) snprintf(r->reason, sizeof r->reason, "kind:%.*s", (int)f[K_KIND].n, f[K_KIND].p);
        else if (!spaneq(f[K_SPLIT], "corpus")) snprintf(r->reason, sizeof r->reason, "split:%.*s", (int)f[K_SPLIT].n, f[K_SPLIT].p);
        else if (!spaneq(f[K_VALIDITY], "verified")) snprintf(r->reason, sizeof r->reason, "validity:%.*s", (int)f[K_VALIDITY].n, f[K_VALIDITY].p);
        if (!r->reason[0] && nexcl) {
            span fam = f[K_FAMILY]; size_t s = 0;
            while (!r->reason[0] && s <= fam.n) {
                size_t e = s; while (e < fam.n && fam.p[e] != ' ') e++;
                for (int x = 0; x < nexcl; x++)
                    if (cmpb(fam.p + s, e - s, excl[x], strlen(excl[x])) == 0) {
                        snprintf(r->reason, sizeof r->reason, "family_excluded:%s", excl[x]); break;
                    }
                s = e + 1;
            }
        }
        if (!r->reason[0] && (memmem(r->body.p, r->body.n, "```", 3) || memmem(f[K_DESC].p, f[K_DESC].n, "```", 3)))
            snprintf(r->reason, sizeof r->reason, "poisoned:fence");
        if (r->reason[0]) continue;
        r->indexed = 1; N++;
        size_t tcap = 0;
        static const int DOC[4] = { K_ENTRY, K_PARAMS, K_RET, K_DESC };
        for (int d = 0; d < 4; d++)
            tokenize(r->low + (f[DOC[d]].p - (const char *)r->raw), f[DOC[d]].n, &r->tok, &r->ntok, &tcap);
        tokenize(r->low + (r->body.p - (const char *)r->raw), r->body.n, &r->tok, &r->ntok, &tcap);
        qsort(r->tok, r->ntok, sizeof *r->tok, cmpspan);
        T += r->ntok;
    }

    /* ---- scoring (CONTRACT.md 6) ---- */
    for (size_t t = 0; t < nq; t++) {
        uint64_t df = 0;
        for (size_t i = 0; i < nr; i++) {
            R[i].tf = R[i].indexed ? count_tok(R[i].tok, R[i].ntok, q[t]) : 0;
            if (R[i].tf) df++;
        }
        if (!df) continue;
        uint64_t idf = log2q16(2 * (uint64_t)N + 2, 2 * df + 1);
        for (size_t i = 0; i < nr; i++) {
            if (!R[i].tf) continue;
            u128 tf = R[i].tf, dl = R[i].ntok;
            u128 num = (u128)idf * tf * 44 * T;
            u128 den = 20 * (u128)T * tf + 6 * (u128)T + 18 * dl * N;
            R[i].score += (uint64_t)(num / den);
        }
    }

    /* ---- ranking and selection ---- */
    rec **ord = xmalloc((N ? N : 1) * sizeof *ord);
    size_t no = 0;
    for (size_t i = 0; i < nr; i++) if (R[i].indexed) ord[no++] = &R[i];
    qsort(ord, no, sizeof *ord, m_irr ? cmp_asc : cmp_desc);
    rec **sel = xmalloc((N + 1) * sizeof *sel);
    size_t ns = 0, total = 0, unpadded = 0, npad = 0, hlen = strlen(HEADING);
    const char *pad_reason = "-";
    if (m_empty) {
        for (size_t i = 0; i < no; i++) strcpy(ord[i]->reason, "ablation_empty");
    } else {
        const char *stop = NULL;
        for (size_t i = 0; i < no; i++) {
            rec *r = ord[i];
            if (!m_irr && r->score == 0) { strcpy(r->reason, "no_match"); continue; }
            if (!stop) {
                if (ns == (size_t)k) stop = "beyond_k";
                else {
                    size_t add = block_len(ns + 1, r) + (ns == 0 ? hlen : 0);
                    if (total + add > (size_t)cap) stop = "cap_bytes";
                    else { total += add; r->selected = 1; sel[ns++] = r; continue; }
                }
            }
            strcpy(r->reason, stop);
        }
        /* length-matched (CONTRACT.md 6): the target is the other arm's realised suffix_bytes. A selection
         * already at or over it is left alone (c_not_shorter). A shorter one gets whole extra records from
         * the rest of its own ranking (rank k+1, k+2, ...; any that would pass the cap is skipped) until the
         * suffix reaches or passes the target (reached) or no candidate is left (exhausted). */
        unpadded = total;
        if (m_len && total >= (size_t)target) pad_reason = "c_not_shorter";
        else if (m_len) {
            pad_reason = "exhausted";
            for (size_t i = 0; i < no && total < (size_t)target; i++) {
                rec *r = ord[i];
                if (strcmp(r->reason, "beyond_k") && strcmp(r->reason, "cap_bytes")) continue;
                size_t add = block_len(ns + 1, r) + (ns == 0 ? hlen : 0);
                if (total + add > (size_t)cap) continue;
                total += add; r->selected = 1; r->pad = 1; r->reason[0] = 0; sel[ns++] = r; npad++;
            }
            if (total >= (size_t)target) pad_reason = "reached";
        }
    }

    /* ---- outputs (CONTRACT.md 8) ---- */
    buf bs = { 0 }, be = { 0 }, bx = { 0 }, bq = { 0 }, br = { 0 }, bh = { 0 };
    bputs(&bs, "rank\tid\tdigest\tscore\tbytes\trole\n");
    if (ns) render_head(&bx);
    for (size_t i = 0; i < ns; i++) {
        size_t before = bx.n;
        render_block(&bx, i + 1, sel[i]);
        bprintf(&bs, "%zu\t", i + 1); bspan(&bs, sel[i]->f[K_ID]);
        bprintf(&bs, "\t%s\t%llu\t%zu\t%s\n", sel[i]->digest, (unsigned long long)sel[i]->score, bx.n - before, sel[i]->pad ? "pad" : "top");
    }
    if (bx.n != total) fatal("internal", "suffix length %zu != accounted %zu", bx.n, total);
    bputs(&be, "id\tdigest\tscore\treason\n");
    for (size_t i = 0; i < nr; i++) {
        rec *r = byid[i];
        if (r->selected) continue;
        bspan(&be, r->f[K_ID]); bprintf(&be, "\t%s\t", r->digest);
        if (r->indexed) bprintf(&be, "%llu", (unsigned long long)r->score); else bputs(&be, "-");
        bprintf(&be, "\t%s\n", r->reason);
    }
    for (size_t i = 0; i < nq; i++) { bspan(&bq, q[i]); bputs(&bq, "\n"); }
    char d_req[65], d_pr[65], d_q[65], d_x[65], d_s[65], d_e[65];
    hexdigest(req, rn, d_req); hexdigest(pr, pn, d_pr); hexdigest(bq.p ? bq.p : "", bq.n, d_q);
    hexdigest(bx.p ? bx.p : "", bx.n, d_x); hexdigest(bs.p, bs.n, d_s); hexdigest(be.p, be.n, d_e);
    char padv[24] = "-", unpv[24] = "-", gapv[24] = "-";
    if (m_len) {
        if (npad) snprintf(padv, sizeof padv, "%zu", npad); else strcpy(padv, "none");
        snprintf(unpv, sizeof unpv, "%zu", unpadded);
        snprintf(gapv, sizeof gapv, "%lld", (long long)bx.n - (long long)target);
    }
    bprintf(&br, "contract=%s\nmode=%s\nkind=%s\nk=%ld\ncap_bytes=%ld\n", CONTRACT, mode, kind, k, cap);
    if (m_len) bprintf(&br, "target_bytes=%ld\n", target); else bputs(&br, "target_bytes=-\n");
    bputs(&br, "exclude_family=");
    if (!nexcl) bputs(&br, "-");
    for (int x = 0; x < nexcl; x++) { if (x) bputs(&br, ","); bputs(&br, excl[x]); }
    bprintf(&br, "\nmanifest_sha256=%s\nrecheck_sha256=%s\nrecheck_source_sha256=%s\n", mdig, rsha, GENRET_RECHECK_SOURCE_SHA256);
    bprintf(&br, "requirement_sha256=%s\nprompt_sha256=%s\nquery_sha256=%s\n", d_req, d_pr, d_q);
    bprintf(&br, "records=%zu\nindexed=%zu\ntotal_tokens=%llu\nselected=%zu\n", nr, N, (unsigned long long)T, ns);
    bprintf(&br, "pad=%s\npad_reason=%s\nunpadded_bytes=%s\nsuffix_bytes=%zu\nlength_gap=%s\n", padv, pad_reason, unpv, bx.n, gapv);
    bprintf(&br, "suffix_sha256=%s\nselected_sha256=%s\nexcluded_sha256=%s\n", d_x, d_s, d_e);
    outfile fs[6] = { { "selected.tsv", &bs }, { "excluded.tsv", &be }, { "suffix.txt", &bx }, { "query.txt", &bq },
                      { "receipt.txt", &br }, { "SHA256SUMS", &bh } };
    for (int i = 0; i < 5; i++) {
        char d[65]; hexdigest(fs[i].b->p ? fs[i].b->p : "", fs[i].b->n, d);
        bprintf(&bh, "%s  %s\n", d, fs[i].name);
    }
    write_out(out, fs, 6);
    return 0;
}
