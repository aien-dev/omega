/*
 * osc_lower.c -- see osc_lower.h.
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 */
#include "osc_lower.h"
#include "osc_check.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const OscAst *ast;
    OscDiag *d;
    OscFunc *f;
    int fnode;
    /* unordered instruction buffer, tagged with the owning block */
    OscInsn ins[OSC_MAX_INSNS];
    int16_t iblk[OSC_MAX_INSNS];
    uint32_t nins;
    uint8_t closed[OSC_MAX_BLOCKS];
    int nblk;
    int cur;                 /* current block or -1 (dead) */
    int16_t vreg[OSC_CHECK_MAX_SYMS];
    int failed;
    int fi;                  /* function being lowered */
    /* OSC-3 item 2: symbol kinds the lowerer must tell apart (reset per
     * function). A pool symbol's vreg is its u64 pool id; a handle symbol's
     * vreg r is its slot and r + 1 its generation. */
    uint8_t ispool[OSC_CHECK_MAX_SYMS];
    uint8_t ishandle[OSC_CHECK_MAX_SYMS];
    /* OSC-3 item 3: drop-flag vreg (bool) of an owner symbol whose
     * declaration the checker marked (dflag), -1 otherwise. 1 = live,
     * 0 = moved. Set at the declaration, cleared at every move, tested by
     * the owner's scope-end release (CBR around RELEASE). */
    int16_t dfv[OSC_CHECK_MAX_SYMS];
} L;

#define NODE(i) (&l->ast->nodes[(i)])

static void lcap(L *l, uint32_t line, const char *what)
{
    if (l->failed) return;
    l->failed = 1;
    osc_diag_set(l->d, OSC_DIAG_CAPACITY, line, 0, l->f->name, NODE(l->fnode)->line, NULL, what,
                 "function '%s' exceeds the IR %s", l->f->name, what);
}

static int newv(L *l, const OscType *t, uint32_t line)
{
    if (l->f->nvregs >= OSC_MAX_VREGS) { lcap(l, line, "vreg limit"); return 0; }
    l->f->vtype[l->f->nvregs] = *t;
    return l->f->nvregs++;
}
static int newvs(L *l, OscScalar s, uint32_t line)
{
    OscType t = {s, OSC_REF_NONE, OSC_T_VOID, 0, 0};
    return newv(l, &t, line);
}

static int newblk(L *l, uint32_t line)
{
    if (l->nblk >= OSC_MAX_BLOCKS) { lcap(l, line, "block limit"); return 0; }
    l->closed[l->nblk] = 0;
    return l->nblk++;
}

static OscInsn *emit(L *l, uint8_t op, uint32_t line)
{
    static OscInsn sink;
    if (l->cur < 0 || l->failed) { memset(&sink, 0, sizeof sink); return &sink; }
    if (l->nins >= OSC_MAX_INSNS) { lcap(l, line, "instruction limit"); memset(&sink, 0, sizeof sink); return &sink; }
    OscInsn *x = &l->ins[l->nins];
    l->iblk[l->nins] = (int16_t)l->cur;
    l->nins++;
    memset(x, 0, sizeof *x);
    x->op = op;
    x->dst = x->a = x->b = x->c = x->blk_t = x->blk_f = x->callee = -1;
    x->line = line;
    if (op == OSC_I_BR || op == OSC_I_CBR || op == OSC_I_RET) {
        l->closed[l->cur] = 1;
        l->cur = -1;
    }
    return x;
}

static void br(L *l, int target, uint32_t line)
{
    if (l->cur < 0) return;
    OscInsn *x = emit(l, OSC_I_BR, line);
    x->blk_t = (int16_t)target;
}
static void cbr(L *l, int cond, int t, int f, uint32_t line)
{
    if (l->cur < 0) return;
    OscInsn *x = emit(l, OSC_I_CBR, line);
    x->a = (int16_t)cond;
    x->blk_t = (int16_t)t;
    x->blk_f = (int16_t)f;
}
static int kconst(L *l, OscScalar s, uint64_t v, uint32_t line)
{
    int r = newvs(l, s, line);
    OscInsn *x = emit(l, OSC_I_CONST, line);
    x->dst = (int16_t)r;
    x->imm = v;
    return r;
}

