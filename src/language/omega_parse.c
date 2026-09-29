/* omega_parse.c -- Omega surface language V0 parser. See omega_parse.h. */
#include "omega_parse.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const OmegaToken *t;
    size_t count;
    size_t pos;
    OmegaAst *ast;
    char *err;
    size_t n;
} Parser;

static int fail(Parser *p, uint32_t col, int rc, const char *msg) {
    if (p->err && p->n) snprintf(p->err, p->n, "column %u: %s", (unsigned)col, msg);
    return rc;
}

static const OmegaToken *peek(Parser *p) { return &p->t[p->pos < p->count ? p->pos : p->count - 1]; }
static const OmegaToken *peek2(Parser *p) {
    size_t i = p->pos + 1;
    return &p->t[i < p->count ? i : p->count - 1];
}
static const OmegaToken *next(Parser *p) {
    const OmegaToken *t = peek(p);
    if (p->pos < p->count - 1) p->pos++;
    return t;
}

static int expect(Parser *p, OmegaTokKind k, const char *what) {
    const OmegaToken *t = peek(p);
    if (t->kind != k) {
        char msg[160];
        snprintf(msg, sizeof msg, "expected %s but found '%s'", what,
                 t->kind == OTOK_IDENT ? t->text : omega_language_tok_text(t->kind));
        return fail(p, t->col, -1, msg);
    }
    next(p);
    return 0;
}

static const char *const RESERVED_EXTRA[] = {
    /* types */
    "u8", "u16", "u32", "u64", "bool", "i8", "i16", "i32", "i64",
    /* contract */
    "result",
    /* effects / authority -- never expressible in V0 */
    "effect", "effects", "cap", "capability", "mint", "revoke", "grant", "publish",
    "promote", "authority", "admin", "syscall", "io", "print", "import", "extern",
    "unsafe", "asm",
    /* control flow / mutation -- not in V0 */
    "if", "else", "while", "for", "loop", "return", "match", "struct", "enum",
    "mut", "var", "const", "self", "rec",
    NULL
};

bool omega_language_is_reserved(const char *w) {
    if (!w) return true;
    static const char *const kw[] = { "let", "fn", "requires", "ensures", "true", "false", NULL };
    for (int i = 0; kw[i]; i++) if (strcmp(w, kw[i]) == 0) return true;
    for (int i = 0; RESERVED_EXTRA[i]; i++) if (strcmp(w, RESERVED_EXTRA[i]) == 0) return true;
    return false;
}

const char *omega_language_type_text(const OmegaLangType *t) {
    if (!t) return "?";
    if (t->tag == TYPE_BOOL) return "bool";
    if (t->tag == TYPE_UNSIGNED_INT) {
        switch (t->width) {
        case 8: return "u8";
        case 16: return "u16";
        case 32: return "u32";
        case 64: return "u64";
        default: break;
        }
    }
    return "?";
}

static int parse_type(Parser *p, OmegaLangType *out) {
    const OmegaToken *t = peek(p);
    if (t->kind != OTOK_IDENT)
        return fail(p, t->col, -1, "expected a type (u8, u16, u32, u64 or bool)");
    const char *w = t->text;
    memset(out, 0, sizeof(*out));
    if (strcmp(w, "u8") == 0) { out->tag = TYPE_UNSIGNED_INT; out->width = 8; }
    else if (strcmp(w, "u16") == 0) { out->tag = TYPE_UNSIGNED_INT; out->width = 16; }
    else if (strcmp(w, "u32") == 0) { out->tag = TYPE_UNSIGNED_INT; out->width = 32; }
    else if (strcmp(w, "u64") == 0) { out->tag = TYPE_UNSIGNED_INT; out->width = 64; }
    else if (strcmp(w, "bool") == 0) { out->tag = TYPE_BOOL; out->width = 1; }
    else if (w[0] == 'i' && (strcmp(w, "i8") == 0 || strcmp(w, "i16") == 0 ||
                             strcmp(w, "i32") == 0 || strcmp(w, "i64") == 0))
        return fail(p, t->col, -2,
                    "signed integers are not supported in V0 (the existing builders and evaluator are unsigned-only)");
    else {
        char msg[160];
        snprintf(msg, sizeof msg, "'%s' is not a V0 type (use u8, u16, u32, u64 or bool)", w);
        return fail(p, t->col, -2, msg);
    }
    next(p);
    return 0;
}

static int new_node(Parser *p, OmegaAstKind k, uint32_t col) {
    OmegaAst *a = p->ast;
    if (a->node_count >= OMEGA_AST_MAX_NODES) return -1;
    OmegaAstNode *nd = &a->nodes[a->node_count];
    memset(nd, 0, sizeof(*nd));
    nd->kind = k;
    nd->col = col;
    nd->lhs = nd->rhs = -1;
    return (int)a->node_count++;
}

