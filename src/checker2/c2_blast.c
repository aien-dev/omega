/* c2_blast.c: symbolic execution of one IR function into CNF (see c2_blast.h). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c2_blast.h"

#define T 1
#define F (-1)
#define MAXW 128

typedef struct {
    C2Cnf *cnf;
    int nvars;
    int oom;
} Bl;

static int newvar(Bl *b) { return ++b->nvars; }
static void clause(Bl *b, int n, ...)
{
    int32_t l[4];
    va_list ap;
    va_start(ap, n);
    for (int k = 0; k < n; k++) l[k] = va_arg(ap, int);
    va_end(ap);
    if (c2_cnf_add(b->cnf, l, n)) b->oom = 1;
}

static int AND(Bl *b, int x, int y)
{
    if (x == F || y == F || x == -y) return F;
    if (x == T) return y;
    if (y == T || x == y) return x;
    int v = newvar(b);
    clause(b, 2, -v, x);
    clause(b, 2, -v, y);
    clause(b, 3, v, -x, -y);
    return v;
}
static int OR(Bl *b, int x, int y) { return -AND(b, -x, -y); }
static int XOR(Bl *b, int x, int y)
{
    if (x == F) return y;
    if (y == F) return x;
    if (x == T) return -y;
    if (y == T) return -x;
    if (x == y) return F;
    if (x == -y) return T;
    int v = newvar(b);
    clause(b, 3, -v, x, y);
    clause(b, 3, -v, -x, -y);
    clause(b, 3, v, -x, y);
    clause(b, 3, v, x, -y);
    return v;
}
static int MUX(Bl *b, int s, int x, int y) /* s ? x : y */
{
    if (s == T || x == y) return x;
    if (s == F) return y;
    return OR(b, AND(b, s, x), AND(b, -s, y));
}

/* ------------------------------------------------------------------ bit vectors */

static void add(Bl *b, const int *x, const int *y, int cin, int w, int *out, int *cout)
{
    int c = cin; /* out may alias x or y: read bit i of both before writing out[i] */
    for (int i = 0; i < w; i++) {
        int p = XOR(b, x[i], y[i]), g = AND(b, x[i], y[i]);
        out[i] = XOR(b, p, c);
        c = OR(b, g, AND(b, c, p));
    }
    if (cout) *cout = c;
}
static void neg_bits(const int *x, int w, int *o) { for (int i = 0; i < w; i++) o[i] = -x[i]; }
static int eq(Bl *b, const int *x, const int *y, int w)
{
    int r = T;
    for (int i = 0; i < w; i++) r = AND(b, r, -XOR(b, x[i], y[i]));
    return r;
}
static int ult(Bl *b, const int *x, const int *y, int w) /* x < y unsigned: no carry out of x + ~y + 1 */
{
    int ny[MAXW], s[MAXW], c;
    neg_bits(y, w, ny);
    add(b, x, ny, T, w, s, &c);
    return -c;
}
static int slt(Bl *b, const int *x, const int *y, int w)
{
    int fx[MAXW], fy[MAXW];
    memcpy(fx, x, (size_t)w * sizeof *x);
    memcpy(fy, y, (size_t)w * sizeof *y);
    fx[w - 1] = -x[w - 1];
    fy[w - 1] = -y[w - 1];
    return ult(b, fx, fy, w);
}
static void mul(Bl *b, const int *x, const int *y, int w, int *out)
{
    int acc[MAXW], part[MAXW];
    for (int i = 0; i < w; i++) acc[i] = F;
    for (int i = 0; i < w; i++) {
        for (int k = 0; k < w; k++) part[k] = k < i ? F : AND(b, y[i], x[k - i]);
        add(b, acc, part, F, w, acc, NULL);
    }
    memcpy(out, acc, (size_t)w * sizeof *acc);
}
static int all_equal(Bl *b, const int *x, int lo, int hi) /* x[lo..hi] all equal */
{
    int r = T;
    for (int i = lo + 1; i <= hi; i++) r = AND(b, r, -XOR(b, x[i], x[lo]));
    return r;
}

/* ------------------------------------------------------------------ types */

static int width(uint8_t s)
{
    static const int w[] = {0, 1, 8, 16, 32, 64, 8, 16, 32, 64};
    return s <= S_I64 ? w[s] : 0;
}
static int is_signed(uint8_t s) { return s >= S_I8 && s <= S_I64; }
static int log2w(int w) { int k = 0; while ((1 << k) < w) k++; return k; }

/* ------------------------------------------------------------------ symbolic execution */

