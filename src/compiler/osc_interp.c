/*
 * osc_interp.c -- reference interpreter for the OSC-1 IR (DESIGN section 5).
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 *
 * Checked signed arithmetic is computed exactly in 128-bit integers and then
 * range-checked, independently of the native code's flag/extension tricks, so
 * the differential test compares two different derivations of section 5.
 */
#include "osc_interp.h"

#include <string.h>

typedef struct {
    const OscUnit *u;
    OscRt *rt;
    uint64_t steps;
} Ctx;

typedef __int128 i128;

static void trap(Ctx *c, unsigned code) { osc_rt_trap(c->rt, code); }

static uint64_t canon(OscScalar t, uint64_t x) {
    unsigned w = osc_scalar_width(t);
    if (t == OSC_T_BOOL) return x & 1;
    if (w >= 64) return x;
    if (osc_scalar_signed(t)) {
        uint64_t m = 1ULL << (w - 1);
        x &= (1ULL << w) - 1;
        return (x ^ m) - m;
    }
    return x & ((1ULL << w) - 1);
}

static i128 smin(unsigned w) { return -((i128)1 << (w - 1)); }
static i128 smax(unsigned w) { return ((i128)1 << (w - 1)) - 1; }

/* exact signed result -> canonical, or OVERFLOW */
static uint64_t sfit(Ctx *c, unsigned w, i128 r) {
    if (r < smin(w) || r > smax(w)) trap(c, OSC_TRAP_OVERFLOW);
    return (uint64_t)(int64_t)r;
}

static uint64_t do_bin(Ctx *c, unsigned sub, OscScalar t, OscScalar tb, uint64_t a, uint64_t b) {
    unsigned w = osc_scalar_width(t);
    bool s = osc_scalar_signed(t);
    i128 A = s ? (i128)(int64_t)a : (i128)a, B = osc_scalar_signed(tb) ? (i128)(int64_t)b : (i128)b;
    switch (sub) {
    case OSC_B_ADD: return s ? sfit(c, w, A + B) : canon(t, a + b);
    case OSC_B_SUB: return s ? sfit(c, w, A - B) : canon(t, a - b);
    case OSC_B_MUL: return s ? sfit(c, w, A * B) : canon(t, a * b);
    case OSC_B_DIV:
        if (b == 0) trap(c, OSC_TRAP_DIV0);
        return s ? sfit(c, w, A / B) : a / b;              /* C: truncation toward zero */
    case OSC_B_REM:
        if (b == 0) trap(c, OSC_TRAP_DIV0);
        return s ? (uint64_t)(int64_t)(A % B) : a % b;     /* sign of the dividend; MIN % -1 == 0 */
    case OSC_B_AND: return a & b;
    case OSC_B_OR: return a | b;
    case OSC_B_XOR: return a ^ b;
    case OSC_B_SHL:
    case OSC_B_SHR: {
        if (B < 0 || B >= (i128)w) trap(c, OSC_TRAP_SHIFT);
        unsigned k = (unsigned)B;
        if (sub == OSC_B_SHL) return s ? sfit(c, w, A * ((i128)1 << k)) : canon(t, a << k);
        return s ? (uint64_t)((int64_t)a >> k) : a >> k;
    }
    default: trap(c, OSC_TRAP_RUNTIME);
    }
    return 0;
}

static uint64_t do_un(Ctx *c, unsigned sub, OscScalar t, uint64_t a) {
    switch (sub) {
    case OSC_U_NEG:
        if (osc_scalar_signed(t)) return sfit(c, osc_scalar_width(t), -(i128)(int64_t)a);
        return canon(t, 0 - a);
    case OSC_U_BNOT: return canon(t, ~a);
    case OSC_U_LNOT: return a ^ 1;
    default: trap(c, OSC_TRAP_RUNTIME);
    }
    return 0;
}

