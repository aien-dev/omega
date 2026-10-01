/*
 * osc_parse.h -- OSC-1 recursive-descent parser producing the disposable
 * typed AST (docs/osc/OSC-1-DESIGN.md section 2). Fixed node arena, bounded
 * recursion depth; running out of either is a CAPACITY diagnostic.
 * The checker (osc_check.*) annotates the same nodes with types, resolved
 * symbols, folded constants and release lists; the lowerer (osc_lower.*)
 * reads them.
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 */
#ifndef OSC_PARSE_H
#define OSC_PARSE_H

#include <stdint.h>
#include "osc_diag.h"
#include "osc_ir.h"
#include "osc_lex.h"

#define OSC_AST_MAX_NODES 16384
#define OSC_AST_MAX_DEPTH 64       /* nesting of blocks + expressions */
#define OSC_AST_MAX_REL   16384    /* release-list entries (checker) */

typedef enum {
    ON_NONE = 0,
    /* top level */
    ON_FN,          /* tok = name; a = first ON_PARAM; b = body ON_BLOCK; ty = ret (VOID if none) */
    ON_PARAM,       /* tok = name; ty = scalar or REF type */
    /* statements */
    ON_BLOCK,       /* a = first stmt (linked by next); rel = releases at normal block end */
    ON_LET,         /* mut; tok; ty = scalar; a = expr */
    ON_LET_ALLOC,   /* tok; ty = REF OWN; a = init expr */
    ON_LET_MOVE,    /* tok; ty = REF OWN; a = ON_NAME source */
    ON_LET_BORROW,  /* mut (binding reassignable); tok; ty = REF SHARED/MUT; a = ON_BORROW */
    ON_ASSIGN,      /* tok = target; a = expr or ON_BORROW */
    ON_STORE,       /* tok = array; a = index; b = value */
    ON_IF,          /* a = cond; b = then block; c = else (ON_BLOCK or ON_IF) or -1 */
    ON_WHILE,       /* a = cond; b = body block; ival = bound; flag = 1 if a bound was given */
    ON_FOR,         /* tok = loop var; b = body block; lo, hi = literal ends (half-open) */
    ON_RETURN,      /* a = expr / ON_BORROW / -1; rel = releases before RET */
    ON_CALLSTMT,    /* a = ON_CALL */
    /* expressions */
    ON_INT,         /* ival */
    ON_BOOL,        /* ival 0/1 */
    ON_NAME,        /* tok */
    ON_INDEX,       /* tok = array; a = index */
    ON_CALL,        /* tok = callee; a = first arg (linked by next) */
    ON_BORROW,      /* tok = name; mut */
    ON_BIN,         /* op = OscTokKind of the operator (incl. && ||); a, b */
    ON_UN,          /* op = OT_MINUS / OT_TILDE / OT_BANG; a */
    ON_CAST,        /* a; ty = target scalar */
    /* OSC-2 structs (docs/osc/OSC-2-DESIGN.md section 2). A struct literal is an
     * ON_LET_ALLOC whose ty is a struct ref (ty.sid != 0) and whose a is the
     * first ON_FINIT (linked by next, source order). */
    ON_FINIT,       /* tok = field name; a = value expr; hi = field index;
                     * flag = 1 for an array field "[e; N]" (ival = N) */
    ON_FIELD,       /* tok = struct binding; lo = field name token; a = index or -1;
                     * checker: sym = binding, hi = field index */
    ON_FSTORE       /* tok = struct binding; lo = field name token; a = index or -1;
                     * b = value; checker: sym = binding, hi = field index */
} OscNodeKind;

typedef struct {
    uint16_t kind;      /* OscNodeKind */
    uint16_t op;        /* operator token kind */
    uint32_t line, col;
    int32_t a, b, c;    /* children (-1 = none) */
    int32_t next;       /* sibling link (-1 = end) */
    uint32_t tok;       /* name token index */
    uint64_t ival;
    int64_t lo, hi;     /* ON_FOR literal ends */
    OscType ty;         /* declared type (decls, params, casts, fn ret) / result type (exprs, checker) */
    uint8_t mut;
    uint8_t flag;
    /* checker annotations */
    uint8_t is_const;   /* expression folded to cval (already canonical for ty) */
    uint64_t cval;
    int32_t sym;        /* resolved symbol (names, decls, params) or callee function index (calls) */
    int32_t sym2;       /* ON_LET_MOVE / ON_ASSIGN-borrow / ON_BORROW source symbol */
    uint32_t rel_start; /* release list: indices into OscAst.rel */
    uint16_t rel_count;
} OscNode;

typedef struct {
    const char *src;
    const OscToken *toks;
    uint32_t ntok;
    OscNode nodes[OSC_AST_MAX_NODES];
    uint32_t nnodes;
    int32_t fns[OSC_MAX_FUNCS];
    uint32_t nfns;
    char req[OSC_MAX_FUNCS][OSC_CLAUSE_MAX];
    char ens[OSC_MAX_FUNCS][OSC_CLAUSE_MAX];
    /* OSC-2 contracts (docs/osc/OSC-2-DESIGN.md section 1). Parser: clause
     * expression nodes (-1 = no clause). Checker: res_sym = symbol id of the
     * `result` pseudo binding of a non-void function with an ensures clause
     * (-1 otherwise); *_elide = 1 when the clause folds to true on its own and
     * no runtime check is emitted. */
    int32_t reqn[OSC_MAX_FUNCS];
    int32_t ensn[OSC_MAX_FUNCS];
    int32_t res_sym[OSC_MAX_FUNCS];
    uint8_t req_elide[OSC_MAX_FUNCS];
    uint8_t ens_elide[OSC_MAX_FUNCS];
    /* filled by the checker: symbol ids of owners to release, in emission order */
    int16_t rel[OSC_AST_MAX_REL];
    uint32_t nrel;
    /* OSC-2 structs: declarations in source order (parser); copied into the
     * OscUnit by the lowerer. struct_line = line of the declaration. */
    OscStruct structs[OSC_MAX_STRUCTS];
    uint32_t struct_line[OSC_MAX_STRUCTS];
    uint8_t nstructs;
} OscAst;

/* Parse the token stream into ast (ast->src/toks/ntok must be set). 0 ok, -1
 * with *d filled (SYNTAX, UNSUPPORTED, CAPACITY, UNBOUNDED_LOOP for a while
 * without bound). */
int osc_parse(OscAst *ast, OscDiag *d);

/* Copy the spelling of node->tok. */
void osc_node_name(const OscAst *ast, const OscNode *n, char *buf, size_t cap);

#endif /* OSC_PARSE_H */
