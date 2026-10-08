/*
 * osc_lex.c -- OSC-1 lexer. See osc_lex.h.
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 */
#include "osc_lex.h"

#include <string.h>

static const struct { const char *w; int k; } keywords[] = {
    {"fn", OT_FN}, {"let", OT_LET}, {"mut", OT_MUT}, {"own", OT_OWN}, {"alloc", OT_ALLOC},
    {"if", OT_IF}, {"else", OT_ELSE}, {"while", OT_WHILE}, {"bound", OT_BOUND}, {"for", OT_FOR},
    {"in", OT_IN}, {"return", OT_RETURN}, {"requires", OT_REQUIRES}, {"ensures", OT_ENSURES},
    {"as", OT_AS}, {"true", OT_TRUE}, {"false", OT_FALSE}, {"u8", OT_U8}, {"u16", OT_U16},
    {"u32", OT_U32}, {"u64", OT_U64}, {"i8", OT_I8}, {"i16", OT_I16}, {"i32", OT_I32},
    {"i64", OT_I64}, {"bool", OT_BOOL}, {"struct", OT_STRUCT}, {"arena", OT_ARENA},
    {"pool", OT_POOL}, {"handle", OT_HANDLE}, {"import", OT_IMPORT},
    {"bytes", OT_BYTES}, {"cells", OT_CELLS},
};

/* Reserved: destruction words (destruction has no surface syntax, III.8) and
 * the V0 effect / authority / control-flow words. Never names. */
static const char *const reserved[] = {
    "free", "drop", "delete", "dealloc",
    "effect", "cap", "capability", "mint", "revoke", "grant", "publish", "promote", "authority", "admin",
    "syscall", "io", "print", "extern", "unsafe", "asm", "loop", "match", "enum",
    "var", "const", "self", "rec", "break", "continue",
};

static const char *const kind_names[OT__COUNT] = {
    "end of input", "NAME", "INT", "reserved word",
    "fn", "let", "mut", "own", "alloc", "if", "else", "while", "bound", "for", "in", "return", "requires",
    "ensures", "as", "true", "false", "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "bool",
    "(", ")", "{", "}", "[", "]", ",", ";", ":", "->", "..", "=",
    "+", "-", "*", "/", "%", "&", "|", "^", "~", "!", "&&", "||", "<<", ">>", "==", "!=", "<", "<=", ">", ">=",
    "struct", ".", "arena", "pool", "handle", "import",
    "bytes", "cells",
};

const char *osc_tok_kind_name(int kind)
{
    return (kind >= 0 && kind < OT__COUNT) ? kind_names[kind] : "?";
}

void osc_tok_text(const char *src, const OscToken *t, char *buf, size_t cap)
{
    size_t n = t->len;
    if (cap == 0) return;
    if (n >= cap) n = cap - 1;
    memcpy(buf, src + t->off, n);
    buf[n] = 0;
}

static int is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int digit_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 99;
}

