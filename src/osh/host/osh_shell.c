/* osh_shell.c -- see osh_shell.h. */
#include "osh_shell.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "osh_layout.h"

#define WS ((size_t)OSH_WS_CELLS)
enum { ST_MORE = 100, ST_BUDGET = 101, ST_NEED_VAR = 102, ST_PIPE = 103, ST_COMPLETE = 104 };
#define BUF_MAX ((size_t)1 << 20)

typedef struct {
    const char *str;
    size_t len, pos;
    int fd;
} LineSrc;

int osh_shell_init(OshShell *sh, char *const *envp, int native, int interactive, const char *arg0, char **pos, int npos)
{
    memset(sh, 0, sizeof *sh);
    sh->native = native;
    sh->interactive = interactive;
    sh->arg0 = (char *)arg0;
    sh->pos = pos;
    sh->npos = npos;
    const char *src[3] = {(const char *)osh_unit_lex, (const char *)osh_unit_parse, (const char *)osh_unit_expand};
    size_t len[3] = {osh_unit_lex_len, osh_unit_parse_len, osh_unit_expand_len};
    char err[300];
    if (osh_core_init(&sh->core, src, len, err, sizeof err) != 0) {
        fprintf(stderr, "osh: cannot load the shell core: %s\n", err);
        return -1;
    }
    sh->w = calloc(WS, sizeof(uint64_t));
    if (!sh->w || osh_session_init(&sh->s, envp) != 0) {
        fprintf(stderr, "osh: out of memory\n");
        return -1;
    }
    /* A Linux host has no capability authority (osh_exec.c): the embedder supplies the binding. The osh program is its own
     * embedder on Linux and grants itself the hosted-authority binding (domain 2). */
    sh->s.binding.valid = 1;
    sh->s.binding.domain = 2;
    sh->s.binding.cap_generation = 1;
    if (interactive) {
        sh->s.interactive = 1;
        sh->s.tty_fd = 0;
    }
    return 0;
}

void osh_shell_free(OshShell *sh)
{
    osh_session_free(&sh->s);
    osh_core_free(&sh->core);
    free(sh->w);
    free(sh->buf);
}

static void reset_list(OshShell *sh)
{
    memset(sh->w, 0, WS * sizeof(uint64_t));
    sh->w[OSH_S_MAGIC] = OSH_ABI_MAGIC;
    sh->w[OSH_S_VERSION] = OSH_ABI_VERSION;
    sh->w[OSH_S_WS_CELLS] = OSH_WS_CELLS;
    sh->blen = 0;
}

static uint64_t call(OshShell *sh, int unit)
{
    return osh_core_call(&sh->core, unit, sh->native, sh->buf, sh->blen, sh->w, WS);
}

static uint64_t run_entry(OshShell *sh, int unit)
{
    for (int i = 0; i < 100000; i++) {
        uint64_t st = call(sh, unit);
        if (st != ST_BUDGET) return st;
    }
    return OSH_CORE_FAULT;
}

/* lexer until it reports done, then the parser. 0: list parsed; 100: needs more input; >= 200: refused. */
static uint64_t lex_parse(OshShell *sh)
{
    uint64_t st = run_entry(sh, OSH_U_LEX);
    if (st != 0) return st;
    return run_entry(sh, OSH_U_PARSE);
}

static int append(OshShell *sh, const uint8_t *p, size_t n)
{
    if (sh->blen + n > BUF_MAX) return -1;
    if (sh->blen + n > sh->bcap) {
        size_t nc = sh->bcap ? sh->bcap * 2 : 4096;
        while (nc < sh->blen + n) nc *= 2;
        uint8_t *nb = realloc(sh->buf, nc);
        if (!nb) return -1;
        sh->buf = nb;
        sh->bcap = nc;
    }
    memcpy(sh->buf + sh->blen, p, n);
    sh->blen += n;
    return 0;
}

/* Append one line (with its newline; one is added at end of input) to the list buffer.
 * 1 got a line, 0 end of input with nothing read, -1 error (too long / read error). */
static int read_line(OshShell *sh, LineSrc *src)
{
    size_t got = 0;
    if (src->fd < 0) {
        if (src->pos >= src->len) return 0;
        const char *s = src->str + src->pos;
        size_t n = 0;
        while (src->pos + n < src->len && s[n] != '\n') n++;
        int nl = src->pos + n < src->len;
        if (append(sh, (const uint8_t *)s, n) != 0) return -1;
        if (append(sh, (const uint8_t *)"\n", 1) != 0) return -1;
        src->pos += n + (size_t)nl;
        return 1;
    }
    for (;;) {
        uint8_t c;
        ssize_t r = read(src->fd, &c, 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            if (got == 0) return 0;
            c = '\n';
            return append(sh, &c, 1) == 0 ? 1 : -1;
        }
        got++;
        if (append(sh, &c, 1) != 0) return -1;
        if (c == '\n') return 1;
    }
}

static size_t dec(uint64_t v, uint8_t *o)
{
    uint8_t t[24];
    size_t n = 0;
    do { t[n++] = (uint8_t)('0' + v % 10); v /= 10; } while (v);
    for (size_t i = 0; i < n; i++) o[i] = t[n - 1 - i];
    return n;
}

