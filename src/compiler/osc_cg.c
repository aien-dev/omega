/*
 * osc_cg.c -- OSC-1 IR -> AArch64 (DESIGN section 7).
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 *
 * Frame: stp x29,x30,[sp,#-16]!; mov x29,sp; sub sp,sp,#F; F = 8*nvregs+8
 * rounded up to 16. [sp,#0] = x7 (OscRt*); vreg v at [sp,#8*(v+1)]. Every
 * non-parameter slot is zeroed in the prologue (same as the interpreter's
 * zero-initialised vregs). Operands are loaded into x9..x13, the result is
 * canonicalised (section 5) and stored back. Runtime entry points are called
 * with x0 = rt via ldr x16,[x0,#off]; blr x16. Conditional traps branch to
 * one stub per trap code at the end of the function.
 */
#include "osc_cg.h"
#include "osc_a64.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SP OSC_A64_SP
#define ZR OSC_A64_XZR

enum { FIX_BLOCK = 1, FIX_TRAP = 2 };

typedef struct {
    size_t at;          /* word index */
    OscA64Insn insn;    /* imm (byte offset) filled at patch time */
    int kind, target;
} Fix;

typedef struct {
    uint32_t *w;
    size_t n, cap;
    int bad;
    char *err;
    size_t errn;
    /* per function */
    int64_t blk[OSC_MAX_BLOCKS];
    int64_t trapl[OSC_TRAP_MAX + 1];
    int trap_used[OSC_TRAP_MAX + 1];
    Fix *fx;
    size_t nfx, capfx;
    uint32_t frame;
    uint32_t entry_word[OSC_MAX_FUNCS];
} Cg;

static void cg_fail(Cg *g, const char *fmt, ...) {
    if (g->bad) return;
    g->bad = 1;
    if (g->err && g->errn) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(g->err, g->errn, fmt, ap);
        va_end(ap);
    }
}

static size_t here(Cg *g) { return g->n; }

static void put(Cg *g, uint32_t word) {
    if (g->n == g->cap) {
        size_t nc = g->cap ? g->cap * 2 : 4096;
        uint32_t *p = realloc(g->w, nc * sizeof *p);
        if (!p) { cg_fail(g, "out of memory"); return; }
        g->w = p;
        g->cap = nc;
    }
    g->w[g->n++] = word;
}

static void E(Cg *g, OscA64Insn i) {
    uint32_t word = 0;
    char e[160];
    if (osc_a64_encode(&i, &word, e, sizeof e)) cg_fail(g, "encoder refused %s: %s", osc_a64_op_name(i.op), e);
    put(g, word);
}

static void fix(Cg *g, OscA64Insn i, int kind, int target) {
    if (g->nfx == g->capfx) {
        size_t nc = g->capfx ? g->capfx * 2 : 256;
        Fix *p = realloc(g->fx, nc * sizeof *p);
        if (!p) { cg_fail(g, "out of memory"); return; }
        g->fx = p;
        g->capfx = nc;
    }
    if (kind == FIX_TRAP) g->trap_used[target] = 1;
    g->fx[g->nfx++] = (Fix){here(g), i, kind, target};
    put(g, 0);
}

static void patch(Cg *g, size_t at, OscA64Insn i, size_t target) {
    i.imm = ((int64_t)target - (int64_t)at) * 4;
    uint32_t word = 0;
    char e[160];
    if (osc_a64_encode(&i, &word, e, sizeof e)) cg_fail(g, "encoder refused branch: %s", e);
    if (at < g->n) g->w[at] = word;
}

/* ---- small emission helpers ------------------------------------------- */
static int64_t slot(int v) { return 8 * ((int64_t)v + 1); }
static void ld(Cg *g, int r, int v) { E(g, osc_a64_mem(OSC_A64_LDR_UOFF, r, SP, slot(v))); }
static void st(Cg *g, int r, int v) { E(g, osc_a64_mem(OSC_A64_STR_UOFF, r, SP, slot(v))); }

