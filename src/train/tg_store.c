/* tg_store.c -- M22 transactional parameter-state substrate. See tg_store.h. */
#include "train/tg_store.h"
#include "sha256.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define GEN_MAGIC "OMTRGEN1"
#define GEN_VERSION 1u
#define GEN_HDR 152u          /* bytes; digest is the last 32 */
#define GEN_HASHED_HDR 120u   /* header bytes covered by the digest */
#define CUR_MAGIC "OMTRCUR1"
#define CUR_LEN 95u           /* "OMTRCUR1 <20 digits> <64 hex>\n" */
#define REC_MAGIC 0x31444754u /* "TGD1" */
#define REC_SIZE 192u
#define REC_HASHED 160u       /* record bytes before the chain field */
#define PATHMAX 4096

enum { SH_OPEN = 1, SH_COMMITTED = 2, SH_DISCARDED = 3 };

struct tg_store {
    char dir[PATHMAX];
    int lock_fd;
    int log_fd;
    tg_snapshot cur;
    tg_stats stats;
    int poisoned;   /* a switch is visible but not known durable: no commits until reopen */
};

/* ---- fail points ------------------------------------------------------ */
static tg_failpoint g_fp = TG_FP_NONE;
static tg_fpmode g_fpmode = TG_FPMODE_EXIT;

void tg_failpoint_arm(tg_failpoint fp, tg_fpmode mode) { g_fp = fp; g_fpmode = mode; }
void tg_failpoint_disarm(void) { g_fp = TG_FP_NONE; }

static void fp_hit(tg_failpoint fp)
{
    if (g_fp == fp && g_fpmode == TG_FPMODE_EXIT)
        _exit(TG_FP_EXIT_CODE);
}

const char *tg_failpoint_name(tg_failpoint fp)
{
    static const char *n[TG_FP_COUNT] = {
        "none", "before_shadow_write", "mid_shadow_write", "after_shadow_write",
        "after_gen_rename", "mid_switch", "before_switch_rename",
        "after_switch_before_fsync", "after_switch"};
    return (fp >= 0 && fp < TG_FP_COUNT) ? n[fp] : "?";
}

const char *tg_err_name(int e)
{
    switch (e) {
    case TG_OK: return "OK";
    case TG_E_ARG: return "E_ARG";
    case TG_E_IO: return "E_IO";
    case TG_E_NOSTORE: return "E_NOSTORE";
    case TG_E_FORMAT: return "E_FORMAT";
    case TG_E_TRUNC: return "E_TRUNC";
    case TG_E_DIGEST: return "E_DIGEST";
    case TG_E_CHAIN: return "E_CHAIN";
    case TG_E_REJECT: return "E_REJECT";
    case TG_E_STATE: return "E_STATE";
    case TG_E_STALE: return "E_STALE";
    case TG_E_SHADOW: return "E_SHADOW";
    case TG_E_LOCKED: return "E_LOCKED";
    case TG_E_EXISTS: return "E_EXISTS";
    case TG_E_NOMEM: return "E_NOMEM";
    }
    return "E_UNKNOWN";
}

/* ---- small helpers ---------------------------------------------------- */
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get32(const uint8_t *p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static uint64_t get64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }

void tg_hex(const uint8_t d[TG_DIGEST], char out[2 * TG_DIGEST + 1])
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < TG_DIGEST; i++) { out[2 * i] = hx[d[i] >> 4]; out[2 * i + 1] = hx[d[i] & 15]; }
    out[2 * TG_DIGEST] = 0;
}

static int unhex(const char *s, uint8_t d[TG_DIGEST])
{
    for (int i = 0; i < 2 * TG_DIGEST; i++) {
        char c = s[i]; int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else return -1;
        if (i & 1) d[i / 2] = (uint8_t)(d[i / 2] | v); else d[i / 2] = (uint8_t)(v << 4);
    }
    return 0;
}

static int path_of(char *out, const char *dir, const char *name)
{
    int n = snprintf(out, PATHMAX, "%s/%s", dir, name);
    return (n > 0 && n < PATHMAX) ? 0 : -1;
}

static int gen_name(char *out, const char *dir, const char *pfx, uint64_t g, const char *sfx)
{
    int n = snprintf(out, PATHMAX, "%s/%s%020llu%s", dir, pfx, (unsigned long long)g, sfx);
    return (n > 0 && n < PATHMAX) ? 0 : -1;
}

