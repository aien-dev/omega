/*
 * osh_parse_ref.c -- independent reference parser (see osh_parse_ref.h). Recursive descent over the token
 * array: list -> pipeline -> command. The grammar and every refusal are taken from the spec text, in token order:
 *  - a command starts at a word or a redirection operator; any other token where one is needed is
 *    SYNTAX_EMPTY_CMD at that token (a `;` `|` `&&` `||` with no command before it)
 *  - NL after `|` `&&` `||` is skipped; NL after a command or after `;` ends the list (the NL is consumed)
 *  - capacities are checked when the thing they count is about to be added, before it is written
 *  - at the end of the tokens: after `|` `&&` `||` need more input (SYNTAX_EOF at offset n with eoi); after a
 *    redirection operator SYNTAX_REDIR_TARGET at offset n with eoi (else need more); otherwise the list is done
 */
#include "osh_parse_ref.h"

#include <string.h>

enum { K_WORD = 1, K_PIPE, K_OR, K_AND, K_SEMI, K_LT, K_GT, K_APPEND, K_NL, K_DUP_OUT, K_DUP_IN };

typedef struct {
    const uint8_t *in;
    size_t n;
    const OshRefLex *lx;
    int eoi;
    OshRefParse *o;
    unsigned i;       /* next token */
    unsigned pc;      /* commands in the open pipeline */
    int bad;          /* the outcome is decided */
} P;

static unsigned ntok(const P *p) { return p->lx->ntok; }
static const OshRefTok *tk(const P *p, unsigned j) { return &p->lx->tok[j]; }
static int is_redir(uint64_t k) { return k == K_LT || k == K_GT || k == K_APPEND || k == K_DUP_OUT || k == K_DUP_IN; }
static int starts_cmd(const P *p, unsigned j) { return tk(p, j)->kind == K_WORD || is_redir(tk(p, j)->kind); }

static void refuse(P *p, unsigned code, uint64_t off)
{
    p->o->status = code;
    p->o->err_off = off;
    p->bad = 1;
}

/* the bytes of in[s..e) without backslash-newline pairs, up to 8 of them plus a terminator; *over set if more */
static void sig(const P *p, uint64_t s, uint64_t e, char out[9], int *over)
{
    size_t k = 0;
    *over = 0;
    for (uint64_t q = s; q < e; q++) {
        if (p->in[q] == '\\' && q + 1 < e && p->in[q + 1] == '\n') { q++; continue; }
        if (k == 8) { *over = 1; break; }
        out[k++] = (char)p->in[q];
    }
    out[k] = 0;
}

static unsigned reserved_code(const char *w)
{
    static const struct { const char *w; unsigned code; } R[] = {
        { "if", 233 }, { "then", 233 }, { "elif", 233 }, { "else", 233 }, { "fi", 233 },
        { "for", 234 }, { "while", 234 }, { "until", 234 }, { "do", 234 }, { "done", 234 }, { "in", 234 }, { "select", 234 },
        { "case", 235 }, { "esac", 235 }, { "function", 237 }, { "!", 238 }, { "time", 239 }, { "coproc", 239 },
    };
    for (size_t q = 0; q < sizeof R / sizeof R[0]; q++) if (strcmp(R[q].w, w) == 0) return R[q].code;
    return 0;
}

static int all_digits(const P *p, uint64_t s, uint64_t len)
{
    for (uint64_t q = 0; q < len; q++) if (p->in[s + q] < '0' || p->in[s + q] > '9') return 0;
    return 1;
}

