/* c2_check.c: checker2 acceptance rules over a decoded unit (spec/one-layer-receipts.md section 9).
 *
 * Written from docs/osc/OSC-{1,2,3}-DESIGN.md, OSC-EXT-BYTES-DESIGN.md and the osc_ir.h comments
 * only. Rules, in the order applied:
 *   unit tables  -> per-function types -> CFG shape -> per-instruction typing -> calls ->
 *   handle disciplines -> definite assignment -> loop bounds (termination) -> ownership flow.
 * The ownership flow is a may-analysis that rejects only DEFINITE errors (a state that holds on
 * every path), so it cannot reject a correct program through imprecision; drop-flag guarded
 * releases (OSC-3 item 3) are therefore never refused by it. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c2_ir.h"

static int fail(C2Verdict *v, const char *rule, int func, int insn, const char *fmt, ...)
{
    if (v) {
        v->ok = 0;
        v->rule = rule;
        v->func = func;
        v->insn = insn;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(v->detail, sizeof v->detail, fmt, ap);
        va_end(ap);
    }
    return -1;
}

/* ------------------------------------------------------------------ types */

static int is_value(uint8_t s) { return s >= S_BOOL && s <= S_I64; }
static int is_int(uint8_t s) { return s >= S_U8 && s <= S_I64; }
static int is_signed(uint8_t s) { return s >= S_I8 && s <= S_I64; }
static unsigned width(uint8_t s)
{
    switch (s) {
    case S_BOOL: return 1;
    case S_U8: case S_I8: return 8;
    case S_U16: case S_I16: return 16;
    case S_U32: case S_I32: return 32;
    case S_U64: case S_I64: return 64;
    default: return 0;
    }
}
static int is_scalar_t(const C2Type *t) { return is_value(t->s); }
static int is_ref(const C2Type *t) { return t->s == S_REF; }
static int is_array_ref(const C2Type *t) { return t->s == S_REF && t->sid == 0; }
static int is_struct_ref(const C2Type *t) { return t->s == S_REF && t->sid != 0; }
static int is_slice(const C2Type *t) { return t->s == S_BYTES || t->s == S_CELLS; }
static int is_u64(const C2Type *t) { return t->s == S_U64; }
static int same_type(const C2Type *a, const C2Type *b)
{
    if (a->s != b->s) return 0;
    if (a->s != S_REF) return 1;
    return a->kind == b->kind && a->elem == b->elem && a->len == b->len && a->sid == b->sid;
}
/* Same object shape (element, length, struct), any reference kind. */
static int same_shape(const C2Type *a, const C2Type *b)
{
    return a->s == S_REF && b->s == S_REF && a->elem == b->elem && a->len == b->len && a->sid == b->sid;
}
/* A reference of kind `from` may be bound to a destination of kind `to` (move, borrow, reborrow,
 * downgrade): own->own|shared|mut, mut->mut|shared, shared->shared. */
static int bindable(uint8_t to, uint8_t from)
{
    if (from == K_OWN) return 1;
    if (from == K_MUT) return to == K_MUT || to == K_SHARED;
    if (from == K_SHARED) return to == K_SHARED;
    return 0;
}
/* imm is the canonical 64-bit value of a constant of scalar s */
static int canonical(uint8_t s, uint64_t imm)
{
    unsigned w = width(s);
    if (s == S_BOOL) return imm <= 1;
    if (w == 64) return 1;
    if (!is_signed(s)) return imm < (1ull << w);
    int64_t x = (int64_t)imm;
    int64_t lo = -(int64_t)(1ull << (w - 1)), hi = (int64_t)(1ull << (w - 1)) - 1;
    return x >= lo && x <= hi;
}

/* ------------------------------------------------------------------ unit tables */

static int check_structs(const C2Unit *u, C2Verdict *v)
{
    for (int k = 0; k < u->nstructs; k++) {
        const C2Struct *s = &u->st[k];
        for (int j = 0; j < k; j++)
            if (strcmp(u->st[j].name, s->name) == 0)
                return fail(v, "C2-STRUCT-NAME", -1, -1, "struct %s defined twice", s->name);
        unsigned off = 0;
        for (int f = 0; f < s->nfields; f++) {
            const C2Field *fd = &s->f[f];
            for (int g = 0; g < f; g++)
                if (strcmp(s->f[g].name, fd->name) == 0)
                    return fail(v, "C2-STRUCT-NAME", -1, -1, "field %s.%s twice", s->name, fd->name);
            if (!is_value(fd->s)) return fail(v, "C2-STRUCT-FIELD-TYPE", -1, -1, "%s.%s", s->name, fd->name);
            if (fd->alen > C2_MAX_ARRAY)
                return fail(v, "C2-STRUCT-FIELD-LEN", -1, -1, "%s.%s length %u", s->name, fd->name, fd->alen);
            if (fd->off != off) /* GUARD:struct-offset */
                return fail(v, "C2-STRUCT-LAYOUT", -1, -1, "%s.%s offset %u, prefix sum %u", s->name, fd->name, fd->off, off);
            off += fd->alen ? fd->alen : 1;
        }
        if (s->ncells != off || off < 1 || off > C2_MAX_ARRAY) /* GUARD:struct-cells */
            return fail(v, "C2-STRUCT-LAYOUT", -1, -1, "%s cells %u, fields need %u", s->name, s->ncells, off);
    }
    return 0;
}

static int check_type(const C2Unit *u, const C2Type *t)
{
    if (t->s >= S_LIMIT || t->s == S_VOID) return 0;
    if (t->s != S_REF) return 1;
    if (t->kind < K_OWN || t->kind > K_MUT) return 0;
    if (t->elem == S_VOID) { /* struct reference */
        if (t->sid < 1 || t->sid > u->nstructs) return 0;
        return t->len == u->st[t->sid - 1].ncells;
    }
    return is_value(t->elem) && t->sid == 0 && t->len >= 1 && t->len <= C2_MAX_ARRAY;
}

/* ------------------------------------------------------------------ per-function context */

#define BSW 8 /* 512 bits: enough for C2_MAX_VREGS (480) and C2_MAX_BLOCKS (512) */
typedef struct { uint64_t w[BSW]; } Bits;
static void bset(Bits *b, unsigned i) { b->w[i >> 6] |= 1ull << (i & 63); }
static int btest(const Bits *b, unsigned i) { return (int)((b->w[i >> 6] >> (i & 63)) & 1); }
static void ball(Bits *b) { memset(b, 0xff, sizeof *b); }
static void band(Bits *d, const Bits *s) { for (int i = 0; i < BSW; i++) d->w[i] &= s->w[i]; }
static int beq(const Bits *a, const Bits *b) { return memcmp(a, b, sizeof *a) == 0; }

