/*
 * osc_parse.c -- OSC-1 recursive-descent parser. See osc_parse.h and
 * docs/osc/OSC-1-DESIGN.md section 2 (grammar).
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 */
#include "osc_parse.h"

#include <stdio.h>
#include <string.h>

#define LOOP_BOUND_MAX 1000000

typedef struct {
    OscAst *ast;
    OscDiag *d;
    uint32_t pos;
    int depth;
} P;

static const OscToken *cur(P *p) { return &p->ast->toks[p->pos]; }
static int peek_kind(P *p, uint32_t k) { uint32_t i = p->pos + k; return i < p->ast->ntok ? p->ast->toks[i].kind : OT_EOF; }
static int at(P *p, int k) { return cur(p)->kind == k; }
static void adv(P *p) { if (cur(p)->kind != OT_EOF) p->pos++; }

void osc_node_name(const OscAst *ast, const OscNode *n, char *buf, size_t cap)
{
    osc_tok_text(ast->src, &ast->toks[n->tok], buf, cap);
}

static int syntax(P *p, const char *what)
{
    const OscToken *t = cur(p);
    char buf[64];
    if (t->kind == OT_EOF) snprintf(buf, sizeof buf, "end of input");
    else osc_tok_text(p->ast->src, t, buf, sizeof buf);
    if (t->kind == OT_RESERVED)
        osc_diag_set(p->d, OSC_DIAG_SYNTAX, t->line, t->col, buf, 0, NULL, "reserved word",
                     "'%s' is reserved (destruction and effects have no surface syntax)", buf);
    else
        osc_diag_set(p->d, OSC_DIAG_SYNTAX, t->line, t->col, buf, 0, NULL, what, "expected %s, found '%s'", what, buf);
    return -1;
}

static int expect(P *p, int k)
{
    if (!at(p, k)) return syntax(p, osc_tok_kind_name(k));
    adv(p);
    return 0;
}

static int new_node(P *p, int kind, const OscToken *t)
{
    OscAst *a = p->ast;
    if (a->nnodes >= OSC_AST_MAX_NODES) {
        osc_diag_set(p->d, OSC_DIAG_CAPACITY, t->line, t->col, "unit", 0, NULL, "AST node capacity",
                     "more than %d AST nodes", OSC_AST_MAX_NODES);
        return -1;
    }
    int i = (int)a->nnodes++;
    OscNode *n = &a->nodes[i];
    memset(n, 0, sizeof *n);
    n->kind = (uint16_t)kind;
    n->line = t->line;
    n->col = t->col;
    n->a = n->b = n->c = n->next = -1;
    n->sym = n->sym2 = -1;
    n->tok = (uint32_t)(t - a->toks);
    return i;
}
#define N(i) (&p->ast->nodes[(i)])

static int enter(P *p)
{
    if (++p->depth > OSC_AST_MAX_DEPTH) {
        const OscToken *t = cur(p);
        osc_diag_set(p->d, OSC_DIAG_CAPACITY, t->line, t->col, "unit", 0, NULL, "nesting depth",
                     "nesting deeper than %d", OSC_AST_MAX_DEPTH);
        return -1;
    }
    return 0;
}
static void leave(P *p) { p->depth--; }

static int scalar_of(int k)
{
    switch (k) {
    case OT_U8: return OSC_T_U8; case OT_U16: return OSC_T_U16; case OT_U32: return OSC_T_U32;
    case OT_U64: return OSC_T_U64; case OT_I8: return OSC_T_I8; case OT_I16: return OSC_T_I16;
    case OT_I32: return OSC_T_I32; case OT_I64: return OSC_T_I64; case OT_BOOL: return OSC_T_BOOL;
    default: return 0;
    }
}

static int parse_scalar(P *p, OscType *ty)
{
    int s = scalar_of(cur(p)->kind);
    if (at(p, OT_BYTES) || at(p, OT_CELLS)) {
        const OscToken *t = cur(p);
        osc_diag_set(p->d, OSC_DIAG_SLICE_ESCAPE, t->line, t->col, at(p, OT_BYTES) ? "bytes" : "cells", 0, NULL,
                     "slice type outside a parameter", "'%s' is a parameter type only: a slice is never a local, field, element or result",
                     at(p, OT_BYTES) ? "bytes" : "cells");
        return -1;
    }
    if (!s) return syntax(p, "a type (u8 u16 u32 u64 i8 i16 i32 i64 bool)");
    memset(ty, 0, sizeof *ty);
    ty->s = (OscScalar)s;
    adv(p);
    return 0;
}

/* "[" type ";" INT "]" with ref kind already consumed */
static int parse_arr_body(P *p, OscType *ty, OscRefKind rk)
{
    OscType el;
    if (expect(p, OT_LBRACK)) return -1;
    if (parse_scalar(p, &el)) return -1;
    if (expect(p, OT_SEMI)) return -1;
    if (!at(p, OT_INT)) return syntax(p, "array length INT");
    const OscToken *lt = cur(p);
    if (lt->ival == 0) {
        osc_diag_set(p->d, OSC_DIAG_UNSUPPORTED, lt->line, lt->col, "array", 0, NULL, "array length 0",
                     "array length must be 1..%d", OSC_MAX_ARRAY_LEN);
        return -1;
    }
    if (lt->ival > OSC_MAX_ARRAY_LEN) {
        osc_diag_set(p->d, OSC_DIAG_CAPACITY, lt->line, lt->col, "array", 0, NULL, "array length",
                     "array length %llu > %d", (unsigned long long)lt->ival, OSC_MAX_ARRAY_LEN);
        return -1;
    }
    adv(p);
    if (expect(p, OT_RBRACK)) return -1;
    memset(ty, 0, sizeof *ty);
    ty->s = OSC_T_REF;
    ty->ref = rk;
    ty->elem = el.s;
    ty->len = (uint16_t)lt->ival;
    return 0;
}

static int parse_expr(P *p);

/* ---- OSC-2 structs (docs/osc/OSC-2-DESIGN.md section 2) ---------------- */
static int tok_eq(P *p, const OscToken *t, const char *s)
{
    size_t n = strlen(s);
    return t->kind == OT_NAME && t->len == n && memcmp(p->ast->src + t->off, s, n) == 0;
}