static int alloc_or_fail(Parser *p, OmegaAstKind k, uint32_t col, int *out) {
    int idx = new_node(p, k, col);
    if (idx < 0) return fail(p, col, -3, "expression is too large for V0 (node limit reached)");
    *out = idx;
    return 0;
}

static int parse_expr(Parser *p, int depth, int *out);

static int reserved_msg(Parser *p, const OmegaToken *t) {
    char msg[200];
    snprintf(msg, sizeof msg,
             "'%s' is reserved: effects, authority, control flow and type names are not values in Omega V0",
             t->text);
    return fail(p, t->col, -2, msg);
}

static int parse_primary(Parser *p, int depth, int *out) {
    const OmegaToken *t = peek(p);
    int rc, idx;
    switch (t->kind) {
    case OTOK_INT:
        if ((rc = alloc_or_fail(p, OAST_INT, t->col, &idx))) return rc;
        p->ast->nodes[idx].value = t->value;
        next(p);
        *out = idx;
        return 0;
    case OTOK_TRUE:
    case OTOK_FALSE:
        if ((rc = alloc_or_fail(p, OAST_BOOL, t->col, &idx))) return rc;
        p->ast->nodes[idx].bval = (t->kind == OTOK_TRUE);
        next(p);
        *out = idx;
        return 0;
    case OTOK_IDENT:
        if (peek2(p)->kind == OTOK_LPAREN)
            return fail(p, t->col, -2, "function calls are not supported in V0");
        if (omega_language_is_reserved(t->text)) return reserved_msg(p, t);
        if ((rc = alloc_or_fail(p, OAST_NAME, t->col, &idx))) return rc;
        snprintf(p->ast->nodes[idx].name, sizeof p->ast->nodes[idx].name, "%s", t->text);
        next(p);
        *out = idx;
        return 0;
    case OTOK_LPAREN: {
        next(p);
        int inner;
        if ((rc = parse_expr(p, depth + 1, &inner))) return rc;
        if (peek(p)->kind == OTOK_COLON) {
            uint32_t ccol = peek(p)->col;
            next(p);
            OmegaLangType ty;
            if ((rc = parse_type(p, &ty))) return rc;
            if ((rc = alloc_or_fail(p, OAST_ASCRIBE, ccol, &idx))) return rc;
            p->ast->nodes[idx].lhs = inner;
            p->ast->nodes[idx].type = ty;
            inner = idx;
        }
        if ((rc = expect(p, OTOK_RPAREN, "')'"))) return rc;
        *out = inner;
        return 0;
    }
    case OTOK_MINUS:
        return fail(p, t->col, -2, "negative numbers are not supported in V0 (unsigned integers only)");
    case OTOK_BANG:
        return fail(p, t->col, -2, "'!' (logical not) is not supported in V0");
    case OTOK_LBRACE:
        return fail(p, t->col, -2, "blocks and records are not supported in V0 expressions");
    case OTOK_LET:
    case OTOK_FN:
    case OTOK_REQUIRES:
    case OTOK_ENSURES:
        return fail(p, t->col, -1, "keyword cannot be used inside an expression");
    default: {
        char msg[128];
        snprintf(msg, sizeof msg, "expected a value but found '%s'", omega_language_tok_text(t->kind));
        return fail(p, t->col, -1, msg);
    }
    }
}

/* Binary precedence levels, lowest first. Each level is a loop (left-assoc),
 * so a long operator chain does not recurse. */
static int level_of(OmegaTokKind k) {
    switch (k) {
    case OTOK_EQEQ: case OTOK_NE: case OTOK_LT: case OTOK_LE: case OTOK_GT: case OTOK_GE: return 0;
    case OTOK_PIPE: return 1;
    case OTOK_AMP: return 2;
    case OTOK_PLUS: case OTOK_MINUS: return 3;
    case OTOK_STAR: case OTOK_SLASH: return 4;
    default: return -1;
    }
}

static int parse_level(Parser *p, int level, int depth, int *out) {
    int rc, lhs;
    if (level > 4) return parse_primary(p, depth, out);
    if ((rc = parse_level(p, level + 1, depth, &lhs))) return rc;
    int seen_cmp = 0;
    while (level_of(peek(p)->kind) == level) {
        const OmegaToken *op = next(p);
        if (level == 0 && seen_cmp++)
            return fail(p, op->col, -1, "comparisons cannot be chained; use parentheses");
        int rhs, idx;
        if ((rc = parse_level(p, level + 1, depth, &rhs))) return rc;
        if ((rc = alloc_or_fail(p, OAST_BINARY, op->col, &idx))) return rc;
        p->ast->nodes[idx].op = op->kind;
        p->ast->nodes[idx].lhs = lhs;
        p->ast->nodes[idx].rhs = rhs;
        lhs = idx;
    }
    *out = lhs;
    return 0;
}