static void mat(Cg *g, int r, uint64_t imm) {
    int zeros = 0, ones = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t c = (uint16_t)(imm >> (16 * i));
        zeros += c == 0;
        ones += c == 0xFFFF;
    }
    int first = 1;
    if (ones > zeros) {
        for (int i = 0; i < 4; i++) {
            uint16_t c = (uint16_t)(imm >> (16 * i));
            if (c == 0xFFFF) continue;
            if (first) { E(g, osc_a64_mov16(OSC_A64_MOVN, r, (uint16_t)~c, 16 * i)); first = 0; }
            else E(g, osc_a64_mov16(OSC_A64_MOVK, r, c, 16 * i));
        }
        if (first) E(g, osc_a64_mov16(OSC_A64_MOVN, r, 0, 0)); /* all ones */
    } else {
        for (int i = 0; i < 4; i++) {
            uint16_t c = (uint16_t)(imm >> (16 * i));
            if (c == 0) continue;
            if (first) { E(g, osc_a64_mov16(OSC_A64_MOVZ, r, c, 16 * i)); first = 0; }
            else E(g, osc_a64_mov16(OSC_A64_MOVK, r, c, 16 * i));
        }
        if (first) E(g, osc_a64_mov16(OSC_A64_MOVZ, r, 0, 0));
    }
}

static void trap_if(Cg *g, OscA64Cond c, int code) { fix(g, osc_a64_bcond(c, 0), FIX_TRAP, code); }
static void cmp_rr(Cg *g, int a, int b) { E(g, osc_a64_r3s(OSC_A64_SUBS_REG, ZR, a, b, OSC_A64_LSL, 0)); }
static void cmp_ri(Cg *g, int a, int imm) { E(g, osc_a64_ri(OSC_A64_SUBS_IMM, ZR, a, imm, 0)); }

/* unsigned w<64: zero-extend in place */
static void zext(Cg *g, int r, unsigned w) {
    if (w < 64) E(g, osc_a64_bfm(OSC_A64_UBFM, r, r, 0, (int)w - 1));
}
/* signed w<64: r must equal sext_w(r) else OVERFLOW (uses x12) */
static void schk(Cg *g, int r, unsigned w, int code) {
    if (w >= 64) return;
    E(g, osc_a64_bfm(OSC_A64_SBFM, 12, r, 0, (int)w - 1));
    cmp_rr(g, 12, r);
    trap_if(g, OSC_A64_NE, code);
}

static void rt_call(Cg *g, int off) {
    E(g, osc_a64_mem(OSC_A64_LDR_UOFF, 16, 0, off));
    E(g, osc_a64_breg(OSC_A64_BLR, 16));
}
static void load_rt(Cg *g, int r) { E(g, osc_a64_mem(OSC_A64_LDR_UOFF, r, SP, 0)); }

static void trap_seq(Cg *g, int code) {
    load_rt(g, 0);
    mat(g, 1, (uint64_t)code);
    rt_call(g, 16);
    E(g, osc_a64_brk(code)); /* never reached: osc_rt_trap does not return */
}

static OscA64Cond cc_of(unsigned cc, bool s) {
    switch (cc) {
    case OSC_C_EQ: return OSC_A64_EQ;
    case OSC_C_NE: return OSC_A64_NE;
    case OSC_C_LT: return s ? OSC_A64_LT : OSC_A64_LO;
    case OSC_C_LE: return s ? OSC_A64_LE : OSC_A64_LS;
    case OSC_C_GT: return s ? OSC_A64_GT : OSC_A64_HI;
    default: return s ? OSC_A64_GE : OSC_A64_HS;
    }
}

static void bounds(Cg *g, int idx, unsigned len) {
    /* unsigned compare: a negative signed index is >= 2^63 and fails too */
    cmp_ri(g, idx, (int)len);
    trap_if(g, OSC_A64_HS, OSC_TRAP_BOUNDS);
}

/* slice access: the length is a register (x13), not an immediate */
static void bounds_r(Cg *g, int idx, int len) {
    cmp_rr(g, idx, len);
    trap_if(g, OSC_A64_HS, OSC_TRAP_BOUNDS);
}

