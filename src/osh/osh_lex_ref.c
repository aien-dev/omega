/*
 * osh_lex_ref.c -- independent reference tokenizer (see osh_lex_ref.h). Direct scanning with lookahead,
 * deliberately not a state machine. Rules, all from the spec and from bash:
 *  - a word is emitted when the byte after it is seen; an operator when its last byte is known (a one-byte
 *    operator needs the next byte); a buffer ending earlier is incomplete (status 100)
 *  - status 0 only when the last thing consumed was a newline token at the end of the buffer
 *  - refusals are decided at the first byte that decides them (offsets per ABI section 5.4)
 *  - reading a byte at offset >= 4096 is CAP_LINE at offset 4096; a 129th token is CAP_TOKENS at its start
 *  - bash removes every backslash-newline pair before it classifies the byte after it, except inside single
 *    quotes and comments (and a backslash that is itself escaped is not the start of a pair). Here that is
 *    skipc(): every place that looks at the "next byte" of a word, operator, `$` or `${` construct first steps
 *    over such pairs. A final lone backslash with no byte after it is incomplete, unless the end-of-input flag
 *    is set, when it is a literal backslash. Offsets always point into the original buffer; a token's span is
 *    the raw source from its first to its last byte.
 */
#include "osh_lex_ref.h"

#include <string.h>

enum { K_WORD = 1, K_PIPE, K_OR, K_AND, K_SEMI, K_LT, K_GT, K_APPEND, K_NL, K_DUP_OUT, K_DUP_IN };
enum { E_CAP_LINE = 201, E_CAP_TOKENS = 202, E_NUL = 220, E_BACKTICK, E_GLOB, E_TILDE, E_PARAM_OP, E_SPECIAL_PARAM,
       E_CMDSUB, E_BACKGROUND, E_SUBSHELL, E_HEREDOC, E_CASEEND, E_REDIR_OTHER, E_FD_RANGE, E_GROUP = 236, E_BRACE = 246,
       E_POSITIONAL = 247, E_APPEND_ASSIGN = 249 };
#define INCOMPLETE (-1)
#define CAPLINE (-2)

typedef struct {
    const uint8_t *in;
    size_t n;
    OshRefLex *o;
    uint64_t line;      /* 1-based current line */
    int last_was_nl;    /* the last thing consumed was a newline token */
    int eoi;            /* end-of-input flag */
} Ref;

/* byte at j, or INCOMPLETE past the end of the buffer, or CAPLINE for offset >= 4096 */
static int peek(const Ref *r, size_t j)
{
    if (j >= OSH_REF_LINE_CAP && r->n > OSH_REF_LINE_CAP) return CAPLINE;
    if (j >= r->n) return INCOMPLETE;
    return r->in[j];
}

static void refuse(Ref *r, unsigned code, uint64_t off)
{
    r->o->status = code;
    r->o->err_off = off;
}

/* a negative peek result becomes the outcome */
static int stop(Ref *r, int pk)
{
    if (pk == INCOMPLETE) r->o->status = 100;
    else refuse(r, E_CAP_LINE, OSH_REF_LINE_CAP);
    return 1;
}

/* same as stop, for the functions that return 0 on an outcome */
static int stopz(Ref *r, int pk) { stop(r, pk); return 0; }

