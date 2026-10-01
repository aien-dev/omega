/*
 * M21 OMEGA_AUTODIFF CPU tests (docs/autodiff/M21_OMEGA_AUTODIFF.md).
 *
 * Four independent checks of every backward rule, all computed here:
 *  1. Contract: the tape's gradient against an exact reference written in
 *     this file from the calculus (double precision, index arithmetic on
 *     shapes only), within the per-op bound of omega_autodiff.h.
 *  2. Bit-exact (approved M21 gate): the backward rule replayed here by
 *     calling the same M20 tensor ops directly, in the tape's order; the
 *     tape's gradient must equal it bit for bit (memcmp).
 *  3. Finite differences (approved M21 gate): central differences of the M20
 *     CPU forward pass (the same tape ops, re-run on perturbed inputs), pure
 *     relative tolerance |tape - fd| <= FD_RTOL * |fd| with step FD_H, on a
 *     separate data set whose gradients are away from zero (or exactly zero).
 *  4. Determinism: two full runs in two contexts give bit-identical
 *     gradients and the same loss value id.
 * Plus the error paths (tape full, shape mismatch, seed shape, state,
 * scratch, dtype, bad ids) and a leak check. Each check computes its own
 * verdict; the in-process counterexamples prove the comparators can fail.
 * No GPU, no device, no libm.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "omega_numeric.h"
#include "omega_tensor.h"
#include "omega_autodiff.h"

static int g_pass, g_fail;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static OmegaTensorCtx *g;

#define FD_H    0.015625   /* 2^-6 */
/* Approved M21 gate (owner, 2026-10-01): "gradients bit-exact against the same
 * M20 operations, plus a finite-difference check within 1e-3 relative error".
 * FD_RTOL is that 1e-3 relative bound, applied with no absolute slack term
 * (fd_ok below). Near-zero gradients are not compared against FD: the FD data
 * sets (gen_fd_data, lin_data_fd) keep every gradient element either >= ~0.1
 * in magnitude or exactly zero, and every element, near zero or not, is
 * covered by the bit-exact layer. Do not raise FD_RTOL or add an absolute term. */
#define FD_RTOL 1e-3
#define U32     5.9604644775390625e-08   /* 2^-24 */
#define MAXE    128

/* ---- tiny helpers (no libm) ---------------------------------------------------- */

static double dabs(double x) { return x < 0 ? -x : x; }

static double dsqrt(double a) {
    if (a <= 0) return 0;
    double y = a > 1 ? a : 1;
    for (int i = 0; i < 200; i++) {
        double ny = 0.5 * (y + a / y);
        if (ny == y) break;
        y = ny;
    }
    return y;
}

/* Approved M21 FD gate: relative error <= 1e-3, no absolute term. */
static bool fd_ok(double an, double fd) { return dabs(an - fd) <= FD_RTOL * dabs(fd); }

static double gamma_k(int k) { return k * U32 / (1.0 - k * U32); }

static int levels(uint64_t m) {
    int L = 1;
    uint64_t cap = 32;
    while (cap < m) { cap *= 32; L++; }
    return L;
}

static uint32_t g_rng = 0x2545F491u;
static float urand(float lo, float hi) {
    g_rng = g_rng * 1664525u + 1013904223u;
    double r = (double)(g_rng >> 8) / 16777216.0;
    return (float)(lo + (hi - lo) * r);
}

static size_t numel(uint32_t rank, const uint64_t *s) {
    size_t n = 1;
    for (uint32_t d = 0; d < rank; d++) n *= (size_t)s[d];
    return n;
}

static OmegaTensor mk(uint32_t rank, const uint64_t *shape, const float *data) {
    OmegaTensor t = { 0, 0 };
    uint64_t dummy = 1;
    int rc = omega_tensor_from_f32(g, rank, rank ? shape : &dummy, data, &t);
    if (rc) printf("mk: rc %d\n", rc);
    return t;
}

static void unravel(size_t flat, uint32_t rank, const uint64_t *s, uint64_t *coord) {
    for (uint32_t d = rank; d-- > 0;) { coord[d] = flat % s[d]; flat /= s[d]; }
}

/* Flat index into an input of shape si for output coordinate (numpy rules). */
static size_t bmap(const uint64_t *oc, uint32_t ro, uint32_t ri, const uint64_t *si) {
    size_t idx = 0;
    for (uint32_t d = 0; d < ri; d++) {
        uint64_t c = si[d] == 1 ? 0 : oc[d + ro - ri];
        idx = idx * si[d] + c;
    }
    return idx;
}

/* D(x): depth the unbroadcast reductions add (header definition). */
static int unb_depth(uint32_t ro, const uint64_t *so, uint32_t ri, const uint64_t *si) {
    int D = 0;
    for (uint32_t d = 0; d < ro - ri; d++) if (so[d] > 1) D += 5 * levels(so[d]);
    for (uint32_t d = 0; d < ri; d++) if (si[d] == 1 && so[d + ro - ri] > 1) D += 5 * levels(so[d + ro - ri]);
    return D;
}

/* Bound check: |an - ref| <= gamma(k) * abs + tiny slack for the double oracle. */
static size_t bound_violations(const float *an, const double *ref, const double *abs_, const int *k,
                               size_t n, size_t *first) {
    size_t bad = 0;
    for (size_t i = 0; i < n; i++) {
        double lim = gamma_k(k[i]) * abs_[i] + 1e-12 * abs_[i] + 1e-30;
        if (!(dabs((double)an[i] - ref[i]) <= lim)) { if (!bad) *first = i; bad++; }
    }
    return bad;
}

/* ---- op cases ---------------------------------------------------------------------- */

enum { DATA_UNIFORM = 0, DATA_AWAY_FROM_ZERO, DATA_OFFSET_FROM_B };

typedef struct {
    const char *name;
    OmegaAdOp   op;
    uint32_t    ra; uint64_t sa[4];
    uint32_t    rb; uint64_t sb[4];     /* rb 0 and op unary: no b        */
    bool        has_b;
    uint32_t    axis; bool keepdims;    /* SUM / MEAN                     */
    uint32_t    rt; uint64_t st[4];     /* BROADCAST target               */
    float       alo, ahi, blo, bhi;
    int         data;
} OpCase;