static void gen_bin(Cg *g, const OscFunc *f, const OscInsn *in) {
    OscScalar t = f->vtype[in->a].s;
    unsigned w = osc_scalar_width(t);
    bool s = osc_scalar_signed(t);
    ld(g, 9, in->a);
    ld(g, 10, in->b);
    switch (in->sub) {
    case OSC_B_ADD:
    case OSC_B_SUB: {
        bool add = in->sub == OSC_B_ADD;
        if (s && w == 64) {
            E(g, osc_a64_r3s(add ? OSC_A64_ADDS_REG : OSC_A64_SUBS_REG, 11, 9, 10, OSC_A64_LSL, 0));
            trap_if(g, OSC_A64_VS, OSC_TRAP_OVERFLOW);
        } else {
            E(g, osc_a64_r3s(add ? OSC_A64_ADD_REG : OSC_A64_SUB_REG, 11, 9, 10, OSC_A64_LSL, 0));
            if (s) schk(g, 11, w, OSC_TRAP_OVERFLOW); else zext(g, 11, w);
        }
        break;
    }
    case OSC_B_MUL:
        E(g, osc_a64_r4(OSC_A64_MADD, 11, 9, 10, ZR));
        if (s && w == 64) {
            E(g, osc_a64_r3(OSC_A64_SMULH, 12, 9, 10));
            E(g, osc_a64_bfm(OSC_A64_SBFM, 13, 11, 63, 63)); /* asr x13, x11, #63 */
            cmp_rr(g, 12, 13);
            trap_if(g, OSC_A64_NE, OSC_TRAP_OVERFLOW);
        } else if (s) {
            schk(g, 11, w, OSC_TRAP_OVERFLOW);
        } else {
            zext(g, 11, w);
        }
        break;
    case OSC_B_DIV:
        fix(g, osc_a64_cb(OSC_A64_CBZ, 10, 0), FIX_TRAP, OSC_TRAP_DIV0);
        if (!s) {
            E(g, osc_a64_r3(OSC_A64_UDIV, 11, 9, 10));
        } else {
            if (w == 64) {
                /* INT64_MIN / -1: SDIV would silently give INT64_MIN */
                E(g, osc_a64_ri(OSC_A64_ADDS_IMM, ZR, 10, 1, 0)); /* cmn x10, #1 */
                size_t skip = here(g);
                put(g, 0);
                E(g, osc_a64_mov16(OSC_A64_MOVZ, 12, 0x8000, 48));
                cmp_rr(g, 9, 12);
                trap_if(g, OSC_A64_EQ, OSC_TRAP_OVERFLOW);
                patch(g, skip, osc_a64_bcond(OSC_A64_NE, 0), here(g));
            }
            E(g, osc_a64_r3(OSC_A64_SDIV, 11, 9, 10));
            schk(g, 11, w, OSC_TRAP_OVERFLOW); /* narrow MIN / -1 */
        }
        break;
    case OSC_B_REM:
        fix(g, osc_a64_cb(OSC_A64_CBZ, 10, 0), FIX_TRAP, OSC_TRAP_DIV0);
        E(g, osc_a64_r3(s ? OSC_A64_SDIV : OSC_A64_UDIV, 12, 9, 10));
        E(g, osc_a64_r4(OSC_A64_MSUB, 11, 12, 10, 9)); /* x11 = x9 - x12*x10 */
        break;
    case OSC_B_AND: E(g, osc_a64_r3s(OSC_A64_AND_REG, 11, 9, 10, OSC_A64_LSL, 0)); break;
    case OSC_B_OR:  E(g, osc_a64_r3s(OSC_A64_ORR_REG, 11, 9, 10, OSC_A64_LSL, 0)); break;
    case OSC_B_XOR: E(g, osc_a64_r3s(OSC_A64_EOR_REG, 11, 9, 10, OSC_A64_LSL, 0)); break;
    case OSC_B_SHL:
    case OSC_B_SHR:
        /* amount in 0..w-1; a negative signed amount is >= 2^63 unsigned */
        cmp_ri(g, 10, (int)w);
        trap_if(g, OSC_A64_HS, OSC_TRAP_SHIFT);
        if (in->sub == OSC_B_SHR) {
            E(g, osc_a64_r3(s ? OSC_A64_ASRV : OSC_A64_LSRV, 11, 9, 10));
        } else if (!s) {
            E(g, osc_a64_r3(OSC_A64_LSLV, 11, 9, 10));
            zext(g, 11, w);
        } else {
            E(g, osc_a64_r3(OSC_A64_LSLV, 11, 9, 10));
            if (w < 64) E(g, osc_a64_bfm(OSC_A64_SBFM, 11, 11, 0, (int)w - 1));
            E(g, osc_a64_r3(OSC_A64_ASRV, 12, 11, 10)); /* shifting back must give a */
            cmp_rr(g, 12, 9);
            trap_if(g, OSC_A64_NE, OSC_TRAP_OVERFLOW);
        }
        break;
    default:
        cg_fail(g, "bad BIN sub-op");
    }
    st(g, 11, in->dst);
}