typedef struct {
    int block;
    int guard;
    int **reg; /* nvregs pointers into the value pool (values are immutable once built) */
} St;

typedef struct {
    Bl b;
    const C2Unit *u;
    const C2Func *f;
    C2BlastInfo *info;
    int **pool; /* every allocated value, freed at the end */
    long npool, cappool;
    int32_t *bad;
    int nbad, capbad;
    long steps;
} Ex;

static int *val(Ex *x, int w)
{
    if (x->npool == x->cappool) {
        x->cappool = x->cappool ? x->cappool * 2 : 1024;
        int **np = realloc(x->pool, (size_t)x->cappool * sizeof *np);
        if (!np) { x->b.oom = 1; return NULL; }
        x->pool = np;
    }
    int *v = calloc((size_t)(w ? w : 1), sizeof *v);
    if (!v) { x->b.oom = 1; return NULL; }
    x->pool[x->npool++] = v;
    return v;
}

static int refuse(Ex *x, const char *rule, const char *fmt, ...)
{
    x->info->rule = rule;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(x->info->detail, sizeof x->info->detail, fmt, ap);
    va_end(ap);
    return -1;
}

/* a trap can fire on this path when cond holds: record it, return the guard of the run going on */
static int trap_if(Ex *x, int g, int cond)
{
    int t = AND(&x->b, g, cond);
    if (t != F) {
        if (x->nbad == x->capbad) {
            x->capbad = x->capbad ? x->capbad * 2 : 64;
            int32_t *nb = realloc(x->bad, (size_t)x->capbad * sizeof *nb);
            if (!nb) { x->b.oom = 1; return F; }
            x->bad = nb;
        }
        x->bad[x->nbad++] = t;
    }
    return AND(&x->b, g, -cond);
}