static const OpCase CASES[] = {
    { "add [3,4]+[4]",        OMEGA_AD_ADD, 2, {3,4}, 1, {4}, true, 0, false, 0, {0}, -2, 2, -2, 2, DATA_UNIFORM },
    { "add [2,1,3]+[4,1]",    OMEGA_AD_ADD, 3, {2,1,3}, 2, {4,1}, true, 0, false, 0, {0}, -2, 2, -2, 2, DATA_UNIFORM },
    { "add [3,4]+[3,4]",      OMEGA_AD_ADD, 2, {3,4}, 2, {3,4}, true, 0, false, 0, {0}, -2, 2, -2, 2, DATA_UNIFORM },
    { "sub [3,4]-[3,1]",      OMEGA_AD_SUB, 2, {3,4}, 2, {3,1}, true, 0, false, 0, {0}, -2, 2, -2, 2, DATA_UNIFORM },
    { "sub [3,4]-[3,4]",      OMEGA_AD_SUB, 2, {3,4}, 2, {3,4}, true, 0, false, 0, {0}, -2, 2, -2, 2, DATA_UNIFORM },
    { "mul [3,4]*[1,4]",      OMEGA_AD_MUL, 2, {3,4}, 2, {1,4}, true, 0, false, 0, {0}, -2, 2, -2, 2, DATA_UNIFORM },
    { "mul [2,3]*[1]",        OMEGA_AD_MUL, 2, {2,3}, 1, {1}, true, 0, false, 0, {0}, -2, 2, -2, 2, DATA_UNIFORM },
    { "div [3,4]/[4]",        OMEGA_AD_DIV, 2, {3,4}, 1, {4}, true, 0, false, 0, {0}, -2, 2, 1, 2, DATA_UNIFORM },
    { "div [3,4]/[3,4]",      OMEGA_AD_DIV, 2, {3,4}, 2, {3,4}, true, 0, false, 0, {0}, -2, 2, 1, 2, DATA_UNIFORM },
    { "max relu [3,4] vs [1]", OMEGA_AD_MAX, 2, {3,4}, 1, {1}, true, 0, false, 0, {0}, -1, 1, 0, 0, DATA_AWAY_FROM_ZERO },
    { "max [3,4] vs [3,4]",   OMEGA_AD_MAX, 2, {3,4}, 2, {3,4}, true, 0, false, 0, {0}, -1, 1, -1, 1, DATA_OFFSET_FROM_B },
    { "sqrt [3,4]",           OMEGA_AD_SQRT, 2, {3,4}, 0, {0}, false, 0, false, 0, {0}, 1, 4, 0, 0, DATA_UNIFORM },
    { "matmul [3,5]x[5,4]",   OMEGA_AD_MATMUL, 2, {3,5}, 2, {5,4}, true, 0, false, 0, {0}, -1, 1, -1, 1, DATA_UNIFORM },
    { "matmul [2,3,5]x[5,4]", OMEGA_AD_MATMUL, 3, {2,3,5}, 2, {5,4}, true, 0, false, 0, {0}, -1, 1, -1, 1, DATA_UNIFORM },
    { "transpose [3,4]",      OMEGA_AD_TRANSPOSE, 2, {3,4}, 0, {0}, false, 0, false, 0, {0}, -2, 2, 0, 0, DATA_UNIFORM },
    { "broadcast [4]->[3,4]", OMEGA_AD_BROADCAST, 1, {4}, 0, {0}, false, 0, false, 2, {3,4}, -2, 2, 0, 0, DATA_UNIFORM },
    { "broadcast [3,1]->[2,3,4]", OMEGA_AD_BROADCAST, 2, {3,1}, 0, {0}, false, 0, false, 3, {2,3,4}, -2, 2, 0, 0, DATA_UNIFORM },
    { "sum [3,4] axis 1",     OMEGA_AD_SUM, 2, {3,4}, 0, {0}, false, 1, false, 0, {0}, -2, 2, 0, 0, DATA_UNIFORM },
    { "sum [3,4] axis 0 keep", OMEGA_AD_SUM, 2, {3,4}, 0, {0}, false, 0, true, 0, {0}, -2, 2, 0, 0, DATA_UNIFORM },
    { "mean [3,4] axis 1",    OMEGA_AD_MEAN, 2, {3,4}, 0, {0}, false, 1, false, 0, {0}, -2, 2, 0, 0, DATA_UNIFORM },
    { "mean [3,4] axis 0 keep", OMEGA_AD_MEAN, 2, {3,4}, 0, {0}, false, 0, true, 0, {0}, -2, 2, 0, 0, DATA_UNIFORM },
};
#define NCASES (sizeof(CASES) / sizeof(CASES[0]))

/* Records the case's op on a fresh tape. Leaves a (and b) require grad. */
static int build(const OpCase *c, OmegaAdTape *t, OmegaTensor ta, OmegaTensor tb,
                 uint32_t *ia, uint32_t *ib, uint32_t *iy) {
    int rc = omega_ad_leaf(t, ta, true, ia);
    if (rc) return rc;
    *ib = OMEGA_AD_NONE;
    if (c->has_b) { rc = omega_ad_leaf(t, tb, true, ib); if (rc) return rc; }
    switch (c->op) {
    case OMEGA_AD_ADD: case OMEGA_AD_SUB: case OMEGA_AD_MUL: case OMEGA_AD_DIV: case OMEGA_AD_MAX:
        return omega_ad_binary(t, c->op, *ia, *ib, iy);
    case OMEGA_AD_SQRT: return omega_ad_sqrt(t, *ia, iy);
    case OMEGA_AD_MATMUL: return omega_ad_matmul(t, *ia, *ib, iy);
    case OMEGA_AD_TRANSPOSE: return omega_ad_transpose(t, *ia, iy);
    case OMEGA_AD_BROADCAST: return omega_ad_broadcast(t, *ia, c->rt, c->st, iy);
    case OMEGA_AD_SUM: case OMEGA_AD_MEAN: return omega_ad_reduce(t, c->op, *ia, c->axis, c->keepdims, iy);
    default: return OMEGA_AD_ERR_BAD_ARGS;
    }
}

/* Forward of the case on the M20 CPU tier; returns L = sum(y * R) in double. */
static int forward_loss(const OpCase *c, const float *a, const float *b, const float *R, size_t ny,
                        double *L) {
    OmegaAdNode nodes[4];
    OmegaAdTape t;
    OmegaTensor ta = mk(c->ra, c->sa, a), tb = { 0, 0 };
    if (c->has_b) tb = mk(c->rb, c->sb, b);
    int rc = omega_ad_tape_init(&t, g, nodes, 4, NULL, 0);
    uint32_t ia = 0, ib = 0, iy = 0;
    if (!rc) rc = build(c, &t, ta, tb, &ia, &ib, &iy);
    float y[MAXE];
    OmegaTensor ty = { 0, 0 };
    if (!rc) rc = omega_ad_value(&t, iy, &ty);
    if (!rc) rc = omega_tensor_read_f32(g, ty, y, ny);
    if (!rc) {
        double s = 0;
        for (size_t i = 0; i < ny; i++) s += (double)y[i] * (double)R[i];
        *L = s;
    }
    omega_ad_tape_release(&t);
    omega_tensor_release(g, ta);
    if (c->has_b) omega_tensor_release(g, tb);
    return rc;
}

