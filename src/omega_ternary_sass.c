#include "omega_ternary_sass.h"
#include <stdio.h>
#include <string.h>

/* =========================================================================
 * Minimal SASS-level sequence: 32-bit virtual registers, straight line with
 * at most one loop. Enough to count instructions and live registers.
 * ========================================================================= */

#define SS_MAX 96
#define SS_NONE (-1)

typedef struct {
    const char *mn;
    int d[2];
    int s[3];
} SInsn;

typedef struct {
    SInsn v[SS_MAX];
    int n;
    int nv;
    int npred;
    int loop_b, loop_e;
    int in[6], nin;
    int out[4], nout;
} SSeq;

static int V(SSeq *q) { return q->nv++; }

static void I(SSeq *q, const char *mn, int d0, int d1, int s0, int s1, int s2) {
    if (q->n >= SS_MAX) return;
    SInsn *x = &q->v[q->n++];
    x->mn = mn;
    x->d[0] = d0; x->d[1] = d1;
    x->s[0] = s0; x->s[1] = s1; x->s[2] = s2;
}

static void P(SSeq *q, int n) { if (n > q->npred) q->npred = n; }
static void loop_begin(SSeq *q) { q->loop_b = q->n; }
static void loop_end(SSeq *q) { q->loop_e = q->n; }

static void seq_init(SSeq *q) {
    memset(q, 0, sizeof(*q));
    q->loop_b = q->loop_e = -1;
}

static void inputs(SSeq *q, int n, int *regs) {
    for (int i = 0; i < n; ++i) { regs[i] = V(q); q->in[q->nin++] = regs[i]; }
}

static void outputs(SSeq *q, int a, int b) {
    q->out[q->nout++] = a;
    if (b != SS_NONE) q->out[q->nout++] = b;
}

/* Peak live registers by interval scan. Values that are read inside the
 * loop before being redefined there are loop-carried: live for the whole loop. */
static uint32_t peak_live(const SSeq *q) {
    int first[128], last[128];
    for (int r = 0; r < q->nv; ++r) { first[r] = q->n; last[r] = -1; }
    for (int i = 0; i < q->nin; ++i) first[q->in[i]] = -1;
    for (int i = 0; i < q->n; ++i) {
        for (int k = 0; k < 2; ++k) if (q->v[i].d[k] >= 0 && i < first[q->v[i].d[k]]) first[q->v[i].d[k]] = i;
        for (int k = 0; k < 3; ++k) if (q->v[i].s[k] >= 0 && i > last[q->v[i].s[k]]) last[q->v[i].s[k]] = i;
    }
    for (int i = 0; i < q->nout; ++i) last[q->out[i]] = q->n;
    if (q->loop_b >= 0) {
        for (int r = 0; r < q->nv; ++r) {
            int first_use = -1, first_def = -1;
            for (int i = q->loop_b; i < q->loop_e; ++i) {
                for (int k = 0; k < 3; ++k) if (q->v[i].s[k] == r && first_use < 0) first_use = i;
                for (int k = 0; k < 2; ++k) if (q->v[i].d[k] == r && first_def < 0) first_def = i;
            }
            if (first_use >= 0 && (first_def < 0 || first_def >= first_use) && last[r] < q->loop_e) last[r] = q->loop_e;
        }
    }
    uint32_t peak = 0;
    for (int p = -1; p < q->n; ++p) {
        uint32_t live = 0;
        for (int r = 0; r < q->nv; ++r) if (first[r] <= p && last[r] > p) live++;
        if (live > peak) peak = live;
    }
    return peak;
}

/* ---------------- shared fragments ---------------- */

/* 64-bit signed clamp of (lo, hi) to [-TW_MAX, TW_MAX] with immediates. */
static void clamp64(SSeq *q, int lo, int hi) {
    I(q, "ISETP.GT.U32.AND P1, PT, lo, TMAX.lo", SS_NONE, SS_NONE, lo, SS_NONE, SS_NONE);
    I(q, "ISETP.GT.AND.EX P1, PT, hi, TMAX.hi, P1", SS_NONE, SS_NONE, hi, SS_NONE, SS_NONE);
    I(q, "SEL lo, TMAX.lo, lo, P1", lo, SS_NONE, lo, SS_NONE, SS_NONE);
    I(q, "SEL hi, TMAX.hi, hi, P1", hi, SS_NONE, hi, SS_NONE, SS_NONE);
    I(q, "ISETP.LT.U32.AND P2, PT, lo, -TMAX.lo", SS_NONE, SS_NONE, lo, SS_NONE, SS_NONE);
    I(q, "ISETP.LT.AND.EX P2, PT, hi, -TMAX.hi, P2", SS_NONE, SS_NONE, hi, SS_NONE, SS_NONE);
    I(q, "SEL lo, -TMAX.lo, lo, P2", lo, SS_NONE, lo, SS_NONE, SS_NONE);
    I(q, "SEL hi, -TMAX.hi, hi, P2", hi, SS_NONE, hi, SS_NONE, SS_NONE);
    P(q, 3);
}

