/*
 * omega_parse.h -- Omega surface language V0 parser (spec/omega-language-v0.md).
 *
 * The syntax tree lives in a fixed arena inside OmegaAst and is DISPOSABLE:
 * it is never hashed, stored, or used as identity. Lowering (omega_lower.h)
 * turns it into existing canonical OmegaGraph objects / OmegaProgram and the
 * tree is then thrown away.
 *
 * Recursion is bounded by OMEGA_AST_MAX_DEPTH (nesting of parentheses) and the
 * arena by OMEGA_AST_MAX_NODES; exceeding either fails closed with -3.
 */
#ifndef OMEGA_LANG_PARSE_H
#define OMEGA_LANG_PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "omega_types.h"
#include "omega_lex.h"

#define OMEGA_AST_MAX_NODES   128
#define OMEGA_AST_MAX_DEPTH   32
#define OMEGA_CLAUSE_MAX      64    /* matches OmegaContract.precondition[64] */

typedef enum {
    OAST_INT = 1,     /* integer literal (value)               */
    OAST_BOOL,        /* true / false (bval)                   */
    OAST_NAME,        /* reference to an existing binding      */
    OAST_BINARY,      /* lhs op rhs                            */
    OAST_ASCRIBE      /* (lhs : type)  -- explicit typing      */
} OmegaAstKind;

typedef enum {
    OSTMT_EMPTY = 0,  /* blank / comment-only line */
    OSTMT_EXPR,       /* expr [: type]             */
    OSTMT_LET,        /* let name : type = expr    */
    OSTMT_FN          /* fn name(p: T) -> T [requires ..] [ensures ..] { expr } */
} OmegaStmtKind;

/* A V0 surface type. tag is TYPE_UNSIGNED_INT (width 8/16/32/64) or TYPE_BOOL. */
typedef struct {
    TypeTag tag;
    uint16_t width;
} OmegaLangType;

typedef struct {
    OmegaAstKind kind;
    uint32_t col;
    OmegaTokKind op;            /* OAST_BINARY */
    int lhs, rhs;               /* child indices; OAST_ASCRIBE uses lhs */
    uint64_t value;             /* OAST_INT */
    bool bval;                  /* OAST_BOOL */
    char name[OMEGA_LANG_IDENT_MAX]; /* OAST_NAME */
    OmegaLangType type;         /* OAST_ASCRIBE */
} OmegaAstNode;

typedef struct {
    OmegaStmtKind stmt;
    uint32_t stmt_col;
    OmegaAstNode nodes[OMEGA_AST_MAX_NODES];
    size_t node_count;
    int root;                   /* expression root (let RHS, fn body, expr line) */

    /* let */
    char name[OMEGA_LANG_IDENT_MAX];   /* let binding name or fn name */
    OmegaLangType let_type;

    /* fn */
    char param[OMEGA_LANG_IDENT_MAX];
    OmegaLangType param_type;
    OmegaLangType ret_type;
    /* Canonical contract text: tokens re-joined with single spaces, comments
     * dropped, literals in decimal, the parameter renamed to `x`. "true" when
     * the clause is omitted. */
    char requires_text[OMEGA_CLAUSE_MAX];
    char ensures_text[OMEGA_CLAUSE_MAX];
} OmegaAst;

/* Parse a token stream (from omega_language_lex) into `ast`.
 * Returns 0 ok, -1 syntax error, -2 unsupported construct, -3 capacity
 * (too deep / too many nodes). `err` gets a one-line message with column. */
int omega_language_parse(const OmegaToken *toks, size_t count, OmegaAst *ast,
                         char *err, size_t n);

/* Convenience: lex + parse one line. Same return codes. */
int omega_language_parse_line(const char *src, OmegaAst *ast, char *err, size_t n);

/* "u64", "bool", ... */
const char *omega_language_type_text(const OmegaLangType *t);

/* True for words V0 reserves (keywords, type names, effect/authority/control
 * words). Such words cannot be binding names. */
bool omega_language_is_reserved(const char *word);

#endif /* OMEGA_LANG_PARSE_H */