/* Exact reference gradients and bound terms for one case. */
static void reference(const OpCase *c, const float *a, const float *b, const float *R,
                      uint32_t ry, const uint64_t *sy,
                      double *ra_, double *aa, int *ka, double *rb_, double *ab, int *kb) {
    size_t na = numel(c->ra, c->sa), nb = c->has_b ? numel(c->rb, c->sb) : 0, ny = numel(ry, sy);
    for (size_t i = 0; i < na; i++) { ra_[i] = 0; aa[i] = 0; ka[i] = 0; }
    for (size_t i = 0; i < nb; i++) { rb_[i] = 0; ab[i] = 0; kb[i] = 0; }
    uint64_t oc[8], ic[8];
    switch (c->op) {
    case OMEGA_AD_ADD: case OMEGA_AD_SUB: case OMEGA_AD_MUL: case OMEGA_AD_DIV: case OMEGA_AD_MAX:
    case OMEGA_AD_BROADCAST: {
        int Da = unb_depth(ry, sy, c->ra, c->sa);
        int Db = c->has_b ? unb_depth(ry, sy, c->rb, c->sb) : 0;
        for (size_t j = 0; j < ny; j++) {
            unravel(j, ry, sy, oc);
            size_t xa = bmap(oc, ry, c->ra, c->sa);
            size_t xb = c->has_b ? bmap(oc, ry, c->rb, c->sb) : 0;
            double gv = R[j], av = a[xa], bv = c->has_b ? b[xb] : 0, ta = 0, tb = 0;
            int ea = Da, eb = Db;
            switch (c->op) {
            case OMEGA_AD_ADD: ta = gv; tb = gv; break;
            case OMEGA_AD_SUB: ta = gv; tb = -gv; break;
            case OMEGA_AD_MUL: ta = gv * bv; tb = gv * av; ea += 1; eb += 1; break;
            case OMEGA_AD_DIV: ta = gv / bv; tb = -gv * av / (bv * bv); ea += 1; eb += 3; break;
            case OMEGA_AD_MAX: {
                bool to_a = av == av && (bv != bv || av > bv);
                ta = to_a ? gv : 0; tb = to_a ? 0 : gv;
                break;
            }
            default: ta = gv; break; /* BROADCAST */
            }
            ra_[xa] += ta; aa[xa] += dabs(ta); ka[xa] = ea;
            if (c->has_b) { rb_[xb] += tb; ab[xb] += dabs(tb); kb[xb] = eb; }
        }
        break;
    }
    case OMEGA_AD_SQRT:
        for (size_t i = 0; i < na; i++) {
            ra_[i] = (double)R[i] / (2.0 * dsqrt(a[i]));
            aa[i] = dabs(ra_[i]);
            ka[i] = 2;
        }
        break;
    case OMEGA_AD_MATMUL: {
        /* a: [Bt,M,K] (Bt = 1 for rank 2), b: [K,N] */
        size_t Bt = c->ra == 3 ? c->sa[0] : 1, M = c->sa[c->ra - 2], K = c->sa[c->ra - 1], N = c->sb[1];
        int kA = 1 + 5 * levels(N), kB = 1 + 5 * levels(M) + (Bt > 1 ? 5 * levels(Bt) : 0);
        for (size_t q = 0; q < Bt; q++)
            for (size_t i = 0; i < M; i++)
                for (size_t k = 0; k < K; k++) {
                    double s = 0, sa = 0;
                    for (size_t j = 0; j < N; j++) {
                        double p = (double)R[(q * M + i) * N + j] * b[k * N + j];
                        s += p; sa += dabs(p);
                    }
                    size_t x = (q * M + i) * K + k;
                    ra_[x] = s; aa[x] = sa; ka[x] = kA;
                }
        for (size_t k = 0; k < K; k++)
            for (size_t j = 0; j < N; j++) {
                double s = 0, sa = 0;
                for (size_t q = 0; q < Bt; q++)
                    for (size_t i = 0; i < M; i++) {
                        double p = (double)a[(q * M + i) * K + k] * R[(q * M + i) * N + j];
                        s += p; sa += dabs(p);
                    }
                rb_[k * N + j] = s; ab[k * N + j] = sa; kb[k * N + j] = kB;
            }
        break;
    }
    case OMEGA_AD_TRANSPOSE: {
        size_t M = c->sa[0], N = c->sa[1];
        for (size_t i = 0; i < M; i++)
            for (size_t j = 0; j < N; j++) {
                ra_[i * N + j] = R[j * M + i];
                aa[i * N + j] = dabs(R[j * M + i]);
            }
        break;
    }
    case OMEGA_AD_SUM: case OMEGA_AD_MEAN: {
        double n = (double)c->sa[c->axis];
        for (size_t i = 0; i < na; i++) {
            unravel(i, c->ra, c->sa, ic);
            size_t j = 0;
            for (uint32_t d = 0; d < c->ra; d++) {
                if (d == c->axis) continue; /* keepdims: size-1 axis adds nothing */
                j = j * c->sa[d] + ic[d];
            }
            ra_[i] = c->op == OMEGA_AD_SUM ? (double)R[j] : (double)R[j] / n;
            aa[i] = dabs(ra_[i]);
            ka[i] = c->op == OMEGA_AD_SUM ? 0 : 1;
        }
        break;
    }
    default: break;
    }
}

static void gen_data(const OpCase *c, float *a, float *b) {
    size_t na = numel(c->ra, c->sa), nb = c->has_b ? numel(c->rb, c->sb) : 0;
    for (size_t i = 0; i < nb; i++) b[i] = urand(c->blo, c->bhi);
    for (size_t i = 0; i < na; i++) {
        switch (c->data) {
        case DATA_AWAY_FROM_ZERO: {
            float m = urand(0.1f, 1.0f);
            a[i] = (urand(0, 1) < 0.5f) ? -m : m;
            break;
        }
        case DATA_OFFSET_FROM_B: {
            float m = urand(0.1f, 0.6f);
            a[i] = b[i] + ((urand(0, 1) < 0.5f) ? -m : m);
            break;
        }
        default: a[i] = urand(c->alo, c->ahi);
        }
    }
}

/*
 * FD data set. With the FD seed R in [0.5, 1], every gradient element is a
 * sum of same-sign terms of magnitude >= ~0.1, or exactly zero (MAX routing,
 * where the perturbed forward is unchanged and fd is exactly 0 too), so the
 * pure relative bound compares meaningful values. ADD/SUB/BROADCAST/SUM/MEAN/
 * TRANSPOSE/MAX gradients depend only on R; SQRT keeps a in [1, 4]; MUL,
 * DIV and MATMUL get positive operands here.
 */
#define FD_RLO 0.5f
#define FD_RHI 1.0f
static void gen_fd_data(const OpCase *c, float *a, float *b) {
    size_t na = numel(c->ra, c->sa), nb = c->has_b ? numel(c->rb, c->sb) : 0;
    gen_data(c, a, b);
    switch (c->op) {
    case OMEGA_AD_MUL: case OMEGA_AD_MATMUL:
        for (size_t i = 0; i < na; i++) a[i] = urand(0.5f, 1.0f);
        for (size_t i = 0; i < nb; i++) b[i] = urand(0.5f, 1.0f);
        break;
    case OMEGA_AD_DIV: /* b stays in [1, 2] */
        for (size_t i = 0; i < na; i++) a[i] = urand(1.0f, 2.0f);
        break;
    default: break;
    }
}

/* Records the case on a fresh tape, seeds backward with R drawn from
 * [rlo, rhi] (y's shape, written to R), reads the gradients of a (and b). */
static int tape_grads(const OpCase *c, const float *a, const float *b, float rlo, float rhi,
                      float *R, size_t *ny, float *ga, float *gb) {
    OmegaAdNode nodes[4];
    OmegaAdTape t;
    float scratch[3 * MAXE];
    size_t na = numel(c->ra, c->sa), nb = c->has_b ? numel(c->rb, c->sb) : 0;
    memset(&t, 0, sizeof(t));
    OmegaTensor ta = mk(c->ra, c->sa, a), tb = { 0, 0 }, ty = { 0, 0 }, tr = { 0, 0 };
    if (c->has_b) tb = mk(c->rb, c->sb, b);
    OmegaTensorInfo yi;
    memset(&yi, 0, sizeof(yi));
    uint32_t ia = 0, ib = 0, iy = 0;
    int rc = omega_ad_tape_init(&t, g, nodes, 4, scratch, 3 * MAXE);
    if (!rc) rc = build(c, &t, ta, tb, &ia, &ib, &iy);
    if (!rc) rc = omega_ad_value(&t, iy, &ty);
    if (!rc) rc = omega_tensor_info(g, ty, &yi);
    if (!rc) {
        *ny = (size_t)yi.elements;
        for (size_t i = 0; i < *ny; i++) R[i] = urand(rlo, rhi);
        tr = mk(yi.rank, yi.shape, R);
        rc = omega_ad_backward_seed(&t, iy, tr);
        omega_tensor_release(g, tr);
    }
    if (!rc) rc = omega_ad_grad_read_f32(&t, ia, ga, na);
    if (!rc && c->has_b) rc = omega_ad_grad_read_f32(&t, ib, gb, nb);
    omega_ad_tape_release(&t);
    omega_tensor_release(g, ta);
    if (c->has_b) omega_tensor_release(g, tb);
    return rc;
}

/* ---- bit-exact layer: each backward rule replayed with direct M20 calls ----------- */

static int rp_scalar(float v, OmegaTensor *out) {
    uint64_t dummy = 1;
    return omega_tensor_from_f32(g, 0, &dummy, &v, out);
}

/* Reads x into out and releases x. */
static int rp_take(OmegaTensor x, float *out, size_t n) {
    int rc = omega_tensor_read_f32(g, x, out, n);
    omega_tensor_release(g, x);
    return rc;
}

/* Sums `full` (consumed) back to shape (rt, st) in the tape's order: extra
 * leading axes first (reduce SUM over axis 0, one at a time), then every
 * size-1 axis of the target (reduce SUM, keepdims), and reads the result. */