/* run one path to its end; forks are pushed on the stack */
static int run(Ex *x, St *s, St *stack, int *sp)
{
    const C2Func *f = x->f;
    Bl *b = &x->b;
    for (;;) {
        const C2Block *bk = &f->blk[s->block];
        for (uint32_t i = bk->first; i < bk->first + bk->count; i++) {
            const C2Insn *in = &f->in[i];
            if (++x->steps > C2_BLAST_MAX_STEPS) return refuse(x, "BLAST-TOO-LARGE", "more than %ld steps", C2_BLAST_MAX_STEPS);
            if (b->oom) return refuse(x, "BLAST-RESOURCE", "out of memory");
            const C2Type *td = in->dst >= 0 ? &f->vt[in->dst] : NULL;
            const C2Type *ta = in->a >= 0 ? &f->vt[in->a] : NULL;
            int w = td ? width(td->s) : 0;
            int *r = NULL;
            switch (in->op) {
            case C2_CONST:
                r = val(x, w);
                for (int k = 0; r && k < w; k++) r[k] = (in->imm >> k) & 1 ? T : F;
                break;
            case C2_MOV:
                s->reg[in->dst] = s->reg[in->a];
                continue;
            case C2_BIN: {
                const int *A = s->reg[in->a], *B = s->reg[in->b];
                int sg = is_signed(td->s);
                r = val(x, w);
                if (!r) break;
                switch (in->sub) {
                case B_ADD: case B_SUB: {
                    int nb[MAXW];
                    if (in->sub == B_SUB) neg_bits(B, w, nb); else memcpy(nb, B, (size_t)w * sizeof *nb);
                    add(b, A, nb, in->sub == B_SUB ? T : F, w, r, NULL);
                    if (sg) { /* overflow: operands (after negation for SUB) agree in sign, result differs */
                        int same = in->sub == B_ADD ? -XOR(b, A[w - 1], B[w - 1]) : XOR(b, A[w - 1], B[w - 1]);
                        s->guard = trap_if(x, s->guard, AND(b, same, XOR(b, r[w - 1], A[w - 1])));
                    }
                    break;
                }
                case B_MUL:
                    if (!sg) { mul(b, A, B, w, r); break; }
                    {
                        int ax[MAXW], bx[MAXW], p[MAXW];
                        for (int k = 0; k < 2 * w; k++) { ax[k] = A[k < w ? k : w - 1]; bx[k] = B[k < w ? k : w - 1]; }
                        mul(b, ax, bx, 2 * w, p);
                        memcpy(r, p, (size_t)w * sizeof *r);
                        s->guard = trap_if(x, s->guard, -all_equal(b, p, w - 1, 2 * w - 1));
                    }
                    break;
                case B_AND: for (int k = 0; k < w; k++) r[k] = AND(b, A[k], B[k]); break;
                case B_OR: for (int k = 0; k < w; k++) r[k] = OR(b, A[k], B[k]); break;
                case B_XOR: for (int k = 0; k < w; k++) r[k] = XOR(b, A[k], B[k]); break;
                case B_SHL: case B_SHR: {
                    if (in->sub == B_SHR && sg) return refuse(x, "BLAST-UNSUPPORTED", "SHR on a signed operand (insn %u)", i);
                    int wb = width(f->vt[in->b].s), lg = log2w(w);
                    int big = F; /* amount >= w, or negative (sign bit of a signed amount) */
                    for (int k = lg; k < wb; k++) big = OR(b, big, B[k]);
                    s->guard = trap_if(x, s->guard, big);
                    int cur[MAXW], nxt[MAXW];
                    memcpy(cur, A, (size_t)w * sizeof *cur);
                    for (int j = 0; j < lg && j < wb; j++) {
                        int sh = 1 << j;
                        for (int k = 0; k < w; k++) {
                            int from = in->sub == B_SHL ? k - sh : k + sh;
                            int moved = from >= 0 && from < w ? cur[from] : F;
                            nxt[k] = MUX(b, B[j], moved, cur[k]);
                        }
                        memcpy(cur, nxt, (size_t)w * sizeof *cur);
                    }
                    memcpy(r, cur, (size_t)w * sizeof *r);
                    if (in->sub == B_SHL && sg) { /* a << k representable iff a[w-1-k .. w-1] all equal */
                        int ovf = F;
                        for (int k = 1; k < w; k++) {
                            int amt = T;
                            for (int j = 0; j < lg; j++) amt = AND(b, amt, (k >> j) & 1 ? B[j] : -B[j]);
                            ovf = OR(b, ovf, AND(b, amt, -all_equal(b, A, w - 1 - k, w - 1)));
                        }
                        s->guard = trap_if(x, s->guard, ovf);
                    }
                    break;
                }
                default: return refuse(x, "BLAST-UNSUPPORTED", "binary op %u (insn %u)", in->sub, i);
                }
                break;
            }
            case C2_UN: {
                const int *A = s->reg[in->a];
                r = val(x, w);
                if (!r) break;
                if (in->sub == U_NEG) {
                    int zero[MAXW], na[MAXW];
                    for (int k = 0; k < w; k++) zero[k] = F;
                    neg_bits(A, w, na);
                    add(b, zero, na, T, w, r, NULL);
                    if (is_signed(td->s)) { /* -MIN is not representable */
                        int min = A[w - 1];
                        for (int k = 0; k < w - 1; k++) min = AND(b, min, -A[k]);
                        s->guard = trap_if(x, s->guard, min);
                    }
                } else if (in->sub == U_BNOT) neg_bits(A, w, r);
                else r[0] = -A[0];
                break;
            }
            case C2_CMP: {
                const int *A = s->reg[in->a], *B = s->reg[in->b];
                int wa = width(ta->s), sg = is_signed(ta->s);
                int lt = sg ? slt(b, A, B, wa) : ult(b, A, B, wa);
                int gt = sg ? slt(b, B, A, wa) : ult(b, B, A, wa);
                int e = eq(b, A, B, wa), c;
                switch (in->sub) {
                case CC_EQ: c = e; break;
                case CC_NE: c = -e; break;
                case CC_LT: c = lt; break;
                case CC_LE: c = -gt; break;
                case CC_GT: c = gt; break;
                default: c = -lt; break; /* CC_GE */
                }
                r = val(x, 1);
                if (r) r[0] = c;
                break;
            }
            case C2_CAST: {
                const int *A = s->reg[in->a];
                int ws = width(ta->s), wt = w, W = (ws > wt ? ws : wt) + 1;
                int ext[MAXW + 1];
                for (int k = 0; k < W; k++) ext[k] = k < ws ? A[k] : (is_signed(ta->s) ? A[ws - 1] : F);
                int fill = is_signed(td->s) ? ext[wt - 1] : F, fits = T;
                for (int k = wt; k < W; k++) fits = AND(b, fits, -XOR(b, ext[k], fill));
                s->guard = trap_if(x, s->guard, -fits);
                r = val(x, wt);
                if (r) memcpy(r, ext, (size_t)wt * sizeof *r);
                break;
            }
            case C2_TRAP:
                if (in->imm != 9) (void)trap_if(x, s->guard, T); /* 9 = REQUIRES: the caller's fault */
                return 0;
            case C2_RET:
                return 0;
            case C2_BR:
                s->block = in->t;
                goto next_block;
            case C2_CBR: {
                int c = s->reg[in->a][0];
                int gt = AND(b, s->guard, c), gf = AND(b, s->guard, -c);
                if (gt != F && gf != F) {
                    if (x->info->paths >= C2_BLAST_MAX_PATHS) return refuse(x, "BLAST-TOO-LARGE", "more than %d paths", C2_BLAST_MAX_PATHS);
                    St *o = &stack[(*sp)++];
                    o->block = in->f;
                    o->guard = gf;
                    o->reg = malloc((size_t)f->nvregs * sizeof *o->reg);
                    if (!o->reg) return refuse(x, "BLAST-RESOURCE", "out of memory");
                    memcpy(o->reg, s->reg, (size_t)f->nvregs * sizeof *o->reg);
                    x->info->paths++;
                }
                if (gt == F && gf == F) return 0;
                s->block = gt != F ? in->t : in->f;
                s->guard = gt != F ? gt : gf;
                goto next_block;
            }
            default:
                return refuse(x, "BLAST-UNSUPPORTED", "opcode %u (insn %u)", in->op, i);
            }
            if (!r) return refuse(x, "BLAST-RESOURCE", "out of memory");
            s->reg[in->dst] = r;
            if (s->guard == F) return 0; /* every run of this path has trapped */
        }
        return refuse(x, "BLAST-UNSUPPORTED", "block %d falls through", s->block);
    next_block:;
    }
}