/* OSC-3 item 3 drop flags: write imm (1 live / 0 moved) into owner s's flag */
static void dflag_set(L *l, int s, uint64_t imm, uint32_t line)
{
    if (s < 0 || l->dfv[s] < 0) return;
    OscInsn *x = emit(l, OSC_I_CONST, line);
    x->dst = l->dfv[s];
    x->imm = imm;
}
/* owner s just declared by node n: give it a flag (set live) if marked */
static void dflag_decl(L *l, int s, const OscNode *n)
{
    if (!n->dflag) return;
    l->dfv[s] = (int16_t)newvs(l, OSC_T_BOOL, n->line);
    dflag_set(l, s, 1, n->line);
}

static void releases(L *l, const OscNode *n)
{
    if (l->cur < 0) return; /* dead code: nothing to release (emit would drop it) */
    for (uint32_t k = 0; k < n->rel_count; k++) {
        int s = l->ast->rel[n->rel_start + k];
        /* OSC-2 arenas: an arena symbol's vreg is its u64 handle (owners are REFs) */
        int arena = l->f->vtype[l->vreg[s]].s != OSC_T_REF;
        int guard = !l->ispool[s] && !arena && l->dfv[s] >= 0 ? l->dfv[s] : -1;
        int rb = -1, jb = -1;
        if (guard >= 0) { /* OSC-3 item 3: release only if the drop flag says live */
            rb = newblk(l, n->line);
            jb = newblk(l, n->line);
            cbr(l, guard, rb, jb, n->line);
            l->cur = rb;
        }
        OscInsn *x = emit(l, l->ispool[s] ? OSC_I_PCLOSE : arena ? OSC_I_ADESTROY : OSC_I_RELEASE, n->line);
        x->a = l->vreg[s];
        if (guard >= 0) {
            br(l, jb, n->line);
            l->cur = jb;
        }
    }
}

static int bop(int tok)
{
    switch (tok) {
    case OT_PLUS: return OSC_B_ADD;
    case OT_MINUS: return OSC_B_SUB;
    case OT_STAR: return OSC_B_MUL;
    case OT_SLASH: return OSC_B_DIV;
    case OT_PERCENT: return OSC_B_REM;
    case OT_AMP: return OSC_B_AND;
    case OT_PIPE: return OSC_B_OR;
    case OT_CARET: return OSC_B_XOR;
    case OT_SHL: return OSC_B_SHL;
    case OT_SHR: return OSC_B_SHR;
    default: return 0;
    }
}
static int cop(int tok)
{
    switch (tok) {
    case OT_EQ: return OSC_C_EQ;
    case OT_NE: return OSC_C_NE;
    case OT_LT: return OSC_C_LT;
    case OT_LE: return OSC_C_LE;
    case OT_GT: return OSC_C_GT;
    case OT_GE: return OSC_C_GE;
    default: return 0;
    }
}

static int expr(L *l, int i);

static int call(L *l, int i, int want_value)
{
    const OscNode *n = NODE(i);
    int args[OSC_MAX_PARAMS], na = 0;
    for (int a = n->a; a >= 0 && na < OSC_MAX_PARAMS; a = NODE(a)->next) {
        const OscNode *an = NODE(a);
        if (an->kind == ON_BORROW) args[na++] = l->vreg[an->sym];
        else if (an->kind == ON_NAME && an->sym >= 0 && l->f->vtype[l->vreg[an->sym]].s == OSC_T_REF)
            args[na++] = l->vreg[an->sym];  /* own argument: moves the owner */
        else args[na++] = expr(l, a);
    }
    /* OSC-3 item 3: an owner passed to an own parameter is moved here */
    for (int a = n->a; a >= 0; a = NODE(a)->next) {
        const OscNode *an = NODE(a);
        if (an->kind == ON_NAME && an->sym >= 0 && an->sym < OSC_CHECK_MAX_SYMS && l->dfv[an->sym] >= 0)
            dflag_set(l, an->sym, 0, n->line);
    }
    const OscNode *g = NODE(l->ast->fns[n->sym]);
    int dst = -1;
    if (g->ty.s != OSC_T_VOID && want_value) dst = newvs(l, g->ty.s, n->line);
    else if (g->ty.s != OSC_T_VOID) dst = newvs(l, g->ty.s, n->line); /* result discarded */
    OscInsn *x = emit(l, OSC_I_CALL, n->line);
    x->dst = (int16_t)dst;
    x->callee = (int16_t)n->sym;
    x->nargs = (uint8_t)na;
    for (int k = 0; k < na; k++) x->args[k] = (int16_t)args[k];
    return dst;
}