static int write_all(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

static int read_full(int fd, uint8_t *p, size_t n, size_t *got)
{
    size_t t = 0;
    while (t < n) {
        ssize_t r = read(fd, p + t, n - t);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) break;
        t += (size_t)r;
    }
    *got = t;
    return 0;
}

static int fsync_dir(const char *dir)
{
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return -1;
    int r = fsync(fd);
    close(fd);
    return r;
}

static void record_refusal(const char *dir, int err, uint64_t gen, const char *detail)
{
    char p[PATHMAX], line[512];
    if (!dir || path_of(p, dir, "refusals.log")) return;
    int fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    int n = snprintf(line, sizeof line, "REFUSE %s gen=%llu %s\n", tg_err_name(err),
                     (unsigned long long)gen, detail ? detail : "");
    if (n > 0) (void)!write(fd, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
    close(fd);
}

/* ---- generation file -------------------------------------------------- */
static void gen_header(uint8_t h[GEN_HDR], uint64_t gen, uint64_t step, size_t pb, size_t ob,
                       uint64_t dlen, const uint8_t dhead[TG_DIGEST], const uint8_t prev[TG_DIGEST])
{
    memset(h, 0, GEN_HDR);
    memcpy(h, GEN_MAGIC, 8);
    put32(h + 8, GEN_VERSION);
    put32(h + 12, 0);
    put64(h + 16, gen);
    put64(h + 24, step);
    put64(h + 32, (uint64_t)pb);
    put64(h + 40, (uint64_t)ob);
    put64(h + 48, dlen);
    memcpy(h + 56, dhead, TG_DIGEST);
    memcpy(h + 88, prev, TG_DIGEST);
}

static void gen_digest(const uint8_t h[GEN_HDR], const uint8_t *p, size_t pb, const uint8_t *o, size_t ob,
                       uint8_t d[TG_DIGEST])
{
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, h, GEN_HASHED_HDR);
    if (pb) sha256_update(&c, p, pb);
    if (ob) sha256_update(&c, o, ob);
    sha256_final(&c, d);
}

void tg_snapshot_free(tg_snapshot *s)
{
    if (!s) return;
    free(s->params); free(s->opt);
    s->params = s->opt = NULL;
    s->param_bytes = s->opt_bytes = 0;
}

/* Load and fully verify one generation file. expect_digest may be NULL. */
static int load_gen(const char *path, uint64_t expect_gen, const uint8_t *expect_digest, tg_snapshot *out)
{
    memset(out, 0, sizeof *out);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return errno == ENOENT ? TG_E_NOSTORE : TG_E_IO;
    struct stat sb;
    if (fstat(fd, &sb) < 0) { close(fd); return TG_E_IO; }
    uint8_t h[GEN_HDR];
    size_t got;
    if (read_full(fd, h, GEN_HDR, &got) < 0) { close(fd); return TG_E_IO; }
    if (got < GEN_HDR) { close(fd); return TG_E_TRUNC; }
    if (memcmp(h, GEN_MAGIC, 8) || get32(h + 8) != GEN_VERSION || get32(h + 12) != 0) { close(fd); return TG_E_FORMAT; }
    uint64_t pb = get64(h + 32), ob = get64(h + 40);
    /* Each length is capped at 2^40 before the sum, so `want` cannot overflow
     * and the file-size check below bounds every allocation. */
    if (pb > (1ull << 40) || ob > (1ull << 40)) { close(fd); return TG_E_FORMAT; }
    uint64_t want = GEN_HDR + pb + ob;
    if ((uint64_t)sb.st_size < want) { close(fd); return TG_E_TRUNC; }
    if ((uint64_t)sb.st_size > want) { close(fd); return TG_E_FORMAT; }
    if (get64(h + 16) != expect_gen) { close(fd); return TG_E_FORMAT; }
    out->params = malloc(pb ? pb : 1);
    out->opt = malloc(ob ? ob : 1);
    if (!out->params || !out->opt) { close(fd); tg_snapshot_free(out); return TG_E_NOMEM; }
    size_t g1 = 0, g2 = 0;
    if (read_full(fd, out->params, pb, &g1) < 0 || read_full(fd, out->opt, ob, &g2) < 0) {
        close(fd); tg_snapshot_free(out); return TG_E_IO;
    }
    close(fd);
    if (g1 != pb || g2 != ob) { tg_snapshot_free(out); return TG_E_TRUNC; }
    uint8_t d[TG_DIGEST];
    gen_digest(h, out->params, pb, out->opt, ob, d);
    if (memcmp(d, h + 120, TG_DIGEST) || (expect_digest && memcmp(d, expect_digest, TG_DIGEST))) {
        tg_snapshot_free(out); return TG_E_DIGEST;
    }
    out->gen = expect_gen;
    out->step = get64(h + 24);
    out->param_bytes = pb;
    out->opt_bytes = ob;
    out->dispatch_len = get64(h + 48);
    memcpy(out->dispatch_head, h + 56, TG_DIGEST);
    memcpy(out->prev_digest, h + 88, TG_DIGEST);
    memcpy(out->digest, d, TG_DIGEST);
    return TG_OK;
}