/* answer one NEED_VAR round trip (ABI draft: STAGING = {found, len, npos, 0, bytes one per cell}) */
static void answer(OshShell *sh)
{
    uint64_t *w = sh->w;
    uint64_t kind = w[OSH_S_VR_KIND], a = w[OSH_S_VR_A], len = w[OSH_S_VR_LEN];
    const uint8_t *val = NULL;
    uint8_t num[24];
    size_t vl = 0;
    int found = 0;
    switch (kind) {
    case 1: {
        char name[256];
        if (a <= sh->blen && len < sizeof name && len <= sh->blen - a) {
            memcpy(name, sh->buf + a, (size_t)len);
            name[len] = 0;
            const char *v = osh_var_get(&sh->s, name);
            if (v) { found = 1; val = (const uint8_t *)v; vl = strlen(v); }
        }
        break;
    }
    case 2: found = 1; vl = dec((uint64_t)(unsigned)sh->s.last_status, num); val = num; break;
    case 3:
        if (a == 0 && sh->arg0) { found = 1; val = (const uint8_t *)sh->arg0; vl = strlen(sh->arg0); }
        else if (a >= 1 && a <= (uint64_t)sh->npos) { found = 1; val = (const uint8_t *)sh->pos[a - 1]; vl = strlen(sh->pos[a - 1]); }
        break;
    case 4: found = 1; vl = dec((uint64_t)sh->npos, num); val = num; break;
    default: break;
    }
    w[OSH_STAGING + 0] = (uint64_t)found;
    w[OSH_STAGING + 1] = vl;
    w[OSH_STAGING + 2] = (uint64_t)sh->npos;
    w[OSH_STAGING + 3] = 0;
    for (size_t i = 0; i < vl && i < 1024; i++) w[OSH_STAGING + 4 + i] = val[i]; /* longer: the unit refuses by len */
}

static void refusal(OshShell *sh, uint64_t code, int syntax)
{
    const char *nm = osh_code_name((unsigned)code);
    char tmp[16];
    if (!nm) { snprintf(tmp, sizeof tmp, "E%llu", (unsigned long long)code); nm = tmp; }
    fprintf(stderr, "osh: %s: %s at byte %llu\n", syntax ? "syntax error" : "refused", nm, (unsigned long long)sh->w[OSH_S_ERR_OFF]);
    sh->s.last_status = 2;
    sh->refused++;
}

/* expand and run the parsed list. Returns 1 when the shell must stop (exit builtin, interrupted, fault). */
static int run_list(OshShell *sh)
{
    uint64_t *w = sh->w;
    w[OSH_S_LAST_STATUS] = (uint64_t)(unsigned)sh->s.last_status;
    for (int guard = 0; guard < 1000000; guard++) {
        uint64_t st = osh_core_call(&sh->core, OSH_U_EXPAND, sh->native, sh->buf, sh->blen, w, WS);
        switch (st) {
        case ST_BUDGET: continue;
        case ST_NEED_VAR: answer(sh); continue;
        case ST_PIPE: {
            uint64_t nc = w[OSH_REQUEST + 1];
            OshResult res;
            osh_exec_record(&sh->s, w + OSH_REQUEST, (size_t)(OSH_REQ_HDR_CELLS + nc * OSH_CMD_CELLS), w + OSH_OUT, (size_t)w[OSH_REQUEST + 4], &res);
            w[OSH_S_LAST_STATUS] = (uint64_t)(unsigned)sh->s.last_status;
            if (sh->s.exit_requested) return 1;
            if (res.killed_by_int) return 0;
            continue;
        }
        case ST_COMPLETE: return 0;
        default:
            if (st >= 200 && st != OSH_CORE_FAULT) { refusal(sh, st, 0); return !sh->interactive; }
            fprintf(stderr, "osh: internal error (unit status %llu)\n", (unsigned long long)st);
            sh->s.last_status = 70;
            return 1;
        }
    }
    return 1;
}

static int run_src(OshShell *sh, LineSrc *src)
{
    for (;;) {
        reset_list(sh);
        if (sh->interactive) { fputs("osh$ ", stderr); fflush(stderr); }
        uint64_t st;
        int eof = 0;
        for (;;) {
            int r = read_line(sh, src);
            if (r < 0) { fprintf(stderr, "osh: input too long or unreadable\n"); sh->s.last_status = 2; return sh->interactive ? sh->s.last_status : 2; }
            if (r == 0) {
                eof = 1;
                if (sh->blen == 0) return sh->s.last_status;
                sh->w[OSH_S_EOI] = 1;
            }
            st = lex_parse(sh);
            if (st == 0 && sh->w[OSH_PX_STATE] != 4) st = eof ? ST_COMPLETE : ST_MORE; /* only blank so far */
            if (st == ST_MORE && !eof) {
                if (sh->interactive) { fputs("> ", stderr); fflush(stderr); }
                continue;
            }
            break;
        }
        if (st == ST_COMPLETE) {
            /* an empty list at end of input: nothing to run */
        } else if (st == 0) {
            sh->lists++;
            if (run_list(sh)) return sh->s.exit_requested ? sh->s.exit_status : sh->s.last_status;
        } else {
            if (st >= 200 && st != OSH_CORE_FAULT) refusal(sh, st, 1);
            else { fprintf(stderr, "osh: syntax error: unexpected end of input\n"); sh->s.last_status = 2; }
            if (!sh->interactive) return 2;
        }
        if (eof) return sh->s.last_status;
    }
}

int osh_shell_run_string(OshShell *sh, const char *text, size_t len)
{
    LineSrc src = {text, len, 0, -1};
    return run_src(sh, &src);
}

int osh_shell_run_fd(OshShell *sh, int fd)
{
    LineSrc src = {NULL, 0, 0, fd};
    return run_src(sh, &src);
}