/* one command; p->i is at a word or a redirection operator */
static void command(P *p)
{
    const OshRefTok *first = tk(p, p->i);
    OshRefParse *o = p->o;
    if (p->pc >= 8) { refuse(p, 205, first->start); return; }
    if (p->pc == 0 && o->npipe >= OSH_PREF_PIPES) { refuse(p, 204, first->start); return; }
    if (o->ncmd >= OSH_PREF_CMDS) { refuse(p, 203, first->start); return; }
    unsigned c = o->ncmd++;
    if (p->pc == 0) {
        o->pipe[o->npipe][0] = c; o->pipe[o->npipe][1] = 0; o->pipe[o->npipe][2] = 0; o->pipe[o->npipe][3] = p->i;
        o->npipe++;
    }
    unsigned pi = o->npipe - 1;
    memset(o->cmd[c], 0, sizeof o->cmd[c]);
    o->cmd[c][0] = p->i;
    o->cmd[c][5] = pi;
    o->cmd[c][6] = p->pc;
    o->pipe[pi][1] = ++p->pc;
    int any_word = 0;
    while (p->i < ntok(p)) {
        const OshRefTok *t = tk(p, p->i);
        if (t->kind == K_WORD) {
            if ((t->flags & 16) && !any_word) {
                char nm[9]; int over;
                sig(p, t->start, t->aux - 1, nm, &over);
                if (!over && strcmp(nm, "IFS") == 0) { refuse(p, 240, t->start); return; }
                if (o->cmd[c][2] >= 16) { refuse(p, 207, t->start); return; }
                o->cmd[c][2]++;
            } else {
                if (!any_word && (t->flags & ~32ULL) == 0) {
                    char w[9]; int over;
                    sig(p, t->start, t->start + t->len, w, &over);
                    unsigned rc = over ? 0 : reserved_code(w);
                    if (rc) { refuse(p, rc, t->start); return; }
                }
                if (o->cmd[c][3] >= 32) { refuse(p, 206, t->start); return; }
                o->cmd[c][3]++;
                any_word = 1;
            }
            p->i++;
        } else if (is_redir(t->kind)) {
            if (o->cmd[c][4] >= 8) { refuse(p, 208, t->start); return; }
            o->cmd[c][4]++;
            p->i++;
            if (p->i >= ntok(p)) {
                if (p->eoi) refuse(p, 242, p->n);
                else { o->status = 100; p->bad = 1; }
                return;
            }
            const OshRefTok *g = tk(p, p->i);
            if (g->kind != K_WORD) { refuse(p, 242, g->start); return; }
            if (t->kind == K_DUP_OUT || t->kind == K_DUP_IN) {
                if (g->flags != 0) { refuse(p, 231, g->start); return; }
                if (g->len == 1 && g->start < p->n && p->in[g->start] >= '0' && p->in[g->start] <= '2') { /* fine */ }
                else if (all_digits(p, g->start, g->len)) { refuse(p, 232, g->start); return; }
                else { refuse(p, 231, g->start); return; }
            }
            p->i++;
        } else {
            break;
        }
    }
    o->cmd[c][1] = p->i;
}

static void skip_nl(P *p) { while (p->i < ntok(p) && tk(p, p->i)->kind == K_NL) p->i++; }

/* the tokens ran out after a `|` `&&` `||` */
static void run_out(P *p)
{
    if (p->eoi) refuse(p, 248, p->n);
    else { p->o->status = 100; p->bad = 1; }
}

void osh_parse_ref(const uint8_t *in, size_t n, const OshRefLex *lx, int eoi, OshRefParse *out)
{
    memset(out, 0, sizeof *out);
    P p = { in, n, lx, eoi, out, 0, 0, 0 };
    if (p.i >= ntok(&p)) { out->next_tok = p.i; return; }
    if (tk(&p, p.i)->kind == K_NL) { out->next_tok = ++p.i; return; } /* a blank line is a complete, empty list */
    if (!starts_cmd(&p, p.i)) { refuse(&p, 241, tk(&p, p.i)->start); return; }
    for (;;) {
        /* pipeline */
        command(&p);
        if (p.bad) return;
        while (p.i < ntok(&p) && tk(&p, p.i)->kind == K_PIPE) {
            p.i++;
            skip_nl(&p);
            if (p.i >= ntok(&p)) { run_out(&p); return; }
            if (!starts_cmd(&p, p.i)) { refuse(&p, 241, tk(&p, p.i)->start); return; }
            command(&p);
            if (p.bad) return;
        }
        unsigned pi = out->npipe - 1;
        p.pc = 0;
        if (p.i >= ntok(&p)) {
            if (!eoi) { out->status = 100; return; }
            out->pipe[pi][2] = 0;
            out->next_tok = p.i;
            return;
        }
        const OshRefTok *t = tk(&p, p.i);
        if (t->kind == K_NL) {
            out->pipe[pi][2] = 1;
            out->next_tok = ++p.i;
            return;
        }
        if (t->kind == K_SEMI) {
            out->pipe[pi][2] = 1;
            p.i++;
            if (p.i >= ntok(&p)) { out->next_tok = p.i; return; }
            if (tk(&p, p.i)->kind == K_NL) { out->next_tok = ++p.i; return; }
            if (!starts_cmd(&p, p.i)) { refuse(&p, 241, tk(&p, p.i)->start); return; }
            continue;
        }
        /* && or || */
        out->pipe[pi][2] = t->kind == K_AND ? 2 : 3;
        p.i++;
        skip_nl(&p);
        if (p.i >= ntok(&p)) { run_out(&p); return; }
        if (!starts_cmd(&p, p.i)) { refuse(&p, 241, tk(&p, p.i)->start); return; }
    }
}