static int is_alpha_(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_glob(int c) { return c == '*' || c == '?' || c == '['; }
static int is_opch(int c) { return c == '|' || c == '&' || c == ';' || c == '<' || c == '>' || c == '(' || c == ')'; }

/* Step *j over backslash-newline pairs, counting them in *np. Returns INCOMPLETE when a final lone backslash
 * must wait for the next byte (no end-of-input flag), else 0. The lookahead reads the raw buffer: whether a
 * pair starts at the final byte below the line cap is not a cap question; the cap is reported by peek(). */
static int skipc(const Ref *r, size_t *j, uint64_t *np)
{
    while (*j < r->n && *j < OSH_REF_LINE_CAP && r->in[*j] == '\\') {
        if (*j + 1 >= r->n) return r->eoi ? 0 : INCOMPLETE;
        if (r->in[*j + 1] != '\n') break;
        *j += 2;
        (*np)++;
    }
    return 0;
}

/* skipc inside a word: the pairs set flag 32 and count lines */
static int sk(Ref *r, size_t *j, uint64_t *fl, uint64_t *line)
{
    uint64_t np = 0;
    int rc = skipc(r, j, &np);
    if (np) { *fl |= 32; *line += np; }
    return rc;
}

/* offset of the last byte before j that is not part of a backslash-newline pair (j > 0) */
static size_t prev_sig(const Ref *r, size_t j)
{
    while (j >= 3 && r->in[j - 1] == '\n' && r->in[j - 2] == '\\') j -= 2;
    return j - 1;
}

/* append a token; 1 and CAP_TOKENS if full */
static int add(Ref *r, uint64_t kind, uint64_t flags, uint64_t nseg, uint64_t line, uint64_t start, uint64_t len, uint64_t aux)
{
    if (r->o->ntok >= OSH_REF_TOKEN_CAP) { refuse(r, E_CAP_TOKENS, start); return 1; }
    OshRefTok *t = &r->o->tok[r->o->ntok++];
    t->kind = kind; t->flags = flags; t->nseg = nseg; t->line = line; t->start = start; t->len = len; t->aux = aux;
    return 0;
}

/* parameter expansion after a `$` at j (inside double quotes when dq); returns the offset after the
 * construct, or 0 with the outcome set. A `$` with nothing special after it is literal: returns j + 1 and the
 * caller sees the next byte itself. Pairs stepped over inside a consumed construct set *fl and *line. */
static size_t dollar(Ref *r, size_t j, int dq, uint64_t *fl, uint64_t *line)
{
    uint64_t f2 = 0, l2 = 0;
    size_t q = j + 1;
    int rc = sk(r, &q, &f2, &l2);
    if (rc < 0) { stop(r, rc); return 0; }
    int x = peek(r, q);
    if (x < 0) { stop(r, x); return 0; }
    if (x == 0) { refuse(r, E_NUL, q); return 0; }
    if (is_alpha_(x) || is_digit(x) || x == '?' || x == '#' || x == '@' || x == '*') {
        if (x == '_') {
            /* `$_` is the "last argument" special parameter unless the name goes on (`$_x`) */
            uint64_t f3 = 0, l3 = 0;
            size_t q2 = q + 1;
            rc = sk(r, &q2, &f3, &l3);
            if (rc < 0) { stop(r, rc); return 0; }
            int nb = peek(r, q2);
            if (nb < 0) { stop(r, nb); return 0; }
            if (nb != 0 && !is_alpha_(nb) && !is_digit(nb)) { refuse(r, E_SPECIAL_PARAM, q); return 0; }
        }
        *fl |= f2; *line += l2;
        return q + 1;
    }
    if (x == '(') { refuse(r, E_CMDSUB, q); return 0; }
    if (x == '$' || x == '!' || x == '-') { refuse(r, E_SPECIAL_PARAM, q); return 0; }
    if (!dq && (x == '\'' || x == '"')) { refuse(r, E_SPECIAL_PARAM, q); return 0; }
    if (x != '{') return j + 1; /* a literal `$` */
    size_t k = q + 1;
    int numeric = 0; /* the name began with a digit: it must be that one digit ($0..$9 only) */
    int named = 0, us = 0;
    size_t us_off = 0;
    for (;;) {
        rc = sk(r, &k, &f2, &l2);
        if (rc < 0) { stop(r, rc); return 0; }
        int b = peek(r, k);
        if (b < 0) { stop(r, b); return 0; }
        if (b == 0) { refuse(r, E_NUL, k); return 0; }
        if (is_alpha_(b) || is_digit(b)) {
            if (!named) { numeric = is_digit(b); us = b == '_'; us_off = k; }
            else if (numeric) { refuse(r, is_digit(b) ? E_POSITIONAL : E_PARAM_OP, k); return 0; }
            else us = 0;
            named = 1;
            k++;
            continue;
        }
        if (b == '}' && named) {
            if (us) { refuse(r, E_SPECIAL_PARAM, us_off); return 0; } /* `${_}` */
            *fl |= f2; *line += l2;
            return k + 1;
        }
        refuse(r, E_PARAM_OP, k);
        return 0;
    }
}

/* Scan the word that starts at b. On a normal end returns 1 with *end set; else 0 (outcome set). */
static int word(Ref *r, size_t b, size_t *end, uint64_t *flags, uint64_t *nseg, uint64_t *aux, uint64_t *line_out)
{
    uint64_t fl = 0, ns = 0, line = r->line;
    size_t eq = 0;  /* offset of the `=` of a leading NAME=, 0 if none */
    size_t app = 0; /* offset of the `=` of a leading NAME+= (append assignment), 0 if none */
    {
        size_t j = b;
        int first = 1;
        for (;;) {
            while (j + 1 < r->n && r->in[j] == '\\' && r->in[j + 1] == '\n') j += 2;
            if (j >= r->n) break;
            int c = r->in[j];
            if (c == '=' && !first) { eq = j; break; }
            if (c == '+' && !first) {
                size_t q = j + 1;
                while (q + 1 < r->n && r->in[q] == '\\' && r->in[q + 1] == '\n') q += 2;
                if (q < r->n && r->in[q] == '=') app = q;
                break;
            }
            if (first ? is_alpha_(c) : (is_alpha_(c) || is_digit(c))) { first = 0; j++; continue; }
            break;
        }
    }
    size_t j = b;
    /* unquoted brace expansion detector: depth, a bit per open depth for "has an unquoted , or .. directly inside" */
    unsigned bd = 0;
    uint64_t bm = 0;
    int pdot = 0;
    for (;;) {
        int rc = sk(r, &j, &fl, &line);
        int pd = pdot;
        pdot = 0;
        if (rc < 0) return stopz(r, rc);
        int c = peek(r, j);
        if (c < 0) return stopz(r, c);
        if (c == 0) { refuse(r, E_NUL, j); return 0; }
        if (c == ' ' || c == '\t' || c == '\n' || is_opch(c)) break;
        if (app && j == app) { refuse(r, E_APPEND_ASSIGN, j); return 0; }
        if (c == '\'') {
            ns++; fl |= 1;
            size_t k = j + 1;
            for (;;) {
                int d = peek(r, k);
                if (d < 0) return stopz(r, d);
                if (d == 0) { refuse(r, E_NUL, k); return 0; }
                if (d == '\n') line++;
                k++;
                if (d == '\'') break;
            }
            j = k;
        } else if (c == '"') {
            ns++; fl |= 2;
            size_t k = j + 1;
            for (;;) {
                rc = sk(r, &k, &fl, &line);
                if (rc < 0) return stopz(r, rc);
                int d = peek(r, k);
                if (d < 0) return stopz(r, d);
                if (d == 0) { refuse(r, E_NUL, k); return 0; }
                if (d == '"') { k++; break; }
                if (d == '`') { refuse(r, E_BACKTICK, k); return 0; }
                if (d == '\\') {
                    int e = peek(r, k + 1);
                    if (e == INCOMPLETE && r->eoi) { k++; continue; } /* a final lone backslash is literal */
                    if (e < 0) return stopz(r, e);
                    if (e == 0) { refuse(r, E_NUL, k + 1); return 0; }
                    fl |= 4;
                    k += 2;
                } else if (d == '$') {
                    fl |= 8;
                    size_t nk = dollar(r, k, 1, &fl, &line);
                    if (!nk) return 0;
                    k = nk;
                } else {
                    if (d == '\n') line++;
                    k++;
                }
            }
            j = k;
        } else if (c == '\\') {
            int e = peek(r, j + 1);
            if (e == INCOMPLETE && r->eoi) { j++; continue; } /* a final lone backslash is literal */
            if (e < 0) return stopz(r, e);
            if (e == 0) { refuse(r, E_NUL, j + 1); return 0; }
            fl |= 4;
            j += 2;
        } else if (c == '$') {
            fl |= 8;
            size_t nk = dollar(r, j, 0, &fl, &line);
            if (!nk) return 0;
            j = nk;
        } else if (c == '`') { refuse(r, E_BACKTICK, j); return 0;
        } else if (is_glob(c)) { refuse(r, E_GLOB, j); return 0;
        } else if (c == '~' && eq && j > eq) {
            size_t pp = prev_sig(r, j);
            if (pp == eq || r->in[pp] == ':') { refuse(r, E_TILDE, j); return 0; }
            j++;
        } else {
            if (c == '{') {
                if (bd >= 62) { refuse(r, E_BRACE, j); return 0; }
                bm &= ~(1ULL << bd);
                bd++;
            } else if (bd > 0) {
                if (c == '}') {
                    bd--;
                    if (bm & (1ULL << bd)) { refuse(r, E_BRACE, j); return 0; }
                } else if (c == ',') {
                    bm |= 1ULL << (bd - 1);
                } else if (c == '.') {
                    if (pd) bm |= 1ULL << (bd - 1);
                    else pdot = 1;
                }
            }
            j++;
        }
    }
    *end = j;
    *flags = fl;
    *nseg = ns;
    *aux = eq ? (uint64_t)eq + 1 : 0;
    *line_out = line;
    return 1;
}

void osh_lex_ref_eoi(const uint8_t *in, size_t n, int eoi, OshRefLex *out)
{
    memset(out, 0, sizeof *out);
    Ref r = { in, n, out, 1, 0, eoi };
    size_t i = 0;
    for (;;) {
        if (i == n) { out->status = r.last_was_nl || n == 0 ? 0 : 100; return; }
        int c = peek(&r, i);
        if (c < 0) { stop(&r, c); return; }
        if (c == 0) { refuse(&r, E_NUL, i); return; }
        if (c == ' ' || c == '\t') { r.last_was_nl = 0; i++; continue; }
        if (c == '\n') {
            if (add(&r, K_NL, 0, 0, r.line, i, 1, 0)) return;
            r.line++; r.last_was_nl = 1; i++; continue;
        }
        r.last_was_nl = 0;
        if (c == '#') {
            size_t k = i + 1;
            for (;;) {
                int d = peek(&r, k);
                if (d < 0) { stop(&r, d); return; }
                if (d == 0) { refuse(&r, E_NUL, k); return; }
                if (d == '\n') break;
                k++;
            }
            i = k; /* the newline is handled by the loop head */
            continue;
        }
        if (c == '\\') { /* a continuation between words is only white space */
            uint64_t np = 0;
            size_t k = i;
            if (skipc(&r, &k, &np) < 0) { r.o->status = 100; return; }
            if (np) { r.line += np; i = k; continue; }
        }
        if (c == '(' || c == ')') { refuse(&r, E_SUBSHELL, i); return; }
        if (c == '~') { refuse(&r, E_TILDE, i); return; }
        if (is_opch(c)) {
            uint64_t np = 0; /* pairs between the first operator byte and the byte that follows it */
            size_t q = i + 1;
            if (skipc(&r, &q, &np) < 0) { r.o->status = 100; return; }
            int c2 = peek(&r, q);
            if (c2 < 0) { stop(&r, c2); return; }
            if (c2 == 0) { refuse(&r, E_NUL, q); return; }
            unsigned kind = 0;
            int two = 0;
            uint64_t aux = 0;
            if (c == '|') { if (c2 == '|') { kind = K_OR; two = 1; } else kind = K_PIPE; }
            else if (c == '&') {
                if (c2 == '&') { kind = K_AND; two = 1; } else { refuse(&r, E_BACKGROUND, i); return; }
            }
            else if (c == ';') { if (c2 == ';') { refuse(&r, E_CASEEND, q); return; } kind = K_SEMI; }
            else if (c == '<') {
                if (c2 == '<') { refuse(&r, E_HEREDOC, q); return; }
                if (c2 == '>') { refuse(&r, E_REDIR_OTHER, q); return; }
                if (c2 == '&') { kind = K_DUP_IN; two = 1; } else kind = K_LT;
            } else { /* '>' */
                aux = 1;
                if (c2 == '|') { refuse(&r, E_REDIR_OTHER, q); return; }
                if (c2 == '>') { kind = K_APPEND; two = 1; } else if (c2 == '&') { kind = K_DUP_OUT; two = 1; } else kind = K_GT;
            }
            size_t oend = two ? q + 1 : i + 1; /* the token ends after its last byte */
            if (kind == K_LT || kind == K_DUP_IN || kind == K_GT || kind == K_APPEND || kind == K_DUP_OUT) {
                OshRefTok *pt = out->ntok ? &out->tok[out->ntok - 1] : NULL;
                int adjacent = pt && pt->kind == K_WORD && (pt->flags & ~32ULL) == 0 && pt->start + pt->len == i;
                if (adjacent) {
                    unsigned sig = 0;
                    int v = 0, digits = 1;
                    for (uint64_t z = 0; z < pt->len; z++) {
                        uint64_t o = pt->start + z;
                        if (in[o] == '\\' && z + 1 < pt->len && in[o + 1] == '\n') { z++; continue; }
                        if (!is_digit(in[o])) digits = 0;
                        if (!sig) v = in[o];
                        sig++;
                    }
                    if (digits && (sig != 1 || v > '2')) { refuse(&r, E_FD_RANGE, i); return; }
                    if (digits) { /* IO number: becomes the operator */
                        pt->kind = kind; pt->len = oend - pt->start; pt->aux = (uint64_t)(v - '0');
                        if (two) r.line += np;
                        i = oend;
                        continue;
                    }
                }
            }
            if (add(&r, kind, 0, 0, r.line, i, oend - i, aux)) return;
            if (two) r.line += np;
            i = oend;
            continue;
        }
        size_t end;
        uint64_t fl, ns, aux, wl;
        if (!word(&r, i, &end, &fl, &ns, &aux, &wl)) return;
        if ((fl & ~32ULL) == 0 && !aux) { /* a lone `{` or `}` where a command may start is the reserved group syntax */
            int cs = out->ntok == 0;
            if (!cs) {
                uint64_t pk = out->tok[out->ntok - 1].kind;
                cs = pk == K_NL || pk == K_PIPE || pk == K_OR || pk == K_AND || pk == K_SEMI;
            }
            if (cs) {
                unsigned sig = 0;
                int v = 0;
                for (size_t q = i; q < end; q++) {
                    if (in[q] == '\\' && q + 1 < end && in[q + 1] == '\n') { q++; continue; }
                    sig++; v = in[q];
                }
                if (sig == 1 && (v == '{' || v == '}')) { refuse(&r, E_GROUP, i); return; }
            }
        }
        if (aux) fl |= 16;
        if (add(&r, K_WORD, fl, ns, r.line, i, end - i, aux)) return;
        r.line = wl;
        i = end;
    }
}

void osh_lex_ref(const uint8_t *in, size_t n, OshRefLex *out)
{
    osh_lex_ref_eoi(in, n, 0, out);
}
