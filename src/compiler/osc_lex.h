/*
 * osc_lex.h -- OSC-1 lexer (docs/osc/OSC-1-DESIGN.md section 2; literal,
 * comment and identifier spelling as in spec/omega-language-v0.md).
 * Multi-line source, fixed-capacity token array, no allocation.
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 */
#ifndef OSC_LEX_H
#define OSC_LEX_H

#include <stddef.h>
#include <stdint.h>
#include "osc_diag.h"

#define OSC_LEX_MAX_TOKENS 32768
#define OSC_LEX_NAME_MAX   63   /* identifier length limit (V0) */

typedef enum {
    OT_EOF = 0, OT_NAME, OT_INT, OT_RESERVED,
    /* keywords */
    OT_FN, OT_LET, OT_MUT, OT_OWN, OT_ALLOC, OT_IF, OT_ELSE, OT_WHILE, OT_BOUND, OT_FOR, OT_IN,
    OT_RETURN, OT_REQUIRES, OT_ENSURES, OT_AS, OT_TRUE, OT_FALSE,
    OT_U8, OT_U16, OT_U32, OT_U64, OT_I8, OT_I16, OT_I32, OT_I64, OT_BOOL,
    /* punctuation */
    OT_LPAREN, OT_RPAREN, OT_LBRACE, OT_RBRACE, OT_LBRACK, OT_RBRACK, OT_COMMA, OT_SEMI, OT_COLON,
    OT_ARROW, OT_DOTDOT, OT_ASSIGN,
    OT_PLUS, OT_MINUS, OT_STAR, OT_SLASH, OT_PERCENT, OT_AMP, OT_PIPE, OT_CARET, OT_TILDE, OT_BANG,
    OT_ANDAND, OT_OROR, OT_SHL, OT_SHR, OT_EQ, OT_NE, OT_LT, OT_LE, OT_GT, OT_GE,
    /* OSC-2 item 2 (structs), appended */
    OT_STRUCT, OT_DOT,
    /* OSC-2 item 3 (arenas), appended */
    OT_ARENA,
    /* OSC-3 item 2 (versioned handles), appended */
    OT_POOL, OT_HANDLE,
    OT__COUNT
} OscTokKind;

typedef struct {
    uint16_t kind;      /* OscTokKind */
    uint32_t line, col; /* 1-based */
    uint32_t off, len;  /* spelling in the source */
    uint64_t ival;      /* OT_INT value */
} OscToken;

/* Tokenise src[0..len). 0 ok (toks ends with OT_EOF), -1 with *d filled:
 * SYNTAX (bad character, unterminated comment, malformed literal),
 * OVERFLOW_UNSAFE (literal wider than 64 bits), CAPACITY (too many tokens,
 * identifier longer than 63 characters). */
int osc_lex(const char *src, size_t len, OscToken *toks, uint32_t cap, uint32_t *ntok, OscDiag *d);

/* Copy a token's spelling (truncated to cap-1). */
void osc_tok_text(const char *src, const OscToken *t, char *buf, size_t cap);
/* Printable name of a token kind ("fn", "(", "NAME", ...). */
const char *osc_tok_kind_name(int kind);

#endif /* OSC_LEX_H */