int c2_func_index(const C2Unit *u, const char *name)
{
    for (int i = 0; i < u->nfuncs; i++)
        if (strcmp(u->fn[i].name, name) == 0) return i;
    return -1;
}

int c2_blast_notrap(const C2Unit *u, int fi, C2Cnf *cnf, C2BlastInfo *info)
{
    memset(cnf, 0, sizeof *cnf);
    memset(info, 0, sizeof *info);
    info->rule = "BLAST-OK";
    C2Verdict v;
    if (c2_check_unit(u, &v)) {
        info->rule = "BLAST-UNIT-REJECTED";
        snprintf(info->detail, sizeof info->detail, "%s", v.rule);
        return -1;
    }
    if (fi < 0 || fi >= u->nfuncs) { info->rule = "BLAST-NO-FUNCTION"; return -1; }
    const C2Func *f = &u->fn[fi];
    Ex x;
    memset(&x, 0, sizeof x);
    x.b.cnf = cnf;
    x.u = u;
    x.f = f;
    x.info = info;
    int rc = -1;
    St *stack = calloc(C2_BLAST_MAX_PATHS + 1, sizeof *stack);
    St s = {0, T, calloc((size_t)f->nvregs + 1, sizeof(int *))};
    int sp = 0;
    if (!stack || !s.reg) { refuse(&x, "BLAST-RESOURCE", "out of memory"); goto out; }
    for (int r = 0; r < f->nvregs; r++)
        if (!(f->vt[r].s >= S_BOOL && f->vt[r].s <= S_I64)) { refuse(&x, "BLAST-UNSUPPORTED", "register v%d is not a scalar", r); goto out; } /* GUARD:blast-scalar-only */
    /* variable 1 is TRUE; argument bits are variables 2..inputs+1 */
    x.b.nvars = 1;
    clause(&x.b, 1, T);
    for (int r = 0; r < f->nparams; r++) {
        int w = width(f->vt[r].s);
        int *p = val(&x, w);
        if (!p) { refuse(&x, "BLAST-RESOURCE", "out of memory"); goto out; }
        for (int k = 0; k < w; k++) p[k] = newvar(&x.b);
        s.reg[r] = p;
        info->inputs += w;
    }
    info->paths = 1;
    if (run(&x, &s, stack, &sp)) goto out;
    while (sp > 0) {
        St o = stack[--sp];
        int e = run(&x, &o, stack, &sp);
        free(o.reg);
        if (e) goto out;
    }
    /* some trap fires: OR of all trap sites (no site: the empty clause, unsatisfiable at once) */
    info->trap_sites = x.nbad;
    if (c2_cnf_add(cnf, x.bad, x.nbad) || x.b.oom) { refuse(&x, "BLAST-RESOURCE", "out of memory"); goto out; }
    cnf->nvars = x.b.nvars;
    rc = 0;
out:
    while (sp > 0) free(stack[--sp].reg);
    free(stack);
    free(s.reg);
    for (long i = 0; i < x.npool; i++) free(x.pool[i]);
    free(x.pool);
    free(x.bad);
    if (rc) c2_cnf_free(cnf);
    return rc;
}
