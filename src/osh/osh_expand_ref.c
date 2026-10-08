/* osh_expand_ref.c -- see osh_expand_ref.h. */
#include "osh_expand_ref.h"

#include <stdio.h>
#include <string.h>

#define CAP_FIELDS 32
#define CAP_VALUE 1024
#define CAP_VARREQ 64

typedef struct {
    const uint8_t *in;
    const OshRefLex *lx;
    const OshXrEnv *env;
    OshXrOut *o;
    unsigned status; /* 0 while fine */
    uint64_t woff;   /* start offset of the word being expanded */
    int nvreq;
    /* the command being built */
    uint64_t *blk;
    int mode;               /* 1 argv, 2 assignment, 3 redirection target */
    size_t fstart;          /* start of the open field in out */
    int exists;             /* a field is open */
    int nfields_word;       /* redirection target fields */
    uint64_t a0off;
    int standalone;         /* the command has no command word */
    struct {
        uint8_t name[256];
        size_t nlen;
        size_t voff, vlen;
    } prior[16];
    int nprior;
} Cx;

static void err(Cx *c, unsigned code)
{
    if (c->status) return;
    c->status = code;
    c->o->err_off = c->woff;
}

static void put(Cx *c, uint8_t b)
{
    if (c->status) return;
    if (c->o->out_used >= OSH_XR_OUTCAP) { err(c, 210); return; }
    if (!c->exists && c->mode != 2) { c->exists = 1; c->fstart = c->o->out_used; }
    c->o->out[c->o->out_used++] = b;
}

static void push_field(Cx *c)
{
    if (c->status || c->mode == 2 || !c->exists) return;
    c->exists = 0;
    size_t len = c->o->out_used - c->fstart;
    if (c->mode == 1) {
        uint64_t n = c->blk[0];
        if (n >= CAP_FIELDS) { err(c, 209); return; }
        if (n == 0) c->a0off = c->woff;
        c->blk[4 + 2 * n] = c->fstart;
        c->blk[5 + 2 * n] = len;
        c->blk[0] = n + 1;
    } else {
        if (c->nfields_word) { err(c, 243); return; }
        c->nfields_word = 1;
        uint64_t r = c->blk[2] - 1;
        c->blk[4 + 64 + 64 + 4 * r + 2] = c->fstart;
        c->blk[4 + 64 + 64 + 4 * r + 3] = len;
    }
}

static void mark(Cx *c)
{
    if (c->status || c->mode == 2) return;
    if (!c->exists) { c->exists = 1; c->fstart = c->o->out_used; }
}

/* quoted or literal text: always part of the current field, even when empty */
static void piece_text(Cx *c, const uint8_t *b, size_t n)
{
    mark(c);
    for (size_t i = 0; i < n && !c->status; i++) {
        if (b[i] == 0) { err(c, 244); return; }
        put(c, b[i]);
    }
}

/* the result of an unquoted expansion */
static void piece_split(Cx *c, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n && !c->status; i++) {
        uint8_t ch = b[i];
        if (ch == 0) err(c, 244);
        else if (ch == 0x20 || ch == 9 || ch == 10) push_field(c);
        else if (ch == '*' || ch == '?' || ch == '[') err(c, 245);
        else put(c, ch);
    }
}

static int is_nm(int ch, int first)
{
    return ch == '_' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (!first && ch >= '0' && ch <= '9');
}

static void record_req(Cx *c, int kind, uint64_t a, uint64_t len)
{
    if (c->nvreq >= CAP_VARREQ) { err(c, 212); return; }
    c->nvreq++;
    if (c->o->nreq < OSH_XR_MAXREQ) {
        c->o->req[c->o->nreq].kind = kind;
        c->o->req[c->o->nreq].a = a;
        c->o->req[c->o->nreq].len = len;
        c->o->nreq++;
    }
}