/* Write a complete generation file at `path` (fails at the injected points). */
static int write_gen_file(const char *path, const uint8_t h[GEN_HDR], const uint8_t *p, size_t pb,
                          const uint8_t *o, size_t ob, int with_fp)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return TG_E_IO;
    if (write_all(fd, h, GEN_HDR)) goto io;
    if (with_fp && g_fp == TG_FP_MID_SHADOW_WRITE) {
        size_t half = pb / 2;
        if (write_all(fd, p, half)) goto io;
        fp_hit(TG_FP_MID_SHADOW_WRITE);
        if (write_all(fd, p + half, pb - half)) goto io;
    } else if (pb && write_all(fd, p, pb)) goto io;
    if (ob && write_all(fd, o, ob)) goto io;
    if (fsync(fd)) goto io;
    return close(fd) ? TG_E_IO : TG_OK;
io:
    close(fd);
    return TG_E_IO;
}

static int write_pointer(const char *dir, uint64_t gen, const uint8_t d[TG_DIGEST], int with_fp)
{
    char tmp[PATHMAX], cur[PATHMAX], hx[2 * TG_DIGEST + 1], buf[CUR_LEN + 1];
    if (path_of(tmp, dir, "CURRENT.tmp") || path_of(cur, dir, "CURRENT")) return TG_E_ARG;
    tg_hex(d, hx);
    int n = snprintf(buf, sizeof buf, CUR_MAGIC " %020llu %s\n", (unsigned long long)gen, hx);
    if (n != (int)CUR_LEN) return TG_E_ARG;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return TG_E_IO;
    if (with_fp && g_fp == TG_FP_MID_SWITCH) {
        if (write_all(fd, (const uint8_t *)buf, CUR_LEN / 2)) { close(fd); return TG_E_IO; }
        fp_hit(TG_FP_MID_SWITCH);
        if (write_all(fd, (const uint8_t *)buf + CUR_LEN / 2, CUR_LEN - CUR_LEN / 2)) { close(fd); return TG_E_IO; }
    } else if (write_all(fd, (const uint8_t *)buf, CUR_LEN)) { close(fd); return TG_E_IO; }
    if (fsync(fd) || close(fd)) return TG_E_IO;
    if (with_fp) fp_hit(TG_FP_BEFORE_SWITCH_RENAME);
    if (rename(tmp, cur)) return TG_E_IO;           /* THE atomic switch */
    if (with_fp) fp_hit(TG_FP_AFTER_SWITCH_BEFORE_FSYNC);
    if (fsync_dir(dir)) return 1;               /* switched; directory fsync failed */
    return TG_OK;
}

static int read_pointer(const char *dir, uint64_t *gen, uint8_t d[TG_DIGEST])
{
    char p[PATHMAX];
    char buf[CUR_LEN + 8];
    if (path_of(p, dir, "CURRENT")) return TG_E_ARG;
    int fd = open(p, O_RDONLY);
    if (fd < 0) return errno == ENOENT ? TG_E_NOSTORE : TG_E_IO;
    size_t got;
    int r = read_full(fd, (uint8_t *)buf, sizeof buf, &got);
    close(fd);
    if (r < 0) return TG_E_IO;
    if (got != CUR_LEN || memcmp(buf, CUR_MAGIC " ", 9) || buf[29] != ' ' || buf[CUR_LEN - 1] != '\n')
        return TG_E_FORMAT;
    uint64_t g = 0;
    for (int i = 9; i < 29; i++) {
        if (buf[i] < '0' || buf[i] > '9') return TG_E_FORMAT;
        g = g * 10 + (uint64_t)(buf[i] - '0');
    }
    if (unhex(buf + 30, d)) return TG_E_FORMAT;
    *gen = g;
    return TG_OK;
}

