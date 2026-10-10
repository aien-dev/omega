/* Exact kernel of the literal constraint matrix.
 *
 * H_total = H_C (x) I_S + I_C (x) H_S + sum_j |E_j><E_j| (x) (v_j . sigma)   (AT1_CASE_V1 s3)
 * is built term by term with an explicit Kronecker product, as a 2N x 2N matrix of exact
 * Gaussian rationals. Nothing about its block structure or its level eigenvectors is assumed:
 * the kernel is found by exact Gauss-Jordan elimination of that matrix (rank, free columns,
 * one basis vector per free column), the orthogonal projector is formed exactly as
 * P_0 = B (B^dagger B)^(-1) B^dagger, and both H_total B = 0 and the kernel dimension are
 * re-checked exactly. The closed forms of AT1_SPEC.md sections 2 to 4 are not used here; they
 * are what the tests compare the engine with.
 *
 * Zero test (AT1_CASE_V1 s3): Psi = P_0 (|t_r> (x) psi_0) = N^(-1/2) sum_j w_j y_j with
 * |w_j| = 1 and y_j = P_0 (|E_j> (x) psi_0) exact. When the nonzero y_j have pairwise disjoint
 * supports, Psi = 0 exactly iff every y_j = 0. Disjointness is checked, not assumed; it always
 * holds for a clock-diagonal V, and its failure is reported as an internal error. */
#include "at1_model.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int r, c; gq **e; } mat;   /* row major; NULL entry = exact zero */

static mat mat_new(int r, int c)
{
    mat m = { r, c, at1_xcalloc((size_t)r * (size_t)c, sizeof(gq *)) };
    return m;
}
static void ent_free(gq *x) { if (x) { gq_free(x); free(x); } }
static void mat_free(mat *m)
{
    if (!m->e) return;
    for (int i = 0; i < m->r * m->c; i++) ent_free(m->e[i]);
    free(m->e); m->e = NULL;
}
static gq **at(mat *m, int i, int j) { return &m->e[i * m->c + j]; }
static const gq *get(const mat *m, int i, int j) { return m->e[i * m->c + j]; }

/* *slot += x (x exact); keeps exact zeros as NULL */
static void ent_add(gq **slot, const gq *x)
{
    if (!x || gq_is_zero(x)) return;
    if (!*slot) { *slot = at1_xmalloc(sizeof(gq)); gq_init(*slot); gq_copy(*slot, x); return; }
    gq_add(*slot, *slot, x);
    if (gq_is_zero(*slot)) { ent_free(*slot); *slot = NULL; }
}
static void ent_set(gq **slot, const gq *x) { ent_free(*slot); *slot = NULL; ent_add(slot, x); }

static void gq_set_rat(gq *g, const bq *re, const bq *im)
{
    if (re) bq_copy(&g->re, re); else bq_set_i64(&g->re, 0, 1);
    if (im) bq_copy(&g->im, im); else bq_set_i64(&g->im, 0, 1);
}
static void gq_set_int(gq *g, int64_t re, int64_t im) { bq_set_i64(&g->re, re, 1); bq_set_i64(&g->im, im, 1); }