static int parse_expr(Parser *p, int depth, int *out) {
    if (depth > OMEGA_AST_MAX_DEPTH)
        return fail(p, peek(p)->col, -3, "expression is nested too deeply for V0 (limit 32)");
    return parse_level(p, 0, depth, out);
}

static int expect_end(Parser *p) {
    const OmegaToken *t = peek(p);
    if (t->kind == OTOK_EOF) return 0;
    char msg[128];
    snprintf(msg, sizeof msg, "unexpected '%s' after the end of the statement",
             t->kind == OTOK_IDENT ? t->text : omega_language_tok_text(t->kind));
    return fail(p, t->col, -1, msg);
}

static int check_binding_name(Parser *p, const OmegaToken *t) {
    if (t->kind != OTOK_IDENT) return fail(p, t->col, -1, "expected a name");
    if (strcmp(t->text, "_") == 0) return fail(p, t->col, -1, "'_' is reserved for the last result");
    if (omega_language_is_reserved(t->text)) return reserved_msg(p, t);
    return 0;
}

/* Canonical contract clause: tokens joined by single spaces (no space after
 * '(' or before ')'), literals in decimal, parameter renamed to `x`.
 * Stops at `stop1` or `stop2` at paren depth 0. */
static int parse_clause(Parser *p, OmegaTokKind stop1, OmegaTokKind stop2, bool allow_result,
                        char *out, size_t out_len) {
    size_t len = 0;
    int parens = 0;
    bool first = true, prev_lparen = false;
    const OmegaToken *start = peek(p);
    out[0] = '\0';
    while (1) {
        const OmegaToken *t = peek(p);
        if (parens == 0 && (t->kind == stop1 || t->kind == stop2)) break;
        char piece[OMEGA_LANG_IDENT_MAX + 24];
        switch (t->kind) {
        case OTOK_EOF:
            return fail(p, t->col, -1, "contract clause is not followed by the function body '{ ... }'");
        case OTOK_IDENT:
            if (strcmp(t->text, p->ast->param) == 0) snprintf(piece, sizeof piece, "x");
            else if (strcmp(t->text, "result") == 0) {
                if (!allow_result) return fail(p, t->col, -2, "'result' can only appear in 'ensures'");
                snprintf(piece, sizeof piece, "result");
            } else {
                char msg[160];
                snprintf(msg, sizeof msg,
                         "'%s' is not allowed in a contract (only the parameter, 'result', numbers and operators)",
                         t->text);
                return fail(p, t->col, -2, msg);
            }
            break;
        case OTOK_INT:
            snprintf(piece, sizeof piece, "%llu", (unsigned long long)t->value);
            break;
        case OTOK_TRUE: case OTOK_FALSE:
        case OTOK_PLUS: case OTOK_MINUS: case OTOK_STAR: case OTOK_SLASH:
        case OTOK_AMP: case OTOK_PIPE: case OTOK_EQEQ: case OTOK_NE:
        case OTOK_LT: case OTOK_LE: case OTOK_GT: case OTOK_GE: case OTOK_BANG:
            snprintf(piece, sizeof piece, "%s", omega_language_tok_text(t->kind));
            break;
        case OTOK_LPAREN:
            parens++;
            snprintf(piece, sizeof piece, "(");
            break;
        case OTOK_RPAREN:
            if (parens == 0) return fail(p, t->col, -1, "unbalanced ')' in contract clause");
            parens--;
            snprintf(piece, sizeof piece, ")");
            break;
        default: {
            char msg[128];
            snprintf(msg, sizeof msg, "'%s' is not allowed in a contract clause", omega_language_tok_text(t->kind));
            return fail(p, t->col, -1, msg);
        }
        }
        bool space = !first && !prev_lparen && t->kind != OTOK_RPAREN;
        size_t plen = strlen(piece);
        if (len + (space ? 1 : 0) + plen >= out_len)
            return fail(p, start->col, -2, "contract clause is longer than 63 characters (V0 contract text limit)");
        if (space) out[len++] = ' ';
        memcpy(out + len, piece, plen);
        len += plen;
        out[len] = '\0';
        first = false;
        prev_lparen = (t->kind == OTOK_LPAREN);
        next(p);
    }
    if (first) return fail(p, peek(p)->col, -1, "empty contract clause");
    return 0;
}