/* 64 x 64 -> high 64 unsigned, into (h0, h1). */
static void mulhi64(SSeq *q, int alo, int ahi, int blo, int bhi, int *h0, int *h1, int *l0) {
    int a0 = V(q), a1 = V(q), b0 = V(q), b1 = V(q), c0 = V(q), c1 = V(q), d0 = V(q), d1 = V(q), m = V(q), m2 = V(q);
    I(q, "IMAD.WIDE.U32 a, alo, blo, RZ", a0, a1, alo, blo, SS_NONE);
    I(q, "IMAD.WIDE.U32 b, alo, bhi, RZ", b0, b1, alo, bhi, SS_NONE);
    I(q, "IMAD.WIDE.U32 c, ahi, blo, RZ", c0, c1, ahi, blo, SS_NONE);
    I(q, "IMAD.WIDE.U32 d, ahi, bhi, RZ", d0, d1, ahi, bhi, SS_NONE);
    I(q, "IADD3 m, P5, a.hi, b.lo, c.lo", m, SS_NONE, a1, b0, c0);
    I(q, "IADD3.X m2, b.hi, c.hi, RZ, P5", m2, SS_NONE, b1, c1, SS_NONE);
    I(q, "IADD3 h0, P6, d.lo, m2, RZ", d0, SS_NONE, d0, m2, SS_NONE);
    I(q, "IADD3.X h1, d.hi, RZ, RZ, P6", d1, SS_NONE, d1, SS_NONE, SS_NONE);
    P(q, 7);
    *h0 = d0; *h1 = d1;
    if (l0) *l0 = a0;
    (void)m;
}

/* ---------------- PLANES realization (two 32-bit planes) ---------------- */

