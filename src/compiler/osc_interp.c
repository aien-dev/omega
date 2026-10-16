/*
 * osc_interp.c -- reference interpreter for the OSC-1 IR (DESIGN section 5).
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 *
 * Checked signed arithmetic is computed exactly in 128-bit integers and then
 * range-checked, independently of the native code's flag/extension tricks, so
 * the differential test compares two different derivations of section 5.
 */
#include "osc_interp.h"

#include <math.h>
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

/* ---- OSC-4 Slice A floats (docs/osc/OSC-4-DESIGN.md 1.3) ----------------
 * Plain C float / double on volatile locals, one C expression per IR op, built
 * with -ffp-contract=off -fno-fast-math -fno-math-errno (mk/compiler.mk), so
 * every op is one hardware instruction with one RNE rounding, the same the
 * native code performs. f32 values live in the low 32 bits of a cell. */
static float f32_of(uint64_t x) { uint32_t w = (uint32_t)x; float f; memcpy(&f, &w, 4); return f; }
static double f64_of(uint64_t x) { double d; memcpy(&d, &x, 8); return d; }
static uint64_t bits_f32(float f) { uint32_t w; memcpy(&w, &f, 4); return w; }
static uint64_t bits_f64(double d) { uint64_t w; memcpy(&w, &d, 8); return w; }

static bool fnan(bool dbl, uint64_t a) {
    return dbl ? (a & 0x7fffffffffffffffull) > 0x7ff0000000000000ull : (a & 0x7fffffffu) > 0x7f800000u;
}
static bool fsnan(bool dbl, uint64_t a) {
    return fnan(dbl, a) && !(a & (dbl ? 0x0008000000000000ull : 0x00400000ull));
}
static uint64_t fquiet(bool dbl, uint64_t a) { return a | (dbl ? 0x0008000000000000ull : 0x00400000ull); }
static bool fsign(bool dbl, uint64_t a) { return (a >> (dbl ? 63 : 31)) & 1; }

/* FMINNM / FMAXNM (IEEE minNum / maxNum, design 1.1 item 6 and 1.3 item 11): a
 * signalling NaN propagates quieted (first operand first), one quiet NaN yields
 * the other operand, two quiet NaNs yield the first, and -0.0 orders below +0.0.
 * C fmin / fmax are never called. */
static uint64_t do_fminmax(bool dbl, bool is_max, uint64_t a, uint64_t b) {
    bool an = fnan(dbl, a), bn = fnan(dbl, b);
    if (an || bn) {
        if (fsnan(dbl, a)) return fquiet(dbl, a);
        if (fsnan(dbl, b)) return fquiet(dbl, b);
        if (an && bn) return a;
        return an ? b : a;
    }
    bool lt, eq;
    if (dbl) { volatile double x = f64_of(a), y = f64_of(b); lt = x < y; eq = x == y; }
    else { volatile float x = f32_of(a), y = f32_of(b); lt = x < y; eq = x == y; }
    if (eq) { /* equal values: only the zeros can differ in bits, by sign */
        bool neg_a = fsign(dbl, a);
        return is_max ? (neg_a ? b : a) : (neg_a ? a : b);
    }
    return is_max ? (lt ? b : a) : (lt ? a : b);
}

static uint64_t do_fbin(Ctx *c, unsigned sub, bool dbl, uint64_t a, uint64_t b) {
    if (sub == OSC_FB_MIN || sub == OSC_FB_MAX) return do_fminmax(dbl, sub == OSC_FB_MAX, a, b);
    if (dbl) {
        volatile double x = f64_of(a), y = f64_of(b), r;
        switch (sub) {
        case OSC_FB_ADD: r = x + y; break;
        case OSC_FB_SUB: r = x - y; break;
        case OSC_FB_MUL: r = x * y; break;
        case OSC_FB_DIV: r = x / y; break;
        default: trap(c, OSC_TRAP_RUNTIME); return 0;
        }
        return bits_f64(r);
    }
    volatile float x = f32_of(a), y = f32_of(b), r;
    switch (sub) {
    case OSC_FB_ADD: r = x + y; break;
    case OSC_FB_SUB: r = x - y; break;
    case OSC_FB_MUL: r = x * y; break;
    case OSC_FB_DIV: r = x / y; break;
    default: trap(c, OSC_TRAP_RUNTIME); return 0;
    }
    return bits_f32(r);
}