/* index of the struct named by token t, -1 if none is declared (yet) */
static int find_struct(P *p, const OscToken *t)
{
    for (int k = 0; k < p->ast->nstructs; k++)
        if (tok_eq(p, t, p->ast->structs[k].name)) return k;
    return -1;
}

/* index of field `t` in struct k, -1 if none */
static int find_field(P *p, int k, const OscToken *t)
{
    const OscStruct *s = &p->ast->structs[k];
    for (int f = 0; f < s->nfields; f++)
        if (tok_eq(p, t, s->fields[f].name)) return f;
    return -1;
}

static int undefined_type(P *p, const OscToken *t)
{
    char nm[64];
    osc_tok_text(p->ast->src, t, nm, sizeof nm);
    osc_diag_set(p->d, OSC_DIAG_UNDEFINED_TYPE, t->line, t->col, nm, 0, NULL, "undefined struct type",
                 "struct type '%s' is not declared (structs are declared before use)", nm);
    return -1;
}

/* a struct named where a value type is expected (let x: S, p: S, -> S) */
static int struct_by_value(P *p, const OscToken *t, const char *transition)
{
    char nm[64];
    osc_tok_text(p->ast->src, t, nm, sizeof nm);
    osc_diag_set(p->d, OSC_DIAG_UNSUPPORTED, t->line, t->col, nm, p->ast->struct_line[find_struct(p, t)], NULL,
                 transition, "struct '%s' is used through 'own', '&' or '&mut' only; it is never a value "
                 "or a return type", nm);
    return -1;
}

/* NAME with the ref kind already consumed: a reference to a declared struct */
static int parse_struct_ref(P *p, OscType *ty, OscRefKind rk)
{
    const OscToken *t = cur(p);
    int k = find_struct(p, t);
    if (k < 0) return undefined_type(p, t);
    adv(p);
    memset(ty, 0, sizeof *ty);
    ty->s = OSC_T_REF;
    ty->ref = rk;
    ty->elem = OSC_T_VOID;
    ty->len = p->ast->structs[k].ncells;
    ty->sid = (uint8_t)(k + 1);
    return 0;
}

/* "[" ... (array) or NAME (struct), ref kind already consumed */
static int parse_ref_body(P *p, OscType *ty, OscRefKind rk)
{
    if (at(p, OT_NAME)) return parse_struct_ref(p, ty, rk);
    return parse_arr_body(p, ty, rk);
}

/* OSC-3 item 2: pools and handles are function-local (never parameters or
 * results) */
static int local_only(P *p)
{
    const OscToken *t = cur(p);
    osc_diag_set(p->d, OSC_DIAG_UNSUPPORTED, t->line, t->col, at(p, OT_HANDLE) ? "handle" : "pool", 0, NULL,
                 "handle crosses a function boundary",
                 "pools and handles are function-local: no handle or pool parameters or return types");
    return -1;
}

/* ptype: scalar | own[..] | &[..] | &mut[..] | own S | &S | &mut S */
static int parse_ptype(P *p, OscType *ty)
{
    if (at(p, OT_OWN)) { adv(p); return parse_ref_body(p, ty, OSC_REF_OWN); }
    if (at(p, OT_AMP)) {
        adv(p);
        if (at(p, OT_MUT)) { adv(p); return parse_ref_body(p, ty, OSC_REF_MUT); }
        return parse_ref_body(p, ty, OSC_REF_SHARED);
    }
    if (at(p, OT_NAME) && find_struct(p, cur(p)) >= 0) return struct_by_value(p, cur(p), "struct by value");
    if (at(p, OT_BYTES) || at(p, OT_CELLS)) { /* external byte slice: pointer + length registers */
        memset(ty, 0, sizeof *ty);
        ty->s = at(p, OT_BYTES) ? OSC_T_BYTES : OSC_T_CELLS;
        adv(p);
        return 0;
    }
    if (at(p, OT_HANDLE) || at(p, OT_POOL)) return local_only(p);
    return parse_scalar(p, ty);
}

/* struct_decl = "struct" NAME "{" field { "," field } [","] "}" ;
 * field = NAME ":" ( scalar | "[" scalar ";" INT "]" ) */