typedef struct {
    const C2Unit *u;
    const C2Func *f;
    int fi;
    C2Verdict *v;
    int nb;
    int *blk_of;          /* insn -> block */
    int succ[C2_MAX_BLOCKS][2];
    int nsucc[C2_MAX_BLOCKS];
    int reach[C2_MAX_BLOCKS];
    Bits dom[C2_MAX_BLOCKS];
    int ndefs[C2_MAX_VREGS];
    int def_insn[C2_MAX_VREGS]; /* the single definition, or -1 */
    int quiet;                  /* ownership fixpoint: apply effects, report nothing */
} Fx;

static int term(uint8_t op) { return op == C2_BR || op == C2_CBR || op == C2_RET; }

/* uses of an instruction (vregs read); returns count */
static int uses(const C2Insn *in, int16_t *o)
{
    int n = 0;
    switch (in->op) {
    case C2_CONST: case C2_TRAP: case C2_BR: case C2_AOPEN: case C2_POPEN: break;
    case C2_MOV: case C2_CAST: case C2_UN: case C2_ALLOC: case C2_RELEASE: case C2_ADESTROY: case C2_PCLOSE:
    case C2_CBR: o[n++] = in->a; break;
    case C2_RET: if (in->a >= 0) o[n++] = in->a; break;
    case C2_BIN: case C2_CMP: case C2_LOAD: case C2_AALLOC: case C2_HALLOC: case C2_HGEN: case C2_SLOAD:
        o[n++] = in->a; o[n++] = in->b; break;
    case C2_STORE: case C2_HFREE: case C2_HLOAD: case C2_SSTORE: o[n++] = in->a; o[n++] = in->b; o[n++] = in->c; break;
    case C2_HSTORE: o[n++] = in->a; o[n++] = in->b; o[n++] = in->c; o[n++] = in->args[0]; break;
    case C2_FLOAD: o[n++] = in->a; if (in->b >= 0) o[n++] = in->b; break;
    case C2_FSTORE: o[n++] = in->a; if (in->b >= 0) o[n++] = in->b; o[n++] = in->c; break;
    case C2_CALL: for (int k = 0; k < in->nargs; k++) o[n++] = in->args[k]; break;
    default: break;
    }
    return n;
}

static int defines(const C2Insn *in)
{
    switch (in->op) {
    case C2_CONST: case C2_MOV: case C2_BIN: case C2_UN: case C2_CMP: case C2_CAST: case C2_ALLOC: case C2_LOAD:
    case C2_FLOAD: case C2_AOPEN: case C2_AALLOC: case C2_POPEN: case C2_HALLOC: case C2_HGEN: case C2_HLOAD:
    case C2_SLOAD:
        return in->dst;
    case C2_CALL: return in->dst; /* -1 for a void call */
    default: return -1;
    }
}

#define F(rule, ...) return fail(x->v, rule, x->fi, (int)ii, __VA_ARGS__)

static const C2Type *T(const Fx *x, int16_t r) { return &x->f->vt[r]; }