/* NEG is `-x` (FNEG: sign flip, so -(+0.0) is -0.0), ABS clears the sign, SQRT
 * is the correctly rounded sqrt / sqrtf (the only libm calls, inlined to FSQRT). */
static uint64_t do_fun(Ctx *c, unsigned sub, bool dbl, uint64_t a) {
    if (dbl) {
        volatile double x = f64_of(a), r;
        switch (sub) {
        case OSC_FU_NEG: r = -x; break;
        case OSC_FU_ABS: return a & 0x7fffffffffffffffull;
        case OSC_FU_SQRT: r = sqrt(x); break;
        default: trap(c, OSC_TRAP_RUNTIME); return 0;
        }
        return bits_f64(r);
    }
    volatile float x = f32_of(a), r;
    switch (sub) {
    case OSC_FU_NEG: r = -x; break;
    case OSC_FU_ABS: return a & 0x7fffffffu;
    case OSC_FU_SQRT: r = sqrtf(x); break;
    default: trap(c, OSC_TRAP_RUNTIME); return 0;
    }
    return bits_f32(r);
}

/* C comparison operators: every ordered relation and == read false on NaN, != true (FCMP + cond) */
static uint64_t do_fcmp(unsigned cc, bool dbl, uint64_t a, uint64_t b) {
    bool lt, eq, gt;
    if (dbl) { volatile double x = f64_of(a), y = f64_of(b); lt = x < y; eq = x == y; gt = x > y; }
    else { volatile float x = f32_of(a), y = f32_of(b); lt = x < y; eq = x == y; gt = x > y; }
    switch (cc) {
    case OSC_C_EQ: return eq;
    case OSC_C_NE: return !eq;
    case OSC_C_LT: return lt;
    case OSC_C_LE: return lt || eq;
    case OSC_C_GT: return gt;
    case OSC_C_GE: return gt || eq;
    default: return 0;
    }
}

static uint64_t do_fcvt(bool to_dbl, uint64_t a) {
    if (to_dbl) { volatile float x = f32_of(a); volatile double r = (double)x; return bits_f64(r); }
    volatile double x = f64_of(a); volatile float r = (float)x; return bits_f32(r);
}

static uint64_t do_itof(unsigned sub, bool dbl, uint64_t a) {
    if (dbl) {
        volatile double r = sub == OSC_FI_SIGNED ? (double)(int64_t)a : (double)a;
        return bits_f64(r);
    }
    volatile float r = sub == OSC_FI_SIGNED ? (float)(int64_t)a : (float)a;
    return bits_f32(r);
}

/* FCVTZS / FCVTZU semantics spelled out: truncate toward zero, saturate at the
 * destination's range, NaN -> 0. C's out-of-range conversion is undefined, so
 * the range test happens first, in double (an f32 widens to double exactly). */