static int parse_struct(P *p)
{
    OscAst *a = p->ast;
    const OscToken *st = cur(p);
    adv(p);
    if (!at(p, OT_NAME)) return syntax(p, "a struct name");
    const OscToken *nt = cur(p);
    char sname[64];
    osc_tok_text(a->src, nt, sname, sizeof sname);
    int prev = find_struct(p, nt);
    if (prev >= 0) {
        osc_diag_set(p->d, OSC_DIAG_REDEFINED_NAME, nt->line, nt->col, sname, a->struct_line[prev], NULL,
                     "struct declared twice", "struct '%s' is already declared", sname);
        return -1;
    }
    if (a->nstructs >= OSC_MAX_STRUCTS) {
        osc_diag_set(p->d, OSC_DIAG_CAPACITY, nt->line, nt->col, sname, 0, NULL, "struct capacity",
                     "more than %d structs", OSC_MAX_STRUCTS);
        return -1;
    }
    adv(p);
    OscStruct *s = &a->structs[a->nstructs];
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", sname);
    if (expect(p, OT_LBRACE)) return -1;
    if (at(p, OT_RBRACE)) {
        osc_diag_set(p->d, OSC_DIAG_UNSUPPORTED, nt->line, nt->col, sname, 0, NULL, "empty struct",
                     "struct '%s' declares no fields (1..%d required)", sname, OSC_MAX_FIELDS);
        return -1;
    }
    unsigned cells = 0;
    while (!at(p, OT_RBRACE)) {
        const OscToken *ft = cur(p);
        if (!at(p, OT_NAME)) return syntax(p, "a field name");
        char fname[64];
        osc_tok_text(a->src, ft, fname, sizeof fname);
        for (int g = 0; g < s->nfields; g++)
            if (!strcmp(s->fields[g].name, fname)) {
                osc_diag_set(p->d, OSC_DIAG_DUPLICATE_FIELD, ft->line, ft->col, fname, nt->line, sname,
                             "field declared twice", "struct '%s' declares field '%s' twice", sname, fname);
                return -1;
            }
        if (s->nfields >= OSC_MAX_FIELDS) {
            osc_diag_set(p->d, OSC_DIAG_CAPACITY, ft->line, ft->col, sname, nt->line, fname, "field capacity",
                         "struct '%s' has more than %d fields", sname, OSC_MAX_FIELDS);
            return -1;
        }
        adv(p);
        if (expect(p, OT_COLON)) return -1;
        OscField *fd = &s->fields[s->nfields];
        snprintf(fd->name, sizeof fd->name, "%s", fname);
        const OscToken *tt = cur(p);
        if (at(p, OT_OWN) || at(p, OT_AMP) || at(p, OT_NAME)) {
            uint32_t q = p->pos;
            while (q < a->ntok && (a->toks[q].kind == OT_OWN || a->toks[q].kind == OT_AMP || a->toks[q].kind == OT_MUT)) q++;
            const OscToken *rt = &a->toks[q];
            if (tok_eq(p, rt, sname)) {
                osc_diag_set(p->d, OSC_DIAG_RECURSIVE_STRUCT, rt->line, rt->col, sname, nt->line, fname,
                             "struct field of its own type", "field '%s' of struct '%s' names '%s' itself "
                             "(no recursive or self-referential structs)", fname, sname, sname);
                return -1;
            }
            if (rt->kind == OT_NAME && find_struct(p, rt) < 0 && tt->kind == OT_NAME) return undefined_type(p, rt);
            osc_diag_set(p->d, OSC_DIAG_UNSUPPORTED, tt->line, tt->col, fname, nt->line, sname,
                         tt->kind == OT_NAME ? "struct-typed field" : "owner or borrow field",
                         "field '%s': a field is an integer, bool or fixed array [T; N]", fname);
            return -1;
        }
        if (at(p, OT_LBRACK)) {
            OscType at_;
            if (parse_arr_body(p, &at_, OSC_REF_OWN)) return -1;
            fd->s = at_.elem;
            fd->alen = at_.len;
        } else {
            OscType sc;
            if (parse_scalar(p, &sc)) return -1;
            fd->s = sc.s;
        }
        fd->off = (uint16_t)cells;
        cells += fd->alen ? fd->alen : 1;
        if (cells > OSC_MAX_ARRAY_LEN) {
            osc_diag_set(p->d, OSC_DIAG_CAPACITY, tt->line, tt->col, sname, nt->line, fname, "struct size",
                         "struct '%s' needs more than %d cells", sname, OSC_MAX_ARRAY_LEN);
            return -1;
        }
        s->nfields++;
        if (at(p, OT_COMMA)) { adv(p); continue; }
        if (!at(p, OT_RBRACE)) return syntax(p, "',' or '}' after a field");
    }
    adv(p);
    s->ncells = (uint16_t)cells;
    a->struct_line[a->nstructs++] = st->line;
    return 0;
}

/* struct literal  S "{" NAME ":" ( expr | "[" expr ";" INT "]" ) { "," ... } [","] "}"
 * as the initialiser of `let x: own S`; cur is S. Fills n->a with ON_FINIT
 * nodes. Every field exactly once: UNKNOWN_FIELD / DUPLICATE_FIELD / MISSING_FIELD. */
static int parse_struct_lit(P *p, int n)
{
    OscAst *a = p->ast;
    const OscToken *lt = cur(p);
    int k = find_struct(p, lt);
    char bname[64];
    osc_node_name(a, N(n), bname, sizeof bname);
    if (k < 0) return undefined_type(p, lt);
    const OscStruct *s = &a->structs[k];
    if (k + 1 != N(n)->ty.sid) {
        osc_diag_set(p->d, OSC_DIAG_TYPE_MISMATCH, lt->line, lt->col, bname, N(n)->line,
                     s->name, "struct literal of another type", "'%s' is declared own %s, initialised with a %s literal",
                     bname, a->structs[N(n)->ty.sid - 1].name, s->name);
        return -1;
    }
    adv(p);
    if (expect(p, OT_LBRACE)) return -1;
    uint32_t seen = 0;
    int last = -1;
    while (!at(p, OT_RBRACE)) {
        const OscToken *ft = cur(p);
        if (!at(p, OT_NAME)) return syntax(p, "a field name");
        char fname[64];
        osc_tok_text(a->src, ft, fname, sizeof fname);
        int f = find_field(p, k, ft);
        if (f < 0) {
            osc_diag_set(p->d, OSC_DIAG_UNKNOWN_FIELD, ft->line, ft->col, fname, a->struct_line[k], s->name,
                         "initialise unknown field", "struct '%s' has no field '%s'", s->name, fname);
            return -1;
        }
        if (seen >> f & 1) {
            osc_diag_set(p->d, OSC_DIAG_DUPLICATE_FIELD, ft->line, ft->col, fname, lt->line, s->name,
                         "field initialised twice", "field '%s' of '%s' is initialised twice", fname, s->name);
            return -1;
        }
        seen |= 1u << f;
        int fi = new_node(p, ON_FINIT, ft);
        if (fi < 0) return -1;
        N(fi)->hi = f;
        adv(p);
        if (expect(p, OT_COLON)) return -1;
        const OscField *fd = &s->fields[f];
        if (fd->alen) {
            if (!at(p, OT_LBRACK)) {
                const OscToken *t = cur(p);
                osc_diag_set(p->d, OSC_DIAG_TYPE_MISMATCH, t->line, t->col, fname, a->struct_line[k], s->name,
                             "array field without [e; N]", "array field '%s' is initialised with '[value; %u]'",
                             fname, fd->alen);
                return -1;
            }
            adv(p);
            int e = parse_expr(p);
            if (e < 0) return -1;
            N(fi)->a = e;
            if (expect(p, OT_SEMI)) return -1;
            if (!at(p, OT_INT)) return syntax(p, "array length INT");
            const OscToken *nt = cur(p);
            if (nt->ival != fd->alen) {
                osc_diag_set(p->d, OSC_DIAG_TYPE_MISMATCH, nt->line, nt->col, fname, a->struct_line[k], s->name,
                             "array field length mismatch", "array field '%s' has length %u, initialiser gives %llu",
                             fname, fd->alen, (unsigned long long)nt->ival);
                return -1;
            }
            N(fi)->flag = 1;
            N(fi)->ival = nt->ival;
            adv(p);
            if (expect(p, OT_RBRACK)) return -1;
        } else {
            int e = parse_expr(p);
            if (e < 0) return -1;
            N(fi)->a = e;
        }
        if (last < 0) N(n)->a = fi; else N(last)->next = fi;
        last = fi;
        if (at(p, OT_COMMA)) { adv(p); continue; }
        if (!at(p, OT_RBRACE)) return syntax(p, "',' or '}' in a struct literal");
    }
    for (int f = 0; f < s->nfields; f++)
        if (!(seen >> f & 1)) {
            const OscToken *t = cur(p);
            osc_diag_set(p->d, OSC_DIAG_MISSING_FIELD, t->line, t->col, s->fields[f].name, lt->line, s->name,
                         "field not initialised", "struct literal of '%s' does not initialise field '%s'",
                         s->name, s->fields[f].name);
            return -1;
        }
    adv(p);
    return 0;
}

