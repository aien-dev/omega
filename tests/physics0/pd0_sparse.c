#include "pd0_sparse.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LIB_MAX 64

typedef struct { pd0_term lib[LIB_MAX]; int n; } library;

static void build_library(library *L, int n_obs, int n_ch) {
    L->n = 0;
    /* all exponent vectors over n_obs vars with total degree 1..3 (constant excluded) */
    int e[PD0_MAX_OBS] = {0};
    for (;;) {
        int deg = 0;
        for (int v = 0; v < n_obs; v++) deg += e[v];
        if (deg >= 1 && deg <= PD0_MAX_DEG && L->n < LIB_MAX) {
            pd0_term t; memset(&t, 0, sizeof t); t.coef = 1;
            for (int v = 0; v < n_obs; v++) t.ex[v] = (uint8_t)e[v];
            L->lib[L->n++] = t;
        }
        int v = 0;
        while (v < n_obs) { if (++e[v] <= PD0_MAX_DEG) break; e[v] = 0; v++; }
        if (v == n_obs) break;
    }
    for (int c = 0; c < n_ch && L->n < LIB_MAX; c++) {
        pd0_term t; memset(&t, 0, sizeof t); t.coef = 1; t.ex[PD0_MAX_VARS + c] = 1;
        L->lib[L->n++] = t;
    }
}

static int is_fit(uint32_t ep) { return ep % 5 <= 2; }
static int is_select(uint32_t ep) { return ep % 5 == 3; }

/* residual sum of squares of y - X[:,sel] b over rows where mask(ep) */
static double rss_on(const double *F, const double *y, const pd0_gather *g, int nlib, const int *sel, int k,
                     const double *b, int (*mask)(uint32_t), int *rows) {
    double s = 0; int n = 0;
    for (int r = 0; r < g->n; r++) {
        if (!mask(g->t[r].episode)) continue;
        double p = 0;
        for (int j = 0; j < k; j++) p += F[r * nlib + sel[j]] * b[j];
        s += (y[r] - p) * (y[r] - p); n++;
    }
    *rows = n;
    return s;
}

int pd0_sparse_fit(const pd0_gather *g, int n_obs, int n_ch, pd0_relation *out) {
    library L;
    build_library(&L, n_obs, n_ch);
    memset(out, 0, sizeof *out);
    out->n_vars = (uint8_t)n_obs; out->n_channels = (uint8_t)n_ch; out->n_eq = 0;
    int nlib = L.n, n = g->n;
    if (n < 8) return -1;
    double *F = malloc(sizeof(double) * (size_t)n * (size_t)nlib);
    double *y = malloc(sizeof(double) * (size_t)n);
    double *Xs = malloc(sizeof(double) * (size_t)n * PD0_SPARSE_MAX_TERMS);
    double *ys = malloc(sizeof(double) * (size_t)n);
    if (!F || !y || !Xs || !ys) { free(F); free(y); free(Xs); free(ys); return -1; }
    for (int r = 0; r < n; r++) {
        int64_t vars[PD0_MAX_VARS] = {0};
        for (int v = 0; v < n_obs; v++) vars[v] = g->t[r].before[v];
        for (int j = 0; j < nlib; j++) F[r * nlib + j] = (double)pd0_monomial(&L.lib[j], vars, g->t[r].u, n_obs, n_ch) / 1e6;
    }
    for (int target = 0; target < n_obs; target++) {
        for (int r = 0; r < n; r++) y[r] = (double)(g->t[r].after[target] - g->t[r].before[target]) / 1e6;
        int sel[PD0_SPARSE_MAX_TERMS], k = 0, best_k = 0;
        double best_bits = INFINITY, best_b[PD0_SPARSE_MAX_TERMS] = {0}, b[PD0_SPARSE_MAX_TERMS];
        int best_sel[PD0_SPARSE_MAX_TERMS] = {0};
        /* k = 0: the null model */
        {
            int rows; double rss = rss_on(F, y, g, nlib, sel, 0, b, is_select, &rows);
            if (rows > 0) best_bits = rows * log2(rss / rows + 1e-18);
        }
        while (k < PD0_SPARSE_MAX_TERMS) {
            /* greedy: add the library term that most reduces FIT RSS */
            int pick = -1; double pick_rss = INFINITY;
            for (int j = 0; j < nlib; j++) {
                int used = 0;
                for (int i = 0; i < k; i++) used |= sel[i] == j;
                if (used) continue;
                sel[k] = j;
                int nf = 0;
                for (int r = 0; r < n; r++) {
                    if (!is_fit(g->t[r].episode)) continue;
                    for (int i = 0; i <= k; i++) Xs[nf * (k + 1) + i] = F[r * nlib + sel[i]];
                    ys[nf++] = y[r];
                }
                if (nf <= k + 1 || pd0_lstsq(Xs, ys, nf, k + 1, b) != 0) continue;
                int rows; double rss = rss_on(F, y, g, nlib, sel, k + 1, b, is_fit, &rows);
                if (rss < pick_rss) { pick_rss = rss; pick = j; }
            }
            if (pick < 0) break;
            sel[k++] = pick;
            int nf = 0;
            for (int r = 0; r < n; r++) {
                if (!is_fit(g->t[r].episode)) continue;
                for (int i = 0; i < k; i++) Xs[nf * k + i] = F[r * nlib + sel[i]];
                ys[nf++] = y[r];
            }
            if (pd0_lstsq(Xs, ys, nf, k, b) != 0) break;
            int rows; double rss = rss_on(F, y, g, nlib, sel, k, b, is_select, &rows);
            if (rows == 0) break;
            double bits = (double)k * (8.0 * (n_obs + n_ch) + 24.0) + rows * log2(rss / rows + 1e-18);
            if (bits < best_bits) { best_bits = bits; best_k = k; memcpy(best_sel, sel, sizeof best_sel); memcpy(best_b, b, sizeof best_b); }
        }
        if (best_k == 0) continue;
        pd0_eq *e = &out->eq[out->n_eq++];
        e->target = (uint8_t)target; e->n_terms = 0;
        for (int i = 0; i < best_k; i++) {
            int64_t c = (int64_t)llround(best_b[i] * 1e6);
            if (c == 0) continue;
            pd0_term t = L.lib[best_sel[i]]; t.coef = c;
            e->t[e->n_terms++] = t;
        }
        if (e->n_terms == 0) out->n_eq--;
    }
    free(F); free(y); free(Xs); free(ys);
    return 0;
}