static int build_planes(SSeq *q, TernaryOp op) {
    int r[4];
    bool unary = (op == TOP_TNEG || op == TOP_TSIGN || op == TOP_TSHL || op == TOP_TSHR);
    inputs(q, unary ? 2 : 4, r);
    int ap = r[0], an = r[1], bp = unary ? SS_NONE : r[2], bn = unary ? SS_NONE : r[3];
    switch (op) {
        case TOP_TNEG:
            outputs(q, an, ap); /* negation swaps the planes: a register renaming */
            return 0;
        case TOP_TAND: case TOP_TOR: {
            int rp = V(q), rn = V(q);
            I(q, "LOP3.LUT rp, ap, bp, RZ", rp, SS_NONE, ap, bp, SS_NONE);
            I(q, "LOP3.LUT rn, an, bn, RZ", rn, SS_NONE, an, bn, SS_NONE);
            outputs(q, rp, rn);
            return 0;
        }
        case TOP_TXOR: {
            int t = V(q), rp = V(q), u = V(q), rn = V(q);
            I(q, "LOP3.LUT t, ap, bn, RZ (and)", t, SS_NONE, ap, bn, SS_NONE);
            I(q, "LOP3.LUT rp, t, an, bp (t | an&bp)", rp, SS_NONE, t, an, bp);
            I(q, "LOP3.LUT u, ap, bp, RZ (and)", u, SS_NONE, ap, bp, SS_NONE);
            I(q, "LOP3.LUT rn, u, an, bn (u | an&bn)", rn, SS_NONE, u, an, bn);
            outputs(q, rp, rn);
            return 0;
        }
        case TOP_TSIGN: {
            int rp = V(q), rn = V(q);
            I(q, "ISETP.GT.U32.AND P0, PT, ap, an, PT", SS_NONE, SS_NONE, ap, an, SS_NONE);
            I(q, "ISETP.LT.U32.AND P1, PT, ap, an, PT", SS_NONE, SS_NONE, ap, an, SS_NONE);
            I(q, "SEL rp, 1, RZ, P0", rp, SS_NONE, SS_NONE, SS_NONE, SS_NONE);
            I(q, "SEL rn, 1, RZ, P1", rn, SS_NONE, SS_NONE, SS_NONE, SS_NONE);
            P(q, 2);
            outputs(q, rp, rn);
            return 0;
        }
        case TOP_TCMP: {
            int g1 = V(q), g = V(q), l1 = V(q), l = V(q), rp = V(q), rn = V(q);
            I(q, "LOP3.LUT g1, ap, bp, RZ (a&~b)", g1, SS_NONE, ap, bp, SS_NONE);
            I(q, "LOP3.LUT g, g1, bn, an (g1 | bn&~an)", g, SS_NONE, g1, bn, an);
            I(q, "LOP3.LUT l1, bp, ap, RZ (b&~a)", l1, SS_NONE, bp, ap, SS_NONE);
            I(q, "LOP3.LUT l, l1, an, bn (l1 | an&~bn)", l, SS_NONE, l1, an, bn);
            I(q, "ISETP.GT.U32.AND P0, PT, g, l, PT", SS_NONE, SS_NONE, g, l, SS_NONE);
            I(q, "ISETP.LT.U32.AND P1, PT, g, l, PT", SS_NONE, SS_NONE, g, l, SS_NONE);
            I(q, "SEL rp, 1, RZ, P0", rp, SS_NONE, SS_NONE, SS_NONE, SS_NONE);
            I(q, "SEL rn, 1, RZ, P1", rn, SS_NONE, SS_NONE, SS_NONE, SS_NONE);
            P(q, 2);
            outputs(q, rp, rn);
            return 0;
        }
        case TOP_TSHL: {
            int p0 = V(q), n0 = V(q), o = V(q), sp = V(q), sn = V(q), rp = V(q), rn = V(q);
            I(q, "SHF.L.U32 p0, ap, k, RZ", p0, SS_NONE, ap, SS_NONE, SS_NONE);
            I(q, "SHF.L.U32 n0, an, k, RZ", n0, SS_NONE, an, SS_NONE, SS_NONE);
            I(q, "LOP3.LUT P0, o, ap, an, TOPMASK ((a|b)&m != 0)", o, SS_NONE, ap, an, SS_NONE);
            I(q, "ISETP.GT.U32.AND P1, PT, ap, an, PT", SS_NONE, SS_NONE, ap, an, SS_NONE);
            I(q, "SEL sp, 0xffffffff, RZ, P1", sp, SS_NONE, SS_NONE, SS_NONE, SS_NONE);
            I(q, "SEL sn, RZ, 0xffffffff, P1", sn, SS_NONE, SS_NONE, SS_NONE, SS_NONE);
            I(q, "SEL rp, sp, p0, P0", rp, SS_NONE, sp, p0, SS_NONE);
            I(q, "SEL rn, sn, n0, P0", rn, SS_NONE, sn, n0, SS_NONE);
            P(q, 2);
            outputs(q, rp, rn);
            (void)o;
            return 0;
        }
        case TOP_TSHR: {
            int rp = V(q), rn = V(q);
            I(q, "SHF.R.U32.HI rp, RZ, k, ap", rp, SS_NONE, ap, SS_NONE, SS_NONE);
            I(q, "SHF.R.U32.HI rn, RZ, k, an", rn, SS_NONE, an, SS_NONE, SS_NONE);
            outputs(q, rp, rn);
            return 0;
        }
        case TOP_TADD: case TOP_TSUB: {
            /* TSUB is TADD with b's planes swapped: a renaming. */
            if (op == TOP_TSUB) { int t = bp; bp = bn; bn = t; }
            int e = V(q), pp = V(q), nn = V(q), x1 = V(q), x2 = V(q), y1 = V(q), y2 = V(q), cp = V(q), cn = V(q);
            int sp = V(q), sn = V(q), rp = V(q), rn = V(q);
            I(q, "MOV e, RZ", e, SS_NONE, SS_NONE, SS_NONE, SS_NONE);
            loop_begin(q);
            I(q, "LOP3.LUT pp, ap, bp, RZ (and)", pp, SS_NONE, ap, bp, SS_NONE);
            I(q, "LOP3.LUT nn, an, bn, RZ (and)", nn, SS_NONE, an, bn, SS_NONE);
            I(q, "LOP3.LUT x1, ap, bp, bn (a&~(b|c))", x1, SS_NONE, ap, bp, bn);
            I(q, "LOP3.LUT x2, bp, ap, an (a&~(b|c))", x2, SS_NONE, bp, ap, an);
            I(q, "LOP3.LUT y1, an, bp, bn (a&~(b|c))", y1, SS_NONE, an, bp, bn);
            I(q, "LOP3.LUT y2, bn, ap, an (a&~(b|c))", y2, SS_NONE, bn, ap, an);
            I(q, "LOP3.LUT ap, x1, x2, nn (or3)", ap, SS_NONE, x1, x2, nn);
            I(q, "LOP3.LUT an, y1, y2, pp (or3)", an, SS_NONE, y1, y2, pp);
            I(q, "SHF.R.U32.HI cp, RZ, 31, pp", cp, SS_NONE, pp, SS_NONE, SS_NONE);
            I(q, "SHF.R.U32.HI cn, RZ, 31, nn", cn, SS_NONE, nn, SS_NONE, SS_NONE);
            I(q, "IADD3 e, e, cp, -cn", e, SS_NONE, e, cp, cn);
            I(q, "IADD3 bp, pp, pp, RZ (<<1)", bp, SS_NONE, pp, SS_NONE, SS_NONE);
            I(q, "IADD3 bn, nn, nn, RZ (<<1)", bn, SS_NONE, nn, SS_NONE, SS_NONE);
            I(q, "LOP3.LUT P0, RZ, pp, nn, RZ (or != 0)", SS_NONE, SS_NONE, pp, nn, SS_NONE);
            I(q, "@P0 BRA loop", SS_NONE, SS_NONE, SS_NONE, SS_NONE, SS_NONE);
            loop_end(q);
            I(q, "ISETP.NE.AND P1, PT, e, RZ, PT", SS_NONE, SS_NONE, e, SS_NONE, SS_NONE);
            I(q, "ISETP.GT.AND P2, PT, e, RZ, PT", SS_NONE, SS_NONE, e, SS_NONE, SS_NONE);
            I(q, "SEL sp, 0xffffffff, RZ, P2", sp, SS_NONE, SS_NONE, SS_NONE, SS_NONE);
            I(q, "SEL sn, RZ, 0xffffffff, P2", sn, SS_NONE, SS_NONE, SS_NONE, SS_NONE);
            I(q, "SEL rp, sp, ap, P1", rp, SS_NONE, sp, ap, SS_NONE);
            I(q, "SEL rn, sn, an, P1", rn, SS_NONE, sn, an, SS_NONE);
            P(q, 3);
            outputs(q, rp, rn);
            return 0;
        }
        default:
            return -1; /* TMUL: no PLANES lowering */
    }
}