static int parse_fn(Parser *p) {
    int rc;
    OmegaAst *a = p->ast;
    next(p); /* fn */
    const OmegaToken *nm = peek(p);
    if ((rc = check_binding_name(p, nm))) return rc;
    snprintf(a->name, sizeof a->name, "%s", nm->text);
    next(p);
    if ((rc = expect(p, OTOK_LPAREN, "'(' after the function name"))) return rc;
    if (peek(p)->kind == OTOK_RPAREN)
        return fail(p, peek(p)->col, -2, "V0 functions take exactly one parameter");
    const OmegaToken *pn = peek(p);
    if ((rc = check_binding_name(p, pn))) return rc;
    if (strcmp(pn->text, a->name) == 0)
        return fail(p, pn->col, -2, "parameter cannot share the function's name (recursion is not supported)");
    snprintf(a->param, sizeof a->param, "%s", pn->text);
    next(p);
    if ((rc = expect(p, OTOK_COLON, "':' and a parameter type"))) return rc;
    if ((rc = parse_type(p, &a->param_type))) return rc;
    if (peek(p)->kind == OTOK_COMMA)
        return fail(p, peek(p)->col, -2, "V0 functions take exactly one parameter");
    if ((rc = expect(p, OTOK_RPAREN, "')'"))) return rc;
    if ((rc = expect(p, OTOK_ARROW, "'->' and a result type"))) return rc;
    if ((rc = parse_type(p, &a->ret_type))) return rc;
    snprintf(a->requires_text, sizeof a->requires_text, "true");
    snprintf(a->ensures_text, sizeof a->ensures_text, "true");
    if (peek(p)->kind == OTOK_REQUIRES) {
        next(p);
        if ((rc = parse_clause(p, OTOK_ENSURES, OTOK_LBRACE, false, a->requires_text, sizeof a->requires_text)))
            return rc;
    }
    if (peek(p)->kind == OTOK_ENSURES) {
        next(p);
        if ((rc = parse_clause(p, OTOK_LBRACE, OTOK_LBRACE, true, a->ensures_text, sizeof a->ensures_text)))
            return rc;
    }
    if ((rc = expect(p, OTOK_LBRACE, "'{' starting the function body"))) return rc;
    if ((rc = parse_expr(p, 0, &a->root))) return rc;
    if ((rc = expect(p, OTOK_RBRACE, "'}' closing the function body"))) return rc;
    return expect_end(p);
}

int omega_language_parse(const OmegaToken *toks, size_t count, OmegaAst *ast, char *err, size_t n) {
    if (err && n) err[0] = '\0';
    if (!ast) return -1;
    memset(ast, 0, sizeof(*ast));
    ast->root = -1;
    if (!toks || count == 0 || toks[count - 1].kind != OTOK_EOF) {
        if (err && n) snprintf(err, n, "column 0: internal: token stream must end with end-of-line");
        return -1;
    }
    Parser p = { toks, count, 0, ast, err, n };
    const OmegaToken *t = peek(&p);
    ast->stmt_col = t->col;
    int rc;
    switch (t->kind) {
    case OTOK_EOF:
        ast->stmt = OSTMT_EMPTY;
        return 0;
    case OTOK_LET: {
        ast->stmt = OSTMT_LET;
        next(&p);
        const OmegaToken *nm = peek(&p);
        if ((rc = check_binding_name(&p, nm))) return rc;
        snprintf(ast->name, sizeof ast->name, "%s", nm->text);
        next(&p);
        if ((rc = expect(&p, OTOK_COLON, "':' and a type (V0 bindings need an explicit type: let x: u64 = 7)")))
            return rc;
        if ((rc = parse_type(&p, &ast->let_type))) return rc;
        if ((rc = expect(&p, OTOK_ASSIGN, "'='"))) return rc;
        if ((rc = parse_expr(&p, 0, &ast->root))) return rc;
        return expect_end(&p);
    }
    case OTOK_FN:
        ast->stmt = OSTMT_FN;
        return parse_fn(&p);
    default: {
        ast->stmt = OSTMT_EXPR;
        int root;
        if ((rc = parse_expr(&p, 0, &root))) return rc;
        if (peek(&p)->kind == OTOK_COLON) {
            uint32_t ccol = peek(&p)->col;
            next(&p);
            OmegaLangType ty;
            if ((rc = parse_type(&p, &ty))) return rc;
            int idx;
            if ((rc = alloc_or_fail(&p, OAST_ASCRIBE, ccol, &idx))) return rc;
            ast->nodes[idx].lhs = root;
            ast->nodes[idx].type = ty;
            root = idx;
        }
        ast->root = root;
        return expect_end(&p);
    }
    }
}

int omega_language_parse_line(const char *src, OmegaAst *ast, char *err, size_t n) {
    OmegaToken toks[OMEGA_LANG_MAX_TOKENS];
    size_t count = 0;
    int rc = omega_language_lex(src, toks, OMEGA_LANG_MAX_TOKENS, &count, err, n);
    if (rc) {
        if (ast) { memset(ast, 0, sizeof(*ast)); ast->root = -1; }
        return rc;
    }
    return omega_language_parse(toks, count, ast, err, n);
}