static void gen_un(Cg *g, const OscFunc *f, const OscInsn *in) {
    OscScalar t = f->vtype[in->a].s;
    unsigned w = osc_scalar_width(t);
    bool s = osc_scalar_signed(t);
    ld(g, 9, in->a);
    switch (in->sub) {
    case OSC_U_NEG:
        if (s && w == 64) {
            E(g, osc_a64_r3s(OSC_A64_SUBS_REG, 11, ZR, 9, OSC_A64_LSL, 0));
            trap_if(g, OSC_A64_VS, OSC_TRAP_OVERFLOW);
        } else {
            E(g, osc_a64_r3s(OSC_A64_SUB_REG, 11, ZR, 9, OSC_A64_LSL, 0));
            if (s) schk(g, 11, w, OSC_TRAP_OVERFLOW); else zext(g, 11, w);
        }
        break;
    case OSC_U_BNOT:
        E(g, osc_a64_r3s(OSC_A64_ORN_REG, 11, ZR, 9, OSC_A64_LSL, 0));
        if (!s) zext(g, 11, w);
        break;
    case OSC_U_LNOT:
        E(g, osc_a64_mov16(OSC_A64_MOVZ, 12, 1, 0));
        E(g, osc_a64_r3s(OSC_A64_EOR_REG, 11, 9, 12, OSC_A64_LSL, 0));
        break;
    default:
        cg_fail(g, "bad UN sub-op");
    }
    st(g, 11, in->dst);
}

static void gen_cast(Cg *g, const OscFunc *f, const OscInsn *in) {
    OscScalar from = f->vtype[in->a].s, to = f->vtype[in->dst].s;
    unsigned wf = osc_scalar_width(from), wt = osc_scalar_width(to);
    ld(g, 9, in->a);
    if (!osc_scalar_signed(to)) {
        if (osc_scalar_signed(from)) { cmp_ri(g, 9, 0); trap_if(g, OSC_A64_LT, OSC_TRAP_CAST); }
        if (wt < 64) {
            E(g, osc_a64_bfm(OSC_A64_UBFM, 12, 9, 0, (int)wt - 1));
            cmp_rr(g, 12, 9);
            trap_if(g, OSC_A64_NE, OSC_TRAP_CAST);
        }
    } else {
        if (!osc_scalar_signed(from) && wf == 64) { cmp_ri(g, 9, 0); trap_if(g, OSC_A64_LT, OSC_TRAP_CAST); }
        if (wt < 64) {
            E(g, osc_a64_bfm(OSC_A64_SBFM, 12, 9, 0, (int)wt - 1));
            cmp_rr(g, 12, 9);
            trap_if(g, OSC_A64_NE, OSC_TRAP_CAST);
        }
    }
    st(g, 9, in->dst);
}

static void epilogue(Cg *g) {
    E(g, osc_a64_ri(OSC_A64_ADD_IMM, SP, SP, g->frame, 0));
    E(g, osc_a64_pair(OSC_A64_LDP_POST, 29, 30, SP, 16));
    E(g, osc_a64_breg(OSC_A64_RET, 30));
}

