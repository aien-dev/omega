/* omega_lex.c -- Omega surface language V0 lexer. See omega_lex.h. */
#include "omega_lex.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static void set_err(char *err, size_t n, uint32_t col, const char *msg) {
    if (err && n) snprintf(err, n, "column %u: %s", (unsigned)col, msg);
}

const char *omega_language_tok_text(OmegaTokKind k) {
    switch (k) {
    case OTOK_EOF: return "end of line";
    case OTOK_IDENT: return "name";
    case OTOK_INT: return "integer";
    case OTOK_LET: return "let";
    case OTOK_FN: return "fn";
    case OTOK_REQUIRES: return "requires";
    case OTOK_ENSURES: return "ensures";
    case OTOK_TRUE: return "true";
    case OTOK_FALSE: return "false";
    case OTOK_LPAREN: return "(";
    case OTOK_RPAREN: return ")";
    case OTOK_LBRACE: return "{";
    case OTOK_RBRACE: return "}";
    case OTOK_COLON: return ":";
    case OTOK_COMMA: return ",";
    case OTOK_ARROW: return "->";
    case OTOK_ASSIGN: return "=";
    case OTOK_PLUS: return "+";
    case OTOK_MINUS: return "-";
    case OTOK_STAR: return "*";
    case OTOK_SLASH: return "/";
    case OTOK_AMP: return "&";
    case OTOK_PIPE: return "|";
    case OTOK_EQEQ: return "==";
    case OTOK_NE: return "!=";
    case OTOK_LT: return "<";
    case OTOK_LE: return "<=";
    case OTOK_GT: return ">";
    case OTOK_GE: return ">=";
    case OTOK_BANG: return "!";
    }
    return "?";
}

static bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static bool is_ident_char(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}
static int digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 99;
}

/* Decode an integer literal starting at s[*i]. Detects > 64-bit overflow. */
static int lex_int(const char *s, size_t *i, uint64_t *out, uint32_t col, char *err, size_t n) {
    unsigned base = 10;
    size_t p = *i;
    if (s[p] == '0' && s[p + 1] == 'x') { base = 16; p += 2; }
    else if (s[p] == '0' && s[p + 1] == 'b') { base = 2; p += 2; }
    size_t start = p;
    uint64_t v = 0;
    bool overflow = false;
    while (s[p]) {
        int d = digit_value(s[p]);
        if (d >= (int)base) break;
        if (v > (UINT64_MAX - (uint64_t)d) / base) overflow = true;
        else v = v * base + (uint64_t)d;
        p++;
    }
    if (p == start) {
        set_err(err, n, col, base == 16 ? "'0x' needs hex digits after it"
                                        : "'0b' needs binary digits (0 or 1) after it");
        return -1;
    }
    if (is_ident_char(s[p])) {
        set_err(err, n, (uint32_t)(p + 1),
                "a number runs straight into letters or digits it cannot use (V0 has no literal suffixes; write `7: u64`)");
        return -1;
    }
    if (overflow) {
        set_err(err, n, col, "integer literal does not fit in 64 bits");
        return -2;
    }
    *i = p;
    *out = v;
    return 0;
}

static OmegaTokKind keyword(const char *w) {
    if (strcmp(w, "let") == 0) return OTOK_LET;
    if (strcmp(w, "fn") == 0) return OTOK_FN;
    if (strcmp(w, "requires") == 0) return OTOK_REQUIRES;
    if (strcmp(w, "ensures") == 0) return OTOK_ENSURES;
    if (strcmp(w, "true") == 0) return OTOK_TRUE;
    if (strcmp(w, "false") == 0) return OTOK_FALSE;
    return OTOK_IDENT;
}