static int check_insn(Fx *x, uint32_t ii)
{
    const C2Func *f = x->f;
    const C2Insn *in = &f->in[ii];
    int nv = f->nvregs;
    int16_t us[8];
    int nu = uses(in, us);
    for (int k = 0; k < nu; k++) {
        if (us[k] < 0 || us[k] >= nv) F("C2-OPERAND-RANGE", "operand v%d out of range", us[k]); /* GUARD:operand-range */
    }
    int d = defines(in);
    int need_dst = !(in->op == C2_CALL && f->in[ii].dst == -1) && d != -1 ? 1 : 0;
    if (in->op != C2_CALL && (in->op == C2_CONST || in->op == C2_MOV || in->op == C2_BIN || in->op == C2_UN ||
                              in->op == C2_CMP || in->op == C2_CAST || in->op == C2_ALLOC || in->op == C2_LOAD ||
                              in->op == C2_FLOAD || in->op == C2_AOPEN || in->op == C2_AALLOC || in->op == C2_POPEN ||
                              in->op == C2_HALLOC || in->op == C2_HGEN || in->op == C2_HLOAD || in->op == C2_SLOAD) &&
        in->dst < 0)
        F("C2-OPERAND-RANGE", "missing destination");
    if (need_dst && (d < 0 || d >= nv)) F("C2-OPERAND-RANGE", "destination v%d out of range", d); /* GUARD:dst-range */
    if (d >= 0 && is_slice(T(x, d))) F("C2-SLICE-WRITE", "slice register v%d redefined", d);
    if (d >= 0 && d < f->nparams && d > 0 && is_slice(T(x, d - 1)))
        F("C2-SLICE-WRITE", "slice length register v%d redefined", d);

    const C2Type *td = d >= 0 ? T(x, d) : NULL;
    const C2Type *ta = in->a >= 0 && in->a < nv ? T(x, in->a) : NULL;
    const C2Type *tb = in->b >= 0 && in->b < nv ? T(x, in->b) : NULL;
    const C2Type *tc = in->c >= 0 && in->c < nv ? T(x, in->c) : NULL;

    switch (in->op) {
    case C2_CONST:
        if (!is_scalar_t(td)) F("C2-TYPE-CONST", "constant into a non-scalar register");
        if (!canonical(td->s, in->imm)) F("C2-CONST-CANONICAL", "constant %llu not canonical for its type", (unsigned long long)in->imm); /* GUARD:const-canonical */
        break;
    case C2_MOV:
        if (is_slice(ta) || is_slice(td)) F("C2-SLICE-ESCAPE", "slice copied");
        if (is_ref(td) || is_ref(ta)) {
            if (!same_shape(td, ta) || !bindable(td->kind, ta->kind)) F("C2-TYPE-MOV", "reference kinds or shapes do not bind"); /* GUARD:mov-ref */
        } else {
            if (!same_type(td, ta) || !is_scalar_t(td)) F("C2-TYPE-MOV", "MOV between different types"); /* GUARD:mov-type */
        }
        break;
    case C2_BIN:
        if (in->sub < B_ADD || in->sub >= B_LIMIT) F("C2-DECODE-SUBOP", "binary op %u", in->sub);
        if (in->sub == B_SHL || in->sub == B_SHR) {
            if (!is_int(td->s) || !same_type(td, ta) || !is_int(tb->s)) F("C2-TYPE-BIN", "shift operand types"); /* GUARD:bin-shift */
        } else if (in->sub == B_AND || in->sub == B_OR || in->sub == B_XOR) {
            if (!is_scalar_t(td) || !same_type(td, ta) || !same_type(td, tb)) F("C2-TYPE-BIN", "bitwise operand types");
        } else {
            if (!is_int(td->s) || !same_type(td, ta) || !same_type(td, tb)) F("C2-TYPE-BIN", "arithmetic operand types"); /* GUARD:bin-type */
        }
        break;
    case C2_UN:
        if (in->sub < U_NEG || in->sub >= U_LIMIT) F("C2-DECODE-SUBOP", "unary op %u", in->sub);
        if (!same_type(td, ta)) F("C2-TYPE-UN", "unary result type differs from operand");
        if (in->sub == U_LNOT ? td->s != S_BOOL : !is_int(td->s)) F("C2-TYPE-UN", "unary operand type"); /* GUARD:un-type */
        break;
    case C2_CMP:
        if (in->sub < CC_EQ || in->sub >= CC_LIMIT) F("C2-DECODE-SUBOP", "comparison %u", in->sub);
        if (td->s != S_BOOL || !is_scalar_t(ta) || !same_type(ta, tb)) F("C2-TYPE-CMP", "comparison operand types"); /* GUARD:cmp-type */
        break;
    case C2_CAST:
        if (!is_int(td->s) || !is_int(ta->s)) F("C2-TYPE-CAST", "cast between non-integers (bool casts are refused)"); /* GUARD:cast-type */
        break;
    case C2_ALLOC: case C2_AALLOC:
        if (!is_ref(td) || td->kind != K_OWN) F("C2-TYPE-ALLOC", "allocation into a non-owner"); /* GUARD:alloc-own */
        if (is_array_ref(td) ? ta->s != td->elem : ta->s != S_U64) F("C2-TYPE-ALLOC", "initial value type"); /* GUARD:alloc-init */
        if (in->op == C2_AALLOC && !is_u64(tb)) F("C2-TYPE-ALLOC", "arena handle is not u64");
        break;
    case C2_RELEASE:
        if (!is_ref(ta) || ta->kind != K_OWN) F("C2-TYPE-RELEASE", "release of a non-owner"); /* GUARD:release-own */
        break;
    case C2_LOAD: case C2_STORE:
        if (!is_array_ref(ta)) F("C2-TYPE-INDEX", "indexing a non-array"); /* GUARD:index-array */
        if (!is_int(tb->s)) F("C2-TYPE-INDEX", "index is not an integer");
        if (in->op == C2_LOAD ? td->s != ta->elem : tc->s != ta->elem) F("C2-TYPE-INDEX", "element type"); /* GUARD:index-elem */
        if (in->op == C2_STORE && ta->kind == K_SHARED) F("C2-READ-ONLY", "store through a shared borrow"); /* GUARD:store-shared */
        break;
    case C2_FLOAD: case C2_FSTORE: {
        if (!is_struct_ref(ta)) F("C2-TYPE-FIELD", "field access on a non-struct");
        const C2Struct *s = &x->u->st[ta->sid - 1];
        if (in->imm >= s->nfields) F("C2-TYPE-FIELD", "field %llu of %s", (unsigned long long)in->imm, s->name); /* GUARD:field-index */
        const C2Field *fd = &s->f[in->imm];
        if (fd->alen ? (!tb || !is_int(tb->s)) : in->b != -1) F("C2-TYPE-FIELD", "element index of field %s.%s", s->name, fd->name); /* GUARD:field-elem-index */
        if (in->op == C2_FLOAD ? td->s != fd->s : tc->s != fd->s) F("C2-TYPE-FIELD", "type of field %s.%s", s->name, fd->name); /* GUARD:field-type */
        if (in->op == C2_FSTORE && ta->kind == K_SHARED) F("C2-READ-ONLY", "field store through a shared borrow"); /* GUARD:fstore-shared */
        break;
    }
    case C2_AOPEN:
        if (!is_u64(td)) F("C2-TYPE-HANDLE", "arena handle is not u64");
        if (in->imm < 1 || in->imm > C2_MAX_ARRAY) F("C2-ARENA-CAPACITY", "arena of %llu cells", (unsigned long long)in->imm); /* GUARD:arena-cap */
        break;
    case C2_ADESTROY: case C2_PCLOSE:
        if (!is_u64(ta)) F("C2-TYPE-HANDLE", "handle is not u64");
        break;
    case C2_POPEN:
        if (!is_u64(td)) F("C2-TYPE-HANDLE", "pool id is not u64");
        if (!is_value(in->sub)) F("C2-TYPE-POOL", "pool element type %u", in->sub);
        if (in->nargs < 1 || in->nargs > C2_POOL_SLOTS) F("C2-POOL-CAPACITY", "pool of %u slots", in->nargs); /* GUARD:pool-cap */
        break;
    case C2_HALLOC: if (!is_u64(td) || !is_u64(tb) || !is_value(ta->s)) F("C2-TYPE-POOL", "pool alloc operand types"); break;
    case C2_HGEN: if (!is_u64(td) || !is_u64(ta) || !is_u64(tb)) F("C2-TYPE-POOL", "pool generation operand types"); break;
    case C2_HFREE: if (!is_u64(ta) || !is_u64(tb) || !is_u64(tc)) F("C2-TYPE-POOL", "pool free operand types"); break;
    case C2_HLOAD: if (!is_value(td->s) || !is_u64(ta) || !is_u64(tb) || !is_u64(tc)) F("C2-TYPE-POOL", "pool load operand types"); break;
    case C2_HSTORE:
        if (in->args[0] < 0 || in->args[0] >= nv) F("C2-OPERAND-RANGE", "pool store value");
        if (!is_u64(ta) || !is_u64(tb) || !is_u64(tc)) F("C2-TYPE-POOL", "pool store operand types");
        break;
    case C2_SLOAD: case C2_SSTORE:
        if (!is_slice(ta)) F("C2-TYPE-SLICE", "slice access through a non-slice register"); /* GUARD:slice-ptr */
        if (in->op == C2_SSTORE && ta->s != S_CELLS) F("C2-SLICE-WRITE", "store into a read-only bytes slice"); /* GUARD:slice-bytes-ro */
        if (!is_int(tb->s)) F("C2-TYPE-SLICE", "slice index is not an integer");
        if (in->op == C2_SLOAD ? !is_u64(td) : !is_u64(tc)) F("C2-TYPE-SLICE", "slice element is u64");
        break;
    case C2_TRAP:
        if (in->imm < 1 || in->imm > C2_TRAP_MAX) F("C2-TRAP-CODE", "trap code %llu", (unsigned long long)in->imm); /* GUARD:trap-code */
        break;
    case C2_BR:
        if (in->t >= f->nblocks) F("C2-CFG-TARGET", "branch to block %u", in->t); /* GUARD:br-target */
        break;
    case C2_CBR:
        if (in->t >= f->nblocks || in->f >= f->nblocks) F("C2-CFG-TARGET", "branch to block %u/%u", in->t, in->f); /* GUARD:cbr-target */
        if (ta->s != S_BOOL) F("C2-TYPE-COND", "branch condition is not bool"); /* GUARD:cbr-bool */
        break;
    case C2_RET:
        if (f->ret.s == S_VOID ? in->a != -1 : (in->a < 0 || !same_type(ta, &f->ret))) F("C2-TYPE-RET", "return value type"); /* GUARD:ret-type */
        break;
    case C2_CALL: {
        if (in->callee >= x->fi) F("C2-CALL-ORDER", "call to function %u from function %d (no recursion, no forward calls)", in->callee, x->fi); /* GUARD:call-order */
        const C2Func *g = &x->u->fn[in->callee];
        if (in->nargs != g->nparams) F("C2-CALL-ARITY", "%u arguments for %u parameters", in->nargs, g->nparams); /* GUARD:call-arity */
        if (g->ret.s == S_VOID ? in->dst != -1 : (in->dst < 0 || in->dst >= nv || !same_type(T(x, in->dst), &g->ret)))
            F("C2-TYPE-CALL", "call result type"); /* GUARD:call-ret */
        for (int k = 0; k < in->nargs; k++) {
            const C2Type *pa = T(x, in->args[k]), *pp = &g->vt[k];
            if (is_ref(pp)) {
                if (!same_shape(pa, pp) || !bindable(pp->kind, pa->kind)) F("C2-TYPE-CALL", "argument %d does not bind", k); /* GUARD:call-ref */
            } else if (is_slice(pp)) {
                if (pa->s != pp->s) F("C2-TYPE-CALL", "slice argument %d type", k);
                if (k + 1 >= in->nargs || in->args[k + 1] != in->args[k] + 1)
                    F("C2-TYPE-CALL", "slice argument %d is not passed with its own length", k);
            } else {
                if (!same_type(pa, pp)) F("C2-TYPE-CALL", "argument %d type", k); /* GUARD:call-scalar */
            }
            /* aliasing: the same register twice where either parameter may write or take it */
            for (int j = 0; j < k; j++) {
                int alias = in->args[j] == in->args[k] && (is_ref(pp) || pp->s == S_CELLS) &&
                            (pp->kind != K_SHARED || g->vt[j].kind != K_SHARED);
                if (alias) F("C2-CALL-ALIAS", "register v%d passed twice and written", in->args[k]); /* GUARD:call-alias */
            }
        }
        break;
    }
    default: F("C2-DECODE-OPCODE", "opcode %u", in->op);
    }
    return 0;
}