static uint64_t do_cmp(unsigned cc, OscScalar t, uint64_t a, uint64_t b) {
    bool lt, eq = a == b;
    if (osc_scalar_signed(t)) lt = (int64_t)a < (int64_t)b;
    else lt = a < b;
    switch (cc) {
    case OSC_C_EQ: return eq;
    case OSC_C_NE: return !eq;
    case OSC_C_LT: return lt;
    case OSC_C_LE: return lt || eq;
    case OSC_C_GT: return !lt && !eq;
    case OSC_C_GE: return !lt;
    default: return 0;
    }
}

static uint64_t do_cast(Ctx *c, OscScalar to, OscScalar from, uint64_t a) {
    i128 v = osc_scalar_signed(from) ? (i128)(int64_t)a : (i128)a;
    unsigned w = osc_scalar_width(to);
    i128 lo = osc_scalar_signed(to) ? smin(w) : 0;
    i128 hi = osc_scalar_signed(to) ? smax(w) : (((i128)1 << w) - 1);
    if (v < lo || v > hi) trap(c, OSC_TRAP_CAST);
    return (uint64_t)v;
}

/* element pointer with the bounds rule of section 5 */
static uint64_t *elem(Ctx *c, const OscType *rt_, uint64_t base, OscScalar ti, uint64_t idx) {
    if (osc_scalar_signed(ti) && (int64_t)idx < 0) trap(c, OSC_TRAP_BOUNDS);
    if (idx >= rt_->len) trap(c, OSC_TRAP_BOUNDS);
    /* defensive: a REF must point into the pool (validated IR guarantees it) */
    uintptr_t lo = (uintptr_t)&c->rt->cells[0][0], hi = lo + sizeof c->rt->cells;
    if ((uintptr_t)base < lo || (uintptr_t)base + 8 * (idx + 1) > hi) trap(c, OSC_TRAP_RUNTIME);
    return (uint64_t *)(uintptr_t)base + idx;
}

/* OSC-2 structs: pointer to cell (field off + element idx) of struct ref base;
 * an array field index obeys the same bounds rule as elem() against alen */
static uint64_t *fcell(Ctx *c, const OscType *rt_, uint64_t base, unsigned field, bool has_ix, OscScalar ti,
                       uint64_t idx) {
    const OscField *fd = &c->u->structs[rt_->sid - 1].fields[field];
    uint64_t cell = fd->off;
    if (has_ix) {
        if (osc_scalar_signed(ti) && (int64_t)idx < 0) trap(c, OSC_TRAP_BOUNDS);
        if (idx >= fd->alen) trap(c, OSC_TRAP_BOUNDS);
        cell += idx;
    }
    uintptr_t lo = (uintptr_t)&c->rt->cells[0][0], hi = lo + sizeof c->rt->cells;
    if ((uintptr_t)base < lo || (uintptr_t)base + 8 * (cell + 1) > hi) trap(c, OSC_TRAP_RUNTIME);
    return (uint64_t *)(uintptr_t)base + cell;
}