/* NAME "." NAME [ "[" expr "]" ] ; cur is the first NAME. kind = ON_FIELD / ON_FSTORE */
static int parse_field_ref(P *p, int kind)
{
    int n = new_node(p, kind, cur(p));
    if (n < 0) return -1;
    adv(p);
    adv(p);  /* '.' */
    if (!at(p, OT_NAME)) return syntax(p, "a field name after '.'");
    N(n)->lo = p->pos;
    adv(p);
    if (at(p, OT_LBRACK)) {
        adv(p);
        int ix = parse_expr(p);
        if (ix < 0) return -1;
        N(n)->a = ix;
        if (expect(p, OT_RBRACK)) return -1;
    }
    return n;
}

static int parse_expr(P *p);
static int parse_block(P *p);

static int parse_borrow(P *p)
{
    int n = new_node(p, ON_BORROW, cur(p));
    if (n < 0) return -1;
    if (expect(p, OT_AMP)) return -1;
    if (at(p, OT_MUT)) { N(n)->mut = 1; adv(p); }
    if (!at(p, OT_NAME)) return syntax(p, "a name after '&'");
    N(n)->tok = p->pos;
    N(n)->line = cur(p)->line;
    N(n)->col = cur(p)->col;
    adv(p);
    return n;
}

/* call: NAME "(" [arg {"," arg}] ")" ; cur is NAME */
static int parse_call(P *p)
{
    int n = new_node(p, ON_CALL, cur(p));
    if (n < 0) return -1;
    if (enter(p)) return -1;
    adv(p);
    if (expect(p, OT_LPAREN)) return -1;
    int last = -1;
    if (!at(p, OT_RPAREN)) {
        for (;;) {
            int a = at(p, OT_AMP) ? parse_borrow(p) : parse_expr(p);
            if (a < 0) return -1;
            if (last < 0) N(n)->a = a; else N(last)->next = a;
            last = a;
            if (at(p, OT_COMMA)) { adv(p); continue; }
            break;
        }
    }
    if (expect(p, OT_RPAREN)) return -1;
    leave(p);
    return n;
}

static int parse_primary(P *p)
{
    const OscToken *t = cur(p);
    int n;
    switch (t->kind) {
    case OT_INT:
        n = new_node(p, ON_INT, t);
        if (n < 0) return -1;
        N(n)->ival = t->ival;
        adv(p);
        return n;
    case OT_TRUE: case OT_FALSE:
        n = new_node(p, ON_BOOL, t);
        if (n < 0) return -1;
        N(n)->ival = t->kind == OT_TRUE;
        adv(p);
        return n;
    case OT_NAME:
        if (peek_kind(p, 1) == OT_LPAREN) return parse_call(p);
        if (peek_kind(p, 1) == OT_DOT && peek_kind(p, 2) == OT_ALLOC) { /* OSC-3 item 2: P.alloc(e) */
            n = new_node(p, ON_PALLOC, t);
            if (n < 0) return -1;
            adv(p); adv(p); adv(p);
            if (expect(p, OT_LPAREN)) return -1;
            if (enter(p)) return -1;
            int e0 = parse_expr(p);
            leave(p);
            if (e0 < 0) return -1;
            N(n)->a = e0;
            if (expect(p, OT_RPAREN)) return -1;
            return n;
        }
        if (peek_kind(p, 1) == OT_DOT) return parse_field_ref(p, ON_FIELD);
        if (peek_kind(p, 1) == OT_LBRACK) {
            n = new_node(p, ON_INDEX, t);
            if (n < 0) return -1;
            adv(p); adv(p);
            int ix = parse_expr(p);
            if (ix < 0) return -1;
            N(n)->a = ix;
            if (expect(p, OT_RBRACK)) return -1;
            return n;
        }
        n = new_node(p, ON_NAME, t);
        if (n < 0) return -1;
        adv(p);
        return n;
    case OT_LPAREN: {
        if (enter(p)) return -1;
        adv(p);
        int e = parse_expr(p);
        if (e < 0) return -1;
        if (expect(p, OT_RPAREN)) return -1;
        leave(p);
        return e;
    }
    case OT_AMP:
        osc_diag_set(p->d, OSC_DIAG_UNSUPPORTED, t->line, t->col, "&", 0, NULL, "borrow in expression",
                     "a borrow may only be a call argument or a borrow binding's value");
        return -1;
    default:
        return syntax(p, "an expression");
    }
}

static int parse_postfix(P *p)
{
    int e = parse_primary(p);
    if (e < 0) return -1;
    if (at(p, OT_AS)) {
        int n = new_node(p, ON_CAST, cur(p));
        if (n < 0) return -1;
        adv(p);
        if (parse_scalar(p, &N(n)->ty)) return -1;
        N(n)->a = e;
        N(n)->line = N(e)->line;
        N(n)->col = N(e)->col;
        if (at(p, OT_AS)) return syntax(p, "';' (chained 'as' needs parentheses)");
        return n;
    }
    return e;
}

static int parse_unary(P *p)
{
    int k = cur(p)->kind;
    if (k == OT_MINUS || k == OT_TILDE || k == OT_BANG) {
        if (enter(p)) return -1;
        int n = new_node(p, ON_UN, cur(p));
        if (n < 0) return -1;
        adv(p);
        int a = parse_unary(p);
        leave(p);
        if (a < 0) return -1;
        N(n)->op = (uint16_t)k;
        N(n)->a = a;
        return n;
    }
    return parse_postfix(p);
}