int omega_language_lex(const char *src, OmegaToken *toks, size_t max, size_t *count,
                       char *err, size_t n) {
    if (err && n) err[0] = '\0';
    if (!src || !toks || !count || max == 0) {
        set_err(err, n, 0, "internal: bad lexer arguments");
        return -1;
    }
    *count = 0;
    size_t len = strnlen(src, OMEGA_LANG_LINE_MAX + 1);
    if (len > OMEGA_LANG_LINE_MAX) {
        set_err(err, n, OMEGA_LANG_LINE_MAX, "line is too long for V0");
        return -3;
    }
    size_t i = 0, k = 0;
    while (1) {
        char c = src[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }
        if (c == '/' && src[i + 1] == '/') break;               /* line comment */
        if (c == '/' && src[i + 1] == '*') {                    /* block comment */
            size_t j = i + 2;
            while (src[j] && !(src[j] == '*' && src[j + 1] == '/')) j++;
            if (!src[j]) { set_err(err, n, (uint32_t)(i + 1), "comment is never closed with */"); return -1; }
            i = j + 2;
            continue;
        }
        if (c == '\0') break;
        if (k + 1 >= max) { set_err(err, n, (uint32_t)(i + 1), "too many tokens on one line"); return -3; }
        OmegaToken *t = &toks[k];
        memset(t, 0, sizeof(*t));
        t->col = (uint32_t)(i + 1);
        if (c >= '0' && c <= '9') {
            int rc = lex_int(src, &i, &t->value, t->col, err, n);
            if (rc) return rc;
            t->kind = OTOK_INT;
            k++;
            continue;
        }
        if (is_ident_start(c)) {
            size_t j = i;
            while (is_ident_char(src[j])) j++;
            if (j - i >= OMEGA_LANG_IDENT_MAX) { set_err(err, n, t->col, "name is longer than 63 characters"); return -1; }
            memcpy(t->text, src + i, j - i);
            t->text[j - i] = '\0';
            t->kind = keyword(t->text);
            i = j;
            k++;
            continue;
        }
        char d = src[i + 1];
        size_t adv = 1;
        switch (c) {
        case '(': t->kind = OTOK_LPAREN; break;
        case ')': t->kind = OTOK_RPAREN; break;
        case '{': t->kind = OTOK_LBRACE; break;
        case '}': t->kind = OTOK_RBRACE; break;
        case ':': t->kind = OTOK_COLON; break;
        case ',': t->kind = OTOK_COMMA; break;
        case '+': t->kind = OTOK_PLUS; break;
        case '*': t->kind = OTOK_STAR; break;
        case '/': t->kind = OTOK_SLASH; break;
        case '&': t->kind = OTOK_AMP; break;
        case '|': t->kind = OTOK_PIPE; break;
        case '-': if (d == '>') { t->kind = OTOK_ARROW; adv = 2; } else t->kind = OTOK_MINUS; break;
        case '=': if (d == '=') { t->kind = OTOK_EQEQ; adv = 2; } else t->kind = OTOK_ASSIGN; break;
        case '!': if (d == '=') { t->kind = OTOK_NE; adv = 2; } else t->kind = OTOK_BANG; break;
        case '<': if (d == '=') { t->kind = OTOK_LE; adv = 2; } else t->kind = OTOK_LT; break;
        case '>': if (d == '=') { t->kind = OTOK_GE; adv = 2; } else t->kind = OTOK_GT; break;
        case '"': case '\'':
            set_err(err, n, t->col, "strings and characters are not part of Omega V0");
            return -2;
        case '[': case ']':
            set_err(err, n, t->col, "lists/containers are not part of Omega V0");
            return -2;
        default: {
            char msg[64];
            if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7f)
                snprintf(msg, sizeof msg, "unexpected character '%c'", c);
            else
                snprintf(msg, sizeof msg, "unexpected byte 0x%02x", (unsigned)(unsigned char)c);
            set_err(err, n, t->col, msg);
            return -1;
        }
        }
        i += adv;
        k++;
    }
    memset(&toks[k], 0, sizeof(toks[k]));
    toks[k].kind = OTOK_EOF;
    toks[k].col = (uint32_t)(i + 1);
    *count = k + 1;
    return 0;
}