static int rp_out(OmegaTensor full, uint32_t rt, const uint64_t *st, float *out) {
    OmegaTensor cur = full;
    OmegaTensorInfo ci;
    int rc = omega_tensor_info(g, cur, &ci);
    while (!rc && ci.rank > rt) {
        OmegaTensor nx;
        rc = omega_tensor_reduce(g, OMEGA_TR_SUM, cur, 0, false, &nx);
        omega_tensor_release(g, cur);
        if (rc) return rc;
        cur = nx;
        rc = omega_tensor_info(g, cur, &ci);
    }
    for (uint32_t d = 0; !rc && d < rt; d++) {
        if (st[d] == 1 && ci.shape[d] != 1) {
            OmegaTensor nx;
            rc = omega_tensor_reduce(g, OMEGA_TR_SUM, cur, d, true, &nx);
            omega_tensor_release(g, cur);
            if (rc) return rc;
            cur = nx;
            rc = omega_tensor_info(g, cur, &ci);
        }
    }
    if (rc) { omega_tensor_release(g, cur); return rc; }
    return rp_take(cur, out, numel(rt, st));
}

/*
 * Expected gradients ea (and eb) for seed tg: the backward formula of the
 * case's op computed here with the same M20 calls the tape is documented to
 * make (omega_autodiff.h), in the same order. MAX routing is the documented
 * host data move, with the routing rule written here independently.
 */
static int replay(const OpCase *c, OmegaTensor ta, OmegaTensor tb, OmegaTensor tg,
                  const OmegaTensorInfo *yi, float *ea, float *eb) {
    OmegaTensor x = { 0, 0 }, x2 = { 0, 0 }, k = { 0, 0 };
    int rc = 0;
    switch (c->op) {
    case OMEGA_AD_ADD: case OMEGA_AD_SUB:
        rc = omega_tensor_contiguous(g, tg, &x);
        if (!rc) rc = rp_out(x, c->ra, c->sa, ea);
        if (rc) return rc;
        if (c->op == OMEGA_AD_ADD) {
            rc = omega_tensor_contiguous(g, tg, &x);
        } else {
            rc = rp_scalar(-1.0f, &k);
            if (rc) return rc;
            rc = omega_tensor_binary(g, OMEGA_TB_MUL, tg, k, &x);
            omega_tensor_release(g, k);
        }
        if (!rc) rc = rp_out(x, c->rb, c->sb, eb);
        return rc;
    case OMEGA_AD_MUL:
        rc = omega_tensor_binary(g, OMEGA_TB_MUL, tg, tb, &x);
        if (!rc) rc = rp_out(x, c->ra, c->sa, ea);
        if (!rc) rc = omega_tensor_binary(g, OMEGA_TB_MUL, tg, ta, &x);
        if (!rc) rc = rp_out(x, c->rb, c->sb, eb);
        return rc;
    case OMEGA_AD_DIV: {
        OmegaTensor y = { 0, 0 };
        rc = omega_tensor_binary(g, OMEGA_TB_DIV, tg, tb, &x);             /* g / b */
        if (!rc) rc = rp_out(x, c->ra, c->sa, ea);
        if (rc) return rc;
        rc = omega_tensor_binary(g, OMEGA_TB_DIV, ta, tb, &y);             /* y = a / b */
        if (rc) return rc;
        rc = omega_tensor_binary(g, OMEGA_TB_DIV, y, tb, &x2);             /* y / b */
        omega_tensor_release(g, y);
        if (rc) return rc;
        rc = omega_tensor_binary(g, OMEGA_TB_MUL, tg, x2, &x);             /* g * (y / b) */
        omega_tensor_release(g, x2);
        if (rc) return rc;
        rc = rp_scalar(-1.0f, &k);
        if (rc) { omega_tensor_release(g, x); return rc; }
        rc = omega_tensor_binary(g, OMEGA_TB_MUL, x, k, &x2);              /* * -1 */
        omega_tensor_release(g, k);
        omega_tensor_release(g, x);
        if (!rc) rc = rp_out(x2, c->rb, c->sb, eb);
        return rc;
    }
    case OMEGA_AD_MAX: {
        size_t m = (size_t)yi->elements;
        float va[MAXE], vb[MAXE], vg[MAXE], da[MAXE], db[MAXE];
        rc = omega_tensor_broadcast_to(g, ta, yi->rank, yi->shape, &x);
        if (!rc) rc = rp_take(x, va, m);
        if (!rc) rc = omega_tensor_broadcast_to(g, tb, yi->rank, yi->shape, &x);
        if (!rc) rc = rp_take(x, vb, m);
        if (!rc) rc = omega_tensor_read_f32(g, tg, vg, m);
        if (rc) return rc;
        for (size_t i = 0; i < m; i++) {
            bool to_a = va[i] == va[i] && (vb[i] != vb[i] || va[i] > vb[i]);
            da[i] = to_a ? vg[i] : 0.0f;
            db[i] = to_a ? 0.0f : vg[i];
        }
        rc = omega_tensor_from_f32(g, yi->rank, yi->shape, da, &x);
        if (!rc) rc = rp_out(x, c->ra, c->sa, ea);
        if (!rc) rc = omega_tensor_from_f32(g, yi->rank, yi->shape, db, &x);
        if (!rc) rc = rp_out(x, c->rb, c->sb, eb);
        return rc;
    }
    case OMEGA_AD_SQRT: {
        OmegaTensor y = { 0, 0 };
        rc = omega_tensor_unary(g, OMEGA_TU_SQRT, ta, &y);
        if (rc) return rc;
        rc = omega_tensor_binary(g, OMEGA_TB_ADD, y, y, &x2);              /* y + y */
        omega_tensor_release(g, y);
        if (rc) return rc;
        rc = omega_tensor_binary(g, OMEGA_TB_DIV, tg, x2, &x);             /* g / (y + y) */
        omega_tensor_release(g, x2);
        if (!rc) rc = rp_out(x, c->ra, c->sa, ea);
        return rc;
    }
    case OMEGA_AD_MATMUL:
        rc = omega_tensor_transpose(g, tb, &x2);
        if (rc) return rc;
        rc = omega_tensor_matmul(g, tg, x2, &x);                            /* g @ B^T */
        omega_tensor_release(g, x2);
        if (!rc) rc = rp_out(x, c->ra, c->sa, ea);
        if (rc) return rc;
        rc = omega_tensor_transpose(g, ta, &x2);
        if (rc) return rc;
        rc = omega_tensor_matmul(g, x2, tg, &x);                            /* A^T @ g */
        omega_tensor_release(g, x2);
        if (!rc) rc = rp_out(x, c->rb, c->sb, eb);
        return rc;
    case OMEGA_AD_TRANSPOSE:
        rc = omega_tensor_transpose(g, tg, &x2);
        if (rc) return rc;
        rc = omega_tensor_contiguous(g, x2, &x);
        omega_tensor_release(g, x2);
        if (!rc) rc = rp_out(x, c->ra, c->sa, ea);
        return rc;
    case OMEGA_AD_BROADCAST:
        rc = omega_tensor_contiguous(g, tg, &x);
        if (!rc) rc = rp_out(x, c->ra, c->sa, ea);
        return rc;
    case OMEGA_AD_SUM: case OMEGA_AD_MEAN: {
        uint64_t ks[4];
        OmegaTensor r = { 0, 0 }, bc = { 0, 0 };
        for (uint32_t d = 0; d < c->ra; d++) ks[d] = d == c->axis ? 1 : c->sa[d];
        rc = omega_tensor_reshape(g, tg, c->ra, ks, &r);
        if (rc) return rc;
        rc = omega_tensor_broadcast_to(g, r, c->ra, c->sa, &bc);
        if (rc) { omega_tensor_release(g, r); return rc; }
        rc = omega_tensor_contiguous(g, bc, &x2);
        omega_tensor_release(g, bc);
        omega_tensor_release(g, r);
        if (rc) return rc;
        if (c->op == OMEGA_AD_SUM) return rp_out(x2, c->ra, c->sa, ea);
        rc = rp_scalar((float)c->sa[c->axis], &k);
        if (rc) { omega_tensor_release(g, x2); return rc; }
        rc = omega_tensor_binary(g, OMEGA_TB_DIV, x2, k, &x);              /* / n */
        omega_tensor_release(g, k);
        omega_tensor_release(g, x2);
        if (!rc) rc = rp_out(x, c->ra, c->sa, ea);
        return rc;
    }
    default:
        return OMEGA_AD_ERR_BAD_ARGS;
    }
}