static uint64_t do_ftoi(bool dbl, uint64_t a, OscScalar to) {
    if (fnan(dbl, a)) return 0;
    volatile double x = dbl ? f64_of(a) : (double)f32_of(a);
    unsigned w = osc_scalar_width(to);
    double half = (double)(1ULL << (w - 1)); /* 2^(w-1), exact */
    if (osc_scalar_signed(to)) {
        if (x >= half) return canon(to, (uint64_t)smax(w));
        if (x <= -half) return canon(to, (uint64_t)smin(w));
        volatile int64_t v = (int64_t)x;
        return canon(to, (uint64_t)v);
    }
    if (x <= 0.0) return 0; /* also every negative value and -0.0 */
    if (x >= half * 2.0) return w >= 64 ? UINT64_MAX : (1ULL << w) - 1;
    volatile uint64_t v = (uint64_t)x;
    return v;
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

/* External byte slice access (docs/osc/OSC-EXT-BYTES-DESIGN.md): its own checked path,
 * independent of the pool-range check of elem(). The index is unsigned: a negative signed
 * index is >= 2^63 and fails with every length, len 0 fails for every index (the caller
 * pointer, possibly NULL, is not touched). The buffer belongs to the caller. */
static uint64_t sload(Ctx *c, OscScalar kind, uint64_t ptr, uint64_t len, uint64_t idx) {
    if (idx >= len) trap(c, OSC_TRAP_BOUNDS);
    if (kind == OSC_T_BYTES) return ((const uint8_t *)(uintptr_t)ptr)[idx];
    return ((const uint64_t *)(uintptr_t)ptr)[idx];
}
static void sstore(Ctx *c, uint64_t ptr, uint64_t len, uint64_t idx, uint64_t val) {
    if (idx >= len) trap(c, OSC_TRAP_BOUNDS);
    ((uint64_t *)(uintptr_t)ptr)[idx] = val;
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
            /* OSC-4 Slice A */
            case OSC_I_FBIN: v[in->dst] = do_fbin(c, in->sub, T[in->a].s == OSC_T_F64, v[in->a], v[in->b]); break;
            case OSC_I_FUN: v[in->dst] = do_fun(c, in->sub, T[in->a].s == OSC_T_F64, v[in->a]); break;
            case OSC_I_FCMP: v[in->dst] = do_fcmp(in->sub, T[in->a].s == OSC_T_F64, v[in->a], v[in->b]); break;
            case OSC_I_FCVT: v[in->dst] = do_fcvt(T[in->dst].s == OSC_T_F64, v[in->a]); break;
            case OSC_I_ITOF: v[in->dst] = do_itof(in->sub, T[in->dst].s == OSC_T_F64, v[in->a]); break;
            case OSC_I_FTOI: v[in->dst] = do_ftoi(T[in->a].s == OSC_T_F64, v[in->a], T[in->dst].s); break;
            case OSC_I_FBITS: v[in->dst] = canon(T[in->dst].s, v[in->a]); break;
            case OSC_I_ALLOC: v[in->dst] = osc_rt_alloc(c->rt, T[in->dst].len, v[in->a]); break;
            case OSC_I_RELEASE: osc_rt_release(c->rt, v[in->a], T[in->a].len); break;
            case OSC_I_AOPEN: v[in->dst] = osc_rt_arena_open(c->rt, in->imm); break;
            case OSC_I_AALLOC: v[in->dst] = osc_rt_arena_alloc(c->rt, v[in->b], T[in->dst].len, v[in->a]); break;
            case OSC_I_ADESTROY: osc_rt_arena_destroy(c->rt, v[in->a]); break;
            case OSC_I_POPEN: v[in->dst] = osc_rt_pool_open(c->rt, in->nargs, in->imm); break;
            case OSC_I_PCLOSE: osc_rt_pool_close(c->rt, v[in->a]); break;
            case OSC_I_HALLOC: v[in->dst] = osc_rt_h_alloc(c->rt, v[in->b], v[in->a]); break;
            case OSC_I_HGEN: v[in->dst] = osc_rt_h_gen(c->rt, v[in->a], v[in->b]); break;
            case OSC_I_HFREE: osc_rt_h_free(c->rt, v[in->a], v[in->b], v[in->c]); break;
            case OSC_I_HLOAD: v[in->dst] = osc_rt_h_load(c->rt, v[in->a], v[in->b], v[in->c]); break;
            case OSC_I_HSTORE: osc_rt_h_store(c->rt, v[in->a], v[in->b], v[in->c], v[in->args[0]]); break;
            case OSC_I_LOAD: v[in->dst] = *elem(c, &T[in->a], v[in->a], T[in->b].s, v[in->b]); break;
            case OSC_I_STORE: *elem(c, &T[in->a], v[in->a], T[in->b].s, v[in->b]) = v[in->c]; break;
            case OSC_I_SLOAD: v[in->dst] = sload(c, T[in->a].s, v[in->a], v[in->a + 1], v[in->b]); break;
            case OSC_I_SSTORE: sstore(c, v[in->a], v[in->a + 1], v[in->b], v[in->c]); break;
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
        if (t->s == OSC_T_BYTES || t->s == OSC_T_CELLS) continue; /* external pointer: checked as a pair below */
        if (t->s == OSC_T_REF) {
            if (!osc_rt_is_live_ref(rt, args[p], t->len)) return -1;
        } else if (canon(t->s, args[p]) != args[p]) {
            return -1;
        }
    }
    return osc_ir_slice_args_ok(f, args, nargs); /* NULL with len > 0, wrapping range, overlapping cells */
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