/* C = A (x) B */
static mat kron(const mat *A, const mat *B)
{
    mat C = mat_new(A->r * B->r, A->c * B->c);
    gq t; gq_init(&t);
    for (int i = 0; i < A->r; i++) for (int j = 0; j < A->c; j++) {
        const gq *a = get(A, i, j);
        if (!a) continue;
        for (int k = 0; k < B->r; k++) for (int l = 0; l < B->c; l++) {
            const gq *b = get(B, k, l);
            if (!b) continue;
            gq_mul(&t, a, b);
            ent_add(at(&C, i * B->r + k, j * B->c + l), &t);
        }
    }
    gq_free(&t);
    return C;
}
/* A += B */
static void mat_addto(mat *A, const mat *B)
{
    for (int i = 0; i < A->r * A->c; i++) if (B->e[i]) ent_add(&A->e[i], B->e[i]);
}
/* A * s (exact scalar) into a new matrix */
static mat mat_scale(const mat *A, const gq *s)
{
    mat C = mat_new(A->r, A->c);
    gq t; gq_init(&t);
    for (int i = 0; i < A->r * A->c; i++) if (A->e[i]) { gq_mul(&t, A->e[i], s); ent_set(&C.e[i], &t); }
    gq_free(&t);
    return C;
}
static mat mat_mul(const mat *A, const mat *B)
{
    mat C = mat_new(A->r, B->c);
    gq t; gq_init(&t);
    for (int i = 0; i < A->r; i++) for (int k = 0; k < A->c; k++) {
        const gq *a = get(A, i, k);
        if (!a) continue;
        for (int j = 0; j < B->c; j++) {
            const gq *b = get(B, k, j);
            if (!b) continue;
            gq_mul(&t, a, b);
            ent_add(at(&C, i, j), &t);
        }
    }
    gq_free(&t);
    return C;
}
static mat mat_adjoint(const mat *A)
{
    mat C = mat_new(A->c, A->r);
    gq t; gq_init(&t);
    for (int i = 0; i < A->r; i++) for (int j = 0; j < A->c; j++) {
        const gq *a = get(A, i, j);
        if (a) { gq_conj(&t, a); ent_set(at(&C, j, i), &t); }
    }
    gq_free(&t);
    return C;
}
static void mat_set_int(mat *m, int i, int j, int64_t re, int64_t im)
{
    gq t; gq_init(&t); gq_set_int(&t, re, im); ent_set(at(m, i, j), &t); gq_free(&t);
}

/* Gauss-Jordan to reduced row echelon form, in place. pivcol[r] = pivot column of row r.
 * Returns the rank. Exact: a pivot is any exactly nonzero entry. */
static int rref(mat *A, int *pivcol)
{
    int rank = 0;
    gq inv, t, one; gq_init(&inv); gq_init(&t); gq_init(&one); gq_set_int(&one, 1, 0);
    for (int c = 0; c < A->c && rank < A->r; c++) {
        int p = -1;
        for (int i = rank; i < A->r; i++) if (get(A, i, c)) { p = i; break; }
        if (p < 0) continue;
        if (p != rank) for (int j = 0; j < A->c; j++) { gq *s = *at(A, p, j); *at(A, p, j) = *at(A, rank, j); *at(A, rank, j) = s; }
        gq_div(&inv, &one, get(A, rank, c));
        for (int j = 0; j < A->c; j++) if (get(A, rank, j)) gq_mul(*at(A, rank, j), get(A, rank, j), &inv);
        for (int i = 0; i < A->r; i++) {
            if (i == rank || !get(A, i, c)) continue;
            gq f; gq_init(&f); gq_copy(&f, get(A, i, c));
            for (int j = 0; j < A->c; j++) {
                const gq *rj = get(A, rank, j);
                if (!rj) continue;
                gq_mul(&t, &f, rj); gq_neg(&t, &t);
                ent_add(at(A, i, j), &t);
            }
            gq_free(&f);
        }
        pivcol[rank++] = c;
    }
    gq_free(&inv); gq_free(&t); gq_free(&one);
    return rank;
}


void at1_kernel_free(at1_kernel *k)
{
    free(k->h_re); free(k->h_im); free(k->y_re); free(k->y_im); free(k->owner);
    memset(k, 0, sizeof *k);
}