static void test_op_case(const OpCase *c) {
    float a[MAXE], b[MAXE], R[MAXE], ga[MAXE], gb[MAXE];
    double ra_[MAXE], aa[MAXE], rb_[MAXE], ab[MAXE];
    int ka[MAXE], kb[MAXE];
    size_t na = numel(c->ra, c->sa), nb = c->has_b ? numel(c->rb, c->sb) : 0;
    gen_data(c, a, b);

    OmegaAdNode nodes[4];
    OmegaAdTape t;
    float scratch[3 * MAXE];
    OmegaTensor ta = mk(c->ra, c->sa, a), tb = { 0, 0 };
    if (c->has_b) tb = mk(c->rb, c->sb, b);
    int rc = omega_ad_tape_init(&t, g, nodes, 4, scratch, sizeof(scratch) / sizeof(scratch[0]));
    uint32_t ia = 0, ib = 0, iy = 0;
    if (!rc) rc = build(c, &t, ta, tb, &ia, &ib, &iy);
    CHECK(rc == 0, "%s: record rc %d", c->name, rc);
    if (rc) { omega_ad_tape_release(&t); goto out; }
    OmegaTensor ty = { 0, 0 };
    OmegaTensorInfo yi;
    memset(&yi, 0, sizeof(yi));
    omega_ad_value(&t, iy, &ty);
    omega_tensor_info(g, ty, &yi);
    size_t ny = (size_t)yi.elements;
    for (size_t i = 0; i < ny; i++) R[i] = urand(-1, 1);
    OmegaTensor tr = mk(yi.rank, yi.shape, R);
    rc = omega_ad_backward_seed(&t, iy, tr);
    CHECK(rc == 0, "%s: backward rc %d", c->name, rc);
    int r1 = omega_ad_grad_read_f32(&t, ia, ga, na);
    int r2 = c->has_b ? omega_ad_grad_read_f32(&t, ib, gb, nb) : 0;
    CHECK(r1 == 0 && r2 == 0, "%s: grad read rc %d %d (grad shape must equal input shape)", c->name, r1, r2);
    omega_ad_tape_release(&t);
    if (rc || r1 || r2) { omega_tensor_release(g, tr); goto out; }

    /* bit-exact (approved M21 gate): same M20 ops called directly, same bits */
    float ea[MAXE], eb[MAXE];
    int rr = replay(c, ta, tb, tr, &yi, ea, eb);
    omega_tensor_release(g, tr);
    CHECK(rr == 0, "%s: bit-exact replay rc %d", c->name, rr);
    if (!rr) {
        CHECK(memcmp(ga, ea, na * sizeof(float)) == 0,
              "%s: grad a not bit-exact with the direct M20 replay", c->name);
        if (c->has_b)
            CHECK(memcmp(gb, eb, nb * sizeof(float)) == 0,
                  "%s: grad b not bit-exact with the direct M20 replay", c->name);
        if (c == &CASES[0]) { /* counterexample: a one-bit change must be seen */
            uint32_t bits;
            memcpy(&bits, &ea[0], 4); bits ^= 1u; memcpy(&ea[0], &bits, 4);
            CHECK(memcmp(ga, ea, na * sizeof(float)) != 0, "bit-exact comparator missed a one-bit difference");
        }
    }

    /* 1. contract */
    reference(c, a, b, R, yi.rank, yi.shape, ra_, aa, ka, rb_, ab, kb);
    size_t first = 0, bad = bound_violations(ga, ra_, aa, ka, na, &first);
    CHECK(bad == 0, "%s: grad a outside contract at %zu elements (first %zu: %.9g vs %.9g)", c->name, bad,
          first, (double)ga[first], ra_[first]);
    if (c->has_b) {
        bad = bound_violations(gb, rb_, ab, kb, nb, &first);
        CHECK(bad == 0, "%s: grad b outside contract at %zu elements (first %zu: %.9g vs %.9g)", c->name,
              bad, first, (double)gb[first], rb_[first]);
    }

    /* central finite differences of the CPU forward (approved M21 gate), on
     * the FD data set: a fresh tape, its own inputs and a positive seed */
    float fa[MAXE], fb[MAXE], fR[MAXE], fga[MAXE], fgb[MAXE];
    size_t fny = 0;
    gen_fd_data(c, fa, fb);
    int rf = tape_grads(c, fa, fb, FD_RLO, FD_RHI, fR, &fny, fga, fgb);
    CHECK(rf == 0, "%s: FD data set tape rc %d", c->name, rf);
    if (rf) goto out;
    for (int which = 0; which < (c->has_b ? 2 : 1); which++) {
        float *x = which ? fb : fa;
        const float *an = which ? fgb : fga;
        size_t nx = which ? nb : na, fbad = 0, ffirst = 0;
        double fdv = 0;
        for (size_t i = 0; i < nx; i++) {
            float x0 = x[i], xp = (float)(x0 + FD_H), xm = (float)(x0 - FD_H);
            double Lp = 0, Lm = 0;
            x[i] = xp;
            int e1 = forward_loss(c, fa, fb, fR, fny, &Lp);
            x[i] = xm;
            int e2 = forward_loss(c, fa, fb, fR, fny, &Lm);
            x[i] = x0;
            double fd = (Lp - Lm) / ((double)xp - (double)xm);
            if (e1 || e2 || !fd_ok((double)an[i], fd)) {
                if (!fbad) { ffirst = i; fdv = fd; }
                fbad++;
            }
        }
        CHECK(fbad == 0, "%s: grad %c disagrees with central FD at %zu elements (first %zu: %.9g vs fd %.9g)",
              c->name, which ? 'b' : 'a', fbad, ffirst, (double)an[ffirst], fdv);
    }
out:
    omega_tensor_release(g, ta);
    if (c->has_b) omega_tensor_release(g, tb);
}

/* Counterexamples: the comparators reject a wrong gradient. */
static void test_comparators_reject(void) {
    const OpCase *c = &CASES[5]; /* mul [3,4]*[1,4] */
    float a[MAXE], b[MAXE], R[MAXE], wrong[MAXE];
    double ra_[MAXE], aa[MAXE], rb_[MAXE], ab[MAXE];
    int ka[MAXE], kb[MAXE];
    gen_data(c, a, b);
    uint64_t sy[2] = { 3, 4 };
    for (int i = 0; i < 12; i++) R[i] = urand(-1, 1);
    reference(c, a, b, R, 2, sy, ra_, aa, ka, rb_, ab, kb);
    /* off by one float ulp-ish: 1 + 2^-20 relative, far above gamma(1) */
    for (int i = 0; i < 12; i++) wrong[i] = (float)(ra_[i] * (1.0 + 1.0 / 1048576.0));
    size_t first = 0;
    CHECK(bound_violations(wrong, ra_, aa, ka, 12, &first) > 0, "contract comparator accepted a perturbed gradient");
    for (int i = 0; i < 12; i++) wrong[i] = (float)ra_[i];
    CHECK(bound_violations(wrong, ra_, aa, ka, 12, &first) == 0, "contract comparator rejected the exact gradient");
    /* FD comparator (pure 1e-3 relative), on the FD data set: it accepts the
     * exact gradient, and rejects a sign-flipped one and one 2e-3 too large */
    gen_fd_data(c, a, b);
    for (int i = 0; i < 12; i++) R[i] = urand(FD_RLO, FD_RHI);
    reference(c, a, b, R, 2, sy, ra_, aa, ka, rb_, ab, kb);
    size_t fbad = 0, facc = 0, fbig = 0;
    for (size_t i = 0; i < 12; i++) {
        float x0 = a[i], xp = (float)(x0 + FD_H), xm = (float)(x0 - FD_H);
        double Lp = 0, Lm = 0;
        a[i] = xp; forward_loss(c, a, b, R, 12, &Lp);
        a[i] = xm; forward_loss(c, a, b, R, 12, &Lm);
        a[i] = x0;
        double fd = (Lp - Lm) / ((double)xp - (double)xm);
        if (fd_ok(ra_[i], fd)) facc++;
        if (!fd_ok(-ra_[i], fd)) fbad++;
        if (!fd_ok(ra_[i] * 1.002, fd)) fbig++;
    }
    CHECK(facc == 12, "FD comparator rejected the exact gradient at %zu of 12 elements", 12 - facc);
    CHECK(fbad > 0, "FD comparator accepted a sign-flipped gradient");
    CHECK(fbig == 12, "FD comparator accepted a gradient 2e-3 too large at %zu of 12 elements", 12 - fbig);
}