static int expr(L *l, int i)
{
    const OscNode *n = NODE(i);
    if (n->is_const) return kconst(l, n->ty.s, n->cval, n->line);
    switch (n->kind) {
    case ON_BOOL: return kconst(l, OSC_T_BOOL, n->ival ? 1 : 0, n->line);
    case ON_INT: return kconst(l, n->ty.s, n->cval, n->line);
    case ON_NAME: return l->vreg[n->sym];
    case ON_HLOAD: { /* OSC-3 item 2: P[h] */
        int r = newvs(l, n->ty.s, n->line);
        OscInsn *x = emit(l, OSC_I_HLOAD, n->line);
        x->dst = (int16_t)r;
        x->a = l->vreg[n->sym];
        x->b = l->vreg[n->sym2];
        x->c = (int16_t)(l->vreg[n->sym2] + 1);
        return r;
    }
    case ON_INDEX: {
        int ix = expr(l, n->a);
        int r = newvs(l, n->ty.s, n->line);
        OscInsn *x = emit(l, OSC_I_LOAD, n->line);
        x->dst = (int16_t)r;
        x->a = l->vreg[n->sym];
        x->b = (int16_t)ix;
        return r;
    }
    case ON_FIELD: {
        int ix = n->a >= 0 ? expr(l, n->a) : -1;
        int r = newvs(l, n->ty.s, n->line);
        OscInsn *x = emit(l, OSC_I_FLOAD, n->line);
        x->dst = (int16_t)r;
        x->a = l->vreg[n->sym];
        x->b = (int16_t)ix;
        x->imm = (uint64_t)n->hi;
        return r;
    }
    case ON_CALL: return call(l, i, 1);
    case ON_CAST: {
        int a = expr(l, n->a);
        int r = newvs(l, n->ty.s, n->line);
        OscInsn *x = emit(l, OSC_I_CAST, n->line);
        x->dst = (int16_t)r;
        x->a = (int16_t)a;
        return r;
    }
    case ON_UN: {
        int a = expr(l, n->a);
        int r = newvs(l, n->ty.s, n->line);
        OscInsn *x = emit(l, OSC_I_UN, n->line);
        x->sub = n->op == OT_MINUS ? OSC_U_NEG : n->op == OT_TILDE ? OSC_U_BNOT : OSC_U_LNOT;
        x->dst = (int16_t)r;
        x->a = (int16_t)a;
        return r;
    }
    case ON_BIN: {
        if (n->op == OT_ANDAND || n->op == OT_OROR) {
            int res = newvs(l, OSC_T_BOOL, n->line);
            int a = expr(l, n->a);
            OscInsn *m = emit(l, OSC_I_MOV, n->line);
            m->dst = (int16_t)res;
            m->a = (int16_t)a;
            int bb = newblk(l, n->line), jb = newblk(l, n->line);
            if (n->op == OT_ANDAND) cbr(l, a, bb, jb, n->line);
            else cbr(l, a, jb, bb, n->line);
            l->cur = bb;
            int b = expr(l, n->b);
            m = emit(l, OSC_I_MOV, n->line);
            m->dst = (int16_t)res;
            m->a = (int16_t)b;
            br(l, jb, n->line);
            l->cur = jb;
            return res;
        }
        int a = expr(l, n->a);
        int b = expr(l, n->b);
        int r = newvs(l, n->ty.s, n->line);
        int cc = cop(n->op);
        OscInsn *x = emit(l, cc ? OSC_I_CMP : OSC_I_BIN, n->line);
        x->sub = (uint8_t)(cc ? cc : bop(n->op));
        x->dst = (int16_t)r;
        x->a = (int16_t)a;
        x->b = (int16_t)b;
        return r;
    }
    default:
        lcap(l, n->line, "lowering (unexpected expression)");
        return 0;
    }
}

/* OSC-2 contract check: evaluate clause (bool) with the checked semantics;
 * false -> TRAP code (docs/osc/OSC-2-DESIGN.md section 1.5). */
static void contract_check(L *l, int clause, unsigned code)
{
    uint32_t line = NODE(clause)->line;
    int c = expr(l, clause);
    int ok = newblk(l, line), bad = newblk(l, line);
    cbr(l, c, ok, bad, line);
    l->cur = bad;
    OscInsn *x = emit(l, OSC_I_TRAP, line);
    x->imm = code;
    br(l, ok, line);
    l->cur = ok;
}