at1_status at1_kernel_build(const at1_case *c, at1_kernel *k)
{
    memset(k, 0, sizeof *k);
    const int N = c->clock_dim, D = 2 * N;
    at1_status st = AT1_OK;
    gq t; gq_init(&t);

    /* single-factor matrices */
    mat HC = mat_new(N, N), IN = mat_new(N, N), I2 = mat_new(2, 2);
    mat X = mat_new(2, 2), Y = mat_new(2, 2), Z = mat_new(2, 2);
    for (int j = 0; j < N; j++) { gq_set_rat(&t, &c->energies[j], NULL); ent_set(at(&HC, j, j), &t); mat_set_int(&IN, j, j, 1, 0); }
    mat_set_int(&I2, 0, 0, 1, 0); mat_set_int(&I2, 1, 1, 1, 0);
    mat_set_int(&X, 0, 1, 1, 0); mat_set_int(&X, 1, 0, 1, 0);
    mat_set_int(&Y, 0, 1, 0, -1); mat_set_int(&Y, 1, 0, 0, 1);
    mat_set_int(&Z, 0, 0, 1, 0); mat_set_int(&Z, 1, 1, -1, 0);
    const mat *pauli[3] = { &X, &Y, &Z };

    /* H_S = h0 I + hx X + hy Y + hz Z */
    mat HS = mat_new(2, 2);
    { gq_set_rat(&t, &c->h0, NULL); mat s = mat_scale(&I2, &t); mat_addto(&HS, &s); mat_free(&s); }
    for (int a = 0; a < 3; a++) { gq_set_rat(&t, &c->h[a], NULL); mat s = mat_scale(pauli[a], &t); mat_addto(&HS, &s); mat_free(&s); }

    /* H_total = H_C (x) I + I (x) H_S + sum_j |E_j><E_j| (x) v_j.sigma */
    mat H = kron(&HC, &I2);
    { mat s = kron(&IN, &HS); mat_addto(&H, &s); mat_free(&s); }
    for (int j = 0; j < N; j++) {
        mat Pj = mat_new(N, N), Vj = mat_new(2, 2);
        mat_set_int(&Pj, j, j, 1, 0);
        for (int a = 0; a < 3; a++) { gq_set_rat(&t, &c->v[j][a], NULL); mat s = mat_scale(pauli[a], &t); mat_addto(&Vj, &s); mat_free(&s); }
        mat s = kron(&Pj, &Vj); mat_addto(&H, &s);
        mat_free(&s); mat_free(&Pj); mat_free(&Vj);
    }

    /* exact nullspace */
    mat R = mat_new(D, D);
    for (int i = 0; i < D * D; i++) if (H.e[i]) ent_set(&R.e[i], H.e[i]);
    int *pivcol = at1_xmalloc((size_t)D * sizeof(int));
    int rank = rref(&R, pivcol);
    int dk = D - rank;
    k->dim = D; k->kernel_dim = dk;
    int *is_piv = at1_xcalloc((size_t)D, sizeof(int));
    for (int r = 0; r < rank; r++) is_piv[pivcol[r]] = 1;
    mat B = mat_new(D, dk > 0 ? dk : 1);
    {
        int col = 0;
        for (int f = 0; f < D; f++) {
            if (is_piv[f]) continue;
            mat_set_int(&B, f, col, 1, 0);
            for (int r = 0; r < rank; r++) {
                const gq *x = get(&R, r, f);
                if (x) { gq_neg(&t, x); ent_set(at(&B, pivcol[r], col), &t); }
            }
            col++;
        }
    }
    mat P0 = mat_new(D, D);
    if (dk > 0) {
        /* re-check H_total B = 0 exactly */
        mat HB = mat_mul(&H, &B);
        for (int i = 0; i < HB.r * HB.c; i++) if (HB.e[i]) st = AT1_ERR_INTERNAL;
        mat_free(&HB);
        /* P_0 = B (B^dagger B)^(-1) B^dagger */
        mat Bd = mat_adjoint(&B);
        mat G = mat_mul(&Bd, &B);
        mat Aug = mat_new(dk, 2 * dk);
        for (int i = 0; i < dk; i++) {
            for (int j = 0; j < dk; j++) if (get(&G, i, j)) ent_set(at(&Aug, i, j), get(&G, i, j));
            mat_set_int(&Aug, i, dk + i, 1, 0);
        }
        int *pc = at1_xmalloc((size_t)dk * sizeof(int));
        int gr = rref(&Aug, pc);
        if (gr != dk) st = AT1_ERR_INTERNAL;          /* Gram matrix of a basis is invertible */
        for (int r = 0; r < gr && st == AT1_OK; r++) if (pc[r] != r) st = AT1_ERR_INTERNAL;
        free(pc);
        mat Gi = mat_new(dk, dk);
        for (int i = 0; i < dk; i++) for (int j = 0; j < dk; j++) if (get(&Aug, i, dk + j)) ent_set(at(&Gi, i, j), get(&Aug, i, dk + j));
        mat W = mat_mul(&B, &Gi);
        mat_free(&P0);
        P0 = mat_mul(&W, &Bd);
        mat_free(&Bd); mat_free(&G); mat_free(&Aug); mat_free(&Gi); mat_free(&W);
    }

    /* exact zero test and the number of clock levels carrying Psi */
    {
        int *owner = at1_xmalloc((size_t)D * sizeof(int));
        for (int i = 0; i < D; i++) owner[i] = -1;
        k->y_re = at1_xcalloc((size_t)D, sizeof(double)); k->y_im = at1_xcalloc((size_t)D, sizeof(double));
        int L = 0;
        gq y, prod; gq_init(&y); gq_init(&prod);
        for (int j = 0; j < N && st == AT1_OK; j++) {
            int nonzero = 0;
            for (int i = 0; i < D; i++) {
                bq_set_i64(&y.re, 0, 1); bq_set_i64(&y.im, 0, 1);
                for (int s = 0; s < 2; s++) {
                    const gq *p = get(&P0, i, 2 * j + s);
                    if (!p) continue;
                    gq_mul(&prod, p, &c->psi0[s]);
                    gq_add(&y, &y, &prod);
                }
                if (gq_is_zero(&y)) continue;
                nonzero = 1;
                if (owner[i] >= 0) st = AT1_ERR_INTERNAL;  /* supports overlap: not clock-diagonal */
                owner[i] = j;
                k->y_re[i] = bq_to_double(&y.re); k->y_im[i] = bq_to_double(&y.im);   /* rounded once */
            }
            L += nonzero;
        }
        gq_free(&y); gq_free(&prod);
        k->owner = owner;
        k->levels_in_psi = L;
        k->psi_zero = (L == 0);
    }

    /* binary64 copies, each entry correctly rounded from its exact value */
    k->h_re = at1_xcalloc((size_t)D * D, sizeof(double)); k->h_im = at1_xcalloc((size_t)D * D, sizeof(double));
    for (int i = 0; i < D * D; i++) {
        if (H.e[i]) { k->h_re[i] = bq_to_double(&H.e[i]->re); k->h_im[i] = bq_to_double(&H.e[i]->im); }
    }
    /* hmax = max_i sum over columns l carrying Psi of |H_total[i,l]|: the row sums of |H| that act on
     * Psi, which is what the rounding of H_total Psi_hat and the error of Psi are multiplied by */
    k->hmax = 0.0;
    for (int i = 0; i < D; i++) {
        double s = 0.0;
        for (int l = 0; l < D; l++) if (k->owner[l] >= 0) s += hypot(k->h_re[i * D + l], k->h_im[i * D + l]);
        if (s > k->hmax) k->hmax = s;
    }

    free(pivcol); free(is_piv);
    mat_free(&HC); mat_free(&IN); mat_free(&I2); mat_free(&X); mat_free(&Y); mat_free(&Z);
    mat_free(&HS); mat_free(&H); mat_free(&R); mat_free(&B); mat_free(&P0);
    gq_free(&t);
    if (st != AT1_OK) at1_kernel_free(k);
    return st;
}