/* MAX ties go to b (relu gradient 0 at 0). */
static void test_max_ties(void) {
    uint64_t s[1] = { 4 };
    float a[4] = { 0.5f, -0.25f, 0.0f, -0.0f }, b[4] = { 0.5f, -0.25f, -0.0f, 0.0f }, R[4] = { 1, 2, 3, 4 };
    OmegaAdNode nodes[3];
    OmegaAdTape t;
    float scratch[12], ga[4] = { 9, 9, 9, 9 }, gb[4] = { 9, 9, 9, 9 };
    OmegaTensor ta = mk(1, s, a), tb = mk(1, s, b), tr = mk(1, s, R);
    uint32_t ia = 0, ib = 0, iy = 0;
    int rc = omega_ad_tape_init(&t, g, nodes, 3, scratch, 12);
    if (!rc) rc = omega_ad_leaf(&t, ta, true, &ia);
    if (!rc) rc = omega_ad_leaf(&t, tb, true, &ib);
    if (!rc) rc = omega_ad_binary(&t, OMEGA_AD_MAX, ia, ib, &iy);
    if (!rc) rc = omega_ad_backward_seed(&t, iy, tr);
    if (!rc) rc = omega_ad_grad_read_f32(&t, ia, ga, 4);
    if (!rc) rc = omega_ad_grad_read_f32(&t, ib, gb, 4);
    CHECK(rc == 0, "max ties: rc %d", rc);
    bool ok = true;
    for (int i = 0; i < 4; i++) ok = ok && ga[i] == 0.0f && gb[i] == R[i];
    CHECK(ok, "max ties: gradient must go to b (ga %g %g %g %g, gb %g %g %g %g)",
          (double)ga[0], (double)ga[1], (double)ga[2], (double)ga[3],
          (double)gb[0], (double)gb[1], (double)gb[2], (double)gb[3]);
    omega_ad_tape_release(&t);
    omega_tensor_release(g, ta); omega_tensor_release(g, tb); omega_tensor_release(g, tr);
}

/* ---- composite: linear layer + squared error ------------------------------------------- */

#define CX_N 4
#define CX_IN 3
#define CX_OUT 2

typedef struct {
    float X[CX_N * CX_IN], W[CX_IN * CX_OUT], bias[CX_OUT], Y[CX_N * CX_OUT];
} LinData;

/* loss = mean_i( sum_j ( (X W + bias - Y)_ij ^2 ) ). d is used twice: fan-out. */
/* loss_d (optional): the same loss computed in double from the tape's float d
 * values, mean_i sum_j d_ij^2, for the FD layer. */
static int lin_run(OmegaTensorCtx *ctx, const LinData *D, float *loss, float *flat_grads,
                   uint8_t loss_id[32], double *loss_d) {
    uint64_t sx[2] = { CX_N, CX_IN }, sw[2] = { CX_IN, CX_OUT }, sb[1] = { CX_OUT }, sy[2] = { CX_N, CX_OUT };
    OmegaTensor tx, tw, tb, ty;
    int rc = omega_tensor_from_f32(ctx, 2, sx, D->X, &tx);
    if (!rc) rc = omega_tensor_from_f32(ctx, 2, sw, D->W, &tw);
    if (!rc) rc = omega_tensor_from_f32(ctx, 1, sb, D->bias, &tb);
    if (!rc) rc = omega_tensor_from_f32(ctx, 2, sy, D->Y, &ty);
    if (rc) return rc;
    OmegaAdNode nodes[16];
    OmegaAdTape t;
    uint32_t ix = 0, iw = 0, ib = 0, iy = 0, p = 0, q = 0, d = 0, s = 0, r = 0, l = 0;
    rc = omega_ad_tape_init(&t, ctx, nodes, 16, NULL, 0);
    if (!rc) rc = omega_ad_leaf(&t, tx, false, &ix);
    if (!rc) rc = omega_ad_leaf(&t, tw, true, &iw);
    if (!rc) rc = omega_ad_leaf(&t, tb, true, &ib);
    if (!rc) rc = omega_ad_leaf(&t, ty, false, &iy);
    if (!rc) rc = omega_ad_matmul(&t, ix, iw, &p);
    if (!rc) rc = omega_ad_binary(&t, OMEGA_AD_ADD, p, ib, &q);
    if (!rc) rc = omega_ad_binary(&t, OMEGA_AD_SUB, q, iy, &d);
    if (!rc) rc = omega_ad_binary(&t, OMEGA_AD_MUL, d, d, &s);
    if (!rc) rc = omega_ad_reduce(&t, OMEGA_AD_SUM, s, 1, false, &r);
    if (!rc) rc = omega_ad_reduce(&t, OMEGA_AD_MEAN, r, 0, false, &l);
    OmegaTensor tl = { 0, 0 };
    if (!rc) rc = omega_ad_value(&t, l, &tl);
    if (!rc) rc = omega_tensor_read_f32(ctx, tl, loss, 1);
    if (!rc && loss_id) rc = omega_tensor_value_id(ctx, tl, loss_id);
    if (!rc && loss_d) {
        OmegaTensor td = { 0, 0 };
        float dv[CX_N * CX_OUT];
        rc = omega_ad_value(&t, d, &td);
        if (!rc) rc = omega_tensor_read_f32(ctx, td, dv, CX_N * CX_OUT);
        if (!rc) {
            double acc = 0;
            for (int i = 0; i < CX_N * CX_OUT; i++) acc += (double)dv[i] * (double)dv[i];
            *loss_d = acc / CX_N;
        }
    }
    if (!rc && flat_grads) {
        rc = omega_ad_backward(&t, l);
        uint32_t ids[2] = { iw, ib };
        if (!rc) rc = omega_ad_grads_flatten(&t, ids, 2, flat_grads, CX_IN * CX_OUT + CX_OUT);
        if (!rc) {
            OmegaTensor gx;
            int e1 = omega_ad_grad(&t, ix, &gx), e2 = omega_ad_grad(&t, iy, &gx), e3 = omega_ad_grad(&t, iw, &gx);
            if (e1 != OMEGA_AD_ERR_NO_GRAD || e2 != OMEGA_AD_ERR_NO_GRAD || e3 != 0) rc = 1000;
        }
    }
    omega_ad_tape_release(&t);
    omega_tensor_release(ctx, tx); omega_tensor_release(ctx, tw);
    omega_tensor_release(ctx, tb); omega_tensor_release(ctx, ty);
    return rc;
}

static void lin_data(LinData *D) {
    for (int i = 0; i < CX_N * CX_IN; i++) D->X[i] = urand(-1, 1);
    for (int i = 0; i < CX_IN * CX_OUT; i++) D->W[i] = urand(-1, 1);
    for (int i = 0; i < CX_OUT; i++) D->bias[i] = urand(-1, 1);
    for (int i = 0; i < CX_N * CX_OUT; i++) D->Y[i] = urand(-1, 1);
}

/* FD data set: X, W, bias in [0.5, 1], Y in [-0.5, 0], so E = XW + b - Y lies
 * in [1.25, 4.5] and every gradient, (2/N) X^T E and (2/N) sum_i E, is >= 1.25:
 * no near-zero gradient element for the pure relative FD bound. */