/* ensures at a return whose value is in vreg v (-1 for void) */
static void ensures_check(L *l, int v)
{
    const OscAst *a = l->ast;
    if (a->ensn[l->fi] < 0 || a->ens_elide[l->fi]) return;
    if (a->res_sym[l->fi] >= 0) l->vreg[a->res_sym[l->fi]] = (int16_t)v;
    contract_check(l, a->ensn[l->fi], OSC_TRAP_ENSURES);
}

/* OSC-3 item 2: the (slot, gen) vregs of handle value i (ON_PALLOC allocates
 * from its pool: HALLOC then HGEN; ON_NAME is a copy of another handle).
 * Returns the slot vreg; *gen gets the generation vreg. */
static int hval(L *l, int i, int *gen)
{
    const OscNode *v = NODE(i);
    if (v->kind == ON_PALLOC) {
        int init = expr(l, v->a);
        int pool = l->vreg[v->sym];
        int s = newvs(l, OSC_T_U64, v->line);
        int g = newvs(l, OSC_T_U64, v->line);
        OscInsn *x = emit(l, OSC_I_HALLOC, v->line);
        x->dst = (int16_t)s;
        x->a = (int16_t)init;
        x->b = (int16_t)pool;
        x = emit(l, OSC_I_HGEN, v->line);
        x->dst = (int16_t)g;
        x->a = (int16_t)pool;
        x->b = (int16_t)s;
        *gen = g;
        return s;
    }
    int r = l->vreg[v->sym];
    *gen = r + 1;
    return r;
}

/* bind handle symbol s (vregs r, r + 1) to the value of node i */
static void hbind(L *l, int s, int i, uint32_t line)
{
    int g, v = hval(l, i, &g);
    OscInsn *x = emit(l, OSC_I_MOV, line);
    x->dst = l->vreg[s];
    x->a = (int16_t)v;
    x = emit(l, OSC_I_MOV, line);
    x->dst = (int16_t)(l->vreg[s] + 1);
    x->a = (int16_t)g;
}

static void block(L *l, int i);