/* fetch a value through the host protocol; returns 0 ok, else sets the error */
static int fetch(Cx *c, int kind, const uint8_t *name, size_t nlen, uint64_t a, uint64_t reqa, uint8_t *val, size_t *vlen, uint64_t *npos)
{
    record_req(c, kind, reqa, kind == 1 ? nlen : 0);
    if (c->status) return -1;
    int found;
    osh_xr_env_get(c->env, kind, name, nlen, a, &found, vlen, npos, val);
    if (kind != 5 && found && *vlen > CAP_VALUE) { err(c, 211); return -1; }
    if (!found) *vlen = 0;
    return 0;
}

/* a list reference: at = '@' or '*'; quoted = inside "" */
static void expand_list(Cx *c, int at, int quoted)
{
    uint8_t val[OSH_XR_VALCAP];
    size_t vl;
    uint64_t np = 0;
    if (fetch(c, 5, NULL, 0, 0, 0, val, &vl, &np)) return;
    int join = c->mode == 2 || (quoted && !at);
    for (uint64_t k = 1; k <= np && !c->status; k++) {
        if (fetch(c, 3, NULL, 0, k, k, val, &vl, &np)) return;
        if (k > 1) {
            if (join && quoted) piece_text(c, (const uint8_t *)" ", 1);
            else if (join) put(c, ' ');
            else if (quoted) push_field(c); /* "$@": a field per positional */
            else piece_split(c, (const uint8_t *)" ", 1);
        }
        if (c->mode == 2) piece_text(c, val, vl);
        else if (quoted) piece_text(c, val, vl);
        else piece_split(c, val, vl);
    }
}

/* the $ at in[p]: handle the reference; returns the number of bytes it covers, or 0 when it is a literal dollar */
static size_t expand_ref(Cx *c, size_t p, size_t e, int quoted)
{
    const uint8_t *in = c->in;
    uint8_t val[OSH_XR_VALCAP];
    size_t vl = 0;
    uint64_t np = 0;
    int kind = 0;
    uint64_t k = 0;
    const uint8_t *nm = NULL;
    size_t nl = 0, used = 0;
    if (p + 1 >= e) return 0;
    int d = in[p + 1];
    if (is_nm(d, 1)) {
        size_t q = p + 1;
        while (q < e && is_nm(in[q], 0)) q++;
        kind = 1; nm = in + p + 1; nl = q - p - 1; used = q - p;
    } else if (d >= '0' && d <= '9') {
        kind = 3; k = (uint64_t)(d - '0'); used = 2;
    } else if (d == '?') { kind = 2; used = 2; }
    else if (d == '#') { kind = 4; used = 2; }
    else if (d == '@' || d == '*') {
        expand_list(c, d == '@', quoted);
        return 2;
    } else if (d == '{' && p + 3 < e) {
        int d2 = in[p + 2];
        if (d2 >= '0' && d2 <= '9') {
            if (in[p + 3] != '}') return 0;
            kind = 3; k = (uint64_t)(d2 - '0'); used = 4;
        } else if (is_nm(d2, 1)) {
            size_t q = p + 2;
            while (q < e && is_nm(in[q], 0)) q++;
            if (q >= e || in[q] != '}') return 0;
            kind = 1; nm = in + p + 2; nl = q - p - 2; used = q - p + 1;
        } else return 0;
    } else return 0;
    /* standalone assignments see earlier values of the same name */
    if (kind == 1 && c->mode == 2 && c->standalone) {
        for (int j = c->nprior - 1; j >= 0; j--)
            if (c->prior[j].nlen == nl && memcmp(c->prior[j].name, nm, nl) == 0) {
                const uint8_t *src = c->o->out + c->prior[j].voff;
                size_t sl = c->prior[j].vlen;
                if (c->mode == 2 || quoted) piece_text(c, src, sl);
                return used;
            }
    }
    if (fetch(c, kind, nm, nl, k, kind == 1 ? (uint64_t)(nm - in) : kind == 3 ? k : 0, val, &vl, &np)) return used;
    if (c->mode == 2 || quoted) piece_text(c, val, vl);
    else piece_split(c, val, vl);
    return used;
}