/* ------------------------------------------------------------------ CFG */

static int build_cfg(Fx *x)
{
    const C2Func *f = x->f;
    uint32_t pos = 0;
    for (int b = 0; b < f->nblocks; b++) {
        const C2Block *bk = &f->blk[b];
        if (bk->count < 1 || bk->first != pos || bk->first + bk->count > f->ninsns) /* GUARD:cfg-layout */
            return fail(x->v, "C2-CFG-LAYOUT", x->fi, -1, "block %d is not the next contiguous range", b);
        for (uint32_t i = bk->first; i < bk->first + bk->count; i++) {
            x->blk_of[i] = b;
            int last = i + 1 == bk->first + bk->count;
            if (term(f->in[i].op) != last) /* GUARD:cfg-terminator */
                return fail(x->v, "C2-CFG-TERMINATOR", x->fi, (int)i, last ? "block %d does not end in a terminator" : "terminator inside block %d", b);
        }
        pos += bk->count;
    }
    if (pos != f->ninsns) return fail(x->v, "C2-CFG-LAYOUT", x->fi, -1, "blocks cover %u of %u instructions", pos, f->ninsns);
    return 0;
}

static void link_cfg(Fx *x)
{
    const C2Func *f = x->f;
    for (int b = 0; b < f->nblocks; b++) {
        const C2Insn *t = &f->in[f->blk[b].first + f->blk[b].count - 1];
        x->nsucc[b] = 0;
        if (t->op == C2_BR) x->succ[b][x->nsucc[b]++] = t->t;
        if (t->op == C2_CBR) {
            x->succ[b][x->nsucc[b]++] = t->t;
            if (t->f != t->t) x->succ[b][x->nsucc[b]++] = t->f;
        }
    }
    int stack[C2_MAX_BLOCKS], sp = 0;
    memset(x->reach, 0, sizeof x->reach);
    x->reach[0] = 1;
    stack[sp++] = 0;
    while (sp) {
        int b = stack[--sp];
        for (int k = 0; k < x->nsucc[b]; k++)
            if (!x->reach[x->succ[b][k]]) { x->reach[x->succ[b][k]] = 1; stack[sp++] = x->succ[b][k]; }
    }
    /* dominators over reachable blocks */
    for (int b = 0; b < f->nblocks; b++) ball(&x->dom[b]);
    memset(&x->dom[0], 0, sizeof x->dom[0]);
    bset(&x->dom[0], 0);
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int b = 1; b < f->nblocks; b++) {
            if (!x->reach[b]) continue;
            Bits n;
            ball(&n);
            for (int p = 0; p < f->nblocks; p++) {
                if (!x->reach[p]) continue;
                for (int k = 0; k < x->nsucc[p]; k++)
                    if (x->succ[p][k] == b) band(&n, &x->dom[p]);
            }
            bset(&n, (unsigned)b);
            if (!beq(&n, &x->dom[b])) { x->dom[b] = n; changed = 1; }
        }
    }
}