static void stmt(L *l, int i)
{
    const OscNode *n = NODE(i);
    switch (n->kind) {
    case ON_LET: {
        int v = expr(l, n->a);
        int r = newv(l, &n->ty, n->line);
        l->vreg[n->sym] = (int16_t)r;
        OscInsn *x = emit(l, OSC_I_MOV, n->line);
        x->dst = (int16_t)r;
        x->a = (int16_t)v;
        break;
    }
    case ON_LET_ALLOC: {
        /* OSC-2 arenas: "in NAME" allocates from that arena's handle */
        int ah = n->c >= 0 ? l->vreg[NODE(n->c)->sym] : -1;
        if (n->ty.sid) {
            /* struct literal: field values in source order, then one
             * allocation (cells zeroed), then one FSTORE per cell */
            int vals[OSC_MAX_FIELDS], nv = 0;
            for (int fi = n->a; fi >= 0; fi = NODE(fi)->next) vals[nv++] = expr(l, NODE(fi)->a);
            int zero = kconst(l, OSC_T_U64, 0, n->line);
            int r = newv(l, &n->ty, n->line);
            l->vreg[n->sym] = (int16_t)r;
            OscInsn *x = emit(l, ah >= 0 ? OSC_I_AALLOC : OSC_I_ALLOC, n->line);
            if (ah >= 0) x->b = (int16_t)ah;
            x->dst = (int16_t)r;
            x->a = (int16_t)zero;
            int ix = -1, k = 0;
            for (int fi = n->a; fi >= 0; fi = NODE(fi)->next, k++) {
                const OscNode *fn = NODE(fi);
                unsigned cnt = fn->flag ? (unsigned)fn->ival : 1;
                for (unsigned e = 0; e < cnt; e++) {
                    if (fn->flag) {
                        if (ix < 0) ix = newvs(l, OSC_T_U64, n->line);
                        OscInsn *c = emit(l, OSC_I_CONST, fn->line);
                        c->dst = (int16_t)ix;
                        c->imm = e;
                    }
                    x = emit(l, OSC_I_FSTORE, fn->line);
                    x->a = (int16_t)r;
                    x->b = (int16_t)(fn->flag ? ix : -1);
                    x->c = (int16_t)vals[k];
                    x->imm = (uint64_t)fn->hi;
                }
            }
            dflag_decl(l, n->sym, n); /* OSC-3 item 3 */
            break;
        }
        int v = expr(l, n->a);
        int r = newv(l, &n->ty, n->line);
        l->vreg[n->sym] = (int16_t)r;
        OscInsn *x = emit(l, ah >= 0 ? OSC_I_AALLOC : OSC_I_ALLOC, n->line);
        if (ah >= 0) x->b = (int16_t)ah;
        x->dst = (int16_t)r;
        x->a = (int16_t)v;
        dflag_decl(l, n->sym, n); /* OSC-3 item 3 */
        break;
    }
    case ON_LET_MOVE:
    case ON_LET_BORROW: {
        int r = newv(l, &n->ty, n->line);
        l->vreg[n->sym] = (int16_t)r;
        OscInsn *x = emit(l, OSC_I_MOV, n->line);
        x->dst = (int16_t)r;
        x->a = l->vreg[n->sym2];
        if (n->kind == ON_LET_MOVE) { /* OSC-3 item 3: the source is moved; the new owner starts live */
            dflag_set(l, n->sym2, 0, n->line);
            dflag_decl(l, n->sym, n);
        }
        break;
    }
    case ON_ASSIGN: {
        if (l->ishandle[n->sym]) { hbind(l, n->sym, n->a, n->line); break; }
        const OscNode *v = NODE(n->a);
        int src = v->kind == ON_BORROW ? l->vreg[n->sym2] : expr(l, n->a);
        OscInsn *x = emit(l, OSC_I_MOV, n->line);
        x->dst = l->vreg[n->sym];
        x->a = (int16_t)src;
        break;
    }
    case ON_STORE: {
        int ix = expr(l, n->a);
        int v = expr(l, n->b);
        OscInsn *x = emit(l, OSC_I_STORE, n->line);
        x->a = l->vreg[n->sym];
        x->b = (int16_t)ix;
        x->c = (int16_t)v;
        break;
    }
    case ON_FSTORE: {
        int ix = n->a >= 0 ? expr(l, n->a) : -1;
        int v = expr(l, n->b);
        OscInsn *x = emit(l, OSC_I_FSTORE, n->line);
        x->a = l->vreg[n->sym];
        x->b = (int16_t)ix;
        x->c = (int16_t)v;
        x->imm = (uint64_t)n->hi;
        break;
    }
    case ON_IF: {
        int c = expr(l, n->a);
        int tb = newblk(l, n->line);
        int eb = -1, jb = -1;
        if (n->c >= 0) eb = newblk(l, n->line);
        else jb = newblk(l, n->line);
        cbr(l, c, tb, eb >= 0 ? eb : jb, n->line);
        l->cur = tb;
        block(l, n->b);
        int then_open = l->cur;
        int else_open = -1;
        if (n->c >= 0) {
            l->cur = eb;
            if (NODE(n->c)->kind == ON_IF) stmt(l, n->c);
            else block(l, n->c);
            else_open = l->cur;
        }
        if (n->c < 0) {
            if (then_open >= 0) { l->cur = then_open; br(l, jb, n->line); }
            l->cur = jb;
        } else if (then_open >= 0 || else_open >= 0) {
            if (jb < 0) jb = newblk(l, n->line);
            if (then_open >= 0) { l->cur = then_open; br(l, jb, n->line); }
            if (else_open >= 0) { l->cur = else_open; br(l, jb, n->line); }
            l->cur = jb;
        } else {
            l->cur = -1;
        }
        break;
    }
    case ON_WHILE: {
        int cnt = newvs(l, OSC_T_U64, n->line);
        OscInsn *x = emit(l, OSC_I_CONST, n->line);
        x->dst = (int16_t)cnt;
        x->imm = 0;
        int lim = kconst(l, OSC_T_U64, n->ival, n->line);
        int one = kconst(l, OSC_T_U64, 1, n->line);
        int head = newblk(l, n->line), chk = newblk(l, n->line), trap = newblk(l, n->line);
        int body = newblk(l, n->line), exit = newblk(l, n->line);
        br(l, head, n->line);
        l->cur = head;
        int c = expr(l, n->a);
        cbr(l, c, chk, exit, n->line);
        l->cur = chk;
        int ge = newvs(l, OSC_T_BOOL, n->line);
        x = emit(l, OSC_I_CMP, n->line);
        x->sub = OSC_C_GE;
        x->dst = (int16_t)ge;
        x->a = (int16_t)cnt;
        x->b = (int16_t)lim;
        cbr(l, ge, trap, body, n->line);
        l->cur = trap;
        x = emit(l, OSC_I_TRAP, n->line);
        x->imm = OSC_TRAP_LOOP_BOUND;
        br(l, exit, n->line);
        l->cur = body;
        x = emit(l, OSC_I_BIN, n->line);
        x->sub = OSC_B_ADD;
        x->dst = (int16_t)cnt;
        x->a = (int16_t)cnt;
        x->b = (int16_t)one;
        block(l, n->b);
        br(l, head, n->line);
        l->cur = exit;
        break;
    }
    case ON_FOR: {
        int iv = newvs(l, OSC_T_I64, n->line);
        l->vreg[n->sym] = (int16_t)iv;
        OscInsn *x = emit(l, OSC_I_CONST, n->line);
        x->dst = (int16_t)iv;
        x->imm = (uint64_t)n->lo;
        int hi = kconst(l, OSC_T_I64, (uint64_t)n->hi, n->line);
        int one = kconst(l, OSC_T_I64, 1, n->line);
        int head = newblk(l, n->line), body = newblk(l, n->line), exit = newblk(l, n->line);
        br(l, head, n->line);
        l->cur = head;
        int lt = newvs(l, OSC_T_BOOL, n->line);
        x = emit(l, OSC_I_CMP, n->line);
        x->sub = OSC_C_LT;
        x->dst = (int16_t)lt;
        x->a = (int16_t)iv;
        x->b = (int16_t)hi;
        cbr(l, lt, body, exit, n->line);
        l->cur = body;
        block(l, n->b);
        x = emit(l, OSC_I_BIN, n->line);
        x->sub = OSC_B_ADD;
        x->dst = (int16_t)iv;
        x->a = (int16_t)iv;
        x->b = (int16_t)one;
        br(l, head, n->line);
        l->cur = exit;
        break;
    }
    case ON_RETURN: {
        int v = -1;
        if (n->a >= 0) v = expr(l, n->a);
        if (!n->flag) ensures_check(l, v); /* flag: checker proved it true here */
        releases(l, n);
        OscInsn *x = emit(l, OSC_I_RET, n->line);
        x->a = (int16_t)v;
        break;
    }
    case ON_CALLSTMT:
        call(l, n->a, 0);
        break;
    case ON_POOL: { /* OSC-3 item 2: POPEN, body, then (if live) PCLOSE */
        int p = newvs(l, OSC_T_U64, n->line);
        l->vreg[n->sym] = (int16_t)p;
        l->ispool[n->sym] = 1;
        OscInsn *x = emit(l, OSC_I_POPEN, n->line);
        x->dst = (int16_t)p;
        x->sub = (uint8_t)n->ty.s;
        x->nargs = (uint8_t)n->ival;
        x->imm = (uint64_t)n->lo;
        block(l, n->b);
        if (l->cur >= 0) releases(l, n);
        break;
    }
    case ON_LET_HANDLE: {
        int r = newvs(l, OSC_T_U64, n->line);
        int g = newvs(l, OSC_T_U64, n->line);
        if (g != r + 1) { lcap(l, n->line, "vreg limit"); break; }
        l->vreg[n->sym] = (int16_t)r;
        l->ishandle[n->sym] = 1;
        hbind(l, n->sym, n->a, n->line);
        break;
    }
    case ON_PFREE: {
        OscInsn *x = emit(l, OSC_I_HFREE, n->line);
        x->a = l->vreg[n->sym];
        x->b = l->vreg[n->sym2];
        x->c = (int16_t)(l->vreg[n->sym2] + 1);
        break;
    }
    case ON_HSTORE: {
        int v = expr(l, n->b);
        OscInsn *x = emit(l, OSC_I_HSTORE, n->line);
        x->a = l->vreg[n->sym];
        x->b = l->vreg[n->sym2];
        x->c = (int16_t)(l->vreg[n->sym2] + 1);
        x->args[0] = (int16_t)v;
        x->nargs = 1;
        break;
    }
    case ON_ARENA: { /* OSC-2: AOPEN, body, then (if live) the body's releases were
                      * emitted by block(); the arena's own list destroys it */
        int h = newvs(l, OSC_T_U64, n->line);
        l->vreg[n->sym] = (int16_t)h;
        OscInsn *x = emit(l, OSC_I_AOPEN, n->line);
        x->dst = (int16_t)h;
        x->imm = n->ival;
        block(l, n->b);
        if (l->cur >= 0) releases(l, n);
        break;
    }
    case ON_BLOCK:
        block(l, i);
        break;
    default:
        lcap(l, n->line, "lowering (unexpected statement)");
    }
}

