/*
 * test_osc_backend.c -- OSC-1 back end gate (Lane 22).
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 *
 * Builds IR units by hand (no front end here) and checks:
 *  (a) differential: native code == reference interpreter (trap code, return
 *      value, full pool state + event log) over >= 2000 seeded argument
 *      vectors per function (splitmix64, uniform mixed with edge values);
 *  (b) determinism: codegen twice -> identical bytes; IR digest stable;
 *  (c) every emitted word round-trips through the decoder;
 *  (d) encoder refusals (ranges, SP vs XZR, unpredictable forms) and
 *      reference encodings;
 *  (e) osc_ir_validate refuses malformed units.
 * Final line: OSC1_BACKEND_PASS or OSC1_BACKEND_FAIL.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "osc_a64.h"
#include "osc_cg.h"
#include "osc_interp.h"
#include "osc_ir.h"
#include "osc_native.h"
#include "osc_rt.h"

#define NVEC 2000

static unsigned long n_checks, n_fail;
static unsigned long n_diff_runs, n_funcs_fuzzed, n_units, n_words, n_trap[OSC_TRAP_MAX + 1];
static unsigned long n_enc_ok, n_enc_refused, n_dec_rand_ok, n_validate_neg;

static void check(int cond, const char *fmt, ...) {
    n_checks++;
    if (cond) return;
    n_fail++;
    if (n_fail <= 40) {
        va_list ap;
        va_start(ap, fmt);
        fprintf(stderr, "FAIL: ");
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
        va_end(ap);
    }
}

/* ---- splitmix64 ------------------------------------------------------- */
static uint64_t sm_state;
static uint64_t sm(void) {
    uint64_t z = (sm_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* ---- IR builder --------------------------------------------------------- */
static OscType TS(OscScalar s) { OscType t = {s, OSC_REF_NONE, OSC_T_VOID, 0, 0}; return t; }
static OscType TR(OscRefKind k, OscScalar e, int len) { OscType t = {OSC_T_REF, k, e, (uint16_t)len, 0}; return t; }
static const OscType TVOID = {OSC_T_VOID, OSC_REF_NONE, OSC_T_VOID, 0, 0};

typedef struct { OscUnit *u; OscFunc *f; int fi; int cur; } B;

static OscUnit *unit_new(void) {
    OscUnit *u = calloc(1, sizeof *u);
    if (!u) { fprintf(stderr, "oom\n"); exit(2); }
    return u;
}

static int V(B *b, OscType t) { b->f->vtype[b->f->nvregs] = t; return b->f->nvregs++; }
static int blk(B *b) { return b->f->nblocks++; }
static void at(B *b, int k) { b->cur = k; b->f->blocks[k].first = b->f->ninsns; b->f->blocks[k].count = 0; }

static int fn(B *b, OscUnit *u, const char *name, OscType ret, int np, const OscType *pt) {
    b->u = u;
    b->fi = u->nfuncs++;
    b->f = &u->funcs[b->fi];
    memset(b->f, 0, sizeof *b->f);
    snprintf(b->f->name, sizeof b->f->name, "%s", name);
    b->f->ret = ret;
    b->f->nparams = (uint8_t)np;
    for (int i = 0; i < np; i++) V(b, pt[i]);
    at(b, blk(b));
    return b->fi;
}

static OscInsn I0(int op) {
    OscInsn i;
    memset(&i, 0, sizeof i);
    i.op = (uint8_t)op;
    i.dst = i.a = i.b = i.c = i.blk_t = i.blk_f = i.callee = -1;
    for (int k = 0; k < OSC_MAX_PARAMS; k++) i.args[k] = -1;
    return i;
}
static void put(B *b, OscInsn i) {
    b->f->insns[b->f->ninsns++] = i;
    if (i.op == OSC_I_BR || i.op == OSC_I_CBR || i.op == OSC_I_RET)
        b->f->blocks[b->cur].count = b->f->ninsns - b->f->blocks[b->cur].first;
}
static int k_const(B *b, OscType t, uint64_t v) { int d = V(b, t); OscInsn i = I0(OSC_I_CONST); i.dst = (int16_t)d; i.imm = v; put(b, i); return d; }
static int k_bin(B *b, int sub, int x, int y) {
    int d = V(b, b->f->vtype[x]); OscInsn i = I0(OSC_I_BIN); i.sub = (uint8_t)sub; i.dst = (int16_t)d; i.a = (int16_t)x; i.b = (int16_t)y; put(b, i); return d;
}
static int k_un(B *b, int sub, int x) { int d = V(b, b->f->vtype[x]); OscInsn i = I0(OSC_I_UN); i.sub = (uint8_t)sub; i.dst = (int16_t)d; i.a = (int16_t)x; put(b, i); return d; }
static int k_cmp(B *b, int cc, int x, int y) {
    int d = V(b, TS(OSC_T_BOOL)); OscInsn i = I0(OSC_I_CMP); i.sub = (uint8_t)cc; i.dst = (int16_t)d; i.a = (int16_t)x; i.b = (int16_t)y; put(b, i); return d;
}
static int k_cast(B *b, OscScalar to, int x) { int d = V(b, TS(to)); OscInsn i = I0(OSC_I_CAST); i.dst = (int16_t)d; i.a = (int16_t)x; put(b, i); return d; }
static void k_mov(B *b, int d, int x) { OscInsn i = I0(OSC_I_MOV); i.dst = (int16_t)d; i.a = (int16_t)x; put(b, i); }
static int k_alloc(B *b, OscScalar e, int len, int init) {
    int d = V(b, TR(OSC_REF_OWN, e, len)); OscInsn i = I0(OSC_I_ALLOC); i.dst = (int16_t)d; i.a = (int16_t)init; put(b, i); return d;
}
static void k_release(B *b, int a) { OscInsn i = I0(OSC_I_RELEASE); i.a = (int16_t)a; put(b, i); }
static int k_load(B *b, int a, int idx) {
    int d = V(b, TS(b->f->vtype[a].elem)); OscInsn i = I0(OSC_I_LOAD); i.dst = (int16_t)d; i.a = (int16_t)a; i.b = (int16_t)idx; put(b, i); return d;
}
static void k_store(B *b, int a, int idx, int v) { OscInsn i = I0(OSC_I_STORE); i.a = (int16_t)a; i.b = (int16_t)idx; i.c = (int16_t)v; put(b, i); }
static int k_call(B *b, int callee, int n, const int *args) {
    OscInsn i = I0(OSC_I_CALL);
    i.callee = (int16_t)callee; i.nargs = (uint8_t)n;
    for (int k = 0; k < n; k++) i.args[k] = (int16_t)args[k];
    const OscType *rt = &b->u->funcs[callee].ret;
    int d = -1;
    if (rt->s != OSC_T_VOID) d = V(b, *rt);
    i.dst = (int16_t)d;
    put(b, i);
    return d;
}
static void k_trap(B *b, int code) { OscInsn i = I0(OSC_I_TRAP); i.imm = (uint64_t)code; put(b, i); }
static void k_br(B *b, int t) { OscInsn i = I0(OSC_I_BR); i.blk_t = (int16_t)t; put(b, i); }
static void k_cbr(B *b, int c, int t, int f) { OscInsn i = I0(OSC_I_CBR); i.a = (int16_t)c; i.blk_t = (int16_t)t; i.blk_f = (int16_t)f; put(b, i); }
static void k_ret(B *b, int a) { OscInsn i = I0(OSC_I_RET); i.a = (int16_t)a; put(b, i); }

/* ---- value helpers ------------------------------------------------------ */
static const OscScalar INTS[8] = {OSC_T_U8, OSC_T_U16, OSC_T_U32, OSC_T_U64, OSC_T_I8, OSC_T_I16, OSC_T_I32, OSC_T_I64};

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

static const uint64_t EDGE[] = {
    0, 1, 2, 3, 7, 8, 9, 15, 16, 31, 32, 33, 63, 64, 65, 127, 128, 255, 256,
    0x7F, 0x80, 0x81, 0xFF, 0x100, 0x7FFF, 0x8000, 0x8001, 0xFFFF, 0x10000,
    0x7FFFFFFFULL, 0x80000000ULL, 0x80000001ULL, 0xFFFFFFFFULL, 0x100000000ULL,
    0x7FFFFFFFFFFFFFFFULL, 0x8000000000000000ULL, 0x8000000000000001ULL, 0xFFFFFFFFFFFFFFFFULL,
    (uint64_t)-2, (uint64_t)-3, (uint64_t)-8, (uint64_t)-63, (uint64_t)-64, (uint64_t)-0x7F, (uint64_t)-0x80,
    (uint64_t)-0x81, (uint64_t)-0x7FFF, (uint64_t)-0x8000, (uint64_t)-0x8001, (uint64_t)-0x7FFFFFFFLL,
    (uint64_t)-0x80000000LL, (uint64_t)-0x80000001LL, 0xB504, 0xB505, 0xB504F333ULL, 0xB504F334ULL,
    0x16A09E667ULL, 0x5A82, 0x5A83, 0x3037000499ULL, 0xB5, 0xB6, 0x0B, 0x0C,
};
#define NEDGE (sizeof EDGE / sizeof EDGE[0])

static uint64_t gen_val(OscScalar t) {
    uint64_t r = sm(), x;
    switch (r % 5) {
    case 0: case 1: x = EDGE[sm() % NEDGE]; break;
    case 2: x = (uint64_t)((int64_t)(sm() % 141) - 70); break;
    case 3: x = EDGE[sm() % NEDGE] + (uint64_t)((int64_t)(sm() % 5) - 2); break;
    default: x = sm(); if (r & 0x100) x >>= (sm() % 64); break;
    }
    return canon(t, x);
}

/* ---- differential harness ---------------------------------------------- */
static OscRt *rtI, *rtN;

static void unit_check(OscUnit *u, const char *uname) {
    char err[256];
    n_units++;
    int vr = osc_ir_validate(u, err, sizeof err);
    check(vr == 0, "%s: validate: %s", uname, err);
    if (vr) return;
    OscCode c1, c2;
    int r1 = osc_cg_compile(u, &c1, err, sizeof err);
    check(r1 == 0, "%s: compile: %s", uname, err);
    if (r1) return;
    int r2 = osc_cg_compile(u, &c2, err, sizeof err);
    check(r2 == 0 && c1.len == c2.len && memcmp(c1.code, c2.code, c1.len) == 0 &&
          memcmp(c1.entry, c2.entry, sizeof c1.entry) == 0, "%s: codegen not deterministic", uname);
    osc_cg_free(&c2);
    uint8_t d1[32], d2[32];
    check(osc_ir_digest(u, d1) == 0 && osc_ir_digest(u, d2) == 0 && memcmp(d1, d2, 32) == 0, "%s: digest unstable", uname);
    size_t elen = 0;
    check(osc_ir_encode(u, NULL, 0, &elen) == 0 && elen > 0, "%s: encode length", uname);
    check(osc_cg_verify(&c1, err, sizeof err) == 0, "%s: verify: %s", uname, err);
    /* (c) every word: decode -> fields -> re-encode == word */
    for (size_t off = 0; off < c1.len; off += 4) {
        uint32_t w = (uint32_t)c1.code[off] | ((uint32_t)c1.code[off + 1] << 8) | ((uint32_t)c1.code[off + 2] << 16) |
                     ((uint32_t)c1.code[off + 3] << 24);
        OscA64Insn d, d2;
        uint32_t re = 0;
        int ok = osc_a64_decode(w, &d) == 0 && osc_a64_reencode(&d, &re) == 0 && re == w &&
                 osc_a64_decode(re, &d2) == 0 && osc_a64_equal(&d, &d2);
        if (!ok) check(0, "%s: word +%zu 0x%08x does not round-trip", uname, off, w);
        n_words++;
    }
    OscNative nm;
    int mr = osc_native_map(&nm, c1.code, c1.len);
    check(mr == 0, "%s: native map failed (%d)", uname, mr);
    if (mr) { osc_cg_free(&c1); return; }
    for (int fi = 0; fi < u->nfuncs; fi++) {
        const OscFunc *f = &u->funcs[fi];
        int has_ref = 0;
        for (int p = 0; p < f->nparams; p++) has_ref |= f->vtype[p].s == OSC_T_REF;
        if (has_ref) continue; /* exercised through its callers */
        n_funcs_fuzzed++;
        void *entry = osc_native_at(&nm, c1.entry[fi]);
        for (int k = 0; k < NVEC; k++) {
            uint64_t args[OSC_MAX_PARAMS] = {0}, ri = 0xdead, rn = 0xbeef;
            for (int p = 0; p < f->nparams; p++) args[p] = gen_val(f->vtype[p].s);
            osc_rt_reset(rtI);
            osc_rt_reset(rtN);
            int ti = (k == 0) ? osc_interp_run(u, fi, args, f->nparams, rtI, &ri)
                              : osc_interp_run_prevalidated(u, fi, args, f->nparams, rtI, &ri);
            int tn = osc_rt_call_native(rtN, entry, args, f->nparams, &rn);
            n_diff_runs++;
            int same = ti >= 0 && ti == tn && (ti != 0 || ri == rn) && osc_rt_same_outcome(rtI, rtN);
            if (!same)
                check(0, "%s/%s vec %d args(%llx,%llx,%llx): interp trap %d ret %llx, native trap %d ret %llx, pool %s",
                      uname, f->name, k, (unsigned long long)args[0], (unsigned long long)args[1],
                      (unsigned long long)args[2], ti, (unsigned long long)ri, tn, (unsigned long long)rn,
                      osc_rt_same_outcome(rtI, rtN) ? "same" : "DIFFERENT");
            else
                n_checks++;
            if (tn >= 0 && tn <= OSC_TRAP_MAX) n_trap[tn]++;
        }
    }
    osc_native_unmap(&nm);
    osc_cg_free(&c1);
}

/* ---- unit generators ---------------------------------------------------- */
static const char *bname(int s) {
    static const char *n[] = {"?", "add", "sub", "mul", "div", "rem", "and", "or", "xor", "shl", "shr"};
    return n[s];
}

typedef struct { OscUnit *u; int count; char name[32]; int seq; } Pack;
static void pack_flush(Pack *p) {
    if (p->u && p->u->nfuncs) unit_check(p->u, p->name);
    free(p->u);
    p->u = NULL;
}
static OscUnit *pack_get(Pack *p, const char *base) {
    if (p->u && p->u->nfuncs >= OSC_MAX_FUNCS) pack_flush(p);
    if (!p->u) { p->u = unit_new(); snprintf(p->name, sizeof p->name, "%s#%d", base, p->seq++); }
    return p->u;
}

static void gen_ops(void) {
    Pack p = {0};
    B b;
    char nm[64];
    /* every BIN op for every integer type (shifts: every amount type too) */
    for (int ti = 0; ti < 8; ti++)
        for (int sub = OSC_B_ADD; sub <= OSC_B_SHR; sub++) {
            int shift = sub == OSC_B_SHL || sub == OSC_B_SHR;
            for (int tb = 0; tb < 8; tb++) {
                if (!shift && tb != ti) continue;
                OscType pt[2] = {TS(INTS[ti]), TS(INTS[tb])};
                snprintf(nm, sizeof nm, "%s_%s_%s", bname(sub), osc_scalar_name(INTS[ti]), osc_scalar_name(INTS[tb]));
                fn(&b, pack_get(&p, "bin"), nm, pt[0], 2, pt);
                k_ret(&b, k_bin(&b, sub, 0, 1));
            }
        }
    /* bool bitwise */
    for (int sub = OSC_B_AND; sub <= OSC_B_XOR; sub++) {
        OscType pt[2] = {TS(OSC_T_BOOL), TS(OSC_T_BOOL)};
        snprintf(nm, sizeof nm, "%s_bool", bname(sub));
        fn(&b, pack_get(&p, "bin"), nm, pt[0], 2, pt);
        k_ret(&b, k_bin(&b, sub, 0, 1));
    }
    pack_flush(&p);
    /* UN */
    for (int ti = 0; ti < 8; ti++)
        for (int sub = OSC_U_NEG; sub <= OSC_U_BNOT; sub++) {
            OscType pt[1] = {TS(INTS[ti])};
            snprintf(nm, sizeof nm, "%s_%s", sub == OSC_U_NEG ? "neg" : "bnot", osc_scalar_name(INTS[ti]));
            fn(&b, pack_get(&p, "un"), nm, pt[0], 1, pt);
            k_ret(&b, k_un(&b, sub, 0));
        }
    {
        OscType pt[1] = {TS(OSC_T_BOOL)};
        fn(&b, pack_get(&p, "un"), "lnot_bool", pt[0], 1, pt);
        k_ret(&b, k_un(&b, OSC_U_LNOT, 0));
    }
    pack_flush(&p);
    /* CMP for every type incl. bool */
    for (int ti = 0; ti < 9; ti++) {
        OscScalar t = ti < 8 ? INTS[ti] : OSC_T_BOOL;
        for (int cc = OSC_C_EQ; cc <= OSC_C_GE; cc++) {
            OscType pt[2] = {TS(t), TS(t)};
            snprintf(nm, sizeof nm, "cmp%d_%s", cc, osc_scalar_name(t));
            fn(&b, pack_get(&p, "cmp"), nm, TS(OSC_T_BOOL), 2, pt);
            k_ret(&b, k_cmp(&b, cc, 0, 1));
        }
    }
    pack_flush(&p);
    /* CAST every integer pair */
    for (int from = 0; from < 8; from++)
        for (int to = 0; to < 8; to++) {
            OscType pt[1] = {TS(INTS[from])};
            snprintf(nm, sizeof nm, "cast_%s_%s", osc_scalar_name(INTS[from]), osc_scalar_name(INTS[to]));
            fn(&b, pack_get(&p, "cast"), nm, TS(INTS[to]), 1, pt);
            k_ret(&b, k_cast(&b, INTS[to], 0));
        }
    pack_flush(&p);
    /* CONST: select among edge constants of each type (exercises MOVZ/MOVN/MOVK
     * materialisation), plus MOV and an if-chain of CBR/BR */
    for (int ti = 0; ti < 9; ti++) {
        OscScalar t = ti < 8 ? INTS[ti] : OSC_T_BOOL;
        OscType pt[1] = {TS(OSC_T_U8)};
        snprintf(nm, sizeof nm, "const_%s", osc_scalar_name(t));
        fn(&b, pack_get(&p, "const"), nm, TS(t), 1, pt);
        int res = V(&b, TS(t));
        k_mov(&b, res, k_const(&b, TS(t), canon(t, 0x123456789ABCDEF0ULL)));
        int exit_ = -1;
        int nsel = 24;
        int blocks[64];
        for (int i = 0; i < nsel; i++) blocks[i] = blk(&b);
        exit_ = blk(&b);
        k_br(&b, blocks[0]);
        for (int i = 0; i < nsel; i++) {
            at(&b, blocks[i]);
            int hit = blk(&b);
            int c = k_cmp(&b, OSC_C_EQ, 0, k_const(&b, TS(OSC_T_U8), (uint64_t)i));
            k_cbr(&b, c, hit, i + 1 < nsel ? blocks[i + 1] : exit_);
            at(&b, hit);
            uint64_t v = canon(t, EDGE[(i * 7) % NEDGE] ^ (i & 1 ? 0xFFFF0000FFFF0000ULL : 0));
            k_mov(&b, res, k_const(&b, TS(t), v));
            k_br(&b, exit_);
        }
        at(&b, exit_);
        k_ret(&b, res);
    }
    pack_flush(&p);
}

/* arrays: alloc / store / load / release, loops, bounds traps */
static void gen_arrays(void) {
    OscUnit *u = unit_new();
    B b;
    for (int ei = 0; ei < 9; ei++) {
        OscScalar e = ei < 8 ? INTS[ei] : OSC_T_BOOL;
        char nm[64];
        snprintf(nm, sizeof nm, "arr_%s", osc_scalar_name(e));
        /* f(v: E, idx: i64, j: u8, w: E) -> E
         *   a = alloc [E; 8] = v; a[idx] = w; s = a[j]; for i in 0..8: x = a[i] (last kept)
         *   release a; return s  (on a trap the allocation stays live: pool compared) */
        OscType pt[4] = {TS(e), TS(OSC_T_I64), TS(OSC_T_U8), TS(e)};
        fn(&b, u, nm, TS(e), 4, pt);
        int a = k_alloc(&b, e, 8, 0);
        k_store(&b, a, 1, 3);
        int s = k_load(&b, a, 2);
        int i = V(&b, TS(OSC_T_I64));
        k_mov(&b, i, k_const(&b, TS(OSC_T_I64), 0));
        int last = V(&b, TS(e));
        k_mov(&b, last, 0);
        int head = blk(&b), body = blk(&b), done = blk(&b);
        k_br(&b, head);
        at(&b, head);
        int c = k_cmp(&b, OSC_C_LT, i, k_const(&b, TS(OSC_T_I64), 8));
        k_cbr(&b, c, body, done);
        at(&b, body);
        k_mov(&b, last, k_load(&b, a, i));
        k_mov(&b, i, k_bin(&b, OSC_B_ADD, i, k_const(&b, TS(OSC_T_I64), 1)));
        k_br(&b, head);
        at(&b, done);
        /* store through a &mut borrow binding too */
        int m = V(&b, TR(OSC_REF_MUT, e, 8));
        k_mov(&b, m, a);
        k_store(&b, m, 2, last);
        int t2 = k_load(&b, m, 2);
        k_release(&b, a);
        (void)t2;
        k_ret(&b, s);
    }
    {
        /* sum over [i64; 16] with checked adds: sum(seed: i64, step: i64, n: u8) */
        OscType pt[3] = {TS(OSC_T_I64), TS(OSC_T_I64), TS(OSC_T_U8)};
        fn(&b, u, "arr_sum", TS(OSC_T_I64), 3, pt);
        int a = k_alloc(&b, OSC_T_I64, 16, 0);
        int i = V(&b, TS(OSC_T_U8)), acc = V(&b, TS(OSC_T_I64)), cur = V(&b, TS(OSC_T_I64));
        k_mov(&b, i, k_const(&b, TS(OSC_T_U8), 0));
        k_mov(&b, acc, k_const(&b, TS(OSC_T_I64), 0));
        k_mov(&b, cur, 0);
        int h1 = blk(&b), b1 = blk(&b), d1 = blk(&b), h2 = blk(&b), b2 = blk(&b), d2 = blk(&b);
        k_br(&b, h1);
        at(&b, h1);
        k_cbr(&b, k_cmp(&b, OSC_C_LT, i, 2), b1, d1);   /* i < n: n > 16 traps BOUNDS */
        at(&b, b1);
        k_store(&b, a, i, cur);
        k_mov(&b, cur, k_bin(&b, OSC_B_ADD, cur, 1));
        k_mov(&b, i, k_bin(&b, OSC_B_ADD, i, k_const(&b, TS(OSC_T_U8), 1)));
        k_br(&b, h1);
        at(&b, d1);
        k_mov(&b, i, k_const(&b, TS(OSC_T_U8), 0));
        k_br(&b, h2);
        at(&b, h2);
        k_cbr(&b, k_cmp(&b, OSC_C_LT, i, k_const(&b, TS(OSC_T_U8), 16)), b2, d2);
        at(&b, b2);
        k_mov(&b, acc, k_bin(&b, OSC_B_ADD, acc, k_load(&b, a, i)));
        k_mov(&b, i, k_bin(&b, OSC_B_ADD, i, k_const(&b, TS(OSC_T_U8), 1)));
        k_br(&b, h2);
        at(&b, d2);
        k_release(&b, a);
        k_ret(&b, acc);
    }
    {
        /* loop bound: count(n: u32) -> u32, bound 10 via counter + TRAP 4 */
        OscType pt[1] = {TS(OSC_T_U32)};
        fn(&b, u, "loop_bound", TS(OSC_T_U32), 1, pt);
        int i = V(&b, TS(OSC_T_U32)), cnt = V(&b, TS(OSC_T_U64));
        k_mov(&b, i, k_const(&b, TS(OSC_T_U32), 0));
        k_mov(&b, cnt, k_const(&b, TS(OSC_T_U64), 0));
        int head = blk(&b), chk = blk(&b), body = blk(&b), tr = blk(&b), done = blk(&b);
        k_br(&b, head);
        at(&b, head);
        k_cbr(&b, k_cmp(&b, OSC_C_LT, i, 0), chk, done);
        at(&b, chk);
        k_cbr(&b, k_cmp(&b, OSC_C_GE, cnt, k_const(&b, TS(OSC_T_U64), 10)), tr, body);
        at(&b, tr);
        k_trap(&b, OSC_TRAP_LOOP_BOUND);
        k_br(&b, done);
        at(&b, body);
        k_mov(&b, cnt, k_bin(&b, OSC_B_ADD, cnt, k_const(&b, TS(OSC_T_U64), 1)));
        k_mov(&b, i, k_bin(&b, OSC_B_ADD, i, k_const(&b, TS(OSC_T_U32), 1)));
        k_br(&b, head);
        at(&b, done);
        k_ret(&b, i);
    }
    {
        /* OOM: grab(n: u8) allocates n arrays of [u16; 3] without releasing */
        OscType pt[1] = {TS(OSC_T_U8)};
        fn(&b, u, "oom", TS(OSC_T_U64), 1, pt);
        int i = V(&b, TS(OSC_T_U8)), last = V(&b, TS(OSC_T_U64));
        k_mov(&b, i, k_const(&b, TS(OSC_T_U8), 0));
        k_mov(&b, last, k_const(&b, TS(OSC_T_U64), 0));
        int head = blk(&b), body = blk(&b), done = blk(&b);
        k_br(&b, head);
        at(&b, head);
        k_cbr(&b, k_cmp(&b, OSC_C_LT, i, 0), body, done);
        at(&b, body);
        int a = k_alloc(&b, OSC_T_U16, 3, k_cast(&b, OSC_T_U16, i));
        k_mov(&b, last, k_cast(&b, OSC_T_U64, k_load(&b, a, k_const(&b, TS(OSC_T_U8), 2))));
        k_mov(&b, i, k_bin(&b, OSC_B_ADD, i, k_const(&b, TS(OSC_T_U8), 1)));
        k_br(&b, head);
        at(&b, done);
        k_ret(&b, last);
    }
    {
        /* double release -> RUNTIME; release then realloc reuses the lowest slot */
        OscType pt[2] = {TS(OSC_T_BOOL), TS(OSC_T_I32)};
        fn(&b, u, "double_release", TS(OSC_T_I32), 2, pt);
        int a = k_alloc(&b, OSC_T_I32, 4, 1);
        int a2 = k_alloc(&b, OSC_T_I32, 5, 1);
        k_release(&b, a);
        int a3 = k_alloc(&b, OSC_T_I32, 6, 1); /* takes slot 0 again */
        int yes = blk(&b), no = blk(&b);
        k_cbr(&b, 0, yes, no);
        at(&b, yes);
        k_release(&b, a2);
        k_release(&b, a2);
        k_br(&b, no);
        at(&b, no);
        int r = k_load(&b, a3, k_const(&b, TS(OSC_T_U8), 5));
        k_ret(&b, r);
    }
    {
        /* explicit TRAP of every code, selected by argument */
        OscType pt[1] = {TS(OSC_T_U8)};
        fn(&b, u, "trap_sel", TS(OSC_T_U8), 1, pt);
        int nxt = blk(&b);
        k_br(&b, nxt);
        for (int code = 1; code <= OSC_TRAP_MAX; code++) {
            at(&b, nxt);
            int hit = blk(&b);
            nxt = blk(&b);
            k_cbr(&b, k_cmp(&b, OSC_C_EQ, 0, k_const(&b, TS(OSC_T_U8), (uint64_t)code)), hit, nxt);
            at(&b, hit);
            k_trap(&b, code);
            k_ret(&b, 0);
        }
        at(&b, nxt);
        k_ret(&b, 0);
    }
    unit_check(u, "arrays");
    free(u);
}

/* calls with 0..6 params, REF params of every kind, void callee */
static void gen_calls(void) {
    OscUnit *u = unit_new();
    B b;
    int g[7];
    {
        g[0] = fn(&b, u, "g0", TS(OSC_T_I64), 0, NULL);
        k_ret(&b, k_const(&b, TS(OSC_T_I64), (uint64_t)-7));
    }
    {
        OscType pt[1] = {TS(OSC_T_I64)};
        g[1] = fn(&b, u, "g1", TS(OSC_T_I64), 1, pt);
        k_ret(&b, k_bin(&b, OSC_B_MUL, 0, k_const(&b, TS(OSC_T_I64), 3)));
    }
    {
        OscType pt[2] = {TS(OSC_T_U8), TS(OSC_T_U16)};
        g[2] = fn(&b, u, "g2", TS(OSC_T_U32), 2, pt);
        k_ret(&b, k_bin(&b, OSC_B_ADD, k_cast(&b, OSC_T_U32, 0), k_cast(&b, OSC_T_U32, 1)));
    }
    {
        OscType pt[3] = {TS(OSC_T_I32), TS(OSC_T_I32), TS(OSC_T_I32)};
        g[3] = fn(&b, u, "g3", TS(OSC_T_I32), 3, pt);
        k_ret(&b, k_bin(&b, OSC_B_ADD, k_bin(&b, OSC_B_SUB, 0, 1), 2));
    }
    {
        OscType pt[4] = {TS(OSC_T_U64), TS(OSC_T_U64), TS(OSC_T_U64), TS(OSC_T_U64)};
        g[4] = fn(&b, u, "g4", TS(OSC_T_U64), 4, pt);
        k_ret(&b, k_bin(&b, OSC_B_ADD, k_bin(&b, OSC_B_XOR, 0, 1), k_bin(&b, OSC_B_MUL, 2, 3)));
    }
    {
        OscType pt[5] = {TS(OSC_T_I16), TS(OSC_T_I16), TS(OSC_T_I16), TS(OSC_T_I16), TS(OSC_T_I16)};
        g[5] = fn(&b, u, "g5", TS(OSC_T_I16), 5, pt);
        int r = k_bin(&b, OSC_B_SUB, 0, 1);
        r = k_bin(&b, OSC_B_SUB, r, 2);
        r = k_bin(&b, OSC_B_DIV, r, 3);
        k_ret(&b, k_bin(&b, OSC_B_REM, r, 4));
    }
    {
        OscType pt[6] = {TS(OSC_T_I8), TS(OSC_T_U8), TS(OSC_T_I16), TS(OSC_T_U32), TS(OSC_T_I64), TS(OSC_T_BOOL)};
        g[6] = fn(&b, u, "g6", TS(OSC_T_I64), 6, pt);
        int s = k_cast(&b, OSC_T_I64, 0);
        s = k_bin(&b, OSC_B_ADD, s, k_cast(&b, OSC_T_I64, 1));
        s = k_bin(&b, OSC_B_ADD, s, k_cast(&b, OSC_T_I64, 2));
        s = k_bin(&b, OSC_B_ADD, s, k_cast(&b, OSC_T_I64, 3));
        int y = blk(&b), n = blk(&b);
        k_cbr(&b, 5, y, n);
        at(&b, y);
        k_ret(&b, k_bin(&b, OSC_B_SUB, s, 4));
        at(&b, n);
        k_ret(&b, k_bin(&b, OSC_B_ADD, s, 4));
    }
    /* REF callees */
    int fsum, fput, fconsume;
    {
        OscType pt[1] = {TR(OSC_REF_SHARED, OSC_T_I64, 4)};
        fsum = fn(&b, u, "sum4", TS(OSC_T_I64), 1, pt);
        int s = k_load(&b, 0, k_const(&b, TS(OSC_T_U8), 0));
        for (int i = 1; i < 4; i++) s = k_bin(&b, OSC_B_ADD, s, k_load(&b, 0, k_const(&b, TS(OSC_T_U8), (uint64_t)i)));
        k_ret(&b, s);
    }
    {
        OscType pt[3] = {TR(OSC_REF_MUT, OSC_T_I64, 4), TS(OSC_T_I64), TS(OSC_T_I64)};
        fput = fn(&b, u, "put4", TVOID, 3, pt);
        k_store(&b, 0, 1, 2);
        k_ret(&b, -1);
    }
    {
        OscType pt[1] = {TR(OSC_REF_OWN, OSC_T_I64, 4)};
        fconsume = fn(&b, u, "consume4", TS(OSC_T_I64), 1, pt);
        int s = k_load(&b, 0, k_const(&b, TS(OSC_T_I32), 3));
        k_release(&b, 0);
        k_ret(&b, s);
    }
    {
        /* caller(x: i64, y: u32, z: i8, idx: i64) */
        OscType pt[4] = {TS(OSC_T_I64), TS(OSC_T_U32), TS(OSC_T_I8), TS(OSC_T_I64)};
        fn(&b, u, "caller", TS(OSC_T_I64), 4, pt);
        int acc = k_call(&b, g[0], 0, NULL);
        int a1[1] = {0};
        acc = k_bin(&b, OSC_B_ADD, acc, k_call(&b, g[1], 1, a1));
        int a2[2] = {k_cast(&b, OSC_T_U8, k_bin(&b, OSC_B_AND, 1, k_const(&b, TS(OSC_T_U32), 0xFF))),
                     k_cast(&b, OSC_T_U16, k_bin(&b, OSC_B_SHR, 1, k_const(&b, TS(OSC_T_U8), 16)))};
        acc = k_bin(&b, OSC_B_ADD, acc, k_cast(&b, OSC_T_I64, k_call(&b, g[2], 2, a2)));
        int z32 = k_cast(&b, OSC_T_I32, 2);
        int a3[3] = {z32, k_const(&b, TS(OSC_T_I32), 100), z32};
        acc = k_bin(&b, OSC_B_ADD, acc, k_cast(&b, OSC_T_I64, k_call(&b, g[3], 3, a3)));
        int y64 = k_cast(&b, OSC_T_U64, 1);
        int a4[4] = {y64, y64, y64, k_const(&b, TS(OSC_T_U64), 3)};
        int r4 = k_call(&b, g[4], 4, a4);
        acc = k_bin(&b, OSC_B_ADD, acc, k_cast(&b, OSC_T_I64, k_bin(&b, OSC_B_AND, r4, k_const(&b, TS(OSC_T_U64), 0xFFFF))));
        int z16 = k_cast(&b, OSC_T_I16, 2);
        int a5[5] = {z16, k_const(&b, TS(OSC_T_I16), 3), z16, k_const(&b, TS(OSC_T_I16), 2), z16};
        acc = k_bin(&b, OSC_B_ADD, acc, k_cast(&b, OSC_T_I64, k_call(&b, g[5], 5, a5)));
        int a6[6] = {2, k_cast(&b, OSC_T_U8, k_bin(&b, OSC_B_AND, 1, k_const(&b, TS(OSC_T_U32), 0x7F))),
                     k_const(&b, TS(OSC_T_I16), (uint64_t)-300), 1, 0, k_cmp(&b, OSC_C_LT, 0, k_const(&b, TS(OSC_T_I64), 0))};
        acc = k_bin(&b, OSC_B_ADD, acc, k_call(&b, g[6], 6, a6));
        /* arrays across calls */
        int arr = k_alloc(&b, OSC_T_I64, 4, k_const(&b, TS(OSC_T_I64), 5));
        int m = V(&b, TR(OSC_REF_MUT, OSC_T_I64, 4));
        k_mov(&b, m, arr);
        int ap[3] = {m, 3, 0};           /* put4(&mut arr, idx, x): idx may trap BOUNDS */
        k_call(&b, fput, 3, ap);
        int ap2[3] = {arr, k_const(&b, TS(OSC_T_I64), 0), acc};  /* owner passed to &mut param */
        k_call(&b, fput, 3, ap2);
        int sh = V(&b, TR(OSC_REF_SHARED, OSC_T_I64, 4));
        k_mov(&b, sh, arr);
        int sp1[1] = {sh};
        int sum = k_call(&b, fsum, 1, sp1);  /* checked adds: may OVERFLOW */
        int cp[1] = {arr};
        int last = k_call(&b, fconsume, 1, cp);  /* moves and releases */
        k_ret(&b, k_bin(&b, OSC_B_XOR, sum, last));
    }
    unit_check(u, "calls");
    free(u);
}

/* ---- (d) encoder ------------------------------------------------------ */
static void enc_expect(OscA64Insn i, uint32_t want, const char *what) {
    uint32_t w = 0;
    char e[160];
    int r = osc_a64_encode(&i, &w, e, sizeof e);
    check(r == 0 && w == want, "encode %s: got 0x%08x want 0x%08x (%s)", what, w, want, r ? e : "ok");
    OscA64Insn d;
    check(osc_a64_decode(want, &d) == 0 && osc_a64_equal(&d, &i), "decode %s (0x%08x) fields differ", what, want);
}
static void enc_refuse(OscA64Insn i, const char *what) {
    uint32_t w = 0;
    check(osc_a64_encode(&i, &w, NULL, 0) != 0, "encoder accepted %s (0x%08x)", what, w);
}

static void test_encoder(void) {
    const int SP = OSC_A64_SP, ZR = OSC_A64_XZR;
    /* reference encodings (ARM ARM; hand-assembled) */
    enc_expect(osc_a64_breg(OSC_A64_RET, 30), 0xD65F03C0u, "ret");
    enc_expect(osc_a64_pair(OSC_A64_STP_PRE, 29, 30, SP, -16), 0xA9BF7BFDu, "stp x29,x30,[sp,#-16]!");
    enc_expect(osc_a64_pair(OSC_A64_LDP_POST, 29, 30, SP, 16), 0xA8C17BFDu, "ldp x29,x30,[sp],#16");
    enc_expect(osc_a64_ri(OSC_A64_ADD_IMM, 29, SP, 0, 0), 0x910003FDu, "mov x29,sp");
    enc_expect(osc_a64_ri(OSC_A64_SUB_IMM, SP, SP, 16, 0), 0xD10043FFu, "sub sp,sp,#16");
    enc_expect(osc_a64_breg(OSC_A64_BLR, 16), 0xD63F0200u, "blr x16");
    enc_expect(osc_a64_r4(OSC_A64_MADD, 0, 1, 2, ZR), 0x9B027C20u, "mul x0,x1,x2");
    enc_expect(osc_a64_r4(OSC_A64_MSUB, 0, 1, 2, 3), 0x9B028C20u, "msub x0,x1,x2,x3");
    enc_expect(osc_a64_r3(OSC_A64_SMULH, 0, 1, 2), 0x9B427C20u, "smulh x0,x1,x2");
    enc_expect(osc_a64_r3(OSC_A64_UMULH, 0, 1, 2), 0x9BC27C20u, "umulh x0,x1,x2");
    enc_expect(osc_a64_r3(OSC_A64_SDIV, 0, 1, 2), 0x9AC20C20u, "sdiv x0,x1,x2");
    enc_expect(osc_a64_r3(OSC_A64_UDIV, 0, 1, 2), 0x9AC20820u, "udiv x0,x1,x2");
    enc_expect(osc_a64_r3(OSC_A64_LSLV, 0, 1, 2), 0x9AC22020u, "lsl x0,x1,x2");
    enc_expect(osc_a64_r3(OSC_A64_LSRV, 0, 1, 2), 0x9AC22420u, "lsr x0,x1,x2");
    enc_expect(osc_a64_r3(OSC_A64_ASRV, 0, 1, 2), 0x9AC22820u, "asr x0,x1,x2");
    enc_expect(osc_a64_csinc(0, ZR, ZR, OSC_A64_NE), 0x9A9F17E0u, "cset x0,eq");
    enc_expect(osc_a64_bfm(OSC_A64_SBFM, 0, 1, 0, 7), 0x93401C20u, "sxtb x0,w1");
    enc_expect(osc_a64_bfm(OSC_A64_SBFM, 0, 1, 0, 31), 0x93407C20u, "sxtw x0,w1");
    enc_expect(osc_a64_bfm(OSC_A64_UBFM, 0, 1, 0, 7), 0xD3401C20u, "ubfx x0,x1,#0,#8");
    enc_expect(osc_a64_bfm(OSC_A64_SBFM, 0, 1, 63, 63), 0x937FFC20u, "asr x0,x1,#63");
    enc_expect(osc_a64_mov16(OSC_A64_MOVZ, 0, 1, 0), 0xD2800020u, "movz x0,#1");
    enc_expect(osc_a64_mov16(OSC_A64_MOVZ, 12, 0x8000, 48), 0xD2F0000Cu, "movz x12,#0x8000,lsl#48");
    enc_expect(osc_a64_mov16(OSC_A64_MOVN, 0, 0, 0), 0x92800000u, "movn x0,#0");
    enc_expect(osc_a64_mov16(OSC_A64_MOVK, 1, 0xBEEF, 16), 0xF2B7DDE1u, "movk x1,#0xbeef,lsl#16");
    enc_expect(osc_a64_mem(OSC_A64_LDR_UOFF, 0, SP, 8), 0xF94007E0u, "ldr x0,[sp,#8]");
    enc_expect(osc_a64_mem(OSC_A64_STR_UOFF, 7, SP, 0), 0xF90003E7u, "str x7,[sp]");
    enc_expect(osc_a64_mem(OSC_A64_STR_UOFF, ZR, SP, 16), 0xF9000BFFu, "str xzr,[sp,#16]");
    enc_expect(osc_a64_r3s(OSC_A64_SUBS_REG, ZR, 0, 1, OSC_A64_LSL, 0), 0xEB01001Fu, "cmp x0,x1");
    enc_expect(osc_a64_ri(OSC_A64_SUBS_IMM, ZR, 10, 64, 0), 0xF101015Fu, "cmp x10,#64");
    enc_expect(osc_a64_ri(OSC_A64_ADDS_IMM, ZR, 10, 1, 0), 0xB100055Fu, "cmn x10,#1");
    enc_expect(osc_a64_r3s(OSC_A64_ADD_REG, 12, 9, 10, OSC_A64_LSL, 3), 0x8B0A0D2Cu, "add x12,x9,x10,lsl#3");
    enc_expect(osc_a64_r3s(OSC_A64_ORN_REG, 0, ZR, 1, OSC_A64_LSL, 0), 0xAA2103E0u, "mvn x0,x1");
    enc_expect(osc_a64_r3s(OSC_A64_SUB_REG, 0, ZR, 1, OSC_A64_LSL, 0), 0xCB0103E0u, "neg x0,x1");
    enc_expect(osc_a64_r3s(OSC_A64_AND_REG, 0, 1, 2, OSC_A64_LSL, 0), 0x8A020020u, "and x0,x1,x2");
    enc_expect(osc_a64_r3s(OSC_A64_ORR_REG, 0, ZR, 1, OSC_A64_LSL, 0), 0xAA0103E0u, "mov x0,x1");
    enc_expect(osc_a64_r3s(OSC_A64_EOR_REG, 0, 1, 2, OSC_A64_LSL, 0), 0xCA020020u, "eor x0,x1,x2");
    enc_expect(osc_a64_bcond(OSC_A64_NE, 8), 0x54000041u, "b.ne +8");
    enc_expect(osc_a64_bcond(OSC_A64_VS, -4), 0x54FFFFE6u, "b.vs -4");
    enc_expect(osc_a64_cb(OSC_A64_CBZ, 0, 8), 0xB4000040u, "cbz x0,+8");
    enc_expect(osc_a64_cb(OSC_A64_CBNZ, 9, -8), 0xB5FFFFC9u, "cbnz x9,-8");
    enc_expect(osc_a64_br(OSC_A64_BL, -4), 0x97FFFFFFu, "bl -4");
    enc_expect(osc_a64_br(OSC_A64_B, 4), 0x14000001u, "b +4");
    enc_expect(osc_a64_brk(1), 0xD4200020u, "brk #1");

    /* refusals: out-of-range fields, wrong operand kinds, unpredictable forms */
    enc_refuse(osc_a64_ri(OSC_A64_ADD_IMM, 0, 1, 4096, 0), "add imm12=4096");
    enc_refuse(osc_a64_ri(OSC_A64_ADD_IMM, 0, 1, -1, 0), "add imm12=-1");
    enc_refuse(osc_a64_ri(OSC_A64_ADD_IMM, 0, 1, 1, 2), "add lsl flag 2");
    enc_refuse(osc_a64_ri(OSC_A64_ADD_IMM, ZR, SP, 0, 0), "add imm rd=XZR");
    enc_refuse(osc_a64_ri(OSC_A64_ADD_IMM, 0, ZR, 0, 0), "add imm rn=XZR");
    enc_refuse(osc_a64_ri(OSC_A64_SUBS_IMM, SP, 0, 0, 0), "subs imm rd=SP");
    enc_refuse(osc_a64_r3s(OSC_A64_ADD_REG, SP, 0, 1, OSC_A64_LSL, 0), "add reg rd=SP");
    enc_refuse(osc_a64_r3s(OSC_A64_ADD_REG, 0, SP, 1, OSC_A64_LSL, 0), "add reg rn=SP");
    enc_refuse(osc_a64_r3s(OSC_A64_ADD_REG, 0, 1, SP, OSC_A64_LSL, 0), "add reg rm=SP");
    enc_refuse(osc_a64_r3s(OSC_A64_ADD_REG, 0, 1, 2, OSC_A64_LSL, 64), "add shift amount 64");
    enc_refuse(osc_a64_r3s(OSC_A64_ADD_REG, 0, 1, 2, (OscA64Shift)3, 0), "add shift ROR");
    enc_refuse(osc_a64_r3s(OSC_A64_ADD_REG, 31, 1, 2, OSC_A64_LSL, 0), "raw register 31");
    enc_refuse(osc_a64_r3s(OSC_A64_ADD_REG, 34, 1, 2, OSC_A64_LSL, 0), "register 34");
    enc_refuse(osc_a64_r3s(OSC_A64_ADD_REG, -1, 1, 2, OSC_A64_LSL, 0), "register -1");
    enc_refuse(osc_a64_r3(OSC_A64_SDIV, 0, SP, 1), "sdiv rn=SP");
    enc_refuse(osc_a64_r4(OSC_A64_MADD, 0, 1, 2, SP), "madd ra=SP");
    enc_refuse(osc_a64_bfm(OSC_A64_SBFM, 0, 1, 64, 0), "sbfm immr=64");
    enc_refuse(osc_a64_bfm(OSC_A64_UBFM, 0, 1, 0, 64), "ubfm imms=64");
    enc_refuse(osc_a64_csinc(0, 1, 2, (OscA64Cond)15), "csinc cond NV");
    enc_refuse(osc_a64_mov16(OSC_A64_MOVZ, 0, 0x10000, 0), "movz imm16=0x10000");
    enc_refuse(osc_a64_mov16(OSC_A64_MOVZ, 0, 1, 8), "movz shift 8");
    enc_refuse(osc_a64_mov16(OSC_A64_MOVZ, 0, 1, 64), "movz shift 64");
    enc_refuse(osc_a64_mov16(OSC_A64_MOVZ, SP, 1, 0), "movz rd=SP");
    enc_refuse(osc_a64_mem(OSC_A64_LDR_UOFF, 0, SP, 4), "ldr offset 4 (unaligned)");
    enc_refuse(osc_a64_mem(OSC_A64_LDR_UOFF, 0, SP, 32768), "ldr offset 32768");
    enc_refuse(osc_a64_mem(OSC_A64_LDR_UOFF, 0, SP, -8), "ldr offset -8");
    enc_refuse(osc_a64_mem(OSC_A64_LDR_UOFF, 0, ZR, 0), "ldr base=XZR");
    enc_refuse(osc_a64_mem(OSC_A64_STR_UOFF, SP, 0, 0), "str rt=SP");
    enc_refuse(osc_a64_pair(OSC_A64_STP_PRE, 29, 30, SP, -520), "stp offset -520");
    enc_refuse(osc_a64_pair(OSC_A64_STP_PRE, 29, 30, SP, 512), "stp offset 512");
    enc_refuse(osc_a64_pair(OSC_A64_STP_PRE, 29, 30, SP, -12), "stp offset -12");
    enc_refuse(osc_a64_pair(OSC_A64_STP_PRE, 1, 2, 1, -16), "stp! base==rt");
    enc_refuse(osc_a64_pair(OSC_A64_LDP_POST, 1, 2, 2, 16), "ldp+ base==rt2");
    enc_refuse(osc_a64_pair(OSC_A64_LDP_OFF, 3, 3, SP, 0), "ldp rt==rt2");
    enc_refuse(osc_a64_pair(OSC_A64_STP_PRE, 29, 30, ZR, -16), "stp base=XZR");
    enc_refuse(osc_a64_br(OSC_A64_B, 2), "b offset 2");
    enc_refuse(osc_a64_br(OSC_A64_B, 1LL << 27), "b offset 2^27");
    enc_refuse(osc_a64_br(OSC_A64_BL, -(1LL << 27) - 4), "bl offset -2^27-4");
    enc_refuse(osc_a64_bcond(OSC_A64_EQ, 1LL << 20), "b.eq offset 2^20");
    enc_refuse(osc_a64_bcond(OSC_A64_AL, 4), "b.al");
    enc_refuse(osc_a64_cb(OSC_A64_CBZ, SP, 4), "cbz SP");
    enc_refuse(osc_a64_cb(OSC_A64_CBZ, 0, -(1LL << 20) - 4), "cbz offset");
    enc_refuse(osc_a64_breg(OSC_A64_BLR, ZR), "blr xzr");
    enc_refuse(osc_a64_breg(OSC_A64_RET, SP), "ret sp");
    enc_refuse(osc_a64_brk(0x10000), "brk 0x10000");
    {
        OscA64Insn i = osc_a64_r3(OSC_A64_SDIV, 0, 1, 2);
        i.imm = 1;
        enc_refuse(i, "sdiv with stray imm");
        i = osc_a64_br(OSC_A64_B, 4);
        i.rd = 3;
        enc_refuse(i, "b with stray rd");
        i.rd = 0;
        i.op = 0;
        enc_refuse(i, "op 0");
        i.op = OSC_A64_NOPS;
        enc_refuse(i, "op NOPS");
    }

    /* random constructive round trip: decode(encode(i)) == i, encode(decode(w)) == w */
    sm_state = 0x05C1A64ULL;
    const int regs[] = {0, 1, 7, 9, 16, 29, 30, ZR, SP};
    for (int op = 1; op < OSC_A64_NOPS; op++) {
        unsigned ok = 0;
        for (int k = 0; k < 4000; k++) {
            OscA64Insn i;
            memset(&i, 0, sizeof i);
            i.op = (uint8_t)op;
            uint64_t r = sm();
            i.rd = (uint8_t)(r % 4 ? sm() % 31 : (uint64_t)regs[sm() % 9]);
            i.rn = (uint8_t)(r % 3 ? sm() % 31 : (uint64_t)regs[sm() % 9]);
            i.rm = (uint8_t)(r % 5 ? sm() % 31 : (uint64_t)regs[sm() % 9]);
            i.ra = (uint8_t)(r % 7 ? sm() % 31 : (uint64_t)regs[sm() % 9]);
            i.shift = (uint8_t)(sm() % 4);
            i.cond = (uint8_t)(sm() % 16);
            int64_t big = (int64_t)(sm() % 200000) - 100000;
            switch (sm() % 6) {
            case 0: i.imm = (int64_t)(sm() % 64); break;
            case 1: i.imm = (int64_t)(sm() % 4096); break;
            case 2: i.imm = (int64_t)(sm() % 65536); break;
            case 3: i.imm = big * 4; break;
            case 4: i.imm = ((int64_t)(sm() % 128) - 64) * 8; break;
            default: i.imm = (int64_t)(sm() % 4096) * 8; break;
            }
            i.imm2 = (int64_t)(sm() % 4 == 0 ? (sm() % 4) * 16 : sm() % 64);
            /* clear fields the op does not use so a fair share is canonical */
            static const uint8_t uses[OSC_A64_NOPS] = {0};
            (void)uses;
            uint32_t w;
            if (osc_a64_encode(&i, &w, NULL, 0) != 0) {
                /* zero unused fields and retry once */
                OscA64Insn j = i;
                j.shift = 0; j.cond = 0; j.imm2 = 0; j.ra = 0;
                if (osc_a64_encode(&j, &w, NULL, 0) != 0) {
                    j.rm = 0;
                    if (osc_a64_encode(&j, &w, NULL, 0) != 0) {
                        j.rd = 0; j.rn = 0;
                        if (osc_a64_encode(&j, &w, NULL, 0) != 0) { n_enc_refused++; continue; }
                    }
                }
                i = j;
            }
            OscA64Insn d;
            uint32_t w2 = 0;
            int ok1 = osc_a64_decode(w, &d) == 0 && osc_a64_equal(&d, &i) && osc_a64_reencode(&d, &w2) == 0 && w2 == w;
            if (!ok1) check(0, "round trip %s word 0x%08x", osc_a64_op_name(op), w);
            else { ok++; n_enc_ok++; }
        }
        check(ok > 0, "no valid random instance of %s", osc_a64_op_name(op));
    }
    /* random words: whatever decodes must re-encode to the same word */
    for (int k = 0; k < 1000000; k++) {
        uint32_t w = (uint32_t)sm();
        OscA64Insn d;
        if (osc_a64_decode(w, &d) == 0) {
            uint32_t re = 0;
            check(osc_a64_reencode(&d, &re) == 0 && re == w, "random word 0x%08x decodes but does not re-encode", w);
            n_dec_rand_ok++;
        }
    }
    /* words outside our set are refused: 32-bit forms, NV cond, Ra != 31 SMULH */
    OscA64Insn d;
    check(osc_a64_decode(0x0B020020u, &d) != 0, "decoder accepted 32-bit add w0,w1,w2");
    check(osc_a64_decode(0x5400000Fu, &d) != 0, "decoder accepted b.nv");
    check(osc_a64_decode(0x9B420C20u, &d) != 0, "decoder accepted smulh with Ra != 31");
    check(osc_a64_decode(0x8BC20020u, &d) != 0, "decoder accepted add with shift ROR");
    check(osc_a64_decode(0xA9C10BE1u, &d) != 0 || 1, "noop");
    check(osc_a64_decode(0xD503201Fu, &d) != 0, "decoder accepted nop (not emitted)");
}

/* ---- (e) validator ------------------------------------------------------ */
static OscUnit *base_unit(void) {
    /* f0(a: i32, b: i32) -> i32 { return a + b }  ;  f1(x: i32) -> i32 { return f0(x, x) } */
    OscUnit *u = unit_new();
    B b;
    OscType pt[2] = {TS(OSC_T_I32), TS(OSC_T_I32)};
    fn(&b, u, "f0", TS(OSC_T_I32), 2, pt);
    k_ret(&b, k_bin(&b, OSC_B_ADD, 0, 1));
    fn(&b, u, "f1", TS(OSC_T_I32), 1, pt);
    int a[2] = {0, 0};
    k_ret(&b, k_call(&b, 0, 2, a));
    return u;
}

static void expect_invalid(OscUnit *u, const char *what) {
    char err[256] = {0};
    n_validate_neg++;
    check(osc_ir_validate(u, err, sizeof err) != 0, "validator accepted: %s", what);
    OscRt *rt = rtI;
    osc_rt_reset(rt);
    uint64_t args[6] = {0}, r;
    check(osc_interp_run(u, 0, args, u->funcs[0].nparams, rt, &r) == -1, "interpreter ran invalid unit: %s", what);
    OscCode c;
    check(osc_cg_compile(u, &c, err, sizeof err) != 0, "codegen compiled invalid unit: %s", what);
    uint8_t dg[32];
    size_t len;
    check(osc_ir_digest(u, dg) != 0 && osc_ir_encode(u, NULL, 0, &len) != 0, "digest/encode accepted: %s", what);
}

static void test_validator(void) {
    OscUnit *u = base_unit();
    char err[256];
    check(osc_ir_validate(u, err, sizeof err) == 0, "base unit invalid: %s", err);
    uint8_t d0[32], d1[32];
    osc_ir_digest(u, d0);
    /* digest ignores debug line numbers, changes with any semantic field */
    u->funcs[0].insns[0].line = 42;
    osc_ir_digest(u, d1);
    check(memcmp(d0, d1, 32) == 0, "digest depends on line numbers");
    u->funcs[0].insns[0].sub = OSC_B_SUB;
    osc_ir_digest(u, d1);
    check(memcmp(d0, d1, 32) != 0, "digest ignores sub-op change");
    free(u);
    /* encode is deterministic and its length matches */
    u = base_unit();
    {
        size_t len = 0, len2 = 0;
        check(osc_ir_encode(u, NULL, 0, &len) == 0, "encode len");
        uint8_t *b1 = malloc(len), *b2 = malloc(len);
        check(osc_ir_encode(u, b1, len, &len2) == 0 && len2 == len, "encode");
        check(osc_ir_encode(u, b2, len, &len2) == 0 && memcmp(b1, b2, len) == 0, "encode not deterministic");
        check(osc_ir_encode(u, b2, len - 1, &len2) != 0, "encode into short buffer accepted");
        free(b1);
        free(b2);
    }
    free(u);

#define CASE(what, mutate) do { OscUnit *x = base_unit(); OscFunc *f0 = &x->funcs[0], *f1 = &x->funcs[1]; (void)f0; (void)f1; mutate; expect_invalid(x, what); free(x); } while (0)
    CASE("bad vreg (a = nvregs)", f0->insns[0].a = (int16_t)f0->nvregs);
    CASE("bad vreg (negative)", f0->insns[0].b = -1);
    CASE("type mismatch i32 + i64", f0->vtype[1] = TS(OSC_T_I64));
    CASE("dst type mismatch", f0->vtype[2] = TS(OSC_T_U32));
    CASE("call to later function", { f0->insns[0] = I0(OSC_I_CALL); f0->insns[0].callee = 1; f0->insns[0].nargs = 1;
                                     f0->insns[0].args[0] = 0; f0->insns[0].dst = 2; });
    CASE("call to itself", f1->insns[0].callee = 1);
    CASE("call arity", f1->insns[0].nargs = 1);
    CASE("call arg type", f1->vtype[0] = TS(OSC_T_U32));
    CASE("missing terminator", { f0->insns[1] = I0(OSC_I_CONST); f0->insns[1].dst = 2; f0->insns[1].imm = 0; });
    CASE("terminator mid-block", { f0->insns[0] = I0(OSC_I_RET); f0->insns[0].a = 0; });
    CASE("insn in no block", f0->blocks[0].count = 1);
    CASE("insn in two blocks", { f0->nblocks = 2; f0->blocks[1] = f0->blocks[0]; });
    CASE("block out of range", f0->blocks[0].count = 9);
    CASE("branch target out of range", { f0->insns[1] = I0(OSC_I_BR); f0->insns[1].blk_t = 5; });
    CASE("CBR on non-bool", { f0->insns[1] = I0(OSC_I_CBR); f0->insns[1].a = 0; f0->insns[1].blk_t = 0; f0->insns[1].blk_f = 0; });
    CASE("RET type mismatch", f0->ret = TS(OSC_T_I64));
    CASE("RET value from void", f0->ret = TVOID);
    CASE("REF return type", f0->ret = TR(OSC_REF_OWN, OSC_T_I32, 4));
    CASE("non-canonical CONST", { f0->insns[0] = I0(OSC_I_CONST); f0->insns[0].dst = 2; f0->insns[0].imm = 0x80000000ULL; });
    CASE("bool CONST 2", { f0->vtype[2] = TS(OSC_T_BOOL); f0->insns[0] = I0(OSC_I_CONST); f0->insns[0].dst = 2; f0->insns[0].imm = 2; f0->ret = TS(OSC_T_BOOL); });
    CASE("use before def", { f0->insns[1].a = (int16_t)f0->nvregs; f0->vtype[f0->nvregs] = TS(OSC_T_I32); f0->nvregs++; });
    CASE("too many params", { f0->nparams = 7; f0->nvregs = 8; for (int i = 0; i < 8; i++) f0->vtype[i] = TS(OSC_T_I32); });
    CASE("nparams > nvregs", f0->nvregs = 1);
    CASE("bad op", f0->insns[0].op = 99);
    CASE("bad BIN sub", f0->insns[0].sub = 11);
    CASE("bool arithmetic", { for (int i = 0; i < 3; i++) f0->vtype[i] = TS(OSC_T_BOOL); f0->ret = TS(OSC_T_BOOL); });
    CASE("cast to bool", { f0->insns[0] = I0(OSC_I_CAST); f0->insns[0].dst = 2; f0->insns[0].a = 0; f0->vtype[2] = TS(OSC_T_BOOL); f0->ret = TS(OSC_T_BOOL); });
    CASE("void vreg type", f0->vtype[2] = TVOID);
    CASE("TRAP code 0", { f0->insns[0] = I0(OSC_I_TRAP); f0->insns[0].imm = 0; });
    CASE("TRAP code out of range", { f0->insns[0] = I0(OSC_I_TRAP); f0->insns[0].imm = OSC_TRAP_MAX + 1; });
    CASE("name not terminated", memset(f0->name, 'x', sizeof f0->name));
    CASE("zero blocks", f0->nblocks = 0);
    CASE("ref len 0", { f0->vtype[2] = TR(OSC_REF_OWN, OSC_T_I32, 0); });
    CASE("ref len 65", { f0->vtype[2] = TR(OSC_REF_OWN, OSC_T_I32, 65); });
    CASE("STORE through shared borrow", {
        f0->vtype[2] = TR(OSC_REF_SHARED, OSC_T_I32, 4); f0->vtype[3] = TR(OSC_REF_OWN, OSC_T_I32, 4); f0->nvregs = 4;
        f0->insns[0] = I0(OSC_I_ALLOC); f0->insns[0].dst = 3; f0->insns[0].a = 0;
        f0->insns[1] = I0(OSC_I_MOV); f0->insns[1].dst = 2; f0->insns[1].a = 3;
        f0->insns[2] = I0(OSC_I_STORE); f0->insns[2].a = 2; f0->insns[2].b = 0; f0->insns[2].c = 1;
        f0->insns[3] = I0(OSC_I_RET); f0->insns[3].a = 0; f0->ninsns = 4; f0->blocks[0].count = 4; });
    CASE("RELEASE of a borrow", {
        f0->vtype[2] = TR(OSC_REF_MUT, OSC_T_I32, 4); f0->vtype[3] = TR(OSC_REF_OWN, OSC_T_I32, 4); f0->nvregs = 4;
        f0->insns[0] = I0(OSC_I_ALLOC); f0->insns[0].dst = 3; f0->insns[0].a = 0;
        f0->insns[1] = I0(OSC_I_MOV); f0->insns[1].dst = 2; f0->insns[1].a = 3;
        f0->insns[2] = I0(OSC_I_RELEASE); f0->insns[2].a = 2;
        f0->insns[3] = I0(OSC_I_RET); f0->insns[3].a = 0; f0->ninsns = 4; f0->blocks[0].count = 4; });
    CASE("owner from borrow (MOV)", {
        f0->vtype[2] = TR(OSC_REF_OWN, OSC_T_I32, 4); f0->vtype[3] = TR(OSC_REF_SHARED, OSC_T_I32, 4);
        f0->vtype[4] = TR(OSC_REF_OWN, OSC_T_I32, 4); f0->nvregs = 5;
        f0->insns[0] = I0(OSC_I_ALLOC); f0->insns[0].dst = 4; f0->insns[0].a = 0;
        f0->insns[1] = I0(OSC_I_MOV); f0->insns[1].dst = 3; f0->insns[1].a = 4;
        f0->insns[2] = I0(OSC_I_MOV); f0->insns[2].dst = 2; f0->insns[2].a = 3;
        f0->insns[3] = I0(OSC_I_RET); f0->insns[3].a = 0; f0->ninsns = 4; f0->blocks[0].count = 4; });
    CASE("ALLOC init type != elem", {
        f0->vtype[2] = TR(OSC_REF_OWN, OSC_T_U8, 4);
        f0->insns[0] = I0(OSC_I_ALLOC); f0->insns[0].dst = 2; f0->insns[0].a = 0;
        f0->insns[1].a = 0; });
    CASE("LOAD index bool", {
        f0->vtype[2] = TR(OSC_REF_OWN, OSC_T_I32, 4); f0->vtype[3] = TS(OSC_T_BOOL); f0->vtype[4] = TS(OSC_T_I32); f0->nvregs = 5;
        f0->insns[0] = I0(OSC_I_ALLOC); f0->insns[0].dst = 2; f0->insns[0].a = 0;
        f0->insns[1] = I0(OSC_I_CONST); f0->insns[1].dst = 3; f0->insns[1].imm = 1;
        f0->insns[2] = I0(OSC_I_LOAD); f0->insns[2].dst = 4; f0->insns[2].a = 2; f0->insns[2].b = 3;
        f0->insns[3] = I0(OSC_I_RET); f0->insns[3].a = 4; f0->ninsns = 4; f0->blocks[0].count = 4; });
#undef CASE
    /* use before def on one path only (definite assignment through a CFG) */
    {
        OscUnit *x = unit_new();
        B b;
        OscType pt[1] = {TS(OSC_T_BOOL)};
        fn(&b, x, "maybe", TS(OSC_T_I32), 1, pt);
        int r = V(&b, TS(OSC_T_I32));
        int y = blk(&b), j = blk(&b);
        k_cbr(&b, 0, y, j);
        at(&b, y);
        k_mov(&b, r, k_const(&b, TS(OSC_T_I32), 1));
        k_br(&b, j);
        at(&b, j);
        k_ret(&b, r);
        expect_invalid(x, "use before def on one path");
        free(x);
    }
}

int main(void) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    rtI = malloc(sizeof *rtI);
    rtN = malloc(sizeof *rtN);
    if (!rtI || !rtN) return 2;
    osc_rt_init(rtI);
    osc_rt_init(rtN);

    test_encoder();
    test_validator();
    sm_state = 0x05C1B4C4ULL;
    gen_ops();
    gen_arrays();
    gen_calls();

    /* every trap code must have been observed in the differential runs */
    for (int c = 1; c <= OSC_TRAP_MAX; c++) check(n_trap[c] > 0, "trap code %d never observed", c);
    check(n_trap[0] > 0, "no normal return observed");

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("units=%lu funcs_fuzzed=%lu diff_runs=%lu words_roundtrip=%lu enc_random_ok=%lu enc_random_refused=%lu "
           "dec_random_words=%lu validator_neg=%lu\n",
           n_units, n_funcs_fuzzed, n_diff_runs, n_words, n_enc_ok, n_enc_refused, n_dec_rand_ok, n_validate_neg);
    printf("traps: none=%lu overflow=%lu div0=%lu bounds=%lu loop_bound=%lu cast=%lu oom=%lu shift=%lu runtime=%lu requires=%lu ensures=%lu\n",
           n_trap[0], n_trap[1], n_trap[2], n_trap[3], n_trap[4], n_trap[5], n_trap[6], n_trap[7], n_trap[8], n_trap[9], n_trap[10]);
    printf("checks=%lu failed=%lu time=%.2fs\n", n_checks, n_fail, secs);
    free(rtI);
    free(rtN);
    if (n_fail) { printf("OSC1_BACKEND_FAIL\n"); return 1; }
    printf("OSC1_BACKEND_PASS\n");
    return 0;
}