int osc_lex(const char *src, size_t len, OscToken *toks, uint32_t cap, uint32_t *ntok, OscDiag *d)
{
    size_t i = 0;
    uint32_t line = 1, col = 1, n = 0;
#define ADV() do { if (src[i] == '\n') { line++; col = 1; } else col++; i++; } while (0)
    for (;;) {
        /* skip whitespace and comments */
        while (i < len) {
            char c = src[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ADV(); continue; }
            if (c == '/' && i + 1 < len && src[i + 1] == '/') {
                while (i < len && src[i] != '\n') ADV();
                continue;
            }
            if (c == '/' && i + 1 < len && src[i + 1] == '*') {
                uint32_t sl = line, sc = col;
                ADV(); ADV();
                while (i < len && !(src[i] == '*' && i + 1 < len && src[i + 1] == '/')) ADV();
                if (i >= len) {
                    osc_diag_set(d, OSC_DIAG_SYNTAX, sl, sc, "/*", 0, NULL, "unterminated comment",
                                 "unterminated /* comment");
                    return -1;
                }
                ADV(); ADV();
                continue;
            }
            break;
        }
        if (n >= cap) {
            osc_diag_set(d, OSC_DIAG_CAPACITY, line, col, "unit", 0, NULL, "token capacity",
                         "more than %u tokens", cap);
            return -1;
        }
        OscToken *t = &toks[n];
        memset(t, 0, sizeof *t);
        t->line = line;
        t->col = col;
        t->off = (uint32_t)i;
        if (i >= len) { t->kind = OT_EOF; n++; break; }
        char c = src[i];
        if (is_alpha(c)) {
            size_t s = i;
            while (i < len && (is_alpha(src[i]) || is_digit(src[i]))) ADV();
            t->len = (uint32_t)(i - s);
            char buf[OSC_LEX_NAME_MAX + 1];
            if (t->len > OSC_LEX_NAME_MAX) {
                osc_tok_text(src, t, buf, sizeof buf);
                osc_diag_set(d, OSC_DIAG_CAPACITY, t->line, t->col, buf, 0, NULL, "identifier length",
                             "identifier longer than %d characters", OSC_LEX_NAME_MAX);
                return -1;
            }
            osc_tok_text(src, t, buf, sizeof buf);
            t->kind = OT_NAME;
            for (size_t k = 0; k < sizeof keywords / sizeof keywords[0]; k++)
                if (strcmp(buf, keywords[k].w) == 0) t->kind = (uint16_t)keywords[k].k;
            for (size_t k = 0; k < sizeof reserved / sizeof reserved[0]; k++)
                if (strcmp(buf, reserved[k]) == 0) t->kind = OT_RESERVED;
            n++;
            continue;
        }
        if (is_digit(c)) {
            size_t s = i;
            unsigned base = 10;
            if (c == '0' && i + 1 < len && (src[i + 1] == 'x' || src[i + 1] == 'b')) {
                base = src[i + 1] == 'x' ? 16 : 2;
                ADV(); ADV();
            }
            size_t ds = i;
            int overflow = 0;
            uint64_t v = 0;
            while (i < len && (is_alpha(src[i]) || is_digit(src[i]))) {
                int dv = digit_val(src[i]);
                if (dv >= (int)base) break;
                if (v > (UINT64_MAX - (uint64_t)dv) / base) overflow = 1;
                v = v * base + (uint64_t)dv;
                ADV();
            }
            t->len = (uint32_t)(i - s);
            char buf[64];
            if (i == ds || (i < len && (is_alpha(src[i]) || is_digit(src[i])))) {
                while (i < len && (is_alpha(src[i]) || is_digit(src[i]))) ADV();
                t->len = (uint32_t)(i - s);
                osc_tok_text(src, t, buf, sizeof buf);
                osc_diag_set(d, OSC_DIAG_SYNTAX, t->line, t->col, buf, 0, NULL, "malformed literal",
                             "malformed integer literal '%s'", buf);
                return -1;
            }
            if (overflow) {
                osc_tok_text(src, t, buf, sizeof buf);
                osc_diag_set(d, OSC_DIAG_OVERFLOW_UNSAFE, t->line, t->col, buf, 0, NULL, "literal wider than 64 bits",
                             "integer literal does not fit in 64 bits");
                return -1;
            }
            t->kind = OT_INT;
            t->ival = v;
            n++;
            continue;
        }
        /* punctuation: longest match first */
        static const struct { const char *s; int k; } punct[] = {
            {"->", OT_ARROW}, {"..", OT_DOTDOT}, {"&&", OT_ANDAND}, {"||", OT_OROR}, {"<<", OT_SHL},
            {">>", OT_SHR}, {"==", OT_EQ}, {"!=", OT_NE}, {"<=", OT_LE}, {">=", OT_GE},
            {"(", OT_LPAREN}, {")", OT_RPAREN}, {"{", OT_LBRACE}, {"}", OT_RBRACE}, {"[", OT_LBRACK},
            {"]", OT_RBRACK}, {",", OT_COMMA}, {";", OT_SEMI}, {":", OT_COLON}, {"=", OT_ASSIGN},
            {"+", OT_PLUS}, {"-", OT_MINUS}, {"*", OT_STAR}, {"/", OT_SLASH}, {"%", OT_PERCENT},
            {"&", OT_AMP}, {"|", OT_PIPE}, {"^", OT_CARET}, {"~", OT_TILDE}, {"!", OT_BANG},
            {"<", OT_LT}, {">", OT_GT}, {".", OT_DOT},
        };
        int found = 0;
        for (size_t k = 0; k < sizeof punct / sizeof punct[0]; k++) {
            size_t pl = strlen(punct[k].s);
            if (i + pl <= len && memcmp(src + i, punct[k].s, pl) == 0) {
                t->kind = (uint16_t)punct[k].k;
                t->len = (uint32_t)pl;
                for (size_t q = 0; q < pl; q++) ADV();
                found = 1;
                break;
            }
        }
        if (!found) {
            char buf[8];
            unsigned char uc = (unsigned char)c;
            if (uc >= 0x20 && uc < 0x7f) { buf[0] = (char)uc; buf[1] = 0; }
            else
            {
                static const char hx[] = "0123456789abcdef";
                buf[0] = '\\'; buf[1] = 'x'; buf[2] = hx[uc >> 4]; buf[3] = hx[uc & 15]; buf[4] = 0;
            }
            osc_diag_set(d, OSC_DIAG_SYNTAX, line, col, buf, 0, NULL, "unexpected character",
                         "unexpected character '%s'", buf);
            return -1;
        }
        n++;
    }
#undef ADV
    *ntok = n;
    return 0;
}