/* ---------------- INT realization (64-bit register pair) ---------------- */

static int build_int(SSeq *q, TernaryOp op) {
    int r[4];
    bool unary = (op == TOP_TNEG || op == TOP_TSIGN || op == TOP_TSHL || op == TOP_TSHR);
    inputs(q, unary ? 2 : 4, r);
    int alo = r[0], ahi = r[1], blo = unary ? SS_NONE : r[2], bhi = unary ? SS_NONE : r[3];
    int lo = V(q), hi = V(q);
    switch (op) {
        case TOP_TNEG:
            I(q, "IADD3 lo, P0, -alo, RZ, RZ", lo, SS_NONE, alo, SS_NONE, SS_NONE);
            I(q, "IADD3.X hi, ~ahi, RZ, RZ, P0", hi, SS_NONE, ahi, SS_NONE, SS_NONE);
            P(q, 1);
            break;
        case TOP_TADD: case TOP_TSUB:
            I(q, op == TOP_TADD ? "IADD3 lo, P0, alo, blo, RZ" : "IADD3 lo, P0, alo, -blo, RZ", lo, SS_NONE, alo, blo, SS_NONE);
            I(q, op == TOP_TADD ? "IADD3.X hi, ahi, bhi, RZ, P0" : "IADD3.X hi, ahi, ~bhi, RZ, P0", hi, SS_NONE, ahi, bhi, SS_NONE);
            P(q, 1);
            clamp64(q, lo, hi);
            break;
        case TOP_TSHL:
            I(q, "IMAD.WIDE.U32 lo:hi, alo, 3^k, RZ", lo, hi, alo, SS_NONE, SS_NONE);
            I(q, "IMAD hi, ahi, 3^k, hi", hi, SS_NONE, ahi, hi, SS_NONE);
            clamp64(q, lo, hi);
            break;
        case TOP_TMUL: {
            int t = V(q);
            I(q, "IMAD.WIDE.U32 lo:hi, alo, blo, RZ", lo, hi, alo, blo, SS_NONE);
            I(q, "IMAD t, alo, bhi, hi", t, SS_NONE, alo, bhi, hi);
            I(q, "IMAD hi, ahi, blo, t", hi, SS_NONE, ahi, blo, t);
            /* High 64 bits for the exact overflow check, signed corrections included. */
            int h0, h1;
            mulhi64(q, alo, ahi, blo, bhi, &h0, &h1, NULL);
            I(q, "ISETP.LT.AND P3, PT, bhi, RZ, PT", SS_NONE, SS_NONE, bhi, SS_NONE, SS_NONE);
            I(q, "@P3 IADD3 h0, P4, h0, -alo, RZ", h0, SS_NONE, h0, alo, SS_NONE);
            I(q, "@P3 IADD3.X h1, h1, ~ahi, RZ, P4", h1, SS_NONE, h1, ahi, SS_NONE);
            I(q, "ISETP.LT.AND P3, PT, ahi, RZ, PT", SS_NONE, SS_NONE, ahi, SS_NONE, SS_NONE);
            I(q, "@P3 IADD3 h0, P4, h0, -blo, RZ", h0, SS_NONE, h0, blo, SS_NONE);
            I(q, "@P3 IADD3.X h1, h1, ~bhi, RZ, P4", h1, SS_NONE, h1, bhi, SS_NONE);
            int s = V(q), sx = V(q), slo = V(q), shi = V(q);
            I(q, "SHF.R.S32.HI s, RZ, 31, hi", s, SS_NONE, hi, SS_NONE, SS_NONE);
            I(q, "ISETP.NE.AND P2, PT, h0, s, PT", SS_NONE, SS_NONE, h0, s, SS_NONE);
            I(q, "ISETP.NE.OR P2, PT, h1, s, P2", SS_NONE, SS_NONE, h1, s, SS_NONE);
            I(q, "LOP3.LUT sx, ahi, bhi, RZ (xor)", sx, SS_NONE, ahi, bhi, SS_NONE);
            I(q, "ISETP.LT.AND P3, PT, sx, RZ, PT", SS_NONE, SS_NONE, sx, SS_NONE, SS_NONE);
            I(q, "SEL slo, -TMAX.lo, TMAX.lo, P3", slo, SS_NONE, SS_NONE, SS_NONE, SS_NONE);
            I(q, "SEL shi, -TMAX.hi, TMAX.hi, P3", shi, SS_NONE, SS_NONE, SS_NONE, SS_NONE);
            I(q, "SEL lo, slo, lo, P2", lo, SS_NONE, slo, lo, SS_NONE);
            I(q, "SEL hi, shi, hi, P2", hi, SS_NONE, shi, hi, SS_NONE);
            P(q, 7);
            clamp64(q, lo, hi);
            break;
        }
        case TOP_TSHR: {
            /* Round-to-nearest division by 3^k on the magnitude, sign restored. */
            int nlo = V(q), nhi = V(q), ulo = V(q), uhi = V(q), mlo = V(q), mhi = V(q);
            I(q, "ISETP.LT.AND P0, PT, ahi, RZ, PT", SS_NONE, SS_NONE, ahi, SS_NONE, SS_NONE);
            I(q, "IADD3 nlo, P1, -alo, RZ, RZ", nlo, SS_NONE, alo, SS_NONE, SS_NONE);
            I(q, "IADD3.X nhi, ~ahi, RZ, RZ, P1", nhi, SS_NONE, ahi, SS_NONE, SS_NONE);
            I(q, "SEL ulo, nlo, alo, P0", ulo, SS_NONE, nlo, alo, SS_NONE);
            I(q, "SEL uhi, nhi, ahi, P0", uhi, SS_NONE, nhi, ahi, SS_NONE);
            I(q, "MOV mlo, MAGIC.lo", mlo, SS_NONE, SS_NONE, SS_NONE, SS_NONE);
            I(q, "MOV mhi, MAGIC.hi", mhi, SS_NONE, SS_NONE, SS_NONE, SS_NONE);
            int q0, q1;
            mulhi64(q, ulo, uhi, mlo, mhi, &q0, &q1, NULL);
            I(q, "SHF.R.U64 q0, q0, s, q1", q0, SS_NONE, q0, q1, SS_NONE);
            I(q, "SHF.R.U32.HI q1, RZ, s, q1", q1, SS_NONE, q1, SS_NONE, SS_NONE);
            int t0 = V(q), t1 = V(q), rlo = V(q), rhi = V(q);
            I(q, "IMAD.WIDE.U32 t, q0, 3^k, RZ", t0, t1, q0, SS_NONE, SS_NONE);
            I(q, "IMAD t1, q1, 3^k, t1", t1, SS_NONE, q1, t1, SS_NONE);
            I(q, "IADD3 rlo, P3, ulo, -t0, RZ", rlo, SS_NONE, ulo, t0, SS_NONE);
            I(q, "IADD3.X rhi, uhi, ~t1, RZ, P3", rhi, SS_NONE, uhi, t1, SS_NONE);
            I(q, "ISETP.GT.U32.AND P4, PT, rlo, HALF, PT", SS_NONE, SS_NONE, rlo, SS_NONE, SS_NONE);
            I(q, "ISETP.GT.AND.EX P4, PT, rhi, RZ, PT, P4", SS_NONE, SS_NONE, rhi, SS_NONE, SS_NONE);
            I(q, "@P4 IADD3 q0, P5, q0, 1, RZ", q0, SS_NONE, q0, SS_NONE, SS_NONE);
            I(q, "@P4 IADD3.X q1, q1, RZ, RZ, P5", q1, SS_NONE, q1, SS_NONE, SS_NONE);
            int n0 = V(q), n1 = V(q);
            I(q, "IADD3 n0, P6, -q0, RZ, RZ", n0, SS_NONE, q0, SS_NONE, SS_NONE);
            I(q, "IADD3.X n1, ~q1, RZ, RZ, P6", n1, SS_NONE, q1, SS_NONE, SS_NONE);
            I(q, "SEL lo, n0, q0, P0", lo, SS_NONE, n0, q0, SS_NONE);
            I(q, "SEL hi, n1, q1, P0", hi, SS_NONE, n1, q1, SS_NONE);
            P(q, 7);
            break;
        }
        case TOP_TSIGN:
            I(q, "ISETP.LT.AND P1, PT, ahi, RZ, PT", SS_NONE, SS_NONE, ahi, SS_NONE, SS_NONE);
            I(q, "LOP3.LUT P0, RZ, alo, ahi, RZ (or != 0)", SS_NONE, SS_NONE, alo, ahi, SS_NONE);
            I(q, "SEL lo, 1, RZ, P0", lo, SS_NONE, SS_NONE, SS_NONE, SS_NONE);
            I(q, "SEL lo, -1, lo, P1", lo, SS_NONE, lo, SS_NONE, SS_NONE);
            I(q, "SHF.R.S32.HI hi, RZ, 31, lo", hi, SS_NONE, lo, SS_NONE, SS_NONE);
            P(q, 2);
            break;
        case TOP_TCMP:
            I(q, "ISETP.GT.U32.AND P0, PT, alo, blo, PT", SS_NONE, SS_NONE, alo, blo, SS_NONE);
            I(q, "ISETP.GT.AND.EX P0, PT, ahi, bhi, PT, P0", SS_NONE, SS_NONE, ahi, bhi, SS_NONE);
            I(q, "ISETP.LT.U32.AND P1, PT, alo, blo, PT", SS_NONE, SS_NONE, alo, blo, SS_NONE);
            I(q, "ISETP.LT.AND.EX P1, PT, ahi, bhi, PT, P1", SS_NONE, SS_NONE, ahi, bhi, SS_NONE);
            I(q, "SEL lo, 1, RZ, P0", lo, SS_NONE, SS_NONE, SS_NONE, SS_NONE);
            I(q, "SEL lo, -1, lo, P1", lo, SS_NONE, lo, SS_NONE, SS_NONE);
            I(q, "SHF.R.S32.HI hi, RZ, 31, lo", hi, SS_NONE, lo, SS_NONE, SS_NONE);
            P(q, 2);
            break;
        default:
            return -1; /* trit-wise logic: lowered through the PLANES conversions */
    }
    outputs(q, lo, hi);
    return 0;
}