static int dominates(const Fx *x, int a, int b) { return btest(&x->dom[b], (unsigned)a); }

/* ------------------------------------------------------------------ disciplines */

static int count_defs(Fx *x)
{
    const C2Func *f = x->f;
    for (int r = 0; r < f->nvregs; r++) { x->ndefs[r] = r < f->nparams ? 1 : 0; x->def_insn[r] = -1; }
    for (uint32_t i = 0; i < f->ninsns; i++) {
        int d = defines(&f->in[i]);
        if (d < 0) continue;
        x->ndefs[d]++;
        x->def_insn[d] = x->ndefs[d] == 1 ? (int)i : -1;
    }
    return 0;
}

/* Arena handles and pool ids: defined exactly once, by AOPEN / POPEN, never a parameter, and used
 * only in their handle positions (OSC-2 section 3.3, OSC-3 item 2). */
static int check_handles(Fx *x)
{
    const C2Func *f = x->f;
    for (uint32_t i = 0; i < f->ninsns; i++) {
        const C2Insn *in = &f->in[i];
        if (in->op != C2_AOPEN && in->op != C2_POPEN) continue;
        int h = in->dst;
        if (h < f->nparams || x->ndefs[h] != 1) /* also implied by handle-origin and the leak rule (untagged) */
            return fail(x->v, "C2-HANDLE-DISCIPLINE", x->fi, (int)i, "handle v%d defined more than once or a parameter", h);
        for (uint32_t j = 0; j < f->ninsns; j++) {
            const C2Insn *o = &f->in[j];
            int16_t us[8];
            int nu = uses(o, us);
            for (int k = 0; k < nu; k++) {
                if (us[k] != h) continue;
                int ok;
                if (in->op == C2_AOPEN)
                    ok = (o->op == C2_AALLOC && o->b == h && o->a != h) || (o->op == C2_ADESTROY && o->a == h);
                else
                    ok = (o->op == C2_PCLOSE && o->a == h) || (o->op == C2_HALLOC && o->b == h && o->a != h) ||
                         ((o->op == C2_HGEN || o->op == C2_HFREE || o->op == C2_HLOAD || o->op == C2_HSTORE) && o->a == h &&
                          o->b != h && o->c != h && o->args[0] != h);
                if (!ok) /* GUARD:handle-use */
                    return fail(x->v, "C2-HANDLE-DISCIPLINE", x->fi, (int)j, "handle v%d used outside its handle position", h);
            }
        }
    }
    /* every handle operand is such a handle, and pool element types agree with the pool */
    for (uint32_t j = 0; j < f->ninsns; j++) {
        const C2Insn *o = &f->in[j];
        int h = -1;
        if (o->op == C2_AALLOC) h = o->b;
        if (o->op == C2_ADESTROY || o->op == C2_PCLOSE || o->op == C2_HGEN || o->op == C2_HFREE || o->op == C2_HLOAD ||
            o->op == C2_HSTORE)
            h = o->a;
        if (o->op == C2_HALLOC) h = o->b;
        if (h < 0) continue;
        int di = x->def_insn[h];
        uint8_t want = (o->op == C2_AALLOC || o->op == C2_ADESTROY) ? C2_AOPEN : C2_POPEN;
        if (di < 0 || f->in[di].op != want) /* GUARD:handle-origin */
            return fail(x->v, "C2-HANDLE-DISCIPLINE", x->fi, (int)j, "v%d is not a handle opened by %s", h, want == C2_AOPEN ? "AOPEN" : "POPEN");
        uint8_t es = f->in[di].sub;
        if (o->op == C2_HALLOC && f->vt[o->a].s != es) return fail(x->v, "C2-TYPE-POOL", x->fi, (int)j, "pool value type");
        if (o->op == C2_HLOAD && f->vt[o->dst].s != es) return fail(x->v, "C2-TYPE-POOL", x->fi, (int)j, "pool value type"); /* GUARD:pool-elem */
        if (o->op == C2_HSTORE && f->vt[o->args[0]].s != es) return fail(x->v, "C2-TYPE-POOL", x->fi, (int)j, "pool value type");
    }
    return 0;
}

/* ------------------------------------------------------------------ definite assignment */