static void lin_data_fd(LinData *D) {
    for (int i = 0; i < CX_N * CX_IN; i++) D->X[i] = urand(0.5f, 1.0f);
    for (int i = 0; i < CX_IN * CX_OUT; i++) D->W[i] = urand(0.5f, 1.0f);
    for (int i = 0; i < CX_OUT; i++) D->bias[i] = urand(0.5f, 1.0f);
    for (int i = 0; i < CX_N * CX_OUT; i++) D->Y[i] = urand(-0.5f, 0.0f);
}

static void test_composite(void) {
    LinData D;
    lin_data(&D);
    float loss, grads[CX_IN * CX_OUT + CX_OUT];
    int rc = lin_run(g, &D, &loss, grads, NULL, NULL);
    CHECK(rc == 0, "composite: rc %d (incl. no grad for X, Y; grad for W)", rc);
    if (rc) return;
    /* exact reference in double: dL/dW = (2/N) X^T E, dL/db = (2/N) sum_i E, E = XW + b - Y */
    double E[CX_N * CX_OUT];
    for (int i = 0; i < CX_N; i++)
        for (int j = 0; j < CX_OUT; j++) {
            double s = D.bias[j];
            for (int k = 0; k < CX_IN; k++) s += (double)D.X[i * CX_IN + k] * D.W[k * CX_OUT + j];
            E[i * CX_OUT + j] = s - D.Y[i * CX_OUT + j];
        }
    size_t bad_ref = 0;
    for (int k = 0; k < CX_IN; k++)
        for (int j = 0; j < CX_OUT; j++) {
            double s = 0;
            for (int i = 0; i < CX_N; i++) s += D.X[i * CX_IN + k] * E[i * CX_OUT + j];
            s *= 2.0 / CX_N;
            if (!(dabs(grads[k * CX_OUT + j] - s) <= 1e-5 + 1e-5 * dabs(s))) bad_ref++;
        }
    for (int j = 0; j < CX_OUT; j++) {
        double s = 0;
        for (int i = 0; i < CX_N; i++) s += E[i * CX_OUT + j];
        s *= 2.0 / CX_N;
        if (!(dabs(grads[CX_IN * CX_OUT + j] - s) <= 1e-5 + 1e-5 * dabs(s))) bad_ref++;
    }
    CHECK(bad_ref == 0, "composite: %zu gradients differ from the closed form (2/N) X^T E", bad_ref);
    /* central FD on every parameter (W then bias, the flatten order), pure
     * 1e-3 relative (approved M21 gate), on the FD data set lin_data_fd */
    LinData F;
    lin_data_fd(&F);
    float floss, fgrads[CX_IN * CX_OUT + CX_OUT];
    rc = lin_run(g, &F, &floss, fgrads, NULL, NULL);
    CHECK(rc == 0, "composite FD data set: rc %d", rc);
    if (rc) return;
    size_t fbad = 0;
    float *params[2] = { F.W, F.bias };
    int counts[2] = { CX_IN * CX_OUT, CX_OUT };
    for (int pnum = 0, off = 0; pnum < 2; off += counts[pnum], pnum++)
        for (int i = 0; i < counts[pnum]; i++) {
            float *x = &params[pnum][i], x0 = *x, xp = (float)(x0 + FD_H), xm = (float)(x0 - FD_H);
            float Lpf = 0, Lmf = 0;
            double Lp = 0, Lm = 0;
            *x = xp; int e1 = lin_run(g, &F, &Lpf, NULL, NULL, &Lp);
            *x = xm; int e2 = lin_run(g, &F, &Lmf, NULL, NULL, &Lm);
            *x = x0;
            double fd = (Lp - Lm) / ((double)xp - (double)xm);
            if (e1 || e2 || !fd_ok((double)fgrads[off + i], fd)) fbad++;
        }
    CHECK(fbad == 0, "composite: %zu parameter gradients disagree with central FD", fbad);
}

/* Two full runs in two contexts: bit-identical gradients and loss value id. */
static void test_determinism(void) {
    LinData D;
    lin_data(&D);
    OmegaTensorCtx *c2 = NULL;
    int rc = omega_tensor_ctx_create(1024, omega_tensor_cpu_realization(), &c2);
    CHECK(rc == 0, "determinism: second ctx rc %d", rc);
    if (rc) return;
    float l1, l2, g1[8], g2[8];
    uint8_t id1[32], id2[32];
    int r1 = lin_run(g, &D, &l1, g1, id1, NULL), r2 = lin_run(c2, &D, &l2, g2, id2, NULL);
    CHECK(r1 == 0 && r2 == 0, "determinism: runs rc %d %d", r1, r2);
    CHECK(memcmp(g1, g2, sizeof(g1)) == 0, "determinism: gradients differ between two runs");
    CHECK(memcmp(id1, id2, 32) == 0, "determinism: loss value id differs between two runs");
    /* counterexample: one flipped low bit must be seen */
    float g3[8];
    memcpy(g3, g1, sizeof(g1));
    uint32_t bits;
    memcpy(&bits, &g3[3], 4); bits ^= 1u; memcpy(&g3[3], &bits, 4);
    CHECK(memcmp(g1, g3, sizeof(g1)) != 0, "determinism comparator missed a one-bit difference");
    uint32_t lt, ls;
    omega_tensor_live_counts(c2, &lt, &ls);
    CHECK(lt == 0 && ls == 0, "determinism: second ctx leaked %u tensors %u storages", lt, ls);
    omega_tensor_ctx_destroy(c2);
}

/* ---- error paths ------------------------------------------------------------------------- */

static void live(uint32_t *lt, uint32_t *ls) { omega_tensor_live_counts(g, lt, ls); }