/* ---------------- boundary conversions ---------------- */

static void build_to_planes(SSeq *q) {
    int r[2];
    inputs(q, 2, r);
    int xlo = r[0], xhi = r[1];
    int pos = V(q), neg = V(q), bit = V(q);
    I(q, "MOV pos, RZ", pos, SS_NONE, SS_NONE, SS_NONE, SS_NONE);
    I(q, "MOV neg, RZ", neg, SS_NONE, SS_NONE, SS_NONE, SS_NONE);
    I(q, "MOV bit, 1", bit, SS_NONE, SS_NONE, SS_NONE, SS_NONE);
    loop_begin(q);
    int nlo = V(q), nhi = V(q), ulo = V(q), uhi = V(q), mlo = V(q), mhi = V(q);
    I(q, "ISETP.LT.AND P0, PT, xhi, RZ, PT", SS_NONE, SS_NONE, xhi, SS_NONE, SS_NONE);
    I(q, "IADD3 nlo, P1, -xlo, RZ, RZ", nlo, SS_NONE, xlo, SS_NONE, SS_NONE);
    I(q, "IADD3.X nhi, ~xhi, RZ, RZ, P1", nhi, SS_NONE, xhi, SS_NONE, SS_NONE);
    I(q, "SEL ulo, nlo, xlo, P0", ulo, SS_NONE, nlo, xlo, SS_NONE);
    I(q, "SEL uhi, nhi, xhi, P0", uhi, SS_NONE, nhi, xhi, SS_NONE);
    I(q, "MOV mlo, MAGIC3.lo", mlo, SS_NONE, SS_NONE, SS_NONE, SS_NONE);
    I(q, "MOV mhi, MAGIC3.hi", mhi, SS_NONE, SS_NONE, SS_NONE, SS_NONE);
    int q0, q1;
    mulhi64(q, ulo, uhi, mlo, mhi, &q0, &q1, NULL);
    I(q, "SHF.R.U64 q0, q0, 1, q1", q0, SS_NONE, q0, q1, SS_NONE);
    I(q, "SHF.R.U32.HI q1, RZ, 1, q1", q1, SS_NONE, q1, SS_NONE, SS_NONE);
    int rd = V(q);
    I(q, "IMAD rd, q0, -3, ulo (u - 3q, low word suffices)", rd, SS_NONE, q0, ulo, SS_NONE);
    I(q, "ISETP.EQ.AND P2, PT, rd, 2, PT", SS_NONE, SS_NONE, rd, SS_NONE, SS_NONE);
    I(q, "@P2 IADD3 q0, P3, q0, 1, RZ", q0, SS_NONE, q0, SS_NONE, SS_NONE);
    I(q, "@P2 IADD3.X q1, q1, RZ, RZ, P3", q1, SS_NONE, q1, SS_NONE, SS_NONE);
    I(q, "@P2 MOV rd, -1", rd, SS_NONE, rd, SS_NONE, SS_NONE);
    I(q, "@P0 IADD3 rd, -rd, RZ, RZ (digit sign)", rd, SS_NONE, rd, SS_NONE, SS_NONE);
    I(q, "@P0 IADD3 xlo, P4, -q0, RZ, RZ", xlo, SS_NONE, q0, SS_NONE, SS_NONE);
    I(q, "@P0 IADD3.X xhi, ~q1, RZ, RZ, P4", xhi, SS_NONE, q1, SS_NONE, SS_NONE);
    I(q, "@!P0 MOV xlo, q0", xlo, SS_NONE, q0, SS_NONE, SS_NONE);
    I(q, "@!P0 MOV xhi, q1", xhi, SS_NONE, q1, SS_NONE, SS_NONE);
    I(q, "ISETP.EQ.AND P5, PT, rd, 1, PT", SS_NONE, SS_NONE, rd, SS_NONE, SS_NONE);
    I(q, "@P5 LOP3.LUT pos, pos, bit, RZ (or)", pos, SS_NONE, pos, bit, SS_NONE);
    I(q, "ISETP.EQ.AND P6, PT, rd, -1, PT", SS_NONE, SS_NONE, rd, SS_NONE, SS_NONE);
    I(q, "@P6 LOP3.LUT neg, neg, bit, RZ (or)", neg, SS_NONE, neg, bit, SS_NONE);
    I(q, "IADD3 bit, bit, bit, RZ (<<1)", bit, SS_NONE, bit, SS_NONE, SS_NONE);
    I(q, "LOP3.LUT P0, RZ, xlo, xhi, RZ (or != 0)", SS_NONE, SS_NONE, xlo, xhi, SS_NONE);
    I(q, "@P0 BRA loop", SS_NONE, SS_NONE, SS_NONE, SS_NONE, SS_NONE);
    loop_end(q);
    P(q, 7);
    outputs(q, pos, neg);
}