static void gen_func(Cg *g, const OscUnit *u, int fi) {
    const OscFunc *f = &u->funcs[fi];
    g->entry_word[fi] = (uint32_t)here(g);
    g->frame = (8u * f->nvregs + 8u + 15u) & ~15u;
    g->nfx = 0;
    for (int b = 0; b < OSC_MAX_BLOCKS; b++) g->blk[b] = -1;
    for (int c = 0; c <= OSC_TRAP_MAX; c++) { g->trapl[c] = -1; g->trap_used[c] = 0; }

    /* prologue */
    E(g, osc_a64_pair(OSC_A64_STP_PRE, 29, 30, SP, -16));
    E(g, osc_a64_ri(OSC_A64_ADD_IMM, 29, SP, 0, 0));            /* mov x29, sp */
    E(g, osc_a64_ri(OSC_A64_SUB_IMM, SP, SP, g->frame, 0));
    E(g, osc_a64_mem(OSC_A64_STR_UOFF, 7, SP, 0));              /* slot 0 = rt */
    for (int v = f->nparams; v < f->nvregs; v++) st(g, ZR, v);  /* zero-init */
    for (int p = 0; p < f->nparams; p++) st(g, p, p);

    for (int b = 0; b < f->nblocks; b++) {
        g->blk[b] = (int64_t)here(g);
        const OscBlock *bl = &f->blocks[b];
        for (uint32_t i = bl->first; i < bl->first + bl->count; i++) {
            const OscInsn *in = &f->insns[i];
            const OscType *T = f->vtype;
            switch (in->op) {
            case OSC_I_CONST: mat(g, 9, in->imm); st(g, 9, in->dst); break;
            case OSC_I_MOV: ld(g, 9, in->a); st(g, 9, in->dst); break;
            case OSC_I_BIN: gen_bin(g, f, in); break;
            case OSC_I_UN: gen_un(g, f, in); break;
            case OSC_I_CMP: {
                bool s = osc_scalar_signed(T[in->a].s);
                ld(g, 9, in->a);
                ld(g, 10, in->b);
                cmp_rr(g, 9, 10);
                /* cset x11, cc == csinc x11, xzr, xzr, !cc */
                E(g, osc_a64_csinc(11, ZR, ZR, (OscA64Cond)(cc_of(in->sub, s) ^ 1)));
                st(g, 11, in->dst);
                break;
            }
            case OSC_I_CAST: gen_cast(g, f, in); break;
            case OSC_I_ALLOC:
                load_rt(g, 0);
                mat(g, 1, T[in->dst].len);
                ld(g, 2, in->a);
                rt_call(g, 0);
                st(g, 0, in->dst);
                break;
            case OSC_I_RELEASE:
                load_rt(g, 0);
                ld(g, 1, in->a);
                mat(g, 2, T[in->a].len);
                rt_call(g, 8);
                break;
            case OSC_I_AOPEN: /* x0 = rt->arena_open(rt, K) */
                load_rt(g, 0);
                mat(g, 1, in->imm);
                rt_call(g, 24);
                st(g, 0, in->dst);
                break;
            case OSC_I_AALLOC: /* x0 = rt->arena_alloc(rt, handle, len, init) */
                load_rt(g, 0);
                ld(g, 1, in->b);
                mat(g, 2, T[in->dst].len);
                ld(g, 3, in->a);
                rt_call(g, 32);
                st(g, 0, in->dst);
                break;
            case OSC_I_ADESTROY: /* rt->arena_destroy(rt, handle) */
                load_rt(g, 0);
                ld(g, 1, in->a);
                rt_call(g, 40);
                break;
            /* OSC-3 item 2 pools: calls into the shared runtime */
            case OSC_I_POPEN: /* x0 = rt->pool_open(rt, K, base) */
                load_rt(g, 0);
                mat(g, 1, in->nargs);
                mat(g, 2, in->imm);
                rt_call(g, 48);
                st(g, 0, in->dst);
                break;
            case OSC_I_PCLOSE: /* rt->pool_close(rt, pool) */
                load_rt(g, 0);
                ld(g, 1, in->a);
                rt_call(g, 56);
                break;
            case OSC_I_HALLOC: /* x0 = rt->h_alloc(rt, pool, init) */
                load_rt(g, 0);
                ld(g, 1, in->b);
                ld(g, 2, in->a);
                rt_call(g, 64);
                st(g, 0, in->dst);
                break;
            case OSC_I_HGEN: /* x0 = rt->h_gen(rt, pool, slot) */
                load_rt(g, 0);
                ld(g, 1, in->a);
                ld(g, 2, in->b);
                rt_call(g, 72);
                st(g, 0, in->dst);
                break;
            case OSC_I_HFREE:  /* rt->h_free(rt, pool, slot, gen) */
            case OSC_I_HLOAD:  /* x0 = rt->h_load(rt, pool, slot, gen) */
            case OSC_I_HSTORE: /* rt->h_store(rt, pool, slot, gen, value) */
                load_rt(g, 0);
                ld(g, 1, in->a);
                ld(g, 2, in->b);
                ld(g, 3, in->c);
                if (in->op == OSC_I_HSTORE) ld(g, 4, in->args[0]);
                rt_call(g, in->op == OSC_I_HFREE ? 80 : in->op == OSC_I_HLOAD ? 88 : 96);
                if (in->op == OSC_I_HLOAD) st(g, 0, in->dst);
                break;
            case OSC_I_LOAD:
            case OSC_I_STORE:
                ld(g, 9, in->a);
                ld(g, 10, in->b);
                if (in->op == OSC_I_STORE) ld(g, 11, in->c);
                bounds(g, 10, T[in->a].len);
                E(g, osc_a64_r3s(OSC_A64_ADD_REG, 12, 9, 10, OSC_A64_LSL, 3));
                if (in->op == OSC_I_LOAD) {
                    E(g, osc_a64_mem(OSC_A64_LDR_UOFF, 11, 12, 0));
                    st(g, 11, in->dst);
                } else {
                    E(g, osc_a64_mem(OSC_A64_STR_UOFF, 11, 12, 0));
                }
                break;
            case OSC_I_SLOAD:
            case OSC_I_SSTORE:
                /* external byte slice: pointer = vreg a, length = vreg a + 1; unsigned i >= len traps
                 * (len 0 traps on every index, the pointer is never touched) */
                ld(g, 9, in->a);
                ld(g, 10, in->b);
                ld(g, 13, in->a + 1);
                if (in->op == OSC_I_SSTORE) ld(g, 11, in->c);
                bounds_r(g, 10, 13);
                if (in->op == OSC_I_SLOAD && T[in->a].s == OSC_T_BYTES) {
                    E(g, osc_a64_r3(OSC_A64_LDRB_REG, 11, 9, 10));
                    st(g, 11, in->dst);
                    break;
                }
                E(g, osc_a64_r3s(OSC_A64_ADD_REG, 12, 9, 10, OSC_A64_LSL, 3));
                if (in->op == OSC_I_SLOAD) {
                    E(g, osc_a64_mem(OSC_A64_LDR_UOFF, 11, 12, 0));
                    st(g, 11, in->dst);
                } else {
                    E(g, osc_a64_mem(OSC_A64_STR_UOFF, 11, 12, 0));
                }
                break;
            case OSC_I_FLOAD:
            case OSC_I_FSTORE: {
                /* OSC-2 structs: cell = field offset (+ checked element index) */
                const OscField *fd = &u->structs[T[in->a].sid - 1].fields[in->imm];
                int base = 9;
                ld(g, 9, in->a);
                if (in->op == OSC_I_FSTORE) ld(g, 11, in->c);
                if (in->b >= 0) {
                    ld(g, 10, in->b);
                    bounds(g, 10, fd->alen);
                    E(g, osc_a64_r3s(OSC_A64_ADD_REG, 12, 9, 10, OSC_A64_LSL, 3));
                    base = 12;
                }
                if (in->op == OSC_I_FLOAD) {
                    E(g, osc_a64_mem(OSC_A64_LDR_UOFF, 11, base, 8 * (int64_t)fd->off));
                    st(g, 11, in->dst);
                } else {
                    E(g, osc_a64_mem(OSC_A64_STR_UOFF, 11, base, 8 * (int64_t)fd->off));
                }
                break;
            }
            case OSC_I_CALL:
                /* Direct BL needs the callee already emitted. osc_ir_validate refuses
                 * calls to later functions (and self), so this cannot trigger on a
                 * validated unit; kept so the invariant is local to the emitter. */
                if (in->callee < 0 || in->callee >= fi) { cg_fail(g, "CALL to function %d not emitted before function %d", in->callee, fi); return; }
                for (int k = 0; k < in->nargs; k++) ld(g, k, in->args[k]);
                load_rt(g, 7);
                E(g, osc_a64_br(OSC_A64_BL, ((int64_t)g->entry_word[in->callee] - (int64_t)here(g)) * 4));
                if (in->dst >= 0) st(g, 0, in->dst);
                break;
            case OSC_I_TRAP: trap_seq(g, (int)in->imm); break;
            case OSC_I_BR: fix(g, osc_a64_br(OSC_A64_B, 0), FIX_BLOCK, in->blk_t); break;
            case OSC_I_CBR:
                ld(g, 9, in->a);
                fix(g, osc_a64_cb(OSC_A64_CBNZ, 9, 0), FIX_BLOCK, in->blk_t);
                fix(g, osc_a64_br(OSC_A64_B, 0), FIX_BLOCK, in->blk_f);
                break;
            case OSC_I_RET:
                if (in->a >= 0) ld(g, 0, in->a);
                else E(g, osc_a64_mov16(OSC_A64_MOVZ, 0, 0, 0)); /* void: x0 = 0 */
                epilogue(g);
                break;
            default:
                cg_fail(g, "func %d: unknown op %u", fi, in->op);
            }
        }
    }
    /* trap stubs, in code order */
    for (int c = 1; c <= OSC_TRAP_MAX; c++) {
        if (!g->trap_used[c]) continue;
        g->trapl[c] = (int64_t)here(g);
        trap_seq(g, c);
    }
    for (size_t k = 0; k < g->nfx; k++) {
        Fix *x = &g->fx[k];
        int64_t t = x->kind == FIX_BLOCK ? g->blk[x->target] : g->trapl[x->target];
        if (t < 0) { cg_fail(g, "func %d: unresolved label", fi); continue; }
        patch(g, x->at, x->insn, (size_t)t);
    }
}