static uint64_t run_func(Ctx *c, int fi, const uint64_t *args) {
    const OscFunc *f = &c->u->funcs[fi];
    const OscType *T = f->vtype;
    uint64_t v[OSC_MAX_VREGS];
    memset(v, 0, sizeof(uint64_t) * f->nvregs);
    for (int p = 0; p < f->nparams; p++) v[p] = args[p];
    int b = 0;
    for (;;) {
        const OscBlock *bl = &f->blocks[b];
        int next = -1;
        for (uint32_t i = bl->first; i < bl->first + bl->count; i++) {
            const OscInsn *in = &f->insns[i];
            if (++c->steps > OSC_INTERP_MAX_STEPS) trap(c, OSC_TRAP_RUNTIME);
            switch (in->op) {
            case OSC_I_CONST: v[in->dst] = in->imm; break;
            case OSC_I_MOV: v[in->dst] = v[in->a]; break;
            case OSC_I_BIN: v[in->dst] = do_bin(c, in->sub, T[in->a].s, T[in->b].s, v[in->a], v[in->b]); break;
            case OSC_I_UN: v[in->dst] = do_un(c, in->sub, T[in->a].s, v[in->a]); break;
            case OSC_I_CMP: v[in->dst] = do_cmp(in->sub, T[in->a].s, v[in->a], v[in->b]); break;
            case OSC_I_CAST: v[in->dst] = do_cast(c, T[in->dst].s, T[in->a].s, v[in->a]); break;
            case OSC_I_ALLOC: v[in->dst] = osc_rt_alloc(c->rt, T[in->dst].len, v[in->a]); break;
            case OSC_I_RELEASE: osc_rt_release(c->rt, v[in->a], T[in->a].len); break;
            case OSC_I_LOAD: v[in->dst] = *elem(c, &T[in->a], v[in->a], T[in->b].s, v[in->b]); break;
            case OSC_I_STORE: *elem(c, &T[in->a], v[in->a], T[in->b].s, v[in->b]) = v[in->c]; break;
            case OSC_I_FLOAD:
                v[in->dst] = *fcell(c, &T[in->a], v[in->a], (unsigned)in->imm, in->b >= 0,
                                    in->b >= 0 ? T[in->b].s : OSC_T_VOID, in->b >= 0 ? v[in->b] : 0);
                break;
            case OSC_I_FSTORE:
                *fcell(c, &T[in->a], v[in->a], (unsigned)in->imm, in->b >= 0, in->b >= 0 ? T[in->b].s : OSC_T_VOID,
                       in->b >= 0 ? v[in->b] : 0) = v[in->c];
                break;
            case OSC_I_CALL: {
                uint64_t ca[OSC_MAX_PARAMS] = {0};
                for (int k = 0; k < in->nargs; k++) ca[k] = v[in->args[k]];
                uint64_t r = run_func(c, in->callee, ca);
                if (in->dst >= 0) v[in->dst] = r;
                break;
            }
            case OSC_I_TRAP: trap(c, (unsigned)in->imm); break;
            case OSC_I_BR: next = in->blk_t; break;
            case OSC_I_CBR: next = v[in->a] ? in->blk_t : in->blk_f; break;
            case OSC_I_RET: return in->a >= 0 ? v[in->a] : 0;
            default: trap(c, OSC_TRAP_RUNTIME);
            }
        }
        if (next < 0) trap(c, OSC_TRAP_RUNTIME); /* unreachable for validated IR */
        b = next;
    }
}

static int check_args(const OscUnit *u, int func, const uint64_t *args, unsigned nargs, const OscRt *rt) {
    if (!rt || func < 0 || func >= u->nfuncs) return -1;
    const OscFunc *f = &u->funcs[func];
    if (nargs != f->nparams || (nargs && !args)) return -1;
    for (unsigned p = 0; p < nargs; p++) {
        const OscType *t = &f->vtype[p];
        if (t->s == OSC_T_REF) {
            if (!osc_rt_is_live_ref(rt, args[p], t->len)) return -1;
        } else if (canon(t->s, args[p]) != args[p]) {
            return -1;
        }
    }
    return 0;
}

int osc_interp_run_prevalidated(const OscUnit *u, int func, const uint64_t *args, unsigned nargs, OscRt *rt,
                                uint64_t *ret) {
    if (!u || !ret || check_args(u, func, args, nargs, rt)) return -1;
    Ctx c = {u, rt, 0};
    uint64_t a[OSC_MAX_PARAMS] = {0};
    for (unsigned i = 0; i < nargs; i++) a[i] = args[i];
    rt->trap_code = 0;
    if (setjmp(rt->jb) != 0) return (int)rt->trap_code;
    *ret = run_func(&c, func, a);
    return 0;
}

int osc_interp_run(const OscUnit *u, int func, const uint64_t *args, unsigned nargs, OscRt *rt, uint64_t *ret) {
    if (!u || osc_ir_validate(u, NULL, 0)) return -1;
    return osc_interp_run_prevalidated(u, func, args, nargs, rt, ret);
}