static void block(L *l, int i)
{
    const OscNode *n = NODE(i);
    for (int s = n->a; s >= 0 && l->cur >= 0; s = NODE(s)->next) stmt(l, s);
    if (l->cur >= 0) releases(l, n);
}

static int lower_fn(L *l, int fi)
{
    const OscAst *a = l->ast;
    const OscNode *fn = NODE(a->fns[fi]);
    OscFunc *f = l->f;
    memset(f, 0, sizeof *f);
    l->fnode = a->fns[fi];
    osc_node_name(a, fn, f->name, sizeof f->name);
    memcpy(f->requires_text, a->req[fi], OSC_CLAUSE_MAX);
    memcpy(f->ensures_text, a->ens[fi], OSC_CLAUSE_MAX);
    f->requires_text[OSC_CLAUSE_MAX - 1] = 0;
    f->ensures_text[OSC_CLAUSE_MAX - 1] = 0;
    f->ret = fn->ty;
    l->nins = 0;
    l->nblk = 0;
    memset(l->ispool, 0, sizeof l->ispool);
    memset(l->ishandle, 0, sizeof l->ishandle);
    memset(l->dfv, 0xff, sizeof l->dfv); /* OSC-3 item 3: no drop flags */
    for (int p = fn->a; p >= 0; p = NODE(p)->next) {
        const OscNode *pn = NODE(p);
        int r = newv(l, &pn->ty, pn->line);
        l->vreg[pn->sym] = (int16_t)r;
        f->nparams++;
    }
    l->cur = newblk(l, fn->line);
    /* OSC-3 item 3: own parameters that may be moved on only some paths get
     * their drop flag (vregs after the parameters), set live at entry */
    for (int p = fn->a; p >= 0; p = NODE(p)->next) dflag_decl(l, NODE(p)->sym, NODE(p));
    l->fi = fi;
    if (a->reqn[fi] >= 0 && !a->req_elide[fi]) contract_check(l, a->reqn[fi], OSC_TRAP_REQUIRES);
    /* body block inlined (same emission as block()) so a void function's
     * fall-off ensures check precedes the body scope's releases */
    const OscNode *body = NODE(fn->b);
    for (int s = body->a; s >= 0 && l->cur >= 0; s = NODE(s)->next) stmt(l, s);
    if (l->cur >= 0) {
        ensures_check(l, -1);
        releases(l, body);
        releases(l, fn);
        OscInsn *x = emit(l, OSC_I_RET, (uint32_t)NODE(fn->b)->ival);
        x->a = -1;
    }
    if (l->failed) return -1;
    /* layout: blocks in creation order, instructions in emission order */
    uint32_t k = 0;
    for (int b = 0; b < l->nblk; b++) {
        f->blocks[b].first = k;
        for (uint32_t j = 0; j < l->nins; j++)
            if (l->iblk[j] == b) f->insns[k++] = l->ins[j];
        f->blocks[b].count = k - f->blocks[b].first;
    }
    f->ninsns = k;
    f->nblocks = (uint16_t)l->nblk;
    return 0;
}

int osc_lower(const OscAst *ast, OscUnit *out, OscDiag *d)
{
    L *l = calloc(1, sizeof *l);
    if (!l) {
        osc_diag_set(d, OSC_DIAG_CAPACITY, 0, 0, "unit", 0, NULL, "lowering memory", "out of memory");
        return -1;
    }
    memset(out, 0, sizeof *out);
    l->ast = ast;
    l->d = d;
    int rc = 0;
    for (uint32_t fi = 0; fi < ast->nfns && rc == 0; fi++) {
        l->f = &out->funcs[fi];
        rc = lower_fn(l, (int)fi);
    }
    out->nfuncs = (uint16_t)ast->nfns;
    memcpy(out->structs, ast->structs, sizeof out->structs);
    out->nstructs = ast->nstructs;
    free(l);
    return rc;
}