/* expand the raw bytes in[s..e) as one word in the current mode */
static void expand_word(Cx *c, size_t s, size_t e)
{
    const uint8_t *in = c->in;
    size_t p = s;
    int q = 0;           /* 0 unquoted, 1 '', 2 "" */
    int dq_other = 0, dq_at = 0;
    while (p < e && !c->status) {
        int ch = in[p];
        if (q == 1) {
            if (ch == '\'') { q = 0; mark(c); } else put(c, (uint8_t)ch);
            p++;
            continue;
        }
        if (ch == '\\' && p + 1 < e) {
            int d = in[p + 1];
            if (d == '\n') { p += 2; continue; }
            if (q == 0 || d == '$' || d == '"' || d == '\\' || d == '`') {
                put(c, (uint8_t)d);
                if (q == 2) dq_other = 1;
                p += 2;
                continue;
            }
        }
        if (q == 2 && ch == '"') {
            if (dq_other || !dq_at) mark(c);
            q = 0;
            p++;
            continue;
        }
        if (q == 0 && ch == '"') { q = 2; dq_other = dq_at = 0; p++; continue; }
        if (q == 0 && ch == '\'') { q = 1; p++; continue; }
        if (ch == '$') {
            size_t used = expand_ref(c, p, e, q == 2);
            if (used) {
                if (q == 2) {
                    if (in[p + 1] == '@') dq_at = 1; else dq_other = 1;
                }
                p += used;
                continue;
            }
        }
        if (q == 2) dq_other = 1;
        put(c, (uint8_t)ch);
        p++;
    }
    push_field(c);
}

static uint64_t pack_name(const uint8_t *b, size_t n)
{
    if (n == 0 || n > 8) return 0;
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) v = (v << 8) | b[i];
    return v;
}