/* binary precedence levels, lowest first */
static const int levels[][3] = {
    {OT_OROR, 0, 0}, {OT_ANDAND, 0, 0}, {0, 0, 0} /* cmp */, {OT_PIPE, 0, 0}, {OT_CARET, 0, 0},
    {OT_AMP, 0, 0}, {OT_SHL, OT_SHR, 0}, {OT_PLUS, OT_MINUS, 0}, {OT_STAR, OT_SLASH, OT_PERCENT},
};
#define NLEVELS ((int)(sizeof levels / sizeof levels[0]))
#define CMP_LEVEL 2

static int is_cmp(int k) { return k == OT_EQ || k == OT_NE || k == OT_LT || k == OT_LE || k == OT_GT || k == OT_GE; }

static int parse_level(P *p, int lv)
{
    if (lv >= NLEVELS) return parse_unary(p);
    int l = parse_level(p, lv + 1);
    if (l < 0) return -1;
    if (lv == CMP_LEVEL) {
        if (is_cmp(cur(p)->kind)) {
            int n = new_node(p, ON_BIN, cur(p));
            if (n < 0) return -1;
            N(n)->op = cur(p)->kind;
            adv(p);
            int r = parse_level(p, lv + 1);
            if (r < 0) return -1;
            N(n)->a = l;
            N(n)->b = r;
            l = n;
            if (is_cmp(cur(p)->kind)) return syntax(p, "no second comparison (comparisons do not chain)");
        }
        return l;
    }
    for (;;) {
        int k = cur(p)->kind;
        if (k == 0 || !(k == levels[lv][0] || k == levels[lv][1] || k == levels[lv][2])) break;
        int n = new_node(p, ON_BIN, cur(p));
        if (n < 0) return -1;
        N(n)->op = (uint16_t)k;
        adv(p);
        int r = parse_level(p, lv + 1);
        if (r < 0) return -1;
        N(n)->a = l;
        N(n)->b = r;
        l = n;
    }
    return l;
}

static int parse_expr(P *p) { return parse_level(p, 0); }

static int parse_name_tok(P *p, int n)
{
    if (!at(p, OT_NAME)) return syntax(p, "a name");
    N(n)->tok = p->pos;
    adv(p);
    return 0;
}

static int parse_int_signed(P *p, int64_t *out)
{
    int neg = 0;
    if (at(p, OT_MINUS)) { neg = 1; adv(p); }
    if (!at(p, OT_INT)) return syntax(p, "an integer literal");
    const OscToken *t = cur(p);
    if (t->ival > (uint64_t)INT64_MAX + (uint64_t)neg) {
        osc_diag_set(p->d, OSC_DIAG_OVERFLOW_UNSAFE, t->line, t->col, "for", 0, NULL, "literal does not fit i64",
                     "for-loop end does not fit i64");
        return -1;
    }
    *out = neg ? (int64_t)(0 - t->ival) : (int64_t)t->ival;
    adv(p);
    return 0;
}

static int parse_if(P *p);