/* ---- dispatch chain --------------------------------------------------- */
static void chain_step(const uint8_t prev[TG_DIGEST], const uint8_t *rec, uint8_t out[TG_DIGEST])
{
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, prev, TG_DIGEST);
    sha256_update(&c, rec, REC_HASHED);
    sha256_final(&c, out);
}

int tg_dispatch_chain(const char *dir, uint64_t len, uint8_t head[TG_DIGEST], uint64_t *n_records)
{
    char p[PATHMAX];
    if (len % REC_SIZE) return TG_E_CHAIN;
    if (path_of(p, dir, "dispatch.log")) return TG_E_ARG;
    memset(head, 0, TG_DIGEST);
    if (n_records) *n_records = 0;
    int fd = open(p, O_RDONLY);
    if (fd < 0) return len == 0 ? TG_OK : TG_E_CHAIN;
    uint8_t rec[REC_SIZE];
    for (uint64_t i = 0; i < len / REC_SIZE; i++) {
        size_t got;
        if (read_full(fd, rec, REC_SIZE, &got) < 0 || got != REC_SIZE) { close(fd); return TG_E_CHAIN; }
        if (get32(rec) != REC_MAGIC || get64(rec + 8) != i) { close(fd); return TG_E_CHAIN; }
        uint8_t nh[TG_DIGEST];
        chain_step(head, rec, nh);
        if (memcmp(nh, rec + REC_HASHED, TG_DIGEST)) { close(fd); return TG_E_CHAIN; }
        memcpy(head, nh, TG_DIGEST);
    }
    close(fd);
    if (n_records) *n_records = len / REC_SIZE;
    return TG_OK;
}