static int classify(uint64_t v)
{
    static const struct { const char *n; int id; } t[] = {
        {"cd", 1}, {"pwd", 2}, {"printf", 3}, {"export", 4}, {"unset", 5}, {"exit", 6},
        {"set", 99}, {"eval", 99}, {"exec", 99}, {".", 99}, {"source", 99}, {"trap", 99}, {"shift", 99}, {"read", 99},
        {"return", 99}, {"break", 99}, {"continue", 99}, {"local", 99}, {"readonly", 99}, {"alias", 99}, {"wait", 99},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (pack_name((const uint8_t *)t[i].n, strlen(t[i].n)) == v) return t[i].id;
    return 0;
}

unsigned osh_xr_next(const OshRefParse *pr, unsigned cand, uint64_t last_status, int noskip)
{
    unsigned p = cand;
    while (p < pr->npipe && p > 0 && !noskip) {
        uint64_t cn = pr->pipe[p - 1][2];
        if ((cn == 2 && last_status != 0) || (cn == 3 && last_status == 0)) p++;
        else break;
    }
    return p < pr->npipe ? p : pr->npipe;
}

void osh_xr_pipe(const uint8_t *in, size_t n, const OshRefLex *lx, const OshRefParse *pr, const OshXrEnv *env, unsigned p, OshXrOut *out)
{
    (void)n;
    Cx c;
    memset(&c, 0, sizeof c);
    memset(out, 0, sizeof *out);
    c.in = in; c.lx = lx; c.env = env; c.o = out;
    unsigned first = (unsigned)pr->pipe[p][0], nc = (unsigned)pr->pipe[p][1];
    out->nrec = 8 + (size_t)nc * 164;
    for (unsigned ci = 0; ci < nc && !c.status; ci++) {
        c.blk = out->rec + 8 + (size_t)ci * 164;
        c.nprior = 0;
        c.standalone = pr->cmd[first + ci][3] == 0;
        unsigned t = (unsigned)pr->cmd[first + ci][0], te = (unsigned)pr->cmd[first + ci][1];
        int anyw = 0;
        while (t < te && !c.status) {
            const OshRefTok *tk = &lx->tok[t];
            c.woff = tk->start;
            c.exists = 0;
            c.nfields_word = 0;
            if (tk->kind == 1) {
                if ((tk->flags & 16) && !anyw) {
                    /* assignment: name copied to out, then the value */
                    uint64_t na = c.blk[1];
                    size_t nl = tk->aux - 1 - tk->start;
                    if (out->out_used + nl > OSH_XR_OUTCAP) { err(&c, 210); break; }
                    size_t noff = out->out_used;
                    memcpy(out->out + noff, in + tk->start, nl);
                    out->out_used += nl;
                    uint64_t *e = c.blk + 4 + 64 + 4 * na;
                    e[0] = noff; e[1] = nl; e[2] = out->out_used;
                    c.blk[1] = na + 1;
                    c.mode = 2;
                    expand_word(&c, tk->aux, tk->start + tk->len);
                    e[3] = out->out_used - e[2];
                    if (c.nprior < 16) {
                        memcpy(c.prior[c.nprior].name, in + tk->start, nl < 256 ? nl : 256);
                        c.prior[c.nprior].nlen = nl;
                        c.prior[c.nprior].voff = e[2];
                        c.prior[c.nprior].vlen = e[3];
                        c.nprior++;
                    }
                } else {
                    anyw = 1;
                    c.mode = 1;
                    expand_word(&c, tk->start, tk->start + tk->len);
                }
                t++;
            } else {
                uint64_t r = c.blk[2];
                uint64_t *e = c.blk + 4 + 64 + 64 + 4 * r;
                c.blk[2] = r + 1;
                e[0] = tk->kind == 6 ? 1 : tk->kind == 7 ? 2 : tk->kind == 8 ? 3 : 4;
                e[1] = tk->aux;
                const OshRefTok *tg = &lx->tok[t + 1];
                if (e[0] == 4) {
                    e[2] = (uint64_t)(in[tg->start] - '0');
                } else {
                    c.woff = tg->start;
                    c.mode = 3;
                    expand_word(&c, tg->start, tg->start + tg->len);
                    if (!c.status && c.nfields_word == 0) err(&c, 243);
                }
                t += 2;
            }
        }
        if (!c.status && c.blk[0] > 0) {
            uint64_t a0o = c.blk[4], a0l = c.blk[5];
            int b = classify(pack_name(out->out + a0o, a0l));
            if (b == 99) { c.woff = c.a0off; err(&c, 239); }
            else c.blk[3] = (uint64_t)b;
        }
    }
    if (c.status) { out->status = c.status; return; }
    out->rec[0] = 0x4F524551;
    out->rec[1] = nc;
    out->rec[2] = pr->pipe[p][2];
    out->rec[4] = out->out_used;
    out->rec[5] = 1;
    out->status = 103;
}

static void set_dec(OshXrStr *s, uint64_t v)
{
    char t[24];
    int n = snprintf(t, sizeof t, "%llu", (unsigned long long)v);
    s->set = 1;
    s->len = (size_t)n;
    memcpy(s->b, t, (size_t)n);
}

void osh_xr_env_get(const OshXrEnv *env, int kind, const uint8_t *name, size_t nlen, uint64_t k, int *found, size_t *len, uint64_t *npos, uint8_t *val)
{
    OshXrStr tmp;
    const OshXrStr *s = NULL;
    memset(&tmp, 0, sizeof tmp);
    *npos = (uint64_t)env->npos;
    *len = 0;
    *found = 0;
    if (kind == 5) { *found = 1; return; }
    if (kind == 1) {
        for (int i = 0; i < env->nvars; i++)
            if (strlen(env->var[i].name) == nlen && memcmp(env->var[i].name, name, nlen) == 0) s = &env->var[i].v;
    } else if (kind == 2) { set_dec(&tmp, env->status); s = &tmp; }
    else if (kind == 4) { set_dec(&tmp, (uint64_t)env->npos); s = &tmp; }
    else if (kind == 3) {
        if (k == 0) s = &env->arg0;
        else if (k <= (uint64_t)env->npos) s = &env->pos[k - 1];
    }
    if (s && s->set) {
        *found = 1;
        *len = s->len;
        memcpy(val, s->b, s->len);
    }
}