static void build_from_planes(SSeq *q) {
    int r[2];
    inputs(q, 2, r);
    int pos = r[0], neg = r[1];
    int m = V(q), top = V(q), cnt = V(q), sh = V(q), alo = V(q), ahi = V(q), tp = V(q), tn = V(q), c = V(q);
    I(q, "LOP3.LUT m, pos, neg, RZ (or)", m, SS_NONE, pos, neg, SS_NONE);
    I(q, "FLO.U32 top, m", top, SS_NONE, m, SS_NONE, SS_NONE);
    I(q, "IADD3 cnt, top, 1, RZ", cnt, SS_NONE, top, SS_NONE, SS_NONE);
    I(q, "IADD3 sh, -cnt, 32, RZ", sh, SS_NONE, cnt, SS_NONE, SS_NONE);
    I(q, "SHF.L.U32 pos, pos, sh, RZ", pos, SS_NONE, pos, sh, SS_NONE);
    I(q, "SHF.L.U32 neg, neg, sh, RZ", neg, SS_NONE, neg, sh, SS_NONE);
    I(q, "MOV alo, RZ", alo, SS_NONE, SS_NONE, SS_NONE, SS_NONE);
    I(q, "MOV ahi, RZ", ahi, SS_NONE, SS_NONE, SS_NONE, SS_NONE);
    loop_begin(q);
    I(q, "SHF.R.U32.HI tp, RZ, 31, pos", tp, SS_NONE, pos, SS_NONE, SS_NONE);
    I(q, "SHF.R.U32.HI tn, RZ, 31, neg", tn, SS_NONE, neg, SS_NONE, SS_NONE);
    I(q, "IMAD.WIDE.U32 alo:c, alo, 3, RZ", alo, c, alo, SS_NONE, SS_NONE);
    I(q, "IMAD ahi, ahi, 3, c", ahi, SS_NONE, ahi, c, SS_NONE);
    I(q, "IADD3 alo, P0, alo, tp, -tn", alo, SS_NONE, alo, tp, tn);
    I(q, "IADD3.X ahi, ahi, RZ, RZ, P0 (with borrow)", ahi, SS_NONE, ahi, SS_NONE, SS_NONE);
    I(q, "IADD3 pos, pos, pos, RZ (<<1)", pos, SS_NONE, pos, SS_NONE, SS_NONE);
    I(q, "IADD3 neg, neg, neg, RZ (<<1)", neg, SS_NONE, neg, SS_NONE, SS_NONE);
    I(q, "IADD3 cnt, cnt, -1, RZ", cnt, SS_NONE, cnt, SS_NONE, SS_NONE);
    I(q, "ISETP.NE.AND P1, PT, cnt, RZ, PT", SS_NONE, SS_NONE, cnt, SS_NONE, SS_NONE);
    I(q, "@P1 BRA loop", SS_NONE, SS_NONE, SS_NONE, SS_NONE, SS_NONE);
    loop_end(q);
    P(q, 2);
    outputs(q, alo, ahi);
}