int osc_cg_verify(const OscCode *c, char *err, size_t n) {
    if (!c || !c->code || (c->len & 3)) {
        if (err && n) snprintf(err, n, "bad code buffer");
        return -1;
    }
    for (size_t off = 0; off < c->len; off += 4) {
        uint32_t w = (uint32_t)c->code[off] | ((uint32_t)c->code[off + 1] << 8) |
                     ((uint32_t)c->code[off + 2] << 16) | ((uint32_t)c->code[off + 3] << 24);
        OscA64Insn d;
        uint32_t re;
        if (osc_a64_decode(w, &d) || osc_a64_reencode(&d, &re) || re != w) {
            if (err && n) snprintf(err, n, "word at +%zu (0x%08x) does not round-trip through the decoder", off, w);
            return -1;
        }
    }
    return 0;
}

int osc_cg_compile(const OscUnit *u, OscCode *out, char *err, size_t n) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!u) return -1;
    if (osc_ir_validate(u, err, n)) return -1;
    Cg g;
    memset(&g, 0, sizeof g);
    g.err = err;
    g.errn = n;
    for (int fi = 0; fi < u->nfuncs && !g.bad; fi++) gen_func(&g, u, fi);
    if (!g.bad && g.n == 0) cg_fail(&g, "empty unit");
    if (g.bad) {
        free(g.w);
        free(g.fx);
        return -1;
    }
    out->code = malloc(g.n * 4);
    if (!out->code) { free(g.w); free(g.fx); return -1; }
    for (size_t i = 0; i < g.n; i++)
        for (int b = 0; b < 4; b++) out->code[4 * i + b] = (uint8_t)(g.w[i] >> (8 * b));
    out->len = g.n * 4;
    out->nfuncs = u->nfuncs;
    for (int fi = 0; fi < u->nfuncs; fi++) out->entry[fi] = g.entry_word[fi] * 4;
    free(g.w);
    free(g.fx);
    if (osc_cg_verify(out, err, n)) {
        osc_cg_free(out);
        return -1;
    }
    return 0;
}

void osc_cg_free(OscCode *c) {
    if (!c) return;
    free(c->code);
    memset(c, 0, sizeof *c);
}
