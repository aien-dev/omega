/*
 * omega_lex.h -- Omega surface language V0 lexer (spec/omega-language-v0.md).
 *
 * Tokens are disposable: they never enter any SemanticId. Comments and
 * whitespace are dropped here, which is what makes `x+y` and `x + y` with a
 * block comment in between lower to the same object. Integer literals are
 * decoded to their numeric value here (decimal, 0x hex, 0b binary), so the
 * spelling of a literal never survives past the lexer.
 */
#ifndef OMEGA_LANG_LEX_H
#define OMEGA_LANG_LEX_H

#include <stddef.h>
#include <stdint.h>

#define OMEGA_LANG_MAX_TOKENS   256
#define OMEGA_LANG_IDENT_MAX    64   /* including NUL */
#define OMEGA_LANG_LINE_MAX     1024

typedef enum {
    OTOK_EOF = 0,
    OTOK_IDENT,
    OTOK_INT,
    /* keywords */
    OTOK_LET, OTOK_FN, OTOK_REQUIRES, OTOK_ENSURES, OTOK_TRUE, OTOK_FALSE,
    /* punctuation */
    OTOK_LPAREN, OTOK_RPAREN, OTOK_LBRACE, OTOK_RBRACE, OTOK_COLON, OTOK_COMMA,
    OTOK_ARROW,   /* -> */
    OTOK_ASSIGN,  /* =  */
    /* operators */
    OTOK_PLUS, OTOK_MINUS, OTOK_STAR, OTOK_SLASH, OTOK_AMP, OTOK_PIPE,
    OTOK_EQEQ, OTOK_NE, OTOK_LT, OTOK_LE, OTOK_GT, OTOK_GE, OTOK_BANG
} OmegaTokKind;

typedef struct {
    OmegaTokKind kind;
    uint32_t col;                       /* 1-based column of first char */
    uint64_t value;                     /* OTOK_INT: decoded value */
    char text[OMEGA_LANG_IDENT_MAX];    /* OTOK_IDENT / keywords: the word */
} OmegaToken;

/* Tokenize one line. Always ends with an OTOK_EOF token (counted in *count).
 * Returns 0 ok, -1 syntax error, -2 unsupported construct (strings,
 * containers, literal wider than 64 bits), -3 too many tokens / line too long.
 * `err` gets a one-line message naming the column. */
int omega_language_lex(const char *src, OmegaToken *toks, size_t max, size_t *count,
                       char *err, size_t n);

/* Canonical spelling of a token kind (for messages and contract text). */
const char *omega_language_tok_text(OmegaTokKind k);

#endif /* OMEGA_LANG_LEX_H */