/* ---------------- binary 64-bit counterparts ---------------- */

static int build_binary64(SSeq *q, TernaryOp op) {
    int r[4];
    bool unary = (op == TOP_TNEG || op == TOP_TSIGN || op == TOP_TSHL || op == TOP_TSHR);
    inputs(q, unary ? 2 : 4, r);
    int alo = r[0], ahi = r[1], blo = unary ? SS_NONE : r[2], bhi = unary ? SS_NONE : r[3];
    int lo = V(q), hi = V(q);
    switch (op) {
        case TOP_TNEG:
            I(q, "IADD3 lo, P0, -alo, RZ, RZ", lo, SS_NONE, alo, SS_NONE, SS_NONE);
            I(q, "IADD3.X hi, ~ahi, RZ, RZ, P0", hi, SS_NONE, ahi, SS_NONE, SS_NONE);
            break;
        case TOP_TADD: case TOP_TSUB:
            I(q, "IADD3 lo, P0, alo, blo, RZ", lo, SS_NONE, alo, blo, SS_NONE);
            I(q, "IADD3.X hi, ahi, bhi, RZ, P0", hi, SS_NONE, ahi, bhi, SS_NONE);
            break;
        case TOP_TMUL: {
            int t = V(q);
            I(q, "IMAD.WIDE.U32 lo:hi, alo, blo, RZ", lo, hi, alo, blo, SS_NONE);
            I(q, "IMAD t, alo, bhi, hi", t, SS_NONE, alo, bhi, hi);
            I(q, "IMAD hi, ahi, blo, t", hi, SS_NONE, ahi, blo, t);
            break;
        }
        case TOP_TAND: case TOP_TOR: case TOP_TXOR:
            I(q, "LOP3.LUT lo, alo, blo, RZ", lo, SS_NONE, alo, blo, SS_NONE);
            I(q, "LOP3.LUT hi, ahi, bhi, RZ", hi, SS_NONE, ahi, bhi, SS_NONE);
            break;
        case TOP_TSHL:
            I(q, "SHF.L.U64.HI hi, alo, k, ahi", hi, SS_NONE, alo, ahi, SS_NONE);
            I(q, "SHF.L.U32 lo, alo, k, RZ", lo, SS_NONE, alo, SS_NONE, SS_NONE);
            break;
        case TOP_TSHR:
            I(q, "SHF.R.S64 lo, alo, k, ahi", lo, SS_NONE, alo, ahi, SS_NONE);
            I(q, "SHF.R.S32.HI hi, RZ, k, ahi", hi, SS_NONE, ahi, SS_NONE, SS_NONE);
            break;
        case TOP_TSIGN: case TOP_TCMP:
            /* Three-way results are the same work in either radix. */
            return build_int(q, op) == 0 ? 1 : -1;
        default:
            return -1;
    }
    outputs(q, lo, hi);
    return 0;
}