static int parse_stmt(P *p)
{
    const OscToken *t = cur(p);
    int n;
    switch (t->kind) {
    case OT_LET: {
        adv(p);
        int mut = 0;
        if (at(p, OT_MUT)) { mut = 1; adv(p); }
        n = new_node(p, ON_LET, t);
        if (n < 0) return -1;
        if (parse_name_tok(p, n)) return -1;
        if (expect(p, OT_COLON)) return -1;
        if (at(p, OT_HANDLE)) { /* OSC-3 item 2: let [mut] h: handle P = P.alloc(e) | h2; */
            N(n)->kind = ON_LET_HANDLE;
            N(n)->mut = (uint8_t)mut;
            adv(p);
            if (!at(p, OT_NAME)) return syntax(p, "a pool name after 'handle'");
            int pn = new_node(p, ON_NAME, cur(p));
            if (pn < 0) return -1;
            adv(p);
            N(n)->c = pn;
            if (expect(p, OT_ASSIGN)) return -1;
            int e = parse_expr(p);
            if (e < 0) return -1;
            N(n)->a = e;
            if (expect(p, OT_SEMI)) return -1;
            return n;
        }
        if (at(p, OT_OWN)) {
            if (mut) {
                const OscToken *mt = &p->ast->toks[N(n)->tok - 1];
                osc_diag_set(p->d, OSC_DIAG_SYNTAX, mt->line, mt->col, "mut", 0, NULL, "let mut owner",
                             "an owner binding cannot be 'mut' (owners are never reassigned)");
                return -1;
            }
            adv(p);
            if (parse_ref_body(p, &N(n)->ty, OSC_REF_OWN)) return -1;
            int in_arena = -1; /* OSC-2 arenas: "in NAME" */
            if (at(p, OT_IN)) {
                adv(p);
                if (!at(p, OT_NAME)) return syntax(p, "an arena name after 'in'");
                in_arena = new_node(p, ON_NAME, cur(p));
                if (in_arena < 0) return -1;
                adv(p);
            }
            if (expect(p, OT_ASSIGN)) return -1;
            if (N(n)->ty.sid && at(p, OT_NAME) && peek_kind(p, 1) == OT_LBRACE) {
                N(n)->kind = ON_LET_ALLOC;
                if (parse_struct_lit(p, n)) return -1;
            } else if (N(n)->ty.sid && at(p, OT_ALLOC)) {
                const OscToken *at_ = cur(p);
                char nm[64];
                osc_node_name(p->ast, N(n), nm, sizeof nm);
                osc_diag_set(p->d, OSC_DIAG_UNSUPPORTED, at_->line, at_->col, nm, 0,
                             p->ast->structs[N(n)->ty.sid - 1].name, "alloc of a struct",
                             "a struct owner is created by a struct literal, not alloc");
                return -1;
            } else if (at(p, OT_ALLOC)) {
                adv(p);
                N(n)->kind = ON_LET_ALLOC;
                if (expect(p, OT_LPAREN)) return -1;
                int e = parse_expr(p);
                if (e < 0) return -1;
                N(n)->a = e;
                if (expect(p, OT_RPAREN)) return -1;
            } else if (at(p, OT_NAME)) {
                N(n)->kind = ON_LET_MOVE;
                int s = new_node(p, ON_NAME, cur(p));
                if (s < 0) return -1;
                adv(p);
                N(n)->a = s;
            } else {
                return syntax(p, "'alloc' or an owner name");
            }
            N(n)->c = in_arena;
        } else if (at(p, OT_AMP)) {
            N(n)->kind = ON_LET_BORROW;
            N(n)->mut = (uint8_t)mut;
            adv(p);
            OscRefKind rk = OSC_REF_SHARED;
            if (at(p, OT_MUT)) { rk = OSC_REF_MUT; adv(p); }
            if (parse_ref_body(p, &N(n)->ty, rk)) return -1;
            if (expect(p, OT_ASSIGN)) return -1;
            if (!at(p, OT_AMP)) return syntax(p, "a borrow '&name' or '&mut name'");
            int b = parse_borrow(p);
            if (b < 0) return -1;
            N(n)->a = b;
        } else {
            N(n)->mut = (uint8_t)mut;
            if (at(p, OT_NAME) && find_struct(p, cur(p)) >= 0) return struct_by_value(p, cur(p), "struct by value");
            if (parse_scalar(p, &N(n)->ty)) return -1;
            if (expect(p, OT_ASSIGN)) return -1;
            int e = parse_expr(p);
            if (e < 0) return -1;
            N(n)->a = e;
        }
        if (expect(p, OT_SEMI)) return -1;
        return n;
    }
    case OT_NAME: {
        int k1 = peek_kind(p, 1);
        if (k1 == OT_LPAREN) {
            n = new_node(p, ON_CALLSTMT, t);
            if (n < 0) return -1;
            int c = parse_call(p);
            if (c < 0) return -1;
            N(n)->a = c;
            if (expect(p, OT_SEMI)) return -1;
            return n;
        }
        if (k1 == OT_DOT && peek_kind(p, 2) == OT_RESERVED && p->ast->toks[p->pos + 2].len == 4 &&
            memcmp(p->ast->src + p->ast->toks[p->pos + 2].off, "free", 4) == 0) { /* OSC-3 item 2: P.free(h); */
            n = new_node(p, ON_PFREE, t);
            if (n < 0) return -1;
            adv(p); adv(p); adv(p);
            if (expect(p, OT_LPAREN)) return -1;
            int h = parse_expr(p);
            if (h < 0) return -1;
            N(n)->a = h;
            if (expect(p, OT_RPAREN)) return -1;
            if (expect(p, OT_SEMI)) return -1;
            return n;
        }
        if (k1 == OT_DOT) {
            n = parse_field_ref(p, ON_FSTORE);
            if (n < 0) return -1;
            if (expect(p, OT_ASSIGN)) return -1;
            int v = parse_expr(p);
            if (v < 0) return -1;
            N(n)->b = v;
            if (expect(p, OT_SEMI)) return -1;
            return n;
        }
        if (k1 == OT_ASSIGN) {
            n = new_node(p, ON_ASSIGN, t);
            if (n < 0) return -1;
            adv(p); adv(p);
            int e = at(p, OT_AMP) ? parse_borrow(p) : parse_expr(p);
            if (e < 0) return -1;
            N(n)->a = e;
            if (expect(p, OT_SEMI)) return -1;
            return n;
        }
        if (k1 == OT_LBRACK) {
            n = new_node(p, ON_STORE, t);
            if (n < 0) return -1;
            adv(p); adv(p);
            int ix = parse_expr(p);
            if (ix < 0) return -1;
            if (expect(p, OT_RBRACK)) return -1;
            if (expect(p, OT_ASSIGN)) return -1;
            if (at(p, OT_AMP)) {
                const OscToken *bt = cur(p);
                char nm[64];
                osc_node_name(p->ast, N(n), nm, sizeof nm);
                osc_diag_set(p->d, OSC_DIAG_BORROW_OUTLIVES_OWNER, bt->line, bt->col, nm, 0, NULL,
                             "store borrow into array", "a borrow cannot be stored anywhere but a borrow binding");
                return -1;
            }
            int v = parse_expr(p);
            if (v < 0) return -1;
            N(n)->a = ix;
            N(n)->b = v;
            if (expect(p, OT_SEMI)) return -1;
            return n;
        }
        adv(p);
        return syntax(p, "'=', '[' or '(' after a name at statement start");
    }
    case OT_POOL: { /* OSC-3 item 2: pool NAME: [T; K] [gen B] { ... } */
        n = new_node(p, ON_POOL, t);
        if (n < 0) return -1;
        adv(p);
        if (parse_name_tok(p, n)) return -1;
        if (expect(p, OT_COLON)) return -1;
        if (expect(p, OT_LBRACK)) return -1;
        if (parse_scalar(p, &N(n)->ty)) return -1;
        if (expect(p, OT_SEMI)) return -1;
        if (!at(p, OT_INT)) return syntax(p, "a slot count INT");
        const OscToken *kt = cur(p);
        if (kt->ival == 0 || kt->ival > OSC_POOL_MAX_SLOTS) {
            char nm[64];
            osc_node_name(p->ast, N(n), nm, sizeof nm);
            osc_diag_set(p->d, OSC_DIAG_POOL_CAPACITY, kt->line, kt->col, nm, t->line, NULL,
                         "pool size outside 1..16", "pool size %llu outside 1..%d slots",
                         (unsigned long long)kt->ival, OSC_POOL_MAX_SLOTS);
            return -1;
        }
        N(n)->ival = kt->ival;
        adv(p);
        if (expect(p, OT_RBRACK)) return -1;
        N(n)->lo = 0;
        if (at(p, OT_NAME) && tok_eq(p, cur(p), "gen")) { /* declared generation base */
            adv(p);
            if (!at(p, OT_INT)) return syntax(p, "gen INT (the generation base)");
            N(n)->lo = (int64_t)cur(p)->ival;
            adv(p);
        }
        int b = parse_block(p);
        if (b < 0) return -1;
        N(n)->b = b;
        return n;
    }
    case OT_ARENA: { /* OSC-2 arenas: arena NAME bound K { ... } */
        n = new_node(p, ON_ARENA, t);
        if (n < 0) return -1;
        adv(p);
        if (parse_name_tok(p, n)) return -1;
        if (!at(p, OT_BOUND)) return syntax(p, "'bound K' after the arena name");
        adv(p);
        if (!at(p, OT_INT)) return syntax(p, "bound INT");
        const OscToken *bt = cur(p);
        if (bt->ival == 0 || bt->ival > OSC_ARENA_MAX_CELLS) {
            char nm[64];
            osc_node_name(p->ast, N(n), nm, sizeof nm);
            osc_diag_set(p->d, OSC_DIAG_ARENA_CAPACITY, bt->line, bt->col, nm, t->line, NULL,
                         "arena bound outside 1..64", "arena bound %llu outside 1..%d cells",
                         (unsigned long long)bt->ival, OSC_ARENA_MAX_CELLS);
            return -1;
        }
        N(n)->ival = bt->ival;
        adv(p);
        int b = parse_block(p);
        if (b < 0) return -1;
        N(n)->b = b;
        return n;
    }
    case OT_IF:
        return parse_if(p);
    case OT_WHILE: {
        n = new_node(p, ON_WHILE, t);
        if (n < 0) return -1;
        adv(p);
        int c = parse_expr(p);
        if (c < 0) return -1;
        N(n)->a = c;
        if (!at(p, OT_BOUND)) {
            osc_diag_set(p->d, OSC_DIAG_UNBOUNDED_LOOP, t->line, t->col, "while", 0, NULL, "while without bound",
                         "'while' needs 'bound N' (static trip-count bound)");
            return -1;
        }
        adv(p);
        if (!at(p, OT_INT)) return syntax(p, "bound INT");
        const OscToken *bt = cur(p);
        if (bt->ival == 0 || bt->ival > LOOP_BOUND_MAX) {
            osc_diag_set(p->d, OSC_DIAG_UNBOUNDED_LOOP, bt->line, bt->col, "while", t->line, NULL,
                         "bound outside 1..1000000", "loop bound %llu outside 1..%d",
                         (unsigned long long)bt->ival, LOOP_BOUND_MAX);
            return -1;
        }
        N(n)->ival = bt->ival;
        N(n)->flag = 1;
        adv(p);
        int b = parse_block(p);
        if (b < 0) return -1;
        N(n)->b = b;
        return n;
    }
    case OT_FOR: {
        n = new_node(p, ON_FOR, t);
        if (n < 0) return -1;
        adv(p);
        if (parse_name_tok(p, n)) return -1;
        if (expect(p, OT_IN)) return -1;
        const OscToken *lt = cur(p);
        if (parse_int_signed(p, &N(n)->lo)) return -1;
        if (expect(p, OT_DOTDOT)) return -1;
        if (parse_int_signed(p, &N(n)->hi)) return -1;
        if (N(n)->hi < N(n)->lo || (uint64_t)(N(n)->hi - N(n)->lo) > LOOP_BOUND_MAX) {
            char nm[64];
            osc_node_name(p->ast, N(n), nm, sizeof nm);
            osc_diag_set(p->d, OSC_DIAG_UNBOUNDED_LOOP, lt->line, lt->col, nm, t->line, NULL,
                         "for range outside 0..1000000 trips", "for range must satisfy A <= B and B - A <= %d",
                         LOOP_BOUND_MAX);
            return -1;
        }
        int b = parse_block(p);
        if (b < 0) return -1;
        N(n)->b = b;
        return n;
    }
    case OT_RETURN: {
        n = new_node(p, ON_RETURN, t);
        if (n < 0) return -1;
        adv(p);
        if (!at(p, OT_SEMI)) {
            int e = at(p, OT_AMP) ? parse_borrow(p) : parse_expr(p);
            if (e < 0) return -1;
            N(n)->a = e;
        }
        if (expect(p, OT_SEMI)) return -1;
        return n;
    }
    case OT_LBRACE:
        return parse_block(p);
    default:
        return syntax(p, "a statement");
    }
}