/* ---- create / open / close -------------------------------------------- */
static int take_lock(const char *dir, int *fd_out)
{
    char p[PATHMAX];
    if (path_of(p, dir, "LOCK")) return TG_E_ARG;
    int fd = open(p, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return TG_E_IO;
    if (flock(fd, LOCK_EX | LOCK_NB)) { close(fd); return TG_E_LOCKED; }
    *fd_out = fd;
    return TG_OK;
}

int tg_create(const char *dir, const void *params, size_t pb, const void *opt, size_t ob)
{
    char p[PATHMAX], g[PATHMAX];
    uint64_t gen;
    uint8_t d[TG_DIGEST], zero[TG_DIGEST] = {0};
    if (!dir || (pb && !params) || (ob && !opt)) return TG_E_ARG;
    if (mkdir(dir, 0755) && errno != EEXIST) return TG_E_IO;
    if (read_pointer(dir, &gen, d) != TG_E_NOSTORE) return TG_E_EXISTS;
    int lk;
    int r = take_lock(dir, &lk);
    if (r) return r;
    if (path_of(p, dir, "dispatch.log")) { close(lk); return TG_E_ARG; }
    int lf = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (lf < 0 || fsync(lf)) { if (lf >= 0) close(lf); close(lk); return TG_E_IO; }
    close(lf);
    uint8_t h[GEN_HDR];
    gen_header(h, 0, 0, pb, ob, 0, zero, zero);
    gen_digest(h, params, pb, opt, ob, d);
    memcpy(h + 120, d, TG_DIGEST);
    if (gen_name(p, dir, "shadow-", 0, ".tmp") || gen_name(g, dir, "gen-", 0, ".bin")) { close(lk); return TG_E_ARG; }
    r = write_gen_file(p, h, params, pb, opt, ob, 0);
    if (!r && rename(p, g)) r = TG_E_IO;
    if (!r && fsync_dir(dir)) r = TG_E_IO;
    if (!r) r = write_pointer(dir, 0, d, 0);
    close(lk);
    return r;
}

/* Remove leftovers of an interrupted commit: *.tmp and generations newer than CURRENT. */
static int sweep(const char *dir, uint64_t cur_gen, tg_recovery *rec)
{
    DIR *dp = opendir(dir);
    if (!dp) return TG_E_IO;
    struct dirent *e;
    char p[PATHMAX];
    int changed = 0;
    while ((e = readdir(dp))) {
        const char *n = e->d_name;
        size_t l = strlen(n);
        if (l > 4 && !strcmp(n + l - 4, ".tmp")) {
            if (!path_of(p, dir, n) && !unlink(p)) { rec->removed_tmp++; changed = 1; }
        } else if (l == 28 && !strncmp(n, "gen-", 4) && !strcmp(n + 24, ".bin")) {
            uint64_t g = strtoull(n + 4, NULL, 10);
            if (g > cur_gen && !path_of(p, dir, n) && !unlink(p)) { rec->removed_orphan_gen++; changed = 1; }
        }
    }
    closedir(dp);
    if (changed && fsync_dir(dir)) return TG_E_IO;
    return TG_OK;
}

int tg_open(const char *dir, tg_store **out, tg_recovery *rec_out)
{
    tg_recovery rec = {0, 0, 0};
    char p[PATHMAX];
    uint64_t gen;
    uint8_t pd[TG_DIGEST];
    if (!dir || !out) return TG_E_ARG;
    *out = NULL;
    tg_store *st = calloc(1, sizeof *st);
    if (!st) return TG_E_NOMEM;
    st->lock_fd = st->log_fd = -1;
    if (snprintf(st->dir, sizeof st->dir, "%s", dir) >= (int)sizeof st->dir) { free(st); return TG_E_ARG; }
    int r = take_lock(dir, &st->lock_fd);
    if (r) { free(st); return r; }
    r = read_pointer(dir, &gen, pd);
    if (!r && gen_name(p, dir, "gen-", gen, ".bin")) r = TG_E_ARG;
    if (!r) r = load_gen(p, gen, pd, &st->cur);
    if (r) {
        char det[64];
        snprintf(det, sizeof det, "open: committed generation does not verify");
        record_refusal(dir, r, gen, det);
        tg_close(st);
        return r;
    }
    /* Dispatch log: verify the committed prefix first. Nothing in the store is
     * modified until the committed generation AND its chain both verify. */
    if (path_of(p, dir, "dispatch.log")) { tg_close(st); return TG_E_ARG; }
    st->log_fd = open(p, O_RDWR);
    struct stat sb;
    if (st->log_fd < 0 || fstat(st->log_fd, &sb)) { tg_close(st); return TG_E_IO; }
    if ((uint64_t)sb.st_size < st->cur.dispatch_len) {
        record_refusal(dir, TG_E_CHAIN, gen, "open: dispatch log shorter than committed length");
        tg_close(st);
        return TG_E_CHAIN;
    }
    uint8_t head[TG_DIGEST];
    r = tg_dispatch_chain(dir, st->cur.dispatch_len, head, NULL);
    if (!r && memcmp(head, st->cur.dispatch_head, TG_DIGEST)) r = TG_E_CHAIN;
    if (r) {
        record_refusal(dir, r, gen, "open: dispatch chain does not match committed head");
        tg_close(st);
        return r;
    }
    /* Then drop leftovers of an interrupted commit. */
    if ((uint64_t)sb.st_size > st->cur.dispatch_len) {
        rec.dispatch_truncated = (uint64_t)sb.st_size - st->cur.dispatch_len;
        if (ftruncate(st->log_fd, (off_t)st->cur.dispatch_len) || fsync(st->log_fd)) { tg_close(st); return TG_E_IO; }
    }
    if ((r = sweep(dir, gen, &rec))) { tg_close(st); return r; }
    if (rec_out) *rec_out = rec;
    *out = st;
    return TG_OK;
}

void tg_close(tg_store *st)
{
    if (!st) return;
    tg_snapshot_free(&st->cur);
    if (st->log_fd >= 0) close(st->log_fd);
    if (st->lock_fd >= 0) close(st->lock_fd);
    free(st);
}

const tg_snapshot *tg_committed(const tg_store *st) { return &st->cur; }
const tg_stats *tg_get_stats(const tg_store *st) { return &st->stats; }

/* ---- shadow ----------------------------------------------------------- */
int tg_shadow_begin(tg_store *st, tg_shadow *sh)
{
    if (!st || !sh) return TG_E_ARG;
    memset(sh, 0, sizeof *sh);
    sh->params = malloc(st->cur.param_bytes ? st->cur.param_bytes : 1);
    sh->opt = malloc(st->cur.opt_bytes ? st->cur.opt_bytes : 1);
    if (!sh->params || !sh->opt) { free(sh->params); free(sh->opt); return TG_E_NOMEM; }
    memcpy(sh->params, st->cur.params, st->cur.param_bytes);
    memcpy(sh->opt, st->cur.opt, st->cur.opt_bytes);
    sh->param_bytes = st->cur.param_bytes;
    sh->opt_bytes = st->cur.opt_bytes;
    sh->base_gen = st->cur.gen;
    sh->base_step = st->cur.step;
    memcpy(sh->base_digest, st->cur.digest, TG_DIGEST);
    sh->recs = NULL;
    sh->rec_cap = 0;
    sh->state = SH_OPEN;
    return TG_OK;
}

void tg_shadow_discard(tg_shadow *sh)
{
    if (!sh) return;
    free(sh->params); free(sh->opt); free(sh->recs);
    sh->params = sh->opt = sh->recs = NULL;
    sh->n_dispatch = sh->rec_cap = 0;
    if (sh->state == SH_OPEN) sh->state = SH_DISCARDED;
}

int tg_dispatch(tg_store *st, tg_shadow *sh, uint32_t op_id, uint64_t arg0, uint64_t arg1,
                const tg_ref *refs, uint32_t n_refs)
{
    if (!st || !sh || n_refs > TG_MAX_REFS || (n_refs && !refs)) return TG_E_ARG;
    if (sh->state != SH_OPEN) return TG_E_STATE;
    if (sh->n_dispatch == sh->rec_cap) {
        uint32_t nc = sh->rec_cap ? sh->rec_cap * 2 : 8;
        uint8_t *n = realloc(sh->recs, (size_t)nc * REC_SIZE);
        if (!n) return TG_E_NOMEM;
        sh->recs = n; sh->rec_cap = nc;
    }
    uint8_t *r = sh->recs + (size_t)sh->n_dispatch * REC_SIZE;
    memset(r, 0, REC_SIZE);
    put32(r, REC_MAGIC);
    put32(r + 4, op_id);
    /* r+8 seq: assigned at commit */
    put64(r + 16, sh->base_step + 1);
    put64(r + 24, sh->base_gen);
    put64(r + 32, arg0);
    put64(r + 40, arg1);
    put32(r + 48, n_refs);
    for (uint32_t i = 0; i < n_refs; i++) {
        uint8_t *q = r + 56 + 24 * i;
        put32(q, refs[i].buf);
        put64(q + 8, refs[i].off);
        put64(q + 16, refs[i].len);
    }
    memcpy(r + 128, sh->base_digest, TG_DIGEST); /* inputs by reference to the base generation */
    sh->n_dispatch++;
    return TG_OK;
}

/* ---- commit ----------------------------------------------------------- */
static int refuse(tg_store *st, tg_shadow *sh, int err, const char *detail)
{
    st->stats.refusals++;
    record_refusal(st->dir, err, st->cur.gen, detail);
    if (sh && sh->state == SH_OPEN) tg_shadow_discard(sh);
    return err;
}

int tg_commit(tg_store *st, tg_shadow *sh, tg_validate_fn validate, void *ctx)
{
    char sp[PATHMAX], gp[PATHMAX];
    if (!st || !sh) return TG_E_ARG;
    if (st->poisoned)
        return refuse(st, sh, TG_E_STATE, "commit: previous switch not known durable; reopen the store to recover");
    if (sh->state != SH_OPEN) {
        st->stats.refusals++;
        record_refusal(st->dir, TG_E_STATE, st->cur.gen, "commit: shadow already committed or discarded (double switch)");
        return TG_E_STATE;
    }
    if (sh->base_gen != st->cur.gen || memcmp(sh->base_digest, st->cur.digest, TG_DIGEST))
        return refuse(st, sh, TG_E_STALE, "commit: shadow base is not the current generation");
    if (sh->param_bytes != st->cur.param_bytes || sh->opt_bytes != st->cur.opt_bytes)
        return refuse(st, sh, TG_E_ARG, "commit: shadow size differs from committed state");
    if (validate && validate(sh, &st->cur, ctx) != 0)
        return refuse(st, sh, TG_E_REJECT, "commit: validation callback rejected the shadow");

    uint64_t ng = st->cur.gen + 1, nstep = st->cur.step + 1;
    uint64_t base_len = st->cur.dispatch_len;
    uint8_t head[TG_DIGEST];
    memcpy(head, st->cur.dispatch_head, TG_DIGEST);

    /* Tier (a): append this step's dispatch records, chained, then fsync. */
    for (uint32_t i = 0; i < sh->n_dispatch; i++) {
        uint8_t *r = sh->recs + (size_t)i * REC_SIZE;
        put64(r + 8, base_len / REC_SIZE + i);
        chain_step(head, r, r + REC_HASHED);
        memcpy(head, r + REC_HASHED, TG_DIGEST);
        st->stats.dispatch_records++;
        st->stats.dispatch_hashed_bytes += TG_DIGEST + REC_HASHED;
    }
    uint64_t new_len = base_len + (uint64_t)sh->n_dispatch * REC_SIZE;
    if (sh->n_dispatch) {
        size_t n = (size_t)sh->n_dispatch * REC_SIZE;
        if (pwrite(st->log_fd, sh->recs, n, (off_t)base_len) != (ssize_t)n || fsync(st->log_fd))
            goto io_undo;
    }

    /* Tier (b): full digest of the new state, only here. */
    uint8_t h[GEN_HDR], d[TG_DIGEST];
    gen_header(h, ng, nstep, sh->param_bytes, sh->opt_bytes, new_len, head, st->cur.digest);
    gen_digest(h, sh->params, sh->param_bytes, sh->opt, sh->opt_bytes, d);
    memcpy(h + 120, d, TG_DIGEST);
    st->stats.commit_hashed_bytes += GEN_HASHED_HDR + sh->param_bytes + sh->opt_bytes;

    if (gen_name(sp, st->dir, "shadow-", ng, ".tmp") || gen_name(gp, st->dir, "gen-", ng, ".bin"))
        return refuse(st, sh, TG_E_ARG, "commit: path too long");
    fp_hit(TG_FP_BEFORE_SHADOW_WRITE);
    if (write_gen_file(sp, h, sh->params, sh->param_bytes, sh->opt, sh->opt_bytes, 1)) goto io_undo;

    /* Read the shadow file back and verify it before it can become a generation. */
    if (g_fp == TG_FP_AFTER_SHADOW_WRITE && g_fpmode == TG_FPMODE_CORRUPT) {
        int fd = open(sp, O_RDWR);
        uint8_t b;
        off_t at = GEN_HDR + (off_t)(sh->param_bytes / 3);
        if (fd >= 0 && pread(fd, &b, 1, at) == 1) { b ^= 0x5a; (void)!pwrite(fd, &b, 1, at); }
        if (fd >= 0) close(fd);
    }
    {
        tg_snapshot chk;
        int vr = load_gen(sp, ng, d, &chk);
        tg_snapshot_free(&chk);
        if (vr) {
            unlink(sp);
            (void)!ftruncate(st->log_fd, (off_t)base_len);
            (void)!fsync(st->log_fd);
            return refuse(st, sh, TG_E_SHADOW, "commit: shadow file read-back did not verify (corrupted shadow)");
        }
    }
    fp_hit(TG_FP_AFTER_SHADOW_WRITE);
    if (rename(sp, gp)) goto io_undo;
    if (fsync_dir(st->dir)) { unlink(gp); goto io_undo; }
    fp_hit(TG_FP_AFTER_GEN_RENAME);
    int wp = write_pointer(st->dir, ng, d, 1);
    if (wp < 0) { unlink(gp); goto io_undo; }
    if (wp == 0) fp_hit(TG_FP_AFTER_SWITCH);

    /* Adopt the shadow as the committed in-memory state. */
    tg_snapshot_free(&st->cur);
    st->cur.gen = ng;
    st->cur.step = nstep;
    memcpy(st->cur.prev_digest, sh->base_digest, TG_DIGEST);
    memcpy(st->cur.digest, d, TG_DIGEST);
    memcpy(st->cur.dispatch_head, head, TG_DIGEST);
    st->cur.dispatch_len = new_len;
    st->cur.params = sh->params; st->cur.param_bytes = sh->param_bytes;
    st->cur.opt = sh->opt; st->cur.opt_bytes = sh->opt_bytes;
    sh->params = sh->opt = NULL;
    free(sh->recs); sh->recs = NULL;
    sh->state = SH_COMMITTED;
    if (wp > 0) {
        st->poisoned = 1;
        /* The switch is visible (readers see NEW) but its directory entry may not
         * be durable yet: report it, never undo it. */
        st->stats.refusals++;
        record_refusal(st->dir, TG_E_IO, ng, "commit: switched, directory fsync failed (durability unknown)");
        return TG_E_IO;
    }
    return TG_OK;

io_undo:
    unlink(sp);
    (void)!ftruncate(st->log_fd, (off_t)base_len);
    return refuse(st, sh, TG_E_IO, "commit: I/O error before switch");
}

/* ---- readers ---------------------------------------------------------- */
int tg_read_current(const char *dir, tg_snapshot *out)
{
    char p[PATHMAX];
    uint64_t gen;
    uint8_t d[TG_DIGEST];
    if (!dir || !out) return TG_E_ARG;
    memset(out, 0, sizeof *out);
    int r = read_pointer(dir, &gen, d);
    if (r) return r;
    if (gen_name(p, dir, "gen-", gen, ".bin")) return TG_E_ARG;
    return load_gen(p, gen, d, out);
}

int tg_verify_history(const char *dir, uint64_t *n_verified)
{
    char p[PATHMAX];
    uint64_t gen;
    uint8_t d[TG_DIGEST], zero[TG_DIGEST] = {0};
    if (n_verified) *n_verified = 0;
    int r = read_pointer(dir, &gen, d);
    if (r) return r;
    uint64_t later_len = UINT64_MAX;
    for (;;) {
        tg_snapshot s;
        if (gen_name(p, dir, "gen-", gen, ".bin")) return TG_E_ARG;
        if ((r = load_gen(p, gen, d, &s))) return r;
        int bad = s.dispatch_len > later_len || (gen == 0 && (memcmp(s.prev_digest, zero, TG_DIGEST) || s.step != 0));
        later_len = s.dispatch_len;
        memcpy(d, s.prev_digest, TG_DIGEST);
        tg_snapshot_free(&s);
        if (bad) return TG_E_CHAIN;
        if (n_verified) (*n_verified)++;
        if (gen == 0) return TG_OK;
        gen--;
    }
}

int tg_dispatch_read(const char *dir, uint64_t seq, tg_dispatch_rec *out)
{
    uint8_t head[TG_DIGEST];
    char p[PATHMAX];
    if (!dir || !out || seq > (UINT64_MAX / REC_SIZE) - 1) return TG_E_ARG;
    int r = tg_dispatch_chain(dir, (seq + 1) * REC_SIZE, head, NULL);
    if (r) return r;
    if (path_of(p, dir, "dispatch.log")) return TG_E_ARG;
    int fd = open(p, O_RDONLY);
    if (fd < 0) return TG_E_IO;
    uint8_t rec[REC_SIZE];
    ssize_t g = pread(fd, rec, REC_SIZE, (off_t)(seq * REC_SIZE));
    close(fd);
    if (g != REC_SIZE) return TG_E_CHAIN;
    memset(out, 0, sizeof *out);
    out->op_id = get32(rec + 4);
    out->seq = get64(rec + 8);
    out->step = get64(rec + 16);
    out->base_gen = get64(rec + 24);
    out->arg0 = get64(rec + 32);
    out->arg1 = get64(rec + 40);
    out->n_refs = get32(rec + 48);
    if (out->n_refs > TG_MAX_REFS) return TG_E_FORMAT;
    for (uint32_t i = 0; i < out->n_refs; i++) {
        const uint8_t *q = rec + 56 + 24 * i;
        out->refs[i].buf = get32(q);
        out->refs[i].off = get64(q + 8);
        out->refs[i].len = get64(q + 16);
    }
    memcpy(out->base_digest, rec + 128, TG_DIGEST);
    memcpy(out->chain, rec + REC_HASHED, TG_DIGEST);
    return TG_OK;
}

int tg_read_gen(const char *dir, uint64_t gen, const uint8_t *expect_digest, tg_snapshot *out)
{
    char p[PATHMAX];
    if (!dir || !out) return TG_E_ARG;
    memset(out, 0, sizeof *out);
    if (gen_name(p, dir, "gen-", gen, ".bin")) return TG_E_ARG;
    return load_gen(p, gen, expect_digest, out);
}