static void cost_of(const SSeq *q, TSassCost *out) {
    memset(out, 0, sizeof(*out));
    out->supported = true;
    uint32_t loop = (q->loop_b >= 0) ? (uint32_t)(q->loop_e - q->loop_b) : 0;
    out->loop_insns = loop;
    out->fixed_insns = (uint32_t)q->n - loop;
    out->peak_regs = peak_live(q);
    out->preds = (uint32_t)q->npred;
}

static int build(TernaryOp op, TernaryRep rep, SSeq *q) {
    seq_init(q);
    if (op == TSASS_TO_PLANES) { build_to_planes(q); return 0; }
    if (op == TSASS_FROM_PLANES) { build_from_planes(q); return 0; }
    return rep == TREP_PLANES ? build_planes(q, op) : build_int(q, op);
}

int omega_t_sass_op_cost(TernaryOp op, TernaryRep rep, TSassCost *out) {
    static SSeq q;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (build(op, rep, &q) != 0) return 0; /* unsupported: out->supported == false */
    cost_of(&q, out);
    return 0;
}

int omega_t_sass_binary64_cost(TernaryOp op, TSassCost *out) {
    static SSeq q;
    if (!out) return -1;
    seq_init(&q);
    memset(out, 0, sizeof(*out));
    int rc = build_binary64(&q, op);
    if (rc < 0) return -1;
    if (rc == 1) { seq_init(&q); build_int(&q, op); }
    cost_of(&q, out);
    return 0;
}

static uint64_t slcg(uint64_t *s) {
    *s = *s * 6364136223846793005ULL + 1442695040888963407ULL;
    return *s;
}

static int64_t srand_word(uint64_t *s, uint32_t trits) {
    int64_t v = 0;
    for (uint32_t i = 0; i < trits; ++i) v = v * 3 + (int64_t)((slcg(s) >> 33) % 3) - 1;
    return v;
}

double omega_t_sass_mean_iters(TernaryOp op, TernaryRep rep, uint32_t operand_trits, bool warp_max) {
    uint64_t seed = 0xB1AC0000ULL + operand_trits;
    const size_t warps = 2000;
    double total = 0;
    for (size_t w = 0; w < warps; ++w) {
        uint32_t mx = 0, sum = 0;
        for (size_t lane = 0; lane < 32; ++lane) {
            int64_t a = srand_word(&seed, operand_trits), b = srand_word(&seed, operand_trits);
            uint32_t it = 0;
            if (op == TSASS_TO_PLANES || op == TSASS_FROM_PLANES) it = omega_t_trit_len(a);
            else if (rep == TREP_PLANES && op == TOP_TADD) it = omega_t_planes_add_iters(a, b);
            else if (rep == TREP_PLANES && op == TOP_TSUB) it = omega_t_planes_add_iters(a, -b);
            sum += it;
            if (it > mx) mx = it;
        }
        total += warp_max ? (double)mx : (double)sum / 32.0;
    }
    return total / (double)warps;
}

int omega_t_sass_listing(TernaryOp op, TernaryRep rep, char *out, size_t out_len) {
    static SSeq q;
    if (!out || out_len == 0) return -1;
    out[0] = '\0';
    if (build(op, rep, &q) != 0) return -1;
    size_t used = 0;
    for (int i = 0; i < q.n && used + 2 < out_len; ++i) {
        const char *mark = (i == q.loop_b) ? "loop: " : "      ";
        int w = snprintf(out + used, out_len - used, "%s%s\n", mark, q.v[i].mn);
        if (w < 0) break;
        used += (size_t)w;
    }
    return 0;
}