static int parse_if(P *p)
{
    if (enter(p)) return -1;
    int n = new_node(p, ON_IF, cur(p));
    if (n < 0) return -1;
    adv(p);
    int c = parse_expr(p);
    if (c < 0) return -1;
    N(n)->a = c;
    int b = parse_block(p);
    if (b < 0) return -1;
    N(n)->b = b;
    if (at(p, OT_ELSE)) {
        adv(p);
        int e = at(p, OT_IF) ? parse_if(p) : parse_block(p);
        if (e < 0) return -1;
        N(n)->c = e;
    }
    leave(p);
    return n;
}

static int parse_block(P *p)
{
    if (enter(p)) return -1;
    int n = new_node(p, ON_BLOCK, cur(p));
    if (n < 0) return -1;
    if (expect(p, OT_LBRACE)) return -1;
    int last = -1;
    while (!at(p, OT_RBRACE)) {
        if (at(p, OT_EOF)) return syntax(p, "'}'");
        int s = parse_stmt(p);
        if (s < 0) return -1;
        if (last < 0) N(n)->a = s; else N(last)->next = s;
        last = s;
    }
    N(n)->ival = cur(p)->line; /* closing brace line */
    adv(p);
    leave(p);
    return n;
}

/* clause: one expression (OSC-2, docs/osc/OSC-2-DESIGN.md section 1). The
 * source text is still recorded (token spellings joined by single spaces,
 * none after '(' or before ')') and carried into the IR; the expression node
 * is checked and lowered into runtime checks by the checker / lowerer. */
static int parse_clause(P *p, char *out, const OscToken *kw, int32_t *node)
{
    uint32_t start = p->pos;
    out[0] = 0;
    if (at(p, OT_LBRACE) || at(p, OT_ENSURES) || at(p, OT_EOF)) return syntax(p, "a contract expression");
    int e = parse_expr(p);
    if (e < 0) return -1;
    if (!at(p, OT_LBRACE) && !at(p, OT_ENSURES))
        return syntax(p, "'ensures' or a function body '{' after the contract expression");
    size_t len = 0;
    int prev = -1;
    for (uint32_t i = start; i < p->pos; i++) {
        const OscToken *t = &p->ast->toks[i];
        int k = t->kind;
        int sp = len > 0 && prev != OT_LPAREN && k != OT_RPAREN;
        if (len + (size_t)sp + t->len + 1 > OSC_CLAUSE_MAX) {
            osc_diag_set(p->d, OSC_DIAG_CAPACITY, kw->line, kw->col, osc_tok_kind_name(kw->kind), 0, NULL,
                         "contract text length", "contract clause longer than %d bytes", OSC_CLAUSE_MAX - 1);
            return -1;
        }
        if (sp) out[len++] = ' ';
        memcpy(out + len, p->ast->src + t->off, t->len);
        len += t->len;
        out[len] = 0;
        prev = k;
    }
    *node = e;
    return 0;
}

