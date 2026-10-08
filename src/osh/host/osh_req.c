/*
 * osh_req.c -- request record decoder and builder (OSH_PLATFORM_ABI.md sections 3, 7.1, 11).
 * The decoder is the only way bytes reach the executor: the record path and the direct-argv path both end here.
 */
#include "osh_host.h"

#include <string.h>

#define CMD_BASE(i) (OSH_REQ_HDR_CELLS + (size_t)(i) * OSH_CMD_CELLS)
#define ARGV_OFF 4
#define ASSIGN_OFF (4 + 2 * OSH_MAX_ARGV)
#define REDIR_OFF (ASSIGN_OFF + 4 * OSH_MAX_ASSIGN)

int osh_name_valid(const char *s)
{
    if (!s || !(*s == '_' || (*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z'))) return 0;
    for (const char *p = s + 1; *p; p++)
        if (!(*p == '_' || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))) return 0;
    return 1;
}

/* copy out[off..off+len) into the arena as a NUL terminated string; NULL on any violation */
static const char *take(OshRequest *r, const uint64_t *out, uint64_t used, uint64_t off, uint64_t len, int *nul_ok)
{
    (void)nul_ok;
    if (off > used || len > used - off) return NULL;
    if (r->arena_used + len + 1 > sizeof r->arena) return NULL;
    char *d = r->arena + r->arena_used;
    for (uint64_t i = 0; i < len; i++) {
        uint64_t v = out[off + i];
        if (v == 0 || v > 255) return NULL; /* NUL is never valid in an argument, name, value or path */
        d[i] = (char)v;
    }
    d[len] = 0;
    r->arena_used += len + 1;
    return d;
}

int osh_req_decode(const uint64_t *rec, size_t nrec, const uint64_t *out, size_t nout, OshRequest *r)
{
    if (!rec || nrec < OSH_REQ_HDR_CELLS) return OSH_ABI_LENGTH;
    if (rec[0] != OSH_REQ_MAGIC) return OSH_ABI_MAGIC;
    if (rec[5] != OSH_REQ_VERSION) return OSH_ABI_VERSION;
    if (rec[6] != 0 || rec[7] != 0 || (rec[3] & ~(uint64_t)OSH_REQ_FLAG_DIRECT)) return OSH_ABI_RESERVED;
    uint64_t ncmds = rec[1], conn = rec[2], used = rec[4];
    if (ncmds < 1 || ncmds > OSH_MAX_CMDS || conn > OSH_CONN_OR) return OSH_REQ_FIELD;
    if (nrec < OSH_REQ_HDR_CELLS + ncmds * OSH_CMD_CELLS) return OSH_ABI_LENGTH;
    if (used > OSH_OUT_CAP) return OSH_REQ_FIELD;
    if (used > 0 && (!out || nout < used)) return OSH_ABI_LENGTH;

    memset(r, 0, sizeof *r);
    r->ncmds = (int)ncmds;
    r->connector_after = (int)conn;
    r->flags = (unsigned)rec[3];
    for (uint64_t c = 0; c < ncmds; c++) {
        const uint64_t *b = rec + CMD_BASE(c);
        OshCmd *k = &r->cmd[c];
        uint64_t na = b[0], ns = b[1], nr = b[2], bi = b[3];
        if (na > OSH_MAX_ARGV || ns > OSH_MAX_ASSIGN || nr > OSH_MAX_REDIR || bi > OSH_B_EXIT) return OSH_REQ_FIELD;
        if (bi != 0 && (na == 0 || (r->flags & OSH_REQ_FLAG_DIRECT))) return OSH_REQ_FIELD;
        for (uint64_t i = na; i < OSH_MAX_ARGV; i++)
            if (b[ARGV_OFF + 2 * i] || b[ARGV_OFF + 2 * i + 1]) return OSH_ABI_RESERVED;
        for (uint64_t i = ns; i < OSH_MAX_ASSIGN; i++)
            for (int j = 0; j < 4; j++)
                if (b[ASSIGN_OFF + 4 * i + j]) return OSH_ABI_RESERVED;
        for (uint64_t i = nr; i < OSH_MAX_REDIR; i++)
            for (int j = 0; j < 4; j++)
                if (b[REDIR_OFF + 4 * i + j]) return OSH_ABI_RESERVED;
        k->builtin_id = (int)bi;
        k->nargv = (int)na;
        k->nassign = (int)ns;
        k->nredir = (int)nr;
        for (uint64_t i = 0; i < na; i++) {
            k->argv[i] = take(r, out, used, b[ARGV_OFF + 2 * i], b[ARGV_OFF + 2 * i + 1], NULL);
            if (!k->argv[i]) return OSH_REQ_FIELD;
        }
        k->argv[na] = NULL;
        for (uint64_t i = 0; i < ns; i++) {
            const uint64_t *e = b + ASSIGN_OFF + 4 * i;
            if (e[1] == 0) return OSH_REQ_FIELD;
            k->assign[i].name = take(r, out, used, e[0], e[1], NULL);
            k->assign[i].value = take(r, out, used, e[2], e[3], NULL);
            if (!k->assign[i].name || !k->assign[i].value || !osh_name_valid(k->assign[i].name)) return OSH_REQ_FIELD;
        }
        for (uint64_t i = 0; i < nr; i++) {
            const uint64_t *e = b + REDIR_OFF + 4 * i;
            OshRedir *d = &k->redir[i];
            if (e[0] < OSH_R_IN || e[0] > OSH_R_DUP || e[1] > 2) return OSH_REQ_FIELD;
            d->kind = (int)e[0];
            d->fd = (int)e[1];
            if (e[0] == OSH_R_DUP) {
                if (e[2] > 2 || e[3] != 0) return OSH_REQ_FIELD;
                d->src_fd = (int)e[2];
            } else {
                d->path = take(r, out, used, e[2], e[3], NULL);
                if (!d->path) return OSH_REQ_FIELD;
            }
        }
    }
    return 0;
}

/* ---- builder ---- */

void osh_rb_init(OshBuilder *b, unsigned flags, int connector_after)
{
    memset(b, 0, sizeof *b);
    b->rec[2] = (uint64_t)connector_after;
    b->rec[3] = flags;
}

void osh_rb_cmd(OshBuilder *b, int builtin_id)
{
    if (b->ncmds >= OSH_MAX_CMDS) { b->err = OSH_REQ_FIELD; return; }
    b->rec[CMD_BASE(b->ncmds) + 3] = (uint64_t)builtin_id;
    b->ncmds++;
}

static int put_str(OshBuilder *b, const char *s, uint64_t *off, uint64_t *len)
{
    size_t n = strlen(s);
    if (b->out_used + n > OSH_OUT_CAP) { b->err = OSH_REQ_FIELD; return -1; }
    *off = b->out_used;
    *len = n;
    for (size_t i = 0; i < n; i++) b->out[b->out_used + i] = (unsigned char)s[i];
    b->out_used += n;
    return 0;
}

void osh_rb_arg(OshBuilder *b, const char *s)
{
    if (b->ncmds == 0) { b->err = OSH_REQ_FIELD; return; }
    uint64_t *blk = b->rec + CMD_BASE(b->ncmds - 1), off, len;
    if (blk[0] >= OSH_MAX_ARGV) { b->err = OSH_REQ_FIELD; return; }
    if (put_str(b, s, &off, &len)) return;
    blk[ARGV_OFF + 2 * blk[0]] = off;
    blk[ARGV_OFF + 2 * blk[0] + 1] = len;
    blk[0]++;
}

void osh_rb_assign(OshBuilder *b, const char *name, const char *value)
{
    if (b->ncmds == 0) { b->err = OSH_REQ_FIELD; return; }
    uint64_t *blk = b->rec + CMD_BASE(b->ncmds - 1), no, nl, vo, vl;
    if (blk[1] >= OSH_MAX_ASSIGN) { b->err = OSH_REQ_FIELD; return; }
    if (put_str(b, name, &no, &nl) || put_str(b, value, &vo, &vl)) return;
    uint64_t *e = blk + ASSIGN_OFF + 4 * blk[1];
    e[0] = no; e[1] = nl; e[2] = vo; e[3] = vl;
    blk[1]++;
}

void osh_rb_redir(OshBuilder *b, int kind, int fd, const char *path, int src_fd)
{
    if (b->ncmds == 0) { b->err = OSH_REQ_FIELD; return; }
    uint64_t *blk = b->rec + CMD_BASE(b->ncmds - 1), off = 0, len = 0;
    if (blk[2] >= OSH_MAX_REDIR) { b->err = OSH_REQ_FIELD; return; }
    if (kind == OSH_R_DUP) off = (uint64_t)src_fd;
    else if (!path || put_str(b, path, &off, &len)) { if (!path) b->err = OSH_REQ_FIELD; return; }
    uint64_t *e = blk + REDIR_OFF + 4 * blk[2];
    e[0] = (uint64_t)kind; e[1] = (uint64_t)fd; e[2] = off; e[3] = len;
    blk[2]++;
}

size_t osh_rb_cells(const OshBuilder *b) { return OSH_REQ_HDR_CELLS + (size_t)b->ncmds * OSH_CMD_CELLS; }

void osh_rb_seal(OshBuilder *b)
{
    b->rec[0] = OSH_REQ_MAGIC;
    b->rec[1] = (uint64_t)b->ncmds;
    b->rec[4] = b->out_used;
    b->rec[5] = OSH_REQ_VERSION;
}