static int check_defined(Fx *x)
{
    const C2Func *f = x->f;
    static Bits in[C2_MAX_BLOCKS], out[C2_MAX_BLOCKS]; /* not reentrant; checker2 is single-threaded */
    Bits params;
    memset(&params, 0, sizeof params);
    for (int r = 0; r < f->nparams; r++) bset(&params, (unsigned)r);
    for (int b = 0; b < f->nblocks; b++) ball(&out[b]);
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int b = 0; b < f->nblocks; b++) {
            if (!x->reach[b]) continue;
            Bits cur;
            if (b == 0) cur = params;
            else {
                ball(&cur);
                for (int p = 0; p < f->nblocks; p++) {
                    if (!x->reach[p]) continue;
                    for (int k = 0; k < x->nsucc[p]; k++)
                        if (x->succ[p][k] == b) band(&cur, &out[p]);
                }
            }
            if (b == 0) {
                /* the entry block may also be a loop header */
                for (int p = 0; p < f->nblocks; p++) {
                    if (!x->reach[p]) continue;
                    for (int k = 0; k < x->nsucc[p]; k++)
                        if (x->succ[p][k] == 0) band(&cur, &out[p]);
                }
            }
            in[b] = cur;
            for (uint32_t i = f->blk[b].first; i < f->blk[b].first + f->blk[b].count; i++) {
                if (f->in[i].op == C2_TRAP) { ball(&cur); break; } /* a trap ends the path */
                int d = defines(&f->in[i]);
                if (d >= 0) bset(&cur, (unsigned)d);
            }
            if (!beq(&cur, &out[b])) { out[b] = cur; changed = 1; }
        }
    }
    for (int b = 0; b < f->nblocks; b++) {
        if (!x->reach[b]) continue;
        Bits cur = in[b];
        for (uint32_t i = f->blk[b].first; i < f->blk[b].first + f->blk[b].count; i++) {
            if (f->in[i].op == C2_TRAP) break;
            int16_t us[8];
            int nu = uses(&f->in[i], us);
            for (int k = 0; k < nu; k++) {
                if (!btest(&cur, (unsigned)us[k])) /* GUARD:definite-assignment */
                    return fail(x->v, "C2-UNDEFINED-USE", x->fi, (int)i, "v%d may be read before it is written", us[k]);
            }
            int d = defines(&f->in[i]);
            if (d >= 0) bset(&cur, (unsigned)d);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ loop bounds */

/* Every loop must be bounded (OSC-1 section 3 "Loops"; OSC-1 section 4 "loops to blocks with an
 * explicit counter vreg, CMP and CBR on the bound"). checker2 proves it structurally: the CFG is
 * reducible, and each natural loop L contains a guard block G that dominates every latch of L and
 * ends in CBR on cond, where
 *   cond is defined once, in G, by CMP c, K with
 *   K defined exactly once in the function, by CONST, |K| <= 1,000,000;
 *   c an integer register whose only definitions inside L are  c = BIN ADD c, s  with s defined
 *   exactly once in the function by CONST 1, each in a block that dominates every latch of L;
 *   CMP GE with the TRUE successor outside L, or CMP LT with the FALSE successor outside L.
 * Then every iteration raises c by one and the loop is left once c >= K: at most |K - c0| + 2
 * iterations per entry (an unsigned counter can wrap at most once, on the first iteration; a
 * signed one traps on overflow). */
static int single_const(const Fx *x, int r, uint64_t *val)
{
    if (r < 0 || x->ndefs[r] != 1 || x->def_insn[r] < 0) return 0;
    const C2Insn *d = &x->f->in[x->def_insn[r]];
    if (d->op != C2_CONST) return 0;
    *val = d->imm;
    return 1;
}

static int loop_guarded(const Fx *x, const int *inloop, const int *latch, int nlatch)
{
    const C2Func *f = x->f;
    for (int g = 0; g < f->nblocks; g++) {
        if (!inloop[g]) continue;
        int domall = 1;
        for (int k = 0; k < nlatch; k++) domall &= dominates(x, g, latch[k]);
        if (!domall) continue;
        const C2Insn *t = &f->in[f->blk[g].first + f->blk[g].count - 1];
        if (t->op != C2_CBR) continue;
        int ci = x->def_insn[t->a];
        if (ci < 0 || x->blk_of[ci] != g || f->in[ci].op != C2_CMP) continue;
        const C2Insn *cmp = &f->in[ci];
        int exits;
        if (cmp->sub == CC_GE) exits = !inloop[t->t];
        else if (cmp->sub == CC_LT) exits = !inloop[t->f];
        else continue;
        if (!exits) continue;
        uint64_t kv, one;
        if (!single_const(x, cmp->b, &kv)) continue;
        uint8_t ks = f->vt[cmp->b].s;
        int64_t ksig = is_signed(ks) ? (int64_t)kv : (kv > 2000000 ? 2000000 : (int64_t)kv);
        if (ksig > 1000000 || ksig < -1000000) continue;
        int c = cmp->a;
        if (!is_int(f->vt[c].s)) continue;
        int incs = 0, bad = 0;
        for (uint32_t i = 0; i < f->ninsns && !bad; i++) {
            if (!inloop[x->blk_of[i]] || defines(&f->in[i]) != c) continue;
            const C2Insn *d = &f->in[i];
            int ok = d->op == C2_BIN && d->sub == B_ADD && d->a == c && single_const(x, d->b, &one) && one == 1;
            for (int k = 0; ok && k < nlatch; k++) ok = dominates(x, x->blk_of[i], latch[k]);
            if (!ok) bad = 1;
            else incs++;
        }
        if (!bad && incs >= 1) return 1;
    }
    return 0;
}

static int check_loops(Fx *x)
{
    const C2Func *f = x->f;
    /* reducibility: every edge into a block that does not dominate its source must go forward in a
     * DFS without creating a cycle; equivalently, removing back edges (to a dominator) leaves a DAG. */
    static int color[C2_MAX_BLOCKS], stk[C2_MAX_BLOCKS], idx[C2_MAX_BLOCKS];
    memset(color, 0, sizeof color);
    int sp = 0;
    stk[sp] = 0; idx[sp] = 0; sp++;
    color[0] = 1;
    while (sp) {
        int b = stk[sp - 1];
        if (idx[sp - 1] < x->nsucc[b]) {
            int s = x->succ[b][idx[sp - 1]++];
            if (color[s] == 1 && !dominates(x, s, b)) /* GUARD:reducible */
                return fail(x->v, "C2-CFG-IRREDUCIBLE", x->fi, -1, "cycle entered other than through its header (block %d)", s);
            if (color[s] == 0) { color[s] = 1; stk[sp] = s; idx[sp] = 0; sp++; }
        } else {
            color[b] = 2;
            sp--;
        }
    }
    /* natural loops, one per header (all its back edges together) */
    for (int h = 0; h < f->nblocks; h++) {
        if (!x->reach[h]) continue;
        int latch[C2_MAX_BLOCKS], nlatch = 0;
        for (int p = 0; p < f->nblocks; p++) {
            if (!x->reach[p]) continue;
            for (int k = 0; k < x->nsucc[p]; k++)
                if (x->succ[p][k] == h && dominates(x, h, p)) latch[nlatch++] = p;
        }
        if (!nlatch) continue;
        static int inloop[C2_MAX_BLOCKS];
        memset(inloop, 0, sizeof inloop);
        inloop[h] = 1;
        int st[C2_MAX_BLOCKS], n = 0;
        for (int k = 0; k < nlatch; k++)
            if (!inloop[latch[k]]) { inloop[latch[k]] = 1; st[n++] = latch[k]; }
        while (n) {
            int b = st[--n];
            for (int p = 0; p < f->nblocks; p++) {
                if (!x->reach[p] || inloop[p]) continue;
                for (int k = 0; k < x->nsucc[p]; k++)
                    if (x->succ[p][k] == b) { inloop[p] = 1; st[n++] = p; break; }
            }
        }
        if (!loop_guarded(x, inloop, latch, nlatch)) /* GUARD:loop-bound */
            return fail(x->v, "C2-LOOP-UNBOUNDED", x->fi, (int)f->blk[h].first, "loop at block %d has no counter guard", h);
    }
    return 0;
}

/* ------------------------------------------------------------------ ownership flow */

/* May-states of a reference-owner register: bit 0 not yet holding, bit 1 live, bit 2 dead
 * (released, moved or its arena destroyed). Handles: bit 0 not opened, bit 1 open, bit 2 closed.
 * Only a state that excludes "live" on every path is an error. */
enum { ST_U = 1, ST_L = 2, ST_D = 4 };

typedef struct {
    int arena_obj[C2_MAX_VREGS];   /* defined by AALLOC: its handle register, else -1 */
    int origin[C2_MAX_VREGS];      /* borrow -> root owner register; -1 none/external; -2 mixed */
    uint8_t in[C2_MAX_BLOCKS][C2_MAX_VREGS];
} Own;

static int tracked(const Fx *x, int r)
{
    const C2Type *t = &x->f->vt[r];
    return (t->s == S_REF && t->kind == K_OWN);
}

#define OF(rule, ...) do { if (!x->quiet) return fail(x->v, rule, x->fi, (int)i, __VA_ARGS__); } while (0)

static int own_use(const Fx *x, const Own *o, const uint8_t *st, uint32_t i, int r)
{
    if (tracked(x, r) && !(st[r] & ST_L)) /* GUARD:own-use-dead */
        OF("C2-OWN-USE-DEAD", "owner v%d used after it was released or moved (every path)", r);
    int g = o->origin[r];
    if (g >= 0 && !(st[g] & ST_L)) /* GUARD:borrow-outlives */
        OF("C2-BORROW-OUTLIVES-OWNER", "borrow v%d used after its owner v%d is gone (every path)", r, g);
    return 0;
}

static int own_kill(const Fx *x, uint8_t *st, uint32_t i, int r, const char *what)
{
    (void)what;
    if (!(st[r] & ST_L)) OF("C2-OWN-USE-DEAD", "owner v%d moved after it was released or moved (every path)", r); /* GUARD:own-move-dead */
    st[r] = ST_D;
    return 0;
}

static int own_birth(const Fx *x, uint8_t *st, uint32_t i, int r)
{
    if (st[r] == ST_L) /* GUARD:own-overwrite */
        OF("C2-OWN-LEAK", "owner v%d overwritten while it still holds an object (every path)", r);
    st[r] = ST_L;
    return 0;
}

static int own_step(const Fx *x, const Own *o, uint8_t *st, uint32_t i)
{
    const C2Func *f = x->f;
    const C2Insn *in = &f->in[i];
    switch (in->op) {
    case C2_ALLOC:
        return own_birth(x, st, i, in->dst);
    case C2_AALLOC:
        if (!(st[in->b] & ST_L)) OF("C2-ARENA-DEAD", "allocation from arena v%d after it was destroyed (every path)", in->b); /* GUARD:arena-alloc-dead */
        return own_birth(x, st, i, in->dst);
    case C2_RELEASE:
        if (o->arena_obj[in->a] >= 0) OF("C2-OWN-RELEASE-ARENA", "release of arena object v%d", in->a); /* GUARD:release-arena */
        if (!(st[in->a] & ST_L)) OF("C2-OWN-DOUBLE-RELEASE", "v%d released when it holds nothing (every path)", in->a); /* GUARD:double-release */
        st[in->a] = ST_D;
        return 0;
    case C2_MOV:
        if (tracked(x, in->a)) {
            if (f->vt[in->dst].kind == K_OWN) {
                if (o->arena_obj[in->a] >= 0 || o->arena_obj[in->dst] >= 0)
                    OF("C2-OWN-ARENA-MOVE", "move involving arena object"); /* GUARD:arena-move */
                if (own_kill(x, st, i, in->a, "move")) return -1;
                return own_birth(x, st, i, in->dst);
            }
            return own_use(x, o, st, i, in->a);
        }
        return is_ref(&f->vt[in->a]) ? own_use(x, o, st, i, in->a) : 0;
    case C2_LOAD: case C2_STORE: case C2_FLOAD: case C2_FSTORE:
        return own_use(x, o, st, i, in->a);
    case C2_CALL: {
        const C2Func *g = &x->u->fn[in->callee];
        for (int k = 0; k < in->nargs; k++) {
            int r = in->args[k];
            if (!is_ref(&f->vt[r])) continue;
            if (own_use(x, o, st, i, r)) return -1;
            if (g->vt[k].kind == K_OWN) {
                if (o->arena_obj[r] >= 0) OF("C2-OWN-ARENA-MOVE", "arena object v%d passed to an own parameter", r);
                if (own_kill(x, st, i, r, "move into call")) return -1;
            }
        }
        return 0;
    }
    case C2_AOPEN: case C2_POPEN:
        if (st[in->dst] & ST_L) OF("C2-HANDLE-REOPEN", "handle v%d reopened while open", in->dst);
        st[in->dst] = ST_L;
        return 0;
    case C2_ADESTROY: case C2_PCLOSE:
        if (!(st[in->a] & ST_L)) OF("C2-HANDLE-DOUBLE-CLOSE", "handle v%d closed when not open (every path)", in->a); /* GUARD:double-close */
        st[in->a] = ST_D;
        if (in->op == C2_ADESTROY)
            for (int r = 0; r < f->nvregs; r++)
                if (o->arena_obj[r] == in->a && st[r] != ST_U) st[r] = ST_D;
        return 0;
    case C2_HALLOC: case C2_HGEN: case C2_HFREE: case C2_HLOAD: case C2_HSTORE: {
        int h = in->op == C2_HALLOC ? in->b : in->a;
        if (!(st[h] & ST_L)) OF("C2-HANDLE-DEAD", "pool v%d used when not open (every path)", h); /* GUARD:pool-dead */
        return 0;
    }
    case C2_RET:
        for (int r = 0; r < f->nvregs; r++) {
            int handle = x->def_insn[r] >= 0 && (f->in[x->def_insn[r]].op == C2_AOPEN || f->in[x->def_insn[r]].op == C2_POPEN);
            if (st[r] == ST_L && (handle || (tracked(x, r) && o->arena_obj[r] < 0))) /* GUARD:leak */
                OF(handle ? "C2-HANDLE-LEAK" : "C2-OWN-LEAK", "v%d still %s at return (every path)", r, handle ? "open" : "owns an object");
        }
        return 0;
    default:
        return 0;
    }
}

static int check_ownership(Fx *x)
{
    const C2Func *f = x->f;
    Own *o = malloc(sizeof *o);
    if (!o) return fail(x->v, "C2-RESOURCE", x->fi, -1, "out of memory");
    for (int r = 0; r < f->nvregs; r++) { o->arena_obj[r] = -1; o->origin[r] = -1; }
    for (uint32_t i = 0; i < f->ninsns; i++) {
        const C2Insn *in = &f->in[i];
        if (in->op == C2_AALLOC) o->arena_obj[in->dst] = in->b;
    }
    for (uint32_t i = 0; i < f->ninsns; i++) {
        const C2Insn *in = &f->in[i];
        if ((in->op == C2_ALLOC && o->arena_obj[in->dst] >= 0) ||
            (in->op == C2_AALLOC && o->arena_obj[in->dst] != in->b)) {
            free(o);
            return fail(x->v, "C2-OWN-ARENA-MOVE", x->fi, (int)i, "register v%d holds both arena and pool objects", in->dst);
        }
    }
    /* borrow origins: flow-insensitive, single root owner or unknown */
    int changed = 1;
    while (changed) {
        changed = 0;
        for (uint32_t i = 0; i < f->ninsns; i++) {
            const C2Insn *in = &f->in[i];
            if (in->op != C2_MOV || !is_ref(&f->vt[in->dst]) || f->vt[in->dst].kind == K_OWN) continue;
            int root = f->vt[in->a].kind == K_OWN ? in->a : o->origin[in->a];
            int cur = o->origin[in->dst];
            int nw = cur == -1 && root >= 0 ? root : (cur == root ? cur : -2);
            if (in->dst < f->nparams) nw = -2;
            if (nw != cur && cur != -2) { o->origin[in->dst] = nw; changed = 1; }
        }
    }
    for (int r = 0; r < f->nvregs; r++)
        if (o->origin[r] == -2) o->origin[r] = -1;

    /* Fixpoint of the may-states. In-states only grow (bitwise OR over a finite lattice), so this
     * terminates. Transfer runs quietly (effects applied, no verdict); a TRAP ends its path, so a
     * trapping block propagates nothing. Blocks no non-trapping path reaches are not judged. */
    static int live[C2_MAX_BLOCKS];
    memset(o->in, 0, sizeof o->in);
    memset(live, 0, sizeof live);
    for (int r = 0; r < f->nvregs; r++) o->in[0][r] = (r < f->nparams && tracked(x, r)) ? ST_L : ST_U;
    live[0] = 1;
    x->quiet = 1;
    changed = 1;
    while (changed) {
        changed = 0;
        for (int b = 0; b < f->nblocks; b++) {
            if (!live[b]) continue;
            uint8_t st[C2_MAX_VREGS];
            memcpy(st, o->in[b], f->nvregs);
            int trapped = 0;
            for (uint32_t i = f->blk[b].first; i < f->blk[b].first + f->blk[b].count && !trapped; i++) {
                if (f->in[i].op == C2_TRAP) trapped = 1;
                else (void)own_step(x, o, st, i);
            }
            if (trapped) continue;
            for (int k = 0; k < x->nsucc[b]; k++) {
                int s = x->succ[b][k];
                if (!live[s]) { live[s] = 1; changed = 1; }
                for (int r = 0; r < f->nvregs; r++) {
                    uint8_t m = o->in[s][r] | st[r];
                    if (m != o->in[s][r]) { o->in[s][r] = m; changed = 1; }
                }
            }
        }
    }
    x->quiet = 0;
    /* report the first definite error */
    for (int b = 0; b < f->nblocks; b++) {
        if (!live[b]) continue;
        uint8_t st[C2_MAX_VREGS];
        memcpy(st, o->in[b], f->nvregs);
        for (uint32_t i = f->blk[b].first; i < f->blk[b].first + f->blk[b].count; i++) {
            if (f->in[i].op == C2_TRAP) break;
            if (own_step(x, o, st, i)) { free(o); return -1; }
        }
    }
    free(o);
    return 0;
}

/* ------------------------------------------------------------------ driver */

static int check_func(const C2Unit *u, int fi, C2Verdict *v)
{
    const C2Func *f = &u->fn[fi];
    for (int g = 0; g < fi; g++)
        if (strcmp(u->fn[g].name, f->name) == 0) return fail(v, "C2-FUNC-NAME", fi, -1, "function %s defined twice", f->name); /* GUARD:func-name */
    if (f->nparams > C2_MAX_PARAMS || f->nparams > f->nvregs) /* GUARD:params */
        return fail(v, "C2-FUNC-PARAMS", fi, -1, "%u parameters, %u registers", f->nparams, f->nvregs);
    if (!(f->ret.s == S_VOID || is_value(f->ret.s))) /* GUARD:ret-scalar */
        return fail(v, "C2-FUNC-RETURN", fi, -1, "function returns a non-scalar");
    for (int r = 0; r < f->nvregs; r++) {
        const C2Type *t = &f->vt[r];
        if (!check_type(u, t)) return fail(v, "C2-TYPE-INVALID", fi, -1, "register v%d has an invalid type", r); /* GUARD:vreg-type */
        if (is_slice(t)) {
            if (r + 1 >= f->nparams || f->vt[r + 1].s != S_U64) /* GUARD:slice-param */
                return fail(v, "C2-SLICE-PARAM", fi, -1, "slice register v%d is not a parameter followed by its u64 length", r);
            if (r > 0 && is_slice(&f->vt[r - 1]))
                return fail(v, "C2-SLICE-PARAM", fi, -1, "slice v%d sits in the length register of v%d", r, r - 1);
        }
    }
    Fx *x = calloc(1, sizeof *x);
    if (!x) return fail(v, "C2-RESOURCE", fi, -1, "out of memory");
    x->u = u; x->f = f; x->fi = fi; x->v = v; x->nb = f->nblocks;
    x->blk_of = calloc(f->ninsns, sizeof *x->blk_of);
    int rc = -1;
    if (!x->blk_of) { fail(v, "C2-RESOURCE", fi, -1, "out of memory"); goto out; }
    if (build_cfg(x)) goto out;
    for (uint32_t i = 0; i < f->ninsns; i++)
        if (check_insn(x, i)) goto out;
    link_cfg(x);
    count_defs(x);
    if (check_handles(x)) goto out;
    if (check_defined(x)) goto out;
    if (check_loops(x)) goto out;
    if (check_ownership(x)) goto out;
    rc = 0;
out:
    free(x->blk_of);
    free(x);
    return rc;
}

int c2_check_unit(const C2Unit *u, C2Verdict *v)
{
    if (v) { memset(v, 0, sizeof *v); v->ok = 1; v->func = v->insn = -1; v->rule = "ACCEPT"; }
    if (check_structs(u, v)) return -1;
    for (int fi = 0; fi < u->nfuncs; fi++)
        if (check_func(u, fi, v)) return -1;
    return 0;
}