static int parse_fn(P *p)
{
    OscAst *a = p->ast;
    const OscToken *ft = cur(p);
    if (expect(p, OT_FN)) return -1;
    if (a->nfns >= OSC_MAX_FUNCS) {
        osc_diag_set(p->d, OSC_DIAG_CAPACITY, ft->line, ft->col, "unit", 0, NULL, "function capacity",
                     "more than %d functions", OSC_MAX_FUNCS);
        return -1;
    }
    int f = new_node(p, ON_FN, ft);
    if (f < 0) return -1;
    if (parse_name_tok(p, f)) return -1;
    N(f)->line = a->toks[N(f)->tok].line;
    N(f)->col = a->toks[N(f)->tok].col;
    char fname[64];
    osc_node_name(a, N(f), fname, sizeof fname);
    if (expect(p, OT_LPAREN)) return -1;
    int last = -1, np = 0, nslice = 0;
    if (!at(p, OT_RPAREN)) {
        for (;;) {
            int pn = new_node(p, ON_PARAM, cur(p));
            if (pn < 0) return -1;
            if (parse_name_tok(p, pn)) return -1;
            if (expect(p, OT_COLON)) return -1;
            if (parse_ptype(p, &N(pn)->ty)) return -1;
            int is_slice = N(pn)->ty.s == OSC_T_BYTES || N(pn)->ty.s == OSC_T_CELLS;
            nslice += is_slice;
            np += is_slice ? 2 : 1; /* parameter registers: a slice is pointer + length */
            if (np > OSC_MAX_PARAMS && nslice) {
                osc_diag_set(p->d, OSC_DIAG_SLICE_PARAMS, N(pn)->line, N(pn)->col, fname, N(f)->line, NULL,
                             "parameter registers", "function '%s' needs %d parameter registers (a slice takes two); at most %d",
                             fname, np, OSC_MAX_PARAMS);
                return -1;
            }
            if (np > OSC_MAX_PARAMS) {
                osc_diag_set(p->d, OSC_DIAG_CAPACITY, N(pn)->line, N(pn)->col, fname, N(f)->line, NULL,
                             "parameter count", "function '%s' has more than %d parameters", fname, OSC_MAX_PARAMS);
                return -1;
            }
            if (last < 0) N(f)->a = pn; else N(last)->next = pn;
            last = pn;
            if (at(p, OT_COMMA)) { adv(p); continue; }
            break;
        }
    }
    if (expect(p, OT_RPAREN)) return -1;
    N(f)->ty.s = OSC_T_VOID;
    if (at(p, OT_ARROW)) {
        adv(p);
        {
            uint32_t q = p->pos;
            while (q < a->ntok && (a->toks[q].kind == OT_OWN || a->toks[q].kind == OT_AMP || a->toks[q].kind == OT_MUT)) q++;
            if (a->toks[q].kind == OT_NAME && find_struct(p, &a->toks[q]) >= 0)
                return struct_by_value(p, &a->toks[q], "struct return type");
        }
        if (at(p, OT_HANDLE) || at(p, OT_POOL)) return local_only(p);
        if (at(p, OT_OWN) || at(p, OT_AMP) || at(p, OT_LBRACK)) {
            const OscToken *t = cur(p);
            osc_diag_set(p->d, OSC_DIAG_UNSUPPORTED, t->line, t->col, fname, N(f)->line, NULL,
                         "array return type", "OSC-1 functions return a scalar only");
            return -1;
        }
        if (parse_scalar(p, &N(f)->ty)) return -1;
    }
    uint32_t fi = a->nfns;
    a->req[fi][0] = a->ens[fi][0] = 0;
    a->reqn[fi] = a->ensn[fi] = a->res_sym[fi] = -1;
    a->req_elide[fi] = a->ens_elide[fi] = 0;
    if (at(p, OT_REQUIRES)) {
        const OscToken *kw = cur(p);
        adv(p);
        if (parse_clause(p, a->req[fi], kw, &a->reqn[fi])) return -1;
    }
    if (at(p, OT_ENSURES)) {
        const OscToken *kw = cur(p);
        adv(p);
        if (parse_clause(p, a->ens[fi], kw, &a->ensn[fi])) return -1;
    }
    int b = parse_block(p);
    if (b < 0) return -1;
    N(f)->b = b;
    a->fns[a->nfns++] = f;
    return 0;
}

static int parse_import(P *p)
{
    OscAst *a = p->ast;
    const OscToken *kw = cur(p);
    adv(p);
    if (!at(p, OT_NAME)) return syntax(p, "NAME");
    const OscToken *nm = cur(p);
    if (a->nimports >= OSC_MAX_IMPORTS) {
        osc_diag_set(p->d, OSC_DIAG_CAPACITY, kw->line, kw->col, "unit", 0, NULL, "import capacity",
                     "more than %d imports", OSC_MAX_IMPORTS);
        return -1;
    }
    OscImport *im = &a->imports[a->nimports];
    im->line = nm->line;
    im->col = nm->col;
    osc_tok_text(a->src, nm, im->name, sizeof im->name);
    for (uint8_t k = 0; k < a->nimports; k++)
        if (strcmp(a->imports[k].name, im->name) == 0) {
            osc_diag_set(p->d, OSC_DIAG_REDEFINED_NAME, nm->line, nm->col, im->name, a->imports[k].line, NULL,
                         "import", "import '%s' is listed twice", im->name);
            return -1;
        }
    adv(p);
    if (expect(p, OT_SEMI)) return -1;
    a->nimports++;
    return 0;
}

int osc_parse(OscAst *ast, OscDiag *d)
{
    P p = {ast, d, 0, 0};
    ast->nnodes = 0;
    ast->nfns = 0;
    ast->nrel = 0;
    ast->nstructs = 0;
    ast->nimports = 0;
    while (!at(&p, OT_EOF)) {
        if (at(&p, OT_IMPORT)) {
            if (ast->nstructs || ast->nfns) {
                const OscToken *t = cur(&p);
                osc_diag_set(d, OSC_DIAG_SYNTAX, t->line, t->col, "import", 0, NULL, "import position",
                             "imports must come before any struct or function");
                return -1;
            }
            if (parse_import(&p)) return -1;
            continue;
        }
        if (at(&p, OT_STRUCT)) { if (parse_struct(&p)) return -1; continue; }
        if (parse_fn(&p)) return -1;
    }
    return 0;
}