static void test_errors(void) {
    OmegaAdNode nodes[3];
    OmegaAdTape t;
    uint64_t s34[2] = { 3, 4 }, s5[1] = { 5 }, s4[1] = { 4 }, s35[2] = { 3, 5 }, s42[2] = { 4, 2 };
    float d[20];
    for (int i = 0; i < 20; i++) d[i] = urand(-1, 1);
    OmegaTensor a = mk(2, s34, d), b5 = mk(1, s5, d), b4 = mk(1, s4, d), m35 = mk(2, s35, d), m42 = mk(2, s42, d);

    /* init refusals, with the valid call as counterexample */
    CHECK(omega_ad_tape_init(&t, g, nodes, 0, NULL, 0) == OMEGA_AD_ERR_BAD_ARGS, "capacity 0 accepted");
    CHECK(omega_ad_tape_init(&t, g, NULL, 3, NULL, 0) == OMEGA_AD_ERR_BAD_ARGS, "NULL nodes accepted");
    CHECK(omega_ad_tape_init(&t, g, nodes, 3, NULL, 5) == OMEGA_AD_ERR_BAD_ARGS, "NULL scratch with count accepted");
    CHECK(omega_ad_tape_init(&t, g, nodes, 3, NULL, 0) == 0, "valid init refused");

    /* tape capacity: 3 nodes fit, the 4th is refused and nothing leaks */
    uint32_t ia = 0, ib = 0, iy = 0, iz = 77;
    int rc = omega_ad_leaf(&t, a, true, &ia);
    if (!rc) rc = omega_ad_leaf(&t, b4, true, &ib);
    if (!rc) rc = omega_ad_binary(&t, OMEGA_AD_ADD, ia, ib, &iy);
    CHECK(rc == 0 && t.count == 3, "3 nodes on a capacity-3 tape: rc %d count %u", rc, t.count);
    uint32_t lt0, ls0, lt1, ls1;
    live(&lt0, &ls0);
    rc = omega_ad_binary(&t, OMEGA_AD_ADD, iy, ib, &iz);
    live(&lt1, &ls1);
    CHECK(rc == OMEGA_AD_ERR_TAPE_FULL, "full tape: rc %d, want TAPE_FULL", rc);
    CHECK(t.count == 3 && iz == 77 && lt0 == lt1 && ls0 == ls1, "full tape changed state (count %u, live %u->%u)",
          t.count, lt0, lt1);
    CHECK(omega_ad_leaf(&t, a, true, &iz) == OMEGA_AD_ERR_TAPE_FULL, "leaf on full tape accepted");
    omega_ad_tape_release(&t);

    /* shape mismatches: tensor error passed through, tape and ctx unchanged */
    OmegaAdNode n2[8];
    float scratch[6];
    omega_ad_tape_init(&t, g, n2, 8, scratch, 6);
    uint32_t i35 = 0, i42 = 0, i5 = 0;
    omega_ad_leaf(&t, a, true, &ia);
    omega_ad_leaf(&t, b5, true, &i5);
    omega_ad_leaf(&t, m35, true, &i35);
    omega_ad_leaf(&t, m42, true, &i42);
    CHECK(omega_ad_sqrt(&t, 99, &iz) == OMEGA_AD_ERR_BAD_ARGS, "node id 99 accepted");
    live(&lt0, &ls0);
    uint32_t before = t.count;
    int e1 = omega_ad_binary(&t, OMEGA_AD_ADD, ia, i5, &iz);
    int e2 = omega_ad_matmul(&t, i35, i42, &iz);
    uint64_t s4b[1] = { 4 };
    int e3 = omega_ad_broadcast(&t, i5, 1, s4b, &iz);
    int e4 = omega_ad_reduce(&t, OMEGA_AD_SUM, ia, 2, false, &iz);
    int e5 = omega_ad_binary(&t, OMEGA_AD_MATMUL, ia, ia, &iz);
    live(&lt1, &ls1);
    CHECK(e1 == OMEGA_TENSOR_ERR_SHAPE, "[3,4]+[5]: rc %d, want TENSOR_ERR_SHAPE", e1);
    CHECK(e2 == OMEGA_TENSOR_ERR_SHAPE, "[3,5]x[4,2]: rc %d, want TENSOR_ERR_SHAPE", e2);
    CHECK(e3 == OMEGA_TENSOR_ERR_SHAPE, "broadcast [5]->[4]: rc %d, want TENSOR_ERR_SHAPE", e3);
    CHECK(e4 == OMEGA_TENSOR_ERR_AXIS, "reduce axis 2 of rank 2: rc %d, want TENSOR_ERR_AXIS", e4);
    CHECK(e5 == OMEGA_AD_ERR_BAD_ARGS, "binary with MATMUL op: rc %d, want BAD_ARGS", e5);
    CHECK(t.count == before && lt0 == lt1 && ls0 == ls1, "refused ops changed the tape or leaked");
    uint32_t imm = 0;
    CHECK(omega_ad_matmul(&t, i35, i35, &imm) == OMEGA_TENSOR_ERR_SHAPE, "[3,5]x[3,5] accepted");
    /* counterexample: compatible shapes record (MAX below) */
    uint32_t imax = 0;
    rc = omega_ad_binary(&t, OMEGA_AD_MAX, ia, ia, &imax); /* [3,4]: backward needs 36 scratch floats */
    CHECK(rc == 0, "max record rc %d", rc);
    /* seed / root shape (enough scratch, so only the shape can refuse) */
    float big[36];
    t.scratch = big; t.scratch_count = 36;
    OmegaTensor seed34 = mk(2, s34, d);
    CHECK(omega_ad_backward(&t, imax) == OMEGA_AD_ERR_SHAPE, "scalar seed accepted for a [3,4] root");
    CHECK(omega_ad_backward_seed(&t, imax, b4) == OMEGA_AD_ERR_SHAPE, "seed [4] accepted for root [3,4]");
    /* scratch: 6 floats for MAX [3,4] (needs 36) is refused */
    t.scratch = scratch; t.scratch_count = 6;
    CHECK(omega_ad_backward_seed(&t, imax, seed34) == OMEGA_AD_ERR_SCRATCH, "6 floats of scratch accepted for MAX [3,4]");
    size_t need = 0;
    CHECK(omega_ad_scratch_need(&t, &need) == 0 && need == 36, "scratch need %zu, want 36", need);
    t.scratch = big; t.scratch_count = 36;
    CHECK(!t.backward_done, "refused backward marked the tape done");
    rc = omega_ad_backward_seed(&t, imax, seed34);
    CHECK(rc == 0, "valid seed refused: rc %d", rc);
    CHECK(omega_ad_backward_seed(&t, imax, seed34) == OMEGA_AD_ERR_STATE, "second backward accepted");
    CHECK(omega_ad_sqrt(&t, ia, &iz) == OMEGA_AD_ERR_STATE, "recording after backward accepted");
    OmegaTensor gg;
    CHECK(omega_ad_grad(&t, ia, &gg) == 0, "max input has no grad");
    CHECK(omega_ad_grad(&t, i35, &gg) == OMEGA_AD_ERR_NO_GRAD, "unreached leaf has a grad");
    float tiny[11];
    CHECK(omega_ad_grad_read_f32(&t, ia, tiny, 11) == OMEGA_AD_ERR_COUNT, "short grad read accepted");
    /* flatten: exact count only, nothing written otherwise */
    float flat[13];
    for (int i = 0; i < 13; i++) flat[i] = 7.0f;
    uint32_t one_id[1] = { ia };
    CHECK(omega_ad_grads_flatten(&t, one_id, 1, flat, 13) == OMEGA_AD_ERR_COUNT, "flatten count 13 for 12 accepted");
    bool untouched = true;
    for (int i = 0; i < 13; i++) untouched = untouched && flat[i] == 7.0f;
    CHECK(untouched, "refused flatten wrote the buffer");
    CHECK(omega_ad_grads_flatten(&t, one_id, 1, flat, 12) == 0, "flatten count 12 refused");
    float direct[12];
    omega_ad_grad_read_f32(&t, ia, direct, 12);
    CHECK(memcmp(direct, flat, sizeof(direct)) == 0, "flatten differs from grad read");
    omega_ad_tape_release(&t);
    omega_tensor_release(g, seed34);

    /* root without grad; F16 leaf refused */
    omega_ad_tape_init(&t, g, nodes, 3, NULL, 0);
    omega_ad_leaf(&t, a, false, &ia);
    rc = omega_ad_binary(&t, OMEGA_AD_ADD, ia, ia, &iy);
    CHECK(rc == 0, "no-grad add rc %d", rc);
    CHECK(omega_ad_backward_seed(&t, iy, a) == OMEGA_AD_ERR_NO_GRAD, "backward from a no-grad root accepted");
    uint16_t h[4] = { 0x3c00, 0x3c00, 0x3c00, 0x3c00 };
    OmegaTensor th;
    rc = omega_tensor_from_data(g, OMEGA_DT_F16, 1, s4, h, &th);
    CHECK(rc == 0, "f16 tensor rc %d", rc);
    CHECK(omega_ad_leaf(&t, th, true, &iz) == OMEGA_TENSOR_ERR_DTYPE, "F16 leaf accepted");
    omega_ad_tape_release(&t);
    omega_tensor_release(g, th);

    omega_tensor_release(g, a); omega_tensor_release(g, b5); omega_tensor_release(g, b4);
    omega_tensor_release(g, m35); omega_tensor_release(g, m42);
}

int main(void) {
    if (!omega_numeric_fpenv_ok()) { printf("FPCR not RNE/no-FTZ: refusing to run\n"); return 2; }
    if (omega_tensor_ctx_create(4096, omega_tensor_cpu_realization(), &g)) { printf("ctx\n"); return 2; }
    for (size_t i = 0; i < NCASES; i++) test_op_case(&CASES[i]);
    test_comparators_reject();
    test_max_ties();
    test_composite();
    test_determinism();
    test_errors();
    uint32_t lt, ls;
    omega_tensor_live_counts(g, &lt, &ls);
    CHECK(lt == 0 && ls == 0, "no leaked tensors (%u) or storage (%u)", lt, ls);
    omega_tensor_ctx_destroy(g);
    printf("M21 OMEGA_AUTODIFF CPU tests: %d pass, %d fail\n", g_pass, g_fail);
    printf("M21 verdict: NOT QUALIFIED (CPU tier only; GB10 NOT_RUN)\n");
    return g_fail ? 1 : 0;
}
